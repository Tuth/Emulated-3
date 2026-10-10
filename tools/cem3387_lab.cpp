// cem3387_lab — CEM3387 / EIII voice-channel laboratory without the EIII CPU.
// Drives one voice of the V1 or V2 software model (and the exact analog transfer functions of the datasheet sections)
// with explicit CV codes and test signals; writes WAV / TSV / summaries.  Later: the physical-device comparison harness
// (same stimuli, same TSV columns; see analysis/CEM3387_PHYSICAL_MEASUREMENT_PLAN.md).
//
// CV codes are firmware words (bits 13..2 -> DAC code), e.g. 0x3994 = 4.46 V, 0x2000 = 2.48 V.
#include "../cem/eiii_cem3387.h"
#include "../cem/eiii_cem3387_v2.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace eiii;
namespace {
constexpr double PI = 3.14159265358979323846;
using cplx = std::complex<double>;

struct opts
{
	std::string mode, model = "v2", wav, tsv, calib;
	uint16_t fc1 = 0x3994, fc2 = 0x3994, q = 0x3994, gain = 0x3FFC, pan = 0x2000;
	double level = 1.0;            // input amplitude, fraction of PCM full scale (1.0 = +-1.8 V at CEM IN)
	double rate = 192000;          // model rate
	double duration = 1.0, freq = 1000.0, warp = 20000.0;
	int step_bank = 3; uint16_t step_from = 0, step_to = 0x3FFC;
	bool linear = false, quiet_noise = false;
};

double word_v(cem_params const &p, uint16_t w) { return double((w >> 2) & 0xfff) / 4095.0 * p.dac_full_scale_v; }

// ---------------------------------------------------------------- exact analog transfer functions (datasheet sections)
struct analog
{
	cem_params p;
	double v1, v2, vq, vg, vp;
	cplx h1(double f) const { double const w1 = 2 * PI * cem_laws::fc1_s1_hz(p, v1); return 1.0 / (1.0 + cplx(0, 2 * PI * f) / w1); }
	cplx h2(double f) const
	{
		double const w = 2 * PI * cem_laws::fc1_s2_hz(p, v1), d = std::sqrt(p.c2a / p.c2b);
		cplx const s(0, 2 * PI * f / w); return 1.0 / (1.0 + d * s + s * s);
	}
	cplx h34(double f) const
	{
		double const r = cem_laws::rcv(p, p.fc2_r0, v2);
		double const w3 = 1.0 / (r * std::sqrt(p.c3a * p.c3b)), w4 = 1.0 / (r * std::sqrt(p.c4a * p.c4b));
		cplx const s3(0, 2 * PI * f / w3), s4(0, 2 * PI * f / w4);
		return 1.0 / (1.0 + std::sqrt(p.c3a / p.c3b) * s3 + s3 * s3) / (1.0 + std::sqrt(p.c4a / p.c4b) * s4 + s4 * s4);
	}
	cplx fc1(double f) const { return h1(f) * h2(f); }
	cplx fc2(double f) const { cplx const h = h34(f); double const k = cem_laws::q_k(p, vq); return h / (1.0 + k * h); }
	cplx vcf(double f) const { return fc1(f) * fc2(f); }
};

// ---------------------------------------------------------------- model wrappers (V1 / V2), one voice
struct model
{
	bool v2 = true;
	cem_params p;
	cem3387_voice m1;
	cem3387_v2_voice m2;
	double fs;
	void init(opts const &o, cem_params const &pp)
	{
		v2 = o.model != "v1"; p = pp; fs = o.rate;
		cem_v2_options vo; vo.warp_limit_hz = o.warp; vo.nonlinear = !o.linear; vo.noise = !(o.linear || o.quiet_noise);
		if (v2) m2.prepare(p, fs, vo, 12345); else m1.prepare(fs);
		set(CV_FC1, o.fc1); set(CV_FC2, o.fc2); set(CV_Q, o.q); set(CV_GAIN, o.gain); set(CV_PAN, o.pan);
		if (v2) m2.reset(); else m1.reset();
	}
	void set(unsigned b, uint16_t w) { if (v2) m2.set_word(b, w); else m1.set_cv(b, w); }
	// returns jack-side volts for L and R of this channel, I/V included (39 k), plus VCF OUT (V2 only)
	void step(double vin, double &l, double &r, double &vcf)
	{
		if (v2) { double il, ir; m2.process_volts(vin, il, ir); l = il * p.r_iv; r = ir * p.r_iv; vcf = m2.vcf_out(); }
		else { int16_t const pcm = int16_t(std::clamp(vin / 1.8 * 32768.0, -32768.0, 32767.0)); m1.process(pcm, l, r); vcf = 0; }
	}
	double pin(unsigned b) const { return v2 ? m2.cv().pin(b) : m1.cv_volts(b); }
};

void write_wav(std::string const &fn, std::vector<float> const &lr, double fs)
{
	FILE *f = std::fopen(fn.c_str(), "wb");
	if (!f) { std::perror(fn.c_str()); return; }
	uint32_t const n = uint32_t(lr.size() / 2), bytes = n * 8, rate = uint32_t(fs);
	auto p32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); }; auto p16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f); p32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); p32(16); p16(3); p16(2); p32(rate); p32(rate * 8); p16(8); p16(32);
	std::fwrite("data", 1, 4, f); p32(bytes);
	std::fwrite(lr.data(), 4, lr.size(), f);
	std::fclose(f);
}

