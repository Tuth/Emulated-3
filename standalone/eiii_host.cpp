// eiii-standalone spike: the UNMODIFIED MAME NS32016 + NS32081 cores (via shim/emu.h) on a minimal EIII motherboard.
// Milestone 1: execute the authentic boot PROMs and compare the instruction stream with MAME's debugger trace.
// usage: eiii-standalone --rom-dir DIR [--cycles N] [--pc-trace FILE] [--log FILE]
#include "../runtime/eiii_runtime_impl.h"
#include "eiii_panel_codes.h"
#include "host_io.h"
#include "ui_macro.h"
#include "eiii_gui.h"
#include <chrono>
#include <csignal>
#include <regex>
#include <thread>
#include <fstream>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif
#include <sstream>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>







namespace {

#define EIII_STANDALONE_VERSION "0.9.0 (OS 2.42 runtime; MAME-equivalent boot/SCSI/panel; software CEM)"

struct options_t
{
	std::string midi_in, audio_out;
	unsigned period_frames = 0, periods = 0;
	int autoload_bank = -1, autoload_drive = 3, preset = -1;
	bool realtime = false, verbose_midi = false, stats = false;
	std::string cem_model = "v1", cem_calib;
} g_opt;

volatile std::sig_atomic_t g_stop = 0;
bool g_log_leds = false;   // --log-leds: print LED latch changes (decoding aid)
void on_sigint(int) { g_stop = 1; }


using eiii::machine_t;
using eiii::CPU_HZ;
machine_t *g_m = nullptr;   // the standalone's single instance (debug hooks)

void print_help()
{
	printf(
"eiii-standalone %s\n"
"E-mu Emulator III runtime: original boot ROM + OS from the original disk images, original UI/MIDI paths,\n"
"portable F-Chip + software CEM3387 output.  No E-mu material is built in; supply your own ROMs and images.\n\n"
"usage: eiii-standalone --hard OS_HD_IMAGE [options]\n\n"
"media / firmware\n"
"  --rom-dir DIR          boot ROM directory (default $EIII_ROM_DIR, else ./roms):\n"
"                           e3-lsboot_ip381a_emu_systems_4088.ic3, e3-msboot_ip380a_emu_systems_4088.ic4\n"
"  --hard IMAGE           SCSI ID 1 hard disk image (opened read-only; writes kept in RAM)\n"
"  --cdrom ISO            SCSI ID 3 CD-ROM image (read-only)\n"
"live use\n"
"  --midi-in DEV          ALSA raw MIDI input, e.g. hw:1,0,0 (see --list-midi); bytes go to the Z85230 MIDI IN\n"
"  --audio-out DEV        ALSA PCM output, e.g. default or plughw:0,0 (see --list-audio); 48 kHz stereo\n"
"  --period N / --periods N   ALSA period size in frames (256) / number of periods (4)\n"
"  --realtime             pace to the wall clock when no --audio-out is given (with --audio-out the device paces)\n"
"  --autoload-bank N      operate the ORIGINAL front panel to load bank N: Load Bank, Enter, drive, Enter, N, Enter\n"
"  --drive D              drive for --autoload-bank (default 3 = SCSI CD-ROM)\n"
"  --preset P             then select preset P on the keypad (after --autoload-bank, or on its own)\n"
"  --list-midi / --list-audio   list devices and exit\n"
"front panel\n"
"  --gui                  show the rack front panel (X11) and operate the instrument from it; with --audio-out,\n"
"                         --midi-in, --autoload-bank/--preset as above (scripts/debug options are for the headless mode)\n"
"  --scale PCT            panel size, 100 = 1472 x 538 (125, 150, 200 for HiDPI); the window is also resizable\n"
"  --quit-after S / --screenshot FILE.png   (GUI tests) quit after S emulated seconds / save the window contents\n"
"offline / regression\n"
"  --seconds S | --cycles N     stop after S emulated seconds / N CPU cycles (default: run until Ctrl-C)\n"
"  --script FILE          timed events: 'T press|down|up NAME', 'T extra-press NAME', 'T key-down I V', 'T midi HEX',\n"
"                         'T scan HEX', 'T lcd', 'T cv V', 'T dump PREFIX', 'T quit'\n"
"  --midi T:HEX[,T:HEX]   scheduled MIDI bytes\n"
"  --wav FILE             also write the stereo output (16-bit 48 kHz) to FILE\n"
"  --verbose-midi         log the MIDI chain: host byte -> SCC data read -> firmware parser -> voice start\n"
"  --stats                print realtime factor and timing breakdown at exit\n"
"  --no-ftz               strict IEEE denormals in the analog model (slower; identical audio)\n"
"  --cem-model v1|v2      analog voice model: v1 (default, released sound) or v2 (evidence-refined CEM3387 model)\n"
"  --cem-calib FILE       CEM3387 calibration file for v2 (see analysis/CEM3387_CALIBRATION_FORMAT.md)\n"
"  --log FILE, --pc-trace FILE, --pc-trace-from CYC, --watch LO:HI, --probe PC[,PC], --dump-at CYC:PC:FILE, --pot N:V\n"
"                         debug aids used by the MAME-equivalence regressions\n"
"  --help, --version\n", EIII_STANDALONE_VERSION);
}

FILE *g_pc = nullptr; unsigned long long g_insns = 0;
unsigned g_watch_lo = 0, g_watch_hi = 0;
unsigned long long g_pc_from = 0;
bool g_no_ftz = false;
std::vector<unsigned> g_probes;  // --probe PC[,PC...]: log R0-R7/SP/FP when PC executes (debug aid)
std::vector<std::pair<unsigned, unsigned>> g_pots;  // --pot N:VALUE (front-panel analog controls, firmware ADC units)
// --dump-at CYCLES:PC:FILE — dump RAM 0x8000-0x27FFF at the first execution of PC after CYCLES (for state comparison)
unsigned long long g_dump_cycles = ~0ull; unsigned g_dump_pc = 0; std::string g_dump_file; bool g_dumped = false;
void pc_hook(offs_t pc)
{
	++g_insns;
	g_m->last_pc = pc;
	if (g_opt.verbose_midi)
	{
		// DIAGNOSTIC ONLY (no behaviour): OS 2.42 MIDI parser / note engine entry points from analysis/traces/probes/midi_validation.cmd
		auto &c = *g_m->cpu;
		if (pc == 0x00f36f) fprintf(stderr, "MIDI-CHAIN %.6f firmware MIDI parser: note key=%02X vel=%02X\n", c.total_cycles() / double(CPU_HZ), unsigned(c.state_value("R0") & 0xff), unsigned(c.state_value("R1") & 0xff));
		else if (pc == 0x012774) fprintf(stderr, "MIDI-CHAIN %.6f firmware note-on engine entered\n", c.total_cycles() / double(CPU_HZ));
		else if (pc == 0x0127ee) fprintf(stderr, "MIDI-CHAIN %.6f firmware note-off engine entered\n", c.total_cycles() / double(CPU_HZ));
	}
	for (unsigned pp : g_probes) if (pp == unsigned(pc) && g_m->log)
	{
		auto &c = *g_m->cpu;
		fprintf(g_m->log, "PROBE cyc=%llu pc=%06X", (unsigned long long)c.total_cycles(), unsigned(pc));
		for (int r = 0; r < 8; ++r) fprintf(g_m->log, " r%d=%08X", r, unsigned(c.state_value("R" + std::to_string(r))));
		fprintf(g_m->log, " sp=%08X fp=%08X\n", unsigned(c.state_value("SP0")), unsigned(c.state_value("FP")));
	}
	if (g_pc && g_m->cpu->total_cycles() >= g_pc_from) fprintf(g_pc, "%06X\n", unsigned(pc));
	if (!g_dumped && unsigned(pc) == g_dump_pc && g_m->cpu->total_cycles() >= g_dump_cycles)
	{
		g_dumped = true;
		FILE *f = fopen(g_dump_file.c_str(), "wb");
		for (u16 w : g_m->ram) { fputc(w & 0xff, f); fputc(w >> 8, f); }
		fclose(f);
		fprintf(stderr, "dumped RAM at cycle %llu pc %06X\n", (unsigned long long)g_m->cpu->total_cycles(), unsigned(pc));
	}
}

} // namespace

