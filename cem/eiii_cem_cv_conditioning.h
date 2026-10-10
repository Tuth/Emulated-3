// EIII CV conditioning for one CEM3387 channel (SK105 sheets 2-11): ideal DAC code -> voltage at each CEM control pin.
//
//   firmware word -> DAC-6012 + LF356 (code/4095 x 4.96 V) -> SSM2300 S/H (step at the write time) -> network -> pin
//   Fc1, Fc2, Q, Pan:  R (10 k) into C (0.1 uF) to ground            -> exact one-pole step response, tau = R C = 1.0 ms
//   Gain:              (1N914 + 6.8 k) || 56 k into 0.1 uF            -> diode path conducts only while the S/H output is
//                                                                       above the capacitor: fast attack, 5.6 ms release
// The diode is modelled with the Shockley equation and its series resistance (standard 1N914 SPICE parameters); the
// diode + 6.8 k current is tabulated at 1 mV resolution and integrated with Heun's method (time step << 0.6 ms).
#pragma once
#include "eiii_cem_calibration.h"
#include <cstdint>
#include <vector>

namespace eiii {

enum cem_cv : unsigned { CV_FC1 = 0, CV_FC2 = 1, CV_Q = 2, CV_GAIN = 3, CV_PAN = 4 };

class cem_cv_conditioning
{
public:
	void prepare(cem_params const &p, double sample_rate);
	void reset_to_targets();                                   // pins = targets (power-up settled)
	double word_to_volts(uint16_t word) const { return double((word >> 2) & 0xfff) / 4095.0 * m_vfs; }
	void set_word(unsigned bank, uint16_t word) { m_target[bank] = word_to_volts(word); }
	void set_volts(unsigned bank, double v) { m_target[bank] = v; }   // for the lab tool
	void step();                                               // advance one sample period
	double pin(unsigned bank) const { return m_pin[bank]; }
	double target(unsigned bank) const { return m_target[bank]; }
	double diode_path_current(double d) const;                 // A through (1N914 + 6.8 k) for a forward voltage d
private:
	double m_vfs = 4.96, m_T = 0, m_cgain = 0.1e-6, m_rslow = 56e3;
	double m_a[5] = {};                                        // exact one-pole coefficients (index CV_GAIN: slow path)
	double m_target[5] = {0, 0, 0, 0, 2.48}, m_pin[5] = {0, 0, 0, 0, 2.48};
	std::vector<float> m_diode;                                // fast-path current vs d, 1 mV steps, 0..6 V
	double m_diode_r = 6.8e3;
};

} // namespace eiii
