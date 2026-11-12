// Offline test of the N10 test panel (src/test_panel) and of the forced levels
// of the dirt model (dirt::Model::Force): navigation, the 0..1 limits, the
// steps, "real", "all real", "clean all", the mud and spray pairs, the hotkey
// text of the INI, the key repeat, the font and the layout.
// Usage: paneltest

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dirt_model.h"
#include "menu.h"
#include "overlay.h"
#include "test_panel.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what, double value = 0) {
    printf("%s %-62s %.3f\n", ok ? "ok  " : "FAIL", what, value);
    if (!ok) ++g_failures;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// A panel with its own model: every key goes through the shown layers, as in the plugin.
struct Rig {
    dirt::Model model;
    panel::Panel p;
    Rig() { p.Enable(true); }
    bool Press(panel::Key k, int times = 1) {
        bool changed = false;
        for (int i = 0; i < times; ++i) {
            dirt::Coat shown[dirt::kUnits] = {model.Layers(0), model.Layers(1)};
            if (p.Press(k, shown)) {
                model.Force(p.Forced());
                changed = true;
            }
        }
        return changed;
    }
    // Moves the selection to a row (from anywhere: Up wraps).
    void Go(int row) {
        while (p.Selected() != row) Press(panel::kDown);
    }
    void Target(panel::Target t) {
        Go(panel::kRowTarget);
        while (p.GetTarget() != t) Press(panel::kRight);
    }
    float Body(int unit, dirt::Layer l) const { return model.Layers(unit).body[l]; }
    panel::View View() const {
        dirt::Coat shown[dirt::kUnits] = {model.Layers(0), model.Layers(1)};
        return p.MakeView(shown);
    }
};

int LayerRow(dirt::Layer l) { return panel::kRowLayer + l; }

// Some real dirt: 2 km of wet dirt after rain, truck and trailer.
void MakeDirty(dirt::Model& m) {
    m.Weather(60, true);
    dirt::Ground g[6];
    bool og[6];
    for (int w = 0; w < 6; ++w) g[w] = dirt::kDirt, og[w] = true;
    for (int i = 0; i < 400; ++i) {  // 40 km (D14: the mud 20x slower per km of the odometer)
        m.Drive(0, 0.1, g, og, 6, 50.f);
        m.Drive(1, 0.1, g, og, 6, 50.f);
    }
}

void ModelForce() {
    using namespace dirt;
    {
        Model m;
        MakeDirty(m);
        const Coat before = m.Layers(0);
        Check(!Forced{}.Any() && m.Layers(0).body[kWetMud] == before.body[kWetMud],
              "model: nothing forced by default", before.body[kWetMud]);
        Forced f;
        f.level[0][kWetMud] = 0.2f;
        f.level[1][kSalt] = 0.9f;
        f.level[1][kDust] = 0.6f;
        f.reach[1] = 0.75f;
        m.Force(f);
        Check(f.Any() && Near(m.Layers(0).body[kWetMud], 0.2f), "model: a forced layer is shown", m.Layers(0).body[kWetMud]);
        Check(Near(m.Layers(1).body[kWetMud], before.body[kWetMud]) || m.Layers(1).body[kWetMud] > 0.f,
              "model: ... only on its unit (the trailer keeps its mud)", m.Layers(1).body[kWetMud]);
        Check(Near(m.Layers(1).front[kSalt], 0.9f) && Near(m.Layers(1).body[kSalt], 0.9f),
              "model: a forced trailer layer covers its front too", m.Layers(1).front[kSalt]);
        Check(Near(m.Level(1), 0.6f) && Near(m.Layers(1).body[kDust], 0.6f) && !Near(m.Level(0), 0.6f),
              "model: forced dust, one unit", m.Level(1));
        Check(Near(m.Layers(1).reach[kSalt], 0.75f) && !Near(m.Layers(1).reach[kWetMud], 0.75f),
              "model: forced reach, only on the forced layers", m.Layers(1).reach[kSalt]);
        Check(Near(m.Real(0).body[kWetMud], before.body[kWetMud]) && m.Real(1).body[kSalt] == 0.f,
              "model: Real() is the simulation, without the forced levels", m.Real(0).body[kWetMud]);
        // The simulation runs on underneath: the time dries the real mud.
        m.Weather(120, false);
        Check(Near(m.Layers(0).body[kWetMud], 0.2f) && m.Real(0).body[kWetMud] < before.body[kWetMud] * 0.5f,
              "model: the simulation keeps running under a forced layer", m.Real(0).body[kWetMud]);
        m.Force(Forced{});
        Check(Near(m.Layers(0).body[kWetMud], m.Real(0).body[kWetMud]) && Near(m.Level(1), m.Real(1).body[kDust]),
              "model: Forced{} gives the real levels back", m.Layers(0).body[kWetMud]);
    }
    {
        // The forced levels win over the INI overrides.
        Model m;
        Settings s;
        s.override_level = 0.3f;
        s.override_layer[kGrit] = 0.4f;
        m.Configure(s);
        Forced f;
        f.level[0][kDust] = 0.8f;
        f.level[0][kGrit] = 0.1f;
        m.Force(f);
        Check(Near(m.Level(0), 0.8f) && Near(m.Level(1), 0.3f), "model: forced dust wins over [dust] level", m.Level(0));
        Check(Near(m.Layers(0).body[kGrit], 0.1f) && Near(m.Layers(1).body[kGrit], 0.4f),
              "model: a forced layer wins over the INI override", m.Layers(0).body[kGrit]);
        Check(m.Real(0).body[kGrit] == 0.f && m.Real(0).body[kDust] == 0.f, "model: Real() ignores the INI overrides",
              m.Real(0).body[kDust]);
    }
}

