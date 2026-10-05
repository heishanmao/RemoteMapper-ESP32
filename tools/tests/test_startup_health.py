from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class StartupHealthRegressionTests(unittest.TestCase):
    def test_audio_pipeline_has_one_retry_guarded_owner(self):
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        uac = (ROOT / "src/usb/uac_microphone.cpp").read_text(encoding="utf-8")

        self.assertNotIn("audio_pipeline_init(", main)
        self.assertEqual(uac.count("audio_pipeline_init(&g_audio_pipeline)"), 1)
        self.assertIn("if (!s_audio_pipeline_initialized)", uac)

    def test_failed_uac_worker_creation_does_not_report_ready(self):
        uac = (ROOT / "src/usb/uac_microphone.cpp").read_text(encoding="utf-8")

        failure = uac.index('app_log("UAC", "Failed to spawn TX pump task")')
        ready_assignment = uac.index("s_uac_initialized = true;", failure)
        self.assertIn("return false;", uac[failure:ready_assignment])
        self.assertIn("return s_uac_initialized && s_uac_interface_enabled && s_uac_push_task != nullptr;", uac)

    def test_ota_confirmation_requires_critical_startup_health(self):
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")

        self.assertIn("ble_task_created == pdPASS && usb_composite_is_initialized()", main)
        self.assertIn("if (s_critical_startup_ok && usb_composite_is_initialized() && uac_microphone_is_ready())", main)
        self.assertIn("ota_manager_watchdog_confirm();", main)


if __name__ == "__main__":
    unittest.main()
