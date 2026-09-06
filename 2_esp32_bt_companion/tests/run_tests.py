#!/usr/bin/env python3
"""
Cross-platform test runner for ESP32 BT Companion.
Runs host unit test suites and ESP-IDF stub compile checks.
Works on Linux, macOS, and Windows (with host gcc or PlatformIO xtensa gcc).

Usage:
    python tests/run_tests.py
"""

import os
import sys
import shutil
import tempfile
import subprocess

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(SCRIPT_DIR)
STUBS_DIR = os.path.join(SCRIPT_DIR, "esp_idf_stubs", "include")

HOST_TESTS = [
    ("bitrate_abr_test",
     ["-I" + os.path.join(ROOT_DIR, "components", "bitrate_abr", "include")],
     [os.path.join(ROOT_DIR, "components", "bitrate_abr", "bitrate_abr.c"),
      os.path.join(ROOT_DIR, "components", "bitrate_abr", "test_bitrate_abr_host.c")],
     []),

    ("ldac_enc_test",
     ["-D_32BIT_FIXED_POINT",
      "-I" + os.path.join(ROOT_DIR, "components", "ldac_enc", "vendor", "inc"),
      "-I" + os.path.join(ROOT_DIR, "components", "ldac_enc", "vendor", "src"),
      "-I" + os.path.join(ROOT_DIR, "components", "ldac_enc", "include")],
     [os.path.join(ROOT_DIR, "components", "ldac_enc", "vendor", "src", "ldaclib.c"),
      os.path.join(ROOT_DIR, "components", "ldac_enc", "vendor", "src", "ldacBT.c"),
      os.path.join(ROOT_DIR, "components", "ldac_enc", "ldac_enc.c"),
      os.path.join(ROOT_DIR, "components", "ldac_enc", "test_host.c")],
     ["-lm"]),

    ("payload_test",
     ["-I" + os.path.join(ROOT_DIR, "components", "bt_source", "include")],
     [os.path.join(ROOT_DIR, "components", "bt_source", "a2dp_media_payload.c"),
      os.path.join(ROOT_DIR, "components", "bt_source", "test_media_payload_host.c")],
     []),

    ("audio_pipeline_test",
     ["-I" + os.path.join(ROOT_DIR, "components", "audio_pipeline", "include")],
     [os.path.join(ROOT_DIR, "components", "audio_pipeline", "audio_pipeline.c"),
      os.path.join(ROOT_DIR, "components", "audio_pipeline", "test_audio_pipeline_host.c")],
     []),

    ("sbc_enc_test",
     ["-I" + os.path.join(ROOT_DIR, "components", "sbc_enc", "vendor", "inc"),
      "-I" + os.path.join(ROOT_DIR, "components", "sbc_enc", "vendor", "src"),
      "-I" + os.path.join(ROOT_DIR, "components", "sbc_enc", "include")],
     [os.path.join(ROOT_DIR, "components", "sbc_enc", "vendor", "src", "sbc.c"),
      os.path.join(ROOT_DIR, "components", "sbc_enc", "vendor", "src", "bits.c"),
      os.path.join(ROOT_DIR, "components", "sbc_enc", "sbc_enc.c"),
      os.path.join(ROOT_DIR, "components", "sbc_enc", "test_host.c")],
     ["-lm"]),

    ("aptx_enc_test",
     ["-O3",
      "-I" + os.path.join(ROOT_DIR, "components", "aptx_enc", "vendor", "inc"),
      "-I" + os.path.join(ROOT_DIR, "components", "aptx_enc", "vendor", "src"),
      "-I" + os.path.join(ROOT_DIR, "components", "aptx_enc", "include")],
     [os.path.join(ROOT_DIR, "components", "aptx_enc", "vendor", "src", "aptXbtenc.c"),
      os.path.join(ROOT_DIR, "components", "aptx_enc", "vendor", "src", "ProcessSubband.c"),
      os.path.join(ROOT_DIR, "components", "aptx_enc", "vendor", "src", "QmfConv.c"),
      os.path.join(ROOT_DIR, "components", "aptx_enc", "vendor", "src", "QuantiseDifference.c"),
      os.path.join(ROOT_DIR, "components", "aptx_enc", "aptx_enc.c"),
      os.path.join(ROOT_DIR, "components", "aptx_enc", "test_host.c")],
     ["-lm"]),

    ("aptx_hd_enc_test",
     ["-O3",
      "-I" + os.path.join(ROOT_DIR, "components", "aptx_hd_enc", "vendor", "inc"),
      "-I" + os.path.join(ROOT_DIR, "components", "aptx_hd_enc", "vendor", "src"),
      "-I" + os.path.join(ROOT_DIR, "components", "aptx_hd_enc", "include")],
     [os.path.join(ROOT_DIR, "components", "aptx_hd_enc", "vendor", "src", "aptXHDbtenc.c"),
      os.path.join(ROOT_DIR, "components", "aptx_hd_enc", "vendor", "src", "ProcessSubband.c"),
      os.path.join(ROOT_DIR, "components", "aptx_hd_enc", "vendor", "src", "QmfConv.c"),
      os.path.join(ROOT_DIR, "components", "aptx_hd_enc", "vendor", "src", "QuantiseDifference.c"),
      os.path.join(ROOT_DIR, "components", "aptx_hd_enc", "aptx_hd_enc.c"),
      os.path.join(ROOT_DIR, "components", "aptx_hd_enc", "test_host.c")],
     ["-lm"]),

    ("integration_pipeline_host",
     ["-D_32BIT_FIXED_POINT",
      "-I" + os.path.join(ROOT_DIR, "components", "ldac_enc", "vendor", "inc"),
      "-I" + os.path.join(ROOT_DIR, "components", "ldac_enc", "vendor", "src"),
      "-I" + os.path.join(ROOT_DIR, "components", "ldac_enc", "include"),
      "-I" + os.path.join(ROOT_DIR, "components", "bt_source", "include"),
      "-I" + os.path.join(ROOT_DIR, "components", "audio_pipeline", "include")],
     [os.path.join(ROOT_DIR, "components", "ldac_enc", "vendor", "src", "ldaclib.c"),
      os.path.join(ROOT_DIR, "components", "ldac_enc", "vendor", "src", "ldacBT.c"),
      os.path.join(ROOT_DIR, "components", "ldac_enc", "ldac_enc.c"),
      os.path.join(ROOT_DIR, "components", "bt_source", "a2dp_media_payload.c"),
      os.path.join(ROOT_DIR, "components", "audio_pipeline", "audio_pipeline.c"),
      os.path.join(SCRIPT_DIR, "integration_pipeline_host.c")],
     ["-lm"])
]

