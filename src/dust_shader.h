// D3 + D4 (N5): the dirt pixel shader (one render target per variant) and its constants.
// Shared by the DLL (tint.cpp) and the offline preview (tools/dustpreview).
//
// Every layer of the unit is drawn by the same draw: each one gets a coverage
// and a colour where it lands, and they are stacked (spray / film at the
// bottom, then salt, grit, dust, mud on top) into one colour and one alpha,
// so the pass still writes one target per draw. The look of each layer
// [estimate: every colour, height and scale below], from the D4 table in
// the development notes:
// - wet mud: dark brown, glossy (specular), clumps and drops in the arches,
//   behind the wheels and low on the body;
// - dry mud: the same clumps (it is the same mud), light brown, matte,
//   cracked into plates; the thick parts stay wet the longest while drying;
// - road spray: dark grey, glossy, a soft film low on the body, behind the
//   wheels and on the rear faces (the wake), streaky along the body;
// - film (dried spray): lighter grey, matte, the same places; it gathers in
//   the vertical rain runs on the side and rear faces;
// - grit: fine light-grey specks (a mean tone where they are under a pixel);
// - salt: a whitish crust low on the body with whiter tide lines; on the
//   wheels a grey, matte powder, thinner on the tread, without tide lines;
// - N39, road soot: dark brown-black with a faint oily sheen, over the dust and
//   under the mud: the wheels (rims and side walls more than the tread), the
//   arches, behind the wheels, low on the body and the chassis, fading up;
//   the rear faces take the wake; smeared along the body;
// - the reach (0..1) of each layer sets how high its splash goes (~0.8 m to
//   ~2.3 m, mud ~2.8 m); the trailer's front face takes the front levels;
// - while the road is wet, the dust on vertical faces is broken into runs.
// N12 (D4b, from the first in-game test of D4a):
// - a wheel's dirt is fixed to the wheel: the pixel goes into the wheel's own
//   frame (its steering, then its rolling turn), so it turns with the wheel
//   and does not slide over the tyre when it steers; at speed the angle
//   fades into rings (spin.x), as a spinning wheel looks;
// - no layer's threshold goes below its value at level 0.9 (it stays > 0):
//   past zero every pixel was covered alike, and level 1 looked like new
//   paint [verified: D4a test, print 3]. From 0.9 to 1 the coat thickens
//   instead, by a varied thickness (more low down and on the edges, vertical
//   runs, thin places that show the paint, darker where thick) and the
//   splash reaches a little higher.
// N13 (D4c, from the D4b test):
// - a wheel's dirt sits where the wheel is this frame (rig::GetWheels: config
//   + suspension deflection), so braking does not drag it along with the
//   chassis; the arches and the fan behind the wheel are the body's, so they
//   stay at the wheel's place at rest (wheel.w, the centre's height at rest);
// - level 1 of any layer looks like level 0.8 did before (kLevelScale, in
//   Levels below): the simulation, the saved state and the test panel keep
//   0..1, only the pass constants are scaled [verified: D4b test, level 1 too
//   dirty, the user asked for today's 0.8]. The heavy coat past 0.9 (N12)
//   stays in the shader but the DLL no longer reaches it.
// - D11d: the dust's own scale is lower (kDustScale: level 1 looks as 0.85
//   did); the soot lays a thin brown film where the dust lies, under its dark
//   grime low down.
// N16 (D4f, from the D4e test): the cab's pass (spin.y = 1) has no wheel
//   pixels: a fender over the tyre keeps the body's dirt, fixed to the cab.
// N17 (D4g, from the D4f test): a pixel is on a wheel up to 1.12 times the
//   centre's height at rest (was 1.05). A loaded tyre stands lower than its
//   radius (V3a truck: centre 0.50 m, a 315/80 R22.5 tyre ~0.54 m), so the
//   tread fell outside, took the body's dirt and slid on it when steering
//   [verified: D4f test print, the tread's top with the body's pattern].
// N19 (D4i, from the D4h test): a pixel is on a wheel from 0.35 m inwards of
//   the wheel's position to 0.55 m outwards (was 0.35 both ways). A trailer's
//   wheel position can be the axle's end, not the tyre's middle: the V3a log
//   trailer's wheels are at x 0.82 and their hub caps at 1.02 (the truck's
//   front hub cap 3 cm out of its wheel's), so the tyre's outer side fell
//   outside, kept the body's dirt and stood still while the wheel turned
//   [verified: D4h test print; V3a captures, the draws at the trailer's
//   wheels]. Nothing of a unit stands outside its wheels within their radius
//   while they point straight; a steered wheel's frame swings the body just
//   behind it into that band (a front corner of the preview's tank at 29
//   degrees), so the band narrows back to 0.35 m between 8 and 16 degrees of
//   steering. The trailers' wheels (most) do not steer.
// N20 (D4j): each wheel's reach outwards is measured in the game (wout: how
//   far out of its position the outermost of its parts on the axle sits,
//   rig::GetWheelReach), and the band goes 0.35 m past it, never narrower
//   than N19's: the 0.2 m of the V3a trailer were one vehicle's, another may
//   set its wheels deeper still (a pair of tyres) [estimativa].
// N22 (D4m, from the ATS test): a pixel is on a wheel up to 1.04 times the
//   tyre's radius as the SDK gives it (wheel.radius, wrad), when given; else
//   1.12 times the centre's height, as N17. The centre's height is the
//   radius of a loaded tyre on the V3a truck (FH front: centre 0.50 m, tyre
//   0.538 m: N17's 1.12) but not in ATS (centre 0.511 m, tyre 0.5065 m): the
//   band reached 6.6 cm past the tyre and took the tight fenders of the
//   American trucks [verified: D4k ATS test print, Western Star 57X; the
//   configs of both games]. The arches, the fan and the pattern keep the
//   centre's height.
// N23 (D4m, from the ATS test): the dust has a film over the whole body
//   (weight 0.32 up to 1 m, 0.18 from 4 m), so from about level 0.6 the
//   upper body takes dust too. Its weight came from the height alone (none
//   above ~2 m on a vertical face): the cab and sleeper sides of a tall
//   American truck stayed clean at every level [verified: D4k ATS test, the
//   57X's cab draws claimed in the captures, print 2 clean above the skirts].
// N25 (D4o, from the ATS test): the paint's reflection over the dust. The
//   truck paint works out its environment reflection in the G-buffer pass and
//   writes it as emittance into rt3 (integers: rgb 5-6-5 in x, an exponent in
//   y), with the reflection strength in z (bits 2-15) [verified: GLSL of
//   eut2.truckpaint's defattr pass, probe/effect]. The lighting adds that
//   emittance over the albedo: on the white, glossy paint of the 57X the sky's
//   reflection hid the dust on the skirts, fenders and cab at any level, while
//   the plain metal steps next to them took it [verified: D4k and D4n ATS
//   prints; every truck draw in view claimed in the captures, none drawn again
//   over it]. A third variant (REFL) scales both by what the dust leaves of
//   them: nothing under dry dust, part under wet mud. It reads a copy of rt3
//   (an integer target cannot be blended) and writes all four channels.
// N31 (the user, 29/10): the tread cleans on paved ground, "not 100% clean,
//   but the tyre's own colour showing again". body1.w is the share of the
//   wheel's dirt the road wore off the tread (dirt::Model::TreadWear, up to
//   0.8 after 1-2 km of asphalt; 0 after a few hundred metres of dirt): on the
//   tread that share of every layer goes and the rest turns to a matte light
//   grey film, a little more of it in the grooves; the side walls keep the
//   wheel's dirt. The wet spray stays on the tread (a wet road throws it back
//   at once); the film it dries into wears as the rest.
// N31, the trailer's sides (the user, ATS 28/10: "almost no dust on the
//   side even at the top level", grain hopper): N23's film gave a side a
//   weight of ~0.2-0.3 everywhere, so the threshold left it clean up to about
//   level 0.6 and covered it evenly, top to bottom, at level 1. A trailer's
//   pass (spin.z) drops N23's film on its vertical faces and lays a film that
//   grows with the level instead (alpha = level x ~0.5 high up .. ~1.4 low
//   down, in broad clouds and hanging streaks, greyer low down: a mid tone
//   that shows on light paint as on dark):
//   seen from level 0.5, heavier low down at every level. The truck and its
//   cab keep N23 as it was.
#pragma once

#include <cmath>

