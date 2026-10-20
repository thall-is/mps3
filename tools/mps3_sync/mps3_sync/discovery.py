"""
Cross-platform MPS3 Discovery Engine (`discovery.py`).

Discovers the MPS3 on the local network using:
1. Explicit host/IP verification
2. Cached last-known IP verification
3. mDNS (`_mps3._tcp.local.` via `zeroconf` + `mps3.local` hostname resolution)
4. OS neighbor/ARP table inspection (`ip neigh` on Linux, `arp -a` on Windows/macOS/Linux)
5. Fast concurrent TCP/HTTP subnet probe verifying hardware MAC via `GET /api/ota`
"""

from __future__ import annotations

import json
import os
import re
import socket
import subprocess
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

from .config import DEFAULT_AP_MAC, DEFAULT_TARGET_MAC


def normalize_mac(mac: str) -> str:
    """Normalizes a MAC address string to lowercase `xx:xx:xx:xx:xx:xx`."""
    if not mac:
        return ""
    s = str(mac).strip().lower()
    parts = re.split(r"[:-]", s)
    if len(parts) == 6 and all(0 < len(p) <= 2 for p in parts):
        return ":".join(p.zfill(2) for p in parts)
    clean = re.sub(r"[^0-9a-f]", "", s)
    if len(clean) == 12:
        return ":".join(clean[i : i + 2] for i in range(0, 12, 2))
    return s


def is_esp32_mac_pair(mac1: str, mac2: str) -> bool:
    """Checks if two MACs are the STA/AP pair of the same ESP32 (differ by +/-1 in last byte)."""
    n1 = normalize_mac(mac1)
    n2 = normalize_mac(mac2)
    if not n1 or not n2:
        return False
    p1 = n1.split(":")
    p2 = n2.split(":")
    if len(p1) != 6 or len(p2) != 6 or p1[:5] != p2[:5]:
        return False
    try:
        return abs(int(p1[5], 16) - int(p2[5], 16)) == 1
    except ValueError:
        return False


def matches_target_mac(candidate_mac: str, target_mac: str = DEFAULT_TARGET_MAC) -> bool:
    """Returns True if `candidate_mac` matches `target_mac` (or its ESP32 STA/AP pair)."""
    if not candidate_mac or not target_mac:
        return False
    n_cand = normalize_mac(candidate_mac)
    n_targ = normalize_mac(target_mac)
    if n_cand == n_targ:
        return True
    if {n_cand, n_targ} == {normalize_mac(DEFAULT_TARGET_MAC), normalize_mac(DEFAULT_AP_MAC)}:
        return True
    return is_esp32_mac_pair(n_cand, n_targ)


