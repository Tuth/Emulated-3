// Shim replacement for MAME imagedev/harddriv.h: raw hard-disk image, opened READ-ONLY; writes go to an in-memory
// overlay so the proven image file is never modified.  Geometry guess copied from MAME lib/util/harddisk.cpp
// (hard_disk_file::hard_disk_file(corefile, skipoffs)) so READ CAPACITY / MODE SENSE match MAME exactly.
#pragma once
#include "emu.h"
#include <map>
#include <system_error>
#include <string>

struct hard_disk_file { struct info { u32 cylinders = 0, heads = 0, sectors = 0, sectorbytes = 512; }; };

class harddisk_image_device : public device_t
{
public:
	harddisk_image_device(machine_config const &mc, char const *tag, device_t *owner, u32 clock = 0);
	~harddisk_image_device() { if (m_f) fclose(m_f); }
	bool load(std::string const &path);
	bool exists() const { return m_f != nullptr; }
	hard_disk_file::info const &get_info() const { return m_info; }
	bool read(u32 lba, void *buffer);
	bool write(u32 lba, void const *buffer);
	std::error_condition get_inquiry_data(std::vector<u8> &) const { return std::make_error_condition(std::errc::no_such_file_or_directory); }
	harddisk_image_device &set_interface(char const *) { return *this; }
	u64 writes() const { return m_writes; }
	// the writes as a change set (sectors over the read-only image), e.g. to keep them with a project:
	// "EIIIHDC1", image length (u64), image fingerprint (u64), sector count (u32), then per sector: LBA (u32) + 512 bytes
	std::string export_changes() const;
	bool import_changes(std::string const &blob, std::string &error);   // replaces the current changes
	size_t changed_sectors() const { return m_overlay.size(); }
	u64 length() const { return m_length; }
	u64 fingerprint() const { return m_fingerprint; }
private:
	FILE *m_f = nullptr;
	u64 m_length = 0;
	hard_disk_file::info m_info;
	std::map<u32, std::vector<u8>> m_overlay;
	u64 m_writes = 0;
	u64 m_fingerprint = 0;          // FNV-1a over the image's first 64 KiB (which image a change set belongs to)
};
template <typename... A> harddisk_image_device &HARDDISK(machine_config &, A &&...) { throw std::logic_error("HARDDISK() config not used"); }
