#!/usr/bin/env python3
"""Push a signed .hexfw to a HexHound over USB serial.

    python scripts/push_ota.py dist/hexhound-147b-0.5.0.hexfw --port COM14

── The one thing to know before using this ─────────────────────────────────

**You have to arm the device yourself, on the device.** Menu -> UPDATE ->
START UPDATE, with the button in your hand. This script cannot do it for you
and there is no flag that makes it. That is not an oversight to be worked
around; it is the entire implementation of "no automatic updates and no silent
ones". The protocol has no ARM verb, so no host, no script and nothing that
compromises a host can start an update on somebody's device. See the frame type
list in src/ota/ota_serial.h.

This script will wait for you to do it, and tell you what it is waiting for.

── Why it verifies before it sends a byte ──────────────────────────────────

Because the device is going to erase its passive flash slot the moment it
accepts the header, and on a device that has already been updated once that
slot holds the only firmware there is to roll back to. Spending three seconds
checking the file on the machine that has the file is much better than spending
ninety seconds pushing one that was never going to install.

The check is `sign_ota_image.py`'s own `verify()`, imported. Not reimplemented,
and deliberately not "a quick sanity check": it is the same gate in the same
order, so if this passes and the device still refuses, that disagreement is
itself the bug and is worth reporting rather than working around.

── Reading the wire ────────────────────────────────────────────────────────

Device-to-host framing has a non-ASCII magic (0x5A "HXH"), so response frames
can be picked out of the firmware's ordinary log lines without any escaping in
either direction. Everything that is not a frame is a log line, and this script
shows those too when asked, because during a failed update they are usually the
most useful thing on the screen.
"""

import argparse
import binascii
import os
import struct
import sys
import time

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(PROJECT_DIR, "scripts"))

# Reuse, never reimplement. This is the same verify() the build machine runs and
# the same one whose order mirrors ota_image.cpp's accept().
try:
    from sign_ota_image import (  # noqa: E402
        OTA_HEADER_BYTES,
        V_ACCEPTED,
        parse,
        read_trusted_keys,
        unpack_fw_version,
        verify,
    )
except ImportError as exc:  # pragma: no cover
    sys.exit("cannot import scripts/sign_ota_image.py: %s" % exc)

try:
    import serial  # pyserial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required: python -m pip install pyserial")


# ══ The wire, from src/ota/ota_serial.h ═══════════════════════════════════
#
# These constants mirror that header one for one. A diff of the two is a thing
# a person can actually do, which is the same reason sign_ota_image.py carries
# the header offset table.

def _safe_print(text):
    """print() that cannot kill an update in progress.

    The device's log is whatever the firmware chose to emit, and a Windows
    console is usually cp1252, which cannot encode most of it. A single arrow
    in an unrelated boot message is enough to raise UnicodeEncodeError out of
    print(), and because the log is drained from inside the frame reader, that
    exception unwinds the PUSH: an update dies on a diagnostic that was only
    ever meant to be helpful.

    It cost exactly that once, on `If dark -> hardware or power issue`, with
    the device armed and waiting. Diagnostics do not get to fail the operation
    they are describing.
    """
    enc = getattr(sys.stdout, "encoding", None) or "ascii"
    try:
        print(text)
    except UnicodeEncodeError:
        print(text.encode(enc, "replace").decode(enc, "replace"))


OTA_WIRE_VERSION = 1

MAGIC_H2D = b"\xa5HXD"
MAGIC_D2H = b"\x5aHXH"
MAGIC_BYTES = 4
HEAD_BYTES = 8
CRC_BYTES = 4

MAX_PAYLOAD = 4096  # HEXHOUND_OTA_CHUNK_MAX

# Host to device. Note what is not here: there is no ARM.
F_HELLO = 0x01
F_BEGIN = 0x02
F_DATA = 0x03
F_COMMIT = 0x04
F_ABORT = 0x05

# Device to host.
F_IDENT = 0x81
F_PROGRESS = 0x82
F_RESULT = 0x83

