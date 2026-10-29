"""Verify that every sensor report exposes raw temperature and pressure data."""

import json
import sys
import time
import unittest

import serial


class RawMeasurementHardwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports = []
        connection = serial.Serial(port=None, baudrate=115200, timeout=0.5)
        connection.port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
        connection.dtr = False
        connection.rts = False
        with connection:
            deadline = time.monotonic() + 6
            while time.monotonic() < deadline:
                line = connection.readline().decode("utf-8", errors="replace")
                try:
                    report = json.loads(line)
                except (json.JSONDecodeError, ValueError):
                    continue
                if isinstance(report, dict) and report.get("type") == "measurement":
                    cls.reports.append(report)
                    print(report, flush=True)

    def test_reports_raw_data_before_compensation(self):
        self.assertGreaterEqual(len(self.reports), 3, "Measurement firmware is not reporting")
        for report in self.reports:
            self.assertRegex(report["raw_bytes"], r"^[0-9A-F]{12}$")
            self.assertIsInstance(report["raw_temperature"], int)
            self.assertIsInstance(report["raw_pressure"], int)
            self.assertIsInstance(report["status"], int)

    def test_reports_compensated_fields_even_when_invalid(self):
        self.assertGreaterEqual(len(self.reports), 3, "Measurement firmware is not reporting")
        for report in self.reports:
            self.assertIn("temperature_c", report)
            self.assertIn("pressure_pa", report)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
