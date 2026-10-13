#include "host_io.h"
#include <alsa/asoundlib.h>
#include <cmath>
#include <cstdio>

int list_midi_inputs()
{
	int card = -1, found = 0;
	printf("ALSA raw MIDI inputs (use with --midi-in):\n");
	while (snd_card_next(&card) == 0 && card >= 0)
	{
		char ctlname[32]; snprintf(ctlname, sizeof(ctlname), "hw:%d", card);
		snd_ctl_t *ctl;
		if (snd_ctl_open(&ctl, ctlname, 0) < 0) continue;
		int dev = -1;
		while (snd_ctl_rawmidi_next_device(ctl, &dev) == 0 && dev >= 0)
		{
			snd_rawmidi_info_t *info; snd_rawmidi_info_alloca(&info);
			snd_rawmidi_info_set_device(info, unsigned(dev));
			snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
			snd_rawmidi_info_set_subdevice(info, 0);
			if (snd_ctl_rawmidi_info(ctl, info) < 0) continue;
			unsigned const subs = snd_rawmidi_info_get_subdevices_count(info);
			for (unsigned s = 0; s < subs; ++s)
			{
				snd_rawmidi_info_set_subdevice(info, s);
				if (snd_ctl_rawmidi_info(ctl, info) < 0) continue;
				printf("  hw:%d,%d,%u  %s / %s\n", card, dev, s, snd_rawmidi_info_get_name(info), snd_rawmidi_info_get_subdevice_name(info));
				++found;
			}
		}
		snd_ctl_close(ctl);
	}
	if (!found) printf("  (none found)\n");
	return 0;
}

int list_audio_outputs()
{
	printf("ALSA PCM playback devices (use with --audio-out):\n");
	void **hints;
	if (snd_device_name_hint(-1, "pcm", &hints) < 0) { printf("  (cannot enumerate)\n"); return 1; }
	for (void **h = hints; *h; ++h)
	{
		char *name = snd_device_name_get_hint(*h, "NAME"), *desc = snd_device_name_get_hint(*h, "DESC"), *io = snd_device_name_get_hint(*h, "IOID");
		if (name && (!io || std::string(io) == "Output"))
		{
			std::string d = desc ? desc : "";
			for (char &c : d) if (c == '\n') c = ' ';
			printf("  %-40s %s\n", name, d.c_str());
		}
		free(name); free(desc); free(io);
	}
	snd_device_name_free_hint(hints);
	return 0;
}

midi_input::~midi_input() { if (m_h) snd_rawmidi_close(static_cast<snd_rawmidi_t *>(m_h)); }

bool midi_input::open(std::string const &name)
{
	snd_rawmidi_t *in = nullptr;
	int const err = snd_rawmidi_open(&in, nullptr, name.c_str(), SND_RAWMIDI_NONBLOCK);
	if (err < 0) { fprintf(stderr, "MIDI: cannot open %s: %s\n", name.c_str(), snd_strerror(err)); return false; }
	m_h = in;
	return true;
}

size_t midi_input::read(std::vector<uint8_t> &out)
{
	if (!m_h) return 0;
	uint8_t buf[256];
	size_t total = 0;
	for (;;)
	{
		ssize_t const n = snd_rawmidi_read(static_cast<snd_rawmidi_t *>(m_h), buf, sizeof(buf));
		if (n <= 0) break;
		out.insert(out.end(), buf, buf + n);
		total += size_t(n);
	}
	return total;
}

audio_output::~audio_output()
{
	if (m_h) { snd_pcm_drain(static_cast<snd_pcm_t *>(m_h)); snd_pcm_close(static_cast<snd_pcm_t *>(m_h)); }
}

bool audio_output::open(std::string const &name, unsigned rate, unsigned period_frames, unsigned periods)
{
	snd_pcm_t *pcm = nullptr;
	int err = snd_pcm_open(&pcm, name.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) { fprintf(stderr, "audio: cannot open %s: %s\n", name.c_str(), snd_strerror(err)); return false; }
	snd_pcm_hw_params_t *hw; snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	snd_pcm_uframes_t period = period_frames ? period_frames : 256, buffer = 0;
	unsigned r = rate, nper = periods ? periods : 4;
	if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &r, nullptr)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr)) < 0 ||
	    (err = snd_pcm_hw_params_set_periods_near(pcm, hw, &nper, nullptr)) < 0 ||
	    (err = snd_pcm_hw_params(pcm, hw)) < 0)
	{
		fprintf(stderr, "audio: cannot configure %s: %s\n", name.c_str(), snd_strerror(err));
		snd_pcm_close(pcm);
		return false;
	}
	if (r != rate)
	{
		// no resampling here: the emulated clock is the timing authority and runs at exactly `rate`
		fprintf(stderr, "audio: %s does not support %u Hz (offered %u); use a 'plughw:' or 'default' device\n", name.c_str(), rate, r);
		snd_pcm_close(pcm);
		return false;
	}
	snd_pcm_hw_params_get_buffer_size(hw, &buffer);
	// start only when the buffer is full; wake the writer whenever a period is free
	snd_pcm_sw_params_t *sw; snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer);
	snd_pcm_sw_params_set_avail_min(pcm, sw, period);
	if ((err = snd_pcm_sw_params(pcm, sw)) < 0) { fprintf(stderr, "audio: sw params on %s: %s\n", name.c_str(), snd_strerror(err)); snd_pcm_close(pcm); return false; }
	m_h = pcm; m_period = unsigned(period); m_buffer = unsigned(buffer);
	return true;
}

bool audio_output::wait_writable(unsigned) { return m_h != nullptr; }

void audio_output::write(float const *x, size_t frames)
{
	if (!m_h) return;
	snd_pcm_t *const pcm = static_cast<snd_pcm_t *>(m_h);
	m_tmp.resize(frames * 2);
	for (size_t i = 0; i < frames * 2; ++i)
	{
		long const v = lround(double(x[i]) * 32767.0);
		m_tmp[i] = int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
	int16_t const *p = m_tmp.data();
	while (frames)
	{
		snd_pcm_sframes_t const n = snd_pcm_writei(pcm, p, frames);
		if (n == -EAGAIN) continue;
		if (n < 0)
		{
			if (n == -EPIPE) { ++m_xruns; if (on_xrun) on_xrun(m_xruns); }
			if (snd_pcm_recover(pcm, int(n), 1) < 0) { fprintf(stderr, "audio: unrecoverable error %s\n", snd_strerror(int(n))); return; }
			continue;
		}
		p += n * 2; frames -= size_t(n);
	}
}
