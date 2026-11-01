// Offline test of the D10 texts (src/lang and lang/*.txt): the file format,
// the reserve, the placeholders and the price, then every language file
// against the English reserve: every key there, no key more, the same
// placeholders (no {price} lost), the glyphs of its _script and no fixed
// currency. Ends with the hint of every language, filled in, for review.
// Usage: langtest <lang dir>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "lang.h"
#include "wash.h"

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what, const std::string& detail = std::string()) {
    printf("%s %-66s %s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++g_failures;
}

std::string Join(const std::vector<std::string>& v, const char* sep = ", ") {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : sep) + x;
    return s;
}

bool ReadFile(const std::filesystem::path& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::vector<std::string> Sorted(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
}

// Code points every script may use besides ASCII: no-break spaces, dashes,
// quotes, the ellipsis, guillemets.
bool CommonPunctuation(char32_t c) {
    return c == 0xa0 || c == 0x202f || c == 0x2009 || c == 0x2013 || c == 0x2014 || (c >= 0x2018 && c <= 0x201e) ||
           c == 0x2026 || c == 0xab || c == 0xbb;
}

bool InScript(const std::string& script, char32_t c) {
    if (c < 0x80 || CommonPunctuation(c)) return true;
    if (script == "latin") return (c >= 0xc0 && c <= 0x24f) || (c >= 0x1e00 && c <= 0x1eff);
    if (script == "greek") return (c >= 0x370 && c <= 0x3ff) || (c >= 0x1f00 && c <= 0x1fff);
    if (script == "cyrillic") return c >= 0x400 && c <= 0x4ff;
    if (script == "georgian") return c >= 0x10a0 && c <= 0x10ff;
    if (script == "cjk")
        return (c >= 0x3000 && c <= 0x30ff) || (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xac00 && c <= 0xd7af) ||
               (c >= 0x1100 && c <= 0x11ff) || (c >= 0x3130 && c <= 0x318f) || (c >= 0xff00 && c <= 0xffef);
    return false;
}

std::string Hex(char32_t c) {
    char b[16];
    snprintf(b, sizeof(b), "U+%04X", static_cast<unsigned>(c));
    return b;
}

void FormatTests() {
    printf("-- file format\n");
    {
        lang::Table t;
        std::vector<std::string> errors;
        const bool ok = lang::Parse("\xEF\xBB\xBF# comment\r\n\r\n  a.b = x y  \r\nc = one\\ntwo\\\\\nd=\\s1\\s\n", &t, &errors);
        Check(ok && errors.empty() && t.text.size() == 3, "BOM, CRLF, comments and blank lines", std::to_string(t.text.size()));
        Check(t.Get("a.b") == "x y", "spaces around key and text trimmed", "\"" + t.Get("a.b") + "\"");
        Check(t.Get("c") == "one\ntwo\\", "\\n and \\\\ escapes");
        Check(t.Get("d") == "\xC2\xA0" "1\xC2\xA0", "\\s is a no-break space, kept at the ends");
    }
    {
        lang::Table t;
        std::vector<std::string> errors;
        const bool ok = lang::Parse("a = 1\na = 2\nno equals\nB = 3\nbad = \\x\nutf = \xC3\x28\nlast = ok", &t, &errors);
        Check(!ok && errors.size() == 5, "bad lines reported", Join(errors, " | "));
        Check(t.Get("a") == "1" && t.Get("last") == "ok" && t.text.size() == 2, "the first of a key twice stays, the rest read");
    }
    {
        std::vector<char32_t> cp;
        Check(lang::DecodeUtf8("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x9A\x9A", &cp) && cp.size() == 4 && cp[1] == 0xe9 &&
                  cp[2] == 0x20ac && cp[3] == 0x1f69a,
              "UTF-8 of 1 to 4 bytes");
        int rejected = 0;
        for (const char* bad : {"\xC0\xAF", "\xE0\x80\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE2\x82", "\x80", "\xF5\x80\x80\x80"})
            rejected += lang::DecodeUtf8(bad, &cp) ? 0 : 1;
        Check(rejected == 7, "overlong, surrogate, past U+10FFFF, cut, stray: rejected", std::to_string(rejected) + " of 7");
    }
    printf("-- placeholders, reserve and price\n");
    Check(lang::Fill("{key}: {price} {other}", {{"key", "F8"}, {"price", "120"}}) == "F8: 120 {other}",
          "Fill: known filled, unknown kept");
    Check(lang::Fill("{key", {{"key", "F8"}}) == "{key", "Fill: an unclosed brace is text");
    Check(Join(lang::Placeholders("a {price} b {key} {price}")) == "price, key, price", "Placeholders in order");
    {
        lang::Table language, fallback;
        lang::Parse("x = língua\n_price = {amount}\\s{currency}\n_thousands = \\s", &language, nullptr);
        lang::Parse("x = reserve\ny = only reserve\n_price = {currency}{amount}\n_thousands = ,", &fallback, nullptr);
        const lang::Catalog c(language, fallback);
        Check(c.Get("x") == "língua" && c.Get("y") == "only reserve" && c.Get("z") == "z",
              "the language, else the reserve, else the key");
        Check(c.Price(1234567, "\xE2\x82\xAC") == "1\xC2\xA0" "234\xC2\xA0" "567\xC2\xA0\xE2\x82\xAC",
              "price: the language's pattern and separator", c.Price(1234567, "EUR"));
        const lang::Catalog english(fallback, fallback);
        Check(english.Price(180, "\xC2\xA3") == "\xC2\xA3" "180" && english.Price(-1500, "CHF") == "CHF-1,500" &&
                  english.Price(0, "$") == "$0",
              "price: under 1000, negative, zero", english.Price(-1500, "CHF"));
        lang::Table none;
        lang::Parse("_thousands =", &none, nullptr);
        Check(lang::Catalog(none, fallback).Price(70000, "Ft") == "Ft70000", "price: an empty separator is kept",
              lang::Catalog(none, fallback).Price(70000, "Ft"));
    }
    {
        const std::vector<std::string> have = {"en_gb", "pl_pl", "pt_br", "pt_pt", "sr_sp"};
        Check(lang::Pick("pt_br", have) == "pt_br" && lang::Pick("PT-PT", have) == "pt_pt", "Pick: the code itself");
        Check(lang::Pick("pl_si", have) == "pl_pl" && lang::Pick("pt_xx", have) == "pt_br" &&
                  lang::Pick("sr_sr", have) == "sr_sp",
              "Pick: the same language, in kCodes order");
        Check(lang::Pick("fr_ca", have) == "en_gb" && lang::Pick("", have) == "en_gb", "Pick: else the reserve");
    }
}

struct Loaded {
    std::string code;
    lang::Table table;
};

void FileTests(const std::filesystem::path& dir) {
    printf("-- lang files (%d codes)\n", lang::kCodeCount);
    std::vector<Loaded> files;
    int unreadable = 0;
    for (int i = 0; i < lang::kCodeCount; ++i) {
        Loaded l;
        l.code = lang::kCodes[i];
        std::string bytes;
        std::vector<std::string> errors;
        if (!ReadFile(dir / (l.code + ".txt"), &bytes)) {
            Check(false, l.code + ": file", "missing");
            ++unreadable;
            continue;
        }
        if (!lang::Parse(bytes, &l.table, &errors)) {
            Check(false, l.code + ": parse", Join(errors, " | "));
            ++unreadable;
        }
        files.push_back(std::move(l));
    }
    Check(unreadable == 0, "every code has a file that reads without a problem",
          std::to_string(lang::kCodeCount - unreadable) + " of " + std::to_string(lang::kCodeCount));
    {
        std::set<std::string> known(lang::kCodes, lang::kCodes + lang::kCodeCount);
        std::vector<std::string> extra;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::string name = e.path().filename().string();
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".txt") == 0 &&
                !known.count(name.substr(0, name.size() - 4)))
                extra.push_back(name);
        }
        Check(!ec && extra.empty(), "no file for a code the game does not have", Join(Sorted(extra)));
    }
    const auto reserve =
        std::find_if(files.begin(), files.end(), [](const Loaded& l) { return l.code == lang::kFallback; });
    if (reserve == files.end()) {
        Check(false, "the reserve (en_gb) read");
        return;
    }
    const lang::Table& en = reserve->table;

    // The reserve has every text the plugin asks for.
    {
        std::vector<std::string> missing;
        for (int m = static_cast<int>(wash::Message::kHint); m <= static_cast<int>(wash::Message::kUnknownStation); ++m) {
            const std::string key = std::string("wash.") + wash::MessageName(static_cast<wash::Message>(m));
            if (en.Get(key).empty()) missing.push_back(key);
        }
        for (const char* key : {"wash.washing", "_name", "_script", "_review", "_price", "_thousands"})
            if (!en.Has(key)) missing.push_back(key);
        Check(missing.empty(), "en_gb: a text for every wash::Message and the meta keys", Join(missing));
        Check(Sorted(en.Has("wash.hint") ? lang::Placeholders(en.Get("wash.hint")) : std::vector<std::string>()) ==
                      std::vector<std::string>({"key", "price"}) &&
                  lang::Placeholders(en.Get("wash.no_money")) == std::vector<std::string>({"price"}),
              "en_gb: {key} and {price} in the hint, {price} in no_money");
    }

    int missing_keys = 0, extra_keys = 0, placeholder_bad = 0, empty = 0, glyph_bad = 0, meta_bad = 0, currency = 0;
    std::vector<std::string> estimate, other_font;
    for (const Loaded& l : files) {
        const lang::Table& t = l.table;
        for (const auto& kv : en.text) {
            if (!t.Has(kv.first)) {
                Check(false, l.code + ": " + kv.first, "missing");
                ++missing_keys;
                continue;
            }
            const std::string& text = t.Get(kv.first);
            if (text.empty() && kv.first != "_thousands") {
                Check(false, l.code + ": " + kv.first, "empty");
                ++empty;
            }
            if (Sorted(lang::Placeholders(text)) != Sorted(lang::Placeholders(kv.second))) {
                Check(false, l.code + ": " + kv.first + " placeholders",
                      "{" + Join(lang::Placeholders(text), "} {") + "} instead of {" +
                          Join(lang::Placeholders(kv.second), "} {") + "}");
                ++placeholder_bad;
            }
            if (kv.first[0] != '_' && (text.find("\xE2\x82\xAC") != std::string::npos || text.find('$') != std::string::npos ||
                                       text.find("\xC2\xA3") != std::string::npos)) {
                Check(false, l.code + ": " + kv.first, "a fixed currency (the plugin passes it in {price})");
                ++currency;
            }
        }
        for (const auto& kv : t.text) {
            if (!en.Has(kv.first)) {
                Check(false, l.code + ": " + kv.first, "not in en_gb");
                ++extra_keys;
            }
        }
        const std::string script = t.Get("_script");
        const std::string review = t.Get("_review");
        if (!(script == "latin" || script == "greek" || script == "cyrillic" || script == "georgian" || script == "cjk") ||
            !(review == "ok" || review == "estimate")) {
            Check(false, l.code + ": _script / _review", script + " / " + review);
            ++meta_bad;
        }
        if (review == "estimate") estimate.push_back(l.code);
        if (script == "cjk" || script == "georgian") other_font.push_back(l.code);
        std::set<char32_t> outside;
        for (const auto& kv : t.text) {
            if (kv.first == "_name") continue;  // the language's own name
            std::vector<char32_t> cp;
            lang::DecodeUtf8(kv.second, &cp);
            for (char32_t c : cp)
                if (!InScript(script, c)) outside.insert(c);
        }
        if (!outside.empty()) {
            std::vector<std::string> list;
            for (char32_t c : outside) list.push_back(Hex(c));
            Check(false, l.code + ": glyphs outside " + script, Join(list));
            ++glyph_bad;
        }
    }
    Check(missing_keys == 0 && empty == 0, "every language has every key of en_gb, none empty",
          std::to_string(en.text.size()) + " keys");
    Check(extra_keys == 0, "no language has a key en_gb does not have");
    Check(placeholder_bad == 0, "the same placeholders as en_gb everywhere (no {price} or {key} lost)");
    Check(currency == 0, "no fixed currency in a text");
    Check(meta_bad == 0 && glyph_bad == 0, "_script and _review valid; every glyph in its script");
    printf("     to check by a speaker [estimate] (%zu): %s\n", estimate.size(), Join(estimate).c_str());
    printf("     need another font (%zu): %s\n", other_font.size(), Join(other_font).c_str());

    printf("-- every language (script, review, distinct non-ASCII glyphs, the hint filled in)\n");
    for (const Loaded& l : files) {
        const lang::Catalog c(l.table, en);
        std::set<char32_t> glyphs;
        for (const auto& kv : l.table.text) {
            std::vector<char32_t> cp;
            lang::DecodeUtf8(kv.second, &cp);
            for (char32_t g : cp)
                if (g >= 0x80) glyphs.insert(g);
        }
        printf("     %s %-8s %-8s %3zu  %s\n", l.code.c_str(), l.table.Get("_script").c_str(),
               l.table.Get("_review").c_str(), glyphs.size(),
               c.Format("wash.hint", {{"key", "F8"}, {"price", c.Price(1180, "\xE2\x82\xAC")}}).c_str());
    }
}

