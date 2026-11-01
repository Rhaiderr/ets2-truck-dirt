// D5: saved dirt between sessions (see dirt_state.h).

#include "dirt_state.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#include <io.h>  // _commit
#else
#include <unistd.h>  // fsync
#endif

namespace dirt_state {
namespace {

constexpr const char* kHeader = "truckdirt_state";
constexpr int kScale = 10000;                 // 4 decimals in the file
constexpr std::uintmax_t kMaxFileBytes = 1 << 22;  // far above kMaxEntries lines; more is not ours
constexpr size_t kMaxLayerName = 32;

// Key fields: bytes outside '!'..'~' and the separators are written %XX.
bool NeedsEscape(unsigned char c) { return c < 0x21 || c > 0x7e || c == '%' || c == '|' || c == '='; }

std::string Escape(const std::string& s) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (NeedsEscape(c)) {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;  // lower case is not what Escape writes: not canonical
}

// One escaped key field as Escape writes it.
bool ValidField(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '%') {
            if (i + 2 >= s.size()) return false;
            const int hi = HexDigit(s[i + 1]), lo = HexDigit(s[i + 2]);
            if (hi < 0 || lo < 0 || !NeedsEscape(static_cast<unsigned char>(hi * 16 + lo))) return false;
            i += 2;
        } else if (NeedsEscape(c)) {
            return false;
        }
    }
    return true;
}

bool ValidKey(const std::string& key) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t bar = key.find('|', start);
        parts.push_back(key.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    if (parts.size() != 3 || (parts[0] != "truck" && parts[0] != "trailer") || parts[1].empty()) return false;
    return ValidField(parts[1]) && ValidField(parts[2]);
}

bool ValidLayerName(const std::string& s) {
    if (s.empty() || s.size() > kMaxLayerName) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// The level as the file keeps it: 0..1 in steps of 1/kScale (NaN = 0).
int Steps(float v) {
    if (!(v > 0.f)) return 0;
    if (v >= 1.f) return kScale;
    return static_cast<int>(std::lround(double(v) * kScale));
}

float Round(float v) { return static_cast<float>(Steps(v)) / kScale; }

std::string FormatLevel(float v) {
    const int n = Steps(v);
    char b[16];
    snprintf(b, sizeof(b), "%d.%04d", n / kScale, n % kScale);  // no locale: integers only
    return b;
}

// "0", "1", "0.4120", "1.0": digits, an optional fraction, at most 1.
bool ParseLevel(const std::string& s, float* out) {
    if (s.empty() || s.size() > 12) return false;
    double value = 0, unit = 1;
    bool fraction = false, digits = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '.' && !fraction && digits && i + 1 < s.size()) {
            fraction = true;
        } else if (c >= '0' && c <= '9') {
            digits = true;
            if (fraction) {
                unit /= 10;
                value += (c - '0') * unit;
            } else {
                value = value * 10 + (c - '0');
            }
        } else {
            return false;
        }
    }
    if (!digits || value > 1.0) return false;
    *out = Round(static_cast<float>(value));
    return true;
}

bool ParseInt(const std::string& s, long* out) {
    if (s.empty() || s.size() > 9) return false;
    long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

std::vector<std::string> Tokens(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) out.push_back(cur), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// "<key> <layer>=<level> ..." -> key and layers; false with the reason.
bool ParseEntry(const std::vector<std::string>& tok, std::string* key, Layers* layers, std::string* why) {
    if (!ValidKey(tok[0])) {
        *why = "bad key";
        return false;
    }
    if (tok.size() < 2) {
        *why = "no layers";
        return false;
    }
    *key = tok[0];
    layers->clear();
    for (size_t i = 1; i < tok.size(); ++i) {
        const size_t eq = tok[i].find('=');
        float level = 0;
        if (eq == std::string::npos || !ValidLayerName(tok[i].substr(0, eq)) ||
            !ParseLevel(tok[i].substr(eq + 1), &level) || !layers->emplace(tok[i].substr(0, eq), level).second) {
            *why = "bad layer '" + tok[i] + "'";
            return false;
        }
    }
    return true;
}

bool AllClean(const Layers& layers) {
    for (const auto& l : layers)
        if (Steps(l.second) != 0) return false;
    return true;
}

}  // namespace

std::string TruckKey(const std::string& id, const std::string& plate) {
    return id.empty() ? std::string() : "truck|" + Escape(id) + "|" + Escape(plate);
}

std::string TrailerKey(const std::string& id, const std::string& plate) {
    return id.empty() ? std::string() : "trailer|" + Escape(id) + "|" + Escape(plate);
}

// ---- Store ----

const Layers* Store::Find(const std::string& key) const {
    for (const auto& e : entries_)
        if (e.first == key) return &e.second;
    return nullptr;
}

