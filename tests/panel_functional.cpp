// Functional panel test (campaign Phase 17 + Phase 8 LED tests): from power-on, operate the EIII ONLY through the
// PanelUI adapter (press/release, sliders, read LCD/LEDs) and MIDI IN, exactly as a user at the rack would:
// Load Bank, Enter, 3, Enter, 44, Enter, 01, play a note, Master Volume, Master, Master Tune + Data slider,
// Analog Processing, Supermode, Mode, Transpose, Record/Play/Stop/Erase — asserting the LCD rows and the LEDs.
// Media: EIII_ROM_DIR, EIII_HD (OS 2.42 image), EIII_CD (factory CD with bank 44).
#include "panel_rig.h"

using namespace eiii_test;
static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); ++fails; } printf("\n"); fflush(stdout); } while (0)

int main()
{
	auto cfg = media_config();
	if (cfg.rt.hd_path.empty() || cfg.rt.cd_path.empty()) { printf("SKIP: set EIII_ROM_DIR, EIII_HD, EIII_CD\n"); return 77; }
	rig t(cfg);

	// ---- power-on ----
	CHECK(t.wait_row(0, "P00 ", 20.0) && t.wait_row(1, "S00 ", 1.0), "power-on main screen at %.2f s: [%s] [%s]", t.now(), t.row(0).c_str(), t.row(1).c_str());
	t.pump(0.5);
	CHECK(t.leds() == "Song", "power-on LEDs: {%s} (only Mode: Song)", t.leds().c_str());

	// ---- Load Bank, Enter, 3, Enter, 44, Enter ----
	t.press("load_bank");
	CHECK(t.wait_row(0, "   LOAD BANK from", 3.0), "Load Bank -> row 1 [%s]", t.row(0).c_str());
	{
		// the OS waits for Enter: the Enter LED blinks
		bool on = false, off = false;
		for (int i = 0; i < 60; ++i) { t.pump(0.025); (t.led("led_enter") ? on : off) = true; }
		CHECK(on && off, "Enter LED blinks while the OS waits for Enter (on %d, off %d)", on, off);
	}
	t.press("enter");
	CHECK(t.wait_text("Select a Drive", 3.0), "Enter -> row 4 [%s]", t.row(3).c_str());
	t.press("keypad_3");
	CHECK(t.wait_row(1, "D3 ", 3.0), "3 -> row 2 [%s]", t.row(1).c_str());
	t.press("enter");
	CHECK(t.wait_text("Select a Bank", 10.0), "Enter -> bank list, row 4 [%s]", t.row(3).c_str());
	t.press("keypad_4"); t.press("keypad_4");
	CHECK(t.wait_row(2, "B44 Tenor Sax", 3.0), "44 -> row 3 [%s] (bank on line three)", t.row(2).c_str());
	t.press("enter");
	CHECK(t.wait_text("Loading Bank", 3.0), "Enter -> [%s]", t.row(3).c_str());
	CHECK(t.wait_row(1, "S00 Scott's Place", 90.0), "bank loaded at %.1f s: [%s] [%s]", t.now(), t.row(0).c_str(), t.row(1).c_str());

	// ---- preset 01 ----
	t.press("keypad_0"); t.press("keypad_1");
	CHECK(t.wait_row(0, "P01 Crisp Sax", 3.0), "01 -> [%s]", t.row(0).c_str());

	// ---- play a note through MIDI IN (factory banks receive on channel 14) ----
	double const quiet = t.rms_over(0.3);
	t.midi({ 0x9d, 0x3c, 0x64 });
	t.pump(0.15);
	double const loud = t.rms_over(0.4);
	CHECK(loud > 20 * quiet + 1e-4, "MIDI note 60 ch14 audible: rms %.5f vs %.6f before", loud, quiet);
	// ---- Master Volume slider -> ADC X5 -> firmware master volume -> VCA ----
	int const vol = eiii::panel_find_control("master_volume");
	t.ui.set_slider(vol, 0.0);
	t.pump(0.3);
	double const soft = t.rms_over(0.3);
	CHECK(soft < loud * 0.1, "Master Volume to minimum during the held note: rms %.5f -> %.6f (%.1f dB)", loud, soft, 20 * std::log10(std::max(soft, 1e-9) / loud));
	t.ui.set_slider(vol, eiii::adc_to_slider(eiii::slider_default_adc(vol)));
	t.pump(0.3);
	double const back = t.rms_over(0.3);
	CHECK(back > soft * 4, "Master Volume restored: rms %.5f", back);
	t.midi({ 0x8d, 0x3c, 0x40 });
	t.pump(1.0);

	// ---- Master module, Master Tune + Data slider ----
	t.press("master");
	CHECK(t.wait_row(0, "       MASTER", 3.0), "MASTER -> [%s] [%s]", t.row(0).c_str(), t.row(3).c_str());
	CHECK(t.led("led_master") && t.led("led_mode_song"), "LEDs after MASTER: {%s}", t.leds().c_str());
	t.press("keypad_1");
	CHECK(t.wait_row(0, "    MASTER TUNE", 3.0), "1 -> [%s] [%s]", t.row(0).c_str(), t.row(2).c_str());
	std::string const before = t.row(2);
	int const data = eiii::panel_find_control("data_slider");
	for (int i = 1; i <= 10; ++i) { t.ui.set_slider(data, i * 0.05); t.pump(0.03); }
	t.pump(0.5);
	CHECK(t.row(2) != before && t.row(2).find("Offset:") != std::string::npos, "Data slider moves the tuning offset: [%s] -> [%s]", before.c_str(), t.row(2).c_str());
	std::string const mid = t.row(2);
	for (int i = 11; i <= 20; ++i) { t.ui.set_slider(data, i * 0.05); t.pump(0.03); }
	t.pump(0.5);
	std::string const top = t.row(2);
	for (int i = 19; i >= 0; --i) { t.ui.set_slider(data, i * 0.05); t.pump(0.03); }
	t.pump(0.5);
	CHECK(top.find("+100.0") != std::string::npos && t.row(2).find("-100.0") != std::string::npos,
		"Data slider is absolute: middle [%s], top [%s], bottom [%s]", mid.c_str(), top.c_str(), t.row(2).c_str());
	for (int i = 1; i <= 10; ++i) { t.ui.set_slider(data, i * 0.05); t.pump(0.03); }   // back to the middle (about 0 cents)
	t.pump(0.3);
	t.press("enter");
	t.pump(0.5);

	// ---- Analog Processing ----
	t.press("analog_processing");
	CHECK(t.wait_text("ANALOG PROCESSING", 3.0), "Analog Processing -> [%s]", t.row(0).c_str());
	CHECK(t.led("led_analog_processing") && !t.led("led_master"), "module LEDs: {%s}", t.leds().c_str());
	// ---- every module button lights its own LED and only it (module table 0x105BF) ----
	struct mod { char const *button, *led, *title; };
	mod const mods[] = { { "sample", "led_sample", "SAMPLE" }, { "preset_management", "led_preset_management", "PRESET MANAGEMENT" },
		{ "digital_processing", "led_digital_processing", "DIGITAL" }, { "preset_definition", "led_preset_definition", "PRESET DEFINITION" },
		{ "seq_management", "led_seq_management", "SEQUENCER MANAGE" }, { "seq_setup", "led_seq_setup", "SEQUENCER SETUP" },
		{ "seq_edit", "led_seq_edit", "SEQUENCER EDIT" }, { "master", "led_master", "MASTER" } };
	char const *const module_leds[] = { "led_sample", "led_master", "led_seq_management", "led_seq_setup", "led_seq_edit", "led_preset_management",
		"led_digital_processing", "led_analog_processing", "led_preset_definition" };
	for (mod const &m : mods)
	{
		t.press(m.button);
		bool const shown = t.wait_text(m.title, 3.0);
		t.pump(0.3);
		int lit = 0; for (char const *l : module_leds) lit += t.led(l);
		CHECK(shown && t.led(m.led) && lit == 1, "%s -> [%s], its LED lit and %d module LED(s) lit", m.button, t.row(0).c_str(), lit);
	}
	t.press("master");                 // leave the module (module buttons toggle)
	CHECK(t.wait_row(0, "P01 ", 3.0), "module left: main screen [%s] [%s] [%s] [%s]", t.row(0).c_str(), t.row(1).c_str(), t.row(2).c_str(), t.row(3).c_str());
	t.pump(0.5);

	// ---- Supermode toggles its LED ----
	bool const sm0 = t.led("led_supermode");
	t.press("supermode"); t.pump(0.3);
	bool const sm1 = t.led("led_supermode");
	t.press("supermode"); t.pump(0.3);
	CHECK(!sm0 && sm1 && !t.led("led_supermode"), "Supermode LED off -> on -> off");

	// ---- Mode: Song <-> Segment/Cue List (visiting Sequencer Edit above may already have selected Segment) ----
	if (t.led("led_mode_segment")) { t.press("seq_mode"); t.pump(0.3); }
	bool const song = t.led("led_mode_song") && !t.led("led_mode_segment");
	std::string const songrow = t.row(1);
	t.press("seq_mode"); t.pump(0.3);
	bool const seg = t.led("led_mode_segment") && !t.led("led_mode_song");
	std::string const segrow = t.row(1);
	t.press("seq_mode"); t.pump(0.3);
	CHECK(song && seg && t.led("led_mode_song") && !t.led("led_mode_segment"), "Mode toggles Song [%s] -> Segment/Cue List [%s] -> Song", songrow.c_str(), segrow.c_str());

	// ---- Play / Stop ----
	t.press("play"); t.pump(0.5);
	bool const playing = t.led("led_play");
	std::string const pos = t.row(2);
	t.press("stop"); t.pump(0.3);
	CHECK(playing && !t.led("led_play"), "Play LED while playing [%s], off after Stop", pos.c_str());

	// ---- Transpose (held): screen and LED while held, as on the rack ----
	int const tr = eiii::panel_find_control("transpose");
	t.ui.press_button(tr);
	bool const tr_shown = t.wait_text("TRANSPOSE", 1.0);
	int lit = 0, dark = 0;
	for (int i = 0; i < 40; ++i) { t.pump(0.025); (t.led("led_transpose") ? lit : dark)++; }
	std::string const trlcd = t.row(0) + "|" + t.row(3);
	t.ui.release_button(tr);
	t.pump(0.4);
	CHECK(tr_shown && lit > 0, "Transpose held: [%s], LED lit in %d/40 samples (dark %d); released: [%s] LED %d", trlcd.c_str(), lit, dark, t.row(0).c_str(), t.led("led_transpose"));

	// ---- Record (Insert) arms segment recording; Erase LED (misc latch bit 7) while Erase is held during it ----
	t.press("seq_mode"); t.pump(0.3);
	t.press("record"); t.pump(0.3);
	bool const rec = t.led("led_record");
	std::string const recrow = t.row(3);
	t.press("play"); t.pump(1.5);
	CHECK(rec, "Record LED lit in segment mode [%s]; LEDs {%s}", recrow.c_str(), t.leds().c_str());
	int const er = eiii::panel_find_control("erase");
	t.ui.press_button(er);
	bool const erase_lit = t.wait([&] { return t.led("led_erase"); }, 1.5);
	t.ui.release_button(er);
	t.press("stop"); t.pump(0.5);
	CHECK(erase_lit && !t.led("led_erase") && !t.led("led_record") && !t.led("led_play"), "Erase LED lit while Erase is held during segment record; Erase/Record/Play off after Stop {%s}", t.leds().c_str());
	t.press("seq_mode"); t.pump(0.3);
	CHECK(t.led("led_mode_song"), "back to Song mode");

	auto st = t.e.stats();
	printf("engine: %.1f s emulated, %llu frames, underruns %llu, inputs dropped %llu/%llu\n", st.emu_seconds,
		(unsigned long long)st.frames_out, (unsigned long long)st.underruns, (unsigned long long)st.panel_dropped, (unsigned long long)st.midi_dropped);
	CHECK(st.panel_dropped == 0 && st.midi_dropped == 0, "no input dropped");
	printf("%s: %d failure(s)\n", fails ? "FAIL" : "OK", fails);
	return fails ? 1 : 0;
}
