#include "eiii_cem3387_v2.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace eiii {
namespace {
constexpr double PI = 3.14159265358979323846;
inline uint64_t bits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

// pan shape: linear (slope s) within +-w of the centre, exponential tails matched in value and slope (time constant a),
// reaching -80 dB at m80 volts from the centre.  Solved for s by bisection.
struct pan_shape { double s, le, a; };
pan_shape solve_pan(cem_params const &p)
{
	double const w = p.pan_linear_half_width, delta = p.pan_v_m80_offset - w;
	double lo = 1e-6, hi = 0.5 / w - 1e-9;                     // s < 0.5/w keeps the edge value le = 0.5 - s w positive
	for (int it = 0; it < 200; ++it)
	{
		double const s = 0.5 * (lo + hi), le = 0.5 - s * w, a = le / s;
		double const at_m80 = le * std::exp(-delta / a);       // decreasing in s
		(at_m80 > 1e-4 ? lo : hi) = s;
	}
	double const s = 0.5 * (lo + hi), le = 0.5 - s * w;
	return { s, le, le / s };
}
inline double pan_eval(pan_shape const &ps, double w, double vc, double v)
{
	double const dv = v - vc;
	if (dv < -w) return ps.le * std::exp((dv + w) / ps.a);
	if (dv > w) return 1.0 - ps.le * std::exp(-(dv - w) / ps.a);
	return 0.5 + ps.s * dv;
}
inline double vca_eval(cem_params const &p, double tail_a, double v)
{
	double const vr = p.vr();
	if (v >= p.vca_v_knee) return p.vca_max_gain * std::min(v, vr) / vr;
	double const gk = p.vca_max_gain * p.vca_v_knee / vr;
	return gk * std::exp((v - p.vca_v_knee) / tail_a);
}
inline double vca_tail_a(cem_params const &p)
{
	double const gk = p.vca_max_gain * p.vca_v_knee / p.vr();
	return (p.vca_v_knee - p.vca_v_m96) / std::log(gk / std::pow(10.0, -96.0 / 20.0));
}
} // namespace

// ---------------------------------------------------------------- laws
double cem_laws::fc1_s1_hz(cem_params const &p, double v) { return 1.0 / (2 * PI * rcv(p, p.fc1_r0, v) * p.c1); }
double cem_laws::fc1_s2_hz(cem_params const &p, double v) { return 1.0 / (2 * PI * rcv(p, p.fc1_r0, v) * std::sqrt(p.c2a * p.c2b)); }
double cem_laws::fc2_w0_hz(cem_params const &p, double v) { return 1.0 / (2 * PI * rcv(p, p.fc2_r0, v) * std::sqrt(p.c3a * p.c3b)); }

double cem_laws::loop_gain_osc(cem_params const &p)
{
	// H3 H4 (non-inverting convention) on a frequency axis normalised to section 3; find the -180 degree point
	double const w4r = std::sqrt(p.c3a * p.c3b) / std::sqrt(p.c4a * p.c4b), d3 = std::sqrt(p.c3a / p.c3b), d4 = std::sqrt(p.c4a / p.c4b);
	auto h = [&](double x) {
		std::complex<double> const j(0, 1);
		auto sec = [&](double xx, double d) { return 1.0 / (1.0 - xx * xx + j * d * xx); };
		return sec(x, d3) * sec(x / w4r, d4);
	};
	double lo = 0.01, hi = 100.0;                              // phase goes 0 -> -360 deg; -180 is where imag changes sign
	for (int it = 0; it < 200; ++it)
	{
		double const mid = std::sqrt(lo * hi);
		(std::arg(h(mid)) < 0 && std::imag(h(mid)) < 0 ? lo : hi) = mid;
	}
	return 1.0 / std::abs(h(std::sqrt(lo * hi)));
}

double cem_laws::q_k(cem_params const &p, double v)
{
	if (!p.q_table.empty())
	{
		auto const &t = p.q_table;
		if (v <= t.front().first) return t.front().second;
		if (v >= t.back().first) return t.back().second;
		for (size_t i = 1; i < t.size(); ++i)
			if (v <= t[i].first)
				return t[i - 1].second + (t[i].second - t[i - 1].second) * (v - t[i - 1].first) / (t[i].first - t[i - 1].first);
	}
	double const k = loop_gain_osc(p) * (p.q_v_none - v) / (p.q_v_none - p.q_v_osc);
	return std::max(0.0, k);
}

double cem_laws::vca_gain(cem_params const &p, double v) { return vca_eval(p, vca_tail_a(p), v); }
double cem_laws::pan_left(cem_params const &p, double v) { return pan_eval(solve_pan(p), p.pan_linear_half_width, p.pan_v_center, v); }

// ---------------------------------------------------------------- voice

