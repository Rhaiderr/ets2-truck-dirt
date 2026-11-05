// Viability tests V2 and V3a (see gpu_probe.h).
//
// The D3D11 function addresses come from a throwaway device made through the
// *system* dxgi.dll (SnowyMoon puts a dxgi.dll wrapper in the game folder),
// as in the tire plugin. The game draws on the immediate context only
// [verified: tire plugin R2b], so deferred contexts are just forwarded.
//
// Rules carried over from the tire plugin: the DLL is pinned before any
// detour exists, and detours are never removed (on shutdown they only
// forward), so no jump ever points at freed memory. Shader constants are
// read with a GPU copy into a staging buffer (inside the D3D11 contract), not
// by reading a mapped buffer after Unmap.

#include "gpu_probe.h"

#include <chrono>

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "MinHook.h"
#include "dirt_version.h"
#include "panel_draw.h"
#include "premap.h"
#include "ptr_map.h"
#include "test_panel.h"
#include "rig_tracker.h"
#include "tint.h"

namespace gpu_probe {
namespace {

struct CatalogEntry {
    const char* checksum;
    const char* label;  // "effect/semantics/state" of the first effect that uses it
};
const CatalogEntry kCatalog[] = {
#include "generated/shader_catalog.inc"
};
constexpr int kCatalogSize = static_cast<int>(sizeof(kCatalog) / sizeof(kCatalog[0]));

std::unordered_map<std::string, int> g_by_checksum;  // filled before the hooks go live
std::vector<bool> g_is_glass;                        // catalog index -> def.glassdrops.*

// ---- log ----

std::mutex g_mutex;  // guards g_messages and g_file
std::vector<std::string> g_messages;
FILE* g_file = nullptr;
std::thread g_thread;
Config g_cfg;
std::wstring g_dir;

// Every line goes to gpu_probe.log; with `game` also to the game's log.
void Emit(const std::string& msg, bool game = false) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_file, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg.c_str());
        fflush(g_file);
    }
    if (game && g_messages.size() < 200) g_messages.push_back("gpu probe: " + msg);
}

std::string Hex(uintptr_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string Narrow(const wchar_t* w) {
    char buf[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return buf;
}

std::string Where(const void* addr) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(addr), &m);
    if (!m) return Hex(reinterpret_cast<uintptr_t>(addr)) + " (no module: relay/heap)";
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    return Hex(reinterpret_cast<uintptr_t>(addr)) + " = " + Narrow(path) + "+" +
           Hex(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(m));
}

// Destination of a jump at addr (jmp rel32 or jmp [rip+x]), or null: shows who
// already hooked a function (the tire plugin, the Steam overlay...).
const void* JumpTarget(const void* addr) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return nullptr;
    const auto* p = static_cast<const unsigned char*>(addr);
    if (p[0] == 0xE9) return p + 5 + *reinterpret_cast<const int32_t*>(p + 1);
    if (p[0] == 0xFF && p[1] == 0x25) return *reinterpret_cast<const void* const*>(p + 6 + *reinterpret_cast<const int32_t*>(p + 2));
    return nullptr;
}

std::string Describe(const void* addr) {
    std::string s = Where(addr);
    const void* cur = addr;
    for (int hop = 0; hop < 4; ++hop) {
        const void* next = JumpTarget(cur);
        if (!next) break;
        s += " -> " + Where(next);
        cur = next;
    }
    return s;
}

// ---- telemetry from the game thread ----

std::atomic<bool> g_wipers{false};
std::atomic<float> g_speed_kmh{0.f};
std::atomic<bool> g_paused{true};
// N11: the units the self-check expects in the game (SetPose).
std::atomic<bool> g_truck_known{false}, g_trailer_coupled{false};
std::mutex g_pose_mutex;  // guards g_pose (game thread writes, render thread copies)
Pose g_pose;

// ---- pixel shader table ----
//
// Keys are published last (release), so a reader that finds a key sees its
// label. Render-thread fields are only touched on the render thread.

constexpr int kLabelBeforeHook = -2;  // created before our hook: no bytecode seen
constexpr int kLabelUnknown = -1;     // bytecode seen, checksum not in the catalog
constexpr size_t kTableSize = 16384;  // power of two

struct PsEntry {
    std::atomic<void*> ps{nullptr};
    std::atomic<int> label{kLabelBeforeHook};
    std::atomic<bool> reset{false};  // the address was reused by a new shader
    char checksum[33] = {};
    // render thread only
    uint32_t window_draws = 0;
    uint64_t total_draws = 0;
    bool described = false;
    uint8_t glass = 0;  // 1 = by checksum, 2 = by signature (3 targets + a 43-vec4 constant buffer)
    uint64_t last_sample_ms = 0;
    uint64_t last_dump_ms = 0;
};

PsEntry g_table[kTableSize];
std::mutex g_insert_mutex;
std::atomic<int> g_entries{0};
std::atomic<bool> g_table_full{false};

size_t Slot(const void* p) {
    return static_cast<size_t>((reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 50) & (kTableSize - 1);
}

PsEntry* Find(const void* ps) {
    for (size_t i = Slot(ps), n = 0; n < kTableSize; i = (i + 1) & (kTableSize - 1), ++n) {
        void* k = g_table[i].ps.load(std::memory_order_acquire);
        if (k == ps) return &g_table[i];
        if (!k) return nullptr;
    }
    return nullptr;
}

// Any thread. label/checksum describe the bytecode (kLabelBeforeHook = none).
PsEntry* Insert(void* ps, int label, const char* checksum) {
    std::lock_guard<std::mutex> lock(g_insert_mutex);
    if (PsEntry* e = Find(ps)) {
        if (label != kLabelBeforeHook) {  // a new shader at a reused address
            if (checksum) memcpy(e->checksum, checksum, 33);
            e->label.store(label, std::memory_order_release);
            e->reset.store(true, std::memory_order_release);
        }
        return e;
    }
    if (g_entries.load() >= static_cast<int>(kTableSize * 3 / 4)) {
        g_table_full.store(true);
        return nullptr;
    }
    for (size_t i = Slot(ps);; i = (i + 1) & (kTableSize - 1)) {
        if (g_table[i].ps.load(std::memory_order_relaxed)) continue;
        PsEntry& e = g_table[i];
        if (checksum) memcpy(e.checksum, checksum, 33);
        e.label.store(label, std::memory_order_relaxed);
        e.ps.store(ps, std::memory_order_release);
        g_entries.fetch_add(1);
        return &e;
    }
}

std::string LabelOf(const PsEntry& e) {
    const int l = e.label.load(std::memory_order_acquire);
    if (l >= 0) return kCatalog[l].label;
    if (l == kLabelUnknown) return std::string("unknown ") + e.checksum;
    return "created before the hook";
}

// ---- hooks ----

using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                                        ID3D11PixelShader**);
using PSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*,
                                               UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
// N28: the indirect draws (parameters in a GPU buffer), counted only.
using DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);

CreatePixelShaderFn g_orig_create_ps = nullptr;
PSSetShaderFn g_orig_ps_set = nullptr;
DrawIndexedFn g_orig_draw_indexed = nullptr;
DrawFn g_orig_draw = nullptr;
DrawIndexedInstancedFn g_orig_draw_indexed_inst = nullptr;
DrawInstancedFn g_orig_draw_inst = nullptr;
DrawAutoFn g_orig_draw_auto = nullptr;
DrawIndirectFn g_orig_draw_indexed_indirect = nullptr, g_orig_draw_inst_indirect = nullptr;

// Statics survive a plugin reload because the DLL is pinned once hooked.
std::atomic<bool> g_installed{false};
std::atomic<bool> g_active{false};
std::atomic<bool> g_failed{false};
std::atomic<bool> g_tint_failed{false};  // an exception in the tint path: tint off (see TintLive)
// D13e: the tint path's exceptions: how many, and the last one's code and address (said in the log).
std::atomic<int> g_tint_exceptions{0};
std::atomic<unsigned> g_tint_code{0};
std::atomic<uintptr_t> g_tint_addr{0};
std::atomic<bool> g_tint_rects_off{false};
std::atomic<void*> g_immediate{nullptr};
std::atomic<int> g_created{0}, g_created_named{0}, g_created_unknown{0};
void* g_current_ps = nullptr;  // render thread only (immediate context)

// Tint's own calls are running. After a tint exception its flag is not
// trusted (it could have been left set): the probe keeps watching.
inline bool TintBusy() { return !g_tint_failed.load(std::memory_order_relaxed) && tint::Busy(); }

bool IsImmediate(ID3D11DeviceContext* c) {
    void* imm = g_immediate.load(std::memory_order_relaxed);
    if (imm) return imm == c;
    if (c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    g_immediate.store(c, std::memory_order_relaxed);
    return true;
}

HRESULT STDMETHODCALLTYPE HookCreatePixelShader(ID3D11Device* dev, const void* code, SIZE_T len,
                                                ID3D11ClassLinkage* link, ID3D11PixelShader** out) {
    const HRESULT hr = g_orig_create_ps(dev, code, len, link, out);
    if (SUCCEEDED(hr) && out && *out && g_active.load(std::memory_order_relaxed) && code && len >= 20 &&
        !memcmp(code, "DXBC", 4)) {
        char sum[33];
        const auto* b = static_cast<const unsigned char*>(code) + 4;
        for (int i = 0; i < 16; ++i) snprintf(sum + i * 2, 3, "%02x", b[i]);
        auto it = g_by_checksum.find(sum);
        const int label = it == g_by_checksum.end() ? kLabelUnknown : it->second;
        Insert(*out, label, sum);
        const int n = g_created.fetch_add(1) + 1;
        if (label >= 0) {
            g_created_named.fetch_add(1);
        } else if (g_created_unknown.fetch_add(1) < 50) {
            Emit(std::string("created pixel shader not in the catalog: ") + sum + " (" + std::to_string(len) + " bytes)");
        }
        if (n == 1) Emit("first CreatePixelShader seen after the hook", true);
    }
    return hr;
}

void STDMETHODCALLTYPE HookPSSetShader(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci,
                                       UINT n) {
    if (g_active.load(std::memory_order_relaxed) && !TintBusy() && IsImmediate(c)) g_current_ps = ps;
    g_orig_ps_set(c, ps, ci, n);
}

// ---- first-draw description ----

enum DrawKind { kDraw, kDrawIndexed, kDrawInstanced, kDrawIndexedInstanced };
const char* const kDrawNames[] = {"Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced"};

struct CbBinding {
    ID3D11Buffer* buf = nullptr;
    UINT offset = 0;  // bytes
    UINT size = 0;    // bytes
};

// The pixel shader constant buffers bound now (caller releases bufs).
int GetPsConstantBuffers(ID3D11DeviceContext* c, CbBinding* out) {
    ID3D11DeviceContext1* c1 = nullptr;
    ID3D11Buffer* bufs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    UINT first[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    UINT num[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    constexpr UINT kSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
    if (SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1)))) {
        c1->PSGetConstantBuffers1(0, kSlots, bufs, first, num);
        c1->Release();
    } else {
        c->PSGetConstantBuffers(0, kSlots, bufs);
    }
    for (UINT i = 0; i < kSlots; ++i) {
        out[i].buf = bufs[i];
        if (!bufs[i]) continue;
        D3D11_BUFFER_DESC d = {};
        bufs[i]->GetDesc(&d);
        // num == 0: bound without a range (PSSetConstantBuffers) = the whole buffer.
        out[i].offset = num[i] ? first[i] * 16 : 0;
        out[i].size = num[i] ? (std::min)(num[i] * 16, d.ByteWidth - (std::min)(d.ByteWidth, first[i] * 16)) : d.ByteWidth;
    }
    return kSlots;
}

void DescribeDraw(ID3D11DeviceContext* c, PsEntry* e, DrawKind kind, UINT count, UINT instances) {
    char b[256];
    std::string s = "ps " + Hex(reinterpret_cast<uintptr_t>(e->ps.load())) + " " + LabelOf(*e) + " | first draw " +
                    kDrawNames[kind] + " n=" + std::to_string(count) +
                    (instances > 1 ? " x" + std::to_string(instances) : std::string());
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    int targets = 0;
    s += " | rt";
    for (auto* v : rtv) {
        if (!v) continue;
        ++targets;
        D3D11_RENDER_TARGET_VIEW_DESC d = {};
        v->GetDesc(&d);
        ID3D11Resource* res = nullptr;
        v->GetResource(&res);
        UINT w = 0, h = 0;
        ID3D11Texture2D* tex = nullptr;
        if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
            D3D11_TEXTURE2D_DESC td = {};
            tex->GetDesc(&td);
            w = td.Width, h = td.Height;
            tex->Release();
        }
        if (res) res->Release();
        snprintf(b, sizeof(b), " f%d:%ux%u", static_cast<int>(d.Format), w, h);
        s += b;
        v->Release();
    }
    if (!targets) s += " none";
    if (dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC d = {};
        dsv->GetDesc(&d);
        s += " | ds f" + std::to_string(static_cast<int>(d.Format));
        dsv->Release();
    }
    D3D11_VIEWPORT vp = {};
    UINT nvp = 1;
    c->RSGetViewports(&nvp, &vp);
    if (nvp) {
        snprintf(b, sizeof(b), " | vp %.0fx%.0f@%.0f,%.0f", vp.Width, vp.Height, vp.TopLeftX, vp.TopLeftY);
        s += b;
    }
    CbBinding cb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
    const int slots = GetPsConstantBuffers(c, cb);
    bool params_sized = false;
    s += " | pscb";
    for (int i = 0; i < slots; ++i) {
        if (!cb[i].buf) continue;
        D3D11_BUFFER_DESC d = {};
        cb[i].buf->GetDesc(&d);
        snprintf(b, sizeof(b), " b%d:%u@%u/%u%s", i, cb[i].size, cb[i].offset, d.ByteWidth,
                 d.Usage == D3D11_USAGE_DYNAMIC ? "d" : "");
        s += b;
        if (cb[i].size >= 43 * 16 && cb[i].size <= 64 * 16) params_sized = true;
        cb[i].buf->Release();
    }
    // A glass-drop pass that was created before our hook: a full-screen quad
    // into 3 targets reading a constant buffer of glass_drops_params_cb_t size.
    if (!e->glass && targets == 3 && (kind == kDraw || kind == kDrawInstanced) && count <= 6 && params_sized) {
        e->glass = 2;
        s += " | glass-drop signature";
    }
    Emit(s);
}

