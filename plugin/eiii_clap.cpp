// EIII Rack — CLAP instrument: one emulated Emulator III (original boot ROM + OS 2.42 from the user's disk images)
// per plugin instance, with the rack front panel as its GUI.  The panel is the instrument: there are no plugin
// parameters; MIDI goes to the emulated MIDI IN, the panel controls are the hardware inputs (eiii::panel_adapter).
//
// Threads: process() [audio] only exchanges lock-free data with the instance's engine thread; everything else is
// [main-thread].  No process-wide mutable state: every instance owns its engine, adapter, X connection and window.
//
// Media (never bundled): environment EIII_ROM_DIR / EIII_HD / EIII_CD, else ~/.config/eiii/eiii.conf
//   rom_dir = /path/to/roms        hd = /path/to/EIII_OS242.hd        cd = /path/to/factory.iso
//   cem_model = v1|v2              autoload_bank = 44   autoload_drive = 3   autoload_preset = 1   (optional)
// These are the defaults of a new instance; the media in use are saved in the CLAP state, so a project reopens with
// its disks.  Clicking the drive area of the panel changes them at runtime (CD: hot swap / eject, the OS sees a disc
// change; HD: the unit restarts from power-on with the new OS disk); the eject button ejects the CD.  The chooser is a
// separate process (zenity or kdialog, or the program named by EIII_RACK_PICKER) so no plugin thread ever waits for it.
#include "clap/clap.h"
#include "panel/eiii_panel_adapter.h"
#include "runtime/eiii_engine.h"
#include "standalone/ui_macro.h"
#include "ui/panel_window.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace {

char const *const k_features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SAMPLER, CLAP_PLUGIN_FEATURE_STEREO, nullptr };
clap_plugin_descriptor_t const k_desc = {
	CLAP_VERSION_INIT,
	"org.eiii-core.eiii-rack",
	"EIII Rack",
	"eiii-core (unofficial)",
	"",
	"",
	"",
	"0.9.0",
	"Unofficial E-mu Emulator III runtime: the original OS from your own disk images, operated from the rack front panel",
	k_features,
};

struct media_config
{
	std::string rom_dir, hd, cd, cem_model = "v1";
	std::string cd_last_dir;        // where the CD chooser opens when the drive is empty
	int autoload_bank = -1, autoload_drive = 3, autoload_preset = -1;
};

media_config read_media_config()
{
	media_config m;
	if (char const *home = getenv("HOME"))
	{
		std::ifstream in(std::string(home) + "/.config/eiii/eiii.conf");
		std::string line;
		while (std::getline(in, line))
		{
			if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
			auto const eq = line.find('=');
			if (eq == std::string::npos) continue;
			auto trim = [](std::string v) { while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back(); size_t i = 0; while (i < v.size() && isspace((unsigned char)v[i])) ++i; return v.substr(i); };
			std::string const k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
			if (k == "rom_dir") m.rom_dir = v; else if (k == "hd") m.hd = v; else if (k == "cd") m.cd = v; else if (k == "cem_model") m.cem_model = v;
			else if (k == "autoload_bank") m.autoload_bank = atoi(v.c_str()); else if (k == "autoload_drive") m.autoload_drive = atoi(v.c_str());
			else if (k == "autoload_preset") m.autoload_preset = atoi(v.c_str());
		}
	}
	auto env = [](char const *k, std::string &v) { if (char const *e = getenv(k)) v = e; };
	env("EIII_ROM_DIR", m.rom_dir); env("EIII_HD", m.hd); env("EIII_CD", m.cd); env("EIII_CEM_MODEL", m.cem_model);
	if (char const *e = getenv("EIII_AUTOLOAD_BANK")) m.autoload_bank = atoi(e);
	if (char const *e = getenv("EIII_AUTOLOAD_DRIVE")) m.autoload_drive = atoi(e);
	if (char const *e = getenv("EIII_AUTOLOAD_PRESET")) m.autoload_preset = atoi(e);
	return m;
}

