// N10: the test panel's D3D11 drawing (see panel_draw.h).

#include "panel_draw.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace panel_draw {
namespace {

// One quad on the GPU: clip-space corners, atlas corners, colour (0xAABBGGRR).
struct GpuQuad {
    float pos[4];
    float uv[4];
    UINT rgba;
    UINT pad[3];
};
static_assert(sizeof(GpuQuad) == 48, "structured buffer stride");

const char kVs[] = R"(
struct Q { float4 pos; float4 uv; uint rgba; uint3 pad; };
StructuredBuffer<Q> g_quads : register(t0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; };
V main(uint id : SV_VertexID) {
    const uint corner[6] = {0, 1, 2, 2, 1, 3};  // two triangles: top-left, top-right, bottom-left, bottom-right
    const Q q = g_quads[id / 6];
    const uint k = corner[id % 6];
    const float2 t = float2(k & 1, k >> 1);
    V v;
    v.pos = float4(lerp(q.pos.xy, q.pos.zw, t), 0, 1);
    v.uv = lerp(q.uv.xy, q.uv.zw, t);
    v.color = float4(q.rgba & 255, (q.rgba >> 8) & 255, (q.rgba >> 16) & 255, q.rgba >> 24) / 255.0;
    return v;
}
)";

// One render target (SnowyMoon stopped the game on a two-target pixel shader: see tint.cpp).
// The atlas is the 8x8 font (panel::kAtlasW x kAtlasH) or the overlay's glyphs (GlyphFont).
const char kPs[] = R"(
Texture2D<float> g_font : register(t0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; };
float4 main(V v) : SV_Target0 {
    uint w, h;
    g_font.GetDimensions(w, h);
    const float2 size = float2(w, h);
    const float a = g_font.Load(int3(min(v.uv * size, size - 1), 0));
    return float4(v.color.rgb, v.color.a * a);
}
)";

bool Compile(const char* src, const char* target, ID3DBlob** out, std::string* error) {
    // d3dcompiler_47.dll is part of Windows 10/11 (System32), as in tint.cpp.
    static pD3DCompile compile = nullptr;
    if (!compile) {
        wchar_t path[MAX_PATH] = {};
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\d3dcompiler_47.dll");
        HMODULE m = LoadLibraryW(path);
        if (m) compile = reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile"));
        if (!compile) {
            *error = "d3dcompiler_47.dll not found";
            return false;
        }
    }
    ID3DBlob* err = nullptr;
    const HRESULT hr = compile(src, strlen(src), "panel", nullptr, nullptr, "main", target,
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) *error = err ? static_cast<const char*>(err->GetBufferPointer()) : "compile error";
    if (err) err->Release();
    return SUCCEEDED(hr);
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// A typeless back buffer needs a typed view.
DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}

}  // namespace

bool Renderer::Fail(const std::string& what) {
    failed_ = true;
    if (error_.empty()) error_ = "panel drawing off: " + what;
    return false;
}

void Renderer::Release() {
    glyphs_.Release();
    SafeRelease(vs_);
    SafeRelease(ps_);
    SafeRelease(font_);
    SafeRelease(quads_srv_);
    SafeRelease(quads_);
    SafeRelease(blend_);
    SafeRelease(depth_);
    SafeRelease(raster_);
    capacity_ = 0;
    dev_ = nullptr;
}

