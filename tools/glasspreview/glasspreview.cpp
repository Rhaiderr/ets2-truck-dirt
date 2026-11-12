// N64j: the windscreen's dirt, rendered offline, exactly as the game draws it.
//
// Three times running the user looked at his Volvo in the game and said the glass was not dirty, and three times
// I reasoned about the shader instead of measuring it. This tool ends that: it runs the GLASS variant of
// src/dust_shader.h over the windscreen the plugin MEASURED on that truck (D11i, 06/11: front z -3.10, base
// 2.20, top 3.65, dz/dy 0.40, half width 1.13), with the dirt of that save and the game's own dual-source blend,
// over a scene that stands in for what is behind the pane. One PNG per case, plus the numbers: the mean and the
// range of the coverage over the pane.
//
// The pane is drawn as two triangles ON the measured plane, so SV_Position.w interpolates exactly as it does in
// the game (N62b: that is the whole of how the shader places a pixel on the glass).
//
// Usage: glasspreview <out dir>   (GLASSPREVIEW_WARP=1: software rasterizer)

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wincodec.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define GLASS_PREVIEW 1
#include "dust_shader.h"

namespace {

constexpr UINT kW = 1000, kH = 640;

// The user's Volvo FH 2024, as D11i measured it in the game on 06/11.
constexpr float kFront = -3.10f, kBase = 2.20f, kSlope = 0.40f, kTop = 3.65f, kHalf = 1.13f;
constexpr float kTanY = 0.21f, kTanX = kTanY * float(kW) / float(kH);

int Fail(const char* what, HRESULT hr = S_OK) {
    printf("FAIL: %s (0x%08lx)\n", what, (unsigned long)hr);
    return 1;
}

// What is behind the glass, seen from outside: the cab's dark interior, a little sky caught at the top and the
// road reflected low down. N64k: in the scene's OWN units, as the game's forward target holds them -- radiance
// before the tone map, where a sunlit surface is worth tens and the cab's inside a fraction. Without that the
// preview cannot show the very thing that was wrong in the game.
const char kScenePs[] = R"(
float4 main(float4 pos : SV_Position) : SV_Target0 {
    const float2 uv = float2(pos.x / 1000.0, pos.y / 640.0);
    float3 sky = lerp(float3(0.35, 0.52, 0.72), float3(0.62, 0.74, 0.88), uv.y * 1.6);
    float3 cab = lerp(float3(0.045, 0.055, 0.075), float3(0.12, 0.13, 0.15), uv.y);
    // the pane's own reflection of the sky, strongest at the top, and a bright streak across it
    float refl = saturate(1.1 - uv.y * 1.7);
    refl += 0.35 * exp(-pow((uv.x - 0.72 + 0.25 * uv.y) * 7.0, 2.0));
    float3 c = lerp(cab, sky, saturate(refl));
    // a hint of the road and the grass outside the cab, at the sides
    c = lerp(c, float3(0.30, 0.33, 0.28), 0.25 * smoothstep(0.42, 0.5, abs(uv.x - 0.5)));
    return float4(c * 34.0, 1);  // N64k: the scene's units, not 0..1
}
)";

// The game's tone map, near enough: the exposure measured from the user's own [glass] on = 5 shot (the shader
// painted 0.72 and the screen read 52/255 with the gamma undone), then Reinhard and sRGB.
const char kTonePs[] = R"(
Texture2D<float4> g_hdr : register(t0);
cbuffer T : register(b0) { float4 tm; };  // x: the exposure
float4 main(float4 pos : SV_Position) : SV_Target0 {
    float3 c = g_hdr.Load(int3(pos.xy, 0)).rgb * tm.x;
    c = c / (1.0 + c);
    return float4(pow(saturate(c), 1.0 / 2.2), 1);
}
)";

// The pane: six camera-space corners in a constant buffer, projected by hand with the same projection the dirt
// shader is given. Nothing else interpolates, so the rasteriser's w is the only thing the shader reads.
const char kPaneVs[] = R"(
cbuffer V : register(b0) { float4 corner[6]; float4 proj; };  // proj: 1/tanX, 1/tanY
float4 main(uint id : SV_VertexID) : SV_Position {
    const float3 pe = corner[id].xyz;
    return float4(pe.x * proj.x, pe.y * proj.y, 0.5 * (-pe.z), -pe.z);
}
)";