void cem3387_v2_voice::prepare(cem_params const &p, double fs, cem_v2_options const &o, uint32_t seed)
{
	m_p = p; m_o = o; m_fs = fs; m_T = 1.0 / fs;
	m_cv.prepare(p, fs);
	m_in_scale = p.in_peak_v / 32768.0;
	m_wlim = 2 * PI * o.warp_limit_hz;
	m_warp_scale = std::tan(m_wlim * m_T / 2) / (m_wlim * m_T / 2);
	m_d2 = std::sqrt(p.c2a / p.c2b); m_d3 = std::sqrt(p.c3a / p.c3b); m_d4 = std::sqrt(p.c4a / p.c4b);
	double const ghp = g_for(1.0 / (p.r_vca_in * p.c_vca_couple));
	m_hpG = ghp / (1.0 + ghp);
	m_rng = seed ? seed : 1;
	m_noise_scale = o.noise ? p.noise_v_rms * std::sqrt(3.0) * std::sqrt((fs / 2) / 20000.0) : 0.0;   // white, p.noise in 0-20 kHz
	pan_shape const ps = solve_pan(p);
	m_pan_s = ps.s; m_pan_le = ps.le; m_pan_a = ps.a;
	m_vca_a = vca_tail_a(p);
	m_kosc = cem_laws::loop_gain_osc(p);
	reset();
}

void cem3387_v2_voice::reset()
{
	m_cv.reset_to_targets();
	m_s1 = m_hp = m_vcf = 0;
	m_s2[0] = m_s2[1] = m_s3[0] = m_s3[1] = m_s4[0] = m_s4[1] = 0;
	for (auto &k : m_key) k = ~0ull;
	update_coefficients();
}

double cem3387_v2_voice::g_for(double w) const
{
	if (w <= m_wlim) return std::tan(w * m_T / 2);             // prewarped: pole frequency exact
	return (w * m_T / 2) * m_warp_scale;                       // bilinear matched at the warp limit: in-band shape kept
}

double cem3387_v2_voice::softclip(double x, double lim) const
{
	double const knee = lim * m_p.clip_knee_fraction, ax = std::fabs(x);
	if (ax <= knee) return x;
	double const r = lim - knee, y = knee + r * std::tanh((ax - knee) / r);
	return x < 0 ? -y : y;
}

void cem3387_v2_voice::update_coefficients()
{
	double const v1 = m_cv.pin(CV_FC1), v2 = m_cv.pin(CV_FC2), vq = m_cv.pin(CV_Q), vg = m_cv.pin(CV_GAIN), vp = m_cv.pin(CV_PAN);
	if (bits(v1) != m_key[0])
	{
		m_key[0] = bits(v1);
		double const r = cem_laws::rcv(m_p, m_p.fc1_r0, v1);
		double const g1 = g_for(1.0 / (r * m_p.c1));
		m_G1 = g1 / (1.0 + g1);
		double const g = g_for(1.0 / (r * std::sqrt(m_p.c2a * m_p.c2b)));
		m_s2a1 = 1.0 / (1.0 + g * (g + m_d2)); m_s2a2 = g * m_s2a1; m_s2a3 = g * m_s2a2;
	}
	if (bits(v2) != m_key[1])
	{
		m_key[1] = bits(v2);
		double const r = cem_laws::rcv(m_p, m_p.fc2_r0, v2);
		double const g3 = g_for(1.0 / (r * std::sqrt(m_p.c3a * m_p.c3b)));
		m_s3a1 = 1.0 / (1.0 + g3 * (g3 + m_d3)); m_s3a2 = g3 * m_s3a1; m_s3a3 = g3 * m_s3a2;
		if (m_p.c4a == m_p.c3a && m_p.c4b == m_p.c3b) { m_s4a1 = m_s3a1; m_s4a2 = m_s3a2; m_s4a3 = m_s3a3; }   // identical sections (EIII)
		else
		{
			double const g4 = g_for(1.0 / (r * std::sqrt(m_p.c4a * m_p.c4b)));
			m_s4a1 = 1.0 / (1.0 + g4 * (g4 + m_d4)); m_s4a2 = g4 * m_s4a1; m_s4a3 = g4 * m_s4a2;
		}
	}
	if (bits(vq) != m_key[2])
	{
		m_key[2] = bits(vq);
		m_k = m_p.q_table.empty() ? std::max(0.0, m_kosc * (m_p.q_v_none - vq) / (m_p.q_v_none - m_p.q_v_osc)) : cem_laws::q_k(m_p, vq);
	}
	if (bits(vg) != m_key[3]) { m_key[3] = bits(vg); m_gain = vca_eval(m_p, m_vca_a, vg); }
	if (bits(vp) != m_key[4])
	{
		m_key[4] = bits(vp);
		pan_shape const ps { m_pan_s, m_pan_le, m_pan_a };
		m_left = pan_eval(ps, m_p.pan_linear_half_width, m_p.pan_v_center, vp);
	}
}

