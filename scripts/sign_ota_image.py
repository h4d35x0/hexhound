#!/usr/bin/env python3
"""Generate the OTA signing key, and sign and verify HexHound firmware images.

SEPARATE KEY FROM CONTENT PACKS, deliberately. That was a considered call and
the reasoning is worth keeping next to the code: a leaked CONTENT key lets
somebody write dialogue, while a leaked FIRMWARE key lets somebody own the
device completely. Different blast radius means different custody and
different rotation, and the cost is one more key to look after.

The Ed25519 implementation is imported from sign_content_pack.py rather than
copied. Two copies of curve arithmetic is two places for a subtle bug to
live, and only one of them would ever get fixed.

Note that ota_image.cpp signs over a DOMAIN TAG ("HEXHOUND-FW-v1") followed
by the header prefix. That domain separation means a content pack signature
can never be replayed as a firmware signature even if the keys were ever
shared by mistake, which is a good belt-and-braces property to keep now that
they are deliberately separate.

Usage:
    python scripts/sign_ota_image.py --gen-key
    python scripts/sign_ota_image.py --self-test

    python scripts/sign_ota_image.py --sign .pio/build/<env>/firmware.bin \\
        --board lilygo-t-dongle-s3 --fw-version 0.4.0 --build 17 \\
        --out dist/hexhound-t-dongle-s3-0.4.0.hexfw

    python scripts/sign_ota_image.py --verify dist/hexhound-....hexfw

── Why this file also VERIFIES ──────────────────────────────────────────────

Because a signer nobody can check is a signer nobody should trust. The device
is the only other implementation of this format, and "it worked on the
hardware" is a test you can only run after you have already shipped the bytes.
--verify re-reads a finished .hexfw from disk and applies the same gate
ota_image.cpp applies, in the same order, reporting the same verdict names, so
a bad image is caught on the build machine instead of on a stranger's desk.

The verifier deliberately shares NOTHING with the signer except the layout
constants: it re-parses the file it is handed rather than remembering what was
just written, and it checks the signature with a public key rather than by
re-signing with the private one. A "verifier" that re-signs only proves the
signer is deterministic.
"""

import argparse
import binascii
import hashlib
import os
import re
import sys

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(PROJECT_DIR, "scripts"))

# Reuse, never reimplement.
from sign_content_pack import ed25519_public_key, ed25519_sign, key_id  # noqa: E402

# The curve arithmetic, for the VERIFY half. sign_content_pack.py has a signer
# and no verifier, because the content pipeline never needed one: the firmware
# verifies and the tool signs. A firmware signer does need one, and writing a
# second copy of the group law here to get it would be exactly the mistake the
# import above exists to avoid. These are underscore-private names in that
# module; reaching for them is uglier than duplicating them is dangerous.
from sign_content_pack import (  # noqa: E402
    BASE as _BASE,
    D as _CURVE_D,
    L as _GROUP_L,
    P as _FIELD_P,
    _edwards_add,
    _scalar_mult,
    _x_recover,
)

KEY_DIR = os.path.join(PROJECT_DIR, "keys")
KEY_PATH = os.path.join(KEY_DIR, "ota-dev-signing.key")
HEADER_PATH = os.path.join(PROJECT_DIR, "src", "ota", "ota_pubkey.h")

# ── The envelope, from src/ota/ota_image.h and src/ota/ota_image.cpp ───────
#
# These names mirror the OFF_* table in the anonymous namespace of
# ota_image.cpp one for one, deliberately, so a diff of that table against
# this block is a thing a person can actually do. The C++ comment claims the
# signing script "carries the same table"; this is that table.
#
#     off  len  field
#     0    8    magic "HEXHOTA1"
#     8    2    headerVersion, must be 1
#     10   2    flags, reserved, must be 0
#     12   4    imageLen
#     16   32   boardId, NUL-padded ASCII
#     48   4    fwVersion, major<<16 | minor<<8 | patch
#     52   4    buildNumber
#     56   4    keyId
#     60   64   imageDigest, SHA-512 of the image bytes
#     124  4    reserved, must be 0
#     128  64   Ed25519 signature
#
# Everything is little-endian. The signature at 128 is the only part of the
# header outside the signed prefix, which is what makes the layout work: you
# cannot sign a field that contains the signature.

OFF_MAGIC = 0
OFF_HDR_VERSION = 8
OFF_FLAGS = 10
OFF_IMAGE_LEN = 12
OFF_BOARD_ID = 16
OFF_FW_VERSION = 48
OFF_BUILD_NUMBER = 52
OFF_KEY_ID = 56
OFF_DIGEST = 60
OFF_RESERVED = 124
OFF_SIG = 128

OTA_HEADER_BYTES = 192
OTA_SIGNED_PREFIX_BYTES = 128
OTA_MAGIC = b"HEXHOTA1"
OTA_MAGIC_BYTES = 8
OTA_HEADER_VERSION = 1
OTA_BOARD_ID_BYTES = 32
OTA_DIGEST_BYTES = 64
OTA_SIG_BYTES = 64
OTA_KEYID_BYTES = 4
OTA_RESERVED_BYTES = 4

OTA_DOMAIN_TAG = b"HEXHOUND-FW-v1"
OTA_DOMAIN_TAG_BYTES = 16

# Must match OTA_MIN_IMAGE_BYTES in ota_image.h and HEXHOUND_OTA_SLOT_BYTES in
# ota_identity.h. The slot size is app0/app1 from default_16MB.csv; on hardware
# the real bound is read from the partition table and that one is authoritative,
# so this value exists to catch an oversized build on the machine that made it.
OTA_MIN_IMAGE_BYTES = 1024
OTA_SLOT_BYTES = 0x640000

