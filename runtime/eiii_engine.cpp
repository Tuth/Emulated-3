// eiii::engine — see eiii_engine.h.
#include "eiii_engine.h"
#include <algorithm>
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

struct media_request { media_result r; std::function<void(runtime &)> fn; };

char const *media_slot_name(media_slot s)
{
	switch (s) { case media_slot::cd: return "cd"; case media_slot::hd: return "hd"; case media_slot::rom: return "rom_dir";
	case media_slot::scsi: return "scsi"; case media_slot::cem_model: return "cem_model"; case media_slot::restart: return "restart"; case media_slot::inspect: return "inspect"; case media_slot::hd_changes: return "hd_changes"; }
	return "?";
}

engine::engine() = default;
engine::~engine()
{
	shutdown();
	media_request *rq;
	while (m_media_in.pop(rq)) delete rq;
	while (m_media_out.pop(rq)) delete rq;
}

bool engine::queue_media(media_request *rq)
{
	if (m_media_inflight >= 8) { delete rq; return false; }
	if (!m_thread.joinable())
	{
		apply_media(*rq, false);                     // no engine thread: only the configuration of the next start changes
		m_media_out.push(rq);
	}
	else m_media_in.push(rq);
	++m_media_inflight;
	return true;
}

bool engine::set_media(media_slot slot, std::string const &path)
{
	auto *rq = new media_request;
	rq->r.slot = slot; rq->r.path = path;
	if (slot == media_slot::cd) { rq->r.slot = media_slot::scsi; rq->r.scsi_id = 3; rq->r.kind = scsi_kind::cd; }
	if (slot == media_slot::hd) { rq->r.slot = media_slot::scsi; rq->r.scsi_id = 1; rq->r.kind = scsi_kind::hd; }
	return queue_media(rq);
}

bool engine::set_scsi(unsigned id, scsi_kind kind, std::string const &path)
{
	auto *rq = new media_request;
	rq->r.slot = media_slot::scsi; rq->r.scsi_id = uint8_t(id); rq->r.kind = kind; rq->r.path = path;
	return queue_media(rq);
}

bool engine::poll_media_result(media_result &out)
{
	media_request *rq;
	if (!m_media_out.pop(rq)) return false;
	--m_media_inflight;
	out = std::move(rq->r);
	delete rq;
	if (out.ok)
		switch (out.slot)
		{
		case media_slot::rom: m_view.rom_dir = out.path; break;
		case media_slot::cem_model: m_view.cem_model = out.path; break;
		case media_slot::scsi:
			if (out.scsi_id == 1) m_view.hd_path = out.path;
			else if (out.scsi_id == 3) m_view.cd_path = out.path;
			else if (out.scsi_id < 8) m_view.scsi[out.scsi_id] = { out.kind, out.kind == scsi_kind::none ? std::string() : out.path };
			break;
		default: break;
		}
	return true;
}

bool engine::set_hd_changes(unsigned id, std::string const &changes)
{
	auto *rq = new media_request;
	rq->r.slot = media_slot::hd_changes; rq->r.scsi_id = uint8_t(id); rq->r.path = changes;
	return queue_media(rq);
}

std::string engine::hd_changes(unsigned id) const { std::lock_guard<std::mutex> g(m_hd_mutex); return id < 8 ? m_hd_pub[id] : std::string(); }
std::string engine::take_warning() { std::lock_guard<std::mutex> g(m_hd_mutex); std::string w; w.swap(m_warning); return w; }

void engine::keep_hd_changes()
{
	if (!m_rt) return;
	for (unsigned id = 1; id < 8; ++id)
		if (m_rt->drive_kind(id) == scsi_kind::hd) m_cfg.rt.hd_changes[id] = m_rt->hd_changed_sectors(id) ? m_rt->hd_changes(id) : std::string();
}

