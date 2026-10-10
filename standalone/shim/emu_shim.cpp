// Runtime for the shim: scheduler, globals, image devices.
#include "emu.h"
#include "imagedev/harddriv.h"
#include "imagedev/cdromimg.h"
#include "sound/cdda.h"
#include <algorithm>
#include <cstring>
#include <fcntl.h>

running_machine device_t::s_machine;
thread_local FILE *device_t::s_log = nullptr;
thread_local address_space *device_memory_interface::s_host_space = nullptr;
thread_local void (*g_insn_hook)(offs_t) = nullptr;
void shim_fatal(std::string const &msg) { fprintf(stderr, "FATAL: %s\n", msg.c_str()); exit(3); }

static shim_scheduler s_default_sched;
thread_local shim_scheduler *g_shim_sched = &s_default_sched;

emu_timer::~emu_timer()
{
	auto &v = m_sched->m_timers;
	v.erase(std::remove(v.begin(), v.end(), this), v.end());
}

attotime shim_scheduler::time() const
{
	if (m_cpu_running && m_cpu) return m_cpu->local_time();
	return m_basetime;
}

attotime shim_scheduler::next_expiry() const
{
	attotime n = attotime::never;
	for (emu_timer const *t : m_timers)
		if (t->m_enabled && t->m_expire < n) n = t->m_expire;
	return n;
}

void shim_scheduler::fire_due(attotime upto)
{
	// fire in expiry order, each at its own time (MAME executes timers in time order)
	for (;;)
	{
		emu_timer *best = nullptr;
		for (emu_timer *t : m_timers)
			if (t->m_enabled && t->m_expire <= upto && (!best || t->m_expire < best->m_expire)) best = t;
		if (!best) break;
		m_basetime = best->m_expire;
		if (!best->m_period.is_never() && !best->m_period.is_zero()) best->m_expire = best->m_expire + best->m_period;
		else best->m_enabled = false;
		best->m_cb(best->m_param);
	}
	if (m_basetime < upto) m_basetime = upto;
}

emu_timer *shim_scheduler::timer_alloc(timer_expired_delegate d)
{
	m_owned.emplace_back(new emu_timer(std::move(d.m_f)));
	return m_owned.back().get();
}

void shim_scheduler::abort_timeslice()
{
	if (m_cpu_running && m_cpu) m_cpu->abort_timeslice();
}

// ---- hard disk (read-only file + RAM overlay) ----
harddisk_image_device::harddisk_image_device(machine_config const &mc, char const *tag, device_t *owner, u32 clock)
	: device_t(mc, cdda_device::s_type, tag, owner, clock) { }

bool harddisk_image_device::load(std::string const &path)
{
	m_f = fopen(path.c_str(), "rb");
	if (!m_f) return false;
	fseek(m_f, 0, SEEK_END); m_length = u64(ftell(m_f));
	// ask the kernel to start reading the OS image into the page cache now (asynchronously), so the boot's SCSI reads
	// do not wait for the disk while the engine thread must stay ahead of the audio
	posix_fadvise(fileno(m_f), 0, 0, POSIX_FADV_WILLNEED);
	// MAME lib/util/harddisk.cpp geometry guess (raw image, skip 0)
	m_info = {};
	m_info.sectorbytes = 512;
	for (u32 totalsectors = u32(m_length / m_info.sectorbytes); ; totalsectors++)
		for (u32 cursectors = 63; cursectors > 1; cursectors--)
			if (totalsectors % cursectors == 0)
			{
				u32 const totalheads = totalsectors / cursectors;
				for (u32 curheads = 16; curheads > 1; curheads--)
					if (totalheads % curheads == 0)
					{
						m_info.cylinders = totalheads / curheads; m_info.heads = curheads; m_info.sectors = cursectors;
						return true;
					}
			}
}

bool harddisk_image_device::read(u32 lba, void *buffer)
{
	if (device_t::s_log) fprintf(device_t::s_log, "HDRD t=%.6f lba=%08X\n", double(shim_sched().time().m_seconds), lba);
	if (auto it = m_overlay.find(lba); it != m_overlay.end()) { memcpy(buffer, it->second.data(), 512); return true; }
	if (!m_f || u64(lba + 1) * 512 > m_length) return false;
	fseek(m_f, long(u64(lba) * 512), SEEK_SET);
	return fread(buffer, 1, 512, m_f) == 512;
}

bool harddisk_image_device::write(u32 lba, void const *buffer)
{
	if (!m_f || u64(lba + 1) * 512 > m_length) return false;
	auto &v = m_overlay[lba]; v.assign((u8 const *)buffer, (u8 const *)buffer + 512); ++m_writes;
	return true;
}

// ---- CD-ROM (read-only single-track MODE1 ISO) ----
cdrom_image_device::cdrom_image_device(machine_config const &mc, char const *tag, device_t *owner, u32 clock)
	: device_t(mc, cdda_device::s_type, tag, owner, clock) { }

bool cdrom_image_device::load(std::string const &path)
{
	unload();
	m_f = fopen(path.c_str(), "rb");
	if (!m_f) return false;
	fseek(m_f, 0, SEEK_END); u64 const size = u64(ftell(m_f));
	if (size % 2048) { fclose(m_f); m_f = nullptr; return false; }  // only the MODE1/2048 ISO case is supported
	m_frames = u32(size / 2048);
	++m_seq;
	return true;
}

void cdrom_image_device::unload()
{
	if (m_f) { fclose(m_f); m_f = nullptr; m_frames = 0; ++m_seq; }
}

bool cdrom_image_device::read_data(u32 lba, void *buffer, u32 datatype, bool)
{
	if (device_t::s_log) fprintf(device_t::s_log, "CDRD t=%.6f lba=%08X type=%u f=%d frames=%u\n", double(shim_sched().time().m_seconds), lba, datatype, m_f != nullptr, m_frames);
	if (!m_f || lba >= m_frames) return false;
	if (datatype != cdrom_file::CD_TRACK_MODE1 && datatype != cdrom_file::CD_TRACK_RAW_DONTCARE) return false;
	fseek(m_f, long(u64(lba) * 2048), SEEK_SET);
	return fread(buffer, 1, 2048, m_f) == 2048;
}

device_type_impl const cdda_device::s_type{ "shim_device", "shim device" };
