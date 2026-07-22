// Experiment: can we drop the virtual audio device entirely?
//
// Windows 10 build 20348+ can capture the audio of one process tree directly
// (AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK). That solves capture with no
// driver. It does not solve *diverting* -- the original still reaches the
// speakers, so you would hear it twice.
//
// The idea under test: capture Discord via process loopback, then silence its
// normal path with ISimpleAudioVolume. That only works if the session volume
// is applied AFTER the loopback tap. If it is applied before, we capture
// silence and the whole approach is dead.
//
// This is undocumented for the per-process case, so measure it.

#include "com_ptr.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#else
// MinGW does not ship this header. The layout is public ABI and stable since
// Windows 10 build 20348, so declaring it here is safe.
typedef enum {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;

typedef enum {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;

typedef struct {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

typedef struct {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
} AUDIOCLIENT_ACTIVATION_PARAMS;

#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK \
    L"VAD\\Process_Loopback"
#endif
#include <audiopolicy.h>
#include <cmath>
#include <cstdio>
#include <vector>

namespace probe {
namespace {

// {41D949AB-9862-444A-80F6-C261334DA5EB}
const GUID __uuidof_IActivateAudioInterfaceCompletionHandler = {
    0x41D949AB, 0x9862, 0x444A,
    {0x80, 0xF6, 0xC2, 0x61, 0x33, 0x4D, 0xA5, 0xEB}};

// ActivateAudioInterfaceAsync is asynchronous by contract even though it
// completes almost immediately here; a handler is mandatory. Hand-rolled
// rather than WRL::RuntimeClass, which MinGW does not ship.
class Handler : public IActivateAudioInterfaceCompletionHandler {
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_FAIL;
    ComPtr<IAudioClient> client;

    // Stack-allocated and outlives the call, so refcounting is a formality.
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, __uuidof_IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        IUnknown* raw = nullptr;
        op->GetActivateResult(&result, &raw);
        if (SUCCEEDED(result) && raw) {
            raw->QueryInterface(IID_IAudioClient, client.put_void());
            raw->Release();
        }
        SetEvent(done);
        return S_OK;
    }
};

// Peak of `seconds` of captured audio, or -1 on failure.
float CapturePeak(IAudioClient* client, IAudioCaptureClient* cap, double seconds) {
    const DWORD until = GetTickCount() + (DWORD)(seconds * 1000);
    float peak = 0.0f;
    while (GetTickCount() < until) {
        UINT32 packet = 0;
        if (FAILED(cap->GetNextPacketSize(&packet))) return -1.0f;
        if (packet == 0) { Sleep(5); continue; }

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
            return -1.0f;
        if (frames && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            const float* f = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < frames * 2; ++i)
                peak = (std::fabs(f[i]) > peak) ? std::fabs(f[i]) : peak;
        }
        cap->ReleaseBuffer(frames);
    }
    (void)client;
    return peak;
}

ComPtr<ISimpleAudioVolume> SessionVolumeFor(DWORD pid) {
    ComPtr<ISimpleAudioVolume> out;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator, en.put_void())))
        return out;
    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, dev.put())))
        return out;
    ComPtr<IAudioSessionManager2> mgr;
    if (FAILED(dev->Activate(IID_IAudioSessionManager2, CLSCTX_ALL, nullptr,
                             mgr.put_void())))
        return out;
    ComPtr<IAudioSessionEnumerator> se;
    if (FAILED(mgr->GetSessionEnumerator(se.put()))) return out;

    int n = 0;
    se->GetCount(&n);
    for (int i = 0; i < n; ++i) {
        ComPtr<IAudioSessionControl> c;
        if (FAILED(se->GetSession(i, c.put()))) continue;
        ComPtr<IAudioSessionControl2> c2;
        if (FAILED(c.query(IID_IAudioSessionControl2, c2))) continue;
        DWORD spid = 0;
        if (FAILED(c2->GetProcessId(&spid)) || spid != pid) continue;
        if (SUCCEEDED(c2.query(IID_ISimpleAudioVolume, out))) return out;
    }
    return out;
}

} // namespace

int Run(DWORD pid) {
    std::printf("Process loopback probe on pid %lu\n\n", pid);

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    // Resolved at runtime: MinGW has no import library for mmdevapi, and this
    // also degrades gracefully on Windows builds that predate the API.
    using ActivateFn = HRESULT(STDAPICALLTYPE*)(
        LPCWSTR, REFIID, PROPVARIANT*,
        IActivateAudioInterfaceCompletionHandler*,
        IActivateAudioInterfaceAsyncOperation**);

    HMODULE mmdev = LoadLibraryW(L"Mmdevapi.dll");
    auto Activate = mmdev ? (ActivateFn)GetProcAddress(
                                mmdev, "ActivateAudioInterfaceAsync")
                          : nullptr;
    if (!Activate) {
        std::puts("ActivateAudioInterfaceAsync unavailable "
                  "(needs Windows 10 build 20348 or newer)");
        return 1;
    }

    Handler handler;
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = Activate(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                          IID_IAudioClient, &pv, &handler, op.put());
    if (FAILED(hr)) {
        std::printf("ActivateAudioInterfaceAsync failed: 0x%08lX\n", hr);
        return 1;
    }
    WaitForSingleObject(handler.done, 5000);
    if (FAILED(handler.result) || !handler.client) {
        std::printf("activation failed: 0x%08lX\n", handler.result);
        return 1;
    }
    std::puts("process loopback activated");

    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

    hr = handler.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0,
                                     &wf, nullptr);
    if (FAILED(hr)) {
        std::printf("Initialize failed: 0x%08lX\n", hr);
        return 1;
    }

    ComPtr<IAudioCaptureClient> cap;
    if (FAILED(handler.client->GetService(IID_IAudioCaptureClient,
                                           cap.put_void()))) {
        std::puts("GetService failed");
        return 1;
    }
    handler.client->Start();

    std::puts("\nPlay audio in the target app now.\n");
    Sleep(2000);

    const float before = CapturePeak(handler.client.get(), cap.get(), 3.0);
    std::printf("  peak with session at normal volume : %.4f\n", before);

    auto vol = SessionVolumeFor(pid);
    if (!vol) {
        std::puts("\ncould not find the audio session; is it playing?");
        handler.client->Stop();
        return 1;
    }

    float original = 1.0f;
    vol->GetMasterVolume(&original);
    vol->SetMasterVolume(0.0f, nullptr);
    Sleep(500);
    const float muted = CapturePeak(handler.client.get(), cap.get(), 3.0);
    std::printf("  peak with session volume at zero   : %.4f\n", muted);
    vol->SetMasterVolume(original, nullptr);

    handler.client->Stop();

    std::puts("");
    if (before < 0.001f) {
        std::puts("INCONCLUSIVE: nothing was playing. Start audio and retry.");
        return 2;
    }
    if (muted > before * 0.5f) {
        std::puts("VIABLE: capture survives the mute.");
        std::puts("The virtual audio device can be dropped entirely.");
        return 0;
    }
    std::puts("DEAD: muting the session also silences the capture.");
    std::puts("Session volume is applied before the loopback tap, so this");
    std::puts("cannot divert audio. A virtual device remains necessary.");
    return 3;
}

} // namespace probe
