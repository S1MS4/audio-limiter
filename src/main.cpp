// M0 spike harness.
//
// Proves (or disproves) the two things the whole architecture rests on:
//   1. the undocumented routing API is reachable and actually moves an app
//   2. Discord's audio-owning PID can be identified reliably
//
// This is a throwaway CLI, not the product. The product is a tray app.

#include "audio_policy_config.h"
#include "session_finder.h"
#include "com_ptr.h"

#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace probe { int Run(DWORD pid); }

namespace {

// Mixing printf and wprintf on one stream orients it to bytes and silently
// truncates the wide output. Convert to UTF-8 and use narrow printf only.
std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

struct Endpoint {
    std::wstring id;
    std::wstring friendlyName;
    bool isDefault = false;
};

std::vector<Endpoint> EnumerateRenderEndpoints() {
    std::vector<Endpoint> out;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator,
                                enumerator.put_void()))) {
        return out;
    }

    std::wstring defaultId;
    ComPtr<IMMDevice> defaultDevice;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia,
                                                      defaultDevice.put()))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id)) && id) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                              collection.put()))) {
        return out;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put()))) continue;

        Endpoint e;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            e.id = id;
            CoTaskMemFree(id);
        }

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.put()))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) &&
                v.vt == VT_LPWSTR && v.pwszVal) {
                e.friendlyName = v.pwszVal;
            }
            PropVariantClear(&v);
        }

        e.isDefault = (!e.id.empty() && e.id == defaultId);
        out.push_back(std::move(e));
    }
    return out;
}

void PrintUsage() {
    std::puts(
        "M0 spike harness\n"
        "\n"
        "  devices              list active render endpoints\n"
        "  sessions             list audio sessions and owning processes\n"
        "  discord              identify Discord's audio-owning PID\n"
        "  get <pid>            read a process's current endpoint override\n"
        "  route <pid> <devid>  route a process to an endpoint\n"
        "  restore <pid>        clear the override (back to system default)\n"
        "\n"
        "<devid> is the raw endpoint id from `devices`; wrapping is automatic.\n");
}

int CmdDevices() {
    const auto endpoints = EnumerateRenderEndpoints();
    if (endpoints.empty()) {
        std::puts("no active render endpoints found");
        return 1;
    }
    for (const auto& e : endpoints) {
        std::printf("%s %s\n    %s\n", e.isDefault ? "[default]" : "         ",
                    U8(e.friendlyName).c_str(), U8(e.id).c_str());
    }
    return 0;
}

int CmdSessions() {
    const auto all = sessions::EnumerateRenderSessions();
    if (all.empty()) {
        std::puts("no audio sessions found (is anything playing?)");
        return 1;
    }
    for (const auto& s : all) {
        std::printf("pid %-6lu %-8s %s\n", s.processId,
                    s.active ? "ACTIVE" : "idle",
                    U8(s.executablePath).c_str());
    }
    return 0;
}

int CmdDiscord() {
    const auto matches = sessions::FindByExecutable({
        L"Discord.exe", L"DiscordPTB.exe", L"DiscordCanary.exe",
        L"DiscordDevelopment.exe"});

    if (matches.empty()) {
        std::puts("no Discord audio session found.\n"
                  "Discord must be running AND have produced audio at least "
                  "once since launch -- join a voice channel first.");
        return 1;
    }

    std::puts("Discord audio sessions:");
    for (const auto& s : matches) {
        std::printf("  pid %-6lu %-8s %s\n", s.processId,
                    s.active ? "ACTIVE" : "idle", U8(s.executablePath).c_str());
    }
    std::printf("\nselected pid: %lu\n", sessions::FindDiscordAudioPid());
    return 0;
}

int CmdGet(DWORD pid) {
    routing::EndpointRouter router;
    HRESULT hr = router.Initialize();
    if (FAILED(hr)) {
        std::printf("routing factory unavailable: hr=0x%08lX\n", hr);
        return 1;
    }
    std::printf("factory variant: %s\n",
                router.IsWin11Variant() ? "Windows 11 21H2+" : "Windows 10 pre-21H2");

    std::wstring current;
    hr = router.GetEndpoint(pid, current);
    if (FAILED(hr)) {
        std::printf("GetPersistedDefaultAudioEndpoint failed: hr=0x%08lX\n", hr);
        return 1;
    }
    if (current.empty()) {
        std::printf("pid %lu: no override (following system default)\n", pid);
    } else {
        std::printf("pid %lu: %s\n", pid, U8(current).c_str());
    }
    return 0;
}

int CmdRoute(DWORD pid, const std::wstring& rawDeviceId) {
    routing::EndpointRouter router;
    HRESULT hr = router.Initialize();
    if (FAILED(hr)) {
        std::printf("routing factory unavailable: hr=0x%08lX\n", hr);
        return 1;
    }

    // Capture prior state first. In the real app this gets persisted to disk
    // BEFORE the change, so a crash can never strand the user.
    std::wstring previous;
    router.GetEndpoint(pid, previous);
    std::printf("previous: %s\n",
                previous.empty() ? "(system default)" : U8(previous).c_str());

    const std::wstring policyId =
        rawDeviceId.empty() ? std::wstring()
                            : routing::EndpointRouter::MakePolicyDeviceId(rawDeviceId);

    hr = router.SetEndpoint(pid, policyId);
    if (FAILED(hr)) {
        std::printf("SetPersistedDefaultAudioEndpoint failed: hr=0x%08lX\n", hr);
        return 1;
    }

    std::wstring now;
    router.GetEndpoint(pid, now);
    std::printf("now:      %s\n",
                now.empty() ? "(system default)" : U8(now).c_str());
    std::puts("\nVerify in Settings > System > Sound > Volume mixer.");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { PrintUsage(); return 2; }

    SetConsoleOutputCP(CP_UTF8);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::printf("CoInitializeEx failed: hr=0x%08lX\n", hr);
        return 1;
    }

    const std::wstring cmd = argv[1];
    int rc = 2;

    if (cmd == L"devices") {
        rc = CmdDevices();
    } else if (cmd == L"sessions") {
        rc = CmdSessions();
    } else if (cmd == L"discord") {
        rc = CmdDiscord();
    } else if (cmd == L"get" && argc >= 3) {
        rc = CmdGet(static_cast<DWORD>(_wtoi(argv[2])));
    } else if (cmd == L"route" && argc >= 4) {
        rc = CmdRoute(static_cast<DWORD>(_wtoi(argv[2])), argv[3]);
    } else if (cmd == L"looptest" && argc >= 3) {
        rc = probe::Run(static_cast<DWORD>(_wtoi(argv[2])));
    } else if (cmd == L"restore" && argc >= 3) {
        rc = CmdRoute(static_cast<DWORD>(_wtoi(argv[2])), L"");
    } else {
        PrintUsage();
    }

    CoUninitialize();
    return rc;
}
