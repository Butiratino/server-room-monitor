"""Hardware checks for the C diagnostic firmware; run with the ESP-IDF Python."""

import json
import sys
import time
import unittest

import serial


class SensorProbeHardwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports = []
        port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
        connection = serial.Serial(port=None, baudrate=115200, timeout=0.5)
        connection.port = port
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
                if isinstance(report, dict) and report.get("type") == "sensor_probe":
                    cls.reports.append(report)
                    print(report, flush=True)

    def test_reports_both_sensor_addresses(self):
        addresses = {report.get("address") for report in self.reports}
        self.assertEqual(addresses, {0x76, 0x77}, "No complete sensor diagnostic received")
        for address in (0x76, 0x77):
            count = sum(report.get("address") == address for report in self.reports)
            self.assertGreaterEqual(count, 3, "Diagnostic stopped or reports are missing")

    def test_identifies_a_supported_connected_sensor(self):
        found = [report for report in self.reports if report.get("state") == "found"]
        self.assertTrue(found, "No BMP280/BME280 detected; inspect diagnostic reports and wiring")
        for report in found:
            self.assertIn(report.get("chip_id"), (0x58, 0x60))
            self.assertEqual(report.get("model"), {0x58: "BMP280", 0x60: "BME280"}[report["chip_id"]])
            self.assertEqual(report.get("error"), "ESP_OK")
        for address in {report["address"] for report in found}:
            samples = [report for report in self.reports if report.get("address") == address]
            self.assertGreaterEqual(len(samples), 3, "Too few sensor samples")
            self.assertTrue(all(report.get("state") == "found" for report in samples),
                            "Sensor detection was intermittent")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