void Navigation() {
    using namespace panel;
    {
        Rig r;
        r.p.Enable(false);
        Check(!r.Press(kToggle) && !r.p.Open() && !r.View().open && !r.View().badge,
              "off ([test] panel = 0): the key does nothing");
    }
    Rig r;
    Check(!r.p.Open() && !r.View().open, "enabled: starts closed");
    Check(!r.Press(kRight) && !r.Press(kOne) && !r.p.Forced().Any(), "closed: the other keys do nothing");
    r.Press(kToggle);
    Check(r.p.Open() && r.View().open, "the panel key opens it");
    Check(r.p.Selected() == kRowLayer && r.p.GetTarget() == kBoth, "starts on the dust, truck + trailer", r.p.Selected());
    Check(r.View().rows.size() == size_t(kRows) && r.View().rows[kRowLayer].selected, "one row per item, the selected one marked",
          double(r.View().rows.size()));
    r.Press(kUp, 2);
    Check(r.p.Selected() == kRowCleanAll, "up from the first row wraps to the last", r.p.Selected());
    r.Press(kDown);
    Check(r.p.Selected() == kRowTarget, "down from the last row wraps to the first", r.p.Selected());
    r.Press(kRight);
    Check(r.p.GetTarget() == kTruck && r.View().rows[kRowTarget].value == "truck", "right on the unit: the next unit",
          r.p.GetTarget());
    r.Press(kLeft);
    Check(r.p.GetTarget() == kBoth, "left on the unit wraps", r.p.GetTarget());
    r.Press(kEnter);
    Check(r.p.GetTarget() == kTruck, "enter on the unit: the next one too", r.p.GetTarget());
    r.Press(kToggle);
    Check(!r.p.Open() && !r.View().badge, "the panel key closes it (no badge: nothing forced)");
}

void Values() {
    using namespace panel;
    using dirt::kDust;
    {
        Rig r;
        r.Press(kToggle);
        r.Go(LayerRow(kDust));
        const bool changed = r.Press(kRight);
        Check(changed && Near(r.Body(0, kDust), 0.1f) && Near(r.Body(1, kDust), 0.1f),
              "right: +0.1 from the shown level, both units", r.Body(0, kDust));
        r.Press(kRight, 15);
        Check(Near(r.Body(0, kDust), 1.f) && Near(r.p.Forced().level[0][kDust], 1.f), "the top is 1", r.Body(0, kDust));
        Check(!r.Press(kRight), "right at 1: nothing changes");
        r.Press(kFineLeft, 3);
        Check(Near(r.Body(0, kDust), 0.97f), "fine left: -0.01 each", r.Body(0, kDust));
        r.Press(kLeft);
        Check(Near(r.Body(0, kDust), 0.9f), "left from 0.97: the grid below, 0.9", r.Body(0, kDust));
        r.Press(kLeft, 20);
        Check(Near(r.Body(0, kDust), 0.f) && Near(r.p.Forced().level[1][kDust], 0.f), "the bottom is 0 (forced, not real)",
              r.Body(0, kDust));
        Check(!r.Press(kFineLeft), "left at 0: nothing changes");
        r.Press(kOne);
        Check(Near(r.Body(1, kDust), 1.f), "end: 1 directly", r.Body(1, kDust));
        r.Press(kZero);
        Check(Near(r.Body(1, kDust), 0.f), "home: 0 directly", r.Body(1, kDust));
        // 100 fine steps up land exactly on 1 (no drift).
        r.Press(kFineRight, 100);
        Check(r.p.Forced().level[0][kDust] == 1.f, "100 fine steps from 0: exactly 1", r.p.Forced().level[0][kDust]);
        r.Press(kReal);
        Check(r.p.Forced().level[0][kDust] < 0.f && r.p.Forced().level[1][kDust] < 0.f && !r.p.Forced().Any(),
              "delete: the row is real again", r.p.Forced().level[0][kDust]);
    }
    {
        // A real level is the start; one unit at a time; the other keeps its own.
        Rig r;
        MakeDirty(r.model);
        const float real_mud = r.Body(0, dirt::kWetMud);
        r.Press(kToggle);
        r.Target(kTrailer);
        r.Go(LayerRow(dirt::kWetMud));
        r.Press(kRight);
        const float expect = std::floor(real_mud * 10.f + 1e-3f) / 10.f + 0.1f;
        Check(Near(r.Body(1, dirt::kWetMud), expect), "from a real level: the next 0.1 on the grid", r.Body(1, dirt::kWetMud));
        Check(Near(r.Body(0, dirt::kWetMud), real_mud) && r.p.Forced().level[0][dirt::kWetMud] < 0.f,
              "trailer only: the truck stays real", r.Body(0, dirt::kWetMud));
        r.Target(kBoth);
        r.Go(LayerRow(dirt::kWetMud));
        Check(r.View().rows[LayerRow(dirt::kWetMud)].value.find('/') != std::string::npos &&
                  r.View().rows[LayerRow(dirt::kWetMud)].forced,
              "both, different values: \"truck/trailer\", marked forced");
        r.Press(kOne);
        Check(Near(r.Body(0, dirt::kWetMud), 1.f) && Near(r.Body(1, dirt::kWetMud), 1.f) &&
                  r.View().rows[LayerRow(dirt::kWetMud)].value == "1.00",
              "both: the same value", r.Body(0, dirt::kWetMud));
    }
}

