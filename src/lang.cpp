#include "lang.h"

#include <algorithm>
#include <cctype>

namespace lang {

const char* const kCodes[] = {
    "bg_bg", "ca_es", "cs_cz", "da_dk", "de_de", "el_gr", "en_gb", "en_us", "es_es", "es_la", "et_ee",
    "eu_es", "fi_fi", "fr_fr", "gl_es", "hr_hr", "hu_hu", "it_it", "ja_jp", "ka_ge", "ko_kr", "lt_lt",
    "lv_lv", "mk_mk", "nl_nl", "no_no", "pl_pl", "pl_si", "pt_br", "pt_pt", "ro_ro", "ru_ru", "sk_sk",
    "sl_sl", "sr_sp", "sr_sr", "sv_se", "tr_tr", "uk_uk", "vi_vn", "zh_cn", "zh_tw",
};
const int kCodeCount = static_cast<int>(sizeof(kCodes) / sizeof(kCodes[0]));

namespace {

const std::string kEmpty;

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

bool ValidKey(const std::string& k) {
    if (k.empty()) return false;
    for (char c : k)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
    return true;
}

// `\n` -> line break, `\s` -> no-break space (U+00A0: a plain space at an
// end would be trimmed), `\\` -> backslash; false on any other escape.
bool Unescape(const std::string& in, std::string* out) {
    out->clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\') {
            *out += in[i];
            continue;
        }
        if (i + 1 >= in.size()) return false;
        const char c = in[++i];
        if (c == 'n') *out += '\n';
        else if (c == 's') *out += "\xC2\xA0";
        else if (c == '\\') *out += '\\';
        else return false;
    }
    return true;
}

std::string Normalise(const std::string& code) {
    std::string s;
    for (char c : code) s += c == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return Trim(s);
}

}  // namespace

const std::string& Table::Get(const std::string& key) const {
    const auto it = text.find(key);
    return it == text.end() ? kEmpty : it->second;
}

bool DecodeUtf8(const std::string& bytes, std::vector<char32_t>* out) {
    out->clear();
    size_t i = 0;
    while (i < bytes.size()) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        int extra;
        char32_t cp;
        if (c < 0x80) { extra = 0; cp = c; }
        else if (c >= 0xc2 && c <= 0xdf) { extra = 1; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { extra = 2; cp = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { extra = 3; cp = c & 0x07; }
        else return false;
        if (i + extra >= bytes.size() && extra > 0) return false;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char d = static_cast<unsigned char>(bytes[i + k]);
            if ((d & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3f);
        }
        if ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10ffff)) ||
            (cp >= 0xd800 && cp <= 0xdfff))
            return false;
        out->push_back(cp);
        i += 1 + extra;
    }
    return true;
}

bool Parse(const std::string& bytes, Table* out, std::vector<std::string>* errors) {
    bool ok = true;
    auto fail = [&](int line, const std::string& why) {
        ok = false;
        if (errors) errors->push_back("line " + std::to_string(line) + ": " + why);
    };
    size_t pos = bytes.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    std::vector<char32_t> scratch;
    for (int line = 1; pos <= bytes.size(); ++line) {
        size_t end = bytes.find('\n', pos);
        if (end == std::string::npos) end = bytes.size();
        std::string raw = bytes.substr(pos, end - pos);
        pos = end + 1;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        const std::string s = Trim(raw);
        if (s.empty() || s[0] == '#') continue;
        if (!DecodeUtf8(s, &scratch)) {
            fail(line, "not UTF-8");
            continue;
        }
        const size_t eq = s.find('=');
        if (eq == std::string::npos) {
            fail(line, "no '='");
            continue;
        }
        const std::string key = Trim(s.substr(0, eq));
        std::string value;
        if (!ValidKey(key)) {
            fail(line, "bad key \"" + key + "\"");
            continue;
        }
        if (!Unescape(Trim(s.substr(eq + 1)), &value)) {
            fail(line, key + ": bad escape (only \\n, \\s and \\\\)");
            continue;
        }
        if (out->Has(key)) {
            fail(line, key + ": twice (the first one stays)");
            continue;
        }
        out->text[key] = value;
    }
    return ok;
}