ID3DBlob* CompileOrDie(const char* src, const char* target, const D3D_SHADER_MACRO* defines = nullptr) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), "glasspreview", defines, nullptr, "main", target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        printf("FAIL: shader compile\n%s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        exit(1);
    }
    if (err) err->Release();
    return code;
}

bool SavePng(const std::wstring& path, const void* rgba, UINT pitch) {
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
        return false;
    IWICStream* s = nullptr;
    IWICBitmapEncoder* e = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) &&
              SUCCEEDED(e->Initialize(s, WICBitmapEncoderNoCache)) && SUCCEEDED(e->CreateNewFrame(&fr, nullptr)) &&
              SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(kW, kH));
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

// Camera -> truck rows, from an eye and a target in the truck's frame (y up, +z rear).
struct Camera {
    float rows[3][4];
    float eye[3];
};

Camera LookAt(float ex, float ey, float ez, float tx, float ty, float tz) {
    float f[3] = {tx - ex, ty - ey, tz - ez};
    float n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f) v /= n;
    float r[3] = {-f[2], 0.f, f[0]};  // f x up(0,1,0)
    n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (float& v : r) v /= n;
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    Camera c = {};
    for (int i = 0; i < 3; ++i) c.rows[i][0] = r[i], c.rows[i][1] = u[i], c.rows[i][2] = -f[i];
    c.rows[0][3] = ex, c.rows[1][3] = ey, c.rows[2][3] = ez;
    c.eye[0] = ex, c.eye[1] = ey, c.eye[2] = ez;
    return c;
}

// A point of the truck into camera space: the inverse of the camera -> truck rows (a rotation plus a translation).
void ToCamera(const Camera& c, const float p[3], float out[3]) {
    const float d[3] = {p[0] - c.rows[0][3], p[1] - c.rows[1][3], p[2] - c.rows[2][3]};
    for (int i = 0; i < 3; ++i) out[i] = c.rows[0][i] * d[0] + c.rows[1][i] * d[1] + c.rows[2][i] * d[2];
}

float PaneZ(float y) { return kFront + (y > kBase ? (y - kBase) * kSlope : 0.f); }
// N68: the two blades of the user's Volvo FH 2024, exactly as tools/premap reads them out of the game's own
// cabin model and animation (06/11). Pivot, the axis the arm turns about, the arm at rest, the rubber's length
// and how far the arm swings.
struct RawBlade {
    float pivot[3], axis[3], arm[3], length, sweep;
};
const RawBlade kFhBlades[2] = {
    {{-0.8174f, 2.1189f, -3.1044f}, {0.0970f, -0.1030f, 0.9899f}, {0.5286f, 0.1755f, -0.0208f}, 0.700f, 1.5334f},
    {{0.1552f, 2.1189f, -3.1328f}, {-0.0264f, -0.1088f, 0.9937f}, {0.8657f, 0.1517f, 0.1038f}, 0.700f, 1.8010f},
};

