// Offline test of the dirt model (src/dirt_model): drives made-up routes
// in 100 m steps and checks the levels against the targets of the plan.
// The D3 dust targets come first (unchanged), then the D4 layers.
// Usage: dusttest

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "dirt_model.h"
#include "dust_shader.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what, double value) {
    printf("%s %-58s %.3f\n", ok ? "ok  " : "FAIL", what, value);
    if (!ok) ++g_failures;
}

// Drives `km` on `substance` with all 6 wheels on the ground (or none).
void Drive(dirt::Model& m, int unit, double km, const char* substance, bool on_ground = true) {
    dirt::Ground g[6];
    bool og[6];
    for (int w = 0; w < 6; ++w) g[w] = dirt::GroundOf(substance), og[w] = on_ground;
    for (double d = 0; d < km - 1e-9; d += 0.1) m.Drive(unit, 0.1, g, og, 6);
}

// D4: drives `km` on `substance` at `kmh` in steps of `step` km; with `rig`, the
// trailer (unit 1) follows the truck at each step, as the plugin does.
void DriveAt(dirt::Model& m, int unit, double km, const char* substance, float kmh, bool rig = false,
             double step = 0.1) {
    dirt::Ground g[6];
    bool og[6];
    for (int w = 0; w < 6; ++w) g[w] = dirt::GroundOf(substance), og[w] = true;
    const int steps = static_cast<int>(km / step + 0.5);
    for (int i = 0; i < steps; ++i) {
        m.Drive(unit, step, g, og, 6, kmh);
        if (rig) m.Drive(1, step, g, og, 6, kmh);
    }
}

// D4: `minutes` of game time in 1-minute steps (or `step`), raining or not.
void Wait(dirt::Model& m, double minutes, bool raining, float intensity = 1.f, double step = 1.0) {
    const int steps = static_cast<int>(minutes / step + 0.5);
    for (int i = 0; i < steps; ++i) m.Weather(step, raining, intensity);
}

// D14: the dirt's own layers 20x slower per km (dust, mud, grit), the spray and the salt 10x (dirt_model.cpp):
// the distances below scaled so, the targets as before.
constexpr double kDirtKm = 20.0, kWetKm = 10.0;

float Body(const dirt::Model& m, int unit, dirt::Layer l) { return m.Layers(unit).body[l]; }
float Front(const dirt::Model& m, int unit, dirt::Layer l) { return m.Layers(unit).front[l]; }
float Reach(const dirt::Model& m, int unit, dirt::Layer l) { return m.Layers(unit).reach[l]; }

