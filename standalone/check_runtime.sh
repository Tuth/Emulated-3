#!/bin/sh
# Runtime regressions: --autoload-bank/--preset (operator automation of the original UI), FTZ equivalence,
# realtime capacity, live MIDI through ALSA (virtual raw MIDI port), optional live audio device (EIII_AUDIO_TEST=dev).
set -e
BIN=${EIII_BIN:-./eiii-standalone}
export EIII_ROM_DIR=${EIII_ROM_DIR:-$HOME/EIII_RE/20261003_LINUX_BOOT_PROVEN/roms}
S=${TRACES:-$HOME/EIII_RE/analysis/traces/standalone_spike}
HD=${EIII_HD:-/home/paul/Downloads/EIIIOS2_42_HardDiskImages/EIIIOS2_42_HDImage_96MB.ISO}
CD=${EIII_CD:-/home/paul/Downloads/EIII.01 Emulator Standards.iso}
SCRIPT=$S/runtime_autoload_note.txt
printf '15.5 dump %s/host_autoload\n16.0 midi 9D3C64\n17.5 midi 8D3C40\n' "$S" > "$SCRIPT"
run() { "$BIN" --hard "$HD" --cdrom "$CD" --autoload-bank 44 --preset 1 --seconds 19 --script "$SCRIPT" --stats "$@" 2>&1; }
OUT=$(run --wav "$S/runtime_ftz.wav")
echo "$OUT" | grep -q "AUTOLOAD done at .*\[P01 Crisp Sax" && echo "PASS --autoload-bank 44 --preset 1 reaches P01 Crisp Sax via the original UI" || { echo "FAIL autoload"; echo "$OUT" | grep -E "UI|AUTOLOAD"; exit 1; }
python3 - "$S" "$CD" <<'PY'
import sys
S=sys.argv[1]+'/'; cd=open(sys.argv[2],'rb'); base=0x5306F*512+0x4C
ram=open(S+'host_autoload.ram','rb').read(); snd=open(S+'host_autoload.snd','rb').read()
def get(off,n): cd.seek(base+off); return bytearray(cd.read(n))
hdr=get(0,0xCD33); hdr[0x10]=0x01            # bank header +0x10 = currently selected preset: firmware sets it to P01
ok=hdr==ram[0x1A000-0x8000:0x1A000-0x8000+0xCD33] and get(0xCD33,0x3ACD56)==snd[:0x3ACD56] and get(0xCD33+0x3ACD56,0xD00)==snd[0xBFCB00-0x400000:0xBFCB00-0x400000+0xD00]
print(('PASS' if ok else 'FAIL')+' autoloaded bank byte-identical to the CD bank file (header +0x10 = selected preset 01)'); sys.exit(0 if ok else 1)
PY
echo "$OUT" | grep -q "^START t=16.00[0-9]* v=00 pitch=01FE start=1247B6 loop=12CFDA end=12F0C7" && echo "PASS note 60 after autoload: same voice descriptor as MAME F04" || { echo "FAIL start"; exit 1; }
CAP=$(echo "$OUT" | sed -n "s/^STATS emulated \([0-9.]*\) s in \([0-9.]*\) s wall.*/\1 \2/p")
python3 -c "
e,w=map(float,'$CAP'.split()); c=e/w
print(('PASS' if c>=1.25 else 'FAIL')+' realtime capacity %.2fx (unpaced; boot + SCSI bank load + note; target >= 1.25x)'%c)
import sys; sys.exit(0 if c>=1.25 else 1)"
run --no-ftz --wav "$S/runtime_strict.wav" > /dev/null
cmp -s "$S/runtime_ftz.wav" "$S/runtime_strict.wav" && echo "PASS FTZ/DAZ output byte-identical to strict IEEE denormals (--no-ftz)" || { echo "FAIL FTZ changes the audio"; exit 1; }
if command -v aplaymidi > /dev/null && aconnect -l > /dev/null 2>&1; then
	LOG=$S/runtime_live_midi.log
	"$BIN" --hard "$HD" --cdrom "$CD" --autoload-bank 44 --preset 1 --midi-in virtual --realtime --verbose-midi --seconds 24 > "$LOG" 2>&1 &
	PID=$!
	sleep 16
	P=$(aconnect -l | grep -B1 "Virtual RawMIDI" | tail -2 | head -1 | sed 's/client \([0-9]*\):.*/\1/')
	aplaymidi -p "$P:0" "$HOME/EIII_RE/analysis/tests/midi/live_phrase_ch14_nodelay.mid"
	wait $PID
	HB=$(grep -c 'host byte' "$LOG"); SR=$(grep -c 'SCC chA data read' "$LOG"); PN=$(grep -c 'parser: note' "$LOG"); ST=$(grep -c '^START' "$LOG")
	[ "$HB" = 35 ] && [ "$SR" = 35 ] && [ "$PN" = 4 ] && [ "$ST" = 4 ] \
	  && echo "PASS live ALSA MIDI: 35 bytes -> 35 SCC reads -> 4 firmware notes -> 4 voice starts (realtime, $(grep -o 'XRUNs [0-9]*' "$LOG"))" \
	  || { echo "FAIL live MIDI chain: host $HB scc $SR parser $PN starts $ST"; exit 1; }
else
	echo "SKIP live ALSA MIDI (no ALSA sequencer / aplaymidi)"
fi
if [ -n "$EIII_AUDIO_TEST" ]; then
	printf '16.0 midi 9D3C64\n17.5 midi 8D3C40\n' > "$S/runtime_audio_notes.txt"     # no 'dump': it blocks the emulation thread
	A=$("$BIN" --hard "$HD" --cdrom "$CD" --autoload-bank 44 --preset 1 --audio-out "$EIII_AUDIO_TEST" --seconds 19 --script "$S/runtime_audio_notes.txt" 2>&1)
	X=$(echo "$A" | sed -n 's/.*XRUNs \([0-9]*\).*/\1/p')
	[ "$X" = 0 ] && echo "PASS live audio on $EIII_AUDIO_TEST: 19 s incl. boot and bank load, 0 XRUNs" || { echo "FAIL audio XRUNs: $X"; echo "$A" | grep audio; exit 1; }
fi
