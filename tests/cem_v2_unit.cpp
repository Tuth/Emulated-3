// CEM3387_MODEL_V2 unit tests: datasheet anchor points, external-network time constants, numerical accuracy against the
// exact analog section transfer functions, and pathological stability (minutes of self-oscillation, CV extremes,
// abrupt cutoff changes, long silence, denormal decay).  Every check prints PASS/FAIL with the measured value.
#include "../cem/eiii_cem3387_v2.h"
#include <string>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace eiii;
namespace {
int fails = 0;
constexpr double PI = 3.14159265358979323846;
void check(bool ok, char const *what, double value, char const *range)
{
	std::printf("%s %-64s %12.5g  %s\n", ok ? "PASS" : "FAIL", what, value, range);
	if (!ok) ++fails;
}
double word_v(cem_params const &p, uint16_t w) { return double((w >> 2) & 0xfff) / 4095.0 * p.dac_full_scale_v; }
bool is_fin(double x) { return std::isfinite(x); }

cem3387_v2_voice make(cem_params const &p, double fs, bool linear, uint16_t fc1, uint16_t fc2, uint16_t q, uint16_t gain, uint16_t pan)
{
	cem_v2_options o; o.nonlinear = !linear; o.noise = !linear;
	cem3387_v2_voice v; v.prepare(p, fs, o, 42);
	v.set_word(CV_FC1, fc1); v.set_word(CV_FC2, fc2); v.set_word(CV_Q, q); v.set_word(CV_GAIN, gain); v.set_word(CV_PAN, pan);
	v.reset();
	return v;
}
} // namespace

