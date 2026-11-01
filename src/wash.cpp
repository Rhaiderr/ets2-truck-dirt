#include "wash.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wash {

namespace {

std::string Trim(const std::string& v) {
    const size_t b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = v.find_last_not_of(" \t\r\n");
    return v.substr(b, e - b + 1);
}

// "12", "0.5", ".5", "3." (no sign, no exponent), without the C locale.
bool ParseNumber(const std::string& text, double* out) {
    const std::string v = Trim(text);
    double whole = 0, frac = 0, scale = 1;
    bool digits = false, dot = false;
    for (const char c : v) {
        if (c == '.' && !dot) {
            dot = true;
        } else if (c >= '0' && c <= '9') {
            digits = true;
            if (dot) {
                scale /= 10;
                frac += (c - '0') * scale;
            } else {
                whole = whole * 10 + (c - '0');
                if (whole > 1e9) return false;
            }
        } else {
            return false;
        }
    }
    if (!digits) return false;
    *out = whole + frac;
    return true;
}

bool Range(const std::string& key, const std::string& value, double lo, double hi, bool integer, double* out,
           std::string* error) {
    double v = 0;
    if (!ParseNumber(value, &v) || v < lo || v > hi || (integer && v != std::floor(v))) {
        if (error) {
            *error = key + " = \"" + Trim(value) + "\": expected " + (integer ? "a whole number" : "a number") +
                     " from " + std::to_string(static_cast<long long>(lo)) + " to " +
                     std::to_string(static_cast<long long>(hi));
        }
        return false;
    }
    *out = v;
    return true;
}

}  // namespace

bool SetOption(Settings* s, const std::string& key, const std::string& value, std::string* error) {
    double v = 0;
    if (key == "price_truck" || key == "price_trailer") {
        if (!Range(key, value, 0, 1000000, true, &v, error)) return false;
        (key == "price_truck" ? s->price_truck : s->price_trailer) = static_cast<int>(v);
    } else if (key == "charge") {
        if (!Range(key, value, 0, 1, true, &v, error)) return false;
        s->charge = v != 0;
    } else if (key == "darken_s" || key == "lighten_s") {
        if (!Range(key, value, 0, 10, false, &v, error)) return false;
        (key == "darken_s" ? s->darken_s : s->lighten_s) = v;
    } else if (key == "show_s" || key == "message_s") {
        if (!Range(key, value, 0, 60, false, &v, error)) return false;
        (key == "show_s" ? s->show_s : s->message_s) = v;
    } else if (key == "display_rate") {
        if (!Range(key, value, 0, 1000000, false, &v, error)) return false;
        if (v <= 0) {
            if (error) *error = key + " = \"" + Trim(value) + "\": expected a number above 0";
            return false;
        }
        s->display_rate = v;
    } else if (key == "min_dirt") {
        if (!Range(key, value, 0, 1, false, &v, error)) return false;
        s->min_dirt = static_cast<float>(v);
    } else {
        if (error) *error = "unknown option " + key;
        return false;
    }
    return true;
}

int Price(const Settings& s, bool trailer) { return trailer ? s.price_trailer : s.price_truck; }

long long DisplayPrice(const Settings& s, int base) { return std::llround(base * s.display_rate); }

const char* PhaseName(Phase p) {
    switch (p) {
        case Phase::kIdle: return "idle";
        case Phase::kCharging: return "charging";
        case Phase::kDarken: return "darken";
        case Phase::kShow: return "show";
        case Phase::kLighten: return "lighten";
    }
    return "?";
}

const char* MessageName(Message m) {
    switch (m) {
        case Message::kNone: return "-";
        case Message::kHint: return "hint";
        case Message::kStopFirst: return "stop_first";
        case Message::kParkingBrake: return "parking_brake";
        case Message::kAlreadyClean: return "already_clean";
        case Message::kNoMoney: return "no_money";
        case Message::kChargeFailed: return "charge_failed";
        case Message::kWashed: return "washed";
        case Message::kEndedEarly: return "ended_early";
        case Message::kUnknownStation: return "unknown_station";
    }
    return "?";
}

void Machine::Say(Message m, int price) {
    message_ = m;
    message_price_ = price;
    message_t_ = settings_.message_s;
}

// The dirt goes now, under the darkest screen this wash reached.
void Machine::Lighten(Step* step) {
    step->clean |= units_;
    lighten_from_ = darkness_;
    phase_ = Phase::kLighten;
    t_ = 0;
}

