#include "session_finder.h"
#include "com_ptr.h"

#include <audiopolicy.h>
#include <psapi.h>
#include <algorithm>
#include <cwctype>

namespace sessions {
namespace {

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ProcessImagePath(DWORD pid) {
    if (pid == 0) return {};
    // LIMITED_INFORMATION is enough for the image name and, unlike
    // QUERY_INFORMATION, succeeds against processes at other integrity levels.
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};

    wchar_t buf[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &len);
    CloseHandle(h);
    return ok ? std::wstring(buf, len) : std::wstring();
}

} // namespace

std::vector<AudioSession> EnumerateRenderSessions(const std::wstring& deviceId) {
    std::vector<AudioSession> out;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator,
                                enumerator.put_void()))) {
        return out;
    }

    ComPtr<IMMDevice> device;
    if (deviceId.empty()) {
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia,
                                                       device.put())))
            return out;
    } else if (FAILED(enumerator->GetDevice(deviceId.c_str(), device.put()))) {
        return out;
    }

    ComPtr<IAudioSessionManager2> manager;
    if (FAILED(device->Activate(IID_IAudioSessionManager2, CLSCTX_ALL,
                                nullptr, manager.put_void()))) {
        return out;
    }

    ComPtr<IAudioSessionEnumerator> sessionEnum;
    if (FAILED(manager->GetSessionEnumerator(sessionEnum.put()))) return out;

    int count = 0;
    if (FAILED(sessionEnum->GetCount(&count))) return out;

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessionEnum->GetSession(i, control.put()))) continue;

        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(control.query(IID_IAudioSessionControl2, control2))) continue;

        // Skip the system sounds session; it has no meaningful owning process.
        if (control2->IsSystemSoundsSession() == S_OK) continue;

        AudioSession s;
        if (FAILED(control2->GetProcessId(&s.processId))) continue;
        s.executablePath = ProcessImagePath(s.processId);
        if (s.executablePath.empty()) continue;

        AudioSessionState state = AudioSessionStateInactive;
        if (SUCCEEDED(control2->GetState(&state))) {
            s.active = (state == AudioSessionStateActive);
        }

        LPWSTR name = nullptr;
        if (SUCCEEDED(control2->GetDisplayName(&name)) && name) {
            s.displayName = name;
            CoTaskMemFree(name);
        }

        out.push_back(std::move(s));
    }

    return out;
}

bool GetSessionVolume(DWORD pid, const std::wstring& deviceId, float& volumeOut) {
    if (pid == 0) return false;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator,
                                enumerator.put_void()))) {
        return false;
    }

    ComPtr<IMMDevice> device;
    if (deviceId.empty()) {
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia,
                                                       device.put())))
            return false;
    } else if (FAILED(enumerator->GetDevice(deviceId.c_str(), device.put()))) {
        return false;
    }

    ComPtr<IAudioSessionManager2> manager;
    if (FAILED(device->Activate(IID_IAudioSessionManager2, CLSCTX_ALL,
                                nullptr, manager.put_void()))) {
        return false;
    }

    ComPtr<IAudioSessionEnumerator> sessionEnum;
    if (FAILED(manager->GetSessionEnumerator(sessionEnum.put()))) return false;

    int count = 0;
    if (FAILED(sessionEnum->GetCount(&count))) return false;

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessionEnum->GetSession(i, control.put()))) continue;

        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(control.query(IID_IAudioSessionControl2, control2))) continue;

        DWORD owner = 0;
        if (FAILED(control2->GetProcessId(&owner)) || owner != pid) continue;

        ComPtr<ISimpleAudioVolume> volume;
        if (FAILED(control.query(IID_ISimpleAudioVolume, volume))) continue;

        float level = 1.0f;
        if (FAILED(volume->GetMasterVolume(&level))) continue;
        BOOL mute = FALSE;
        volume->GetMute(&mute);

        volumeOut = mute ? 0.0f : level;
        return true;
    }
    return false;
}

std::vector<AudioSession> FindByExecutable(const std::vector<std::wstring>& names) {
    std::vector<std::wstring> wanted;
    wanted.reserve(names.size());
    for (const auto& n : names) wanted.push_back(ToLower(FileNameOf(n)));

    std::vector<AudioSession> out;
    for (auto& s : EnumerateRenderSessions()) {
        const std::wstring exe = ToLower(FileNameOf(s.executablePath));
        if (std::find(wanted.begin(), wanted.end(), exe) != wanted.end()) {
            out.push_back(std::move(s));
        }
    }
    return out;
}

bool IsRenderingTo(DWORD pid, const std::wstring& deviceId) {
    for (const auto& s : EnumerateRenderSessions(deviceId))
        if (s.processId == pid && s.active) return true;
    return false;
}

DWORD FindAudioOwningPid(const std::vector<std::wstring>& names) {
    const auto matches = FindByExecutable(names);
    if (matches.empty()) return 0;

    for (const auto& s : matches) {
        if (s.active) return s.processId;
    }
    // Nothing active -- Discord is open but silent. Fall back to the first
    // match so routing can be pre-applied before a call starts.
    return matches.front().processId;
}

DWORD FindDiscordAudioPid() {
    return FindAudioOwningPid({
        L"Discord.exe",
        L"DiscordPTB.exe",
        L"DiscordCanary.exe",
        L"DiscordDevelopment.exe",
    });
}

} // namespace sessions
