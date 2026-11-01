// Offline preview of the D3 dust (src/dust_shader.h) on a simple truck, to
// judge the look without the game.
//
// A full-screen pass ray-traces a box-and-cylinder truck (FH-like layout, the
// V3a wheel positions) and a ground plane into a fake G-buffer laid out as the
// game's (rt0 = camera-space normal + camera z, albedo, specular), marks the
// truck's pixels with stencil bit 0x10, runs the dust shader exactly as the
// DLL does (albedo draw, then specular draw, stencil EQUAL 0x10) and lights
// the result with a sun and an ambient term. One PNG per view and level.
// N5: a box trailer behind the truck (stencil bit 0x20, its own draw, as the
// DLL's trailer) and one scene per D4 layer, then a few with several layers
// (dust_layer_*, dust_mix_*, dust_trailer_*).
// N10: the test panel (src/panel_draw) over the last view, open and closed
// (badge), with a check that it gives the D3D state back as it found it.
// N12: steered and turned front wheels (the scene draws them steered, the
// dirt pass gets the same turns as from the telemetry), levels 0.9 and 1 side
// by side (dust_level_*: the middle half of each view), salt on the tyre.
// N13: the pass gets its levels through dust::Levels, as the DLL (level 1 =
// the look 0.8 had): old 0.8 | new 1 side by side (dust_level_*_080old_100new),
// checked to be the same image; 0.9 | 1 on the new scale; the front wheels
// raised by their suspension deflection (braking), the dirt at the wheels
// this frame (rig::GetWheels) against the dirt at their config height (before
// N13: wheel_deflected_*).
// N19: the trailer's wheels given to the dirt pass 0.2 m inwards of their
// tyres, as the game gives a trailer's (the axle's end), rolled 0 | 1/8 turn
// side by side: the tyre's outer side must turn with it (trailer_wheel_*).
// N20: the same wheel given 0.5 m inwards, without | with its reach measured
// (0.5 m, as rig::GetWheelReach would give it): the tyre's outer side takes
// the body's dirt | the wheel's (trailer_wheel_deep_*).
// N22: the rear wheels from below, the mudguard's underside 0.55 m over their
// centre: the pass without | with the tyres' radius (the SDK's wheel.radius):
// the underside takes the wheel's dirt | the body's (wheel_fender_*).
// N31: the layers from the dirt model (mud, then dry dirt): right after the
// mud | after 2 km of asphalt, the tread worn back to the rubber while the
// side walls keep the dirt; and on a wet road (tread_*).
// Usage: dustpreview <out dir> [--bench] (DUSTPREVIEW_WARP=1: software rasterizer; --bench: the GPU time of the
// pass at the game's size, no PNGs)

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wincodec.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "dust_shader.h"
#include "panel_draw.h"
#include "test_panel.h"

namespace {

constexpr UINT kW = 1280, kH = 720;
constexpr float kTanY = 0.36f;  // ~40 degrees vertical field of view
constexpr float kTanX = kTanY * kW / kH;

// The V3a truck's wheels (Volvo FH6), as the SDK gives them: x y z (y = centre height).
const float kTruckWheels[6][3] = {{-1.04f, 0.501832f, -1.79247f}, {1.04f, 0.501832f, -1.79247f},
                                  {-0.9317f, 0.501832f, 1.41812f}, {0.9317f, 0.501832f, 1.41812f},
                                  {-0.9317f, 0.501832f, 2.76783f}, {0.9317f, 0.501832f, 2.76783f}};
// A three-axle box trailer, in the truck's frame (the preview gives both units the same frame).
const float kTrailerWheels[6][3] = {{-1.02f, 0.5f, 9.4f},   {1.02f, 0.5f, 9.4f},   {-1.02f, 0.5f, 10.75f},
                                    {1.02f, 0.5f, 10.75f}, {-1.02f, 0.5f, 12.1f}, {1.02f, 0.5f, 12.1f}};

int Fail(const char* what, HRESULT hr = S_OK) {
    printf("FAIL: %s (hr=0x%08X)\n", what, static_cast<unsigned>(hr));
    return 1;
}

// N3 B6: what PSGetConstantBuffers1 gives back for a buffer bound without a
// range (PSSetConstantBuffers), as tint's restore of the game's PS cb0 sees it.
// The answer comes from the D3D11 runtime, so WARP shows the same as a GPU.
void ReportCbRange(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1)))) {
        printf("cb range: no ID3D11DeviceContext1\n");
        return;
    }
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = 4096, d.Usage = D3D11_USAGE_DYNAMIC, d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* b = nullptr;
    if (SUCCEEDED(dev->CreateBuffer(&d, nullptr, &b))) {
        ID3D11Buffer* got = nullptr;
        UINT first = 1234, num = 1234;
        ctx->PSSetConstantBuffers(0, 1, &b);
        c1->PSGetConstantBuffers1(0, 1, &got, &first, &num);
        printf("cb range: bound without a range (%u bytes) -> first %u, num %u%s\n", d.ByteWidth, first, num,
               got == b ? "" : " (other buffer)");
        if (got) got->Release();
        const UINT set_first = 16, set_num = 32;
        c1->PSSetConstantBuffers1(0, 1, &b, &set_first, &set_num);
        got = nullptr, first = num = 1234;
        c1->PSGetConstantBuffers1(0, 1, &got, &first, &num);
        printf("cb range: bound with first %u, num %u -> first %u, num %u\n", set_first, set_num, first, num);
        if (got) got->Release();
        ID3D11Buffer* none = nullptr;
        ctx->PSSetConstantBuffers(0, 1, &none);
        b->Release();
    }
    c1->Release();
}

