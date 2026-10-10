// eiii::engine — see eiii_engine.h.
#include "eiii_engine.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace eiii {

namespace {
int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void set_ftz()
{
	// as the standalone: flush denormals in the analog model (outputs proven identical, see check_runtime.sh)
#if defined(__x86_64__) || defined(__i386__)
	_mm_setcsr(_mm_getcsr() | 0x8040);
#elif defined(__aarch64__)
	uint64_t fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr)); fpcr |= (1ull << 24); __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
}
}

engine::engine() = default;
engine::~engine() { shutdown(); }

void engine::start(engine_config const &cfg)
{
	shutdown();
	m_cfg = cfg;
	m_realtime = cfg.realtime;
	double const sr = cfg.rt.sample_rate;
	unsigned const min_lat = unsigned(std::lround(1024.0 * sr / 48000.0));
	m_latency = std::max<unsigned>(min_lat, unsigned(2 * cfg.max_block + 512));
	if (m_latency > m_ring.capacity() / 2) m_latency = unsigned(m_ring.capacity() / 2);
	m_quit = false;
	m_primed = false;
	m_power_state = false;
	m_last_pull_ns = 0;
	m_thread = std::thread([this] { thread_main(); });
}

void engine::shutdown()
{
	if (!m_thread.joinable()) return;
	m_quit = true;
	m_thread.join();
}

bool engine::post(panel_input const &in)
{
	if (m_panel.push({ m_ring.read_index() + m_latency, in })) return true;
	++m_panel_dropped; return false;
}

bool engine::snapshot(panel_snapshot &out) { return m_snap.read(out); }
void engine::power(bool on) { m_power_req = on; }

engine_stats engine::stats() const
{
	engine_stats s;
	s.frames_out = m_frames_out; s.underruns = m_underruns; s.first_underrun_at = m_first_underrun_at; s.underrun_frames = m_underrun_frames; s.dropped_frames = m_dropped;
	s.midi_dropped = m_midi_dropped; s.panel_dropped = m_panel_dropped; s.emu_seconds = m_emu_seconds; s.busy_fraction = m_busy;
	return s;
}

bool engine::midi(uint32_t block_offset, uint8_t const *bytes, uint8_t n)
{
	uint64_t const f = m_ring.read_index() + block_offset + m_latency;
	bool ok = true;
	for (uint8_t i = 0; i < n; i += 3)
	{
		midi_record r{ f, uint8_t(std::min(3, n - i)), {} };
		memcpy(r.b, bytes + i, r.n);
		if (!m_midi.push(r)) { ++m_midi_dropped; ok = false; }
	}
	return ok;
}

size_t engine::pull(float *left, float *right, size_t frames)
{
	m_last_pull_ns = now_ns();
	if (!m_realtime)
		while (m_ring.fill() < frames && !m_quit && m_thread.joinable()) std::this_thread::sleep_for(std::chrono::microseconds(100));
	size_t const got = m_ring.read(left, right, frames);
	if (got < frames)
	{
		std::fill(left + got, left + frames, 0.0f); std::fill(right + got, right + frames, 0.0f);
		if (m_primed) { if (!m_underruns) m_first_underrun_at = m_frames_out.load(); ++m_underruns; m_underrun_frames += frames - got; }
	}
	if (got) m_frames_out += frames;
	return frames;
}

bool engine::boot(std::string &err)
{
	m_rt = std::make_unique<runtime>();
	if (!m_rt->open(m_cfg.rt, err)) { m_rt.reset(); return false; }
	m_emu_frames = 0;
	m_ring_to_emu = -int64_t(m_ring.write_index());
	return true;
}

void engine::publish(bool running, char const *status)
{
	if (m_rt) m_rt->snapshot(m_work);
	else
	{
		memset(&m_work.lcd, 0, sizeof m_work.lcd);
		m_work.led_latch = 0xffff; m_work.misc_latch = 0xff; m_work.emu_seconds = 0;
		++m_work.serial;
	}
	m_work.running = running && m_rt;
	snprintf(m_work.status, sizeof m_work.status, "%s", status ? status : "");
	m_snap.back() = m_work;
	m_snap.publish();
}

