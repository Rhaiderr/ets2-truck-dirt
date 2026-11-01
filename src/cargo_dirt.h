#pragma once
// N57 (the user, 31/10: "my truck can be dirty, my trailer dirty, but I just picked up a new cargo, so there would be no
// reason for it to be dirty too"; only the loads seen on a trailer, a flatbed's or a lowboy's; premise: a new cargo is
// always clean): the cargo's dirt is the trailer's gathered since the cargo was picked up. At a new cargo the
// trailer's levels are kept as its base; the cargo shows what the trailer gathered over it, and a wash that takes the
// trailer under the base takes the base down with it.
#include <string>

#include "dust_shader.h"

namespace cargo {

struct State {
    std::string key;  // the cargo now on the trailer ("" = none seen yet)
    float base_level = 0.f;
    float base_body[dust::kSlots] = {};
    float base_front[dust::kSlots] = {};
};

// The slots that are dirt levels (the others are the road's wetness, the tread's wear, how high a layer reaches:
// taken as the trailer's).
bool IsLevel(int slot);

// One step: the cargo's key ("" = no cargo: the state is kept) and the trailer's dirt as shown -> the cargo's.
// Returns true when the key changed (a new cargo: clean).
bool Step(State* s, const std::string& key, float trailer_level, const dust::Layers& trailer, float* level,
          dust::Layers* out);

std::string Serialize(const State& s);
bool Parse(const std::string& text, State* s);

}  // namespace cargo