int main()
{
	cem_params const p;
	double const fs = 192000;
	std::puts("-- datasheet anchor points");
	check(std::fabs(cem_laws::rcv(p, 25e3, p.vr()) - 50e3) < 1e-6, "Rcv at Vcf = Vr (datasheet: 50 k)", cem_laws::rcv(p, 25e3, p.vr()), "[50 k]");
	{
		double const oct = std::log2(cem_laws::fc2_w0_hz(p, 5.0) / cem_laws::fc2_w0_hz(p, 0.0));
		check(oct > 10.3 && oct < 10.7, "frequency range 0..5 V in octaves (datasheet 10.5)", oct, "[10.3 .. 10.7]");
		double const s = std::log2(cem_laws::fc2_w0_hz(p, 2.0) / cem_laws::fc2_w0_hz(p, 1.0));
		check(s > 2.2 && s < 2.4, "control scale below Vr - 1.3 V, oct/V (datasheet 2.3)", s, "[2.2 .. 2.4]");
	}
	check(std::fabs(cem_laws::loop_gain_osc(p) - 68.0 / 15.0) < 1e-6, "4-pole oscillation loop gain = C3a/C3b (overdamped sections)", cem_laws::loop_gain_osc(p), "[4.5333]");
	check(std::fabs(cem_laws::q_k(p, 4.5)) < 1e-12, "resonance loop gain at +4.5 V (datasheet: no feedback)", cem_laws::q_k(p, 4.5), "[0]");
	check(std::fabs(cem_laws::q_k(p, 1.0) / cem_laws::loop_gain_osc(p) - 1) < 1e-12, "k / k_osc at +1.0 V (datasheet: sustained oscillation)", cem_laws::q_k(p, 1.0) / cem_laws::loop_gain_osc(p), "[1]");
	auto dbv = [](double x) { return 20 * std::log10(x); };
	check(std::fabs(dbv(cem_laws::vca_gain(p, 5.0))) < 1e-9, "VCA gain at +5.0 V (datasheet: max gain)", dbv(cem_laws::vca_gain(p, 5.0)), "[0 dB]");
	check(std::fabs(dbv(cem_laws::vca_gain(p, 0.1)) + 96) < 1e-6, "VCA gain at +0.1 V (datasheet: -96 dB)", dbv(cem_laws::vca_gain(p, 0.1)), "[-96 dB]");
	check(std::fabs(dbv(cem_laws::vca_gain(p, 0.2)) + 27.96) < 0.01, "VCA gain at +0.2 V (datasheet: 'about -30 dB')", dbv(cem_laws::vca_gain(p, 0.2)), "[-27.96 dB]");
	check(std::fabs(cem_laws::pan_left(p, 2.5) - 0.5) < 1e-12, "pan left share at +2.5 V (datasheet: equal, half gain)", cem_laws::pan_left(p, 2.5), "[0.5]");
	check(std::fabs(dbv(cem_laws::pan_left(p, 0.1)) + 80) < 0.01, "pan left at +0.1 V (datasheet: -80 dB)", dbv(cem_laws::pan_left(p, 0.1)), "[-80 dB]");
	{
		double maxdev = 0;
		// probe [v, v + 5 mV] stays inside [0.5, 4.5] V
		for (double v = 0.5; v <= 4.49; v += 0.01) { double const L = cem_laws::pan_left(p, v); double const lin = 0.5 + (L - 0.5); maxdev = std::max(maxdev, std::fabs((cem_laws::pan_left(p, v + 0.005) - L) / 0.005 - (cem_laws::pan_left(p, 2.5 + 0.005) - 0.5) / 0.005)); (void)lin; }
		check(maxdev < 1e-6, "pan law linear between +0.5 V and +4.5 V (slope deviation)", maxdev, "[< 1e-6 /V]");
	}

	std::puts("-- calibration file");
	{
		char const *fn = "tests/.cem_cal_test.ini";
		FILE *f = std::fopen(fn, "w");
		std::fputs("[global]\ndevice.serial = test\nfc.fc2_r0 = 24000\n[voice 3]\nfc.fc2_r0 = 20000\nq.table = 4.5:0, 1.5:4.533, 0.0:6\n", f);
		std::fclose(f);
		cem_calibration cal;
		std::string const err = cal.load(fn);
		std::remove(fn);
		check(err.empty() && cal.voice[0].fc2_r0 == 24000 && cal.voice[3].fc2_r0 == 20000 && cal.voice[3].serial == "test",
			"calibration: global + [voice 3] override applied", cal.voice[3].fc2_r0, "[20000]");
		double const k = cem_laws::q_k(cal.voice[3], 1.24);
		check(std::fabs(k - (4.533 + (6 - 4.533) * (1.5 - 1.24) / 1.5)) < 1e-9, "calibration: q.table (any order) interpolated", k, "[4.787]");
		cem_calibration bad;
		f = std::fopen(fn, "w"); std::fputs("bogus.key = 1\n", f); std::fclose(f);
		std::string const e2 = bad.load(fn);
		std::remove(fn);
		check(!e2.empty(), "calibration: unknown key rejected", double(e2.size()), "[error text]");
	}
	std::puts("-- external CV networks (SK105)");
	{
		cem_cv_conditioning cv; cv.prepare(p, fs);
		auto t63 = [&](unsigned bank, uint16_t from, uint16_t to) {
			cv.set_word(bank, from); cv.reset_to_targets(); cv.set_word(bank, to);
			double const v0 = cv.pin(bank), v1 = cv.target(bank);
			for (int i = 1; i < int(fs * 0.1); ++i) { cv.step(); if ((cv.pin(bank) - v0) / (v1 - v0) >= 1 - std::exp(-1.0)) return i / fs * 1e3; }
			return -1.0;
		};
		double const a = t63(CV_FC2, 0, 0x3FFC), b = t63(CV_Q, 0x3994, 0), c = t63(CV_PAN, 0x3FFC, 0);
		check(std::fabs(a - 1.0) < 0.01, "Fc2 network 63 % time (10 k x 0.1 uF)", a, "[1.00 ms]");
		check(std::fabs(b - 1.0) < 0.01, "Q network 63 % time", b, "[1.00 ms]");
		check(std::fabs(c - 1.0) < 0.01, "Pan network 63 % time", c, "[1.00 ms]");
		double const up = t63(CV_GAIN, 0, 0x3FFC), dn = t63(CV_GAIN, 0x3FFC, 0), small = t63(CV_GAIN, 0x2000, 0x2004);
		check(up > 0.6 && up < 0.8, "Gain full rise 63 % (diode + 6.8 k || 56 k)", up, "[0.6 .. 0.8 ms]");
		check(std::fabs(dn - 5.6) < 0.05, "Gain fall 63 % (56 k x 0.1 uF)", dn, "[5.60 ms]");
		check(std::fabs(small - 5.6) < 0.1, "Gain 1-LSB rise 63 % (diode off)", small, "[5.6 ms]");
	}

	std::puts("-- numerical accuracy vs exact analog sections (192 kHz, linear)");
	{
		auto resp = [&](uint16_t fc1, uint16_t fc2, double f) {
			cem3387_v2_voice v = make(p, fs, true, fc1, fc2, 0x3994, 0x3FFC, 0x2000);
			int const settle = int(fs * 0.2), n = int(std::round(std::max(8.0, f * 0.05) * fs / f));
			std::complex<double> acc = 0; double il, ir;
			for (int i = 0; i < settle + n; ++i)
			{
				v.process_volts(0.1 * std::sin(2 * PI * f * i / fs), il, ir);
				if (i >= settle) acc += v.vcf_out() * std::exp(std::complex<double>(0, -2 * PI * f * i / fs));
			}
			return std::abs(acc) / (n / 2.0) / 0.1;
		};
		auto analog = [&](uint16_t fc1, uint16_t fc2, double f) {
			double const v1 = word_v(p, fc1), v2 = word_v(p, fc2), r1 = cem_laws::rcv(p, p.fc1_r0, v1), r2 = cem_laws::rcv(p, p.fc2_r0, v2);
			std::complex<double> const s(0, 2 * PI * f);
			auto sec = [&](double r, double ca, double cb) { return 1.0 / (r * r * ca * cb * s * s + r * ca * s + 1.0); };
			std::complex<double> const h34 = sec(r2, p.c3a, p.c3b) * sec(r2, p.c4a, p.c4b);
			double const k = cem_laws::q_k(p, word_v(p, 0x3994));
			return std::abs(1.0 / (r1 * p.c1 * s + 1.0) * sec(r1, p.c2a, p.c2b) * h34 / (1.0 + k * h34));
		};
		double worst = 0;
		for (uint16_t fc : {uint16_t(0x3994), uint16_t(0x3000), uint16_t(0x2000), uint16_t(0x1000)})
			for (double f : {100.0, 1000.0, 5000.0, 10000.0, 16000.0})
			{
				double const a = analog(0x3994, fc, f);
				if (20 * std::log10(a) < -6) continue;             // passband / near-cutoff region only
				worst = std::max(worst, std::fabs(20 * std::log10(resp(0x3994, fc, f) / a)));
			}
		check(worst < 0.25, "max |model - analog| in the Fc2 pass band (dB)", worst, "[< 0.25 dB]");
	}

	std::puts("-- linearity at EIII levels");
	{
		cem3387_v2_voice v = make(p, fs, false, 0x3994, 0x3994, 0x3994, 0x3FFC, 0x2000);
		// 5 V p-p sine at CEM IN (datasheet THD figure) through the nonlinear model; THD from a DFT
		int const n = int(fs), settle = int(fs * 0.2);
		std::vector<double> y;
		double il, ir;
		for (int i = 0; i < settle + n; ++i) { v.process_volts(2.5 * std::sin(2 * PI * 1000 * i / fs), il, ir); if (i >= settle) y.push_back(v.vcf_out()); }
		auto bin = [&](double f) { std::complex<double> a = 0; for (int i = 0; i < n; ++i) a += y[size_t(i)] * std::exp(std::complex<double>(0, -2 * PI * f * i / fs)); return std::abs(a); };
		double const h1 = bin(1000), h2 = bin(2000), h3 = bin(3000), h5 = bin(5000);
		double const thd = std::sqrt(h2 * h2 + h3 * h3 + h5 * h5) / h1 * 100;
		check(thd < 0.1, "THD at 5 V p-p input (datasheet < 0.1 %)", thd, "[< 0.1 %]");
	}

	std::puts("-- pathological stability");
	{
		// minutes of self-oscillation at maximum firmware Q (0 V): bounded, stable amplitude, stable frequency
		cem3387_v2_voice v = make(p, fs, false, 0x3994, 0x2000, 0x0000, 0x3FFC, 0x2000);
		double il, ir, pk_early = 0, pk_late = 0; bool ok = true;
		size_t const n = size_t(fs * 120);
		for (size_t i = 0; i < n; ++i)
		{
			v.process_volts(0.0, il, ir);
			double const y = v.vcf_out();
			if (!is_fin(y) || !is_fin(il)) { ok = false; break; }
			if (i > size_t(fs) && i < size_t(2 * fs)) pk_early = std::max(pk_early, std::fabs(y));
			if (i > n - size_t(fs)) pk_late = std::max(pk_late, std::fabs(y));
		}
		check(ok && pk_late < p.out_limit_v + 1e-9 && std::fabs(pk_late / pk_early - 1) < 0.01, "120 s self-oscillation: late/early peak ratio", pk_late / pk_early, "[1 +- 1 %, finite, <= 3.5 V]");
	}
	{
		// abrupt cutoff/Q/gain/pan extremes every 1 ms with a full-scale input, 20 s
		cem3387_v2_voice v = make(p, fs, false, 0x3994, 0x3994, 0x3994, 0x3FFC, 0x2000);
		uint32_t rng = 9; bool ok = true; double pk = 0, il, ir;
		auto rnd = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
		for (size_t i = 0; i < size_t(fs * 20); ++i)
		{
			if (i % 192 == 0) for (unsigned b = 0; b < 5; ++b) v.set_word(b, (rnd() & 1) ? 0x3FFC : 0x0000);
			v.process_volts(1.8 * ((rnd() & 0xFFFF) / 32768.0 - 1.0), il, ir);
			if (!is_fin(v.vcf_out()) || !is_fin(il) || !is_fin(ir)) { ok = false; break; }
			pk = std::max(pk, std::fabs(v.vcf_out()));
		}
		check(ok && pk <= p.out_limit_v, "20 s random CV extremes every 1 ms, full-scale noise: VCF peak", pk, "[finite, <= 3.5 V]");
	}
	{
		// per-sample alternation of Fc2 extremes at high Q (worst case for coefficient modulation)
		cem3387_v2_voice v = make(p, fs, false, 0x3994, 0x0000, 0x0400, 0x3FFC, 0x2000);
		bool ok = true; double pk = 0, il, ir;
		for (size_t i = 0; i < size_t(fs * 5); ++i)
		{
			v.set_word(CV_FC2, (i & 1) ? 0x3FFC : 0x0000);
			v.process_volts(std::sin(0.05 * double(i)), il, ir);
			if (!is_fin(v.vcf_out())) { ok = false; break; }
			pk = std::max(pk, std::fabs(v.vcf_out()));
		}
		check(ok && pk <= p.out_limit_v, "Fc2 alternating 0 / 4.96 V target every sample, Q 0.31 V: peak", pk, "[finite, <= 3.5 V]");
	}
	{
		// long silence after a loud burst, noise off: decays to a stable finite state; no slowdown (denormals)
		cem3387_v2_voice v = make(p, fs, true, 0x3994, 0x2000, 0x2000, 0x3FFC, 0x2000);
		double il, ir;
		for (int i = 0; i < int(fs); ++i) v.process_volts(1.8 * std::sin(0.03 * i), il, ir);
		auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < int(fs); ++i) v.process_volts(0.0, il, ir);
		double const early = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		for (int i = 0; i < int(fs * 120); ++i) v.process_volts(0.0, il, ir);  // 2 minutes of silence
		auto t1 = std::chrono::steady_clock::now();
		for (int i = 0; i < int(fs); ++i) v.process_volts(0.0, il, ir);
		double const late = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
		check(is_fin(v.vcf_out()) && std::fabs(v.vcf_out()) < 1e-6, "2 min silence after full-scale burst: |VCF OUT|", std::fabs(v.vcf_out()), "[< 1 uV]");
		check(late < early * 3 + 0.05, "silence cost late/early (denormal slowdown)", late / early, "[< 3x]");
	}
	std::printf("%s\n", fails ? "SOME TESTS FAILED" : "all CEM3387_MODEL_V2 tests passed");
	return fails ? 1 : 0;
}
