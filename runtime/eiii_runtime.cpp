// eiii::runtime — see eiii_runtime.h.  Device construction, timer order and the slice loop are the ones the
// standalone has been proven with (instruction-identical to MAME); only process globals became instance state.
#include "eiii_runtime_impl.h"
#include <algorithm>

namespace eiii {

void runtime::impl::bind()
{
	g_shim_sched = &sched;
	device_memory_interface::s_host_space = &m;
	device_t::s_log = (cfg.log && getenv("EIII_DEVLOG")) ? cfg.log : nullptr;
}

bool runtime::impl::open(runtime_config const &c, std::string &err)
{
	cfg = c;
	bind();
	m.log = cfg.log;
	m.log_voice_starts = cfg.log_voice_starts;
	m.log_leds = cfg.log_leds;
	m.log_midi_chain = cfg.log_midi_chain;

	auto load = [&](char const *name, int lane) {
		std::string const path = cfg.rom_dir + "/" + name;
		FILE *f = fopen(path.c_str(), "rb");
		if (!f) { err = "cannot open boot ROM " + path; return false; }
		std::vector<u8> b(0x4000); size_t const n = fread(b.data(), 1, b.size(), f); fclose(f);
		for (size_t i = 0; i < n; ++i) m.rom[i] = lane ? u16((m.rom[i] & 0x00ff) | (b[i] << 8)) : u16((m.rom[i] & 0xff00) | b[i]);
		return true;
	};
	if (!load("e3-lsboot_ip381a_emu_systems_4088.ic3", 0) || !load("e3-msboot_ip380a_emu_systems_4088.ic4", 1)) return false;

	cpu = std::make_unique<ns32016_device>(mcfg, "maincpu", nullptr, 10'000'000);
	fpu = std::make_unique<ns32081_device>(mcfg, "fpu", nullptr, 10'000'000);
	cpu->set_fpu(static_cast<ns32000_fpu_interface *>(fpu.get()));
	m.cpu = cpu.get(); m.fpu = fpu.get();
	sched.m_cpu = cpu.get();

	// SCSI exactly as emu3.cpp: NCR5380 = external device 0, harddisk on scsi:1, cdrom on scsi:3 (block size 512)
	scsi = std::make_unique<nscsi_bus_device>(mcfg, "scsi", nullptr);
	hd.reset(eiii_make_hd(mcfg, "harddisk", nullptr));
	hd_img = std::make_unique<harddisk_image_device>(mcfg, "image", hd.get());
	cd.reset(eiii_make_cd(mcfg, "cdrom", nullptr));
	cd_img = std::make_unique<cdrom_image_device>(mcfg, "image", cd.get());
	cd_cdda = std::make_unique<cdda_device>(mcfg, "cdda", cd.get());
	cd->set_block_size(512);
	hdc = std::make_unique<ncr5380_device>(mcfg, "ncr5380", nullptr);
	// SCC exactly as emu3.cpp: SCC85230 at 16 MHz / 4, int output inverted into SCCINT; channels are child devices
	scc = std::make_unique<scc85230_device>(mcfg, "scc", nullptr, 16'000'000 / 4);
	scc_cha = std::make_unique<z80scc_channel>(mcfg, "cha", scc.get(), 0);
	scc_chb = std::make_unique<z80scc_channel>(mcfg, "chb", scc.get(), 0);
	m.scc = scc.get();
	machine_t *const mp = &m;
	scc->out_int_callback().set([mp](int s) { mp->irq_w(machine_t::SCCINT, !s); });
	m.hdc = hdc.get();
	if (cfg.hd_path.empty()) { err = "no OS hard disk image configured"; return false; }
	if (!hd_img->load(cfg.hd_path)) { err = "cannot open hard disk image " + cfg.hd_path; return false; }
	if (!cfg.cd_path.empty() && !cd_img->load(cfg.cd_path)) { err = "cannot open CD-ROM image " + cfg.cd_path; return false; }
	scsi->set_external_device(0, static_cast<nscsi_device_interface *>(hdc.get()));
	scsi->set_external_device(1, static_cast<nscsi_device_interface *>(hd.get()));
	scsi->set_external_device(3, static_cast<nscsi_device_interface *>(cd.get()));
	hdc->irq_handler().set([mp](int s) { mp->irq_w(machine_t::HDINT, s); });

	// PIT clocks as in the MAME driver: ch0 20 MHz/2, ch1 16 MHz/16, ch2 unclocked
	m.pit[0].clock_hz = 10e6; m.pit[1].clock_hz = 1e6; m.pit[2].clock_hz = 0;
	m.pit[0].out_cb = [mp](bool o) { mp->irq_w(machine_t::TGINT, o); };
	m.pit[1].out_cb = [mp](bool o) { if (mp->pit1_last && !o && !BIT(mp->misc, 6)) mp->irq_w(machine_t::STINT, 0); mp->pit1_last = o; };

	// output hardware -> software CEM backend
	backend = std::make_unique<software_backend>(m.snd.data(), m.snd.size(), cfg.sample_rate);
	backend->start_at(0);
	if (cfg.cem_model == "v2")
	{
		cem_calibration cal;
		if (!cfg.cem_calib.empty()) { std::string const e = cal.load(cfg.cem_calib); if (!e.empty()) { err = "cem calibration: " + e; return false; } }
		backend->use_model_v2(cal);
	}
	else if (cfg.cem_model != "v1") { err = "cem model must be v1 or v2"; return false; }
	m.out = backend.get();

	// timers in the order the proven standalone created them: MIDI RxD clock, then the scanner IRQ re-assert
	midi_timer = std::make_unique<emu_timer>([this](s32) { midi_tick(); });
	scan_irq_timer = std::make_unique<emu_timer>([mp](s32) { if (mp->scan_count) mp->irq_w(machine_t::SCNINT, 0); });
	m.scan_timer = scan_irq_timer.get();
	order = { cpu.get(), fpu.get(), scsi.get(), hd.get(), hd_img.get(), cd.get(), cd_img.get(), cd_cdda.get(), hdc.get(), scc.get(), scc_cha.get(), scc_chb.get() };
	return true;
}

void runtime::impl::power_on()
{
	bind();
	// MAME order: resolve objects, then start and reset in device order
	for (device_t *d : order) d->resolve();
	for (device_t *d : order) d->start();
	for (device_t *d : order) d->reset();
	for (device_t *d : order) d->reset_after_children();
	m.irq_w(machine_t::SCNINT, 1);
	scc->rxa_w(1);
	std::sort(midi.ev.begin(), midi.ev.end(), [](auto const &x, auto const &y) { return x.first < y.first; });
	if (cfg.midi_clock || !midi.ev.empty()) midi_timer->adjust(attotime::from_hz(31250), 0, attotime::from_hz(31250));
	powered = true;
}

void runtime::impl::midi_tick()
{
	double const now_s = double(sched.time().m_seconds);
	while (midi.next < midi.ev.size() && midi.ev[midi.next].first <= now_s + 1e-9) { for (u8 b2 : midi.ev[midi.next].second) midi.q.push_back(b2); ++midi.next; }
	if (midi.bit < 0 && midi.q_head < midi.q.size())
	{
		midi.cur = midi.q[midi.q_head++];
		if (midi.q_head == midi.q.size()) { midi.q.clear(); midi.q_head = 0; }
		midi.bit = 0; scc->rxa_w(0); return;                    // start bit
	}
	if (midi.bit >= 0 && midi.bit < 8) { scc->rxa_w(BIT(midi.cur, midi.bit)); ++midi.bit; return; }
	if (midi.bit == 8) { scc->rxa_w(1); midi.bit = -1; return; }   // stop bit
}

runtime::runtime() : m(std::make_unique<impl>()) { }
runtime::~runtime()
{
	// devices and timers unregister from the instance's scheduler: bind it while they are destroyed
	shim_scheduler *const prev = g_shim_sched;
	g_shim_sched = &m->sched;
	m.reset();
	g_shim_sched = prev;
}

bool runtime::open(runtime_config const &cfg, std::string &error)
{
	if (!m->open(cfg, error)) return false;
	m->power_on();
	return true;
}
bool runtime::open_unpowered(runtime_config const &cfg, std::string &error) { return m->open(cfg, error); }
void runtime::power_on() { m->power_on(); }
void runtime::bind_thread() { m->bind(); }

void runtime::scanner_push(uint8_t byte) { m->m.scanner_push(byte); }
void runtime::panel_switch(uint8_t code, bool down) { m->m.scanner_push(down ? 0xc0 : 0xc3); m->m.scanner_push(code); }
void runtime::extra_switch(uint8_t mask, bool down) { if (down) m->m.extra_switches |= mask; else m->m.extra_switches &= u8(~mask); }
void runtime::set_adc_input(unsigned input, uint16_t value) { if (input < 8) m->m.adc_pot[input] = u16(value & 0x7fff); }
uint16_t runtime::adc_input(unsigned input) const { return input < 8 ? m->m.adc_pot[input] : 0; }
void runtime::midi_in(uint8_t const *bytes, size_t n) { m->midi.q.insert(m->midi.q.end(), bytes, bytes + n); }
void runtime::schedule_midi(double t, std::vector<uint8_t> bytes) { m->midi.ev.emplace_back(t, std::move(bytes)); }

uint64_t runtime::cycles() const { return m->cpu ? uint64_t(m->cpu->total_cycles()) : 0; }
double runtime::seconds() const { return double(cycles()) / cpu_hz; }
int runtime::stop_code() const { return m->stop; }
void runtime::request_stop(int code) { if (code > m->stop) m->stop = code; }

void runtime::run_until_cycle(uint64_t c_target)
{
	impl &s = *m;
	auto &cpu = *s.cpu;
	attotime const chunk_end = attotime(attotime::T(c_target) / attotime::T(CPU_HZ));
	while (u64(cpu.total_cycles()) < c_target && !s.stop)
	{
		// next event: earliest MAME-style timer, the PIT's next output change, or the caller's boundary
		attotime target = std::min(s.sched.next_expiry(), chunk_end);
		for (int i = 0; i < 3; ++i)
		{
			double const e = s.m.pit_next_event(i);
			if (e < 1e29) target = std::min(target, attotime(attotime::T(e) / attotime::T(CPU_HZ)));
		}
		attotime const now = cpu.local_time();
		attotime upto = target;
		if (target > now)
		{
			// MAME: run the CPU for floor(delta / cycle) cycles; it stops at the first instruction boundary past that
			long double const dc = (target - now).m_seconds * attotime::T(CPU_HZ);
			int const budget = int(std::min<long double>(std::floor(dc + 1e-9L), 1e9L));
			if (budget >= 1)
			{
				cpu.run(budget);
				// a slice aborted by a newly armed timer ends at the CPU's current time
				if (cpu.local_time() < target) upto = cpu.local_time();
			}
		}
		s.sched.fire_due(upto);
		for (int i = 0; i < 3; ++i) s.m.pit_update(i, double(cpu.total_cycles()));
		// watchdog: emulated time must advance; a scheduler/PIT event stuck in the past would spin forever
		if (cpu.local_time() == s.wd_last) { if (++s.wd_spins == 1000000) {
			fprintf(stderr, "WATCHDOG: emulated time stuck at %.9f s (cycle %llu); next timer %.9f; target was %.9f\n", double(cpu.local_time().m_seconds),
				(unsigned long long)cpu.total_cycles(), double(s.sched.next_expiry().m_seconds), double(target.m_seconds));
			for (int i = 0; i < 3; ++i) { auto const &c = s.m.pit[i]; fprintf(stderr, "  PIT%d mode %u loaded %d gate %d out %d count %u reload %u bcd %d load_delay %u rem %llu last_t %.3f next %.3f\n",
				i, c.mode, c.loaded, c.gate, c.out, c.count, c.reload, c.bcd, c.load_delay, (unsigned long long)c.rem, c.last_t, s.m.pit_next_event(i)); }
			s.stop = 2; } }
		else { s.wd_last = cpu.local_time(); s.wd_spins = 0; }
	}
}

void runtime::render_audio() { m->backend->render_until(m->m.t20()); }
std::vector<float> &runtime::audio() { return m->backend->audio(); }

void runtime::snapshot(panel_snapshot &o) const
{
	machine_t const &x = m->m;
	memcpy(o.lcd.ddram, x.lcd_ddram, sizeof o.lcd.ddram);
	memcpy(o.lcd.cgram, x.lcd_cgram, sizeof o.lcd.cgram);
	o.lcd.ac = u8(x.lcd_ac);
	o.lcd.cgram_active = x.lcd_cgram_active;
	o.lcd.display_on = x.lcd_display_on; o.lcd.cursor_on = x.lcd_cursor_on; o.lcd.blink_on = x.lcd_blink_on;
	o.lcd.disp_shift = u8(x.lcd_disp_shift); o.lcd.num_lines = u8(x.lcd_num_line);
	o.led_latch = x.leds; o.misc_latch = x.misc;
	o.emu_seconds = seconds();
	o.running = m->powered && !m->stop;
	++o.serial;
}

} // namespace eiii
