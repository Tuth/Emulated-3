// EIII 4U rack front panel artwork + interaction — see panel_view.h.
// Geometry: measured on the reference photograph (panel units = photo pixels minus (58, 215)); legends transcribed
// in analysis/EIII_RACK_PANEL_CONTROL_MAP.md.  Colours: graphite plate, muted magenta / yellow / cyan rules.
#include "panel_view.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace eiii::ui {

namespace {
// palette
rgba const c_plate     = rgba::hex(0x2a2b2e);
rgba const c_ink       = rgba::hex(0xebe9e3);
rgba const c_magenta   = rgba::hex(0xc9408f);
rgba const c_yellow    = rgba::hex(0xd8c43c);
rgba const c_cyan      = rgba::hex(0x38a7c0);
rgba const c_divider   = rgba::hex(0xbfbfbd);
rgba const c_cap       = rgba::hex(0x18191b);
rgba const c_window    = rgba::hex(0x241319);
rgba const c_lcd_bg    = rgba::hex(0x8e9a57);
rgba const c_lcd_ink   = rgba::hex(0x1c2316);
rgba const c_led_off   = rgba::hex(0x3b3130);
rgba const c_led_on    = rgba::hex(0xff3a2c);
rgba const c_letterbox = rgba::hex(0x141414);

// LCD geometry (panel units): window and 5x8 dot grid of the 4 x 20 module
float const lcd_x0 = 147.5f, lcd_y0 = 82.5f, lcd_x1 = 378.f, lcd_y1 = 155.5f;
float const dot_pitch = 1.85f, dot_size = 1.62f, char_pitch_x = 11.0f, char_pitch_y = 17.0f;

} // namespace

// 4 x 20 display themes (host UI; eiii::lcd_theme).  k = the theme's Contrast-knob level 0..1.
//   Stock       reflective yellow-green STN (the original rendering): k = drive / contrast
//   Blue/White  negative STN, white dots on a blue backlight: k = backlight + dot brightness
//   Amber/Red   amber dots through the unit's red window: k = brightness + slight bloom
//   VFD         cyan-green phosphor on black glass: k = brightness + glow radius/intensity
//   OLED        yellow on true black (no ghost dots, no reflection): k = brightness
lcd_look lcd_look_for(lcd_theme t, float k)
{
	lcd_look L;
	switch (t)
	{
	default:
	case lcd_theme::stock:
		L.bg = c_lcd_bg; L.ink = mix(c_lcd_bg, c_lcd_ink, 0.35f + 0.65f * k); L.ghost = mix(c_lcd_bg, c_lcd_ink, 0.03f + 0.08f * k);
		break;
	case lcd_theme::blue_white:
	{
		float const b = 0.35f + 0.65f * k;
		L.bg = rgba::hex(0x1d3fd0).scaled(b); L.ink = mix(L.bg, rgba::hex(0xf2f6ff), 0.55f + 0.45f * k); L.ghost = mix(L.bg, rgba::hex(0x000820), 0.18f);
		L.shade = 1.6f; L.glow = 0.12f * b; L.glow_col = rgba::hex(0xc8d8ff); L.glow_sigma = 1.1f; L.glow_gain = 1.4f;
		break;
	}
	case lcd_theme::amber_red:
	{
		float const b = 0.4f + 0.6f * k;
		L.bg = rgba::hex(0x200609); L.ink = mix(L.bg, rgba::hex(0xff7a22), b); L.ghost = mix(L.bg, rgba::hex(0xff5a1a), 0.045f);
		L.shade = 1.f; L.glow = 0.25f * b; L.glow_col = rgba::hex(0xff4a14); L.glow_sigma = 1.4f; L.glow_gain = 1.5f; L.reflect = 0.04f;
		break;
	}
	case lcd_theme::vfd:
	{
		float const b = 0.55f + 0.45f * k;
		rgba const phos = rgba::hex(0x52ffd0);
		L.bg = rgba::hex(0x030605); L.ink = mix(L.bg, mix(phos, rgba::hex(0xeafff9), 0.3f * k), b); L.ghost = mix(L.bg, phos, 0.04f);
		L.shade = 0.f; L.glow = 0.3f + 0.5f * k; L.glow_col = phos; L.glow_sigma = 1.2f + 1.8f * k; L.glow_gain = 2.4f;
		L.dot_round = 0.45f; L.reflect = 0.035f;
		break;
	}
	case lcd_theme::oled:
		L.bg = rgba::hex(0x000000); L.ink = rgba::hex(0xffd21e).scaled(0.3f + 0.7f * k); L.ghost = rgba::hex(0, 0);
		L.shade = 0.f; L.reflect = 0.f; L.dot_round = 0.1f;
		break;
	}
	return L;
}

namespace {
uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
}

