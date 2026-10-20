"""
High-speed HTTP Push Client (`client.py`) for the MPS3 Audio Player.

Uses a persistent `requests.Session` (HTTP Keep-Alive) to communicate with the
MPS3 dual-core transfer server:
- `GET /api/sync/manifest` (with automatic fallback to `GET /api/podcasts_local` + `GET /api/status`)
- `POST /api/sync/begin`
- `PUT /api/upload` (to `/Podcasts/.staging/<Show>/<ep>` with `X-Expected-SHA256`)
- `POST /api/sync/verify` (read-back SHA-256 verification on SD card)
- `POST /api/rename` (atomic move from `/Podcasts/.staging/<Show>/<ep>` to `/Podcasts/<Show>/<ep>`)
- `DELETE /api/delete?path=/Podcasts/<Show>/<ep>`
- `POST /api/sync/done`
- Optional `X-MPS3-Key` pre-shared key header support
"""

from __future__ import annotations

import time
import urllib.parse
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, Iterator, List, Optional

import requests

from .ledger import SyncLedger, normalize_rel_path
from .planner import DeviceEpisode, ServerEpisode, SyncPlan


UPLOAD_CHUNK_SIZE = 64 * 1024  # 64 KB streaming chunks


class MPS3SyncError(Exception):
    """Base exception for MPS3 sync client errors."""


class MPS3AuthError(MPS3SyncError):
    """Raised when the MPS3 rejects a request with HTTP 401/403 (missing/invalid X-MPS3-Key)."""


class MPS3HashMismatchError(MPS3SyncError):
    """Raised when SHA-256 verification fails during upload or SD read-back."""


@dataclass
class UploadResult:
    """Telemetry and verification result for a single uploaded episode."""

    rel_path: str
    size_bytes: int
    sha256: str
    elapsed_sec: float
    speed_mbs: float
    upload_stats_header: str = ""
    verified_on_sd: bool = False
    attempts: int = 1


@dataclass
class SyncSessionSummary:
    """Result summary for an executed `SyncPlan`."""

    target_ip: str
    uploaded: List[UploadResult] = field(default_factory=list)
    deleted: List[str] = field(default_factory=list)
    errors: List[str] = field(default_factory=list)
    skipped_for_space: List[str] = field(default_factory=list)
    bytes_transferred: int = 0
    duration_sec: float = 0.0


def _stream_file_chunks(file_path: Path, chunk_size: int = UPLOAD_CHUNK_SIZE) -> Iterator[bytes]:
    """Generator yielding fixed-size byte chunks from `file_path`."""
    with open(file_path, "rb") as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            yield chunk


