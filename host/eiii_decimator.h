// Stereo integer-ratio FIR decimator (Kaiser-windowed sinc), used by the V2 output path to go from the model rate
// (e.g. 192 kHz) to the host rate (48 kHz) without the in-band droop and aliasing of a plain sample average.
// Default design: pass band to 20 kHz (ripple < 0.001 dB), stop band from 28 kHz at >= 100 dB.
#pragma once
#include <cmath>
#include <vector>

namespace eiii {

class fir_decimator
{
public:
	void design(double in_rate, unsigned ratio, double pass_hz = 20000.0, double stop_hz = 28000.0, double atten_db = 100.0)
	{
		m_ratio = ratio;
		double const pi = 3.14159265358979323846;
		double const dw = 2 * pi * (stop_hz - pass_hz) / in_rate;
		unsigned n = unsigned(std::ceil((atten_db - 8.0) / (2.285 * dw))) | 1u;
		double const beta = atten_db > 50 ? 0.1102 * (atten_db - 8.7) : 0.5842 * std::pow(atten_db - 21, 0.4) + 0.07886 * (atten_db - 21);
		double const fc = 0.5 * (pass_hz + stop_hz) / in_rate;   // normalised cutoff (cycles/sample)
		auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 40; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
		m_h.assign(n, 0.0);
		double sum = 0;
		for (unsigned i = 0; i < n; ++i)
		{
			double const m = double(i) - double(n - 1) / 2, r = 2 * m / double(n - 1);
			double const sinc = m == 0 ? 2 * fc : std::sin(2 * pi * fc * m) / (pi * m);
			m_h[i] = sinc * i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta);
			sum += m_h[i];
		}
		for (double &c : m_h) c /= sum;                        // unity DC gain
		m_buf[0].assign(2 * n, 0.0); m_buf[1].assign(2 * n, 0.0);
		m_pos = 0; m_phase = 0;
	}
	unsigned taps() const { return unsigned(m_h.size()); }
	// push one input frame; returns true (and the output frame) every `ratio` inputs
	bool push(double l, double r, double &ol, double &orr)
	{
		size_t const n = m_h.size();
		m_buf[0][m_pos] = m_buf[0][m_pos + n] = l;            // doubled ring: contiguous window without wrap checks
		m_buf[1][m_pos] = m_buf[1][m_pos + n] = r;
		m_pos = (m_pos + 1) % n;
		if (++m_phase < m_ratio) return false;
		m_phase = 0;
		double al = 0, ar = 0;
		double const *bl = &m_buf[0][m_pos], *br = &m_buf[1][m_pos];
		for (size_t i = 0; i < n; ++i) { al += m_h[i] * bl[i]; ar += m_h[i] * br[i]; }
		ol = al; orr = ar;
		return true;
	}
private:
	std::vector<double> m_h, m_buf[2];
	size_t m_pos = 0;
	unsigned m_ratio = 4, m_phase = 0;
};

} // namespace eiii
