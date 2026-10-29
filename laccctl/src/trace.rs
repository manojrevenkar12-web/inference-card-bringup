//! Turn the driver's tracepoints (ftrace text) into a Chrome/Perfetto JSON trace.
//!
//! Each job becomes an async slice from submission to completion (jobs overlap in the queue),
//! named by its opcode, with the device's own timing and the result as arguments. Completion
//! batches and resets become instant events, and the number of jobs in flight a counter.

use serde_json::{json, Value};
use std::collections::HashMap;

#[derive(Debug, Clone, PartialEq)]
pub struct Event {
    pub task: String,
    pub pid: u32,
    pub ts_us: f64,
    pub name: String,
    pub args: HashMap<String, String>,
}

/// Parse one line of /sys/kernel/tracing/trace, e.g.
/// `  lacc-validate-123   [001] ..... 12.345678: lacc_submit: seq=5 opcode=7 ndesc=1`
pub fn parse_line(line: &str) -> Option<Event> {
    let at = line.find(": lacc_")?;
    let head = &line[..at];
    let rest = &line[at + 2..];
    let (name, args) = rest.split_once(": ").unwrap_or((rest, ""));
    let ts = head
        .rsplit(char::is_whitespace)
        .next()?
        .parse::<f64>()
        .ok()?;
    let who = head.trim_start().split(" [").next()?.trim_end();
    let (task, pid) = match who.rsplit_once('-') {
        Some((t, p)) => (t.to_string(), p.parse().unwrap_or(0)),
        None => (who.to_string(), 0),
    };
    let mut map = HashMap::new();
    if name == "lacc_reset" {
        map.insert("why".to_string(), args.to_string());
    } else {
        for kv in args.split_whitespace() {
            if let Some((k, v)) = kv.split_once('=') {
                map.insert(k.to_string(), v.to_string());
            }
        }
    }
    Some(Event {
        task,
        pid,
        ts_us: ts * 1e6,
        name: name.to_string(),
        args: map,
    })
}

pub fn opcode_name(op: u32) -> &'static str {
    match op {
        0 => "nop",
        1 => "h2d",
        2 => "d2h",
        3 => "gemm_lns8",
        4 => "gemm_i8",
        5 => "sg_h2d",
        6 => "sg_d2h",
        7 => "fill",
        _ => "unknown",
    }
}

pub struct Summary {
    pub jobs: usize,
    pub failed: usize,
    pub resets: usize,
    pub p50_us: f64,
    pub p99_us: f64,
}

