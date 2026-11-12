#include "premap.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace premap {
namespace {

struct Piece {
    float lo[3], hi[3];
    int kind;
    bool merged = false;  // N58c: a variant's pieces drawn as one ("m" line): its size only, its place moved
};
// Index count and vertex count -> the pieces with them. Written at load (before the game draws), read on the render
// thread.
std::unordered_map<uint64_t, std::vector<Piece>> g_pieces;
std::unordered_set<uint32_t> g_counts;
int g_count = 0;
// N68: the wiper blades, by truck id. Written at load, read on the render thread.
std::unordered_map<std::string, std::vector<Blade>> g_blades;
constexpr float kTol = 0.003f;  // m: the table keeps 4 decimals

uint64_t Key(uint32_t count, uint32_t verts) { return (uint64_t(count) << 32) | verts; }

// N68: "1.90:2.00,1.90:0.00,1.00:0.00" -> the three speeds' stroke and pause.
void ReadRhythm(const char* s, Blade* b) {
    for (int i = 0; i < kSpeeds && s && *s; ++i) {
        float stroke = 0.f, pause = 0.f;
        if (sscanf(s, "%f:%f", &stroke, &pause) != 2) return;
        b->stroke[i] = stroke, b->pause[i] = pause;
        const char* comma = strchr(s, ',');
        s = comma ? comma + 1 : nullptr;
    }
}

int KindOf(const char* s) {
    if (!strcmp(s, "int_ext")) return kIntExt;
    if (!strcmp(s, "int")) return kInt;
    if (!strcmp(s, "ext")) return kExt;
    if (!strcmp(s, "cargo")) return kCargo;
    return kNone;
}

}  // namespace

int Parse(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("truck dirt premap 1", 0) != 0) return 0;
    int taken = 0;
    while (std::getline(in, line)) {
        if (line.size() > 2 && line[0] == 'b' && line[1] == ' ') {  // N68: a wiper blade
            char tid[64] = {}, rhythm[48] = {}, model[96] = {};
            int idx = 0;
            Blade b;
            if (sscanf(line.c_str() + 1, " %63s %d %d %d %f %f %f %f %f %f %f %f %f %f %f %47s %95s", tid, &idx,
                       &b.arm_bone, &b.blade_bone, &b.pivot[0], &b.pivot[1], &b.pivot[2], &b.axis[0], &b.axis[1],
                       &b.axis[2], &b.arm[0], &b.arm[1], &b.arm[2], &b.length, &b.sweep, rhythm, model) != 17)
                continue;
            ReadRhythm(rhythm, &b);
            snprintf(b.model, sizeof(b.model), "%s", model);
            auto& v = g_blades[tid];
            if (v.size() < 32) v.push_back(b);
            continue;
        }
        if (line.size() < 2 || (line[0] != 'p' && line[0] != 'm') || line[1] != ' ') continue;
        unsigned count = 0, verts = 0;
        Piece p = {};
        p.merged = line[0] == 'm';
        char kind[16] = {};
        if (sscanf(line.c_str() + 1, " %u %u %f %f %f %f %f %f %15s", &count, &verts, &p.lo[0], &p.lo[1], &p.lo[2],
                   &p.hi[0], &p.hi[1], &p.hi[2], kind) != 9)
            continue;
        p.kind = KindOf(kind);
        if (!count || !verts || p.kind == kNone) continue;
        g_pieces[Key(count, verts)].push_back(p);
        g_counts.insert(count);
        ++taken;
    }
    g_count += taken;
    return taken;
}

