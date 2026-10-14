#!/usr/bin/env python3
"""Validate a 12.70 HV code-window capture and list the old byte lead.

Usage: fw1270_hv_capture.py CAPTURE.bin CAPTURE.txt
The files are acquired from the console and deliberately kept outside Git.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

BASE_PA = 0x62820000
PAGE_SIZE = 0x1000
PAGE_COUNT = 48
OLD_LEAD = bytes.fromhex("ba fc 0c 00 00 ed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()

    data = args.image.read_bytes()
    lines = args.log.read_text().splitlines()
    expected_start = ("start,fw=0x12700001,soc=0x840f60,",
                      "start=0x0000000062820000,end=0x0000000062850000,"
                      "page_bytes=0x1000")
    if len(data) != PAGE_SIZE * PAGE_COUNT:
        parser.error(f"image is {len(data)} bytes; expected {PAGE_SIZE * PAGE_COUNT}")
    if not lines or not lines[0].startswith(expected_start[0]) or \
            expected_start[1] not in lines[0]:
        parser.error("log header does not identify the expected 12.70 capture")
    if lines[-1] != "end,complete=1,pages=48,bytes=0x30000":
        parser.error("log has no complete marker")
    if len(lines) != 2 * PAGE_COUNT + 2:
        parser.error(f"log has {len(lines)} lines; expected {2 * PAGE_COUNT + 2}")
    for page in range(PAGE_COUNT):
        offset = page * PAGE_SIZE
        pair = [f"before,offset=0x{offset:05x}",
                f"after,offset=0x{offset:05x}"]
        if lines[1 + 2 * page:3 + 2 * page] != pair:
            parser.error(f"checkpoint mismatch at image offset 0x{offset:x}")

    for path, content in ((args.image, data), (args.log, args.log.read_bytes())):
        print(f"{path.name}: {len(content)} bytes, sha256={hashlib.sha256(content).hexdigest()}")
    for offset in range(len(data)):
        if data.startswith(OLD_LEAD, offset):
            print(f"old lead at PA 0x{BASE_PA + offset:x}")
    old_address = 0x6283D800
    at_old_address = data[old_address - BASE_PA:old_address - BASE_PA + 8]
    print(f"13.60 address on 12.70: PA 0x{old_address:x} bytes={at_old_address.hex()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
