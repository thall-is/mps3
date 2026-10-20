# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller specification file for building a standalone single-file executable
(`mps3-sync` / `mps3-sync.exe`) across Linux (ARM64/x64), macOS, and Windows.

Usage:
    pyinstaller tools/mps3_sync/packaging/mps3_sync.spec
"""

from pathlib import Path

SPEC_DIR = Path(SPECPATH).resolve()
PKG_ROOT = SPEC_DIR.parent

block_cipher = None

a = Analysis(
    [str(PKG_ROOT / "mps3_sync" / "cli.py")],
    pathex=[str(PKG_ROOT)],
    binaries=[],
    datas=[],
    hiddenimports=[
        "mps3_sync",
        "mps3_sync.config",
        "mps3_sync.ledger",
        "mps3_sync.discovery",
        "mps3_sync.client",
        "mps3_sync.planner",
        "mps3_sync.downloader",
        "mps3_sync.platform",
        "mps3_sync.platform.service",
        "requests",
        "zeroconf",
        "platformdirs",
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["tkinter", "matplotlib", "numpy", "scipy", "PIL"],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name="mps3-sync",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
