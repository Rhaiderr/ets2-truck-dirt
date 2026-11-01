// N10: draws the test panel (the quads of panel::Layout) with D3D11 over a
// render target: the game's back buffer at Present (gpu_probe) or the
// preview's image (tools/dustpreview).
// D5: also the player's overlay (overlay::Layout: the wash at the station and
// the keys menu), with the glyphs of a system font (GlyphFont: GDI renders
// each glyph once into a texture) instead of the panel's 8x8 font.
//
// - One draw for every quad: the vertex shader reads them from a structured
//   buffer (6 vertices each, no input layout), the pixel shader (one output)
//   takes the colour times the font texel (Load, no sampler).
// - Blends by alpha over the target; no depth, no stencil, no culling.
// - Saves every state it touches and puts it back (targets, blend, depth-
//   stencil, rasterizer, viewports, topology, input layout, the five shaders,
//   VS and PS resource slot 0).
#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <string>
#include <vector>

#include "overlay.h"
#include "test_panel.h"

namespace panel_draw {

// The face of the overlay's text: a font of every Windows 10/11 with the
// Latin, Greek and Cyrillic alphabets [verified: Segoe UI, Arial].
constexpr const wchar_t* kFace = L"Segoe UI";
constexpr const wchar_t* kFallbackFace = L"Arial";

// The glyphs of kFace at one pixel size, rendered by GDI on first use into an
// R8 texture (1024 x 1024). A full texture starts over on the next frame.
class GlyphFont : public overlay::Font {
public:
    ~GlyphFont() override { Release(); }
    // Before a layout, on the render thread: the device, the context the
    // glyphs are uploaded with and the pixel size. False on an error.
    bool Begin(ID3D11Device* dev, ID3D11DeviceContext* c, int px);
    void End() { ctx_ = nullptr; }
    int LineHeight() const override { return line_; }
    bool Get(char32_t c, overlay::Glyph* out) override;
    void Solid(float* u, float* v) override;
    ID3D11ShaderResourceView* Texture() const { return srv_; }
    void Release();
    const std::string& Error() const { return error_; }

private:
    bool Fail(const std::string& what);
    void Reset();

    ID3D11Device* dev_ = nullptr;          // not referenced
    ID3D11DeviceContext* ctx_ = nullptr;   // between Begin and End only
    ID3D11Texture2D* tex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    void* dc_ = nullptr;    // HDC
    void* font_ = nullptr;  // HFONT
    void* old_ = nullptr;   // the DC's previous font
    int px_ = 0, line_ = 0, ascent_ = 0;
    int x_ = 0, y_ = 0, row_ = 0;  // the shelf being filled
    bool full_ = false;
    std::vector<std::pair<char32_t, overlay::Glyph>> glyphs_;  // sorted by code point
    std::vector<char32_t> missing_;
    std::string error_;
};

// The code points of `chars` that kFace (or kFallbackFace) has no glyph for (any thread; GDI only).
std::vector<char32_t> MissingGlyphs(const std::vector<char32_t>& chars);

class Renderer {
public:
    ~Renderer() { Release(); }
    // Draws on `rtv` (`width` x `height` pixels). False on an error (Error() says which; it stays off then).
    // `tex`: the texture of the quads' atlas coordinates (null: the 8x8 font).
    bool Draw(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, int width, int height,
              const std::vector<panel::Quad>& quads, ID3D11ShaderResourceView* tex = nullptr);
    // Draws on the back buffer of `sc` (created for this frame and released: a reference held
    // across frames would make the game's ResizeBuffers fail).
    bool DrawOnSwapChain(ID3D11DeviceContext* c, IDXGISwapChain* sc, const panel::View& view);
    // D5: the overlay, on a target or on the back buffer of `sc` (as above).
    bool DrawOverlay(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, int width, int height,
                     const overlay::View& view);
    bool DrawOverlayOnSwapChain(ID3D11DeviceContext* c, IDXGISwapChain* sc, const overlay::View& view);
    void Release();
    const std::string& Error() const { return error_; }

private:
    bool Init(ID3D11Device* dev);
    bool Fail(const std::string& what);
    // A view of the back buffer of `sc` for this frame (null: nothing to draw on, or an error: see failed_).
    ID3D11RenderTargetView* BackBuffer(ID3D11DeviceContext* c, IDXGISwapChain* sc, int* width, int* height);

    ID3D11Device* dev_ = nullptr;  // not referenced: the device the objects belong to
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11ShaderResourceView* font_ = nullptr;
    ID3D11Buffer* quads_ = nullptr;
    ID3D11ShaderResourceView* quads_srv_ = nullptr;
    UINT capacity_ = 0;
    ID3D11BlendState* blend_ = nullptr;
    ID3D11DepthStencilState* depth_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    bool failed_ = false;
    std::string error_;
    GlyphFont glyphs_;
};

}  // namespace panel_draw