panel_view::panel_view()
{
	load_builtin_fonts(m_fonts);
	auto ctl = [](char const *id) { return panel_find_control(id); };
	auto btn = [&](char const *id, float x, float y, float r = 20.f) { element e{}; e.control = ctl(id); e.kind = element::button; e.x = x; e.y = y; e.r = r; m_elements.push_back(e); };
	auto sld = [&](char const *id, float x) { element e{}; e.control = ctl(id); e.kind = element::slider; e.x = x; e.r = 21.f; e.y_top = 253.f; e.y_bottom = 380.f; m_elements.push_back(e); };
	auto led = [&](char const *id, float x, float y) { element e{}; e.led = panel_find_led(id); e.kind = element::led_lamp; e.x = x; e.y = y; m_elements.push_back(e); };

	btn("power", 652, 44, 16);
	{ element e{}; e.control = ctl("contrast"); e.kind = element::knob; e.x = 652; e.y = 170; e.r = 16; m_elements.push_back(e); }
	btn("cursor_up", 512, 124); btn("cursor_left", 464, 170); btn("cursor_right", 559, 170); btn("cursor_down", 512, 217);
	btn("keypad_1", 453, 319); btn("keypad_2", 512, 319); btn("keypad_3", 570, 319);
	btn("keypad_4", 453, 377); btn("keypad_5", 512, 377); btn("keypad_6", 570, 377);
	btn("keypad_7", 453, 436); btn("keypad_8", 512, 436); btn("keypad_9", 570, 436);
	btn("keypad_0", 512, 495);
	btn("yes", 214, 260); btn("no", 214, 317);
	btn("transpose", 652, 261); btn("supermode", 652, 320); btn("save_bank", 652, 378); btn("load_bank", 652, 436); btn("enter", 652, 495);
	sld("data_slider", 138); sld("sample_input", 293); sld("master_volume", 370);
	btn("seq_mode", 768, 44); btn("seq_management", 938, 44); btn("seq_setup", 768, 216); btn("seq_edit", 938, 216);
	btn("play", 768, 495); btn("stop", 814, 495); btn("locate_left", 878, 495); btn("locate_right", 924, 495); btn("record", 988, 495); btn("erase", 1036, 495);
	btn("master", 1102, 45); btn("sample", 1271, 45); btn("preset_management", 1102, 217); btn("digital_processing", 1271, 217);
	btn("preset_definition", 1102, 391); btn("analog_processing", 1271, 391);

	led("led_transpose", 701, 277); led("led_supermode", 701, 336); led("led_enter", 701, 511);
	led("led_mode_segment", 804, 98); led("led_mode_song", 804, 136);
	led("led_seq_management", 987, 59); led("led_seq_setup", 812, 232); led("led_seq_edit", 987, 232);
	led("led_play", 769, 454); led("led_record", 990, 454); led("led_erase", 1037, 454);
	led("led_master", 1154, 60); led("led_sample", 1323, 60); led("led_preset_management", 1155, 232);
	led("led_digital_processing", 1323, 232); led("led_preset_definition", 1154, 405); led("led_analog_processing", 1322, 405);

	{ element e{}; e.kind = element::lcd; e.x = (lcd_x0 + lcd_x1) * .5f; e.y = (lcd_y0 + lcd_y1) * .5f; m_lcd_element = int(m_elements.size()); m_elements.push_back(e); }
	m_drawn.resize(m_elements.size());
}

void panel_view::preferred_size(double pct, int &w, int &h) { w = int(std::lround(panel_w * pct / 100.0)); h = int(std::lround(panel_h * pct / 100.0)); }

void panel_view::set_size(int w, int h)
{
	if (w == m_w && h == m_h && m_static_ok) return;
	m_w = std::max(w, 16); m_h = std::max(h, 16);
	m_s = std::min(m_w / panel_w, m_h / panel_h);
	m_xf = { m_s, (m_w - panel_w * m_s) * .5f, (m_h - panel_h * m_s) * .5f };
	m_static.resize(m_w, m_h);
	draw_static(m_static);
	m_static_ok = true;
	for (auto &d : m_drawn) d.valid = false;
}

bool panel_view::to_logical(float x, float y, float &lx, float &ly) const
{
	lx = (x - m_xf.ox) / m_xf.s; ly = (y - m_xf.oy) / m_xf.s;
	return lx >= 0 && ly >= 0 && lx < panel_w && ly < panel_h;
}

void panel_view::text(canvas &c, font &f, float lx, float ly, char const *s, float cap, rgba col, float tracking, int align)
{
	// 'cap' = cap height in panel units (as measured on the photograph)
	float const size = cap / std::max(0.3f, f.cap_height(100.f) / 100.f);
	// the panel's screen print is a heavy condensed face: embolden the available faces with a fractional double strike
	f.draw(c, m_xf.X(lx), m_xf.Y(ly), s, size * m_s, col, tracking * m_s, align);
	f.draw(c, m_xf.X(lx + 0.3f), m_xf.Y(ly), s, size * m_s, col.with_alpha(col.a * 0.85f), tracking * m_s, align);
}

