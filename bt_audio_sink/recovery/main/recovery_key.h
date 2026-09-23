#pragma once
#include <stdint.h>
#include <stddef.h>

// ============================================================
// Encryption Configuration (OTA Recovery)
// ============================================================
// To secure your OTA updates, generate a random 32-byte AES-256 key:
//   python tools/encrypt_firmware.py --generate-key
//
// Copy the generated C++ array here and the Python bytes into your
// local signing environment or ota_key.bin.
// DO NOT COMMIT YOUR REAL PRODUCTION KEY TO PUBLIC REPOSITORIES.
// ============================================================

#ifndef RECOVERY_AES_KEY_CONFIGURED
#define RECOVERY_AES_KEY_CONFIGURED 1

static const uint8_t AES_KEY[32] = {
    // Default dummy development key. Replace before deploying!
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
};

static const size_t AES_KEY_SIZE = 32;
static const size_t AES_BLOCK_SIZE = 16;

#endif