IDENT_BYTES = 48
IDENT_FLAG_ON_TRIAL = 0x01
IDENT_FLAG_AFTER_ROLLBACK = 0x02
IDENT_FLAG_HAS_TARGET = 0x04

# OtaSession::State, in enum order.
STATE_NAMES = ["idle", "armed", "receiving", "verifying", "ready", "failed"]
S_IDLE, S_ARMED, S_RECEIVING, S_VERIFYING, S_READY, S_FAILED = range(6)


def crc32(data):
    """The CRC the device computes: standard reflected CRC-32, same as zlib.

    The device uses a nibble table rather than this, but bit-for-bit the same
    polynomial and the same init/final xor, which is exactly why the host side
    is one library call and not a second implementation that could drift.
    """
    return binascii.crc32(data) & 0xFFFFFFFF


def build_frame(ftype, payload=b""):
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload %d exceeds the device's %d byte limit"
                         % (len(payload), MAX_PAYLOAD))
    head = MAGIC_H2D + struct.pack("<BBH", ftype, 0, len(payload))
    # The CRC covers from the type byte, not from the magic: the magic is
    # matched exactly by the device's reader before anything else is read, so
    # what the CRC protects is the fields that reader is about to trust.
    body = head[MAGIC_BYTES:] + payload
    return head + payload + struct.pack("<I", crc32(body))


class DeviceError(Exception):
    """Anything that means the transfer cannot honestly continue."""


class Link(object):
    """A framed reader over a serial port that is also carrying log text.

    Keeps a rolling buffer and hunts for MAGIC_D2H. Everything before a magic
    is log output; it is surfaced rather than swallowed, because when an update
    fails the firmware's own log lines are usually more informative than the
    verdict.
    """

    def __init__(self, port, baud, show_log):
        self.show_log = show_log
        try:
            self.ser = serial.Serial(port, baud, timeout=0.05)
        except serial.SerialException as exc:
            raise DeviceError("cannot open %s: %s" % (port, exc))
        self.buf = bytearray()
        self.log = bytearray()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def send(self, ftype, payload=b""):
        self.ser.write(build_frame(ftype, payload))
        self.ser.flush()

    def _emit_log(self, chunk):
        self.log.extend(chunk)
        while b"\n" in self.log:
            line, _, rest = self.log.partition(b"\n")
            self.log = bytearray(rest)
            text = line.decode("utf-8", "replace").rstrip("\r")
            if text and self.show_log:
                _safe_print("    device | %s" % text)

    def read_frame(self, timeout):
        """Next device frame as (type, payload), or None on timeout."""
        deadline = time.time() + timeout
        while True:
            idx = self.buf.find(MAGIC_D2H)
            if idx >= 0:
                if idx:
                    self._emit_log(self.buf[:idx])
                    del self.buf[:idx]
                if len(self.buf) >= HEAD_BYTES:
                    ftype, flags, length = struct.unpack_from(
                        "<BBH", self.buf, MAGIC_BYTES)
                    total = HEAD_BYTES + length + CRC_BYTES
                    if length > MAX_PAYLOAD:
                        # Not a frame we can believe. Step over this magic and
                        # keep hunting rather than waiting for bytes that are
                        # never going to make sense.
                        del self.buf[:MAGIC_BYTES]
                        continue
                    if len(self.buf) >= total:
                        frame = bytes(self.buf[:total])
                        del self.buf[:total]
                        payload = frame[HEAD_BYTES:HEAD_BYTES + length]
                        want = struct.unpack_from("<I", frame, HEAD_BYTES + length)[0]
                        got = crc32(frame[MAGIC_BYTES:HEAD_BYTES + length])
                        if want != got:
                            raise DeviceError(
                                "a reply from the device failed its CRC "
                                "(wanted %08x, computed %08x). The serial link "
                                "is corrupting data; nothing was installed."
                                % (want, got))
                        if flags:
                            raise DeviceError(
                                "the device set reserved frame flags 0x%02x, "
                                "which this script does not understand" % flags)
                        return ftype, payload
            elif len(self.buf) > MAGIC_BYTES:
                # No magic anywhere; everything but a possible partial magic at
                # the tail is log text.
                keep = MAGIC_BYTES - 1
                self._emit_log(self.buf[:-keep])
                del self.buf[:-keep]

            if time.time() >= deadline:
                return None
            chunk = self.ser.read(4096)
            if chunk:
                self.buf.extend(chunk)
            else:
                time.sleep(0.005)

    def expect(self, timeout, *types):
        """Next frame of one of `types`, skipping IDENT state announcements.

        IDENT arrives unsolicited whenever the session state changes, so it can
        legitimately turn up in the middle of a transfer. It is informational
        and is not the reply to anything.
        """
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            got = self.read_frame(remaining)
            if got is None:
                return None
            if got[0] in types:
                return got
            if got[0] == F_IDENT:
                continue
            raise DeviceError("unexpected frame type 0x%02x from the device"
                              % got[0])


