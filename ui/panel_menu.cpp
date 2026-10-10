// Host menu bar of panel_view (see panel_view.h): a strip above the panel with one title per menu and a dropdown for
// the open one.  Logical coordinates as the panel (the bar spans y = -menu_bar_h .. 0); host UI only, no machine input.
#include "panel_view.h"
#include <algorithm>
#include <cmath>

namespace eiii::ui {

namespace {
rgba const c_bar       = rgba::hex(0x1b1c1f);
rgba const c_bar_line  = rgba::hex(0x3a3b40);
rgba const c_bar_open  = rgba::hex(0x34363c);
rgba const c_text      = rgba::hex(0xe4e2dc);
rgba const c_text_dim  = rgba::hex(0x86888e);
rgba const c_status    = rgba::hex(0xe8a868);
rgba const c_drop      = rgba::hex(0x25262a);
rgba const c_drop_edge = rgba::hex(0x4c4e54);
rgba const c_hover     = rgba::hex(0x3c5f8c);
float const title_size = 12.f, item_size = 12.f, note_size = 10.5f;
float const title_pad = 10.f, arrow_w = 10.f, item_h = 22.f, sep_h = 9.f, drop_pad = 5.f, check_w = 22.f;
}

void panel_view::set_menus(std::vector<menu> menus)
{
	bool const had = has_menu_bar();
	m_menus = std::move(menus);
	m_open = -1; m_hover = -1; m_open_items.clear();
	if (had != has_menu_bar()) { int const w = m_w, h = m_h; m_w = 0; set_size(w, h); }   // the layout changes
	m_menu_dirty = true;
}

void panel_view::set_menu_status(std::string const &text) { m_menu_status = text; }

int panel_view::find_menu(std::string const &key) const
{
	for (size_t i = 0; i < m_menus.size(); ++i) if (m_menus[i].key == key) return int(i);
	return -1;
}

void panel_view::layout_bar(std::vector<bar_slot> &out)
{
	out.clear();
	float const status_w = m_menu_status.empty() ? 0.f : m_fonts.regular.measure(m_menu_status, title_size) + 2 * title_pad;
	float const avail = panel_w - 8.f - status_w;
	std::vector<std::string> titles;
	for (auto const &m : m_menus) titles.push_back(m.title ? m.title() : m.key);
	// widest titles are shortened (with an ellipsis) until everything fits
	float cap = 340.f;
	auto fit = [&](std::string t) {
		if (m_fonts.regular.measure(t, title_size) <= cap) return t;
		while (!t.empty() && m_fonts.regular.measure(t + "\xE2\x80\xA6", title_size) > cap)
		{
			t.pop_back();
			while (!t.empty() && (uint8_t(t.back()) & 0xC0) == 0x80) t.pop_back();   // whole UTF-8 characters
		}
		return t + "\xE2\x80\xA6";
	};
	for (;;)
	{
		out.clear();
		float x = 8.f;
		for (auto const &t : titles)
		{
			std::string const f = fit(t);
			float const w = m_fonts.regular.measure(f, title_size) + 2 * title_pad + arrow_w;
			out.push_back({ x, x + w, f });
			x += w;
		}
		if (x <= avail || cap <= 60.f) break;
		cap -= 10.f;
	}
}

recti panel_view::bar_rect() const
{
	return { 0, std::max(0, int(std::floor(m_xf.Y(-menu_bar_h)))), m_w, int(std::ceil(m_xf.Y(0))) };
}

void panel_view::draw_bar(canvas &c, std::vector<bar_slot> const &slots)
{
	float const s = m_s;
	fill_rect(c, 0, m_xf.Y(-menu_bar_h), float(m_w), m_xf.Y(0), c_bar);
	fill_rect(c, 0, m_xf.Y(0) - std::max(1.f, 1.f * s), float(m_w), m_xf.Y(0), c_bar_line);
	float const base = -menu_bar_h * 0.5f + title_size * 0.36f;
	for (size_t i = 0; i < slots.size(); ++i)
	{
		bar_slot const &b = slots[i];
		if (int(i) == m_open) fill_rect(c, m_xf.X(b.x0), m_xf.Y(-menu_bar_h + 2), m_xf.X(b.x1), m_xf.Y(-1.5f), c_bar_open, 3 * s);
		bool const empty = b.title.find("\xE2\x80\x94") != std::string::npos;   // "Dn —": nothing connected
		m_fonts.regular.draw(c, m_xf.X(b.x0 + title_pad), m_xf.Y(base), b.title, title_size * s, empty ? c_text_dim : c_text);
		// small down arrow
		float const ax = b.x1 - title_pad - arrow_w * 0.5f + 2, ay = base - title_size * 0.3f;
		fill_triangle(c, m_xf.X(ax - 3.5f), m_xf.Y(ay - 2), m_xf.X(ax + 3.5f), m_xf.Y(ay - 2), m_xf.X(ax), m_xf.Y(ay + 2.5f), c_text_dim);
		if (i + 1 < slots.size()) fill_rect(c, m_xf.X(b.x1) - 0.5f * s, m_xf.Y(-menu_bar_h + 6), m_xf.X(b.x1) + 0.5f * s, m_xf.Y(-6), c_bar_line);
	}
	if (!m_menu_status.empty())
		m_fonts.regular.draw(c, m_xf.X(panel_w - title_pad), m_xf.Y(base), m_menu_status, title_size * s, c_status, 0, 2);
}

// dropdown layout: y of each item's top (logical, panel top = 0)
static float item_top(std::vector<menu_item> const &items, size_t i)
{
	float y = drop_pad;
	for (size_t k = 0; k < i; ++k) y += items[k].separator ? sep_h : item_h;
	return y;
}

recti panel_view::dropdown_rect() const
{
	if (m_open < 0) return {};
	float const h = item_top(m_open_items, m_open_items.size()) + drop_pad;
	return { int(std::floor(m_xf.X(m_drop_x))) - 2, int(std::floor(m_xf.Y(m_drop_y))) - 1, int(std::ceil(m_xf.X(m_drop_x + m_drop_w))) + 3, int(std::ceil(m_xf.Y(m_drop_y + h))) + 3 };
}

void panel_view::draw_dropdown(canvas &c)
{
	if (m_open < 0) return;
	float const s = m_s;
	float const y0 = m_drop_y, h = item_top(m_open_items, m_open_items.size()) + drop_pad;
	float const x0 = m_drop_x, x1 = m_drop_x + m_drop_w;
	fill_rect(c, m_xf.X(x0) + 2 * s, m_xf.Y(y0) + 2 * s, m_xf.X(x1) + 2 * s, m_xf.Y(y0 + h) + 2 * s, rgba::hex(0x000000, 0.45f), 3 * s);   // shadow
	fill_rect(c, m_xf.X(x0), m_xf.Y(y0), m_xf.X(x1), m_xf.Y(y0 + h), c_drop_edge, 3 * s);
	fill_rect(c, m_xf.X(x0) + s, m_xf.Y(y0) + s, m_xf.X(x1) - s, m_xf.Y(y0 + h) - s, c_drop, 2.5f * s);
	for (size_t i = 0; i < m_open_items.size(); ++i)
	{
		menu_item const &it = m_open_items[i];
		float const y = y0 + item_top(m_open_items, i);
		if (it.separator) { fill_rect(c, m_xf.X(x0 + 8), m_xf.Y(y + sep_h * 0.5f) - 0.5f * s, m_xf.X(x1 - 8), m_xf.Y(y + sep_h * 0.5f) + 0.5f * s, c_bar_line); continue; }
		if (int(i) == m_hover && it.enabled) fill_rect(c, m_xf.X(x0 + 3), m_xf.Y(y + 1), m_xf.X(x1 - 3), m_xf.Y(y + item_h - 1), c_hover, 2.5f * s);
		float const base = y + item_h * 0.5f + item_size * 0.36f;
		if (it.checked)
		{
			float const cx = x0 + 11, cy = y + item_h * 0.5f;
			fill_line(c, m_xf.X(cx - 4), m_xf.Y(cy), m_xf.X(cx - 1), m_xf.Y(cy + 3.5f), 1.6f * s, c_text);
			fill_line(c, m_xf.X(cx - 1), m_xf.Y(cy + 3.5f), m_xf.X(cx + 5), m_xf.Y(cy - 4), 1.6f * s, c_text);
		}
		m_fonts.regular.draw(c, m_xf.X(x0 + check_w), m_xf.Y(base), it.label, (it.note ? note_size : item_size) * s,
			it.note ? c_text_dim : it.enabled ? c_text : c_text_dim);
	}
}

int panel_view::bar_hit(float lx, float ly) const
{
	if (ly < -menu_bar_h || ly >= 0) return -1;
	for (size_t i = 0; i < m_bar.size(); ++i) if (lx >= m_bar[i].x0 && lx < m_bar[i].x1) return int(i);
	return -1;
}

int panel_view::item_hit(float lx, float ly) const
{
	if (m_open < 0 || lx < m_drop_x || lx >= m_drop_x + m_drop_w) return -1;
	for (size_t i = 0; i < m_open_items.size(); ++i)
	{
		float const y = m_drop_y + item_top(m_open_items, i), h = m_open_items[i].separator ? sep_h : item_h;
		if (ly >= y && ly < y + h) return m_open_items[i].separator ? -1 : int(i);
	}
	return -1;
}

void panel_view::open_menu(int index)
{
	if (index < 0 || index >= int(m_menus.size())) { close_menu(); return; }
	if (m_bar.size() != m_menus.size()) layout_bar(m_bar);
	m_open = index;
	m_open_items = m_menus[index].items ? m_menus[index].items() : std::vector<menu_item>{};
	float w = 200.f;
	for (auto const &it : m_open_items)
		if (!it.separator) w = std::max(w, m_fonts.regular.measure(it.label, it.note ? note_size : item_size) + check_w + 18.f);
	m_drop_w = std::min(w, panel_w - 8.f);
	m_drop_x = std::clamp(m_bar[size_t(index)].x0, 4.f, panel_w - 4.f - m_drop_w);
	m_drop_y = 0;
	m_hover = -1;
	m_menu_dirty = true;
}

void panel_view::open_menu_at(int index, float lx, float ly)
{
	open_menu(index);
	if (m_open < 0) return;
	// top-left at the point; moved left / up as far as needed to stay on the panel
	float const h = item_top(m_open_items, m_open_items.size()) + drop_pad;
	m_drop_x = std::clamp(lx, 4.f, panel_w - 4.f - m_drop_w);
	m_drop_y = std::clamp(ly, 0.f, std::max(0.f, panel_h - 4.f - h));
}

void panel_view::close_menu()
{
	if (m_open < 0) return;
	m_open = -1; m_hover = -1; m_open_items.clear();
	m_menu_dirty = true;
}

bool panel_view::menu_mouse_down(float lx, float ly)
{
	int const b = bar_hit(lx, ly);
	if (m_open >= 0)
	{
		if (item_hit(lx, ly) >= 0) return true;           // chosen on release
		if (b == m_open) { close_menu(); return true; }
		if (b >= 0) { open_menu(b); m_open_by_down = true; return true; }
		bool const inside = lx >= m_drop_x && lx < m_drop_x + m_drop_w && ly >= m_drop_y && ly < m_drop_y + item_top(m_open_items, m_open_items.size()) + drop_pad;
		if (!inside) close_menu();                         // a click elsewhere only closes the menu
		return true;
	}
	if (b >= 0) { open_menu(b); m_open_by_down = true; return true; }
	return ly < 0;                                         // the bar itself is not part of the panel
}

bool panel_view::menu_mouse_move(float lx, float ly)
{
	if (m_open < 0) return false;
	int const b = bar_hit(lx, ly);
	if (b >= 0 && b != m_open) { open_menu(b); return true; }
	int const h = item_hit(lx, ly);
	if (h != m_hover) { m_hover = h; m_menu_dirty = true; }
	return true;
}

bool panel_view::menu_mouse_up(float lx, float ly)
{
	if (m_open < 0) return false;
	int const i = item_hit(lx, ly);
	m_open_by_down = false;
	if (i >= 0 && m_open_items[size_t(i)].enabled)
	{
		auto const action = m_open_items[size_t(i)].action;
		close_menu();
		if (action) action();
	}
	return true;
}

} // namespace eiii::ui
