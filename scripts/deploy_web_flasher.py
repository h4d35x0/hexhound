#!/usr/bin/env python3
"""Publish the staged web flasher to the public flasher repo, with gates.

    python scripts/build_flashes.py <version>       # build every image
    python scripts/stage_web_flasher.py <version>   # stage + write manifests
    python scripts/deploy_web_flasher.py <version>  # <- this, publishes it

Why this script exists at all
----------------------------
`web/firmware/`, `web/manifests/` and `web/boards.json` are gitignored, so they
never leave your working tree. Staging changes NOTHING about what the live page
serves, and merging a PR in this repo does not deploy either. Publishing means
pushing to a SEPARATE PUBLIC repo. That second step being manual and invisible is
exactly how a stale pre-OTA T-Dongle C5 image sat on the live flasher for a day
while six fresh images sat beside it.

This is a gate, not a copier. It refuses to publish rather than publish something
wrong, because the thing it publishes is firmware that strangers flash onto
hardware, at a security conference, from a security company's domain.

What it refuses to do
---------------------
1. Publish a PARTIAL board set. `stage_web_flasher.py` writes `boards.json` from
   only the images present, so staging after a `--only` build silently drops
   every other board from the live picker. The expected set is imported from
   `stage_web_flasher.BOARDS` so the two cannot drift.
2. Publish a version other than the one you asked for.
3. Publish an image whose bytes do not match the sha256 recorded for it.
4. Publish an image with no OTA receiver in it. An image lacking the HEXHOTA1
   magic can never be updated over the air, which is the whole point of the
   giveaway units being flashable from a web page.
5. Publish anything if PRIVATE signing key material appears in any image. A
   leaked OTA signing key lets anyone sign firmware for every unit in the field.
6. Publish images that TRUST a development signing key. Refusal 5 catches a
   private key that leaked INTO an image; this catches images built to accept
   a key that should never have been trusted at all. The headers said
   "DEVELOPMENT KEY" in a comment the whole time, and a comment is not a
   control. Override with --allow-dev-keys, which says so out loud.
7. Publish if it CANNOT PERFORM that secret check, or if the check cannot be
   shown to work. "I did not look" must never render as "it is clean", so the
   search is validated against the PUBLIC keys, which are compiled in and must
   be found. If they are not found, the search is broken and the result proves
   nothing.

It also does not touch the flasher repo's own README prose. `web/README.md` is
developer documentation and would be the wrong front page for strangers, so the
public README lives in the flasher repo and only its generated firmware table is
rewritten here, between the BOARDS markers.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stage_web_flasher import BOARDS  # noqa: E402  single source of truth

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_DIR = os.path.join(PROJECT_DIR, "web")

FLASHER_REMOTE = "https://github.com/h4d35x0/hexhound-flasher.git"
PAGES_URL = "https://h4d35x0.github.io/hexhound-flasher"

# The OTA receiver's frame magic. Present in any image that can take an update.
OTA_MAGIC = b"HEXHOTA1"

# Copied into the flasher repo. web/README.md is deliberately NOT here.
PAYLOAD_FROM_WEB = ["index.html", "img", "houndlink", "manifests", "firmware",
                    "boards.json"]
PAYLOAD_FROM_ROOT = ["LICENSE"]

# Private halves. Their bytes must never appear in a published image.
PRIVATE_KEYS = [os.path.join(PROJECT_DIR, "keys", "ota-dev-signing.key"),
                os.path.join(PROJECT_DIR, "keys", "content-dev-signing.key")]
# Public halves, compiled in. Used to prove the byte search actually works.
PUBLIC_KEY_HEADERS = [os.path.join(PROJECT_DIR, "src", "ota", "ota_pubkey.h"),
                      os.path.join(PROJECT_DIR, "src", "content", "content_pubkey.h")]

BOARDS_BEGIN = "<!-- BOARDS:BEGIN -->"
BOARDS_END = "<!-- BOARDS:END -->"


class Refused(Exception):
    """A gate said no. Nothing has been published."""


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, cwd=None, check=True):
    result = subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)
    if check and result.returncode != 0:
        raise Refused(f"command failed: {' '.join(cmd)}\n"
                      f"{result.stdout[-2000:]}{result.stderr[-2000:]}")
    return result


# ---------------------------------------------------------------- gate 1: set

def preflight(version):
    """Everything that must hold before anything is published."""
    boards_path = os.path.join(WEB_DIR, "boards.json")
    if not os.path.exists(boards_path):
        raise Refused(f"no {boards_path}\n"
                      f"run: python scripts/stage_web_flasher.py {version}")

    with open(boards_path, encoding="utf-8") as fh:
        staged = json.load(fh)

    if staged.get("version") != version:
        raise Refused(
            f"staged boards.json is version {staged.get('version')!r}, you asked "
            f"for {version!r}.\nRe-stage before deploying: "
            f"python scripts/stage_web_flasher.py {version}")

    expected = {board_id for _, board_id, _, _ in BOARDS}
    present = {b["id"] for b in staged["boards"]}
    missing = expected - present
    if missing:
        raise Refused(
            "staged board set is INCOMPLETE, refusing to publish a partial "
            "picker.\n  missing: " + ", ".join(sorted(missing)) +
            "\nThis is what a --only build followed by staging does: boards.json "
            "is\nrewritten from just the images present. Rebuild everything:\n"
            f"  python scripts/build_flashes.py {version}\n"
            f"  python scripts/stage_web_flasher.py {version}")
    unexpected = present - expected
    if unexpected:
        raise Refused("staged boards.json has boards absent from "
                      "stage_web_flasher.BOARDS: " + ", ".join(sorted(unexpected)))

    print(f"  board set        OK  all {len(expected)} boards staged")

    # Every manifest resolves, and to offset 0, because these are merged images.
    for board in staged["boards"]:
        man_path = os.path.join(WEB_DIR, "manifests", f"{board['id']}.json")
        if not os.path.exists(man_path):
            raise Refused(f"missing manifest: {man_path}")
        with open(man_path, encoding="utf-8") as fh:
            manifest = json.load(fh)
        for build in manifest["builds"]:
            for part in build["parts"]:
                target = os.path.normpath(
                    os.path.join(WEB_DIR, "manifests", part["path"]))
                if not os.path.exists(target):
                    raise Refused(f"{board['id']} manifest points at a missing "
                                  f"file: {target}")
                if part["offset"] != 0:
                    raise Refused(f"{board['id']} manifest offset is "
                                  f"{part['offset']}, expected 0 for a merged "
                                  f"image")
    print(f"  manifests        OK  all resolve, all at offset 0")

    # Bytes match what boards.json claims, and every image can take an update.
    binaries = []
    for board in staged["boards"]:
        path = os.path.join(WEB_DIR, "firmware", board["bin"])
        if not os.path.exists(path):
            raise Refused(f"missing binary: {path}")
        digest = sha256_file(path)
        if digest != board["sha256"]:
            raise Refused(
                f"{board['id']}: sha256 does not match boards.json.\n"
                f"  boards.json {board['sha256']}\n  on disk     {digest}\n"
                f"Re-stage; the recorded hash and the file have diverged.")
        with open(path, "rb") as fh:
            data = fh.read()
        if os.path.getsize(path) != board["size"]:
            raise Refused(f"{board['id']}: size does not match boards.json")
        if OTA_MAGIC not in data:
            raise Refused(
                f"{board['id']} contains no {OTA_MAGIC.decode()} magic, so it has "
                f"NO OTA RECEIVER and could never be updated over the air.\n"
                f"Refusing to publish it. This is the exact defect that left a "
                f"stale C5 image on the live flasher.")
        binaries.append((board["id"], path, data))
    print(f"  image integrity  OK  sha256 and size match, "
          f"{OTA_MAGIC.decode()} present in all {len(binaries)}")
    return staged, binaries


# ------------------------------------------------------- gate 2: no secrets

def _key_byte_forms(path):
    """Plausible on-disk encodings of a key file, as raw bytes to search for."""
    import base64
    with open(path, "rb") as fh:
        raw = fh.read()
    forms = {raw}
    stripped = raw.strip()
    try:
        text = stripped.decode("ascii")
    except UnicodeDecodeError:
        text = None
    if text:
        compact = "".join(text.split())
        if re.fullmatch(r"[0-9a-fA-F]+", compact) and len(compact) % 2 == 0:
            forms.add(bytes.fromhex(compact))
        else:
            try:
                forms.add(base64.b64decode(compact, validate=True))
            except Exception:
                pass
    # An Ed25519 secret scalar is the first 32 bytes of an expanded key.
    for form in list(forms):
        if len(form) > 32:
            forms.add(form[:32])
    return {f for f in forms if len(f) >= 16}


def _pubkey_bytes(header_path):
    with open(header_path, encoding="utf-8") as fh:
        return bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", fh.read()))


def development_key_gate(allow_dev_keys=False):
    """Refuse to publish images that trust a DEVELOPMENT signing key.

    secret_scan() below answers "did a private key leak into an image?".
    This answers a different and, until now, unasked question: "should anyone
    trust the key these images are built to accept?"

    A development key is one whose private half was minted onto a developer
    machine and lives in a gitignored file there. The firmware headers say so
    in a comment, and a comment is not a control: these images have been served
    publicly with development keys compiled in and nothing objected, because
    nothing was looking. The OTA key is the one that matters, because it
    authorises replacing the firmware on every unit in the field.

    Provenance is read from the generated headers. An ABSENT marker counts as
    development: a header too old or too hand-edited to say what it holds is
    exactly the case that must not sail through.
    """
    checks = (
        (os.path.join(PROJECT_DIR, "src", "ota", "ota_pubkey.h"),
         "OTA_KEY_PROVENANCE_DEVELOPMENT"),
        (os.path.join(PROJECT_DIR, "src", "content", "content_pubkey.h"),
         "CONTENT_KEY_PROVENANCE_DEVELOPMENT"),
    )
    findings = []
    for header, macro in checks:
        rel = os.path.relpath(header, PROJECT_DIR)
        if not os.path.exists(header):
            findings.append(rel + ": absent, so its key provenance is unknown")
            continue
        with open(header, encoding="utf-8") as fh:
            text = fh.read()
        pattern = r"^@S*#define@S+" + macro + r"@S+([01])@S*$"
        pattern = pattern.replace("@S", chr(92) + "s")
        m = re.search(pattern, text, re.M)
        if m is None:
            findings.append(rel + ": no " + macro + ", so provenance is unknown")
        elif m.group(1) == "1":
            findings.append(rel + ": built against a DEVELOPMENT key")
        else:
            print("  key provenance   OK  " + rel + " carries a release key")

    if not findings:
        return

    detail = chr(10).join("  " + f for f in findings)
    if allow_dev_keys:
        print("  key provenance   OVERRIDDEN, publishing development-keyed images:")
        print(detail)
        return
    raise Refused(
        "these images trust a signing key that is not fit to ship:" + chr(10)
        + detail + chr(10) + chr(10)
        + "A development key's private half sits in a gitignored file on a "
          "developer machine. Anyone who obtains it can sign firmware that "
          "every device in the field will accept as genuine, which is the "
          "whole of what OTA signing exists to prevent." + chr(10) + chr(10)
        + "Fix it properly: generate the keypair somewhere that is not a "
          "developer laptop, keep the private half there, and install only "
          "the public half here:" + chr(10)
        + "    python scripts/sign_ota_image.py --set-release-pubkey <64 hex>" + chr(10)
        + "    python scripts/sign_content_pack.py --set-release-pubkey <64 hex>" + chr(10) + chr(10)
        + "To publish anyway, knowing what that means, pass --allow-dev-keys. "
          "That is a deliberate act and it is recorded in the output; the "
          "previous behaviour was to do it silently.")


def secret_scan(binaries, allow_missing_keys=False):
    """Refuse to publish private key material, and prove the search works."""
    # Validate the method FIRST. A search that cannot find something it should
    # find tells you nothing when it finds nothing.
    validated = False
    for header in PUBLIC_KEY_HEADERS:
        if not os.path.exists(header):
            continue
        pub = _pubkey_bytes(header)
        if len(pub) < 32:
            continue
        windows = [pub[i:i + 32] for i in range(len(pub) - 31)]
        for _, _, data in binaries:
            if any(w in data for w in windows):
                validated = True
                break
        if validated:
            print(f"  scan validated   OK  found the public key from "
                  f"{os.path.relpath(header, PROJECT_DIR)} in a published image")
            break
    if not validated:
        raise Refused(
            "the byte search could not find any PUBLIC key in any image, even "
            "though\nthe public keys are compiled in and must be there. The "
            "search is therefore\nbroken, and a clean private-key result from it "
            "would prove nothing.\nFix the search before publishing firmware.")

    missing = [k for k in PRIVATE_KEYS if not os.path.exists(k)]
    if missing:
        message = ("cannot check for leaked private keys, these files are not on "
                   "this machine:\n  " + "\n  ".join(
                       os.path.relpath(m, PROJECT_DIR) for m in missing) +
                   "\n'I did not look' is not 'it is clean', so this refuses by "
                   "default.\nRe-run with --allow-missing-keys ONLY if you are "
                   "certain those keys were\nnever available to the build that "
                   "produced these images.")
        if not allow_missing_keys:
            raise Refused(message)
        print("  SECRET SCAN SKIPPED (--allow-missing-keys)")
        for line in message.splitlines():
            print(f"    {line}")
        return

    leaks = []
    for key_path in PRIVATE_KEYS:
        forms = _key_byte_forms(key_path)
        for board_id, _, data in binaries:
            if any(form in data for form in forms):
                leaks.append((board_id, os.path.basename(key_path)))
    if leaks:
        raise Refused(
            "PRIVATE SIGNING KEY MATERIAL FOUND IN IMAGES. Nothing published.\n" +
            "\n".join(f"  {board} contains bytes from {key}"
                      for board, key in leaks) +
            "\nA leaked OTA signing key lets anyone sign firmware that every "
            "unit in\nthe field will accept. Do not publish these images. Treat "
            "the key as\ncompromised and rotate it.")
    print(f"  secret scan      OK  no private key material in any of "
          f"{len(binaries)} images")


# ----------------------------------------------------------------- publishing

def render_boards_table(staged):
    lines = [f"## Firmware v{staged['version']}", "",
             "| Board | Chip | Size | SHA256 (first 16) |", "|---|---|---|---|"]
    for board in staged["boards"]:
        lines.append(f"| {board['name']} | `{board['chip']}` | "
                     f"{board['size'] / 1024:.0f} KB | `{board['sha256'][:16]}` |")
    return "\n".join(lines)


def update_readme(repo_dir, staged):
    readme = os.path.join(repo_dir, "README.md")
    if not os.path.exists(readme):
        raise Refused(f"the flasher repo has no README.md at {readme}")
    with open(readme, encoding="utf-8") as fh:
        text = fh.read()
    if BOARDS_BEGIN not in text or BOARDS_END not in text:
        raise Refused(
            f"the flasher repo README.md has no generated-table markers.\n"
            f"Add these two lines around the firmware table so this script can "
            f"refresh it\nwithout touching your prose:\n"
            f"  {BOARDS_BEGIN}\n  ...generated table...\n  {BOARDS_END}")
    start = text.index(BOARDS_BEGIN) + len(BOARDS_BEGIN)
    end = text.index(BOARDS_END)
    updated = (text[:start] + "\n" + render_boards_table(staged) + "\n" +
               text[end:])
    if updated == text:
        return False
    with open(readme, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(updated)
    return True


def copy_payload(repo_dir):
    for name in PAYLOAD_FROM_WEB:
        src = os.path.join(WEB_DIR, name)
        dst = os.path.join(repo_dir, name)
        if not os.path.exists(src):
            raise Refused(f"payload item missing: {src}")
        if os.path.isdir(src):
            shutil.rmtree(dst, ignore_errors=True)
            shutil.copytree(src, dst)
        else:
            shutil.copy2(src, dst)
    for name in PAYLOAD_FROM_ROOT:
        shutil.copy2(os.path.join(PROJECT_DIR, name), os.path.join(repo_dir, name))
    # Pages otherwise runs the payload through Jekyll, which ignores
    # underscore-prefixed paths and adds a build step nothing here needs.
    open(os.path.join(repo_dir, ".nojekyll"), "w").close()

    # Turn EOL translation off for everyone, not just for this machine.
    # Windows git enables core.autocrlf in its SYSTEM config, so a clone checks
    # out CRLF while the generators here all write LF. Without this, every
    # deploy shows a spurious README modification and the committed line
    # endings flip back and forth depending on who ran the deploy.
    with open(os.path.join(repo_dir, ".gitattributes"), "w",
              encoding="ascii", newline="\n") as fh:
        fh.write("# Generated by scripts/deploy_web_flasher.py in the firmware "
                 "repo.\n"
                 "# No EOL translation: this repo holds firmware binaries plus\n"
                 "# generated text that is always written LF.\n"
                 "* -text\n")


def git_identity():
    """Reuse this repo's committer identity rather than inventing one."""
    name = run(["git", "config", "user.name"], cwd=PROJECT_DIR, check=False)
    email = run(["git", "config", "user.email"], cwd=PROJECT_DIR, check=False)
    # The fallbacks are deliberately impersonal. This function runs in a
    # publish path, so an identity baked in here would be attributed to a real
    # person on every automated commit whether or not they ran it.
    return (name.stdout.strip() or "HexHound release",
            email.stdout.strip() or "noreply@users.noreply.github.com")


