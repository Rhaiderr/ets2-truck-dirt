// V3 part 2 + D3: draws on the player's truck and trailer in the G-buffer:
// red (mode 1, the V3 gate test) or their dust (mode 2, D3).
//
// - The game state the decision needs is tracked from the detours of the
//   state calls: render targets, viewport, depth-stencil state, VS constant
//   buffer 0 (with its range), index and vertex buffer, and the CPU pointer
//   of every dynamic constant buffer the game maps.
// - A G-buffer pass = 4+ render targets R16G16B16A16 float x3 + uint
//   [verified: V3a, 4 without SnowyMoon, 7 with it] with a stencil depth buffer.
// - At each DrawIndexed of a G-buffer pass the draw's model->camera matrix is
//   read from VS cb0 rows 0-2 through the mapped pointer (as the tire plugin,
//   SEH-guarded) and handed to rig::Classify; rows 4-7 (model->clip) give the
//   view's projection [verified: V3a captures, row 7 = -row 2]. While a
//   G-buffer pass is open every depth-stencil state the game sets is replaced
//   by a copy that also writes stencil bits 0x30 (the game never writes them:
//   its G-buffer writes 0x0f [verified: V3a]): truck draws write 0x10, trailer
//   draws 0x20, every other draw clears both, so what covers a player pixel
//   later (grass, a pole) loses the mark.
// - When the pass ends (the game binds other targets, or Present), one
//   full-screen triangle per unit and target blends into the albedo (rt2),
//   then the specular (rt1) target where the unit's bit is set; the last one
//   clears the bit (stencil op ZERO, mask 0x30), so every later pass of the
//   game sees its own stencil values. One render target per pixel shader:
//   SnowyMoon stopped the game on a two-target one (see tint.cpp). All state
//   is restored.
// - Dust (mode 2): the pixel's camera-space position comes from rt0.w (depth
//   in camera space) and the projection, its normal from rt0.xyz [verified:
//   truckpaint GLSL]; both go into the unit's frame (rig::GetFrame), where the
//   shader weighs the dust by height, wheels (arches, the fan behind them,
//   the wheel itself in its own frame, turned by its steering and rolling
//   from the telemetry: N12), top and rear faces, with noise fixed to the body.
// - N11 (compat): what the detours already see feeds the self-check of these
//   assumptions (stencil bits, G-buffer formats, the cb0 matrix, ref 2 in the
//   interior camera...); when one stops holding, the drawing (or only the
//   interior camera's dust) turns off and the log says why. See compat.h.
#pragma once

#include <d3d11_1.h>

#include <string>
#include <vector>

#include "dust_shader.h"

