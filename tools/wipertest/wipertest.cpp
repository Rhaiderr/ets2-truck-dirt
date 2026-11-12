// N67: offline test of the wiper learner (src/wiper.cpp). Drives it with made-up matrices -- an arm turning
// about a pivot at the windscreen's base, and the things that must NOT be taken for one -- and checks what it
// learns: the pivot, the axis, the sweep, the angle this frame and, once the mesh has been read, the blade's
// direction and length.
//
// Usage: wipertest

#include <cmath>
#include <cstdio>
#include <cstring>

#include "wiper.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what, double value) {
    printf("%s %-58s %.3f\n", ok ? "ok  " : "FAIL", what, value);
    if (!ok) ++g_failures;
}

constexpr float kPi = 3.14159265f;

// A 3x4 matrix: a rotation of `angle` about `axis`, at `pivot`.
void Make(const float axis[3], float angle, const float pivot[3], float t[12]) {
    const float c = std::cos(angle), s = std::sin(angle), k = 1.f - c;
    const float x = axis[0], y = axis[1], z = axis[2];
    const float r[9] = {c + x * x * k,     x * y * k - z * s, x * z * k + y * s,
                        y * x * k + z * s, c + y * y * k,     y * z * k - x * s,
                        z * x * k - y * s, z * y * k + x * s, c + z * z * k};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) t[i * 4 + j] = r[i * 3 + j];
        t[i * 4 + 3] = pivot[i];
    }
}

// The windscreen of the user's Volvo FH, as D11i measured it in the game (06/11).
void Pane() { wiper::SetPane(-3.10f, 2.20f, 0.40f, 3.65f, 1.13f); }

// An arm turning about the axis across the pane, through the angles given.
void Sweep(uint64_t mesh, const float pivot[3], const float* angles, int n, const float* axis = nullptr) {
    // The windscreen leans back by dz/dy = 0.40, so its own normal (and the axis an arm turns about) leans with
    // it: the blade stays on the glass.
    const float lean = std::atan(0.40f);
    const float a[3] = {0.f, std::sin(lean), -std::cos(lean)};
    const float* use = axis ? axis : a;
    for (int i = 0; i < n; ++i) {
        float t[12];
        Make(use, angles[i], pivot, t);
        wiper::OnDraw(mesh, t);
        wiper::Frame();
    }
}

void Learns() {
    wiper::Reset();
    Pane();
    const float pivot[3] = {-0.51f, 2.24f, -3.08f};  // at the base, left of the middle
    // The arm sweeps from -40 to +40 degrees and back, a few times, as a wiper does.
    float angles[120];
    int n = 0;
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i <= 19; ++i) {
            const float t = float(i) / 19.f;
            angles[n++] = (pass % 2 ? 1.f - 2.f * t : -1.f + 2.f * t) * 40.f * kPi / 180.f;
        }
    Sweep(1, pivot, angles, n);

    wiper::Blade b[wiper::kBlades];
    const int got = wiper::Get(b);
    Check(got == 1, "one arm found", got);
    if (got != 1) return;
    const float dp = std::fabs(b[0].pivot[0] - pivot[0]) + std::fabs(b[0].pivot[1] - pivot[1]) +
                     std::fabs(b[0].pivot[2] - pivot[2]);
    Check(dp < 0.01f, "the pivot is where the arm turns", dp);
    const float lean = std::atan(0.40f);
    const float want[3] = {0.f, std::sin(lean), -std::cos(lean)};
    const float dot = std::fabs(b[0].axis[0] * want[0] + b[0].axis[1] * want[1] + b[0].axis[2] * want[2]);
    Check(dot > 0.99f, "the axis is the one it turns about", dot);
    Check(std::fabs(b[0].half - 40.f * kPi / 180.f) < 0.02f, "half the sweep is 40 degrees",
          b[0].half * 180.f / kPi);
    Check(b[0].turning, "it is turning", 1.0);
    Check(b[0].length == 0.f, "its length is unknown until the mesh is read", b[0].length);

    // The mesh read: the farthest vertex from the pivot is the blade's tip.
    Check(wiper::Wanted() == 1, "it asks for that mesh's vertices", double(wiper::Wanted()));
    // The tip in the MESH's own frame. A real arm lies on the glass, so in the truck's frame it has to be
    // across the axis: take the direction up the pane and turn it back by the rotation the mesh had when it was
    // first seen, which is what the learner will undo.
    float tip[3];
    {
        const float up_pane[3] = {0.f, std::cos(lean), std::sin(lean)};
        float r0[12];
        Make(want, angles[0], pivot, r0);
        for (int i = 0; i < 3; ++i)  // r0 transposed x (0.68 up the pane)
            tip[i] = 0.68f * (r0[i] * up_pane[0] + r0[4 + i] * up_pane[1] + r0[8 + i] * up_pane[2]);
    }
    wiper::SetReach(1, tip);
    Check(wiper::Wanted() == 0, "and asks only once", double(wiper::Wanted()));
    wiper::Get(b);
    Check(std::fabs(b[0].length - 0.68f) < 0.01f, "the blade is as long as the mesh reaches", b[0].length);
    const float arm_len = std::sqrt(b[0].arm[0] * b[0].arm[0] + b[0].arm[1] * b[0].arm[1] + b[0].arm[2] * b[0].arm[2]);
    Check(std::fabs(arm_len - 1.f) < 0.01f, "and it points somewhere", arm_len);
    // At the middle of its sweep the arm must stand where the mesh says it does, across the axis.
    const float along = std::fabs(b[0].arm[0] * want[0] + b[0].arm[1] * want[1] + b[0].arm[2] * want[2]);
    Check(along < 0.05f, "the blade lies across the axis, on the glass", along);
}

