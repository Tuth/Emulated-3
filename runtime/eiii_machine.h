// EIII motherboard model (internal to the runtime): memory map, interrupt latch, 8254 PIT, HD44780, LED latches,
// panel scanner FIFO + extra switches, ADC board pots, output hardware -> eiii-core software backend.
// Moved verbatim from standalone/eiii_host.cpp (behaviour unchanged; process globals became members).
// Includes the MAME shim: never include this from UI/plugin code — use eiii_runtime.h.
#pragma once
#include "emu.h"
#include "cpu/ns32000/ns32000.h"
#include "machine/ns32081.h"
#include "machine/ncr5380.h"
#include "machine/z80scc.h"
#include "../host/eiii_backend.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace eiii {

constexpr double CPU_HZ = 10'000'000.0;

// ---- minimal 8254 (modes 0, 2, 3, 5; binary/BCD; LSB/MSB) — counts on the channel's input clock ----
struct pit_channel
{
	double clock_hz = 0;      // input clock
	int mode = 0, rw = 3, bcd = 0;
	unsigned reload = 0, count = 0;
	bool loaded = false, msb_next = false, gate = true, out = true, latched = false;
	bool out_known = false;
	unsigned load_delay = 0;  // MAME pit8253 phase 1: the count is transferred to the counter on the clock after the write   // MAME pit8253: output undefined after reset, so the first level always fires the callback
	unsigned latch = 0; bool latch_msb = false;
	double last_t = 0;        // CPU time of last update (cycles)
	uint64_t rem = 0;         // exact fractional clock: accumulated (cycles x clock_hz) modulo CPU_HZ (no float drift)
	std::function<void(bool)> out_cb;
};

struct machine_t : public address_space
{
	std::vector<u16> rom = std::vector<u16>(0x4000), ram = std::vector<u16>(0x10000), snd = std::vector<u16>(0x400000);
	ns32016_device *cpu = nullptr;
	ns32081_device *fpu = nullptr;
	ncr5380_device *hdc = nullptr;
	z80scc_device *scc = nullptr;
	// output hardware -> eiii-core software backend (F-Chip lanes + CEM3387 model); time base = 20 MHz lane clock
	eiii::software_backend *out = nullptr;
	u16 dig_words[0x80] = {};
	u32 rf_latch = 0;
	u16 cv_words[80] = {};
	u32 w_start[16] = {}, w_loop[16] = {}, w_size[16] = {}; bool w_pending[16] = {};
	u64 n_rf = 0, n_ctrl = 0, n_cv = 0;   // output-hardware activity counters
	u64 t20() const { return u64(cpu->total_cycles()) * 2; }
	u8 irq_latch = 0x4b; bool irq_state = false;
	pit_channel pit[3];
	// HD44780 (2 x 40 used by the EIII): DDRAM, address counter, busy flag
	// HD44780 (8-bit interface), semantics of MAME's hd44780_base_device: DDRAM + CGRAM, address counter on either,
	// entry mode, display/cursor/blink flags, display shift, busy timing at 270 kHz.
	u8 lcd_ddram[0x80]; u8 lcd_cgram[0x40] = {}; int lcd_ac = 0; bool lcd_cgram_active = false; int lcd_dir = 1; bool lcd_shift_on = false;
	bool lcd_display_on = false, lcd_cursor_on = false, lcd_blink_on = false; int lcd_disp_shift = 0, lcd_num_line = 1;
	double lcd_busy_until = 0;
	u16 leds = 0; u8 misc = 0;
	// WD1772 (drive present, no disk): only the behaviour MAME shows on the EIII boot path is modelled —
	// status 0x04 (TR00) before any command, 0xA4 (MON | SPIN | TR00) after a force-interrupt command, never INTRQ.
	// Any other FDC access is logged as unmodelled.
	u8 fdc_status = 0x04;
	// front-panel scanner, ported from emu3.cpp (scanner_r / scanner_push / scanner_irq): 16-byte FIFO, SCNINT asserted
	// when the first byte enters an empty FIFO; every CSSCAN read pops one byte, releases SCNINT and, if bytes remain,
	// re-asserts it 50 us later.  Extra switches: polled byte at 0xFA0000.
	u8 scan_fifo[16] = {}; unsigned scan_head = 0, scan_tail = 0, scan_count = 0;
	emu_timer *scan_timer = nullptr;
	u8 extra_switches = 0;
	offs_t watch_lo = 0, watch_hi = 0, last_pc = 0;  // --watch LO:HI debug aid (RAM writes)
	void scanner_push(u8 d)
	{
		if (scan_count == 16) return;
		bool const empty = !scan_count;
		scan_fifo[scan_tail] = d; scan_tail = (scan_tail + 1) % 16; ++scan_count;
		if (log) fprintf(log, "SCANPUSH cyc=%.0f byte=%02X fifo=%u\n", now(), d, scan_count);
		if (empty) irq_w(SCNINT, 0);
	}
	u8 scanner_r()
	{
		u8 d = 0;
		if (scan_count) { d = scan_fifo[scan_head]; scan_head = (scan_head + 1) % 16; --scan_count; }
		irq_w(SCNINT, 1);
		if (scan_count && scan_timer) scan_timer->adjust(attotime::from_usec(50));
		return d;
	}
	// ADC board, ported from emu3.cpp adc_ctrl_w/adc_r: control latch 0x360000; 0x368000 left / 0x368002 right word;
	// a left-word read stalls the CPU to the next conversion boundary (10 MHz / 227 with KHZ44, else / 300);
	// pots in firmware ADC units with the MAME driver's default port values.
	u8 adc_ctrl = 0;
	s16 adc_latch[2] = {0, 0};
	u16 adc_pot[8] = {0x3800, 0x0000, 0x31f1, 0x0001, 0x73cc, 0x708d, 0x2909, 0x0000};
	unsigned long long adc_pot_reads[8] = {};
	// host audio input: (conversion time in emulated seconds) -> L/R ADC words; unset = silence
	std::function<void(double, s16 *)> adc_source;   // right-word reads per multiplexer input with -POTSON active (statistics)
	FILE *log = nullptr;
	unsigned long long unmapped = 0, ncr_logged = 0;
	bool log_voice_starts = false;   // "START ..." line on stderr when a voice starts (regression scripts parse it)
	bool log_leds = false;           // LED latch changes on stderr (decoding aid)
	bool log_midi_chain = false;     // --verbose-midi: firmware reads of the SCC data register
	bool pit1_last = true;           // previous PIT1 output (STINT is latched on its falling edge)

