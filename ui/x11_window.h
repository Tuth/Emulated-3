// X11 window for the panel: a top-level window (standalone --gui) or a child of a host window (CLAP embedding).
// Each window owns its own Display connection, so plugin instances never share X state; nothing here blocks (the
// owner pumps events when fd() is readable or on a timer).  Blits come from a 0x00RRGGBB canvas (MIT-SHM when the
// display is local, XPutImage otherwise).
#pragma once
#include "raster.h"
#include <functional>
#include <string>

namespace eiii::ui {

class x11_window
{
public:
	struct callbacks
	{
		std::function<void(int x, int y, int button)> down, up;
		std::function<void(int x, int y)> move;
		std::function<void(int x, int y, int steps)> wheel;
		std::function<void(int w, int h)> resized;
		std::function<void()> expose, close_request, focus_out, tick;
		std::function<void(std::string const &)> error;
		std::function<void(int w,int h)> resize_requested;
		std::function<void()> resize_finished;
		std::function<void(unsigned long keysym, bool down)> key;
	};
	x11_window();
	~x11_window();
	x11_window(x11_window const &) = delete;
	x11_window &operator=(x11_window const &) = delete;

	// parent = 0: top-level window with this title and a fixed aspect ratio; else a child of the host's window
	bool open(uintptr_t parent, int w, int h, char const *title, std::string &error);
	void close();
	bool is_open() const;
	int fd() const;
	uintptr_t window_id() const;
	void pump();                                      // handle all pending events (non-blocking)
	void present(canvas const &c, recti r);           // copy a region of the canvas to the window
	void set_size(int w, int h);
	void show(bool visible);
	void set_aspect(int w, int h);                    // top-level only: keep this aspect ratio when resized
	int width() const { return m_w; }
	int height() const { return m_h; }
	callbacks cb;
private:
	struct impl;
	impl *m;
	int m_w = 0, m_h = 0;
};

} // namespace eiii::ui
