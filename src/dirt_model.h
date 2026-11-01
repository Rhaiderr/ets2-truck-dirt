// D3 + D4: how dirty the player's truck and trailer are, per layer (0 = clean,
// 1 = covered).
//
// Pure bookkeeping, no game or D3D calls (tools/dusttest runs it offline).
// Everything moves per km driven (deposits) and per game minute (rain, drying),
// never per frame: the result does not depend on the frame rate.
//
// Dust (D3, unchanged by D4):
// - dust grows per km driven, saturating (level += rate * km * (1 - level)),
//   with a rate per ground class from the wheels on the ground [verified: V1,
//   substance per wheel]: dirt fast, road_dirt / gravel a bit slower, grass
//   slow, asphalt very slow, snow and ice none;
// - "static" (pre-built road pieces: bridges, junctions, yards) and "invis"
//   keep the last real ground for 300 m, then count as asphalt [verified: V1,
//   static is a third of the samples even at 90 km/h];
// - wheels in the air do not count; a unit with every wheel in the air does
//   not get dirtier (any layer);
// - rain washes the dust off over time (half in 2 game hours, ~6 real
//   minutes), driving or not, and the road stays wet for 1 game hour after
//   the rain: no dust then;
// - targets [estimate, from the plan]: ~1 h on dry asphalt = lightly dusty,
//   ~5 km of dry dirt road = mostly dusty.
//
// The other layers (D4, table in the development notes) [estimate: every rate and
// time below]:
// - the road wetness (0..1) is plugin state: it rises during the rain
//   (towards the rain intensity, ~10 game min) and falls back to 0 over the
//   hour the road stays wet;
// - wet mud: wet road + dirt / road_dirt / grass; dries into dry mud once the
//   road is dry (half in 30 game min); rain softens dry mud back into wet
//   mud, faster the harder and the longer it rains;
// - road spray: wet road + asphalt (road, concrete...); dries into a grey
//   film (half in 15 game min); rain takes part of the spray and the film;
// - gravel grit: gravel, wet or dry (half as much when wet); rain washes it;
// - salt crust: snow, road_snow, ice; rain washes it, and it melts slowly
//   (half in 4 game hours) while the unit is not on snow;
// - N39, road soot (oil, grease, rubber and the street's grime): asphalt and
//   concrete, dry or wet (twice as fast wet); slow (~300 km of motorway = a
//   light coat low down, ~1000 km = well marked); rain hardly takes it (~10%
//   per game hour of hard rain), the wash does;
// - the faster, the more splash per km (mud, spray, grit, salt; not dust) and
//   the higher it reaches on the body (Reach: 0 = only around the wheels,
//   1 = up to the top of the splash zone);
// - the front of the trailer takes the splash of the truck's wheels: each
//   Drive(0) step leaves its splash for the next Drive(1) step (the plugin
//   drives the truck, then the trailer, with the same km);
// - targets [estimate, from the plan]: 2-3 km of wet dirt = the lower part
//   well muddy.
// The rates can be scaled and the levels forced from the INI (tests), and
// forced per unit and layer by the test panel (Forced, N10). Forcing changes
// only what Level() and Layers() report: the simulation runs on underneath.
//
// N31, the tread (the face of the tyre on the road) [estimate: both rates]:
// the wheels take the unit's layers (side walls and tread alike), but paved
// ground (asphalt, concrete... and "static" past its 300 m) wears the tread's
// share of them off by km, towards kTreadWearMax (most of the way in 1-2 km, the
// rest over the next ~8 km);
// any other ground (dirt, grass, gravel, snow) puts it back within a few
// hundred metres, road_dirt within ~1 km. Wet or dry alike: the shader keeps the wet spray on
// the tread. Not saved: a unit restored or cleaned starts at kTreadWearMax
// (the last km before a save are nearly always on a road), and the test
// panel's forced levels leave it as the simulation has it.
#pragma once

#include <string>

