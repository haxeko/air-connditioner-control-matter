#!/usr/bin/env python3
"""Generate ac_ir_s3.kicad_sch from netdef.py (KiCad 9 format).

Every used pin gets a short wire stub ending in a net label or a power symbol, except the pin pairs
listed in WIRES, which are joined by a straight wire. Unused pins get a no-connect flag.
Run with the system python3 (no KiCad module needed): python3 make_sch.py <out.kicad_sch>
"""
import copy
import math
import os
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netdef  # noqa: E402
from sexp import Sym, dump, find, first, parse  # noqa: E402

SYMDIR = "/usr/share/kicad/symbols"
NS = uuid.UUID("6f1c2f4e-3a59-4d7e-9a43-ac1500000001")
ROOT = str(uuid.uuid5(NS, "root"))
G = 1.27

# ---------------------------------------------------------------- sheet layout (x, y, rotation)
SCH = {
    # Power input: USB-C or PH2.0, OR-ing Schottky diodes, PTC fuse
    "J1": (33.02, 50.8, 0), "R3": (58.42, 71.12, 0), "R4": (66.04, 71.12, 0),
    "D5": (66.04, 30.48, 180), "D6": (66.04, 40.64, 180), "F1": (86.36, 30.48, 90),
    "J2": (93.98, 60.96, 0),
    # 3.3 V regulator
    "C2": (109.22, 36.83, 0), "U2": (124.46, 35.56, 0), "C1": (142.24, 36.83, 0),
    # ESP32-S3 module and decoupling
    "U1": (182.88, 99.06, 0), "C3": (203.2, 62.23, 0), "C4": (210.82, 62.23, 0),
    # EN power-on reset, BOOT
    "R1": (106.68, 72.39, 0), "C5": (114.3, 83.82, 0),
    "R2": (134.62, 72.39, 0), "SW1": (144.78, 83.82, 270),
    # IR transmitter
    "R8": (43.18, 110.49, 0), "C8": (58.42, 110.49, 0),
    "D2": (43.18, 123.19, 90), "D3": (43.18, 135.89, 90),
    "Q1": (40.64, 151.13, 0), "R6": (27.94, 151.13, 90), "R7": (33.02, 154.94, 0),
    # IR receiver
    "U4": (96.52, 140.97, 0), "R5": (121.92, 132.08, 0), "C6": (129.54, 139.7, 0),
    # Status LED
    "D1": (238.76, 27.94, 180), "C7": (279.4, 34.29, 0),
    "R9": (243.84, 53.34, 90), "D4": (269.24, 53.34, 0),
    # Programming pads (bottom side of the board)
    "TP1": (236.22, 101.6, 0), "TP2": (246.38, 101.6, 0), "TP3": (256.54, 101.6, 0),
    "TP4": (266.7, 101.6, 0), "TP5": (276.86, 101.6, 0),
}

# Reference / Value positions (x, y, justify) where the defaults collide with stubs or symbols.
FIELDS = {
    "J1": ((22.86, 36.83, "left"), (27.94, 36.83, "left")),
    "J2": ((93.98, 55.88, "center"), (93.98, 67.31, "center")),
    "U2": ((119.38, 25.4, "left"), (124.46, 25.4, "left")),
    "U1": ((167.64, 69.85, "left"), (196.85, 132.08, "left")),
    "Q1": ((45.72, 149.86, "left"), (45.72, 152.4, "left")),
    "D4": ((275.59, 46.99, "left"), (254.0, 62.23, "left")),
    "C7": ((276.86, 33.02, "right"), (276.86, 35.56, "right")),
    "D2": ((52.07, 121.92, "center"), (52.07, 124.46, "center")),
    "D3": ((52.07, 134.62, "center"), (52.07, 137.16, "center")),
    "D5": ((66.04, 26.67, "center"), (66.04, 34.29, "center")),
    "D6": ((66.04, 36.83, "center"), (66.04, 44.45, "center")),
}
for _i in range(5):
    _x = 236.22 + 10.16 * _i
    FIELDS["TP%d" % (_i + 1)] = ((_x + 1.27, 95.25, "left"), (_x + 1.27, 97.79, "left"))

