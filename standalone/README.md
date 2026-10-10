# eiii-standalone — E-mu Emulator III runtime for Linux

Runs the **original EIII boot ROM and Operating System 2.42** from the original disk images, with the original front
panel, SCSI, and MIDI paths, and renders the voice outputs through the portable F-Chip and a software CEM3387 model.
It contains **no E-mu material**: you supply the boot ROMs, the OS hard-disk image, and (optionally) a factory CD image.

MAME is the behavioural reference this runtime is checked against (`make check`); the MAME executable is not needed.

## Build

Required packages (Debian/Ubuntu/Raspberry Pi OS):

    sudo apt install build-essential libasound2-dev

Build:

    cd eiii-core/standalone
    make release          # -> dist/eiii-standalone (stripped) + dist/README.md + dist/SHA256SUMS
    make                  # development build -> ./eiii-standalone (used by the regression scripts)
    make debug | asan     # build/debug, build/asan (AddressSanitizer + UBSan)

Everything compiles from this tree: the MAME device sources it uses are vendored in `third_party/mame`
(BSD-3-Clause, provenance and SHA256 manifest in `third_party/mame/PROVENANCE.md`), together with Berkeley SoftFloat 3e.
`MAME=/path/to/mame make` builds from a live MAME tree instead.

## Media

| What | Option | Notes |
|---|---|---|
| Boot ROMs `e3-lsboot_ip381a_emu_systems_4088.ic3`, `e3-msboot_ip380a_emu_systems_4088.ic4` | `--rom-dir DIR` or `$EIII_ROM_DIR` (default `./roms`) | 2 × 16 KB |
| OS 2.42 hard disk image (SCSI ID 1) | `--hard IMAGE` | required; opened **read-only**, OS writes go to a RAM overlay and are discarded at exit |
| Factory CD-ROM image (SCSI ID 3) | `--cdrom ISO` | optional; read-only |

## Run

List devices:

    ./eiii-standalone --list-midi
    ./eiii-standalone --list-audio

Play live (USB MIDI interface in, sound card out), loading factory bank 44 and preset 1 through the original front panel:

    export EIII_ROM_DIR=/path/to/roms
    ./eiii-standalone \
      --hard  /path/to/EIII_OS242_96MB.hd \
      --cdrom "/path/to/EIII.01 Emulator Standards.iso" \
      --autoload-bank 44 --preset 1 \
      --midi-in hw:1,0,0 \
      --audio-out plughw:0,0

