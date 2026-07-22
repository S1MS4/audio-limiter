// Audio Limiter -- tray application.
//
// The entire user-facing product. It must do the right thing with no
// configuration, and it must never leave the user's audio broken, including
// when it crashes.

#include "audio_engine.h"
#include "audio_policy_config.h"
#include "session_finder.h"
#include "com_ptr.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <cmath>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr wchar_t kAppName[] = L"Audio Limiter";
constexpr wchar_t kWndClass[] = L"AudioLimiterTrayWnd";
constexpr UINT WM_TRAYICON = WM_APP + 1;

enum : UINT {
    IDM_TOGGLE = 1000,
    IDM_SETTINGS,
    IDM_AUTOSTART,
    IDM_EXIT,
};

enum : int {
    IDC_CEILING = 2000,
    IDC_OUTPUT,
    IDC_VIRTUAL,
    IDC_STATUS,
    IDC_CEILING_LABEL,
    IDC_APPLY,
};

struct Device {
    std::wstring id;
    std::wstring name;
};

// ---------------------------------------------------------------- helpers

std::wstring AppDataPath(const wchar_t* file) {
    wchar_t* base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base))) {
        dir = base;
        CoTaskMemFree(base);
    }
    dir += L"\\AudioLimiter";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + file;
}

std::wstring ReadFileW(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    DWORD size = GetFileSize(h, nullptr);
    std::string bytes(size, '\0');
    DWORD got = 0;
    ReadFile(h, bytes.data(), size, &got, nullptr);
    CloseHandle(h);
    if (got == 0) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)got, nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)got, out.data(), n);
    return out;
}

void WriteFileW(const std::wstring& path, const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                      nullptr, 0, nullptr, nullptr);
    std::string bytes(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                        bytes.data(), n, nullptr, nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr);
    FlushFileBuffers(h);   // survive a hard power loss, not just a crash
    CloseHandle(h);
}

std::vector<Device> EnumRenderDevices() {
    std::vector<Device> out;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator, en.put_void())))
        return out;

    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, col.put())))
        return out;

    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(col->Item(i, d.put()))) continue;
        Device dev;
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id)) && id) { dev.id = id; CoTaskMemFree(id); }
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, ps.put()))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) &&
                v.vt == VT_LPWSTR && v.pwszVal)
                dev.name = v.pwszVal;
            PropVariantClear(&v);
        }
        if (!dev.id.empty()) out.push_back(std::move(dev));
    }
    return out;
}

std::wstring DefaultRenderId() {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator, en.put_void())))
        return {};
    ComPtr<IMMDevice> d;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, d.put()))) return {};
    LPWSTR id = nullptr;
    std::wstring out;
    if (SUCCEEDED(d->GetId(&id)) && id) { out = id; CoTaskMemFree(id); }
    return out;
}

// A virtual endpoint is one whose name matches a known virtual audio driver.
// Discord gets routed into it; nothing physical ever plays it.
bool LooksVirtual(const std::wstring& name) {
    static const wchar_t* kMarkers[] = {
        L"VB-Audio", L"Voicemeeter", L"VAIO", L"CABLE",
        L"Virtual Audio", L"Virtual Cable", L"AudioMirror", L"VAC",
    };
    for (const wchar_t* m : kMarkers)
        if (StrStrIW(name.c_str(), m)) return true;
    return false;
}


// ---------------------------------------------------------------- autostart

constexpr wchar_t kRunKey[] = L"Software\Microsoft\Windows\CurrentVersion\Run";
constexpr wchar_t kRunValue[] = L"AudioLimiter";

std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

bool IsAutoStart() {
    HKEY k{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    wchar_t val[MAX_PATH * 2] = {};
    DWORD sz = sizeof(val), type = 0;
    const LONG r = RegQueryValueExW(k, kRunValue, nullptr, &type, (LPBYTE)val, &sz);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && type == REG_SZ && val[0] != 0;
}

void SetAutoStart(bool on) {
    HKEY k{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        // Quoted: the install path contains spaces under Program Files, and an
        // unquoted Run value would be parsed as a different executable.
        const std::wstring cmd = L""" + ExePath() + L""";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- state

struct Settings {
    float ceilingDb = -6.0f;
    float releaseMs = 250.0f;
    std::wstring outputDeviceId;    // empty = system default
    std::wstring virtualDeviceId;   // empty = auto-detect
    bool enabled = true;
};

Settings g_settings;
engine::AudioEngine g_engine;
routing::EndpointRouter g_router;
HWND g_hwnd = nullptr;
HWND g_settingsWnd = nullptr;
NOTIFYICONDATAW g_nid = {};
DWORD g_routedPid = 0;
bool g_active = false;
std::vector<Device> g_dlgDevices;

std::wstring SettingsPath() { return AppDataPath(L"settings.ini"); }
std::wstring RoutingStatePath() { return AppDataPath(L"routing.state"); }

void LoadSettings() {
    const std::wstring txt = ReadFileW(SettingsPath());
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t eol = txt.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = txt.size();
        std::wstring line = txt.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' '))
            line.pop_back();
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == L"ceiling") g_settings.ceilingDb = (float)_wtof(v.c_str());
        else if (k == L"release") g_settings.releaseMs = (float)_wtof(v.c_str());
        else if (k == L"output") g_settings.outputDeviceId = v;
        else if (k == L"virtual") g_settings.virtualDeviceId = v;
        else if (k == L"enabled") g_settings.enabled = (v != L"0");
    }
}