bool file_exists(std::string const &p) { struct stat st; return !p.empty() && stat(p.c_str(), &st) == 0; }
std::string base_name(std::string const &p) { auto const s = p.find_last_of('/'); return s == std::string::npos ? p : p.substr(s + 1); }
std::string dir_name(std::string const &p) { auto const s = p.find_last_of('/'); return s == std::string::npos ? std::string() : p.substr(0, s + 1); }
bool in_path(char const *prog)
{
	char const *path = getenv("PATH");
	std::string const ps = path ? path : "/usr/bin:/bin";
	for (size_t pos = 0; pos <= ps.size();)
	{
		size_t e = ps.find(':', pos); if (e == std::string::npos) e = ps.size();
		std::string const f = ps.substr(pos, e - pos) + "/" + prog;
		if (e > pos && access(f.c_str(), X_OK) == 0) return true;
		pos = e + 1;
	}
	return false;
}

// One chooser dialog as a child process; its stdout (the choice) is collected without blocking.  [main-thread]
struct picker
{
	pid_t pid = -1;
	int fd = -1;
	std::string out;
	bool running() const { return pid > 0 || fd >= 0; }
	bool start(std::vector<std::string> const &args)
	{
		if (running() || args.empty()) return false;
		int p[2];
		if (pipe2(p, O_CLOEXEC) != 0) return false;
		posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_adddup2(&fa, p[1], 1);
		std::vector<char *> av; for (auto const &a : args) av.push_back(const_cast<char *>(a.c_str())); av.push_back(nullptr);
		pid_t child;
		int const rc = posix_spawnp(&child, av[0], &fa, nullptr, av.data(), environ);
		posix_spawn_file_actions_destroy(&fa);
		close(p[1]);
		if (rc != 0) { close(p[0]); return false; }
		fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
		pid = child; fd = p[0]; out.clear();
		return true;
	}
	// true once the dialog has closed; `result` = its first output line ("" = cancelled)
	bool poll(std::string &result)
	{
		if (fd >= 0)
		{
			char b[512];
			for (;;)
			{
				ssize_t const n = read(fd, b, sizeof b);
				if (n > 0) { out.append(b, size_t(n)); if (out.size() > 8192) out.resize(8192); continue; }
				if (n == 0 || (errno != EAGAIN && errno != EINTR)) { close(fd); fd = -1; }
				break;
			}
		}
		if (pid > 0)
		{
			int st;
			pid_t const r = waitpid(pid, &st, WNOHANG);
			if (r == pid || (r < 0 && errno == ECHILD)) pid = -1;   // ECHILD: the host reaps children itself
		}
		if (running()) return false;
		result = out.substr(0, out.find('\n'));
		return true;
	}
	void cancel()
	{
		if (pid > 0) { kill(pid, SIGTERM); waitpid(pid, nullptr, 0); pid = -1; }
		if (fd >= 0) { close(fd); fd = -1; }
	}
	~picker() { cancel(); }
};

// argv of a chooser: what = "menu" (prints cd | eject | hd) or "cd" / "hd" (prints a file path), start = folder/file
std::vector<std::string> picker_args(std::string const &what, std::string const &start, bool cd_loaded)
{
	if (char const *custom = getenv("EIII_RACK_PICKER")) return { custom, what, start };
	std::string const title = "EIII Rack - disks";
	if (in_path("zenity"))
	{
		if (what == "menu")
		{
			std::vector<std::string> a = { "zenity", "--list", "--title=" + title, "--text=Choose what to do with the EIII's drives", "--column=id", "--column=Action",
				"--hide-column=1", "--print-column=1", "--hide-header", "--width=380", "--height=230", "cd", "Insert CD-ROM image (SCSI 3)..." };
			if (cd_loaded) { a.push_back("eject"); a.push_back("Eject the CD-ROM"); }
			a.push_back("hd"); a.push_back("Change OS hard disk image (SCSI 1, restarts the unit)...");
			return a;
		}
		std::vector<std::string> a = { "zenity", "--file-selection", "--title=" + title + (what == "cd" ? ": CD-ROM image" : ": OS hard disk image") };
		if (!start.empty()) a.push_back("--filename=" + start);
		a.push_back(what == "cd" ? "--file-filter=CD-ROM images (*.iso) | *.iso *.ISO *.Iso" : "--file-filter=Hard disk images | *.iso *.ISO *.hd *.HD *.img *.IMG *.hda *.raw");
		a.push_back("--file-filter=All files | *");
		return a;
	}
	if (in_path("kdialog"))
	{
		if (what == "menu")
		{
			std::vector<std::string> a = { "kdialog", "--title", title, "--menu", "Choose what to do with the EIII's drives", "cd", "Insert CD-ROM image (SCSI 3)..." };
			if (cd_loaded) { a.push_back("eject"); a.push_back("Eject the CD-ROM"); }
			a.push_back("hd"); a.push_back("Change OS hard disk image (SCSI 1, restarts the unit)...");
			return a;
		}
		return { "kdialog", "--title", title, "--getopenfilename", start.empty() ? "." : start,
			what == "cd" ? "*.iso *.ISO|CD-ROM images (*.iso)\n*|All files" : "*.iso *.ISO *.hd *.HD *.img *.IMG *.hda *.raw|Hard disk images\n*|All files" };
	}
	return {};
}

