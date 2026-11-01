// N11: self-check of the assumptions the drawing makes about the game's render.
//
// A game update or a mod (truck or graphics) can break an assumption the dust
// pass relies on (see docs/compatibilidade.md). Each check here watches what
// the plugin already sees every frame and, when an assumption stops holding,
// turns off only the affected part and says why in the log, with the measured
// number and the limit. The simulation, the layers and the saved dirt are never
// touched: only the drawing.
//
//  1. stencil: the game never writes the bits the plugin uses (0xf0; N57: 0x80 for the cargo)
//     [verified: V3a, its G-buffer writes 0x0f] -> drawing off for the session;
//  2. G-buffer: a pass with 4+ targets that is not taken as the G-buffer (in the
//     summary only when no G-buffer is seen in the session: the game may have
//     other passes of 4+ targets), or no G-buffer for a while with the truck in
//     the game -> log only (nothing is drawn without it anyway);
//  3. matrix: the share of G-buffer draws whose VS cb0 is not a model->camera
//     matrix stays low [verified: testdata/rig, 114 of 32601 draws = 0.35%]
//     -> drawing off for the session when it is high for a while;
//  4. parts: a unit in the game whose parts are never recognised for a while
//     (an unusual mod model) -> log only;
//  5. rain: wipers on for a long time and no rain seen on the GPU -> log only
//     (the rain then comes from the wipers alone);
//  6. interior: in the interior camera the cabin is drawn with stencil ref 2
//     [verified: V3a, ~86 ref-2 draws per frame in its main view]; none for a
//     while -> dust off in the interior camera, back on when ref 2 is seen again.
//     The interior camera is the frame with 2+ G-buffer views: the mirrors are
//     only drawn there [verified: testdata/rig, 15 interior captures with 4-5
//     mirror views, 13 exterior ones with a single view].
//
// All windows are in real seconds (injected by the caller), never in frames:
// the decisions do not depend on the FPS. With enforce off the decisions are
// only logged ("would turn off") and nothing is turned off.
// Portable (no D3D, no windows.h): tested offline by tools/compattest.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace compat {

constexpr int kUnits = 2;  // 0 = truck, 1 = trailer (as rig::kUnits)

// ---- 1. stencil ----

// The plugin's stencil bits (the units' and the cab's codes in 0x70, N57: a cargo's 0x80).
constexpr uint8_t kOurBits = 0xF0;

// D3D11_STENCIL_OP values.
enum StencilOp { kOpKeep = 1, kOpZero, kOpReplace, kOpIncrSat, kOpDecrSat, kOpInvert, kOpIncr, kOpDecr };

struct StencilDesc {
    bool enable = false;
    uint8_t write_mask = 0;
    // Front fail, depth fail, pass, then the same for the back face.
    int ops[6] = {kOpKeep, kOpKeep, kOpKeep, kOpKeep, kOpKeep, kOpKeep};
};

// What a depth-stencil state can set to 1: `any` bits whatever the reference
// (increment, decrement, invert), plus the reference's bits under `replace`.
struct StencilWrites {
    uint8_t any = 0;
    uint8_t replace = 0;
    uint8_t Bits(uint8_t ref) const { return static_cast<uint8_t>(any | (ref & replace)); }
};
StencilWrites WritesOf(const StencilDesc& d);
// The ops as text, e.g. "keep/keep/replace keep/keep/replace".
std::string OpsText(const StencilDesc& d);

// ---- limits [estimativa: all of them; see the development notes] ----

struct Limits {
    double max_dt_s = 0.5;             // a longer gap between frames (hitch, alt-tab) counts as this much
    double no_gbuffer_s = 15.0;        // 2: game running, truck in it, no G-buffer pass
    int max_mismatch_lines = 8;        // 2: different 4+ target signatures logged per session
    double matrix_window_s = 5.0;      // 3: one window of G-buffer draws
    long long matrix_min_draws = 300;  // 3: fewer draws in a window: no verdict
    double matrix_max_share = 0.25;    // 3: share of draws without a matrix above which a window is bad
    int matrix_bad_windows = 2;        // 3: bad windows in a row that turn the drawing off
    double parts_s = 60.0;             // 4: a unit in the game, G-buffer drawn, none of its parts recognised
    double rain_s = 120.0;             // 5: wipers on without rain seen on the GPU
    double interior_s = 30.0;          // 6: interior camera without a single ref-2 draw
    int max_repeat_lines = 3;          // 4, 5: warnings per check and unit per session
};