namespace dust {

constexpr int kWheels = 14;

// The D4 layer slots of the constants: dirt::Layer without the dust, then the road wetness and (N31) the
// tread's wear, then (N39) the road soot; three float4 each (the last three slots unused).
enum Slot { kWetMud, kDryMud, kSpray, kFilm, kGrit, kSalt, kWet, kTread, kSoot, kSlots = 12 };

// One unit's layers for the pass (0..1): body and front-face levels, how high
// each body layer reaches; body[kWet] = the road wetness, body[kTread] = the
// share of the wheels' dirt worn off the tread (dirt::Model::TreadWear; 0 =
// as the side walls); front and reach of both unused.
struct Layers {
    float body[kSlots];
    float front[kSlots];
    float reach[kSlots];
};

// The pass constants (Constants below). params: x dust, y 1 = red / 2 = position test / 3 = diagnosis (N26), z wheels,
// w valid.
// N12: spin and turn place each wheel's dirt on the wheel (Wheels below).
// D11m (the user, 30/10: "as light as possible without losing quality"; dustpreview --bench: the dirt took 3.3 ms of
// a frame on the dev PC for the rig seen from outside, as each of the four draws worked it all out again): the PACK
// variant works the dirt out once per pixel (shade) and writes all of it into our own uint4 target (t2 for the
// others); four small variants, one render target each, take it from there: the albedo (no define), the specular
// (SPEC), rt3's emittance and reflection strength (REFL, N25) and rt0's relief (NORM, D11k).
// SnowyMoon stopped the game with "Error (40:860)" when the first version (two
// outputs, a dynamic loop over the wheels) was created [verified: 27/10 test,
// the error came at the first frame of the map, when this shader is made];
// the red one-output shader of V3 never upset it. [estimate] SnowyMoon reads
// every pixel shader the game creates; keep ours as plain as possible.
inline constexpr char kPixelShader[] = R"(
// N62 (the user, 06/11: the glass must be dirty "the same way as the truck -- dust or salt or soot, mud too,
// only denser and not uniform"; the veil was "just opaque"): the GLASS variant runs this very shader on the
// truck's glass, in the game's own forward draw of it. It takes nothing from the G-buffer (the glass is not in
// it): the pixel's depth comes from SV_Position.w, which the rasteriser interpolates exactly, and its normal from
// the derivatives of the position it gives. The dirt that comes out is the same dirt as the body's, with its own
// layers, colours and patterns, fixed to the truck. It writes the game's dual-source blend: the second output
// lets less of the scene through, the first adds the dirt's own colour.
#ifndef GLASS
Texture2D<float4> g_rt0 : register(t0);
#ifdef REFL
Texture2D<uint4> g_rt3 : register(t1);  // N25: a copy of rt3, as the game wrote it
#endif
Texture2D<uint4> g_pack : register(t2);  // D11m: what PACK wrote (the other variants)
#endif
#ifdef GLASS
cbuffer C : register(b13) {  // the game's own slots stay as they are (the plugin saves and restores only b13)
#else
cbuffer C : register(b0) {
#endif
    float4 vp;            // x, y, 1/w, 1/h
    float4 px, py, pw;    // camera -> clip rows x, y, w
    float4 u0, u1, u2;    // camera -> unit rows
    float4 params;
    float4 body0, body1, body2;  // body: wet mud, dry mud, spray, film | grit, salt, road wetness, tread wear (N31) |
                                 // soot (N39), -, -, -
    float4 front0, front1, front2;  // the front face (trailer: the truck's splash), same slots
    float4 reach0, reach1, reach2;  // how high each body layer reaches, 0..1
    float4 wheel[14];       // the unit's wheels: x y z this frame (+ deflection), radius = centre height at rest
    float4 spin;            // x: 0..1, how much a wheel's angle blurs into rings (speed); y: 1 = the cab's pass (N16);
                            // z: 1 = a trailer's pass (N31)
    float4 turn[14];        // per wheel: cos, sin of its steering; cos, sin of its rolling turn
    float4 wout[4];         // N20: per wheel (i / 4, i % 4), how far out of its position its outermost part sits
    float4 wrad[4];         // N22: per wheel (i / 4, i % 4), the tyre's radius (SDK; 0: not given)
    float4 inside_lo;       // N41: the cab's inside (the truck's passes, w 1 = on): a box in the unit's frame, its
    float4 inside_hi;       //   low corner and its high one (m); no dust in it
    float4 cavity_lo;       // D11f: the cab's whole cavity, the sleeper too (w 1 = on): no dust on the faces in it that
    float4 cavity_hi;       //   turn towards its middle
    float4 dash0;           // D11j: the windshield measured (D11i): its front z + 3 cm, the base's height, dz/dy, its top
    float4 dash1;           //   the front zone's half width, its floor, its back (the eye's z), w 1 = on
    float4 glass;           // N62 (GLASS): x how strong the dirt is on the glass, y the daylight, z the test mode
                            //   ([glass] on: 2 magenta, 3 the place, 4 the wiper's arc), w N64: how much the blade
                            //   has taken inside its arc, 0..1
    float4 pane;            // N64b: the windshield measured: x half its width (m), y 1 = measured; N65: z how
                            //   much of the layers a blade cannot touch the jet took inside the arc, 0..1; N64k: w
                            //   how bright the dirt on the glass is in the scene's own units ([glass] brightness)
    float4 blade;           // N64c: the stroke running now: x how much of the dust the blades had taken when it
                            //   started (glass.w is after it), y where the blade is across the sweep, -1..1
                            //   (9 = no stroke: the whole arc shows glass.w), z which way it goes (+1/-1); N67e:
                            //   w 1 = the arm is the one the learner measured, so the rubber's ridge is drawn
    float4 wiper;           // N64 (GLASS): the old guessed fan, kept for the arc's test mode: x the pivots'
                            //   distance from the middle and y their height, both shares of the pane; z the blade's
                            //   length as a share of the pane's height; w half the sweep (radians). x 0 = off
    float4 wblade[6];       // N68 (GLASS): the two blades the game itself describes, three rows each, all in the
                            //   TRUCK's frame: (pivot xyz, where the rubber starts, m from the pivot),
                            //   (the axis it turns about xyz, where the rubber ends), (the arm at rest xyz, unit
                            //   and already in the plane it turns in, how far it swings in radians, signed)
    float4 wnow;            // N68: x, y where each blade is across its own stroke, 0 parked .. 1 far end
                            //   (9 = no stroke running); z how many blades there are (0 = none, use nothing); w -
};
// N64 (the user, 06/11: with the wipers on "deveria dar uma limpada na poeira mas nao tirar o filme nem a
// fuligem"): inside the arc the blades sweep, a DRY blade takes the loose dust and little else. The film and the
// soot are not touched at all (they are greasy: a dry blade smears them), and the washer jet of the next step is
// what takes everything. [estimativa: falta o teste no jogo]
// How much of what the blade took (glass.w, from the model, which already knows a dry blade leaves a haze behind)
// reaches each layer: all of it for the dust and the grit -- the "poeira" of the user's words -- and a fraction of
// it for the two the rubber smears rather than removes.
static const float kWipeDust = 1.00;  // the dust level and the grit
static const float kWipeSalt = 0.35;  // dried salt: a crust the rubber smears more than it removes
static const float kWipeMud = 0.25;   // mud: the blade pushes it aside, it does not wash it

// Lattice hash (D. Hoskins' hash13): no visible grid even far from the origin.
float hash(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}
// N64d: bands along the way a blade travels, for the mud it smears. One value per band, smoothly joined, taken on
// the distance from the pivot: bands of a constant radius are arcs that follow the sweep, which is the way a blade
// drags mud. The pattern does not move -- nothing here remembers where the mud went from one frame to the next --
// so it reads as mud already smeared, not as mud being smeared this instant.
float smear_bands(float r, float seed) {
    const float t = r * 7.0;
    const float i = floor(t), f = frac(t);
    const float s = f * f * (3.0 - 2.0 * f);
    return lerp(hash(float3(i, seed, 3.0)), hash(float3(i + 1.0, seed, 3.0)), s);
}
float vnoise(float3 x) {
    float3 i = floor(x), f = frac(x);
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);  // quintic: no visible cell edges
    return lerp(lerp(lerp(hash(i), hash(i + float3(1, 0, 0)), f.x),
                     lerp(hash(i + float3(0, 1, 0)), hash(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(hash(i + float3(0, 0, 1)), hash(i + float3(1, 0, 1)), f.x),
                     lerp(hash(i + float3(0, 1, 1)), hash(i + float3(1, 1, 1)), f.x), f.y), f.z);
}
// Band-limited noise: an octave finer than ~3 pixels fades to its mean, so
// far or grazing surfaces do not sparkle (fp = metres per pixel here).
// D13f: an octave faded out whole is its mean, without its eight lookups.
float fnoise(float3 q, float freq, float fp) {
    const float k = saturate(1.5 - fp * freq * 3.0);
    return lerp(0.5, vnoise(q * freq), k);
}
// Four octaves from `freq` up (cycles per metre), each turned so the lattices
// of the octaves never line up (no blocky look).
float fbm(float3 q, float freq, float fp) {
    const float3x3 r = float3x3(0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64);
    const float3 q1 = mul(r, q) + 17.3, q2 = mul(r, q1) + 41.7, q3 = mul(r, q2) + 73.1;
    return 0.48 * fnoise(q, freq, fp) + 0.27 * fnoise(q1, freq * 2.13, fp) +
           0.15 * fnoise(q2, freq * 4.71, fp) + 0.10 * fnoise(q3, freq * 9.93, fp);
}
// Fine grain: two turned octaves, a few centimetres, faded with distance.
float grain(float3 q, float fp) {
    const float3x3 r = float3x3(0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64);
    return 0.6 * fnoise(mul(r, q) + 5.5, 45.0, fp) + 0.4 * fnoise(mul(r, mul(r, q)) + 8.8, 97.0, fp);
}
// Cell distances (F1, F2) around x, in cells: the plates of dried mud. 27
// neighbours, unrolled (no dynamic loop: see the SnowyMoon note above).
float2 cells(float3 x) {
    const float3 i = floor(x), f = frac(x);
    float d1 = 9.0, d2 = 9.0;
    [unroll] for (int k = 0; k < 27; ++k) {
        const float3 o = float3(k % 3, (k / 3) % 3, k / 9) - 1.0;
        const float3 c = i + o;
        const float3 r = o + 0.1 + 0.8 * float3(hash(c), hash(c + 17.17), hash(c + 43.43)) - f;
        const float d = dot(r, r);
        d2 = min(d2, max(d1, d));
        d1 = min(d1, d);
    }
    return sqrt(float2(d1, d2));
}
// Cracks between ~4 cm plates, slightly bent, at least ~1 pixel wide, gone
// where a plate is under ~4 pixels.
float cracks(float3 q, float fp) {
    const float freq = 26.0;
    [branch] if (fp * freq * 5.0 >= 1.5) return 0.0;  // D13f: faded out whole
    const float2 d = cells(q * freq + 0.6 * vnoise(q * 6.0));
    const float w = 0.035 + fp * freq * 0.8;
    return (1.0 - smoothstep(w, w * 2.0, d.y - d.x)) * saturate(1.5 - fp * freq * 5.0);
}
// Where the wheels' splash lands: low on the body up to `top` metres (a
// ragged line), in the arches and the fan behind each wheel; faces looking
// down catch more, top faces less.
float splash(float y, float top, float edge, float zone, float fan, float under, float up) {
    const float low = smoothstep(top, top * 0.3, y + (edge - 0.5) * 0.5 * top);
    return saturate((max(low * 0.85, max(zone, fan * 0.9)) + under * 0.35) * (1.0 - 0.45 * up));
}
// N58: the mud's: in the arches, behind the wheels and in the rays off them, the low band along the skirts thin (~40%
// of the other layers'); faces looking down catch more near the wheels, top faces less.
float mud_splash(float y, float top, float edge, float zone, float fan, float rays, float under, float up) {
    const float low = smoothstep(top, top * 0.3, y + (edge - 0.5) * 0.5 * top);
    const float near = max(zone * 0.95, max(fan * 0.75, rays));
    return saturate((max(low * 0.35, near) + under * 0.3 * max(near, low * 0.5)) * (1.0 - 0.45 * up));
}
// Body or front face, by which one puts more of the layer here: weight, level, share of the front.
float3 pick(float wb, float lb, float wf, float lf) {
    const float k = wf * lf / max(wb * lb + wf * lf, 1e-5);
    return float3(lerp(wb, wf, k), lerp(lb, lf, k), k);
}
// N12: a layer's threshold, never below its value at level 0.9 (> 0), and
// how far the level is past 0.9 (0..1), where the coat thickens instead.
float thr(float level) { return 1.05 - 1.1 * min(level, 0.9); }
// D13f: a layer's level under this shows under ~0.1% of coverage (its D11b fade): not worked out.
static const float kSeen = 0.004;
float high(float level) { return saturate(level * 10.0 - 9.0); }
// D14 (the user, ATS 31/10: "a very big gap between the phases... dirty the truck little by little"): a film's
// coverage, level x weight thickening towards 1 (1 - e^-x), a little uneven with the pattern (+-10%). Low down (weight
// ~1) ~20% at level 0.1, ~50% at 0.3, ~75% at 0.6; top faces (~0.6) 13%, 34%, 56%; high on the sides (~0.2) 5, 15, 27%.
float film(float level, float weight, float shape) {
    return 1.0 - exp(-1.3 * level * (0.1 + 1.6 * weight) * (0.9 + 0.2 * shape));
}
// One layer over the stack: premultiplied colour + coverage, specular + coverage.
void over(inout float4 acc, inout float2 sacc, float3 c, float a, float s, float sa) {
    acc = float4(c * a + acc.rgb * (1.0 - a), a + acc.a * (1.0 - a));
    sacc = float2(s * sa + sacc.x * (1.0 - sa), sa + sacc.y * (1.0 - sa));
}
// N31: one layer over the stack where the road wore the tread: `keep` of its coverage, its colour and
// specular `grey` of the way to a matte film the colour of the road.
// N71 (the user, ATS 07/11): that film was a *light* grey (0.36), so a worn tread came out lighter than the
// dusty side walls -- the other way round from a real tyre, whose contact patch is the darkest part of it.
// Asphalt leaves a dark grey.
void over_worn(inout float4 acc, inout float2 sacc, float3 c, float a, float s, float sa, float keep, float grey) {
    over(acc, sacc, lerp(c, float3(0.055, 0.052, 0.048), grey), a * keep, lerp(s, 0.01, grey), sa * keep);
}
)"  // four literals: MSVC 2022 caps one at 16380 characters (C2026)
R"(
// D11k (N44, the user 30/10: "up close, on foot, grains, drops of oil and single bits of dirt"):
// one point per cell of `cell` metres at a random spot, a disc of random size (never past its cell) where the cell's
// hash is under `density`. x: coverage (soft over a pixel), y: the point's own random value. `dir`: the offset from
// the point, 0 at its middle and 1 at its rim (a dome's slope, for the relief). Gone when a disc is under ~1/3 pixel.
float2 specks(float3 q, float cell, float density, float fp, out float3 dir) {
    dir = 0;
    const float3 x = q / cell, i = floor(x);
    if (hash(i + 3.7) > density) return 0;
    const float3 c = 0.3 + 0.4 * float3(hash(i + 11.1), hash(i + 23.3), hash(i + 37.7));
    const float r = 0.09 + 0.16 * hash(i + 51.5);  // in cells
    const float3 o = x - i - c;
    const float d = length(o) * (0.8 + 0.4 * vnoise(x * 2.7 + 5.0));  // not round: a grain, a bit of grit
    const float px = fp / cell;  // a pixel, in cells
    const float cov = saturate((r - d) / max(px, 1e-4) + 0.5) * saturate(1.5 - px / (r * 2.0));
    dir = o / r * cov;
    return float2(cov, hash(i + 61.3));
}
// D11k: the relief inside the mud and the heavy coat: clumps of ~2.5 cm, ~1 cm and ~5 mm (0..1, three octaves turned as
// fbm's: no squares), each octave gone where it is under ~3 pixels; its slope (per metre, in the unit's frame).
// D11m: the slope worked out from the noise's own derivative, one lookup per octave (central differences took six:
// ~2 ms of a frame up close at 3440x2880; the same relief within ~5%).
float3 vnoise_grad(float3 x) {
    const float3 i = floor(x), f = frac(x);
    const float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0), du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    const float a = hash(i), b = hash(i + float3(1, 0, 0)), c = hash(i + float3(0, 1, 0)), d = hash(i + float3(1, 1, 0));
    const float e = hash(i + float3(0, 0, 1)), g = hash(i + float3(1, 0, 1)), h = hash(i + float3(0, 1, 1));
    const float k = hash(i + float3(1, 1, 1));
    const float k1 = b - a, k2 = c - a, k3 = e - a, k4 = a - b - c + d, k5 = a - c - e + h, k6 = a - b - e + g;
    const float k7 = -a + b + c - d + e - g - h + k;
    return du * float3(k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z, k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
                       k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y);
}
// N59 (the user, ATS 05/11: the mud "lacks volume"): the cakes of mud, far coarser than D11k's clumps (~14 cm and
// ~6 cm, where those are 2.5 cm down), so they still read from several metres away: their height (0..1) and their
// slope (per metre). One value plus one gradient lookup per octave.
float4 cake_field(float3 q, float fp) {
    const float3x3 r = float3x3(0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64);
    const float3 q0 = mul(r, q) + 5.9, q1 = mul(r, q0) + 2.7;
    const float f0 = saturate(1.5 - fp * 21.0), f1 = saturate(1.5 - fp * 48.0);  // gone under ~3 pixels, as fnoise's
    const float h = 0.62 * lerp(0.5, vnoise(q0 * 7.0), f0) + 0.38 * lerp(0.5, vnoise(q1 * 16.0), f1);
    const float3 g0 = vnoise_grad(q0 * 7.0) * (0.62 * 7.0 * f0);
    const float3 g1 = vnoise_grad(q1 * 16.0) * (0.38 * 16.0 * f1);
    return float4(mul(mul(g1, r) + g0, r), h);
}
float3 relief_grad(float3 q, float fp) {
    const float3x3 r = float3x3(0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64);
    const float3 q0 = mul(r, q) + 1.3, q1 = mul(r, q0) + 9.1, q2 = mul(r, q1) + 3.7;
    // Each octave's slope times its weight, frequency and fade (fnoise's), back through its turns (mul(g, r) = r^T g).
    const float3 g0 = vnoise_grad(q0 * 40.0) * (0.5 * 40.0 * saturate(1.5 - fp * 120.0));
    const float3 g1 = vnoise_grad(q1 * 95.0) * (0.33 * 95.0 * saturate(1.5 - fp * 285.0));
    const float3 g2 = vnoise_grad(q2 * 210.0) * (0.17 * 210.0 * saturate(1.5 - fp * 630.0));
    return mul(mul(mul(g2, r) + g1, r) + g0, r);
}
// D11k: what shade() leaves for the NORM variant (rt0's relief): the pixel in the unit's frame, its normal and
// footprint, the relief's height (m) and the specks' slope.
static float3 s_p = 0, s_n = float3(0, 1, 0);
static float4 s_g = 0;  // D13f: rt0 at the pixel (the game's normal and depth), for PACK's tilted normal
static float s_fp = 1.0, s_amp = 0.0;
static float3 s_bump = 0;
// D11m: true once shade() got to the end (the dirt's own specular is in s_spec: its strength, its coverage).
static bool s_end = false;
static float2 s_spec = 0;
// The dust over this pixel: colour + coverage, as the albedo takes it; the rest in the statics above.
float4 shade(float4 pos) {
    const float4 none = float4(0, 0, 0, 0);  // alpha 0: nothing changes, the stencil bit is still cleared
    if (params.y > 0.5 && params.y < 1.5) return float4(0.85, 0.03, 0.03, 1);  // the V3 red test (the albedo only)
    // N26, diagnosis: the albedo says what the pass made of each marked pixel (green as bright as the dust's
    // coverage; black: no coverage; grey: every level 0; red: rt0's normal not a unit vector; blue: no depth;
    // magenta: no position; yellow: no frame of the unit) and rt3's emittance goes, so the reflection cannot hide it.
    const bool diag = params.y > 2.5;
#ifdef GLASS
    // N62: no G-buffer here, so the depth comes from the rasteriser's own interpolation of SV_Position, which is
    // exact and smooth (the half float in rt0 could not even be differentiated: D3c). Its w is the clip w
    // [verified: N62b, tools/probetest with [glass] on = 3: taking it for 1/w placed every pixel 6 cm from the
    // camera and the dirt came out flat, which is what the user saw]. The camera depth follows from the
    // projection's own w row rather than from any assumption about it; should a driver hand us 1/w after all, the
    // depth comes out absurd and the reciprocal is taken instead.
    float wc = abs(pos.w);
    float zc = (abs(pw.z) > 1e-6) ? (wc - pw.w) / pw.z : -wc;
    if (!(zc < -0.1)) {
        wc = 1.0 / max(wc, 1e-6);
        zc = (abs(pw.z) > 1e-6) ? (wc - pw.w) / pw.z : -wc;
    }
    const float4 g = float4(0, 0, 0, zc);
    const float z = min(g.w, -0.05);
#else
    const float4 g = g_rt0.Load(int3(pos.xy, 0));
    const float z = min(g.w, -0.05);  // camera space: in front of the camera z < 0
#endif
    // Camera-space position: the pixel's NDC and the projection, solved for x and y at depth z.
    const float2 ndc = float2((pos.x - vp.x) * vp.z * 2.0 - 1.0, 1.0 - (pos.y - vp.y) * vp.w * 2.0);
    const float wz = pw.z * z + pw.w;
    const float a00 = px.x - ndc.x * pw.x, a01 = px.y - ndc.x * pw.y, b0 = ndc.x * wz - (px.z * z + px.w);
    const float a10 = py.x - ndc.y * pw.x, a11 = py.y - ndc.y * pw.y, b1 = ndc.y * wz - (py.z * z + py.w);
    float det = a00 * a11 - a01 * a10;
    const bool solvable = abs(det) > 1e-8;
    det = solvable ? det : 1.0;
    const float3 pe = float3((b0 * a11 - a01 * b1) / det, (a00 * b1 - b0 * a10) / det, z);
    // Into the unit: y up from the ground, +z = rear, wheels at their config positions.
    const float3 p = float3(dot(u0.xyz, pe) + u0.w, dot(u1.xyz, pe) + u1.w, dot(u2.xyz, pe) + u2.w);
#ifdef GLASS
    // N62: the pane's own normal, from how the position changes across the pixel; turned to face the camera.
    float3 gn = normalize(cross(ddx(pe), ddy(pe)) + 1e-6);
    gn = dot(gn, pe) > 0.0 ? -gn : gn;
    const float3 n = normalize(float3(dot(u0.xyz, gn), dot(u1.xyz, gn), dot(u2.xyz, gn)) + 1e-6);
#else
    const float3 n = normalize(float3(dot(u0.xyz, g.xyz), dot(u1.xyz, g.xyz), dot(u2.xyz, g.xyz)) + 1e-6);
#endif
    // Pixel footprint (m): one pixel's height at this depth, stretched where the
    // surface is seen at a grazing angle. From the projection, not from ddx/ddy:
    // the depth in rt0 is a half float, and its steps made rings ("ondulações")
    // where the derivatives were used [verified: D3c test, roof at mid distance].
    const float3 view_dir = normalize(pe);
#ifdef GLASS
    const float facing = max(abs(dot(n, view_dir)), 0.25);  // N62: our own normal (the unit's frame is a rotation)
#else
    const float facing = max(abs(dot(normalize(g.xyz), view_dir)), 0.25);
#endif
    const float fp = 2.0 * abs(z) * vp.w / max(abs(py.y), 1e-4) / facing;
    // The normal-mapped normal only nudges the face weights (no hard thresholds on it).
    const float3 ns = n;
    s_p = p, s_n = n, s_fp = fp, s_g = g;  // D11k

)"  // eight literals: MSVC 2022 caps one at 16380 characters (C2026)
R"(
    // N64: how much the blades have taken at this pixel. The pane comes from the windshield the plugin measures in
    // the game (D11i/D11j: dash0 = its front z, the base's height, dz/dy, the top; dash1.x = its half width), so no
    // per-truck data is needed yet. Only the windshield is wiped: a pixel has to sit on its slanted plane, which
    // leaves the side and rear windows out. Inside the pane each blade sweeps a sector of a disc around its pivot.
    float wiped = 0.0;   // N64: of the dust, here
    float jetted = 0.0;  // N65: of everything else, here
    float smear = 1.0;   // N64d: what the blade did to the mud it could not take
    float ridge = 0.0;   // N67e: the dirt the rubber is pushing along in front of itself, right now
    float3 ring_dbg = 0.0;  // N69e ([glass] on = 6): the first blade's radius, place across the stroke, coverage
    float2 pane_q = 0.0;      // N64i: where this pixel is on the pane: across (m), above its base (m)
    float pane_h = 1.0;       // its height (m)
    bool on_pane = false;     // and whether this pixel is on the windshield at all
#ifdef GLASS
    [branch] if (pane.y > 0.5 && abs(p.x) < pane.x) {
        const float H = max(dash0.w - dash0.y, 0.1);              // the pane's height (m)
        const float plane = dash0.x + max(p.y - dash0.y, 0.0) * dash0.z;  // the windshield's own plane at this height
        const float2 q = float2(p.x, p.y - dash0.y);              // on the pane: across, above its base
        // N69c: and it has to FACE the way the truck is going. A door window is tall and sits at nearly the
        // windscreen's own half width, so a slab of it near the A-pillar falls inside any box drawn around the
        // windscreen's plane -- which is the square the user saw. What a pane faces cannot be confused.
        pane_q = q, pane_h = H;
        on_pane = abs(p.z - plane) < 0.18 && q.y > -0.15 && q.y < H + 0.15 && abs(ns.z) > 0.55;
        [branch] if (on_pane) {
            float bands = 0.0;
            // N68e: one pass per blade, each solved in the TRUCK's frame around its own axis -- the game's own
            // pivot, arm and sweep (tools/premap reads them from the cabin's wiper model and animation).
            [loop] for (int bi = 0; bi < 2; ++bi) {
                [branch] if (float(bi) >= wnow.z) break;
                const float4 g0 = wblade[bi * 3 + 0];   // the pivot, and where the rubber starts
                const float4 g1 = wblade[bi * 3 + 1];   // the axis it turns about, and where the rubber ends
                const float4 g2 = wblade[bi * 3 + 2];   // the arm at rest, and how far it swings (signed)
                float3 d = p - g0.xyz;
                d -= g1.xyz * dot(d, g1.xyz);           // onto the plane the arm turns in
                const float r = length(d);
                [branch] if (r < 1e-3 || g2.w == 0.0) continue;
                const float3 u = d / r;
                // Where this pixel sits across the stroke: 0 parked, 1 at the far end.
                const float ang = atan2(dot(g1.xyz, cross(g2.xyz, u)), dot(g2.xyz, u));
                const float t = ang / g2.w;
                // The rubber is a segment of the arm, so what it sweeps is a RING, not a disc.
                const float soft = 0.05;                 // m, the soft edge of the rubber's ends
                const float across = 0.04;               // of the stroke, the soft edge at each end
                const float cov = smoothstep(g0.w - soft, g0.w + soft, r) * smoothstep(g1.w + soft, g1.w - soft, r) *
                                  smoothstep(-across, across, t) * smoothstep(1.0 + across, 1.0 - across, t);
                if (bi == 0) ring_dbg = float3(r * 0.5, t, cov);  // N69e
                [branch] if (cov <= 0.0) continue;
                // N64c: while a stroke is running, a pixel the blade has already crossed shows what the blades
                // have taken and one it has not yet shows what they had taken before this stroke, so the clearing
                // travels across the pane with the blade instead of appearing all at once.
                float took = glass.w;
                const float at = bi > 0 ? wnow.y : wnow.x;   // where this blade is now, across its own stroke
                [branch] if (at < 8.0) {
                    const float edge = 0.05;
                    const float behind = blade.z > 0.0 ? smoothstep(at + edge, at - edge, t)
                                                       : smoothstep(at - edge, at + edge, t);
                    took = lerp(blade.x, glass.w, behind);
                    // N67e: the rubber does not make dirt vanish, it pushes it along; the ridge it carries is
                    // what the eye actually follows across the glass.
                    const float ahead = (t - at) * blade.z;   // > 0: the blade has not come this far yet
                    const float wide = 0.05;
                    ridge = max(ridge, cov * exp(-ahead * ahead / (wide * wide)) * saturate(glass.w) *
                                       step(-0.25 * wide, ahead));
                }
                wiped = max(wiped, cov * took);
                jetted = max(jetted, cov * saturate(pane.z));
                bands = max(bands, cov * smear_bands(r, bi > 0 ? 11.0 : 5.0));
            }
            // N64d: the mud the blade could not lift is pushed into bands along its way: less of it where the
            // rubber scraped, more where it piled up. The blade's reach is what decides how marked this is.
            smear = 1.0 + (bands * 2.0 - 1.0) * 0.55 * saturate(max(wiped, jetted) * 1.5);
        }
    }
    if (glass.z > 3.5 && glass.z < 4.5)  // [glass] on = 4: the arc, red for the blades and blue for the jet
        return float4(1.0 - jetted, 0.1, jetted, max(wiped, jetted));
    // N69f ([glass] on = 6): what the shader RECEIVED for the first blade, flat over the whole pane -- red
    // where the rubber starts (m), green where it ends (halved), blue how far the arm swings (rad, halved).
    // One pixel gives three numbers, and against the ones the log prints they either match or they do not.
    if (glass.z > 5.5)
        return float4(saturate(wblade[0].w), saturate(wblade[1].w * 0.5), saturate(wblade[2].w * 0.5), 1.0);
#endif