// ---------------------------------------------------------------------------------------------- static artwork
void panel_view::draw_static(canvas &c)
{
	xf const &t = m_xf;
	float const s = m_s;
	c.clear(0x141414);
	(void)c_letterbox;
	// plate with a fine speckle (the panel's textured paint), slightly lighter top edge
	{
		int const x0 = int(std::floor(t.X(0))), y0 = int(std::floor(t.Y(0))), x1 = int(std::ceil(t.X(panel_w))), y1 = int(std::ceil(t.Y(panel_h)));
		for (int y = std::max(0, y0); y < std::min(c.h, y1); ++y)
			for (int x = std::max(0, x0); x < std::min(c.w, x1); ++x)
			{
				float const lx = (x + .5f - t.ox) / s, ly = (y + .5f - t.oy) / s;
				uint32_t const hsh = hash32(uint32_t(int(lx * 2.2f)) * 73856093u ^ uint32_t(int(ly * 2.2f)) * 19349663u);
				float n = ((hsh & 255) / 255.f - .5f) * 0.035f;
				if ((hsh >> 8 & 255) > 246) n += 0.05f;   // sparse light flecks
				float const edge = (ly < 2.f || ly > panel_h - 2.f) ? 0.03f : 0.f;
				rgba col = c_plate; col.r += n + edge; col.g += n + edge; col.b += n + edge;
				c.blend(x, y, col, 1.f);
			}
		// panel outline
		c.fill(t.X(0), t.Y(0), t.X(panel_w), t.Y(panel_h), [&](float x, float y) {
			float const d = sd_round_rect(x, y, t.X(0), t.Y(0), t.X(panel_w), t.Y(panel_h), 3 * s);
			return std::fabs(d + 0.6f * s) - 0.6f * s; }, [&](float, float) { return rgba::hex(0x3c3d40); });
	}
	// rack-ear mounting slots
	for (auto [hx, hy] : { std::pair{ 25.f, 112.f }, { 27.f, 422.f }, { 1449.f, 116.f }, { 1447.f, 427.f } })
	{
		fill_rect(c, t.X(hx - 16), t.Y(hy - 11), t.X(hx + 16), t.Y(hy + 11), rgba::hex(0x0e0e0f), 11 * s);
		c.fill(t.X(hx - 16), t.Y(hy - 11), t.X(hx + 16), t.Y(hy + 11), [&](float x, float y) {
			float const d = sd_round_rect(x, y, t.X(hx - 16), t.Y(hy - 11), t.X(hx + 16), t.Y(hy + 11), 11 * s);
			return std::fabs(d) - 0.7f * s; }, [&](float, float y) { return rgba::hex(0x55565a, y > t.Y(hy) ? 0.8f : 0.3f); });
	}
	// section dividers
	fill_line(c, t.X(739), t.Y(6), t.X(739), t.Y(532), 1.7f * s, c_divider);
	fill_line(c, t.X(1073), t.Y(6), t.X(1073), t.Y(532), 1.7f * s, c_divider);

	// ---- logo, model name ----
	for (float y : { 8.f, 23.8f, 39.6f }) fill_rect(c, t.X(144), t.Y(y), t.X(183.5f), t.Y(y + 7), c_ink, 0.4f * s);
	for (float x : { 192.f, 208.3f, 224.6f }) fill_rect(c, t.X(x), t.Y(7.5f), t.X(x + 7.6f), t.Y(46.6f), c_ink, 0.4f * s);
	fill_rect(c, t.X(242), t.Y(42.6f), t.X(586), t.Y(47.2f), c_magenta, 0.5f * s);
	auto spread = [&](font &f, char const *str, float x0, float x1, float baseline, float size) {
		// letters evenly spread between x0 and x1 (the panel's wide-tracked headings)
		int n = 0; for (char const *p = str; *p; ++p) ++n;
		float const step = (x1 - x0 - size * 0.9f) / float(std::max(1, n - 1));
		for (int i = 0; i < n; ++i) { char ch[2] = { str[i], 0 }; if (ch[0] != ' ') text(c, f, x0 + i * step + size * 0.45f, baseline, ch, size, c_ink, 0, 1); }
	};
	spread(m_fonts.bold, "E-MU SYSTEMS", 144, 379, 75.5f, 8.2f);
	spread(m_fonts.bold, "EMULATOR THREE", 144, 380, 172.5f, 11.0f);

	// LCD window (the display itself is drawn dynamically)
	fill_rect(c, t.X(lcd_x0 - 1.5f), t.Y(lcd_y0 - 1.5f), t.X(lcd_x1 + 1.5f), t.Y(lcd_y1 + 1.5f), rgba::hex(0x101012), 1.5f * s);

	// ---- sliders ----
	text(c, m_fonts.regular, 134, 217, "Data", 6.4f, c_ink, 0, 1);
	text(c, m_fonts.regular, 290, 217, "Sample Input", 6.4f, c_ink, 0, 1);
	text(c, m_fonts.regular, 372, 217, "Master Volume", 6.4f, c_ink, 0, 1);
	text(c, m_fonts.regular, 214, 235, "+/Yes/On", 6.4f, c_ink, 0, 1);
	text(c, m_fonts.regular, 214, 293, "-/No/Off", 6.4f, c_ink, 0, 1);
	for (float x : { 138.f, 293.f, 370.f })
	{
		fill_rect(c, t.X(x - 2.8f), t.Y(236), t.X(x + 2.8f), t.Y(386), rgba::hex(0x060607), 1.2f * s);
		fill_line(c, t.X(x + 2.4f), t.Y(238), t.X(x + 2.4f), t.Y(384), 0.6f * s, rgba::hex(0x4a4b4f));
	}
	// ---- floppy drive (not emulated; drawn) ----
	{
		fill_rect(c, t.X(112), t.Y(419), t.X(413), t.Y(510), rgba::hex(0x1c1c1e), 1.5f * s);
		fill_rect(c, t.X(116), t.Y(423), t.X(409), t.Y(445), rgba::hex(0x0b0b0c), 1 * s);          // slot area
		fill_rect(c, t.X(176), t.Y(429), t.X(352), t.Y(438), rgba::hex(0x232325), 0.5f * s);        // shutter
		fill_rect(c, t.X(116), t.Y(449), t.X(409), t.Y(506), rgba::hex(0x222224), 1 * s);           // bezel
		fill_rect(c, t.X(184), t.Y(452), t.X(318), t.Y(503), rgba::hex(0x19191b), 0.8f * s);        // door
		fill_rect(c, t.X(350), t.Y(470), t.X(382), t.Y(494), rgba::hex(0x2e2e31), 1.5f * s);        // eject button
		c.fill(t.X(350), t.Y(470), t.X(382), t.Y(494), [&](float x, float y) { return std::fabs(sd_round_rect(x, y, t.X(350), t.Y(470), t.X(382), t.Y(494), 1.5f * s)) - 0.5f * s; },
			[&](float, float) { return rgba::hex(0x4a4a4e); });
		fill_rect(c, t.X(132), t.Y(487), t.X(140), t.Y(490), rgba::hex(0x3a3420), 0.5f * s);        // drive LED
	}
	// ---- cursor cluster and keypad ----
	fill_triangle(c, t.X(507), t.Y(97), t.X(519), t.Y(97), t.X(513), t.Y(86), c_magenta);
	fill_triangle(c, t.X(508), t.Y(243), t.X(520), t.Y(243), t.X(514), t.Y(254), c_magenta);
	fill_triangle(c, t.X(438), t.Y(164), t.X(438), t.Y(176), t.X(427), t.Y(170), c_magenta);
	fill_triangle(c, t.X(587), t.Y(164), t.X(587), t.Y(176), t.X(598), t.Y(170), c_magenta);
	text(c, m_fonts.regular, 512, 173, "Cursor/Page", 6.2f, c_ink, 0, 1);
	struct kd { char const *d; float x, y; } const digits[] = { { "1", 453, 319 }, { "2", 512, 319 }, { "3", 570, 319 }, { "4", 453, 377 }, { "5", 512, 377 },
		{ "6", 570, 377 }, { "7", 453, 436 }, { "8", 512, 436 }, { "9", 570, 436 }, { "0", 512, 495 } };
	for (auto const &k : digits) text(c, m_fonts.regular, k.x, k.y - 25.5f, k.d, 6.6f, c_ink, 0, 1);

	// ---- function column: labels above magenta rules ----
	auto rule_label = [&](float by, char const *label, rgba col, bool bold) {
		text(c, bold ? m_fonts.bold : m_fonts.regular, 676, by - 5.5f, label, bold ? 7.2f : 6.6f, c_ink);
		fill_rect(c, t.X(670), t.Y(by - 2.4f), t.X(719), t.Y(by + 1.2f), col, 0.4f * s);
	};
	rule_label(44, "POWER", c_magenta, true);
	rule_label(170, "Contrast", c_magenta, false);
	rule_label(261, "Transpose", c_magenta, false);
	rule_label(320, "Supermode", c_magenta, false);
	rule_label(378, "Save Bank", c_magenta, false);
	rule_label(436, "Load Bank", c_magenta, false);
	rule_label(495, "Enter", c_magenta, false);

	// ---- sequencer ----
	spread(m_fonts.bold, "SEQUENCER", 753, 943, 19.5f, 8.2f);
	auto head = [&](float bx, float by, char const *label, rgba col) {
		text(c, m_fonts.bold, bx + 22, by - 6.8f, label, 7.4f, c_ink);
		fill_rect(c, t.X(bx + 20), t.Y(by - 1.8f), t.X(bx + 124), t.Y(by + 2.2f), col, 0.4f * s);
	};
	head(768, 44, "MODE", c_yellow); head(938, 44, "MANAGEMENT", c_yellow);
	head(768, 216, "SETUP", c_yellow); head(938, 216, "EDIT", c_yellow);
	text(c, m_fonts.narrow, 822, 101, "Segment/Cue List", 6.0f, c_ink);
	text(c, m_fonts.narrow, 822, 139, "Song", 6.0f, c_ink);
	float const step = 8.65f;
	auto items = [&](float dx, float tx, float y0, std::initializer_list<char const *> lines, float indent_x = 0) {
		// lines: "1 Item" -> numbered; "  Item" (leading spaces) -> continuation under indent_x
		float y = y0;
		for (char const *ln : lines)
		{
			if (ln[0] == ' ') { text(c, m_fonts.narrow, indent_x, y, ln + 2, 5.5f, c_ink); y += step; continue; }
			char d[2] = { ln[0], 0 };
			text(c, m_fonts.narrow, dx, y, d, 5.5f, c_ink, 0, 1);
			fill_line(c, t.X(dx + 3.6f), t.Y(y - 2.6f), t.X(tx - 2.6f), t.Y(y - 2.6f), 0.6f * s, c_ink.with_alpha(0.85f));
			text(c, m_fonts.narrow, tx, y, ln + 2, 5.5f, c_ink);
			y += step;
		}
	};
	items(937, 949, 78, { "1 Load Segment/Song", "2 Tempo", "3 Clock", "4 Track Status", "5 Track Mix", "6 Data Filters", "7 MIDI Options", "8 SMPTE Start", "9 Write SMPTE" });
	text(c, m_fonts.bold, 781, 251, "Segment/Cue List", 6.8f, c_ink);
	items(769, 781, 260.5f, { "1 Length", "2 Time Signature", "3 Countdown", "4 Autocorrect", "5 Metronome", "6 Looping", "7 Preset Assignment", "8 Cue List Mode" });
	text(c, m_fonts.bold, 948, 251, "Segment/Cue List", 6.8f, c_ink);
	items(937, 949, 260.5f, { "1 Step Edit", "2 Rename Segment", "3 Erase Segment", "4 Copy Segment", "5 Cut Segment", "6 Paste Segment", "7 Erase Track",
		"8 Bounce Track", "9 Punch-In", "0 Special Functions" });
	text(c, m_fonts.bold, 948, 363, "Song", 6.8f, c_ink);
	items(937, 949, 372, { "1 Step Edit", "2 Rename Song", "3 Erase Song", "4 Copy Song", "5 Cut Song", "6 Paste Song" });
	// transport legends
	text(c, m_fonts.regular, 768, 439, "Play", 6.4f, c_ink, 0, 1);
	text(c, m_fonts.regular, 816, 439, "Stop/", 6.4f, c_ink, 0, 1);
	text(c, m_fonts.regular, 816, 448.5f, "Continue", 6.4f, c_ink, 0, 1);
	text(c, m_fonts.regular, 902, 439, "Locate", 6.4f, c_ink, 0, 1);
	for (float x : { 864.f, 872.f }) fill_triangle(c, t.X(x + 7), t.Y(430.5f), t.X(x + 7), t.Y(438.5f), t.X(x), t.Y(434.5f), c_yellow);
	for (float x : { 925.f, 933.f }) fill_triangle(c, t.X(x), t.Y(430.5f), t.X(x), t.Y(438.5f), t.X(x + 7), t.Y(434.5f), c_yellow);
	text(c, m_fonts.regular, 990, 439, "Record", 6.4f, c_ink, 0, 1);
	text(c, m_fonts.regular, 990, 448.5f, "(Insert)", 6.4f, c_ink, 0, 1);
	text(c, m_fonts.regular, 1038, 439, "Erase", 6.4f, c_ink, 0, 1);

	// ---- modules ----
	spread(m_fonts.bold, "MODULES", 1086, 1251, 19.5f, 8.2f);
	auto mhead = [&](float bx, float by, char const *label) {
		text(c, m_fonts.bold, bx + 20, by - 8.0f, label, 7.4f, c_ink);
		fill_rect(c, t.X(bx + 20), t.Y(by + 0.2f), t.X(bx + 124), t.Y(by + 4.0f), c_cyan, 0.4f * s);
	};
	mhead(1102, 45, "MASTER"); mhead(1271, 45, "SAMPLE");
	mhead(1102, 217, "PRESET MANAGEMENT"); mhead(1271, 217, "DIGITAL PROCESSING");
	mhead(1102, 391, "PRESET DEFINITION"); mhead(1271, 391, "ANALOG PROCESSING");
	items(1104, 1115, 78.5f, { "1 Master Tune", "2 Rename Bank", "3 Erase Bank", "4 Supermode", "5 Audio Trigger", "6 Memory Available", "7 Disk Utilities", "8 Special" });
	items(1271, 1283, 79.5f, { "1 Load Sample", "2 Rename Sample", "3 Erase Sample", "4 Copy Sample", "5 Setup - VU Meter", "  Threshold", "  Stereo/Mono", "  Rate", "  Length",
		"6 Place Sample", "7 Arm Sampling", "8 Force Sampling", "0 Select Sample" }, 1303);
	items(1104, 1115, 251, { "1 Load Preset", "2 Rename Preset", "3 Erase Preset", "4 Copy Preset", "5 Create Preset", "6 Preset Size" });
	items(1271, 1283, 251, { "1 Setup - Loop Type", "  Loop In Reverse", "  Reverse Playback", "2 Loop - Start/Size", "  Autoloop", "  Crossfade Loop", "3 Truncate",
		"4 Copy Section", "5 Cut Section", "6 Paste Section", "7 Sample Rate Convert", "8 Digital Effects", "9 Undo", "0 Select Sample" }, 1304);
	items(1104, 1115, 424, { "1 Load Zone", "2 Edit Assignment", "3 Erase Zone", "4 Copy Zone", "5 Crossfade/Switch", "6 MIDI", "7 Arpeggiator", "8 Pitch Bend Range",
		"9 Velocity Curve", "0 Realtime Controls" });
	items(1271, 1283, 424, { "1 Setup - Tuning", "  Delay", "  Chorus", "2 VCA", "3 VCF", "4 LFO", "5 Auxiliary Envelope", "6 Velocity To", "7 Keyboard Mode",
		"8 Realtime Control Enable", "9 Output Channels", "0 Select Zone" }, 1306);

	// ---- button collars (panel cut-outs) and LED windows ----
	for (element const &e : m_elements)
	{
		if (e.kind == element::button || e.kind == element::knob)
		{
			float const cx = t.X(e.x), cy = t.Y(e.y), r = (e.r + 1.8f) * s;
			fill_circle(c, cx, cy + 0.6f * s, r, rgba::hex(0x050505));
			c.fill(cx - r, cy - r, cx + r, cy + r, [&](float x, float y) { return std::fabs(sd_circle(x, y, cx, cy, r)) - 0.45f * s; },
				[&](float, float y) { return rgba::hex(0x6a6b70, y > cy ? 0.55f : 0.18f); });
		}
	}
}