void engine::thread_main()
{
	set_ftz();
	{
		// a modest realtime priority when the process is allowed one (RLIMIT_RTPRIO / rtkit-granted hosts); else unchanged
		sched_param sp{}; sp.sched_priority = 10;
		pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
	}
	m_silence.assign(4096, 0.0f);
	std::string status;
	auto t_window = now_ns(); int64_t busy_ns = 0;
	int64_t free_t0 = 0; uint64_t free_f0 = 0;
	double const sr = m_cfg.rt.sample_rate;
	uint64_t const step_frames = std::max<uint64_t>(1, uint64_t(sr / 1000.0));
	while (!m_quit)
	{
		bool const want = m_power_req.load();
		if (want != m_power_state)
		{
			m_power_state = want;
			if (want) { std::string err; status = boot(err) ? "" : err; }
			else { m_rt.reset(); status = "POWER OFF"; }
			publish(m_rt != nullptr, status.c_str());
		}
		panel_record pr;
		if (!m_rt) while (m_panel.pop(pr)) { }       // no machine: switches do nothing

		int64_t const t0 = now_ns();
		uint64_t const R = m_ring.read_index(), W = m_ring.write_index();
		bool const consumer = m_last_pull_ns.load() != 0 && t0 - m_last_pull_ns.load() < 200'000'000;
		uint64_t target_ring;
		if (consumer)
		{
			free_t0 = 0;
			uint64_t const want_w = R + m_latency;
			if (W >= want_w) { m_primed = true; std::this_thread::sleep_for(std::chrono::microseconds(500)); continue; }
			target_ring = std::min(want_w, W + step_frames);
		}
		else
		{
			// nobody pulls audio (host stopped processing, or GUI-only use): run on the wall clock so the panel stays
			// alive; keep at most `latency` frames queued and drop the rest
			if (!m_rt) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
			if (!free_t0) { free_t0 = t0; free_f0 = m_emu_frames; }
			uint64_t const due = free_f0 + uint64_t(double(t0 - free_t0) * 1e-9 * sr);
			if (due <= m_emu_frames) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
			target_ring = uint64_t(int64_t(std::min(due, m_emu_frames + step_frames)) - m_ring_to_emu);
		}

		if (!m_rt)
		{
			// powered off / boot error: keep the host fed with silence at the same latency
			size_t const n = size_t(std::min<uint64_t>(target_ring - W, m_silence.size() / 2));
			m_ring.write(m_silence.data(), n);
			continue;
		}

		auto push = [&] {
			m_rt->render_audio();
			auto &a = m_rt->audio();
			size_t const n = a.size() / 2;
			size_t const w = (consumer || m_ring.fill() < m_latency) ? m_ring.write(a.data(), n) : 0;
			if (w < n) { m_dropped += n - w; m_ring_to_emu += int64_t(n - w); }
			m_emu_frames += n;
			a.clear();
		};
		auto cycle_of = [&](int64_t emu_frame) { return uint64_t(std::ceil(double(emu_frame) * runtime::cpu_hz / sr)); };

		int64_t const target_emu = int64_t(target_ring) + m_ring_to_emu;
		// timestamped inputs (MIDI from the audio thread, panel from the GUI thread), earliest first, each applied at its
		// own frame
		for (;;)
		{
			midi_record mr; bool const hm = m_midi.peek(mr);
			bool const hp = m_panel.peek(pr);
			if (!hm && !hp) break;
			bool const use_midi = hm && (!hp || mr.ring_frame <= pr.ring_frame);
			int64_t const f = int64_t(use_midi ? mr.ring_frame : pr.ring_frame) + m_ring_to_emu;
			if (f > target_emu) break;
			if (f > int64_t(m_emu_frames)) { m_rt->run_until_cycle(cycle_of(f)); push(); }
			if (use_midi) { m_rt->midi_in(mr.b, mr.n); m_midi.pop(mr); }
			else
			{
				panel_input const &in = pr.in;
				if (in.kind == panel_input::SCAN) m_rt->panel_switch(in.code, in.down);
				else if (in.kind == panel_input::XSW) m_rt->extra_switch(in.code, in.down);
				else if (in.kind == panel_input::ADC) m_rt->set_adc_input(in.code, in.value);
				m_panel.pop(pr);
			}
		}
		m_rt->run_until_cycle(cycle_of(target_emu));
		push();
		if (m_rt->stop_code() == 2) { status = "emulation stopped (watchdog)"; m_rt.reset(); }
		publish(m_rt != nullptr, status.c_str());
		m_emu_seconds = m_rt ? m_rt->seconds() : 0.0;

		int64_t const t1 = now_ns();
		busy_ns += t1 - t0;
		if (t1 - t_window > 500'000'000) { m_busy = double(busy_ns) / double(t1 - t_window); busy_ns = 0; t_window = t1; }
	}
	m_rt.reset();
}

} // namespace eiii
