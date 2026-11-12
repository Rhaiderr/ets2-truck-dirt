// N67: the wipers, learned from the game instead of described in the INI.
//
// The user, 06/11, looking at a hand-drawn arc on his windscreen: "deveria seguir exatamente o movimento da
// paleta, n eu ter q desenhar uma forma por cima, ate pq cada caminhao muda". He is right: a fan tuned by hand
// fits one truck and no other, and four numbers in the INI cannot follow a blade that is moving.
//
// The game draws the wiper arms as part of the truck's exterior model, and they are the only parts of the cab
// that TURN while staying put: everything else on it is rigid in the cab's frame (the wheels turn, but they are
// nowhere near the windscreen; the mirrors and the doors do not move while driving). That is a signature this
// can pick out of the draws the plugin already watches, with their matrices, every frame:
//
//   - a mesh whose origin in the cab's frame stays within kPivotJitter of where it was (the pivot), while its
//     rotation moves by more than kMinSweep about one steady axis;
//   - with that pivot near the windscreen D11i measures: close to its plane, around its base, inside its width.
//
// From that come the pivot, the axis, the angle THIS frame and the sweep actually swept, all exact and all per
// truck -- a mod truck included, since nothing here is tabulated. The blade's length and which way the arm
// points come from reading the mesh's own vertices once (the farthest one from the pivot), the way D11i reads
// the windscreen's.
//
// Portable (no windows.h, no D3D): tools/wipertest drives it with made-up matrices.
#pragma once

#include <cstdint>

namespace wiper {

constexpr int kBlades = 2;          // a windscreen's two arms (a split screen's are one each, same thing here)
constexpr int kCandidates = 24;     // meshes watched at once near the windscreen
constexpr float kPivotJitter = 0.03f;   // m: how far a pivot may wander and still be a pivot
constexpr float kMinSweep = 0.26f;      // rad (~15 deg): under this it is not a wiper, it is noise
constexpr float kSameAxis = 0.95f;      // the axis must keep pointing the same way

// One arm, in the truck's frame (metres, +x right, +y up, +z rear).
struct Blade {
    float pivot[3] = {};   // where it turns
    float axis[3] = {};    // the axis it turns about, unit
    float arm[3] = {};     // along the blade right now, unit (0 until the mesh has been read)
    float length = 0.f;    // m, 0 = the mesh has not been read yet
    float angle = 0.f;     // rad now, about the axis, from the middle of the sweep
    float half = 0.f;      // rad, half of the sweep seen so far
    bool turning = false;  // it moved in the last few frames (the wipers are running)
};

// The windscreen as D11i measured it, in the truck's frame: the front z of its base, the base's height, its
// dz/dy and its top, and half its width (N64b). Nothing is learned before this is known, because the pivot's
// test needs it. half_width 0 = not measured.
void SetPane(float front_z, float base_y, float slope, float top_y, float half_width);

// Render thread, once per cab draw of the truck: a stable id for the mesh (the plugin's part id) and its 3x4
// matrix in the TRUCK's frame, row-major, translation in t[3], t[7], t[11].
void OnDraw(uint64_t mesh, const float t[12]);

// Render thread, once a frame.
void Frame();

// The blades known now; returns how many (0..kBlades). Ordered left (x < 0) first.
int Get(Blade out[kBlades]);

// A mesh whose vertices are still wanted, 0 = none. The caller reads it and answers with SetReach.
uint64_t Wanted();
// The farthest vertex of that mesh from its own origin, in the MESH's frame (m). Called once per mesh; a mesh
// that cannot be read is answered with length 0, and is not asked for again.
void SetReach(uint64_t mesh, const float local[3]);

// Everything forgotten (a new truck).
void Reset();

// For the log: how many meshes are being watched, how many turned, and how many became blades.
void Stats(int* watched, int* turning, int* blades);

// N67b: why nothing was learned. How many draws reached the learner, how many sat where a wiper turns, whether
// the pane is known at all, and the draw that came CLOSEST to the pane (its place in the truck's frame and how
// far outside the test it fell, 0 = inside). Without this, "0 of 0 watched" cannot tell a learner that never
// sees a draw from one whose test is in the wrong place.
void Why(long long* offered, long long* at_pane, bool* pane_known, float nearest[3], float* nearest_off);

}  // namespace wiper
