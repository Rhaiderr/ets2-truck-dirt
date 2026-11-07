// V6: the keys menu (see menu.h).

#include "menu.h"

namespace menu {

const Item kItemList[kItems] = {
    {"wash", "key", "menu.wash_key", "ctrl+f7"},
    {"test", "panel_key", "menu.panel_key", "ctrl+f5"},
};

namespace {

constexpr int kVkReturn = 0x0D, kVkEscape = 0x1B, kVkUp = 0x26, kVkDown = 0x28, kVkDelete = 0x2E;

bool Same(const panel::Hotkey& a, const panel::Hotkey& b) {
    return a.vk && a.vk == b.vk && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt;
}

}  // namespace

bool HookKeeps(int vk, bool ctrl, bool alt) {
    if (IsModifier(vk)) return false;
    switch (vk) {
        case 0x5D: case 0x2C: return false;  // the menu key, Print Screen
        case 0x09: case 0x73: return !alt;   // Alt+Tab, Alt+F4
        case 0x1B: return !(ctrl || alt);    // Ctrl+Esc, Alt+Esc
        case 0x2E: return !(ctrl && alt);    // Ctrl+Alt+Del
        default: return true;
    }
}

bool IsModifier(int vk) {
    switch (vk) {
        case 0x10: case 0x11: case 0x12:          // shift, ctrl, alt
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:  // left/right of each
        case 0x5B: case 0x5C:                     // windows keys
            return true;
        default:
            return false;
    }
}

void Menu::Set(int item, const std::string& ini_value) {
    if (item < 0 || item >= kItems) return;
    panel::Hotkey h;
    const std::string& text = ini_value.empty() ? std::string(kItemList[item].fallback) : ini_value;
    set_[item] = !text.empty() && panel::ParseHotkey(text, &h);
    keys_[item] = set_[item] ? h : panel::Hotkey();
}

panel::Hotkey Menu::Get(int item) const {
    return item >= 0 && item < kItems && set_[item] ? keys_[item] : panel::Hotkey();
}

void Menu::Toggle() {
    if (open_) {
        Close();
        return;
    }
    open_ = true;
    capturing_ = false;
    status_ = Status::kNone;
}

void Menu::Close() {
    open_ = false;
    capturing_ = false;
}

int Menu::Owner(const panel::Hotkey& h, int except) const {
    if (Same(h, menu_key_)) return kItems;
    for (int i = 0; i < kItems; ++i)
        if (i != except && set_[i] && Same(h, keys_[i])) return i;
    return -1;
}

void Menu::Assign(int item, const panel::Hotkey& h, bool set, std::vector<Write>* writes) {
    keys_[item] = set ? h : panel::Hotkey();
    set_[item] = set;
    if (writes) writes->push_back({kItemList[item].section, kItemList[item].key, set ? panel::HotkeyName(h) : ""});
    status_ = Status::kSaved;
}

bool Menu::Key(int vk, bool ctrl, bool shift, bool alt, bool all_keys, std::vector<Write>* writes) {
    if (!open_ || vk <= 0) return false;
    if (capturing_) {
        if (IsModifier(vk)) return false;  // waits for the key itself
        capturing_ = false;
        if (vk == kVkEscape && !ctrl && !shift && !alt) {
            status_ = Status::kCancelled;
            return true;
        }
        const panel::Hotkey h{vk, ctrl, shift, alt};
        panel::Hotkey check;
        if (!panel::ParseHotkey(panel::HotkeyName(h), &check) || !Same(check, h)) {
            status_ = Status::kUnsupported;
            return true;
        }
        const int owner = Owner(h, row_);
        if (owner >= 0) {
            status_ = Status::kTaken;
            status_item_ = owner;
            return true;
        }
        Assign(row_, h, true, writes);
        return true;
    }
    if (panel::IsHotkey(menu_key_, vk, ctrl, shift, alt)) {
        Close();
        return true;
    }
    const bool plain = all_keys && !ctrl && !shift && !alt;
    const bool act = (ctrl && !shift && !alt) || plain;
    if (!act) return false;
    switch (vk) {
        case kVkUp: row_ = (row_ + kItems - 1) % kItems; status_ = Status::kNone; return true;
        case kVkDown: row_ = (row_ + 1) % kItems; status_ = Status::kNone; return true;
        case kVkReturn: capturing_ = true; status_ = Status::kNone; return true;
        case kVkDelete: {
            panel::Hotkey h;
            const bool has = *kItemList[row_].fallback && panel::ParseHotkey(kItemList[row_].fallback, &h);
            const int owner = has ? Owner(h, row_) : -1;
            if (owner >= 0) {
                status_ = Status::kTaken;
                status_item_ = owner;
            } else {
                Assign(row_, h, has, writes);
            }
            return true;
        }
        case kVkEscape:
            if (!plain) return false;
            Close();
            return true;
        default:
            return false;
    }
}

overlay::Menu Menu::MakeView(const lang::Catalog& t, bool all_keys) const {
    overlay::Menu v;
    v.open = open_;
    if (!open_) return v;
    v.title = t.Get("menu.title");
    for (int i = 0; i < kItems; ++i) {
        overlay::MenuRow r;
        r.label = t.Get(kItemList[i].label);
        r.selected = i == row_;
        r.editing = capturing_ && i == row_;
        r.value = r.editing ? t.Format("menu.press_key", {{"key", "Esc"}})
                            : set_[i] ? panel::HotkeyLabel(keys_[i]) : t.Get("menu.not_set");
        v.rows.push_back(r);
    }
    switch (status_) {
        case Status::kSaved: v.status = t.Get("menu.saved"); break;
        case Status::kTaken:
            v.status = t.Format("menu.taken", {{"name", status_item_ >= kItems ? t.Get("menu.title")
                                                                               : t.Get(kItemList[status_item_].label)}});
            break;
        case Status::kUnsupported: v.status = t.Get("menu.unsupported"); break;
        case Status::kCancelled:
        case Status::kNone: break;
    }
    const std::string c = all_keys ? "" : "Ctrl+";
    v.help.push_back(t.Format("menu.help", {{"move", c + "Up/Down"},
                                            {"edit", c + "Enter"},
                                            {"default", c + "Del"},
                                            {"close", panel::HotkeyLabel(menu_key_) + (all_keys ? ", Esc" : "")}}));
    return v;
}

}  // namespace menu
