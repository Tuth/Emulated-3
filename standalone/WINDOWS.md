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
Display settings are stored in `%APPDATA%/eiii/gui.state`.

## Initial limitations

* WinMM waveOut output is a compatibility backend, not an ASIO low-latency backend.
* MIDI includes short messages and buffered SysEx. Actual Windows device testing is still needed.
* The standalone has no host audio sampling input in this milestone.
* Window resizing is supported; aspect ratio is not constrained.
* The Windows window backend supports standalone and VST3 child windows.
* Use simple ASCII media paths for this first build; Unicode path support needs follow-up.
