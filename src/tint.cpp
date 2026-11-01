// V3 part 2 + D3: red tint or dust on the player's rig (see tint.h).

#include "tint.h"

#include <d3dcompiler.h>

#include <intrin.h>
#include <smmintrin.h>

#include <algorithm>
#include <climits>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "compat.h"
#include "dust_shader.h"
#include "premap.h"
#include "rig_tracker.h"

namespace tint {
namespace {

// Stencil bits the game leaves alone [verified: V3a, its G-buffer writes 0x0f]: a code per class of pixel, so
// the dust pass knows whose pixel it is: the truck (its cab apart: its dust follows the cab on its suspension) and
// each trailer. N21: the three bits hold 7 codes: truck 0x10, cab 0x50, trailer.0 0x20, trailer.1-4 0x30, 0x40,
// 0x60, 0x70 (a road train's dollies and trailers).
constexpr UINT8 kUnitCode[rig::kUnits] = {0x10, 0x20, 0x30, 0x40, 0x60, 0x70};
constexpr UINT8 kCabCode = 0x50;
// N57: a load on a trailer (the pre-map's cargo pieces) is its trailer's code with 0x80: its own dirt, gathered since it
// was picked up [the bit as 0x70 in every captured game state: the G-buffer's write 0x0f (V3a), the states that write
// 0xff write all four of ours alike (D11l captures, 13 frames)].
constexpr UINT8 kCargoBit = 0x80;
constexpr UINT8 kBits = 0xF0;
static_assert(rig::kUnits == 6, "one stencil code per unit");
// The dust pass draws once per class of pixel: truck chassis, truck cab, each trailer, each trailer's cargo.
struct DustClass {
    int unit;
    bool cab;
    UINT8 ref;
    bool cargo = false;
};
constexpr DustClass kClasses[] = {{0, false, 0x10},       {0, true, 0x50},        {1, false, 0x20},
                                  {2, false, 0x30},       {3, false, 0x40},       {4, false, 0x60},
                                  {5, false, 0x70},       {1, false, 0xA0, true}, {2, false, 0xB0, true},
                                  {3, false, 0xC0, true}, {4, false, 0xE0, true}, {5, false, 0xF0, true}};
constexpr int kClassCount = 12;
// The class of a marked draw (kClasses' index).
int ClassOf(int unit, bool cab, bool cargo = false) {
    return unit == 0 ? (cab ? 1 : 0) : cargo ? unit + 6 : unit + 1;
}

int g_mode = 0;  // 0 off, 1 red, 2 dust, 3 position test, 4 diagnosis (N26)
bool g_on = false;
std::atomic<float> g_dust[rig::kUnits];
// The D4 layers of each unit: body, front and reach slots (dust::Layers), one value at a time.
std::atomic<float> g_layers[rig::kUnits][3][dust::kSlots];
// N57: each trailer's cargo: its dust and layers (SetCargo).
std::atomic<float> g_cargo_dust[rig::kUnits];
std::atomic<float> g_cargo_layers[rig::kUnits][3][dust::kSlots];
std::atomic<long long> g_cargo_marked{0};
// N12: each wheel's steering and rolling turn (turns), and the unit's speed (km/h).
std::atomic<float> g_steer[rig::kUnits][dust::kWheels], g_roll[rig::kUnits][dust::kWheels], g_speed[rig::kUnits];
bool g_busy = false;  // render thread only
// N41: the cab's inside, a box in the truck's frame (SetCabInside); on = known and half width > 0.
std::mutex g_inside_mutex;
bool g_inside_on = false;
float g_inside_lo[3] = {}, g_inside_hi[3] = {};
bool g_cavity_on = false;  // D11f: the cab's whole cavity, its faces turned to its middle
float g_cavity_lo[3] = {}, g_cavity_hi[3] = {};
// D11i: the driver's eye (truck frame) and the windshield's front measured from the game's glass (1e9: none yet).
bool g_eye_known = false;
float g_eye[3] = {};
float g_ws_front = 1e9f;
float g_ws_base_y = 0.f, g_ws_slope = 0.f, g_ws_top = 0.f;  // D11j: the windshield's base height, dz/dy, top
std::atomic<bool> g_ws_reset{false};  // a new truck: measure again
std::atomic<int> g_mp_int_ext{0};  // P1: the current truck's interior pieces found (MeshPrint): the boxes go
std::atomic<bool> g_mp_reset{false};  // P1: a new truck
// D11i: the windshield's measure (WindshieldDraw, WindshieldStep below); render thread.
struct Windshield {
    // The frame's main view: the largest G-buffer view; its depth buffer is the one the forward draws test.
    float frame_area = 0.f;
    int main_view = -1;
    const void* main_depth = nullptr;  // compared only
    bool forward = false;              // the targets bound now are over that depth buffer
    struct Seen {
        const void* ib;
        UINT start, count;
        INT base;
    };
    std::vector<Seen> seen;  // meshes tried (once each)
    int measured = 0;        // meshes read whole
    // The mesh being read: 1 its indices to copy, 2 copied (to map), 3 its vertices copied (to map).
    int stage = 0, wait = 0;
    ID3D11Buffer* ib = nullptr;
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* staging = nullptr;
    DXGI_FORMAT ib_format = DXGI_FORMAT_UNKNOWN;
    UINT ib_offset = 0, vb_offset = 0, stride = 0, start = 0, count = 0, first = 0, vertices = 0;
    INT base = 0;
    float to_truck[12] = {};  // the mesh -> the truck's frame
};
Windshield g_ws;
std::mutex g_err_mutex;
std::string g_error;

void Error(const std::string& e) {
    std::lock_guard<std::mutex> lock(g_err_mutex);
    if (g_error.empty()) g_error = e;
}

// ---- tracked game state (immediate context, render thread) ----

ID3D11DepthStencilState* g_dss = nullptr;
UINT g_ref = 0;
D3D11_VIEWPORT g_vp = {};
ID3D11Buffer* g_cb0 = nullptr;
UINT g_cb0_first = 0;
ID3D11Buffer* g_ib = nullptr;
ID3D11Buffer* g_vb0 = nullptr;

// Latest Map pointer of each dynamic constant buffer (the game rotates a few
// 2 MB rings [verified: tire plugin R4b2]).
constexpr int kWatched = 64;
void* g_cb_buf[kWatched] = {};
const uint8_t* g_cb_ptr[kWatched] = {};
UINT g_cb_width[kWatched] = {};
int g_cb_next = 0;  // next slot to replace when the table is full (0..kWatched-1)

// ---- the open G-buffer pass ----

struct Pass {
    bool open = false;
    ID3D11RenderTargetView* albedo = nullptr;  // rt2, referenced while open
    ID3D11RenderTargetView* spec = nullptr;    // rt1, referenced while open
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11RenderTargetView* rt0 = nullptr;     // normal + depth, referenced while open
    ID3D11RenderTargetView* rt3 = nullptr;     // N25: emittance + reflection strength, referenced while open
    int view = -1;                             // assigned at the first draw (the viewport comes after the targets)
    int group = -1;                            // views of the same viewport size (mirrors sharing a viewport)
    D3D11_VIEWPORT vp = {};
    int marked = 0;
    int marked_class[kClassCount] = {};
    bool has_proj = false;
    float proj[3][4] = {};  // camera -> clip rows x, y, w
};
Pass g_pass;

// View ids: (viewport, run of that viewport in the frame) -> id, kept across frames.
// A full table reuses its oldest entry unused for kStaleFrames (a resolution,
// render scale or window size change leaves the old viewports behind); rig's
// state of a reused view does not match the new one and is found again as
// after a camera cut.
constexpr long long kStaleFrames = 300;
long long g_frame = 0;  // Present count
struct ViewSlot {
    float w, h, x, y;
    int run;
    long long last;  // frame it was last used
};
ViewSlot g_view_slots[rig::kMaxViews];
int g_view_n = 0;
struct RunCount {
    float w, h, x, y;
    int runs;
    long long last;
};
RunCount g_runs[16];
int g_runs_n = 0;

// Group of a viewport size, for rig::Classify (kept across frames).
struct GroupSlot {
    float w, h;
    long long last;
};
GroupSlot g_groups[rig::kMaxViews];
int g_group_n = 0;

// The entry of a full table to reuse: the oldest one unused for kStaleFrames, or -1.
template <typename T>
int Stale(const T* slots, int n) {
    int best = -1;
    for (int i = 0; i < n; ++i)
        if (g_frame - slots[i].last >= kStaleFrames && (best < 0 || slots[i].last < slots[best].last)) best = i;
    return best;
}

int GroupId(const D3D11_VIEWPORT& vp) {
    for (int i = 0; i < g_group_n; ++i)
        if (g_groups[i].w == vp.Width && g_groups[i].h == vp.Height) {
            g_groups[i].last = g_frame;
            return i;
        }
    int i = g_group_n < rig::kMaxViews ? g_group_n++ : Stale(g_groups, g_group_n);
    if (i < 0) return -1;
    g_groups[i] = {vp.Width, vp.Height, g_frame};
    return i;
}

int ViewId(const D3D11_VIEWPORT& vp) {
    int i = 0;
    for (; i < g_runs_n; ++i)
        if (g_runs[i].w == vp.Width && g_runs[i].h == vp.Height && g_runs[i].x == vp.TopLeftX && g_runs[i].y == vp.TopLeftY)
            break;
    if (i == g_runs_n) {
        i = g_runs_n < 16 ? g_runs_n++ : Stale(g_runs, g_runs_n);
        if (i < 0) return -1;
        g_runs[i] = {vp.Width, vp.Height, vp.TopLeftX, vp.TopLeftY, 0, g_frame};
    }
    g_runs[i].last = g_frame;
    const int run = g_runs[i].runs++;
    for (int v = 0; v < g_view_n; ++v) {
        ViewSlot& s = g_view_slots[v];
        if (s.w == vp.Width && s.h == vp.Height && s.x == vp.TopLeftX && s.y == vp.TopLeftY && s.run == run) {
            s.last = g_frame;
            return v;
        }
    }
    const int v = g_view_n < rig::kMaxViews ? g_view_n++ : Stale(g_view_slots, g_view_n);
    if (v < 0) return -1;
    g_view_slots[v] = {vp.Width, vp.Height, vp.TopLeftX, vp.TopLeftY, run, g_frame};
    return v;
}

// ---- our objects ----

ID3D11Device* g_dev = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;       // albedo (or red)
ID3D11PixelShader* g_ps_spec = nullptr;  // specular
// D11m: the dirt worked out once per pixel into g_pack (dust_shader.h's PACK), which the draws above and below read.
ID3D11PixelShader* g_ps_pack = nullptr;
ID3D11PixelShader* g_ps_clear = nullptr;  // D13e: the clearing draw's (writes 0 into the pack)
// N25: rt3, the paint's reflection under the dirt. Compiled on a thread of its own (each variant takes 3-5 s
// [verified: d3dcompiler_47 on the dev PC]): the first pass does not wait for it; until then rt3 is left alone.
std::atomic<ID3D11PixelShader*> g_ps_refl{nullptr};
// D11k (N44): rt0's relief (the NORM variant): the normal tilted where there is dirt; null: no relief.
ID3D11PixelShader* g_ps_norm = nullptr;
std::atomic<bool> g_relief{true};  // [dust] relief
// D11e: every shader is compiled on a thread started with Configure (the dust's took ~6-9 s each on the dev PC, 29/10,
// and two of them were compiled in the game's first G-buffer pass: the game stood still that long). The pass starts
// once they are there. 0 not started, 1 compiling, 2 ready, 3 failed.
std::atomic<int> g_compile_state{0};
ID3DBlob* g_blob_vs = nullptr;
ID3DBlob* g_blob_ps = nullptr;
ID3DBlob* g_blob_spec = nullptr;
ID3DBlob* g_blob_refl = nullptr;  // null: the rt3 variant failed (the reflection stays over the dust)
ID3DBlob* g_blob_norm = nullptr;  // D11k; null: the relief variant failed (no relief)
ID3DBlob* g_blob_pack = nullptr;  // D11m
ID3DBlob* g_blob_clear = nullptr;  // D13e
std::atomic<long long> g_compile_ms{-1};
ID3D11BlendState* g_refl_bs = nullptr;   // N25: no blending (rt3 is an integer target)
ID3D11DepthStencilState* g_pass_dss = nullptr;       // tests the unit's bit and clears it
ID3D11DepthStencilState* g_pass_dss_keep = nullptr;  // tests it and keeps it (the draw before the last)
ID3D11BlendState* g_pass_bs = nullptr;
ID3D11RasterizerState* g_pass_rs = nullptr;
ID3D11RasterizerState* g_pass_rs_scissor = nullptr;  // D13d: the class's draws inside its unit's rectangle
ID3D11Buffer* g_pass_cb = nullptr;
bool g_objects_ok = false;      // every object above exists
bool g_objects_failed = false;  // a creation failed: no retry this session
// The game's state (referenced, so its address is not reused while cached) -> our variant.
std::unordered_map<ID3D11DepthStencilState*, ID3D11DepthStencilState*> g_variants;
constexpr size_t kMaxVariants = 256;  // the game makes its states at load: a few dozen
// N25: a copy of rt3 made before the pass draws into it (a target cannot be read while it is drawn into),
// remade when rt3's size or format changes.
ID3D11Texture2D* g_rt3_copy = nullptr;
ID3D11ShaderResourceView* g_rt3_srv = nullptr;
D3D11_TEXTURE2D_DESC g_rt3_desc = {};
bool g_rt3_failed = false;
std::atomic<long long> g_rt3_copies{0};
// D11k: a copy of rt0 for the relief pass (rt0 is its target), as rt3's.
// D11m: our own target (uint4, rt0's size) for the pack, and a shader view of it; made again when rt0's size changes.
ID3D11Texture2D* g_pack = nullptr;
ID3D11RenderTargetView* g_pack_rtv = nullptr;
ID3D11ShaderResourceView* g_pack_srv = nullptr;
UINT g_pack_w = 0, g_pack_h = 0;
bool g_pack_failed = false;
// N27: draws on deferred contexts, and those into a G-buffer (OnDeferredDraw).
std::atomic<long long> g_deferred_draws{0}, g_deferred_gbuffer{0};
// N28: the last draw's verdict (LastVerdict) and the indirect draws (OnIndirectDraw).
int g_verdict = -1;
std::atomic<long long> g_indirect_draws{0}, g_indirect_gbuffer{0};
std::atomic<long long> g_shifted{0};  // N29: draws whose matrix sits 1-3 rows down VS cb0
// A shader view of the normal + depth target (rt0), made once per texture.
ID3D11Resource* g_rt0_res = nullptr;
ID3D11ShaderResourceView* g_rt0_srv = nullptr;
bool g_rt0_failed = false;

// Stats (render thread writes, the log reads now and then).
std::atomic<long long> g_passes{0}, g_marked{0}, g_gbuffer_draws{0}, g_no_pointer{0}, g_not_matrix{0}, g_faults{0};
std::atomic<long long> g_no_proj{0}, g_no_frame{0}, g_unit_draws{0};

// ---- N11: self-check of the render assumptions (compat.h) ----

static_assert(compat::kOurBits == kBits, "the self-check watches the bits the pass uses");
static_assert(compat::kOpKeep == D3D11_STENCIL_OP_KEEP && compat::kOpReplace == D3D11_STENCIL_OP_REPLACE &&
                  compat::kOpDecr == D3D11_STENCIL_OP_DECR,
              "compat::StencilOp = D3D11_STENCIL_OP");
std::mutex g_compat_mutex;  // guards g_compat and g_compat_lines (the summary comes from the game thread)
compat::Checker g_compat;
std::vector<std::string> g_compat_lines;
std::atomic<bool> g_compat_pending{false};  // g_compat_lines not empty
// The checker's decisions, taken at Present (so a pass never changes halfway): render thread.
bool g_draw_off = false;      // nothing marked, the game's depth-stencil states left alone
bool g_interior_off = false;  // nothing marked in the interior camera's frames
// This frame (render thread): G-buffer views drawn, draws whose cb0 was read, not a matrix, stencil ref 2.
int g_frame_views = 0, g_prev_views = 0;
long long g_frame_draws = 0, g_frame_not_matrix = 0, g_frame_ref2 = 0;
// Game depth-stencil states checked once (referenced, so the address is not reused while kept).
struct CheckedState {
    compat::StencilWrites writes;
    int id;
    bool reported;
};
std::unordered_map<ID3D11DepthStencilState*, CheckedState> g_checked;
constexpr size_t kMaxChecked = 1024;  // the game makes a few dozen at load
ID3D11DepthStencilState* g_last_state = nullptr;  // the last one looked up (most calls repeat it)
CheckedState* g_last_checked = nullptr;
int g_state_ids = 0;
// 4+ target passes with a depth buffer that are not the G-buffer: formats already reported.
struct Signature {
    DXGI_FORMAT f[5];
};
Signature g_signatures[16];
int g_signature_n = 0;

// Cost (QPC ticks, render thread) since the last StatsLine: BeforeDrawIndexed and the
// stencil check are timed one call in kSample (and counted kSample times).
constexpr unsigned kSample = 32;
long long g_cost_draw = 0, g_cost_end_frame = 0, g_cost_compat = 0, g_cost_frames = 0;
// D13f (the user, ATS 30/10: a drop to 65 FPS that the 10 s means hide): since the last StatsLine, the longest frame
// (Present to Present, the game not paused), when it came, how many took over 13.3 ms (under 75 FPS) and over 16.7 ms
// (under 60), and the rig's longest end-frame.
long long g_frame_last = 0, g_frame_worst = 0, g_frame_slow75 = 0, g_frame_slow60 = 0, g_rig_worst = 0;
SYSTEMTIME g_frame_worst_at = {};
unsigned g_sample_draw = 0, g_sample_state = 0;

long long Ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double TickSeconds() {
    static const double f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return 1.0 / double(q.QuadPart);
    }();
    return f;
}
// Real seconds since the first call (the self-check's clock).
double Now() {
    static const long long t0 = Ticks();
    return double(Ticks() - t0) * TickSeconds();
}

// Under g_compat_mutex: the checker's new lines go out (TakeCompatLines).
void MoveCompatLines() {
    for (std::string& l : g_compat.TakeLines()) g_compat_lines.push_back(std::move(l));
    if (!g_compat_lines.empty()) g_compat_pending.store(true, std::memory_order_release);
}

const char kVs[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {"
    " float2 uv = float2((id << 1) & 2, id & 2);"
    " return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }";

// D13e: the clearing draw's pixel shader. The draw first had none and no target (depth-stencil only): the ATS's
// first pass in a rectangle raised an exception there, with SnowyMoon's dxgi.dll between us and the driver
// [estimativa: not seen in probetest, which has no such layer]; now a target (the pack) and a shader as our others.
const char kClearPs[] = "uint4 main(float4 pos : SV_Position) : SV_Target0 { return uint4(0, 0, 0, 0); }";

// The dust pixel shader and its constants: src/dust_shader.h (shared with tools/dustpreview).
const char* const kPs = dust::kPixelShader;
using PassCb = dust::Constants;
static_assert(rig::kMaxWheels == dust::kWheels, "wheel count");

bool Compile(const char* src, const char* target, ID3DBlob** out, const D3D_SHADER_MACRO* defines = nullptr) {
    // d3dcompiler_47.dll is part of Windows 10/11 (System32).
    static pD3DCompile compile = nullptr;
    if (!compile) {
        wchar_t path[MAX_PATH] = {};
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\d3dcompiler_47.dll");
        HMODULE m = LoadLibraryW(path);
        if (m) compile = reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile"));
        if (!compile) return false;
    }
    ID3DBlob* err = nullptr;
    const HRESULT hr = compile(src, strlen(src), "tint", defines, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                               out, &err);
    if (err) err->Release();
    return SUCCEEDED(hr);
}

// D13d: the GPU's time in our passes: a timestamp before and after each pass and a disjoint query around the
// frame's, read back a few frames later without waiting (a frame whose queries are still out is not timed).
constexpr int kTimerFrames = 4, kTimedPasses = 8;
struct GpuTimer {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* ts[2 * kTimedPasses] = {};
    int passes = 0;
    bool open = false, pending = false;
};
GpuTimer g_timer[kTimerFrames];
int g_timer_at = 0;
bool g_timer_failed = false;
double g_gpu_us = 0, g_gpu_max_us = 0;  // since the last StatsLine
long long g_gpu_frames = 0;
// D13d: how much of its view the copies' rectangle took, summed over the passes since the last StatsLine.
double g_rect_cover = 0, g_rect_class_cover = 0;
long long g_rect_passes = 0, g_rect_parts = 0, g_rect_unknown = 0;
// D13e: the rectangles turned off after an exception while they were in use (RectsFallback).
std::atomic<bool> g_rects_off{false};
bool g_rects_used = false;

void ReleaseTimer(GpuTimer& t) {
    if (t.disjoint) t.disjoint->Release();
    for (ID3D11Query* q : t.ts)
        if (q) q->Release();
    t = GpuTimer();
}

void ReleaseTimers() {
    for (GpuTimer& t : g_timer) ReleaseTimer(t);
    g_timer_at = 0;
}

bool TimerMade(GpuTimer& t) {
    if (t.disjoint) return true;
    if (g_timer_failed || !g_dev) return false;
    D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    bool ok = SUCCEEDED(g_dev->CreateQuery(&d, &t.disjoint));
    d.Query = D3D11_QUERY_TIMESTAMP;
    for (ID3D11Query*& q : t.ts) ok = ok && SUCCEEDED(g_dev->CreateQuery(&d, &q));
    if (!ok) ReleaseTimer(t), g_timer_failed = true;
    return ok;
}

// Before a pass: its first timestamp (the frame's disjoint query begun at its first pass); nullptr: not timed.
GpuTimer* TimerBegin(ID3D11DeviceContext* c) {
    GpuTimer& t = g_timer[g_timer_at];
    if (t.pending || t.passes >= kTimedPasses || !TimerMade(t)) return nullptr;
    if (!t.open) c->Begin(t.disjoint), t.open = true;
    c->End(t.ts[2 * t.passes]);
    return &t;
}

void TimerEnd(ID3D11DeviceContext* c, GpuTimer* t) {
    if (!t) return;
    c->End(t->ts[2 * t->passes + 1]);
    ++t->passes;
}

// At Present: the frame's queries closed, the ones ready read.
void TimerFrame(ID3D11DeviceContext* c) {
    GpuTimer& cur = g_timer[g_timer_at];
    if (cur.open) {
        c->End(cur.disjoint);
        cur.open = false, cur.pending = true;
        g_timer_at = (g_timer_at + 1) % kTimerFrames;
    }
    for (GpuTimer& t : g_timer) {
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        if (c->GetData(t.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        bool ok = !dj.Disjoint && dj.Frequency > 0;
        double us = 0;
        for (int p = 0; p < t.passes && ok; ++p) {
            UINT64 a = 0, b = 0;
            ok = c->GetData(t.ts[2 * p], &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                 c->GetData(t.ts[2 * p + 1], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (ok && b > a) us += double(b - a) * 1e6 / double(dj.Frequency);
        }
        if (ok) g_gpu_us += us, g_gpu_max_us = (std::max)(g_gpu_max_us, us), ++g_gpu_frames;
        t.pending = false, t.passes = 0;
    }
}

// After a failed MakeObjects: none of our objects is kept (all or nothing).
void ReleaseObjects() {
    for (IUnknown* u : {static_cast<IUnknown*>(g_vs), static_cast<IUnknown*>(g_ps), static_cast<IUnknown*>(g_ps_spec),
                        static_cast<IUnknown*>(g_ps_norm), static_cast<IUnknown*>(g_ps_pack),
                        static_cast<IUnknown*>(g_pass_dss),
                        static_cast<IUnknown*>(g_pass_dss_keep), static_cast<IUnknown*>(g_pass_bs),
                        static_cast<IUnknown*>(g_refl_bs), static_cast<IUnknown*>(g_pass_rs),
                        static_cast<IUnknown*>(g_pass_rs_scissor),
                        static_cast<IUnknown*>(g_pass_cb), static_cast<IUnknown*>(g_dev)})
        if (u) u->Release();
    ReleaseTimers();
    if (g_ps_clear) g_ps_clear->Release();
    g_vs = nullptr, g_ps = g_ps_spec = g_ps_norm = g_ps_pack = g_ps_clear = nullptr;
    g_pass_dss = g_pass_dss_keep = nullptr;
    g_pass_bs = g_refl_bs = nullptr, g_pass_rs = g_pass_rs_scissor = nullptr, g_pass_cb = nullptr;
    g_dev = nullptr;
}

// D11e: the shaders' bytecode, compiled on a thread (no device needed); once per session.
void StartCompile() {
    int expected = 0;
    if (!g_compile_state.compare_exchange_strong(expected, 1)) return;
    std::thread([] {
        const ULONGLONG t0 = GetTickCount64();
        // The variants side by side, one thread each. D11m: only PACK works the dirt out (~6-9 s alone, on the dev PC);
        // the others are a few lines each.
        ID3DBlob* none = nullptr;
        Compile(kVs, "vs_5_0", &none);  // loads d3dcompiler_47 first (Compile's pointer is set once)
        if (none) none->Release();
        if (!Compile(kClearPs, "ps_5_0", &g_blob_clear)) g_blob_clear = nullptr;  // D13e: without it no rectangles
        const D3D_SHADER_MACRO spec[] = {{"SPEC", "1"}, {nullptr, nullptr}};
        const D3D_SHADER_MACRO refl[] = {{"REFL", "1"}, {nullptr, nullptr}};
        const D3D_SHADER_MACRO norm[] = {{"NORM", "1"}, {nullptr, nullptr}};
        const D3D_SHADER_MACRO pack[] = {{"PACK", "1"}, {nullptr, nullptr}};
        bool done[6] = {};
        std::thread jobs[6] = {
            std::thread([&] { done[0] = Compile(kVs, "vs_5_0", &g_blob_vs); }),
            std::thread([&] { done[1] = Compile(kPs, "ps_5_0", &g_blob_ps); }),
            std::thread([&] { done[2] = Compile(kPs, "ps_5_0", &g_blob_spec, spec); }),
            std::thread([&] { done[3] = Compile(kPs, "ps_5_0", &g_blob_refl, refl); }),
            std::thread([&] { done[4] = Compile(kPs, "ps_5_0", &g_blob_norm, norm); }),
            std::thread([&] { done[5] = Compile(kPs, "ps_5_0", &g_blob_pack, pack); }),
        };
        for (std::thread& j : jobs) j.join();
        const bool ok = done[0] && done[1] && done[2] && done[5];
        if (ok && !done[3]) {
            if (g_blob_refl) g_blob_refl->Release();
            g_blob_refl = nullptr;
            Error("dust: the rt3 shader was not made: the paint's reflection stays over the dust");
        }
        if (ok && !done[4]) {
            if (g_blob_norm) g_blob_norm->Release();
            g_blob_norm = nullptr;
            Error("dust: the relief shader (D11k) was not made: the dirt stays flat");
        }
        g_compile_ms.store(static_cast<long long>(GetTickCount64() - t0));
        g_compile_state.store(ok ? 2 : 3);  // after the blobs: MakeObjects reads them once it sees 2
    }).detach();
}

bool MakeObjects(ID3D11DeviceContext* c) {
    if (g_objects_ok) return true;
    if (g_objects_failed) return false;
    // D11e: not before the compile thread is done (the game's frames go on meanwhile).
    StartCompile();
    const int state = g_compile_state.load();
    if (state == 1) return false;
    g_objects_failed = true;  // until everything below succeeds
    if (state != 2) {
        Error("tint off: shaders not compiled (d3dcompiler_47.dll)");
        return false;
    }
    c->GetDevice(&g_dev);
    if (!g_dev) return false;
    HRESULT hr = g_dev->CreateVertexShader(g_blob_vs->GetBufferPointer(), g_blob_vs->GetBufferSize(), nullptr, &g_vs);
    if (SUCCEEDED(hr)) hr = g_dev->CreatePixelShader(g_blob_ps->GetBufferPointer(), g_blob_ps->GetBufferSize(), nullptr, &g_ps);
    if (SUCCEEDED(hr))
        hr = g_dev->CreatePixelShader(g_blob_spec->GetBufferPointer(), g_blob_spec->GetBufferSize(), nullptr, &g_ps_spec);
    if (SUCCEEDED(hr))
        hr = g_dev->CreatePixelShader(g_blob_pack->GetBufferPointer(), g_blob_pack->GetBufferSize(), nullptr, &g_ps_pack);
    if (SUCCEEDED(hr) && g_blob_refl) {  // N25: without it rt3 is left alone
        ID3D11PixelShader* refl = nullptr;
        if (FAILED(g_dev->CreatePixelShader(g_blob_refl->GetBufferPointer(), g_blob_refl->GetBufferSize(), nullptr, &refl)))
            refl = nullptr, Error("dust: the rt3 shader was not made: the paint's reflection stays over the dust");
        g_ps_refl.store(refl);
    }
    if (SUCCEEDED(hr) && g_blob_clear &&  // D13e: without it the pass keeps the whole viewport
        FAILED(g_dev->CreatePixelShader(g_blob_clear->GetBufferPointer(), g_blob_clear->GetBufferSize(), nullptr, &g_ps_clear)))
        g_ps_clear = nullptr;
    if (SUCCEEDED(hr) && g_blob_norm &&  // D11k: without it no relief
        FAILED(g_dev->CreatePixelShader(g_blob_norm->GetBufferPointer(), g_blob_norm->GetBufferSize(), nullptr, &g_ps_norm)))
        g_ps_norm = nullptr, Error("dust: the relief shader (D11k) was not made: the dirt stays flat");
    // The unit's bit (the reference) under mask 0x30, cleared where it passes.
    D3D11_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable = FALSE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    d.StencilEnable = TRUE;
    d.StencilReadMask = kBits;
    d.StencilWriteMask = kBits;
    d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO, D3D11_COMPARISON_EQUAL};
    d.BackFace = d.FrontFace;
    if (SUCCEEDED(hr)) hr = g_dev->CreateDepthStencilState(&d, &g_pass_dss);
    d.FrontFace.StencilPassOp = d.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    if (SUCCEEDED(hr)) hr = g_dev->CreateDepthStencilState(&d, &g_pass_dss_keep);
    // Blended by the shader's alpha into one target (albedo, then specular);
    // their alpha channels are material parameters and are kept.
    D3D11_BLEND_DESC b = {};
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                              D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (SUCCEEDED(hr)) hr = g_dev->CreateBlendState(&b, &g_pass_bs);
    // N25: rt3 is written whole (an integer target cannot be blended): the shader keeps what it does not scale.
    b.RenderTarget[0].BlendEnable = FALSE;
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = g_dev->CreateBlendState(&b, &g_refl_bs);
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(PassCb);
    cbd.Usage = D3D11_USAGE_DEFAULT;  // filled with UpdateSubresource: no Map (our Map detour stays out of it)
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) hr = g_dev->CreateBuffer(&cbd, nullptr, &g_pass_cb);
    D3D11_RASTERIZER_DESC r = {};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = g_dev->CreateRasterizerState(&r, &g_pass_rs);
    r.ScissorEnable = TRUE;
    if (SUCCEEDED(hr)) hr = g_dev->CreateRasterizerState(&r, &g_pass_rs_scissor);
    if (FAILED(hr)) {
        char b2[96];
        snprintf(b2, sizeof(b2), "tint off: pass objects not created, hr=0x%08X", static_cast<unsigned>(hr));
        Error(b2);
        ReleaseObjects();
        return false;
    }
    g_objects_failed = false;
    g_objects_ok = true;
    return true;
}

// The stencil part of a game state, for the self-check.
compat::StencilDesc StencilOf(ID3D11DepthStencilState* s) {
    D3D11_DEPTH_STENCIL_DESC d = {};
    s->GetDesc(&d);
    compat::StencilDesc sd;
    sd.enable = d.StencilEnable != FALSE;
    sd.write_mask = d.StencilWriteMask;
    const D3D11_DEPTH_STENCILOP_DESC* faces[2] = {&d.FrontFace, &d.BackFace};
    for (int f = 0; f < 2; ++f) {
        sd.ops[f * 3] = faces[f]->StencilFailOp;
        sd.ops[f * 3 + 1] = faces[f]->StencilDepthFailOp;
        sd.ops[f * 3 + 2] = faces[f]->StencilPassOp;
    }
    return sd;
}

// N11 check 1: whether the game's state `s` with reference `ref` writes any of
// our bits. Each state is described once; one that writes them goes to the
// checker once.
void CheckStencilBody(ID3D11DepthStencilState* s, UINT ref) {
    CheckedState* cs = g_last_checked;
    if (s != g_last_state) {
        auto it = g_checked.find(s);
        if (it == g_checked.end()) {
            if (g_checked.size() >= kMaxChecked) {  // more states than expected: start over (checked again on demand)
                for (auto& e : g_checked) e.first->Release();
                g_checked.clear();
            }
            s->AddRef();
            it = g_checked.emplace(s, CheckedState{compat::WritesOf(StencilOf(s)), ++g_state_ids, false}).first;
        }
        cs = &it->second;  // stays valid until the map is cleared (above, and g_last_state changes then too)
        g_last_state = s;
        g_last_checked = cs;
    }
    const uint8_t bits = cs->writes.Bits(static_cast<uint8_t>(ref)) & compat::kOurBits;
    if (!bits || cs->reported) return;
    cs->reported = true;
    const compat::StencilDesc sd = StencilOf(s);
    std::lock_guard<std::mutex> lock(g_compat_mutex);
    g_compat.StencilConflict(Now(), "#" + std::to_string(cs->id) + " (ops " + compat::OpsText(sd) + ")", sd.write_mask,
                             static_cast<uint8_t>(ref), bits);
    MoveCompatLines();
}

void CheckStencil(ID3D11DepthStencilState* s, UINT ref) {
    if (!s) return;  // the D3D11 default state: no stencil
    if (++g_sample_state % kSample) {
        CheckStencilBody(s, ref);
        return;
    }
    const long long t0 = Ticks();
    CheckStencilBody(s, ref);
    g_cost_compat += (Ticks() - t0) * kSample;
}

// N11 check 2: a pass with 4+ targets and a depth buffer that is not the G-buffer, once per set of formats.
void ReportMismatch(const Signature& sig) {
    for (int i = 0; i < g_signature_n; ++i)
        if (!memcmp(&g_signatures[i], &sig, sizeof(sig))) return;
    if (g_signature_n < 16) g_signatures[g_signature_n++] = sig;
    else return;  // 16 different ones already: the checker logs at most a few anyway
    char b[160];
    snprintf(b, sizeof(b), "DXGI formats rt0-3 %d, %d, %d, %d, depth %d", int(sig.f[0]), int(sig.f[1]), int(sig.f[2]),
             int(sig.f[3]), int(sig.f[4]));
    std::lock_guard<std::mutex> lock(g_compat_mutex);
    g_compat.GBufferMismatch(Now(), b);
    MoveCompatLines();
}

// The game's state plus writing bits 0x30 where the draw passes.
ID3D11DepthStencilState* Variant(ID3D11DepthStencilState* s) {
    auto it = g_variants.find(s);
    if (it != g_variants.end()) return it->second;
    D3D11_DEPTH_STENCIL_DESC d = {};
    if (s) {
        s->GetDesc(&d);
    } else {  // D3D11 default state
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_LESS;
        d.StencilReadMask = d.StencilWriteMask = 0xFF;
        d.FrontFace = d.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                    D3D11_COMPARISON_ALWAYS};
    }
    if (!d.StencilEnable) {
        d.StencilEnable = TRUE;
        d.StencilReadMask = 0xFF;
        d.StencilWriteMask = kBits;
        d.FrontFace = d.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                                    D3D11_COMPARISON_ALWAYS};
    } else {
        // REPLACE writes the reference under the mask: the game's bits keep the
        // game's reference; with another pass op only our bits are written.
        const bool replace = d.FrontFace.StencilPassOp == D3D11_STENCIL_OP_REPLACE &&
                             d.BackFace.StencilPassOp == D3D11_STENCIL_OP_REPLACE;
        d.StencilWriteMask = static_cast<UINT8>((replace ? d.StencilWriteMask : 0) | kBits);
        d.FrontFace.StencilPassOp = d.BackFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    }
    ID3D11DepthStencilState* v = nullptr;
    if (FAILED(g_dev->CreateDepthStencilState(&d, &v))) v = nullptr;
    if (g_variants.size() >= kMaxVariants) {  // more states than expected: start over (made again on demand)
        for (auto& e : g_variants) {
            if (e.first) e.first->Release();
            if (e.second) e.second->Release();
        }
        g_variants.clear();
    }
    if (s) s->AddRef();
    g_variants[s] = v;
    return v;
}

// A pass with 4+ targets and a depth buffer whose formats are not the G-buffer's goes to the self-check.
bool IsGBuffer(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (n < 4 || n > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT || !rtvs || !dsv) return false;
    const DXGI_FORMAT want[4] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UINT};
    Signature sig = {};
    bool match = true;
    for (int i = 0; i < 4; ++i) {
        if (!rtvs[i]) return false;
        D3D11_RENDER_TARGET_VIEW_DESC d = {};
        rtvs[i]->GetDesc(&d);
        sig.f[i] = d.Format;
        match = match && d.Format == want[i];
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
    dsv->GetDesc(&dd);
    sig.f[4] = dd.Format;
    match = match && (dd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || dd.Format == DXGI_FORMAT_D24_UNORM_S8_UINT);
    if (!match) ReportMismatch(sig);
    return match;
}

void ReleasePass() {
    for (IUnknown* u : {static_cast<IUnknown*>(g_pass.albedo), static_cast<IUnknown*>(g_pass.spec),
                        static_cast<IUnknown*>(g_pass.rt0), static_cast<IUnknown*>(g_pass.rt3),
                        static_cast<IUnknown*>(g_pass.dsv)})
        if (u) u->Release();
    g_pass = Pass();
}

// A shader view of rt0's texture (normal + camera depth), kept while the texture stays the same.
ID3D11ShaderResourceView* Rt0View() {
    if (!g_pass.rt0) return nullptr;
    ID3D11Resource* res = nullptr;
    g_pass.rt0->GetResource(&res);
    if (!res) return nullptr;
    if (res == g_rt0_res) {
        res->Release();
        return g_rt0_srv;
    }
    if (g_rt0_srv) g_rt0_srv->Release();
    if (g_rt0_res) g_rt0_res->Release();
    g_rt0_srv = nullptr;
    g_rt0_res = res;  // keeps the reference
    ID3D11Texture2D* tex = nullptr;
    D3D11_TEXTURE2D_DESC td = {};
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
        tex->GetDesc(&td);
        tex->Release();
    }
    if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE) || td.SampleDesc.Count != 1 || td.ArraySize != 1) {
        if (!g_rt0_failed) Error("dust: rt0 cannot be read by a shader (bind flags / multisampling): no dust");
        g_rt0_failed = true;
        return nullptr;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(g_dev->CreateShaderResourceView(res, &sd, &g_rt0_srv))) {
        g_rt0_srv = nullptr;
        if (!g_rt0_failed) Error("dust: no shader view of rt0: no dust");
        g_rt0_failed = true;
    }
    return g_rt0_srv;
}

