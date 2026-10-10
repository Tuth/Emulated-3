// Offline renderer: authentic-core event trace (MAME EIII_FCHIP_TRACE with V lines) -> eiii::fchip_bank
// -> eiii::cem3387_bank -> stereo 16-bit WAV.  usage: render_trace trace sound.bin out.wav [rate=96000]
#include "../fchip/eiii_fchip.h"
#include "../cem/eiii_cem3387.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: %s trace sound.bin out.wav [rate]\n", argv[0]); return 2; }
	unsigned const out_rate = argc > 4 ? unsigned(atoi(argv[4])) : 96000;
	unsigned const os = 2;                                  // CEM model runs at 2x the output rate
	double const fs = double(out_rate) * os;
	FILE *s = fopen(argv[2], "rb"); if (!s) { perror(argv[2]); return 2; }
	std::vector<uint16_t> ram(0x400000); size_t const n = fread(ram.data(), 2, ram.size(), s); fclose(s);
	eiii::fchip_bank fc(ram.data(), n);
	eiii::cem3387_bank cem; cem.prepare(fs);

	FILE *t = fopen(argv[1], "r"); if (!t) { perror(argv[1]); return 2; }
	std::vector<int16_t> out;
	char line[512];
	bool init = false;
	unsigned long long now = 0;          // 20 MHz ticks
	double next_sample = 0;              // in 20 MHz ticks
	double const tick_per_sample = 20e6 / fs;
	double accl = 0, accr = 0; unsigned k = 0;
	auto render_until = [&](unsigned long long tt) {
		while (next_sample < double(tt))
		{
			unsigned long long const ts = (unsigned long long)next_sample;
			if (ts > now) { fc.advance(ts - now); now = ts; }
			int16_t pcm[16]; for (unsigned v = 0; v < 16; ++v) pcm[v] = fc.pcm(v);
			double l, r; cem.process(pcm, l, r);
			accl += l; accr += r;
			if (++k == os)
			{
				auto cv = [&](double x) { x = x / os / 10.0 * 32767.0; return int16_t(x > 32767 ? 32767 : x < -32768 ? -32768 : x); };
				out.push_back(cv(accl)); out.push_back(cv(accr)); accl = accr = 0; k = 0;
			}
			next_sample += tick_per_sample;
		}
		if (tt > now) { fc.advance(tt - now); now = tt; }
	};
	while (fgets(line, sizeof line, t))
	{
		char const kind = line[0];
		unsigned long long tt = 0; sscanf(line + 2, "%llu", &tt);
		if (kind == 'S')
		{
			unsigned v, act, cph, chi, rate, rf[4]; unsigned long long ph;
			sscanf(line + 2, "%llu %u %x %x %x %x %llu %u %u %u %u", &tt, &v, &rf[0], &rf[1], &rf[2], &rf[3], &ph, &rate, &act, &cph, &chi);
			auto &st = fc.state(v); for (int i = 0; i < 4; ++i) st.rf[i] = rf[i];
			st.phase = uint32_t(ph); st.rate = uint16_t(rate); st.active = act; st.ctrl_phase = uint8_t(cph); st.ctrl_hi = uint8_t(chi);
			if (!init) { now = tt; next_sample = double(tt); init = true; }
			continue;
		}
		if (!init) continue;
		if (kind == 'V') { unsigned b, c, w; sscanf(line + 2, "%*u %u %u %x", &b, &c, &w); if (tt > now) render_until(tt); if (b < 5) cem.cv_write(b, c, uint16_t(w)); continue; }
		render_until(tt);
		if (kind == 'W') { unsigned v, sl, val; sscanf(line + 2, "%*u %u %u %x", &v, &sl, &val); fc.rf_write(v, sl, val); }
		else if (kind == 'C') { unsigned v, b; sscanf(line + 2, "%*u %u %x", &v, &b); fc.ctrl_write(v, uint8_t(b)); }
	}
	fclose(t);
	FILE *w = fopen(argv[3], "wb"); if (!w) { perror(argv[3]); return 2; }
	uint32_t const bytes = uint32_t(out.size() * 2);
	fwrite("RIFF", 1, 4, w); put32(w, 36 + bytes); fwrite("WAVEfmt ", 1, 8, w); put32(w, 16); put16(w, 1); put16(w, 2);
	put32(w, out_rate); put32(w, out_rate * 4); put16(w, 4); put16(w, 16); fwrite("data", 1, 4, w); put32(w, bytes);
	fwrite(out.data(), 2, out.size(), w); fclose(w);
	printf("%s: %zu frames at %u Hz (model %g Hz); full scale = 10 V at the jacks\n", argv[3], out.size() / 2, out_rate, fs);
	return 0;
}
