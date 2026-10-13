// panel_window: the rack panel in an X11 window (top-level for the standalone, embedded for the CLAP plugin).
// tick() handles pending events and redraws what changed; call it from the GUI thread on a timer (~30-60 Hz) and/or
// when fd() becomes readable.  All machine interaction goes through the panel_adapter.
#pragma once
#include "panel_view.h"
#include "x11_window.h"

namespace eiii::ui {

class panel_window
{
public:
	explicit panel_window(panel_adapter &ui) : m_ui(ui) { }
	bool open(uintptr_t parent, int w, int h, std::string &error);
	void close() { m_win.close(); }
	bool is_open() const { return m_win.is_open(); }
	uintptr_t native_handle() const { return m_win.window_id(); }
	int fd() const { return m_win.fd(); }
	void tick();
	void set_size(int w, int h);
	void show(bool visible) { m_win.show(visible); }
	int width() const { return m_view.width(); }
	int height() const { return m_view.height(); }
	bool close_requested() const { return m_close; }
	std::function<void()> on_tick;
	std::function<void(int w,int h)> on_resize_request;
	std::function<void()> on_resize_finish;
	bool keyboard_shortcuts = true;             // digits, Return, arrows, +/-  -> panel buttons (top-level window)
	panel_view &view() { return m_view; }
	canvas const &frame() const { return m_frame; }
private:
	void on_key(unsigned long keysym, bool down);
	panel_adapter &m_ui;
	panel_view m_view;
	x11_window m_win;
	canvas m_frame;
	bool m_full = true, m_close = false;
};

} // namespace eiii::ui
