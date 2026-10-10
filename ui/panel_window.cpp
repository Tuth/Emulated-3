// panel_window — see panel_window.h.
#include "panel_window.h"
#include <X11/keysym.h>
#include <cstdio>

namespace eiii::ui {

bool panel_window::open(unsigned long parent, int w, int h, std::string &error)
{
	m_close = false;
	m_view.set_size(w, h);
	if (!m_win.open(parent, w, h, "E-mu Emulator III", error)) return false;
	if (!parent) m_win.set_aspect(int(panel_w), int(panel_view::total_h(m_view.has_menu_bar())));
	m_win.cb.down = [this](int x, int y, int b) { m_view.mouse_down(float(x), float(y), b, m_ui); };
	m_win.cb.up = [this](int x, int y, int b) { m_view.mouse_up(float(x), float(y), b, m_ui); };
	m_win.cb.move = [this](int x, int y) { m_view.mouse_move(float(x), float(y), m_ui); };
	m_win.cb.wheel = [this](int x, int y, int s) { m_view.wheel(float(x), float(y), s, m_ui); };
	m_win.cb.focus_out = [this] { m_view.focus_lost(m_ui); m_ui.release_all(); };
	m_win.cb.expose = [this] { m_full = true; };
	m_win.cb.resized = [this](int nw, int nh) { m_view.set_size(nw, nh); m_full = true; };
	m_win.cb.close_request = [this] { m_close = true; };
	m_win.cb.key = [this](unsigned long k, bool d) { if (keyboard_shortcuts) on_key(k, d); };
	m_full = true;
	return true;
}

void panel_window::set_size(int w, int h)
{
	m_win.set_size(w, h);
	m_view.set_size(w, h);
	m_full = true;
}

void panel_window::tick()
{
	if (!m_win.is_open()) return;
	m_win.pump();
	m_ui.poll();
	recti const r = m_view.render(m_frame, m_ui, m_full);
	if (m_full) m_win.present(m_frame, { 0, 0, m_frame.w, m_frame.h });
	else if (!r.empty()) m_win.present(m_frame, r);
	m_full = false;
}

void panel_window::on_key(unsigned long k, bool down)
{
	char const *id = nullptr;
	char digit[16];
	if (k >= XK_0 && k <= XK_9) { snprintf(digit, sizeof digit, "keypad_%d", int(k - XK_0)); id = digit; }
	else if (k >= XK_KP_0 && k <= XK_KP_9) { snprintf(digit, sizeof digit, "keypad_%d", int(k - XK_KP_0)); id = digit; }
	else switch (k)
	{
	case XK_Return: case XK_KP_Enter: id = "enter"; break;
	case XK_Up: id = "cursor_up"; break;
	case XK_Down: id = "cursor_down"; break;
	case XK_Left: id = "cursor_left"; break;
	case XK_Right: id = "cursor_right"; break;
	case XK_plus: case XK_equal: case XK_KP_Add: id = "yes"; break;
	case XK_minus: case XK_KP_Subtract: id = "no"; break;
	default: return;
	}
	int const c = panel_find_control(id);
	if (down) m_ui.press_button(c); else m_ui.release_button(c);
}

} // namespace eiii::ui
