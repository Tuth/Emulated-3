// Minimal software rasterizer for the panel UI: 0x00RRGGBB framebuffer, anti-aliased shapes from signed distance
// functions (one-pixel coverage ramp), src-over blending, TrueType text (stb_truetype) with a glyph cache, PNG output.
// Platform-free; used from one thread (the GUI thread).
#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace eiii::ui {

struct rgba
{
	float r = 0, g = 0, b = 0, a = 1;
	static rgba hex(uint32_t rgb, float a = 1) { return { ((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a }; }
	rgba with_alpha(float na) const { return { r, g, b, na }; }
	rgba scaled(float k) const { return { r * k, g * k, b * k, a }; }
};
inline rgba mix(rgba x, rgba y, float t) { return { x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t }; }

struct recti { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool empty() const { return x1 <= x0 || y1 <= y0; } };
inline recti unite(recti a, recti b) { if (a.empty()) return b; if (b.empty()) return a; return { std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1) }; }

class canvas
{
public:
	int w = 0, h = 0;
	std::vector<uint32_t> px;
	void resize(int nw, int nh) { w = nw; h = nh; px.assign(size_t(w) * h, 0); }
	void clear(uint32_t c) { std::fill(px.begin(), px.end(), c); }
	void blend(int x, int y, rgba c, float cov)
	{
		if (unsigned(x) >= unsigned(w) || unsigned(y) >= unsigned(h)) return;
		float const a = c.a * cov;
		if (a <= 0) return;
		uint32_t &d = px[size_t(y) * w + x];
		float const dr = ((d >> 16) & 255) / 255.f, dg = ((d >> 8) & 255) / 255.f, db = (d & 255) / 255.f;
		auto q = [](float v) { int i = int(v * 255.f + 0.5f); return uint32_t(i < 0 ? 0 : i > 255 ? 255 : i); };
		d = (q(dr + (c.r - dr) * a) << 16) | (q(dg + (c.g - dg) * a) << 8) | q(db + (c.b - db) * a);
	}
	void copy_from(canvas const &s, recti r);              // same size
	bool write_png(std::string const &path) const;

	// generic AA fill: sdf(x, y) in pixels (negative inside), shade(x, y) -> colour; evaluated at pixel centres
	template <class SDF, class SHADE> void fill(float x0, float y0, float x1, float y1, SDF sdf, SHADE shade)
	{
		int const ix0 = std::max(0, int(std::floor(x0 - 1))), iy0 = std::max(0, int(std::floor(y0 - 1)));
		int const ix1 = std::min(w, int(std::ceil(x1 + 1))), iy1 = std::min(h, int(std::ceil(y1 + 1)));
		for (int y = iy0; y < iy1; ++y)
			for (int x = ix0; x < ix1; ++x)
			{
				float const fx = x + 0.5f, fy = y + 0.5f;
				float const d = sdf(fx, fy);
				float const cov = d <= -0.5f ? 1.f : d >= 0.5f ? 0.f : 0.5f - d;
				if (cov > 0) blend(x, y, shade(fx, fy), cov);
			}
	}
};

// signed distance helpers (pixels)
inline float sd_circle(float x, float y, float cx, float cy, float r) { return std::hypot(x - cx, y - cy) - r; }
inline float sd_round_rect(float x, float y, float x0, float y0, float x1, float y1, float r)
{
	float const cx = (x0 + x1) * .5f, cy = (y0 + y1) * .5f, hx = (x1 - x0) * .5f - r, hy = (y1 - y0) * .5f - r;
	float const qx = std::fabs(x - cx) - hx, qy = std::fabs(y - cy) - hy;
	return std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - r;
}
inline float sd_segment(float x, float y, float ax, float ay, float bx, float by, float r)
{
	float const px = x - ax, py = y - ay, vx = bx - ax, vy = by - ay;
	float const l2 = vx * vx + vy * vy;
	float t = l2 > 0 ? (px * vx + py * vy) / l2 : 0; t = t < 0 ? 0 : t > 1 ? 1 : t;
	return std::hypot(px - vx * t, py - vy * t) - r;
}
// convex polygon (counter-clockwise or clockwise; exact inside, edge-distance outside)
float sd_convex(float x, float y, float const *xy, int n);

// shapes with a flat colour
void fill_circle(canvas &c, float cx, float cy, float r, rgba col);
void fill_rect(canvas &c, float x0, float y0, float x1, float y1, rgba col, float radius = 0);
void fill_line(canvas &c, float ax, float ay, float bx, float by, float width, rgba col);
void fill_triangle(canvas &c, float ax, float ay, float bx, float by, float cx, float cy, rgba col);

// TrueType text
class font
{
public:
	bool load(unsigned char const *data, size_t size);
	// draw UTF-8 text; (x, baseline) in pixels; tracking in pixels between glyphs; align: 0 left, 1 centre, 2 right
	float draw(canvas &c, float x, float baseline, std::string_view text, float size_px, rgba col, float tracking = 0, int align = 0);
	float measure(std::string_view text, float size_px, float tracking = 0);
	float cap_height(float size_px) const;
private:
	struct glyph_bitmap { int w = 0, h = 0, xoff = 0, yoff = 0; std::vector<uint8_t> a; };
	glyph_bitmap const &glyph(uint32_t cp, float size_px, int subpx);
	std::shared_ptr<void> m_info;
	unsigned char const *m_data = nullptr;
	std::unordered_map<uint64_t, glyph_bitmap> m_cache;
};

// fonts compiled into the binary (SIL OFL 1.1, third_party/fonts); each view owns its set (no process-wide state)
struct ui_fonts { font regular, bold, narrow; bool ok = false; };
bool load_builtin_fonts(ui_fonts &f);

} // namespace eiii::ui
