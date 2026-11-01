// N47: offline test of the rig maps (rig::ExportMap / rig::ImportMap) on the V3a captures.
//
// Arguments in pairs: <learnt from> <opened>. The rig is learnt from the first capture driving (three frames,
// both units moving 2 m a frame), its maps written; then the second capture is replayed with the rig parked
// (three frames, no move), as when the game opens:
//  - "drive": the second capture learnt from scratch driving, the reference: which draws are the units';
//  - "parked": parked with no map: only the wheels (a box draw waits for 1 m of driving);
//  - "map": parked with the first capture's maps: the box draws join at once.
// In "map", made-up draws beside the units' box draws (the same index count 8 cm off, or the same place with
// another index count: grass, road-side meshes) are claimed only where they are without the map (near a wheel,
// a wheel's part), and no draw the reference gives to no unit or to the other unit is. The run1 and run2 captures are two sessions of the game with the same rig (the buffers
// and their offsets differ): run1 -> run2 is the real case; a capture with itself the best one.
// Also: a map of another key, or of another format, is not read; one written back keeps the parts not found
// again, aged by the distance driven.
// Usage: maptest <capture.csv> <capture.csv> [...]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rig_tracker.h"

namespace {

struct Draw {
    int view;
    rig::DrawKey key;
    float m[12];
    bool interior;
    bool main = false;  // N55: in the camera's own view (with interior: the cab's inside), not a mirror's
};

struct Capture {
    std::vector<Draw> draws;
    rig::UnitConfig cfg[rig::kUnits];
    double place[rig::kUnits][6] = {};
    bool placed[rig::kUnits] = {};
    float defl[rig::kUnits][rig::kMaxWheels] = {};
    bool cab = false;
    float pivot[3] = {}, off[6] = {};
    int group[rig::kMaxViews] = {};
};

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        const size_t j = s.find(',', i);
        out.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return out;
}

bool Load(const char* path, Capture& c) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    int view_w[rig::kMaxViews] = {}, view_h[rig::kMaxViews] = {};
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        const auto v = Split(s);
        if (v[0] == "placement") {
            const int u = atoi(v[1].c_str());
            for (int i = 0; i < 6; ++i) c.place[u][i] = atof(v[2 + i].c_str());
            c.placed[u] = v.size() < 9 || v[8] == "1";
        } else if (v[0] == "unit") {
            rig::UnitConfig& uc = c.cfg[atoi(v[1].c_str())];
            uc.wheels = static_cast<int>((v.size() - 2) / 3);
            for (int w = 0; w < uc.wheels; ++w)
                for (int k = 0; k < 3; ++k) uc.wheel[w][k] = static_cast<float>(atof(v[2 + w * 3 + k].c_str()));
        } else if (v[0] == "hook") {
            rig::UnitConfig& uc = c.cfg[atoi(v[1].c_str())];
            uc.has_hook = true;
            for (int k = 0; k < 3; ++k) uc.hook[k] = static_cast<float>(atof(v[2 + k].c_str()));
        } else if (v[0] == "cab" && v.size() >= 10) {
            c.cab = true;
            for (int i = 0; i < 3; ++i) c.pivot[i] = static_cast<float>(atof(v[1 + i].c_str()));
            for (int i = 0; i < 6; ++i) c.off[i] = static_cast<float>(atof(v[4 + i].c_str()));
        } else if (v[0] == "deflection") {
            const int u = atoi(v[1].c_str());
            for (size_t w = 2; w < v.size() && w - 2 < rig::kMaxWheels; ++w)
                c.defl[u][w - 2] = static_cast<float>(atof(v[w].c_str()));
        } else if (v[0] == "view" && v.size() >= 4) {
            const int id = atoi(v[1].c_str());
            if (id >= 0 && id < rig::kMaxViews) view_w[id] = atoi(v[2].c_str()), view_h[id] = atoi(v[3].c_str());
        } else if (v[0] == "draw" && v.size() >= 19) {
            Draw d;
            d.view = atoi(v[1].c_str());
            d.key.vb = reinterpret_cast<const void*>(strtoull(v[2].c_str(), nullptr, 16));
            d.key.ib = reinterpret_cast<const void*>(strtoull(v[3].c_str(), nullptr, 16));
            d.key.start = static_cast<uint32_t>(atoll(v[4].c_str()));
            d.key.base = atoi(v[5].c_str());
            d.key.count = static_cast<uint32_t>(atoll(v[6].c_str()));
            for (int i = 0; i < 12; ++i) d.m[i] = static_cast<float>(atof(v[7 + i].c_str()));
            d.interior = v.size() >= 20 && atoi(v[19].c_str()) == 2;
            c.draws.push_back(d);
        }
    }
    fclose(f);
    for (int i = 0; i < rig::kMaxViews; ++i) {  // group = the first view of the same size
        c.group[i] = i;
        for (int j = 0; j < i; ++j)
            if (view_w[i] && view_w[j] == view_w[i] && view_h[j] == view_h[i]) {
                c.group[i] = j;
                break;
            }
    }
    return true;
}

