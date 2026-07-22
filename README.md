# Audio Limiter

**Stop people blowing your ears out in Discord voice.** A hard volume ceiling on everyone else, running quietly in your system tray. Normal talking sounds exactly the same. Screaming does not.

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Build](https://github.com/S1MS4/audio-limiter/actions/workflows/build.yml/badge.svg)](https://github.com/S1MS4/audio-limiter/actions/workflows/build.yml)

<p align="center">
  <img src="docs/screenshot.png" alt="Audio Limiter" width="420">
</p>

Discord has no volume ceiling, and per-user sliders only help *after* the blast. The Windows mixer doesn't help either: it multiplies, so quiet talkers and screamers drop together and the painful gap between them stays. A limiter leaves everything under the line untouched and only caps what crosses it.

## Quick start

1. Install [VB-CABLE](https://vb-audio.com/Cable/) (free) if you don't already have a virtual audio device.
2. Download the installer from [Releases](https://github.com/S1MS4/audio-limiter/releases/latest) and run it.
3. Done. It lives in the tray; right-click to change the limit.

The installer is not code-signed yet, so SmartScreen will warn on first run — "More info", then "Run anyway". Uninstalling restores your audio exactly as it was.

**Requirements:** Windows 10 (1903+) or 11, Discord desktop (Vesktop and browser work too).

## How it works

```
Discord  ──►  virtual audio device  ──►  limiter  ──►  your headphones
```

Only Discord is redirected (via the same per-app routing Windows' own sound settings use); everything else on your PC is untouched. Nothing is installed into Discord — no client mods, no injection, no account risk. A lookahead buffer means loud transients are caught before they reach your ears, at the cost of ~35 ms of delay you won't notice while listening. Per-speaker limits are impossible in this design: Windows only sees Discord's already-mixed stream.

## Building from source

Requires CMake 3.20+ and MSVC or MinGW-w64.

```bash
cmake -B build
cmake --build build --config Release
ctest --test-dir build
```

## License

MIT — see [LICENSE](LICENSE).
