// Offline test of the V1/V2 probe DLL, without the game.
//
// Loads ets2_truck_dirt.dll with a fake SDK, then draws with two real
// glass-drop pixel shaders from effect.scs: one created before the hooks
// (must be found by its signature) and one after (must be named from the
// catalog). A dynamic constant buffer carries known values; the probe must
// read them back through its GPU copy. A full-screen triangle VS feeds the
// glass-drop pixel shaders (their input: SV_Position + TEXCOORD0).
// V3a: a real swap chain is presented every frame and an indexed draw carries
// a vertex-shader constant buffer bound with a range (rows 16-31 of known
// values): the frame capture must write those rows.
// V3 part 2: a fake G-buffer pass (4 targets + stencil depth buffer, the
// game's stencil state) draws a truck (6 wheels at the configured positions +
// body parts) and traffic (one part reusing a truck mesh, far away; one
// drawn last over a truck part, like grass in front of the truck). After a
// few frames the albedo target must be red exactly at the visible truck parts, the
// stencil bits 0x30 must be clear everywhere, and the state the "game" set must
// be back after the tint pass.
// D3: the parts carry a perspective projection (VS cb0 rows 4-7, as the game)
// and write their camera-space normal and depth into rt0. The mode comes from
// the INI next to the DLL: tint = 1 -> red as above; tint = 2 with [dust]
// level >= 0.95 (0.65 in the pass, which draws the dust x 0.68 since D11d: dust::kDustScale)
// -> most visible truck parts dusty (albedo changed, beige; every one before D11d), traffic
// untouched; level <= 0 (the fake game never drives any distance) -> nothing
// changes, but the bits are cleared all the same; tint = 3 (position test) ->
// the albedo of each visible truck part is its position in the truck (the
// camera-space position rebuilt from rt0.w and the projection, taken into the
// truck's frame), within 5 cm.
// N21: the rig also has a trailer (trailer.0: 4 wheels, a hook) and a dolly behind it (trailer.1: 2 wheels with
// two meshes each, like a real one), with their configs, couplings and placements from the fake telemetry; every
// check above covers their parts as well, each in its own unit's frame.
// N30: after the G-buffer pass, glass is multiplied over a one-target image: in front of one truck part by 0.5 m
// (the cab's inside seen through a window: it must keep the "game's" look in the dust mode, cyan in the diagnosis),
// in front of another by 0.05 m (a lamp's lens: dust) and turned (traffic glass: ignored).
// D8: the image under the glass is cleared to a known colour each frame. When its targets change, the truck's glass
// (the two quads in the truck's frame) must get the glass dirt in the dust mode (level > 0) and in the diagnosis, and
// nothing else changes: traffic glass, a truck glass hidden behind a part, pixels without glass; the "game" state
// (blend, shaders, sampler) intact. Then a few frames of the interior camera: ref-2 parts sharing one matrix at the
// eye (cabin.position + head.position of the fake config) and the windshield drawn at it, one quad in a wiper's arc
// (green in the diagnosis), one outside (magenta), and traffic glass beside them (untouched).
//
// Usage: probetest <dll> <glassdrops .sm5x.fso> <another glassdrops .sm5x.fso> <log dir>
// Put an ets2_truck_dirt.ini with census_s = 10 and verbose = 1 (the glass-drop
// constants are read back only then) next to the DLL copy.

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <vector>

#include "MinHook.h"
#include "scssdk_telemetry.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_truck_common_channels.h"

namespace {

scs_telemetry_event_callback_t g_frame_end = nullptr;
scs_telemetry_event_callback_t g_started = nullptr;  // V3a: captures only run while not paused
scs_telemetry_event_callback_t g_config = nullptr;   // V3 part 2: the truck's wheel layout

SCSAPI_VOID FakeLog(const scs_log_type_t, const scs_string_t msg) { printf("game log: %s\n", msg); }

SCSAPI_RESULT FakeRegisterEvent(const scs_event_t ev, const scs_telemetry_event_callback_t cb, const scs_context_t) {
    if (ev == SCS_TELEMETRY_EVENT_frame_end) g_frame_end = cb;
    if (ev == SCS_TELEMETRY_EVENT_started) g_started = cb;
    if (ev == SCS_TELEMETRY_EVENT_configuration) g_config = cb;
    return SCS_RESULT_ok;
}
SCSAPI_RESULT FakeUnregisterEvent(const scs_event_t) { return SCS_RESULT_ok; }
// 2c: the truck's world placement, so the truck can drive (box parts are only
// learned once they have moved with the truck).
scs_telemetry_channel_callback_t g_placement = nullptr;
scs_context_t g_placement_ctx = nullptr;

// N21: the trailers' channels (placement, coupling): trailer.0 and the dolly trailer.1.
struct TrailerChannels {
    scs_telemetry_channel_callback_t placement = nullptr, connected = nullptr;
    scs_context_t placement_ctx = nullptr, connected_ctx = nullptr;
};
TrailerChannels g_trailers[2];
// Their origins behind the truck's (heading 0: +Z is behind), m: in line, the dolly hooked to the trailer 0.8 m
// behind its last axle (as the ATS double grain hopper: 0.82 m).
constexpr double kTrailerBehind[2] = {4.0, 8.8};

SCSAPI_RESULT FakeRegisterChannel(const scs_string_t name, const scs_u32_t, const scs_value_type_t, const scs_u32_t,
                                  const scs_telemetry_channel_callback_t cb, const scs_context_t ctx) {
    if (name && strcmp(name, SCS_TELEMETRY_TRUCK_CHANNEL_world_placement) == 0) {
        g_placement = cb;
        g_placement_ctx = ctx;
    }
    for (int t = 0; t < 2 && name; ++t) {
        const std::string prefix = "trailer." + std::to_string(t);
        if (prefix + ".world.placement" == name) g_trailers[t].placement = cb, g_trailers[t].placement_ctx = ctx;
        if (prefix + ".connected" == name) g_trailers[t].connected = cb, g_trailers[t].connected_ctx = ctx;
    }
    return SCS_RESULT_ok;
}

// The truck drives along -Z (heading 0), `step` m per frame, its trailers in line behind it.
void DriveTruck(int frame, double step) {
    if (!g_placement) return;
    scs_value_t v = {};
    v.type = SCS_VALUE_TYPE_dplacement;
    v.value_dplacement.position.z = -step * frame;
    g_placement(SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_U32_NIL, &v, g_placement_ctx);
    for (int t = 0; t < 2; ++t) {
        const std::string prefix = "trailer." + std::to_string(t);
        if (g_trailers[t].placement) {
            scs_value_t p = v;
            p.value_dplacement.position.z = -step * frame + kTrailerBehind[t];
            g_trailers[t].placement((prefix + ".world.placement").c_str(), SCS_U32_NIL, &p, g_trailers[t].placement_ctx);
        }
        if (g_trailers[t].connected) {
            scs_value_t c = {};
            c.type = SCS_VALUE_TYPE_bool;
            c.value_bool.value = 1;
            g_trailers[t].connected((prefix + ".connected").c_str(), SCS_U32_NIL, &c, g_trailers[t].connected_ctx);
        }
    }
}
SCSAPI_RESULT FakeUnregisterChannel(const scs_string_t, const scs_u32_t, const scs_value_type_t) { return SCS_RESULT_ok; }

std::vector<char> ReadDxbc(const char* path) {
    std::vector<char> data;
    if (FILE* f = fopen(path, "rb")) {
        fseek(f, 0, SEEK_END);
        data.resize(ftell(f));
        fseek(f, 0, SEEK_SET);
        fread(data.data(), 1, data.size(), f);
        fclose(f);
    }
    for (size_t i = 0; i + 32 < data.size(); ++i)
        if (!memcmp(&data[i], "DXBC", 4)) {
            const unsigned size = *reinterpret_cast<const unsigned*>(&data[i + 24]);
            return std::vector<char>(data.begin() + i, data.begin() + i + size);
        }
    return {};
}

int Fail(const char* what) {
    printf("FAIL: %s\n", what);
    return 1;
}

// V2b: a DDS file as D3D11 subresources (mip chain of one image).
struct Dds {
    UINT w = 0, h = 0, mips = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<char> bytes;
    std::vector<D3D11_SUBRESOURCE_DATA> subs;
};

bool LoadDds(const std::string& path, Dds* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    out->bytes.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(out->bytes.data(), 1, out->bytes.size(), f);
    fclose(f);
    const char* d = out->bytes.data();
    if (out->bytes.size() < 128 || memcmp(d, "DDS ", 4)) return false;
    auto u32 = [&](size_t o) { return *reinterpret_cast<const unsigned*>(d + o); };
    out->h = u32(12), out->w = u32(16), out->mips = u32(28) ? u32(28) : 1;
    size_t off = 128;
    if (!memcmp(d + 84, "DX10", 4)) {
        out->format = static_cast<DXGI_FORMAT>(u32(128));
        off += 20;
    } else if (u32(88) == 32) {
        out->format = u32(92) == 0x00FF0000 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    } else if (!memcmp(d + 84, "DXT5", 4)) {
        out->format = DXGI_FORMAT_BC3_UNORM;
    } else {
        return false;
    }
    const bool bc = out->format >= DXGI_FORMAT_BC1_TYPELESS && out->format <= DXGI_FORMAT_BC5_SNORM;
    for (UINT m = 0; m < out->mips; ++m) {
        const UINT mw = (std::max)(1u, out->w >> m), mh = (std::max)(1u, out->h >> m);
        const UINT pitch = bc ? (mw + 3) / 4 * 16 : mw * 4, rows = bc ? (mh + 3) / 4 : mh;
        if (off + size_t(pitch) * rows > out->bytes.size()) return false;
        out->subs.push_back({out->bytes.data() + off, pitch, 0});
        off += size_t(pitch) * rows;
    }
    return true;
}

ID3D11ShaderResourceView* MakeTexture(ID3D11Device* dev, const Dds& dds, bool with_data) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = dds.w, td.Height = dds.h, td.MipLevels = dds.mips, td.ArraySize = 1;
    td.Format = dds.format;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, with_data ? dds.subs.data() : nullptr, &tex))) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    return srv;
}

}  // namespace

