// Finds which process actually owns Discord's audio session.
//
// Discord is Electron, so it runs a main process plus renderer, GPU and
// utility children. The PID that renders audio is usually NOT the one a
// naive "first Discord.exe" lookup returns. Matching on executable name
// alone picks the wrong process and the routing call silently does nothing.
//
// So: enumerate real audio sessions on the endpoint, keep the ones whose
// process image is Discord, and prefer sessions that are actually active.

#pragma once

#include <mmdeviceapi.h>
#include <string>
#include <vector>

namespace sessions {

struct AudioSession {
    DWORD processId = 0;
    std::wstring executablePath;
    std::wstring displayName;
    bool active = false;   // AudioSessionStateActive, i.e. currently making sound
};

// All render sessions on the default endpoint, including inactive ones.
std::vector<AudioSession> EnumerateRenderSessions();

// Sessions whose executable matches any of `names` (case-insensitive,
// filename only). Pass e.g. {L"Discord.exe", L"DiscordPTB.exe"}.
std::vector<AudioSession> FindByExecutable(const std::vector<std::wstring>& names);

// Best guess at the single PID to route.
//
// Prefers an active session over an inactive one, because during a call the
// audio-rendering process is by definition active. Returns 0 if nothing
// matched -- callers must handle that rather than routing PID 0, which
// would apply the override to the entire system.
DWORD FindAudioOwningPid(const std::vector<std::wstring>& names);

// Convenience wrapper for the Discord family (stable, PTB, Canary).
DWORD FindDiscordAudioPid();

} // namespace sessions