# The verdict vocabulary, copied from OtaImage::Verdict so the build machine and
# the device describe the same refusal with the same word. Anything the device
# has no enum value for is prefixed host_, so nobody can mistake a host-only
# finding for something the firmware would report.
V_ACCEPTED = "accepted"
V_SHORT_HEADER = "short_header"
V_BAD_MAGIC = "bad_magic"
V_BAD_HEADER_VERSION = "bad_header_version"
V_BAD_FLAGS = "bad_flags"
V_BAD_RESERVED = "bad_reserved"
V_BOARD_MISMATCH = "board_mismatch"
V_IMAGE_TOO_SMALL = "image_too_small"
V_IMAGE_TOO_LARGE = "image_too_large"
V_UNKNOWN_KEY = "unknown_key"
V_BAD_SIGNATURE = "bad_signature"
V_DIGEST_MISMATCH = "digest_mismatch"

# Host-only. The device has no Verdict for this because it never sees a file:
# it is handed a header, then a stream, and a stream that does not deliver
# exactly imageLen bytes is refused by OtaSession rather than by OtaImage. On
# disk the same disagreement is visible up front, so it is caught up front.
V_HOST_LENGTH_MISMATCH = "host_image_length_mismatch"

HEADER_TEMPLATE = """#pragma once

#include <stdint.h>

// ── HexHound - Trusted OTA Signing Keys ──────────────────────────
//
// GENERATED FILE. Regenerate with:
//     python scripts/sign_ota_image.py --gen-key
//
// These are PUBLIC keys. Nothing secret is in this file and nothing secret is
// ever allowed in it.
//
// SEPARATE FROM THE CONTENT PACK KEY, deliberately. A leaked content key lets
// somebody write dialogue; a leaked firmware key lets somebody own the device.
// Different blast radius, so different custody and different rotation.
//
// An image is accepted only when it is signed by a key in this list. Adding a
// key here is a firmware change, which is the point: who may replace this
// firmware is not data.
//
// ── THIS IS A DEVELOPMENT KEY ─────────────────────────────────────────────
// Generated by a developer, on a developer machine, with its private half in
// a gitignored file on that machine. Fine for bring-up. It MUST be replaced
// before any build reaches anybody, because "the signature checks out" means
// nothing when the signing key is that easy to reach. An OTA key is the most
// valuable key in this project: it authorises replacing the firmware.

// Machine-readable provenance. Read by the publish gate in
// scripts/deploy_web_flasher.py, NOT by the firmware: it compiles to nothing,
// costs zero bytes, and exists so that "is this a release key?" is a question
// a tool can answer instead of a comment a human has to notice.
//
// 1 means the private half was generated by whoever ran --gen-key, onto that
// machine's disk. 0 means a release public key was installed with
// --set-release-pubkey and its private half has never been in this repository.
// An ABSENT define is treated as 1 by the gate, because unknown provenance is
// not release provenance.
#define OTA_KEY_PROVENANCE_DEVELOPMENT {devkey}

#define OTA_TRUSTED_KEY_COUNT {count}

// Key id {keyid} (first 4 bytes of SHA-512 of the key below). The id selects
// which key to try; it is a selector and never a credential.
static const uint8_t OTA_TRUSTED_KEY_IDS[OTA_TRUSTED_KEY_COUNT][{idbytes}] = {{
  {{ {idrow} }},
}};

static const uint8_t OTA_TRUSTED_KEYS[OTA_TRUSTED_KEY_COUNT][32] = {{
  {{
{keyrows}
  }},
}};
"""


def _fmt_bytes(data, per_line=8, indent="    "):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append(indent + ", ".join("0x%02x" % b for b in chunk) + ",")
    return "\n".join(lines).rstrip(",")


# ══ Ed25519 verification ══════════════════════════════════════════════════
#
# The signing half is imported. This is the half that does not exist anywhere
# else in the repository's Python, and it is written against RFC 8032 section
# 5.1.7 using the imported group law, so there is still exactly one copy of the
# curve arithmetic in this tree.


def _on_curve(point):
    x, y = point
    return (-x * x + y * y - 1 - _CURVE_D * x * x * y * y) % _FIELD_P == 0


def _decode_point(raw):
    """Decompress a 32-byte encoded point, or raise.

    Raising rather than returning a garbage point matters: a signature carrying
    an un-decodable R is a malformed signature, not a signature over different
    data, and it must fail rather than be compared against something.
    """
    if len(raw) != 32:
        raise ValueError("point must be 32 bytes")
    value = int.from_bytes(raw, "little")
    sign = value >> 255
    y = value & ((1 << 255) - 1)
    if y >= _FIELD_P:
        raise ValueError("non-canonical y coordinate")
    x = _x_recover(y)
    if (x & 1) != sign:
        x = _FIELD_P - x
    point = (x, y)
    if not _on_curve(point):
        # _x_recover returns a value unconditionally, including when no square
        # root exists, so this check is what actually rejects a bogus encoding.
        raise ValueError("point is not on the curve")
    return point


def ed25519_verify(msg, sig, pk):
    """True when `sig` is a valid Ed25519 signature over `msg` under `pk`."""
    if len(sig) != OTA_SIG_BYTES or len(pk) != 32:
        return False
    try:
        big_r = _decode_point(sig[:32])
        a_point = _decode_point(pk)
    except ValueError:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= _GROUP_L:
        # A non-canonical scalar. Rejecting it keeps one signature per message
        # per key, so nobody can hand two distinct byte strings to two
        # different verifiers and have both say yes about the same image.
        return False
    k = int.from_bytes(hashlib.sha512(sig[:32] + pk + msg).digest(), "little") % _GROUP_L
    return _scalar_mult(_BASE, s) == _edwards_add(big_r, _scalar_mult(a_point, k))


# ══ Header construction ═══════════════════════════════════════════════════


def _u16(value):
    return int(value).to_bytes(2, "little")


def _u32(value):
    return int(value).to_bytes(4, "little")


