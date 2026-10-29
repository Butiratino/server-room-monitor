# ESP32 Wi-Fi Provisioning and Zabbix Reporting Implementation Plan

> **For agentic workers:** Execute inline, task-by-task, with test-first changes and verification checkpoints. Do not delegate to subagents unless the user asks.

**Goal:** Provision the Heltec ESP32-S3 through a protected setup page and send BMP280 readings directly to the existing Zabbix host.

**Architecture:** Keep the current one-second sensor/OLED/USB loop responsive. A Wi-Fi manager reads settings from NVS, joins the configured network, and provides an AP/HTTP setup page on first boot or after a five-second PRG hold. A separate sender task transmits the latest sample to Zabbix every ten seconds using its trapper sender protocol.

**Tech Stack:** ESP-IDF 6.1, ESP32-S3, C, FreeRTOS, `esp_wifi`, `esp_netif`, `esp_event`, `nvs_flash`, `esp_http_server`, lwIP sockets, Zabbix 7.4.

**Spec:** `docs/superpowers/specs/2026-10-28-wifi-zabbix-firmware-design.md`

## Global Constraints

- Target `esp32s3`; board Heltec HTIT-WB32LAF V4.3.
- Sensor I2C: SDA GPIO4, SCL GPIO6, address 0x76. OLED I2C: SDA GPIO17, SCL GPIO18.
- Preserve `set(FIRMWARE "temperature_monitor.c")` as the normal build choice.
- Never block the one-second measurement/display loop on Wi-Fi or socket operations.
- Store Wi-Fi credentials and Zabbix address in NVS, never in source, Git, or periodic JSON diagnostics.
- Normal RST/power cycle preserves configuration. PRG enters setup only after normal firmware startup; do not hold GPIO0/PRG while resetting.
- Zabbix host is exactly `server-room-esp32`; items are `sensor.temperature`, `sensor.pressure_pa`, and `sensor.status`.
- Send every 10 seconds. Do not queue/replay stale readings; on reconnect send the latest snapshot.
- Invalid sensor readings send `sensor.status=error` only, never fabricated numeric temperature/pressure.
- First version uses plaintext sender protocol only on the trusted home LAN; TLS PSK and automatic server discovery are out of scope.

## File Responsibilities

- `main/wifi_config.h/.c`: bounded configuration, validation, and NVS persistence.
- `main/wifi_provisioning.h/.c`: temporary SoftAP, setup password, HTTP page, and URL-encoded form parser.
- `main/wifi_manager.h/.c`: Wi-Fi states/events, reconnect policy, PRG long-press, and latest-snapshot queue.
- `main/zabbix_sender.h/.c`: sender JSON, Zabbix frame encoding, TCP connection, and response parsing.
- `main/temperature_monitor.c`: existing sensor/OLED/USB loop plus handoff to Wi-Fi/sender modules; no network socket operations.
- `main/wifi_zabbix_test.c`: Unity runner for deterministic validation, state, parser, and protocol-frame tests.
- `tools/check_zabbix_firmware.py`: serial assertions for Wi-Fi/sender status and continued sensor cadence; no credentials.
- `START_HERE_RU.md`: setup, recovery, and network/test instructions.

The Unity test runner is enabled in the existing `sdkconfig`. Extend the current
`FIRMWARE` selector with a test-only branch; keep `temperature_monitor.c` as the
default and compile the same production modules into the test branch.

## File Responsibilities

- `main/wifi_config.h/.c`: bounded settings structure, input validation, and NVS persistence.
- `main/wifi_provisioning.h/.c`: temporary SoftAP, setup password, HTTP page, and URL-encoded form parser.
- `main/wifi_manager.h/.c`: Wi-Fi states/events, reconnect policy, PRG long-press, and one-slot latest-snapshot queue.
- `main/zabbix_sender.h/.c`: sender JSON, Zabbix frame encoding, TCP connection, and response parsing.
- `main/temperature_monitor.c`: existing sensor/OLED/USB loop plus handoff to Wi-Fi/sender modules; contains no network protocol implementation.
- `main/wifi_zabbix_test.c`: ESP-IDF Unity runner for deterministic config, form, state, and framing tests.
- `tools/check_zabbix_firmware.py`: validates periodic serial JSON/status and cadence without credentials.
- `START_HERE_RU.md`: portal instructions, PRG behavior, network requirements, and Zabbix verification steps.

