// Minimal software rasterizer — see raster.h.
#include "raster.h"
#include <algorithm>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb/stb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "third_party/stb/stb_image_write.h"

extern "C" {
extern unsigned char const eiii_font_regular[], eiii_font_regular_end[];
extern unsigned char const eiii_font_bold[], eiii_font_bold_end[];
extern unsigned char const eiii_font_narrow[], eiii_font_narrow_end[];
}

namespace eiii::ui {

void canvas::copy_from(canvas const &s, recti r)
{
	r.x0 = std::max(r.x0, 0); r.y0 = std::max(r.y0, 0); r.x1 = std::min(r.x1, std::min(w, s.w)); r.y1 = std::min(r.y1, std::min(h, s.h));
	for (int y = r.y0; y < r.y1; ++y) memcpy(&px[size_t(y) * w + r.x0], &s.px[size_t(y) * s.w + r.x0], size_t(r.x1 - r.x0) * 4);
}

bool canvas::write_png(std::string const &path) const
{
	std::vector<uint8_t> rgb(size_t(w) * h * 3);
	for (size_t i = 0; i < px.size(); ++i) { rgb[i * 3] = uint8_t(px[i] >> 16); rgb[i * 3 + 1] = uint8_t(px[i] >> 8); rgb[i * 3 + 2] = uint8_t(px[i]); }
	return stbi_write_png(path.c_str(), w, h, 3, rgb.data(), w * 3) != 0;
}

float sd_convex(float x, float y, float const *p, int n)
{
	// orientation-independent: distance to each edge line, sign from the polygon's winding
	float area = 0;
	for (int i = 0; i < n; ++i) { int const j = (i + 1) % n; area += p[2 * i] * p[2 * j + 1] - p[2 * j] * p[2 * i + 1]; }
	float const s = area > 0 ? 1.f : -1.f;
	float d = -1e30f;
	for (int i = 0; i < n; ++i)
	{
		int const j = (i + 1) % n;
		float const ex = p[2 * j] - p[2 * i], ey = p[2 * j + 1] - p[2 * i + 1], l = std::hypot(ex, ey);
		if (l <= 0) continue;
		float const e = s * ((x - p[2 * i]) * ey - (y - p[2 * i + 1]) * ex) / l;   // > 0 outside
		d = std::max(d, e);
	}
	return d;
}

void fill_circle(canvas &c, float cx, float cy, float r, rgba col)
{
	c.fill(cx - r, cy - r, cx + r, cy + r, [&](float x, float y) { return sd_circle(x, y, cx, cy, r); }, [&](float, float) { return col; });
}
void fill_rect(canvas &c, float x0, float y0, float x1, float y1, rgba col, float radius)
{
	c.fill(x0, y0, x1, y1, [&](float x, float y) { return sd_round_rect(x, y, x0, y0, x1, y1, radius); }, [&](float, float) { return col; });
}
void fill_line(canvas &c, float ax, float ay, float bx, float by, float width, rgba col)
{
	float const r = width * .5f;
	c.fill(std::min(ax, bx) - r, std::min(ay, by) - r, std::max(ax, bx) + r, std::max(ay, by) + r,
		[&](float x, float y) { return sd_segment(x, y, ax, ay, bx, by, r); }, [&](float, float) { return col; });
}
void fill_triangle(canvas &c, float ax, float ay, float bx, float by, float cx, float cy, rgba col)
{
	float const p[6] = { ax, ay, bx, by, cx, cy };
	c.fill(std::min({ ax, bx, cx }), std::min({ ay, by, cy }), std::max({ ax, bx, cx }), std::max({ ay, by, cy }),
		[&](float x, float y) { return sd_convex(x, y, p, 3); }, [&](float, float) { return col; });
}

// ---- text ----
namespace {
uint32_t next_cp(std::string_view s, size_t &i)
{
	unsigned char const c = (unsigned char)s[i++];
	if (c < 0x80) return c;
	int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
	uint32_t cp = c & (0x3f >> n);
	while (n-- && i < s.size()) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3f);
	return cp;
}
}

bool font::load(unsigned char const *data, size_t)
{
	auto info = std::make_shared<stbtt_fontinfo>();
	if (!stbtt_InitFont(info.get(), data, stbtt_GetFontOffsetForIndex(data, 0))) return false;
	m_info = info; m_data = data; m_cache.clear();
	return true;
}