const char* kKey[2] = {"test truck", "test trailer"};

// A new session with this capture's rig: the layouts again (forgetting what was learnt) and the maps.
void Open(const Capture& c, const std::string maps[2]) {
    for (int u = 0; u < rig::kUnits; ++u) {
        rig::SetUnit(u, rig::UnitConfig());
        rig::SetUnit(u, c.cfg[u]);
        rig::SetDeflection(u, c.defl[u], c.cfg[u].wheels);
        if (u < 2) rig::ImportMap(u, kKey[u], maps[u]);
    }
    rig::SetCabin(c.cab, c.pivot);
    rig::SetCabinOffset(c.off);
}

// Replays the capture `frames` times (driving: the units move 2 m a frame); the last frame's unit per draw, and
// the made-up draws' (`extra`, drawn after the capture's).
void Replay(const Capture& c, int frames, bool driving, std::vector<int>* unit, const std::vector<Draw>& extra,
            std::vector<int>* extra_unit) {
    for (int frame = 0; frame < frames; ++frame) {
        for (int u = 0; u < rig::kUnits; ++u) {
            double p[6];
            for (int i = 0; i < 6; ++i) p[i] = c.place[u][i];
            if (driving) p[0] += 2.0 * frame, p[2] += 2.0 * frame;
            rig::SetPlacement(u, p, c.placed[u]);
        }
        rig::BeginFrame();
        const bool last = frame == frames - 1;
        if (last) unit->assign(c.draws.size(), -1), extra_unit->assign(extra.size(), -1);
        for (size_t i = 0; i < c.draws.size(); ++i) {
            const Draw& d = c.draws[i];
            const int got = rig::Classify(d.view, c.group[d.view], d.key, d.m, d.interior, nullptr, d.main);
            if (last) (*unit)[i] = got;
        }
        for (size_t i = 0; i < extra.size(); ++i) {
            const Draw& d = extra[i];
            const int got = rig::Classify(d.view, c.group[d.view], d.key, d.m, d.interior, nullptr, d.main);
            if (last) (*extra_unit)[i] = got;
        }
        rig::EndFrame();
    }
}

int Count(const std::vector<int>& unit, int u) {
    int n = 0;
    for (int x : unit) n += x == u ? 1 : 0;
    return n;
}

