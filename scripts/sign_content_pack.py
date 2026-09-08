#!/usr/bin/env python3
"""Sign a HexHound content pack.

Takes the SAME authored JSON the plain-JSON loader has always read, encodes it
as canonical CBOR, wraps it in a signed envelope and writes a .hcp file the
firmware will accept. The authored JSON is not rewritten and does not need to
change; this tool is the only thing that knows about CBOR.

    # one time, per developer
    python scripts/sign_content_pack.py --gen-key

    # every time content changes
    python scripts/sign_content_pack.py --kind dialogue \\
        --in content-src/dialogue.json --out data/content/dialogue.hcp

    # prove the crypto in this file is the crypto in the firmware
    python scripts/sign_content_pack.py --self-test

No third-party imports. Ed25519 is the RFC 8032 reference implementation
inline, checked against the RFC 8032 section 7.1 vectors by --self-test, so
this script runs on a bare Python 3 with nothing installed. It is slow
(fractions of a second per signature) and that is fine for a build step.

── KEYS ──────────────────────────────────────────────────────────────────
--gen-key writes a DEVELOPMENT key. Its private half goes to keys/, which is
gitignored, and it is not a release key. A real release key must be generated
on a machine that is not a developer laptop and held somewhere a repository
cannot reach. See docs/p3w2-wiring.md.
"""

import argparse
import hashlib
import json
import os
import secrets
import sys

# ── Envelope format ───────────────────────────────────────────────────────
#
#   offset  size  field
#   0       4     magic "HXCP"
#   4       1     format version
#   5       1     pack kind
#   6       2     reserved, must be zero
#   8       4     key id: first 4 bytes of SHA-512(public key)
#   12      4     body length, little-endian uint32
#   16      64    Ed25519 signature over bytes [0,16) concatenated with body
#   80      ...   CBOR body
#
# The signature covers the header, so the kind and the declared body length
# are as protected as the content is. Renaming a signed dialogue pack to
# quests.hcp does not make it a quest pack.

MAGIC = b"HXCP"
FORMAT_VERSION = 1
HEADER_BYTES = 16
SIG_BYTES = 64
ENVELOPE_BYTES = HEADER_BYTES + SIG_BYTES

KIND_DIALOGUE = 1
KIND_QUESTS = 2
KIND_RECIPES = 3

KIND_BY_NAME = {
    "dialogue": KIND_DIALOGUE,
    "quests": KIND_QUESTS,
    "recipes": KIND_RECIPES,
}

# Must match CONTENT_MAX_PACK_BYTES / RECIPE_MAX_PACK_BYTES in the firmware.
MAX_PACK_BYTES = {
    KIND_DIALOGUE: 6144,
    KIND_QUESTS: 6144,
    KIND_RECIPES: 4096,
}

# Must match content_cbor.h.
CBOR_MAX_ITEMS = 256
CBOR_MAX_STR_BYTES = 128

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_KEY_DIR = os.path.join(REPO_ROOT, "keys")
DEFAULT_KEY_FILE = os.path.join(DEFAULT_KEY_DIR, "content-dev-signing.key")
PUBKEY_HEADER = os.path.join(REPO_ROOT, "src", "content", "content_pubkey.h")


# ══ Ed25519, RFC 8032 reference implementation ════════════════════════════

P = 2 ** 255 - 19
L = 2 ** 252 + 27742317777372353535851937790883648493


def _inv(x):
    return pow(x, P - 2, P)


D = -121665 * _inv(121666) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)


