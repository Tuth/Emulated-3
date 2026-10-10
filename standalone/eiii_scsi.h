// EIII-specific SCSI target classes, copied VERBATIM from mame/src/mame/emusys/emu3.cpp (BSD-3-Clause) by
// extract script — do not edit here; they carry the proven EIII SCSI timing contract.
#pragma once
#include "emu.h"
#include "bus/nscsi/cd.h"
#include "bus/nscsi/hd.h"

nscsi_harddisk_device *eiii_make_hd(machine_config const &mc, char const *tag, device_t *owner);
nscsi_cdrom_device *eiii_make_cd(machine_config const &mc, char const *tag, device_t *owner);
