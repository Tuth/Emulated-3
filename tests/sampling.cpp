// Sampling through the original OS with host audio on the ADC (eiii::engine::input), panel operations only:
//   SAMPLE -> 7 (Arm Sampling) -> a 1 kHz tone starts (L 0.5, R 0.25 of full scale) -> the OS's threshold trigger
//   starts recording -> Enter stops -> the new sample S01.  Checked in sample RAM: rate 44 053 Hz, stereo, the
//   tone's period (44.05 words per cycle at 44 053 Hz), level (unity at the Sample Input slider's default),
//   continuity (no gaps or repeated blocks); then the Sample Input slider's level law and a MIDI note playing it.
#include "panel_rig.h"
#include "runtime/eiii_runtime.h"

using namespace eiii_test;
static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); ++fails; } printf("\n"); fflush(stdout); } while (0)

struct sample_info { uint32_t ptr = 0, rate = 0; uint8_t flags = 0; std::string name; std::vector<int16_t> left; };

// read sample n's header and its first `words` left-channel words from sample RAM (engine thread)
static void dump(rig &t, char const *what) { if (getenv("SAMPLING_TRACE")) printf("  %s @%.2f: [%s] [%s] [%s] [%s]\n", what, t.now(), t.row(0).c_str(), t.row(1).c_str(), t.row(2).c_str(), t.row(3).c_str()); }
static bool read_sample(rig &t, int n, size_t words, sample_info &out)
{
	t.e.inspect([&out, n, words](eiii::runtime &rt) {
		out.ptr = rt.debug_read32(0x1A1B4 + 4 * uint32_t(n));
		if (out.ptr < 0x400000 || out.ptr >= 0xC00000) return;
		for (int i = 0; i < 16; i += 2) { uint16_t const w = rt.debug_read16(out.ptr + uint32_t(i)); out.name += char(w & 0xff); out.name += char(w >> 8); }
		out.rate = rt.debug_read32(out.ptr + 0x34);
		out.flags = uint8_t(rt.debug_read16(out.ptr + 0x3A) & 0xff);
		for (size_t i = 0; i < words; ++i) out.left.push_back(int16_t(rt.debug_read16(out.ptr + 0x5C + uint32_t(2 * i))));
	});
	eiii::media_result r;
	return t.wait([&] { return t.e.poll_media_result(r); }, 5.0) && r.ok && r.slot == eiii::media_slot::inspect;
}