// steady-state complex gain of the model at frequency f (linear mode recommended): returns VCF OUT / input (V2) or
// channel output (L+R) / (input x gain x 39k/10k) (V1, where VCF OUT is not exposed)
cplx model_response(opts o, cem_params const &p, double f, bool at_vcf = true)
{
	o.linear = true;
	model m; m.init(o, p);
	double const fs = o.rate, amp = 0.1;
	// settle (filters + 4.8 Hz coupling for V1 path), then measure over an integer number of cycles
	int const settle = int(fs * std::max(0.25, 20.0 / f)), ncyc = std::max(8, int(f * 0.05)), n = int(std::round(ncyc * fs / f));
	double l, r, vcf;
	for (int i = 0; i < settle; ++i) m.step(amp * std::sin(2 * PI * f * i / fs), l, r, vcf);
	cplx acc = 0;
	for (int i = 0; i < n; ++i)
	{
		int const t = settle + i;
		m.step(amp * std::sin(2 * PI * f * t / fs), l, r, vcf);
		double const y = (at_vcf && m.v2) ? vcf : (l + r) / (cem_laws::vca_gain(p, word_v(p, o.gain)) * p.r_iv / p.r_vca_in);
		acc += y * std::exp(cplx(0, -2 * PI * f * t / fs));
	}
	// input reference phasor: sin = Im -> reference = amp * e^{-j pi/2} x n/2
	return acc / (double(n) / 2.0) / (amp * cplx(0, -1));
}

double db(double x) { return 20.0 * std::log10(std::max(x, 1e-30)); }

// ---------------------------------------------------------------- modes
int mode_law_table(opts const &o, cem_params const &p)
{
	FILE *f = o.tsv.empty() ? stdout : std::fopen(o.tsv.c_str(), "w");
	std::fprintf(f, "code\tword\tvolts\trcv_ohm\tfc1_s1_hz\tfc1_s2_hz\tfc1_m3db_hz\tfc2_w0_hz\tfc2_m3db_hz\tq_loop_k\tq_k_over_osc\tvca_gain\tvca_db\tpan_left\tpan_right\tpan_left_db\tpan_right_db\n");
	analog a{p, 0, 0, p.q_v_none, 5, 2.5};
	auto m3db = [&](auto hfun, double fguess) {                 // -3 dB point by bisection on log frequency
		double lo = 1, hi = 1e6;
		for (int it = 0; it < 80; ++it) { double const mid = std::sqrt(lo * hi); (std::abs(hfun(mid)) > std::sqrt(0.5) ? lo : hi) = mid; }
		(void)fguess; return std::sqrt(lo * hi);
	};
	for (unsigned code = 0; code < 4096; ++code)
	{
		uint16_t const word = uint16_t(code << 2);
		double const v = word_v(p, word);
		a.v1 = a.v2 = v; a.vq = p.q_v_none;
		double const f1 = m3db([&](double ff) { return a.fc1(ff); }, 0), f2 = m3db([&](double ff) { return a.h34(ff); }, 0);
		double const k = cem_laws::q_k(p, v), g = cem_laws::vca_gain(p, v), L = cem_laws::pan_left(p, v);
		std::fprintf(f, "%u\t%04X\t%.6f\t%.1f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.5f\t%.5f\t%.6e\t%.3f\t%.6f\t%.6f\t%.3f\t%.3f\n", code, word, v,
			cem_laws::rcv(p, p.fc1_r0, v), cem_laws::fc1_s1_hz(p, v), cem_laws::fc1_s2_hz(p, v), f1, cem_laws::fc2_w0_hz(p, v), f2,
			k, k / cem_laws::loop_gain_osc(p), g, db(g), L, 1 - L, db(L), db(1 - L));
	}
	if (f != stdout) std::fclose(f);
	return 0;
}

