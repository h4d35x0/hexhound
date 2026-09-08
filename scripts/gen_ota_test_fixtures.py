#!/usr/bin/env python3
"""Generate the signed-firmware fixtures the native OTA test suite compiles in.

    python scripts/gen_ota_test_fixtures.py

Writes test/test_ota/fixtures.h.

Why fixtures and not signing in the test: the firmware contains a VERIFIER and
no signer, which is the correct shape (a device that can sign firmware can mint
firmware for every other device). So the images the test feeds it have to be
made by the tool. Every header below comes out of scripts/sign_ota_image.py's
own build_header(), which is what proves the tool and the firmware agree at the
byte level, and it keeps proving it on every future test run rather than only
on the day someone ran a round trip by hand.

── The test key is its own key, and that is the whole point ────────────────

The keypair here is minted from a FIXED, PUBLIC seed and has nothing to do
with keys/ota-dev-signing.key or with src/ota/ota_pubkey.h. Regenerating the
real OTA signing key must never break these tests, because it already did that
once to the content pack suite: a key was rotated, the fixtures were signed by
the key that no longer existed, and the push went red for a reason that had
nothing to do with the change being made. The fix is not to remember to
regenerate; it is to make the tests independent of a key anybody would ever
rotate.

The cost of that independence is real and worth naming: nothing in this file
proves the signer agrees with the REAL trusted list in ota_pubkey.h. That
proof lives in `python scripts/sign_ota_image.py --verify`, which parses that
header and checks a signed image against it.

── The seam the test suite needs, and now has ──────────────────────────────

Both OtaImage::checkSignature() / accept() and OtaSession take the trusted key
list as an OtaImage::KeyRing, defaulting to firmwareKeys() so the device path
is unchanged. A test hands the key below straight in:

    static const OtaImage::KeyRing ring = {
        (const uint8_t (*)[4])FIX_OTA_TEST_KEYID,
        (const uint8_t (*)[32])FIX_OTA_TEST_PUBKEY, 1 };
    session.setTrustedKeys(ring);

DO NOT redefine OTA_TRUSTED_KEYS / OTA_TRUSTED_KEY_IDS with the preprocessor to
get at this. That was the only way before the seam existed and it is no longer
necessary; it makes the test lie about the trust anchor and it stops matching
the code the day firmwareKeys() is built differently. FIX_OTA_TEST_PUBKEY and
FIX_OTA_TEST_KEYID are still emitted in the shape of one row of those arrays,
because that is the shape a KeyRing wants.

── Structurally illegal, but correctly signed ──────────────────────────────

Most of the corrupt variants below are signed OVER their own corruption rather
than tampered with afterwards. That is deliberate. A fixture whose magic was
flipped after signing fails for two reasons at once, and a parse() that had
forgotten to check the magic would still be caught by the signature, so the
test would pass while testing nothing. Signing the corruption makes the
structural check the ONLY thing standing between the fixture and acceptance.
"""

import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sign_ota_image as sot  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "test", "test_ota", "fixtures.h")

# Fixed test seeds, derived from labels so they are self-describing in a hex
# dump and obviously not anybody's real key. Not secret, not in the firmware's
# trusted list, and deliberately different from the content pack test seeds so
# a fixture can never be valid in both suites at once.
TEST_SEED = hashlib.sha512(b"hexhound-ota-test-key-v1").digest()[:32]
OTHER_SEED = hashlib.sha512(b"hexhound-ota-other-key-v1").digest()[:32]

# The board the native test env builds as. platformio's [env:native] defines no
# HEXHOUND_BOARD_* symbol, so ota_identity.h falls through to its default. The
# test should assert this equals HEXHOUND_OTA_BOARD_ID rather than trusting the
# comment, and the emitted #define below is there so it can.
BOARD_ID = "lilygo-t-dongle-s3"
OTHER_BOARD_ID = "waveshare-esp32-s3-lcd-147b"

FW_VERSION = sot.pack_fw_version("0.4.0")
BUILD_NUMBER = 4242

# Small on purpose: these arrays are compiled into a test binary. 1536 bytes is
# comfortably over OTA_MIN_IMAGE_BYTES (1024) so the good fixture survives
# checkFit, and small enough that twelve of them are not a problem.
#
# 1536 is also UNDER HEXHOUND_OTA_CHUNK_MAX, which means every fixture at this
# size verifies in a single read-back pass. That is worth keeping, because the
# single-pass case is the boundary the loop has to get right too. It is also
# why BIG_IMAGE_BYTES below exists.
IMAGE_BYTES = 1536

