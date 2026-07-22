# Discord Audio Limiter — Project Plan

A standalone Windows tray app that caps how loud anyone in Discord voice can get. No client modding, no injection, no browser extension. OS-level audio only.

This plan commits to one stack. Alternatives were considered and rejected; the rejections are recorded in Section 9 so they don't get relitigated.

---

## 1. Goal

Hard-limit incoming Discord voice in real time on Windows, with inaudible latency, without touching any other app's audio.

**Non-goals:** cross-platform, EQ, noise suppression, fixing the sender's mic, mobile, paid tier.

---

## 2. Before you build: two hours that could end the project

**Enable Windows Loudness Equalization tonight.** Sound Control Panel → your output device → Properties → Enhancements → Loudness Equalization. It's a built-in dynamic-range compressor ([Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/loudness-equalization-dsp)).

It's per-*device*, so it also squashes your games and music, and it's driver-provided so it's [missing on many systems](https://www.thewindowsclub.com/loudness-equalization-missing-in-windows-10). But if it's good enough for you, **stop — the project is unnecessary.** One evening to find out.

For the record, Discord itself has nothing. Its *attenuation* feature lowers **other applications** when someone speaks — it does not cap Discord users ([docs](https://support.discord.com/hc/en-us/articles/206342888-How-do-I-stop-Discord-from-lowering-my-volume-when-someone-else-is-talking)). A real limiter has been requested for years and never shipped: [1](https://support.discord.com/hc/en-us/community/posts/360061468432--Voice-Chat-Audio-Limiter), [2](https://support.discord.com/hc/en-us/community/posts/360068173192-Audio-Limiter), [3](https://support.discord.com/hc/en-us/community/posts/360062603392-Microphone-Channel-Loudness-Limit). Those threads are your launch audience.

---

## 3. Architecture

Capturing Discord's audio is not enough — a capture is a *copy*, so the original still reaches your speakers and you'd hear both. The audio must be **diverted**.

```
Discord ──► Virtual endpoint ──► [capture → limiter → render] ──► Real output device
              ▲                   event-driven, ~3ms periods, zero-alloc
              │
        routed programmatically at startup via IAudioPolicyConfigFactory
```

Discord outputs **only** into the virtual endpoint, so nothing reaches the speakers unprocessed. Routing is per-process, so every other app is untouched.

---

## 4. The stack — committed

### 4.1 Language: C++20, Win32, no framework

Not C#. **A garbage collector in a real-time audio path is a correctness bug, not a performance preference** — a GC pause during the render callback is an audible dropout, and mitigating it means writing allocation-free C# anyway, which discards the reason to use C#. Rule one of real-time audio is no allocation on the audio thread ([Bencina](http://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing)).

The only argument for C# was that EarTrumpet's routing interop is C#. That's a false constraint: it's a COM interface, and **C++ is the native way to call COM** — you transcribe the GUIDs and vtable and you're done.

Result: one statically-linked exe, no .NET runtime, ~1 MB, instant start, no installer prerequisites. That is also the *fastest* to ship, because runtime-dependency handling is most of installer pain.

UI is a Win32 tray icon plus one small dialog. Roughly 300 lines. Do not add a UI framework for two sliders and a combo box.

### 4.2 Routing: `IAudioPolicyConfigFactory`

Windows has supported per-app output routing since Windows 10 1803, with no documented API. It is reachable via the undocumented COM interface **`IAudioPolicyConfigFactory::SetPersistedDefaultAudioEndpoint(pid, dataFlow, role, deviceId)`** — the same call the Windows Sound settings UI makes.

Proven in production by **EarTrumpet**: [IAudioPolicyConfigFactory.cs](https://github.com/File-New-Project/EarTrumpet/blob/dev/EarTrumpet/Interop/MMDeviceAPI/IAudioPolicyConfigFactory.cs). Transcribe it to C++.

This is what makes the app usable — Discord is routed automatically at launch and **the user never opens Sound settings.** Manual-setup tools get abandoned during setup.

### 4.3 Virtual endpoint: bundled signed MIT driver

[VirtualDrivers/Virtual-Audio-Driver](https://github.com/VirtualDrivers/Virtual-Audio-Driver) — MIT (MS-PL for Microsoft sample portions), Win10 1903+/Win11, x64 and ARM64.

**Ship the production-signed build (25.7.14+).** The beta requires `bcdedit /set testsigning on`, which is an absolute non-starter for end users — if the signed build doesn't work, that is an M0 failure, not a workaround to document.

Configure it for the minimum viable buffer. The virtual device's own buffering is pure additive latency and it is the one component you don't control directly — measure it in M0 and, if it's poor, fork the driver and tune it. It's MIT; that's allowed.

### 4.4 Audio engine: WASAPI `IAudioClient3`, event-driven

Do not use the default 10ms shared-mode period. Use **`IAudioClient3::GetSharedModeEnginePeriod`** to discover the minimum, then **`InitializeSharedAudioStream`** at or near it. Microsoft's own example returns a minimum of **48 frames ≈ 1.09 ms** ([docs](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-getsharedmodeengineperiod)). `PeriodInFrames` must be an integral multiple of the fundamental period and within the min/max range.

Non-negotiable rules for the audio path:
- **Event-driven** (`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`), never polling.
- **Zero allocation, zero locks, zero syscalls** in the callback. Pre-allocate everything at init.
- **Lock-free SPSC ring buffer** between capture and render threads. Single producer, single consumer, no mutex.
- **MMCSS** — register the audio threads with `AvSetMmThreadCharacteristics("Pro Audio")` so the scheduler treats them correctly.
- Settings changes arrive via a lock-free command queue. The UI thread never touches DSP state directly.
- Match the capture and render formats at init to avoid a resampler in the path.

### 4.5 DSP: Signalsmith `limiter.h`

[Signalsmith Audio basics](https://github.com/Signalsmith-Audio/basics) — MIT, header-only C++, lookahead limiter. The author's [design write-up](https://signalsmith-audio.co.uk/writing/2022/limiter/) explains it properly.

MIT is a hard requirement: a GPL DSP library would force the entire app to GPL.

Tuning for voice:
- **Lookahead 5 ms.** Enough to catch any transient before it's audible. More is wasted latency.
- **Soft knee ~10 dB, attack ~5 ms** — a hard-knee 1 ms limiter adds crackle to normal speech.
- **Gate on peak AND RMS together.** Peak alone false-triggers on plosives and laughter.
- **Release ~250 ms, asymmetric.** Duck fast, recover slowly. This is what prevents pumping.

### 4.6 Latency budget

| Stage | Target |
|---|---|
| Capture period | ~3 ms |
| Ring buffer safety | ~3 ms |
| Limiter lookahead | 5 ms |
| Render period | ~3 ms |
| Virtual driver overhead | measure at M0 |
| **Total** | **~15 ms + driver** |

Well under the ~20–50 ms a naive 10 ms-period implementation gives. Irrelevant for listening either way — you aren't speaking through this chain — but low latency also means a small ring buffer, which means less to go wrong.

**Measure it at M3 and put the real number in the README.** Never claim single-digit milliseconds.

---

## 5. Milestones

**M0 — Spike. Go/no-go. A few evenings, hard timebox.**
1. **(2 hours, do first)** Test whether per-process loopback survives a session mute — see Section 9.1. If it works, **delete the driver from this plan entirely.** Low probability, enormous payoff, cheap to check.
2. Transcribe EarTrumpet's interop to C++; route Discord to an arbitrary device and confirm it takes effect.
3. Install the **signed** virtual driver. Confirm it enumerates and needs no test signing. Measure its added latency.
4. Identify Discord's audio-owning PID reliably (Section 6).

Gate: (2) and (3) both pass → build. (3) fails → the project is blocked on a driver problem; solve that before anything else.

**M1 — Routing proof.** Discord's audio provably reaches your app **and** provably no longer reaches the speakers. Confirm no doubling by ear. This is the milestone that de-risks everything; the rest is ordinary work.

**M2 — Audio engine.** `IAudioClient3` capture→ring→render passthrough at minimum period. **No DSP yet.** Run it for an hour and confirm zero dropouts. Get the plumbing glitch-free before adding anything to it.

**M3 — Limiter.** Drop Signalsmith in. Test against recorded blasts and normal speech. Measure end-to-end latency. Dogfood a week.

**M4 — Tray UI.** Toggle, threshold, output device picker. Nothing else visible.

**M5 — Packaging.** Authenticode-signed exe, installer that also installs the driver, checksums, auto-start, and **routing restoration on uninstall** (Section 7).

**M6 — Repo.** MIT LICENSE, SECURITY.md, SUPPORT.md, third-party attributions, README with measured latency, FUNDING.yml.

**M7 — Launch.** The three Discord threads in Section 2, r/discordapp, r/Windows11.

**Do not start M4 before M1 is proven.** UI is the most seductive form of procrastination on this project.

---

## 6. Discord is Electron

Discord runs main, renderer, GPU and utility processes; the one rendering audio is not necessarily what a `Discord.exe` lookup returns first. Enumerate sessions with `IAudioSessionManager2` and find the one whose process tree belongs to Discord. **Never match on executable name alone.**

Discord restarting means a new PID and lost routing. Subscribe to session notifications and re-apply. Test this explicitly — it happens to every user constantly and silent failure here looks like your app is broken.

---

## 7. The one bug that will define this project's reputation

**If your app dies while Discord is routed to a virtual endpoint that isn't being drained, Discord goes silent — and the user has no idea why.** They won't blame the audio device. They'll blame you, loudly, in your issue tracker.

Treat restoration as a first-class feature:
- Persist the *previous* endpoint to disk **before** changing routing.
- Restore on clean exit.
- On startup, detect a dirty previous exit and restore immediately.
- Uninstaller restores routing **before** removing the driver.
- Install a watchdog: if the render thread stalls beyond a threshold, restore routing and exit rather than leaving the user in silence.

Every other bug in this project is recoverable by restarting the app. This one isn't.

---

## 8. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Silent Discord after crash/uninstall | **Critical** | Section 7 — first-class feature, not cleanup |
| Signed driver won't install cleanly | High | M0.3. This gates the project. Test signing is not an acceptable fallback |
| Undocumented routing API breaks in a Windows update | High | It's what Windows' own UI uses, so unlikely to vanish silently. Wrap behind one interface. Test on Insider builds |
| Driver install trips SmartScreen/AV | High | Sign the exe. Audio-rerouting tools get heuristically flagged as spyware routinely — monitor VirusTotal per release and submit to AV allowlists **before** launch |
| Dropouts from a GC/alloc/lock in the callback | High | Structurally prevented by 4.1/4.4. Verified at M2 before any DSP exists |
| Discord restarts → routing lost | Medium | Session notifications + re-apply. Test explicitly |
| Virtual driver adds unacceptable latency | Medium | Measure at M0. It's MIT — fork and tune the buffer if needed |
| Discord changes its audio pipeline | **Low** | You're entirely outside Discord. The structural advantage of this architecture |
| Solo maintainer burnout | Medium | Hold scope at Section 1. Ship SUPPORT.md ("best effort, no SLA") on day one |

---

## 9. Rejected alternatives — do not relitigate

**9.1 Per-process loopback + session mute.** `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` (Win10 build 20348+, [docs](https://learn.microsoft.com/en-us/windows/win32/api/audioclientactivationparams/ns-audioclientactivationparams-audioclient_activation_params), [sample](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)) captures one process tree with no driver. You'd mute Discord's own path via `ISimpleAudioVolume`.

**Almost certainly dead:** WASAPI "copies the output stream **from the audio engine**" ([Loopback Recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)) — that's *after* volume, so muting should mute the capture too. Genuinely undocumented for the per-process case though, and reports conflict ([cscore#86](https://github.com/filoe/cscore/issues/86)). **Worth exactly two hours in M0** because success deletes the entire driver dependency. Test `IAudioSessionControl` mute separately — mute and volume are distinct code paths.

**9.2 C# / .NET.** GC pauses in the audio callback. Section 4.1.

**9.3 DSP inside the driver.** Theoretically optimal — eliminates the user-mode round trip. Kernel debugging, a signing chain, and no crash recovery. Not for v1.

**9.4 Custom APO.** Per-*device*, not per-app, so it can't isolate Discord. Fatal.

**9.5 VB-CABLE.** Can't be bundled without a licence from VB-Audio, forcing manual user install. The MIT driver is strictly better.

**9.6 SoundVolumeView shell-out.** Third-party binary, process spawn per routing change, own redistribution terms. Only as an emergency fallback if 4.2 fails.

**9.7 Client mods.** Cannot reach voice audio on stock Discord Desktop at all — it plays through a native module unreachable from JavaScript. Forces GPL and a permanent maintenance treadmill against minified internals.

---

## 10. Legal & funding

- **License: MIT.** Nothing in the chain forces otherwise — Signalsmith is MIT, the driver is MIT/MS-PL, EarTrumpet is MIT. Keep their copyright notices for anything you transcribe.
- **Discord ToS:** low risk. You don't modify Discord, touch its API, or inject anything. Same posture as Voicemeeter or EqualizerAPO. Read the current ToS before a wide launch anyway.
- **Never log audio.** If you add crash reporting it must never contain audio content. Storing another person's voice can implicate two-party-consent law and, since you're in the EU, GDPR. Permanent and non-negotiable.
- **Name:** no "Discord" in it (trademark). Not "Fable" (collides with an Anthropic model line; wrecks your search results). EUIPO/USPTO search before locking it in.
- **Ko-fi:** free tier, one-time donations, 0% platform fee. Confirm Lithuania payout methods once before launch. Donations are reportable personal income above local thresholds — irrelevant at zero, check before it's real money.

---

## 11. Repo cleanup

1. **`git init` and commit first** — this repo is not under version control and step 2 deletes the only copy.
2. Delete `Vencord/`, `Vesktop-Setup-1.6.5.exe` (200 MB of someone else's redistributed installer), `inject.ps1`, `inject.bat`, `install.bat`, `setup-vesktop.ps1`, `setup-vesktop.bat`.
3. Delete `audioLimiter/index.tsx` — but read Section 4.5 against it first. Its DSP *reasoning* (lookahead, peak+RMS gating, duck-fast/recover-slow) is sound and carries over. Keep the ideas, not the code.
4. Rewrite `README.md` at M6.

---

## 12. Definition of done

- Loudness Equalization tried and found insufficient — you know why you're building this
- M0 passed with evidence: routing works from C++, signed driver installs, Discord's audio PID identified, driver latency measured
- Discord rerouted not duplicated; no doubling, verified by ear
- Other apps provably untouched
- One hour of passthrough at minimum period with zero dropouts (M2, pre-DSP)
- **Routing restored on exit, crash, and uninstall** — no user ever left with silent Discord
- End-to-end latency measured and stated in the README
- Signed exe, no SmartScreen warning, VirusTotal clean or flagged-and-explained
- MIT LICENSE, SECURITY.md, SUPPORT.md, third-party attributions present
- Ko-fi payout confirmed for Lithuania

---

## 13. Summary

Route Discord to a bundled MIT virtual endpoint using the undocumented API EarTrumpet already proved works. Capture and render it with `IAudioClient3` at ~3 ms periods, event-driven, zero-allocation, MMCSS-registered. Run Signalsmith's MIT lookahead limiter over it at 5 ms lookahead. Ship one statically-linked signed exe with a Win32 tray icon. Total latency ~15 ms plus driver.

The architecture is settled. **Two hours on the loopback test, then M0. Don't write another plan.**
