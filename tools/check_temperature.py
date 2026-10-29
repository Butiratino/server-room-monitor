"""Check real C firmware measurements against Bosch's fixed-point formula."""

import json
import math
import sys
import time
import unittest

import serial


def reference_temperature(raw, t1, t2, t3):
    var1 = (((raw >> 3) - (t1 << 1)) * t2) >> 11
    delta = (raw >> 4) - t1
    var2 = (((delta * delta) >> 12) * t3) >> 14
    return (((var1 + var2) * 5 + 128) >> 8) / 100.0


class BoschReferenceTests(unittest.TestCase):
    def test_datasheet_temperature_example(self):
        self.assertEqual(reference_temperature(519888, 27504, 26435, -1000), 25.08)


class TemperatureHardwareTests(unittest.TestCase):
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

    def test_repeated_valid_temperature(self):
        self.assertGreaterEqual(len(self.reports), 4, "Temperature firmware is not reporting")
        for report in self.reports:
            self.assertEqual(report.get("state"), "ok", report)
            self.assertEqual(report.get("error"), "ESP_OK")
            self.assertEqual(report.get("address"), 0x76)
            self.assertEqual(report.get("model"), "BMP280")
            temperature = report.get("temperature_c")
            self.assertIsInstance(temperature, (int, float))
            self.assertTrue(math.isfinite(temperature))
            self.assertGreaterEqual(temperature, -40)
            self.assertLessEqual(temperature, 85)

    def test_factory_calibration_is_applied(self):
        self.assertGreaterEqual(len(self.reports), 4, "No temperature samples to validate")
        for report in self.reports:
            self.assertEqual(report.get("state"), "ok", report)
            self.assertGreater(report["t1"], 0)
            self.assertLess(report["t1"], 65535)
            self.assertNotEqual(report["raw_temperature"], 0x80000)
            expected = reference_temperature(
                report["raw_temperature"], report["t1"], report["t2"], report["t3"]
            )
            self.assertAlmostEqual(report["temperature_c"], expected, delta=0.03)

    def test_measurements_are_updated_once_per_second(self):
        self.assertGreaterEqual(len(self.reports), 4, "Too few samples to check interval")
        timestamps = [report["uptime_ms"] for report in self.reports]
        for previous, current in zip(timestamps, timestamps[1:]):
            self.assertGreaterEqual(current - previous, 800)
            self.assertLessEqual(current - previous, 1200)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
