// D5: the player's overlay (src/overlay: the wash at the station, the keys
// menu) drawn with the DLL's own renderer (panel_draw: GDI glyphs of the
// system font in a texture) over a test picture, into PNGs, to look at the
// texts and the screens without the game. Checks that the drawing gives the
// "game's" state back, as tools/dustpreview does for the test panel.
// Usage: overlaypreview <out dir> <lang dir>   (OVERLAYPREVIEW_WARP=1: software rasterizer)

#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lang.h"
#include "menu.h"
#include "overlay.h"
#include "panel_draw.h"

namespace {

constexpr UINT kW = 2560, kH = 1440;  // D11i: the user's screen height (3440x1440), the font as in the game

bool SavePng(const std::wstring& path, const void* rgba, UINT pitch) {
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

std::string ReadFile(const std::string& path) {
    std::string out;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char b[4096];
        for (size_t n; (n = fread(b, 1, sizeof(b), f)) > 0;) out.append(b, n);
        fclose(f);
    }
    return out;
}

lang::Catalog Load(const std::string& dir, const std::string& code) {
    lang::Table t, en;
    lang::Parse(ReadFile(dir + "/" + code + ".txt"), &t, nullptr);
    lang::Parse(ReadFile(dir + "/en_gb.txt"), &en, nullptr);
    return lang::Catalog(t, en);
}

// A picture standing in for the game: a sky-to-road gradient with stripes.
std::vector<uint32_t> Picture() {
    std::vector<uint32_t> px(size_t(kW) * kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            const float t = float(y) / kH;
            uint32_t r = uint32_t(120 + 80 * (1 - t)), g = uint32_t(150 + 60 * (1 - t)), b = uint32_t(200 - 90 * t);
            if (((x / 80) + (y / 80)) % 2) r = r * 8 / 10, g = g * 8 / 10, b = b * 8 / 10;
            px[size_t(y) * kW + x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    return px;
}

int g_failures = 0;

void Fail(const std::string& what) {
    printf("FAIL %s\n", what.c_str());
    ++g_failures;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: overlaypreview <out dir> <lang dir>\n");
        return 2;
    }
    const std::string out = argv[1], langs = argv[2];
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const bool warp = getenv("OVERLAYPREVIEW_WARP") != nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr) && !warp)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        printf("FAIL D3D11CreateDevice\n");
        return 1;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kW, td.Height = kH, td.MipLevels = td.ArraySize = 1, td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1, td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateTexture2D(&td, nullptr, &tex);
    dev->CreateRenderTargetView(tex, nullptr, &rtv);
    td.BindFlags = 0, td.Usage = D3D11_USAGE_STAGING, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    dev->CreateTexture2D(&td, nullptr, &staging);
    const std::vector<uint32_t> picture = Picture();
    panel_draw::Renderer renderer;

    auto draw = [&](const char* name, const overlay::View& v) {
        ctx->UpdateSubresource(tex, 0, nullptr, picture.data(), kW * 4, 0);
        // A "game" state the drawing must give back.
        const D3D11_VIEWPORT game_vp = {3, 5, 300, 200, 0, 1};
        ctx->RSSetViewports(1, &game_vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        if (!renderer.DrawOverlay(ctx, rtv, int(kW), int(kH), v)) Fail(std::string(name) + ": " + renderer.Error());
        D3D11_VIEWPORT vp = {};
        UINT nvp = 1;
        ctx->RSGetViewports(&nvp, &vp);
        D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ctx->IAGetPrimitiveTopology(&topo);
        if (nvp != 1 || vp.TopLeftX != 3 || vp.Width != 300 || topo != D3D11_PRIMITIVE_TOPOLOGY_LINELIST)
            Fail(std::string(name) + ": the game's state not given back");
        ctx->CopyResource(staging, tex);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            Fail(std::string(name) + ": Map");
            return;
        }
        const std::string path = out + "/" + name + ".png";
        const std::wstring wpath(path.begin(), path.end());
        const bool ok = SavePng(wpath, m.pData, m.RowPitch);
        ctx->Unmap(staging, 0);
        if (!ok) Fail(path);
        else printf("ok   %s\n", path.c_str());
    };

