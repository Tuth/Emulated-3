// X11 window for the panel — see x11_window.h.
#include "x11_window.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <cstring>

namespace eiii::ui {

struct x11_window::impl
{
	Display *dpy = nullptr;
	Window win = 0;
	GC gc = nullptr;
	XImage *img = nullptr;
	XShmSegmentInfo shm{};
	bool use_shm = false, shm_attached = false;
	int img_w = 0, img_h = 0;
	Atom wm_delete = 0, wm_protocols = 0;
	bool embedded = false;

	void free_image()
	{
		if (!img) return;
		if (shm_attached) { XShmDetach(dpy, &shm); XSync(dpy, False); shm_attached = false; }
		if (use_shm && shm.shmaddr) { shmdt(shm.shmaddr); shm.shmaddr = nullptr; }
		if (!use_shm && img->data) { free(img->data); img->data = nullptr; }
		img->data = nullptr;
		XDestroyImage(img); img = nullptr;
		img_w = img_h = 0;
	}
	bool make_image(int w, int h)
	{
		free_image();
		Visual *const vis = DefaultVisual(dpy, DefaultScreen(dpy));
		int const depth = DefaultDepth(dpy, DefaultScreen(dpy));
		if (use_shm)
		{
			img = XShmCreateImage(dpy, vis, unsigned(depth), ZPixmap, nullptr, &shm, unsigned(w), unsigned(h));
			if (img)
			{
				shm.shmid = shmget(IPC_PRIVATE, size_t(img->bytes_per_line) * h, IPC_CREAT | 0600);
				if (shm.shmid >= 0)
				{
					shm.shmaddr = img->data = static_cast<char *>(shmat(shm.shmid, nullptr, 0));
					shm.readOnly = False;
					shmctl(shm.shmid, IPC_RMID, nullptr);       // freed when both sides detach
					if (shm.shmaddr != reinterpret_cast<char *>(-1) && XShmAttach(dpy, &shm)) { XSync(dpy, False); shm_attached = true; img_w = w; img_h = h; return true; }
				}
				XDestroyImage(img); img = nullptr;
			}
			use_shm = false;                                   // fall back for good
		}
		char *data = static_cast<char *>(malloc(size_t(w) * h * 4));
		img = XCreateImage(dpy, vis, unsigned(depth), ZPixmap, 0, data, unsigned(w), unsigned(h), 32, 0);
		if (!img) { free(data); return false; }
		img_w = w; img_h = h;
		return true;
	}
};

x11_window::x11_window() : m(new impl) { }
x11_window::~x11_window() { close(); delete m; }
bool x11_window::is_open() const { return m->dpy && m->win; }
int x11_window::fd() const { return m->dpy ? ConnectionNumber(m->dpy) : -1; }
uintptr_t x11_window::window_id() const { return m->win; }

bool x11_window::open(uintptr_t parent, int w, int h, char const *title, std::string &error)
{
	close();
	m->dpy = XOpenDisplay(nullptr);
	if (!m->dpy) { error = "cannot open the X display (DISPLAY not set?)"; return false; }
	int const scr = DefaultScreen(m->dpy);
	if (DefaultDepth(m->dpy, scr) < 24) { error = "X display depth < 24 bits is not supported"; XCloseDisplay(m->dpy); m->dpy = nullptr; return false; }
	m->embedded = parent != 0;
	Window const par = parent ? Window(parent) : RootWindow(m->dpy, scr);
	XSetWindowAttributes a{};
	a.background_pixel = 0x141414;
	a.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | FocusChangeMask | KeyPressMask | KeyReleaseMask;
	m->win = XCreateWindow(m->dpy, par, 0, 0, unsigned(w), unsigned(h), 0, CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &a);
	m_w = w; m_h = h;
	m->gc = XCreateGC(m->dpy, m->win, 0, nullptr);
	if (!m->embedded)
	{
		XStoreName(m->dpy, m->win, title);
		XClassHint ch{ const_cast<char *>("eiii"), const_cast<char *>("EIII") };
		XSetClassHint(m->dpy, m->win, &ch);
		m->wm_protocols = XInternAtom(m->dpy, "WM_PROTOCOLS", False);
		m->wm_delete = XInternAtom(m->dpy, "WM_DELETE_WINDOW", False);
		XSetWMProtocols(m->dpy, m->win, &m->wm_delete, 1);
	}
	else
	{
		// XEmbed info (version 0, mapped) for hosts that use the XEmbed protocol; harmless otherwise
		Atom const xi = XInternAtom(m->dpy, "_XEMBED_INFO", False);
		long const info[2] = { 0, 1 };
		XChangeProperty(m->dpy, m->win, xi, xi, 32, PropModeReplace, reinterpret_cast<unsigned char const *>(info), 2);
	}
	Bool supported = False;
	XkbSetDetectableAutoRepeat(m->dpy, True, &supported);
	char const *const ds = DisplayString(m->dpy);
	m->use_shm = XShmQueryExtension(m->dpy) && ds && ds[0] == ':';
	if (!m->make_image(w, h)) { error = "cannot create the X image"; close(); return false; }
	XMapWindow(m->dpy, m->win);
	XFlush(m->dpy);
	return true;
}

void x11_window::close()
{
	if (!m->dpy) return;
	m->free_image();
	if (m->gc) XFreeGC(m->dpy, m->gc);
	if (m->win) XDestroyWindow(m->dpy, m->win);
	XSync(m->dpy, False);
	XCloseDisplay(m->dpy);
	m->dpy = nullptr; m->win = 0; m->gc = nullptr;
}

void x11_window::show(bool v)
{
	if (!is_open()) return;
	if (v) XMapWindow(m->dpy, m->win); else XUnmapWindow(m->dpy, m->win);
	XFlush(m->dpy);
}

void x11_window::set_size(int w, int h)
{
	if (!is_open() || (w == m_w && h == m_h)) return;
	m_w = w; m_h = h;
	XResizeWindow(m->dpy, m->win, unsigned(w), unsigned(h));
	XFlush(m->dpy);
}

void x11_window::set_aspect(int w, int h)
{
	if (!is_open() || m->embedded) return;
	XSizeHints *sh = XAllocSizeHints();
	sh->flags = PAspect | PMinSize;
	sh->min_aspect.x = sh->max_aspect.x = w; sh->min_aspect.y = sh->max_aspect.y = h;
	sh->min_width = w / 4; sh->min_height = h / 4;
	XSetWMNormalHints(m->dpy, m->win, sh);
	XFree(sh);
}

void x11_window::pump()
{
	if (!is_open()) return;
	while (XPending(m->dpy))
	{
		XEvent e; XNextEvent(m->dpy, &e);
		switch (e.type)
		{
		case Expose: if (e.xexpose.count == 0 && cb.expose) cb.expose(); break;
		case ConfigureNotify:
			if (e.xconfigure.width != m_w || e.xconfigure.height != m_h)
			{
				m_w = e.xconfigure.width; m_h = e.xconfigure.height;
				if (cb.resized) cb.resized(m_w, m_h);
			}
			break;
		case ButtonPress:
			if (e.xbutton.button == Button4 || e.xbutton.button == Button5) { if (cb.wheel) cb.wheel(e.xbutton.x, e.xbutton.y, e.xbutton.button == Button4 ? 1 : -1); }
			else if (cb.down) cb.down(e.xbutton.x, e.xbutton.y, int(e.xbutton.button));
			break;
		case ButtonRelease:
			if (e.xbutton.button != Button4 && e.xbutton.button != Button5 && cb.up) cb.up(e.xbutton.x, e.xbutton.y, int(e.xbutton.button));
			break;
		case MotionNotify: if (cb.move) cb.move(e.xmotion.x, e.xmotion.y); break;
		case FocusOut: if (cb.focus_out) cb.focus_out(); break;
		case KeyPress: case KeyRelease:
			if (cb.key) cb.key(XLookupKeysym(&e.xkey, 0), e.type == KeyPress);
			break;
		case ClientMessage:
			if (Atom(e.xclient.data.l[0]) == m->wm_delete && cb.close_request) cb.close_request();
			break;
		default: break;
		}
	}
}

void x11_window::present(canvas const &c, recti r)
{
	if (!is_open() || r.empty()) return;
	if (m->img_w != c.w || m->img_h != c.h) { if (!m->make_image(c.w, c.h)) return; r = { 0, 0, c.w, c.h }; }
	r.x0 = std::max(0, r.x0); r.y0 = std::max(0, r.y0); r.x1 = std::min(c.w, r.x1); r.y1 = std::min(c.h, r.y1);
	if (r.empty()) return;
	XImage *img = m->img;
	for (int y = r.y0; y < r.y1; ++y)
		memcpy(img->data + size_t(y) * img->bytes_per_line + size_t(r.x0) * 4, &c.px[size_t(y) * c.w + r.x0], size_t(r.x1 - r.x0) * 4);
	if (m->use_shm) { XShmPutImage(m->dpy, m->win, m->gc, img, r.x0, r.y0, r.x0, r.y0, unsigned(r.x1 - r.x0), unsigned(r.y1 - r.y0), False); XSync(m->dpy, False); }
	else { XPutImage(m->dpy, m->win, m->gc, img, r.x0, r.y0, r.x0, r.y0, unsigned(r.x1 - r.x0), unsigned(r.y1 - r.y0)); XFlush(m->dpy); }
}

} // namespace eiii::ui