def pack_fw_version(text):
    """"0.4.0" -> 0x00000400, matching HEXHOUND_FW_VERSION in ota_identity.h."""
    parts = str(text).split(".")
    if len(parts) != 3:
        raise ValueError("firmware version must be major.minor.patch, got %r" % (text,))
    try:
        major, minor, patch = (int(p, 10) for p in parts)
    except ValueError:
        raise ValueError("firmware version components must be decimal: %r" % (text,))
    for name, value in (("major", major), ("minor", minor), ("patch", patch)):
        if not 0 <= value <= 255:
            # The firmware packs these as byte fields and the companion app
            # unpacks them the same way. A 256 here would silently reappear as
            # a carry into the field above it.
            raise ValueError("%s must be 0..255, got %d" % (name, value))
    return (major << 16) | (minor << 8) | patch


def unpack_fw_version(value):
    return "%d.%d.%d" % ((value >> 16) & 0xFFFF, (value >> 8) & 0xFF, value & 0xFF)


def encode_board_id(board_id):
    """NUL-padded ASCII in a fixed 32-byte field.

    Exactly 32 characters is legal and is not an error: the longest board id in
    ota_identity.h, "waveshare-esp32-s3-touch-lcd-147", is 32 characters and
    therefore carries no NUL at all. The device copies the field into a buffer
    one byte longer and zero-fills it first, which is what makes that safe. A
    signer that insisted on room for a terminator would be unable to sign for
    that board.
    """
    raw = board_id.encode("ascii")  # deliberately not utf-8; the field is ASCII
    if not raw:
        raise ValueError("board id must not be empty")
    if len(raw) > OTA_BOARD_ID_BYTES:
        raise ValueError(
            "board id %r is %d bytes, the field is %d"
            % (board_id, len(raw), OTA_BOARD_ID_BYTES))
    if b"\x00" in raw:
        raise ValueError("board id must not contain a NUL")
    return raw + b"\x00" * (OTA_BOARD_ID_BYTES - len(raw))


def signed_message(prefix):
    """The exact bytes the signature covers.

    OTA_DOMAIN_TAG is 14 characters padded with NULs to 16, then the first 128
    bytes of the header. 144 bytes total, and not one byte of the image: the
    image is bound in by imageLen and imageDigest, both of which live inside
    the prefix.
    """
    if len(prefix) != OTA_SIGNED_PREFIX_BYTES:
        raise ValueError("signed prefix must be exactly %d bytes"
                         % OTA_SIGNED_PREFIX_BYTES)
    tag = OTA_DOMAIN_TAG + b"\x00" * (OTA_DOMAIN_TAG_BYTES - len(OTA_DOMAIN_TAG))
    return tag + bytes(prefix)


def build_header(sk, image_digest, image_len, board_id, fw_version, build_number,
                 pk=None, magic=OTA_MAGIC, header_version=OTA_HEADER_VERSION,
                 flags=0, reserved=b"\x00" * OTA_RESERVED_BYTES,
                 key_id_override=None):
    """The 192-byte header, signed.

    The overrides exist for ONE caller: gen_ota_test_fixtures.py, which has to
    be able to produce a header that is correctly signed and structurally
    illegal. That combination is the only way to prove the structural checks in
    ota_image.cpp are real gates rather than things the signature check happens
    to catch on their behalf. Nothing in the normal signing path passes them.
    """
    if pk is None:
        pk = ed25519_public_key(sk)
    if len(image_digest) != OTA_DIGEST_BYTES:
        raise ValueError("image digest must be %d bytes" % OTA_DIGEST_BYTES)
    if not 0 <= image_len < 2 ** 32:
        raise ValueError("image length does not fit a uint32")

    header = bytearray(OTA_HEADER_BYTES)
    header[OFF_MAGIC:OFF_MAGIC + OTA_MAGIC_BYTES] = magic
    header[OFF_HDR_VERSION:OFF_HDR_VERSION + 2] = _u16(header_version)
    header[OFF_FLAGS:OFF_FLAGS + 2] = _u16(flags)
    header[OFF_IMAGE_LEN:OFF_IMAGE_LEN + 4] = _u32(image_len)
    header[OFF_BOARD_ID:OFF_BOARD_ID + OTA_BOARD_ID_BYTES] = encode_board_id(board_id)
    header[OFF_FW_VERSION:OFF_FW_VERSION + 4] = _u32(fw_version)
    header[OFF_BUILD_NUMBER:OFF_BUILD_NUMBER + 4] = _u32(build_number)
    header[OFF_KEY_ID:OFF_KEY_ID + OTA_KEYID_BYTES] = (
        key_id_override if key_id_override is not None else key_id(pk))
    header[OFF_DIGEST:OFF_DIGEST + OTA_DIGEST_BYTES] = image_digest
    header[OFF_RESERVED:OFF_RESERVED + OTA_RESERVED_BYTES] = reserved

    # Signed last, over the prefix as it now stands. Every field above is
    # inside that prefix, so editing any of them in transit breaks this.
    sig = ed25519_sign(signed_message(header[:OTA_SIGNED_PREFIX_BYTES]), sk, pk)
    header[OFF_SIG:OFF_SIG + OTA_SIG_BYTES] = sig
    return bytes(header)


def build_image(sk, image, board_id, fw_version, build_number, pk=None, **kwargs):
    """A complete .hexfw: 192-byte signed header followed by the raw image."""
    digest = hashlib.sha512(image).digest()
    header = build_header(sk, digest, len(image), board_id, fw_version,
                          build_number, pk=pk, **kwargs)
    return header + bytes(image)


# ══ Parsing and verification ══════════════════════════════════════════════