	machine_t() { memset(lcd_ddram, 0x20, sizeof lcd_ddram); }
	double now() const { return double(cpu->total_cycles()); }

	enum { SCNINT = 0, SCCINT, FDCINT, STINT, TGINT, HDINT };
	void irq_w(int irq, int state)
	{
		if (state) irq_latch |= 1u << irq; else irq_latch &= ~(1u << irq);
		bool const s = (irq_latch & 0x34) || (~irq_latch & 0x0b);
		if (s != irq_state) { irq_state = s; if (log) fprintf(log, "IRQ %d cyc=%.0f latch=%02X\n", int(s), now(), irq_latch); cpu->set_input_line(INPUT_LINE_IRQ0, s ? ASSERT_LINE : CLEAR_LINE); }
	}

	// ---- PIT ----
	unsigned from_bcd(unsigned v) const { return (v & 0xf) + ((v >> 4) & 0xf) * 10 + ((v >> 8) & 0xf) * 100 + ((v >> 12) & 0xf) * 1000; }
	void pit_set_out(int ch, bool o) { pit_channel &c = pit[ch]; if (c.out != o || !c.out_known) { c.out = o; c.out_known = true; if (c.out_cb) c.out_cb(o); } }
	// next CPU cycle at which the channel's output changes (for scheduling); huge if none
	double pit_next_event(int ch) const
	{
		pit_channel const &c = pit[ch];
		if (!c.loaded || !c.gate || c.clock_hz == 0) return 1e30;
		unsigned need;
		switch (c.mode)
		{
		case 2: need = c.out ? (c.count > 1 ? c.count - 1 : c.reload) : 1; break;
		case 3: need = c.count / 2 ? c.count / 2 : 1; break;
		case 0: need = c.out ? 1u << 30 : c.count; break;
		default: return 1e30;
		}
		// cycles until (need + load_delay) more clocks: ceil(((need + load_delay) * CPU_HZ - rem) / clock_hz), exact in integers
		uint64_t const hz = uint64_t(c.clock_hz), cpu_hz = uint64_t(CPU_HZ);
		uint64_t const num = (uint64_t(need) + c.load_delay) * cpu_hz - c.rem;
		return c.last_t + double((num + hz - 1) / hz);
	}
	void pit_update(int ch, double t)
	{
		pit_channel &c = pit[ch];
		t = std::floor(t);
		if (t <= c.last_t) return;
		uint64_t const dt = uint64_t(t - c.last_t);
		c.last_t = t;
		if (!c.loaded || !c.gate || c.clock_hz == 0) { c.rem = 0; return; }
		uint64_t const cpu_hz = uint64_t(CPU_HZ), num = dt * uint64_t(c.clock_hz) + c.rem;
		auto whole = (unsigned long long)(num / cpu_hz); c.rem = num % cpu_hz;
		unsigned const period = c.reload ? c.reload : (c.bcd ? 10000 : 65536);
		if (c.load_delay && whole) { --whole; c.load_delay = 0; c.count = period; }
		while (whole)
		{
			switch (c.mode)
			{
			case 2: // rate generator: OUT low for one clock when count reaches 1
				if (!c.out) { pit_set_out(ch, true); c.count = period; --whole; break; }
				if (c.count > 2) { unsigned long long const s = std::min<unsigned long long>(whole, c.count - 2); c.count -= unsigned(s); whole -= s; if (!whole) break; }
				--c.count; --whole; if (c.count == 1) pit_set_out(ch, false);
				break;
			case 3: // square wave
			{
				unsigned long long const s = std::min<unsigned long long>(whole, (c.count + 1) / 2);
				unsigned dec = unsigned(s * 2); c.count = c.count > dec ? c.count - dec : 0; whole -= s;
				if (!c.count) { pit_set_out(ch, !c.out); c.count = period; }
				break;
			}
			case 0: // interrupt on terminal count
			{
				unsigned long long const s = std::min<unsigned long long>(whole, c.count); c.count -= unsigned(s); whole -= s;
				if (!c.count) { pit_set_out(ch, true); c.count = period; }
				break;
			}
			default: whole = 0; break;      // modes 1/5 need a gate edge; gates are tied high on the EIII
			}
		}
	}
	void pit_w(unsigned reg, u8 d)
	{
		// as MAME's pit8253 re-arms its update timer on every write, the scheduler must re-target: abort the slice
		shim_sched().abort_timeslice();
		double const t = now();
		for (int i = 0; i < 3; ++i) pit_update(i, t);
		if (reg == 3)
		{
			int const ch = d >> 6; if (ch == 3) return;
			pit_channel &c = pit[ch];
			if (((d >> 4) & 3) == 0) { c.latch = c.count; c.latched = true; c.latch_msb = false; return; }
			c.rw = (d >> 4) & 3; c.mode = (d >> 1) & 7; if (c.mode > 5) c.mode -= 4; c.bcd = d & 1;
			c.loaded = false; c.msb_next = false; c.rem = 0;
			pit_set_out(ch, c.mode != 0);
			return;
		}
		pit_channel &c = pit[reg];
		unsigned v = c.reload;
		if (c.rw == 1) v = d; else if (c.rw == 2) v = unsigned(d) << 8;
		else { if (!c.msb_next) { v = (v & 0xff00) | d; c.msb_next = true; c.reload = v; return; } v = (v & 0x00ff) | (unsigned(d) << 8); c.msb_next = false; }
		unsigned nc = v;
		if (nc == 1 && c.mode == 2) nc = 2;          // MAME pit8253 load_count: count 1 illegal in modes 2/3
		if (nc == 1 && c.mode == 3) nc = 0;
		c.reload = c.bcd ? from_bcd(nc) : nc; if (!c.reload) c.reload = c.bcd ? 10000 : 65536;
		// MAME: in modes 2/3 a rewrite while counting only stores the count; it takes effect at the next reload
		if (c.loaded && (c.mode == 2 || c.mode == 3)) return;
		c.count = c.reload; c.loaded = true; c.rem = 0; c.last_t = std::floor(t); c.load_delay = 1;
		if (c.mode == 0) pit_set_out(int(reg), false);
	}
	u8 pit_r(unsigned reg)
	{
		if (reg > 2) return 0xff;
		for (int i = 0; i < 3; ++i) pit_update(i, now());
		pit_channel &c = pit[reg];
		unsigned v = c.latched ? c.latch : c.count;
		u8 r = c.latch_msb ? u8(v >> 8) : u8(v);
		if (c.rw == 3) { c.latch_msb = !c.latch_msb; if (!c.latch_msb) c.latched = false; } else c.latched = false;
		return r;
	}

