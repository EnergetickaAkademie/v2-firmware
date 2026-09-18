# Firmware for ENAKv2 boards

## Powerplant hardware and protocol

The current powerplant board uses PA2 for the communication LED, PC0 for the
internal status RGB, PC4/PC3 for the L9110S inputs, PD0 for the optional solar
RGB, and PA1 for the solar-panel ADC input. Connect a panel rated at no more
than 1.5 V between PA1 and ground, preferably through a 4.7–10 kOhm series
resistor.

Wind and hydro use proportional PWM with a 15% start threshold, a 300 ms
kick-start, and a 10% stop threshold. Coal and nuclear use binary nebulizer
control with a five-second restart lockout after shutdown. Active actuators
stop if commands are absent for 10 seconds.

### V1 ESP8266 powerplant backport

Legacy Wemos D1 mini powerplants can be reflashed to join the v2 PodSync bus
directly. In `uploader.py`, select **Powerplant (v1 ESP8266)**, choose one of
the eight concrete powerplant types, select its USB serial port, and click
**Upload V1 Powerplant**. Unlike a v2 CH32 powerplant, its UID is read from
`ESP.getChipId()` at runtime and is not generated or stored by the uploader.

| Signal or peripheral | Legacy pin |
| --- | --- |
| PodSync DATA | D1 / GPIO5 |
| PodSync CLOCK | D7 / GPIO13 |
| RGB LED or atomizer | D2 / GPIO4 |
| Motor inputs | D5 / GPIO14 and D6 / GPIO12 |
| Identification LED | D4 / GPIO2, active-low |

Connect the v2 substation clock to D7, data to D1, and ground to ground. The
existing open-drain data pull-up must remain present. The firmware deliberately
disables Wi-Fi and supports USB flashing only, which avoids radio activity
interfering with the bit-banged bus.

The old board has only one peripheral RGB LED. On solar, gas, pumped-hydro,
and battery modules it displays the RGB value sent by the v2 system. On a
solar module this means A0 cannot simultaneously drive a separate local solar
indicator as it does on v2 hardware. The legacy `PowerplantModule/OneWireSlave`
firmware remains StarWire-only and is used as a hardware reference; it must be
replaced by the `powerplant_v1` target.

The backport requires the PodSync version that supports `PODSYNC_CLK_PIN` and
`PODSYNC_DAT_PIN`. Land that PodSync update before using this repository from
a clean checkout because PlatformIO downloads PodSync from its Git repository.


## ESP32-S3 mainboard OTA

1. Copy `uploader.conf.example` to `uploader.conf` and configure Wi-Fi, API, and OTA values.
2. Install the OTA-capable mainboard firmware once over USB. This initial flash installs the OTA partition table.
3. Start `uploader.py`, select **Mainboard** and **Wi-Fi OTA**, then enter the board IP or mDNS hostname.
4. The uploader builds `.pio/build/mainboard/firmware.bin` and sends it to the authenticated endpoint on port 8080.

The OTA password must contain at least eight characters. OTA updates apply only to the ESP32-S3 mainboard; CH32V003 powerplant and substation boards still require their wired programmer. For HTTPS API endpoints, set api_ca_cert_file to a PEM root certificate file trusted by the board. The certificate is compiled into the firmware.

### WebControl firmware artifacts

The Mainboard form also contains release-artifact settings. Configure
`firmware_download_url`, `firmware_artifact_dir`, `firmware_manifest_file`,
`firmware_channel`, and `firmware_config_schema` in `uploader.conf`, then click
**Build Firmware + Manifest**. The uploader builds the mainboard with the
current Wi-Fi/API/OTA values, writes `mb_firmware.bin` to the artifact
directory, calculates its SHA-256, and adds or replaces that version in the
manifest file. Host the generated binary and manifest at the configured HTTPS
locations before selecting the release in WebControl.

For wired CH32V003 flashing, select **Powerplant** or **Substation** in
`uploader.py`. CH32 powerplants additionally require a device type and receive
a generated UID; substations use the fixed `substation` PlatformIO environment.

The uploader can also inspect logs without flashing. Select **USB / serial** and
click **Show serial**, or select **Wi-Fi OTA** and click **Show Wi-Fi log**. The
Wi-Fi view uses the authenticated `/ota/log` endpoint and includes buffered
application messages from boot, even if Wi-Fi connected later. The same OTA
password protects firmware uploads and remote logs.

## Workshop v2 board sync

The mainboard exchanges telemetry, production coefficients/ranges, building
consumption values, and authoritative building counts through one fixed-size
binary `POST /board/sync/v2` request every 500 ms. The response echoes the
request sequence and carries a configuration revision, so a complete snapshot
is validated before it replaces the local values. Firmware temporarily falls
back to the legacy endpoints when CoreAPI does not yet expose the v2 route.

The normal `mainboard` environment prefers MQTT v3 and automatically falls
back to this HTTP endpoint. For a broker-independent recovery image, build
`pio run -e mainboard_http_v2`; it uses `/board/sync/v2` exclusively and keeps
the board's existing NVS provisioning when installed over OTA.
The graphical uploader exposes the same choice as the **Communication**
checkbox; uncheck MQTT v3 before uploading to install the HTTP-only image.

## NFC debug portal

The NFC Flasher can write v2 administrative cards. A debug card uses
`cz.enak:cmd`, payload `02 02`; a bulldozer card uses the same external type
with payload `02 03`. Present the bulldozer card once, then present building
cards within ten seconds to remove those buildings from the board and server.
The window accepts multiple building cards and expires automatically.
For a debug card, hold it continuously on the mainboard reader for three seconds. The board then
pauses cloud networking and exposes an AP named `ENAK-<board-username>` (or a
chip-ID fallback) using the configured debug AP password, `enak-debug` by
default. Join that AP and open `http://enak.local/`; captive-portal probes are
redirected there automatically.

The portal edits the supported device and network settings and shows live
substation/powerplant diagnostics. Password fields are write-only. Saving or
exiting reboots the board into normal operation; a Wi-Fi change is tested on
the next boot and the previous network is restored if it cannot connect.

## Mainboard status LED

The user-visible WS2812 status LED is connected to GPIO 7. The original red
GPIO 38 board LED remains an internal heartbeat indicator.

| Pattern | Meaning |
| --- | --- |
| White breathing | Mainboard initialization |
| Blue breathing | Connecting to Wi-Fi |
| Blue double flash | Wi-Fi connection lost |
| Purple breathing | Authenticating or registering with CoreAPI |
| Purple double flash | CoreAPI unreachable or stale |
| Orange triple flash | Authentication or board registration rejected |
| Alternating purple/red | Invalid API response |
| Cyan fast pulse | OTA update in progress |
| Alternating cyan/red | OTA update failed |
| Yellow breathing | No substations online |
| Light green | One substation online (normal operation) |
| Medium green | Two substations online |
| Dark green | Three substations online |
| Two green/red flashes | NFC tag accepted/rejected |
| Periodic pink triple flash | PN532 unavailable |

Network and OTA errors take priority over the substation indication. API
transport/protocol failures are shown only after repeated failures or a stale
connection, so a single transient request does not make the LED flicker.
