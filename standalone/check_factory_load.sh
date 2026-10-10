#!/bin/sh
# Gate A: AUTHENTIC END-TO-END PLAYBACK WITHOUT RAM INJECTION.
# boot OS 2.42 -> original Load Bank UI (scanner + Enter extra switch, MAME's exact input instants) -> D3 factory CD ->
# bank 44 Tenor Sax -> P01 Crisp Sax -> MIDI note 60 ch14 through the Z85230 -> firmware voice allocation ->
# portable F-Chip -> software CEM3387 -> stereo.  Golden data captured from MAME (mame_load*.txt / .bin).
set -e
BIN=${EIII_BIN:-./eiii-standalone}
export EIII_ROM_DIR=${EIII_ROM_DIR:-$HOME/EIII_RE/20261003_LINUX_BOOT_PROVEN/roms}
S=${TRACES:-$HOME/EIII_RE/analysis/traces/standalone_spike}
HD=${EIII_HD:-/home/paul/Downloads/EIIIOS2_42_HardDiskImages/EIIIOS2_42_HDImage_96MB.ISO}
CD=${EIII_CD:-/home/paul/Downloads/EIII.01 Emulator Standards.iso}
OUT=$(EIII_DEVLOG=1 "$BIN" --hard "$HD" --cdrom "$CD" --seconds 58 --script "$S/factory_load_e2e.txt" \
      --dump-at "490000000:E820:$S/host_ram_load49.bin" --log "$S/host_factory.log" --wav "$S/factory_e2e.wav" 2>&1)
echo "$OUT" | grep -q HARNESS && { echo "FAIL a RAM-injection harness ran"; exit 1; }
echo "PASS no RAM injection, no host-side voice triggering (inputs: scanner bytes, extra switches, MIDI bytes only)"
cmp -s "$S/host_ram_load49.bin" "$S/mame_ram_load49.bin" && echo "PASS main RAM identical to MAME at 49 s (after firmware bank load + preset select)" || { echo "FAIL RAM differs from MAME"; exit 1; }
python3 - "$S" "$CD" <<'PY'
import sys, re, glob, wave, array, math
S=sys.argv[1]+'/'; CD=sys.argv[2]; ok=True
def res(c, msg):
    global ok; ok &= bool(c); print(('PASS ' if c else 'FAIL ')+msg)
log=open(S+'host_factory.log', errors='ignore').read()
lh=[d for c,d in re.findall(r'LCDD cyc=(\d+) d=([0-9A-F]{2})', log) if int(c) < 490000000]
lm=open(S+'mame_load_lcdd.txt').read().split()
res(lh==lm, 'LCD data writes identical to MAME through the load: %d'%len(lm))
ch=re.findall(r'command (READ start=\w+ blocks=\w+)', log)
cm=open(S+'mame_load_cdreads.txt').read().split('\n'); cm=[c for c in cm if c]
res(ch==cm, 'CD-ROM READ commands identical to MAME: %d (bank 44 at LBA 0x5306F)'%len(cm))
cd=open(CD,'rb'); base=0x5306F*512+0x4C
ram=open(S+'host_factory_after_load.ram','rb').read(); snd=open(S+'host_factory_after_load.snd','rb').read()
def same(off,n,mem,memoff): cd.seek(base+off); return cd.read(n)==mem[memoff:memoff+n]
res(same(0,0xCD33,ram,0x1A000-0x8000) and same(0xCD33,0x3ACD56,snd,0) and same(0xCD33+0x3ACD56,0xD00,snd,0xBFCB00-0x400000),
    'firmware-loaded bank byte-identical to the CD bank file (52531 + 3853654 + 3328 bytes)')
PY
echo "$OUT" | grep -q "LCD 48.000 \[P01 Crisp Sax" && echo "PASS P01 Crisp Sax selected through the original UI" || { echo "FAIL preset"; exit 1; }
echo "$OUT" | grep -q "^START t=53.00[0-9]* v=00 pitch=01FE start=1247B6 loop=12CFDA end=12F0C7" \
  && echo "PASS MIDI note 60 -> firmware voice 0, rate 01FE, start 1247B6, loop 12CFDA, end 12F0C7 (= MAME F04)" || { echo "FAIL voice start"; echo "$OUT" | grep START; exit 1; }
echo "$OUT" | grep -E "^CV 5[36].500" | sed 's/^/     /'
echo "$OUT" | grep -q "^CV 53.500 voice 0 .*Gain=[1-9A-F]" && echo "$OUT" | grep -q "^CV 56.500 voice 0 .*Gain=0000" \
  && echo "PASS CV activity: voice 0 gain open during the note (0x3E42 = MAME causal VCA peak), closed after release" || { echo "FAIL CV activity"; exit 1; }
python3 - "$S" <<'PY'
import sys, wave, array, math, glob
S=sys.argv[1]+'/'
def env(fn, ch, win=0.05, t0=0):
    w=wave.open(fn); n=w.getnframes(); c=w.getnchannels(); sr=w.getframerate()
    a=array.array('h', w.readframes(n)); m=a[ch::c]; W=int(sr*win)
    return {round(i/sr-t0,2): math.sqrt(sum(x*x for x in m[i:i+W])/W) for i in range(0,len(m)-W,W)}
L=env(S+'factory_e2e.wav',0,t0=27.0); R=env(S+'factory_e2e.wav',1,t0=27.0)   # note at 53 s here, 26 s in MAME F04
F=sorted(glob.glob('/home/paul/EIII_RE/analysis/traces/fidelity_runs/*/F04_midi_note/out.wav'))[-1]
M=env(F,1)
pm=max(M.values()); pl=max(L.values()); db=lambda x,p: 20*math.log10(x/p) if x>0 else -120
d=[abs(db(M[t],pm)-db(L[t],pl)) for t in L if t in M and 26.0 <= t < 27.0 and db(M[t],pm)>-30]   # both notes held 26..27
ok=len(d)>=18 and max(d)<1.0 and max(R.values())>0.1*pl
print(('PASS' if ok else 'FAIL')+' audible stereo PCM: envelope vs MAME F04 max %.2f dB over %d frames of the held note; L peak %.0f R peak %.0f'%(max(d),len(d),pl,max(R.values())))
sys.exit(0 if ok else 1)
PY
echo "AUTHENTIC END-TO-END PLAYBACK WITHOUT RAM INJECTION: YES"