const char kFullVs[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {"
    " float2 uv = float2((id << 1) & 2, id & 2);"
    " return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }";

// The scene: camera rows (camera -> unit), the field of view, the sun.
const char kScenePs[] = R"(
cbuffer S : register(b0) {
    float4 c0, c1, c2;   // camera -> unit rows
    float4 view;         // tan x, tan y, width, height
    float4 wheel[6];     // x y z radius
    float4 opts;         // x trailer, y truck, z the unit the mask marks (0 truck, 1 trailer), w the trailer's paint (0: grey)
    float4 twheel[6];    // the trailer's wheels
    float4 steer[6];     // the truck's wheels: cos, sin of the steering
    float4 paint;        // D14: x the truck's paint albedo (0: the dark brown-grey)
};
struct Hit { float t; float3 n; int mat; };
void Box(float3 o, float3 d, float3 lo, float3 hi, int mat, inout Hit h) {
    const float3 inv = 1.0 / d;
    const float3 t0 = (lo - o) * inv, t1 = (hi - o) * inv;
    const float3 tn = min(t0, t1), tf = max(t0, t1);
    const float a = max(max(tn.x, tn.y), tn.z), b = min(min(tf.x, tf.y), tf.z);
    if (a > b || a <= 0.0 || a >= h.t) return;
    h.t = a;
    h.n = a == tn.x ? float3(-sign(d.x), 0, 0) : a == tn.y ? float3(0, -sign(d.y), 0) : float3(0, 0, -sign(d.z));
    h.mat = mat;
}
// A wheel: cylinder about x, radius r, from x0 to x1 (material mat = tyre, mat + 1 = rim).
void Wheel(float3 o, float3 d, float3 c, float r, float x0, float x1, int mat, inout Hit h) {
    const float2 oc = o.yz - c.yz, dd = d.yz;
    const float A = dot(dd, dd), B = dot(oc, dd), C = dot(oc, oc) - r * r;
    const float disc = B * B - A * C;
    if (disc > 0.0) {
        const float t = (-B - sqrt(disc)) / A;
        const float x = o.x + d.x * t;
        if (t > 0.0 && t < h.t && x >= x0 && x <= x1) {
            h.t = t;
            h.n = float3(0, normalize(oc + dd * t));
            h.mat = mat;
        }
    }
    const float xs[2] = {x0, x1};
    [unroll] for (int k = 0; k < 2; ++k) {
        const float t = (xs[k] - o.x) / d.x;
        const float2 q = o.yz + d.yz * t - c.yz;
        if (t > 0.0 && t < h.t && dot(q, q) < r * r) {
            h.t = t;
            h.n = float3(k == 0 ? -1 : 1, 0, 0);
            h.mat = length(q) < r * 0.62 ? mat + 1 : mat;
        }
    }
}
Hit Trace(float3 o, float3 d, bool truck_only) {
    Hit h;
    h.t = 1e9;
    h.n = 0;
    h.mat = -1;
    if (opts.y > 0.5) {
        Box(o, d, float3(-1.25, 1.05, -2.55), float3(1.25, 3.75, -0.35), 0, h);   // cab
        Box(o, d, float3(-1.25, 3.75, -2.2), float3(1.25, 3.95, -0.6), 0, h);    // roof deflector
        Box(o, d, float3(-1.25, 0.45, -2.70), float3(1.25, 1.05, -2.30), 0, h);  // bumper
        Box(o, d, float3(-1.25, 1.00, -2.30), float3(1.25, 1.15, -1.25), 0, h);  // front fender
        Box(o, d, float3(-0.45, 0.55, -2.30), float3(0.45, 0.95, 3.60), 1, h);   // chassis
        Box(o, d, float3(-1.20, 0.45, -1.10), float3(-0.50, 1.00, 0.70), 2, h);  // fuel tank
        Box(o, d, float3(0.50, 0.50, -1.10), float3(1.20, 1.00, 0.20), 1, h);    // battery box
        Box(o, d, float3(-1.25, 1.05, 0.90), float3(1.25, 1.12, 3.30), 1, h);    // rear mudguards
        Box(o, d, float3(-1.00, 1.12, 0.40), float3(1.00, 1.30, 3.30), 2, h);    // fifth wheel plate
        [unroll] for (int i = 0; i < 6; ++i) {
            // In the wheel's frame (its steering undone about y through its centre), then the normal back.
            const float4 w = wheel[i];
            const float2 cs = steer[i].xy;
            const float3 ro = o - w.xyz;
            const float3 lo = float3(ro.x * cs.x - ro.z * cs.y, ro.y, ro.x * cs.y + ro.z * cs.x);
            const float3 ld = float3(d.x * cs.x - d.z * cs.y, d.y, d.x * cs.y + d.z * cs.x);
            const float before = h.t;
            Wheel(lo, ld, float3(0, 0, 0), w.w, -0.16, 0.16, 3, h);
            if (h.t < before) h.n = float3(h.n.x * cs.x + h.n.z * cs.y, h.n.y, -h.n.x * cs.y + h.n.z * cs.x);
        }
    }
    if (opts.x > 0.5) {  // materials 5-8: the trailer
        Box(o, d, float3(-1.27, 1.32, 0.55), float3(1.27, 4.00, 14.10), 5, h);   // box body
        Box(o, d, float3(-0.50, 1.00, 0.55), float3(0.50, 1.32, 14.00), 6, h);   // chassis rails
        Box(o, d, float3(-1.00, 0.30, 2.90), float3(-0.85, 1.32, 3.10), 6, h);   // landing gear
        Box(o, d, float3(0.85, 0.30, 2.90), float3(1.00, 1.32, 3.10), 6, h);
        Box(o, d, float3(-1.27, 1.10, 8.70), float3(1.27, 1.32, 12.80), 6, h);   // mudguards
        Box(o, d, float3(-1.20, 0.50, 13.90), float3(1.20, 0.65, 14.10), 6, h);  // rear bumper
        [unroll] for (int i = 0; i < 6; ++i) {
            const float4 w = twheel[i];
            Wheel(o, d, w.xyz, w.w, w.x - 0.16, w.x + 0.16, 7, h);
        }
    }
    if (!truck_only && d.y < 0.0) {  // the ground
        const float t = -o.y / d.y;
        if (t > 0.0 && t < h.t) {
            h.t = t;
            h.n = float3(0, 1, 0);
            h.mat = 9;
        }
    }
    return h;
}
void Ray(float4 pos, out float3 dc, out float3 o, out float3 d) {
    const float2 ndc = float2(pos.x / view.z * 2.0 - 1.0, 1.0 - pos.y / view.w * 2.0);
    dc = float3(ndc.x * view.x, ndc.y * view.y, -1.0);
    o = float3(c0.w, c1.w, c2.w);
    d = float3(dot(c0.xyz, dc), dot(c1.xyz, dc), dot(c2.xyz, dc));
}
#ifdef MASK
float4 main(float4 pos : SV_Position) : SV_Target0 {
    float3 dc, o, d;
    Ray(pos, dc, o, d);
    const Hit all = Trace(o, d, false);
    // Only the unit's pixels get its stencil bit.
    if (all.mat < 0 || all.mat == 9 || (all.mat >= 5 && all.mat <= 8 ? 1 : 0) != (int)opts.z) discard;
    return 0;
}
#else
struct O { float4 rt0 : SV_Target0; float4 albedo : SV_Target1; float4 spec : SV_Target2; };
O main(float4 pos : SV_Position) {
    float3 dc, o, d;
    Ray(pos, dc, o, d);
    const Hit h = Trace(o, d, false);
    O r;
    r.rt0 = 0;
    r.albedo = float4(0.45, 0.62, 0.85, 0);  // sky
    r.spec = 0;
    if (h.mat < 0) return r;
    // Camera space: the ray at t (dc has z = -1), the normal by the transposed rotation.
    const float3 pc = dc * h.t;
    const float3 nc = normalize(float3(dot(float3(c0.x, c1.x, c2.x), h.n), dot(float3(c0.y, c1.y, c2.y), h.n),
                                       dot(float3(c0.z, c1.z, c2.z), h.n)));
    r.rt0 = float4(nc, pc.z);
    const float3 hp = float3(c0.w, c1.w, c2.w) + float3(dot(c0.xyz, dc), dot(c1.xyz, dc), dot(c2.xyz, dc)) * h.t;
    float3 alb = paint.x > 0.0 ? paint.x : float3(0.07, 0.06, 0.05);  // paint: dark brown-grey (the user's truck)
    float sp = 0.30;
    if (h.mat == 1) { alb = 0.045; sp = 0.10; }
    if (h.mat == 2) { alb = 0.55; sp = 0.60; }
    if (h.mat == 3) { alb = 0.035; sp = 0.04; }
    if (h.mat == 4 || h.mat == 8) { alb = 0.50; sp = 0.45; }
    if (h.mat == 5) { alb = opts.w > 0.0 ? opts.w : float3(0.30, 0.31, 0.33); sp = 0.30; }  // trailer paint: mid grey
    if (h.mat == 6) { alb = 0.045; sp = 0.10; }
    if (h.mat == 7) { alb = 0.035; sp = 0.04; }
    if (h.mat == 9) {
        const float lane = abs(frac(hp.x / 3.6) - 0.5) < 0.02 ? 1.0 : 0.0;
        alb = lerp(0.16, 0.7, lane);
        sp = 0.02;
    }
    r.albedo = float4(alb, 1);
    r.spec = float4(sp, sp, sp, 40);
    return r;
}
#endif
)";

// Light: ambient + sun + specular, then a plain tone curve to sRGB.
const char kLightPs[] = R"(
Texture2D<float4> g_rt0 : register(t0);
Texture2D<float4> g_albedo : register(t1);
Texture2D<float4> g_spec : register(t2);
cbuffer L : register(b0) { float4 sun; };  // camera space
float4 main(float4 pos : SV_Position) : SV_Target0 {
    const int3 q = int3(pos.xy, 0);
    const float4 g = g_rt0.Load(q);
    const float3 a = g_albedo.Load(q).rgb;
    if (dot(g.xyz, g.xyz) < 0.25) return float4(pow(a, 1.0 / 2.2), 1);
    const float3 n = normalize(g.xyz);
    const float3 v = normalize(-float3(0, 0, g.w));
    const float nl = saturate(dot(n, sun.xyz));
    const float3 hv = normalize(sun.xyz + float3(0, 0, 1));
    const float3 s = g_spec.Load(q).rgb * pow(saturate(dot(n, hv)), 40.0) * 2.0;
    const float3 c = a * (0.45 + 1.6 * nl) + s * nl;
    return float4(pow(c / (1.0 + c), 1.0 / 2.2) * 1.15, 1);
}
)";

ID3DBlob* CompileOrDie(const char* src, const char* target, const D3D_SHADER_MACRO* defines = nullptr) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), "preview", defines, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                          0, &code, &err))) {
        printf("FAIL: shader compile\n%s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        exit(1);
    }
    if (err) err->Release();
    return code;
}

bool SavePng(const std::wstring& path, const void* rgba, UINT pitch, UINT kW = ::kW, UINT kH = ::kH) {
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    IWICStream* s = nullptr;
    IWICBitmapEncoder* e = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) &&
              SUCCEEDED(e->Initialize(s, WICBitmapEncoderNoCache)) && SUCCEEDED(e->CreateNewFrame(&fr, nullptr)) &&
              SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(kW, kH));
    // The PNG encoder takes BGRA: swap red and blue.
    std::vector<BYTE> bgra(size_t(pitch) * kH);
    memcpy(bgra.data(), rgba, bgra.size());
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) std::swap(bgra[y * pitch + x * 4], bgra[y * pitch + x * 4 + 2]);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    ok = ok && SUCCEEDED(fr->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
         SUCCEEDED(fr->WritePixels(kH, pitch, pitch * kH, bgra.data())) && SUCCEEDED(fr->Commit()) &&
         SUCCEEDED(e->Commit());
    for (IUnknown* u : {static_cast<IUnknown*>(fr), static_cast<IUnknown*>(e), static_cast<IUnknown*>(s),
                        static_cast<IUnknown*>(f)})
        if (u) u->Release();
    return ok;
}

struct Camera {
    float rows[3][4];  // camera -> unit
};

// Eye and target in the truck's frame (y up, +z rear).
Camera LookAt(float ex, float ey, float ez, float tx, float ty, float tz) {
    float f[3] = {tx - ex, ty - ey, tz - ez};
    float n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f) v /= n;
    float r[3] = {f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0};  // f x up
    n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (float& v : r) v /= n;
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    Camera c;
    for (int i = 0; i < 3; ++i) {
        c.rows[i][0] = r[i];
        c.rows[i][1] = u[i];
        c.rows[i][2] = -f[i];
    }
    c.rows[0][3] = ex, c.rows[1][3] = ey, c.rows[2][3] = ez;
    return c;
}

struct Shot {
    const char* name;
    Camera cam;
    float dust;  // the truck's
    bool truck = true, trailer = false;
    float trailer_dust = 0.f;
    ::dust::Layers layers[2] = {};  // truck, trailer
    // N12: the truck's front wheels' steering and every wheel's rolling turn (turns, as the SDK), the speed.
    float steer = 0.f, roll = 0.f, speed = 0.f;
    // N12: 1 = the left half of a pair (kept, not written), 2 = the right half: the middle half of
    // the previous view and of this one, side by side, written under this name.
    int pair = 0;
    bool flat = false;  // D11k: without the relief pass (the before of a pair)
    // N13: the levels go to the pass as before N13 (not scaled), the front wheels' deflection (m, up), the
    // dirt pass given the wheels at their config height (before N13), and the pair's two views must be the same.
    bool old_scale = false;
    float lift = 0.f;
    bool lift_before = false;
    bool same_as_left = false;
    // N19: the trailer's wheels go to the dirt pass this far inwards of their tyres (m).
    float twheel_in = 0.f;
    float twheel_reach = 0.f;  // N20: the trailer's wheels' reach as measured (dust::Constants::wheel_out)
    bool tyre_radius = false;  // N22: the truck's tyre radius given to the pass (dust::Constants::wheel_radius)
    float trailer_paint = 0.f;  // N31: the trailer's paint albedo (0: mid grey)
    float truck_paint = 0.f;    // D14: the truck's (0: dark brown-grey)
    const char* grid = nullptr;  // D14: a tile of this grid (dustpreview --ladder), at `cell`
    int cell = 0;
};