# Wires between pins (straight lines through the listed points). label = (name, direction of text).
WIRES = [
    dict(path=[("D5", "1"), ("F1", "1")], label=("VIN", (1, 0))),
    dict(path=[("R8", "2"), ("D2", "2")], label=("IR_A", (-1, 0))),
    dict(path=[("D2", "1"), ("D3", "2")], label=("IR_M", (-1, 0))),
    dict(path=[("D3", "1"), ("Q1", "3")], label=("IR_K", (-1, 0))),
    dict(path=[("R6", "2"), ("R7", "1"), ("Q1", "1")], label=None),
    dict(path=[("U4", "3"), ("R5", "2"), ("C6", "1")], label=("IR_VS", (1, 0))),
    dict(path=[("R9", "2"), ("D4", "3")], label=("RGB_DIN", (1, 0)), at=2.54),
    dict(path=[("U2", "3"), ("U2", "1")], label=None),
]
# Wired pins that still get their own stub and power symbol / label.
KEEP_STUB = {("U2", "1"), ("R7", "1")}

# PWR_FLAG positions: flag pin at (x, y); a wire goes 10.16 mm to the right to the net's symbol/label.
FLAGS = {"GND": (167.64, 151.13), "VBUS": (186.69, 151.13), "PH_5V": (205.74, 151.13),
         "+5V": (224.79, 151.13), "LED_VDD": (243.84, 151.13), "IR_VS": (262.89, 151.13)}

GROUPS = [  # title, x1, y1, x2, y2
    ("5 V input: USB-C (power only) or PH2.0", 12.7, 15.24, 100.33, 87.63),
    ("3.3 V LDO (600 mA)", 101.6, 15.24, 152.4, 50.8),
    ("EN power-on reset, BOOT button", 101.6, 55.88, 154.94, 108.71),
    ("ESP32-S3 module", 163.83, 50.8, 226.06, 137.16),
    ("IR transmitter (2 x 940 nm LED)", 12.7, 92.71, 76.2, 171.45),
    ("IR receiver (38 kHz, demodulating)", 77.47, 116.84, 137.16, 158.75),
    ("Status LED (GPIO48)", 227.33, 15.24, 284.48, 69.85),
    ("Programming pads (bottom side)", 227.33, 82.55, 284.48, 127.0),
    ("Power flags", 163.83, 139.7, 284.48, 163.83),
]

NOTES = [
    (102.87, 99.06, ["BOOT x2: Matter pairing, x3: setup Wi-Fi", "Hold BOOT (or short IO0 to GND) at power-up:",
                     "ROM download mode, flash through UART0 pads"]),
    (229.87, 115.57, ["2.54 mm pitch: 3V3 TX RX GND IO0", "Short IO0 to GND at power-up to flash"]),
    (17.78, 85.09, ["Each input has its own Schottky diode (PH2.0 reverse-polarity safe)"]),
    (12.7, 177.8, [
        "Firmware defaults: IR TX GPIO4 (active high), IR RX GPIO5 (active low), BOOT GPIO0, RGB LED GPIO48 (WS2812).",
        "N16R8: GPIO35-37 belong to the octal PSRAM, keep them unconnected. GPIO8/9/19/20 are unused.",
        "USB-C and PH2.0 supply 5 V only. Flashing, logs and REPL console: UART0 pads with a 3.3 V USB-UART adapter.",
        "No reset button: EN has an RC power-on reset, remove power to reset.",
        "IR LED current: (5 V - 0.4 V - 2 x 1.35 V) / 18 ohm = ~100 mA peak at 33 % carrier duty.",
        "U4: TSOP38238, VS1838B or any 38 kHz receiver with pinout OUT-GND-VS (check the datasheet).",
    ]),
]


# ---------------------------------------------------------------- library symbols
_libs = {}


def lib_file(name):
    if name not in _libs:
        _libs[name] = parse(open(os.path.join(SYMDIR, name + ".kicad_sym"), encoding="utf-8").read())
    return _libs[name]


def raw_symbol(lib, name):
    for s in find(lib_file(lib), "symbol"):
        if s[1] == name:
            return s
    raise KeyError("%s:%s" % (lib, name))


