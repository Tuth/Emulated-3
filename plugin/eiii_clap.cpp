// EIII Rack — CLAP instrument: one emulated Emulator III (original boot ROM + OS 2.42 from the user's disk images)
// per plugin instance, with the rack front panel as its GUI.  The panel is the instrument: there are no plugin
// parameters; MIDI goes to the emulated MIDI IN, the panel controls are the hardware inputs (eiii::panel_adapter).
//
// Threads: process() [audio] only exchanges lock-free data with the instance's engine thread; everything else is
// [main-thread].  No process-wide mutable state: every instance owns its engine, adapter, X connection and window.
//
// Media (never bundled): environment EIII_ROM_DIR / EIII_HD / EIII_CD, else ~/.config/eiii/eiii.conf
//   rom_dir = /path/to/roms        hd = /path/to/EIII_OS242.hd        cd = /path/to/factory.iso
//   scsi4 .. scsi7 = cd:/path/disc.iso | cd: (drive without a disc) | hd:/path/disk.img     (optional extra drives D4..D7)
//   cem_model = v1|v2              autoload_bank = 44   autoload_drive = 3   autoload_preset = 1   (optional)
// These are the defaults of a new instance; the media in use are saved in the CLAP state, so a project reopens with
// its disks.  The menu bar above the panel shows the drives (D1 OS hard disk, D3 CD-ROM, extra drives D4..D7) and
// changes them at runtime: a disc into / out of a CD drive is a hot swap (the OS sees a disc change); any other change
// restarts the unit from power-on.  Clicking the floppy drive opens the D3 menu, its eject button ejects the CD.  File
// choosers are a separate process (zenity or kdialog, or the program named by EIII_RACK_PICKER: called as
// "PICKER cd|hd|rom START" and printing the chosen path) so no plugin thread ever waits for one.
#include "clap/clap.h"
#include "panel/eiii_panel_adapter.h"
#include "runtime/eiii_engine.h"
#include "standalone/ui_macro.h"
#include "ui/panel_window.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <dirent.h>
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
	std::string rom_dir, cem_model = "v1";
	eiii::scsi_target scsi[8];      // [1] OS hard disk, [3] CD-ROM, [4..7] extra drives
	std::string cd_last_dir;        // where the CD chooser opens when the drive is empty
	int autoload_bank = -1, autoload_drive = 3, autoload_preset = -1;
	std::string &hd() { return scsi[1].path; }
	std::string &cd() { return scsi[3].path; }
	media_config() { scsi[1].kind = eiii::scsi_kind::hd; scsi[3].kind = eiii::scsi_kind::cd; }
};

// "cd:/path", "cd:", "hd:/path", "none"
bool parse_target(std::string const &v, eiii::scsi_target &t)
{
	if (v == "none" || v.empty()) { t = {}; return true; }
	if (v.rfind("cd:", 0) == 0) { t = { eiii::scsi_kind::cd, v.substr(3) }; return true; }
	if (v.rfind("hd:", 0) == 0 && v.size() > 3) { t = { eiii::scsi_kind::hd, v.substr(3) }; return true; }
	return false;
}
std::string target_text(eiii::scsi_target const &t)
{
	return t.kind == eiii::scsi_kind::cd ? "cd:" + t.path : t.kind == eiii::scsi_kind::hd ? "hd:" + t.path : "none";
}

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
			if (k == "rom_dir") m.rom_dir = v; else if (k == "hd") m.hd() = v; else if (k == "cd") m.cd() = v; else if (k == "cem_model") m.cem_model = v;
			else if (k.size() == 5 && k.rfind("scsi", 0) == 0 && eiii::scsi_extra_id(unsigned(k[4] - '0')))
			{
				if (!parse_target(v, m.scsi[k[4] - '0'])) fprintf(stderr, "EIII Rack: eiii.conf: %s = %s: use cd:/path, cd:, hd:/path or none\n", k.c_str(), v.c_str());
			}
			else if (k == "autoload_bank") m.autoload_bank = atoi(v.c_str()); else if (k == "autoload_drive") m.autoload_drive = atoi(v.c_str());
			else if (k == "autoload_preset") m.autoload_preset = atoi(v.c_str());
		}
	}
	auto env = [](char const *k, std::string &v) { if (char const *e = getenv(k)) v = e; };
	env("EIII_ROM_DIR", m.rom_dir); env("EIII_HD", m.hd()); env("EIII_CD", m.cd()); env("EIII_CEM_MODEL", m.cem_model);
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