// D14 (the user, ATS 31/10: "a very big gap between the phases... dirty the truck little by little"): each layer's
// look level by level, a grid of tiles (left to right, top to bottom: kLadder), on a white truck and a light trailer
// as the user's VNL 2025 and lowboy; the rig, the cab from above and in front (the user's print), a front wheel.
constexpr float kLadder[] = {0.02f, 0.05f, 0.1f, 0.15f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.85f, 1.f};
constexpr int kLadderCols = 4, kLadderRows = 3;
void LadderShots(std::vector<Shot>& out) {
    const Camera rig = LookAt(-12.f, 4.f, -9.f, 0.f, 1.8f, 5.5f);
    const Camera top = LookAt(-3.f, 7.5f, -7.f, 0.f, 2.f, -1.2f);
    const Camera wheel = LookAt(-3.6f, 0.9f, -1.6f, -1.04f, 0.5f, -1.79f);
    const Camera tread = LookAt(-3.4f, 1.15f, -4.4f, -1.04f, 0.5f, -1.79f);  // D14c: the front tyre's tread
    const struct { const char* name; Camera cam; } views[] = {{"rig", rig}, {"top", top}, {"wheel", wheel}, {"tread", tread}};
    static std::vector<std::string> names;  // the grids' names live as long as the shots
    names.reserve(16);
    for (const char* layer : {"dust", "soot"})
        for (const auto& v : views) {
            names.push_back(std::string("ladder_") + layer + "_" + v.name);
            for (int i = 0; i < kLadderCols * kLadderRows; ++i) {
                const float level = kLadder[i];
                Shot s = {names.back().c_str(), v.cam, 0.f};
                s.trailer = true, s.trailer_paint = 0.62f, s.truck_paint = 0.8f;
                // The dust on the dark truck and the grey trailer: the preview's tone curve washes a white truck in
                // the sun out (in the game the dust shows on white, the user's prints of 31/10).
                if (layer[0] == 'd') s.dust = level, s.trailer_dust = level, s.truck_paint = 0.f, s.trailer_paint = 0.f;
                else
                    for (int u = 0; u < 2; ++u) s.layers[u].body[::dust::kSoot] = level, s.layers[u].reach[::dust::kSoot] = 0.95f;
                s.grid = s.name, s.cell = i;
                out.push_back(s);
            }
        }
}

// N5: one scene per D4 layer (truck only), then truck + trailer with several layers.
void LayerShots(std::vector<Shot>& out) {
    using namespace dust;
    const Camera front34 = LookAt(-6.5f, 2.6f, -7.5f, 0.f, 1.3f, 0.3f);
    const Camera rear34 = LookAt(-6.f, 2.4f, 8.f, 0.f, 1.2f, 0.5f);
    const Camera side_close = LookAt(-3.6f, 1.3f, -2.6f, -1.f, 0.8f, -1.4f);
    const Camera wheel_close = LookAt(-3.2f, 1.f, 0.6f, -0.9f, 0.6f, 2.1f);
    const Camera rig34 = LookAt(-12.f, 4.f, -9.f, 0.f, 1.8f, 5.5f);
    const Camera rig_rear = LookAt(-7.5f, 3.2f, 22.f, 0.f, 1.8f, 10.f);
    const Camera trailer_front = LookAt(-4.5f, 2.4f, -4.5f, 0.f, 2.f, 2.f);
    const Camera rig_far = LookAt(-28.f, 8.f, -20.f, 0.f, 1.8f, 6.f);
    // One layer of the truck's body: level, reach, road wetness.
    auto layer = [&](const char* name, const Camera& cam, int slot, float level, float reach, float wet) {
        Shot s = {name, cam, 0.f};
        s.layers[0].body[slot] = level;
        s.layers[0].reach[slot] = reach;
        s.layers[0].body[kWet] = wet;
        out.push_back(s);
        return &out.back();
    };
    layer("layer_wet_mud", front34, kWetMud, 0.75f, 0.5f, 1.f);
    layer("layer_wet_mud_rear", rear34, kWetMud, 0.75f, 0.5f, 1.f);
    layer("layer_dry_mud", front34, kDryMud, 0.75f, 0.5f, 0.f);
    layer("layer_dry_mud_close", side_close, kDryMud, 0.85f, 0.6f, 0.f);
    Shot* s = layer("layer_mud_drying", front34, kWetMud, 0.35f, 0.5f, 0.f);
    s->layers[0].body[kDryMud] = 0.4f, s->layers[0].reach[kDryMud] = 0.5f;
    layer("layer_mud_reach_low", front34, kWetMud, 0.7f, 0.f, 1.f);
    layer("layer_mud_reach_high", front34, kWetMud, 0.7f, 1.f, 1.f);
    layer("layer_spray", rear34, kSpray, 0.8f, 0.6f, 1.f);
    layer("layer_film", rear34, kFilm, 0.8f, 0.6f, 0.f);
    layer("layer_grit", front34, kGrit, 0.8f, 0.5f, 0.f);
    layer("layer_grit_close", wheel_close, kGrit, 0.85f, 0.5f, 0.f);
    layer("layer_salt", front34, kSalt, 0.8f, 0.5f, 0.f);
    layer("layer_salt_close", side_close, kSalt, 0.8f, 0.5f, 0.f);
    // N39: the road soot alone, then under the dust (the user's photo: beige dust on the cab, dark grime low down).
    // D11b: a little dust (0.02: 350 m after the wash, 100 m of it on road_dirt), then 0.1, on the wheel.
    out.push_back({"wheel_dust_002", wheel_close, 0.02f});
    out.push_back({"wheel_dust_010", wheel_close, 0.1f});
    layer("layer_soot", front34, kSoot, 0.8f, 0.4f, 0.f);
    layer("layer_soot_close", wheel_close, kSoot, 0.8f, 0.4f, 0.f);
    s = layer("layer_soot_light", front34, kSoot, 0.4f, 0.4f, 0.f);
    s = layer("mix_motorway", front34, kSoot, 0.8f, 0.4f, 0.f);
    s->dust = 0.6f;

    // Truck + trailer. set(unit, slot, body, front, reach).
    auto set = [](Shot& r, int unit, int slot, float body, float front, float reach) {
        r.layers[unit].body[slot] = body, r.layers[unit].front[slot] = front, r.layers[unit].reach[slot] = reach;
    };
    Shot rain = {"mix_rain_rig", rig34, 0.f};
    rain.trailer = true;
    set(rain, 0, kWetMud, 0.35f, 0.f, 0.4f);
    set(rain, 0, kSpray, 0.6f, 0.f, 0.6f);
    set(rain, 1, kSpray, 0.65f, 0.6f, 0.6f);
    set(rain, 1, kWetMud, 0.15f, 0.3f, 0.4f);
    rain.layers[0].body[kWet] = rain.layers[1].body[kWet] = 1.f;
    out.push_back(rain);
    Shot front = rain;
    front.name = "trailer_front", front.cam = trailer_front, front.truck = false;
    out.push_back(front);
    Shot distant = rain;
    distant.name = "mix_far", distant.cam = rig_far;
    out.push_back(distant);
    Shot dried = {"mix_dried_rig", rig34, 0.3f};
    dried.trailer = true, dried.trailer_dust = 0.3f;
    set(dried, 0, kDryMud, 0.5f, 0.f, 0.5f);
    set(dried, 0, kFilm, 0.5f, 0.f, 0.6f);
    set(dried, 1, kFilm, 0.6f, 0.5f, 0.6f);
    set(dried, 1, kDryMud, 0.15f, 0.3f, 0.4f);
    out.push_back(dried);
    Shot runs = dried;
    runs.name = "trailer_rear_runs", runs.cam = rig_rear;
    set(runs, 1, kFilm, 0.75f, 0.5f, 0.8f);
    out.push_back(runs);
    Shot winter = {"mix_winter", front34, 0.f};
    set(winter, 0, kSalt, 0.7f, 0.f, 0.5f);
    set(winter, 0, kFilm, 0.4f, 0.f, 0.6f);
    set(winter, 0, kGrit, 0.2f, 0.f, 0.4f);
    out.push_back(winter);
    Shot offroad = {"mix_offroad", front34, 0.6f};
    set(offroad, 0, kDryMud, 0.55f, 0.f, 0.6f);
    set(offroad, 0, kWetMud, 0.1f, 0.f, 0.6f);
    set(offroad, 0, kGrit, 0.3f, 0.f, 0.5f);
    out.push_back(offroad);
}

