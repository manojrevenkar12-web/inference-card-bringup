#!/usr/bin/env python3
"""Build LACC firmware images and whole-flash images.

An image is a 56-byte header (magic, version, rollback index, payload length, SHA-256 of the
payload, flags, CRC-32 of the header) followed by TLV records: the LNS lookup table, power and
thermal limits, clock table and a build string. The card's boot ROM verifies all of it.

  mkfw.py image --version 3 --rollback 1 -o fw-3.bin
  mkfw.py flash --a fw-3.bin --b fw-2.bin --boot-slot 0 -o flash.img
  mkfw.py image --version 9 --corrupt payload -o bad.bin     (for the validation suite)
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import zlib
from pathlib import Path

FW_MAGIC = 0x3157464C  # "LFW1"
CFG_MAGIC = 0x4746434C  # "LCFG"
FLASH_SIZE = 16 << 20
SLOT_A, SLOT_B, SLOT_SIZE = 0x000000, 0x100000, 0x100000
CONFIG, CONFIG_SIZE = 0x200000, 0x1000
HEADER = struct.Struct("<IIII32sII")  # 56 bytes
TLV_LUT, TLV_POWER, TLV_THERMAL, TLV_CLOCKS, TLV_BUILD = 1, 2, 3, 4, 5


def lns_lut() -> list[int]:
    """2^(i/8) in Q16 for i = 0..7: the table the LNS8 datapath uses to linearise products."""
    return [round(2 ** (i / 8) * 65536) for i in range(8)]


def tlv(t: int, data: bytes) -> bytes:
    pad = (-len(data)) % 4
    return struct.pack("<HH", t, len(data)) + data + b"\0" * pad


def payload(
    build: str,
    *,
    tdp_mw: int = 75000,
    idle_mw: int = 18000,
    cap_mw: int = 75000,
    throttle_mc: int = 95000,
    critical_mc: int = 105000,
    clocks=(1500, 1000, 400),
    lut: list[int] | None = None,
) -> bytes:
    return b"".join(
        [
            tlv(TLV_LUT, struct.pack("<8I", *(lut or lns_lut()))),
            tlv(TLV_POWER, struct.pack("<3I", tdp_mw, idle_mw, cap_mw)),
            tlv(TLV_THERMAL, struct.pack("<2I", throttle_mc, critical_mc)),
            tlv(TLV_CLOCKS, struct.pack("<3I", *clocks)),
            tlv(TLV_BUILD, build.encode()),
        ]
    )


def image(version: int, rollback: int, build: str | None = None, corrupt: str | None = None, **kw) -> bytes:
    body = payload(build or f"lacc-fw {version}", **kw)
    digest = hashlib.sha256(body).digest()
    if corrupt == "payload":
        body = body[:-1] + bytes([body[-1] ^ 0xFF])  # SHA-256 no longer matches
    head = HEADER.pack(FW_MAGIC, version, rollback, len(body), digest, 0, 0)
    crc = zlib.crc32(head) & 0xFFFFFFFF
    if corrupt == "header":
        crc ^= 1
    head = HEADER.pack(FW_MAGIC, version, rollback, len(body), digest, 0, crc)
    return head + body


def config(boot_slot: int = 0, min_version: int = 0) -> bytes:
    fields = struct.pack("<III", CFG_MAGIC, boot_slot, min_version)
    return fields + struct.pack("<I", zlib.crc32(fields) & 0xFFFFFFFF)


def parse_header(data: bytes) -> dict:
    magic, version, rollback, plen, digest, flags, crc = HEADER.unpack_from(data)
    zero = HEADER.pack(magic, version, rollback, plen, digest, flags, 0)
    body = data[HEADER.size : HEADER.size + plen]
    return {
        "magic_ok": magic == FW_MAGIC,
        "version": version,
        "rollback_index": rollback,
        "payload_len": plen,
        "crc_ok": (zlib.crc32(zero) & 0xFFFFFFFF) == crc,
        "sha256_ok": len(body) == plen and hashlib.sha256(body).digest() == digest,
    }


def flash(a: bytes | None, b: bytes | None, boot_slot: int = 0, min_version: int = 0) -> bytes:
    out = bytearray(b"\xff" * FLASH_SIZE)
    if a:
        out[SLOT_A : SLOT_A + len(a)] = a
    if b:
        out[SLOT_B : SLOT_B + len(b)] = b
    out[CONFIG : CONFIG + 16] = config(boot_slot, min_version)
    return bytes(out)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("image")
    i.add_argument("--version", type=int, required=True)
    i.add_argument("--rollback", type=int, default=0)
    i.add_argument("--build")
    i.add_argument("--corrupt", choices=["payload", "header"])
    i.add_argument("-o", "--output", required=True)
    f = sub.add_parser("flash")
    f.add_argument("--a")
    f.add_argument("--b")
    f.add_argument("--boot-slot", type=int, default=0)
    f.add_argument("--min-version", type=int, default=0)
    f.add_argument("-o", "--output", required=True)
    s = sub.add_parser("inspect")
    s.add_argument("image")
    a = p.parse_args()
    if a.cmd == "image":
        Path(a.output).write_bytes(image(a.version, a.rollback, a.build, a.corrupt))
    elif a.cmd == "flash":

        def read(path: str | None) -> bytes | None:
            return Path(path).read_bytes() if path else None

        Path(a.output).write_bytes(flash(read(a.a), read(a.b), a.boot_slot, a.min_version))
    else:
        print(parse_header(Path(a.image).read_bytes()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