class Parsed(object):
    """Whatever could be read out of the file, whether or not it is acceptable.

    Populated as far as parsing got, so a failure can be REPORTED with context
    instead of just refused. Nothing on this object is trustworthy until
    verify() has returned accepted.
    """

    def __init__(self):
        self.magic = b""
        self.header_version = 0
        self.flags = 0
        self.image_len = 0
        self.board_id = ""
        self.fw_version = 0
        self.build_number = 0
        self.key_id = b""
        self.image_digest = b""
        self.reserved = b""
        self.sig = b""
        self.signed_prefix = b""
        self.body_len = 0
        self.body_digest = b""


def parse(blob):
    """Structural read of a .hexfw. Returns (verdict, Parsed)."""
    info = Parsed()
    if len(blob) < OTA_HEADER_BYTES:
        return V_SHORT_HEADER, info

    info.magic = bytes(blob[OFF_MAGIC:OFF_MAGIC + OTA_MAGIC_BYTES])
    if info.magic != OTA_MAGIC:
        return V_BAD_MAGIC, info

    info.header_version = int.from_bytes(
        blob[OFF_HDR_VERSION:OFF_HDR_VERSION + 2], "little")
    if info.header_version != OTA_HEADER_VERSION:
        return V_BAD_HEADER_VERSION, info

    info.flags = int.from_bytes(blob[OFF_FLAGS:OFF_FLAGS + 2], "little")
    if info.flags != 0:
        return V_BAD_FLAGS, info

    info.reserved = bytes(blob[OFF_RESERVED:OFF_RESERVED + OTA_RESERVED_BYTES])
    if info.reserved != b"\x00" * OTA_RESERVED_BYTES:
        return V_BAD_RESERVED, info

    info.image_len = int.from_bytes(blob[OFF_IMAGE_LEN:OFF_IMAGE_LEN + 4], "little")
    info.fw_version = int.from_bytes(blob[OFF_FW_VERSION:OFF_FW_VERSION + 4], "little")
    info.build_number = int.from_bytes(
        blob[OFF_BUILD_NUMBER:OFF_BUILD_NUMBER + 4], "little")
    raw_board = bytes(blob[OFF_BOARD_ID:OFF_BOARD_ID + OTA_BOARD_ID_BYTES])
    info.board_id = raw_board.split(b"\x00", 1)[0].decode("ascii", "replace")
    info.key_id = bytes(blob[OFF_KEY_ID:OFF_KEY_ID + OTA_KEYID_BYTES])
    info.image_digest = bytes(blob[OFF_DIGEST:OFF_DIGEST + OTA_DIGEST_BYTES])
    info.sig = bytes(blob[OFF_SIG:OFF_SIG + OTA_SIG_BYTES])
    info.signed_prefix = bytes(blob[:OTA_SIGNED_PREFIX_BYTES])

    body = bytes(blob[OTA_HEADER_BYTES:])
    info.body_len = len(body)
    info.body_digest = hashlib.sha512(body).digest()
    return V_ACCEPTED, info


def verify(blob, trusted, expect_board=None, slot_bytes=OTA_SLOT_BYTES):
    """Apply the device's gate to a finished .hexfw. Returns (verdict, Parsed).

    `trusted` maps a 4-byte key id to a 32-byte public key, which is the same
    shape OTA_TRUSTED_KEY_IDS / OTA_TRUSTED_KEYS have in the firmware.

    The order is the order in ota_image.cpp's accept(): parse, then fit, then
    signature, then the digest. It matters that this matches, because the
    verdict a build engineer sees here should be the verdict the owner would
    have seen, and a tool that checks the signature first would report
    bad_signature for an image whose real problem is that it is for the wrong
    board.
    """
    verdict, info = parse(blob)
    if verdict != V_ACCEPTED:
        return verdict, info

    if expect_board is not None and info.board_id != expect_board:
        return V_BOARD_MISMATCH, info
    if info.image_len < OTA_MIN_IMAGE_BYTES:
        return V_IMAGE_TOO_SMALL, info
    if slot_bytes == 0 or info.image_len > slot_bytes:
        return V_IMAGE_TOO_LARGE, info

    # Host-only, and checked before the crypto so a truncated download is
    # reported as a truncated download rather than as a signature failure.
    if info.body_len != info.image_len:
        return V_HOST_LENGTH_MISMATCH, info

    pk = trusted.get(info.key_id)
    if pk is None:
        return V_UNKNOWN_KEY, info
    if not ed25519_verify(signed_message(info.signed_prefix), info.sig, pk):
        return V_BAD_SIGNATURE, info

    # Last, and on the bytes that are actually here. The device does this
    # against a digest read back OFF FLASH rather than off the wire; on disk
    # the file is the artefact, so the file is what gets hashed.
    if info.body_digest != info.image_digest:
        return V_DIGEST_MISMATCH, info

    return V_ACCEPTED, info


# ══ Keys ══════════════════════════════════════════════════════════════════


