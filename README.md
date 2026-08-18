# Firmware for ENAKv2 boards

Notes: for testing, disconnect WCHLink, or the CH32V003 won't respond to UART commands.

`Serial.begin()` does not work, `if (USART1->STATR & USART_STATR_RXNE) { char incomingChar = USART1->DATAR & 0xFF; }` does. Huh...

## ESP32-S3 mainboard OTA

1. Copy `uploader.conf.example` to `uploader.conf` and configure Wi-Fi, API, and OTA values.
2. Install the OTA-capable mainboard firmware once over USB. This initial flash installs the OTA partition table.
3. Start `uploader.py`, select **Mainboard** and **Wi-Fi OTA**, then enter the board IP or mDNS hostname.
4. The uploader builds `.pio/build/mainboard/firmware.bin` and sends it to the authenticated endpoint on port 8080.

The OTA password must contain at least eight characters. OTA updates apply only to the ESP32-S3 mainboard; CH32V003 powerplant and substation boards still require their wired programmer.

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
