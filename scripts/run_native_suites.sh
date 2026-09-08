#!/usr/bin/env bash
#
# Run every native test suite on Windows, as shared libraries.
#
# WHY NOT JUST RUN THE EXECUTABLES. On some Windows machines a real-time
# scanner refuses to LAUNCH a freshly linked, unsigned binary and quarantines
# it on the attempt. Building is not blocked; starting the process is. The
# failure then presents as a missing file rather than a denied one, so a suite
# appears to vanish after a successful build. See docs/open-work.md item O.
#
# Loading the same code as a shared library from an already-trusted host
# process is not blocked, so each suite is built as a DLL and driven from
# Python. This is a WORKAROUND, not a fix: it changes how the code is loaded,
# so a green run here is not evidence that the executable path would work.
# CI on Linux runs the suites normally and is unaffected.
#
#     bash scripts/run_native_suites.sh
#
# Requires MinGW g++ and a Python on PATH. Flags mirror [env:native] in
# platformio.ini; -DUNIT_TEST in particular is load-bearing, and omitting it
# gives a confusing redefinition of millis() between test_stubs.h and
# hal/tft_compat.h.
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO" || exit 1
[ -d /c/msys64/mingw64/bin ] && export PATH="/c/msys64/mingw64/bin:$PATH"

BUILD="$REPO/.pio/build/native"
WORK="$BUILD/_suite_runner"
mkdir -p "$BUILD" "$WORK" || { echo "cannot create $BUILD"; exit 2; }

# The suites define main(); rename it and export a C entry point the loader can
# call. Written here so the script has no external file dependency.
cat > "$WORK/wrap.cpp" <<'WRAP'
int hh_impl();
extern "C" __declspec(dllexport) int hh_run_tests() { return hh_impl(); }
WRAP

cat > "$WORK/run.py" <<'RUNPY'
import ctypes, os, sys
for d in (os.environ.get("HEXHOUND_MINGW_BIN", "C:/msys64/mingw64/bin"),):
    if os.path.isdir(d):
        os.add_dll_directory(d)
lib = ctypes.CDLL(sys.argv[1])
rc = lib.hh_run_tests()
sys.stdout.flush()
sys.exit(rc)
RUNPY

AJ=""
for c in "$REPO/.pio/libdeps/desktop-sim/ArduinoJson"; do
    [ -d "$c" ] && AJ="-I$c -I$c/src"
done
STD="-DSIMULATOR_BUILD -DUNIT_TEST -DLED_DATA_PIN=40 -DLED_CLK_PIN=39 -DNUM_LEDS=1 -std=c++17"

total_fail=0; ran=0; broke=0
for d in test/test_*/; do
    n=$(basename "$d"); f="$d$n.cpp"
    [ -f "$f" ] || continue

    # Every suite builds the same way: one translation unit, the suite's own
    # .cpp, self-including whatever project sources it needs. That is what
    # `pio test -e native` does, so anything special here would be a way for
    # this runner to pass while CI fails. test_mission_confirm used to be an
    # exception and was exactly that: it linked green here for a week while
    # every CI run failed to link it at all.
    dll="$BUILD/$n.dll"
    if ! g++ $STD -O0 $AJ -shared -Dmain=hh_impl -o "$dll" "$f" "$WORK/wrap.cpp" \
            > "$WORK/$n.build.log" 2>&1; then
        echo "BUILD FAILED  $n"
        grep -m3 "error" "$WORK/$n.build.log" | sed 's/^/    /'
        broke=$((broke + 1)); continue
    fi

    if command -v cygpath >/dev/null 2>&1; then dllpath=$(cygpath -w "$dll"); else dllpath="$dll"; fi
    python "$WORK/run.py" "$dllpath" > "$WORK/$n.run.log" 2>&1
    rc=$?
    line=$(grep -oE "[0-9]+ passed, [0-9]+ failed" "$WORK/$n.run.log" | tail -1)
    ran=$((ran + 1))
    # Only the failed count and the exit code mean anything. A failing case
    # increments BOTH counters in this harness, so the passed number is not a
    # measurement. See docs/build-and-bench-notes.md.
    if [ "$rc" -ne 0 ]; then
        echo "FAIL  $n  (${line:-no summary line})"
        grep -E "FAIL" "$WORK/$n.run.log" | head -5 | sed 's/^/    /'
        total_fail=$((total_fail + 1))
    else
        echo "ok    $n  ${line:-}"
    fi
done

echo
echo "$ran suites ran, $total_fail with failures, $broke did not build"
[ "$total_fail" -eq 0 ] && [ "$broke" -eq 0 ] && exit 0 || exit 1
