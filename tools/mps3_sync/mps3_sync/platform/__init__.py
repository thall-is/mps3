"""
Platform-specific utilities (`mps3_sync.platform`).

Isolates OS-specific service generation/installation and single-instance file locking
for Linux (systemd), macOS (launchd), and Windows (Task Scheduler).
"""

from .service import (
    SingleInstanceLock,
    generate_launchd_plist,
    generate_systemd_units,
    generate_windows_schtasks_command,
    install_service_for_current_os,
)

__all__ = [
    "SingleInstanceLock",
    "generate_launchd_plist",
    "generate_systemd_units",
    "generate_windows_schtasks_command",
    "install_service_for_current_os",
]
