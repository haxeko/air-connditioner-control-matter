#!/usr/bin/env python3
"""Write the project footprint library (ac_ir_s3.pretty) and its fp-lib-table.

Usage: python3 local_lib.py
The library holds footprints that differ from the KiCad library. They are rewritten from the KiCad
library on every build, so a KiCad update is picked up and the difference stays in this file.
"""
import os

FPDIR = "/usr/share/kicad/footprints"
PROJDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCAL_LIB = "ac_ir_s3"

# name: (KiCad library, [(text in the KiCad footprint, replacement)])
# The KiCad footprint of the module has 0.2 mm thermal vias under its GND pad. Low-cost 2-layer
# processes drill 0.3 mm at the smallest, so the copy has 0.3 mm holes (pad 0.6 mm, like a signal via).
LOCAL_FOOTPRINTS = {"ESP32-S3-WROOM-1": ("RF_Module", [("(drill 0.2)", "(drill 0.3)")])}

FP_LIB_TABLE = """(fp_lib_table
  (version 7)
  (lib (name "%s")(type "KiCad")(uri "${KIPRJMOD}/%s.pretty")(options "")(descr "Project footprints: module with 0.3 mm thermal vias"))
)
""" % (LOCAL_LIB, LOCAL_LIB)


def write():
    lib = os.path.join(PROJDIR, LOCAL_LIB + ".pretty")
    os.makedirs(lib, exist_ok=True)
    for name, (src, edits) in LOCAL_FOOTPRINTS.items():
        text = open(os.path.join(FPDIR, src + ".pretty", name + ".kicad_mod"), encoding="utf-8").read()
        for old, new in edits:
            if old not in text:
                raise SystemExit("%s: '%s' not found in the KiCad footprint" % (name, old))
            text = text.replace(old, new)
        open(os.path.join(lib, name + ".kicad_mod"), "w", encoding="utf-8").write(text)
    open(os.path.join(PROJDIR, "fp-lib-table"), "w", encoding="utf-8").write(FP_LIB_TABLE)
    print("wrote %s.pretty (%d footprint) and fp-lib-table" % (LOCAL_LIB, len(LOCAL_FOOTPRINTS)))


if __name__ == "__main__":
    write()