// D4 targets [estimate: from the plan in the development notes].
void Layers() {
    using namespace dirt;
    {
        Model m;
        Wait(m, 60, true);
        Check(m.Wetness() > 0.99f, "D4: 1 game hour of rain soaks the road", m.Wetness());
        DriveAt(m, 0, 2.5 * kDirtKm, "dirt", 50);
        const float mud = Body(m, 0, kWetMud);
        Check(mud > 0.6f && mud < 0.8f, "50 km of wet dirt at 50 km/h: the lower part well muddy", mud);
        Check(m.Level(0) == 0.f, "... and no dust (wet road)", m.Level(0));
        Wait(m, 30, false);
        Check(m.Wetness() > 0.45f && m.Wetness() < 0.55f, "30 game min after the rain: road half dry", m.Wetness());
        Wait(m, 30, false);
        Check(m.Wetness() == 0.f, "60 game min after the rain: road dry", m.Wetness());
        Wait(m, 120, false);
        const float wet = Body(m, 0, kWetMud), dry = Body(m, 0, kDryMud);
        Check(dry > 4 * wet, "3 game hours later the mud is mostly dry", dry);
        Check(std::fabs(wet + dry - mud) < 0.001f, "... and none of it is lost drying", wet + dry);
        Wait(m, 60, true);
        Check(Body(m, 0, kWetMud) > Body(m, 0, kDryMud), "1 game hour of hard rain softens the dry mud",
              Body(m, 0, kWetMud));
        Model light;
        light.SetLayer(0, kDryMud, 0.6f);
        Wait(light, 60, true, 0.3f);
        Check(Body(light, 0, kDryMud) > Body(m, 0, kDryMud) + 0.1f, "... light rain softens less",
              Body(light, 0, kDryMud));
    }
    {
        Model m;
        DriveAt(m, 0, 5, "dirt", 50);
        Check(Body(m, 0, kWetMud) == 0.f && Body(m, 0, kSpray) == 0.f, "dry dirt: no mud", Body(m, 0, kWetMud));
        DriveAt(m, 0, 30, "road", 80);
        Check(Body(m, 0, kSpray) == 0.f, "dry asphalt: no spray", Body(m, 0, kSpray));
        Wait(m, 60, true);
        DriveAt(m, 0, 30 * kWetKm, "road", 80);  // D14: 10x longer
        const float spray = Body(m, 0, kSpray);
        Check(spray > 0.5f && spray < 0.75f, "300 km of wet motorway at 80 km/h: spray", spray);
        Check(Body(m, 0, kWetMud) == 0.f, "... and no mud", Body(m, 0, kWetMud));
        Wait(m, 60, true);
        Check(Body(m, 0, kSpray) < 0.8f * spray, "hard rain takes part of the spray", Body(m, 0, kSpray));
        Wait(m, 180, false);
        Check(Body(m, 0, kFilm) > 10 * Body(m, 0, kSpray), "dry again: the spray turns into film", Body(m, 0, kFilm));
    }
    {
        // N39: the road soot.
        Model m;
        DriveAt(m, 0, 300, "road", 80);
        const float light = Body(m, 0, kSoot);
        // D12c: much slower (the user, ATS 30/10: about 30% after a 12 h trip).
        Check(light > 0.05f && light < 0.1f, "N39: 300 km of dry motorway at 80 km/h: a hint of soot", light);
        DriveAt(m, 0, 700, "road", 80);
        Check(Body(m, 0, kSoot) > 0.18f && Body(m, 0, kSoot) < 0.28f, "... 1000 km: about a fifth", Body(m, 0, kSoot));
        Model trip;
        DriveAt(trip, 0, 1170, "road", 110);
        Check(Body(trip, 0, kSoot) > 0.25f && Body(trip, 0, kSoot) < 0.35f,
              "D12c: 12 game hours of motorway (~1170 km at 110 km/h): about 30%", Body(trip, 0, kSoot));
        const float before = Body(m, 0, kSoot);
        Wait(m, 60, true);
        Wait(m, 60, true);
        Check(Body(m, 0, kSoot) > 0.8f * before && Body(m, 0, kSoot) < before, "... 2 game hours of hard rain take little",
              Body(m, 0, kSoot) / before);
        Model dirt_road;
        DriveAt(dirt_road, 0, 50, "dirt", 50);
        Check(Body(dirt_road, 0, kSoot) == 0.f, "... none on dirt", Body(dirt_road, 0, kSoot));
        Model wet;
        Wait(wet, 60, true);
        DriveAt(wet, 0, 30, "road", 80);
        Model dry;
        DriveAt(dry, 0, 30, "road", 80);
        Check(Body(wet, 0, kSoot) > 1.8f * Body(dry, 0, kSoot), "... twice as fast on a soaked road",
              Body(wet, 0, kSoot) / Body(dry, 0, kSoot));
        m.Reset(0);
        Check(Body(m, 0, kSoot) == 0.f, "... the wash takes it", Body(m, 0, kSoot));
    }
    {
        Model m;
        DriveAt(m, 0, 5 * kDirtKm, "gravel", 50);
        const float grit = Body(m, 0, kGrit);
        Check(grit > 0.4f && grit < 0.5f, "100 km of dry gravel: grit", grit);
        Check(m.Level(0) > 0.4f, "... and dust, as in D3", m.Level(0));
        Wait(m, 60, true);
        Check(Body(m, 0, kGrit) > 0.45f * grit && Body(m, 0, kGrit) < 0.55f * grit, "1 game hour of rain halves the grit",
              Body(m, 0, kGrit));
    }
    {
        Model m;
        DriveAt(m, 0, 10 * kWetKm, "road_snow", 50);
        const float salt = Body(m, 0, kSalt);
        Check(salt > 0.55f && salt < 0.7f, "100 km of snow road: salt crust", salt);
        Wait(m, 240, false);
        Check(std::fabs(Body(m, 0, kSalt) - salt) < 1e-6f, "parked on snow: the salt stays", Body(m, 0, kSalt));
        DriveAt(m, 0, 0.1, "road", 50);
        Wait(m, 240, false);
        Check(std::fabs(Body(m, 0, kSalt) - 0.5f * salt) < 0.01f, "4 game hours off the snow: half melts",
              Body(m, 0, kSalt));
    }
    {
        Model slow, fast;
        Wait(slow, 60, true);
        Wait(fast, 60, true);
        DriveAt(slow, 0, kDirtKm, "dirt", 30);
        DriveAt(fast, 0, kDirtKm, "dirt", 90);
        Check(Body(fast, 0, kWetMud) > 1.5f * Body(slow, 0, kWetMud), "90 km/h splashes more mud per km than 30",
              Body(fast, 0, kWetMud) / Body(slow, 0, kWetMud));
        Check(Reach(fast, 0, kWetMud) > 0.95f && Reach(slow, 0, kWetMud) < 0.3f, "... and higher on the body",
              Reach(fast, 0, kWetMud));
        DriveAt(slow, 0, kDirtKm, "dirt", 90);
        const float r = Reach(slow, 0, kWetMud);
        Check(r > 0.25f && r < 0.95f, "the reach is the mean of the speeds, by amount", r);
        Wait(slow, 180, false);
        Check(std::fabs(Reach(slow, 0, kDryMud) - r) < 0.001f, "... and stays with the mud when it dries",
              Reach(slow, 0, kDryMud));
    }
    {
        Model m;
        Wait(m, 60, true);
        DriveAt(m, 0, 2 * kDirtKm, "dirt", 60, true);
        const float front = Front(m, 1, kWetMud);
        Check(front > 0.3f && front < Body(m, 0, kWetMud), "the trailer front takes the truck's mud", front);
        Check(Front(m, 0, kWetMud) == 0.f, "... the truck front does not", Front(m, 0, kWetMud));
        Model solo;
        Wait(solo, 60, true);
        DriveAt(solo, 1, 2 * kDirtKm, "dirt", 60);
        Check(Front(solo, 1, kWetMud) == 0.f && Body(solo, 1, kWetMud) > 0.5f,
              "a trailer driven alone: no truck splash on its front", Body(solo, 1, kWetMud));
        Wait(m, 240, false);
        Check(Front(m, 1, kDryMud) > 0.9f * front, "the front's mud dries too", Front(m, 1, kDryMud));
    }
    {
        Model m;
        Wait(m, 60, true);
        dirt::Ground g[6];
        bool og[6];
        for (int w = 0; w < 6; ++w) g[w] = GroundOf("dirt"), og[w] = false;
        for (int i = 0; i < 50; ++i) m.Drive(0, 0.1, g, og, 6, 50.f);
        Check(Body(m, 0, kWetMud) == 0.f, "wheels in the air: no mud", Body(m, 0, kWetMud));
        for (int w = 0; w < 3; ++w) og[w] = true;
        for (int i = 0; i < 10; ++i) m.Drive(0, 0.1, g, og, 6, 50.f);
        Model all;
        Wait(all, 60, true);
        DriveAt(all, 0, 1, "dirt", 50);
        const float half = Body(m, 0, kWetMud), full = Body(all, 0, kWetMud);
        Check(half > 0.4f * full && half < 0.6f * full, "half the wheels on the ground: about half the mud", half / full);
        m.Drive(0, 5.0, g, og, 6, 50.f);
        Check(Body(m, 0, kWetMud) == half, "a teleport (> 1 km in one step) is ignored", Body(m, 0, kWetMud));
    }
    {
        // Frame-rate independence: the same route in 10 m / 1 km steps and 0.1 / 5 game minute steps.
        Model a, b;
        Wait(a, 60, true, 1.f, 0.1);
        Wait(b, 60, true, 1.f, 5.0);
        DriveAt(a, 0, 3, "dirt", 60, true, 0.01);
        DriveAt(b, 0, 3, "dirt", 60, true, 1.0);
        DriveAt(a, 0, 20, "road", 80, true, 0.01);
        DriveAt(b, 0, 20, "road", 80, true, 1.0);
        Wait(a, 90, false, 1.f, 0.1);
        Wait(b, 90, false, 1.f, 5.0);
        float worst = 0.f;
        const Coat ca = a.Layers(0), cb = b.Layers(0), fa = a.Layers(1), fb = b.Layers(1);
        for (int k = 0; k < kLayers; ++k) {
            worst = (std::max)(worst, std::fabs(ca.body[k] - cb.body[k]));
            worst = (std::max)(worst, std::fabs(fa.front[k] - fb.front[k]));
        }
        Check(worst < 0.01f, "small or big steps: the same layers (max difference)", worst);
    }
    {
        Model m;
        Wait(m, 60, true);
        DriveAt(m, 0, 2 * kDirtKm, "dirt", 60, true);
        DriveAt(m, 1, 2, "road", 60);
        m.Reset(1);
        float left = 0.f;
        for (int k = 0; k < kLayers; ++k) left += Body(m, 1, static_cast<Layer>(k)) + Front(m, 1, static_cast<Layer>(k));
        Check(left == 0.f && Body(m, 0, kWetMud) > 0.5f, "a new trailer starts clean (every layer)", left);
        m.Set(0, 0.3f);
        Check(m.Level(0) == 0.3f && Body(m, 0, kWetMud) > 0.5f, "setting the dust leaves the mud", Body(m, 0, kWetMud));
        Settings s;
        s.override_layer[kDryMud] = 0.8f;
        Model forced;
        forced.Configure(s);
        Check(Body(forced, 0, kDryMud) == 0.8f && Front(forced, 1, kDryMud) == 0.8f && Front(forced, 0, kDryMud) == 0.f,
              "INI override forces a layer (body and trailer front)", Body(forced, 0, kDryMud));
        Model fast;
        fast.Configure({10.f, -1.f});
        Wait(fast, 60, true);
        DriveAt(fast, 0, 0.3 * kDirtKm, "dirt", 50);
        Check(std::fabs(Body(fast, 0, kWetMud) - 0.777f) < 0.01f, "rate x10: 6 km of wet dirt as muddy as 60 km",
              Body(fast, 0, kWetMud));
        Model saved;
        saved.SetLayer(0, kWetMud, 0.5f);
        saved.SetReach(0, kWetMud, 0.7f);
        saved.SetReach(0, kDust, 0.7f);
        Check(saved.Layers(0).reach[kWetMud] == 0.7f && saved.Layers(0).reach[kDust] == 0.f,
              "a saved reach is restored (not for the dust)", saved.Layers(0).reach[kWetMud]);
    }
}

