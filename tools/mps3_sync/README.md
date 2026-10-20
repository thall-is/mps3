# MPS3 Podcast Sync (`mps3_sync`)

Standalone, cross-platform Podcast Sync & Push Client for the **MPS3 Hi-Res Audio Player (ESP32-S3)**.
Designed for **Linux (ARM64 Radxa & x64)**, **macOS**, and **Windows**, and ready to be bundled into a single standalone executable with PyInstaller.

---

## Architecture & Retention Rules

1. **Inverted Push Architecture**:
   - The server discovers the MPS3 on the LAN via mDNS (`_mps3._tcp.local.` / `mps3.local`), ARP neighbor cache, or fast concurrent subnet probing with hardware MAC verification (`28:84:85:52:35:84`).
   - Uses a persistent HTTP `Keep-Alive` session (`requests.Session`) against the MPS3 transfer endpoints (`/api/sync/manifest`, `/api/sync/begin`, `/api/upload`, `/api/sync/verify`, `/api/rename`, `/api/delete`, `/api/sync/done`).
2. **Retention & Heard-Tracking Rules (`planner.py` + `ledger.py`)**:
   - **Keep Top 3 Recent per Show**: The 3 newest episodes of each podcast show on the server are always kept on the MPS3 (even if already played).
   - **Keep All Unplayed Episodes**: Any episode that has **not** been played is kept/uploaded even if it is 4th, 5th, or older.
   - **Never Re-Upload Played Episodes Outside Top 3**: Played episodes reported via `manifest["podcasts"]` (`played: true`) or `manifest["played_history"]` are permanently recorded in the server's atomic `state.json` (`ledger.played_episodes`). Once played and outside the top 3 of their show, they are deleted from the MPS3 and **never re-uploaded**.
   - **Never Delete Unplayed Episodes from MPS3**: Unplayed episodes on the MPS3 are never deleted, even if removed from the server.
3. **End-to-End Integrity & Space Safety**:
   - Every upload is streamed to `/Podcasts/.staging/<Show>/<episode>` with `X-Expected-SHA256`, verified on the SD card via `POST /api/sync/verify`, and atomically moved into `/Podcasts/<Show>/<episode>` via `POST /api/rename`.
   - Enforces a 32 MB SD card safety reserve, deleting old played episodes after new uploads succeed (or before uploads if the SD card is nearly full).

---

## Installation

```bash
pip install -e tools/mps3_sync
```

---

## CLI Usage

```bash
# Single sync cycle (discover MPS3, reconcile manifest, upload new, delete old played)
mps3-sync --once --podcasts-dir "/mnt/USBT/Podcasts"

# Dry-run plan inspection (no changes made to MPS3)
mps3-sync --dry-run --podcasts-dir "/mnt/USBT/Podcasts"

# Verify SHA-256 of local server episodes
mps3-sync --verify-only --podcasts-dir "/mnt/USBT/Podcasts"

# Inspect server library, ledger, and device status
mps3-sync --status --podcasts-dir "/mnt/USBT/Podcasts"

# Run as a continuous daemon polling every 15 minutes (900s)
mps3-sync --daemon --interval 900 --podcasts-dir "/mnt/USBT/Podcasts"

# Generate a 256-bit pre-shared API key (X-MPS3-Key)
mps3-sync --gen-key

# Install OS-native background service (systemd on Linux, launchd on macOS, Task Scheduler on Windows)
mps3-sync --install-service --podcasts-dir "/mnt/USBT/Podcasts"
```

---

## Configuration (Environment Variables / `.env`)

No paths are hardcoded. Configure via CLI flags, environment variables, or `.env`:

| Variable | Description | Default |
|---|---|---|
| `MPS3_PODCASTS_DIR` | Local Podcasts library root directory | `~/Podcasts` |
| `MPS3_STATE_FILE` | Path to persistent `state.json` ledger | OS user data dir via `platformdirs` |
| `MPS3_TARGET_HOST` | Optional explicit IP/host of MPS3 | Auto-discovered by MAC |
| `MPS3_TARGET_MAC` | Hardware MAC of MPS3 | `28:84:85:52:35:84` |
| `MPS3_PORT` | HTTP port on MPS3 | `80` |
| `MPS3_KEY` | Optional pre-shared `X-MPS3-Key` | `None` |
| `MPS3_KEEP_RECENT` | Number of recent episodes kept per show | `3` |

---

## Standalone Executable Build (PyInstaller)

```bash
pip install pyinstaller
pyinstaller tools/mps3_sync/packaging/mps3_sync.spec
```

The standalone binary will be created at `dist/mps3-sync` (Linux/macOS) or `dist/mps3-sync.exe` (Windows).