pub fn to_perfetto(events: &[Event]) -> (Value, Summary) {
    const PID: u32 = 1;
    let mut out = vec![
        json!({"ph": "M", "pid": PID, "name": "process_name", "args": {"name": "lacc0"}}),
        json!({"ph": "M", "pid": PID, "tid": 1, "name": "thread_name", "args": {"name": "jobs"}}),
    ];
    let mut open: HashMap<u64, &Event> = HashMap::new();
    let mut latencies = Vec::new();
    let (mut failed, mut resets) = (0, 0);
    let arg = |e: &Event, k: &str| {
        e.args
            .get(k)
            .and_then(|v| v.parse::<u64>().ok())
            .unwrap_or(0)
    };
    for e in events {
        match e.name.as_str() {
            "lacc_submit" => {
                let seq = arg(e, "seq");
                open.insert(seq, e);
                let name = opcode_name(arg(e, "opcode") as u32);
                out.push(json!({"ph": "b", "cat": "job", "name": name, "id": seq, "pid": PID, "tid": 1, "ts": e.ts_us,
                                 "args": {"seq": seq, "descriptors": arg(e, "ndesc"), "submitter": format!("{}-{}", e.task, e.pid)}}));
                out.push(json!({"ph": "C", "name": "in flight", "pid": PID, "ts": e.ts_us, "args": {"jobs": open.len()}}));
            }
            "lacc_complete" => {
                let seq = arg(e, "seq");
                let Some(s) = open.remove(&seq) else { continue };
                let name = opcode_name(arg(s, "opcode") as u32);
                let result = arg(e, "result");
                let result_name = ["ok", "failed", "aborted"].get(result as usize).copied().unwrap_or("?");
                failed += (result != 0) as usize;
                latencies.push(e.ts_us - s.ts_us);
                out.push(json!({"ph": "e", "cat": "job", "name": name, "id": seq, "pid": PID, "tid": 1, "ts": e.ts_us,
                                 "args": {"status": arg(e, "status"), "result": result_name,
                                          "device_ns": arg(e, "device_ns")}}));
                out.push(json!({"ph": "C", "name": "in flight", "pid": PID, "ts": e.ts_us, "args": {"jobs": open.len()}}));
            }
            "lacc_cq" => out.push(json!({"ph": "i", "s": "t", "name": "completions", "pid": PID, "tid": 2, "ts": e.ts_us,
                                         "args": {"cqes": arg(e, "cqes"), "head": arg(e, "head")}})),
            "lacc_reset" => {
                resets += 1;
                out.push(json!({"ph": "i", "s": "p", "name": "reset", "pid": PID, "tid": 2, "ts": e.ts_us,
                                "args": {"why": e.args.get("why").cloned().unwrap_or_default()}}));
            }
            _ => {}
        }
    }
    out.insert(2, json!({"ph": "M", "pid": PID, "tid": 2, "name": "thread_name", "args": {"name": "interrupts and resets"}}));
    latencies.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let pct = |p: f64| {
        if latencies.is_empty() {
            0.0
        } else {
            latencies[((latencies.len() - 1) as f64 * p).round() as usize]
        }
    };
    let summary = Summary {
        jobs: latencies.len(),
        failed,
        resets,
        p50_us: pct(0.5),
        p99_us: pct(0.99),
    };
    (
        json!({"traceEvents": out, "displayTimeUnit": "ns"}),
        summary,
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = "\
# tracer: nop
#
     lacc-validate-131     [000] ..... 1000.000100: lacc_submit: seq=1 opcode=7 ndesc=1
     lacc-validate-131     [000] ..... 1000.000150: lacc_submit: seq=2 opcode=3 ndesc=1
          <idle>-0         [001] d.h1. 1000.000300: lacc_cq: cqes=1 head=1
 irq/24-lacc0-cq-57        [001] ..... 1000.000310: lacc_complete: seq=1 status=0 result=0 device_ns=2100
 irq/24-lacc0-cq-57        [001] ..... 1000.000900: lacc_complete: seq=2 status=1 result=1 device_ns=700000
     kworker/0:1-12        [000] ..... 1000.001000: lacc_reset: engine hang: no progress for 500 ms
";

    #[test]
    fn parses_task_pid_time_and_args() {
        let e = parse_line(SAMPLE.lines().nth(2).unwrap()).unwrap();
        assert_eq!(e.task, "lacc-validate");
        assert_eq!(e.pid, 131);
        assert!((e.ts_us - 1000.0001e6).abs() < 1e-3);
        assert_eq!(e.name, "lacc_submit");
        assert_eq!(e.args["opcode"], "7");
        let irq = parse_line(SAMPLE.lines().nth(5).unwrap()).unwrap();
        assert_eq!(irq.task, "irq/24-lacc0-cq");
        assert_eq!(irq.pid, 57);
        let r = parse_line(SAMPLE.lines().last().unwrap()).unwrap();
        assert_eq!(r.args["why"], "engine hang: no progress for 500 ms");
        assert!(parse_line("# tracer: nop").is_none());
    }

    #[test]
    fn jobs_become_async_slices() {
        let ev: Vec<_> = SAMPLE.lines().filter_map(parse_line).collect();
        let (trace, s) = to_perfetto(&ev);
        assert_eq!(s.jobs, 2);
        assert_eq!(s.failed, 1);
        assert_eq!(s.resets, 1);
        let evs = trace["traceEvents"].as_array().unwrap();
        let begins: Vec<_> = evs.iter().filter(|e| e["ph"] == "b").collect();
        assert_eq!(begins.len(), 2);
        assert_eq!(begins[1]["name"], "gemm_lns8");
        let end2 = evs.iter().find(|e| e["ph"] == "e" && e["id"] == 2).unwrap();
        assert_eq!(end2["args"]["result"], "failed");
        assert!((s.p99_us - 750.0).abs() < 1e-6);
    }
}