namespace dirt {

constexpr int kUnits = 2;  // 0 = truck, 1 = trailer.0

enum Ground { kAsphalt, kDirt, kRoadDirt, kGravel, kGrass, kSnow, kInherit, kGroundKinds };

// N31: the share of the wheels' dirt the road wears off the tread at most [estimate]: after long on a road the
// tread keeps less than a tenth of it (D4y; was a fifth), the rubber's dark colour showing.
constexpr float kTreadWearMax = 0.92f;

// Dirt layers. Wet mud + dry mud <= 1, spray + film <= 1 (one turns into the other).
enum Layer { kDust, kWetMud, kDryMud, kSpray, kFilm, kGrit, kSalt, kSoot, kLayers };

// The class of an SDK substance name ("road", "dirt", "static"...).
Ground GroundOf(const std::string& substance);

// A short name of the layer ("dust", "wet_mud"...), for logs and the INI.
const char* LayerName(Layer l);

struct Settings {
    float rate = 1.f;        // multiplies every rate
    float override_level = -1.f;  // >= 0: the dust level is forced (test)
    // >= 0: the layer is forced, body and trailer front (test; kDust: see override_level).
    float override_layer[kLayers] = {-1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f};
};

// Test levels per unit and layer (the test panel): >= 0 forces the layer (body,
// and the trailer's front), < 0 = the real one. reach >= 0 forces the reach of
// the forced layers. They win over the INI overrides.
struct Forced {
    float level[kUnits][kLayers] = {{-1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f}, {-1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f}};
    float reach[kUnits] = {-1.f, -1.f};
    bool Any() const;
};

// One unit's dirt.
struct Coat {
    float body[kLayers] = {};   // the body around and behind the wheels
    float front[kLayers] = {};  // the front face (trailer: the truck's splash; truck: always 0)
    float reach[kLayers] = {};  // how high the body's layer reaches, 0..1 (from the speed it was thrown at)
};

class Model {
public:
    void Configure(const Settings& s) { settings_ = s; }
    // The test levels (replace the previous ones; Forced{} = all real).
    void Force(const Forced& f) { forced_ = f; }
    const Forced& GetForced() const { return forced_; }
    // Driving step of one unit: km driven since the last step, the ground
    // class under each wheel and whether it touches the ground, and the speed
    // (km/h; < 0 = unknown, counts as 50 km/h).
    void Drive(int unit, double km, const Ground* ground, const bool* on_ground, int wheels, float speed_kmh = -1.f);
    // Weather step (both units): game minutes passed, whether it rains and how
    // hard (0..1; 1 when only "it rains" is known).
    void Weather(double minutes, bool raining, float intensity = 1.f);
    // The dust level (D3), with the INI override and the forced level.
    float Level(int unit) const;
    // Every layer of the unit (with the INI overrides and the forced levels).
    Coat Layers(int unit) const;
    // Every layer as the simulation has it, without any override (what is saved).
    Coat Real(int unit) const;
    // The road wetness, 0 = dry .. 1 = soaked.
    float Wetness() const { return static_cast<float>(wet_); }
    // N31: the share of the wheels' dirt the road wore off the tread, 0 .. kTreadWearMax.
    float TreadWear(int unit) const { return unit >= 0 && unit < kUnits ? tread_wear_[unit] : 0.f; }
    // Clean: every layer of the unit.
    void Reset(int unit);
    // The dust level only (the other layers stay).
    void Set(int unit, float level);
    // One layer of the body (front = false) or of the front face.
    void SetLayer(int unit, Layer l, float level, bool front = false);
    // How high one layer of the body reaches, 0..1 (restoring a saved unit).
    void SetReach(int unit, Layer l, float reach);

private:
    void Splash(float* coat, float* reach, const double* rate_km, double km, float height);
    void Age(float* coat, float* reach, double minutes, double wet, bool raining, float intensity, bool on_snow);

    Settings settings_;
    Forced forced_;
    float level_[kUnits] = {};  // dust (kept apart: the D3 code path, unchanged)
    Coat coat_[kUnits];         // the D4 layers (coat_[u].body[kDust] unused: level_)
    Ground last_[kUnits] = {kAsphalt, kAsphalt};  // last real ground, for "static"
    double inherit_km_[kUnits] = {};               // km driven on "static" since the last real ground
    float tread_wear_[kUnits] = {kTreadWearMax, kTreadWearMax};  // N31
    double wet_minutes_ = 0;                        // the road is still wet this long
    double wet_ = 0;                                // road wetness 0..1
    double wet_peak_ = 0;                           // the wetness when the rain stopped
    // The truck's last step, for the trailer front: splash rate per km of each layer, km, height.
    double front_rate_[kLayers] = {};
    double front_km_ = 0;
    float front_height_ = 0;
};

// Rate per km of each ground class (before Settings::rate).
float RatePerKm(Ground g);

}  // namespace dirt
