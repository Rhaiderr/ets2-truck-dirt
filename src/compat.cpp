// N11: self-check of the render assumptions (see compat.h).

#include "compat.h"

#include <algorithm>
#include <cstdio>

namespace compat {

StencilWrites WritesOf(const StencilDesc& d) {
    StencilWrites w;
    if (!d.enable) return w;
    for (int op : d.ops) {
        switch (op) {
            case kOpReplace:
                w.replace = d.write_mask;
                break;
            case kOpIncrSat:
            case kOpDecrSat:
            case kOpInvert:
            case kOpIncr:
            case kOpDecr:
                w.any = d.write_mask;  // any value under the mask
                break;
            default:  // keep, zero: never sets a bit
                break;
        }
    }
    return w;
}

std::string OpsText(const StencilDesc& d) {
    static const char* const kNames[] = {"?", "keep", "zero", "replace", "incr_sat", "decr_sat", "invert", "incr", "decr"};
    std::string s;
    for (int i = 0; i < 6; ++i) {
        const int op = d.ops[i] >= kOpKeep && d.ops[i] <= kOpDecr ? d.ops[i] : 0;
        s += i == 3 ? " " : i ? "/" : "";
        s += kNames[op];
    }
    return s;
}

namespace {

// Float sums of frame times land a hair under a whole number of seconds: no extra frame for that.
constexpr double kEps = 1e-6;

std::string Fmt(const char* f, double a, double b = 0, double c = 0, double d = 0) {
    char buf[256];
    snprintf(buf, sizeof(buf), f, a, b, c, d);
    return buf;
}

}  // namespace

void Checker::Line(double now, const std::string& text) {
    char t[32];
    snprintf(t, sizeof(t), "compat %.1f s: ", now);
    lines_.push_back(t + text);
}

void Checker::Note(const std::string& text) {
    if (std::find(notes_.begin(), notes_.end(), text) == notes_.end()) notes_.push_back(text);
}

void Checker::Decide(double now, const std::string& what, const std::string& off_text, bool* off, std::string* reason) {
    *reason = what;
    *off = enforce_;
    Line(now, what + (enforce_ ? " -> " + off_text : " -> would turn the " + off_text.substr(0, off_text.find(" off")) +
                                                         " off (enforce = 0: only logged)"));
}

void Checker::StencilConflict(double now, const std::string& state, uint8_t mask, uint8_t ref, uint8_t bits) {
    ++stencil_states_;
    char b[300];
    snprintf(b, sizeof(b),
             "stencil: game state %s writes bits 0x%02x of ours (0x%02x): mask 0x%02x, ref 0x%02x (measured 0x%02x, "
             "limit 0x00)",
             state.c_str(), bits, kOurBits, mask, ref, bits);
    if (drawing_reason_.empty()) {
        Decide(now, b, "drawing off for the session", &drawing_off_, &drawing_reason_);
    } else if (stencil_states_ <= 3) {  // already decided: a few more states for the record
        Line(now, std::string(b) + " -> already decided");
    }
}

void Checker::GBufferMismatch(double now, const std::string& what) {
    if (mismatches_ >= lim_.max_mismatch_lines) return;
    ++mismatches_;
    Line(now, "g-buffer: a pass with 4+ targets is not taken as the G-buffer: " + what +
                  " (want rt0-2 R16G16B16A16_FLOAT, rt3 R16G16B16A16_UINT, depth with stencil) -> log only");
    if (mismatches_ == 1) mismatch_ = what;  // a warning in the summary only if no G-buffer is ever seen
}

void Checker::EndFrame(double now, const Frame& f) {
    double dt = last_ < 0.0 ? 0.0 : now - last_;
    last_ = now;
    dt = std::min(std::max(dt, 0.0), lim_.max_dt_s);
    if (f.paused) return;  // menu, loading: nothing counts
    if (f.views > 0) ++frames_with_gbuffer_;

    // 2. no G-buffer with the truck in the game.
    if (f.views > 0) {
        if (no_gbuffer_warned_)
            Line(now, Fmt("g-buffer: G-buffer pass seen again after %.1f s without it", no_gbuffer_));
        no_gbuffer_ = 0.0;
        no_gbuffer_warned_ = false;
    } else if (f.unit_present[0]) {
        no_gbuffer_ += dt;
        if (!no_gbuffer_warned_ && no_gbuffer_ >= lim_.no_gbuffer_s - kEps) {
            no_gbuffer_warned_ = true;
            Line(now, Fmt("g-buffer: no G-buffer pass for %.1f s with the game running and the truck in it (limit %.0f s): "
                          "nothing is drawn -> log only",
                          no_gbuffer_, lim_.no_gbuffer_s));
            Note(Fmt("no G-buffer pass for %.0f s", lim_.no_gbuffer_s));
        }
    }

    Matrix(now, f, dt);

    // 4. parts: only while the G-buffer is drawn (otherwise nothing could be recognised).
    for (int u = 0; u < kUnits; ++u) {
        const char* name = u ? "trailer" : "truck";
        if (!f.unit_present[u] || f.views == 0) continue;
        if (f.unit_recognised[u]) {
            if (parts_warned_[u])
                Line(now, std::string("parts: ") + name + Fmt(" recognised again after %.1f s", unseen_[u]));
            unseen_[u] = 0.0;
            parts_warned_[u] = false;
            continue;
        }
        unseen_[u] += dt;
        if (!parts_warned_[u] && unseen_[u] >= lim_.parts_s - kEps) {
            parts_warned_[u] = true;
            if (parts_lines_[u]++ < lim_.max_repeat_lines)
                Line(now, std::string("parts: ") + name +
                              Fmt(" in the game, G-buffer drawn, none of its parts recognised for %.1f s (limit %.0f s): "
                                  "an unusual model (mod)? It gets no dust -> log only",
                                  unseen_[u], lim_.parts_s));
            Note(std::string(name) + Fmt(": no part recognised for %.0f s", lim_.parts_s));
        }
    }

    // 5. rain.
    if (f.wipers && f.rain) {
        if (rain_warned_) Line(now, "rain: rain seen on the GPU again (with the wipers on)");
        rain_warned_ = false;
        dry_wipers_ = 0.0;
    } else if (f.wipers) {
        dry_wipers_ += dt;
        if (!rain_warned_ && dry_wipers_ >= lim_.rain_s - kEps) {
            rain_warned_ = true;
            if (rain_lines_++ < lim_.max_repeat_lines)
                Line(now, Fmt("rain: wipers on for %.1f s and no rain seen on the GPU (limit %.0f s): the rain now comes "
                              "from the wipers alone (an update or weather mod changed the rain, or the wipers run in dry "
                              "weather) -> log only",
                              dry_wipers_, lim_.rain_s));
            Note(Fmt("rain from the wipers alone (wipers on %.0f s, no rain on the GPU)", lim_.rain_s));
        }
    } else {
        dry_wipers_ = 0.0;  // the count is for one continuous wiper run
    }

    Interior(now, f, dt);
}

void Checker::Matrix(double now, const Frame& f, double dt) {
    if (f.views == 0) return;
    matrix_t_ += dt;
    matrix_draws_ += f.draws;
    matrix_bad_ += f.not_matrix;
    if (matrix_t_ < lim_.matrix_window_s - kEps) return;
    const long long draws = matrix_draws_, bad = matrix_bad_;
    matrix_t_ -= lim_.matrix_window_s;  // the frame's overshoot goes to the next window: no drift with the FPS
    matrix_draws_ = matrix_bad_ = 0;
    if (draws < lim_.matrix_min_draws) return;  // too few draws: no verdict, the run of bad windows stays
    const double share = double(bad) / double(draws);
    last_share_ = share;
    max_share_ = std::max(max_share_, share);
    if (share <= lim_.matrix_max_share) {
        bad_windows_ = 0;
        return;
    }
    ++bad_windows_;
    if (bad_windows_ < lim_.matrix_bad_windows || !drawing_reason_.empty()) return;
    char b[300];
    snprintf(b, sizeof(b),
             "matrix: %.1f%% of the G-buffer draws have no model->camera matrix in VS cb0 (%lld of %lld in the last "
             "%.0f s, %d windows in a row; limit %.0f%%, normal ~0.4%%)",
             share * 100.0, bad, draws, lim_.matrix_window_s, bad_windows_, lim_.matrix_max_share * 100.0);
    Decide(now, b, "drawing off for the session", &drawing_off_, &drawing_reason_);
}

void Checker::Interior(double now, const Frame& f, double dt) {
    if (f.views < 2) return;  // not the interior camera
    ++interior_frames_;
    if (f.ref2 > 0) {
        if (!interior_reason_.empty()) {
            Line(now, Fmt("interior: %.0f ref-2 draws in the interior camera again", double(f.ref2)) +
                          (enforce_ ? " -> interior dust back on" : " -> would turn the interior dust back on"));
            interior_reason_.clear();
            interior_off_ = false;
        }
        interior_t_ = 0.0;
        return;
    }
    interior_t_ += dt;
    if (!interior_reason_.empty() || interior_t_ < lim_.interior_s - kEps) return;
    ++interior_offs_;
    Decide(now,
           Fmt("interior: no ref-2 draw (the cabin seen from inside) in %.1f s of interior camera (measured 0, limit "
               "> 0 in %.0f s; normal ~86 per frame)",
               interior_t_, lim_.interior_s),
           "interior dust off (back on when ref 2 returns)", &interior_off_, &interior_reason_);
}

std::vector<std::string> Checker::TakeLines() {
    std::vector<std::string> out;
    out.swap(lines_);
    return out;
}

std::string Checker::StatsText() const {
    std::string s = drawing_off_ ? "DRAWING OFF" : !drawing_reason_.empty() ? "drawing would be off" : "drawing ok";
    s += interior_off_ ? ", INTERIOR OFF" : !interior_reason_.empty() ? ", interior would be off" : "";
    s += last_share_ < 0 ? ", no matrix -" : Fmt(", no matrix %.2f%% (max %.2f%%)", last_share_ * 100.0, max_share_ * 100.0);
    if (!notes_.empty()) s += Fmt(", %.0f warning(s)", double(notes_.size()));
    if (!enforce_) s += " [enforce 0]";
    return s;
}

std::string Checker::Summary() const {
    std::vector<std::string> off;
    if (!drawing_reason_.empty()) off.push_back("drawing (" + drawing_reason_ + ")");
    if (!interior_reason_.empty()) off.push_back("interior dust (" + interior_reason_ + ")");
    else if (interior_offs_ > 0) off.push_back(Fmt("interior dust %.0f time(s), back on later (ref 2 seen again)", interior_offs_));
    std::vector<std::string> notes = notes_;
    if (!mismatch_.empty() && frames_with_gbuffer_ == 0)
        notes.push_back("no G-buffer seen; a 4+ target pass did not match (" + mismatch_ + ")");
    if (off.empty() && notes.empty()) return enforce_ ? "compatibility: all ok" : "compatibility: all ok (enforce = 0)";
    std::string s = "compatibility:";
    if (!off.empty()) {
        s += enforce_ ? " turned off: " : " would have turned off (enforce = 0, nothing was): ";
        for (size_t i = 0; i < off.size(); ++i) s += (i ? "; " : "") + off[i];
    } else {
        s += " nothing turned off";
    }
    if (!notes.empty()) {
        s += " | warnings: ";
        for (size_t i = 0; i < notes.size(); ++i) s += (i ? "; " : "") + notes[i];
    }
    return s;
}

}  // namespace compat
