"""
Pure, deterministic retention and diff engine (`planner.py`) for MPS3 Podcast Sync.

Implements the exact retention and reconciliation rules:
1. Reconcile Heard State:
   - Merge all episodes reported as `played: true` in `manifest["podcasts"]` AND
     all entries in `manifest["played_history"]` into `ledger.played_episodes`.
   - Normalize relative paths consistently as `<Show>/<filename>` (forward slashes,
     case-normalized for comparison).
2. Per-Podcast Grouping & Ordering:
   - Scan `<podcasts_dir>/<Show>/<episode>` on the server (ignoring dotfiles, `.part`, `.tmp`).
   - Sort each `<Show>`'s episodes from newest to oldest using episode date in filename
     (`YYYYMMDD` / `YYYY-MM-DD`) if present, with fallback to file `mtime`, and
     `filename` descending as tie-breaker.
   - The first `keep_recent` (default 3) episodes of each `<Show>` form `top_recent`.
3. Target Set (`desired_on_device`):
   - An episode on the server belongs to `desired_on_device` iff:
     - It is in `top_recent` (top 3 most recent of that podcast), OR
     - It has NOT been played (`rel_path not in played_set`).
4. Diff Calculation:
   - `to_upload`: Episodes in `desired_on_device` that are NOT currently on the MPS3
     (or whose size on the MPS3 differs from the server file size).
   - `to_delete`: Episodes currently on the MPS3 that are played (`played == true` on MPS3
     or in `played_set`) AND are NOT in `top_recent` of their `<Show>`.
   - Unplayed episodes on the MPS3 are NEVER deleted even if removed from the server.
5. Space Safety:
   - Respects a 32 MB safety reserve on the SD card (`safety_reserve_bytes`), reclaiming
     space from `to_delete` first only when the SD card is nearly full.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

from .ledger import SyncLedger, normalize_key, normalize_rel_path


AUDIO_EXTENSIONS: Set[str] = {
    ".mp3",
    ".m4a",
    ".aac",
    ".ogg",
    ".opus",
    ".flac",
    ".wav",
}

IGNORED_SUFFIXES: Tuple[str, ...] = (
    ".part",
    ".tmp",
    ".download",
    ".lock",
    ".temp",
    ".crdownload",
)

DEFAULT_SAFETY_RESERVE_BYTES = 32 * 1024 * 1024  # 32 MB

# Matches YYYY-MM-DD or YYYYMMDD not embedded inside longer digit sequences
_DATE_HYPHEN_RE = re.compile(r"(?<!\d)((?:19|20)\d{2})-(0[1-9]|1[0-2])-(0[1-9]|[12]\d|3[01])(?!\d)")
_DATE_COMPACT_RE = re.compile(r"(?<!\d)((?:19|20)\d{2})(0[1-9]|1[0-2])(0[1-9]|[12]\d|3[01])(?!\d)")


def extract_episode_date(filename: str) -> Optional[Tuple[int, int, int]]:
    """
    Extracts `(year, month, day)` from `filename` if a valid `YYYY-MM-DD` or `YYYYMMDD`
    date is present. Returns `None` if no valid date is found.
    """
    for pattern in (_DATE_HYPHEN_RE, _DATE_COMPACT_RE):
        for match in pattern.finditer(filename):
            year, month, day = int(match.group(1)), int(match.group(2)), int(match.group(3))
            try:
                datetime(year, month, day)
                return (year, month, day)
            except ValueError:
                continue
    return None


@dataclass(frozen=True)
class ServerEpisode:
    """Represents a podcast episode file available on the server."""

    show: str
    filename: str
    rel_path: str
    local_path: Path
    size: int
    mtime: float
    episode_date: Optional[Tuple[int, int, int]] = None

    @property
    def norm_key(self) -> str:
        return normalize_key(self.rel_path)

    def sort_key(self) -> Tuple[float, float, str]:
        """
        Sort key for ordering episodes newest-to-oldest (`reverse=True`):
        1. Primary timestamp: UTC timestamp of `episode_date` (`YYYY-MM-DD` / `YYYYMMDD`)
           if present in filename, else file `mtime`.
        2. Secondary tie-breaker: file `mtime`.
        3. Tertiary tie-breaker: `filename` (descending when `reverse=True`).
        """
        if self.episode_date is not None:
            y, m, d = self.episode_date
            primary_ts = datetime(y, m, d, 12, 0, 0, tzinfo=timezone.utc).timestamp()
        else:
            primary_ts = float(self.mtime)
        return (primary_ts, float(self.mtime), self.filename)


@dataclass(frozen=True)
class DeviceEpisode:
    """Represents a podcast episode currently stored on the MPS3 SD card."""

    show: str
    filename: str
    rel_path: str
    device_path: str
    size: int
    played: bool = False
    position_pct: float = 0.0

    @property
    def norm_key(self) -> str:
        return normalize_key(self.rel_path)


@dataclass
class SyncPlan:
    """Deterministic result of planning a sync session between Server and MPS3."""

    top_recent_by_show: Dict[str, List[ServerEpisode]] = field(default_factory=dict)
    desired_on_device: List[ServerEpisode] = field(default_factory=list)
    to_upload: List[ServerEpisode] = field(default_factory=list)
    to_delete: List[DeviceEpisode] = field(default_factory=list)
    skipped_for_space: List[ServerEpisode] = field(default_factory=list)
    delete_before_upload: bool = False
    free_bytes_before: Optional[int] = None
    reclaimable_bytes: int = 0
    upload_bytes: int = 0
    safety_reserve_bytes: int = DEFAULT_SAFETY_RESERVE_BYTES


def _is_ignored_file(name: str) -> bool:
    """Returns True if a file or directory name is a dotfile or temporary/partial file."""
    if not name or name.startswith("."):
        return True
    lower = name.lower()
    if any(lower.endswith(suffix) for suffix in IGNORED_SUFFIXES):
        return True
    return False


def scan_server_episodes(
    podcasts_dir: Path | str,
    allowed_extensions: Optional[Set[str]] = None,
) -> Dict[str, List[ServerEpisode]]:
    """
    Scans `<podcasts_dir>/<Show>/<episode>` on the server, ignoring dotfiles,
    `.staging`, `.part`, `.tmp`, `.download`, and empty (0-byte) files.

    Also seamlessly supports the Radxa layout where `<podcasts_dir>/Recentes/<Show>/<episode>`
    (and optionally `<podcasts_dir>/Arquivos/<Show>/<episode>`) exist, normalizing every
    episode's `rel_path` as `<Show>/<filename>`.

    Returns a dictionary mapping `<Show>` -> list of `ServerEpisode` sorted newest to oldest.
    """
    base = Path(podcasts_dir)
    exts = allowed_extensions if allowed_extensions is not None else AUDIO_EXTENSIONS
    by_show: Dict[str, Dict[str, ServerEpisode]] = {}

    if not base.is_dir():
        return {}

    # Detect whether `base` uses the Radxa `Recentes/` container layout
    # or direct `<podcasts_dir>/<Show>/<episode>` layout.
    recentes_dir = base / "Recentes"
    scan_roots = [recentes_dir] if recentes_dir.is_dir() else [base]

    for scan_root in scan_roots:
        try:
            show_entries = sorted(scan_root.iterdir(), key=lambda p: p.name)
        except OSError:
            continue

        for show_dir in show_entries:
            if not show_dir.is_dir():
                continue
            if _is_ignored_file(show_dir.name):
                continue
            if show_dir.name.lower() in ("mps3_server", "arquivados"):
                continue

            show_name = show_dir.name
            show_map = by_show.setdefault(show_name, {})

            try:
                file_entries = sorted(show_dir.iterdir(), key=lambda p: p.name)
            except OSError:
                continue

            for ep_file in file_entries:
                if not ep_file.is_file():
                    continue
                if _is_ignored_file(ep_file.name):
                    continue
                if exts and ep_file.suffix.lower() not in exts:
                    continue

                try:
                    st = ep_file.stat()
                except OSError:
                    continue
                if st.st_size <= 0:
                    continue

                rel_path = normalize_rel_path(f"{show_name}/{ep_file.name}")
                norm_k = rel_path.casefold()
                # If already found in `Recentes`, do not overwrite with `Arquivos` copy
                if norm_k in show_map:
                    continue

                ep_date = extract_episode_date(ep_file.name)
                show_map[norm_k] = ServerEpisode(
                    show=show_name,
                    filename=ep_file.name,
                    rel_path=rel_path,
                    local_path=ep_file,
                    size=st.st_size,
                    mtime=st.st_mtime,
                    episode_date=ep_date,
                )

    sorted_by_show: Dict[str, List[ServerEpisode]] = {}
    for show_name, eps_map in by_show.items():
        if not eps_map:
            continue
        eps_list = list(eps_map.values())
        eps_list.sort(key=lambda e: e.sort_key(), reverse=True)
        sorted_by_show[show_name] = eps_list

    return sorted_by_show


def parse_manifest_episodes(manifest: Dict[str, Any]) -> Dict[str, DeviceEpisode]:
    """
    Extracts the map of `norm_key -> DeviceEpisode` currently present on the MPS3
    from `manifest["podcasts"]`.
    """
    device_map: Dict[str, DeviceEpisode] = {}
    if not isinstance(manifest, dict):
        return device_map

    raw_list = manifest.get("podcasts", [])
    if not isinstance(raw_list, list):
        return device_map

    for item in raw_list:
        if not isinstance(item, dict):
            continue

        raw_path = (
            item.get("rel_path")
            or item.get("path")
            or (
                f"{item['program']}/{item['filename']}"
                if item.get("program") and item.get("filename")
                else ""
            )
        )
        norm_rel = normalize_rel_path(raw_path)
        if not norm_rel or "/" not in norm_rel:
            continue

        parts = norm_rel.split("/", 1)
        show_name, filename = parts[0], parts[1]
        if _is_ignored_file(show_name) or _is_ignored_file(filename):
            continue

        size_val = item.get("size", item.get("size_bytes", 0))
        try:
            size = int(size_val)
        except (TypeError, ValueError):
            size = 0

        played = bool(item.get("played", False))
        try:
            pos_pct = float(item.get("position_pct", 0.0))
        except (TypeError, ValueError):
            pos_pct = 0.0

        device_path = item.get("path") or f"/Podcasts/{norm_rel}"
        if not str(device_path).startswith("/"):
            device_path = f"/Podcasts/{norm_rel}"

        dev_ep = DeviceEpisode(
            show=show_name,
            filename=filename,
            rel_path=norm_rel,
            device_path=str(device_path),
            size=size,
            played=played,
            position_pct=pos_pct,
        )
        device_map[dev_ep.norm_key] = dev_ep

    return device_map


class SyncPlanner:
    """
    Stateless/deterministic planner that reconciles the MPS3 manifest with the
    server's `SyncLedger` and computes the exact `SyncPlan`.
    """

    def __init__(
        self,
        keep_recent: int = 3,
        safety_reserve_bytes: int = DEFAULT_SAFETY_RESERVE_BYTES,
    ):
        self.keep_recent = max(0, int(keep_recent))
        self.safety_reserve_bytes = max(0, int(safety_reserve_bytes))

    def create_plan(
        self,
        podcasts_dir: Path | str,
        manifest: Dict[str, Any],
        ledger: SyncLedger,
        only_show: Optional[str] = None,
    ) -> SyncPlan:
        """
        Executes the 5-step reconciliation and planning algorithm:
        1. Reconciles `manifest["podcasts"]` (`played: true`) and `manifest["played_history"]`
           into `ledger.played_episodes` and persists the updated ledger.
        2. Scans and sorts server episodes per show from newest to oldest, picking
           the first `keep_recent` (default 3) as `top_recent`.
        3. Builds `desired_on_device` = episodes in `top_recent` OR not in `played_set`.
        4. Computes `to_upload` (missing on device or size mismatch) and `to_delete`
           (on device AND played AND not in `top_recent` of its show).
        5. Applies SD card free-space safety check (`safety_reserve_bytes` = 32 MB).
        """
        # Step 1: Reconcile Heard State into Ledger
        ledger.reconcile_manifest(manifest)
        if ledger.state_file:
            ledger.save()

        played_keys: Set[str] = ledger.get_played_keys()

        # Step 2: Scan and group server episodes per show (sorted newest -> oldest)
        by_show = scan_server_episodes(podcasts_dir)
        if only_show:
            target_show_lower = only_show.strip().casefold()
            by_show = {
                k: v for k, v in by_show.items() if k.casefold() == target_show_lower
            }

        top_recent_by_show: Dict[str, List[ServerEpisode]] = {}
        top_recent_keys: Set[str] = set()
        desired_on_device: List[ServerEpisode] = []

        # Step 3: Target Set (What MUST be on the MPS3)
        for show_name in sorted(by_show.keys(), key=lambda s: s.casefold()):
            episodes = by_show[show_name]
            top_recent = episodes[: self.keep_recent]
            top_recent_by_show[show_name] = top_recent

            show_top_keys = {ep.norm_key for ep in top_recent}
            top_recent_keys.update(show_top_keys)

            for ep in episodes:
                is_in_top_recent = ep.norm_key in show_top_keys
                is_unplayed = ep.norm_key not in played_keys
                if is_in_top_recent or is_unplayed:
                    desired_on_device.append(ep)

        # Parse episodes currently on the MPS3
        device_episodes_map = parse_manifest_episodes(manifest)
        if only_show:
            target_show_lower = only_show.strip().casefold()
            device_episodes_map = {
                k: v
                for k, v in device_episodes_map.items()
                if v.show.casefold() == target_show_lower
            }

        # Step 4: Diff Calculation
        # 4a. Uploads: desired episodes missing on MPS3 OR whose size on MPS3 mismatches server size
        candidate_uploads: List[ServerEpisode] = []
        for server_ep in desired_on_device:
            dev_ep = device_episodes_map.get(server_ep.norm_key)
            if dev_ep is None or dev_ep.size != server_ep.size:
                candidate_uploads.append(server_ep)

        # Sort uploads so top_recent & newest episodes across shows are uploaded first
        candidate_uploads.sort(
            key=lambda ep: (
                1 if ep.norm_key in top_recent_keys else 0,
                ep.sort_key(),
            ),
            reverse=True,
        )

        # 4b. Deletions: episodes currently on MPS3 that are PLAYED and NOT in top_recent of their show
        # Never delete an unplayed episode from the MPS3 even if removed from the server!
        to_delete: List[DeviceEpisode] = []
        for dev_key, dev_ep in sorted(device_episodes_map.items(), key=lambda kv: kv[0]):
            is_played = dev_ep.played or (dev_key in played_keys)
            is_in_top_recent = dev_key in top_recent_keys
            if is_played and not is_in_top_recent:
                to_delete.append(dev_ep)

        # Step 5: Execution Order & Space Safety
        reclaimable_bytes = sum(ep.size for ep in to_delete)
        raw_free = manifest.get("free_bytes", manifest.get("free")) if isinstance(manifest, dict) else None
        free_bytes: Optional[int] = None
        if raw_free is not None:
            try:
                free_bytes = int(raw_free)
            except (TypeError, ValueError):
                free_bytes = None

        to_upload: List[ServerEpisode] = []
        skipped_for_space: List[ServerEpisode] = []
        delete_before_upload = False

        if free_bytes is None:
            # Manifest did not report free space: schedule all candidate uploads
            to_upload = list(candidate_uploads)
        else:
            # Account for bytes replaced when re-uploading a corrupted/partial file already on device
            total_needed = 0
            for ep in candidate_uploads:
                existing = device_episodes_map.get(ep.norm_key)
                existing_sz = existing.size if existing else 0
                total_needed += max(0, ep.size - existing_sz)

            usable_without_delete = max(0, free_bytes - self.safety_reserve_bytes)
            usable_with_delete = max(0, free_bytes + reclaimable_bytes - self.safety_reserve_bytes)

            if total_needed > usable_without_delete and reclaimable_bytes > 0:
                delete_before_upload = True
                budget = usable_with_delete
            else:
                budget = usable_without_delete

            used_budget = 0
            for ep in candidate_uploads:
                existing = device_episodes_map.get(ep.norm_key)
                net_bytes = max(0, ep.size - (existing.size if existing else 0))
                if used_budget + net_bytes <= budget:
                    to_upload.append(ep)
                    used_budget += net_bytes
                else:
                    skipped_for_space.append(ep)

        upload_bytes = sum(ep.size for ep in to_upload)

        return SyncPlan(
            top_recent_by_show=top_recent_by_show,
            desired_on_device=desired_on_device,
            to_upload=to_upload,
            to_delete=to_delete,
            skipped_for_space=skipped_for_space,
            delete_before_upload=delete_before_upload,
            free_bytes_before=free_bytes,
            reclaimable_bytes=reclaimable_bytes,
            upload_bytes=upload_bytes,
            safety_reserve_bytes=self.safety_reserve_bytes,
        )