void Store::Put(const std::string& key, const Layers& layers) {
    if (!ValidKey(key)) return;
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& e) { return e.first == key; });
    Layers merged = it != entries_.end() ? it->second : Layers();
    for (const auto& l : layers)
        if (ValidLayerName(l.first)) merged[l.first] = Round(l.second);
    if (AllClean(merged)) {
        Erase(key);
        return;
    }
    if (it != entries_.end() && it == entries_.begin() && it->second == merged) return;  // most recent, same levels
    if (it != entries_.end()) entries_.erase(it);
    entries_.insert(entries_.begin(), {key, merged});
    if (entries_.size() > static_cast<size_t>(kMaxEntries)) entries_.resize(kMaxEntries);
    dirty_ = true;
}

void Store::Erase(const std::string& key) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& e) { return e.first == key; });
    if (it == entries_.end()) return;
    entries_.erase(it);
    dirty_ = true;
}

std::string Store::Serialize() const {
    std::string out =
        "# ETS2/ATS Truck Dirt: saved dirt of the player's trucks and own trailers (0 = clean, 1 = covered).\n"
        "# Written by the plugin: edit only with the game closed.\n";
    out += std::string(kHeader) + " " + std::to_string(kFormatVersion) + "\n";
    for (const auto& e : entries_) {
        out += e.first;
        for (const auto& l : e.second)
            if (Steps(l.second)) out += " " + l.first + "=" + FormatLevel(l.second);  // missing = 0
        out += "\n";
    }
    out += "end " + std::to_string(entries_.size()) + "\n";
    return out;
}

ParseReport Store::Deserialize(const std::string& text) {
    entries_.clear();
    read_only_ = false;
    ParseReport r;
    bool header = false, ended = false;
    long lines = 0;  // entry lines, good or bad
    auto damage = [&](const std::string& why) {
        ++r.skipped;
        if (r.detail.empty()) r.detail = why;
    };
    std::istringstream in(text);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> tok = Tokens(line);
        if (tok.empty() || tok[0][0] == '#') continue;
        const std::string where = "line " + std::to_string(number) + ": ";
        if (!header) {
            long version = 0;
            if (tok.size() != 2 || tok[0] != kHeader || !ParseInt(tok[1], &version) || version < 1) {
                r.result = Parse::kUnreadable;
                r.detail = where + "not a dirt_state file";
                break;
            }
            if (version > kFormatVersion) {
                r.result = Parse::kNewer;
                r.detail = "format " + std::to_string(version) + ", this plugin reads up to " +
                           std::to_string(kFormatVersion);
                break;
            }
            header = true;
        } else if (ended) {
            damage(where + "text after the end line");
        } else if (tok[0] == "end") {
            long count = -1;
            ended = true;
            if (tok.size() != 2 || !ParseInt(tok[1], &count) || count != lines)
                damage(where + "the end line does not match the " + std::to_string(lines) + " units");
        } else {
            ++lines;
            std::string key, why;
            Layers layers;
            if (!ParseEntry(tok, &key, &layers, &why)) damage(where + why);
            else if (Find(key)) damage(where + "unit twice");
            else if (!AllClean(layers) && entries_.size() < static_cast<size_t>(kMaxEntries))
                entries_.push_back({key, layers});
        }
    }
    if (r.result == Parse::kOk && !header) {
        r.result = Parse::kUnreadable;
        r.detail = "empty file";
    }
    if (r.result != Parse::kOk) {
        entries_.clear();
        read_only_ = r.result == Parse::kNewer;
        dirty_ = r.result == Parse::kUnreadable;  // the next save writes a good file (the bad one is kept)
        return r;
    }
    if (!ended) {
        ++r.skipped;
        if (r.detail.empty()) r.detail = "no end line: the file was cut";
    }
    if (r.skipped) r.result = Parse::kDamaged;
    r.entries = Size();
    dirty_ = r.result == Parse::kDamaged;
    return r;
}

ParseReport Store::Load(const std::filesystem::path& file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    entries_.clear();
    dirty_ = read_only_ = false;
    if (!fs::exists(file, ec)) return ParseReport();  // first run: everything clean
    ParseReport r;
    const std::uintmax_t size = fs::file_size(file, ec);
    std::ifstream in(file, std::ios::binary);
    if (ec || !in) {
        // Not ours to overwrite blindly: it may be fine and only locked.
        read_only_ = true;
        r.result = Parse::kUnreadable;
        r.detail = "cannot open the file";
        return r;
    }
    if (size > kMaxFileBytes) {
        r.result = Parse::kUnreadable;
        r.detail = "file too big (" + std::to_string(size) + " bytes)";
        dirty_ = true;
    } else {
        std::ostringstream text;
        text << in.rdbuf();
        r = Deserialize(text.str());
    }
    in.close();
    if (r.result == Parse::kDamaged || r.result == Parse::kUnreadable) {
        fs::path bad = file;
        bad += ".bad";
        fs::copy_file(file, bad, fs::copy_options::overwrite_existing, ec);
        if (ec) r.detail += " (copy to .bad failed: " + ec.message() + ")";
    }
    return r;
}

