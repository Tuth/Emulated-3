// Hard disk changes: what the original OS writes to the (read-only) HD image is kept as a change set that survives
// restarts and power cycles, can be taken out (the CLAP plugin keeps it in the project), restored into a new instance,
// written into a new image file, and discarded.  Only through the panel: sample a 1 kHz tone, place it, Save Bank to
// D1 B00, then Load Bank D1 B00 in each situation and check the bank, its sample and that it plays.
#include "panel_rig.h"
#include "runtime/eiii_runtime.h"
#include <unistd.h>

using namespace eiii_test;
static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); ++fails; } printf("\n"); fflush(stdout); } while (0)

static bool media_done(rig &t, eiii::media_result &out, double timeout = 30.0) { return t.wait([&] { return t.e.poll_media_result(out); }, timeout); }

// the D1 bank list (Load Bank -> ... -> D1), whatever state the module opens in
static std::string d1_bank0(rig &t)
{
	t.press("load_bank");
	if (!t.wait_row(0, "   LOAD BANK from", 3.0)) return "?";
	t.pump(0.3);
	t.wait([&] { return t.row(3).find("Wait") == std::string::npos; }, 10.0);
	t.pump(0.5);
	if (t.lcd().find("Select a Bank") != std::string::npos && t.row(1).compare(0, 3, "D1 ") != 0) t.press("cursor_up");
	if (t.lcd().find("Select a Bank") == std::string::npos)
	{
		if (t.lcd().find("Select a Drive") == std::string::npos) { t.press("enter"); t.wait_text("Select a Drive", 3.0); }
		t.press("keypad_1"); t.wait_row(1, "D1 ", 3.0);
		t.press("enter");
		if (!t.wait_text("Select a Bank", 10.0)) return "?";
	}
	t.press("keypad_0"); t.press("keypad_0"); t.pump(0.3);
	return t.row(2);
}
static void leave_load_bank(rig &t) { t.press("load_bank"); t.wait([&] { return t.row(1).compare(0, 4, "S00 ") == 0; }, 3.0); t.pump(0.3); }

static uint32_t sample1_rate(rig &t)
{
	uint32_t rate = 0;
	t.e.inspect([&rate](eiii::runtime &rt) { uint32_t const p = rt.debug_read32(0x1A1B4 + 4); if (p >= 0x400000 && p < 0xC00000) rate = rt.debug_read32(p + 0x34); });
	eiii::media_result r; media_done(t, r, 5.0);
	return rate;
}

// Load Bank D1 B00; true when it is the saved bank with its sample (S01 at 44 053 Hz) and G2 sounds (channel 14)
static bool saved_bank_works(rig &t, char const *where)
{
	std::string const b = d1_bank0(t);
	if (b.compare(0, 17, "B00 Untitled Bank") != 0) { printf("  %s: D1 B00 is [%s]\n", where, b.c_str()); leave_load_bank(t); return false; }
	t.press("enter");
	bool const loaded = t.wait_text("Loading Bank", 3.0) && t.wait([&] { return t.row(0).compare(0, 4, "P00 ") == 0 && t.row(1).compare(0, 4, "S00 ") == 0; }, 30.0);
	t.pump(0.5);
	uint32_t const rate = sample1_rate(t);
	// a loaded bank receives on its own MIDI channel: 14, as the factory banks (the power-on state used channel 1)
	t.midi({ 0x9D, 43, 100 });
	double const rms = t.rms_over(0.4);
	t.midi({ 0x8D, 43, 64 });
	t.pump(2.0);
	if (!(loaded && rate == 44053 && rms > 0.02)) printf("  %s: loaded %d, S01 rate %u, G2 rms %.4f\n", where, loaded, rate, rms);
	return loaded && rate == 44053 && rms > 0.02;
}

