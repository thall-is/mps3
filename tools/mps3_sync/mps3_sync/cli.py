"""
Cross-platform CLI (`cli.py`) for the MPS3 Podcast Sync package (`mps3-sync`).

Supports:
- `--once`: Run a single discovery + reconciliation + sync push cycle
- `--daemon`: Run continuously at `--interval` seconds
- `--status`: Discover MPS3, inspect manifest & local library status
- `--verify-only`: Verify SHA-256 of episodes on the server and compare with device manifest
- `--dry-run`: Calculate and print the exact sync plan (`to_upload`, `to_delete`) without modifying device
- `--target`: Explicit MPS3 IP/hostname override
- `--podcasts-dir`: Local Podcasts library directory
- `--install-service`: Generate and install the OS background service/timer
- `--gen-key`: Generate a random 256-bit pre-shared `X-MPS3-Key`
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import List, Optional

from . import __version__
from .client import MPS3AuthError, MPS3Client, MPS3SyncError
from .config import SyncConfig, generate_api_key
from .discovery import discover_mps3, probe_mps3_host
from .ledger import SyncLedger
from .planner import SyncPlanner, scan_server_episodes
from .platform.service import SingleInstanceLock, install_service_for_current_os


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="mps3-sync",
        description="Standalone Cross-Platform Podcast Sync & Push Client for MPS3 (ESP32-S3)",
    )
    parser.add_argument("--version", action="version", version=f"mps3-sync {__version__}")

    # Mode flags
    mode_group = parser.add_argument_group("Execution Modes")
    mode_group.add_argument(
        "--once",
        action="store_true",
        help="Run a single discovery and sync push session, then exit (default if no mode specified)",
    )
    mode_group.add_argument(
        "--daemon",
        action="store_true",
        help="Run continuously in the background, polling every --interval seconds",
    )
    mode_group.add_argument(
        "--status",
        action="store_true",
        help="Display local library, ledger, and MPS3 device status",
    )
    mode_group.add_argument(
        "--verify-only",
        action="store_true",
        help="Compute/verify SHA-256 of server episodes and inspect sync diff without uploading",
    )
    mode_group.add_argument(
        "--dry-run",
        action="store_true",
        help="Calculate and display the sync plan (to_upload / to_delete) without modifying the MPS3",
    )
    mode_group.add_argument(
        "--install-service",
        action="store_true",
        help="Install background timer/service for the current OS (systemd / launchd / Task Scheduler)",
    )
    mode_group.add_argument(
        "--gen-key",
        action="store_true",
        help="Generate a new 256-bit pre-shared API key (X-MPS3-Key) and exit",
    )

    # Configuration flags
    cfg_group = parser.add_argument_group("Configuration & Filtering")
    cfg_group.add_argument(
        "--target",
        "--ip",
        dest="target_host",
        help="Target MPS3 IP address or hostname (skips mDNS/subnet scan)",
    )
    cfg_group.add_argument(
        "--mac",
        dest="target_mac",
        help="Target MPS3 hardware MAC address (default: 28:84:85:52:35:84)",
    )
    cfg_group.add_argument(
        "--port",
        type=int,
        help="Target MPS3 HTTP port (default: 80)",
    )
    cfg_group.add_argument(
        "--podcasts-dir",
        "--dir",
        dest="podcasts_dir",
        help="Path to the local Podcasts directory on this server",
    )
    cfg_group.add_argument(
        "--state-file",
        dest="state_file",
        help="Path to persistent ledger state.json file",
    )
    cfg_group.add_argument(
        "--env-file",
        dest="env_file",
        help="Optional path to .env configuration file",
    )
    cfg_group.add_argument(
        "--api-key",
        dest="api_key",
        help="Pre-shared X-MPS3-Key (prefer setting MPS3_KEY in .env)",
    )
    cfg_group.add_argument(
        "--keep-recent",
        type=int,
        help="Number of latest episodes per podcast show to always keep on device (default: 3)",
    )
    cfg_group.add_argument(
        "--only",
        dest="only_show",
        help="Sync only a specific podcast show name",
    )
    cfg_group.add_argument(
        "--forget",
        dest="forget_show",
        help="Forget played history for a specific show name in the ledger",
    )
    cfg_group.add_argument(
        "--interval",
        type=int,
        dest="interval_sec",
        help="Polling interval in seconds when running in --daemon mode (default: 1800)",
    )

    return parser


def _print_plan(plan, ledger: SyncLedger) -> None:
    print("=" * 68)
    print("  MPS3 SYNC PLAN SUMMARY")
    print("=" * 68)
    print(f"  Shows scanned:         {len(plan.top_recent_by_show)}")
    print(f"  Desired on MPS3:       {len(plan.desired_on_device)} episode(s)")
    print(f"  Played in ledger:      {len(ledger.played_episodes)} episode(s)")
    if plan.free_bytes_before is not None:
        print(f"  Device free space:     {plan.free_bytes_before / (1024 * 1024):.1f} MB")
    print(f"  Uploads scheduled:     {len(plan.to_upload)} ({plan.upload_bytes / (1024 * 1024):.2f} MB)")
    for ep in plan.to_upload:
        print(f"    [+] UPLOAD: {ep.rel_path} ({ep.size // 1024} KB)")
    print(f"  Deletions scheduled:   {len(plan.to_delete)} ({plan.reclaimable_bytes / (1024 * 1024):.2f} MB)")
    for dev_ep in plan.to_delete:
        print(f"    [-] DELETE: {dev_ep.rel_path} ({dev_ep.size // 1024} KB)")
    if plan.skipped_for_space:
        print(f"  Skipped (low space):   {len(plan.skipped_for_space)}")
        for ep in plan.skipped_for_space:
            print(f"    [!] SKIP:   {ep.rel_path} ({ep.size // 1024} KB)")
    print("=" * 68)


def run_single_cycle(
    config: SyncConfig,
    dry_run: bool = False,
    verify_only: bool = False,
    only_show: Optional[str] = None,
) -> int:
    """Runs one discovery, manifest reconciliation, planning, and execution cycle."""
    ledger = SyncLedger(config.state_file)
    planner = SyncPlanner(
        keep_recent=config.keep_recent,
        safety_reserve_bytes=config.safety_reserve_bytes,
    )

    if verify_only:
        by_show = scan_server_episodes(config.podcasts_dir)
        total_eps = 0
        for show_name, eps in sorted(by_show.items()):
            print(f"[{show_name}] ({len(eps)} episode(s))")
            for idx, ep in enumerate(eps, start=1):
                sha = ledger.get_file_sha256(ep.local_path, ep.rel_path)
                played_tag = "PLAYED" if ledger.is_played(ep.rel_path) else "UNPLAYED"
                top_tag = f"TOP-{idx}" if idx <= config.keep_recent else f"#{idx}"
                print(f"  {top_tag:6s} | {played_tag:8s} | {sha[:12]}... | {ep.filename}")
                total_eps += 1
        ledger.save()
        print(f"[*] Verified SHA-256 for {total_eps} server episode(s).")
        if not config.target_host:
            return 0

    host = discover_mps3(
        target_host=config.target_host,
        target_mac=config.target_mac,
        port=config.port,
        cache_dir=config.cache_dir,
    )

    if not host:
        if dry_run:
            print("[*] MPS3 not reachable on LAN; computing offline dry-run plan against empty device manifest.")
            plan = planner.create_plan(
                podcasts_dir=config.podcasts_dir,
                manifest={"podcasts": [], "played_history": []},
                ledger=ledger,
                only_show=only_show,
            )
            _print_plan(plan, ledger)
            return 0
        print("[*] MPS3 not detected on network. Exiting quietly until next cycle.")
        return 0

    print(f"[*] Connected to MPS3 at {host}:{config.port}")
    with MPS3Client(
        host=host,
        port=config.port,
        api_key=config.api_key,
        timeout=config.timeout_sec,
        max_retries=config.max_retries,
    ) as client:
        manifest = client.get_manifest()
        plan = planner.create_plan(
            podcasts_dir=config.podcasts_dir,
            manifest=manifest,
            ledger=ledger,
            only_show=only_show,
        )
        _print_plan(plan, ledger)

        if dry_run or verify_only:
            return 0

        if not plan.to_upload and not plan.to_delete:
            print("[OK] MPS3 is already 100% in sync.")
            return 0

        summary = client.execute_plan(
            plan=plan,
            ledger=ledger,
            dry_run=False,
            progress_cb=lambda msg: print(msg, flush=True),
        )
        avg_speed = (
            (summary.bytes_transferred / (1024.0 * 1024.0)) / summary.duration_sec
            if summary.duration_sec > 0 and summary.bytes_transferred > 0
            else 0.0
        )
        print(
            f"[OK] Sync finished in {summary.duration_sec:.1f}s "
            f"(uploaded={len(summary.uploaded)}, deleted={len(summary.deleted)}, "
            f"errors={len(summary.errors)}, avg={avg_speed:.2f} MB/s)"
        )
        return 1 if summary.errors else 0


def run_status(config: SyncConfig) -> int:
    ledger = SyncLedger(config.state_file)
    by_show = scan_server_episodes(config.podcasts_dir)
    total_files = sum(len(v) for v in by_show.values())

    status_report = {
        "config": config.masked_dict(),
        "server_library": {
            "podcasts_dir": str(config.podcasts_dir),
            "exists": config.podcasts_dir.is_dir(),
            "shows_count": len(by_show),
            "episodes_count": total_files,
            "shows": {k: [e.filename for e in v] for k, v in by_show.items()},
        },
        "ledger": {
            "state_file": str(config.state_file),
            "played_episodes_count": len(ledger.played_episodes),
            "cached_hashes_count": len(ledger.sha256_cache),
            "last_sync": ledger.sync_history[-1] if ledger.sync_history else None,
        },
        "device": None,
    }

    host = discover_mps3(
        target_host=config.target_host,
        target_mac=config.target_mac,
        port=config.port,
        cache_dir=config.cache_dir,
        use_subnet_scan=False,
    )
    if host:
        info = probe_mps3_host(host, port=config.port, target_mac=None, timeout=2.0)
        status_report["device"] = info or {"ip": host, "port": config.port}

    print(json.dumps(status_report, indent=2, ensure_ascii=False))
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    if args.gen_key:
        print(generate_api_key())
        return 0

    config = SyncConfig.load(
        env_file=Path(args.env_file) if args.env_file else None,
        podcasts_dir=args.podcasts_dir,
        state_file=args.state_file,
        target_host=args.target_host,
        target_mac=args.target_mac,
        port=args.port,
        api_key=args.api_key,
        keep_recent=args.keep_recent,
        daemon_interval_sec=args.interval_sec,
    )

    if args.forget_show:
        ledger = SyncLedger(config.state_file)
        removed = ledger.forget_show(args.forget_show)
        ledger.save()
        print(f"[*] Removed {removed} played episode record(s) for show '{args.forget_show}'.")
        return 0

    if args.install_service:
        res = install_service_for_current_os(config, dry_run=args.dry_run)
        print(json.dumps(res, indent=2, ensure_ascii=False))
        return 0

    if args.status:
        return run_status(config)

    lock_file = config.cache_dir / "mps3-sync.lock"
    lock = SingleInstanceLock(lock_file)
    if not lock.acquire():
        print(f"[!] Another mps3-sync instance is already active ({lock_file}). Exiting.")
        return 0

    try:
        if args.daemon:
            print(
                f"[*] Starting mps3-sync daemon (interval={config.daemon_interval_sec}s, "
                f"podcasts_dir={config.podcasts_dir})"
            )
            while True:
                try:
                    run_single_cycle(
                        config=config,
                        dry_run=args.dry_run,
                        verify_only=args.verify_only,
                        only_show=args.only_show,
                    )
                except KeyboardInterrupt:
                    raise
                except Exception as e:
                    print(f"[!] Sync cycle error: {e}", file=sys.stderr)
                time.sleep(max(5, config.daemon_interval_sec))
        else:
            return run_single_cycle(
                config=config,
                dry_run=args.dry_run,
                verify_only=args.verify_only,
                only_show=args.only_show,
            )
    except KeyboardInterrupt:
        print("\n[*] Interrupted by user.")
        return 130
    except MPS3AuthError as e:
        print(f"[!] Authentication Error: {e}", file=sys.stderr)
        return 2
    except MPS3SyncError as e:
        print(f"[!] Sync Error: {e}", file=sys.stderr)
        return 1
    finally:
        lock.release()


if __name__ == "__main__":
    sys.exit(main())
