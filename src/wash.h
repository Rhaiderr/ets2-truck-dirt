// D5: the paid wash at the service station, the logic only (no drawing, no
// charging in the exe: the plugin does both, V4).
//
// Pure bookkeeping, no game, Windows or D3D calls (tools/washtest runs it
// offline). The plugin calls Machine::Update once per frame with the real
// time and what it knows of the game; the time is injected, so the test is
// deterministic and the phases do not depend on the frame rate.
//
// The wash, as the user decided (the development notes):
// - the player is in the service station's area (the map trigger, V4),
//   stopped and with the parking brake on; only then the hint shows, with the
//   price; the wash key (F8 by default, read by the plugin) starts it;
// - fixed price per rig, from the INI: the truck alone, or the truck with a
//   trailer (EUR 120 / 180 in ETS2, USD 120 / 180 in ATS; the game's base
//   currency, as the exe charges it, for both games); the hint and the
//   messages show it in the currency picked in the game's menu
//   (Settings::display_rate, given by the plugin, V4);
// - the balance, when the plugin knows it, must cover the price (no wash in
//   the red); a company trailer is washed too, at the price with a trailer;
//   the plugin
//   charges (Step::charge) and reports the result (Machine::Charged); with
//   [wash] charge = 0 (tests) nothing is charged;
// - the phases run in real time: darken -> animation -> lighten, the screen
//   black in between and nothing moved in the game. The dirt is cleaned
//   (Step::clean) when the lighten phase starts, under the black screen;
// - "moved, ends early": moving the truck anyway (the parking brake is only
//   needed to start), leaving the area or pressing the key again (it skips
//   the animation) during darken or animation goes straight to lighten (from
//   the darkness reached); the rig is still all cleaned and the wash is paid;
// - the game paused (menu) freezes the phases; a frame longer than
//   kMaxFrameS counts as kMaxFrameS (a loading hitch does not skip a phase).
// [estimate: every time, threshold and default below, for the game test]
//
// L7 (Spots, below): the SDK has no service-station event and the station is
// not a map trigger action [verified: D4l probe, the development notes]. So
// the plugin learns the stations where the player refuels: stopped, the
// parking brake on and the fuel rising there. The spot is kept (the plugin
// saves Spots::Serialize to wash_spots.txt); stopped near a known spot later,
// the truck is "in the area" of Input::in_area.
#pragma once

#include <string>
#include <vector>

namespace wash {

constexpr double kMaxFrameS = 0.25;  // longest frame step counted, in seconds

struct Settings {
    int price_truck = 120;    // the truck alone (game's base currency, whole units)
    int price_trailer = 180;  // the truck with a trailer
    bool charge = true;       // false: the wash is free (tests, before V4)
    double darken_s = 0.8;    // screen to black (D11f)
    double show_s = 5.0;      // the animation on the black screen
    double lighten_s = 0.8;   // back to the game
    double message_s = 4.0;   // how long a message stays
    double charge_timeout_s = 3.0;  // no answer to the charge: it failed
    float min_dirt = 0.02f;   // below this (every layer) the rig is already clean
    float stop_kmh = 0.5f;    // |speed| below this is stopped (to start)
    float move_kmh = 2.0f;    // |speed| above this is moving (ends the wash early)
    // Shown prices = base price x this (the currency picked in the game's
    // menu; the plugin sets it from the game, V4). The charge stays in the
    // base currency.
    double display_rate = 1.0;
};

// One INI option of [wash] (key = price_truck, price_trailer, charge,
// darken_s, show_s, lighten_s, message_s, min_dirt, display_rate). Numbers
// without locale.
// A bad value leaves the setting as it was and returns false with the reason.
bool SetOption(Settings* s, const std::string& key, const std::string& value, std::string* error);

// The price of the rig, in the base currency (what is charged).
int Price(const Settings& s, bool trailer);
// A base-currency amount as shown (display_rate applied, whole units).
long long DisplayPrice(const Settings& s, int base);

enum class Phase {
    kIdle,      // not in the area, or the area with nothing going on
    kCharging,  // waiting for Charged()
    kDarken,
    kShow,
    kLighten,
};

// What the plugin shows, in the language of the game (texts: N9).
enum class Message {
    kNone,
    kHint,          // "F8: wash the truck (price)" while in the area, stopped, the parking brake on
    kStopFirst,     // the key in the area, moving
    kParkingBrake,  // the key in the area, stopped, the parking brake off
    kAlreadyClean,  // nothing to wash
    kNoMoney,       // the balance is below the price
    kChargeFailed,  // the charge failed or did not answer
    kWashed,        // the wash ended
    kEndedEarly,    // the wash ended early (moved, left, key)
    kUnknownStation,  // D4y: the key outside every known station (refuel there once: L7)
};

const char* PhaseName(Phase p);
const char* MessageName(Message m);

// What the plugin knows this frame.
struct Input {
    double now_s = 0;       // real time, seconds (any origin, never going back)
    bool paused = false;    // the game is paused (menu)
    bool in_area = false;   // in the wash area of a service station (V4)
    float speed_kmh = 0;    // the truck's speed (signed)
    bool parking_brake = false;  // the truck's parking brake is on (truck.brake.parking)
    bool trailer = false;   // a trailer is attached (trailer.0)
    float dirt = 0;         // the dirtiest layer of the units the wash would clean
    bool key = false;       // the wash key went down this frame (an edge, not held)
    bool balance_known = false;
    long long balance = 0;  // the player's money, when known
};

// What the plugin must do after this frame.
struct Step {
    bool charge = false;  // charge `amount`, then call Charged()
    int amount = 0;
    unsigned clean = 0;   // reset these units (bit 1 << unit: 0 truck, 1 trailer), then save
};

// What the plugin draws this frame.
struct View {
    Phase phase = Phase::kIdle;
    float darkness = 0;     // 0 = the game, 1 = black
    float show = 0;         // animation progress 0..1 (kShow; 1 after)
    Message message = Message::kNone;
    int price = 0;          // for kHint and kNoMoney, in the base currency
    long long display_price = 0;  // the same, as shown (Settings::display_rate)
    // D11f: what this wash costs while it runs (kDarken to kLighten; 0 = free), for the visit screen.
    int paid = 0;
    long long paid_display = 0;
};

class Machine {
public:
    explicit Machine(const Settings& s = Settings()) : settings_(s) {}
    void Configure(const Settings& s) { settings_ = s; }
    const Settings& Config() const { return settings_; }