// N25: rt3 copied into g_rt3_copy (made or remade to rt3's size and format) and a shader view of the copy;
// nullptr when it cannot be done (the dust is drawn without touching the reflection).
void CopyRegion(ID3D11DeviceContext* c, ID3D11Texture2D* dst, ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& td,
                const D3D11_BOX* box) {
    if (!box) return c->CopyResource(dst, src);
    D3D11_BOX b = *box;
    b.right = (std::min)(b.right, td.Width), b.bottom = (std::min)(b.bottom, td.Height);
    if (b.left >= b.right || b.top >= b.bottom) return;
    c->CopySubresourceRegion(dst, 0, b.left, b.top, 0, src, 0, &b);
}

ID3D11ShaderResourceView* Rt3Copy(ID3D11DeviceContext* c, const D3D11_BOX* box) {
    if (!g_pass.rt3 || g_rt3_failed) return nullptr;
    ID3D11Resource* res = nullptr;
    g_pass.rt3->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    if (res) res->Release();
    if (!tex) return nullptr;
    D3D11_TEXTURE2D_DESC td = {};
    tex->GetDesc(&td);
    if (!g_rt3_copy || td.Width != g_rt3_desc.Width || td.Height != g_rt3_desc.Height || td.Format != g_rt3_desc.Format ||
        td.MipLevels != g_rt3_desc.MipLevels || td.ArraySize != g_rt3_desc.ArraySize) {
        if (g_rt3_srv) g_rt3_srv->Release();
        if (g_rt3_copy) g_rt3_copy->Release();
        g_rt3_srv = nullptr, g_rt3_copy = nullptr;
        g_rt3_desc = td;
        D3D11_TEXTURE2D_DESC cd = td;
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cd.CPUAccessFlags = 0;
        cd.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R16G16B16A16_UINT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (td.SampleDesc.Count != 1 || td.ArraySize != 1 || td.MipLevels != 1 ||
            FAILED(g_dev->CreateTexture2D(&cd, nullptr, &g_rt3_copy)) ||
            FAILED(g_dev->CreateShaderResourceView(g_rt3_copy, &sd, &g_rt3_srv))) {
            if (g_rt3_copy) g_rt3_copy->Release();
            g_rt3_copy = nullptr, g_rt3_srv = nullptr;
            g_rt3_failed = true;
            Error("dust: no copy of rt3 (multisampling, mips or format): the paint's reflection stays over the dust");
            tex->Release();
            return nullptr;
        }
    }
    CopyRegion(c, g_rt3_copy, tex, td, box);  // D13d: the passes' rectangles only
    tex->Release();
    g_rt3_copies.fetch_add(1, std::memory_order_relaxed);
    return g_rt3_srv;
}

