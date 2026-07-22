// Per-application audio endpoint routing via the undocumented
// Windows.Media.Internal.AudioPolicyConfig activation factory.
//
// This is the same mechanism the Windows Sound settings UI uses for
// "App volume and device preferences". It is not documented by Microsoft.
// The interface layout below is transcribed from two independent
// open-source implementations that ship it in production:
//   EarTrumpet  - https://github.com/File-New-Project/EarTrumpet
//   SoundSwitch - https://github.com/Belphemur/SoundSwitch
//
// Two vtable variants exist. The IID selects which one you get, and the
// padding counts differ, so calling the wrong one lands on the wrong
// function pointer and crashes. Probe Win11 first, then Win10.

#pragma once

#include <windows.h>
#include <inspectable.h>
#include <mmdeviceapi.h>
#include <string>

namespace routing {

// Windows 11 21H2 and later.
// 19 unused slots precede the three methods we care about.
struct __declspec(uuid("ab3d4648-e242-459f-b02f-541c70306324"))
IAudioPolicyConfigFactoryWin11 : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Unused00() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused01() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused02() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused03() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused04() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused05() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused06() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused07() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused08() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused09() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused10() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused11() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused12() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused13() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused14() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused15() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused16() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused17() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused18() = 0;

    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(
        UINT32 processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(
        UINT32 processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

// Windows 10 before 21H2. One additional padding slot relative to Win11.
struct __declspec(uuid("2a59116d-6c4f-45e0-a74f-707e3fef9258"))
IAudioPolicyConfigFactoryWin10 : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Unused00() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused01() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused02() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused03() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused04() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused05() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused06() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused07() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused08() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused09() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused10() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused11() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused12() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused13() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused14() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused15() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused16() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused17() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused18() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused19() = 0;

    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(
        UINT32 processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(
        UINT32 processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

// Routes one process's audio to a specific endpoint.
//
// deviceId is NOT the raw MMDevice id. It must be wrapped in the
// "\\?\SWD#MMDEVAPI#{id}#{guid}" form -- see MakePolicyDeviceId. Passing an
// empty string resets the process to the system default, which is how
// routing is restored on shutdown.
class EndpointRouter {
public:
    EndpointRouter() = default;
    ~EndpointRouter();

    EndpointRouter(const EndpointRouter&) = delete;
    EndpointRouter& operator=(const EndpointRouter&) = delete;

    // Resolves the activation factory. Call once. Returns S_OK or the
    // failing HRESULT from RoGetActivationFactory.
    HRESULT Initialize();

    bool IsWin11Variant() const { return win11_ != nullptr; }

    // Empty deviceId restores the process to the system default.
    HRESULT SetEndpoint(DWORD processId, const std::wstring& deviceId,
                        EDataFlow flow = eRender, ERole role = eMultimedia);

    // Reads back what a process is currently routed to. Empty result means
    // "system default". Use this to capture prior state before changing it.
    HRESULT GetEndpoint(DWORD processId, std::wstring& deviceIdOut,
                        EDataFlow flow = eRender, ERole role = eMultimedia);

    // Converts an MMDevice id into the form this API expects.
    static std::wstring MakePolicyDeviceId(const std::wstring& mmDeviceId,
                                           EDataFlow flow = eRender);

private:
    IAudioPolicyConfigFactoryWin11* win11_ = nullptr;
    IAudioPolicyConfigFactoryWin10* win10_ = nullptr;
};

} // namespace routing