class MPS3Client:
    """HTTP Keep-Alive client for pushing podcasts to the MPS3."""

    def __init__(
        self,
        host: str,
        port: int = 80,
        api_key: Optional[str] = None,
        timeout: float = 30.0,
        max_retries: int = 3,
        retry_backoff_sec: float = 0.5,
    ):
        clean_host = host.strip()
        if "://" in clean_host:
            parsed = urllib.parse.urlparse(clean_host)
            clean_host = parsed.hostname or clean_host
            if parsed.port:
                port = parsed.port
        self.host = clean_host
        self.port = int(port)
        self.base_url = f"http://{self.host}:{self.port}"
        self.api_key = api_key
        self.timeout = float(timeout)
        self.max_retries = max(1, int(max_retries))
        self.retry_backoff_sec = float(retry_backoff_sec)

        self.session = requests.Session()
        self.session.headers.update(
            {
                "User-Agent": "MPS3-Sync-Client/2.0",
                "Connection": "keep-alive",
            }
        )
        if self.api_key:
            self.session.headers["X-MPS3-Key"] = self.api_key

    def close(self) -> None:
        self.session.close()

    def __enter__(self) -> "MPS3Client":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def _check_auth(self, resp: requests.Response) -> None:
        if resp.status_code in (401, 403):
            raise MPS3AuthError(
                f"Authentication failed (HTTP {resp.status_code}): check X-MPS3-Key / MPS3_KEY"
            )

    def get_status(self) -> Dict[str, Any]:
        """Calls `GET /api/status` and returns device storage & battery telemetry."""
        resp = self.session.get(f"{self.base_url}/api/status", timeout=self.timeout)
        self._check_auth(resp)
        resp.raise_for_status()
        return resp.json()

    def get_manifest(self) -> Dict[str, Any]:
        """
        Fetches `GET /api/sync/manifest`.
        If the firmware does not yet expose `/api/sync/manifest` (HTTP 404),
        seamlessly falls back to combining `GET /api/podcasts_local` + `GET /api/status`.
        """
        resp = self.session.get(f"{self.base_url}/api/sync/manifest", timeout=self.timeout)
        self._check_auth(resp)
        if resp.status_code == 200:
            data = resp.json()
            if isinstance(data, dict):
                return data

        if resp.status_code != 404:
            resp.raise_for_status()

        # Fallback: combine /api/podcasts_local and /api/status
        resp_pod = self.session.get(f"{self.base_url}/api/podcasts_local", timeout=self.timeout)
        self._check_auth(resp_pod)
        resp_pod.raise_for_status()
        pod_data = resp_pod.json() if resp_pod.content else {}

        status_data: Dict[str, Any] = {}
        try:
            status_data = self.get_status()
        except Exception:
            pass

        episodes = []
        for item in pod_data.get("podcasts", []):
            if not isinstance(item, dict):
                continue
            prog = item.get("program", "")
            fname = item.get("filename", "")
            rel_p = item.get("rel_path") or item.get("path") or (f"{prog}/{fname}" if prog and fname else "")
            norm_rel = normalize_rel_path(rel_p)
            episodes.append(
                {
                    "path": f"/Podcasts/{norm_rel}",
                    "rel_path": norm_rel,
                    "program": prog,
                    "filename": fname,
                    "size": int(item.get("size", 0)),
                    "played": bool(item.get("played", False)),
                }
            )

        manifest: Dict[str, Any] = {
            "podcasts": episodes,
            "played_history": pod_data.get("played_history", []),
        }
        if "free" in status_data:
            manifest["free_bytes"] = int(status_data["free"])
        if "total" in status_data:
            manifest["total_bytes"] = int(status_data["total"])
        if "battery_percent" in status_data:
            manifest["battery_percent"] = int(status_data["battery_percent"])
        return manifest

    def sync_begin(self, total_files: int = 0, total_bytes: int = 0) -> Dict[str, Any]:
        """Notifies the MPS3 that a sync session is starting (`POST /api/sync/begin`)."""
        payload = {"total_files": total_files, "total_bytes": total_bytes}
        resp = self.session.post(
            f"{self.base_url}/api/sync/begin",
            json=payload,
            timeout=self.timeout,
        )
        self._check_auth(resp)
        if resp.status_code == 404:
            return {"status": "fallback_no_endpoint"}
        resp.raise_for_status()
        return resp.json() if resp.content else {"status": "ok"}

    def sync_done(
        self,
        uploaded_count: int = 0,
        deleted_count: int = 0,
        error_count: int = 0,
    ) -> Dict[str, Any]:
        """Notifies the MPS3 that the sync session has finished (`POST /api/sync/done`)."""
        payload = {
            "uploaded": uploaded_count,
            "deleted": deleted_count,
            "errors": error_count,
        }
        resp = self.session.post(
            f"{self.base_url}/api/sync/done",
            json=payload,
            timeout=self.timeout,
        )
        self._check_auth(resp)
        if resp.status_code == 404:
            return {"status": "fallback_no_endpoint"}
        resp.raise_for_status()
        return resp.json() if resp.content else {"status": "ok"}

    def upload_to_staging(
        self,
        local_path: Path | str,
        staging_device_path: str,
        expected_sha256: str,
        idx: int = 1,
        count: int = 1,
        batch_total: int = 0,
    ) -> Dict[str, Any]:
        """
        Streams `local_path` via `PUT /api/upload` to `staging_device_path`
        (e.g. `/Podcasts/.staging/<Show>/<ep>`) with `X-Expected-SHA256`.
        """
        p = Path(local_path)
        file_size = p.stat().st_size
        encoded_path = urllib.parse.quote(staging_device_path, safe="/")
        url = (
            f"{self.base_url}/api/upload"
            f"?path={encoded_path}&idx={idx}&count={count}&batchTotal={batch_total or file_size}"
        )

        headers = {
            "Content-Type": "application/octet-stream",
            "Content-Length": str(file_size),
            "X-Expected-SHA256": expected_sha256.lower(),
        }

        # Give larger files plenty of socket read/write time while keeping connect timeout snappy
        upload_timeout = max(self.timeout, (file_size / (256 * 1024)) + 30.0)
        with open(p, "rb") as f:
            resp = self.session.put(
                url,
                data=f,
                headers=headers,
                timeout=(self.timeout, upload_timeout),
            )
        self._check_auth(resp)

        if resp.status_code == 422:
            device_hash = resp.headers.get("X-Content-SHA256", "")
            raise MPS3HashMismatchError(
                f"In-flight SHA-256 mismatch on upload of {staging_device_path}: "
                f"expected {expected_sha256}, device got {device_hash or resp.text}"
            )

        resp.raise_for_status()

        returned_hash = resp.headers.get("X-Content-SHA256", "").strip().lower()
        if returned_hash and returned_hash != expected_sha256.lower():
            raise MPS3HashMismatchError(
                f"Header X-Content-SHA256 mismatch for {staging_device_path}: "
                f"expected {expected_sha256.lower()}, got {returned_hash}"
            )

        return {
            "status_code": resp.status_code,
            "upload_stats": resp.headers.get("X-Upload-Stats", ""),
            "content_sha256": returned_hash,
        }

    def verify_on_sd(
        self,
        device_path: str,
        expected_sha256: str,
        expected_size: Optional[int] = None,
    ) -> bool:
        """
        Requests the MPS3 to re-read `device_path` from the SD card and verify its SHA-256
        (`POST /api/sync/verify?path=...`).
        """
        encoded_path = urllib.parse.quote(device_path, safe="/")
        url = f"{self.base_url}/api/sync/verify?path={encoded_path}"
        resp = self.session.post(
            url,
            json={"path": device_path, "expected_sha256": expected_sha256.lower()},
            timeout=(self.timeout, max(self.timeout, 120.0)),
        )
        self._check_auth(resp)

        if resp.status_code == 404:
            # Older firmware without /api/sync/verify endpoint
            return False

        if resp.status_code == 422:
            raise MPS3HashMismatchError(
                f"SD card read-back verification failed (HTTP 422) for {device_path}: {resp.text}"
            )

        resp.raise_for_status()
        data = resp.json() if resp.content else {}
        actual_sha = str(data.get("sha256", "")).strip().lower()
        actual_size = data.get("size")

        if actual_sha and actual_sha != expected_sha256.lower():
            raise MPS3HashMismatchError(
                f"SD read-back SHA-256 mismatch for {device_path}: "
                f"expected {expected_sha256.lower()}, got {actual_sha}"
            )
        if expected_size is not None and actual_size is not None and int(actual_size) != int(expected_size):
            raise MPS3HashMismatchError(
                f"SD read-back size mismatch for {device_path}: "
                f"expected {expected_size}, got {actual_size}"
            )
        return True

    def rename_path(self, old_path: str, new_path: str) -> None:
        """
        Atomically moves/renames `old_path` to `new_path` on the MPS3 SD card
        (`POST /api/rename`).
        """
        resp = self.session.post(
            f"{self.base_url}/api/rename",
            json={"old_path": old_path, "new_path": new_path},
            timeout=self.timeout,
        )
        self._check_auth(resp)
        resp.raise_for_status()

    def delete_path(self, device_path: str) -> None:
        """Deletes `device_path` on the MPS3 SD card (`DELETE /api/delete?path=...`)."""
        if not device_path.startswith("/"):
            device_path = f"/Podcasts/{normalize_rel_path(device_path)}"
        encoded_path = urllib.parse.quote(device_path, safe="/")
        resp = self.session.delete(
            f"{self.base_url}/api/delete?path={encoded_path}",
            timeout=self.timeout,
        )
        self._check_auth(resp)
        if resp.status_code not in (200, 204, 404):
            resp.raise_for_status()

    def upload_episode_atomic(
        self,
        episode: ServerEpisode,
        ledger: SyncLedger,
        idx: int = 1,
        count: int = 1,
        batch_total: int = 0,
    ) -> UploadResult:
        """
        Uploads a single episode using the 3-step atomic commit pipeline with retry:
        1. `PUT /api/upload` to `/Podcasts/.staging/<Show>/<filename>` with `X-Expected-SHA256`
        2. `POST /api/sync/verify` on `/Podcasts/.staging/<Show>/<filename>`
        3. `POST /api/rename` from `/Podcasts/.staging/<Show>/<filename>` to `/Podcasts/<Show>/<filename>`
        Retries up to `self.max_retries` times on failure, removing any partial `.staging` file.
        """
        norm_rel = normalize_rel_path(episode.rel_path)
        staging_path = f"/Podcasts/.staging/{norm_rel}"
        final_path = f"/Podcasts/{norm_rel}"
        expected_sha256 = ledger.get_file_sha256(episode.local_path, norm_rel)

        last_err: Optional[Exception] = None
        for attempt in range(1, self.max_retries + 1):
            t0 = time.perf_counter()
            try:
                up_info = self.upload_to_staging(
                    local_path=episode.local_path,
                    staging_device_path=staging_path,
                    expected_sha256=expected_sha256,
                    idx=idx,
                    count=count,
                    batch_total=batch_total,
                )
                verified = self.verify_on_sd(
                    device_path=staging_path,
                    expected_sha256=expected_sha256,
                    expected_size=episode.size,
                )
                # Ensure destination file is replaced cleanly if re-uploading a corrupted episode
                try:
                    self.rename_path(staging_path, final_path)
                except Exception:
                    try:
                        self.delete_path(final_path)
                    except Exception:
                        pass
                    self.rename_path(staging_path, final_path)

                elapsed = max(0.001, time.perf_counter() - t0)
                speed_mbs = (episode.size / (1024.0 * 1024.0)) / elapsed
                ledger.record_episode_sent(norm_rel, episode.size, expected_sha256)
                return UploadResult(
                    rel_path=norm_rel,
                    size_bytes=episode.size,
                    sha256=expected_sha256,
                    elapsed_sec=elapsed,
                    speed_mbs=speed_mbs,
                    upload_stats_header=str(up_info.get("upload_stats", "")),
                    verified_on_sd=verified,
                    attempts=attempt,
                )
            except MPS3AuthError:
                raise
            except Exception as e:
                last_err = e
                try:
                    self.delete_path(staging_path)
                except Exception:
                    pass
                if attempt < self.max_retries:
                    time.sleep(self.retry_backoff_sec * attempt)

        raise MPS3SyncError(
            f"Failed to upload '{norm_rel}' after {self.max_retries} attempts: {last_err}"
        )

    def execute_plan(
        self,
        plan: SyncPlan,
        ledger: SyncLedger,
        dry_run: bool = False,
        progress_cb: Optional[Callable[[str], None]] = None,
    ) -> SyncSessionSummary:
        """
        Executes a `SyncPlan` against the MPS3:
        - If `plan.delete_before_upload` is True (SD card nearly full), executes deletions
          before uploading so space is reclaimed first.
        - Otherwise, uploads and verifies all new episodes FIRST, and only deletes old played
          episodes AFTER uploads succeed.
        """
        t_start = time.perf_counter()
        summary = SyncSessionSummary(
            target_ip=self.host,
            skipped_for_space=[ep.rel_path for ep in plan.skipped_for_space],
        )

        if dry_run:
            return summary

        total_files = len(plan.to_upload)
        batch_total = sum(ep.size for ep in plan.to_upload)

        self.sync_begin(total_files=total_files, total_bytes=batch_total)
        try:
            if plan.delete_before_upload and plan.to_delete:
                self._run_deletions(plan.to_delete, ledger, summary, progress_cb)

            for idx, ep in enumerate(plan.to_upload, start=1):
                if progress_cb:
                    progress_cb(f"[{idx}/{total_files}] Uploading {ep.rel_path} ({ep.size // 1024} KB)...")
                try:
                    res = self.upload_episode_atomic(
                        episode=ep,
                        ledger=ledger,
                        idx=idx,
                        count=total_files,
                        batch_total=batch_total,
                    )
                    summary.uploaded.append(res)
                    summary.bytes_transferred += res.size_bytes
                    if progress_cb:
                        progress_cb(
                            f"  -> OK ({res.size_bytes / (1024 * 1024):.2f} MB in {res.elapsed_sec:.1f}s, "
                            f"{res.speed_mbs:.2f} MB/s, verified_on_sd={res.verified_on_sd}, sha256={res.sha256[:16]}...)"
                        )
                except MPS3AuthError:
                    raise
                except Exception as e:
                    if progress_cb:
                        progress_cb(f"  -> ERROR: {e}")
                    summary.errors.append(f"{ep.rel_path}: {e}")

            # Only delete after uploads if not already deleted before uploads
            if not plan.delete_before_upload and plan.to_delete:
                # If all uploads failed (and there were uploads scheduled), avoid wiping device
                if not plan.to_upload or len(summary.uploaded) > 0:
                    self._run_deletions(plan.to_delete, ledger, summary, progress_cb)
        finally:
            summary.duration_sec = time.perf_counter() - t_start
            ledger.record_sync_run(
                target_ip=self.host,
                uploaded=[u.rel_path for u in summary.uploaded],
                deleted=summary.deleted,
                errors=summary.errors,
                bytes_transferred=summary.bytes_transferred,
                duration_sec=summary.duration_sec,
            )
            ledger.save()
            try:
                self.sync_done(
                    uploaded_count=len(summary.uploaded),
                    deleted_count=len(summary.deleted),
                    error_count=len(summary.errors),
                )
            except Exception:
                pass

        return summary

    def _run_deletions(
        self,
        to_delete: List[DeviceEpisode],
        ledger: SyncLedger,
        summary: SyncSessionSummary,
        progress_cb: Optional[Callable[[str], None]] = None,
    ) -> None:
        for dev_ep in to_delete:
            if progress_cb:
                progress_cb(f"[DEL] Removing played episode {dev_ep.rel_path}...")
            try:
                self.delete_path(dev_ep.device_path)
                ledger.record_episode_deleted(dev_ep.rel_path)
                summary.deleted.append(dev_ep.rel_path)
            except MPS3AuthError:
                raise
            except Exception as e:
                summary.errors.append(f"DELETE {dev_ep.rel_path}: {e}")