void SaveSettings() {
    std::wstring t;
    t += L"ceiling=" + std::to_wstring(g_settings.ceilingDb) + L"\n";
    t += L"release=" + std::to_wstring(g_settings.releaseMs) + L"\n";
    t += L"output=" + g_settings.outputDeviceId + L"\n";
    t += L"virtual=" + g_settings.virtualDeviceId + L"\n";
    t += L"enabled=" + std::wstring(g_settings.enabled ? L"1" : L"0") + L"\n";
    WriteFileW(SettingsPath(), t);
}

// Crash recovery. The state file records that we changed someone's routing
// and what it was before. If we find one at startup, a previous run died
// without cleaning up and the user's Discord is currently silent.
void RecoverStaleRouting() {
    const std::wstring state = ReadFileW(RoutingStatePath());
    if (state.empty()) return;

    const size_t nl = state.find(L'\n');
    if (nl == std::wstring::npos) { DeleteFileW(RoutingStatePath().c_str()); return; }

    const DWORD pid = (DWORD)_wtoi(state.substr(0, nl).c_str());
    std::wstring prev = state.substr(nl + 1);
    while (!prev.empty() && (prev.back() == L'\n' || prev.back() == L'\r'))
        prev.pop_back();

    if (pid != 0) g_router.SetEndpoint(pid, prev);
    DeleteFileW(RoutingStatePath().c_str());
}

void ClearRouting() {
    if (g_routedPid != 0) {
        g_router.SetEndpoint(g_routedPid, L"");
        g_routedPid = 0;
    }
    DeleteFileW(RoutingStatePath().c_str());
}

std::wstring PickVirtualDevice(const std::vector<Device>& devs) {
    if (!g_settings.virtualDeviceId.empty()) return g_settings.virtualDeviceId;
    for (const auto& d : devs)
        if (LooksVirtual(d.name)) return d.id;
    return {};
}

void UpdateTrayIcon();
void SaveSettings();
std::wstring DefaultRenderId();
std::vector<Device> EnumRenderDevices();
std::wstring PickVirtualDevice(const std::vector<Device>&);
void StopProtection();
bool StartProtection(std::wstring&);

bool StartProtection(std::wstring& errorOut) {
    const auto devices = EnumRenderDevices();
    const std::wstring virt = PickVirtualDevice(devices);
    if (virt.empty()) {
        errorOut = L"No virtual audio device found.\n\n"
                   L"Audio Limiter needs one to redirect Discord's sound "
                   L"through. Install VB-CABLE (free) and start again.";
        return false;
    }

    std::wstring out = g_settings.outputDeviceId;
    if (out.empty()) out = DefaultRenderId();
    if (out == virt) {
        errorOut = L"Your output device is the virtual device.\n\n"
                   L"Pick your headphones or speakers in Settings.";
        return false;
    }

    const DWORD pid = sessions::FindDiscordAudioPid();
    if (pid == 0) {
        errorOut = L"Discord is not playing any audio yet.\n\n"
                   L"Open Discord and join a voice channel, then try again.";
        return false;
    }

    // Record intent BEFORE changing anything, so a crash between these two
    // lines is still recoverable on next launch.
    std::wstring previous;
    g_router.GetEndpoint(pid, previous);
    WriteFileW(RoutingStatePath(), std::to_wstring(pid) + L"\n" + previous);

    const std::wstring policyId =
        routing::EndpointRouter::MakePolicyDeviceId(virt);
    if (FAILED(g_router.SetEndpoint(pid, policyId))) {
        DeleteFileW(RoutingStatePath().c_str());
        errorOut = L"Windows would not let us redirect Discord's audio.";
        return false;
    }
    g_routedPid = pid;

    engine::Config cfg;
    cfg.captureDeviceId = virt;
    cfg.renderDeviceId = out;
    cfg.ceilingDb = g_settings.ceilingDb;
    cfg.releaseMs = g_settings.releaseMs;
    cfg.lookaheadMs = 5.0f;

    if (FAILED(g_engine.Start(cfg))) {
        ClearRouting();
        errorOut = g_engine.lastError().empty() ? L"Could not start audio."
                                                : g_engine.lastError();
        return false;
    }

    g_active = true;
    UpdateTrayIcon();
    return true;
}

