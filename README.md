# Audio Limiter

**Stop people blowing your ears out in Discord voice.** A hard volume ceiling on everyone else, running quietly in your system tray. Normal talking sounds exactly the same. Screaming does not.

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-0078D4?logo=windows&logoColor=white)](#requirements)
[![Language](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](#building-from-source)
[![Status](https://img.shields.io/badge/Status-Pre--release-orange)](#project-status)
[![Last commit](https://img.shields.io/github/last-commit/S1MS4/audio-limiter)](https://github.com/S1MS4/audio-limiter/commits/main)
[![Stars](https://img.shields.io/github/stars/S1MS4/audio-limiter?style=social)](https://github.com/S1MS4/audio-limiter/stargazers)

<p align="center">
  <a href="https://github.com/S1MS4/audio-limiter/releases/latest"><img src="https://img.shields.io/badge/Download-Windows_(.exe)-0078D4?style=for-the-badge&logo=windows&logoColor=white" alt="Download for Windows"></a>&nbsp;
  <a href="#quick-start"><img src="https://img.shields.io/badge/Setup-60_seconds-16a34a?style=for-the-badge" alt="Setup in 60 seconds"></a>&nbsp;
  <a href="#how-it-works"><img src="https://img.shields.io/badge/How_it_works-Read-6366f1?style=for-the-badge" alt="How it works"></a>
</p>

<p align="center">
  <a href="#quick-start"><strong>Quick Start</strong></a> &nbsp;·&nbsp;
  <a href="#faq"><strong>FAQ</strong></a> &nbsp;·&nbsp;
  <a href="#roadmap"><strong>Roadmap</strong></a> &nbsp;·&nbsp;
  <a href="https://github.com/S1MS4/audio-limiter/issues"><strong>Report a problem</strong></a>
</p>

<p align="center">
  <img src="docs/screenshot.png" alt="Audio Limiter" width="420">
</p>

---

## The Problem

Someone in your voice channel screams, drops a soundboard clip, or has their mic gain cranked to maximum. Your ears take the hit. You dive for the volume slider, and by the time you find it the damage is done.

Discord has no volume ceiling. It has never had one, and people have been [asking](https://support.discord.com/hc/en-us/community/posts/360061468432--Voice-Chat-Audio-Limiter) [for](https://support.discord.com/hc/en-us/community/posts/360068173192-Audio-Limiter) [years](https://support.discord.com/hc/en-us/community/posts/360062603392-Microphone-Channel-Loudness-Limit). Per-user volume sliders do not help, because you can only reach for them *after* it happens.

This fixes that. You set a maximum. Nothing gets past it. Ever.

---

## What It Does

- **A hard ceiling on everyone** — set it once, and no one in voice can exceed it
- **Normal speech is untouched** — quiet talkers stay audible; only the spikes get caught
- **Instant reaction** — a lookahead buffer catches loud sounds before they reach your ears, not after
- **Discord only** — your music, games, and videos are completely unaffected
- **Set and forget** — lives in the system tray, starts with Windows, no configuration required
- **Nothing installed into Discord** — no client modification, no injection, no plugins, no account risk

---

## Quick Start

**1.** Download the installer from [Releases](https://github.com/S1MS4/audio-limiter/releases/latest)

**2.** Run it. Click through the prompts.

**3.** Done. The tray icon turns green when it is protecting you.

That is the whole setup. No config files, no command line, no Discord settings to change. If you want to adjust the ceiling, right-click the tray icon and drag one slider.

> **Uninstalling is equally simple.** The uninstaller puts your audio back exactly as it was.

---

## How It Works

Most "audio limiter" ideas fail for a subtle reason: listening to Discord's audio does not stop it reaching your speakers. You would hear the raw sound and the processed sound at once, which is worse than doing nothing.

So this does not listen. It **redirects**.

```
Discord  ──►  private audio channel  ──►  limiter  ──►  your headphones
```

Discord's audio is sent into a private channel that only this app can hear. The app turns the volume down on anything too loud, then passes it along to your headphones. Because Discord's sound now goes *through* the app instead of *past* it, the loud version never reaches you at all.

Only Discord is redirected. Every other program on your computer is untouched.

<details>
<summary><strong>Technical details</strong></summary>

<br>

Routing uses `IAudioPolicyConfigFactory::SetPersistedDefaultAudioEndpoint`, the undocumented COM interface behind Windows' own per-app sound settings. Two vtable variants exist (Windows 11 21H2+ and Windows 10 pre-21H2) with different padding, so the factory is probed for the newer IID first. Interface layout transcribed from [EarTrumpet](https://github.com/File-New-Project/EarTrumpet) and [SoundSwitch](https://github.com/Belphemur/SoundSwitch).

Discord runs as multiple Electron processes and the one rendering audio is not the first `Discord.exe` you find. The target PID comes from enumerating live render sessions via `IAudioSessionManager2` and preferring active ones.

Audio is captured and rendered through WASAPI using `IAudioClient3::InitializeSharedAudioStream` at close to the minimum engine period, event-driven, with no allocation or locking on the audio thread and MMCSS "Pro Audio" scheduling. Target end-to-end latency is roughly 15 ms, which is inaudible when listening — you are not speaking through the chain.

Full design notes live in [fable-final-plan.md](fable-final-plan.md).

</details>

---

## Project Status

Working, and used daily by the author. Not yet packaged as a signed installer, so for now you build it yourself.

| Milestone | Status |
|---|---|
| Per-app routing verified on Windows 11 | Done |
| Discord audio process identified reliably | Done |
| WASAPI capture and render pipeline | Done |
| Lookahead limiter | Done |
| Tray app and interface | Done |
| Crash recovery for routing | Done |
| Signed installer | Not started |

There is **no installer yet.** Build it with the two commands below, or star the repo to hear when a signed release lands.

---

## Requirements

- Windows 10 (build 1903 or newer) or Windows 11
- Discord desktop, or any other app you want to cap

Works with Discord, Vesktop, and Discord in a browser. It caps whatever the program plays, so it does not care which one you use.

---

## Building From Source

Requires CMake 3.20+ and either MSVC or MinGW-w64.

```bash
git clone https://github.com/S1MS4/audio-limiter.git
cd audio-limiter
cmake -B build
cmake --build build --config Release
```

<details>
<summary><strong>Building with MinGW-w64</strong></summary>

<br>

```bash
cmake -B build -G "MinGW Makefiles" \
  -DCMAKE_CXX_COMPILER=g++.exe \
  -DCMAKE_MAKE_PROGRAM=gmake.exe
cmake --build build
```

The source avoids `__uuidof`, WRL, and `IID_PPV_ARGS` so it compiles under both toolchains. GUID definitions are emitted by `src/guids.cpp`, since MinGW does not ship a Core Audio GUID library.

</details>

<details>
<summary><strong>Developer harness</strong></summary>

<br>

`m0spike` is a throwaway CLI used to verify the routing layer:

```
m0spike devices              list audio output devices
m0spike sessions             list running audio sessions
m0spike discord              identify Discord's audio process
m0spike get <pid>            read a process's current output device
m0spike route <pid> <devid>  send a process to a device
m0spike restore <pid>        undo it
```

Discord must have played audio at least once since launch before it appears as a session.

</details>

---

## FAQ

**Will this get my Discord account banned?**
No. Nothing is installed into Discord, no files are modified, and Discord's API is never touched. From Discord's point of view nothing has changed. This works the same way Voicemeeter and EqualizerAPO do, at the Windows audio layer.

**Does it affect my games and music?**
No. Only the program you point it at is redirected. Everything else plays normally.

**Will people sound quieter or muffled?**
No. Anything below your ceiling passes through completely unchanged. The limiter only engages on sounds that would have hurt.

**Does it add delay?**
About 15 milliseconds, which you will not notice. You are listening through it, not speaking through it, so it does not affect conversation timing.

**Can I use it on something other than Discord?**
Yes. It caps any application's output. Discord is just the obvious use case.

**Is there something I can try right now, for free?**
Yes, and you should try it first. Windows has a built-in compressor: **Sound Control Panel → your output device → Properties → Enhancements → Loudness Equalization**. It affects every program rather than just Discord, and some audio drivers do not offer it, but it costs nothing and it is already on your machine. If it solves your problem, you do not need this.

---

## Roadmap

- Audio passthrough with verified zero dropouts
- Lookahead limiter with adjustable ceiling
- System tray interface with a single slider
- Signed installer with automatic setup and clean uninstall
- Automatic recovery if the app is ever closed unexpectedly
- A real project name

---

## Contributing

Early days, but issues and ideas are welcome. If you have hit the loud-user problem and have opinions about how this should behave, that is genuinely useful.

---

## License

MIT — see [LICENSE](LICENSE). Use it, fork it, ship it, no obligations.

---

## Author

**S1MS4** — [GitHub](https://github.com/S1MS4)

Built because someone screamed into a microphone one too many times.
