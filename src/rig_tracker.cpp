// V3 part 2: player rig tracker (see rig_tracker.h).

#include "rig_tracker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rig {
namespace {

constexpr float kWheelTol = 0.08f;     // wheel draw vs config position [verified: V3a, < 8 cm]
constexpr float kMemberTol = 0.10f;    // a known mesh at its known local position
// A wheel's parts may sit this far above or below the wheel's place this frame (config +
// deflection): with the truck on its side the wheels hang off the axles and the deflection no
// longer says where [estimativa: the rear wheels lost their dust for seconds, truck tipped over].
constexpr float kWheelSlack = 0.30f;
constexpr float kAnchorMove = 1.0f;    // frame-to-frame move of a unit in a view
constexpr float kAnchorTurn = 0.15f;   // largest rotation-matrix entry change frame to frame
// A frame-to-frame motion up to this is a camera move, beyond it a jump (no prediction).
constexpr float kPredictMove = 6.0f;
constexpr float kPredictTurn = 0.5f;
constexpr int kMinWheels = 3;
constexpr int kMinPredictedMembers = 3;
// N17: a frame found by the wheel search is taken only when it is the unit's for some other reason: the unit
// was tracked in that view the frame before and the find is where it was expected (a LOD swap of every part);
// or kFoundBody of its known parts away from the wheels are at their place; or meshes known as its wheels are
// at kMinWheels of its wheels (a mirror drawing the body in a LOD not learnt yet); or it knows fewer than
// kFoundBodyKnown parts away from its wheels (nothing to check against). Three wheel hits alone can be
// anything with draws where the wheels would be [estimativa: D4f test, the roadside vegetation in a mirror
// took the dirt's colour].
constexpr int kFoundBodyKnown = 8;
// N24: a unit's box ends this far behind the hook of the unit hooked behind it, where a dolly's drawbar
// starts. Up to the D4m it ran 3.9 m past the last axle: the dolly's wheels sat in the first trailer's box,
// were learnt as its parts driving straight and lost in the turns [verified: D4m ATS captures, the grain
// hopper's box to z 11.2 m, the dolly's hook at 8.125 m and its wheels at ~10.2 m, claimed as the hopper's].
constexpr float kTailPad = 0.3f;
// N21: the unit behind is towed by a drawbar (a dolly) when its hook is this far behind this unit's last axle; nearer
// (between the axles) it rests on a fifth wheel (a semi-trailer on the truck or on a dolly) [verified: D4m/D4s ATS
// captures: the dolly's hook 0.82 m behind the grain hopper's last axle; the second hopper's on the dolly's axle].
constexpr float kDrawbarBehind = 0.25f;
constexpr int kFoundBody = 2;
// D13 (the user, ATS 30/10: back to the exterior camera, ~1 s before the bumper's dirt came back): a view where a
// unit is not tracked finds it by its known parts first: a sure, rigid member's draw gives a frame, taken when
// kAnchorParts of the unit's parts away from its wheels are at their place in it (the wheel search then waits for
// its N17 check). kAnchorTries draws are tried at most a view and frame. Only in a view that tracked the unit
// before (since its layout): the main view and the mirrors after a camera change, not a view that never showed it
// (the V3a captures' view 1 would take a frame there).
constexpr int kAnchorParts = 6, kAnchorTries = 8;
constexpr int kLostAfter = 10;         // frames in a row a view is not drawn (mirrors in the exterior camera)
constexpr float kSamePose = 0.03f;     // a box draw seen again at the same local position
constexpr double kPromoteMove = 1.0;   // world move of the unit before a box draw joins (m)
constexpr int kMinHeld = 2;            // known draws matching the previous frame's pose to hold it
constexpr size_t kMaxPending = 20000;
constexpr float kCabVoteMag = 0.015f;  // the cab's move (m, 2 m from the pivot) from which members vote
constexpr float kCabVoteDiff = 0.008f; // one hypothesis must fit this much better
constexpr float kCabRest = 0.015f;     // below this move an unclassified part may anchor the frame
constexpr int kCabSure = 3;            // votes to call a member cab (or chassis, negative)
// Searches without a find in a row before a view's search backs off (1, 3, 7, then 15 frames
// skipped), until the unit is tracked there or kMinWheels of its known meshes are drawn there (a
// unit in sight draws at least its wheels; one or two are a shared mesh, a tyre in traffic).
constexpr int kSearchBackoff = 3;
// Known members: one not matched while its unit drove this far is forgotten (a mesh the game no
// longer draws: another LOD, a part removed, a buffer reused), checked every kAgeSweep of driving.
// Distance, not frames or time: the same at any FPS, and a parked rig forgets nothing. When the
// unit drives with only some parts in sight (interior camera), the others are learnt again after
// 1 m of driving with them in sight.
constexpr double kForget = 50000.0;    // m [estimativa]
constexpr double kAgeSweep = 1000.0;   // m
constexpr double kOdoJump = 50.0;      // a placement step beyond this is a jump (ferry, teleport), not driving
// N13: a new mesh at a known member's place (kSamePose, rotation within this) while that member is not
// drawn in the view: the game swapped the part's LOD [estimativa: D4b test, the truck's draws went 97 <-> 74
// while it stood still, and the new meshes waited for 1 m of driving].
constexpr float kLodTurn = 0.02f;
// N14: the D4c test showed the inheritance adopting far too many meshes (known 250 -> 3200-4000, 65501
// inherited, rig end-frame 50 -> 200-620 us) and a chassis part taking the cab's class from a cab part at the
// same place (the truck's origin, unturned) [verified: D4c test, gpu_probe.log]. So a new mesh inherits only:
//  - once it has been seen in kLodSightings frames, none more than kPendingGap apart: a mesh drawn once (a
//    ring buffer: a new key every frame, about one inherited a frame in the D4c log [estimativa]) never
//    does, while a LOD flickering every frame at a LOD boundary does;
//  - from a member last drawn in that view from the frame before the new mesh first appeared, and not this
//    frame: the part it replaces there;
//  - kMaxHeirs times per parent at most;
// and it copies the parent's cab votes only when every member at that place is of one class (cab, chassis
// or not yet known); else it starts at 0 and votes as a new member.
constexpr int kLodSightings = 2;
constexpr int kMaxHeirs = 2;
// D12b (the user, ATS 30/10, a Volvo VNL 2025: "stuttering and drops to 25 fps in the exterior camera"): that truck
// draws most of its parts at its origin, unturned, and the game loads its meshes into other buffers again and again
// (the same index count at the same place, another buffer: a new key). Each one was another LOD of the one it
// replaced (N13): known 100 -> 3851, 1359 of them at the origin, and the LOD lookups over them took the rig's
// end-frame to 5-47 ms a frame [verified: ATS gpu_probe.log 20:58-21:00, the map in TruckDirt\rigs]. A new mesh at
// the place of a member with the same index count that stopped being drawn when it appeared is that member in
// another buffer: it joins with the member's votes, and the other copies there go (two copies at most: the game
// may go back to the last buffer, as RIGTEST_LOD and RIGTEST_CHURN do, but they must not grow without end).
// N20: each wheel's parts on its axle (members turned about x only, kOnAxle of the wheel's centre in y and z,
// on its side from kAxleIn inwards to kAxleOut outwards of the SDK's wheel position): the outermost one
// says how far out the wheel reaches. The SDK's position is the tyre's middle on the V3a truck (front hub
// cap 3 cm out) but the axle's end on its trailer (hub caps 20 cm out) [verified: V3a captures; D4h test].
constexpr float kOnAxle = 0.12f;
constexpr float kAxleIn = 0.6f, kAxleOut = 1.0f;
constexpr float kAxial = 0.99f;  // |r[0][0]|: the part's x stays the wheel's axle
// N47: a map's part not found again while the unit drove this far (in all sessions) leaves the map: a part taken
// off, another LOD never drawn [estimativa]; the map is written every kMapEvery frames at most.
constexpr double kMapForget = 300000.0;  // m
// D12c: the driver's seat (the eye moved down and back to the chest) and how far from it a draw is the driver's:
// the door (x 1.2 m out) and the cab's parts (at the cab's origin) are farther [estimativa: the VNL's driver draws
// within 0.17 m of it].
constexpr float kDriverDown = 0.15f, kDriverBack = 0.2f, kDriverRadius = 0.6f;
constexpr long long kMapEvery = 120;
constexpr const char* kMapHeader = "truck dirt rig map 1";

struct Mat {
    float r[3][3];
    float t[3];
};

Mat FromRows(const float m[12]) {
    Mat a;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) a.r[i][j] = m[i * 4 + j];
        a.t[i] = m[i * 4 + 3];
    }
    return a;
}

Mat Mul(const Mat& a, const Mat& b) {
    Mat c;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) c.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
        c.t[i] = a.r[i][0] * b.t[0] + a.r[i][1] * b.t[1] + a.r[i][2] * b.t[2] + a.t[i];
    }
    return c;
}

// The parts' matrices are rotations (mirrors add a reflection): inverse = transpose.
Mat Inv(const Mat& a) {
    Mat c;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) c.r[i][j] = a.r[j][i];
    for (int i = 0; i < 3; ++i) c.t[i] = -(c.r[i][0] * a.t[0] + c.r[i][1] * a.t[1] + c.r[i][2] * a.t[2]);
    return c;
}

void Apply(const Mat& a, const float p[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = a.r[i][0] * p[0] + a.r[i][1] * p[1] + a.r[i][2] * p[2] + a.t[i];
}

float Dist(const float a[3], const float b[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// Rotation of b seen from a (a^T b).
void RelRot(const Mat& a, const Mat& b, float rel[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) rel[i][j] = a.r[0][i] * b.r[0][j] + a.r[1][i] * b.r[1][j] + a.r[2][i] * b.r[2][j];
}

bool NearIdentity(const float rel[3][3], float tol) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::fabs(rel[i][j] - (i == j ? 1.f : 0.f)) > tol) return false;
    return true;
}

// The unit's own parts: the unit's rotation, axis-swapped (90-degree pieces),
// or spinning about the unit's X axis (wheels, axles, cargo pipes) [verified: V3a].
bool PartRotation(const float rel[3][3]) {
    if (std::fabs(rel[0][0]) > 0.995f) return true;  // about X
    for (int i = 0; i < 3; ++i) {
        int ones = 0;
        for (int j = 0; j < 3; ++j) {
            const float v = std::fabs(rel[i][j]);
            if (v > 0.98f) ++ones;
            else if (v > 0.03f) return false;
        }
        if (ones != 1) return false;
    }
    return true;
}

bool MatClose(const Mat& a, const Mat& b, float move = kAnchorMove, float turn = kAnchorTurn) {
    if (Dist(a.t, b.t) > move) return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::fabs(a.r[i][j] - b.r[i][j]) > turn) return false;
    return true;
}

uint64_t KeyHash(const DrawKey& k) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    mix(reinterpret_cast<uintptr_t>(k.vb));
    mix(reinterpret_cast<uintptr_t>(k.ib));
    mix(k.start);
    mix(static_cast<uint32_t>(k.base));
    mix(k.count);
    return h;
}

struct Member {
    Mat local;   // pose in the unit's frame when it was learned
    bool rigid;  // same rotation as the unit: can re-anchor the frame
    bool wheel;  // at a wheel: matched against the wheel's position this frame (deflection)
    int cab = 0; // votes: > 0 moves with the cab, < 0 with the chassis
    Mat rest{};  // the same pose with the cab's motion at that time taken out (a cab part at rest)
    long long seen_frame = 0;  // last frame it was matched (which members go first past kMaxKnown)
    double seen_odo = 0;       // the unit's odometer then (kForget)
    // Seen where it could not be the other unit's (at this unit's wheel, outside the other's box or
    // off its parts' rotation: the trailer turned): the unit's for sure. Where the two boxes overlap
    // and the units are in line, a draw fits both; the one sure of it keeps it.
    bool sure = false;
    long long drawn[kMaxViews] = {};  // N14: the last frame it was matched in Learn in each view (0: never)
    int heirs = 0;                    // N14: new meshes that inherited from it (kMaxHeirs)
    uint32_t count = 0;               // N47: the draw's index count (its map entry)
};
// N16: a wheel's part is never the cab's. It sits at the wheel's place this frame (config + deflection), and
// braking moves it up while the cab dives: closer to the cab's predicted motion than to the place it was learnt
// at, it voted cab, and the top of the tyre took the cab's frame [estimativa: D4e test, the cab's dirt over the
// top of the tyre moved with the cab under hard braking]. So wheel parts never vote and are never claimed as cab.
bool IsCab(const Member& m) { return !m.wheel && m.cab >= kCabSure; }
bool IsChassis(const Member& m) { return m.cab <= -kCabSure; }