// frequency sweep: analog ideal vs model, at a set of log-spaced frequencies
int mode_freq(opts const &o, cem_params const &p, char const *which)
{
	analog a{p, word_v(p, o.fc1), word_v(p, o.fc2), word_v(p, o.q), word_v(p, o.gain), word_v(p, o.pan)};
	FILE *f = o.tsv.empty() ? stdout : std::fopen(o.tsv.c_str(), "w");
	std::fprintf(f, "# %s sweep  model=%s rate=%.0f warp=%.0f  fc1=%04X (%.3f V) fc2=%04X (%.3f V) q=%04X (%.3f V, k=%.3f / osc %.3f)\n",
		which, o.model.c_str(), o.rate, o.warp, o.fc1, a.v1, o.fc2, a.v2, o.q, a.vq, cem_laws::q_k(p, a.vq), cem_laws::loop_gain_osc(p));
	std::fprintf(f, "freq_hz\tanalog_db\tanalog_phase_deg\tanalog_group_delay_us\tmodel_db\tmodel_phase_deg\terr_db\n");
	double maxerr = 0, maxerr_f = 0, e6 = 0, e30 = 0, e60 = 0;
	auto H = [&](double ff) { return !std::strcmp(which, "fc1") ? a.fc1(ff) : !std::strcmp(which, "fc2") ? a.fc2(ff) : a.vcf(ff); };
	opts om = o;
	if (!std::strcmp(which, "fc1")) { om.fc2 = 0x3FFC; om.q = 0x3FFC; }        // isolate: other filter fully open, no Q
	if (!std::strcmp(which, "fc2")) om.fc1 = 0x3FFC;
	for (double fr = 20; fr <= std::min(40000.0, 0.45 * o.rate); fr *= std::pow(2.0, 1.0 / 6))
	{
		cplx const ha = H(fr);
		double const df = fr * 1e-4;
		double const gd = -(std::arg(H(fr + df)) - std::arg(H(fr - df))) / (2 * PI * 2 * df) * 1e6;
		cplx hm = model_response(om, p, fr);
		if (!std::strcmp(which, "fc1") && om.model != "v1")
		{   // remove the (fully open) 4-pole contribution measured with the same model
			analog b = a; b.v2 = word_v(p, om.fc2); b.vq = word_v(p, om.q);
			hm /= b.fc2(fr);
		}
		if (!std::strcmp(which, "fc2") && om.model != "v1") { analog b = a; b.v1 = word_v(p, om.fc1); hm /= b.fc1(fr); }
		double const e = db(std::abs(hm)) - db(std::abs(ha));
		if (fr <= 20000 && std::fabs(e) > std::fabs(maxerr)) { maxerr = e; maxerr_f = fr; }
		double const adb = db(std::abs(ha));
		if (fr <= 20000)
		{
			if (adb >= -6 && std::fabs(e) > std::fabs(e6)) e6 = e;
			if (adb >= -30 && std::fabs(e) > std::fabs(e30)) e30 = e;
			if (adb >= -60 && std::fabs(e) > std::fabs(e60)) e60 = e;
		}
		std::fprintf(f, "%.1f\t%.4f\t%.2f\t%.3f\t%.4f\t%.2f\t%+.4f\n", fr, db(std::abs(ha)), std::arg(ha) * 180 / PI, gd,
			db(std::abs(hm)), std::arg(hm) * 180 / PI, e);
	}
	std::fprintf(f, "# max |model - analog| up to 20 kHz: %+.4f dB at %.0f Hz\n", maxerr, maxerr_f);
	if (f != stdout) std::fclose(f);
	std::fprintf(stderr, "%s sweep: max |model - analog| up to 20 kHz = %+.4f dB at %.0f Hz; where analog >= -6 dB: %+.4f, >= -30 dB: %+.4f, >= -60 dB: %+.4f\n",
		which, maxerr, maxerr_f, e6, e30, e60);
	return 0;
}

// time-domain run with a generated input; writes WAV (L, R at the channel output, float volts/10) and/or TSV
int mode_signal(opts const &o, cem_params const &p)
{
	model m; m.init(o, p);
	size_t const n = size_t(o.duration * o.rate);
	std::vector<float> wav; wav.reserve(n * 2);
	FILE *t = o.tsv.empty() ? nullptr : std::fopen(o.tsv.c_str(), "w");
	if (t) std::fprintf(t, "t_s\tin_v\tvcf_out_v\tleft_v\tright_v\tpin_fc1\tpin_fc2\tpin_q\tpin_gain\tpin_pan\n");
	uint32_t rng = 777;
	double const amp = o.level * 1.8, sweep_k = std::log(20000.0 / 20.0) / o.duration;
	double peak = 0, sumsq = 0, vpeak = 0;
	for (size_t i = 0; i < n; ++i)
	{
		double const tt = double(i) / o.rate;
		double x = 0;
		if (o.mode == "impulse") x = (i == 0) ? amp : 0.0;
		else if (o.mode == "step") x = amp;
		else if (o.mode == "sine") x = amp * std::sin(2 * PI * o.freq * tt);
		else if (o.mode == "sweep") x = amp * std::sin(2 * PI * 20.0 * (std::exp(sweep_k * tt) - 1) / sweep_k);
		else if (o.mode == "noise") { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; x = amp * (double(rng) / 2147483648.0 - 1.0); }
		if (o.mode == "cv-step" && i == size_t(0.01 * o.rate)) m.set(unsigned(o.step_bank), o.step_to);
		double l, r, vcf;
		m.step(x, l, r, vcf);
		wav.push_back(float(l / 10)); wav.push_back(float(r / 10));
		peak = std::max(peak, std::max(std::fabs(l), std::fabs(r))); sumsq += l * l + r * r; vpeak = std::max(vpeak, std::fabs(vcf));
		if (t && (i % std::max<size_t>(1, size_t(o.rate / 20000)) == 0))
			std::fprintf(t, "%.7f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\n", tt, x, vcf, l, r,
				m.pin(CV_FC1), m.pin(CV_FC2), m.pin(CV_Q), m.pin(CV_GAIN), m.pin(CV_PAN));
	}
	if (t) std::fclose(t);
	if (!o.wav.empty()) write_wav(o.wav, wav, o.rate);
	std::printf("%s: %zu samples at %.0f Hz; output peak %.4f V, rms %.4f V; VCF OUT peak %.4f V\n", o.mode.c_str(), n, o.rate, peak,
		std::sqrt(sumsq / double(2 * n)), vpeak);
	return 0;
}

