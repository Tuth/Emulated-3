// CEM3387_MODEL_V2 — evidence-refined software model of one EIII voice channel (CEM3387 + external network).
//
// Built from the CEM3387 datasheet (Curtis, Preliminary Jan 1987) section equations and the EIII SK105/SK107 values:
//   IN -> [1st order: -1/(s Rcv1 C1 + 1)] -> [2nd order: -1/(s^2 Rcv1^2 C2a C2b + s Rcv1 C2a + 1)]           (Fc1, 3-pole)
//      -> (sum, minus k(Vq) x VCF OUT) -> [2nd order C3a/C3b] -> [2nd order C4a/C4b] -> VCF OUT          (Fc2, 4-pole + Q)
//      -> 3.3 uF / 10 k -> VCA (current gain g(Vgain)) -> pan (L, R = 1 - L) -> output currents
//   Rcv = 25 k [exp(0.045 (Vr - V)/28 mV) + 1], Vr = (5/12) Vcc.
// Section pole frequency w = 1/(Rcv sqrt(Ca Cb)), damping d = sqrt(Ca/Cb) (datasheet p.6).  With the EIII caps the
// 4-pole sections are OVERDAMPED (d = 2.129, poles 0.70 w0 and 1.43 w0) and the loop oscillates at w0 when k = d3 d4.
// Evidence grades for every element: analysis/CEM3387_MODEL_V2_REPORT.md.
#pragma once
#include "eiii_cem_calibration.h"
#include "eiii_cem_cv_conditioning.h"
#include <cmath>
#include <cstdint>

namespace eiii {

// pure laws (no state), shared by the model, the lab tool and the 4096-code tables
struct cem_laws
{
	static double rcv(cem_params const &p, double r0, double v) { return r0 * (std::exp(p.fc_k * (p.vr() - v) / p.fc_vt) + 1.0); }
	static double fc1_s1_hz(cem_params const &p, double v);   // 1st-order pole
	static double fc1_s2_hz(cem_params const &p, double v);   // 2nd-order pole frequency (= datasheet 3-pole "Fc")
	static double fc2_w0_hz(cem_params const &p, double v);   // 4-pole section pole frequency (= oscillation frequency)
	static double loop_gain_osc(cem_params const &p);         // k at which the 4-pole loop oscillates (d3 d4 for equal sections)
	static double q_k(cem_params const &p, double v);         // resonance loop gain vs Q pin voltage
	static double vca_gain(cem_params const &p, double v);    // current gain vs Gain pin voltage
	static double pan_left(cem_params const &p, double v);    // left share (right = 1 - left) vs Pan pin voltage
};

struct cem_v2_options
{
	double warp_limit_hz = 20000.0;  // section poles above this keep the in-band shape (uniform bilinear scale) instead of
	                                 // being prewarped onto (and beyond) Nyquist; poles below are prewarped exactly
	bool noise = true;               // datasheet noise floor (seeds self-oscillation); deterministic per voice
	bool nonlinear = true;           // soft limits at the datasheet swing figures
};

class cem3387_v2_voice
{
public:
	void prepare(cem_params const &p, double sample_rate, cem_v2_options const &o, uint32_t seed);
	void reset();                                              // state zero, CV pins settled to their targets
	void set_word(unsigned bank, uint16_t word) { m_cv.set_word(bank, word); }
	void set_volts(unsigned bank, double v) { m_cv.set_volts(bank, v); }
	cem_cv_conditioning &cv() { return m_cv; }
	cem_cv_conditioning const &cv() const { return m_cv; }
	// one sample: input volts at CEM IN (relative to its 0.63 V input level); outputs = L/R output-pin currents (A)
	void process_volts(double vin, double &il, double &ir);
	void process(int16_t pcm, double &il, double &ir) { process_volts(double(pcm) * m_in_scale, il, ir); }
	double vcf_out() const { return m_vcf; }                   // last VCF OUT voltage (lab diagnostics)
	double loop_k() const { return m_k; }
	cem_params const &params() const { return m_p; }
private:
	void update_coefficients();
	double g_for(double w_rad) const;                          // bilinear integrator gain for an analog pole
	double softclip(double x, double lim) const;

	cem_params m_p;
	cem_v2_options m_o;
	cem_cv_conditioning m_cv;
	double m_fs = 192000, m_T = 1.0 / 192000, m_in_scale = 1.8 / 32768.0, m_warp_scale = 1.0, m_wlim = 0;
	uint64_t m_key[5] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull};
	// coefficients
	double m_G1 = 0;                                           // S1 TPT one-pole
	double m_s2a1 = 0, m_s2a2 = 0, m_s2a3 = 0;                 // S2 SVF
	double m_s3a1 = 0, m_s3a2 = 0, m_s3a3 = 0, m_s4a1 = 0, m_s4a2 = 0, m_s4a3 = 0;
	double m_d2 = 0.5, m_d3 = 2, m_d4 = 2, m_k = 0, m_gain = 0, m_left = 0.5;
	double m_hpG = 0;
	// state
	double m_s1 = 0, m_s2[2] = {}, m_s3[2] = {}, m_s4[2] = {}, m_hp = 0, m_vcf = 0;
	uint32_t m_rng = 1;
	double m_noise_scale = 0;
	double m_pan_s = 0.25, m_pan_le = 0, m_pan_a = 0.1, m_vca_a = 0.0128, m_kosc = 4.533;
};

class cem3387_v2_bank
{
public:
	void prepare(cem_calibration const &cal, double sample_rate, cem_v2_options const &o);
	void reset();
	// CV DAC write as the firmware addresses it: channel c feeds digital voice 15 - c
	void cv_write(unsigned bank, unsigned channel, uint16_t word) { m_voice[15 - channel].set_word(bank, word); }
	// one internal-rate sample: pcm[v] = held lane value; returns the stereo mix in volts at the LEFT/RIGHT jacks
	void process(int16_t const pcm[16], double &left, double &right);
	cem3387_v2_voice &voice(unsigned v) { return m_voice[v]; }
private:
	struct one_pole { double G = 0, s = 0; double lp(double x) { double const v = (x - s) * G, y = v + s; s = y + v; return y; } };
	cem3387_v2_voice m_voice[16];
	cem_params m_out;                                          // output-stage parameters (global)
	one_pole m_iv[2], m_mixlp[2], m_mixhp[2];
	double m_iv_r = 39e3, m_mix_gain = 0.955;
};

} // namespace eiii