struct Track {
    bool prev = false, cur = false;
    Mat f_prev{}, f_cur{};
    Mat f_pred{};  // the previous pose moved on by the last frame-to-frame motion (camera turning)
    int miss = 0;
    int held = 0;   // draws claimed this frame at the previous frame's pose
    int seen = 0;   // draws of the unit's known meshes in the view this frame
    int fails = 0;  // searches in a row that found nothing
    int wait = 0;   // frames left before the next search
};

using Members = std::unordered_map<uint64_t, std::vector<Member>>;

// A box draw not yet known: where the unit was in the world when it was first
// seen, and the last frame it was seen at this local position (it must stay
// there frame after frame: repeated road-side meshes only pass by).
struct Pending {
    Mat local;
    bool rigid;
    double world[3];
    long long last;
    long long lone = -1;  // N13: no member was known at its place at this version of the members (PlaceIndex)
    long long first;      // N14: the frame it was first seen
    int sightings = 1;    // N14: frames it was seen in, none more than kPendingGap apart
};
constexpr long long kPendingGap = 5;  // frames a pending draw may go unseen
using PendingMap = std::unordered_map<uint64_t, std::vector<Pending>>;

struct FrameDraw {
    uint64_t key;
    Mat m;
    uint32_t count;  // N47
};

struct View {
    Track unit[kUnits];
    int group = -1;
    std::vector<FrameDraw> draws;
};

struct Box {
    float lo[3], hi[3];
};

// Game-thread input, copied at BeginFrame.
std::mutex g_in_mutex;
UnitConfig g_in_cfg[kUnits];
double g_in_place[kUnits][6] = {};
bool g_in_placed[kUnits] = {};
float g_in_defl[kUnits][kMaxWheels] = {};
bool g_in_changed[kUnits] = {};
bool g_in_next[kUnits] = {};  // N24: the unit hooked behind, its hook.position and placement
float g_in_next_hook[kUnits][3] = {};
double g_in_next_place[kUnits][6] = {};
float g_tail[kUnits] = {};  // N24: where the box ends behind (local z; 0: no unit behind)
bool g_drawbar[kUnits] = {};  // N21: the unit behind (u + 1) is towed by a drawbar, not on a fifth wheel

UnitConfig g_cfg[kUnits];
Box g_box[kUnits];
double g_place[kUnits][6] = {};
bool g_placed[kUnits] = {};
float g_wheel[kUnits][kMaxWheels][3];  // this frame: config position + deflection
// The truck's cab: pivot, offset this frame, and the cab's motion in the
// truck's frame (rest pose -> now) with its size (move 2 m from the pivot).
bool g_in_has_cab = false, g_has_cab = false;
bool g_in_has_driver = false, g_has_driver = false;  // D12c
float g_in_driver[3] = {}, g_driver[3] = {};
float g_in_pivot[3] = {}, g_in_cab_off[6] = {};
Mat g_cab{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {0, 0, 0}};
Mat g_cab_inv{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {0, 0, 0}};  // D14b: its inverse, once a frame (was per draw)
float g_cab_mag = 0.f;
View g_views[kMaxViews];
bool g_tracked_once[kMaxViews][kUnits] = {};  // D13: the view tracked the unit since its layout
Members g_members[kUnits];  // known meshes and positions, from every view (until the layout changes)
std::vector<Member> g_no_members;  // stays empty
int g_known_n[kUnits] = {};        // members in g_members[u]
double g_odo[kUnits] = {};         // distance driven (placement steps), m
double g_odo_place[kUnits][3] = {};
bool g_odo_valid[kUnits] = {};
double g_swept[kUnits] = {};       // g_odo at the last age sweep
PendingMap g_pending[kUnits];
// N13: the members away from the wheels by place (IndexPlaces), for a new LOD to find the member it
// replaces. Rebuilt when needed after the members changed (version).
struct PlaceEntry {
    uint64_t key;
    int index;     // in the key's list
    bool at_rest;  // the truck's rest pose (cab parts), else the learnt local position
};
struct PlaceIndex {
    long long version = -1;
    std::unordered_map<uint64_t, std::vector<PlaceEntry>> cells;  // 0.1 m cells
};
PlaceIndex g_place_index[kUnits];
long long g_members_version[kUnits] = {};
Stats g_stats;
std::mutex g_stats_mutex;
// N47: the unit's map from an earlier session: a part away from the wheels, by its index count and pose.
struct Saved {
    uint32_t count = 0;
    Mat local{}, rest{};
    int cab = 0;
    bool sure = false;
    double km = 0;      // driven without it being found, up to the import
    bool used = false;  // found again this session (a member now)
};
std::vector<Saved> g_in_saved[kUnits];  // ImportMap -> BeginFrame (g_in_mutex)

// N55 (the user, ATS 31/10: "the box that keeps the dust out of the cab is coming out of the truck... every truck has
// its own shape and size"): the cab's inside learned per truck. In the interior camera the game draws the cabin with
// stencil ref 2 [verified: D11l captures, 72 meshes in the main view]; in the mirrors' views ref 2 is the cab's outer
// shell (20 meshes, 4 of them in the main view too), and 14 of the main view's meshes were the plugin's dusty ones in
// the outside camera (the dashboard and the seats seen through the glass). An inside part: a mesh (index count) at a
// place in the cab's frame, drawn with ref 2 in the main view for kInsideVotes looks and never in a mirror; in every
// other view its draws get no dust.
struct Inside {
    uint32_t count = 0;
    float p[3] = {};
    int main = 0, shell = 0;
};
constexpr float kInsideTol = 0.05f;  // m, the place in the cab's frame
constexpr int kInsideVotes = 3;      // looks in the main view before a part counts as inside
constexpr int kMaxInside = 600;
constexpr int kInsideEvery = 4;      // frames between looks at the interior's draws
constexpr int kInsideEveryKnown = 32;  // D14b: and once kInsideKnown parts count (new ones still join, slower)
constexpr int kInsideKnown = 10;
bool g_inside_look = false;  // D14b: this frame looks (decided at BeginFrame: the whole look in one frame)
// D14c: the main view's ref-2 draws of this look (the truck is not tracked there), placed at EndFrame.
struct LookDraw {
    uint64_t k;
    uint32_t count;
    Mat m;
};
std::vector<LookDraw> g_look;
constexpr int kLookAgree = 3;  // known meshes agreeing on the truck's frame before the look counts
std::vector<Inside> g_inside;        // the truck's (unit 0)
std::unordered_map<uint32_t, std::vector<int>> g_inside_by_count;
int g_inside_known = 0;              // of them, counting as inside
int g_out_inside = 0;                // g_inside_known at the last WriteMap of the truck
std::vector<Inside> g_in_inside;     // ImportMap -> BeginFrame (g_in_mutex)
bool g_in_inside_set = false;
std::string g_in_map_key[kUnits];
bool g_in_map[kUnits] = {};
std::vector<Saved> g_saved[kUnits];
std::unordered_map<uint32_t, std::vector<int>> g_saved_by_count[kUnits];
std::string g_map_key[kUnits];   // empty: the unit's vehicle not known (no map read or written)
double g_map_odo[kUnits] = {};   // g_odo at the import
int g_map_used[kUnits] = {};
// The map as last written (ExportMap, any thread).
std::mutex g_map_mutex;
std::string g_out_key[kUnits];
std::vector<MapPart> g_out_parts[kUnits];
long long g_out_serial[kUnits] = {};
long long g_out_version[kUnits] = {-1, -1, -1, -1, -1, -1};
long long g_out_frame[kUnits] = {};
double g_out_odo[kUnits] = {};
// N20: per wheel, how far out of its SDK position its outermost part on the axle sits (m, >= 0), and the parts
// found there (MeasureWheels; render thread).
float g_wheel_out[kUnits][kMaxWheels] = {};
int g_wheel_parts[kUnits][kMaxWheels] = {};

// Box around the unit from its wheels (and the trailer's hook) [verified: V3a boxes].
Box MakeBox(const UnitConfig& c) {
    Box b{{0, -0.4f, 0}, {0, 4.4f, 0}};
    float xmax = 0, zmin = 1e9f, zmax = -1e9f;
    for (int w = 0; w < c.wheels; ++w) {
        xmax = (std::max)(xmax, std::fabs(c.wheel[w][0]));
        zmin = (std::min)(zmin, c.wheel[w][2]);
        zmax = (std::max)(zmax, c.wheel[w][2]);
    }
    b.lo[0] = -(xmax + 0.7f), b.hi[0] = xmax + 0.7f;
    if (c.has_hook) {
        b.lo[2] = (std::min)(zmin, c.hook[2]) - 1.8f;
        b.hi[2] = zmax + 3.9f;
    } else {
        b.lo[2] = zmin - 1.6f;
        b.hi[2] = zmax + 1.8f;
    }
    return b;
}

bool InBox(const Box& b, const float p[3]) {
    for (int i = 0; i < 3; ++i)
        if (p[i] < b.lo[i] || p[i] > b.hi[i]) return false;
    return true;
}

// World placement as a frame (SCS: heading about +Y with 0 = -Z forward, then
// pitch about X, then roll about Z, in turns [verified: V3a, trailer pose]).
void WorldFrame(const double p[6], double r[3][3], double t[3]) {
    const double kTurn = 6.283185307179586;
    const double a = p[3] * kTurn, b = p[4] * kTurn, c = p[5] * kTurn;
    const double ry[3][3] = {{std::cos(a), 0, std::sin(a)}, {0, 1, 0}, {-std::sin(a), 0, std::cos(a)}};
    const double rx[3][3] = {{1, 0, 0}, {0, std::cos(b), -std::sin(b)}, {0, std::sin(b), std::cos(b)}};
    const double rz[3][3] = {{std::cos(c), -std::sin(c), 0}, {std::sin(c), std::cos(c), 0}, {0, 0, 1}};
    double tmp[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) tmp[i][j] = ry[i][0] * rx[0][j] + ry[i][1] * rx[1][j] + ry[i][2] * rx[2][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = tmp[i][0] * rz[0][j] + tmp[i][1] * rz[1][j] + tmp[i][2] * rz[2][j];
    for (int i = 0; i < 3; ++i) t[i] = p[i];
}

// Frame of unit `to` expressed in the frame of unit `from` (double precision: world coordinates are large).
Mat Relative(int from, int to) {
    double ra[3][3], ta[3], rb[3][3], tb[3];
    WorldFrame(g_place[from], ra, ta);
    WorldFrame(g_place[to], rb, tb);
    Mat m;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            m.r[i][j] = static_cast<float>(ra[0][i] * rb[0][j] + ra[1][i] * rb[1][j] + ra[2][i] * rb[2][j]);
        m.t[i] = static_cast<float>(ra[0][i] * (tb[0] - ta[0]) + ra[1][i] * (tb[1] - ta[1]) + ra[2][i] * (tb[2] - ta[2]));
    }
    return m;
}

