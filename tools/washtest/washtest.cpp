// Offline test of the D5 wash at the service station (src/wash): the price,
// the INI options, the conditions to start (area, stopped, parking brake,
// dirt, balance), the shown price in the game's currency,
// the charge, the phases in real time, "moved, ends early", the pause and the
// frame rate. The clock is injected: every run prints the same.
// Usage: washtest

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "wash.h"

using wash::Input;
using wash::Machine;
using wash::Message;
using wash::Phase;
using wash::Settings;
using wash::Step;
using wash::View;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what, const std::string& detail = std::string()) {
    printf("%s %-66s %s\n", ok ? "ok  " : "FAIL", what, detail.c_str());
    if (!ok) ++g_failures;
}

std::string Num(double v) {
    char b[32];
    snprintf(b, sizeof(b), "%.3f", v);
    return b;
}

// The plugin's side: a truck stopped in the wash area, a clock and a frame
// step; it charges (or refuses) as told and records what happened.
// The phases' times the checks below were written for (D11f: the defaults are 0.8 / 5 / 0.8 s now, checked apart).
Settings Timed() {
    Settings s;
    s.darken_s = 1.0, s.show_s = 6.0, s.lighten_s = 1.0;
    return s;
}

struct Game {
    explicit Game(const Settings& s = Timed()) : machine(s) {
        in.in_area = true;
        in.parking_brake = true;
        in.dirt = 0.5f;
    }
    Machine machine;
    Input in;
    double fps = 60;
    int pay = 1;  // answer to a charge: 1 paid, 0 refused, -1 no answer
    int charges = 0;
    int charged = 0;          // sum of the amounts
    unsigned cleaned = 0;     // units reset
    int clean_events = 0;
    double clean_at = -1;     // time of the last clean
    float clean_darkness = -1;  // the screen drawn the frame before the clean
    bool trace = false;
    Phase last_phase = Phase::kIdle;
    Message last_message = Message::kNone;

    void Frame(double dt) {
        const float before = machine.Current().darkness;
        in.now_s += dt;
        const Step s = machine.Update(in);
        in.key = false;
        if (s.charge) {
            ++charges;
            if (pay == 1) charged += s.amount;
            if (pay >= 0) machine.Charged(pay == 1);
        }
        if (s.clean) {
            cleaned |= s.clean;
            ++clean_events;
            clean_at = in.now_s;
            clean_darkness = before;
            in.dirt = 0;
        }
        const View v = machine.Current();
        if (trace && (v.phase != last_phase || v.message != last_message || s.charge || s.clean)) {
            printf("     t=%7.3f %-8s dark=%.3f show=%.3f msg=%-13s price=%d%s%s\n", in.now_s, wash::PhaseName(v.phase),
                   v.darkness, v.show, wash::MessageName(v.message), v.price,
                   s.charge ? (" charge " + std::to_string(s.amount)).c_str() : "",
                   s.clean ? (" clean " + std::to_string(s.clean)).c_str() : "");
        }
        last_phase = v.phase;
        last_message = v.message;
    }
    // Frames at `fps` for `seconds`.
    void Run(double seconds) {
        const int n = static_cast<int>(std::lround(seconds * fps));
        for (int i = 0; i < n; ++i) Frame(1.0 / fps);
    }
    // Runs until the machine is idle again (at most `limit` seconds).
    double RunToIdle(double limit = 30) {
        const double start = in.now_s;
        while (machine.Busy() && in.now_s - start < limit) Frame(1.0 / fps);
        return in.now_s - start;
    }
    void Key() {
        in.key = true;
        Frame(1.0 / fps);
    }
    View Now() const { return machine.Current(); }
};