// D11m: the pack's target at rt0's size (the depth-stencil's), made or made again; false: no dirt this pass.
bool PackTarget() {
    if (!g_pass.rt0 || g_pack_failed) return false;
    ID3D11Resource* res = nullptr;
    g_pass.rt0->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    if (res) res->Release();
    if (!tex) return false;
    D3D11_TEXTURE2D_DESC td = {};
    tex->GetDesc(&td);
    tex->Release();
    if (g_pack && td.Width == g_pack_w && td.Height == g_pack_h) return true;
    for (IUnknown* u : {static_cast<IUnknown*>(g_pack_srv), static_cast<IUnknown*>(g_pack_rtv),
                        static_cast<IUnknown*>(g_pack)})
        if (u) u->Release();
    g_pack = nullptr, g_pack_rtv = nullptr, g_pack_srv = nullptr;
    D3D11_TEXTURE2D_DESC pd = {};
    pd.Width = td.Width, pd.Height = td.Height, pd.MipLevels = pd.ArraySize = 1;
    pd.Format = DXGI_FORMAT_R32G32B32A32_UINT, pd.SampleDesc.Count = 1, pd.Usage = D3D11_USAGE_DEFAULT;
    pd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (td.SampleDesc.Count != 1 || FAILED(g_dev->CreateTexture2D(&pd, nullptr, &g_pack)) ||
        FAILED(g_dev->CreateRenderTargetView(g_pack, nullptr, &g_pack_rtv)) ||
        FAILED(g_dev->CreateShaderResourceView(g_pack, nullptr, &g_pack_srv))) {
        for (IUnknown* u : {static_cast<IUnknown*>(g_pack_srv), static_cast<IUnknown*>(g_pack_rtv),
                            static_cast<IUnknown*>(g_pack)})
            if (u) u->Release();
        g_pack = nullptr, g_pack_rtv = nullptr, g_pack_srv = nullptr;
        g_pack_failed = true;
        Error("dust: no target for the dirt's pack (D11m): no dust");
        return false;
    }
    g_pack_w = td.Width, g_pack_h = td.Height;
    return true;
}

// 3x4 inverse of a 3x4 affine matrix (row-major, translation in column 3).
bool Invert(const float m[12], float out[12]) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1e-12f)) return false;
    const float k = 1.f / det;
    const float r[9] = {A * k, -(b * i - c * h) * k, (b * f - c * e) * k,
                        B * k, (a * i - c * g) * k,  -(a * f - c * d) * k,
                        C * k, -(a * h - b * g) * k, (a * e - b * d) * k};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) out[row * 4 + col] = r[row * 3 + col];
        out[row * 4 + 3] = -(r[row * 3] * m[3] + r[row * 3 + 1] * m[7] + r[row * 3 + 2] * m[11]);
    }
    return true;
}

// The view's projection from one draw: cb0 rows 0-2 (model -> camera) and
// rows 4, 5, 7 (model -> clip x, y, w) [verified: V3a captures, row 7 = -row 2].
bool Projection(const float m[32], float out[3][4]) {
    const float* w = m + 28;
    for (int k = 0; k < 4; ++k)
        if (std::fabs(w[k] + m[8 + k]) > 1e-3f * (1.f + std::fabs(w[k]))) return false;  // not w = -z
    float inv[12];
    if (!Invert(m, inv)) return false;
    const float* rows[3] = {m + 16, m + 20, m + 28};
    for (int r = 0; r < 3; ++r) {
        const float* c = rows[r];
        float* o = out[r];
        for (int j = 0; j < 3; ++j) o[j] = c[0] * inv[j] + c[1] * inv[4 + j] + c[2] * inv[8 + j];
        o[3] = c[0] * inv[3] + c[1] * inv[7] + c[2] * inv[11] + c[3];
    }
    return true;
}

// The game's state the dust pass changes, saved before the first change (references held).
struct SavedState {
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView* dsv;
    // D10d: the pixel shader's UAVs share the output slots with the targets: OMSetRenderTargets unbinds them, so
    // they are kept and bound again.
    ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT];
    ID3D11BlendState* bs;
    float factor[4];
    UINT sample_mask;
    ID3D11DepthStencilState* dss;
    UINT ref;
    ID3D11RasterizerState* rs;
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nvp;
    D3D11_RECT scs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];  // D13d
    UINT nsc;
    ID3D11InputLayout* il;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11GeometryShader* gs;
    ID3D11HullShader* hs;
    ID3D11DomainShader* ds;
    ID3D11ShaderResourceView* srv0;  // pixel shader slot 0 (view and constants)
    ID3D11ShaderResourceView* srv1;  // N25: slot 1 (the copy of rt3)
    ID3D11ShaderResourceView* srv2;  // D11m: slot 2 (the pack)
    ID3D11DeviceContext1* c1;
    ID3D11Buffer* cb0;
    UINT cb0_first, cb0_num;  // the constant range, when the game set one
};

