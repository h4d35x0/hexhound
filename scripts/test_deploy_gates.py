"""Prove every refusal gate in deploy_web_flasher.py actually fires.

    python scripts/test_deploy_gates.py

A gate that has never fired is not known to work, and these gates are the only
thing standing between a bad build and firmware published to strangers. Each case
mutates a COPY of web/ with WEB_DIR monkeypatched, so the real staged payload is
never touched, and nothing here talks to the network or to the flasher repo.

Requires a staged payload to mutate: run build_flashes.py and
stage_web_flasher.py first, and keys/ must be present for the leak-check cases.
"""
import hashlib
import json
import os
import shutil
import sys
import tempfile

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(PROJECT, "scripts"))
import deploy_web_flasher as d  # noqa: E402

# Read the version from the staged payload rather than hardcoding it. A literal
# here has to be edited every release, and if anyone forgets, every case fails on
# the version gate and the suite looks broken instead of the version looking
# stale.
with open(os.path.join(PROJECT, "web", "boards.json"), encoding="utf-8") as _fh:
    VERSION = json.load(_fh)["version"]
print(f"testing gates against the staged payload, version {VERSION}\n")

results = []


def case(name, expect_refuse, fn):
    try:
        fn()
        ok = not expect_refuse
        detail = "no refusal"
    except d.Refused as exc:
        ok = expect_refuse
        detail = "REFUSED: " + str(exc).splitlines()[0][:88]
    except Exception as exc:                        # noqa: BLE001
        ok = False
        detail = f"UNEXPECTED {type(exc).__name__}: {exc}"
    results.append((ok, name, detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}\n         {detail}")


def fresh():
    """A pristine copy of web/ with WEB_DIR pointed at it."""
    tmp = tempfile.mkdtemp(prefix="gate-")
    shutil.copytree(os.path.join(PROJECT, "web"), os.path.join(tmp, "web"))
    d.WEB_DIR = os.path.join(tmp, "web")
    return d.WEB_DIR


def load_boards(web):
    with open(os.path.join(web, "boards.json"), encoding="utf-8") as fh:
        return json.load(fh)


def save_boards(web, data):
    with open(os.path.join(web, "boards.json"), "w", encoding="utf-8",
              newline="\n") as fh:
        json.dump(data, fh, indent=2)


def rehash(web, board):
    """Recompute size+sha256 so a later gate is the one under test."""
    path = os.path.join(web, "firmware", board["bin"])
    with open(path, "rb") as fh:
        data = fh.read()
    board["size"] = len(data)
    board["sha256"] = hashlib.sha256(data).hexdigest()


print("baseline, must NOT refuse")
def baseline():
    web = fresh()
    staged, binaries = d.preflight(VERSION)
    d.secret_scan(binaries)
case("clean payload passes every gate", False, baseline)

print("\ngate: version")
def wrong_version():
    fresh()
    d.preflight("9.9.9")
case("refuses a version that is not what was staged", True, wrong_version)

print("\ngate: complete board set (the --only trap)")
def partial():
    web = fresh()
    data = load_boards(web)
    dropped = data["boards"].pop(3)
    save_boards(web, data)
    print(f"         (dropped {dropped['id']} from boards.json)")
    d.preflight(VERSION)
case("refuses a partial board set", True, partial)

print("\ngate: image integrity")
def bad_sha():
    web = fresh()
    data = load_boards(web)
    data["boards"][0]["sha256"] = "0" * 64
    save_boards(web, data)
    d.preflight(VERSION)
case("refuses an image whose sha256 does not match", True, bad_sha)

def missing_bin():
    web = fresh()
    data = load_boards(web)
    os.remove(os.path.join(web, "firmware", data["boards"][2]["bin"]))
    d.preflight(VERSION)
case("refuses when a listed binary is absent", True, missing_bin)

print("\ngate: OTA receiver present (the stale-C5 defect)")
def no_ota():
    web = fresh()
    data = load_boards(web)
    board = data["boards"][5]                      # the C5
    path = os.path.join(web, "firmware", board["bin"])
    with open(path, "rb") as fh:
        blob = fh.read()
    blob = blob.replace(b"HEXHOTA1", b"XXXXXXXX")  # strip the magic
    with open(path, "wb") as fh:
        fh.write(blob)
    rehash(web, board)                             # so the sha gate passes
    save_boards(web, data)
    print(f"         (stripped HEXHOTA1 from {board['id']}, sha recomputed)")
    d.preflight(VERSION)
case("refuses an image with no OTA receiver", True, no_ota)

print("\ngate: manifest sanity")
def bad_offset():
    web = fresh()
    mp = os.path.join(web, "manifests", "t-dongle-c5.json")
    with open(mp, encoding="utf-8") as fh:
        man = json.load(fh)
    man["builds"][0]["parts"][0]["offset"] = 65536
    with open(mp, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(man, fh, indent=2)
    d.preflight(VERSION)
case("refuses a manifest not at offset 0", True, bad_offset)

def dangling_manifest():
    web = fresh()
    mp = os.path.join(web, "manifests", "waveshare-147b.json")
    with open(mp, encoding="utf-8") as fh:
        man = json.load(fh)
    man["builds"][0]["parts"][0]["path"] = "../firmware/does-not-exist.bin"
    with open(mp, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(man, fh, indent=2)
    d.preflight(VERSION)
case("refuses a manifest pointing at a missing file", True, dangling_manifest)

print("\ngate: no private key material (the one that matters most)")
def planted_key():
    web = fresh()
    data = load_boards(web)
    board = data["boards"][0]
    path = os.path.join(web, "firmware", board["bin"])
    with open(os.path.join(PROJECT, "keys", "ota-dev-signing.key"), "rb") as fh:
        secret = fh.read()
    with open(path, "rb") as fh:
        blob = bytearray(fh.read())
    blob[4096:4096 + len(secret)] = secret         # plant the private key
    with open(path, "wb") as fh:
        fh.write(bytes(blob))
    rehash(web, board)
    save_boards(web, data)
    print(f"         (planted the OTA private key inside {board['id']})")
    staged, binaries = d.preflight(VERSION)
    d.secret_scan(binaries)
case("refuses when a private key is embedded in an image", True, planted_key)

print("\ngate: the search must be proven to work")
def broken_validation():
    fresh()
    staged, binaries = d.preflight(VERSION)
    saved = d.PUBLIC_KEY_HEADERS
    d.PUBLIC_KEY_HEADERS = [os.path.join(PROJECT, "nope", "absent.h")]
    try:
        d.secret_scan(binaries)
    finally:
        d.PUBLIC_KEY_HEADERS = saved
case("refuses when it cannot validate its own byte search", True,
     broken_validation)

def missing_private_keys():
    fresh()
    staged, binaries = d.preflight(VERSION)
    saved = d.PRIVATE_KEYS
    d.PRIVATE_KEYS = [os.path.join(PROJECT, "keys", "not-here.key")]
    try:
        d.secret_scan(binaries)
    finally:
        d.PRIVATE_KEYS = saved
case("refuses when the keys are absent so it cannot check", True,
     missing_private_keys)

def missing_keys_override():
    fresh()
    staged, binaries = d.preflight(VERSION)
    saved = d.PRIVATE_KEYS
    d.PRIVATE_KEYS = [os.path.join(PROJECT, "keys", "not-here.key")]
    try:
        d.secret_scan(binaries, allow_missing_keys=True)
    finally:
        d.PRIVATE_KEYS = saved
case("--allow-missing-keys permits it, loudly", False, missing_keys_override)

print(chr(10) + "gate: signing key provenance")

OTA_H = os.path.join(d.PROJECT_DIR, "src", "ota", "ota_pubkey.h")
CONTENT_H = os.path.join(d.PROJECT_DIR, "src", "content", "content_pubkey.h")
PROV = ((OTA_H, "OTA_KEY_PROVENANCE_DEVELOPMENT"),
        (CONTENT_H, "CONTENT_KEY_PROVENANCE_DEVELOPMENT"))


def _with_provenance(value):
    """Rewrite both headers to a given provenance, returning the originals.

    value is "0", "1", or None to delete the marker entirely. Deleting it is
    the case that matters most: an unmarked header must fail closed.
    """
    saved = {p: open(p, "rb").read() for p, _ in PROV}
    for path, macro in PROV:
        text = open(path, encoding="utf-8").read()
        for present in ("0", "1"):
            old = "#define " + macro + " " + present
            if old in text:
                new = "" if value is None else "#define " + macro + " " + value
                text = text.replace(old, new)
                break
        open(path, "w", encoding="utf-8", newline=chr(10)).write(text)
    return saved


def _restore(saved):
    for path, data in saved.items():
        open(path, "wb").write(data)


def dev_key_refused():
    saved = _with_provenance("1")
    try:
        d.development_key_gate()
    finally:
        _restore(saved)
case("refuses images that trust a development signing key", True, dev_key_refused)


def dev_key_override():
    saved = _with_provenance("1")
    try:
        d.development_key_gate(allow_dev_keys=True)
    finally:
        _restore(saved)
case("--allow-dev-keys permits it, loudly", False, dev_key_override)


def release_key_passes():
    saved = _with_provenance("0")
    try:
        d.development_key_gate()
    finally:
        _restore(saved)
case("accepts images that trust a release key", False, release_key_passes)


def unmarked_header_refused():
    # An absent marker is the interesting case. A header too old, or too
    # hand-edited, to say what it holds must not be read as a release key.
    saved = _with_provenance(None)
    try:
        d.development_key_gate()
    finally:
        _restore(saved)
case("refuses a header whose provenance is unstated", True, unmarked_header_refused)


passed = sum(1 for ok, _, _ in results if ok)
print(f"\n{'=' * 74}\n{passed}/{len(results)} gate checks behaved as specified")
for ok, name, _ in results:
    if not ok:
        print(f"  FAILED: {name}")
sys.exit(0 if passed == len(results) else 1)
