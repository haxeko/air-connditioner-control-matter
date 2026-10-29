#!/usr/bin/env python3
"""Concatenate DER-encoded PAA root certificates into one binary bundle.

The firmware embeds the bundle and splits it again at runtime (each certificate is a DER
SEQUENCE, so the encoding is self-delimiting). Every input file is validated: it must be a single
DER SEQUENCE with a definite length that covers the whole file.

Usage: gen_paa_bundle.py OUTPUT INPUT_DIR [INPUT_DIR ...]
"""

import os
import sys


def der_total_length(data: bytes) -> int:
    if len(data) < 2 or data[0] != 0x30:
        raise ValueError("not a DER SEQUENCE")
    first = data[1]
    if first < 0x80:
        return 2 + first
    num = first & 0x7F
    if num == 0 or num > 3 or len(data) < 2 + num:
        raise ValueError("unsupported DER length encoding")
    length = int.from_bytes(data[2:2 + num], "big")
    return 2 + num + length


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    out_path = sys.argv[1]
    files = []
    for directory in sys.argv[2:]:
        for name in sorted(os.listdir(directory)):
            if name.lower().endswith(".der"):
                files.append(os.path.join(directory, name))
    if not files:
        print("no .der files found", file=sys.stderr)
        return 1

    bundle = bytearray()
    for path in files:
        with open(path, "rb") as f:
            data = f.read()
        try:
            total = der_total_length(data)
        except ValueError as exc:
            print(f"{path}: {exc}", file=sys.stderr)
            return 1
        if total != len(data):
            print(f"{path}: DER length {total} != file size {len(data)}", file=sys.stderr)
            return 1
        if total > 600:
            print(f"{path}: certificate larger than 600 bytes (CHIP kMaxDERCertLength)", file=sys.stderr)
            return 1
        bundle += data

    tmp_path = out_path + ".tmp"
    with open(tmp_path, "wb") as f:
        f.write(bundle)
    os.replace(tmp_path, out_path)
    print(f"PAA bundle: {len(files)} certificates, {len(bundle)} bytes -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
