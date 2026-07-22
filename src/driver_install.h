// Install / remove the bundled virtual audio device. Requires elevation.
#pragma once
#include <string>

namespace driver {
bool IsInstalled();
bool Install(const std::wstring& infPath, std::wstring& error);
bool Uninstall(std::wstring& error);
}