// CV network step: times to 10/63/90 % for a code step on one bank
int mode_cv_step(opts const &o, cem_params const &p)
{
	cem_cv_conditioning cv;
	cv.prepare(p, o.rate);
	cv.set_word(unsigned(o.step_bank), o.step_from); cv.reset_to_targets();
	cv.set_word(unsigned(o.step_bank), o.step_to);
	double const v0 = cv.pin(unsigned(o.step_bank)), v1 = cv.target(unsigned(o.step_bank));
	double t10 = -1, t63 = -1, t90 = -1, t99 = -1;
	FILE *t = o.tsv.empty() ? nullptr : std::fopen(o.tsv.c_str(), "w");
	if (t) std::fprintf(t, "t_ms\tpin_v\n");
	for (size_t i = 1; i < size_t(o.rate * 0.05); ++i)
	{
		cv.step();
		double const v = cv.pin(unsigned(o.step_bank)), fr = (v - v0) / (v1 - v0), ms = double(i) / o.rate * 1e3;
		if (t10 < 0 && fr >= 0.1) t10 = ms;
		if (t63 < 0 && fr >= 1 - std::exp(-1.0)) t63 = ms;
		if (t90 < 0 && fr >= 0.9) t90 = ms;
		if (t99 < 0 && fr >= 0.99) t99 = ms;
		if (t && i % 10 == 0) std::fprintf(t, "%.4f\t%.6f\n", ms, v);
	}
	if (t) std::fclose(t);
	std::printf("cv-step bank %d %04X -> %04X (%.3f V -> %.3f V): 10 %% %.3f ms, 63 %% %.3f ms, 90 %% %.3f ms, 99 %% %.3f ms\n",
		o.step_bank, o.step_from, o.step_to, v0, v1, t10, t63, t90, t99);
	return 0;
}

// self-oscillation: no input (noise only), measure frequency and amplitude at VCF OUT after settling
struct osc_result { bool osc; double freq, amp_peak, amp_rms, rise_ms; };
osc_result measure_osc(opts o, cem_params const &p, double seconds)
{
	o.linear = false; o.quiet_noise = false; o.model = "v2";
	model m; m.init(o, p);
	size_t const n = size_t(seconds * o.rate), half = n / 2;
	double prev = 0, peak = 0, sumsq = 0; int crossings = 0; double first_t = -1, last_t = -1, rise = -1;
	for (size_t i = 0; i < n; ++i)
	{
		double l, r, vcf;
		m.step(0.0, l, r, vcf);
		if (rise < 0 && std::fabs(vcf) > 0.1) rise = double(i) / o.rate * 1e3;
		if (i >= half)
		{
			peak = std::max(peak, std::fabs(vcf)); sumsq += vcf * vcf;
			if (prev <= 0 && vcf > 0) { double const tc = (double(i) - vcf / (vcf - prev)) / o.rate; if (first_t < 0) first_t = tc; last_t = tc; ++crossings; }
		}
		prev = vcf;
	}
	osc_result res{};
	res.amp_peak = peak; res.amp_rms = std::sqrt(sumsq / double(n - half)); res.rise_ms = rise;
	res.osc = peak > 0.05 && crossings > 4;
	res.freq = (crossings > 1) ? (crossings - 1) / (last_t - first_t) : 0;
	return res;
}

int mode_self_osc(opts const &o, cem_params const &p)
{
	osc_result const r = measure_osc(o, p, std::max(1.0, o.duration));
	double const v2 = word_v(p, o.fc2);
	std::printf("self-oscillation fc2=%04X (w0 %.1f Hz) q=%04X (%.3f V, k %.3f, k_osc %.3f): %s, f %.2f Hz, VCF OUT peak %.3f V rms %.3f V, "
		"reaches 0.1 V after %.1f ms\n", o.fc2, cem_laws::fc2_w0_hz(p, v2), o.q, word_v(p, o.q), cem_laws::q_k(p, word_v(p, o.q)),
		cem_laws::loop_gain_osc(p), r.osc ? "OSCILLATES" : "no oscillation", r.freq, r.amp_peak, r.amp_rms, r.rise_ms);
	return 0;
}

