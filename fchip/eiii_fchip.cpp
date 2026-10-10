#include "eiii_fchip.h"

namespace eiii {

namespace {
constexpr uint32_t DATA = 0x003fffffU; // RFD00-21
constexpr uint32_t FORBK = 1U << 22, DEC = 1U << 23, LOOP = 1U << 24;
inline uint32_t step(uint32_t w) { uint32_t const d = w & DATA; return (w & ~DATA) | (((w & DEC) ? d - 1 : d + 1) & DATA); }
}

void fchip_bank::rf_write(unsigned voice, unsigned slot, uint32_t value)
{
	voice_state &v = m_v[voice];
	v.rf[slot & 3] = value & 0x01ffffffU;
	if (slot == 0)
		v.active = true;  // a current-size write gives the channel a new remaining count
	else if (slot == 2)
		v.phase = 0;      // new current address restarts the divide-by-N count
}

void fchip_bank::ctrl_write(unsigned voice, uint8_t data)
{
	voice_state &v = m_v[voice];
	if (!v.ctrl_phase)
	{
		v.ctrl_hi = data;
		v.ctrl_phase = 1;
		return;
	}
	v.ctrl_phase = 0;
	v.rate = uint16_t(((v.ctrl_hi & 0x0f) << 8) | data);
}

int16_t fchip_bank::pcm(unsigned voice) const
{
	voice_state const &v = m_v[voice];
	if (!v.active || !v.rate)
		return 0;
	size_t const a = v.rf[2] & DATA;
	return a < m_words ? int16_t(m_ram[a]) : 0;
}

void fchip_bank::advance(uint64_t clocks)
{
	if (!clocks)
		return;
	for (voice_state &v : m_v)
	{
		if (!v.active || !v.rate)
			continue;
		uint64_t const acc = uint64_t(v.phase) + clocks;
		uint64_t steps = acc / v.rate;
		v.phase = uint32_t(acc % v.rate);

		uint32_t &size = v.rf[0];
		uint32_t &addr = v.rf[2];
		while (steps--)
		{
			uint32_t const d = size & DATA;
			bool const end = (size & DEC) ? d == 0 : d == DATA;
			if (end)
			{
				if (size & FORBK)
				{
					// forward-backward turnaround (STRONG): one more step, flip direction, reload size
					addr = step(addr) ^ DEC;
					size = v.rf[1];
					continue;
				}
				if (size & LOOP)
				{
					size = v.rf[1];
					addr = v.rf[3];
					continue;
				}
				v.active = false; // one-shot end (terminal output TENTATIVE: silence)
				break;
			}
			size = step(size);
			addr = step(addr);
		}
	}
}

} // namespace eiii
