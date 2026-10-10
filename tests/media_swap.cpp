// Runtime media changes (eiii::engine::set_media) seen by the original OS, through the panel only:
//   1. CD re-insert after boot (new medium: UNIT ATTENTION) -> Load Bank 44 from D3 still works
//   2. CD eject (NOT READY) -> Load Bank from D3 does not reach the bank list; the OS reports "SCSI Hardware Error!"
//   3. CD mount again -> Load Bank 44 works again
//   3b. a different disc (EIII_CD2, e.g. EIII.02 More Emulator Standards): Load Bank 01 from it, then back to the
//       first disc and Load Bank 44 again
//   4. a CD image that cannot be used is refused and the mounted disc stays
//   5. HD swap -> the machine restarts from power-on and boots the OS from the new image
// EIII_CD2 (optional): a second EIII CD with bank 01 "Arc&Spic Celli4M" (EIII.02); default ~/EIII_RE/media/cd/EIII.02*.iso
// EIII_HD2 (optional): a second OS hard disk image for step 5 (default: the 239 MB OS 2.42 image next to EIII_HD).
#include "panel_rig.h"
#include <sys/stat.h>

using namespace eiii_test;
static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); ++fails; } printf("\n"); fflush(stdout); } while (0)

// wait (pumping audio) for the engine to report the outcome of a set_media request
static bool media_done(rig &t, eiii::media_result &out, double timeout = 5.0)
{
	return t.wait([&] { return t.e.poll_media_result(out); }, timeout);
}

// Load Bank -> Enter -> drive 3 -> Enter; true if the bank list appears
static void dump(rig &t, char const *what) { if (getenv("MEDIA_SWAP_TRACE")) printf("  %s @%.2f: [%s] [%s] [%s] [%s]\n", what, t.now(), t.row(0).c_str(), t.row(1).c_str(), t.row(2).c_str(), t.row(3).c_str()); }
static bool open_bank_list(rig &t)
{
	t.press("load_bank");
	dump(t, "load bank");
	if (!t.wait_row(0, "   LOAD BANK from", 3.0)) return false;
	// the module opens on the last drive: first time it waits for Enter (-> drive selection); after a load from D3 it
	// reads D3 again and lists its banks; after a drive error it opens drive selection
	t.pump(0.3);
	if (!t.wait([&] { return t.row(3).find("Wait") == std::string::npos; }, 10.0)) { dump(t, "drive not ready"); return false; }
	t.pump(0.5);                                     // let the OS finish writing the prompt
	dump(t, "opened");
	if (t.lcd().find("Select a Bank") != std::string::npos && t.row(1).compare(0, 3, "D3 ") == 0) return true;
	if (t.lcd().find("Select a Drive") == std::string::npos)
	{
		t.press("enter");
		dump(t, "enter");
		if (!t.wait_text("Select a Drive", 3.0)) return false;
	}
	t.press("keypad_3");
	dump(t, "3");
	if (!t.wait_row(1, "D3 ", 3.0)) return false;
	t.press("enter");
	bool const ok = t.wait_text("Select a Bank", 10.0);
	dump(t, "enter");
	return ok;
}
static bool load_bank(rig &t, char const *digits, std::string const &listed, std::function<bool()> loaded)
{
	if (!open_bank_list(t)) return false;
	t.press((std::string("keypad_") + digits[0]).c_str()); t.press((std::string("keypad_") + digits[1]).c_str());
	if (!t.wait_row(2, listed, 3.0)) { dump(t, "bank number"); return false; }
	t.press("enter");
	return t.wait(loaded, 90.0);
}
static bool load_bank_44(rig &t) { return load_bank(t, "44", "B44 Tenor Sax", [&] { return t.row(1).compare(0, 17, "S00 Scott's Place") == 0; }); }
static bool file_exists(std::string const &p) { struct stat st; return !p.empty() && stat(p.c_str(), &st) == 0; }

