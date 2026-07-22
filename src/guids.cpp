// Emits the COM GUID and PROPERTYKEY definitions once for the whole program.
//
// MSVC gets these from uuid.lib, but MinGW does not ship them for the Core
// Audio interfaces, so the link fails with "undefined reference to
// CLSID_MMDeviceEnumerator" and friends. Including <initguid.h> first turns
// the DEFINE_GUID / DEFINE_PROPERTYKEY macros in the headers below from
// declarations into definitions.
//
// This must happen in exactly one translation unit. Do not include
// <initguid.h> anywhere else.

#include <initguid.h>

#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
