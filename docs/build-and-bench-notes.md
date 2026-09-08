# Build and bench notes

Operational gotchas that cost real time at least once. Each one describes a
symptom that looks like a code regression and is not.

For defect retrospectives see `docs/bugfix_log.md`; for environment setup see
`docs/build-environments.md`; for board-level symptoms see
`docs/troubleshooting.md`.

## A killed build leaves orphaned compiler processes

Interrupting a long firmware build can leave `cc1plus` or `xtensa-*` processes
alive and holding `.o` files open. Builds afterwards fail with

    Error 3221225794

which is `STATUS_DLL_INIT_FAILED`, and it appears across unrelated third-party
files rather than at the change you just made.

**That spread is the tell.** A real regression fails at your code. A failure
that lands on files you have never edited, in libraries you did not touch, is
an environment fault. Kill the orphaned processes and build one board at a
time.

## A long serial capture can go deaf while the board keeps printing

On the ESP32-S3 native USB CDC, a capture that runs for more than roughly two
minutes can stop delivering bytes while the firmware carries on emitting them.
It looks exactly like a board that has hung or reset, and it is neither.

- **Reopen the port on a cycle** rather than holding one handle open for the
  whole session. Roughly 75 seconds per handle has run cleanly for 15 minutes.
- **Read uptime back** before concluding anything died. One command separates a
  deaf handle from a dead board.

**Open the port with DTR asserted and RTS clear.** USB CDC drops output when
DTR is low, so the opposite setting yields a silent port that is purely the
host's doing. Note also that DTR low plus RTS asserted is the reset knock, so
choosing it by accident resets the board you were trying to observe.

## A size baseline goes stale the moment shared code moves

The byte-identity check described in `docs/open-work.md` section B compares a
board's `.bin` against a baseline build. That baseline is only valid while
every change since it is confined to code the compared board does not compile.

Once all-boards work lands, the baseline must be rebuilt. Comparing against a
stale one produced an alarming 1.19 MB "regression" that was entirely an
artefact of the reference, with nothing wrong in the tree at all.

**The safe way to take a same-tree baseline** is to check the previous revision
of the relevant files into the working tree, build, then restore:

    git checkout HEAD~1 -- <files>
    # build, keep the .bin
    git checkout HEAD -- <files>

**Do not use `git stash` for this.** Every worktree attached to a repository
shares one stash stack, so a `pop` in one worktree can apply work in progress
from another.

## A probe that finds nothing is usually a broken probe

Before reporting that something is absent, confirm the probe can find the thing
when it IS present.

Worked example: two separate regexes reported an empty SPIFFS backup that was
in fact intact. SPIFFS splits a file across pages, so a contiguous regex match
over a raw partition dump cannot work by construction, and a byte-order-aware
read of the same image found the data immediately. A second instance of the
same mistake read the tail of a grep over a dump and returned stale pages,
which is how a pet's recorded stage came back two evolutions out of date.

The generic form: an absence is a claim about your instrument as much as about
the data. Validate the instrument on a known-positive first.

## Identify a board by what its firmware prints, not by its USB descriptors

If more than one microcontroller board is ever attached to the same machine,
the port number is not an identity and neither is the USB descriptor. Serial
port assignments move between reboots, and two boards from different projects
can present descriptors that look interchangeable at a glance.

**Grep for a string this project owns.** HexHound tags its serial output with
`[Loop]`, `[Pet]`, `[Rules]`, `[Storage]`, `[Touch]`, `[USB]` and `[BLE]`.
Reading one line of output is cheaper than any descriptor check, needs no
bootloader entry, and cannot be confused by a sibling project.

Two separate near-misses came from skipping this. In one, a console was opened
on a board believed to be the target because its vendor and product ids looked
right; it was an unrelated project's device, and a USB HID mission was one step
from being fired at it. In the other, an unexpected board appeared mid-session
and was correctly identified as foreign only because its address did not match
the known one; flashing it would have destroyed someone else's firmware.

**Open a port for reading with DTR asserted and RTS clear.** That is
deliberately not the reset sequence, so a misidentified board is read from
rather than reset or written to.

## The native test harness inflates its own passed count

Two facts about the test harness in `test/`, both of which will mislead you:

