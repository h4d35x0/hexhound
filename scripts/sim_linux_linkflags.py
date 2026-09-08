"""Link flags for the Linux desktop-sim builds.

PlatformIO's flag parser routes `-l`/`-L` entries from `build_flags` into LIBS
and LIBPATH, but anything it does not recognise (such as `-static-libstdc++`)
lands in CCFLAGS, where it never reaches the link step. Setting the flags here
is the only place they actually apply.

Why static libstdc++/libgcc: the packaged simulator has to run on a con laptop
with no build toolchain installed, where a matching libstdc++ ABI is not
guaranteed. Statically linking the two compiler runtimes leaves libSDL2 as the
only third-party shared object worth bundling (see scripts/package_sim_linux.sh).
"""

Import("env")  # noqa: F821 - injected by PlatformIO/SCons

env.Append(LINKFLAGS=["-static-libstdc++", "-static-libgcc"])