void Pairs() {
    using namespace panel;
    using namespace dirt;
    Rig r;
    r.Press(kToggle);
    r.Go(LayerRow(kWetMud));
    r.Press(kRight, 7);
    r.Go(LayerRow(kDryMud));
    r.Press(kRight, 6);
    Check(Near(r.Body(0, kDryMud), 0.6f) && Near(r.Body(0, kWetMud), 0.4f), "wet + dry mud <= 1: dry 0.6 lowers wet to 0.4",
          r.Body(0, kWetMud));
    r.Press(kOne);
    Check(Near(r.Body(1, kDryMud), 1.f) && Near(r.Body(1, kWetMud), 0.f), "dry mud 1: wet mud 0", r.Body(1, kWetMud));
    r.Go(LayerRow(kSpray));
    r.Press(kRight, 3);
    r.Go(LayerRow(kFilm));
    r.Press(kRight, 5);
    Check(Near(r.Body(0, kSpray), 0.3f) && Near(r.Body(0, kFilm), 0.5f), "spray + film under 1: both stay", r.Body(0, kFilm));
    r.Press(kOne);
    Check(Near(r.Body(0, kSpray), 0.f) && Near(r.Body(0, kFilm), 1.f), "film 1: spray 0", r.Body(0, kSpray));
    // A real partner counts too.
    Rig d;
    MakeDirty(d.model);
    const float real_wet = d.Body(0, kWetMud);
    d.Press(kToggle);
    d.Go(LayerRow(kDryMud));
    d.Press(kOne);
    Check(real_wet > 0.3f && Near(d.Body(0, kWetMud), 0.f) && d.p.Forced().level[0][kWetMud] == 0.f,
          "the real wet mud is forced down too", real_wet);
    d.Press(kReal);
    Check(d.p.Forced().level[0][kDryMud] < 0.f && Near(d.Body(0, kWetMud), 0.f),
          "real on dry mud: the forced wet mud stays", d.Body(0, kWetMud));
}

void Actions() {
    using namespace panel;
    using namespace dirt;
    Rig r;
    MakeDirty(r.model);
    const float real_wet = r.Body(0, kWetMud);
    r.Press(kToggle);
    r.Go(kRowCleanAll);
    Check(r.Press(kEnter), "clean all");
    bool clean = true;
    for (int u = 0; u < kUnits; ++u)
        for (int k = 0; k < kLayers; ++k) clean = clean && r.Body(u, static_cast<Layer>(k)) == 0.f;
    Check(clean, "clean all: every layer of both units shows 0");
    Check(Near(r.model.Real(0).body[kWetMud], real_wet), "clean all: the simulation keeps its mud", r.model.Real(0).body[kWetMud]);
    r.Go(kRowReach);
    Check(r.View().rows[kRowReach].value == "auto", "reach: \"auto\" while real");
    r.Press(kRight);
    Check(Near(r.p.Forced().reach[0], 0.5f) && Near(r.model.Layers(1).reach[kWetMud], 0.5f),
          "reach: the first step sets the middle, 0.5", r.p.Forced().reach[0]);
    r.Press(kRight, 9);
    Check(Near(r.p.Forced().reach[1], 1.f), "reach: the top is 1", r.p.Forced().reach[1]);
    r.Press(kToggle);
    const View closed = r.View();
    Check(!closed.open && closed.badge && closed.title.find("ctrl+f5") != std::string::npos,
          "closed with forced levels: they stay, with a badge");
    Check(r.Body(0, kWetMud) == 0.f, "... and stay in the model", r.Body(0, kWetMud));
    r.Press(kToggle);
    r.Go(kRowAllReal);
    Check(r.Press(kEnter) && !r.p.Forced().Any(), "all real: nothing forced");
    Check(Near(r.Body(0, kWetMud), real_wet) && r.View().rows[kRowReach].value == "auto",
          "all real: the real levels are shown again", r.Body(0, kWetMud));
    Check(!r.Press(kEnter), "all real twice: nothing changes");
    r.Press(kToggle);
    Check(!r.View().badge, "closed, nothing forced: no badge");
}

