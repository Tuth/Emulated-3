// Several EIII instances in one process (CLAP requirement): each engine runs its own machine on its own thread.
//   A and B receive identical inputs at identical frames; A's panel state is read every block, B's never.
//   -> bit-identical audio proves determinism, instance independence and that reading the LCD/LEDs has no effect.
//   C receives different inputs and must end in a different state while A/B are unaffected.
#include "panel_rig.h"

using namespace eiii_test;
static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); ++fails; } printf("\n"); fflush(stdout); } while (0)

int main()
{
	auto cfg = media_config();
	if (cfg.rt.hd_path.empty() || cfg.rt.cd_path.empty()) { printf("SKIP: set EIII_ROM_DIR, EIII_HD, EIII_CD\n"); return 77; }
	rig a(cfg), b(cfg), c(cfg);
	b.poll_every_block = false;
	c.poll_every_block = false;
	// one script, applied at the same block to A and B (C gets a different one); the three engines run concurrently
	struct ev { double t; char const *id; int midi; };
	std::vector<ev> const script = {
		{ 6.0, "load_bank", 0 }, { 7.0, "enter", 0 }, { 8.0, "keypad_3", 0 }, { 9.0, "enter", 0 }, { 10.5, "keypad_4", 0 },
		{ 11.0, "keypad_4", 0 }, { 12.0, "enter", 0 }, { 22.0, "keypad_0", 0 }, { 22.5, "keypad_1", 0 },
		{ 24.0, nullptr, 1 }, { 25.0, nullptr, 2 }, { 25.2, "master", 0 } };
	std::vector<ev> const script_c = { { 6.0, "master", 0 }, { 7.0, "keypad_1", 0 } };
	double const end = 27.0, block_s = 256.0 / 48000.0;
	size_t ia = 0, ic = 0;
	std::vector<std::pair<double, int>> releases_ab, releases_c;
	for (double t = 0; t < end; t += block_s)
	{
		while (ia < script.size() && script[ia].t <= t)
		{
			ev const &e = script[ia++];
			for (rig *r : { &a, &b })
			{
				if (e.id) r->ui.press_button(eiii::panel_find_control(e.id));
				else if (e.midi == 1) r->midi({ 0x9d, 0x3c, 0x64 });
				else r->midi({ 0x8d, 0x3c, 0x40 });
			}
			if (e.id) releases_ab.emplace_back(t + 0.08, eiii::panel_find_control(e.id));
		}
		while (ic < script_c.size() && script_c[ic].t <= t) { c.ui.press_button(eiii::panel_find_control(script_c[ic].id)); releases_c.emplace_back(t + 0.08, eiii::panel_find_control(script_c[ic].id)); ++ic; }
		for (auto it = releases_ab.begin(); it != releases_ab.end();) if (it->first <= t) { a.ui.release_button(it->second); b.ui.release_button(it->second); it = releases_ab.erase(it); } else ++it;
		for (auto it = releases_c.begin(); it != releases_c.end();) if (it->first <= t) { c.ui.release_button(it->second); it = releases_c.erase(it); } else ++it;
		a.pump_frames(256); b.pump_frames(256); c.pump_frames(256);
	}
	b.ui.poll(); c.ui.poll(); a.ui.poll();
	printf("A: [%s] [%s] peak %.4f hash %016llx\n", a.row(0).c_str(), a.row(3).c_str(), a.peak, (unsigned long long)a.hash);
	printf("B: [%s] [%s] peak %.4f hash %016llx\n", b.row(0).c_str(), b.row(3).c_str(), b.peak, (unsigned long long)b.hash);
	printf("C: [%s] [%s] peak %.4f hash %016llx\n", c.row(0).c_str(), c.row(3).c_str(), c.peak, (unsigned long long)c.hash);
	CHECK(a.hash == b.hash && a.peak > 0.05, "A and B bit-identical over %.0f s with a note (peak %.3f): determinism, no read side effects", end, a.peak);
	CHECK(a.lcd() == b.lcd() && a.ui.read_leds() == b.ui.read_leds(), "A and B end in the same panel state");
	CHECK(c.row(0).find("MASTER TUNE") != std::string::npos && a.row(0).find("MASTER") != std::string::npos && c.peak < 1e-3f,
		"C independent: [%s], silent (peak %.2g)", c.row(0).c_str(), c.peak);
	auto sa = a.e.stats(), sc = c.e.stats();
	CHECK(sa.underruns == 0 && sc.underruns == 0 && sa.panel_dropped == 0, "no underruns, no dropped inputs");
	printf("%s: %d failure(s)\n", fails ? "FAIL" : "OK", fails);
	return fails ? 1 : 0;
}
