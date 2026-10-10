// EIII 4U rack front panel: what each control is and which hardware input it drives; what each LED shows and which
// latch bit drives it.  No geometry (that is the UI's: ui/panel_layout), no CPU addresses (that is the runtime's).
// Evidence: analysis/EIII_RACK_PANEL_CONTROL_MAP.md, analysis/EIII_PANEL_LED_MAP.md.
#pragma once
#include <cstddef>
#include <cstdint>

namespace eiii {

enum class panel_route : uint8_t
{
	scan,       // panel scanner FIFO: make C0 code, break C3 code
	xsw,        // extra-switch port bit (level while held)
	adc,        // ADC board multiplexer input X0..X7 (firmware ADC units)
	host,       // host UI only (no path into the emulated machine)
};

enum class panel_kind : uint8_t { button, slider, knob };

enum class panel_status : uint8_t { proven, proven_path, host_ui, unimplemented_hardware };

struct panel_control
{
	char const *id;             // stable identifier ("load_bank", "keypad_4", "data_slider", ...)
	char const *label;          // printed rack legend
	char const *internal;       // MAME port name (scripts, eiii_panel_codes.h); "" for host controls
	panel_kind kind;
	panel_route route;
	uint8_t code;               // scan: scanner code; xsw: bit mask; adc: multiplexer input
	panel_status status;
};

enum class led_source : uint8_t { led_latch, misc_latch };

struct panel_led
{
	char const *id;
	char const *label;
	led_source source;
	uint8_t bit;                // active low
};

extern panel_control const panel_controls[];
extern size_t const panel_control_count;
extern panel_led const panel_leds[];
extern size_t const panel_led_count;

int panel_find_control(char const *id);   // index or -1
int panel_find_led(char const *id);

// sliders: electrical range driven over the full mechanical travel (MODEL CHOICE: covers the calibration pairs stored
// on the OS 2.42 disk — Data {-7, 0x6FDB}, Sample Input {0, 0x7000}, Master Volume {1, 0x708D})
constexpr uint16_t slider_adc_min = 0x0000, slider_adc_max = 0x7100;
uint16_t slider_to_adc(double position01);
double adc_to_slider(uint16_t value);
// power-on positions of the three sliders (the runtime's defaults, from the MAME driver)
uint16_t slider_default_adc(int control);

} // namespace eiii
