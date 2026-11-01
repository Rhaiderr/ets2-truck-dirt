// Offline test of the rig tracker on V3a captures (CSV from tools/v3a/export_rig.py).
// Each capture is replayed as three identical frames (four with RIGTEST_HOLD) while both units move
// 2 m per frame in the world: the first finds the player's units and learns
// the wheels, the second learns the box parts that moved with them, the third
// must claim their draws at draw time. Checks:
//  - nothing is claimed in the first frame (nothing known yet);
//  - interior draws (stencil ref 2) are never claimed;
//  - RIGTEST_STATIC=1: the units do not move, so only wheels may be learned
//    (a stopped truck must not learn the grass around it);
//  - RIGTEST_HOLD=1: in the third frame the truck's rigid draws are left out
//    (a mirror showing only a wheel's edge): the rest must still be claimed;
//  - RIGTEST_SWAP=1: in the third frame the views of one viewport size trade
//    ids (two mirrors sharing a viewport drawn in the other order);
//  - RIGTEST_TURN=<degrees>: the camera turns this much about its vertical
//    axis every frame (a fast camera move), four frames: the last frame must
//    claim as many draws as the one before.
//  - the plain run, captures with a "cab" line: the cab check (N15), the draws
//    at the cab's frame given the capture's own cab offset (see below);
//  - RIGTEST_CAB=1 (captures with a "cab" line): from the fourth frame on the
//    cab rolls and pitches about its pivot (about 1 degree, changing every
//    frame) and the telemetry says so; the truck's draws in front of the
//    trailer and above 1.05 m (the cab) move with it. Eight frames: the cab's
//    draws must be learnt as such and every truck draw still claimed. N16:
//    the truck's wheels move too, as when braking: where the cab's motion
//    would take them, with the deflection saying so; no wheel draw may be
//    claimed as the cab's.
//  - RIGTEST_RESEND=1: five frames; before the fourth the telemetry sends both
//    configs again, unchanged (coupling, job start or end): nothing may be
//    lost; before the fifth the trailer's config goes empty (uncoupled): the
//    truck must keep its draws, the trailer has none.
//  - RIGTEST_SEARCH=1: the cost of looking for units that are not in sight.
//    63 frames: 30 tracked, then 30 without the draws claimed so far (the rig
//    out of every view while its parts are known: interior camera looking
//    ahead), then 3 with every draw again: the rig must be claimed again on
//    the second of those. Prints the searches and finds per phase; the time
//    per frame (Classify, EndFrame) goes to stderr (not deterministic, not in
//    the baseline).
//  - RIGTEST_TIP=1: six frames; from the fourth on the wheels' draws of both
//    units hang 0.25 m below their place (a truck on its side) while the
//    telemetry's deflection stays the same: every draw must still be claimed,
//    in every one of those frames.
//  - RIGTEST_AGE=1: the members' ageing and limit. 3 frames as usual; then
//    the rig drives 52 km (40 m per frame) with only the main view (the one
//    with the most truck claims) drawn once a km: members seen only in the
//    other views must be forgotten (nothing when only the main view claims),
//    the main view's kept; then 3 frames with
//    every view: everything claimed again; then 200 frames of the main view
//    with 64 new meshes a frame riding in the truck's box, over its front axle
//    (outside the trailer's box; streamed LODs): known members must stay
//    within rig::kMaxKnown and the real ones claimed. Since N13 each batch
//    joins in its first frame, as another LOD of the batch before it.
//  - RIGTEST_ARTIC=1: the trailer turns about its hook (the kingpin) relative
//    to the truck, 18 frames: 3 in line, then 2 degrees more a frame up to 12,
//    back down to 0 and 3 in line again; the trailer's draws (those it claimed
//    in the third frame) and its placement turn with it. A made-up truck part
//    rides behind the cab, inside the trailer's box (an exhaust stack): in line
//    it fits both units and goes to the trailer; once the trailer has turned it
//    must be the truck's. Every frame: each unit keeps its claims of the third
//    frame and claims none of the other's draws; the made-up part is the
//    truck's at the end. RIGTEST_ARTIC=first (N18): the made-up part is drawn
//    before every other draw of its view (the order that anchors a unit on
//    the first known part it meets), and the trailer's frame must stay the
//    trailer's while it turns, within 1 cm.
//  - RIGTEST_LOD=1 (N13): the game swaps LODs while the rig stands still.
//    3 frames as usual (moving), then the rig stops; from the fifth frame on
//    every draw gets another mesh (vb and ib changed) at the same place,
//    except the seventh frame, which has the old meshes again (LODs going
//    back and forth: 97 <-> 74 draws in the D4b test). The new meshes must be
//    claimed from their third frame on (N14: they join on their second), no
//    draw that was not a unit's
//    in the third frame may be claimed, and two made-up grass draws next to a
//    truck part (8 cm beside it; at its place but turned 30 degrees) never.
//  - RIGTEST_DEFL=1 (N13): from the fourth frame on the suspension works
//    (braking: the truck's front wheels rise up to 6 cm towards the chassis,
//    the other wheels sink a little) and the wheels' draws move with the
//    deflection the telemetry gives. Eight frames: every draw still claimed,
//    and rig::GetWheels gives each wheel where its draws are (config +
//    deflection; the radius stays the config's), so the wheel's dirt does
//    not follow the chassis.
//  - RIGTEST_LODCAB=1 (N14): a chassis part and cab parts at one place (the
//    truck's origin, unturned: many of the truck's parts are drawn there),
//    20 frames. Made-up parts: one of the chassis, one of the cab, and three
//    other LODs of the cab there. 3 frames moving, then the rig stops and the
//    cab sways (pitch and roll about 1 degree, frames 4-9: the parts learn
//    their class). In frame 10 the chassis part gets another mesh and the
//    cab's other LODs go away, with the cab at rest (frames 10-11: every
//    part at the same place); in frame 15, with the cab tilted, the cab part
//    gets another mesh. The chassis part's new mesh must be claimed from its
//    third frame on and never as the cab's (D4c test: the chassis' side
//    followed the cab); the cab part's new mesh must be claimed as the cab's
//    from its third frame on.
//  - RIGTEST_GRASS=1 (N17): 8 frames; from the fourth on, a view of its own
//    (the last id) shows made-up draws laid out like the truck's wheels around
//    one more draw (vegetation dense enough for three wheel hits, as the D4f
//    test's mirror): nothing there may be claimed, and the truck's draws in
//    the capture's views must still be.
//  - RIGTEST_CHURN=1 (N14): growth and cost with the rig parked, 400 frames.
//    3 frames as usual, then the rig stops; from the fourth frame on, every
//    frame, 8 draws with a mesh never drawn before (a ring buffer) at the
//    truck's origin and at the places of three truck parts; the rig's meshes
//    swap LOD every 40 frames, and in frames 261-300 flicker every frame
//    between the first LOD and a third one (the camera at a LOD boundary).
//    The made-up draws must never be claimed, the known members must stay
//    within 3x those learnt (one per LOD), and the rig's draws must be
//    claimed at the end of each LOD phase. The time per frame (Classify,
//    EndFrame; parked, and the last 100 frames) goes to stderr (not in the
//    baseline).
// Every mode: no draw may be learnt by both units (rig::SharedMembers() = 0
// after every frame; "shared" is the most seen in the capture).
// RIGTEST_FRAMES=<n> replays up to 8 frames (members that move with the
// truck's cab need a few frames of votes). RIGTEST_TRACE=1 prints every draw of
// the last frame.
// Usage: rigtest <capture.csv>...

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <map>
#include <string>
#include <vector>

#include "rig_tracker.h"

namespace {

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

struct Draw {
    int view;
    rig::DrawKey key;
    float m[12];
    bool interior;
};

// Rotation part rounded to 0.01, to find a view's most common truck rotation (its frame's).
std::string RotKey(const float m[12]) {
    char b[160];
    snprintf(b, sizeof(b), "%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f", m[0], m[1], m[2], m[4], m[5], m[6], m[8],
             m[9], m[10]);
    return b;
}

constexpr int kMaxFrames = 8;  // RIGTEST_FRAMES

// 3x4 row-major affine matrices (rotation + translation in column 3).
void Mul34(const float a[12], const float b[12], float out[12]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
        }
        out[i * 4 + 3] += a[i * 4 + 3];
    }
}
void InvRigid34(const float a[12], float out[12]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i * 4 + j] = a[j * 4 + i];
    for (int i = 0; i < 3; ++i) out[i * 4 + 3] = -(out[i * 4] * a[3] + out[i * 4 + 1] * a[7] + out[i * 4 + 2] * a[11]);
}
// The cab's motion in the truck's frame for an offset (pitch and roll in turns)
// about the pivot, as the game moves the cab: R = Ry Rx Rz (N15: not transposed, see the cab check).
void CabMotion(const float pivot[3], float pitch, float roll, float out[12]) {
    const double k = 6.283185307179586, b = pitch * k, c = roll * k;
    const double rx[3][3] = {{1, 0, 0}, {0, std::cos(b), -std::sin(b)}, {0, std::sin(b), std::cos(b)}};
    const double rz[3][3] = {{std::cos(c), -std::sin(c), 0}, {std::sin(c), std::cos(c), 0}, {0, 0, 1}};
    double e[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) e[i][j] = rx[i][0] * rz[0][j] + rx[i][1] * rz[1][j] + rx[i][2] * rz[2][j];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i * 4 + j] = static_cast<float>(e[i][j]);
        out[i * 4 + 3] = pivot[i] - (out[i * 4] * pivot[0] + out[i * 4 + 1] * pivot[1] + out[i * 4 + 2] * pivot[2]);
    }
}