int main(int argc, char **argv)
{
	char const *const env_rom = getenv("EIII_ROM_DIR");
	std::string romdir = env_rom ? env_rom : "roms", pcfile, logfile, hdpath, cdpath, wavfile;
	std::vector<std::pair<double, std::vector<u8>>> midi_events;  // (seconds, bytes)
	double inject_at = -1;
	std::string scriptfile;
	bool gui = false; gui_options go;
	double cycles = 0;              // 0 = run until stopped (realtime) or until the script/autoload says so
	for (int i = 1; i < argc; ++i)
	{
		std::string const k = argv[i];
		auto val = [&]() -> char const * {
			if (i + 1 >= argc) { fprintf(stderr, "eiii-standalone: option %s needs a value (see --help)\n", k.c_str()); exit(2); }
			return argv[++i];
		};
		if (k == "--help" || k == "-h") { print_help(); return 0; }
		else if (k == "--version") { printf("eiii-standalone %s\n", EIII_STANDALONE_VERSION); return 0; }
		else if (k == "--list-midi") { return list_midi_inputs(); }
		else if (k == "--list-audio") { return list_audio_outputs(); }
		else if (k == "--rom-dir") romdir = val();
		else if (k == "--hard") hdpath = val();
		else if (k == "--cdrom") cdpath = val();
		else if (k == "--cycles") cycles = atof(val());
		else if (k == "--seconds") cycles = atof(val()) * CPU_HZ;
		else if (k == "--wav") wavfile = val();
		else if (k == "--script") scriptfile = val();
		else if (k == "--midi-in") g_opt.midi_in = val();
		else if (k == "--audio-out") g_opt.audio_out = val();
		else if (k == "--period") g_opt.period_frames = unsigned(atoi(val()));
		else if (k == "--periods") g_opt.periods = unsigned(atoi(val()));
		else if (k == "--autoload-bank") g_opt.autoload_bank = atoi(val());
		else if (k == "--drive") g_opt.autoload_drive = atoi(val());
		else if (k == "--preset") g_opt.preset = atoi(val());
		else if (k == "--cem-model") g_opt.cem_model = val();
		else if (k == "--cem-calib") g_opt.cem_calib = val();
		else if (k == "--realtime") g_opt.realtime = true;
		else if (k == "--verbose-midi") g_opt.verbose_midi = true;
		else if (k == "--stats") g_opt.stats = true;
		else if (k == "--no-ftz") g_no_ftz = true;
		else if (k == "--log-leds") g_log_leds = true;
		else if (k == "--gui") gui = true;
		else if (k == "--scale") go.scale = atof(val());
		else if (k == "--quit-after") go.quit_after = atof(val());
		else if (k == "--screenshot") go.screenshot = val();
		// debug / regression aids
		else if (k == "--log") logfile = val();
		else if (k == "--pc-trace") pcfile = val();
		else if (k == "--pc-trace-from") g_pc_from = strtoull(val(), nullptr, 0);
		else if (k == "--pot") { unsigned n2, v2; if (sscanf(val(), "%u:%x", &n2, &v2) == 2 && n2 < 8) g_pots.emplace_back(n2, v2); }
		else if (k == "--probe") { std::string v = val(); size_t p0 = 0; while (p0 < v.size()) { g_probes.push_back(unsigned(strtoul(v.c_str() + p0, nullptr, 16))); size_t c = v.find(',', p0); if (c == std::string::npos) break; p0 = c + 1; } }
		else if (k == "--watch") sscanf(val(), "%x:%x", &g_watch_lo, &g_watch_hi);
		else if (k == "--dump-at") { char f[512]; if (sscanf(val(), "%llu:%x:%511s", &g_dump_cycles, &g_dump_pc, f) == 3) g_dump_file = f; }
		else if (k == "--inject-bank-at") inject_at = atof(val());   // TEST HARNESS ONLY (legacy regression); never used by acceptance
		else if (k == "--midi")
		{
			// --midi "T:HEXBYTES[,T:HEXBYTES...]"  scheduled MIDI bytes, e.g. "26.0:903C7F,27.0:803C40"
			std::string spec = val();
			size_t p0 = 0;
			while (p0 < spec.size())
			{
				size_t const p1 = spec.find(',', p0); std::string const item = spec.substr(p0, p1 == std::string::npos ? std::string::npos : p1 - p0);
				size_t const c = item.find(':');
				std::vector<u8> bytes; for (size_t k2 = c + 1; k2 + 2 <= item.size(); k2 += 2) bytes.push_back(u8(strtoul(item.substr(k2, 2).c_str(), nullptr, 16)));
				midi_events.emplace_back(atof(item.substr(0, c).c_str()), bytes);
				if (p1 == std::string::npos) break;
				p0 = p1 + 1;
			}
		}
		else { fprintf(stderr, "eiii-standalone: unknown option '%s' (see --help)\n", k.c_str()); return 2; }
	}
	if (hdpath.empty()) { fprintf(stderr, "eiii-standalone: --hard <OS 2.42 hard disk image> is required (see --help)\n"); return 2; }
	if (gui)
	{
		go.rom_dir = romdir; go.hd = hdpath; go.cd = cdpath; go.cem_model = g_opt.cem_model; go.cem_calib = g_opt.cem_calib;
		go.midi_in = g_opt.midi_in; go.audio_out = g_opt.audio_out;
		if (g_opt.period_frames) go.period = g_opt.period_frames;
		if (g_opt.periods) go.periods = g_opt.periods;
		go.autoload_bank = g_opt.autoload_bank; go.drive = g_opt.autoload_drive; go.preset = g_opt.preset; go.verbose = g_opt.verbose_midi;
		return run_gui(go);
	}
	// Flush-to-zero / denormals-are-zero for the analog model: decaying filter states otherwise sit in the denormal range
	// (< 2.2e-308 V) and cost ~7x CPU.  Outputs are unchanged at any audio resolution (all coefficients < 1e5); check_*.sh
	// verify identical WAVs with and without it.  --no-ftz = strict IEEE denormals.
	if (!g_no_ftz)
	{
#if defined(__x86_64__) || defined(__i386__)
		_mm_setcsr(_mm_getcsr() | 0x8040);
#elif defined(__aarch64__)
		uint64_t fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr)); fpcr |= (1ull << 24); __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
	}
	// the machine: one eiii::runtime on this thread (the hook must be set before the CPU is built: it captures it)
	FILE *const logf = logfile.empty() ? nullptr : fopen(logfile.c_str(), "w");
	if (!pcfile.empty()) g_pc = fopen(pcfile.c_str(), "w");
	g_insn_hook = pc_hook;
	eiii::runtime rt;
	{
		eiii::runtime_config rc;
		rc.rom_dir = romdir; rc.hd_path = hdpath; rc.cd_path = cdpath;
		rc.cem_model = g_opt.cem_model; rc.cem_calib = g_opt.cem_calib;
		rc.sample_rate = 48000.0;
		rc.midi_clock = !g_opt.midi_in.empty();     // scheduled MIDI also starts the clock (power_on)
		rc.log_voice_starts = !g_opt.realtime || g_opt.verbose_midi;
		rc.log_leds = g_log_leds;
		rc.log_midi_chain = g_opt.verbose_midi;
		rc.log = logf;
		std::string err;
		if (!rt.open_unpowered(rc, err)) { fprintf(stderr, "eiii-standalone: %s\n", err.c_str()); return 2; }
	}
	eiii::runtime::impl &R = rt.internals();
	machine_t &m = R.m; g_m = &m;
	auto &cpu = *R.cpu;
	auto &backend = *R.backend;
	m.watch_lo = g_watch_lo; m.watch_hi = g_watch_hi;
	for (auto [n2, v2] : g_pots) m.adc_pot[n2] = u16(v2 & 0x7fff);
	if (g_opt.cem_model == "v2")
		fprintf(stderr, "analog model: %s (192 kHz, FIR decimation)%s%s\n", backend.model_name(), g_opt.cem_calib.empty() ? ", nominal parameters" : ", calibration ", g_opt.cem_calib.c_str());
	for (auto &e : midi_events) rt.schedule_midi(e.first, e.second);

	// TEST HARNESS (not emulation): copy the factory Tenor Sax bank images into RAM exactly as the MAME test Lua does
	// (headless original Load Bank does not work yet, U1).  file+0x4C: 0xCD33 -> 0x1A000, 0x3ACD56 -> 0x400000, 0xD00 -> 0xBFCB00
	auto inject_bank = [&]() {
		FILE *f = fopen(cdpath.c_str(), "rb"); if (!f) return;
		auto copy = [&](u32 dest, u32 len) {
			std::vector<u8> buf(len + 1, 0); size_t const n = fread(buf.data(), 1, len, f); (void)n;
			for (u32 k2 = 0; k2 < len; k2 += 2)
			{
				u16 const w = u16(buf[k2] | (buf[k2 + 1] << 8));
				u32 const ad = dest + k2;
				if (ad >= 0x8000 && ad < 0x28000) m.ram[(ad - 0x8000) >> 1] = w; else if (ad >= 0x400000 && ad < 0xc00000) m.snd[(ad - 0x400000) >> 1] = w;
			}
		};
		fseek(f, long(0x5306Full * 512 + 0x4C), SEEK_SET); copy(0x1A000, 0xCD34);
		fseek(f, long(0x5306Full * 512 + 0x4C + 0xCD33), SEEK_SET); copy(0x400000, 0x3ACD56); copy(0xBFCB00, 0xD00);
		fclose(f);
		fprintf(stderr, "HARNESS: Tenor Sax bank image injected at %.6f s\n", double(shim_sched().time().m_seconds));
	};
	emu_timer inject_timer([&](s32) { inject_bank(); });


	// ---- event script: "TIME COMMAND ARGS" (TIME in emulated seconds; '#' comments) ----
	//   down NAME | up NAME | press NAME [HOLD_S]   panel switch by MAME port name (make C0/break C3 via the scanner FIFO)
	//   extra-down NAME | extra-up NAME | extra-press NAME [HOLD_S]   0xFA0000 switches (Master, Enter, ...)
	//   key-down INDEX VELOCITY | key-up INDEX     keyboard scanner bytes (status 0x80|index, velocity / 0xC2, index)
	//   scan HEXBYTE                               raw scanner byte (for exact replays of MAME's logged pushes)
	//   midi HEXBYTES                              bytes into SCC channel A at 31250 baud
	//   lcd                                        print the LCD
	struct sev_t { double t; std::string cmd, arg; };
	std::vector<sev_t> sev;
	auto panel_code = [](std::string const &n) -> int { for (auto const &e : eiii_panel_codes) if (n == e.name) return int(e.code); char *end; long v = strtol(n.c_str(), &end, 16); return (*end || n.empty()) ? -1 : int(v); };
	auto extra_bit = [](std::string const &n) -> int { for (auto const &e : eiii_extra_bits) if (n == e.name) return int(e.code); return -1; };
	if (!scriptfile.empty())
	{
		std::ifstream in(scriptfile);
		std::string line;
		while (std::getline(in, line))
		{
			if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
			std::istringstream ls(line); double t; std::string cmd;
			if (!(ls >> t >> cmd)) continue;
			std::string rest; std::getline(ls, rest); while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1); while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\r')) rest.pop_back();
			if (cmd == "press" || cmd == "extra-press")
			{
				double hold = 0.08; auto sp = rest.find_last_of(' ');
				if (sp != std::string::npos && rest.find('.', sp) != std::string::npos) { char *e; double hv = strtod(rest.c_str() + sp + 1, &e); if (!*e) { hold = hv; rest.resize(sp); } }  // hold must contain '.' (names end in digits)
				bool const ex = cmd == "extra-press";
				sev.push_back({ t, ex ? "extra-down" : "down", rest }); sev.push_back({ t + hold, ex ? "extra-up" : "up", rest });
			}
			else if (cmd == "midi") rt.schedule_midi(t, [&] { std::vector<u8> v; for (size_t k2 = 0; k2 + 1 < rest.size(); k2 += 2) v.push_back(u8(strtoul(rest.substr(k2, 2).c_str(), nullptr, 16))); return v; }());
			else sev.push_back({ t, cmd, rest });
		}
		std::stable_sort(sev.begin(), sev.end(), [](sev_t const &x, sev_t const &y) { return x.t < y.t; });
	}
	size_t sev_next = 0;
	emu_timer script_timer([&](s32) {
		double const now_s = double(shim_sched().time().m_seconds);
		while (sev_next < sev.size() && sev[sev_next].t <= now_s + 1e-12)
		{
			sev_t const &e = sev[sev_next++];
			if (e.cmd == "down" || e.cmd == "up")
			{
				int const c = panel_code(e.arg);
				if (c < 0) { fprintf(stderr, "script: unknown panel name '%s'\n", e.arg.c_str()); continue; }
				m.scanner_push(e.cmd == "down" ? 0xc0 : 0xc3); m.scanner_push(u8(c));
			}
			else if (e.cmd == "extra-down" || e.cmd == "extra-up")
			{
				int const b = extra_bit(e.arg);
				if (b < 0) { fprintf(stderr, "script: unknown extra switch '%s'\n", e.arg.c_str()); continue; }
				if (e.cmd == "extra-down") m.extra_switches |= u8(b); else m.extra_switches &= u8(~b);
			}
			else if (e.cmd == "key-down") { unsigned idx, vel; if (sscanf(e.arg.c_str(), "%u %u", &idx, &vel) == 2) { m.scanner_push(u8(0x80 | (idx & 0x3f))); m.scanner_push(u8(vel & 0x7f)); } }
			else if (e.cmd == "key-up") { unsigned idx; if (sscanf(e.arg.c_str(), "%u", &idx) == 1) { m.scanner_push(0xc2); m.scanner_push(u8(idx & 0x7f)); } }
			else if (e.cmd == "scan") m.scanner_push(u8(strtoul(e.arg.c_str(), nullptr, 16)));
			else if (e.cmd == "dump")
			{
				// main RAM 0x8000-0x27FFF -> ARG.ram, sample RAM 0x400000-0xBFFFFF -> ARG.snd (little-endian words)
				auto save = [](std::string const &fn, std::vector<u16> const &v) { FILE *f = fopen(fn.c_str(), "wb"); for (u16 w : v) { fputc(w & 0xff, f); fputc(w >> 8, f); } fclose(f); };
				save(e.arg + ".ram", m.ram); save(e.arg + ".snd", m.snd);
				fprintf(stderr, "DUMP %.3f %s\n", now_s, e.arg.c_str());
			}
			else if (e.cmd == "cv")
			{
				unsigned const v = unsigned(atoi(e.arg.c_str())) & 15;
				fprintf(stderr, "CV %.3f voice %u Fc1=%04X Fc2=%04X Q=%04X Gain=%04X Pan=%04X lane_rate=%03X\n", now_s, v,
					backend.cv_word(v, 0), backend.cv_word(v, 1), backend.cv_word(v, 2), backend.cv_word(v, 3), backend.cv_word(v, 4), unsigned(backend.lanes().state(v).rate));
			}
			else if (e.cmd == "cvpin")
			{
				unsigned const v = unsigned(atoi(e.arg.c_str())) & 15;
				backend.render_until(m.t20());
				fprintf(stderr, "CVPIN %.4f voice %u Fc1 %.4f Fc2 %.4f Q %.4f Gain %.4f Pan %.4f (words %04X %04X %04X %04X %04X)\n", now_s, v,
					backend.cv_pin(v, 0), backend.cv_pin(v, 1), backend.cv_pin(v, 2), backend.cv_pin(v, 3), backend.cv_pin(v, 4),
					backend.cv_word(v, 0), backend.cv_word(v, 1), backend.cv_word(v, 2), backend.cv_word(v, 3), backend.cv_word(v, 4));
			}
			else if (e.cmd == "lcd") fprintf(stderr, "LCD %.3f [%s] [%s]\n", now_s, m.lcd_text(0).c_str(), m.lcd_text(1).c_str());
			else if (e.cmd == "quit") { g_stop = 1; rt.request_stop(1); }
			else if (e.cmd == "leds") fprintf(stderr, "LEDS %.3f latch %04X misc7 %d\n", now_s, m.leds, BIT(m.misc, 7));
			else if (e.cmd == "pot") { unsigned n = 0, v = 0; if (sscanf(e.arg.c_str(), "%u %i", &n, &v) == 2 && n < 8) m.adc_pot[n] = u16(v & 0x7fff); else fprintf(stderr, "script: bad pot '%s' (want: pot INPUT VALUE)\n", e.arg.c_str()); }
			else fprintf(stderr, "script: unknown command '%s'\n", e.cmd.c_str());
		}
		if (sev_next < sev.size()) script_timer.adjust(attotime(attotime::T(sev[sev_next].t)) - shim_sched().time());
	});

	// ---- operator automation of the original front panel (--autoload-bank / --preset) ----
	ui_macro macro;
	macro.panel = [&](std::string const &n, bool down) { int const c = panel_code(n); m.scanner_push(down ? 0xc0 : 0xc3); m.scanner_push(u8(c)); };
	macro.extra = [&](std::string const &n, bool down) { int const b = extra_bit(n); if (down) m.extra_switches |= u8(b); else m.extra_switches &= u8(~b); };
	macro.lcd = [&] { return m.lcd_text(0) + "\n" + m.lcd_text(1); };
	macro.m_log = [&](double t, std::string const &what) { fprintf(stderr, "UI %7.3f %-34s LCD [%s] [%s]\n", t, what.c_str(), m.lcd_text(0).c_str(), m.lcd_text(1).c_str()); };
	auto keypad = [](int d) { return "Keypad " + std::to_string(d); };
	std::string const ready_re = "^P\\d\\d .*\\nS\\d\\d ";              // main play screen "Pnn ... / Snn ..."
	if (g_opt.autoload_bank >= 0 || g_opt.preset >= 0)
		macro.wait(ready_re, 60.0, "main screen after boot");
	if (g_opt.autoload_bank >= 0)
	{
		int const b = g_opt.autoload_bank, d = g_opt.autoload_drive;
		char bb[16]; snprintf(bb, sizeof(bb), "B%02d ", b % 100);
		macro.press("Load Bank");            macro.wait("LOAD BANK from", 10.0, "LOAD BANK screen");
		macro.extra_press("Enter");          macro.wait("Select a Drive", 10.0, "drive selection");
		macro.press(keypad(d));              macro.wait("\\nD" + std::to_string(d) + " ", 10.0, "drive D" + std::to_string(d));
		macro.extra_press("Enter");          macro.wait("Select a Bank", 30.0, "bank list of drive D" + std::to_string(d));
		macro.press(keypad(b / 10 % 10));    macro.press(keypad(b % 10));
		macro.wait(bb, 10.0, std::string("bank ") + bb);
		macro.extra_press("Enter");          macro.wait("Loading Bank", 10.0, "loading");
		macro.wait(ready_re, 120.0, "main screen with the loaded bank");
	}
	if (g_opt.preset >= 0)
	{
		char pp[16]; snprintf(pp, sizeof(pp), "^P%02d ", g_opt.preset % 100);
		macro.press(keypad(g_opt.preset / 10 % 10)); macro.press(keypad(g_opt.preset % 10));
		macro.wait(pp, 10.0, std::string("preset ") + (pp + 1));
	}
	emu_timer macro_timer([&](s32) {
		double const now_s = double(shim_sched().time().m_seconds);
		if (!macro.tick(now_s))
		{
			macro_timer.adjust(attotime::never);
			if (macro.failed()) { fprintf(stderr, "AUTOLOAD FAILED: %s\n", macro.error().c_str()); g_stop = 2; rt.request_stop(2); }
			else fprintf(stderr, "AUTOLOAD done at %.3f s: [%s] [%s]\n", now_s, m.lcd_text(0).c_str(), m.lcd_text(1).c_str());
		}
	});

	// power on: resolve/start/reset the devices in MAME order, start the MIDI RxD clock if there is MIDI
	rt.power_on();
	if (inject_at >= 0) inject_timer.adjust(attotime::from_double(inject_at));
	if (!sev.empty()) script_timer.adjust(attotime(attotime::T(sev[0].t)));
	if (!macro.empty()) macro_timer.adjust(attotime::from_msec(10), 0, attotime::from_msec(10));

	// live MIDI: bytes are clocked in by the same 31250-baud RxD timer as scheduled MIDI
	midi_input midi_in;
	if (!g_opt.midi_in.empty() && !midi_in.open(g_opt.midi_in)) return 3;

	audio_output audio;
	if (!g_opt.audio_out.empty())
	{
		if (!audio.open(g_opt.audio_out, 48000, g_opt.period_frames, g_opt.periods)) return 3;
		fprintf(stderr, "audio: %s 48000 Hz stereo S16, period %u frames, buffer %u frames (%.1f ms)\n", g_opt.audio_out.c_str(), audio.period(), audio.buffer(), audio.buffer() / 48.0);
		audio.on_xrun = [&](unsigned n) { if (n <= 10) fprintf(stderr, "audio: XRUN #%u at %.3f s emulated%s\n", n, double(cpu.local_time().m_seconds), n == 10 ? " (further XRUNs counted only)" : ""); };
	}
	if (midi_in.is_open()) fprintf(stderr, "MIDI in: %s -> Z85230 channel A RxD (31250 baud)\n", g_opt.midi_in.c_str());

	// WAV output is streamed (no whole-run buffering)
	FILE *wav = nullptr; u64 wav_frames = 0;
	if (!wavfile.empty())
	{
		wav = fopen(wavfile.c_str(), "wb");
		if (!wav) { perror(wavfile.c_str()); return 2; }
		u8 hdr[44] = {}; fwrite(hdr, 1, 44, wav);
	}
	double t_wait = 0;   // time blocked on the audio device / wall clock (idle)
	std::vector<s16> pcm16;
	auto consume_audio = [&]() {
		auto &a2 = backend.audio();
		if (a2.empty()) return;
		if (wav)
		{
			pcm16.resize(a2.size());
			for (size_t k2 = 0; k2 < a2.size(); ++k2) { long v = lround(a2[k2] * 32767.0); pcm16[k2] = s16(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
			fwrite(pcm16.data(), 2, pcm16.size(), wav);
			wav_frames += a2.size() / 2;
		}
		if (audio.is_open()) { auto const tw = std::chrono::steady_clock::now(); audio.write(a2.data(), a2.size() / 2); t_wait += std::chrono::duration<double>(std::chrono::steady_clock::now() - tw).count(); }
		a2.clear();
	};
	std::signal(SIGINT, on_sigint);
	std::signal(SIGTERM, on_sigint);

	u64 const end_cyc = cycles > 0 ? u64(std::ceil(cycles)) : ~0ull;
	// host I/O granularity (audio out, MIDI in, pacing): 1 ms, kept in integer CPU cycles so a boundary is reached exactly
	u64 const chunk_cycles = CPU_HZ / 1000;
	u64 chunk_end_cyc = chunk_cycles;
	auto chunk_time = [&](u64 c) { return attotime(attotime::T(c) / attotime::T(CPU_HZ)); };
	attotime chunk_end = chunk_time(chunk_end_cyc);
	auto const wall0 = std::chrono::steady_clock::now();
	double t_cpu = 0, t_audio = 0;
	std::vector<u8> midi_bytes;
	while (rt.cycles() < end_cyc && !g_stop && !rt.stop_code())
	{
		if (rt.cycles() >= chunk_end_cyc)
		{
			// ---- chunk boundary: host I/O only; no emulated state is touched except queuing MIDI bytes ----
			auto const ta = std::chrono::steady_clock::now();
			rt.render_audio();
			consume_audio();
			if (midi_in.is_open() && midi_in.read(midi_bytes))
			{
				for (u8 b2 : midi_bytes)
				{
					rt.midi_in(&b2, 1);
					if (g_opt.verbose_midi) fprintf(stderr, "MIDI-CHAIN %.6f host byte from %s: %02X\n", rt.seconds(), g_opt.midi_in.c_str(), b2);
				}
				midi_bytes.clear();
			}
			if (g_opt.realtime && !audio.is_open())
			{
				auto const due = wall0 + std::chrono::duration<double>(double(chunk_end.m_seconds));
				auto const tw = std::chrono::steady_clock::now();
				std::this_thread::sleep_until(due);
				t_wait += std::chrono::duration<double>(std::chrono::steady_clock::now() - tw).count();
			}
			t_audio += std::chrono::duration<double>(std::chrono::steady_clock::now() - ta).count();
			while (chunk_end_cyc <= rt.cycles()) chunk_end_cyc += chunk_cycles;
			chunk_end = chunk_time(chunk_end_cyc);
		}
		rt.run_until_cycle(std::min(chunk_end_cyc, end_cyc));
	}
	if (rt.stop_code() == 2) g_stop = 2;
	double const wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
	t_cpu = wall - t_audio;
	if (g_stop == 1) fprintf(stderr, "stopped at %.3f s emulated\n", double(cpu.local_time().m_seconds));
	fprintf(stderr, "hd writes kept in overlay (image untouched): %llu\n", (unsigned long long)R.hd_img->writes());
	fprintf(stderr, "OUTPUT register-file writes %llu, F-Chip control writes %llu, CV DAC writes %llu\n", (unsigned long long)m.n_rf, (unsigned long long)m.n_ctrl, (unsigned long long)m.n_cv);
	backend.render_until(m.t20());
	consume_audio();
	if (wav)
	{
		u32 const bytes = u32(wav_frames * 4);
		fseek(wav, 0, SEEK_SET);
		auto p32 = [&](u32 v) { fwrite(&v, 4, 1, wav); }; auto p16 = [&](u16 v) { fwrite(&v, 2, 1, wav); };
		fwrite("RIFF", 1, 4, wav); p32(36 + bytes); fwrite("WAVEfmt ", 1, 8, wav); p32(16); p16(1); p16(2); p32(48000); p32(48000 * 4); p16(4); p16(16);
		fwrite("data", 1, 4, wav); p32(bytes);
		fclose(wav);
		fprintf(stderr, "wrote %s: %.2f s stereo from the software CEM backend\n", wavfile.c_str(), wav_frames / 48000.0);
	}
	double const emu_s = double(cpu.local_time().m_seconds);
	if (g_opt.stats || audio.is_open() || g_opt.realtime)
	{
		double const busy = wall - t_wait;
		fprintf(stderr, "STATS emulated %.3f s in %.3f s wall (%.2fx%s); busy %.3f s = %.0f %% load -> %.2fx realtime capacity; "
			"emulation+timers+CEM-on-write %.3f s, chunk render+audio+MIDI %.3f s; XRUNs %u\n",
			emu_s, wall, emu_s / wall, (audio.is_open() || g_opt.realtime) ? ", paced" : "", busy, 100.0 * busy / wall, emu_s / busy,
			t_cpu, t_audio - t_wait, audio.xruns());
	}
	if (g_opt.stats)
	{
		fprintf(stderr, "STATS ADC pot reads (mux input X0..X7):");
		for (int i = 0; i < 8; ++i) fprintf(stderr, " %llu", m.adc_pot_reads[i]);
		fprintf(stderr, "\n");
	}
	if (g_stop == 2) return 4;
	printf("ran %llu cycles, %llu instructions; PC=%06X PSR=%04X irq_latch=%02X unmapped=%llu\n",
		(unsigned long long)cpu.total_cycles(), g_insns, unsigned(cpu.state_pc()), 0u, m.irq_latch, m.unmapped);
	printf("LCD: [%s]\n     [%s]\n", m.lcd_text(0).c_str(), m.lcd_text(1).c_str());
	if (g_pc) fclose(g_pc);
	if (logf) fclose(logf);
	return 0;
}
