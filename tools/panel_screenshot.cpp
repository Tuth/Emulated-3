// panel_screenshot: boot an EIII instance, operate it through the PanelUI adapter (optional factory bank load) and
// write the rendered rack panel as PNG at the requested scales.  No X server needed.
//   panel_screenshot OUT_PREFIX [--load-bank] [--scales 100,125,150,200] [--off] [--master]
//                    [--theme stock|blue|amber|vfd|oled[,...]] [--level 0..1] [--crop X0,Y0,X1,Y1 (panel units)] [--exact]
// --exact writes OUT_PREFIX itself (one scale) instead of OUT_PREFIX_<scale>.png.  "{theme}" in OUT_PREFIX is replaced
// by the theme name; with several themes, one image per theme from the same machine state.
// Media: EIII_ROM_DIR, EIII_HD, EIII_CD.
#include "tests/panel_rig.h"
#include "ui/panel_view.h"
#include <sstream>

using namespace eiii_test;

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: panel_screenshot OUT_PREFIX [--load-bank] [--scales 100,125,150,200] [--off] [--master] [--theme NAME] [--level V] [--crop X0,Y0,X1,Y1] [--exact]\n"); return 2; }
	std::string const prefix_arg = argv[1];
	bool load = false, off = false, master = false, exact = false; std::string scales = "100";
	std::vector<eiii::lcd_theme> themes; double level = -1; float crop[4] = { 0, 0, 0, 0 };
	for (int i = 2; i < argc; ++i)
	{
		std::string const a = argv[i];
		if (a == "--load-bank") load = true; else if (a == "--off") off = true; else if (a == "--master") master = true; else if (a == "--exact") exact = true;
		else if (a == "--scales" && i + 1 < argc) scales = argv[++i];
		else if (a == "--theme" && i + 1 < argc)
		{
			std::stringstream ts(argv[++i]); std::string n; eiii::lcd_theme th;
			while (std::getline(ts, n, ',')) { if (!eiii::lcd_theme_from_name(n.c_str(), th)) { fprintf(stderr, "unknown theme %s\n", n.c_str()); return 2; } themes.push_back(th); }
		}
		else if (a == "--level" && i + 1 < argc) level = atof(argv[++i]);
		else if (a == "--crop" && i + 1 < argc) { if (sscanf(argv[++i], "%f,%f,%f,%f", &crop[0], &crop[1], &crop[2], &crop[3]) != 4) { fprintf(stderr, "bad --crop\n"); return 2; } }
	}
	rig t(media_config());
	if (off) { t.e.power(false); t.pump(0.2); }
	else
	{
		if (!t.wait_row(0, "P00 ", 20.0)) { fprintf(stderr, "no main screen\n"); return 1; }
		if (load)
		{
			t.press("load_bank"); t.wait_text("LOAD BANK", 3); t.press("enter"); t.wait_text("Select a Drive", 3); t.press("keypad_3"); t.wait_row(1, "D3", 3);
			t.press("enter"); t.wait_text("Select a Bank", 10); t.press("keypad_4"); t.press("keypad_4"); t.wait_row(2, "B44", 3); t.press("enter");
			t.wait_row(1, "S00 Scott's Place", 90); t.press("keypad_0"); t.press("keypad_1"); t.wait_row(0, "P01", 3);
			t.press("play"); t.pump(1.3);                      // sequencer running: tempo/position on row 3, Play LED
		}
		if (master) { t.press("master"); t.press("keypad_1"); t.pump(0.6); }
		t.pump(0.3);
	}
	t.ui.poll();
	if (themes.empty()) themes.push_back(eiii::lcd_theme::stock);
	printf("LCD:\n%s\nLEDs: {%s}\n", t.lcd().c_str(), t.leds().c_str());
	for (eiii::lcd_theme const theme : themes)
	{
		t.ui.set_display_theme(theme);
		if (level >= 0) t.ui.set_contrast(level);
		std::string prefix = prefix_arg;
		if (size_t const at = prefix.find("{theme}"); at != std::string::npos) prefix.replace(at, 7, eiii::lcd_theme_name(theme));
		std::stringstream ss(scales); std::string sc;
		while (std::getline(ss, sc, ','))
		{
			double const pct = atof(sc.c_str());
			eiii::ui::panel_view v; int w, h; eiii::ui::panel_view::preferred_size(pct, w, h); v.set_size(w, h);
			eiii::ui::canvas frame; v.render(frame, t.ui, true);
			if (crop[2] > crop[0] && crop[3] > crop[1])
			{
				float const s = v.scale(), ox = (w - eiii::ui::panel_w * s) * .5f, oy = (h - eiii::ui::panel_h * s) * .5f;
				int const x0 = std::max(0, int(ox + crop[0] * s)), y0 = std::max(0, int(oy + crop[1] * s));
				int const x1 = std::min(w, int(ox + crop[2] * s)), y1 = std::min(h, int(oy + crop[3] * s));
				eiii::ui::canvas cut; cut.resize(x1 - x0, y1 - y0);
				for (int y = y0; y < y1; ++y) std::copy_n(&frame.px[size_t(y) * w + x0], x1 - x0, &cut.px[size_t(y - y0) * cut.w]);
				frame = std::move(cut); w = frame.w; h = frame.h;
			}
			std::string const out = exact ? prefix : prefix + "_" + sc + ".png";
			if (!frame.write_png(out)) { fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
			printf("wrote %s (%dx%d)\n", out.c_str(), w, h);
		}
	}
	return 0;
}
