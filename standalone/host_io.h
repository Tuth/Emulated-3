// Host I/O for the standalone EIII: ALSA raw MIDI input and ALSA PCM output.
// Pure consumers/producers at the hardware boundary: MIDI bytes go to the emulated Z85230 RxD line, audio frames come
// from the eiii-core software backend.  Nothing here knows about EIII voices or firmware.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

int list_midi_inputs();     // prints ALSA raw MIDI input ports (hw:C,D,S); returns process exit code
int list_audio_outputs();   // prints ALSA PCM playback devices; returns process exit code

class midi_input
{
public:
	~midi_input();
	bool open(std::string const &name);          // e.g. "hw:1,0,0"; non-blocking
	size_t read(std::vector<uint8_t> &out);      // appends any bytes available now; returns count
	bool is_open() const { return m_h != nullptr; }
private:
	void *m_h = nullptr;                         // snd_rawmidi_t *
};

class audio_output
{
public:
	~audio_output();
	// 48 kHz stereo S16; period/periods 0 = defaults (256 frames x 4)
	bool open(std::string const &name, unsigned rate, unsigned period_frames, unsigned periods);
	// blocking write of interleaved stereo float (+-1.0 full scale); recovers from underruns and counts them
	void write(float const *interleaved, size_t frames);
	unsigned xruns() const { return m_xruns; }
	std::function<void(unsigned)> on_xrun;      // optional report hook
	unsigned period() const { return m_period; }
	unsigned buffer() const { return m_buffer; }
	bool is_open() const { return m_h != nullptr; }
private:
	void *m_h = nullptr;                         // snd_pcm_t *
	unsigned m_xruns = 0, m_period = 0, m_buffer = 0;
	std::vector<int16_t> m_tmp;
};