// ---- glass-drop constants: GPU copy -> staging -> log ----

constexpr int kStaging = 8;
constexpr UINT kStagingBytes = 1024;
constexpr UINT kParamsBytes = 43 * 16;  // glass_drops_params_cb_t [verified: GLSL of def.glassdrops]

struct Readback {
    ID3D11Buffer* staging = nullptr;
    bool pending = false;
    uint64_t t_ms = 0;
    UINT bytes = 0;
    int cb_slot = 0;
    PsEntry* entry = nullptr;
    bool dump = false;
    bool wipers = false;
    float speed = 0.f;
};
Readback g_rb[kStaging];
int g_pending = 0;
int g_readback_errors = 0;  // only the first few are logged

void Sample(ID3D11DeviceContext* c, PsEntry* e, uint64_t now) {
    e->last_sample_ms = now;
    const bool dump = now - e->last_dump_ms >= 10000;
    if (dump) e->last_dump_ms = now;
    CbBinding cb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
    const int slots = GetPsConstantBuffers(c, cb);
    ID3D11Device* dev = nullptr;
    for (int i = 0; i < slots; ++i) {
        if (!cb[i].buf) continue;
        if (cb[i].size >= kParamsBytes) {
            int r = 0;
            while (r < kStaging && g_rb[r].pending) ++r;
            if (r < kStaging) {
                if (!g_rb[r].staging) {
                    if (!dev) c->GetDevice(&dev);
                    D3D11_BUFFER_DESC d = {};
                    d.ByteWidth = kStagingBytes;
                    d.Usage = D3D11_USAGE_STAGING;
                    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    const HRESULT hr = dev ? dev->CreateBuffer(&d, nullptr, &g_rb[r].staging) : E_POINTER;
                    if (FAILED(hr) && g_readback_errors++ < 5)
                        Emit("staging buffer not created, hr=" + Hex(static_cast<uint32_t>(hr)));
                }
                if (g_rb[r].staging) {
                    const UINT bytes = (std::min)(cb[i].size, kStagingBytes);
                    const D3D11_BOX box = {cb[i].offset, 0, 0, cb[i].offset + bytes, 1, 1};
                    c->CopySubresourceRegion(g_rb[r].staging, 0, 0, 0, 0, cb[i].buf, 0, &box);
                    Readback& rb = g_rb[r];
                    rb.pending = true;
                    rb.t_ms = now;
                    rb.bytes = bytes;
                    rb.cb_slot = i;
                    rb.entry = e;
                    rb.dump = dump;
                    rb.wipers = g_wipers.load(std::memory_order_relaxed);
                    rb.speed = g_speed_kmh.load(std::memory_order_relaxed);
                    ++g_pending;
                }
            }
        }
        cb[i].buf->Release();
    }
    if (dev) dev->Release();
}

void LogReadback(const Readback& rb, const float* v) {
    char b[512];
    // Rows of glass_drops_params_cb_t (float4 each): 6.z drops_wiping_factor,
    // 40 = (rendered_drop_count, min_requested_alive_drop_count, time_delta_immediate, time_delta).
    snprintf(b, sizeof(b),
             "glass %s b%d (%u B) wipers=%d %.0f km/h: drawn %.0f requested %.0f dt %.4f/%.4f wiping %.3f "
             "lifetime %.2f-%.2f",
             LabelOf(*rb.entry).c_str(), rb.cb_slot, rb.bytes, rb.wipers ? 1 : 0, rb.speed, v[160], v[161], v[162],
             v[163], v[26], v[10], v[11]);
    std::string s = b;
    if (rb.dump) {
        s += " | rows 10-37 (wiper data 0-2, edge equations 0-3):";
        for (int row = 10; row <= 37; ++row) {
            snprintf(b, sizeof(b), " %d=(%.4g %.4g %.4g %.4g)", row, v[row * 4], v[row * 4 + 1], v[row * 4 + 2],
                     v[row * 4 + 3]);
            s += b;
        }
    }
    Emit(s);
}

void PollReadbacks(ID3D11DeviceContext* c, uint64_t now) {
    for (Readback& rb : g_rb) {
        if (!rb.pending || now - rb.t_ms < 30) continue;
        D3D11_MAPPED_SUBRESOURCE m = {};
        const HRESULT hr = c->Map(rb.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            if (now - rb.t_ms < 2000) continue;
        } else if (SUCCEEDED(hr)) {
            float v[kStagingBytes / 4] = {};
            memcpy(v, m.pData, rb.bytes);
            c->Unmap(rb.staging, 0);
            LogReadback(rb, v);
        } else if (g_readback_errors++ < 5) {
            Emit("staging Map failed, hr=" + Hex(static_cast<uint32_t>(hr)));
        }
        rb.pending = false;
        --g_pending;
    }
}

// ---- V2b: rain textures ----
//
// The falling rain is /material/environment/rain.mat (eut2.dif.flat.billb.over
// + rain.tobj) and the drops on the glass use blobs_rain(_heavy).tobj
// [verified: materials from base.scs]. The pixel shader is shared with other
// "flat" effects, so the texture tells the rain apart. It is recognised by the
// FNV-1a hash of its mip data when the game creates or uploads it, as the
// tire plugin's catalog does (hashes from tools/texture_hash/dds_hash.py).
// Only textures with the size and format of one of them are watched, so the
// tables stay small (a table that only grows cost the tire plugin its FPS
// in long sessions). A texture created before the hook is caught by its
// description at a big non-indexed draw (particles).

enum RainKind { kRainNone, kRainFalling, kRainBlobs, kRainBlobsHeavy, kRainByDesc, kRainKinds };
const char* const kRainNames[kRainKinds] = {"-", "rain.tobj", "blobs_rain.tobj", "blobs_rain_heavy.tobj",
                                            "rain-sized (by description)"};

struct RainShape {
    UINT w, h;
    bool bc3;  // BC3 (the falling rain) or 8-bit BGRA/RGBA (the blobs)
    RainKind kind;
};
const RainShape kRainShapes[] = {{64, 64, true, kRainFalling}, {128, 128, false, kRainBlobs}, {32, 32, false, kRainBlobsHeavy}};

struct RainHash {
    uint64_t hash;
    RainKind kind;
};
// Mips 0-2 (at least 16 px wide) of the files extracted from base.scs.
const RainHash kRainHashes[] = {
    {0xa267706d843976adull, kRainFalling}, {0xe1b9b89ac00f85dfull, kRainFalling}, {0x3f96db776596d00dull, kRainFalling},
    {0x7e65df0464355d84ull, kRainBlobs},   {0xdc3fa320f697dc58ull, kRainBlobs},   {0xdf7d926f96a8b5edull, kRainBlobs},
    {0x81b00f8fe4811c9full, kRainBlobsHeavy},
};

bool IsBc3(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_BC3_TYPELESS || f == DXGI_FORMAT_BC3_UNORM || f == DXGI_FORMAT_BC3_UNORM_SRGB;
}
bool Is8Bit4(DXGI_FORMAT f) {
    return (f >= DXGI_FORMAT_R8G8B8A8_TYPELESS && f <= DXGI_FORMAT_R8G8B8A8_SINT) ||
           (f >= DXGI_FORMAT_B8G8R8A8_UNORM && f <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM;
}

const RainShape* ShapeOf(const D3D11_TEXTURE2D_DESC& d) {
    if (d.ArraySize != 1 || d.SampleDesc.Count != 1) return nullptr;
    for (const RainShape& s : kRainShapes)
        if (d.Width == s.w && d.Height == s.h && (s.bc3 ? IsBc3(d.Format) : Is8Bit4(d.Format))) return &s;
    return nullptr;
}

struct RainCand {
    void* tex = nullptr;
    UINT w = 0, h = 0, mips = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::atomic<int> kind{kRainNone};  // what it turned out to be
};
constexpr int kMaxRainCands = 4096;
RainCand g_rain_cands[kMaxRainCands];
int g_rain_cand_n = 0;          // under g_rain_mutex
std::vector<int> g_rain_free;   // candidates whose texture is gone, never recognised (under g_rain_mutex)
std::mutex g_rain_mutex;
PtrMap<8192> g_rain_tex;        // texture -> 1 + candidate index
PtrMap<8192> g_rain_srv;        // view -> 1 + candidate index
PtrMap<4096> g_rain_checked;    // view seen at a big draw and not rain-sized
std::atomic<int> g_rain_recognised{0}, g_rain_unmatched{0}, g_rain_cands_full{0};

int AddRainCand(void* tex, const D3D11_TEXTURE2D_DESC& d, int kind) {
    std::lock_guard<std::mutex> lock(g_rain_mutex);
    int idx = g_rain_tex.Get(tex) - 1;
    if (idx < 0) {
        bool reused = false;
        if (!g_rain_free.empty()) {
            idx = g_rain_free.back();
            g_rain_free.pop_back();
            reused = true;
        } else if (g_rain_cand_n >= kMaxRainCands) {
            g_rain_cands_full.store(1);
            return -1;
        } else {
            idx = g_rain_cand_n++;
        }
        if (!g_rain_tex.Set(tex, idx + 1)) {
            if (reused) g_rain_free.push_back(idx);
            return -1;
        }
    }
    RainCand& c = g_rain_cands[idx];
    c.tex = tex;
    c.w = d.Width, c.h = d.Height, c.mips = d.MipLevels ? d.MipLevels : 1;
    c.format = d.Format;
    c.kind.store(kind, std::memory_order_release);
    return idx;
}

// The address of a watched texture now belongs to something else: forget it.
// Its candidate is reused when it never turned out to be rain (a recognised
// one stays, as views of it may still be listed in g_rain_srv).
void ReleaseRainTex(void* tex) {
    std::lock_guard<std::mutex> lock(g_rain_mutex);
    const int idx = g_rain_tex.Get(tex) - 1;
    if (idx < 0) return;
    g_rain_tex.Erase(tex);
    RainCand& c = g_rain_cands[idx];
    if (c.kind.load(std::memory_order_acquire) != kRainNone) return;
    c.tex = nullptr;
    g_rain_free.push_back(idx);
}

// Hash of one mip given as rows (blocks for BC3, pixels otherwise).
bool HashMip(const RainCand& c, UINT mip, const void* data, UINT pitch, uint64_t* out) {
    const UINT w = (std::max)(1u, c.w >> mip), h = (std::max)(1u, c.h >> mip);
    if (w < 16 || !data) return false;  // tiny mips are too generic
    const bool bc = IsBc3(c.format);
    const UINT row_bytes = bc ? (w + 3) / 4 * 16 : w * 4, rows = bc ? (h + 3) / 4 : h;
    if (pitch < row_bytes) return false;
    uint64_t hash = 1469598103934665603ull;
    const auto* p = static_cast<const uint8_t*>(data);
    for (UINT r = 0; r < rows; ++r)
        for (UINT i = 0; i < row_bytes; ++i) {
            hash ^= p[size_t(r) * pitch + i];
            hash *= 1099511628211ull;
        }
    *out = hash;
    return true;
}

void MatchUpload(int idx, UINT sub, const D3D11_BOX* box, const void* data, UINT pitch, const char* how) {
    RainCand& c = g_rain_cands[idx];
    const UINT mip = sub % c.mips;
    const UINT w = (std::max)(1u, c.w >> mip), h = (std::max)(1u, c.h >> mip);
    if (box && (box->left || box->top || box->right < w || box->bottom < h)) return;  // partial: not hashed
    uint64_t hash = 0;
    if (!HashMip(c, mip, data, pitch, &hash)) return;
    for (const RainHash& rh : kRainHashes)
        if (rh.hash == hash) {
            if (c.kind.exchange(rh.kind) != rh.kind && g_rain_recognised.fetch_add(1) < 20)
                Emit(std::string("rain texture recognised via ") + how + ": " + kRainNames[rh.kind] + " (mip " +
                     std::to_string(mip) + ", texture " + Hex(reinterpret_cast<uintptr_t>(c.tex)) + ")", true);
            return;
        }
    if (g_rain_unmatched.fetch_add(1) < 10) {
        char b[160];
        snprintf(b, sizeof(b), "rain-sized texture, other data: %ux%u format %d mip %u hash %016llx (%s)", c.w, c.h,
                 static_cast<int>(c.format), mip, static_cast<unsigned long long>(hash), how);
        Emit(b);
    }
}

using CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                      const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateSrvFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                ID3D11ShaderResourceView**);
using PSSetSrvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using UpdateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*,
                                          UINT, UINT);
using Update1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*,
                                           UINT, UINT, UINT);
CreateTexture2DFn g_orig_create_tex = nullptr;
CreateSrvFn g_orig_create_srv = nullptr;
PSSetSrvFn g_orig_ps_set_srv = nullptr;
UpdateFn g_orig_update = nullptr;
Update1Fn g_orig_update1 = nullptr;

