"""
Persistent Ledger (`state.json`) for the MPS3 Podcast Sync engine.

Provides:
- Atomic persistence via temporary file + `os.replace()` across Linux, macOS, and Windows.
- SHA-256 hash cache keyed by `(rel_path, size, mtime_ns)` so large audio files are hashed once.
- Cumulative `played_episodes` tracking (never forgets a heard episode even after deletion from MPS3).
- Sync run history and per-episode status tracking.
"""

from __future__ import annotations

import hashlib
import json
import os
import time
import unicodedata
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Set


LEDGER_SCHEMA_VERSION = 1
HASH_CHUNK_SIZE = 128 * 1024  # 128 KB chunks for fast file hashing
MAX_SYNC_HISTORY = 100


def normalize_rel_path(rel_path: str) -> str:
    """
    Normalizes an episode relative path consistently as `<Show>/<filename>`:
    - Converts backslashes to forward slashes
    - Normalizes Unicode to NFC (macOS uses NFD, FAT32/Windows/Linux expect NFC)
    - Strips leading slashes and prefixes like `Podcasts/` or `Recentes/` or `Arquivos/`
    """
    if not rel_path:
        return ""
    s = unicodedata.normalize("NFC", str(rel_path)).replace("\\", "/").strip()
    while "//" in s:
        s = s.replace("//", "/")
    s = s.lstrip("/")

    # Strip known container prefixes case-insensitively
    prefixes = ("podcasts/", "recentes/", "arquivos/")
    changed = True
    while changed and "/" in s:
        changed = False
        lower_s = s.lower()
        for prefix in prefixes:
            if lower_s.startswith(prefix):
                s = s[len(prefix):].lstrip("/")
                changed = True
                break
    return s


def normalize_key(rel_path: str) -> str:
    """Returns a case-folded normalized `<show>/<filename>` key for set comparisons."""
    return normalize_rel_path(rel_path).casefold()