// Q sweep at one Fc2: peak gain, peak frequency, DC gain, -3 dB bandwidth Q estimate (analog) + self-oscillation (model)
int mode_q_sweep(opts const &o, cem_params const &p)
{
	FILE *f = o.tsv.empty() ? stdout : std::fopen(o.tsv.c_str(), "w");
	std::fprintf(f, "q_word\tq_volts\tloop_k\tk_over_osc\tdc_gain_db\tpeak_gain_db\tpeak_over_dc_db\tpeak_freq_hz\tq_estimate\tmodel_osc\tosc_freq_hz\tosc_vcf_peak_v\n");
	analog a{p, word_v(p, 0x3FFC), word_v(p, o.fc2), 0, 5, 2.5};
	for (int qw = 0x3994; qw >= 0; qw -= 0x0100)
	{
		a.vq = word_v(p, uint16_t(qw));
		double const k = cem_laws::q_k(p, a.vq), kosc = cem_laws::loop_gain_osc(p);
		double const dc = std::abs(a.fc2(1.0));
		double pk = 0, pf = 0;
		double const w0 = cem_laws::fc2_w0_hz(p, a.v2);
		for (double fr = w0 / 8; fr < w0 * 4; fr *= 1.0005) { double const m = std::abs(a.fc2(fr)); if (m > pk) { pk = m; pf = fr; } }
		double qe = 0;
		if (k < kosc && pk / dc > std::sqrt(2.0) * 1.0001)
		{
			double lo = pf, hi = pf;
			while (std::abs(a.fc2(lo)) > pk / std::sqrt(2.0) && lo > 1) lo /= 1.0005;
			while (std::abs(a.fc2(hi)) > pk / std::sqrt(2.0) && hi < 1e7) hi *= 1.0005;
			qe = pf / (hi - lo);
		}
		opts oo = o; oo.q = uint16_t(qw); oo.fc1 = 0x3FFC;
		osc_result const r = (k >= 0.95 * kosc) ? measure_osc(oo, p, 0.5) : osc_result{};
		std::fprintf(f, "%04X\t%.4f\t%.4f\t%.4f\t%.3f\t%.3f\t%.3f\t%.1f\t%.3f\t%s\t%.1f\t%.3f\n", qw, a.vq, k, k / kosc, db(dc),
			k < kosc ? db(pk) : 1e9, k < kosc ? db(pk / dc) : 1e9, pf, qe, r.osc ? "yes" : "no", r.freq, r.amp_peak);
	}
	if (f != stdout) std::fclose(f);
	return 0;
}

// VCA / pan sweeps through the model (settled pins): measured channel gain vs code
int mode_vca_pan(opts const &o, cem_params const &p, bool pan)
{
	FILE *f = o.tsv.empty() ? stdout : std::fopen(o.tsv.c_str(), "w");
	std::fprintf(f, pan ? "pan_word\tpan_volts\tleft_gain\tright_gain\tleft_db\tright_db\tsum_voltage\tsum_power_db\tlaw_left\n"
	                    : "gain_word\tgain_volts\tmodel_gain\tmodel_db\tlaw_gain\tlaw_db\n");
	for (unsigned code = 0; code < 4096; code += (code < 256 ? 4 : 32))
	{
		opts oo = o; oo.linear = true;
		uint16_t const w = uint16_t(code << 2);
		if (pan) oo.pan = w; else oo.gain = w;
		model m; m.init(oo, p);
		// 1 kHz sine, filters open: ratio of channel output to the input-referred full-gain output
		double const fr = 1000, amp = 0.5;
		double al = 0, ar = 0; size_t const settle = size_t(0.2 * o.rate), n = size_t(0.1 * o.rate);
		for (size_t i = 0; i < settle + n; ++i)
		{
			double l, r, vcf;
			m.step(amp * std::sin(2 * PI * fr * double(i) / o.rate), l, r, vcf);
			if (i >= settle) { al += l * l; ar += r * r; }
		}
		double const full = amp * p.r_iv / p.r_vca_in / std::sqrt(2.0);   // rms at full gain, full pan, unity filter
		double const gl = std::sqrt(al / double(n)) / full, gr = std::sqrt(ar / double(n)) / full;
		double const v = word_v(p, w);
		if (pan) std::fprintf(f, "%04X\t%.4f\t%.6f\t%.6f\t%.2f\t%.2f\t%.4f\t%.3f\t%.6f\n", w, v, gl, gr, db(gl), db(gr), gl + gr, 10 * std::log10(gl * gl + gr * gr), cem_laws::pan_left(p, v));
		else std::fprintf(f, "%04X\t%.4f\t%.6e\t%.2f\t%.6e\t%.2f\n", w, v, gl + gr, db(gl + gr), cem_laws::vca_gain(p, v), db(cem_laws::vca_gain(p, v)));
	}
	if (f != stdout) std::fclose(f);
	return 0;
}

int mode_bench(opts const &o, cem_params const &p)
{
	for (std::string mdl : {"v1", "v2"})
	{
		opts oo = o; oo.model = mdl;
		model m; m.init(oo, p);
		size_t const n = size_t(2.0 * oo.rate);
		double l, r, vcf, acc = 0;
		auto t0 = std::chrono::steady_clock::now();
		for (size_t i = 0; i < n; ++i)
		{
			if (i % 192 == 0) { m.set(CV_FC2, uint16_t(0x2000 + (i / 192 % 512) * 8)); m.set(CV_GAIN, uint16_t(0x3000 - (i / 192 % 256) * 16)); }
			m.step(0.5 * std::sin(0.01 * double(i)), l, r, vcf); acc += l;
		}
		double const ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / double(n);
		std::printf("bench %s at %.0f Hz: %.1f ns per voice-sample (moving Fc2 + Gain every 1 ms) -> 16 voices %.2fx realtime  [%g]\n",
			mdl.c_str(), oo.rate, ns, 1e9 / (ns * 16 * oo.rate), acc * 1e-30);
	}
	return 0;
}