void Keys() {
    using namespace panel;
    Hotkey h;
    Check(ParseHotkey("ctrl+f9", &h) && h.vk == 0x78 && h.ctrl && !h.shift && !h.alt, "hotkey: ctrl+f9");
    Check(ParseHotkey(" Ctrl + Shift + P ", &h) && h.vk == 'P' && h.ctrl && h.shift && HotkeyName(h) == "ctrl+shift+p",
          "hotkey: spaces and case, back to text");
    Check(ParseHotkey("F24", &h) && h.vk == 0x87 && !h.ctrl && HotkeyName(h) == "f24", "hotkey: f24");
    Check(ParseHotkey("alt+numpad5", &h) && h.vk == 0x65 && h.alt, "hotkey: alt+numpad5");
    Check(ParseHotkey("control+PgDn", &h) && h.vk == 0x22 && HotkeyName(h) == "ctrl+pagedown", "hotkey: control+pgdn");
    Check(ParseHotkey("ctrl+f9", &h) && !ParseHotkey("", &h) && !ParseHotkey("ctrl+", &h) && !ParseHotkey("f25", &h) &&
              !ParseHotkey("ctrl+f9+f10", &h) && !ParseHotkey("hyper+f9", &h) && !ParseHotkey("f9x", &h) &&
              h.vk == 0x78,
          "hotkey: bad text is refused and leaves the old one");
    ParseHotkey("ctrl+f9", &h);
    Check(IsHotkey(h, 0x78, true, false, false) && !IsHotkey(h, 0x77, true, false, false) &&
              !IsHotkey(h, 0x78, false, false, false) && !IsHotkey(h, 0x78, true, true, false),
          "hotkey: ctrl+f9 only (not ctrl+f8, f9, ctrl+shift+f9)");
    Check(KeyOf(kVkUp, true, false, false) == kUp && KeyOf(kVkRight, true, true, false) == kFineRight &&
              KeyOf(kVkLeft, true, false, false) == kLeft && KeyOf(kVkHome, true, false, false) == kZero &&
              KeyOf(kVkEnd, true, false, false) == kOne && KeyOf(kVkDelete, true, false, false) == kReal &&
              KeyOf(kVkReturn, true, false, false) == kEnter,
          "keys: ctrl + arrows, home, end, delete, enter");
    Check(KeyOf(kVkUp, false, false, false) == kNone && KeyOf(kVkUp, true, false, true) == kNone &&
              KeyOf('W', true, false, false) == kNone,
          "keys: without ctrl, with alt or another key: nothing");
    Repeat rep;
    int presses = rep.Step(true, 1000);
    Check(presses == 1, "repeat: one press when the key goes down", presses);
    for (uint64_t t = 1016; t < 1400; t += 16) presses += rep.Step(true, t);
    Check(presses == 1, "repeat: nothing more for 400 ms", presses);
    for (uint64_t t = 1400; t <= 2000; t += 16) presses += rep.Step(true, t);
    Check(presses >= 9 && presses <= 12, "repeat: then about every 60 ms", presses);
    presses = rep.Step(true, 5000);
    presses += rep.Step(true, 5016) + rep.Step(true, 5032);
    Check(presses == 1, "repeat: a long frame gives one press, not a burst", presses);
    rep.Step(false, 5016);
    Check(rep.Step(true, 5032) == 1, "repeat: released and pressed again: a new press");
}