def parse_ident(payload):
    if len(payload) < IDENT_BYTES:
        raise DeviceError("the device sent a short IDENT (%d bytes, expected %d)"
                          % (len(payload), IDENT_BYTES))
    wire, state, verdict, flags = struct.unpack_from("<BBBB", payload, 0)
    fw, slot = struct.unpack_from("<II", payload, 4)
    chunk_max, _ = struct.unpack_from("<HH", payload, 12)
    board = payload[16:48].split(b"\x00", 1)[0].decode("ascii", "replace")
    return {
        "wire": wire, "state": state, "verdict": verdict, "flags": flags,
        "fw": fw, "slot": slot, "chunk_max": chunk_max, "board": board,
    }


def parse_progress(payload):
    state, verdict = struct.unpack_from("<BB", payload, 0)
    written, total = struct.unpack_from("<II", payload, 2)
    return state, verdict, written, total


def parse_result(payload):
    state, verdict = struct.unpack_from("<BB", payload, 0)
    written, total = struct.unpack_from("<II", payload, 2)
    parts = payload[10:].split(b"\x00")
    text = [p.decode("utf-8", "replace") for p in parts]
    while len(text) < 3:
        text.append("")
    return {
        "state": state, "verdict": verdict, "written": written, "total": total,
        "name": text[0], "help": text[1], "reason": text[2],
    }


def state_name(value):
    return STATE_NAMES[value] if 0 <= value < len(STATE_NAMES) else "?%d" % value


def show_refusal(result, where):
    """Everything known about a refusal, at once. Never just 'failed'."""
    print()
    print("REFUSED during %s." % where)
    print("  device state : %s" % state_name(result["state"]))
    print("  verdict      : %s (%d)" % (result["name"], result["verdict"]))
    if result["reason"]:
        print("  cause        : %s" % result["reason"])
    print("  progress     : %d of %d bytes" % (result["written"], result["total"]))
    if result["help"]:
        # The device's own words, which are written for the person holding it.
        print()
        print("  %s" % result["help"])
    print()
    print("Nothing was installed. The device still boots the firmware it was")
    print("booting before this attempt.")


# ══ Progress display ══════════════════════════════════════════════════════


def render_bar(written, total, started):
    width = 34
    frac = (written / total) if total else 0.0
    filled = int(frac * width)
    elapsed = max(time.time() - started, 1e-6)
    rate = written / elapsed / 1024.0
    sys.stdout.write("\r  [%s%s] %5.1f%%  %d/%d B  %.0f KiB/s"
                     % ("#" * filled, "." * (width - filled), frac * 100.0,
                        written, total, rate))
    sys.stdout.flush()


# ══ The push ══════════════════════════════════════════════════════════════


def autodetect_port():
    """Ports that look like an ESP32-S3 CDC device, best guess first."""
    hits = []
    for p in list_ports.comports():
        vid = getattr(p, "vid", None)
        if vid == 0x303A:  # Espressif
            hits.append((0, p.device, p.description))
        elif vid in (0x1A86, 0x10C4, 0x0403):  # CH34x, CP210x, FTDI bridges
            hits.append((1, p.device, p.description))
    hits.sort()
    return hits