// N31: the tread's wear (dirt::Model::TreadWear): mud, then asphalt km by km, the body keeping its dirt.
void Tread() {
    using namespace dirt;
    Model m;
    Check(m.TreadWear(0) == kTreadWearMax && m.TreadWear(1) == kTreadWearMax,
          "N31: a new unit's tread starts worn (clean tyres)", m.TreadWear(0));
    Wait(m, 60, true);
    DriveAt(m, 0, 2 * kDirtKm, "dirt", 50, true);
    Check(m.TreadWear(0) < 0.001f && m.TreadWear(1) < 0.001f, "40 km of wet dirt: the tread as dirty as the wheel",
          m.TreadWear(0));
    Wait(m, 60, false);  // the road dries
    const float mud = Body(m, 0, kWetMud) + Body(m, 0, kDryMud), dust = m.Level(0);
    // D4y: 0.92 * (1 - e^(-1.5 * km)) up to 0.7, then e^(-0.4 * km) of the rest: 0.72, 0.78, 0.83, 0.86.
    const float lo[4] = {0.70f, 0.77f, 0.82f, 0.85f}, hi[4] = {0.73f, 0.79f, 0.84f, 0.87f};
    float last = 0.f;
    for (int km = 1; km <= 4; ++km) {
        DriveAt(m, 0, 1, "road", 80, true);
        const float w = m.TreadWear(0);
        char what[96];
        snprintf(what, sizeof what, "... then %d km of dry asphalt: the tread wears to the floor", km);
        Check(w > last && w >= lo[km - 1] && w <= hi[km - 1] && w <= kTreadWearMax, what, w);
        last = w;
    }
    Check(std::fabs(m.TreadWear(1) - last) < 1e-5f, "... the trailer's tread alike", m.TreadWear(1));
    const float mud_after = Body(m, 0, kWetMud) + Body(m, 0, kDryMud);
    Check(std::fabs(mud_after - mud) < 0.001f && m.Level(0) >= dust && mud > 0.5f,
          "... while the body (and the side walls) keep their mud and dust", mud_after);
    Drive(m, 0, 2, "road", false);
    Check(m.TreadWear(0) == last, "wheels in the air: the tread stays as it is", m.TreadWear(0));
    DriveAt(m, 0, 0.3, "dirt", 50);
    Check(m.TreadWear(0) < 0.15f, "300 m of dirt: the tread takes the wheel's dirt again", m.TreadWear(0));
    const float back = m.TreadWear(0);
    DriveAt(m, 0, 0.2, "static", 50);
    Check(m.TreadWear(0) < back, "static right after dirt counts as dirt for the tread too", m.TreadWear(0));
    DriveAt(m, 0, 1, "static", 50);
    Check(m.TreadWear(0) > 0.4f, "static after 300 m wears it as asphalt", m.TreadWear(0));
    m.Reset(0);
    Check(m.TreadWear(0) == kTreadWearMax, "a cleaned unit's tread starts worn again", m.TreadWear(0));
    Model wet;
    Wait(wet, 60, true);
    DriveAt(wet, 0, 2, "dirt", 50);
    DriveAt(wet, 0, 2, "road", 80);
    Check(wet.TreadWear(0) > 0.75f && Body(wet, 0, kSpray) > 0.f, "wet asphalt wears it too (the spray stays)",
          wet.TreadWear(0));
    Model a, b;
    DriveAt(a, 0, 1, "dirt", 50, false, 0.01);
    DriveAt(b, 0, 1, "dirt", 50, false, 1.0);
    DriveAt(a, 0, 1.5, "road", 80, false, 0.01);
    DriveAt(b, 0, 1.5, "road", 80, false, 0.5);
    Check(std::fabs(a.TreadWear(0) - b.TreadWear(0)) < 0.002f, "small or big steps: the same tread wear",
          a.TreadWear(0) - b.TreadWear(0));
}

}  // namespace

