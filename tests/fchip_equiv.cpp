// Bit-exact equivalence: replay a MAME F-Chip trace (EIII_FCHIP_TRACE) into eiii::fchip_bank.
// usage: fchip_equiv trace.txt sound_ram.bin
#include "../fchip/eiii_fchip.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s trace sound.bin\n", argv[0]); return 2; }
	FILE *s = fopen(argv[2], "rb");
	if (!s) { perror(argv[2]); return 2; }
	std::vector<uint16_t> ram(0x400000);
	size_t const n = fread(ram.data(), 2, ram.size(), s);
	fclose(s);
	eiii::fchip_bank fc(ram.data(), n);

	FILE *t = fopen(argv[1], "r");
	if (!t) { perror(argv[1]); return 2; }
	char line[512];
	unsigned long long now = 0, lines = 0, reads = 0, writes = 0, ctrl = 0, ticks = 0, pcmvals = 0, mism = 0;
	bool init = false;
	while (fgets(line, sizeof line, t))
	{
		++lines;
		unsigned long long tt = 0;
		char k = line[0];
		if (k == 'S')
		{
			unsigned v, act, cph, chi, rate; unsigned rf[4]; unsigned long long ph;
			sscanf(line + 2, "%llu %u %x %x %x %x %llu %u %u %u %u", &tt, &v, &rf[0], &rf[1], &rf[2], &rf[3], &ph, &rate, &act, &cph, &chi);
			auto &st = fc.state(v);
			for (int i = 0; i < 4; ++i) st.rf[i] = rf[i];
			st.phase = uint32_t(ph); st.rate = uint16_t(rate); st.active = act; st.ctrl_phase = uint8_t(cph); st.ctrl_hi = uint8_t(chi);
			now = tt; init = true;
			continue;
		}
		if (!init) continue;
		sscanf(line + 2, "%llu", &tt);
		if (tt < now) { fprintf(stderr, "time went backwards at line %llu\n", lines); return 1; }
		fc.advance(tt - now);
		now = tt;
		if (k == 'W') { unsigned v, sl, val; sscanf(line + 2, "%*u %u %u %x", &v, &sl, &val); fc.rf_write(v, sl, val); ++writes; }
		else if (k == 'C') { unsigned v, b; sscanf(line + 2, "%*u %u %x", &v, &b); fc.ctrl_write(v, uint8_t(b)); ++ctrl; }
		else if (k == 'R')
		{
			unsigned v, sl, val; sscanf(line + 2, "%*u %u %u %x", &v, &sl, &val); ++reads;
			if (fc.rf_read(v, sl) != val)
			{
				if (mism++ < 10) fprintf(stderr, "READ mismatch line %llu v%u slot%u mame=%08X core=%08X\n", lines, v, sl, val, fc.rf_read(v, sl));
			}
		}
		else if (k == 'P')
		{
			++ticks;
			char *p = line + 2; strtoull(p, &p, 10);
			for (unsigned v = 0; v < 16; ++v)
			{
				long const m = strtol(p, &p, 10); ++pcmvals;
				if (m != fc.pcm(v) && mism++ < 10)
					fprintf(stderr, "PCM mismatch line %llu v%u mame=%ld core=%d\n", lines, v, m, fc.pcm(v));
			}
		}
	}
	fclose(t);
	printf("%s: %llu writes, %llu ctrl bytes, %llu reads, %llu audio ticks (%llu pcm values), mismatches=%llu\n",
		argv[1], writes, ctrl, reads, ticks, pcmvals, mism);
	return mism ? 1 : 0;
}