void PriceTests() {
    printf("-- price and INI\n");
    Settings s;
    Check(wash::Price(s, false) == 120 && wash::Price(s, true) == 180, "default: 120 the truck alone, 180 with a trailer",
          std::to_string(wash::Price(s, false)) + " / " + std::to_string(wash::Price(s, true)));
    Check(s.darken_s == 0.8 && s.show_s == 5.0 && s.lighten_s == 0.8,
          "D11f: the visit's times by default: 0.8 s, 5 s, 0.8 s");
    {
        Settings r = Timed();
        r.display_rate = 1.5;
        Game g(r);
        g.Key();
        for (int i = 0; i < 120; ++i) g.Frame(1.0 / g.fps);
        const View v = g.Now();
        Check(v.phase == wash::Phase::kShow && v.paid == 120 && v.paid_display == 180,
              "D11f: the visit shows what this wash costs (base 120, shown x1.5)",
              std::to_string(v.paid) + " / " + std::to_string(v.paid_display));
    }
    std::string err;
    const bool ok = wash::SetOption(&s, "price_truck", " 95 ", &err) && wash::SetOption(&s, "price_trailer", "150", &err) &&
                    wash::SetOption(&s, "show_s", "2.5", &err) && wash::SetOption(&s, "min_dirt", ".05", &err) &&
                    wash::SetOption(&s, "charge", "0", &err);
    Check(ok && s.price_truck == 95 && s.price_trailer == 150 && s.show_s == 2.5 && s.min_dirt == 0.05f && !s.charge,
          "INI options read (spaces trimmed, no locale)", err);
    const Settings before = s;
    struct Bad {
        const char* key;
        const char* value;
    };
    const Bad bad[] = {{"price_truck", "-5"},  {"price_truck", "12.5"}, {"price_trailer", "1e3"},
                       {"price_trailer", ""},  {"charge", "2"},         {"show_s", "1,5"},
                       {"darken_s", "11"},     {"min_dirt", "1.5"},     {"price", "100"},
                       {"show_s", "."},        {"price_truck", "99999999999"}};
    int rejected = 0;
    for (const Bad& b : bad) {
        std::string e;
        if (!wash::SetOption(&s, b.key, b.value, &e) && !e.empty()) ++rejected;
    }
    Check(rejected == static_cast<int>(sizeof(bad) / sizeof(bad[0])) && s.price_truck == before.price_truck &&
              s.price_trailer == before.price_trailer && s.show_s == before.show_s && s.min_dirt == before.min_dirt &&
              s.charge == before.charge,
          "bad values rejected, the settings unchanged", std::to_string(rejected) + " rejected");
    {
        std::string e;
        wash::SetOption(&s, "price_truck", "abc", &e);
        Check(e == "price_truck = \"abc\": expected a whole number from 0 to 1000000", "the reason, for the log", e);
    }
}

