// N10: the test panel's state, keys and layout (see test_panel.h).

#include "test_panel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace panel {
namespace {

#include "generated/font8x8.inc"

// The layer that one turns into (as in dirt_model.cpp); kLayers = none.
dirt::Layer Partner(dirt::Layer l) {
    switch (l) {
        case dirt::kWetMud: return dirt::kDryMud;
        case dirt::kDryMud: return dirt::kWetMud;
        case dirt::kSpray: return dirt::kFilm;
        case dirt::kFilm: return dirt::kSpray;
        default: return dirt::kLayers;
    }
}

// Values on the 0.01 grid, so that steps do not drift.
float Round2(float v) { return std::round(std::clamp(v, 0.f, 1.f) * 100.f) / 100.f; }

// The next value on the grid of `step` from `base`, up (step > 0) or down.
float Snap(float base, float step) {
    const float s = std::fabs(step);
    const float v = step > 0 ? std::floor(base / s + 1e-3f) * s + s : std::ceil(base / s - 1e-3f) * s - s;
    return Round2(v);
}

bool Same(const dirt::Forced& a, const dirt::Forced& b) {
    for (int u = 0; u < dirt::kUnits; ++u) {
        if (a.reach[u] != b.reach[u]) return false;
        for (int k = 0; k < dirt::kLayers; ++k)
            if (a.level[u][k] != b.level[u][k]) return false;
    }
    return true;
}

std::string Num(float v) {
    char b[16];
    snprintf(b, sizeof(b), "%.2f", v);
    return b;
}

// One value or "truck/trailer" when they differ.
std::string Pair(float a, float b, bool both) {
    if (!both || std::fabs(a - b) < 0.005f) return Num(a);
    return Num(a) + "/" + Num(b);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct VkName {
    const char* name;
    int vk;
};
const VkName kVkNames[] = {{"home", 0x24},     {"end", 0x23},      {"insert", 0x2D},    {"ins", 0x2D},
                           {"delete", 0x2E},   {"del", 0x2E},      {"pageup", 0x21},    {"pgup", 0x21},
                           {"pagedown", 0x22}, {"pgdn", 0x22},     {"pause", 0x13},     {"scrolllock", 0x91},
                           {"space", 0x20},    {"backspace", 0x08}, {"tab", 0x09},       {"enter", 0x0D}};

}  // namespace

const int kNavKeys[8] = {kVkUp, kVkDown, kVkLeft, kVkRight, kVkHome, kVkEnd, kVkDelete, kVkReturn};

// ---- panel ----

void Panel::Enable(bool on, const std::string& key_name) {
    enabled_ = on;
    key_name_ = key_name;
    if (!on) open_ = false;
}

void Panel::SetLayer(int unit, dirt::Layer l, float v, const dirt::Coat& shown) {
    v = Round2(v);
    forced_.level[unit][l] = v;
    const dirt::Layer p = Partner(l);
    if (p == dirt::kLayers) return;
    const float pv = forced_.level[unit][p] >= 0.f ? forced_.level[unit][p] : shown.body[p];
    if (v + pv > 1.f + 1e-4f) forced_.level[unit][p] = Round2(1.f - v);
}

void Panel::Move(float step, const dirt::Coat shown[dirt::kUnits]) {
    const int first = target_ == kTrailer ? 1 : 0;
    const int last = target_ == kTruck ? 0 : 1;
    if (row_ == kRowReach) {
        const float base = forced_.reach[first];
        const float v = base >= 0.f ? Snap(base, step) : 0.5f;  // from "auto": the middle
        for (int u = first; u <= last; ++u) forced_.reach[u] = v;
        return;
    }
    const auto l = static_cast<dirt::Layer>(row_ - kRowLayer);
    const float v = Snap(shown[first].body[l], step);
    for (int u = first; u <= last; ++u) SetLayer(u, l, v, shown[u]);
}

void Panel::SetValue(float v, const dirt::Coat shown[dirt::kUnits]) {
    const int first = target_ == kTrailer ? 1 : 0;
    const int last = target_ == kTruck ? 0 : 1;
    for (int u = first; u <= last; ++u) {
        if (row_ == kRowReach) forced_.reach[u] = v;
        else SetLayer(u, static_cast<dirt::Layer>(row_ - kRowLayer), v, shown[u]);
    }
}

void Panel::RowReal() {
    const int first = target_ == kTrailer ? 1 : 0;
    const int last = target_ == kTruck ? 0 : 1;
    for (int u = first; u <= last; ++u) {
        if (row_ == kRowReach) forced_.reach[u] = -1.f;
        else forced_.level[u][row_ - kRowLayer] = -1.f;
    }
}

bool Panel::Press(Key k, const dirt::Coat shown[dirt::kUnits]) {
    if (!enabled_) return false;
    if (k == kToggle) {
        open_ = !open_;
        return false;
    }
    if (!open_) return false;
    const dirt::Forced before = forced_;
    const bool value_row = row_ >= kRowLayer && row_ <= kRowReach;
    switch (k) {
        case kUp: row_ = (row_ + kRows - 1) % kRows; break;
        case kDown: row_ = (row_ + 1) % kRows; break;
        case kLeft:
        case kRight:
        case kFineLeft:
        case kFineRight: {
            const bool up = k == kRight || k == kFineRight;
            if (row_ == kRowTarget) target_ = static_cast<Target>((target_ + (up ? 1 : kTargets - 1)) % kTargets);
            else if (value_row) Move((k == kLeft || k == kRight ? kStep : kFineStep) * (up ? 1.f : -1.f), shown);
            break;
        }
        case kZero:
        case kOne:
            if (value_row) SetValue(k == kOne ? 1.f : 0.f, shown);
            break;
        case kReal:
            if (value_row) RowReal();
            break;
        case kEnter:
            if (row_ == kRowTarget) target_ = static_cast<Target>((target_ + 1) % kTargets);
            else if (row_ == kRowAllReal) forced_ = dirt::Forced{};
            else if (row_ == kRowCleanAll)
                for (auto& unit : forced_.level)
                    for (float& l : unit) l = 0.f;
            break;
        default: break;
    }
    return !Same(before, forced_);
}

View Panel::MakeView(const dirt::Coat shown[dirt::kUnits]) const {
    View v;
    v.open = enabled_ && open_;
    v.badge = enabled_ && !open_ && forced_.Any();
    if (v.badge) v.title = "DIRT TEST: forced levels on (" + key_name_ + ": panel)";
    if (!v.open) return v;
    v.title = "TRUCK DIRT - TEST PANEL";
    const int first = target_ == kTrailer ? 1 : 0;
    const int last = target_ == kTruck ? 0 : 1;
    const bool both = first != last;
    static const char* const kTargetName[] = {"truck", "trailer", "truck + trailer"};
    v.rows.push_back({"unit", kTargetName[target_], -1.f, false, false});
    for (int k = 0; k < dirt::kLayers; ++k) {
        ViewRow r;
        r.label = dirt::LayerName(static_cast<dirt::Layer>(k));
        std::replace(r.label.begin(), r.label.end(), '_', ' ');
        r.value = Pair(shown[first].body[k], shown[last].body[k], both);
        r.bar = shown[first].body[k];
        r.forced = forced_.level[first][k] >= 0.f || forced_.level[last][k] >= 0.f;
        v.rows.push_back(r);
    }
    {
        ViewRow r;
        r.label = "reach";
        r.forced = forced_.reach[first] >= 0.f || forced_.reach[last] >= 0.f;
        r.value = r.forced ? Pair((std::max)(0.f, forced_.reach[first]), (std::max)(0.f, forced_.reach[last]), both)
                           : "auto";
        if (r.forced) r.bar = (std::max)(0.f, forced_.reach[first]);
        v.rows.push_back(r);
    }
    v.rows.push_back({"all real", "", -1.f, false, false});
    v.rows.push_back({"clean all", "", -1.f, false, false});
    v.rows[row_].selected = true;
    v.help = {"ctrl+up/down: row  ctrl+left/right: -/+0.1", "ctrl+shift+left/right: -/+0.01",
              "ctrl+home/end: 0/1  ctrl+del: real", "ctrl+enter: unit, all real, clean all",
              key_name_ + ": close (the forced levels stay)"};
    return v;
}

std::string LearningNotice(bool truck, bool trailer) {
    if (!truck && !trailer) return "";
    const char* unit = truck && trailer ? "truck and trailer" : truck ? "truck" : "trailer";
    return std::string("drive ~1 m: the ") + unit + " is known by its wheels only";
}

// ---- keys ----

bool ParseHotkey(const std::string& text, Hotkey* out) {
    Hotkey h;
    std::string s = Lower(text);
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
    if (s.empty()) return false;
    size_t start = 0;
    while (true) {
        const size_t plus = s.find('+', start);
        const std::string part = s.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (plus == std::string::npos) {  // the key itself
            if (h.vk) return false;
            if (part.size() == 1 && std::isalnum(static_cast<unsigned char>(part[0]))) {
                h.vk = std::toupper(static_cast<unsigned char>(part[0]));
            } else if (part.size() >= 2 && part.size() <= 3 && part[0] == 'f' &&
                       std::all_of(part.begin() + 1, part.end(), [](unsigned char c) { return std::isdigit(c); })) {
                const int n = std::atoi(part.c_str() + 1);
                if (n < 1 || n > 24) return false;
                h.vk = 0x70 + n - 1;
            } else if (part.size() == 7 && part.compare(0, 6, "numpad") == 0 && std::isdigit(static_cast<unsigned char>(part[6]))) {
                h.vk = 0x60 + (part[6] - '0');
            } else {
                for (const VkName& n : kVkNames)
                    if (part == n.name) h.vk = n.vk;
                if (!h.vk) return false;
            }
            break;
        }
        if (part == "ctrl" || part == "control") h.ctrl = true;
        else if (part == "shift") h.shift = true;
        else if (part == "alt") h.alt = true;
        else return false;
        start = plus + 1;
    }
    *out = h;
    return true;
}

std::string HotkeyName(const Hotkey& h) {
    std::string s;
    if (h.ctrl) s += "ctrl+";
    if (h.shift) s += "shift+";
    if (h.alt) s += "alt+";
    if (h.vk >= 0x70 && h.vk <= 0x87) return s + "f" + std::to_string(h.vk - 0x70 + 1);
    if (h.vk >= 0x60 && h.vk <= 0x69) return s + "numpad" + std::to_string(h.vk - 0x60);
    if ((h.vk >= 'A' && h.vk <= 'Z') || (h.vk >= '0' && h.vk <= '9')) return s + char(std::tolower(h.vk));
    for (const VkName& n : kVkNames)
        if (n.vk == h.vk) return s + n.name;
    return s + "?";
}

std::string HotkeyLabel(const Hotkey& h) {
    std::string s = HotkeyName(h), out;
    bool start = true;
    for (const char c : s) {
        out += start ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        start = c == '+';
    }
    // Two words in one name: "Pageup" -> "PageUp"
    const struct {
        const char* from;
        const char* to;
    } kWords[] = {{"Pageup", "PageUp"}, {"Pagedown", "PageDown"}, {"Scrolllock", "ScrollLock"}};
    for (const auto& w : kWords) {
        const size_t at = out.find(w.from);
        if (at != std::string::npos) out.replace(at, strlen(w.from), w.to);
    }
    return out;
}

Key KeyOf(int vk, bool ctrl, bool shift, bool alt) {
    if (!ctrl || alt) return kNone;
    switch (vk) {
        case kVkUp: return kUp;
        case kVkDown: return kDown;
        case kVkLeft: return shift ? kFineLeft : kLeft;
        case kVkRight: return shift ? kFineRight : kRight;
        case kVkHome: return kZero;
        case kVkEnd: return kOne;
        case kVkDelete: return kReal;
        case kVkReturn: return kEnter;
        default: return kNone;
    }
}

bool IsHotkey(const Hotkey& h, int vk, bool ctrl, bool shift, bool alt) {
    return h.vk && vk == h.vk && ctrl == h.ctrl && shift == h.shift && alt == h.alt;
}

int Repeat::Step(bool down, uint64_t now_ms) {
    constexpr uint64_t kDelayMs = 400, kEveryMs = 60;
    if (!down) {
        down_ = false;
        return 0;
    }
    if (!down_) {
        down_ = true;
        next_ = now_ms + kDelayMs;
        return 1;
    }
    if (now_ms < next_) return 0;
    next_ = now_ms + kEveryMs;  // one press per poll at most: a slow frame does not jump
    return 1;
}

// ---- layout ----

void FontAtlas(uint8_t out[kAtlasW * kAtlasH]) {
    for (int i = 0; i < 96; ++i) {
        const int gx = (i % kAtlasCols) * kGlyph, gy = (i / kAtlasCols) * kGlyph;
        for (int y = 0; y < kGlyph; ++y)
            for (int x = 0; x < kGlyph; ++x)
                out[(gy + y) * kAtlasW + gx + x] = (kFont8x8[i][y] >> x) & 1 ? 255 : 0;
    }
}

int ScaleFor(int height) { return (std::max)(1, height / 540); }

namespace {

// Colours, 0xAABBGGRR.
constexpr uint32_t kBack = 0xD0181410, kSelected = 0xF0804A20, kText = 0xFFF0F0F0, kDim = 0xFFA0A0A0,
                   kTitle = 0xFFFFD890, kForced = 0xFF40D0FF, kBarBack = 0xFF404040, kBarFill = 0xFF3A8CE0,
                   kNotice = 0xFF5070FF;  // N12: orange-red

struct Painter {
    std::vector<Quad>* out;
    float cell;  // one glyph, in pixels

