// L12 / V6: the low-level keyboard hook of the panel and the menu (see input_hook.h).

#include "input_hook.h"
#include "menu.h"  // HookKeeps

#include <windows.h>

#include <atomic>
#include <mutex>

namespace input_hook {
namespace {

constexpr UINT kInstall = WM_APP + 1, kRemove = WM_APP + 2;
constexpr uint64_t kBeatMs = 500;  // the game thread silent this long: nothing swallowed

std::atomic<int> g_mode{0};
std::atomic<uint64_t> g_beat{0};
std::atomic<bool> g_installed{false}, g_failed{false}, g_want{false};
std::atomic<bool> g_held[256];
std::mutex g_mutex;  // guards g_combos and g_events
std::vector<panel::Hotkey> g_combos;
std::vector<Event> g_events;
HANDLE g_thread = nullptr;
DWORD g_thread_id = 0;
DWORD g_pid = 0;
// The hook thread's own.
HHOOK g_hook = nullptr;
bool g_swallowed[256] = {};
bool g_ctrl[2] = {}, g_shift[2] = {}, g_alt[2] = {};

bool GameFocused() {
    DWORD pid = 0;
    const HWND w = GetForegroundWindow();
    return w && GetWindowThreadProcessId(w, &pid) && pid == g_pid;
}

// Keys never kept from the game or Windows (D11p: the rule in menu::HookKeeps, which the menu also checks).
bool Never(int vk, bool ctrl, bool alt) { return !menu::HookKeeps(vk, ctrl, alt); }

void Push(int vk, bool down) {
    const bool ctrl = g_ctrl[0] || g_ctrl[1], shift = g_shift[0] || g_shift[1], alt = g_alt[0] || g_alt[1];
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_events.size() < 256) g_events.push_back({vk, down, ctrl, shift, alt});
}

LRESULT CALLBACK Proc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
    const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
    const int vk = static_cast<int>(k->vkCode & 0xFF);
    const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    switch (vk) {
        case VK_LCONTROL: g_ctrl[0] = down; break;
        case VK_RCONTROL: g_ctrl[1] = down; break;
        case VK_LSHIFT: g_shift[0] = down; break;
        case VK_RSHIFT: g_shift[1] = down; break;
        case VK_LMENU: g_alt[0] = down; break;
        case VK_RMENU: g_alt[1] = down; break;
        default: break;
    }
    if (!down) {
        // The release of a swallowed press is swallowed too; any other reaches the game.
        if (!g_swallowed[vk]) return CallNextHookEx(nullptr, code, wp, lp);
        g_swallowed[vk] = false;
        g_held[vk] = false;
        Push(vk, false);
        return 1;
    }
    const Mode mode = static_cast<Mode>(g_mode.load(std::memory_order_relaxed));
    const bool ctrl = g_ctrl[0] || g_ctrl[1], shift = g_shift[0] || g_shift[1], alt = g_alt[0] || g_alt[1];
    if (mode == Mode::kOff || GetTickCount64() - g_beat.load(std::memory_order_relaxed) > kBeatMs || !GameFocused() ||
        Never(vk, ctrl, alt))
        return CallNextHookEx(nullptr, code, wp, lp);
    bool take = mode == Mode::kAll || g_swallowed[vk];  // a held key's repeats stay with the plugin
    if (!take) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const panel::Hotkey& h : g_combos)
            if (panel::IsHotkey(h, vk, ctrl, shift, alt)) take = true;
    }
    if (!take) return CallNextHookEx(nullptr, code, wp, lp);
    g_swallowed[vk] = true;
    g_held[vk] = true;
    Push(vk, true);
    return 1;
}

void Clear() {
    for (int i = 0; i < 256; ++i) g_swallowed[i] = false, g_held[i] = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_events.clear();
}

DWORD WINAPI Thread(void* ready) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // the thread's queue, before anyone posts
    SetEvent(static_cast<HANDLE>(ready));
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Proc), &self);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kInstall && !g_hook) {
            // The modifiers held now (the hotkey that opened the menu may still be down).
            g_ctrl[0] = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0, g_ctrl[1] = (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
            g_shift[0] = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0, g_shift[1] = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
            g_alt[0] = (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0, g_alt[1] = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
            Clear();
            g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, Proc, self, 0);
            g_installed = g_hook != nullptr;
            if (!g_hook) g_failed = true;
        } else if (msg.message == kRemove && g_hook) {
            UnhookWindowsHookEx(g_hook);
            g_hook = nullptr;
            g_installed = false;
            Clear();
        }
    }
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    g_installed = false;
    return 0;
}

}  // namespace

bool SetMode(Mode m, const std::vector<panel::Hotkey>& combos) {
    if (g_failed) return false;
    g_beat.store(GetTickCount64(), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_combos = combos;
    }
    g_mode.store(static_cast<int>(m), std::memory_order_relaxed);
    const bool want = m != Mode::kOff;
    if (want && !g_thread) {
        g_pid = GetCurrentProcessId();
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_thread = CreateThread(nullptr, 0, Thread, ready, 0, &g_thread_id);
        if (!g_thread || WaitForSingleObject(ready, 2000) != WAIT_OBJECT_0) g_failed = true;
        if (ready) CloseHandle(ready);
        if (g_failed) return false;
    }
    if (g_thread && want != g_want.load()) {
        g_want = want;
        PostThreadMessageW(g_thread_id, want ? kInstall : kRemove, 0, 0);
    }
    return true;
}

std::vector<Event> Drain() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Event> out;
    out.swap(g_events);
    return out;
}

bool Held(int vk) { return vk >= 0 && vk < 256 && g_held[vk].load(std::memory_order_relaxed); }

bool Active() { return g_installed.load(std::memory_order_relaxed); }

void Stop() {
    g_mode = static_cast<int>(Mode::kOff);
    if (!g_thread) return;
    PostThreadMessageW(g_thread_id, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_want = false;
}

}  // namespace input_hook