void StartTests() {
    printf("-- conditions to start\n");
    {
        Game g;
        g.in.in_area = false;
        g.Frame(0.016);
        Check(g.Now().message == Message::kNone, "out of the area: no hint");
        g.Key();
        g.Run(1);
        Check(!g.machine.Busy() && g.charges == 0 && g.Now().message == Message::kUnknownStation,
              "key out of the area: no wash, refuel here first (D4y)", wash::MessageName(g.Now().message));
        g.Run(10);
        Check(g.Now().message == Message::kNone, "... the message goes");
        g.in.in_area = true;
        g.Frame(0.016);
        Check(g.Now().message == Message::kHint && g.Now().price == 120, "in the area, stopped: hint with the price",
              std::to_string(g.Now().price));
        g.in.trailer = true;
        g.Frame(0.016);
        Check(g.Now().price == 180, "with a trailer: the rig's price", std::to_string(g.Now().price));
        g.in.speed_kmh = 30;
        g.Frame(0.016);
        Check(g.Now().message == Message::kHint, "moving in the area: the hint already (D11c)");
        g.Key();
        Check(!g.machine.Busy() && g.Now().message == Message::kStopFirst && g.charges == 0, "key while moving: stop first",
              wash::MessageName(g.Now().message));
        g.Run(3.9);
        Check(g.Now().message == Message::kStopFirst, "the message stays 4 s");
        g.Run(0.2);
        Check(g.Now().message == Message::kHint, "then goes: the hint again, still moving in the area (D11c)");
        g.in.speed_kmh = -0.3f;
        g.Frame(0.016);
        Check(g.Now().message == Message::kHint, "rolling back under 0.5 km/h counts as stopped");
        g.in.paused = true;
        g.Key();
        Check(!g.machine.Busy() && g.Now().message == Message::kNone, "key in the pause menu: ignored, no hint");
    }
    {
        Game g;
        g.in.dirt = 0.01f;
        g.Key();
        Check(!g.machine.Busy() && g.Now().message == Message::kAlreadyClean && g.charges == 0,
              "already clean (dirtiest layer under 0.02): refused, not charged");
    }
    {
        Game g;
        g.in.trailer = true;
        g.in.balance_known = true;
        g.in.balance = 179;
        g.Key();
        Check(!g.machine.Busy() && g.Now().message == Message::kNoMoney && g.Now().price == 180 && g.charges == 0,
              "balance 179 < 180: no money, not charged");
        g.Run(5);
        g.in.balance = 180;
        g.Key();
        Check(g.machine.Busy() && g.charges == 1 && g.charged == 180, "balance 180 = price: charged",
              std::to_string(g.charged));
    }
    {
        Game g;
        g.in.balance_known = false;
        g.Key();
        Check(g.charges == 1 && g.charged == 120, "balance unknown: the charge decides", std::to_string(g.charged));
    }
}

void PhaseTests() {
    printf("-- a full wash (trace)\n");
    {
        Game g;
        g.trace = true;
        g.in.trailer = true;
        g.Frame(0.016);
        g.Key();  // charged and paid: the phases start
        const double t0 = g.in.now_s;
        const double busy = g.RunToIdle();
        g.Run(5);
        Check(g.charges == 1 && g.charged == 180, "charged once, the rig's price", std::to_string(g.charged));
        Check(g.clean_events == 1 && g.cleaned == 3u, "cleaned once: truck and trailer", std::to_string(g.cleaned));
        Check(g.clean_at - t0 >= 7.0 && g.clean_at - t0 < 7.0 + 1 / 60.0 && g.clean_darkness == 1.f,
              "clean at the end of darken + show (7 s), screen black",
              Num(g.clean_at - t0) + " s, dark " + Num(g.clean_darkness));
        Check(std::fabs(busy - 8.0) < 0.02, "darken 1 s + show 6 s + lighten 1 s", Num(busy) + " s");
        Check(g.Now().message == Message::kHint, "after the message, the hint again (still in the area)");
    }
    {
        Game g;
        g.Frame(0.016);
        g.Key();
        g.Run(0.5);
        Check(g.Now().phase == Phase::kDarken && std::fabs(g.Now().darkness - 0.5) < 0.02, "darken: half way at 0.5 s",
              Num(g.Now().darkness));
        g.Run(3.5);
        Check(g.Now().phase == Phase::kShow && g.Now().darkness == 1.f && std::fabs(g.Now().show - 0.5) < 0.01,
              "show: black, the animation half way at 4 s", Num(g.Now().show));
        g.in.speed_kmh = 1.5f;
        g.Run(1);
        Check(g.Now().phase == Phase::kShow, "creeping under 2 km/h does not end it");
        g.in.speed_kmh = 0;
        g.RunToIdle();
        Check(g.Now().message == Message::kWashed && g.cleaned == 1u, "truck alone: washed, only the truck cleaned",
              std::to_string(g.cleaned));
    }
    {
        Game g;
        g.Key();
        g.Run(1);
        g.Key();
        g.Run(1);
        Check(g.charges == 1, "the key during the wash does not start or charge another");
    }
}