// Spatial hash of the frame's draw positions, 0.5 m cells.
// D11n (the user, 30/10: "optimise the CPU too, without losing quality"; rigtest's search mode: a search took ~76 us,
// ~0.9 a frame in the game, most of it in this grid's hash map): flat, kept between frames (no allocation per cell or
// per search), and Near looks only at the cells the tolerance reaches (1-8 of 27). The same answers.
struct Grid {
    static constexpr uint64_t kEmpty = ~0ull;  // Cell uses 63 bits: never a cell
    std::vector<uint64_t> keys;                // open addressing, a power of two
    std::vector<int> first, count;             // per slot: its draws in `order`
    std::vector<int> order, slot_of, fill;     // the draws grouped by cell (in their order)
    size_t mask = 0;
    static uint64_t Cell(int x, int y, int z) {
        return (uint64_t(uint32_t(x) & 0x1FFFFF) << 42) | (uint64_t(uint32_t(y) & 0x1FFFFF) << 21) | (uint32_t(z) & 0x1FFFFF);
    }
    static int C(float v) { return static_cast<int>(std::floor(v * 2.f)); }
    static size_t Hash(uint64_t k) {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdull;
        return size_t(k ^ (k >> 33));
    }
    size_t Slot(uint64_t k) const {
        size_t i = Hash(k) & mask;
        while (keys[i] != kEmpty && keys[i] != k) i = (i + 1) & mask;
        return i;
    }
    void Build(const std::vector<FrameDraw>& d) {
        size_t cap = 64;
        while (cap < d.size() * 2) cap <<= 1;
        keys.assign(cap, kEmpty);
        count.assign(cap, 0);
        first.resize(cap);
        mask = cap - 1;
        slot_of.resize(d.size());
        for (size_t i = 0; i < d.size(); ++i) {
            const uint64_t k = Cell(C(d[i].m.t[0]), C(d[i].m.t[1]), C(d[i].m.t[2]));
            const size_t sl = Slot(k);
            keys[sl] = k;
            ++count[sl];
            slot_of[i] = static_cast<int>(sl);
        }
        int run = 0;
        for (size_t sl = 0; sl < cap; ++sl) first[sl] = run, run += count[sl];
        fill = first;
        order.resize(d.size());
        for (size_t i = 0; i < d.size(); ++i) order[fill[slot_of[i]]++] = static_cast<int>(i);
    }
    // Any draw within tol of p.
    bool Near(const std::vector<FrameDraw>& d, const float p[3], float tol) const {
        for (int x = C(p[0] - tol); x <= C(p[0] + tol); ++x)
            for (int y = C(p[1] - tol); y <= C(p[1] + tol); ++y)
                for (int z = C(p[2] - tol); z <= C(p[2] + tol); ++z) {
                    const size_t sl = Slot(Cell(x, y, z));
                    if (keys[sl] == kEmpty) continue;
                    for (int j = first[sl], e = first[sl] + count[sl]; j < e; ++j)
                        if (Dist(d[order[j]].m.t, p) < tol) return true;
                }
        return false;
    }
};

// D11n: the matrices a search tried (their hashes), flat and kept between searches.
struct TriedSet {
    std::vector<uint64_t> keys;
    size_t mask = 0;
    bool zero = false;  // 0 is the empty slot: hash 0 kept aside
    void Reset(size_t n) {
        size_t cap = 64;
        while (cap < n * 2) cap <<= 1;
        keys.assign(cap, 0);
        mask = cap - 1;
        zero = false;
    }
    // True when h was not there yet.
    bool Insert(uint64_t h) {
        if (!h) return zero ? false : (zero = true);
        size_t i = Grid::Hash(h) & mask;
        while (keys[i] && keys[i] != h) i = (i + 1) & mask;
        if (keys[i]) return false;
        keys[i] = h;
        return true;
    }
};
TriedSet g_tried;  // EndFrame's thread only

// Wheels with a draw at their place at frame f; stops early (less than need)
// once the wheels left cannot bring the count to need.
int WheelHits(const Grid& g, const std::vector<FrameDraw>& d, const Mat& f, int u, int need) {
    int hits = 0;
    const int n = g_cfg[u].wheels;
    for (int w = 0; w < n && hits + n - w >= need; ++w) {
        float p[3];
        Apply(f, g_wheel[u][w], p);
        if (g.Near(d, p, kWheelTol)) ++hits;
    }
    return hits;
}

// Sum of the distances from the unit's wheels at frame f to the nearest draws.
float WheelFit(const std::vector<FrameDraw>& d, const Mat& f, int u) {
    float sum = 0.f;
    for (int w = 0; w < g_cfg[u].wheels; ++w) {
        float p[3];
        Apply(f, g_wheel[u][w], p);
        float best = kWheelTol;
        for (const FrameDraw& fd : d) best = (std::min)(best, Dist(fd.m.t, p));
        sum += best;
    }
    return sum;
}

// taken: frames of units already found in the view; a candidate there is that unit (N21: two identical trailers
// in a road train each take their own).
bool Search(const Grid& g, const std::vector<FrameDraw>& d, int u, const Track& tr, Mat* out, const Mat* const* taken,
            int ntaken) {
    TriedSet& tried = g_tried;
    tried.Reset(d.size());
    int best = 0;
    float best_fit = 1e9f, best_dist = 1e9f;
    for (const FrameDraw& fd : d) {
        // The same matrix is shared by many parts: try it once.
        uint64_t h = 1469598103934665603ull;
        for (int i = 0; i < 3; ++i) {
            h = (h ^ uint64_t(int64_t(std::lround(fd.m.t[i] * 1000.f)))) * 1099511628211ull;
            for (int j = 0; j < 3; ++j) h = (h ^ uint64_t(int64_t(std::lround(fd.m.r[i][j] * 1000.f)))) * 1099511628211ull;
        }
        if (!tried.Insert(h)) continue;
        bool other = false;
        for (int i = 0; i < ntaken && !other; ++i) other = MatClose(fd.m, *taken[i]);
        if (other) continue;
        // A candidate below the best so far can never be taken.
        const int hits = WheelHits(g, d, fd.m, u, (std::max)(kMinWheels, best));
        if (hits < kMinWheels || hits < best) continue;
        // Ties: the closest fit to the wheels (the chassis; the cab fits worse when it rolls), then
        // the closest to the previous frame.
        const float fit = WheelFit(d, fd.m, u);
        const float dist = tr.prev ? Dist(fd.m.t, tr.f_prev.t) : 0.f;
        if (hits > best || (hits == best && (fit < best_fit - 0.005f || (fit < best_fit + 0.005f && dist < best_dist)))) {
            best = hits;
            best_fit = fit;
            best_dist = dist;
            *out = fd.m;
        }
    }
    return best >= kMinWheels;
}

// A wheel part's place: at one of the unit's wheels this frame, give or take the slack up and down.
bool NearWheel(int u, const float lp[3]) {
    for (int w = 0; w < g_cfg[u].wheels; ++w) {
        const float dx = lp[0] - g_wheel[u][w][0], dz = lp[2] - g_wheel[u][w][2];
        if (dx * dx + dz * dz < kWheelTol * kWheelTol * 2.25f && std::fabs(lp[1] - g_wheel[u][w][1]) < kWheelSlack)
            return true;
    }
    return false;
}

// A known member drawn at local position lp of its unit: its own place, the cab's motion applied (a
// cab part), or a wheel's place give or take the slack (a wheel part).
bool AtPose(int u, const Member& mb, const float lp[3]) {
    if (Dist(lp, mb.local.t) < kMemberTol) return true;
    if (u == 0 && g_has_cab && !IsChassis(mb) && !mb.wheel) {
        float moved[3];
        Apply(g_cab, mb.rest.t, moved);
        if (Dist(lp, moved) < kMemberTol) return true;
    }
    return mb.wheel && NearWheel(u, lp);
}

void Seen(Member& mb, int u) {
    mb.seen_frame = g_stats.frames;
    mb.seen_odo = g_odo[u];
}

void AddMember(int u, std::vector<Member>& list, const Member& mb) {
    if (list.size() >= 16) return;
    list.push_back(mb);
    Seen(list.back(), u);
    ++g_known_n[u];
    ++g_members_version[u];
}

// Drops the members `drop` says, and the meshes left without any.
template <class F>
void DropMembers(int u, F drop) {
    for (auto it = g_members[u].begin(); it != g_members[u].end();) {
        auto& list = it->second;
        const size_t before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(), drop), list.end());
        g_known_n[u] -= static_cast<int>(before - list.size());
        g_stats.forgotten += static_cast<long long>(before - list.size());
        if (before != list.size()) ++g_members_version[u];
        it = list.empty() ? g_members[u].erase(it) : std::next(it);
    }
}

// Drops one member of unit u (a draw that turned out to be the other unit's).
void DropMember(int u, uint64_t key, const Member* mb) {
    auto it = g_members[u].find(key);
    if (it == g_members[u].end()) return;
    auto& list = it->second;
    for (auto m = list.begin(); m != list.end(); ++m) {
        if (&*m != mb) continue;
        list.erase(m);
        --g_known_n[u];
        ++g_stats.handed;
        ++g_members_version[u];
        break;
    }
    if (list.empty()) g_members[u].erase(it);
}

// What the other unit o makes of a draw, from o's frame in the view (of, and its inverse):
// at o's wheel (o's), at a known member of o (that member), or where o's parts could be.
struct Rival {
    bool wheel = false;
    bool could = false;
    const Member* member = nullptr;
};

Rival Rivalry(int o, const FrameDraw& fd, const Mat& of, const Mat& of_inv) {
    Rival r;
    float lp[3];
    Apply(of_inv, fd.m.t, lp);
    for (int w = 0; w < g_cfg[o].wheels && !r.wheel; ++w) r.wheel = Dist(lp, g_wheel[o][w]) < kWheelTol;
    float rel[3][3];
    RelRot(of, fd.m, rel);
    r.could = r.wheel || (InBox(g_box[o], lp) && PartRotation(rel));
    const auto found = g_members[o].find(fd.key);
    if (found != g_members[o].end())
        for (const Member& mb : found->second)
            if (AtPose(o, mb, lp)) {
                r.member = &mb;
                break;
            }
    return r;
}

// N13: the 0.1 m cell of p, and the cells a sphere of radius kSamePose around p touches: 1 to 8, most often 1.
uint64_t PlaceCell(const float p[3]) {
    return Grid::Cell(static_cast<int>(std::floor(p[0] * 10.f)), static_cast<int>(std::floor(p[1] * 10.f)),
                      static_cast<int>(std::floor(p[2] * 10.f)));
}
template <class F>
void PlaceCells(const float p[3], F visit) {
    int lo[3], hi[3];
    for (int i = 0; i < 3; ++i) {
        lo[i] = static_cast<int>(std::floor((p[i] - kSamePose) * 10.f));
        hi[i] = static_cast<int>(std::floor((p[i] + kSamePose) * 10.f));
    }
    for (int x = lo[0]; x <= hi[0]; ++x)
        for (int y = lo[1]; y <= hi[1]; ++y)
            for (int z = lo[2]; z <= hi[2]; ++z) visit(Grid::Cell(x, y, z));
}

