// N67: see wiper.h.
#include "wiper.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wiper {
namespace {

// A mesh being watched: where it sits, how it was turned when first seen, and how far it has turned since.
struct Candidate {
    uint64_t mesh = 0;
    bool used = false;
    float pivot[3] = {};    // the mean of its origins (it must hardly move)
    int pivot_n = 0;
    float r0[9] = {};       // its rotation when first seen
    float prev[9] = {};     // and the frame before this one, to tell whether it is moving NOW
    float axis[3] = {};     // the axis it turns about (from the first turn past kMinSweep)
    bool axis_known = false;
    float ref[3] = {};      // a direction perpendicular to the axis, for the signed angle
    float lo = 0.f, hi = 0.f;  // the signed angles seen
    float angle = 0.f;      // the signed angle now
    int frames = 0;
    int still = 0;          // frames in a row without turning
    bool blade = false;     // it passed every test
    bool asked = false;     // its vertices have been asked for
    float local[3] = {};    // the farthest vertex from the pivot, in the mesh's frame
    float first_arm[3] = {};  // that direction in the TRUCK's frame, as the arm stood when first seen
    float length = 0.f;
};

Candidate g_cand[kCandidates];
float g_pane[5] = {};  // front z, base y, slope, top y, half width
bool g_pane_known = false;
int g_blades = 0;
// N67b: for the log.
long long g_offered = 0, g_at_pane = 0;
float g_near[3] = {};
float g_near_off = 1e9f;

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void Cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool Norm(float v[3]) {
    const float n = std::sqrt(Dot(v, v));
    if (!(n > 1e-6f)) return false;
    v[0] /= n, v[1] /= n, v[2] /= n;
    return true;
}

// The rotation rows of a 3x4 matrix.
void Rot(const float t[12], float r[9]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = t[i * 4 + j];
}

// b x a^T (both rotations): how much b turned from a.
void RelRot(const float b[9], const float a[9], float out[9]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i * 3 + j] = b[i * 3] * a[j * 3] + b[i * 3 + 1] * a[j * 3 + 1] + b[i * 3 + 2] * a[j * 3 + 2];
}

void Apply(const float r[9], const float v[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = r[i * 3] * v[0] + r[i * 3 + 1] * v[1] + r[i * 3 + 2] * v[2];
}

// The angle and axis of a rotation; false when it is too small to give an axis.
bool AxisAngle(const float r[9], float axis[3], float* angle) {
    const float tr = r[0] + r[4] + r[8];
    const float c = std::min(1.f, std::max(-1.f, (tr - 1.f) * 0.5f));
    *angle = std::acos(c);
    if (!(*angle > 1e-3f)) return false;
    axis[0] = r[7] - r[5], axis[1] = r[2] - r[6], axis[2] = r[3] - r[1];
    return Norm(axis);
}

// Is this pivot where a wiper's would be? Against the windscreen D11i measured: near its plane, from a little
// below its base up to a third of the way towards its top, and inside its width. A wiper turns at the bottom of
// the glass; nothing else on the cab turns anywhere near there.
bool AtThePane(const float p[3]) {
    if (!g_pane_known) return false;
    const float base = g_pane[1], top = g_pane[3], half = g_pane[4];
    const float h = top - base;
    if (!(h > 0.2f) || !(half > 0.2f)) return false;
    const float plane = g_pane[0] + std::max(p[1] - base, 0.f) * g_pane[2];
    // N67b: how far outside the test this origin falls, so the log can say where the cab's parts really are.
    const float off = std::max(std::fabs(p[2] - plane) - 0.45f, 0.f) + std::max(base - 0.45f - p[1], 0.f) +
                      std::max(p[1] - (base + 0.35f * h), 0.f) + std::max(std::fabs(p[0]) - half * 1.25f, 0.f);
    if (off < g_near_off) {
        g_near_off = off;
        g_near[0] = p[0], g_near[1] = p[1], g_near[2] = p[2];
    }
    return off <= 0.f;
}

Candidate* Find(uint64_t mesh) {
    Candidate* free_one = nullptr;
    for (Candidate& c : g_cand) {
        if (c.used && c.mesh == mesh) return &c;
        if (!c.used && !free_one) free_one = &c;
    }
    return free_one;
}

void Count() {
    g_blades = 0;
    for (const Candidate& c : g_cand)
        if (c.used && c.blade) ++g_blades;
}

}  // namespace