def flat_symbol(lib_id):
    """Library symbol with 'extends' resolved, named lib_id (as embedded in a schematic)."""
    lib, name = lib_id.split(":")
    s = copy.deepcopy(raw_symbol(lib, name))
    chain = [s]
    while first(chain[-1], "extends"):
        chain.append(copy.deepcopy(raw_symbol(lib, first(chain[-1], "extends")[1])))
    base = chain[-1]
    props = {}
    order = []
    for sym in reversed(chain):  # base first, derived overrides
        for p in find(sym, "property"):
            if p[1] not in props:
                order.append(p[1])
            props[p[1]] = p
    flags = {}
    for sym in reversed(chain):
        for key in ("pin_numbers", "pin_names", "exclude_from_sim", "in_bom", "on_board", "power"):
            f = first(sym, key)
            if f is not None:
                flags[key] = f
    out = [Sym("symbol"), lib_id]
    for key in ("power", "pin_numbers", "pin_names", "exclude_from_sim", "in_bom", "on_board"):
        if key in flags:
            out.append(flags[key])
    out += [props[k] for k in order]
    basename = base[1]
    for unit in find(base, "symbol"):
        unit = copy.deepcopy(unit)
        suffix = unit[1][len(basename):]
        unit[1] = name + suffix
        out.append(unit)
    out.append([Sym("embedded_fonts"), Sym("no")])
    return out


def sym_pins(lib_id):
    """[(number, name, x, y, angle, hidden)] in library coordinates (Y up)."""
    s = flat_symbol(lib_id)
    res = []

    def walk(e):
        for x in e:
            if isinstance(x, list) and x and x[0] == "symbol":
                walk(x)
            elif isinstance(x, list) and x and x[0] == "pin":
                at = first(x, "at")
                hidden = any(isinstance(y, Sym) and y == "hide" for y in x) or \
                    any(isinstance(y, list) and y and y[0] == "hide" and y[1] == "yes" for y in x)
                res.append((first(x, "number")[1], first(x, "name")[1], float(at[1]), float(at[2]),
                            int(float(at[3])) if len(at) > 3 else 0, hidden))
    walk(s)
    return res


# ---------------------------------------------------------------- geometry
def snap(v):
    return round(round(v / G) * G, 4)


def xform(at, lx, ly):
    """Library point -> sheet point for a symbol placed at (x, y, rot), rot CCW on screen."""
    x, y, rot = at
    a = math.radians(rot)
    sx, sy = lx, -ly
    rx = sx * math.cos(a) + sy * math.sin(a)
    ry = -sx * math.sin(a) + sy * math.cos(a)
    return (snap(x + rx), snap(y + ry))


def xdir(at, angle):
    """Outward direction (sheet coords) of a pin whose library angle is 'angle'."""
    x, y, rot = at
    a = math.radians(angle)
    dx, dy = -math.cos(a), math.sin(a)  # outward = opposite of pin direction, Y flipped
    r = math.radians(rot)
    rx = dx * math.cos(r) + dy * math.sin(r)
    ry = -dx * math.sin(r) + dy * math.cos(r)
    return (int(round(rx)), int(round(ry)))


def uid(*parts):
    return str(uuid.uuid5(NS, "/".join(str(p) for p in parts)))


def font(size=1.27, bold=False):
    f = [Sym("font"), [Sym("size"), size, size]]
    if bold:
        f.append([Sym("bold"), Sym("yes")])
    return f


def prop(name, value, x, y, angle=0, hide=False, justify=None, size=1.27):
    eff = [Sym("effects"), font(size)]
    if justify and justify != "center":
        eff.append([Sym("justify")] + [Sym(j) for j in justify.split()])
    if hide:
        eff.append([Sym("hide"), Sym("yes")])
    return [Sym("property"), name, value, [Sym("at"), x, y, angle], eff]


