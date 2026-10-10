// LCD presentation: the EIII's HD44780 (2-line mode, 40 characters per line) as the 4 x 20 module shows it.
// Display only — reads an lcd_state snapshot, never the runtime.  Semantics follow MAME's hd44780_base_device
// (render + 4-line pixel fold), see analysis/EIII_PANEL_UI_ARCHITECTURE.md § LCD for the verified row mapping:
//   row 1 = DDRAM 0x00-0x13, row 2 = 0x40-0x53, row 3 = 0x14-0x27, row 4 = 0x54-0x67.
#pragma once
#include "runtime/eiii_runtime.h"
#include <cstdint>
#include <string>
#include <vector>

namespace eiii {

struct lcd_view
{
	static constexpr int rows = 4, cols = 20;
	bool display_on = false;
	uint8_t code[rows][cols] = {};       // character code shown in each cell
	uint8_t dots[rows][cols][8] = {};    // 5x8 dot rows of each cell (bit 4 = leftmost), cursor/blink applied
	int cursor_row = -1, cursor_col = -1; // cell at the address counter (if visible), else -1
};

// DDRAM address shown at display row/col, or -1 when that cell shows nothing (1-line mode beyond 40 characters).
int lcd_ddram_address(int row, int col, int disp_shift, int num_lines);

// character generator: CGRAM for codes 0x00-0x0F (5x8 mode), else a CGROM.  The built-in CGROM is an original
// A00-layout set for 0x20-0x7F; load_cgrom() accepts a real 4096-byte HD44780 A00 dump (MAME layout, 16 bytes per
// code) to show 0x80-0xFF as well.  Codes without a glyph are drawn blank.
class lcd_font
{
public:
	lcd_font();
	bool load_cgrom(std::string const &path, std::string &error);
	bool has_external_rom() const { return !m_rom.empty(); }
	void glyph(lcd_state const &s, uint8_t code, uint8_t rows[8]) const;
private:
	std::vector<uint8_t> m_rom;
};

// Fill `out` from the controller state.  emu_seconds drives the hardware blink phase (102400 controller clocks at
// 270 kHz per half period, as MAME); OS 2.42 never enables hardware blink (it toggles the underline cursor itself).
void lcd_present(lcd_state const &s, double emu_seconds, lcd_font const &font, lcd_view &out);

// Row text with codes outside 0x20-0x7D shown as '.', for logs and tests.
std::string lcd_row_text(lcd_view const &v, int row);
// Controller line (0/1) as the 40-character DDRAM text the firmware wrote (same form as the standalone's LCD logs).
std::string lcd_line_text(lcd_state const &s, int line);

} // namespace eiii