// ---------------------------------------------------------------------------------------------- dynamic elements
recti panel_view::element_rect(element const &e) const
{
	float x0, y0, x1, y1;
	switch (e.kind)
	{
	case element::led_lamp: x0 = e.x - 12; x1 = e.x + 12; y0 = e.y - 8; y1 = e.y + 8; break;
	case element::slider: x0 = e.x - e.r - 4; x1 = e.x + e.r + 4; y0 = e.y_top - e.r - 6; y1 = e.y_bottom + e.r + 8; break;
	case element::lcd: x0 = lcd_x0; x1 = lcd_x1; y0 = lcd_y0; y1 = lcd_y1; break;
	default: x0 = e.x - e.r - 1.5f; x1 = e.x + e.r + 1.5f; y0 = e.y - e.r - 1.5f; y1 = e.y + e.r + 2.5f; break;
	}
	return { int(std::floor(m_xf.X(x0))) - 1, int(std::floor(m_xf.Y(y0))) - 1, int(std::ceil(m_xf.X(x1))) + 1, int(std::ceil(m_xf.Y(y1))) + 1 };
}

void panel_view::draw_button(canvas &c, element const &e, bool down)
{
	float const s = m_s, cx = m_xf.X(e.x), cy = m_xf.Y(e.y) + (down ? 0.9f * s : 0.f), r = e.r * s;
	float const hl = down ? 0.16f : 0.32f;
	c.fill(cx - r, cy - r, cx + r, cy + r, [&](float x, float y) { return sd_circle(x, y, cx, cy, r); }, [&](float x, float y) {
		float const nx = (x - cx) / r, ny = (y - cy) / r;
		float const rr = std::sqrt(nx * nx + ny * ny);
		rgba col = mix(c_cap.scaled(down ? 0.8f : 1.f), rgba::hex(0x060607), std::clamp(rr * rr, 0.f, 1.f) * 0.8f);
		// soft specular highlight (upper left) and a faint lower rim
		float const hx = nx + 0.28f, hy = ny + 0.42f;
		float const h = std::exp(-(hx * hx * 4.5f + hy * hy * 9.f)) * hl;
		float const rim = std::clamp((ny - 0.55f) * 2.2f, 0.f, 1.f) * std::clamp((rr - 0.7f) * 3.f, 0.f, 1.f) * 0.10f;
		col = mix(col, rgba::hex(0xe8ecf2), h + rim);
		return col; });
}