void SaveState(ID3D11DeviceContext* c, SavedState* s) {
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s->rtvs, &s->dsv);
    c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_PS_CS_UAV_REGISTER_COUNT, s->uavs);
    c->OMGetBlendState(&s->bs, s->factor, &s->sample_mask);
    c->OMGetDepthStencilState(&s->dss, &s->ref);
    c->RSGetState(&s->rs);
    s->nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetViewports(&s->nvp, s->vps);
    s->nsc = 0;
    c->RSGetScissorRects(&s->nsc, nullptr);  // the count bound, then the rectangles
    s->nsc = (std::min)(s->nsc, UINT(D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE));
    if (s->nsc) c->RSGetScissorRects(&s->nsc, s->scs);
    c->IAGetInputLayout(&s->il);
    c->IAGetPrimitiveTopology(&s->topo);
    c->VSGetShader(&s->vs, nullptr, nullptr);
    c->PSGetShader(&s->ps, nullptr, nullptr);
    c->GSGetShader(&s->gs, nullptr, nullptr);
    c->HSGetShader(&s->hs, nullptr, nullptr);
    c->DSGetShader(&s->ds, nullptr, nullptr);
    c->PSGetShaderResources(0, 1, &s->srv0);
    c->PSGetShaderResources(1, 1, &s->srv1);
    c->PSGetShaderResources(2, 1, &s->srv2);
    c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&s->c1));
    if (s->c1) s->c1->PSGetConstantBuffers1(0, 1, &s->cb0, &s->cb0_first, &s->cb0_num);
    else c->PSGetConstantBuffers(0, 1, &s->cb0);
}

// Sets the saved state again and drops its references.
void RestoreState(ID3D11DeviceContext* c, SavedState* s) {
    // The pixel shader slot first: rt0 becomes a target again below.
    c->PSSetShaderResources(0, 1, &s->srv0);
    c->PSSetShaderResources(1, 1, &s->srv1);
    c->PSSetShaderResources(2, 1, &s->srv2);
    // num 0: bound without a range (or nothing bound); setting it back with a
    // range would bind an empty window [N3 B6: dustpreview prints what Get1 returns].
    if (s->c1 && s->cb0_num) s->c1->PSSetConstantBuffers1(0, 1, &s->cb0, &s->cb0_first, &s->cb0_num);
    else c->PSSetConstantBuffers(0, 1, &s->cb0);
    // The targets up to the last one bound, then the UAVs from the first one bound (after the targets).
    UINT targets = 0, first_uav = D3D11_PS_CS_UAV_REGISTER_COUNT;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (s->rtvs[i]) targets = i + 1;
    for (UINT i = D3D11_PS_CS_UAV_REGISTER_COUNT; i-- > 0;)
        if (s->uavs[i]) first_uav = i;
    if (first_uav < D3D11_PS_CS_UAV_REGISTER_COUNT && first_uav >= targets) {
        UINT keep_counts[D3D11_PS_CS_UAV_REGISTER_COUNT];
        for (UINT& k : keep_counts) k = UINT(-1);  // the hidden counters as they are
        c->OMSetRenderTargetsAndUnorderedAccessViews(targets, s->rtvs, s->dsv, first_uav,
                                                      D3D11_PS_CS_UAV_REGISTER_COUNT - first_uav, s->uavs + first_uav,
                                                      keep_counts);
    } else {
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s->rtvs, s->dsv);
    }
    c->OMSetBlendState(s->bs, s->factor, s->sample_mask);
    c->OMSetDepthStencilState(s->dss, s->ref);
    c->RSSetState(s->rs);
    if (s->nvp) c->RSSetViewports(s->nvp, s->vps);
    c->RSSetScissorRects(s->nsc, s->nsc ? s->scs : nullptr);
    c->IASetInputLayout(s->il);
    c->IASetPrimitiveTopology(s->topo);
    c->VSSetShader(s->vs, nullptr, 0);
    c->PSSetShader(s->ps, nullptr, 0);
    c->GSSetShader(s->gs, nullptr, 0);
    c->HSSetShader(s->hs, nullptr, 0);
    c->DSSetShader(s->ds, nullptr, 0);
    for (auto* v : s->rtvs)
        if (v) v->Release();
    for (auto* v : s->uavs)
        if (v) v->Release();
    for (IUnknown* u : {static_cast<IUnknown*>(s->dsv), static_cast<IUnknown*>(s->bs), static_cast<IUnknown*>(s->dss),
                        static_cast<IUnknown*>(s->rs), static_cast<IUnknown*>(s->il), static_cast<IUnknown*>(s->vs),
                        static_cast<IUnknown*>(s->ps), static_cast<IUnknown*>(s->gs), static_cast<IUnknown*>(s->hs),
                        static_cast<IUnknown*>(s->ds), static_cast<IUnknown*>(s->srv0), static_cast<IUnknown*>(s->srv1),
                        static_cast<IUnknown*>(s->srv2),
                        static_cast<IUnknown*>(s->cb0),
                        static_cast<IUnknown*>(s->c1)})
        if (u) u->Release();
    *s = SavedState();
}

// ---- D13d: the pass inside the units' rectangles on screen ----
// The user, ATS 30/10: drops to 50 FPS on a bridge looking at the right mirrors. Every class's 4-5 draws covered
// the whole viewport (stencil-tested) and rt0 and rt3 were copied whole every pass (3440x2880 there, ~80 MB each).
// Now a class draws inside the screen rectangle of its unit's box, widened by kRectPad (the meshes reach past
// their origins' box: the bumper, the mirrors, a load), the copies take the union of the rectangles, and one draw
// without a pixel shader clears the class's bit in the whole viewport: no mark is left behind even when a part
// reaches out of its rectangle (it would only miss its dust there).
constexpr float kRectPad[3][2] = {{1.0f, 1.0f}, {0.6f, 1.2f}, {2.5f, 2.5f}};  // m: x, y, z (below, above the box)
constexpr float kRectNear = 0.05f;   // camera space: in front at z < -kRectNear
constexpr double kRectWhole = 0.9;   // a rectangle over 90% of the viewport: the whole one (no clearing draw)

// The rectangle (target pixels) of the unit's widened box in this pass's view; false when not known (no frame or
// projection, nothing in front of the camera, off the viewport while its draws were marked).
bool UnitRect(int unit, D3D11_RECT* out) {
    float f[12], lo[3], hi[3];
    if (!g_pass.has_proj || !rig::GetFrame(g_pass.view, unit, f, false) || !rig::GetBox(unit, lo, hi)) return false;
    for (int i = 0; i < 3; ++i) lo[i] -= kRectPad[i][0], hi[i] += kRectPad[i][1];
    float cam[8][3];
    for (int k = 0; k < 8; ++k) {
        const float p[3] = {k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2]};
        for (int r = 0; r < 3; ++r) cam[k][r] = f[r * 4] * p[0] + f[r * 4 + 1] * p[1] + f[r * 4 + 2] * p[2] + f[r * 4 + 3];
    }
    // The corners in front of the camera and where the box's edges cross the near plane (the eye inside the box).
    float pts[8 + 12][3];
    int n = 0;
    for (int k = 0; k < 8; ++k) {
        const float za = cam[k][2] + kRectNear;
        if (za < 0) memcpy(pts[n++], cam[k], sizeof(cam[k]));
        for (int axis = 0; axis < 3; ++axis) {
            const int j = k | (1 << axis);
            if (j == k) continue;
            const float zb = cam[j][2] + kRectNear;
            if ((za < 0) == (zb < 0)) continue;
            const float t = za / (za - zb);
            for (int r = 0; r < 3; ++r) pts[n][r] = cam[k][r] + t * (cam[j][r] - cam[k][r]);
            ++n;
        }
    }
    if (!n) return false;
    const D3D11_VIEWPORT& vp = g_pass.vp;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < n; ++i) {
        float clip[3];
        for (int r = 0; r < 3; ++r)
            clip[r] = g_pass.proj[r][0] * pts[i][0] + g_pass.proj[r][1] * pts[i][1] + g_pass.proj[r][2] * pts[i][2] +
                      g_pass.proj[r][3];
        if (!(clip[2] > 1e-6f)) return false;
        const float sx = vp.TopLeftX + (clip[0] / clip[2] * 0.5f + 0.5f) * vp.Width;
        const float sy = vp.TopLeftY + (0.5f - clip[1] / clip[2] * 0.5f) * vp.Height;
        x0 = (std::min)(x0, sx), x1 = (std::max)(x1, sx), y0 = (std::min)(y0, sy), y1 = (std::max)(y1, sy);
    }
    // 4 pixels more, inside the viewport (NaN fails the test below).
    const float l = (std::max)(x0 - 4.f, vp.TopLeftX), t = (std::max)(y0 - 4.f, vp.TopLeftY);
    const float r = (std::min)(x1 + 4.f, vp.TopLeftX + vp.Width), b = (std::min)(y1 + 4.f, vp.TopLeftY + vp.Height);
    if (!(r > l && b > t)) return false;
    *out = {LONG(std::floor(l)), LONG(std::floor(t)), LONG(std::ceil(r)), LONG(std::ceil(b))};
    return true;
}

double RectArea(const D3D11_RECT& r) { return double(r.right - r.left) * double(r.bottom - r.top); }