HRESULT STDMETHODCALLTYPE HookCreateTexture2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* d,
                                              const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    const HRESULT hr = g_orig_create_tex(dev, d, init, out);
    if (SUCCEEDED(hr) && out && *out && d && g_active.load(std::memory_order_relaxed)) {
        if (ShapeOf(*d)) {
            const int idx = AddRainCand(*out, *d, kRainNone);
            if (idx >= 0 && init)
                for (UINT m = 0; m < g_rain_cands[idx].mips && m < 3; ++m)
                    MatchUpload(idx, m, nullptr, init[m].pSysMem, init[m].SysMemPitch, "CreateTexture2D");
        } else {
            ReleaseRainTex(*out);  // the address now belongs to another texture
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSrv(ID3D11Device* dev, ID3D11Resource* res, const D3D11_SHADER_RESOURCE_VIEW_DESC* d,
                                        ID3D11ShaderResourceView** out) {
    const HRESULT hr = g_orig_create_srv(dev, res, d, out);
    if (SUCCEEDED(hr) && out && *out && g_active.load(std::memory_order_relaxed)) {
        int v = res ? g_rain_tex.Get(res) : 0;
        if (v) {  // only a texture of a rain shape: the address may now belong to a resource made elsewhere
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            res->GetType(&dim);
            D3D11_TEXTURE2D_DESC td = {};
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
            if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D || !ShapeOf(td)) {
                ReleaseRainTex(res);
                v = 0;
            }
        }
        if (v) g_rain_srv.Set(*out, v);
        else if (g_rain_srv.Get(*out)) g_rain_srv.Erase(*out);  // reused address
        if (g_rain_checked.Get(*out)) g_rain_checked.Erase(*out);
    }
    return hr;
}

void STDMETHODCALLTYPE HookUpdate(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT sub, const D3D11_BOX* box,
                                  const void* data, UINT pitch, UINT depth_pitch) {
    if (g_active.load(std::memory_order_relaxed) && dst)
        if (const int v = g_rain_tex.Get(dst)) MatchUpload(v - 1, sub, box, data, pitch, "UpdateSubresource");
    g_orig_update(c, dst, sub, box, data, pitch, depth_pitch);
}

void STDMETHODCALLTYPE HookUpdate1(ID3D11DeviceContext1* c, ID3D11Resource* dst, UINT sub, const D3D11_BOX* box,
                                   const void* data, UINT pitch, UINT depth_pitch, UINT flags) {
    if (g_active.load(std::memory_order_relaxed) && dst)
        if (const int v = g_rain_tex.Get(dst)) MatchUpload(v - 1, sub, box, data, pitch, "UpdateSubresource1");
    g_orig_update1(c, dst, sub, box, data, pitch, depth_pitch, flags);
}

// Which pixel-shader slots hold a watched texture now (immediate context, render thread).
constexpr UINT kRainSlots = 32;
int g_slot_cand[kRainSlots] = {};  // 1 + candidate index, 0 = none
uint32_t g_rain_mask = 0;

void STDMETHODCALLTYPE HookPSSetSrv(ID3D11DeviceContext* c, UINT start, UINT num, ID3D11ShaderResourceView* const* v) {
    if (g_active.load(std::memory_order_relaxed) && !TintBusy() && IsImmediate(c))
        for (UINT i = 0; i < num && start + i < kRainSlots; ++i) {
            const UINT s = start + i;
            const int cand = v && v[i] ? g_rain_srv.Get(v[i]) : 0;
            g_slot_cand[s] = cand;
            if (cand) g_rain_mask |= 1u << s;
            else g_rain_mask &= ~(1u << s);
        }
    g_orig_ps_set_srv(c, start, num, v);
}

// A big non-indexed draw with a rain-sized texture that we never saw being
// created (made before the hook): remember it, as "by description".
int CheckByDescription(ID3D11DeviceContext* c) {
    ID3D11ShaderResourceView* views[8] = {};
    c->PSGetShaderResources(0, 8, views);
    int found = kRainNone;
    for (UINT s = 0; s < 8; ++s) {
        ID3D11ShaderResourceView* v = views[s];
        if (!v) continue;
        if (const int cand = g_rain_srv.Get(v)) {
            found = (std::max)(found, g_rain_cands[cand - 1].kind.load());
        } else if (!g_rain_checked.Get(v)) {
            ID3D11Resource* res = nullptr;
            v->GetResource(&res);
            ID3D11Texture2D* tex = nullptr;
            bool sized = false;
            if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
                D3D11_TEXTURE2D_DESC d = {};
                tex->GetDesc(&d);
                if (ShapeOf(d)) {
                    const int idx = AddRainCand(res, d, kRainByDesc);
                    if (idx >= 0) {
                        g_rain_srv.Set(v, idx + 1);
                        sized = true;
                        found = (std::max)(found, static_cast<int>(kRainByDesc));
                        char b[160];
                        snprintf(b, sizeof(b), "rain-sized texture created before the hook, seen at a big draw: %ux%u format %d mips %u",
                                 d.Width, d.Height, static_cast<int>(d.Format), d.MipLevels);
                        Emit(b);
                    }
                }
                tex->Release();
            }
            if (res) res->Release();
            if (!sized) g_rain_checked.Set(v, 1);
        }
        v->Release();
    }
    return found;
}

// Per-second rain summary, by viewport (render thread).
struct RainVp {
    float w, h;
    int kind;
    uint32_t draws;
    uint64_t verts;
    UINT last_n;
};
RainVp g_rain_vp[12];
int g_rain_vp_n = 0;
uint32_t g_sec_glass_draws = 0;
uint64_t g_rain_sec_t0 = 0;
bool g_rain_was_active = false;
std::atomic<bool> g_rain_last_second{false};  // D3: read by the game thread (RainActive)
std::atomic<uint64_t> g_rain_last_ms{0};       // when it was last updated (GetTickCount64)

void NoteRainDraw(ID3D11DeviceContext* c, int kind, UINT count, UINT instances) {
    D3D11_VIEWPORT vp = {};
    UINT n = 1;
    c->RSGetViewports(&n, &vp);
    for (int i = 0; i < g_rain_vp_n; ++i) {
        RainVp& r = g_rain_vp[i];
        if (r.w == vp.Width && r.h == vp.Height && r.kind == kind) {
            ++r.draws;
            r.verts += uint64_t(count) * instances;
            r.last_n = count;
            return;
        }
    }
    if (g_rain_vp_n < 12) g_rain_vp[g_rain_vp_n++] = {vp.Width, vp.Height, kind, 1, uint64_t(count) * instances, count};
}

// V2c: every big non-indexed draw (particles: the rain is one, with a vertex
// count that changes with the weather), grouped by pixel shader, viewport and
// the texture in slot 0, so the rain can be told apart without knowing its
// texture (weather mods replace it [verified: Weather_3.9_16k, V2b]).
constexpr UINT kBigDrawVerts = 600;
struct BigDraw {
    void* ps;
    float vw, vh;
    UINT tw, th;
    int format;  // -1 = no texture in slot 0
    uint32_t draws;
    uint64_t verts;
    UINT min_n, max_n;
};
BigDraw g_big[32];
int g_big_n = 0;
uint32_t g_big_dropped = 0;

void NoteBigDraw(ID3D11DeviceContext* c, UINT count, UINT instances) {
    D3D11_VIEWPORT vp = {};
    UINT nvp = 1;
    c->RSGetViewports(&nvp, &vp);
    UINT tw = 0, th = 0;
    int format = -1;
    ID3D11ShaderResourceView* v = nullptr;
    c->PSGetShaderResources(0, 1, &v);
    if (v) {
        ID3D11Resource* res = nullptr;
        v->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
            D3D11_TEXTURE2D_DESC d = {};
            tex->GetDesc(&d);
            tw = d.Width, th = d.Height, format = static_cast<int>(d.Format);
            tex->Release();
        }
        if (res) res->Release();
        v->Release();
    }
    const uint64_t n = uint64_t(count) * instances;
    for (int i = 0; i < g_big_n; ++i) {
        BigDraw& b = g_big[i];
        if (b.ps == g_current_ps && b.vw == vp.Width && b.vh == vp.Height && b.tw == tw && b.th == th &&
            b.format == format) {
            ++b.draws;
            b.verts += n;
            b.min_n = (std::min)(b.min_n, count);
            b.max_n = (std::max)(b.max_n, count);
            return;
        }
    }
    if (g_big_n < 32) g_big[g_big_n++] = {g_current_ps, vp.Width, vp.Height, tw, th, format, 1, n, count, count};
    else ++g_big_dropped;
}

void BigDrawSecond(double secs) {
    if (!g_big_n) return;
    char b[256];
    snprintf(b, sizeof(b), "big draws %.1f s (non-indexed, >= %u verts): wipers=%d %.0f km/h, glass-drop draws %u%s", secs,
             kBigDrawVerts, g_wipers.load() ? 1 : 0, g_speed_kmh.load(), g_sec_glass_draws,
             g_big_dropped ? " (some groups dropped)" : "");
    std::string s = b;
    for (int i = 0; i < g_big_n; ++i) {
        const BigDraw& d = g_big[i];
        const PsEntry* e = Find(d.ps);
        std::string label = e ? LabelOf(*e) : std::string("no pixel shader");
        if (!e || e->label.load() < 0) label += " @" + Hex(reinterpret_cast<uintptr_t>(d.ps));
        if (d.format >= 0)
            snprintf(b, sizeof(b), "\n    %s | vp %.0fx%.0f | tex %ux%u f%d | %u draws, %llu verts, n %u-%u", label.c_str(),
                     d.vw, d.vh, d.tw, d.th, d.format, d.draws, static_cast<unsigned long long>(d.verts), d.min_n, d.max_n);
        else
            snprintf(b, sizeof(b), "\n    %s | vp %.0fx%.0f | no texture | %u draws, %llu verts, n %u-%u", label.c_str(), d.vw,
                     d.vh, d.draws, static_cast<unsigned long long>(d.verts), d.min_n, d.max_n);
        s += b;
    }
    Emit(s);
    g_big_n = 0;
    g_big_dropped = 0;
}

void RainSecond(uint64_t now) {
    if (!g_rain_sec_t0) g_rain_sec_t0 = now;
    if (now - g_rain_sec_t0 < 1000) return;
    BigDrawSecond((now - g_rain_sec_t0) / 1000.0);
    const bool active = g_rain_vp_n > 0 || g_sec_glass_draws > 0;
    if (g_cfg.verbose && (active || g_rain_was_active)) {
        char b[256];
        snprintf(b, sizeof(b), "rain %.1f s: wipers=%d %.0f km/h, glass-drop draws %u", (now - g_rain_sec_t0) / 1000.0,
                 g_wipers.load() ? 1 : 0, g_speed_kmh.load(), g_sec_glass_draws);
        std::string s = b;
        if (!active) s += " | nothing this second";
        for (int i = 0; i < g_rain_vp_n; ++i) {
            const RainVp& r = g_rain_vp[i];
            snprintf(b, sizeof(b), " | %s vp %.0fx%.0f: %u draws, %llu verts (last n=%u)", kRainNames[r.kind], r.w, r.h,
                     r.draws, static_cast<unsigned long long>(r.verts), r.last_n);
            s += b;
        }
        Emit(s);
    }
    g_rain_was_active = active;
    g_rain_last_second.store(active, std::memory_order_relaxed);
    g_rain_last_ms.store(now, std::memory_order_relaxed);
    g_rain_vp_n = 0;
    g_sec_glass_draws = 0;
    g_rain_sec_t0 = now;
}

void RainOnDraw(ID3D11DeviceContext* c, DrawKind kind, UINT count, UINT instances) {
    int rain = kRainNone;
    for (uint32_t m = g_rain_mask; m; m &= m - 1) {
        unsigned long s = 0;
        _BitScanForward(&s, m);
        rain = (std::max)(rain, g_rain_cands[g_slot_cand[s] - 1].kind.load(std::memory_order_relaxed));
    }
    const bool big = (kind == kDraw || kind == kDrawInstanced) && count >= kBigDrawVerts;
    if (!rain && big) rain = CheckByDescription(c);
    if (rain) NoteRainDraw(c, rain, count, instances);
    if (big && g_cfg.verbose) NoteBigDraw(c, count, instances);
}

// ---- census ----

uint64_t g_census_t0 = 0;

void Census(uint64_t now) {
    struct Row {
        uint32_t draws;
        PsEntry* e;
    };
    std::vector<Row> rows;
    uint64_t total = 0, glass = 0;
    for (PsEntry& e : g_table) {
        if (!e.ps.load(std::memory_order_relaxed) || !e.window_draws) continue;
        rows.push_back({e.window_draws, &e});
        total += e.window_draws;
        if (e.glass) glass += e.window_draws;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.draws > b.draws; });
    const double secs = (now - g_census_t0) / 1000.0;
    char b[320];
    snprintf(b, sizeof(b),
             "census %.0f s: %.0f draws/s, %zu pixel shaders used, glass-drop draws %.1f/s; created since hook %d "
             "(catalog %d, unknown %d), table %d%s; wipers=%d %.0f km/h%s",
             secs, total / secs, rows.size(), glass / secs, g_created.load(), g_created_named.load(),
             g_created_unknown.load(), g_entries.load(), g_table_full.load() ? " (full)" : "",
             g_wipers.load() ? 1 : 0, g_speed_kmh.load(), g_paused.load() ? ", paused" : "");
    std::string s = b;
    int watched = 0;
    {
        std::lock_guard<std::mutex> lock(g_rain_mutex);
        watched = g_rain_cand_n - static_cast<int>(g_rain_free.size());
    }
    snprintf(b, sizeof(b), "\n    rain textures: %d watched, %d recognised, %d other data%s", watched,
             g_rain_recognised.load(), g_rain_unmatched.load(), g_rain_cands_full.load() ? " (table full)" : "");
    s += b;
    // Shaders without a catalog name also get their pointer, to match them with the first-draw lines.
    auto row_text = [&](const Row& r) {
        std::string label = LabelOf(*r.e);
        if (r.e->label.load() < 0) label += " @" + Hex(reinterpret_cast<uintptr_t>(r.e->ps.load()));
        snprintf(b, sizeof(b), "\n    %9.1f/s %s", r.draws / secs, label.c_str());
        return std::string(b);
    };
    const size_t top = (std::min)(rows.size(), size_t(60));
    for (size_t i = 0; i < top; ++i) s += row_text(rows[i]);
    // Every glass-drop shader used, even when it is not in the top list.
    for (size_t i = top; i < rows.size(); ++i)
        if (rows[i].e->glass) s += row_text(rows[i]);
    Emit(s);
    for (Row& r : rows) r.e->window_draws = 0;
    g_census_t0 = now;
}

