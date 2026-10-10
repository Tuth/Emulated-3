#!/bin/sh
# Panel regression P1: original front-panel input path only (scanner FIFO + 0xFA0000 extra switches), no RAM patching.
#   Master (extra switch) -> Keypad 8 -> Keypad 1  =>  MASTER / SPECIAL / AUDITION KEY
# Input timing is MAME's (panel_seq1_exact.txt, derived from mame_panel1_error.log). Golden data from MAME:
#   mame_panel1_pc.sha256  digest of MAME's first N instruction PCs (debugger trace, 0..20 s)
#   mame_panel1_lcdd.txt   MAME's LCD data writes
set -e
BIN=${EIII_BIN:-./eiii-standalone}
export EIII_ROM_DIR=${EIII_ROM_DIR:-$HOME/EIII_RE/20261003_LINUX_BOOT_PROVEN/roms}
S=${TRACES:-$HOME/EIII_RE/analysis/traces/standalone_spike}
HD=/home/paul/Downloads/EIIIOS2_42_HardDiskImages/EIIIOS2_42_HDImage_96MB.ISO
CD="/home/paul/Downloads/EIII.01 Emulator Standards.iso"
OUT=$("$BIN" --hard "$HD" --cdrom "$CD" --cycles 200000000 --script "$S/panel_seq1_exact.txt" \
      --pc-trace "$S/host_panel1_pc.txt" --log "$S/host_panel1.log" 2>&1)
echo "$OUT" | grep -q "LCD 19.900 \[    AUDITION KEY    Pitch:           C3 \] \[                    Select a Key & \[0-9\]\]" \
  && echo "PASS LCD reaches AUDITION KEY via MASTER/SPECIAL" || { echo "FAIL LCD"; echo "$OUT"; exit 1; }
python3 - "$S" <<'PY'
import sys, hashlib, re
S=sys.argv[1]+'/'
N, dig = open(S+'mame_panel1_pc.sha256').read().split()
N=int(N); h=hashlib.sha256(); i=0
for pc in open(S+'host_panel1_pc.txt'):
    pc=pc.strip()
    if i==0 and pc=='000000': continue
    h.update(pc.encode()+b'\n'); i+=1
    if i==N: break
ok = i==N and h.hexdigest()==dig
print(('PASS' if ok else 'FAIL')+' instruction stream identical to MAME through the panel session: %d instructions'%i)
H=[m.group(2) for m in re.finditer(r'LCDD cyc=(\d+) d=([0-9A-F]{2})', open(S+'host_panel1.log').read()) if int(m.group(1))<=200000000]
M=open(S+'mame_panel1_lcdd.txt').read().split()
ok2 = H==M
print(('PASS' if ok2 else 'FAIL')+' LCD data writes identical to MAME: %d'%len(M))
sys.exit(0 if ok and ok2 else 1)
PY