void EarlyTests() {
    printf("-- moved, ends early (trace)\n");
    {
        Game g;
        g.trace = true;
        g.Key();
        g.Run(3);
        g.in.speed_kmh = 5;
        const double t0 = g.in.now_s;
        const double busy = g.RunToIdle();
        Check(g.clean_events == 1 && std::fabs(g.clean_at - t0 - 1 / 60.0) < 1e-6 && g.clean_darkness == 1.f,
              "moved in the show: cleaned at once, from black", Num(g.clean_at - t0));
        Check(std::fabs(busy - 1.0) < 0.02, "lighten takes the full 1 s", Num(busy) + " s");
        Check(g.Now().message == Message::kEndedEarly && g.charged == 120, "ended early, still paid");
    }
    {
        Game g;
        g.trace = true;
        g.Key();
        g.Run(0.4);
        g.in.in_area = false;
        const double busy = g.RunToIdle();
        Check(g.clean_events == 1 && std::fabs(g.clean_darkness - 0.4) < 0.02,
              "left the area while darkening: cleaned at the darkness reached", Num(g.clean_darkness));
        Check(std::fabs(busy - 0.4) < 0.03, "lightens from there at the same speed (0.4 s)", Num(busy) + " s");
        Check(g.Now().message == Message::kEndedEarly, "ended early");
    }
    {
        Game g;
        g.Key();
        g.Run(2);
        g.Key();
        g.RunToIdle();
        Check(g.clean_events == 1 && g.Now().message == Message::kEndedEarly, "the key again in the show: ends early");
    }
    {
        Game g;
        g.Key();
        g.Run(7.5);
        Check(g.Now().phase == Phase::kLighten, "in lighten");
        g.in.speed_kmh = 20;
        g.Key();
        g.RunToIdle();
        Check(g.clean_events == 1 && g.Now().message == Message::kWashed,
              "moving or the key while lightening: nothing changes, washed");
    }
}

void ChargeTests() {
    printf("-- the charge\n");
    {
        Game g;
        g.pay = 0;
        g.Key();
        g.Run(1);
        Check(!g.machine.Busy() && g.clean_events == 0 && g.Now().message == Message::kChargeFailed,
              "charge refused: no wash, charge failed");
    }
    {
        Game g;
        g.pay = -1;
        g.Key();
        g.Run(2.9);
        Check(g.Now().phase == Phase::kCharging && g.Now().darkness == 0.f, "no answer yet: waiting, the game visible");
        g.in.speed_kmh = 40;  // moving while the charge is out does not matter
        g.Run(0.2);
        Check(!g.machine.Busy() && g.clean_events == 0 && g.Now().message == Message::kChargeFailed,
              "no answer in 3 s: charge failed");
        g.machine.Charged(true);
        g.Run(1);
        Check(!g.machine.Busy() && g.clean_events == 0, "a late answer is ignored");
    }
    {
        Settings s = Timed();
        s.charge = false;
        Game g(s);
        g.Frame(0.016);
        Check(g.Now().message == Message::kHint && g.Now().price == 0, "charge = 0: the hint shows no price");
        g.in.balance_known = true;
        g.in.balance = 0;
        g.Key();
        g.RunToIdle();
        Check(g.charges == 0 && g.clean_events == 1 && g.Now().message == Message::kWashed,
              "charge = 0: free, with no money");
    }
    {
        Settings s = Timed();
        s.price_truck = 0;
        Game g(s);
        g.Key();
        g.RunToIdle();
        Check(g.charges == 0 && g.clean_events == 1, "price 0: washed without a charge");
    }
}

