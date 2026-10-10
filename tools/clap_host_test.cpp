// clap_host_test: a minimal CLAP host for the EIII Rack plugin's GUI/threading regressions.
//   * N instances, each with its own realtime audio thread (256-frame blocks at 48 kHz, MIDI note events on ch 14,
//     a quiet tone on the Sample In port)
//   * main thread = GUI thread: host timers + posix fds, GUI create / set_parent / show / resize / destroy cycles
//   * rapid button presses and slider drags delivered as X events to the plugin's window while notes play
//   * --media-test: the menu bar and the drive area: eject button, drive click -> D3 menu (quick disc list, Change
//     disc... -> scripted chooser), Settings -> analog model V2 / V1, checked through the saved state; states with
//     another disc state / an extra CD drive at SCSI 4 restored into a second instance
// usage: clap_host_test PLUGIN.clap [--instances N] [--seconds S] [--gui-cycles N] [--media-test] [--report FILE]
// Media through the plugin's own configuration (EIII_ROM_DIR, EIII_HD, EIII_CD, EIII_AUTOLOAD_*).
#include "clap/clap.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <memory>
#include <map>
#include <regex>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::thread::id g_main;
float const bar_h = 26.f;                // the plugin's menu bar above the panel (panel units), eiii::ui::menu_bar_h
struct plug;
thread_local plug *t_audio_owner = nullptr;

struct timer_rec { uint32_t period_ms; std::chrono::steady_clock::time_point next; };
struct plug
{
	clap_host_t host{};
	clap_plugin_t const *p = nullptr;
	clap_plugin_gui_t const *gui = nullptr;
	clap_plugin_timer_support_t const *timer = nullptr;
	clap_plugin_posix_fd_support_t const *fdx = nullptr;
	clap_plugin_latency_t const *lat = nullptr;
	std::map<clap_id, timer_rec> timers; clap_id next_timer = 1;
	std::vector<int> fds;
	std::thread audio;
	std::atomic<bool> run_audio{false};
	std::atomic<double> rms{0}, peak{0};
	std::atomic<uint64_t> blocks{0};
	std::atomic<uint64_t> late_blocks{0};
	int latency_changes = 0;
	int state_dirty = 0;
	bool dirty_on_main = true;
	Window parent = 0;
	int gui_w = 0, gui_h = 0;
	int index = 0;
};
plug *of(clap_host_t const *h) { return static_cast<plug *>(h->host_data); }

bool h_register_timer(clap_host_t const *h, uint32_t ms, clap_id *id) { plug *s = of(h); *id = s->next_timer++; s->timers[*id] = { std::max<uint32_t>(ms, 10), std::chrono::steady_clock::now() }; return true; }
bool h_unregister_timer(clap_host_t const *h, clap_id id) { return of(h)->timers.erase(id) == 1; }
clap_host_timer_support_t const k_timer = { h_register_timer, h_unregister_timer };
bool h_register_fd(clap_host_t const *h, int fd, clap_posix_fd_flags_t) { of(h)->fds.push_back(fd); return true; }
bool h_modify_fd(clap_host_t const *, int, clap_posix_fd_flags_t) { return true; }
bool h_unregister_fd(clap_host_t const *h, int fd) { auto &v = of(h)->fds; for (size_t i = 0; i < v.size(); ++i) if (v[i] == fd) { v.erase(v.begin() + long(i)); return true; } return false; }
clap_host_posix_fd_support_t const k_fd = { h_register_fd, h_modify_fd, h_unregister_fd };
void h_latency_changed(clap_host_t const *h) { ++of(h)->latency_changes; }
clap_host_latency_t const k_latency = { h_latency_changed };
bool h_is_main(clap_host_t const *) { return std::this_thread::get_id() == g_main; }
bool h_is_audio(clap_host_t const *h) { return t_audio_owner == of(h); }
clap_host_thread_check_t const k_thread = { h_is_main, h_is_audio };
void h_log(clap_host_t const *, clap_log_severity sev, char const *msg) { fprintf(stderr, "plugin log %d: %s\n", int(sev), msg); }
clap_host_log_t const k_log = { h_log };
void h_resize_hints_changed(clap_host_t const *) { }
bool h_request_resize(clap_host_t const *, uint32_t, uint32_t) { return false; }
bool h_request_show(clap_host_t const *) { return false; }
bool h_request_hide(clap_host_t const *) { return false; }
void h_closed(clap_host_t const *, bool) { }
clap_host_gui_t const k_gui = { h_resize_hints_changed, h_request_resize, h_request_show, h_request_hide, h_closed };
void h_mark_dirty(clap_host_t const *h) { ++of(h)->state_dirty; if (std::this_thread::get_id() != g_main) of(h)->dirty_on_main = false; }
clap_host_state_t const k_state = { h_mark_dirty };

