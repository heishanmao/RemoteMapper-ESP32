"""Integration contracts for the sender queue and USB recovery gate."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TestRecoveryGate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "src/usb/usb_composite.cpp").read_text(encoding="utf-8")
        cls.code = re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S)
        cls.recovery = cls.code.split("static bool usb_recovery_tick(", 1)[1].split(
            "static void hid_clear_locked(", 1)[0]
        cls.queue = (ROOT / "src/usb/hid_command_queue.h").read_text(encoding="utf-8")

    def test_recovery_invalidates_epoch_and_closes_sender_before_detach(self):
        begin = self.recovery.index("s_hid_commands.begin_recovery();")
        force = self.recovery.index("usb_composite_force_release_all(", begin)
        pause = self.recovery.index("s_hid_commands.pause_for_detach();", force)
        detach = self.recovery.index("tud_disconnect();", pause)
        self.assertLess(begin, force)
        self.assertLess(force, pause)
        self.assertLess(pause, detach)
        pause_helper = re.search(
            r"static void hid_sender_pause_for_detach\(void\)\s*\{([^}]*)\}", self.code
        ).group(1)
        self.assertLess(pause_helper.index("hid_lock();"), pause_helper.index("pause_for_detach()"))

    def test_sender_checks_epoch_and_keeps_completion_guard_timeout(self):
        sender = self.code.split("static bool hid_send_command_report(", 1)[1].split(
            "static void hid_sender_task(", 1)[0]
        self.assertIn("s_hid_commands.begin_send(command)", sender)
        self.assertIn("hid_lock();", sender)
        self.assertIn("SendReport(HID_REPORT_ID_KEYBOARD, &report, sizeof(report), 20)", sender)
        self.assertIn("SendReport(HID_REPORT_ID_CONSUMER_CONTROL, &report, sizeof(report), 20)", sender)
        self.assertIn("is_current(command)", self.code.split("static bool hid_command_current(", 1)[1].split("}", 1)[0])
        self.assertIn("m_recovering && !is_emergency(command.kind)", self.queue)

    def test_ble_producers_only_take_short_queue_lock_and_notify(self):
        for name, next_name in (
            ("bool usb_hid_keyboard_press(", "bool usb_hid_keyboard_release("),
            ("bool usb_hid_keyboard_release(", "bool usb_hid_keyboard_tap("),
            ("bool usb_hid_keyboard_tap(", "bool usb_hid_consumer_press("),
            ("bool usb_hid_consumer_press(", "bool usb_hid_consumer_release("),
            ("bool usb_hid_consumer_release(", "bool usb_hid_consumer_tap("),
        ):
            body = self.code.split(name, 1)[1].split(next_name, 1)[0]
            self.assertIn("s_hid_command_mux", body)
            self.assertIn("hid_command_notify()", body)
            self.assertNotIn("hid_lock()", body)
            self.assertNotIn("SendReport", body)
            self.assertNotIn("delay(", body)
        tap = self.code.split("bool usb_hid_keyboard_tap(", 1)[1].split(
            "bool usb_hid_consumer_press(", 1)[0]
        self.assertIn("enqueue_keyboard(modifier, keycode, true)", tap)
        self.assertNotIn("delay(", tap)

    def test_worker_owns_tap_dwell_and_stress_observes_recovery(self):
        worker = self.code.split("static void hid_sender_task(", 1)[1].split(
            "bool usb_hid_stress_start(", 1)[0]
        self.assertIn("pdMS_TO_TICKS(15)", worker)
        self.assertIn("hid_command_current(command)", worker)
        stress = self.code.split("static void hid_stress_task(", 1)[1].split(
            "bool usb_hid_stress_start(", 1)[0]
        self.assertLess(stress.index("s_hid_recovering"), stress.index(".SendReport("))

    def test_reconnect_samples_mount_sequence_before_attach(self):
        baseline = self.recovery.index("s_reconnect_mount_seq = __atomic_load_n(")
        self.assertLess(baseline, self.recovery.index("tud_connect();"))
        reopen = self.recovery.index("s_hid_recovering = false;")
        condition = self.recovery[self.recovery.index("const bool remounted"):reopen]
        self.assertIn("tud_mounted()", condition)
        self.assertIn("!= s_reconnect_mount_seq", condition)

    def test_asynchronous_started_event_cannot_reopen_transport(self):
        event = self.code.split("id == ARDUINO_USB_STARTED_EVENT", 1)[1].split(
            "id == ARDUINO_USB_STOPPED_EVENT", 1)[0]
        self.assertNotIn("s_hid_recovering = false", event)
        self.assertNotIn("end_recovery()", event)
        hook = self.code.split('extern "C" void remotemapper_usb_lifecycle(', 1)[1].split(
            "static uint32_t s_usb_recovery_request", 1)[0]
        self.assertIn("__atomic_add_fetch", hook)
        self.assertNotIn("hid_lock(", hook)

    def test_wakeup_uses_coordinator_and_sender_wakes_before_ready_check(self):
        wake = self.code.split("if (now - s_hw_sleep_start_ms >= 2000)", 1)[1].split(
            "bool usb_hid_keyboard_press(", 1)[0]
        self.assertIn("usb_composite_request_recovery(USB_RECOVERY_WAKE)", wake)
        for call in ("pinMode(", "digitalWrite(", "vTaskDelay(", "tud_disconnect("):
            self.assertNotIn(call, wake)
        sender = self.code.split("static bool hid_send_command_report(", 1)[1].split(
            "static void hid_sender_task(", 1)[0]
        self.assertLess(sender.index("tud_remote_wakeup();"), sender.index("if (!tud_ready())"))

    def test_voice_guard_clamps_nonzero_values_to_one_second_minimum(self):
        load = self.code.split("static void guard_load_config(", 1)[1].split(
            "static void guard_add(", 1)[0]
        setter = self.code.split("bool usb_composite_guard_set(", 1)[1].split(
            "void usb_composite_force_release_all(", 1)[0]
        self.assertIn("stored_voice_ms < 1000 ? 1000 : stored_voice_ms", load)
        self.assertIn("cfg->voice_ms < 1000 ? 1000 : cfg->voice_ms", setter)
        for body in (load, setter):
            self.assertIn("HID_GUARD_VOICE_EXTREME_MS", body)

    def test_voice_drain_rechecks_epoch_under_lock_before_stopping_session(self):
        drain = self.code.split("static bool finish_voice_drain(", 1)[1].split(
            "void usb_composite_init(", 1)[0]
        self.assertLess(drain.index("portENTER_CRITICAL(&s_voice_state_mux)"),
                        drain.index("s_voice_diag_seq != expected_sequence"))
        self.assertIn("!s_voice_drain_pending || s_voice_diag_seq != expected_sequence", drain)
        self.assertLess(drain.index("if (!due)"), drain.index("audio_pipeline_stop_session(&g_audio_pipeline)"))
        stop = drain.index("audio_pipeline_stop_session(&g_audio_pipeline)")
        self.assertLess(stop, drain.index("portEXIT_CRITICAL(&s_voice_state_mux)", stop))

        actions = self.code.split("case ACTION_VOICE_HOLD:", 1)[1].split(
            "case ACTION_VOICE_RELEASE:", 1)[0]
        hold_lock = actions.index("portENTER_CRITICAL(&s_voice_state_mux)")
        sequence = actions.index("++s_voice_diag_seq", hold_lock)
        start = actions.index("audio_pipeline_start_session(&g_audio_pipeline, 0)", sequence)
        unlock = actions.index("portEXIT_CRITICAL(&s_voice_state_mux)", start)
        self.assertLess(hold_lock, sequence)
        self.assertLess(sequence, start)
        self.assertLess(start, unlock)
        critical = actions[hold_lock:unlock]
        for forbidden in ("app_log(", "SendReport(", "key_engine_"):
            self.assertNotIn(forbidden, critical)


if __name__ == "__main__":
    unittest.main()