// ---- V3a: frame capture ----
//
// One frame, Present to Present, every capture_s seconds while the game is
// not paused. At each draw of that frame the state the draw uses is recorded
// and the GPU copies the first kCapCbBytes of the vertex-shader constant
// buffers 0-3 into one staging buffer (where the tire plugin found each
// wheel's matrix: VS cb0 rows 0-2 [verified: tire plugin R4a]). At the next
// Present the staging buffer is read once (one stall per capture) and a writer
// thread turns it into text, so only that one frame is slower.

constexpr int kCapMaxDraws = 16384;  // the game draws up to ~7000 per frame [estimate: V2 census, 720k draws/s]
constexpr int kCapCbSlots = 4;
constexpr UINT kCapCbBytes = 256;  // 16 rows of float4
constexpr UINT kCapStride = kCapCbSlots * kCapCbBytes;

struct CapCb {
    void* buf = nullptr;
    UINT first = 0, num = 0, width = 0;  // num == 0: bound without a range (whole buffer)
    UINT copied = 0;                     // bytes copied to the staging buffer
    bool dynamic = false;
};

struct CapDraw {
    DrawKind kind = kDraw;
    UINT count = 0, instances = 1, start = 0, start_inst = 0;
    INT base = 0;
    int verdict = -99;  // N28: tint::LastVerdict after the draw's classification (-99: not a DrawIndexed)
    void* ps = nullptr;
    void* vs = nullptr;
    int targets = 0;
    int rt_format[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    UINT rt_w = 0, rt_h = 0;
    int ds_format = -1;  // -1 = no depth buffer
    float vp[4] = {};
    bool has_dss = false, depth_on = false, depth_write = false, stencil_on = false;
    int depth_func = 0, st_func = 0, st_pass = 0;
    UINT st_ref = 0;
    UINT8 st_read = 0, st_write = 0;
    bool has_blend = false, blend_on = false;
    int blend_src = 0, blend_dst = 0;
    UINT8 write_mask = 0;
    void* ib = nullptr;
    int ib_format = 0;
    UINT ib_offset = 0;
    void* vb[3] = {};
    UINT vb_stride[3] = {}, vb_offset[3] = {};
    void* tex = nullptr;
    UINT tex_w = 0, tex_h = 0;
    int tex_format = 0;
    CapCb cb[kCapCbSlots];
};

struct CapJob {
    int number = 0;
    SYSTEMTIME time = {};
    double frame_ms = 0;
    int dropped = 0;
    Pose pose;
    std::vector<CapDraw> draws;
    std::vector<uint8_t> data;  // kCapStride bytes per draw
    std::unordered_map<void*, std::string> labels;
};

std::atomic<bool> g_cap_on{false};  // capture in progress (render thread)
std::vector<CapDraw> g_cap;
int g_cap_n = 0, g_cap_dropped = 0, g_cap_taken = 0, g_cap_errors = 0;
uint64_t g_cap_next_ms = 0, g_cap_t0_ms = 0;
LARGE_INTEGER g_cap_t0_qpc = {};
Pose g_cap_pose;
SYSTEMTIME g_cap_time = {};
ID3D11Buffer* g_cap_staging = nullptr;
ID3D11DeviceContext1* g_cap_ctx1 = nullptr;
std::thread g_cap_writer;
std::atomic<bool> g_cap_writing{false};

std::string Hp(const void* p) { return Hex(reinterpret_cast<uintptr_t>(p)); }

void CaptureDraw(ID3D11DeviceContext* c, DrawKind kind, UINT count, UINT instances, UINT start, INT base,
                 UINT start_inst) {
    if (g_cap_n >= kCapMaxDraws) {
        ++g_cap_dropped;
        return;
    }
    const int index = g_cap_n++;
    CapDraw& d = g_cap[index];
    d = CapDraw();
    d.kind = kind, d.count = count, d.instances = instances, d.start = start, d.base = base, d.start_inst = start_inst;
    d.ps = g_current_ps;
    ID3D11VertexShader* vs = nullptr;
    c->VSGetShader(&vs, nullptr, nullptr);
    d.vs = vs;
    if (vs) vs->Release();  // only the address is kept

    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (!rtv[i]) continue;
        d.targets = i + 1;
        D3D11_RENDER_TARGET_VIEW_DESC vd = {};
        rtv[i]->GetDesc(&vd);
        d.rt_format[i] = static_cast<int>(vd.Format);
        if (!d.rt_w) {
            ID3D11Resource* res = nullptr;
            rtv[i]->GetResource(&res);
            ID3D11Texture2D* tex = nullptr;
            if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
                D3D11_TEXTURE2D_DESC td = {};
                tex->GetDesc(&td);
                d.rt_w = td.Width, d.rt_h = td.Height;
                tex->Release();
            }
            if (res) res->Release();
        }
        rtv[i]->Release();
    }
    if (dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC vd = {};
        dsv->GetDesc(&vd);
        d.ds_format = static_cast<int>(vd.Format);
        dsv->Release();
    }
    D3D11_VIEWPORT vp = {};
    UINT nvp = 1;
    c->RSGetViewports(&nvp, &vp);
    if (nvp) d.vp[0] = vp.Width, d.vp[1] = vp.Height, d.vp[2] = vp.TopLeftX, d.vp[3] = vp.TopLeftY;

    ID3D11DepthStencilState* dss = nullptr;
    c->OMGetDepthStencilState(&dss, &d.st_ref);
    if (dss) {
        D3D11_DEPTH_STENCIL_DESC sd = {};
        dss->GetDesc(&sd);
        d.has_dss = true;
        d.depth_on = sd.DepthEnable != 0, d.depth_write = sd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
        d.depth_func = sd.DepthFunc;
        d.stencil_on = sd.StencilEnable != 0, d.st_read = sd.StencilReadMask, d.st_write = sd.StencilWriteMask;
        d.st_func = sd.FrontFace.StencilFunc, d.st_pass = sd.FrontFace.StencilPassOp;
        dss->Release();
    }
    ID3D11BlendState* bs = nullptr;
    float factor[4];
    UINT sample_mask = 0;
    c->OMGetBlendState(&bs, factor, &sample_mask);
    if (bs) {
        D3D11_BLEND_DESC bd = {};
        bs->GetDesc(&bd);
        d.has_blend = true;
        d.blend_on = bd.RenderTarget[0].BlendEnable != 0;
        d.blend_src = bd.RenderTarget[0].SrcBlend, d.blend_dst = bd.RenderTarget[0].DestBlend;
        d.write_mask = bd.RenderTarget[0].RenderTargetWriteMask;
        bs->Release();
    }

    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ib_format = DXGI_FORMAT_UNKNOWN;
    c->IAGetIndexBuffer(&ib, &ib_format, &d.ib_offset);
    d.ib = ib, d.ib_format = static_cast<int>(ib_format);
    if (ib) ib->Release();
    ID3D11Buffer* vb[3] = {};
    c->IAGetVertexBuffers(0, 3, vb, d.vb_stride, d.vb_offset);
    for (int i = 0; i < 3; ++i) {
        d.vb[i] = vb[i];
        if (vb[i]) vb[i]->Release();
    }
    ID3D11ShaderResourceView* srv = nullptr;
    c->PSGetShaderResources(0, 1, &srv);
    if (srv) {
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        d.tex = res;
        ID3D11Texture2D* tex = nullptr;
        if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
            D3D11_TEXTURE2D_DESC td = {};
            tex->GetDesc(&td);
            d.tex_w = td.Width, d.tex_h = td.Height, d.tex_format = static_cast<int>(td.Format);
            tex->Release();
        }
        if (res) res->Release();
        srv->Release();
    }

    ID3D11Buffer* cbs[kCapCbSlots] = {};
    UINT first[kCapCbSlots] = {}, num[kCapCbSlots] = {};
    if (g_cap_ctx1) g_cap_ctx1->VSGetConstantBuffers1(0, kCapCbSlots, cbs, first, num);
    else c->VSGetConstantBuffers(0, kCapCbSlots, cbs);
    for (int s = 0; s < kCapCbSlots; ++s) {
        if (!cbs[s]) continue;
        CapCb& cb = d.cb[s];
        D3D11_BUFFER_DESC bd = {};
        cbs[s]->GetDesc(&bd);
        cb.buf = cbs[s], cb.first = first[s], cb.num = num[s], cb.width = bd.ByteWidth;
        cb.dynamic = bd.Usage == D3D11_USAGE_DYNAMIC;
        const UINT offset = num[s] ? first[s] * 16 : 0;
        const UINT avail = bd.ByteWidth > offset ? bd.ByteWidth - offset : 0;
        const UINT range = num[s] ? (std::min)(avail, num[s] * 16) : avail;
        const UINT bytes = (std::min)(range, kCapCbBytes) & ~15u;
        if (bytes && g_cap_staging && bd.Usage != D3D11_USAGE_STAGING) {
            const D3D11_BOX box = {offset, 0, 0, offset + bytes, 1, 1};
            c->CopySubresourceRegion(g_cap_staging, 0, index * kCapStride + s * kCapCbBytes, 0, 0, cbs[s], 0, &box);
            cb.copied = bytes;
        }
        cbs[s]->Release();
    }
}

void FormatDraw(std::string& s, const CapJob& job, int i, std::unordered_map<std::string, int>& seen) {
    const CapDraw& d = job.draws[i];
    char b[512];
    auto label = job.labels.find(d.ps);
    snprintf(b, sizeof(b), "d %d %s n=%u x%u start=%u base=%d inst0=%u | ps %s %s | vs %s | rt %d", i,
             kDrawNames[d.kind], d.count, d.instances, d.start, d.base, d.start_inst, Hp(d.ps).c_str(),
             label == job.labels.end() ? "-" : label->second.c_str(), Hp(d.vs).c_str(), d.targets);
    s += b;
    for (int t = 0; t < d.targets; ++t) s += (t ? "," : ":") + std::string("f") + std::to_string(d.rt_format[t]);
    snprintf(b, sizeof(b), " %ux%u | ds %d | vp %.0fx%.0f@%.0f,%.0f", d.rt_w, d.rt_h, d.ds_format, d.vp[0], d.vp[1],
             d.vp[2], d.vp[3]);
    s += b;
    if (d.has_dss) {
        snprintf(b, sizeof(b), " | depth %d/%d/f%d stencil %d ref %u r%02x w%02x f%d p%d", d.depth_on, d.depth_write,
                 d.depth_func, d.stencil_on, d.st_ref, d.st_read, d.st_write, d.st_func, d.st_pass);
        s += b;
    }
    if (d.has_blend) {
        snprintf(b, sizeof(b), " | blend %d %d/%d m%x", d.blend_on, d.blend_src, d.blend_dst, d.write_mask);
        s += b;
    }
    if (d.verdict != -99) {  // N28
        snprintf(b, sizeof(b), " | tint %d", d.verdict);
        s += b;
    }
    snprintf(b, sizeof(b), " | ib %s f%d +%u | vb %s/%u/%u %s/%u %s/%u | tex %s %ux%u f%d\n", Hp(d.ib).c_str(),
             d.ib_format, d.ib_offset, Hp(d.vb[0]).c_str(), d.vb_stride[0], d.vb_offset[0], Hp(d.vb[1]).c_str(),
             d.vb_stride[1], Hp(d.vb[2]).c_str(), d.vb_stride[2], Hp(d.tex).c_str(), d.tex_w, d.tex_h, d.tex_format);
    s += b;
    for (int k = 0; k < kCapCbSlots; ++k) {
        const CapCb& cb = d.cb[k];
        if (!cb.buf) continue;
        snprintf(b, sizeof(b), "  vs_cb%d %s first %u num %u width %u%s", k, Hp(cb.buf).c_str(), cb.first, cb.num,
                 cb.width, cb.dynamic ? " dyn" : "");
        s += b;
        if (!cb.copied) {
            s += " (not copied)\n";
            continue;
        }
        const uint8_t* bytes = job.data.data() + size_t(i) * kCapStride + k * kCapCbBytes;
        // Per-frame constants repeat in many draws: print them once.
        std::string key(reinterpret_cast<const char*>(bytes), cb.copied);
        key += Hp(cb.buf);
        auto it = seen.find(key);
        if (it != seen.end()) {
            s += " = same rows as d " + std::to_string(it->second) + "\n";
            continue;
        }
        seen.emplace(std::move(key), i);
        s += "\n";
        const float* f = reinterpret_cast<const float*>(bytes);
        for (UINT r = 0; r < cb.copied / 16; ++r) {
            snprintf(b, sizeof(b), "    r%u %.7g %.7g %.7g %.7g\n", r, f[r * 4], f[r * 4 + 1], f[r * 4 + 2], f[r * 4 + 3]);
            s += b;
        }
    }
}

