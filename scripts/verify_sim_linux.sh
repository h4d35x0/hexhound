#!/usr/bin/env bash
# Prove the Linux simulator actually works on THIS machine.
#
# Written for the DEF CON demo laptop: run it there, not on the build box.
# "It compiled" and "the file exists" are not evidence; every check below
# executes the binary and looks at what it produced.
#
# Usage:
#   ./scripts/verify_sim_linux.sh [path-to-binary] [soak_seconds]
# Defaults to .pio/build/desktop-sim-linux/program and a 600s idle soak.
# Pass 0 as soak_seconds to skip the soak for a quick pass.
#
# Not covered here (do these by hand, they need a human):
#   - the window actually renders something recognisable
#   - SPACE / ENTER / S / Q do the right thing under a real display
#   - behaviour with the radios switched off at the OS level

set -uo pipefail

BIN="${1:-.pio/build/desktop-sim-linux/program}"
SOAK_SECONDS="${2:-600}"
WORK="$(mktemp -d)"
PASS=0
FAIL=0

trap 'rm -rf "$WORK"' EXIT

ok()   { echo "  PASS  $*"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL  $*"; FAIL=$((FAIL + 1)); }
head_() { echo; echo "== $* =="; }

head_ "1. binary is real and executable"
if [ ! -f "$BIN" ]; then
    bad "$BIN does not exist"
    echo "Nothing else can be checked. Build it: pio run -e desktop-sim-linux"
    exit 1
fi
SIZE=$(stat -c %s "$BIN")
if [ "$SIZE" -lt 100000 ]; then
    bad "$BIN is only $SIZE bytes - a truncated or failed build reads as present"
else
    ok "$BIN is $SIZE bytes"
fi
[ -x "$BIN" ] && ok "executable bit set" || bad "not executable (chmod +x)"

head_ "2. dynamic libraries all resolve"
if ldd "$BIN" 2>&1 | grep -q "not found"; then
    bad "missing shared libraries:"
    ldd "$BIN" | grep "not found" | sed 's/^/        /'
else
    ok "every shared library resolves"
    ldd "$BIN" | sed 's/^/        /'
fi

head_ "3. it starts and quits cleanly with no display (SDL dummy driver)"
SDL_VIDEODRIVER=dummy HEXHOUND_SIM_SHOT_DIR="$WORK/cold" \
HEXHOUND_SIM_SHOT_MS=5000 HEXHOUND_SIM_QUIT_MS=6000 \
timeout 90 "$BIN" > "$WORK/run.log" 2>&1
RC=$?
if [ $RC -eq 0 ]; then
    ok "exited 0 (6s of scripted runtime)"
else
    bad "exit code $RC - last lines:"
    tail -5 "$WORK/run.log" | sed 's/^/        /'
fi
if ls "$WORK/cold"/*.bmp > /dev/null 2>&1; then
    ok "produced a rendered frame"
else
    bad "no frame was produced at all"
fi

head_ "3b. the radios are fake, so the network being off cannot matter"
# The sim links its own WiFiModule/BLEModule from src/hal/sim_modules.cpp, so
# those symbols DO exist in the binary - what matters is that they announce
# fake-data mode and that nothing radio-related is linked in.
if ldd "$BIN" | grep -qE "libbluetooth|libnl-|libpcap|libnm|libcurl"; then
    bad "a networking/bluetooth library is linked in:"
    ldd "$BIN" | grep -E "libbluetooth|libnl-|libpcap|libnm|libcurl" | sed 's/^/        /'
else
    ok "no networking or bluetooth library linked (only SDL2, libm, libc)"
fi
if grep -q "WiFi-Sim.*fake data mode" "$WORK/run.log" &&
   grep -q "BLE-Sim.*fake data mode" "$WORK/run.log"; then
    ok "WiFi and BLE modules both reported fake-data mode at init"
else
    bad "the fake-data banners are missing - this may not be a simulator build"
fi

head_ "4. cold start: how long until the pet is on screen"
# The firmware prints its own boot timing; trust that over wall clock, which
# also contains the scripted quit delay. Boot includes the splash animation.
BOOT_MS=$(grep -oE "Total boot time: [0-9]+" "$WORK/run.log" | grep -oE "[0-9]+" | head -1)
if [ -n "$BOOT_MS" ]; then
    if [ "$BOOT_MS" -lt 4000 ]; then
        ok "boot completes in ${BOOT_MS}ms"
    else
        bad "boot takes ${BOOT_MS}ms - people walk away from a demo that slow"
    fi
else
    bad "the boot-time line never appeared; setup() may not have finished"
fi

head_ "5. it renders MORE than one thing: the pet evolves"
SDL_VIDEODRIVER=dummy HEXHOUND_SIM_FORCE_EVOLVE=2,3 \
HEXHOUND_SIM_SHOT_DIR="$WORK/evolve" \
HEXHOUND_SIM_SHOT_MS=200,900,1800,3000 HEXHOUND_SIM_QUIT_MS=4000 \
timeout 90 "$BIN" >> "$WORK/run.log" 2>&1
SHOTS=$(ls "$WORK/evolve"/*.bmp 2>/dev/null | wc -l)
UNIQUE=$(md5sum "$WORK/evolve"/*.bmp 2>/dev/null | awk '{print $1}' | sort -u | wc -l)
if [ "$SHOTS" -ge 3 ] && [ "$UNIQUE" -ge 3 ]; then
    ok "$SHOTS frames captured, $UNIQUE of them distinct - the cutscene animates"
else
    bad "$SHOTS frames, only $UNIQUE distinct - a sim that never changes state is a screenshot"
fi

head_ "6. every major screen renders"
for SCREEN in menu stats journal patrol missions config; do
    OUT="$WORK/screen_$SCREEN"
    SDL_VIDEODRIVER=dummy HEXHOUND_SIM_FORCE_SCREEN="$SCREEN" \
    HEXHOUND_SIM_SHOT_DIR="$OUT" HEXHOUND_SIM_SHOT_MS=400 \
    HEXHOUND_SIM_QUIT_MS=900 timeout 60 "$BIN" >> "$WORK/run.log" 2>&1
    if ls "$OUT"/*.bmp > /dev/null 2>&1; then
        ok "screen '$SCREEN' rendered"
    else
        bad "screen '$SCREEN' produced no frame"
    fi
done

head_ "7. idle soak (${SOAK_SECONDS}s) - it sits on a table for hours"
if [ "$SOAK_SECONDS" -gt 0 ]; then
    SDL_VIDEODRIVER=dummy HEXHOUND_SIM_QUIT_MS=$((SOAK_SECONDS * 1000)) \
        timeout $((SOAK_SECONDS + 60)) "$BIN" > "$WORK/soak.log" 2>&1 &
    SOAK_PID=$!
    RSS_FIRST=""
    RSS_LAST=""
    while kill -0 "$SOAK_PID" 2>/dev/null; do
        RSS=$(ps -o rss= -p "$SOAK_PID" 2>/dev/null | tr -d ' ')
        [ -n "$RSS" ] && { [ -z "$RSS_FIRST" ] && RSS_FIRST="$RSS"; RSS_LAST="$RSS"; }
        sleep 10
    done
    wait "$SOAK_PID"
    SOAK_RC=$?
    if [ $SOAK_RC -eq 0 ]; then
        ok "survived ${SOAK_SECONDS}s and exited 0"
    else
        bad "died during the soak, exit code $SOAK_RC - last lines:"
        tail -5 "$WORK/soak.log" | sed 's/^/        /'
    fi
    if [ -n "$RSS_FIRST" ] && [ -n "$RSS_LAST" ]; then
        GROWTH=$(( RSS_LAST - RSS_FIRST ))
        echo "        RSS ${RSS_FIRST}kB -> ${RSS_LAST}kB (${GROWTH}kB growth)"
        if [ "$GROWTH" -gt 51200 ]; then
            bad "memory grew ${GROWTH}kB over ${SOAK_SECONDS}s - looks like a leak"
        else
            ok "memory stable over the soak"
        fi
    fi
else
    echo "  SKIP  soak disabled (pass a second argument in seconds to enable)"
fi

head_ "RESULT"
echo "  $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then
    echo "  NOT READY for the table."
    exit 1
fi
echo "  Automated checks are green. Still confirm by hand, on this machine:"
echo "    - a window opens and the pet is visible"
echo "    - SPACE / ENTER / S / Q each do what the guide says"
echo "    - it behaves with WiFi and Bluetooth switched off at the OS level"