// all: right after a boot (the change sets now in use); else the ones the OS changed, once writing has paused
void engine::publish_hd_changes(bool all)
{
	int64_t const now = now_ns();
	for (unsigned id = 1; id < 8; ++id)
	{
		bool const hd = m_rt && m_rt->drive_kind(id) == scsi_kind::hd;
		if (all)
		{
			std::string const blob = hd && m_rt->hd_changed_sectors(id) ? m_rt->hd_changes(id) : std::string();
			m_hd_seen[id] = hd ? m_rt->hd_writes(id) : 0; m_hd_dirty_ns[id] = 0;
			std::lock_guard<std::mutex> g(m_hd_mutex);
			if (m_hd_pub[id] != blob) { m_hd_pub[id] = blob; ++m_hd_serial; }
			continue;
		}
		if (!hd) continue;
		uint64_t const w = m_rt->hd_writes(id);
		if (w != m_hd_seen[id]) { m_hd_seen[id] = w; m_hd_dirty_ns[id] = now; continue; }
		if (m_hd_dirty_ns[id] && now - m_hd_dirty_ns[id] > 300'000'000)
		{
			m_hd_dirty_ns[id] = 0;
			std::string blob = m_rt->hd_changes(id);
			std::lock_guard<std::mutex> g(m_hd_mutex);
			m_hd_pub[id].swap(blob); ++m_hd_serial;
		}
	}
}

bool engine::inspect(std::function<void(runtime &)> f)
{
	auto *rq = new media_request;
	rq->r.slot = media_slot::inspect; rq->fn = std::move(f);
	return queue_media(rq);
}

// ---- sampling input ----
void engine::input(float const *left, float const *right, size_t frames)
{
	// the block's first frame is at input index = ring read index (ring frame - latency): pad / skip to stay aligned
	uint64_t const want = m_ring.read_index();
	float tmp[2 * 256];
	uint64_t w = m_in.write_index();
	while (w < want && m_in.space())
	{
		size_t const n = size_t(std::min<uint64_t>({ want - w, 256, m_in.space() }));
		std::fill(tmp, tmp + 2 * n, 0.0f);
		w += m_in.write(tmp, n);
	}
	size_t i = w > want ? size_t(std::min<uint64_t>(frames, w - want)) : 0;
	while (i < frames && m_in.space())
	{
		size_t const n = std::min<size_t>({ frames - i, 256, m_in.space() });
		auto clean = [](float v) { return std::fabs(v) < 1e-20f ? 0.0f : v; };   // no denormals / tiny values downstream
		for (size_t k = 0; k < n; ++k) { tmp[2 * k] = left ? clean(left[i + k]) : 0.0f; tmp[2 * k + 1] = right ? clean(right[i + k]) : left ? clean(left[i + k]) : 0.0f; }
		m_in.write(tmp, n);
		i += n;
	}
}

// engine thread: move new input frames into m_in_buf, drop what the emulation has passed
void engine::drain_input()
{
	float l[256], r[256];
	for (;;)
	{
		size_t const n = m_in.read(l, r, 256);
		if (!n) break;
		for (size_t k = 0; k < n; ++k) { m_in_buf.push_back(l[k]); m_in_buf.push_back(r[k]); }
		m_in_end += n;
	}
	uint64_t const base = m_in_end - m_in_buf.size() / 2;
	int64_t const cur = int64_t(m_emu_frames) - m_ring_to_emu - int64_t(m_latency) - 64;   // input index now in use
	if (!m_rt) { m_in_buf.clear(); return; }
	if (cur > int64_t(base))
	{
		size_t const drop = size_t(std::min<uint64_t>(uint64_t(cur) - base, m_in_buf.size() / 2));
		m_in_buf.erase(m_in_buf.begin(), m_in_buf.begin() + long(2 * drop));
	}
}

// engine thread, inside the emulation: the analog input at a conversion (emulated seconds)
void engine::adc_sample(double t, int16_t lr[2])
{
	lr[0] = lr[1] = 0;
	if (!m_rt) return;
	double const idx = t * m_cfg.rt.sample_rate - double(m_ring_to_emu) - double(m_latency);   // input index
	// the audio thread hands over each block's input before it pulls that block, so frames the emulation may reach
	// are in the ring by now; fetch them if this step started before they arrived
	if (idx + 1 >= double(m_in_end)) drain_input();
	if (m_in_buf.size() < 4) return;
	double const pos = idx - double(m_in_end - m_in_buf.size() / 2);
	if (pos < 0 || pos >= double(m_in_buf.size() / 2 - 1)) return;
	size_t const i = size_t(pos);
	float const f = float(pos - double(i));
	// input level: the Sample Input slider (ADC multiplexer X0).  The original dbx 2155 gain law is not documented;
	// assumed: centre of travel = unity, +-20 dB at the ends (log taper), silent at the very bottom.
	double const p = double(m_rt->adc_input(0)) / double(0x7100);
	double const gain = p < 0.01 ? 0.0 : std::pow(10.0, (std::min(p, 1.0) - 0.5) * 2.0);
	for (int c = 0; c < 2; ++c)
	{
		float const x = m_in_buf[2 * i + c] + (m_in_buf[2 * (i + 1) + c] - m_in_buf[2 * i + c]) * f;
		double const v = std::lround(double(x) * gain * 32767.0);
		lr[c] = int16_t(std::clamp(v, -32768.0, 32767.0));
	}
}

