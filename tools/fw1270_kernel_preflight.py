#!/usr/bin/env python3
"""Check the pinned 12.70 kernel dump and the first handoff signatures.

This is an offline, read-only check. Passing it does not validate live kernel
bytes or any hypervisor address, and must not enable a console payload.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


EXPECTED_SIZE = 0xD50000
EXPECTED_SHA256 = "9d79abbbb4dec7e077d5c65031b84b38955306d0772a7007c675d6e27a446c61"
EXPECTED_TEXT_BASE = 0xFFFFFFFFC1070000
HV_MP_STARTUP_CALL = 0xC9DFB0
SMP_RENDEZVOUS = 0xB8FBC0
SMP_RENDEZVOUS_CPUS = 0xB8F980
ALL_CPUS_DATA = 0x3D30228

SIGNATURES = {
    "hv_mp_startup_call": (
        HV_MP_STARTUP_CALL,
        bytes.fromhex("55 48 89 e5 53 48 89 d3 48 89 f1 b8 0f 00 00 00 "
                      "89 fa 0f 01 d9 5b 5d c3"),
    ),
    "smp_rendezvous": (
        SMP_RENDEZVOUS,
        bytes.fromhex("55 48 89 e5 49 89 c8 48 89 d1 48 89 f2 48 89 fe "
                      "48 8b 3d 51 06 1a 03 5d e9 a3 fd ff ff"),
    ),
    "smp_rendezvous_cpus entry": (
        SMP_RENDEZVOUS_CPUS,
        bytes.fromhex("55 48 89 e5 41 57 41 56 41 55 41 54 53 50"),
    ),
    "setup callback": (0xB8FAF7, bytes.fromhex("41 ff d4")),
    "action callback": (0xB8FB1C, bytes.fromhex("41 ff d7")),
    "teardown callback": (0xB8FB41, bytes.fromhex("41 ff d6")),
}


def check_dump(data: bytes) -> list[str]:
    errors = []
    if len(data) != EXPECTED_SIZE:
        errors.append(f"size: got {len(data):#x}, expected {EXPECTED_SIZE:#x}")
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_SHA256:
        errors.append(f"SHA-256: got {digest}, expected {EXPECTED_SHA256}")
    if errors:
        return errors

    for name, (offset, expected) in SIGNATURES.items():
        actual = data[offset:offset + len(expected)]
        if actual != expected:
            errors.append(f"{name} at text+{offset:#x}: got {actual.hex()}, "
                          f"expected {expected.hex()}")

    # Decode the two PC-relative operands instead of trusting only raw bytes.
    rip_after_load = SMP_RENDEZVOUS + 0x17
    load_displacement = int.from_bytes(data[SMP_RENDEZVOUS + 0x13:
                                            SMP_RENDEZVOUS + 0x17],
                                       "little", signed=True)
    if rip_after_load + load_displacement != ALL_CPUS_DATA:
        errors.append("smp_rendezvous CPU-set data target changed")

    rip_after_jump = SMP_RENDEZVOUS + 0x1D
    jump_displacement = int.from_bytes(data[SMP_RENDEZVOUS + 0x19:
                                            SMP_RENDEZVOUS + 0x1D],
                                       "little", signed=True)
    if rip_after_jump + jump_displacement != SMP_RENDEZVOUS_CPUS:
        errors.append("smp_rendezvous tail-call target changed")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel_dump", type=Path, help="12.70 kernel-text dump")
    parser.add_argument("--log", type=Path, help="companion dump log")
    args = parser.parse_args()

    data = args.kernel_dump.read_bytes()
    errors = check_dump(data)
    if args.log:
        lines = args.log.read_text(errors="replace").splitlines()
        if not lines or f"text={EXPECTED_TEXT_BASE:#x}" not in lines[0]:
            errors.append("companion log does not report the expected text base")
        if not lines or "end,complete=1" not in lines[-1]:
            errors.append("companion log does not report a complete dump")

    if errors:
        for error in errors:
            print(f"FAIL {error}")
        return 1

    print(f"PASS pinned 12.70 kernel text: {len(data):#x} bytes, SHA-256 matched")
    print(f"PASS hv_mp_startup_call: text+{HV_MP_STARTUP_CALL:#x}")
    print(f"PASS smp_rendezvous wrapper: text+{SMP_RENDEZVOUS:#x} "
          f"-> text+{SMP_RENDEZVOUS_CPUS:#x}, CPU set at text+{ALL_CPUS_DATA:#x}")
    print("PASS setup, action, teardown callback call sites")
    print("LIMIT live kernel and hypervisor bytes remain unverified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
