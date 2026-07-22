# Audio Limiter

**Discord has no volume ceiling why??? :c** this is an imitation, still in early beta, open source, fork it if you want to add to the project.

[![Build](https://github.com/S1MS4/audio-limiter/actions/workflows/build.yml/badge.svg)](https://github.com/S1MS4/audio-limiter/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-0078D4?logo=windows&logoColor=white)](#quick-start)
[![Language](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](#building-from-source)
[![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?logo=cmake&logoColor=white)](CMakeLists.txt)
[![Audio](https://img.shields.io/badge/Audio-WASAPI-5C2D91?logo=windows&logoColor=white)](#how-it-works)
[![Installer](https://img.shields.io/badge/Installer-NSIS-orange)](installer/installer.nsi)
[![CI](https://img.shields.io/badge/CI-GitHub_Actions-2088FF?logo=githubactions&logoColor=white)](.github/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/S1MS4/audio-limiter?include_prereleases&label=Release)](https://github.com/S1MS4/audio-limiter/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/S1MS4/audio-limiter/total?label=Downloads)](https://github.com/S1MS4/audio-limiter/releases)
[![Code size](https://img.shields.io/github/languages/code-size/S1MS4/audio-limiter)](https://github.com/S1MS4/audio-limiter)
[![Last commit](https://img.shields.io/github/last-commit/S1MS4/audio-limiter)](https://github.com/S1MS4/audio-limiter/commits/main)
[![Issues](https://img.shields.io/github/issues/S1MS4/audio-limiter)](https://github.com/S1MS4/audio-limiter/issues)
[![Stars](https://img.shields.io/github/stars/S1MS4/audio-limiter?style=social)](https://github.com/S1MS4/audio-limiter/stargazers)

<p align="center">
  <img src="docs/screenshot.png" alt="Audio Limiter" width="420">
</p>

Per-user volume sliders only help *after* the blast. The Windows mixer doesn't help either: it multiplies, so quiet talkers and screamers drop together and the painful gap between them stays. A limiter leaves everything under the line untouched and only caps what crosses it.

## Quick start

1. Install [VB-CABLE](https://vb-audio.com/Cable/) (free) if you don't already have a virtual audio device.
2. Download the installer from [Releases](https://github.com/S1MS4/audio-limiter/releases/latest) and run it.
3. Done. It lives in the tray; right-click to change the limit.

The installer is not code-signed yet, so SmartScreen will warn on first run, just click "More info" then "Run anyway". Uninstalling restores your audio exactly as it was.

**Requirements:** Windows 10 (1903+) or 11, Discord desktop (Vesktop and browser work too).

## How it works

```
Discord  ──►  virtual audio device  ──►  limiter  ──►  your headphones
```

Only Discord is redirected (via the same per-app routing Windows' own sound settings use); everything else on your PC is untouched. Nothing is installed into Discord: no client mods, no injection, no account risk. A lookahead buffer means loud transients are caught before they reach your ears, at the cost of ~35 ms of delay you won't notice while listening. Per-speaker limits are impossible in this design: Windows only sees Discord's already-mixed stream.

## Building from source

Requires CMake 3.20+ and MSVC or MinGW-w64.

```bash
cmake -B build
cmake --build build --config Release
ctest --test-dir build
```

## License

MIT, see [LICENSE](LICENSE).
