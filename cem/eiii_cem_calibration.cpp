#include "eiii_cem_calibration.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace eiii {
namespace {

struct key_t { char const *name; double cem_params::*d; char const *unit; char const *source; };
key_t const keys[] = {
	{ "supply.vcc", &cem_params::vcc, "V", "SK105 sheet 4 (+12 V)" },
	{ "cv.dac_full_scale_v", &cem_params::dac_full_scale_v, "V", "DAC-6012 IFS x 1.24 k (SK105 sheet 2)" },
	{ "cv.r_fc1", &cem_params::r_fc1, "ohm", "RN2 10 k 2 %" }, { "cv.c_fc1", &cem_params::c_fc1, "F", "C145-C152 0.1 uF" },
	{ "cv.r_fc2", &cem_params::r_fc2, "ohm", "RN3 10 k 2 %" }, { "cv.c_fc2", &cem_params::c_fc2, "F", "C153-C160 0.1 uF" },
	{ "cv.r_q", &cem_params::r_q, "ohm", "R36-R43/R53 10 k 5 %" }, { "cv.c_q", &cem_params::c_q, "F", "C177/C181/... 0.1 uF" },
	{ "cv.r_pan", &cem_params::r_pan, "ohm", "RN4 10 k 2 %" }, { "cv.c_pan", &cem_params::c_pan, "F", "C161-C168 0.1 uF" },
	{ "cv.r_gain_slow", &cem_params::r_gain_slow, "ohm", "RN5 56 k 2 %" },
	{ "cv.r_gain_fast", &cem_params::r_gain_fast, "ohm", "RN1 6.8 k 2 % + 1N914" },
	{ "cv.c_gain", &cem_params::c_gain, "F", "C169-C176 0.1 uF" },
	{ "cv.diode_is", &cem_params::diode_is, "A", "1N914 SPICE IS" }, { "cv.diode_n", &cem_params::diode_n, "", "1N914 SPICE N" },
	{ "cv.diode_rs", &cem_params::diode_rs, "ohm", "1N914 SPICE RS" }, { "cv.diode_vt", &cem_params::diode_vt, "V", "kT/q" },
	{ "cv.load_ua_per_v", &cem_params::cv_load_ua_per_v, "uA/V", "CEM CV input bias (OPEN)" },
	{ "fc.fc1_r0", &cem_params::fc1_r0, "ohm", "datasheet 25 k +-25 %" }, { "fc.fc2_r0", &cem_params::fc2_r0, "ohm", "datasheet 25 k +-25 %" },
	{ "fc.k", &cem_params::fc_k, "", "datasheet 0.045" }, { "fc.vt", &cem_params::fc_vt, "V", "datasheet 28 mV at 20 C" },
	{ "caps.c1", &cem_params::c1, "F", "C44 39 pF" }, { "caps.c2a", &cem_params::c2a, "F", "C45 10 pF" },
	{ "caps.c2b", &cem_params::c2b, "F", "C46 39 pF" }, { "caps.c3a", &cem_params::c3a, "F", "C47 68 pF" },
	{ "caps.c3b", &cem_params::c3b, "F", "C48 15 pF" }, { "caps.c4a", &cem_params::c4a, "F", "C49 68 pF" },
	{ "caps.c4b", &cem_params::c4b, "F", "C123 15 pF" },
	{ "q.v_none", &cem_params::q_v_none, "V", "datasheet +4.5 V" }, { "q.v_osc", &cem_params::q_v_osc, "V", "datasheet +1.0 V" },
	{ "vca.v_knee", &cem_params::vca_v_knee, "V", "datasheet ~0.2 V" }, { "vca.v_m96", &cem_params::vca_v_m96, "V", "datasheet +0.1 V" },
	{ "vca.max_gain", &cem_params::vca_max_gain, "", "datasheet 1.0" },
	{ "pan.v_center", &cem_params::pan_v_center, "V", "datasheet nominal 2.5 V (typ 2.4 V)" },
	{ "pan.linear_half_width", &cem_params::pan_linear_half_width, "V", "datasheet 0.5..4.5 V" },
	{ "pan.v_m80_offset", &cem_params::pan_v_m80_offset, "V", "datasheet -80 dB at +0.1 V" },
	{ "levels.in_peak_v", &cem_params::in_peak_v, "V", "PCM53 + deglitcher (STRONG/TENTATIVE)" },
	{ "levels.in_offset_v", &cem_params::in_offset_v, "V", "OPEN" },
	{ "levels.node_limit_v", &cem_params::node_limit_v, "V", "datasheet (7/12)Vcc-0.5 V p-p" },
	{ "levels.out_limit_v", &cem_params::out_limit_v, "V", "datasheet +-3.5 V" },
	{ "levels.clip_knee_fraction", &cem_params::clip_knee_fraction, "", "MODEL CHOICE" },
	{ "levels.noise_v_rms", &cem_params::noise_v_rms, "V", "datasheet 96 dB S/N" },
	{ "levels.vca_in_limit_a", &cem_params::vca_in_limit_a, "A", "OPEN" },
	{ "output.r_vca_in", &cem_params::r_vca_in, "ohm", "R11 10 k" }, { "output.c_vca_couple", &cem_params::c_vca_couple, "F", "C111 3.3 uF" },
	{ "output.r_iv", &cem_params::r_iv, "ohm", "R19/R27 39 k" }, { "output.c_iv", &cem_params::c_iv, "F", "C121/C122 47 pF" },
	{ "output.r_mix_in", &cem_params::r_mix_in, "ohm", "RN 10 k + 470" }, { "output.r_mix_f", &cem_params::r_mix_f, "ohm", "R4/R5 10 k" },
	{ "output.c_mix_f", &cem_params::c_mix_f, "F", "C2/C3 220 pF" }, { "output.c_mix_couple", &cem_params::c_mix_couple, "F", "C5/C6 47 uF" },
};

bool set_key(cem_params &p, std::string const &k, std::string const &v, std::string &err)
{
	if (k == "device.serial") { p.serial = v; return true; }
	if (k == "output.mix_channels") { p.mix_channels = std::atoi(v.c_str()); return true; }
	if (k == "q.table")
	{
		// "V:k, V:k, ..." measured resonance loop gain vs Q CV
		p.q_table.clear();
		std::stringstream ss(v); std::string item;
		while (std::getline(ss, item, ','))
		{
			double a, b;
			if (std::sscanf(item.c_str(), " %lf : %lf", &a, &b) != 2) { err = "bad q.table entry '" + item + "'"; return false; }
			p.q_table.emplace_back(a, b);
		}
		std::sort(p.q_table.begin(), p.q_table.end());          // any order in the file; ascending volts here
		return true;
	}
	for (auto const &e : keys)
		if (k == e.name)
		{
			char *end; double const d = std::strtod(v.c_str(), &end);
			if (end == v.c_str()) { err = "bad number for " + k; return false; }
			p.*(e.d) = d;
			return true;
		}
	err = "unknown key '" + k + "'";
	return false;
}

} // namespace