void NotAWiper() {
    {  // A part that does not turn: the cab, a mirror, a door while driving.
        wiper::Reset();
        Pane();
        const float pivot[3] = {-0.51f, 2.24f, -3.08f};
        float angles[40];
        for (int i = 0; i < 40; ++i) angles[i] = 0.f;
        Sweep(2, pivot, angles, 40);
        wiper::Blade b[wiper::kBlades];
        Check(wiper::Get(b) == 0, "a part that never turns is not an arm", 0.0);
    }
    {  // A part that turns but is nowhere near the windscreen: a wheel.
        wiper::Reset();
        Pane();
        const float pivot[3] = {-1.0f, 0.5f, -1.2f};
        float angles[40];
        for (int i = 0; i < 40; ++i) angles[i] = float(i) * 0.3f;
        Sweep(3, pivot, angles, 40);
        wiper::Blade b[wiper::kBlades];
        Check(wiper::Get(b) == 0, "a wheel turns, but not at the windscreen", 0.0);
    }
    {  // A part that turns a little: a needle, a vibration, rounding in the matrix.
        wiper::Reset();
        Pane();
        const float pivot[3] = {0.4f, 2.25f, -3.05f};
        float angles[40];
        for (int i = 0; i < 40; ++i) angles[i] = std::sin(float(i)) * 0.05f;  // +-3 degrees
        Sweep(4, pivot, angles, 40);
        wiper::Blade b[wiper::kBlades];
        Check(wiper::Get(b) == 0, "a part that barely moves is not an arm", 0.0);
    }
    {  // A part that travels while turning: not turning about a pivot at all.
        wiper::Reset();
        Pane();
        wiper::Blade b[wiper::kBlades];
        for (int i = 0; i < 40; ++i) {
            const float pivot[3] = {-0.51f + 0.02f * i, 2.24f, -3.08f};
            float t[12];
            const float axis[3] = {0.f, 0.37f, -0.93f};
            Make(axis, float(i) * 0.1f, pivot, t);
            wiper::OnDraw(5, t);
            wiper::Frame();
        }
        Check(wiper::Get(b) == 0, "a part that travels is not turning about a pivot", 0.0);
    }
    {  // Nothing is learned before the windscreen has been measured.
        wiper::Reset();
        wiper::SetPane(0.f, 0.f, 0.f, 0.f, 0.f);
        const float pivot[3] = {-0.51f, 2.24f, -3.08f};
        float angles[40];
        for (int i = 0; i < 40; ++i) angles[i] = float(i) * 0.05f;
        Sweep(6, pivot, angles, 40);
        wiper::Blade b[wiper::kBlades];
        Check(wiper::Get(b) == 0, "without the windscreen measured, nothing is learned", 0.0);
    }
}

void TwoArms() {
    wiper::Reset();
    Pane();
    const float left[3] = {-0.51f, 2.24f, -3.08f}, right[3] = {0.47f, 2.24f, -3.09f};
    float angles[80];
    int n = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i <= 19; ++i) {
            const float t = float(i) / 19.f;
            angles[n++] = (pass % 2 ? 1.f - 2.f * t : -1.f + 2.f * t) * 38.f * kPi / 180.f;
        }
    Sweep(7, left, angles, n);
    Sweep(8, right, angles, n);
    wiper::Blade b[wiper::kBlades];
    const int got = wiper::Get(b);
    Check(got == 2, "both arms found", got);
    if (got != 2) return;
    Check(b[0].pivot[0] < b[1].pivot[0], "the left one comes first", b[0].pivot[0]);
    Check(std::fabs(b[1].pivot[0] - right[0]) < 0.01f, "each keeps its own pivot", b[1].pivot[0]);
    int watched = 0, turning = 0, blades = 0;
    wiper::Stats(&watched, &turning, &blades);
    Check(blades == 2 && turning == 2, "the log counts them", blades);
}

void Stops() {
    wiper::Reset();
    Pane();
    const float pivot[3] = {-0.51f, 2.24f, -3.08f};
    float angles[60];
    int n = 0;
    for (int i = 0; i <= 19; ++i) angles[n++] = (-1.f + 2.f * float(i) / 19.f) * 40.f * kPi / 180.f;
    Sweep(9, pivot, angles, n);
    wiper::Blade b[wiper::kBlades];
    wiper::Get(b);
    Check(b[0].turning, "turning while it sweeps", 1.0);
    // The wipers go off: the same angle frame after frame.
    float still[60];
    for (int i = 0; i < 60; ++i) still[i] = angles[n - 1];
    Sweep(9, pivot, still, 60);
    wiper::Get(b);
    Check(!b[0].turning, "parked: it is still there, but not turning", 0.0);
    Check(std::fabs(b[0].half - 40.f * kPi / 180.f) < 0.02f, "and what it swept is remembered",
          b[0].half * 180.f / kPi);
}

}  // namespace

int main() {
    Learns();
    NotAWiper();
    TwoArms();
    Stops();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "done", g_failures);
    return g_failures ? 1 : 0;
}