void WriteWheels(FILE* f, const char* unit, const PoseWheels& w) {
    for (int i = 0; i < w.count && i < kPoseWheels; ++i)
        fprintf(f, "# %s wheel %d rotation %.5f steering %.5f deflection %.4f lift %.3f\n", unit, i, w.rotation[i],
                w.steering[i], w.deflection[i], w.lift[i]);
}

void WriteCapture(CapJob* job) {
    const std::wstring dir = g_dir + L"\\captures";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[64];
    swprintf(name, 64, L"\\capture_%03d.txt", job->number);
    FILE* f = _wfopen((dir + name).c_str(), L"wb");
    if (!f) {
        Emit("capture " + std::to_string(job->number) + ": cannot write the file");
        delete job;
        g_cap_writing.store(false);
        return;
    }
    const SYSTEMTIME& t = job->time;
    const Pose& p = job->pose;
    fprintf(f, "# capture %d at %02d:%02d:%02d.%03d: %zu draws%s, frame %.1f ms, %.1f km/h\n", job->number, t.wHour,
            t.wMinute, t.wSecond, t.wMilliseconds, job->draws.size(),
            job->dropped ? (" (" + std::to_string(job->dropped) + " more not recorded)").c_str() : "", job->frame_ms,
            p.speed_kmh);
    fprintf(f, "# truck placement %.4f %.4f %.4f h %.6f p %.6f r %.6f\n", p.truck[0], p.truck[1], p.truck[2], p.truck[3],
            p.truck[4], p.truck[5]);
    fprintf(f, "# trailer.0 connected %d placement %.4f %.4f %.4f h %.6f p %.6f r %.6f\n", p.trailer_connected ? 1 : 0,
            p.trailer[0], p.trailer[1], p.trailer[2], p.trailer[3], p.trailer[4], p.trailer[5]);
    fprintf(f, "# cabin offset %.4f %.4f %.4f h %.6f p %.6f r %.6f\n", p.cabin[0], p.cabin[1], p.cabin[2], p.cabin[3],
            p.cabin[4], p.cabin[5]);
    WriteWheels(f, "truck", p.truck_wheels);
    WriteWheels(f, "trailer.0", p.trailer_wheels);
    // N21 (step 1): the trailers behind trailer.0 (a road train's dollies and trailers), the same lines.
    for (int i = 0; i < kPoseMoreTrailers; ++i) {
        const PoseTrailer& m = p.more[i];
        if (!m.wheels.count) continue;
        const std::string tid = "trailer." + std::to_string(i + 1);
        fprintf(f, "# %s connected %d placement %.4f %.4f %.4f h %.6f p %.6f r %.6f\n", tid.c_str(), m.connected ? 1 : 0,
                m.place[0], m.place[1], m.place[2], m.place[3], m.place[4], m.place[5]);
        WriteWheels(f, tid.c_str(), m.wheels);
    }
    std::unordered_map<std::string, int> seen;
    std::string s;
    for (int i = 0; i < static_cast<int>(job->draws.size()); ++i) {
        FormatDraw(s, *job, i, seen);
        if (s.size() > (1 << 20)) {
            fwrite(s.data(), 1, s.size(), f);
            s.clear();
        }
    }
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    Emit("capture " + std::to_string(job->number) + " written: " + std::to_string(job->draws.size()) + " draws", true);
    delete job;
    g_cap_writing.store(false);
}

void BeginCapture(ID3D11DeviceContext* c, uint64_t now) {
    if (!g_cap_staging) {
        ID3D11Device* dev = nullptr;
        c->GetDevice(&dev);
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = kCapMaxDraws * kCapStride;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const HRESULT hr = dev ? dev->CreateBuffer(&d, nullptr, &g_cap_staging) : E_POINTER;
        if (dev) dev->Release();
        if (FAILED(hr)) {
            Emit("capture off: staging buffer not created, hr=" + Hex(static_cast<uint32_t>(hr)), true);
            g_cfg.capture_s = 0;
            return;
        }
        if (FAILED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_cap_ctx1))))
            g_cap_ctx1 = nullptr;
        g_cap.resize(kCapMaxDraws);
    }
    {
        std::lock_guard<std::mutex> lock(g_pose_mutex);
        g_cap_pose = g_pose;
    }
    GetLocalTime(&g_cap_time);
    QueryPerformanceCounter(&g_cap_t0_qpc);
    g_cap_t0_ms = now;
    g_cap_n = g_cap_dropped = 0;
    g_cap_on.store(true, std::memory_order_relaxed);
}

void FinishCapture(ID3D11DeviceContext* c) {
    g_cap_on.store(false, std::memory_order_relaxed);
    LARGE_INTEGER t1, freq;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    auto* job = new CapJob();
    job->number = ++g_cap_taken;
    job->time = g_cap_time;
    job->frame_ms = (t1.QuadPart - g_cap_t0_qpc.QuadPart) * 1000.0 / freq.QuadPart;
    job->dropped = g_cap_dropped;
    job->pose = g_cap_pose;
    job->draws.assign(g_cap.begin(), g_cap.begin() + g_cap_n);
    // The copies are in the GPU queue: this Map waits for them (the one stall of a capture).
    D3D11_MAPPED_SUBRESOURCE m = {};
    const HRESULT hr = c->Map(g_cap_staging, 0, D3D11_MAP_READ, 0, &m);
    if (SUCCEEDED(hr)) {
        job->data.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + size_t(g_cap_n) * kCapStride);
        c->Unmap(g_cap_staging, 0);
    } else {
        if (g_cap_errors++ < 5) Emit("capture: staging Map failed, hr=" + Hex(static_cast<uint32_t>(hr)));
        job->data.assign(size_t(g_cap_n) * kCapStride, 0);
        for (CapDraw& d : job->draws)
            for (CapCb& cb : d.cb) cb.copied = 0;
    }
    for (const CapDraw& d : job->draws)
        if (d.ps && !job->labels.count(d.ps)) {
            const PsEntry* e = Find(d.ps);
            job->labels[d.ps] = e ? LabelOf(*e) : std::string("not in the table");
        }
    if (g_cap_writer.joinable()) g_cap_writer.join();  // the previous one is done (g_cap_writing was false)
    g_cap_writing.store(true);
    g_cap_writer = std::thread(WriteCapture, job);
}

// Render thread, at every Present of the game: frame boundaries for V3a.
void OnPresent() {
    if (!g_active.load(std::memory_order_relaxed) || g_failed.load(std::memory_order_relaxed)) return;
    ID3D11DeviceContext* c = static_cast<ID3D11DeviceContext*>(g_immediate.load(std::memory_order_relaxed));
    if (!c) return;
    const uint64_t now = GetTickCount64();
    if (g_cap_on.load(std::memory_order_relaxed)) {
        if (g_cap_n == 0) return;  // nothing drawn yet (a Present inside a Present)
        FinishCapture(c);
        g_cap_next_ms = now + static_cast<uint64_t>(g_cfg.capture_s) * 1000;
        return;
    }
    if (g_cfg.capture_s <= 0 || g_cap_taken >= g_cfg.captures || g_cap_writing.load()) return;
    if (!g_cap_next_ms) g_cap_next_ms = now + static_cast<uint64_t>(g_cfg.capture_s) * 1000;
    if (now < g_cap_next_ms || g_paused.load(std::memory_order_relaxed)) return;
    BeginCapture(c, now);
}

void GuardedPresent() {
    __try {
        OnPresent();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_failed.store(true);
    }
}

// ---- per draw ----

void DrawBody(ID3D11DeviceContext* c, DrawKind kind, UINT count, UINT instances, UINT start, INT base,
              UINT start_inst) {
    if (g_cap_on.load(std::memory_order_relaxed)) CaptureDraw(c, kind, count, instances, start, base, start_inst);
    const uint64_t now = GetTickCount64();
    if (!g_census_t0) g_census_t0 = now;
    if (g_current_ps) {
        PsEntry* e = Find(g_current_ps);
        if (!e) e = Insert(g_current_ps, kLabelBeforeHook, nullptr);
        if (e) {
            if (e->reset.exchange(false, std::memory_order_acq_rel)) {
                e->described = false;
                e->glass = 0;
            }
            if (!e->described) {
                const int l = e->label.load(std::memory_order_acquire);
                if (l >= 0 && g_is_glass[l]) e->glass = 1;
                e->described = true;
                DescribeDraw(c, e, kind, count, instances);
            }
            ++e->window_draws;
            ++e->total_draws;
            if (e->glass) ++g_sec_glass_draws;
            if (g_cfg.verbose && e->glass && now - e->last_sample_ms >= static_cast<uint64_t>(g_cfg.sample_ms))
                Sample(c, e, now);
        }
    }
    RainOnDraw(c, kind, count, instances);
    RainSecond(now);
    if (g_pending) PollReadbacks(c, now);
    if (now - g_census_t0 >= static_cast<uint64_t>(g_cfg.census_s) * 1000) Census(now);
}

void GuardedDraw(ID3D11DeviceContext* c, DrawKind kind, UINT count, UINT instances, UINT start, INT base,
                 UINT start_inst) {
    __try {
        DrawBody(c, kind, count, instances, start, base, start_inst);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_failed.store(true);  // reported from the game thread (TakeMessages)
    }
}

inline void OnDraw(ID3D11DeviceContext* c, DrawKind kind, UINT count, UINT instances, UINT start, INT base = 0,
                   UINT start_inst = 0) {
    if (!g_active.load(std::memory_order_relaxed) || g_failed.load(std::memory_order_relaxed)) return;
    if (!IsImmediate(c)) {  // N27: counted only (a command list the game or a mod records)
        if (tint::On() && !g_tint_failed.load(std::memory_order_relaxed)) tint::OnDeferredDraw(c);
        return;
    }
    if (TintBusy()) return;
    GuardedDraw(c, kind, count, instances, start, base, start_inst);
}

// ---- V3 part 2: tint (see tint.h) ----
//
// The state detours feed tint's view of the game state; they only forward
// while tint runs its own calls. Any exception turns tint off for the session
// (the game keeps drawing normally): the game's depth-stencil state comes back
// and the open pass is released (tint::Abandon; the pass's own state is
// restored by EndPass).

// D13e: the exception's code and address kept for the log.
int TintFilter(EXCEPTION_POINTERS* e) {
    if (e && e->ExceptionRecord) {
        g_tint_code.store(static_cast<unsigned>(e->ExceptionRecord->ExceptionCode));
        g_tint_addr.store(reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress));
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// After an exception in the tint path: tint off, its leftovers cleaned. D13e: the first one while the pass drew in
// its rectangles turns only those off.
void TintFailed(ID3D11DeviceContext* c) {
    g_tint_exceptions.fetch_add(1);
    if (tint::RectsFallback()) g_tint_rects_off.store(true);
    else g_tint_failed.store(true);
    __try {
        tint::Abandon(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

inline bool TintLive(ID3D11DeviceContext* c) {
    return g_active.load(std::memory_order_relaxed) && tint::On() && !tint::Busy() &&
           !g_tint_failed.load(std::memory_order_relaxed) && IsImmediate(c);
}

bool GuardedTintBefore(ID3D11DeviceContext* c, UINT n, UINT start, INT base) {
    __try {
        return tint::BeforeDrawIndexed(c, n, start, base);
    } __except (TintFilter(GetExceptionInformation())) {
        TintFailed(c);
        return false;
    }
}

void GuardedTintAfter(ID3D11DeviceContext* c) {
    __try {
        tint::AfterDraw(c);
    } __except (TintFilter(GetExceptionInformation())) {
        TintFailed(c);
    }
}

void GuardedTintTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    __try {
        tint::OnSetRenderTargets(c, n, rtvs, dsv);
    } __except (TintFilter(GetExceptionInformation())) {
        TintFailed(c);
    }
}

void GuardedTintDss(ID3D11DeviceContext* c, ID3D11DepthStencilState** s, UINT* ref) {
    __try {
        tint::FilterDepthStencilState(c, s, ref);
    } __except (TintFilter(GetExceptionInformation())) {
        TintFailed(c);
    }
}

void GuardedTintPresent(ID3D11DeviceContext* c) {
    tint::GameState game;
    game.paused = g_paused.load(std::memory_order_relaxed);
    game.wipers = g_wipers.load(std::memory_order_relaxed);
    game.rain = RainActive();
    game.truck = g_truck_known.load(std::memory_order_relaxed);
    game.trailer = g_trailer_coupled.load(std::memory_order_relaxed);
    __try {
        tint::OnPresent(c, game);
    } __except (TintFilter(GetExceptionInformation())) {
        TintFailed(c);
    }
}

void GuardedTintMap(ID3D11DeviceContext* c, ID3D11Resource* res, const D3D11_MAPPED_SUBRESOURCE* m) {
    __try {
        tint::OnMap(res, m);
    } __except (TintFilter(GetExceptionInformation())) {
        TintFailed(c);
    }
}

using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
using VSSetCbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using VSSetCb1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, UINT, UINT, ID3D11Buffer* const*, const UINT*,
                                            const UINT*);
using IASetVbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*,
                                           const UINT*);
using IASetIbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, DXGI_FORMAT, UINT);
using OMSetRtFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                           ID3D11DepthStencilView*);
using OMSetRtUavFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                              ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*,
                                              const UINT*);
using OMSetDssFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
using RSSetVpFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
MapFn g_orig_map = nullptr;
VSSetCbFn g_orig_vs_set_cb = nullptr;
VSSetCb1Fn g_orig_vs_set_cb1 = nullptr;
IASetVbFn g_orig_ia_set_vb = nullptr;
IASetIbFn g_orig_ia_set_ib = nullptr;
OMSetRtFn g_orig_om_set_rt = nullptr;
OMSetRtUavFn g_orig_om_set_rt_uav = nullptr;
OMSetDssFn g_orig_om_set_dss = nullptr;
RSSetVpFn g_orig_rs_set_vp = nullptr;