// The largest distance between where frames a and b put a few points 2 m apart (m).
float PoseGap(const float a[12], const float b[12]) {
    const float pts[4][3] = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 0, 2}};
    float gap = 0.f;
    for (const auto& p : pts) {
        float d2 = 0.f;
        for (int i = 0; i < 3; ++i) {
            const float x = a[i * 4] * p[0] + a[i * 4 + 1] * p[1] + a[i * 4 + 2] * p[2] + a[i * 4 + 3];
            const float y = b[i * 4] * p[0] + b[i * 4 + 1] * p[1] + b[i * 4 + 2] * p[2] + b[i * 4 + 3];
            d2 += (x - y) * (x - y);
        }
        gap = (std::max)(gap, std::sqrt(d2));
    }
    return gap;
}

// RIGTEST_ARTIC: a turn of `deg` degrees about the vertical axis through the trailer's hook, in the
// trailer's frame (the trailer turning about the kingpin, seen from itself).
void HookTurn(const float hook[3], float deg, float out[12]) {
    const float a = deg * 3.14159265f / 180.f, c = std::cos(a), s = std::sin(a);
    const float r[12] = {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0};
    for (int i = 0; i < 12; ++i) out[i] = r[i];
    for (int i = 0; i < 3; ++i) out[i * 4 + 3] = hook[i] - (r[i * 4] * hook[0] + r[i * 4 + 1] * hook[1] + r[i * 4 + 2] * hook[2]);
}

// m = F T F^-1 m: a draw of a unit at frame F moved by T, given in the unit's frame.
void Conjugate(const float f[12], const float t[12], float m[12]) {
    float fi[12], t1[12], t2[12], t3[12];
    InvRigid34(f, fi);
    Mul34(fi, m, t1);
    Mul34(t, t1, t2);
    Mul34(f, t2, t3);
    memcpy(m, t3, sizeof(t3));
}

// The world placement (x y z, heading pitch roll in turns; R = Ry Rx Rz as the tracker reads it) of a
// unit moved by T in its own frame: W' = W T.
void TurnPlacement(double p[6], const float t[12]) {
    const double k = 6.283185307179586, a = p[3] * k, b = p[4] * k, c = p[5] * k;
    const double ry[3][3] = {{std::cos(a), 0, std::sin(a)}, {0, 1, 0}, {-std::sin(a), 0, std::cos(a)}};
    const double rx[3][3] = {{1, 0, 0}, {0, std::cos(b), -std::sin(b)}, {0, std::sin(b), std::cos(b)}};
    const double rz[3][3] = {{std::cos(c), -std::sin(c), 0}, {std::sin(c), std::cos(c), 0}, {0, 0, 1}};
    double tmp[3][3], w[3][3], r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) tmp[i][j] = ry[i][0] * rx[0][j] + ry[i][1] * rx[1][j] + ry[i][2] * rx[2][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) w[i][j] = tmp[i][0] * rz[0][j] + tmp[i][1] * rz[1][j] + tmp[i][2] * rz[2][j];
    double pos[3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) r[i][j] = w[i][0] * t[j] + w[i][1] * t[4 + j] + w[i][2] * t[8 + j];
        pos[i] = p[i] + w[i][0] * t[3] + w[i][1] * t[7] + w[i][2] * t[11];
    }
    // R = Ry Rx Rz: r[1][2] = -sin(pitch), r[1][0] / r[1][1] = tan(roll), r[0][2] / r[2][2] = tan(heading).
    for (int i = 0; i < 3; ++i) p[i] = pos[i];
    p[3] = std::atan2(r[0][2], r[2][2]) / k;
    p[4] = std::asin(-r[1][2]) / k;
    p[5] = std::atan2(r[1][0], r[1][1]) / k;
}