// N13: the members away from the wheels by place: their learnt local position, and for the truck also their
// rest pose (a cab part is found at the cab's motion applied to it). Must be up to date before FindLodPlace:
// members added after it are not in it (they were just drawn, so never a parent), and the ones in it
// keep their index in their lists.
double Seconds(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// D14c (the user, ATS 31/10: stutters looking around in the cab; the end-frame's worst 2.5-3.5 ms there, 0.3 outside):
// each part's time this frame, and its worst frame since TakeWorst.
enum Part { kPartSearch, kPartAnchor, kPartLearn, kPartIndex, kPartAge, kPartMap, kPartInside, kParts };
double g_part_frame[kParts] = {}, g_part_worst[kParts] = {};
void AddSec(double* total, Part p, double sec) {
    *total += sec;
    g_part_frame[p] += sec;
}

void IndexPlaces(int u) {
    PlaceIndex& ix = g_place_index[u];
    if (ix.version == g_members_version[u]) return;
    const auto t0 = std::chrono::steady_clock::now();  // D13
    ix.cells.clear();
    for (const auto& kv : g_members[u])
        for (int i = 0; i < static_cast<int>(kv.second.size()); ++i) {
            const Member& mb = kv.second[i];
            if (mb.wheel) continue;
            ix.cells[PlaceCell(mb.local.t)].push_back({kv.first, i, false});
            if (u == 0) ix.cells[PlaceCell(mb.rest.t)].push_back({kv.first, i, true});
        }
    ix.version = g_members_version[u];
    AddSec(&g_stats.sec_index, kPartIndex, Seconds(t0));
}

// N13, N14: the members of unit u away from the wheels whose place this frame is `local`'s, give or take
// kSamePose and kLodTurn: a chassis part's learnt place, a cab part's rest pose with the cab's motion applied,
// both for a part not yet known either way. Only where a member is known: grass and road never sit at a known
// part's place with its rotation.
struct LodPlace {
    bool any = false;           // some member there
    bool mixed = false;         // members of different classes there (cab, chassis, not yet known)
    Member* parent = nullptr;   // the part `local` is another LOD of (see kLodSightings), the fewest heirs first
    Member* same = nullptr;     // D12b: a member like `parent` with the new mesh's index count (the same mesh), the
    uint64_t same_key = 0;      // last drawn first
    std::vector<std::pair<uint64_t, const Member*>> copies;  // D12b: every such member (the older copies too)
};
// N47: the map's part with this index count at `local` (kSamePose, kLodTurn): a chassis part at its place, a
// cab part at its rest pose with the cab's motion applied. nullptr: none.
Saved* FindSaved(int u, uint32_t count, const Mat& local) {
    const auto it = g_saved_by_count[u].find(count);
    if (it == g_saved_by_count[u].end()) return nullptr;
    const bool cab_moves = u == 0 && g_has_cab;
    for (int i : it->second) {
        Saved& s = g_saved[u][i];
        for (int at_rest = 0; at_rest < (cab_moves ? 2 : 1); ++at_rest) {
            const Mat pose = at_rest ? Mul(g_cab, s.rest) : s.local;
            float rel[3][3];
            RelRot(pose, local, rel);
            if (Dist(pose.t, local.t) < kSamePose && NearIdentity(rel, kLodTurn)) return &s;
        }
    }
    return nullptr;
}

LodPlace FindLodPlace(int u, const Mat& local, int view, long long first, uint32_t count) {
    const PlaceIndex& ix = g_place_index[u];
    const long long now = g_stats.frames;
    const bool cab_moves = u == 0 && g_has_cab;
    LodPlace r;
    int cls = 0;
    // at_rest: the entries of rest poses, matched with the cab's motion applied (q: local taken back by it).
    auto look = [&](const float q[3], bool at_rest) {
        PlaceCells(q, [&](uint64_t cell) {
            const auto it = ix.cells.find(cell);
            if (it == ix.cells.end()) return;
            for (const PlaceEntry& e : it->second) {
                if (e.at_rest != at_rest) continue;
                Member& mb = g_members[u].find(e.key)->second[e.index];
                if (at_rest ? IsChassis(mb) : cab_moves && IsCab(mb)) continue;
                const Mat pose = at_rest ? Mul(g_cab, mb.rest) : mb.local;
                float rel[3][3];
                RelRot(pose, local, rel);
                if (Dist(pose.t, local.t) >= kSamePose || !NearIdentity(rel, kLodTurn)) continue;
                const int c = IsCab(mb) ? 1 : IsChassis(mb) ? -1 : 0;
                r.mixed = r.mixed || (r.any && c != cls);
                r.any = true;
                cls = c;
                const long long seen = mb.drawn[view];
                const bool gone = seen > 0 && seen >= first - 1 && seen < now;
                if (count && mb.count == count) {  // older copies are long gone: any drawn before
                    r.copies.push_back({e.key, &mb});
                    if (gone && (!r.same || seen > r.same->drawn[view])) r.same = &mb, r.same_key = e.key;
                }
                if (gone && mb.heirs < kMaxHeirs && (!r.parent || mb.heirs < r.parent->heirs)) r.parent = &mb;
            }
        });
    };
    look(local.t, false);
    if (cab_moves) {
        float q[3];
        Apply(g_cab_inv, local.t, q);
        look(q, true);
    }
    return r;
}

// Ageing (kForget) and the limit (kMaxKnown: the least recently matched quarter goes, but not
// members matched in the last two frames).
void AgeMembers(int u) {
    if (g_odo[u] - g_swept[u] >= kAgeSweep) {
        g_swept[u] = g_odo[u];
        const double odo = g_odo[u];
        DropMembers(u, [odo](const Member& mb) { return odo - mb.seen_odo > kForget; });
    }
    if (g_known_n[u] <= kMaxKnown) return;
    std::vector<long long> seen;
    seen.reserve(g_known_n[u]);
    for (const auto& kv : g_members[u])
        for (const Member& mb : kv.second) seen.push_back(mb.seen_frame);
    auto cut = seen.begin() + seen.size() / 4;
    std::nth_element(seen.begin(), cut, seen.end());
    const long long oldest = *cut, recent = g_stats.frames - 1;  // never the ones matched this frame or the last
    DropMembers(u, [oldest, recent](const Member& mb) { return mb.seen_frame <= oldest && mb.seen_frame < recent; });
}

// N17: whether the unit knows kFoundBodyKnown parts away from its wheels; and those drawn at their place at frame f (a chassis part's learnt
// place, a cab part's rest pose with the cab's motion). Counts only: nothing is learnt, seen or voted.
// N20: g_wheel_out / g_wheel_parts of unit u from its members.
void MeasureWheels(int u) {
    const UnitConfig& c = g_cfg[u];
    for (int w = 0; w < kMaxWheels; ++w) g_wheel_out[u][w] = 0.f, g_wheel_parts[u][w] = 0;
    for (const auto& kv : g_members[u]) {
        for (const Member& mb : kv.second) {
            if (IsCab(mb) || std::fabs(mb.local.r[0][0]) < kAxial) continue;
            const float* p = mb.local.t;
            for (int w = 0; w < c.wheels; ++w) {
                const float* wp = c.wheel[w];
                const float out = wp[0] < 0.f ? wp[0] - p[0] : p[0] - wp[0];
                if (std::fabs(p[1] - wp[1]) > kOnAxle || std::fabs(p[2] - wp[2]) > kOnAxle || out < -kAxleIn ||
                    out > kAxleOut)
                    continue;
                ++g_wheel_parts[u][w];
                g_wheel_out[u][w] = (std::max)(g_wheel_out[u][w], out);
            }
        }
    }
}

bool KnowsBody(int u) {
    int n = 0;
    for (const auto& kv : g_members[u])
        for (const Member& mb : kv.second)
            if (!mb.wheel && ++n >= kFoundBodyKnown) return true;
    return false;
}

// The unit's wheels with a draw of a mesh known as one of its wheels' at their place (config + deflection).
int KnownWheels(int u, const std::vector<FrameDraw>& d, const Mat& f) {
    const Mat inv = Inv(f);
    bool hit[kMaxWheels] = {};
    int n = 0;
    for (const FrameDraw& fd : d) {
        const auto found = g_members[u].find(fd.key);
        if (found == g_members[u].end()) continue;
        bool wheel = false;
        for (const Member& mb : found->second) wheel = wheel || mb.wheel;
        if (!wheel) continue;
        float lp[3];
        Apply(inv, fd.m.t, lp);
        for (int w = 0; w < g_cfg[u].wheels; ++w)
            if (!hit[w] && Dist(lp, g_wheel[u][w]) < kWheelTol) hit[w] = true, ++n;
    }
    return n;
}

int BodyAtPlace(int u, const std::vector<FrameDraw>& d, const Mat& f);

// D13: the unit's frame in the view from its known parts: a sure, rigid member's draw (as Classify re-anchors)
// gives a candidate, taken when kAnchorParts of its parts away from the wheels are at their place in it.
bool AnchorKnown(int u, const std::vector<FrameDraw>& d, Mat* out) {
    const bool cab_moves = u == 0 && g_has_cab;
    int tries = 0;
    for (const FrameDraw& fd : d) {
        const auto found = g_members[u].find(fd.key);
        if (found == g_members[u].end()) continue;
        for (const Member& mb : found->second) {
            if (!mb.rigid || !mb.sure || mb.wheel) continue;
            const bool on_cab = cab_moves && IsCab(mb);
            if (cab_moves && !on_cab && !IsChassis(mb) && g_cab_mag > kCabRest) continue;
            const Mat f = Mul(fd.m, Inv(on_cab ? Mul(g_cab, mb.rest) : mb.local));
            if (BodyAtPlace(u, d, f) >= kAnchorParts) {
                *out = f;
                return true;
            }
            if (++tries >= kAnchorTries) return false;
            break;  // one candidate per draw
        }
    }
    return false;
}

int BodyAtPlace(int u, const std::vector<FrameDraw>& d, const Mat& f) {
    const Mat inv = Inv(f);
    const bool cab_moves = u == 0 && g_has_cab;
    int n = 0;
    for (const FrameDraw& fd : d) {
        const auto found = g_members[u].find(fd.key);
        if (found == g_members[u].end()) continue;
        float lp[3];
        Apply(inv, fd.m.t, lp);
        for (const Member& mb : found->second) {
            if (mb.wheel) continue;
            float moved[3];
            if (cab_moves) Apply(g_cab, mb.rest.t, moved);
            if (Dist(mb.local.t, lp) < kSamePose || (cab_moves && Dist(moved, lp) < kSamePose)) {
                ++n;
                break;
            }
        }
    }
    return n;
}

// The view's draws that belong to the unit at frame f: wheels and known
// members count; new wheels join at once, new box draws once the unit has
// moved with them (learn = false: only count, the frame is a held one).
// `other`: the other unit's frame in the view (nullptr: unknown). A draw is
// learnt by one unit only (C10): a draw at the other unit's wheel or at one of
// its members stays the other's, and one that fits both units (where the boxes
// overlap, the trailer in line) is the trailer's [verified: V3a, the only such
// box draws are the trailer's front parts, the trailer's alone at 11 degrees in
// run2 capture 003]. A truck part there goes to the truck once the trailer has
// turned (tools/rigtest RIGTEST_ARTIC) [estimativa: none in the captures]:
// a member sure of a draw takes it from the other unit's member that is not.
// N13: a new box draw at the place of a known member not drawn in this view
// this frame is that part's other LOD: it joins without waiting for the unit
// to move (the unit may stand still). N14: on its second sighting, from a
// member drawn in this view until it appeared, with the member's votes only
// when every member at that place is of one class (kLodSightings).
// Returns the number of draws that are the unit's.
// N21: the units hooked to u, in front (u - 1) and behind (u + 1), and their frames in the view (nullptr: unknown).
struct Neighbours {
    int o[2] = {-1, -1};
    const Mat* f[2] = {nullptr, nullptr};
    Mat inv[2];
};

// What the neighbours make of a draw (Rivalry with each one present).
struct Rivals {
    Rival r[2];
    // Sure that the draw is not a neighbour's: each one absent, or known here and could not have drawn it.
    bool clear(const Neighbours& nb) const {
        for (int i = 0; i < 2; ++i)
            if (nb.o[i] >= 0 && !(nb.f[i] && !r[i].could)) return false;
        return true;
    }
};

Rivals RivalsOf(const Neighbours& nb, const FrameDraw& fd) {
    Rivals rv;
    for (int i = 0; i < 2; ++i)
        if (nb.o[i] >= 0 && nb.f[i]) rv.r[i] = Rivalry(nb.o[i], fd, *nb.f[i], nb.inv[i]);
    return rv;
}

int Learn(int u, int view, const std::vector<FrameDraw>& d, const Mat& f, const Neighbours& nb, bool learn) {
    const UnitConfig& c = g_cfg[u];
    const Mat inv = Inv(f);
    const bool cab_moves = u == 0 && g_has_cab;
    const bool vote = learn && cab_moves && g_cab_mag >= kCabVoteMag;
    const long long now = g_stats.frames;
    // N13: new box draws not promoted, at the place of a known member, with their member-to-be: once every
    // draw of the view has marked the members it matched, the ones at an unmatched member's place are that
    // member's other LOD. A pending draw with no member at its place is not looked up again until the
    // members change (grass under a stopped truck, every frame).
    struct Fresh {
        Member mb;
        long long first;  // the pending draw's first sighting
    };
    std::vector<std::pair<const FrameDraw*, Fresh>> fresh;
    // D12b: the older copies of the members a new copy came from, dropped after the loop (the place index points
    // into the lists): key, place and index count.
    struct Older {
        uint64_t key;
        Mat at;
        uint32_t count;
    };
    std::vector<Older> older;
    // D13: a mesh joining from the map or by driving 1 m: the copies of it there not matched this frame or the last
    // are another buffer's, gone (the ATS lowboy, 30/10: its map's parts joined again in every new buffer, known
    // 241 -> 757 in 28 min while its map had 35).
    auto stale_copies = [&](const Mat& local, uint32_t count) {
        if (!count || !learn) return;
        for (const auto& cp : FindLodPlace(u, local, view, now, count).copies)
            if (cp.second->seen_frame < now - 1) older.push_back({cp.first, cp.second->local, count});
    };
    if (learn) IndexPlaces(u);
    const long long indexed = g_place_index[u].version;
    int n = 0;
    for (const FrameDraw& fd : d) {
        float lp[3];
        Apply(inv, fd.m.t, lp);
        // D13b: the scene's draws away from the unit go first (most of a view's thousands): its wheels are inside
        // its box, so nothing outside the box widened by kWheelTol is a wheel's or a part's (the ATS, 30/10: learn
        // 140-200 us a frame with the VNL in four views).
        bool near = true;
        for (int i = 0; i < 3 && near; ++i) near = lp[i] >= g_box[u].lo[i] - kWheelTol && lp[i] <= g_box[u].hi[i] + kWheelTol;
        if (!near) continue;
        bool at_wheel = false;
        for (int w = 0; w < c.wheels && !at_wheel; ++w) at_wheel = Dist(lp, g_wheel[u][w]) < kWheelTol;
        float rel[3][3];
        RelRot(f, fd.m, rel);
        if (!at_wheel && !(InBox(g_box[u], lp) && PartRotation(rel))) continue;
        if (!at_wheel && u == 0 && g_has_driver && Dist(lp, g_driver) < kDriverRadius) continue;  // D12c
        const Rivals rv = RivalsOf(nb, fd);
        // Sure: at its wheel, or each neighbour is absent or known here and could not have drawn it.
        const bool sure = at_wheel || rv.clear(nb);
        // find, not []: an empty list for every box draw would count as a known mesh in Classify (missed).
        const auto found = g_members[u].find(fd.key);
        bool known = false, known_sure = false;
        for (Member& mb : found == g_members[u].end() ? g_no_members : found->second) {
            const float dc = Dist(mb.local.t, lp);
            float dk = dc;
            if (cab_moves) {
                float moved[3];
                Apply(g_cab, mb.rest.t, moved);
                dk = Dist(moved, lp);
            }
            if ((std::min)(dc, dk) >= kSamePose) {
                // A wheel's part off the wheel's place but within its slack (hanging): the same part,
                // not a new one to learn.
                if (!at_wheel && mb.wheel && NearWheel(u, lp)) {
                    known = true, Seen(mb, u);
                    mb.drawn[view] = now;
                    if (learn && sure) mb.sure = true;
                    known_sure = known_sure || mb.sure;
                }
                continue;
            }
            known = true;
            Seen(mb, u);
            mb.drawn[view] = now;
            if (learn && sure) mb.sure = true;
            known_sure = known_sure || mb.sure;
            // The cab is tilted: does this part stay with the chassis or move with the cab?
            if (vote && !mb.wheel && std::fabs(dc - dk) > kCabVoteDiff) mb.cab = std::clamp(mb.cab + (dk < dc ? 1 : -1), -6, 6);
        }
        // Known to both units: the one sure of it keeps it.
        for (int i = 0; i < 2; ++i)
            if (learn && known && known_sure && rv.r[i].member && !rv.r[i].member->sure)
                DropMember(nb.o[i], fd.key, rv.r[i].member);
        if (known || at_wheel) ++n;
        if (known || !learn) continue;
        if (!at_wheel) {
            // A neighbour's (its wheel, or its member not shown wrong), or fits both: left to the unit on top
            // where one rests on the other's fifth wheel (the semi-trailer over the truck or the dolly), to the
            // unit in front where a drawbar tows the one behind (the trailer's rear over the dolly's drawbar).
            bool theirs = false;
            for (int i = 0; i < 2; ++i) {
                const Rival& r = rv.r[i];
                const int o = nb.o[i];
                const bool yield = o > u ? !g_drawbar[u] : o >= 0 && g_drawbar[o];
                theirs = theirs || r.wheel || (r.member && (r.member->sure || !sure)) || (yield && r.could);
            }
            if (theirs) continue;
        }
        Member mb{Mul(inv, fd.m), NearIdentity(rel, 0.02f), at_wheel};
        mb.sure = sure;
        mb.count = fd.count;
        mb.drawn[view] = now;  // drawn here now: never the parent of another LOD in this view this frame
        mb.rest = cab_moves ? Mul(g_cab_inv, mb.local) : mb.local;
        // Joining here (this unit's wheel, or seen where the other's could not be) takes it from the other.
        if (at_wheel) {
            for (int i = 0; i < 2; ++i)
                if (rv.r[i].member) DropMember(nb.o[i], fd.key, rv.r[i].member);
            AddMember(u, g_members[u][fd.key], mb);
            continue;
        }
        // N47: a part of the unit's map (an earlier session): joins now, with the cab votes it had.
        if (Saved* s = FindSaved(u, fd.count, mb.local)) {
            mb.cab = s->cab;
            mb.sure = mb.sure || s->sure;
            if (!s->used) ++g_map_used[u];
            s->used = true;
            for (int i = 0; i < 2; ++i)
                if (rv.r[i].member) DropMember(nb.o[i], fd.key, rv.r[i].member);
            stale_copies(mb.local, mb.count);
            AddMember(u, g_members[u][fd.key], mb);
            auto pit = g_pending[u].find(fd.key);
            if (pit != g_pending[u].end())
                pit->second.erase(std::remove_if(pit->second.begin(), pit->second.end(),
                                                 [&](const Pending& pd) { return Dist(pd.local.t, mb.local.t) < kSamePose; }),
                                  pit->second.end());
            ++g_stats.restored;
            ++n;
            continue;
        }
        auto& pend = g_pending[u][fd.key];
        pend.erase(std::remove_if(pend.begin(), pend.end(), [&](const Pending& p) { return now - p.last > kPendingGap; }),
                   pend.end());
        bool seen = false, promoted = false;
        Pending* pe = nullptr;
        for (size_t i = 0; i < pend.size() && !seen; ++i) {
            if (Dist(pend[i].local.t, lp) >= kSamePose) continue;
            seen = true;
            pe = &pend[i];
            if (pend[i].last != now) ++pend[i].sightings;  // once a frame, whatever the views
            pend[i].last = now;
            const double* w = pend[i].world;
            const double dx = g_place[u][0] - w[0], dy = g_place[u][1] - w[1], dz = g_place[u][2] - w[2];
            if (dx * dx + dy * dy + dz * dz >= kPromoteMove * kPromoteMove) {
                for (int k = 0; k < 2; ++k)
                    if (rv.r[k].member) DropMember(nb.o[k], fd.key, rv.r[k].member);
                stale_copies(mb.local, mb.count);
                AddMember(u, g_members[u][fd.key], mb);
                pend.erase(pend.begin() + i);
                ++g_stats.promoted;
                ++n;
                promoted = true;
            }
        }
        if (!seen && pend.size() < 16) {
            pend.push_back({mb.local, mb.rigid, {g_place[u][0], g_place[u][1], g_place[u][2]}, now, -1, now});
            pe = &pend.back();
        }
        if (promoted || !pe || pe->sightings < kLodSightings || pe->lone == indexed) continue;
        if (FindLodPlace(u, mb.local, view, pe->first, mb.count).any) fresh.push_back({&fd, {mb, pe->first}});
        else pe->lone = indexed;
    }
    // N13: another LOD of a known part. The other unit is asked again: a member of it the loop handed
    // away may be gone.
    for (auto& fr : fresh) {
        const FrameDraw& fd = *fr.first;
        Member& mb = fr.second.mb;
        const LodPlace place = FindLodPlace(u, mb.local, view, fr.second.first, mb.count);
        Member* parent = place.same ? place.same : place.parent;
        if (!parent) continue;
        const auto found = g_members[u].find(fd.key);
        bool dup = false;  // the same mesh twice at this place in the view: joined with the first
        for (const Member& k : found == g_members[u].end() ? g_no_members : found->second)
            dup = dup || Dist(k.local.t, mb.local.t) < kSamePose;
        if (dup) continue;
        const Rivals rv = RivalsOf(nb, fd);
        mb.sure = mb.sure || parent->sure;
        bool theirs = false;
        for (int i = 0; i < 2; ++i) theirs = theirs || (rv.r[i].member && (rv.r[i].member->sure || !mb.sure));
        if (theirs) continue;
        // A cab part and a chassis part at one place (the truck's origin, unturned, while the cab is at rest):
        // the new mesh could be either, so it learns its class itself [verified: D4c test, the chassis' side
        // took a cab part's class there and followed the cab].
        if (place.same) {  // D12b: the same mesh in another buffer: its votes; the copy before it goes
            mb.cab = parent->cab;
            mb.heirs = parent->heirs;
            // Only copies last matched before the one it came from: not a twin drawn with it (a part may be drawn
            // twice at one place, the same mesh: its copy may come a frame later).
            for (const auto& cp : place.copies)
                if (cp.second != parent && cp.second->seen_frame < parent->seen_frame)
                    older.push_back({cp.first, cp.second->local, mb.count});
            ++g_stats.reloaded;
        } else {
            mb.cab = place.mixed ? 0 : parent->cab;
            if (place.mixed) ++g_stats.unclassed;
            ++parent->heirs;
            ++g_stats.inherited;
        }
        for (int i = 0; i < 2; ++i)
            if (rv.r[i].member) DropMember(nb.o[i], fd.key, rv.r[i].member);
        AddMember(u, g_members[u][fd.key], mb);
        auto& pend = g_pending[u][fd.key];
        pend.erase(std::remove_if(pend.begin(), pend.end(),
                                  [&](const Pending& p) { return Dist(p.local.t, mb.local.t) < kSamePose; }),
                   pend.end());
        ++n;
    }
    for (const Older& o : older) {
        auto it = g_members[u].find(o.key);
        if (it == g_members[u].end()) continue;
        auto& list = it->second;
        for (auto m = list.begin(); m != list.end(); ++m) {
            // Not one matched this frame or the last (the game drew that buffer again).
            if (m->count != o.count || Dist(m->local.t, o.at.t) > 1e-5f || m->seen_frame >= now - 1) continue;
            list.erase(m);
            --g_known_n[u];
            ++g_members_version[u];
            break;
        }
        if (list.empty()) g_members[u].erase(it);
    }
    if (g_pending[u].size() > kMaxPending) g_pending[u].clear();
    return n;
}

// N47: the unit's map for ExportMap (render thread: a copy, no text): its members away from the wheels, then the
// imported parts not found again (aged), kMaxKnown at most.
// D12b: a map part's identity (index count, place to the mm, rotation to 0.01): one part per mesh and place.
uint64_t PartId(uint32_t count, const Mat& l) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](int64_t v) { h = (h ^ static_cast<uint64_t>(v)) * 1099511628211ull; };
    mix(count);
    for (int i = 0; i < 3; ++i) {
        mix(static_cast<int64_t>(std::lround(l.t[i] * 1000.f)));
        for (int j = 0; j < 3; ++j) mix(static_cast<int64_t>(std::lround(l.r[i][j] * 100.f)));
    }
    return h;
}