std::string cem_calibration::load(std::string const &path)
{
	std::ifstream in(path);
	if (!in) return "cannot open " + path;
	std::string line, err;
	int section = -1, lineno = 0;
	std::vector<std::pair<std::string, std::string>> global_kv;
	std::array<std::vector<std::pair<std::string, std::string>>, 16> voice_kv;
	while (std::getline(in, line))
	{
		++lineno;
		if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
		auto trim = [](std::string s) { size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r"); return a == std::string::npos ? std::string() : s.substr(a, b - a + 1); };
		line = trim(line);
		if (line.empty()) continue;
		if (line.front() == '[')
		{
			int v;
			if (line == "[global]") section = -1;
			else if (std::sscanf(line.c_str(), "[voice %d]", &v) == 1 && v >= 0 && v < 16) section = v;
			else return path + ":" + std::to_string(lineno) + ": bad section " + line;
			continue;
		}
		auto eq = line.find('=');
		if (eq == std::string::npos) return path + ":" + std::to_string(lineno) + ": expected key = value";
		auto kv = std::make_pair(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
		cem_params probe;
		if (!set_key(probe, kv.first, kv.second, err)) return path + ":" + std::to_string(lineno) + ": " + err;
		(section < 0 ? global_kv : voice_kv[size_t(section)]).push_back(kv);
	}
	// global keys first, then per-voice overrides on top
	for (auto const &kv : global_kv) set_key(global, kv.first, kv.second, err);
	for (unsigned v = 0; v < 16; ++v)
	{
		voice[v] = global;
		for (auto const &kv : voice_kv[v]) set_key(voice[v], kv.first, kv.second, err);
	}
	return "";
}

std::string cem_calibration::write_template()
{
	cem_params const p;
	std::string s =
		"# EIII CEM3387 calibration file (eiii-core CEM3387_MODEL_V2). Omitted keys keep their nominal values.\n"
		"# [global] applies to all voices; [voice N] (N = digital voice 0..15) overrides one channel.\n"
		"# Values below are the NOMINAL datasheet/schematic values, not measurements.\n[global]\ndevice.serial = nominal\n";
	char buf[256];
	for (auto const &e : keys)
	{
		std::snprintf(buf, sizeof(buf), "%-26s = %-12.6g # %s %s\n", e.name, p.*(e.d), e.unit, e.source);
		s += buf;
	}
	s += "output.mix_channels       = 16           # channels normalled into the mix (SK107)\n";
	s += "# q.table = 4.5:0, 1.0:4.533               # optional measured resonance loop gain vs Q CV (volts:k)\n";
	return s;
}

} // namespace eiii