void SetPane(float front_z, float base_y, float slope, float top_y, float half_width) {
    g_pane[0] = front_z, g_pane[1] = base_y, g_pane[2] = slope, g_pane[3] = top_y, g_pane[4] = half_width;
    g_pane_known = half_width > 0.f && top_y > base_y;
}

void OnDraw(uint64_t mesh, const float t[12]) {
    ++g_offered;  // N67b
    if (!mesh || !g_pane_known) return;
    const float p[3] = {t[3], t[7], t[11]};
    Candidate* c = Find(mesh);
    if (!c) return;  // the table is full of nearer candidates
    if (!c->used) {
        if (!AtThePane(p)) return;  // only what sits where a wiper turns is worth watching
        ++g_at_pane;  // N67b
        *c = Candidate();
        c->used = true, c->mesh = mesh;
        memcpy(c->pivot, p, sizeof(p));
        c->pivot_n = 1;
        Rot(t, c->r0);
        memcpy(c->prev, c->r0, sizeof(c->r0));
        c->frames = 1;
        return;
    }
    // The pivot must stay put: a part that travels is not turning about anything.
    const float d[3] = {p[0] - c->pivot[0], p[1] - c->pivot[1], p[2] - c->pivot[2]};
    if (Dot(d, d) > kPivotJitter * kPivotJitter) {
        *c = Candidate();  // it moved: not this one
        return;
    }
    const float n = float(c->pivot_n);
    for (int i = 0; i < 3; ++i) c->pivot[i] = (c->pivot[i] * n + p[i]) / (n + 1.f);
    ++c->pivot_n;
    ++c->frames;

    float r[9], rel[9], step[9], axis[3], sa = 0.f, a = 0.f;
    Rot(t, r);
    // Moving NOW is this frame against the one before, not against the first one: an arm parked at the end of
    // its sweep is 40 degrees from where it started and is not moving at all.
    RelRot(r, c->prev, step);
    float step_axis[3];
    const bool moved = AxisAngle(step, step_axis, &sa) && sa > 0.004f;
    memcpy(c->prev, r, sizeof(r));
    if (moved) c->still = 0;
    else ++c->still;
    // How far it has turned from where it was first seen: that is what gives the axis and the sweep.
    RelRot(r, c->r0, rel);
    if (!AxisAngle(rel, axis, &a)) return;
    if (!c->axis_known) {
        if (a < kMinSweep) return;  // too small to trust an axis to
        // N67d: the axis is kept pointing OUT of the windscreen (towards the front of the truck, -z). Which of
        // the two senses AxisAngle hands back depends on which way the arm happened to turn first, and the sign
        // of every angle below follows from it -- so without this the arc could sweep backwards.
        if (axis[2] > 0.f) axis[0] = -axis[0], axis[1] = -axis[1], axis[2] = -axis[2];
        memcpy(c->axis, axis, sizeof(axis));
        // A direction across the axis, to measure signed angles from.
        const float up[3] = {0.f, 1.f, 0.f}, fwd[3] = {0.f, 0.f, -1.f};
        const float* seed = std::fabs(Dot(c->axis, up)) < 0.9f ? up : fwd;
        float ref[3];
        Cross(c->axis, seed, ref);
        if (!Norm(ref)) return;
        memcpy(c->ref, ref, sizeof(ref));
        c->axis_known = true;
        c->blade = true;  // it turns, about one axis, where a wiper turns
    } else if (std::fabs(Dot(c->axis, axis)) < kSameAxis) {
        *c = Candidate();  // it turns about something else each time: not an arm
        return;
    }
    // The signed angle from the first frame's orientation, about the axis.
    float v[3];
    Apply(rel, c->ref, v);
    float cr[3];
    Cross(c->ref, v, cr);
    c->angle = std::atan2(Dot(c->axis, cr), Dot(c->ref, v));
    c->lo = std::min(c->lo, c->angle);
    c->hi = std::max(c->hi, c->angle);
    Count();
}