def publish(staged, version, sign, dry_run):
    repo_dir = tempfile.mkdtemp(prefix="hexhound-flasher-")
    try:
        print(f"\ncloning {FLASHER_REMOTE}")
        # autocrlf must be off for the CHECKOUT, not merely set afterwards.
        # Windows git turns it on in its system config, so a plain clone writes
        # CRLF into the working tree and every LF file this script generates then
        # reads as modified.
        run(["git", "-c", "core.autocrlf=false", "clone", "--depth", "1",
             FLASHER_REMOTE, repo_dir])

        name, email = git_identity()
        run(["git", "config", "user.name", name], cwd=repo_dir)
        run(["git", "config", "user.email", email], cwd=repo_dir)
        run(["git", "config", "core.autocrlf", "false"], cwd=repo_dir)

        copy_payload(repo_dir)
        update_readme(repo_dir, staged)

        run(["git", "add", "-A"], cwd=repo_dir)
        status = run(["git", "status", "--porcelain"], cwd=repo_dir)
        if not status.stdout.strip():
            print("\nthe live flasher already serves exactly these bytes, "
                  "nothing to publish")
            return False

        print("\nchanges to publish:")
        for line in status.stdout.strip().splitlines():
            print(f"  {line}")

        if dry_run:
            print("\n--dry-run, stopping before commit. Nothing published.")
            return False

        message = (
            f"firmware v{version}\n\n"
            f"{len(staged['boards'])} boards. Published by "
            f"scripts/deploy_web_flasher.py, which verified before pushing that "
            f"the\nboard set is complete, every image matches its recorded "
            f"sha256, every image\ncontains the {OTA_MAGIC.decode()} OTA "
            f"receiver magic, and no private signing key\nmaterial appears in "
            f"any image (with that search validated against the public\nkeys, "
            f"which are present as expected).")
        commit = ["git", "commit", "-q", "-m", message]
        if not sign:
            # Signing here would block on an interactive agent prompt, which is
            # not something a deploy script should hang on. Opt in with --sign.
            commit = ["git", "-c", "commit.gpgsign=false"] + commit[1:]
        run(commit, cwd=repo_dir)
        run(["git", "push", "origin", "HEAD:main"], cwd=repo_dir)
        head = run(["git", "rev-parse", "--short", "HEAD"], cwd=repo_dir)
        print(f"\npushed {head.stdout.strip()} to hexhound-flasher")
        return True
    finally:
        shutil.rmtree(repo_dir, ignore_errors=True)