// The full-screen pass over the marked pixels, one draw per unit. The game's
// state goes into `saved` first (*saved_ok); the caller restores it.
void RunPass(ID3D11DeviceContext* c, SavedState* saved, bool* saved_ok) {
    if (!MakeObjects(c) || !PackTarget()) return;
    SaveState(c, saved);
    *saved_ok = true;

    const bool red = g_mode == 1;
    const bool position_test = g_mode == 3;
    const bool diagnosis = g_mode == 4;  // N26
    ID3D11ShaderResourceView* view = red ? nullptr : Rt0View();
    const bool spec_draw = !red && !position_test && !diagnosis && g_pass.spec;
    // D13d: each marked class's rectangle (the dust mode only), and their union for the copies.
    const D3D11_VIEWPORT& pvp = g_pass.vp;
    const D3D11_RECT whole = {LONG(pvp.TopLeftX), LONG(pvp.TopLeftY), LONG(pvp.TopLeftX + pvp.Width),
                              LONG(pvp.TopLeftY + pvp.Height)};
    D3D11_RECT rect[kClassCount];
    bool part[kClassCount] = {};  // drawn in its rectangle, its bit cleared after by a draw over the viewport
    D3D11_RECT all = {LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
    for (int k = 0; k < kClassCount; ++k) {
        if (!g_pass.marked_class[k]) continue;
        rect[k] = whole;
        if (g_mode == 2 && g_pass_rs_scissor && g_ps_clear && !g_rects_off.load(std::memory_order_relaxed)) {
            if (UnitRect(kClasses[k].unit, &rect[k])) part[k] = RectArea(rect[k]) < kRectWhole * RectArea(whole);
            else ++g_rect_unknown;
        }
        if (!part[k]) rect[k] = whole;
        g_rect_parts += part[k];
        g_rects_used = g_rects_used || part[k];
        if (part[k]) g_rect_class_cover += RectArea(rect[k]) / RectArea(whole);
        all.left = (std::min)(all.left, rect[k].left), all.top = (std::min)(all.top, rect[k].top);
        all.right = (std::max)(all.right, rect[k].right), all.bottom = (std::max)(all.bottom, rect[k].bottom);
    }
    if (all.left >= all.right) all = whole;
    all.left = (std::max)(all.left, 0L), all.top = (std::max)(all.top, 0L);
    const D3D11_BOX box = {UINT(all.left), UINT(all.top), 0, UINT((std::max)(all.right, all.left)),
                           UINT((std::max)(all.bottom, all.top)), 1};
    if (RectArea(whole) > 0) g_rect_cover += RectArea(all) / RectArea(whole), ++g_rect_passes;
    c->OMSetRenderTargets(1, &g_pass.albedo, g_pass.dsv);  // rt0 is no longer a target: it can be read
    // N25: in the dust mode, rt3 too: copied first (no longer a target), before any of our draws.
    ID3D11PixelShader* const ps_refl = g_ps_refl.load();
    ID3D11ShaderResourceView* rt3_copy = (g_mode == 2 || diagnosis) && view && ps_refl ? Rt3Copy(c, &box) : nullptr;
    // D11k: the relief last, in the dust mode, into rt0 from a copy of it (taken before any of our draws).
    // D13f: rt0 is not copied any more (the NORM draw takes the tilted normal from the pack).
    const bool relief = g_mode == 2 && view && g_ps_norm && g_relief.load(std::memory_order_relaxed);
    c->OMSetBlendState(g_pass_bs, nullptr, 0xFFFFFFFF);
    c->RSSetState(g_pass_rs);
    c->RSSetViewports(1, &g_pass.vp);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g_vs, nullptr, 0);
    c->PSSetShader(g_ps, nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    c->PSSetShaderResources(0, 1, &view);
    c->PSSetConstantBuffers(0, 1, &g_pass_cb);
    for (int k = 0; k < kClassCount; ++k) {
        if (!g_pass.marked_class[k]) continue;
        const int u = kClasses[k].unit;
        c->RSSetState(part[k] ? g_pass_rs_scissor : g_pass_rs);  // D13d
        if (part[k]) c->RSSetScissorRects(1, &rect[k]);
        // Every marked unit gets its draw: with nothing to show (alpha 0) it still clears its bit.
        PassCb cb = {};
        cb.vp[0] = g_pass.vp.TopLeftX, cb.vp[1] = g_pass.vp.TopLeftY;
        cb.vp[2] = g_pass.vp.Width > 0 ? 1.f / g_pass.vp.Width : 0.f;
        cb.vp[3] = g_pass.vp.Height > 0 ? 1.f / g_pass.vp.Height : 0.f;
        // N13: clamped to 0..1 and scaled (level 1 = the look 0.8 had before N13).
        dust::Layers layers;
        float* const slots[3] = {layers.body, layers.front, layers.reach};
        const bool cargo = kClasses[k].cargo;  // N57
        for (int r = 0; r < 3; ++r)
            for (int s = 0; s < dust::kSlots; ++s)
                slots[r][s] = (cargo ? g_cargo_layers : g_layers)[u][r][s].load(std::memory_order_relaxed);
        dust::Levels(cb, (cargo ? g_cargo_dust : g_dust)[u].load(std::memory_order_relaxed), layers);
        cb.params[1] = red ? 1.f : position_test ? 2.f : diagnosis ? 3.f : 0.f;
        float frame[12], inv[12];
        const bool have_frame = rig::GetFrame(g_pass.view, u, frame, kClasses[k].cab) && Invert(frame, inv);
        if (!red && !have_frame) g_no_frame.fetch_add(1, std::memory_order_relaxed);
        if (!red && !g_pass.has_proj) g_no_proj.fetch_add(1, std::memory_order_relaxed);
        if (have_frame && g_pass.has_proj && view) {
            memcpy(cb.proj, g_pass.proj, sizeof(cb.proj));
            memcpy(cb.unit, inv, sizeof(cb.unit));
            cb.params[2] = static_cast<float>(rig::GetWheels(u, cb.wheel));
            rig::GetWheelReach(u, cb.wheel_out);     // N20
            rig::GetWheelRadius(u, cb.wheel_radius);  // N22
            float steer[dust::kWheels], roll[dust::kWheels];
            for (int w = 0; w < dust::kWheels; ++w) {
                steer[w] = g_steer[u][w].load(std::memory_order_relaxed);
                roll[w] = g_roll[u][w].load(std::memory_order_relaxed);
            }
            dust::Wheels(cb, steer, roll, dust::kWheels, g_speed[u].load(std::memory_order_relaxed));
            // N16: the cab's pixels are never a wheel's: a fender over the tyre keeps the body's dirt, fixed to the cab,
            // instead of the wheel's pattern at a place that moves as the cab dives [estimativa: D4e test].
            cb.spin[1] = kClasses[k].cab ? 1.f : 0.f;
            cb.spin[2] = u > 0 ? 1.f : 0.f;  // N31: a trailer's sides take the side film
            cb.params[3] = 1.f;
            if (u == 0) {  // N41: the truck's passes (chassis and cab: the interior's parts are either)
                std::lock_guard<std::mutex> lock(g_inside_mutex);
                // D14d: the boxes stay (N55's learned parts keep their dust: see rig::Classify). P1: not once the
                // pre-map found the truck's interior pieces (each gets no dust itself: MeshPrint).
                const bool premapped = g_mp_int_ext.load(std::memory_order_relaxed) > 0;
                cb.inside_lo[3] = g_inside_on && !premapped ? 1.f : 0.f;
                for (int i = 0; i < 3; ++i) cb.inside_lo[i] = g_inside_lo[i], cb.inside_hi[i] = g_inside_hi[i];
                cb.cavity_lo[3] = g_cavity_on && !premapped ? 1.f : 0.f;
                for (int i = 0; i < 3; ++i) cb.cavity_lo[i] = g_cavity_lo[i], cb.cavity_hi[i] = g_cavity_hi[i];
                // N55 (the user, ATS 31/10, the VNL 2025: the box came out of the cab): never past the cab's sides, the
                // front wheels' track (the eye sits off the middle: the box around it reached past the driver's door).
                float wh[rig::kMaxWheels][4];
                const int nw = rig::GetWheels(0, wh);
                float front = 1e9f, side = 0.f;
                for (int w = 0; w < nw; ++w) front = (std::min)(front, wh[w][2]);
                for (int w = 0; w < nw; ++w)
                    if (wh[w][2] < front + 0.3f) side = (std::max)(side, std::fabs(wh[w][0]));
                if (side > 0.5f)
                    for (float* lo : {cb.inside_lo, cb.cavity_lo}) lo[0] = (std::max)(lo[0], -side);
                if (side > 0.5f)
                    for (float* hi : {cb.inside_hi, cb.cavity_hi}) hi[0] = (std::min)(hi[0], side);
                // D11i: the cavity reaches the windshield (the dashboard's top behind it), 3 cm behind the glass.
                if (g_ws_front < 1e8f) cb.cavity_lo[2] = std::min(cb.cavity_lo[2], g_ws_front + 0.03f);
                // D11j: the cab's front behind the windshield's plane, ahead of the eye.
                cb.dash1[3] = g_ws_front < 1e8f && g_cavity_on && !premapped ? 1.f : 0.f;
                cb.dash0[0] = g_ws_front + 0.03f, cb.dash0[1] = g_ws_base_y, cb.dash0[2] = g_ws_slope;
                cb.dash0[3] = g_ws_top;
                cb.dash1[0] = 0.9f, cb.dash1[1] = g_eye[1] - 1.0f, cb.dash1[2] = g_eye[2];
            }
        }
        cb.spin[3] = relief ? 1.f : 0.f;  // D11m: the pack works the relief out for the NORM draw
        c->UpdateSubresource(g_pass_cb, 0, nullptr, &cb, 0, 0);
        // D11m: the dirt into the pack first (the class's only full shading), then the draws that take it.
        ID3D11ShaderResourceView* no_pack = nullptr;
        c->PSSetShaderResources(2, 1, &no_pack);  // the last class's draws read it: a target now
        c->OMSetRenderTargets(1, &g_pack_rtv, g_pass.dsv);
        c->PSSetShaderResources(0, 1, &view);  // D11k: the relief draw below had rt0 as its target
        c->PSSetShader(g_ps_pack, nullptr, 0);
        c->OMSetBlendState(g_refl_bs, nullptr, 0xFFFFFFFF);
        c->OMSetDepthStencilState(g_pass_dss_keep, kClasses[k].ref);
        c->Draw(3, 0);
        c->OMSetBlendState(g_pass_bs, nullptr, 0xFFFFFFFF);
        c->OMSetRenderTargets(1, &g_pass.albedo, g_pass.dsv);
        c->PSSetShaderResources(2, 1, &g_pack_srv);
        c->PSSetShader(g_ps, nullptr, 0);
        // The last draw of the class clears its stencil bit; the ones before keep it.
        c->OMSetDepthStencilState(spec_draw || rt3_copy || relief || part[k] ? g_pass_dss_keep : g_pass_dss,
                                  kClasses[k].ref);
        c->Draw(3, 0);
        if (spec_draw) {
            c->OMSetRenderTargets(1, &g_pass.spec, g_pass.dsv);
            c->PSSetShader(g_ps_spec, nullptr, 0);
            c->OMSetDepthStencilState(rt3_copy || relief || part[k] ? g_pass_dss_keep : g_pass_dss, kClasses[k].ref);
            c->Draw(3, 0);
        }
        if (rt3_copy) {  // N25
            ID3D11ShaderResourceView* views[2] = {view, rt3_copy};
            c->OMSetRenderTargets(1, &g_pass.rt3, g_pass.dsv);
            c->PSSetShaderResources(0, 2, views);
            c->PSSetShader(ps_refl, nullptr, 0);
            c->OMSetBlendState(g_refl_bs, nullptr, 0xFFFFFFFF);
            c->OMSetDepthStencilState(relief || part[k] ? g_pass_dss_keep : g_pass_dss, kClasses[k].ref);
            c->Draw(3, 0);
            c->OMSetBlendState(g_pass_bs, nullptr, 0xFFFFFFFF);
        }
        if (relief) {  // D11k: rt0's relief. D13f: the pack's tilted normals through the albedo's blend (rt0 not read)
            ID3D11ShaderResourceView* views[2] = {nullptr, nullptr};
            c->PSSetShaderResources(0, 2, views);  // rt0's view off first (rt0 becomes the target); t2: the pack
            c->OMSetRenderTargets(1, &g_pass.rt0, g_pass.dsv);
            c->PSSetShader(g_ps_norm, nullptr, 0);
            c->OMSetBlendState(g_pass_bs, nullptr, 0xFFFFFFFF);
            c->OMSetDepthStencilState(part[k] ? g_pass_dss_keep : g_pass_dss, kClasses[k].ref);
            c->Draw(3, 0);
        }
        if (part[k]) {  // D13d: the class's bit cleared over the whole viewport
            // D13e: into the pack (read again only after the next class's pack draw writes it, in its own rectangle).
            c->RSSetState(g_pass_rs);
            c->PSSetShaderResources(2, 1, &no_pack);
            c->OMSetRenderTargets(1, &g_pack_rtv, g_pass.dsv);
            c->PSSetShader(g_ps_clear, nullptr, 0);
            c->OMSetBlendState(g_refl_bs, nullptr, 0xFFFFFFFF);
            c->OMSetDepthStencilState(g_pass_dss, kClasses[k].ref);
            c->Draw(3, 0);
            c->OMSetBlendState(g_pass_bs, nullptr, 0xFFFFFFFF);
        }
        g_unit_draws.fetch_add(1, std::memory_order_relaxed);
    }
    g_passes.fetch_add(1, std::memory_order_relaxed);
}

// Ends the pass. The __finally also runs when an exception leaves RunPass (the
// tint guard in gpu_probe catches it): the game's state comes back, the pass's
// references are dropped and Busy() turns false on every path.
void EndPass(ID3D11DeviceContext* c) {
    SavedState saved = {};
    bool saved_ok = false;
    g_busy = true;
    __try {
        if (g_pass.open && g_pass.marked > 0) {
            GpuTimer* const timer = TimerBegin(c);  // D13d
            RunPass(c, &saved, &saved_ok);
            TimerEnd(c, timer);
        }
    } __finally {
        if (saved_ok) RestoreState(c, &saved);
        c->OMSetDepthStencilState(g_dss, g_ref);  // the game's own state again
        ReleasePass();
        g_busy = false;
    }
}

// The open pass's version of the game's state: writes bits 0xf0 (the unit's
// code for a player draw, the cab's, a cargo's; unit < 0: all cleared).
void SetPassState(ID3D11DeviceContext* c, int unit, bool cab = false, bool cargo = false) {
    ID3D11DepthStencilState* v = MakeObjects(c) ? Variant(g_dss) : nullptr;
    g_busy = true;
    const UINT bits = unit >= 0 ? UINT(cab && unit == 0 ? kCabCode : kUnitCode[unit]) | (cargo ? kCargoBit : 0u) : 0u;
    if (v) c->OMSetDepthStencilState(v, (g_ref & ~UINT(kBits)) | bits);
    else c->OMSetDepthStencilState(g_dss, g_ref);
    g_busy = false;
}

// Plain body for __try: copies `bytes` bytes, false on an access fault.
// Rows 0-2 of r: each the length of a rotation's row (a mirror or a small scale allowed).
bool RotationRows(const float* r) {
    for (int row = 0; row < 3; ++row) {
        const float n2 = r[row * 4] * r[row * 4] + r[row * 4 + 1] * r[row * 4 + 1] + r[row * 4 + 2] * r[row * 4 + 2];
        if (!(n2 > 0.25f && n2 < 4.f)) return false;
    }
    return true;
}

// N29: rows 0-2 of r a model -> camera matrix for sure: rotation rows at right angles to each other, then the row
// (0 0 0 1), as every matrix the game passes is followed [verified: D4r ATS capture, all VS cb0 layouts seen].
bool MatrixAt(const float* r) {
    if (!RotationRows(r)) return false;
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            const float d = r[a * 4] * r[b * 4] + r[a * 4 + 1] * r[b * 4 + 1] + r[a * 4 + 2] * r[b * 4 + 2];
            if (!(std::fabs(d) < 0.05f)) return false;
        }
    const float* w = r + 12;
    return std::fabs(w[0]) < 1e-4f && std::fabs(w[1]) < 1e-4f && std::fabs(w[2]) < 1e-4f && std::fabs(w[3] - 1.f) < 1e-4f;
}

// D13d: the game's constant buffers are dynamic, mapped for writing: write-combined memory, where plain loads go
// out uncached one by one [estimativa: the D3D11 drivers' usual mapping]; MOVNTDQA (SSE4.1) takes a whole 64-byte
// line per read into a streaming buffer. On cached memory it is a plain load.
const bool g_stream_loads = [] {
    int r[4] = {};
    __cpuid(r, 1);
    return (r[2] & (1 << 19)) != 0;
}();

bool ReadRows(const uint8_t* src, float* rows, size_t bytes) {
    __try {
        if (g_stream_loads && !(reinterpret_cast<uintptr_t>(src) & 15) && !(bytes & 15)) {
            for (size_t i = 0; i < bytes; i += 16)
                _mm_storeu_si128(reinterpret_cast<__m128i*>(reinterpret_cast<uint8_t*>(rows) + i),
                                 _mm_stream_load_si128((__m128i*)(src + i)));
            return true;
        }
        memcpy(rows, src, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void Configure(int mode, bool compat_enforce) {
    g_mode = mode >= 1 && mode <= 4 ? mode : 0;
    g_on = g_mode != 0;
    if (g_on) StartCompile();  // D11e: ready before the first G-buffer pass, mostly
    std::lock_guard<std::mutex> lock(g_compat_mutex);
    g_compat.Configure(compat_enforce);
}
bool On() { return g_on; }
void SetCabInside(bool known, const float eye[3], const float box[5], const float cavity[5]) {
    std::lock_guard<std::mutex> lock(g_inside_mutex);
    auto set = [&](const float* b, bool* on, float* lo, float* hi) {
        *on = known && b[0] > 0.f;
        if (!*on) return;
        lo[0] = -b[0], hi[0] = b[0];
        lo[1] = eye[1] - b[1], hi[1] = eye[1] + b[2];
        lo[2] = eye[2] - b[3], hi[2] = eye[2] + b[4];  // +z = rear
    };
    set(box, &g_inside_on, g_inside_lo, g_inside_hi);
    set(cavity, &g_cavity_on, g_cavity_lo, g_cavity_hi);
    g_eye_known = known;  // D11i: a new truck config: the windshield measured again
    g_mp_reset.store(true);  // P1: and its interior found again
    memcpy(g_eye, eye, sizeof(g_eye));
    g_ws_front = 1e9f;
    g_ws_reset.store(true);
}
void SetRelief(bool on) { g_relief.store(on); }
void SetDust(int unit, float level) {
    if (unit >= 0 && unit < rig::kUnits) g_dust[unit].store(level, std::memory_order_relaxed);
}
void SetLayers(int unit, const dust::Layers& layers) {
    if (unit < 0 || unit >= rig::kUnits) return;
    const float* const slots[3] = {layers.body, layers.front, layers.reach};
    for (int r = 0; r < 3; ++r)
        for (int s = 0; s < dust::kSlots; ++s) g_layers[unit][r][s].store(slots[r][s], std::memory_order_relaxed);
}
void SetCargo(int unit, float level, const dust::Layers& layers) {
    if (unit < 0 || unit >= rig::kUnits) return;
    g_cargo_dust[unit].store(level, std::memory_order_relaxed);
    const float* const slots[3] = {layers.body, layers.front, layers.reach};
    for (int r = 0; r < 3; ++r)
        for (int s = 0; s < dust::kSlots; ++s) g_cargo_layers[unit][r][s].store(slots[r][s], std::memory_order_relaxed);
}
bool WheelsOnly(int unit) { return unit >= 0 && unit < rig::kUnits && rig::GetStats().body[unit] == 0; }
void SetWheels(int unit, const float* steering, const float* rotation, int n, float speed_kmh) {
    if (unit < 0 || unit >= rig::kUnits) return;
    for (int w = 0; w < dust::kWheels; ++w) {
        g_steer[unit][w].store(w < n ? steering[w] : 0.f, std::memory_order_relaxed);
        g_roll[unit][w].store(w < n ? rotation[w] : 0.f, std::memory_order_relaxed);
    }
    g_speed[unit].store(speed_kmh, std::memory_order_relaxed);
}
bool RectsFallback() {
    if (!g_rects_used || g_rects_off.exchange(true)) return false;
    return true;
}

bool Busy() { return g_busy; }
void SetBusy(bool busy) { g_busy = busy; }

void Abandon(ID3D11DeviceContext* c) {
    g_busy = true;
    if (c && g_pass.open) c->OMSetDepthStencilState(g_dss, g_ref);
    ReleasePass();
    g_busy = false;
}

void OnSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!g_on) return;
    const bool gbuffer = IsGBuffer(n, rtvs, dsv);
    if (g_pass.open) {
        if (gbuffer && rtvs[0] == g_pass.rt0 && rtvs[2] == g_pass.albedo && dsv == g_pass.dsv) return;  // same pass bound again
        EndPass(c);
    }
    if (!gbuffer) {
        // D11i: the main view's forward draws (one target or more over its depth buffer): the glass among them.
        g_ws.forward = false;
        if (n >= 1 && dsv && g_ws.main_depth) {
            ID3D11Resource* res = nullptr;
            dsv->GetResource(&res);
            if (res) res->Release();  // compared only
            g_ws.forward = res == g_ws.main_depth;
        }
        return;
    }
    g_ws.forward = false;
    g_pass.open = true;  // also with the drawing off: the self-check counts the pass's draws
    g_pass.rt0 = rtvs[0];
    g_pass.rt0->AddRef();
    g_pass.spec = rtvs[1];
    g_pass.spec->AddRef();
    g_pass.albedo = rtvs[2];
    g_pass.albedo->AddRef();
    g_pass.rt3 = rtvs[3];
    g_pass.rt3->AddRef();
    g_pass.dsv = dsv;
    g_pass.dsv->AddRef();
    if (!g_draw_off) SetPassState(c, -1);
}

void FilterDepthStencilState(ID3D11DeviceContext* c, ID3D11DepthStencilState** s, UINT* ref) {
    g_dss = *s;
    g_ref = *ref;
    if (!g_on) return;
    CheckStencil(*s, *ref);
    if (g_draw_off || !g_pass.open || !MakeObjects(c)) return;
    if (ID3D11DepthStencilState* v = Variant(*s)) {
        *s = v;
        *ref &= ~UINT(kBits);
    }
}

void OnSetViewports(UINT n, const D3D11_VIEWPORT* vps) {
    if (n && vps) g_vp = vps[0];
}

void OnSetVSConstantBuffers(UINT start, UINT n, ID3D11Buffer* const* bufs, const UINT* first, const UINT* num) {
    if (start != 0 || n == 0) return;
    g_cb0 = bufs ? bufs[0] : nullptr;
    g_cb0_first = (first && num && num[0]) ? first[0] : 0;
}

void OnSetIndexBuffer(ID3D11Buffer* ib) { g_ib = ib; }

void OnSetVertexBuffers(UINT start, UINT n, ID3D11Buffer* const* vbs) {
    if (start == 0 && n) g_vb0 = vbs ? vbs[0] : nullptr;
}

void OnMap(ID3D11Resource* res, const D3D11_MAPPED_SUBRESOURCE* m) {
    if (!g_on || !res || !m || !m->pData) return;
    int slot = -1;
    for (int i = 0; i < kWatched && slot < 0; ++i)
        if (g_cb_buf[i] == res) slot = i;
    // The description every time: a watched address can belong to a new buffer
    // (another size, or no longer a dynamic constant buffer).
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    D3D11_BUFFER_DESC d = {};
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER || !(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) ||
        d.Usage != D3D11_USAGE_DYNAMIC) {
        if (slot >= 0) g_cb_buf[slot] = nullptr, g_cb_ptr[slot] = nullptr, g_cb_width[slot] = 0;
        return;
    }
    for (int i = 0; i < kWatched && slot < 0; ++i)
        if (!g_cb_buf[i]) slot = i;
    if (slot < 0) {
        slot = g_cb_next;
        g_cb_next = (g_cb_next + 1) % kWatched;
    }
    g_cb_buf[slot] = res;
    g_cb_ptr[slot] = static_cast<const uint8_t*>(m->pData);
    g_cb_width[slot] = d.ByteWidth;
}

