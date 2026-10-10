#!/usr/bin/env python3
"""Standalone acceptance (steps D/E): the eiii-core fake host, fed the authentic core's event stream recorded from
MAME, must reproduce MAME's note starts exactly, its lane output and read-backs bit-exactly, and its audio envelope
within 1 dB above -30 dB (MAME's monitor is linear-law mono; the software CEM is stereo with the datasheet VCA law).
usage: standalone_acceptance.py <dir with phrase.trace, phrase.snd, phrase_mame_error.log, phrase_mame_monitor.wav>"""
import math, os, re, subprocess, sys, wave, array
A = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/EIII_RE/analysis/traces/standalone_accept')
FH = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'eiii_fakehost')
out = subprocess.run([FH, '--trace', f'{A}/phrase.trace', '--sound', f'{A}/phrase.snd', '--out', f'{A}/phrase_softcem.wav',
                      '--rate', '48000'], capture_output=True, text=True)
fake = re.findall(r'START t=\S+ v=(\d+) pitch=(\w+) start=(\w+) loop=(\w+) end=(\w+)', out.stdout)
mame = re.findall(r'v=(\d+) type=START pitch=(\w+) start=(\w+) loop=(\w+) end=(\w+)', open(f'{A}/phrase_mame_error.log').read())
ev = re.search(r'(\d+) read-backs \((\d+) mismatches\), (\d+) audio ticks \((\d+) PCM mismatches\)', out.stdout)

def env(fn, ch, t0=0.0, win=0.1):
    w = wave.open(fn); n = w.getnframes(); c = w.getnchannels(); sr = w.getframerate()
    a = array.array('h', w.readframes(n)); m = a[ch::c]; W = int(sr * win)
    return {round(t0 + i / sr, 1): math.sqrt(sum(x * x for x in m[i:i + W]) / W) for i in range(0, len(m) - W, W)}
M = env(f'{A}/phrase_mame_monitor.wav', 1); L = env(f'{A}/phrase_softcem.wav', 0, 24.5); R = env(f'{A}/phrase_softcem.wav', 1, 24.5)
pm, pl = max(M.values()), max(L.values())
db = lambda x, p: 20 * math.log10(x / p) if x > 0 else -120
d = [abs(db(M[t], pm) - db(L[t], pl)) for t in L if t in M and db(M[t], pm) > -30]
bal = [db(L[t], pl) - db(R[t], pl) for t in L if L[t] > pl * 0.03]
checks = [
    ('fake host ran (exit 0)', out.returncode == 0, out.returncode),
    ('11 note starts identical to MAME (voice, pitch, start, loop, end)', fake == mame and len(mame) == 11, (len(fake), len(mame))),
    ('lane output bit-exact at every MAME audio tick', bool(ev) and ev.group(4) == '0' and int(ev.group(3)) > 300000, ev and ev.groups()),
    ('envelope within 1 dB of MAME above -30 dB', bool(d) and max(d) < 1.0, (len(d), round(max(d), 2) if d else None)),
    ('stereo balanced at centre pan (|L-R| < 0.5 dB)', bool(bal) and abs(sum(bal) / len(bal)) < 0.5, round(sum(bal) / len(bal), 2) if bal else None),
]
for name, ok, val in checks: print('%s %s  [%s]' % ('PASS' if ok else 'FAIL', name, val))
sys.exit(0 if all(c[1] for c in checks) else 1)
