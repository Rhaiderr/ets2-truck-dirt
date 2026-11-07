// N10: the in-game test panel: forces any level of any dirt layer of the truck,
// the trailer or both, to test the look without driving for it.
//
// Portable (no windows.h, tools/paneltest runs it offline): the panel's state
// and navigation, the hotkey text of the INI, the key repeat, the text of
// what it shows and its layout in quads over the embedded 8x8 font. The DLL
// reads the keys (plugin.cpp) and draws the quads (panel_draw.h).
//
// - Only with [test] panel = 1 in the INI; panel_key (default ctrl+f5: the
//   game has its own teleport on ctrl+f9) opens and closes it.
// - Rows: the unit (truck, trailer, both), one per layer (dirt::Layer), the
//   reach of the forced layers, "all real" and "clean all".
// - A layer starts from the level it shows and moves on a grid of 0.1 (0.01
//   fine), clamped to 0..1; 0 and 1 directly; "real" gives the row back to the
//   simulation. With both units the two get the same value (the truck's).
// - Wet + dry mud and spray + film stay <= 1: forcing one lowers its partner
//   (forced too, then) when the sum would pass 1.
// - "all real" drops every forced level; "clean all" forces every layer of
//   both units to 0. Neither touches the simulation, which keeps running under
//   the forced levels (dirt::Model::Force).
// - Closing the panel keeps the forced levels (to look at the truck); a badge
//   says they are on.
// - N12: while a unit is known by its wheels only (it has not moved 1 m since
//   it appeared, so only its wheels get dirt), the panel and the badge say to
//   drive ~1 m (LearningNotice; the DLL fills View::notice when it draws).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dirt_model.h"

namespace panel {

// What the panel does; the plugin maps the real keys to these (KeyOf).
enum Key { kNone, kToggle, kUp, kDown, kLeft, kRight, kFineLeft, kFineRight, kZero, kOne, kReal, kEnter, kKeys };

enum Target { kTruck, kTrailer, kBoth, kTargets };

// Rows, top to bottom.
enum Row { kRowTarget, kRowLayer, kRowReach = kRowLayer + dirt::kLayers, kRowAllReal, kRowCleanAll, kRows };

constexpr float kStep = 0.1f, kFineStep = 0.01f;

// What the panel shows (built for the drawing, game thread -> render thread).
struct ViewRow {
    std::string label;
    std::string value;
    float bar = -1.f;  // 0..1, < 0 = no bar
    bool selected = false;
    bool forced = false;
};
struct View {
    bool open = false;
    bool badge = false;  // closed with forced levels
    std::string title;
    std::string notice;  // N12: a warning under the title (LearningNotice), empty = none
    std::vector<ViewRow> rows;
    std::vector<std::string> help;
};

class Panel {
public:
    void Enable(bool on, const std::string& key_name = "ctrl+f5");
    bool Enabled() const { return enabled_; }
    bool Open() const { return open_; }
    // A key; `shown` = the unit's layers as they are shown now (start of a new
    // forced value). True when the forced levels changed.
    bool Press(Key k, const dirt::Coat shown[dirt::kUnits]);
    const dirt::Forced& Forced() const { return forced_; }
    // D5: every level real again (the wash at the station cleaned the rig).
    void ClearForced() { forced_ = dirt::Forced(); }
    int Selected() const { return row_; }
    Target GetTarget() const { return target_; }
    View MakeView(const dirt::Coat shown[dirt::kUnits]) const;

private:
    void SetLayer(int unit, dirt::Layer l, float v, const dirt::Coat& shown);
    void Move(float step, const dirt::Coat shown[dirt::kUnits]);
    void SetValue(float v, const dirt::Coat shown[dirt::kUnits]);
    void RowReal();

    bool enabled_ = false;
    bool open_ = false;
    std::string key_name_;
    int row_ = kRowLayer;
    Target target_ = kBoth;
    dirt::Forced forced_;
};

// N12: the warning while the truck and/or the trailer are known by their wheels
// only (empty when neither is).
std::string LearningNotice(bool truck, bool trailer);

// ---- keys ----

// A hotkey of the INI: "ctrl+f5", "shift+f7", "ctrl+shift+p", "alt+home"... (Windows virtual-key codes).
struct Hotkey {
    int vk = 0;
    bool ctrl = false, shift = false, alt = false;
};
bool ParseHotkey(const std::string& text, Hotkey* out);
std::string HotkeyName(const Hotkey& h);
// D5: the hotkey as the player reads it on the screen: "F8", "Ctrl+F9", "Ctrl+Shift+P", "Numpad5", "PgDn".
std::string HotkeyLabel(const Hotkey& h);

// Virtual-key codes the panel reads (the same values as windows.h).
enum Vk {
    kVkReturn = 0x0D, kVkEnd = 0x23, kVkHome = 0x24, kVkLeft = 0x25, kVkUp = 0x26, kVkRight = 0x27, kVkDown = 0x28,
    kVkDelete = 0x2E
};
// The navigation keys (held with ctrl while the panel is open), for the plugin to poll.
extern const int kNavKeys[8];
// The panel key of one key press (a navigation key with the modifiers held), kNone when it is not one.
Key KeyOf(int vk, bool ctrl, bool shift, bool alt);
// The hotkey's own press: its key with exactly its modifiers.
bool IsHotkey(const Hotkey& h, int vk, bool ctrl, bool shift, bool alt);

// Auto repeat of a held key: one press when it goes down, then after 400 ms
// one every 60 ms. Step() with the key state and a clock in ms: the presses now.
class Repeat {
public:
    int Step(bool down, uint64_t now_ms);

private:
    bool down_ = false;
    uint64_t next_ = 0;
};

// ---- layout ----

// The embedded font: 8x8 glyphs of ASCII 32..127 in a 16 x 6 grid, one byte
// per pixel (0 or 255); glyph 127 is a solid block (the plain quads).
constexpr int kGlyph = 8, kAtlasCols = 16, kAtlasW = kGlyph * kAtlasCols, kAtlasH = kGlyph * 6;
void FontAtlas(uint8_t out[kAtlasW * kAtlasH]);

// A textured quad in pixels (y down) with its atlas coordinates and colour (0xAABBGGRR).
struct Quad {
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
    uint32_t rgba;
};
// The scale of the 8x8 font for a screen height (2 at 1080p).
int ScaleFor(int height);
// The view in quads on a `width` x `height` screen.
std::vector<Quad> Layout(const View& v, int width, int height);

}  // namespace panel