// N12: the wheel constants of the dirt pass (dust::Wheels) and the way the
// shader uses them (a copy of its few lines): unit frame -> tyre frame.
void TyreFrame(const dust::Constants& c, int w, const float rel[3], float out[3]) {
    const float* t = c.turn[w];
    const float rw[3] = {rel[0] * t[0] - rel[2] * t[1], rel[1], rel[0] * t[1] + rel[2] * t[0]};
    out[0] = rw[0], out[1] = rw[1] * t[2] - rw[2] * t[3], out[2] = rw[1] * t[3] + rw[2] * t[2];
}

bool Near3(const float a[3], float x, float y, float z) {
    return std::fabs(a[0] - x) < 1e-4f && std::fabs(a[1] - y) < 1e-4f && std::fabs(a[2] - z) < 1e-4f;
}

void Wheels() {
    dust::Constants c = {};
    dust::Wheels(c, nullptr, nullptr, 0, 0.f);
    bool still = c.spin[0] == 0.f;
    for (const auto& t : c.turn) still = still && t[0] == 1.f && t[1] == 0.f && t[2] == 1.f && t[3] == 0.f;
    Check(still, "wheels: no telemetry, no turn and no blur", c.spin[0]);
    const float steer[2] = {0.25f, 0.f}, roll[2] = {0.f, 0.25f};
    dust::Wheels(c, steer, roll, 2, 10.f);
    float m[3];
    const float left[3] = {-1.f, 0.f, 0.f}, front[3] = {0.f, 0.f, -1.f};
    TyreFrame(c, 0, left, m);
    Check(Near3(m, 0.f, 0.f, -1.f), "wheels: steered 1/4 turn left, its front is at -x", m[2]);
    TyreFrame(c, 1, front, m);
    Check(Near3(m, 0.f, 1.f, 0.f), "wheels: rolled 1/4 turn forward, its top is in front", m[1]);
    TyreFrame(c, 2, front, m);
    Check(Near3(m, 0.f, 0.f, -1.f), "wheels: a wheel past the count stays as it is", m[2]);
    const float slow = c.spin[0];
    dust::Wheels(c, steer, roll, 2, -30.f);
    const float mid = c.spin[0];
    dust::Wheels(c, steer, roll, 2, 50.f);
    Check(slow == 0.f && mid > 0.3f && mid < 0.7f && c.spin[0] == 1.f, "wheels: the angle blurs from 15 to 45 km/h",
          mid);
}

