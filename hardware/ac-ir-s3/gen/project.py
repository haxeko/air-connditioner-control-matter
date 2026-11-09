#!/usr/bin/env python3
"""Set design rules, net classes and DRC severities in ac_ir_s3.kicad_pro (JSON)."""
import json
import sys

POWER_NETS = ["VBUS", "/PH_5V", "/VIN", "+5V", "+3V3", "/IR_A", "/IR_M", "/IR_K"]


def patch(path):
    d = json.load(open(path, encoding="utf-8"))
    ds = d["board"]["design_settings"]
    ds["rules"].update({
        "min_clearance": 0.2,
        # narrow necks in the GND pours are allowed down to 0.15 mm (every GND pad also has its own via)
        "min_connection": 0.15,
        "min_copper_edge_clearance": 0.3,
        "min_hole_clearance": 0.2,
        "min_hole_to_hole": 0.25,
        # The ESP32-S3-WROOM-1 library footprint has 0.2 mm thermal vias under its GND pad.
        "min_through_hole_diameter": 0.2,
        "min_track_width": 0.2,
        "min_via_annular_width": 0.1,
        "min_via_diameter": 0.5,
        "min_text_height": 0.8,
        "min_text_thickness": 0.12,
    })
    ds["track_widths"] = [0.0, 0.25, 0.5]
    ds["via_dimensions"] = [{"diameter": 0.0, "drill": 0.0}, {"diameter": 0.6, "drill": 0.3},
                            {"diameter": 0.8, "drill": 0.4}]
    sev = ds.setdefault("rule_severities", {})
    # The antenna, the USB-C receptacle and the IR LEDs / receiver intentionally overhang the board
    # edge; their library silkscreen outside the board is simply not printed.
    sev["silk_edge_clearance"] = "ignore"
    ns = d["net_settings"]
    default = next(c for c in ns["classes"] if c["name"] == "Default")
    default.update({"clearance": 0.2, "track_width": 0.25, "via_diameter": 0.6, "via_drill": 0.3})
    power = dict(default)
    power.update({"name": "Power", "track_width": 0.5, "via_diameter": 0.8, "via_drill": 0.4, "priority": 0})
    ns["classes"] = [default, power]
    ns["netclass_patterns"] = [{"netclass": "Power", "pattern": n} for n in POWER_NETS]
    ns["netclass_assignments"] = None
    json.dump(d, open(path, "w", encoding="utf-8"), indent=2)
    open(path, "a").write("\n")
    print("patched", path)


if __name__ == "__main__":
    patch(sys.argv[1])
