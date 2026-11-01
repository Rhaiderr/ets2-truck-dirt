// Offline test of the N11 self-check (src/compat): every check firing and not
// firing, the false alarms it must avoid (menu, loading, pause without a
// G-buffer; the exterior camera without ref 2; a single bad window), enforce = 0,
// the summary line, and the same decisions at 30, 60 and 144 FPS (the windows
// are in seconds, injected here, never in frames).
// Usage: compattest

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "compat.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what, double value = 0) {
    printf("%s %-66s %.3f\n", ok ? "ok  " : "FAIL", what, value);
    if (!ok) ++g_failures;
}

// A checker and its clock: frames at a fixed rate, every log line printed.
struct Sim {
    compat::Checker c;
    long long us = 0;  // integer microseconds: the same times on every compiler
    std::vector<std::string> lines;
    explicit Sim(bool enforce = true) {
        c.Configure(enforce);
        compat::Frame start;  // the clock starts at 0 (a frame only sets it: the first one has no length)
        start.paused = true;
        c.EndFrame(0.0, start);
    }
    double Now() const { return us / 1e6; }
    void Take() {
        for (const std::string& l : c.TakeLines()) {
            printf("    | %s\n", l.c_str());
            lines.push_back(l);
        }
    }
    double drawing_off_at = -1, interior_off_at = -1;  // first time each part turned off
    // `seconds` of frames like f at `fps` (the last one exactly `seconds` later).
    void Run(const compat::Frame& f, double seconds, int fps = 60) {
        const long long base = us, n = std::llround(seconds * fps);
        for (long long i = 1; i <= n; ++i) {
            us = base + std::llround(i * 1e6 / fps);
            c.EndFrame(Now(), f);
            if (drawing_off_at < 0 && c.DrawingOff()) drawing_off_at = Now();
            if (interior_off_at < 0 && c.InteriorOff()) interior_off_at = Now();
        }
        Take();
    }
    // One frame `gap_s` after the previous one (a hitch, a loading screen).
    void Gap(const compat::Frame& f, double gap_s) {
        us += std::llround(gap_s * 1e6);
        c.EndFrame(Now(), f);
        Take();
    }
    bool Logged(const char* text) const {
        for (const std::string& l : lines)
            if (l.find(text) != std::string::npos) return true;
        return false;
    }
    size_t Count(const char* text) const {
        size_t n = 0;
        for (const std::string& l : lines) n += l.find(text) != std::string::npos;
        return n;
    }
};

// Driving in the exterior camera, everything as the game draws it [verified: testdata/rig].
compat::Frame Driving() {
    compat::Frame f;
    f.unit_present[0] = f.unit_present[1] = true;
    f.unit_recognised[0] = f.unit_recognised[1] = true;
    f.views = 1;
    f.draws = 500;
    f.not_matrix = 2;  // 0.4 %
    return f;
}

// The interior camera: the main view and four mirrors, ~86 ref-2 draws.
compat::Frame Interior() {
    compat::Frame f = Driving();
    f.views = 5;
    f.draws = 1500;
    f.ref2 = 86;
    return f;
}

compat::StencilDesc Stencil(bool enable, uint8_t mask, int pass_op, int other_op = compat::kOpKeep) {
    compat::StencilDesc d;
    d.enable = enable;
    d.write_mask = mask;
    for (int i = 0; i < 6; ++i) d.ops[i] = (i % 3 == 2) ? pass_op : other_op;
    return d;
}