    void Box(float x0, float y0, float x1, float y1, uint32_t c) const {
        // The middle of the solid glyph (127): every sample is opaque.
        const float u = (15 * kGlyph + kGlyph / 2) / float(kAtlasW), v = (5 * kGlyph + kGlyph / 2) / float(kAtlasH);
        out->push_back({x0, y0, x1, y1, u, v, u, v, c});
    }
    void Text(float x, float y, const std::string& s, uint32_t c) const {
        for (char ch : s) {
            int i = static_cast<unsigned char>(ch) - 32;
            if (i < 0 || i >= 95) i = '?' - 32;
            if (i != 0) {  // spaces: nothing to draw
                const float u0 = (i % kAtlasCols) * kGlyph / float(kAtlasW), v0 = (i / kAtlasCols) * kGlyph / float(kAtlasH);
                out->push_back({x, y, x + cell, y + cell, u0, v0, u0 + kGlyph / float(kAtlasW),
                                v0 + kGlyph / float(kAtlasH), c});
            }
            x += cell;
        }
    }
};

}  // namespace

std::vector<Quad> Layout(const View& v, int width, int height) {
    std::vector<Quad> q;
    if (!v.open && !v.badge) return q;
    const float cell = float(kGlyph * ScaleFor(height));
    const Painter p{&q, cell};
    const float x0 = std::round((std::max)(cell, width * 0.02f)), y0 = std::round(height * 0.12f);
    if (!v.open) {  // the badge: one line (two with a notice)
        const float lines = v.notice.empty() ? 1.f : 2.f;
        p.Box(x0, y0, x0 + float((std::max)(v.title.size(), v.notice.size()) + 2) * cell, y0 + (lines + 1) * cell, kBack);
        p.Text(x0 + cell, y0 + cell / 2, v.title, kForced);
        if (!v.notice.empty()) p.Text(x0 + cell, y0 + cell * 1.5f, v.notice, kNotice);
        return q;
    }
    // Columns, in cells: label, bar, value.
    constexpr int kLabelCols = 12, kBarCols = 14, kValueCols = 11;
    size_t cols = kLabelCols + kBarCols + 1 + kValueCols;
    for (const std::string& h : v.help) cols = (std::max)(cols, h.size());
    cols = (std::max)(cols, (std::max)(v.title.size(), v.notice.size()));
    const float line = std::round(cell * 1.5f);
    const float w = float(cols + 2) * cell;
    const size_t notice = v.notice.empty() ? 0 : 1;
    const float h = line * float(v.rows.size() + v.help.size() + 3 + notice) + cell;
    p.Box(x0, y0, x0 + w, y0 + h, kBack);
    float y = y0 + cell;
    p.Text(x0 + cell, y, v.title, kTitle);
    if (notice) {
        y += line;
        p.Text(x0 + cell, y, v.notice, kNotice);
    }
    y += line * 2;
    for (const ViewRow& r : v.rows) {
        if (r.selected) p.Box(x0 + cell / 2, y - (line - cell) / 2, x0 + w - cell / 2, y + cell + (line - cell) / 2, kSelected);
        const float tx = x0 + cell;
        p.Text(tx, y, (r.selected ? "> " : "  ") + r.label, kText);
        if (r.bar >= 0.f) {
            const float bx = tx + kLabelCols * cell, bw = (kBarCols - 1) * cell;
            const float pad = std::round(cell / 8);
            p.Box(bx, y + pad, bx + bw, y + cell - pad, kBarBack);
            const float fill = std::round(bw * std::clamp(r.bar, 0.f, 1.f));
            if (fill > 0) p.Box(bx, y + pad, bx + fill, y + cell - pad, r.forced ? kForced : kBarFill);
        }
        p.Text(tx + (kLabelCols + kBarCols) * cell, y, r.value + (r.forced ? " *" : ""), r.forced ? kForced : kText);
        y += line;
    }
    y += line - cell;
    for (const std::string& s : v.help) {
        p.Text(x0 + cell, y, s, kDim);
        y += line;
    }
    return q;
}

}  // namespace panel