The normal CMake choice remains `temperature_monitor.c`. Add an explicit
`wifi_zabbix_test.c` branch that compiles the Unity runner and production modules
for tests; the test choice must never become the default firmware.

---

### Task 1: NVS Configuration Model and Test Harness

**Files:**
- Create: `main/wifi_config.h`
- Create: `main/wifi_config.c`
- Create: `main/wifi_zabbix_test.c` (test-only application entry point)
- Modify: `main/CMakeLists.txt` to support the test-only firmware choice while preserving `temperature_monitor.c` as the default choice.

**Interfaces:**
- `room_monitor_config_t`: `ssid[33]`, `password[65]`, `zabbix_server[254]`, `zabbix_port`.
- `esp_err_t wifi_config_validate(const room_monitor_config_t *config)` validates bounded input and port 1–65535.
- `esp_err_t wifi_config_load(room_monitor_config_t *config)` returns `ESP_ERR_NVS_NOT_FOUND` if not provisioned.
- `esp_err_t wifi_config_save(const room_monitor_config_t *config)` validates before writing all fields to one NVS namespace.
- `esp_err_t wifi_config_clear(void)` erases only this namespace.
- Test-only CMake selection `FIRMWARE="wifi_zabbix_test.c"` links `wifi_zabbix_test.c` and `wifi_config.c` with ESP-IDF Unity.

- [x] **Step 1: Add failing Unity cases** named `config_accepts_valid_fields`, `config_rejects_empty_ssid`, `config_rejects_empty_server`, `config_rejects_invalid_port`, `config_missing_returns_not_found`, and `config_save_load_round_trip`.
- [x] **Step 2: Add the CMake test branch** for `wifi_zabbix_test.c`; keep the current `temperature_monitor.c` default unchanged.
- [x] **Step 3: Build and run the test firmware**; confirm the new cases fail before implementation.
- [x] **Step 4: Implement the config struct, validation, and NVS functions**; do not log or return credential values in error messages.
- [x] **Step 5: Re-run the same Unity tests** and verify all tests pass, including persistence after NVS reinitialization.
- [x] **Step 6: Restore the regular monitor selection** and run `idf.py build`.

**Test procedure:** Set `FIRMWARE` in `main/CMakeLists.txt` to `wifi_zabbix_test.c`, build, flash COM4, and run the Unity menu in `idf.py monitor`. Exit with `Ctrl+]`; restore `temperature_monitor.c` and rebuild before further hardware testing. The test branch must link `wifi_zabbix_test.c`, `wifi_config.c`, `wifi_provisioning.c`, `wifi_manager.c`, and `zabbix_sender.c` as they are added.

---

### Task 2: Protected AP and HTTP Setup Page

**Files:**
- Create: `main/wifi_provisioning.h`
- Create: `main/wifi_provisioning.c`
- Modify: `main/temperature_monitor.c` to enter/exit setup mode and show provisioning status.
- Modify: `main/CMakeLists.txt` with required ESP-IDF component dependencies.

**Interfaces:**
- `esp_err_t wifi_provisioning_start(void)` starts a temporary WPA2 AP and HTTP server.
- `esp_err_t wifi_provisioning_stop(void)` stops HTTP/AP resources cleanly.
- `esp_err_t wifi_provisioning_parse_form(const char *body, size_t body_length, room_monitor_config_t *out)` URL-decodes bounded fields, validates them, and never logs field contents.
- `wifi_provisioning_start` reports submitted settings through the NVS configuration API; it never returns raw credentials through logs or diagnostic JSON.

