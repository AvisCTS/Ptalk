#pragma once

#include <memory>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "system/StateTypes.hpp"
#include "system/StateManager.hpp"

// Forward declarations
class AudioInput;
class AudioOutput;
class AudioCodec;

// Coordinates mic/capture, codec, and speaker playback based on interaction state;
// exposes stream buffers to other modules (e.g., NetworkManager) and no networking logic.
class AudioManager
{
public:
    AudioManager();
    ~AudioManager();

    // ------------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------------
    // Initialize input/output/codec and create stream buffers; returns false on missing deps or buffer failure.
    bool init();

    // Start audio tasks (mic, codec, speaker); no-op if already started.
    void start();

    // Stop tasks and keep buffers allocated for reuse.
    void stop();

    // Lazily allocate buffers if init was skipped earlier; returns false on OOM.
    bool allocateResources() ;

    // Free stream buffers and stop any running tasks.
    void freeResources();
    // ------------------------------------------------------------------------
    // Dependency injection
    // ------------------------------------------------------------------------
    // Provide ownership of the microphone capture implementation.
    void setInput(std::unique_ptr<AudioInput> in);

    // Provide ownership of the speaker output implementation.
    void setOutput(std::unique_ptr<AudioOutput> out);

    // Provide ownership of the codec used for encode/decode.
    void setCodec(std::unique_ptr<AudioCodec> cdc);

    // ------------------------------------------------------------------------
    // Stream buffer access (NetworkManager dùng)
    // ------------------------------------------------------------------------
    // Encoded uplink buffer for microphone data.
    StreamBufferHandle_t getMicEncodedBuffer() const { return sb_mic_encoded; }

    // Encoded downlink buffer for speaker data.
    StreamBufferHandle_t getSpeakerEncodedBuffer() const { return sb_spk_encoded; }

    // ------------------------------------------------------------------------
    // Power / control
    // ------------------------------------------------------------------------
    // Enable low-power mode; stops capture/playback when true.
    void setPowerSaving(bool enable);

    // Set speaker output volume (0-100%). Applies immediately if output present.
    void setVolume(uint8_t percent);

private:
    // ------------------------------------------------------------------------
    // State callback
    // ------------------------------------------------------------------------
    // React to interaction state changes to start/stop listening and speaking.
    void handleInteractionState(state::InteractionState s,
                                state::InputSource src);

    // ------------------------------------------------------------------------
    // Audio actions
    // ------------------------------------------------------------------------
    // Begin capture path and reset playback buffers/codec for a fresh session.
    void startListening(state::InputSource src);

    // Temporarily halt capture without clearing buffers.
    void pauseListening();

    // Fully stop capture and mark listening false.
    void stopListening();

    // Mark device as speaking; leaves codec state intact to preserve continuity.
    void startSpeaking();

    // Stop speaking path and halt speaker playback if active.
    void stopSpeaking();

    // Convenience to stop both capture and playback.
    void stopAll();

private:
    // ------------------------------------------------------------------------
    // Tasks
    // ------------------------------------------------------------------------
    // FreeRTOS task trampolines.
    static void micTaskEntry(void *arg);
    static void codecTaskEntry(void *arg);
    static void spkTaskEntry(void *arg);

    // Task loops for mic capture/encode, codec decode/encode, and speaker playback.
    void micTaskLoop();
    void codecTaskLoop();
    void spkTaskLoop();

private:
    // ------------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------------
    std::atomic<bool> started{false};
    std::atomic<bool> listening{false};
    std::atomic<bool> speaking{false};
    std::atomic<bool> power_saving{false};
    std::atomic<bool> spk_playing{false};

    state::InputSource current_source = state::InputSource::UNKNOWN;

    // ------------------------------------------------------------------------
    // Components
    // ------------------------------------------------------------------------
    std::unique_ptr<AudioInput> input;
    std::unique_ptr<AudioOutput> output;
    std::unique_ptr<AudioCodec> codec;

    // ------------------------------------------------------------------------
    // Stream buffers (FreeRTOS - thread-safe, no race conditions)
    // ------------------------------------------------------------------------
    StreamBufferHandle_t sb_mic_pcm;     // PCM from mic
    StreamBufferHandle_t sb_mic_encoded; // Encoded uplink
    StreamBufferHandle_t sb_spk_pcm;     // PCM to speaker
    StreamBufferHandle_t sb_spk_encoded; // Encoded downlink

    // PCM decode buffer (static allocation to avoid heap alloc in task)
    int16_t spk_pcm_buffer[4096] = {};

    // ------------------------------------------------------------------------
    // Tasks
    // ------------------------------------------------------------------------
    TaskHandle_t mic_task = nullptr;
    TaskHandle_t codec_task = nullptr;
    TaskHandle_t spk_task = nullptr;

    // ------------------------------------------------------------------------
    // StateManager subscription
    // ------------------------------------------------------------------------
    int sub_interaction_id = -1;
};