bool Renderer::Init(ID3D11Device* dev) {
    if (dev_ == dev && vs_) return true;
    Release();
    std::string error;
    ID3DBlob* b = nullptr;
    if (!Compile(kVs, "vs_5_0", &b, &error)) return Fail("vertex shader: " + error);
    HRESULT hr = dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs_);
    b->Release();
    if (FAILED(hr)) return Fail("CreateVertexShader");
    if (!Compile(kPs, "ps_5_0", &b, &error)) return Fail("pixel shader: " + error);
    hr = dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps_);
    b->Release();
    if (FAILED(hr)) return Fail("CreatePixelShader");

    static uint8_t atlas[panel::kAtlasW * panel::kAtlasH];
    panel::FontAtlas(atlas);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = panel::kAtlasW, td.Height = panel::kAtlasH, td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM, td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init = {atlas, panel::kAtlasW, 0};
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &init, &tex))) return Fail("font texture");
    hr = dev->CreateShaderResourceView(tex, nullptr, &font_);
    tex->Release();
    if (FAILED(hr)) return Fail("font view");

    D3D11_BLEND_DESC bd = {};
    auto& t = bd.RenderTarget[0];
    t.BlendEnable = TRUE, t.SrcBlend = D3D11_BLEND_SRC_ALPHA, t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    t.BlendOp = D3D11_BLEND_OP_ADD, t.SrcBlendAlpha = D3D11_BLEND_ZERO, t.DestBlendAlpha = D3D11_BLEND_ONE;
    t.BlendOpAlpha = D3D11_BLEND_OP_ADD, t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
                                                                   D3D11_COLOR_WRITE_ENABLE_GREEN |
                                                                   D3D11_COLOR_WRITE_ENABLE_BLUE;
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE, dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO, dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = FALSE;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateBlendState(&bd, &blend_)) || FAILED(dev->CreateDepthStencilState(&dd, &depth_)) ||
        FAILED(dev->CreateRasterizerState(&rd, &raster_)))
        return Fail("states");
    dev_ = dev;
    return true;
}

