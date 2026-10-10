// Shim: the cdrom_file constants and helpers used by bus/nscsi/cd.cpp (values from MAME lib/util/cdrom.h).
#pragma once
#include "emu.h"
class cdrom_file
{
public:
	enum { CD_TRACK_MODE1 = 0, CD_TRACK_MODE1_RAW, CD_TRACK_MODE2, CD_TRACK_MODE2_FORM1, CD_TRACK_MODE2_FORM2,
	       CD_TRACK_MODE2_FORM_MIX, CD_TRACK_MODE2_RAW, CD_TRACK_AUDIO, CD_TRACK_RAW_DONTCARE };
	enum { CD_FLAG_CONTROL_DATA_TRACK = 4 };
	enum { CD_FLAG_ADR_START_TIME = 1 };
	static inline u32 msf_to_lba(u32 msf) { return (((msf & 0x00ff0000) >> 16) * 60 * 75) + (((msf & 0x0000ff00) >> 8) * 75) + (msf & 0xff); }
};
