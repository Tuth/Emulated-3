// Operator automation of the ORIGINAL front panel: a list of steps "wait until the LCD shows X" / "press switch Y".
// It only produces panel/extra-switch presses (the same scanner FIFO / 0xFA0000 inputs a person would produce) and reads
// the LCD the firmware drew; it never touches RAM or firmware state.  Ticked from an emulated-time timer, so runs are
// deterministic and reproducible.
#pragma once
#include <functional>
#include <regex>
#include <string>
#include <vector>

class ui_macro
{
public:
	enum class kind { wait_lcd, press, extra, pause };
	struct step { kind k; std::string arg; double timeout_s; std::string what; };

	// press/extra: callbacks (down=true/false) supplied by the host
	std::function<void(std::string const &, bool)> panel, extra;
	std::function<std::string()> lcd;                       // "row0\nrow1"
	double hold_s = 0.08, gap_s = 0.25, settle_s = 0.30;

	void wait(std::string const &re, double timeout_s, std::string what) { m_steps.push_back({ kind::wait_lcd, re, timeout_s, std::move(what) }); }
	void press(std::string const &name) { m_steps.push_back({ kind::press, name, 0, "press " + name }); }
	void extra_press(std::string const &name) { m_steps.push_back({ kind::extra, name, 0, "press " + name }); }
	void pause(double s) { m_steps.push_back({ kind::pause, std::to_string(s), 0, "pause" }); }
	bool empty() const { return m_steps.empty(); }
	bool done() const { return m_i >= m_steps.size(); }
	bool failed() const { return m_failed; }
	std::string const &error() const { return m_error; }

	// call periodically with the emulated time; returns true while still running
	bool tick(double now)
	{
		if (done() || m_failed) return false;
		if (now < m_next) return true;
		step const &s = m_steps[m_i];
		switch (s.k)
		{
		case kind::wait_lcd:
			if (m_wait_start < 0) m_wait_start = now;
			if (std::regex_search(lcd(), std::regex(s.arg)))
			{
				if (m_log) m_log(now, "LCD matched: " + s.what);
				advance(now + settle_s);
			}
			else if (now - m_wait_start > s.timeout_s)
			{
				m_failed = true;
				m_error = "timed out waiting for " + s.what + " (/" + s.arg + "/); LCD shows: " + lcd();
			}
			break;
		case kind::press:
		case kind::extra:
			if (!m_down) { (s.k == kind::press ? panel : extra)(s.arg, true); m_down = true; m_next = now + hold_s; }
			else { (s.k == kind::press ? panel : extra)(s.arg, false); m_down = false; if (m_log) m_log(now, s.what); advance(now + gap_s); }
			break;
		case kind::pause:
			advance(now + std::stod(s.arg));
			break;
		}
		return !done() && !m_failed;
	}
	std::function<void(double, std::string const &)> m_log;

private:
	void advance(double next) { ++m_i; m_next = next; m_wait_start = -1; }
	std::vector<step> m_steps;
	size_t m_i = 0;
	double m_next = 0, m_wait_start = -1;
	bool m_down = false, m_failed = false;
	std::string m_error;
};
