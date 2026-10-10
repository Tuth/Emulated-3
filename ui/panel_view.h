// The EIII 4U rack front panel as vector artwork + interaction.  Logical coordinates = panel units (1472 x 538 for the
// whole panel including rack ears, ~0.328 mm each); the view scales them to any device size (aspect kept, letterboxed)
// and re-rasterises the artwork at each size.  Talks to the machine only through eiii::panel_adapter.
#pragma once
#include "raster.h"
#include "panel/eiii_panel_adapter.h"
#include <array>
#include <functional>
#include <vector>

namespace eiii::ui {

constexpr float panel_w = 1472.f, panel_h = 538.f;
constexpr float knob_click_slop = 3.f;              // panel units of vertical travel before a Contrast click becomes a turn

// how the 4 x 20 display is drawn for a theme at a Contrast level (see lcd_look_for in panel_view.cpp)
struct lcd_look
{
	rgba bg, ink, ghost;                            // window, lit dots, unlit dots (alpha 0: not drawn)
	float shade = 1.f;                              // strength of the window's top/left shading
	float glow = 0, glow_sigma = 1, glow_gain = 1;  // halo opacity, blur radius (panel units), map gain
	rgba glow_col;
	float dot_round = 0.25f / 1.62f;                      // dot corner radius / dot size
	float reflect = 0.05f;                          // glass reflection strength
};
lcd_look lcd_look_for(lcd_theme t, float level);

class panel_view
{
public:
	panel_view();
	void set_size(int w, int h);                    // device pixels
	int width() const { return m_w; }
	int height() const { return m_h; }
	float scale() const { return m_s; }
	static void preferred_size(double scale_percent, int &w, int &h);   // 100 % = 1472 x 538

	// draw what changed since the last call (everything when full); returns the device rectangle that changed
	recti render(canvas &frame, panel_adapter &ui, bool full);

	// pointer input in device pixels; button 1 = left
	void mouse_down(float x, float y, int button, panel_adapter &ui);
	void mouse_move(float x, float y, panel_adapter &ui);
	void mouse_up(float x, float y, int button, panel_adapter &ui);
	void wheel(float x, float y, int steps, panel_adapter &ui);
	void focus_lost(panel_adapter &ui);

	// host UI: a click on the drive area of the panel (not a machine input); eject = on the drive's eject button
	std::function<void(bool eject)> on_drive_click;

	int hit_test(float lx, float ly) const;         // control index at a logical point, -1 if none
	bool to_logical(float x, float y, float &lx, float &ly) const;
	bool control_center(int control, float &lx, float &ly) const;   // logical centre of a control (tests)

	struct element
	{
		int control = -1, led = -1;
		enum kind_t { button, slider, knob, led_lamp, lcd, drive } kind;
		float x = 0, y = 0, r = 0;                  // buttons/knobs: centre, radius; LEDs: centre
		float y_top = 0, y_bottom = 0;              // sliders: knob centre travel
	};
private:
	struct xf { float s, ox, oy; float X(float lx) const { return ox + lx * s; } float Y(float ly) const { return oy + ly * s; } };
	void draw_static(canvas &c);
	void draw_button(canvas &c, element const &e, bool down);
	void draw_slider_knob(canvas &c, element const &e, double pos);
	void draw_contrast(canvas &c, element const &e, double v);
	void draw_led(canvas &c, element const &e, bool on);
	void draw_lcd(canvas &c, panel_adapter &ui);
	void draw_drive_label(canvas &c, std::string const &label);
	template <class LIT, class DX, class DY> void draw_lcd_glow(canvas &c, lcd_look const &L, LIT lit, DX dot_x, DY dot_y, float ds, recti box);
	recti element_rect(element const &e) const;
	void text(canvas &c, font &f, float lx, float ly_baseline, char const *s, float size, rgba col, float tracking = 0, int align = 0);

	int m_w = 0, m_h = 0;
	float m_s = 1;
	xf m_xf{ 1, 0, 0 };
	ui_fonts m_fonts;
	canvas m_static;
	bool m_static_ok = false;
	std::vector<element> m_elements;
	int m_lcd_element = -1;
	// last drawn state per element (for dirty tracking)
	struct drawn { bool valid = false; uint32_t key = 0; };
	std::vector<drawn> m_drawn;
	// pointer capture
	int m_capture = -1; float m_grab_dy = 0; float m_drag_ly0 = 0; double m_drag_v0 = 0; bool m_knob_turned = false;
	std::vector<float> m_glow_map, m_glow_tmp;      // LCD glow scratch (GUI thread)
};

} // namespace eiii::ui