int main()
{
	auto cfg = media_config();
	if (cfg.rt.hd_path.empty()) { printf("SKIP: set EIII_ROM_DIR, EIII_HD\n"); return 77; }
	std::string hd2 = env("EIII_HD2");
	if (hd2.empty()) { auto const sl = cfg.rt.hd_path.rfind('/'); hd2 = cfg.rt.hd_path.substr(0, sl + 1) + "EIIIOS2_42_HDImage_239MB.ISO"; }
	eiii::media_result r;
	std::string blob;
	{
		rig t(cfg);
		double tone_on = 1e30;
		t.input = [&](uint64_t f0, float *l, float *rr, size_t n) {
			for (size_t k = 0; k < n; ++k) { double const s = double(f0 + k) / 48000.0; l[k] = rr[k] = s >= tone_on ? float(0.5 * std::sin(2 * M_PI * 1000.0 * (s - tone_on))) : 0.f; }
		};
		CHECK(t.wait_row(0, "P00 ", 25.0), "power-on");
		t.pump(1.0);
		CHECK(t.e.hd_changes(1).empty(), "a fresh image has no changes");
		// sample, place, save
		t.press("sample"); t.wait_row(1, "S01 ", 3.0);
		t.press("keypad_7"); t.wait_text("Sampling Armed...", 5.0);
		tone_on = double(t.frames_pulled) / 48000.0 + 0.2;
		t.wait_text("Sampling...", 5.0); t.pump(1.0);
		t.press("enter"); t.wait_row(1, "S01 Untitled Sample", 10.0);
		tone_on = 1e30;
		t.press("keypad_6"); t.wait_text("PLACE SAMPLE", 3.0);
		for (char const *prompt : { "Select pri/sec", "Select Original Key", "Select Low Key", "Select High Key", "Select a Submodule" }) { t.press("enter"); t.wait_text(prompt, 3.0); }
		t.press("sample"); t.wait_row(0, "P00 ", 3.0);
		t.pump(0.5);
		t.midi({ 0x90, 43, 100 }); double const rms0 = t.rms_over(0.4); t.midi({ 0x80, 43, 64 }); t.pump(2.0);
		CHECK(rms0 > 0.02, "placed sample plays on G2 before saving (rms %.3f)", rms0);
		uint64_t const serial0 = t.e.hd_changes_serial();
		t.press("save_bank");
		bool const prompt = t.wait_text("SAVE BANK into", 3.0) && t.wait_row(1, "D1 ", 3.0) && t.wait_row(2, "B00 ", 3.0);
		t.press("enter");
		bool const saved = t.wait_text("Saving Bank", 3.0) && t.wait_row(0, "P00 ", 20.0);
		CHECK(prompt && saved, "Save Bank into D1 B00: [%s]", t.row(0).c_str());
		// published once the OS has stopped writing (the engine waits for a pause in the writes)
		bool const pub = t.wait([&] { return t.e.hd_changes_serial() != serial0 && !t.e.hd_changes(1).empty(); }, 10.0);
		blob = t.e.hd_changes(1);
		CHECK(pub && blob.compare(0, 8, "EIIIHDC1") == 0, "change set published: %zu bytes, %zu sectors", blob.size(), blob.size() > 28 ? (blob.size() - 28) / 516 : 0);
		CHECK(saved_bank_works(t, "same run"), "Load Bank D1 B00 in the same run: the saved bank with its sample");

		t.e.set_media(eiii::media_slot::restart, "");
		CHECK(media_done(t, r) && r.ok && r.restarted && t.wait_row(0, "P00 ", 25.0), "restart (cold boot)");
		CHECK(saved_bank_works(t, "after restart"), "after a restart the saved bank is still on D1");
		t.e.power(false); t.pump(0.5); t.e.power(true);
		CHECK(t.wait([&] { return t.now() < 5.0; }, 5.0) && t.wait_row(0, "P00 ", 25.0), "power off / on");
		CHECK(saved_bank_works(t, "after power cycle"), "after a power cycle the saved bank is still on D1");

		t.e.set_hd_changes(1, "");
		CHECK(media_done(t, r) && r.ok && r.restarted && t.wait_row(0, "P00 ", 25.0), "changes discarded (restart)");
		std::string const b = d1_bank0(t);
		t.wait_text("No Banks On Drive!", 10.0);
		bool const none = t.lcd().find("No Banks On Drive!") != std::string::npos;
		if (!none) printf("  after discard: [%s] [%s] [%s] [%s] (%s)\n", t.row(0).c_str(), t.row(1).c_str(), t.row(2).c_str(), t.row(3).c_str(), b.c_str());
		leave_load_bank(t);
		CHECK(none && t.e.hd_changes(1).empty(), "after discarding, D1 is the image's own again: no banks on drive");
		t.e.set_hd_changes(1, blob);
		CHECK(media_done(t, r) && r.ok && r.restarted && t.wait_row(0, "P00 ", 25.0), "change set restored into the running unit");
		CHECK(saved_bank_works(t, "restored"), "restored: the saved bank is back");
	}
	{
		auto c2 = cfg; c2.rt.hd_changes[1] = blob;
		rig t(c2);
		CHECK(t.wait_row(0, "P00 ", 25.0) && t.e.take_warning().empty(), "new instance started with the change set (as from a project)");
		CHECK(saved_bank_works(t, "new instance"), "new instance: the saved bank is on D1");
	}
	if (access(hd2.c_str(), R_OK) == 0)
	{
		auto c3 = cfg; c3.rt.hd_path = hd2; c3.rt.hd_changes[1] = blob;
		rig t(c3);
		bool const up = t.wait_row(0, "P00 ", 25.0);
		std::string const w = t.e.take_warning();
		CHECK(up && w.find("different disk image") != std::string::npos, "a change set on another image is refused, the unit boots: %s", w.c_str());
	}
	{
		char tmpl[] = "/tmp/eiii_hd_XXXXXX";
		int const fd = mkstemp(tmpl); close(fd);
		std::string err;
		bool const wrote = eiii::runtime::write_image_with_changes(cfg.rt.hd_path, blob, tmpl, err);
		CHECK(wrote, "image with the changes written to a new file %s", err.c_str());
		std::string e2;
		CHECK(!eiii::runtime::write_image_with_changes(cfg.rt.hd_path, blob, cfg.rt.hd_path, e2), "never over the original image: %s", e2.c_str());
		if (wrote)
		{
			auto c4 = cfg; c4.rt.hd_path = tmpl;
			rig t(c4);
			CHECK(t.wait_row(0, "P00 ", 25.0), "boot from the new image");
			CHECK(saved_bank_works(t, "new image"), "the new image has the saved bank");
		}
		unlink(tmpl);
	}
	printf("%s: %d failure(s)\n", fails ? "FAIL" : "OK", fails);
	return fails ? 1 : 0;
}