// D10f: with PROBETEST_RIVAL=1, a second MinHook (another plugin's, as the tire plugin's) on DrawIndexed: made from
// the bytes before the plugin's hooks, put in after them, so its jump goes over the plugin's and its trampoline skips
// it (29/10 21:35, ATS). The plugin must put its own back in front; both are then called.
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
DrawIndexedFn g_rival_orig = nullptr;
volatile LONG g_rival_calls = 0;
void STDMETHODCALLTYPE RivalDrawIndexed(ID3D11DeviceContext* c, UINT n, UINT start, INT base) {
    InterlockedIncrement(&g_rival_calls);
    g_rival_orig(c, n, start, base);
}

int main(int argc, char** argv) {
    if (argc < 6) return Fail("usage: probetest <dll> <ps_after.fso> <ps_before.fso> <log dir> <rain dds dir>");
    Dds rain, blobs;
    if (!LoadDds(std::string(argv[5]) + "\\rain.dds", &rain) || !LoadDds(std::string(argv[5]) + "\\blobs_rain.dds", &blobs))
        return Fail("rain.dds / blobs_rain.dds");
    SetEnvironmentVariableA("TRUCK_DIRT_TEST_DIR", argv[4]);

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx)))
        return Fail("D3D11CreateDevice");
    LoadLibraryA("dxgi.dll");  // the probe only uses modules that are already loaded

    // Created before the hooks: the probe never sees its bytecode.
    const std::vector<char> before = ReadDxbc(argv[3]);
    ID3D11PixelShader* ps_before = nullptr;
    if (before.empty() || FAILED(dev->CreatePixelShader(before.data(), before.size(), nullptr, &ps_before)))
        return Fail("CreatePixelShader (before)");

    // V2b case 3: the rain texture made before the hooks (found "by description").
    ID3D11ShaderResourceView* srv_rain_before = MakeTexture(dev, rain, true);
    if (!srv_rain_before) return Fail("rain texture (before)");

    char rival_env[8] = {};
    const bool rival = GetEnvironmentVariableA("PROBETEST_RIVAL", rival_env, sizeof(rival_env)) && rival_env[0] == '1';
    void* rival_target = (*reinterpret_cast<void***>(ctx))[12];  // ID3D11DeviceContext::DrawIndexed
    if (rival && (MH_Initialize() != MH_OK ||
                  MH_CreateHook(rival_target, reinterpret_cast<void*>(&RivalDrawIndexed),
                                reinterpret_cast<void**>(&g_rival_orig)) != MH_OK))
        return Fail("rival hook");

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) return Fail("LoadLibrary");
    auto init = reinterpret_cast<decltype(&scs_telemetry_init)>(GetProcAddress(dll, "scs_telemetry_init"));
    auto shutdown = reinterpret_cast<decltype(&scs_telemetry_shutdown)>(GetProcAddress(dll, "scs_telemetry_shutdown"));
    if (!init || !shutdown) return Fail("exports");
    scs_telemetry_init_params_v101_t params = {};
    params.common.game_name = "probetest";
    params.common.game_id = "eut2";
    params.common.game_version = SCS_MAKE_VERSION(1, 21);
    params.common.log = FakeLog;
    params.register_for_event = FakeRegisterEvent;
    params.unregister_from_event = FakeUnregisterEvent;
    params.register_for_channel = FakeRegisterChannel;
    params.unregister_from_channel = FakeUnregisterChannel;
    if (init(SCS_TELEMETRY_VERSION_1_01, &params) != SCS_RESULT_ok) return Fail("scs_telemetry_init");
    Sleep(1500);  // the hook thread
    if (rival) {
        if (MH_EnableHook(rival_target) != MH_OK) return Fail("rival hook enable");
        printf("rival hook (D10f): its jump written over the plugin's\n");
        Sleep(1500);  // the plugin's watch
    }
    if (g_started) g_started(SCS_TELEMETRY_EVENT_started, nullptr, nullptr);
    // V3 part 2: truck configuration with the V3a truck's wheels (Volvo FH6).
    const float kWheels[6][3] = {{-1.04f, 0.501832f, -1.79247f}, {1.04f, 0.501832f, -1.79247f},
                                 {-0.9317f, 0.501832f, 1.41812f}, {0.9317f, 0.501832f, 1.41812f},
                                 {-0.9317f, 0.501832f, 2.76783f}, {0.9317f, 0.501832f, 2.76783f}};
    if (g_config) {
        std::vector<scs_named_value_t> attrs;
        scs_named_value_t count = {};
        count.name = "wheels.count";
        count.index = SCS_U32_NIL;
        count.value.type = SCS_VALUE_TYPE_u32;
        count.value.value_u32.value = 6;
        attrs.push_back(count);
        for (int w = 0; w < 6; ++w) {
            scs_named_value_t a = {};
            a.name = "wheel.position";
            a.index = w;
            a.value.type = SCS_VALUE_TYPE_fvector;
            a.value.value_fvector = {kWheels[w][0], kWheels[w][1], kWheels[w][2]};
            attrs.push_back(a);
        }
        // D8: the driver's eye = cabin.position + head.position = (-0.6, 2.6, -1.6).
        for (const auto& v : {std::make_pair("cabin.position", scs_value_fvector_t{0.f, 1.2f, -2.f}),
                              std::make_pair("head.position", scs_value_fvector_t{-0.6f, 1.4f, 0.4f})}) {
            scs_named_value_t a = {};
            a.name = v.first;
            a.index = SCS_U32_NIL;
            a.value.type = SCS_VALUE_TYPE_fvector;
            a.value.value_fvector = v.second;
            attrs.push_back(a);
        }
        attrs.push_back(scs_named_value_t{});
        scs_telemetry_configuration_t cfg = {"truck", attrs.data()};
        g_config(SCS_TELEMETRY_EVENT_configuration, &cfg, nullptr);
    }
    // N21: trailer.0 (4 wheels) and the dolly trailer.1 (2 wheels), each with its hook.
    const float kTrailerWheels[4][3] = {{-0.9f, 0.5f, 2.0f}, {0.9f, 0.5f, 2.0f}, {-0.9f, 0.5f, 3.0f}, {0.9f, 0.5f, 3.0f}};
    const float kDollyWheels[2][3] = {{-0.9f, 0.5f, 0.7f}, {0.9f, 0.5f, 0.7f}};
    const float kHooks[2][3] = {{0, 1.f, -1.f}, {0, 1.f, -1.f}};
    for (int t = 0; t < 2 && g_config; ++t) {
        const int n = t ? 2 : 4;
        std::vector<scs_named_value_t> attrs;
        scs_named_value_t count = {};
        count.name = "wheels.count";
        count.index = SCS_U32_NIL;
        count.value.type = SCS_VALUE_TYPE_u32;
        count.value.value_u32.value = n;
        attrs.push_back(count);
        for (int w = 0; w < n; ++w) {
            const float* p = t ? kDollyWheels[w] : kTrailerWheels[w];
            scs_named_value_t a = {};
            a.name = "wheel.position";
            a.index = w;
            a.value.type = SCS_VALUE_TYPE_fvector;
            a.value.value_fvector = {p[0], p[1], p[2]};
            attrs.push_back(a);
        }
        scs_named_value_t hook = {};
        hook.name = "hook.position";
        hook.index = SCS_U32_NIL;
        hook.value.type = SCS_VALUE_TYPE_fvector;
        hook.value.value_fvector = {kHooks[t][0], kHooks[t][1], kHooks[t][2]};
        attrs.push_back(hook);
        attrs.push_back(scs_named_value_t{});
        const std::string id = "trailer." + std::to_string(t);
        scs_telemetry_configuration_t cfg = {id.c_str(), attrs.data()};
        g_config(SCS_TELEMETRY_EVENT_configuration, &cfg, nullptr);
    }

    const std::vector<char> after = ReadDxbc(argv[2]);
    ID3D11PixelShader* ps_after = nullptr;
    if (after.empty() || FAILED(dev->CreatePixelShader(after.data(), after.size(), nullptr, &ps_after)))
        return Fail("CreatePixelShader (after)");

    // V2b case 1: rain.tobj created empty, uploaded mip by mip (full box), like the game.
    ID3D11ShaderResourceView* srv_rain = MakeTexture(dev, rain, false);
    if (!srv_rain) return Fail("rain texture");
    {
        ID3D11Resource* res = nullptr;
        srv_rain->GetResource(&res);
        ID3D11DeviceContext1* c1 = nullptr;
        ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));
        for (UINT m = 0; m < rain.mips; ++m) {
            const D3D11_BOX box = {0, 0, 0, (std::max)(4u, rain.w >> m), (std::max)(4u, rain.h >> m), 1};
            if (c1) c1->UpdateSubresource1(res, m, &box, rain.subs[m].pSysMem, rain.subs[m].SysMemPitch, 0, 0);
        }
        if (c1) c1->Release();
        res->Release();
    }
    // Case 2: blobs_rain.tobj created with its data. Case 4: rain-sized, other data (must not count).
    ID3D11ShaderResourceView* srv_blobs = MakeTexture(dev, blobs, true);
    Dds other = rain;
    for (size_t i = 128 + 20; i < other.bytes.size(); ++i) other.bytes[i] = static_cast<char>(i * 7);
    for (UINT m = 0; m < other.mips; ++m)
        other.subs[m].pSysMem = other.bytes.data() + (static_cast<const char*>(rain.subs[m].pSysMem) - rain.bytes.data());
    ID3D11ShaderResourceView* srv_other = MakeTexture(dev, other, true);
    if (!srv_blobs || !srv_other) return Fail("blobs / other texture");

    // 3 float targets, like the drop buffer.
    ID3D11RenderTargetView* rtv[3] = {};
    for (auto& v : rtv) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 64;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex)) || FAILED(dev->CreateRenderTargetView(tex, nullptr, &v)))
            return Fail("render target");
        tex->Release();
    }
    ctx->OMSetRenderTargets(3, rtv, nullptr);
    const char kVs[] =
        "struct O { float4 pos : SV_Position; float4 tc : TEXCOORD0; };"
        "O main(uint id : SV_VertexID) { O o; float2 uv = float2((id << 1) & 2, id & 2);"
        " o.pos = float4(uv * 2 - 1, 0, 1); o.tc = float4(uv, uv * 64); return o; }";
    ID3DBlob* vs_code = nullptr;
    ID3D11VertexShader* vs = nullptr;
    if (FAILED(D3DCompile(kVs, sizeof(kVs) - 1, "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs_code, nullptr)) ||
        FAILED(dev->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs)))
        return Fail("vertex shader");
    vs_code->Release();
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
    ctx->RSSetViewports(1, &vp);

    // glass_drops_params_cb_t: row 40 = (drawn, requested, dt, dt), 6.z = wiping factor, row r = r elsewhere.
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 48 * 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* cb = nullptr;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb))) return Fail("constant buffer");
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return Fail("Map");
    float* f = static_cast<float*>(m.pData);
    for (int row = 0; row < 48; ++row)
        for (int k = 0; k < 4; ++k) f[row * 4 + k] = static_cast<float>(row);
    f[160] = 12.f, f[161] = 345.f, f[162] = 0.016f, f[163] = 0.017f;
    f[26] = 0.75f;
    ctx->Unmap(cb, 0);

    ID3D11DeviceContext1* ctx1 = nullptr;
    ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1));

    // V3a: swap chain on a small window, through the device's own factory.
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"probetest";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"probetest", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                               wc.hInstance, nullptr);
    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    IDXGISwapChain* swap = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev));
    if (dxgi_dev) dxgi_dev->GetAdapter(&adapter);
    if (adapter) adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = wnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    if (!wnd || !factory || FAILED(factory->CreateSwapChain(dev, &sd, &swap))) return Fail("swap chain");

    // V3a: VS constant buffer, row r = (r, r + 0.25, r + 0.5, r + 0.75); bound as rows 16-31.
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = 64 * 16;
    vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* vs_cb = nullptr;
    if (FAILED(dev->CreateBuffer(&vbd, nullptr, &vs_cb))) return Fail("VS constant buffer");
    if (FAILED(ctx->Map(vs_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return Fail("Map VS cb");
    f = static_cast<float*>(m.pData);
    for (int row = 0; row < 64; ++row)
        for (int k = 0; k < 4; ++k) f[row * 4 + k] = row + k * 0.25f;
    ctx->Unmap(vs_cb, 0);
    const unsigned short indices[3] = {0, 1, 2};
    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = sizeof(indices);
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibdata = {indices, 0, 0};
    ID3D11Buffer* ib = nullptr;
    if (FAILED(dev->CreateBuffer(&ibd, &ibdata, &ib))) return Fail("index buffer");
    // ---- V3 part 2: fake G-buffer ----
    constexpr UINT kG = 512;
    constexpr float kProj = 4.f;  // x_clip = 4 x, y_clip = 4 y, w = -z
    const DXGI_FORMAT gfmt[4] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UINT};
    ID3D11Texture2D* gtex[4] = {};
    ID3D11RenderTargetView* grtv[4] = {};
    for (int i = 0; i < 4; ++i) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = kG;
        td.MipLevels = td.ArraySize = 1;
        td.Format = gfmt[i];
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;  // the game reads them when lighting
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &gtex[i])) || FAILED(dev->CreateRenderTargetView(gtex[i], nullptr, &grtv[i])))
            return Fail("G-buffer target");
    }
    ID3D11Texture2D* dtex = nullptr;
    ID3D11DepthStencilView* gdsv = nullptr;
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = kG;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
        dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &dtex)) || FAILED(dev->CreateDepthStencilView(dtex, &dd, &gdsv)))
            return Fail("G-buffer depth");
    }
    // The game's G-buffer state [verified: V3a]: stencil ALWAYS/REPLACE, ref 1, write mask 0x0f.
    ID3D11DepthStencilState* gdss = nullptr;
    {
        D3D11_DEPTH_STENCIL_DESC d = {};
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        d.StencilEnable = TRUE;
        d.StencilReadMask = 0xFF;
        d.StencilWriteMask = 0x0F;
        d.FrontFace = d.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                                    D3D11_COMPARISON_ALWAYS};
        if (FAILED(dev->CreateDepthStencilState(&d, &gdss))) return Fail("G-buffer depth-stencil state");
    }
    // VS: a small quad facing the camera around the part's origin, projected with
    // cb0 rows 4-7 (model -> clip); its camera depth (row 2, column 3) goes to rt0.w.
    const char kGVs[] =
        "cbuffer C : register(b0) { float4 r0; float4 r1; float4 r2; float4 r3; float4 r4; float4 r5; float4 r6; float4 r7;"
        " float4 r8; };"
        "struct V { float4 pos : SV_Position; float z : TEXCOORD0; };"
        "V main(uint id : SV_VertexID) {"
        " const float2 c[6] = { float2(-1,-1), float2(-1,1), float2(1,1), float2(-1,-1), float2(1,1), float2(1,-1) };"
        // N29: a part drawn like the ATS truck paint, its own constants in row 0 and the matrices one row down.
        " const bool shifted = r0.x > 5.0;"
        // D8: the quad around a point of the model (row 8, 0 = its origin; the interior's glass sits ahead of it).
        " const float4 o = shifted ? float4(0, 0, 0, 1) : float4(r8.xyz, 1);"
        " float4 p = shifted ? float4(r5.w, r6.w, r7.w, r8.w) : float4(dot(r4, o), dot(r5, o), dot(r6, o), dot(r7, o));"
        " p.xy += c[id % 6] * 0.012 * p.w;"
        " V v; v.pos = p; v.z = shifted ? r3.w : dot(r2, o); return v; }";
    const char kGPs[] =
        "struct O { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; uint4 d : SV_Target3; };"
        "O main(float4 pos : SV_Position, float z : TEXCOORD0) {"
        " O o; o.a = float4(0, 0, 1, z); o.b = 0.2; o.c = float4(0.5, 0.5, 0.5, 0.25);"
        // N25: rt3 as the paint writes it: emittance rgb 5-6-5 (all full) and exponent, reflection strength << 2 | bits,
        // the other bits.
        " o.d = uint4(65535, 20, (1000 << 2) | 2, 4660); return o; }";
    ID3DBlob* gvs_code = nullptr;
    ID3DBlob* gps_code = nullptr;
    ID3D11VertexShader* gvs = nullptr;
    ID3D11PixelShader* gps = nullptr;
    if (FAILED(D3DCompile(kGVs, sizeof(kGVs) - 1, "gvs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &gvs_code, nullptr)) ||
        FAILED(D3DCompile(kGPs, sizeof(kGPs) - 1, "gps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &gps_code, nullptr)) ||
        FAILED(dev->CreateVertexShader(gvs_code->GetBufferPointer(), gvs_code->GetBufferSize(), nullptr, &gvs)) ||
        FAILED(dev->CreatePixelShader(gps_code->GetBufferPointer(), gps_code->GetBufferSize(), nullptr, &gps)))
        return Fail("G-buffer shaders");
    gvs_code->Release();
    gps_code->Release();
    // Parts: (camera-space position, mesh = start index / 6). Truck frame: translation (0, -1, -15).
    struct Part {
        float pos[3];
        UINT mesh;
        bool truck;  // the player's rig (any unit)
        bool covered = false;  // a later draw covers it: must not stay red
        int unit = 0;          // N21: 0 truck, 1 trailer.0, 2 the dolly
        bool inside = false;   // N41: inside the cab's box around the driver's eye: no dust
    };
    std::vector<Part> parts;
    for (int w = 0; w < 6; ++w) parts.push_back({{kWheels[w][0], kWheels[w][1] - 1.f, kWheels[w][2] - 15.f}, 0, true});
    // N21: the rear part at z 1 (it was at 2, over the fifth wheel: in the trailer's box, it goes to the trailer while
    // in line, C10).
    const float body[6][3] = {{0, 1.5f, -2.f}, {0, 2.5f, -1.5f}, {-0.8f, 1.f, 0.f}, {0.8f, 1.f, 0.f}, {0, 0.9f, 1.f}, {0, 0.f, 0.f}};
    for (int b = 0; b < 6; ++b) parts.push_back({{body[b][0], body[b][1] - 1.f, body[b][2] - 15.f}, UINT(1 + b), true});
    // N21: each unit's origin in camera space (the truck's: the camera translated by (0, -1, -15)).
    const float kOrigin[3][3] = {{0, -1.f, -15.f}, {0, -1.f, -15.f + float(kTrailerBehind[0])},
                                 {0, -1.f, -15.f + float(kTrailerBehind[1])}};
    auto add = [&](int unit, float x, float y, float z, UINT mesh) {
        Part p{{x + kOrigin[unit][0], y + kOrigin[unit][1], z + kOrigin[unit][2]}, mesh, true};
        p.unit = unit;
        parts.push_back(p);
    };
    for (const auto& w : kTrailerWheels) add(1, w[0], w[1], w[2], 12);  // the trailer's wheels: one mesh
    add(1, 0, 0, 0, 13);                                                // trailer.0's origin
    add(1, 0, 1.6f, 1.0f, 14);
    add(1, 0, 2.2f, 2.5f, 15);
    for (const auto& w : kDollyWheels) {  // the dolly: tyre and rim at each wheel
        add(2, w[0], w[1], w[2], 12);
        add(2, w[0], w[1], w[2], 16);
    }
    add(2, 0, 0, 0, 17);       // the dolly's origin
    add(2, 0, 0.6f, -0.6f, 18);  // its drawbar
    parts.push_back({{2.8f, -2.f, -12.f}, 1, false});   // traffic: the mesh of truck body part 0, elsewhere
    parts.push_back({{-2.8f, 1.6f, -30.f}, 9, false});  // traffic: other meshes
    parts.push_back({{2.8f, 2.4f, -25.f}, 10, false});
    // Drawn last, on the line of sight of truck body part 2 but 25 m further (outside the truck's box).
    parts[6 + 2].covered = true;
    {
        const float* b2 = parts[6 + 2].pos;
        const float k = (b2[2] - 25.f) / b2[2];
        parts.push_back({{b2[0] * k, b2[1] * k, b2[2] * k}, 11, false});
    }
    // N30: glass drawn after the G-buffer pass (multiplied over a one-target image, the depth tested and not
    // written), each on the line of sight of a truck body part: 0.5 m in front of part 1 (the cab's inside seen
    // through its window: no dust), 0.05 m in front of part 3 (a lamp's lens: dust), and 0.5 m in front of part 4 but
    // turned a quarter (not the truck's frame: traffic glass, ignored).
    struct GlassQuad {
        float pos[3];
        bool turned;
    };
    std::vector<GlassQuad> glass;
    for (const auto& g : {std::make_pair(1, 0.5f), std::make_pair(3, 0.05f), std::make_pair(4, 0.5f)}) {
        const float* b = parts[6 + g.first].pos;
        const float k = (b[2] + g.second) / b[2];
        glass.push_back({{b[0] * k, b[1] * k, b[2] * k}, g.first == 4});
    }
    {  // D8: a truck glass 0.5 m behind body part 5 (hidden by it: no dirt drawn there)
        const float* b = parts[6 + 5].pos;
        const float k = (b[2] - 0.5f) / b[2];
        glass.push_back({{b[0] * k, b[1] * k, b[2] * k}, false});
    }
    parts[6 + 1].inside = true;
    // D11i: the windshield, drawn as the truck's glass but degenerate (all its indices 0: nothing on screen), 0.3 m right
    // of the truck's middle, 2.3 m up, at z -2.9 (1.3 m ahead of the eye at (-0.6, 2.6, -1.6)); the plugin measures the
    // glass's front from its vertices (WindshieldStep): the most forward point near the middle at the eye's height.
    const float kWindshield[3] = {0.3f, 2.3f - 1.f, -2.9f - 15.f};  // camera space (the truck's frame: (0, -1, -15))
    // The part's spot on the G-buffer (pixel), through the same projection as the VS.
    auto spot = [&](const float* p, int* px, int* py) {
        *px = int((kProj * p[0] / -p[2] + 1) / 2 * kG);
        *py = int((1 - kProj * p[1] / -p[2]) / 2 * kG);
        return *px >= 0 && *py >= 0 && *px < int(kG) && *py < int(kG);
    };
    std::vector<unsigned short> gidx;
    for (int q = 0; q < 32; ++q)
        for (unsigned short i = 0; i < 6; ++i) gidx.push_back(i);
    for (int i = 0; i < 6; ++i) gidx.push_back(0);  // D11i: the windshield's draw (start 192), degenerate
    D3D11_BUFFER_DESC gibd = {};
    gibd.ByteWidth = UINT(gidx.size() * sizeof(unsigned short));
    gibd.Usage = D3D11_USAGE_IMMUTABLE;
    gibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA gidata = {gidx.data(), 0, 0};
    ID3D11Buffer* gib = nullptr;
    if (FAILED(dev->CreateBuffer(&gibd, &gidata, &gib))) return Fail("G-buffer index buffer");
    // D11i: the glass draws' vertex buffer, float3 positions as the game's first stream (the meshes' origin; the VS
    // still places the quads from SV_VertexID).
    const float gverts[6 * 3] = {};
    D3D11_BUFFER_DESC gvbd = {};
    gvbd.ByteWidth = sizeof(gverts);
    gvbd.Usage = D3D11_USAGE_IMMUTABLE;
    gvbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA gvdata = {gverts, 0, 0};
    ID3D11Buffer* gvb = nullptr;
    if (FAILED(dev->CreateBuffer(&gvbd, &gvdata, &gvb))) return Fail("glass vertex buffer");
    // The per-draw constants: one 2 MB-style dynamic ring, 16 rows per draw (as the game).
    D3D11_BUFFER_DESC rbd = {};
    rbd.ByteWidth = 64 * 256;
    rbd.Usage = D3D11_USAGE_DYNAMIC;
    rbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    rbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* ring = nullptr;
    if (FAILED(dev->CreateBuffer(&rbd, nullptr, &ring))) return Fail("constant ring");
    ID3D11RenderTargetView* after_rtv = rtv[0];  // what the "game" binds after its G-buffer pass
    // N30: the image the glass is drawn over, its blend (DEST_COLOR / ZERO) and depth state (GREATER, as the depth
    // here grows nearer the camera; not written) [verified: ETS2 V3a and ATS D4s captures].
    ID3D11Texture2D* fwd_tex = nullptr;
    ID3D11RenderTargetView* fwd_rtv = nullptr;
    ID3D11BlendState* glass_bs = nullptr;
    // N40c: what ATS and ETS2 1.62 use for the glass's tint pass: dual-source blending, ONE / SRC1_COLOR, where the
    // shader's second output multiplies the image [verified: ATS 05/11 21:32, the truck's forward blends in the
    // plugin's log]. The first glass quad is drawn this way, the others the old way (DEST_COLOR / ZERO), so both
    // shapes are tested.
    ID3D11BlendState* glass_bs_dual = nullptr;
    // N40e: the reflection the game adds over the glass right after its tint pass (SRC_ALPHA / ONE), where the
    // plugin puts its veil with a blend of its own.
    ID3D11BlendState* glass_bs_add = nullptr;
    ID3D11PixelShader* glass_ps_add = nullptr;  // adds nothing of its own: the check below is about the veil
    ID3D11PixelShader* glass_ps_dual = nullptr;
    ID3D11DepthStencilState* glass_dss = nullptr;
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = kG;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;  // D8: post-processing reads it
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_GREATER;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &fwd_tex)) || FAILED(dev->CreateRenderTargetView(fwd_tex, nullptr, &fwd_rtv)) ||
            FAILED(dev->CreateBlendState(&bd, &glass_bs)) || FAILED(dev->CreateDepthStencilState(&dd, &glass_dss)))
            return Fail("glass pass objects");
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;  // N40c
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
        if (FAILED(dev->CreateBlendState(&bd, &glass_bs_dual))) return Fail("dual-source glass blend");
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;  // N40e
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        if (FAILED(dev->CreateBlendState(&bd, &glass_bs_add))) return Fail("additive glass blend");
        const char* const add_src = "float4 main(float4 pos : SV_Position) : SV_Target0 { return float4(0, 0, 0, 0); }";
        ID3DBlob* add_blob = nullptr;
        if (FAILED(D3DCompile(add_src, strlen(add_src), "add", nullptr, nullptr, "main", "ps_5_0", 0, 0, &add_blob,
                              nullptr)) ||
            FAILED(dev->CreatePixelShader(add_blob->GetBufferPointer(), add_blob->GetBufferSize(), nullptr,
                                          &glass_ps_add)))
            return Fail("additive glass shader");
        add_blob->Release();
        // The same result as the old pass: nothing added, the image multiplied by (0, 0, 1).
        const char* const dual_src =
            "void main(float4 pos : SV_Position, out float4 o0 : SV_Target0, out float4 o1 : SV_Target1) {"
            " o0 = float4(0, 0, 0, 0); o1 = float4(0, 0, 1, 1); }";
        ID3DBlob* dual_blob = nullptr;
        if (FAILED(D3DCompile(dual_src, strlen(dual_src), "dual", nullptr, nullptr, "main", "ps_5_0", 0, 0,
                              &dual_blob, nullptr)) ||
            FAILED(dev->CreatePixelShader(dual_blob->GetBufferPointer(), dual_blob->GetBufferSize(), nullptr,
                                          &glass_ps_dual)))
            return Fail("dual-source glass shader");
        dual_blob->Release();
    }
    // D10d: a UAV bound with the glass in slot 1, as the game may bind one with its transparent draws: tint's redraw
    // of the glass must leave it bound (restoring with OMSetRenderTargets alone unbound it and hid the game's glass).
    ID3D11Buffer* glass_uav_buf = nullptr;
    ID3D11UnorderedAccessView* glass_uav = nullptr;
    {
        D3D11_BUFFER_DESC ub = {};
        ub.ByteWidth = 64;
        ub.Usage = D3D11_USAGE_DEFAULT;
        ub.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 16;
        if (FAILED(dev->CreateBuffer(&ub, nullptr, &glass_uav_buf)) ||
            FAILED(dev->CreateUnorderedAccessView(glass_uav_buf, &ud, &glass_uav)))
            return Fail("glass UAV");
    }
    int uav_lost = 0;  // frames the UAV was gone after the glass draws
    // D8: the "game's" sampler in pixel shader slot 0, which the glass's dirt pass must leave there.
    ID3D11SamplerState* game_sampler = nullptr;
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev->CreateSamplerState(&sd, &game_sampler))) return Fail("sampler");
    }
    const float kImage[4] = {0.4f, 0.5f, 0.6f, 1.f};  // D8: the image under the glass

    const DWORD t0 = GetTickCount();
    int frames = 0;
    int tint_errors = 0;
    // N62: the glass runs the dust shader, compiled in a second wave after the dust's own (each variant is as
    // heavy as PACK). With the dirt on the glass on, the frames have to go on until it is ready.
    std::string ini_path = argv[1];
    ini_path = ini_path.substr(0, ini_path.find_last_of("\\/") + 1) + "ets2_truck_dirt.ini";
    const int glass_mode = static_cast<int>(GetPrivateProfileIntA("glass", "on", 0, ini_path.c_str()));
    // > census_s = 10 from the test INI; and N25's rt3 shader, compiled on a thread after the first pass (3-5 s), in use.
    while (GetTickCount() - t0 < (glass_mode ? 90000u : 20000u)) {
        // The "after" shader with a ranged binding, the "before" one with a plain binding.
        const UINT first = 0, num = 48;
        if (ctx1) ctx1->PSSetConstantBuffers1(0, 1, &cb, &first, &num);
        ctx->PSSetShader(ps_after, nullptr, 0);
        ctx->Draw(3, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetShader(ps_before, nullptr, 0);
        ctx->Draw(3, 0);
        // V2b: big non-indexed "particle" draws with each texture in slot 0.
        ID3D11ShaderResourceView* const cases[] = {srv_rain, srv_blobs, srv_rain_before, srv_other};
        for (ID3D11ShaderResourceView* v : cases) {
            ctx->PSSetShaderResources(0, 1, &v);
            ctx->Draw(1200, 0);
        }
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        // V3a: the indexed draw with the ranged VS constant buffer.
        const UINT vs_first = 16, vs_num = 16;
        if (ctx1) ctx1->VSSetConstantBuffers1(0, 1, &vs_cb, &vs_first, &vs_num);
        ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
        ctx->DrawIndexed(3, 0, 0);
        // V3 part 2: the G-buffer pass.
        {
            const float zero[4] = {};
            for (auto* v : grtv) ctx->ClearRenderTargetView(v, zero);
            ctx->ClearDepthStencilView(gdsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.f, 0);
            D3D11_MAPPED_SUBRESOURCE rm = {};
            if (FAILED(ctx->Map(ring, 0, D3D11_MAP_WRITE_DISCARD, 0, &rm))) return Fail("Map ring");
            float* rf = static_cast<float*>(rm.pData);
            memset(rf, 0, rbd.ByteWidth);  // D8: row 8 (the VS's model-space offset) 0 unless set
            for (size_t i = 0; i < parts.size(); ++i) {
                float* r = rf + i * 64;
                const float* p = parts[i].pos;
                // Rows 0-2 model -> camera, 3 = (0 0 0 1), 4-7 model -> clip (as the game [verified: V3a]).
                const float row[32] = {1, 0, 0, p[0], 0, 1, 0, p[1], 0, 0, 1, p[2], 0, 0, 0, 1,
                                       kProj, 0, 0, kProj * p[0], 0, kProj, 0, kProj * p[1], 0, 0, 0, 0.1f,
                                       0, 0, -1, -p[2]};
                // N29: the first body part (cab-like, 1.5 m up) as the ATS truck paint: "10 30 5 0.5" first.
                const bool shifted = i == 6;
                const float paint[4] = {10, 30, 5, 0.5f};
                if (shifted) memcpy(r, paint, sizeof(paint));
                memcpy(r + (shifted ? 4 : 0), row, sizeof(row));
            }
            for (size_t g = 0; g < glass.size(); ++g) {  // N30: after the parts' constants
                const float* p = glass[g].pos;
                const float t = glass[g].turned ? 1.f : 0.f, c = 1.f - t;  // a quarter turn about y, or none
                const float row[32] = {c, 0, t, p[0], 0, 1, 0, p[1], -t, 0, c, p[2], 0, 0, 0, 1,
                                       kProj, 0, 0, kProj * p[0], 0, kProj, 0, kProj * p[1], 0, 0, 0, 0.1f,
                                       0, 0, -1, -p[2]};
                memcpy(rf + (parts.size() + g) * 64, row, sizeof(row));
            }
            {  // D11i: the windshield's constants, after the glass's
                const float* p = kWindshield;
                const float row[32] = {1, 0, 0, p[0], 0, 1, 0, p[1], 0, 0, 1, p[2], 0, 0, 0, 1,
                                       kProj, 0, 0, kProj * p[0], 0, kProj, 0, kProj * p[1], 0, 0, 0, 0.1f,
                                       0, 0, -1, -p[2]};
                memcpy(rf + (parts.size() + glass.size()) * 64, row, sizeof(row));
            }
            ctx->Unmap(ring, 0);
            ctx->OMSetRenderTargets(4, grtv, gdsv);
            D3D11_VIEWPORT gvp = {0, 0, float(kG), float(kG), 0, 1};
            ctx->RSSetViewports(1, &gvp);
            ctx->OMSetDepthStencilState(gdss, 1);
            ctx->VSSetShader(gvs, nullptr, 0);
            ctx->PSSetShader(gps, nullptr, 0);
            ctx->IASetIndexBuffer(gib, DXGI_FORMAT_R16_UINT, 0);
            for (size_t i = 0; i < parts.size(); ++i) {
                const UINT first = UINT(i * 16), num = 16;
                if (ctx1) ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
                ctx->DrawIndexed(6, parts[i].mesh * 6, 0);
            }
            ctx->OMSetRenderTargets(1, &after_rtv, nullptr);  // end of the pass: the tint pass runs here
            // The "game" state must be intact after the tint pass.
            ID3D11DepthStencilState* s_dss = nullptr;
            UINT s_ref = 0;
            ctx->OMGetDepthStencilState(&s_dss, &s_ref);
            ID3D11PixelShader* s_ps = nullptr;
            ctx->PSGetShader(&s_ps, nullptr, nullptr);
            ID3D11VertexShader* s_vs = nullptr;
            ctx->VSGetShader(&s_vs, nullptr, nullptr);
            ID3D11RenderTargetView* s_rtv = nullptr;
            ctx->OMGetRenderTargets(1, &s_rtv, nullptr);
            if (s_dss != gdss || s_ref != 1 || s_ps != gps || s_vs != gvs || s_rtv != after_rtv) ++tint_errors;
            for (IUnknown* u : {static_cast<IUnknown*>(s_dss), static_cast<IUnknown*>(s_ps), static_cast<IUnknown*>(s_vs),
                                static_cast<IUnknown*>(s_rtv)})
                if (u) u->Release();
            // N30: the glass over the image, then the "game" state must be intact again.
            ctx->ClearRenderTargetView(fwd_rtv, kImage);  // D8
            ctx->PSSetSamplers(0, 1, &game_sampler);
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &fwd_rtv, gdsv, 1, 1, &glass_uav, nullptr);  // D10d
            ctx->OMSetBlendState(glass_bs, nullptr, 0xFFFFFFFF);
            ctx->OMSetDepthStencilState(glass_dss, 0);
            const UINT gstride = 12, goffset = 0;
            ctx->IASetVertexBuffers(0, 1, &gvb, &gstride, &goffset);  // D11i
            for (size_t g = 0; g < glass.size(); ++g) {
                const UINT first = UINT((parts.size() + g) * 16), num = 16;
                if (ctx1) ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
                // N40c: the first truck glass the way 1.62 draws it, the rest the old way.
                ctx->OMSetBlendState(g == 0 ? glass_bs_dual : glass_bs, nullptr, 0xFFFFFFFF);
                ctx->PSSetShader(g == 0 ? glass_ps_dual : gps, nullptr, 0);
                ctx->DrawIndexed(6, UINT(19 + g) * 6, 0);
            }
            {  // N40e: the first truck glass gets its reflection added, as the game does
                const UINT first = UINT(parts.size() * 16), num = 16;
                if (ctx1) ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
                ctx->OMSetBlendState(glass_bs_add, nullptr, 0xFFFFFFFF);
                ctx->PSSetShader(glass_ps_add, nullptr, 0);
                ctx->DrawIndexed(6, 19 * 6, 0);
                ctx->OMSetBlendState(glass_bs, nullptr, 0xFFFFFFFF);
                ctx->PSSetShader(gps, nullptr, 0);  // as the "game" had it, for the state check below
            }
            {  // D11i: the windshield (degenerate)
                const UINT first = UINT((parts.size() + glass.size()) * 16), num = 16;
                if (ctx1) ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
                ctx->DrawIndexed(6, 192, 0);
            }
            {
                ID3D11RenderTargetView* f_rtv = nullptr;
                ID3D11DepthStencilView* f_dsv = nullptr;
                ctx->OMGetRenderTargets(1, &f_rtv, &f_dsv);
                ID3D11BlendState* f_bs = nullptr;
                ctx->OMGetBlendState(&f_bs, nullptr, nullptr);
                ID3D11DepthStencilState* f_dss = nullptr;
                UINT f_ref = 0;
                ctx->OMGetDepthStencilState(&f_dss, &f_ref);
                ID3D11PixelShader* f_ps = nullptr;
                ctx->PSGetShader(&f_ps, nullptr, nullptr);
                if (f_rtv != fwd_rtv || f_dsv != gdsv || f_bs != glass_bs || f_dss != glass_dss || f_ps != gps) ++tint_errors;
                ID3D11UnorderedAccessView* f_uav = nullptr;  // D10d
                ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1, &f_uav);
                if (f_uav != glass_uav) ++uav_lost, ++tint_errors;
                if (f_uav) f_uav->Release();
                for (IUnknown* u : {static_cast<IUnknown*>(f_rtv), static_cast<IUnknown*>(f_dsv), static_cast<IUnknown*>(f_bs),
                                    static_cast<IUnknown*>(f_dss), static_cast<IUnknown*>(f_ps)})
                    if (u) u->Release();
            }
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx->OMSetRenderTargets(1, &after_rtv, nullptr);  // D8: the glass's dirt pass runs here
            {
                ID3D11BlendState* a_bs = nullptr;
                ctx->OMGetBlendState(&a_bs, nullptr, nullptr);
                ID3D11PixelShader* a_ps = nullptr;
                ctx->PSGetShader(&a_ps, nullptr, nullptr);
                ID3D11SamplerState* a_sampler = nullptr;
                ctx->PSGetSamplers(0, 1, &a_sampler);
                ID3D11RenderTargetView* a_rtv = nullptr;
                ctx->OMGetRenderTargets(1, &a_rtv, nullptr);
                if (a_bs || a_ps != gps || a_sampler != game_sampler || a_rtv != after_rtv) ++tint_errors;
                for (IUnknown* u : {static_cast<IUnknown*>(a_bs), static_cast<IUnknown*>(a_ps), static_cast<IUnknown*>(a_sampler),
                                    static_cast<IUnknown*>(a_rtv)})
                    if (u) u->Release();
            }
            ctx->VSSetShader(vs, nullptr, 0);
        }
        swap->Present(0, 0);
        ctx->OMSetRenderTargets(3, rtv, nullptr);  // Present unbinds nothing, but keep the state explicit
        DriveTruck(frames, 0.05);
        if (g_frame_end) g_frame_end(SCS_TELEMETRY_EVENT_frame_end, nullptr, nullptr);
        Sleep(16);
        ++frames;
    }
    printf("frames: %d, device removed reason 0x%08X\n", frames, static_cast<unsigned>(dev->GetDeviceRemovedReason()));
    // V3 part 2: read back the albedo and the stencil of the last frame.
    {
        int red_truck = 0, red_traffic = 0, gray_truck = 0, bad_stencil = 0, bit_left = 0;
        int dusty_truck = 0, dusty_traffic = 0, placed_truck = 0;
        float dust_rgb[3] = {}, worst_place = 0.f;
        D3D11_TEXTURE2D_DESC td = {};
        gtex[2]->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st_albedo = nullptr;
        dev->CreateTexture2D(&td, nullptr, &st_albedo);
        dtex->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st_depth = nullptr;
        dev->CreateTexture2D(&td, nullptr, &st_depth);
        gtex[3]->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st_rt3 = nullptr;  // N25
        dev->CreateTexture2D(&td, nullptr, &st_rt3);
        gtex[0]->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st_rt0 = nullptr;  // D11k
        dev->CreateTexture2D(&td, nullptr, &st_rt0);
        if (!st_albedo || !st_depth || !st_rt3 || !st_rt0) return Fail("staging textures");
        ctx->CopyResource(st_rt0, gtex[0]);
        D3D11_MAPPED_SUBRESOURCE m0 = {};
        if (FAILED(ctx->Map(st_rt0, 0, D3D11_MAP_READ, 0, &m0))) return Fail("Map rt0");
        // D11k: rt0 after the pass: the depth (w) as the "game" wrote it everywhere, the normal (0, 0, 1) on traffic
        // and a unit vector on the rig (tilted only by the relief).
        int rt0_bad = 0, rt0_tilted = 0;
        ctx->CopyResource(st_albedo, gtex[2]);
        ctx->CopyResource(st_depth, dtex);
        ctx->CopyResource(st_rt3, gtex[3]);
        D3D11_MAPPED_SUBRESOURCE m3 = {};
        if (FAILED(ctx->Map(st_rt3, 0, D3D11_MAP_READ, 0, &m3))) return Fail("Map rt3");
        // N25: per part, rt3 as the "game" wrote it, or with the reflection scaled (emittance and strength lower,
        // the exponent and the bits the same), or broken (anything else).
        int rt3_scaled_truck = 0, rt3_changed_truck = 0, rt3_changed_traffic = 0, rt3_bad = 0;
        // N21: the rig's parts in sight, and per unit (truck, trailer.0, dolly) seen / dusty / red.
        int parts_seen = 0, unit_parts[3] = {}, unit_dusty[3] = {}, unit_red[3] = {};
        // N30: the part behind the cab's glass: seen, left gray, cyan (diagnosis), rt3 as the "game" wrote it.
        int inside_seen = 0, inside_gray = 0, inside_cyan = 0, inside_rt3_same = 0;
        D3D11_MAPPED_SUBRESOURCE ma = {}, md = {};
        if (FAILED(ctx->Map(st_albedo, 0, D3D11_MAP_READ, 0, &ma)) || FAILED(ctx->Map(st_depth, 0, D3D11_MAP_READ, 0, &md)))
            return Fail("Map staging");
        auto half = [](unsigned short h) {
            const unsigned e = (h >> 10) & 31, m = h & 1023;
            float v = e ? std::ldexp(float(m | 1024), int(e) - 25) : std::ldexp(float(m), -24);
            return (h & 0x8000) ? -v : v;
        };
        for (const Part& pt : parts) {
            int px = 0, py = 0;
            if (!spot(pt.pos, &px, &py)) continue;
            const auto* a = reinterpret_cast<const unsigned short*>(static_cast<const char*>(ma.pData) + py * ma.RowPitch) + px * 4;
            const float r = half(a[0]), gch = half(a[1]), b = half(a[2]);
            const bool red = r > 0.8f && gch < 0.1f;
            const bool gray = std::fabs(r - 0.5f) < 0.01f && std::fabs(gch - 0.5f) < 0.01f && std::fabs(b - 0.5f) < 0.01f;
            const bool dusty = !red && !gray && r > b;  // beige over the gray
            const auto* q = reinterpret_cast<const unsigned short*>(static_cast<const char*>(m3.pData) + py * m3.RowPitch) + px * 4;
            const bool rt3_same = q[0] == 65535 && q[1] == 20 && q[2] == ((1000 << 2) | 2) && q[3] == 4660;
            // D4y: a very light dust (the tread worn clean, N31) may leave one of the two scaled values rounded as it was.
            const bool rt3_scaled = (q[0] < 65535 || (q[2] >> 2) < 1000) && q[1] == 20 && (q[2] & 3) == 2 &&
                                    (q[2] >> 2) <= 1000 && q[3] == 4660 && !rt3_same;
            if (!rt3_same && !rt3_scaled) ++rt3_bad;
            {  // D11k
                const auto* n0 = reinterpret_cast<const unsigned short*>(static_cast<const char*>(m0.pData) + py * m0.RowPitch) + px * 4;
                const float nx = half(n0[0]), ny = half(n0[1]), nz = half(n0[2]), w = half(n0[3]);
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                const bool flat = std::fabs(nx) < 1e-3f && std::fabs(ny) < 1e-3f && std::fabs(nz - 1.f) < 1e-3f;
                // a covered part's pixel is the covering part's: its depth is not this part's
                if ((!pt.covered && !(std::fabs(w - pt.pos[2]) < 0.02f * (1.f + std::fabs(pt.pos[2])))) || !(std::fabs(len - 1.f) < 0.01f) ||
                    ((pt.covered || !pt.truck) && !flat))
                    ++rt0_bad;
                if (!flat) ++rt0_tilted;
            }
            if (pt.covered || !pt.truck) {  // traffic, or a truck part covered by traffic: untouched
                if (!rt3_same) ++rt3_changed_traffic;
                if (red) ++red_traffic;
                if (dusty) ++dusty_traffic;
                continue;
            }
            ++parts_seen;
            ++unit_parts[pt.unit];
            if (pt.inside) {  // N30
                ++inside_seen;
                if (gray) ++inside_gray;
                if (r < 0.1f && gch > 0.9f && b > 0.9f) ++inside_cyan;
                if (rt3_same) ++inside_rt3_same;
            }
            if (red) ++red_truck;
            if (gray) ++gray_truck;
            if (!rt3_same) ++rt3_changed_truck;
            if (rt3_scaled) ++rt3_scaled_truck;
            // Position test: each unit's frame is the camera translated to its origin (N21).
            const float local[3] = {pt.pos[0] - kOrigin[pt.unit][0], pt.pos[1] - kOrigin[pt.unit][1],
                                    pt.pos[2] - kOrigin[pt.unit][2]};
            const float err = std::sqrt((r - local[0]) * (r - local[0]) + (gch - local[1]) * (gch - local[1]) +
                                        (b - local[2]) * (b - local[2]));
            if (err < 0.05f) ++placed_truck;
            worst_place = (std::max)(worst_place, err);
            if (getenv("PROBETEST_PARTS"))
                printf("  part unit %d mesh %u at (%.2f %.2f %.2f) pixel %d %d: %.3f %.3f %.3f\n", pt.unit, pt.mesh, pt.pos[0],
                       pt.pos[1], pt.pos[2], px, py, r, gch, b);
            if (dusty) ++unit_dusty[pt.unit];
            if (red) ++unit_red[pt.unit];
            if (dusty) {
                ++dusty_truck;
                dust_rgb[0] += r, dust_rgb[1] += gch, dust_rgb[2] += b;
            }
        }
        for (UINT y = 0; y < kG; ++y)
            for (UINT x = 0; x < kG; ++x) {
                const unsigned char st = static_cast<const unsigned char*>(md.pData)[y * md.RowPitch + x * 8 + 4];
                if (st & 0xF0) ++bit_left;  // N21: every unit's code; N57: a cargo's 0x80
                if ((st & 0x0F) > 1) ++bad_stencil;
            }
        ctx->Unmap(st_albedo, 0);
        ctx->Unmap(st_depth, 0);
        ctx->Unmap(st_rt3, 0);
        ctx->Unmap(st_rt0, 0);
        st_albedo->Release();
        st_depth->Release();
        st_rt3->Release();
        st_rt0->Release();
        // The mode under test, from the INI next to the DLL.
        const std::string& ini = ini_path;
        const int mode = static_cast<int>(GetPrivateProfileIntA("probe", "tint", 2, ini.c_str()));
        char level_text[32] = {};
        GetPrivateProfileStringA("dust", "level", "-1", level_text, sizeof(level_text), ini.c_str());
        const float level = static_cast<float>(atof(level_text));
        // N40 step 1: with [glass] on = 1 the truck's glass (the game's own multiply draw, redrawn with our pixel
        // shader) must take the dust, and nothing else: not the traffic's glass, not a truck glass behind another
        // surface, not a pixel without glass. With it off the forward pass is not touched at all and nothing
        // changes. N41: the part inside the cab's box around the eye gets no dust.
        const bool glass_on = glass_mode != 0;
        const int inside = inside_seen;
        printf("mode %d (dust level %.2f): truck parts red %d / dusty %d / gray %d, traffic red %d / dusty %d, "
               "stencil bits 0xf0 left on %d px, bad low bits %d px, state not restored %d frames\n",
               mode, level, red_truck, dusty_truck, gray_truck, red_traffic, dusty_traffic, bit_left, bad_stencil,
               tint_errors);
        printf("rt3 (N25): truck parts with the reflection scaled %d (changed %d), traffic changed %d, broken %d\n",
               rt3_scaled_truck, rt3_changed_truck, rt3_changed_traffic, rt3_bad);
        printf("rig parts in sight (N21): %d (truck %d, trailer %d, dolly %d); dusty truck %d, trailer %d, dolly %d; "
               "red truck %d, trailer %d, dolly %d\n", parts_seen, unit_parts[0], unit_parts[1], unit_parts[2], unit_dusty[0],
               unit_dusty[1], unit_dusty[2], unit_red[0], unit_red[1], unit_red[2]);
        printf("the game's UAV with the glass (D10d): lost after the glass draws in %d frame(s)\n", uav_lost);
        printf("inside the cab (N41): %d part(s) in sight, gray %d, cyan %d, rt3 untouched %d\n", inside_seen,
               inside_gray, inside_cyan, inside_rt3_same);
        // D11i: the windshield's front the plugin measured (its gpu_probe.log), against the glass placed here: the most
        // forward point of the truck's glass (not turned) near the middle at the eye's height, 0.6-2 m ahead of the eye.
        float ws_expected = 1e9f, ws_measured = 1e9f;
        {
            const float eye[3] = {-0.6f, 2.6f, -1.6f};
            auto consider = [&](const float* cam, bool turned) {
                const float x = cam[0], y = cam[1] + 1.f, z = cam[2] + 15.f;
                if (!turned && y > eye[1] - 0.9f && y < eye[1] + 1.2f && std::fabs(x) < 0.7f && z < eye[2] - 0.6f &&
                    z > eye[2] - 2.f)
                    ws_expected = std::min(ws_expected, z);
            };
            for (const GlassQuad& g : glass) consider(g.pos, g.turned);
            consider(kWindshield, false);
            FILE* lf = fopen((std::string(argv[4]) + "/gpu_probe.log").c_str(), "rb");
            if (lf) {
                std::string text;
                char buf[4096];
                size_t got;
                while ((got = fread(buf, 1, sizeof(buf), lf)) > 0) text.append(buf, got);
                fclose(lf);
                const size_t at = text.rfind("windshield (D11i): front z ");
                if (at != std::string::npos) ws_measured = float(atof(text.c_str() + at + 27));
            }
        }
        printf("windshield (D11i): measured front z %.2f, expected %.2f\n", ws_measured, ws_expected);
        printf("rt0 (D11k): parts with the depth or the normal broken %d, normals tilted by the relief %d\n", rt0_bad,
               rt0_tilted);
        if (mode == 3) printf("position test: %d parts within 5 cm, worst %.3f m\n", placed_truck, worst_place);
        if (dusty_truck)
            printf("dust colour (mean albedo): %.3f %.3f %.3f\n", dust_rgb[0] / dusty_truck, dust_rgb[1] / dusty_truck,
                   dust_rgb[2] / dusty_truck);
        // N21: every unit in sight (the truck's 11, the trailer's 7, the dolly's 6 parts).
        bool ok = unit_parts[0] == 11 && unit_parts[1] == 7 && unit_parts[2] == 6;
        ok = ok && !red_traffic && !dusty_traffic && !bit_left && !bad_stencil && !tint_errors && !rt3_changed_traffic &&
                  !rt3_bad && (mode == 2 || mode == 4 || !rt3_changed_truck);  // N25: only the dust touches rt3
        // N30: the glass only counts in the dust and the diagnosis; there the part behind it keeps the "game's" look
        // (cyan in the diagnosis), the lamp behind its lens and the part behind traffic glass get theirs.
        ok = ok && inside_seen == 1;
        ok = ok && (mode != 2 || std::fabs(ws_measured - ws_expected) < 0.02f);  // D11i (the dust mode)
        ok = ok && rt0_bad == 0;  // D11k
        if (mode == 1) ok = ok && red_truck == parts_seen;
        else if (mode == 4)  // N26: every part coloured, no emittance
            ok = ok && !gray_truck && rt3_scaled_truck == parts_seen && inside_cyan == inside;
        else if (mode == 3) ok = !red_traffic && !bit_left && !bad_stencil && !tint_errors && placed_truck == parts_seen;
        else if (level >= 0.95f)  // D11d: the top level (0.68 in the pass)
            // D11d: the dust's top level is lighter (dust::kDustScale): most parts covered, not every one.
            ok = ok && dusty_truck * 4 >= (parts_seen - inside) * 3 && !red_truck && rt3_scaled_truck >= dusty_truck &&
                 inside_gray == inside && inside_rt3_same == inside;
        else if (level > 0.f) ok = ok && !red_truck && inside_gray == inside_seen;  // light dust: some spots
        else ok = ok && gray_truck == parts_seen && !rt3_changed_truck;
        // ---- D8: the glass's dirt over the image ----
        // The image as it is now (rgb of each pixel).
        auto read_image = [&](std::vector<float>* out) {
            D3D11_TEXTURE2D_DESC fd = {};
            fwd_tex->GetDesc(&fd);
            fd.Usage = D3D11_USAGE_STAGING;
            fd.BindFlags = 0;
            fd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D* st = nullptr;
            if (FAILED(dev->CreateTexture2D(&fd, nullptr, &st))) return false;
            ctx->CopyResource(st, fwd_tex);
            D3D11_MAPPED_SUBRESOURCE mf = {};
            if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &mf))) {
                st->Release();
                return false;
            }
            out->assign(size_t(kG) * kG * 3, 0.f);
            for (UINT y = 0; y < kG; ++y)
                for (UINT x = 0; x < kG; ++x) {
                    const auto* h = reinterpret_cast<const unsigned short*>(static_cast<const char*>(mf.pData) + y * mf.RowPitch) + x * 4;
                    for (int k = 0; k < 3; ++k) (*out)[(size_t(y) * kG + x) * 3 + k] = half(h[k]);
                }
            ctx->Unmap(st, 0);
            st->Release();
            return true;
        };
        // The pixel at camera-space point p differs from `base` (any channel by > 0.01); its colour in *rgb.
        auto changed = [&](const std::vector<float>& img, const float* p, const float* base, float* rgb) {
            int px = 0, py = 0;
            if (!spot(p, &px, &py)) return false;
            bool diff = false;
            for (int k = 0; k < 3; ++k) {
                rgb[k] = img[(size_t(py) * kG + px) * 3 + k];
                diff = diff || std::fabs(rgb[k] - base[k]) > 0.01f;
            }
            return diff;
        };
        const bool dirt_expected = glass_on && mode == 2 && level > 0.f;
        const float multiplied[3] = {0.f, 0.f, kImage[2]};  // the image under a glass (DEST_COLOR x (0, 0, 1))
        std::vector<float> img;
        if (!read_image(&img)) return Fail("image readback");
        float rgb[3] = {};
        int truck_glass_dirty = 0;
        for (int g = 0; g < 2; ++g) truck_glass_dirty += changed(img, glass[g].pos, multiplied, rgb) ? 1 : 0;
        const bool traffic_glass = changed(img, glass[2].pos, multiplied, rgb);
        const bool hidden_glass = changed(img, glass[3].pos, kImage, rgb);
        const bool no_glass = changed(img, parts[0].pos, kImage, rgb);
        printf("glass dirt (D8), exterior: truck glass changed %d of 2, traffic glass changed %d, hidden truck glass "
               "changed %d, no glass changed %d\n", truck_glass_dirty, traffic_glass ? 1 : 0, hidden_glass ? 1 : 0,
               no_glass ? 1 : 0);
        bool glass_ok = truck_glass_dirty == (dirt_expected ? 2 : 0) && !traffic_glass && !hidden_glass && !no_glass;
        // N62b ([glass] on = 3): the glass paints each of its pixels where the shader thinks it is in the truck's
        // frame. Without the G-buffer the shader works that out from SV_Position.w alone, and the whole look
        // depends on it: a wrong depth puts every pixel at one place and the dirt comes out flat (the user, 06/11:
        // "the tone is better but it is completely uniform"). The truck's frame here is the camera moved to its
        // origin, as the parts' own position test (above).
        if (glass_mode == 3) {
            // Only the first quad: it is the one drawn the way 1.62 draws its glass (dual-source), where the
            // shader's output lands on the screen as it is. The second is multiplied into the image, which keeps
            // only what the image is not zero in (its blue), so its place cannot be read back whole.
            float worst = 0.f;
            for (int g = 0; g < 1; ++g) {
                float rgb_p[3] = {};
                int px = 0, py = 0;
                if (!spot(glass[g].pos, &px, &py)) continue;
                for (int k = 0; k < 3; ++k) rgb_p[k] = img[(size_t(py) * kG + px) * 3 + k];
                const float want[3] = {glass[g].pos[0] - kOrigin[0][0], glass[g].pos[1] - kOrigin[0][1],
                                       glass[g].pos[2] - kOrigin[0][2]};
                const float err = std::sqrt((rgb_p[0] - want[0]) * (rgb_p[0] - want[0]) +
                                            (rgb_p[1] - want[1]) * (rgb_p[1] - want[1]) +
                                            (rgb_p[2] - want[2]) * (rgb_p[2] - want[2]));
                worst = (std::max)(worst, err);
                printf("glass place (N62b): quad %d at (%.2f %.2f %.2f), the shader says (%.2f %.2f %.2f), off by %.3f m\n",
                       g, want[0], want[1], want[2], rgb_p[0], rgb_p[1], rgb_p[2], err);
            }
            glass_ok = worst < 0.05f;
            printf(glass_ok ? "glass place ok\n" : "FAIL: glass place (worst %.3f m)\n", worst);
        }
        // The interior camera: no truck in sight, the cabin's parts with stencil ref 2 at the eye's frame (10 draws:
        // the frame needs 8), the windshield drawn at it, traffic glass beside. The eye's frame in camera space: the
        // camera 3 cm above the eye, unturned. Quads around model-space points (row 8): the windshield 3 m ahead
        // (cab x -0.35 y 2.27: in the driver's wiper arc; x -1.2 y 3.02: outside), the cabin's parts up right.
        const float eye_t[3] = {0.f, -0.03f, 0.f};
        struct Quad {
            float t[3], o[3];
            UINT mesh;
        };
        const Quad cabin_part = {{eye_t[0], eye_t[1], eye_t[2]}, {0.6f, 0.6f, -3.f}, 23};
        const Quad inner[3] = {{{eye_t[0], eye_t[1], eye_t[2]}, {0.25f, -0.3f, -3.f}, 24},
                               {{eye_t[0], eye_t[1], eye_t[2]}, {-0.6f, 0.45f, -3.f}, 25},
                               {{2.f, 0.f, -10.f}, {0.f, 0.f, 0.f}, 26}};
        auto write_quad = [&](float* r, const Quad& q) {
            const float* p = q.t;
            const float row[36] = {1, 0, 0, p[0], 0, 1, 0, p[1], 0, 0, 1, p[2], 0, 0, 0, 1,
                                   kProj, 0, 0, kProj * p[0], 0, kProj, 0, kProj * p[1], 0, 0, 0, 0.1f,
                                   0, 0, -1, -p[2], q.o[0], q.o[1], q.o[2], 0};
            memcpy(r, row, sizeof(row));
        };
        for (int i = 0; i < 40; ++i) {
            const float zero[4] = {};
            for (auto* v : grtv) ctx->ClearRenderTargetView(v, zero);
            ctx->ClearDepthStencilView(gdsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.f, 0);
            D3D11_MAPPED_SUBRESOURCE rm = {};
            if (FAILED(ctx->Map(ring, 0, D3D11_MAP_WRITE_DISCARD, 0, &rm))) return Fail("Map ring");
            float* rf = static_cast<float*>(rm.pData);
            memset(rf, 0, rbd.ByteWidth);
            for (int k = 0; k < 10; ++k) write_quad(rf + k * 64, cabin_part);
            for (int k = 0; k < 3; ++k) write_quad(rf + (10 + k) * 64, inner[k]);
            ctx->Unmap(ring, 0);
            ctx->OMSetRenderTargets(4, grtv, gdsv);
            const D3D11_VIEWPORT gvp = {0, 0, float(kG), float(kG), 0, 1};
            ctx->RSSetViewports(1, &gvp);
            ctx->OMSetDepthStencilState(gdss, 2);  // the interior camera's cabin: stencil ref 2
            ctx->VSSetShader(gvs, nullptr, 0);
            ctx->PSSetShader(gps, nullptr, 0);
            ctx->IASetIndexBuffer(gib, DXGI_FORMAT_R16_UINT, 0);
            for (int k = 0; k < 10; ++k) {
                const UINT first = UINT(k * 16), num = 16;
                if (ctx1) ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
                ctx->DrawIndexed(6, cabin_part.mesh * 6, 0);
            }
            ctx->OMSetRenderTargets(1, &after_rtv, nullptr);
            ctx->ClearRenderTargetView(fwd_rtv, kImage);
            ctx->OMSetRenderTargets(1, &fwd_rtv, gdsv);
            ctx->OMSetBlendState(glass_bs, nullptr, 0xFFFFFFFF);
            ctx->OMSetDepthStencilState(glass_dss, 0);
            for (int k = 0; k < 3; ++k) {
                const UINT first = UINT((10 + k) * 16), num = 16;
                if (ctx1) ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
                ctx->DrawIndexed(6, inner[k].mesh * 6, 0);
            }
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx->OMSetRenderTargets(1, &after_rtv, nullptr);  // the glass's dirt pass runs here
            ctx->VSSetShader(vs, nullptr, 0);
            swap->Present(0, 0);
            ctx->OMSetRenderTargets(3, rtv, nullptr);
            DriveTruck(frames, 0.05);
            if (g_frame_end) g_frame_end(SCS_TELEMETRY_EVENT_frame_end, nullptr, nullptr);
            Sleep(16);
            ++frames;
        }
        if (!read_image(&img)) return Fail("image readback");
        float rgb_in[3][3] = {};
        bool in_changed[3] = {};
        for (int k = 0; k < 3; ++k) {
            float at[3];
            for (int j = 0; j < 3; ++j) at[j] = inner[k].t[j] + inner[k].o[j];
            in_changed[k] = changed(img, at, multiplied, rgb_in[k]);
        }
        printf("glass dirt (D8), interior camera: windshield in the arc changed %d (%.3f %.3f %.3f), outside it changed %d "
               "(%.3f %.3f %.3f), traffic glass changed %d\n", in_changed[0] ? 1 : 0, rgb_in[0][0], rgb_in[0][1],
               rgb_in[0][2], in_changed[1] ? 1 : 0, rgb_in[1][0], rgb_in[1][1], rgb_in[1][2], in_changed[2] ? 1 : 0);
        // N40 step 1 is the outside cameras only: in the interior camera the truck has no frame (its only draws
        // there are the cabin's, stencil ref 2, which the tracker never claims), so nothing of the windscreen
        // changes yet -- including the traffic's glass beside it.
        glass_ok = glass_ok && !in_changed[0] && !in_changed[1] && !in_changed[2];
        printf(glass_ok ? "glass dirt ok\n" : "FAIL: glass dirt\n");
        ok = ok && glass_ok;
        if (rival) {
            const LONG calls = g_rival_calls;
            printf("rival hook (D10f): its DrawIndexed detour called %ld times\n", calls);
            ok = ok && calls > 0;
        }
        printf(ok ? "tint ok\n" : "FAIL: tint\n");
    }
    shutdown();
    if (ctx1) ctx1->Release();
    ring->Release();
    for (IUnknown* u : {static_cast<IUnknown*>(glass_uav), static_cast<IUnknown*>(glass_uav_buf)})
        if (u) u->Release();
    for (IUnknown* u : {static_cast<IUnknown*>(glass_bs_dual), static_cast<IUnknown*>(glass_bs_add),
                        static_cast<IUnknown*>(glass_ps_add),
                        static_cast<IUnknown*>(glass_ps_dual),
                        static_cast<IUnknown*>(fwd_rtv), static_cast<IUnknown*>(fwd_tex), static_cast<IUnknown*>(glass_bs),
                        static_cast<IUnknown*>(glass_dss), static_cast<IUnknown*>(game_sampler)})
        u->Release();
    gib->Release();
    gvb->Release();
    gvs->Release();
    gps->Release();
    gdss->Release();
    gdsv->Release();
    dtex->Release();
    for (int i = 0; i < 4; ++i) {
        grtv[i]->Release();
        gtex[i]->Release();
    }
    ib->Release();
    vs_cb->Release();
    swap->Release();
    factory->Release();
    adapter->Release();
    dxgi_dev->Release();
    DestroyWindow(wnd);
    cb->Release();
    for (auto* v : rtv) v->Release();
    ps_after->Release();
    ps_before->Release();
    for (auto* v : {srv_rain, srv_blobs, srv_rain_before, srv_other}) v->Release();
    vs->Release();
    ctx->Release();
    dev->Release();
    printf("done\n");
    return 0;
}