# -------------------------------------------------------------- verification

def fetch(url, timeout=180):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return response.read()


def verify_live(staged, timeout_s=300):
    """Check the bytes the live site actually serves, not the ones we built.

    Pages rebuilds asynchronously, so boards.json is polled until it matches
    what we just pushed. Verifying the build output instead of the served bytes
    would defeat the purpose of this whole script.
    """
    print(f"\nwaiting for {PAGES_URL} to serve v{staged['version']}")
    want = {b["id"]: b["sha256"] for b in staged["boards"]}
    deadline = time.time() + timeout_s
    served = None
    while time.time() < deadline:
        try:
            served = json.loads(fetch(f"{PAGES_URL}/boards.json?t={int(time.time())}",
                                      timeout=30))
            if (served.get("version") == staged["version"] and
                    {b["id"]: b["sha256"] for b in served["boards"]} == want):
                break
        except (urllib.error.URLError, json.JSONDecodeError, TimeoutError) as exc:
            served = None
            print(f"  not ready yet ({type(exc).__name__})")
        time.sleep(10)
    else:
        raise Refused(
            f"the live site did not serve v{staged['version']} within "
            f"{timeout_s}s.\nThe push succeeded, so this is a Pages build delay "
            f"or failure rather than\na bad payload. Check the repo's Pages "
            f"build log, then re-run with\n--verify-only to confirm.")

    print(f"  boards.json      OK  live version {served['version']}, "
          f"{len(served['boards'])} boards")

    bad = []
    for board in served["boards"]:
        data = fetch(f"{PAGES_URL}/firmware/{board['bin']}")
        digest = hashlib.sha256(data).hexdigest()
        ok_hash = digest == want.get(board["id"])
        ok_ota = OTA_MAGIC in data
        if not (ok_hash and ok_ota):
            bad.append((board["id"], ok_hash, ok_ota))
        print(f"  {board['id']:22s} {len(data):>9} B  "
              f"sha256 {'OK' if ok_hash else 'MISMATCH'}  "
              f"{OTA_MAGIC.decode()} {'yes' if ok_ota else 'NO'}")
    if bad:
        raise Refused("the LIVE site is serving images that do not match what "
                      "was published:\n" + "\n".join(
                          f"  {b} sha256_ok={h} ota_ok={o}" for b, h, o in bad))
    print(f"\nverified live: all {len(served['boards'])} served images match the "
          f"build and carry the OTA receiver")


