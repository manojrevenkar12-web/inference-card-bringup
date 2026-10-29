#!/usr/bin/env python3
"""Turn a bring-up serial log into report.json, REPORT.md and trace.json.

    tools/report.py serial.log --out reports/bringup

The log holds "@@R {json}" result lines from the guest (and from the host's JTAG session),
"@@STAGE", "@@SPLAT", "@@BOOT" and "@@FILE name base64(gzip)" lines. Everything else is the
ordinary console. The report is generated; edit this file, not REPORT.md.
"""

from __future__ import annotations

import argparse
import base64
import gzip
import json
import re
from pathlib import Path

STAGES = ["validate", "inference", "jtag", "firmware", "trace", "reload", "leaks"]
GROUPS = [
    ("pci", "PCI enumeration"),
    ("driver", "Driver"),
    ("dma", "DMA"),
    ("compute", "Compute"),
    ("inference", "Inference"),
    ("board", "Board management (SMBus, SPI flash, boot ROM)"),
    ("concurrency", "Concurrency"),
    ("perf", "Performance"),
    ("power", "Power and thermal"),
    ("faults", "Fault injection and recovery"),
    ("jtag", "JTAG debug port"),
    ("firmware", "Firmware update"),
    ("trace", "Tracing"),
    ("kernel", "Kernel health"),
]

# (group, check-name prefix, metric, label, format)
HIGHLIGHTS = [
    ("compute", "LNS8 vs INT8", "gaussian_lns8_rel_error", "Matmul error vs FP64, Gaussian data, LNS8", "pct"),
    ("compute", "LNS8 vs INT8", "gaussian_int8_rel_error", "Matmul error vs FP64, Gaussian data, INT8", "pct"),
    ("compute", "LNS8 vs INT8", "heavy_tail_lns8_rel_error", "Matmul error vs FP64, heavy-tailed data, LNS8", "pct"),
    ("compute", "LNS8 vs INT8", "heavy_tail_int8_rel_error", "Matmul error vs FP64, heavy-tailed data, INT8", "pct"),
    ("compute", "LNS8 vs INT8", "lns8_energy_uj_per_gemm", "Energy per 192x192x256 GEMM, LNS8 (modelled)", "uj"),
    ("compute", "LNS8 vs INT8", "int8_energy_uj_per_gemm", "Energy per 192x192x256 GEMM, INT8 (modelled)", "uj"),
    ("inference", "MLP", "fp32_accuracy", "Classifier accuracy, FP32 (host)", "pct"),
    ("inference", "MLP", "lns8_accuracy", "Classifier accuracy, LNS8 on the card", "pct"),
    ("inference", "MLP", "int8_accuracy", "Classifier accuracy, INT8 on the card", "pct"),
    ("inference", "MLP", "lns8_agreement", "Predictions equal to FP32, LNS8", "pct"),
    ("inference", "MLP", "int8_agreement", "Predictions equal to FP32, INT8", "pct"),
    ("perf", "GEMM throughput", "lns8_tops", "GEMM throughput, LNS8 (modelled)", "tops"),
    ("perf", "host-to-card", "sg_model_gbps", "Host-to-card DMA, one SG descriptor (modelled)", "gbps"),
    ("perf", "host-to-card", "per_segment_model_gbps", "Host-to-card DMA, descriptor per segment (modelled)", "gbps"),
    ("perf", "interrupt coalescing", "irqs_off", "Interrupts for 512 jobs, coalescing off", "int"),
    ("perf", "interrupt coalescing", "irqs_32_1ms", "Interrupts for 512 jobs, 32 jobs / 1 ms", "int"),
    ("power", "a power cap", "uncapped_avg_w", "Board power under INT8 load, uncapped", "w"),
    ("power", "a power cap", "capped_avg_w", "Board power under the same load, 40 W cap", "w"),
    ("power", "a power cap", "clock_capped_mhz", "Clock the controller settled at under the cap", "mhz"),
    ("faults", "a hung engine", "detect_ms", "Hung engine: detected, reset and job aborted", "ms"),
    ("faults", "link down", "recovery_ms", "Link down: driver back to running jobs", "ms"),
]

FMT = {
    "pct": lambda v: f"{v * 100:.2f}%",
    "uj": lambda v: f"{v:.1f} µJ",
    "tops": lambda v: f"{v:.1f} TOPS",
    "gbps": lambda v: f"{v:.2f} GB/s",
    "int": lambda v: f"{int(v)}",
    "w": lambda v: f"{v:.1f} W",
    "mhz": lambda v: f"{int(v)} MHz",
    "ms": lambda v: f"{v:.0f} ms",
}


def parse(lines: list[str]) -> dict:
    results, splats, stages, files, boot, done = [], [], [], {}, None, False
    for raw in lines:
        line = raw.strip()
        if line.startswith("@@R "):
            try:
                results.append(json.loads(line[4:]))
            except json.JSONDecodeError:
                results.append(
                    {
                        "group": "report",
                        "name": "unparseable result line",
                        "pass": False,
                        "detail": line[:200],
                        "metrics": {},
                    }
                )
        elif line.startswith("@@SPLAT "):
            splats.append(line[8:])
        elif line.startswith("@@STAGE "):
            stages.append(line[8:])
        elif line.startswith("@@BOOT "):
            boot = line[7:]
        elif line.startswith("@@FILE "):
            parts = line.split(" ", 2)
            if len(parts) == 3:
                try:
                    files[parts[1]] = gzip.decompress(base64.b64decode(parts[2]))
                except (ValueError, OSError, EOFError) as e:
                    results.append(
                        {
                            "group": "report",
                            "name": f"attached file {parts[1]} is damaged",
                            "pass": False,
                            "detail": str(e)[:200],
                            "metrics": {},
                        }
                    )
        elif line == "@@DONE":
            done = True
    if boot and boot.startswith("kernel "):
        boot = "Linux " + boot[len("kernel ") :]
    return {"results": results, "splats": splats, "stages": stages, "files": files, "boot": boot, "done": done}


