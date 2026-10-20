"""
Cross-platform configuration management for `mps3_sync`.

Resolves directories using `platformdirs` (with standard-library fallback),
reads `.env` files and environment variables, and merges CLI overrides without
any hardcoded OS-specific paths.
"""

from __future__ import annotations

import os
import sys
import secrets
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Optional

try:
    from platformdirs import user_config_dir, user_data_dir, user_cache_dir
except ImportError:
    def user_config_dir(appname: str = "mps3-sync", appauthor: bool = False) -> str:
        if sys.platform.startswith("win"):
            base = os.environ.get("APPDATA") or str(Path.home() / "AppData" / "Roaming")
            return str(Path(base) / appname)
        elif sys.platform == "darwin":
            return str(Path.home() / "Library" / "Application Support" / appname)
        else:
            base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
            return str(Path(base) / appname)

    def user_data_dir(appname: str = "mps3-sync", appauthor: bool = False) -> str:
        if sys.platform.startswith("win"):
            base = os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA") or str(Path.home() / "AppData" / "Local")
            return str(Path(base) / appname)
        elif sys.platform == "darwin":
            return str(Path.home() / "Library" / "Application Support" / appname)
        else:
            base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
            return str(Path(base) / appname)

    def user_cache_dir(appname: str = "mps3-sync", appauthor: bool = False) -> str:
        if sys.platform.startswith("win"):
            base = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local")
            return str(Path(base) / appname / "Cache")
        elif sys.platform == "darwin":
            return str(Path.home() / "Library" / "Caches" / appname)
        else:
            base = os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
            return str(Path(base) / appname)


APP_NAME = "mps3-sync"
DEFAULT_TARGET_MAC = "28:84:85:52:35:84"
DEFAULT_AP_MAC = "28:84:85:52:35:85"
DEFAULT_SAFETY_RESERVE_BYTES = 32 * 1024 * 1024  # 32 MB safety reserve on SD card


def generate_api_key(num_bytes: int = 32) -> str:
    """Generates a cryptographically random pre-shared API key in hexadecimal."""
    return secrets.token_hex(num_bytes)


def parse_dotenv(dotenv_path: Path) -> Dict[str, str]:
    """Parses a simple KEY=VALUE .env file without external dependencies."""
    values: Dict[str, str] = {}
    if not dotenv_path.is_file():
        return values
    try:
        content = dotenv_path.read_text(encoding="utf-8")
        for raw_line in content.splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("export "):
                line = line[len("export "):].strip()
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            k = k.strip()
            v = v.strip()
            if len(v) >= 2 and ((v[0] == v[-1] == '"') or (v[0] == v[-1] == "'")):
                v = v[1:-1]
            values[k] = v
    except Exception:
        pass
    return values


def _default_podcasts_dir() -> Path:
    """Returns a sensible cross-platform default Podcasts directory."""
    return Path.home() / "Podcasts"


