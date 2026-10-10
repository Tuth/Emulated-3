#include "eiii_cem_cv_conditioning.h"
#include <cmath>

namespace eiii {

void cem_cv_conditioning::prepare(cem_params const &p, double fs)
{
	m_vfs = p.dac_full_scale_v;
	m_T = 1.0 / fs;
	double const rc[5] = { p.r_fc1 * p.c_fc1, p.r_fc2 * p.c_fc2, p.r_q * p.c_q, p.r_gain_slow * p.c_gain, p.r_pan * p.c_pan };
	for (unsigned b = 0; b < 5; ++b) m_a[b] = 1.0 - std::exp(-m_T / rc[b]);   // exact for a held (S/H) input
	m_cgain = p.c_gain;
	m_rslow = p.r_gain_slow;
	m_diode_r = p.r_gain_fast + p.diode_rs;
	// tabulate I(d): d = I (R + Rs) + n Vt ln(1 + I/Is), solved by bisection (monotonic)
	m_diode.assign(6001, 0.0f);
	double const nvt = p.diode_n * p.diode_vt;
	for (size_t i = 1; i < m_diode.size(); ++i)
	{
		double const d = double(i) * 1e-3;
		double lo = 0.0, hi = d / m_diode_r;
		for (int it = 0; it < 80; ++it)
		{
			double const mid = 0.5 * (lo + hi);
			double const f = mid * m_diode_r + nvt * std::log1p(mid / p.diode_is) - d;
			(f > 0 ? hi : lo) = mid;
		}
		m_diode[i] = float(0.5 * (lo + hi));
	}
}

double cem_cv_conditioning::diode_path_current(double d) const
{
	if (d <= 0.0) return 0.0;
	double const x = d * 1000.0;
	size_t const n = m_diode.size() - 1;
	if (x >= double(n)) return double(m_diode[n]) + (d - double(n) * 1e-3) / m_diode_r;   // ohmic beyond the table
	size_t const i = size_t(x);
	double const f = x - double(i);
	return double(m_diode[i]) + (double(m_diode[i + 1]) - double(m_diode[i])) * f;
}

void cem_cv_conditioning::reset_to_targets()
{
	for (unsigned b = 0; b < 5; ++b) m_pin[b] = m_target[b];
}

void cem_cv_conditioning::step()
{
	for (unsigned b : {CV_FC1, CV_FC2, CV_Q, CV_PAN})
		m_pin[b] += (m_target[b] - m_pin[b]) * m_a[b];
	double const u = m_target[CV_GAIN], v = m_pin[CV_GAIN];
	if (u - v <= 0.0)
		m_pin[CV_GAIN] = v + (u - v) * m_a[CV_GAIN];           // falling / equal: 56 k only, exact
	else
	{
		// rising: 56 k || (1N914 + 6.8 k); Heun step
		auto dvdt = [&](double vv) { double const d = u - vv; return (d / m_rslow + diode_path_current(d)) / m_cgain; };
		double const k1 = dvdt(v);
		double const k2 = dvdt(v + m_T * k1);
		double nv = v + 0.5 * m_T * (k1 + k2);
		if (nv > u) nv = u;                                    // never overshoot the source (cannot happen for T << tau)
		m_pin[CV_GAIN] = nv;
	}
}

} // namespace eiii
