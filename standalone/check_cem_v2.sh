#!/bin/sh
# CEM3387_MODEL_V2 end-to-end regression (V1 stays the release default; see analysis/CEM3387_MODEL_V2_REPORT.md).
# Original UI autoload of bank 44 / P01 Crisp Sax, MIDI note 60 ch 14 through the Z85230, rendered with --cem-model v2.
# The firmware/F-Chip side must be identical to V1 (same voice start); the audio is pinned to a recorded V2 reference
# (bit-identical, or within 1e-4 of full scale on a platform whose libm differs).
set -e
BIN=${EIII_BIN:-./eiii-standalone}
export EIII_ROM_DIR=${EIII_ROM_DIR:-$HOME/EIII_RE/20261003_LINUX_BOOT_PROVEN/roms}
S=${TRACES:-$HOME/EIII_RE/analysis/traces/standalone_spike}
R=${CEM_TRACES:-$HOME/EIII_RE/analysis/traces/cem_v2}
HD=${EIII_HD:-/home/paul/Downloads/EIIIOS2_42_HardDiskImages/EIIIOS2_42_HDImage_96MB.ISO}
CD=${EIII_CD:-/home/paul/Downloads/EIII.01 Emulator Standards.iso}
printf '16.0 midi 9D3C64\n17.5 midi 8D3C40\n' > "$S/cem_v2_notes.txt"
OUT=$("$BIN" --hard "$HD" --cdrom "$CD" --autoload-bank 44 --preset 1 --seconds 19 --script "$S/cem_v2_notes.txt" --cem-model v2 --wav "$S/cem_v2_crisp.wav" 2>&1)
echo "$OUT" | grep -q "analog model: CEM3387_MODEL_V2" && echo "PASS --cem-model v2 selected CEM3387_MODEL_V2" || { echo "FAIL model selection"; exit 1; }
echo "$OUT" | grep -q "^START t=16.00[0-9]* v=00 pitch=01FE start=1247B6 loop=12CFDA end=12F0C7" \
  && echo "PASS firmware voice start identical to V1/MAME F04 (analog model does not affect the digital side)" || { echo "FAIL start"; exit 1; }
if [ "$1" = "--record" ]; then cp "$S/cem_v2_crisp.wav" "$R/ref_crisp_autoload_v2.wav"; echo "recorded $R/ref_crisp_autoload_v2.wav"; exit 0; fi
python3 - "$S/cem_v2_crisp.wav" "$R/ref_crisp_autoload_v2.wav" <<'PY'
import sys, numpy as np
def rd(fn):
    d = open(fn, 'rb').read(); i = 12
    while i < len(d):
        cid = d[i:i+4]; n = int.from_bytes(d[i+4:i+8], 'little')
        if cid == b'data': return np.frombuffer(d[i+8:i+8+n], dtype='<i2').astype(float) / 32768
        i += 8 + n
a, b = rd(sys.argv[1]), rd(sys.argv[2])
if len(a) == len(b) and (a == b).all():
    print('PASS V2 Crisp Sax render bit-identical to the recorded V2 reference'); sys.exit(0)
m = np.abs(a[:len(b)] - b[:len(a)]).max() if len(a) == len(b) else 1.0
ok = m < 1e-4
print(('PASS' if ok else 'FAIL') + ' V2 Crisp Sax render vs reference: max |diff| %.2e of full scale (tolerance 1e-4)' % m); sys.exit(0 if ok else 1)
PY
python3 - "$S/cem_v2_crisp.wav" <<'PY'
import sys, numpy as np
d = open(sys.argv[1], 'rb').read(); i = 12
while i < len(d):
    cid = d[i:i+4]; n = int.from_bytes(d[i+4:i+8], 'little')
    if cid == b'data': x = np.frombuffer(d[i+8:i+8+n], dtype='<i2').reshape(-1, 2).astype(float) / 32768; break
    i += 8 + n
seg = x[int(16.0 * 48000):int(17.5 * 48000)]
l, r = np.sqrt((seg[:, 0] ** 2).mean()), np.sqrt((seg[:, 1] ** 2).mean())
ok = l > 0.01 and r > 0.01 and abs(20 * np.log10(l / r)) < 0.5
print(('PASS' if ok else 'FAIL') + ' V2 audible stereo during the note: L %.4f R %.4f rms (balance %+.2f dB)' % (l, r, 20 * np.log10(l / r))); sys.exit(0 if ok else 1)
PY