std::string engine::media_path(media_slot slot) const
{
	switch (slot)
	{
	case media_slot::cd: return m_view.cd_path;
	case media_slot::hd: return m_view.hd_path;
	case media_slot::rom: return m_view.rom_dir;
	case media_slot::cem_model: return m_view.cem_model;
	default: return {};
	}
}

scsi_target engine::scsi(unsigned id) const
{
	if (id == 1) return { scsi_kind::hd, m_view.hd_path };
	if (id == 3) return { scsi_kind::cd, m_view.cd_path };
	return id < 8 && scsi_extra_id(id) ? m_view.scsi[id] : scsi_target{};
}

// engine thread (live) or main thread with no engine thread (configuration only)
void engine::apply_media(media_request &rq, bool live)
{
	media_result &r = rq.r;
	runtime_config &c = m_cfg.rt;
	std::string err;
	bool restart = false, ok = false;
	runtime *const rt = live ? m_rt.get() : nullptr;
	if (rt && r.slot != media_slot::inspect) keep_hd_changes();   // a restart below must not lose what the OS wrote
	switch (r.slot)
	{
	case media_slot::hd_changes:
		ok = r.scsi_id == 1 || (r.scsi_id < 8 && scsi_extra_id(r.scsi_id) && c.scsi[r.scsi_id].kind == scsi_kind::hd);
		if (!ok) err = "no hard disk at SCSI " + std::to_string(r.scsi_id);
		else { c.hd_changes[r.scsi_id] = r.path; restart = true; }
		break;
	case media_slot::rom:
		ok = runtime::check_rom_dir(r.path, err);
		if (ok) { c.rom_dir = r.path; restart = true; }
		break;
	case media_slot::cem_model:
		ok = r.path == "v1" || r.path == "v2";
		if (!ok) err = "analog model must be v1 or v2";
		else { c.cem_model = r.path; restart = true; }
		break;
	case media_slot::restart:
		ok = true; restart = true;
		break;
	case media_slot::inspect:
		ok = rt != nullptr;
		if (ok && rq.fn) rq.fn(*rt);
		if (!ok) err = "no machine running";
		break;
	case media_slot::scsi:
	{
		unsigned const id = r.scsi_id;
		if (id == 1)
		{
			if (r.kind != scsi_kind::hd || r.path.empty()) err = "SCSI 1 is the OS hard disk: choose a hard disk image";
			else if ((ok = runtime::check_hd_image(r.path, err))) { if (r.path != c.hd_path) c.hd_changes[1].clear(); c.hd_path = r.path; restart = true; }
		}
		else if (id == 3)
		{
			if (r.kind != scsi_kind::cd) err = "SCSI 3 is the unit's CD-ROM drive";
			else if (r.path.empty()) { if (rt) rt->eject_cd(3); c.cd_path.clear(); ok = true; }
			else if (rt) { if ((ok = rt->mount_cd(3, r.path, err))) c.cd_path = r.path; }
			else if ((ok = runtime::check_cd_image(r.path, err))) c.cd_path = r.path;
		}
		else if (id < 8 && scsi_extra_id(id))
		{
			scsi_target &t = c.scsi[id];
			bool const hot = r.kind == scsi_kind::cd && t.kind == scsi_kind::cd && rt && rt->drive_kind(id) == scsi_kind::cd;
			if (r.kind == scsi_kind::hd) ok = !r.path.empty() && runtime::check_hd_image(r.path, err);
			else if (r.kind == scsi_kind::cd) ok = r.path.empty() || runtime::check_cd_image(r.path, err);
			else ok = true;
			if (r.kind == scsi_kind::hd && r.path.empty()) err = "choose a hard disk image";
			if (ok && hot) ok = r.path.empty() ? rt->eject_cd(id) : rt->mount_cd(id, r.path, err);
			if (ok)
			{
				restart = !hot && !(r.kind == t.kind && r.path == t.path && r.kind == scsi_kind::none);
				if (r.kind != t.kind || r.path != t.path) c.hd_changes[id].clear();   // another disk: its own changes
				t = { r.kind, r.kind == scsi_kind::none ? std::string() : r.path };
			}
		}
		else err = "SCSI ID must be 1, 3 or 4..7 (the OS does not use D2)";
		break;
	}
	default: err = "unsupported request"; break;
	}
	r.ok = ok;
	r.error = ok ? "" : err;
	if (!ok || !restart || !live || !m_power_state) return;
	// the OS and its file system state live on the HD and it finds its drives at boot: restart like a power cycle
	m_rt.reset();
	std::string berr;
	if (!boot(berr)) r.error = berr;
	publish(m_rt != nullptr, berr.c_str());
	r.restarted = true;
	if (m_rt && r.slot == media_slot::hd_changes && !m_rt->open_warning().empty()) { r.ok = false; r.error = m_rt->open_warning(); }
}