// argv of a file chooser: what = "cd" / "hd" (prints a file path) or "rom" (prints a folder), start = folder/file
std::vector<std::string> picker_args(std::string const &what, std::string const &start)
{
	if (char const *custom = getenv("EIII_RACK_PICKER")) return { custom, what, start };
	std::string const title = what == "cd" ? "EIII Rack - CD-ROM image" : what == "hd" ? "EIII Rack - hard disk image" : "EIII Rack - boot ROM folder";
	if (in_path("zenity"))
	{
		std::vector<std::string> a = { "zenity", "--file-selection", "--title=" + title };
		if (!start.empty()) a.push_back("--filename=" + start);
		if (what == "rom") { a.push_back("--directory"); return a; }
		a.push_back(what == "cd" ? "--file-filter=CD-ROM images (*.iso) | *.iso *.ISO *.Iso" : "--file-filter=Hard disk images | *.iso *.ISO *.hd *.HD *.img *.IMG *.hda *.raw");
		a.push_back("--file-filter=All files | *");
		return a;
	}
	if (in_path("kdialog"))
	{
		if (what == "rom") return { "kdialog", "--title", title, "--getexistingdirectory", start.empty() ? "." : start };
		return { "kdialog", "--title", title, "--getopenfilename", start.empty() ? "." : start,
			what == "cd" ? "*.iso *.ISO|CD-ROM images (*.iso)\n*|All files" : "*.iso *.ISO *.hd *.HD *.img *.IMG *.hda *.raw|Hard disk images\n*|All files" };
	}
	return {};
}