// N9 wiring: the game's language from its own files, and which languages the overlay's font takes.
void GameLanguageTests(const std::filesystem::path& dir) {
    printf("-- the game's language (game.log.txt, config.cfg) and the font\n");
    // As the game writes them [verified: ETS2 and ATS game.log.txt, config.cfg, 29/10/2026].
    const std::string log = "00:00:01.374 : [hashfs] locale.scs: Created and validated, 522 entries\r\n"
                            "00:00:03.670 : uset g_lang_init \"pt_br\"\r\n"
                            "00:00:03.708 : Selected language: pt_br\r\n"
                            "00:00:11.696 : uset g_lang_set \"1\"\r\n"
                            "00:00:11.696 : uset g_lang \"de_de\"\r\n"
                            "00:00:11.696 : uset g_cutscenes \"1\"\r\n";
    Check(lang::FromGameLog(log) == "de_de", "game.log: the profile's g_lang wins over the start", lang::FromGameLog(log));
    Check(lang::FromGameLog("00:00:03.708 : Selected language: pt_br\r\n00:00:04 : [lang] x\r\n") == "pt_br",
          "game.log: no g_lang yet: the language selected at the start");
    Check(lang::FromGameLog(log + "00:10:00.000 : uset g_lang \"fr_fr\"\n") == "fr_fr", "game.log: the last g_lang");
    Check(lang::FromGameLog("nothing here\n").empty() && lang::FromGameLog("").empty(), "game.log: none: \"\"");
    Check(lang::FromGameLog("uset g_lang_init \"pt_br\"\n").empty(), "game.log: g_lang_init is not g_lang");
    const std::string cfg = "uset g_lang_init \"pt_br\"\nuset g_vsync \"1\"\n";
    Check(lang::FromConfig(cfg) == "pt_br", "config.cfg: g_lang_init when there is no g_lang", lang::FromConfig(cfg));
    Check(lang::FromConfig(cfg + "uset g_lang \"ru_ru\"\r\n") == "ru_ru", "config.cfg: g_lang wins");
    Check(lang::FromConfig("uset g_lang \"\"\n").empty(), "config.cfg: an empty value: \"\"");

    std::vector<std::string> codes, english;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.path().extension() == ".txt") codes.push_back(e.path().stem().string());
    std::sort(codes.begin(), codes.end());
    for (const std::string& code : codes) {
        std::string bytes;
        lang::Table t;
        if (ReadFile(dir / (code + ".txt"), &bytes)) lang::Parse(bytes, &t, nullptr);
        if (!lang::FontCanShow(t)) english.push_back(code);
    }
    Check(Join(english) == "ja_jp, ko_kr, zh_cn, zh_tw", "the overlay's font: English for cjk (Georgian: by its glyphs)",
          Join(english));
    lang::Table pt;
    std::string bytes;
    ReadFile(dir / "pt_br.txt", &bytes);
    lang::Parse(bytes, &pt, nullptr);
    const std::vector<char32_t> g = lang::Glyphs(pt);
    std::string non_ascii;
    for (const char32_t c : g)
        if (c >= 0x80) non_ascii += std::to_string(static_cast<unsigned>(c)) + " ";
    Check(std::is_sorted(g.begin(), g.end()) && std::adjacent_find(g.begin(), g.end()) == g.end() &&
              std::find(g.begin(), g.end(), U'ã') != g.end() && std::find(g.begin(), g.end(), U'_') == g.end(),
          "the glyphs of pt_br's texts: sorted, once each, no meta key", non_ascii);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: langtest <lang dir>\n");
        return 2;
    }
    FormatTests();
    FileTests(argv[1]);
    GameLanguageTests(argv[1]);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
