#!/usr/bin/env python3
"""Reproduce the committed, offline-only Download Ideas QR asset.

Install requirements-qr.txt only when regenerating this asset. Normal firmware
builds consume the PNG and do not need a QR encoder.
"""
from pathlib import Path

import qrcode

URL = "https://mosaico.ai/"
OUTPUT = Path(__file__).resolve().parents[1] / "ui/assets/ideas-qr.png"
COMPACT_OUTPUT = OUTPUT.parent / "qr/ideas.png"


def main():
    qr = qrcode.QRCode(version=3, error_correction=qrcode.constants.ERROR_CORRECT_M,
                       box_size=5, border=4, mask_pattern=0)
    qr.add_data(URL, optimize=0)
    qr.make(fit=False)
    for output, box_size in ((OUTPUT, 5), (COMPACT_OUTPUT, 1)):
        output.parent.mkdir(parents=True, exist_ok=True)
        qr.box_size = box_size
        qr.make_image(fill_color="black", back_color="white").save(output)


if __name__ == "__main__":
    main()