def build(lines: list[str], env: dict | None = None) -> dict:
    p = parse(lines)
    res = p["results"]
    missing = [s for s in STAGES if s not in p["stages"]]
    passed = sum(1 for r in res if r.get("pass"))
    highlights = []
    for group, prefix, key, label, kind in HIGHLIGHTS:
        for r in res:
            if r.get("group") == group and r.get("name", "").startswith(prefix) and key in r.get("metrics", {}):
                v = r["metrics"][key]
                if isinstance(v, (int, float)):
                    highlights.append({"label": label, "value": v, "text": FMT[kind](v)})
                break
    return {
        "summary": {
            "total": len(res),
            "passed": passed,
            "failed": len(res) - passed,
            "splats": len(p["splats"]),
            "complete": p["done"] and not missing,
            "missing_stages": missing,
        },
        "environment": {"guest_kernel": p["boot"], **(env or {})},
        "results": res,
        "highlights": highlights,
        "kernel_reports": p["splats"],
        "_files": p["files"],
    }


def esc(s: str) -> str:
    return s.replace("|", "\\|").replace("\n", " ")


def markdown(rep: dict) -> str:
    s, env = rep["summary"], rep["environment"]
    failed = s["failed"]
    verdict = f"{failed} of {s['total']} checks failed" if failed else f"All {s['total']} checks passed"
    if not s["complete"]:
        verdict += f"; the run did not finish (missing stages: {', '.join(s['missing_stages']) or 'none'})"
    out = [
        "# Bring-up report: LACC virtual inference card",
        "",
        "<!-- Generated by tools/report.py from serial.log. Do not edit. -->",
        "",
        f"**{verdict}.** Kernel reports (KASAN, lockdep, DMA-API, UBSAN, WARN): {s['splats']}.",
        "",
        f"- Guest kernel: {env.get('guest_kernel') or 'unknown'} (KASAN, lockdep, DMA-API debug, kmemleak)",
        f"- Emulator: {env.get('qemu', 'unknown')}; debugger: {env.get('openocd', 'unknown')}",
        f"- Run time: {env.get('wall_seconds', '?')} s under TCG emulation",
        "",
        "The card is a model in QEMU (`qemu/hw/misc/lacc.c`). Results marked *modelled* come from its timing and",
        "energy model (`docs/MODEL.md`); they describe the model, not silicon. Everything else (enumeration, DMA",
        "correctness, bit-exact compute, recovery, firmware handling, sensors, JTAG) is checked end to end",
        "through the real Linux driver.",
        "",
    ]
    if rep["highlights"]:
        out += ["## Measurements", "", "| Measurement | Value |", "|---|---|"]
        out += [f"| {h['label']} | {h['text']} |" for h in rep["highlights"]]
        out.append("")
    by_group: dict[str, list] = {}
    for r in rep["results"]:
        by_group.setdefault(r.get("group", "other"), []).append(r)
    names = dict(GROUPS)
    order = [g for g, _ in GROUPS] + sorted(set(by_group) - set(names))
    out += ["## Checks", ""]
    for g in order:
        if g not in by_group:
            continue
        rows = by_group[g]
        ok = sum(1 for r in rows if r.get("pass"))
        out += [f"### {names.get(g, g)} ({ok}/{len(rows)})", "", "| Result | Check | Detail |", "|---|---|---|"]
        for r in rows:
            mark = "pass" if r.get("pass") else "**FAIL**"
            where = " (host)" if r.get("source") == "host" else ""
            out.append(f"| {mark} | {esc(r.get('name', ''))}{where} | {esc(r.get('detail', ''))} |")
        out.append("")
    out += ["## Kernel log", ""]
    if rep["kernel_reports"]:
        out += ["```"] + rep["kernel_reports"] + ["```", ""]
    else:
        out += ["No KASAN, lockdep, DMA-API debug, UBSAN or WARN reports.", ""]
    if "trace.json" in rep["_files"]:
        out += [
            "## Trace",
            "",
            "`trace.json` is a Perfetto trace of a two-second mixed workload, recorded from the",
            "driver's tracepoints (`lacc:lacc_submit`, `lacc_complete`, `lacc_cq`, `lacc_reset`). Open it at",
            "ui.perfetto.dev.",
            "",
        ]
    return "\n".join(out)


def write(rep: dict, lines: list[str], out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for name, data in rep["_files"].items():
        if re.fullmatch(r"[\w.-]+", name):
            (out / name).write_bytes(data)
    clean = {k: v for k, v in rep.items() if not k.startswith("_")}
    (out / "report.json").write_text(json.dumps(clean, indent=2) + "\n")
    (out / "REPORT.md").write_text(markdown(rep) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    lines = Path(a.log).read_text(errors="replace").splitlines()
    rep = build(lines)
    write(rep, lines, Path(a.out))
    s = rep["summary"]
    print(f"{s['passed']}/{s['total']} passed, {s['splats']} kernel reports, complete={s['complete']}")
    return 0 if s["failed"] == 0 and s["complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
