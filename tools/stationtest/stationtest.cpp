// N36: station::Locate against the games' executables, offline (each mapped as an image, nothing run):
//   stationtest <eurotrucks2.exe or amtrucks.exe>...
// Prints what the plugin would log at start; exit code 1 when one is not found.
#include <windows.h>

#include <cstdio>

#include "station.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: stationtest <game exe>...\n");
        return 2;
    }
    int bad = 0;
    for (int i = 1; i < argc; ++i) {
        HMODULE m = LoadLibraryExA(argv[i], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!m) {
            printf("%s: not mapped (error %lu)\n", argv[i], GetLastError());
            ++bad;
            continue;
        }
        // An image resource handle has its low bits set; the mapping itself starts at the rounded address.
        const void* base = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(m) & ~uintptr_t(3));
        const station::Located l = station::Locate(pattern::TextOf(base));
        printf("%s: %s, %s\n", argv[i], l.ok ? "found" : "NOT found", l.detail.c_str());
        if (!l.ok) ++bad;
        FreeLibrary(m);
    }
    return bad ? 1 : 0;
}