namespace {

// ---- D11i: the windshield, measured from the game's own glass (read only) ----
// The user, 30/10: dust still on the dashboard's top, which reaches past the cavity's front (0.85 m ahead of the eye);
// "take the glass code that kept it all clean, but without redrawing, so the game's glass is not affected". N30
// redrew the glass in the forward pass and that hid the game's glass with SnowyMoon (D10c-D10g); here nothing is drawn
// nor set there: the truck's glass draws are only recognised (the multiply blend, the depth tested and not written,
// at the truck's frame in the main view, as N30), their buffers kept, and once per mesh, at Present, its indices and
// then its vertices are copied to the CPU (a copy binds nothing). The glass's most forward point at the windshield's
// height and middle is where the cavity's box starts (the dashboard behind it; the hood ahead of it).
constexpr int kWsMaxMeshes = 24;
std::unordered_map<ID3D11BlendState*, bool> g_glass_blends;      // the multiply blend (referenced)
std::unordered_map<ID3D11DepthStencilState*, bool> g_glass_depth;  // tested, not written (referenced)

bool GlassBlend(ID3D11BlendState* bs) {
    const auto it = g_glass_blends.find(bs);
    if (it != g_glass_blends.end()) return it->second;
    D3D11_BLEND_DESC d = {};
    bs->GetDesc(&d);
    const D3D11_RENDER_TARGET_BLEND_DESC& t = d.RenderTarget[0];
    const bool glass = t.BlendEnable && t.SrcBlend == D3D11_BLEND_DEST_COLOR && t.DestBlend == D3D11_BLEND_ZERO;
    if (g_glass_blends.size() < 256) {
        bs->AddRef();
        g_glass_blends.emplace(bs, glass);
    }
    return glass;
}

bool GlassDepth(ID3D11DepthStencilState* s) {
    const auto it = g_glass_depth.find(s);
    if (it != g_glass_depth.end()) return it->second;
    D3D11_DEPTH_STENCIL_DESC d = {};
    s->GetDesc(&d);
    const bool glass = d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO;
    if (g_glass_depth.size() < 256) {
        s->AddRef();
        g_glass_depth.emplace(s, glass);
    }
    return glass;
}

// The draw's model -> camera rows from the game's cb0 (as the G-buffer draws'; the matrix may sit 1-3 rows down).
bool CurrentMatrix(float m[12]) {
    if (!g_cb0) return false;
    for (int i = 0; i < kWatched; ++i) {
        if (g_cb_buf[i] != g_cb0) continue;
        float rows[28];
        if (size_t(g_cb0_first) * 16 + sizeof(rows) > g_cb_width[i] ||
            !ReadRows(g_cb_ptr[i] + size_t(g_cb0_first) * 16, rows, sizeof(rows)))
            return false;
        for (int k = 0; k <= 3; ++k)
            if (MatrixAt(rows + 4 * k)) {
                memcpy(m, rows + 4 * k, 12 * sizeof(float));
                return true;
            }
        return false;
    }
    return false;
}

void WsRelease() {
    for (ID3D11Buffer** b : {&g_ws.ib, &g_ws.vb, &g_ws.staging})
        if (*b) (*b)->Release(), *b = nullptr;
    g_ws.stage = g_ws.wait = 0;
}

// A forward draw over the main view's depth: when it is the truck's glass and its mesh is new, its buffers are kept
// for WindshieldStep. Only Get calls on the context, and only for draws already at the truck's frame.
void WindshieldDraw(ID3D11DeviceContext* c, UINT count, UINT start, INT base) {
    if (g_ws.stage != 0 || g_ws.main_view < 0 || !g_dss || g_draw_off || int(g_ws.seen.size()) >= kWsMaxMeshes) return;
    if (!GlassDepth(g_dss)) return;
    float m[12], f[12], inv[12];
    if (!CurrentMatrix(m) || !rig::GetFrame(g_ws.main_view, 0, f, false) || !Invert(f, inv)) return;
    float t[12];  // the mesh -> the truck: inv(truck -> camera) x (mesh -> camera)
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 4; ++col)
            t[r * 4 + col] = inv[r * 4] * m[col] + inv[r * 4 + 1] * m[4 + col] + inv[r * 4 + 2] * m[8 + col] +
                             (col == 3 ? inv[r * 4 + 3] : 0.f);
    // At the truck's frame: near its origin, hardly turned (N30's test).
    if (t[3] * t[3] + t[7] * t[7] + t[11] * t[11] > 16.f || t[0] + t[5] + t[10] < 2.9f) return;
    for (const Windshield::Seen& s : g_ws.seen)
        if (s.ib == g_ib && s.start == start && s.count == count && s.base == base) return;
    ID3D11BlendState* bs = nullptr;
    float factor[4];
    UINT mask = 0;
    c->OMGetBlendState(&bs, factor, &mask);
    const bool multiply = bs && GlassBlend(bs);
    if (bs) bs->Release();
    if (!multiply) return;
    g_ws.seen.push_back({g_ib, start, count, base});
    ID3D11Buffer* ib = nullptr;
    ID3D11Buffer* vb = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT ib_offset = 0, stride = 0, vb_offset = 0;
    c->IAGetIndexBuffer(&ib, &format, &ib_offset);
    c->IAGetVertexBuffers(0, 1, &vb, &stride, &vb_offset);
    if (!ib || !vb || stride < 12 || (format != DXGI_FORMAT_R16_UINT && format != DXGI_FORMAT_R32_UINT) || !count ||
        count > 300000) {
        if (ib) ib->Release();
        if (vb) vb->Release();
        return;
    }
    g_ws.ib = ib, g_ws.vb = vb, g_ws.ib_format = format, g_ws.ib_offset = ib_offset, g_ws.stride = stride;
    g_ws.vb_offset = vb_offset, g_ws.start = start, g_ws.count = count, g_ws.base = base;
    memcpy(g_ws.to_truck, t, sizeof(t));
    g_ws.stage = 1;
}

// A staging copy of `bytes` of `src` from `offset` (nullptr: not made).
ID3D11Buffer* WsCopy(ID3D11DeviceContext* c, ID3D11Buffer* src, UINT offset, UINT bytes) {
    ID3D11Device* dev = nullptr;
    c->GetDevice(&dev);
    if (!dev) return nullptr;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = (bytes + 15) & ~15u;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* out = nullptr;
    const HRESULT hr = dev->CreateBuffer(&d, nullptr, &out);
    dev->Release();
    if (FAILED(hr)) return nullptr;
    const D3D11_BOX box = {offset, 0, 0, offset + bytes, 1, 1};
    c->CopySubresourceRegion(out, 0, 0, 0, 0, src, 0, &box);
    return out;
}

// At Present: one step of the mesh's reading (a copy, or a map of the last frame's copy when it is ready).
void WindshieldStep(ID3D11DeviceContext* c) {
    g_ws.frame_area = 0.f;
    if (g_ws_reset.exchange(false)) {
        WsRelease();
        g_ws.seen.clear();
        g_ws.measured = 0;
    }
    if (!g_ws.stage) return;
    const UINT isize = g_ws.ib_format == DXGI_FORMAT_R16_UINT ? 2 : 4;
    if (g_ws.stage == 1) {
        D3D11_BUFFER_DESC d = {};
        g_ws.ib->GetDesc(&d);
        const UINT from = g_ws.ib_offset + g_ws.start * isize, bytes = g_ws.count * isize;
        g_ws.staging = from + bytes <= d.ByteWidth ? WsCopy(c, g_ws.ib, from, bytes) : nullptr;
        if (!g_ws.staging) return WsRelease();
        g_ws.stage = 2;
        return;
    }
    D3D11_MAPPED_SUBRESOURCE m = {};
    const HRESULT hr = c->Map(g_ws.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (++g_ws.wait > 60) WsRelease();
        return;
    }
    if (FAILED(hr)) return WsRelease();
    if (g_ws.stage == 2) {  // the indices: the vertex range they use
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (UINT i = 0; i < g_ws.count; ++i) {
            const uint32_t v = isize == 2 ? static_cast<const uint16_t*>(m.pData)[i] : static_cast<const uint32_t*>(m.pData)[i];
            lo = std::min(lo, v), hi = std::max(hi, v);
        }
        c->Unmap(g_ws.staging, 0);
        g_ws.staging->Release();
        g_ws.staging = nullptr;
        const long long first = (long long)g_ws.base + lo, n = (long long)hi - lo + 1;
        D3D11_BUFFER_DESC d = {};
        g_ws.vb->GetDesc(&d);
        if (first < 0 || n <= 0 || n > 200000 || g_ws.vb_offset + (first + n) * g_ws.stride > d.ByteWidth) return WsRelease();
        g_ws.first = UINT(first), g_ws.vertices = UINT(n);
        g_ws.staging = WsCopy(c, g_ws.vb, g_ws.vb_offset + g_ws.first * g_ws.stride, g_ws.vertices * g_ws.stride);
        if (!g_ws.staging) return WsRelease();
        g_ws.stage = 3, g_ws.wait = 0;
        return;
    }
    // The vertices: float3 positions first in each (a mesh whose points land far off the truck is not read so).
    float eye[3];
    bool known;
    {
        std::lock_guard<std::mutex> lock(g_inside_mutex);
        known = g_eye_known;
        memcpy(eye, g_eye, sizeof(eye));
    }
    const float* t = g_ws.to_truck;
    bool sane = true;
    float front = 1e9f, front_y = 0.f, top = -1e9f;
    double sy = 0, sz = 0, syy = 0, syz = 0;  // D11j: z on y, least squares (the glass's slant)
    int fit = 0;
    for (UINT i = 0; i < g_ws.vertices && sane; ++i) {
        float p[3];
        memcpy(p, static_cast<const uint8_t*>(m.pData) + size_t(i) * g_ws.stride, sizeof(p));
        const float x = t[0] * p[0] + t[1] * p[1] + t[2] * p[2] + t[3];
        const float y = t[4] * p[0] + t[5] * p[1] + t[6] * p[2] + t[7];
        const float z = t[8] * p[0] + t[9] * p[1] + t[10] * p[2] + t[11];
        sane = std::fabs(x) < 3.f && y > -1.f && y < 6.f && std::fabs(z) < 15.f;
        // The windshield's middle at its height: not the headlamps' or a side mirror's glass.
        if (known && y > eye[1] - 0.9f && y < eye[1] + 1.2f && std::fabs(x) < 0.7f && z < eye[2] - 0.3f) {
            if (z < front) front = z, front_y = y;
            top = std::max(top, y);
            sy += y, sz += z, syy += double(y) * y, syz += double(y) * z, ++fit;
        }
    }
    c->Unmap(g_ws.staging, 0);
    WsRelease();
    if (!sane) return;
    ++g_ws.measured;
    const double den = fit * syy - sy * sy;
    const float slope = fit >= 3 && den > 1e-6 ? std::min(std::max(float((fit * syz - sy * sz) / den), 0.f), 2.f) : 0.f;
    if (front < eye[2] - 0.6f && front > eye[2] - 2.0f) {
        std::lock_guard<std::mutex> lock(g_inside_mutex);
        if (front < g_ws_front) g_ws_front = front, g_ws_base_y = front_y, g_ws_slope = slope, g_ws_top = top;
    }
}

