// Shim: CD audio is not used by the EIII (data CD only); this stub reports "no audio playing".
#pragma once
#include "emu.h"
class cdda_device : public device_t
{
public:
	cdda_device(machine_config const &mc, char const *tag, device_t *owner, u32 clock = 0) : device_t(mc, s_type, tag, owner, clock) { }
	template <typename T> cdda_device &set_cdrom_tag(T &&) { return *this; }
	void start_audio(u32, u32) { }
	void stop_audio() { }
	void cancel_scan() { }
	void set_audio_lba(u32) { }
	void scan_forward() { }
	void scan_reverse() { }
	void pause_audio(int) { }
	int audio_active() const { return 0; }
	int audio_paused() const { return 0; }
	int audio_ended() const { return 0; }
	u32 get_audio_lba() const { return 0; }
	void set_output_gain(int, float) { }
	float output_gain(int) const { return 1.0f; }
	template <typename... A> cdda_device &add_route(A &&...) { return *this; }
	static device_type_impl const s_type;
};
template <typename... A> cdda_device &CDDA(machine_config &, A &&...) { throw std::logic_error("CDDA() config not used"); }