int main()
{
	auto cfg = media_config();
	if (cfg.rt.hd_path.empty()) { printf("SKIP: set EIII_ROM_DIR, EIII_HD\n"); return 77; }
	rig t(cfg);
	double tone_on = 1e30, amp = 0.5;
	t.input = [&](uint64_t f0, float *l, float *r, size_t n) {
		for (size_t k = 0; k < n; ++k)
		{
			double const s = double(f0 + k) / 48000.0;
			float const v = s >= tone_on ? float(amp * std::sin(2 * M_PI * 1000.0 * (s - tone_on))) : 0.f;
			l[k] = v; r[k] = 0.5f * v;
		}
	};
	CHECK(t.wait_row(0, "P00 ", 25.0), "power-on main screen: [%s]", t.row(0).c_str());
	t.pump(1.0);

	// audio on the input while not sampling must not disturb the panel (the OS's pot scan reads the same converter):
	// Load Bank -> Enter -> 3 -> Enter still lists the CD's banks with a loud tone present
	tone_on = 0;
	t.press("load_bank"); t.wait_row(0, "   LOAD BANK from", 3.0); t.pump(0.3);
	t.press("enter"); t.wait_text("Select a Drive", 3.0);
	t.press("keypad_3"); t.pump(0.5);
	// With no CD inserted, the original OS falls back to the internal HD.
	bool const drive = t.row(1).compare(0, 3, cfg.rt.cd_path.empty() ? "D1 " : "D3 ") == 0;
	t.press("enter");
	bool const listed = !cfg.rt.cd_path.empty() ? t.wait_text("Select a Bank", 10.0) : true;
	CHECK(drive && listed, "with a loud tone on the input the panel works normally: [%s] [%s]", t.row(1).c_str(), t.row(3).c_str());
	t.press("load_bank"); t.wait_row(0, "P00 ", 3.0);
	tone_on = 1e30; t.pump(0.5);

	t.press("sample");
	CHECK(t.wait_text("SAMPLE", 3.0) && t.wait_row(1, "S01 Empty Sample", 3.0), "SAMPLE module: [%s]", t.row(1).c_str());
	t.press("keypad_7");
	CHECK(t.wait_text("Sampling Armed...", 5.0), "7 -> [%s]", t.row(0).c_str());
	t.pump(1.0);
	CHECK(t.lcd().find("Sampling Armed...") != std::string::npos, "silence does not trigger");
	tone_on = double(t.frames_pulled) / 48000.0 + 0.2;
	CHECK(t.wait_text("Sampling...", 5.0), "the tone triggers recording: [%s]", t.row(0).c_str());
	t.pump(3.0);
	t.press("enter");
	CHECK(t.wait_row(1, "S01 Untitled Sample", 10.0), "Enter stops; new sample: [%s]", t.row(1).c_str());

	sample_info s;
	CHECK(read_sample(t, 1, 44053, s) && s.name.compare(0, 15, "Untitled Sample") == 0, "S01 in sample RAM at %06X: \"%s\"", s.ptr, s.name.c_str());
	CHECK(s.rate == 44053, "rate %u Hz (10 MHz / 227)", s.rate);
	CHECK((s.flags & 0x60) == 0x60, "stereo (flags 0x%02X)", s.flags);
	// tone analysis on one second of the left channel: rising zero crossings -> period; peak; continuity
	std::vector<double> cross;
	int peak = 0;
	double max_step = 0;
	for (size_t i = 1; i < s.left.size(); ++i)
	{
		if (s.left[i - 1] < 0 && s.left[i] >= 0) cross.push_back(double(i - 1) + double(-s.left[i - 1]) / double(s.left[i] - s.left[i - 1]));
		peak = std::max(peak, std::abs(int(s.left[i])));
		max_step = std::max(max_step, std::fabs(double(s.left[i]) - s.left[i - 1]));
	}
	double const period = cross.size() > 10 ? (cross.back() - cross[1]) / double(cross.size() - 2) : 0;
	CHECK(std::fabs(period - 44.053) < 0.02, "1 kHz tone: %.3f words per cycle over %zu cycles (expect 44.053)", period, cross.size() - 2);
	CHECK(peak > 15800 && peak < 16800, "level at the default Sample Input position: peak %d (0.5 FS -> ~16384)", peak);
	// a sine of amplitude 16384 at 1000/44053 cycles per word changes by at most 2*pi*16384/44.05 = 2337 per word
	CHECK(max_step < 2450, "continuous: largest step between words %.0f", max_step);

	// Sample Input level: slider at the top = +20 dB (the 0.5 FS tone clips), at the bottom = silent (never triggers)
	int const slider = eiii::panel_find_control("sample_input");
	tone_on = 1e30; t.pump(0.3);
	t.ui.set_slider(slider, 1.0); t.pump(0.3);
	dump(t, "before 2nd arm");
	t.press("keypad_7");
	dump(t, "7");
	bool const armed2 = t.wait_text("Sampling Armed...", 5.0);
	tone_on = double(t.frames_pulled) / 48000.0 + 0.2;
	bool const rec2 = t.wait_text("Sampling...", 5.0);
	dump(t, "tone");
	t.pump(1.0); t.press("enter");
	dump(t, "enter");
	// the OS records into the selected sample: the new take replaces S01
	// a clipped take: the OS reports it ("Sample Overload!") and waits for Enter
	bool const overload = t.wait_text("Sample Overload!", 5.0);
	if (overload) t.press("enter");
	bool const s02 = overload && t.wait_row(1, "S01 Untitled Sample", 10.0);
	sample_info s2;
	read_sample(t, 1, 4405, s2);
	int peak2 = 0, clipped = 0;
	for (int16_t v : s2.left) { peak2 = std::max(peak2, std::abs(int(v))); clipped += v == 32767 || v == -32768; }
	if (!(armed2 && rec2 && s02)) printf("  armed %d recording %d stopped %d\n", armed2, rec2, s02);
	CHECK(armed2 && rec2 && s02 && peak2 >= 32767 && clipped > 100, "Sample Input at the top: +20 dB, the take (S01 again) clips, OS shows \"Sample Overload!\" (peak %d, %d clipped words)", peak2, clipped);
	tone_on = 1e30; t.pump(0.3);
	t.ui.set_slider(slider, 0.0); t.pump(0.3);
	t.press("keypad_7");
	dump(t, "3rd 7");
	bool const armed3 = t.wait_text("Sampling Armed...", 5.0);
	tone_on = double(t.frames_pulled) / 48000.0 + 0.2;
	t.pump(2.0);
	CHECK(armed3 && t.lcd().find("Sampling Armed...") != std::string::npos, "Sample Input at the bottom: silent, the tone does not trigger");
	tone_on = 1e30;                                      // input silent again (the Data slider is read through
	t.press("enter");                                    //   the same ADC while the SAMPLE module keeps it in audio mode)
	t.pump(1.0);
	dump(t, "abort");
	t.ui.set_slider(slider, 0.5); t.pump(0.3);

	// playback: Place Sample (SAMPLE 6) with the defaults the OS offers: primary, original key G2, range C2..B2;
	// G2 (MIDI 43) must play the tone at its recorded pitch (1 kHz), C2 (MIDI 36) seven semitones lower, C4 nothing
	t.press("keypad_6");
	bool placed = t.wait_text("PLACE SAMPLE", 3.0);
	for (char const *prompt : { "Select pri/sec", "Select Original Key", "Select Low Key", "Select High Key" })
	{ t.press("enter"); placed = placed && t.wait_text(prompt, 3.0); dump(t, prompt); }
	t.press("enter");
	placed = placed && t.wait_text("Select a Submodule", 3.0);
	CHECK(placed, "Place Sample S01: primary, original key G2, C2..B2");
	t.press("sample");
	t.wait_row(0, "P00 ", 3.0);
	t.pump(0.5);
	std::vector<float> cap;
	t.on_block = [&] { cap.insert(cap.end(), t.l.begin(), t.l.begin() + long(t.block)); };
	// strongest frequency of the held note (Goertzel scan, 1 Hz steps around the expected pitch, 0.1 .. 0.5 s)
	auto note_pitch = [&](int key, double lo, double hi, double &rms) {
		cap.clear();
		t.midi({ 0x90, uint8_t(key), 100 });
		rms = t.rms_over(0.5);
		t.midi({ 0x80, uint8_t(key), 64 });
		t.pump(3.0);
		double best_f = 0, best_p = 0;
		size_t const a0 = 4800, n = std::min<size_t>(cap.size(), 24000) - a0;
		for (double f = lo; f <= hi; f += 1.0)
		{
			double const w = 2 * M_PI * f / 48000.0, c = 2 * std::cos(w);
			double s1 = 0, s2 = 0;
			for (size_t i = 0; i < n; ++i) { double const s0 = cap[a0 + i] + c * s1 - s2; s2 = s1; s1 = s0; }
			double const pw = s1 * s1 + s2 * s2 - c * s1 * s2;
			if (pw > best_p) { best_p = pw; best_f = f; }
		}
		return best_f;
	};
	double rms43 = 0, rms36 = 0, rms60 = 0;
	double const f43 = note_pitch(43, 900, 1100, rms43);
	double const f36 = note_pitch(36, 600, 740, rms36);
	note_pitch(60, 900, 1100, rms60);
	t.on_block = nullptr;
	CHECK(rms43 > 0.05 && std::fabs(f43 - 1000.0) <= 2.0, "MIDI 43 (G2, original key) plays the sample at %.0f Hz (rms %.3f)", f43, rms43);
	CHECK(rms36 > 0.05 && std::fabs(f36 - 1000.0 * std::pow(2.0, -7.0 / 12.0)) <= 2.0, "MIDI 36 (C2) plays it 7 semitones lower: %.0f Hz (expect 667)", f36);
	CHECK(rms60 < 0.001, "MIDI 60 (outside C2..B2) is silent (rms %.4f)", rms60);

	auto const st = t.e.stats();
	CHECK(st.panel_dropped == 0 && st.midi_dropped == 0, "no dropped inputs");
	printf("%s: %d failure(s)\n", fails ? "FAIL" : "OK", fails);
	return fails ? 1 : 0;
}
