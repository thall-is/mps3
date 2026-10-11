#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_ota_mac_discovery.py — Testes unitarios e de integracao para descoberta inteligente por MAC no OTA do MPS3
"""

import unittest
import sys
import os
import json
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

# Adiciona versionamento ao path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "versionamento"))
import ota_upload


class MockOtaHandler(BaseHTTPRequestHandler):
    mac_to_return = "28:84:85:52:35:84"

    def do_GET(self):
        if self.path == "/api/ota":
            resp = {
                "running_version": "mps3_test",
                "running_partition": "ota_0",
                "next_partition": "ota_1",
                "battery_percent": 85,
                "battery_charging": False,
                "ota_in_progress": False,
                "mac": self.mac_to_return
            }
            data = json.dumps(resp).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        # Silencia logs do servidor mock durante os testes
        pass


class TestOtaMacDiscovery(unittest.TestCase):

    def test_normalize_mac(self):
        expected = "28:84:85:52:35:84"
        self.assertEqual(ota_upload.normalize_mac("28:84:85:52:35:84"), expected)
        self.assertEqual(ota_upload.normalize_mac("28-84-85-52-35-84"), expected)
        self.assertEqual(ota_upload.normalize_mac("288485523584"), expected)
        self.assertEqual(ota_upload.normalize_mac(" 28:84:85:52:35:84 \n"), expected)
        self.assertEqual(ota_upload.normalize_mac("28-84-85-52-35-84"), expected)
        self.assertEqual(ota_upload.normalize_mac("28:84:85:52:35:85"), "28:84:85:52:35:85")

    def test_matches_target_mac(self):
        sta = "28:84:85:52:35:84"
        ap = "28:84:85:52:35:85"
        diff = "aa:bb:cc:dd:ee:ff"

        # Mesmos MACs com formatos variados
        self.assertTrue(ota_upload.matches_target_mac("28-84-85-52-35-84", sta))
        self.assertTrue(ota_upload.matches_target_mac("28:84:85:52:35:84", "28-84-85-52-35-84"))

        # Target padrao aceita tanto STA quanto AP do ESP32-S3 (ambas as direcoes)
        self.assertTrue(ota_upload.matches_target_mac(ap, sta))
        self.assertTrue(ota_upload.matches_target_mac(sta, ap))
        self.assertTrue(ota_upload.matches_target_mac("28-84-85-52-35-85", sta))
        self.assertTrue(ota_upload.matches_target_mac(sta, "28-84-85-52-35-85"))

        # MACs customizados de ESP32 (STA e AP consecutivos)
        c_sta = "30:ae:a4:01:02:0a"
        c_ap = "30:ae:a4:01:02:0b"
        self.assertTrue(ota_upload.matches_target_mac(c_sta, c_ap))
        self.assertTrue(ota_upload.matches_target_mac(c_ap, c_sta))

        # MAC diferente deve falhar
        self.assertFalse(ota_upload.matches_target_mac(diff, sta))
        self.assertFalse(ota_upload.matches_target_mac("", sta))
        self.assertFalse(ota_upload.matches_target_mac(None, sta))

    def test_get_arp_table(self):
        entries = ota_upload.get_arp_table()
        self.assertIsInstance(entries, list)
        for ip, mac in entries:
            self.assertRegex(ip, r"^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$")
            self.assertRegex(mac, r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$")

    def test_get_local_subnets(self):
        subnets = ota_upload.get_local_subnets()
        self.assertIsInstance(subnets, list)
        self.assertGreater(len(subnets), 0)
        for sub in subnets:
            self.assertRegex(sub, r"^\d{1,3}\.\d{1,3}\.\d{1,3}\.$")

    def test_cache_ip(self):
        test_ip = "192.168.15.99"
        old_ip = ota_upload.get_cached_ip()
        try:
            ota_upload.save_cached_ip(test_ip)
            self.assertEqual(ota_upload.get_cached_ip(), test_ip)
        finally:
            if old_ip:
                ota_upload.save_cached_ip(old_ip)
            elif os.path.exists(ota_upload.CACHE_FILE):
                os.remove(ota_upload.CACHE_FILE)

    def test_mock_device_explicit_target_matching_mac(self):
        server = HTTPServer(("127.0.0.1", 0), MockOtaHandler)
        port = server.server_port
        t = threading.Thread(target=server.serve_forever)
        t.daemon = True
        t.start()

        try:
            h, p, info = ota_upload.discover_device(
                target_arg=f"127.0.0.1:{port}",
                target_mac="28:84:85:52:35:84"
            )
            self.assertEqual(h, "127.0.0.1")
            self.assertEqual(p, port)
            self.assertIsNotNone(info)
            self.assertEqual(info.get("mac"), "28:84:85:52:35:84")
            self.assertEqual(info.get("running_version"), "mps3_test")
        finally:
            server.shutdown()
            server.server_close()

    def test_mock_device_explicit_target_ap_mac_matches_sta_response(self):
        server = HTTPServer(("127.0.0.1", 0), MockOtaHandler)
        port = server.server_port
        t = threading.Thread(target=server.serve_forever)
        t.daemon = True
        t.start()

        try:
            # Passando MAC de AP, deve aceitar a resposta do dispositivo (STA)
            h, p, info = ota_upload.discover_device(
                target_arg=f"127.0.0.1:{port}",
                target_mac="28:84:85:52:35:85"
            )
            self.assertEqual(h, "127.0.0.1")
            self.assertEqual(p, port)
            self.assertIsNotNone(info)
        finally:
            server.shutdown()
            server.server_close()

    def test_mock_device_arp_discovery_multiple_entries(self):
        server = HTTPServer(("127.0.0.1", 0), MockOtaHandler)
        port = server.server_port
        t = threading.Thread(target=server.serve_forever)
        t.daemon = True
        t.start()

        orig_arp = ota_upload.get_arp_table
        try:
            # Simula tabela ARP com primeiro IP inativo e segundo IP ativo (o server mock)
            ota_upload.get_arp_table = lambda: [
                ("192.0.2.1", "28:84:85:52:35:84"),
                ("127.0.0.1", "28:84:85:52:35:84")
            ]
            h, p, info = ota_upload.discover_device(
                target_arg=None,
                target_mac="28:84:85:52:35:84",
                port=port,
                probe_timeout=0.2
            )
            self.assertEqual(h, "127.0.0.1")
            self.assertEqual(p, port)
            self.assertIsNotNone(info)
        finally:
            ota_upload.get_arp_table = orig_arp
            server.shutdown()
            server.server_close()

    def test_mock_device_fallback_cache_discovery(self):
        server = HTTPServer(("127.0.0.1", 0), MockOtaHandler)
        port = server.server_port
        t = threading.Thread(target=server.serve_forever)
        t.daemon = True
        t.start()

        old_ip = ota_upload.get_cached_ip()
        orig_arp = ota_upload.get_arp_table
        orig_subnets = ota_upload.get_local_subnets
        try:
            # Esvazia ARP e subnets para forcar ir direto ao Passo 4
            ota_upload.get_arp_table = lambda: []
            ota_upload.get_local_subnets = lambda: []
            ota_upload.save_cached_ip(f"127.0.0.1:{port}")
            h, p, info = ota_upload.discover_device(
                target_arg=None,
                target_mac="28:84:85:52:35:84",
                port=port,
                probe_timeout=0.2
            )
            self.assertEqual(h, "127.0.0.1")
            self.assertEqual(p, port)
            self.assertIsNotNone(info)
            self.assertEqual(info.get("mac"), "28:84:85:52:35:84")
        finally:
            ota_upload.get_arp_table = orig_arp
            ota_upload.get_local_subnets = orig_subnets
            server.shutdown()
            server.server_close()
            if old_ip:
                ota_upload.save_cached_ip(old_ip)
            elif os.path.exists(ota_upload.CACHE_FILE):
                os.remove(ota_upload.CACHE_FILE)


if __name__ == "__main__":
    unittest.main()