// ---- the checker ----

// What one frame showed (render thread, at Present).
struct Frame {
    bool paused = false;  // game paused (menu, loading): no time counts
    bool wipers = false;
    bool rain = false;  // rain seen on the GPU in the last second (textures or glass drops)
    bool unit_present[kUnits] = {};     // truck configured / trailer coupled (telemetry)
    bool unit_recognised[kUnits] = {};  // draws of the unit's known parts in this frame
    int views = 0;                      // G-buffer passes with draws (2+: interior camera with its mirrors)
    long long draws = 0;                // G-buffer draws whose VS cb0 could be read
    long long not_matrix = 0;           // of those, not a model->camera matrix
    long long ref2 = 0;                 // G-buffer draws with stencil ref 2
};

class Checker {
public:
    explicit Checker(const Limits& limits = Limits()) : lim_(limits) {}
    // enforce = false: decisions are logged only ([compat] enforce = 0).
    void Configure(bool enforce) { enforce_ = enforce; }
    bool Enforce() const { return enforce_; }

    // 1. A game state that writes some of kOurBits (`bits`, with reference `ref`).
    // `state` names it in the log (e.g. "#12 ops keep/keep/replace ...").
    void StencilConflict(double now, const std::string& state, uint8_t mask, uint8_t ref, uint8_t bits);
    // 2. A pass with 4+ targets and a depth buffer that is not the G-buffer (`what`: its formats).
    // The caller sends each signature once.
    void GBufferMismatch(double now, const std::string& what);
    // Every frame, at Present.
    void EndFrame(double now, const Frame& f);

    // The parts turned off (only ever true with enforce on).
    bool DrawingOff() const { return drawing_off_; }
    bool InteriorOff() const { return interior_off_; }

    // New log lines since the last call.
    std::vector<std::string> TakeLines();
    // Short state for the periodic stats line.
    std::string StatsText() const;
    // One line for the end of the session.
    std::string Summary() const;

private:
    void Line(double now, const std::string& text);
    // A decision: "-> <off_text>" with enforce, "-> would ..." without.
    void Decide(double now, const std::string& what, const std::string& off_text, bool* off, std::string* reason);
    void Note(const std::string& text);
    void Matrix(double now, const Frame& f, double dt);
    void Interior(double now, const Frame& f, double dt);

    Limits lim_;
    bool enforce_ = true;
    std::vector<std::string> lines_;
    std::vector<std::string> notes_;  // log-only findings, for the summary

    double last_ = -1.0;  // time of the previous frame

    // 1.
    bool drawing_off_ = false;
    std::string drawing_reason_;  // why the drawing is (or would be) off; empty = it is not
    int stencil_states_ = 0;

    // 2.
    double no_gbuffer_ = 0.0;
    bool no_gbuffer_warned_ = false;
    int mismatches_ = 0;
    std::string mismatch_;  // the first one
    long long frames_with_gbuffer_ = 0;

    // 3.
    double matrix_t_ = 0.0;
    long long matrix_draws_ = 0, matrix_bad_ = 0;
    int bad_windows_ = 0;
    double last_share_ = -1.0;  // last conclusive window
    double max_share_ = -1.0;

    // 4.
    double unseen_[kUnits] = {};
    bool parts_warned_[kUnits] = {};
    int parts_lines_[kUnits] = {};

    // 5.
    double dry_wipers_ = 0.0;
    bool rain_warned_ = false;
    int rain_lines_ = 0;

    // 6.
    bool interior_off_ = false;
    std::string interior_reason_;
    double interior_t_ = 0.0;  // interior-camera time without ref 2
    long long interior_frames_ = 0;
    int interior_offs_ = 0;
};

}  // namespace compat