def probe_mps3_host(
    host: str,
    port: int = 80,
    target_mac: Optional[str] = DEFAULT_TARGET_MAC,
    timeout: float = 1.5,
) -> Optional[Dict[str, object]]:
    """
    Probes `http://<host>:<port>/api/ota` (with fallback to `/api/status`) and verifies
    the hardware MAC address if `target_mac` is specified.
    Returns device metadata dict (including `"ip"` and `"mac"`) if confirmed, else `None`.
    """
    url_ota = f"http://{host}:{port}/api/ota"
    try:
        req = urllib.request.Request(url_ota, headers={"Connection": "close"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            if resp.status == 200:
                data = json.loads(resp.read().decode("utf-8", errors="ignore"))
                if isinstance(data, dict):
                    dev_mac = str(data.get("mac", ""))
                    if not target_mac or not dev_mac or matches_target_mac(dev_mac, target_mac):
                        data["ip"] = host
                        data["port"] = port
                        return data
                    return None
    except Exception:
        pass

    # Fallback probe via /api/status if /api/ota is not mocked or unavailable
    url_status = f"http://{host}:{port}/api/status"
    try:
        req = urllib.request.Request(url_status, headers={"Connection": "close"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            if resp.status == 200:
                data = json.loads(resp.read().decode("utf-8", errors="ignore"))
                if isinstance(data, dict) and ("free" in data or "sd_status" in data or "total" in data):
                    dev_mac = str(data.get("mac", ""))
                    if dev_mac and target_mac and not matches_target_mac(dev_mac, target_mac):
                        return None
                    data["ip"] = host
                    data["port"] = port
                    return data
    except Exception:
        pass

    return None


def discover_via_zeroconf(
    target_mac: str = DEFAULT_TARGET_MAC,
    port: int = 80,
    timeout: float = 2.0,
) -> Optional[str]:
    """
    Discovers the MPS3 via mDNS (`_mps3._tcp.local.` or `mps3.local`) using `zeroconf`
    when available, plus standard `socket.gethostbyname("mps3.local")`.
    """
    # 1. Try zeroconf service browser for `_mps3._tcp.local.`
    try:
        from zeroconf import ServiceBrowser, ServiceStateChange, Zeroconf

        found_ips: List[Tuple[str, int, Optional[str]]] = []

        def on_service_state_change(
            zeroconf: Zeroconf,
            service_type: str,
            name: str,
            state_change: ServiceStateChange,
        ) -> None:
            if state_change is ServiceStateChange.Added:
                info = zeroconf.get_service_info(service_type, name, timeout=int(timeout * 1000))
                if info:
                    props = info.properties or {}
                    mac_bytes = props.get(b"mac")
                    mac_str = mac_bytes.decode("utf-8", errors="ignore") if mac_bytes else None
                    for addr in info.parsed_scoped_addresses():
                        found_ips.append((addr, info.port or port, mac_str))

        zc = Zeroconf()
        try:
            ServiceBrowser(zc, ["_mps3._tcp.local.", "_http._tcp.local."], handlers=[on_service_state_change])
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline and not found_ips:
                time.sleep(0.1)
        finally:
            zc.close()

        for ip, svc_port, svc_mac in found_ips:
            if svc_mac and target_mac and matches_target_mac(svc_mac, target_mac):
                return ip
            if probe_mps3_host(ip, port=svc_port, target_mac=target_mac, timeout=1.2):
                return ip
    except Exception:
        pass

    # 2. Try standard mDNS hostname resolution (`mps3.local`)
    try:
        ip = socket.gethostbyname("mps3.local")
        if ip and probe_mps3_host(ip, port=port, target_mac=target_mac, timeout=1.5):
            return ip
    except Exception:
        pass

    return None


def get_arp_neighbors() -> List[Tuple[str, str]]:
    """
    Reads the OS ARP/neighbor cache (`ip neigh` on Linux, `arp -a` cross-platform)
    and returns a list of `(ip, normalized_mac)` tuples.
    """
    entries: List[Tuple[str, str]] = []
    ip_re = re.compile(r"(\b\d{1,3}(?:\.\d{1,3}){3}\b)")
    mac_re = re.compile(r"([0-9a-fA-F]{2}(?:[:-][0-9a-fA-F]{2}){5})")

    commands = [["ip", "neigh"], ["arp", "-a"]]
    for cmd in commands:
        try:
            out = subprocess.check_output(
                cmd,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=2.0,
                errors="ignore",
            )
            for line in out.splitlines():
                ip_m = ip_re.search(line)
                mac_m = mac_re.search(line)
                if ip_m and mac_m:
                    entries.append((ip_m.group(1), normalize_mac(mac_m.group(1))))
            if entries:
                break
        except Exception:
            continue
    return entries


def get_local_subnets() -> List[str]:
    """Detects active local IPv4 `/24` subnet prefixes (e.g. `'192.168.15.'`)."""
    subnets: List[str] = []
    seen: Set[str] = set()

    def _add_ip(ip: str) -> None:
        parts = ip.split(".")
        if len(parts) == 4 and not ip.startswith("127.") and not ip.startswith("169.254."):
            prefix = f"{parts[0]}.{parts[1]}.{parts[2]}."
            if prefix not in seen:
                seen.add(prefix)
                subnets.append(prefix)

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            _add_ip(s.getsockname()[0])
    except Exception:
        pass

    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            _add_ip(str(info[4][0]))
    except Exception:
        pass

    for ip, _ in get_arp_neighbors():
        if not ip.startswith("224.") and not ip.startswith("239.") and not ip.endswith(".255"):
            _add_ip(ip)

    return subnets


def scan_subnets_for_mps3(
    target_mac: str = DEFAULT_TARGET_MAC,
    port: int = 80,
    timeout: float = 0.6,
) -> Optional[str]:
    """
    Performs a fast concurrent HTTP probe across local `/24` subnets to find
    the MPS3 and verify its hardware MAC via `GET /api/ota`.
    """
    # Always check ESP32 SoftAP default gateway first
    if probe_mps3_host("192.168.4.1", port=port, target_mac=target_mac, timeout=0.6):
        return "192.168.4.1"

    subnets = get_local_subnets()
    candidates: List[str] = []
    for prefix in subnets:
        for host_id in range(1, 255):
            candidates.append(f"{prefix}{host_id}")

    if not candidates:
        return None

    def _check_ip(ip: str) -> Optional[str]:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.settimeout(timeout)
                if sock.connect_ex((ip, port)) != 0:
                    return None
        except Exception:
            return None
        info = probe_mps3_host(ip, port=port, target_mac=target_mac, timeout=1.2)
        return ip if info else None

    with ThreadPoolExecutor(max_workers=48) as pool:
        futures = [pool.submit(_check_ip, ip) for ip in candidates]
        for fut in as_completed(futures):
            res = fut.result()
            if res:
                return res

    return None


def discover_mps3(
    target_host: Optional[str] = None,
    target_mac: str = DEFAULT_TARGET_MAC,
    port: int = 80,
    cache_dir: Optional[Path] = None,
    use_mdns: bool = True,
    use_subnet_scan: bool = True,
) -> Optional[str]:
    """
    Full cross-platform discovery pipeline:
    1. Explicit `target_host` (if provided)
    2. Last known cached IP in `cache_dir / "last_ip"`
    3. OS ARP / neighbor cache matching `target_mac`
    4. mDNS (`_mps3._tcp.local.` via `zeroconf` and `mps3.local`)
    5. Concurrent `/24` subnet TCP+HTTP probe with MAC verification
    """
    cache_file = (Path(cache_dir) / "last_ip") if cache_dir else None

    def _save_cache(ip: str) -> str:
        if cache_file:
            try:
                cache_file.parent.mkdir(parents=True, exist_ok=True)
                cache_file.write_text(f"{ip}\n", encoding="utf-8")
            except OSError:
                pass
        return ip

    # 1. Explicit target host
    if target_host:
        clean_host = target_host.strip()
        if "://" in clean_host:
            clean_host = clean_host.split("://", 1)[1].split("/", 1)[0]
        if ":" in clean_host:
            clean_host = clean_host.split(":", 1)[0]
        if probe_mps3_host(clean_host, port=port, target_mac=None, timeout=2.0):
            return _save_cache(clean_host)
        return None

    # 2. Cached IP
    if cache_file and cache_file.is_file():
        try:
            cached_ip = cache_file.read_text(encoding="utf-8").strip()
            if cached_ip and probe_mps3_host(cached_ip, port=port, target_mac=target_mac, timeout=1.2):
                return cached_ip
        except OSError:
            pass

    # 3. ARP / neighbor table match
    for ip, mac in get_arp_neighbors():
        if matches_target_mac(mac, target_mac):
            if probe_mps3_host(ip, port=port, target_mac=target_mac, timeout=1.5):
                return _save_cache(ip)

    # 4. mDNS / Zeroconf
    if use_mdns:
        mdns_ip = discover_via_zeroconf(target_mac=target_mac, port=port, timeout=1.5)
        if mdns_ip:
            return _save_cache(mdns_ip)

    # 5. Fast subnet scan
    if use_subnet_scan:
        scanned_ip = scan_subnets_for_mps3(target_mac=target_mac, port=port)
        if scanned_ip:
            return _save_cache(scanned_ip)

    return None
