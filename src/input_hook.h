// L12 / V6: the plugin's keys kept from the game while its panel or menu is
// open, by a low-level keyboard hook (WH_KEYBOARD_LL).
//
// Why this hook: the game reads the keyboard with DirectInput [verified: the
// tire plugin's hooks, game.log], so a hook of the game's window (WM_KEYDOWN)
// would not keep a key from it, and registering raw input in the game's
// process would replace the game's own registration. A low-level hook sees
// every key before it is delivered to the programs, and a key it swallows
// is not delivered [estimate: from the Windows documentation; that it keeps
// the key from the game's DirectInput could not be measured here, the
// sandbox gets no injected keys even without a hook: the game test says].
//
// Safety:
// - installed only while the panel or the menu is open (its own thread, with
//   a message loop), removed when both close and at shutdown;
// - swallows only while the game's window has the focus and the game thread
//   is alive (it calls SetMode every frame; 0.5 s without it: nothing is
//   swallowed, so a frozen game never eats the keyboard);
// - never swallows Ctrl, Shift, Alt, the Windows keys, Alt+Tab, Alt+F4,
//   Ctrl+Alt+Del, nor the release of a key whose press reached the game (no
//   key stuck down in the game);
// - Windows itself skips a hook that does not answer in time.
#pragma once

#include <vector>

#include "test_panel.h"

namespace input_hook {

enum class Mode {
    kOff,     // no hook
    kCombos,  // swallow only the given hotkeys (the panel's keys)
    kAll,     // swallow every key but the ones above (the menu)
};

// A key the hook swallowed, with the modifiers held then.
struct Event {
    int vk = 0;
    bool down = false;
    bool ctrl = false, shift = false, alt = false;
};

// Game thread, every frame: what to swallow now (the hook is installed or
// removed as needed). False when the hook could not be installed (the keys
// reach the game then; the caller polls them itself).
bool SetMode(Mode m, const std::vector<panel::Hotkey>& combos = {});
// The keys swallowed since the last call (game thread).
std::vector<Event> Drain();
// A swallowed key still held down.
bool Held(int vk);
// The hook is installed now.
bool Active();
// Shutdown: the hook removed and its thread ended.
void Stop();

}  // namespace input_hook