    const float dust = params.x * (1.0 - saturate(kWipeDust * wiped + jetted));
#ifdef GLASS
    if (params.w < 0.5 || !solvable) return none;  // N62: no normal or depth of the game's to check
#else
    if (params.w < 0.5 || !solvable || !(g.w < -0.05) || dot(g.xyz, g.xyz) < 0.25) {
        if (diag)
            return float4(params.w < 0.5 ? float3(1, 1, 0) : !solvable ? float3(1, 0, 1) : !(g.w < -0.05) ? float3(0, 0, 1)
                                                                                                   : float3(1, 0, 0), 1);
        return none;
    }
#endif
    if (params.y > 1.5 && params.y < 2.5) return float4(p, 1);  // position test: the pixel's position in the unit
    // N41 (the user, 29/10: the dust got inside the cab once N30's redraw of the glass was gone): on the truck's
    // passes a pixel inside a box around the driver's seat (the seats, the dashboard, the bunk; the cab's shell lies
    // outside it) gets no dust; cyan in the diagnosis. Nothing in the game's forward pass.
#ifndef GLASS  // N62: the glass is the cab's own window; these zones keep the dust off what is behind it
    if (inside_lo.w > 0.5 && all(p > inside_lo.xyz) && all(p < inside_hi.xyz))
        return diag ? float4(0, 1, 1, 1) : none;
    // D11f (the user, 29/10: still dust in the cab, the sleeper's back in the print): the cavity, a larger box up to
    // the shell, the sleeper too. Seen from inside, every face of it turns towards its middle (walls, ceiling, bunks,
    // the dashboard); the shell's outer faces turn away, so they keep their dust even inside the box.
    if (cavity_lo.w > 0.5 && all(p > cavity_lo.xyz) && all(p < cavity_hi.xyz) &&
        dot(n, normalize(0.5 * (cavity_lo.xyz + cavity_hi.xyz) - p)) > 0.15)
        return diag ? float4(0, 0.6, 1, 1) : none;
)"  // eight literals: MSVC 2022 caps one at 16380 characters (C2026)
R"(
    // D11j (the user, 30/10: the dashboard's top still dusty): it slopes down to the glass, facing up and ahead, away
    // from the cavity's middle. Behind the windshield's slanted plane (vertical below its base), ahead of the eye: the
    // cab's front, no dust whichever way a face turns; the hood and the wipers lie ahead of the plane.
    if (dash1.w > 0.5 && abs(p.x) < dash1.x && p.y > dash1.y && p.y < dash0.w && p.z < dash1.z &&
        p.z > dash0.x + max(p.y - dash0.y, 0.0) * dash0.z)
        return diag ? float4(0.3, 0.6, 1, 1) : none;
