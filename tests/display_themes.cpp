// Display themes on the Contrast knob (host UI only): from power-on, operate the rack panel view with the pointer and
// check that a click on Contrast cycles Stock -> Blue/White -> Amber/Red -> VFD -> OLED -> Stock, that a drag / the
// wheel only changes the current theme's level, that each theme renders differently (OLED window = true black), that
// the settings round-trip through the GUI/CLAP state text, and that none of it reaches the emulated machine.
// Media: EIII_ROM_DIR, EIII_HD, EIII_CD.
#include "panel_rig.h"
#include "ui/panel_view.h"

using namespace eiii_test;
static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); ++fails; } printf("\n"); fflush(stdout); } while (0)

namespace {
using eiii::lcd_theme;

// mean colour over the LCD window (panel units 147.5..378 x 82.5..155.5 at scale 1)
uint32_t lcd_mean(eiii::ui::canvas const &f)
{
	uint64_t r = 0, g = 0, b = 0, n = 0;
	for (int y = 84; y < 154; ++y) for (int x = 149; x < 376; ++x) { uint32_t const p = f.px[size_t(y) * f.w + x]; r += p >> 16 & 255; g += p >> 8 & 255; b += p & 255; ++n; }
	return uint32_t(r / n) << 16 | uint32_t(g / n) << 8 | uint32_t(b / n);
}
}

int main()
{
	auto cfg = media_config();
	if (cfg.rt.hd_path.empty()) { printf("SKIP: set EIII_ROM_DIR, EIII_HD, EIII_CD\n"); return 77; }
	rig t(cfg);
	CHECK(t.wait_row(0, "P00 ", 20.0), "power-on main screen [%s]", t.row(0).c_str());
	t.pump(0.3); t.ui.poll();
	std::string const lcd0 = t.lcd();

	eiii::ui::panel_view v; v.set_size(1472, 538);
	eiii::ui::canvas frame;
	int const knob = eiii::panel_find_control("contrast");
	float kx, ky; v.control_center(knob, kx, ky);
	auto click = [&] { v.mouse_down(kx, ky, 1, t.ui); v.mouse_up(kx, ky, 1, t.ui); };
	auto render = [&] { v.render(frame, t.ui, false); return lcd_mean(frame); };

	CHECK(t.ui.display_theme() == lcd_theme::stock, "default theme %s", eiii::lcd_theme_name(t.ui.display_theme()));
	uint32_t mean[eiii::lcd_theme_count];
	mean[0] = render();
	lcd_theme const order[] = { lcd_theme::blue_white, lcd_theme::amber_red, lcd_theme::vfd, lcd_theme::oled, lcd_theme::stock };
	for (int i = 0; i < 5; ++i)
	{
		double const lv = t.ui.contrast();
		click();
		CHECK(t.ui.display_theme() == order[i], "click %d on Contrast -> %s", i + 1, eiii::lcd_theme_name(t.ui.display_theme()));
		if (i < 4) mean[i + 1] = render();
		(void)lv;
	}
	bool distinct = true;
	for (int a = 0; a < 5; ++a) for (int b = a + 1; b < 5; ++b) if (mean[a] == mean[b]) distinct = false;
	CHECK(distinct, "each theme renders differently: LCD means %06x %06x %06x %06x %06x", mean[0], mean[1], mean[2], mean[3], mean[4]);
	CHECK(render() == mean[0], "back to Stock renders as before (%06x)", mean[0]);

	// OLED: the window background is true black (no ghost dots, no shading, no reflection)
	t.ui.set_display_theme(lcd_theme::oled); render();
	uint32_t const corner = frame.px[size_t(151) * frame.w + 372], row4 = frame.px[size_t(146) * frame.w + 260];
	CHECK(corner == 0 && row4 == 0, "OLED window background true black (corner %06x, empty row 4 %06x)", corner, row4);
	t.ui.set_display_theme(lcd_theme::stock);

	// drag = turn: level of the current theme only, theme unchanged
	double const stock0 = t.ui.contrast(), vfd0 = t.ui.theme_level(lcd_theme::vfd);
	v.mouse_down(kx, ky, 1, t.ui); v.mouse_move(kx, ky - 15, t.ui); v.mouse_move(kx, ky - 30, t.ui); v.mouse_up(kx, ky - 30, 1, t.ui);
	CHECK(t.ui.display_theme() == lcd_theme::stock && std::fabs(t.ui.contrast() - std::min(1.0, stock0 + 0.25)) < 1e-9 && t.ui.theme_level(lcd_theme::vfd) == vfd0,
		"drag up 30 units: theme %s, level %.3f -> %.3f, VFD level untouched %.3f", eiii::lcd_theme_name(t.ui.display_theme()), stock0, t.ui.contrast(), t.ui.theme_level(lcd_theme::vfd));
	// a small jitter is still a click
	double const before = t.ui.contrast();
	v.mouse_down(kx, ky, 1, t.ui); v.mouse_move(kx + 1, ky - 2, t.ui); v.mouse_up(kx + 1, ky - 2, 1, t.ui);
	CHECK(t.ui.display_theme() == lcd_theme::blue_white && t.ui.theme_level(lcd_theme::stock) == before, "2-unit jitter counts as a click -> %s, Stock level kept %.3f",
		eiii::lcd_theme_name(t.ui.display_theme()), t.ui.theme_level(lcd_theme::stock));
	// wheel = turn
	double const blue0 = t.ui.contrast();
	v.wheel(kx, ky, -5, t.ui);
	CHECK(t.ui.display_theme() == lcd_theme::blue_white && std::fabs(t.ui.contrast() - (blue0 - 0.1)) < 1e-9, "wheel -5: Blue level %.3f -> %.3f", blue0, t.ui.contrast());
	uint32_t const dim = render(); v.wheel(kx, ky, 20, t.ui); uint32_t const bright = render();
	CHECK((dim & 255) < (bright & 255), "Blue/White brighter with the knob up (%06x -> %06x)", dim, bright);

	// state text round trip (the CLAP state and the standalone gui.state use it)
	t.ui.set_display_theme(lcd_theme::vfd); t.ui.set_contrast(0.9);
	std::string const st = t.ui.display_state();
	eiii::panel_adapter other(t.e);
	size_t pos = 0;
	while (pos < st.size()) { size_t const e = st.find('\n', pos); other.apply_display_state_line(st.substr(pos, e - pos).c_str()); pos = e + 1; }
	CHECK(other.display_state() == st && other.display_theme() == lcd_theme::vfd && other.contrast() == 0.9, "state round trip:\n%s", st.c_str());
	eiii::panel_adapter legacy(t.e);
	legacy.apply_display_state_line("contrast 0.3000");
	CHECK(legacy.display_theme() == lcd_theme::stock && legacy.contrast() == 0.3, "pre-theme state 'contrast 0.3000' -> Stock level %.2f", legacy.contrast());
	CHECK(!legacy.apply_display_state_line("lcd_theme nonsense") || legacy.display_theme() == lcd_theme::stock, "unknown theme name ignored");

	// none of it reached the machine
	t.pump(0.3); t.ui.poll();
	auto const es = t.e.stats();
	CHECK(t.lcd() == lcd0 && es.panel_dropped == 0, "machine untouched: LCD unchanged, no panel input dropped");
	printf("%s: %d failure(s)\n", fails ? "FAIL" : "OK", fails);
	return fails ? 1 : 0;
}