// radix-2 in-place FFT
void fft(std::vector<cplx> &a)
{
	size_t const n = a.size();
	for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
	for (size_t len = 2; len <= n; len <<= 1)
	{
		cplx const wl = std::polar(1.0, -2 * PI / double(len));
		for (size_t i = 0; i < n; i += len) { cplx w = 1; for (size_t k = 0; k < len / 2; ++k) { cplx const u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; } }
	}
}

// characterization row: impulse responses (linear) for the filter figures, a 1 kHz sine (nonlinear) for gain/THD,
// a zero-input run with noise for self-oscillation, plus stability flags
struct char_row
{
	uint16_t fc1, fc2, q, gain, pan;
	double f1_m3, f2_m3, peak_db, peak_hz, out_gain_db, l_gain, r_gain, thd_pct, osc_hz, osc_amp; bool osc, stable;
	double vcf_m3_model, vcf_m3_analog, peak_db_analog;
};
char_row characterize(opts o, cem_params const &p, uint16_t fc1, uint16_t fc2, uint16_t q, uint16_t gain, uint16_t pan)
{
	char_row r{fc1, fc2, q, gain, pan, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, true, 0, 0, 0};
	o.model = "v2"; o.fc1 = fc1; o.fc2 = fc2; o.q = q; o.gain = gain; o.pan = pan;
	size_t const N = size_t(1) << 18;                         // 1.37 s at 192 kHz
	auto impulse_resp = [&](uint16_t f1, uint16_t f2, uint16_t qq) {
		opts oo = o; oo.linear = true; oo.fc1 = f1; oo.fc2 = f2; oo.q = qq;
		model m; m.init(oo, p);
		std::vector<cplx> h(N);
		double l, rr, vcf;
		for (size_t i = 0; i < N; ++i) { m.step(i == 0 ? 1.0 : 0.0, l, rr, vcf); h[i] = vcf; if (!std::isfinite(vcf)) r.stable = false; }
		fft(h);
		return h;
	};
	auto m3db = [&](std::vector<cplx> const &h) {            // first frequency where |H| < -3 dB re DC
		double const dc = std::abs(h[1]);
		for (size_t k = 1; k < N / 2; ++k) if (std::abs(h[k]) < dc * std::sqrt(0.5)) return double(k) * o.rate / double(N);
		return o.rate / 2;
	};
	if (cem_laws::q_k(p, word_v(p, q)) < cem_laws::loop_gain_osc(p))
	{
		analog a{p, word_v(p, fc1), word_v(p, fc2), word_v(p, 0x3FFC), 5, 2.5};
		auto bis = [&](auto hf) { double lo = 1, hi = 1e6; for (int it = 0; it < 80; ++it) { double const mid = std::sqrt(lo * hi); (std::abs(hf(mid)) > std::sqrt(0.5) ? lo : hi) = mid; } return std::sqrt(lo * hi); };
		r.f1_m3 = bis([&](double ff) { return a.fc1(ff); });                         // laws: Fc1 3-pole alone
		r.f2_m3 = bis([&](double ff) { return a.h34(ff); });                         // laws: Fc2 4-pole alone, no Q
		r.vcf_m3_analog = bis([&](double ff) { return a.vcf(ff); });                 // laws: both, no Q
		r.vcf_m3_model = m3db(impulse_resp(fc1, fc2, 0x3FFC));                       // model: both, no Q
		auto const hq = impulse_resp(fc1, fc2, q);
		double pk = 0; size_t kp = 1;
		for (size_t k = 1; k < size_t(20000.0 * double(N) / o.rate); ++k) if (std::abs(hq[k]) > pk) { pk = std::abs(hq[k]); kp = k; }
		r.peak_db = 20 * std::log10(pk / std::abs(hq[1])); r.peak_hz = double(kp) * o.rate / double(N);
		a.vq = word_v(p, q);
		double pka = 0; for (double ff = 10; ff < 20000; ff *= 1.001) pka = std::max(pka, std::abs(a.vcf(ff)));
		r.peak_db_analog = 20 * std::log10(pka / std::abs(a.vcf(1.0)));
	}
	{   // 1 kHz, input full scale (+-1.8 V), nonlinear, no noise
		opts oo = o; oo.quiet_noise = true;
		model m; m.init(oo, p);
		size_t const settle = size_t(0.3 * o.rate), n = size_t(o.rate * 0.5);
		cplx al = 0, ar = 0; std::vector<double> y; y.reserve(n);
		double l, rr, vcf, pk = 0;
		for (size_t i = 0; i < settle + n; ++i)
		{
			double const x = 1.8 * std::sin(2 * PI * 1000 * double(i) / o.rate);
			m.step(x, l, rr, vcf);
			if (!std::isfinite(l) || !std::isfinite(rr)) r.stable = false;
			pk = std::max(pk, std::fabs(vcf));
			if (i >= settle) { cplx const e = std::exp(cplx(0, -2 * PI * 1000 * double(i) / o.rate)); al += l * e; ar += rr * e; y.push_back(l + rr); }
		}
		double const full = 1.8 * p.r_iv / p.r_vca_in * double(n) / 2.0;   // phasor magnitude at unity filter/gain/pan
		r.l_gain = std::abs(al) / full; r.r_gain = std::abs(ar) / full;
		r.out_gain_db = 20 * std::log10(std::max(1e-12, r.l_gain + r.r_gain));
		auto bin = [&](double f) { cplx a = 0; for (size_t i = 0; i < y.size(); ++i) a += y[i] * std::exp(cplx(0, -2 * PI * f * double(i + settle) / o.rate)); return std::abs(a); };
		double const h1 = bin(1000); double hs = 0; for (int k = 2; k <= 7; ++k) { double const hk = bin(1000.0 * k); hs += hk * hk; }
		r.thd_pct = h1 > 0 ? std::sqrt(hs) / h1 * 100 : 0;
		if (pk > p.out_limit_v + 1e-9) r.stable = false;
	}
	osc_result const os = measure_osc(o, p, 0.5);
	r.osc = os.osc; r.osc_hz = os.freq; r.osc_amp = os.amp_peak;
	return r;
}

