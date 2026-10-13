// eiii::runtime — one emulated Emulator III (original boot ROM + OS from the original disk images, MAME-equivalent
// devices, eiii-core output hardware).  Several instances can exist in one process: each binds its own shim context
// to the thread that builds and runs it (bind_thread()).  Every call below must come from that thread; other threads
// talk to an instance through eiii::engine (eiii_engine.h), which owns such a thread.
//
// Inputs are hardware inputs only — the three paths of the real front panel (scanner bytes, extra-switch bits, ADC
// multiplexer inputs) and MIDI IN bytes.  There is no API to touch RAM, voices or presets.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace eiii {

// optional extra SCSI targets.  The OS addresses SCSI ID n as drive Dn: D1 = internal HD, D3 = CD-ROM; it accepts extra
// drives at D4..D7 (D2 is always "Invalid Drive" for OS 2.42, so it is not offered).
enum class scsi_kind : uint8_t { none, hd, cd };
struct scsi_target { scsi_kind kind = scsi_kind::none; std::string path; };   // cd: "" = drive without a disc
constexpr bool scsi_extra_id(unsigned id) { return id >= 4 && id <= 7; }

struct runtime_config
{
	bool rom_only = false;         // explicit boot-ROM diagnostic without an OS disk
	std::string rom_dir;            // e3-lsboot_ip381a_emu_systems_4088.ic3 / e3-msboot_ip380a_emu_systems_4088.ic4
	std::string hd_path;            // OS 2.42 hard disk image (SCSI ID 1), opened read-only
	std::string cd_path;            // optional CD-ROM image (SCSI ID 3)
	scsi_target scsi[8];            // extra drives at IDs 4..7 (scsi_extra_id); IDs 1 and 3 are hd_path / cd_path
	std::string hd_changes[8];      // hard disk change sets to apply at open (SCSI 1, extra HDs), see hd_changes()
	std::string cem_model = "v1";   // "v1" | "v2"
	std::string cem_calib;          // v2 calibration file (optional)
	double sample_rate = 48000.0;   // output rate of the software backend
	bool midi_clock = true;         // run the 31250-baud MIDI IN bit clock from power-on (needed for live MIDI)
	// diagnostics (stderr)
	bool log_voice_starts = false, log_leds = false, log_midi_chain = false;
	FILE *log = nullptr;            // bus/device event log (debug aid)
	std::function<void(std::string const &)> on_error; // rare fatal diagnostics, on the engine thread
};

// LCD controller state as the display shows it (HD44780: DDRAM, CGRAM, address counter, display control).
struct lcd_state
{
	uint8_t ddram[0x80];
	uint8_t cgram[0x40];
	uint8_t ac;                     // address counter (cursor position when it addresses DDRAM)
	bool cgram_active;              // the address counter addresses CGRAM (cursor not shown)
	bool display_on, cursor_on, blink_on;
	uint8_t disp_shift;             // 0..79
	uint8_t num_lines;              // 1 or 2 (function set)
};

// Everything a front panel can see, published by the runtime's thread.
struct panel_snapshot
{
	lcd_state lcd;
	uint16_t led_latch = 0xffff;    // 0xDB0000 LED latch, active low
	uint8_t misc_latch = 0xff;      // 0xD70000 misc latch (bit 7 = Erase LED, active low)
	double emu_seconds = 0;
	uint64_t serial = 0;            // increments with every publish
	bool running = false;           // false before boot / after power-off / on error
	char status[128] = {};          // host message (e.g. missing media), empty when running normally
};

class runtime
{
public:
	runtime();
	~runtime();
	runtime(runtime const &) = delete;
	runtime &operator=(runtime const &) = delete;

	// Builds and resets the machine on the calling thread (which becomes the instance's thread).  On failure returns
	// false with a message; the instance must then be destroyed.
	bool open(runtime_config const &cfg, std::string &error);   // = open_unpowered() + power_on()
	bool open_unpowered(runtime_config const &cfg, std::string &error);   // build only (host timers may be added)
	void power_on();                                      // resolve/start/reset devices, start the MIDI clock
	void bind_thread();                                   // re-bind the instance's shim context (same thread or a new owner)