#endif
    if (dust <= 0.0 && dot(body0 + front0, float4(1, 1, 1, 1)) + body1.x + body1.y + front1.x + front1.y + body2.x +
                           front2.x <= 0.0)
        return diag ? float4(0.35, 0.35, 0.35, 1) : none;

    float zone = 0.0;  // wheel arches
    float fan = 0.0;   // the spray behind each wheel
    float ring = -1.0; // on a wheel: distance from the axle / radius
    float3 wm = 0;     // on a wheel: the pixel in the tyre's own frame (turns with it), in radii
    float wr = 1.0;    // on a wheel: its radius
    float wid = 0.0;   // on a wheel: its index (each wheel its own pattern)
    float2 wst = float2(1, 0), wrt = float2(1, 0);  // N72: its steering and its rolling turn, to undo them
    float tread = 0.0; // on a wheel: on the tread (facing out), not on a side wall
    const int wheels = (int)params.z;
    // N58: the mud's rays off the wheel that throws the most here (its centre at rest, this pixel from it).
    float ray_w = 0.0, ray_z = 0.0, ray_y = -1.0, ray_d = 1.0, ray_id = 0.0;
    const float mud_reach = (body0.x * reach0.x + body0.y * reach0.y) / max(body0.x + body0.y, 1e-4);
    float wz0 = 1e9, wz1 = -1e9;  // N74: the first and the last axle along the unit
    [unroll] for (int i = 0; i < 14; ++i) {
        if (i >= wheels) continue;
        const float4 w = wheel[i];
        const float r = max(w.w, 0.3);
        wz0 = min(wz0, w.z), wz1 = max(wz1, w.z);
        // The arches and the fan are the body's: around the wheel at rest (its centre at height w.w).
        const float d = length(float2(p.y - w.w, p.z - w.z));
        const float lateral = abs(abs(p.x) - abs(w.x));
        // Into the wheel's frame: its steering undone (a turn about y), then
        // its rolling turn (about x). Zeros (no turn given) mean none.
        const float4 tt = turn[i];
        const float2 st = dot(tt.xy, tt.xy) > 0.5 ? tt.xy : float2(1, 0);
        const float2 rt = dot(tt.zw, tt.zw) > 0.5 ? tt.zw : float2(1, 0);
        const float3 rel = p - w.xyz;
        const float3 rw = float3(rel.x * st.x - rel.z * st.y, rel.y, rel.x * st.y + rel.z * st.x);
        const float dw = length(rw.yz);
        // N19: across the wheel, + away from the unit's middle; outwards up to 0.55 m while it points straight.
        // N20: and 0.35 m past its outermost part on the axle, as measured.
        const float across = w.x < 0.0 ? -rw.x : rw.x;
        const float outer = 0.35 + max(wout[i >> 2][i & 3], 0.2 * smoothstep(0.961, 0.990, st.x));
        const float tyre = wrad[i >> 2][i & 3];
        const float edge = tyre > 0.1 ? tyre * 1.04 : r * 1.12;  // N22: the SDK's tyre radius; else N17
        // N58 (the user, ATS 1.62 04/11: "a black stain at the bottom of the tyre"): where the tyre squats on the road
        // its bottom reached past the edge and took the body's lowest dirt (the near black oily soot); straight down
        // from the axle the edge goes up to 12% further (below it there is only the road, never one of our pixels).
        const float down = smoothstep(0.3, 0.8, -rw.y / max(dw, 1e-3));
        // N73 (the user, ATS 07/11, with a print: "there is a black stain at the bottom of the tyre... it looks
        // like a shadow in the wrong place"). The same stain the N58 met on 04/11, and widening the edge by 12%
        // straight down was not enough. What it is: a pixel this test loses is the *body's*, and the body's dirt
        // is laid by height (`low`, near black at the ground, over a dithered threshold), so what shows is a flat
        // line across the tyre with a speckled edge -- measured on the print at ~10 cm over the road, flat across
        // 65 cm of tyre, which no circle about an axle can draw. Under the axle, inside the wheel's own window
        // across and within a tyre's radius along the unit, there is nothing of ours but the tyre: the mudguard
        // is above the axle (and its trailing edge well over this height), the tanks and the steps are further
        // back than a radius. So the lowest 0.45 r is the tyre's whatever the radius we were given says, and the
        // bottom of a tyre can no longer fall to the body. The cap keeps the mudguard out of it.
        const bool bottom = rw.y < 0.0 && abs(rw.z) < edge && p.y < 0.45 * r;
        if (spin.y < 0.5 && (dw < edge * (1.0 + 0.12 * down) || bottom) && across > -0.35 &&
            across < outer) {  // N16: no cab pixel
            ring = dw / r;
            wr = r;
            wid = i;
            wm = float3(rw.x, rw.y * rt.x - rw.z * rt.y, rw.y * rt.y + rw.z * rt.x) / r;
            wst = st, wrt = rt;  // N72
            tread = 1.0 - smoothstep(0.3, 0.7, abs(ns.x * st.x - ns.z * st.y));
        }
        zone = max(zone, smoothstep(1.75, 1.0, d / r) * smoothstep(0.9, 0.3, lateral));
        // Behind the wheel (+z), below ~1.4 m, fading over ~2 m.
        const float behind = smoothstep(-0.2, 0.6, (p.z - w.z) / r);
        fan = max(fan, smoothstep(2.4, 0.9, d) * behind * smoothstep(1.6, 0.6, p.y));
        // N58 (the user, 04/11: the mud "focused on the wheel arch and near the wheels, spreading in lines radial to
        // the wheel"): the mud a rolling tyre throws, in rays from the axle over the faces beside it, from the tyre's
        // edge out to ~2-3.4 radii (with the mud's reach), more towards the rear and upwards (the tread leaves the
        // road going back, the top of the tyre runs forward into the arch).
        const float rr = d / r;
        const float2 dir = float2(p.z - w.z, p.y - w.w) / max(d, 1e-3);
        const float rays_i = smoothstep(2.0 + 1.4 * mud_reach, 1.1, rr) * smoothstep(0.9, 1.08, rr) *
                             smoothstep(0.9, 0.3, lateral) * (0.35 + 0.65 * saturate(0.5 + 0.6 * dot(dir, float2(0.8, 0.6))));
        if (rays_i > ray_w) ray_w = rays_i, ray_z = p.z - w.z, ray_y = p.y - w.w, ray_d = d, ray_id = i;
    }
    // Noise coordinates: the unit's frame on the body; on a wheel the tyre's
    // own frame (x across, y z in its plane, in radii), so the dirt turns with
    // the wheel; at speed its angle fades into rings around the axle.
    const bool on_wheel = ring >= 0.0;
    // N58: the rays, 0..1: thin lines around the axle (their angle; the seam straight down, on the road) that break
    // along their length; two widths, the pixel's size in radians keeping them from flickering far away.
    float rays = 0.0;
    [branch] if (ray_w > 0.0 && !on_wheel && max(body0.x + body0.y, front0.x + front0.y) > kSeen) {
        const float ang = atan2(ray_z, ray_y);
        const float fa = fp / max(ray_d, 0.2);
        const float l1 = fnoise(float3(ang * 7.0, ray_d * 1.3, ray_id * 5.7), 1.0, fa * 7.0);
        const float l2 = fnoise(float3(ang * 19.0, ray_d * 2.9, ray_id * 3.3 + 11.0), 1.0, fa * 19.0);
        rays = ray_w * saturate(smoothstep(0.52, 0.72, l1) + 0.7 * smoothstep(0.6, 0.78, l2));
    }
    const float blur = saturate(spin.x);
    const float3 sq = on_wheel ? float3(wm.x * 1.6 + wid * 7.31, lerp(wm.y, ring, blur) * 1.2, wm.z * (1.0 - blur) * 1.2 + wid * 3.17) : p;
    const float fq = on_wheel ? fp * 1.4 / wr : fp;
    // N58b (the user, ATS 05/11: the tyre's mud "rounded" patches; photos of muddy truck tyres: a coat thickest at the
    // shoulder, thinner towards the rim, its inner edge in radial fingers, more at the rim's lip, never round blots):
    // a noise around the axle in the tyre's frame (on a circle: no seam), slow along the radius, so it runs in radial
    // streaks that turn with the wheel; at speed they blur into rings, as the rest of the wheel's pattern.
    float tyre_streak = 0.5;
    [branch] if (on_wheel) {
        const float2 dir = wm.yz / max(length(wm.yz), 1e-3);
        const float fa = fq / max(ring, 0.2) + blur * 0.5;  // a pixel around the axle, in radians
        const float s1 = fnoise(float3(dir * 4.0, ring * 1.6 + wid * 3.1), 1.0, fa * 4.0);
        const float s2 = fnoise(float3(dir * 16.0 + 5.3, ring * 2.4 + wid * 1.7), 1.0, fa * 16.0);
        tyre_streak = 0.45 * s1 + 0.55 * s2;
    }
)"  // two literals: MSVC 2022 caps one at 16380 characters (C2026)
R"(
    const float side_film = on_wheel ? 0.0 : saturate(spin.z);  // N31: a trailer's body
    // N74 (the user, 12/11: "as realistic as possible... take the physics of the movement into account for where
    // the dirt goes"). What a moving rig's own wheels raise only ever travels BACK: the air carries it rearwards and
    // the layer of slow, dirty air along the sides thickens from the nose to the tail, where the wake behind the
    // unit holds it against the last metres. So (a) every layer the wheels raise grows along the unit (`aft`, 0 at
    // the nose, 1 at the tail; a trailer's nose is ~1 m ahead of its kingpin, a truck's 1.4 m ahead of its first
    // axle), and (b) ahead of the truck's first axle nothing of its own arrives (`ahead`): the bumper and the grille
    // keep only what the traffic in front and the still air leave -- the arch itself is the wheel's (zone).
    float aft = 0.5, ahead = 0.0;
    [branch] if (wheels > 0) {
        const float zf = spin.z > 0.5 ? min(wz0 - 1.4, -1.0) : wz0 - 1.4;
        aft = saturate((p.z - zf) / max(wz1 + 1.2 - zf, 1.0));
        ahead = spin.z > 0.5 ? 0.0 : smoothstep(wz0 - 0.35, wz0 - 1.1, p.z) * (1.0 - zone);
    }
    const float along = spin.z > 0.5 ? lerp(0.62, 1.22, aft) : lerp(0.86, 1.10, aft);
    float sweep = 0.5, swept = 0.0;  // N74: the streaks the airflow draws along the sides (below)
    float weight, shape, detail;
    if (on_wheel) {
        // The wheel: in the tyre's frame, the tread taking the most. N58b: half of it in radial streaks.
        shape = lerp(fbm(sq, 3.0, fq), tyre_streak, 0.3);
        detail = grain(sq, fq);
        weight = 0.55 + 0.45 * smoothstep(0.5, 0.97, ring);
    } else {
        // Body: large patches fixed to the unit, slightly stretched downwards.
        shape = fbm(p * float3(1.0, 0.65, 1.0), 1.4, fp);
        detail = grain(p, fp);
        const float vertical = 1.0 - smoothstep(0.35, 0.6, abs(ns.y));
        // N74: low on the sides nothing settles in round patches at speed: what the wheels throw is drawn out
        // along the way the air runs, in long streaks that climb a little towards the tail (the plume off a wheel
        // rises as it is carried back). A noise ~2 m long and ~25 cm tall, sheared so its lines rise rearwards;
        // the dust's patches, the soot's coat and the mud's clumps all take it (the same air carries them).
        swept = vertical * smoothstep(2.4, 0.7, p.y) * 0.6;
        [branch] if (swept > 0.0) {
            const float3 sw = float3(p.x * 0.7, p.y * 2.4 - p.z * 0.3, p.z * 0.36) + 57.3;  // two octaves: half fbm's cost
            sweep = 0.62 * fnoise(sw, 1.4, fp * 2.4) + 0.38 * fnoise(sw.zxy * float3(0.4, 1.0, 2.5) + 23.9, 3.1, fp * 2.4);
            shape = lerp(shape, sweep, swept);
        }
        const float low = pow(smoothstep(2.3, 0.15, p.y), 1.3) * along * (1.0 - 0.55 * ahead);
        // N23: a film of dust over the whole body, thinner high up: only the high levels reach it. N31: not on a
        // trailer's sides, which take the side film below instead.
        const float film = (0.18 + 0.14 * smoothstep(4.0, 1.0, p.y)) * (1.0 - side_film * vertical);
        weight = max(max(low * 0.9, film), max(zone, fan * 0.8));
        // N33 (the user, 29/10: the trailer's side must follow the rest of the rig's dirt, not only near the top
        // level): a trailer's side weighs like the truck's body at that height, heavier low down, so its patches
        // come at the same levels as the truck's skirts and cab.
        weight = max(weight, side_film * vertical * (0.30 + 0.48 * smoothstep(3.8, 1.0, p.y)) * along);  // N74
        // Faces: dust settles on top faces and in the wake (rear faces).
        weight += smoothstep(0.35, 0.97, ns.y) * (0.30 + 0.25 * shape);
        weight += smoothstep(0.2, 0.9, ns.z) * 0.30;
        // Runs: on vertical faces, thin streaks that hang down from the dust above.
        float streak = 0.0;  // D13f: only where it adds (upright faces below 3.2 m)
        [branch] if (vertical > 0.0 && p.y < 3.2) streak = fbm(float3(p.x, p.y * 0.08, p.z) + 3.3, 6.0, fp);
        weight += vertical * smoothstep(0.55, 0.75, streak) * 0.18 * smoothstep(3.2, 0.8, p.y);
#ifdef GLASS
        // N64f (the game, 06/11: the windscreen stayed clean with the body at full dust). Every weight above is
        // the body's: `low`, which carries most of it, dies above 2.3 m, because the dust the wheels lift settles
        // low down. A windscreen sits at 2.2 to 3.6 m [measured here: D11i on this truck], so all of it falls in
        // the part of the model that is meant to stay nearly clean -- and it was getting the thin film alone.
        // A pane does not get dirty that way: it faces the way the truck is going and the airflow drives into it
        // everything the road lifts, which is why a windscreen is filthy while the roof above it is not. So on the
        // glass the weight does not fall off with height at all; it follows how squarely the pane meets the wind.
        // |ns.z| and not -ns.z: the pane's normal is turned towards the camera, so its sign changes with where the
        // camera is, and the dirt must not.
        weight = max(weight, 0.45 + 0.40 * saturate(abs(ns.z)));
#endif
        weight = saturate(weight);
    }
    const float vertical = on_wheel ? 0.0 : 1.0 - smoothstep(0.35, 0.6, abs(ns.y));
    // N12: the heavy coat past level 0.9, its thickness 0..1 (0: thin, the
    // paint shows): more low down and on the edges, in broad thick and thin
    // areas, and on vertical faces in runs left by water running down.
    const float4 l0 = max(body0, front0), l1 = max(body1, front1);
    const float top_level = max(max(dust, max(max(l0.x, l0.y), max(l0.z, l0.w))), max(max(l1.x, l1.y), max(body2.x, front2.x)));
    float heavy = 0.0;
    [branch] if (top_level > 0.9) {
        if (on_wheel) {
            heavy = saturate(0.2 + 1.6 * (shape - 0.5) + 0.4 * smoothstep(0.6, 0.97, ring));
        } else {
            const float big = smoothstep(0.32, 0.68, fbm(p * float3(0.8, 0.45, 0.8) + 37.1, 0.9, fp));
            const float runs = smoothstep(0.38, 0.62, fbm(float3(p.x, p.y * 0.15, p.z) + 13.7, 8.0, fp));
            const float edge = smoothstep(0.95, 0.75, max(abs(ns.x), max(abs(ns.y), abs(ns.z))));
            heavy = 0.5 * smoothstep(3.0, 0.3, p.y) + 0.3 * edge + 0.6 * (big - 0.5) + 0.12 * smoothstep(0.5, 0.9, ns.y);
            heavy += vertical * 0.4 * (runs - 0.5);
            heavy = saturate(heavy + 0.2);
        }
    }
)"  // N74: one more literal here: MSVC 2022 caps one at 16380 characters (C2026)
R"(
    // Coverage: patches grow from the dirtiest spots as the level rises, with a
    // soft edge (two scales), over a thin film everywhere; past 0.9 the heavy coat.
    const float hd = high(dust);
    const float cover = weight * (0.45 + 0.85 * shape + 0.12 * (detail - 0.5)) + hd * 0.9 * heavy;
    const float t = thr(dust);
    // D14: first the film (low down, the wheels, the rear and top faces first: the weight), the patches from the
    // dirtiest spots only from level ~0.35 on (on a wheel ~0.6: the tread "more uniform"), the heavy coat past 0.9.
    float a = smoothstep(t - 0.24, t + 0.24, cover) * (0.88 + 0.08 * hd * heavy) *
              smoothstep(on_wheel ? 0.6 : 0.35, on_wheel ? 0.95 : 0.75, dust);
    a = max(a, film(dust, weight, shape));
    // N31: a trailer's sides ride in the dust the wheels raise: a film that grows with the level from the
    // medium ones on (not past a threshold), heavier low down, in broad clouds and streaks hanging down.
    float side_cloud = 0.0;  // the side film per unit of level (the soot's coat takes it too)
    [branch] if (side_film > 0.0) {
        const float cloud = fbm(p * float3(0.7, 0.5, 0.7) + 41.3, 0.8, fp);
        const float hang = fbm(float3(p.x, p.y * 0.1, p.z) + 7.7, 4.0, fp);
        const float lowness = smoothstep(3.8, 1.0, p.y);
        side_cloud = side_film * vertical * (0.7 + 1.0 * lowness) * along *  // N74: thicker towards the tail
                     saturate(0.2 + 1.6 * cloud + 0.8 * (hang - 0.5) + 0.3 * (shape - 0.5));
        a = max(a, dust * side_cloud);
    }
    // D11f (the user, 29/10: the soot on the trailer's sides too, "the dust has the distribution I expected of the
    // soot"): the soot's coat is the dust's own coverage at the soot's level (patches, heavy coat, the side film).
    const float sl = max(body2.x, front2.x);
    float soot_a = 0.0;
    [branch] if (sl > kSeen) {
        const float ts = thr(sl);
        soot_a = smoothstep(ts - 0.24, ts + 0.24, weight * (0.45 + 0.85 * shape + 0.12 * (detail - 0.5)) +
                                                      high(sl) * 0.9 * heavy) * smoothstep(0.35, 0.75, sl);
        soot_a = max(max(soot_a, film(sl, weight, shape)), sl * side_cloud);  // D14: the film first, as the dust's
        soot_a = saturate(soot_a * (0.9 + 0.2 * detail)) * smoothstep(0.0, 0.05, sl);
    }
    // Splatter: small drops thrown by the wheels, only where they are big enough to see.
    float drops = 0.0;  // D13f: only where they show (behind a wheel, dust past 0.15)
    [branch] if (fan > 0.0 && dust > 0.15) drops = smoothstep(0.80, 0.86, fnoise(p + 5.1, 30.0, fp)) * fan * smoothstep(0.15, 0.5, dust);
    a = max(a, drops * 0.8);
    // Grain: the film is never perfectly even.
    a = saturate(a * (0.9 + 0.2 * detail));
    // Colour: light dust, darker road grime in blotches low on the body, both
    // varied by a mid-size pattern so a fully covered area is not flat; the
    // heavy coat darker and browner where it is thick.
    const float tone = fbm(sq * float3(1.0, 1.6, 1.0) + 9.7, 3.2, fq);
    // N75 (the user, 12/11: "review the colour, the gloss and the light of every dirt texture"): dry road dust
    // reflects 0.35-0.45 of the light; at 0.50 with a +-25% pattern its bright spots reached 0.62, which is chalk.
    const float3 light_dust = float3(0.47, 0.42, 0.34);
    const float3 grime = float3(0.25, 0.23, 0.20);
    const float low = smoothstep(1.2, 0.2, p.y);
    float blotch = 0.0;  // D13f: only where it tints (the wheels, the body below 1.2 m)
    [branch] if (on_wheel || low > 0.0) blotch = smoothstep(0.45, 0.75, fbm(sq + 23.1, 2.4, fq));
    // N31: a trailer's side film greyer low down, a mid tone that shows on light paint (aluminium) as on dark.
    const float grime_mix = on_wheel ? 0.3 * blotch + 0.15
                                     : low * (0.25 + 0.6 * blotch) + fan * 0.2 + side_film * vertical * 0.5 * smoothstep(3.2, 1.0, p.y);
    float3 col = lerp(light_dust, grime, saturate(grime_mix));
    col *= 0.80 + 0.38 * tone + 0.08 * (detail - 0.5);  // N75: 0.80-1.18 (was 0.74-1.24)
    col = lerp(col, col * float3(0.55, 0.47, 0.38), hd * smoothstep(0.15, 0.65, heavy));
    a *= 0.82 + 0.2 * tone;
    a = dust > 0.0 ? a : 0.0;
    // D11b (ATS 29/10: the tyres patchy 350 m after the wash, dust 0.02): the patches come in gently over the first
    // fifth of the level; past it they grew from the threshold alone, full in the heaviest spots at any level.
    a *= smoothstep(0.0, 0.05, dust);  // D14: the film needs no more than the first sliver (the patches come late)
    // D14c (the user, 31/10: the tread "in contact with the ground should take the road's tone much faster than the
    // rest, but rolling all the time it should not get as dirty at the limit as the other dirt, except the mud"): on the
    // tread the dust comes fast (full by level ~0.15, ~1 real minute of dirt road) and stops at ~45%; the side walls as
    // the rest. (The mud, a layer of its own, cakes on the tread as before.)
    // N71 (the user, ATS 07/11, a print of the drive wheels with a little soot: the tread "needs a bigger
    // difference in tone, a little darker"): the road's tone is the asphalt's dark grey, not the beige the body
    // takes -- and a rolling contact patch holds far less loose dust than D14c gave it. On the tread the film is
    // thinner (0.45 -> 0.12) and goes to that tone; the side walls and the rims keep the beige. The salt stays
    // as it was: in winter it really does cake on a tread.
    // N75 (the user, 12/11: the tread "still not very realistic when it is dirty"). N71 gave the tread the
    // asphalt's dark tone whatever the road, so a tyre coming off a dirt road had beige side walls around a clean
    // black band -- and a tyre rolling on dirt is dusted over its whole face at every turn, crowns included. The
    // road's tone is the road's: while nothing paved has scrubbed it (body1.w, 0 on dirt) the band carries the
    // dust, a little darker where it is pressed; the first few hundred metres of asphalt take it to N71's thin
    // dark film.
    if (on_wheel) {
        const float scrub = smoothstep(0.0, 0.6, saturate(body1.w));
        a = lerp(a, lerp(0.55, 0.12, scrub) * saturate(dust * 7.0) * (0.92 + 0.16 * detail), tread);
        col = lerp(col, lerp(col * 0.9, float3(0.07, 0.065, 0.058) * (0.7 + 0.6 * tone), scrub), tread);
    }
    // D11k (N44): up close (a pixel under ~1 mm) the film breaks into grains of ~1-2 mm, lighter and darker than it,
    // with a few bits of grit of ~2-6 mm; both stand out a little (the NORM variant's relief).
    // N72 (the user, 07/11, on the tyre of the N71 print: "the colours look right, but I think it is still short
    // of realism -- I do not know if it is the play of light or the want of texture"): the wheel was left out of
    // this from the start, and that is why its dirt was perfectly smooth up close while the cab's had grain. The
    // reason was real -- these grains are laid in the unit's frame and would slide over a turning tyre -- so on a
    // wheel they are laid in the tyre's own frame (wm, in radii, back to metres, one offset per wheel) and the
    // slope they leave is turned back into the unit's frame for the relief pass. At speed they fade out: the
    // tyre's pattern blurs into rings there, and 5 mm grains turning would only strobe.
    const float grain_w = on_wheel ? 1.0 - smoothstep(0.15, 0.5, blur) : 1.0;
    [branch] if (a > 0.0 && grain_w > 0.0 && fp < 0.004) {
        const float3 gp = on_wheel ? wm * wr + wid * 2.37 : p;
        float3 d1, d2;
        // N72: on a tyre the grains are coarser and show through a thinner film than on paint -- what the wheels
        // throw up is grit, not the fine dust that settles on a cab, and a pale grain on black rubber reads at a
        // tenth of the coverage it needs on a painted panel.
        const float2 g1 = specks(gp, on_wheel ? 0.008 : 0.005, 0.75, fp, d1);
        const float2 g2 = specks(gp + 7.7, on_wheel ? 0.020 : 0.014, 0.22, fp, d2);
        const float ga = saturate(g1.x * (on_wheel ? smoothstep(0.02, 0.22, a) : smoothstep(0.05, 0.4, a))) * grain_w;
        col = lerp(col, col * (0.45 + 1.1 * g1.y), ga * 0.9);
        const float gb = g2.x * (on_wheel ? smoothstep(0.04, 0.30, a) : smoothstep(0.1, 0.5, a)) * grain_w;
        col = lerp(col, lerp(float3(0.14, 0.13, 0.11), float3(0.58, 0.54, 0.47), g2.y), gb);
        a = max(a, max(ga * a * 1.2, gb));
        float3 bump = d1 * ga * 0.4 + d2 * gb * 0.5;
        [branch] if (on_wheel) {  // out of the tyre's frame: its rolling turn (about x), then its steering (about y)
            const float3 b = float3(bump.x, bump.y * wrt.x + bump.z * wrt.y, -bump.y * wrt.y + bump.z * wrt.x);
            bump = float3(b.x * wst.x + b.z * wst.y, b.y, -b.x * wst.y + b.z * wst.x);
        }
        s_bump += bump;
        s_amp = max(s_amp, a * hd * heavy * 0.0015);
    }

    // D4 layers. On a wheel the noise runs in the tyre's frame, as the dust's.
    const float wet_road = body1.z;
    const float up = smoothstep(0.5, 0.9, ns.y);
    const float under = smoothstep(-0.15, -0.7, ns.y);
    const float rear = smoothstep(0.2, 0.9, ns.z);
    // Faces looking forward (-z), strongest low: where the truck's splash hits the trailer.
    const float front_face = on_wheel ? 0.0 : smoothstep(-0.3, -0.8, ns.z) * smoothstep(3.8, 1.0, p.y);
    const float tyre = 0.6 + 0.4 * smoothstep(0.5, 0.97, ring);  // the tread takes the most
    // N31: what the road wore off the tread (body1.w): a share of every layer but the wet spray goes, the
    // rest turns to a grey film; a little more stays in the grooves (broken rings around the tyre).
    const float worn = on_wheel ? tread * saturate(body1.w) : 0.0;
    float groove = 0.0;  // D13f: only on a worn tread
    [branch] if (worn > 0.0)
        groove = smoothstep(0.55, 0.8, fnoise(float3(sq.x, wid * 0.37, 0.0) + 19.7, 9.0, fq)) *
                 smoothstep(0.3, 0.6, fnoise(sq + 27.1, 4.0, fq));
    const float tk = 1.0 - worn * (1.0 - 0.35 * groove), tg = worn * 0.6;
    // Rain runs: thin vertical streaks, ~10 cm apart, long downwards.
    // D13f: only on upright faces, for the layers that take it (the dust on a wet road, the spray, the soot).
    float run = 0.0;
    [branch] if (vertical > 0.0 && (wet_road > 0.0 || max(body0.z + body0.w, front0.z + front0.w) > kSeen ||
                                    max(body2.x, front2.x) > kSeen))
        run = smoothstep(0.42, 0.68, fbm(float3(sq.x, sq.y * 0.07, sq.z) + 61.3, 7.0, fq));
    // While the road is wet, the water running down the sides breaks the dust into runs.
    a *= lerp(1.0, 0.3 + 1.1 * run, vertical * wet_road * 0.8);

    float4 acc = 0;   // albedo: premultiplied colour, coverage
    float2 sacc = 0;  // specular: premultiplied value, coverage

)"  // three literals: MSVC 2022 caps one at 16380 characters (C2026)
R"(
    // Road spray (wet, glossy) and the film it dries into (matte).
    // N65: the film is in here with the spray. A dry blade leaves it alone (it smears, which is why the user
    // asked for it to stay); the jet is what takes it.
    const float spray_b = (body0.z + body0.w) * (1.0 - jetted), spray_f = (front0.z + front0.w) * (1.0 - jetted);
    [branch] if (max(spray_b, spray_f) > kSeen) {
        const float reach = (body0.z * reach0.z + body0.w * reach0.w) / max(spray_b, 1e-4);
        const float hs = high(max(max(body0.z, body0.w), max(front0.z, front0.w)));
        const float top = lerp(0.9, 2.6, reach) + 0.3 * hs;
        // D13f: the edge moves splash()'s line only below 1.25 x its top, and not on a wheel (edge in 0..1).
        float edge = 0.5;
        [branch] if (!on_wheel && p.y < 1.25 * top) edge = fbm(sq * float3(1.0, 1.0, 0.4) + 47.1, 1.2, fq);
        const float wb = on_wheel ? tyre * 0.8
                                  : max(splash(p.y, top, edge, zone, fan, under, up),
                                        rear * 0.8 * (1.0 - up) * smoothstep(top + 1.2, top * 0.5, p.y));
        const float3 wl = pick(wb, spray_b, front_face, spray_f);
        const float wet_share = lerp(body0.z / max(spray_b, 1e-4), front0.z / max(spray_f, 1e-4), wl.z);
        const float shape = fbm(sq * float3(1.0, 2.2, 0.3) + 51.3, 1.6, fq);  // streaky along the body
        const float t = thr(wl.y);
        const float cover = wl.x * (0.45 + 0.8 * shape) + high(wl.y) * 0.5 * heavy * saturate(wl.x * 2.5);
        float as = max(smoothstep(t - 0.3, t + 0.3, cover) * 0.85, wl.y * wl.x * 0.45);
        as *= lerp(1.0, 0.6 + 0.6 * run, vertical * 0.5);  // gathers along the runs, thinner between
        as = saturate(as * (0.92 + 0.16 * detail));
        const float lw = smoothstep(-0.2, 0.2, shape - (1.0 - wet_share) + (wet_share - 0.5) * 0.4);
        as *= smoothstep(0.0, 0.2, wl.y);  // D11b
        as *= lerp(tk, 1.0, lw);  // N31: the worn tread keeps the wet spray, not the dried film
        const float3 film = float3(0.25, 0.24, 0.22) * (0.85 + 0.3 * shape) * (1.0 - 0.3 * high(wl.y) * heavy);
        over(acc, sacc, lerp(film, float3(0.085, 0.08, 0.072), lw), as * lerp(0.8, 0.92, lw), lerp(0.02, 0.22, lw), as);
    }
    // Salt crust, whiter at the rims and along tide lines; darker and thinner
    // while wet. On a wheel a grey, matte powder: no tide lines, less on the tread.
    const float salt_keep = 1.0 - saturate(max(kWipeSalt * wiped, jetted));
    const float salt_b = body1.y * salt_keep, salt_f = front1.y * salt_keep;
    [branch] if (max(salt_b, salt_f) > kSeen) {
        const float hs = high(max(salt_b, salt_f));
        const float top = lerp(0.8, 2.0, reach1.y) + 0.3 * hs;
        float edge = 0.5;  // D13f (as the spray's)
        [branch] if (!on_wheel && p.y < 1.25 * top) edge = fbm(sq + 88.8, 1.6, fq);
        const float wb = on_wheel ? tyre * 0.6 * (1.0 - 0.45 * tread) : splash(p.y, top, edge, zone, fan, under, up);
        const float3 wl = pick(wb, salt_b, front_face, salt_f);
        const float shape = fbm(sq * float3(1.0, 1.3, 1.0) + 91.3, 2.2, fq);
        const float t = thr(wl.y);
        const float cover = wl.x * (0.25 + 1.1 * shape) + high(wl.y) * 0.4 * heavy * saturate(wl.x * 2.5);
        const float tide = on_wheel ? 0.0 : smoothstep(0.36, 0.47, abs(frac(shape * 7.0) - 0.5)) * saturate(1.5 - fq * 60.0) * wl.x;
        const float rim = on_wheel ? 0.0 : 1.0 - saturate(abs(cover - t) / 0.12);
        float asl = max(max(smoothstep(t - 0.12, t + 0.12, cover) * 0.75, wl.y * wl.x * 0.2), tide * wl.y * 0.6);
        // On a wheel: patches of powder (the rubber shows between them), more on the side walls.
        if (on_wheel)
            asl = wl.y * (0.15 + 0.55 * smoothstep(0.44, 0.64, shape + 0.15 * (detail - 0.5) + 0.12 * (1.0 - tread) +
                                                   0.1 * high(wl.y) * heavy)) * (1.0 - 0.4 * tread);
        asl = saturate(asl * (0.88 + 0.24 * detail)) * (1.0 - 0.35 * wet_road) * smoothstep(0.0, 0.2, wl.y);  // D11b
        float3 c = on_wheel ? float3(0.50, 0.50, 0.48) * (0.88 + 0.16 * shape + 0.08 * (detail - 0.5))
                            : float3(0.60, 0.59, 0.56) * (0.85 + 0.25 * shape);
        c = lerp(c, float3(0.80, 0.79, 0.76), saturate(max(rim * 0.6, tide * 0.7))) * (1.0 - 0.25 * wet_road);
        over_worn(acc, sacc, c, asl, on_wheel ? 0.008 : 0.03, asl, tk, tg);
    }
    // Gravel grit: fine specks, a mean tone where a speck is under a pixel.
    // N64: the grit is the "poeira" of the user's words: inside the arc the blade takes almost all of it.
    const float grit_keep = 1.0 - saturate(kWipeDust * wiped + jetted);
    const float grit_b = body1.x * grit_keep, grit_f = front1.x * grit_keep;
    [branch] if (grit_b + grit_f > 0.0) {
        const float hs = high(max(grit_b, grit_f));
        const float top = lerp(0.7, 2.0, reach1.x) + 0.3 * hs;
        float edge = 0.5;  // D13f (as the spray's; the grit's specks show at any level: no kSeen)
        [branch] if (!on_wheel && p.y < 1.25 * top) edge = fbm(sq + 71.9, 2.0, fq);
        // N71: the grit is a light grey; on the tread, scrubbed by the road, little of it stays.
        const float wb = on_wheel ? tyre * (1.0 - 0.7 * tread)
                                  : saturate(splash(p.y, top, edge, zone, fan, under, 0.0) + up * zone * 0.4);
        const float3 wl = pick(wb, grit_b, front_face, grit_f);
        const float c = saturate(wl.x * min(wl.y, 0.9) * (0.5 + 1.0 * fbm(sq + 77.7, 2.0, fq)) +
                                 high(wl.y) * 0.4 * heavy * saturate(wl.x * 2.5));
        // Turned lattices: on faces lined up with the unit's axes a plain one shows square specks.
        const float3x3 r = float3x3(0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64);
        const float fine = 0.6 * fnoise(mul(r, sq) + 81.1, 70.0, fq) + 0.4 * fnoise(mul(r, mul(r, sq)) + 3.7, 150.0, fq);
        // N76 (the user, 12/11: white spots on the truck up close that "do not look real" and vanish as the camera
        // backs off; the truck carried a trace of grit, 0.0105). The specks covered ~5% of the face at any level, at
        // 0.9 alpha, while the far view showed the level's mean (~0.75%): a trace of grit was a field of white dots
        // up close and nothing from 10 m. Now the specks' area follows the coverage (`fine`'s spread, measured:
        // above 0.80 1.6%, 0.70 8.7%, 0.60 25%) and a speck's alpha the level, so near and far carry the same amount;
        // and the grit is the road's grey, not a near-white.
        const float t_s = 0.80 - 0.35 * sqrt(c);
        const float speck = smoothstep(t_s, t_s + 0.06, fine) * 0.8 * smoothstep(0.0, 0.05, c);
        const float ag = max(lerp(c * 0.75, speck, saturate(1.5 - fq * 210.0)), c * 0.3);
        over_worn(acc, sacc, float3(0.43, 0.415, 0.38) * (0.8 + 0.35 * fine), ag, 0.02, ag, tk, tg);
    }
    // The dust (D3, as before).
    // dust is matte; N75: damp (a wet road), it is a third darker and takes a faint sheen, as wet soil does
    over_worn(acc, sacc, col * (1.0 - 0.35 * wet_road), a, lerp(0.012, 0.07, wet_road), saturate(a * 1.05), tk, tg);
)"  // six literals: MSVC 2022 caps one at 16380 characters (C2026)
R"(
    // N39: road soot (oil, grease, rubber, the street's grime): dark brown-black, a faint oily sheen. Over the dust
    // (the low parts stay dark under a dusty cab, as in the user's photo, 29/10), under the mud. Not worn off the
    // tread as the rest (it is the road's own grime): the tread just takes less of it than the rims and side walls.
    // N65: the soot is greasy, so a dry blade only moves it about; the jet takes it.
    const float soot_b = body2.x * (1.0 - jetted), soot_f = front2.x * (1.0 - jetted);
    [branch] if (max(soot_b, soot_f) > kSeen) {
        // D11f: first the coat where the dust would lie (soot_a, above: the trailer's sides too), dark grey-brown,
        // darker low down; then (D11d) the darkest grime low down and on the wheels over it. D11g (the user, 30/10:
        // "the upper part too strong in colour and intensity"): up high a light brown haze, at most ~35%; low down
        // as before (~85%, near black).
        // N60 (the user, ATS 05/11: as the soot builds up the tread goes "very shiny, as if it had oil around
        // it"; on the tread he expected "something more matte, cleaning itself over time on asphalt until it looks
        // like the rubber it is, since the contact patch wears all the time", with the road's grime "less
        // noticeable than it is today"): on the tread the road's grime is thin, matte and sanded off as the tyre
        // rolls -- `tk` already does that for the other layers (N31), the soot was left out of it because it is
        // the road's own grime. It still is, only far less of it: at most about a quarter of the side walls' coat,
        // and what the road has worn (body1.w) takes most of the rest with it. The rims and the side walls keep
        // what they had, grease near the hub included.
        const float on_tread = on_wheel ? tread : 0.0;
        // N60b (the user, ATS 05/11 23:13: "the bottom of the wheel is darker again"): with the grime nearly gone
        // the bare rubber showed where the tread faces the camera, at the bottom of the tyre, and against beige
        // side walls that reads as a dark band. A little more of the road's grime stays, still matte: about 45% of
        // it on a worn tread instead of 30%.
        const float tread_keep = 1.0 - 0.55 * on_tread * (0.3 + 0.7 * saturate(body1.w));
        const float lowdown = smoothstep(3.0, 0.8, p.y);
        // D14: a grey-brown haze at first, low down too; darker as the level rises (near black low down from ~0.6).
        const float3 coat = lerp(float3(0.225, 0.195, 0.158), float3(0.06, 0.05, 0.04), lowdown * smoothstep(0.1, 0.6, sl));  // N75: greyer
        const float coat_a = soot_a * lerp(0.45, 0.8, lowdown) * tread_keep;  // D14: up high a haze up to ~40%
        over(acc, sacc, coat * (0.85 + 0.3 * tone), coat_a, 0.05, coat_a);
        const float hs = high(sl);
        const float top = lerp(1.1, 2.1, reach2.x) + 0.4 * hs;
        float edge = 0.5;  // D13f (as the spray's)
        [branch] if (!on_wheel && p.y < 1.25 * top) edge = fbm(sq + 105.3, 1.4, fq);
        const float wb = on_wheel ? 0.95 - 0.6 * tread  // N60/N60b: the tread takes a third of the side walls
                                  : max(splash(p.y, top, edge, zone, fan, under, up),
                                        rear * 0.6 * (1.0 - up) * smoothstep(top + 0.8, top * 0.4, p.y)) * (1.0 - 0.5 * ahead);  // N74
        const float3 wl = pick(wb, soot_b, front_face, soot_f);
        const float smear = fbm(sq * float3(1.0, 1.8, 0.5) + 111.7, 1.3, fq);  // smeared along the body
        const float t = thr(wl.y);
        const float cover = wl.x * (0.6 + 0.8 * smear) + high(wl.y) * 0.5 * heavy * saturate(wl.x * 2.5);
        // D14: a film first (it jumped to near black between levels 0.2 and 0.4), its blotches from ~0.4 on.
        float ak = max(smoothstep(t - 0.3, t + 0.3, cover) * 0.97 * smoothstep(0.4, 0.8, wl.y),
                       1.0 - exp(-1.8 * wl.y * wl.x * (0.7 + 0.6 * smear)));
        ak *= lerp(1.0, 0.7 + 0.5 * run, vertical * 0.4);
        // D11k (N44, the user 30/10: "mixed with a little oil near the chassis and the tank, blacker and denser"): low
        // on the body, from ~1.5 m down, denser, near black and glossier.
        // N60: no oily sheen on the tread (the rims and the side walls keep theirs).
        const float oily = on_wheel ? 0.25 * (1.0 - tread) : smoothstep(1.5, 0.6, p.y);
        ak = saturate(ak * (0.9 + 0.2 * detail) * (1.0 + 0.6 * oily)) * smoothstep(0.0, 0.05, wl.y);  // D11b, D14
        ak *= tread_keep;  // N60
        const float3 c = lerp(lerp(float3(0.024, 0.021, 0.018), float3(0.06, 0.05, 0.04), smear), float3(0.03, 0.026, 0.021),
                              0.5 * oily) * (0.9 + 0.2 * detail);
        // N60: matte on the tread (0.08 -> ~0.015), as the rubber is.
        over(acc, sacc, c, ak, (0.08 + 0.06 * smear + 0.22 * oily) * (1.0 - 0.85 * on_tread), ak);
        // D11k: drops of oil up close (~2-6 mm, stretched downwards on upright faces), glossy and slightly raised.
        [branch] if (!on_wheel && fp < 0.008 && oily > 0.0) {
            float3 dd;
            const float2 od = specks(p * float3(1.0, lerp(1.0, 0.55, vertical), 1.0) + 13.1, 0.018,
                                     0.2 * oily * saturate(wl.y * 1.6), fp, dd);
            const float oa = od.x * (0.45 + 0.5 * od.y);  // some thin and half dried, some fresh and black
            over(acc, sacc, float3(0.004, 0.0035, 0.003), oa, 0.6, oa);
            s_bump += dd * oa * 0.5;
        }
    }
    // Mud: one pattern for wet and dry (dry mud is wet mud that dried).
    // N64d: of the mud the blade takes little and smears the rest into bands; the jet takes all of it.
    const float mud_keep = (1.0 - saturate(max(kWipeMud * wiped, jetted))) * smear;
    const float mud_b = (body0.x + body0.y) * mud_keep, mud_f = (front0.x + front0.y) * mud_keep;
    [branch] if (max(mud_b, mud_f) > kSeen) {
        const float reach = (body0.x * reach0.x + body0.y * reach0.y) / max(mud_b, 1e-4);
        const float hs = high(max(max(body0.x, body0.y), max(front0.x, front0.y)));
        // Up to ~1.55 m at reach 0.5 and ~2.8 m at reach 1 (2.3 m before N12); higher past level 0.9.
        const float top = 0.8 + 1.5 * reach + 0.5 * smoothstep(0.6, 1.0, reach) + 0.3 * hs;
        float edge = 0.5;  // D13f (as the spray's)
        [branch] if (!on_wheel && p.y < 1.25 * top) edge = fbm(sq + 31.7, 1.8, fq);
        // N58b: on a wheel the tread full, the side wall from the shoulder in, more at the rim's lip. N58c (the user, ATS
        // 05/11: N58b's tyre "not realistic", light stripes over the whole side wall): the shoulder a solid coat, the
        // fingers only where it thins out (~0.6-0.9 of the radius), little further in.
        const float lip = 1.0 - smoothstep(0.0, 0.05, abs(ring - 0.56));
        const float tyre_mud = saturate(lerp(0.04 + 1.15 * smoothstep(0.6, 0.93, ring), 1.0, tread) + 0.2 * lip);
        const float wb = on_wheel ? tyre_mud  // N58; N74: a tyre throws its mud back, never ahead of the first axle
                                  : mud_splash(p.y, top, edge, zone, fan, rays, under, up) * (1.0 - 0.65 * ahead);
        const float3 wl = pick(wb, mud_b, front_face, mud_f);
        const float wet_share = lerp(body0.x / max(mud_b, 1e-4), front0.x / max(mud_f, 1e-4), wl.z);
        float shape = fbm(sq * float3(1.0, 0.8, 1.0) + 4.4, 2.6, fq);  // clumps
        shape = lerp(shape, sweep, swept * 0.75);  // N74: on the sides, dashes drawn back by the air (0 on a wheel)
        // N58b: radial fingers, a few clumps in them; N58c: the fingers at the coat's thinning edge only.
        if (on_wheel) shape = lerp(shape, tyre_streak, 0.6 * (1.0 - abs(smoothstep(0.6, 0.93, ring) * 2.0 - 1.0)));
        const float thick = fbm(sq + 12.9, 6.0, fq);
        const float hm = high(wl.y);
        const float t = thr(wl.y);
        float am = smoothstep(t - 0.08, t + 0.08, wl.x * (0.35 + 0.95 * shape) + 0.15 * (thick - 0.5) +
                                                      hm * 0.45 * heavy * saturate(wl.x * 2.5));
        am = max(am, wl.y * wl.x * 0.22);  // a thin smear around the clumps
        // Drops of two sizes thrown in the fan and the arches.
        const float drops = max(smoothstep(0.80, 0.85, fnoise(sq + 7.3, 14.0, fq)), smoothstep(0.81, 0.86, fnoise(sq + 2.9, 31.0, fq)));
        am = max(am, drops * max(fan, zone * 0.7) * smoothstep(0.1, 0.45, wl.y) * 0.95);  // N58c: not in the rays (dots)
        am = saturate(am * (0.9 + 0.2 * detail)) * 0.96 * smoothstep(0.0, 0.2, wl.y);  // D11b
        // Wet or dry here: the thick parts stay wet the longest.
        const float lw = smoothstep(-0.12, 0.12, thick - (1.0 - wet_share) + (wet_share - 0.5) * 0.3);
        const float3 wet_col = float3(0.12, 0.085, 0.055) * (0.75 + 0.5 * thick);
        float3 dry_col = lerp(float3(0.41, 0.335, 0.245), float3(0.31, 0.24, 0.165), saturate(smoothstep(0.3, 0.7, thick) + hm * 0.6 * heavy));  // N75
        float crack = 0.0;
        [branch] if (wet_share < 0.999) crack = cracks(sq, fq) * smoothstep(0.45, 0.65, thick);
        dry_col *= (1.0 - 0.4 * crack) * (0.9 + 0.2 * detail);
        float3 mud_col = lerp(dry_col, wet_col, lw);
        // N59: the mud lies in cakes of ~6-14 cm, 1-2 cm thick where it piles up: the slope of that field tilts the
        // normal (so the cakes catch the game's light from metres away, not only the D11k clumps up close), the
        // hollows between them go darker and the raised tops lighter, and a cake's rim takes a little more mud.
        // Far away both octaves are faded out (fp > ~7 cm a pixel): nothing to work out.
        [branch] if (!on_wheel && am > 0.0 && fp < 0.07) {
            const float4 cake = cake_field(p + 71.3, fp);
            const float body = smoothstep(0.08, 0.5, am);        // only where the coat is more than a smear
            const float h = saturate(cake.w * 1.8 - 0.4);        // its height, 0..1
            const float thickness = body * (0.006 + 0.026 * h) * (1.0 + 0.4 * hm) * (1.0 - 0.25 * lw);
            s_bump += cake.xyz * thickness;
            // Lit by shape alone: the hollows in shadow, the tops catching more (a cake reads as a cake in flat light);
            // under a cake's own edge the darkest, where in life it shades the layer below.
            const float ledge = saturate(1.0 - abs(h - 0.32) * 7.0) * saturate(-cake.y * 0.35);
            mud_col *= (0.58 + 0.8 * h) * (1.0 - 0.3 * ledge);
            // A caked edge is ragged, not a fade: where the coat thins out, the cakes stand apart.
            const float rim = smoothstep(0.65, 0.12, am);
            am = saturate(am * lerp(1.0, saturate(0.25 + 1.9 * h), rim) + body * 0.1 * h);
        }
        // N59: two generations where the mud dried and was splashed again: over the pale dried base, darker wet
        // splatter in patches of ~3-10 cm, as much as the layer's fresh share says.
        [branch] if (!on_wheel && am > 0.0 && wet_share > 0.05 && lw < 0.85 && fp < 0.06) {
            const float fresh = smoothstep(0.52, 0.74, fbm(p * float3(1.0, 1.1, 1.0) + 97.7, 9.0, fp)) *
                                smoothstep(0.05, 0.5, wet_share) * (1.0 - lw);
            mud_col = lerp(mud_col, float3(0.095, 0.068, 0.045) * (0.8 + 0.4 * thick), fresh * 0.85);
        }
        // D11k (N44): the mud's volume: ~2-8 mm thick in clumps (the thick parts thicker; wet mud a little flatter), and
        // up close bits of grit and stones of ~1-4 mm in it, lighter and darker.
        if (!on_wheel) s_amp = max(s_amp, am * (0.002 + 0.006 * thick) * (1.0 + 0.5 * hm) * (1.0 - 0.3 * lw));
        [branch] if (!on_wheel && am > 0.0 && fp < 0.004) {
            float3 dm;
            const float2 st = specks(p + 3.3, 0.007, 0.45, fp, dm);
            const float sa = st.x * smoothstep(0.2, 0.7, am);
            mud_col = lerp(mud_col, lerp(float3(0.10, 0.08, 0.06), float3(0.52, 0.47, 0.40), st.y) * lerp(1.0, 0.6, lw), sa * 0.8);
            s_bump += dm * sa * 0.7;
        }
        over_worn(acc, sacc, mud_col, am, lerp(0.015, 0.28 * (0.7 + 0.6 * detail), lw), am, tk, tg);
    }
    // N67e: the dirt the rubber is carrying along, laid over whatever the pane still has. It is the same
    // stuff that is on the glass, so it takes the glass's own colour where there is one.
    [branch] if (ridge > 0.0) {
        const float3 rc = acc.a > 0.01 ? acc.rgb / acc.a : float3(0.42, 0.38, 0.32);
        const float ra = saturate(0.85 * ridge);
        over(acc, sacc, rc, ra, 0.05, ra);
    }
    if (diag) return float4(0.04, 0.04 + 0.96 * saturate(acc.a), 0.04, 1);
    if (acc.a <= 0.0) return none;
    s_end = true, s_spec = float2(sacc.x / max(sacc.y, 1e-5), saturate(sacc.y));
