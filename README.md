# Firmware for ENAKv2 boards

Notes: for testing, disconnect WCHLink, or the CH32V003 won't respond to UART commands.

`Serial.begin()` does not work, `if (USART1->STATR & USART_STATR_RXNE) { char incomingChar = USART1->DATAR & 0xFF; }` does. Huh...

## ESP32-S3 mainboard OTA

1. Copy `uploader.conf.example` to `uploader.conf` and configure Wi-Fi, API, and OTA values.
2. Install the OTA-capable mainboard firmware once over USB. This initial flash installs the OTA partition table.
3. Start `uploader.py`, select **Mainboard** and **Wi-Fi OTA**, then enter the board IP or mDNS hostname.
4. The uploader builds `.pio/build/mainboard/firmware.bin` and sends it to the authenticated endpoint on port 8080.

The OTA password must contain at least eight characters. OTA updates apply only to the ESP32-S3 mainboard; CH32V003 powerplant and substation boards still require their wired programmer.

## Workshop v2 board sync

The mainboard exchanges telemetry, production coefficients/ranges, building
consumption values, and authoritative building counts through one fixed-size
binary `POST /board/sync/v2` request every 500 ms. The response echoes the
request sequence and carries a configuration revision, so a complete snapshot
is validated before it replaces the local values. Firmware temporarily falls
back to the legacy endpoints when CoreAPI does not yet expose the v2 route.
