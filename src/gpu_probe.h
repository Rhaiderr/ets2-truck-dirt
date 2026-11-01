// Viability test V2: logging-only D3D11 hooks.
//
// Names every pixel shader the game draws with (DXBC checksum at creation,
// matched against the SM5X catalog built from effect.scs), logs what each one
// renders to the first time it draws, and keeps a per-minute census of draws.
// The glass-drop passes (def.glassdrops.*, the game's rain on the glass) get
// their pixel-shader constants copied by the GPU about once a second, so the
// log shows how many drops the game asks for and the wiper data. Nothing the
// game draws changes.
//
// V3a: every capture_s seconds one whole frame (Present to Present) is
// written to captures\capture_NNN.txt: every draw with its pixel shader,
// targets, viewport, depth/stencil state, buffers and the first rows of its
// vertex-shader constant buffers (copied by the GPU), plus the telemetry pose.
// Offline analysis finds which draws are the player's truck and trailer.
#pragma once

#include <string>
#include <vector>

#include "dust_shader.h"
#include "overlay.h"

namespace panel {
struct View;
}

namespace gpu_probe {

struct Config {
    int census_s = 60;     // census period, seconds
    int sample_ms = 1000;  // glass-drop constants, at most this often per shader
    int capture_s = 20;    // V3a: one frame capture every N seconds while driving (0 = off)
    int captures = 20;     // V3a: at most this many captures per session
    int tint = 0;          // 0 = off, 1 = paint the player's rig red (V3 test), 2 = dust (D3) (see tint.h)
    int verbose = 0;       // 1 = the V2/V2c lines every second (rain, big draws) and the glass-drop constants
    int compat_enforce = 1;  // N11: 1 = a broken render assumption turns its part off, 0 = only logged (see compat.h)
    // P3: the release's pre-map tables, next to the DLL (plugins\ets2_truck_dirt_premap), this game's only ("ats_",
    // "ets2_"); TruckDirt\premap is read too (tables made on the player's PC).
    std::wstring premap_dir, premap_prefix;
};

// V3a: what the telemetry says about the player's units, copied into each
// frame capture so the draws can be matched offline. Angles in turns, as the SDK.
constexpr int kPoseWheels = 14;
struct PoseWheels {
    int count = 0;
    float rotation[kPoseWheels] = {};
    float steering[kPoseWheels] = {};
    float deflection[kPoseWheels] = {};
    float lift[kPoseWheels] = {};
};
// N21 (step 1): the trailers behind trailer.0 (dollies and trailers of a road train), for the captures only.
constexpr int kPoseMoreTrailers = 4;  // trailer.1 .. trailer.4
struct PoseTrailer {
    double place[6] = {};
    bool connected = false;
    PoseWheels wheels;  // count 0: no such trailer
};
struct Pose {
    double truck[6] = {};    // x y z heading pitch roll
    double trailer[6] = {};  // trailer.0
    bool trailer_connected = false;
    float cabin[6] = {};     // truck.cabin.offset: x y z heading pitch roll
    float speed_kmh = 0.f;
    PoseWheels truck_wheels, trailer_wheels;
    PoseTrailer more[kPoseMoreTrailers];  // N21: trailer.1 ..
};

// Starts a thread that locates the D3D11 functions and hooks them. The log
// goes to <dir>\gpu_probe.log.
void Start(const std::wstring& dir, const Config& c);
// Detours stay in place as pass-through (the DLL is pinned once hooked).
void Stop();

// Game thread, every frame: telemetry copied into the GPU log lines.
void SetTelemetry(bool wipers, float speed_kmh, bool paused);
void SetPose(const Pose& pose);
// Any rain seen on the GPU in the last second (rain textures or glass drops,
// V2: both cameras without a weather mod, glass drops in the interior camera).
// False when the probe stopped or has not updated it for 2 s.
bool RainActive();
// D3: dust level of each unit (0 truck, 1 trailer) for the dust pass.
void SetDust(float truck, float trailer);
// N5: the D4 layers of each unit for the dust pass.
void SetLayers(const dust::Layers& truck, const dust::Layers& trailer);
// N57: the trailers' cargo (every trailer of a road train takes it, as their dirt).
void SetCargo(float level, const dust::Layers& cargo);

// N10: what the test panel shows, drawn on the game's back buffer at each Present (any thread).
void SetPanel(const panel::View& view);

// D5: the player's overlay (the wash at the station, the keys menu), drawn at each Present after the panel.
void SetOverlay(const overlay::View& view);
// The overlay's drawing stopped after an error (the game log says why).
bool OverlayFailed();

// Lines for the game log (game thread).
std::vector<std::string> TakeMessages();

}  // namespace gpu_probe
