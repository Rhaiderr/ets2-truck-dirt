#pragma once
// P1: the pre-map of the vehicles' meshes, made from the game's own models by tools/premap/premap.py. Each line is a
// model piece: its index count, its vertex count, the box of its vertices (model space) and what it is. A truck
// mesh read once from the GPU (its indices and vertices) is looked up here by the same three.
//
// [verified: the VNL 2025's .pmg, PMG 0x15: the piece header's box equals its vertices' own; an interior accessory
// has `model` (the interior camera's) and `ext_model` (the interior seen from outside, interior/external/); 19 of
// the 22 pieces of external/s74h.pmg are among the meshes the plugin learned outside; with the box no ext_model
// piece shares its signature with another kind among the DLC's 1611 pieces.]
#include <cstdint>
#include <string>

namespace premap {

enum Kind { kNone = 0, kExt = 1, kInt = 2, kIntExt = 3, kCargo = 4, kKinds = 5 };

// The pieces of one table's text ("truck dirt premap 1", "name ...", "p count verts lo3 hi3 kind ref" lines); returns
// how many were taken (0: not a table).
int Parse(const std::string& text);
// Every *.premap in `dir` (wide path) whose name starts with `prefix` ("" = all); returns the pieces taken.
int LoadDir(const std::wstring& dir, const std::wstring& prefix = L"");
void Clear();
int Pieces();
// The kind of a mesh by its index count, vertex count and box (model space; 3 mm either way), kNone when no piece
// matches; a box matching pieces of two kinds is kNone too. N57c: no piece at that box, a cargo by the box's size
// alone (the load moved as a whole: *by_size), under the same rule.
int Lookup(uint32_t count, uint32_t verts, const float lo[3], const float hi[3], bool* by_size = nullptr);
const char* KindName(int kind);
// N57b: some piece has this index count (whatever its vertices and box): a mesh read with it but matching no piece
// points at the read (the vertex format), not at a missing table.
bool CountKnown(uint32_t count);

// N68: one wiper blade, as the game's own files describe it -- the cabin's `wiper_model` skeleton (the pivot and
// the bone at the blade's centre), its `wiper_anim` (the axis and how far the arm turns) and the windscreen's
// `wiper_data` (how long the blade is). Everything is already in the TRUCK's frame, the one the plugin measures
// the windscreen in; metres and radians. tools/premap writes these as "b" lines.
constexpr int kMaxBlades = 4;   // two on every truck seen so far; a split windscreen could have more
constexpr int kSpeeds = 3;      // the three the player cycles through

struct Blade {
    float pivot[3] = {};    // where the arm turns, at rest
    float axis[3] = {};     // the axis it turns about, unit, pointing out of the glass's back
    float arm[3] = {};      // from the pivot to the blade's centre, at rest
    float length = 0.f;     // the blade itself (m)
    float sweep = 0.f;      // how far the arm turns from rest (rad)
    int arm_bone = -1;      // N68c: which bone of the wiper model is this arm,
    int blade_bone = -1;    //   and which is its blade's centre -- where it IS comes from the model's own palette
    float stroke[kSpeeds] = {};  // N68c: each speed's stroke (s), 0 = not given
    float pause[kSpeeds] = {};   //   and its pause (s): the first is the intermittent one
    char model[96] = {};    // the wiper model this blade belongs to (a truck has one set per cabin)
};

// N68: the blades of a truck id, as telemetry gives it ("vehicle.volvo.fh_2024"; the leading "vehicle." is
// optional). A truck with several cabins has several sets, so `pane_y` and `pane_z` -- the windscreen's base and
// its front, as D11i measured them -- pick the set whose pivots sit closest to it; pass 0 for the first set.
// Returns how many blades were written (0: this truck is not in the tables).
int Blades(const char* truck_id, float pane_y, float pane_z, Blade out[kMaxBlades]);
int BladeTrucks();  // how many trucks have blades (for the log)

}  // namespace premap