#ifdef GLASS
    // N64i: break the coverage up so the pane reads as dirty glass and not as tinted glass. The film's own even
    // haze is left alone -- that part IS flat on a real windscreen -- and what varies is the dust over it: broad
    // patches, the same fine grain the body gets, and more of it towards the border, where the airflow is slow.
    [branch] if (on_pane) {
        const float patches = fbm(p * 2.6 + 11.3, 1.1, fp);
        const float fine = grain(p * 1.6, fp);
        const float ex = saturate(abs(pane_q.x) / max(pane.x, 0.1));            // 0 middle .. 1 the side edges
        const float ey = saturate(abs(pane_q.y - 0.5 * pane_h) / (0.5 * pane_h));  // 0 middle .. 1 top and bottom
        const float border = 0.30 * smoothstep(0.45, 1.0, max(ex, ey));
        const float even = 0.34;  // the film's share, which stays even however the dust falls
        const float uneven = saturate(0.30 + 1.15 * patches + 0.30 * (fine - 0.5) + border);
        acc.a *= saturate(even + (1.0 - even) * uneven);
    }
    // N64g ([glass] on = 5): paint what the shader worked out, instead of blending it -- red is how much dirt it
    // puts on this pixel and green is how much the blades took there. One screenshot then says whether the dirt
    // is reaching the pane at all, how strong it is where it reaches, and where the arc really sits, which no
    // amount of looking at a blended windscreen can settle.
    if (glass.z > 4.5 && glass.z < 5.5) return float4(saturate(acc.a), wiped, 0, 1);  // [glass] on = 5