bool Store::Save(const std::filesystem::path& file, std::string* error) {
    if (read_only_) {
        if (error) *error = "the file is of a newer format or could not be read: left alone";
        return false;
    }
    if (!WriteText(file, Serialize(), error)) return false;
    dirty_ = false;
    return true;
}

bool Store::WriteText(const std::filesystem::path& file, const std::string& text, std::string* error) {
    namespace fs = std::filesystem;
    fs::path tmp = file;
    tmp += ".tmp";
    std::error_code ec;
    {
        // D11o (30/10: a blue screen a minute after the game closed left the file saved at its shutdown as 459 zero
        // bytes: the text was still in Windows' cache): the text on the disk itself (_commit / fsync) before the
        // rename, so a crash leaves the old file or the new one, never zeros.
#ifdef _WIN32
        FILE* f = _wfopen(tmp.c_str(), L"wb");
#else
        FILE* f = fopen(tmp.c_str(), "wb");
#endif
        bool ok = f && fwrite(text.data(), 1, text.size(), f) == text.size() && fflush(f) == 0;
#ifdef _WIN32
        ok = ok && _commit(_fileno(f)) == 0;
#else
        ok = ok && fsync(fileno(f)) == 0;
#endif
        if (f && fclose(f) != 0) ok = false;
        if (!ok) {
            fs::remove(tmp, ec);
            if (error) *error = "cannot write " + tmp.filename().string();
            return false;
        }
    }
    // Replaces the old file in one step (MoveFileEx with MOVEFILE_REPLACE_EXISTING on Windows).
    fs::rename(tmp, file, ec);
    if (ec) {
        if (error) *error = "cannot replace the file: " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// ---- who is saved ----

Kind TruckKind(const Rig& rig) {
    if (rig.truck_id.empty()) return Kind::kNone;
    return rig.job_market == "quick_job" ? Kind::kCompany : Kind::kPlayer;
}

Kind TrailerKind(const Rig& rig) {
    if (rig.trailer_id.empty()) return Kind::kNone;
    return rig.job_market.empty() || rig.job_market == "cargo_market" ? Kind::kPlayer : Kind::kCompany;
}

void Tracker::Put(const std::string& key, const Layers& layers) {
    // A company unit seen as the player's for a frame (config order) must not
    // end up in the file; a unit already there is the player's.
    if (company_.count(key) && !store_->Find(key)) return;
    store_->Put(key, layers);
}

// The outgoing unit keeps its dirt: in the file or in this session.
void Tracker::Keep(int unit, const Layers& current) {
    if (kind_[unit] == Kind::kPlayer) Put(key_[unit], current);
    else if (kind_[unit] == Kind::kCompany) session_[key_[unit]] = current;
}

unsigned Tracker::Sync(const Rig& rig, const Layers current[kUnits], Layers start[kUnits]) {
    const std::string keys[kUnits] = {TruckKey(rig.truck_id, rig.truck_plate),
                                      TrailerKey(rig.trailer_id, rig.trailer_plate)};
    const Kind kinds[kUnits] = {TruckKind(rig), TrailerKind(rig)};
    unsigned changed = 0;
    for (int u = 0; u < kUnits; ++u) {
        if (keys[u] == key_[u] && kinds[u] == kind_[u]) continue;
        if (kinds[u] == Kind::kCompany) company_.insert(keys[u]);
        if (keys[u] == key_[u]) {
            // Same unit, other kind: it keeps its dirt (saved first if it was the player's).
            if (kind_[u] == Kind::kPlayer) Put(key_[u], current[u]);
            kind_[u] = kinds[u];
            continue;
        }
        Keep(u, current[u]);
        key_[u] = keys[u];
        kind_[u] = kinds[u];
        start[u].clear();
        if (kinds[u] == Kind::kPlayer) {
            if (const Layers* saved = store_->Find(keys[u])) start[u] = *saved;
        } else if (kinds[u] == Kind::kCompany) {
            const auto it = session_.find(keys[u]);
            if (it != session_.end()) start[u] = it->second;
        }
        changed |= 1u << u;
    }
    return changed;
}

void Tracker::Commit(const Layers current[kUnits]) {
    for (int u = 0; u < kUnits; ++u)
        if (kind_[u] == Kind::kPlayer) Put(key_[u], current[u]);
}

}  // namespace dirt_state
