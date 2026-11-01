// P1: offline test of the pre-map's table (src/premap.cpp): parsing and the lookup by index count, vertex count and
// box. The pieces are lines of the VNL 2025's table (tools/premap/premap.py on its DLC): the same count and vertex
// count as an interior piece and an ext_model piece, told apart by the box.
#include <cstdio>

#include "premap.h"

namespace {

int failures = 0;

void Expect(const char* what, int got, int want) {
    printf("%s: %s%s\n", what, premap::KindName(got), got == want ? "" : "  <- FAIL");
    if (got != want) ++failures;
}

}  // namespace

int main() {
    const char* table =
        "truck dirt premap 1\n"
        "name volvo.vnl2025\n"
        "p 144 74 -0.3295 0.6280 1.0416 1.4328 0.9873 1.1031 int vehicle/truck/volvo_vnl2025/interior/model/s74h.pmg#105\n"
        "p 144 74 -0.8811 3.0448 -0.4171 0.8811 3.4041 -0.3556 int_ext vehicle/truck/volvo_vnl2025/interior/external/s74h.pmg#1\n"
        "p 3717 1440 -1.0735 1.3420 -2.9095 1.0716 2.7987 -1.1678 int_ext vehicle/truck/volvo_vnl2025/interior/external/s74h.pmg#19\n"
        "p 1212 366 -0.4865 3.5633 -0.3470 0.4865 3.6794 -0.3134 int_ext vehicle/truck/volvo_vnl2025/interior/external/s74h.pmg#5\n"
        "p 1212 366 -0.4865 3.5633 -0.3470 0.4865 3.6794 -0.3134 ext test/same_box_other_kind.pmg#0\n"
        "p 96 36 -0.8414 3.2542 -0.9671 0.8414 3.2682 -0.9531 ext vehicle/truck/volvo_vnl2025/cabin/test.pmg#0\n"
        "p 12 0 0 0 0 1 1 1 ext no_vertices.pmg#0\n"
        "p 12 4 0 0 0 1 1 1 bogus unknown_kind.pmg#0\n"
        "p 12 4 0 0 0\n";
    const int taken = premap::Parse(table);
    printf("table: %d pieces taken (of 9 lines; 3 not pieces)\n", taken);
    if (taken != 6) ++failures;
    printf("another format: %d\n", premap::Parse("truck dirt premap 2\np 1 1 0 0 0 1 1 1 ext x#0\n"));
    if (premap::Pieces() != 6) ++failures;

    const float ext_lo[3] = {-0.8811f, 3.0448f, -0.4171f}, ext_hi[3] = {0.8811f, 3.4041f, -0.3556f};
    Expect("144/74 at the ext_model's box", premap::Lookup(144, 74, ext_lo, ext_hi), premap::kIntExt);
    const float int_lo[3] = {-0.3295f, 0.6280f, 1.0416f}, int_hi[3] = {1.4328f, 0.9873f, 1.1031f};
    Expect("144/74 at the interior model's box", premap::Lookup(144, 74, int_lo, int_hi), premap::kInt);
    const float near_lo[3] = {-1.0745f, 1.3430f, -2.9085f}, near_hi[3] = {1.0726f, 2.7977f, -1.1688f};
    Expect("3717/1440, 1 mm off", premap::Lookup(3717, 1440, near_lo, near_hi), premap::kIntExt);
    const float off_lo[3] = {-1.0835f, 1.3420f, -2.9095f}, off_hi[3] = {1.0716f, 2.7987f, -1.1678f};
    Expect("3717/1440, 1 cm off", premap::Lookup(3717, 1440, off_lo, off_hi), premap::kNone);
    Expect("3717 with another vertex count", premap::Lookup(3717, 1441, near_lo, near_hi), premap::kNone);
    const float b_lo[3] = {-0.4865f, 3.5633f, -0.3470f}, b_hi[3] = {0.4865f, 3.6794f, -0.3134f};
    Expect("1212/366 matching two kinds", premap::Lookup(1212, 366, b_lo, b_hi), premap::kNone);
    const float c_lo[3] = {-0.8414f, 3.2542f, -0.9671f}, c_hi[3] = {0.8414f, 3.2682f, -0.9531f};
    Expect("96/36, an outside piece", premap::Lookup(96, 36, c_lo, c_hi), premap::kExt);
    // N57c: the JCB 457's cargo piece as the game drew it (0.60 m up, 0.36 m back from the model's box).
    premap::Parse("truck dirt premap 1\n"
                  "p 1182 534 -1.2651 1.0631 -3.5295 1.2414 3.2230 2.0574 cargo vehicle/machine/jcb_457_wheel_loader/cargo/cargo.pmg#3\n"
                  "p 2463 1135 -1.3085 0.6900 -3.8867 1.3271 1.8364 0.5873 cargo jcb#6\n"
                  "p 2463 1135 0 0 0 2.6356 1.1464 4.4740 ext same_size_other_kind#0\n");
    const float j_lo[3] = {-1.265f, 1.663f, -3.167f}, j_hi[3] = {1.241f, 3.823f, 2.420f};
    bool by_size = false;
    Expect("1182/534 moved 0.60 up, 0.36 back", premap::Lookup(1182, 534, j_lo, j_hi, &by_size), premap::kCargo);
    printf("  by size: %d\n", by_size);
    if (!by_size) ++failures;
    const float k_lo[3] = {-1.309f, 1.290f, -3.525f}, k_hi[3] = {1.327f, 2.436f, 0.949f};
    Expect("2463/1135 moved, its size also another kind's", premap::Lookup(2463, 1135, k_lo, k_hi), premap::kNone);
    const float x_lo[3] = {-0.8811f + 0.6f, 3.0448f, -0.4171f}, x_hi[3] = {0.8811f + 0.6f, 3.4041f, -0.3556f};
    Expect("144/74 int_ext's size, moved (only a cargo goes by size)", premap::Lookup(144, 74, x_lo, x_hi), premap::kNone);
    const float m_lo[3] = {-1.3085f, 0.6900f, -3.8867f}, m_hi[3] = {1.3271f, 1.8364f, 0.5873f};
    Expect("2463/1135 at the model's box", premap::Lookup(2463, 1135, m_lo, m_hi), premap::kCargo);
    premap::Clear();
    Expect("after Clear", premap::Lookup(144, 74, ext_lo, ext_hi), premap::kNone);
    printf(failures ? "FAILED (%d)\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
