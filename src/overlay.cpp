// D5: the wash screen and the keys menu in quads (see overlay.h).

#include "overlay.h"

#include <algorithm>
#include <cmath>

#include "lang.h"
#include "truck_icon.h"

namespace overlay {
namespace {

// 0xAABBGGRR
constexpr uint32_t kText = 0xFFF0F0F0, kBox = 0xC8101010, kKeyText = 0xFF40D0FF, kDim = 0xFFB0B0B0,
                   kSelected = 0xF0804A20, kTitle = 0xFFFFD890;
// D11c: the prompt in the look of other on-foot mods' prompts (the user, 29/10): a dark blue-grey rounded panel at 80% with a faint white outline inside the
// edge, a light keycap with a darker band along its bottom, a lime line, near-white text; Segoe UI as ours.
constexpr uint32_t kPromptPanel = 0xCC120E0C, kPromptOutline = 0x29FFFFFF, kPromptText = 0xFFFFF7F5,
                   kKeyTop = 0xFFF5F2F0, kKeyBand = 0xFFB3A8A3, kKeyLabel = 0xFF1A1614, kLime = 0xE628E6AA;
// D11f: the wash's visit screen in the look of that plugin's visits (shower, meal; the user, 29/10: "the same style"):
// on the black screen a white line icon with lime accents, the title, the price in grey and a lime bar on a light
// track in a dark pill with a faint outline.
constexpr uint32_t kVisitDetail = 0xFFBBB0A9, kPill = 0xFF120E0C, kPillEdge = 0x29FFFFFF, kTrack = 0x29FFFFFF,
                   kLimeSolid = 0xFF28E6AA;

uint32_t WithAlpha(uint32_t rgba, float a) {
    const float k = std::min(std::max(a, 0.f), 1.f);
    const uint32_t alpha = static_cast<uint32_t>(std::lround((rgba >> 24) * k));
    return (rgba & 0x00FFFFFFu) | (alpha << 24);
}

std::vector<char32_t> Codepoints(const std::string& utf8) {
    std::vector<char32_t> out;
    if (lang::DecodeUtf8(utf8, &out)) return out;
    out.clear();
    for (const unsigned char c : utf8) out.push_back(c);  // not UTF-8: Latin-1
    return out;
}

class Painter {
public:
    Painter(Font& f, std::vector<panel::Quad>* out) : font_(f), out_(out) { f.Solid(&su_, &sv_); }

    void Rect(float x0, float y0, float x1, float y1, uint32_t rgba) {
        if ((rgba >> 24) == 0 || x1 <= x0 || y1 <= y0) return;
        out_->push_back({x0, y0, x1, y1, su_, sv_, su_, sv_, rgba});
    }

    // One line from (x, y) = its top-left; returns its width.
    int Text(float x, float y, const std::string& utf8, uint32_t rgba) {
        if ((rgba >> 24) == 0) return 0;
        const float x_start = x;
        x = std::round(x), y = std::round(y);
        for (const char32_t c : Codepoints(utf8)) {
            Glyph g;
            if (!font_.Get(c, &g) && !font_.Get(U'?', &g)) continue;
            if (g.w > 0 && g.h > 0)
                out_->push_back({x + g.x, y + g.y, x + g.x + g.w, y + g.y + g.h, g.u0, g.v0, g.u1, g.v1, rgba});
            x += g.advance;
        }
        return static_cast<int>(x - std::round(x_start));
    }

    int Width(const std::string& utf8) { return TextWidth(font_, utf8); }

