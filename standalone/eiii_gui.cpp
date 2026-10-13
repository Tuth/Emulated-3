// eiii-standalone --gui — see eiii_gui.h.
#include "eiii_gui.h"
#include "gui_config.h"
#include "gui_menu.h"
#include <mutex>
#include <memory>
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

// GUI state of the standalone (display theme + per-theme Contrast level): $XDG_CONFIG_HOME/eiii/eiii-standalone-gui.state, else
// ~/.config/eiii/eiii-standalone-gui.state.  Host UI only; read at start, written when it changes and at exit.
std::string gui_state_path(bool create_dir)
{
	return standalone_storage_path("eiii-standalone-gui.state", create_dir);
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
	gui_options cfg = o;
	eiii::engine_config ec;
	ec.rt.rom_only = o.rom_only;
	ec.rt.rom_dir = o.rom_dir; ec.rt.hd_path = o.hd; ec.rt.cd_path = o.cd;
	ec.rt.cem_model = o.cem_model; ec.rt.cem_calib = o.cem_calib;
	ec.rt.sample_rate = 48000.0;
	ec.max_block = o.period;
	ec.realtime = true;
	if(!ec.rt.hd_path.empty())ec.rt.hd_changes[1]=standalone_read(standalone_hd_state(1));
	ec.rt.on_error=[](std::string const &message){auto p=standalone_storage_path("eiii-standalone-watchdog.log",true);std::ofstream out(p,std::ios::app);out<<message<<"\n";};
	eiii::engine eng;
	eng.start(ec);
	eiii::panel_adapter ui(eng);
	load_gui_state(ui);
	std::string saved_state = ui.display_state();

	struct io_state {std::mutex mutex;std::string audio,midi,error;unsigned requested=1,applied=0;};
	io_state io;io.audio=o.audio_out;io.midi=o.midi_in;
	auto request_io=[&](bool audio,std::string name){std::lock_guard<std::mutex> g(io.mutex);(audio?io.audio:io.midi)=std::move(name);++io.requested;};
	// Only this thread owns WinMM/ALSA handles. Failed device changes keep the previous device.
	std::thread audio([&] {
		std::unique_ptr<midi_input> mi;std::unique_ptr<audio_output> ao;
		std::string current_audio,current_midi;unsigned revision=0;
		unsigned const n=o.period;
		std::vector<float> l(n),r(n),lr(2*n);std::vector<uint8_t> bytes;
		auto next=std::chrono::steady_clock::now();
		while(!s_quit) {
			std::string wanted_audio,wanted_midi;unsigned wanted_revision;
			{std::lock_guard<std::mutex> g(io.mutex);wanted_revision=io.requested;wanted_audio=io.audio;wanted_midi=io.midi;}
			if(wanted_revision!=revision) {
				std::string error;
				if(wanted_audio!=current_audio) {
					if(wanted_audio.empty()){ao.reset();current_audio.clear();}
					else {auto candidate=std::make_unique<audio_output>();if(candidate->open(wanted_audio,48000,o.period,o.periods)){ao=std::move(candidate);current_audio=wanted_audio;}else error="Cannot open audio output; previous device retained";}
				}
				if(wanted_midi!=current_midi) {
					if(wanted_midi.empty()){mi.reset();current_midi.clear();}
					else {auto candidate=std::make_unique<midi_input>();if(candidate->open(wanted_midi)){mi=std::move(candidate);current_midi=wanted_midi;}else error="Cannot open MIDI input; previous device retained";}
				}
				revision=wanted_revision;
				{std::lock_guard<std::mutex> g(io.mutex);if(io.requested==revision){io.audio=current_audio;io.midi=current_midi;io.error=error;io.applied=revision;}}
				next=std::chrono::steady_clock::now();
			}
			if(mi && mi->read(bytes)){for(uint8_t b:bytes)eng.midi(0,&b,1);bytes.clear();}
			eng.pull(l.data(),r.data(),n);
			if(ao){for(unsigned i=0;i<n;++i){lr[2*i]=l[i];lr[2*i+1]=r[i];}ao->write(lr.data(),n);}
			else {next+=std::chrono::nanoseconds(int64_t(n*1e9/48000.0));std::this_thread::sleep_until(next);}
		}
	});

	eiii::ui::panel_window win(ui);
	auto save=[&]{cfg.view_width=win.width();if(!save_gui_defaults(cfg))win.view().set_menu_status("Cannot save standalone settings");};
	standalone_menus(win,eng,cfg,request_io,save);
	unsigned applied_io=0;
	win.on_tick=[&]{
		eiii::media_result result;
		while(eng.poll_media_result(result)) {
			if(!result.ok){win.view().set_menu_status(result.error);continue;}
			if(result.slot==eiii::media_slot::rom)cfg.rom_dir=result.path;
			else if(result.slot==eiii::media_slot::cem_model)cfg.cem_model=result.path;
			else if(result.slot==eiii::media_slot::scsi && result.scsi_id==1){cfg.hd=result.path;cfg.rom_only=false;}
			else if(result.slot==eiii::media_slot::scsi && result.scsi_id==3)cfg.cd=result.path;
			win.view().set_menu_status(result.restarted?"Restarted":"");save();
		}
		std::string error;bool changed=false;
		{std::lock_guard<std::mutex> g(io.mutex);if(io.applied!=applied_io){applied_io=io.applied;cfg.audio_out=io.audio;cfg.midi_in=io.midi;error=io.error;changed=true;}}
		if(changed){win.view().set_menu_status(error);save();}
		auto warning=eng.take_warning();if(!warning.empty())win.view().set_menu_status(warning);
	};
	win.on_resize_finish=save;
	int w,h;eiii::ui::panel_view::preferred_size(o.scale,w,h,true);
	if(o.view_width){w=o.view_width;h=int(std::lround(w*564.0/1472));}
	std::string err;
	if(!win.open(0,w,h,err)){fprintf(stderr,"GUI: %s\n",err.c_str());s_quit=true;audio.join();return 3;}
	uint64_t saved_hd_serial=~uint64_t(0);
	auto save_disks=[&]{auto serial=eng.hd_changes_serial();if(serial==saved_hd_serial)return;for(unsigned id=1;id<8;++id){auto state=eng.hd_changes(id);if(!standalone_write(standalone_hd_state(id),state))win.view().set_menu_status("Cannot save standalone disk changes");}saved_hd_serial=serial;};
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
			save_disks();
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
	save();
	win.close();
	eng.shutdown();
	saved_hd_serial=~uint64_t(0);save_disks();
	return 0;
}
