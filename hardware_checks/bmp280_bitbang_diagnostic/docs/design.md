# BMP280 Bit-Banged I2C Diagnostic Design

## Scope

Create an isolated ESP-IDF 6.1 application for ESP32-S3. It diagnoses a BMP280 at address `0x76` using software I2C on GPIO4 (SDA) and GPIO6 (SCL), writes results through USB Serial JTAG, and is built but never flashed.

## Transport

Both bus pins use GPIO open-drain output mode with internal pull-ups. Driving a pin low asserts it; writing high releases it. A 30 us half-cycle targets about 16.7 kHz. Every SCL release waits for the sampled pin to become high, with a 2 ms timeout for bounded clock stretching. Transactions implement START, repeated START, STOP, byte transmit/receive, and ACK/NACK without any ESP-IDF I2C API.

Transport operations return explicit results for success, SCL timeout, bus busy, address NACK, and data NACK. STOP is attempted after transaction failures so later diagnostic operations can continue.

## Diagnostic Flow

Initialize USB Serial JTAG and GPIO, report configuration, and probe address `0x76`. Read chip ID (`0xD0`), status (`0xF3`), control (`0xF4`), config (`0xF5`), and the full 24-byte calibration block (`0x88`-`0x9F`). Issue soft reset (`0xE0=0xB6`), wait at least 20 ms, then repeat register inspection.

For five iterations separated by one second, write config `0x00` and control `0x21` (temperature x1, pressure skipped, forced mode). Capture status immediately and after 20 ms, then read raw `0xF7`-`0xFC` both as one burst and as six individual register transactions.

## Output And Verification

Emit concise newline-delimited JSON objects with elapsed milliseconds, event or phase, operation result, and hexadecimal byte strings. Build with ESP-IDF 6.1 for `esp32s3`; do not flash.