void panel_view::draw_slider_knob(canvas &c, element const &e, double pos)
{
	float const s = m_s, cx = m_xf.X(e.x), cy = m_xf.Y(float(e.y_bottom - pos * (e.y_bottom - e.y_top))), r = e.r * s;
	fill_circle(c, cx + 0.6f * s, cy + 1.6f * s, r, rgba::hex(0x000000, 0.45f));
	c.fill(cx - r, cy - r, cx + r, cy + r, [&](float x, float y) { return sd_circle(x, y, cx, cy, r); }, [&](float x, float y) {
		float const nx = (x - cx) / r, ny = (y - cy) / r, rr = std::sqrt(nx * nx + ny * ny);
		rgba col = mix(rgba::hex(0x1d1e20), rgba::hex(0x0a0a0b), std::clamp(rr, 0.f, 1.f) * 0.6f);
		float const dish = std::exp(-std::pow((rr - 0.78f) * 9.f, 2.f)) * 0.10f;           // concave rim
		float const h = std::exp(-((nx + 0.3f) * (nx + 0.3f) * 3.f + (ny + 0.5f) * (ny + 0.5f) * 7.f)) * 0.18f;
		return mix(col, rgba::hex(0xdfe3ea), dish + h); });
}

void panel_view::draw_contrast(canvas &c, element const &e, double v)
{
	draw_button(c, e, false);
	float const s = m_s, cx = m_xf.X(e.x), cy = m_xf.Y(e.y), r = e.r * s;
	double const a = (-135.0 + 270.0 * v) * M_PI / 180.0;
	fill_line(c, cx + float(std::sin(a)) * r * 0.35f, cy - float(std::cos(a)) * r * 0.35f, cx + float(std::sin(a)) * r * 0.82f, cy - float(std::cos(a)) * r * 0.82f, 1.6f * s, rgba::hex(0xb8b8b8));
}

