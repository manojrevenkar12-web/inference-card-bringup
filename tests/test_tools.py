"""Host-side tools: firmware images, the register generator, the report and the JTAG session logic."""

from __future__ import annotations

import base64
import gzip
import hashlib
import importlib.util
import json
import struct
import subprocess
import sys
import zlib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


mkfw = load("mkfw", ROOT / "fw" / "mkfw.py")
sys.path.insert(0, str(ROOT / "tools"))
report = load("report", ROOT / "tools" / "report.py")
bringup = load("bringup", ROOT / "tools" / "bringup.py")


# ---- firmware images ------------------------------------------------------------------------


def test_image_header_is_56_bytes_and_verifies():
    img = mkfw.image(5, 3)
    h = mkfw.parse_header(img)
    assert mkfw.HEADER.size == 56
    assert h == {
        "magic_ok": True,
        "version": 5,
        "rollback_index": 3,
        "payload_len": len(img) - 56,
        "crc_ok": True,
        "sha256_ok": True,
    }


@pytest.mark.parametrize("corrupt,field", [("payload", "sha256_ok"), ("header", "crc_ok")])
def test_corruption_is_detected(corrupt, field):
    h = mkfw.parse_header(mkfw.image(9, 1, corrupt=corrupt))
    assert h[field] is False
    assert all(v for k, v in h.items() if k.endswith("_ok") and k != field)


def test_lut_is_two_to_the_eighths_in_q16():
    lut = mkfw.lns_lut()
    assert lut[0] == 65536 and len(lut) == 8
    for i, v in enumerate(lut):
        assert abs(v / 65536 - 2 ** (i / 8)) < 1 / 65536


def test_payload_records_are_four_byte_aligned():
    body = mkfw.payload("x" * 5)
    off, kinds = 0, []
    while off < len(body):
        t, n = struct.unpack_from("<HH", body, off)
        kinds.append(t)
        off += 4 + ((n + 3) & ~3)
    assert off == len(body) and kinds == [1, 2, 3, 4, 5]


def test_flash_layout_and_config():
    a, b = mkfw.image(2, 1), mkfw.image(1, 0)
    fl = mkfw.flash(a, b, boot_slot=2, min_version=1)
    assert len(fl) == 16 << 20
    assert fl[: len(a)] == a and fl[0x100000 : 0x100000 + len(b)] == b
    magic, slot, floor, crc = struct.unpack_from("<IIII", fl, 0x200000)
    assert (magic, slot, floor) == (mkfw.CFG_MAGIC, 2, 1)
    assert crc == zlib.crc32(fl[0x200000:0x20000C]) & 0xFFFFFFFF
    assert fl[0x201000:0x202000] == b"\xff" * 0x1000  # spare partition is erased flash


def test_payload_digest_matches_header():
    img = mkfw.image(3, 2)
    assert img[16:48] == hashlib.sha256(img[56:]).digest()


# ---- register generator ---------------------------------------------------------------------


def test_generated_files_are_up_to_date():
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_regs.py"), "--check"], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def test_generated_header_offsets_are_unique_and_aligned():
    import yaml

    m = yaml.safe_load((ROOT / "regs" / "lacc.yaml").read_text())
    offs = [r["offset"] for r in m["registers"]]
    assert len(offs) == len(set(offs))
    assert all(o % 4 == 0 and o < m["device"]["bar0_size"] for o in offs)


# ---- report ---------------------------------------------------------------------------------


def r(group, name, ok=True, **metrics):
    return "@@R " + json.dumps({"group": group, "name": name, "pass": ok, "detail": "d|x", "metrics": metrics})


def full_log(extra=()):
    lines = ["noise", "@@BOOT kernel 6.12.59"] + [f"@@STAGE {s}" for s in report.STAGES]
    lines += [r("pci", "card enumerates"), r("perf", "GEMM throughput (modelled)", lns8_tops=46.7), *extra, "@@DONE"]
    return lines


def test_report_counts_and_highlights():
    rep = report.build(full_log())
    assert rep["summary"] == {"total": 2, "passed": 2, "failed": 0, "splats": 0, "complete": True, "missing_stages": []}
    assert rep["highlights"] == [{"label": "GEMM throughput, LNS8 (modelled)", "value": 46.7, "text": "46.7 TOPS"}]
    md = report.markdown(rep)
    assert "**All 2 checks passed.**" in md and "Linux 6.12.59" in md
    assert "d\\|x" in md  # pipes in details do not break the table


def test_report_flags_failures_splats_and_missing_stages():
    lines = [
        ln
        for ln in full_log([r("dma", "round trip", ok=False), "@@SPLAT BUG: KASAN: use-after-free"])
        if ln != "@@STAGE leaks"
    ]
    rep = report.build(lines)
    s = rep["summary"]
    assert s["failed"] == 1 and s["splats"] == 1 and not s["complete"] and s["missing_stages"] == ["leaks"]
    md = report.markdown(rep)
    assert "**FAIL**" in md and "use-after-free" in md and "did not finish" in md


def test_report_unparseable_result_is_a_failure():
    rep = report.build(full_log(["@@R {not json"]))
    assert rep["summary"]["failed"] == 1


def test_report_extracts_files(tmp_path):
    blob = base64.b64encode(gzip.compress(b'{"traceEvents":[]}')).decode()
    lines = full_log([f"@@FILE trace.json {blob}", f"@@FILE ../evil {blob}"])
    rep = report.build(lines)
    report.write(rep, lines, tmp_path)
    assert (tmp_path / "trace.json").read_text() == '{"traceEvents":[]}'
    assert not (tmp_path.parent / "evil").exists()
    assert json.loads((tmp_path / "report.json").read_text())["summary"]["total"] == 2


# ---- JTAG session logic against a model of the debug port -----------------------------------


class FakeTap:
    """IR 1 IDCODE, 2 DBG_ADDR, 3 DBG_DATA (33 bits, bit 32 = write), as in qemu/hw/misc/lacc.c."""

    def __init__(self):
        self.regs = {0x00: bringup.ID_MAGIC, 0x10: 0x9, 0x0C: 0, 0xA4: 2, 0x140: 0}
        self.ir, self.addr, self.link_down = 1, 0, False

    def scan(self, ir, bits, value):
        self.ir = ir
        if ir == 1:
            return bringup.IDCODE
        if ir == 2:
            old, self.addr = self.addr, value
            return old
        captured = self.regs.get(self.addr, 0)
        if value >> 32 & 1:
            self.regs[self.addr] = value & 0xFFFFFFFF
            if self.addr == 0x140 and value & bringup.FAULT_LINK_DOWN:
                self.link_down = True
        return captured

    read = bringup.OpenOcd.read
    write = bringup.OpenOcd.write


def test_jtag_checks_pass_on_a_correct_debug_port():
    tap = FakeTap()
    lines = bringup.jtag_checks(tap)
    res = [json.loads(ln[4:]) for ln in lines]
    assert [x["pass"] for x in res] == [True] * 4
    assert tap.regs[0x0C] == 0x0BADCAFE and tap.link_down


def test_jtag_checks_catch_reads_that_write():
    class Clobbering(FakeTap):
        def scan(self, ir, bits, value):  # a debug port that writes on every scan
            if ir == 3:
                captured = self.regs.get(self.addr, 0)
                self.regs[self.addr] = value & 0xFFFFFFFF
                return captured
            return super().scan(ir, bits, value)

    res = [json.loads(ln[4:]) for ln in bringup.jtag_checks(Clobbering())]
    assert not all(x["pass"] for x in res)
