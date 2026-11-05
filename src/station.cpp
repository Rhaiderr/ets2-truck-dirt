// N36: the game's "refuel here" signal (see station.h).

#include "station.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "pattern.h"

namespace station {
namespace {

// A reader of the gameplay mode (the "activation allowed" check): jb; mov rax,[rip+global];
// mov rcx,[rax+mode]; test rcx,rcx; je; mov rax,[rcx]; call [rax+slot]; mov edx,eax (slot 0xE0 in 1.61; the
// virtual slots move between builds).
const char* kGlobalPattern = "72 ?? 48 8B 05 ?? ?? ?? ?? 48 8B 88 ?? ?? 00 00 48 85 C9 74 ?? 48 8B 01 FF 90 ?? ?? 00 00 8B D0";
constexpr size_t kGlobalRel = 5;   // rel32 of "mov rax,[rip+x]"; next instruction at +9
constexpr size_t kGlobalMode = 12;  // disp32 of "mov rcx,[rax+mode]"
// start_activation: cmp [rcx+0x14],0; mov r14,r8; mov edi,edx; mov rsi,rcx; je; mov rax,[rcx]; call [rax+slot]
// (the previous offer ended; 0x198 in 1.61, 0x1A0 in the 2026-11 update); xor r15d,r15d; mov [rsi+0x18],r14.
const char* kStartPattern = "83 79 14 00 4D 8B F0 8B FA 48 8B F1 74 ?? 48 8B 01 FF 90 ?? ?? 00 00 45 33 FF 4C 89 76 18";
// Its switch on the type: lea eax,[rdi-1]; mov rbx,[r9+x]; cmp eax,0x1E; ja; lea rdx,[rip+image]; mov ecx,[rdx+rax*4+table].
const char* kSwitchPattern = "8D 47 FF 49 8B 99 ?? ?? 00 00 83 F8 1E 0F 87 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 8B 8C 82 ?? ?? ?? ??";
constexpr size_t kSwitchImageRel = 22;  // next instruction at +26
constexpr size_t kSwitchTable = 29;
constexpr size_t kSwitchSearch = 0x400;  // bytes of start_activation searched for it
constexpr int kTypes = 31;
// The gas station case: the site's kind 0x23 check, then the gas station HUD's show call.
const char* kGasPattern = "80 78 0A 23 75 ?? 48 8B 86 F0 00 00 00 48 8B 88 70 01 00 00 F3 0F 5C 49 40 48 8B 4B 30 F3 0F 59 C8 E8";
constexpr ptrdiff_t kGasCaseMax = 0x40;  // the pattern starts this close after its case's label at most
constexpr uint32_t kTypeOffset = 0x14;

int ReadType(uintptr_t global, uint32_t mode_off) {
    __try {
        const uintptr_t game = *reinterpret_cast<const uintptr_t*>(global);
        if (!game) return -1;
        const uintptr_t mode = *reinterpret_cast<const uintptr_t*>(game + mode_off);
        if (!mode) return -1;
        return *reinterpret_cast<const int32_t*>(mode + kTypeOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int32_t Int32(const uint8_t* p) {
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

}  // namespace

Located Locate() { return Locate(pattern::GameText()); }

Located Locate(const pattern::Range& text) {
    Located out;
    if (!text.size) {
        out.detail = "no .text section in the game's executable";
        return out;
    }
    const auto global = pattern::Find(text, kGlobalPattern);
    const auto start = pattern::Find(text, kStartPattern);
    const auto gas = pattern::Find(text, kGasPattern);
    if (global.size() != 1 || start.size() != 1 || gas.size() != 1) {
        out.detail = "patterns found: mode reader " + pattern::Count(global.size()) + ", start_activation " +
                     pattern::Count(start.size()) + ", gas station case " + pattern::Count(gas.size()) + " time(s) (need 1 each)";
        return out;
    }
    const uint8_t* g = global[0];
    out.global = reinterpret_cast<uintptr_t>(pattern::Rel32(g + kGlobalRel, g + kGlobalRel + 4));
    out.mode_off = static_cast<uint32_t>(Int32(g + kGlobalMode));
    pattern::Range in_start = text;
    in_start.begin = start[0];
    in_start.size = (std::min)(kSwitchSearch, static_cast<size_t>(text.begin + text.size - start[0]));
    const auto sw = pattern::Find(in_start, kSwitchPattern);
    if (sw.size() != 1) {
        out.detail = "start_activation's switch found " + pattern::Count(sw.size()) + " time(s) (need 1)";
        return out;
    }
    const uint8_t* image = pattern::Rel32(sw[0] + kSwitchImageRel, sw[0] + kSwitchImageRel + 4);
    const uint8_t* table = image + Int32(sw[0] + kSwitchTable);
    if (image != text.base || table < text.base || table + 4 * kTypes > text.begin + text.size) {
        out.detail = "start_activation's switch table not where expected (exe+" + pattern::Hex(table - text.base) + ")";
        return out;
    }
    for (int type = 1; type <= kTypes; ++type) {
        const uint8_t* label = image + Int32(table + 4 * (type - 1));
        const ptrdiff_t d = gas[0] - label;
        if (d >= 0 && d < kGasCaseMax) {
            out.refuel_type = type;
            break;
        }
    }
    if (!out.refuel_type) {
        out.detail = "no type of start_activation's switch leads to the gas station case (exe+" +
                     pattern::Hex(gas[0] - text.base) + ")";
        return out;
    }
    out.ok = true;
    out.detail = "game object at exe+" + pattern::Hex(out.global - reinterpret_cast<uintptr_t>(text.base)) +
                 ", gameplay mode at +" + pattern::Hex(out.mode_off) + ", start_activation's type check at exe+" +
                 pattern::Hex(start[0] - text.base) + ", refuel = activation " + std::to_string(out.refuel_type);
    return out;
}

int Activation(const Located& l) {
    if (!l.ok) return -1;
    return ReadType(l.global, l.mode_off);
}

}  // namespace station