def read_key(path):
    """Read a 32-byte Ed25519 seed.

    Two formats are accepted, because two exist. --gen-key in THIS file writes
    the raw 32 bytes; sign_content_pack.py writes hex text with a comment
    banner. A signer that only understood its own output would fail
    confusingly on a key file that is perfectly good, so it understands both.
    """
    with open(path, "rb") as handle:
        raw = handle.read()
    if len(raw) == 32:
        return raw
    for line in raw.decode("utf-8", "replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            seed = bytes.fromhex(line)
        except ValueError:
            break
        if len(seed) == 32:
            return seed
        break
    raise ValueError(
        "%s is not a 32-byte Ed25519 seed: expected 32 raw bytes or 64 hex "
        "characters, got %d bytes" % (path, len(raw)))


_KEY_ARRAY_RE = re.compile(
    r"OTA_TRUSTED_KEYS\s*\[[^\]]*\]\s*\[\s*32\s*\]\s*=\s*\{(.*?)\n\}\s*;",
    re.S)
_KEYID_ARRAY_RE = re.compile(
    r"OTA_TRUSTED_KEY_IDS\s*\[[^\]]*\]\s*\[[^\]]*\]\s*=\s*\{(.*?)\n\}\s*;",
    re.S)
_BYTE_RE = re.compile(r"0[xX]([0-9a-fA-F]{1,2})")


def _bytes_from_c(text, chunk):
    flat = bytes(int(m.group(1), 16) for m in _BYTE_RE.finditer(text))
    if len(flat) % chunk != 0:
        raise ValueError("expected a multiple of %d bytes, found %d"
                         % (chunk, len(flat)))
    return [flat[i:i + chunk] for i in range(0, len(flat), chunk)]


def read_trusted_keys(path=HEADER_PATH):
    """The compiled-in trusted list, read straight out of ota_pubkey.h.

    READ ONLY. This function must never write that file. It is parsed rather
    than duplicated so --verify answers the question a person actually has,
    which is not "is this signature valid" but "would a device running this
    firmware install this". Those differ the moment a key is rotated, and the
    difference is the whole reason the check is worth running.
    """
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    keys_match = _KEY_ARRAY_RE.search(text)
    ids_match = _KEYID_ARRAY_RE.search(text)
    if not keys_match or not ids_match:
        raise ValueError("could not find the trusted key arrays in %s" % path)
    keys = _bytes_from_c(keys_match.group(1), 32)
    ids = _bytes_from_c(ids_match.group(1), OTA_KEYID_BYTES)
    if len(keys) != len(ids):
        raise ValueError("%s lists %d keys but %d key ids"
                         % (path, len(keys), len(ids)))
    trusted = {}
    for kid, pk in zip(ids, keys):
        derived = key_id(pk)
        if derived != kid:
            # The id is derived from the key, so a mismatch means the header
            # was hand-edited. Say so instead of silently trusting one of them.
            raise ValueError(
                "%s: key id %s does not match SHA-512 of its key (%s). "
                "Regenerate it with --gen-key rather than editing it."
                % (path, kid.hex(), derived.hex()))
        trusted[kid] = pk
    return trusted


def set_release_pubkey(pubkey_hex):
    """Install a RELEASE public key, whose private half never comes near here.

    This is the whole point of the function existing separately from
    --gen-key. --gen-key mints a private key onto this machine's disk, which is
    exactly what makes it a development key no matter how carefully it is
    handled afterwards. A release key has to be generated somewhere else, kept
    somewhere else, and only its PUBLIC half ever enters this repository.

    So this takes 32 bytes of public key and nothing else. There is no code
    path here that can write a release private key, because there is no
    release private key to write.
    """
    raw = pubkey_hex.strip().lower().replace(" ", "")
    if raw.startswith("0x"):
        raw = raw[2:]
    try:
        pk = bytes.fromhex(raw)
    except ValueError:
        sys.exit("--set-release-pubkey needs 64 hex characters (32 bytes), "
                 "not %r" % pubkey_hex)
    if len(pk) != 32:
        sys.exit("an Ed25519 public key is 32 bytes; got %d" % len(pk))
    if not _on_curve(_decode_point(pk)):
        sys.exit("that is not a valid Ed25519 public key: the point is not on "
                 "the curve.\nA typo here would ship a device that can never "
                 "accept an update.")

    kid = key_id(pk)
    header = HEADER_TEMPLATE.format(
        count=1,
        devkey=0,
        keyid=kid.hex(),
        idbytes=OTA_KEYID_BYTES,
        idrow=", ".join("0x%02x" % b for b in kid),
        keyrows=_fmt_bytes(pk),
    )
    with open(HEADER_PATH, "w", newline="\n", encoding="utf-8") as fh:
        fh.write(header)

    print("RELEASE OTA public key installed.")
    print("  public key  : %s" % pk.hex())
    print("  key id      : %s" % kid.hex())
    print("  firmware    : %s" % HEADER_PATH)
    print()
    print("No private key was written and none is needed here. Signing a")
    print("release image happens wherever that private half is held.")
    print("The publish gate will now stop calling these images development-keyed.")
    return 0


def gen_key(force):
    os.makedirs(KEY_DIR, exist_ok=True)
    if os.path.exists(KEY_PATH) and not force:
        sys.exit("refusing to overwrite %s without --force\n"
                 "an OTA key is the most valuable key in this project; losing\n"
                 "it means every device trusting it can no longer be updated"
                 % KEY_PATH)

    sk = os.urandom(32)
    pk = ed25519_public_key(sk)
    kid = key_id(pk)

    with open(KEY_PATH, "wb") as fh:
        fh.write(sk)
    try:
        os.chmod(KEY_PATH, 0o600)
    except OSError:
        pass  # best effort on Windows

    header = HEADER_TEMPLATE.format(
        count=1,
        devkey=1,
        keyid=kid.hex(),
        idbytes=OTA_KEYID_BYTES,
        idrow=", ".join("0x%02x" % b for b in kid),
        keyrows=_fmt_bytes(pk),
    )
    # encoding is explicit: the header comment uses box-drawing characters and
    # Windows defaults to cp1252, which cannot encode them.
    with open(HEADER_PATH, "w", newline="\n", encoding="utf-8") as fh:
        fh.write(header)

    print("DEVELOPMENT OTA keypair generated.")
    print("  private key : %s   <- gitignored, NOT a release key" % KEY_PATH)
    print("  public key  : %s" % pk.hex())
    print("  key id      : %s" % kid.hex())
    print("  firmware    : %s" % HEADER_PATH)
    print()
    print("This key is SEPARATE from the content pack key, by design.")
    print("A release build must NOT ship it. Generate the release key off")
    print("this machine and keep the private half out of the repository.")
    print()
    print("Every image ever signed by the previous key is now UNINSTALLABLE on")
    print("a device flashed with this header. Re-sign anything still in use,")
    print("and regenerate the OTA test fixtures if the tests reference it.")
    return 0


# ══ Commands ══════════════════════════════════════════════════════════════


def cmd_sign(args):
    if not os.path.exists(args.key):
        print("no signing key at %s" % args.key)
        print("run: python scripts/sign_ota_image.py --gen-key")
        return 1

    with open(args.sign, "rb") as handle:
        image = handle.read()

    if len(image) < OTA_MIN_IMAGE_BYTES:
        # Refused here rather than at install time. A device that rejects an
        # image is a person confused on the far side of a transfer; a build
        # machine that rejects it is a person who can fix it.
        print("%s is %d bytes, which is below OTA_MIN_IMAGE_BYTES (%d)."
              % (args.sign, len(image), OTA_MIN_IMAGE_BYTES))
        print("The device would refuse this as image_too_small. Signing it")
        print("would only move the failure somewhere less useful.")
        return 1
    if len(image) > args.slot_bytes:
        print("%s is %d bytes and the OTA slot is %d."
              % (args.sign, len(image), args.slot_bytes))
        print("The device would refuse this as image_too_large.")
        return 1
    if image[:1] != b"\xe9":
        # 0xE9 is the ESP32 image magic. This is a warning and not a refusal:
        # the envelope carries whatever it is told to carry, and a test payload
        # is a legitimate thing to sign. But signing a .elf or an already
        # wrapped .hexfw by mistake is easy, and silence would not help.
        print("warning: %s does not start with the ESP32 image magic 0xE9."
              % args.sign)
        print("         Signing it anyway; check you passed firmware.bin and")
        print("         not an .elf or an already-signed .hexfw.")

    seed = read_key(args.key)
    pk = ed25519_public_key(seed)
    fw_version = pack_fw_version(args.fw_version)

    blob = build_image(seed, image, args.board, fw_version, args.build, pk=pk)

    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "wb") as handle:
        handle.write(blob)

    # Verified before the tool claims success, with the public key and the
    # parser rather than with anything the signer remembers. It costs one more
    # Ed25519 verify and it means "signed" is never printed over a file the
    # device would refuse.
    verdict, info = verify(blob, {key_id(pk): pk}, expect_board=args.board,
                           slot_bytes=args.slot_bytes)
    if verdict != V_ACCEPTED:
        print("INTERNAL ERROR: the file just written does not verify (%s)."
              % verdict)
        print("Nothing about %s should be trusted. This is a bug in the signer."
              % args.out)
        return 2

    print("signed %s -> %s" % (args.sign, args.out))
    print("  board      : %s" % args.board)
    print("  fw version : %s (0x%06x)" % (args.fw_version, fw_version))
    print("  build      : %d" % args.build)
    print("  image      : %d bytes of %d in the slot" % (len(image), args.slot_bytes))
    print("  digest     : %s..." % info.image_digest.hex()[:32])
    print("  key id     : %s" % key_id(pk).hex())
    print("  public key : %s" % pk.hex())
    print("  total      : %d bytes (%d header + %d image)"
          % (len(blob), OTA_HEADER_BYTES, len(image)))
    print("  verified   : accepted (re-parsed and checked against the public key)")
    return 0


