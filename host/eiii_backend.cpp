#include "eiii_backend.h"

namespace eiii {

software_backend::software_backend(uint16_t const *sound_ram, size_t words, double output_rate, unsigned oversample)
	: m_fc(sound_ram, words), m_rate(output_rate), m_fs(output_rate * oversample), m_os(oversample)
{
	m_cem.prepare(m_fs);
}

void software_backend::advance_to(uint64_t t20)
{
	if (t20 > m_now) { m_fc.advance(t20 - m_now); m_now = t20; }
}

void software_backend::use_model_v2(cem_calibration const &cal, unsigned oversample, cem_v2_options const &o)
{
	m_v2 = true;
	m_os = oversample;
	m_fs = m_rate * oversample;
	m_cem2 = std::make_unique<cem3387_v2_bank>();
	m_cem2->prepare(cal, m_fs, o);
	m_dec.design(m_fs, oversample);
	// replay the CV state the firmware may already have written
	for (unsigned v = 0; v < 16; ++v)
		for (unsigned b = 0; b < 5; ++b) m_cem2->voice(v).set_word(b, m_cv[v][b]);
	m_cem2->reset();
}

void software_backend::render_until(uint64_t t20)
{
	double const step = 20e6 / m_fs;
	if (m_v2)
	{
		while (m_next < double(t20))
		{
			advance_to(uint64_t(m_next));
			int16_t pcm[16];
			for (unsigned v = 0; v < 16; ++v) pcm[v] = m_fc.pcm(v);
			double l, r, ol, orr;
			m_cem2->process(pcm, l, r);
			if (m_dec.push(l, r, ol, orr)) { m_out.push_back(float(ol / 10.0)); m_out.push_back(float(orr / 10.0)); }
			m_next += step;
		}
		advance_to(t20);
		return;
	}
	while (m_next < double(t20))
	{
		advance_to(uint64_t(m_next));
		int16_t pcm[16];
		for (unsigned v = 0; v < 16; ++v) pcm[v] = m_fc.pcm(v);   // held lane value (zero-order hold)
		double l, r;
		m_cem.process(pcm, l, r);
		m_accl += l; m_accr += r;
		if (++m_k == m_os)
		{
			m_out.push_back(float(m_accl / m_os / 10.0));
			m_out.push_back(float(m_accr / m_os / 10.0));
			m_accl = m_accr = 0; m_k = 0;
		}
		m_next += step;
	}
	advance_to(t20);
}

void software_backend::rf_write(uint64_t t20, unsigned voice, unsigned slot, uint32_t value) { render_until(t20); m_fc.rf_write(voice, slot, value); }
uint32_t software_backend::rf_read(uint64_t t20, unsigned voice, unsigned slot) { render_until(t20); return m_fc.rf_read(voice, slot); }
void software_backend::ctrl_write(uint64_t t20, unsigned voice, uint8_t byte) { render_until(t20); m_fc.ctrl_write(voice, byte); }
void software_backend::cv_write(uint64_t t20, unsigned bank, unsigned channel, uint16_t word)
{
	render_until(t20);
	if (bank < 5 && channel < 16)
	{
		if (m_v2) m_cem2->cv_write(bank, channel, word); else m_cem.cv_write(bank, channel, word);
		m_cv[15 - channel][bank] = word;
	}
}

} // namespace eiii
