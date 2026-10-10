#include "eiii_scsi.h"

// Early EIII software predates standardized CD-ROM peripheral discovery.  The
// supported Sony mechanism identifies as direct-access while retaining
// read-only CD media semantics and 512-byte logical blocks.
class emu3_cdrom_device : public nscsi_cdrom_device
{
public:
	emu3_cdrom_device(machine_config const &mconfig, char const *tag, device_t *owner, u32 clock = 0);

protected:
	virtual void scsi_command() override;
	virtual attotime scsi_data_command_delay() override
     {
          // EIII starts MODE SELECT parameter DMA immediately after the CDB.
          // Holding the target in COMMAND for 1 ms makes the host finish its
          // send routine before DATA OUT/REQ appears.
          return m_scsi_cmdbuf[0] == 0x15
               ? attotime::zero
               : attotime::from_msec(1);
     }
	virtual attotime scsi_phase_transition_delay() override { return attotime::from_msec(1); }
	virtual attotime scsi_bus_settle_delay() override { return attotime::from_msec(1); }
};


class emu3_harddisk_device : public nscsi_harddisk_device
{
public:
        emu3_harddisk_device(
                machine_config const &mconfig,
                char const *tag,
                device_t *owner,
                u32 clock = 0);

protected:
        virtual attotime scsi_data_command_delay() override
        {
                // Match the proven EIII CD timing contract.
                // MODE SELECT DATA OUT must begin immediately; other
                // commands need a small real-device turnaround interval.
                return m_scsi_cmdbuf[0] == 0x15
                        ? attotime::zero
                        : attotime::from_msec(1);
        }

        virtual attotime scsi_phase_transition_delay() override
        {
                // EIII firmware does not tolerate an instantaneous
                // command/data/status phase handoff.
                return attotime::from_msec(1);
        }

        virtual attotime scsi_bus_settle_delay() override
        {
                // EIII updates target-selection data after asserting SEL.
                return attotime::from_msec(1);
        }
};

DEFINE_DEVICE_TYPE_PRIVATE(
        EMU3_HARDDISK,
        nscsi_harddisk_device,
        emu3_harddisk_device,
        "emu3_harddisk",
        "E-mu EIII-compatible SCSI Hard Disk")

emu3_harddisk_device::emu3_harddisk_device(
        machine_config const &mconfig,
        char const *tag,
        device_t *owner,
        u32 clock)
        : nscsi_harddisk_device(
                mconfig,
                EMU3_HARDDISK,
                tag,
                owner,
                clock)
{
        // Keep MAME's period-appropriate default identity for now.
        // We are changing selection timing only.
}

DEFINE_DEVICE_TYPE_PRIVATE(EMU3_CDROM, nscsi_cdrom_device, emu3_cdrom_device, "emu3_cdrom", "E-mu EIII-compatible CD-ROM")

emu3_cdrom_device::emu3_cdrom_device(machine_config const &mconfig, char const *tag, device_t *owner, u32 clock)
	: nscsi_cdrom_device(mconfig, EMU3_CDROM, tag, owner, "SONY    ", "CD-ROM CDU-8002 ", "1.8g", 0x00, 0x01)
{
}

void emu3_cdrom_device::scsi_command()
{
	bool const legacy_inquiry = m_scsi_cmdbuf[0] == SC_INQUIRY
		&& !(m_scsi_cmdbuf[1] & 0xe0) && !m_scsi_cmdbuf[2];

	nscsi_cdrom_device::scsi_command();

	if (legacy_inquiry)
		m_scsi_cmdbuf[0] = 0x05; // diagnostic: genuine CD-ROM peripheral type
}


// ---- standalone host factories (not part of the verbatim copy above) ----
nscsi_harddisk_device *eiii_make_hd(machine_config const &mc, char const *tag, device_t *owner) { return new emu3_harddisk_device(mc, tag, owner, 0); }
nscsi_cdrom_device *eiii_make_cd(machine_config const &mc, char const *tag, device_t *owner) { return new emu3_cdrom_device(mc, tag, owner, 0); }