bool Renderer::Draw(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, int width, int height,
                    const std::vector<panel::Quad>& quads, ID3D11ShaderResourceView* tex) {
    if (failed_ || quads.empty() || width <= 0 || height <= 0) return !failed_;
    ID3D11Device* dev = nullptr;
    c->GetDevice(&dev);
    const bool ok = dev && Init(dev);
    if (dev) dev->Release();
    if (!ok) return false;

    // The quads, in clip space.
    const UINT n = static_cast<UINT>(quads.size());
    if (n > capacity_) {
        SafeRelease(quads_srv_);
        SafeRelease(quads_);
        capacity_ = 0;
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = static_cast<UINT>(sizeof(GpuQuad)) * (n + 256);
        d.Usage = D3D11_USAGE_DYNAMIC, d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE, d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        d.StructureByteStride = sizeof(GpuQuad);
        D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = DXGI_FORMAT_UNKNOWN, v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        v.Buffer.FirstElement = 0, v.Buffer.NumElements = n + 256;
        if (FAILED(dev_->CreateBuffer(&d, nullptr, &quads_)) ||
            FAILED(dev_->CreateShaderResourceView(quads_, &v, &quads_srv_)))
            return Fail("quad buffer");
        capacity_ = n + 256;
    }
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(c->Map(quads_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return Fail("Map");
    GpuQuad* out = static_cast<GpuQuad*>(m.pData);
    const float sx = 2.f / width, sy = 2.f / height;
    for (UINT i = 0; i < n; ++i) {
        const panel::Quad& q = quads[i];
        out[i] = {{q.x0 * sx - 1.f, 1.f - q.y0 * sy, q.x1 * sx - 1.f, 1.f - q.y1 * sy}, {q.u0, q.v0, q.u1, q.v1}, q.rgba, {}};
    }
    c->Unmap(quads_, 0);

    // The game's state.
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    ID3D11BlendState* bs = nullptr;
    float factor[4] = {};
    UINT sample_mask = 0;
    c->OMGetBlendState(&bs, factor, &sample_mask);
    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    c->OMGetDepthStencilState(&dss, &ref);
    ID3D11RasterizerState* rs = nullptr;
    c->RSGetState(&rs);
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetViewports(&nvp, vps);
    ID3D11InputLayout* il = nullptr;
    c->IAGetInputLayout(&il);
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    c->IAGetPrimitiveTopology(&topo);
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    c->GSGetShader(&gs, nullptr, nullptr);
    c->HSGetShader(&hs, nullptr, nullptr);
    c->DSGetShader(&ds, nullptr, nullptr);
    ID3D11ShaderResourceView* vs_srv0 = nullptr;
    ID3D11ShaderResourceView* ps_srv0 = nullptr;
    c->VSGetShaderResources(0, 1, &vs_srv0);
    c->PSGetShaderResources(0, 1, &ps_srv0);

    // The panel.
    const D3D11_VIEWPORT vp = {0.f, 0.f, float(width), float(height), 0.f, 1.f};
    c->OMSetRenderTargets(1, &rtv, nullptr);
    c->OMSetBlendState(blend_, nullptr, 0xFFFFFFFF);
    c->OMSetDepthStencilState(depth_, 0);
    c->RSSetState(raster_);
    c->RSSetViewports(1, &vp);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs_, nullptr, 0);
    c->PSSetShader(ps_, nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    c->VSSetShaderResources(0, 1, &quads_srv_);
    c->PSSetShaderResources(0, 1, tex ? &tex : &font_);
    c->Draw(n * 6, 0);

    // Back to the game's state.
    c->VSSetShaderResources(0, 1, &vs_srv0);
    c->PSSetShaderResources(0, 1, &ps_srv0);
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    c->OMSetBlendState(bs, factor, sample_mask);
    c->OMSetDepthStencilState(dss, ref);
    c->RSSetState(rs);
    if (nvp) c->RSSetViewports(nvp, vps);
    c->IASetInputLayout(il);
    c->IASetPrimitiveTopology(topo);
    c->VSSetShader(vs, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->GSSetShader(gs, nullptr, 0);
    c->HSSetShader(hs, nullptr, 0);
    c->DSSetShader(ds, nullptr, 0);
    for (auto* v : rtvs)
        if (v) v->Release();
    for (IUnknown* u : {static_cast<IUnknown*>(dsv), static_cast<IUnknown*>(bs), static_cast<IUnknown*>(dss),
                        static_cast<IUnknown*>(rs), static_cast<IUnknown*>(il), static_cast<IUnknown*>(vs),
                        static_cast<IUnknown*>(ps), static_cast<IUnknown*>(gs), static_cast<IUnknown*>(hs),
                        static_cast<IUnknown*>(ds), static_cast<IUnknown*>(vs_srv0), static_cast<IUnknown*>(ps_srv0)})
        if (u) u->Release();
    return true;
}

ID3D11RenderTargetView* Renderer::BackBuffer(ID3D11DeviceContext* c, IDXGISwapChain* sc, int* width, int* height) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb)
        return nullptr;  // not a D3D11 swap chain (or none now): nothing to draw on
    D3D11_TEXTURE2D_DESC td = {};
    bb->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = ViewFormat(td.Format);
    rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11Device* dev = nullptr;
    c->GetDevice(&dev);
    ID3D11RenderTargetView* rtv = nullptr;
    const HRESULT hr = dev ? dev->CreateRenderTargetView(bb, &rd, &rtv) : E_FAIL;
    if (dev) dev->Release();
    bb->Release();
    if (FAILED(hr)) {
        Fail("back buffer view (format " + std::to_string(int(td.Format)) + ")");
        return nullptr;
    }
    *width = static_cast<int>(td.Width), *height = static_cast<int>(td.Height);
    return rtv;
}

bool Renderer::DrawOnSwapChain(ID3D11DeviceContext* c, IDXGISwapChain* sc, const panel::View& view) {
    if (failed_ || (!view.open && !view.badge)) return !failed_;
    int w = 0, h = 0;
    ID3D11RenderTargetView* rtv = BackBuffer(c, sc, &w, &h);
    if (!rtv) return !failed_;
    const bool ok = Draw(c, rtv, w, h, panel::Layout(view, w, h));
    rtv->Release();
    return ok;
}

bool Renderer::DrawOverlay(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, int width, int height,
                           const overlay::View& view) {
    if (failed_ || !view.Any() || width <= 0 || height <= 0) return !failed_;
    ID3D11Device* dev = nullptr;
    c->GetDevice(&dev);
    const bool ready = dev && Init(dev);
    const bool font = ready && glyphs_.Begin(dev, c, overlay::FontPx(height));
    if (dev) dev->Release();
    if (!ready) return false;
    if (!font) return Fail(glyphs_.Error());
    const std::vector<panel::Quad> quads = overlay::Layout(view, glyphs_, width, height);
    glyphs_.End();
    return Draw(c, rtv, width, height, quads, glyphs_.Texture());
}

bool Renderer::DrawOverlayOnSwapChain(ID3D11DeviceContext* c, IDXGISwapChain* sc, const overlay::View& view) {
    if (failed_ || !view.Any()) return !failed_;
    int w = 0, h = 0;
    ID3D11RenderTargetView* rtv = BackBuffer(c, sc, &w, &h);
    if (!rtv) return !failed_;
    const bool ok = DrawOverlay(c, rtv, w, h, view);
    rtv->Release();
    return ok;
}

// ---- GlyphFont ----

namespace {

constexpr int kAtlas = 1024;  // the glyph texture, R8, kAtlas x kAtlas
constexpr int kSolid = 4;     // a full block at its top-left, for the plain quads

HFONT MakeFont(HDC dc, int px) {
    for (const wchar_t* face : {kFace, kFallbackFace}) {
        HFONT f = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        if (!f) continue;
        // GDI gives some other face when this one is missing: then the next.
        HGDIOBJ old = SelectObject(dc, f);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, old);
        if (_wcsicmp(got, face) == 0) return f;
        DeleteObject(f);
    }
    return nullptr;
}

// No glyph in the selected font (GDI would draw its "missing" box).
bool Missing(HDC dc, char32_t c) {
    if (c > 0xFFFF) return true;  // outside the BMP: not drawn (no language here needs one)
    const wchar_t w = static_cast<wchar_t>(c);
    WORD index = 0;
    return GetGlyphIndicesW(dc, &w, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR || index == 0xFFFF;
}

}  // namespace

std::vector<char32_t> MissingGlyphs(const std::vector<char32_t>& chars) {
    std::vector<char32_t> out;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return chars;
    HFONT f = MakeFont(dc, 32);
    if (!f) {
        DeleteDC(dc);
        return chars;
    }
    HGDIOBJ old = SelectObject(dc, f);
    for (const char32_t c : chars)
        if (c >= 0x20 && Missing(dc, c) && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
    return out;
}

bool GlyphFont::Fail(const std::string& what) {
    if (error_.empty()) error_ = "overlay text: " + what;
    return false;
}

void GlyphFont::Release() {
    SafeRelease(srv_);
    SafeRelease(tex_);
    if (dc_) {
        HDC dc = static_cast<HDC>(dc_);
        if (old_) SelectObject(dc, static_cast<HGDIOBJ>(old_));
        DeleteDC(dc);
    }
    if (font_) DeleteObject(static_cast<HGDIOBJ>(font_));
    dc_ = font_ = old_ = nullptr;
    dev_ = nullptr;
    ctx_ = nullptr;
    px_ = line_ = ascent_ = 0;
    glyphs_.clear();
    missing_.clear();
}

// An empty texture but the solid block; every glyph rendered again on use.
void GlyphFont::Reset() {
    glyphs_.clear();
    missing_.clear();
    x_ = kSolid + 1, y_ = 0, row_ = kSolid;
    full_ = false;
}

bool GlyphFont::Begin(ID3D11Device* dev, ID3D11DeviceContext* c, int px) {
    if (!error_.empty()) return false;
    if (dev != dev_) {
        Release();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = kAtlas, td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM, td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<uint8_t> zero(size_t(kAtlas) * kAtlas, 0);
        for (int y = 0; y < kSolid; ++y)
            for (int x = 0; x < kSolid; ++x) zero[size_t(y) * kAtlas + x] = 255;
        const D3D11_SUBRESOURCE_DATA init = {zero.data(), kAtlas, 0};
        if (FAILED(dev->CreateTexture2D(&td, &init, &tex_)) || FAILED(dev->CreateShaderResourceView(tex_, nullptr, &srv_)))
            return Fail("glyph texture");
        dev_ = dev;
        Reset();
    }
    if (px != px_ || full_) {
        if (!dc_) {
            HDC dc = CreateCompatibleDC(nullptr);
            if (!dc) return Fail("CreateCompatibleDC");
            dc_ = dc;
        }
        HDC dc = static_cast<HDC>(dc_);
        if (px != px_) {
            HFONT f = MakeFont(dc, px);
            if (!f) return Fail("no Segoe UI or Arial font");
            HGDIOBJ prev = SelectObject(dc, f);
            if (!old_) old_ = prev;
            if (font_) DeleteObject(static_cast<HGDIOBJ>(font_));
            font_ = f;
            TEXTMETRICW tm = {};
            GetTextMetricsW(dc, &tm);
            line_ = tm.tmHeight, ascent_ = tm.tmAscent;
            px_ = px;
        }
        Reset();
    }
    ctx_ = c;
    return true;
}

void GlyphFont::Solid(float* u, float* v) { *u = *v = (kSolid / 2.f) / kAtlas; }

bool GlyphFont::Get(char32_t c, overlay::Glyph* out) {
    const auto it = std::lower_bound(glyphs_.begin(), glyphs_.end(), c,
                                     [](const std::pair<char32_t, overlay::Glyph>& g, char32_t k) { return g.first < k; });
    if (it != glyphs_.end() && it->first == c) {
        *out = it->second;
        return true;
    }
    if (!dc_ || !ctx_ || full_ || std::find(missing_.begin(), missing_.end(), c) != missing_.end()) return false;
    HDC dc = static_cast<HDC>(dc_);
    if (Missing(dc, c)) {
        missing_.push_back(c);
        return false;
    }
    GLYPHMETRICS gm = {};
    const MAT2 identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    const UINT ch = static_cast<UINT>(c);
    const DWORD size = GetGlyphOutlineW(dc, ch, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &identity);
    if (size == GDI_ERROR) {
        missing_.push_back(c);
        return false;
    }
    overlay::Glyph g;
    g.advance = gm.gmCellIncX;
    if (size > 0) {
        const int w = static_cast<int>(gm.gmBlackBoxX), h = static_cast<int>(gm.gmBlackBoxY);
        if (w + 1 > kAtlas || h + 1 > kAtlas) {
            missing_.push_back(c);
            return false;
        }
        if (x_ + w + 1 > kAtlas) x_ = 0, y_ += row_ + 1, row_ = 0;  // the next shelf
        if (y_ + h + 1 > kAtlas) {
            full_ = true;  // started over on the next frame (Begin)
            return false;
        }
        std::vector<uint8_t> bits(size);
        if (GetGlyphOutlineW(dc, ch, GGO_GRAY8_BITMAP, &gm, size, bits.data(), &identity) == GDI_ERROR) {
            missing_.push_back(c);
            return false;
        }
        // 65 levels (0..64), rows padded to 4 bytes: to 0..255.
        const int pitch = (w + 3) & ~3;
        std::vector<uint8_t> px(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                px[size_t(y) * w + x] = static_cast<uint8_t>((std::min)(255, bits[size_t(y) * pitch + x] * 255 / 64));
        const D3D11_BOX box = {UINT(x_), UINT(y_), 0, UINT(x_ + w), UINT(y_ + h), 1};
        ctx_->UpdateSubresource(tex_, 0, &box, px.data(), UINT(w), 0);
        g.w = w, g.h = h;
        g.x = gm.gmptGlyphOrigin.x;
        g.y = ascent_ - gm.gmptGlyphOrigin.y;
        g.u0 = float(x_) / kAtlas, g.v0 = float(y_) / kAtlas;
        g.u1 = float(x_ + w) / kAtlas, g.v1 = float(y_ + h) / kAtlas;
        x_ += w + 1;
        row_ = (std::max)(row_, h);
    }
    glyphs_.insert(it, {c, g});
    *out = g;
    return true;
}

}  // namespace panel_draw