int mode_matrix(opts const &o, cem_params const &p)
{
	std::vector<std::array<uint16_t, 5>> rows;
	// 1. filter grid at full gain, centre pan
	for (uint16_t f1 : {uint16_t(0x1000), uint16_t(0x3000), uint16_t(0x3994)})
		for (uint16_t f2 : {uint16_t(0x1000), uint16_t(0x2400), uint16_t(0x3000), uint16_t(0x3994)})
			for (uint16_t q : {uint16_t(0x3994), uint16_t(0x2594), uint16_t(0x1194), uint16_t(0x0994), uint16_t(0x0000)})
				rows.push_back({f1, f2, q, 0x3FFC, 0x2000});
	// 2. VCA and pan rows, filters open
	for (uint16_t g : {uint16_t(0x3FFC), uint16_t(0x2000), uint16_t(0x0800), uint16_t(0x0294), uint16_t(0x0180), uint16_t(0x0100)}) rows.push_back({0x3994, 0x3994, 0x3994, g, 0x2000});
	for (uint16_t pn : {uint16_t(0x0000), uint16_t(0x0400), uint16_t(0x1000), uint16_t(0x3000), uint16_t(0x3C00), uint16_t(0x3FFC)}) rows.push_back({0x3994, 0x3994, 0x3994, 0x3FFC, pn});
	// 3. corners: hot resonance at low gain / hard pan; lowest and highest cutoffs at maximum Q
	rows.push_back({0x3994, 0x0400, 0x0000, 0x0294, 0x0000});
	rows.push_back({0x3994, 0x3FFC, 0x0000, 0x3FFC, 0x3FFC});
	rows.push_back({0x0000, 0x0000, 0x0000, 0x3FFC, 0x2000});
	rows.push_back({0x3FFC, 0x3FFC, 0x3FFC, 0x3FFC, 0x2000});
	FILE *f = o.tsv.empty() ? stdout : std::fopen(o.tsv.c_str(), "w");
	std::fprintf(f, "# CEM3387_MODEL_V2 characterization, nominal parameters, model rate %.0f Hz. input: 1 kHz sine +-1.8 V (PCM full scale) for\n"
		"# gains/THD; unit impulse (linear) for the filter figures; zero input + datasheet noise for self-oscillation.\n", o.rate);
	std::fprintf(f, "input\tfc1_word\tfc1_v\tfc2_word\tfc2_v\tq_word\tq_v\tgain_word\tgain_v\tpan_word\tpan_v\tfc1_m3db_law_hz\tfc2_m3db_law_hz\tpeak_resonance_db\tpeak_hz\toutput_gain_db\tl_gain\tr_gain\tthd_pct_1k_fs\tself_osc\tosc_hz\tosc_vcf_peak_v\tstable\tvcf_m3db_model_hz\tvcf_m3db_analog_hz\tpeak_resonance_analog_db\n");
	std::fprintf(f, "# fc1/fc2_m3db_law_hz: exact section laws (each filter alone, no Q); vcf_m3db_*: both filters as set, no Q;\n"
		"# peak_resonance_db: model, re DC, up to 20 kHz; osc_hz above the 20 kHz warp limit is the warped (ultrasonic) model frequency.\n");
	for (auto const &c : rows)
	{
		char_row const r = characterize(o, p, c[0], c[1], c[2], c[3], c[4]);
		bool const osc_region = cem_laws::q_k(p, word_v(p, c[2])) >= cem_laws::loop_gain_osc(p);
		std::fprintf(f, "sine1k_fs+impulse\t%04X\t%.3f\t%04X\t%.3f\t%04X\t%.3f\t%04X\t%.3f\t%04X\t%.3f\t%s\t%s\t%s\t%s\t%.2f\t%.5f\t%.5f\t%s\t%s\t%s\t%.3f\t%s\t%s\t%s\t%s\n",
			c[0], word_v(p, c[0]), c[1], word_v(p, c[1]), c[2], word_v(p, c[2]), c[3], word_v(p, c[3]), c[4], word_v(p, c[4]),
			osc_region ? "-" : std::to_string(int(r.f1_m3)).c_str(), osc_region ? "-" : std::to_string(int(r.f2_m3)).c_str(),
			osc_region ? "-" : std::to_string(r.peak_db).substr(0, 6).c_str(), osc_region ? "-" : std::to_string(int(r.peak_hz)).c_str(),
			r.out_gain_db, r.l_gain, r.r_gain, r.osc ? "-" : std::to_string(r.thd_pct).substr(0, 7).c_str(), r.osc ? "yes" : "no",
			r.osc ? std::to_string(int(r.osc_hz)).c_str() : "-", r.osc_amp, r.stable ? "ok" : "UNSTABLE",
			osc_region ? "-" : std::to_string(int(r.vcf_m3_model)).c_str(), osc_region ? "-" : std::to_string(int(r.vcf_m3_analog)).c_str(),
			osc_region ? "-" : std::to_string(r.peak_db_analog).substr(0, 6).c_str());
		std::fflush(f);
	}
	if (f != stdout) std::fclose(f);
	return 0;
}