int Lines(const std::string& text) {
    int n = 0;
    for (char ch : text) n += ch == '\n' ? 1 : 0;
    return n > 2 ? n - 2 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    int failures = 0;
    const std::vector<Draw> none;
    std::vector<int> dummy;
    for (int a = 1; a + 1 < argc; a += 2) {
        Capture learnt, opened;
        if (!Load(argv[a], learnt) || !Load(argv[a + 1], opened)) {
            printf("FAIL: cannot open %s or %s\n", argv[a], argv[a + 1]);
            return 1;
        }
        const char* na = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
        const char* nb = strrchr(argv[a + 1], '/') ? strrchr(argv[a + 1], '/') + 1 : argv[a + 1];
        printf("%s -> %s\n", na, nb);
        const std::string empty[2];
        // The first session: learnt driving, the maps written.
        std::vector<int> unit;
        Open(learnt, empty);
        Replay(learnt, 3, true, &unit, none, &dummy);
        rig::WriteMaps();
        std::string maps[2];
        for (int u = 0; u < 2; ++u) {
            std::string key;
            std::vector<rig::MapPart> parts;
            const bool got = rig::ExportMap(u, &key, &parts, nullptr);
            maps[u] = rig::MapText(key, parts);
            if (!got || key != kKey[u]) {
                printf("  FAIL: no map for unit %d\n", u);
                ++failures;
            }
        }
        printf("  map: truck %d parts, trailer %d\n", Lines(maps[0]), Lines(maps[1]));
        if (getenv("MAPTEST_DUMP")) printf("%s%s", maps[0].c_str(), maps[1].c_str());
        // The reference: the second capture learnt driving.
        std::vector<int> ref;
        Open(opened, empty);
        Replay(opened, 3, true, &ref, none, &dummy);
        // Parked, no map (first without the made-up draws, to tell the box draws from the wheels').
        std::vector<int> parked, parked_extra;
        Open(opened, empty);
        Replay(opened, 3, false, &parked, none, &dummy);
        // The made-up draws beside the reference's box draws.
        std::vector<Draw> extra;
        for (size_t i = 0; i < opened.draws.size(); ++i) {
            if (ref[i] < 0 || ref[i] > 1 || parked[i] == ref[i]) continue;  // a box draw (not a wheel's)
            Draw beside = opened.draws[i];
            beside.key.vb = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x7f0000000000ull + i));
            // 8 cm off, along the camera's x, y or z: the first way with no draw of the same mesh near (a part may be
            // repeated 10 cm away: two lamps, the made-up draw on the other one).
            for (int axis = 0; axis < 3; ++axis) {
                const float* m = opened.draws[i].m;
                const float p[3] = {m[3] + (axis == 0 ? 0.08f : 0.f), m[7] + (axis == 1 ? 0.08f : 0.f),
                                    m[11] + (axis == 2 ? 0.08f : 0.f)};
                bool twin = false;
                for (const Draw& o : opened.draws) {
                    const float dx = o.m[3] - p[0], dy = o.m[7] - p[1], dz = o.m[11] - p[2];
                    twin = twin || (o.view == opened.draws[i].view && o.key.count == opened.draws[i].key.count &&
                                    dx * dx + dy * dy + dz * dz < 0.06f * 0.06f);
                }
                if (twin && axis < 2) continue;
                beside.m[3] = p[0], beside.m[7] = p[1], beside.m[11] = p[2];
                break;
            }
            extra.push_back(beside);
            Draw other = opened.draws[i];
            other.key.vb = beside.key.vb;
            other.key.count += 1000003;  // the same place and rotation, a mesh no part has (a part is often several
                                         // draws at one pose, its materials: +6 hit a real one)
            extra.push_back(other);
        }
        // Near a wheel a made-up draw joins at once with or without a map (a wheel's part): only the ones claimed
        // with the map and not without it count.
        Open(opened, empty);
        Replay(opened, 3, false, &dummy, extra, &parked_extra);
        const long long restored0 = rig::GetStats().restored, inherited0 = rig::GetStats().inherited;
        std::vector<int> mapped, extra_unit;
        Open(opened, maps);
        Replay(opened, 3, false, &mapped, extra, &extra_unit);
        const rig::Stats st = rig::GetStats();
        int wrong = 0, extra_claims = 0;
        for (size_t i = 0; i < mapped.size(); ++i) wrong += mapped[i] >= 0 && mapped[i] != ref[i] ? 1 : 0;
        for (size_t i = 0; i < extra_unit.size(); ++i) extra_claims += extra_unit[i] >= 0 && parked_extra[i] < 0 ? 1 : 0;
        if (getenv("MAPTEST_TRACE")) printf("  inherited %lld\n", st.inherited - inherited0);
        if (getenv("MAPTEST_TRACE"))
            for (size_t i = 0; i < extra_unit.size(); ++i)
                if (extra_unit[i] >= 0 && parked_extra[i] < 0)
                    printf("  made-up %zu (%s) count %u -> %d\n", i, i % 2 ? "other count" : "beside", extra[i].key.count, extra_unit[i]);
        printf("  draws claimed (truck/trailer): drive %d/%d, parked %d/%d, parked with the map %d/%d\n", Count(ref, 0),
               Count(ref, 1), Count(parked, 0), Count(parked, 1), Count(mapped, 0), Count(mapped, 1));
        printf("  restored %lld, map parts found %d/%d and %d/%d, wrong claims %d, made-up claims %d of %zu\n",
               st.restored - restored0, st.map_used[0], st.map[0], st.map_used[1], st.map[1], wrong, extra_claims,
               extra.size());
        if (wrong || extra_claims) {
            printf("  FAIL: claims the reference does not make\n");
            ++failures;
        }
        // (A capture with the rig out of sight claims nothing in any case.)
        if (Count(ref, 0) + Count(ref, 1) > Count(parked, 0) + Count(parked, 1) &&
            Count(mapped, 0) + Count(mapped, 1) <= Count(parked, 0) + Count(parked, 1)) {
            printf("  FAIL: the map claims no more than the parked rig without it\n");
            ++failures;
        }
        // Written back: the parts found again as members, the others kept (aged by the distance driven: none here).
        rig::WriteMaps();
        std::string key;
        std::vector<rig::MapPart> parts;
        rig::ExportMap(0, &key, &parts, nullptr);
        const std::string again = rig::MapText(key, parts);
        printf("  written back: truck %d parts\n", Lines(again));
        if (Lines(again) < st.map[0]) {
            printf("  FAIL: the truck's map lost parts\n");
            ++failures;
        }
    }
    // D12b: the game loads the truck's meshes into other buffers again and again (a Volvo VNL 2025 in the ATS, 30/10:
    // known 100 -> 3851, the rig's end-frame 5-47 ms). The first capture learnt driving, then parked for 120 frames,
    // every box draw of the truck in a new buffer each 4 frames (the same index count at the same place, a new key):
    // its known members stop growing after the first rounds (two copies of a part) and its draws stay claimed.
    if (argc >= 2) {
        Capture c;
        Load(argv[1], c);
        const std::string empty[2];
        std::vector<int> unit, extra_unit;
        Open(c, empty);
        Replay(c, 3, true, &unit, none, &dummy);
        const int learnt = rig::GetStats().known[0], claimed = Count(unit, 0);
        std::vector<bool> box(c.draws.size(), false);
        {
            std::vector<int> parked;
            Open(c, empty);
            Replay(c, 3, true, &unit, none, &dummy);  // learnt again, the same
            Replay(c, 3, false, &parked, none, &dummy);
        }
        std::vector<int> wheels;
        {
            Capture still = c;
            Open(still, empty);
            Replay(still, 3, false, &wheels, none, &dummy);  // parked from the start: the wheels' draws only
        }
        Open(c, empty);
        Replay(c, 3, true, &unit, none, &dummy);
        for (size_t i = 0; i < c.draws.size(); ++i) box[i] = unit[i] == 0 && wheels[i] != 0;
        rig::WriteMaps();
        std::string maps[2];
        for (int u = 0; u < 2; ++u) {
            std::string key;
            std::vector<rig::MapPart> parts;
            rig::ExportMap(u, &key, &parts, nullptr);
            maps[u] = rig::MapText(key, parts);
        }
        // D13: the same opened with the maps, parked: the parts join from the map in every new buffer (the ATS lowboy,
        // 30/10: known 241 -> 757); the older copies must go there too.
        for (int from_map = 0; from_map < 2; ++from_map) {
            Open(c, from_map ? maps : empty);
            Replay(c, 3, !from_map, &unit, none, &dummy);
            int most = 0, lost = 0, settled = 0;
            const long long reloaded0 = rig::GetStats().reloaded, restored0 = rig::GetStats().restored;
            Capture moved = c;
            for (int round = 0; round < 30; ++round) {
                for (size_t i = 0; i < moved.draws.size(); ++i)
                    if (box[i]) moved.draws[i].key.vb = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x7d0000000000ull + i * 64 + round));
                Replay(moved, 4, false, &unit, none, &dummy);
                if (round == 4) settled = rig::GetStats().known[0];
                if (round > 4) most = (std::max)(most, rig::GetStats().known[0]);
                if (getenv("MAPTEST_TRACE")) printf(" %d/%d/%lld", rig::GetStats().known[0], Count(unit, 0), rig::GetStats().reloaded);
                if (round >= 1 && Count(unit, 0) < claimed) ++lost;
            }
            printf("reloads%s: truck learnt %d members (%d draws claimed), %d box draws in a new buffer every 4 frames for "
                   "120 frames: known %d after 5 rounds, at most %d after, reloaded %lld, restored %lld, rounds with draws "
                   "lost %d\n", from_map ? " (opened with the map, parked)" : "", learnt, claimed,
                   static_cast<int>(std::count(box.begin(), box.end(), true)), settled, most,
                   rig::GetStats().reloaded - reloaded0, rig::GetStats().restored - restored0, lost);
            if (most > settled + settled / 10 || lost) {  // two copies of a part at most: no growth past the first rounds
                printf("FAIL: the reloaded meshes pile up or lose the truck's draws\n");
                ++failures;
            }
        }
        // N55: 13 of the truck's box parts drawn again with ref 2 (the interior camera's), at the same place; 12 in the
        // main view only (the cab's inside), the 13th in a mirror too (the cab's shell). After the looks (every 4
        // frames, 3 votes) the 12 count as inside, the 13th not; the map carries them over. D14d: all 13 keep their
        // dust in the outside view (a part drawn there at its inside place is the hood or the cab's shell).
        {
            Open(c, empty);
            Replay(c, 3, true, &unit, none, &dummy);
            std::vector<size_t> picked;
            std::vector<uint32_t> counts;  // one view, one draw per mesh (no twin at the same count)
            int view0 = -1;
            for (size_t i = 0; i < c.draws.size() && picked.size() < 13; ++i) {
                const Draw& d = c.draws[i];
                if (!box[i] || unit[i] != 0 || !d.key.count || (view0 >= 0 && d.view != view0) ||
                    std::find(counts.begin(), counts.end(), d.key.count) != counts.end())
                    continue;
                view0 = d.view;
                counts.push_back(d.key.count);
                picked.push_back(i);
            }
            // D14c: the main view's draws in a view of their own where the truck is not tracked (the interior camera's:
            // nothing of the truck but the cabin there), its camera turned 30 degrees and moved; the shell's mirror draw
            // in the capture's view (tracked).
            const float cs = std::cos(0.5236f), sn = std::sin(0.5236f);
            const float rot[3][3] = {{cs, 0, sn}, {0, 1, 0}, {-sn, 0, cs}}, mv[3] = {1.f, 2.f, 3.f};
            std::vector<Draw> cabin;
            for (size_t k = 0; k < picked.size(); ++k) {
                const Draw& d = c.draws[picked[k]];
                if (k == 12) {  // the shell: seen in a mirror too
                    Draw mirror = d;
                    mirror.interior = true, mirror.main = false;
                    cabin.push_back(mirror);
                }
                Draw own = d;
                own.interior = true, own.main = true, own.view = rig::kMaxViews - 1;
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 4; ++j)
                        own.m[i * 4 + j] = rot[i][0] * d.m[j] + rot[i][1] * d.m[4 + j] + rot[i][2] * d.m[8 + j] + (j == 3 ? mv[i] : 0.f);
                cabin.push_back(own);
            }
            std::vector<int> cabin_unit;
            Replay(c, 16, false, &unit, cabin, &cabin_unit);
            Replay(c, 2, false, &unit, none, &dummy);
            int inside = 0, shell = -2;
            for (size_t k = 0; k < picked.size(); ++k) {
                if (k < 12) inside += unit[picked[k]] == -1 ? 1 : 0;
                else shell = unit[picked[k]];
            }
            const rig::Stats st = rig::GetStats();
            printf("cab inside (N55): %zu parts drawn with ref 2, %d left without dust outside (of 12; D14d: 0), the mirror's one "
                   "unit %d; inside parts %d of %d seen; main view looks %lld, placed %lld (D14c: the truck not tracked "
                   "there)\n", picked.size(), inside, shell, st.inside_parts, st.inside_all, st.inside_looks, st.inside_placed);
            rig::WriteMaps();
            std::string key, text;
            std::vector<rig::MapPart> parts;
            rig::ExportMap(0, &key, &parts, nullptr);
            text = rig::MapText(key, parts);
            int lines = 0;
            for (size_t pos = 0; (pos = text.find("\ninside ", pos)) != std::string::npos; ++pos) ++lines;
            std::string with[2] = {text, empty[1]};
            Open(c, with);
            Replay(c, 3, true, &unit, none, &dummy);
            int again = 0;
            for (size_t k = 0; k < 12 && k < picked.size(); ++k) again += unit[picked[k]] == -1 ? 1 : 0;
            printf("cab inside (N55): %d inside lines in the map; opened with it, %d of 12 without dust (D14d: 0)\n",
                   lines, again);
            if (picked.size() < 13 || inside != 0 || shell != 0 || st.inside_parts != 12 || lines != 13 || again != 0) {
                printf("FAIL: the cab's inside parts\n");
                ++failures;
            }
        }
    }
    // Another key's map, another format, a broken line: not read.
    std::string text = std::string("truck dirt rig map 1\nkey test truck\n") +
                       "12 0 1 0 1 0 0 0.1 0 1 0 0.2 0 0 1 0.3 1 0 0 0.1 0 1 0 0.2 0 0 1 0.3\n" + "12 0 1 0 1 0 0\n";
    const int good = rig::ImportMap(0, "test truck", text), other = rig::ImportMap(0, "another truck", text),
              format = rig::ImportMap(0, "test truck", "truck dirt rig map 2\nkey test truck\n" + text.substr(36));
    printf("maps read: %d part(s) of 2 lines, another key %d, another format %d\n", good, other, format);
    if (good != 1 || other || format) {
        printf("FAIL: map reading\n");
        ++failures;
    }
    printf(failures ? "FAILED (%d)\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