- [x] **Step 1: Add failing Unity cases** named `form_decodes_url_encoded_ssid`, `form_rejects_empty_ssid`, `form_rejects_empty_server`, `form_defaults_port_to_10051`, `form_rejects_port_out_of_range`, and `form_error_does_not_echo_password`.
- [x] **Step 2: Run the test firmware and verify these parser cases fail before implementation.**
- [x] **Step 3: Generate a device-specific SSID** (`ServerRoom-` plus a short MAC suffix) and a fresh random WPA2 setup password; display the setup credentials once on OLED/USB.
- [x] **Step 4: Serve a local form at `http://192.168.4.1`** containing Wi-Fi SSID/password and Zabbix IPv4/hostname/port.
- [x] **Step 5: Save only validated form values** through `wifi_config_save`; keep AP/page running until Wi-Fi station association succeeds, and present a safe correction message on failure.
- [x] **Step 6: Manually test with a phone/laptop**: join the protected AP, load the form, enter test settings, submit, verify the page reports success/failure without echoing the Wi-Fi password.

---

### Task 3: Wi-Fi State Manager, Reconnect, and PRG Entry

**Files:**
- Create: `main/wifi_manager.h`
- Create: `main/wifi_manager.c`
- Modify: `main/temperature_monitor.c` to initialize NVS, start Wi-Fi management, and publish sample/state snapshots.

**Interfaces:**
- States: `WIFI_STATE_UNCONFIGURED`, `WIFI_STATE_PROVISIONING`, `WIFI_STATE_CONNECTING`, `WIFI_STATE_ONLINE`, `WIFI_STATE_RETRY_WAIT`.
- Events: `WIFI_EVENT_CONFIG_MISSING`, `WIFI_EVENT_CONNECT_STARTED`, `WIFI_EVENT_CONNECTED`, `WIFI_EVENT_DISCONNECTED`, `WIFI_EVENT_SETUP_REQUESTED`.
- `esp_err_t wifi_manager_start(void)` loads config and starts provisioning or station mode.
- `wifi_manager_state_t wifi_manager_get_state(void)` returns current state.
- `wifi_manager_state_t wifi_manager_transition(wifi_manager_state_t state, wifi_event_t event)` is side-effect-free and determines state transitions.
- `void wifi_manager_request_setup(void)` enters provisioning on a deliberate PRG long-press.
- `void wifi_manager_publish_snapshot(const bmp280_temperature_t *sample, esp_err_t sensor_error)` publishes only the newest result to the sender task.

- [x] **Step 1: Add failing Unity cases** named `wifi_missing_config_enters_provisioning`, `wifi_connected_event_enters_online`, `wifi_disconnect_enters_retry`, and `wifi_setup_request_enters_provisioning`.
- [x] **Step 2: Run the test firmware and verify these transition cases fail before implementation.**
- [x] **Step 3: Implement ESP-IDF station setup and event handlers**; use bounded exponential reconnect backoff and leave sensor/display cadence independent.
- [x] **Step 4: Configure GPIO0/PRG as an input only after normal app startup**; debounce the active-low button and trigger setup only after five seconds held. Normal reset must not erase NVS.
- [x] **Step 5: Implement first-boot AP and reconfiguration flow**; after valid station connection stop the temporary AP, and on failed saved-network connection allow PRG to re-enter setup.
- [ ] **Step 6: Flash and verify**: OLED/USB reports continue each second while disconnected; connect to a reachable Wi-Fi network, then interrupt/restore it and confirm automatic reconnect.

---

### Task 4: Zabbix Sender Packet Builder and TCP Transport

**Files:**
- Create: `main/zabbix_sender.h`
- Create: `main/zabbix_sender.c`
- Modify: `main/wifi_manager.h`/`.c` only for the sender task's snapshot queue.
- Modify: `main/CMakeLists.txt` with lwIP/socket dependencies as required by ESP-IDF 6.1.

