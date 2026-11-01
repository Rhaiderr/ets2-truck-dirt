#pragma once

// V4: watches the game's map trigger actions, to learn which one the service
// station's fuel marker runs (the wash starts there, D5). Nothing changes in
// the game. From ets2-tire-wear (T3-B2): the game runs every trigger action
// through one dispatcher, Run(action*), with the action's type in
// [action+0x14] (1-31, a jump table; type 6 is the weigh station)
// [verified: disasm of eurotrucks2.exe 1.61.1.9s at 0x9C0D20, amtrucks.exe
// 1.61.3.18s at 0x9C2120; ATS weigh station test, type 6, 26/10].
//
// The detour reads the type and the action's first bytes, always calls the
// game's own function first and only then queues a record; the plugin drains
// the queue once per frame.

#include <cstdint>
#include <string>
#include <vector>

namespace trigger_probe {

constexpr int kBytes = 64;  // the action's first bytes, for the study (0 where unreadable)

struct Action {
    int type = 0;
    double seconds = 0.0;  // steady clock
    uint8_t bytes[kBytes] = {};
};

// Finds the dispatcher by pattern and hooks it (MinHook). Returns a line for
// the log; nothing is hooked when the pattern is not found exactly once.
std::string Start();
void Stop();

// Any thread: the actions run since the last call.
std::vector<Action> Drain();

}  // namespace trigger_probe
