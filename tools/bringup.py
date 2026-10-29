#!/usr/bin/env python3
"""Run the bring-up: boot the guest with the card, drive its JTAG port, write the report.

    tools/bringup.py --qemu Q --kernel bzImage --initrd initramfs.cpio.gz --flash flash.img --out reports/bringup

QEMU's serial console is the transport: the guest prints "@@R {json}" result lines, and the
host answers "@@JTAG ready" with its own JTAG checks (OpenOCD over remote_bitbang), which go
into the same log. At the end the log becomes report.json, REPORT.md and trace.json.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / "tools"))
import report  # noqa: E402 - tools/ is not a package

REG = {"ID": 0x00, "STATUS": 0x10, "SCRATCH": 0x0C, "FW_VERSION": 0xA4, "FAULT_INJECT": 0x140}
ID_MAGIC, IDCODE, FAULT_LINK_DOWN = 0x4C414343, 0x1ACC0001, 1 << 3


class OpenOcd:
    """OpenOCD's Tcl RPC: commands end with 0x1a, so do replies."""

    def __init__(self, cfg: Path, bitbang_port: int, tcl_port: int, log: Path):
        self.log = open(log, "w")  # noqa: SIM115 - lives as long as the OpenOCD process
        self.proc = subprocess.Popen(
            ["openocd", "-f", str(cfg), "-c", f"remote_bitbang port {bitbang_port}", "-c", f"tcl_port {tcl_port}"],
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )
        deadline = time.time() + 20
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", tcl_port), timeout=10)
                break
            except OSError as e:
                if time.time() > deadline or self.proc.poll() is not None:
                    raise RuntimeError(f"OpenOCD did not start; see {log}") from e
                time.sleep(0.2)

    def cmd(self, c: str) -> str:
        self.sock.sendall(c.encode() + b"\x1a")
        buf = b""
        while not buf.endswith(b"\x1a"):
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("OpenOCD closed the connection")
            buf += chunk
        return buf[:-1].decode().strip()

    def scan(self, ir: int, bits: int, value: int) -> int:
        self.cmd(f"irscan lacc.tap {ir}")
        return int(self.cmd(f"drscan lacc.tap {bits} 0x{value:x}"), 16)

    def read(self, reg: int) -> int:
        self.scan(2, 32, reg)
        return self.scan(3, 33, 0) & 0xFFFFFFFF

    def write(self, reg: int, value: int) -> None:
        self.scan(2, 32, reg)
        self.scan(3, 33, (1 << 32) | value)

    def close(self) -> None:
        with contextlib.suppress(OSError, RuntimeError):
            self.cmd("shutdown")
        self.proc.wait(timeout=10)
        self.log.close()


def result(group: str, name: str, ok: bool, detail: str, **metrics) -> str:
    return "@@R " + json.dumps(
        {"group": group, "name": name, "pass": ok, "detail": detail, "metrics": metrics, "source": "host"}
    )