void engine::start(engine_config const &cfg)
{
	shutdown();
	// A fresh activation must not play audio or apply MIDI left in the previous host session.
	m_ring.reset(); m_in.reset();
	midi_record mi; while (m_midi.pop(mi)) { }
	panel_record pi; while (m_panel.pop(pi)) { }
	media_request *rq;
	while (m_media_in.pop(rq)) delete rq;
	while (m_media_out.pop(rq)) delete rq;
	m_media_inflight = 0;
	m_cfg = cfg;
	m_realtime = cfg.realtime;
	double const sr = cfg.rt.sample_rate;
	unsigned const min_lat = unsigned(std::lround(1024.0 * sr / 48000.0));
	m_latency = std::max<unsigned>(min_lat, unsigned(2 * cfg.max_block + 512));
	if (m_latency > m_ring.capacity() / 2) m_latency = unsigned(m_ring.capacity() / 2);
	m_quit = false;
	m_primed = false;
	m_view = m_cfg.rt;
	{
		std::lock_guard<std::mutex> g(m_hd_mutex);
		for (unsigned id = 0; id < 8; ++id) m_hd_pub[id] = m_cfg.rt.hd_changes[id];
		++m_hd_serial;
	}
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
	m_rt->set_adc_source([this](double t, int16_t lr[2]) { adc_sample(t, lr); });
	if (!m_rt->open_warning().empty()) { std::lock_guard<std::mutex> g(m_hd_mutex); m_warning = m_rt->open_warning(); }
	publish_hd_changes(true);
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
	m_in_buf.clear(); m_in_end = m_in.read_index();
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
			else { keep_hd_changes(); m_rt.reset(); status = "POWER OFF"; }
			publish(m_rt != nullptr, status.c_str());
		}
		media_request *mq;
		while (m_media_in.pop(mq))
		{
			apply_media(*mq, true);
			if (mq->r.restarted) { status = mq->r.error; free_t0 = 0; }
			while (!m_media_out.push(mq)) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // bounded by m_media_inflight
		}
		drain_input();
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
		if (m_rt->stop_code() == 2) { status = "emulation stopped (watchdog)"; keep_hd_changes(); m_rt.reset(); }
		else publish_hd_changes(false);
		publish(m_rt != nullptr, status.c_str());
		m_emu_seconds = m_rt ? m_rt->seconds() : 0.0;

		int64_t const t1 = now_ns();
		busy_ns += t1 - t0;
		if (t1 - t_window > 500'000'000) { m_busy = double(busy_ns) / double(t1 - t_window); busy_ns = 0; t_window = t1; }
	}
	keep_hd_changes();
	m_rt.reset();
	// the final change sets, for a host that starts the engine again (e.g. at another sample rate)
	std::lock_guard<std::mutex> g(m_hd_mutex);
	for (unsigned id = 1; id < 8; ++id) if (m_hd_pub[id] != m_cfg.rt.hd_changes[id]) { m_hd_pub[id] = m_cfg.rt.hd_changes[id]; ++m_hd_serial; }
}

} // namespace eiii