void StopProtection() {
    g_engine.Stop();
    ClearRouting();
    g_active = false;
    UpdateTrayIcon();
}

// ---------------------------------------------------------------- tray

HICON MakeDotIcon(COLORREF colour) {
    const int sz = 32;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP colourBmp = CreateCompatibleBitmap(screen, sz, sz);
    HBITMAP maskBmp = CreateBitmap(sz, sz, 1, 1, nullptr);

    HGDIOBJ old = SelectObject(dc, colourBmp);
    RECT r{0, 0, sz, sz};
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &r, bg);
    DeleteObject(bg);

    HBRUSH fill = CreateSolidBrush(colour);
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(20, 20, 20));
    HGDIOBJ ob = SelectObject(dc, fill);
    HGDIOBJ op = SelectObject(dc, pen);
    Ellipse(dc, 4, 4, sz - 4, sz - 4);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(fill);
    DeleteObject(pen);
    SelectObject(dc, old);

    // Mask: black where the icon should show.
    HDC mdc = CreateCompatibleDC(screen);
    HGDIOBJ mold = SelectObject(mdc, maskBmp);
    RECT mr{0, 0, sz, sz};
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(mdc, &mr, white);
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    HGDIOBJ mb = SelectObject(mdc, black);
    Ellipse(mdc, 4, 4, sz - 4, sz - 4);
    SelectObject(mdc, mb);
    SelectObject(mdc, mold);
    DeleteDC(mdc);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = maskBmp;
    ii.hbmColor = colourBmp;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(colourBmp);
    DeleteObject(maskBmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

void UpdateTrayIcon() {
    static HICON on = nullptr, off = nullptr;
    if (!on) on = MakeDotIcon(RGB(34, 197, 94));    // green
    if (!off) off = MakeDotIcon(RGB(120, 120, 120)); // grey

    g_nid.hIcon = g_active ? on : off;
    wcscpy_s(g_nid.szTip, g_active ? L"Audio Limiter -- protecting"
                                   : L"Audio Limiter -- off");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void ShowBalloon(const wchar_t* title, const wchar_t* text, DWORD flags) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = flags;
    wcscpy_s(n.szInfoTitle, title);
    wcscpy_s(n.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ---------------------------------------------------------------- settings UI
//
// Fully owner-drawn. Stock Win32 controls cannot be themed convincingly --
// trackbars and combo boxes ignore background colour and render a grey 1990s
// widget on a dark surface. Drawing everything costs a few hundred lines and
// is the only way to get a coherent look.

namespace ui {

// Warm palette. Deliberately not the usual cool-grey dark theme -- the browns
// and the terracotta accent are what give it character next to Discord.
constexpr COLORREF kBg      = RGB(0x2B, 0x27, 0x23);
constexpr COLORREF kCard    = RGB(0x35, 0x30, 0x2B);
constexpr COLORREF kInset   = RGB(0x30, 0x2B, 0x27);
constexpr COLORREF kField   = RGB(0x3A, 0x34, 0x2E);
constexpr COLORREF kHover   = RGB(0x43, 0x3C, 0x35);
constexpr COLORREF kFooter  = RGB(0x27, 0x23, 0x20);
constexpr COLORREF kTrack   = RGB(0x4A, 0x42, 0x3B);
constexpr COLORREF kAccent  = RGB(0xE0, 0x88, 0x5A);   // terracotta
constexpr COLORREF kSage    = RGB(0xA9, 0xBC, 0x8C);   // muted green
constexpr COLORREF kSageDim = RGB(0x6F, 0x82, 0x58);
constexpr COLORREF kText    = RGB(0xF5, 0xF0, 0xE8);   // cream
constexpr COLORREF kMuted   = RGB(0xA6, 0x9A, 0x8C);
constexpr COLORREF kFaint   = RGB(0x7A, 0x6E, 0x63);

constexpr int kW = 400, kH = 470;

struct Fonts {
    HFONT serifTitle{}, serifValue{}, label{}, body{}, small_{}, bodyBold{};
    void Create(int dpi) {
        auto mk = [&](const wchar_t* face, const wchar_t* fallback, int pt,
                      int weight) {
            LOGFONTW lf{};
            lf.lfHeight = -MulDiv(pt, dpi, 72);
            lf.lfWeight = weight;
            lf.lfQuality = CLEARTYPE_QUALITY;
            wcscpy_s(lf.lfFaceName, face);
            HFONT f = CreateFontIndirectW(&lf);
            if (!f) {
                wcscpy_s(lf.lfFaceName, fallback);
                f = CreateFontIndirectW(&lf);
            }
            return f;
        };
        // Georgia is on every Windows install and carries the warm, slightly
        // heavy feel the design calls for far better than a UI sans.
        serifTitle = mk(L"Georgia", L"Times New Roman", 16, FW_BOLD);
        serifValue = mk(L"Georgia", L"Times New Roman", 26, FW_BOLD);
        label      = mk(L"Segoe UI", L"Tahoma", 8, FW_SEMIBOLD);
        body       = mk(L"Segoe UI", L"Tahoma", 10, FW_NORMAL);
        bodyBold   = mk(L"Segoe UI", L"Tahoma", 9, FW_SEMIBOLD);
        small_     = mk(L"Segoe UI", L"Tahoma", 8, FW_NORMAL);
    }
    void Destroy() {
        for (HFONT* f : {&serifTitle, &serifValue, &label, &body, &bodyBold,
                         &small_})
            if (*f) { DeleteObject(*f); *f = nullptr; }
    }
};

Fonts g_fonts;
int g_dpi = 96;
int g_clientW = kW;
int S(int v) { return MulDiv(v, g_clientW, kW); }

RECT g_rSlider{}, g_rOutput{}, g_rVirtual{}, g_rToggle{}, g_rAdvanced{};
bool g_dragging = false;
int g_hot = 0;   // 1 output, 2 virtual, 3 toggle, 4 advanced

void ApplySize(HWND h, int x, int y);

void FillRound(HDC dc, const RECT& r, int radius, COLORREF fill) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

void FillCircle(HDC dc, int cx, int cy, int r, COLORREF fill) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

void Text(HDC dc, const wchar_t* s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(dc, of);
}

SIZE Measure(HDC dc, const wchar_t* s, HFONT f) {
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, of);
    return sz;
}

std::wstring DeviceLabel(const std::wstring& id) {
    for (const auto& d : g_dlgDevices)
        if (d.id == id) return d.name;
    return L"Default";
}

void Paint(HWND h) {
    PAINTSTRUCT ps;
    HDC front = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);

    // Authored against kW and scaled by the real client width, so the layout
    // can never overflow the window it is drawn into.
    if (rc.right != g_clientW && rc.right > 0) {
        g_clientW = rc.right;
        g_fonts.Destroy();
        g_fonts.Create(MulDiv(96, g_clientW, kW));
    }

    HDC dc = CreateCompatibleDC(front);
    HBITMAP bmp = CreateCompatibleBitmap(front, rc.right, rc.bottom);
    HGDIOBJ obmp = SelectObject(dc, bmp);

    HBRUSH bg = CreateSolidBrush(kCard);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    const int pad = S(24);
    const auto& st = g_engine.stats();
    const bool on = g_active && st.running.load();

    // --- header -----------------------------------------------------------
    {
        const int badge = S(38);
        RECT rb{pad, S(20), pad + badge, S(20) + badge};
        FillRound(dc, rb, S(12), kSageDim);

        // Shield glyph, drawn rather than shipped as an icon resource.
        HPEN sp = CreatePen(PS_SOLID, S(2), kText);
        HGDIOBJ osp = SelectObject(dc, sp);
        HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        const int cx = (rb.left + rb.right) / 2, cy = (rb.top + rb.bottom) / 2;
        const int w = S(7), t = S(9);
        POINT shield[] = {{cx - w, cy - t}, {cx + w, cy - t},
                          {cx + w, cy + S(1)}, {cx, cy + t}, {cx - w, cy + S(1)}};
        Polygon(dc, shield, 5);
        SelectObject(dc, osp); SelectObject(dc, obr);
        DeleteObject(sp);

        RECT rt{rb.right + S(14), S(21), rc.right - S(90), S(45)};
        Text(dc, L"Audio Limiter", rt, g_fonts.serifTitle, kText,
             DT_LEFT | DT_TOP);

        RECT rs{rb.right + S(14), S(44), rc.right - S(90), S(62)};
        Text(dc, on ? L"Protecting" : L"Paused", rs, g_fonts.small_,
             on ? kAccent : kFaint, DT_LEFT | DT_TOP);

        // Toggle switch.
        const int tw = S(52), th = S(28);
        g_rToggle = {rc.right - pad - tw, S(25), rc.right - pad, S(25) + th};
        FillRound(dc, g_rToggle, th, on ? kSageDim : kTrack);
        const int kr = (th - S(6)) / 2;
        const int kx = on ? g_rToggle.right - S(3) - kr : g_rToggle.left + S(3) + kr;
        FillCircle(dc, kx, (g_rToggle.top + g_rToggle.bottom) / 2, kr,
                   on ? kText : kMuted);
    }

    // --- ceiling ----------------------------------------------------------
    {
        RECT rl{pad, S(84), rc.right / 2, S(102)};
        Text(dc, L"CEILING", rl, g_fonts.label, kMuted, DT_LEFT | DT_TOP);

        wchar_t val[16];
        swprintf_s(val, L"%d", (int)g_settings.ceilingDb);
        const SIZE vs = Measure(dc, val, g_fonts.serifValue);
        const SIZE us = Measure(dc, L"dB", g_fonts.small_);

        RECT ru{rc.right - pad - us.cx, S(90), rc.right - pad, S(112)};
        Text(dc, L"dB", ru, g_fonts.small_, kMuted, DT_RIGHT | DT_TOP);

        RECT rv{rc.right - pad - us.cx - S(4) - vs.cx, S(74),
                rc.right - pad - us.cx - S(4), S(112)};
        Text(dc, val, rv, g_fonts.serifValue, kAccent, DT_RIGHT | DT_TOP);
    }

    // --- slider -----------------------------------------------------------
    {
        const int y = S(122), th = S(6);
        g_rSlider = {pad, y - S(12), rc.right - pad, y + th + S(12)};
        RECT tr{pad, y, rc.right - pad, y + th};
        FillRound(dc, tr, th, kTrack);

        const float t = (g_settings.ceilingDb + 40.0f) / 40.0f;
        const int span = tr.right - tr.left;
        const int fx = tr.left + (int)(span * (t < 0 ? 0 : t > 1 ? 1 : t));
        RECT fr{tr.left, y, fx, y + th};
        if (fr.right > fr.left + 2) FillRound(dc, fr, th, kAccent);
        FillCircle(dc, fx, y + th / 2, S(8), kAccent);

        RECT rq{pad, y + S(14), rc.right - pad, y + S(32)};
        Text(dc, L"Stricter", rq, g_fonts.small_, kFaint, DT_LEFT | DT_TOP);
        Text(dc, L"Louder allowed", rq, g_fonts.small_, kFaint, DT_RIGHT | DT_TOP);
    }

    // --- status inset -----------------------------------------------------
    {
        RECT ri{pad, S(178), rc.right - pad, S(250)};
        FillRound(dc, ri, S(12), kInset);

        const float in = st.inputPeakDb.load();
        const float gr = -st.gainReductionDb.load();

        const wchar_t* msg;
        COLORREF dot;
        if (!on)                    { msg = L"Paused";                            dot = kFaint; }
        else if (gr > 0.5f)         { msg = L"Limiting — holding it down";    dot = kAccent; }
        else if (in <= -59.0f)      { msg = L"Listening — nothing playing";   dot = kSage; }
        else                        { msg = L"Listening — under the ceiling"; dot = kSage; }

        FillCircle(dc, ri.left + S(18), ri.top + S(21), S(4), dot);
        RECT rm{ri.left + S(30), ri.top + S(13), ri.right - S(14), ri.top + S(32)};
        Text(dc, msg, rm, g_fonts.bodyBold, kText, DT_LEFT | DT_TOP);

        // Level meter with a tick marking where the ceiling sits.
        const int my = ri.top + S(44), mh = S(6);
        RECT mr{ri.left + S(18), my, ri.right - S(18), my + mh};
        FillRound(dc, mr, mh, kTrack);

        float lvl = in < -60.0f ? -60.0f : in;
        const float lt = (lvl + 60.0f) / 60.0f;
        const int lw = (int)((mr.right - mr.left) * lt);
        if (on && lw > 2) {
            RECT lr{mr.left, my, mr.left + lw, my + mh};
            FillRound(dc, lr, mh, gr > 0.5f ? kAccent : kSage);
        }

        const float ct = (g_settings.ceilingDb + 60.0f) / 60.0f;
        const int cxp = mr.left + (int)((mr.right - mr.left) *
                                        (ct < 0 ? 0 : ct > 1 ? 1 : ct));
        HPEN tp = CreatePen(PS_SOLID, S(2), kText);
        HGDIOBJ otp = SelectObject(dc, tp);
        MoveToEx(dc, cxp, my - S(3), nullptr);
        LineTo(dc, cxp, my + mh + S(3));
        SelectObject(dc, otp);
        DeleteObject(tp);
    }

    // --- device fields ----------------------------------------------------
    auto field = [&](RECT& out, int y, const wchar_t* cap,
                     const std::wstring& v, int hotId) {
        RECT rl{pad, y, rc.right - pad, y + S(16)};
        Text(dc, cap, rl, g_fonts.label, kMuted, DT_LEFT | DT_TOP);

        out = {pad, y + S(20), rc.right - pad, y + S(20) + S(38)};
        FillRound(dc, out, S(19), g_hot == hotId ? kHover : kField);

        RECT rv{out.left + S(16), out.top, out.right - S(28), out.bottom};
        Text(dc, v.c_str(), rv, g_fonts.body, kText,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    };

    const std::wstring outName = g_settings.outputDeviceId.empty()
                                     ? DeviceLabel(DefaultRenderId())
                                     : DeviceLabel(g_settings.outputDeviceId);
    const std::wstring virtName = DeviceLabel(PickVirtualDevice(g_dlgDevices));

    field(g_rOutput, S(266), L"YOU HEAR AUDIO THROUGH", outName, 1);
    field(g_rVirtual, S(336), L"DISCORD IS REDIRECTED INTO", virtName, 2);

    // --- footer -----------------------------------------------------------
    {
        RECT rf{0, S(414), rc.right, rc.bottom};
        HBRUSH fb = CreateSolidBrush(kFooter);
        FillRect(dc, &rf, fb);
        DeleteObject(fb);

        wchar_t info[128];
        swprintf_s(info, L"%s  ·  ~%.0f ms latency",
                   IsAutoStart() ? L"Starts with Windows" : L"Manual start",
                   5.0 + (st.sampleRate.load()
                              ? st.periodFrames.load() * 1000.0 /
                                    st.sampleRate.load()
                              : 10.0));
        RECT ri2{pad, rf.top + S(18), rc.right / 2 + S(60), rf.bottom};
        Text(dc, info, ri2, g_fonts.small_, kFaint, DT_LEFT | DT_TOP);

        const SIZE as = Measure(dc, L"Advanced...", g_fonts.bodyBold);
        g_rAdvanced = {rc.right - pad - as.cx, rf.top + S(14),
                       rc.right - pad, rf.top + S(36)};
        Text(dc, L"Advanced...", g_rAdvanced, g_fonts.bodyBold,
             g_hot == 4 ? kText : kAccent, DT_RIGHT | DT_TOP);
    }

    BitBlt(front, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

void ApplySize(HWND h, int x, int y) {
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST),
                                MDT_EFFECTIVE_DPI, &dx, &dy)))
        dx = 96;
    const int dpi = (int)dx;
    const DWORD style = (DWORD)GetWindowLongPtrW(h, GWL_STYLE);
    RECT r{0, 0, MulDiv(kW, dpi, 96), MulDiv(kH, dpi, 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, (UINT)dpi);
    SetWindowPos(h, nullptr, x, y, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void SetCeilingFromX(HWND h, int x) {
    const int span = g_rSlider.right - g_rSlider.left;
    if (span <= 0) return;
    float t = (float)(x - g_rSlider.left) / (float)span;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const int db = (int)std::lround(-40.0f + t * 40.0f);
    if ((int)g_settings.ceilingDb != db) {
        g_settings.ceilingDb = (float)db;
        g_engine.SetCeilingDb(g_settings.ceilingDb);
        InvalidateRect(h, nullptr, FALSE);
    }
}

// Device chooser. A native popup menu rather than a combo box: it already
// matches the system theme and needs no owner-draw of its own.
void ChooseDevice(HWND h, bool forOutput) {
    g_dlgDevices = EnumRenderDevices();
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < g_dlgDevices.size(); ++i)
        AppendMenuW(m, MF_STRING, 100 + i, g_dlgDevices[i].name.c_str());

    RECT anchor = forOutput ? g_rOutput : g_rVirtual;
    POINT p{anchor.left, anchor.bottom};
    ClientToScreen(h, &p);
    SetForegroundWindow(h);
    const int sel = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN,
                                        p.x, p.y, 0, h, nullptr);
    DestroyMenu(m);
    if (sel < 100) return;

    const size_t idx = (size_t)(sel - 100);
    if (idx >= g_dlgDevices.size()) return;

    if (forOutput) g_settings.outputDeviceId = g_dlgDevices[idx].id;
    else g_settings.virtualDeviceId = g_dlgDevices[idx].id;
    SaveSettings();

    if (g_active) {
        StopProtection();
        std::wstring err;
        if (!StartProtection(err))
            MessageBoxW(h, err.c_str(), kAppName, MB_ICONWARNING);
    }
    InvalidateRect(h, nullptr, FALSE);
}

void ShowAdvanced(HWND h) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (IsAutoStart() ? MF_CHECKED : 0), 1,
                L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2, L"Open settings folder");
    AppendMenuW(m, MF_STRING, 3, L"About");

    POINT p{g_rAdvanced.right, g_rAdvanced.top};
    ClientToScreen(h, &p);
    SetForegroundWindow(h);
    const int sel = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN,
                                        p.x, p.y, 0, h, nullptr);
    DestroyMenu(m);

    if (sel == 1) SetAutoStart(!IsAutoStart());
    else if (sel == 2) ShellExecuteW(h, L"open", AppDataPath(L"").c_str(),
                                     nullptr, nullptr, SW_SHOWNORMAL);
    else if (sel == 3)
        MessageBoxW(h,
                    L"Audio Limiter\n\n"
                    L"A volume ceiling for Discord voice.\n"
                    L"MIT licensed — github.com/S1MS4/audio-limiter",
                    kAppName, MB_ICONINFORMATION);
    InvalidateRect(h, nullptr, FALSE);
}

