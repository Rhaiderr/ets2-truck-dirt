#pragma once
// P1: the pre-map of the vehicles' meshes, made from the game's own models by tools/premap/premap.py. Each line is a
// model piece: its index count, its vertex count, the box of its vertices (model space) and what it is. A truck
// mesh read once from the GPU (its indices and vertices) is looked up here by the same three.
//
// [verified: the VNL 2025's .pmg, PMG 0x15: the piece header's box equals its vertices' own; an interior accessory
// has `model` (the interior camera's) and `ext_model` (the interior seen from outside, interior/external/); 19 of
// the 22 pieces of external/s74h.pmg are among the meshes the plugin learned outside; with the box no ext_model
// piece shares its signature with another kind among the DLC's 1611 pieces.]
#include <cstdint>
#include <string>

namespace premap {

enum Kind { kNone = 0, kExt = 1, kInt = 2, kIntExt = 3, kCargo = 4, kKinds = 5 };

// The pieces of one table's text ("truck dirt premap 1", "name ...", "p count verts lo3 hi3 kind ref" lines); returns
// how many were taken (0: not a table).
int Parse(const std::string& text);
// Every *.premap in `dir` (wide path) whose name starts with `prefix` ("" = all); returns the pieces taken.
int LoadDir(const std::wstring& dir, const std::wstring& prefix = L"");
void Clear();
int Pieces();
// The kind of a mesh by its index count, vertex count and box (model space; 3 mm either way), kNone when no piece
// matches; a box matching pieces of two kinds is kNone too. N57c: no piece at that box, a cargo by the box's size
// alone (the load moved as a whole: *by_size), under the same rule.
int Lookup(uint32_t count, uint32_t verts, const float lo[3], const float hi[3], bool* by_size = nullptr);
const char* KindName(int kind);
// N57b: some piece has this index count (whatever its vertices and box): a mesh read with it but matching no piece
// points at the read (the vertex format), not at a missing table.
bool CountKnown(uint32_t count);

}  // namespace premap
