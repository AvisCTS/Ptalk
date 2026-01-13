#include "AudioManager.hpp"

#include "AudioInput.hpp"
#include "AudioOutput.hpp"
#include "AudioCodec.hpp"
#include "esp_wifi.h"

#include "esp_log.h"
#include <cstring>

static const char *TAG = "AudioManager";

// ============================================================================
// Constructor / Destructor
// ============================================================================
AudioManager::AudioManager() = default;

AudioManager::~AudioManager()
{
    stop();
    // Stream buffers cleanup via xStreamBufferDelete if created
}

// ============================================================================
// Dependency injection
// ============================================================================
void AudioManager::setInput(std::unique_ptr<AudioInput> in)
{
    input = std::move(in);
}

void AudioManager::setOutput(std::unique_ptr<AudioOutput> out)
{
    output = std::move(out);
}

void AudioManager::setCodec(std::unique_ptr<AudioCodec> cdc)
{
    codec = std::move(cdc);
}

void AudioManager::setVolume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    if (output)
    {
        output->setVolume(percent);
        ESP_LOGI(TAG, "Volume set to %u%%", (unsigned)percent);
    }
}

// ============================================================================
// Init / Start / Stop
// ============================================================================
bool AudioManager::init()
{
    ESP_LOGI(TAG, "init()");

    if (!input || !output || !codec)
    {
        ESP_LOGE(TAG, "Missing input/output/codec");
        return false;
    }

    if (!input->init())
    {
        ESP_LOGE(TAG, "Failed to init Audio Input hardware");
        return false;
    }

    // -------------------------------
    // Stream buffers (FreeRTOS - thread-safe, no race conditions)
    // -------------------------------
    sb_mic_pcm = xStreamBufferCreate(
        4 * 1024, // Buffer size
        1         // Trigger level (1 byte to unblock reader)
    );
    sb_mic_encoded = xStreamBufferCreate(
        32 * 1024,
        1);
    sb_spk_pcm = xStreamBufferCreate(
        8 * 1024,
        1);
    sb_spk_encoded = xStreamBufferCreate(
        16 * 1024, // Increased for better jitter tolerance
        1);

    if (!sb_mic_pcm || !sb_mic_encoded || !sb_spk_pcm || !sb_spk_encoded)
    {
        ESP_LOGE(TAG, "Failed to create stream buffers");
        return false;
    }

    // -------------------------------
    // Subscribe InteractionState
    // -------------------------------
    sub_interaction_id =
        StateManager::instance().subscribeInteraction(
            [this](state::InteractionState s, state::InputSource src)
            {
                this->handleInteractionState(s, src);
            });

    ESP_LOGI(TAG, "AudioManager init OK");
    return true;
}

void AudioManager::start()
{
    if (started)
        return;
    started = true;

    ESP_LOGI(TAG, "start()");

    // -------------------------------
    // Create MIC task
    // -------------------------------
    xTaskCreatePinnedToCore(
        &AudioManager::micTaskEntry,
        "AudioMicTask",
        4096,
        this,
        6,
        &mic_task,
        1 // Core 0
    );

    // -------------------------------
    // Create CODEC task (decodes ADPCM to PCM)
    // -------------------------------
    xTaskCreatePinnedToCore(
        &AudioManager::codecTaskEntry,
        "AudioCodecTask",
        8192, // Stack for codec operations
        this,
        5, // Priority 5 - between mic and speaker
        &codec_task,
        1 // Core 0 with mic task
    );

    // -------------------------------
    // Create SPEAKER task (lower priority to not starve WiFi)
    // -------------------------------
    xTaskCreatePinnedToCore(
        &AudioManager::spkTaskEntry,
        "AudioSpkTask",
        4096, // Reduced stack - no longer doing decode
        this,
        6, // Priority 6 - below WiFi task (prio 23) to prevent beacon timeout
        &spk_task,
        1 // Core 1 for smooth playback
    );
}

void AudioManager::stop()
{
    if (!started)
        return;
    started = false;

    ESP_LOGW(TAG, "stop()");

    stopAll();

    // ✅ Allow tasks to exit themselves (they check `started` and self-delete)
    // Wait up to 1s for both tasks to terminate; then force delete as fallback.
    const uint32_t TIMEOUT_MS = 1000;
    uint32_t waited = 0;

    auto waitForExit = [&](TaskHandle_t &th)
    {
        if (!th)
            return;
        while (eTaskGetState(th) != eDeleted && waited < TIMEOUT_MS)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
        }
        if (eTaskGetState(th) != eDeleted)
        {
            ESP_LOGW(TAG, "Audio task did not exit; force deleting");
            vTaskDelete(th);
        }
        th = nullptr;
    };

    waitForExit(mic_task);
    waitForExit(codec_task);
    waitForExit(spk_task);
}

