# Wi-Fi Provisioning and Zabbix Reporting for the ESP32 Monitor

## Status

Implemented. This file records the original design requirements; current implementation details, operational procedures, error reference, and verified/unverified test status are in `docs/TECHNICAL_NOTE_RU.md`.

## Context

The project runs on a Heltec HTIT-WB32LAF V4.3 (ESP32-S3) with a BMP280 sensor
and an onboard OLED. The current monitor measures temperature and pressure
every second, displays them on the OLED, and emits JSON diagnostics over USB
Serial/JTAG. The working sensor recently reported approximately 24.8 °C and
102.6 kPa.

An official Zabbix 7.4 Docker Compose installation is running on the Windows PC.
Its Zabbix Server listens on TCP 10051. The Zabbix host `server-room-esp32` is in
the `IoT devices` group and has these enabled trapper items:

| Name | Key | Zabbix type | Value type | Unit |
| --- | --- | --- | --- | --- |
| Temperature | `sensor.temperature` | Zabbix trapper | Numeric float | C |
| Air pressure | `sensor.pressure_pa` | Zabbix trapper | Numeric float | Pa |
| Sensor status | `sensor.status` | Zabbix trapper | Character | — |

The items exist but the ESP32 firmware does not yet send values to them.

## Goals

- Let an operator provision or change Wi-Fi and Zabbix server address without
  rebuilding firmware.
- Preserve saved settings across normal resets and power loss.
- Keep sensor measurement, OLED updates, and USB diagnostics responsive while
  Wi-Fi or Zabbix is unavailable.
- Send current values directly from the ESP32 to the configured Zabbix trapper
  items.
- Report invalid sensor state without sending invalid numeric measurements.

## Non-Goals for the First Version

- Automatic discovery of Zabbix servers on the LAN. The Zabbix Server does not
  advertise itself through a standard mechanism that this ESP32 can simply
  query. The setup page will accept an editable IP address or hostname instead.
- A PC-side bridge, Zabbix Agent on the ESP32, or HTTP polling by Zabbix.
- Backfilling a history of measurements after a long network outage.
- Telegram, email, TLS PSK, or other notification-channel configuration in the
  firmware. Notifications belong in Zabbix.

## Provisioning and Recovery

### First boot

When no Wi-Fi configuration exists in NVS, the firmware enters setup mode:

1. Start a temporary WPA2-protected Wi-Fi access point with an SSID containing
   a short device identifier.
2. Generate a temporary setup password and show the SSID/password once on the
   OLED and USB terminal; never print the user's Wi-Fi password or repeat the
   temporary password in periodic JSON diagnostics.
3. Serve a local configuration page at `http://192.168.4.1`.
4. Collect Wi-Fi SSID/password and Zabbix server IPv4 address or hostname.
   Default Zabbix port is TCP 10051; the Zabbix host identity is fixed as
   `server-room-esp32` to match the pre-created host.
5. Validate input, save it in NVS, then connect in station mode. Keep the setup
   AP/page available until the station connection succeeds, so a failed
   password or server address can be corrected without a firmware reflash.

The setup web page is local HTTP on a WPA2-protected temporary AP; it is not a
publicly reachable cloud portal.

Wi-Fi credentials are stored in NVS and are not written to source files or
diagnostic output. Flash encryption is not enabled in the current project, so
physical access to the board/flash is outside the first-version protection
scope.

### Normal boot and reconfiguration

- A normal RST or power cycle preserves NVS configuration and reconnects to the
  saved Wi-Fi network.
- If a saved network cannot be reached at boot, keep retrying without blocking
  measurements; the user can hold PRG after boot to enter setup mode and replace
  the saved network settings.
- Holding the board's `PRG`/`USER_SW` button for five seconds **after normal
  firmware startup** enters setup mode. Do not hold it while asserting reset:
  the button is on GPIO0, an ESP32-S3 boot-strap pin, and holding it during reset
  can enter the ROM download mode instead of starting the application.