float font::cap_height(float size_px) const
{
	auto *fi = static_cast<stbtt_fontinfo *>(m_info.get());
	int x0, y0, x1, y1;
	if (stbtt_GetCodepointBox(fi, 'H', &x0, &y0, &x1, &y1)) return float(y1) * stbtt_ScaleForPixelHeight(fi, size_px);
	return size_px * 0.7f;
}

font::glyph_bitmap const &font::glyph(uint32_t cp, float size_px, int subpx)
{
	uint64_t const key = (uint64_t(cp) << 24) | (uint64_t(std::lround(size_px * 16)) << 4) | uint64_t(subpx & 15);
	auto it = m_cache.find(key);
	if (it != m_cache.end()) return it->second;
	auto *fi = static_cast<stbtt_fontinfo *>(m_info.get());
	float const sc = stbtt_ScaleForPixelHeight(fi, size_px);
	glyph_bitmap g;
	int x0, y0, x1, y1;
	stbtt_GetCodepointBitmapBoxSubpixel(fi, int(cp), sc, sc, subpx / 4.f, 0, &x0, &y0, &x1, &y1);
	g.w = x1 - x0; g.h = y1 - y0; g.xoff = x0; g.yoff = y0;
	if (g.w > 0 && g.h > 0)
	{
		g.a.resize(size_t(g.w) * g.h);
		stbtt_MakeCodepointBitmapSubpixel(fi, g.a.data(), g.w, g.h, g.w, sc, sc, subpx / 4.f, 0, int(cp));
	}
	return m_cache.emplace(key, std::move(g)).first->second;
}

float font::measure(std::string_view s, float size_px, float tracking)
{
	auto *fi = static_cast<stbtt_fontinfo *>(m_info.get());
	float const sc = stbtt_ScaleForPixelHeight(fi, size_px);
	float x = 0; uint32_t prev = 0; size_t i = 0; int n = 0;
	while (i < s.size())
	{
		uint32_t const cp = next_cp(s, i);
		int adv, lsb; stbtt_GetCodepointHMetrics(fi, int(cp), &adv, &lsb);
		if (prev) x += sc * stbtt_GetCodepointKernAdvance(fi, int(prev), int(cp));
		x += adv * sc + tracking; prev = cp; ++n;
	}
	return n ? x - tracking : 0;
}

float font::draw(canvas &c, float x, float baseline, std::string_view s, float size_px, rgba col, float tracking, int align)
{
	if (!m_info) return 0;
	float const wtot = measure(s, size_px, tracking);
	if (align == 1) x -= wtot * .5f; else if (align == 2) x -= wtot;
	auto *fi = static_cast<stbtt_fontinfo *>(m_info.get());
	float const sc = stbtt_ScaleForPixelHeight(fi, size_px);
	int const by = int(std::lround(baseline));
	uint32_t prev = 0; size_t i = 0;
	while (i < s.size())
	{
		uint32_t const cp = next_cp(s, i);
		if (prev) x += sc * stbtt_GetCodepointKernAdvance(fi, int(prev), int(cp));
		int const ix = int(std::floor(x)); int const sub = int((x - ix) * 4) & 3;
		glyph_bitmap const &g = glyph(cp, size_px, sub);
		for (int yy = 0; yy < g.h; ++yy)
			for (int xx = 0; xx < g.w; ++xx)
			{
				uint8_t const a = g.a[size_t(yy) * g.w + xx];
				if (a) c.blend(ix + g.xoff + xx, by + g.yoff + yy, col, a / 255.f);
			}
		int adv, lsb; stbtt_GetCodepointHMetrics(fi, int(cp), &adv, &lsb);
		x += adv * sc + tracking; prev = cp;
	}
	return wtot;
}

bool load_builtin_fonts(ui_fonts &f)
{
	f.ok = f.regular.load(eiii_font_regular, size_t(eiii_font_regular_end - eiii_font_regular))
		&& f.bold.load(eiii_font_bold, size_t(eiii_font_bold_end - eiii_font_bold))
		&& f.narrow.load(eiii_font_narrow, size_t(eiii_font_narrow_end - eiii_font_narrow));
	return f.ok;
}

} // namespace eiii::ui