std::string Pick(const std::string& code, const std::vector<std::string>& available) {
    const std::string want = Normalise(code);
    for (const std::string& a : available)
        if (a == want) return a;
    const std::string language = want.substr(0, want.find('_'));
    if (!language.empty()) {
        // In kCodes order, so the choice does not depend on the folder listing.
        for (int i = 0; i < kCodeCount; ++i) {
            const std::string c = kCodes[i];
            if (c.compare(0, language.size() + 1, language + "_") != 0) continue;
            for (const std::string& a : available)
                if (a == c) return a;
        }
    }
    return kFallback;
}

namespace {

// The value of `uset <name> "<value>"` on one line ("" when the line is not that).
std::string Uset(const std::string& line, const std::string& name) {
    const std::string head = "uset " + name + " \"";
    const size_t at = line.find(head);
    if (at == std::string::npos) return std::string();
    const size_t start = at + head.size(), end = line.find('"', start);
    return end == std::string::npos ? std::string() : line.substr(start, end - start);
}

// Calls f(line) for every line of text.
template <typename F>
void Lines(const std::string& text, F f) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        f(text.substr(pos, end - pos));
        pos = end + 1;
    }
}

}  // namespace

std::string FromGameLog(const std::string& text) {
    std::string set, selected;
    const std::string kSelected = "Selected language: ";
    Lines(text, [&](const std::string& line) {
        const std::string v = Uset(line, "g_lang");
        if (!v.empty()) set = v;
        const size_t at = line.find(kSelected);
        if (at != std::string::npos) {
            std::string s = line.substr(at + kSelected.size());
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (!s.empty()) selected = s;
        }
    });
    return set.empty() ? selected : set;
}

std::string FromConfig(const std::string& text) {
    std::string lang, init;
    Lines(text, [&](const std::string& line) {
        const std::string v = Uset(line, "g_lang"), i = Uset(line, "g_lang_init");
        if (!v.empty()) lang = v;
        if (!i.empty()) init = i;
    });
    return lang.empty() ? init : lang;
}

bool FontCanShow(const Table& t) {
    const std::string& script = t.Get("_script");
    return script != "cjk";
}

std::vector<char32_t> Glyphs(const Table& t) {
    std::vector<char32_t> out;
    for (const auto& kv : t.text) {
        if (!kv.first.empty() && kv.first[0] == '_') continue;
        std::vector<char32_t> cps;
        if (DecodeUtf8(kv.second, &cps)) out.insert(out.end(), cps.begin(), cps.end());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string Catalog::Get(const std::string& key) const {
    const std::string& t = language_.Get(key);
    if (!t.empty()) return t;
    const std::string& f = fallback_.Get(key);
    if (!f.empty()) return f;
    return key;
}

std::string Catalog::Format(const std::string& key,
                            const std::vector<std::pair<std::string, std::string>>& vars) const {
    return Fill(Get(key), vars);
}

std::string Catalog::Price(long long amount, const std::string& currency) const {
    const bool negative = amount < 0;
    std::string digits = std::to_string(negative ? -amount : amount);
    // _thousands may be set to empty on purpose: only the reserve's when absent.
    const std::string sep = language_.Has("_thousands") ? language_.Get("_thousands") : fallback_.Get("_thousands");
    std::string grouped;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) grouped += sep;
        grouped += digits[i];
    }
    if (negative) grouped = "-" + grouped;
    std::string pattern = Get("_price");
    if (pattern == "_price") pattern = "{currency}{amount}";
    return Fill(pattern, {{"amount", grouped}, {"currency", currency}});
}

std::string Fill(const std::string& text, const std::vector<std::pair<std::string, std::string>>& vars) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '{') {
            const size_t close = text.find('}', i + 1);
            if (close != std::string::npos) {
                const std::string name = text.substr(i + 1, close - i - 1);
                bool found = false;
                for (const auto& v : vars) {
                    if (v.first == name) {
                        out += v.second;
                        found = true;
                        break;
                    }
                }
                if (found) {
                    i = close + 1;
                    continue;
                }
            }
        }
        out += text[i++];
    }
    return out;
}

std::vector<std::string> Placeholders(const std::string& text) {
    std::vector<std::string> names;
    size_t i = 0;
    while ((i = text.find('{', i)) != std::string::npos) {
        const size_t close = text.find('}', i + 1);
        if (close == std::string::npos) break;
        names.push_back(text.substr(i + 1, close - i - 1));
        i = close + 1;
    }
    return names;
}

}  // namespace lang
