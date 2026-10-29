# BMP280 Bit-Banged I2C Diagnostic Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone ESP32-S3 BMP280 diagnostic using GPIO-only software I2C.

**Architecture:** A single application component owns the bit-banged open-drain transport and sequential diagnostic flow. ESP-IDF is used only for GPIO, timing, task delay, and USB Serial JTAG output.

**Tech Stack:** ESP-IDF 6.1, C, ESP32-S3, CMake/Ninja

**Spec:** `docs/design.md`

## Global Constraints

- Create files only under `D:/Oled1/hardware_checks/bmp280_bitbang_diagnostic`.
- Use GPIO4 for SDA and GPIO6 for SCL with open-drain mode and internal pull-ups.
- Do not include or call ESP-IDF I2C drivers.
- Use a conservative software bus rate near 10-20 kHz.
- Bound clock-stretch waits and check every ACK.
- Build for ESP32-S3 with ESP-IDF 6.1 and do not flash.

---

### Task 1: Standalone Project And Diagnostic

**Files:**
- Create: `CMakeLists.txt`
- Create: `sdkconfig.defaults`
- Create: `main/CMakeLists.txt`
- Create: `main/main.c`

**Interfaces:**
- Consumes: ESP-IDF GPIO, timer, FreeRTOS, and USB Serial JTAG APIs.
- Produces: `app_main(void)` and newline-delimited JSON serial diagnostics.

- [ ] **Step 1: Add project scaffolding configured for ESP32-S3 and USB Serial JTAG.**
- [ ] **Step 2: Implement GPIO open-drain START/STOP, byte read/write, ACK, and bounded clock-stretch behavior.**
- [ ] **Step 3: Implement BMP280 register transactions and the specified five-iteration diagnostic sequence.**
- [ ] **Step 4: Statistically verify that no I2C driver header or API appears in source.**
- [ ] **Step 5: Build with ESP-IDF 6.1 using `idf.py set-target esp32s3 build`.**
- [ ] **Step 6: Inspect the build result and report it without flashing.**
