// The fork boundary: everything the authentic core emits towards the output hardware.
// Events carry core time in 20 MHz ticks (F-Chip clock; = 2 x 10 MHz CPU cycles) and must arrive in time order.
// software_backend = F-Chip lanes + CEM3387 model; a Teensy bridge implements the same interface over USB.
#pragma once
#include "../fchip/eiii_fchip.h"
#include "../cem/eiii_cem3387.h"
#include "../cem/eiii_cem3387_v2.h"
#include "eiii_decimator.h"
#include <memory>
#include <cstdint>
#include <vector>

namespace eiii {

class output_backend
{
public:
	virtual ~output_backend() = default;
	virtual void rf_write(uint64_t t20, unsigned voice, unsigned slot, uint32_t value) = 0;   // 0x3E8000 + 16v + 4slot
	virtual uint32_t rf_read(uint64_t t20, unsigned voice, unsigned slot) = 0;               // live read-back
	virtual void ctrl_write(uint64_t t20, unsigned voice, uint8_t byte) = 0;                 // 0xD30010 + 2v
	virtual void cv_write(uint64_t t20, unsigned bank, unsigned channel, uint16_t word) = 0; // 0xDE0000 + 2(16b + c)
	virtual void render_until(uint64_t t20) = 0;                                            // produce audio up to t20
};

class software_backend : public output_backend
{
public:
	software_backend(uint16_t const *sound_ram, size_t words, double output_rate, unsigned oversample = 2);
	void start_at(uint64_t t20) { m_now = t20; m_next = double(t20); }
	// CEM3387_MODEL_V2 (evidence-refined section model + external network, FIR decimation); V1 stays the default.
	// Must be called before rendering starts.
	void use_model_v2(cem_calibration const &cal, unsigned oversample = 4, cem_v2_options const &o = {});
	char const *model_name() const { return m_v2 ? "CEM3387_MODEL_V2" : "CEM3387_MODEL_V1"; }
	fchip_bank &lanes() { return m_fc; }

	void rf_write(uint64_t t20, unsigned voice, unsigned slot, uint32_t value) override;
	uint32_t rf_read(uint64_t t20, unsigned voice, unsigned slot) override;
	void ctrl_write(uint64_t t20, unsigned voice, uint8_t byte) override;
	void cv_write(uint64_t t20, unsigned bank, unsigned channel, uint16_t word) override;
	void render_until(uint64_t t20) override;

	std::vector<float> &audio() { return m_out; }    // interleaved L/R, volts at the jacks / 10 V
	uint16_t cv_word(unsigned voice, unsigned bank) const { return m_cv[voice][bank]; }
	double cv_pin(unsigned voice, unsigned bank) { return m_v2 ? m_cem2->voice(voice).cv().pin(bank) : m_cem.voice(voice).cv_volts(bank); }
	uint64_t now() const { return m_now; }

private:
	void advance_to(uint64_t t20);
	fchip_bank m_fc;
	cem3387_bank m_cem;
	std::unique_ptr<cem3387_v2_bank> m_cem2;
	fir_decimator m_dec;
	bool m_v2 = false;
	double m_rate, m_fs;
	unsigned m_os;
	uint64_t m_now = 0;
	double m_next = 0;
	double m_accl = 0, m_accr = 0;
	unsigned m_k = 0;
	std::vector<float> m_out;
	uint16_t m_cv[16][5] = {};
};

} // namespace eiii