// ---- P1: the truck's meshes read once and looked up in the pre-map (premap.h) ----
// The user (ATS 31/10, D14d: the cab's boxes still cut the hood): "map where the dust must not go... even pre-mapping
// every truck in the game, as long as gameplay is not slower". The interior seen from outside is an accessory model of
// its own (an interior's ext_model); its pieces are known from the game's files by index count, vertex count and the
// box of their vertices. A truck draw's mesh is read once (its indices, then the vertices they use: the float3
// position first in each, as D11i's glass) a step a frame at Present, like the windshield's; the result is kept per
// mesh (its buffers and range), and an interior piece is never marked again.
struct MeshRead {
    int stage = 0, wait = 0;  // 1 the indices to copy, 2 copied (to map), 3 the vertices copied (to map)
    uint64_t key = 0, part = 0;
    int unit = 0;
    ID3D11Buffer* ib = nullptr;
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* staging = nullptr;
    DXGI_FORMAT ib_format = DXGI_FORMAT_UNKNOWN;
    UINT ib_offset = 0, vb_offset = 0, stride = 0, start = 0, count = 0, first = 0, vertices = 0;
    INT base = 0;
};
MeshRead g_mr;
std::unordered_map<uint64_t, int8_t> g_prints;  // mesh -> premap kind (-1: being read or not readable)
// N57b (ATS 31/10, N57 session: 9000 meshes read in 12 min, nearly all again: the game moves its meshes to other
// buffer ranges all the time, the tracker restored 31097): a part's kind by the tracker's part id (unit, index count,
// place). Two reads agreeing make it sure: a new range of the part takes it unread; two disagreeing (two meshes of a
// count at one place) leave the part to its ranges' reads.
struct PartKind {
    int8_t kind = 0;
    int8_t reads = 0;
    bool mixed = false;
};
std::unordered_map<uint64_t, PartKind> g_part_kinds;
constexpr int kPartSure = 2;
std::atomic<long long> g_mp_by_part{0};
std::atomic<size_t> g_mp_parts{0};  // g_part_kinds' size, for the stats line
constexpr size_t kMaxPrints = 8192;
std::atomic<int> g_mp_read{0}, g_mp_failed{0}, g_mp_kind[premap::kKinds] = {}, g_mp_pruned{0};
std::mutex g_mp_mutex;
std::string g_mp_misses[2];  // the first meshes matching no piece (count, vertices, box): the truck's, the trailers'
std::atomic<int> g_mp_trailer_read{0};  // N57b: of the reads, the trailers' meshes
std::atomic<int> g_mp_by_size{0};       // N57c: of the matches, by the box's size (the mesh moved)

uint64_t MeshKey(UINT count, UINT start, INT base) {
    uint64_t h = 1469598103934665603ull;
    for (const uint64_t v : {uint64_t(reinterpret_cast<uintptr_t>(g_ib)), uint64_t(reinterpret_cast<uintptr_t>(g_vb0)),
                             uint64_t(count), uint64_t(start), uint64_t(uint32_t(base))})
        h = (h ^ v) * 1099511628211ull;
    return h;
}

void MrRelease() {
    for (ID3D11Buffer** b : {&g_mr.ib, &g_mr.vb, &g_mr.staging})
        if (*b) (*b)->Release(), *b = nullptr;
    g_mr.stage = g_mr.wait = 0;
}

// A truck draw (unit 0): its mesh's kind, kNone until read; a new mesh is taken for reading when the reader is free.
int MeshPrint(ID3D11DeviceContext* c, int unit, UINT count, UINT start, INT base, uint64_t part) {
    if (!premap::Pieces()) return premap::kNone;
    const uint64_t key = MeshKey(count, start, base);
    const auto it = g_prints.find(key);
    if (it != g_prints.end()) {
        // A truck seen again (its interior read before the reset): its boxes go as well.
        if (it->second == premap::kIntExt && !g_mp_int_ext.load(std::memory_order_relaxed)) g_mp_int_ext.store(1);
        return it->second < 0 ? premap::kNone : it->second;
    }
    int provisional = -1;  // R1: a part's kind that takes the dust off, for this range until its own read
    if (part) {  // N57b: a part read and sure: its kind for this range too
        const auto pk = g_part_kinds.find(part);
        if (pk != g_part_kinds.end() && !pk->second.mixed && pk->second.reads >= kPartSure) {
            const int kind = pk->second.kind;
            g_mp_by_part.fetch_add(1, std::memory_order_relaxed);
            if (kind == premap::kIntExt && !g_mp_int_ext.load(std::memory_order_relaxed)) g_mp_int_ext.store(1);
            // R1 (review 31/10): two meshes of one index count at one place are one part id [verified: the VNL 2025's
            // outside map, two 276-index meshes at the cab's pivot, one the interior's (external/s74h.pmg #18)]. A
            // kind that keeps the dust (ext, none) is safe to share unread; an interior's or a cargo's is taken for
            // now and the range is read too: a twin reading otherwise makes the part mixed (read per range after).
            if (kind != premap::kIntExt && kind != premap::kCargo) {
                if (g_prints.size() < kMaxPrints) g_prints.emplace(key, int8_t(kind));
                return kind;
            }
            provisional = kind;
        }
    }
    if (g_mr.stage != 0) return provisional < 0 ? premap::kNone : provisional;
    if (g_prints.size() >= kMaxPrints) {
        // P1b (ATS 31/10, P1: the cap reached in 22 min, 8192 meshes read, 56 of them the interior's: the game's
        // pooled buffers give a mesh new ranges as it streams): the rest forgotten, the interior's kept.
        for (auto e = g_prints.begin(); e != g_prints.end();)
            e = e->second == premap::kIntExt ? std::next(e) : g_prints.erase(e);
        g_mp_pruned.fetch_add(1, std::memory_order_relaxed);
        if (g_prints.size() >= kMaxPrints / 2) return provisional < 0 ? premap::kNone : provisional;
    }
    g_prints.emplace(key, int8_t(provisional));  // -1: being read (R1: or the part's kind until then)
    ID3D11Buffer* ib = nullptr;
    ID3D11Buffer* vb = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT ib_offset = 0, stride = 0, vb_offset = 0;
    c->IAGetIndexBuffer(&ib, &format, &ib_offset);
    c->IAGetVertexBuffers(0, 1, &vb, &stride, &vb_offset);
    if (!ib || !vb || stride < 12 || (format != DXGI_FORMAT_R16_UINT && format != DXGI_FORMAT_R32_UINT) || !count ||
        count > 600000) {
        if (ib) ib->Release();
        if (vb) vb->Release();
        g_mp_failed.fetch_add(1, std::memory_order_relaxed);
        return provisional < 0 ? premap::kNone : provisional;
    }
    g_mr.key = key, g_mr.part = part, g_mr.unit = unit, g_mr.ib = ib, g_mr.vb = vb, g_mr.ib_format = format, g_mr.ib_offset = ib_offset;
    g_mr.stride = stride, g_mr.vb_offset = vb_offset, g_mr.start = start, g_mr.count = count, g_mr.base = base;
    g_mr.stage = 1;
    return provisional < 0 ? premap::kNone : provisional;
}

// At Present: one step of the mesh's reading.
void MeshPrintStep(ID3D11DeviceContext* c) {
    if (g_mp_reset.exchange(false)) g_mp_int_ext.store(0);  // a new truck: its interior found again
    if (!g_mr.stage) return;
    const UINT isize = g_mr.ib_format == DXGI_FORMAT_R16_UINT ? 2 : 4;
    auto fail = [&]() {
        g_mp_failed.fetch_add(1, std::memory_order_relaxed);
        g_prints[g_mr.key] = -1;  // R1: not the part's kind either (unread: keeps its dust)
        MrRelease();
    };
    if (g_mr.stage == 1) {
        D3D11_BUFFER_DESC d = {};
        g_mr.ib->GetDesc(&d);
        const UINT from = g_mr.ib_offset + g_mr.start * isize, bytes = g_mr.count * isize;
        g_mr.staging = from + bytes <= d.ByteWidth ? WsCopy(c, g_mr.ib, from, bytes) : nullptr;
        if (!g_mr.staging) return fail();
        g_mr.stage = 2;
        return;
    }
    D3D11_MAPPED_SUBRESOURCE m = {};
    const HRESULT hr = c->Map(g_mr.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (++g_mr.wait > 60) fail();
        return;
    }
    if (FAILED(hr)) return fail();
    if (g_mr.stage == 2) {  // the indices: the vertex range they use
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (UINT i = 0; i < g_mr.count; ++i) {
            const uint32_t v = isize == 2 ? static_cast<const uint16_t*>(m.pData)[i] : static_cast<const uint32_t*>(m.pData)[i];
            lo = std::min(lo, v), hi = std::max(hi, v);
        }
        c->Unmap(g_mr.staging, 0);
        g_mr.staging->Release();
        g_mr.staging = nullptr;
        const long long first = (long long)g_mr.base + lo, n = (long long)hi - lo + 1;
        D3D11_BUFFER_DESC d = {};
        g_mr.vb->GetDesc(&d);
        if (first < 0 || n <= 0 || n > 300000 || g_mr.vb_offset + (first + n) * g_mr.stride > d.ByteWidth) return fail();
        g_mr.first = UINT(first), g_mr.vertices = UINT(n);
        g_mr.staging = WsCopy(c, g_mr.vb, g_mr.vb_offset + g_mr.first * g_mr.stride, g_mr.vertices * g_mr.stride);
        if (!g_mr.staging) return fail();
        g_mr.stage = 3, g_mr.wait = 0;
        return;
    }
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    bool sane = true;
    for (UINT i = 0; i < g_mr.vertices && sane; ++i) {
        float p[3];
        memcpy(p, static_cast<const uint8_t*>(m.pData) + size_t(i) * g_mr.stride, sizeof(p));
        for (int k = 0; k < 3; ++k) {
            sane = sane && std::isfinite(p[k]) && std::fabs(p[k]) < 100.f;
            lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
        }
    }
    c->Unmap(g_mr.staging, 0);
    bool by_size = false;
    const int kind = sane ? premap::Lookup(g_mr.count, g_mr.vertices, lo, hi, &by_size) : premap::kNone;
    if (by_size) g_mp_by_size.fetch_add(1, std::memory_order_relaxed);  // N57c
    g_prints[g_mr.key] = int8_t(kind);
    if (g_mr.part) {  // N57b
        if (g_part_kinds.size() >= kMaxPrints * 2) g_part_kinds.clear();
        PartKind& pk = g_part_kinds[g_mr.part];
        if (pk.reads && pk.kind != kind) pk.mixed = true;
        pk.kind = int8_t(kind);
        pk.reads = int8_t((std::min)(pk.reads + 1, 100));
        g_mp_parts.store(g_part_kinds.size(), std::memory_order_relaxed);
    }
    g_mp_read.fetch_add(1, std::memory_order_relaxed);
    if (g_mr.unit > 0) g_mp_trailer_read.fetch_add(1, std::memory_order_relaxed);
    g_mp_kind[kind].fetch_add(1, std::memory_order_relaxed);
    if (kind == premap::kIntExt) g_mp_int_ext.fetch_add(1);
    if (kind == premap::kNone) {
        std::lock_guard<std::mutex> lock(g_mp_mutex);
        std::string& misses = g_mp_misses[g_mr.unit > 0 ? 1 : 0];
        if (std::count(misses.begin(), misses.end(), ';') < 5) {
            char b[160];  // N57b: * = a piece has this index count (the read, not the table, is off)
            snprintf(b, sizeof(b), " %u%s/%u %s[%.3f %.3f %.3f .. %.3f %.3f %.3f];", g_mr.count,
                     premap::CountKnown(g_mr.count) ? "*" : "", g_mr.vertices, sane ? "" : "not float3 ", lo[0], lo[1],
                     lo[2], hi[0], hi[1], hi[2]);
            misses += b;
        }
    }
    MrRelease();
}

bool BeforeDrawBody(ID3D11DeviceContext* c, UINT count, UINT start, INT base) {
    if (g_pass.view < 0) {
        g_pass.view = ViewId(g_vp);
        g_pass.group = GroupId(g_vp);
        g_pass.vp = g_vp;
        ++g_frame_views;  // N11: 2+ in a frame = the interior camera with its mirrors
        // D11i: the frame's largest view is the main camera's; its forward draws test its depth buffer.
        const float area = g_vp.Width * g_vp.Height;
        if (g_pass.view >= 0 && area > g_ws.frame_area && g_pass.dsv) {
            ID3D11Resource* res = nullptr;
            g_pass.dsv->GetResource(&res);
            if (res) res->Release();  // compared only
            g_ws.frame_area = area, g_ws.main_view = g_pass.view, g_ws.main_depth = res;
        }
    }
    // Stencil ref 2: the cabin in the interior camera (the world is 1) [verified: V3a captures].
    const bool interior = (g_ref & 0x0f) == 2;
    if (interior) ++g_frame_ref2;
    if (g_pass.view < 0 || !g_cb0) return g_verdict = -2, false;
    g_gbuffer_draws.fetch_add(1, std::memory_order_relaxed);
    const uint8_t* ptr = nullptr;
    UINT width = 0;
    for (int i = 0; i < kWatched; ++i)
        if (g_cb_buf[i] == g_cb0) {
            ptr = g_cb_ptr[i];
            width = g_cb_width[i];
            break;
        }
    if (!ptr || size_t(g_cb0_first) * 16 + 48 > width) {
        g_no_pointer.fetch_add(1, std::memory_order_relaxed);
        return g_verdict = -3, false;
    }
    // Rows 0-2 (model -> camera); rows 0-7 when the view's projection is still missing.
    float m[32];
    const bool want_proj = g_mode >= 2 && !g_pass.has_proj && size_t(g_cb0_first) * 16 + 128 <= width;
    if (!ReadRows(ptr + size_t(g_cb0_first) * 16, m, want_proj ? 128 : 48)) {
        if (g_faults.fetch_add(1) == 0) Error("tint: access fault reading VS cb0 (draw skipped)");
        return g_verdict = -8, false;
    }
    ++g_frame_draws;
    // N29: the truck paint of the ATS trucks puts its own constants first, the matrix one or two rows down
    // [verified: D4r ATS capture, the Western Star 57X's cab, sleeper, hood, fenders and skirts: 10 draws with
    // "10 30 5 0.5" or two rows before it; their pixels never got the dust]. Rows 1-3 then, each checked whole.
    int shift = 0;
    if (!RotationRows(m)) {
        float more[28];
        shift = -1;
        if (size_t(g_cb0_first) * 16 + sizeof(more) <= width && ReadRows(ptr + size_t(g_cb0_first) * 16, more, sizeof(more)))
            for (int k = 1; k <= 3 && shift < 0; ++k)
                if (MatrixAt(more + 4 * k)) shift = k;
        if (shift < 0) {  // not a model->camera rotation: some other constant layout
            g_not_matrix.fetch_add(1, std::memory_order_relaxed);
            ++g_frame_not_matrix;
            return g_verdict = -4, false;
        }
        memcpy(m, more + 4 * shift, 12 * sizeof(float));
        g_shifted.fetch_add(1, std::memory_order_relaxed);
    }
    // N11: turned off by the self-check: nothing marked (and nothing learned).
    if (g_draw_off || (g_interior_off && (g_prev_views >= 2 || g_frame_views >= 2))) return g_verdict = -5, false;
    if (want_proj && !shift) g_pass.has_proj = Projection(m, g_pass.proj);
    rig::DrawKey key;
    key.vb = g_vb0, key.ib = g_ib, key.start = start, key.base = base, key.count = count;
    bool cab = false;
    // N55: a view 1200 px wide or more is the camera's own (the mirrors' are 512-1024 px).
    uint64_t part = 0;  // N57b
    const int unit = rig::Classify(g_pass.view, g_pass.group, key, m, interior, &cab, g_pass.vp.Width >= 1200.f, &part);
    if (unit < 0 || unit >= rig::kUnits) return g_verdict = -6, false;
    // P1: the interior seen from outside (the pre-map's ext_model pieces): no dust. N57: a trailer's load: its own.
    const int kind = MeshPrint(c, unit, count, start, base, part);
    if (unit == 0 && kind == premap::kIntExt) return g_verdict = -10, false;
    if (!MakeObjects(c) || !Variant(g_dss)) return g_verdict = -7, false;
    cab = cab && unit == 0;
    const bool cargo = unit > 0 && kind == premap::kCargo;
    SetPassState(c, unit, cab, cargo);
    if (cargo) g_cargo_marked.fetch_add(1, std::memory_order_relaxed);
    ++g_pass.marked;
    ++g_pass.marked_class[ClassOf(unit, cab, cargo)];
    g_marked.fetch_add(1, std::memory_order_relaxed);
    g_verdict = unit;
    return true;
}

