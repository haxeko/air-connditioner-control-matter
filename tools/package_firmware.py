#!/usr/bin/env python3
"""Package the prebuilt firmware with the version in the file names.

Run from the ESP-IDF terminal after `idf.py build`:

    python tools/package_firmware.py

Creates in firmware/ (version = PROJECT_VER of the build, e.g. 1.6.0):
    ac_matter_ir_v1.6.0_full_0x0.bin        merged image, flash at 0x0 (first flash, erases NVS)
    ac_matter_ir_v1.6.0_app_0x40000.bin     application only: upload on the web page, or flash at
                                            0x40000 (keeps NVS data)
    ac_matter_ir_v1.6.0_bootloader_0x0.bin  bootloader only, flash at 0x0 (keeps NVS data): gives a board
                                            first flashed before v1.5.0 the rollback of failed updates
    ac_matter_ir_v1.6.0_partitions_0xC000.bin  partition table only, flash at 0xC000 (keeps NVS data): gives a
                                            board first flashed before v1.6.0 the "coredump" partition
    ac_matter_ir_v1.6.0_symbols.txt.gz      function addresses of this build, for
                                            tools/decode_backtrace.py (crash report of the device)
removes the files of other versions, and prints the SHA-256 lines for firmware/README.md.
"""
import glob
import gzip
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
OUT = os.path.join(ROOT, "firmware")


def write_symbols(desc, out_path):
    """Function symbols of the ELF: '<address> <size> <name>' in hex, sorted by address.

    The first line carries the SHA-256 of the ELF: the device reports its first characters with a crash
    report, so the decoder can tell whether the addresses belong to this build.
    """
    elf = os.path.join(BUILD, desc["app_elf"])
    nm = desc.get("monitor_toolprefix", "xtensa-esp32s3-elf-") + "nm"
    out = subprocess.run([nm, "-n", "-S", "-C", "--defined-only", elf], check=True, capture_output=True, text=True).stdout
    with open(elf, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    lines = [f"# {desc['project_name']} {desc['project_version']} elf_sha256 {sha}"]
    for line in out.splitlines():
        parts = line.split(" ", 3)
        if len(parts) == 4 and parts[2] in "TtWw":      # address size type name: code only
            lines.append(f"{parts[0]} {parts[1]} {parts[3]}")
    # mtime=0 and no file name in the header: the same build gives the same file
    with open(out_path, "wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as gz:
        gz.write(("\n".join(lines) + "\n").encode("utf-8"))
    print(f"{len(lines) - 1} function symbols -> {os.path.basename(out_path)}")


def main() -> int:
    desc_path = os.path.join(BUILD, "project_description.json")
    if not os.path.exists(desc_path):
        print("build/project_description.json missing: run `idf.py build` first", file=sys.stderr)
        return 1
    with open(desc_path, encoding="utf-8") as f:
        desc = json.load(f)
    version = desc["project_version"]
    name = desc["project_name"]
    full = f"{name}_v{version}_full_0x0.bin"
    app = f"{name}_v{version}_app_0x40000.bin"
    boot = f"{name}_v{version}_bootloader_0x0.bin"
    part = f"{name}_v{version}_partitions_0xC000.bin"

    symbols = f"{name}_v{version}_symbols.txt.gz"

    for old in glob.glob(os.path.join(OUT, f"{name}_*.bin")) + glob.glob(os.path.join(OUT, f"{name}_*_symbols.txt.gz")):
        if os.path.basename(old) not in (full, app, boot, part, symbols):
            os.remove(old)

    subprocess.run([sys.executable, "-m", "esptool", "--chip", desc.get("target", "esp32s3"), "merge-bin",
                    "-o", os.path.join(OUT, full), "@flash_args"], cwd=BUILD, check=True)
    shutil.copyfile(os.path.join(BUILD, f"{name}.bin"), os.path.join(OUT, app))
    shutil.copyfile(os.path.join(BUILD, "bootloader", "bootloader.bin"), os.path.join(OUT, boot))
    shutil.copyfile(os.path.join(BUILD, "partition_table", "partition-table.bin"), os.path.join(OUT, part))

    write_symbols(desc, os.path.join(OUT, symbols))

    print(f"\nfirmware v{version}, SHA-256:")
    for fn in (full, app, boot, part):
        with open(os.path.join(OUT, fn), "rb") as f:
            print(f"- `{hashlib.sha256(f.read()).hexdigest()}`  {fn}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
