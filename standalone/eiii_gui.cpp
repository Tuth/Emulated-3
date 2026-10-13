// eiii-standalone --gui — see eiii_gui.h.
#include "eiii_gui.h"
#include "host_io.h"
#include "ui_macro.h"
#include "panel/eiii_panel_adapter.h"
#include "runtime/eiii_engine.h"
#include "ui/panel_window.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#ifndef _WIN32
#include <poll.h>
#endif
#include "native_path.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> s_quit{false};
void on_signal(int) { s_quit = true; }

// GUI state of the standalone (display theme + per-theme Contrast level): $XDG_CONFIG_HOME/eiii/gui.state, else
// ~/.config/eiii/gui.state.  Host UI only; read at start, written when it changes and at exit.
std::string gui_state_path(bool create_dir)
{
	std::string dir;
#ifdef _WIN32
	if (char const *x = getenv("APPDATA"); x && *x) dir = x;
	else return {};
#else
	if (char const *x = getenv("XDG_CONFIG_HOME"); x && *x) dir = x;
	else if (char const *h = getenv("HOME"); h && *h) dir = std::string(h) + "/.config";
	else return {};
#endif
	if (create_dir) eiii::create_native_directory(dir);
	dir += "/eiii";
	if (create_dir) eiii::create_native_directory(dir);
	return dir + "/gui.state";
}
void load_gui_state(eiii::panel_adapter &ui)
{
	std::ifstream in(gui_state_path(false));
	for (std::string ln; std::getline(in, ln); ) ui.apply_display_state_line(ln.c_str());
}
void save_gui_state(std::string const &state)
{
	std::string const p = gui_state_path(true);
	if (p.empty()) return;
	std::ofstream out(p + ".tmp");
	out << state;
	out.close();
	#ifdef _WIN32
	if (out) MoveFileExA((p + ".tmp").c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
	if (out) std::rename((p + ".tmp").c_str(), p.c_str());
#endif
}
}

