"""
Cross-platform service generator/installer and single-instance lock (`platform/service.py`).

Supports:
- Linux (ARM64 Radxa & x64): `systemd` user/system `.service` and `.timer` units
- macOS: `launchd` `~/Library/LaunchAgents/com.mps3.sync.plist`
- Windows: Windows Task Scheduler (`schtasks`) and Startup VBS/BAT wrapper
"""

from __future__ import annotations

import os
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from ..config import SyncConfig


class SingleInstanceLock:
    """
    Cross-platform non-blocking file lock (`fcntl.flock` on POSIX, `msvcrt.locking` on Windows)
    to prevent two concurrent sync cycles from colliding.
    """

    def __init__(self, lock_path: Path | str):
        self.lock_path = Path(lock_path)
        self._fh = None

    def acquire(self) -> bool:
        self.lock_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            self._fh = open(self.lock_path, "a+b")
            if sys.platform.startswith("win"):
                import msvcrt

                self._fh.seek(0)
                if self._fh.tell() == 0:
                    self._fh.write(b"0")
                    self._fh.flush()
                self._fh.seek(0)
                msvcrt.locking(self._fh.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(self._fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except Exception:
            if self._fh is not None:
                try:
                    self._fh.close()
                except Exception:
                    pass
                self._fh = None
            return False

    def release(self) -> None:
        if self._fh is None:
            return
        try:
            if sys.platform.startswith("win"):
                import msvcrt

                self._fh.seek(0)
                msvcrt.locking(self._fh.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl

                fcntl.flock(self._fh.fileno(), fcntl.LOCK_UN)
        except Exception:
            pass
        finally:
            try:
                self._fh.close()
            except Exception:
                pass
            self._fh = None

    def __enter__(self) -> "SingleInstanceLock":
        if not self.acquire():
            raise RuntimeError(f"Another mps3-sync instance is already running ({self.lock_path})")
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.release()


def _resolve_exec_command(config: SyncConfig) -> List[str]:
    """Builds the command line array to run `mps3-sync --once` on the current system."""
    if getattr(sys, "frozen", False):
        base_cmd = [sys.executable]
    else:
        base_cmd = [sys.executable, "-m", "mps3_sync.cli"]
    cmd = base_cmd + [
        "--once",
        "--podcasts-dir",
        str(config.podcasts_dir),
        "--state-file",
        str(config.state_file),
    ]
    if config.target_host:
        cmd.extend(["--target", config.target_host])
    if config.target_mac:
        cmd.extend(["--mac", config.target_mac])
    return cmd


def generate_systemd_units(
    config: SyncConfig,
    interval_min: int = 15,
    env_file: Optional[Path] = None,
) -> Tuple[str, str]:
    """Generates `(service_unit_content, timer_unit_content)` for Linux systemd."""
    cmd = _resolve_exec_command(config)
    exec_start = " ".join(shlex.quote(part) for part in cmd)
    env_line = f"EnvironmentFile=-{env_file}\n" if env_file else ""

    service_unit = (
        "[Unit]\n"
        "Description=MPS3 Podcast Push Sync Service\n"
        "After=network-online.target local-fs.target\n"
        "Wants=network-online.target\n\n"
        "[Service]\n"
        "Type=oneshot\n"
        f"WorkingDirectory={config.podcasts_dir}\n"
        f"{env_line}"
        f"ExecStart={exec_start}\n"
        "Nice=10\n"
    )

    timer_unit = (
        "[Unit]\n"
        "Description=Periodic MPS3 Podcast Push Sync Timer\n\n"
        "[Timer]\n"
        "OnBootSec=2min\n"
        f"OnUnitActiveSec={max(1, int(interval_min))}min\n"
        "Persistent=true\n\n"
        "[Install]\n"
        "WantedBy=timers.target\n"
    )
    return service_unit, timer_unit


def generate_launchd_plist(
    config: SyncConfig,
    interval_sec: int = 900,
    label: str = "com.mps3.sync",
) -> str:
    """Generates a macOS `launchd` `.plist` XML string."""
    cmd = _resolve_exec_command(config)
    args_xml = "\n".join(f"        <string>{part}</string>" for part in cmd)
    log_dir = config.cache_dir
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
        '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
        '<plist version="1.0">\n'
        "<dict>\n"
        "    <key>Label</key>\n"
        f"    <string>{label}</string>\n"
        "    <key>ProgramArguments</key>\n"
        "    <array>\n"
        f"{args_xml}\n"
        "    </array>\n"
        "    <key>StartInterval</key>\n"
        f"    <integer>{max(60, int(interval_sec))}</integer>\n"
        "    <key>RunAtLoad</key>\n"
        "    <true/>\n"
        "    <key>StandardOutPath</key>\n"
        f"    <string>{log_dir / 'mps3-sync.log'}</string>\n"
        "    <key>StandardErrorPath</key>\n"
        f"    <string>{log_dir / 'mps3-sync.err.log'}</string>\n"
        "</dict>\n"
        "</plist>\n"
    )


def generate_windows_schtasks_command(
    config: SyncConfig,
    interval_min: int = 15,
    task_name: str = "MPS3PodcastSync",
) -> List[str]:
    """Generates a Windows `schtasks` command list to register a periodic task."""
    cmd = _resolve_exec_command(config)
    tr_str = subprocess.list2cmdline(cmd)
    return [
        "schtasks",
        "/Create",
        "/F",
        "/SC",
        "MINUTE",
        "/MO",
        str(max(1, int(interval_min))),
        "/TN",
        task_name,
        "/TR",
        tr_str,
    ]


def install_service_for_current_os(
    config: SyncConfig,
    dry_run: bool = False,
) -> Dict[str, object]:
    """
    Writes/installs the background service for the current OS (Linux systemd user units,
    macOS LaunchAgents plist, or Windows Task Scheduler / generated unit files).
    """
    interval_min = max(1, config.daemon_interval_sec // 60)

    if sys.platform.startswith("linux"):
        service_txt, timer_txt = generate_systemd_units(
            config,
            interval_min=interval_min,
            env_file=config.config_dir / "env",
        )
        systemd_dir = Path.home() / ".config" / "systemd" / "user"
        svc_path = systemd_dir / "mps3-sync.service"
        tmr_path = systemd_dir / "mps3-sync.timer"
        if not dry_run:
            systemd_dir.mkdir(parents=True, exist_ok=True)
            svc_path.write_text(service_txt, encoding="utf-8")
            tmr_path.write_text(timer_txt, encoding="utf-8")
            subprocess.run(["systemctl", "--user", "daemon-reload"], check=False)
            subprocess.run(["systemctl", "--user", "enable", "--now", "mps3-sync.timer"], check=False)
        return {
            "platform": "linux-systemd",
            "service_path": str(svc_path),
            "timer_path": str(tmr_path),
            "service_content": service_txt,
            "timer_content": timer_txt,
        }

    elif sys.platform == "darwin":
        plist_txt = generate_launchd_plist(config, interval_sec=config.daemon_interval_sec)
        agents_dir = Path.home() / "Library" / "LaunchAgents"
        plist_path = agents_dir / "com.mps3.sync.plist"
        if not dry_run:
            agents_dir.mkdir(parents=True, exist_ok=True)
            config.cache_dir.mkdir(parents=True, exist_ok=True)
            plist_path.write_text(plist_txt, encoding="utf-8")
            subprocess.run(["launchctl", "unload", str(plist_path)], check=False)
            subprocess.run(["launchctl", "load", str(plist_path)], check=False)
        return {
            "platform": "macos-launchd",
            "plist_path": str(plist_path),
            "plist_content": plist_txt,
        }

    else:
        schtasks_cmd = generate_windows_schtasks_command(config, interval_min=interval_min)
        if not dry_run:
            subprocess.run(schtasks_cmd, check=False)
        return {
            "platform": "windows-schtasks",
            "command": schtasks_cmd,
        }