def _trusted_for_verify(args):
    """Which keys --verify treats as trusted, and where they came from."""
    if args.pubkey:
        trusted = {}
        for text in args.pubkey:
            try:
                pk = bytes.fromhex(text.strip())
            except (ValueError, binascii.Error):
                sys.exit("--pubkey must be 64 hex characters, got %r" % text)
            if len(pk) != 32:
                sys.exit("--pubkey must be 32 bytes, got %d" % len(pk))
            trusted[key_id(pk)] = pk
        return trusted, "--pubkey (%d key%s)" % (len(trusted),
                                                 "" if len(trusted) == 1 else "s")
    return read_trusted_keys(), "%s (the compiled-in trusted list)" % (
        os.path.relpath(HEADER_PATH, PROJECT_DIR).replace("\\", "/"))


def cmd_verify(args):
    with open(args.verify, "rb") as handle:
        blob = handle.read()

    trusted, source = _trusted_for_verify(args)
    verdict, info = verify(blob, trusted, expect_board=args.expect_board,
                           slot_bytes=args.slot_bytes)

    print("%s" % args.verify)
    print("  file       : %d bytes" % len(blob))
    print("  trusting   : %s" % source)
    if info.magic:
        print("  magic      : %r" % info.magic)
    if verdict not in (V_SHORT_HEADER, V_BAD_MAGIC):
        print("  header ver : %d" % info.header_version)
        print("  flags      : 0x%04x" % info.flags)
        print("  board      : %r" % info.board_id)
        print("  fw version : %s (0x%06x)"
              % (unpack_fw_version(info.fw_version), info.fw_version))
        print("  build      : %d" % info.build_number)
        print("  key id     : %s" % info.key_id.hex())
        print("  image len  : %d declared, %d present"
              % (info.image_len, info.body_len))
        print("  digest     : %s..." % info.image_digest.hex()[:32])
        print("  body sha512: %s..." % info.body_digest.hex()[:32])
    print("  verdict    : %s" % verdict)

    if verdict == V_ACCEPTED:
        print("\nACCEPTED. A device carrying one of those keys would install this.")
        return 0
    print("\nREJECTED: %s" % verdict)
    print("This file must not be handed to a device. It would be refused, and")
    print("if it were not refused it would be the refusal that is broken.")
    return 1


# ══ Self test ═════════════════════════════════════════════════════════════


