// Test rig: an eiii::engine driven like a host would (offline mode, 256-frame blocks), with the PanelUI adapter.
#pragma once
#include "panel/eiii_panel_adapter.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace eiii_test {

inline std::string env(char const *k, char const *def = "") { char const *v = getenv(k); return v ? v : def; }

inline eiii::engine_config media_config(bool realtime = false)
{
	eiii::engine_config c;
	c.rt.rom_dir = env("EIII_ROM_DIR", "roms");
	c.rt.hd_path = env("EIII_HD");
	c.rt.cd_path = env("EIII_CD");
	c.rt.sample_rate = 48000.0;
	c.max_block = 256;
	c.realtime = realtime;
	return c;
}

struct rig
{
	eiii::engine e;
	eiii::panel_adapter ui{e};
	size_t block = 256;
	std::vector<float> l = std::vector<float>(4096), r = std::vector<float>(4096);
	double sum_sq = 0; size_t n_meas = 0; float peak = 0;
	uint64_t hash = 1469598103934665603ull;     // FNV-1a over the output samples
	bool poll_every_block = true;
	std::function<void()> on_block;

	explicit rig(eiii::engine_config const &c) { e.start(c); }
	double now() const { return ui.snapshot().emu_seconds; }
	void pump_frames(size_t frames)
	{
		while (frames)
		{
			size_t const n = std::min(frames, block);
			e.pull(l.data(), r.data(), n);
			for (size_t i = 0; i < n; ++i)
			{
				sum_sq += double(l[i]) * l[i] + double(r[i]) * r[i]; ++n_meas;
				peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
				uint32_t bits[2]; memcpy(&bits[0], &l[i], 4); memcpy(&bits[1], &r[i], 4);
				for (uint32_t b : bits) for (int k = 0; k < 4; ++k) { hash ^= (b >> (8 * k)) & 0xff; hash *= 1099511628211ull; }
			}
			frames -= n;
			if (poll_every_block) ui.poll();
			if (on_block) on_block();
		}
	}
	void pump(double seconds) { pump_frames(size_t(seconds * e.sample_rate())); }
	std::string lcd() const
	{
		eiii::lcd_view v; ui.read_lcd(v);
		std::string t; for (int i = 0; i < 4; ++i) { t += eiii::lcd_row_text(v, i); if (i < 3) t += "\n"; }
		return t;
	}
	std::string row(int i) const { eiii::lcd_view v; ui.read_lcd(v); return eiii::lcd_row_text(v, i); }
	// pump until pred() holds (checked every block) or timeout (emulated seconds)
	bool wait(std::function<bool()> pred, double timeout)
	{
		size_t const max_blocks = size_t(timeout * e.sample_rate() / double(block)) + 1;
		for (size_t i = 0; i < max_blocks; ++i) { ui.poll(); if (pred()) return true; pump_frames(block); }
		ui.poll(); return pred();
	}
	bool wait_text(std::string const &s, double timeout) { return wait([&] { return lcd().find(s) != std::string::npos; }, timeout); }
	bool wait_row(int i, std::string const &prefix, double timeout) { return wait([&] { return row(i).compare(0, prefix.size(), prefix) == 0; }, timeout); }
	void press(char const *id, double hold = 0.08, double after = 0.25)
	{
		int const c = eiii::panel_find_control(id);
		if (c < 0) { fprintf(stderr, "rig: unknown control %s\n", id); abort(); }
		ui.press_button(c); pump(hold); ui.release_button(c); pump(after);
	}
	void midi(std::vector<uint8_t> const &b) { e.midi(0, b.data(), uint8_t(b.size())); }
	double rms_over(double seconds)
	{
		sum_sq = 0; n_meas = 0; pump(seconds);
		return n_meas ? std::sqrt(sum_sq / double(2 * n_meas)) : 0.0;
	}
	bool led(char const *id) { return ui.led(eiii::panel_find_led(id)); }
	std::string leds()
	{
		std::string s;
		for (size_t i = 0; i < eiii::panel_led_count; ++i) if (ui.led(int(i))) { if (!s.empty()) s += ","; s += eiii::panel_leds[i].label; }
		return s;
	}
};

} // namespace eiii_test
