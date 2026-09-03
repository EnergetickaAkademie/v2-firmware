#!/usr/bin/env python3
"""Generate the C header used to validate the API/MQTT WSS certificate."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CERT_PATH = ROOT / "certs" / "isrgrootx1.pem"
HEADER_PATH = ROOT / "src" / "GeneratedApiCaCert.h"


def main() -> None:
    certificate = CERT_PATH.read_text(encoding="ascii")
    if "-----BEGIN CERTIFICATE-----" not in certificate:
        raise SystemExit(f"Not a PEM certificate: {CERT_PATH}")

    escaped = (
        certificate.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
    )
    HEADER_PATH.write_text(
        "#pragma once\n"
        "// Generated from certs/isrgrootx1.pem; do not edit manually.\n"
        f'#define API_CA_CERT "{escaped}"\n',
        encoding="ascii",
    )


if __name__ == "__main__":
    main()