void const *h_get_extension(clap_host_t const *, char const *id)
{
	if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &k_timer;
	if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &k_fd;
	if (!strcmp(id, CLAP_EXT_LATENCY)) return &k_latency;
	if (!strcmp(id, CLAP_EXT_THREAD_CHECK)) return &k_thread;
	if (!strcmp(id, CLAP_EXT_LOG)) return &k_log;
	if (!strcmp(id, CLAP_EXT_GUI)) return &k_gui;
	if (!strcmp(id, CLAP_EXT_STATE)) return &k_state;
	return nullptr;
}
void h_request_restart(clap_host_t const *) { }
void h_request_process(clap_host_t const *) { }
void h_request_callback(clap_host_t const *) { }

// input events for one block
struct event_list { std::vector<clap_event_midi_t> ev; };
uint32_t ev_size(clap_input_events_t const *l) { return uint32_t(static_cast<event_list *>(l->ctx)->ev.size()); }
clap_event_header_t const *ev_get(clap_input_events_t const *l, uint32_t i) { return &static_cast<event_list *>(l->ctx)->ev[i].header; }
bool out_try_push(clap_output_events_t const *, clap_event_header_t const *) { return true; }

void audio_thread(plug *s)
{
	t_audio_owner = s;
	s->p->start_processing(s->p);
	uint32_t const n = 256;
	std::vector<float> l(n), r(n);
	float *chans[2] = { l.data(), r.data() };
	clap_audio_buffer_t out{ chans, nullptr, 2, 0, 0 };
	event_list el; clap_input_events_t in{ &el, ev_size, ev_get };
	clap_output_events_t oe{ nullptr, out_try_push };
	// Sample In: a quiet 440 Hz tone (exercises the realtime input path; nothing records it unless the OS is armed)
	std::vector<float> il(n), ir(n);
	float *ichans[2] = { il.data(), ir.data() };
	clap_audio_buffer_t inb{ ichans, nullptr, 2, 0, 0 };
	clap_process_t pr{}; pr.frames_count = n; pr.audio_outputs = &out; pr.audio_outputs_count = 1; pr.audio_inputs = &inb; pr.audio_inputs_count = 1; pr.in_events = &in; pr.out_events = &oe; pr.steady_time = 0;
	auto next = std::chrono::steady_clock::now();
	uint64_t blk = 0;
	double acc = 0; uint64_t cnt = 0;
	while (s->run_audio)
	{
		el.ev.clear();
		// a note every 1.5 s (on ch 14, the factory banks' channel), held 0.75 s; different keys per instance
		uint64_t const f0 = blk * n, period = 72000, hold = 36000;
		for (uint32_t k = 0; k < n; ++k)
		{
			uint64_t const f = f0 + k;
			if (f % period == 0 || f % period == hold)
			{
				clap_event_midi_t e{}; e.header = { sizeof e, k, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
				uint8_t const key = uint8_t(55 + 5 * s->index + (f / period) % 8);
				e.data[0] = f % period == 0 ? 0x9d : 0x8d; e.data[1] = key; e.data[2] = f % period == 0 ? 0x64 : 0x40;
				el.ev.push_back(e);
			}
		}
		for (uint32_t k = 0; k < n; ++k) il[k] = ir[k] = 0.05f * float(std::sin(2 * M_PI * 440.0 * double(blk * n + k) / 48000.0));
		auto const t0 = std::chrono::steady_clock::now();
		s->p->process(s->p, &pr);
		double const dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (dt > double(n) / 48000.0 * 0.5) ++s->late_blocks;      // a realtime host would have 5.3 ms for this
		for (uint32_t k = 0; k < n; ++k)
		{
			if (!std::isfinite(l[k]) || !std::isfinite(r[k])) { fprintf(stderr, "non-finite output\n"); abort(); }
			acc += double(l[k]) * l[k] + double(r[k]) * r[k]; ++cnt;
			s->peak = std::max<double>(s->peak, std::max(std::fabs(l[k]), std::fabs(r[k])));
		}
		if (cnt >= 48000) { s->rms = std::sqrt(acc / double(2 * cnt)); acc = 0; cnt = 0; }
		++blk; ++s->blocks; pr.steady_time = int64_t(blk * n);
		next += std::chrono::nanoseconds(int64_t(n * 1e9 / 48000.0));
		std::this_thread::sleep_until(next);
	}
	s->p->stop_processing(s->p);
}

Window find_child(Display *d, Window parent)
{
	Window root, par, *kids = nullptr; unsigned n = 0;
	if (!XQueryTree(d, parent, &root, &par, &kids, &n) || !n) return 0;
	Window const w = kids[n - 1]; XFree(kids); return w;
}
void send_button(Display *d, Window w, int x, int y, bool press, unsigned state = 0)
{
	XEvent e{}; e.xbutton.type = press ? ButtonPress : ButtonRelease; e.xbutton.display = d; e.xbutton.window = w; e.xbutton.x = x; e.xbutton.y = y;
	e.xbutton.button = Button1; e.xbutton.same_screen = True; e.xbutton.state = state;
	XSendEvent(d, w, False, press ? ButtonPressMask : ButtonReleaseMask, &e);
}
void send_motion(Display *d, Window w, int x, int y)
{
	XEvent e{}; e.xmotion.type = MotionNotify; e.xmotion.display = d; e.xmotion.window = w; e.xmotion.x = x; e.xmotion.y = y; e.xmotion.same_screen = True; e.xmotion.state = Button1Mask;
	XSendEvent(d, w, False, PointerMotionMask, &e);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: clap_host_test PLUGIN.clap [--instances N] [--seconds S] [--gui-cycles N]\n"); return 2; }
	g_main = std::this_thread::get_id();
	int ninst = 2, cycles = 6; double seconds = 40; bool media_test = false;
	for (int i = 2; i < argc; ++i)
	{
		std::string const a = argv[i];
		if (a == "--instances" && i + 1 < argc) ninst = atoi(argv[++i]);
		else if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
		else if (a == "--gui-cycles" && i + 1 < argc) cycles = atoi(argv[++i]);
		else if (a == "--media-test") media_test = true;
	}
	std::string const test_cd_real = getenv("EIII_CD") ? getenv("EIII_CD") : "";
	std::string test_cd, test_dir;
	std::string picker_script;
	if (media_test && !test_cd_real.empty())
	{
		// the instances start with the CD as a link in a private folder (a known quick list in the drive menus); the
		// plugin's file chooser, scripted, answers with the real path ($EIII_TEST_CD)
		char dt[] = "/tmp/eiii_media_XXXXXX";
		if (!mkdtemp(dt)) { perror("mkdtemp"); return 1; }
		test_dir = dt; test_cd = test_dir + "/disc.iso";
		if (symlink(test_cd_real.c_str(), test_cd.c_str()) != 0) { perror("symlink"); return 1; }
		setenv("EIII_CD", test_cd.c_str(), 1);
		char tmpl[] = "/tmp/eiii_picker_XXXXXX";
		int const fd = mkstemp(tmpl);
		if (fd < 0) { perror("mkstemp"); return 1; }
		std::string const sh = "#!/bin/sh\necho \"$*\" >> \"$0.log\"\ncase \"$1\" in cd) printf '%s\\n' \"$EIII_TEST_CD\";; *) exit 1;; esac\n";
		if (write(fd, sh.data(), sh.size()) != ssize_t(sh.size())) return 1;
		close(fd); chmod(tmpl, 0755);
		picker_script = tmpl;
		setenv("EIII_RACK_PICKER", tmpl, 1);
		setenv("EIII_TEST_CD", test_cd_real.c_str(), 1);
	}
	void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
	auto const *entry = static_cast<clap_plugin_entry_t const *>(dlsym(lib, "clap_entry"));
	if (!entry || !entry->init(argv[1])) { fprintf(stderr, "no clap_entry\n"); return 1; }
	auto const *fac = static_cast<clap_plugin_factory_t const *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	clap_plugin_descriptor_t const *desc = fac->get_plugin_descriptor(fac, 0);
	printf("plugin: %s (%s) %s\n", desc->name, desc->id, desc->version);

	Display *dpy = XOpenDisplay(nullptr);
	if (!dpy) { fprintf(stderr, "no X display\n"); return 1; }
	std::vector<std::unique_ptr<plug>> ps;
	for (int i = 0; i < ninst; ++i)
	{
		auto s = std::make_unique<plug>();
		s->index = i;
		s->host = { CLAP_VERSION_INIT, s.get(), "eiii clap_host_test", "eiii-core", "", "1.0", h_get_extension, h_request_restart, h_request_process, h_request_callback };
		s->p = fac->create_plugin(fac, &s->host, desc->id);
		if (!s->p || !s->p->init(s->p)) { fprintf(stderr, "create/init failed\n"); return 1; }
		s->gui = static_cast<clap_plugin_gui_t const *>(s->p->get_extension(s->p, CLAP_EXT_GUI));
		s->timer = static_cast<clap_plugin_timer_support_t const *>(s->p->get_extension(s->p, CLAP_EXT_TIMER_SUPPORT));
		s->fdx = static_cast<clap_plugin_posix_fd_support_t const *>(s->p->get_extension(s->p, CLAP_EXT_POSIX_FD_SUPPORT));
		s->lat = static_cast<clap_plugin_latency_t const *>(s->p->get_extension(s->p, CLAP_EXT_LATENCY));
		if (!s->p->activate(s->p, 48000.0, 32, 256)) { fprintf(stderr, "activate failed\n"); return 1; }
		printf("instance %d: latency %u frames (%d change notifications)\n", i, s->lat ? s->lat->get(s->p) : 0u, s->latency_changes);
		s->run_audio = true;
		s->audio = std::thread(audio_thread, s.get());
		// host window for the embedded GUI
		s->parent = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 40 + 60 * i, 40 + 300 * i, 1472, 564, 0, 0, 0x202020);
		XStoreName(dpy, s->parent, ("clap_host_test instance " + std::to_string(i)).c_str());
		XMapWindow(dpy, s->parent);
		ps.push_back(std::move(s));
	}
	XFlush(dpy);

	auto run_main = [&](double secs, std::function<void(double)> each = nullptr) {
		auto const t_end = std::chrono::steady_clock::now() + std::chrono::duration<double>(secs);
		auto const t0 = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() < t_end)
		{
			std::vector<pollfd> pf; std::vector<plug *> owner;
			for (auto &s : ps) for (int fd : s->fds) { pf.push_back({ fd, POLLIN, 0 }); owner.push_back(s.get()); }
			poll(pf.data(), nfds_t(pf.size()), 5);
			for (size_t k = 0; k < pf.size(); ++k) if (pf[k].revents & POLLIN) owner[k]->fdx->on_fd(owner[k]->p, pf[k].fd, CLAP_POSIX_FD_READ);
			auto const now = std::chrono::steady_clock::now();
			for (auto &s : ps) for (auto &[id, t] : s->timers) if (now >= t.next) { t.next = now + std::chrono::milliseconds(t.period_ms); s->timer->on_timer(s->p, id); }
			if (each) each(std::chrono::duration<double>(now - t0).count());
		}
	};
	auto open_gui = [&](plug &s, uint32_t w, uint32_t h) {
		if (!s.gui->create(s.p, CLAP_WINDOW_API_X11, false)) { fprintf(stderr, "gui create failed\n"); exit(1); }
		s.gui->set_scale(s.p, 1.0);
		uint32_t aw = w, ah = h; s.gui->adjust_size(s.p, &aw, &ah); s.gui->set_size(s.p, aw, ah);
		clap_window_t win{ CLAP_WINDOW_API_X11, {} }; win.x11 = s.parent;
		if (!s.gui->set_parent(s.p, &win)) { fprintf(stderr, "set_parent failed\n"); exit(1); }
		s.gui->show(s.p);
		XResizeWindow(dpy, s.parent, aw, ah); XFlush(dpy);
		s.gui_w = int(aw); s.gui_h = int(ah);
	};

	// phase 1: boot + (optional) autoload with the GUI open on instance 0
	open_gui(*ps[0], 1472, 564);
	run_main(std::max(20.0, seconds * 0.5));      // boot + autoload (Load Bank 44 / preset 01) finish before clicking
	// phase 2: GUI cycles on every instance: create/show/resize/destroy, clicks and slider drags while notes play
	int clicks = 0, drags = 0, knob_clicks = 0;
	double const scales[] = { 1.0, 1.25, 1.5, 0.75, 2.0, 1.0 };
	for (int c = 0; c < cycles; ++c)
	{
		for (auto &sp : ps)
		{
			plug &s = *sp;
			bool const was_open = s.gui_w > 0 && &s == ps[0].get() && c == 0;
			if (!was_open) open_gui(s, uint32_t(1472 * scales[c % 6]), uint32_t(564 * scales[c % 6]));
			run_main(0.3);
			Window const child = find_child(dpy, s.parent);
			float const k = float(s.gui_w) / 1472.f;
			// rapid presses: Master on/off (extra-switch level) and cursor right/left (scanner make/break), 20 ms apart
			if (&s == ps[0].get())
				for (int rep = 0; rep < 6; ++rep)
					for (auto [x, y] : { std::pair{ 1102.f, 45.f }, { 559.f, 170.f }, { 464.f, 170.f } })
					{
						send_button(dpy, child, int(x * k), int((y + bar_h) * k), true); XFlush(dpy); run_main(0.02);
						send_button(dpy, child, int(x * k), int((y + bar_h) * k), false); XFlush(dpy); run_main(0.02);
						++clicks;
					}
			// Contrast knob (host UI): two clicks -> display theme Amber/Red, then a turn (drag up) -> its level
			if (&s == ps[0].get() && c == 0)
			{
				for (int rep = 0; rep < 2; ++rep)
				{
					send_button(dpy, child, int(652 * k), int((170 + bar_h) * k), true); XFlush(dpy); run_main(0.03);
					send_button(dpy, child, int(652 * k), int((170 + bar_h) * k), false); XFlush(dpy); run_main(0.05);
					++knob_clicks;
				}
				send_button(dpy, child, int(652 * k), int((170 + bar_h) * k), true);
				for (int st = 1; st <= 6; ++st) { send_motion(dpy, child, int(652 * k), int(((170 - st * 5) + bar_h) * k)); XFlush(dpy); run_main(0.01); }
				send_button(dpy, child, int(652 * k), int((140 + bar_h) * k), false); XFlush(dpy); run_main(0.05);
			}
			// slider drag on the Data slider from bottom to top and back
			send_button(dpy, child, int(138 * k), int((380 + bar_h) * k), true);
			for (int st = 0; st <= 20; ++st) { send_motion(dpy, child, int(138 * k), int(((380 - st * 6.35f) + bar_h) * k)); XFlush(dpy); run_main(0.01); }
			for (int st = 20; st >= 0; --st) { send_motion(dpy, child, int(138 * k), int(((380 - st * 6.35f) + bar_h) * k)); XFlush(dpy); run_main(0.01); }
			send_button(dpy, child, int(138 * k), int((380 + bar_h) * k), false); XFlush(dpy);
			++drags;
			// host-side resize while open
			uint32_t w = uint32_t(1472 * scales[(c + 1) % 6]), h = 9999;
			s.gui->adjust_size(s.p, &w, &h); s.gui->set_size(s.p, w, h); XResizeWindow(dpy, s.parent, w, h); s.gui_w = int(w); s.gui_h = int(h);
			run_main(0.4);
			s.gui->hide(s.p); run_main(0.1); s.gui->show(s.p); run_main(0.1);
			s.gui->destroy(s.p); s.gui_w = 0;
			run_main(0.2);
		}
	}
	struct buf { std::string d; size_t rd = 0; };
	auto save_state = [](clap_plugin_t const *p) {
		auto *st = static_cast<clap_plugin_state_t const *>(p->get_extension(p, CLAP_EXT_STATE));
		buf b; clap_ostream_t os{ &b, [](clap_ostream_t const *o, void const *d, uint64_t n) -> int64_t { static_cast<buf *>(o->ctx)->d.append(static_cast<char const *>(d), n); return int64_t(n); } };
		return st->save(p, &os) ? b.d : std::string();
	};
	auto load_state = [](clap_plugin_t const *p, std::string const &t) {
		auto *st = static_cast<clap_plugin_state_t const *>(p->get_extension(p, CLAP_EXT_STATE));
		buf in{ t }; clap_istream_t is{ &in, [](clap_istream_t const *i, void *d, uint64_t n) -> int64_t {
			buf *b = static_cast<buf *>(i->ctx); size_t const k = std::min<size_t>(n, b->d.size() - b->rd); memcpy(d, b->d.data() + b->rd, k); b->rd += k; return int64_t(k); } };
		return st->load(p, &is);
	};
	int media_fails = 0;
	if (media_test && !picker_script.empty())
	{
		plug &s = *ps[0];
		open_gui(s, 1472, 564);
		run_main(0.5);
		Window const child = find_child(dpy, s.parent);
		float const k = float(s.gui_w) / 1472.f;
		// panel coordinates (y < 0: the menu bar)
		auto click = [&](float x, float y) { send_button(dpy, child, int(x * k), int((y + bar_h) * k), true); XFlush(dpy); run_main(0.03); send_button(dpy, child, int(x * k), int((y + bar_h) * k), false); XFlush(dpy); run_main(0.1); };
		auto has = [&](std::string const &line) { return save_state(s.p).find("\n" + line + "\n") != std::string::npos; };
		auto wait_for = [&](std::string const &line, double secs) { bool ok = false; run_main(secs, [&](double) { if (!ok) ok = has(line); }); return ok || has(line); };
		bool const before = has("media cd " + test_cd);
		int const dirty0 = s.state_dirty;
		click(366, 482);                         // drive eject button
		bool const ejected = wait_for("media cd ", 1.0);
		int const dirty_eject = s.state_dirty - dirty0;
		// drive click -> D3 menu at the click, moved up to fit: info, Insert disc..., Eject, separator, the one disc in
		// the test folder (5 + 3 x 22 + 9 + 22 + 5 = 107 units -> top at 538 - 4 - 107 = 427)
		click(250, 480);
		click(300, 427 + 5 + 3 * 22 + 9 + 11);   // quick list: the disc
		bool const quick = wait_for("media cd " + test_cd, 2.0);
		click(250, 480);
		click(300, 427 + 5 + 22 + 11);           // Change disc... -> scripted chooser prints the real path
		bool const chosen = wait_for("media cd " + test_cd_real, 5.0);
		std::string log; { FILE *f = fopen((picker_script + ".log").c_str(), "r"); char b[256]; while (f && fgets(b, sizeof b, f)) log += b; if (f) fclose(f); }
		bool const ok = before && ejected && quick && chosen && dirty_eject > 0 && log.rfind("cd ", 0) == 0;
		printf("%s media: CD in state %d -> eject button -> no disc %d (mark_dirty %d) -> drive menu quick list -> disc %d -> Change disc... chooser (%s) -> %d\n",
			ok ? "PASS" : "FAIL", before, ejected, dirty_eject, quick, log.empty() ? "not run" : "ran", chosen);
		if (!ok) ++media_fails;
		// Settings (first menu, x 8..): Power, Restart, separator, Analog V1, Analog V2
		click(30, -13); click(60, 5 + 2 * 22 + 9 + 22 + 11);
		bool const v2 = wait_for("media cem_model v2", 3.0);
		click(30, -13); click(60, 5 + 2 * 22 + 9 + 11);
		bool const v1 = wait_for("media cem_model v1", 3.0);
		printf("%s menu bar: Settings -> Analog model V2 %d -> V1 %d\n", v2 && v1 ? "PASS" : "FAIL", v2, v1);
		if (!(v2 && v1)) ++media_fails;
		s.gui->destroy(s.p); s.gui_w = 0;
		if (ps.size() > 1)
		{
			// a project state with no disc restores into instance 1 (hot eject, no restart) ...
			std::string st = save_state(s.p);
			auto const pos = st.find("\nmedia cd ");
			std::string const no_disc = st.substr(0, pos + 1) + "media cd \n" + st.substr(st.find('\n', pos + 1) + 1);
			bool const loaded = load_state(ps[1]->p, no_disc);
			bool done = false;
			run_main(3.0, [&](double) { if (!done) done = save_state(ps[1]->p).find("\nmedia cd \n") != std::string::npos; });
			printf("%s media: state without disc loaded into instance 1 -> its CD ejected (load %d)\n", loaded && done ? "PASS" : "FAIL", loaded);
			if (!(loaded && done)) ++media_fails;
			// ... and one with an extra CD-ROM drive at SCSI 4 (restart)
			std::string const with4 = std::regex_replace(save_state(s.p), std::regex("\nmedia scsi4 [^\n]*"), "\nmedia scsi4 cd:" + test_cd_real);
			load_state(ps[1]->p, with4);
			bool d4 = false;
			run_main(4.0, [&](double) { if (!d4) d4 = save_state(ps[1]->p).find("\nmedia scsi4 cd:" + test_cd_real + "\n") != std::string::npos; });
			printf("%s media: state with a CD-ROM drive at SCSI 4 restored into instance 1 (%d)\n", d4 ? "PASS" : "FAIL", d4);
			if (!d4) ++media_fails;
			// ... and one with hard disk changes (a one-sector change set made for the OS image): restored, and saved
			// back out of instance 1 after its restart
			if (char const *hd = getenv("EIII_HD"))
			{
				FILE *f = fopen(hd, "rb");
				std::string head(65536, '\0');
				size_t const got = f ? fread(head.data(), 1, head.size(), f) : 0;
				if (f) { fseeko(f, 0, SEEK_END); }
				uint64_t const len = f ? uint64_t(ftello(f)) : 0;
				if (f) fclose(f);
				uint64_t fp = 1469598103934665603ull;
				for (size_t i = 0; i < got; ++i) { fp ^= uint8_t(head[i]); fp *= 1099511628211ull; }
				std::string blob = "EIIIHDC1";
				auto le = [&](uint64_t v, int n) { for (int i = 0; i < n; ++i) blob += char((v >> (8 * i)) & 0xff); };
				le(len, 8); le(fp, 8); le(1, 4); le(len / 512 - 1, 4);
				for (int i = 0; i < 512; ++i) blob += char(i & 0xff);
				std::string const section = "hd_changes 1 " + std::to_string(blob.size()) + "\n" + blob + "\n";
				load_state(ps[1]->p, save_state(s.p) + section);
				bool back = false;
				run_main(5.0, [&](double) { if (!back) back = save_state(ps[1]->p).find(section) != std::string::npos; });
				printf("%s media: state with hard disk changes restored into instance 1 and saved back out (%d)\n", back ? "PASS" : "FAIL", back);
				if (!back) ++media_fails;
			}
			load_state(ps[1]->p, save_state(s.p));   // instance 0's media back for the rest of the run (no changes)
			run_main(2.0);
		}
		unlink(picker_script.c_str()); unlink((picker_script + ".log").c_str());
	}
	else if (media_test) printf("SKIP media test (EIII_CD not set)\n");

	// phase 3: GUIs closed, audio keeps running (audio must not depend on the GUI)
	double const rms_closed_start = ps[0]->rms;
	run_main(seconds * 0.25);
	printf("GUI cycles %d, clicks %d, slider drags %d\n", cycles * ninst, clicks, drags);
	int fails = media_fails;
	for (auto &sp : ps)
	{
		plug &s = *sp;
		printf("instance %d: %llu blocks, late %llu, peak %.3f, last rms %.4f\n", s.index, (unsigned long long)s.blocks.load(), (unsigned long long)s.late_blocks.load(), s.peak.load(), s.rms.load());
		if (s.peak <= 0.01) { printf("FAIL instance %d produced no audio\n", s.index); ++fails; }
	}
	(void)rms_closed_start;
	// state: the display theme chosen by clicking Contrast on instance 0 is saved and restores into instance 1
	if (cycles > 0)
	{
		auto *st0 = static_cast<clap_plugin_state_t const *>(ps[0]->p->get_extension(ps[0]->p, CLAP_EXT_STATE));
		struct buf { std::string d; size_t rd = 0; };
		auto save = [](clap_plugin_t const *p, clap_plugin_state_t const *st) {
			buf b; clap_ostream_t os{ &b, [](clap_ostream_t const *o, void const *d, uint64_t n) -> int64_t { static_cast<buf *>(o->ctx)->d.append(static_cast<char const *>(d), n); return int64_t(n); } };
			return st->save(p, &os) ? b.d : std::string();
		};
		auto display_lines = [](std::string const &t) { std::string r; size_t pos = 0; while (pos < t.size()) { size_t e = t.find('\n', pos); if (e == std::string::npos) e = t.size(); if (t.compare(pos, 4, "lcd_") == 0) r += t.substr(pos, e - pos) + "\n"; pos = e + 1; } return r; };
		std::string const s0 = save(ps[0]->p, st0);
		bool const amber = s0.find("lcd_theme amber\n") != std::string::npos && s0.find("lcd_level amber 0.7000") == std::string::npos && s0.find("lcd_level stock 0.5500") != std::string::npos;
		printf("%s state after %d Contrast clicks + a turn: theme Amber/Red with a turned level (mark_dirty %d, on main thread %d)\n%s",
			amber && ps[0]->state_dirty > 0 && ps[0]->dirty_on_main ? "PASS" : "FAIL", knob_clicks, ps[0]->state_dirty, ps[0]->dirty_on_main, display_lines(s0).c_str());
		if (!(amber && ps[0]->state_dirty > 0 && ps[0]->dirty_on_main)) ++fails;
		if (ps.size() > 1)
		{
			auto *st1 = static_cast<clap_plugin_state_t const *>(ps[1]->p->get_extension(ps[1]->p, CLAP_EXT_STATE));
			buf in{ s0 }; clap_istream_t is{ &in, [](clap_istream_t const *i, void *d, uint64_t n) -> int64_t {
				buf *b = static_cast<buf *>(i->ctx); size_t const k = std::min<size_t>(n, b->d.size() - b->rd); memcpy(d, b->d.data() + b->rd, k); b->rd += k; return int64_t(k); } };
			bool const ok = st1->load(ps[1]->p, &is);
			std::string const s1 = save(ps[1]->p, st1);
			bool const same = ok && display_lines(s1) == display_lines(s0);
			printf("%s state load into instance 1 restores the display settings\n", same ? "PASS" : "FAIL");
			if (!same) ++fails;
		}
	}
	for (auto &sp : ps) { sp->run_audio = false; sp->audio.join(); sp->p->deactivate(sp->p); sp->p->destroy(sp->p); XDestroyWindow(dpy, sp->parent); }
	XCloseDisplay(dpy);
	if (!test_dir.empty()) { unlink(test_cd.c_str()); rmdir(test_dir.c_str()); }
	entry->deinit();
	dlclose(lib);
	printf("%s\n", fails ? "FAIL" : "OK");
	return fails ? 1 : 0;
}
