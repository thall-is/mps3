#!/usr/bin/env python3
"""
Automated Unit & Integration Tests for `tools/mps3_sync/` (`test_mps3_sync_planner.py`).

Validates:
1. Keeping top 3 episodes of each podcast show even when played.
2. Keeping/uploading 4th, 5th episodes when unplayed.
3. Deleting 4th, 5th episodes from the MPS3 when played.
4. Never re-uploading an episode outside top 3 that was previously played and deleted
   (present in `ledger.played_episodes` or `manifest["played_history"]`).
5. Re-uploading a corrupted/partial episode if its size on the MPS3 mismatches.
6. Ledger SHA-256 caching (keyed by `(rel_path, size, mtime_ns)`) and atomic persistence.
7. Never deleting an unplayed episode on the MPS3 even if removed from the server.
8. SD card free space safety reserve (32 MB) and `delete_before_upload` ordering.
9. End-to-end HTTP Push Client (`MPS3Client`) staging, SHA-256 verification, atomic rename,
   and deletion against a simulated MPS3 HTTP server.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from typing import Dict, List

# Add tools/mps3_sync to sys.path
REPO_ROOT = Path(__file__).resolve().parent.parent
MPS3_SYNC_ROOT = REPO_ROOT / "tools" / "mps3_sync"
if str(MPS3_SYNC_ROOT) not in sys.path:
    sys.path.insert(0, str(MPS3_SYNC_ROOT))

from mps3_sync.client import MPS3AuthError, MPS3Client, MPS3HashMismatchError
from mps3_sync.ledger import SyncLedger, normalize_rel_path
from mps3_sync.planner import SyncPlanner, extract_episode_date, scan_server_episodes


def _create_episode_file(
    base_dir: Path,
    show: str,
    filename: str,
    size: int = 4096,
    mtime: float | None = None,
) -> Path:
    show_dir = base_dir / show
    show_dir.mkdir(parents=True, exist_ok=True)
    ep_path = show_dir / filename
    payload = (f"{show}/{filename}:".encode("utf-8") * ((size // 16) + 1))[:size]
    ep_path.write_bytes(payload)
    if mtime is not None:
        os.utime(ep_path, (mtime, mtime))
    return ep_path


class TestMPS3SyncPlanner(unittest.TestCase):
    """Unit tests for `SyncPlanner` and `SyncLedger`."""

    def setUp(self) -> None:
        self.tmp_dir = Path(tempfile.mkdtemp(prefix="mps3_sync_test_"))
        self.podcasts_dir = self.tmp_dir / "Podcasts"
        self.podcasts_dir.mkdir(parents=True, exist_ok=True)
        self.state_file = self.tmp_dir / "state.json"
        self.ledger = SyncLedger(self.state_file)
        self.planner = SyncPlanner(keep_recent=3)

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp_dir, ignore_errors=True)

    def _populate_five_episodes(self, show: str = "Jovem Nerd") -> List[str]:
        """
        Creates 5 episodes sorted newest (ep5) to oldest (ep1) using YYYY-MM-DD dates in filenames:
        - ep5: 2026-10-05_ep5.mp3 (top 1)
        - ep4: 2026-10-04_ep4.mp3 (top 2)
        - ep3: 2026-10-03_ep3.mp3 (top 3)
        - ep2: 2026-10-02_ep2.mp3 (4th)
        - ep1: 2026-10-01_ep1.mp3 (5th)
        """
        names = [
            "2026-10-05_ep5.mp3",
            "2026-10-04_ep4.mp3",
            "2026-10-03_ep3.mp3",
            "2026-10-02_ep2.mp3",
            "2026-10-01_ep1.mp3",
        ]
        base_ts = 1790000000.0
        for idx, name in enumerate(names):
            # Give older files older mtimes as well
            _create_episode_file(
                self.podcasts_dir,
                show,
                name,
                size=10000 + idx * 100,
                mtime=base_ts - idx * 3600,
            )
        return [f"{show}/{n}" for n in names]

    def test_01_keep_top_3_episodes_even_when_played(self) -> None:
        """
        Rule 1 & 3: The 3 most recent episodes of each podcast MUST be kept on the MPS3
        (and uploaded if missing) even when they have already been played.
        """
        rels = self._populate_five_episodes("Jovem Nerd")
        top3 = rels[:3]
        older2 = rels[3:]

        # Mark all 5 episodes as played in the manifest; top 3 are currently on the device
        manifest = {
            "podcasts": [
                {
                    "path": f"/Podcasts/{rel}",
                    "size": (self.podcasts_dir / rel).stat().st_size,
                    "played": True,
                }
                for rel in top3
            ],
            "played_history": older2,
        }

        plan = self.planner.create_plan(self.podcasts_dir, manifest, self.ledger)
        desired_rels = [ep.rel_path for ep in plan.desired_on_device]
        delete_rels = [ep.rel_path for ep in plan.to_delete]
        upload_rels = [ep.rel_path for ep in plan.to_upload]

        # Top 3 must be in desired_on_device and MUST NOT be in to_delete
        for rel in top3:
            self.assertIn(rel, desired_rels, f"Top-3 episode {rel} must remain desired even when played")
            self.assertNotIn(rel, delete_rels, f"Top-3 episode {rel} must NEVER be deleted even when played")

        # Since top 3 are already on the device with matching size, nothing needs uploading
        self.assertEqual(upload_rels, [])
        self.assertEqual(delete_rels, [])

        # If one of the played top-3 episodes is missing from the device, it is uploaded
        manifest_missing_top1 = {
            "podcasts": [
                {
                    "path": f"/Podcasts/{rel}",
                    "size": (self.podcasts_dir / rel).stat().st_size,
                    "played": True,
                }
                for rel in top3[1:]
            ],
            "played_history": [top3[0]] + older2,
        }
        plan2 = self.planner.create_plan(self.podcasts_dir, manifest_missing_top1, self.ledger)
        self.assertEqual([ep.rel_path for ep in plan2.to_upload], [top3[0]])

    def test_02_keep_4th_and_5th_episodes_when_unplayed(self) -> None:
        """
        Rule 3: 4th and 5th episodes MUST be kept on the device (and uploaded if missing)
        when they have NOT been played.
        """
        rels = self._populate_five_episodes("Broxada")
        top3 = rels[:3]
        ep4, ep5 = rels[3], rels[4]

        # Suppose top3 and ep4 are on the device (ep4 unplayed), and ep5 is not yet on the device (also unplayed)
        manifest = {
            "podcasts": [
                {
                    "path": f"/Podcasts/{rel}",
                    "size": (self.podcasts_dir / rel).stat().st_size,
                    "played": True,
                }
                for rel in top3
            ]
            + [
                {
                    "path": f"/Podcasts/{ep4}",
                    "size": (self.podcasts_dir / ep4).stat().st_size,
                    "played": False,
                }
            ],
            "played_history": [],
        }

        plan = self.planner.create_plan(self.podcasts_dir, manifest, self.ledger)
        desired_rels = [ep.rel_path for ep in plan.desired_on_device]
        upload_rels = [ep.rel_path for ep in plan.to_upload]
        delete_rels = [ep.rel_path for ep in plan.to_delete]

        # Both ep4 and ep5 are unplayed -> both belong in desired_on_device
        self.assertIn(ep4, desired_rels)
        self.assertIn(ep5, desired_rels)
        # ep4 is already on device -> not deleted, not re-uploaded
        self.assertNotIn(ep4, delete_rels)
        self.assertNotIn(ep4, upload_rels)
        # ep5 is unplayed and missing on device -> scheduled for upload
        self.assertEqual(upload_rels, [ep5])
        self.assertEqual(delete_rels, [])

    def test_03_delete_4th_and_5th_episodes_when_played(self) -> None:
        """
        Rule 4: 4th and 5th episodes currently on the MPS3 MUST be scheduled for deletion
        (`to_delete`) when they are marked played (on device or in ledger).
        """
        rels = self._populate_five_episodes("Vortex Podcast")
        top3 = rels[:3]
        ep4, ep5 = rels[3], rels[4]

        # All 5 are on the MPS3; ep4 is marked played=True in manifest, ep5 is in ledger.played_episodes
        self.ledger.add_played(ep5)
        manifest = {
            "podcasts": [
                {
                    "path": f"/Podcasts/{rel}",
                    "size": (self.podcasts_dir / rel).stat().st_size,
                    "played": (rel == ep4),
                }
                for rel in rels
            ],
            "played_history": [],
        }

        plan = self.planner.create_plan(self.podcasts_dir, manifest, self.ledger)
        delete_rels = [ep.rel_path for ep in plan.to_delete]
        desired_rels = [ep.rel_path for ep in plan.desired_on_device]

        self.assertCountEqual(delete_rels, [ep4, ep5])
        for rel in top3:
            self.assertIn(rel, desired_rels)
            self.assertNotIn(rel, delete_rels)
        self.assertNotIn(ep4, desired_rels)
        self.assertNotIn(ep5, desired_rels)

    def test_04_never_reupload_played_and_deleted_episode_outside_top_3(self) -> None:
        """
        Rule 1, 3 & 4: An episode outside the top 3 that was previously played and deleted from the MPS3
        (present in `ledger.played_episodes` or `manifest["played_history"]`) MUST NEVER be re-uploaded!
        """
        rels = self._populate_five_episodes("Hora Boa Podcast")
        top3 = rels[:3]
        ep4, ep5 = rels[3], rels[4]

        # Pre-record ep4 in persistent ledger, and report ep5 in manifest["played_history"].
        # Neither ep4 nor ep5 is currently on the MPS3 SD card (they were already deleted).
        self.ledger.add_played(ep4)
        manifest = {
            "podcasts": [
                {
                    "path": f"/Podcasts/{rel}",
                    "size": (self.podcasts_dir / rel).stat().st_size,
                    "played": False,
                }
                for rel in top3
            ],
            "played_history": [f"/Podcasts/{ep5}"],
        }

        plan = self.planner.create_plan(self.podcasts_dir, manifest, self.ledger)
        upload_rels = [ep.rel_path for ep in plan.to_upload]
        desired_rels = [ep.rel_path for ep in plan.desired_on_device]

        self.assertNotIn(ep4, desired_rels)
        self.assertNotIn(ep5, desired_rels)
        self.assertNotIn(ep4, upload_rels)
        self.assertNotIn(ep5, upload_rels)
        self.assertEqual(upload_rels, [])

        # Verify ep5 was persisted into state.json by reloading a fresh SyncLedger instance
        reloaded_ledger = SyncLedger(self.state_file)
        self.assertTrue(reloaded_ledger.is_played(ep4))
        self.assertTrue(reloaded_ledger.is_played(ep5))

        # Even if future manifests have empty played_history, ledger remembers both ep4 and ep5
        manifest_next_cycle = {
            "podcasts": [
                {
                    "path": f"/Podcasts/{rel}",
                    "size": (self.podcasts_dir / rel).stat().st_size,
                    "played": False,
                }
                for rel in top3
            ],
            "played_history": [],
        }
        plan_next = self.planner.create_plan(self.podcasts_dir, manifest_next_cycle, reloaded_ledger)
        self.assertEqual([ep.rel_path for ep in plan_next.to_upload], [])

    def test_05_reupload_corrupted_or_partial_episode_on_size_mismatch(self) -> None:
        """
        Rule 4: If an episode in `desired_on_device` is on the MPS3 but its size on the MPS3
        differs from the server file size (corrupted/partial file), it MUST be re-uploaded.
        """
        rels = self._populate_five_episodes("Atila Iamarino")
        top3 = rels[:3]
        corrupted_rel = top3[1]

        manifest_podcasts = []
        for rel in top3:
            true_size = (self.podcasts_dir / rel).stat().st_size
            reported_size = true_size - 512 if rel == corrupted_rel else true_size
            manifest_podcasts.append(
                {
                    "path": f"/Podcasts/{rel}",
                    "size": reported_size,
                    "played": False,
                }
            )

        manifest = {
            "podcasts": manifest_podcasts,
            "played_history": rels[3:],
        }

        plan = self.planner.create_plan(self.podcasts_dir, manifest, self.ledger)
        upload_rels = [ep.rel_path for ep in plan.to_upload]
        self.assertEqual(upload_rels, [corrupted_rel])

    def test_06_ledger_sha256_caching_and_atomic_persistence(self) -> None:
        """
        Rule 6: Validates SHA-256 caching keyed by `(rel_path, size, mtime_ns)` and
        atomic `state.json` persistence via `.tmp` + `os.replace()`.
        """
        ep_path = _create_episode_file(
            self.podcasts_dir,
            "Desce a Letra",
            "2026-10-09_dlshow_655.mp3",
            size=65536,
        )
        rel_path = "Desce a Letra/2026-10-09_dlshow_655.mp3"
        expected_sha = hashlib.sha256(ep_path.read_bytes()).hexdigest()

        # First call computes SHA-256 and populates cache
        computed_sha = self.ledger.get_file_sha256(ep_path, rel_path)
        self.assertEqual(computed_sha, expected_sha)
        self.assertIn(rel_path, self.ledger.sha256_cache)

        # Save atomically and verify no leftover .tmp files exist
        self.ledger.add_played("Desce a Letra/2026-09-01_old.mp3")
        self.ledger.save()
        tmp_files = list(self.tmp_dir.glob("*.tmp"))
        self.assertEqual(tmp_files, [], "No temporary .tmp files should remain after atomic save")
        self.assertTrue(self.state_file.is_file())

        # Reload into a new SyncLedger instance and verify cache hit without re-reading disk
        ledger2 = SyncLedger(self.state_file)
        self.assertTrue(ledger2.is_played("desce a letra/2026-09-01_old.mp3"))
        self.assertIn(rel_path, ledger2.sha256_cache)

        # Tamper with cached sha256 in memory while keeping (size, mtime_ns) identical to prove cache hit
        sentinel_hash = "a" * 64
        ledger2.sha256_cache[rel_path]["sha256"] = sentinel_hash
        self.assertEqual(ledger2.get_file_sha256(ep_path, rel_path), sentinel_hash)

        # Now modify file size/mtime -> cache must invalidate and recompute real SHA-256
        ep_path.write_bytes(b"new_audio_content" * 1000)
        new_expected = hashlib.sha256(ep_path.read_bytes()).hexdigest()
        self.assertEqual(ledger2.get_file_sha256(ep_path, rel_path), new_expected)

    def test_07_never_delete_unplayed_episode_removed_from_server(self) -> None:
        """
        Rule 4 note: Never delete an unplayed episode from the MPS3 even if it was removed from the server!
        """
        rels = self._populate_five_episodes("Jovem Nerd")
        # Episode on MPS3 that no longer exists on server, but is UNPLAYED
        orphan_unplayed = "Jovem Nerd/2025-01-01_classic_unplayed.mp3"
        # Episode on MPS3 that no longer exists on server, and WAS PLAYED
        orphan_played = "Jovem Nerd/2025-01-02_classic_played.mp3"

        manifest = {
            "podcasts": [
                {"path": f"/Podcasts/{orphan_unplayed}", "size": 50000, "played": False},
                {"path": f"/Podcasts/{orphan_played}", "size": 50000, "played": True},
            ],
            "played_history": [],
        }

        plan = self.planner.create_plan(self.podcasts_dir, manifest, self.ledger)
        delete_rels = [ep.rel_path for ep in plan.to_delete]
        self.assertNotIn(orphan_unplayed, delete_rels, "Unplayed episode on MPS3 must NEVER be deleted")
        self.assertIn(orphan_played, delete_rels, "Played episode outside top 3 must be deleted")

    def test_08_space_safety_reserve_and_end_to_end_mock_push(self) -> None:
        """
        Rule 5: Validates 32 MB safety reserve, `delete_before_upload` when space is tight,
        and end-to-end execution via `MPS3Client` against an in-process mock MPS3 HTTP server.
        """
        rels = self._populate_five_episodes("TechCast")
        top3 = rels[:3]
        ep4, ep5 = rels[3], rels[4]

        # Mark ep4 & ep5 as played on the device so they can be deleted to free space
        ep4_size = 20 * 1024 * 1024  # 20 MB on device
        ep5_size = 20 * 1024 * 1024  # 20 MB on device
        # Free space is 32 MB + 5 KB (not enough for all 3 top episodes without deleting ep4/ep5 first!)
        reserve = 32 * 1024 * 1024
        manifest = {
            "free_bytes": reserve + 5000,
            "podcasts": [
                {"path": f"/Podcasts/{ep4}", "size": ep4_size, "played": True},
                {"path": f"/Podcasts/{ep5}", "size": ep5_size, "played": True},
            ],
            "played_history": [],
        }

        plan = self.planner.create_plan(self.podcasts_dir, manifest, self.ledger)
        self.assertTrue(
            plan.delete_before_upload,
            "Planner must set delete_before_upload=True when free_bytes - 32MB < upload_bytes",
        )
        self.assertEqual(len(plan.to_upload), 3)
        self.assertEqual(len(plan.to_delete), 2)

        # Spin up a mock MPS3 HTTP server to verify the full upload -> verify -> rename -> delete flow
        mock_state = {
            "files": {
                f"/Podcasts/{ep4}": b"x" * 1024,
                f"/Podcasts/{ep5}": b"y" * 1024,
            },
            "events": [],
            "api_key": "secret-test-key-123",
        }

        class MockMPS3Handler(BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass

            def _auth_ok(self) -> bool:
                if self.headers.get("X-MPS3-Key") != mock_state["api_key"]:
                    self.send_response(401)
                    self.end_headers()
                    return False
                return True

            def do_POST(self):
                if not self._auth_ok():
                    return
                parsed = urllib.parse.urlparse(self.path)
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length) if length > 0 else b"{}"

                if parsed.path == "/api/sync/begin":
                    mock_state["events"].append("begin")
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(b'{"status":"ok"}')
                elif parsed.path == "/api/sync/verify":
                    q = urllib.parse.parse_qs(parsed.query)
                    p = urllib.parse.unquote(q["path"][0])
                    content = mock_state["files"].get(p, b"")
                    sha = hashlib.sha256(content).hexdigest()
                    mock_state["events"].append(f"verify:{p}")
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(json.dumps({"size": len(content), "sha256": sha}).encode("utf-8"))
                elif parsed.path == "/api/rename":
                    data = json.loads(body.decode("utf-8"))
                    old_p, new_p = data["old_path"], data["new_path"]
                    mock_state["files"][new_p] = mock_state["files"].pop(old_p)
                    mock_state["events"].append(f"rename:{old_p}->{new_p}")
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(b'{"status":"ok"}')
                elif parsed.path == "/api/sync/done":
                    mock_state["events"].append("done")
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(b'{"status":"ok"}')
                else:
                    self.send_response(404)
                    self.end_headers()

            def do_PUT(self):
                if not self._auth_ok():
                    return
                parsed = urllib.parse.urlparse(self.path)
                q = urllib.parse.parse_qs(parsed.query)
                p = urllib.parse.unquote(q["path"][0])
                length = int(self.headers.get("Content-Length", "0"))
                expected_sha = self.headers.get("X-Expected-SHA256", "").lower()
                data = self.rfile.read(length)
                actual_sha = hashlib.sha256(data).hexdigest().lower()
                if expected_sha and actual_sha != expected_sha:
                    self.send_response(422)
                    self.send_header("X-Content-SHA256", actual_sha)
                    self.end_headers()
                    return
                mock_state["files"][p] = data
                mock_state["events"].append(f"upload:{p}")
                self.send_response(200)
                self.send_header("X-Content-SHA256", actual_sha)
                self.send_header("X-Upload-Stats", "rate_kbs=1420")
                self.end_headers()
                self.wfile.write(b'{"status":"ok"}')

            def do_DELETE(self):
                if not self._auth_ok():
                    return
                parsed = urllib.parse.urlparse(self.path)
                q = urllib.parse.parse_qs(parsed.query)
                p = urllib.parse.unquote(q["path"][0])
                mock_state["files"].pop(p, None)
                mock_state["events"].append(f"delete:{p}")
                self.send_response(200)
                self.end_headers()

        server = HTTPServer(("127.0.0.1", 0), MockMPS3Handler)
        port = server.server_port
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            # Verify invalid key raises MPS3AuthError
            with MPS3Client("127.0.0.1", port=port, api_key="wrong-key") as bad_client:
                with self.assertRaises(MPS3AuthError):
                    bad_client.sync_begin()

            # Execute plan with valid key
            with MPS3Client("127.0.0.1", port=port, api_key=mock_state["api_key"]) as client:
                summary = client.execute_plan(plan, self.ledger)

            self.assertEqual(len(summary.uploaded), 3)
            self.assertEqual(len(summary.deleted), 2)
            self.assertEqual(summary.errors, [])

            # Because delete_before_upload=True, deletions must happen right after 'begin' and before 'upload'
            first_upload_idx = next(i for i, ev in enumerate(mock_state["events"]) if ev.startswith("upload:"))
            last_delete_idx = max(i for i, ev in enumerate(mock_state["events"]) if ev.startswith("delete:"))
            self.assertLess(last_delete_idx, first_upload_idx)

            # Final files on mock SD card must be exactly the top 3 episodes
            for rel in top3:
                self.assertIn(f"/Podcasts/{rel}", mock_state["files"])
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
