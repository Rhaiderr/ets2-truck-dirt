// D5: what the player sees of the plugin over the game: the wash at the
// station (the black screen, its animation, the hint and the messages) and
// the keys menu (V6), in the game's language.
//
// Portable (no windows.h; tools/washtest and tools/paneltest run it offline):
// the layout in quads (panel::Quad, the same drawing as the test panel) over
// a Font the caller gives. The DLL's font is the system's (GDI glyphs in a
// texture, panel_draw.h), so any alphabet the font has is shown; tests use a
// fixed one.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "test_panel.h"

namespace overlay {

// A glyph of the font, in pixels; its atlas corners 0..1.
struct Glyph {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    int w = 0, h = 0;  // the bitmap
    int x = 0, y = 0;  // its top-left from the pen: x right, y down from the line's top
    int advance = 0;
};

// The font the layout measures and places text with.
class Font {
public:
    virtual ~Font() = default;
    virtual int LineHeight() const = 0;
    // False: no such glyph (the layout puts '?' there).
    virtual bool Get(char32_t c, Glyph* out) = 0;
    // A texel of full coverage, for the plain quads.
    virtual void Solid(float* u, float* v) = 0;
};

// The font's pixel height on a screen this high: the game's notice's share of it (14 at 1440p, 11 at 1080p).
int FontPx(int screen_h);

struct Wash {
    float darkness = 0;        // 0 = the game, 1 = black
    bool washing = false;      // the animation is on the screen (wash::Phase::kShow, and fading out after it)
    float show = 0;            // its progress, 0..1
    std::string washing_text;  // "Washing the truck"
    std::string washing_detail;  // D11f: under it, the price (or the word for free); "" = none
    std::string message;       // the hint or a message; "" = none
    // D11c: the hint as a prompt: the key on a keycap before the message, the price after it ("" = none of them).
    std::string key;
    std::string detail;
    float y = 0.82f;           // where the prompt's bottom edge sits, from the top (0..1 of the screen's height; D11g: lower)
};

struct MenuRow {
    std::string label;
    std::string value;
    bool selected = false;
    bool editing = false;  // waiting for the new key
};

struct Menu {
    bool open = false;
    std::string title;
    std::vector<MenuRow> rows;
    std::string status;             // the last result ("saved", "already used by ...")
    std::vector<std::string> help;  // the keys, one line each
};

struct View {
    Wash wash;
    Menu menu;
    bool Any() const { return wash.darkness > 0.f || !wash.message.empty() || menu.open; }
};

// The text's width in pixels (one line; invalid UTF-8 is read as Latin-1).
int TextWidth(Font& f, const std::string& utf8);

// The view in quads on a `width` x `height` screen.
std::vector<panel::Quad> Layout(const View& v, Font& f, int width, int height);

}  // namespace overlay