void TestStencilWrites() {
    printf("== 1. stencil: what a state writes\n");
    using namespace compat;
    // The game's G-buffer state [verified: V3a]: ALWAYS/REPLACE, mask 0x0f, ref 1.
    const StencilWrites game = WritesOf(Stencil(true, 0x0f, kOpReplace));
    Check(game.Bits(1) == 0x01 && (game.Bits(1) & kOurBits) == 0, "game G-buffer state (0x0f, replace, ref 1): 0x01, none of ours",
          game.Bits(1));
    Check((game.Bits(0xff) & kOurBits) == 0, "same state with ref 0xff: still none of ours (mask 0x0f)", game.Bits(0xff));
    const StencilWrites off = WritesOf(Stencil(false, 0xff, kOpIncr));
    Check(off.Bits(0xff) == 0, "stencil disabled: writes nothing", off.Bits(0xff));
    const StencilWrites keep = WritesOf(Stencil(true, 0xff, kOpKeep));
    Check(keep.Bits(0xff) == 0, "keep everywhere (a stencil test only): writes nothing", keep.Bits(0xff));
    const StencilWrites zero = WritesOf(Stencil(true, 0xff, kOpZero));
    Check(zero.Bits(0xff) == 0, "zero: never sets a bit", zero.Bits(0xff));
    const StencilWrites rep = WritesOf(Stencil(true, 0xff, kOpReplace));
    Check((rep.Bits(0x01) & kOurBits) == 0, "mask 0xff, replace, ref 0x01: none of ours", rep.Bits(0x01));
    Check((rep.Bits(0x21) & kOurBits) == 0x20, "mask 0xff, replace, ref 0x21: bit 0x20 of ours", rep.Bits(0x21) & kOurBits);
    const StencilWrites inc = WritesOf(Stencil(true, 0x7f, kOpIncrSat));
    Check((inc.Bits(0) & kOurBits) == 0x70, "mask 0x7f, incr_sat: all of ours, whatever the ref", inc.Bits(0) & kOurBits);
    const StencilWrites fail = WritesOf(Stencil(true, 0x40, kOpKeep, kOpInvert));
    Check((fail.Bits(0) & kOurBits) == 0x40, "mask 0x40, invert on fail only: bit 0x40", fail.Bits(0) & kOurBits);
    const std::string ops = OpsText(Stencil(true, 0x0f, kOpReplace));
    printf("    ops text: %s\n", ops.c_str());
    Check(ops == "keep/keep/replace keep/keep/replace", "ops text");
}

void TestStencilDecision() {
    printf("== 1. stencil: decision\n");
    {
        Sim s;
        s.Run(Driving(), 10);
        Check(!s.c.DrawingOff() && s.lines.empty(), "no conflict: drawing on, nothing logged");
        s.c.StencilConflict(s.Now(), "#7 ops keep/keep/replace keep/keep/replace", 0xff, 0x11, 0x10);
        s.Take();
        Check(s.c.DrawingOff(), "game writes 0x10: drawing off");
        Check(s.Logged("state #7") && s.Logged("mask 0xff") && s.Logged("measured 0x10, limit 0x00") &&
                  s.Logged("drawing off for the session"),
              "the line names the state, the mask, the measure and the limit");
        s.c.StencilConflict(s.Now(), "#9", 0x7f, 0x00, 0x70);
        s.Take();
        Check(s.c.DrawingOff() && s.Count("already decided") == 1, "a second state: one more line, still off");
        s.Run(Driving(), 60);
        Check(s.c.DrawingOff(), "stays off for the session (the state still exists)");
        printf("    summary: %s\n", s.c.Summary().c_str());
        Check(s.c.Summary().find("turned off: drawing (stencil:") != std::string::npos, "summary: drawing off by the stencil");
    }
    {
        Sim s(false);
        s.c.StencilConflict(1.0, "#7", 0xff, 0x11, 0x10);
        s.Take();
        Check(!s.c.DrawingOff() && s.Logged("would turn the drawing off (enforce = 0: only logged)"),
              "enforce = 0: logged, drawing stays on");
        printf("    summary: %s\n", s.c.Summary().c_str());
        Check(s.c.Summary().find("would have turned off (enforce = 0, nothing was): drawing") != std::string::npos,
              "summary with enforce = 0");
    }
}

void TestGBuffer() {
    printf("== 2. G-buffer\n");
    compat::Frame none = Driving();
    none.views = 0;
    none.draws = none.not_matrix = 0;
    none.unit_recognised[0] = none.unit_recognised[1] = false;
    {
        Sim s;
        compat::Frame menu = none;
        menu.paused = true;
        s.Run(menu, 120);
        Check(s.lines.empty(), "menu / pause: 120 s without a G-buffer, nothing logged");
        // Loading: a few frames, seconds apart (each counts as max_dt_s at most).
        for (int i = 0; i < 10; ++i) s.Gap(none, 8.0);
        Check(s.lines.empty(), "loading: 10 frames 8 s apart (80 s), nothing logged");
        compat::Frame no_truck = none;
        no_truck.unit_present[0] = no_truck.unit_present[1] = false;
        s.Run(no_truck, 60);
        Check(s.lines.empty(), "no truck configured yet: nothing logged");
        s.Run(Driving(), 5);
        s.Run(none, 14);
        Check(s.lines.empty(), "14 s without a G-buffer while driving: below the limit");
        s.Run(Driving(), 1);
        s.Run(none, 16);
        Check(s.Count("no G-buffer pass for") == 1 && s.Logged("limit 15 s"), "16 s without it: one line, with the limit");
        Check(!s.c.DrawingOff(), "log only: the drawing is not turned off");
        s.Run(none, 30);
        Check(s.Count("no G-buffer pass for") == 1, "still without it: no repeat");
        s.Run(Driving(), 1);
        Check(s.Logged("G-buffer pass seen again"), "back: one line");
    }
    {
        Sim s;
        for (int i = 0; i < 12; ++i) s.c.GBufferMismatch(1.0, "formats 10,10,10,11 depth 20 (#" + std::to_string(i) + ")");
        s.Take();
        Check(s.Count("is not taken as the G-buffer") == 8, "4+ target mismatches: at most 8 lines a session", double(s.lines.size()));
        printf("    summary: %s\n", s.c.Summary().c_str());
        Check(s.c.Summary().find("nothing turned off | warnings: no G-buffer seen; a 4+ target pass did not match") !=
                  std::string::npos,
              "no G-buffer in the session: the mismatch is a warning in the summary");
        s.Run(Driving(), 1);
        printf("    summary: %s\n", s.c.Summary().c_str());
        Check(s.c.Summary() == "compatibility: all ok", "with the G-buffer seen: another 4+ target pass of the game, all ok");
    }
}

