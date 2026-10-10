#!/bin/bash
# Panel UI / engine / CLAP regressions (see README.md § Verification).  The GUI part needs an X display (DISPLAY).
set -u
cd "$(dirname "$0")"
export EIII_ROM_DIR=${EIII_ROM_DIR:-$HOME/EIII_RE/20261003_LINUX_BOOT_PROVEN/roms}
export EIII_HD=${EIII_HD:-/home/paul/Downloads/EIIIOS2_42_HardDiskImages/EIIIOS2_42_HDImage_96MB.ISO}
export EIII_CD=${EIII_CD:-/home/paul/Downloads/EIII.01 Emulator Standards.iso}
B=build/default
fail=0
run() { local name=$1; shift; if "$@" > /tmp/eiii_panel_$$.log 2>&1; then echo "PASS $name ($(grep -c '^PASS' /tmp/eiii_panel_$$.log) checks)"; else echo "FAIL $name"; tail -20 /tmp/eiii_panel_$$.log; fail=1; fi; }
run "LCD mapping (DDRAM -> 4x20, cursor, CGRAM)" $B/tests/lcd_mapping
run "functional panel test from power-on via the PanelUI adapter" $B/tests/panel_functional
run "three concurrent instances (bit-identical A/B, independent C)" $B/tests/engine_instances
run "runtime media changes: CD swap / eject / re-insert + Load Bank, HD swap restarts and boots" $B/tests/media_swap
run "sampling through the original OS: host audio -> ADC at 44 053 Hz, Sample Input level, Place Sample, MIDI playback" $B/tests/sampling
run "display themes on the Contrast knob (click cycles, turn = level, state, machine untouched)" $B/tests/display_themes
if [ -x "${CLAP_VALIDATOR:-$HOME/EIII_RE/tools/clap-validator}" ] && [ -f build/pic/EIII-Rack.clap ]; then
	out=$("${CLAP_VALIDATOR:-$HOME/EIII_RE/tools/clap-validator}" validate build/pic/EIII-Rack.clap 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep "tests run")
	if echo "$out" | grep -q " 0 failed"; then echo "PASS clap-validator: $out"; else echo "FAIL clap-validator: $out"; fail=1; fi
fi
if [ -n "${DISPLAY:-}" ] && [ -f build/pic/EIII-Rack.clap ]; then
	rep=/tmp/eiii_rack_report_$$.txt; rm -f $rep
	if EIII_AUTOLOAD_BANK=44 EIII_AUTOLOAD_PRESET=1 EIII_RACK_REPORT=$rep $B/tools/clap_host_test build/pic/EIII-Rack.clap --instances 2 --seconds 40 --gui-cycles 6 --media-test > /tmp/eiii_panel_$$.log 2>&1; then
		echo "PASS CLAP host: $(grep 'GUI cycles' /tmp/eiii_panel_$$.log); $(grep -cE '^PASS (media|menu)' /tmp/eiii_panel_$$.log) media/menu checks; $(grep -c . $rep) instances; underruns: $(grep -o 'underruns [0-9]*' $rep | tr '\n' ' ')"
	else echo "FAIL CLAP host"; tail -20 /tmp/eiii_panel_$$.log; fail=1; fi
	rm -f $rep
else echo "SKIP CLAP host GUI test (no DISPLAY or plugin not built: make plugin)"; fi
rm -f /tmp/eiii_panel_$$.log
exit $fail