HRESULT STDMETHODCALLTYPE HookMap(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags,
                                  D3D11_MAPPED_SUBRESOURCE* m) {
    const HRESULT hr = g_orig_map(c, res, sub, type, flags, m);
    if (SUCCEEDED(hr) && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE) && TintLive(c))
        GuardedTintMap(c, res, m);
    return hr;
}

void STDMETHODCALLTYPE HookVSSetCb(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b) {
    if (TintLive(c)) tint::OnSetVSConstantBuffers(start, n, b, nullptr, nullptr);
    g_orig_vs_set_cb(c, start, n, b);
}

void STDMETHODCALLTYPE HookVSSetCb1(ID3D11DeviceContext1* c, UINT start, UINT n, ID3D11Buffer* const* b,
                                    const UINT* first, const UINT* num) {
    if (TintLive(c)) tint::OnSetVSConstantBuffers(start, n, b, first, num);
    g_orig_vs_set_cb1(c, start, n, b, first, num);
}

void STDMETHODCALLTYPE HookIASetVb(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b,
                                   const UINT* strides, const UINT* offsets) {
    if (TintLive(c)) tint::OnSetVertexBuffers(start, n, b);
    g_orig_ia_set_vb(c, start, n, b, strides, offsets);
}

void STDMETHODCALLTYPE HookIASetIb(ID3D11DeviceContext* c, ID3D11Buffer* b, DXGI_FORMAT f, UINT offset) {
    if (TintLive(c)) tint::OnSetIndexBuffer(b);
    g_orig_ia_set_ib(c, b, f, offset);
}

void STDMETHODCALLTYPE HookOMSetRt(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                                   ID3D11DepthStencilView* dsv) {
    if (TintLive(c)) GuardedTintTargets(c, n, rtvs, dsv);
    g_orig_om_set_rt(c, n, rtvs, dsv);
}

void STDMETHODCALLTYPE HookOMSetRtUav(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                                      ID3D11DepthStencilView* dsv, UINT uav_start, UINT uavs,
                                      ID3D11UnorderedAccessView* const* u, const UINT* counts) {
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && TintLive(c)) GuardedTintTargets(c, n, rtvs, dsv);
    g_orig_om_set_rt_uav(c, n, rtvs, dsv, uav_start, uavs, u, counts);
}

void STDMETHODCALLTYPE HookOMSetDss(ID3D11DeviceContext* c, ID3D11DepthStencilState* s, UINT ref) {
    if (TintLive(c)) GuardedTintDss(c, &s, &ref);
    g_orig_om_set_dss(c, s, ref);
}

void STDMETHODCALLTYPE HookRSSetVp(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
    if (TintLive(c)) tint::OnSetViewports(n, v);
    g_orig_rs_set_vp(c, n, v);
}

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* c, UINT n, UINT start, INT base) {
    const int before = g_cap_n;  // render thread: a capture entry made by OnDraw is this draw's
    OnDraw(c, kDrawIndexed, n, 1, start, base);
    const bool captured = IsImmediate(c) && g_cap_n == before + 1;
    const bool live = TintLive(c);
    const bool swapped = live && GuardedTintBefore(c, n, start, base);
    if (captured) g_cap[before].verdict = live ? tint::LastVerdict() : -9;  // N28; -9: tint not running
    g_orig_draw_indexed(c, n, start, base);
    if (swapped) GuardedTintAfter(c);
}

void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* c, UINT n, UINT start) {
    OnDraw(c, kDraw, n, 1, start);
    g_orig_draw(c, n, start);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, INT base,
                                                UINT start_inst) {
    OnDraw(c, kDrawIndexedInstanced, n, inst, start, base, start_inst);
    g_orig_draw_indexed_inst(c, n, inst, start, base, start_inst);
}

void CountIndirect(ID3D11DeviceContext* c) {
    if (g_active.load(std::memory_order_relaxed) && !g_failed.load(std::memory_order_relaxed) && tint::On() &&
        !g_tint_failed.load(std::memory_order_relaxed) && !TintBusy())
        tint::OnIndirectDraw(c, IsImmediate(c));
}

void STDMETHODCALLTYPE HookDrawAuto(ID3D11DeviceContext* c) {
    CountIndirect(c);
    g_orig_draw_auto(c);
}

void STDMETHODCALLTYPE HookDrawIndexedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset) {
    CountIndirect(c);
    g_orig_draw_indexed_indirect(c, args, offset);
}

void STDMETHODCALLTYPE HookDrawInstIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset) {
    CountIndirect(c);
    g_orig_draw_inst_indirect(c, args, offset);
}

void STDMETHODCALLTYPE HookDrawInstanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, UINT start_inst) {
    OnDraw(c, kDrawInstanced, n, inst, start, 0, start_inst);
    g_orig_draw_inst(c, n, inst, start, start_inst);
}

// V3a: Present / Present1 of the game's swap chain mark the frames.
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
PresentFn g_orig_present = nullptr;
Present1Fn g_orig_present1 = nullptr;

// A Present inside a Present (Present1 forwarding to Present) is one frame.
int g_present_depth = 0;

uint64_t g_tint_log_ms = 0;

// N10: the test panel, drawn last before the game's Present.
std::mutex g_panel_mutex;  // guards g_panel_view (game thread writes, render thread copies)
panel::View g_panel_view;
std::atomic<bool> g_panel_shown{false};
std::atomic<bool> g_panel_failed{false};
// Never deleted: at process exit the game's device may be gone already (the DLL is pinned).
panel_draw::Renderer& PanelRenderer() {
    static panel_draw::Renderer* r = new panel_draw::Renderer;
    return *r;
}

void DrawPanel(ID3D11DeviceContext* c, IDXGISwapChain* sc) {
    panel::View v;
    {
        std::lock_guard<std::mutex> lock(g_panel_mutex);
        v = g_panel_view;
    }
    // N12: until a unit has moved ~1 m only its wheels are known (and get dirt): say so.
    if (tint::On()) {
        const bool truck = g_truck_known.load(std::memory_order_relaxed) && tint::WheelsOnly(0);
        const bool trailer = g_trailer_coupled.load(std::memory_order_relaxed) && tint::WheelsOnly(1);
        v.notice = panel::LearningNotice(truck, trailer);
    }
    tint::SetBusy(true);  // our own calls: the detours only forward
    const bool ok = PanelRenderer().DrawOnSwapChain(c, sc, v);
    tint::SetBusy(false);
    if (!ok) {
        g_panel_failed.store(true);
        Emit(PanelRenderer().Error(), true);
    }
}

void GuardedPanel(ID3D11DeviceContext* c, IDXGISwapChain* sc) {
    __try {
        DrawPanel(c, sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        tint::SetBusy(false);
        g_panel_failed.store(true);
    }
}

// D5: the player's overlay (the wash at the station, the keys menu), drawn after the test panel.
std::mutex g_overlay_mutex;  // guards g_overlay_view
overlay::View g_overlay_view;
std::atomic<bool> g_overlay_shown{false};
std::atomic<bool> g_overlay_failed{false};
panel_draw::Renderer& OverlayRenderer() {
    static panel_draw::Renderer* r = new panel_draw::Renderer;  // never deleted, as PanelRenderer
    return *r;
}

void DrawOverlay(ID3D11DeviceContext* c, IDXGISwapChain* sc) {
    overlay::View v;
    {
        std::lock_guard<std::mutex> lock(g_overlay_mutex);
        v = g_overlay_view;
    }
    tint::SetBusy(true);  // our own calls: the detours only forward
    const bool ok = OverlayRenderer().DrawOverlayOnSwapChain(c, sc, v);
    tint::SetBusy(false);
    if (!ok) {
        g_overlay_failed.store(true);
        Emit(OverlayRenderer().Error(), true);
    }
}

void GuardedOverlay(ID3D11DeviceContext* c, IDXGISwapChain* sc) {
    __try {
        DrawOverlay(c, sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        tint::SetBusy(false);
        g_overlay_failed.store(true);
    }
}

void FrameEnd(IDXGISwapChain* sc) {
    GuardedPresent();
    ID3D11DeviceContext* c = static_cast<ID3D11DeviceContext*>(g_immediate.load(std::memory_order_relaxed));
    if (c && TintLive(c)) GuardedTintPresent(c);
    for (const std::string& l : tint::TakeCompatLines()) Emit(l, true);  // N11: decisions go to the game log too
    if (c && sc && g_panel_shown.load(std::memory_order_relaxed) && !g_panel_failed.load(std::memory_order_relaxed) &&
        g_active.load(std::memory_order_relaxed) && !g_failed.load(std::memory_order_relaxed) && !tint::Busy())
        GuardedPanel(c, sc);
    if (c && sc && g_overlay_shown.load(std::memory_order_relaxed) && !g_overlay_failed.load(std::memory_order_relaxed) &&
        g_active.load(std::memory_order_relaxed) && !g_failed.load(std::memory_order_relaxed) && !tint::Busy())
        GuardedOverlay(c, sc);
    if (!tint::On() || !g_active.load(std::memory_order_relaxed)) return;
    const uint64_t now = GetTickCount64();
    if (now - g_tint_log_ms < 10000) return;
    if (!g_tint_log_ms)
        Emit(g_cfg.tint == 1   ? "tint on: the player's truck and trailer are painted red in the G-buffer (V3 part 2 test)"
             : g_cfg.tint == 4 ? "diagnosis on (N26): the rig's pixels coloured by what the dust pass made of them"
                               : "dust on: the player's truck and trailer get their dust in the G-buffer (D3)",
             true);
    g_tint_log_ms = now;
    const std::string e = tint::TakeError();
    if (!e.empty()) Emit(e, true);
    static bool failure_reported = false, rects_reported = false;
    const auto exception_text = [] {
        char b[64];
        snprintf(b, sizeof(b), "exception 0x%08X at ", g_tint_code.load());
        return std::string(b) + Where(reinterpret_cast<const void*>(g_tint_addr.load()));
    };
    if (g_tint_rects_off.load() && !rects_reported) {  // D13e
        rects_reported = true;
        Emit("dust pass rectangles (D13d) off: " + exception_text() + "; the dust keeps the whole viewport", true);
    }
    if (g_tint_failed.load() && !failure_reported) {
        failure_reported = true;
        Emit("tint off: exception in the tint path (the game keeps drawing normally): " + exception_text(), true);
    }
    Emit(tint::StatsLine());
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST) && g_present_depth++ == 0) FrameEnd(sc);
    const HRESULT hr = g_orig_present(sc, sync, flags);
    if (!(flags & DXGI_PRESENT_TEST)) --g_present_depth;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (!(flags & DXGI_PRESENT_TEST) && g_present_depth++ == 0) FrameEnd(sc);
    const HRESULT hr = g_orig_present1(sc, sync, flags, p);
    if (!(flags & DXGI_PRESENT_TEST)) --g_present_depth;
    return hr;
}

// ---- locate and install ----

struct Targets {
    void* create_ps = nullptr;      // ID3D11Device slot 15
    void* ps_set = nullptr;         // ID3D11DeviceContext slot 9
    void* draw_indexed = nullptr;   // 12
    void* draw = nullptr;           // 13
    void* draw_indexed_inst = nullptr;  // 20
    void* draw_inst = nullptr;      // 21
    void* draw_auto = nullptr;      // 38 (N28)
    void* draw_indexed_indirect = nullptr;  // 39
    void* draw_inst_indirect = nullptr;     // 40
    // V2b
    void* create_tex = nullptr;     // ID3D11Device slot 5
    void* create_srv = nullptr;     // ID3D11Device slot 7
    void* ps_set_srv = nullptr;     // ID3D11DeviceContext slot 8
    void* update = nullptr;         // ID3D11DeviceContext slot 48
    void* update1 = nullptr;        // ID3D11DeviceContext1 slot 116
    // V3a
    void* present = nullptr;        // IDXGISwapChain slot 8
    void* present1 = nullptr;       // IDXGISwapChain1 slot 22
    // V3 part 2 (ID3D11DeviceContext slots; vs_set_cb1 is ID3D11DeviceContext1)
    void* map = nullptr;            // 14
    void* vs_set_cb = nullptr;      // 7
    void* vs_set_cb1 = nullptr;     // 119
    void* ia_set_vb = nullptr;      // 18
    void* ia_set_ib = nullptr;      // 19
    void* om_set_rt = nullptr;      // 33
    void* om_set_rt_uav = nullptr;  // 34
    void* om_set_dss = nullptr;     // 36
    void* rs_set_vp = nullptr;      // 44
};