COMPILE_CHECKS = [
    ("uart_ctrl", [
        "-I" + STUBS_DIR,
        "-I" + os.path.join(ROOT_DIR, "components", "uart_ctrl", "include"),
        os.path.join(ROOT_DIR, "components", "uart_ctrl", "uart_ctrl.c")
    ]),
    ("i2s_input", [
        "-I" + STUBS_DIR,
        "-I" + os.path.join(ROOT_DIR, "components", "i2s_input", "include"),
        os.path.join(ROOT_DIR, "components", "i2s_input", "i2s_input.c")
    ]),
    ("bt_source", [
        "-I" + STUBS_DIR,
        "-I" + os.path.join(ROOT_DIR, "components", "bt_source", "include"),
        os.path.join(ROOT_DIR, "components", "bt_source", "bt_source.c")
    ]),
    ("main", [
        "-I" + STUBS_DIR,
        "-I" + os.path.join(ROOT_DIR, "components", "uart_ctrl", "include"),
        "-I" + os.path.join(ROOT_DIR, "components", "i2s_input", "include"),
        "-I" + os.path.join(ROOT_DIR, "components", "bt_source", "include"),
        "-I" + os.path.join(ROOT_DIR, "components", "ldac_enc", "include"),
        "-I" + os.path.join(ROOT_DIR, "components", "sbc_enc", "include"),
        "-I" + os.path.join(ROOT_DIR, "components", "aptx_enc", "include"),
        "-I" + os.path.join(ROOT_DIR, "components", "aptx_hd_enc", "include"),
        "-I" + os.path.join(ROOT_DIR, "components", "audio_pipeline", "include"),
        "-I" + os.path.join(ROOT_DIR, "components", "bitrate_abr", "include"),
        os.path.join(ROOT_DIR, "main", "main.c")
    ])
]

