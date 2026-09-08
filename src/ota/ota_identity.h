#pragma once

#include <stdint.h>

// ── HexHound - Firmware Identity ────────────────────────────────
//
// What this build IS, in the two terms an update has to reason about: which
// hardware it runs on, and which version it is.
//
// This lives here rather than in config.h on purpose. config.h is included by
// most of the tree, so a version bump there rebuilds everything and collides
// with any other branch touching that file. Nothing outside the OTA path needs
// a firmware version today. If that changes, move both constants to config.h
// and include this header from there; the names are chosen to survive the move.

// ── Version ───────────────────────────────────────────────────────────────
//
// Packed major<<16 | minor<<8 | patch, which is what travels in the signed
// update header. Bump this in the same commit as the release, never later.
#define HEXHOUND_FW_VERSION_MAJOR  0
#define HEXHOUND_FW_VERSION_MINOR  4
#define HEXHOUND_FW_VERSION_PATCH  4

#define HEXHOUND_FW_VERSION  ( ((uint32_t)HEXHOUND_FW_VERSION_MAJOR << 16) | \
                               ((uint32_t)HEXHOUND_FW_VERSION_MINOR << 8)  | \
                                (uint32_t)HEXHOUND_FW_VERSION_PATCH )

// ── Board identity ────────────────────────────────────────────────────────
//
// The string an update image must name to be installable here. It identifies
// HARDWARE, not a build variant: the HID build and the vendor-TFT build of the
// T-Dongle S3 share an id because they run on the same board and either one
// boots on it. Choosing between variants is the owner's business, not a safety
// property. Choosing between BOARDS is a safety property.
//
// Why this exists at all, when the ESP32 image header already carries a chip
// id: five of the seven targets in this repo are ESP32-S3. A chip-family check
// passes a T-Dongle S3 image onto a Waveshare 1.47B, where the panel pins,
// the PSRAM mode and the backlight GPIO are all different. That is a dead
// screen on a device with no serial console and no recovery UI, in someone's
// hand, permanently. The board id is what makes that refusable, and it is
// inside the signed region so it cannot be edited in transit.
//
// These strings deliberately match the platformio env names, so the thing a
// developer types to build is the thing the signing script stamps in. Do not
// derive this from HEXHOUND_BOARD_NAME: that is a human-facing display string
// and it is allowed to be reworded, which would silently invalidate every
// image ever signed for that board.
#if defined(HEXHOUND_BOARD_LILYGO_T_DONGLE_C5)
#define HEXHOUND_OTA_BOARD_ID  "lilygo-t-dongle-c5"
#elif defined(HEXHOUND_BOARD_LILYGO_T_DISPLAY_S3)
#define HEXHOUND_OTA_BOARD_ID  "lilygo-t-display-s3"
#elif defined(HEXHOUND_BOARD_WAVESHARE_147B)
#define HEXHOUND_OTA_BOARD_ID  "waveshare-esp32-s3-lcd-147b"
#elif defined(HEXHOUND_BOARD_WAVESHARE_TOUCH_147)
#define HEXHOUND_OTA_BOARD_ID  "waveshare-esp32-s3-touch-lcd-147"
#elif defined(HEXHOUND_BOARD_WAVESHARE_LCD_128)
#define HEXHOUND_OTA_BOARD_ID  "waveshare-esp32-s3-lcd-128"
#else
#define HEXHOUND_OTA_BOARD_ID  "lilygo-t-dongle-s3"
#endif

// ── Slot size ─────────────────────────────────────────────────────────────
//
// app0 and app1 from default_16MB.csv, which every board in boards/ uses:
//
//     nvs       0x009000  0x005000
//     otadata   0x00E000  0x002000
//     app0      0x010000  0x640000   <- 6,553,600 bytes
//     app1      0x650000  0x640000   <- 6,553,600 bytes
//     spiffs    0xC90000  0x360000   <- the pet save lives here
//     coredump  0xFF0000  0x010000
//
// Two OTA slots already exist and are already the same size, so OTA needs no
// repartition and no migration. That is the single luckiest fact about this
// feature and it is why a giveaway device can be updated at all.
//
// This constant is a COMPILE-TIME UPPER BOUND used by the portable code and
// the tests. On hardware the real bound is the actual partition size read from
// the partition table, which is authoritative and is what the device enforces;
// this value only has to be right for the tests to be meaningful.
#define HEXHOUND_OTA_SLOT_BYTES  ((uint32_t)0x640000)

// Largest single chunk the device will accept in one write. Bounded so a host
// cannot make the device allocate or stack-copy something unreasonable, and
// small enough that a chunk buffer is comfortable in 250 KB of RAM with no
// PSRAM. Also the read-back granularity when the image is hashed off flash.
#define HEXHOUND_OTA_CHUNK_MAX  4096