def self_test():
    failures = 0

    # The same RFC 8032 vector the content signer uses, run through the
    # imported implementation, so this script proves it is actually reaching
    # working crypto rather than assuming the import succeeded.
    sk = bytes.fromhex(
        "9d61b19deffd5a60ba844af492ec2cc4"
        "4449c5697b326919703bac031cae7f60")
    expect_pk = bytes.fromhex(
        "d75a980182b10ab7d54bfed3c964073a"
        "0ee172f3daa62325af021a68f707511a")
    pk = ed25519_public_key(sk)
    if pk != expect_pk:
        print("FAIL: public key mismatch")
        failures += 1
    sig = ed25519_sign(b"", sk, pk)
    expect_sig = bytes.fromhex(
        "e5564300c360ac729086e2cc806e828a"
        "84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46b"
        "d25bf5f0595bbe24655141438e7a100b")
    if sig != expect_sig:
        print("FAIL: signature mismatch")
        failures += 1
    if failures == 0:
        print("RFC 8032 TEST 1: public key ok, signature ok")

    # The verifier is new code in this file, so it gets checked against the
    # same vector from the other direction, including two negatives. A verifier
    # that says yes to everything passes every positive test ever written.
    if not ed25519_verify(b"", expect_sig, expect_pk):
        print("FAIL: verify rejected the RFC 8032 vector")
        failures += 1
    if ed25519_verify(b"x", expect_sig, expect_pk):
        print("FAIL: verify accepted a signature over a different message")
        failures += 1
    tampered = bytearray(expect_sig)
    tampered[0] ^= 0x01
    if ed25519_verify(b"", bytes(tampered), expect_pk):
        print("FAIL: verify accepted a tampered signature")
        failures += 1
    if failures == 0:
        print("RFC 8032 TEST 1: verify ok, wrong message rejected, "
              "tampered signature rejected")

    # ── Full round trip ───────────────────────────────────────────────────
    #
    # A THROWAWAY key, generated here and never written anywhere. The real OTA
    # key is not touched, not read and not required, so --self-test is safe to
    # run on a machine that has never held one.
    throwaway = hashlib.sha512(b"hexhound ota self-test throwaway seed").digest()[:32]
    tpk = ed25519_public_key(throwaway)
    trusted = {key_id(tpk): tpk}
    board = "lilygo-t-dongle-s3"
    image = bytes((i * 31 + 7) & 0xFF for i in range(OTA_MIN_IMAGE_BYTES + 512))

    blob = build_image(throwaway, image, board, pack_fw_version("0.4.0"), 42, pk=tpk)

    checks = []
    if len(blob) != OTA_HEADER_BYTES + len(image):
        print("FAIL: signed file is %d bytes, expected %d"
              % (len(blob), OTA_HEADER_BYTES + len(image)))
        failures += 1

    verdict, info = verify(blob, trusted, expect_board=board)
    checks.append(("round trip verifies", verdict == V_ACCEPTED, verdict))
    checks.append(("header is 192 bytes",
                   len(blob) - len(image) == OTA_HEADER_BYTES, len(blob) - len(image)))
    checks.append(("image length is signed", info.image_len == len(image),
                   info.image_len))
    checks.append(("board round-trips", info.board_id == board, info.board_id))
    checks.append(("fw version round-trips",
                   unpack_fw_version(info.fw_version) == "0.4.0",
                   unpack_fw_version(info.fw_version)))
    checks.append(("build number round-trips", info.build_number == 42,
                   info.build_number))
    checks.append(("key id is SHA-512(pubkey)[:4]", info.key_id == key_id(tpk),
                   info.key_id.hex()))
    checks.append(("digest is SHA-512 of the image",
                   info.image_digest == hashlib.sha512(image).digest(),
                   info.image_digest.hex()[:16]))
    checks.append(("reserved bytes are zero",
                   blob[OFF_RESERVED:OFF_RESERVED + OTA_RESERVED_BYTES] ==
                   b"\x00" * OTA_RESERVED_BYTES,
                   blob[OFF_RESERVED:OFF_RESERVED + OTA_RESERVED_BYTES].hex()))
    checks.append(("signature is outside the signed prefix", OFF_SIG ==
                   OTA_SIGNED_PREFIX_BYTES, OFF_SIG))

    # Negatives. Each one flips exactly one byte, in a different region, and
    # asserts the SPECIFIC verdict rather than just "not accepted": a verifier
    # that returns bad_magic for everything would pass a weaker test.
    def mutate(offset, verdict_expected, label, xor=0x01):
        bad = bytearray(blob)
        bad[offset] ^= xor
        got, _ = verify(bytes(bad), trusted, expect_board=board)
        checks.append((label, got == verdict_expected, got))

    mutate(OFF_MAGIC, V_BAD_MAGIC, "a flipped magic byte is refused")
    mutate(OFF_HDR_VERSION, V_BAD_HEADER_VERSION, "a bumped header version is refused")
    mutate(OFF_FLAGS, V_BAD_FLAGS, "an unknown flag bit is refused")
    mutate(OFF_RESERVED, V_BAD_RESERVED, "a non-zero reserved byte is refused")
    mutate(OFF_BOARD_ID, V_BOARD_MISMATCH, "an edited board id is refused")
    mutate(OFF_DIGEST, V_BAD_SIGNATURE, "an edited digest breaks the signature")
    mutate(OFF_BUILD_NUMBER, V_BAD_SIGNATURE, "an edited build number breaks the signature")
    mutate(OFF_SIG, V_BAD_SIGNATURE, "a flipped signature byte is refused")
    mutate(OTA_HEADER_BYTES, V_DIGEST_MISMATCH, "a flipped image byte is refused")
    mutate(OTA_HEADER_BYTES + len(image) - 1, V_DIGEST_MISMATCH,
           "a flipped byte in the last image block is refused")

    truncated, _ = verify(blob[:-1], trusted, expect_board=board)
    checks.append(("a truncated image is refused",
                   truncated == V_HOST_LENGTH_MISMATCH, truncated))
    stub, _ = verify(blob[:OTA_HEADER_BYTES - 1], trusted, expect_board=board)
    checks.append(("a header-only fragment is refused",
                   stub == V_SHORT_HEADER, stub))

    # Signed by a key nobody trusts, honestly labelled with its own id.
    other = hashlib.sha512(b"hexhound ota self-test other seed").digest()[:32]
    other_blob = build_image(other, image, board, pack_fw_version("0.4.0"), 42)
    got, _ = verify(other_blob, trusted, expect_board=board)
    checks.append(("an untrusted key is refused", got == V_UNKNOWN_KEY, got))

    # Signed by a key nobody trusts, LYING about which key signed it. The id is
    # a selector, so this must die on the signature and not on the id.
    liar = build_image(other, image, board, pack_fw_version("0.4.0"), 42,
                       key_id_override=key_id(tpk))
    got, _ = verify(liar, trusted, expect_board=board)
    checks.append(("a forged key id is refused on the signature",
                   got == V_BAD_SIGNATURE, got))

    # Domain separation: the same 128 bytes signed WITHOUT the tag must not
    # verify. This is the property that stops a content pack signature, or any
    # other signature this key might one day be induced to make, from being
    # replayed as a firmware signature.
    naked = bytearray(blob)
    naked[OFF_SIG:OFF_SIG + OTA_SIG_BYTES] = ed25519_sign(
        blob[:OTA_SIGNED_PREFIX_BYTES], throwaway, tpk)
    got, _ = verify(bytes(naked), trusted, expect_board=board)
    checks.append(("an untagged signature is refused", got == V_BAD_SIGNATURE, got))

    for label, ok, detail in checks:
        print("%-52s %s" % (label, "ok" if ok else "FAIL (%s)" % (detail,)))
        if not ok:
            failures += 1

    print()
    print("domain tag: %r padded to %d bytes"
          % (OTA_DOMAIN_TAG, OTA_DOMAIN_TAG_BYTES))
    print("signed message: tag || header[0..%d), %d bytes total"
          % (OTA_SIGNED_PREFIX_BYTES,
             OTA_DOMAIN_TAG_BYTES + OTA_SIGNED_PREFIX_BYTES))
    print("throwaway key id: %s (generated in memory, never written to disk)"
          % key_id(tpk).hex())

    print("\n%s" % ("ALL PASS" if failures == 0 else "%d FAILURES" % failures))
    return 1 if failures else 0