Stop with Ctrl-C. The factory Tenor Sax bank receives on **MIDI channel 14** (the bank's own MIDI setup).

* `--autoload-bank N [--drive D] [--preset P]` presses the original panel switches exactly as an operator would
  (Load Bank, Enter, drive digit, Enter, bank digits, Enter, preset digits), waiting at each step for the LCD text the
  firmware draws. Progress is printed as `UI ...` lines; a step that does not appear within its timeout aborts with the
  LCD contents (exit code 4). It never writes RAM or calls firmware routines.
* `--audio-out`: 48 kHz stereo. `default` (PipeWire/Pulse) works with its own buffering (~85 ms); a direct
  `plughw:C,D` device runs underrun-free down to `--period 64 --periods 4` (5.3 ms) on the test machine.
  The emulated machine produces the audio; the device only paces the wall clock. XRUNs are reported.
* `--midi-in`: ALSA raw MIDI device (`hw:C,D,S` from `--list-midi`, or `virtual` for a sequencer port that other
  programs can connect to). Bytes are fed, unmodified, into the emulated Z85230 MIDI IN at 31 250 baud; the original
  OS parses them (notes, velocity, controllers, pitch bend, program change — whatever the OS supports).
* Without `--audio-out`, `--realtime` paces to the wall clock; otherwise the run is as fast as possible.
* `--cem-model v1|v2`: analog voice model. `v1` (default) is the released model; `v2` is the evidence-refined CEM3387
  model (datasheet sections with the EIII capacitors, 192 kHz + FIR decimation; `analysis/CEM3387_MODEL_V2_REPORT.md`).
  `--cem-calib FILE` loads measured chip parameters for v2 (`analysis/CEM3387_CALIBRATION_FORMAT.md`).

Offline / regression use:

    ./eiii-standalone --hard ... --cdrom ... --autoload-bank 44 --preset 1 --seconds 20 \
        --script notes.txt --wav out.wav --stats

Script lines are `TIME COMMAND ARGS` in emulated seconds: `press NAME [HOLD]`, `down|up NAME`,
`extra-press Enter|Master|...`, `key-down INDEX VEL`, `key-up INDEX`, `midi HEXBYTES`, `scan HEX`, `lcd`, `cv VOICE`,
`dump PREFIX`, `pot INPUT VALUE` (ADC multiplexer input X0-X7, firmware ADC units 0-0x7FFF), `quit`. Panel names are the MAME port names (`Load Bank`, `Keypad 4`, `Cursor Left`, ...).
`./eiii-standalone --help` lists every option.

## Front panel (GUI)

    ./eiii-standalone --gui --hard /path/to/EIII_OS242_96MB.hd --cdrom "/path/to/factory.iso" \
        [--audio-out plughw:0,0] [--midi-in hw:1,0,0] [--autoload-bank 44 --preset 1] [--scale 150]

The 4U rack front panel (vector artwork, any size; `--scale 100|125|150|200`, window resizable with a fixed aspect
ratio). Mouse-down on a button is the switch's make, mouse-up its break; the three sliders drive the ADC inputs; the LCD
and the 17 LEDs show the controller RAM and LED latches. POWER and Contrast are host controls: POWER cold-boots;
a click on Contrast cycles the display theme (Stock STN, Blue/White, Amber/Red, VFD, OLED), turning it (drag up/down or
wheel) sets that theme's contrast/brightness/glow. The display settings persist in `$XDG_CONFIG_HOME/eiii/gui.state`
(default `~/.config/eiii/gui.state`) and, in the plugin, in the CLAP state.
Keyboard: digits = keypad, Return = Enter, arrows = Cursor/Page, +/- = Yes/No. Needs libX11/libXext.

## CLAP plugin

    make plugin            # -> build/pic/EIII-Rack.clap  (copy to ~/.clap/)

One emulated EIII per plugin instance, the rack panel as its embedded X11 GUI, MIDI in (note events are converted to
MIDI bytes for the emulated MIDI IN), stereo out, latency reported to the host (1024 frames at 48 kHz, or 2 x block + 512).
There are no plugin parameters — the panel is the instrument. Media come from the environment or `~/.config/eiii/eiii.conf`:

    rom_dir = /path/to/roms
    hd      = /path/to/EIII_OS242_96MB.hd
    cd      = /path/to/EIII.01 Emulator Standards.iso
    autoload_bank = 44      # optional: the plugin operates Load Bank / Enter / ... on the panel after power-on
    autoload_preset = 1

The factory banks receive MIDI on channel 14 (a setting of the bank, changeable on the instrument). A new host sample
rate restarts the instrument from power-on; deactivate/activate otherwise keeps it running.

## Verification

`make check` (needs the media; paths can be overridden with `EIII_ROM_DIR`, `EIII_HD`, `EIII_CD`, `TRACES`):

| Script | Proves |
|---|---|
| `check_boot.sh` | instruction stream identical to MAME from reset to OS ready (5.44 M instructions), RAM identical, NCR5380/WD1772/LCD event streams identical, images unchanged |
| `check_panel.sh` | panel-driven UI session (MASTER → SPECIAL → AUDITION KEY) instruction-identical to MAME (12.3 M instructions) |
| `check_factory_load.sh` | original-UI factory bank load: RAM identical to MAME after loading, 1213 LCD writes and 45 CD READs identical, bank bytes identical to the CD file, P01 Crisp Sax, MIDI note → voice start = MAME, CV + audible stereo vs MAME |
| `check_runtime.sh` | `--autoload-bank/--preset`, realtime capacity, FTZ equivalence, live ALSA MIDI chain (virtual port); `EIII_AUDIO_TEST=plughw:0,0` adds a live audio XRUN test |
| `check_audio.sh` | legacy harness-injected bank (kept as a regression only) |
| `check_cem_v2.sh` | `--cem-model v2` end to end: same firmware voice start as V1, Crisp Sax render pinned to the recorded V2 reference |
| `build/default/tests/lcd_mapping` | HD44780 2 x 40 DDRAM -> 4 x 20 rows (verified against the manual), shift, cursor, blink, CGRAM |
| `build/default/tests/panel_functional` | from power-on, only through the PanelUI adapter: Load Bank, Enter, 3, Enter, 44, Enter, 01, MIDI note, Master Volume, Master Tune + Data slider, module LEDs, Supermode, Mode, Play/Stop, Transpose, Record, Erase |
| `build/default/tests/engine_instances` | three concurrent instances: identical inputs give bit-identical audio (reading the panel has no effect), independence |
| `build/default/tests/display_themes` | Contrast knob: click cycles the 5 display themes, drag/wheel change only the current theme's level, each theme renders differently (OLED = true black), state text round trip, machine untouched |
| `build/default/tools/clap_host_test` | CLAP host: 2 instances, realtime audio + MIDI, GUI create/resize/destroy cycles, rapid presses, slider drags, Contrast clicks -> CLAP state (mark_dirty on the main thread, restored into the second instance) |

`cd .. && make check` runs the eiii-core unit tests (F-Chip bit-exact vs MAME traces, CEM laws vs the Python reference,
CEM bit-exact golden).

## Performance

i5-3570K (2012, 3.4 GHz): about **5× realtime capacity** with boot, SCSI bank load and notes; live audio at 1.00× with
0 XRUNs. Details: `analysis/STANDALONE_PERFORMANCE.md`.

## Known limitations

* Only the paths exercised by the regressions are verified against MAME; the floppy controller model covers the no-disk
  boot path only (inserting floppy images is not supported).
* No MIDI OUT, no sampling input (ADC audio), no SMPTE/sync, no metronome click, no individual outputs.
* No state save/restore; OS writes to the hard disk are discarded at exit (images are never modified).
* The analog voice model (CEM3387) follows the datasheet and schematics; it has not been compared to a real EIII.
* ARM64 (Raspberry Pi) builds are expected to work but are untested: `analysis/ARM64_PORTABILITY_AUDIT.md`.
