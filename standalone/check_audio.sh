#!/bin/sh
# Live audio through the standalone (no MAME): harness-injected Tenor Sax bank at 20 s, MIDI note 60 ch1 26-27 s
# (same stimulus as MAME regression F04).  Expect MAME's note start and an envelope within 1 dB of MAME's F04 output.
set -e
BIN=${EIII_BIN:-./eiii-standalone}
export EIII_ROM_DIR=${EIII_ROM_DIR:-$HOME/EIII_RE/20261003_LINUX_BOOT_PROVEN/roms}
S=${TRACES:-$HOME/EIII_RE/analysis/traces/standalone_spike}
OUT=$("$BIN" --hard /home/paul/Downloads/EIIIOS2_42_HardDiskImages/EIIIOS2_42_HDImage_96MB.ISO \
      --cdrom "/home/paul/Downloads/EIII.01 Emulator Standards.iso" --cycles 300000000 --inject-bank-at 20.0 \
      --midi "26.0:903C7F,27.0:803C40" --wav "$S/standalone_note60.wav" 2>&1)
echo "$OUT" | grep -q "START t=26.00.* v=00 pitch=01FE start=1247B6 loop=12CFDA end=12F0C7" \
  && echo "PASS note start identical to MAME F04 (01FE/1247B6/12CFDA/12F0C7)" || { echo "FAIL note start"; echo "$OUT"; exit 1; }
python3 - "$S" <<'PY'
import wave, array, math, glob, sys
S=sys.argv[1]+'/'
F=sorted(glob.glob('/home/paul/EIII_RE/analysis/traces/fidelity_runs/*/F04_midi_note/out.wav'))[-1]
def env(fn, ch, win=0.05):
    w=wave.open(fn); n=w.getnframes(); c=w.getnchannels(); sr=w.getframerate()
    a=array.array('h', w.readframes(n)); m=a[ch::c]; W=int(sr*win)
    return {round(i/sr,2): math.sqrt(sum(x*x for x in m[i:i+W])/W) for i in range(0,len(m)-W,W)}
M=env(F,1); L=env(S+'standalone_note60.wav',0)
pm=max(M.values()); pl=max(L.values()); db=lambda x,p: 20*math.log10(x/p) if x>0 else -120
d=[abs(db(M[t],pm)-db(L[t],pl)) for t in L if t in M and db(M[t],pm)>-30]
ok=len(d)>15 and max(d)<1.0
print(('PASS' if ok else 'FAIL')+' standalone envelope vs MAME F04: %d frames, max %.2f dB'%(len(d),max(d)))
sys.exit(0 if ok else 1)
PY