#endif
    return float4(acc.rgb / acc.a, saturate(acc.a));
}
)"  // five literals: MSVC 2022 caps one at 16380 characters (C2026)
R"(
// D11m: the pack (uint4): x the colour (sqrt, 10 bits each) and in its top two bits what the pixel is (1: shade() got to
// the end, 2: the diagnosis, 3: the position test, the position in yzw; 0: none of these), y the coverage and the specular's coverage (16 bits each), z the specular and the gloss
// (16 bits each), w the relief's tilted normal in camera space (D13f: octahedral, 15 bits each; bit 30: tilted).
uint q16(float v) { return uint(saturate(v) * 65535.0 + 0.5); }
uint q10(float v) { return uint(saturate(v) * 1023.0 + 0.5); }
float u16(uint v) { return float(v & 65535u) / 65535.0; }
float3 u10x3(uint v) { return float3(v & 1023u, (v >> 10) & 1023u, (v >> 20) & 1023u) / 1023.0; }
uint q15(float v) { return uint(saturate(v) * 32767.0 + 0.5); }
// D13f: a unit vector in two numbers 0..1 (octahedral) and back.
float2 oct(float3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const float2 s = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    const float2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * s;
    return e * 0.5 + 0.5;
}
float3 unoct(float2 e) {
    e = e * 2.0 - 1.0;
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
#ifdef GLASS
// N62: the dirt on the glass, lit by the daylight since nothing else lights it for us. One entry point per blend
// the game draws its glass with:
//  - dual-source (1.62): result = o0 + what is on the screen x o1, so o1 lets less of the scene through and o0
//    puts the dirt's own colour in -- the dirt covers the glass as it covers the paint;
//  - GLASS_MULT (DEST_COLOR / ZERO, older versions): only a multiply is possible, so the glass is multiplied by
//    the dirt's colour where it covers;
//  - GLASS_OVER (our own blend, over a reflection the game added in a pass of its own): plain alpha.
// shade() is called once and once only in each of them: it is the whole shader, and a second call doubles both
// the code and the time it takes to compile (N62b: with two, the compile outlived a 90 s test run).
// glass.z: 2 = paint the glass where the dirt reaches it, 3 = paint each pixel where the shader places it in the
// truck (the plugin sets the shader's own position test for that; tools/probetest checks it), 4 = paint the arc the
// wipers sweep (N64), to see on one screenshot whether it sits where the blades really are.
// N64k (the user, 06/11, with a print of the rear quarter window: "esperava algo assim nos vidros da frente e
// lateral, de fato obstruindo a visao"). The dirt on the glass was being written in ALBEDO units (0..1, a
// reflectance) into the game's forward target, which holds the scene's RADIANCE before the tone map -- and that
// target is exposed down by some forty times, as the glass's own coverage test showed: [glass] on = 5 painted
// 0.72 and the screen read 52/255 once the gamma is undone. So the dirt landed at a fortieth of what a lit
// surface there is worth: a pane that only darkened the glass a little instead of hazing it.
// The body never had this problem: its dirt goes into the G-buffer's albedo and the game's own lighting puts it
// in the scene's units. A forward draw has to do that itself, which is what pane.w is: how bright a surface of
// this dirt is under the sky, in the scene's units. The daylight keeps shaping it over the day.
float4 glass_dirt(float4 pos, out float a) {
    const float4 d = shade(pos);
    const bool raw = glass.z > 2.5;  // the place, the arc and the ring are numbers: no strength, no daylight
    a = raw ? saturate(d.a) : min(saturate(d.a) * glass.x, 0.97);
    const float3 lit = raw ? d.rgb : d.rgb * max(pane.w, 0.0) * (0.12 + 0.88 * glass.y);
    return float4(lit, a);
}
#if defined(GLASS_MULT)
float4 main(float4 pos : SV_Position) : SV_Target0 {
    if (glass.z > 1.5 && glass.z < 2.5) return float4(0, 0, 0, 1);
    float a;
    const float4 d = glass_dirt(pos, a);
    if (glass.z > 2.5) return float4(d.rgb, 1);  // the position test (3) and the coverage (5), unlit
    return float4(lerp(float3(1, 1, 1), d.rgb, a), 1);
}
#elif defined(GLASS_OVER)
float4 main(float4 pos : SV_Position) : SV_Target0 {
    if (glass.z > 1.5 && glass.z < 2.5) return float4(5, 0, 2, 1);
    float a;
    const float4 d = glass_dirt(pos, a);
    if (glass.z > 2.5) return float4(d.rgb, 1);
    return float4(d.rgb, a);
}
#else
void main(float4 pos : SV_Position, out float4 o0 : SV_Target0, out float4 o1 : SV_Target1) {
    if (glass.z > 1.5 && glass.z < 2.5) {  // [glass] on = 2
        o0 = float4(5, 0, 2, 0);
        o1 = float4(0, 0, 0, 1);
        return;
    }
    float a;
    const float4 d = glass_dirt(pos, a);
    if (glass.z > 4.5) {  // [glass] on = 5 and 6: the numbers themselves, nothing of the screen left
        o0 = float4(d.rgb, 0);
        o1 = float4(0, 0, 0, 1);
        return;
    }
    if (glass.z > 2.5 && glass.z < 3.5) {  // [glass] on = 3: the place itself, nothing of the screen left
        o0 = float4(d.rgb, 0);
        o1 = float4(0, 0, 0, 1);
        return;
    }
    o0 = float4(d.rgb * a, 0);
    o1 = float4(1.0 - a, 1.0 - a, 1.0 - a, 1);
}
#endif
#elif defined(PACK)
uint4 main(float4 pos : SV_Position) : SV_Target0 {
    const float4 d = shade(pos);
    uint4 o = 0;
    if (params.y > 1.5 && params.y < 2.5) {  // the position test: the position unclamped
        if (d.a > 0.0) o = uint4(3u << 30, asuint(d.x), asuint(d.y), asuint(d.z));
        return o;
    }
    const float3 c = sqrt(saturate(d.rgb));  // finer steps in the darks, where the dirt's colours are
    o.x = q10(c.r) | (q10(c.g) << 10) | (q10(c.b) << 20) | ((params.y > 2.5 ? 2u : s_end ? 1u : 0u) << 30);
    o.y = q16(d.a) | (q16(s_spec.y) << 16);
    // Dry dust 0.015, wet mud up to ~0.3 (the layers' specular): gloss 0 = matte, 1 = wet (REFL).
    o.z = q16(s_spec.x) | (q16((s_spec.x - 0.015) / 0.265) << 16);
    // D11k (N44): the relief, where there is dirt (spin.w: the NORM draw follows): the normal tilted by the slope of
    // the dirt's height (the mud's clumps, the grains and drops) on the surface.
    [branch] if (spin.w > 0.5 && d.a > 0.0 && params.y < 0.5) {
        float3 slope = s_bump;
        [branch] if (s_amp > 0.0) slope += s_amp * relief_grad(s_p, s_fp);
        const float3 t = (slope - s_n * dot(slope, s_n)) * d.a;
        if (dot(t, t) > 1e-10) {
            // To camera space: the transpose of the camera -> unit rotation (rows u0..u2).
            const float3 tc = t.x * u0.xyz + t.y * u1.xyz + t.z * u2.xyz;
            // D13f: the tilted normal itself, so NORM writes it without reading rt0 (no copy of rt0). The game's
            // normal is a unit vector; where it is not, no relief.
            const float len = length(s_g.xyz);
            if (abs(len - 1.0) < 0.01) {
                const float2 e = oct(normalize(s_g.xyz / len - tc));
                o.w = q15(e.x) | (q15(e.y) << 15) | (1u << 30);
            }
        }
    }
    return o;
}
#else
float4 colour(uint4 k) {
    if ((k.x >> 30) == 3u) return float4(asfloat(k.y), asfloat(k.z), asfloat(k.w), 1);  // the position test
    const float3 c = u10x3(k.x);
    return float4(c * c, u16(k.y));
}
#if defined(SPEC)
float4 main(float4 pos : SV_Position) : SV_Target0 {
    const uint4 k = g_pack.Load(int3(pos.xy, 0));
    if ((k.x >> 30) == 2u) return colour(k);  // the diagnosis: as the albedo
    if ((k.x >> 30) != 1u) return 0;
    const float s = u16(k.z);
    return float4(s, s, s, u16(k.y >> 16));
}
#elif defined(REFL)
// N25: rt3 with the paint's reflection (emittance and strength) scaled by what the dirt leaves of it; the
// exponent, the material bits and the shadow stay as the game wrote them.
uint scaled(uint v, float k) { return uint(float(v) * k + 0.5); }
uint4 main(float4 pos : SV_Position) : SV_Target0 {
    uint4 o = g_rt3.Load(int3(pos.xy, 0));
    const uint4 k = g_pack.Load(int3(pos.xy, 0));
    // The coverage and the gloss; the diagnosis takes the emittance away (it cannot hide what the albedo says).
    const float2 d = params.y > 2.5 ? float2(1, 0) : (k.x >> 30) == 1u ? float2(u16(k.y), u16(k.z >> 16)) : 0;
    const float keep = 1.0 - d.x * (1.0 - 0.6 * d.y);
    o.x = (scaled((o.x >> 11) & 31u, keep) << 11) | (scaled((o.x >> 5) & 63u, keep) << 5) | scaled(o.x & 31u, keep);
    o.z = (scaled(o.z >> 2, keep) << 2) | (o.z & 3u);
    return o;
}
#elif defined(NORM)
// D11k (N44): rt0 (the normal xyz in camera space, the camera depth w) with the dirt's relief. D13f: the tilted
// normal PACK worked out, through the albedo's blend (alpha 1 where tilted: written; 0: rt0 as the game wrote it; the
// depth kept by the write mask). rt0 is not read (it is the target): no copy of it.
float4 main(float4 pos : SV_Position) : SV_Target0 {
    const uint w = g_pack.Load(int3(pos.xy, 0)).w;
    if (!(w & (1u << 30))) return 0;
    return float4(unoct(float2(w & 32767u, (w >> 15) & 32767u) / 32767.0), 1);
}
#else
float4 main(float4 pos : SV_Position) : SV_Target0 { return colour(g_pack.Load(int3(pos.xy, 0))); }
#endif
#endif
)";