void TestMatrix() {
    printf("== 3. matrix in cb0\n");
    compat::Frame bad = Driving();
    bad.not_matrix = 300;  // 60 %
    {
        Sim s;
        s.Run(Driving(), 120);
        Check(!s.c.DrawingOff() && s.lines.empty(), "0.4% for 120 s: nothing");
        printf("    stats: %s\n", s.c.StatsText().c_str());
        Check(s.c.StatsText().find("no matrix 0.40%") != std::string::npos, "stats: the measured share");
        s.Run(bad, 5);
        s.Run(Driving(), 5);
        s.Run(bad, 5);
        s.Run(Driving(), 5);
        Check(!s.c.DrawingOff(), "single bad windows between good ones: on");
        s.Run(bad, 10);
        Check(s.c.DrawingOff(), "two bad windows in a row (10 s at 60%): off");
        Check(s.Logged("60.0% of the G-buffer draws") && s.Logged("limit 25%") && s.Logged("90000 of 150000"),
              "the line has the share, the counts and the limit");
        s.Run(Driving(), 60);
        Check(s.c.DrawingOff(), "stays off for the session");
    }
    {
        Sim s;
        compat::Frame few = bad;
        few.draws = 1;
        few.not_matrix = 1;  // 100 % but 150 draws a window at 30 FPS: no verdict
        s.Run(few, 60, 30);
        Check(!s.c.DrawingOff() && s.lines.empty(), "too few draws per window (150 < 300): no verdict");
        compat::Frame paused = bad;
        paused.paused = true;
        s.Run(paused, 60);
        Check(!s.c.DrawingOff(), "paused: nothing counts");
    }
    {
        Sim s(false);
        s.Run(bad, 12);
        Check(!s.c.DrawingOff() && s.Logged("would turn the drawing off"), "enforce = 0: logged only");
    }
}

void TestParts() {
    printf("== 4. wheels and parts\n");
    compat::Frame f = Driving();
    f.unit_recognised[1] = false;  // a trailer whose parts are never found
    Sim s;
    s.Run(Driving(), 30);
    s.Run(f, 59);
    Check(s.lines.empty(), "trailer unrecognised 59 s: nothing yet");
    compat::Frame paused = f;
    paused.paused = true;
    s.Run(paused, 30);
    compat::Frame no_gbuffer = f;
    no_gbuffer.views = 0;
    s.Run(no_gbuffer, 5);
    Check(s.Count("none of its parts recognised") == 0, "paused or no G-buffer: does not count");
    s.Run(f, 2);
    Check(s.Count("parts: trailer in the game") == 1 && s.Logged("limit 60 s"), "60 s: one line for the trailer");
    Check(s.Count("parts: truck") == 0 && !s.c.DrawingOff(), "the truck is fine; log only");
    s.Run(Driving(), 1);
    Check(s.Logged("parts: trailer recognised again"), "recognised again: one line");
    compat::Frame uncoupled = f;
    uncoupled.unit_present[1] = false;
    s.Run(uncoupled, 120);
    Check(s.Count("parts: trailer in the game") == 1, "trailer uncoupled: no line");
}

void TestRain() {
    printf("== 5. rain\n");
    compat::Frame rain = Driving();
    rain.wipers = true;
    rain.rain = true;
    compat::Frame dry = rain;
    dry.rain = false;
    Sim s;
    s.Run(rain, 300);
    Check(s.lines.empty(), "wipers on with rain for 300 s: nothing");
    s.Run(dry, 100);
    s.Run(Driving(), 1);  // wipers off: a new run
    s.Run(dry, 100);
    Check(s.lines.empty(), "two dry wiper runs of 100 s: nothing (limit per run)");
    s.Run(dry, 21);
    Check(s.Count("rain: wipers on for") == 1 && s.Logged("limit 120 s"), "121 s with wipers and no rain: one line");
    s.Run(dry, 100);
    Check(s.Count("rain: wipers on for") == 1, "no repeat");
    s.Run(rain, 1);
    Check(s.Logged("rain seen on the GPU again"), "rain back: one line");
}