Step Machine::Update(const Input& in) {
    Step step;
    double dt = started_ ? std::min(std::max(in.now_s - last_now_, 0.0), kMaxFrameS) : 0.0;
    last_now_ = in.now_s;
    started_ = true;
    if (in.paused) dt = 0;

    if (message_t_ > 0) {
        message_t_ -= dt;
        if (message_t_ <= 0) message_ = Message::kNone;
    }

    const float speed = std::fabs(in.speed_kmh);
    const bool stopped = speed < settings_.stop_kmh;

    // D4y: the key away from every known station said nothing (the user, 29/10: "F8 did nothing").
    if (phase_ == Phase::kIdle && in.key && !in.in_area && !in.paused) Say(Message::kUnknownStation);
    if (phase_ == Phase::kIdle && in.key && in.in_area && !in.paused) {
        const int price = Price(settings_, in.trailer);
        if (!stopped) {
            Say(Message::kStopFirst);
        } else if (!in.parking_brake) {
            Say(Message::kParkingBrake);
        } else if (in.dirt < settings_.min_dirt) {
            Say(Message::kAlreadyClean);
        } else if (settings_.charge && price > 0 && in.balance_known && in.balance < price) {
            Say(Message::kNoMoney, price);
        } else {
            message_ = Message::kNone;
            message_t_ = 0;
            units_ = 1u | (in.trailer ? 2u : 0u);
            amount_ = settings_.charge ? price : 0;
            ended_early_ = false;
            darkness_ = 0;
            show_ = 0;
            t_ = 0;
            if (amount_ > 0) {
                phase_ = Phase::kCharging;
                step.charge = true;
                step.amount = amount_;
            } else {
                phase_ = Phase::kDarken;
            }
        }
    } else if ((phase_ == Phase::kDarken || phase_ == Phase::kShow) && !in.paused &&
               (speed > settings_.move_kmh || !in.in_area || in.key)) {
        ended_early_ = true;
        Lighten(&step);
    }

    // The frame's time through the phases; what is left of one goes to the
    // next, so the phase ends do not depend on the frame rate.
    double left = dt;
    for (bool more = true; more;) {
        switch (phase_) {
            case Phase::kCharging:
                t_ += left;
                left = 0;
                if (t_ >= settings_.charge_timeout_s) {
                    phase_ = Phase::kIdle;
                    Say(Message::kChargeFailed);
                }
                more = false;
                break;
            case Phase::kDarken:
                if (t_ + left < settings_.darken_s) {
                    t_ += left;
                    darkness_ = static_cast<float>(t_ / settings_.darken_s);
                    more = false;
                } else {
                    left -= settings_.darken_s - t_;
                    darkness_ = 1;
                    phase_ = Phase::kShow;
                    t_ = 0;
                }
                break;
            case Phase::kShow:
                if (t_ + left < settings_.show_s) {
                    t_ += left;
                    show_ = static_cast<float>(t_ / settings_.show_s);
                    more = false;
                } else {
                    left -= settings_.show_s - t_;
                    show_ = 1;
                    Lighten(&step);
                }
                break;
            case Phase::kLighten: {
                // From the darkness reached, at the full fade's speed.
                const double length = settings_.lighten_s * lighten_from_;
                if (t_ + left < length) {
                    t_ += left;
                    darkness_ = static_cast<float>(lighten_from_ - t_ / settings_.lighten_s);
                    more = false;
                } else {
                    darkness_ = 0;
                    phase_ = Phase::kIdle;
                    t_ = 0;
                    Say(ended_early_ ? Message::kEndedEarly : Message::kWashed);
                    more = false;
                }
                break;
            }
            case Phase::kIdle:
                more = false;
                break;
        }
    }

    // D11c (the user, 29/10: "just arriving at the pump the option must show"): the hint as soon as the truck is in
    // the area, moving or not; the key then asks to stop and to pull the parking brake (28/10: the wash only starts
    // with it on).
    hint_ = phase_ == Phase::kIdle && in.in_area && !in.paused;
    hint_price_ = settings_.charge ? Price(settings_, in.trailer) : 0;
    return step;
}

void Machine::Charged(bool ok) {
    if (phase_ != Phase::kCharging) return;  // late answer (timed out): ignored
    t_ = 0;
    if (ok) {
        phase_ = Phase::kDarken;
    } else {
        phase_ = Phase::kIdle;
        Say(Message::kChargeFailed);
    }
}

// ---- L7: spots ----

namespace {

double Horizontal(const Spot& a, const Spot& b) { return std::hypot(a.x - b.x, a.z - b.z); }

// "-18189.25" (a sign, digits, an optional fraction), without the C locale.
bool ParseCoord(const std::string& text, double* out) {
    std::string v = Trim(text);
    const bool minus = !v.empty() && v[0] == '-';
    if (minus || (!v.empty() && v[0] == '+')) v.erase(0, 1);
    if (!ParseNumber(v, out)) return false;
    if (minus) *out = -*out;
    return true;
}

const char* const kSpotsHeader = "truck_dirt_wash_spots 1";

}  // namespace

