#!/usr/bin/env bash
# D7: the players' zip, dist/ets2_truck_dirt-<version>.zip, from a DLL already built (MSVC, Release).
#
#   ets2_truck_dirt-<version>/
#     README.md, README.pt-BR.md, LICENSE.txt, THIRD_PARTY_LICENSES.txt
#     plugins/                      everything here goes into <game>\bin\win_x64\plugins
#       ets2_truck_dirt.dll
#       ets2_truck_dirt.ini         dist/ets2_truck_dirt.ini (player defaults), with CRLF for Notepad
# The texts of lang/*.txt are built into the DLL (src/lang_embed.cpp); TruckDirt\lang\<code>.txt overrides one.
#
# The version is the one the build wrote next to the DLL (<bin>/version.txt, tools/release/version.cmake), and
# the DLL must carry it. A "-dirty" version (built with uncommitted changes) is refused unless --allow-dirty.
# The zip and its staging folder stay in dist/ (gitignored); nothing is installed or published.
#
# Usage: tools/release/make_release.sh [--bin build/out] [--allow-dirty]
set -euo pipefail

cd "$(dirname "$0")/../.."

bin=build/out
allow_dirty=0
premap=1
while [ $# -gt 0 ]; do
    case "$1" in
        --bin) bin=$2; shift 2 ;;
        --allow-dirty) allow_dirty=1; shift ;;
        --no-premap) premap=0; shift ;;  # CI: no game files there, so no tables
        *) echo "usage: $0 [--bin <dir>] [--allow-dirty] [--no-premap]" >&2; exit 2 ;;
    esac
done

die() { echo "make_release: $*" >&2; exit 1; }

dll=$bin/ets2_truck_dirt.dll
[ -f "$dll" ] || die "$dll not found: build the Release DLL first (cmake --build build --config Release)"
[ -f "$bin/version.txt" ] || die "$bin/version.txt not found: rebuild with the current CMakeLists.txt"
version=$(tr -d '\r\n' < "$bin/version.txt")
[ -n "$version" ] || die "$bin/version.txt is empty"
case "$version" in
    *-dirty) [ "$allow_dirty" = 1 ] || die "version $version: built with uncommitted changes (--allow-dirty to package it anyway)" ;;
esac
grep -a -q "version $version" "$dll" || die "$dll does not carry version $version (stale build?)"

for f in dist/ets2_truck_dirt.ini README.md README.pt-BR.md LICENSE THIRD_PARTY_LICENSES.txt; do
    [ -f "$f" ] || die "$f missing"
done
# P3: the pre-map tables (tools/premap/premap.py --all on the extracted games, prefixes ats_ and ets2_).
premap_src=${PREMAP_DIR:-probe/premap}
[ "$premap" = 0 ] || ls "$premap_src"/ats/ats_*.premap > /dev/null 2>&1 || die "$premap_src/ats/ats_*.premap missing (tools/premap/premap.py --all)"
[ "$premap" = 0 ] || ls "$premap_src"/ets2/ets2_*.premap > /dev/null 2>&1 || die "$premap_src/ets2/ets2_*.premap missing (tools/premap/premap.py --all)"

py=
for p in python3 python; do
    if command -v "$p" > /dev/null 2>&1 && "$p" -I -c "import zipfile" > /dev/null 2>&1; then py=$p; break; fi
done
[ -n "$py" ] || die "python (3) is needed to write the zip"

name=ets2_truck_dirt-$version
stage=dist/$name
zip=dist/$name.zip
rm -rf "$stage" "$zip"
mkdir -p "$stage/plugins"
cp "$dll" "$stage/plugins/"
if [ "$premap" = 1 ]; then
    mkdir -p "$stage/plugins/ets2_truck_dirt_premap"
    cp "$premap_src"/ats/ats_*.premap "$premap_src"/ets2/ets2_*.premap "$stage/plugins/ets2_truck_dirt_premap/"
else
    echo "make_release: no pre-map tables in this zip (--no-premap): not for players" >&2
fi
sed 's/\r$//; s/$/\r/' dist/ets2_truck_dirt.ini > "$stage/plugins/ets2_truck_dirt.ini"
cp README.md README.pt-BR.md THIRD_PARTY_LICENSES.txt "$stage/"
cp LICENSE "$stage/LICENSE.txt"

# Sorted entries under one top folder, so the same files give the same listing.
"$py" -I - "$stage" "$zip" <<'EOF'
import os, sys, zipfile
stage, out = sys.argv[1], sys.argv[2]
top = os.path.basename(stage)
files = []
for root, _, names in os.walk(stage):
    for n in names:
        full = os.path.join(root, n)
        files.append((os.path.relpath(full, stage).replace(os.sep, "/"), full))
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for rel, full in sorted(files):
        z.write(full, top + "/" + rel)
EOF

(cd "$stage" && find . -type f | LC_ALL=C sort | while read -r f; do
    printf '%-48s %9s  %s\n' "${f#./}" "$(wc -c < "$f")" "$(sha256sum "$f" | cut -c1-16)"
done)
echo "zip: $zip ($(wc -c < "$zip") bytes) sha256 $(sha256sum "$zip" | cut -d' ' -f1)"
# The README's "latest version" link downloads this name from the release (attach it to every release).
cp "$zip" dist/ets2-truck-dirt.zip
echo "release asset: dist/ets2-truck-dirt.zip"