void WriteMap(int u) {
    std::vector<MapPart> parts;
    parts.reserve(static_cast<size_t>(g_known_n[u]) + g_saved[u].size());
    std::unordered_set<uint64_t> ids;  // D12b: the same mesh in several buffers is one part
    auto add = [&](uint32_t count, int cab, bool sure, double km, const Mat& l, const Mat& r) {
        if (parts.size() >= static_cast<size_t>(kMaxKnown) || !ids.insert(PartId(count, l)).second) return;
        MapPart p;
        p.count = count, p.cab = cab, p.sure = sure, p.km = km;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) p.local[i * 4 + j] = l.r[i][j], p.rest[i * 4 + j] = r.r[i][j];
            p.local[i * 4 + 3] = l.t[i], p.rest[i * 4 + 3] = r.t[i];
        }
        parts.push_back(p);
    };
    for (const auto& kv : g_members[u])
        for (const Member& mb : kv.second)
            if (!mb.wheel && mb.count) add(mb.count, mb.cab, mb.sure, 0.0, mb.local, mb.rest);
    const double driven = g_odo[u] - g_map_odo[u];
    for (const Saved& s : g_saved[u])
        if (!s.used && s.km + driven <= kMapForget) add(s.count, s.cab, s.sure, s.km + driven, s.local, s.rest);
    if (u == 0) g_out_inside = g_inside_known;
    if (u == 0)  // N55: the cab's inside parts (and the shell's, so they stay out)
        for (const Inside& e : g_inside) {
            MapPart p;
            p.inside = true, p.count = e.count, p.main = e.main, p.shell = e.shell;
            p.local[3] = e.p[0], p.local[7] = e.p[1], p.local[11] = e.p[2];
            parts.push_back(p);
        }
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_out_key[u] = g_map_key[u];
    g_out_parts[u] = std::move(parts);
    ++g_out_serial[u];
    g_out_version[u] = g_members_version[u];
    g_out_frame[u] = g_stats.frames;
    g_out_odo[u] = g_odo[u];
}

// N47: the parsed map into the tracker (BeginFrame), indexed by index count.
bool CountsInside(const Inside& e) { return e.main >= kInsideVotes && e.shell == 0; }

void SetInside(std::vector<Inside>&& parts) {
    g_inside = std::move(parts);
    g_inside_by_count.clear();
    g_inside_known = 0;
    for (int i = 0; i < static_cast<int>(g_inside.size()); ++i) {
        g_inside_by_count[g_inside[i].count].push_back(i);
        g_inside_known += CountsInside(g_inside[i]);
    }
}

