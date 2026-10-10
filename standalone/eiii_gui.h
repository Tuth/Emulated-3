// eiii-standalone --gui: the rack front panel in an X11 window, driving the same engine + PanelUI adapter as the
// CLAP plugin.  Audio (ALSA) and MIDI IN run on their own thread; the window on the main thread.
#pragma once
#include <string>

struct gui_options
{
	std::string rom_dir, hd, cd, cem_model = "v1", cem_calib, midi_in, audio_out;
	unsigned period = 256, periods = 4;
	int autoload_bank = -1, drive = 3, preset = -1;
	double scale = 100.0;           // percent of 1472 x 538
	double quit_after = 0;          // emulated seconds (tests); 0 = until the window is closed
	std::string screenshot;         // PNG of the window contents at exit (tests)
	bool verbose = false;
};

int run_gui(gui_options const &o);