// The capture's last lines: the tracker's counters and the members learnt by both units (must be none).
int Summary(int shared_most) {
    const rig::Stats st = rig::GetStats();
    printf("  promoted %lld, held %lld, cab claims %lld, missed %lld, handed %lld, shared %d\n", st.promoted, st.held,
           st.cab_claims, st.missed, st.handed, shared_most);
    if (!shared_most) return 0;
    printf("  FAIL: members learnt by both units\n");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    int failures = 0;
    const bool still = getenv("RIGTEST_STATIC") != nullptr;
    const bool hold = getenv("RIGTEST_HOLD") != nullptr;
    const bool swapping = getenv("RIGTEST_SWAP") != nullptr;
    const float turn_deg = getenv("RIGTEST_TURN") ? static_cast<float>(atof(getenv("RIGTEST_TURN"))) : 0.f;
    const bool cab_test = getenv("RIGTEST_CAB") != nullptr;
    const bool resend = getenv("RIGTEST_RESEND") != nullptr;
    const bool search_test = getenv("RIGTEST_SEARCH") != nullptr;
    const bool tip = getenv("RIGTEST_TIP") != nullptr;
    const bool age = getenv("RIGTEST_AGE") != nullptr;
    const bool artic = getenv("RIGTEST_ARTIC") != nullptr;
    const bool stack_first = artic && !strcmp(getenv("RIGTEST_ARTIC"), "first");
    const bool lod = getenv("RIGTEST_LOD") != nullptr;
    const bool deflect = getenv("RIGTEST_DEFL") != nullptr;
    const bool lodcab = getenv("RIGTEST_LODCAB") != nullptr;
    const bool churn = getenv("RIGTEST_CHURN") != nullptr;
    const bool grass_test = getenv("RIGTEST_GRASS") != nullptr;
    const bool plain = !still && !hold && !swapping && turn_deg == 0.f && !cab_test && !resend && !search_test && !tip &&
                       !age && !artic && !lod && !deflect && !lodcab && !churn && !grass_test;
    // RIGTEST_LOD: the first frame with the other meshes, the frame with the old ones again, the frames.
    constexpr int kLodFrom = 4, kLodBack = 6, kLodFrames = 9;
    constexpr float kGrassBeside = 0.08f, kGrassTurn = 30.f;  // the made-up grass next to a truck part
    // RIGTEST_DEFL: how far the truck's front wheels rise per frame (m; the others sink a share of it).
    constexpr int kDeflFrames = 8;
    constexpr float kDeflRise[kDeflFrames] = {0, 0, 0, 0.03f, 0.06f, 0.06f, 0.02f, 0};
    // RIGTEST_LODCAB: the frames, the chassis part's and the cab part's LOD swaps, the made-up parts (0 chassis,
    // 1 cab, 2-4 the cab's other LODs, gone at the chassis' swap) and the cab's pitch and roll per frame (turns).
    constexpr int kLcFrames = 20, kLcChassisSwap = 9, kLcCabSwap = 14, kLcParts = 5;
    constexpr float kLcPitch[kLcFrames] = {0, 0, 0, 0.002f, -0.002f, 0.002f, -0.002f, 0.002f, -0.002f, 0,
                                           0, 0.002f, -0.002f, 0.002f, 0.003f, 0.003f, -0.002f, 0.002f, -0.002f, 0.002f};
    constexpr float kLcRoll[kLcFrames] = {0, 0, 0, 0.003f, -0.003f, -0.003f, 0.003f, 0.003f, -0.003f, 0,
                                          0, 0.003f, -0.003f, -0.003f, 0.004f, 0.004f, -0.003f, -0.003f, 0.003f, 0.003f};
    // RIGTEST_CHURN: the frames, the made-up draws per frame (new meshes), the LOD phases and the flicker.
    constexpr int kChurnFrames = 400, kChurnJunk = 8, kChurnPlaces = 4, kChurnFirst = 23, kChurnPhase = 40, kFlickFrom = 260,
                  kFlickTo = 300;
    // RIGTEST_CHURN: the rig's LOD in a frame (0 the captured meshes, 1 and 2 others).
    auto churn_lod_at = [&](int frame) {
        return !churn || frame < kChurnFirst                 ? 0
               : frame >= kFlickFrom && frame < kFlickTo ? (frame % 2 ? 2 : 0)
                                                         : ((frame - kChurnFirst) / kChurnPhase) % 2;
    };
    // RIGTEST_ARTIC: the trailer's angle to the truck per frame (degrees), and the made-up truck part.
    constexpr int kArticFrames = 18;
    constexpr float kArticDeg[kArticFrames] = {0, 0, 0, 2, 4, 6, 8, 10, 12, 12, 10, 8, 6, 4, 2, 0, 0, 0};
    constexpr float kStackY = 2.0f, kStackZ = 1.0f;  // truck frame: behind the cab, in the trailer's box
    constexpr float kDrop = 0.25f;  // RIGTEST_TIP: how far the wheels hang
    // RIGTEST_AGE phases: first frame of the drive, of the return of every view, of the new meshes; the end.
    constexpr int kDrive = 3, kReturn = kDrive + 1300, kStream = kReturn + 3, kAgeEnd = kStream + 200;
    constexpr int kKm = 25, kNewMeshes = 64;  // frames a km at 40 m a frame; new meshes a frame
    constexpr int kAway = 30, kBack = 60;  // RIGTEST_SEARCH phases: first frame away, first frame back
    double ms_total[3][2] = {};            // RIGTEST_SEARCH: per phase, Classify and EndFrame
    int ms_frames[3] = {};
    for (int a = 1; a < argc; ++a) {
        FILE* f = fopen(argv[a], "rb");
        if (!f) {
            printf("FAIL: cannot open %s\n", argv[a]);
            return 1;
        }
        std::vector<Draw> draws;
        rig::UnitConfig cfg[rig::kUnits];
        double place[rig::kUnits][6] = {};
        bool placed[rig::kUnits] = {};
        float defl[rig::kUnits][rig::kMaxWheels] = {};
        bool have_cab = false;
        float cab_pivot[3] = {};
        bool next = false;  // N24: the unit hooked behind trailer.0 (a "next" line)
        float next_hook[3] = {};
        double next_place[6] = {};
        int view_w[rig::kMaxViews] = {}, view_h[rig::kMaxViews] = {};
        char line[4096];
        while (fgets(line, sizeof(line), f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            const auto v = Split(s);
            if (v[0] == "placement") {
                const int u = atoi(v[1].c_str());
                for (int i = 0; i < 6; ++i) place[u][i] = atof(v[2 + i].c_str());
                placed[u] = v.size() < 9 || v[8] == "1";
            } else if (v[0] == "unit") {
                rig::UnitConfig& c = cfg[atoi(v[1].c_str())];
                c.wheels = static_cast<int>((v.size() - 2) / 3);
                for (int w = 0; w < c.wheels; ++w)
                    for (int k = 0; k < 3; ++k) c.wheel[w][k] = static_cast<float>(atof(v[2 + w * 3 + k].c_str()));
            } else if (v[0] == "hook") {
                rig::UnitConfig& c = cfg[atoi(v[1].c_str())];
                c.has_hook = true;
                for (int k = 0; k < 3; ++k) c.hook[k] = static_cast<float>(atof(v[2 + k].c_str()));
            } else if (v[0] == "next" && v.size() >= 12) {
                for (int k = 0; k < 3; ++k) next_hook[k] = static_cast<float>(atof(v[2 + k].c_str()));
                for (int i = 0; i < 6; ++i) next_place[i] = atof(v[5 + i].c_str());
                next = v[11] == "1";
            } else if (v[0] == "cab" && v.size() >= 10) {
                float pivot[3], off[6];
                for (int i = 0; i < 3; ++i) pivot[i] = static_cast<float>(atof(v[1 + i].c_str()));
                for (int i = 0; i < 6; ++i) off[i] = static_cast<float>(atof(v[4 + i].c_str()));
                rig::SetCabin(true, pivot);
                if (cab_test || lodcab) {  // the test drives the offset itself, from rest
                    const float rest[6] = {};
                    rig::SetCabinOffset(rest);
                } else {
                    rig::SetCabinOffset(off);
                }
                have_cab = true;
                for (int i = 0; i < 3; ++i) cab_pivot[i] = pivot[i];
            } else if (v[0] == "deflection") {
                const int u = atoi(v[1].c_str());
                for (size_t w = 2; w < v.size() && w - 2 < rig::kMaxWheels; ++w)
                    defl[u][w - 2] = static_cast<float>(atof(v[w].c_str()));
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
                draws.push_back(d);
            }
        }
        fclose(f);
        for (int u = 0; u < rig::kUnits; ++u) {
            // A new capture: an empty layout first, so the tracker forgets the last capture's even when
            // the layout is the same (the same truck and trailer).
            rig::SetUnit(u, rig::UnitConfig());
            rig::SetUnit(u, cfg[u]);
            rig::SetDeflection(u, defl[u], cfg[u].wheels);
        }
        // Group = first view of the same size; swap = the other view of the same size.
        int group[rig::kMaxViews], swap[rig::kMaxViews];
        for (int i = 0; i < rig::kMaxViews; ++i) {
            group[i] = i;
            swap[i] = i;
            for (int j = 0; j < rig::kMaxViews; ++j)
                if (j != i && view_w[i] && view_w[j] == view_w[i] && view_h[j] == view_h[i]) {
                    if (j < group[i]) group[i] = j;
                    if (swap[i] == i) swap[i] = j;
                }
        }
        // hold: a full frame first, then the one without rigid draws
        int frames = hold || turn_deg ? 4
                     : cab_test       ? 8
                     : resend         ? 5
                     : search_test    ? kBack + 3
                     : tip            ? 6
                     : age            ? kAgeEnd
                     : artic          ? kArticFrames
                     : lod            ? kLodFrames
                     : deflect        ? kDeflFrames
                     : lodcab         ? kLcFrames
                     : churn          ? kChurnFrames
                     : grass_test     ? 8
                                      : 3;
        if (getenv("RIGTEST_FRAMES")) frames = (std::max)(frames, (std::min)(kMaxFrames, atoi(getenv("RIGTEST_FRAMES"))));
        std::vector<std::array<std::array<int, rig::kUnits>, rig::kMaxViews>> claimed(frames);
        for (auto& fr : claimed)
            for (auto& vc : fr) vc.fill(0);
        std::vector<int> unit_at(draws.size(), -1);  // the next-to-last frame's answer per draw
        std::vector<bool> skip(draws.size(), false);
        int interior_claims = 0, interior_draws = 0, expected_held[rig::kMaxViews] = {};
        // RIGTEST_CAB: the truck's frame per view at the third frame, and which draws are the cab's.
        float chassis[rig::kMaxViews][12] = {};
        bool chassis_ok[rig::kMaxViews] = {};
        std::vector<bool> on_cab(draws.size(), false);
        std::vector<int> unit_third(draws.size(), -1);
        const long long cab_claims0 = rig::GetStats().cab_claims;
        int cab_draws = 0;
        // RIGTEST_CAB, N16: the truck's wheel draws (the wheel's index), each wheel's move by the cab this frame
        // (chassis frame) and the wheel draws claimed as the cab's.
        std::vector<int> cab_wheel(draws.size(), -1);
        float cab_wheel_move[rig::kMaxWheels][3] = {};
        int wheel_draws = 0, wheel_cab_claims = 0;
        // RIGTEST_GRASS: the made-up draws a frame, those claimed, the rejected searches before.
        int veg_draws = 0, veg_claims = 0;
        float trailer_off = 0.f;  // RIGTEST_ARTIC (N18): the trailer's frame off the trailer, at most (m)
        const long long rejected0 = rig::GetStats().rejected;
        std::vector<int> unit_prev(draws.size(), -1);  // the previous frame's answer per draw
        long long searches[3] = {}, found[3] = {};       // RIGTEST_SEARCH, per phase
        // RIGTEST_TIP and RIGTEST_AGE: each unit's frame per view at the third frame.
        float unit_frame[rig::kMaxViews][rig::kUnits][12] = {};
        bool unit_frame_ok[rig::kMaxViews][rig::kUnits] = {};
        std::vector<int> hanging(draws.size(), -1);  // RIGTEST_TIP, RIGTEST_DEFL: the unit whose wheel the draw is
        std::vector<int> hanging_w(draws.size(), -1);  // ... and which wheel
        int hanging_n = 0;
        // RIGTEST_DEFL: each wheel's deflection this frame, the most a wheel draw's place in its wheel changed
        // (cm) and the most GetWheels was off the config + deflection (m).
        float defl_now[rig::kUnits][rig::kMaxWheels] = {};
        float wheel_drift = 0.f, wheel_off = 0.f;
        // RIGTEST_LOD: draws claimed that were no unit's (or the other unit's) in the third frame, the
        // made-up grass draws claimed, the grass's model matrices (from a truck part of the third frame).
        int lod_strays = 0, grass_claims = 0;
        bool grass_ok = false;
        float grass[2][12] = {};
        // D12b: a new mesh with a known part's index count at its place is that part in another buffer (reloaded),
        // one with another count another LOD (inherited): the made-up swaps here keep the counts. Both counted.
        auto joined = [] { const rig::Stats s = rig::GetStats(); return s.inherited + s.reloaded; };
        const long long inherited0 = joined(), unclassed0 = rig::GetStats().unclassed;
        // RIGTEST_LODCAB: per made-up part, (frame, view) pairs drawn after its swap's second frame, claimed by
        // the truck, claimed as the cab's; the chassis part claimed as the cab's in any frame; the cab parts
        // claimed as the cab's in the frame before the chassis' swap (of lc_ready_n drawn).
        int lc_drawn[2] = {}, lc_claimed[2] = {}, lc_as_cab[2] = {}, lc_chassis_cab = 0, lc_ready = 0, lc_ready_n = 0;
        int lc_views = 0;
        // RIGTEST_CHURN: the made-up draws' local poses (the origin and three truck parts), their claims, the
        // known members after learning and the most since, the rig's draws lost at the end of a LOD phase.
        float churn_pose[kChurnPlaces][12] = {};
        int churn_poses = 0, churn_claims = 0, churn_known_most[rig::kUnits] = {}, churn_lost = 0, churn_checks = 0;
        // RIGTEST_AGE: the main view, known members after each phase, the most during the new meshes.
        int main_view = 0, known_learnt[rig::kUnits] = {}, known_driven[rig::kUnits] = {}, known_most = 0;
        long long forgotten0 = rig::GetStats().forgotten, forgotten_driven = 0, fake_claims = 0;
        double odo[2] = {};  // RIGTEST_AGE: the rig's x and z offset
        int shared_most = 0;  // rig::SharedMembers(), the most after a frame
        // RIGTEST_ARTIC: the made-up part's claims per view and frame (-2: not drawn), draws claimed by the
        // other unit than in the third frame, and the frames from the first frame on (the replay is still).
        std::vector<std::array<int, rig::kMaxViews>> stack_unit(frames);
        for (auto& sv : stack_unit) sv.fill(-2);
        int artic_swapped = 0;
        std::string debug;
        for (int frame = 0; frame < frames; ++frame) {
            const bool last = frame == frames - 1;
            const int phase = frame < kAway ? 0 : frame < kBack ? 1 : 2;
            if (search_test && frame == kAway)
                for (size_t i = 0; i < draws.size(); ++i) skip[i] = unit_prev[i] >= 0;
            if (search_test && frame == kBack) std::fill(skip.begin(), skip.end(), false);
            const rig::Stats st0 = rig::GetStats();
            const auto t0 = std::chrono::steady_clock::now();
            if (last && hold) {
                // Leave out the truck's draws at the view's frame rotation: what remains has no rigid anchor.
                std::map<std::string, int> rot[rig::kMaxViews];
                for (size_t i = 0; i < draws.size(); ++i)
                    if (unit_at[i] == 0) ++rot[draws[i].view][RotKey(draws[i].m)];
                for (size_t i = 0; i < draws.size(); ++i) {
                    if (unit_at[i] != 0) continue;
                    const auto& r = rot[draws[i].view];
                    std::string best;
                    int n = 0;
                    for (const auto& kv : r)
                        if (kv.second > n) best = kv.first, n = kv.second;
                    skip[i] = RotKey(draws[i].m) == best;
                    if (!skip[i]) ++expected_held[draws[i].view];
                }
            }
            if (age && frame > 0) {
                const bool driving = frame >= kDrive && frame < kReturn;
                odo[0] += driving ? 40.0 : 2.0, odo[1] += driving ? 0.0 : 2.0;
            }
            const float artic_deg = artic ? kArticDeg[frame] : 0.f;
            float turn_local[12];  // RIGTEST_ARTIC: the trailer's turn about its hook, in its own frame
            HookTurn(cfg[1].hook, artic_deg, turn_local);
            for (int u = 0; u < rig::kUnits; ++u) {
                double p[6];
                for (int i = 0; i < 6; ++i) p[i] = place[u][i];
                if (age) p[0] += odo[0], p[2] += odo[1];
                else if (lod || lodcab || churn) p[0] += 2.0 * (std::min)(frame, 2), p[2] += 2.0 * (std::min)(frame, 2);  // then stopped
                else if (!still) p[0] += 2.0 * frame, p[2] += 2.0 * frame;  // both units move together
                if (u == 1 && artic_deg != 0.f) TurnPlacement(p, turn_local);
                rig::SetPlacement(u, p, placed[u]);
            }
            {  // N24: the unit behind trailer.0 moves with the rig
                double p[6];
                for (int i = 0; i < 6; ++i) p[i] = next_place[i];
                if (!age && !lod && !lodcab && !churn && !still) p[0] += 2.0 * frame, p[2] += 2.0 * frame;
                rig::SetNext(1, next_hook, p, next && !artic_deg && !age && !lod && !lodcab && !churn);
            }
            float cab_move[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            if (cab_test && have_cab && frame >= 3) {
                const float pitch = 0.002f * std::cos(0.9f * frame), roll = 0.003f * std::sin(1.3f * frame);
                const float off[6] = {0, 0, 0, 0, pitch, roll};
                rig::SetCabinOffset(off);
                CabMotion(cab_pivot, pitch, roll, cab_move);
                // N16, braking: the truck's wheels go where the cab's motion would take them (the axle rising
                // while the cab dives), the telemetry's deflection saying how far up.
                float d[rig::kMaxWheels];
                for (int w = 0; w < cfg[0].wheels; ++w) {
                    float c[3] = {cfg[0].wheel[w][0], cfg[0].wheel[w][1] + defl[0][w], cfg[0].wheel[w][2]};
                    for (int k = 0; k < 3; ++k)
                        cab_wheel_move[w][k] = cab_move[k * 4] * c[0] + cab_move[k * 4 + 1] * c[1] + cab_move[k * 4 + 2] * c[2] +
                                               cab_move[k * 4 + 3] - c[k];
                    d[w] = defl[0][w] + cab_wheel_move[w][1];
                }
                rig::SetDeflection(0, d, cfg[0].wheels);
            }
            if (lodcab && have_cab) {
                const float off[6] = {0, 0, 0, 0, kLcPitch[frame], kLcRoll[frame]};
                rig::SetCabinOffset(off);
                CabMotion(cab_pivot, kLcPitch[frame], kLcRoll[frame], cab_move);
            }
            const int churn_lod = churn_lod_at(frame);
            if (deflect) {
                // Braking: the truck's front axle (its smallest z) rises, its other wheels and the trailer's sink.
                float front_z = 1e9f;
                for (int w = 0; w < cfg[0].wheels; ++w) front_z = (std::min)(front_z, cfg[0].wheel[w][2]);
                for (int u = 0; u < rig::kUnits; ++u) {
                    for (int w = 0; w < cfg[u].wheels; ++w) {
                        const bool front = u == 0 && cfg[u].wheel[w][2] < front_z + 0.01f;
                        defl_now[u][w] = defl[u][w] + kDeflRise[frame] * (front ? 1.f : u == 0 ? -0.5f : -0.3f);
                    }
                    rig::SetDeflection(u, defl_now[u], cfg[u].wheels);
                }
            }
            if (resend && frame == 3)
                for (int u = 0; u < rig::kUnits; ++u) rig::SetUnit(u, cfg[u]);
            if (resend && frame == 4) rig::SetUnit(1, rig::UnitConfig());
            rig::BeginFrame();
            auto draw_stack = [&]() {
                for (int vi = 0; frame >= 1 && vi < rig::kMaxViews; ++vi) {
                    // The made-up truck part, in every view where the truck was found in the first frame.
                    if (!unit_frame_ok[vi][0]) continue;
                    const float local[12] = {1, 0, 0, 0, 0, 1, 0, kStackY, 0, 0, 1, kStackZ};
                    float m[12];
                    Mul34(unit_frame[vi][0], local, m);
                    rig::DrawKey key;
                    key.vb = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x7e0000000000ull));
                    key.count = 3;
                    stack_unit[frame][vi] = rig::Classify(vi, group[vi], key, m);
                }
            };
            if (stack_first) draw_stack();
            const bool trace = last && getenv("RIGTEST_TRACE");
            // The camera's turn this frame: every model->camera matrix is rotated about the camera's Y.
            const float ang = turn_deg * frame * 3.14159265f / 180.f, cs = std::cos(ang), sn = std::sin(ang);
            // RIGTEST_AGE: the drive draws the main view once a km, the new meshes only the main view.
            const bool age_some = age && frame >= kDrive && frame < kReturn;
            const bool age_main = age && (frame >= kStream || (age_some && (frame - kDrive) % kKm == kKm - 1));
            for (size_t i = 0; i < draws.size(); ++i) {
                if (skip[i] || (age_some && !age_main) || (age_main && draws[i].view != main_view)) continue;
                const Draw& d = draws[i];
                const int view = last && swapping ? swap[d.view] : d.view;
                float m[12];
                for (int c = 0; c < 4; ++c) {
                    m[c] = cs * d.m[c] + sn * d.m[8 + c];
                    m[4 + c] = d.m[4 + c];
                    m[8 + c] = -sn * d.m[c] + cs * d.m[8 + c];
                }
                if (cab_test && frame >= 3 && cab_wheel[i] >= 0 && chassis_ok[d.view]) {  // N16: the wheel moved
                    const float* c = cab_wheel_move[cab_wheel[i]];
                    const float move[12] = {1, 0, 0, c[0], 0, 1, 0, c[1], 0, 0, 1, c[2]};
                    Conjugate(chassis[d.view], move, m);
                }
                if (on_cab[i] && chassis_ok[d.view]) {  // the cab's draw moved by the cab: F T F^-1 M
                    float fi[12], t1[12], t2[12], t3[12];
                    InvRigid34(chassis[d.view], fi);
                    Mul34(fi, m, t1);
                    Mul34(cab_move, t1, t2);
                    Mul34(chassis[d.view], t2, t3);
                    memcpy(m, t3, sizeof(m));
                }
                if (artic_deg != 0.f && unit_third[i] == 1 && unit_frame_ok[d.view][1])  // the trailer turned: F T F^-1 M
                    Conjugate(unit_frame[d.view][1], turn_local, m);
                if (tip && frame >= 3 && hanging[i] >= 0) {  // the wheel hangs: F T F^-1 M, T = kDrop down
                    const float* fu = unit_frame[d.view][hanging[i]];
                    float fi[12], t1[12], t2[12], t3[12];
                    const float down[12] = {1, 0, 0, 0, 0, 1, 0, -kDrop, 0, 0, 1, 0};
                    InvRigid34(fu, fi);
                    Mul34(fi, m, t1);
                    Mul34(down, t1, t2);
                    Mul34(fu, t2, t3);
                    memcpy(m, t3, sizeof(m));
                }
                if (deflect && frame >= 3 && hanging[i] >= 0) {  // the wheel moves with its deflection
                    const int hu = hanging[i], hw = hanging_w[i];
                    const float up[12] = {1, 0, 0, 0, 0, 1, 0, defl_now[hu][hw] - defl[hu][hw], 0, 0, 1, 0};
                    Conjugate(unit_frame[d.view][hu], up, m);
                    // Its place relative to the wheel GetWheels gives, against the third frame's (no deflection change).
                    float wheels[rig::kMaxWheels][4], fi[12];
                    rig::GetWheels(hu, wheels);
                    InvRigid34(unit_frame[d.view][hu], fi);
                    const float y = fi[4] * m[3] + fi[5] * m[7] + fi[6] * m[11] + fi[7];
                    const float y0 = fi[4] * d.m[3] + fi[5] * d.m[7] + fi[6] * d.m[11] + fi[7];
                    const float rest = cfg[hu].wheel[hw][1] + defl[hu][hw];
                    wheel_drift = (std::max)(wheel_drift, 100.f * std::fabs((y - wheels[hw][1]) - (y0 - rest)));
                }
                rig::DrawKey key = d.key;
                if ((lod && frame >= kLodFrom && frame != kLodBack) || churn_lod == 1) {  // another LOD: other buffers, the same place
                    key.vb = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(d.key.vb) ^ 0x5a5a00000ull);
                    key.ib = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(d.key.ib) ^ 0x3c3c00000ull);
                } else if (churn_lod == 2) {
                    key.vb = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(d.key.vb) ^ 0x6b6b00000ull);
                    key.ib = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(d.key.ib) ^ 0x4d4d00000ull);
                }
                bool as_cab = false;
                const int u = rig::Classify(view, group[view], key, m, d.interior, &as_cab);
                if (cab_test && frame >= 3 && cab_wheel[i] >= 0 && u == 0 && as_cab) ++wheel_cab_claims;
                if (lod && frame >= 3 && u >= 0 && u != unit_third[i]) ++lod_strays;
                if (u >= 0) ++claimed[frame][d.view][u];
                if (artic && frame >= 3 && u >= 0 && unit_third[i] >= 0 && u != unit_third[i]) ++artic_swapped;
                if (frame == frames - 2) unit_at[i] = u;
                unit_prev[i] = u;
                if (frame == 2) unit_third[i] = u;
                if (d.interior) {
                    ++interior_draws;
                    if (u >= 0) ++interior_claims;
                }
                if (trace) printf("  draw %zu view %d n=%u -> %d\n", i, d.view, d.key.count, u);
            }
            if (age && frame >= kStream && unit_frame_ok[main_view][0]) {
                // New meshes in the truck's box, each drawn for 3 frames at one place: promoted on the second.
                float local[12] = {1, 0, 0, 0, 0, 1, 0, 2.f, 0, 0, 1, 0};
                local[11] = cfg[0].wheel[0][2];  // over the front axle: the truck's box only
                float m[12];
                Mul34(unit_frame[main_view][0], local, m);
                const int now = frame - kStream;
                for (int b = (std::max)(0, now - 2); b <= now; ++b)
                    for (int j = 0; j < kNewMeshes; ++j) {
                        rig::DrawKey key;
                        key.vb = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x7f0000000000ull + b * kNewMeshes + j));
                        key.count = 3;
                        if (rig::Classify(main_view, group[main_view], key, m) >= 0) ++fake_claims;
                    }
            }
            if (grass_test && frame >= 3) {
                // The last view: a draw 30 m ahead and made-up draws where the truck's wheels would be around it,
                // plus a patch of the same mesh around (a grass chunk drawn many times).
                const int gv = rig::kMaxViews - 1;
                const float g[12] = {1, 0, 0, 0.4f, 0, 1, 0, -1.6f, 0, 0, 1, -30.f};
                veg_draws = 0;
                auto draw = [&](uint64_t id, float x, float y, float z) {
                    float m[12];
                    memcpy(m, g, sizeof(m));
                    m[3] += x, m[7] += y, m[11] += z;
                    rig::DrawKey key;
                    key.vb = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x7c0000000000ull + id));
                    key.count = 3;
                    ++veg_draws;
                    if (rig::Classify(gv, gv, key, m) >= 0) ++veg_claims;
                };
                draw(0xff, 0, 0, 0);
                for (int w = 0; w < cfg[0].wheels; ++w) draw(w, cfg[0].wheel[w][0], cfg[0].wheel[w][1] + defl[0][w], cfg[0].wheel[w][2]);
                for (int k = 0; k < 16; ++k) draw(0x100, -3.f + 2.f * (k % 4), 0.f, -4.f + 2.5f * (k / 4));
            }
            if (artic && !stack_first) draw_stack();
            for (int vi = 0; lodcab && have_cab && frame >= 1 && vi < rig::kMaxViews; ++vi) {
                // The made-up parts at the truck's origin, in every view where the truck was found in the first
                // frame: the chassis' at the truck's frame, the cab's moved by the cab.
                if (!unit_frame_ok[vi][0]) continue;
                lc_views += frame == 1 ? 1 : 0;
                for (int p = 0; p < kLcParts && (p < 2 || frame < kLcChassisSwap); ++p) {
                    const int swap_at = p == 0 ? kLcChassisSwap : kLcCabSwap;
                    const int mesh = p * 2 + (p < 2 && frame >= swap_at ? 1 : 0);
                    rig::DrawKey key;
                    key.vb = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x7c0000000000ull + mesh));
                    key.count = 3;
                    float m[12];
                    if (p == 0) memcpy(m, unit_frame[vi][0], sizeof(m));
                    else Mul34(unit_frame[vi][0], cab_move, m);
                    bool on = false;
                    const int u = rig::Classify(vi, group[vi], key, m, false, &on);
                    if (p == 0 && u == 0 && on) ++lc_chassis_cab;
                    if (p >= 1 && frame == kLcChassisSwap - 1) ++lc_ready_n, lc_ready += u == 0 && on ? 1 : 0;
                    if (p < 2 && frame >= swap_at + 2) {
                        ++lc_drawn[p];
                        lc_claimed[p] += u == 0 ? 1 : 0;
                        lc_as_cab[p] += u == 0 && on ? 1 : 0;
                    }
                }
            }
            for (int j = 0; churn_poses && j < kChurnJunk && frame >= 3; ++j) {
                // RIGTEST_CHURN: a mesh never drawn before at a truck part's place, in the main view.
                rig::DrawKey key;
                key.vb = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x7b0000000000ull));
                key.start = static_cast<uint32_t>(frame * kChurnJunk + j);
                key.count = 3;
                float m[12];
                Mul34(unit_frame[main_view][0], churn_pose[j % churn_poses], m);
                if (rig::Classify(main_view, group[main_view], key, m) >= 0) ++churn_claims;
            }
            for (int g = 0; lod && grass_ok && frame >= 3 && g < 2; ++g) {
                // The made-up grass: a fixed mesh beside the part, a new mesh every frame at its place, turned.
                rig::DrawKey key;
                key.vb = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x7d0000000000ull + (g ? frame : 0)));
                key.count = 3;
                if (rig::Classify(main_view, group[main_view], key, grass[g]) >= 0) ++grass_claims;
            }
            // RIGTEST_ARTIC: the trailer's frame this frame against where it is (its third frame's turned by T).
            for (int vi = 0; artic && frame >= 3 && vi < rig::kMaxViews; ++vi) {
                float tf[12], want[12];
                if (!unit_frame_ok[vi][1] || !rig::GetFrame(vi, 1, tf)) continue;
                Mul34(unit_frame[vi][1], turn_local, want);
                trailer_off = (std::max)(trailer_off, PoseGap(tf, want));
            }
            debug.clear();
            const auto t1 = std::chrono::steady_clock::now();
            rig::EndFrame(search_test ? nullptr : &debug);
            const auto t2 = std::chrono::steady_clock::now();
            shared_most = (std::max)(shared_most, rig::SharedMembers());
            if ((artic || lodcab) && frame == 0)  // the units' frames, to draw the made-up parts from the second frame on
                for (int vi = 0; vi < rig::kMaxViews; ++vi)
                    for (int u = 0; u < rig::kUnits; ++u) unit_frame_ok[vi][u] = rig::GetFrame(vi, u, unit_frame[vi][u]);
            if (deflect && frame >= 3) {  // GetWheels: config + this frame's deflection, radius the config's
                float wheels[rig::kMaxWheels][4];
                for (int u = 0; u < rig::kUnits; ++u) {
                    const int n = rig::GetWheels(u, wheels);
                    for (int w = 0; w < n; ++w) {
                        const float* c = cfg[u].wheel[w];
                        wheel_off = (std::max)({wheel_off, std::fabs(wheels[w][0] - c[0]), std::fabs(wheels[w][2] - c[2]),
                                                std::fabs(wheels[w][1] - c[1] - defl_now[u][w]), std::fabs(wheels[w][3] - c[1])});
                    }
                }
            }
            if ((tip || age || artic || lod || deflect || churn) && frame == 2) {
                int best = -1;
                for (int vi = 0; vi < rig::kMaxViews; ++vi) {
                    for (int u = 0; u < rig::kUnits; ++u) unit_frame_ok[vi][u] = rig::GetFrame(vi, u, unit_frame[vi][u]);
                    if (claimed[2][vi][0] > best) best = claimed[2][vi][0], main_view = vi;
                }
                // RIGTEST_TIP, RIGTEST_DEFL: the draws at a wheel's place (config + deflection) of the unit that
                // claimed them.
                for (size_t i = 0; (tip || deflect) && i < draws.size(); ++i) {
                    const int u = unit_third[i];
                    if (u < 0 || !unit_frame_ok[draws[i].view][u]) continue;
                    float fi[12];
                    InvRigid34(unit_frame[draws[i].view][u], fi);
                    const float* t = draws[i].m;
                    float lp[3];
                    for (int k = 0; k < 3; ++k) lp[k] = fi[k * 4] * t[3] + fi[k * 4 + 1] * t[7] + fi[k * 4 + 2] * t[11] + fi[k * 4 + 3];
                    for (int w = 0; w < cfg[u].wheels && hanging[i] < 0; ++w) {
                        const float dx = lp[0] - cfg[u].wheel[w][0], dy = lp[1] - cfg[u].wheel[w][1] - defl[u][w],
                                    dz = lp[2] - cfg[u].wheel[w][2];
                        if (dx * dx + dy * dy + dz * dz < 0.12f * 0.12f) hanging[i] = u, hanging_w[i] = w;
                    }
                    hanging_n += hanging[i] >= 0 ? 1 : 0;
                }
                for (int u = 0; u < rig::kUnits; ++u) known_learnt[u] = rig::GetStats().known[u];
                // RIGTEST_LOD: the grass beside the main view's first truck part claimed off its wheels (a part
                // rotated as the truck: a box draw, learnt after the move).
                for (size_t i = 0; lod && !grass_ok && i < draws.size(); ++i) {
                    if (unit_third[i] != 0 || draws[i].view != main_view || !unit_frame_ok[main_view][0]) continue;
                    float fi[12], local[12];
                    InvRigid34(unit_frame[main_view][0], fi);
                    Mul34(fi, draws[i].m, local);
                    bool at_wheel = false, rigid = true;
                    for (int w = 0; w < cfg[0].wheels; ++w)
                        at_wheel = at_wheel || std::fabs(local[3] - cfg[0].wheel[w][0]) + std::fabs(local[11] - cfg[0].wheel[w][2]) < 0.5f;
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c) rigid = rigid && std::fabs(local[r * 4 + c] - (r == c ? 1.f : 0.f)) < 0.01f;
                    if (at_wheel || !rigid) continue;
                    grass_ok = true;
                    const float beside[12] = {1, 0, 0, kGrassBeside, 0, 1, 0, 0, 0, 0, 1, 0};
                    const float grass_rad = kGrassTurn * 3.14159265f / 180.f;
                    const float turned[12] = {std::cos(grass_rad), 0, std::sin(grass_rad), 0, 0, 1, 0, 0, -std::sin(grass_rad), 0, std::cos(grass_rad), 0};
                    memcpy(grass[0], draws[i].m, sizeof(grass[0]));
                    Conjugate(unit_frame[main_view][0], beside, grass[0]);
                    Mul34(draws[i].m, turned, grass[1]);
                }
            }
            if (churn && frame == 2 && unit_frame_ok[main_view][0]) {
                // RIGTEST_CHURN: the made-up draws' places: the truck's origin and its first three parts claimed off
                // its wheels in the main view (rotated as the truck: box draws).
                const float origin[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
                memcpy(churn_pose[churn_poses++], origin, sizeof(origin));
                for (size_t i = 0; churn_poses < kChurnPlaces && i < draws.size(); ++i) {
                    if (unit_third[i] != 0 || draws[i].view != main_view) continue;
                    float fi[12], local[12];
                    InvRigid34(unit_frame[main_view][0], fi);
                    Mul34(fi, draws[i].m, local);
                    bool at_wheel = false, rigid = true;
                    for (int w = 0; w < cfg[0].wheels; ++w)
                        at_wheel = at_wheel || std::fabs(local[3] - cfg[0].wheel[w][0]) + std::fabs(local[11] - cfg[0].wheel[w][2]) < 0.5f;
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c) rigid = rigid && std::fabs(local[r * 4 + c] - (r == c ? 1.f : 0.f)) < 0.01f;
                    if (!at_wheel && rigid) memcpy(churn_pose[churn_poses++], local, sizeof(local));
                }
            }
            if (churn) {
                for (int u = 0; u < rig::kUnits; ++u)
                    churn_known_most[u] = (std::max)(churn_known_most[u], frame >= 3 ? rig::GetStats().known[u] : 0);
                // The end of a LOD phase (not inside the flicker, but its last frame, a third-LOD one): every draw
                // claimed in the third frame claimed again.
                const bool flicker = frame >= kFlickFrom && frame < kFlickTo - 1;
                if (frame >= 3 && !flicker && (churn_lod_at(frame + 1) != churn_lod || last)) {
                    ++churn_checks;
                    for (int vi = 0; vi < rig::kMaxViews; ++vi)
                        for (int u = 0; u < rig::kUnits; ++u) churn_lost += claimed[frame][vi][u] < claimed[2][vi][u] * 95 / 100 ? 1 : 0;
                }
                for (int p = 0; p < 2; ++p) {  // the parked frames, and the last 100 (the most members if they grow)
                    if (frame < (p ? frames - 100 : 3)) continue;
                    ms_total[p][0] += std::chrono::duration<double, std::milli>(t1 - t0).count();
                    ms_total[p][1] += std::chrono::duration<double, std::milli>(t2 - t1).count();
                    ++ms_frames[p];
                }
            }
            if (age && frame == kReturn - 1) {
                const rig::Stats st1 = rig::GetStats();
                for (int u = 0; u < rig::kUnits; ++u) known_driven[u] = st1.known[u];
                forgotten_driven = st1.forgotten - forgotten0;
            }
            if (age && frame >= kStream)
                for (int u = 0; u < rig::kUnits; ++u) known_most = (std::max)(known_most, rig::GetStats().known[u]);
            if (search_test) {
                const rig::Stats st1 = rig::GetStats();
                searches[phase] += st1.searches - st0.searches;
                found[phase] += st1.found - st0.found;
                if (frame >= 3) {  // the first frames learn: not the steady cost
                    ms_total[phase][0] += std::chrono::duration<double, std::milli>(t1 - t0).count();
                    ms_total[phase][1] += std::chrono::duration<double, std::milli>(t2 - t1).count();
                    ++ms_frames[phase];
                }
            }
            if ((cab_test || lodcab) && have_cab && frame == 2) {
                // The chassis frame per view, and the cab's draws: the truck's, in front of z = 0 and above 1.05 m.
                for (int vi = 0; vi < rig::kMaxViews; ++vi) chassis_ok[vi] = rig::GetFrame(vi, 0, chassis[vi]);
                for (size_t i = 0; i < draws.size(); ++i) {
                    if (unit_third[i] != 0 || !chassis_ok[draws[i].view]) continue;
                    float fi[12];
                    InvRigid34(chassis[draws[i].view], fi);
                    const float* t = draws[i].m;
                    const float y = fi[4] * t[3] + fi[5] * t[7] + fi[6] * t[11] + fi[7];
                    const float z = fi[8] * t[3] + fi[9] * t[7] + fi[10] * t[11] + fi[11];
                    on_cab[i] = z < 0.f && y > 1.05f;
                    cab_draws += on_cab[i] ? 1 : 0;
                    const float x = fi[0] * t[3] + fi[1] * t[7] + fi[2] * t[11] + fi[3];
                    for (int w = 0; w < cfg[0].wheels && cab_wheel[i] < 0; ++w) {
                        const float dx = x - cfg[0].wheel[w][0], dy = y - cfg[0].wheel[w][1] - defl[0][w], dz = z - cfg[0].wheel[w][2];
                        if (dx * dx + dy * dy + dz * dz < 0.12f * 0.12f) cab_wheel[i] = w;
                    }
                    wheel_draws += cab_wheel[i] >= 0 ? 1 : 0;
                }
            }
            if (!last && getenv("RIGTEST_VERBOSE")) printf("frame %d:\n%s", frame + 1, debug.c_str());
        }
        printf("%s: %zu draws\n%s", argv[a], draws.size(), debug.c_str());
        if (search_test) {
            printf("  searches (found): tracked %lld (%lld), away %lld (%lld), back %lld (%lld)\n", searches[0], found[0],
                   searches[1], found[1], searches[2], found[2]);
            for (int vi = 0; vi < rig::kMaxViews; ++vi) {
                const int* tracked = claimed[kAway - 1][vi].data();
                if (!tracked[0] && !tracked[1]) continue;
                printf("  view %d claimed (truck/trailer): frame 3 %d/%d frame %d %d/%d frame %d %d/%d frame %d %d/%d\n", vi,
                       claimed[2][vi][0], claimed[2][vi][1], kAway, tracked[0], tracked[1], kAway + 1,
                       claimed[kAway][vi][0], claimed[kAway][vi][1], kBack + 2, claimed[kBack + 1][vi][0],
                       claimed[kBack + 1][vi][1]);
                if (tracked[0] + tracked[1] < (claimed[2][vi][0] + claimed[2][vi][1]) * 95 / 100 ||
                    claimed[kAway][vi][0] + claimed[kAway][vi][1] ||
                    claimed[kBack + 1][vi][0] + claimed[kBack + 1][vi][1] < (tracked[0] + tracked[1]) * 95 / 100) {
                    printf("  FAIL: %s\n", claimed[kAway][vi][0] + claimed[kAway][vi][1] ? "claims while the rig is away"
                                                                                         : "draws lost");
                    ++failures;
                }
            }
            failures += Summary(shared_most);
            continue;
        }
        if (age && !claimed[2][main_view][0]) {
            printf("  age: the truck is in no view, nothing to test\n");
            continue;
        }
        if (age) {
            printf("  age: main view %d, known (truck/trailer) %d/%d, after 52 km %d/%d (%lld forgotten)\n", main_view,
                   known_learnt[0], known_learnt[1], known_driven[0], known_driven[1], forgotten_driven);
            bool others = false;  // other views claimed: members seen only there
            for (int vi = 0; vi < rig::kMaxViews; ++vi) others = others || (vi != main_view && (claimed[2][vi][0] || claimed[2][vi][1]));
            if (!known_driven[0] || (others ? !forgotten_driven : forgotten_driven != 0)) {
                printf("  FAIL: %s\n", !known_driven[0] ? "the main view's members forgotten"
                                        : others        ? "nothing forgotten"
                                                        : "members forgotten though the main view drew them");
                ++failures;
            }
            for (int vi = 0; vi < rig::kMaxViews; ++vi) {
                if (!claimed[2][vi][0] && !claimed[2][vi][1]) continue;
                printf("  view %d claimed (truck/trailer): frame 3 %d/%d frame %d %d/%d\n", vi, claimed[2][vi][0],
                       claimed[2][vi][1], kStream, claimed[kStream - 1][vi][0], claimed[kStream - 1][vi][1]);
                for (int u = 0; u < rig::kUnits; ++u)
                    if (claimed[kStream - 1][vi][u] < claimed[2][vi][u] * 95 / 100) {
                        printf("  FAIL: draws not claimed again after the drive\n");
                        ++failures;
                        break;
                    }
            }
            const int real = claimed[kAgeEnd - 1][main_view][0];
            printf("  new meshes: %d, %lld claims of them (%lld inherited), known members at most %d (limit %d), main "
                   "view truck claims %d\n", 200 * kNewMeshes, fake_claims, joined() - inherited0,
                   known_most, rig::kMaxKnown, real);
            if (known_most > rig::kMaxKnown || real < claimed[2][main_view][0] * 95 / 100 || !fake_claims) {
                printf("  FAIL: %s\n", known_most > rig::kMaxKnown ? "more members than the limit"
                                        : !fake_claims          ? "the new meshes were never learnt"
                                                                : "the truck's own draws lost to the new meshes");
                ++failures;
            }
            failures += Summary(shared_most);
            continue;
        }
        if (churn) {
            if (!claimed[2][main_view][0]) {
                printf("  churn: the truck is in no view, nothing to test\n");
                continue;
            }
            printf("  churn: main view %d, %d made-up draws a frame (new meshes, at %d places) claimed %d, known "
                   "(truck/trailer) %d/%d learnt, at most %d/%d, %lld inherited, LOD phase ends checked %d, views with draws "
                   "lost %d\n", main_view, kChurnJunk, churn_poses, churn_claims, known_learnt[0], known_learnt[1], churn_known_most[0],
                   churn_known_most[1], joined() - inherited0, churn_checks, churn_lost);
            bool grew = false;
            for (int u = 0; u < rig::kUnits; ++u) grew = grew || churn_known_most[u] > 3 * known_learnt[u];
            if (churn_claims || grew || churn_lost) {
                printf("  FAIL: %s\n", churn_claims ? "made-up draws claimed" : grew ? "the known members grew past one per LOD"
                                                                                     : "draws lost after a LOD swap");
                ++failures;
            }
            failures += Summary(shared_most);
            continue;
        }
        for (int vi = 0; vi < rig::kMaxViews; ++vi) {
            bool any = false;
            for (int fr = 0; fr < frames; ++fr) any = any || claimed[fr][vi][0] || claimed[fr][vi][1];
            if (!any) continue;
            printf("  view %d claimed (truck/trailer):", vi);
            for (int fr = 0; fr < frames; ++fr) printf(" frame %d %d/%d", fr + 1, claimed[fr][vi][0], claimed[fr][vi][1]);
            if (hold) printf(" | held truck draws expected %d", expected_held[vi]);
            printf("\n");
            if (claimed[0][vi][0] || claimed[0][vi][1]) {
                printf("  FAIL: claims before anything was learned\n");
                ++failures;
            }
            if (turn_deg && claimed[frames - 1][vi][0] + claimed[frames - 1][vi][1] <
                                (claimed[frames - 2][vi][0] + claimed[frames - 2][vi][1]) * 95 / 100) {
                printf("  FAIL: draws lost while the camera turns\n");
                ++failures;
            }
            if (resend && (claimed[3][vi][0] < claimed[2][vi][0] * 95 / 100 ||
                           claimed[3][vi][1] < claimed[2][vi][1] * 95 / 100)) {
                printf("  FAIL: draws lost when the same configs were sent again\n");
                ++failures;
            }
            if (resend && (claimed[4][vi][0] < claimed[3][vi][0] * 95 / 100 || claimed[4][vi][1])) {
                printf("  FAIL: %s\n", claimed[4][vi][1] ? "trailer draws claimed after it went away"
                                                          : "truck draws lost when the trailer went away");
                ++failures;
            }
            for (int fr = 3; tip && fr < frames; ++fr)
                if (claimed[fr][vi][0] < claimed[2][vi][0] * 95 / 100 || claimed[fr][vi][1] < claimed[2][vi][1] * 95 / 100) {
                    printf("  FAIL: draws lost while the wheels hang (frame %d)\n", fr + 1);
                    ++failures;
                }
            for (int fr = 3; (lod || deflect) && fr < frames; ++fr) {
                // The new meshes' first two frames: seen, then learnt (N14: on their second sighting).
                if (lod && (fr == kLodFrom || fr == kLodFrom + 1)) continue;
                if (claimed[fr][vi][0] < claimed[2][vi][0] * 95 / 100 || claimed[fr][vi][1] < claimed[2][vi][1] * 95 / 100) {
                    printf("  FAIL: draws lost %s (frame %d)\n", lod ? "after the LOD swap" : "while the suspension works", fr + 1);
                    ++failures;
                }
            }
            if (hold && claimed[frames - 1][vi][0] < expected_held[vi] * 8 / 10) {
                printf("  FAIL: the truck's non-rigid draws were not held\n");
                ++failures;
            }
        }
        // N15, the plain run: the capture's own cab offset against the cab's draws. In the V3a truck the cab
        // is drawn at the truck's origin turned by the offset (39 draws a view): with the offset moving it
        // 1 cm or more, some draws must sit at the cab's frame (GetFrame, cab) within 5 mm (the worst capture,
        // run2 013, is 3.9 mm off: the telemetry is not quite the drawn frame's). Read the wrong way round
        // (transposed, until the D4d), none did in the 9 captures where the cab moves that much.
        if (plain && have_cab) {
            constexpr float kCabFit = 0.005f;
            int at_cab = 0, at_chassis = 0;
            float move = 0.f;
            for (int vi = 0; vi < rig::kMaxViews; ++vi) {
                float fc[12], fk[12];
                if (!rig::GetFrame(vi, 0, fk) || !rig::GetFrame(vi, 0, fc, true)) continue;
                move = (std::max)(move, PoseGap(fc, fk));
                for (const Draw& d : draws) {
                    if (d.view != vi) continue;
                    if (PoseGap(d.m, fc) < kCabFit) ++at_cab;
                    else if (PoseGap(d.m, fk) < kCabFit) ++at_chassis;
                }
            }
            printf("  cab check: the offset moves the cab %.1f cm; draws at the cab's frame %d, at the chassis' %d\n",
                   move * 100.f, at_cab, at_chassis);
            if (move >= 0.01f && !at_cab) {
                printf("  FAIL: no draw at the cab's frame\n");
                ++failures;
            }
        }
        // N20, the plain run: each wheel's reach outwards, from the parts learnt on its axle (cm out of the SDK's
        // position): the V3a truck's ~3 (front hub caps), its trailer's ~20 (hub caps; the SDK gives the axle's end).
        if (plain) {
            rig::MeasureWheelReach();
            printf("  wheel reach (cm):");
            for (int u = 0; u < rig::kUnits; ++u) {
                if (!cfg[u].wheels) continue;
                float reach[rig::kMaxWheels];
                rig::GetWheelReach(u, reach);
                printf(u ? "; trailer" : " truck");
                for (int w = 0; w < cfg[u].wheels; ++w) printf(" %.0f", reach[w] * 100.f);
            }
            printf("\n");
        }
        // N24, the plain run with a unit hooked behind the trailer (a "next" line: the D4m ATS captures of the
        // double grain hopper): the trailer's box ends just behind that unit's hook and the trailer claims
        // nothing past it (the dolly's drawbar, wheels and fifth wheel; the second hopper's front).
        if (plain && next) {
            const float tail = rig::GetStats().tail[1];
            int trailer_claims = 0, behind = 0;
            for (size_t i = 0; i < draws.size(); ++i) {
                float tf[12], fi[12];
                if (unit_prev[i] != 1 || !rig::GetFrame(draws[i].view, 1, tf)) continue;
                InvRigid34(tf, fi);
                const float* m = draws[i].m;
                ++trailer_claims;
                if (fi[8] * m[3] + fi[9] * m[7] + fi[10] * m[11] + fi[11] > tail + 0.01f) ++behind;
            }
            printf("  unit behind (N24): the trailer's box ends at z %.2f; trailer claims %d, %d of them past it\n", tail,
                   trailer_claims, behind);
            if (tail <= 0.f || behind) {
                printf("  FAIL: the trailer's box does not end at the next unit's hook\n");
                ++failures;
            }
        }
        // N21, the plain run with units past trailer.0 (a road train): what each claims, and no two units at one
        // place in a view (two identical trailers each tracked on its own).
        if (plain && cfg[2].wheels) {
            int claims[rig::kUnits] = {}, shared = 0, views[rig::kUnits] = {};
            for (size_t i = 0; i < draws.size(); ++i)
                if (unit_prev[i] >= 0) ++claims[unit_prev[i]];
            for (int vi = 0; vi < rig::kMaxViews; ++vi) {
                float fu[rig::kUnits][12];
                bool ok[rig::kUnits] = {};
                for (int u = 0; u < rig::kUnits; ++u) {
                    ok[u] = cfg[u].wheels && rig::GetFrame(vi, u, fu[u]);
                    views[u] += ok[u] ? 1 : 0;
                }
                for (int ua = 0; ua < rig::kUnits; ++ua)
                    for (int ub = ua + 1; ub < rig::kUnits; ++ub) {
                        if (!ok[ua] || !ok[ub]) continue;
                        const float dx = fu[ua][3] - fu[ub][3], dy = fu[ua][7] - fu[ub][7], dz = fu[ua][11] - fu[ub][11];
                        if (dx * dx + dy * dy + dz * dz < 0.25f) ++shared;
                    }
            }
            printf("  road train (N21):");
            for (int u = 1; u < rig::kUnits; ++u)
                if (cfg[u].wheels) printf(" trailer.%d claims %d in %d view(s);", u - 1, claims[u], views[u]);
            printf(" units at one place %d\n", shared);
            if (shared) {
                printf("  FAIL: two units tracked at one place\n");
                ++failures;
            }
        }
        if (cab_test && have_cab) {
            const long long cab_claims = rig::GetStats().cab_claims - cab_claims0;
            printf("  cab test: %d cab draws moved, %lld claims as cab; %d wheel draws moved with the cab, %d claimed as cab\n",
                   cab_draws, cab_claims, wheel_draws, wheel_cab_claims);
            bool lost = false;
            for (int vi = 0; vi < rig::kMaxViews; ++vi)
                lost = lost || claimed[frames - 1][vi][0] < claimed[2][vi][0] * 95 / 100;
            if (lost || (cab_draws && !cab_claims) || wheel_cab_claims) {
                printf("  FAIL: %s\n", lost               ? "truck draws lost while the cab moves"
                                      : wheel_cab_claims ? "a wheel's draw claimed as the cab's"
                                                         : "no draw learnt as the cab's");
                ++failures;
            }
        }
        bool truck_seen = false;  // RIGTEST_GRASS: the truck claimed in a capture's view in the third frame
        for (int vi = 0; vi < rig::kMaxViews - 1; ++vi) truck_seen = truck_seen || claimed[2][vi][0] > 0;
        if (grass_test && !truck_seen) printf("  grass test: the truck is in no view, nothing to test\n");
        if (grass_test && truck_seen) {
            bool lost = false;
            for (int vi = 0; vi < rig::kMaxViews - 1; ++vi)
                lost = lost || claimed[frames - 1][vi][0] < claimed[2][vi][0] * 95 / 100;
            printf("  grass test: %d made-up draws a frame in view %d, %d claims of them, %lld searches rejected\n",
                   veg_draws, rig::kMaxViews - 1, veg_claims, rig::GetStats().rejected - rejected0);
            if (veg_claims || lost) {
                printf("  FAIL: %s\n", veg_claims ? "the made-up draws were claimed" : "truck draws lost");
                ++failures;
            }
        }
        if (artic) {
            // Each unit keeps its claims of the third frame while the trailer turns and back in line.
            bool lost = false;
            for (int vi = 0; vi < rig::kMaxViews; ++vi)
                for (int fr = 3; fr < frames; ++fr)
                    for (int u = 0; u < rig::kUnits; ++u) lost = lost || claimed[fr][vi][u] < claimed[2][vi][u] * 95 / 100;
            // The made-up part: who claims it per frame, in the first view that draws it.
            std::string owners;
            int first = -1;
            bool truck_at_end = true;
            for (int vi = 0; vi < rig::kMaxViews; ++vi) {
                if (stack_unit[1][vi] == -2) continue;
                if (first < 0) first = vi;
                truck_at_end = truck_at_end && stack_unit[frames - 1][vi] == 0;
            }
            for (int fr = 1; first >= 0 && fr < frames; ++fr) owners += stack_unit[fr][first] == 0 ? 'T' : stack_unit[fr][first] == 1 ? 'R' : '-';
            printf("  artic test: up to %.0f degrees, draws claimed by the other unit %d, made-up part (view %d, "
                   "frames 2-%d; T truck, R trailer) %s; the trailer's frame off by %.1f cm at most\n", kArticDeg[8], artic_swapped,
                   first, frames, owners.c_str(), trailer_off * 100.f);
            const bool off = trailer_off > 0.01f;
            if (lost || artic_swapped || (first >= 0 && !truck_at_end) || off) {
                printf("  FAIL: %s\n", lost ? "draws lost while the trailer turns" : artic_swapped ? "draws claimed by the other unit"
                                       : off ? "the trailer's frame follows something else (the truck)"
                                             : "the made-up truck part is not the truck's");
                ++failures;
            }
        }
        if (tip) printf("  tip test: %d wheel draws hang %.2f m\n", hanging_n, kDrop);
        if (lod) {
            printf("  lod test: other meshes from frame %d (the old ones in frame %d), %lld inherited, claims of "
                   "draws not the unit's in frame 3 %d, grass (view %d) claims %d\n", kLodFrom + 1, kLodBack + 1,
                   joined() - inherited0, lod_strays, grass_ok ? main_view : -1, grass_claims);
            // No grass without a truck part to put it by: only where the truck is in no view (nothing to test).
            if (lod_strays || grass_claims || (!grass_ok && claimed[2][main_view][0])) {
                printf("  FAIL: %s\n", lod_strays ? "draws claimed that were not the unit's" : grass_claims ? "grass claimed"
                                                                                                  : "no truck part for the grass");
                ++failures;
            }
        }
        if (deflect) {
            printf("  deflection test: %d wheel draws, front wheels up to %.2f m, a wheel draw's place in its wheel moved "
                   "%.2f cm at most, GetWheels off the config + deflection by %.6f m at most\n", hanging_n, kDeflRise[4],
                   wheel_drift, wheel_off);
            bool rig_seen = false;  // a capture with the rig in no view has no wheel draw (nothing to test)
            for (int vi = 0; vi < rig::kMaxViews; ++vi) rig_seen = rig_seen || claimed[2][vi][0] || claimed[2][vi][1];
            if ((!hanging_n && rig_seen) || wheel_drift > 0.5f || wheel_off > 1e-5f) {
                printf("  FAIL: %s\n", !hanging_n && rig_seen ? "no wheel draw" : "the wheels' dirt does not follow the wheels' deflection");
                ++failures;
            }
        }
        if (lodcab && have_cab && !lc_views) printf("  lodcab test: the truck is in no view, nothing to test\n");
        if (lodcab && have_cab && lc_views) {
            printf("  lodcab test: %d views; cab parts claimed as the cab's before the chassis' swap %d of %d; after the "
                   "swaps: chassis part claimed %d of %d (as the cab's %d), cab part claimed %d of %d (as the cab's %d); "
                   "the chassis part claimed as the cab's in any frame %d; %lld inherited (%lld unclassed)\n", lc_views,
                   lc_ready, lc_ready_n, lc_claimed[0], lc_drawn[0], lc_as_cab[0], lc_claimed[1], lc_drawn[1], lc_as_cab[1],
                   lc_chassis_cab, joined() - inherited0, rig::GetStats().unclassed - unclassed0);
            bool lost = false;
            for (int vi = 0; vi < rig::kMaxViews; ++vi)
                lost = lost || claimed[frames - 1][vi][0] < claimed[2][vi][0] * 95 / 100;
            if (lc_ready < lc_ready_n || lc_chassis_cab || lc_claimed[0] < lc_drawn[0] || lc_as_cab[1] < lc_drawn[1] || lost) {
                printf("  FAIL: %s\n", lc_ready < lc_ready_n     ? "the cab parts were not learnt as the cab's"
                                        : lc_chassis_cab          ? "the chassis part took the cab's class"
                                        : lc_claimed[0] < lc_drawn[0] ? "the chassis part's new mesh not claimed"
                                        : lost                    ? "truck draws lost"
                                                                  : "the cab part's new mesh not claimed as the cab's");
                ++failures;
            }
        }
        if (interior_draws) printf("  interior draws %d, claimed %d\n", interior_draws, interior_claims);
        if (interior_claims) {
            printf("  FAIL: interior draws claimed\n");
            ++failures;
        }
        failures += Summary(shared_most);
    }
    for (int p = 0; churn && p < 2; ++p)
        if (ms_frames[p])
            fprintf(stderr, "time per frame, parked with LOD swaps and new meshes%s: Classify %.3f ms, EndFrame %.3f ms (%d frames)\n",
                    p ? ", last 100 frames" : "", ms_total[p][0] / ms_frames[p], ms_total[p][1] / ms_frames[p], ms_frames[p]);
    if (search_test) {
        const char* names[3] = {"tracked", "away", "back"};
        for (int p = 0; p < 3; ++p)
            if (ms_frames[p])
                fprintf(stderr, "time per frame, %s: Classify %.3f ms, EndFrame %.3f ms (%d frames)\n", names[p],
                        ms_total[p][0] / ms_frames[p], ms_total[p][1] / ms_frames[p], ms_frames[p]);
    }
    printf(failures ? "FAILED\n" : "done\n");
    return failures ? 1 : 0;
}
