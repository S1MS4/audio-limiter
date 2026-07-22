// Real-time DSP primitives. Header-only, allocation-free once constructed.
//
// Everything here runs on the audio thread. Nothing in it may allocate,
// lock, block, or call into the OS. Violating that produces dropouts, not
// slowdowns -- a missed deadline is an audible click, and there is no
// recovering from it after the fact.

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace dsp {

// Single-producer single-consumer lock-free ring buffer.
//
// One capture thread writes, one render thread reads. Never call Write from
// two threads or Read from two threads.
class RingBuffer {
public:
    void Resize(size_t frames, size_t channels) {
        channels_ = channels;
        // One slot stays empty so full and empty are distinguishable.
        buf_.assign((frames + 1) * channels, 0.0f);
        capacity_ = frames + 1;
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
    }

    size_t Available() const {
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t r = read_.load(std::memory_order_acquire);
        return w >= r ? w - r : capacity_ - r + w;
    }

    // Returns frames actually written. A short return means the consumer is
    // behind and we are dropping audio, which is a real defect worth counting.
    size_t Write(const float* src, size_t frames) {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        const size_t free_frames = (r > w ? r - w : capacity_ - w + r) - 1;
        const size_t n = std::min(frames, free_frames);

        for (size_t i = 0; i < n; ++i) {
            const size_t slot = ((w + i) % capacity_) * channels_;
            for (size_t c = 0; c < channels_; ++c)
                buf_[slot + c] = src[i * channels_ + c];
        }
        write_.store((w + n) % capacity_, std::memory_order_release);
        return n;
    }

    // Frames not available are filled with silence, so the render callback
    // always has something to hand the device. Underrun is preferable to
    // stalling the audio engine.
    size_t Read(float* dst, size_t frames) {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t avail = (w >= r ? w - r : capacity_ - r + w);
        const size_t n = std::min(frames, avail);

        for (size_t i = 0; i < n; ++i) {
            const size_t slot = ((r + i) % capacity_) * channels_;
            for (size_t c = 0; c < channels_; ++c)
                dst[i * channels_ + c] = buf_[slot + c];
        }
        for (size_t i = n; i < frames; ++i)
            for (size_t c = 0; c < channels_; ++c)
                dst[i * channels_ + c] = 0.0f;

        read_.store((r + n) % capacity_, std::memory_order_release);
        return n;
    }

private:
    std::vector<float> buf_;
    size_t capacity_ = 0;
    size_t channels_ = 2;
    std::atomic<size_t> read_{0};
    std::atomic<size_t> write_{0};
};

inline float DbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }
inline float LinearToDb(float x) {
    return x > 1e-9f ? 20.0f * std::log10(x) : -180.0f;
}

// Lookahead peak limiter.
//
// The lookahead delay is the whole trick: audio is held back by a few
// milliseconds so the gain has already been pulled down by the time a loud
// transient arrives at the output. Without it a limiter always leaks the
// first millisecond of every spike, which is exactly the part that hurts.
//
// Gain reduction uses a sliding-maximum over the lookahead window, then a
// one-pole smoother. Attack is instantaneous by construction (the window
// sees the peak before it plays); release is slow enough not to pump.
class Limiter {
public:
    void Configure(double sampleRate, size_t channels, float lookaheadMs) {
        sampleRate_ = sampleRate;
        channels_ = channels;
        lookahead_ = std::max<size_t>(1, (size_t)(sampleRate * lookaheadMs / 1000.0));

        delay_.assign((lookahead_ + 1) * channels, 0.0f);
        window_.assign(lookahead_ + 1, 0.0f);
        delayPos_ = 0;
        gain_ = 1.0f;
        SetRelease(releaseMs_);
    }

    void SetCeilingDb(float db) { ceiling_ = DbToLinear(db); }

    // Linear gain applied to the input before peak detection, so limiting
    // acts on the signal the listener would have heard in bypass. Carries
    // the Windows-mixer volume the source had before it was rerouted here;
    // without it the limited path is louder than bypass ever was.
    void SetInputGain(float g) { inputGain_ = std::max(0.0f, g); }

    void SetRelease(float ms) {
        releaseMs_ = ms;
        // One-pole coefficient reaching ~63% of target in `ms`.
        releaseCoef_ = (float)std::exp(-1.0 / (sampleRate_ * (ms / 1000.0)));
    }

    // Reports the current gain reduction so the UI can show a meter. Read
    // from another thread; approximate is fine.
    float GainReductionDb() const {
        return LinearToDb(gainAtomic_.load(std::memory_order_relaxed));
    }

    void Reset() {
        std::fill(delay_.begin(), delay_.end(), 0.0f);
        std::fill(window_.begin(), window_.end(), 0.0f);
        delayPos_ = 0;
        gain_ = 1.0f;
    }

    // Processes in place. frames * channels_ samples.
    void Process(float* buf, size_t frames) {
        const size_t ch = channels_;
        const size_t len = lookahead_ + 1;

        for (size_t i = 0; i < frames; ++i) {
            float* in = buf + i * ch;

            for (size_t c = 0; c < ch; ++c) in[c] *= inputGain_;

            // Peak across channels: a spike in either ear must duck both,
            // otherwise the stereo image lurches sideways on every transient.
            float peak = 0.0f;
            for (size_t c = 0; c < ch; ++c) peak = std::max(peak, std::fabs(in[c]));

            window_[delayPos_] = peak;

            float* slot = &delay_[delayPos_ * ch];
            float held[8];
            for (size_t c = 0; c < ch && c < 8; ++c) {
                held[c] = slot[c];
                slot[c] = in[c];
            }

            delayPos_ = (delayPos_ + 1) % len;

            // Loudest sample anywhere in the lookahead window, including the
            // ones not yet played. This is what makes the attack lossless.
            float windowPeak = 0.0f;
            for (size_t k = 0; k < len; ++k)
                windowPeak = std::max(windowPeak, window_[k]);

            const float target =
                windowPeak > ceiling_ ? ceiling_ / windowPeak : 1.0f;

            // Duck immediately, recover gently.
            gain_ = target < gain_
                        ? target
                        : target + (gain_ - target) * releaseCoef_;

            for (size_t c = 0; c < ch && c < 8; ++c) {
                float v = held[c] * gain_;
                // Absolute backstop. The window maths should make this
                // unreachable, but clipping is cheaper than trusting it.
                in[c] = std::clamp(v, -ceiling_, ceiling_);
            }
        }

        gainAtomic_.store(gain_, std::memory_order_relaxed);
    }

private:
    double sampleRate_ = 48000.0;
    size_t channels_ = 2;
    size_t lookahead_ = 240;
    size_t delayPos_ = 0;

    float ceiling_ = 1.0f;
    float inputGain_ = 1.0f;
    float gain_ = 1.0f;
    float releaseMs_ = 250.0f;
    float releaseCoef_ = 0.999f;

    std::vector<float> delay_;
    std::vector<float> window_;
    std::atomic<float> gainAtomic_{1.0f};
};

} // namespace dsp
