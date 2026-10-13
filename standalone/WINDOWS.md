# Windows x64 standalone port

Standalone milestone. See VST3.md for the additional Windows VST3 preview; Windows CLAP is not implemented.
The emulation, panel renderer and original boot firmware are unchanged.

## Build

Cross compile on Linux with the POSIX-thread MinGW-w64 toolchain:

    make -C standalone windows -j4

In MSYS2 UCRT64, install GCC and make, then:

    make -C standalone TARGET=windows CROSS= build/windows/eiii-standalone.exe -j4

Output: `standalone/build/windows/eiii-standalone.exe`.
Standard Windows DLLs only; compiler runtime is linked statically.

## Run from a terminal

Place your two boot ROM files in `roms` next to the executable:
`EIII_IP381A_L.Bin` and `EIII_IP380A_R.Bin` (16,384 bytes each).

    eiii-standalone.exe --gui --rom-only --rom-dir roms --audio-out default
    eiii-standalone.exe --list-midi
    eiii-standalone.exe --list-audio
    eiii-standalone.exe --gui --rom-only --rom-dir roms --audio-out default --midi-in 0

Add `--hard path/to/your-os242.img` for the full operating system.
Boot ROMs alone allow an initial firmware boot test; they do not provide the sampler OS or banks.
For a finite diagnostic run without a GUI:

    eiii-standalone.exe --rom-only --rom-dir roms --seconds 10 --log boot.log --pc-trace boot-pc.txt

The supplied EPROMs must be kept separately and must not be committed or distributed.
Display settings are stored in `%APPDATA%/eiii/eiii-standalone-gui.state`.

## Initial limitations

* WinMM waveOut output is a compatibility backend, not an ASIO low-latency backend.
* MIDI includes short messages and buffered SysEx. Actual Windows device testing is still needed.
* The standalone has no host audio sampling input in this milestone.
* Window resizing is supported; aspect ratio is not constrained.
* The Windows window backend supports standalone and VST3 child windows.
* Use simple ASCII media paths for this first build; Unicode path support needs follow-up.

## 0.2.4 standalone GUI and persistent OS settings

Double-click `eiii-standalone.exe` to open the Windows GUI even without media configured. Choose a boot ROM BIN in Settings, then the OS disk in D1 HD. Native file dialogs do not change the current working directory. Settings, D1 HD and D3 CD provide the media controls; Audio and MIDI In list available WinMM devices and allow live device changes. A refused device change retains the previous device. Settings offers zoom, cold restart and analog model selection.

Standalone files are independent of VST3:

- `%APPDATA%/eiii/eiii-standalone.conf`: paths, device choices, analog model, window width.
- `%APPDATA%/eiii/eiii-standalone-gui.state`: display theme and contrast.
- `%APPDATA%/eiii/eiii-standalone-hd1.state`: OS disk writes, including settings the original OS saves. Other HD sidecars use their SCSI ID.
- `%APPDATA%/eiii/eiii-standalone-watchdog.log`: rare stop diagnostics.

The standalone never reads or changes `eiii-vst3.conf`. Existing command-line media/device arguments override its saved defaults when using `--gui`; headless runs keep their previous command-line behavior. `--scale` overrides remembered width.

Paths and successful device/media choices are saved automatically. Disk changes are checkpointed after the engine publishes them and flushed at normal exit. The source HD remains read-only; the disk sidecar is checked against the image fingerprint. On restart the OS and settings are read from the image plus that sidecar, not from an invented NVRAM/RAM snapshot. The original OS was tested changing MIDI Globals Poly to Omni: it writes disk sectors and a fresh instance restored from the sidecar retains Omni. Load the desired bank again after power-on.

## 0.2.5 D0 floppy

D0 Floppy > Insert floppy image accepts EIII 819200-byte raw IMG (80 tracks, 2 sides, 10 x 512-byte sectors) and HFE v1 (80 tracks, 2 sides, 250 kbit/s MFM). Click the front-panel floppy or eject control to operate D0; CD remains in D3. The MAME WD1772 runs at 8 MHz with actual rotating flux, index pulses, head stepping, motor and side signals. All floppy images are write protected; Save Bank/format onto floppy is not supported in this milestone. The original OS reports write protection.

For a floppy-only cold boot: insert the OS floppy at D0, then choose D1 HD > Disconnect HD (cold boot). To cold boot after changing the D0 disk, use Settings > Restart. Mount/eject does not reset the running machine. A bad replacement is refused and retains the previous medium. `--floppy path/to/EIIIOS242.img` or `.hfe` also works without `--hard`. The D0 path is stored only in standalone defaults; VST3 uses its separate defaults/project state. See [floppy details](../docs/FLOPPY.md).

IMG and HFE OS 2.42 boot, MIDI Globals menu loading, hot eject/reinsert, synthetic WD1772 sector/CRC/IRQ/write-protection checks and native VST3 sampling/MIDI/state regressions pass. Windows GUI/file-dialog confirmation remains a host-machine test. No ROM, OS or sample media is bundled.

Validation: independent configuration/binary sidecar regression, original-OS Globals persistence across a fresh engine, actual menu rendering without an OS window, native VST3 sampling/MIDI/bank/state regression, and Windows cross builds. Windows file dialogs/device changes still require host-machine confirmation. Linux X11 window execution was unavailable in this environment; menu rendering was validated offscreen. The new file chooser and device-list GUI is intended for Windows.
