#include "eiii_cem3387.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace eiii {

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double VT = 0.028;
constexpr double C2A = 10e-12, CB = 15e-12;
constexpr double IN_PEAK_V = 1.8;          // PCM53 + deglitcher full scale at CEM IN (STRONG/TENTATIVE)
constexpr double VCF_LIMIT_V = 3.5;        // datasheet max VCF output
constexpr double RIN_VCA = 10e3, RF_IV = 39e3;
constexpr double MIX_GAIN = 10e3 / (10e3 + 470.0);  // SK107: 470 R + 10 k into 10 k summer
// 3rd-order 1 dB Chebyshev prototype (ripple-band edge = 1 rad/s)
constexpr double CHEB_REAL = 0.494170, CHEB_W0 = 0.997098, CHEB_Q = 2.017730;
inline double tpt_g(double f, double fs) { return std::tan(PI * std::min(f, 0.45 * fs) / fs); }
inline uint64_t bits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
}

double cem_law::rcv(double v) { return 25e3 * (std::exp(0.045 * (VR - v) / VT) + 1.0); }
double cem_law::fc1_hz(double v) { return 1.0 / (4.0 * PI * rcv(v) * C2A); }
double cem_law::fc2_hz(double v) { return 1.0 / (4.0 * PI * rcv(v) * CB); }

double cem_law::gain_db(double v)
{
	if (v <= 0.0) return -100.0;
	double const knee_v = 0.2, knee_db = 20.0 * std::log10(knee_v / VR);
	if (v >= knee_v) return 20.0 * std::log10(std::min(v, VR) / VR);
	return -100.0 + (knee_db + 100.0) * (v / knee_v);
}

void cem_law::pan_lr(double v, double &l, double &r)
{
	double const x = std::clamp((v - 0.5) / 4.0, 0.0, 1.0);
	l = x; r = 1.0 - x;
}

double cem_law::resonance_k(double v) { return 4.0 * std::clamp((4.5 - v) / 3.5, 0.0, 1.05); }

void cem3387_voice::prepare(double sample_rate)
{
	m_fs = sample_rate;
	m_rc_coef = 1.0 - std::exp(-1.0 / (m_fs * 10e3 * 0.1e-6));      // 10 k + 0.1 uF = 1.0 ms
	double const g = tpt_g(1.0 / (2.0 * PI * 10e3 * 3.3e-6), m_fs);  // 3.3 uF + 10 k = 4.8 Hz
	m_hp_g = g / (1.0 + g);
	double const gi = tpt_g(1.0 / (2.0 * PI * RF_IV * 47e-12), m_fs); // 39 k || 47 pF = 86.8 kHz
	m_iv_g = gi / (1.0 + gi);
	reset();
}

void cem3387_voice::reset()
{
	m_cv = m_target;
	m_p1 = m_svf1 = m_svf2 = m_hp_s = m_iv_l = m_iv_r = 0.0;
	m_lad.fill(0.0);
	for (auto &k : m_key) k = ~0ull;
}

