#!/usr/bin/env bash
# Package the Linux desktop simulator into a folder anyone can copy and run
# on a machine with NO build toolchain and NO SDL2 installed.
#
# Usage (run on the machine you built on, from the repo root):
#   pio run -e desktop-sim-linux
#   pio run -e desktop-sim-wave-linux        # optional, big-panel variant
#   ./scripts/package_sim_linux.sh [output_dir]
#
# Output: <output_dir>/HexHound-Simulator-Linux/ plus a .tar.gz of it.
#
# What gets bundled and why:
#   - program binaries          : libstdc++/libgcc are statically linked by
#                                 scripts/sim_linux_linkflags.py, so no
#                                 compiler runtime is needed on the target.
#   - lib/libSDL2-2.0.so.0      : the SDL we linked against.
#   - lib/libSDL3.so.0          : REQUIRED on Kali/Debian trixie and newer,
#                                 where libsdl2-2.0-0 is the sdl2-compat shim.
#                                 That shim dlopens SDL3 at runtime, so ldd on
#                                 the program never mentions it and a package
#                                 built from ldd alone dies with
#                                 "Failed loading SDL3 library."
#   - run-simulator.sh          : sets LD_LIBRARY_PATH to the bundled lib dir.
# Deliberately NOT bundled: glibc, X11/Wayland/xkbcommon, libdrm/libgbm, audio.
# Every machine with a graphical session already has them, and shipping our own
# libdrm/libgbm is a reliable way to break the host's GPU driver. Build on the
# oldest glibc you need to support.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_ROOT="${1:-$REPO_ROOT/dist}"
PKG_NAME="HexHound-Simulator-Linux"
PKG_DIR="$OUT_ROOT/$PKG_NAME"

MAIN_BIN="$REPO_ROOT/.pio/build/desktop-sim-linux/program"
WAVE_BIN="$REPO_ROOT/.pio/build/desktop-sim-wave-linux/program"

if [ ! -x "$MAIN_BIN" ]; then
    echo "ERROR: $MAIN_BIN not found. Run: pio run -e desktop-sim-linux" >&2
    exit 1
fi

rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR/lib"

install -m 755 "$MAIN_BIN" "$PKG_DIR/HexHound-Simulator"
if [ -x "$WAVE_BIN" ]; then
    install -m 755 "$WAVE_BIN" "$PKG_DIR/HexHound-Simulator-BigScreen"
else
    echo "NOTE: big-screen build not present, packaging the 160x80 sim only."
    echo "      Build it with: pio run -e desktop-sim-wave-linux"
fi

# Bundle libSDL2 itself (resolved from the actual link, not a guessed path).
SDL_SO="$(ldd "$MAIN_BIN" | awk '/libSDL2/ {print $3}' | head -1)"
if [ -z "$SDL_SO" ] || [ ! -e "$SDL_SO" ]; then
    echo "ERROR: could not resolve libSDL2 from $MAIN_BIN via ldd" >&2
    exit 1
fi
cp -L "$SDL_SO" "$PKG_DIR/lib/$(basename "$SDL_SO")"

# On sdl2-compat systems (Kali/Debian trixie+) libSDL2 is a thin shim that
# dlopens SDL3, which ldd cannot see. Bundle SDL3 too when the system has it.
SDL3_SO="$(ldconfig -p 2>/dev/null | awk '/libSDL3\.so\.0 .*x86-64|libSDL3\.so\.0 =>/ {print $NF}' | head -1)"
if [ -n "$SDL3_SO" ] && [ -e "$SDL3_SO" ]; then
    cp -L "$SDL3_SO" "$PKG_DIR/lib/$(basename "$SDL3_SO")"
    echo "Bundled SDL3 runtime: $SDL3_SO"
elif strings "$SDL_SO" 2>/dev/null | grep -q "libSDL3.so.0"; then
    echo "ERROR: $SDL_SO is an sdl2-compat shim but libSDL3.so.0 was not found" >&2
    echo "       on this system. Install libsdl3-0 and re-run." >&2
    exit 1
fi

# Record exactly what shipped, so a bug report from the con floor is diagnosable.
{
    echo "HexHound simulator package"
    echo "built on: $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
    echo "glibc:    $(ldd --version | head -1)"
    echo "kernel:   $(uname -r)"
    echo "bundled:"
    (cd "$PKG_DIR/lib" && ls -l --time-style=+ | sed 's/^/  /')
} > "$PKG_DIR/BUILD_INFO.txt"

cat > "$PKG_DIR/run-simulator.sh" <<'LAUNCHER'
#!/usr/bin/env bash
# Launch the HexHound simulator using the bundled SDL2.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$HERE/HexHound-Simulator"
if [ "${1:-}" = "--big" ]; then
    BIN="$HERE/HexHound-Simulator-BigScreen"
    shift
fi
if [ ! -x "$BIN" ]; then
    echo "ERROR: $BIN missing from this package." >&2
    exit 1
fi
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$BIN" "$@"
LAUNCHER
chmod 755 "$PKG_DIR/run-simulator.sh"

cat > "$PKG_DIR/README.txt" <<'READMETXT'
HexHound Desktop Simulator - Linux
==================================

Run it:
    ./run-simulator.sh              160x80 panel (T-Dongle S3)
    ./run-simulator.sh --big        320x172 panel (Waveshare 1.47")

Controls:
    SPACE   short press  (start patrol, scroll, dismiss)
    ENTER   long press   (open menu, select, back)
    S       save a screenshot next to the simulator
    Q       quit

No install, no network, no toolchain needed. SDL2 ships in ./lib and the
launcher points the loader at it. The simulator runs entirely offline: the
Wi-Fi and BLE modules are not compiled into this build at all, and the scan
results you see are injected fake data.

If the window does not open over SSH or on a bare TTY, you need a graphical
session (X11 or Wayland). To check it runs without a display:
    SDL_VIDEODRIVER=dummy HEXHOUND_SIM_QUIT_MS=3000 ./run-simulator.sh
READMETXT

# Reproducible-ish tarball ordering; timestamps still come from the files.
tar -C "$OUT_ROOT" -czf "$OUT_ROOT/$PKG_NAME.tar.gz" "$PKG_NAME"

echo
echo "Package: $PKG_DIR"
echo "Tarball: $OUT_ROOT/$PKG_NAME.tar.gz"
du -sh "$PKG_DIR" "$OUT_ROOT/$PKG_NAME.tar.gz"