// N13: the levels of the pass (dust::Levels): level 1 draws as 0.8 did, the road wetness and the reach
// are not scaled; and the wheel lines of the shader (a copy) with a wheel raised by its deflection.
void Levels() {
    dust::Layers l = {};
    for (int s = 0; s < dust::kSlots; ++s) l.body[s] = l.front[s] = 1.f, l.reach[s] = 0.7f;
    dust::Constants c = {};
    dust::Levels(c, 1.f, l);
    bool top = c.params[0] == dust::kDustScale;  // D11d: the dust lower (0.68)
    for (int s = 0; s < dust::kSlots; ++s) {
        const float k = s == dust::kWet || s == dust::kTread ? 1.f : 0.8f;
        top = top && c.body[s] == k && c.front[s] == k && c.reach[s] == 0.7f;
    }
    Check(top, "levels: 1 goes to the pass as 0.8, the dust as 0.68 (not the wetness, the tread wear, the reach)",
          c.params[0]);
    dust::Layers odd = {};
    odd.body[dust::kWetMud] = 1.7f, odd.body[dust::kSpray] = -0.2f, odd.body[dust::kSalt] = std::nanf("");
    odd.body[dust::kDryMud] = 0.5f;
    dust::Levels(c, std::nanf(""), odd);
    Check(c.params[0] == 0.f && c.body[dust::kWetMud] == 0.8f && c.body[dust::kSpray] == 0.f && c.body[dust::kSalt] == 0.f &&
              c.body[dust::kDryMud] == 0.4f,
          "levels: clamped to 0..1 first (NaN = 0), then scaled", c.body[dust::kDryMud]);
}