	// ---- LCD ----
	// MAME timer semantics: the expiry is reached at the first CPU instruction boundary at or after floor(expiry in cycles)
	void lcd_busy(unsigned lcd_cycles) { lcd_busy_until = std::floor(now() + lcd_cycles * CPU_HZ / 270000.0); }
	void lcd_correct_ac()
	{
		if (!lcd_cgram_active)
		{
			int const max_ac = (lcd_num_line == 1) ? 0x4f : 0x67;
			if (lcd_ac > max_ac) lcd_ac -= max_ac + 1;
			else if (lcd_ac < 0) lcd_ac = max_ac;
			else if (lcd_num_line == 2 && lcd_ac > 0x27 && lcd_ac < 0x40) lcd_ac = 0x40 + (lcd_ac - 0x28);
		}
		else lcd_ac &= 0x3f;
	}
	void lcd_update_ac(int dir)
	{
		if (!lcd_cgram_active && lcd_num_line == 2 && dir == -1 && lcd_ac == 0x40) lcd_ac = 0x27; else lcd_ac += dir;
		lcd_correct_ac();
	}
	void lcd_shift_display(int dir) { lcd_disp_shift += dir; if (lcd_disp_shift == 0x50) lcd_disp_shift = 0; else if (lcd_disp_shift == -1) lcd_disp_shift = 0x4f; }
	void lcd_control_w(u8 d)
	{
		if (d & 0x80) { lcd_cgram_active = false; lcd_ac = d & 0x7f; lcd_correct_ac(); lcd_busy(10); }
		else if (d & 0x40) { lcd_cgram_active = true; lcd_ac = d & 0x3f; lcd_busy(10); }
		else if (d & 0x20) { lcd_num_line = (d & 0x08) ? 2 : 1; lcd_correct_ac(); lcd_busy(10); }
		else if (d & 0x10) { int const dir = (d & 0x04) ? 1 : -1; if (d & 0x08) lcd_shift_display(dir); else lcd_update_ac(dir); lcd_busy(10); }
		else if (d & 0x08) { lcd_display_on = d & 4; lcd_cursor_on = d & 2; lcd_blink_on = d & 1; lcd_busy(10); }
		else if (d & 0x04) { lcd_dir = (d & 2) ? 1 : -1; lcd_shift_on = d & 1; lcd_busy(10); }
		else if (d & 0x02) { lcd_ac = 0; lcd_cgram_active = false; lcd_dir = 1; lcd_disp_shift = 0; lcd_busy(410); }
		else if (d & 0x01) { lcd_ac = 0; lcd_cgram_active = false; lcd_dir = 1; lcd_disp_shift = 0; memset(lcd_ddram, 0x20, sizeof lcd_ddram); lcd_busy(410); }
	}
	u8 lcd_control_r() { return (now() < lcd_busy_until ? 0x80 : 0) | (lcd_ac & 0x7f); }
	u8 lcd_data_r() { u8 const v = lcd_cgram_active ? lcd_cgram[lcd_ac & 0x3f] : lcd_ddram[lcd_ac & 0x7f]; lcd_busy(10); lcd_update_ac(lcd_dir); return v; }
	void lcd_data_w(u8 d) { if (lcd_cgram_active) lcd_cgram[lcd_ac & 0x3f] = d; else lcd_ddram[lcd_ac & 0x7f] = d; lcd_busy(10); lcd_update_ac(lcd_dir); if (lcd_shift_on) lcd_shift_display(lcd_dir); }
	std::string lcd_text(int row) const { std::string s; for (int i = 0; i < 40; ++i) { u8 c = lcd_ddram[row * 0x40 + i]; s += (c >= 32 && c < 127) ? char(c) : '.'; } return s; }