    // D11c: a box with rounded corners (radius r), in one-pixel rows at the corners. D11l (N45, the user 30/10: "more
    // quality"): each corner row
    // ends in a partly covered pixel (its exact edge), and the corner's outermost rows by how much of them the arc holds.
    void RoundRect(float x0, float y0, float x1, float y1, float r, uint32_t rgba) {
        r = std::min(r, std::min(x1 - x0, y1 - y0) / 2);
        if (r < 1) return Rect(x0, y0, x1, y1, rgba);
        const int rows = int(std::ceil(r));
        for (int i = 0; i < rows; ++i) {
            // The arc's width over this row, sampled at four heights in it (the row's own coverage).
            const float top = r - float(i), bottom = std::max(r - float(i + 1), 0.f);
            float in = 0.f, cover = 0.f;
            for (int k = 0; k < 4; ++k) {
                const float dy = top - (top - bottom) * (float(k) + 0.5f) / 4.f;
                const float half = std::sqrt(std::max(0.f, r * r - dy * dy));
                in += (r - half) / 4.f;
            }
            cover = std::min(1.f, top - bottom);
            const uint32_t c = WithAlpha(rgba, cover);
            Span(x0 + in, x1 - in, y0 + float(i), y0 + float(i) + 1, c);
            Span(x0 + in, x1 - in, y1 - float(i) - 1, y1 - float(i), c);
        }
        Rect(x0, y0 + float(rows), x1, y1 - float(rows), rgba);
    }
    // D11l: a row from x a to x b, the two end pixels as much as they are covered.
    void Span(float a, float b, float ya, float yb, uint32_t rgba) {
        const float ia = std::ceil(a), ib = std::floor(b);
        if (ib < ia) return Rect(std::floor(a), ya, std::floor(a) + 1, yb, WithAlpha(rgba, b - a));
        Rect(ia, ya, ib, yb, rgba);
        if (ia > a) Rect(ia - 1, ya, ia, yb, WithAlpha(rgba, ia - a));
        if (b > ib) Rect(ib, ya, ib + 1, yb, WithAlpha(rgba, b - ib));
    }
    int Line() const { return font_.LineHeight(); }

private:
    Font& font_;
    std::vector<panel::Quad>* out_;
    float su_ = 0, sv_ = 0;
};

// D11f: the visit's line icon, drawn on a small pixel canvas (the quads are axis-aligned boxes): every stroke keeps
// its coverage per pixel (soft edges, in 4 steps), then each row goes out as runs of the same ink and step, so the
// strokes never overlap (no brighter joins while the screen fades).
class Icon {
public:
    // A canvas of w x h pixels; (ox, oy) = where the icon's (0, 0) falls on it; s = pixels per icon unit.
    Icon(int w, int h, float ox, float oy, float s)
        : w_(w), h_(h), ox_(ox), oy_(oy), s_(s), cover_(size_t(w) * size_t(h), 0.f), ink_(size_t(w) * size_t(h), 0) {}