struct instance
{
	clap_plugin_t plugin{};
	clap_host_t const *host = nullptr;
	clap_host_timer_support_t const *host_timer = nullptr;
	clap_host_posix_fd_support_t const *host_fd = nullptr;
	clap_host_latency_t const *host_latency = nullptr;
	clap_host_gui_t const *host_gui = nullptr;
	clap_host_state_t const *host_state = nullptr;
	std::string saved_display;      // display settings as last saved/loaded (theme change -> host_state->mark_dirty)
	media_config media;
	eiii::engine eng;
	std::unique_ptr<eiii::panel_adapter> ui;
	std::unique_ptr<eiii::ui::panel_window> win;
	clap_id timer = CLAP_INVALID_ID;
	int fd = -1;
	double sample_rate = 0;
	uint32_t max_frames = 0;
	unsigned latency = 0;
	int gui_w = 0, gui_h = 0;
	double gui_scale = 1.0;
	bool want_shown = true;
	ui_macro macro;
	bool macro_running = false;
	picker pick;
	std::string pick_step;           // "" idle, "menu", "cd", "hd"
	std::string media_msg;           // transient message in the drive area (refused image, no chooser)
	std::chrono::steady_clock::time_point media_msg_until;

	static instance *of(clap_plugin_t const *p) { return static_cast<instance *>(p->plugin_data); }

	void setup_macro()
	{
		auto by_internal = [](std::string const &n) { for (size_t i = 0; i < eiii::panel_control_count; ++i) if (n == eiii::panel_controls[i].internal) return int(i); return -1; };
		macro = ui_macro();
		macro.panel = [this, by_internal](std::string const &n, bool down) { int const c = by_internal(n); if (down) ui->press_button(c); else ui->release_button(c); };
		macro.extra = macro.panel;
		macro.lcd = [this] { return eiii::lcd_line_text(ui->snapshot().lcd, 0) + "\n" + eiii::lcd_line_text(ui->snapshot().lcd, 1); };
		std::string const ready = "^P\\d\\d .*\\nS\\d\\d ";
		if (media.autoload_bank >= 0 || media.autoload_preset >= 0) macro.wait(ready, 60, "main screen");
		if (media.autoload_bank >= 0)
		{
			int const b = media.autoload_bank, d = media.autoload_drive;
			char bb[16]; snprintf(bb, sizeof bb, "B%02d ", b % 100);
			auto key = [](int k) { return "Keypad " + std::to_string(k); };
			macro.press("Load Bank"); macro.wait("LOAD BANK from", 10, "LOAD BANK");
			macro.extra_press("Enter"); macro.wait("Select a Drive", 10, "drive selection");
			macro.press(key(d)); macro.wait("\\nD" + std::to_string(d) + " ", 10, "drive");
			macro.extra_press("Enter"); macro.wait("Select a Bank", 30, "bank list");
			macro.press(key(b / 10 % 10)); macro.press(key(b % 10)); macro.wait(bb, 10, "bank");
			macro.extra_press("Enter"); macro.wait("Loading Bank", 10, "loading");
			macro.wait(ready, 120, "main screen with the bank");
		}
		if (media.autoload_preset >= 0)
		{
			char pp[16]; snprintf(pp, sizeof pp, "^P%02d ", media.autoload_preset % 100);
			macro.press("Keypad " + std::to_string(media.autoload_preset / 10 % 10)); macro.press("Keypad " + std::to_string(media.autoload_preset % 10));
			macro.wait(pp, 10, "preset");
		}
		macro_running = !macro.empty();
	}

	std::string &media_field(eiii::media_slot sl) { return sl == eiii::media_slot::cd ? media.cd : sl == eiii::media_slot::hd ? media.hd : media.rom_dir; }