int main()
{
	auto cfg = media_config();
	if (cfg.rt.hd_path.empty() || cfg.rt.cd_path.empty()) { printf("SKIP: set EIII_ROM_DIR, EIII_HD, EIII_CD\n"); return 77; }
	std::string hd2 = env("EIII_HD2");
	if (hd2.empty()) { auto const s = cfg.rt.hd_path.rfind('/'); hd2 = cfg.rt.hd_path.substr(0, s + 1) + "EIIIOS2_42_HDImage_239MB.ISO"; }
	std::string const cd = cfg.rt.cd_path;
	rig t(cfg);
	eiii::media_result r;
	CHECK(t.wait_row(0, "P00 ", 25.0), "power-on main screen at %.2f s: [%s]", t.now(), t.row(0).c_str());
	t.pump(1.0);

	// 1. the same disc taken out and put back: a medium change for the drive
	t.e.set_media(eiii::media_slot::cd, cd);
	CHECK(media_done(t, r) && r.ok && r.slot == eiii::media_slot::cd && r.path == cd, "CD re-mounted on the engine thread (%s)", r.error.c_str());
	CHECK(load_bank_44(t), "after the disc change Load Bank 44 from D3 loads Tenor Sax: [%s] [%s]", t.row(0).c_str(), t.row(1).c_str());
	t.pump(1.0);

	// 2. eject: the drive reports no medium
	t.e.set_media(eiii::media_slot::cd, "");
	CHECK(media_done(t, r) && r.ok && r.path.empty(), "CD ejected");
	bool const listed = open_bank_list(t);
	printf("  with no disc: [%s] [%s] [%s] [%s]\n", t.row(0).c_str(), t.row(1).c_str(), t.row(2).c_str(), t.row(3).c_str());
	CHECK(!listed, "with the CD ejected Load Bank from D3 does not list banks");

	// the OS retries, then reports the empty drive; Enter acknowledges (back to drive selection), Load Bank leaves
	CHECK(t.wait_text("SCSI Hardware Error!", 30.0), "the OS reports the empty drive: [%s]", t.row(3).c_str());
	t.press("enter");
	CHECK(t.wait_text("Select a Drive", 3.0), "Enter acknowledges the error: [%s]", t.row(3).c_str());
	t.press("load_bank");
	CHECK(t.wait_row(1, "S00 Scott's Place", 3.0), "Load Bank leaves to the main screen, bank still in memory: [%s] [%s]", t.row(0).c_str(), t.row(1).c_str());

	// 3. disc in again
	t.e.set_media(eiii::media_slot::cd, cd);
	CHECK(media_done(t, r) && r.ok, "CD mounted again");
	CHECK(load_bank_44(t), "Load Bank 44 works again after re-inserting: [%s]", t.row(1).c_str());

	// 3b. another disc, then the first one again
	std::string cd2 = env("EIII_CD2");
	if (cd2.empty() && getenv("HOME")) cd2 = std::string(getenv("HOME")) + "/EIII_RE/media/cd/EIII.02 More Emulator Standards.iso";
	if (file_exists(cd2))
	{
		t.e.set_media(eiii::media_slot::cd, cd2);
		CHECK(media_done(t, r) && r.ok, "second disc mounted: %s", cd2.c_str());
		CHECK(load_bank(t, "01", "B01 Arc&Spic Celli4M", [&] { return t.row(0).compare(0, 20, "P00 Basic Arco Celli") == 0; }),
			"Load Bank 01 from the second disc: [%s] [%s]", t.row(0).c_str(), t.row(1).c_str());
		t.e.set_media(eiii::media_slot::cd, cd);
		CHECK(media_done(t, r) && r.ok, "first disc back in the drive");
		CHECK(load_bank_44(t), "Load Bank 44 from the first disc again: [%s]", t.row(1).c_str());
	}
	else printf("SKIP second disc (set EIII_CD2)\n");

	// 4. unusable images are refused; the current disc stays
	t.e.set_media(eiii::media_slot::cd, "/nonexistent/disc.iso");
	CHECK(media_done(t, r) && !r.ok && !r.error.empty(), "missing CD image refused: %s", r.error.c_str());
	t.e.set_media(eiii::media_slot::hd, "/nonexistent/os.hd");
	CHECK(media_done(t, r) && !r.ok, "missing HD image refused: %s", r.error.c_str());
	t.pump(1.0);
	CHECK(t.ui.snapshot().running && t.row(1).compare(0, 17, "S00 Scott's Place") == 0, "machine untouched by the refused requests: [%s]", t.row(1).c_str());
	CHECK(t.e.media_path(eiii::media_slot::cd) == cd, "mounted CD unchanged");

	// 5. HD swap: power-on boot from the new image
	if (file_exists(hd2))
	{
		double const before = t.now();
		t.e.set_media(eiii::media_slot::hd, hd2);
		CHECK(media_done(t, r) && r.ok && r.path == hd2, "HD swapped to %s", hd2.c_str());
		CHECK(t.wait([&] { return t.now() < before; }, 5.0), "machine restarted from power-on (%.2f s -> %.2f s)", before, t.now());
		CHECK(t.wait_row(0, "P00 ", 25.0) && t.wait_row(1, "S00 ", 1.0), "OS booted from the new HD: [%s] [%s]", t.row(0).c_str(), t.row(1).c_str());
		CHECK(t.e.media_path(eiii::media_slot::hd) == hd2 && t.e.media_path(eiii::media_slot::cd) == cd, "media after restart: new HD, same CD");
		CHECK(load_bank_44(t), "Load Bank 44 from the CD after the HD swap: [%s]", t.row(1).c_str());
	}
	else printf("SKIP HD swap (no second HD image: set EIII_HD2)\n");

	auto const st = t.e.stats();
	CHECK(st.panel_dropped == 0, "no dropped panel inputs");
	printf("%s: %d failure(s)\n", fails ? "FAIL" : "OK", fails);
	return fails ? 1 : 0;
}
