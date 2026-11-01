#include "cargo_dirt.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace cargo {

bool IsLevel(int slot) { return (slot >= dust::kWetMud && slot <= dust::kSalt) || slot == dust::kSoot; }

bool Step(State* s, const std::string& key, float trailer_level, const dust::Layers& trailer, float* level,
          dust::Layers* out) {
    bool fresh = false;
    if (!key.empty() && key != s->key) {  // a new cargo: clean, the trailer's dirt now its base
        s->key = key;
        s->base_level = trailer_level;
        for (int k = 0; k < dust::kSlots; ++k) s->base_body[k] = trailer.body[k], s->base_front[k] = trailer.front[k];
        fresh = true;
    }
    s->base_level = (std::min)(s->base_level, trailer_level);  // a wash takes the base down with the trailer
    *level = trailer_level - s->base_level;
    *out = trailer;
    for (int k = 0; k < dust::kSlots; ++k) {
        if (!IsLevel(k)) continue;
        s->base_body[k] = (std::min)(s->base_body[k], trailer.body[k]);
        s->base_front[k] = (std::min)(s->base_front[k], trailer.front[k]);
        out->body[k] = trailer.body[k] - s->base_body[k];
        out->front[k] = trailer.front[k] - s->base_front[k];
    }
    return fresh;
}

std::string Serialize(const State& s) {
    std::string t = "truckdirt_cargo 1\nkey " + s.key + "\n";
    char b[64];
    snprintf(b, sizeof(b), "level %.5f\n", s.base_level);
    t += b;
    for (const auto& row : {std::make_pair("body", s.base_body), std::make_pair("front", s.base_front)}) {
        t += row.first;
        for (int k = 0; k < dust::kSlots; ++k) snprintf(b, sizeof(b), " %.5f", row.second[k]), t += b;
        t += "\n";
    }
    return t;
}

bool Parse(const std::string& text, State* s) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != "truckdirt_cargo 1") return false;
    State r;
    int rows = 0;
    while (std::getline(in, line)) {
        if (line.rfind("key ", 0) == 0) {
            r.key = line.substr(4);
        } else if (line.rfind("level ", 0) == 0) {
            if (sscanf(line.c_str() + 6, "%f", &r.base_level) != 1) return false;
            ++rows;
        } else if (line.rfind("body ", 0) == 0 || line.rfind("front ", 0) == 0) {
            const bool body = line[0] == 'b';
            float* v = body ? r.base_body : r.base_front;
            std::istringstream row(line.substr(body ? 5 : 6));
            for (int k = 0; k < dust::kSlots; ++k)
                if (!(row >> v[k])) return false;
            ++rows;
        }
    }
    if (rows != 3 || r.key.empty()) return false;
    *s = r;
    return true;
}

}  // namespace cargo
