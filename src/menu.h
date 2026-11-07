// V6: the player's keys menu (Ctrl+F8 by default): the wash key and the test
// panel's key (the glass washer's went with the glass code to the vidro-futuro
// branch, N40).
//
// Portable (no windows.h; tools/paneltest runs it offline): the menu's state,
// what a key press does to it and the INI lines to write. The DLL reads the
// keys (the keyboard hook, or polling when it is off) and writes the INI.
//
// - Rows: one per Item. While the menu is open: Ctrl+Up / Ctrl+Down choose a
//   row, Ctrl+Enter waits for a new key, Ctrl+Del gives the row its default,
//   the menu key closes it. With the game's input blocked (the hook swallows
//   every key) the plain keys work too, and Esc closes.
// - Waiting for a key: the first key with its modifiers (Ctrl, Shift, Alt) is
//   the new hotkey; Esc cancels; a modifier alone waits on. A key without a
//   name in the INI syntax (panel::ParseHotkey), the menu's own key or a key of
//   another row is refused, with the reason on the status line.
#pragma once

#include <string>
#include <vector>

#include "lang.h"
#include "overlay.h"
#include "test_panel.h"

namespace menu {

struct Item {
    const char* section;    // the INI section
    const char* key;        // the INI key
    const char* label;      // the text's key in lang/
    const char* fallback;   // the default hotkey ("" = none)
};
constexpr int kItems = 2;
extern const Item kItemList[kItems];  // wash.key, test.panel_key

// An INI line to write.
struct Write {
    std::string section, key, value;
};

enum class Status { kNone, kSaved, kTaken, kUnsupported, kCancelled };

class Menu {
public:
    // The row's value as read from the INI ("" = not set: its default, if any).
    void Set(int item, const std::string& ini_value);
    void SetMenuKey(const panel::Hotkey& h) { menu_key_ = h; }
    const panel::Hotkey& MenuKey() const { return menu_key_; }
    bool Open() const { return open_; }
    void Toggle();
    void Close();
    bool Capturing() const { return capturing_; }
    int Row() const { return row_; }
    // The row's hotkey (vk 0 = none).
    panel::Hotkey Get(int item) const;
    // A key went down while the menu is open (vk + the modifiers held);
    // `all_keys`: the game gets no key now, so the plain keys act too.
    // Changes go to *writes. Returns true when the menu changed.
    bool Key(int vk, bool ctrl, bool shift, bool alt, bool all_keys, std::vector<Write>* writes);
    Status LastStatus() const { return status_; }
    overlay::Menu MakeView(const lang::Catalog& t, bool all_keys) const;

private:
    void Assign(int item, const panel::Hotkey& h, bool set, std::vector<Write>* writes);
    // The row whose key `h` is (-1: none), other than `except`.
    int Owner(const panel::Hotkey& h, int except) const;

    bool open_ = false;
    bool capturing_ = false;
    int row_ = 0;
    panel::Hotkey keys_[kItems];
    bool set_[kItems] = {};
    panel::Hotkey menu_key_{0x76, false, true, false};  // shift+f7
    Status status_ = Status::kNone;
    int status_item_ = -1;  // kTaken: the row that has the key (kItems: the menu itself)
};

// True for Ctrl, Shift, Alt (either side) and the Windows keys: never a hotkey alone.
bool IsModifier(int vk);
// D11p: a key the keyboard hook keeps from the game while the menu is open: all but the modifiers, the Windows and
// menu keys, Print Screen, and Alt+Tab, Alt+F4, Ctrl/Alt+Esc, Ctrl+Alt+Del (input_hook's rule). Such a key seen down
// by the game (polled) means the hook is not getting the keys.
bool HookKeeps(int vk, bool ctrl, bool alt);

}  // namespace menu
