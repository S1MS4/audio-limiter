// Offline DSP tests.
//
// The limiter is the one component whose failure is silent: a broken routing
// layer is obvious, but a broken limiter just means the loud thing still
// hurts. These run the real DSP against synthetic signals and assert on the
// output, which is far more rigorous than listening to a call and hoping.

#include "dsp.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

constexpr double kRate = 48000.0;
constexpr size_t kCh = 2;

std::vector<float> Tone(double freq, double seconds, float amp) {
    const size_t frames = (size_t)(kRate * seconds);
    std::vector<float> b(frames * kCh);
    for (size_t i = 0; i < frames; ++i) {
        const float v = amp * (float)std::sin(2.0 * 3.14159265358979 * freq *
                                              (double)i / kRate);
        for (size_t c = 0; c < kCh; ++c) b[i * kCh + c] = v;
    }
    return b;
}

float PeakOf(const std::vector<float>& b, size_t from = 0) {
    float p = 0.0f;
    for (size_t i = from; i < b.size(); ++i) p = std::max(p, std::fabs(b[i]));
    return p;
}

// Limiter holds an atomic and so is not movable; configure in place.
void InitLimiter(dsp::Limiter& l, float ceilingDb, float lookaheadMs = 5.0f) {
    l.Configure(kRate, kCh, lookaheadMs);
    l.SetCeilingDb(ceilingDb);
    l.SetRelease(250.0f);
    l.Reset();
}

// Processing in one big call is not how the audio engine works; it hands the
// limiter whatever period size WASAPI gives it. Chunking catches state bugs
// that a single call would hide.
void ProcessChunked(dsp::Limiter& l, std::vector<float>& buf, size_t chunkFrames) {
    const size_t total = buf.size() / kCh;
    for (size_t off = 0; off < total; off += chunkFrames) {
        const size_t n = std::min(chunkFrames, total - off);
        l.Process(buf.data() + off * kCh, n);
    }
}

void TestCeilingIsEnforced() {
    std::puts("ceiling is enforced");
    for (float ceilDb : {-6.0f, -12.0f, -20.0f, -30.0f}) {
        auto sig = Tone(440.0, 0.5, 1.0f);   // full-scale, way over any ceiling
        dsp::Limiter lim; InitLimiter(lim, ceilDb);
        ProcessChunked(lim, sig, 480);

        const float allowed = dsp::DbToLinear(ceilDb);
        // Skip the lookahead priming region at the very start.
        const float peak = PeakOf(sig, 2000 * kCh);
        Check(peak <= allowed * 1.02f,
              "peak " + std::to_string(dsp::LinearToDb(peak)) +
                  " dB stays under ceiling " + std::to_string(ceilDb) + " dB");
    }
}

float RmsOf(const std::vector<float>& b, size_t from) {
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = from; i < b.size(); ++i, ++n) acc += (double)b[i] * b[i];
    return n ? (float)std::sqrt(acc / (double)n) : 0.0f;
}

void TestQuietAudioIsUntouched() {
    std::puts("quiet audio passes through unattenuated");
    const float amp = 0.05f;                 // -26 dB, well under the ceiling
    auto sig = Tone(440.0, 0.3, amp);
    const auto original = sig;
    dsp::Limiter lim; InitLimiter(lim, -6.0f);
    ProcessChunked(lim, sig, 480);

    // Output is delayed by the lookahead window -- that delay is the mechanism,
    // not a defect -- so a sample-aligned diff is meaningless here. Assert the
    // property that actually matters: no gain was applied.
    const size_t skip = 8000 * kCh;
    const float inPeak = PeakOf(original, skip);
    const float outPeak = PeakOf(sig, skip);
    const float inRms = RmsOf(original, skip);
    const float outRms = RmsOf(sig, skip);

    Check(std::fabs(outPeak - inPeak) < 1e-4f,
          "peak is unchanged (" + std::to_string(inPeak) + " -> " +
              std::to_string(outPeak) + ")");
    Check(std::fabs(outRms - inRms) < 1e-4f,
          "RMS is unchanged (" + std::to_string(inRms) + " -> " +
              std::to_string(outRms) + ")");
}

// The lookahead delay is a deliberate, bounded cost. Pin it so a change to the
// DSP cannot silently add latency to every call.
void TestLatencyIsBounded() {
    std::puts("added latency stays within the lookahead");
    const float lookaheadMs = 5.0f;
    auto sig = Tone(440.0, 0.1, 0.5f);
    std::vector<float> impulse(sig.size(), 0.0f);
    impulse[0] = 0.5f;
    impulse[1] = 0.5f;

    dsp::Limiter lim; InitLimiter(lim, -6.0f, lookaheadMs);
    lim.Process(impulse.data(), impulse.size() / kCh);

    size_t firstNonZero = impulse.size();
    for (size_t i = 0; i < impulse.size(); ++i)
        if (std::fabs(impulse[i]) > 1e-6f) { firstNonZero = i / kCh; break; }

    const size_t expected = (size_t)(kRate * lookaheadMs / 1000.0) + 1;
    Check(firstNonZero <= expected + 2,
          "impulse emerges after " + std::to_string(firstNonZero) +
              " frames (lookahead is " + std::to_string(expected) + ")");
    Check(firstNonZero * 1000.0 / kRate < 10.0,
          "added latency is under 10 ms (" +
              std::to_string(firstNonZero * 1000.0 / kRate) + " ms)");
}