Inside* FindInside(uint32_t count, const float p[3]) {
    const auto it = g_inside_by_count.find(count);
    if (it == g_inside_by_count.end()) return nullptr;
    for (const int i : it->second) {
        Inside& e = g_inside[i];
        if (std::fabs(e.p[0] - p[0]) < kInsideTol && std::fabs(e.p[1] - p[1]) < kInsideTol &&
            std::fabs(e.p[2] - p[2]) < kInsideTol)
            return &e;
    }
    return nullptr;
}

void TakeMap(int u, std::vector<Saved>&& saved, const std::string& key) {
    g_saved[u] = std::move(saved);
    g_saved_by_count[u].clear();
    for (int i = 0; i < static_cast<int>(g_saved[u].size()); ++i) g_saved_by_count[u][g_saved[u][i].count].push_back(i);
    g_map_key[u] = key;
    g_map_odo[u] = g_odo[u];
    g_map_used[u] = 0;
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_out_key[u].clear();  // nothing to write until the next WriteMap
    g_out_parts[u].clear();
    g_out_version[u] = -1;
}

bool SameConfig(const UnitConfig& a, const UnitConfig& b) {
    if (a.wheels != b.wheels || a.has_hook != b.has_hook) return false;
    for (int w = 0; w < a.wheels; ++w)
        for (int k = 0; k < 3; ++k)
            if (a.wheel[w][k] != b.wheel[w][k] || a.radius[w] != b.radius[w]) return false;
    for (int k = 0; k < 3 && a.has_hook; ++k)
        if (a.hook[k] != b.hook[k]) return false;
    return true;
}

}  // namespace

void SetUnit(int unit, const UnitConfig& c) {
    if (unit < 0 || unit >= kUnits) return;
    std::lock_guard<std::mutex> lock(g_in_mutex);
    if (SameConfig(g_in_cfg[unit], c)) return;  // the SDK sends the config again (coupling, job start/end)
    g_in_cfg[unit] = c;
    g_in_changed[unit] = true;
}

void SetPlacement(int unit, const double placement[6], bool valid) {
    if (unit < 0 || unit >= kUnits) return;
    std::lock_guard<std::mutex> lock(g_in_mutex);
    for (int i = 0; i < 6; ++i) g_in_place[unit][i] = placement[i];
    g_in_placed[unit] = valid;
}

void BeginFrame() {
    std::lock_guard<std::mutex> lock(g_in_mutex);
    for (int u = 0; u < kUnits; ++u) {
        if (!g_in_changed[u]) continue;
        g_in_changed[u] = false;
        g_cfg[u] = g_in_cfg[u];
        g_box[u] = MakeBox(g_cfg[u]);
        for (View& v : g_views) v.unit[u] = Track();  // new layout of this unit: forget what it learned
        for (auto& t : g_tracked_once) t[u] = false;
        g_members[u].clear();
        g_known_n[u] = 0;
        for (int w = 0; w < kMaxWheels; ++w) g_wheel_out[u][w] = 0.f, g_wheel_parts[u][w] = 0;
        ++g_members_version[u];
        g_pending[u].clear();
        TakeMap(u, {}, "");  // N47: another vehicle: its map comes with ImportMap
        if (u == 0) SetInside({});  // N55
    }
    if (g_in_inside_set) {  // N55: the truck's inside parts from its map
        g_in_inside_set = false;
        SetInside(std::move(g_in_inside));
        g_in_inside.clear();
    }
    g_inside_look = g_stats.frames % (g_inside_known >= kInsideKnown ? kInsideEveryKnown : kInsideEvery) == 0;
    for (int u = 0; u < kUnits; ++u) {  // N47
        if (!g_in_map[u]) continue;
        g_in_map[u] = false;
        TakeMap(u, std::move(g_in_saved[u]), g_in_map_key[u]);
        g_in_saved[u].clear();
    }
    // The cab's motion: p_now = pivot + d + R (p_rest - pivot), R = the offset's
    // rotation as it is (heading, pitch, roll) [verified: the 13 captures with a cab line where it moves,
    // the cab's 39 draws at the truck's origin within 0.1-0.7 mm (3.9 mm at most), 1-9 cm off with R
    // transposed]. N15: it was transposed until the D4d (the "29 cab draws" of capture 008 were the
    // chassis'): the cab's tilt was the mirror image of the real one, cab parts voted chassis (13 cab
    // members of ~400 in the D4d test) and the cab's dust went the wrong way.
    g_has_cab = g_in_has_cab;
    g_has_driver = g_in_has_driver;  // D12c
    for (int i = 0; i < 3; ++i) g_driver[i] = g_in_driver[i];
    {
        const double kTurn = 6.283185307179586;
        const double a = g_in_cab_off[3] * kTurn, b = g_in_cab_off[4] * kTurn, cr = g_in_cab_off[5] * kTurn;
        const double ry[3][3] = {{std::cos(a), 0, std::sin(a)}, {0, 1, 0}, {-std::sin(a), 0, std::cos(a)}};
        const double rx[3][3] = {{1, 0, 0}, {0, std::cos(b), -std::sin(b)}, {0, std::sin(b), std::cos(b)}};
        const double rz[3][3] = {{std::cos(cr), -std::sin(cr), 0}, {std::sin(cr), std::cos(cr), 0}, {0, 0, 1}};
        double t[3][3], e[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) t[i][j] = ry[i][0] * rx[0][j] + ry[i][1] * rx[1][j] + ry[i][2] * rx[2][j];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) e[i][j] = t[i][0] * rz[0][j] + t[i][1] * rz[1][j] + t[i][2] * rz[2][j];
        float dev = 0.f;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                g_cab.r[i][j] = g_has_cab ? static_cast<float>(e[i][j]) : (i == j ? 1.f : 0.f);
                dev = (std::max)(dev, std::fabs(g_cab.r[i][j] - (i == j ? 1.f : 0.f)));
            }
        }
        float rp[3];
        for (int i = 0; i < 3; ++i)
            rp[i] = g_cab.r[i][0] * g_in_pivot[0] + g_cab.r[i][1] * g_in_pivot[1] + g_cab.r[i][2] * g_in_pivot[2];
        float dmag = 0.f;
        for (int i = 0; i < 3; ++i) {
            const float d = g_has_cab ? g_in_cab_off[i] : 0.f;
            g_cab.t[i] = g_in_pivot[i] + d - rp[i];
            dmag += d * d;
        }
        g_cab_mag = g_has_cab ? dev * 2.f + std::sqrt(dmag) : 0.f;
        g_cab_inv = Inv(g_cab);
    }
    for (int u = 0; u < kUnits; ++u) {
        for (int i = 0; i < 6; ++i) g_place[u][i] = g_in_place[u][i];
        g_placed[u] = g_in_placed[u];
        if (g_placed[u]) {  // the odometer for ageing
            const double dx = g_place[u][0] - g_odo_place[u][0], dy = g_place[u][1] - g_odo_place[u][1],
                         dz = g_place[u][2] - g_odo_place[u][2];
            const double step = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (g_odo_valid[u] && step < kOdoJump) g_odo[u] += step;
            for (int i = 0; i < 3; ++i) g_odo_place[u][i] = g_place[u][i];
        }
        g_odo_valid[u] = g_placed[u];
        for (int w = 0; w < kMaxWheels; ++w) {
            g_wheel[u][w][0] = g_cfg[u].wheel[w][0];
            g_wheel[u][w][1] = g_cfg[u].wheel[w][1] + g_in_defl[u][w];
            g_wheel[u][w][2] = g_cfg[u].wheel[w][2];
        }
        // N24: the box ends at the next unit's hook, a point fixed in this unit's frame (the coupling).
        g_box[u] = MakeBox(g_cfg[u]);
        g_tail[u] = 0.f;
        g_drawbar[u] = false;
        // N21: the next unit tracked itself (its config and placement), else the one SetNext gave.
        const int nu = u + 1;
        const bool tracked_next = nu < kUnits && g_in_cfg[nu].wheels && g_in_cfg[nu].has_hook && g_in_placed[nu];
        if ((tracked_next || g_in_next[u]) && g_placed[u] && g_cfg[u].wheels) {
            double r[3][3], t[3], rn[3][3], tn[3], w[3];
            WorldFrame(g_place[u], r, t);
            WorldFrame(tracked_next ? g_in_place[nu] : g_in_next_place[u], rn, tn);
            const float* k = tracked_next ? g_in_cfg[nu].hook : g_in_next_hook[u];
            for (int i = 0; i < 3; ++i) w[i] = tn[i] + rn[i][0] * k[0] + rn[i][1] * k[1] + rn[i][2] * k[2] - t[i];
            const double z = r[0][2] * w[0] + r[1][2] * w[1] + r[2][2] * w[2];
            float last = -1e9f;
            for (int i = 0; i < g_cfg[u].wheels; ++i) last = (std::max)(last, g_cfg[u].wheel[i][2]);
            g_drawbar[u] = tracked_next && std::isfinite(z) && z > last + kDrawbarBehind;
            // Only behind the last axle and within the box: anything else is not a coupling.
            if (std::isfinite(z) && z > last && z + kTailPad < g_box[u].hi[2]) {
                g_tail[u] = static_cast<float>(z) + kTailPad;
                g_box[u].hi[2] = g_tail[u];
            }
        }
    }
}

void SetCabin(bool has, const float pivot[3]) {
    std::lock_guard<std::mutex> lock(g_in_mutex);
    g_in_has_cab = has;
    for (int i = 0; i < 3; ++i) g_in_pivot[i] = has ? pivot[i] : 0.f;
}

void SetDriver(bool has, const float eye[3]) {
    std::lock_guard<std::mutex> lock(g_in_mutex);
    g_in_has_driver = has;
    g_in_driver[0] = has ? eye[0] : 0.f;
    g_in_driver[1] = has ? eye[1] - kDriverDown : 0.f;
    g_in_driver[2] = has ? eye[2] + kDriverBack : 0.f;  // +z is backwards in the truck's frame
}

void SetCabinOffset(const float offset[6]) {
    std::lock_guard<std::mutex> lock(g_in_mutex);
    for (int i = 0; i < 6; ++i) g_in_cab_off[i] = std::isfinite(offset[i]) ? offset[i] : 0.f;
}

void SetNext(int unit, const float hook[3], const double placement[6], bool valid) {
    if (unit < 0 || unit >= kUnits) return;
    std::lock_guard<std::mutex> lock(g_in_mutex);
    g_in_next[unit] = valid;
    for (int i = 0; i < 3; ++i) g_in_next_hook[unit][i] = valid ? hook[i] : 0.f;
    for (int i = 0; i < 6; ++i) g_in_next_place[unit][i] = valid ? placement[i] : 0.0;
}

void SetDeflection(int unit, const float* deflection, int wheels) {
    if (unit < 0 || unit >= kUnits) return;
    std::lock_guard<std::mutex> lock(g_in_mutex);
    for (int w = 0; w < kMaxWheels; ++w) {
        const float d = w < wheels ? deflection[w] : 0.f;
        g_in_defl[unit][w] = std::isfinite(d) && std::fabs(d) < 1.f ? d : 0.f;
    }
}

// N55: a vote for an inside part at lp (the truck's frame): the main view's (for) or a mirror's (against: the shell).
void VoteInside(uint32_t count, const float lp[3], bool main_view) {
    for (int i = 0; i < 3; ++i)
        if (lp[i] < g_box[0].lo[i] - 1.f || lp[i] > g_box[0].hi[i] + 1.f) return;  // not the truck's
    float p[3];
    if (g_has_cab) Apply(g_cab_inv, lp, p);  // into the cab's frame (its motion undone)
    else memcpy(p, lp, sizeof(p));
    ++g_stats.inside_seen;
    Inside* e = FindInside(count, p);
    if (!e) {
        if (static_cast<int>(g_inside.size()) >= kMaxInside) return;
        Inside n;
        n.count = count;
        memcpy(n.p, p, sizeof(n.p));
        g_inside_by_count[n.count].push_back(static_cast<int>(g_inside.size()));
        g_inside.push_back(n);
        e = &g_inside.back();
    }
    const bool was = CountsInside(*e);
    if (main_view) e->main = (std::min)(e->main + 1, 1000);
    else e->shell = (std::min)(e->shell + 1, 1000);
    g_inside_known += int(CountsInside(*e)) - int(was);
}