def _x_recover(y):
    xx = (y * y - 1) * _inv(D * y * y + 1)
    x = pow(xx, (P + 3) // 8, P)
    if (x * x - xx) % P != 0:
        x = (x * SQRT_M1) % P
    if x % 2 != 0:
        x = P - x
    return x


_BY = 4 * _inv(5) % P
_BX = _x_recover(_BY)
BASE = (_BX, _BY)


def _edwards_add(pt, q):
    x1, y1 = pt
    x2, y2 = q
    k = D * x1 * x2 * y1 * y2
    x3 = (x1 * y2 + x2 * y1) * _inv(1 + k)
    y3 = (y1 * y2 + x1 * x2) * _inv(1 - k)
    return (x3 % P, y3 % P)


def _scalar_mult(pt, e):
    result = (0, 1)
    addend = pt
    while e > 0:
        if e & 1:
            result = _edwards_add(result, addend)
        addend = _edwards_add(addend, addend)
        e >>= 1
    return result


def _encode_point(pt):
    x, y = pt
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _h(msg):
    return hashlib.sha512(msg).digest()


def _secret_scalar(sk):
    h = _h(sk)
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def ed25519_public_key(sk):
    """32-byte public key for a 32-byte seed."""
    a, _ = _secret_scalar(sk)
    return _encode_point(_scalar_mult(BASE, a))


def ed25519_sign(msg, sk, pk=None):
    """64-byte detached signature."""
    a, prefix = _secret_scalar(sk)
    if pk is None:
        pk = ed25519_public_key(sk)
    r = int.from_bytes(_h(prefix + msg), "little") % L
    big_r = _encode_point(_scalar_mult(BASE, r))
    k = int.from_bytes(_h(big_r + pk + msg), "little") % L
    s = (r + k * a) % L
    return big_r + s.to_bytes(32, "little")


def key_id(pk):
    """First 4 bytes of SHA-512(public key). Picks the key, proves nothing."""
    return hashlib.sha512(pk).digest()[:4]


# ══ Canonical CBOR encoder ════════════════════════════════════════════════
#
# Emits exactly the profile src/content/content_cbor.cpp accepts: unsigned
# integers, text strings, definite-length arrays and maps, minimal head
# encodings, no tags, no floats, no indefinite lengths. Anything this encoder
# cannot express is content the firmware could not have read anyway, so the
# error belongs here where a person is watching.


def _head(major, value):
    if value < 0:
        raise ValueError("negative values are not in the accepted CBOR profile")
    if value < 24:
        return bytes([(major << 5) | value])
    if value < 0x100:
        return bytes([(major << 5) | 24, value])
    if value < 0x10000:
        return bytes([(major << 5) | 25]) + value.to_bytes(2, "big")
    if value < 0x100000000:
        return bytes([(major << 5) | 26]) + value.to_bytes(4, "big")
    raise ValueError("value too large for the accepted CBOR profile")


def cbor_uint(value):
    return _head(0, int(value))


def cbor_text(text):
    raw = str(text).encode("utf-8")
    if len(raw) > CBOR_MAX_STR_BYTES:
        raise ValueError(
            "text is %d bytes, the firmware refuses anything over %d: %r"
            % (len(raw), CBOR_MAX_STR_BYTES, text[:40])
        )
    if b"\x00" in raw:
        raise ValueError("embedded NUL in text: %r" % (text[:40],))
    return _head(3, len(raw)) + raw


def cbor_array(items):
    if len(items) > CBOR_MAX_ITEMS:
        raise ValueError(
            "array has %d entries, the firmware refuses more than %d"
            % (len(items), CBOR_MAX_ITEMS)
        )
    return _head(4, len(items)) + b"".join(items)


def cbor_map(pairs):
    """pairs is a list of (key string, encoded value). Order is preserved.

    Key order is the order the field list below declares, not sorted order, so
    two runs over the same JSON produce byte-identical output. The decoder
    dispatches on the key and does not care about order.
    """
    if len(pairs) > CBOR_MAX_ITEMS:
        raise ValueError("map has too many entries")
    out = _head(5, len(pairs))
    for key, value in pairs:
        out += cbor_text(key) + value
    return out


def _scalar(value):
    """Encodes a JSON scalar the way the firmware's field readers expect.

    Enum-ish fields are left EXACTLY as authored: a name stays a text string
    and an ordinal stays an integer. The firmware resolves both, using the same
    name tables and the same fallbacks the JSON loader uses, so moving a pack
    to CBOR cannot change which line a typo selects.
    """
    if isinstance(value, bool):
        return bytes([0xF5 if value else 0xF4])
    if isinstance(value, int):
        if value < 0:
            raise ValueError("negative numbers are not supported in content")
        return cbor_uint(value)
    if isinstance(value, str):
        return cbor_text(value)
    raise ValueError("unsupported scalar in content: %r" % (value,))


# ── Per-kind JSON to CBOR normalisation ───────────────────────────────────
#
# The field names are the JSON field names, unchanged. The tolerance for
# "{ "lines": [...] } or a bare [...]" that the JSON loader has lives HERE, in
# the tool, so the on-device decoder can insist on exactly one shape.

DIALOGUE_FIELDS = ["text", "context", "trait", "stage", "form", "memory"]
QUEST_FIELDS = ["id", "text", "kind", "target", "xp", "bond", "requires"]
RECIPE_FIELDS = ["id", "output", "inputs", "stage"]


def _rows(doc, key):
    if isinstance(doc, list):
        return doc
    if isinstance(doc, dict) and isinstance(doc.get(key), list):
        return doc[key]
    raise ValueError('expected a list, or an object with a "%s" list' % key)


def _simple_rows(doc, key, fields):
    out = []
    for row in _rows(doc, key):
        if not isinstance(row, dict):
            raise ValueError("every entry must be an object")
        pairs = []
        for name in fields:
            if name in row and row[name] is not None:
                pairs.append((name, _scalar(row[name])))
        for name in row:
            if name not in fields:
                sys.stderr.write(
                    "  warning: dropping unknown field %r (the firmware would "
                    "ignore it anyway)\n" % (name,)
                )
        out.append(cbor_map(pairs))
    return out


def encode_dialogue(doc):
    return cbor_map([("lines", cbor_array(_simple_rows(doc, "lines", DIALOGUE_FIELDS)))])


def encode_quests(doc):
    rows = []
    for row in _rows(doc, "quests"):
        if not isinstance(row, dict):
            raise ValueError("every quest must be an object")
        pairs = []
        for name in QUEST_FIELDS:
            if name not in row or row[name] is None:
                continue
            if name == "requires":
                value = row[name]
                if isinstance(value, list):
                    pairs.append((name, cbor_array([_scalar(v) for v in value])))
                else:
                    pairs.append((name, _scalar(value)))
            else:
                pairs.append((name, _scalar(row[name])))
        rows.append(cbor_map(pairs))
    return cbor_map([("quests", cbor_array(rows))])


def encode_recipes(doc):
    rows = []
    for row in _rows(doc, "recipes"):
        if not isinstance(row, dict):
            raise ValueError("every recipe must be an object")
        pairs = []
        for name in RECIPE_FIELDS:
            if name not in row or row[name] is None:
                continue
            if name == "inputs":
                ins = []
                for entry in row[name]:
                    inner = []
                    if "item" in entry:
                        inner.append(("item", _scalar(entry["item"])))
                    if "qty" in entry:
                        inner.append(("qty", _scalar(entry["qty"])))
                    ins.append(cbor_map(inner))
                pairs.append((name, cbor_array(ins)))
            else:
                pairs.append((name, _scalar(row[name])))
        rows.append(cbor_map(pairs))
    return cbor_map([("recipes", cbor_array(rows))])


ENCODERS = {
    KIND_DIALOGUE: encode_dialogue,
    KIND_QUESTS: encode_quests,
    KIND_RECIPES: encode_recipes,
}


# ══ Envelope ══════════════════════════════════════════════════════════════


def build_pack(kind, body, sk, pk=None):
    if pk is None:
        pk = ed25519_public_key(sk)
    header = bytearray(HEADER_BYTES)
    header[0:4] = MAGIC
    header[4] = FORMAT_VERSION
    header[5] = kind
    header[6] = 0
    header[7] = 0
    header[8:12] = key_id(pk)
    header[12:16] = len(body).to_bytes(4, "little")
    sig = ed25519_sign(bytes(header) + body, sk, pk)
    return bytes(header) + sig + body


# ══ Key handling ══════════════════════════════════════════════════════════

KEY_WARNING = """\
# HexHound DEVELOPMENT content-signing key. NOT A RELEASE KEY.
#
# This file is the private half. It is gitignored and it must stay that way.
# Anyone holding it can mint content that every device carrying the matching
# public key will accept.
#
# A release key must be generated somewhere that is not a developer laptop and
# stored somewhere a repository cannot reach. Do not promote this one.
"""


def write_key(path, seed):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(KEY_WARNING)
        handle.write(seed.hex())
        handle.write("\n")
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass


def read_key(path):
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line and not line.startswith("#"):
                return bytes.fromhex(line)
    raise ValueError("no key material in %s" % path)


def emit_pubkey_header(pk, path, development=True):
    kid = key_id(pk)
    rows = []
    for i in range(0, 32, 8):
        rows.append("    " + " ".join("0x%02x," % b for b in pk[i:i + 8]))
    body = "\n".join(rows)
    text = """#pragma once

#include <stdint.h>

// ── HexHound - Trusted Content Signing Keys ──────────────────────
//
// GENERATED FILE. Regenerate with:
//     python scripts/sign_content_pack.py --gen-key
//
// These are PUBLIC keys. Nothing secret is in this file and nothing secret is
// ever allowed in it. The private halves live outside the repository; see
// docs/p3w2-wiring.md.
//
// A pack is accepted only when it is signed by a key in this list. Adding a
// key here is a firmware change, which is the point: content is data, but who
// may author content is code.
//
// ── THIS IS A DEVELOPMENT KEY ─────────────────────────────────────────────
// The key below was generated by a developer, on a developer machine, and its
// private half is sitting in a gitignored file on that machine. It is fine for
// bring-up and for the simulator. It must be replaced before any build is
// handed to anyone, because "the signature checks out" means nothing when the
// signing key is that easy to reach.

// Machine-readable provenance, read by the publish gate in
// scripts/deploy_web_flasher.py and not by the firmware. It compiles to
// nothing and exists so a tool can answer "is this a release key?" instead of
// a human having to notice a comment. An ABSENT define counts as development,
// because unknown provenance is not release provenance.
#define CONTENT_KEY_PROVENANCE_DEVELOPMENT %d

#define CONTENT_TRUSTED_KEY_COUNT 1

// Key id %s (first 4 bytes of SHA-512 of the key below). The id selects which
// key to try; it is not a credential and is never treated as one.
static const uint8_t CONTENT_TRUSTED_KEYS[CONTENT_TRUSTED_KEY_COUNT][32] = {
  {
%s
  }
};
""" % (1 if development else 0, kid.hex(), body)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


# ══ Self test ═════════════════════════════════════════════════════════════

RFC8032_VECTORS = [
    ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
     "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f"
     "b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
    ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
     "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da08"
     "5ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
    ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
     "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18"
     "ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
]


def self_test():
    failures = 0
    for index, (sk_hex, pk_hex, msg_hex, sig_hex) in enumerate(RFC8032_VECTORS, 1):
        sk = bytes.fromhex(sk_hex)
        msg = bytes.fromhex(msg_hex)
        pk = ed25519_public_key(sk)
        ok_pk = pk.hex() == pk_hex
        sig = ed25519_sign(msg, sk, pk)
        ok_sig = sig.hex() == sig_hex
        print("RFC 8032 TEST %d: public key %s, signature %s"
              % (index, "ok" if ok_pk else "FAIL", "ok" if ok_sig else "FAIL"))
        failures += (not ok_pk) + (not ok_sig)

    # The CBOR encoder has to produce the minimal head the decoder demands.
    checks = [
        (cbor_uint(0), b"\x00"),
        (cbor_uint(23), b"\x17"),
        (cbor_uint(24), b"\x18\x18"),
        (cbor_uint(255), b"\x18\xff"),
        (cbor_uint(256), b"\x19\x01\x00"),
        (cbor_text("hi"), b"\x62hi"),
        (cbor_array([cbor_uint(1)]), b"\x81\x01"),
        (cbor_map([("a", cbor_uint(1))]), b"\xa1\x61a\x01"),
    ]
    for got, want in checks:
        if got != want:
            print("CBOR encoding FAIL: got %s want %s" % (got.hex(), want.hex()))
            failures += 1
    print("CBOR canonical encoding: %s" % ("ok" if failures == 0 else "see above"))

    print("\n%s" % ("ALL PASS" if failures == 0 else "%d FAILURES" % failures))
    return 1 if failures else 0


# ══ Entry point ═══════════════════════════════════════════════════════════


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Sign a HexHound content pack.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--kind", choices=sorted(KIND_BY_NAME))
    parser.add_argument("--in", dest="source", help="authored JSON pack")
    parser.add_argument("--out", dest="dest", help="signed .hcp output")
    parser.add_argument("--key", default=DEFAULT_KEY_FILE,
                        help="private key file (default: %s)" % DEFAULT_KEY_FILE)
    parser.add_argument("--set-release-pubkey", metavar="HEX",
                        help="install a RELEASE public key (64 hex chars) whose "
                             "private half is held elsewhere and never enters "
                             "this repository; clears the development-key "
                             "publish gate")
    parser.add_argument("--gen-key", action="store_true",
                        help="generate a DEVELOPMENT keypair and the firmware "
                             "public-key header")
    parser.add_argument("--force", action="store_true",
                        help="overwrite an existing key file")
    parser.add_argument("--self-test", action="store_true",
                        help="run the RFC 8032 and CBOR vectors and exit")
    parser.add_argument("--emit-c-array", action="store_true",
                        help="also print the signed pack as a C byte array, "
                             "for embedding in a unit test")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.set_release_pubkey:
        raw = args.set_release_pubkey.strip().lower().replace(" ", "")
        if raw.startswith("0x"):
            raw = raw[2:]
        try:
            pk = bytes.fromhex(raw)
        except ValueError:
            print("--set-release-pubkey needs 64 hex characters (32 bytes)")
            return 1
        if len(pk) != 32:
            print("an Ed25519 public key is 32 bytes; got %d" % len(pk))
            return 1
        # PUBLIC half only, deliberately. --gen-key writes a private key onto
        # this machine, which is what makes its output a development key no
        # matter how it is handled afterwards. A release key is generated and
        # kept elsewhere; only these 32 bytes ever come here.
        emit_pubkey_header(pk, PUBKEY_HEADER, development=False)
        print("RELEASE content public key installed.")
        print("  public key  : %s" % pk.hex())
        print("  firmware    : %s" % PUBKEY_HEADER)
        print("No private key was written and none is needed here.")
        return 0

    if args.gen_key:
        if os.path.exists(args.key) and not args.force:
            print("refusing to overwrite %s (use --force)" % args.key)
            return 1
        seed = secrets.token_bytes(32)
        pk = ed25519_public_key(seed)
        write_key(args.key, seed)
        emit_pubkey_header(pk, PUBKEY_HEADER)
        print("DEVELOPMENT keypair generated.")
        print("  private key : %s   <- gitignored, NOT a release key" % args.key)
        print("  public key  : %s" % pk.hex())
        print("  key id      : %s" % key_id(pk).hex())
        print("  firmware    : %s (regenerated)" % PUBKEY_HEADER)
        print("")
        print("A release build must NOT ship this key. Generate the release key")
        print("off this machine and keep the private half out of the repository.")
        return 0

    missing = [name for name, value in
               (("--kind", args.kind), ("--in", args.source), ("--out", args.dest))
               if not value]
    if missing:
        parser.error("need %s (or --gen-key / --self-test)" % ", ".join(missing))

    kind = KIND_BY_NAME[args.kind]

    if not os.path.exists(args.key):
        print("no signing key at %s" % args.key)
        print("run: python scripts/sign_content_pack.py --gen-key")
        return 1
    seed = read_key(args.key)
    pk = ed25519_public_key(seed)

    with open(args.source, "r", encoding="utf-8") as handle:
        doc = json.load(handle)

    body = ENCODERS[kind](doc)
    pack = build_pack(kind, body, seed, pk)

    cap = MAX_PACK_BYTES[kind]
    if len(pack) > cap:
        print("pack is %d bytes, the firmware refuses anything over %d"
              % (len(pack), cap))
        print("trim the content; a pack this size would be rejected whole and")
        print("the device would silently keep its compiled-in baseline.")
        return 1

    out_dir = os.path.dirname(os.path.abspath(args.dest))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.dest, "wb") as handle:
        handle.write(pack)

    print("signed %s -> %s" % (args.source, args.dest))
    print("  kind      : %s (%d)" % (args.kind, kind))
    print("  body      : %d bytes CBOR" % len(body))
    print("  total     : %d bytes of %d allowed" % (len(pack), cap))
    print("  key id    : %s" % key_id(pk).hex())
    print("  public key: %s" % pk.hex())

    if args.emit_c_array:
        print("")
        print("static const uint8_t PACK[%d] = {" % len(pack))
        for i in range(0, len(pack), 12):
            print("    " + " ".join("0x%02x," % b for b in pack[i:i + 12]))
        print("};")

    return 0


if __name__ == "__main__":
    sys.exit(main())
