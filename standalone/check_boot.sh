#!/bin/sh
# Standalone boot equivalence with MAME (golden references in analysis/traces/standalone_spike, captured from MAME):
#  1. instruction stream identical to MAME's debugger trace from reset to OS 2.42 ready (mame_full_pc.txt)
#  2. main RAM identical at the first TG-ISR entry (0xE820) after cycle 90,000,000 (mame_ram_90M.bin)
#  3. NCR5380 / WD1772 / LCD data events identical in order and data (mame_boot_events.txt)
#  4. the proven disk images are untouched (sha256 recorded in image_hashes.txt)
set -e
BIN=${EIII_BIN:-./eiii-standalone}
export EIII_ROM_DIR=${EIII_ROM_DIR:-$HOME/EIII_RE/20261003_LINUX_BOOT_PROVEN/roms}
S=${TRACES:-$HOME/EIII_RE/analysis/traces/standalone_spike}
HD=/home/paul/Downloads/EIIIOS2_42_HardDiskImages/EIIIOS2_42_HDImage_96MB.ISO
CD="/home/paul/Downloads/EIII.01 Emulator Standards.iso"
"$BIN" --hard "$HD" --cdrom "$CD" --cycles 95000000 --pc-trace "$S/host_os_pc.txt" --dump-at "90000000:E820:$S/host_ram_90M.bin" > /dev/null 2>&1
python3 - "$S" <<'PY'
import sys
S=sys.argv[1]+'/'
h=open(S+'host_os_pc.txt').read().split()
if h and h[0]=='000000': h=h[1:]
i=0; bad=False
with open(S+'mame_full_pc.txt') as f:
    for line in f:
        if len(line)<8 or line[6]!=':': continue
        if i>=len(h) or line[:6]!=h[i]: bad=True; break
        i+=1
print(('FAIL' if bad else 'PASS')+' instruction stream identical to MAME: %d instructions'%i)
sys.exit(1 if bad else 0)
PY
cmp -s "$S/host_ram_90M.bin" "$S/mame_ram_90M.bin" && echo "PASS RAM 0x8000-0x27FFF identical at cycle ~90 M" || { echo "FAIL RAM differs"; exit 1; }
"$BIN" --hard "$HD" --cdrom "$CD" --cycles 40000000 --log "$S/host_boot.log" > /dev/null 2>&1
grep -E "^(NCR|FDC|LCDD) " "$S/host_boot.log" | grep "d=" > "$S/host_boot_events.txt"
EV=$(python3 "$S/compare_events.py"); echo "     $EV"   # (no tee /dev/stderr: it truncates a redirected log)
echo "$EV" | grep -q "host 12577; identical (kind,addr,data) prefix 12577" && echo "PASS NCR/FDC/LCD event streams identical" || { echo "FAIL events"; exit 1; }
sha256sum "$HD" "$CD" | cmp -s - "$S/image_hashes.txt" && echo "PASS proven disk images unchanged" || { echo "FAIL image hashes"; exit 1; }