bool AudioManager::allocateResources()
{
    if (sb_mic_pcm != nullptr)
        return true; // Already allocated

    ESP_LOGW(TAG, "Allocating Audio Stream Buffers...");

    sb_mic_pcm = xStreamBufferCreate(4 * 1024, 1);
    sb_mic_encoded = xStreamBufferCreate(32 * 1024, 1);
    sb_spk_pcm = xStreamBufferCreate(8 * 1024, 1);
    sb_spk_encoded = xStreamBufferCreate(16 * 1024, 1);

    if (!sb_mic_pcm || !sb_mic_encoded || !sb_spk_pcm || !sb_spk_encoded)
    {
        ESP_LOGE(TAG, "Failed to allocate audio buffers - OUT OF RAM!");
        return false;
    }
    return true;
}

void AudioManager::freeResources()
{
    stop();
    if (sb_mic_pcm)
    {
        vStreamBufferDelete(sb_mic_pcm);
        sb_mic_pcm = nullptr;
    }
    if (sb_mic_encoded)
    {
        vStreamBufferDelete(sb_mic_encoded);
        sb_mic_encoded = nullptr;
    }
    if (sb_spk_pcm)
    {
        vStreamBufferDelete(sb_spk_pcm);
        sb_spk_pcm = nullptr;
    }
    if (sb_spk_encoded)
    {
        vStreamBufferDelete(sb_spk_encoded);
        sb_spk_encoded = nullptr;
    }
    ESP_LOGI(TAG, "AudioManager resources freed");
}

// ============================================================================
// State handling
// ============================================================================
void AudioManager::handleInteractionState(state::InteractionState s,
                                          state::InputSource src)
{
    switch (s)
    {
    case state::InteractionState::LISTENING:
        startListening(src);
        break;

    case state::InteractionState::PROCESSING:
        pauseListening();
        break;

    case state::InteractionState::SPEAKING:
        startSpeaking();
        break;

    case state::InteractionState::CANCELLING:
    case state::InteractionState::IDLE:
        stopAll();
        break;

    case state::InteractionState::SLEEPING:
        stopAll();
        setPowerSaving(true);
        break;

    default:
        break;
    }
}

// ============================================================================
// Audio actions
// ============================================================================
void AudioManager::startListening(state::InputSource src)
{
    if (listening)
        return;

    ESP_LOGI(TAG, "Start listening (Interruption handled)");

    // Stop speaker immediately if mid-speech
    if (speaking)
    {
        stopSpeaking(); // This calls output->stopPlayback()
    }

    // Clear speaker buffers to avoid playing stale audio
    xStreamBufferReset(sb_spk_encoded); // Drop pending encoded frames
    xStreamBufferReset(sb_spk_pcm);     // Drop pending PCM frames

    // Reset codec to clear ADPCM predictor state for a clean session
    if (codec)
    {
        codec->reset();
    }

    current_source = src;
    listening = true;
    speaking = false;

    // Begin capture
    input->startCapture();
}

void AudioManager::pauseListening()
{
    if (!listening)
        return;
    ESP_LOGI(TAG, "Pause listening");
    input->stopCapture();
}

void AudioManager::stopListening()
{
    if (!listening)
        return;
    ESP_LOGI(TAG, "Stop listening");
    listening = false;

    input->stopCapture();

    // Clear pending mic data immediately to avoid residual uplink
    if (sb_mic_pcm)
        xStreamBufferReset(sb_mic_pcm);
    if (sb_mic_encoded)
        xStreamBufferReset(sb_mic_encoded);

    // Reset codec so next session starts clean
    if (codec)
        codec->reset();
}

void AudioManager::startSpeaking()
{
    if (speaking)
        return;
    ESP_LOGI(TAG, "Start speaking");
    speaking = true;

    // Wake speaker task immediately (don't wait for 100ms idle timeout)
    if (spk_task)
    {
        xTaskNotifyGive(spk_task);
        ESP_LOGD(TAG, "Speaker task notified to wake up");
    }

    // DO NOT reset codec here - it breaks ADPCM predictor continuity
    // Only reset when switching to a completely new audio stream/session
}