# ---------------------------------------------------------------- builder
class Sheet:
    def __init__(self):
        self.items = []
        self.lib_ids = []
        self.pwr_n = 0
        self.flg_n = 0
        self.points = {}  # (x, y) -> count of connection items (for junctions)

    def use_lib(self, lib_id):
        if lib_id not in self.lib_ids:
            self.lib_ids.append(lib_id)

    def touch(self, p):
        self.points[p] = self.points.get(p, 0) + 1

    def wire(self, p1, p2, key):
        self.items.append([Sym("wire"), [Sym("pts"), [Sym("xy"), p1[0], p1[1]], [Sym("xy"), p2[0], p2[1]]],
                           [Sym("stroke"), [Sym("width"), 0], [Sym("type"), Sym("default")]],
                           [Sym("uuid"), uid("wire", key)]])
        self.touch(p1)
        self.touch(p2)

    def label(self, name, p, d, key):
        angle, just = {(1, 0): (0, "left bottom"), (-1, 0): (180, "right bottom"),
                       (0, -1): (90, "left bottom"), (0, 1): (270, "right bottom")}[d]
        self.items.append([Sym("label"), name, [Sym("at"), p[0], p[1], angle],
                           [Sym("effects"), font(), [Sym("justify")] + [Sym(j) for j in just.split()]],
                           [Sym("uuid"), uid("label", key)]])

    def no_connect(self, p, key):
        self.items.append([Sym("no_connect"), [Sym("at"), p[0], p[1]], [Sym("uuid"), uid("nc", key)]])

    def text(self, s, x, y, size=1.27, bold=False, key=None):
        self.items.append([Sym("text"), s, [Sym("exclude_from_sim"), Sym("no")], [Sym("at"), x, y, 0],
                           [Sym("effects"), font(size, bold), [Sym("justify"), Sym("left"), Sym("bottom")]],
                           [Sym("uuid"), uid("text", key or s)]])

    def rect(self, x1, y1, x2, y2, key):
        self.items.append([Sym("rectangle"), [Sym("start"), x1, y1], [Sym("end"), x2, y2],
                           [Sym("stroke"), [Sym("width"), 0], [Sym("type"), Sym("dash")]],
                           [Sym("fill"), [Sym("type"), Sym("none")]], [Sym("uuid"), uid("rect", key)]])

    def symbol(self, lib_id, ref, value, at, footprint="", fields=None, in_bom=True, ref_pos=None, val_pos=None,
               hide_ref=False, hide_val=False):
        self.use_lib(lib_id)
        s = flat_symbol(lib_id)
        props = {p[1]: p for p in find(s, "property")}
        x, y, rot = at
        rp = ref_pos or (x + 2.54, y - 1.27, "left")
        vp = val_pos or (x + 2.54, y + 1.27, "left")
        fa = rot if rot in (90, 270) else 0  # field angles are relative to the symbol: keep text horizontal
        out = [Sym("symbol"), [Sym("lib_id"), lib_id], [Sym("at"), x, y, rot], [Sym("unit"), 1],
               [Sym("exclude_from_sim"), Sym("no")], [Sym("in_bom"), Sym("yes" if in_bom else "no")],
               [Sym("on_board"), Sym("yes")], [Sym("dnp"), Sym("no")], [Sym("uuid"), uid("sym", ref)],
               prop("Reference", ref, rp[0], rp[1], fa, hide_ref, rp[2] if not fa else "center"),
               prop("Value", value, vp[0], vp[1], fa, hide_val, vp[2] if not fa else "center"),
               prop("Footprint", footprint, x, y, 0, True),
               prop("Datasheet", props["Datasheet"][2] if "Datasheet" in props else "~", x, y, 0, True),
               prop("Description", props["Description"][2] if "Description" in props else "", x, y, 0, True)]
        for k, v in (fields or {}).items():
            out.append(prop(k, v, x, y, 0, True))
        seen = []
        for pin in sym_pins(lib_id):
            if pin[0] not in seen:
                seen.append(pin[0])
                out.append([Sym("pin"), pin[0], [Sym("uuid"), uid("pin", ref, pin[0])]])
        out.append([Sym("instances"), [Sym("project"), netdef.PROJECT,
                                       [Sym("path"), "/" + ROOT, [Sym("reference"), ref], [Sym("unit"), 1]]]])
        self.items.append(out)

    def power(self, net, p, d, key):
        """Power symbol whose pin is at p; d = direction away from the circuit (used to orient it)."""
        lib_id = netdef.POWER_SYMBOLS[net]
        self.pwr_n += 1
        ref = "#PWR%02d" % self.pwr_n
        if net == "GND":
            rot = {(0, 1): 0, (0, -1): 180, (1, 0): 90, (-1, 0): 270}[d]
        else:
            rot = {(0, -1): 0, (0, 1): 180, (1, 0): 270, (-1, 0): 90}[d]
        dist = 3.81 if d[1] else 6.35
        tp = (snap(p[0] + d[0] * dist), snap(p[1] + d[1] * dist))
        fa = rot if rot in (90, 270) else 0
        self.use_lib(lib_id)
        self.items.append([Sym("symbol"), [Sym("lib_id"), lib_id], [Sym("at"), p[0], p[1], rot], [Sym("unit"), 1],
                           [Sym("exclude_from_sim"), Sym("no")], [Sym("in_bom"), Sym("yes")],
                           [Sym("on_board"), Sym("yes")], [Sym("dnp"), Sym("no")],
                           [Sym("uuid"), uid("pwr", key)],
                           prop("Reference", ref, p[0], p[1], 0, True),
                           prop("Value", net, tp[0], tp[1], fa, False, "center"),
                           prop("Footprint", "", p[0], p[1], 0, True),
                           prop("Datasheet", "", p[0], p[1], 0, True),
                           prop("Description", "Power symbol creates a global label with name \"%s\"" % net,
                                p[0], p[1], 0, True),
                           [Sym("pin"), "1", [Sym("uuid"), uid("pwrpin", key)]],
                           [Sym("instances"), [Sym("project"), netdef.PROJECT,
                                               [Sym("path"), "/" + ROOT, [Sym("reference"), ref],
                                                [Sym("unit"), 1]]]]])
        self.touch(p)

    def pwr_flag(self, p, key):
        lib_id = "power:PWR_FLAG"
        self.flg_n += 1
        ref = "#FLG%02d" % self.flg_n
        self.use_lib(lib_id)
        self.items.append([Sym("symbol"), [Sym("lib_id"), lib_id], [Sym("at"), p[0], p[1], 0], [Sym("unit"), 1],
                           [Sym("exclude_from_sim"), Sym("no")], [Sym("in_bom"), Sym("yes")],
                           [Sym("on_board"), Sym("yes")], [Sym("dnp"), Sym("no")],
                           [Sym("uuid"), uid("flag", key)],
                           prop("Reference", ref, p[0], p[1] - 1.905, 0, True),
                           prop("Value", "PWR_FLAG", p[0], p[1] - 3.81, 0, False, "center"),
                           prop("Footprint", "", p[0], p[1], 0, True),
                           prop("Datasheet", "~", p[0], p[1], 0, True),
                           prop("Description", "Special symbol for telling ERC where power comes from",
                                p[0], p[1], 0, True),
                           [Sym("pin"), "1", [Sym("uuid"), uid("flagpin", key)]],
                           [Sym("instances"), [Sym("project"), netdef.PROJECT,
                                               [Sym("path"), "/" + ROOT, [Sym("reference"), ref],
                                                [Sym("unit"), 1]]]]])
        self.touch(p)

    def junction(self, p, key):
        self.items.append([Sym("junction"), [Sym("at"), p[0], p[1]], [Sym("diameter"), 0],
                           [Sym("color"), 0, 0, 0, 0], [Sym("uuid"), uid("junction", key)]])

    def document(self):
        libs = [Sym("lib_symbols")] + [flat_symbol(l) for l in self.lib_ids]
        tb = [Sym("title_block"), [Sym("title"), netdef.TITLE], [Sym("date"), netdef.DATE], [Sym("rev"), netdef.REV],
              [Sym("company"), netdef.COMPANY],
              [Sym("comment"), 1, "Firmware: ac_matter_ir (ESP-IDF + esp_matter), see README.md"],
              [Sym("comment"), 2, "Passives 0603 or larger, hand-solder pads"]]
        return [Sym("kicad_sch"), [Sym("version"), 20250114], [Sym("generator"), "eeschema"],
                [Sym("generator_version"), "9.0"], [Sym("uuid"), ROOT], [Sym("paper"), "A4"], tb, libs] + \
            self.items + [[Sym("sheet_instances"), [Sym("path"), "/", [Sym("page"), "1"]]],
                          [Sym("embedded_fonts"), Sym("no")]]