// D14c: the look's main-view draws placed. The interior camera's own view shows nothing of the truck but the cabin
// model (the hood seen from inside is part of it) [verified: D11l captures, no ref-1 truck draw in the main view; ATS
// 31/10, D14b: the truck never tracked there, 0 inside parts after 10236 draws looked at, every vote a mirror's]. Its
// draws of meshes the truck already knows from outside (the same buffers: the dashboard and the seats seen through
// the glass) each give the truck's frame; kLookAgree of them agreeing place the whole look.
void ResolveLook() {
    if (g_look.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Mat> cand;
    for (const LookDraw& d : g_look) {
        const auto it = g_members[0].find(d.k);
        if (it == g_members[0].end()) continue;
        for (const Member& mb : it->second)
            if (!mb.wheel && cand.size() < 96)
                cand.push_back(Mul(d.m, Inv(g_has_cab && IsCab(mb) ? Mul(g_cab, mb.rest) : mb.local)));
    }
    int best = -1, best_n = 0;
    for (size_t i = 0; i < cand.size(); ++i) {
        int n = 0;
        for (size_t j = 0; j < cand.size(); ++j) n += MatClose(cand[i], cand[j], kInsideTol, 0.02f) ? 1 : 0;
        if (n > best_n) best_n = n, best = static_cast<int>(i);
    }
    ++g_stats.inside_looks;
    if (best >= 0 && best_n >= kLookAgree) {
        ++g_stats.inside_placed;
        const Mat inv = Inv(cand[best]);
        for (const LookDraw& d : g_look) {
            float lp[3];
            Apply(inv, d.m.t, lp);
            VoteInside(d.count, lp, true);
        }
    }
    g_look.clear();
    AddSec(&g_stats.sec_inside, kPartInside, Seconds(t0));
}

// N55: a ref-2 draw of the interior camera, looked at every kInsideEvery frames: its place in the truck's cab, a vote
// for an inside part (the main view, placed at EndFrame: ResolveLook) or against it (a mirror: the cab's shell).
void SeeInside(int view, const DrawKey& key, const float m[12], bool main_view) {
    if (!g_inside_look || !g_cfg[0].wheels || !key.count) return;
    if (main_view) {
        if (g_look.size() < 512) g_look.push_back({KeyHash(key), key.count, FromRows(m)});
        return;
    }
    const Track& tr = g_views[view].unit[0];
    if (!tr.cur && !tr.prev) return;
    // D14b: the view's frame inverted once a look (per view, and again when the frame is found in the middle of it).
    static int inv_view = -1;
    static long long inv_frame = -1;
    static bool inv_cur = false;
    static Mat inv;
    if (view != inv_view || g_stats.frames != inv_frame || tr.cur != inv_cur) {
        inv = Inv(tr.cur ? tr.f_cur : tr.f_pred);
        inv_view = view, inv_frame = g_stats.frames, inv_cur = tr.cur;
    }
    const Mat mm = FromRows(m);
    float lp[3];
    Apply(inv, mm.t, lp);
    VoteInside(key.count, lp, false);
}

int Classify(int view, int group, const DrawKey& key, const float m[12], bool interior, bool* cab, bool main_view,
             uint64_t* part) {
    if (cab) *cab = false;
    if (part) *part = 0;
    if (view < 0 || view >= kMaxViews) return -1;
    if (interior) {  // the cabin seen from the interior camera: never painted, never learned
        ++g_stats.interior;
        SeeInside(view, key, m, main_view);  // N55
        return -1;
    }
    View& v = g_views[view];
    v.group = group;
    const uint64_t k = KeyHash(key);
    const Mat mm = FromRows(m);
    v.draws.push_back({k, mm, key.count});
    ++g_stats.draws;
    int misses = 0;  // units that know the mesh but not at this pose: missed, unless the other unit claims it
    for (int u = 0; u < kUnits; ++u) {
        if (!g_cfg[u].wheels) continue;
        Track& tr = v.unit[u];
        auto it = g_members[u].find(k);
        if (it == g_members[u].end()) continue;
        ++tr.seen;
        const bool cab_moves = u == 0 && g_has_cab;
        if (!tr.cur) {
            for (const Member& mb : it->second) {
                // N18: only a part sure to be this unit's anchors its frame. A truck part where the boxes overlap
                // (behind the cab, over the fifth wheel) goes to the trailer while the rig is in line (C10); as
                // the trailer's anchor it made the trailer's frame follow the truck in a turn, and the trailer's
                // wheel dirt with it [verified: RIGTEST_ARTIC=first, up to 1.48 m off; D4g test, the trailer's
                // wheel dirt followed the tractor].
                if (!mb.rigid || !mb.sure) continue;
                // The chassis frame: from a chassis part, from a cab part minus the cab's
                // motion, or from a part not yet known either way while the cab is at rest.
                const bool on_cab = cab_moves && IsCab(mb);
                if (cab_moves && !on_cab && !IsChassis(mb) && g_cab_mag > kCabRest) continue;
                const Mat f = Mul(mm, Inv(on_cab ? Mul(g_cab, mb.rest) : mb.local));
                // This view's previous frame first, then the other views of its group.
                for (int o = 0; o < kMaxViews && !tr.cur; ++o) {
                    const int vi = o == 0 ? view : (o <= view ? o - 1 : o);
                    const Track& p = g_views[vi].unit[u];
                    if (vi != view && g_views[vi].group != group) continue;
                    if (p.prev && MatClose(f, p.f_pred)) {
                        tr.cur = true;
                        tr.f_cur = f;
                    }
                }
                if (tr.cur) break;
            }
        }
        // Not re-anchored (no rigid mesh drawn yet, or none in sight): the
        // previous frame's pose moved on by the last motion, held while known
        // meshes match it.
        if (!tr.cur && !tr.prev) continue;
        float lp[3];
        Apply(Inv(tr.cur ? tr.f_cur : tr.f_pred), mm.t, lp);
        bool match = false, on_cab = false;
        for (Member& mb : it->second) {
            match = AtPose(u, mb, lp);
            if (match) {
                Seen(mb, u);
                on_cab = cab_moves && IsCab(mb);
                if (part) {  // N57b
                    uint64_t h = 1469598103934665603ull;
                    for (const uint64_t x : {uint64_t(u), uint64_t(key.count), uint64_t(int64_t(std::lround(mb.local.t[0] * 20.f))),
                                             uint64_t(int64_t(std::lround(mb.local.t[1] * 20.f))),
                                             uint64_t(int64_t(std::lround(mb.local.t[2] * 20.f)))})
                        h = (h ^ x) * 1099511628211ull;
                    *part = h ? h : 1;
                }
                break;
            }
        }
        // D14d: an inside part (N55) keeps its dust here. The outside camera's draws at an inside part's place were
        // the hood and the cab's shell around the windows, which the interior camera draws too and no mirror shows; the
        // cabin's own meshes are drawn elsewhere from outside [verified: ATS 31/10, D14c, the VNL 2025: of 122 learned
        // parts, the 16 known outside at the same place were the hood's 7 and the cab pivot's 9; none of the other 106
        // at its place in the outside map; the hood clean after a dirt road, the user's print].
        if (match) {
            if (!tr.cur) ++tr.held;
            ++g_stats.claimed;
            if (on_cab) {
                ++g_stats.cab_claims;
                if (cab) *cab = true;
            }
            return u;
        }
        ++misses;
    }
    g_stats.missed += misses;
    return -1;
}

void EndFrame(std::string* debug) {
    ResolveLook();  // D14c
    ++g_stats.frames;
    int tracked[kUnits] = {}, members[kUnits] = {};
    for (int vi = 0; vi < kMaxViews; ++vi) {
        View& v = g_views[vi];
        if (v.draws.empty()) {
            for (Track& t : v.unit) {
                if (t.prev && ++t.miss > kLostAfter) t = Track();  // view not drawn (mirrors in the exterior camera)
                t.cur = false;
                t.held = 0;
                t.seen = 0;
            }
            continue;
        }
        static Grid grid;  // built for the first search only (D11n: its storage kept between views and frames)
        bool grid_built = false;
        bool have[kUnits] = {};
        bool searched[kUnits] = {};
        bool predicted[kUnits] = {};
        bool held[kUnits] = {};
        Mat f[kUnits];
        for (int u = 0; u < kUnits; ++u) {
            Track& tr = v.unit[u];
            if (!g_cfg[u].wheels) continue;
            if (tr.cur) {  // re-anchored on its own parts (wheels may be out of sight: mirrors)
                have[u] = true;
                f[u] = tr.f_cur;
                tr.miss = 0;
            } else if (tr.wait > 0 && tr.seen < kMinWheels) {  // backing off: the unit is not drawn here
                --tr.wait;
                ++g_stats.skipped;
            } else if (g_tracked_once[vi][u] && std::none_of(have, have + u, [](bool h) { return h; }) &&
                       KnowsBody(u) && [&] {
                           // (a unit in front found here: the chain and the telemetry say where the rest are; two
                           // trailers of one model share their meshes: a road train's hoppers, RIGTEST road)
                           const auto t0 = std::chrono::steady_clock::now();
                           const bool ok = AnchorKnown(u, v.draws, &f[u]);
                           AddSec(&g_stats.sec_anchor, kPartAnchor, Seconds(t0));
                           return ok;
                       }()) {  // D13: found again by its known parts
                have[u] = true;
                tr.miss = 0;
                ++g_stats.anchored;
            } else {
                const auto t0 = std::chrono::steady_clock::now();  // D13
                ++g_stats.searches;
                searched[u] = true;
                if (!grid_built) grid.Build(v.draws), grid_built = true;
                const Mat* taken[kUnits];
                int ntaken = 0;
                for (int o = 0; o < u; ++o)
                    if (have[o]) taken[ntaken++] = &f[o];
                if (Search(grid, v.draws, u, tr, &f[u], taken, ntaken)) {
                    const bool expected = tr.prev && MatClose(f[u], tr.f_pred);
                    if (!expected && KnowsBody(u) && BodyAtPlace(u, v.draws, f[u]) < kFoundBody &&
                        KnownWheels(u, v.draws, f[u]) < kMinWheels) {
                        ++g_stats.rejected;  // N17: wheel hits without anything known of the unit there
                    } else {
                        have[u] = true;
                        tr.miss = 0;
                        ++g_stats.found;
                    }
                }
                AddSec(&g_stats.sec_search, kPartSearch, Seconds(t0));
            }
        }
        // A unit not found from a unit that was: relative pose from the telemetry. N21: along the chain (a
        // dolly from the trailer in front, the next trailer from the dolly).
        for (bool more = true; more;) {
            more = false;
            for (int u = 0; u < kUnits; ++u) {
                if (have[u] || !g_cfg[u].wheels || !g_placed[u]) continue;
                for (const int o : {u - 1, u + 1}) {
                    if (o < 0 || o >= kUnits || !have[o] || !g_placed[o] || !g_cfg[o].wheels) continue;
                    f[u] = Mul(f[o], Relative(o, u));
                    have[u] = predicted[u] = more = true;
                    break;
                }
            }
        }
        // Still nothing, but known meshes matched the previous frame's pose (a
        // mirror showing only a wheel's edge): keep that pose.
        for (int u = 0; u < kUnits; ++u) {
            Track& tr = v.unit[u];
            if (have[u] || !g_cfg[u].wheels || !tr.prev) continue;
            if (tr.held >= kMinHeld) {
                have[u] = held[u] = true;
                f[u] = tr.f_pred;
                tr.miss = 0;
                ++g_stats.held;
            } else {
                ++g_stats.lost;
            }
        }
        std::string line;
        for (int u = 0; u < kUnits; ++u) {
            Track& tr = v.unit[u];
            int n = 0;
            if (have[u]) {
                // The neighbours' frames here: tracked, or from the telemetry (both units placed).
                Neighbours nb;
                Mat of[2];
                for (int i = 0; i < 2; ++i) {
                    const int o = i == 0 ? u - 1 : u + 1;
                    if (o < 0 || o >= kUnits || !g_cfg[o].wheels) continue;
                    nb.o[i] = o;
                    if (have[o]) {
                        nb.f[i] = &f[o];
                    } else if (g_placed[u] && g_placed[o]) {
                        of[i] = Mul(f[u], Relative(u, o));
                        nb.f[i] = &of[i];
                    }
                    if (nb.f[i]) nb.inv[i] = Inv(*nb.f[i]);
                }
                // A predicted frame must be confirmed by the unit's own draws before it teaches anything.
                const auto t0 = std::chrono::steady_clock::now();  // D13
                n = Learn(u, vi, v.draws, f[u], nb, !held[u] && !predicted[u]);
                if (predicted[u] && n < kMinPredictedMembers) {
                    have[u] = false;
                } else if (predicted[u]) {
                    ++g_stats.predicted;
                    n = Learn(u, vi, v.draws, f[u], nb, true);
                }
                AddSec(&g_stats.sec_learn, kPartLearn, Seconds(t0));
            }
            if (have[u]) {
                // The next frame's guess: this pose moved on by the motion since the last one,
                // unless that motion is a jump (camera cut, new view).
                tr.f_pred = f[u];
                if (tr.prev && MatClose(f[u], tr.f_prev, kPredictMove, kPredictTurn))
                    tr.f_pred = Mul(Mul(f[u], Inv(tr.f_prev)), f[u]);
                tr.prev = true;
                tr.f_prev = f[u];
                if (!predicted[u] && !held[u]) g_tracked_once[vi][u] = true;  // D13
                ++tracked[u];
                members[u] += n;
            } else {
                tr.prev = false;
            }
            if (have[u]) {
                tr.fails = tr.wait = 0;
            } else if (searched[u] && ++tr.fails >= kSearchBackoff) {
                tr.wait = (2 << (std::min)(tr.fails - kSearchBackoff, 3)) - 1;
            }
            tr.cur = false;
            tr.held = 0;
            tr.seen = 0;
            if (debug && (u < 2 || g_cfg[u].wheels)) {  // N21: the units past trailer.0 when coupled
                char name[16], b[160];
                snprintf(name, sizeof(name), u == 0 ? "truck" : u == 1 ? "trailer" : "trailer.%d", u - 1);
                snprintf(b, sizeof(b), " | %s %s %d members at (%.2f %.2f %.2f)", name,
                         !have[u] ? "-" : predicted[u] ? "predicted" : held[u] ? "held" : "found", n, have[u] ? f[u].t[0] : 0.f,
                         have[u] ? f[u].t[1] : 0.f, have[u] ? f[u].t[2] : 0.f);
                line += b;
            }
        }
        if (debug) *debug += "view " + std::to_string(vi) + ": " + std::to_string(v.draws.size()) + " draws" + line + "\n";
        v.draws.clear();
    }
    auto t0 = std::chrono::steady_clock::now();  // D13
    for (int u = 0; u < kUnits; ++u) AgeMembers(u);
    AddSec(&g_stats.sec_age, kPartAge, Seconds(t0));
    t0 = std::chrono::steady_clock::now();
    // N47: the maps, when the members changed or the unit drove 1 km since the last one.
    for (int u = 0; u < kUnits; ++u)
        if (!g_map_key[u].empty() && g_stats.frames - g_out_frame[u] >= kMapEvery &&
            (g_out_version[u] != g_members_version[u] || g_odo[u] - g_out_odo[u] >= 1000.0 ||
             (u == 0 && g_out_inside != g_inside_known)))  // N55: the cab's inside learned more
            WriteMap(u);
    AddSec(&g_stats.sec_map, kPartMap, Seconds(t0));
    for (int p = 0; p < kParts; ++p) g_part_worst[p] = (std::max)(g_part_worst[p], g_part_frame[p]), g_part_frame[p] = 0;
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    for (int u = 0; u < kUnits; ++u) {
        g_stats.tracked_views[u] = tracked[u];
        g_stats.members[u] = members[u];
        g_stats.known[u] = g_known_n[u];
        g_stats.tail[u] = g_tail[u];  // N24
        g_stats.map[u] = static_cast<int>(g_saved[u].size());  // N47
        g_stats.map_used[u] = g_map_used[u];
        if (u == 0) g_stats.inside_parts = g_inside_known, g_stats.inside_all = static_cast<int>(g_inside.size());  // N55
        if (g_stats.frames % 60 != 1) continue;  // counting the maps is not free
        int pending = 0, cab_n = 0, chassis_n = 0, body_n = 0;
        for (const auto& kv : g_members[u])
            for (const Member& mb : kv.second) {
                cab_n += IsCab(mb) ? 1 : 0;
                chassis_n += IsChassis(mb) ? 1 : 0;
                body_n += mb.wheel ? 0 : 1;
            }
        g_stats.body[u] = body_n;
        MeasureWheels(u);
        for (int w = 0; w < kMaxWheels; ++w) {
            g_stats.wheel_out[u][w] = g_wheel_out[u][w];
            g_stats.wheel_parts[u][w] = g_wheel_parts[u][w];
        }
        if (u == 0) g_stats.cab_members = cab_n, g_stats.chassis_members = chassis_n;
        for (const auto& kv : g_pending[u]) pending += static_cast<int>(kv.second.size());
        g_stats.pending[u] = pending;
    }
}

bool GetFrame(int view, int unit, float m[12], bool cab) {
    if (view < 0 || view >= kMaxViews || unit < 0 || unit >= kUnits || !g_cfg[unit].wheels) return false;
    const Track& tr = g_views[view].unit[unit];
    if (!tr.cur && !tr.prev) return false;
    const Mat f = cab && unit == 0 && g_has_cab ? Mul(tr.cur ? tr.f_cur : tr.f_pred, g_cab) : (tr.cur ? tr.f_cur : tr.f_pred);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = f.r[i][j];
        m[i * 4 + 3] = f.t[i];
    }
    return true;
}