class SyncLedger:
    """
    Persistent state store backed by `state.json`.

    Attributes:
        state_file: Path to the JSON state file on disk.
        played_episodes: Set of normalized episode paths (`<Show>/<filename>`) that have been played.
        sha256_cache: Mapping of `rel_path` -> `{"sha256": str, "size": int, "mtime_ns": int}`.
        episodes: Mapping of `rel_path` -> metadata (`status`, `sent_at`, `played_at`, `sha256`, `size`).
        sync_history: List of past sync run records.
    """

    def __init__(self, state_file: Path | str):
        self.state_file = Path(state_file)
        self.schema_version: int = LEDGER_SCHEMA_VERSION
        self.played_episodes: Set[str] = set()
        self._played_keys_map: Dict[str, str] = {}  # casefold key -> canonical rel_path
        self.sha256_cache: Dict[str, Dict[str, Any]] = {}
        self.episodes: Dict[str, Dict[str, Any]] = {}
        self.sync_history: List[Dict[str, Any]] = []
        self.load()

    def load(self) -> None:
        """Loads state from `self.state_file` if it exists."""
        if not self.state_file.is_file():
            return
        try:
            raw = self.state_file.read_text(encoding="utf-8")
            data = json.loads(raw)
            if not isinstance(data, dict):
                return
            self.schema_version = int(data.get("schema_version", LEDGER_SCHEMA_VERSION))

            raw_played = data.get("played_episodes", [])
            self.played_episodes = set()
            self._played_keys_map = {}
            if isinstance(raw_played, list):
                for item in raw_played:
                    if isinstance(item, str) and item.strip():
                        self.add_played(item)

            raw_cache = data.get("sha256_cache", {})
            if isinstance(raw_cache, dict):
                self.sha256_cache = {
                    normalize_rel_path(k): v
                    for k, v in raw_cache.items()
                    if isinstance(v, dict)
                }

            raw_eps = data.get("episodes", {})
            if isinstance(raw_eps, dict):
                self.episodes = {
                    normalize_rel_path(k): v
                    for k, v in raw_eps.items()
                    if isinstance(v, dict)
                }
                for rel_p, meta in self.episodes.items():
                    if meta.get("status") == "played" or meta.get("played") is True:
                        self.add_played(rel_p)

            raw_hist = data.get("sync_history", [])
            if isinstance(raw_hist, list):
                self.sync_history = raw_hist[-MAX_SYNC_HISTORY:]
        except Exception:
            # Corrupted or unreadable state file: start clean without crashing
            pass

    def save(self) -> None:
        """
        Atomically writes the ledger to `self.state_file` using a temporary file
        in the same directory followed by `os.replace()`.
        """
        self.state_file.parent.mkdir(parents=True, exist_ok=True)
        tmp_file = self.state_file.parent / f".{self.state_file.name}.{os.getpid()}.tmp"

        payload = {
            "schema_version": self.schema_version,
            "updated_at": int(time.time()),
            "played_episodes": sorted(self.played_episodes),
            "sha256_cache": self.sha256_cache,
            "episodes": self.episodes,
            "sync_history": self.sync_history[-MAX_SYNC_HISTORY:],
        }

        try:
            content = json.dumps(payload, indent=2, ensure_ascii=False)
            with open(tmp_file, "w", encoding="utf-8", newline="\n") as f:
                f.write(content)
                f.flush()
                try:
                    os.fsync(f.fileno())
                except OSError:
                    pass
            os.replace(str(tmp_file), str(self.state_file))
        finally:
            if tmp_file.exists():
                try:
                    tmp_file.unlink()
                except OSError:
                    pass

    def add_played(self, rel_path: str, played_at: Optional[int] = None) -> bool:
        """
        Marks `rel_path` as played in the cumulative `played_episodes` set.
        Returns True if this episode was newly added, False if already known as played.
        """
        norm = normalize_rel_path(rel_path)
        if not norm:
            return False
        key = norm.casefold()
        is_new = key not in self._played_keys_map
        canonical = self._played_keys_map.get(key, norm)
        self._played_keys_map[key] = canonical
        self.played_episodes.add(canonical)

        now = played_at if played_at is not None else int(time.time())
        ep_entry = self.episodes.setdefault(canonical, {})
        ep_entry["status"] = "played"
        ep_entry["played"] = True
        ep_entry.setdefault("played_at", now)
        return is_new

    def is_played(self, rel_path: str) -> bool:
        """Checks whether `rel_path` has been played (case-insensitive, normalized)."""
        key = normalize_key(rel_path)
        return bool(key) and key in self._played_keys_map

    def get_played_keys(self) -> Set[str]:
        """Returns the set of case-folded normalized `<show>/<filename>` keys for all played episodes."""
        # Ensure any direct mutations to `self.played_episodes` are reflected
        for p in list(self.played_episodes):
            norm = normalize_rel_path(p)
            if norm:
                self._played_keys_map.setdefault(norm.casefold(), norm)
        return set(self._played_keys_map.keys())

    def forget_show(self, show_name: str) -> int:
        """Removes all played history entries belonging to `show_name`. Returns count removed."""
        prefix = f"{show_name.strip().casefold()}/"
        to_remove_keys = [k for k in self._played_keys_map if k.startswith(prefix)]
        for k in to_remove_keys:
            canonical = self._played_keys_map.pop(k, None)
            if canonical:
                self.played_episodes.discard(canonical)
                self.episodes.pop(canonical, None)
        return len(to_remove_keys)

    def reconcile_manifest(self, manifest: Dict[str, Any]) -> int:
        """
        Merges all episodes marked `played: true` in `manifest["podcasts"]`
        AND all entries in `manifest["played_history"]` into `self.played_episodes`.

        Returns the number of newly marked played episodes.
        """
        if not isinstance(manifest, dict):
            return 0

        newly_marked = 0

        # 1. Episodes currently on the MPS3 with played=True
        podcasts = manifest.get("podcasts", [])
        if isinstance(podcasts, list):
            for item in podcasts:
                if not isinstance(item, dict):
                    continue
                if item.get("played") is True:
                    raw_path = (
                        item.get("rel_path")
                        or item.get("path")
                        or (
                            f"{item['program']}/{item['filename']}"
                            if item.get("program") and item.get("filename")
                            else ""
                        )
                    )
                    if raw_path and self.add_played(raw_path):
                        newly_marked += 1

        # 2. Cumulative played_history reported by the MPS3
        played_history = manifest.get("played_history", [])
        if isinstance(played_history, Iterable) and not isinstance(played_history, (str, bytes)):
            for entry in played_history:
                if isinstance(entry, str):
                    if self.add_played(entry):
                        newly_marked += 1
                elif isinstance(entry, dict):
                    raw_path = (
                        entry.get("rel_path")
                        or entry.get("path")
                        or (
                            f"{entry['program']}/{entry['filename']}"
                            if entry.get("program") and entry.get("filename")
                            else ""
                        )
                    )
                    if raw_path and self.add_played(raw_path):
                        newly_marked += 1

        return newly_marked

    def get_file_sha256(self, file_path: Path | str, rel_path: Optional[str] = None) -> str:
        """
        Returns the lowercase SHA-256 hex digest of `file_path`.
        Uses `self.sha256_cache` keyed by `(rel_path, size, mtime_ns)` to avoid re-hashing
        unchanged files on the server.
        """
        p = Path(file_path)
        st = p.stat()
        size = st.st_size
        mtime_ns = getattr(st, "st_mtime_ns", int(st.st_mtime * 1_000_000_000))
        norm_rel = normalize_rel_path(rel_path) if rel_path else p.name

        cached = self.sha256_cache.get(norm_rel)
        if (
            isinstance(cached, dict)
            and cached.get("size") == size
            and cached.get("mtime_ns") == mtime_ns
            and isinstance(cached.get("sha256"), str)
            and len(cached["sha256"]) == 64
        ):
            return cached["sha256"]

        h = hashlib.sha256()
        with open(p, "rb") as f:
            while True:
                chunk = f.read(HASH_CHUNK_SIZE)
                if not chunk:
                    break
                h.update(chunk)
        digest = h.hexdigest().lower()

        self.sha256_cache[norm_rel] = {
            "sha256": digest,
            "size": size,
            "mtime_ns": mtime_ns,
        }
        return digest

    def record_episode_sent(self, rel_path: str, size: int, sha256: str) -> None:
        """Records that an episode was successfully uploaded and verified on the MPS3."""
        norm = normalize_rel_path(rel_path)
        if not norm:
            return
        now = int(time.time())
        ep = self.episodes.setdefault(norm, {})
        if not self.is_played(norm):
            ep["status"] = "sent"
        ep["size"] = size
        ep["sha256"] = sha256
        ep["sent_at"] = now

    def record_episode_deleted(self, rel_path: str) -> None:
        """Records that a played episode outside top_recent was deleted from the MPS3."""
        norm = normalize_rel_path(rel_path)
        if not norm:
            return
        self.add_played(norm)
        ep = self.episodes.setdefault(norm, {})
        ep["deleted_at"] = int(time.time())

    def record_sync_run(
        self,
        target_ip: str,
        uploaded: List[str],
        deleted: List[str],
        errors: List[str],
        bytes_transferred: int,
        duration_sec: float,
    ) -> None:
        """Appends a sync session summary to `self.sync_history`."""
        self.sync_history.append(
            {
                "timestamp": int(time.time()),
                "target_ip": target_ip,
                "uploaded_count": len(uploaded),
                "deleted_count": len(deleted),
                "error_count": len(errors),
                "uploaded": [normalize_rel_path(u) for u in uploaded],
                "deleted": [normalize_rel_path(d) for d in deleted],
                "errors": errors,
                "bytes_transferred": bytes_transferred,
                "duration_sec": round(duration_sec, 3),
            }
        )
        if len(self.sync_history) > MAX_SYNC_HISTORY:
            self.sync_history = self.sync_history[-MAX_SYNC_HISTORY:]