// Plain C-style body so it can run under __try in the caller.
HRESULT FindTargets(Targets* out, const char** step) {
    wchar_t sysdir[MAX_PATH] = {};
    GetSystemDirectoryW(sysdir, MAX_PATH);
    wchar_t sys_dxgi_path[MAX_PATH] = {};
    swprintf(sys_dxgi_path, MAX_PATH, L"%s\\dxgi.dll", sysdir);
    // Never load anything new: both modules must already be in the process.
    *step = "system dxgi.dll";
    HMODULE sys_dxgi = GetModuleHandleW(sys_dxgi_path);
    if (!sys_dxgi) return E_FAIL;
    *step = "d3d11.dll";
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) return E_FAIL;
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    auto create_factory = reinterpret_cast<CreateFactoryFn>(GetProcAddress(sys_dxgi, "CreateDXGIFactory1"));
    auto create_device = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    *step = "exports";
    if (!create_factory || !create_device) return E_FAIL;

    HRESULT hr;
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* imm = nullptr;
    IDXGISwapChain* swap = nullptr;
    IDXGISwapChain1* swap1 = nullptr;
    HWND wnd = nullptr;
    // V3a: a 1x1 swap chain on a hidden window, only to read the Present address (as the tire plugin).
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ets2_truck_dirt_probe";
    RegisterClassExW(&wc);
    do {
        *step = "CreateDXGIFactory1";
        if (FAILED(hr = create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) break;
        *step = "EnumAdapters1";
        if (FAILED(hr = factory->EnumAdapters1(0, &adapter))) break;
        *step = "D3D11CreateDevice";
        const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(hr = create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &device,
                                      nullptr, &imm)))
            break;
        void** dvt = *reinterpret_cast<void***>(device);
        out->create_ps = dvt[15];
        void** cvt = *reinterpret_cast<void***>(imm);
        out->ps_set = cvt[9];
        out->draw_indexed = cvt[12];
        out->draw = cvt[13];
        out->draw_indexed_inst = cvt[20];
        out->draw_inst = cvt[21];
        out->draw_auto = cvt[38];
        out->draw_indexed_indirect = cvt[39];
        out->draw_inst_indirect = cvt[40];
        out->create_tex = dvt[5];
        out->create_srv = dvt[7];
        out->ps_set_srv = cvt[8];
        out->update = cvt[48];
        *step = "ID3D11DeviceContext1";
        ID3D11DeviceContext1* imm1 = nullptr;
        if (FAILED(hr = imm->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&imm1)))) break;
        out->update1 = (*reinterpret_cast<void***>(imm1))[116];
        out->vs_set_cb1 = (*reinterpret_cast<void***>(imm1))[119];
        out->map = cvt[14];
        out->vs_set_cb = cvt[7];
        out->ia_set_vb = cvt[18];
        out->ia_set_ib = cvt[19];
        out->om_set_rt = cvt[33];
        out->om_set_rt_uav = cvt[34];
        out->om_set_dss = cvt[36];
        out->rs_set_vp = cvt[44];
        imm1->Release();
        *step = "CreateWindow";
        wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        if (!wnd) {
            hr = E_FAIL;
            break;
        }
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferDesc.Width = 1;
        sd.BufferDesc.Height = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.OutputWindow = wnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        *step = "CreateSwapChain";
        if (FAILED(hr = factory->CreateSwapChain(device, &sd, &swap))) break;
        out->present = (*reinterpret_cast<void***>(swap))[8];
        if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swap1))))
            out->present1 = (*reinterpret_cast<void***>(swap1))[22];
        *step = "done";
    } while (false);
    if (swap1) swap1->Release();
    if (swap) swap->Release();
    if (wnd) DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (imm) imm->Release();
    if (device) device->Release();
    if (adapter) adapter->Release();
    if (factory) factory->Release();
    return hr;
}

HRESULT GuardedFind(Targets* out, const char** step, DWORD* seh_code) {
    __try {
        return FindTargets(out, step);
    } __except (*seh_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
}

// D6: other plugins (the tire plugin ets2_tire_wear among them) patch the same d3d11/dxgi functions with their own
// MinHook copy, from their own thread, at the same moment of the game start. Two installs at once can copy a
// jump another plugin is still writing into a trampoline, or suspend each other inside MinHook's thread freeze.
// Plugins that take this per-process named mutex install one at a time; it costs nothing when alone.
// ---- D10f: our jumps, watched after the install ----
//
// Another plugin's MinHook that read a target's first bytes before our jump went in (its install crossing ours; it
// does not take the install lock) writes its own jump over ours afterwards, its trampoline going on to the original
// code: our detour is no longer reached (29/10 21:35, ATS: the tire plugin's hooks went in 3 ms after ours; ours on
// Present, DrawIndexed and the other shared ones were skipped for the whole session: no dust, no panel). The first
// bytes are checked for a while after the install; when another MinHook's jump skips ours, ours goes back in front
// of it and calls it next, so both stay in the chain. A jump that only chains in front of ours is left alone.
struct Watched {
    const char* name = "";
    unsigned char* target = nullptr;
    void** original = nullptr;
    void* detour = nullptr;
    unsigned char ours[5] = {};         // our jump as last written
    unsigned char before[5] = {};       // the bytes it replaced (the start of our trampoline)
    const void* before_jump = nullptr;  // where they jumped (another hook already there), else null
    int put_back = 0;
    bool reported = false;
};
std::vector<Watched> g_watched;  // filled by Install, then only the watch thread
HANDLE g_stop = nullptr;         // set by Stop: the watch ends
constexpr int kMaxPutBack = 3;   // per target: more means another plugin keeps writing its jump

bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return static_cast<const unsigned char*>(p) + n <= static_cast<const unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
}

// 32-byte slots of executable memory within 2 GB of `close_to` (a rel32 jump from it reaches them).
unsigned char* NearSlot(const void* close_to) {
    struct Block {
        unsigned char* base;
        size_t used;
    };
    static std::vector<Block> blocks;
    constexpr size_t kSlot = 32, kBlock = 4096;
    const auto in_reach = [close_to](const unsigned char* p) {
        const long long d = p - static_cast<const unsigned char*>(close_to);
        return d > -0x7FF00000LL && d < 0x7FF00000LL;
    };
    for (Block& b : blocks)
        if (b.used + kSlot <= kBlock && in_reach(b.base)) {
            unsigned char* p = b.base + b.used;
            b.used += kSlot;
            return p;
        }
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t t = reinterpret_cast<uintptr_t>(close_to);
    const uintptr_t low = t > 0x7FE00000u ? t - 0x7FE00000u : gran;
    for (uintptr_t a = (t & ~(gran - 1)) - gran; a > low; a -= gran) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
        if (mbi.State != MEM_FREE) continue;
        void* p = VirtualAlloc(reinterpret_cast<void*>(a), kBlock, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!p) continue;
        blocks.push_back({static_cast<unsigned char*>(p), kSlot});
        return static_cast<unsigned char*>(p);
    }
    return nullptr;
}

// jmp [rip+0]; dq to (14 bytes).
void WriteAbsJump(unsigned char* at, const void* to) {
    const unsigned char op[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(at, op, sizeof(op));
    memcpy(at + 6, &to, sizeof(to));
}

// One 5-byte jump written in a single 8-byte store, so a thread running the target sees either jump whole; false
// when the bytes are not `expect` any more or the jump does not fit one aligned 8 bytes.
bool SwapJump(unsigned char* target, const unsigned char expect[5], const void* to) {
    const uintptr_t off = reinterpret_cast<uintptr_t>(target) & 7;
    const long long rel = static_cast<const unsigned char*>(to) - (target + 5);
    if (off > 3 || rel != static_cast<int32_t>(rel)) return false;
    auto* q = reinterpret_cast<LONG64*>(target - off);
    DWORD old = 0;
    if (!VirtualProtect(q, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    LONG64 cur = *reinterpret_cast<volatile LONG64*>(q), want = cur;
    bool ok = !memcmp(reinterpret_cast<unsigned char*>(&cur) + off, expect, 5);
    if (ok) {
        unsigned char* w = reinterpret_cast<unsigned char*>(&want) + off;
        const int32_t r = static_cast<int32_t>(rel);
        w[0] = 0xE9;
        memcpy(w + 1, &r, 4);
        ok = InterlockedCompareExchange64(q, want, cur) == cur;
    }
    VirtualProtect(q, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), q, 8);
    return ok;
}

void CheckJump(Watched& w) {
    unsigned char now[5];
    memcpy(now, w.target, sizeof(now));
    if (!memcmp(now, w.ours, sizeof(now))) return;
    // Another jump in place of ours. It skips ours when it leads to a MinHook relay (jmp [rip+0] in a 64-byte slot
    // that starts with the trampoline) whose trampoline starts with the bytes ours replaced.
    const auto* relay = now[0] == 0xE9 ? static_cast<const unsigned char*>(JumpTarget(w.target)) : nullptr;
    const bool minhook = relay && Readable(relay, 14) && relay[0] == 0xFF && relay[1] == 0x25 && !relay[2] &&
                         !relay[3] && !relay[4] && !relay[5];
    const auto* tramp =
        minhook ? reinterpret_cast<const unsigned char*>(reinterpret_cast<uintptr_t>(relay) & ~uintptr_t(63)) : nullptr;
    bool skips = false;
    if (tramp && tramp != relay && Readable(tramp, 14))
        skips = w.before_jump ? JumpTarget(tramp) == w.before_jump : !memcmp(tramp, w.before, sizeof(w.before));
    if (!skips) {
        Emit(std::string("hook ") + w.name + ": another jump in front of ours (" + Describe(w.target) +
                 "), not one that skips it: left in place",
             true);
        memcpy(w.ours, now, sizeof(now));  // watched from here on
        return;
    }
    if (w.put_back >= kMaxPutBack) {
        if (!w.reported)
            Emit(std::string("hook ") + w.name + ": another plugin's jump keeps skipping ours, left so (" +
                     Describe(w.target) + ")",
                 true);
        w.reported = true;
        return;
    }
    // Ours in front again: its jump to our detour, our detour going on to the other plugin's relay.
    unsigned char* slot = NearSlot(w.target);
    if (!slot) {
        if (!w.reported)
            Emit(std::string("hook ") + w.name + ": skipped by another plugin's jump, no memory to put ours back", true);
        w.reported = true;
        return;
    }
    WriteAbsJump(slot, w.detour);
    WriteAbsJump(slot + 16, relay);
    FlushInstructionCache(GetCurrentProcess(), slot, 32);
    void* const previous = InterlockedExchangePointer(w.original, slot + 16);
    if (!SwapJump(w.target, now, slot)) {
        InterlockedExchangePointer(w.original, previous);
        return;  // changed again meanwhile: the next check
    }
    memcpy(w.ours, w.target, sizeof(w.ours));
    ++w.put_back;
    Emit(std::string("hook ") + w.name + ": another plugin's jump went over ours and skipped it (" + Where(relay) +
             "): ours put back in front, calling it next",
         true);
}

// The first checks come soon (another install crossing ours ends within ~0.1 s), then rarer.
void Watch() {
    static const DWORD kDelays[] = {100, 150, 250, 500, 1000, 2000, 4000, 8000, 15000};
    for (size_t i = 0;; ++i) {
        const DWORD ms = i < sizeof(kDelays) / sizeof(kDelays[0]) ? kDelays[i] : 30000;
        if (WaitForSingleObject(g_stop, ms) != WAIT_TIMEOUT) return;
        for (Watched& w : g_watched) CheckJump(w);
    }
}

struct InstallLock {
    HANDLE h = nullptr;
    DWORD wait = WAIT_FAILED;
    bool held = false;
    InstallLock() {
        wchar_t name[64];
        swprintf(name, 64, L"Local\\scs_d3d11_hook_install_%lu", GetCurrentProcessId());
        h = CreateMutexW(nullptr, FALSE, name);
        if (h) wait = WaitForSingleObject(h, 10000);
        held = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }
    ~InstallLock() {
        if (held) ReleaseMutex(h);
        if (h) CloseHandle(h);
    }
    InstallLock(const InstallLock&) = delete;
    InstallLock& operator=(const InstallLock&) = delete;
};

void Install(const Targets& t) {
    if (g_installed.load()) {
        Emit("hooks already installed earlier in this process: logging again", true);
        g_active.store(true);
        return;
    }
    // Pin first: once a jump to us exists this DLL must never unload.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&Install), &self)) {
        Emit("off: could not pin the DLL", true);
        return;
    }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Emit(std::string("off: MH_Initialize ") + MH_StatusToString(st), true);
        return;
    }
    struct Item {
        const char* name;
        void* target;
        void* detour;
        void** original;
    };
    std::vector<Item> items = {
        {"CreatePixelShader", t.create_ps, reinterpret_cast<void*>(&HookCreatePixelShader),
         reinterpret_cast<void**>(&g_orig_create_ps)},
        {"PSSetShader", t.ps_set, reinterpret_cast<void*>(&HookPSSetShader), reinterpret_cast<void**>(&g_orig_ps_set)},
        {"DrawIndexed", t.draw_indexed, reinterpret_cast<void*>(&HookDrawIndexed),
         reinterpret_cast<void**>(&g_orig_draw_indexed)},
        {"Draw", t.draw, reinterpret_cast<void*>(&HookDraw), reinterpret_cast<void**>(&g_orig_draw)},
        {"DrawIndexedInstanced", t.draw_indexed_inst, reinterpret_cast<void*>(&HookDrawIndexedInstanced),
         reinterpret_cast<void**>(&g_orig_draw_indexed_inst)},
        {"DrawInstanced", t.draw_inst, reinterpret_cast<void*>(&HookDrawInstanced),
         reinterpret_cast<void**>(&g_orig_draw_inst)},
        {"DrawAuto", t.draw_auto, reinterpret_cast<void*>(&HookDrawAuto), reinterpret_cast<void**>(&g_orig_draw_auto)},
        {"DrawIndexedInstancedIndirect", t.draw_indexed_indirect, reinterpret_cast<void*>(&HookDrawIndexedIndirect),
         reinterpret_cast<void**>(&g_orig_draw_indexed_indirect)},
        {"DrawInstancedIndirect", t.draw_inst_indirect, reinterpret_cast<void*>(&HookDrawInstIndirect),
         reinterpret_cast<void**>(&g_orig_draw_inst_indirect)},
        {"CreateTexture2D", t.create_tex, reinterpret_cast<void*>(&HookCreateTexture2D),
         reinterpret_cast<void**>(&g_orig_create_tex)},
        {"CreateShaderResourceView", t.create_srv, reinterpret_cast<void*>(&HookCreateSrv),
         reinterpret_cast<void**>(&g_orig_create_srv)},
        {"PSSetShaderResources", t.ps_set_srv, reinterpret_cast<void*>(&HookPSSetSrv),
         reinterpret_cast<void**>(&g_orig_ps_set_srv)},
        {"UpdateSubresource", t.update, reinterpret_cast<void*>(&HookUpdate), reinterpret_cast<void**>(&g_orig_update)},
        {"UpdateSubresource1", t.update1, reinterpret_cast<void*>(&HookUpdate1), reinterpret_cast<void**>(&g_orig_update1)},
        {"Present", t.present, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_orig_present)},
        {"Map", t.map, reinterpret_cast<void*>(&HookMap), reinterpret_cast<void**>(&g_orig_map)},
        {"VSSetConstantBuffers", t.vs_set_cb, reinterpret_cast<void*>(&HookVSSetCb), reinterpret_cast<void**>(&g_orig_vs_set_cb)},
        {"VSSetConstantBuffers1", t.vs_set_cb1, reinterpret_cast<void*>(&HookVSSetCb1),
         reinterpret_cast<void**>(&g_orig_vs_set_cb1)},
        {"IASetVertexBuffers", t.ia_set_vb, reinterpret_cast<void*>(&HookIASetVb), reinterpret_cast<void**>(&g_orig_ia_set_vb)},
        {"IASetIndexBuffer", t.ia_set_ib, reinterpret_cast<void*>(&HookIASetIb), reinterpret_cast<void**>(&g_orig_ia_set_ib)},
        {"OMSetRenderTargets", t.om_set_rt, reinterpret_cast<void*>(&HookOMSetRt), reinterpret_cast<void**>(&g_orig_om_set_rt)},
        {"OMSetRenderTargetsAndUnorderedAccessViews", t.om_set_rt_uav, reinterpret_cast<void*>(&HookOMSetRtUav),
         reinterpret_cast<void**>(&g_orig_om_set_rt_uav)},
        {"OMSetDepthStencilState", t.om_set_dss, reinterpret_cast<void*>(&HookOMSetDss),
         reinterpret_cast<void**>(&g_orig_om_set_dss)},
        {"RSSetViewports", t.rs_set_vp, reinterpret_cast<void*>(&HookRSSetVp), reinterpret_cast<void**>(&g_orig_rs_set_vp)},
    };
    if (t.present1)
        items.push_back({"Present1", t.present1, reinterpret_cast<void*>(&HookPresent1),
                         reinterpret_cast<void**>(&g_orig_present1)});
    // D6: the "before hook" lines and the patching happen under the install lock, so they show (and copy) what
    // another plugin left in place, never a jump it is half-way through writing.
    const InstallLock lock;
    if (!lock.held) Emit("hook install lock not taken (wait " + Hex(lock.wait) + "): installing anyway");
    for (const Item& it : items) Emit(std::string("before hook ") + it.name + " " + Describe(it.target));
    // D6: MH_CreateHook copies each target's first bytes into our trampoline; a plugin that patches the target
    // before our jump goes in is dropped by it (28/10 18:13, ETS2: the tire plugin finished its install in the
    // middle of ours and its watchdog then saw its Present detour no longer reached). The first bytes are
    // compared right before the jumps go in; when one changed, the hooks are made again from the new bytes.
    constexpr size_t kPrologue = 16;
    std::vector<unsigned char> seen(items.size() * kPrologue);
    for (int attempt = 1;; ++attempt) {
        for (size_t i = 0; i < items.size(); ++i) memcpy(&seen[i * kPrologue], items[i].target, kPrologue);
        // All or nothing: a partial set would leave the probe half blind.
        for (const Item& it : items) {
            st = MH_CreateHook(it.target, it.detour, it.original);
            if (st == MH_OK) st = MH_QueueEnableHook(it.target);
            if (st != MH_OK) {
                Emit(std::string("off: MH_CreateHook ") + it.name + " " + MH_StatusToString(st), true);
                for (const Item& undo : items) MH_RemoveHook(undo.target);
                return;
            }
        }
        const Item* changed = nullptr;
        for (size_t i = 0; i < items.size() && !changed; ++i)
            if (memcmp(&seen[i * kPrologue], items[i].target, kPrologue)) changed = &items[i];
        if (!changed) break;
        for (const Item& undo : items) MH_RemoveHook(undo.target);
        if (attempt == 5) {
            Emit(std::string("off: ") + changed->name + " kept changing while hooking (another plugin hooking it)", true);
            return;
        }
        Emit(std::string("hook ") + changed->name + " changed while hooking (another plugin hooking at the same time): " +
                 "again from its new bytes " + Describe(changed->target),
             true);
        Sleep(100);
    }
    // D6: every detour goes live in one freeze of the game's threads. MH_EnableHook per target froze them once per
    // function: 1.3 s for the 22 of 28/10 18:13, the render thread running frames with only part of the set (draws
    // seen without their state) and a wide window for another plugin's install to cross ours.
    g_active.store(true);
    st = MH_ApplyQueued();
    if (st != MH_OK) {
        g_active.store(false);
        Emit(std::string("off: MH_ApplyQueued ") + MH_StatusToString(st), true);
        return;  // any already enabled only forward (g_active is false)
    }
    g_installed.store(true);
    // D10f: what each jump looks like now, and what it replaced.
    for (size_t i = 0; i < items.size(); ++i) {
        Watched w;
        w.name = items[i].name;
        w.target = static_cast<unsigned char*>(items[i].target);
        w.original = items[i].original;
        w.detour = items[i].detour;
        memcpy(w.ours, w.target, sizeof(w.ours));
        memcpy(w.before, &seen[i * kPrologue], sizeof(w.before));
        if (w.before[0] == 0xE9) {
            int32_t rel;
            memcpy(&rel, w.before + 1, sizeof(rel));
            w.before_jump = w.target + 5 + rel;
        }
        g_watched.push_back(w);
    }
    Emit("hooks installed (" + std::to_string(items.size()) +
             " functions; " + (!tint::On() ? "logging only, the game's drawing is not changed"
                           : g_cfg.tint == 1 ? "tint ON: the player's rig is painted red"
                                             : "dust ON: the player's rig gets its dust") +
             "); DLL pinned; frame capture " +
             (g_cfg.capture_s > 0 ? "every " + std::to_string(g_cfg.capture_s) + " s, up to " +
                                        std::to_string(g_cfg.captures)
                                  : std::string("off")),
         true);
}

