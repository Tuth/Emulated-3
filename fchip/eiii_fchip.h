// EIII digital voice lane engine (F-Chip rate registers + microcontroller register file).
// Portable C++17, no MAME dependencies.  Semantics: analysis/DIGITAL_VOICE_HARDWARE.md.
// Reference implementation: MAME emu3.cpp (digvoice_w / digvoice_r / digctrl_w / lanes_advance);
// equivalence is checked bit-exactly by tests/fchip_equiv.cpp against MAME traces.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace eiii {

class fchip_bank
{
public:
	static constexpr unsigned VOICES = 16;
	static constexpr uint32_t CLOCK_HZ = 20'000'000; // C20M (SK105 sheet 4)

	struct voice_state
	{
		std::array<uint32_t, 4> rf{}; // +0 current size, +4 initial size, +8 current address, +C initial address (25 bit)
		uint32_t phase = 0;           // divide-by-N count
		uint16_t rate = 0;            // 12-bit divisor N (sample rate = CLOCK_HZ / N)
		bool active = false;          // false once a non-looping segment has ended
		uint8_t ctrl_phase = 0;       // F-Chip port byte-pair position
		uint8_t ctrl_hi = 0;          // pending high byte
	};

	// sound_ram: 16-bit words of sample RAM (CPU 0x400000..), word index = register address bits 0-21
	fchip_bank(uint16_t const *sound_ram, size_t words) : m_ram(sound_ram), m_words(words) {}

	void reset() { m_v = {}; }

	// Clock every voice by 'clocks' cycles of the 20 MHz F-Chip clock.
	void advance(uint64_t clocks);

	// CPU write of a complete register-file word (MS word committed); slot 0..3 = +0/+4/+8/+C.
	void rf_write(unsigned voice, unsigned slot, uint32_t value);
	// CPU read of a register-file word (live).
	uint32_t rf_read(unsigned voice, unsigned slot) const { return m_v[voice].rf[slot]; }
	// CPU byte write to the F-Chip control port 0xD30010 + 2*voice.
	void ctrl_write(unsigned voice, uint8_t data);

	// Current raw output of a lane (sample at the current address, 0 when ended / no rate).
	int16_t pcm(unsigned voice) const;

	voice_state const &state(unsigned voice) const { return m_v[voice]; }
	voice_state &state(unsigned voice) { return m_v[voice]; } // for restoring a snapshot

private:
	uint16_t const *m_ram;
	size_t m_words;
	std::array<voice_state, VOICES> m_v{};
};

} // namespace eiii