// N12: the wheel in its own frame, the high levels, salt on the tyre.
void N12Shots(std::vector<Shot>& out) {
    using namespace dust;
    // The front left wheel (x < 0) from ahead and outside; steering left turns its front outwards.
    const Camera wheel_front = LookAt(-3.4f, 1.15f, -4.4f, -1.04f, 0.5f, -1.79f);
    const Camera wheel_side = LookAt(-3.6f, 0.9f, -1.6f, -1.04f, 0.5f, -1.79f);
    const Camera level_view = LookAt(-9.f, 3.1f, -10.2f, 0.f, 1.5f, 0.4f);
    const Camera level_rear = LookAt(-8.5f, 3.2f, 9.5f, 0.f, 1.5f, 0.6f);
    auto add = [&](const Shot& s) { out.push_back(s); return &out.back(); };
    // Dust and some mud on the wheel: straight, steered 0.08 turn (~29 degrees) to the left, then
    // also rolled 1/8 turn (the pattern must turn with the tyre), then fast (the angle blurs into rings).
    Shot w = {"wheel_straight", wheel_front, 0.7f};
    w.layers[0].body[kDryMud] = 0.5f, w.layers[0].reach[kDryMud] = 0.4f;
    add(w);
    w.name = "wheel_steered", w.steer = 0.08f;
    add(w);
    w.name = "wheel_steered_rolled", w.roll = 0.125f;
    add(w);
    w.name = "wheel_side_rolled_0", w.cam = wheel_side, w.steer = 0.f, w.roll = 0.f;
    add(w);
    w.name = "wheel_side_rolled_45", w.roll = 0.125f;
    add(w);
    w.name = "wheel_side_fast", w.speed = 80.f;
    add(w);
    // Salt on the tyre at the top level, steered (the D4a test's view): grey powder, no tide lines.
    Shot salt = {"wheel_salt", wheel_front, 0.f};
    salt.steer = 0.08f;
    salt.layers[0].body[kSalt] = 1.f, salt.layers[0].reach[kSalt] = 0.6f;
    add(salt);
    salt.name = "wheel_salt_side", salt.cam = wheel_side, salt.steer = 0.f;
    add(salt);
    // N13: level 1 now looks as 0.8 did: old 0.8 | new 1 side by side (the same image, checked), for the
    // dust (front and rear), the mud (reach 1: ~2.8 m) and everything; then 0.9 | 1 on the new scale.
    auto level = [&](const char* name, const Camera& cam, float lv, int pair, auto&& fill, bool old_scale = false) {
        Shot s = {name, cam, 0.f};
        s.pair = pair;
        s.old_scale = old_scale;
        s.same_as_left = pair == 2 && std::strstr(name, "old_100new") != nullptr;
        fill(s, lv);
        add(s);
    };
    auto dust_only = [](Shot& s, float lv) { s.dust = lv; };
    // The levels of mud add up (wet + dry): each at half, the sum at the level.
    auto mud = [](Shot& s, float lv) {
        s.layers[0].body[kWetMud] = lv * 0.5f, s.layers[0].body[kDryMud] = lv * 0.5f;
        s.layers[0].reach[kWetMud] = s.layers[0].reach[kDryMud] = 1.f;
    };
    auto all = [](Shot& s, float lv) {
        s.dust = lv;
        for (int k : {kDryMud, kFilm, kGrit}) s.layers[0].body[k] = lv, s.layers[0].reach[k] = 0.8f;
    };
    // D11d: the dust's own scale is lower (dust::kDustScale): its level 1 is the old 0.68.
    for (const auto& v : {std::make_pair("level_dust_068old_100new", 0), std::make_pair("level_dust_rear_068old_100new", 1)}) {
        const Camera& cam = v.second ? level_rear : level_view;
        level("", cam, ::dust::kDustScale, 1, dust_only, true);
        level(v.first, cam, 1.f, 2, dust_only);
    }
    level("", level_view, 0.8f, 1, mud, true);
    level("level_mud_080old_100new", level_view, 1.f, 2, mud);
    level("", level_view, 0.8f, 1, all, true);
    level("level_all_old_new", level_view, 1.f, 2, all);  // D11d: the dust 0.68, the rest 0.8: not the same
    level("", level_view, 0.9f, 1, dust_only);
    level("level_dust_090_100", level_view, 1.f, 2, dust_only);
    level("", level_view, 0.9f, 1, all);
    level("level_all_090_100", level_view, 1.f, 2, all);
    // N13: braking, the front wheels 6 cm up towards the chassis: at rest | raised (the dirt rides on the
    // tyre, the arches keep theirs), then raised with the dirt at the config height (before N13) | N13.
    Shot d = {"", wheel_side, 0.7f};
    d.layers[0].body[kDryMud] = 0.5f, d.layers[0].reach[kDryMud] = 0.4f;
    d.roll = 0.125f;
    d.pair = 1;
    add(d);
    d.name = "wheel_deflected_0_6cm", d.lift = 0.06f, d.pair = 2;
    add(d);
    d.name = "", d.lift_before = true, d.pair = 1;
    add(d);
    d.name = "wheel_deflected_before_after", d.lift_before = false, d.pair = 2;
    add(d);
    // N19: the trailer's rear left wheel from the side, its position 0.2 m inwards of the tyre (the game's
    // trailers: the axle's end), not rolled | rolled 1/8 turn: the outer side's pattern turns with the tyre.
    Shot t = {"", LookAt(-3.9f, 0.9f, 11.2f, -1.02f, 0.5f, 12.1f), 0.f};
    t.truck = false, t.trailer = true, t.trailer_dust = 0.7f;
    t.layers[1].body[kDryMud] = 0.5f, t.layers[1].reach[kDryMud] = 0.4f;
    t.twheel_in = 0.2f;
    t.pair = 1;
    add(t);
    t.name = "trailer_wheel_offset_rolled_0_45", t.roll = 0.125f, t.pair = 2;
    add(t);
    // N20: 0.5 m inwards (a pair of tyres on an axle's end, say): past N19's band; its reach measured, inside.
    t.name = "", t.twheel_in = 0.5f, t.pair = 1;
    add(t);
    t.name = "trailer_wheel_deep_unmeasured_measured", t.twheel_reach = 0.5f, t.pair = 2;
    add(t);
    // N22: the rear left wheels from below, up at the mudguard (its underside 0.55 m over the wheels' centre, inside
    // N17's 1.12 x 0.50 m): without | with the tyre's radius (0.50 m, the preview's tyres).
    Shot f = {"", LookAt(-2.3f, 0.2f, 0.2f, -0.95f, 1.f, 2.1f), 0.7f};
    f.layers[0].body[kDryMud] = 0.5f, f.layers[0].reach[kDryMud] = 0.4f;
    f.roll = 0.125f;
    f.pair = 1;
    add(f);
    f.name = "wheel_fender_no_radius_radius", f.tyre_radius = true, f.pair = 2;
    add(f);
}

// N31: a unit's layers as the plugin gives them to the pass (plugin.cpp ShownLayers), with the tread's wear.
::dust::Layers Shown(const dirt::Model& m, int unit) {
    ::dust::Layers out = {};
    const dirt::Coat c = m.Layers(unit);
    for (int k = dirt::kWetMud; k <= dirt::kSalt; ++k) {
        out.body[k - dirt::kWetMud] = c.body[k];
        out.front[k - dirt::kWetMud] = c.front[k];
        out.reach[k - dirt::kWetMud] = c.reach[k];
    }
    out.body[::dust::kWet] = m.Wetness();
    out.body[::dust::kTread] = m.TreadWear(unit);
    return out;
}

// N31: the tread through the model (dirt_model): 3 km of wet dirt, the road dry again, a few km of dry dirt
// (dust), then | after 2 km of dry asphalt; the same with a wet road (the spray stays on the tread).
void N31Shots(std::vector<Shot>& out) {
    const Camera wheel_front = LookAt(-3.4f, 1.15f, -4.4f, -1.04f, 0.5f, -1.79f);
    const Camera wheel_side = LookAt(-3.6f, 0.9f, -1.6f, -1.04f, 0.5f, -1.79f);
    const Camera rig = LookAt(-9.f, 2.2f, -6.f, 0.f, 0.8f, 3.f);
    dirt::Ground g[6];
    bool og[6];
    auto drive = [&](dirt::Model& m, double km, const char* substance, float kmh) {
        for (int w = 0; w < 6; ++w) g[w] = dirt::GroundOf(substance), og[w] = true;
        for (int i = 0; i < static_cast<int>(km / 0.1 + 0.5); ++i) m.Drive(0, 0.1, g, og, 6, kmh);
    };
    auto weather = [](dirt::Model& m, int minutes, bool rain) {
        for (int i = 0; i < minutes; ++i) m.Weather(1.0, rain);
    };
    dirt::Model m;
    weather(m, 60, true);
    drive(m, 3, "dirt", 50.f);
    weather(m, 90, false);
    drive(m, 2, "dirt", 50.f);
    dirt::Model wet = m;
    auto shot = [&](const char* name, const Camera& cam, const dirt::Model& from, int pair) {
        Shot s = {name, cam, from.Level(0)};
        s.layers[0] = Shown(from, 0);
        s.roll = 0.125f;
        s.pair = pair;
        out.push_back(s);
    };
    const dirt::Model muddy = m;
    drive(m, 2, "road", 80.f);
    printf("N31: tread wear after the mud %.3f, after 2 km of asphalt %.3f\n", muddy.TreadWear(0), m.TreadWear(0));
    shot("", wheel_front, muddy, 1);
    shot("tread_mud_asphalt2km", wheel_front, m, 2);
    shot("", wheel_side, muddy, 1);
    shot("tread_mud_asphalt2km_side", wheel_side, m, 2);
    shot("", rig, muddy, 1);
    shot("tread_mud_asphalt2km_rig", rig, m, 2);
    weather(wet, 30, true);
    const dirt::Model wet_mud = wet;
    drive(wet, 2, "road", 80.f);
    shot("", wheel_front, wet_mud, 1);
    shot("tread_mud_wet_asphalt2km", wheel_front, wet, 2);
}

// D11k (N44): up close, as on foot next to the truck (~0.6 m): the dust's grains and grit, the soot with oil low on the
// chassis (drops), the dry and the wet mud with their bits of grit and their relief; each also flat (no relief pass).
void CloseShots(std::vector<Shot>& out) {
    // On the sunlit side (+x; the preview's sun comes from +x, up and ahead): the relief shows in the direct light.
    const Camera cab = LookAt(1.85f, 1.6f, -1.9f, 1.2f, 1.45f, -1.7f);
    const Camera low = LookAt(1.8f, 0.9f, -0.6f, 1.15f, 0.7f, -0.3f);
    auto add = [&](const char* name, const Camera& cam, float dust, int slot, float level, float wet) {
        Shot k = {name, cam, dust};
        if (slot >= 0) {
            k.layers[0].body[slot] = level, k.layers[0].reach[slot] = 0.8f;
            k.layers[0].body[::dust::kWet] = wet;
        }
        out.push_back(k);
        std::string flat_name = std::string(name) + "_flat";
        static std::vector<std::string> names;  // the shots keep a pointer to the name
        names.reserve(16);
        names.push_back(flat_name);
        k.name = names.back().c_str();
        k.flat = true;
        out.push_back(k);
    };
    add("close_dust", cab, 1.f, -1, 0.f, 0.f);
    add("close_soot_chassis", low, 0.f, ::dust::kSoot, 0.9f, 0.f);
    add("close_dry_mud", low, 0.f, ::dust::kDryMud, 0.85f, 0.f);
    add("close_wet_mud", low, 0.f, ::dust::kWetMud, 0.85f, 1.f);
}

// N31: the trailer's side and the cab at a medium and the top dust level (the ATS test of 28/10: the side of
// the trailer almost clean even at the top), on the mid grey trailer and on a light one (aluminium, white).
void SideShots(std::vector<Shot>& out) {
    const Camera side = LookAt(-12.5f, 2.6f, 3.5f, 0.f, 2.1f, 6.5f);
    const Camera rear34 = LookAt(-8.f, 3.f, 19.f, 0.f, 2.f, 9.f);
    const Camera rig34 = LookAt(-12.f, 4.f, -9.f, 0.f, 1.8f, 5.5f);
    for (const float level : {0.5f, 1.f}) {
        const bool top = level == 1.f;
        Shot s = {top ? "side_rig_100" : "side_rig_050", side, level};
        s.trailer = true, s.trailer_dust = level;
        out.push_back(s);
        s.name = top ? "side_trailer_rear_100" : "side_trailer_rear_050", s.cam = rear34;
        out.push_back(s);
        s.name = top ? "side_cab_rig34_100" : "side_cab_rig34_050", s.cam = rig34;
        out.push_back(s);
        s.name = top ? "side_light_rig_100" : "side_light_rig_050", s.cam = side, s.trailer_paint = 0.62f;
        out.push_back(s);
        // D11f: the soot alone on the light rig: its coat as the dust's (the trailer's side too, the user 29/10).
        Shot k = {top ? "soot_side_light_rig_100" : "soot_side_light_rig_050", side, 0.f};
        k.trailer = true, k.trailer_paint = 0.62f;
        for (int u = 0; u < 2; ++u) k.layers[u].body[::dust::kSoot] = level, k.layers[u].reach[::dust::kSoot] = 0.4f;
        out.push_back(k);
    }
}

