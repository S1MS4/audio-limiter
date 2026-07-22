#include "audio_policy_config.h"

#include <roapi.h>
#include <winstring.h>

namespace routing {
namespace {

// RAII for HSTRING, which must be deleted with WindowsDeleteString.
class HString {
public:
    HString() = default;
    explicit HString(const std::wstring& s) {
        WindowsCreateString(s.c_str(), static_cast<UINT32>(s.size()), &h_);
    }
    ~HString() { if (h_) WindowsDeleteString(h_); }

    HString(const HString&) = delete;
    HString& operator=(const HString&) = delete;

    HSTRING get() const { return h_; }
    HSTRING* put() { return &h_; }

    std::wstring str() const {
        if (!h_) return {};
        UINT32 len = 0;
        const wchar_t* p = WindowsGetStringRawBuffer(h_, &len);
        return p ? std::wstring(p, len) : std::wstring();
    }

private:
    HSTRING h_ = nullptr;
};

constexpr wchar_t kActivationClass[] = L"Windows.Media.Internal.AudioPolicyConfig";

} // namespace

// {ab3d4648-e242-459f-b02f-541c70306324}
const GUID IID_AudioPolicyConfigFactoryWin11 = {
    0xab3d4648, 0xe242, 0x459f,
    {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};

// {2a59116d-6c4f-45e0-a74f-707e3fef9258}
const GUID IID_AudioPolicyConfigFactoryWin10 = {
    0x2a59116d, 0x6c4f, 0x45e0,
    {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}};

EndpointRouter::~EndpointRouter() {
    if (win11_) win11_->Release();
    if (win10_) win10_->Release();
}

HRESULT EndpointRouter::Initialize() {
    HString cls(kActivationClass);
    if (!cls.get()) return E_OUTOFMEMORY;

    // Probe the Windows 11 layout first. The IID determines which vtable the
    // factory hands back, so a successful QI here means the padding counts in
    // the Win11 struct are the correct ones.
    HRESULT hr = RoGetActivationFactory(
        cls.get(), IID_AudioPolicyConfigFactoryWin11,
        reinterpret_cast<void**>(&win11_));
    if (SUCCEEDED(hr) && win11_) return S_OK;

    win11_ = nullptr;
    hr = RoGetActivationFactory(
        cls.get(), IID_AudioPolicyConfigFactoryWin10,
        reinterpret_cast<void**>(&win10_));
    if (SUCCEEDED(hr) && win10_) return S_OK;

    win10_ = nullptr;
    return hr;
}

std::wstring EndpointRouter::MakePolicyDeviceId(const std::wstring& mmDeviceId,
                                                EDataFlow flow) {
    if (mmDeviceId.empty()) return {};

    // MMDevice ids look like:
    //   {0.0.0.00000000}.{guid}
    // This API wants the software-device path form instead. The trailing GUID
    // is a fixed well-known value that differs for render vs capture.
    const wchar_t* tail = (flow == eRender)
        ? L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}"
        : L"#{2eef81be-33fa-4800-9670-1cd474972c3f}";

    return L"\\\\?\\SWD#MMDEVAPI#" + mmDeviceId + tail;
}

HRESULT EndpointRouter::SetEndpoint(DWORD processId, const std::wstring& deviceId,
                                    EDataFlow flow, ERole role) {
    if (!win11_ && !win10_) return E_UNEXPECTED;  // Initialize() not called

    // An empty string is meaningful: it clears the override and returns the
    // process to following the system default. This is the restore path.
    HString id(deviceId);
    if (!deviceId.empty() && !id.get()) return E_OUTOFMEMORY;

    const UINT32 pid = static_cast<UINT32>(processId);
    return win11_
        ? win11_->SetPersistedDefaultAudioEndpoint(pid, flow, role, id.get())
        : win10_->SetPersistedDefaultAudioEndpoint(pid, flow, role, id.get());
}

HRESULT EndpointRouter::GetEndpoint(DWORD processId, std::wstring& deviceIdOut,
                                    EDataFlow flow, ERole role) {
    if (!win11_ && !win10_) return E_UNEXPECTED;  // Initialize() not called

    HString id;
    const UINT32 pid = static_cast<UINT32>(processId);
    HRESULT hr = win11_
        ? win11_->GetPersistedDefaultAudioEndpoint(pid, flow, role, id.put())
        : win10_->GetPersistedDefaultAudioEndpoint(pid, flow, role, id.put());

    if (SUCCEEDED(hr)) deviceIdOut = id.str();
    return hr;
}

} // namespace routing
