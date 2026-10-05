"""Regression contracts for the actual recovery/sender integration.

These inspect firmware source and demonstrate the queued-event counterexample;
they do not execute FreeRTOS or simulate the USB controller.
"""
from pathlib import Path
import re
import unittest


class TestRecoveryGate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (Path(__file__).resolve().parents[2] /
                  "src/usb/usb_composite.cpp").read_text(encoding="utf-8")
        cls.code = re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S)
        cls.recovery = cls.code.split("static bool usb_recovery_tick(", 1)[1].split(
            "static bool hid_flush_keyboard_locked(", 1)[0]

    def test_recovery_closes_gate_before_release_and_detach(self):
        gate = self.recovery.index("s_hid_recovering = true;")
        prefix = self.recovery[:gate]
        locked = prefix[prefix.rfind("hid_lock();"):]
        # The only unlock before the gate is the resolved-HID early return.
        # Pending recheck and closing the gate must otherwise share one lock.
        self.assertRegex(locked, r"if\s*\(reason == USB_RECOVERY_HID && !s_keyboard_pending && !s_consumer_pending\)\s*\{\s*hid_unlock\(\);\s*return false;\s*\}\s*$")
        self.assertEqual(locked.count("hid_unlock();"), 1)
        self.assertLess(gate, self.recovery.index("usb_composite_force_release_all("))
        self.assertLess(gate, self.recovery.index("tud_disconnect();"))

    def test_all_sender_paths_observe_gate_under_their_existing_lock(self):
        for start, end in (
            ("static bool hid_flush_keyboard_locked(", "static bool hid_flush_consumer_locked("),
            ("static bool hid_flush_consumer_locked(", "static void hid_clear_locked("),
        ):
            body = self.code.split(start, 1)[1].split(end, 1)[0]
            self.assertLess(body.index("s_hid_recovering"), body.index(".SendReport("))
            self.assertRegex(body, r"if\s*\(s_hid_recovering.*?return false;")
        stress = self.code.split("static void hid_stress_task(", 1)[1].split(
            "bool usb_hid_stress_start(", 1)[0]
        self.assertLess(stress.index("hid_lock();"), stress.index("s_hid_recovering"))
        self.assertLess(stress.index("s_hid_recovering"), stress.index(".SendReport("))

    def test_reconnect_samples_mount_sequence_before_attach(self):
        baseline = self.recovery.index("s_reconnect_mount_seq = __atomic_load_n(")
        self.assertLess(baseline, self.recovery.index("tud_connect();"))
        reopen = self.recovery.index("s_hid_recovering = false;")
        condition = self.recovery[self.recovery.index("const bool remounted"):reopen]
        self.assertIn("tud_mounted()", condition)
        self.assertIn("!= s_reconnect_mount_seq", condition)
        self.assertIn("if (remounted)", condition)

    def test_asynchronous_started_event_cannot_reopen_transport(self):
        event = self.code.split("id == ARDUINO_USB_STARTED_EVENT", 1)[1].split(
            "id == ARDUINO_USB_STOPPED_EVENT", 1)[0]
        self.assertNotIn("s_hid_recovering = false", event)
        self.assertNotIn("s_waiting_reconnect_ms = 0", event)
        hook = self.code.split('extern "C" void remotemapper_usb_lifecycle(', 1)[1].split(
            "static uint32_t s_usb_recovery_request", 1)[0]
        self.assertIn("__atomic_add_fetch", hook)
        self.assertNotIn("hid_lock(", hook)  # USB task must never wait for its sender

    def test_wakeup_uses_coordinator_without_gpio_override_or_waits(self):
        wake = self.code.split("if (now - s_hw_sleep_start_ms >= 2000)", 1)[1].split(
            "bool usb_hid_keyboard_press(", 1)[0]
        self.assertIn("usb_composite_request_recovery(USB_RECOVERY_WAKE)", wake)
        for call in ("pinMode(", "digitalWrite(", "vTaskDelay(", "tud_disconnect("):
            self.assertNotIn(call, wake)

    def test_old_started_event_is_not_a_new_mount(self):
        # Old implementation allowed an asynchronous Started reply to erase the
        # reconnect deadline. It can be queued before recovery and delivered
        # after detach. A synchronous mount sequence does not change on delivery.
        sequence = 4
        baseline = sequence  # sampled just before connect
        stale_started_delivered = True
        self.assertTrue(stale_started_delivered)  # would satisfy the old handler
        self.assertFalse(sequence != baseline)
        sequence += 1  # a new TinyUSB mount, rather than a queued Arduino event
        self.assertTrue(sequence != baseline)


if __name__ == "__main__":
    unittest.main()
