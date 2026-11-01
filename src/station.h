#pragma once

// N36: the game's own "refuel here" signal, so the wash is offered at any
// service station without a refuel first (the user, 29/10).
//
// The game offers the player one "activation" at a time (the green spot's
// action: service, weigh station, ferry, job, refuel...) through
// local_gameplay_mode_u::start_activation(hud_activation_t type, void* site),
// which stores the type at [mode+0x14] and the site at [mode+0x18];
// end_activation zeroes both. The mode is [[exe+global]+0x2B30]. The type
// whose case shows the gas station HUD (gas_station_hint) is the refuel: 24
// [verified: amtrucks.exe 1.61.3.18s and eurotrucks2.exe 1.61.1.9s, 29/10].
//
// Found by byte patterns (read only, src/pattern.*), each exactly once:
// - the global and the mode's offset, from one of the game's readers of it;
// - start_activation's first bytes (the type at +0x14, the site at +0x18);
// - the gas station case; the refuel type is the switch entry whose case
//   holds it, so a renumbered enum is followed (or refused).
// Reading is a few guarded loads per frame; nothing in the game is changed.

#include <cstdint>
#include <string>

#include "pattern.h"

namespace station {

struct Located {
    bool ok = false;
    uintptr_t global = 0;   // address of the global holding the game object
    uint32_t mode_off = 0;  // the gameplay mode's offset in it
    int refuel_type = 0;    // the activation type of a refuel offer
    std::string detail;     // one line for the log, success or why not
};

Located Locate();
// The same in a given .text (tools/stationtest: the executable mapped offline).
Located Locate(const pattern::Range& text);

// The activation the game offers now: 0 none, -1 unreadable (no profile yet,
// or not located). Read only, any thread.
int Activation(const Located& l);

}  // namespace station