	// ---- hardware inputs ----
	void scanner_push(uint8_t byte);                      // panel/keyboard scanner FIFO byte (0xF10000)
	void panel_switch(uint8_t code, bool down);           // scanner make C0 code / break C3 code
	void extra_switch(uint8_t mask, bool down);           // 0xFA0000 bits (Erase 01 Record 02 Enter 08 Mode 10 ...)
	void set_adc_input(unsigned input, uint16_t value);   // IC24 multiplexer input X0..X7, firmware ADC units 0..0x7FFF
	uint16_t adc_input(unsigned input) const;
	void midi_in(uint8_t const *bytes, size_t n);         // bytes into the Z85230 channel A RxD at 31250 baud
	void schedule_midi(double t_seconds, std::vector<uint8_t> bytes);   // scheduled MIDI (regressions)

	// ---- sampling input ----
	// The ADC board's analog input: called on the instance's thread at each conversion the OS performs (44 053 or
	// 33 333 Hz, paced by ADC WAIT), with the conversion time in emulated seconds; returns the L/R ADC words.
	void set_adc_source(std::function<void(double t_seconds, int16_t lr[2])> source);

	// ---- hard disk changes ----
	// Hard disk images are opened read-only; what the OS writes (Save Bank, ...) is kept as a change set over the image.
	// It can be taken out (e.g. into a project), put back into a later instance of the same image, or written into a
	// new image file.  A change set only applies to the image it was made on (length + fingerprint).
	std::string hd_changes(unsigned scsi_id) const;     // "" when the ID has no hard disk
	size_t hd_changed_sectors(unsigned scsi_id) const;
	uint64_t hd_writes(unsigned scsi_id) const;          // sector writes since open (changes when the OS writes)
	std::string const &open_warning() const;            // e.g. a change set that did not match its image (dropped)
	static bool write_image_with_changes(std::string const &image, std::string const &changes, std::string const &out_path, std::string &error);

	// ---- diagnostics (tests): read emulated memory as the CPU sees it, no side effects on RAM / sample RAM ----
	uint16_t debug_read16(uint32_t address) const;
	uint32_t debug_read32(uint32_t address) const;   // little-endian, as the NS32016

	// ---- media (SCSI ID 3 CD-ROM drive) ----
	// A disc change as on the real drive: the next command after a mount reports UNIT ATTENTION (new medium), an empty
	// drive reports NOT READY.  An image that cannot be used is refused and the current disc stays in the drive.
	bool mount_cd(std::string const &path, std::string &error);
	void eject_cd();
	bool mount_cd(unsigned scsi_id, std::string const &path, std::string &error);   // ID 3 or an extra CD drive
	bool eject_cd(unsigned scsi_id);
	scsi_kind drive_kind(unsigned scsi_id) const;        // what is on the bus at this ID
	static bool check_cd_image(std::string const &path, std::string &error);   // readable MODE1/2048 ISO
	static bool check_hd_image(std::string const &path, std::string &error);   // readable, non-empty, whole 512-byte sectors
	static bool check_rom_dir(std::string const &dir, std::string &error);     // both boot ROM halves present

	// ---- execution ----
	void run_until_cycle(uint64_t cycle);                 // MAME-semantics slices until the CPU reaches 'cycle'
	uint64_t cycles() const;
	double seconds() const;
	static constexpr double cpu_hz = 10'000'000.0;
	int stop_code() const;                                // 0 running, 1 stop requested, 2 error (watchdog / macro)
	void request_stop(int code);

	// ---- outputs ----
	void render_audio();                                  // bring the analog model up to the current time
	std::vector<float> &audio();                          // rendered interleaved L/R, volts / 10; caller clears it
	void snapshot(panel_snapshot &out) const;

	// ---- internals (standalone diagnostics only; includes the MAME shim) ----
	struct impl;
	impl &internals() { return *m; }
private:
	std::unique_ptr<impl> m;
};

} // namespace eiii