- The `RST` button only resets the MCU; it does not erase network settings.

## Runtime Architecture

Use ESP-IDF `esp_wifi`, `esp_netif`, `esp_event`, `nvs_flash`, and
`esp_http_server` components. The application has distinct responsibilities:

1. **Sensor/display loop:** reads BMP280 and refreshes OLED/USB diagnostics at
   the existing one-second cadence.
2. **Wi-Fi state handling:** connects as a station, reports connection state,
   and retries with bounded exponential backoff without blocking measurement.
3. **Zabbix sender task:** receives the latest valid measurement/status snapshot
   and attempts a transmission every ten seconds when connected.
4. **Provisioning mode:** runs the setup AP and HTTP page when no configuration
   is saved or when PRG is held for the configured duration after boot.

Network operations must not block the one-second sensor/display loop. A bounded
queue may retain only the newest snapshot; stale samples are discarded rather
than uploaded in a burst after an outage.

## Zabbix Data Transport

The ESP32 opens an outbound TCP connection to the configured Zabbix Server on
port 10051 and sends batches using the Zabbix sender protocol (`ZBXD\x01`,
little-endian payload length, JSON sender request). No agent is installed on the
ESP32, and no inbound connection to the device is needed.

The sender host name is exactly `server-room-esp32`. A normal batch contains:

- `sensor.temperature`: numeric degrees Celsius.
- `sensor.pressure_pa`: numeric pascals.
- `sensor.status`: `ok` or `error`.

For invalid BMP280 readings, send `sensor.status=error` and omit temperature or
pressure values for that sample; do not substitute zero or a cached reading.
On a later valid reading, send all three values with status `ok`.

The initial transport uses plaintext Zabbix sender on the trusted home LAN.
Windows Firewall and Zabbix trapper `Allowed hosts` should be restricted to the
local network/device once the actual addresses are confirmed. TLS PSK can be
added as a follow-up hardening step.

## Network Reliability and User Feedback

- Continue sensor reads and OLED display during Wi-Fi loss or Zabbix downtime.
- Use finite connect/write timeouts and retry with backoff.
- After Wi-Fi reconnects, send the latest available measurement immediately,
  then resume the ten-second interval.
- Keep USB JSON diagnostics available, including connection status and last
  sender result and failure stage/reason, without logging passwords. The temporary AP password is shown
  only once during setup, not in periodic diagnostics.
- Display provisioning, connecting, online, Wi-Fi failure reason, and sensor-error states on OLED
  without replacing sensor error reporting with network status.

## Acceptance Criteria

1. With empty NVS, a protected setup AP appears and the page is reachable at
   `192.168.4.1`.
2. Valid Wi-Fi and Zabbix address settings persist after reset; normal reset
   reconnects without erasing them.
3. Holding PRG for five seconds after startup reopens the setup portal; holding
   PRG during reset is documented as unsupported because GPIO0 is a strap pin.
4. OLED and USB measurements continue at one-second cadence while sender
   connections time out or Wi-Fi is disconnected.
5. With valid network settings, values arrive under the exact three Zabbix keys
   on host `server-room-esp32` approximately every ten seconds.
6. Invalid BMP280 samples produce status `error` without sending numeric
   temperature/pressure values; recovery resumes valid values.
7. Reconnection after Wi-Fi loss sends current values without replaying stale
   history.
8. The setup and sender code never puts Wi-Fi credentials into source control,
   JSON diagnostics, or Zabbix values.

## Rollout

1. Implement and verify provisioning/NVS behavior independently of Zabbix.
2. Test Wi-Fi station connection and recovery on the actual home network.
3. Validate one Zabbix sender batch from the board against the existing trapper
   host/items.
4. Add invalid-sensor status reporting and network-loss recovery checks.
5. Configure Zabbix triggers/notifications separately after data arrives.

The first Wi-Fi test must confirm that the board can reach the PC's Zabbix IP;
being connected to the same SSID does not guarantee this if the router has
client isolation enabled.