// The whole point of lookahead: the very first sample of a sudden blast must
// already be attenuated. A limiter without it leaks the transient, which is
// exactly the part that hurts.
void TestNoTransientLeak() {
    std::puts("sudden blast does not leak through");
    const size_t quietFrames = (size_t)(kRate * 0.2);
    auto quiet = Tone(440.0, 0.2, 0.02f);
    auto blast = Tone(440.0, 0.3, 1.0f);

    std::vector<float> sig = quiet;
    sig.insert(sig.end(), blast.begin(), blast.end());

    dsp::Limiter lim; InitLimiter(lim, -20.0f);
    ProcessChunked(lim, sig, 480);

    const float allowed = dsp::DbToLinear(-20.0f);
    float worst = 0.0f;
    for (size_t i = quietFrames * kCh; i < sig.size(); ++i)
        worst = std::max(worst, std::fabs(sig[i]));

    Check(worst <= allowed * 1.05f,
          "no sample after the transition exceeds the ceiling (worst " +
              std::to_string(dsp::LinearToDb(worst)) + " dB)");
}

void TestOutputIsFinite() {
    std::puts("output is always finite");
    auto sig = Tone(1000.0, 0.2, 1.0f);
    dsp::Limiter lim; InitLimiter(lim, -40.0f);
    ProcessChunked(lim, sig, 128);

    bool finite = true;
    for (float v : sig) if (!std::isfinite(v)) finite = false;
    Check(finite, "no NaN or infinity in the output");
}

void TestChunkSizeDoesNotChangeResult() {
    std::puts("result is independent of buffer size");
    auto a = Tone(440.0, 0.3, 0.9f);
    auto b = a;

    dsp::Limiter l1; InitLimiter(l1, -12.0f);
    ProcessChunked(l1, a, 480);
    dsp::Limiter l2; InitLimiter(l2, -12.0f);
    ProcessChunked(l2, b, 97);   // deliberately awkward size

    float maxDelta = 0.0f;
    for (size_t i = 0; i < a.size(); ++i)
        maxDelta = std::max(maxDelta, std::fabs(a[i] - b[i]));

    Check(maxDelta < 1e-5f,
          "480-frame and 97-frame chunking agree (max delta " +
              std::to_string(maxDelta) + ")");
}

void TestGainRecovers() {
    std::puts("gain recovers after the loud part ends");
    auto loud = Tone(440.0, 0.2, 1.0f);
    auto quiet = Tone(440.0, 1.5, 0.05f);
    std::vector<float> sig = loud;
    sig.insert(sig.end(), quiet.begin(), quiet.end());

    dsp::Limiter lim; InitLimiter(lim, -20.0f);
    ProcessChunked(lim, sig, 480);

    // By the end, the quiet tone should be near its original amplitude again
    // rather than still ducked from the blast.
    const float tailPeak = PeakOf(sig, sig.size() - 4000 * kCh);
    Check(tailPeak > 0.04f,
          "quiet audio returns to normal after the blast (tail peak " +
              std::to_string(tailPeak) + " vs original 0.05)");
}

void TestRingBuffer() {
    std::puts("ring buffer");
    dsp::RingBuffer rb;
    rb.Resize(1000, 2);

    std::vector<float> in(200 * 2, 0.5f);
    Check(rb.Write(in.data(), 200) == 200, "writes 200 frames");
    Check(rb.Available() == 200, "reports 200 available");

    std::vector<float> out(200 * 2, 0.0f);
    Check(rb.Read(out.data(), 200) == 200, "reads 200 frames back");
    Check(out[0] == 0.5f && out[399] == 0.5f, "data round-trips intact");
    Check(rb.Available() == 0, "empty after draining");

    // Underrun must produce silence, never stale audio or a stall.
    std::vector<float> under(50 * 2, 9.0f);
    Check(rb.Read(under.data(), 50) == 0, "read on empty returns 0 frames");
    Check(under[0] == 0.0f, "underrun is filled with silence");

    // Overrun must be reported rather than corrupting the buffer.
    std::vector<float> big(2000 * 2, 0.25f);
    const size_t wrote = rb.Write(big.data(), 2000);
    Check(wrote < 2000, "over-large write is truncated, not wrapped over itself");
}

} // namespace

int main() {
    std::puts("=== DSP tests ===\n");
    TestCeilingIsEnforced();
    TestQuietAudioIsUntouched();
    TestLatencyIsBounded();
    TestNoTransientLeak();
    TestOutputIsFinite();
    TestChunkSizeDoesNotChangeResult();
    TestGainRecovers();
    TestRingBuffer();

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