void Run() {
    Emit("version " DIRT_VERSION);  // D7 (the game log has it from plugin.cpp)
    Emit("start: catalog " + std::to_string(kCatalogSize) + " SM5X pixel shaders", true);
    Targets t;
    const char* step = "";
    DWORD seh = 0;
    const HRESULT hr = GuardedFind(&t, &step, &seh);
    if (seh) {
        Emit("off: exception " + Hex(seh) + " at step " + step, true);
        return;
    }
    if (FAILED(hr)) {
        Emit(std::string("off: failed at step ") + step + ", hr=" + Hex(static_cast<uint32_t>(hr)), true);
        return;
    }
    Install(t);
    if (g_installed.load()) Watch();  // D10f
}

}  // namespace

void Start(const std::wstring& dir, const Config& c) {
    if (g_thread.joinable()) return;
    g_cfg = c;
    g_dir = dir;
    tint::Configure(c.tint, c.compat_enforce != 0);
    g_cfg.census_s = (std::max)(10, g_cfg.census_s);
    g_cfg.sample_ms = (std::max)(100, g_cfg.sample_ms);
    if (g_by_checksum.empty()) {
        g_is_glass.resize(kCatalogSize);
        for (int i = 0; i < kCatalogSize; ++i) {
            g_by_checksum.emplace(kCatalog[i].checksum, i);
            g_is_glass[i] = strncmp(kCatalog[i].label, "def.glassdrops", 14) == 0;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_file) {
            // One log per session; the previous one is kept as gpu_probe.prev.log. N58 (the sessions of the user's
            // prints, 02-03/11, were gone by the time they were read): the four before it as prev2..prev5.
            const std::wstring log = dir + L"\\gpu_probe.log", prev = dir + L"\\gpu_probe.prev.log";
            for (int i = 5; i >= 2; --i) {
                const std::wstring from = i == 2 ? prev : dir + L"\\gpu_probe.prev" + std::to_wstring(i - 1) + L".log";
                MoveFileExW(from.c_str(), (dir + L"\\gpu_probe.prev" + std::to_wstring(i) + L".log").c_str(),
                            MOVEFILE_REPLACE_EXISTING);
            }
            MoveFileExW(log.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
            g_file = _wfopen(log.c_str(), L"wb");
        }
    }
    if (!premap::Pieces()) {  // P1: before the game draws
        const auto t0 = std::chrono::steady_clock::now();
        int n = premap::LoadDir(dir + L"\\premap");
        if (!c.premap_dir.empty()) n += premap::LoadDir(c.premap_dir, c.premap_prefix);  // P3
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        char b[160];
        snprintf(b, sizeof(b), "premap (P1): %d pieces read (TruckDirt\\premap, plugins\\ets2_truck_dirt_premap) in %.0f ms",
                 n, ms);
        Emit(b);
    }
    if (!g_stop) g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(g_stop);
    g_thread = std::thread(Run);
}

void Stop() {
    if (g_stop) SetEvent(g_stop);  // D10f: the watch ends
    if (g_thread.joinable()) g_thread.join();
    if (g_installed.load()) {
        g_active.store(false);
        if (g_cap_writer.joinable()) g_cap_writer.join();  // the last capture file is complete
        if (tint::On()) {  // N11: the self-check's last lines and its summary
            for (const std::string& l : tint::TakeCompatLines()) Emit(l, true);
            Emit(tint::CompatSummary(), true);
        }
        Emit("shutdown: detours left in place as pass-through (DLL pinned, nothing unhooked)", true);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void SetTelemetry(bool wipers, float speed_kmh, bool paused) {
    g_wipers.store(wipers, std::memory_order_relaxed);
    g_speed_kmh.store(speed_kmh, std::memory_order_relaxed);
    g_paused.store(paused, std::memory_order_relaxed);
}

void SetPose(const Pose& pose) {
    g_truck_known.store(pose.truck_wheels.count > 0, std::memory_order_relaxed);
    g_trailer_coupled.store(pose.trailer_connected && pose.trailer_wheels.count > 0, std::memory_order_relaxed);
    // N12: the wheels' turns for the dirt pass (the dirt of a wheel stays on it).
    tint::SetWheels(0, pose.truck_wheels.steering, pose.truck_wheels.rotation, pose.truck_wheels.count, pose.speed_kmh);
    tint::SetWheels(1, pose.trailer_wheels.steering, pose.trailer_wheels.rotation, pose.trailer_wheels.count, pose.speed_kmh);
    for (int i = 0; i < kPoseMoreTrailers && 2 + i < rig::kUnits; ++i)  // N21: trailer.1-4
        tint::SetWheels(2 + i, pose.more[i].wheels.steering, pose.more[i].wheels.rotation, pose.more[i].wheels.count,
                        pose.speed_kmh);
    std::lock_guard<std::mutex> lock(g_pose_mutex);
    g_pose = pose;
}

bool RainActive() {
    // A stopped probe (exception) or no update for 2 s: unknown, so no rain
    // from here (the wipers remain the fallback in plugin.cpp).
    if (g_failed.load(std::memory_order_relaxed)) return false;
    const uint64_t last = g_rain_last_ms.load(std::memory_order_relaxed);
    if (!last || GetTickCount64() > last + 2000) return false;
    return g_rain_last_second.load(std::memory_order_relaxed);
}

// N21: every trailer of a road train takes trailer.0's dirt (the rig is dirtied as one: the user's call, 28/10).
void SetDust(float truck, float trailer) {
    tint::SetDust(0, truck);
    for (int u = 1; u < rig::kUnits; ++u) tint::SetDust(u, trailer);
}

void SetLayers(const dust::Layers& truck, const dust::Layers& trailer) {
    tint::SetLayers(0, truck);
    for (int u = 1; u < rig::kUnits; ++u) tint::SetLayers(u, trailer);
}

void SetCargo(float level, const dust::Layers& cargo) {
    for (int u = 1; u < rig::kUnits; ++u) tint::SetCargo(u, level, cargo);
}

void SetPanel(const panel::View& view) {
    std::lock_guard<std::mutex> lock(g_panel_mutex);
    g_panel_view = view;
    g_panel_shown.store(view.open || view.badge, std::memory_order_relaxed);
}

void SetOverlay(const overlay::View& view) {
    std::lock_guard<std::mutex> lock(g_overlay_mutex);
    g_overlay_view = view;
    g_overlay_shown.store(view.Any(), std::memory_order_relaxed);
}

bool OverlayFailed() { return g_overlay_failed.load(std::memory_order_relaxed); }

std::vector<std::string> TakeMessages() {
    static bool failure_reported = false;
    if (g_failed.load() && !failure_reported) {
        failure_reported = true;
        Emit("off: exception inside a draw hook; the probe stopped (the game keeps drawing normally)", true);
    }
    static bool overlay_reported = false;
    if (g_overlay_failed.load() && !overlay_reported) {
        overlay_reported = true;
        Emit("overlay (wash, menu): drawing off after an error (the game keeps drawing normally)", true);
    }
    static bool panel_reported = false;
    if (g_panel_failed.load() && !panel_reported) {
        panel_reported = true;
        Emit("test panel: drawing off after an error (the game keeps drawing normally)", true);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<std::string> out;
    out.swap(g_messages);
    return out;
}

}  // namespace gpu_probe
