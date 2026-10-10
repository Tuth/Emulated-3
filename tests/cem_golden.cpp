// CEM3387 bank bit-exactness gate + micro-benchmark.
// Drives all 16 voices with a deterministic stimulus shaped like real firmware use (500 Hz CV updates while a voice is
// active, constant CV words while idle, held PCM after a one-shot ends, long silent stretches) and hashes the exact bit
// patterns of every output sample.  Any optimisation of cem/ must reproduce the recorded hash exactly.
//   usage: cem_golden [--v2] [expected_hash_hex [reference_file]] | [--v2] --record FILE
//   --v2: CEM3387_MODEL_V2 bank at 192 kHz (same stimulus, CV writes every 768 samples = 500 Hz)
// If the exact hash differs (another CPU/libm: tan/tanh/exp may differ by an ulp), every 997th output sample is compared
// with the stored reference (absolute tolerance 1e-9 V, ~180 dB below the signal); `--record FILE` writes that reference.
#include "../cem/eiii_cem3387.h"
#include "../cem/eiii_cem3387_v2.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
struct lcg { uint32_t s; uint32_t next() { s = s * 1664525u + 1013904223u; return s; } };
uint64_t fnv(uint64_t h, double d) { uint64_t b; std::memcpy(&b, &d, 8); for (int i = 0; i < 8; ++i) { h ^= (b >> (8 * i)) & 0xff; h *= 0x100000001b3ull; } return h; }
}

int main(int argc, char **argv)
{
	bool const v2 = argc > 1 && !std::strcmp(argv[1], "--v2");
	if (v2) { --argc; ++argv; }
	double const fs = v2 ? 192000.0 : 96000.0;  // V1: 48 kHz x 2; V2: 48 kHz x 4
	unsigned const seconds = 6, n = unsigned(fs) * seconds;
	eiii::cem3387_bank bank1;
	eiii::cem3387_v2_bank bank2;
	eiii::cem_calibration const cal;
	if (v2) bank2.prepare(cal, fs, eiii::cem_v2_options{}); else bank1.prepare(fs);
	auto cv_write = [&](unsigned b, unsigned c, uint16_t w) { if (v2) bank2.cv_write(b, c, w); else bank1.cv_write(b, c, w); };
	lcg rng{12345};
	int16_t pcm[16] = {};
	bool active[16] = {};
	unsigned next_change[16];
	for (unsigned v = 0; v < 16; ++v) next_change[v] = rng.next() % unsigned(fs);
	// idle CV words as the firmware leaves them (Fc1/Fc2 open, Q off, gain closed, pan centre)
	for (unsigned v = 0; v < 16; ++v)
	{
		unsigned const c = 15 - v;
		cv_write(0, c, 0x3ffc); cv_write(1, c, 0x3ffc); cv_write(2, c, 0x3ffc); cv_write(3, c, 0); cv_write(4, c, 0x2000);
	}
	uint64_t h = 0xcbf29ce484222325ull;
	std::vector<double> dec;
	unsigned phase[16] = {}, env[16] = {};
	auto t0 = std::chrono::steady_clock::now();
	for (unsigned i = 0; i < n; ++i)
	{
		for (unsigned v = 0; v < 16; ++v)
		{
			if (i == next_change[v])
			{
				// at most 4 voices sound at once (typical); otherwise go idle holding the last PCM value
				unsigned na = 0; for (bool a : active) na += a;
				active[v] = !active[v] && na < 4;
				next_change[v] = i + unsigned(fs * (active[v] ? 0.3 + (rng.next() % 1000) / 1000.0 : 0.5 + (rng.next() % 2000) / 1000.0));
				env[v] = 0;
			}
			if (active[v])
			{
				phase[v] += 1200 + 97 * v;
				pcm[v] = int16_t(int(phase[v] & 0xffff) - 0x8000) / 2 + int16_t(rng.next() >> 22) - 512;
			}
			else if ((i % 4096) == 0 && v == 3)
				pcm[v] = 0;                       // some voices return to 0, others keep holding
		}
		if ((i % unsigned(fs / 500)) == 0)        // TG tick 500 Hz: firmware CV updates for active voices
			for (unsigned v = 0; v < 16; ++v)
			{
				unsigned const c = 15 - v;
				if (active[v])
				{
					env[v] = std::min(env[v] + 1, 400u);
					unsigned const g = env[v] < 50 ? env[v] * 300 : 15000 - (env[v] - 50) * 20;
					cv_write(3, c, uint16_t(g & 0x3ffc));
					cv_write(1, c, uint16_t((0x2800 + (env[v] * 13) % 0x1000) & 0x3ffc));
					cv_write(0, c, 0x3800);
					cv_write(2, c, uint16_t(0x3000 + v * 64));
					cv_write(4, c, uint16_t(0x1000 + v * 0x200));
				}
				else if (env[v] == 0)
				{
					cv_write(3, c, 0);               // gain closed, rewritten every tick like the firmware
					env[v] = 1;
				}
			}
		double l, r;
		if (v2) bank2.process(pcm, l, r); else bank1.process(pcm, l, r);
		h = fnv(fnv(h, l), r);
		if (i % 997 == 0) { dec.push_back(l); dec.push_back(r); }
	}
	double const ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (double(n) * 16.0);
	std::printf("cem_golden%s: %u s at %.0f Hz, hash %016llx, %.1f ns per voice-sample (%.2fx realtime for 16 voices)\n",
		v2 ? " (V2)" : "", seconds, fs, (unsigned long long)h, ns, 1e9 / (ns * 16.0 * fs));
	if (argc > 2 && !std::strcmp(argv[1], "--record"))
	{
		FILE *f = std::fopen(argv[2], "w");
		for (double d : dec) std::fprintf(f, "%.17g\n", d);
		std::fclose(f);
		std::printf("recorded %zu reference samples to %s\n", dec.size(), argv[2]);
		return 0;
	}
	if (argc > 1)
	{
		if (std::strtoull(argv[1], nullptr, 16) == h) { std::printf("PASS CEM bank output bit-identical to recorded golden\n"); return 0; }
		if (argc < 3) { std::printf("FAIL CEM bank hash differs from recorded golden\n"); return 1; }
		FILE *f = std::fopen(argv[2], "r");
		if (!f) { std::printf("FAIL hash differs and reference %s missing\n", argv[2]); return 1; }
		double maxd = 0; size_t n2 = 0; double v;
		while (n2 < dec.size() && std::fscanf(f, "%lf", &v) == 1) { maxd = std::max(maxd, std::fabs(v - dec[n2])); ++n2; }
		std::fclose(f);
		bool const ok = n2 == dec.size() && maxd < 1e-9;
		std::printf("%s CEM bank hash differs (platform libm/FPU), %zu reference samples within %.3g V (tolerance 1e-9 V)\n", ok ? "PASS" : "FAIL", n2, maxd);
		return ok ? 0 : 1;
	}
	return 0;
}
