// V3 part 2: tells the player's truck and trailer apart from everything else
// the G-buffer pass draws, without any catalog [verified offline: V3a].
//
// Every draw carries its model->camera matrix (VS cb0 rows 0-2). Per view
// (main camera, each mirror):
//  - search: a matrix of the frame from which >= 3 draws sit at the unit's
//    wheel positions (telemetry config, < 8 cm) is the unit's frame;
//  - prediction: a unit not found gets its frame from the other unit's and
//    the relative pose of the two world placements (telemetry); in the mirrors
//    of the interior camera the trailer is often seen without the truck;
//  - members: draws at a wheel (config position + suspension deflection), or
//    with their origin inside the unit's box and a rotation the unit's parts
//    have (the unit's own, axis-swapped, or spinning about X: axles, cargo
//    pipes), belong to the unit. Their mesh (buffers + range) and pose in the
//    unit are the same in every view, so they are kept per unit, from all
//    views. A box draw only joins once it has kept its local pose while the
//    unit moved >= 1 m in the world: grass and road-side objects inside the
//    box of a stopped truck look rigid too [verified: 2b test, grass red].
//    N13: a box draw at the place and rotation of a known member that is not
//    drawn in that view this frame joins at once: the game swapped that part's
//    LOD (another mesh, the same place) [estimativa: D4b test, 97 <-> 74 truck
//    draws with the truck stopped, the new meshes left clean]. Grass gets no
//    way in: it never sits at a known part's place with its rotation.
//    N14: the new mesh must be seen in two frames (a mesh drawn once, a ring
//    buffer, never joins), the member it replaces must have been drawn in
//    that view until the new mesh appeared, a member gives way to two new
//    meshes at most, and the new mesh takes the member's cab votes only when
//    every member at that place is of one class: many parts sit at the
//    truck's origin, unturned, cab and chassis alike [verified: D4c test, a
//    chassis part took the cab's class there, known 250 -> 4000];
//  - at draw time a known rigid mesh first re-anchors the unit's frame in the
//    view (close to the previous frame's in this view, or in a view of the
//    same group: two mirrors share one viewport and may swap order), then
//    every draw of a known mesh at its known local position is the unit's.
//    Without a rigid mesh in sight (mirrors showing only a wheel's edge) the
//    previous frame's pose is held while known meshes keep matching it;
//  - a draw is learnt by one unit only: the trailer's box starts over the
//    truck's rear axles, so a draw there may fit both. At a unit's wheel it is
//    that unit's; one that fits both while the trailer is in line goes to the
//    trailer (its front parts) and to the truck once the trailer has turned
//    and it stayed with the truck [verified: V3a captures, C10];
//  - a wheel's parts match its place give or take some slack up and down (the
//    wheels hang off the axles of a truck on its side); members not matched
//    while the unit drives 50 km are forgotten, and at most kMaxKnown are kept
//    per unit (the least recently matched go first, never those matched in the
//    last two frames);
//  - interior draws (stencil ref 2: the cabin in the interior camera) are
//    never the unit's [verified: V3a captures, 88 ref-2 draws only in the
//    interior camera];
//  - the truck's cab rides on its own suspension: the telemetry gives its
//    offset about the cabin pivot [verified: 13 captures, the cab's 39 draws
//    turned by the offset's rotation as it is (N15)]. Each member learns whether it moves
//    with the cab (votes while the cab is tilted); the unit's frame is always
//    the chassis' (wheel fit, chassis parts, or cab parts minus the cab
//    offset), and cab draws are told apart so their dust can follow the cab
//    [verified: D3c test, the dust slid on the chassis when the cab rolled].
// A unit is thus recognised one frame after it appears; a miss leaves the
// game's drawing untouched (safe).
// N47 (the user, 30/10: "the player should see the truck dirty as soon as he
// gets in"): a box draw waits for 1 m of driving only the first time. The
// members away from the wheels are kept per vehicle between sessions (the
// map: ExportMap / ImportMap, a file per truck or trailer model in the
// plugin's folder): each one's index count, its pose in the unit and its cab
// votes. A box draw with a remembered index count at a remembered pose
// (kSamePose, kLodTurn) joins at once, with the votes it had. The buffers and
// their offsets are another session's (the game packs its meshes in big
// shared buffers, in the order they load) [verified: V3a run1 x run2, the
// same truck in two sessions: 8 of 71 of its draws at the same offsets, 62
// with the same index count]; grass and road-side meshes do not sit at a
// truck part's place, with its rotation and its index count. Pure geometry, no D3D: also run offline on
// the V3a captures (tools/rigtest).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rig {

// N21: 0 = truck, 1 = trailer.0, 2-5 = trailer.1-4 (a road train's dollies and trailers), each hooked to the one before.
constexpr int kUnits = 6;
constexpr int kMaxWheels = 14;
constexpr int kMaxViews = 8;
constexpr int kMaxKnown = 4096;  // members (meshes x positions) kept per unit