    // A polyline in icon units, `width` units wide.
    void Stroke(const std::vector<float>& xy, bool closed, float width, uint8_t ink) {
        const size_t n = xy.size() / 2;
        for (size_t i = 0; i + (closed ? 0 : 1) < n; ++i) {
            const size_t j = (i + 1) % n;
            Segment(xy[2 * i], xy[2 * i + 1], xy[2 * j], xy[2 * j + 1], width, ink);
        }
    }
    // D11i: closed polygons in icon units (x y pairs; polygon k is points start[k] .. start[k + 1] - 1), filled with
    // the non-zero rule: 8 sub-rows per pixel row (D11l; 4 before), each span's ends at their exact x.
    static constexpr int kSubRows = 8;
    void Fill(const float* pts, const int* start, int polys, uint8_t ink) {
        std::vector<float> acc(cover_.size(), 0.f);
        struct Cross {
            float x;
            int dir;
        };
        std::vector<Cross> xs;
        for (int r = 0; r < h_; ++r)
            for (int sub = 0; sub < kSubRows; ++sub) {
                const float sy = float(r) + (float(sub) + 0.5f) / float(kSubRows);
                xs.clear();
                for (int k = 0; k < polys; ++k)
                    for (int i = start[k]; i < start[k + 1]; ++i) {
                        const int j = i + 1 < start[k + 1] ? i + 1 : start[k];
                        const float ay = oy_ + pts[2 * i + 1] * s_, by = oy_ + pts[2 * j + 1] * s_;
                        if ((ay <= sy) == (by <= sy)) continue;
                        const float ax = ox_ + pts[2 * i] * s_, bx = ox_ + pts[2 * j] * s_;
                        xs.push_back({ax + (sy - ay) / (by - ay) * (bx - ax), by > ay ? 1 : -1});
                    }
                std::sort(xs.begin(), xs.end(), [](const Cross& a, const Cross& b) { return a.x < b.x; });
                int wind = 0;
                for (size_t q = 0; q + 1 < xs.size(); ++q) {
                    wind += xs[q].dir;
                    if (wind != 0) Span(&acc, r, xs[q].x, xs[q + 1].x);
                }
            }
        for (size_t i = 0; i < acc.size(); ++i) {
            const float cov = std::min(acc[i], 1.f);
            if (cov > cover_[i]) cover_[i] = cov, ink_[i] = ink;
        }
    }
    void Ring(float cx, float cy, float r, float width, uint8_t ink) {
        const float half = width * s_ / 2, rp = r * s_, x = ox_ + cx * s_, y = oy_ + cy * s_;
        Cover(x - rp - half, y - rp - half, x + rp + half, y + rp + half, ink, [&](float px, float py) {
            return std::fabs(std::hypot(px - x, py - y) - rp) - half;
        });
    }
    // A drop (its tip at the top), as the SVG's "q": two quadratic curves.
    void Drop(float x, float top, float h, float width, uint8_t ink) {
        const float b = h * 5.f / 12.f, cy = top + h * 2.f / 3.f;
        std::vector<float> xy;
        for (int side = 0; side < 2; ++side)
            for (int k = 0; k < 8; ++k) {
                const float t = float(k) / 8.f, u = 1.f - t;
                const float y0 = side == 0 ? top : top + h, y1 = side == 0 ? top + h : top;
                const float cx = side == 0 ? x + b : x - b;
                xy.push_back(u * u * x + 2 * u * t * cx + t * t * x);
                xy.push_back(u * u * y0 + 2 * u * t * cy + t * t * y1);
            }
        Stroke(xy, true, width, ink);
    }

    // The canvas in quads at (x, y) on the screen; inks[k] = the colour of ink k (its alpha as wanted).
    void Emit(Painter& p, float x, float y, const uint32_t* inks) const {
        for (int r = 0; r < h_; ++r) {
            int c = 0;
            while (c < w_) {
                const size_t i = size_t(r) * size_t(w_) + size_t(c);
                const int level = Level(i);
                if (level == 0) {
                    ++c;
                    continue;
                }
                int e = c + 1;
                while (e < w_ && ink_[i + size_t(e - c)] == ink_[i] && Level(i + size_t(e - c)) == level) ++e;
                p.Rect(x + float(c), y + float(r), x + float(e), y + float(r + 1),
                       WithAlpha(inks[ink_[i]], float(level) / float(kLevels)));
                c = e;
            }
        }
    }

private:
    // A sub-row's span from x0 to x1 into `acc`, by how much of each pixel it covers.
    void Span(std::vector<float>* acc, int r, float x0, float x1) const {
        x0 = std::max(x0, 0.f), x1 = std::min(x1, float(w_));
        if (x1 <= x0) return;
        float* row = acc->data() + size_t(r) * size_t(w_);
        const int c0 = int(x0), c1 = std::min(int(x1), w_ - 1);
        const float k = 1.f / float(kSubRows);
        if (c0 == c1) {
            row[c0] += (x1 - x0) * k;
            return;
        }
        row[c0] += (float(c0 + 1) - x0) * k;
        for (int c = c0 + 1; c < c1; ++c) row[c] += k;
        row[c1] += (x1 - float(c1)) * k;
    }
    // D11l: 16 steps of coverage (4 before: the user saw the icon "with little resolution").
    static constexpr int kLevels = 16;
    int Level(size_t i) const { return ink_[i] == 0 ? 0 : static_cast<int>(std::lround(cover_[i] * float(kLevels))); }

