# Test data

- `rig/run{1,2}_capture_NNN.csv`: 28 frame captures from the V3a test (ETS2, 25/10/2026), exported by
  `tools/v3a/export_rig.py`. Each row is one G-buffer draw: view, vertex/index buffer ids, model->camera
  matrix and stencil ref, plus the telemetry placement, wheel layout and deflection. No game files are
  included (no meshes, textures or shaders). Input for `rigtest`.
- `rig_dolly/ats_d4m_capture_NNN.csv`: 3 captures from the D4m test (ATS, 28/10/2026): Western Star 57X, grain
  hopper, dolly and a second grain hopper, exported the same way with a `next` line (the dolly's hook and
  placement). Input for `rigtest` (N24: the first hopper's box ends at the dolly's hook).
- `rig_road/ats_d4s_capture_NNN.csv`: 2 captures from the D4s test (ATS, 28/10/2026), the same double grain hopper
  exported as four units (`unit,2` the dolly, `unit,3` the second hopper, from the capture's `# trailer.N` lines):
  capture 4 the exterior camera, capture 7 the interior one with its three mirrors. Input for `rigtest` (N21).
- `baseline/`: output of the MSVC build at `fb3b37c` (LF line endings), with the changes explained in
  `the development notes` (N8/P5: the `missed` counts, lower since empty member lists are no longer created;
  C10: the trailer's front parts claimed by the trailer instead of the truck, no draw learnt by both
  units, `handed` and `shared` on the last line of each capture, the new meshes of `RIGTEST_AGE` over the
  truck's front axle; N13: those new meshes join in their first frame as another LOD, `inherited` in that
  line; N14: they are promoted on their second frame again, as before N13 (the rig moves; inheritance needs
  two sightings), and in `RIGTEST_LOD` the new meshes are claimed from their third frame):
  - `rigtest_<mode>.txt`: `rigtest rig/*.csv` with no mode, `RIGTEST_STATIC=1`, `RIGTEST_HOLD=1`,
    `RIGTEST_SWAP=1`, `RIGTEST_TURN=20` and `RIGTEST_CAB=1`;
  - later rigtest modes, from the gcc/clang build (checked against MSVC by the CI):
    `rigtest_resend.txt` (`RIGTEST_RESEND=1`, N8/P4), `rigtest_search.txt` (`RIGTEST_SEARCH=1`, N8/P6: the
    time per frame goes to stderr and is not part of the baseline), `rigtest_tip.txt` (`RIGTEST_TIP=1`, N8),
    `rigtest_age.txt` (`RIGTEST_AGE=1`, N8), `rigtest_artic.txt` (`RIGTEST_ARTIC=1`, C10),
    `rigtest_lod.txt` (`RIGTEST_LOD=1`, N13), `rigtest_defl.txt` (`RIGTEST_DEFL=1`, N13),
    `rigtest_lodcab.txt` (`RIGTEST_LODCAB=1`, N14), `rigtest_churn.txt` (`RIGTEST_CHURN=1`, N14: the time
    per frame goes to stderr and is not part of the baseline), `rigtest_grass.txt` (`RIGTEST_GRASS=1`,
    N17) and `rigtest_articfirst.txt` (`RIGTEST_ARTIC=first`, N18), the last two from the MSVC 2022 build of
    the local session (the CI checks them with gcc/clang); N18 also added the trailer's frame error to the
    `rigtest_artic.txt` line;
  - `dusttest.txt` (the D4 lines after the D3 ones came with N4, from g++/clang++, the saved reach with
    N5, the wheel constants with N12, the level scale and the deflected wheel with N13; the D3 lines are
    unchanged);
  - `statetest.txt` (N6, added later: output of the gcc/clang build, checked against MSVC by the CI). Its
    files go to a temporary folder (`statetest <dir>` writes to `<dir>/statetest_files`).
  - `washtest.txt` (N7, added later: output of the gcc/clang build, checked against MSVC by the CI; the
    clock is injected, so it does not depend on the machine).
  - `langtest.txt` (N9, added later: `langtest lang`, output of the gcc/clang build, checked against MSVC
    by the CI; UTF-8, with the hint of every language filled in).
  - `compattest.txt` (N11, added later: output of the gcc/clang build, checked against MSVC by the CI; the
    clock is injected, so it does not depend on the machine or the FPS).

  - `rigtest_dolly.txt` (N24, added later: `rigtest rig_dolly/*.csv`, from the MSVC 2022 build of the local
    session, checked by the CI with gcc/clang).

  - `rigtest_road.txt` (N21, added later: `rigtest rig_road/*.csv`, MSVC 2022 build of the local session).

  Compare with `diff --strip-trailing-cr`.

The raw captures (`capture_NNN.txt`, about 80 MB) stay on the Windows PC (`probe/`, not in git).