void Drawing() {
    using namespace panel;
    static uint8_t atlas[kAtlasW * kAtlasH];
    FontAtlas(atlas);
    // 'A' = { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00}: index 33 -> column 1, row 2.
    auto px = [&](int glyph, int x, int y) {
        return atlas[((glyph / kAtlasCols) * kGlyph + y) * kAtlasW + (glyph % kAtlasCols) * kGlyph + x];
    };
    const int a = 'A' - 32;
    Check(px(a, 2, 0) == 255 && px(a, 1, 0) == 0 && px(a, 0, 2) == 255 && px(a, 0, 7) == 0, "font: 'A' as in font8x8");
    int solid = 0, space = 0;
    for (int y = 0; y < kGlyph; ++y)
        for (int x = 0; x < kGlyph; ++x) solid += px(95, x, y) == 255, space += px(0, x, y) == 255;
    Check(solid == 64 && space == 0, "font: 127 is solid, space is empty", solid);
    Check(ScaleFor(720) == 1 && ScaleFor(1080) == 2 && ScaleFor(1440) == 2 && ScaleFor(2160) == 4 && ScaleFor(300) == 1,
          "scale: 1 at 720p, 2 at 1080p and 1440p, 4 at 4K", ScaleFor(1080));

    Rig r;
    Check(Layout(r.View(), 1920, 1080).empty(), "layout: closed and nothing forced: nothing drawn");
    r.Press(kToggle);
    r.Go(LayerRow(dirt::kSalt));
    r.Press(kRight, 4);
    const int sizes[][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    bool inside = true, crisp = true;
    size_t quads = 0;
    for (const auto& sz : sizes) {
        const std::vector<Quad> q = Layout(r.View(), sz[0], sz[1]);
        quads = q.size();
        const float cell = float(kGlyph * ScaleFor(sz[1]));
        for (const Quad& k : q) {
            inside = inside && k.x0 >= 0 && k.y0 >= 0 && k.x1 <= sz[0] && k.y1 <= sz[1] && k.x0 < k.x1 && k.y0 < k.y1;
            inside = inside && k.u0 >= 0 && k.u1 <= 1 && k.v0 >= 0 && k.v1 <= 1;
            // Glyphs on whole pixels, one atlas texel = `scale` pixels: crisp with point sampling.
            if (k.u0 != k.u1) crisp = crisp && k.x0 == std::floor(k.x0) && k.y0 == std::floor(k.y0) && k.x1 - k.x0 == cell;
        }
    }
    Check(quads > 100 && inside, "layout: open panel inside the screen, 720p to 4K", double(quads));
    Check(crisp, "layout: glyphs on whole pixels at the font's scale");
    const View v = r.View();
    Check(v.rows[LayerRow(dirt::kSalt)].value == "0.40" && v.rows[LayerRow(dirt::kSalt)].forced &&
              std::fabs(v.rows[LayerRow(dirt::kSalt)].bar - 0.4f) < 1e-4f && v.rows[LayerRow(dirt::kSalt)].label == "salt",
          "view: label, value, bar and forced mark of a row", v.rows[LayerRow(dirt::kSalt)].bar);
    Check(v.rows[LayerRow(dirt::kWetMud)].label == "wet mud" && !v.rows[LayerRow(dirt::kWetMud)].forced,
          "view: \"wet mud\", not forced");
    r.Press(kToggle);
    const std::vector<Quad> badge = Layout(r.View(), 1920, 1080);
    Check(!badge.empty() && badge.size() < 60, "layout: closed with forced levels: the badge only", double(badge.size()));

    // N12: the "drive ~1 m" notice while a unit is known by its wheels only.
    Check(LearningNotice(false, false).empty() && LearningNotice(true, false).find("drive ~1 m") == 0 &&
              LearningNotice(true, false).find("truck") != std::string::npos &&
              LearningNotice(false, true).find("trailer") != std::string::npos &&
              LearningNotice(true, true).find("truck and trailer") != std::string::npos,
          "notice: \"drive ~1 m\" names the wheels-only units");
    View with = r.View();
    with.notice = LearningNotice(true, true);
    const std::vector<Quad> badge2 = Layout(with, 1920, 1080);
    Check(badge2.size() > badge.size() + 20, "layout: the badge with the notice, one more line",
          double(badge2.size() - badge.size()));
    r.Press(kToggle);
    with = r.View();
    const std::vector<Quad> open = Layout(with, 1920, 1080);
    with.notice = LearningNotice(true, false);
    bool notice_in = true, crisp_notice = true;
    for (const auto& sz : sizes) {
        const float cell = float(kGlyph * ScaleFor(sz[1]));
        for (const Quad& k : Layout(with, sz[0], sz[1])) {
            notice_in = notice_in && k.x0 >= 0 && k.y0 >= 0 && k.x1 <= sz[0] && k.y1 <= sz[1];
            if (k.u0 != k.u1) crisp_notice = crisp_notice && k.x0 == std::floor(k.x0) && k.y0 == std::floor(k.y0) && k.x1 - k.x0 == cell;
        }
    }
    const size_t added = Layout(with, 1920, 1080).size() - open.size();
    Check(notice_in && crisp_notice && added > 20, "layout: the open panel with the notice, inside and crisp", double(added));
}


// N64h: the wipers' arc lives in the panel too, so it can be lined up against the blades on the screen without
// restarting the game. Its four numbers are the panel's own: they belong to no unit and are never "forced".
void Arc() {
    using namespace panel;
    Rig r;
    r.Press(kToggle);  // the keys do nothing while it is closed
    r.Go(kRowArc);
    const float start = r.p.Arc()[0];
    Check(start > 0.f && start < 1.f, "the arc starts at the INI's value", start);
    r.Press(kRight);  // on the grid of 0.1, as every other row of the panel: 0.45 goes to 0.50
    Check(std::fabs(r.p.Arc()[0] - 0.5f) < 1e-4f, "right moves the pivot onto the 0.1 grid", r.p.Arc()[0]);
    r.Press(kFineLeft);
    Check(std::fabs(r.p.Arc()[0] - 0.49f) < 1e-4f, "fine left moves it by 0.01", r.p.Arc()[0]);
    r.Press(kOne);
    Check(r.p.Arc()[0] == 1.f, "end sets it to 1", r.p.Arc()[0]);
    r.Press(kRight);
    Check(r.p.Arc()[0] == 1.f, "and it does not pass 1", r.p.Arc()[0]);
    r.Press(kZero);
    r.Press(kLeft);
    Check(r.p.Arc()[0] == 0.f, "nor go under 0", r.p.Arc()[0]);
    r.Press(kReal);
    Check(std::fabs(r.p.Arc()[0] - start) < 1e-4f, "real gives the INI's value back", r.p.Arc()[0]);
    Check(!r.p.Forced().Any(), "and none of it forces a dirt level", 0.0);
    // The sweep row says the angle it means.
    r.Go(kRowArc + 3);
    r.Press(kOne);
    Check(r.View().rows[kRowArc + 3].value.find("90 deg") != std::string::npos, "the sweep row shows the angle",
          r.p.Arc()[3]);
    // N64k: the last row is not the arc's -- it is how bright the dirt on the glass is, and it says the factor.
    r.Go(kRowArcLast);
    r.Press(kOne);
    Check(r.View().rows[kRowArcLast].value.find("x50") != std::string::npos, "the glass light row shows the factor",
          r.p.Arc()[4]);
    r.Press(kReal);
    Check(std::fabs(r.p.Arc()[4] - 0.20f) < 1e-4f, "real gives the INI's brightness back", r.p.Arc()[4]);
    Check(!r.p.Forced().Any(), "and it forces no dirt level either", 0.0);
    const float set[panel::kArcValues] = {0.3f, 0.1f, 0.5f, 0.4f, 0.7f};
    r.p.SetArc(set);
    Check(r.p.Arc()[2] == 0.5f, "the INI can put its numbers in", r.p.Arc()[2]);
    Check(r.p.Arc()[4] == 0.7f, "the brightness among them", r.p.Arc()[4]);
}

}  // namespace

