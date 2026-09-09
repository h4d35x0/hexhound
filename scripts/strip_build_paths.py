"""Keep the builder's absolute paths out of the firmware image.

The Arduino core's assert and log macros bake __FILE__ into the binary, so
without this every published image carries the path of whatever machine built
it, including that machine's username. Measured 2026-09-09 on the v0.4.4
images: 106 occurrences of one home directory across seven of the eight boards.

Nothing in the repository shows this. The strings exist only in compiled output,
so a text scan of the source tree, the git history, or even the whole object
store reports clean while every published binary carries them.

Why this is a script and not a build_flags line
-----------------------------------------------
-ffile-prefix-map does a LITERAL prefix match. PlatformIO interpolates
${platformio.packages_dir} in the platform's native form, which on Windows is
backslash-separated, while the paths the compiler bakes into __FILE__ are
forward-slash. Those do not match, and the flag silently does nothing: measured,
the image still held all 12 of its paths. Computing the mapping here lets us
emit both separator forms.

Writing the path literally in platformio.ini was the other option and is worse:
it would commit a developer's home directory to a public repository, which is
the thing being fixed.
"""

Import("env")  # noqa: F821  (injected by SCons/PlatformIO)

import os


def _mappings():
    """Every root worth rewriting, in both separator forms."""
    roots = []
    for key in ("PROJECT_PACKAGES_DIR", "PROJECT_CORE_DIR", "PROJECT_DIR"):
        value = env.subst("$" + key)                      # noqa: F821
        if value and value not in [r for r, _ in roots]:
            roots.append((value, key))

    seen, out = set(), []
    for root, key in roots:
        root = root.rstrip("\\/")
        if not root:
            continue
        # A shorter replacement than the original keeps the image from growing.
        # Toolchain and framework sources read as /pio/...; this project's own
        # sources read as /src/..., so a path in a crash log still says which.
        label = "/src" if key == "PROJECT_DIR" else "/pio"
        for form in (root.replace("\\", "/"), root.replace("/", "\\")):
            if form not in seen:
                seen.add(form)
                out.append((form, label))
    return out


flags = []
for src, dst in _mappings():
    # -ffile-prefix-map covers __FILE__, debug info and macro expansion.
    # -fmacro-prefix-map is the one that actually rewrites __FILE__ on some
    # GCC versions, so both are passed; a compiler that does not know a flag
    # would fail loudly at configure time rather than silently ship the path.
    flags.append("-ffile-prefix-map=%s=%s" % (src, dst))

if flags:
    env.Append(CCFLAGS=flags, CXXFLAGS=flags, ASFLAGS=flags)  # noqa: F821
