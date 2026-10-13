# Windows D0 floppy (0.2.5 preview)

D0 uses the original MAME WD1772 controller at 8 MHz, compiled unmodified against the device shim. The EIII motherboard maps its four registers at `0xf50000` (write) and `0xf50008` (read), and its INTRQ to FDCINT. The misc latch selects the side with active-low bit 4 and controls the motor with active-low bit 5, as in MAME's EIII driver. There is no direct OS, RAM or bank injection.

The project drive adapter provides a 300 RPM rotating flux stream, 2 ms index pulses, head stepping, ready, track-zero and write-protect signals. The controller's original PLL, CRC, DRQ, IRQ and command state machines read that flux. Raw images are encoded into MFM tracks with sector headers and CRC; HFE v1 supplies its track bitstreams directly.

## Supported media

- Raw EIII IMG: exactly 819200 bytes, 80 cylinders, 2 sides, sectors 1–10, 512 bytes each, cylinder/head/sector order.
- HFE v1: `HXCPICFE`, revision 0, 80 cylinders, 2 sides, 250 kbit/s MFM. HFE v3/opcode streams and alternate non-MFM track-zero encoding are refused.
- E3O files are OS updater packages, not disk images. ZIP archives must be extracted first.

All mounted floppy media are write protected. Reading, OS boot and loading disk content use the real controller; Save Bank/format to D0 are not supported in this preview. Writable floppy images/overlays are a separate follow-up. D1 hard-disk sidecars and VST3 disk state work as before.

## Use

1. Choose the ROM folder in Settings.
2. D0 Floppy > Insert floppy image; select your IMG or HFE.
3. Settings > Restart for an OS cold boot. To test D0 without a hard disk, D1 HD > Disconnect HD (cold boot).
4. Once booted, operate the original OS front panel. Use D0 in the OS drive selection for floppy content; multi-disk banks require swapping images when the original OS requests another disk.

Clicking the physical front-panel drive opens D0. Its eject control ejects D0. D3 retains its own CD menu. Insertion/ejection is hot and does not reboot the machine; use Settings > Restart explicitly when needed. An invalid replacement is refused and the current image remains mounted. Standalone stores D0 in its own settings file; VST3 stores D0 per instance/project, with an optional separate default saved through Settings. VST3 state v2 preserves the floppy path and still loads v1 projects with an empty D0. The Linux CLAP GUI has not been extended with D0 menus in this Windows milestone.

Command line:

```text
eiii-standalone.exe --gui --rom-dir roms --floppy EIIIOS242.img --audio-out default
```

If saved GUI settings already have an HD, add `--hard ""` for the floppy-only test.

## Validation

`tests/floppy_controller.cpp` reads known synthetic sectors through the real MAME PLL at both heads and cylinders 0/79, checks bytes/CRC and IRQ acknowledgement, verifies write protection, a rejected replacement and ejection. It needs no private firmware. `tests/floppy_boot.cpp` boots the original firmware with no HD from a user-supplied OS floppy, loads the MIDI Globals menu and checks hot eject/reinsert.

```sh
make -C standalone BUILD=build/test build/test/tests/floppy_controller build/test/tests/floppy_boot
standalone/build/test/tests/floppy_controller
EIII_ROM_DIR=/path/to/roms EIII_FLOPPY=/path/to/EIIIOS242.img standalone/build/test/tests/floppy_boot
```

The supplied OS 2.42 IMG and HFE both reached the main screen in about 24 emulated seconds. All 1600 sectors decoded from HFE had valid CRC and matched the raw image before emulator testing. `tools/vst3_host_smoke.cpp` additionally accepts `EIII_BOOT_FLOPPY` to run the real VST3 processor through floppy-only OS boot, host-input sampling, MIDI playback and floppy project-state restore. Without it the existing HD save-bank regression remains enabled. Windows EXE/VST3 are cross-built; Windows file-dialog/DAW testing is performed on the user's host.

No ROM, OS or sample image is included in the source or release.