struct UnitConfig {
    int wheels = 0;  // 0 = unit absent
    float wheel[kMaxWheels][3] = {};
    float radius[kMaxWheels] = {};  // N22: the tyre's radius (SDK wheel.radius; 0: not given)
    bool has_hook = false;
    float hook[3] = {};  // trailer: hook.position (front end of the box)
};

// Game thread: the wheel layout (configuration event) and the world placement
// (every frame: x y z, heading pitch roll in turns). Copied at BeginFrame.
// A new layout makes that unit (only) forget what it learned; the same layout
// sent again changes nothing.
void SetUnit(int unit, const UnitConfig& c);
void SetPlacement(int unit, const double placement[6], bool valid);
// Game thread, every frame: suspension deflection per wheel (m). A wheel's
// draw sits at its config position raised by it [verified: V3a captures].
void SetDeflection(int unit, const float* deflection, int wheels);
// Game thread: the truck's cabin pivot (config cabin.position; has = false:
// no separate cab) and, every frame, its offset (x y z, heading pitch roll in
// turns, SDK truck.cabin.offset).
void SetCabin(bool has, const float pivot[3]);
void SetCabinOffset(const float offset[6]);
// D12c, game thread: the driver's eye in the truck (config cabin.position + head_position; has = false: unknown).
// The driver is drawn in the G-buffer with the truck, a skinned model whose head and arms move: every new pose was
// another part to learn [verified: ATS 30/10, a Volvo VNL 2025, ~2400 of 3412 map parts 15-35 cm behind and below
// the eye, 101 places]. Box draws whose origin is within kDriverRadius of the seat are never the truck's parts.
void SetDriver(bool has, const float eye[3]);
// N24, game thread, every frame: the unit hooked behind `unit` (trailer.1 behind trailer.0), its
// hook.position and world placement; valid = false: none. The unit's box then ends just behind that
// hook, so the next unit's parts (a dolly's wheels) are not learnt as its own.
void SetNext(int unit, const float hook[3], const double placement[6], bool valid);

struct DrawKey {
    const void* vb = nullptr;
    const void* ib = nullptr;
    uint32_t start = 0;
    int32_t base = 0;
    uint32_t count = 0;
};

// Render thread.
void BeginFrame();
// A G-buffer draw of view `view` (0..kMaxViews-1, in view group `group`:
// views of one viewport size) with its 3x4 model->camera matrix (row-major,
// translation in m[3], m[7], m[11]). `interior`: drawn with stencil ref 2.
// Returns the unit it belongs to, or -1.
// `cab` (optional) is set when the draw moves with the truck's cab.
// N55: `main_view` = the camera's own view (not a mirror's): with `interior`, a draw of the cab's inside there.
// N57b: *part (when given) = the matched part's id (its unit, index count and place in the unit, 5 cm): the same
// part keeps it when the game moves its mesh to another buffer range; 0 when nothing matched.
int Classify(int view, int group, const DrawKey& key, const float m[12], bool interior = false, bool* cab = nullptr,
             bool main_view = false, uint64_t* part = nullptr);
// N55 (render thread): the truck's inside parts learned (the dust boxes of the cab are not needed past a few).
int InsideParts();
// D14c (render thread): each end-frame part's worst frame (us) since the last call: search, anchor, learn, index, age,
// map, inside (the interior's look).
void TakeWorst(double us[7]);
// Searches, predicts and learns from the frame's draws; debug gets a line per view.
void EndFrame(std::string* debug = nullptr);

// D3, render thread, between BeginFrame and EndFrame: the unit's frame in the
// view as used for this frame's claims (unit -> camera, 3x4 row-major), false
// when the unit is not tracked in that view.
// cab = true: the frame of the truck's cab (the chassis frame moved by the cab
// offset), for the pixels of cab draws.
bool GetFrame(int view, int unit, float m[12], bool cab = false);
// D13d: the unit's box in its own frame (where its parts' origins are, from its wheels and hook), false when the
// unit has no config.
bool GetBox(int unit, float lo[3], float hi[3]);
// The unit's wheels in its own frame this frame: x y z where the wheel's draws
// are (config + suspension deflection, N13) and radius (the centre's height
// above the ground in the config, at rest). Returns the wheel count (at most
// kMaxWheels).
int GetWheels(int unit, float wheels[][4]);
// N20: per wheel, how far out of its SDK position (away from the unit's middle, m, >= 0) the outermost
// of its parts on the axle sits (a hub cap, the outer tyre of a pair): the SDK gives some vehicles' wheels
// at the axle's end, not the tyre's middle. 0 until the unit's parts are learnt; measured every 60 frames.
void GetWheelReach(int unit, float out[kMaxWheels]);
// N22: each wheel's tyre radius as the SDK gives it (wheel.radius, m; 0 when not given).
void GetWheelRadius(int unit, float out[kMaxWheels]);