namespace tint {

// 0 = off, 1 = red (V3 test), 2 = dust (D3), 3 = position test (the albedo of
// the player's pixels = their position in the unit, in metres: tools/probetest).
// compat_enforce = false: the self-check only logs ([compat] enforce = 0).
void Configure(int mode, bool compat_enforce = true);
bool On();
// Any thread: the dust level (0..1) of unit 0 (truck) or 1 (trailer).
void SetDust(int unit, float level);
// Any thread: the D4 layers (N5) of unit 0 or 1.
void SetLayers(int unit, const dust::Layers& layers);
// N57: a trailer's cargo: its dirt gathered since it was picked up (cargo_dirt.h).
void SetCargo(int unit, float level, const dust::Layers& layers);
// Any thread (N12): the steering and rolling turn of each wheel of unit 0 or 1
// (SDK wheel.steering, wheel.rotation, in turns) and its speed, so the dirt
// of a wheel stays on the wheel.
void SetWheels(int unit, const float* steering, const float* rotation, int n, float speed_kmh);
// Our own calls are running: the detours only forward. SetBusy: the test panel's draws (gpu_probe).
bool Busy();
void SetBusy(bool busy);
// After an exception in the tint path (tint stays off from then on): the
// game's depth-stencil state is set again when a pass was open (c may be null:
// not set), the pass's references are dropped and Busy() is false.
void Abandon(ID3D11DeviceContext* c);
// D13e: after an exception: true (once) when the pass had drawn in its units' rectangles (D13d), which are now off;
// the caller then keeps the tint on, drawing over the whole viewport as before.
bool RectsFallback();

// Detours call these on the immediate context, before calling the game's function.
void OnSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
// May replace the state and reference the game is about to set (open G-buffer pass).
void FilterDepthStencilState(ID3D11DeviceContext* c, ID3D11DepthStencilState** s, UINT* ref);
void OnSetViewports(UINT n, const D3D11_VIEWPORT* vps);
void OnSetVSConstantBuffers(UINT start, UINT n, ID3D11Buffer* const* bufs, const UINT* first, const UINT* num);
void OnSetIndexBuffer(ID3D11Buffer* ib);
void OnSetVertexBuffers(UINT start, UINT n, ID3D11Buffer* const* vbs);
void OnMap(ID3D11Resource* res, const D3D11_MAPPED_SUBRESOURCE* m);
// Around a DrawIndexed: true when the depth-stencil state was swapped (call AfterDraw). Draws outside a G-buffer pass
// are left alone (the game's forward pass is never touched: see the vidro-futuro branch, N40).
bool BeforeDrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base);
// N27, any thread: a draw recorded on a deferred context (a command list), counted for the log, with the ones
// into a G-buffer apart. Nothing else: the tracker and the marking see the immediate context only.
void OnDeferredDraw(ID3D11DeviceContext* c);
// N28, render thread: what the last BeforeDrawIndexed made of its draw, for the frame captures:
// 0 truck, 1 trailer (both marked); -1 no G-buffer pass, -2 no view or VS cb0, -3 no mapped pointer to cb0,
// -4 cb0 rows not a matrix, -5 drawing off, -6 not the rig's (Classify), -7 no state variant, -8 read fault.
int LastVerdict();
// N28, any thread: an indirect draw (DrawAuto, DrawIndexedInstancedIndirect, DrawInstancedIndirect), counted for the
// log, the immediate context's ones in an open G-buffer pass apart. They are not classified.
void OnIndirectDraw(ID3D11DeviceContext* c, bool immediate);
void AfterDraw(ID3D11DeviceContext* c);
// N11: what the game thread knows, for the self-check (gpu_probe fills it at each Present).
struct GameState {
    bool paused = false;
    bool wipers = false;
    bool rain = false;     // rain seen on the GPU in the last second
    bool truck = false;    // the truck's wheel layout is known
    bool trailer = false;  // trailer.0 coupled, with its wheel layout
};
void OnPresent(ID3D11DeviceContext* c, const GameState& game);

// N41, any thread: the box of the truck that gets no dust, the cab's inside seen through its windows (seats,
// dashboard, bunk), around the driver's eye in the truck's frame (cabin.position + head.position): half its width
// (centred on the truck's middle), how far it goes below, above, ahead of and behind the eye (m). Not known, or a
// half width of 0: off. D11f: `cavity` the same for the cab's whole cavity (the sleeper too), where only the faces
// turned to its middle get no dust (the shell's outer faces turn away).
void SetCabInside(bool known, const float eye[3], const float box[5], const float cavity[5]);
// N40 step 1 (the user, 05/11: dirt on the glass without losing the game's glass): the truck's glass takes the
// truck's dirt. At the game's own multiply draw of the glass (forward pass) the same draw is issued once more with
// our pixel shader, so what is behind the glass is multiplied by the dirt's transmission; nothing else about the
// forward pass is touched. [glass] on; off by default. Any thread.
// `debug` ([glass] on): 2 = paint every surface the dirt reaches flat magenta, to see what it reaches;
// 3 = paint each of its pixels where the shader thinks it is in the truck (tools/probetest checks that).
void SetGlassDirt(bool on, float strength = 1.f, int debug = 0);
// N40e, any thread: how much light there is for the dust on the glass to scatter (0 night .. 1 full day).
void SetDaylight(float day);
// N40b: a DrawIndexedInstanced outside a G-buffer pass (the glass of some trucks may be drawn instanced: the
// catalog's glass shaders are all `.instanced` variants). Read-only unless it is the truck's glass.
void OnForwardInstanced(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start, INT base, UINT start_inst);
// D11k (N44): the dirt's relief in rt0 (the G-buffer's normal) on or off ([dust] relief); any thread.
void SetRelief(bool on);

// N12, render thread: the unit is tracked from its wheels only (it has not
// moved 1 m since it appeared, so its body gets no dirt yet).
bool WheelsOnly(int unit);

// One line for the log (every few seconds), and the first error, if any.
std::string StatsLine();
std::string TakeError();
// N11: the self-check's new log lines (any thread), and its one-line summary for the end of the session.
std::vector<std::string> TakeCompatLines();
std::string CompatSummary();

}  // namespace tint
