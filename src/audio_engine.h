// Capture -> limit -> render audio pipeline.
//
// Two threads: one woken by the capture device, one by the render device.
// They communicate only through a lock-free ring buffer. Neither allocates
// after Start() returns.

#pragma once

#include "dsp.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace engine {

struct Config {
    std::wstring captureDeviceId;   // where Discord is routed to
    std::wstring renderDeviceId;    // the user's real headphones
    float ceilingDb = -6.0f;
    float lookaheadMs = 5.0f;
    float releaseMs = 250.0f;
    // Linear pre-limiter gain. Carries the Windows-mixer volume the source
    // app had on the real device, which rerouting otherwise bypasses.
    float preGain = 1.0f;
};

struct Stats {
    std::atomic<uint64_t> underruns{0};   // render wanted audio, buffer was empty
    std::atomic<uint64_t> overruns{0};    // capture produced faster than render drained
    std::atomic<float> gainReductionDb{0.0f};
    std::atomic<float> inputPeakDb{-180.0f};
    std::atomic<bool> running{false};
    std::atomic<uint32_t> sampleRate{0};
    std::atomic<uint32_t> channels{0};
    std::atomic<uint32_t> periodFrames{0};
};

class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Returns S_OK, or a failing HRESULT with lastError() set.
    HRESULT Start(const Config& cfg);
    void Stop();
    bool IsRunning() const { return stats_.running.load(); }

    // Safe to call while running.
    void SetCeilingDb(float db);
    void SetReleaseMs(float ms);
    void SetPreGain(float linear);

    const Stats& stats() const { return stats_; }
    const std::wstring& lastError() const { return lastError_; }

private:
    void CaptureLoop();
    void RenderLoop();

    Config cfg_;
    Stats stats_;
    std::wstring lastError_;

    dsp::RingBuffer ring_;
    dsp::Limiter limiter_;

    std::thread captureThread_;
    std::thread renderThread_;
    std::atomic<bool> stop_{false};

    // Parameter changes are picked up by the audio thread between periods
    // rather than applied directly, so the UI thread never touches DSP state.
    std::atomic<float> pendingCeiling_{-6.0f};
    std::atomic<float> pendingRelease_{250.0f};
    std::atomic<float> pendingPreGain_{1.0f};
    std::atomic<bool> paramsDirty_{false};
};

} // namespace engine
