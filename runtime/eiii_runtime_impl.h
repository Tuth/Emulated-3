// Internal state of eiii::runtime (standalone diagnostics may use it; UI/plugin code must not include this header).
#pragma once
#include "eiii_runtime.h"
#include "eiii_machine.h"
#include "machine/nscsi_bus.h"
#include "imagedev/harddriv.h"
#include "imagedev/cdromimg.h"
#include "sound/cdda.h"
#include "../standalone/eiii_scsi.h"
#include <memory>
#include <utility>

namespace eiii {

struct runtime::impl
{
	// declared first: destroyed last (devices and timers unregister from it)
	shim_scheduler sched;
	runtime_config cfg;
	machine_t m;
	machine_config mcfg;
	std::unique_ptr<ns32016_device> cpu;
	std::unique_ptr<ns32081_device> fpu;
	std::unique_ptr<nscsi_bus_device> scsi;
	std::unique_ptr<nscsi_harddisk_device> hd;
	std::unique_ptr<harddisk_image_device> hd_img;
	std::unique_ptr<nscsi_cdrom_device> cd;
	std::unique_ptr<cdrom_image_device> cd_img;
	std::unique_ptr<cdda_device> cd_cdda;
	// extra targets (IDs 4..7), present only when configured
	struct extra_target
	{
		std::unique_ptr<nscsi_harddisk_device> hd;
		std::unique_ptr<harddisk_image_device> hd_img;
		std::unique_ptr<nscsi_cdrom_device> cd;
		std::unique_ptr<cdrom_image_device> cd_img;
		std::unique_ptr<cdda_device> cdda;
	} extra[8];
	std::string warning;
	harddisk_image_device *hd_image(unsigned id) const { return id == 1 ? hd_img.get() : id < 8 ? extra[id].hd_img.get() : nullptr; }
	cdrom_image_device *cd_image(unsigned id) { return id == 3 ? cd_img.get() : id < 8 ? extra[id].cd_img.get() : nullptr; }
	std::unique_ptr<ncr5380_device> hdc;
	std::unique_ptr<scc85230_device> scc;
	std::unique_ptr<z80scc_channel> scc_cha, scc_chb;
	std::unique_ptr<software_backend> backend;

	// MIDI IN: bytes are clocked into SCC channel A RxD as 31250-baud 8N1 serial (idle high), like MAME's midiin port
	struct midi_tx_t { std::vector<std::pair<double, std::vector<u8>>> ev; size_t next = 0; std::vector<u8> q; size_t q_head = 0; int bit = -1; u8 cur = 0; } midi;
	std::unique_ptr<emu_timer> midi_timer, scan_irq_timer;

	int stop = 0;
	attotime wd_last = attotime::zero; unsigned long wd_spins = 0;
	bool powered = false;
	std::vector<device_t *> order;

	void bind();
	bool open(runtime_config const &c, std::string &err);
	void power_on();
	void midi_tick();
};

} // namespace eiii