// CD images next to the ones in use (quick list in the drive menus), sorted, at most `max`
std::vector<std::string> nearby_images(std::vector<std::string> const &dirs, size_t max)
{
	std::vector<std::string> out;
	for (auto const &d : dirs)
	{
		if (d.empty()) continue;
		DIR *dp = opendir(d.c_str());
		if (!dp) continue;
		while (dirent *e = readdir(dp))
		{
			std::string const n = e->d_name;
			if (n.size() < 5 || n[0] == '.') continue;
			std::string ext = n.substr(n.size() - 4); for (char &ch : ext) ch = char(tolower((unsigned char)ch));
			if (ext != ".iso") continue;
			std::string const full = d + (d.back() == '/' ? "" : "/") + n;
			if (std::find(out.begin(), out.end(), full) == out.end()) out.push_back(full);
		}
		closedir(dp);
	}
	std::sort(out.begin(), out.end());
	if (out.size() > max) out.resize(max);
	return out;
}
std::string disc_name(std::string const &p)
{
	std::string n = base_name(p);
	if (n.size() > 4) { std::string ext = n.substr(n.size() - 4); for (char &ch : ext) ch = char(tolower((unsigned char)ch)); if (ext == ".iso") n.resize(n.size() - 4); }
	return n;
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
	std::string pick_what;           // "" idle, "cd", "hd", "rom"
	unsigned pick_id = 0;            // SCSI ID the chosen image goes to
	std::string media_msg;           // transient message in the menu bar (refused image, no chooser)
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

	void show_media_message(std::string const &m)
	{
		media_msg = m; media_msg_until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
		fprintf(stderr, "EIII Rack: %s\n", m.c_str());
	}
	void update_menu_status()
	{
		if (!media_msg.empty() && std::chrono::steady_clock::now() >= media_msg_until) media_msg.clear();
		if (win) win->view().set_menu_status(!media_msg.empty() ? media_msg : pick.running() ? "choosing a file\xE2\x80\xA6" : "");
	}
	void dirty() { if (host_state) host_state->mark_dirty(host); }

	// [main-thread] requests (validated and applied by the engine; outcome in poll_media())
	void change_scsi(unsigned id, eiii::scsi_kind kind, std::string const &path)
	{
		if (!eng.set_scsi(id, kind, path)) show_media_message("busy: try again");
	}
	void change_setting(eiii::media_slot sl, std::string const &value)
	{
		if (!eng.set_media(sl, value)) show_media_message("busy: try again");
	}
	void poll_media()
	{
		eiii::media_result r;
		while (eng.poll_media_result(r))
		{
			if (!r.ok) { show_media_message(r.error); continue; }
			bool changed = false;
			auto set = [&](std::string &f, std::string const &v) { if (f != v) { f = v; changed = true; } };
			if (r.slot == eiii::media_slot::scsi && r.scsi_id < 8)
			{
				eiii::scsi_target &t = media.scsi[r.scsi_id];
				if (t.kind == eiii::scsi_kind::cd && !t.path.empty()) media.cd_last_dir = dir_name(t.path);
				if (t.kind != r.kind) { t.kind = r.kind; changed = true; }
				set(t.path, r.kind == eiii::scsi_kind::none ? std::string() : r.path);
			}
			else if (r.slot == eiii::media_slot::rom) set(media.rom_dir, r.path);
			else if (r.slot == eiii::media_slot::cem_model) set(media.cem_model, r.path);
			if (changed) dirty();
			if (r.restarted && !r.error.empty()) show_media_message(r.error);
			else if (r.restarted) setup_macro();       // a fresh power-on: the optional autoload runs again
		}
		std::string choice;
		if (!pick_what.empty() && pick.poll(choice))
		{
			std::string const what = pick_what;
			pick_what.clear();
			if (choice.empty()) { }                    // cancelled
			else if (what == "rom") change_setting(eiii::media_slot::rom, choice);
			else change_scsi(pick_id, what == "cd" ? eiii::scsi_kind::cd : eiii::scsi_kind::hd, choice);
		}
		update_menu_status();
	}
	void open_picker(std::string const &what, unsigned id)
	{
		if (pick.running()) { show_media_message("a file chooser is already open"); return; }
		std::string cur;
		if (what == "rom") cur = media.rom_dir;
		else if (id < 8 && media.scsi[id].kind != eiii::scsi_kind::none && !media.scsi[id].path.empty()) cur = media.scsi[id].path;
		else if (what == "cd") cur = media.cd_last_dir.empty() ? dir_name(media.cd()) : media.cd_last_dir;
		else cur = dir_name(media.hd());
		auto const args = picker_args(what, cur);
		if (args.empty()) { show_media_message("no file chooser: install zenity or kdialog (or set paths in eiii.conf)"); return; }
		if (!pick.start(args)) { show_media_message("cannot start " + args[0]); return; }
		pick_what = what; pick_id = id;
		update_menu_status();
	}
	void on_drive_click(bool eject, float lx, float ly)
	{
		if (eject) { if (!media.cd().empty()) change_scsi(3, eiii::scsi_kind::cd, ""); return; }
		if (win) win->view().open_menu_at(win->view().find_menu("d3"), lx, ly);
	}

	// ---- menu bar ----
	std::string drive_title(unsigned id)
	{
		eiii::scsi_target const &t = media.scsi[id];
		std::string const d = "D" + std::to_string(id) + "  ";
		if (t.kind == eiii::scsi_kind::none) return d + "\xE2\x80\x94";
		if (t.kind == eiii::scsi_kind::hd) return d + "HD  " + base_name(t.path);
		return d + "CD  " + (t.path.empty() ? std::string("(no disc)") : disc_name(t.path));
	}
	std::vector<eiii::ui::menu_item> drive_items(unsigned id)
	{
		using eiii::ui::menu_item;
		std::vector<menu_item> v;
		auto item = [&](std::string label, std::function<void()> f, bool enabled = true, bool checked = false) { menu_item m; m.label = std::move(label); m.action = std::move(f); m.enabled = enabled; m.checked = checked; v.push_back(m); };
		eiii::scsi_target const t = media.scsi[id];
		std::string const sid = "SCSI " + std::to_string(id) + " \xC2\xB7 ";
		bool const extra = eiii::scsi_extra_id(id);
		if (t.kind == eiii::scsi_kind::none)
		{
			v.push_back(menu_item::info(sid + "nothing connected (changes restart the unit)"));
			item("Add CD-ROM drive with a disc\xE2\x80\xA6", [this, id] { open_picker("cd", id); });
			item("Add empty CD-ROM drive", [this, id] { change_scsi(id, eiii::scsi_kind::cd, ""); });
			item("Add hard disk image\xE2\x80\xA6", [this, id] { open_picker("hd", id); });
			return v;
		}
		if (t.kind == eiii::scsi_kind::hd)
		{
			v.push_back(menu_item::info(sid + (id == 1 ? "OS hard disk \xC2\xB7 " : "hard disk \xC2\xB7 ") + t.path));
			item(id == 1 ? "Change OS hard disk image\xE2\x80\xA6 (restarts)" : "Change hard disk image\xE2\x80\xA6 (restarts)", [this, id] { open_picker("hd", id); });
		}
		else
		{
			v.push_back(menu_item::info(sid + "CD-ROM \xC2\xB7 " + (t.path.empty() ? std::string("no disc") : t.path)));
			item(t.path.empty() ? "Insert disc\xE2\x80\xA6" : "Change disc\xE2\x80\xA6", [this, id] { open_picker("cd", id); });
			item("Eject", [this, id] { change_scsi(id, eiii::scsi_kind::cd, ""); }, !t.path.empty());
			std::vector<std::string> dirs;
			for (unsigned k = 1; k < 8; ++k) if (media.scsi[k].kind == eiii::scsi_kind::cd && !media.scsi[k].path.empty()) dirs.push_back(dir_name(media.scsi[k].path));
			dirs.push_back(media.cd_last_dir);
			auto const near = nearby_images(dirs, 16);
			if (!near.empty()) v.push_back(menu_item::sep());
			for (auto const &f : near) item(disc_name(f), [this, id, f] { change_scsi(id, eiii::scsi_kind::cd, f); }, true, f == t.path);
		}
		if (extra) { v.push_back(menu_item::sep()); item("Remove drive (restarts)", [this, id] { change_scsi(id, eiii::scsi_kind::none, ""); }); }
		return v;
	}
	std::vector<eiii::ui::menu_item> settings_items()
	{
		using eiii::ui::menu_item;
		std::vector<menu_item> v;
		auto item = [&](std::string label, std::function<void()> f, bool checked = false) { menu_item m; m.label = std::move(label); m.action = std::move(f); m.checked = checked; v.push_back(m); };
		item("Power", [this] { eng.power(!eng.powered()); dirty(); }, eng.powered());
		item("Restart (cold boot)", [this] { change_setting(eiii::media_slot::restart, ""); });
		v.push_back(menu_item::sep());
		item("Analog model V1 (MAME-matched)", [this] { change_setting(eiii::media_slot::cem_model, "v1"); }, media.cem_model != "v2");
		item("Analog model V2 (CEM3387 circuit model)", [this] { change_setting(eiii::media_slot::cem_model, "v2"); }, media.cem_model == "v2");
		v.push_back(menu_item::sep());
		char const *const theme_label[] = { "Display: Stock LCD", "Display: Blue / white", "Display: Amber / red", "Display: VFD", "Display: OLED" };
		for (int k = 0; k < eiii::lcd_theme_count; ++k)
			item(theme_label[k], [this, k] { ui->set_display_theme(eiii::lcd_theme(k)); }, int(ui->display_theme()) == k);
		v.push_back(menu_item::sep());
		item("Boot ROM folder\xE2\x80\xA6 (restarts)", [this] { open_picker("rom", 0); });
		v.push_back(menu_item::info(media.rom_dir.empty() ? std::string("no ROM folder set") : media.rom_dir));
		return v;
	}
	std::vector<eiii::ui::menu> build_menus()
	{
		std::vector<eiii::ui::menu> m;
		m.push_back({ "settings", [] { return std::string("Settings"); }, [this] { return settings_items(); } });
		for (unsigned id : { 1u, 3u, 4u, 5u, 6u, 7u })
			m.push_back({ "d" + std::to_string(id), [this, id] { return drive_title(id); }, [this, id] { return drive_items(id); } });
		return m;
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
	auto media_line = [&](std::string const &k, std::string const &v) { if (v.find('\n') == std::string::npos) t += "media " + k + " " + v + "\n"; };
	media_line("rom_dir", s->media.rom_dir);
	media_line("hd", s->media.hd());
	media_line("cd", s->media.cd());
	for (unsigned id = 4; id < 8; ++id) media_line("scsi" + std::to_string(id), target_text(s->media.scsi[id]));
	media_line("cem_model", s->media.cem_model);
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
			std::string const key = rest.substr(0, sp), val = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
			bool const live = s->eng.started();
			// a project from another machine whose disks are not here keeps this machine's defaults
			auto missing = [&](std::string const &path) { if (path.empty() || file_exists(path)) return false; fprintf(stderr, "EIII Rack: saved %s not found, keeping the current setting\n", path.c_str()); return true; };
			if (key == "rom_dir" && !val.empty() && val != s->media.rom_dir && !missing(val))
			{ if (live) s->change_setting(eiii::media_slot::rom, val); else s->media.rom_dir = val; }
			else if (key == "cem_model" && (val == "v1" || val == "v2") && val != s->media.cem_model)
			{ if (live) s->change_setting(eiii::media_slot::cem_model, val); else s->media.cem_model = val; }
			else if ((key == "hd" || key == "cd") && !missing(val))
			{
				unsigned const id = key == "hd" ? 1 : 3;
				if ((id == 1 && val.empty()) || val == s->media.scsi[id].path) continue;
				if (live) s->change_scsi(id, id == 1 ? eiii::scsi_kind::hd : eiii::scsi_kind::cd, val); else s->media.scsi[id].path = val;
			}
			else if (key.size() == 5 && key.rfind("scsi", 0) == 0 && eiii::scsi_extra_id(unsigned(key[4] - '0')))
			{
				unsigned const id = unsigned(key[4] - '0');
				eiii::scsi_target t;
				if (!parse_target(val, t) || missing(t.path) || (t.kind == s->media.scsi[id].kind && t.path == s->media.scsi[id].path)) continue;
				if (live) s->change_scsi(id, t.kind, t.path); else s->media.scsi[id] = t;
			}
		}
	}
	s->saved_display = s->ui->display_state();
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
	s->win->view().set_menus(s->build_menus());
	s->win->view().on_drive_click = [s](bool eject, float lx, float ly) { s->on_drive_click(eject, lx, ly); };
	if (!s->gui_w) eiii::ui::panel_view::preferred_size(100.0 * s->gui_scale, s->gui_w, s->gui_h, true);
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
	if (!s->win || !s->win->is_open()) eiii::ui::panel_view::preferred_size(100.0 * s->gui_scale, s->gui_w, s->gui_h, true);
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
	h->aspect_ratio_width = uint32_t(eiii::ui::panel_w); h->aspect_ratio_height = uint32_t(eiii::ui::panel_view::total_h(true));
	return true;
}
bool gui_adjust_size(clap_plugin_t const *, uint32_t *w, uint32_t *h)
{
	double const ar = double(eiii::ui::panel_w) / eiii::ui::panel_view::total_h(true);
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
		c.rt.rom_dir = s->media.rom_dir; c.rt.hd_path = s->media.hd(); c.rt.cd_path = s->media.cd(); c.rt.cem_model = s->media.cem_model;
		for (unsigned id = 4; id < 8; ++id) c.rt.scsi[id] = s->media.scsi[id];
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