	void show_media_message(std::string const &m)
	{
		media_msg = m; media_msg_until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
		fprintf(stderr, "EIII Rack: %s\n", m.c_str());
	}
	void update_media_label()
	{
		if (!ui) return;
		if (!media_msg.empty() && std::chrono::steady_clock::now() >= media_msg_until) media_msg.clear();
		std::string l;
		if (!media_msg.empty()) l = media_msg;
		else if (pick.running()) l = "choosing a disk image...";
		else l = "CD  " + (media.cd.empty() ? std::string("(no disc)") : base_name(media.cd)) + "\nHD  " + base_name(media.hd);
		if (l != ui->media_label()) ui->set_media_label(l);
	}
	// [main-thread] request a media change (validated and applied by the engine; outcome in poll_media())
	void change_media(eiii::media_slot sl, std::string const &path)
	{
		if (!eng.set_media(sl, path)) show_media_message("busy: try again");
	}
	void poll_media()
	{
		eiii::media_result r;
		while (eng.poll_media_result(r))
		{
			if (!r.ok) { show_media_message(r.error); continue; }
			std::string &f = media_field(r.slot);
			if (r.slot == eiii::media_slot::cd && !f.empty()) media.cd_last_dir = dir_name(f);
			if (f != r.path) { f = r.path; if (host_state) host_state->mark_dirty(host); }
			if (r.restarted && !r.error.empty()) show_media_message(r.error);
			else if (r.restarted) setup_macro();       // a fresh power-on: the optional autoload runs again
		}
		std::string choice;
		if (!pick_step.empty() && pick.poll(choice))
		{
			std::string const step = pick_step;
			pick_step.clear();
			if (step == "menu")
			{
				if (choice == "eject") change_media(eiii::media_slot::cd, "");
				else if (choice == "cd" || choice == "hd") open_picker(choice);
			}
			else if (!choice.empty()) change_media(step == "cd" ? eiii::media_slot::cd : eiii::media_slot::hd, choice);
		}
		update_media_label();
	}
	void open_picker(std::string const &what)
	{
		if (pick.running()) return;
		std::string const cur = what == "hd" ? media.hd : what == "cd" ? (media.cd.empty() ? media.cd_last_dir : media.cd) : std::string();
		auto const args = picker_args(what, cur, !media.cd.empty());
		if (args.empty()) { show_media_message("no chooser: install zenity or kdialog (or set paths in eiii.conf)"); return; }
		if (!pick.start(args)) { show_media_message("cannot start " + args[0]); return; }
		pick_step = what;
		update_media_label();
	}
	void on_drive_click(bool eject)
	{
		if (eject) { if (!media.cd.empty()) change_media(eiii::media_slot::cd, ""); return; }
		open_picker("menu");
	}

	// [main-thread] periodic: GUI events/redraw, operator macro
	void on_timer()
	{
		poll_media();
		if (win && win->is_open()) win->tick(); else if (ui) ui->poll();
		if (ui)
		{
			std::string const d = ui->display_state();
			if (d != saved_display) { saved_display = d; if (host_state) host_state->mark_dirty(host); }
		}
		if (macro_running && ui && !macro.tick(ui->snapshot().emu_seconds))
		{
			macro_running = false;
			if (macro.failed()) fprintf(stderr, "EIII Rack: autoload failed: %s\n", macro.error().c_str());
		}
	}
};

// ---------------------------------------------------------------------------------------------- extensions
uint32_t audio_ports_count(clap_plugin_t const *, bool is_input) { return is_input ? 0 : 1; }
bool audio_ports_get(clap_plugin_t const *, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
	if (is_input || index != 0) return false;
	info->id = 0;
	snprintf(info->name, sizeof info->name, "%s", "Main Out");
	info->flags = CLAP_AUDIO_PORT_IS_MAIN;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = CLAP_INVALID_ID;
	return true;
}
clap_plugin_audio_ports_t const k_audio_ports = { audio_ports_count, audio_ports_get };

uint32_t note_ports_count(clap_plugin_t const *, bool is_input) { return is_input ? 1 : 0; }
bool note_ports_get(clap_plugin_t const *, uint32_t index, bool is_input, clap_note_port_info_t *info)
{
	if (!is_input || index != 0) return false;
	info->id = 0;
	info->supported_dialects = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP;
	info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
	snprintf(info->name, sizeof info->name, "%s", "MIDI In");
	return true;
}
clap_plugin_note_ports_t const k_note_ports = { note_ports_count, note_ports_get };