void TestInterior() {
    printf("== 6. interior (stencil ref 2)\n");
    compat::Frame lost = Interior();
    lost.ref2 = 0;
    {
        Sim s;
        s.Run(Driving(), 120);
        Check(!s.c.InteriorOff() && s.lines.empty(), "exterior camera (1 view, no ref 2) for 120 s: nothing");
        s.Run(Interior(), 120);
        Check(!s.c.InteriorOff() && s.lines.empty(), "interior camera with ref 2: nothing");
        s.Run(lost, 29);
        Check(!s.c.InteriorOff(), "29 s of interior camera without ref 2: still on");
        s.Run(Driving(), 60);
        s.Run(lost, 2);
        Check(s.c.InteriorOff() && s.Logged("limit > 0 in 30 s"), "31 s in all (the exterior in between does not reset): off");
        Check(!s.c.DrawingOff(), "only the interior: the exterior keeps its dust");
        s.Run(lost, 60);
        Check(s.Count("interior: no ref-2 draw") == 1, "no repeat while off");
        s.Run(Interior(), 1);
        Check(!s.c.InteriorOff() && s.Logged("interior dust back on"), "ref 2 back: interior dust back on");
        printf("    summary: %s\n", s.c.Summary().c_str());
        Check(s.c.Summary().find("interior dust 1 time(s), back on later") != std::string::npos, "summary: off once, back on");
        s.Run(lost, 31);
        Check(s.c.InteriorOff(), "lost again: off again");
        printf("    summary: %s\n", s.c.Summary().c_str());
    }
    {
        Sim s(false);
        s.Run(lost, 31);
        Check(!s.c.InteriorOff() && s.Logged("would turn the interior dust off"), "enforce = 0: logged only");
    }
}

// The same session at three frame rates: the same decisions, within two frames (at 30 FPS) of the same time.
void TestFps() {
    printf("== FPS independence\n");
    compat::Frame bad = Driving();
    bad.not_matrix = 300;
    compat::Frame lost = Interior();
    lost.ref2 = 0;
    const int rates[] = {30, 60, 144};
    double t_matrix[3] = {}, t_interior[3] = {};
    for (int i = 0; i < 3; ++i) {
        Sim s;
        s.Run(Driving(), 20, rates[i]);
        s.Run(lost, 40, rates[i]);
        s.Run(bad, 20, rates[i]);
        t_interior[i] = s.interior_off_at;
        t_matrix[i] = s.drawing_off_at;
        printf("    %3d FPS: interior off at %.3f s, drawing off at %.3f s, lines %zu\n", rates[i], t_interior[i],
               t_matrix[i], s.lines.size());
        Check(s.c.InteriorOff() && s.c.DrawingOff() && s.lines.size() == 2, "both decisions taken, nothing else");
    }
    for (int i = 1; i < 3; ++i) {
        Check(std::fabs(t_interior[i] - t_interior[0]) <= 2.0 / 30, "interior: same time at every rate",
              t_interior[i] - t_interior[0]);
        Check(std::fabs(t_matrix[i] - t_matrix[0]) <= 2.0 / 30, "matrix: same time at every rate",
              t_matrix[i] - t_matrix[0]);
    }
}

void TestSummary() {
    printf("== summary\n");
    Sim s;
    s.Run(Driving(), 60);
    s.Run(Interior(), 60);
    printf("    summary: %s\n", s.c.Summary().c_str());
    Check(s.c.Summary() == "compatibility: all ok", "a normal session: all ok");
    Sim z(false);
    z.Run(Driving(), 10);
    Check(z.c.Summary() == "compatibility: all ok (enforce = 0)", "all ok with enforce = 0");
    printf("    stats: %s\n", z.c.StatsText().c_str());
    Check(z.c.StatsText().find("[enforce 0]") != std::string::npos, "stats show enforce = 0");
}

}  // namespace

int main() {
    TestStencilWrites();
    TestStencilDecision();
    TestGBuffer();
    TestMatrix();
    TestParts();
    TestRain();
    TestInterior();
    TestFps();
    TestSummary();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "all compat checks passed", g_failures);
    return g_failures ? 1 : 0;
}
