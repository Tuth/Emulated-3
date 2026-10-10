// eiii::engine — one emulated EIII on its own thread, for hosts with separate audio and GUI threads (CLAP plugin,
// standalone --gui).  The engine thread owns the runtime (builds, boots and runs it); other threads only:
//   GUI/main thread : post() hardware inputs, snapshot() the panel state, power()
//   audio thread    : midi() timestamped MIDI IN bytes, pull() audio frames — never blocks in realtime mode,
//                     never allocates, never locks.
// Latency (max(1024 frames at 48 kHz, 2 x block + 512)): the engine keeps exactly `latency()` frames ahead of the audio thread; MIDI posted at block offset s is
// applied at that frame + latency, i.e. sample-accurately delayed by the reported latency.
#pragma once
#include "eiii_runtime.h"
#include "eiii_lockfree.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace eiii {

// one hardware input of the front panel
struct panel_input
{
	enum kind_t : uint8_t { SCAN, XSW, ADC };
	kind_t kind;
	uint8_t code;       // SCAN: scanner code; XSW: bit mask; ADC: multiplexer input X0..X7
	uint8_t down;       // SCAN/XSW: 1 = make, 0 = break
	uint16_t value;     // ADC: firmware units 0..0x7FFF
};

// removable / boot media of the emulated unit
enum class media_slot : uint8_t { cd, hd, rom };
char const *media_slot_name(media_slot s);          // "cd", "hd", "rom_dir"

struct media_request;

// outcome of an engine::set_media() request
struct media_result
{
	media_slot slot = media_slot::cd;
	bool ok = false;
	bool restarted = false;         // the machine was restarted from power-on to use it (HD / ROM change while on)
	std::string path;               // requested path ("" = CD ejected)
	std::string error;              // why it was refused (the previous media stay)
};

struct engine_config
{
	runtime_config rt;
	size_t max_block = 1024;        // largest host block (frames)
	bool realtime = true;           // false: pull() waits for the engine (offline render)
};

struct engine_stats
{
	uint64_t frames_out = 0, underruns = 0, underrun_frames = 0, first_underrun_at = 0, dropped_frames = 0, midi_dropped = 0, panel_dropped = 0;
	double emu_seconds = 0, busy_fraction = 0;
};

class engine
{
public:
	engine();
	~engine();
	engine(engine const &) = delete;
	engine &operator=(engine const &) = delete;

	void start(engine_config const &cfg);   // spawn the thread and power on (boot runs on the engine thread)
	void shutdown();                        // stop and join (idempotent)
	bool started() const { return m_thread.joinable(); }
	double sample_rate() const { return m_cfg.rt.sample_rate; }

	// ---- main/GUI thread ----
	// false if the queue is full (input dropped and counted).  Applied `latency()` frames after the audio thread's
	// current position, like MIDI, so a run is deterministic for a given input timing.
	bool post(panel_input const &in);
	bool snapshot(panel_snapshot &out);     // latest published state; true if new since the last call
	void power(bool on);                    // host POWER switch: off = silence + blank panel, on = cold boot
	bool powered() const { return m_power_req.load(); }
	engine_stats stats() const;

	// Media changes (main thread), applied on the engine thread between emulation steps (never on the audio thread):
	//   cd  : hot swap like a real disc change; "" ejects.  The original OS sees UNIT ATTENTION / NOT READY.
	//   hd, rom: the unit's boot media; a running machine restarts from power-on with them (the OS lives on the HD).
	// Unusable images are refused and the current media stay.  Each request produces one media_result.
	// Before start() (or after shutdown()) the change only updates the configuration the next start uses.
	bool set_media(media_slot slot, std::string const &path);   // false: too many requests in flight (try later)
	bool poll_media_result(media_result &out);                  // next completed request, if any
	std::string media_path(media_slot slot) const;              // media in use as last reported (main thread view)

	// ---- audio thread ----
	unsigned latency() const { return m_latency; }
	void set_realtime(bool rt) { m_realtime = rt; }  // offline (false): pull() waits for the engine
	bool midi(uint32_t block_offset, uint8_t const *bytes, uint8_t n);   // call before pull() for that block
	size_t pull(float *left, float *right, size_t frames);               // always fills `frames` (silence on underrun)

private:
	struct midi_record { uint64_t ring_frame; uint8_t n; uint8_t b[3]; };
	struct panel_record { uint64_t ring_frame; panel_input in; };
	void thread_main();
	bool boot(std::string &err);
	void publish(bool running, char const *status);
	void step();
	void apply_media(media_request &rq);

	engine_config m_cfg;
	unsigned m_latency = 512;
	std::thread m_thread;
	std::atomic<bool> m_quit{false};
	std::atomic<bool> m_power_req{true};
	bool m_power_state = false;

	std::unique_ptr<runtime> m_rt;           // engine thread only
	int64_t m_ring_to_emu = 0;               // emulated frame (current runtime) = ring frame + m_ring_to_emu
	uint64_t m_emu_frames = 0;               // frames produced by the current runtime

	audio_ring m_ring{1 << 15};
	spsc_queue<midi_record> m_midi{4096};
	spsc_queue<panel_record> m_panel{1024};
	triple_buffer<panel_snapshot> m_snap;
	spsc_queue<media_request *> m_media_in{16}, m_media_out{16};   // main -> engine -> main (ownership travels)
	unsigned m_media_inflight = 0;           // main thread
	std::string m_media_view[3];             // main thread: paths in use as last reported
	panel_snapshot m_work{};

	std::atomic<int64_t> m_last_pull_ns{0};
	std::atomic<bool> m_realtime{true};
	std::atomic<bool> m_primed{false};          // the ring has been filled to the latency once (underruns count from then)

	std::atomic<uint64_t> m_first_underrun_at{0};
	std::atomic<uint64_t> m_underruns{0}, m_underrun_frames{0}, m_frames_out{0}, m_dropped{0}, m_midi_dropped{0}, m_panel_dropped{0};
	std::atomic<double> m_emu_seconds{0}, m_busy{0};
	std::vector<float> m_silence;
};

} // namespace eiii
