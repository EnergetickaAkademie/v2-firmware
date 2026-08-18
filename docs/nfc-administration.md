# NFC administrative records

Administrative tags use a single NDEF External Type record. All integer fields
are unsigned bytes unless noted otherwise.

## Reset card

- Type: `cz.enak:cmd`
- Payload: `01 01` (`version=1`, `command=RESET_BUILDINGS`)

The card must remain continuously present for two seconds. The board clears its
local scan state, persists a pending reset, and retries the authenticated server
reset until it is acknowledged. A short low tone means the card was recognized,
three rising tones confirm the hold, and a final high tone confirms the server.

## Wi-Fi provisioning tag

- Type: `cz.enak:wifi`
- Payload:

| Field | Size | Meaning |
| --- | ---: | --- |
| Version | 1 | `1` |
| Security | 1 | `0` = open, `1` = WPA/WPA2 passphrase |
| SSID length | 1 | 1–32 UTF-8 bytes |
| SSID | variable | UTF-8 bytes |
| Password length | 1 | 0 for open, otherwise 8–63 UTF-8 bytes |
| Password | variable | UTF-8 bytes |
| CRC32 | 4 | Big-endian CRC32 over every preceding payload byte |

The maximum record occupies less than the 144-byte NTAG213 user memory. The
firmware first stores credentials as a candidate, attempts the connection for
30 seconds, and promotes them to the active ESP32 Preferences configuration only
after joining the requested SSID. A failed candidate is removed and the previous
active configuration is restored.

The password is intentionally never written to logs. It is nevertheless present
in readable form on the NFC tag and in NVS unless ESP32 flash encryption is
enabled, so provisioning tags should be controlled like physical credentials.