void TimeTests() {
    printf("-- real time: pause, frame rate, hitches\n");
    {
        Game g;
        g.Key();
        g.Run(3);
        g.in.paused = true;
        g.in.in_area = false;  // the area is not known in the menu
        g.Run(60);
        Check(g.Now().phase == Phase::kShow && std::fabs(g.Now().show - 2.0 / 6) < 0.01,
              "paused 60 s in the show: frozen, not ended early", Num(g.Now().show));
        g.in.paused = false;
        g.in.in_area = true;
        const double busy = g.RunToIdle();
        Check(std::fabs(busy - 5.0) < 0.02 && g.Now().message == Message::kWashed, "after the pause, the rest (4 + 1 s)",
              Num(busy) + " s");
    }
    {
        // The same wash at several frame rates and with uneven frames: the
        // phases end at the same time, within one frame.
        const double rates[] = {20, 30, 60, 144, 300};
        bool same = true;
        std::string detail;
        for (const double fps : rates) {
            Game g;
            g.fps = fps;
            g.Key();
            const double start = g.in.now_s;
            const double busy = g.RunToIdle();
            same = same && std::fabs(busy - 8.0) <= 1.0 / fps + 1e-9 && std::fabs(g.clean_at - start - 7.0) <= 1.0 / fps + 1e-9;
            detail += Num(busy) + " ";
        }
        Check(same, "20 to 300 fps: 8 s, clean at 7 s, within a frame", detail);
    }
    {
        Game g;
        g.Key();
        const double t0 = g.in.now_s;
        unsigned seed = 12345;
        double busy = 0;
        while (g.machine.Busy() && busy < 30) {
            seed = seed * 1103515245u + 12345u;
            const double dt = 0.004 + (seed >> 16) % 1000 * 0.00006;  // 4 to 64 ms
            g.Frame(dt);
            busy += dt;
        }
        Check(std::fabs(busy - 8.0) < 0.065 && std::fabs(g.clean_at - t0 - 7.0) < 0.065 && g.clean_events == 1,
              "uneven frames (4 to 64 ms): the same 8 s", Num(busy) + " s");
    }
    {
        Game g;
        g.Key();
        g.Frame(5.0);  // a loading hitch
        Check(g.Now().phase == Phase::kDarken && g.Now().darkness == 0.25f,
              "a 5 s frame counts as 0.25 s: no phase skipped", Num(g.Now().darkness));
        g.Frame(-3.0);  // the clock going back
        Check(g.Now().darkness == 0.25f, "the clock going back counts as 0", Num(g.Now().darkness));
    }
    {
        Settings s = Timed();
        s.darken_s = 0;
        s.show_s = 0;
        s.lighten_s = 0;
        Game g(s);
        g.Key();
        g.Frame(1 / 60.0);
        Check(!g.machine.Busy() && g.clean_events == 1 && g.Now().message == Message::kWashed,
              "every time 0: washed in the frame after the charge");
    }
}

// The user's rules of 28/10/2026: the parking brake to start, the price shown
// in the currency of the game's menu, no wash in the red.
void BrakeAndCurrencyTests() {
    printf("-- parking brake and currency\n");
    {
        Game g;
        g.in.parking_brake = false;
        g.Frame(0.016);
        Check(g.Now().message == Message::kHint, "stopped in the area, parking brake off: the hint already (D11c)");
        g.in.parking_brake = true;
        g.Frame(0.016);
        Check(g.Now().message == Message::kHint, "parking brake on: the hint shows");
        g.in.parking_brake = false;
        g.Key();
        Check(!g.machine.Busy() && g.charges == 0 && g.Now().message == Message::kParkingBrake,
              "key stopped, parking brake off: asked for it, not charged", wash::MessageName(g.Now().message));
        g.in.parking_brake = true;
        g.Key();
        Check(g.machine.Busy() && g.charges == 1, "parking brake on: the wash starts");
    }
    {
        Game g;
        g.Key();
        g.Run(0.5);
        g.in.parking_brake = false;
        g.Run(0.5);
        Check(g.machine.Busy() && g.clean_events == 0, "brake released in darken, not moving: the wash goes on");
        g.in.speed_kmh = 5;
        g.RunToIdle();
        Check(g.clean_events == 1 && g.cleaned == 1 && g.charged == 120 && g.Now().message == Message::kEndedEarly,
              "moved anyway: everything cleaned, the full price paid",
              std::to_string(g.charged) + ", clean " + std::to_string(g.cleaned));
    }
    {
        Settings s = Timed();
        std::string err;
        Check(wash::SetOption(&s, "display_rate", "0.86", &err) && std::fabs(s.display_rate - 0.86) < 1e-9,
              "display_rate read", err);
        const Settings before = s;
        Check(!wash::SetOption(&s, "display_rate", "0", &err) && !wash::SetOption(&s, "display_rate", "-1", &err) &&
                  s.display_rate == before.display_rate,
              "display_rate 0 or negative: rejected", err);
        Game g(s);
        g.in.trailer = true;
        g.Frame(1 / 60.0);
        Check(g.Now().message == Message::kHint && g.Now().price == 180 && g.Now().display_price == 155,
              "hint: 180 in the base currency shown as 155 (rate 0.86)",
              std::to_string(g.Now().price) + " -> " + std::to_string(g.Now().display_price));
        g.Key();
        Check(g.charged == 180, "the charge stays in the base currency", std::to_string(g.charged));
    }
    {
        Game g;
        g.in.balance_known = true;
        g.in.balance = -5000;
        g.Key();
        Check(g.charges == 0 && g.Now().message == Message::kNoMoney && g.Now().display_price == 120,
              "balance negative (a loan): no money, not charged", std::to_string(g.Now().display_price));
    }
}

