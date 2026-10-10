// eiii_fakehost — standalone "fake core" host: replays a recorded authentic-core event stream (MAME
// EIII_FCHIP_TRACE: S/W/C/R/V/P lines) into eiii::software_backend through the output_backend boundary and
// renders stereo audio.  No MAME code is linked.  Acceptance checks against the recorded MAME values:
//   R lines: register-file read-back must match; P lines: per-voice lane output at each MAME audio tick must match.
// usage: eiii_fakehost --trace T --sound S --out O.wav [--rate 48000]
#include "eiii_backend.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char **argv)
{
	std::string trace, sound, out = "out.wav"; double rate = 48000;
	for (int i = 1; i + 1 < argc; i += 2)
	{
		std::string const k = argv[i];
		if (k == "--trace") trace = argv[i + 1]; else if (k == "--sound") sound = argv[i + 1];
		else if (k == "--out") out = argv[i + 1]; else if (k == "--rate") rate = atof(argv[i + 1]);
	}
	if (trace.empty() || sound.empty()) { fprintf(stderr, "usage: %s --trace T --sound S --out O.wav [--rate R]\n", argv[0]); return 2; }

	FILE *s = fopen(sound.c_str(), "rb"); if (!s) { perror(sound.c_str()); return 2; }
	std::vector<uint16_t> ram(0x400000); size_t const words = fread(ram.data(), 2, ram.size(), s); fclose(s);
	eiii::software_backend be(ram.data(), words, rate);
	eiii::output_backend &core_out = be;      // everything below goes through the fork boundary only

	FILE *t = fopen(trace.c_str(), "r"); if (!t) { perror(trace.c_str()); return 2; }
	char line[512]; bool init = false; uint64_t t0 = 0;
	unsigned long long nw = 0, nc = 0, nv = 0, nr = 0, np = 0, rbad = 0, pbad = 0;
	// note-start report (instrumentation, same fields as MAME's EIII_MON_EVENT START)
	uint32_t wstart[16] = {}, wloop[16] = {}, wsize[16] = {}; bool pending[16] = {};
	while (fgets(line, sizeof line, t))
	{
		char const k = line[0]; unsigned long long tt = 0; sscanf(line + 2, "%llu", &tt);
		if (k == 'S')
		{
			unsigned v, act, cph, chi, rt, rf[4]; unsigned long long ph;
			sscanf(line + 2, "%llu %u %x %x %x %x %llu %u %u %u %u", &tt, &v, &rf[0], &rf[1], &rf[2], &rf[3], &ph, &rt, &act, &cph, &chi);
			auto &st = be.lanes().state(v); for (int i = 0; i < 4; ++i) st.rf[i] = rf[i];
			st.phase = uint32_t(ph); st.rate = uint16_t(rt); st.active = act; st.ctrl_phase = uint8_t(cph); st.ctrl_hi = uint8_t(chi);
			if (!init) { be.start_at(tt); t0 = tt; init = true; }
			continue;
		}
		if (!init) continue;
		if (k == 'W')
		{
			unsigned v, sl, val; sscanf(line + 2, "%*u %u %u %x", &v, &sl, &val); core_out.rf_write(tt, v, sl, val); ++nw;
			if (sl == 2) wstart[v] = val & 0x3fffff; else if (sl == 3) wloop[v] = val & 0x3fffff;
			else if (sl == 0) { wsize[v] = val; pending[v] = (val & 0x3fffff) > 1; }
		}
		else if (k == 'C')
		{
			unsigned v, b; sscanf(line + 2, "%*u %u %x", &v, &b); core_out.ctrl_write(tt, v, uint8_t(b)); ++nc;
			auto const &st = be.lanes().state(v);
			if (pending[v] && st.ctrl_phase == 0 && st.rate >= 0x0a4)
			{
				pending[v] = false;
				printf("START t=%.6f v=%02u pitch=%04X start=%06X loop=%06X end=%06X flags=%02X cv=%04X/%04X/%04X/%04X/%04X\n",
					tt / 20e6, v, st.rate, wstart[v], wloop[v], wstart[v] + (wsize[v] & 0x3fffff) + 1, wsize[v] >> 22,
					be.cv_word(v, 0), be.cv_word(v, 1), be.cv_word(v, 2), be.cv_word(v, 3), be.cv_word(v, 4));
			}
		}
		else if (k == 'V') { unsigned b, c, w; sscanf(line + 2, "%*u %u %u %x", &b, &c, &w); core_out.cv_write(tt, b, c, uint16_t(w)); ++nv; }
		else if (k == 'R')
		{
			unsigned v, sl, val; sscanf(line + 2, "%*u %u %u %x", &v, &sl, &val); ++nr;
			if (core_out.rf_read(tt, v, sl) != val) ++rbad;
		}
		else if (k == 'P')
		{
			core_out.render_until(tt); ++np;
			char *p = line + 2; strtoull(p, &p, 10);
			for (unsigned v = 0; v < 16; ++v) if (strtol(p, &p, 10) != be.lanes().pcm(v)) ++pbad;
		}
	}
	fclose(t);
	auto const &a = be.audio();
	double peak = 0; for (float x : a) peak = std::fmax(peak, std::fabs(x));
	printf("events: %llu rf writes, %llu F-Chip bytes, %llu CV writes, %llu read-backs (%llu mismatches), %llu audio ticks (%llu PCM mismatches)\n",
		nw, nc, nv, nr, rbad, np, pbad);
	printf("rendered %.3f s stereo at %.0f Hz from t=%.3f s, peak %.3f FS (FS = 10 V at the jacks)\n", a.size() / 2 / rate, rate, t0 / 20e6, peak);
	FILE *w = fopen(out.c_str(), "wb"); if (!w) { perror(out.c_str()); return 2; }
	uint32_t const bytes = uint32_t(a.size() * 2);
	fwrite("RIFF", 1, 4, w); put32(w, 36 + bytes); fwrite("WAVEfmt ", 1, 8, w); put32(w, 16); put16(w, 1); put16(w, 2);
	put32(w, uint32_t(rate)); put32(w, uint32_t(rate) * 4); put16(w, 4); put16(w, 16); fwrite("data", 1, 4, w); put32(w, bytes);
	for (float x : a) { long v = lround(x * 32767.0); put16(w, uint16_t(int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v))); }
	fclose(w);
	return (rbad || pbad) ? 1 : 0;
}
