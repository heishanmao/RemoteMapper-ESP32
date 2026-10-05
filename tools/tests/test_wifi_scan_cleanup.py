"""Source-contract checks for stopping timed-out Arduino/ESP-IDF Wi-Fi scans."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/wifi/wifi_manager.cpp").read_text(encoding="utf-8")


def function_body(name, next_name):
    start = SOURCE.index(f"static void {name}(void)")
    end = SOURCE.index(f"static void {next_name}(void)", start + 1)
    return SOURCE[start:end]


class WifiScanCleanupTests(unittest.TestCase):
    def test_scan_is_async_and_manager_deadline_is_bounded(self):
        self.assertIn("WIFI_SCAN_DEADLINE_MS = 15000", SOURCE)
        self.assertIn("WiFi.scanNetworks(true)", SOURCE)
        self.assertIn("s_scan_deadline_ms = millis() + WIFI_SCAN_DEADLINE_MS", SOURCE)
        self.assertNotIn("WIFI_SCAN_MAX_MS_PER_CHANNEL", SOURCE)

    def test_failure_and_deadline_stop_driver_scan_before_deleting_results(self):
        cleanup_start = SOURCE.index("static void wifi_scan_stop_and_delete(void)")
        tick_start = SOURCE.index("static void wifi_scan_tick(void)", cleanup_start)
        tick_end = SOURCE.index("String wifi_manager_scan_status_json", tick_start)
        cleanup = SOURCE[cleanup_start:tick_start]
        tick = SOURCE[tick_start:tick_end]

        self.assertIn("esp_wifi_scan_stop()", cleanup)
        self.assertIn("ESP_ERR_WIFI_NOT_STARTED", cleanup)
        self.assertIn("ESP_ERR_WIFI_NOT_INIT", cleanup)
        self.assertLess(cleanup.index("esp_wifi_scan_stop()"), cleanup.index("WiFi.scanDelete()"))
        self.assertNotIn("WiFi.disconnect", cleanup)
        self.assertNotIn("WiFi.mode", cleanup)
        self.assertIn("if (s_scan_state == WIFI_SCAN_RUNNING_STATE) wifi_scan_stop_and_delete();", tick)
        self.assertIn("if (result_count < 0)", tick)
        self.assertIn("wifi_scan_stop_and_delete();", tick)

    def test_scan_driver_calls_remain_in_owner_task(self):
        self.assertEqual(SOURCE.count("wifi_scan_tick();"), 1)
        self.assertIn("wifi_scan_tick();\n\n    // ON_DEMAND", SOURCE)


if __name__ == "__main__":
    unittest.main()