// Bench (dustpreview <out dir> --bench, the user 30/10: "less heavy without losing quality"): the GPU time of the
// DLL's pass, part by part, at the game's G-buffer size (3440x2880 in the ATS log of 30/10), from timestamp queries:
// rt3's and rt0's copies, then each class's draws as tint::RunPass makes them, and a draw whose stencil bit no pixel
// has (a class marked but off screen). The number of pixels each unit's draws shade, from an occlusion query.
int Bench(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    constexpr UINT W = 3440, H = 2880;
    constexpr float kBTanY = 0.36f, kBTanX = kBTanY * 3440.f / 1440.f;  // a 21:9 view, rendered at twice its height
    auto make = [&](DXGI_FORMAT fmt, UINT bind, ID3D11Texture2D** t, ID3D11RenderTargetView** rtv,
                    ID3D11ShaderResourceView** srv, ID3D11DepthStencilView** dsv) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = W, td.Height = H, td.MipLevels = td.ArraySize = 1, td.Format = fmt, td.SampleDesc.Count = 1;
        td.BindFlags = bind;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, t))) return false;
        if (rtv && FAILED(dev->CreateRenderTargetView(*t, nullptr, rtv))) return false;
        if (srv && FAILED(dev->CreateShaderResourceView(*t, nullptr, srv))) return false;
        if (dsv && FAILED(dev->CreateDepthStencilView(*t, nullptr, dsv))) return false;
        return true;
    };
    const UINT rs = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *t_rt0, *t_alb, *t_spec, *t_rt3, *t_rt0c, *t_rt3c, *t_d;
    ID3D11RenderTargetView *v_rt0, *v_alb, *v_spec, *v_rt3;
    ID3D11ShaderResourceView *s_rt0, *s_rt0c, *s_rt3c;
    ID3D11DepthStencilView* dsv;
    if (!make(DXGI_FORMAT_R16G16B16A16_FLOAT, rs, &t_rt0, &v_rt0, &s_rt0, nullptr) ||
        !make(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, &t_alb, &v_alb, nullptr, nullptr) ||
        !make(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET, &t_spec, &v_spec, nullptr, nullptr) ||
        !make(DXGI_FORMAT_R16G16B16A16_UINT, D3D11_BIND_RENDER_TARGET, &t_rt3, &v_rt3, nullptr, nullptr) ||
        !make(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, &t_rt0c, nullptr, &s_rt0c, nullptr) ||
        !make(DXGI_FORMAT_R16G16B16A16_UINT, D3D11_BIND_SHADER_RESOURCE, &t_rt3c, nullptr, &s_rt3c, nullptr) ||
        !make(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D11_BIND_DEPTH_STENCIL, &t_d, nullptr, nullptr, &dsv))
        return Fail("bench targets");
    const D3D_SHADER_MACRO mask_def[] = {{"MASK", "1"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO spec_def[] = {{"SPEC", "1"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO refl_def[] = {{"REFL", "1"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO norm_def[] = {{"NORM", "1"}, {nullptr, nullptr}};
    auto ps = [&](const char* src, const D3D_SHADER_MACRO* def) {
        ID3DBlob* b = CompileOrDie(src, "ps_5_0", def);
        ID3D11PixelShader* p = nullptr;
        dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &p);
        b->Release();
        return p;
    };
    ID3DBlob* b_vs = CompileOrDie(kFullVs, "vs_5_0");
    ID3D11VertexShader* vs = nullptr;
    dev->CreateVertexShader(b_vs->GetBufferPointer(), b_vs->GetBufferSize(), nullptr, &vs);
    ID3D11PixelShader* scene = ps(kScenePs, nullptr);
    ID3D11PixelShader* mask = ps(kScenePs, mask_def);
    ID3D11PixelShader* p_alb = ps(dust::kPixelShader, nullptr);
    ID3D11PixelShader* p_spec = ps(dust::kPixelShader, spec_def);
    ID3D11PixelShader* p_refl = ps(dust::kPixelShader, refl_def);
    ID3D11PixelShader* p_norm = ps(dust::kPixelShader, norm_def);
    const D3D_SHADER_MACRO pack_def[] = {{"PACK", "1"}, {nullptr, nullptr}};
    ID3D11PixelShader* p_pack = ps(dust::kPixelShader, pack_def);
    if (!vs || !scene || !mask || !p_alb || !p_spec || !p_refl || !p_norm || !p_pack) return Fail("bench shaders");
    ID3D11Texture2D* t_pack;
    ID3D11RenderTargetView* v_pack;
    ID3D11ShaderResourceView* s_pack;
    if (!make(DXGI_FORMAT_R32G32B32A32_UINT, rs, &t_pack, &v_pack, &s_pack, nullptr)) return Fail("bench pack");
    ID3D11DepthStencilState *mark = nullptr, *keep = nullptr, *off = nullptr;
    {
        D3D11_DEPTH_STENCIL_DESC d = {};
        d.DepthEnable = FALSE, d.StencilEnable = TRUE, d.StencilReadMask = 0x70, d.StencilWriteMask = 0x70;
        d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
        d.BackFace = d.FrontFace;
        dev->CreateDepthStencilState(&d, &mark);
        d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_EQUAL};
        d.BackFace = d.FrontFace;
        dev->CreateDepthStencilState(&d, &keep);
        d.StencilEnable = FALSE;
        dev->CreateDepthStencilState(&d, &off);
    }
    ID3D11BlendState *blend = nullptr, *no_color = nullptr;
    {
        D3D11_BLEND_DESC b = {};
        auto& t = b.RenderTarget[0];
        t.BlendEnable = TRUE, t.SrcBlend = D3D11_BLEND_SRC_ALPHA, t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        t.BlendOp = D3D11_BLEND_OP_ADD, t.SrcBlendAlpha = D3D11_BLEND_ZERO, t.DestBlendAlpha = D3D11_BLEND_ONE;
        t.BlendOpAlpha = D3D11_BLEND_OP_ADD, t.RenderTargetWriteMask = 7;
        dev->CreateBlendState(&b, &blend);
        D3D11_BLEND_DESC n = {};
        n.RenderTarget[0].RenderTargetWriteMask = 0;
        dev->CreateBlendState(&n, &no_color);
    }
    struct SceneCb {
        float c[3][4];
        float view[4];
        float wheel[6][4];
        float opts[4];
        float twheel[6][4];
        float steer[6][4];
        float paint[4];  // D14
    };
    auto cbuffer = [&](UINT size) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = (size + 15) & ~15u, d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Buffer* b = nullptr;
        dev->CreateBuffer(&d, nullptr, &b);
        return b;
    };
    ID3D11Buffer* scene_cb = cbuffer(sizeof(SceneCb));
    ID3D11Buffer* dust_cb[2] = {cbuffer(sizeof(dust::Constants)), cbuffer(sizeof(dust::Constants))};

    struct Case {
        const char* name;
        Camera cam;
        bool trailer;
        float dust;
        float mud, soot;  // wet and dry mud, soot (reach 0.5)
        bool relief = true;
        float spray = 0.f;  // D13f: the spray (reach 0.2)
    };
    const Camera side_close = LookAt(-3.6f, 1.3f, -2.6f, -1.f, 0.8f, -1.4f);
    const Camera rig34 = LookAt(-12.f, 4.f, -9.f, 0.f, 1.8f, 5.5f);
    const Camera rig_far = LookAt(-28.f, 8.f, -20.f, 0.f, 1.8f, 6.f);
    const Case cases[] = {
        {"close, washed (dust 0.02)", side_close, false, 0.02f, 0.f, 0.01f},
        {"close, dirty (dust 1, mud, soot)", side_close, false, 1.f, 0.6f, 0.6f},
        {"close, dirty, relief off", side_close, false, 1.f, 0.6f, 0.6f, false},
        {"rig, washed", rig34, true, 0.02f, 0.f, 0.01f},
        {"rig, dirty", rig34, true, 1.f, 0.6f, 0.6f},
        {"rig far, dirty", rig_far, true, 1.f, 0.6f, 0.6f},
        // D13f: the ATS's VNL and lowboy of 30/10 (dust 0.11, soot 0.24, a trace of spray).
        {"close, ATS 30/10", side_close, false, 0.11f, 0.f, 0.24f, true, 0.0016f},
        {"rig, ATS 30/10", rig34, true, 0.11f, 0.f, 0.24f, true, 0.0016f},
    };
    enum { kTs = 9 };
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* ts[kTs] = {};
    ID3D11Query* occ[2] = {};
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    dev->CreateQuery(&qd, &disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    for (auto& q : ts) dev->CreateQuery(&qd, &q);
    qd.Query = D3D11_QUERY_OCCLUSION;
    for (auto& q : occ) dev->CreateQuery(&qd, &q);
    const D3D11_VIEWPORT vp = {0, 0, float(W), float(H), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    printf("bench %ux%u (ms per frame; mean of 40 frames after 5)\n", W, H);
    printf("%-34s %8s %7s %7s %7s %7s %7s %7s %7s %7s %7s\n", "case", "pixels", "cp rt3", "cp rt0", "pack", "albedo",
           "spec", "refl", "relief", "empty", "total");
    for (const Case& k : cases) {
        SceneCb sc = {};
        memcpy(sc.c, k.cam.rows, sizeof(sc.c));
        sc.view[0] = kBTanX, sc.view[1] = kBTanY, sc.view[2] = float(W), sc.view[3] = float(H);
        for (int w = 0; w < 6; ++w) {
            for (int j = 0; j < 3; ++j) sc.wheel[w][j] = kTruckWheels[w][j], sc.twheel[w][j] = kTrailerWheels[w][j];
            sc.wheel[w][3] = kTruckWheels[w][1], sc.twheel[w][3] = kTrailerWheels[w][1];
            sc.steer[w][0] = 1.f;
        }
        sc.opts[0] = k.trailer ? 1.f : 0.f, sc.opts[1] = 1.f;
        ctx->UpdateSubresource(scene_cb, 0, nullptr, &sc, 0, 0);
        const int units = k.trailer ? 2 : 1;
        for (int u = 0; u < units; ++u) {
            dust::Constants dc = {};
            dc.vp[2] = 1.f / W, dc.vp[3] = 1.f / H;
            dc.proj[0][0] = 1.f / kBTanX, dc.proj[1][1] = 1.f / kBTanY, dc.proj[2][2] = -1.f;
            memcpy(dc.unit, k.cam.rows, sizeof(dc.unit));
            dust::Layers l = {};
            for (const int s : {dust::kWetMud, dust::kDryMud}) l.body[s] = k.mud * 0.5f, l.reach[s] = 0.5f;
            l.body[dust::kSoot] = k.soot, l.reach[dust::kSoot] = 0.5f;
            l.body[dust::kSpray] = k.spray, l.reach[dust::kSpray] = 0.2f;
            dust::Levels(dc, k.dust, l);
            dc.params[2] = 6.f, dc.params[3] = 1.f;
            memcpy(dc.wheel, u ? sc.twheel : sc.wheel, sizeof(sc.wheel));
            const float roll[6] = {};
            dust::Wheels(dc, nullptr, roll, 6, 0.f);
            dc.spin[2] = u ? 1.f : 0.f;
            dc.spin[3] = k.relief ? 1.f : 0.f;  // the relief
            ctx->UpdateSubresource(dust_cb[u], 0, nullptr, &dc, 0, 0);
        }
        double sum[kTs - 1] = {};
        UINT64 pixels = 0;
        int got = 0;
        for (int frame = 0; frame < 45; ++frame) {
            // The G-buffer and the units' bits, again every frame (the relief rewrites rt0).
            ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_STENCIL, 1.f, 0);
            ID3D11RenderTargetView* gb[3] = {v_rt0, v_alb, v_spec};
            ctx->OMSetRenderTargets(3, gb, dsv);
            ctx->OMSetDepthStencilState(off, 0);
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx->PSSetConstantBuffers(0, 1, &scene_cb);
            ctx->PSSetShader(scene, nullptr, 0);
            ctx->Draw(3, 0);
            ctx->OMSetBlendState(no_color, nullptr, 0xFFFFFFFF);
            ctx->PSSetShader(mask, nullptr, 0);
            for (int u = 0; u < units; ++u) {
                sc.opts[2] = float(u);
                ctx->UpdateSubresource(scene_cb, 0, nullptr, &sc, 0, 0);
                ctx->OMSetDepthStencilState(mark, u ? 0x20 : 0x10);
                ctx->Draw(3, 0);
            }
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            const bool timed = frame >= 5;
            if (timed) ctx->Begin(disjoint);
            if (timed) ctx->End(ts[0]);
            ctx->CopyResource(t_rt3c, t_rt3);
            if (timed) ctx->End(ts[1]);
            // D13f: rt0 is not copied any more (the column stays, ~0).
            if (timed) ctx->End(ts[2]);
            // Each stage over the units (the DLL runs the stages per class; the GPU time is the same sum).
            for (int stage = 0; stage < 5; ++stage) {
                for (int u = 0; u < units; ++u) {
                    ctx->PSSetConstantBuffers(0, 1, &dust_cb[u]);
                    ctx->OMSetDepthStencilState(keep, u ? 0x20 : 0x10);
                    ID3D11ShaderResourceView* none2[3] = {};
                    ctx->PSSetShaderResources(0, 3, none2);
                    if (stage > 0) ctx->PSSetShaderResources(2, 1, &s_pack);
                    if (stage == 0) {  // D11m: the pack, the dirt's only full shading
                        ctx->OMSetRenderTargets(1, &v_pack, dsv);
                        ctx->PSSetShaderResources(0, 1, &s_rt0);
                        ctx->PSSetShader(p_pack, nullptr, 0);
                        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                        if (timed && frame == 5) ctx->Begin(occ[u]);
                    } else if (stage == 1) {
                        ctx->OMSetRenderTargets(1, &v_alb, dsv);
                        ctx->PSSetShaderResources(0, 1, &s_rt0);
                        ctx->PSSetShader(p_alb, nullptr, 0);
                        ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
                    } else if (stage == 2) {
                        ctx->OMSetRenderTargets(1, &v_spec, dsv);
                        ctx->PSSetShaderResources(0, 1, &s_rt0);
                        ctx->PSSetShader(p_spec, nullptr, 0);
                    } else if (stage == 3) {
                        ID3D11ShaderResourceView* v2[2] = {s_rt0, s_rt3c};
                        ctx->OMSetRenderTargets(1, &v_rt3, dsv);
                        ctx->PSSetShaderResources(0, 2, v2);
                        ctx->PSSetShader(p_refl, nullptr, 0);
                        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                    } else {  // D13f: the pack's normals through the albedo's blend, rt0 not read
                        ctx->OMSetRenderTargets(1, &v_rt0, dsv);
                        ctx->PSSetShader(p_norm, nullptr, 0);
                        ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
                    }
                    ctx->Draw(3, 0);
                    if (stage == 0 && timed && frame == 5) ctx->End(occ[u]);
                }
                if (timed) ctx->End(ts[3 + stage]);
            }
            // A class marked, none of its pixels in this view: its bit is on no pixel.
            ctx->OMSetRenderTargets(1, &v_alb, dsv);
            ctx->PSSetShaderResources(0, 1, &s_rt0);
            ctx->PSSetShader(p_alb, nullptr, 0);
            ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
            ctx->OMSetDepthStencilState(keep, 0x40);
            ctx->Draw(3, 0);
            if (!timed) continue;
            ctx->End(ts[8]);
            ctx->End(disjoint);
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
            while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) == S_FALSE) {
            }
            UINT64 t[kTs] = {};
            for (int i = 0; i < kTs; ++i)
                while (ctx->GetData(ts[i], &t[i], sizeof(UINT64), 0) == S_FALSE) {
                }
            if (frame == 5)
                for (int u = 0; u < units; ++u) {
                    UINT64 n = 0;
                    while (ctx->GetData(occ[u], &n, sizeof(n), 0) == S_FALSE) {
                    }
                    pixels += n;
                }
            if (dj.Disjoint || !dj.Frequency) continue;
            for (int i = 0; i < kTs - 1; ++i) sum[i] += double(t[i + 1] - t[i]) * 1000.0 / double(dj.Frequency);
            ++got;
        }
        if (!got) return Fail("bench: no timing");
        double total = 0;
        for (double& v : sum) v /= got, total += v;
        printf("%-34s %7.1f%% %7.3f %7.3f %7.3f %7.3f %7.3f %7.3f %7.3f %7.3f\n", k.name, 100.0 * double(pixels) / (W * H),
               sum[0], sum[1], sum[2], sum[3], sum[4], sum[5], sum[6], total);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Fail("usage: dustpreview <out dir>");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    // DUSTPREVIEW_WARP=1 uses the software rasterizer (CI machines have no GPU); so does a failed hardware device.
    const bool warp = getenv("DUSTPREVIEW_WARP") != nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr,
                                   0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr) && !warp)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr,
                               &ctx);
    if (FAILED(hr)) return Fail("D3D11CreateDevice", hr);
    ReportCbRange(dev, ctx);
    if (argc > 2 && strcmp(argv[2], "--bench") == 0) return Bench(dev, ctx);

    auto target = [&](DXGI_FORMAT fmt, ID3D11Texture2D** tex, ID3D11RenderTargetView** rtv, ID3D11ShaderResourceView** srv) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = kW, td.Height = kH, td.MipLevels = td.ArraySize = 1, td.Format = fmt, td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | (srv ? D3D11_BIND_SHADER_RESOURCE : 0);
        return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, tex)) && SUCCEEDED(dev->CreateRenderTargetView(*tex, nullptr, rtv)) &&
               (!srv || SUCCEEDED(dev->CreateShaderResourceView(*tex, nullptr, srv)));
    };
    ID3D11Texture2D* tex[4] = {};
    ID3D11RenderTargetView* rtv[4] = {};
    ID3D11ShaderResourceView* srv[3] = {};
    for (int i = 0; i < 3; ++i)
        if (!target(DXGI_FORMAT_R16G16B16A16_FLOAT, &tex[i], &rtv[i], &srv[i])) return Fail("G-buffer target");
    if (!target(DXGI_FORMAT_R8G8B8A8_UNORM, &tex[3], &rtv[3], nullptr)) return Fail("output target");
    // D11k: rt0's copy for the relief pass (rt0 is its target), as the DLL's.
    ID3D11Texture2D* rt0_copy = nullptr;
    ID3D11ShaderResourceView* rt0_copy_srv = nullptr;
    {
        D3D11_TEXTURE2D_DESC td = {};
        tex[0]->GetDesc(&td);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &rt0_copy)) ||
            FAILED(dev->CreateShaderResourceView(rt0_copy, nullptr, &rt0_copy_srv)))
            return Fail("rt0 copy");
    }
    // D11m: the pack (uint4), which the albedo, specular and relief draws read, as the DLL's.
    ID3D11Texture2D* pack_tex = nullptr;
    ID3D11RenderTargetView* pack_rtv = nullptr;
    ID3D11ShaderResourceView* pack_srv = nullptr;
    if (!target(DXGI_FORMAT_R32G32B32A32_UINT, &pack_tex, &pack_rtv, &pack_srv)) return Fail("pack target");
    ID3D11Texture2D* dtex = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = kW, td.Height = kH, td.MipLevels = td.ArraySize = 1, td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        td.SampleDesc.Count = 1, td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &dtex)) || FAILED(dev->CreateDepthStencilView(dtex, nullptr, &dsv)))
            return Fail("depth");
    }
    ID3D11Texture2D* staging = nullptr;
    {
        D3D11_TEXTURE2D_DESC td = {};
        tex[3]->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) return Fail("staging");
    }

    const D3D_SHADER_MACRO mask_def[] = {{"MASK", "1"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO spec_def[] = {{"SPEC", "1"}, {nullptr, nullptr}};
    ID3DBlob* b_vs = CompileOrDie(kFullVs, "vs_5_0");
    ID3DBlob* b_scene = CompileOrDie(kScenePs, "ps_5_0");
    ID3DBlob* b_mask = CompileOrDie(kScenePs, "ps_5_0", mask_def);
    ID3DBlob* b_dust = CompileOrDie(dust::kPixelShader, "ps_5_0");
    ID3DBlob* b_dust_spec = CompileOrDie(dust::kPixelShader, "ps_5_0", spec_def);
    // N25: the rt3 variant is not drawn here (the scene has no emittance), only compiled.
    const D3D_SHADER_MACRO refl_def[] = {{"REFL", "1"}, {nullptr, nullptr}};
    CompileOrDie(dust::kPixelShader, "ps_5_0", refl_def)->Release();
    const D3D_SHADER_MACRO norm_def[] = {{"NORM", "1"}, {nullptr, nullptr}};  // D11k: the relief, drawn
    ID3DBlob* b_dust_norm = CompileOrDie(dust::kPixelShader, "ps_5_0", norm_def);
    ID3D11PixelShader* dust_norm = nullptr;
    dev->CreatePixelShader(b_dust_norm->GetBufferPointer(), b_dust_norm->GetBufferSize(), nullptr, &dust_norm);
    if (!dust_norm) return Fail("relief shader");
    const D3D_SHADER_MACRO pack_def[] = {{"PACK", "1"}, {nullptr, nullptr}};  // D11m
    ID3DBlob* b_dust_pack = CompileOrDie(dust::kPixelShader, "ps_5_0", pack_def);
    ID3D11PixelShader* dust_pack = nullptr;
    dev->CreatePixelShader(b_dust_pack->GetBufferPointer(), b_dust_pack->GetBufferSize(), nullptr, &dust_pack);
    if (!dust_pack) return Fail("pack shader");
    ID3DBlob* b_light = CompileOrDie(kLightPs, "ps_5_0");
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader *scene = nullptr, *mask = nullptr, *dust_ps = nullptr, *dust_spec = nullptr, *light = nullptr;
    dev->CreateVertexShader(b_vs->GetBufferPointer(), b_vs->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(b_scene->GetBufferPointer(), b_scene->GetBufferSize(), nullptr, &scene);
    dev->CreatePixelShader(b_mask->GetBufferPointer(), b_mask->GetBufferSize(), nullptr, &mask);
    dev->CreatePixelShader(b_dust->GetBufferPointer(), b_dust->GetBufferSize(), nullptr, &dust_ps);
    dev->CreatePixelShader(b_dust_spec->GetBufferPointer(), b_dust_spec->GetBufferSize(), nullptr, &dust_spec);
    dev->CreatePixelShader(b_light->GetBufferPointer(), b_light->GetBufferSize(), nullptr, &light);
    if (!vs || !scene || !mask || !dust_ps || !dust_spec || !light) return Fail("shaders");

    // The DLL's states: the dust blends by its alpha into one target, stencil EQUAL 0x10.
    ID3D11DepthStencilState *mark = nullptr, *test = nullptr, *off = nullptr;
    {
        D3D11_DEPTH_STENCIL_DESC d = {};
        d.DepthEnable = FALSE, d.StencilEnable = TRUE, d.StencilReadMask = 0x30, d.StencilWriteMask = 0x30;
        d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
        d.BackFace = d.FrontFace;
        dev->CreateDepthStencilState(&d, &mark);
        d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_EQUAL};
        d.BackFace = d.FrontFace;
        dev->CreateDepthStencilState(&d, &test);
        d.StencilEnable = FALSE;
        dev->CreateDepthStencilState(&d, &off);
    }
    ID3D11BlendState *blend = nullptr, *no_color = nullptr;
    {
        D3D11_BLEND_DESC b = {};
        auto& t = b.RenderTarget[0];
        t.BlendEnable = TRUE, t.SrcBlend = D3D11_BLEND_SRC_ALPHA, t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        t.BlendOp = D3D11_BLEND_OP_ADD, t.SrcBlendAlpha = D3D11_BLEND_ZERO, t.DestBlendAlpha = D3D11_BLEND_ONE;
        t.BlendOpAlpha = D3D11_BLEND_OP_ADD, t.RenderTargetWriteMask = 7;
        dev->CreateBlendState(&b, &blend);
        D3D11_BLEND_DESC n = {};
        n.RenderTarget[0].RenderTargetWriteMask = 0;
        dev->CreateBlendState(&n, &no_color);
    }
    auto cbuffer = [&](UINT size) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = (size + 15) & ~15u, d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Buffer* b = nullptr;
        dev->CreateBuffer(&d, nullptr, &b);
        return b;
    };
    struct SceneCb {
        float c[3][4];
        float view[4];
        float wheel[6][4];
        float opts[4];
        float twheel[6][4];
        float steer[6][4];
        float paint[4];  // D14
    };
    ID3D11Buffer* scene_cb = cbuffer(sizeof(SceneCb));
    ID3D11Buffer* dust_cb = cbuffer(sizeof(dust::Constants));
    ID3D11Buffer* light_cb = cbuffer(16);

    std::vector<Shot> shots = {
        {"front34", LookAt(-6.5f, 2.6f, -7.5f, 0.f, 1.3f, 0.3f), 0.7f},
        {"front34_light", LookAt(-6.5f, 2.6f, -7.5f, 0.f, 1.3f, 0.3f), 0.3f},
        {"top_rear", LookAt(-4.5f, 8.5f, 7.5f, 0.f, 1.2f, 0.f), 0.7f},
        {"side_close", LookAt(-3.6f, 1.3f, -2.6f, -1.f, 0.8f, -1.4f), 0.7f},
        {"far", LookAt(-19.5f, 6.f, -22.5f, 0.f, 1.3f, 0.3f), 0.7f},
    };
    const bool ladder = argc > 2 && strcmp(argv[2], "--ladder") == 0;  // D14: only the level grids
    if (ladder) {
        shots.clear();
        LadderShots(shots);
    } else {
        LayerShots(shots);
        N12Shots(shots);
        N31Shots(shots);
        SideShots(shots);
        CloseShots(shots);
    }
    // D14: the grids, each tile the view at half size, 2 white pixels between tiles.
    constexpr UINT kTileW = kW / 2, kTileH = kH / 2, kGridW = kTileW * kLadderCols, kGridH = kTileH * kLadderRows;
    std::vector<std::pair<std::string, std::vector<BYTE>>> grids;
    D3D11_VIEWPORT vp = {0, 0, float(kW), float(kH), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    std::vector<BYTE> pair_left;  // N12: the left view of a pair
    for (const Shot& s : shots) {
        SceneCb sc = {};
        memcpy(sc.c, s.cam.rows, sizeof(sc.c));
        sc.view[0] = kTanX, sc.view[1] = kTanY, sc.view[2] = float(kW), sc.view[3] = float(kH);
        for (int w = 0; w < 6; ++w) {
            for (int k = 0; k < 3; ++k) sc.wheel[w][k] = kTruckWheels[w][k];
            sc.wheel[w][3] = kTruckWheels[w][1];
            if (w < 2) sc.wheel[w][1] += s.lift;  // N13: the front wheels' deflection; the radius stays
            for (int k = 0; k < 3; ++k) sc.twheel[w][k] = kTrailerWheels[w][k];
            sc.twheel[w][3] = kTrailerWheels[w][1];
        }
        // The truck's front wheels steered as the shot says, through the same constants as the dirt.
        float steering[6] = {s.steer, s.steer, 0.f, 0.f, 0.f, 0.f};
        const float rolling[6] = {s.roll, s.roll, s.roll, s.roll, s.roll, s.roll};
        dust::Constants turns = {};
        dust::Wheels(turns, steering, rolling, 6, s.speed);
        for (int w = 0; w < 6; ++w) sc.steer[w][0] = turns.turn[w][0], sc.steer[w][1] = turns.turn[w][1];
        sc.opts[0] = s.trailer ? 1.f : 0.f, sc.opts[1] = s.truck ? 1.f : 0.f, sc.opts[3] = s.trailer_paint;
        sc.paint[0] = s.truck_paint;
        ctx->UpdateSubresource(scene_cb, 0, nullptr, &sc, 0, 0);
        // 1. The G-buffer, then each unit's stencil bit (truck 0x10, trailer 0x20).
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_STENCIL, 1.f, 0);
        ctx->OMSetRenderTargets(3, rtv, dsv);
        ctx->OMSetDepthStencilState(off, 0);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx->PSSetConstantBuffers(0, 1, &scene_cb);
        ctx->PSSetShader(scene, nullptr, 0);
        ctx->Draw(3, 0);
        ctx->OMSetBlendState(no_color, nullptr, 0xFFFFFFFF);
        ctx->PSSetShader(mask, nullptr, 0);
        const bool present[2] = {s.truck, s.trailer};
        for (int u = 0; u < 2; ++u) {
            if (!present[u]) continue;
            sc.opts[2] = float(u);
            ctx->UpdateSubresource(scene_cb, 0, nullptr, &sc, 0, 0);
            ctx->OMSetDepthStencilState(mark, u ? 0x20 : 0x10);
            ctx->Draw(3, 0);
        }
        // 2. The dirt of each unit, as the DLL draws it (camera -> clip: x / tan x, y / tan y, w = -z).
        ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
        ctx->PSSetConstantBuffers(0, 1, &dust_cb);
        for (int u = 0; u < 2; ++u) {
            if (!present[u]) continue;
            dust::Constants dc = {};
            dc.vp[2] = 1.f / kW, dc.vp[3] = 1.f / kH;
            dc.proj[0][0] = 1.f / kTanX, dc.proj[1][1] = 1.f / kTanY, dc.proj[2][2] = -1.f;
            memcpy(dc.unit, s.cam.rows, sizeof(dc.unit));
            if (s.old_scale) {  // the levels as the pass got them before N13
                dc.params[0] = u ? s.trailer_dust : s.dust;
                memcpy(dc.body, s.layers[u].body, sizeof(dc.body));
                memcpy(dc.front, s.layers[u].front, sizeof(dc.front));
                memcpy(dc.reach, s.layers[u].reach, sizeof(dc.reach));
            } else {
                dust::Levels(dc, u ? s.trailer_dust : s.dust, s.layers[u]);
            }
            dc.params[2] = 6.f, dc.params[3] = 1.f;
            memcpy(dc.wheel, u ? sc.twheel : sc.wheel, sizeof(sc.wheel));
            for (int w = 0; u && w < 6; ++w) {
                dc.wheel[w][0] -= std::copysign(s.twheel_in, dc.wheel[w][0]);
                dc.wheel_out[w] = s.twheel_reach;
            }
            for (int w = 0; !u && s.tyre_radius && w < 6; ++w) dc.wheel_radius[w] = kTruckWheels[w][1];
            if (!u && s.lift_before)  // before N13 the pass had the wheels at their config height
                for (int w = 0; w < 6; ++w) dc.wheel[w][1] = kTruckWheels[w][1];
            if (u) dust::Wheels(dc, nullptr, rolling, 6, s.speed);
            else dust::Wheels(dc, steering, rolling, 6, s.speed);
            dc.spin[2] = u ? 1.f : 0.f;  // N31: the trailer's pass, as the DLL's
            dc.spin[3] = s.flat ? 0.f : 1.f;  // D11m: the relief worked out in the pack
            ctx->UpdateSubresource(dust_cb, 0, nullptr, &dc, 0, 0);
            ctx->OMSetDepthStencilState(test, u ? 0x20 : 0x10);
            // D11m: the dirt into the pack, then the draws that take it (t2).
            ID3D11ShaderResourceView* no_pack = nullptr;
            ctx->PSSetShaderResources(2, 1, &no_pack);
            ctx->OMSetRenderTargets(1, &pack_rtv, dsv);  // rt0 unbound first: then it can be read
            ctx->PSSetShaderResources(0, 1, &srv[0]);   // (bound while rt0 is a target, D3D11 drops it)
            ctx->PSSetShader(dust_pack, nullptr, 0);
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx->Draw(3, 0);
            ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
            ctx->OMSetRenderTargets(1, &rtv[1], dsv);
            ctx->PSSetShaderResources(2, 1, &pack_srv);
            ctx->PSSetShader(dust_ps, nullptr, 0);
            ctx->Draw(3, 0);
            ctx->OMSetRenderTargets(1, &rtv[2], dsv);
            ctx->PSSetShader(dust_spec, nullptr, 0);
            ctx->Draw(3, 0);
            if (!s.flat) {  // D11k: the relief into rt0; D13f: from the pack alone, through the albedo's blend (as the DLL)
                ID3D11ShaderResourceView* none1 = nullptr;
                ctx->PSSetShaderResources(0, 1, &none1);
                ctx->OMSetRenderTargets(1, &rtv[0], dsv);
                ctx->PSSetShader(dust_norm, nullptr, 0);
                ctx->Draw(3, 0);
            }
        }
        // 3. Light.
        ID3D11ShaderResourceView* none[3] = {};
        ctx->PSSetShaderResources(2, 1, none);
        ctx->PSSetShaderResources(0, 3, none);
        ctx->OMSetRenderTargets(1, &rtv[3], nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(off, 0);
        // The sun in camera space (unit-space direction through the transposed camera rotation).
        const float su[3] = {0.45f, 0.80f, -0.40f};
        float sun[4] = {};
        for (int j = 0; j < 3; ++j) sun[j] = s.cam.rows[0][j] * su[0] + s.cam.rows[1][j] * su[1] + s.cam.rows[2][j] * su[2];
        const float sl = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        for (int j = 0; j < 3; ++j) sun[j] /= sl;
        ctx->UpdateSubresource(light_cb, 0, nullptr, sun, 0, 0);
        ctx->PSSetConstantBuffers(0, 1, &light_cb);
        ctx->PSSetShaderResources(0, 3, srv);
        ctx->PSSetShader(light, nullptr, 0);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 3, none);
        // 4. Save.
        ctx->CopyResource(staging, tex[3]);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return Fail("Map");
        if (s.grid) {  // D14: a tile, averaged 2x2 into its cell
            if (grids.empty() || grids.back().first != s.grid)
                grids.push_back({s.grid, std::vector<BYTE>(size_t(kGridW) * kGridH * 4, 255)});
            std::vector<BYTE>& g = grids.back().second;
            const BYTE* src = static_cast<const BYTE*>(m.pData);
            const UINT cx = (s.cell % kLadderCols) * kTileW, cy = (s.cell / kLadderCols) * kTileH;
            for (UINT y = 1; y + 1 < kTileH; ++y)
                for (UINT x = 1; x + 1 < kTileW; ++x)
                    for (UINT k = 0; k < 4; ++k) {
                        const size_t a = size_t(2 * y) * m.RowPitch + size_t(2 * x) * 4 + k;
                        const UINT sum = src[a] + src[a + 4] + src[a + m.RowPitch] + src[a + m.RowPitch + 4];
                        g[(size_t(cy + y) * kGridW + cx + x) * 4 + k] = BYTE((sum + 2) / 4);
                    }
            ctx->Unmap(staging, 0);
            continue;
        }
        if (s.pair == 1) {  // the left half of a pair: kept for the next view
            pair_left.assign(static_cast<const BYTE*>(m.pData), static_cast<const BYTE*>(m.pData) + size_t(m.RowPitch) * kH);
            ctx->Unmap(staging, 0);
            continue;
        }
        std::vector<BYTE> joined;
        if (s.pair == 2) {  // the middle half of each view, a white line between them
            if (pair_left.empty()) return Fail("pair without its left half");
            if (s.same_as_left) {  // N13: new level 1 = old 0.8, pixel for pixel
                long long differ = 0;
                for (UINT y = 0; y < kH; ++y)
                    for (UINT x = 0; x < kW * 4; ++x) {
                        const size_t i = size_t(y) * m.RowPitch + x;
                        differ += pair_left[i] != static_cast<const BYTE*>(m.pData)[i] ? 1 : 0;
                    }
                printf("%s %s: new level 1 = old 0.8 (%lld bytes differ)\n", differ ? "FAIL" : "ok  ", s.name, differ);
                if (differ) return 1;
            }
            joined.assign(static_cast<const BYTE*>(m.pData), static_cast<const BYTE*>(m.pData) + size_t(m.RowPitch) * kH);
            const UINT half = kW / 2, from = kW / 4;
            for (UINT y = 0; y < kH; ++y) {
                BYTE* row = joined.data() + size_t(y) * m.RowPitch;
                const BYTE* left = pair_left.data() + size_t(y) * m.RowPitch;
                const BYTE* right = static_cast<const BYTE*>(m.pData) + size_t(y) * m.RowPitch;
                memcpy(row, left + from * 4, half * 4);
                memcpy(row + half * 4, right + from * 4, half * 4);
                memset(row + (half - 1) * 4, 255, 8);
            }
            pair_left.clear();
        }
        std::string path = std::string(argv[1]) + "\\dust_" + s.name + ".png";
        const std::wstring wpath(path.begin(), path.end());
        const bool ok = SavePng(wpath, joined.empty() ? m.pData : joined.data(), m.RowPitch);
        ctx->Unmap(staging, 0);
        printf("%s %s\n", ok ? "wrote" : "FAIL: could not write", path.c_str());
        if (!ok) return 1;
    }
    for (const auto& g : grids) {  // D14
        const std::string path = std::string(argv[1]) + "\\" + g.first + ".png";
        const bool ok = SavePng(std::wstring(path.begin(), path.end()), g.second.data(), kGridW * 4, kGridW, kGridH);
        printf("%s %s\n", ok ? "wrote" : "FAIL: could not write", path.c_str());
        if (!ok) return 1;
    }
    if (ladder) return 0;
    // N10: the test panel over the last view (some forced levels, the salt row selected), then the badge.
    {
        ID3D11Texture2D* base = nullptr;
        {
            D3D11_TEXTURE2D_DESC td = {};
            tex[3]->GetDesc(&td);
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &base))) return Fail("panel base");
        }
        ctx->CopyResource(base, tex[3]);
        dirt::Model model;
        panel::Panel pnl;
        pnl.Enable(true);
        auto press = [&](panel::Key k, int times) {
            for (int i = 0; i < times; ++i) {
                const dirt::Coat shown[dirt::kUnits] = {model.Layers(0), model.Layers(1)};
                pnl.Press(k, shown);
                model.Force(pnl.Forced());
            }
        };
        press(panel::kToggle, 1);
        press(panel::kRight, 7);                            // dust 0.7
        press(panel::kDown, 1), press(panel::kRight, 4);    // wet mud 0.4
        press(panel::kDown, 1), press(panel::kRight, 3);    // dry mud 0.3
        press(panel::kDown, 4), press(panel::kFineRight, 25);  // salt 0.25
        panel_draw::Renderer renderer;
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 1) press(panel::kToggle, 1);  // closed: the badge
            const dirt::Coat shown[dirt::kUnits] = {model.Layers(0), model.Layers(1)};
            // Some game state, which must come back unchanged.
            ctx->OMSetRenderTargets(1, &rtv[1], dsv);
            ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
            ctx->OMSetDepthStencilState(test, 0x10);
            ctx->PSSetShader(light, nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &srv[2]);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            const D3D11_VIEWPORT game_vp = {3, 5, 300, 200, 0, 1};
            ctx->RSSetViewports(1, &game_vp);
            panel::View view = pnl.MakeView(shown);
            view.notice = panel::LearningNotice(true, false);  // N12: the truck not learned yet
            if (!renderer.Draw(ctx, rtv[3], int(kW), int(kH), panel::Layout(view, int(kW), int(kH))))
                return Fail(renderer.Error().c_str());
            ID3D11RenderTargetView* got_rtv = nullptr;
            ID3D11DepthStencilView* got_dsv = nullptr;
            ctx->OMGetRenderTargets(1, &got_rtv, &got_dsv);
            ID3D11BlendState* got_bs = nullptr;
            float factor[4];
            UINT mask = 0;
            ctx->OMGetBlendState(&got_bs, factor, &mask);
            ID3D11DepthStencilState* got_dss = nullptr;
            UINT ref = 0;
            ctx->OMGetDepthStencilState(&got_dss, &ref);
            ID3D11PixelShader* got_ps = nullptr;
            ctx->PSGetShader(&got_ps, nullptr, nullptr);
            ID3D11ShaderResourceView* got_srv = nullptr;
            ctx->PSGetShaderResources(0, 1, &got_srv);
            D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
            ctx->IAGetPrimitiveTopology(&topo);
            D3D11_VIEWPORT got_vp = {};
            UINT nvp = 1;
            ctx->RSGetViewports(&nvp, &got_vp);
            const bool same = got_rtv == rtv[1] && got_dsv == dsv && got_bs == blend && got_dss == test && ref == 0x10 &&
                              got_ps == light && got_srv == srv[2] && topo == D3D11_PRIMITIVE_TOPOLOGY_LINELIST &&
                              nvp == 1 && memcmp(&got_vp, &game_vp, sizeof(got_vp)) == 0;
            for (IUnknown* u : {static_cast<IUnknown*>(got_rtv), static_cast<IUnknown*>(got_dsv),
                                static_cast<IUnknown*>(got_bs), static_cast<IUnknown*>(got_dss),
                                static_cast<IUnknown*>(got_ps), static_cast<IUnknown*>(got_srv)})
                if (u) u->Release();
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            ctx->PSSetShaderResources(0, 1, &srv[0]);
            printf("%s panel: the D3D state is given back\n", same ? "ok  " : "FAIL");
            if (!same) return 1;
            ctx->CopyResource(staging, tex[3]);
            ctx->CopyResource(tex[3], base);  // the view without the panel for the next pass
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return Fail("Map");
            const std::string path = std::string(argv[1]) + (pass ? "\\panel_badge.png" : "\\panel_open.png");
            const std::wstring wpath(path.begin(), path.end());
            const bool ok = SavePng(wpath, m.pData, m.RowPitch);
            ctx->Unmap(staging, 0);
            printf("%s %s\n", ok ? "wrote" : "FAIL: could not write", path.c_str());
            if (!ok) return 1;
        }
        base->Release();
    }
    printf("done\n");
    return 0;
}
