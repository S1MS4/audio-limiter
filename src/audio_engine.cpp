#include "audio_engine.h"
#include "com_ptr.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <avrt.h>
#include <algorithm>
#include <vector>

namespace engine {
namespace {

// Raises the calling thread to the scheduler class Windows reserves for
// audio work. Without it a busy desktop will starve the render thread and
// produce dropouts that look like bugs in the DSP.
struct MmcssTask {
    HANDLE h = nullptr;
    MmcssTask() {
        DWORD idx = 0;
        h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
    }
    ~MmcssTask() { if (h) AvRevertMmThreadCharacteristics(h); }
};

// {00000003-0000-0010-8000-00aa00389b71}. Spelled out rather than pulled from
// ksmedia.h, which MinGW only defines under INITGUID and would drag the whole
// kernel-streaming header set into this translation unit.
const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

bool IsFloatFormat(const WAVEFORMATEX* wf) {
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return IsEqualGUID(ext->SubFormat, kSubtypeIeeeFloat) != 0;
    }
    return false;
}

HRESULT GetDevice(const std::wstring& id, EDataFlow flow, ComPtr<IMMDevice>& out) {
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator, en.put_void());
    if (FAILED(hr)) return hr;
    if (id.empty()) return en->GetDefaultAudioEndpoint(flow, eMultimedia, out.put());
    return en->GetDevice(id.c_str(), out.put());
}

} // namespace

AudioEngine::~AudioEngine() { Stop(); }

void AudioEngine::SetCeilingDb(float db) {
    pendingCeiling_.store(db, std::memory_order_relaxed);
    paramsDirty_.store(true, std::memory_order_release);
}

void AudioEngine::SetReleaseMs(float ms) {
    pendingRelease_.store(ms, std::memory_order_relaxed);
    paramsDirty_.store(true, std::memory_order_release);
}

void AudioEngine::SetPreGain(float linear) {
    pendingPreGain_.store(linear, std::memory_order_relaxed);
    paramsDirty_.store(true, std::memory_order_release);
}

HRESULT AudioEngine::Start(const Config& cfg) {
    Stop();

    cfg_ = cfg;
    stop_.store(false);
    lastError_.clear();
    pendingCeiling_.store(cfg.ceilingDb);
    pendingRelease_.store(cfg.releaseMs);
    pendingPreGain_.store(cfg.preGain);

    // Probe the render device first: its mix format becomes the pipeline
    // format, and the capture side is asked to convert into it.
    ComPtr<IMMDevice> renderDev;
    HRESULT hr = GetDevice(cfg_.renderDeviceId, eRender, renderDev);
    if (FAILED(hr)) { lastError_ = L"Output device not found."; return hr; }

    ComPtr<IAudioClient> probe;
    hr = renderDev->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, probe.put_void());
    if (FAILED(hr)) { lastError_ = L"Could not open the output device."; return hr; }

    WAVEFORMATEX* mix = nullptr;
    hr = probe->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) { lastError_ = L"Could not read the output format."; return hr; }

    if (!IsFloatFormat(mix)) {
        CoTaskMemFree(mix);
        lastError_ = L"This output device uses an unsupported sample format.";
        return E_FAIL;
    }

    const uint32_t rate = mix->nSamplesPerSec;
    const uint32_t ch = mix->nChannels;
    CoTaskMemFree(mix);

    stats_.sampleRate.store(rate);
    stats_.channels.store(ch);

    limiter_.Configure(rate, ch, cfg_.lookaheadMs);
    limiter_.SetCeilingDb(cfg_.ceilingDb);
    limiter_.SetRelease(cfg_.releaseMs);
    limiter_.SetInputGain(cfg_.preGain);
    limiter_.Reset();

    // Buffer 200 ms. Large enough to ride out a scheduling hiccup, small
    // enough that it never becomes a source of latency people can hear.
    ring_.Resize(rate / 5, ch);

    stats_.underruns.store(0);
    stats_.overruns.store(0);
    stats_.running.store(true);

    captureThread_ = std::thread(&AudioEngine::CaptureLoop, this);
    renderThread_ = std::thread(&AudioEngine::RenderLoop, this);
    return S_OK;
}

void AudioEngine::Stop() {
    if (!stats_.running.load() && !captureThread_.joinable() &&
        !renderThread_.joinable()) {
        return;
    }
    stop_.store(true);
    if (captureThread_.joinable()) captureThread_.join();
    if (renderThread_.joinable()) renderThread_.join();
    stats_.running.store(false);
}

