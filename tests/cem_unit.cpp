// Behavioural checks of eiii::cem3387_voice against the CEM3387 datasheet laws and EIII schematic values.
#include "../cem/eiii_cem3387.h"
#include <cmath>
#include <cstdio>

using namespace eiii;
static int fails = 0;
static constexpr double FS = 192000.0, PI = 3.14159265358979323846;

static void check(char const *name, double got, double lo, double hi, char const *unit)
{
	bool ok = got >= lo && got <= hi;
	printf("%s %-58s %10.3f %s  [%g .. %g]\n", ok ? "PASS" : "FAIL", name, got, unit, lo, hi);
	fails += !ok;
}

// steady-state RMS gain (dB) of the voice for a sine at f, CVs given as firmware words
static double gain_at(double f, uint16_t fc1, uint16_t fc2, uint16_t q, uint16_t gain = 0x3FFC, uint16_t pan = 0x3FFC)
{
	cem3387_voice v; v.set_cv(0, fc1); v.set_cv(1, fc2); v.set_cv(2, q); v.set_cv(3, gain); v.set_cv(4, pan); v.prepare(FS);
	double in2 = 0, out2 = 0;
	int const n = int(FS * 0.6), skip = int(FS * 0.3);
	for (int i = 0; i < n; ++i)
	{
		double const s = 0.1 * std::sin(2 * PI * f * i / FS);
		double l, r; v.process(int16_t(s * 32767), l, r);
		if (i >= skip) { in2 += s * s; out2 += l * l; }
	}
	return 10 * std::log10(out2 / in2);
}

int main()
{
	check("word 0x3994 -> volts (DAC-6012 4.96 V FS)", cem_law::word_to_volts(0x3994), 4.46, 4.47, "V");
	check("Fc2 at 0x2000 (datasheet law)", cem_law::fc2_hz(cem_law::word_to_volts(0x2000)), 3600, 3680, "Hz");
	check("Fc1 at 0x3994 (pitch-tracking default)", cem_law::fc1_hz(cem_law::word_to_volts(0x3994)), 93000, 96000, "Hz");

	// passband reference: everything open, 1 kHz
	double const ref = gain_at(1000, 0x3994, 0x3994, 0x3994);
	printf("     passband reference (1 kHz, open)  %.2f dB (input 1.8 V peak * 3.9 I/V * mix 0.955 / 1.0 FS)\n", ref);

	// 4-pole: (1+s/wc)^4 -> -12 dB at wc (Fc1 open)
	double const f2 = cem_law::fc2_hz(cem_law::word_to_volts(0x2000));
	check("4-pole response at Fc2 (0x2000), expect -12 dB", gain_at(f2, 0x3FFC, 0x2000, 0x3FFC) - ref, -13.0, -11.0, "dB");
	check("4-pole one octave above Fc2, expect ~ -26 dB", gain_at(2 * f2, 0x3FFC, 0x2000, 0x3FFC) - ref, -28.0, -24.0, "dB");

	// 3-pole Chebyshev: -1 dB at the ripple edge (Fc2 open)
	double const f1 = cem_law::fc1_hz(cem_law::word_to_volts(0x2000));
	check("3-pole Chebyshev at Fc1 (0x2000), expect -1 dB", gain_at(f1, 0x2000, 0x3FFC, 0x3FFC) - ref, -2.0, 0.0, "dB");
	check("3-pole two octaves above Fc1, expect < -30 dB", gain_at(4 * f1, 0x2000, 0x3FFC, 0x3FFC) - ref, -80.0, -30.0, "dB");

	// resonance: peak at Fc2 rises as Q word falls (inverted coding)
	double const r0 = gain_at(f2, 0x3FFC, 0x2000, 0x3994), r1 = gain_at(f2, 0x3FFC, 0x2000, 0x1C00);
	check("resonance gain at Fc2, Q 0x1C00 vs 0x3994 (no resonance)", r1 - r0, 6.0, 40.0, "dB");

	// self-oscillation: Q word 0x0700 (0.54 V < 1.0 V), silent input
	{
		cem3387_voice v; v.set_cv(0, 0x3FFC); v.set_cv(1, 0x2000); v.set_cv(2, 0x0700); v.set_cv(3, 0x3FFC); v.set_cv(4, 0x3FFC); v.prepare(FS);
		double l, r, e = 0; int zc = 0; double prev = 0;
		for (int i = 0; i < int(FS * 1.0); ++i) { v.process(0, l, r); if (i > FS * 0.5) { e += l * l; if (prev < 0 && l >= 0) ++zc; prev = l; } }
		check("self-oscillation RMS at Q 0x0700, silent input", std::sqrt(e / (FS * 0.5)), 0.1, 20.0, "V");
		check("self-oscillation frequency vs Fc2", zc / 0.5 / f2, 0.85, 1.15, "x Fc2");
	}

	// VCA law
	check("VCA at gain 0x2000 vs 0x3FFC (datasheet -6.0 dB)", gain_at(1000, 0x3994, 0x3994, 0x3994, 0x2000) - ref, -6.6, -5.6, "dB");
	check("VCA at gain 0x0100 (exponential tail, ~ -72 dB)", gain_at(1000, 0x3994, 0x3994, 0x3994, 0x0100) - ref, -76.0, -68.0, "dB");

	// pan
	{
		double l, r; cem_law::pan_lr(cem_law::word_to_volts(0x2000), l, r);
		check("pan 0x2000 L/R ratio (equal at ~2.5 V)", l / r, 0.95, 1.05, "");
	}

	// gain CV network: rise (diode) vs fall (56 k)
	{
		cem3387_voice v; v.prepare(FS); v.set_cv(3, 0x3FFC);
		double l, r; int i = 0;
		while (v.cv_volts(3) < 0.632 * 4.96) { v.process(0, l, r); ++i; }
		check("gain CV rise 0 -> 4.96 V, time to 63 %", i / FS * 1e3, 0.5, 1.2, "ms");
		for (int k = 0; k < int(FS * 0.1); ++k) v.process(0, l, r);
		double const top = v.cv_volts(3); v.set_cv(3, 0); i = 0;
		while (v.cv_volts(3) > top * 0.368) { v.process(0, l, r); ++i; }
		check("gain CV fall, time to 37 % (56 k * 0.1 uF = 5.6 ms)", i / FS * 1e3, 5.3, 5.9, "ms");
	}
	{
		cem3387_voice v; v.prepare(FS); v.set_cv(1, 0x3FFC);
		double l, r; int i = 0;
		while (v.cv_volts(1) < 0.632 * 4.96) { v.process(0, l, r); ++i; }
		check("Fc2 CV 10 k + 0.1 uF, time to 63 % (1.0 ms)", i / FS * 1e3, 0.95, 1.05, "ms");
	}
	printf("%s\n", fails ? "SOME TESTS FAILED" : "all tests passed");
	return fails ? 1 : 0;
}