int run_gui(gui_options const &o)
{
	s_quit = false;
	eiii::engine_config ec;
	ec.rt.rom_only = o.rom_only;
	ec.rt.rom_dir = o.rom_dir; ec.rt.hd_path = o.hd; ec.rt.cd_path = o.cd;
	ec.rt.cem_model = o.cem_model; ec.rt.cem_calib = o.cem_calib;
	ec.rt.sample_rate = 48000.0;
	ec.max_block = o.period;
	ec.realtime = true;
	eiii::engine eng;
	eng.start(ec);
	eiii::panel_adapter ui(eng);
	load_gui_state(ui);
	std::string saved_state = ui.display_state();

	// audio + MIDI thread: pulls the engine at the device's pace (or the wall clock without a device)
	std::thread audio([&] {
		midi_input mi;
		if (!o.midi_in.empty() && !mi.open(o.midi_in)) fprintf(stderr, "GUI: cannot open MIDI input %s\n", o.midi_in.c_str());
		audio_output ao;
		if (!o.audio_out.empty() && !ao.open(o.audio_out, 48000, o.period, o.periods)) fprintf(stderr, "GUI: cannot open audio output %s\n", o.audio_out.c_str());
		if (ao.is_open()) fprintf(stderr, "audio: %s 48000 Hz, period %u, buffer %u frames; engine latency %u frames\n", o.audio_out.c_str(), ao.period(), ao.buffer(), eng.latency());
		unsigned const n = ao.is_open() ? ao.period() : o.period;
		std::vector<float> l(n), r(n), lr(2 * n);
		std::vector<uint8_t> bytes;
		auto next = std::chrono::steady_clock::now();
		while (!s_quit)
		{
			if (mi.is_open() && mi.read(bytes)) { for (uint8_t b : bytes) eng.midi(0, &b, 1); bytes.clear(); }
			eng.pull(l.data(), r.data(), n);
			if (ao.is_open())
			{
				for (unsigned i = 0; i < n; ++i) { lr[2 * i] = l[i]; lr[2 * i + 1] = r[i]; }
				ao.write(lr.data(), n);
			}
			else { next += std::chrono::nanoseconds(int64_t(n * 1e9 / 48000.0)); std::this_thread::sleep_until(next); }
		}
		if (ao.is_open()) fprintf(stderr, "audio: XRUNs %u\n", ao.xruns());
	});

	eiii::ui::panel_window win(ui);
	int w, h; eiii::ui::panel_view::preferred_size(o.scale, w, h);
	std::string err;
	if (!win.open(0, w, h, err)) { fprintf(stderr, "GUI: %s\n", err.c_str()); s_quit = true; audio.join(); return 3; }
	std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);

	// optional operator macro (--autoload-bank / --preset): presses panel switches through the adapter and waits for
	// the LCD text the firmware draws, like a person at the panel
	ui_macro macro;
	auto by_internal = [](std::string const &n) { for (size_t i = 0; i < eiii::panel_control_count; ++i) if (n == eiii::panel_controls[i].internal) return int(i); return -1; };
	macro.panel = [&](std::string const &n, bool down) { int const c = by_internal(n); if (down) ui.press_button(c); else ui.release_button(c); };
	macro.extra = macro.panel;
	macro.lcd = [&] { return eiii::lcd_line_text(ui.snapshot().lcd, 0) + "\n" + eiii::lcd_line_text(ui.snapshot().lcd, 1); };
	if (o.verbose) macro.m_log = [&](double t, std::string const &what) { fprintf(stderr, "UI %7.3f %s\n", t, what.c_str()); };
	std::string const ready_re = "^P\\d\\d .*\\nS\\d\\d ";
	if (o.autoload_bank >= 0 || o.preset >= 0) macro.wait(ready_re, 60.0, "main screen after boot");
	if (o.autoload_bank >= 0)
	{
		int const b = o.autoload_bank, d = o.drive;
		char bb[16]; snprintf(bb, sizeof bb, "B%02d ", b % 100);
		auto key = [](int k) { return "Keypad " + std::to_string(k); };
		macro.press("Load Bank"); macro.wait("LOAD BANK from", 10, "LOAD BANK");
		macro.extra_press("Enter"); macro.wait("Select a Drive", 10, "drive selection");
		macro.press(key(d)); macro.wait("\\nD" + std::to_string(d) + " ", 10, "drive");
		macro.extra_press("Enter"); macro.wait("Select a Bank", 30, "bank list");
		macro.press(key(b / 10 % 10)); macro.press(key(b % 10)); macro.wait(bb, 10, "bank");
		macro.extra_press("Enter"); macro.wait("Loading Bank", 10, "loading");
		macro.wait(ready_re, 120, "main screen with the bank");
	}
	if (o.preset >= 0)
	{
		char pp[16]; snprintf(pp, sizeof pp, "^P%02d ", o.preset % 100);
		macro.press("Keypad " + std::to_string(o.preset / 10 % 10)); macro.press("Keypad " + std::to_string(o.preset % 10));
		macro.wait(pp, 10, "preset");
	}
	bool macro_running = !macro.empty();

	auto last_frame = std::chrono::steady_clock::now(), last_state_check = last_frame;
	while (!s_quit && !win.close_requested())
	{
#ifndef _WIN32
		pollfd p{ win.fd(), POLLIN, 0 };
		poll(&p, 1, 8);
#else
		std::this_thread::sleep_for(std::chrono::milliseconds(8));
#endif
		auto const now = std::chrono::steady_clock::now();
#ifndef _WIN32
		if ((p.revents & POLLIN) || now - last_frame >= std::chrono::milliseconds(16))
#else
		if (now - last_frame >= std::chrono::milliseconds(16))
#endif
		{ win.tick(); last_frame = now; }
		if (now - last_state_check >= std::chrono::seconds(1))
		{
			last_state_check = now;
			if (std::string const st = ui.display_state(); st != saved_state) { save_gui_state(st); saved_state = st; }
		}
		double const t = ui.snapshot().emu_seconds;
		if (macro_running && !macro.tick(t))
		{
			macro_running = false;
			if (macro.failed()) fprintf(stderr, "AUTOLOAD FAILED: %s\n", macro.error().c_str());
			else fprintf(stderr, "AUTOLOAD done at %.3f s\n", t);
		}
		if (o.quit_after > 0 && t >= o.quit_after) break;
	}
	win.tick();
	if (std::string const st = ui.display_state(); st != saved_state) save_gui_state(st);
	if (!o.screenshot.empty()) { if (win.frame().write_png(o.screenshot)) fprintf(stderr, "GUI: wrote %s\n", o.screenshot.c_str()); }
	s_quit = true;
	audio.join();
	auto const st = eng.stats();
	fprintf(stderr, "GUI: %.1f s emulated, underruns %llu, inputs dropped %llu\n", st.emu_seconds, (unsigned long long)st.underruns, (unsigned long long)st.panel_dropped);
	win.close();
	eng.shutdown();
	return 0;
}