uint32_t latency_get(clap_plugin_t const *p) { return instance::of(p)->latency; }
clap_plugin_latency_t const k_latency = { latency_get };

bool render_hard_rt(clap_plugin_t const *) { return false; }
bool render_set(clap_plugin_t const *p, clap_plugin_render_mode mode) { instance::of(p)->eng.set_realtime(mode == CLAP_RENDER_REALTIME); return true; }
clap_plugin_render_t const k_render = { render_hard_rt, render_set };

void timer_on(clap_plugin_t const *p, clap_id) { instance::of(p)->on_timer(); }
clap_plugin_timer_support_t const k_timer = { timer_on };

void fd_on(clap_plugin_t const *p, int, clap_posix_fd_flags_t) { instance *const s = instance::of(p); if (s->win && s->win->is_open()) s->win->tick(); }
clap_plugin_posix_fd_support_t const k_fd = { fd_on };

// state: host-UI settings (slider positions, display theme + per-theme Contrast level, power) and the media in use
// ("media rom_dir|hd|cd PATH", cd "" = no disc).  The emulated machine itself starts from power-on like the hardware
// (banks are loaded from disk through the panel, optionally by the autoload operator macro).
bool state_save(clap_plugin_t const *p, clap_ostream_t const *out)
{
	instance *const s = instance::of(p);
	std::string t = "eiii-rack-state 1\n";
	char line[96];
	for (size_t i = 0; i < eiii::panel_control_count; ++i)
		if (eiii::panel_controls[i].kind == eiii::panel_kind::slider) { snprintf(line, sizeof line, "slider %s %.6f\n", eiii::panel_controls[i].id, s->ui->slider(int(i))); t += line; }
	t += s->saved_display = s->ui->display_state();
	snprintf(line, sizeof line, "power %d\n", s->eng.powered() ? 1 : 0); t += line;
	for (eiii::media_slot sl : { eiii::media_slot::rom, eiii::media_slot::hd, eiii::media_slot::cd })
	{
		std::string const &v = s->media_field(sl);
		if (v.find('\n') == std::string::npos) t += std::string("media ") + eiii::media_slot_name(sl) + " " + v + "\n";
	}
	size_t done = 0;
	while (done < t.size()) { int64_t const n = out->write(out, t.data() + done, t.size() - done); if (n <= 0) return false; done += size_t(n); }
	return true;
}
bool state_load(clap_plugin_t const *p, clap_istream_t const *in)
{
	instance *const s = instance::of(p);
	std::string t; char buf[512];
	for (;;) { int64_t const n = in->read(in, buf, sizeof buf); if (n < 0) return false; if (n == 0) break; t.append(buf, size_t(n)); if (t.size() > 65536) return false; }
	if (t.rfind("eiii-rack-state 1", 0) != 0) return false;
	size_t pos = 0;
	while (pos < t.size())
	{
		size_t const e = t.find('\n', pos);
		std::string const ln = t.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
		pos = e == std::string::npos ? t.size() : e + 1;
		char id[64]; double v; int pw;
		if (sscanf(ln.c_str(), "slider %63s %lf", id, &v) == 2) s->ui->set_slider(eiii::panel_find_control(id), v);
		else if (s->ui->apply_display_state_line(ln.c_str())) { }
		else if (sscanf(ln.c_str(), "power %d", &pw) == 1) s->eng.power(pw != 0);
		else if (ln.rfind("media ", 0) == 0)
		{
			std::string const rest = ln.substr(6);
			auto const sp = rest.find(' ');
			std::string const key = rest.substr(0, sp), path = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
			for (eiii::media_slot sl : { eiii::media_slot::rom, eiii::media_slot::hd, eiii::media_slot::cd })
			{
				if (key != eiii::media_slot_name(sl)) continue;
				// a project from another machine whose disks are not here keeps this machine's defaults
				if (!path.empty() && !file_exists(path)) { fprintf(stderr, "EIII Rack: saved %s not found, keeping %s\n", path.c_str(), s->media_field(sl).c_str()); break; }
				if (path.empty() && sl != eiii::media_slot::cd) break;
				if (path == s->media_field(sl)) break;
				if (s->eng.started()) s->change_media(sl, path);   // field updated when the engine reports it
				else s->media_field(sl) = path;
			}
		}
	}
	s->saved_display = s->ui->display_state();
	s->update_media_label();
	return true;
}
clap_plugin_state_t const k_state = { state_save, state_load };