// Mirrors kPixelShader's cbuffer.
struct Constants {
    float vp[4];
    float proj[3][4];
    float unit[3][4];
    float params[4];
    float body[kSlots];   // body0, body1, body2
    float front[kSlots];  // front0, front1, front2
    float reach[kSlots];  // reach0, reach1, reach2
    float wheel[kWheels][4];   // x y z this frame (rig::GetWheels: config + deflection), radius (centre height at rest)
    float spin[4];             // x: how much the wheels' angle blurs into rings, 0..1; y: 1 = the cab's pass;
                               // z: 1 = a trailer's pass (N31)
    float turn[kWheels][4];    // cos, sin of each wheel's steering; cos, sin of its rolling turn
    float wheel_out[16];       // N20: per wheel, rig::GetWheelReach (the last two unused)
    float wheel_radius[16];    // N22: per wheel, rig::GetWheelRadius (the last two unused)
    float inside_lo[4];        // N41: the box with no dust (the cab's inside): low corner, on
    float inside_hi[4];        // N41: its high corner, -
    float cavity_lo[4];        // D11f: the cab's cavity (no dust on the faces turned to its middle): low corner, on
    float cavity_hi[4];        // D11f: its high corner, -
    float dash0[4];            // D11j: the windshield: front z + 3 cm, base height, dz/dy, top height
    float dash1[4];            // D11j: the front zone: half width, floor, back z (the eye), on
    float glass[4];            // N62: on the glass: how strong, the daylight, the test mode, N64: the wipe 0..1
    float pane[4];             // N64b: the windshield: half its width (m), 1 = measured; N65: the jet, 0..1; -
    float blade[4];            // N64c: the stroke: the wipe before it, the blade across the sweep, its way, -
    float wiper[4];            // N64: the arc: the pivots' x and y (shares of the pane), the blade, half the sweep
    float wblade[6][4];        // N68: the two blades the game describes, three rows each (see the cbuffer above)
    float wnow[4];             // N68: where each blade is across its stroke, how many blades there are
};
static_assert(sizeof(Constants) % 16 == 0, "constant buffer size");

