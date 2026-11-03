#!/usr/bin/env python3
"""Package the prebuilt firmware with the version in the file names.

Run from the ESP-IDF terminal after `idf.py build`:

    python tools/package_firmware.py

Creates in firmware/ (version = PROJECT_VER of the build, e.g. 1.2.0):
    ac_matter_ir_v1.2.0_full_0x0.bin       merged image, flash at 0x0 (first flash, erases NVS)
    ac_matter_ir_v1.2.0_app_0x40000.bin    application only, flash at 0x40000 (keeps NVS data)
removes the images of other versions, and prints the SHA-256 lines for firmware/README.md.
"""
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
OUT = os.path.join(ROOT, "firmware")


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

    for old in glob.glob(os.path.join(OUT, f"{name}_*.bin")):
        if os.path.basename(old) not in (full, app):
            os.remove(old)

    subprocess.run([sys.executable, "-m", "esptool", "--chip", desc.get("target", "esp32s3"), "merge-bin",
                    "-o", os.path.join(OUT, full), "@flash_args"], cwd=BUILD, check=True)
    shutil.copyfile(os.path.join(BUILD, f"{name}.bin"), os.path.join(OUT, app))

    print(f"\nfirmware v{version}, SHA-256:")
    for fn in (full, app):
        with open(os.path.join(OUT, fn), "rb") as f:
            print(f"- `{hashlib.sha256(f.read()).hexdigest()}`  {fn}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
