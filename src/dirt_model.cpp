// D3 + D4: dirt layers per unit (see dirt_model.h).

#include "dirt_model.h"

#include <algorithm>
#include <cmath>

namespace dirt {
namespace {

constexpr double kInheritKm = 0.3;         // "static" keeps the last real ground this far
// Game minutes (the game clock runs ~20x real time: 120 game min = ~6 real min).
constexpr double kRainHalfLifeMin = 120.0;  // rain takes half of the dust off in this many game minutes
constexpr double kWetAfterRainMin = 60.0;   // the road dries this long after the rain

// D4 [estimate: every number here].
constexpr double kWetRiseMin = 10.0;        // the road soaks (time constant) while it rains
constexpr double kMudDryHalfMin = 30.0;     // wet mud -> dry mud on a dry road
constexpr double kMudSoftenHalfMin = 40.0;  // dry mud -> wet mud in hard rain on a soaked road
constexpr double kSprayDryHalfMin = 15.0;   // spray -> film on a dry road
constexpr double kSprayWashHalfMin = 90.0;  // hard rain takes spray and film
constexpr double kGritWashHalfMin = 60.0;   // rain washes the grit
constexpr double kSaltWashHalfMin = 60.0;   // rain washes the salt
constexpr double kSaltMeltHalfMin = 240.0;  // the salt melts off snow
constexpr double kFrontShare = 0.6;         // the truck's splash on the trailer front, per km
// N31, per km: paved ground wears the tread towards kTreadWearMax, fast (1 - e^(-1.5 * km) of the way) up to
// kTreadWearFast, then slower (e^(-0.4 * km)): ~0.72 after 1 km, ~0.83 after 3, ~0.9 after 8 (D4y: the user,
// 29/10, "more of the dirt should come off the more one drives on asphalt"). Other ground brings its dirt back,
// e^(-6 * km) of the wear left (17% after 300 m); road_dirt (a dusty paved road) slower, e^(-2 * km).
constexpr double kTreadWearPerKm = 1.5;
constexpr double kTreadWearSlowPerKm = 0.4;
constexpr float kTreadWearFast = 0.7f;
constexpr double kTreadDirtPerKm = 6.0;
constexpr double kTreadRoadDirtPerKm = 2.0;

// D14 (the user, ATS 31/10: the dust "accumulating very fast on the dirt road", "dirty the truck little by little"):
// the telemetry's odometer runs at the game's time scale (~20 km of it a real km driven: 16.6 km in the real minute
// at 49 km/h of 31/10 10:37), so the rates below (per km of it) were ~20x too fast: 0.11 -> 0.89 of dust in two real
// minutes of road_dirt. Now the dirt's own layers (dust, mud, grit) 20x slower, the spray and the salt 10x; the soot
// kept (D12c: it was set on that odometer already).
constexpr double kDirtSlow = 20.0, kWetSlow = 10.0;
// Rate per km of the splash layers on a soaked road (wet = 1), at 50 km/h.
// 50 km of wet dirt (~3 real minutes): 1 - e^(-0.025 * 50) = 0.71 (the lower part well muddy).
constexpr double kWetMudDirt = 0.5 / kDirtSlow, kWetMudRoadDirt = 0.35 / kDirtSlow, kWetMudGrass = 0.2 / kDirtSlow;
// 300 km of wet motorway at 80 km/h (~11 real minutes): 1 - e^(-0.0025 * 1.3 * 300) = 0.62.
constexpr double kSprayAsphalt = 0.025 / kWetSlow;
// 100 km of gravel (~6 real minutes): 1 - e^(-0.006 * 100) = 0.45.
constexpr double kGritGravel = 0.12 / kDirtSlow;
// 100 km of snow road: 1 - e^(-0.01 * 100) = 0.63.
constexpr double kSaltSnow = 0.1 / kWetSlow;
// N39 (the user, 29/10: on asphalt "a darker layer, like soot mixing oil, grease and the street's dirt with rubber"):
// twice as fast on a soaked road (the spray carries more of it). D12c (the user, ATS 30/10: "a 12 h trip to end up
// at about 30% dirt", the soot only, "much slower"): 12 game hours of motorway are ~1170 km (the session's 284 km in
// 175 game min at ~110 km/h, x1.5 for the speed): 1 - e^(-0.0002 * 1.5 * 1170) = 0.30. Was 0.0012 (0.40 in 284 km).
constexpr double kSootAsphalt = 0.0002;
constexpr double kSootWashHalfMin = 400.0;  // hard rain on a soaked road takes ~10% per game hour

// The layer that one turns into (wet mud <-> dry mud, spray <-> film); kLayers = none.
Layer Partner(Layer l) {
    switch (l) {
        case kWetMud: return kDryMud;
        case kDryMud: return kWetMud;
        case kSpray: return kFilm;
        case kFilm: return kSpray;
        default: return kLayers;
    }
}

// Moves `share` (0..1) of layer `from` into layer `to`, carrying its reach.
void Move(float* coat, float* reach, Layer from, Layer to, double share) {
    const float moved = static_cast<float>(coat[from] * share);
    if (!(moved > 0.f)) return;
    const float before = coat[to];
    coat[from] -= moved;
    coat[to] = (std::min)(1.f, before + moved);
    if (reach) reach[to] = (reach[to] * before + reach[from] * moved) / (before + moved);
}

// The share of a layer left after `minutes` with this half-life (in game minutes).
double Keep(double minutes, double half_life) { return std::exp2(-minutes / half_life); }

}  // namespace

Ground GroundOf(const std::string& s) {
    if (s == "dirt") return kDirt;
    if (s == "road_dirt") return kRoadDirt;
    if (s == "gravel") return kGravel;
    if (s == "grass") return kGrass;
    if (s == "snow" || s == "road_snow" || s == "ice") return kSnow;
    if (s == "static" || s == "invis" || s.empty() || s[0] == '#') return kInherit;
    return kAsphalt;  // road, road_smooth, road_coarse, concrete, rumble_stripe, metal, wood...
}

const char* LayerName(Layer l) {
    switch (l) {
        case kDust: return "dust";
        case kWetMud: return "wet_mud";
        case kDryMud: return "dry_mud";
        case kSpray: return "spray";
        case kFilm: return "film";
        case kGrit: return "grit";
        case kSalt: return "salt";
        case kSoot: return "soot";
        default: return "?";
    }
}

// [estimate] 5 km of dirt: 1 - e^(-0.25 * 5) = 0.71; D14: 20x slower (kDirtSlow): 100 km (~6 real minutes at
// 50 km/h) to 0.71, ~14% a real minute of road_dirt; more with the speed (Drive). D12c (the user, ATS 30/10: "I did not expect such a heavy
// dust layer just from driving on asphalt, it should only come on dry dirt roads"): paved ground lays no dust (was
// 0.004 per km: 0.68 in the session's 284 km); the road's own grime there is the soot (N39).
float RatePerKm(Ground g) {
    switch (g) {
        case kDirt: return static_cast<float>(0.25 / kDirtSlow);
        case kRoadDirt: return static_cast<float>(0.18 / kDirtSlow);
        case kGravel: return static_cast<float>(0.15 / kDirtSlow);
        case kGrass: return static_cast<float>(0.08 / kDirtSlow);
        case kSnow: return 0.f;
        default: return 0.f;
    }
}

// Saturating deposit of every layer with a rate: level += (1 - e^(-rate * km)) * room,
// where the room is what the layer and its partner leave uncovered.
void Model::Splash(float* coat, float* reach, const double* rate_km, double km, float height) {
    for (int k = kWetMud; k < kLayers; ++k) {
        if (!(rate_km[k] > 0)) continue;
        const Layer p = Partner(static_cast<Layer>(k));
        const float room = (std::max)(0.f, 1.f - coat[k] - (p != kLayers ? coat[p] : 0.f));
        const float added = static_cast<float>((1.0 - std::exp(-rate_km[k] * km)) * room);
        if (!(added > 0.f)) continue;
        if (reach) reach[k] = (reach[k] * coat[k] + height * added) / (coat[k] + added);
        coat[k] = (std::min)(1.f, coat[k] + added);
    }
}

void Model::Drive(int unit, double km, const Ground* ground, const bool* on_ground, int wheels, float speed_kmh) {
    if (unit == 0) front_km_ = 0;  // a new truck step: the old splash is gone
    if (unit < 0 || unit >= kUnits) return;
    if (unit == 1 && front_km_ > 0) {  // the truck's wheels splash the trailer front
        Splash(coat_[1].front, nullptr, front_rate_, front_km_, front_height_);
        front_km_ = 0;
    }
    if (!(km > 0) || km > 1.0) return;  // > 1 km in one step: a teleport
    // The unit's ground: the most common real class among the wheels on the ground.
    int count[kGroundKinds] = {};
    int touching = 0;
    for (int w = 0; w < wheels; ++w) {
        if (!on_ground[w]) continue;
        ++touching;
        ++count[ground[w]];
    }
    if (!touching) return;
    Ground g = kInherit;
    int best = 0;
    for (int k = 0; k < kGroundKinds; ++k)
        if (k != kInherit && count[k] > best) best = count[k], g = static_cast<Ground>(k);
    if (g == kInherit) {
        inherit_km_[unit] += km;
        g = inherit_km_[unit] <= kInheritKm ? last_[unit] : kAsphalt;
    } else {
        last_[unit] = g;
        inherit_km_[unit] = 0;
    }
    const double share = double(touching) / wheels;
    // N31: paved ground wears the tread's dirt off, any other ground puts it back.
    float& wear = tread_wear_[unit];
    if (g == kAsphalt) {
        // The fast part up to kTreadWearFast, the slow one past it: the km the fast part takes, then the rest
        // (exact, so any split of a step gives the same wear).
        const double scale = settings_.rate * share;
        double left = km, w = wear;
        if (w < kTreadWearFast && scale > 0) {
            const double fast_km = std::log((kTreadWearMax - w) / (kTreadWearMax - kTreadWearFast)) / (kTreadWearPerKm * scale);
            const double d = (std::min)(left, fast_km);
            w += (1.0 - std::exp(-kTreadWearPerKm * scale * d)) * (kTreadWearMax - w);
            left -= d;
        }
        if (left > 0) w += (1.0 - std::exp(-kTreadWearSlowPerKm * scale * left)) * (kTreadWearMax - w);
        wear = static_cast<float>((std::min)(w, double(kTreadWearMax)));
    } else {
        const double k = g == kRoadDirt ? kTreadRoadDirtPerKm : kTreadDirtPerKm;
        wear *= static_cast<float>(std::exp(-k * settings_.rate * share * km));
    }
    // More splash per km and higher on the body with speed: x1 and half height at 50 km/h. D14: the dust too (the
    // dust a tyre lifts grows with the speed [TRB, "Road dust lifting by vehicles with pneumatic tyres"]).
    const bool known = speed_kmh >= 0.f;
    const double v = known ? double(speed_kmh) : 50.0;
    const double faster = std::clamp(0.5 + v / 100.0, 0.5, 1.5);
    if (wet_minutes_ <= 0) {  // wet road: no dust
        const double rate = RatePerKm(g) * faster * settings_.rate * share;
        level_[unit] += static_cast<float>((1.0 - std::exp(-rate * km)) * (1.0 - level_[unit]));
        level_[unit] = std::clamp(level_[unit], 0.f, 1.f);
    }

    // D4 splash: the ground, the wetness and the speed.
    double rate_km[kLayers] = {};
    const double wet = wet_;
    switch (g) {
        case kDirt: rate_km[kWetMud] = kWetMudDirt * wet; break;
        case kRoadDirt: rate_km[kWetMud] = kWetMudRoadDirt * wet; break;
        case kGrass: rate_km[kWetMud] = kWetMudGrass * wet; break;
        case kGravel: rate_km[kGrit] = kGritGravel * (1.0 - 0.5 * wet); break;
        case kSnow: rate_km[kSalt] = kSaltSnow; break;
        default:
            rate_km[kSpray] = kSprayAsphalt * wet;
            rate_km[kSoot] = kSootAsphalt * (1.0 + wet);  // N39
            break;
    }
    const float height = known ? static_cast<float>(std::clamp((v - 10.0) / 80.0, 0.0, 1.0)) : 0.5f;
    for (double& r : rate_km) r *= faster * settings_.rate * share;
    Splash(coat_[unit].body, coat_[unit].reach, rate_km, km, height);
    if (unit == 0) {
        for (int k = 0; k < kLayers; ++k) front_rate_[k] = rate_km[k] * kFrontShare;
        front_km_ = km;
        front_height_ = height;
    }
}

// Drying, softening and washing of one coat over `minutes`, with the road's mean wetness then.
void Model::Age(float* coat, float* reach, double minutes, double wet, bool raining, float intensity, bool on_snow) {
    const double dry = 1.0 - wet;  // the drying goes as fast as the road dries
    if (dry > 0) {
        Move(coat, reach, kWetMud, kDryMud, 1.0 - Keep(minutes * dry, kMudDryHalfMin));
        Move(coat, reach, kSpray, kFilm, 1.0 - Keep(minutes * dry, kSprayDryHalfMin));
    }
    if (raining && intensity > 0.f) {
        // Hard and long rain (a soaked road) softens the dry mud and takes spray, film, grit and salt.
        const double hard = double(intensity) * wet;
        if (hard > 0) Move(coat, reach, kDryMud, kWetMud, 1.0 - Keep(minutes * hard, kMudSoftenHalfMin));
        const double spray = Keep(minutes * hard, kSprayWashHalfMin);
        coat[kSpray] *= static_cast<float>(spray);
        coat[kFilm] *= static_cast<float>(spray);
        coat[kGrit] *= static_cast<float>(Keep(minutes * intensity, kGritWashHalfMin));
        coat[kSalt] *= static_cast<float>(Keep(minutes * intensity, kSaltWashHalfMin));
        coat[kSoot] *= static_cast<float>(Keep(minutes * hard, kSootWashHalfMin));  // N39
    }
    if (!on_snow) coat[kSalt] *= static_cast<float>(Keep(minutes, kSaltMeltHalfMin));
}

void Model::Weather(double minutes, bool raining, float intensity) {
    if (!(minutes > 0) || minutes > 120.0) return;  // a long jump: sleep, ferry, load
    intensity = std::clamp(intensity, 0.f, 1.f);
    // The mean wetness over the step (exact for both curves below), so that the
    // drying and the softening do not depend on how long the steps are.
    double mean_wet = 0;
    if (raining) {
        wet_minutes_ = kWetAfterRainMin;
        const float keep = static_cast<float>(std::exp2(-minutes * intensity / kRainHalfLifeMin));
        for (float& l : level_) l *= keep;
        if (wet_ < intensity) {  // towards the intensity: w(t) = I - (I - w0) e^(-t / T)
            const double rise = 1.0 - std::exp(-minutes / kWetRiseMin);
            mean_wet = intensity - (intensity - wet_) * kWetRiseMin * rise / minutes;
            wet_ += (intensity - wet_) * rise;
        } else {
            mean_wet = wet_;
        }
        wet_peak_ = wet_;
    } else if (wet_minutes_ > 0) {  // dries linearly with the hour after the rain
        const double before = wet_, left = wet_minutes_;
        wet_minutes_ = (std::max)(0.0, wet_minutes_ - minutes);
        wet_ = wet_peak_ * wet_minutes_ / kWetAfterRainMin;
        mean_wet = (before + wet_) / 2 * (left - wet_minutes_) / minutes;
    } else {
        wet_ = wet_peak_ = 0;
    }
    for (int u = 0; u < kUnits; ++u) {
        const bool on_snow = last_[u] == kSnow;
        Age(coat_[u].body, coat_[u].reach, minutes, mean_wet, raining, intensity, on_snow);
        Age(coat_[u].front, nullptr, minutes, mean_wet, raining, intensity, on_snow);
    }
}

bool Forced::Any() const {
    for (int u = 0; u < kUnits; ++u) {
        if (reach[u] >= 0.f) return true;
        for (float l : level[u])
            if (l >= 0.f) return true;
    }
    return false;
}

float Model::Level(int unit) const {
    if (unit < 0 || unit >= kUnits) return 0.f;
    if (forced_.level[unit][kDust] >= 0.f) return (std::min)(forced_.level[unit][kDust], 1.f);
    if (settings_.override_level >= 0.f) return (std::min)(settings_.override_level, 1.f);
    return level_[unit];
}

Coat Model::Layers(int unit) const {
    if (unit < 0 || unit >= kUnits) return {};
    Coat c = coat_[unit];
    c.body[kDust] = Level(unit);
    for (int k = kWetMud; k < kLayers; ++k) {
        float o = settings_.override_layer[k];
        const float f = forced_.level[unit][k];
        if (f >= 0.f) o = f;
        if (!(o >= 0.f)) continue;
        c.body[k] = (std::min)(o, 1.f);
        if (unit == 1) c.front[k] = c.body[k];
        if (f >= 0.f && forced_.reach[unit] >= 0.f) c.reach[k] = (std::min)(forced_.reach[unit], 1.f);
    }
    return c;
}

Coat Model::Real(int unit) const {
    if (unit < 0 || unit >= kUnits) return {};
    Coat c = coat_[unit];
    c.body[kDust] = level_[unit];
    return c;
}

void Model::Reset(int unit) {
    Set(unit, 0.f);
    if (unit >= 0 && unit < kUnits) coat_[unit] = Coat{}, tread_wear_[unit] = kTreadWearMax;
}

void Model::Set(int unit, float level) {
    if (unit < 0 || unit >= kUnits) return;
    level_[unit] = std::clamp(level, 0.f, 1.f);
    inherit_km_[unit] = 0;
    last_[unit] = kAsphalt;
}

void Model::SetLayer(int unit, Layer l, float level, bool front) {
    if (unit < 0 || unit >= kUnits || l < 0 || l >= kLayers) return;
    if (l == kDust) {
        if (!front) level_[unit] = std::clamp(level, 0.f, 1.f);
        return;
    }
    float* coat = front ? coat_[unit].front : coat_[unit].body;
    const Layer p = Partner(l);
    coat[l] = std::clamp(level, 0.f, 1.f - (p != kLayers ? coat[p] : 0.f));
}

void Model::SetReach(int unit, Layer l, float reach) {
    if (unit < 0 || unit >= kUnits || l <= kDust || l >= kLayers) return;
    coat_[unit].reach[l] = std::clamp(reach, 0.f, 1.f);
}

}  // namespace dirt
