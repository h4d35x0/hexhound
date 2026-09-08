#include "ota_target.h"

#if !defined(SIMULATOR_BUILD) && !defined(UNIT_TEST)

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// ── HexHound - ESP32 OTA Flash Target ───────────────────────────
//
// The hardware end of OtaFlashTarget. Thin on purpose: every bound and every
// ordering rule lives in ota_session.cpp where a desktop test can reach it.
// What is here is the part that genuinely cannot be tested off-target, so it
// is kept as small as it can be.
//
// ── Why the pet save cannot be harmed by any of this ──────────────────────
//
// Every board in boards/ uses default_16MB.csv:
//
//     app0      0x010000  0x640000
//     app1      0x650000  0x640000
//     spiffs    0xC90000  0x360000   <- /pet_state_a.json, /pet_state_b.json
//
// Writes go exclusively through esp_ota_write() on a handle esp_ota_begin()
// bound to the passive APP partition. ESP-IDF clamps every write to that
// partition; there is no offset a caller can pass that escapes it, and OTA
// does not rewrite the partition table. The app slots and the filesystem are
// disjoint and not even adjacent.
//
// That is an argument, though, and arguments about data loss are worth
// checking at runtime rather than believing. selectTarget() below refuses to
// proceed if the partition it was handed overlaps the filesystem, so the claim
// is enforced on the device and not merely asserted in a comment.

namespace {

class EspOtaTarget : public OtaFlashTarget {
public:
    uint32_t slotBytes() const override {
        const esp_partition_t* p = target();
        return p != nullptr ? (uint32_t)p->size : 0;
    }

    bool begin(uint32_t imageLen) override {
        abort();

        const esp_partition_t* p = target();
        if (p == nullptr) {
            Serial.println("[OTA] No passive app partition. Refusing.");
            return false;
        }
        if (imageLen > p->size) {
            // Belt and braces: the session already refused this. Two
            // independent bounds on the one operation that can corrupt a
            // partition is not redundancy worth removing.
            Serial.println("[OTA] Image larger than slot. Refusing.");
            return false;
        }

        // esp_ota_begin erases the slot, which is why nothing may call it
        // before the signature has verified. The passive slot holds the
        // rollback image on a device that has been updated once already, so
        // erasing it speculatively would throw away the safety net for an
        // update that turns out to be forged.
        const esp_err_t err = esp_ota_begin(p, imageLen, &_handle);
        if (err != ESP_OK) {
            Serial.printf("[OTA] esp_ota_begin failed: %s\n", esp_err_to_name(err));
            _handle = 0;
            return false;
        }

        _part = p;
        _open = true;
        Serial.printf("[OTA] Writing %u bytes to %s (0x%06X, %u bytes)\n",
                      (unsigned)imageLen, p->label,
                      (unsigned)p->address, (unsigned)p->size);
        return true;
    }

    bool write(const uint8_t* data, size_t len) override {
        if (!_open || data == nullptr) return false;
        if (len > HEXHOUND_OTA_CHUNK_MAX) return false;
        const esp_err_t err = esp_ota_write(_handle, data, len);
        if (err != ESP_OK) {
            Serial.printf("[OTA] esp_ota_write failed: %s\n", esp_err_to_name(err));
            return false;
        }
        return true;
    }

    bool end() override {
        if (!_open) return false;
        // Also runs the SDK's structural validation of the image. A genuinely
        // independent check from the digest: this asks whether the bytes are a
        // well-formed application, the digest asks whether they are the
        // application that was signed.
        const esp_err_t err = esp_ota_end(_handle);
        _open   = false;
        _handle = 0;
        if (err != ESP_OK) {
            Serial.printf("[OTA] esp_ota_end failed: %s\n", esp_err_to_name(err));
            return false;
        }
        return true;
    }

    void abort() override {
        if (_open) {
            esp_ota_abort(_handle);
            _open   = false;
            _handle = 0;
        }
    }

    bool read(uint32_t offset, uint8_t* out, size_t len) const override {
        if (_part == nullptr || out == nullptr) return false;
        if ((uint64_t)offset + len > _part->size) return false;
        // Reads the durable partition contents, not a cache of what was handed
        // to write(). That distinction is the entire value of the digest check.
        return esp_partition_read(_part, offset, out, len) == ESP_OK;
    }

    bool setBootPartition() override {
        if (_part == nullptr) return false;
        const esp_err_t err = esp_ota_set_boot_partition(_part);
        if (err != ESP_OK) {
            Serial.printf("[OTA] esp_ota_set_boot_partition failed: %s\n",
                          esp_err_to_name(err));
            return false;
        }
        Serial.printf("[OTA] %s is now PENDING. It must confirm itself after "
                      "boot or it will be rolled back.\n", _part->label);
        return true;
    }

private:
    // Resolved once and cached, so slotBytes() during header checking and the
    // partition actually written cannot be two different partitions.
    const esp_partition_t* target() const {
        if (_part != nullptr) return _part;
        _part = selectTarget();
        return _part;
    }

    static const esp_partition_t* selectTarget() {
        const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
        if (p == nullptr) {
            // Single-app-partition layout. There is nowhere to write that is
            // not the running image, and overwriting the running image is the
            // one thing guaranteed to brick the device. Refuse.
            return nullptr;
        }

        const esp_partition_t* running = esp_ota_get_running_partition();
        if (running != nullptr && p->address == running->address) {
            // Should be impossible, and is exactly the impossible thing worth
            // checking, because being wrong here overwrites the firmware that
            // is currently executing.
            Serial.println("[OTA] Target resolved to the RUNNING partition. Refusing.");
            return nullptr;
        }

        // Enforce the save-safety claim rather than trusting it. If the target
        // slot overlaps the filesystem holding the pet, refuse the whole
        // feature on this device instead of taking a chance with someone's pet.
        const esp_partition_t* fs = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
        if (fs != nullptr) {
            const uint32_t aStart = p->address,  aEnd = p->address + p->size;
            const uint32_t bStart = fs->address, bEnd = fs->address + fs->size;
            if (aStart < bEnd && bStart < aEnd) {
                Serial.println("[OTA] Target slot overlaps the filesystem holding "
                               "the pet save. Refusing.");
                return nullptr;
            }
        }

        return p;
    }

    mutable const esp_partition_t* _part = nullptr;
    esp_ota_handle_t _handle = 0;
    bool _open = false;
};

EspOtaTarget s_target;

}  // namespace

OtaFlashTarget* espOtaTarget() { return &s_target; }

#endif  // !SIMULATOR_BUILD && !UNIT_TEST
