"""Check production firmware diagnostics and one-second sensor cadence."""

import json
import sys
import time
import unittest

import serial


class ZabbixFirmwareHardwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports = []
        connection = serial.Serial(port=None, baudrate=115200, timeout=0.5)
        connection.port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
        connection.dtr = False
        connection.rts = False
        with connection:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                line = connection.readline().decode("utf-8", errors="replace")
                try:
                    report = json.loads(line)
                except (json.JSONDecodeError, ValueError):
                    continue
                if isinstance(report, dict) and report.get("type") == "measurement":
                    cls.reports.append(report)
                    print(report, flush=True)

    def test_measurement_reports_include_network_status_without_secrets(self):
        self.assertGreaterEqual(len(self.reports), 4, "Measurement firmware is not reporting")
        required_fields = {
            "wifi_state",
            "wifi_error",
            "wifi_reason_code",
            "zabbix_send_state",
            "zabbix_stage",
            "zabbix_error",
            "raw_bytes",
            "raw_temperature",
            "raw_pressure",
            "status",
        }
        for report in self.reports:
            self.assertTrue(required_fields.issubset(report), report)
            self.assertIsInstance(report["wifi_reason_code"], int)
            self.assertIsInstance(report["zabbix_stage"], str)
            if report["zabbix_send_state"] == "error":
                self.assertNotEqual(report["zabbix_stage"], "none", report)
            self.assertNotIn("password", report)
            self.assertNotIn("ssid", report)

    def test_sensor_reports_keep_one_second_cadence(self):
        self.assertGreaterEqual(len(self.reports), 4, "Too few samples to check interval")
        timestamps = [report["uptime_ms"] for report in self.reports]
        for previous, current in zip(timestamps, timestamps[1:]):
            self.assertGreaterEqual(current - previous, 800)
            self.assertLessEqual(current - previous, 1200)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