void ToggleProtection(HWND h) {
    if (g_active) {
        StopProtection();
        g_settings.enabled = false;
    } else {
        std::wstring err;
        if (StartProtection(err)) g_settings.enabled = true;
        else MessageBoxW(h, err.c_str(), kAppName, MB_ICONINFORMATION);
    }
    SaveSettings();
    InvalidateRect(h, nullptr, FALSE);
}

} // namespace ui

LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        ui::g_dpi = (int)GetDpiForWindow(h);
        ui::g_fonts.Create(ui::g_dpi);
        g_dlgDevices = EnumRenderDevices();
        SetTimer(h, 1, 66, nullptr);   // ~15 fps, enough for a level meter
        return 0;

    case WM_ERASEBKGND:
        return 1;   // fully painted in WM_PAINT; erasing first only flickers
    case WM_PAINT:
        ui::Paint(h);
        return 0;

    case WM_TIMER: {
        // Correct the frame here rather than in WM_PAINT: SetWindowPos during
        // painting reenters and gets dropped, leaving the frame at the DPI the
        // window had before it was placed on its monitor.
        const int live = (int)GetDpiForWindow(h);
        if (live != ui::g_dpi) {
            ui::g_dpi = live;
            ui::g_fonts.Destroy();
            ui::g_fonts.Create(ui::g_dpi);
        }
        UINT mdx = 96, mdy = 96;
        GetDpiForMonitor(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST),
                         MDT_EFFECTIVE_DPI, &mdx, &mdy);
        RECT cr{}; GetClientRect(h, &cr);
        if (cr.right != MulDiv(ui::kW, (int)mdx, 96)) {
            RECT wr{}; GetWindowRect(h, &wr);
            ui::ApplySize(h, wr.left, wr.top);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&ui::g_rSlider, p)) {
            ui::g_dragging = true;
            SetCapture(h);
            ui::SetCeilingFromX(h, p.x);
        } else if (PtInRect(&ui::g_rOutput, p)) {
            ui::ChooseDevice(h, true);
        } else if (PtInRect(&ui::g_rVirtual, p)) {
            ui::ChooseDevice(h, false);
        } else if (PtInRect(&ui::g_rToggle, p)) {
            ui::ToggleProtection(h);
        } else if (PtInRect(&ui::g_rAdvanced, p)) {
            ui::ShowAdvanced(h);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (ui::g_dragging) { ui::SetCeilingFromX(h, p.x); return 0; }

        const int was = ui::g_hot;
        ui::g_hot = PtInRect(&ui::g_rOutput, p)     ? 1
                    : PtInRect(&ui::g_rVirtual, p)  ? 2
                    : PtInRect(&ui::g_rToggle, p)   ? 3
                    : PtInRect(&ui::g_rAdvanced, p) ? 4
                                                    : 0;
        if (was != ui::g_hot) {
            InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
        }
        SetCursor(LoadCursor(nullptr, ui::g_hot ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        ui::g_hot = 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_LBUTTONUP:
        if (ui::g_dragging) {
            ui::g_dragging = false;
            ReleaseCapture();
            SaveSettings();
        }
        return 0;

    case WM_DPICHANGED: {
        ui::g_dpi = HIWORD(wp);
        ui::g_fonts.Destroy();
        ui::g_fonts.Create(ui::g_dpi);
        // Windows suggests a rect by scaling the old frame. That is wrong
        // here because the layout is authored at a fixed design size, so
        // recompute the frame from kW/kH at the new DPI instead.
        const RECT* sug = (const RECT*)lp;
        ui::ApplySize(h, sug->left, sug->top);
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        ui::g_fonts.Destroy();
        SaveSettings();
        g_settingsWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ShowSettings() {
    if (g_settingsWnd) { SetForegroundWindow(g_settingsWnd); return; }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"AudioLimiterSettings";
        RegisterClassExW(&wc);
        registered = true;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g_settingsWnd = CreateWindowExW(
        WS_EX_APPWINDOW, L"AudioLimiterSettings", L"Audio Limiter", style,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_settingsWnd) return;

    // Dark title bar to match the client area. Ignored before Windows 10
    // 2004, which simply leaves a light caption -- acceptable degradation.
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(g_settingsWnd, 20 /*USE_IMMERSIVE_DARK_MODE*/,
                          &dark, sizeof(dark));

    ShowWindow(g_settingsWnd, SW_SHOW);

    // Only now is the window on a monitor, so only now is its DPI real.
    ui::g_dpi = (int)GetDpiForWindow(g_settingsWnd);
    ui::g_fonts.Destroy();
    ui::g_fonts.Create(ui::g_dpi);
    RECT cur{};
    GetWindowRect(g_settingsWnd, &cur);
    ui::ApplySize(g_settingsWnd, cur.left, cur.top);
    InvalidateRect(g_settingsWnd, nullptr, TRUE);
    SetForegroundWindow(g_settingsWnd);
}

// ---------------------------------------------------------------- tray window

LRESULT CALLBACK TrayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) { ShowSettings(); return 0; }
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING | (g_active ? MF_CHECKED : 0), IDM_TOGGLE,
                        g_active ? L"Protection on" : L"Protection off");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Settings...");
            AppendMenuW(m, MF_STRING | (IsAutoStart() ? MF_CHECKED : 0),
                        IDM_AUTOSTART, L"Start with Windows");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
            POINT p; GetCursorPos(&p);
            SetForegroundWindow(h);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, h, nullptr);
            DestroyMenu(m);
            return 0;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TOGGLE:
            if (g_active) {
                StopProtection();
                g_settings.enabled = false;
            } else {
                std::wstring err;
                if (StartProtection(err)) {
                    g_settings.enabled = true;
                } else {
                    MessageBoxW(h, err.c_str(), kAppName, MB_ICONINFORMATION);
                }
            }
            SaveSettings();
            return 0;
        case IDM_SETTINGS: ShowSettings(); return 0;
        case IDM_AUTOSTART: SetAutoStart(!IsAutoStart()); return 0;
        case IDM_EXIT: DestroyWindow(h); return 0;
        }
        return 0;

    // Windows asks politely before killing us. Restore routing now, because
    // after this returns we may not run again.
    case WM_QUERYENDSESSION:
        StopProtection();
        return TRUE;
    case WM_ENDSESSION:
        StopProtection();
        return 0;

    case WM_DESTROY:
        StopProtection();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS*) {
    // Last chance to give the user their audio back.
    ClearRouting();
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    SetUnhandledExceptionFilter(CrashHandler);

    // Loaded dynamically so the binary still runs on builds that predate
    // per-monitor v2. Without DPI awareness Windows stretches the window and
    // clips the controls at the bottom.
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto set = (SetCtxFn)GetProcAddress(user32,
                            "SetProcessDpiAwarenessContext")) {
            set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    // One instance only: two copies fighting over Discord's routing would
    // leave whichever loses pointing at a device nobody is draining.
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Global\\AudioLimiterSingleton");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Audio Limiter is already running.\n\n"
                             L"Look for the icon in your system tray.",
                    kAppName, MB_ICONINFORMATION);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    if (FAILED(g_router.Initialize())) {
        MessageBoxW(nullptr,
                    L"This version of Windows does not support per-app audio "
                    L"redirection.\n\nWindows 10 build 1903 or newer is required.",
                    kAppName, MB_ICONERROR);
        return 1;
    }

    const bool firstRun = ReadFileW(SettingsPath()).empty();
    LoadSettings();

    // A leftover state file means a previous run died without restoring the
    // user's routing, so their Discord is currently silent. Fix it before
    // doing anything else.
    RecoverStaleRouting();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, kWndClass, kAppName, 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, inst, nullptr);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = MakeDotIcon(RGB(120, 120, 120));
    wcscpy_s(g_nid.szTip, L"Audio Limiter");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    if (g_settings.enabled) {
        std::wstring err;
        if (!StartProtection(err)) {
            ShowBalloon(kAppName, err.c_str(), NIIF_INFO);
        } else {
            ShowBalloon(kAppName, L"Protecting your ears. Right-click the tray "
                                  L"icon to change the limit.", NIIF_INFO);
        }
    }
    UpdateTrayIcon();

    // First launch: show the window rather than starting invisibly in the
    // tray. A new user needs to see that something happened and what it did.
    if (firstRun) {
        SaveSettings();
        ShowSettings();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_settingsWnd && IsDialogMessageW(g_settingsWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopProtection();
    CoUninitialize();
    if (once) CloseHandle(once);
    return 0;
}
