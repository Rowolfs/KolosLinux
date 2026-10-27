"""Host-only tests of trace interpretation; no Vulkan/device dependencies."""

import tempfile
import unittest
from pathlib import Path

from analyze_sync_logs import Event, analyze, match_events, parse_logs, split_segments


def event(time, side, name, slot=0, serial=-1, generation=1):
    return Event(time, side, name, 2, generation, slot, serial, 7, "fixture", time)


class TraceTests(unittest.TestCase):
    def test_terminal_prefix_dedup_and_malformed_trace(self):
        line = "I KOLOS_SYNC t_ns=10 side=mesa event=SLOT_ACQUIRED session=2 generation=1 slot=0 serial=-1 fence_fd=-1\n"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log.txt"
            path.write_text("mpv status\n" + line + line + "t_ns=bad\n", encoding="utf-8")
            events, warnings, _, _, duplicates = parse_logs([path])
        self.assertEqual(len(events), 1)
        self.assertEqual(len(warnings), 1)
        self.assertEqual(duplicates, 1)

    def test_old_run_with_same_ids_is_not_paired(self):
        old = event(10, "mesa", "SLOT_PRESENTED", serial=0)
        new = event(10_000, "android", "PRESENT_RX", serial=0)
        self.assertEqual(match_events([old], [new], 100), [])
        self.assertEqual(len(split_segments([old, new], 1000)), 2)

    def test_post_send_timestamp_can_be_after_receive(self):
        tx = event(110, "bridge", "PRESENT_TX", serial=0)
        rx = event(100, "android", "PRESENT_RX", serial=0)
        result, _ = analyze([rx, tx], 1000)
        self.assertEqual(result["metrics"]["bridge_to_android"]["negative_count"], 1)
        self.assertEqual(result["suspected_violations"], [])

    def test_partial_capture_is_not_claimed_as_violation(self):
        events = [event(10, "mesa", "SLOT_ACQUIRED"),
                  event(20, "mesa", "SLOT_PRESENTED", serial=3)]
        result, _ = analyze(events, 1000)
        self.assertEqual(result["suspected_violations"], [])
        self.assertEqual(result["coverage"]["mesa_acquire_without_capture_history"], 1)

    def test_reacquisition_without_free_is_detected(self):
        events = [event(1, "mesa", "SLOT_AVAILABLE"),
                  event(2, "mesa", "SLOT_ACQUIRED"),
                  event(3, "mesa", "SLOT_PRESENTED", serial=0),
                  event(4, "mesa", "SLOT_ACQUIRED")]
        result, _ = analyze(events, 1000)
        self.assertTrue(any("without intervening" in f["reason"]
                            for f in result["suspected_violations"]))

    def test_cross_side_early_reacquisition_is_detected(self):
        events = [event(1, "mesa", "SLOT_AVAILABLE"),
                  event(2, "mesa", "SLOT_ACQUIRED"),
                  event(3, "mesa", "SLOT_PRESENTED", serial=0),
                  event(4, "mesa", "SLOT_AVAILABLE"),
                  event(5, "mesa", "SLOT_ACQUIRED"),
                  event(6, "android", "SLOT_AVAILABLE", serial=0)]
        result, _ = analyze(events, 1000)
        self.assertTrue(any("before Android" in f["reason"]
                            for f in result["suspected_violations"]))

    def test_release_fence_refers_to_previous_frame_even_across_resize(self):
        events = [event(1, "android", "PRESENT_RX", slot=0, serial=8),
                  event(2, "android", "PRESENT_RX", slot=1, serial=9, generation=2),
                  event(3, "android", "PREVIOUS_RELEASE_FENCE_DONE", slot=1, serial=9, generation=2),
                  event(4, "android", "SLOT_AVAILABLE", slot=0, serial=8)]
        result, _ = analyze(events, 1000)
        self.assertEqual(result["coverage"]["previous_release_order_checked"], 1)
        self.assertEqual(result["suspected_violations"], [])
        events[-1] = event(2, "android", "SLOT_AVAILABLE", slot=0, serial=8)
        result, _ = analyze(sorted(events, key=lambda e: e.t_ns), 1000)
        self.assertTrue(any("before release handler" in f["reason"]
                            for f in result["suspected_violations"]))

    def test_ack_does_not_free_wayland_slot(self):
        events = [event(1, "wayland", "FREE_TX"),
                  event(2, "wayland", "PRESENT_RX", serial=0),
                  event(3, "wayland", "ACK_TX", serial=0),
                  event(4, "wayland", "PRESENT_RX", serial=1)]
        result, _ = analyze(events, 1000)
        self.assertTrue(any("without FREE" in f["reason"]
                            for f in result["suspected_violations"]))

    def test_slow_free_is_identified_by_slot_cycle(self):
        events = [event(1, "mesa", "SLOT_AVAILABLE"),
                  event(2, "mesa", "SLOT_ACQUIRED"),
                  event(3, "mesa", "SLOT_PRESENTED", serial=0),
                  event(4, "android", "SLOT_AVAILABLE", serial=0),
                  event(30_000_004, "mesa", "FREE_RX"),
                  event(30_000_005, "mesa", "SLOT_AVAILABLE"),
                  event(30_000_006, "mesa", "SLOT_ACQUIRED")]
        result, _ = analyze(events, 1_000_000_000)
        self.assertEqual(result["metrics"]["android_release_to_mesa_free"]["n"], 0)
        self.assertEqual(result["metrics"]["android_release_to_mesa_free_by_cycle"]["median_ms"], 30)
        self.assertEqual(result["suspected_violations"], [])


if __name__ == "__main__":
    unittest.main()