    void Segment(float ax, float ay, float bx, float by, float width, uint8_t ink) {
        ax = ox_ + ax * s_, ay = oy_ + ay * s_, bx = ox_ + bx * s_, by = oy_ + by * s_;
        const float half = width * s_ / 2, dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        Cover(std::min(ax, bx) - half, std::min(ay, by) - half, std::max(ax, bx) + half, std::max(ay, by) + half, ink,
              [&](float px, float py) {
                  const float t = len2 > 0 ? std::min(std::max(((px - ax) * dx + (py - ay) * dy) / len2, 0.f), 1.f) : 0.f;
                  return std::hypot(px - (ax + t * dx), py - (ay + t * dy)) - half;
              });
    }
    // Each pixel of the box: `outside`(its centre) = how far out of the shape, in pixels.
    template <class F>
    void Cover(float x0, float y0, float x1, float y1, uint8_t ink, F outside) {
        const int c0 = std::max(0, int(std::floor(x0 - 1))), c1 = std::min(w_ - 1, int(std::ceil(x1 + 1)));
        const int r0 = std::max(0, int(std::floor(y0 - 1))), r1 = std::min(h_ - 1, int(std::ceil(y1 + 1)));
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) {
                const float cov = std::min(std::max(0.5f - outside(float(c) + 0.5f, float(r) + 0.5f), 0.f), 1.f);
                const size_t i = size_t(r) * size_t(w_) + size_t(c);
                if (cov > cover_[i]) cover_[i] = cov, ink_[i] = ink;
            }
    }

