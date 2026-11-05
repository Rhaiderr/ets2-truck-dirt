# ETS2 & ATS Truck Dirt (developers)

*Players: [README.md](README.md) (English), [README.pt-BR.md](README.pt-BR.md) (português).*

A plugin DLL for Euro Truck Simulator 2 and American Truck Simulator (SCS Telemetry SDK + D3D11 hooks): the
player's truck, trailers and cargo get dirty over time, and the kind of dirt follows the ground and the weather.
Nothing in the game's files is changed; the dirt is blended into the game's own G-buffer.

## How it works

- **Ground and weather:** the SDK gives the substance under every wheel. `dirt_model` keeps per unit a set of
  layers (dust, wet and dry mud, road spray and the film it dries into, grit, salt, road soot), grown per km of the
  odometer and per game minute, never per frame. Rain is recognised on the GPU (rain textures, glass drops) or by
  the wipers; the road stays wet for a while after it.
- **Saved dirt** (`dirt_state`): `Documents\<game>\TruckDirt\dirt_state.txt`, one line per player unit, saved on a
  rig change, on pause, every 2 minutes and at shutdown, and kept with the game's saves (`TruckDirt\saves`). Company
  trailers and the quick job's units start clean.
- **Which pixels are the player's** (`rig_tracker`): every draw carries its model->camera matrix. The tracker finds
  the player's truck and trailers per view from the wheel positions of the telemetry, learns each unit's meshes once
  they move with it, follows the cab on its suspension and keeps a map per vehicle layout (`TruckDirt\rigs`) so a
  known rig is recognised at once.
- **Pre-map** (`premap`, `tools/premap`): every stock truck and trailer piece (index count, vertex count, vertex box,
  kind) from the game's `.pmg` models, one table per archive (`ats_*.premap`, `ets2_*.premap`). A truck or trailer
  mesh is read once from the GPU and looked up: the interior seen from outside (an interior accessory's
  `ext_model`) gets no dust; a load (`.../cargo/` models) gets its own dirt.
- **Cargo** (`cargo_dirt`): a new job's load starts clean; its dirt is what the trailer gathered since.
- **Drawing** (`tint`, `dust_shader.h`): player draws write a code in stencil bits 0xf0 (the game uses 0x0f): the
  truck, its cab, each trailer, each trailer's cargo. After the G-buffer pass, one pass per code rebuilds each
  pixel's position from the depth, takes it into the unit's frame and blends the layers into the albedo, specular,
  reflection and normal targets; the bits are cleared and the game's state restored. A self-check (`compat`) turns
  the drawing off when the game's rendering stops matching what it relies on.
- **Wash** (`wash`, `station`, `payment`): at a fuel station the wash key cleans the rig; the price can be charged
  through the game's own payment function, found by byte patterns (read only).
- **Texts** (`lang`): `lang/<code>.txt`, the game's 42 locales, built into the DLL.

## Build (Windows, Visual Studio 2022, CMake 3.20+)

1. Download the **SCS Telemetry SDK** (1.14 or newer) from the [SCS modding wiki](https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry) and extract it so that `third_party\scs_sdk\include\scssdk.h` exists. The headers are not in the repository.
2. In a *Developer PowerShell for VS 2022*:
   ```bash
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   ```
   ```bash
   cmake --build build --config Release
   ```
   The DLL is `build\out\ets2_truck_dirt.dll`; the tools and tests land in `build\out` too.

## Tests

`bash tools/run_tests.sh --bin build/out` (Git Bash) runs the offline tests and compares their output with
`testdata/baseline/`:

| Test | What it checks |
|---|---|
| `rigtest` | the rig tracker on real frame captures (`testdata/rig*`), in many modes (static, swap, turn, cab, LOD, road trains...) |
| `maptest` | the per-vehicle maps: written, read back, meshes moved to other buffers |
| `dusttest` | the dirt model on made-up routes against the plan's targets |
| `statetest` | the saved dirt file: format, damaged files, trailer swaps, company trailers |
| `washtest`, `paneltest`, `langtest`, `compattest` | the wash, the test panel, the texts, the self-check |
| `premaptest`, `cargotest` | the pre-map lookup, the cargo's dirt |

`build\out\probetest.exe` runs the DLL outside the game with a fake SDK and a real D3D11 device (red, dust and
position modes on a fake G-buffer pass). `build\out\dustpreview.exe <dir> [--bench | --ladder]` renders the dirt
shader on a ray-traced truck to PNGs, to judge the look without the game.

## Pre-map tables

The tables are generated from the game's models; the repository has only the tool.

1. Extract the vehicle models of every archive with an SCS archive extractor (HashFS v2), one folder per archive,
   for example `probe/all_ats/<archive>/vehicle/...` (`*.pmg` and `*.pmd`: the descriptors give each variant's
   pieces, drawn as one mesh per material; `probe/` is gitignored: game files never go into git), and the
   truck definitions the same way (`/def/vehicle/truck/*.sii`, e.g. `probe/def_ats`): their `ext_model` paths are the
   interiors seen from outside wherever their folder is.
2. `python -I tools/premap/premap.py --all probe/all_ats probe/premap/ats ats probe/def_ats` (and `ets2` likewise).
   Again after every game update: a model the update changed is no longer recognised.
3. `tools/release/make_release.sh` packs `probe/premap` into the zip.

## Version and release

- The version comes from git at every build (`tools/release/version.cmake`): a tag `v<version>` on HEAD gives
  `<version>`; otherwise `DIRT_VERSION_BASE` from `CMakeLists.txt` plus the commit, with `-dirty` for uncommitted
  changes.
- `tools/release/make_release.sh [--bin build/out]` (Git Bash, Python 3) writes `dist/ets2_truck_dirt-<version>.zip`
  and `dist/ets2-truck-dirt.zip` (the name the README's download link uses): the DLL, `dist/ets2_truck_dirt.ini`
  (the players' defaults), the pre-map tables in `plugins/ets2_truck_dirt_premap/`, both READMEs, `LICENSE` and
  `THIRD_PARTY_LICENSES.txt`.

## Layout

| Folder | Contents |
|---|---|
| `src/` | the plugin: telemetry and settings (`plugin.cpp`), D3D11 hooks and logs (`gpu_probe.cpp`), the dirt pass (`tint.cpp`, `dust_shader.h`), the tracker (`rig_tracker.cpp`), the dirt model and file (`dirt_model.cpp`, `dirt_state.cpp`), pre-map and cargo (`premap.cpp`, `cargo_dirt.cpp`), wash, menus and overlay |
| `tools/` | tests, `probetest`, `dustpreview`, `premap` (tables), `shader_catalog` (the game shaders' checksums), `release` |
| `testdata/` | frame captures for the tracker tests and the tests' baselines |
| `catalog/` | the game's pixel shaders by checksum (effect and pass names only) |
| `lang/` | the texts, one file per game locale |
| `third_party/` | MinHook 1.3.4 (BSD-2), Phosphor icons (MIT) |

## Licence

MIT (`LICENSE`). Third-party notices: `THIRD_PARTY_LICENSES.txt`.
