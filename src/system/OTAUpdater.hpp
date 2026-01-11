#pragma once
#include <memory>
#include <functional>
#include <string>
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"

// OTAUpdater manages firmware update writes to the OTA partition, validates
// the image, and reports progress. AppController orchestrates it; downloading
// firmware remains in NetworkManager.
class OTAUpdater {
public:
    OTAUpdater() = default;
    ~OTAUpdater() = default;

    // Copy and move semantics
    OTAUpdater(const OTAUpdater&) = delete;
    OTAUpdater& operator=(const OTAUpdater&) = delete;
    OTAUpdater(OTAUpdater&&) = default;
    OTAUpdater& operator=(OTAUpdater&&) = default;

    // ======= Lifecycle =======
    // Initialize OTA subsystem; currently a no-op placeholder.
    bool init();
    // Mark start of OTA capability (no background threads here).
    void start();
    // Mark stop of OTA capability (no cleanup needed currently).
    void stop();

    // ======= OTA Control =======
    // Begin OTA update. Requires total size (bytes) and optional expected SHA-256 (hex string).
    bool beginUpdate(size_t total_size, const std::string &expected_sha256 = "");

    // Write a data chunk to OTA partition; returns bytes written or -1 on error.
    int writeChunk(const uint8_t* data, size_t size);

    // Finish OTA update, validate, and set boot partition; returns false on failure.
    bool finishUpdate();

    // Abort the ongoing OTA update and reset counters.
    void abortUpdate();

    // ======= Status =======
    bool isUpdating() const { return updating; }
    uint32_t getExpectedSize() const { return total_bytes; }
    std::string getExpectedChecksum() const { return expected_sha256_hex; }
    uint32_t getBytesWritten() const { return bytes_written; }
    uint32_t getTotalBytes() const { return total_bytes; }
    // Get current progress percentage (0-100); returns 0 if not updating.
    uint8_t getProgressPercent() const;

    // Check if firmware size fits available OTA partition.
    bool checkStorageSpace(size_t firmware_size);

    // Get available OTA partition size in bytes.
    uint32_t getAvailableSpace() const;

    // ======= Callbacks =======
    using ProgressCallback = std::function<void(uint32_t current, uint32_t total)>;
    // Set progress callback invoked with bytes written/total.
    void setProgressCallback(ProgressCallback cb) { progress_callback = cb; }

private:
    // ======= Internal state =======
    bool updating = false;
    uint32_t bytes_written = 0;
    uint32_t total_bytes = 0;
    std::string expected_sha256_hex;
    bool checksum_enabled = false;

    // SHA-256 state
    mbedtls_sha256_context sha_ctx;
    bool sha_started = false;

    // ======= ESP32 OTA handle =======
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t* update_partition = nullptr;

    // ======= Callback =======
    ProgressCallback progress_callback;

    // ======= Helper functions =======
    bool validateFirmware();
    void reportProgress();
};