def jtag_checks(ocd: OpenOcd) -> list[str]:
    out = []
    t0 = time.perf_counter()
    idcode = ocd.scan(1, 32, 0)
    out.append(result("jtag", "TAP answers with the card's IDCODE", idcode == IDCODE, f"IDCODE 0x{idcode:08x}"))
    ident, fw, st = ocd.read(REG["ID"]), ocd.read(REG["FW_VERSION"]), ocd.read(REG["STATUS"])
    out.append(
        result(
            "jtag",
            "registers read through the debug port",
            ident == ID_MAGIC and st & 1 == 1,
            f"ID 0x{ident:08x}, firmware {fw}, STATUS 0x{st:x}",
            read_ms=(time.perf_counter() - t0) * 1e3 / 3,
        )
    )
    before = ocd.read(REG["ID"])  # a plain DR scan must not write (bit 32 clear)
    ocd.write(REG["SCRATCH"], 0x0BADCAFE)
    back = ocd.read(REG["SCRATCH"])
    out.append(
        result(
            "jtag",
            "write and read back over JTAG; reads have no side effects",
            back == 0x0BADCAFE and before == ID_MAGIC,
            f"SCRATCH 0x{back:08x}, ID still 0x{before:08x}",
        )
    )
    ocd.write(REG["FAULT_INJECT"], FAULT_LINK_DOWN)
    during = ocd.read(REG["ID"])
    st_down = ocd.read(REG["STATUS"])
    out.append(
        result(
            "jtag",
            "the debug port reads the card while the PCIe link is down",
            during == ID_MAGIC,
            f"ID 0x{during:08x}, STATUS 0x{st_down:x} with the link down (the host sees all-ones)",
        )
    )
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qemu", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--initrd", required=True)
    ap.add_argument("--flash", required=True, help="flash image; a copy is used, the original is untouched")
    ap.add_argument("--out", default=str(HERE / "reports" / "bringup"))
    ap.add_argument("--jtag-port", type=int, default=3335)
    ap.add_argument("--tcl-port", type=int, default=6666)
    ap.add_argument("--timeout", type=int, default=2400)
    ap.add_argument("--no-jtag", action="store_true")
    a = ap.parse_args()

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    flash = out / "flash-run.img"
    shutil.copyfile(a.flash, flash)
    log_path = out / "serial.log"
    log = open(log_path, "w")  # noqa: SIM115 - written line by line while the VM runs
    lines: list[str] = []
    lock = threading.Lock()

    def emit(line: str) -> None:
        with lock:
            lines.append(line)
            log.write(line + "\n")
            log.flush()

    cmd = [str(HERE / "scripts" / "run-vm.sh"), a.qemu, a.kernel, a.initrd, str(flash), "validate", str(a.jtag_port)]
    t_start = time.time()
    vm = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    timer = threading.Timer(a.timeout, vm.kill)
    timer.start()
    ocd = None
    assert vm.stdout and vm.stdin
    for raw in vm.stdout:
        line = raw.decode(errors="replace").rstrip("\r\n")
        emit(line)
        if line.startswith(("@@STAGE", "@@R")):
            print(line[:160], flush=True)
        if line.strip() == "@@JTAG ready":
            reply = "jtag done"
            try:
                if a.no_jtag or not shutil.which("openocd"):
                    raise RuntimeError("OpenOCD not available")
                ocd = OpenOcd(HERE / "scripts" / "openocd-lacc.cfg", a.jtag_port, a.tcl_port, out / "openocd.log")
                for r in jtag_checks(ocd):
                    emit(r)
                    print(r[:160], flush=True)
            except Exception as e:  # noqa: BLE001 - any failure is reported and the guest carries on
                emit(result("jtag", "host JTAG session", False, f"{type(e).__name__}: {e}"))
                reply = "jtag skipped"
            finally:
                if ocd:
                    ocd.close()
                    ocd = None
            vm.stdin.write((reply + "\n").encode())
            vm.stdin.flush()
    vm.wait()
    timer.cancel()
    log.close()
    elapsed = time.time() - t_start

    def first_line(cmd: list[str]) -> str:
        r = subprocess.run(cmd, capture_output=True, text=True, check=False)
        return ((r.stdout + r.stderr).splitlines() or ["unknown"])[0]

    env = {
        "qemu": first_line([a.qemu, "--version"]),
        "openocd": first_line(["openocd", "--version"]) if shutil.which("openocd") else "not installed",
        "wall_seconds": round(elapsed),
        "timed_out": elapsed >= a.timeout,
    }
    rep = report.build(lines, env)
    report.write(rep, lines, out)
    flash.unlink(missing_ok=True)
    s = rep["summary"]
    print(f"bring-up: {s['passed']}/{s['total']} checks passed, {s['splats']} kernel reports; {out / 'REPORT.md'}")
    return 0 if s["failed"] == 0 and s["complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