# ── The multi-pass body ───────────────────────────────────────────────────
#
# OtaSession::finish() reads the image back off the target in
# HEXHOUND_OTA_CHUNK_MAX slices and hashes it incrementally, because a 1.2 MB
# image has to be verifiable on a board with 250 KB of RAM and no PSRAM. That
# loop is the single piece of arithmetic standing between this feature and the
# constraint that shaped the whole design, and with only 1536-byte fixtures it
# never went round twice.
#
# 10000 is chosen, not rounded:
#
#   * more than two full chunks, so the loop genuinely iterates
#   * NOT a multiple of the chunk size, so the final short read is exercised.
#     10000 = 2 * 4096 + 1808. The uneven remainder is the part most likely to
#     be wrong, and a round 12288 would have hidden it.
#
# A bug here is most likely a false REJECT rather than a false accept, since a
# partial hash simply will not match the signed digest. That makes it not a
# hole in the security argument and worse in a different way: OTA would look
# broken for everybody, with a "digest mismatch" pointing at entirely the wrong
# thing, which is a miserable thing to debug from a field report.
BIG_IMAGE_BYTES = 10000

# The body is NOT emitted as a literal array. See the note on the emitted
# fixtures.h header for why, and for what keeps that honest.
BIG_IMAGE_LABEL = "hexhound-ota-fixture-image-big"


def chunk_max_from_header():
    """HEXHOUND_OTA_CHUNK_MAX as the firmware actually defines it.

    Read rather than assumed, so that raising the chunk size past
    BIG_IMAGE_BYTES cannot silently turn the multi-pass fixture back into a
    single-pass one and leave the loop untested while every test still passes.
    """
    path = os.path.join(REPO, "src", "ota", "ota_identity.h")
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) >= 3 and parts[0] == "#define" and \
                    parts[1] == "HEXHOUND_OTA_CHUNK_MAX":
                return int(parts[2], 0)
    raise SystemExit("HEXHOUND_OTA_CHUNK_MAX not found in %s" % path)


def keystream(label, count):
    """Deterministic filler that is not a repeating pattern.

    A run of zeros or a short repeating cycle would let a digest check pass by
    accident when an off-by-one reads the wrong window of the image. Chained
    SHA-512 gives bytes that are reproducible from the label and unforgiving of
    that mistake.
    """
    out = bytearray()
    block = label.encode("ascii")
    while len(out) < count:
        block = hashlib.sha512(block).digest()
        out += block
    return bytes(out[:count])


