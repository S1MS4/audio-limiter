// M0 spike harness.
//
// Proves (or disproves) the two things the whole architecture rests on:
//   1. the undocumented routing API is reachable and actually moves an app
//   2. Discord's audio-owning PID can be identified reliably
//
// This is a throwaway CLI, not the product. The product is a tray app.

#include "audio_policy_config.h"
#include "session_finder.h"

#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Endpoint {
    std::wstring id;
    std::wstring friendlyName;
    bool isDefault = false;
};

std::vector<Endpoint> EnumerateRenderEndpoints() {
    std::vector<Endpoint> out;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        return out;
    }

    std::wstring defaultId;
    ComPtr<IMMDevice> defaultDevice;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia,
                                                      &defaultDevice))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id)) && id) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                              &collection))) {
        return out;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        Endpoint e;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            e.id = id;
            CoTaskMemFree(id);
        }

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
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
        std::wprintf(L"%s %s\n    %s\n", e.isDefault ? L"[default]" : L"         ",
                     e.friendlyName.c_str(), e.id.c_str());
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
        std::wprintf(L"pid %-6lu %-8s %s\n", s.processId,
                     s.active ? L"ACTIVE" : L"idle",
                     s.executablePath.c_str());
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
        std::wprintf(L"  pid %-6lu %-8s %s\n", s.processId,
                     s.active ? L"ACTIVE" : L"idle", s.executablePath.c_str());
    }
    std::wprintf(L"\nselected pid: %lu\n", sessions::FindDiscordAudioPid());
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
        std::wprintf(L"pid %lu: %s\n", pid, current.c_str());
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
    std::wprintf(L"previous: %s\n",
                 previous.empty() ? L"(system default)" : previous.c_str());

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
    std::wprintf(L"now:      %s\n", now.empty() ? L"(system default)" : now.c_str());
    std::puts("\nVerify in Settings > System > Sound > Volume mixer.");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { PrintUsage(); return 2; }

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
    } else if (cmd == L"restore" && argc >= 3) {
        rc = CmdRoute(static_cast<DWORD>(_wtoi(argv[2])), L"");
    } else {
        PrintUsage();
    }

    CoUninitialize();
    return rc;
}
