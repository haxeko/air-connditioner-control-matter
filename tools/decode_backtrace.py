#!/usr/bin/env python3
"""Turn the addresses of a crash report into function names.

The device shows the report on the diagnostics page of the configuration portal ("Sự cố khởi động
lại") and with the console command `diag`: the program counter and the return addresses (backtrace)
of the task that crashed. This script looks them up in the symbol file published next to every
firmware (firmware/ac_matter_ir_v<version>_symbols.txt.gz). No ESP-IDF needed.

    python tools/decode_backtrace.py 0x4201a2c4 0x4201a2c1 0x4201b7f0
    python tools/decode_backtrace.py --elf-sha 3f9a1c0de 0x4201a2c4 ...      (also checks the build)
    python tools/decode_backtrace.py --symbols other_symbols.txt.gz 0x4201a2c4 ...

The addresses only mean something for the firmware that crashed: use the symbol file of that
version (the report says whether it was the running firmware, and gives the start of its ELF SHA-256).
"""
import argparse
import bisect
import glob
import gzip
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(path):
    header = ""
    syms = []
    with gzip.open(path, "rt", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#"):
                header = line[1:].strip()
                continue
            parts = line.split(" ", 2)
            if len(parts) == 3:
                syms.append((int(parts[0], 16), int(parts[1], 16), parts[2]))
    syms.sort()
    return header, syms


def lookup(syms, starts, addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0:
        return None
    start, size, name = syms[i]
    if addr < start + max(size, 1) or (size == 0 and addr - start < 0x400):
        return "%s + 0x%x" % (name, addr - start)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--symbols", help="symbol file (default: the one in firmware/)")
    ap.add_argument("--elf-sha", help="start of the ELF SHA-256 shown with the report")
    ap.add_argument("addresses", nargs="+", help="addresses of the report, hex")
    args = ap.parse_args()

    path = args.symbols
    if path is None:
        found = sorted(glob.glob(os.path.join(ROOT, "firmware", "*_symbols.txt.gz")))
        if len(found) != 1:
            print("give --symbols: %d symbol files in firmware/" % len(found), file=sys.stderr)
            return 2
        path = found[0]
    header, syms = load(path)
    starts = [s[0] for s in syms]
    print("symbols: %s (%s)" % (os.path.basename(path), header or "no header"))
    if args.elf_sha:
        sha = header.split("elf_sha256 ")[-1] if "elf_sha256 " in header else ""
        if sha.startswith(args.elf_sha.lower()):
            print("build matches the report")
        else:
            print("WARNING: the report comes from another build (ELF %s...): the names below may be wrong" % args.elf_sha)
    for text in args.addresses:
        addr = int(text, 16)
        hit = lookup(syms, starts, addr)
        if hit is None and addr >> 30 != 1:
            # return addresses of the windowed ABI carry the window size in the two top bits
            alt = (addr & 0x3FFFFFFF) | 0x40000000
            hit = lookup(syms, starts, alt)
        print("0x%08x  %s" % (addr, hit or "? (not in the code of this build)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
