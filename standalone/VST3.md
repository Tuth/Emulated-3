# Windows x64 VST3 preview

`EIII-Rack.vst3` is a directory bundle containing `Contents/x86_64-win/EIII-Rack.vst3`.
Copy the complete directory into `C:\Program Files\Common Files\VST3`, then rescan in Cubase.
Windows user tests confirmed loading in Cubase and VSTHost, firmware/OS boot, CD Insert and MIDI-driven sound. This remains a beta build.

## First instance

1. Load **EIII Rack** as a VST instrument and open its editor.
2. **Settings > Boot ROM folder...**: choose either of your two boot ROM BIN files. The folder is remembered automatically for new instances.
3. **D1 HD > Choose OS hard disk image...**: choose your extracted OS 2.42 raw image.
4. Optionally **D3 CD > Insert CD image...** for EIII bank media.
5. **Settings > Save paths as defaults** makes these paths the defaults of new instances.

No firmware or media is bundled. The supplied OS image has no sound banks.
The EIII boots to **P00 Untitled Preset / S00 Empty Song**.
The defaults file is `%APPDATA%/eiii/eiii-vst3.conf`; `EIII_ROM_DIR`, `EIII_HD`, `EIII_CD`
environment variables can override it. Use ASCII media paths in this preview.

## Audio and MIDI

* Stereo Main Out. Cubase supplies its own audio driver (including ASIO if configured).
* Stereo **Sample In** auxiliary input feeds the EIII ADC. Route audio to this bus if the host supports instrument input routing.
* 16 MIDI channels; note on/off, poly pressure, CC, channel pressure, pitch bend and program change mapping.
* 32-bit or 64-bit host sample buffers, 8–192 kHz. Maximum host block: 8192 samples.
* The emulator runs ahead of the host; its latency is reported to the host for compensation.
* Offline rendering waits for the emulation. Realtime rendering outputs silence on engine underrun.
* Frame-offset MIDI events and controller points are merged before being sent to the emulated serial MIDI input.

The empty power-on bank uses MIDI channel 1. Loaded EIII banks commonly use channel 14;
set the Cubase track channel to match the bank.

## Projects and banks

Save a new or edited bank through the EIII **Save Bank** operation into **D1**, then save the Cubase project.
The plug-in state includes paths, LCD appearance, analog model and the HD change set.
The original disk image remains read-only. No original ROM/OS image is embedded in the project.

Reopening starts the EIII from power-on. Use **Load Bank > D1 > B00** (or the bank slot you saved)
to load the saved bank. This preview does not snapshot emulated RAM or automatically restore the current bank selection.
A changed image path/fingerprint cannot receive an old image's change set.

## Build and verification

Use the official Steinberg VST3 SDK 3.8.1 (MIT), with its base, pluginterfaces and public.sdk directories.
Pinned revisions are in `VST3-SDK-REVISIONS.txt`. The repository includes the pinned build subset in `third_party/vst3sdk`.

    make -C standalone windows-vst3 -j4

For MSYS2 UCRT64, add `CROSS=` and invoke:

    make -C standalone TARGET=windows CROSS= build/windows/EIII-Rack.vst3/Contents/x86_64-win/EIII-Rack.vst3 -j4

Native Linux host regression (same processor/state implementation; HWND editor excluded):

    make -C standalone build/default/tools/vst3_host_smoke VST3_SDK=/path/to/vst3sdk
    EIII_ROM_DIR=/path/to/roms EIII_HD=/path/to/os242.img standalone/build/default/tools/vst3_host_smoke

The regression boots the original OS, samples host input through VST3, plays via VST3 MIDI,
saves the bank, restores plug-in state, reloads and replays it, and processes 64-bit audio after reactivation.
It is not a substitute for the Windows host/GUI test or the official VST3 validator.

The VST3 is a combined processor/controller, with one emulator per instance. Only D1 and D3 are offered
in its current menus. Further SCSI slots, Unicode paths and bank auto-loading are follow-up work.

## 0.2.1 CD Insert hotfix

The initial Windows preview used std::filesystem to construct the drive menu titles from
native code-page strings returned by GetOpenFileNameA. Non-UTF-8 accented filenames could
throw a conversion exception while refreshing the D3 menu, crossing the window callback
and crashing the host. Drive names and ROM parent directories now split native path bytes
without conversion. Only displayed strings are converted from the Windows code page to UTF-8.
Configuration-directory creation follows the same native path convention.

The Win32 callback also contains C++ exceptions. If an unexpected GUI exception occurs,
its timer is stopped and a diagnostic is sent to OutputDebugString rather than unwinding into the host.
Reopen the editor after such a GUI error. This does not catch hardware faults or validate
an ISO's EIII bank contents. Full arbitrary-Unicode file I/O is still follow-up work.

Regression: accented ANSI path splitting, complete VST3 sample/MIDI/bank/state round trip.
The tester confirmed CD Insert works after this fix.

## 0.2.2 editor and ROM preferences

Drag the small grip inside the editor's bottom-right corner, or choose **Settings > GUI 50% / 75% / 100% / 125% / 150% / 200%**.
Both use the VST3 host resize interface; a host that refuses resize requests will show a status message.
The aspect ratio is retained. The current width is included in project state, and the last GUI width is used for new editors.
Cubase resizing was confirmed. VSTHost may forget the size when the editor closes.

Choosing valid boot ROMs remembers their folder automatically. An old project state with an empty ROM path
falls back to that remembered folder. Each instance still owns its media, engine and disk changes;
automatically remembering the ROM folder does not overwrite another instance's HD/CD settings.
**Save paths as defaults** continues to explicitly save all media defaults.

MIDI scope: incoming note on/off, poly pressure, mapped CC 0–127, channel pressure, pitch bend and program change.
Incoming SysEx DataEvents are forwarded only when they fit the bounded per-block buffer (at most 12,288 bytes,
less when other events are present). A message that does not fit is skipped whole, not partially queued.
This is not a validated bulk sample-dump implementation. There is no MIDI/SysEx output port, so protocols
requiring outgoing acknowledgements or hardware-to-host dumps are not supported by this preview.
Host MIDI routing and the original OS/bank settings determine which controllers take effect.

## 0.2.3 long-run watchdog hotfix

Host and PIT deadlines expressed in integer CPU cycles now retain their integer budgets. Previously, subtracting large absolute floating-point timestamps could round a one-cycle interval below one cycle, then floor it to zero and spin until the watchdog stopped the machine. Fractional device timer scheduling and the watchdog remain enabled.

If a future watchdog stop occurs, the VST3 appends the emulated time, cycle, PC and deadline details to `%APPDATA%/eiii/eiii-vst3-watchdog.log`. This contains diagnostic counters, not ROM or sample contents.

Regression: `g++ -std=c++20 -O2 -I. tests/cycle_deadline.cpp -o /tmp/eiii-cycle-deadline && /tmp/eiii-cycle-deadline`. Optional VST3 soak: set `EIII_SOAK_SECONDS=4200` when running the native `vst3_host_smoke` test with your ROMs and OS disk. Windows host confirmation is still required for the new build.