// N12: the wheel constants (spin, turn) from the telemetry: per wheel the SDK's
// wheel.steering and wheel.rotation, both in turns, and the unit's speed.
// [estimate: SDK headers] steering > 0 turns the wheel to the left
// (counterclockwise seen from above, as truck.effective.steering), and the
// rotation grows as the wheel rolls forward; the signs flip them if the game
// shows otherwise. In the unit's frame (+x right, +y up, +z rear) the wheel's
// frame is turned about y by the steering, and the tyre about the wheel's x by
// minus the rotation (rolling forward the top goes to -z).
constexpr float kSteerSign = 1.f, kRollSign = 1.f;
// Between these speeds (km/h) a wheel's angle blurs into rings [estimate]: a
// fast wheel looks like rings, and a telemetry frame behind the drawing would
// turn the pattern by a visible angle.
constexpr float kSpinBlurFrom = 15.f, kSpinBlurTo = 45.f;

// N13: the look of level 1 is the look level 0.8 had before; 0..1 stays everywhere else.
constexpr float kLevelScale = 0.8f;
// D11d (the user, 29/10: "the dust at the top level is too intense"): the dust's own scale, lower: level 1 now looks
// as 0.85 did (0.68 in the pass).
constexpr float kDustScale = 0.68f;

// N13: the levels of the pass (params.x, body, front) from the unit's dust level and layers (0..1, as
// the model and the panel give them), clamped and scaled by kLevelScale. The road wetness (kWet), the
// tread's wear (kTread) and the reach are no levels: clamped only.
inline void Levels(Constants& c, float dust, const Layers& l) {
    auto clamp01 = [](float v) { return v > 0.f ? (v < 1.f ? v : 1.f) : 0.f; };  // NaN -> 0
    c.params[0] = clamp01(dust) * kDustScale;
    for (int s = 0; s < kSlots; ++s) {
        const float k = s == kWet || s == kTread ? 1.f : kLevelScale;
        c.body[s] = clamp01(l.body[s]) * k;
        c.front[s] = clamp01(l.front[s]) * k;
        c.reach[s] = clamp01(l.reach[s]);
    }
}

inline void Wheels(Constants& c, const float* steering, const float* rotation, int n, float speed_kmh) {
    constexpr double kTurn = 6.283185307179586;
    for (int i = 0; i < kWheels; ++i) {
        const double s = i < n && steering ? kSteerSign * steering[i] * kTurn : 0.0;
        const double r = i < n && rotation ? kRollSign * rotation[i] * kTurn : 0.0;
        c.turn[i][0] = static_cast<float>(std::cos(s)), c.turn[i][1] = static_cast<float>(std::sin(s));
        c.turn[i][2] = static_cast<float>(std::cos(r)), c.turn[i][3] = static_cast<float>(std::sin(r));
    }
    float t = (std::fabs(speed_kmh) - kSpinBlurFrom) / (kSpinBlurTo - kSpinBlurFrom);
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    c.spin[0] = t * t * (3.f - 2.f * t);
    c.spin[1] = c.spin[2] = c.spin[3] = 0.f;
}

}  // namespace dust