void cem3387_voice::process(int16_t pcm, double &left, double &right)
{
	// --- CV networks (SK105 sheet 3/4) ---
	for (unsigned b : {0u, 1u, 2u, 4u})
		m_cv[b] += (m_target[b] - m_cv[b]) * m_rc_coef;            // 10 k + 0.1 uF, 1.0 ms
	{   // Gain: (1N914 + 6.8 k) || 56 k into 0.1 uF
		double const d = m_target[3] - m_cv[3];
		double i = d / 56e3;
		if (d > 0.6) i += (d - 0.6) / 6.8e3;
		m_cv[3] += i / 0.1e-6 / m_fs;
	}

	double x = double(pcm) / 32768.0 * IN_PEAK_V;

	// --- coefficients (recomputed only when the CV that determines them changes) ---
	bool const q_changed = bits(m_cv[2]) != m_key[2], fc2_changed = bits(m_cv[1]) != m_key[1];
	if (bits(m_cv[0]) != m_key[0])
	{
		m_key[0] = bits(m_cv[0]);
		double const wc1 = cem_law::fc1_hz(m_cv[0]);
		double const g = tpt_g(CHEB_REAL * wc1, m_fs);
		m_c1G = g / (1.0 + g);
		double const g2 = tpt_g(CHEB_W0 * wc1, m_fs), k = 1.0 / CHEB_Q;
		m_c1a1 = 1.0 / (1.0 + g2 * (g2 + k)); m_c1a2 = g2 * m_c1a1; m_c1a3 = g2 * m_c1a2;
	}
	if (fc2_changed)
	{
		m_key[1] = bits(m_cv[1]);
		double const g = tpt_g(cem_law::fc2_hz(m_cv[1]), m_fs), G = g / (1.0 + g);
		m_c2G = G;
		m_c2s0 = G * G * G * (1 - G); m_c2s1 = G * G * (1 - G); m_c2s2 = G * (1 - G); m_c2s3 = (1 - G);
	}
	if (q_changed) { m_key[2] = bits(m_cv[2]); m_k = cem_law::resonance_k(m_cv[2]); }
	if (q_changed || fc2_changed) { double const G = m_c2G; m_den = 1.0 + m_k * G * G * G * G; }
	if (bits(m_cv[3]) != m_key[3]) { m_key[3] = bits(m_cv[3]); m_gain = std::pow(10.0, cem_law::gain_db(m_cv[3]) / 20.0); }
	if (bits(m_cv[4]) != m_key[4]) { m_key[4] = bits(m_cv[4]); cem_law::pan_lr(m_cv[4], m_pl, m_pr); }
	// --- 3-pole 1 dB Chebyshev reconstruction section, edge = Fc1 ---
	{
		double const G = m_c1G;
		double const v = (x - m_p1) * G, y = v + m_p1; m_p1 = y + v; x = y;
	}
	{
		double const a1 = m_c1a1, a2 = m_c1a2, a3 = m_c1a3;
		double const v3 = x - m_svf2, v1 = a1 * m_svf1 + a2 * v3, v2 = m_svf2 + a2 * m_svf1 + a3 * v3;
		m_svf1 = 2.0 * v1 - m_svf1; m_svf2 = 2.0 * v2 - m_svf2; x = v2;
	}

	// --- 4-pole resonant section, cutoff Fc2, feedback from Q (zero-delay solve, soft limit) ---
	{
		double const G = m_c2G, k = m_k;
		double const s = m_c2s0 * m_lad[0] + m_c2s1 * m_lad[1] + m_c2s2 * m_lad[2] + m_c2s3 * m_lad[3];
		double u = (x - k * s) / m_den;
		u = VCF_LIMIT_V * std::tanh(u / VCF_LIMIT_V);
		for (double &st : m_lad) { double const v = (u - st) * G, y = v + st; st = y + v; u = y; }
		x = VCF_LIMIT_V * std::tanh(u / VCF_LIMIT_V);
		// excitation: the real loop is never silent; tiny noise lets k >= 4 start oscillating
		if (k >= 4.0 && std::fabs(x) < 1e-9) m_lad[0] += 1e-7;
	}

	// --- VCF OUT -> 3.3 uF / 10 k -> VCA IN (4.8 Hz high-pass) ---
	{
		double const v = (x - m_hp_s) * m_hp_g, lp = v + m_hp_s; m_hp_s = lp + v; x -= lp;
	}

	// --- VCA + pan: current outputs, 5532 I/V (39 k || 47 pF) ---
	double const gain = m_gain, pl = m_pl, pr = m_pr;
	double const i = x / RIN_VCA * gain;
	double const vl = i * pl * RF_IV, vr = i * pr * RF_IV;
	{ double const v = (vl - m_iv_l) * m_iv_g, y = v + m_iv_l; m_iv_l = y + v; left = y; }
	{ double const v = (vr - m_iv_r) * m_iv_g, y = v + m_iv_r; m_iv_r = y + v; right = y; }
}

void cem3387_bank::process(int16_t const pcm[16], double &left, double &right)
{
	double l = 0, r = 0;
	for (unsigned v = 0; v < 16; ++v)
	{
		double vl, vr;
		m_voice[v].process(pcm[v], vl, vr);
		l += vl; r += vr;
	}
	left = l * MIX_GAIN; right = r * MIX_GAIN;
}

} // namespace eiii