# ══ Entry point ═══════════════════════════════════════════════════════════


def main():
    ap = argparse.ArgumentParser(
        description="OTA image signing and verification for HexHound.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen-key", action="store_true",
                    help="generate a DEVELOPMENT keypair and the firmware header")
    ap.add_argument("--set-release-pubkey", metavar="HEX",
                    help="install a RELEASE public key (64 hex chars) whose "
                         "private half is held elsewhere and never enters this "
                         "repository; clears the development-key publish gate")
    ap.add_argument("--force", action="store_true",
                    help="overwrite an existing key file")
    ap.add_argument("--self-test", action="store_true",
                    help="run the RFC 8032 vectors and a full signing round trip")

    ap.add_argument("--sign", metavar="FIRMWARE_BIN",
                    help="application image to wrap and sign (a PlatformIO "
                         "firmware.bin, NOT an .elf)")
    # No default. The board id is the field that decides whether an image can
    # brick a device it was never built for, and a default here would be a
    # default for the most dangerous field in the header. Every other argument
    # gets one; this one never will.
    ap.add_argument("--board", metavar="BOARD_ID",
                    help="board id to stamp, e.g. lilygo-t-dongle-s3. Must match "
                         "HEXHOUND_OTA_BOARD_ID in src/ota/ota_identity.h for the "
                         "target. REQUIRED: there is deliberately no default.")
    # Also no default. It could be scraped out of ota_identity.h, but that
    # header describes the tree you are standing in, not necessarily the tree
    # firmware.bin was built from, and a signed version field that quietly
    # disagrees with the binary is worse than one you had to type.
    ap.add_argument("--fw-version", metavar="M.m.p",
                    help="firmware version to stamp, e.g. 0.4.0. REQUIRED.")
    ap.add_argument("--build", type=int, default=0, metavar="N",
                    help="build number to stamp (default: 0, meaning an "
                         "unnumbered local build). Signed, but nothing gates on "
                         "it: there is no downgrade block, by design.")
    ap.add_argument("--key", default=KEY_PATH, metavar="PATH",
                    help="private signing key (default: %(default)s)")
    ap.add_argument("--out", metavar="PATH",
                    help="output .hexfw (default: the input with .hexfw "
                         "substituted for its extension)")

    ap.add_argument("--verify", metavar="HEXFW",
                    help="re-read a signed image and apply the device's gate")
    ap.add_argument("--pubkey", action="append", metavar="HEX",
                    help="verify against this 32-byte public key instead of the "
                         "compiled-in trusted list. Repeatable.")
    ap.add_argument("--expect-board", metavar="BOARD_ID",
                    help="refuse the image unless it names this board, the way a "
                         "device running that board's firmware would")
    ap.add_argument("--slot-bytes", type=int, default=OTA_SLOT_BYTES, metavar="N",
                    help="OTA slot size to bound against (default: %(default)d, "
                         "app0/app1 from default_16MB.csv)")

    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if args.set_release_pubkey:
        return set_release_pubkey(args.set_release_pubkey)
    if args.gen_key:
        return gen_key(args.force)
    if args.verify:
        return cmd_verify(args)
    if args.sign:
        missing = [name for name, value in
                   (("--board", args.board), ("--fw-version", args.fw_version))
                   if not value]
        if missing:
            ap.error("--sign needs %s. Neither has a default: the board id "
                     "decides which hardware may install this image, and the "
                     "version is a signed claim about what it is."
                     % " and ".join(missing))
        if not args.out:
            base = os.path.splitext(args.sign)[0]
            args.out = base + ".hexfw"
        return cmd_sign(args)

    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
