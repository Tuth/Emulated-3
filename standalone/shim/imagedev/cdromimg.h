// Shim replacement for MAME imagedev/cdromimg.h: single-track MODE1 .iso opened READ-ONLY, with the TOC MAME's
// cdrom_file::parse_iso + constructor produce (track 0 at frame 0, lead-out at size/2048, no pregap/postgap).
// mount/eject bump the sequence counter (media change), as the MAME image device does on load/unload.
#pragma once
#include "emu.h"
#include "cdrom.h"

class cdrom_image_device : public device_t
{
public:
	cdrom_image_device(machine_config const &mc, char const *tag, device_t *owner, u32 clock = 0);
	~cdrom_image_device() { unload(); }
	bool load(std::string const &path);
	void unload();
	bool exists() const { return m_f != nullptr; }
	int sequence_counter() const { return m_seq; }
	u32 get_track_start(u32 track) const { return (track == 0xaa || track >= 1) ? m_frames : 0; }
	int get_last_track() const { return exists() ? 1 : 0; }
	int get_adr_control(int track) const { (void)track; return (cdrom_file::CD_FLAG_ADR_START_TIME << 4) | cdrom_file::CD_FLAG_CONTROL_DATA_TRACK; }
	u32 get_track(u32 frame) const { (void)frame; return 0; }
	int get_track_type(int) const { return cdrom_file::CD_TRACK_MODE1; }
	bool read_data(u32 lba, void *buffer, u32 datatype, bool phys = false);
	cdrom_image_device &set_interface(char const *) { return *this; }
private:
	FILE *m_f = nullptr;
	u32 m_frames = 0;
	int m_seq = 0;
};
template <typename... A> cdrom_image_device &CDROM(machine_config &, A &&...) { throw std::logic_error("CDROM() config not used"); }
