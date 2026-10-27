#!/usr/bin/env python3
"""Analyze Kolos sync traces offline. Python 3.9+, standard library only."""

import argparse
import csv
import json
import math
import re
import statistics
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass(frozen=True)
class Event:
    t_ns: int
    side: str
    event: str
    session: int
    generation: int
    slot: int
    serial: int
    fence_fd: int
    source: str
    line: int

    @property
    def buffer(self):
        return self.session, self.generation, self.slot

    @property
    def frame(self):
        return self.buffer + (self.serial,)


def parse_logs(paths):
    events, warnings, errors, video_rates = [], [], [], []
    seen = set()
    duplicates = 0
    for path in paths:
        with path.open(encoding="utf-8-sig", errors="replace") as stream:
            for number, line in enumerate(stream, 1):
                if "Video" in line and (fps := re.search(r"(\d+(?:\.\d+)?) fps", line)):
                    video_rates.append(float(fps.group(1)))
                if re.search(r"\b(failed|timeout|fatal|error)\b", line, re.I):
                    errors.append({"source": str(path), "line": number, "text": line.strip()})
                if "t_ns=" not in line:
                    continue
                fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
                try:
                    event = Event(
                        **{k: int(fields[k]) for k in
                           ("t_ns", "session", "generation", "slot", "serial", "fence_fd")},
                        side=fields["side"], event=fields["event"],
                        source=str(path), line=number)
                except (KeyError, ValueError):
                    warnings.append(f"Malformed trace: {path}:{number}")
                    continue
                identity = (event.t_ns, event.side, event.event, event.frame, event.fence_fd)
                if identity in seen:
                    duplicates += 1
                    continue
                seen.add(identity)
                events.append(event)
    return sorted(events, key=lambda e: e.t_ns), warnings, errors, video_rates, duplicates


def split_segments(events, gap_ns):
    """Idle-gap heuristic, not a boot/session identifier. Never shift clocks."""
    segments = []
    for event in events:
        if not segments or event.t_ns - segments[-1][-1].t_ns > gap_ns:
            segments.append([])
        segments[-1].append(event)
    return segments


def summary_ms(values):
    if not values:
        return {"n": 0}
    values = sorted(values)
    return {
        "n": len(values), "min_ms": min(values), "max_ms": max(values),
        "mean_ms": statistics.mean(values), "median_ms": statistics.median(values),
        "p95_ms": values[math.ceil(0.95 * len(values)) - 1],
        "negative_count": sum(v < 0 for v in values),
    }


def match_events(left, right, max_ns, by_frame=True):
    """Nearest, one-to-one, bounded matches. Preserve negative trace deltas."""
    groups = defaultdict(list)
    key = (lambda e: e.frame) if by_frame else (lambda e: e.buffer)
    for event in right:
        groups[key(event)].append(event)
    for group in groups.values():
        group.sort(key=lambda e: e.t_ns)
    times = {k: [e.t_ns for e in group] for k, group in groups.items()}
    used, pairs = set(), []
    for start in left:
        k = key(start)
        clock = times.get(k, [])
        low = bisect_left(clock, start.t_ns - max_ns)
        high = bisect_right(clock, start.t_ns + max_ns)
        candidates = [end for end in groups[k][low:high] if end not in used]
        if candidates:
            end = min(candidates, key=lambda e: abs(e.t_ns - start.t_ns))
            pairs.append((start, end))
            used.add(end)
    return pairs