// ---- V6: the keys menu ----

constexpr int kF5 = 0x74, kF7 = 0x76, kF8 = 0x77, kF9 = 0x78;
constexpr int kUp = 0x26, kDown = 0x28, kEnter = 0x0D, kDel = 0x2E, kEsc = 0x1B, kCtrl = 0x11;

lang::Catalog English() {
    lang::Table t;
    lang::Parse("menu.title = Truck Dirt: keys\nmenu.wash_key = Wash the truck\nmenu.panel_key = Test panel\n"
                "menu.not_set = (not set)\n"
                "menu.press_key = Press the new key ({key}: cancel)\n"
                "menu.help = {move}: choose, {edit}: change, {default}: default, {close}: close\n"
                "menu.saved = Saved\nmenu.taken = Already used: {name}\nmenu.unsupported = This key cannot be used\n",
                &t, nullptr);
    return lang::Catalog(t, t);
}

void MenuTests() {
    printf("-- V6: the keys menu\n");
    {
        panel::Hotkey h;
        panel::ParseHotkey("ctrl+shift+pagedown", &h);
        panel::Hotkey f8{kF8, false, false, false};
        Check(panel::HotkeyLabel(f8) == "F8" && panel::HotkeyLabel(h) == "Ctrl+Shift+PageDown",
              "hotkey labels: F8, Ctrl+Shift+PageDown");
    }
    menu::Menu m;
    m.Set(0, "");          // the wash key: not in the INI, its default
    m.Set(1, "ctrl+f10");  // the panel key from the INI
    Check(m.Get(0).vk == kF7 && m.Get(0).ctrl && m.Get(1).vk == 0x79 && m.Get(1).ctrl,
          "read: ctrl+f7 by default, ctrl+f10 from the INI");
    std::vector<menu::Write> w;
    Check(!m.Key(kUp, true, false, false, false, &w), "closed: keys do nothing");
    m.Toggle();
    Check(m.Open() && m.Row() == 0, "shift+f7 opens it on the first row");
    Check(!m.Key(kDown, false, false, false, false, &w) && m.Row() == 0,
          "a plain arrow does nothing while the game gets the keys");
    Check(m.Key(kDown, true, false, false, false, &w) && m.Row() == 1, "ctrl+down: the next row");
    Check(m.Key(kUp, false, false, false, true, &w) && m.Row() == 0, "a plain arrow works with the game's keys held back");
    Check(m.Key(kUp, true, false, false, false, &w) && m.Row() == 1, "ctrl+up from the first row: the last (wraps)");
    m.Key(kEnter, true, false, false, false, &w);
    Check(m.Capturing(), "ctrl+enter: waits for the new key");
    Check(!m.Key(kCtrl, true, false, false, false, &w) && m.Capturing(), "a modifier alone: still waiting");
    Check(m.Key(0x47, false, false, false, false, &w) && !m.Capturing() && m.Get(1).vk == 0x47 && w.size() == 1 &&
              w[0].section == "test" && w[0].key == "panel_key" && w[0].value == "g",
          "G: [test] panel_key = g written");
    w.clear();
    m.Key(kEnter, true, false, false, false, &w);
    m.Key(kF7, true, false, false, false, &w);
    Check(w.empty() && m.Get(1).vk == 0x47 && m.LastStatus() == menu::Status::kTaken,
          "ctrl+f7 for the panel: refused, the wash has it");
    m.Key(kEnter, true, false, false, false, &w);
    m.Key(kF7, false, true, false, false, &w);
    Check(w.empty() && m.LastStatus() == menu::Status::kTaken, "shift+f7 for the panel: refused, the menu has it");
    m.Key(kEnter, true, false, false, false, &w);
    m.Key(0xBA, false, false, false, false, &w);  // ';' (VK_OEM_1): no name in the INI syntax
    Check(w.empty() && m.LastStatus() == menu::Status::kUnsupported, "a key the INI cannot name: refused");
    m.Key(kEnter, true, false, false, false, &w);
    m.Key(kEsc, false, false, false, false, &w);
    Check(!m.Capturing() && m.Open() && w.empty() && m.Get(1).vk == 0x47, "esc while waiting: cancelled, menu still open");
    m.Key(kDown, true, false, false, false, &w);  // row 0: the wash (wraps)
    m.Key(kEnter, true, false, false, false, &w);
    m.Key(kF9, false, true, false, false, &w);
    Check(w.size() == 1 && w[0].section == "wash" && w[0].key == "key" && w[0].value == "shift+f9" && m.Get(0).shift,
          "shift+f9 for the wash: [wash] key = shift+f9");
    w.clear();
    m.Key(kDel, true, false, false, false, &w);
    Check(w.size() == 1 && w[0].value == "ctrl+f7" && m.Get(0).vk == kF7 && m.Get(0).ctrl && !m.Get(0).shift,
          "ctrl+del: the wash back to ctrl+f7");
    w.clear();
    m.Key(kUp, true, false, false, false, &w);  // row 1: the panel
    m.Key(kDel, true, false, false, false, &w);
    Check(w.size() == 1 && w[0].section == "test" && w[0].value == "ctrl+f5" && m.Get(1).vk == kF5 && m.Get(1).ctrl,
          "ctrl+del on the panel: back to ctrl+f5");
    const overlay::Menu v = m.MakeView(English(), false);
    Check(v.open && v.rows.size() == 2 && v.rows[0].value == "Ctrl+F7" && v.rows[1].value == "Ctrl+F5" &&
              v.rows[1].selected && v.status == "Saved",
          "the view: labels, values, the selected row, the status");
    Check(v.help.size() == 1 &&
              v.help[0] == "Ctrl+Up/Down: choose, Ctrl+Enter: change, Ctrl+Del: default, Shift+F7: close",
          "the help line names the keys (with Ctrl while the game gets them)");
    Check(m.MakeView(English(), true).help[0] == "Up/Down: choose, Enter: change, Del: default, Shift+F7, Esc: close",
          "the help line without Ctrl when the game's keys are held back");
    Check(!m.Key(kEsc, false, false, false, false, &w) && m.Open(), "esc does not close while the game gets the keys");
    Check(m.Key(kF7, false, true, false, false, &w) && !m.Open(), "shift+f7 closes it");
    m.Toggle();
    Check(m.Key(kEsc, false, false, false, true, &w) && !m.Open(), "esc closes it with the game's keys held back");
    // D11p: the keys the hook keeps (a key seen by the game among them: the hook is not getting the keys).
    Check(menu::HookKeeps(kUp, false, false) && menu::HookKeeps(kEsc, false, false) && menu::HookKeeps(0x47, true, false) &&
              menu::HookKeeps(kF7, false, false) && menu::HookKeeps(kF7, true, false),
          "the hook keeps arrows, Esc, letters, the F7 keys");
    Check(!menu::HookKeeps(kCtrl, true, false) && !menu::HookKeeps(0x5B, false, false) && !menu::HookKeeps(0x09, false, true) &&
              !menu::HookKeeps(0x73, false, true) && !menu::HookKeeps(kEsc, true, false) && !menu::HookKeeps(0x2C, false, false),
          "never Ctrl, Windows, Alt+Tab, Alt+F4, Ctrl+Esc, Print Screen");
}

