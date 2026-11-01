// D10: the plugin's texts in the game's language (lang/<code>.txt), with
// English (en_gb) as the reserve for a missing language or key.
//
// Portable, C++17 only (tools/langtest runs it offline). The plugin reads the
// files itself (wide paths on Windows) and hands their bytes to Parse.
//
// File format (UTF-8, an optional BOM, LF or CRLF):
//   # a comment
//   wash.hint = Press {key} to wash the truck ({price})
// - one `key = text` per line; spaces around both are trimmed; in the text
//   `\n` is a line break, `\s` a no-break space (U+00A0) and `\\` a
//   backslash;
// - keys are [a-z0-9_.]; a key twice is an error (the first one stays);
// - `{name}` is a placeholder the plugin fills (Fill): {key} the wash key as
//   the player set it, {price} the price already formatted (Price);
// - keys starting with `_` describe the file, not a text:
//     _name       the language's own name
//     _script     latin, greek, cyrillic, georgian or cjk: the glyphs the
//                 drawing needs (the panel's font8x8 is ASCII only)
//     _review     ok, or estimate: a translation a speaker should check
//     _price      how a price reads, with {amount} and {currency}
//     _thousands  the thousands separator in {amount} (may be empty)
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace lang {

constexpr const char* kFallback = "en_gb";

// The game's locale codes (SCS Modding Wiki, "Mods localization support",
// 1.33; the game's `g_lang`): 42 codes.
extern const char* const kCodes[];
extern const int kCodeCount;

struct Table {
    std::map<std::string, std::string> text;  // key -> text (meta keys too)
    bool Has(const std::string& key) const { return text.count(key) != 0; }
    // The text, or "" when missing.
    const std::string& Get(const std::string& key) const;
};

// Reads a file's bytes into *out (added to what is there). Every problem
// (line, reason) goes to *errors when given; a bad line is skipped and the
// rest is read. Returns true with no problem.
bool Parse(const std::string& bytes, Table* out, std::vector<std::string>* errors);

// The language to load for the game's code: the code itself when available,
// else the first available code of the same language (pl_si -> pl_pl), else
// kFallback. `available` holds the codes that have a file. Case and '-' are
// normalised (PT-BR -> pt_br).
std::string Pick(const std::string& code, const std::vector<std::string>& available);

// The texts of one language over the reserve.
class Catalog {
public:
    Catalog() = default;
    Catalog(Table language, Table fallback) : language_(std::move(language)), fallback_(std::move(fallback)) {}
    // The language's text, else the reserve's, else the key itself (so a
    // missing text shows up on screen instead of nothing).
    std::string Get(const std::string& key) const;
    // Get(key) with the placeholders filled.
    std::string Format(const std::string& key, const std::vector<std::pair<std::string, std::string>>& vars) const;
    // A price in this language's way: _price with {amount} (thousands
    // separated by _thousands) and {currency} (the symbol or code the plugin
    // passes, e.g. "€", "£", "CHF").
    std::string Price(long long amount, const std::string& currency) const;

private:
    Table language_;
    Table fallback_;
};

// Replaces every {name} of vars in text; unknown placeholders stay as they are.
std::string Fill(const std::string& text, const std::vector<std::pair<std::string, std::string>>& vars);

// The placeholder names in text, in order ("{price} {key}" -> price, key).
std::vector<std::string> Placeholders(const std::string& text);

// Strict UTF-8 decoding (no overlongs, surrogates or values past U+10FFFF).
bool DecodeUtf8(const std::string& bytes, std::vector<char32_t>* out);

// ---- the game's language (N9 wiring) ----

// The language the game shows, from its game.log.txt: the last
// `uset g_lang "pt_br"` (the profile's setting, logged when it loads), else
// the last "Selected language: pt_br" (the start) [verified: ETS2 and ATS
// game.log.txt, 29/10/2026]. "" when neither is there.
std::string FromGameLog(const std::string& text);
// From the game's config.cfg: `uset g_lang "..."`, else `uset g_lang_init "..."`; "" when neither.
std::string FromConfig(const std::string& text);
// False for a language whose _script the overlay's font is not meant to show
// (cjk: Segoe UI has none of it): the plugin uses English for it. The other
// scripts, Georgian too, are checked against the font's glyphs in the game
// (panel_draw::MissingGlyphs) [verified: Segoe UI of Windows 11 shows all
// but cjk, tools/overlaypreview, 29/10/2026].
bool FontCanShow(const Table& t);
// Every code point of the language's texts (not the `_` keys), sorted, once each.
std::vector<char32_t> Glyphs(const Table& t);

// The DLL's own copy of lang/*.txt (src/lang_embed.cpp, written at build time by tools/embed_lang.cmake): the
// file's bytes, "" when there is none; and the codes it has.
std::string Embedded(const std::string& code);
std::vector<std::string> EmbeddedCodes();

}  // namespace lang