void cem3387_v2_voice::process_volts(double vin, double &il, double &ir)
{
	m_cv.step();
	update_coefficients();
	double x = vin + m_p.in_offset_v;
	if (m_noise_scale != 0.0)
	{
		m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
		x += (double(m_rng) * (2.0 / 4294967296.0) - 1.0) * m_noise_scale;
	}
	bool const nl = m_o.nonlinear;
	if (nl) x = softclip(x, m_p.node_limit_v);
	// S1: 1st-order low-pass (C1)
	double y;
	{ double const v = (x - m_s1) * m_G1; y = v + m_s1; m_s1 = y + v; }
	// S2: 2nd-order low-pass (C2a, C2b), SVF form
	{
		double const v3 = y - m_s2[1], v1 = m_s2a1 * m_s2[0] + m_s2a2 * v3, v2 = m_s2[1] + m_s2a2 * m_s2[0] + m_s2a3 * v3;
		m_s2[0] = 2 * v1 - m_s2[0]; m_s2[1] = 2 * v2 - m_s2[1]; y = v2;
	}
	if (nl) y = softclip(y, m_p.node_limit_v);
	// 4-pole with resonance feedback, zero-delay loop: y4 = G u + S, u = y - k y4
	double const b3 = (1 - m_s3a3) * m_s3[1] + m_s3a2 * m_s3[0], b4 = (1 - m_s4a3) * m_s4[1] + m_s4a2 * m_s4[0];
	double const G = m_s3a3 * m_s4a3, S = m_s4a3 * b3 + b4;
	double const y4lin = (G * y + S) / (1.0 + m_k * G);
	double u = y - m_k * y4lin;
	if (nl) u = softclip(u, m_p.node_limit_v);
	double y3;
	{
		double const v3 = u - m_s3[1], v1 = m_s3a1 * m_s3[0] + m_s3a2 * v3, v2 = m_s3[1] + m_s3a2 * m_s3[0] + m_s3a3 * v3;
		m_s3[0] = 2 * v1 - m_s3[0]; m_s3[1] = 2 * v2 - m_s3[1]; y3 = v2;
	}
	double y4;
	{
		double const v3 = y3 - m_s4[1], v1 = m_s4a1 * m_s4[0] + m_s4a2 * v3, v2 = m_s4[1] + m_s4a2 * m_s4[0] + m_s4a3 * v3;
		m_s4[0] = 2 * v1 - m_s4[0]; m_s4[1] = 2 * v2 - m_s4[1]; y4 = v2;
	}
	m_vcf = nl ? softclip(y4, m_p.out_limit_v) : y4;
	// VCF OUT -> 3.3 uF -> 10 k -> VCA IN (virtual ground): high-pass, then current
	double hp;
	{ double const v = (m_vcf - m_hp) * m_hpG, lp = v + m_hp; m_hp = lp + v; hp = m_vcf - lp; }
	double i = hp / m_p.r_vca_in;
	if (m_p.vca_in_limit_a > 0) i = softclip(i, m_p.vca_in_limit_a);
	i *= m_gain;
	il = i * m_left;
	ir = i * (1.0 - m_left);
}

// ---------------------------------------------------------------- bank + output stage
void cem3387_v2_bank::prepare(cem_calibration const &cal, double fs, cem_v2_options const &o)
{
	for (unsigned v = 0; v < 16; ++v) m_voice[v].prepare(cal.voice[v], fs, o, 0x9E3779B9u * (v + 1));
	m_out = cal.global;
	double const T = 1.0 / fs, wlim = 2 * PI * o.warp_limit_hz, scale = std::tan(wlim * T / 2) / (wlim * T / 2);
	auto gfor = [&](double w) { return w <= wlim ? std::tan(w * T / 2) : (w * T / 2) * scale; };
	auto G = [&](double w) { double const g = gfor(w); return g / (1.0 + g); };
	for (int c = 0; c < 2; ++c)
	{
		m_iv[c].G = G(1.0 / (m_out.r_iv * m_out.c_iv));
		m_mixlp[c].G = G(1.0 / (m_out.r_mix_f * m_out.c_mix_f));
		m_mixhp[c].G = G(double(m_out.mix_channels) / (m_out.r_mix_in * m_out.c_mix_couple));
	}
	m_iv_r = m_out.r_iv;
	m_mix_gain = m_out.r_mix_f / m_out.r_mix_in;
	reset();
}

void cem3387_v2_bank::reset()
{
	for (auto &v : m_voice) v.reset();
	for (int c = 0; c < 2; ++c) m_iv[c].s = m_mixlp[c].s = m_mixhp[c].s = 0;
}

void cem3387_v2_bank::process(int16_t const pcm[16], double &left, double &right)
{
	double il = 0, ir = 0;
	for (unsigned v = 0; v < 16; ++v)
	{
		double l, r;
		m_voice[v].process(pcm[v], l, r);
		il += l; ir += r;
	}
	// identical linear I/V stages per channel summed on one bus == one stage on the summed currents
	double const out[2] = { il * m_iv_r, ir * m_iv_r };
	double res[2];
	for (int c = 0; c < 2; ++c)
	{
		double const v = m_iv[c].lp(out[c]);                   // 5532 I/V, 39 k || 47 pF
		double const m = m_mixlp[c].lp(v * m_mix_gain);        // SK107 summer, 10 k || 220 pF
		res[c] = m - m_mixhp[c].lp(m);                         // 47 uF bus coupling (16 normalled channels)
	}
	left = res[0]; right = res[1];
}

} // namespace eiii