    const lang::Catalog en = Load(langs, "en_gb");
    {
        overlay::View v;
        v.wash.key = "F8";
        v.wash.message = en.Get("wash.prompt");
        v.wash.detail = en.Price(180, "\xE2\x82\xAC");
        draw("wash_hint", v);
    }
    {
        // D11c: the prompt in pt_br (ATS, US$), then the key without the parking brake.
        const lang::Catalog pt = Load(langs, "pt_br");
        overlay::View v;
        v.wash.key = "F8";
        v.wash.message = pt.Get("wash.prompt");
        v.wash.detail = pt.Price(120, "US$");
        draw("wash_hint_pt_br", v);
        overlay::View b;
        b.wash.message = pt.Get("wash.parking_brake");
        draw("wash_parking_brake_pt_br", b);
    }
    {
        overlay::View v;
        v.wash.darkness = 0.5f;
        draw("wash_darken", v);
    }
    {
        overlay::View v;
        v.wash.darkness = 1.f;
        v.wash.washing = true;
        v.wash.show = 0.4f;
        v.wash.washing_text = en.Get("wash.washing");
        v.wash.washing_detail = en.Price(180, "\xE2\x82\xAC");
        draw("wash_show", v);
        // D11f: the visit in pt_br (ATS), early (the drops) and late in the bar.
        const lang::Catalog pt = Load(langs, "pt_br");
        v.wash.washing_text = pt.Get("wash.washing");
        v.wash.washing_detail = pt.Price(120, "US$");
        v.wash.show = 0.15f;
        draw("wash_show_pt_br_early", v);
        v.wash.show = 0.8f;
        draw("wash_show_pt_br_late", v);
        v.wash.darkness = 0.6f;
        draw("wash_show_pt_br_lighten", v);
    }
    {
        overlay::View v;
        v.wash.message = en.Get("wash.washed");
        draw("wash_done", v);
    }
    {
        // V6: the keys menu in pt_br, the panel's row waiting for its key.
        menu::Menu m;
        m.Set(0, "");
        m.Set(1, "");
        m.Toggle();
        std::vector<menu::Write> w;
        m.Key(0x26, true, false, false, false, &w);  // ctrl+up: the panel
        overlay::View v;
        v.menu = m.MakeView(Load(langs, "pt_br"), false);
        draw("menu_pt_br", v);
        m.Key(0x0D, true, false, false, false, &w);  // ctrl+enter
        v.menu = m.MakeView(Load(langs, "pt_br"), true);
        draw("menu_pt_br_capture", v);
    }
    // N9: every language's hint in its own letters, two pictures, and the languages the plugin would
    // show in English on this PC (the font rule and the font's glyphs, as LoadTexts in plugin.cpp).
    std::vector<std::string> english;
    overlay::View pages[2];
    for (int i = 0; i < lang::kCodeCount; ++i) {
        const std::string code = lang::kCodes[i];
        lang::Table t;
        lang::Parse(ReadFile(langs + "/" + code + ".txt"), &t, nullptr);
        std::string why;
        if (!lang::FontCanShow(t)) why = t.Get("_script") + " script";
        else if (!panel_draw::MissingGlyphs(lang::Glyphs(t)).empty()) why = "letters not in the font";
        if (!why.empty()) english.push_back(code + " (" + why + ")");
        const lang::Catalog c = Load(langs, code);
        overlay::Menu& m = pages[i < lang::kCodeCount / 2 ? 0 : 1].menu;
        m.open = true;
        m.title = "wash.hint";
        m.rows.push_back({code + (why.empty() ? "" : " *"),
                          c.Format("wash.hint", {{"key", "F8"}, {"price", c.Price(1180, "\xE2\x82\xAC")}})});
    }
    for (overlay::View& v : pages) v.menu.help = {"* English in the game: the font has not its letters"};
    draw("lang_1", pages[0]);
    draw("lang_2", pages[1]);
    printf("     English on this PC (%zu):", english.size());
    for (const std::string& e : english) printf(" %s", e.c_str());
    printf("\n");
    renderer.Release();
    for (IUnknown* u : {static_cast<IUnknown*>(staging), static_cast<IUnknown*>(rtv), static_cast<IUnknown*>(tex),
                        static_cast<IUnknown*>(ctx), static_cast<IUnknown*>(dev)})
        if (u) u->Release();
    printf("%s\n", g_failures ? "FAILED" : "all drawn");
    return g_failures ? 1 : 0;
}