// ---- GUI: X11, embedded, resizable with the panel's aspect ratio ----
bool gui_is_api_supported(clap_plugin_t const *, char const *api, bool is_floating) { return !is_floating && !strcmp(api, CLAP_WINDOW_API_X11); }
bool gui_get_preferred_api(clap_plugin_t const *, char const **api, bool *is_floating) { *api = CLAP_WINDOW_API_X11; *is_floating = false; return true; }
bool gui_create(clap_plugin_t const *p, char const *api, bool is_floating)
{
	instance *const s = instance::of(p);
	if (is_floating || strcmp(api, CLAP_WINDOW_API_X11) || s->win) return false;
	s->win = std::make_unique<eiii::ui::panel_window>(*s->ui);
	s->win->keyboard_shortcuts = false;              // the host owns the keyboard
	s->win->view().on_drive_click = [s](bool eject) { s->on_drive_click(eject); };
	if (!s->gui_w) eiii::ui::panel_view::preferred_size(100.0 * s->gui_scale, s->gui_w, s->gui_h);
	return true;
}
void gui_destroy(clap_plugin_t const *p)
{
	instance *const s = instance::of(p);
	if (s->fd >= 0 && s->host_fd) s->host_fd->unregister_fd(s->host, s->fd);
	s->fd = -1;
	if (s->win) { s->win->close(); s->win.reset(); }
	if (s->ui) s->ui->release_all();
}
bool gui_set_scale(clap_plugin_t const *p, double scale)
{
	instance *const s = instance::of(p);
	s->gui_scale = scale > 0 ? scale : 1.0;
	if (!s->win || !s->win->is_open()) eiii::ui::panel_view::preferred_size(100.0 * s->gui_scale, s->gui_w, s->gui_h);
	return true;
}
bool gui_get_size(clap_plugin_t const *p, uint32_t *w, uint32_t *h)
{
	instance *const s = instance::of(p);
	*w = uint32_t(s->gui_w); *h = uint32_t(s->gui_h);
	return s->gui_w > 0;
}
bool gui_can_resize(clap_plugin_t const *) { return true; }
bool gui_get_resize_hints(clap_plugin_t const *, clap_gui_resize_hints_t *h)
{
	h->can_resize_horizontally = h->can_resize_vertically = true;
	h->preserve_aspect_ratio = true;
	h->aspect_ratio_width = uint32_t(eiii::ui::panel_w); h->aspect_ratio_height = uint32_t(eiii::ui::panel_h);
	return true;
}
bool gui_adjust_size(clap_plugin_t const *, uint32_t *w, uint32_t *h)
{
	double const ar = double(eiii::ui::panel_w) / eiii::ui::panel_h;
	uint32_t const minw = 368;                       // 25 %
	uint32_t nw = std::max(*w, minw), nh = uint32_t(std::lround(nw / ar));
	if (nh > *h && *h >= uint32_t(minw / ar)) { nh = *h; nw = uint32_t(std::lround(nh * ar)); }
	*w = nw; *h = nh;
	return true;
}
bool gui_set_size(clap_plugin_t const *p, uint32_t w, uint32_t h)
{
	instance *const s = instance::of(p);
	s->gui_w = int(w); s->gui_h = int(h);
	if (s->win && s->win->is_open()) s->win->set_size(int(w), int(h));
	return true;
}
bool gui_set_parent(clap_plugin_t const *p, clap_window_t const *window)
{
	instance *const s = instance::of(p);
	if (!s->win || !window || strcmp(window->api, CLAP_WINDOW_API_X11)) return false;
	std::string err;
	if (!s->win->open(window->x11, s->gui_w, s->gui_h, err)) { fprintf(stderr, "EIII Rack: %s\n", err.c_str()); return false; }
	if (!s->want_shown) s->win->show(false);
	if (s->host_fd && s->host_fd->register_fd(s->host, s->win->fd(), CLAP_POSIX_FD_READ)) s->fd = s->win->fd();
	return true;
}
bool gui_set_transient(clap_plugin_t const *, clap_window_t const *) { return false; }
void gui_suggest_title(clap_plugin_t const *, char const *) { }
bool gui_show(clap_plugin_t const *p) { instance *const s = instance::of(p); s->want_shown = true; if (s->win) s->win->show(true); return s->win != nullptr; }
bool gui_hide(clap_plugin_t const *p) { instance *const s = instance::of(p); s->want_shown = false; if (s->win) s->win->show(false); if (s->ui) s->ui->release_all(); return s->win != nullptr; }
clap_plugin_gui_t const k_gui = { gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy, gui_set_scale, gui_get_size, gui_can_resize,
	gui_get_resize_hints, gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient, gui_suggest_title, gui_show, gui_hide };