// The same arithmetic the plugin does: the axis made unit, the arm taken into the plane it turns in, and the
// rubber as a ring from reach - length/2 to reach + length/2.
void BladeRows(const RawBlade& b, float r0[4], float r1[4], float r2[4]) {
    float ax[3] = {b.axis[0], b.axis[1], b.axis[2]};
    float n = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    for (float& v : ax) v /= n;
    float arm[3] = {b.arm[0], b.arm[1], b.arm[2]};
    const float along = arm[0] * ax[0] + arm[1] * ax[1] + arm[2] * ax[2];
    for (int k = 0; k < 3; ++k) arm[k] -= along * ax[k];
    const float reach = std::sqrt(arm[0] * arm[0] + arm[1] * arm[1] + arm[2] * arm[2]);
    for (float& v : arm) v /= reach;
    const float half = 0.5f * b.length;
    r0[0] = b.pivot[0], r0[1] = b.pivot[1], r0[2] = b.pivot[2], r0[3] = (std::max)(reach - half, 0.02f);
    r1[0] = ax[0], r1[1] = ax[1], r1[2] = ax[2], r1[3] = reach + half;
    r2[0] = arm[0], r2[1] = arm[1], r2[2] = arm[2], r2[3] = b.sweep;
}


}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Fail("usage: glasspreview <out dir>");
    const std::string dir = argv[1];
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const bool warp = getenv("GLASSPREVIEW_WARP") != nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr) && !warp)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                               nullptr, &ctx);
    if (FAILED(hr)) return Fail("D3D11CreateDevice", hr);

    ID3D11Texture2D* hdr = nullptr;
    ID3D11RenderTargetView* hdr_rtv = nullptr;
    ID3D11ShaderResourceView* hdr_srv = nullptr;
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = kW, td.Height = kH, td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT, td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &hdr)) ||
            FAILED(dev->CreateRenderTargetView(hdr, nullptr, &hdr_rtv)) ||
            FAILED(dev->CreateShaderResourceView(hdr, nullptr, &hdr_srv)))
            return Fail("hdr target");
    }
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = kW, td.Height = kH, td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM, td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex)) ||
            FAILED(dev->CreateRenderTargetView(tex, nullptr, &rtv)))
            return Fail("target");
    }
    ID3D11Texture2D* staging = nullptr;
    {
        D3D11_TEXTURE2D_DESC td = {};
        tex->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) return Fail("staging");
    }

    // The full-screen triangle for the scene, and the pane's own two triangles.
    const char kFullVs[] =
        "float4 main(uint id : SV_VertexID) : SV_Position {"
        " return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); }";
    ID3DBlob* b_full = CompileOrDie(kFullVs, "vs_5_0");
    ID3DBlob* b_scene = CompileOrDie(kScenePs, "ps_5_0");
    ID3DBlob* b_tone = CompileOrDie(kTonePs, "ps_5_0");
    ID3DBlob* b_pane = CompileOrDie(kPaneVs, "vs_5_0");
    const D3D_SHADER_MACRO glass_def[] = {{"GLASS", "1"}, {nullptr, nullptr}};
    ID3DBlob* b_glass = CompileOrDie(dust::kPixelShader, "ps_5_0", glass_def);
    ID3D11VertexShader *vs_full = nullptr, *vs_pane = nullptr;
    ID3D11PixelShader *ps_scene = nullptr, *ps_glass = nullptr, *ps_tone = nullptr;
    dev->CreateVertexShader(b_full->GetBufferPointer(), b_full->GetBufferSize(), nullptr, &vs_full);
    dev->CreateVertexShader(b_pane->GetBufferPointer(), b_pane->GetBufferSize(), nullptr, &vs_pane);
    dev->CreatePixelShader(b_scene->GetBufferPointer(), b_scene->GetBufferSize(), nullptr, &ps_scene);
    dev->CreatePixelShader(b_glass->GetBufferPointer(), b_glass->GetBufferSize(), nullptr, &ps_glass);
    dev->CreatePixelShader(b_tone->GetBufferPointer(), b_tone->GetBufferSize(), nullptr, &ps_tone);
    if (!vs_full || !vs_pane || !ps_scene || !ps_glass || !ps_tone) return Fail("shaders");

    // The game's own glass blend in 1.62: result = o0 + screen x o1 (N40c).
    ID3D11BlendState* bs_dual = nullptr;
    {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_SRC1_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev->CreateBlendState(&bd, &bs_dual))) return Fail("dual-source blend");
    }

    // The pane is a flat quad whose winding depends on where the camera stands: no culling.
    ID3D11RasterizerState* rs = nullptr;
    {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
        if (FAILED(dev->CreateRasterizerState(&rd, &rs))) return Fail("rasterizer");
    }

    auto make_cb = [&](UINT bytes, ID3D11Buffer** out) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (bytes + 15) & ~15u;
        bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, out));
    };
    ID3D11Buffer* cb_pane = nullptr;
    ID3D11Buffer* cb_dust = nullptr;
    ID3D11Buffer* cb_tone = nullptr;
    if (!make_cb(sizeof(float) * 4 * 7, &cb_pane) || !make_cb(sizeof(dust::Constants), &cb_dust) ||
        !make_cb(sizeof(float) * 4, &cb_tone))
        return Fail("constant buffers");

    // The camera: outside, in front of the cab, at the windscreen's own height -- the user's screenshot.
    const Camera cam = LookAt(0.f, 2.95f, -9.2f, 0.f, 2.92f, -3.2f);

    struct PaneCb {
        float corner[6][4];
        float proj[4];
    } pcb = {};
    {
        const float x0 = -kHalf, x1 = kHalf, y0 = kBase, y1 = kTop;
        const float quad[6][3] = {{x0, y0, PaneZ(y0)}, {x0, y1, PaneZ(y1)}, {x1, y1, PaneZ(y1)},
                                  {x0, y0, PaneZ(y0)}, {x1, y1, PaneZ(y1)}, {x1, y0, PaneZ(y0)}};
        for (int i = 0; i < 6; ++i) ToCamera(cam, quad[i], pcb.corner[i]);
        pcb.proj[0] = 1.f / kTanX, pcb.proj[1] = 1.f / kTanY;
    }
    ctx->UpdateSubresource(cb_pane, 0, nullptr, &pcb, 0, 0);

    // The dirt of the save the user is testing with (dirt_state.txt, 06/11), the dust forced in the test panel.
    dust::Layers layers;
    memset(&layers, 0, sizeof(layers));
    layers.body[dust::kSoot] = 0.2185f;
    layers.reach[dust::kSoot] = 0.9232f;
    layers.reach[dust::kFilm] = 0.2227f;
    layers.reach[dust::kSpray] = 0.2227f;

    struct Case {
        const char* name;
        float dust;       // the level the panel forces
        float mode;       // glass.z: 0 the look, 5 the coverage
        float strength;   // [glass] strength
        float bright;     // N64k: [glass] brightness, the dirt's radiance in the scene's units
        float wipe;       // glass.w
        float wiper_x;    // the arc, 0 = off
        float phase;      // N67e: where the blade is across the sweep, -1..1 (9 = no stroke)
        float learned;    // N67e: 1 = the arm is the learner's, so the rubber's ridge is drawn
        float expose;     // the tone map's exposure (1 = none: the raw numbers, for the coverage)
    };
    // N64k: the exposure the user's own [glass] on = 5 shot measured in the game.
    constexpr float kExposure = 0.022f;
    const Case cases[] = {
        {"glass_cover", 1.0f, 5.f, 1.0f, 1.f, 0.f, 0.f, 9.f, 0.f, 1.0f},
        {"glass_bright01", 1.0f, 0.f, 1.0f, 1.f, 0.f, 0.f, 9.f, 0.f, kExposure},  // the game before N64k
        {"glass_bright10", 1.0f, 0.f, 1.0f, 10.f, 0.f, 0.f, 9.f, 0.f, kExposure},  // the user's number
        {"glass_bright35", 1.0f, 0.f, 1.0f, 35.f, 0.f, 0.f, 9.f, 0.f, kExposure},
        {"glass_bright10_clean", 0.0f, 0.f, 1.0f, 10.f, 0.f, 0.f, 9.f, 0.f, kExposure},
        // N67e: the blade crossing the pane, with the ridge of dirt it pushes along
        {"wipe_a_start", 1.0f, 0.f, 1.0f, 10.f, 0.45f, 1.f, 0.15f, 1.f, kExposure},
        {"wipe_b_middle", 1.0f, 0.f, 1.0f, 10.f, 0.45f, 1.f, 0.50f, 1.f, kExposure},
        {"wipe_c_end", 1.0f, 0.f, 1.0f, 10.f, 0.45f, 1.f, 0.85f, 1.f, kExposure},
        {"wipe_d_done", 1.0f, 0.f, 1.0f, 10.f, 0.90f, 1.f, 9.f, 1.f, kExposure},
        // N69e: the arc painted, exactly as [glass] on = 4 does in the game
        {"arc_mode4", 1.0f, 4.f, 1.0f, 10.f, 1.00f, 1.f, 9.f, 0.f, kExposure},
    };

    for (const Case& k : cases) {
        dust::Constants c = {};
        c.vp[0] = 0.f, c.vp[1] = 0.f, c.vp[2] = 1.f / float(kW), c.vp[3] = 1.f / float(kH);
        c.proj[0][0] = 1.f / kTanX;  // clip x
        c.proj[1][1] = 1.f / kTanY;  // clip y
        c.proj[2][2] = -1.f;         // clip w = -z
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j) c.unit[i][j] = cam.rows[i][j];
        dust::Levels(c, k.dust, layers);
        c.params[3] = 1.f;  // the frame and the projection are known
        c.spin[1] = 1.f;    // the cab's pass
        c.dash0[0] = kFront + 0.03f, c.dash0[1] = kBase, c.dash0[2] = kSlope, c.dash0[3] = kTop;
        c.dash1[0] = 0.9f, c.dash1[1] = 1.2f, c.dash1[2] = -2.0f, c.dash1[3] = 0.f;
        c.pane[0] = kHalf, c.pane[1] = 1.f, c.pane[2] = 0.f, c.pane[3] = k.bright;
        c.glass[0] = k.strength, c.glass[1] = 1.f, c.glass[2] = k.mode, c.glass[3] = k.wipe;
        c.blade[0] = k.wipe * 0.55f, c.blade[1] = k.phase, c.blade[2] = 1.f, c.blade[3] = k.learned;
        c.wiper[0] = k.wiper_x, c.wiper[1] = 0.02f, c.wiper[2] = 0.48f, c.wiper[3] = 0.73f;
        // N68: the blades themselves
        for (int i = 0; i < 2; ++i)
            BladeRows(kFhBlades[i], c.wblade[i * 3], c.wblade[i * 3 + 1], c.wblade[i * 3 + 2]);
        c.wnow[0] = c.wnow[1] = k.phase;
        c.wnow[2] = k.wiper_x > 0.f ? 2.f : 0.f;
        ctx->UpdateSubresource(cb_dust, 0, nullptr, &c, 0, 0);

        const float tone[4] = {k.expose, 0, 0, 0};
        ctx->UpdateSubresource(cb_tone, 0, nullptr, tone, 0, 0);
        const float clear[4] = {0, 0, 0, 1};
        ctx->ClearRenderTargetView(hdr_rtv, clear);
        ctx->OMSetRenderTargets(1, &hdr_rtv, nullptr);
        const D3D11_VIEWPORT vp = {0, 0, float(kW), float(kH), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx->VSSetShader(vs_full, nullptr, 0);
        ctx->PSSetShader(ps_scene, nullptr, 0);
        ctx->Draw(3, 0);
        // the glass over it, the game's blend
        ctx->OMSetBlendState(bs_dual, nullptr, 0xFFFFFFFF);
        ctx->VSSetShader(vs_pane, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb_pane);
        ctx->PSSetShader(ps_glass, nullptr, 0);
        ctx->PSSetConstantBuffers(13, 1, &cb_dust);
        ctx->Draw(6, 0);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        // the tone map: the scene's units down to the screen's, as the game does after its forward pass
        ID3D11ShaderResourceView* none_srv = nullptr;
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->VSSetShader(vs_full, nullptr, 0);
        ctx->PSSetShader(ps_tone, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &hdr_srv);
        ctx->PSSetConstantBuffers(0, 1, &cb_tone);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &none_srv);

        ctx->CopyResource(staging, tex);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return Fail("map");
        // The pane's own pixels: the middle of the image, where the quad certainly lands.
        double sum = 0.0;
        int n = 0, lo = 255, hi = 0;
        for (UINT y = kH / 4; y < kH * 3 / 4; ++y) {
            const BYTE* row = static_cast<const BYTE*>(m.pData) + y * m.RowPitch;
            for (UINT x = kW / 3; x < kW * 2 / 3; ++x) {
                const int v = row[x * 4];  // red: the look's red, or the coverage in mode 5
                sum += v, ++n;
                lo = v < lo ? v : lo, hi = v > hi ? v : hi;
            }
        }
        std::vector<BYTE> img(size_t(m.RowPitch) * kH);
        memcpy(img.data(), m.pData, img.size());
        const UINT pitch = m.RowPitch;
        ctx->Unmap(staging, 0);
        const std::wstring path(dir.begin(), dir.end());
        std::wstring name(k.name, k.name + strlen(k.name));
        SavePng(path + L"\\" + name + L".png", img.data(), pitch);
        printf("%-22s dust %.2f bright %5.1f mode %.0f: red mean %.3f (%.3f .. %.3f) over the pane\n", k.name, k.dust, k.bright, k.mode,
               sum / (n * 255.0), lo / 255.0, hi / 255.0);
    }
    printf("done\n");
    return 0;
}
