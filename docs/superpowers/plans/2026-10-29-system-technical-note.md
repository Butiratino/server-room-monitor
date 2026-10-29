# Technical Note and Deployment Guide Plan

> **For agentic workers:** Execute inline or via a fresh subagent per task. Keep every documented claim traceable to source, tests, or an explicitly marked unverified item. Do not copy credentials or `.env` contents into documentation.

**Goal:** Produce a Russian technical note for the ESP32-S3 BMP280/Zabbix monitor and make the quick-start guide accurate and portable to another Windows PC.

**Architecture:** Put the complete operational reference in `docs/TECHNICAL_NOTE_RU.md`. Keep `START_HERE_RU.md` focused on the first build/flash/setup path and link to the reference. Document verified behavior separately from code-reviewed or not-yet-hardware-tested behavior.

**Tech Stack:** ESP-IDF 6.1.0, ESP32-S3, C, BMP280, FreeRTOS, NVS, Wi-Fi AP/STA, HTTP setup page, Zabbix Sender over TCP, Zabbix Docker Compose 7.4, Windows PowerShell.

**Spec:** `docs/superpowers/specs/2026-10-28-wifi-zabbix-firmware-design.md`; this plan implements its operational documentation requirements using the current source and test results.

## Global Constraints

- Production build selector remains `set(FIRMWARE "temperature_monitor.c")`.
- Target and verified toolchain remain `esp32s3` and ESP-IDF `6.1.0`.
- Do not read or copy `.env`, Docker secret files, Wi-Fi credentials, or AP passwords.
- Document the test-only NVS namespace `wifi_zx_test`; do not imply tests erase production configuration.
- Do not claim physical PRG/RST or Wi-Fi-loss testing unless performed; mark remaining scenarios plainly.
- Do not modify Windows Firewall, Docker bindings, Zabbix settings, or network profile as part of this documentation task.

---

### Task 1: Write the Technical Reference

**Files:**
- Create: `docs/TECHNICAL_NOTE_RU.md`
- Check: `main/temperature_monitor.c`, `main/wifi_manager.c`, `main/wifi_config.c`, `main/wifi_provisioning.c`, `main/zabbix_sender.c`, `main/wifi_zabbix_test.c`

- [x] Describe board/revision, wiring, build selector, component versions, production data flow, state transitions, sender cadence, and exact Zabbix host/item contract.
- [x] Describe startup, first provisioning, saved-config reboot, PRG reconfiguration, reconnect/backoff, sensor failure behavior, and the absence of a user-facing NVS/factory reset.
- [x] Add error reference tables for sensor `esp_err_t`, Wi-Fi state/reason names and numeric reason codes, provisioning HTTP/parser errors, Zabbix sender stages/errors, and fatal initialization checks; include safe operator actions.
- [x] Record verified tests/results and explicitly separate code-reviewed behavior from physical scenarios not yet tested.
- [x] Document complete new-PC deployment: ESP-IDF/toolchain, source/component setup, build/COM flash/monitor, official Zabbix Docker 7.4 setup, host/items, LAN IP, Windows profile/firewall and `Allowed hosts`; keep all credentials as user-supplied placeholders.
- [x] Document security/limits: setup AP is WPA2 but its local page is HTTP; NVS/flash encryption status; plaintext sender; no captive portal; no alert logic; no backfill.

### Task 2: Correct and Shorten the Quick Start

**Files:**
- Modify: `START_HERE_RU.md`

- [x] Correct the current project status and replace obsolete `OLED_*` `idf.py -D` mode commands with the actual `main/CMakeLists.txt` `FIRMWARE` selector.
- [x] Keep the minimal build/flash/monitor and first-time AP steps, link to `docs/TECHNICAL_NOTE_RU.md`, and remove stale claims that Wi-Fi/Zabbix are unimplemented.
- [x] Retain historical BMP280 diagnosis only where clearly labeled historical; do not duplicate the full error/deployment reference.

### Task 3: Validate Documentation Against the Project

**Files:**
- Check: `docs/TECHNICAL_NOTE_RU.md`, `START_HERE_RU.md`, `main/CMakeLists.txt`, scripts and source references.

- [x] Verify command names, source filenames, port/item names, Wi-Fi reasons and sender stage strings against current code.
- [x] Verify deployment guidance identifies machine-specific `D:\Espressif`/`COM4` paths as local examples and requires adapting them on another PC; use placeholders for server/device IPs and credentials.
- [x] Run `git diff --check` and scan the final documentation for `.env` contents, passwords, tokens, and obsolete firmware-selection commands.
- [x] Report physical reset/PRG, power-cycle and Wi-Fi recovery tests accurately; do not present unperformed manual checks as passed.