    Step Update(const Input& in);
    // The result of Step::charge (the same frame or later).
    void Charged(bool ok);

    View Current() const;
    Phase CurrentPhase() const { return phase_; }
    bool Busy() const { return phase_ != Phase::kIdle; }

private:
    void Say(Message m, int price = 0);
    void Lighten(Step* step);

    Settings settings_;
    Phase phase_ = Phase::kIdle;
    double t_ = 0;           // seconds in the phase (paused time not counted)
    double last_now_ = 0;
    bool started_ = false;   // last_now_ valid
    float darkness_ = 0;
    float show_ = 0;         // animation progress
    float lighten_from_ = 0; // darkness when kLighten started
    unsigned units_ = 0;     // the units paid for
    int amount_ = 0;
    bool ended_early_ = false;
    Message message_ = Message::kNone;
    int message_price_ = 0;
    double message_t_ = 0;   // seconds the message has left
    bool hint_ = false;      // idle in the area, stopped, the parking brake on
    int hint_price_ = 0;
};

// ---- L7: the service stations, learnt at a refuel ----

// A place in the game's world, metres (truck.world.placement: y up).
struct Spot {
    double x = 0, y = 0, z = 0;
};

// [estimate: every value, for the game test]
struct SpotSettings {
    double radius_m = 25;    // a station's area around the place of a refuel (horizontal)
    double height_m = 8;     // and up or down (a bridge over the station is not in it)
    float refuel_l = 3.f;    // the fuel risen this much in one place: a refuel
    double refuel_s = 0.5;   // over at least this long, in 3 or more rises (a new truck's fuel jumps at once)
    float jump_l = 50.f;     // a rise this big in one frame is not a pump (a truck or profile change)
    double still_m = 1.5;    // farther from where it stopped: a new place
    float stop_kmh = 0.5f;   // |speed| below this is stopped
    int max_spots = 2000;    // the oldest goes beyond this
};

// What the plugin knows this frame, for the refuel watch.
struct RefuelInput {
    double now_s = 0;  // real time, seconds
    bool paused = false;
    Spot at;           // the truck
    float speed_kmh = 0;
    bool parking_brake = false;
    float fuel_l = 0;  // truck.fuel.amount
};

class Spots {
public:
    explicit Spots(const SpotSettings& s = SpotSettings()) : settings_(s) {}
    void Configure(const SpotSettings& s) { settings_ = s; }
    const SpotSettings& Config() const { return settings_; }

    // One frame of the refuel watch. True when a new spot was learnt now (the
    // plugin saves the file then); the same stop learns at most one.
    bool Update(const RefuelInput& in);
    // In the area of a known spot.
    bool Near(const Spot& at) const;
    // Adds a spot unless one is already that near (true: added).
    bool Add(const Spot& s);
    const std::vector<Spot>& All() const { return spots_; }
    void Clear() { spots_.clear(); }

    // The file: a header line, then "spot x y z" per line ('.' decimals, any locale).
    std::string Serialize() const;
    // Replaces the spots with the text's; a bad line is skipped and goes to
    // *errors (with its number). False when the header is missing (nothing read).
    bool Parse(const std::string& text, std::vector<std::string>* errors);

private:
    void Watch(const RefuelInput& in);

    SpotSettings settings_;
    std::vector<Spot> spots_;
    // The stop being watched.
    bool watching_ = false;
    bool learnt_ = false;    // this stop already gave a spot
    Spot anchor_;
    float base_l_ = 0;       // the lowest fuel seen in this stop
    float last_l_ = 0;
    int rises_ = 0;
    double first_rise_s_ = 0, last_rise_s_ = 0;
};

}  // namespace wash