bool Spots::Near(const Spot& at) const {
    for (const Spot& s : spots_)
        if (Horizontal(s, at) <= settings_.radius_m && std::fabs(s.y - at.y) <= settings_.height_m) return true;
    return false;
}

bool Spots::Add(const Spot& s) {
    if (Near(s)) return false;
    spots_.push_back(s);
    if (settings_.max_spots > 0 && static_cast<int>(spots_.size()) > settings_.max_spots) spots_.erase(spots_.begin());
    return true;
}

// A new stop (or the truck moved in it): watched from here.
void Spots::Watch(const RefuelInput& in) {
    watching_ = true;
    learnt_ = false;
    anchor_ = in.at;
    base_l_ = last_l_ = in.fuel_l;
    rises_ = 0;
}

bool Spots::Update(const RefuelInput& in) {
    if (in.paused) return false;
    const bool stopped = std::fabs(in.speed_kmh) < settings_.stop_kmh && in.parking_brake;
    if (!stopped) {
        watching_ = false;
        return false;
    }
    const double moved = std::hypot(Horizontal(in.at, anchor_), in.at.y - anchor_.y);
    if (!watching_ || moved > settings_.still_m) {
        Watch(in);
        return false;
    }
    const float step = in.fuel_l - last_l_;
    last_l_ = in.fuel_l;
    if (step > settings_.jump_l) {  // not a pump: a new truck or profile; this stop starts again
        Watch(in);
        return false;
    }
    if (step < 0) {
        // The engine burns a little while it waits: the rise counts from the lowest.
        base_l_ = std::min(base_l_, in.fuel_l);
        return false;
    }
    if (step == 0) return false;
    if (rises_++ == 0) first_rise_s_ = in.now_s;
    last_rise_s_ = in.now_s;
    if (learnt_ || in.fuel_l - base_l_ < settings_.refuel_l || rises_ < 3 ||
        last_rise_s_ - first_rise_s_ < settings_.refuel_s)
        return false;
    learnt_ = true;
    return Add(anchor_);
}

std::string Spots::Serialize() const {
    std::string out = std::string(kSpotsHeader) +
                      "\n# Truck Dirt: the service stations where the truck was refuelled (wash at the station, L7).\n"
                      "# One per line: spot x y z (metres, the game's world). Delete a line to forget a station.\n";
    char b[96];
    for (const Spot& s : spots_) {
        snprintf(b, sizeof(b), "spot %.2f %.2f %.2f\n", s.x, s.y, s.z);
        out += b;
    }
    return out;
}

bool Spots::Parse(const std::string& text, std::vector<std::string>* errors) {
    std::vector<Spot> read;
    bool header = false;
    size_t pos = 0;
    for (int line = 1; pos <= text.size(); ++line) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string l = Trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line == 1 && l.compare(0, 3, "\xEF\xBB\xBF") == 0) l.erase(0, 3);
        if (l.empty() || l[0] == '#') continue;
        if (!header) {
            if (l != kSpotsHeader) {
                if (errors) errors->push_back("line " + std::to_string(line) + ": not a wash_spots file (\"" + l + "\")");
                return false;
            }
            header = true;
            continue;
        }
        // "spot x y z"
        std::vector<std::string> f;
        for (size_t a = 0; a < l.size();) {
            const size_t b = l.find_first_of(" \t", a);
            const std::string w = l.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!w.empty()) f.push_back(w);
            if (b == std::string::npos) break;
            a = b + 1;
        }
        Spot s;
        if (f.size() != 4 || f[0] != "spot" || !ParseCoord(f[1], &s.x) || !ParseCoord(f[2], &s.y) ||
            !ParseCoord(f[3], &s.z)) {
            if (errors) errors->push_back("line " + std::to_string(line) + ": expected \"spot x y z\", got \"" + l + "\"");
            continue;
        }
        read.push_back(s);
    }
    if (!header) {
        if (errors) errors->push_back("empty: no header");
        return false;
    }
    spots_.clear();
    for (const Spot& s : read) Add(s);
    return true;
}

View Machine::Current() const {
    View v;
    v.phase = phase_;
    v.darkness = darkness_;
    v.show = phase_ == Phase::kShow || phase_ == Phase::kLighten ? show_ : 0.f;
    if (phase_ == Phase::kIdle) {
        if (message_ != Message::kNone) {
            v.message = message_;
            v.price = message_price_;
        } else if (hint_) {
            v.message = Message::kHint;
            v.price = hint_price_;
        }
    }
    v.display_price = DisplayPrice(settings_, v.price);
    if (phase_ == Phase::kDarken || phase_ == Phase::kShow || phase_ == Phase::kLighten) {
        v.paid = amount_;
        v.paid_display = DisplayPrice(settings_, amount_);
    }
    return v;
}

}  // namespace wash