struct Stats {
    long long frames = 0, draws = 0, claimed = 0, searches = 0, found = 0, predicted = 0, lost = 0;
    long long held = 0, interior = 0, promoted = 0;
    long long inherited = 0;  // N13: box draws that joined at once as another LOD of a known member
    long long restored = 0;   // N47: box draws that joined at once from the unit's map (an earlier session)
    long long unclassed = 0;  // N14: of those, joined without the cab votes (cab and chassis parts at that place)
    long long reloaded = 0;   // D12b: box draws that took a known member's place: the same mesh in another buffer
    long long anchored = 0;   // D13: views where a unit not tracked was found again by its known parts, not its wheels
    // N55: the cab's inside learned from the interior camera: draws looked at, the truck's draws left without dust as
    // inside parts, the parts that count as inside and all those seen.
    long long inside_seen = 0, inside_skipped = 0;
    long long inside_looks = 0, inside_placed = 0;  // D14c: the main view's looks, and those placed by known meshes
    double sec_inside = 0;
    int inside_parts = 0, inside_all = 0;
    // D13: the end-frame's time by part, in seconds since the start (the ATS, 30/10: 6.4 ms a frame for 10 s around a
    // camera change): the wheel search (and its N17 check), finding a unit again by its parts, learning, the LOD place
    // index, the members' ageing and the maps.
    double sec_search = 0, sec_anchor = 0, sec_learn = 0, sec_index = 0, sec_age = 0, sec_map = 0;
    long long rejected = 0;   // N17: frames found by the wheel search without the unit's body parts there
    long long forgotten = 0;  // members dropped: aged out (not matched for 50 km driven) or past kMaxKnown
    long long handed = 0;     // members dropped because the draw is the other unit's (C10)
    long long skipped = 0;  // searches left out: the view's search backs off while nothing of the unit is drawn
    long long missed = 0;  // draws of a known mesh not claimed (off its pose, or the same model in traffic)
    long long cab_claims = 0;  // claimed draws that move with the cab
    int cab_members = 0, chassis_members = 0;  // members known to move with the cab / with the chassis
    int tracked_views[kUnits] = {};  // views where the unit is known now
    int members[kUnits] = {};        // draws of known members last frame, all views
    int known[kUnits] = {};          // members known (meshes x positions), at most kMaxKnown
    int pending[kUnits] = {};        // box draws waiting for the unit to move
    int body[kUnits] = {};           // N12: members away from the wheels (0: known by its wheels only)
    int map[kUnits] = {};            // N47: parts in the unit's map from an earlier session (ImportMap)
    int map_used[kUnits] = {};       // N47: of those, found again this session
    float wheel_out[kUnits][kMaxWheels] = {};  // N20: GetWheelReach
    int wheel_parts[kUnits][kMaxWheels] = {};  // N20: the parts on each wheel's axle it is measured from
    float tail[kUnits] = {};  // N24: where the unit's box ends behind (local z, m; 0: no unit hooked behind)
};
Stats GetStats();

// Tests and diagnostics (render thread): trailer members that are also truck members (the same mesh
// at the same place, through the units' placements this frame). A draw is learnt by one unit only: 0.
int SharedMembers();
// Tests: measures the wheels' reach now (GetWheelReach; the tracker does it every 60 frames).
void MeasureWheelReach();

// N47, any thread: the unit's map for the vehicle `key` (text from ExportMap; empty: none known). Taken at the
// next BeginFrame, after a new layout (SetUnit) sent before it; a new layout with no map after it has none.
// Returns the parts read (malformed lines and another key's text are left out).
int ImportMap(int unit, const std::string& key, const std::string& text);
// N47: one part of a map: its draw's index count, cab votes, whether it was sure to be the unit's, the distance
// driven without it being found (m), its pose in the unit and its pose at the cab's rest (3x4 row-major).
struct MapPart {
    // N55: an inside part of the truck's cab instead (count, its place in the cab's frame in local[3], [7], [11], the
    // votes of the main view and of the mirrors): a line "inside" in the map.
    bool inside = false;
    int main = 0, shell = 0;
    uint32_t count = 0;
    int cab = 0;
    bool sure = false;
    double km = 0;
    float local[12] = {}, rest[12] = {};
};
// N47, any thread: the unit's map as the tracker last copied it (every kMapEvery frames at most, when its members
// changed or it drove 1 km): its parts away from the wheels, and those of the imported map not found again this
// session (aged by the distance driven). The render thread only copies the parts; MapText makes the text, on the
// thread that writes the file (D12: ~4 us a part with snprintf, ~1 ms for a truck and trailer, was on the render
// thread). False while the unit has no key (no ImportMap since its layout). `serial` grows with every copy.
bool ExportMap(int unit, std::string* key, std::vector<MapPart>* parts, long long* serial);
// N47, any thread: the map file's text (ImportMap reads it).
std::string MapText(const std::string& key, const std::vector<MapPart>& parts);
// Tests: copies the units' maps now (ExportMap), whatever kMapEvery says.
void WriteMaps();

}  // namespace rig
