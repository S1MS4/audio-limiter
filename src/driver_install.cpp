// Installs and removes the bundled virtual audio device.
//
// The driver is root-enumerated (ROOT\VirtualAudioDriver), which means
// staging the package with pnputil is only half the job -- nothing will
// enumerate it unless a device node is created first. This is the same
// sequence devcon.exe performs for "devcon install", done directly so no
// extra redistributable is needed.
//
// Requires administrator rights. The installer elevates just for this step.

#include "driver_install.h"

#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <string>
#include <vector>

namespace driver {
namespace {

constexpr wchar_t kHardwareId[] = L"ROOT\\VirtualAudioDriver";

// Hardware id lists are REG_MULTI_SZ: double-null terminated.
std::vector<wchar_t> MultiSz(const wchar_t* s) {
    const size_t n = wcslen(s);
    std::vector<wchar_t> v(n + 2, L'\0');
    memcpy(v.data(), s, n * sizeof(wchar_t));
    return v;
}

} // namespace

bool IsInstalled() {
    ULONG len = 0;
    auto ids = MultiSz(kHardwareId);
    const CONFIGRET cr = CM_Get_Device_ID_List_SizeW(
        &len, kHardwareId, CM_GETIDLIST_FILTER_ENUMERATOR);
    // A bare enumerator match is enough: if anything is present under this
    // hardware id, the device exists.
    return cr == CR_SUCCESS && len > 2;
}

bool Install(const std::wstring& infPath, std::wstring& error) {
    if (GetFileAttributesW(infPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"Driver files are missing from the installation folder.";
        return false;
    }

    GUID classGuid{};
    wchar_t className[MAX_CLASS_NAME_LEN] = {};
    if (!SetupDiGetINFClassW(infPath.c_str(), &classGuid, className,
                             MAX_CLASS_NAME_LEN, nullptr)) {
        error = L"Could not read the driver package.";
        return false;
    }

    HDEVINFO set = SetupDiCreateDeviceInfoList(&classGuid, nullptr);
    if (set == INVALID_HANDLE_VALUE) {
        error = L"Could not prepare the device list.";
        return false;
    }

    SP_DEVINFO_DATA dev{sizeof(SP_DEVINFO_DATA)};
    bool ok = SetupDiCreateDeviceInfoW(set, className, &classGuid, nullptr,
                                       nullptr, DICD_GENERATE_ID, &dev) != FALSE;

    if (ok) {
        auto ids = MultiSz(kHardwareId);
        ok = SetupDiSetDeviceRegistryPropertyW(
                 set, &dev, SPDRP_HARDWAREID, (const BYTE*)ids.data(),
                 (DWORD)(ids.size() * sizeof(wchar_t))) != FALSE;
    }
    if (ok) {
        // Creates the device node. Until this succeeds the machine has no
        // device for the staged driver to bind to.
        ok = SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev) != FALSE;
    }

    SetupDiDestroyDeviceInfoList(set);

    if (!ok) {
        error = L"Could not create the virtual audio device. "
                L"Administrator rights are required.";
        return false;
    }

    // Stage the package into the driver store first. Without this the device
    // node exists but has nothing to bind to, and Device Manager shows it in
    // an error state -- which UpdateDriverForPlugAndPlayDevices reports as
    // success, so the failure is silent unless we check afterwards.
    wchar_t storedInf[MAX_PATH] = {};
    if (!SetupCopyOEMInfW(infPath.c_str(), nullptr, SPOST_PATH, 0, storedInf,
                          MAX_PATH, nullptr, nullptr)) {
        const DWORD e = GetLastError();
        if (e != ERROR_FILE_EXISTS) {
            error = L"Windows rejected the driver package.";
            return false;
        }
    }

    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId,
                                            infPath.c_str(),
                                            INSTALLFLAG_FORCE, &reboot)) {
        error = L"The virtual audio device was created but the driver "
                L"could not be installed.";
        return false;
    }

    // Verify rather than trust: a device left in an error state is worse than
    // a clean failure, because the app will then find an endpoint that does
    // not actually carry audio.
    if (!IsHealthy()) {
        error = L"The virtual audio device installed but did not start. "
                L"A reboot may be required.";
        return false;
    }
    return true;
}

// True only when a device with our hardware id exists AND has started. The
// distinction matters: SetupAPI happily leaves a half-installed node behind.
bool IsHealthy() {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr,
                                        DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;

    bool healthy = false;
    SP_DEVINFO_DATA dev{sizeof(SP_DEVINFO_DATA)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        wchar_t ids[512] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID,
                                               nullptr, (PBYTE)ids, sizeof(ids),
                                               nullptr))
            continue;
        if (_wcsicmp(ids, kHardwareId) != 0) continue;

        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0) == CR_SUCCESS &&
            problem == 0) {
            healthy = true;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return healthy;
}

bool Uninstall(std::wstring& error) {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr,
                                        DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) {
        error = L"Could not enumerate devices.";
        return false;
    }

    bool removedAny = false;
    SP_DEVINFO_DATA dev{sizeof(SP_DEVINFO_DATA)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        wchar_t ids[512] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID,
                                               nullptr, (PBYTE)ids, sizeof(ids),
                                               nullptr))
            continue;
        if (_wcsicmp(ids, kHardwareId) != 0) continue;

        if (SetupDiCallClassInstaller(DIF_REMOVE, set, &dev))
            removedAny = true;
    }
    SetupDiDestroyDeviceInfoList(set);

    if (!removedAny) error = L"No virtual audio device was found to remove.";
    return removedAny;
}

} // namespace driver
