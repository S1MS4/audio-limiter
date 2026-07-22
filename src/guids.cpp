// COM GUID definitions.
//
// The headers only *declare* these (EXTERN_C const GUID ...); the definitions
// have to come from somewhere. Neither available option is portable:
//
//   - <initguid.h> before the headers works only where the SDK spells them
//     with DEFINE_GUID, which the Core Audio headers do not.
//   - uuid.lib carries many GUIDs but not these on current Windows SDKs.
//
// So they are written out by value. This links identically under MSVC and
// MinGW and needs no extra library. Values are from the Windows SDK headers;
// they are ABI-stable and cannot change.

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#define DEFINE_KNOWN_GUID(name, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    EXTERN_C const GUID name = {a, b, c, {d0, d1, d2, d3, d4, d5, d6, d7}}

// {BCDE0395-E52F-467C-8E3D-C4579291692E}
DEFINE_KNOWN_GUID(CLSID_MMDeviceEnumerator,
                  0xBCDE0395, 0xE52F, 0x467C,
                  0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);

// {A95664D2-9614-4F35-A746-DE8DB63617E6}
DEFINE_KNOWN_GUID(IID_IMMDeviceEnumerator,
                  0xA95664D2, 0x9614, 0x4F35,
                  0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);

// {77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F}
DEFINE_KNOWN_GUID(IID_IAudioSessionManager2,
                  0x77AA99A0, 0x1BD6, 0x484F,
                  0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F);

// {BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D}
DEFINE_KNOWN_GUID(IID_IAudioSessionControl2,
                  0xBFB7FF88, 0x7239, 0x4FC9,
                  0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D);

// {1CB9AD4C-DBFA-4C32-B178-C2F568A703B2}
DEFINE_KNOWN_GUID(IID_IAudioClient,
                  0x1CB9AD4C, 0xDBFA, 0x4C32,
                  0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);

// {C8ADBD64-E71E-48A0-A4DE-185C395CD317}
DEFINE_KNOWN_GUID(IID_IAudioCaptureClient,
                  0xC8ADBD64, 0xE71E, 0x48A0,
                  0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);

// {F294ACFC-3146-4483-A7BF-ADDCA7C260E2}
DEFINE_KNOWN_GUID(IID_IAudioRenderClient,
                  0xF294ACFC, 0x3146, 0x4483,
                  0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);

// PKEY_Device_FriendlyName: fmtid {A45C254E-DF1C-4EFD-8020-67D146A850E0}, pid 14
EXTERN_C const PROPERTYKEY PKEY_Device_FriendlyName = {
    {0xA45C254E, 0xDF1C, 0x4EFD,
     {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}},
    14};

// {87CE5498-68D6-44E5-9215-6DA47EF883D8}
DEFINE_KNOWN_GUID(IID_ISimpleAudioVolume,
                  0x87CE5498, 0x68D6, 0x44E5,
                  0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8);