def main():
    parser = argparse.ArgumentParser(
        description="Publish the staged web flasher to the public flasher repo.")
    parser.add_argument("version", help="firmware version, e.g. 0.4.2")
    parser.add_argument("--dry-run", action="store_true",
                        help="run every gate and show the diff, publish nothing")
    parser.add_argument("--verify-only", action="store_true",
                        help="skip publishing, just check what the live site serves")
    parser.add_argument("--skip-verify", action="store_true",
                        help="publish without waiting for the live site")
    parser.add_argument("--sign", action="store_true",
                        help="sign the deploy commit (may prompt an agent)")
    parser.add_argument("--allow-dev-keys", action="store_true",
                        help="publish even though the images trust a "
                             "DEVELOPMENT signing key whose private half is on "
                             "a developer machine")
    parser.add_argument("--allow-missing-keys", action="store_true",
                        help="publish even though the private keys are absent "
                             "and the leak check therefore cannot run")
    parser.add_argument("--yes", action="store_true",
                        help="do not ask for confirmation before publishing")
    args = parser.parse_args()

    try:
        print(f"checking the staged v{args.version} payload")
        staged, binaries = preflight(args.version)
        development_key_gate(allow_dev_keys=args.allow_dev_keys)
        secret_scan(binaries, allow_missing_keys=args.allow_missing_keys)

        if args.verify_only:
            verify_live(staged)
            return 0

        if not (args.dry_run or args.yes):
            print(f"\nThis publishes {len(staged['boards'])} firmware images "
                  f"PUBLICLY to\n  {FLASHER_REMOTE}\nand they will be served "
                  f"from {PAGES_URL}")
            if input("Type 'publish' to continue: ").strip() != "publish":
                print("aborted, nothing published")
                return 1

        pushed = publish(staged, args.version, args.sign, args.dry_run)
        if pushed and not args.skip_verify:
            verify_live(staged)
        return 0
    except Refused as exc:
        print(f"\nREFUSED: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