def ref_value_positions(ref, lib_id, at, pins):
    """Place Reference/Value next to the symbol body."""
    if ref in FIELDS:
        return FIELDS[ref]
    x, y, rot = at
    if ref.startswith("H"):
        return (x + 2.54, y - 1.27, "left"), (x + 2.54, y + 1.27, "left")
    if lib_id in ("Device:R", "Device:C", "Device:Polyfuse"):
        if rot in (0, 180):
            return (x + 2.54, y - 1.27, "left"), (x + 2.54, y + 1.27, "left")
        return (x, y - 2.54, "center"), (x, y + 2.54, "center")
    if lib_id in ("Device:LED", "Diode:1N4148W"):
        if rot in (90, 270):
            return (x + 7.62, y - 1.27, "center"), (x + 7.62, y + 1.27, "center")
        return (x, y - 3.81, "center"), (x, y + 3.81, "center")
    if lib_id == "Switch:SW_Push":
        if rot in (90, 270):
            return (x + 6.35, y - 1.27, "center"), (x + 6.35, y + 1.27, "center")
        return (x, y - 5.08, "center"), (x, y + 2.54, "center")
    xs = [xform(at, p[2], p[3])[0] for p in pins] or [x]
    ys = [xform(at, p[2], p[3])[1] for p in pins] or [y]
    return (min(xs), min(ys) - 3.81, "left"), (min(xs), max(ys) + 6.35, "left")