void Frame() {
    for (Candidate& c : g_cand)
        if (c.used && c.blade && c.still > 30) c.still = 31;  // kept, but not turning
}

int Get(Blade out[kBlades]) {
    Candidate* picked[kBlades] = {};
    int n = 0;
    for (Candidate& c : g_cand) {
        if (!c.used || !c.blade) continue;
        if (n < kBlades) {
            picked[n++] = &c;
        } else {  // keep the two that have swept the most: those are the arms
            int worst = 0;
            for (int i = 1; i < kBlades; ++i)
                if (picked[i]->hi - picked[i]->lo < picked[worst]->hi - picked[worst]->lo) worst = i;
            if (c.hi - c.lo > picked[worst]->hi - picked[worst]->lo) picked[worst] = &c;
        }
    }
    if (n == 2 && picked[0]->pivot[0] > picked[1]->pivot[0]) std::swap(picked[0], picked[1]);
    for (int i = 0; i < n; ++i) {
        const Candidate& c = *picked[i];
        Blade& b = out[i];
        b = Blade();
        memcpy(b.pivot, c.pivot, sizeof(b.pivot));
        memcpy(b.axis, c.axis, sizeof(b.axis));
        b.half = 0.5f * (c.hi - c.lo);
        b.angle = c.angle - 0.5f * (c.hi + c.lo);  // from the middle of the sweep
        b.length = c.length;
        b.turning = c.still <= 30;
        if (c.length > 0.f) {
            // The arm pointed along `local` when it was first seen; it has turned c.angle about the axis since,
            // so where it points now is Rodrigues' rotation of that direction.
            const float s = std::sin(c.angle), co = std::cos(c.angle);
            float v[3] = {c.first_arm[0], c.first_arm[1], c.first_arm[2]};
            float cr[3];
            Cross(c.axis, v, cr);
            const float dp = Dot(c.axis, v);
            for (int k = 0; k < 3; ++k) b.arm[k] = v[k] * co + cr[k] * s + c.axis[k] * dp * (1.f - co);
            Norm(b.arm);
        }
    }
    return n;
}

uint64_t Wanted() {
    for (Candidate& c : g_cand)
        if (c.used && c.blade && !c.asked) return c.mesh;
    return 0;
}

void SetReach(uint64_t mesh, const float local[3]) {
    for (Candidate& c : g_cand) {
        if (!c.used || c.mesh != mesh) continue;
        c.asked = true;
        memcpy(c.local, local, sizeof(c.local));
        c.length = std::sqrt(Dot(local, local));
        if (c.length > 1.5f || c.length < 0.15f) {  // not an arm's reach: left unknown
            c.length = 0.f;
            return;
        }
        // The mesh gives it in its own frame; the arc is worked out in the truck's, so turn it by the rotation
        // the mesh had when it was first seen. Everything after that is the angle it has turned since.
        float v[3] = {local[0], local[1], local[2]};
        Norm(v);
        Apply(c.r0, v, c.first_arm);
        Norm(c.first_arm);
        return;
    }
}

void Reset() {
    for (Candidate& c : g_cand) c = Candidate();
    g_blades = 0;
    g_offered = g_at_pane = 0;
    g_near_off = 1e9f;
    g_near[0] = g_near[1] = g_near[2] = 0.f;
}

void Why(long long* offered, long long* at_pane, bool* pane_known, float nearest[3], float* nearest_off) {
    if (offered) *offered = g_offered;
    if (at_pane) *at_pane = g_at_pane;
    if (pane_known) *pane_known = g_pane_known;
    if (nearest)
        for (int i = 0; i < 3; ++i) nearest[i] = g_near[i];
    if (nearest_off) *nearest_off = g_near_off < 1e8f ? g_near_off : -1.f;  // -1 = no draw was ever looked at
}

void Stats(int* watched, int* turning, int* blades) {
    int w = 0, t = 0, b = 0;
    for (const Candidate& c : g_cand) {
        if (!c.used) continue;
        ++w;
        if (c.blade) ++b;
        if (c.blade && c.still <= 30) ++t;
    }
    if (watched) *watched = w;
    if (turning) *turning = t;
    if (blades) *blades = b;
}

}  // namespace wiper