// ---------------------------------------------------------------------------------------------- plugin
bool p_init(clap_plugin_t const *p)
{
	instance *const s = instance::of(p);
	s->host_timer = static_cast<clap_host_timer_support_t const *>(s->host->get_extension(s->host, CLAP_EXT_TIMER_SUPPORT));
	s->host_fd = static_cast<clap_host_posix_fd_support_t const *>(s->host->get_extension(s->host, CLAP_EXT_POSIX_FD_SUPPORT));
	s->host_latency = static_cast<clap_host_latency_t const *>(s->host->get_extension(s->host, CLAP_EXT_LATENCY));
	s->host_gui = static_cast<clap_host_gui_t const *>(s->host->get_extension(s->host, CLAP_EXT_GUI));
	s->media = read_media_config();
	s->host_state = static_cast<clap_host_state_t const *>(s->host->get_extension(s->host, CLAP_EXT_STATE));
	s->ui = std::make_unique<eiii::panel_adapter>(s->eng);
	s->saved_display = s->ui->display_state();
	s->update_media_label();
	if (s->host_timer) s->host_timer->register_timer(s->host, 30, &s->timer);
	return true;
}
void p_destroy(clap_plugin_t const *p)
{
	instance *const s = instance::of(p);
	if (char const *rep = getenv("EIII_RACK_REPORT"))
	{
		// test instrumentation: one line per destroyed instance (emulated time, audio health, panel state)
		if (s->ui) s->ui->poll();
		if (FILE *f = fopen(rep, "a"))
		{
			auto const st = s->eng.stats();
			eiii::lcd_view v; if (s->ui) s->ui->read_lcd(v);
			fprintf(f, "instance %p emu %.2f frames %llu underruns %llu (first at frame %llu, %llu frames) dropped_in %llu/%llu leds %05x lcd [%s|%s|%s|%s]\n", (void *)s, st.emu_seconds,
				(unsigned long long)st.frames_out, (unsigned long long)st.underruns, (unsigned long long)st.first_underrun_at, (unsigned long long)st.underrun_frames, (unsigned long long)st.panel_dropped, (unsigned long long)st.midi_dropped,
				s->ui ? s->ui->read_leds() : 0u, eiii::lcd_row_text(v, 0).c_str(), eiii::lcd_row_text(v, 1).c_str(), eiii::lcd_row_text(v, 2).c_str(), eiii::lcd_row_text(v, 3).c_str());
			fclose(f);
		}
	}
	gui_destroy(p);
	s->pick.cancel();
	if (s->timer != CLAP_INVALID_ID && s->host_timer) s->host_timer->unregister_timer(s->host, s->timer);
	s->eng.shutdown();
	delete s;
}
bool p_activate(clap_plugin_t const *p, double sample_rate, uint32_t, uint32_t max_frames)
{
	instance *const s = instance::of(p);
	// the machine keeps running across deactivate/activate; a new sample rate (or a larger block size than the
	// latency allows for) restarts it from power-on, like switching the unit off and on
	if (!s->eng.started() || sample_rate != s->sample_rate || max_frames > s->max_frames)
	{
		eiii::engine_config c;
		c.rt.rom_dir = s->media.rom_dir; c.rt.hd_path = s->media.hd; c.rt.cd_path = s->media.cd; c.rt.cem_model = s->media.cem_model;
		c.rt.sample_rate = sample_rate;
		c.max_block = std::max<uint32_t>(max_frames, 64);
		c.realtime = true;
		s->eng.start(c);
		s->sample_rate = sample_rate; s->max_frames = std::max<uint32_t>(max_frames, 64);
		unsigned const lat = s->eng.latency();
		if (lat != s->latency) { s->latency = lat; if (s->host_latency) s->host_latency->changed(s->host); }
		s->setup_macro();
	}
	return true;
}
void p_deactivate(clap_plugin_t const *) { }
bool p_start_processing(clap_plugin_t const *) { return true; }
void p_stop_processing(clap_plugin_t const *) { }
void p_reset(clap_plugin_t const *) { }

