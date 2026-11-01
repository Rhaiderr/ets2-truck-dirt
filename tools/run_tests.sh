#!/usr/bin/env bash
# Builds the portable tests (rigtest, maptest, dusttest, statetest, washtest, paneltest, langtest, compattest) and compares their output
# with testdata/baseline/. Runs the rigtest modes of the baseline (the six of fb3b37c and the later ones, N13's
# lod and defl, N14's lodcab and churn and N17's grass among them),
# dusttest, statetest, washtest, paneltest, langtest (on lang/) and compattest; any difference or failing test fails
# the script.
#
# Usage:
#   tools/run_tests.sh [compiler...]   build with each C++ compiler (default: g++ and clang++, those found)
#                                      in build/tests-<compiler>, with -Wall -Wextra -Werror, then test
#   tools/run_tests.sh --bin <dir>     only test the binaries already in <dir> (e.g. build/out of the MSVC
#                                      build, from Git Bash)
set -u

cd "$(dirname "$0")/.."
export LC_ALL=C  # capture order run1_001 ... run2_013, as in the baseline

MODES="normal:
static:RIGTEST_STATIC=1
hold:RIGTEST_HOLD=1
swap:RIGTEST_SWAP=1
turn20:RIGTEST_TURN=20
cab:RIGTEST_CAB=1
resend:RIGTEST_RESEND=1
search:RIGTEST_SEARCH=1
tip:RIGTEST_TIP=1
age:RIGTEST_AGE=1
artic:RIGTEST_ARTIC=1
articfirst:RIGTEST_ARTIC=first
lod:RIGTEST_LOD=1
defl:RIGTEST_DEFL=1
lodcab:RIGTEST_LODCAB=1
churn:RIGTEST_CHURN=1
grass:RIGTEST_GRASS=1"

failures=0

# Prints the path of tool $2 in directory $1 (with or without .exe).
find_bin() {
    if [ -x "$1/$2" ]; then echo "$1/$2"; elif [ -x "$1/$2.exe" ]; then echo "$1/$2.exe"; fi
}

# Compares output $2 with testdata/baseline/$1.txt; $3 is the test's exit code.
check() {
    local name=$1 out=$2 code=$3
    if [ "$code" -ne 0 ]; then
        echo "FAIL $name: exit code $code"
        failures=$((failures + 1))
    elif ! diff --strip-trailing-cr -u "testdata/baseline/$name.txt" "$out" > "$out.diff"; then
        echo "FAIL $name: differs from testdata/baseline/$name.txt ($(grep -c '^[-+][^-+]' "$out.diff") lines)"
        head -n 20 "$out.diff"
        failures=$((failures + 1))
    else
        echo "ok   $name"
    fi
}

