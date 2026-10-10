// PanelUI adapter — see eiii_panel_adapter.h.
#include "eiii_panel_adapter.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace eiii {

namespace { char const *const k_theme_names[lcd_theme_count] = { "stock", "blue", "amber", "vfd", "oled" }; }

char const *lcd_theme_name(lcd_theme t) { return int(t) >= 0 && int(t) < lcd_theme_count ? k_theme_names[int(t)] : "stock"; }

bool lcd_theme_from_name(char const *name, lcd_theme &t)
{
	for (int i = 0; i < lcd_theme_count; ++i) if (!strcmp(name, k_theme_names[i])) { t = lcd_theme(i); return true; }
	return false;
}

panel_adapter::panel_adapter(engine &e) : m_e(e), m_slider(panel_control_count, 0.0), m_down(panel_control_count, false)
{
	for (size_t i = 0; i < panel_control_count; ++i)
		if (panel_controls[i].kind == panel_kind::slider) m_slider[i] = adc_to_slider(slider_default_adc(int(i)));
	if (char const *rom = getenv("EIII_LCD_CGROM")) { std::string err; m_font.load_cgrom(rom, err); }
}

bool panel_adapter::press_button(int c)
{
	if (c < 0 || size_t(c) >= panel_control_count || m_down[c]) return false;
	panel_control const &k = panel_controls[c];
	if (k.kind != panel_kind::button) return false;
	m_down[c] = true;
	switch (k.route)
	{
	case panel_route::scan: return m_e.post({ panel_input::SCAN, k.code, 1, 0 });
	case panel_route::xsw:  return m_e.post({ panel_input::XSW, k.code, 1, 0 });
	case panel_route::host: if (!strcmp(k.id, "power")) m_e.power(!m_e.powered()); return true;   // push-on/push-off
	default: return false;
	}
}

bool panel_adapter::release_button(int c)
{
	if (c < 0 || size_t(c) >= panel_control_count || !m_down[c]) return false;
	panel_control const &k = panel_controls[c];
	m_down[c] = false;
	switch (k.route)
	{
	case panel_route::scan: return m_e.post({ panel_input::SCAN, k.code, 0, 0 });
	case panel_route::xsw:  return m_e.post({ panel_input::XSW, k.code, 0, 0 });
	default: return true;
	}
}

bool panel_adapter::button_down(int c) const { return c >= 0 && size_t(c) < panel_control_count && m_down[c]; }

void panel_adapter::release_all()
{
	for (size_t i = 0; i < panel_control_count; ++i) if (m_down[i]) release_button(int(i));
}

bool panel_adapter::set_slider(int c, double p)
{
	if (c < 0 || size_t(c) >= panel_control_count || panel_controls[c].kind != panel_kind::slider) return false;
	p = p < 0 ? 0 : p > 1 ? 1 : p;
	uint16_t const v = slider_to_adc(p);
	bool const changed = v != slider_to_adc(m_slider[c]);
	m_slider[c] = p;
	if (!changed) return true;
	return m_e.post({ panel_input::ADC, panel_controls[c].code, 0, v });
}

double panel_adapter::slider(int c) const { return (c >= 0 && size_t(c) < panel_control_count) ? m_slider[c] : 0.0; }

bool panel_adapter::poll() { return m_e.snapshot(m_snap); }

void panel_adapter::read_lcd(lcd_view &out) const { lcd_present(m_snap.lcd, m_snap.emu_seconds, m_font, out); }

bool panel_adapter::led(int i) const
{
	if (i < 0 || size_t(i) >= panel_led_count || !m_snap.running) return false;
	panel_led const &l = panel_leds[i];
	unsigned const latch = l.source == led_source::led_latch ? m_snap.led_latch : m_snap.misc_latch;
	return !((latch >> l.bit) & 1);   // active low
}

uint32_t panel_adapter::read_leds() const
{
	uint32_t r = 0;
	for (size_t i = 0; i < panel_led_count; ++i) if (led(int(i))) r |= 1u << i;
	return r;
}

bool panel_adapter::powered() const { return m_e.powered(); }

std::string panel_adapter::display_state() const
{
	std::string t = std::string("lcd_theme ") + lcd_theme_name(m_theme) + "\n";
	char line[64];
	for (int i = 0; i < lcd_theme_count; ++i) { snprintf(line, sizeof line, "lcd_level %s %.4f\n", k_theme_names[i], m_level[i]); t += line; }
	return t;
}

bool panel_adapter::apply_display_state_line(char const *ln)
{
	char name[32]; double v; lcd_theme t;
	if (sscanf(ln, "lcd_theme %31s", name) == 1) { if (lcd_theme_from_name(name, t)) m_theme = t; return true; }
	if (sscanf(ln, "lcd_level %31s %lf", name, &v) == 2) { if (lcd_theme_from_name(name, t)) m_level[int(t)] = v < 0 ? 0 : v > 1 ? 1 : v; return true; }
	if (sscanf(ln, "contrast %lf", &v) == 1) { set_contrast(v); return true; }   // state written before themes existed
	return false;
}

} // namespace eiii