// The shader's wheel lines (no turn): on the tyre (distance from the centre this frame / radius) and the
// arch's distance (from the centre at rest, wheel.w high).
void WheelLines(const float w[4], const float p[3], float* ring, float* arch) {
    const float r = (std::max)(w[3], 0.3f);
    *arch = std::hypot(p[1] - w[3], p[2] - w[2]);
    *ring = std::hypot(p[1] - w[1], p[2] - w[2]) / r;
}

void Deflection() {
    const float rest[4] = {-1.04f, 0.5f, -1.8f, 0.5f}, raised[4] = {-1.04f, 0.56f, -1.8f, 0.5f};  // braking: 6 cm up
    const float tyre[3] = {-1.04f, 0.56f + 0.45f, -1.8f};  // the raised tyre's top, 0.9 radius out
    const float body[3] = {-1.04f, 1.1f, -1.3f};           // the arch above and behind it
    float ring, arch_rest, arch_raised;
    WheelLines(raised, tyre, &ring, &arch_raised);
    Check(std::fabs(ring - 0.9f) < 1e-4f, "deflection: the raised tyre's top is on the wheel (0.9 r)", ring);
    WheelLines(rest, body, &ring, &arch_rest);
    WheelLines(raised, body, &ring, &arch_raised);
    Check(arch_rest == arch_raised, "deflection: the arch keeps its place on the body", arch_raised - arch_rest);
}