bool GetBox(int unit, float lo[3], float hi[3]) {
    if (unit < 0 || unit >= kUnits || !g_cfg[unit].wheels) return false;
    for (int i = 0; i < 3; ++i) lo[i] = g_box[unit].lo[i], hi[i] = g_box[unit].hi[i];
    return true;
}

int GetWheels(int unit, float wheels[][4]) {
    if (unit < 0 || unit >= kUnits) return 0;
    const UnitConfig& c = g_cfg[unit];
    for (int w = 0; w < c.wheels; ++w) {
        // N13: where the wheel is this frame (config + deflection), as its draws: braking, the chassis dips
        // over the front wheels and their dirt must not dip with it [verified: D4b test]. The radius stays
        // the config height, the centre's at rest.
        for (int k = 0; k < 3; ++k) wheels[w][k] = g_wheel[unit][w][k];
        wheels[w][3] = c.wheel[w][1];
    }
    return c.wheels;
}

void GetWheelReach(int unit, float out[kMaxWheels]) {
    for (int w = 0; w < kMaxWheels; ++w) out[w] = unit >= 0 && unit < kUnits ? g_wheel_out[unit][w] : 0.f;
}

void GetWheelRadius(int unit, float out[kMaxWheels]) {
    for (int w = 0; w < kMaxWheels; ++w) out[w] = unit >= 0 && unit < kUnits ? g_cfg[unit].radius[w] : 0.f;
}

void MeasureWheelReach() {
    for (int u = 0; u < kUnits; ++u) MeasureWheels(u);
}

int SharedMembers() {
    if (!g_cfg[0].wheels || !g_cfg[1].wheels || !g_placed[0] || !g_placed[1]) return 0;
    const Mat rel = Relative(0, 1);
    int n = 0;
    for (const auto& kv : g_members[1]) {
        const auto it = g_members[0].find(kv.first);
        if (it == g_members[0].end()) continue;
        for (const Member& m1 : kv.second) {
            float p[3];
            Apply(rel, m1.local.t, p);
            for (const Member& m0 : it->second)
                if (AtPose(0, m0, p)) {
                    ++n;
                    break;
                }
        }
    }
    return n;
}

int ImportMap(int unit, const std::string& key, const std::string& text) {
    if (unit < 0 || unit >= kUnits) return 0;
    std::vector<Saved> saved;
    std::vector<Inside> inside;  // N55
    std::unordered_set<uint64_t> ids;
    size_t pos = 0;
    int line_no = 0;
    bool ok = false;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ++line_no;
        if (line_no == 1) {
            if (line != kMapHeader) break;  // another format: left alone
            continue;
        }
        if (line_no == 2) {
            ok = line == "key " + key;
            if (!ok) break;
            continue;
        }
        if (line.compare(0, 7, "inside ") == 0) {  // N55
            Inside e;
            unsigned c = 0;
            if (unit == 0 && sscanf(line.c_str() + 7, "%u %f %f %f %d %d", &c, &e.p[0], &e.p[1], &e.p[2], &e.main, &e.shell) == 6 &&
                c && std::isfinite(e.p[0]) && std::isfinite(e.p[1]) && std::isfinite(e.p[2]) && std::fabs(e.p[0]) < 50.f &&
                std::fabs(e.p[1]) < 50.f && std::fabs(e.p[2]) < 50.f && inside.size() < static_cast<size_t>(kMaxInside)) {
                e.count = c;
                e.main = std::clamp(e.main, 0, 1000), e.shell = std::clamp(e.shell, 0, 1000);
                inside.push_back(e);
            }
            continue;
        }
        Saved s;
        unsigned count = 0;
        int sure = 0, used = 0;
        double km = 0;
        float v[24];
        const int got = sscanf(line.c_str(),
                               "%u %d %d %lf %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f%n",
                               &count, &s.cab, &sure, &km, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8],
                               &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16], &v[17], &v[18], &v[19],
                               &v[20], &v[21], &v[22], &v[23], &used);
        if (got != 28 || !count || !std::isfinite(km) || km < 0) continue;
        bool finite = true;
        for (float f : v) finite = finite && std::isfinite(f) && std::fabs(f) < 100.f;
        if (!finite) continue;
        s.count = count;
        s.cab = std::clamp(s.cab, -6, 6);
        s.sure = sure != 0;
        s.km = km;
        for (int m = 0; m < 2; ++m) {
            Mat& t = m ? s.rest : s.local;
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) t.r[i][j] = v[m * 12 + i * 4 + j];
                t.t[i] = v[m * 12 + i * 4 + 3];
            }
        }
        if (saved.size() < static_cast<size_t>(kMaxKnown) && ids.insert(PartId(s.count, s.local)).second)
            saved.push_back(s);  // D12b: a map written before it may hold one mesh many times
    }
    if (!ok) saved.clear(), inside.clear();
    const int n = static_cast<int>(saved.size());
    std::lock_guard<std::mutex> lock(g_in_mutex);
    if (unit == 0) g_in_inside = std::move(inside), g_in_inside_set = true;  // N55
    g_in_saved[unit] = std::move(saved);
    g_in_map_key[unit] = key;
    g_in_map[unit] = true;
    return n;
}

bool ExportMap(int unit, std::string* key, std::vector<MapPart>* parts, long long* serial) {
    if (unit < 0 || unit >= kUnits) return false;
    std::lock_guard<std::mutex> lock(g_map_mutex);
    if (g_out_key[unit].empty()) return false;
    if (key) *key = g_out_key[unit];
    if (parts) *parts = g_out_parts[unit];
    if (serial) *serial = g_out_serial[unit];
    return true;
}

std::string MapText(const std::string& key, const std::vector<MapPart>& parts) {
    std::string text = std::string(kMapHeader) + "\nkey " + key + "\n";
    text.reserve(text.size() + parts.size() * 260);
    char b[640];
    for (const MapPart& p : parts) {
        if (p.inside) {  // N55
            snprintf(b, sizeof(b), "inside %u %.4f %.4f %.4f %d %d", p.count, p.local[3], p.local[7], p.local[11], p.main,
                     p.shell);
            text += b;
            text += '\n';
            continue;
        }
        int k = snprintf(b, sizeof(b), "%u %d %d %.0f", p.count, p.cab, p.sure ? 1 : 0, p.km);
        for (const float* m : {p.local, p.rest})
            for (int i = 0; i < 3; ++i)
                k += snprintf(b + k, sizeof(b) - k, " %.5f %.5f %.5f %.4f", m[i * 4], m[i * 4 + 1], m[i * 4 + 2], m[i * 4 + 3]);
        text += b;
        text += '\n';
    }
    return text;
}

int InsideParts() { return g_inside_known; }

void TakeWorst(double us[7]) {
    for (int p = 0; p < kParts; ++p) us[p] = g_part_worst[p] * 1e6, g_part_worst[p] = 0;
}

void WriteMaps() {
    for (int u = 0; u < kUnits; ++u)
        if (!g_map_key[u].empty()) WriteMap(u);
}

Stats GetStats() {
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    return g_stats;
}

}  // namespace rig