def do_push(args, blob, info):
    link = Link(args.port, args.baud, args.show_log)
    try:
        # ── Who is on the other end ───────────────────────────────────────
        print("Asking the device what it is...")
        ident = None
        for _ in range(args.hello_tries):
            link.send(F_HELLO)
            got = link.expect(1.5, F_IDENT)
            if got:
                ident = parse_ident(got[1])
                break
        if ident is None:
            raise DeviceError(
                "no answer from %s.\n"
                "  - Is this the right port? (--list shows candidates)\n"
                "  - Is the firmware on it built from this branch? Firmware\n"
                "    without the OTA receiver cannot answer, and every device\n"
                "    in the field has to be updated over USB once before OTA\n"
                "    can work at all." % args.port)

        if ident["wire"] != OTA_WIRE_VERSION:
            raise DeviceError(
                "the device speaks wire version %d and this script speaks %d.\n"
                "Use the push_ota.py from the same tree as its firmware."
                % (ident["wire"], OTA_WIRE_VERSION))

        print("  board      : %s" % ident["board"])
        print("  firmware   : %s" % unpack_fw_version(ident["fw"]))
        print("  ota slot   : %d bytes" % ident["slot"])
        print("  state      : %s" % state_name(ident["state"]))
        if ident["flags"] & IDENT_FLAG_AFTER_ROLLBACK:
            print("  note       : this device booted after a REJECTED update")
        if not ident["flags"] & IDENT_FLAG_HAS_TARGET:
            raise DeviceError("this device reports no usable OTA slot, so it "
                              "cannot install anything.")

        # ── Board and size, checked against the device, not against a guess ─
        if info.board_id != ident["board"]:
            raise DeviceError(
                "this image is for %r and the device is %r.\n"
                "The device would refuse it as board_mismatch, and it is right\n"
                "to: every ESP32-S3 board here reports the same chip family,\n"
                "so a wrong image flashes cleanly and leaves a dark screen."
                % (info.board_id, ident["board"]))
        if ident["slot"] and info.image_len > ident["slot"]:
            raise DeviceError("image is %d bytes and the device's slot is %d"
                              % (info.image_len, ident["slot"]))

        chunk = min(args.chunk, ident["chunk_max"], MAX_PAYLOAD)

        if ident["flags"] & IDENT_FLAG_ON_TRIAL:
            raise DeviceError(
                "the device's RUNNING image is still on trial and has not\n"
                "confirmed itself yet. It refuses updates during that window,\n"
                "because the slot an update would erase is currently holding\n"
                "the only firmware it can roll back to. Wait a few seconds.")

        # ── Wait for a person ─────────────────────────────────────────────
        state = ident["state"]
        if state == S_READY:
            raise DeviceError(
                "the device already has an update staged and is waiting to be\n"
                "restarted. Restart it first; it will not accept another image\n"
                "until it has.")
        if state != S_ARMED:
            print()
            print("Waiting for you to arm the device.")
            print("  On the HexHound: MENU -> UPDATE -> START UPDATE")
            print()
            print("  This script cannot do that step. The protocol has no arm")
            print("  command at all, so no host can start an update on someone's")
            print("  device. That is the point, not a limitation.")
            print()
            deadline = time.time() + args.wait_arm
            last_poll = 0.0
            while state != S_ARMED:
                if time.time() > deadline:
                    raise DeviceError(
                        "the device was not armed within %d seconds."
                        % args.wait_arm)
                # The device announces the change unsolicited, so this normally
                # returns the moment the button is held. The poll is a fallback
                # for a frame lost to a reconnect.
                got = link.read_frame(1.0)
                if got and got[0] == F_IDENT:
                    state = parse_ident(got[1])["state"]
                elif time.time() - last_poll > 2.0:
                    last_poll = time.time()
                    link.send(F_HELLO)
            print("Armed. Sending.")

        # ── Header first, and nothing is written until it verifies ────────
        print()
        print("Offering the signed header (%d bytes)..." % OTA_HEADER_BYTES)
        link.send(F_BEGIN, blob[:OTA_HEADER_BYTES])
        got = link.expect(args.timeout, F_PROGRESS, F_RESULT)
        if got is None:
            raise DeviceError("the device did not answer the header within "
                              "%.0f seconds" % args.timeout)
        if got[0] == F_RESULT:
            show_refusal(parse_result(got[1]), "the signature check")
            return 1
        print("Header accepted. The signature verified BEFORE anything was")
        print("written, so the slot is only being erased now.")

        # ── Stream ────────────────────────────────────────────────────────
        body = blob[OTA_HEADER_BYTES:]
        total = len(body)
        sent = 0
        started = time.time()
        print()
        while sent < total:
            piece = body[sent:sent + chunk]
            link.send(F_DATA, piece)
            got = link.expect(args.timeout, F_PROGRESS, F_RESULT)
            if got is None:
                raise DeviceError(
                    "\nthe device stopped answering after %d of %d bytes.\n"
                    "It aborts a stalled transfer by itself and leaves the\n"
                    "device booting what it was already booting."
                    % (sent, total))
            if got[0] == F_RESULT:
                print()
                show_refusal(parse_result(got[1]), "the transfer")
                return 1
            _, _, written, _ = parse_progress(got[1])
            sent += len(piece)
            if written != sent:
                raise DeviceError(
                    "\nthe device has %d bytes and this script sent %d. The\n"
                    "two disagree about what arrived, so the transfer is\n"
                    "abandoned rather than finished on a guess."
                    % (written, sent))
            render_bar(sent, total, started)
        render_bar(sent, total, started)
        print()

        # ── Commit ────────────────────────────────────────────────────────
        print()
        print("Asking the device to verify what it wrote. It reads the image")
        print("back off flash and hashes it, so this takes a few seconds.")
        link.send(F_COMMIT)
        got = link.expect(args.commit_timeout, F_RESULT)
        if got is None:
            raise DeviceError("the device did not report a result within %.0f "
                              "seconds" % args.commit_timeout)
        result = parse_result(got[1])
        if result["state"] != S_READY:
            show_refusal(result, "the final check")
            return 1

        print()
        print("ACCEPTED. The image is verified on flash and staged.")
        print()
        print("  The device is STILL RUNNING THE OLD FIRMWARE. Nothing has")
        print("  rebooted and nothing will reboot on its own. On the device:")
        print("  RESTART NOW, or leave it and it happens at the next power-up.")
        print()
        print("  After it restarts, the new image has to reach every health")
        print("  milestone or the bootloader puts the old one back. A failed")
        print("  update looks like nothing happened, so the update screen says")
        print("  ROLLED BACK when that is what occurred.")
        return 0

    finally:
        link.close()