int main() {
    {
        dirt::Model m;
        Drive(m, 0, 80, "road");
        // D12c (the user, ATS 30/10): the dust only on dry dirt roads; on asphalt the soot (N39).
        Check(m.Level(0) == 0.f, "80 km of dry asphalt (~1 h): no dust (D12c)", m.Level(0));
    }
    {
        dirt::Model m;
        Drive(m, 0, 5 * kDirtKm, "dirt");
        Check(m.Level(0) > 0.6f && m.Level(0) < 0.8f, "100 km of dry dirt (~6 real min): mostly dusty", m.Level(0));
        Drive(m, 0, 30 * kDirtKm, "dirt");
        Check(m.Level(0) > 0.99f && m.Level(0) <= 1.f, "700 km of dirt: saturates at 1", m.Level(0));
        dirt::Model minute;  // D14
        Drive(minute, 0, 16.6, "road_dirt");
        Check(minute.Level(0) > 0.1f && minute.Level(0) < 0.2f, "D14: a real minute of road_dirt (16.6 km): ~14%",
              minute.Level(0));
    }
    {
        dirt::Model m;
        Drive(m, 0, kDirtKm, "dirt");
        const float a = m.Level(0);
        Drive(m, 0, 0.2, "static");
        const float b = m.Level(0);
        Check(b - a > 0.001f, "static right after dirt counts as dirt", b - a);
        Drive(m, 0, 1, "static");
        const float c = m.Level(0);
        Drive(m, 0, 1, "static");
        Check(m.Level(0) - c < 0.005f, "static after 300 m counts as asphalt", m.Level(0) - c);
    }
    {
        dirt::Model m;
        Drive(m, 0, 5, "dirt", false);
        Check(m.Level(0) == 0.f, "wheels in the air: no dust", m.Level(0));
        Drive(m, 0, 5, "snow");
        Check(m.Level(0) == 0.f, "snow: no dust", m.Level(0));
    }
    {
        dirt::Model m;
        m.Set(0, 0.8f);
        m.Set(1, 0.4f);
        for (int i = 0; i < 120; ++i) m.Weather(1, true);
        Check(m.Level(0) > 0.39f && m.Level(0) < 0.41f, "2 game hours of rain halve the truck's dust", m.Level(0));
        Check(m.Level(1) > 0.19f && m.Level(1) < 0.21f, "... and the trailer's", m.Level(1));
        const float a = m.Level(0);
        m.Weather(30, false);
        Drive(m, 0, 2 * kDirtKm, "dirt");
        Check(m.Level(0) == a, "30 game min after the rain: road wet, no dust", m.Level(0));
        m.Weather(40, false);
        Drive(m, 0, 2 * kDirtKm, "dirt");
        Check(m.Level(0) > a + 0.1f, "70 game min after the rain: dry, dust again", m.Level(0));
    }
    {
        dirt::Model m;
        Drive(m, 1, 5 * kDirtKm, "dirt");
        Check(m.Level(0) == 0.f && m.Level(1) > 0.6f, "units are separate (trailer only)", m.Level(1));
        dirt::Model big;
        big.Configure({1.f, 0.7f});
        Check(big.Level(0) == 0.7f && big.Level(1) == 0.7f, "INI override forces the level", big.Level(0));
        dirt::Model fast;
        fast.Configure({10.f, -1.f});
        Drive(fast, 0, 0.5 * kDirtKm, "dirt");
        Check(fast.Level(0) > 0.65f, "rate x10: 10 km of dirt as dusty as 100 km", fast.Level(0));
        m.Drive(0, 5.0, nullptr, nullptr, 0);
        Check(m.Level(0) == 0.f, "a teleport (> 1 km in one step) is ignored", m.Level(0));
    }
    Layers();
    Tread();
    Wheels();
    Levels();
    Deflection();
    printf(g_failures ? "FAILED\n" : "done\n");
    return g_failures ? 1 : 0;
}