    int w_, h_;
    float ox_, oy_, s_;
    std::vector<float> cover_;
    std::vector<uint8_t> ink_;
};

float Smooth(float e0, float e1, float x) {
    const float t = std::min(std::max((x - e0) / (e1 - e0), 0.f), 1.f);
    return t * t * (3 - 2 * t);
}

// D11f: the visit: the truck in white lines, three lime drops falling on it, the title, the price, the bar.
void LayoutVisit(const Wash& w, Painter& p, float W, float H, float line) {
    const float show = std::min(std::max(w.show, 0.f), 1.f);
    const float a = std::min(std::max(w.darkness, 0.f), 1.f) * std::min(1.f, show / 0.06f);  // in over ~0.3 s
    if (a <= 0.f) return;
    // D11i (the user, 30/10: a truck more like the games'): Phosphor's truck-trailer (src/truck_icon.h, the SVG's units
    // 0..256: the trailer left, the tractor right, its cab from x 146 to 222 and top at y 42), ~8 lines wide; the
    // drops above the cab and the hood.
    const float s = line / 22.f, stroke = 7.3f;  // D11l: ~11.5 lines wide (8 before)
    const float u0 = -4, u1 = 260, v0 = -26, v1 = 218;
    const int iw = int(std::ceil((u1 - u0) * s)) + 2, ih = int(std::ceil((v1 - v0) * s)) + 2;
    Icon icon(iw, ih, 1 - u0 * s, 1 - v0 * s, s);
    icon.Fill(truck_icon::kPoints, truck_icon::kStart, truck_icon::kPolys, 1);
    // Each drop falls 37 units in a cycle of 2 s (5 s of animation), one after the other, in and out softly.
    const float drops[3][3] = {{188, -14, 27}, {236, 30, 23}, {164, -22, 23}};
    uint32_t inks[5] = {0, WithAlpha(kPromptText, a), 0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        float t = show * 2.5f + float(i) / 3.f;
        t -= std::floor(t);
        icon.Drop(drops[i][0], drops[i][1] + 37 * t, drops[i][2], stroke, uint8_t(2 + i));
        inks[2 + i] = WithAlpha(kLimeSolid, a * Smooth(0.f, 0.2f, t) * (1 - Smooth(0.6f, 1.f, t)));
    }
    const float tw = float(p.Width(w.washing_text)), dw = float(p.Width(w.washing_detail));
    const float pill_h = std::round(line * 0.76f), pill_w = std::round(std::max(tw, line * 7.f) + line);
    const float title_gap = std::round(line * 0.5f), detail_h = dw > 0 ? std::round(line * 1.15f) : 0.f;
    const float pill_gap = std::round(line * 0.55f);
    const float block = float(ih) + title_gap + line + detail_h + pill_gap + pill_h;
    float y = std::round(H * 0.47f - block / 2);
    icon.Emit(p, std::round((W - float(iw)) / 2), y, inks);
    y += float(ih) + title_gap;
    p.Text(std::round((W - tw) / 2), y, w.washing_text, WithAlpha(kPromptText, a));
    if (dw > 0) p.Text(std::round((W - dw) / 2), y + detail_h, w.washing_detail, WithAlpha(kVisitDetail, a));
    y += line + detail_h + pill_gap;
    // The pill: its faint outline, the dark inside, the light track and the lime bar.
    const float px = std::round((W - pill_w) / 2), o = std::max(1.f, std::round(line / 28));
    p.RoundRect(px, y, px + pill_w, y + pill_h, pill_h / 2, WithAlpha(kPillEdge, a));
    p.RoundRect(px + o, y + o, px + pill_w - o, y + pill_h - o, pill_h / 2 - o, WithAlpha(kPill, a));
    const float inset = std::round(pill_h * 0.31f), th = pill_h - 2 * inset;
    const float tx = px + inset, tx1 = px + pill_w - inset, ty = y + inset;
    p.RoundRect(tx, ty, tx1, ty + th, th / 2, WithAlpha(kTrack, a));
    const float fill = std::round((tx1 - tx) * show);
    if (fill >= 1) p.RoundRect(tx, ty, tx + fill, ty + th, th / 2, WithAlpha(kLimeSolid, a));
}

void LayoutWash(const Wash& w, Painter& p, int width, int height) {
    const float W = float(width), H = float(height), line = float(p.Line());
    if (w.darkness > 0.f) p.Rect(0, 0, W, H, WithAlpha(0xFF000000, w.darkness));
    if (w.washing && w.darkness > 0.f) LayoutVisit(w, p, W, H, line);
    if (!w.message.empty()) {
        // D11c: the prompt, centred, its bottom edge at w.y of the screen: above the bottom prompt line other mods use,
        // so the two never overlap. [key] message  detail.
        const float pad = std::round(line * 0.55f), gap = std::round(line * 0.45f);
        const float key_h = std::round(line * 1.2f);
        const float key_w = w.key.empty() ? 0.f : std::max(key_h, float(p.Width(w.key)) + std::round(line * 0.7f));
        const float tw = float(p.Width(w.message)), dw = w.detail.empty() ? 0.f : float(p.Width(w.detail));
        const float inner = (key_w > 0 ? key_w + gap : 0.f) + tw + (dw > 0 ? gap + dw : 0.f);
        const float box_h = std::round(std::max(key_h, line) + pad * 1.2f), box_w = std::round(inner + 2 * pad);
        const float y_frac = std::min(std::max(w.y, 0.2f), 0.98f);
        const float bx = std::round((W - box_w) / 2), by = std::round(H * y_frac - box_h);
        const float radius = std::round(line * 0.3f);
        p.RoundRect(bx, by, bx + box_w, by + box_h, radius, kPromptPanel);
        // The faint outline just inside the edge, and the lime line along the bottom.
        const float o = std::max(1.f, std::round(line / 28));
        p.Rect(bx + radius, by + o, bx + box_w - radius, by + 2 * o, kPromptOutline);
        p.Rect(bx + o, by + radius, bx + 2 * o, by + box_h - radius, kPromptOutline);
        p.Rect(bx + box_w - 2 * o, by + radius, bx + box_w - o, by + box_h - radius, kPromptOutline);
        const float lime_h = std::max(2.f, std::round(line / 12));
        p.Rect(bx + radius, by + box_h - lime_h - o, bx + box_w - radius, by + box_h - o, kLime);
        float x = bx + pad;
        const float mid = by + std::round((box_h - lime_h) / 2);
        if (key_w > 0) {
            const float ky = std::round(mid - key_h / 2), kr = std::round(line * 0.22f);
            p.RoundRect(x, ky, x + key_w, ky + key_h, kr, kKeyBand);
            p.RoundRect(x, ky, x + key_w, ky + std::round(key_h * 0.86f), kr, kKeyTop);
            p.Text(std::round(x + (key_w - float(p.Width(w.key))) / 2), std::round(ky + (key_h * 0.86f - line) / 2), w.key,
                   kKeyLabel);
            x += key_w + gap;
        }
        const float ty = std::round(mid - line / 2);
        p.Text(x, ty, w.message, kPromptText);
        if (dw > 0) p.Text(x + tw + gap, ty, w.detail, kLime | 0xFF000000u);
    }
}

void LayoutMenu(const Menu& m, Painter& p, int width, int height) {
    const float line = float(p.Line()), pad = std::round(line * 0.8f), gap = std::round(line * 1.5f);
    float label_w = 0, value_w = 0, extra_w = float(p.Width(m.title));
    for (const MenuRow& r : m.rows) {
        label_w = std::max(label_w, float(p.Width(r.label)));
        value_w = std::max(value_w, float(p.Width(r.value)));
    }
    for (const std::string& h : m.help) extra_w = std::max(extra_w, float(p.Width(h)));
    extra_w = std::max(extra_w, float(p.Width(m.status)));
    const float inner = std::max(label_w + gap + value_w, extra_w);
    const float row_h = std::round(line * 1.3f);
    const float box_w = inner + 2 * pad;
    const float box_h = pad + row_h * 1.5f + row_h * float(m.rows.size()) + row_h * 0.5f +
                        (m.status.empty() ? 0.f : row_h) + line * float(m.help.size()) + pad;
    const float x0 = std::round((float(width) - box_w) / 2), y0 = std::round((float(height) - box_h) / 2.5f);
    p.Rect(x0, y0, x0 + box_w, y0 + box_h, kBox);
    float y = y0 + pad;
    p.Text(x0 + pad, y, m.title, kTitle);
    y += row_h * 1.5f;
    for (const MenuRow& r : m.rows) {
        if (r.selected) p.Rect(x0 + pad / 2, y - (row_h - line) / 2, x0 + box_w - pad / 2, y + line + (row_h - line) / 2, kSelected);
        p.Text(x0 + pad, y, r.label, r.selected ? kText : kDim);
        const float vw = float(p.Width(r.value));
        p.Text(x0 + box_w - pad - vw, y, r.value, r.editing ? kTitle : kKeyText);
        y += row_h;
    }
    y += row_h * 0.5f;
    if (!m.status.empty()) {
        p.Text(x0 + pad, y, m.status, kTitle);
        y += row_h;
    }
    for (const std::string& h : m.help) {
        p.Text(x0 + pad, y, h, kDim);
        y += line;
    }
}

}  // namespace

// D11f (the user, 29/10: the prompt "too big"): ~21 px at 1080p (was 28). D11g (30/10: "still too big, half the
// size"): 14 px at 1440p (the user's 3440x1440; was 27). D11h (30/10: "the size must follow each player's screen, as
// the game's own notice"): the game's notice is ~14 px at 1440p (measured in the user's print), so the same share of
// the height on every screen (11 px at 1080p, 21 at 4K); the floor only for tiny windows.
int FontPx(int screen_h) { return std::max(9, static_cast<int>(std::lround(screen_h * 14.0 / 1440.0))); }

int TextWidth(Font& f, const std::string& utf8) {
    int w = 0;
    for (const char32_t c : Codepoints(utf8)) {
        Glyph g;
        if (f.Get(c, &g) || f.Get(U'?', &g)) w += g.advance;
    }
    return w;
}

std::vector<panel::Quad> Layout(const View& v, Font& f, int width, int height) {
    std::vector<panel::Quad> out;
    if (width <= 0 || height <= 0) return out;
    Painter p(f, &out);
    LayoutWash(v.wash, p, width, height);
    if (v.menu.open) LayoutMenu(v.menu, p, width, height);
    return out;
}

}  // namespace overlay
