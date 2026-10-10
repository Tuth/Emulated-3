// CEM3387 + EIII external-network parameters for the V2 model, with nominal defaults and an optional calibration file.
// Every default is a datasheet or schematic value (sources in analysis/CEM3387_EVIDENCE_MAP.md); a calibration file
// (analysis/CEM3387_CALIBRATION_FORMAT.md) may replace any of them globally or per voice, e.g. with values measured on a
// real chip.  No measured values ship with this code.
#pragma once
#include <array>
#include <string>
#include <vector>

namespace eiii {

struct cem_params
{
	// ---- identity ----
	std::string serial = "nominal";            // "nominal" = datasheet/schematic values only

	// ---- supplies / CV DAC (SK105 sheet 2) ----
	double vcc = 12.0;                         // CEM3387 +Vcc; control ranges are ratiometric (5/12)·Vcc
	double dac_full_scale_v = 4.96;            // DAC-6012 IFS 4.0 mA x 1.24 k at code 0xFFF

	// ---- CV smoothing networks (SK105 sheets 3-11) ----
	double r_fc1 = 10e3, c_fc1 = 0.1e-6;       // RN2 10 k 2 %, C145-C152 0.1 uF
	double r_fc2 = 10e3, c_fc2 = 0.1e-6;       // RN3 10 k 2 %, C153-C160
	double r_q = 10e3, c_q = 0.1e-6;           // R36-R43/R53 10 k 5 %, C177/C181... at the RES pin
	double r_pan = 10e3, c_pan = 0.1e-6;       // RN4 10 k 2 %, C161-C168
	double r_gain_slow = 56e3;                 // RN5 56 k 2 % (both directions)
	double r_gain_fast = 6.8e3;                // RN1 6.8 k 2 % in series with a 1N914 (anode at the S/H output)
	double c_gain = 0.1e-6;                    // C169-C176
	double diode_is = 2.52e-9, diode_n = 1.752, diode_rs = 0.568;   // 1N914 standard SPICE model (LTspice standard.dio)
	double diode_vt = 0.02585;                 // kT/q at the SPICE TNOM (27 C)
	double cv_load_ua_per_v = 0.0;             // CEM CV input bias (datasheet -0.4 uA/V, sign convention OPEN) -> off

	// ---- filter control law (datasheet p.7): Rcv = r0 [exp(k (Vr - V)/Vt) + 1] ----
	double fc1_r0 = 25e3, fc2_r0 = 25e3;       // nominal 25 k, tolerance +-25 % (per chip, per filter)
	double fc_k = 0.045, fc_vt = 0.028;        // datasheet constants at 20 C
	// ---- filter capacitors (SK105 sheet 4, identical on all 16 channels) ----
	double c1 = 39e-12, c2a = 10e-12, c2b = 39e-12;              // 3-pole: 1st-order (C1) + 2nd-order (C2A, C2B)
	double c3a = 68e-12, c3b = 15e-12, c4a = 68e-12, c4b = 15e-12; // 4-pole: two 2nd-order sections

	// ---- resonance (datasheet p.3): no feedback at +4.5 V, sustained oscillation at +1.0 V ----
	double q_v_none = 4.5, q_v_osc = 1.0;
	// optional measured law: (volts, loop gain k) points, sorted by volts; empty = linear between the datasheet end points
	std::vector<std::pair<double, double>> q_table;

	// ---- main VCA (datasheet p.3, p.9; CEM3389 p.3 corroboration) ----
	double vca_v_knee = 0.2;                   // exponential below ~0.2 V, linear (through 0) above, 0 dB at Vr
	double vca_v_m96 = 0.1;                    // "Gain C.V. for -96 dB gain: +0.1 V"
	double vca_max_gain = 1.0;                 // max current gain

	// ---- pan (datasheet p.4, p.9) ----
	double pan_v_center = 2.5;                 // nominal equal-gain point (typ +2.4 V in the table)
	double pan_linear_half_width = 2.0;        // linear between +0.5 V and +4.5 V
	double pan_v_m80_offset = 2.4;             // -80 dB on one side 2.4 V from centre (left: +0.1 V)

	// ---- signal levels / headroom (datasheet p.3, p.5; SK105 sheet 4) ----
	double in_peak_v = 1.8;                    // PCM full scale at CEM IN (PCM53 + deglitcher, OUTPUT_BOARD 7.4)
	double in_offset_v = 0.0;                  // DC mismatch vs the 0.63 V input level (OPEN, ~-0.03..-0.1 V)
	double node_limit_v = 3.25;                // internal clipping ~ ((7/12)Vcc - 0.5 V) p-p  -> +-3.25 V
	double out_limit_v = 3.5;                  // VCF OUT max swing +-3.5 V
	double clip_knee_fraction = 0.85;          // soft knee starts at 85 % of the limit (MODEL CHOICE)
	double noise_v_rms = 28e-6;                // 96 dB S/N re 5 V p-p, referred to the filter input (MODEL CHOICE: white)
	double vca_in_limit_a = 0.0;               // VCA input current saturation (OPEN; 0 = linear)

	// ---- VCF -> VCA coupling and output stages (SK105 sheet 4, SK107 sheet 2) ----
	double r_vca_in = 10e3, c_vca_couple = 3.3e-6;               // R11 10 k, C111 3.3 uF
	double r_iv = 39e3, c_iv = 47e-12;                           // R19/R27 39 k || C121/C122 47 pF (5532 I/V)
	double r_mix_in = 10e3 + 470.0, r_mix_f = 10e3, c_mix_f = 220e-12, c_mix_couple = 47e-6;   // SK107 summer
	int mix_channels = 16;                                       // channels normalled into the mix bus

	double vr() const { return vcc * 5.0 / 12.0; }
};

// nominal parameters for all 16 voices plus the shared output stage; per-voice overrides from a calibration file
struct cem_calibration
{
	cem_params global;
	std::array<cem_params, 16> voice;
	cem_calibration() { voice.fill(global); }
	// "section.key = value" lines; "[voice N]" sections override voice N only; '#' comments.  Returns "" or an error.
	std::string load(std::string const &path);
	static std::string write_template();       // a commented template with all nominal values
};

} // namespace eiii