// N11 at Present: the frame's counts and the game state go to the checker, its decisions come back.
void CompatFrame(const GameState& game) {
    const rig::Stats rs = rig::GetStats();
    compat::Frame f;
    f.paused = game.paused;
    f.wipers = game.wipers;
    f.rain = game.rain;
    f.unit_present[0] = game.truck;
    f.unit_present[1] = game.trailer;
    // Nothing is classified while the drawing (or this camera's dust) is off: no warning for parts then.
    const bool not_classified = g_draw_off || (g_interior_off && g_frame_views >= 2);
    for (int u = 0; u < rig::kUnits && u < compat::kUnits; ++u) f.unit_recognised[u] = not_classified || rs.members[u] > 0;
    f.views = g_frame_views;
    f.draws = g_frame_draws;
    f.not_matrix = g_frame_not_matrix;
    f.ref2 = g_frame_ref2;
    std::lock_guard<std::mutex> lock(g_compat_mutex);
    g_compat.EndFrame(Now(), f);
    g_draw_off = g_compat.DrawingOff();
    g_interior_off = g_compat.InteriorOff();
    MoveCompatLines();
}

}  // namespace

void OnDeferredDraw(ID3D11DeviceContext* c) {
    g_deferred_draws.fetch_add(1, std::memory_order_relaxed);
    ID3D11RenderTargetView* rtvs[4] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(4, rtvs, &dsv);
    const DXGI_FORMAT want[4] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UINT};
    bool gbuffer = dsv != nullptr;
    for (int i = 0; i < 4; ++i) {
        D3D11_RENDER_TARGET_VIEW_DESC d = {};
        if (rtvs[i]) rtvs[i]->GetDesc(&d);
        gbuffer = gbuffer && rtvs[i] && d.Format == want[i];
        if (rtvs[i]) rtvs[i]->Release();
    }
    if (dsv) dsv->Release();
    if (gbuffer) g_deferred_gbuffer.fetch_add(1, std::memory_order_relaxed);
}

int LastVerdict() { return g_verdict; }

void OnIndirectDraw(ID3D11DeviceContext* c, bool immediate) {
    (void)c;
    g_indirect_draws.fetch_add(1, std::memory_order_relaxed);
    if (immediate && g_pass.open) g_indirect_gbuffer.fetch_add(1, std::memory_order_relaxed);
}

bool BeforeDrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base) {
    g_verdict = -1;
    if (!g_on) return false;
    if (!g_pass.open) {
        if (g_ws.forward) WindshieldDraw(c, count, start, base);  // D11i: read only
        return false;
    }
    if (++g_sample_draw % kSample) return BeforeDrawBody(c, count, start, base);
    const long long t0 = Ticks();
    const bool swapped = BeforeDrawBody(c, count, start, base);
    g_cost_draw += (Ticks() - t0) * kSample;
    return swapped;
}

void AfterDraw(ID3D11DeviceContext* c) { SetPassState(c, -1); }

void OnPresent(ID3D11DeviceContext* c, const GameState& game) {
    if (!g_on) return;
    {  // D13f
        const long long now = Ticks();
        if (g_frame_last && !game.paused) {
            const long long d = now - g_frame_last;
            const double ms = double(d) * TickSeconds() * 1e3;
            if (d > g_frame_worst) g_frame_worst = d, GetLocalTime(&g_frame_worst_at);
            g_frame_slow75 += ms > 1000.0 / 75.0;
            g_frame_slow60 += ms > 1000.0 / 60.0;
        }
        g_frame_last = now;
    }
    if (g_pass.open) EndPass(c);
    g_ws.forward = false;
    g_busy = true;  // D11i: our own Map and copies stay out of the hooks
    WindshieldStep(c);
    MeshPrintStep(c);  // P1
    TimerFrame(c);  // D13d
    g_busy = false;
    const long long t0 = Ticks();
    rig::EndFrame();
    const long long t1 = Ticks();
    CompatFrame(game);
    const long long t2 = Ticks();
    g_cost_end_frame += t1 - t0;
    g_rig_worst = (std::max)(g_rig_worst, t1 - t0);  // D13f
    g_cost_compat += t2 - t1;
    ++g_cost_frames;
    g_prev_views = g_frame_views;
    g_frame_views = 0;
    g_frame_draws = g_frame_not_matrix = g_frame_ref2 = 0;
    rig::BeginFrame();
    for (int i = 0; i < g_runs_n; ++i) g_runs[i].runs = 0;
    ++g_frame;
}

std::string StatsLine() {
    const rig::Stats s = rig::GetStats();
    char b[1536];
    snprintf(b, sizeof(b),
             "%s: frames %lld, g-buffer draws %lld (no pointer %lld, not a matrix %lld, faults %lld), marked %lld, "
             "passes %lld, unit draws %lld (no projection %lld, no frame %lld), dust truck %.3f trailer %.3f | rig: "
             "claimed %lld (cab %lld), missed %lld, searches %lld, found %lld (rejected %lld), predicted %lld, held %lld, lost %lld, interior %lld, "
             "promoted %lld, inherited %lld (unclassed %lld), reloaded %lld, restored %lld (map N47: truck %d/%d, trailer %d/%d); now truck in %d view(s) (%d draws; known %d: cab %d, chassis %d; pending %d), trailer in %d "
             "(%d; known %d, pending %d)",
             g_mode == 1 ? "tint" : g_mode == 3 ? "position test" : "dust", s.frames, g_gbuffer_draws.load(), g_no_pointer.load(), g_not_matrix.load(),
             g_faults.load(), g_marked.load(), g_passes.load(), g_unit_draws.load(), g_no_proj.load(), g_no_frame.load(),
             g_dust[0].load(), g_dust[1].load(), s.claimed, s.cab_claims, s.missed, s.searches, s.found, s.rejected, s.predicted, s.held, s.lost,
             s.interior, s.promoted, s.inherited, s.unclassed, s.reloaded, s.restored, s.map_used[0], s.map[0], s.map_used[1], s.map[1], s.tracked_views[0], s.members[0], s.known[0], s.cab_members, s.chassis_members, s.pending[0], s.tracked_views[1],
             s.members[1], s.known[1], s.pending[1]);
    std::string line = b;
    // N20: each wheel's reach outwards (cm out of the SDK's position) and the parts on its axle it comes from.
    const char* sep = " | wheel reach cm (parts): ";
    for (int u = 0; u < rig::kUnits; ++u) {
        float wheels[rig::kMaxWheels][4];
        const int n = rig::GetWheels(u, wheels);
        if (!n) continue;
        line += std::string(sep) + (u == 0 ? "truck" : u == 1 ? "trailer" : "trailer." + std::to_string(u - 1));
        sep = ", ";
        for (int w = 0; w < n; ++w) {
            snprintf(b, sizeof(b), " %.0f(%d)", s.wheel_out[u][w] * 100.f, s.wheel_parts[u][w]);
            line += b;
        }
    }
    snprintf(b, sizeof(b), " | paint reflection (N25): rt3 copies %lld | deferred contexts (N27): draws %lld, into a G-buffer %lld",
             g_rt3_copies.load(), g_deferred_draws.load(), g_deferred_gbuffer.load());
    line += b;
    snprintf(b, sizeof(b), " | relief (D11k): %s (D13f: rt0 not copied)",
             !g_relief.load() ? "off ([dust] relief = 0)" : g_ps_norm ? "on" : g_compile_state.load() == 2 ? "no shader" : "not yet");
    line += b;
    snprintf(b, sizeof(b), " | pack (D11m): %ux%u%s", g_pack_w, g_pack_h, g_pack_failed ? " failed" : "");
    line += b;
    {  // D11e
        const int cs = g_compile_state.load();
        snprintf(b, sizeof(b), " | shaders (D11e): %s in %.1f s%s",
                 cs == 2 ? "compiled" : cs == 3 ? "failed" : "compiling", double(g_compile_ms.load()) / 1000.0,
                 cs == 2 && !g_blob_refl ? " (no rt3 variant)" : "");
        line += b;
    }
    {  // D11i
        float front, eye_z, base_y, slope, top;
        {
            std::lock_guard<std::mutex> lock(g_inside_mutex);
            front = g_ws_front, eye_z = g_eye[2], base_y = g_ws_base_y, slope = g_ws_slope, top = g_ws_top;
        }
        if (front < 1e8f)
            snprintf(b, sizeof(b),
                     " | windshield (D11i): front z %.2f (%.2f m ahead of the eye), %d glass mesh(es) read; base y %.2f, "
                     "slant dz/dy %.2f, top y %.2f (D11j)",
                     front, eye_z - front, g_ws.measured, base_y, slope, top);
        else
            snprintf(b, sizeof(b), " | windshield (D11i): not measured (%d glass mesh(es) read)", g_ws.measured);
        line += b;
    }
    {  // P1
        std::string misses;
        {
            std::lock_guard<std::mutex> lock(g_mp_mutex);
            misses = (g_mp_misses[0].empty() ? std::string(" -") : g_mp_misses[0]) + " trailers'" +
                     (g_mp_misses[1].empty() ? std::string(" -") : g_mp_misses[1]);
        }
        snprintf(b, sizeof(b), " | premap (P1): %d pieces; meshes read %d (the trailers' %d; failed %d): int_ext %d, int %d, ext %d, "
                 "cargo %d, none %d (cache emptied %d); cargo draws marked %lld (N57); matched by size %d (N57c); ranges taken by part %lld, parts %zu (N57b); this truck's interior pieces %d, boxes %s; first misses%s",
                 premap::Pieces(), g_mp_read.load(), g_mp_trailer_read.load(), g_mp_failed.load(), g_mp_kind[premap::kIntExt].load(),
                 g_mp_kind[premap::kInt].load(), g_mp_kind[premap::kExt].load(), g_mp_kind[premap::kCargo].load(),
                 g_mp_kind[premap::kNone].load(),
                 g_mp_pruned.load(), g_cargo_marked.load(), g_mp_by_size.load(), g_mp_by_part.load(), g_mp_parts.load(), g_mp_int_ext.load(), g_mp_int_ext.load() > 0 ? "off" : "on", misses.c_str());
        line += b;
    }
    snprintf(b, sizeof(b), " | indirect draws (N28): %lld, in a G-buffer pass %lld | matrix lower in cb0 (N29): %lld draws",
             g_indirect_draws.load(), g_indirect_gbuffer.load(), g_shifted.load());
    line += b;
    for (int u = 2; u < rig::kUnits; ++u) {  // N21: a road train's units past trailer.0
        float wheels[rig::kMaxWheels][4];
        if (!rig::GetWheels(u, wheels)) continue;
        snprintf(b, sizeof(b), " | trailer.%d in %d view(s) (%d; known %d, pending %d)", u - 1, s.tracked_views[u], s.members[u],
                 s.known[u], s.pending[u]);
        line += b;
    }
    if (s.tail[1] > 0.f) {  // N24
        snprintf(b, sizeof(b), " | trailer box ends at z %.2f m (trailer.1 hooked)", s.tail[1]);
        line += b;
    }
    {
        std::lock_guard<std::mutex> lock(g_compat_mutex);
        line += " | compat: " + g_compat.StatsText();
    }
    // N11 / N3 B4: the cost per frame, in microseconds, since the last line (render thread).
    const double us = g_cost_frames ? TickSeconds() * 1e6 / double(g_cost_frames) : 0.0;
    snprintf(b, sizeof(b), " | cost per frame: before-draw %.1f us (1 call in %u timed), rig end-frame %.1f us, compat %.1f us (%lld frames)",
             double(g_cost_draw) * us, kSample, double(g_cost_end_frame) * us, double(g_cost_compat) * us, g_cost_frames);
    line += b;
    // D13: the rig end-frame by part since the last line (us a frame); anchored = views found again by known parts.
    static rig::Stats last;
    const double per = g_cost_frames ? 1e6 / double(g_cost_frames) : 0.0;
    snprintf(b, sizeof(b), " | rig parts (D13): search %.1f, anchor %.1f, learn %.1f (index %.1f), age %.1f, map %.1f us; "
             "anchored %lld",
             (s.sec_search - last.sec_search) * per, (s.sec_anchor - last.sec_anchor) * per,
             (s.sec_learn - last.sec_learn) * per, (s.sec_index - last.sec_index) * per, (s.sec_age - last.sec_age) * per,
             (s.sec_map - last.sec_map) * per, s.anchored);
    last = s;
    line += b;
    // D13d: the GPU's time in our passes (a timed frame's sum) and the copies' rectangle.
    snprintf(b, sizeof(b), " | pass gpu (D13d): %.0f us a frame (max %.0f; %lld frames timed%s), copies %.0f%% of the "
             "view (%lld passes), classes in a rectangle %lld (%.0f%% of the view; not known %lld)%s",
             g_gpu_frames ? g_gpu_us / double(g_gpu_frames) : 0.0, g_gpu_max_us, g_gpu_frames,
             g_timer_failed ? ", no queries" : "", g_rect_passes ? 100.0 * g_rect_cover / double(g_rect_passes) : 0.0,
             g_rect_passes, g_rect_parts, g_rect_parts ? 100.0 * g_rect_class_cover / double(g_rect_parts) : 0.0,
             g_rect_unknown, g_rects_off.load() ? "; rectangles off after an exception (D13e)" : "");
    line += b;
    snprintf(b, sizeof(b), " | cab inside (N55): %d parts of %d seen, %lld draws looked at, main view looks %lld "
             "(placed %lld); not used (D14d), boxes on (to the front wheels' track)",
             s.inside_parts, s.inside_all, s.inside_seen, s.inside_looks, s.inside_placed);
    line += b;
    {  // D14c
        double w[7];
        rig::TakeWorst(w);
        snprintf(b, sizeof(b), " | rig worst (D14c): search %.0f, anchor %.0f, learn %.0f, index %.0f, age %.0f, map %.0f, "
                 "inside %.0f us", w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
        line += b;
    }
    snprintf(b, sizeof(b), " | frames (D13f): worst %.1f ms at %02d:%02d:%02d.%03d, over 13.3 ms %lld, over 16.7 ms %lld; "
             "rig end-frame worst %.0f us",
             double(g_frame_worst) * TickSeconds() * 1e3, g_frame_worst_at.wHour, g_frame_worst_at.wMinute,
             g_frame_worst_at.wSecond, g_frame_worst_at.wMilliseconds, g_frame_slow75, g_frame_slow60,
             double(g_rig_worst) * TickSeconds() * 1e6);
    g_frame_worst = g_frame_slow75 = g_frame_slow60 = g_rig_worst = 0;
    g_gpu_us = g_gpu_max_us = 0, g_gpu_frames = 0;
    g_rect_cover = g_rect_class_cover = 0, g_rect_passes = g_rect_parts = g_rect_unknown = 0;
    g_cost_draw = g_cost_end_frame = g_cost_compat = g_cost_frames = 0;
    return line + b;
}

std::string TakeError() {
    std::lock_guard<std::mutex> lock(g_err_mutex);
    std::string e;
    e.swap(g_error);
    return e;
}

std::vector<std::string> TakeCompatLines() {
    std::vector<std::string> out;
    if (!g_compat_pending.load(std::memory_order_acquire)) return out;
    std::lock_guard<std::mutex> lock(g_compat_mutex);
    out.swap(g_compat_lines);
    g_compat_pending.store(false, std::memory_order_relaxed);
    return out;
}

std::string CompatSummary() {
    std::lock_guard<std::mutex> lock(g_compat_mutex);
    return g_compat.Summary();
}

}  // namespace tint
