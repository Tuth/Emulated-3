// LCD presentation — see eiii_lcd.h.
#include "eiii_lcd.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace eiii {

namespace {
uint8_t const k_font[0x60][8] = {
#include "eiii_lcd_font.inc"
};
constexpr double k_blink_half_period = 102400.0 / 270000.0;   // MAME hd44780: blink toggles every 102400 clocks
}

int lcd_ddram_address(int row, int col, int disp_shift, int num_lines)
{
	if (row < 0 || row >= lcd_view::rows || col < 0 || col >= lcd_view::cols) return -1;
	// MAME 4-line fold: position p of controller line L is drawn at row L (p < 20) or row L + 2 (20 <= p < 40)
	int const line = row & 1, pos = col + (row >= 2 ? lcd_view::cols : 0);
	if (num_lines == 1)
	{
		if (line == 1) return -1;                       // one 80-character line: rows 2 and 4 stay empty
		return (pos + disp_shift) % 80;
	}
	return line * 0x40 + (pos + disp_shift) % 40;
}

lcd_font::lcd_font() = default;

bool lcd_font::load_cgrom(std::string const &path, std::string &error)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) { error = "cannot open " + path; return false; }
	std::vector<uint8_t> b(4096);
	size_t const n = fread(b.data(), 1, b.size(), f); fclose(f);
	if (n != b.size()) { error = path + ": expected a 4096-byte HD44780 character ROM"; return false; }
	bool any = false; for (uint8_t v : b) any |= v != 0;
	if (!any) { error = path + ": all zero (placeholder, not a character ROM dump)"; return false; }
	m_rom = std::move(b);
	return true;
}

void lcd_font::glyph(lcd_state const &s, uint8_t code, uint8_t rows[8]) const
{
	if (code < 0x10) { memcpy(rows, s.cgram + (code & 7) * 8, 8); for (int i = 0; i < 8; ++i) rows[i] &= 0x1f; return; }
	if (!m_rom.empty()) { for (int i = 0; i < 8; ++i) rows[i] = m_rom[code * 16 + i] & 0x1f; return; }
	if (code >= 0x20 && code < 0x80) { memcpy(rows, k_font[code - 0x20], 8); return; }
	memset(rows, 0, 8);
}

void lcd_present(lcd_state const &s, double emu_seconds, lcd_font const &font, lcd_view &out)
{
	out.display_on = s.display_on;
	out.cursor_row = out.cursor_col = -1;
	bool const blink_phase_off = (long(std::floor(emu_seconds / k_blink_half_period)) & 1) == 0;   // MAME m_blink == false
	for (int r = 0; r < lcd_view::rows; ++r)
		for (int c = 0; c < lcd_view::cols; ++c)
		{
			int const a = lcd_ddram_address(r, c, s.disp_shift, s.num_lines);
			uint8_t const code = a < 0 ? 0x20 : s.ddram[a & 0x7f];
			out.code[r][c] = code;
			uint8_t *d = out.dots[r][c];
			if (!s.display_on || a < 0) { memset(d, 0, 8); continue; }
			font.glyph(s, code, d);
			if (a == s.ac)
			{
				out.cursor_row = r; out.cursor_col = c;
				if (s.cursor_on) d[7] = 0x1f;                         // underline cursor
				if (s.blink_on && blink_phase_off) memset(d, 0x1f, 8); // blinking block
			}
		}
}

std::string lcd_row_text(lcd_view const &v, int row)
{
	std::string t;
	for (int c = 0; c < lcd_view::cols; ++c) { uint8_t const k = v.code[row][c]; t += (k >= 0x20 && k < 0x7e) ? char(k) : '.'; }
	return t;
}

std::string lcd_line_text(lcd_state const &s, int line)
{
	std::string t;
	for (int i = 0; i < 40; ++i) { uint8_t const k = s.ddram[line * 0x40 + i]; t += (k >= 32 && k < 127) ? char(k) : '.'; }
	return t;
}

} // namespace eiii