void AudioManager::stopSpeaking()
{
    if (!speaking)
        return;
    ESP_LOGI(TAG, "Stop speaking");
    speaking = false;
    // Stop I2S playback unconditionally; underlying driver tracks running state
    if (output)
        output->stopPlayback();

    // Clear speaker buffers to drop any stale frames
    if (sb_spk_encoded)
        xStreamBufferReset(sb_spk_encoded);
    if (sb_spk_pcm)
        xStreamBufferReset(sb_spk_pcm);

    // Reset codec decode state for a fresh next session
    if (codec)
        codec->reset();
}

void AudioManager::stopAll()
{
    stopListening();
    stopSpeaking();
}

void AudioManager::setPowerSaving(bool enable)
{
    power_saving = enable;
    if (enable)
        stopAll();
}

// ============================================================================
// Tasks
// ============================================================================
void AudioManager::micTaskEntry(void *arg)
{
    static_cast<AudioManager *>(arg)->micTaskLoop();
}

void AudioManager::codecTaskEntry(void *arg)
{
    static_cast<AudioManager *>(arg)->codecTaskLoop();
}

void AudioManager::spkTaskEntry(void *arg)
{
    static_cast<AudioManager *>(arg)->spkTaskLoop();
}

// ============================================================================
// MIC task: PCM → ENCODE → rb_mic_encoded
// ============================================================================
void AudioManager::micTaskLoop()
{
    ESP_LOGI(TAG, "MIC task started");

    constexpr size_t PCM_FRAME = 256;
    int16_t pcm_buf[PCM_FRAME];

    while (started)
    {
        if (!listening || power_saving)
        {
            // ESP_LOGI(TAG, "MIC loop: %s, power_saving=%d",
            //          listening ? "listening" : "not listening",
            //          power_saving.load());
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t samples = input->readPcm(pcm_buf, PCM_FRAME);
        // if (samples > 0)
        // {
        //     ESP_LOGI(TAG, "MIC got %zu samples", samples);
        // }
        if (samples == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        size_t bytes = samples * sizeof(int16_t);

        size_t sent = xStreamBufferSend(
            sb_mic_pcm,
            reinterpret_cast<uint8_t *>(pcm_buf),
            bytes,
            pdMS_TO_TICKS(10));
        if (sent < bytes)
        {
            ESP_LOGW("MIC", "Buffer Full! Dropped %zu bytes", bytes - sent);
        }
    }

    ESP_LOGW(TAG, "MIC task stopped");
    vTaskDelete(nullptr);
}

// ============================================================================
// CODEC task: ADPCM buffer → decode → PCM buffer
// Separates decode logic from I2S timing - flexible for different codecs
// ============================================================================
void AudioManager::codecTaskLoop()
{
    ESP_LOGI(TAG, "Codec task started");

    constexpr size_t PCM_FRAME = 256;   // 16 ms @16kHz
    constexpr size_t ADPCM_FRAME = 512; // Server-side expects 512-byte chunks

    int16_t pcm_in[PCM_FRAME];
    uint8_t encoded[ADPCM_FRAME];

    int16_t pcm_out[1024]; // 64 ms PCM output
    bool new_decode_session = true;

    while (started)
    {
        // =====================
        // ENCODE (MIC → SERVER)
        // =====================
        if (!speaking)
        {
            size_t pcm_bytes = xStreamBufferReceive(
                sb_mic_pcm,
                reinterpret_cast<uint8_t *>(pcm_in),
                sizeof(pcm_in),
                pdMS_TO_TICKS(10)

            );

            if (pcm_bytes == sizeof(pcm_in))
            {
                size_t samples = pcm_bytes / sizeof(int16_t);

                size_t enc_len = codec->encode(
                    pcm_in,
                    samples,
                    encoded,
                    sizeof(encoded));
                // ESP_LOGD(TAG, "Encoded %zu PCM samples to %zu bytes", samples, enc_len);
                if (enc_len > 0)
                {
                    xStreamBufferSend(
                        sb_mic_encoded,
                        encoded,
                        enc_len,
                        pdMS_TO_TICKS(10));
                }
            }
        }
        // =====================
        // DECODE (SERVER → SPK)
        // =====================
        if (!speaking || power_saving)
        {
            xStreamBufferReset(sb_spk_encoded);
            xStreamBufferReset(sb_spk_pcm);
            // codec->reset();
            new_decode_session = true;

            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        size_t got = xStreamBufferReceive(
            sb_spk_encoded,
            encoded,
            sizeof(encoded),
            pdMS_TO_TICKS(20));

        if (got == sizeof(encoded))
        {
            if (new_decode_session)
            {
                codec->reset();
                new_decode_session = false;
                ESP_LOGI(TAG, "Codec: New decode session started");
            }

            size_t out_samples = codec->decode(
                encoded,
                got,
                pcm_out,
                1024);

            if (out_samples > 0)
            {
                size_t written = xStreamBufferSend(
                    sb_spk_pcm,
                    reinterpret_cast<uint8_t *>(pcm_out),
                    out_samples * sizeof(int16_t),
                    pdMS_TO_TICKS(1000));
                if (written != out_samples * sizeof(int16_t))
                {
                    ESP_LOGW(TAG, "Codec: SPK buffer full, dropped %zu bytes", 
                             out_samples * sizeof(int16_t) - written);
                }
            }
        }
        else if (got > 0)
        {
            ESP_LOGW(TAG, "Codec: Got partial ADPCM %zu/%zu bytes", got, sizeof(encoded));
        }
    }

    ESP_LOGW(TAG, "Codec task ended");
    vTaskDelete(nullptr);
}

// ============================================================================
// SPEAKER task: PCM buffer → I2S output
// Simplified - only handles I2S timing, no decode logic
// I2S clock controls timing naturally
// ============================================================================
void AudioManager::spkTaskLoop()
{
    ESP_LOGI(TAG, "Speaker task started");
    
    constexpr size_t PCM_CHUNK_SAMPLES = 1024;
    constexpr size_t PCM_CHUNK_BYTES =
        PCM_CHUNK_SAMPLES * sizeof(int16_t);

    int16_t pcm_chunk[PCM_CHUNK_SAMPLES];
    bool i2s_started = false;
    uint32_t timeout_count = 0;

    while (started)
    {
        // When not speaking, idle but stay alive for next session
        if (!speaking || power_saving)
        {
            if (i2s_started)
            {
                ESP_LOGI(TAG, "Speaker: Stopping I2S (speaking=%d, power_saving=%d)", 
                         speaking.load(), power_saving.load());
                output->stopPlayback();
                i2s_started = false;
                timeout_count = 0;
            }
            // Idle: wait for notification or timeout (100ms max) - allows quick wake-up when speaking starts
            uint32_t notified = ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(100));
            if (notified)
            {
                ESP_LOGD(TAG, "Speaker: Woken by notification");
            }
            continue;
        }

        // Now speaking=true, try to start I2S if not already
        if (!i2s_started)
        {
            ESP_LOGI(TAG, "Speaker: Attempting startPlayback()...");
            if (!output->startPlayback())
            {
                ESP_LOGW(TAG, "Speaker: startPlayback() failed, retrying...");
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            i2s_started = true;
            timeout_count = 0;
            ESP_LOGI(TAG, "Speaker: I2S playback started");
        }

        // Read PCM from buffer and play
        size_t got = xStreamBufferReceive(
            sb_spk_pcm,
            reinterpret_cast<uint8_t *>(pcm_chunk),
            PCM_CHUNK_BYTES,
            pdMS_TO_TICKS(100));

        if (got == PCM_CHUNK_BYTES)
        {
            timeout_count = 0;
            output->writePcm(pcm_chunk, PCM_CHUNK_SAMPLES);
        }
        else if (got > 0)
        {
            // Partial data
            ESP_LOGW(TAG, "Speaker: Got partial PCM data %zu/%zu bytes", got, PCM_CHUNK_BYTES);
            timeout_count = 0;
        }
        else
        {
            // No data (timeout)
            timeout_count++;
            if (timeout_count % 5 == 0)
            {
                ESP_LOGD(TAG, "Speaker: Waiting for PCM data (timeout_count=%u)", timeout_count);
            }
        }
    }

    // Cleanup on manager stop
    if (i2s_started)
    {
        ESP_LOGI(TAG, "Speaker: Cleaning up I2S before exit");
        output->stopPlayback();
    }

    ESP_LOGW(TAG, "Speaker task ended");
    vTaskDelete(nullptr);
}
