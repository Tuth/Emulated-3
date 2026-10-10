// EIII 4U rack panel model — see eiii_panel_model.h.  Codes and bits: MAME emu3.cpp port definitions
// (standalone/eiii_panel_codes.h); LED bits: analysis/EIII_PANEL_LED_MAP.md.
#include "eiii_panel_model.h"
#include <cmath>
#include <cstring>

namespace eiii {

#define B panel_kind::button
#define S panel_kind::slider
#define K panel_kind::knob
#define SCAN panel_route::scan
#define XSW panel_route::xsw
#define ADC panel_route::adc
#define HOST panel_route::host
#define P panel_status::proven
#define PP panel_status::proven_path
#define HU panel_status::host_ui
#define UH panel_status::unimplemented_hardware

panel_control const panel_controls[] = {
	// id                    rack legend              MAME port name          kind route code  status
	{ "power",               "POWER",                 "",                     B, HOST, 0x00, HU },
	{ "contrast",            "Contrast",              "",                     K, HOST, 0x00, HU },
	{ "cursor_up",           "Cursor/Page Up",        "Cursor Up",            B, SCAN, 0x06, PP },
	{ "cursor_left",         "Cursor/Page Left",      "Cursor Left",          B, SCAN, 0x11, P },
	{ "cursor_right",        "Cursor/Page Right",     "Cursor Right",         B, SCAN, 0x01, P },
	{ "cursor_down",         "Cursor/Page Down",      "Cursor Down",          B, SCAN, 0x09, PP },
	{ "keypad_1",            "1",                     "Keypad 1",             B, SCAN, 0x03, P },
	{ "keypad_2",            "2",                     "Keypad 2",             B, SCAN, 0x0b, P },
	{ "keypad_3",            "3",                     "Keypad 3",             B, SCAN, 0x13, P },
	{ "keypad_4",            "4",                     "Keypad 4",             B, SCAN, 0x04, P },
	{ "keypad_5",            "5",                     "Keypad 5",             B, SCAN, 0x0c, P },
	{ "keypad_6",            "6",                     "Keypad 6",             B, SCAN, 0x14, P },
	{ "keypad_7",            "7",                     "Keypad 7",             B, SCAN, 0x05, P },
	{ "keypad_8",            "8",                     "Keypad 8",             B, SCAN, 0x0d, P },
	{ "keypad_9",            "9",                     "Keypad 9",             B, SCAN, 0x15, P },
	{ "keypad_0",            "0",                     "Keypad 0",             B, SCAN, 0x16, P },
	{ "yes",                 "+/Yes/On",              "On/Yes",               B, SCAN, 0x19, P },
	{ "no",                  "-/No/Off",              "Off/No",               B, SCAN, 0x1a, P },
	{ "transpose",           "Transpose",             "Transpose",            B, SCAN, 0x0a, P },
	{ "supermode",           "Supermode",             "Dynamic Allocation",   B, SCAN, 0x1b, P },
	{ "save_bank",           "Save Bank",             "Save Bank",            B, SCAN, 0x1c, P },
	{ "load_bank",           "Load Bank",             "Load Bank",            B, SCAN, 0x1d, P },
	{ "enter",               "Enter",                 "Enter",                B, XSW,  0x08, P },
	{ "data_slider",         "Data",                  "Data Slider",          S, ADC,  3,    P },
	{ "sample_input",        "Sample Input",          "Sample Input Level",   S, ADC,  0,    UH },
	{ "master_volume",       "Master Volume",         "Volume Slider",        S, ADC,  5,    P },
	{ "seq_mode",            "MODE",                  "Mode",                 B, XSW,  0x10, P },
	{ "seq_management",      "MANAGEMENT",            "Management",           B, XSW,  0x20, P },
	{ "seq_setup",           "SETUP",                 "Sequencer Setup",      B, SCAN, 0x18, P },
	{ "seq_edit",            "EDIT",                  "Sequencer Edit",       B, SCAN, 0x10, P },
	{ "play",                "Play",                  "Play",                 B, SCAN, 0x17, P },
	{ "stop",                "Stop/Continue",         "Stop",                 B, SCAN, 0x1f, P },
	{ "locate_left",         "Locate <<",             "Locate (Left)",        B, SCAN, 0x07, PP },
	{ "locate_right",        "Locate >>",             "Locate (Right)",       B, SCAN, 0x0f, PP },
	{ "record",              "Record (Insert)",       "Record",               B, XSW,  0x02, P },
	{ "erase",               "Erase",                 "Erase",                B, XSW,  0x01, P },
	{ "master",              "MASTER",                "Master",               B, XSW,  0x40, P },
	{ "sample",              "SAMPLE",                "Sample",               B, XSW,  0x80, P },
	{ "preset_management",   "PRESET MANAGEMENT",     "Preset Management",    B, SCAN, 0x08, P },
	{ "digital_processing",  "DIGITAL PROCESSING",    "Digital Processing",    B, SCAN, 0x00, P },
	{ "preset_definition",   "PRESET DEFINITION",     "Preset Definition",    B, SCAN, 0x12, P },
	{ "analog_processing",   "ANALOG PROCESSING",     "Analog Processing",    B, SCAN, 0x02, P },
};
size_t const panel_control_count = sizeof panel_controls / sizeof panel_controls[0];

panel_led const panel_leds[] = {
	{ "led_sample",             "Sample",              led_source::led_latch, 0 },
	{ "led_master",             "Master",              led_source::led_latch, 1 },
	{ "led_seq_management",     "Management",          led_source::led_latch, 2 },
	{ "led_mode_segment",       "Segment/Cue List",    led_source::led_latch, 3 },
	{ "led_mode_song",          "Song",                led_source::led_latch, 4 },
	{ "led_seq_setup",          "Setup",               led_source::led_latch, 5 },
	{ "led_seq_edit",           "Edit",                led_source::led_latch, 6 },
	{ "led_preset_management",  "Preset Management",   led_source::led_latch, 7 },
	{ "led_digital_processing", "Digital Processing",  led_source::led_latch, 8 },
	{ "led_analog_processing",  "Analog Processing",   led_source::led_latch, 9 },
	{ "led_preset_definition",  "Preset Definition",   led_source::led_latch, 10 },
	{ "led_transpose",          "Transpose",           led_source::led_latch, 11 },
	{ "led_supermode",          "Supermode",           led_source::led_latch, 12 },
	{ "led_play",               "Play",                led_source::led_latch, 13 },
	{ "led_enter",              "Enter",               led_source::led_latch, 14 },
	{ "led_record",             "Record (Insert)",     led_source::led_latch, 15 },
	{ "led_erase",              "Erase",               led_source::misc_latch, 7 },
};
size_t const panel_led_count = sizeof panel_leds / sizeof panel_leds[0];

int panel_find_control(char const *id) { for (size_t i = 0; i < panel_control_count; ++i) if (!strcmp(panel_controls[i].id, id)) return int(i); return -1; }
int panel_find_led(char const *id) { for (size_t i = 0; i < panel_led_count; ++i) if (!strcmp(panel_leds[i].id, id)) return int(i); return -1; }

uint16_t slider_to_adc(double p)
{
	p = p < 0 ? 0 : p > 1 ? 1 : p;
	return uint16_t(std::lround(slider_adc_min + p * (slider_adc_max - slider_adc_min)));
}
double adc_to_slider(uint16_t v)
{
	double const p = double(int(v) - slider_adc_min) / double(slider_adc_max - slider_adc_min);
	return p < 0 ? 0 : p > 1 ? 1 : p;
}
uint16_t slider_default_adc(int control)
{
	if (control < 0) return 0;
	switch (panel_controls[control].code)
	{
	case 0: return 0x3800;      // X0 Sample Input (MAME adc_pot0 default)
	case 3: return 0x0001;      // X3 Data (minimum)
	case 5: return 0x708d;      // X5 Master Volume (maximum, as calibrated)
	default: return 0;
	}
}

} // namespace eiii