# Runs every test with the binaries in $1, writing the outputs to $2.
run_all() {
    local bin=$1 out=$2 rigtest maptest dusttest statetest washtest paneltest langtest compattest premaptest cargotest pairs f
    rigtest=$(find_bin "$bin" rigtest)
    maptest=$(find_bin "$bin" maptest)
    dusttest=$(find_bin "$bin" dusttest)
    statetest=$(find_bin "$bin" statetest)
    washtest=$(find_bin "$bin" washtest)
    paneltest=$(find_bin "$bin" paneltest)
    langtest=$(find_bin "$bin" langtest)
    compattest=$(find_bin "$bin" compattest)
    premaptest=$(find_bin "$bin" premaptest)
    cargotest=$(find_bin "$bin" cargotest)
    if [ -z "$rigtest" ] || [ -z "$maptest" ] || [ -z "$dusttest" ] || [ -z "$statetest" ] || [ -z "$washtest" ] || [ -z "$paneltest" ] ||
       [ -z "$langtest" ] || [ -z "$compattest" ] || [ -z "$premaptest" ] || [ -z "$cargotest" ]; then
        echo "FAIL: rigtest, maptest, dusttest, statetest, washtest, paneltest, langtest, compattest, premaptest or cargotest not found in $bin"
        failures=$((failures + 1))
        return
    fi
    mkdir -p "$out"
    while IFS=: read -r mode var; do
        if [ -n "$var" ]; then
            env "$var" "$rigtest" testdata/rig/*.csv > "$out/rigtest_$mode.txt"
        else
            "$rigtest" testdata/rig/*.csv > "$out/rigtest_$mode.txt"
        fi
        check "rigtest_$mode" "$out/rigtest_$mode.txt" $?
    done <<< "$MODES"
    # N24: the double grain hopper of the D4m ATS test, a dolly hooked behind the first trailer.
    "$rigtest" testdata/rig_dolly/*.csv > "$out/rigtest_dolly.txt"
    check rigtest_dolly "$out/rigtest_dolly.txt" $?
    # N21: the same rig from the D4s test as four units (truck, grain hopper, dolly, grain hopper), main view and mirrors.
    "$rigtest" testdata/rig_road/*.csv > "$out/rigtest_road.txt"
    check rigtest_road "$out/rigtest_road.txt" $?
    # N47: the rig maps, each run1 capture with itself and with run2's of the same number (another session).
    pairs=()
    for f in testdata/rig/run1_capture_*.csv; do
        pairs+=("$f" "$f")
        [ -f "${f/run1_/run2_}" ] && pairs+=("$f" "${f/run1_/run2_}")
    done
    "$maptest" "${pairs[@]}" > "$out/maptest.txt"
    check maptest "$out/maptest.txt" $?
    "$dusttest" > "$out/dusttest.txt"
    check dusttest "$out/dusttest.txt" $?
    "$statetest" "$out" > "$out/statetest.txt"
    check statetest "$out/statetest.txt" $?
    "$washtest" > "$out/washtest.txt"
    check washtest "$out/washtest.txt" $?
    "$paneltest" > "$out/paneltest.txt"
    check paneltest "$out/paneltest.txt" $?
    "$langtest" lang > "$out/langtest.txt"
    check langtest "$out/langtest.txt" $?
    "$compattest" > "$out/compattest.txt"
    check compattest "$out/compattest.txt" $?
    "$premaptest" > "$out/premaptest.txt"
    check premaptest "$out/premaptest.txt" $?
    "$cargotest" > "$out/cargotest.txt"
    check cargotest "$out/cargotest.txt" $?
}

# MSVC 2022 (the PC's) caps one string literal at 16380 characters (C2026); the CI's MSVC 2026 does not.
# A shader longer than that goes in several R"( )" literals in a row: each one is checked here.
check_literals() {
    local f bad before=$failures
    for f in $(grep -l 'R"(' src/*.h src/*.cpp tools/*/*.cpp); do
        bad=$(awk -v f="$f" '
            !inside && /R"\($/ { inside = 1; n = 0; start = FNR; next }
            inside && /^\)"/ { inside = 0; if (n > 16380) print f ":" start ": " n " characters"; next }
            inside { n += length($0) + 1 }' "$f")
        if [ -n "$bad" ]; then
            echo "FAIL literal over 16380 characters (MSVC 2022, C2026): $bad"
            failures=$((failures + 1))
        fi
    done
    [ "$failures" -eq "$before" ] && echo "ok   string literals <= 16380 characters (MSVC 2022)"
}
check_literals

if [ "${1:-}" = "--bin" ]; then
    [ -n "${2:-}" ] || { echo "usage: $0 --bin <dir>"; exit 2; }
    echo "== binaries in $2"
    run_all "$2" "build/tests-output"
else
    compilers=("$@")
    if [ ${#compilers[@]} -eq 0 ]; then
        for c in g++ clang++; do command -v "$c" > /dev/null && compilers+=("$c"); done
    fi
    [ ${#compilers[@]} -gt 0 ] || { echo "FAIL: no C++ compiler (g++ or clang++) found"; exit 2; }
    for cxx in "${compilers[@]}"; do
        dir="build/tests-$(basename "$cxx")"
        echo "== $cxx ($dir)"
        # A fresh configure each time: the compiler of an existing build directory cannot change.
        rm -rf "$dir"
        mkdir -p "$dir"
        if ! CXXFLAGS="${CXXFLAGS:-} -Werror" cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_CXX_COMPILER="$cxx" > "$dir/configure.log" 2>&1 ||
           ! cmake --build "$dir" --target rigtest maptest dusttest statetest washtest paneltest langtest compattest > "$dir/build.log" 2>&1; then
            echo "FAIL $cxx: build failed (warnings are errors)"
            tail -n 30 "$dir/configure.log" "$dir/build.log" 2> /dev/null
            failures=$((failures + 1))
            continue
        fi
        run_all "$dir/out" "$dir/output"
    done
fi

if [ "$failures" -ne 0 ]; then
    echo "FAILED ($failures)"
    exit 1
fi
echo "all tests match the baseline"