void usage()
{
	std::puts("cem3387_lab MODE [options]\n"
		"modes: --impulse --step --sine --sweep --noise --self-oscillation --cv-step --pan-sweep --vca-sweep --fc1-sweep --fc2-sweep\n"
		"       --vcf-sweep --q-sweep --law-table --matrix --bench --calibration-template\n"
		"options: --model v1|v2  --fc1 W --fc2 W --q W --gain W --pan W (firmware words, e.g. 0x3994)\n"
		"         --level F (input, fraction of PCM full scale = +-1.8 V)  --freq HZ  --rate HZ (model rate, default 192000)\n"
		"         --duration S  --warp HZ (V2 warp limit)  --linear (no soft limits, no noise)  --no-noise\n"
		"         --bank N --from W --to W (cv-step)  --wav FILE  --tsv FILE  --calib FILE");
}
} // namespace

int main(int argc, char **argv)
{
	opts o;
	for (int i = 1; i < argc; ++i)
	{
		std::string const k = argv[i];
		auto val = [&]() -> char const * { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
		auto word = [&]() { return uint16_t(std::strtoul(val(), nullptr, 0)); };
		if (k.size() > 2 && k.rfind("--", 0) == 0 && o.mode.empty() &&
		    (k == "--impulse" || k == "--step" || k == "--sine" || k == "--sweep" || k == "--noise" || k == "--self-oscillation" ||
		     k == "--cv-step" || k == "--pan-sweep" || k == "--vca-sweep" || k == "--fc1-sweep" || k == "--fc2-sweep" ||
		     k == "--vcf-sweep" || k == "--q-sweep" || k == "--law-table" || k == "--bench" || k == "--calibration-template" || k == "--matrix"))
			o.mode = k.substr(2);
		else if (k == "--model") o.model = val();
		else if (k == "--fc1") o.fc1 = word(); else if (k == "--fc2") o.fc2 = word(); else if (k == "--q") o.q = word();
		else if (k == "--gain") o.gain = word(); else if (k == "--pan") o.pan = word();
		else if (k == "--level") o.level = std::atof(val()); else if (k == "--freq") o.freq = std::atof(val());
		else if (k == "--rate") o.rate = std::atof(val()); else if (k == "--duration") o.duration = std::atof(val());
		else if (k == "--warp") o.warp = std::atof(val());
		else if (k == "--linear") o.linear = true; else if (k == "--no-noise") o.quiet_noise = true;
		else if (k == "--bank") o.step_bank = std::atoi(val()); else if (k == "--from") o.step_from = word(); else if (k == "--to") o.step_to = word();
		else if (k == "--wav") o.wav = val(); else if (k == "--tsv") o.tsv = val(); else if (k == "--calib") o.calib = val();
		else { usage(); return 2; }
	}
	if (o.mode.empty()) { usage(); return 2; }
	cem_calibration cal;
	if (!o.calib.empty()) { std::string const e = cal.load(o.calib); if (!e.empty()) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; } }
	cem_params const &p = cal.voice[0];
	if (o.mode == "calibration-template") { std::fputs(cem_calibration::write_template().c_str(), stdout); return 0; }
	if (o.mode == "law-table") return mode_law_table(o, p);
	if (o.mode == "fc1-sweep") return mode_freq(o, p, "fc1");
	if (o.mode == "fc2-sweep") return mode_freq(o, p, "fc2");
	if (o.mode == "vcf-sweep") return mode_freq(o, p, "vcf");
	if (o.mode == "cv-step") return mode_cv_step(o, p);
	if (o.mode == "self-oscillation") return mode_self_osc(o, p);
	if (o.mode == "q-sweep") return mode_q_sweep(o, p);
	if (o.mode == "vca-sweep") return mode_vca_pan(o, p, false);
	if (o.mode == "pan-sweep") return mode_vca_pan(o, p, true);
	if (o.mode == "bench") return mode_bench(o, p);
	if (o.mode == "matrix") return mode_matrix(o, p);
	return mode_signal(o, p);
}