def build():
    sh = Sheet()
    wired = set()
    for w in WIRES:
        for pt in w["path"]:
            if isinstance(pt[0], str):
                wired.add(pt)

    pin_points = {}
    for ref, part in netdef.PARTS.items():
        at = SCH[ref]
        pins = sym_pins(part["lib"])
        rp, vp = ref_value_positions(ref, part["lib"], at, pins)
        sh.symbol(part["lib"], ref, part["value"], at, part["fp"], netdef.fields(ref),
                  in_bom=not ref.startswith(("H", "TP")), ref_pos=rp, val_pos=vp)
        by_point = {}
        for num_, name, lx, ly, ang, hidden in pins:
            p = xform(at, lx, ly)
            pin_points[(ref, num_)] = (p, xdir(at, ang))
            by_point.setdefault(p, []).append(num_)
        # stacked pins: one stub per location
        done = set()
        for num_, name, lx, ly, ang, hidden in pins:
            p, d = pin_points[(ref, num_)]
            if p in done:
                continue
            nums = by_point[p]
            nets_here = {part["pins"][n] for n in nums if n in part["pins"]}
            if len(nets_here) > 1:
                raise SystemExit("%s: stacked pins %s on different nets %s" % (ref, nums, nets_here))
            done.add(p)
            if not nets_here:
                sh.no_connect(p, (ref, num_))
                continue
            sh.touch(p)
            if any((ref, n) in wired and (ref, n) not in KEEP_STUB for n in nums):
                continue
            net = nets_here.pop()
            end = (snap(p[0] + d[0] * 2.54), snap(p[1] + d[1] * 2.54))
            sh.wire(p, end, ("stub", ref, num_))
            if net in netdef.POWER_SYMBOLS:
                sh.power(net, end, d, ("pin", ref, num_))
            else:
                sh.label(net, end, d, ("pin", ref, num_))

    for i, w in enumerate(WIRES):
        pts = [pin_points[pt][0] if isinstance(pt[0], str) else pt for pt in w["path"]]
        for a, b in zip(pts, pts[1:]):
            if a[0] != b[0] and a[1] != b[1]:
                raise SystemExit("WIRES %s: %s-%s not aligned" % (w["path"], a, b))
        if w.get("label"):
            name, d = w["label"]
            a, b = pts[0], pts[1]
            dist = math.hypot(b[0] - a[0], b[1] - a[1])
            t = w.get("at", dist / 2) / dist
            mid = (snap(a[0] + (b[0] - a[0]) * t), snap(a[1] + (b[1] - a[1]) * t))
            pts = [a, mid] + pts[1:]
            sh.label(name, mid, d, ("wire", i))
        for j, (a, b) in enumerate(zip(pts, pts[1:])):
            sh.wire(a, b, ("w", i, j))

    for net, (x, y) in FLAGS.items():
        p = (x, y)
        end = (snap(x + 10.16), y)
        sh.pwr_flag(p, net)
        sh.wire(p, end, ("flag", net))
        if net in netdef.POWER_SYMBOLS:
            sh.power(net, end, (0, 1) if net == "GND" else (0, -1), ("flag", net))
        else:
            sh.label(net, end, (1, 0), ("flag", net))

    for p, n in sh.points.items():
        if n >= 3:
            sh.junction(p, p)

    for i, (title, x1, y1, x2, y2) in enumerate(GROUPS):
        sh.rect(x1, y1, x2, y2, ("group", i))
        sh.text(title, x1 + 1.27, y1 + 3.81, 1.7, True, ("group", i))
    for k, (x, y, lines) in enumerate(NOTES):
        for i, line in enumerate(lines):
            sh.text(line, x, y + i * 3.81, 1.5, False, ("note", k, i))
    return sh.document()


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", netdef.PROJECT + ".kicad_sch")
    doc = build()
    with open(out, "w", encoding="utf-8") as f:
        f.write(dump(doc) + "\n")
    print("wrote", os.path.abspath(out))
