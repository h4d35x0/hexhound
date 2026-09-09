#!/usr/bin/env python3
"""Generate the signed-pack fixtures the native test suite compiles in.

    python scripts/gen_pack_test_fixtures.py

Writes test/test_content_pack/fixtures.h.

Why fixtures and not signing in the test: the firmware contains a VERIFIER and
no signer, which is the correct shape (a device that can sign content can mint
content for every other device). So the packs the test feeds it have to be made
by the tool. Compiling the tool's output into the test is what proves the tool
and the firmware agree at the byte level, and it keeps proving it on every
future test run rather than only on the day someone ran a round trip by hand.

The test keys below are FIXED, not random, so regenerating this file produces
identical bytes and a diff means something actually changed. They are test
keys: both halves are right here and neither is secret. They are NOT in the
firmware's trusted list; the test passes them to verifyImage() explicitly.

One fixture is different: PACK_DEVKEY_DIALOGUE is signed with the repository's
real development key and is verified against the COMPILED-IN trusted list. If
someone rotates that key without regenerating this file, that test fails. That
is the intended behaviour, and the failure message says so.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sign_content_pack as scp  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "test", "test_content_pack", "fixtures.h")

# Fixed test seeds. Not secret, not in the firmware's trusted list.
TEST_SEED = bytes(range(32))
WRONG_SEED = bytes((0xA0 ^ b) for b in range(32))


def c_array(name, data):
    lines = ["static const uint8_t %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 12):
        lines.append("    " + " ".join("0x%02x," % b for b in data[i:i + 12]))
    lines.append("};")
    return "\n".join(lines)


def signed(kind, body, seed, key_id_override=None):
    """Envelope a body, optionally lying about which key signed it."""
    pk = scp.ed25519_public_key(seed)
    header = bytearray(scp.HEADER_BYTES)
    header[0:4] = scp.MAGIC
    header[4] = scp.FORMAT_VERSION
    header[5] = kind
    header[8:12] = key_id_override if key_id_override else scp.key_id(pk)
    header[12:16] = len(body).to_bytes(4, "little")
    sig = scp.ed25519_sign(bytes(header) + body, seed, pk)
    return bytes(header) + sig + body


def main():
    test_pk = scp.ed25519_public_key(TEST_SEED)
    wrong_pk = scp.ed25519_public_key(WRONG_SEED)

    with open(os.path.join(REPO, "content-src", "dialogue.json"), encoding="utf-8") as fh:
        dialogue_doc = json.load(fh)
    with open(os.path.join(REPO, "content-src", "quests.json"), encoding="utf-8") as fh:
        quests_doc = json.load(fh)

    dialogue_body = scp.encode_dialogue(dialogue_doc)
    quests_body = scp.encode_quests(quests_doc)

    parts = []

    def emit(name, data, note=""):
        if note:
            parts.append("// " + note.replace("\n", "\n// "))
        parts.append(c_array(name, data))
        parts.append("")

    emit("FIX_TEST_PUBKEY", test_pk,
         "Public half of the fixed test key. Passed to verifyImage()\n"
         "explicitly; it is deliberately NOT in the firmware's trusted list.")
    emit("FIX_WRONG_PUBKEY", wrong_pk,
         "A second test key, used as the 'some other author' key.")

    emit("FIX_DIALOGUE_OK", signed(scp.KIND_DIALOGUE, dialogue_body, TEST_SEED),
         "content-src/dialogue.json, encoded and signed by the test key.\n"
         "The authored JSON was NOT rewritten for CBOR; this is the same file\n"
         "the plain-JSON loader would have accepted.")
    emit("FIX_QUESTS_OK", signed(scp.KIND_QUESTS, quests_body, TEST_SEED),
         "content-src/quests.json, same story.")

    emit("FIX_DIALOGUE_WRONG_KEY",
         signed(scp.KIND_DIALOGUE, dialogue_body, WRONG_SEED,
                key_id_override=scp.key_id(test_pk)),
         "Signed by the WRONG key while claiming the test key's id. The key id\n"
         "is a selector, so this must fail on the signature, not on the id.")
    emit("FIX_DIALOGUE_UNKNOWN_KEY",
         signed(scp.KIND_DIALOGUE, dialogue_body, WRONG_SEED),
         "Honestly signed by a key the verifier was not given: unknown key.")

    # A validly signed pack whose body is not valid CBOR at all.
    emit("FIX_SIGNED_GARBAGE_BODY",
         signed(scp.KIND_DIALOGUE, bytes([0xFF, 0x1F, 0x7B, 0xC0, 0x00, 0x99]), TEST_SEED),
         "Signature is GOOD, body is garbage. Proves the CBOR decoder is a\n"
         "real gate and not a formality behind the signature check.")

    # Indefinite-length array: legal CBOR, outside the accepted profile.
    emit("FIX_SIGNED_INDEFINITE",
         signed(scp.KIND_DIALOGUE,
                bytes([0xA1]) + scp.cbor_text("lines") + bytes([0x9F, 0xFF]),
                TEST_SEED),
         "An indefinite-length array. Valid RFC 8949, refused by this profile.")

    # Nesting past CBOR_MAX_DEPTH, inside a field the decoder skips.
    deep = scp.cbor_uint(1)
    for _ in range(8):
        deep = scp.cbor_array([deep])
    row = scp.cbor_map([("text", scp.cbor_text("ok")), ("junk", deep)])
    emit("FIX_SIGNED_DEEP_NEST",
         signed(scp.KIND_DIALOGUE,
                scp.cbor_map([("lines", scp.cbor_array([row]))]), TEST_SEED),
         "Eight nested arrays in a field the decoder would otherwise skip.\n"
         "skipValue is the only recursion in the decoder and this is what\n"
         "bounds it.")

    # Non-minimal integer encoding: 5 spelled as a two-byte head.
    emit("FIX_SIGNED_NON_MINIMAL",
         signed(scp.KIND_DIALOGUE,
                bytes([0xA1]) + scp.cbor_text("lines") + bytes([0x81, 0xA1]) +
                scp.cbor_text("stage") + bytes([0x18, 0x05]),
                TEST_SEED),
         "0x18 0x05 is 5 written the long way. Two spellings of one value is a\n"
         "difference someone has to reconcile; the decoder refuses instead.")

    # Well-formed CBOR, correct shape, but no row carries usable text.
    emit("FIX_SIGNED_NO_USABLE_ROWS",
         signed(scp.KIND_DIALOGUE,
                scp.cbor_map([("lines", scp.cbor_array([
                    scp.cbor_map([("context", scp.cbor_text("idle"))]),
                    scp.cbor_map([("text", scp.cbor_text(""))]),
                ]))]),
                TEST_SEED),
         "Decodes perfectly and contains nothing worth saying. Must leave the\n"
         "baseline alone rather than installing an empty table.")

    # Trailing bytes after the top-level value, inside the signed body.
    emit("FIX_SIGNED_TRAILING",
         signed(scp.KIND_DIALOGUE,
                scp.cbor_map([("lines", scp.cbor_array([
                    scp.cbor_map([("text", scp.cbor_text("hello"))])]))]) + b"\x00",
                TEST_SEED),
         "One stray byte after a complete document. Signed, so not an attack\n"
         "by itself, but the pack is not what the signer thought it was.")

    # The signing key whose packs are checked against the compiled-in trusted
    # list. Overridable, because once a RELEASE key is installed the private
    # half deliberately does not live in this repository any more: it sits in a
    # vault, and regenerating this fixture means fetching it to a temporary path
    # and passing it here.
    #     python scripts/gen_pack_test_fixtures.py --key /path/to/seed
    dev_key_path = os.path.join(REPO, "keys", "content-dev-signing.key")
    for i, a in enumerate(sys.argv):
        if a == "--key" and i + 1 < len(sys.argv):
            dev_key_path = sys.argv[i + 1]
        elif a.startswith("--key="):
            dev_key_path = a.split("=", 1)[1]
    if os.path.exists(dev_key_path):
        dev_seed = scp.read_key(dev_key_path)
        dev_pk = scp.ed25519_public_key(dev_seed)
        emit("FIX_DEVKEY_DIALOGUE",
             signed(scp.KIND_DIALOGUE, dialogue_body, dev_seed),
             "Signed with the repository's DEVELOPMENT key, and verified in the\n"
             "test against the COMPILED-IN trusted list rather than an\n"
             "explicitly supplied key. This is the end-to-end proof that\n"
             "scripts/sign_content_pack.py and the firmware agree.\n"
             "\n"
             "If this test fails after a key rotation, regenerate:\n"
             "  python scripts/gen_pack_test_fixtures.py")
        parts.append("#define FIX_HAVE_DEVKEY 1")
        parts.append("// Trusted-list fixture key id: %s" % scp.key_id(dev_pk).hex())
        parts.append("")
    else:
        parts.append("// No keys/content-dev-signing.key present, so the")
        parts.append("// compiled-in-trusted-list fixture was not generated.")
        parts.append("// Run: python scripts/sign_content_pack.py --gen-key")
        parts.append("")

    header = """#pragma once

#include <stdint.h>

// ── HexHound - Signed Content Pack Test Fixtures ─────────────────
//
// GENERATED FILE. Regenerate with:
//     python scripts/gen_pack_test_fixtures.py
//
// Every pack below was produced by scripts/sign_content_pack.py, which is the
// point: the firmware has a verifier and no signer, so the only way to prove
// the tool and the firmware agree is to compile the tool's actual output into
// the test.
//
// The test keys here are fixed and public. Nothing in this file is secret.

"""
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(header + "\n".join(parts).rstrip() + "\n")

    print("wrote %s" % OUT)
    print("  test key id : %s" % scp.key_id(test_pk).hex())
    print("  dialogue    : %d bytes body" % len(dialogue_body))
    print("  quests      : %d bytes body" % len(quests_body))


if __name__ == "__main__":
    sys.exit(main() or 0)
