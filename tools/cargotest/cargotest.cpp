// N57: offline test of the cargo's dirt (src/cargo_dirt.cpp): clean at a new cargo, the trailer's dirt gathered since,
// a wash taking the base down, no cargo keeping the state, the saved state read back.
#include <cmath>
#include <cstdio>

#include "cargo_dirt.h"

namespace {

int failures = 0;

void Check(const char* what, float got, float want) {
    const bool ok = std::fabs(got - want) < 1e-4f;
    printf("%s: %.3f%s\n", what, got, ok ? "" : "  <- FAIL");
    if (!ok) ++failures;
}

dust::Layers Trailer(float dust_like, float soot, float wet) {
    dust::Layers l = {};
    l.body[dust::kFilm] = dust_like, l.front[dust::kFilm] = dust_like * 0.5f;
    l.body[dust::kSoot] = soot, l.reach[dust::kSoot] = 0.9f;
    l.body[dust::kWet] = wet;
    return l;
}

}  // namespace

int main() {
    cargo::State s;
    float level = 0.f;
    dust::Layers out;
    bool fresh = cargo::Step(&s, "excavator|quarry|site|1000", 0.6f, Trailer(0.5f, 0.3f, 0.2f), &level, &out);
    printf("picked up on a dirty trailer: new %d\n", fresh);
    Check("  cargo dust", level, 0.f);
    Check("  cargo film", out.body[dust::kFilm], 0.f);
    Check("  cargo soot", out.body[dust::kSoot], 0.f);
    Check("  road wetness (the trailer's)", out.body[dust::kWet], 0.2f);
    Check("  soot reach (the trailer's)", out.reach[dust::kSoot], 0.9f);
    cargo::Step(&s, "excavator|quarry|site|1000", 0.8f, Trailer(0.7f, 0.35f, 0.f), &level, &out);
    printf("a dirt road later:\n");
    Check("  cargo dust", level, 0.2f);
    Check("  cargo film", out.body[dust::kFilm], 0.2f);
    Check("  cargo film, front", out.front[dust::kFilm], 0.1f);
    Check("  cargo soot", out.body[dust::kSoot], 0.05f);
    cargo::Step(&s, "excavator|quarry|site|1000", 0.f, Trailer(0.f, 0.f, 0.f), &level, &out);
    printf("the trailer washed:\n");
    Check("  cargo dust", level, 0.f);
    cargo::Step(&s, "excavator|quarry|site|1000", 0.1f, Trailer(0.1f, 0.f, 0.f), &level, &out);
    Check("  then 0.1 more", level, 0.1f);
    cargo::Step(&s, "", 0.3f, Trailer(0.3f, 0.f, 0.f), &level, &out);
    printf("delivered (no cargo): key kept '%s'\n", s.key.c_str());
    Check("  cargo dust", level, 0.3f);
    const std::string saved = cargo::Serialize(s);
    cargo::State back;
    const bool parsed = cargo::Parse(saved, &back);
    printf("saved and read back: %d, key '%s'\n", parsed, back.key.c_str());
    if (!parsed || back.key != s.key) ++failures;
    Check("  base level", back.base_level, s.base_level);
    fresh = cargo::Step(&back, "pipes|mill|port|2000", 0.3f, Trailer(0.3f, 0.f, 0.f), &level, &out);
    printf("a new cargo: new %d\n", fresh);
    if (!fresh) ++failures;
    Check("  cargo dust", level, 0.f);
    printf("another format: %d\n", cargo::Parse("truckdirt_cargo 2\nkey x\n", &back));
    printf(failures ? "FAILED (%d)\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