void panel_view::draw_led(canvas &c, element const &e, bool on)
{
	float const s = m_s, x0 = m_xf.X(e.x - 5.6f), x1 = m_xf.X(e.x + 5.6f), y0 = m_xf.Y(e.y - 2.3f), y1 = m_xf.Y(e.y + 2.3f);
	if (on)
	{
		float const gx = (x0 + x1) * .5f, gy = (y0 + y1) * .5f;
		c.fill(x0 - 7 * s, y0 - 6 * s, x1 + 7 * s, y1 + 6 * s, [&](float x, float y) { return sd_round_rect(x, y, x0 - 6 * s, y0 - 5 * s, x1 + 6 * s, y1 + 5 * s, 6 * s); },
			[&](float x, float y) { float const d = std::hypot((x - gx) / (9 * s), (y - gy) / (6 * s)); return c_led_on.with_alpha(std::max(0.f, 0.22f * (1 - d))); });
		c.fill(x0, y0, x1, y1, [&](float x, float y) { return sd_round_rect(x, y, x0, y0, x1, y1, 1.2f * s); },
			[&](float, float y) { return mix(rgba::hex(0xffb0a0), c_led_on, std::clamp((y - y0) / std::max(1.f, y1 - y0) * 1.6f, 0.f, 1.f)); });
	}
	else
	{
		fill_rect(c, x0, y0, x1, y1, c_led_off, 1.2f * s);
		fill_line(c, x0 + 1 * s, y0 + 0.7f * s, x1 - 1 * s, y0 + 0.7f * s, 0.5f * s, rgba::hex(0x8a7a76, 0.35f));
	}
}

void panel_view::draw_lcd(canvas &c, panel_adapter &ui)
{
	float const s = m_s;
	panel_snapshot const &snap = ui.snapshot();
	float const X0 = m_xf.X(lcd_x0), Y0 = m_xf.Y(lcd_y0), X1 = m_xf.X(lcd_x1), Y1 = m_xf.Y(lcd_y1);
	bool const lit = snap.running && ui.powered();
	if (!lit)
	{
		c.fill(X0, Y0, X1, Y1, [&](float x, float y) { return sd_round_rect(x, y, X0, Y0, X1, Y1, 1 * s); },
			[&](float x, float) { return mix(c_window, rgba::hex(0x2e0f14), (x - X0) / std::max(1.f, X1 - X0)); });
		if (snap.status[0])
			m_fonts.regular.draw(c, (X0 + X1) * .5f, (Y0 + Y1) * .5f + 3 * s, snap.status, 8.5f * s, rgba::hex(0xa08080), 0, 1);
		return;
	}
	lcd_view v; ui.read_lcd(v);
	lcd_look const L = lcd_look_for(ui.display_theme(), float(ui.contrast()));
	c.fill(X0, Y0, X1, Y1, [&](float x, float y) { return sd_round_rect(x, y, X0, Y0, X1, Y1, 1 * s); },
		[&](float x, float y) { float const g = 0.04f * ((y - Y0) / std::max(1.f, Y1 - Y0)) - 0.02f * ((x - X0) / std::max(1.f, X1 - X0)); return mix(L.bg, rgba::hex(0x000000), L.shade * (0.08f + g)); });
	float const gx0 = lcd_x0 + ((lcd_x1 - lcd_x0) - (char_pitch_x * 20 - (char_pitch_x - 5 * dot_pitch))) * .5f;
	float const gy0 = lcd_y0 + ((lcd_y1 - lcd_y0) - (char_pitch_y * 4 - (char_pitch_y - 8 * dot_pitch))) * .5f;
	float const ds = dot_size * s, rad = L.dot_round * dot_size * s;
	auto lit_dot = [&](int r, int col, int dy, int dx) { return v.display_on && ((v.dots[r][col][dy] >> (4 - dx)) & 1); };
	auto dot_x = [&](int col, int dx) { return m_xf.X(gx0 + col * char_pitch_x + dx * dot_pitch); };
	auto dot_y = [&](int r, int dy) { return m_xf.Y(gy0 + r * char_pitch_y + dy * dot_pitch); };
	if (L.glow > 0) draw_lcd_glow(c, L, lit_dot, dot_x, dot_y, ds, recti{ int(std::floor(X0)), int(std::floor(Y0)), int(std::ceil(X1)), int(std::ceil(Y1)) });
	for (int r = 0; r < lcd_view::rows; ++r)
		for (int col = 0; col < lcd_view::cols; ++col)
			for (int dy = 0; dy < 8; ++dy)
				for (int dx = 0; dx < 5; ++dx)
				{
					bool const on = lit_dot(r, col, dy, dx);
					if (!on && L.ghost.a <= 0) continue;
					float const x = dot_x(col, dx), y = dot_y(r, dy);
					fill_rect(c, x, y, x + ds, y + ds, on ? L.ink : L.ghost, rad);
				}
	// glass reflection
	if (L.reflect > 0)
		c.fill(X0, Y0, X1, Y1, [&](float x, float y) { return sd_round_rect(x, y, X0, Y0, X1, Y1, 1 * s); },
			[&](float x, float y) { float const u = (x - X0) / std::max(1.f, X1 - X0), w = (y - Y0) / std::max(1.f, Y1 - Y0); return rgba::hex(0xffffff, std::max(0.f, L.reflect - 0.08f * (u + w))); });
}

