/* eiii_core.h — host API of the authentic EIII core (C ABI, for standalone, plugin and bridge hosts).
 *
 * The core = boot ROM + OS 2.42 + overlays running on the virtual motherboard. Hosts talk to it only through
 * hardware-level boundaries (media, MIDI bytes, panel scanner codes, ADC values, audio in/out), never through
 * firmware data structures.  Documented in analysis/EIII_CORE_API.md.
 *
 * Implementation status (2026-10-05): the CPU-side machine is not yet extracted from MAME; the functions marked
 * [BACKEND] are implemented by host/eiii_backend.* and exercised by tools/eiii_fakehost from recorded MAME traces.
 * All other functions are the agreed contract for the extraction (return EIII_E_NOTIMPL until then). */
#ifndef EIII_CORE_H
#define EIII_CORE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct eiii_core eiii_core;

typedef enum { EIII_OK = 0, EIII_E_ARG = -1, EIII_E_IO = -2, EIII_E_STATE = -3, EIII_E_NOTIMPL = -4 } eiii_result;

typedef struct eiii_config {
	const char *bootrom_lo, *bootrom_hi;  /* E3 LSBOOT / MSBOOT PROM images (27128 x2)            */
	double      output_rate;              /* host audio rate for eiii_render_audio (e.g. 48000)    */
	int         output_backend;           /* 0 = software CEM3387 model, 1 = raw lanes (bridge)    */
} eiii_config;

/* lifecycle */
eiii_core  *eiii_create(const eiii_config *cfg);
void        eiii_destroy(eiii_core *c);
eiii_result eiii_reset(eiii_core *c);                     /* power-on reset of the virtual motherboard */

/* media (virtual SCSI; the original OS loads banks itself) */
eiii_result eiii_mount_hd(eiii_core *c, int scsi_id, const char *image_path);
eiii_result eiii_mount_cd(eiii_core *c, int scsi_id, const char *iso_path);  /* reports a media change */
eiii_result eiii_eject_cd(eiii_core *c, int scsi_id);
eiii_result eiii_mount_floppy(eiii_core *c, const char *image_path);

/* inputs at hardware boundaries; 'when' = core time in 10 MHz CPU cycles, 0 = as soon as possible */
eiii_result eiii_midi_in(eiii_core *c, const uint8_t *bytes, size_t n, uint64_t when);     /* SCC ch A, 31250 Bd */
eiii_result eiii_panel_event(eiii_core *c, uint8_t scanner_code, int pressed);            /* C0/C3 make/break   */
eiii_result eiii_key_event(eiii_core *c, uint8_t key, uint8_t velocity, int pressed);     /* scanner keyboard   */
eiii_result eiii_extra_switch(eiii_core *c, unsigned bit, int pressed);                   /* 0xFA0000 switches  */
eiii_result eiii_set_adc_control(eiii_core *c, unsigned mux_channel, uint16_t adc_units); /* pots: wheels etc.  */
eiii_result eiii_audio_in(eiii_core *c, const int16_t *lr, size_t frames, double rate);   /* sampling inputs    */

/* running */
uint64_t    eiii_run_cycles(eiii_core *c, uint64_t cycles);       /* returns cycles actually executed */
uint64_t    eiii_time(const eiii_core *c);                        /* core time, 10 MHz cycles         */
size_t      eiii_render_audio(eiii_core *c, float *lr, size_t frames); /* runs the core as needed [BACKEND] */

/* outputs */
eiii_result eiii_get_lcd(const eiii_core *c, char text[2][41], int *cursor_row, int *cursor_col);
uint16_t    eiii_get_leds(const eiii_core *c);
eiii_result eiii_midi_out(eiii_core *c, uint8_t *bytes, size_t cap, size_t *n);

/* debug / acceptance (read-only views of hardware state, not firmware structures) */
typedef struct eiii_voice_debug {
	uint32_t rf[4];       /* register file: current size, initial size, current address, initial address */
	uint16_t rate;        /* F-Chip divisor N (sample rate = 20 MHz / N) */
	uint8_t  active;      /* lane running (0 after a one-shot ends) */
	int16_t  pcm;         /* current raw lane output */
	uint16_t cv[5];       /* last CV words: Fc1, Fc2, Q, Gain, Pan */
} eiii_voice_debug;
eiii_result eiii_get_voice_debug(const eiii_core *c, unsigned voice, eiii_voice_debug *out);   /* [BACKEND] */

/* state */
eiii_result eiii_save_state(const eiii_core *c, void *buf, size_t cap, size_t *size);
eiii_result eiii_load_state(eiii_core *c, const void *buf, size_t size);

#ifdef __cplusplus
}
#endif
#endif
