// EIII analog voice model: CV DAC -> SK105 smoothing networks -> CEM3387 (3-pole Fc1 reconstruction,
// 4-pole Fc2/Q filter, VCF->VCA coupling, VCA, pan) -> 5532 I/V -> SK107 mix.  Portable C++17, no MAME.
// Laws: CEM3387 datasheet + EIII schematics (analysis/OUTPUT_BOARD_SCHEMATIC_READING.md, tools/voice_hw_model.py).
// Status / approximations: analysis/EIII_CEM3387_MODEL_STATUS.md.
#pragma once
#include <array>
#include <cstdint>

namespace eiii {

struct cem_law
{
	static constexpr double V_FS = 4.0e-3 * 1.24e3;   // DAC-6012 full scale 4.96 V (SK105 sheet 2)
	static constexpr double VR = 5.0;                 // CEM3387 reference (5/12 * Vcc, Vcc = 12 V)
	static double word_to_volts(uint16_t word) { return double((word >> 2) & 0xfff) / 4095.0 * V_FS; }
	static double rcv(double v);                      // datasheet control resistance
	static double fc1_hz(double v);                   // 3-pole Chebyshev, C2A = 10 pF
	static double fc2_hz(double v);                   // 4-pole, Cb = 15 pF
	static double gain_db(double v);                  // exponential below 0.2 V, linear above
	static void pan_lr(double v, double &l, double &r);
	static double resonance_k(double v);              // 4.5 V -> 0, 1.0 V -> 4 (oscillation), APPROX linear
};

class cem3387_voice
{
public:
	void prepare(double sample_rate);
	void reset();
	// raw firmware CV word (bits 13-2 used), bank 0 Fc1, 1 Fc2, 2 Q, 3 Gain, 4 Pan
	void set_cv(unsigned bank, uint16_t word) { m_target[bank] = cem_law::word_to_volts(word); }
	// one output sample; pcm = held 16-bit sample at the CEM input; returns jack-board volts (L, R)
	void process(int16_t pcm, double &left, double &right);
	double cv_volts(unsigned bank) const { return m_cv[bank]; }

private:
	double m_fs = 192000.0;
	std::array<double, 5> m_target{0, 0, 0, 0, 2.48};
	std::array<double, 5> m_cv{0, 0, 0, 0, 2.48};
	double m_rc_coef = 0.0;
	// filters
	double m_p1 = 0, m_svf1 = 0, m_svf2 = 0;          // 3-pole: one-pole + SVF
	std::array<double, 4> m_lad{};                   // 4-pole ladder states
	double m_hp_s = 0;                               // VCF->VCA coupling
	double m_hp_g = 0;
	double m_iv_l = 0, m_iv_r = 0, m_iv_g = 0;       // I/V 86 kHz pole
	// coefficient caches, keyed on the exact bit pattern of the smoothed CV that determines them (pure functions of
	// that value, so reusing them is bit-identical; idle voices' CVs settle to exact fixed points)
	uint64_t m_key[5] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull};
	double m_c1G = 0, m_c1a1 = 0, m_c1a2 = 0, m_c1a3 = 0;                      // Fc1 sections
	double m_c2G = 0, m_c2s0 = 0, m_c2s1 = 0, m_c2s2 = 0, m_c2s3 = 0;          // Fc2 ladder
	double m_k = 0, m_den = 1;                                                  // Q (den depends on Fc2 and Q)
	double m_gain = 0, m_pl = 0, m_pr = 0;                                      // VCA, pan
};

class cem3387_bank
{
public:
	void prepare(double sample_rate) { for (auto &v : m_voice) v.prepare(sample_rate); }
	void reset() { for (auto &v : m_voice) v.reset(); }
	// CV DAC write as the firmware addresses it: 0xDE0000 + 2*(16*bank + channel); channel c feeds digital voice 15 - c
	void cv_write(unsigned bank, unsigned channel, uint16_t word) { m_voice[15 - channel].set_cv(bank, word); }
	// pcm[v] = held output of digital voice v; returns stereo mix in volts at the LEFT/RIGHT jacks
	void process(int16_t const pcm[16], double &left, double &right);
	cem3387_voice &voice(unsigned v) { return m_voice[v]; }

private:
	std::array<cem3387_voice, 16> m_voice;
};

} // namespace eiii
