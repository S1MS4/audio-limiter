# Audio Limiter

Hard-caps how loud anyone in Discord voice can get, so a sudden blast can't hurt your ears. Normal talking is untouched.

Works at the OS audio level. It does not modify Discord, inject into it, or use its API.

Windows only. MIT.

## Status

Pre-release. The M0 feasibility spike is implemented; the limiter is not written yet.

## How it works

Discord's audio output is routed to a virtual audio endpoint at startup, captured from there, passed through a lookahead limiter, and rendered to your real output device. Discord outputs only into that endpoint, so nothing reaches your speakers unprocessed. Routing is per-process, so no other application is affected.

Target latency is about 15 ms, which is irrelevant for listening since you are not speaking through the chain.

Routing uses `IAudioPolicyConfigFactory`, the undocumented interface behind Windows' own per-app sound settings. Interface layout transcribed from [EarTrumpet](https://github.com/File-New-Project/EarTrumpet) and [SoundSwitch](https://github.com/Belphemur/SoundSwitch).

## Build

Requires CMake 3.20+ and MSVC.

    cmake -B build
    cmake --build build --config Release

## M0 spike

    m0spike devices              list render endpoints
    m0spike sessions             list audio sessions
    m0spike discord              identify Discord's audio PID
    m0spike get <pid>            read current endpoint override
    m0spike route <pid> <devid>  route a process to an endpoint
    m0spike restore <pid>        clear the override

Discord must have played audio at least once since launch before it appears as a session. Join a voice channel first.

If routing leaves an app pointed at a dead endpoint it will be silent. `restore` fixes it, as does Settings > System > Sound > Volume mixer.

## Try this first

Windows has a built-in dynamic range compressor that may already solve the problem: Sound Control Panel > your output device > Properties > Enhancements > Loudness Equalization.

It affects every application rather than just Discord, and some drivers do not expose it. But it is free and already installed.

## Plan

See [fable-final-plan.md](fable-final-plan.md).