def c_array(name, data):
    lines = ["static const uint8_t %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 12):
        lines.append("    " + " ".join("0x%02x," % b for b in data[i:i + 12]))
    lines.append("};")
    return "\n".join(lines)


def main():
    test_pk = sot.ed25519_public_key(TEST_SEED)
    test_kid = sot.key_id(test_pk)
    other_pk = sot.ed25519_public_key(OTHER_SEED)
    other_kid = sot.key_id(other_pk)

    image = keystream("hexhound-ota-fixture-image", IMAGE_BYTES)
    image_alt = keystream("hexhound-ota-fixture-image-alt", IMAGE_BYTES)
    digest = hashlib.sha512(image).digest()

    # The multi-pass body. Built here, signed here, but emitted as a rule plus
    # a digest rather than as 10000 literal bytes.
    chunk_max = chunk_max_from_header()
    if BIG_IMAGE_BYTES <= 2 * chunk_max:
        raise SystemExit(
            "BIG_IMAGE_BYTES (%d) must exceed two full chunks (%d) or the "
            "read-back loop still does not iterate" % (BIG_IMAGE_BYTES, 2 * chunk_max))
    if BIG_IMAGE_BYTES % chunk_max == 0:
        raise SystemExit(
            "BIG_IMAGE_BYTES (%d) is a multiple of the chunk size (%d), so the "
            "short final read is never exercised" % (BIG_IMAGE_BYTES, chunk_max))

    big_image = keystream(BIG_IMAGE_LABEL, BIG_IMAGE_BYTES)
    big_digest = hashlib.sha512(big_image).digest()

    def header(seed=TEST_SEED, pk=test_pk, image_digest=digest,
               image_len=len(image), board=BOARD_ID, **kwargs):
        return sot.build_header(seed, image_digest, image_len, board,
                                FW_VERSION, BUILD_NUMBER, pk=pk, **kwargs)

    parts = []

    def comment(note):
        # rstrip per line, so a blank separator line inside a note comes out as
        # "//" and not as "// " with trailing whitespace.
        return "\n".join(("// " + line).rstrip() for line in note.split("\n"))

    def emit(name, data, note=""):
        if note:
            parts.append(comment(note))
        parts.append(c_array(name, data))
        parts.append("")

    def define(name, value, note=""):
        if note:
            parts.append(comment(note))
        parts.append("#define %s %s" % (name, value))
        parts.append("")

    # ── Identity ──────────────────────────────────────────────────────────

    emit("FIX_OTA_TEST_PUBKEY", test_pk,
         "Public half of the fixed test key, in the same shape as one row of\n"
         "OTA_TRUSTED_KEYS. Everything below except FIX_OTA_HDR_UNKNOWN_KEY is\n"
         "signed by it. It is deliberately NOT in the firmware's trusted list,\n"
         "so rotating the real OTA key cannot break a single test here.")
    emit("FIX_OTA_TEST_KEYID", test_kid,
         "SHA-512(FIX_OTA_TEST_PUBKEY)[0..4), in the same shape as one row of\n"
         "OTA_TRUSTED_KEY_IDS. A selector, never a credential.")
    emit("FIX_OTA_OTHER_PUBKEY", other_pk,
         "A second test key, playing 'somebody else's signing key'.")
    emit("FIX_OTA_OTHER_KEYID", other_kid)

    define("FIX_OTA_BOARD_ID", '"%s"' % BOARD_ID,
           "The board id stamped into every good fixture. The test should\n"
           "assert this equals HEXHOUND_OTA_BOARD_ID: if a future change to\n"
           "ota_identity.h moves the default, every fit check below would\n"
           "start failing for a reason that has nothing to do with OTA, and\n"
           "the assertion is what says so out loud.")
    define("FIX_OTA_OTHER_BOARD_ID", '"%s"' % OTHER_BOARD_ID)
    define("FIX_OTA_HEADER_BYTES", sot.OTA_HEADER_BYTES)
    define("FIX_OTA_IMAGE_LEN", len(image))
    define("FIX_OTA_FW_VERSION", "0x%08xu" % FW_VERSION,
           "0.4.0 packed as major<<16 | minor<<8 | patch.")
    define("FIX_OTA_BUILD_NUMBER", "%du" % BUILD_NUMBER)
    define("FIX_OTA_SLOT_BYTES", "0x%08xu" % sot.OTA_SLOT_BYTES,
           "app0/app1 from default_16MB.csv, the bound the oversized fixture\n"
           "is built to exceed. Pass this as slotBytes in the fit tests.")

    # ── Bodies ────────────────────────────────────────────────────────────

    emit("FIX_OTA_IMAGE", image,
         "The image body every header below vouches for, except the digest\n"
         "mismatch case. Chained SHA-512 filler: reproducible from a label,\n"
         "and not a repeating pattern, so an off-by-one in the read-back hash\n"
         "cannot land on matching bytes by luck.")
    emit("FIX_OTA_IMAGE_ALT", image_alt,
         "The same length as FIX_OTA_IMAGE and entirely different content.\n"
         "Feed this body under FIX_OTA_HDR_OK to get a digest mismatch with no\n"
         "length discrepancy to notice first, which is the case that actually\n"
         "exercises checkFlashDigest.")

    # ── The multi-pass body, as a rule ────────────────────────────────────

    define("FIX_OTA_BIG_IMAGE_LEN", "%du" % BIG_IMAGE_BYTES,
           "── The body that makes the read-back loop iterate ──────────────\n"
           "\n"
           "%d bytes, which is %d full HEXHOUND_OTA_CHUNK_MAX (%d) reads plus\n"
           "a short final one of %d. Deliberately not a round multiple: the\n"
           "uneven remainder is the part of finish()'s loop most likely to be\n"
           "wrong, and a body that divided evenly would never show it.\n"
           "\n"
           "Every other body here is 1536 bytes and verifies in ONE pass, so\n"
           "this is the only fixture that exercises the arithmetic that lets a\n"
           "1.2 MB image be checked on a board with 250 KB of RAM."
           % (BIG_IMAGE_BYTES, BIG_IMAGE_BYTES // chunk_max, chunk_max,
              BIG_IMAGE_BYTES % chunk_max))

    define("FIX_OTA_BIG_IMAGE_LABEL", '"%s"' % BIG_IMAGE_LABEL,
           "The label the body is derived from. Same chained-SHA-512 rule as\n"
           "the literal bodies above:\n"
           "\n"
           "    block = SHA512(label); emit it; block = SHA512(block); repeat\n"
           "\n"
           "and truncate to FIX_OTA_BIG_IMAGE_LEN.\n"
           "\n"
           "── Why this one is a rule and not 10000 literal bytes ──────────\n"
           "\n"
           "Because its value is its LENGTH and its position-dependence, not\n"
           "its literalness, and 10000 bytes of hex would add roughly 830\n"
           "lines to this file that no reader learns anything from.\n"
           "\n"
           "What keeps that honest: FIX_OTA_BIG_IMAGE_SHA512 below is the\n"
           "digest of the bytes the GENERATOR built, and it is also inside the\n"
           "signed FIX_OTA_HDR_BIG_OK. So the test's own filler has to agree\n"
           "with this generator byte for byte or nothing passes, and the\n"
           "agreement is checked directly rather than being assumed. A broken\n"
           "read-back loop cannot be papered over by adjusting the filler,\n"
           "because the filler's output has to hash to a digest that sits\n"
           "inside an Ed25519 signature.\n"
           "\n"
           "Assert against FIX_OTA_BIG_IMAGE_SHA512 FIRST in any test that\n"
           "uses this body. If the two fillers ever drift, that assertion says\n"
           "so plainly instead of surfacing as a baffling DIGEST_MISMATCH\n"
           "several layers down.")

    emit("FIX_OTA_BIG_IMAGE_SHA512", big_digest,
         "SHA-512 of the %d bytes the rule above produces. This is the\n"
         "cross-check between the generator's filler and the test's."
         % BIG_IMAGE_BYTES)

    emit("FIX_OTA_HDR_BIG_OK",
         header(image_digest=big_digest, image_len=BIG_IMAGE_BYTES),
         "A valid signed header for the multi-pass body. Drive a whole\n"
         "session with it: the header is ACCEPTED, the transfer streams, and\n"
         "finish() has to read %d bytes back in %d-byte slices and hash them\n"
         "in the right order to reach READY.\n"
         "\n"
         "Then corrupt one byte in the first chunk, one in a middle chunk and\n"
         "one in the short final chunk. A loop that skips a chunk, repeats\n"
         "one, or drops the remainder passes the uncorrupted case and fails\n"
         "at least one of those."
         % (BIG_IMAGE_BYTES, chunk_max))

    # ── The good one ──────────────────────────────────────────────────────

    emit("FIX_OTA_HDR_OK", header(),
         "A valid signed header for FIX_OTA_IMAGE. parse, checkFit and\n"
         "checkSignature must all return ACCEPTED, and checkFlashDigest must\n"
         "accept SHA-512(FIX_OTA_IMAGE).")

    # ── Structurally illegal, and signed over the illegality ──────────────

    emit("FIX_OTA_HDR_BAD_MAGIC", header(magic=b"HEXHOTA2"),
         "Magic is wrong and the signature over it is GOOD. parse() must\n"
         "return BAD_MAGIC on its own merits; if it forgot to check, the\n"
         "signature would not save it here.")
    emit("FIX_OTA_HDR_BAD_VERSION", header(header_version=2),
         "headerVersion 2, correctly signed. A NEWER envelope must be refused\n"
         "outright rather than parsed with a v1 understanding of its fields.")
    emit("FIX_OTA_HDR_BAD_FLAGS", header(flags=0x0001),
         "One unknown flag bit, correctly signed. An unknown flag may change\n"
         "what a field means, so BAD_FLAGS, not a shrug.")
    emit("FIX_OTA_HDR_BAD_RESERVED", header(reserved=b"\x00\x00\x01\x00"),
         "A non-zero reserved byte, correctly signed. This is the check that\n"
         "keeps the four bytes at 124 genuinely free for a future field.")

    # ── Legal shape, wrong content ────────────────────────────────────────

    emit("FIX_OTA_HDR_WRONG_BOARD", header(board=OTHER_BOARD_ID),
         "A perfectly valid, correctly signed image for a DIFFERENT board.\n"
         "This is the fixture that stands in for the real failure mode: five\n"
         "of seven targets are ESP32-S3, so a chip-family check passes this\n"
         "and the panel pins do not. Must be BOARD_MISMATCH.")
    emit("FIX_OTA_HDR_TOO_LARGE",
         header(image_len=sot.OTA_SLOT_BYTES + 1),
         "imageLen one byte past the slot, correctly signed. There is no\n"
         "matching body and there does not need to be: checkFit fires before\n"
         "the target's begin() is ever called, so nothing is erased and\n"
         "nothing is written. Pair it with FIX_OTA_IMAGE if the test needs\n"
         "bytes at all.")
    emit("FIX_OTA_HDR_TOO_SMALL",
         header(image_digest=hashlib.sha512(image[:512]).digest(), image_len=512),
         "An honest, correctly signed 512-byte image: the digest really is\n"
         "SHA-512 of the first 512 bytes of FIX_OTA_IMAGE. Nothing is forged.\n"
         "It is refused purely for being too small to be firmware.")

    # ── Key and signature failures ────────────────────────────────────────

    emit("FIX_OTA_HDR_UNKNOWN_KEY", header(seed=OTHER_SEED, pk=other_pk),
         "Honestly signed by FIX_OTA_OTHER_PUBKEY and honestly labelled with\n"
         "its key id. The verifier was never given that key, so UNKNOWN_KEY,\n"
         "and it must reach that verdict without ever running a verify.")
    emit("FIX_OTA_HDR_FORGED_KEYID",
         header(seed=OTHER_SEED, pk=other_pk, key_id_override=test_kid),
         "Signed by the OTHER key while claiming the test key's id. The id is\n"
         "a selector, so this must die on BAD_SIGNATURE, not on the id. If it\n"
         "returns UNKNOWN_KEY the selector is being treated as a credential.")

    forged = bytearray(header())
    # A signature that is genuinely valid, for a genuinely different message:
    # the same header with a different build number. Cut and pasted onto this
    # header, which is exactly what an attacker with one legitimate signed
    # image and a text editor would try.
    other_message = sot.build_header(TEST_SEED, digest, len(image), BOARD_ID,
                                     FW_VERSION, BUILD_NUMBER + 1, pk=test_pk)
    forged[sot.OFF_SIG:sot.OFF_SIG + sot.OTA_SIG_BYTES] = \
        other_message[sot.OFF_SIG:sot.OFF_SIG + sot.OTA_SIG_BYTES]
    emit("FIX_OTA_HDR_TRANSPLANTED_SIG", bytes(forged),
         "READ THE DIRECTION CAREFULLY. The 128 signed bytes here are the good\n"
         "header, build %d, byte for byte identical to FIX_OTA_HDR_OK. Only the\n"
         "SIGNATURE was replaced, with a real one by the same test key over a\n"
         "different header, build %d. So the header is not the odd one out; the\n"
         "signature is. Every byte of both is legitimate and only the pairing is\n"
         "not, which is exactly what an attacker holding one legitimate signed\n"
         "image and a hex editor would try. Must be BAD_SIGNATURE.\n"
         "\n"
         "buildNumber in THIS fixture reads %d, not %d. An earlier wording of\n"
         "this comment implied the reverse and cost a test author a failing\n"
         "assertion; assert against the bytes."
         % (BUILD_NUMBER, BUILD_NUMBER + 1, BUILD_NUMBER, BUILD_NUMBER + 1))

    untagged = bytearray(header())
    untagged[sot.OFF_SIG:sot.OFF_SIG + sot.OTA_SIG_BYTES] = sot.ed25519_sign(
        bytes(untagged[:sot.OTA_SIGNED_PREFIX_BYTES]), TEST_SEED, test_pk)
    emit("FIX_OTA_HDR_UNTAGGED_SIG", bytes(untagged),
         "The right key signing the right 128 bytes, WITHOUT the domain tag.\n"
         "This is what a signature from some other signing path over the same\n"
         "structure would look like. If it verifies, the tag is not actually\n"
         "in the message and cross-protocol replay is possible.")

    # ── Digest ────────────────────────────────────────────────────────────

    emit("FIX_OTA_HDR_DIGEST_MISMATCH",
         header(image_digest=hashlib.sha512(image_alt).digest()),
         "Signed, in-bounds, right board, right length, and it vouches for\n"
         "FIX_OTA_IMAGE_ALT. Offer it with FIX_OTA_IMAGE as the body: the\n"
         "header must be ACCEPTED and checkFlashDigest must then return\n"
         "DIGEST_MISMATCH. This is the fixture that proves the digest is\n"
         "checked against what landed rather than assumed from the header.")

    header_text = """#pragma once

#include <stdint.h>

// ── HexHound - Signed Firmware Test Fixtures ─────────────────────
//
// GENERATED FILE. Regenerate with:
//     python scripts/gen_ota_test_fixtures.py
//
// Every header below was produced by scripts/sign_ota_image.py, which is the
// point: the firmware has a verifier and no signer, so the only way to prove
// the tool and the firmware agree is to compile the tool's actual output into
// the test.
//
// ── The key here is NOT the repository's OTA key ──────────────────────────
//
// It is minted from a fixed, public seed that lives in the generator. Rotating
// keys/ota-dev-signing.key cannot break any test in this file, which is the
// entire reason it is done this way; the content pack suite learned that the
// expensive way. Nothing in this file is secret.
//
// OtaImage::checkSignature(), OtaImage::accept() and OtaSession all take the
// trusted list as an OtaImage::KeyRing, defaulting to firmwareKeys(). A test
// hands the key below in directly:
//
//     static const OtaImage::KeyRing ring = {
//         (const uint8_t (*)[4])FIX_OTA_TEST_KEYID,
//         (const uint8_t (*)[32])FIX_OTA_TEST_PUBKEY, 1 };
//     session.setTrustedKeys(ring);
//
// Do NOT redefine OTA_TRUSTED_KEYS / OTA_TRUSTED_KEY_IDS with the preprocessor
// to get at this. That was the only way before the seam existed; it makes the
// test lie about the trust anchor and it stops matching the code the day
// firmwareKeys() is built differently.
//
// Layout of every FIX_OTA_HDR_* array, from src/ota/ota_image.h:
//     0 magic | 8 hdrVer | 10 flags | 12 imageLen | 16 boardId(32)
//     48 fwVersion | 52 buildNumber | 56 keyId(4) | 60 digest(64)
//     124 reserved(4) | 128 signature(64)   -> 192 bytes

"""
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(header_text + "\n".join(parts).rstrip() + "\n")

    print("wrote %s" % OUT)
    print("  test key id  : %s" % test_kid.hex())
    print("  other key id : %s" % other_kid.hex())
    print("  board id     : %s" % BOARD_ID)
    print("  image body   : %d bytes each, 2 of them" % len(image))
    print("  big body     : %d bytes, %d x %d + %d, emitted as a rule"
          % (BIG_IMAGE_BYTES, BIG_IMAGE_BYTES // chunk_max, chunk_max,
             BIG_IMAGE_BYTES % chunk_max))

    # The generator checks its own good fixture the same way --verify does,
    # against the public key alone. A fixture generator that emits an image the
    # verifier rejects would otherwise be discovered by a failing C++ test with
    # no obvious cause.
    verdict, _ = sot.verify(bytes(header()) + image, {test_kid: test_pk},
                            expect_board=BOARD_ID)
    if verdict != sot.V_ACCEPTED:
        print("\nINTERNAL ERROR: the good fixture does not verify (%s)" % verdict)
        return 1
    print("  good fixture : verified accepted before writing")

    # And the same for the multi-pass one, whose body is never written out as
    # bytes. If the rule and the signed digest ever disagreed, the C++ test
    # would fail with a digest mismatch pointing at the read-back loop, which
    # is the exact confusion this fixture exists to prevent.
    verdict, _ = sot.verify(
        bytes(header(image_digest=big_digest, image_len=BIG_IMAGE_BYTES)) + big_image,
        {test_kid: test_pk}, expect_board=BOARD_ID)
    if verdict != sot.V_ACCEPTED:
        print("\nINTERNAL ERROR: the big fixture does not verify (%s)" % verdict)
        return 1
    print("  big fixture  : verified accepted before writing")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