# TX points are logged after writes/enqueue; their differences are approximate.
STAGES = [
    ("mesa_to_wayland", "mesa", "SLOT_PRESENTED", "wayland", "PRESENT_RX", True),
    ("wayland_to_android", "wayland", "PRESENT_RX", "android", "PRESENT_RX", True),
    ("bridge_to_android", "bridge", "PRESENT_TX", "android", "PRESENT_RX", True),
    ("android_submit", "android", "PRESENT_RX", "android", "TX_APPLY", True),
    ("android_commit", "android", "TX_APPLY", "android", "TX_COMMIT", True),
    ("android_complete", "android", "TX_APPLY", "android", "TX_COMPLETE", True),
    ("release_handler", "android", "PREVIOUS_RELEASE_FENCE_RX", "android", "PREVIOUS_RELEASE_FENCE_DONE", True),
    ("android_free_dispatch", "android", "SLOT_AVAILABLE", "android", "FREE_TX", False),
    ("android_release_to_mesa_free", "android", "SLOT_AVAILABLE", "mesa", "FREE_RX", False),
    ("android_free_to_bridge", "android", "FREE_TX", "bridge", "FREE_RX", False),
    ("bridge_free_to_wayland", "bridge", "FREE_RX", "wayland", "FREE_TX", False),
    ("wayland_free_to_mesa", "wayland", "FREE_TX", "mesa", "FREE_RX", False),
]


def analyze(events, max_pair_ns):
    indexed = defaultdict(list)
    for e in events:
        indexed[e.side, e.event].append(e)
    rows, metrics, findings, coverage = [], {}, [], Counter()

    def record(name, pairs, left_count=None, right_count=None):
        metrics[name] = summary_ms([(b.t_ns - a.t_ns) / 1e6 for a, b in pairs])
        metrics[name].update(left_count=left_count, right_count=right_count)
        for a, b in pairs:
            rows.append({"metric": name, "session": a.session, "generation": a.generation,
                         "slot": a.slot, "serial": a.serial, "start_ns": a.t_ns,
                         "end_ns": b.t_ns, "delta_ms": (b.t_ns - a.t_ns) / 1e6,
                         "start_source": a.source, "start_line": a.line,
                         "end_source": b.source, "end_line": b.line})

    def finding(reason, a, b=None):
        findings.append({"reason": reason, "event": asdict(a),
                         "related": asdict(b) if b else None})

    for name, s1, e1, s2, e2, by_frame in STAGES:
        left, right = indexed[s1, e1], indexed[s2, e2]
        # FREE lacks serial: allow at most 20 ms to avoid pairing a later cycle.
        bound = max_pair_ns if by_frame else min(max_pair_ns, 20_000_000)
        record(name, match_events(left, right, bound, by_frame), len(left), len(right))

    # Local Mesa lifecycle, including assigning each serial-less acquire to its present.
    state, acquired, previous_present, last_available, last_free = {}, {}, {}, {}, {}
    acquire_present, free_acquire, reuse = [], [], []
    for e in events:
        if e.side != "mesa":
            continue
        k = e.buffer
        if e.event == "FREE_RX":
            last_free[k] = e
        elif e.event == "SLOT_AVAILABLE":
            if state.get(k) == "acquired":
                finding("Mesa became AVAILABLE while already ACQUIRED", e, acquired[k])
            state[k] = "available"
            last_available[k] = e
        elif e.event == "SLOT_ACQUIRED":
            if k not in state:
                coverage["mesa_acquire_without_capture_history"] += 1
            elif state[k] != "available":
                finding("Mesa ACQUIRED without intervening AVAILABLE", e, previous_present.get(k))
            else:
                coverage["mesa_acquire_after_available"] += 1
                free_acquire.append((last_available[k], e))
            if k in previous_present:
                reuse.append((previous_present[k], e, last_free.get(k)))
            state[k], acquired[k] = "acquired", e
        elif e.event == "SLOT_PRESENTED":
            if k in state and state[k] != "acquired":
                finding("Mesa PRESENTED without ACQUIRED", e)
            if state.get(k) == "acquired":
                acquire_present.append((acquired[k], e))
            state[k], previous_present[k] = "presented", e
    record("mesa_available_to_acquire", free_acquire)
    record("mesa_acquire_to_present", acquire_present)

    # Compare actual next Mesa acquisition with Android release of the OLD frame.
    released = indexed["android", "SLOT_AVAILABLE"]
    released_by_frame = defaultdict(list)
    for rel in released:
        released_by_frame[rel.frame].append(rel)
    release_to_reacquire, release_to_free = [], []
    for old_present, next_acquire, free_rx in reuse:
        matches = match_events([old_present], released_by_frame[old_present.frame], max_pair_ns)
        if matches:
            rel = matches[0][1]
            release_to_reacquire.append((rel, next_acquire))
            if free_rx and old_present.t_ns < free_rx.t_ns <= next_acquire.t_ns:
                # The acquire/present cycle identifies FREE without a time-only guess.
                release_to_free.append((rel, free_rx))
            if next_acquire.t_ns < rel.t_ns:
                finding("Mesa reacquired before Android SLOT_AVAILABLE", next_acquire, rel)
    record("android_release_to_next_mesa_acquire", release_to_reacquire)
    record("android_release_to_mesa_free_by_cycle", release_to_free)
    coverage["cross_side_reacquires_checked"] = len(release_to_reacquire)

    # OnComplete's previous-release fence belongs to the PREVIOUS submitted image.
    prior, predecessors = {}, {}
    received_by_frame = defaultdict(list)
    for e in indexed["android", "PRESENT_RX"]:
        received_by_frame[e.frame].append(e)
        if e.session in prior:
            predecessors[e] = prior[e.session]
        prior[e.session] = e
    release_order = []
    for done in indexed["android", "PREVIOUS_RELEASE_FENCE_DONE"]:
        current = match_events([done], received_by_frame[done.frame], max_pair_ns)
        if current and current[0][1] in predecessors:
            previous = predecessors[current[0][1]]
            match = match_events([previous], released_by_frame[previous.frame], max_pair_ns)
            if match:
                rel = match[0][1]
                release_order.append((done, rel))
                if rel.t_ns < done.t_ns:
                    finding("Android released previous slot before release handler completed", rel, done)
    record("release_done_to_previous_available", release_order)
    coverage["previous_release_order_checked"] = len(release_order)

    # ACK never changes availability.
    free, outstanding = {}, {}
    for e in events:
        if e.side != "wayland":
            continue
        if e.event == "FREE_TX":
            free[e.buffer] = e
        elif e.event == "PRESENT_RX":
            if e.buffer in outstanding and e.buffer not in free:
                finding("Wayland repeated PRESENT without FREE", e, outstanding[e.buffer])
            elif e.buffer in free:
                coverage["wayland_present_after_free"] += 1
            else:
                coverage["wayland_present_without_capture_history"] += 1
            free.pop(e.buffer, None)
            outstanding[e.buffer] = e

    rates = {}
    for side, name in [("mesa", "SLOT_PRESENTED"), ("wayland", "PRESENT_RX"), ("android", "PRESENT_RX")]:
        groups = defaultdict(list)
        for e in indexed[side, name]:
            groups[e.session, e.generation].append(e)
        for (session, generation), group in groups.items():
            span = (group[-1].t_ns - group[0].t_ns) / 1e9
            rates[f"{side}/session={session}/generation={generation}"] = {
                "frames": len(group), "first_serial": group[0].serial,
                "last_serial": group[-1].serial, "span_seconds": span,
                "presents_per_second": (len(group) - 1) / span if span else None}

    return {"counts": dict(Counter(f"{e.side}:{e.event}" for e in events)),
            "metrics": metrics, "coverage": dict(coverage), "suspected_violations": findings,
            "rates": rates}, rows


