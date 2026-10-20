"""
MPS3 Podcast Sync (`mps3_sync`) — Standalone Cross-Platform Push Sync Package.

Supports Linux (ARM64 Radxa & x64), macOS, and Windows.
"""

__version__ = "2.0.0"
PROTOCOL_VERSION = 1

from .config import SyncConfig, generate_api_key
from .ledger import SyncLedger, normalize_rel_path, normalize_key
from .planner import (
    ServerEpisode,
    DeviceEpisode,
    SyncPlan,
    SyncPlanner,
    extract_episode_date,
    scan_server_episodes,
)
from .client import MPS3Client, MPS3SyncError, MPS3AuthError, MPS3HashMismatchError
from .discovery import discover_mps3, normalize_mac, matches_target_mac

__all__ = [
    "__version__",
    "PROTOCOL_VERSION",
    "SyncConfig",
    "generate_api_key",
    "SyncLedger",
    "normalize_rel_path",
    "normalize_key",
    "ServerEpisode",
    "DeviceEpisode",
    "SyncPlan",
    "SyncPlanner",
    "extract_episode_date",
    "scan_server_episodes",
    "MPS3Client",
    "MPS3SyncError",
    "MPS3AuthError",
    "MPS3HashMismatchError",
    "discover_mps3",
    "normalize_mac",
    "matches_target_mac",
]