// ---- D5: the overlay's layout with a fixed font ----

class FixedFont : public overlay::Font {
public:
    int LineHeight() const override { return 20; }
    bool Get(char32_t c, overlay::Glyph* out) override {
        if (c > 0x7F && c != U'ã') return false;  // ASCII and one accented letter
        *out = overlay::Glyph();
        out->advance = 10;
        out->w = c == ' ' ? 0 : 8, out->h = 14, out->x = 1, out->y = 3;
        return true;
    }
    void Solid(float* u, float* v) override { *u = *v = 0.001f; }
};

void OverlayTests() {
    printf("-- D5: the overlay's layout\n");
    FixedFont f;
    Check(overlay::TextWidth(f, "Lavar o caminh\xC3\xA3o") == 160, "text width: 16 glyphs of 10 px (UTF-8 decoded)");
    Check(overlay::TextWidth(f, "\xE6\x97\xA5") == 10, "a missing glyph is measured as '?'");
    Check(overlay::FontPx(1440) == 14 && overlay::FontPx(1080) == 11 && overlay::FontPx(2160) == 21 &&
              overlay::FontPx(2880) == 28 && overlay::FontPx(400) == 9,
          "font size (D11h): as the game's notice, 14 px per 1440 of height (11 at 1080p, 21 at 4K), at least 9");
    overlay::View v;
    Check(!v.Any() && overlay::Layout(v, f, 1920, 1080).empty(), "nothing to show: no quad");
    v.wash.message = "Press F8";
    std::vector<panel::Quad> q = overlay::Layout(v, f, 1920, 1080);
    bool inside = !q.empty();
    for (const panel::Quad& x : q) inside = inside && x.x0 >= 0 && x.x1 <= 1920 && x.y0 >= 0 && x.y1 <= 1080;
    Check(v.Any() && q.size() > 8 && inside, "the hint: its rounded box, lines and 7 glyphs, on the screen", double(q.size()));
    float bottom = 0;
    for (const panel::Quad& x : q) bottom = std::max(bottom, x.y1);
    Check(std::fabs(bottom - 0.82f * 1080) < 1.f, "D11g: its bottom edge at 82% of the height (hint_y)", bottom);
    v.wash.key = "F8";
    v.wash.detail = "$120";
    const std::vector<panel::Quad> keyed = overlay::Layout(v, f, 1920, 1080);
    Check(keyed.size() > q.size() + 2 + 4, "D11c: with a key and a price: the keycap, its 2 glyphs and the price's 4",
          double(keyed.size() - q.size()));
    v.wash.key.clear();
    v.wash.detail.clear();
    v.wash.message.clear();
    v.wash.darkness = 1.f;
    v.wash.washing = true;
    v.wash.show = 0.5f;
    v.wash.washing_text = "Washing";
    q = overlay::Layout(v, f, 1920, 1080);
    Check(!q.empty() && q[0].x0 == 0 && q[0].y0 == 0 && q[0].x1 == 1920 && q[0].y1 == 1080 && (q[0].rgba >> 24) == 255,
          "the wash: the whole screen black first", double(q.size()));
    const std::vector<panel::Quad> again = overlay::Layout(v, f, 1920, 1080);
    bool same = again.size() == q.size();
    for (size_t i = 0; same && i < q.size(); ++i) same = again[i].x0 == q[i].x0 && again[i].y0 == q[i].y0;
    Check(same, "the animation depends on its progress only (the same picture twice)");
    v.wash.darkness = 0.5f;
    q = overlay::Layout(v, f, 1920, 1080);
    Check((q[0].rgba >> 24) == 128, "half dark: the black at half alpha", double(q[0].rgba >> 24));
    // D11f: the visit: the icon, the title, the price and a lime bar in a pill, centred.
    v.wash.darkness = 1.f;
    v.wash.washing_detail = "$120";
    auto bar = [&](float show, float* centre) {
        v.wash.show = show;
        const std::vector<panel::Quad> b = overlay::Layout(v, f, 1920, 1080);
        float lo = 1e9f, hi = 0, bottom = 0;
        for (size_t i = 1; i < b.size(); ++i) lo = std::min(lo, b[i].x0), hi = std::max(hi, b[i].x1), bottom = std::max(bottom, b[i].y1);
        *centre = (lo + hi) / 2;
        float x0 = 1e9f, x1 = 0;
        for (const panel::Quad& x : b)
            if (x.rgba == 0xFF28E6AAu && x.y0 > bottom - 20) x0 = std::min(x0, x.x0), x1 = std::max(x1, x.x1);
        return x1 > x0 ? x1 - x0 : 0.f;
    };
    float centre = 0, centre2 = 0;
    const float quarter = bar(0.25f, &centre), most = bar(0.75f, &centre2);
    Check(std::fabs(centre - 960) < 2 && std::fabs(centre2 - 960) < 2, "D11f: the visit centred on the screen", centre);
    Check(quarter > 10 && std::fabs(most / quarter - 3.f) < 0.2f, "D11f: the lime bar grows with the progress (3x from 25% to 75%)",
          most / std::max(quarter, 1.f));
    v.wash.show = 0.01f;
    const size_t faint = overlay::Layout(v, f, 1920, 1080).size();
    v.wash.show = 0.f;
    Check(faint > 1 && overlay::Layout(v, f, 1920, 1080).size() == 1, "D11f: the visit fades in (nothing but the black at 0)");
}

int main() {
    ModelForce();
    Navigation();
    Values();
    Pairs();
    Actions();
    Arc();
    Keys();
    Drawing();
    MenuTests();
    OverlayTests();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "done", g_failures);
    return g_failures ? 1 : 0;
}