void AudioEngine::CaptureLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MmcssTask mmcss;

    ComPtr<IMMDevice> dev;
    if (FAILED(GetDevice(cfg_.captureDeviceId, eRender, dev))) {
        stop_.store(true);
        CoUninitialize();
        return;
    }

    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                             client.put_void()))) {
        stop_.store(true);
        CoUninitialize();
        return;
    }

    WAVEFORMATEX* wf = nullptr;
    if (FAILED(client->GetMixFormat(&wf)) || !wf) {
        stop_.store(true);
        CoUninitialize();
        return;
    }

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Loopback: we read what this endpoint is playing. Discord has been
    // routed here, so this is Discord's audio and nobody else's.
    HRESULT hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        2000000 /* 200ms */, 0, wf, nullptr);

    const uint32_t srcCh = wf->nChannels;
    CoTaskMemFree(wf);

    if (FAILED(hr)) {
        CloseHandle(evt);
        stop_.store(true);
        CoUninitialize();
        return;
    }

    // Loopback streams do not signal an event on all Windows versions, so
    // the wait has a timeout and the loop polls as a fallback.
    client->SetEventHandle(evt);

    ComPtr<IAudioCaptureClient> cap;
    if (FAILED(client->GetService(IID_IAudioCaptureClient, cap.put_void()))) {
        CloseHandle(evt);
        stop_.store(true);
        CoUninitialize();
        return;
    }

    const uint32_t dstCh = stats_.channels.load();
    std::vector<float> scratch(4096 * dstCh);

    client->Start();

    while (!stop_.load(std::memory_order_relaxed)) {
        WaitForSingleObject(evt, 20);

        UINT32 packet = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                break;

            if (frames > 0) {
                if (scratch.size() < (size_t)frames * dstCh)
                    scratch.resize((size_t)frames * dstCh);

                const float* src = reinterpret_cast<const float*>(data);
                float peak = 0.0f;

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill_n(scratch.data(), (size_t)frames * dstCh, 0.0f);
                } else if (srcCh == dstCh) {
                    std::copy_n(src, (size_t)frames * dstCh, scratch.data());
                    for (size_t i = 0; i < (size_t)frames * dstCh; ++i)
                        peak = std::max(peak, std::fabs(scratch[i]));
                } else {
                    // Channel-count mismatch: fold to the first channel and
                    // duplicate. Crude, but only reachable on odd hardware.
                    for (UINT32 f = 0; f < frames; ++f) {
                        const float v = src[(size_t)f * srcCh];
                        peak = std::max(peak, std::fabs(v));
                        for (uint32_t c = 0; c < dstCh; ++c)
                            scratch[(size_t)f * dstCh + c] = v;
                    }
                }

                stats_.inputPeakDb.store(dsp::LinearToDb(peak),
                                         std::memory_order_relaxed);

                const size_t wrote = ring_.Write(scratch.data(), frames);
                if (wrote < frames)
                    stats_.overruns.fetch_add(1, std::memory_order_relaxed);
            }
            cap->ReleaseBuffer(frames);
        }
    }

    client->Stop();
    CloseHandle(evt);
    CoUninitialize();
}

void AudioEngine::RenderLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MmcssTask mmcss;

    ComPtr<IMMDevice> dev;
    if (FAILED(GetDevice(cfg_.renderDeviceId, eRender, dev))) {
        stop_.store(true);
        CoUninitialize();
        return;
    }

    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                             client.put_void()))) {
        stop_.store(true);
        CoUninitialize();
        return;
    }

    WAVEFORMATEX* wf = nullptr;
    if (FAILED(client->GetMixFormat(&wf)) || !wf) {
        stop_.store(true);
        CoUninitialize();
        return;
    }

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    300000 /* 30ms */, 0, wf, nullptr);
    const uint32_t ch = wf->nChannels;
    CoTaskMemFree(wf);

    if (FAILED(hr)) {
        CloseHandle(evt);
        stop_.store(true);
        CoUninitialize();
        return;
    }

    client->SetEventHandle(evt);

    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);
    stats_.periodFrames.store(bufferFrames);

    ComPtr<IAudioRenderClient> ren;
    if (FAILED(client->GetService(IID_IAudioRenderClient, ren.put_void()))) {
        CloseHandle(evt);
        stop_.store(true);
        CoUninitialize();
        return;
    }

    client->Start();

    while (!stop_.load(std::memory_order_relaxed)) {
        if (WaitForSingleObject(evt, 200) != WAIT_OBJECT_0) continue;

        if (paramsDirty_.exchange(false, std::memory_order_acquire)) {
            limiter_.SetCeilingDb(pendingCeiling_.load(std::memory_order_relaxed));
            limiter_.SetRelease(pendingRelease_.load(std::memory_order_relaxed));
            limiter_.SetInputGain(pendingPreGain_.load(std::memory_order_relaxed));
        }

        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        const UINT32 avail = bufferFrames - padding;
        if (avail == 0) continue;

        BYTE* out = nullptr;
        if (FAILED(ren->GetBuffer(avail, &out))) break;

        float* fout = reinterpret_cast<float*>(out);
        const size_t got = ring_.Read(fout, avail);
        if (got < avail) stats_.underruns.fetch_add(1, std::memory_order_relaxed);

        limiter_.Process(fout, avail);
        stats_.gainReductionDb.store(limiter_.GainReductionDb(),
                                     std::memory_order_relaxed);

        ren->ReleaseBuffer(avail, 0);
    }

    client->Stop();
    CloseHandle(evt);
    CoUninitialize();
}

} // namespace engine
