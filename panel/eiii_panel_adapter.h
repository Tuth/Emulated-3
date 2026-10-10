// PanelUI adapter: the only interface between a front-panel view and an emulated EIII.
//   press_button / release_button -> scanner make/break or extra-switch level (hardware inputs)
//   set_slider                    -> ADC multiplexer input
//   read_lcd / read_leds          <- the controller's LCD RAM and the LED latches, as published by the engine
// Host-only controls (POWER, Contrast) act on the host (engine power / LCD rendering: Contrast level + display theme).  The adapter has no operation
// that reads or writes emulated memory, voices or presets.  Use from one thread (the GUI/main thread).
#pragma once
#include "eiii_lcd.h"
#include "eiii_panel_model.h"
#include "runtime/eiii_engine.h"
#include <string>
#include <vector>

namespace eiii {

// Rendering themes of the 4 x 20 display (host UI only: chosen by clicking the Contrast knob).  Each theme keeps its
// own level (turning the knob: contrast for Stock, backlight / brightness / glow for the others).
enum class lcd_theme { stock, blue_white, amber_red, vfd, oled };
constexpr int lcd_theme_count = 5;
char const *lcd_theme_name(lcd_theme t);                  // "stock", "blue", "amber", "vfd", "oled"
bool lcd_theme_from_name(char const *name, lcd_theme &t);

class panel_adapter
{
public:
	explicit panel_adapter(engine &e);

	bool press_button(int control);           // mouse-down: make
	bool release_button(int control);         // mouse-up: break
	bool button_down(int control) const;
	void release_all();                       // e.g. window lost focus with a button held
	bool set_slider(int control, double position01);   // 0 = bottom of travel, 1 = top
	double slider(int control) const;

	bool poll();                              // fetch the latest published state; true if it changed
	panel_snapshot const &snapshot() const { return m_snap; }
	void read_lcd(lcd_view &out) const;       // the 4 x 20 display as the module shows it
	uint32_t read_leds() const;               // bit i set = panel_leds[i] lit
	bool led(int index) const;

	// host UI
	bool powered() const;
	void set_contrast(double c) { m_level[int(m_theme)] = c < 0 ? 0 : c > 1 ? 1 : c; }   // level of the current theme
	double contrast() const { return m_level[int(m_theme)]; }
	lcd_theme display_theme() const { return m_theme; }
	void set_display_theme(lcd_theme t) { if (int(t) >= 0 && int(t) < lcd_theme_count) m_theme = t; }
	void next_display_theme() { m_theme = lcd_theme((int(m_theme) + 1) % lcd_theme_count); }
	double theme_level(lcd_theme t) const { return m_level[int(t)]; }
	// display settings as text lines ("lcd_theme NAME", "lcd_level NAME V") for the GUI / CLAP state;
	// apply_display_state_line() returns false for lines that are not display settings
	std::string display_state() const;
	bool apply_display_state_line(char const *line);
	lcd_font &font() { return m_font; }
	// host UI: what the drive area of the panel shows (media in use), one item per line; empty = nothing drawn
	void set_media_label(std::string const &text) { m_media_label = text; }
	std::string const &media_label() const { return m_media_label; }

private:
	engine &m_e;
	panel_snapshot m_snap{};
	lcd_font m_font;
	std::vector<double> m_slider;
	std::vector<bool> m_down;
	lcd_theme m_theme = lcd_theme::stock;
	std::string m_media_label;
	double m_level[lcd_theme_count] = { 0.55, 0.7, 0.7, 0.6, 0.8 };
};

} // namespace eiii
