#pragma once

// L7: the game's own payment function, to charge the wash at the station.
// The same approach as ets2-tire-wear (T3, src/payment.*): found by byte
// patterns in the game's executable (read only, src/pattern.*):
//
//   bool Pay(Economy* economy, int64_t amount, bool commit)   // rcx, rdx, r8b
//
// economy null -> false; amount > 0 with less money in the bank -> the game
// shows its own "not enough money" message and returns false; commit = 0 only
// checks; commit = 1 takes a positive amount and *deposits* a negative one
// [verified: disasm of eurotrucks2.exe 1.61.1.9s and amtrucks.exe 1.61.3.18s,
// tire plugin T3-M1]. The economy object is a global pointer in the executable.
// Called once by the tire plugin, EUR 1 in ETS2 [verified: the user, T3-M3,
// 25/10/2026]; never in ATS, and the charge after a save and reload was not
// checked. So the wash charges only with [wash] charge = 1 (off by default).

#include <cstdint>
#include <string>

namespace payment {

struct Located {
    bool ok = false;
    uintptr_t pay = 0;           // address of Pay in the running game
    uintptr_t economy_slot = 0;  // address of the global holding Economy*
    std::string detail;          // one line for the log, success or why not
};

// Scans the game's .text section with two patterns that must agree on Pay:
// the function itself and one of the game's own callers (which also gives
// the economy global). Any pattern found 0 or 2+ times -> not ok.
Located Locate();

// Reads the economy global (null until a profile is loaded). Read only.
uintptr_t Economy(const Located& l);

// The bank balance Pay compares against ([[economy+0x10]+0x10]), or -1 when
// there is no economy yet. Read only.
int64_t Balance(const Located& l);

enum class Result { kTaken, kRefused, kUnavailable };

// Game thread only. Takes `amount` (1 .. kMaxAmount, whole units of the
// game's currency) from the bank through the game's own Pay(economy, amount,
// commit = 1). kRefused: the game said no (not enough money; it shows its own
// message). kUnavailable: not located, no economy yet or a bad amount; nothing
// called. Any exception inside the call is kUnavailable too.
constexpr int64_t kMaxAmount = 1000000;
Result Take(const Located& l, int64_t amount);

}  // namespace payment
