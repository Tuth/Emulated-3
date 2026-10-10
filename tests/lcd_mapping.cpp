// LCD presentation unit test: HD44780 2-line DDRAM -> 4 x 20 module rows, display shift, cursor, blink, CGRAM,
// 1-line mode, and that presentation is a pure function of the snapshot (no state anywhere else is touched).
#include "panel/eiii_lcd.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace eiii;
static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); ++fails; } printf("\n"); } while (0)

static lcd_state blank()
{
	lcd_state s; memset(&s, 0, sizeof s);
	memset(s.ddram, 0x20, sizeof s.ddram);
	s.display_on = true; s.num_lines = 2;
	return s;
}
static void put(lcd_state &s, int addr, char const *t) { for (int i = 0; t[i]; ++i) s.ddram[(addr + i) & 0x7f] = uint8_t(t[i]); }

int main()
{
	lcd_font font;
	// the firmware's Load Bank screen (DDRAM as OS 2.42 writes it, captured from the runtime)
	lcd_state s = blank();
	put(s, 0x00, "   LOAD BANK from   "); put(s, 0x14, "B44 Tenor Sax       ");
	put(s, 0x40, "D3 Untitled Drive   "); put(s, 0x54, "   Select a Bank    ");
	s.ac = 0x7f;
	lcd_view v; lcd_present(s, 0, font, v);
	CHECK(lcd_row_text(v, 0) == "   LOAD BANK from   ", "row 1 = DDRAM 0x00-0x13: [%s]", lcd_row_text(v, 0).c_str());
	CHECK(lcd_row_text(v, 1) == "D3 Untitled Drive   ", "row 2 = DDRAM 0x40-0x53: [%s]", lcd_row_text(v, 1).c_str());
	CHECK(lcd_row_text(v, 2) == "B44 Tenor Sax       ", "row 3 = DDRAM 0x14-0x27 (bank on line three, as the manual): [%s]", lcd_row_text(v, 2).c_str());
	CHECK(lcd_row_text(v, 3) == "   Select a Bank    ", "row 4 = DDRAM 0x54-0x67: [%s]", lcd_row_text(v, 3).c_str());
	CHECK(lcd_ddram_address(0, 0, 0, 2) == 0x00 && lcd_ddram_address(1, 19, 0, 2) == 0x53 && lcd_ddram_address(2, 0, 0, 2) == 0x14 && lcd_ddram_address(3, 19, 0, 2) == 0x67,
		"address map corners 00/53/14/67");

	// display shift (MAME: position p shows DDRAM line*0x40 + (p + shift) % 40)
	CHECK(lcd_ddram_address(0, 0, 1, 2) == 0x01 && lcd_ddram_address(2, 19, 1, 2) == 0x00 && lcd_ddram_address(3, 19, 39, 2) == 0x66, "display shift wraps within each 40-character line");
	CHECK(lcd_ddram_address(1, 0, 0, 1) == -1 && lcd_ddram_address(2, 0, 0, 1) == 0x14, "1-line mode: rows 2/4 empty, row 3 continues line 1");

	// glyphs: 'A' from the built-in set, CGRAM for 0x00-0x07 and the 0x08-0x0F mirror
	uint8_t g[8]; font.glyph(s, 'A', g);
	CHECK(g[0] == 0x0e && g[4] == 0x1f && g[7] == 0x00, "built-in glyph 'A' (row 7 free for the cursor)");
	for (int i = 0; i < 8; ++i) s.cgram[3 * 8 + i] = uint8_t(0xe0 | (i * 3));   // bits above 4 must be ignored
	font.glyph(s, 0x03, g); uint8_t g2[8]; font.glyph(s, 0x0b, g2);
	CHECK(g[1] == 0x03 && g[7] == (21 & 0x1f) && !memcmp(g, g2, 8), "CGRAM glyph 3 = code 0x0B, 5 bits only");

	// cursor at the address counter: underline when C is set; block on the blink phase when B is set
	s.ac = 0x16; s.cursor_on = true; s.blink_on = false;
	lcd_present(s, 0, font, v);
	CHECK(v.cursor_row == 2 && v.cursor_col == 2 && v.dots[2][2][7] == 0x1f, "underline cursor at AC 0x16 -> row 3, column 3");
	s.cursor_on = false; s.blink_on = true;
	lcd_view a, b; lcd_present(s, 0.10, font, a); lcd_present(s, 0.50, font, b);
	bool const block_a = a.dots[2][2][0] == 0x1f && a.dots[2][2][3] == 0x1f, block_b = b.dots[2][2][0] == 0x1f && b.dots[2][2][3] == 0x1f;
	CHECK(block_a != block_b, "hardware blink alternates every 102400 / 270 kHz = 0.379 s (t=0.10: %d, t=0.50: %d)", block_a, block_b);
	s.display_on = false; lcd_present(s, 0, font, v);
	bool dark = true; for (auto &r : v.dots) for (auto &c : r) for (uint8_t d : c) dark &= d == 0;
	CHECK(dark, "display off: no dots");

	// presentation is a pure function: same snapshot in, same view out, snapshot unchanged
	lcd_state const before = s; lcd_present(s, 1.0, font, a); lcd_present(s, 1.0, font, b);
	CHECK(!memcmp(&before, &s, sizeof s) && !memcmp(&a, &b, sizeof a), "presentation does not modify the controller state and is deterministic");

	std::string err; CHECK(!font.load_cgrom("/dev/null", err), "a non-ROM file is refused as character ROM (%s)", err.c_str());
	printf("%s: %d failure(s)\n", fails ? "FAIL" : "OK", fails);
	return fails ? 1 : 0;
}