def write_report(path, report):
    lines = ["# Kolos sync log analysis", "",
             f"Selected segment: {report['selected_segment']} / {len(report['segments'])}.",
             f"Input events: {report['input_events']}; analyzed: {report['analyzed_events']}.", "",
             "## Capture segments", "", "| Segment | Start ns | End ns | Sides | Events |",
             "|---|---:|---:|---|---:|"]
    for i, segment in enumerate(report["segments"], 1):
        lines.append(f"| {i} | {segment['start_ns']} | {segment['end_ns']} | {', '.join(segment['sides'])} | {segment['events']} |")
    lines += ["", "## Coverage and findings", "", "```json",
              json.dumps(report["coverage"], indent=2), "```", "",
              f"Suspected violations: {len(report['suspected_violations'])}.",
              "These are trace-order findings; missing/truncated logs can mimic violations.", ""]
    for f in report["suspected_violations"][:30]:
        e = f["event"]
        lines.append(f"- {f['reason']}: {e['source']}:{e['line']} (slot {e['slot']}, serial {e['serial']})")
    lines += ["", "## Timings (milliseconds)", "",
              "| Stage | Matched | Median | P95 | Max | Negative |",
              "|---|---:|---:|---:|---:|---:|"]
    for name, m in report["metrics"].items():
        if m["n"]:
            lines.append(f"| {name} | {m['n']} | {m['median_ms']:.3f} | {m['p95_ms']:.3f} | {m['max_ms']:.3f} | {m['negative_count']} |")
    lines += ["", "## PRESENT rates (not display FPS)", "", "```json",
              json.dumps(report["rates"], indent=2), "```", "", "## Limitations", "",
              "- Same device boot and comparable CLOCK_MONOTONIC clocks are required. No clock offsets are fitted.",
              "- Idle gaps delimit capture segments heuristically. Default analysis uses only the latest segment.",
              "- Identical session/generation/serial values can recur across connections. Matches also require proximity in time.",
              "- TX timestamps occur after writes/enqueue. Negative cross-process deltas are retained, not treated as GPU races.",
              "- FREE has no serial. Its time-only matches are bounded to 20 ms; unmatched events are not invented or forced.",
              "- release_handler includes thread creation/scheduling and logging, not only GPU waiting.",
              "- mesa_acquire_to_present includes CPU work, rendering submission and scheduling; it is not GPU duration.",
              "- SLOT_ACQUIRED is CPU image acquisition, not the instant GPU starts writing.",
              "- No early reacquisition in the trace does not prove the fences cover the correct GPU work or memory visibility.",
              "- Error text and video FPS metadata come from all input segments; inspect source line references.", ""]
    if report["warnings"]:
        lines += ["## Warnings", ""] + [f"- {w}" for w in report["warnings"]]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path, help="Terminal or logcat text exports")
    parser.add_argument("--out", type=Path, default=Path("sync-analysis"))
    parser.add_argument("--gap-seconds", type=float, default=30, help="Idle gap separating capture segments")
    parser.add_argument("--segment", default="latest", help="latest or a 1-based segment number")
    parser.add_argument("--max-pair-ms", type=float, default=1000, help="Maximum time distance for frame matches")
    args = parser.parse_args()
    if args.gap_seconds <= 0 or args.max_pair_ms <= 0:
        parser.error("Time bounds must be positive")
    try:
        events, warnings, errors, rates, duplicates = parse_logs(args.logs)
    except OSError as error:
        parser.error(str(error))
    if not events:
        parser.error("No complete t_ns sync events found")
    segments = split_segments(events, int(args.gap_seconds * 1e9))
    try:
        number = len(segments) if args.segment == "latest" else int(args.segment)
        if number < 1 or number > len(segments):
            raise ValueError()
    except ValueError:
        parser.error("Invalid segment number")
    chosen = segments[number - 1]
    report, rows = analyze(chosen, int(args.max_pair_ms * 1e6))
    report.update(input_events=len(events), analyzed_events=len(chosen),
                  selected_segment=number, duplicates_removed=duplicates, warnings=warnings,
                  error_lines=errors, input_video_fps=sorted(set(rates)),
                  segments=[{"start_ns": s[0].t_ns, "end_ns": s[-1].t_ns,
                             "events": len(s), "sides": sorted(set(e.side for e in s))} for s in segments])
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    write_report(args.out / "report.md", report)
    for name, data in [("events.csv", [asdict(e) for e in chosen]), ("timings.csv", rows)]:
        if data:
            with (args.out / name).open("w", newline="", encoding="utf-8-sig") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(data[0]))
                writer.writeheader()
                writer.writerows(data)
    print(json.dumps({"segment": f"{number}/{len(segments)}", "events": len(chosen),
                      "coverage": report["coverage"], "rates": report["rates"],
                      "suspected_violations": len(report["suspected_violations"]),
                      "malformed_lines": len(warnings), "report": str(args.out / "report.md")}, indent=2))


if __name__ == "__main__":
    main()
