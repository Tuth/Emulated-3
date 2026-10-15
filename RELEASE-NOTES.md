# Windows standalone and VST3 0.2.7-preview host-wait fix

Fix the shared engine's Windows sub-millisecond waits. In the original MinGW binary, nanosleep truncated the 500 us buffer-full wait and 100 us offline-pull wait to zero milliseconds. The worker repeatedly polled a full queue instead of blocking. Use a reusable high-resolution waitable timer, with ordinary-timer and upward-rounded Sleep fallbacks.

The original scalar CEM model, emulated clocks, oversampling, audio queue sizes, MIDI timestamps, plugin ID and saved-state formats are retained. This release does not include the rejected SIMD experiment or the compiler-only O3/LTO experiment. VST3 advertises 0.2.7-preview for host rescanning.

User Windows READY captures show process CPU consumption falling from 102.3% to 12.4% of one logical processor (about 88% less); the emulation worker fell from 99.8% to 10.7%. This validates READY consumption, not sustained-polyphony CPU, driver latency or dropout-free low-buffer operation. Standalone was user-tested; the VST3 was cross-built with the same engine fix and still needs Windows host confirmation. Native firmware/MIDI/sampling/state smoke and deterministic wait/WinMM regressions pass. No proprietary media is included.

# Windows standalone 0.2.6-preview audio hotfix

Fix WinMM pacing for realtime standalone use: replace Sleep polling with buffer-completion events, wait for a writable device slot before pulling engine audio, and start/restart from a filled configured queue. Scope a 1 ms timer-resolution request to the opened output and use above-normal I/O thread priority. Bound stalled-driver waits and report output starvation.

Keep the existing engine prebuffer; no additional engine latency is introduced. Audio offers live 2/4/8 device-buffer choices and underrun counters. Preferences remain standalone-only; audio failures keep the old device. Changed underrun counts write `eiii-standalone-audio.log` with engine/driver diagnostics. At a 256-frame/48 kHz period, 2 device buffers have 10.7 ms queue capacity; the engine queue remains 21.3 ms. These are internal queue capacities, not measured end-to-end latency.

The deterministic mock-driver test exercises the actual Windows backend and passes. WIN64 EXE build, independent preference round trip and menu rendering pass. Actual Windows listening/device testing remains with the user. The included VST3 remains the unchanged, validated 0.2.5 binary. D0 IMG/HFE support and OS/Global persistence are retained. No ROM, OS or sample media is bundled.

# Windows x64 0.2.5-preview

Add real D0 floppy reading and OS boot through the unmodified MAME WD1772 (8 MHz). The EIII drive adapter supplies rotating MFM flux, index, head stepping and active-low motor/side signals. Accept EIII 819200-byte raw IMG and 80-track double-sided 250 kbit/s MFM HFE v1 images.

Standalone and VST3 have D0 insert/eject menus; the physical front-panel drive/eject shortcut now operates D0. D1 can be disconnected for a floppy-only cold boot. Mount/eject preserves the running OS. Standalone D0 defaults remain independent of VST3; VST3 stores the floppy path per project in v2 state and still reads old v1 state.

Floppy media are write protected: reading/boot is supported, Save Bank/format to floppy is not. E3O is an updater package, not a raw floppy image. HD changes and the watchdog fix are retained. OS 2.42 boot tested from both IMG and HFE with no HD, including loading the MIDI Globals menu. Synthetic WD1772 sector bytes/CRC, track/head boundaries, IRQ acknowledgement, write protection and rejected replacement pass. Native VST3 sampling/MIDI/bank/state regression passes. Windows x64 EXE/VST3 cross-build succeeds; host-side Windows GUI confirmation remains necessary.

No E-mu ROM, OS or sample media is bundled.

# Windows x64 0.2.4-preview

Bring the Windows standalone GUI up to date with Settings, boot-ROM selection, D1 OS image selection, D3 CD insert/eject, zoom and analog model controls. Add live WinMM audio and MIDI input device menus. Double-click starts the GUI without command-line media.

Standalone defaults, display appearance, watchdog diagnostics and persistent HD change sidecars have dedicated filenames independent of VST3. The original OS MIDI Globals were verified to survive a fresh engine via its standalone disk sidecar; the base image remains read-only. VST3 continues to store disk changes per instance/project.

The 0.2.3 watchdog fix is retained. Native regression tests and Windows builds pass; Windows GUI/device confirmation is pending. Floppy access remains unimplemented (WD1772 no-disk stub); it requires a separate controller/media implementation and an original EIII floppy image for validation.

No E-mu ROM, OS or sample media is bundled.
