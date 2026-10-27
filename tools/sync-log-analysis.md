# Offline sync log analysis

Run with Python 3.9 or newer; no packages or Linux build are needed:

```sh
python3 tools/analyze_sync_logs.py compositor.log mpv.log android.log --out sync-analysis
```

The script reads `KOLOS_SYNC` and Android `Kolos/SYNC` records containing
`t_ns`, `side`, `event`, `session`, `generation`, `slot`, `serial`, `fence_fd`.
Terminal prefixes, ordinary log lines and MPV status updates are ignored.
Malformed sync lines are reported rather than silently trusted.

Outputs:

- `report.md`: capture coverage, timing table and suspected ordering violations.
- `summary.json`: machine-readable results, unmatched stage counts, source errors.
- `events.csv`: parsed events for the selected segment with input line numbers.
- `timings.csv`: matched intervals and both source locations.

Use logs from one device boot. An idle gap of more than 30 seconds starts a new
segment; the latest segment is analyzed by default. This prevents an older MPV
run from being matched to newer Android frames with the same IDs. Inspect the
segment table, or select `--segment 1`; change the heuristic with
`--gap-seconds 60`. A short reconnect within a segment may still reuse IDs:
matches are additionally bounded in time (`--max-pair-ms 1000`) and one-to-one.
For unambiguous analysis, capture one connection per set of files.

The analyzer checks Mesa AVAILABLE -> ACQUIRED -> PRESENTED ordering, repeated
Wayland PRESENT against FREE (never ACK), and compares the next Mesa acquisition
with Android's release of the previous frame. It associates Android
PREVIOUS_RELEASE_FENCE_DONE with the previous submission, including across a
generation change. Missing history at the beginning is reported as incomplete
coverage. Detach/STOP may lack the regular trace events.

TX timestamps are currently logged after sending, so negative cross-process
intervals can occur. These are preserved and counted, not treated as GPU races.
Release-handler timing includes worker scheduling and is not hardware fence
signal time. Acquire-to-present timing is not GPU rendering time. A valid FD
does not prove it protects all relevant GPU work. No detected violation does
not prove the graphics pipeline correct.

Host-only parser and interpretation tests:

```sh
python3 -B -m unittest discover -s tools -p 'test_analyze_sync_logs.py'
```