**Interfaces:**
- `esp_err_t zabbix_sender_build_payload(const bmp280_temperature_t *sample, esp_err_t sensor_error, char *json, size_t capacity, size_t *length)` builds sender JSON for fixed host `server-room-esp32` and the exact item keys.
- `esp_err_t zabbix_sender_build_frame(const char *json, size_t length, uint8_t *frame, size_t capacity, size_t *frame_length)` writes `ZBXD\x01`, little-endian 64-bit payload length, then JSON bytes.
- `esp_err_t zabbix_sender_send(const room_monitor_config_t *config, const char *json, size_t length)` uses TCP with finite connect/read/write timeouts and validates the Zabbix sender response.

- [x] **Step 1: Add failing Unity cases** named `sender_frame_has_zbxd_signature`, `sender_frame_encodes_little_endian_length`, `sender_frame_rejects_short_buffer`, `sender_valid_batch_has_three_keys`, and `sender_error_batch_has_status_only`; add local TCP peer cases for fragmented success, malformed response, and response timeout.
- [x] **Step 2: Run the test firmware and verify frame/payload cases fail before implementation.**
- [x] **Step 3: Implement payload formatting and frame encoding** with bounds checks; key spelling and host identity must match the pre-created Zabbix objects exactly.
- [x] **Step 4: Implement socket send and response validation**; resolve IPv4/hostname, set finite socket timeouts, close sockets on every success/error path, and never block sensor tasks.
- [x] **Step 5: Run sender protocol tests.** Unity's test-only TCP peer verifies fragmented success, malformed response, and response timeout. The configured Zabbix endpoint still times out during TCP connect, so no synthetic value was sent to production Latest data; end-to-end server validation remains pending.

---

### Task 5: Periodic Send Integration and End-to-End Verification

**Files:**
- Modify: `main/temperature_monitor.c`.
- Create: `tools/check_zabbix_firmware.py` for serial-side Wi-Fi/sender status assertions, without storing Zabbix credentials.
- Modify: `START_HERE_RU.md` with portal setup, recovery, server-address entry, and runtime status instructions.

**Interfaces:**
- Sensor/display samples continue each second.
- Sender transmits immediately after first online snapshot and then every ten seconds.
- On network recovery, send latest current measurement/status; discard stale backlog.
- Periodic USB JSON includes Wi-Fi failure reason, sender failure stage/error code, and status; no Wi-Fi passwords or AP password.

- [x] **Step 1: Add a failing serial hardware test** requiring the new network/sender status fields while preserving raw BMP280 fields and current JSON cadence.
- [x] **Step 2: Run the test against current firmware** and confirm it fails because network fields are absent.
- [x] **Step 3: Integrate Wi-Fi state and sender snapshots** without making network calls from the display/sample loop.
- [x] **Step 4: Run the serial test** and verify measurements continue once per second, status changes to online, and sender errors are surfaced without crashing. Current result: Wi-Fi is online; `zabbix_stage=connect`, `zabbix_error=ESP_ERR_TIMEOUT`.
- [x] **Step 5: Provision through the phone/browser page** using Zabbix server address `192.168.0.95` and port `10051`; verify the device joins the home Wi-Fi.
- [x] **Step 6: Check TCP 10051 reachability** from the board's Wi-Fi network. TCP connect times out. Windows Firewall is enabled and the network profile is Public; no firewall or Docker exposure changes were made. Restrict the eventual allow rule to the reserved ESP32 address after confirming the cause.
- [ ] **Step 7: Check Zabbix Latest data** for `sensor.temperature`, `sensor.pressure_pa`, and `sensor.status` on `server-room-esp32`, with approximately ten-second updates.
- [ ] **Step 8: Test recovery** by temporarily disconnecting Wi-Fi; verify OLED/USB measurement continues, then reconnect and confirm only current values resume.
- [x] **Step 9: Update the user guide** with the PRG behavior, portal address, network requirements, and commands to build/flash/monitor. Do not include credentials.

## Build and Hardware Verification Commands

```powershell
. D:\Espressif\Activate-Oled1.ps1
idf.py build
idf.py -p COM4 flash
idf.py -p COM4 monitor
```

Confirm COM4 before flashing; monitor exits with `Ctrl+]`. The official Compose
stack is in `D:\Zabbix`; Zabbix host/item setup already exists and should not be
recreated by firmware tests.