uint8_t midi_len(uint8_t status)
{
	if (status < 0x80) return 1;                          // running-status data byte
	switch (status & 0xf0) { case 0xc0: case 0xd0: return 2; case 0xf0: return status == 0xf1 || status == 0xf3 ? 2 : status == 0xf2 ? 3 : 1; default: return 3; }
}

clap_process_status p_process(clap_plugin_t const *p, clap_process_t const *proc)
{
	instance *const s = instance::of(p);
	// MIDI IN: every event becomes the bytes the hardware MIDI IN would receive, at its sample offset
	if (proc->in_events)
	{
		uint32_t const n = proc->in_events->size(proc->in_events);
		for (uint32_t i = 0; i < n; ++i)
		{
			clap_event_header_t const *h = proc->in_events->get(proc->in_events, i);
			if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
			if (h->type == CLAP_EVENT_MIDI)
			{
				auto const *e = reinterpret_cast<clap_event_midi_t const *>(h);
				s->eng.midi(h->time, e->data, midi_len(e->data[0]));
			}
			else if (h->type == CLAP_EVENT_MIDI_SYSEX)
			{
				auto const *e = reinterpret_cast<clap_event_midi_sysex_t const *>(h);
				for (uint32_t k = 0; k < e->size; k += 3) s->eng.midi(h->time, e->buffer + k, uint8_t(std::min<uint32_t>(3, e->size - k)));
			}
			else if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF)
			{
				auto const *e = reinterpret_cast<clap_event_note_t const *>(h);
				if (e->key < 0 || e->key > 127) continue;
				int const ch = e->channel >= 0 && e->channel < 16 ? e->channel : 0;
				int vel = int(std::lround(e->velocity * 127.0)); vel = std::clamp(vel, h->type == CLAP_EVENT_NOTE_ON ? 1 : 0, 127);
				uint8_t const b[3] = { uint8_t((h->type == CLAP_EVENT_NOTE_ON ? 0x90 : 0x80) | ch), uint8_t(e->key), uint8_t(vel) };
				s->eng.midi(h->time, b, 3);
			}
		}
	}
	if (proc->audio_outputs_count >= 1 && proc->audio_outputs[0].channel_count >= 2 && proc->audio_outputs[0].data32)
	{
		float *const l = proc->audio_outputs[0].data32[0], *const r = proc->audio_outputs[0].data32[1];
		s->eng.pull(l, r, proc->frames_count);
	}
	return CLAP_PROCESS_CONTINUE;
}

void const *p_get_extension(clap_plugin_t const *, char const *id)
{
	if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &k_audio_ports;
	if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &k_note_ports;
	if (!strcmp(id, CLAP_EXT_LATENCY)) return &k_latency;
	if (!strcmp(id, CLAP_EXT_RENDER)) return &k_render;
	if (!strcmp(id, CLAP_EXT_STATE)) return &k_state;
	if (!strcmp(id, CLAP_EXT_GUI)) return &k_gui;
	if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &k_timer;
	if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &k_fd;
	return nullptr;
}
void p_on_main_thread(clap_plugin_t const *) { }

clap_plugin_t const *create(clap_host_t const *host)
{
	instance *const s = new instance;
	s->host = host;
	s->plugin = { &k_desc, s, p_init, p_destroy, p_activate, p_deactivate, p_start_processing, p_stop_processing, p_reset, p_process, p_get_extension, p_on_main_thread };
	return &s->plugin;
}

// ---------------------------------------------------------------------------------------------- factory / entry
uint32_t f_count(clap_plugin_factory_t const *) { return 1; }
clap_plugin_descriptor_t const *f_desc(clap_plugin_factory_t const *, uint32_t i) { return i == 0 ? &k_desc : nullptr; }
clap_plugin_t const *f_create(clap_plugin_factory_t const *, clap_host_t const *host, char const *id)
{
	if (!clap_version_is_compatible(host->clap_version) || strcmp(id, k_desc.id)) return nullptr;
	return create(host);
}
clap_plugin_factory_t const k_factory = { f_count, f_desc, f_create };

bool e_init(char const *) { return true; }
void e_deinit() { }
void const *e_get_factory(char const *id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &k_factory : nullptr; }

} // namespace

extern "C" CLAP_EXPORT clap_plugin_entry_t const clap_entry = { CLAP_VERSION_INIT, e_init, e_deinit, e_get_factory };