	// ---- bus ----
	u16 read_word(offs_t a, u16 mask) override
	{
		a &= 0xffffff;
		if (a < 0x8000) return rom[(a >> 1) & 0x3fff];
		if (a >= 0x8000 && a < 0x28000) return ram[(a - 0x8000) >> 1];
		if (a >= 0x400000 && a < 0xc00000) return snd[(a - 0x400000) >> 1];
		u8 const lane = (mask & 0x00ff) ? u8(a & ~1) : u8(a | 1); (void)lane;
		if (a >= 0x300000 && a < 0x300010)
		{
			u8 const r = (mask & 0x00ff) ? hdc->read((a - 0x300000) >> 1) : 0;
			if (log && ncr_logged++ < 12288) fprintf(log, "NCR %06X cyc=%.0f pc=%06X d=%02X\n", a, now(), unsigned(cpu->state_pc()), r);
			return r;
		}
		if (a >= 0x2c0000 && a < 0x2c0202) { u16 r = 0; if (mask & 0x00ff) r |= hdc->dma_r(); if (mask & 0xff00) r |= u16(hdc->dma_r()) << 8; if (log && getenv("EIII_LOG_DMA")) fprintf(log, "DMA %06X cyc=%.0f pc=%06X d=%04X m=%04X\n", a, now(), unsigned(cpu->state_pc()), r, mask); return r; }
		if (a >= 0x368000 && a < 0x368004)
		{
			unsigned const off = (a - 0x368000) >> 1;
			if (!(off & 1))
			{
				u64 const period = BIT(adc_ctrl, 4) ? 227 : 300;
				u64 const c = cpu->total_cycles();
				u64 const boundary = (c / period + 1) * period;
				cpu->adjust_icount(-int(boundary - c));
				// the analog input at the end of this conversion (after the input VCA and filters), from the host.
				// Only in the two configurations the Sample overlay uses (0x08 = 33 333 Hz, 0x38 = 44 053 Hz, -POTSON
				// off): the OS's pot scan also reads the right word at 0x28 and takes it as a control value, so live
				// audio there would act like a moving slider.  Which signal the hardware presents in that slot is not
				// known; outside sampling the converter reads silence, as in the proven MAME model without a test input.
				if (adc_source && (adc_ctrl == 0x08 || adc_ctrl == 0x38)) adc_source(double(boundary) / 10'000'000.0, adc_latch);
				else adc_latch[0] = adc_latch[1] = 0;
			}
			if (off & 1)
			{
				if (!BIT(adc_ctrl, 3)) { ++adc_pot_reads[adc_ctrl & 7]; return u16(-s32(adc_pot[adc_ctrl & 7])); }
				return u16(adc_latch[1]);
			}
			return u16(adc_latch[0]);
		}
		if (a >= 0x290000 && a < 0x290008)
		{
			if (!(mask & 0x00ff)) return 0;
			u8 const d = scc->ab_dc_r((a - 0x290000) >> 1);
			if (log_midi_chain && ((a - 0x290000) >> 1) == 3) fprintf(stderr, "MIDI-CHAIN %.6f SCC chA data read by firmware: %02X\n", now() / CPU_HZ, d);
			return d;
		}
		if (a >= 0x3e8000 && a < 0x3e8100)
		{
			// register-file read through the RF UP latch (as emu3.cpp digvoice_r): LS word latches, MS word from latch
			unsigned const bo = a - 0x3e8000, voice = bo >> 4, slot = (bo & 0x0f) >> 2, half = (bo >> 1) & 1;
			if (!half) { rf_latch = out ? out->rf_read(t20(), voice, slot) : 0; return u16(rf_latch); }
			return u16(rf_latch >> 16);
		}
		if ((a & ~1) == 0x250000) return 0;                 // -CSSCIAK (interrupt acknowledge strobe, nopr in emu3.cpp)
		if ((a & ~1) == 0xfffe00) { if (log) fprintf(log, "VEC cyc=%.0f pc=%06X v=%02X\n", now(), unsigned(cpu->state_pc()), irq_latch); return irq_latch; }
		if (a >= 0x390000 && a < 0x390008) return pit_r((a - 0x390000) >> 1);
		if ((a & ~1) == 0xeb0002) { u8 const r = lcd_control_r(); if (log) fprintf(log, "LCD EB0002 cyc=%.0f pc=%06X d=%02X\n", now(), unsigned(cpu->state_pc()), r); return r; }
		if ((a & ~1) == 0xeb0006) return lcd_data_r();
		if (a >= 0xf50008 && a < 0xf50010)
		{
			if (log) fprintf(log, "FDC %06X cyc=%.0f pc=%06X d=%02X\n", a, now(), unsigned(cpu->state_pc()), (a == 0xf50008) ? fdc_status : 0);
			if (a == 0xf50008) return fdc_status;
			if (log) fprintf(log, "UNMODELLED FDC read %06X\n", a);
			return 0;
		}
		if ((a & ~1) == 0xef0002) return 0x02;              // 6850 status: TDRE (stub)
		if ((a & ~1) == 0xfa0000) return extra_switches;    // extra switches (emu3 'extra' port, low lane)
		if ((a & ~1) == 0xf10000) return (mask & 0x00ff) ? scanner_r() : 0;
		if (log && unmapped++ < 200000) fprintf(log, "R %06X mask %04X pc %06X cyc %.0f\n", a, mask, unsigned(cpu->state_pc()), now());
		return 0;
	}
	void write_word(offs_t a, u16 d, u16 mask) override
	{
		a &= 0xffffff;
		u8 const b = (mask & 0x00ff) ? u8(d) : u8(d >> 8);
		if (a >= 0x8000 && a < 0x28000)
		{
			u16 &w = ram[(a - 0x8000) >> 1]; u16 const old = w; w = (w & ~mask) | (d & mask);
			if (watch_lo < watch_hi && a + 1 >= watch_lo && a < watch_hi && log) fprintf(log, "WATCH cyc=%.0f pc=%06X a=%06X %04X->%04X m=%04X\n", now(), last_pc, a, old, w, mask);
			return;
		}
		if (a >= 0x400000 && a < 0xc00000) { u16 &w = snd[(a - 0x400000) >> 1]; w = (w & ~mask) | (d & mask); return; }
		if (a >= 0x390000 && a < 0x390008 && (mask & 0x00ff)) { if (log) fprintf(log, "PITW %06X cyc=%.0f d=%02X\n", a, now(), b); pit_w((a - 0x390000) >> 1, b); return; }
		if (a >= 0x300000 && a < 0x300010)
		{
			if (log && ncr_logged++ < 12288) fprintf(log, "NCR %06X cyc=%.0f pc=%06X d=%02X\n", a, now(), unsigned(cpu->state_pc()), b);
			if (mask & 0x00ff) hdc->write((a - 0x300000) >> 1, b);
			return;
		}
		if (a >= 0x2c0000 && a < 0x2c0202) { if (log && getenv("EIII_LOG_DMA")) fprintf(log, "DMA %06X cyc=%.0f pc=%06X d=%04X m=%04X\n", a, now(), unsigned(cpu->state_pc()), d & mask, mask); if (mask & 0x00ff) hdc->dma_w(u8(d)); if (mask & 0xff00) hdc->dma_w(u8(d >> 8)); return; }
		if (a >= 0x290000 && a < 0x290008) { if (mask & 0x00ff) scc->ab_dc_w((a - 0x290000) >> 1, b); return; }
		if (a >= 0x3e8000 && a < 0x3e8100)
		{
			// as emu3.cpp digvoice_w: 16-bit halves combine; the MS half commits the 25-bit word
			unsigned const idx = (a - 0x3e8000) >> 1;
			dig_words[idx] = (dig_words[idx] & ~mask) | (d & mask);
			unsigned const bo = a - 0x3e8000, voice = bo >> 4, slot = (bo & 0x0f) >> 2, half = (bo >> 1) & 1;
			if (half && out)
			{
				u32 const val = (u32(dig_words[voice * 8 + slot * 2]) | (u32(dig_words[voice * 8 + slot * 2 + 1]) << 16)) & 0x01ffffffU;
				out->rf_write(t20(), voice, slot, val); ++n_rf;
				if (slot == 2) w_start[voice] = val & 0x3fffff; else if (slot == 3) w_loop[voice] = val & 0x3fffff;
				else if (slot == 0) { w_size[voice] = val; w_pending[voice] = (val & 0x3fffff) > 1; }
			}
			return;
		}
		if (a >= 0xd30010 && a < 0xd30030)
		{
			unsigned const v = (a - 0xd30010) >> 1;
			if (out)
			{
				out->ctrl_write(t20(), v, u8(d)); ++n_ctrl;
				auto const &st = out->lanes().state(v);
				if (w_pending[v] && st.ctrl_phase == 0 && st.rate >= 0x0a4)
				{
					w_pending[v] = false;
					if (log_voice_starts)
						fprintf(stderr, "START t=%.6f v=%02u pitch=%04X start=%06X loop=%06X end=%06X\n", now() / CPU_HZ, v, st.rate, w_start[v], w_loop[v], w_start[v] + (w_size[v] & 0x3fffff) + 1);
				}
			}
			return;
		}
		if (a >= 0xde0000 && a < 0xde00a0)
		{
			unsigned const idx = (a - 0xde0000) >> 1;
			cv_words[idx] = (cv_words[idx] & ~mask) | (d & mask);
			if (out) out->cv_write(t20(), idx >> 4, idx & 15, cv_words[idx]);
			++n_cv;
			return;
		}
		if ((a & ~1) == 0xc90000) return;                   // -CSSCIAK write strobe (nopw in emu3.cpp)
		if ((a & ~1) == 0xc00000) { irq_w(STINT, 1); return; }
		if ((a & ~1) == 0xc40000) { if (log) fprintf(log, "TGACK cyc=%.0f\n", now()); irq_w(TGINT, 0); return; }
		if ((a & ~1) == 0xeb0000) { if (log) fprintf(log, "LCD EB0000 cyc=%.0f pc=%06X d=%02X\n", now(), unsigned(cpu->state_pc()), b); lcd_control_w(b); return; }
		if ((a & ~1) == 0xeb0004) { if (log) fprintf(log, "LCDD cyc=%.0f d=%02X\n", now(), b); lcd_data_w(b); return; }
		if ((a & ~1) == 0xdb0000)
		{
			u16 const nl = u16((leds & ~mask) | (d & mask));
			if (log_leds && nl != leds) fprintf(stderr, "LEDS %.6f latch %04X -> %04X (misc bit7 %d) LCD [%s] [%s]\n", now() / CPU_HZ, leds, nl, BIT(misc, 7), lcd_text(0).c_str(), lcd_text(1).c_str());
			leds = nl; return;
		}
		if ((a & ~1) == 0xd70000)
		{
			if (log_leds && BIT(b, 7) != BIT(misc, 7)) fprintf(stderr, "LEDS %.6f misc bit7 %d -> %d (latch %04X) LCD [%s] [%s]\n", now() / CPU_HZ, BIT(misc, 7), BIT(b, 7), leds, lcd_text(0).c_str(), lcd_text(1).c_str());
			misc = b; return;
		}
		if ((a & ~1) == 0x360000) { if (mask & 0x00ff) adc_ctrl = b; return; }
		if (a >= 0xf50000 && a < 0xf50008)
		{
			if (log) fprintf(log, "FDC %06X cyc=%.0f pc=%06X d=%02X\n", a, now(), unsigned(cpu->state_pc()), b);
			if (a == 0xf50000 && (b & 0xf0) == 0xd0) fdc_status = 0xa4;
			else if (log) fprintf(log, "UNMODELLED FDC write %06X=%02X\n", a, b);
			return;
		}
		if (a >= 0xef0000 && a < 0xef0008) return;          // 6850 (stub)
		if (log && unmapped++ < 200000) fprintf(log, "W %06X=%04X mask %04X pc %06X cyc %.0f\n", a, d, mask, unsigned(cpu->state_pc()), now());
	}
};


} // namespace eiii