@dataclass
class SyncConfig:
    """Cross-platform configuration for the MPS3 Podcast Sync engine."""

    podcasts_dir: Path = field(default_factory=_default_podcasts_dir)
    state_file: Path = field(
        default_factory=lambda: Path(user_data_dir(APP_NAME)) / "state.json"
    )
    cache_dir: Path = field(
        default_factory=lambda: Path(user_cache_dir(APP_NAME))
    )
    config_dir: Path = field(
        default_factory=lambda: Path(user_config_dir(APP_NAME))
    )
    target_host: Optional[str] = None
    target_mac: str = DEFAULT_TARGET_MAC
    port: int = 80
    api_key: Optional[str] = None
    keep_recent: int = 3
    safety_reserve_bytes: int = DEFAULT_SAFETY_RESERVE_BYTES
    max_session_bytes: int = 0  # 0 = unlimited
    min_battery_pct: int = 15
    max_retries: int = 3
    timeout_sec: float = 30.0
    daemon_interval_sec: int = 1800  # 30 minutes default

    @classmethod
    def load(
        cls,
        env_file: Optional[Path] = None,
        podcasts_dir: Optional[Path | str] = None,
        state_file: Optional[Path | str] = None,
        target_host: Optional[str] = None,
        target_mac: Optional[str] = None,
        port: Optional[int] = None,
        api_key: Optional[str] = None,
        keep_recent: Optional[int] = None,
        safety_reserve_bytes: Optional[int] = None,
        daemon_interval_sec: Optional[int] = None,
    ) -> "SyncConfig":
        """
        Loads configuration merging:
        1. OS defaults via platformdirs
        2. .env file in config_dir or working directory (or explicit `env_file`)
        3. Process environment variables (`MPS3_*`)
        4. Explicit function/CLI overrides
        """
        cfg_dir = Path(user_config_dir(APP_NAME))
        data_dir = Path(user_data_dir(APP_NAME))
        cache_dir = Path(user_cache_dir(APP_NAME))

        dotenv_candidates = []
        if env_file:
            dotenv_candidates.append(Path(env_file))
        dotenv_candidates.append(Path.cwd() / ".env")
        dotenv_candidates.append(cfg_dir / ".env")
        dotenv_candidates.append(cfg_dir / "env")

        merged_env: Dict[str, str] = {}
        for candidate in reversed(dotenv_candidates):
            if candidate.is_file():
                merged_env.update(parse_dotenv(candidate))

        # Process environment variables override .env files
        for k, v in os.environ.items():
            if k.startswith("MPS3_"):
                merged_env[k] = v

        resolved_podcasts = (
            Path(podcasts_dir)
            if podcasts_dir is not None
            else Path(merged_env["MPS3_PODCASTS_DIR"])
            if "MPS3_PODCASTS_DIR" in merged_env
            else _default_podcasts_dir()
        )

        resolved_state = (
            Path(state_file)
            if state_file is not None
            else Path(merged_env["MPS3_STATE_FILE"])
            if "MPS3_STATE_FILE" in merged_env
            else data_dir / "state.json"
        )

        resolved_host = (
            target_host
            if target_host is not None
            else merged_env.get("MPS3_TARGET_HOST") or merged_env.get("MPS3_HOST") or None
        )

        resolved_mac = (
            target_mac
            if target_mac is not None
            else merged_env.get("MPS3_TARGET_MAC") or merged_env.get("MPS3_MAC") or DEFAULT_TARGET_MAC
        )

        resolved_port = (
            int(port)
            if port is not None
            else int(merged_env.get("MPS3_PORT", "80"))
        )

        resolved_key = (
            api_key
            if api_key is not None
            else merged_env.get("MPS3_KEY") or merged_env.get("MPS3_API_KEY") or None
        )

        resolved_keep_recent = (
            int(keep_recent)
            if keep_recent is not None
            else int(merged_env.get("MPS3_KEEP_RECENT", "3"))
        )

        resolved_reserve = (
            int(safety_reserve_bytes)
            if safety_reserve_bytes is not None
            else int(merged_env.get("MPS3_SAFETY_RESERVE_BYTES", str(DEFAULT_SAFETY_RESERVE_BYTES)))
        )

        resolved_interval = (
            int(daemon_interval_sec)
            if daemon_interval_sec is not None
            else int(merged_env.get("MPS3_INTERVAL_SEC", "1800"))
        )

        return cls(
            podcasts_dir=resolved_podcasts,
            state_file=resolved_state,
            cache_dir=cache_dir,
            config_dir=cfg_dir,
            target_host=resolved_host,
            target_mac=resolved_mac,
            port=resolved_port,
            api_key=resolved_key,
            keep_recent=resolved_keep_recent,
            safety_reserve_bytes=resolved_reserve,
            daemon_interval_sec=resolved_interval,
        )

    def masked_dict(self) -> Dict[str, object]:
        """Returns a dictionary safe for logging (masks `api_key`)."""
        return {
            "podcasts_dir": str(self.podcasts_dir),
            "state_file": str(self.state_file),
            "cache_dir": str(self.cache_dir),
            "config_dir": str(self.config_dir),
            "target_host": self.target_host,
            "target_mac": self.target_mac,
            "port": self.port,
            "api_key": "********" if self.api_key else None,
            "keep_recent": self.keep_recent,
            "safety_reserve_bytes": self.safety_reserve_bytes,
            "daemon_interval_sec": self.daemon_interval_sec,
        }

    def __repr__(self) -> str:
        masked = "********" if self.api_key else None
        return (
            f"SyncConfig(podcasts_dir={str(self.podcasts_dir)!r}, "
            f"state_file={str(self.state_file)!r}, "
            f"target_host={self.target_host!r}, target_mac={self.target_mac!r}, "
            f"port={self.port}, api_key={masked!r}, keep_recent={self.keep_recent})"
        )