// Emissive halo around lit dots: dot coverage rasterised into a float map over the LCD window, blurred (three box
// passes per axis ~ gaussian), then blended in the theme's glow colour under the dots.
template <class LIT, class DX, class DY>
void panel_view::draw_lcd_glow(canvas &c, lcd_look const &L, LIT lit_dot, DX dot_x, DY dot_y, float ds, recti box)
{
	box.x0 = std::max(box.x0, 0); box.y0 = std::max(box.y0, 0); box.x1 = std::min(box.x1, c.w); box.y1 = std::min(box.y1, c.h);
	int const W = box.x1 - box.x0, H = box.y1 - box.y0;
	if (W <= 0 || H <= 0) return;
	std::vector<float> &m = m_glow_map, &tmp = m_glow_tmp;
	m.assign(size_t(W) * H, 0.f); tmp.resize(m.size());
	bool any = false;
	for (int r = 0; r < lcd_view::rows; ++r)
		for (int col = 0; col < lcd_view::cols; ++col)
			for (int dy = 0; dy < 8; ++dy)
				for (int dx = 0; dx < 5; ++dx)
				{
					if (!lit_dot(r, col, dy, dx)) continue;
					any = true;
					float const x = dot_x(col, dx) - box.x0, y = dot_y(r, dy) - box.y0;
					for (int py = std::max(0, int(std::floor(y))); py < std::min(H, int(std::ceil(y + ds))); ++py)
					{
						float const cy = std::clamp(std::min(float(py + 1), y + ds) - std::max(float(py), y), 0.f, 1.f);
						for (int px = std::max(0, int(std::floor(x))); px < std::min(W, int(std::ceil(x + ds))); ++px)
							m[size_t(py) * W + px] += cy * std::clamp(std::min(float(px + 1), x + ds) - std::max(float(px), x), 0.f, 1.f);
					}
				}
	if (!any) return;
	int const hb = std::max(1, int(std::lround(L.glow_sigma * m_s * 0.9f)));      // box half-width for ~sigma
	auto pass = [&](std::vector<float> &src, std::vector<float> &dst, bool horiz) {
		int const n = horiz ? W : H, lines = horiz ? H : W;
		float const inv = 1.f / float(2 * hb + 1);
		for (int l = 0; l < lines; ++l)
		{
			auto at = [&](int i) -> float & { return horiz ? src[size_t(l) * W + i] : src[size_t(i) * W + l]; };
			auto out = [&](int i) -> float & { return horiz ? dst[size_t(l) * W + i] : dst[size_t(i) * W + l]; };
			float acc = 0;
			for (int i = -hb; i <= hb; ++i) if (i >= 0 && i < n) acc += at(i);
			for (int i = 0; i < n; ++i)
			{
				out(i) = acc * inv;
				int const add = i + hb + 1, sub = i - hb;
				if (add < n) acc += at(add);
				if (sub >= 0) acc -= at(sub);
			}
		}
	};
	for (int k = 0; k < 3; ++k) { pass(m, tmp, true); pass(tmp, m, false); }
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x)
		{
			float const a = std::min(1.f, m[size_t(y) * W + x] * L.glow_gain) * L.glow;
			if (a > 0.002f) c.blend(box.x0 + x, box.y0 + y, L.glow_col, a);
		}
}