// L7: a stop at a pump, frame by frame: `litres_s` of fuel a second for `seconds` (60 fps).
struct Pump {
    wash::Spots spots;
    wash::RefuelInput in;
    int learnt = 0;
    Pump() {
        in.at = {-18189.0, 52.0, 18389.0};
        in.parking_brake = true;
        in.fuel_l = 200;
    }
    void Run(double seconds, double litres_s, double fps = 60) {
        const int n = static_cast<int>(std::lround(seconds * fps));
        for (int i = 0; i < n; ++i) {
            in.now_s += 1 / fps;
            in.fuel_l += static_cast<float>(litres_s / fps);
            if (spots.Update(in)) ++learnt;
        }
    }
};

void SpotTests() {
    printf("-- L7: service stations learnt at a refuel\n");
    {
        Pump p;
        p.Run(1, 0);
        p.Run(2, 10);
        Check(p.learnt == 1 && p.spots.All().size() == 1, "stopped, parking brake, fuel rising 20 l: the spot is learnt",
              std::to_string(p.learnt));
        p.Run(5, 10);
        Check(p.learnt == 1, "the same stop learns once", std::to_string(p.learnt));
        Check(p.spots.Near({-18189.0 + 20, 53, 18389.0 + 10}), "22 m away, 1 m up: in the area");
        Check(!p.spots.Near({-18189.0 + 30, 52, 18389.0}), "30 m away: not in the area");
        Check(!p.spots.Near({-18189.0, 62, 18389.0}), "10 m above (a bridge): not in the area");
    }
    {
        Pump p;
        p.in.parking_brake = false;
        p.Run(3, 10);
        Check(p.learnt == 0, "the parking brake off: not learnt (the user's rule)");
    }
    {
        Pump p;
        p.in.speed_kmh = 3;
        p.Run(3, 10);
        Check(p.learnt == 0, "moving: not learnt");
    }
    {
        Pump p;
        p.Run(1, 0);
        p.in.fuel_l += 400;  // a new truck: its fuel at once
        p.Run(3, 0);
        Check(p.learnt == 0, "a jump of 400 l in one frame (a truck change): not a refuel");
        p.Run(1, 0);
        p.in.fuel_l += 20;   // 20 l in one frame, then nothing
        p.Run(3, 0);
        Check(p.learnt == 0, "20 l in one frame, then still: not a refuel (one rise)");
    }
    {
        Pump p;
        p.Run(0.2, 10);
        Check(p.learnt == 0, "2 l: below the 3 l of a refuel");
        p.Run(1, -0.05);  // the idling engine
        p.Run(0.2, 10);
        Check(p.learnt == 1, "the rise counts from the lowest fuel of the stop (4 l in all)", std::to_string(p.learnt));
    }
    {
        Pump p;
        p.Run(0.25, 10);
        p.in.paused = true;
        p.in.at.x += 5000;  // the map, paused: nothing counts
        p.Run(1, 0);
        p.in.paused = false;
        p.in.at.x -= 5000;
        p.Run(0.25, 10);
        Check(p.learnt == 1 && p.spots.All().size() == 1, "a pause in the middle: still one spot", std::to_string(p.learnt));
    }
    {
        Pump p;
        p.Run(0.3, 10);
        p.in.at.x += 3;  // moved to the next pump: a new stop
        p.Run(0.3, 10);
        Check(p.learnt == 0, "3 l over two places 3 m apart: not learnt (each stop counts its own)");
        p.Run(1, 10);
        Check(p.learnt == 1, "then 13 l in the second place: learnt");
    }
    {
        Pump p;
        p.Run(2, 10, 15);
        Check(p.learnt == 1, "15 fps: learnt as at 60", std::to_string(p.learnt));
        Pump q;
        q.Run(2, 10, 144);
        Check(q.learnt == 1, "144 fps: learnt as at 60", std::to_string(q.learnt));
    }
    {
        wash::Spots s;
        Check(s.Add({0, 0, 0}) && !s.Add({10, 0, 10}) && s.Add({100, 0, 0}) && s.All().size() == 2,
              "a spot within 25 m of a known one is the same station");
        wash::SpotSettings small;
        small.max_spots = 2;
        wash::Spots c(small);
        c.Add({0, 0, 0});
        c.Add({100, 0, 0});
        c.Add({200, 0, 0});
        Check(c.All().size() == 2 && c.All()[0].x == 100, "past max_spots the oldest goes");
    }
    {
        wash::Spots s;
        s.Add({-18189.25, 52.5, 18389.75});
        s.Add({1234.5, -3, -98765.126});
        const std::string text = s.Serialize();
        wash::Spots back;
        std::vector<std::string> errors;
        const bool ok = back.Parse(text, &errors);
        Check(ok && errors.empty() && back.All().size() == 2 && std::fabs(back.All()[0].x + 18189.25) < 1e-6 &&
                  std::fabs(back.All()[1].z + 98765.13) < 1e-6 && back.All()[1].y == -3,
              "saved and read back (2 decimals)", std::to_string(back.All().size()));
        printf("     file:\n%s", text.c_str());
        errors.clear();
        const std::string damaged = "\xEF\xBB\xBFtruck_dirt_wash_spots 1\r\nspot 1 2 3\r\nspot 1 2\r\nspot a b c\r\n"
                                    "pump 1 2 3\r\n# note\r\nspot -500 0.5 +7\r\n";
        // Parsed first: the order a call's arguments are evaluated in differs between compilers (MSVC printed 0).
        const bool damaged_ok = back.Parse(damaged, &errors);
        Check(damaged_ok && back.All().size() == 2 && errors.size() == 3 && back.All()[1].x == -500 && back.All()[1].z == 7,
              "BOM, CRLF, bad lines skipped and reported", std::to_string(errors.size()) + " errors");
        for (const std::string& e : errors) printf("     %s\n", e.c_str());
        errors.clear();
        wash::Spots keep;
        keep.Add({1, 1, 1});
        const bool headless = keep.Parse("spot 1 2 3\n", &errors);
        Check(!headless && keep.All().size() == 1 && errors.size() == 1, "no header: nothing read, the spots kept",
              errors.empty() ? "" : errors[0]);
        errors.clear();
        Check(!keep.Parse("", &errors) && keep.All().size() == 1, "empty file: the spots kept");
    }
}

}  // namespace

int main() {
    PriceTests();
    StartTests();
    PhaseTests();
    EarlyTests();
    ChargeTests();
    TimeTests();
    BrakeAndCurrencyTests();
    SpotTests();
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