- **A failing `RUN_TEST` increments BOTH counters.** The harness increments the
  passed count after calling the case, and a failing assertion returns from the
  case rather than skipping that increment. A run can therefore print
  "14 passed, 1 failed". Only the failed count means anything; the passed count
  is not a measurement.
- **`ASSERT_*` expands to a bare `return`**, so it can only be used inside a
  void function. A helper that returns a value cannot assert.

Cases are registered by hand with `RUN_TEST()`, so a case that exists but is
never registered silently does not run. Audit the count of defined cases
against the count of registered ones rather than trusting a green summary.
Green means it linked and nothing tripped, not that anything is covered.

**Build native test binaries into `.pio/build/native/`.** Note that
`pio test -e native` wipes `.pio/build/` down to `native/`, deleting every
firmware build directory, so never run a test sweep concurrently with board
builds.

## A cumulative counter is not a rate

A counter that accumulates since boot, divided by another counter that
accumulates since boot, is a running average. A running average rises whenever
recent behaviour exceeds it, which reads exactly like a trend.

A diagnostic build printed a touch-failure percentage that appeared to climb
steadily with use. Differencing consecutive samples instead of reading the
printed figure showed the underlying behaviour was bimodal, not rising:
intervals alternated between roughly 0 percent and roughly 100 percent, and the
near-100 intervals carried about half as many reads as the others. That
correlation was the actual mechanism, and the printed percentage had concealed
it.

**Difference two samples before describing any trend.** If a numerator and a
denominator did not come out of the same measurement, the ratio is invented.

## Before believing a missing linker, try the alternate linker

A MinGW toolchain ships both `ld.exe` and `ld.bfd.exe`. If `ld.exe` goes
missing or becomes unopenable, `gcc -fuse-ld=bfd` links successfully using the
other file, which sits intact in the same directory. Try that first: it needs
no elevation, no reinstall and no restart.

Two different causes produce the same "linker is gone" symptom, and one command
separates them. Copy the binary to a different name and use it:

- **works under the new name** means the block was on the filename, so the
  contents are fine
- **still blocked under the new name** means something is matching the contents

The same discriminator applies to a freshly linked test executable that
disappears after a successful build. See `docs/open-work.md` item O.

## An event log can report zero because you cannot read it

When querying a host operating system's audit or security log as evidence, a
query without sufficient privilege can return an empty result rather than an
access error. An unreadable log then looks identical to a clean one, and
"we found nothing" becomes a false negative that reads as a pass.

**Prove the instrument on a known-positive in the same query.** If you are
counting failures, count the successes too. A log recording normal activity
will show them; both counts empty means the log is unreadable, not quiet. A
baseline taken without this check was reported as clean and was not.

## Git Bash cannot build the ESP32-C5 target, and the error blames the wrong file

The C5 platform runs `idf_tools.py`, which refuses outright to run under
MSys/Mingw. Its check is literally:

    if 'MSYSTEM' in os.environ:
        fatal('MSys/Mingw is not supported. ...')

**Unsetting the variable does not help from inside Git Bash.** The MSys runtime
re-injects `MSYSTEM` into every child process it spawns, so it is back before
the child looks. Measured: `env -u MSYSTEM python -c "import os;
print('MSYSTEM' in os.environ)"` prints `True`.

So the environment sanitising in `scripts/build_flashes.py` only works when
that script is started from a NATIVE Windows shell. **Build the C5 from
PowerShell or cmd, not from Git Bash.**

**What makes this expensive is where the error lands.** The tool failure is
reported, then the build carries on and dies compiling `FS.cpp.o`, an Arduino
framework file that has nothing to do with it:

    idf_tools.py installation failed (rc=1). Tail:
    ERROR: MSys/Mingw is not supported...
    *** [.pio/build/<env>/lib404/FS/FS.cpp.o] Error 1

A failure on a framework file you have never edited is the tell. Same shape as
the killed-build orphan processes above: when a build dies on code you did not
write, suspect the environment before the source.

## A missing compiler reads as a missing command, not a missing package

Once the MSys problem is out of the way the C5 can still fail with

    'riscv32-esp-elf-g++' is not recognized as an internal or external command

That is a PATH problem, not an absent toolchain: the compiler is present under
the isolated core directory's `packages/riscv32-esp-elf/bin/`. Check that it
exists before reinstalling anything. A failed `idf_tools.py` run earlier in the
same session can leave the install half-registered, so the package is on disk
while nothing has added it to PATH.