// The folder listing is Windows only (the plugin's); the portable tests use Parse.
int LoadDir(const std::wstring& dir, const std::wstring& prefix) {
    int taken = 0;
#ifdef _WIN32
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW((dir + L"\\" + prefix + L"*.premap").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::ifstream f(dir + L"\\" + fd.cFileName, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        taken += Parse(ss.str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    (void)dir;
    (void)prefix;
#endif
    return taken;
}

int Blades(const char* truck_id, float pane_y, float pane_z, Blade out[kMaxBlades]) {
    if (!truck_id || !*truck_id) return 0;
    std::string id = truck_id;
    if (id.rfind("vehicle.", 0) == 0) id = id.substr(8);  // telemetry says "vehicle.volvo.fh_2024"
    const auto it = g_blades.find(id);
    if (it == g_blades.end() || it->second.empty()) return 0;
    // A truck has one set of blades per wiper model (one per cabin). The windscreen the plugin measured says
    // which cabin is on the truck: the right set has its pivots at that glass.
    const char* best = nullptr;
    float best_off = 1e9f;
    for (const Blade& b : it->second) {
        float off = 0.f;
        int n = 0;
        for (const Blade& c : it->second)
            if (!strcmp(c.model, b.model)) off += std::fabs(c.pivot[1] - pane_y) + std::fabs(c.pivot[2] - pane_z), ++n;
        off = n ? off / float(n) : 1e9f;
        if (!(pane_y != 0.f || pane_z != 0.f)) off = 0.f;  // nothing measured: the first set will do
        if (off < best_off) best_off = off, best = b.model;
        if (!(pane_y != 0.f || pane_z != 0.f)) break;
    }
    int n = 0;
    for (const Blade& b : it->second) {
        if (n >= kMaxBlades) break;
        if (best && strcmp(b.model, best)) continue;
        out[n++] = b;
    }
    return n;
}

int BladeTrucks() { return int(g_blades.size()); }

void Clear() {
    g_pieces.clear();
    g_counts.clear();
    g_blades.clear();
    g_count = 0;
}

int Pieces() { return g_count; }

bool CountKnown(uint32_t count) { return g_counts.count(count) != 0; }

int Lookup(uint32_t count, uint32_t verts, const float lo[3], const float hi[3], bool* by_size) {
    if (by_size) *by_size = false;
    const auto it = g_pieces.find(Key(count, verts));
    if (it == g_pieces.end()) return kNone;
    // The box where the model has it; N57c: else, for a cargo, its size anywhere (a cargo's vertices come moved as one [verified:
    // ATS 31/10, N57b, the JCB 457's cargo pieces 1182/534, 2463/1135, 30651/15915 read 0.60 m up and 0.36 m back
    // from their model's boxes, every one alike]).
    for (const bool size_only : {false, true}) {
        int kind = kNone;
        bool any = false, all_merged = true;
        for (const Piece& p : it->second) {
            if (p.merged && !size_only) continue;  // N58c: a merged mesh's place is not its pieces'
            bool close = true;
            for (int i = 0; i < 3 && close; ++i)
                close = size_only ? std::fabs((p.hi[i] - p.lo[i]) - (hi[i] - lo[i])) < kTol
                                  : std::fabs(p.lo[i] - lo[i]) < kTol && std::fabs(p.hi[i] - hi[i]) < kTol;
            if (!close) continue;
            if (any && kind != p.kind) return kNone;  // two kinds: not sure
            kind = p.kind, any = true, all_merged = all_merged && p.merged;
        }
        // By size only a cargo: the interior seen from outside keeps its exact box (no outside piece loses its dust
        // to a piece of the same size elsewhere; 3 ATS and 1 ETS2 interior sizes are shared with another kind).
        // N58c: or a variant's pieces drawn as one, any kind (a sum of counts and vertices, then three sizes).
        if (any && size_only && kind != kCargo && !all_merged) return kNone;
        if (any) {
            if (by_size) *by_size = size_only;
            return kind;
        }
    }
    return kNone;
}

const char* KindName(int kind) {
    switch (kind) {
        case kExt: return "ext";
        case kInt: return "int";
        case kIntExt: return "int_ext";
        case kCargo: return "cargo";
        default: return "none";
    }
}

}  // namespace premap