def main():
    ap = argparse.ArgumentParser(
        description="Push a signed .hexfw to a HexHound over USB serial.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="The device must be armed BY HAND: MENU -> UPDATE -> START "
               "UPDATE. No flag here can do it.")
    ap.add_argument("image", nargs="?", metavar="HEXFW",
                    help="the signed .hexfw to send")
    ap.add_argument("--port", "-p", metavar="PORT",
                    help="serial port, e.g. COM14 or /dev/ttyACM0")
    ap.add_argument("--list", action="store_true",
                    help="list likely serial ports and exit")
    ap.add_argument("--baud", type=int, default=115200,
                    help="nominal baud (default: %(default)s). Native USB CDC "
                         "ignores this and runs at USB speed.")
    ap.add_argument("--chunk", type=int, default=MAX_PAYLOAD, metavar="N",
                    help="bytes per DATA frame (default: %(default)s, the "
                         "device's maximum)")
    ap.add_argument("--timeout", type=float, default=10.0, metavar="SEC",
                    help="per-frame reply timeout (default: %(default)s)")
    ap.add_argument("--commit-timeout", type=float, default=120.0, metavar="SEC",
                    help="timeout for the read-back digest check, which is "
                         "slow by design (default: %(default)s)")
    ap.add_argument("--wait-arm", type=int, default=180, metavar="SEC",
                    help="how long to wait for you to arm the device "
                         "(default: %(default)s)")
    ap.add_argument("--hello-tries", type=int, default=4, metavar="N",
                    help="how many times to ask the device to identify itself")
    ap.add_argument("--show-log", action="store_true",
                    help="print the firmware's own log lines as they arrive")
    ap.add_argument("--pubkey", action="append", metavar="HEX",
                    help="verify against this public key instead of the "
                         "compiled-in trusted list in src/ota/ota_pubkey.h")
    ap.add_argument("--skip-verify", action="store_true",
                    help=argparse.SUPPRESS)  # see the refusal below

    args = ap.parse_args()

    if args.list:
        hits = autodetect_port()
        if not hits:
            print("no likely HexHound ports found.")
            return 1
        for _, dev, desc in hits:
            print("%-12s %s" % (dev, desc))
        return 0

    if not args.image:
        ap.error("an image is required (or use --list)")

    if args.skip_verify:
        # Deliberately not implemented, and deliberately not silently ignored.
        sys.exit(
            "--skip-verify does not exist.\n"
            "Accepting the header is what makes the device erase its passive\n"
            "slot, and on a device that has been updated once that slot is the\n"
            "only firmware it can fall back to. Checking the file first costs\n"
            "seconds; skipping the check spends somebody's safety net on an\n"
            "image nobody looked at.")

    # ── Verify locally, before a single byte goes out ─────────────────────
    try:
        with open(args.image, "rb") as fh:
            blob = fh.read()
    except OSError as exc:
        sys.exit("cannot read %s: %s" % (args.image, exc))

    if args.pubkey:
        trusted = {}
        for text in args.pubkey:
            try:
                pk = bytes.fromhex(text.strip())
            except ValueError:
                sys.exit("--pubkey must be 64 hex characters, got %r" % text)
            if len(pk) != 32:
                sys.exit("--pubkey must be 32 bytes, got %d" % len(pk))
            from sign_ota_image import key_id
            trusted[key_id(pk)] = pk
        source = "--pubkey"
    else:
        try:
            trusted = read_trusted_keys()
        except (OSError, ValueError) as exc:
            sys.exit("cannot read the trusted key list: %s" % exc)
        source = "src/ota/ota_pubkey.h"

    print("Checking %s before sending anything." % args.image)
    verdict, info = verify(blob, trusted)
    if verdict != V_ACCEPTED:
        _, partial = parse(blob)
        print()
        print("REFUSED ON THIS MACHINE: %s" % verdict)
        print("  file    : %d bytes" % len(blob))
        print("  trusting: %s" % source)
        if partial.board_id:
            print("  board   : %r" % partial.board_id)
        print()
        print("The device would refuse this too. Nothing was sent, and no")
        print("device's flash was touched. Fix the image, do not push it.")
        return 1

    print("  ok: %s, fw %s, build %d, %d bytes of image"
          % (info.board_id, unpack_fw_version(info.fw_version),
             info.build_number, info.image_len))

    # ── Port ──────────────────────────────────────────────────────────────
    if not args.port:
        hits = autodetect_port()
        if len(hits) == 1:
            args.port = hits[0][1]
            print("  using %s (%s)" % (hits[0][1], hits[0][2]))
        elif not hits:
            sys.exit("no --port given and no likely device found. Use --list.")
        else:
            print()
            print("several candidate ports; pass one with --port:")
            for _, dev, desc in hits:
                print("  %-12s %s" % (dev, desc))
            return 1

    print()
    try:
        return do_push(args, blob, info)
    except DeviceError as exc:
        print()
        print("FAILED: %s" % exc)
        return 1
    except KeyboardInterrupt:
        print()
        print("Interrupted. The device aborts a stalled transfer by itself and")
        print("keeps booting the firmware it was already booting.")
        return 130


if __name__ == "__main__":
    sys.exit(main())