recti panel_view::render(canvas &frame, panel_adapter &ui, bool full)
{
	if (frame.w != m_w || frame.h != m_h) { frame.resize(m_w, m_h); full = true; }
	recti dirty{};
	if (full) { frame.copy_from(m_static, { 0, 0, m_w, m_h }); dirty = { 0, 0, m_w, m_h }; for (auto &d : m_drawn) d.valid = false; }
	// LCD key: dots + power + contrast
	lcd_view lv; ui.read_lcd(lv);
	uint32_t lcd_key = 2166136261u;
	auto mixk = [&](uint32_t v) { lcd_key = (lcd_key ^ v) * 16777619u; };
	for (auto &rw : lv.dots) for (auto &cl : rw) for (uint8_t d : cl) mixk(d);
	mixk(lv.display_on); mixk(ui.snapshot().running); mixk(ui.powered()); mixk(uint32_t(ui.contrast() * 1000)); mixk(uint32_t(ui.display_theme())); mixk(uint32_t(ui.snapshot().status[0]));
	for (size_t i = 0; i < m_elements.size(); ++i)
	{
		element const &e = m_elements[i];
		uint32_t key = 0;
		switch (e.kind)
		{
		case element::button: key = e.control >= 0 && ui.button_down(e.control) ? 1 : 0; if (e.control == panel_find_control("power")) key |= ui.powered() ? 2 : 0; break;
		case element::slider: key = uint32_t(ui.slider(e.control) * 8192); break;
		case element::knob: key = uint32_t(ui.contrast() * 8192) | uint32_t(ui.display_theme()) << 16; break;
		case element::led_lamp: key = ui.led(e.led) ? 1 : 0; break;
		case element::lcd: key = lcd_key; break;
		}
		if (m_drawn[i].valid && m_drawn[i].key == key) continue;
		recti const r = element_rect(e);
		frame.copy_from(m_static, r);
		switch (e.kind)
		{
		case element::button: draw_button(frame, e, key != 0 && (key & 1 || (key & 2))); break;
		case element::slider: draw_slider_knob(frame, e, ui.slider(e.control)); break;
		case element::knob: draw_contrast(frame, e, ui.contrast()); break;
		case element::led_lamp: draw_led(frame, e, key != 0); break;
		case element::lcd: draw_lcd(frame, ui); break;
		}
		m_drawn[i] = { true, key };
		dirty = unite(dirty, r);
	}
	dirty.x0 = std::max(dirty.x0, 0); dirty.y0 = std::max(dirty.y0, 0); dirty.x1 = std::min(dirty.x1, m_w); dirty.y1 = std::min(dirty.y1, m_h);
	return dirty;
}

// ---------------------------------------------------------------------------------------------- interaction
int panel_view::hit_test(float lx, float ly) const
{
	for (element const &e : m_elements)
	{
		if (e.control < 0) continue;
		if (e.kind == element::slider)
		{
			if (std::fabs(lx - e.x) <= e.r + 2 && ly >= e.y_top - e.r && ly <= e.y_bottom + e.r) return e.control;
		}
		else if (e.kind == element::button || e.kind == element::knob)
		{
			if (std::hypot(lx - e.x, ly - e.y) <= e.r + 3) return e.control;
		}
	}
	return -1;
}

bool panel_view::control_center(int control, float &lx, float &ly) const
{
	for (element const &e : m_elements)
		if (e.control == control) { lx = e.x; ly = e.kind == element::slider ? e.y_bottom : e.y; return true; }
	return false;
}

void panel_view::mouse_down(float x, float y, int button, panel_adapter &ui)
{
	if (button != 1 || m_capture >= 0) return;
	float lx, ly; if (!to_logical(x, y, lx, ly)) return;
	int const c = hit_test(lx, ly);
	if (c < 0) return;
	m_capture = c;
	panel_control const &k = panel_controls[c];
	if (k.kind == panel_kind::button) { ui.press_button(c); return; }
	for (element const &e : m_elements)
		if (e.control == c && e.kind == element::slider)
		{
			float const ky = float(e.y_bottom - ui.slider(c) * (e.y_bottom - e.y_top));
			// grabbing the knob keeps the offset; clicking the slot moves the knob there
			m_grab_dy = std::fabs(ly - ky) <= e.r ? ly - ky : 0.f;
			mouse_move(x, y, ui);
			return;
		}
	if (k.kind == panel_kind::knob) { m_drag_ly0 = ly; m_drag_v0 = ui.contrast(); m_knob_turned = false; }
}

void panel_view::mouse_move(float x, float y, panel_adapter &ui)
{
	if (m_capture < 0) return;
	float lx, ly; to_logical(x, y, lx, ly);
	panel_control const &k = panel_controls[m_capture];
	if (k.kind == panel_kind::slider)
	{
		for (element const &e : m_elements)
			if (e.control == m_capture && e.kind == element::slider)
				ui.set_slider(m_capture, std::clamp(double(e.y_bottom - (ly - m_grab_dy)) / double(e.y_bottom - e.y_top), 0.0, 1.0));
	}
	else if (k.kind == panel_kind::knob)
	{
		// a drag turns the knob (level within the theme); below the threshold it is still a click
		if (std::fabs(m_drag_ly0 - ly) >= knob_click_slop) m_knob_turned = true;
		if (m_knob_turned) ui.set_contrast(m_drag_v0 + (m_drag_ly0 - ly) / 120.0);
	}
}

void panel_view::mouse_up(float x, float y, int button, panel_adapter &ui)
{
	if (button != 1 || m_capture < 0) return;
	mouse_move(x, y, ui);
	if (panel_controls[m_capture].kind == panel_kind::button) ui.release_button(m_capture);
	else if (panel_controls[m_capture].kind == panel_kind::knob && !m_knob_turned) ui.next_display_theme();   // click: next display theme
	m_capture = -1;
}

void panel_view::wheel(float x, float y, int steps, panel_adapter &ui)
{
	float lx, ly; if (!to_logical(x, y, lx, ly)) return;
	int const c = hit_test(lx, ly);
	if (c < 0) return;
	if (panel_controls[c].kind == panel_kind::slider) ui.set_slider(c, ui.slider(c) + steps * (1.0 / 256.0));
	else if (panel_controls[c].kind == panel_kind::knob) ui.set_contrast(ui.contrast() + steps * 0.02);
}

void panel_view::focus_lost(panel_adapter &ui)
{
	if (m_capture >= 0 && panel_controls[m_capture].kind == panel_kind::button) ui.release_button(m_capture);
	m_capture = -1;
}

} // namespace eiii::ui