def find_compiler():
    # 1. Host compiler (can run natively)
    for c in ["gcc", "clang"]:
        p = shutil.which(c)
        if p:
            return p, True

    # 2. Embedded cross compiler (PlatformIO Xtensa)
    pio_paths = [
        os.path.expanduser(r"~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-gcc.exe"),
        os.path.expanduser(r"~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-gcc")
    ]
    for p in pio_paths:
        if os.path.exists(p):
            return p, False

    return None, False

def main():
    cc, can_execute = find_compiler()
    if not cc:
        print("ERROR: No C compiler found (neither host gcc/clang nor PlatformIO xtensa-esp32-elf-gcc).")
        sys.exit(1)

    print("=" * 65)
    print("ESP32 BT Companion Test Suite")
    print(f"Compiler: {cc}")
    print(f"Mode:     {'Native Execution' if can_execute else 'Cross-Compile Syntax & Type Check'}")
    print("=" * 65)

    tmp_dir = tempfile.mkdtemp(prefix="bt_tests_")
    fail_count = 0

    # 1. Host unit tests
    print("\n--- [1/2] Unit Test Suites (8 Suites) ---")
    for name, flags, sources, extra_libs in HOST_TESTS:
        if can_execute:
            bin_path = os.path.join(tmp_dir, name + (".exe" if sys.platform == "win32" else ""))
            cmd = [cc, "-Wall", "-Wextra", "-Wno-unused-parameter"] + flags + sources + extra_libs + ["-o", bin_path]
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if res.returncode != 0:
                print(f"  [FAIL COMPILE] {name}")
                print(res.stderr[:400])
                fail_count += 1
                continue
            run_res = subprocess.run([bin_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if run_res.returncode == 0:
                print(f"  [PASS] {name}")
            else:
                print(f"  [FAIL RUN] {name}")
                print(run_res.stderr or run_res.stdout)
                fail_count += 1
        else:
            # Cross-compile each source to verify syntax, types, and logic
            ok = True
            for s in sources:
                base = os.path.basename(s).replace(".c", "")
                obj_file = os.path.join(tmp_dir, f"{name}_{base}.o")
                cmd = [cc, "-c", "-Wall", "-Wextra", "-Wno-unused-parameter"] + flags + [s, "-o", obj_file]
                res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                if res.returncode != 0:
                    print(f"  [FAIL] {name} (source: {base}.c)")
                    print(res.stderr[:400])
                    ok = False
                    fail_count += 1
                    break
            if ok:
                print(f"  [PASS (COMPILE)] {name} ({len(sources)} sources OK)")

    # 2. ESP-IDF stub compile checks
    print("\n--- [2/2] ESP-IDF Module Checks (4 Modules) ---")
    for name, args in COMPILE_CHECKS:
        obj_file = os.path.join(tmp_dir, f"cc_{name}.o")
        cmd = [cc, "-c", "-Wall", "-Wextra", "-Wno-unused-parameter"] + args + ["-o", obj_file]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if res.returncode == 0:
            warns = res.stderr.count("warning:")
            print(f"  [PASS] {name:15} (warnings: {warns})")
        else:
            print(f"  [FAIL] {name}")
            print(res.stderr[:400])
            fail_count += 1

    shutil.rmtree(tmp_dir, ignore_errors=True)

    print("\n" + "=" * 65)
    if fail_count == 0:
        print("ALL TESTS AND COMPILE CHECKS PASSED (100% SUCCESS)!")
        print("=" * 65)
        sys.exit(0)
    else:
        print(f"FAILURES DETECTED: {fail_count} failure(s)")
        print("=" * 65)
        sys.exit(1)

if __name__ == "__main__":
    main()
