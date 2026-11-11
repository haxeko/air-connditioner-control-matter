#!/usr/bin/env python3
"""Build ac_ir_s3.kicad_pcb from the schematic netlist (placement, outline, GND pours, silkscreen).

Usage (system python3 with the KiCad 9 'pcbnew' module):
  python3 make_pcb.py <netlist.net> <out.kicad_pcb>
Routing is done afterwards by route.py (grid A* router, GND drop vias, zone fill).
"""
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netdef  # noqa: E402
from sexp import find, first, parse  # noqa: E402

from local_lib import FPDIR, LOCAL_LIB, PROJDIR  # noqa: E402  (project footprint library ac_ir_s3.pretty)

OX, OY = 100.0, 100.0           # board origin on the page (mm)
W, H = netdef.BOARD_W, netdef.BOARD_H
CORNER = 1.0                    # outline corner radius

# ref: (x, y, rotation in degrees CCW), board-local mm, origin = top-left corner of the board.
# The module antenna overhangs the top edge (y = 0 is the module's antenna boundary).
PLACE = {
    "U1": (15.0, 6.75, 0),
    # left strip: 3V3 decoupling (C3 is also the LDO output capacitor), LDO, its input capacitor right
    # under the input pin, EN power-on reset RC. The parts keep to the left so that the signals of the
    # module's left pins have a corridor on the top layer between the strip and the module pads.
    "C3": (1.3, 2.3, 90), "C4": (3.4, 2.3, 90),
    "U2": (2.3, 6.9, 90),
    "C2": (1.3, 11.0, 270),
    "R1": (1.3, 15.2, 90), "C5": (3.3, 15.2, 270),      # C5 GND pad faces the free area below
    # right strip: BOOT button
    "SW1": (27.4, 16.4, 90),
    # programming pads on the bottom side, under the module: EN 3V3 TX RX GND IO0
    "TP6": (7.38, 15.6, 0),
    "TP1": (9.92, 15.6, 0), "TP2": (12.46, 15.6, 0), "TP3": (15.0, 15.6, 0),
    "TP4": (17.54, 15.6, 0), "TP5": (20.08, 15.6, 0),
    # power inputs: USB-C (left edge, mouth facing left) and PH2.0 (right edge)
    "J1": (3.2, 26.2, 270), "J2": (26.4, 27.7, 90),
    "D5": (12.4, 21.9, 180), "F1": (17.9, 21.9, 0), "D1": (22.85, 21.9, 0),
    "R3": (9.7, 24.4, 0), "R4": (9.7, 27.6, 0),
    "D4": (15.0, 25.6, 0), "C7": (18.9, 25.3, 90), "D6": (22.0, 25.6, 90),
    "R9": (15.0, 29.4, 0), "R7": (9.7, 30.0, 0),
    # IR receiver supply filter, between the IR LEDs
    "R5": (13.1, 32.6, 0), "C6": (16.8, 33.3, 0),
    # IR driver in the bottom corners
    "R6": (2.3, 32.5, 0), "Q1": (2.3, 35.6, 270),
    "R8": (26.6, 34.6, 90), "C8": (28.85, 34.6, 90),
    # bottom edge: IR LED (D3) - receiver (U4) - IR LED (D2), all facing down (+y)
    "D3": (6.62, 32.9, 0), "U4": (12.46, 36.4, 0), "D2": (20.83, 32.9, 0),
}
BOTTOM = {"TP1", "TP2", "TP3", "TP4", "TP5", "TP6"}

SILK = [  # text, x, y, size, layer, justify
    ("BOOT", 27.4, 12.4, 0.8, "F.SilkS", "center"),
    ("5V", 26.4, 22.55, 0.8, "F.SilkS", "center"),       # above the PH2.0 5 V pin
    ("5V", 28.8, 25.7, 0.8, "B.SilkS", "center"),        # PH2.0 pins, bottom side
    ("GND", 28.8, 27.7, 0.8, "B.SilkS", "center"),
    # bottom side: programming pad names and board name
    ("EN", 7.38, 13.9, 0.8, "B.SilkS", "center"),
    ("3V3", 9.92, 13.9, 0.8, "B.SilkS", "center"),
    ("TX", 12.46, 13.9, 0.8, "B.SilkS", "center"),
    ("RX", 15.0, 13.9, 0.8, "B.SilkS", "center"),
    ("GND", 17.54, 13.9, 0.8, "B.SilkS", "center"),
    ("IO0", 20.08, 13.9, 0.8, "B.SilkS", "center"),
    ("AC IR Matter S3", 15.0, 2.6, 1.2, "B.SilkS", "center"),
    ("ESP32-S3-WROOM-1-N16R8", 15.0, 4.4, 0.8, "B.SilkS", "center"),
    ("rev %s  5V USB-C / PH2.0" % netdef.REV, 15.0, 5.9, 0.8, "B.SilkS", "center"),
]


def mm(v):
    return pcbnew.FromMM(v)


def pt(x, y):
    return pcbnew.VECTOR2I(mm(OX + x), mm(OY + y))


def read_netlist(path):
    net = parse(open(path, encoding="utf-8").read())
    comps = {}
    for c in find(first(net, "components"), "comp"):
        ref = first(c, "ref")[1]
        comps[ref] = dict(value=first(c, "value")[1], footprint=first(c, "footprint")[1],
                          tstamp=first(c, "tstamps")[1], sheet=first(first(c, "sheetpath"), "tstamps")[1])
    nets = []
    for n in find(first(net, "nets"), "net"):
        nets.append((first(n, "name")[1], [(first(x, "ref")[1], first(x, "pin")[1]) for x in find(n, "node")]))
    return comps, nets


def add_outline(board):
    layer = pcbnew.Edge_Cuts
    r = CORNER
    segs = [((r, 0), (W - r, 0)), ((W, r), (W, H - r)), ((W - r, H), (r, H)), ((0, H - r), (0, r))]
    for (x1, y1), (x2, y2) in segs:
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
        s.SetLayer(layer)
        s.SetWidth(mm(0.1))
        s.SetStart(pt(x1, y1))
        s.SetEnd(pt(x2, y2))
        board.Add(s)
    k = r - r / 2 ** 0.5
    arcs = (((0, r), (k, k), (r, 0)), ((W - r, 0), (W - k, k), (W, r)),
            ((W, H - r), (W - k, H - k), (W - r, H)), ((r, H), (k, H - k), (0, H - r)))
    for (sx, sy), (mx, my), (ex, ey) in arcs:
        a = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_ARC)
        a.SetLayer(layer)
        a.SetWidth(mm(0.1))
        a.SetArcGeometry(pt(sx, sy), pt(mx, my), pt(ex, ey))
        board.Add(a)


def add_zone(board, net, layer, priority=0):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(net)
    z.SetAssignedPriority(priority)
    z.SetLocalClearance(mm(0.25))
    z.SetMinThickness(mm(0.25))   # no copper necks narrower than 0.25 mm in the pour
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetThermalReliefGap(mm(0.3))
    z.SetThermalReliefSpokeWidth(mm(0.4))
    z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
    o = z.Outline()
    o.NewOutline()
    for x, y in ((0, 0), (W, 0), (W, H), (0, H)):
        o.Append(mm(OX + x), mm(OY + y))
    z.SetZoneName("GND_" + pcbnew.LayerName(layer).replace(".", ""))
    board.Add(z)
    return z


def add_text(board, text, x, y, size, layer, justify):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(text)
    t.SetLayer({"F.SilkS": pcbnew.F_SilkS, "B.SilkS": pcbnew.B_SilkS}[layer])
    t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
    t.SetTextThickness(mm(max(0.12, size * 0.15)))
    t.SetPosition(pt(x, y))
    t.SetHorizJustify({"left": pcbnew.GR_TEXT_H_ALIGN_LEFT, "right": pcbnew.GR_TEXT_H_ALIGN_RIGHT,
                       "center": pcbnew.GR_TEXT_H_ALIGN_CENTER}[justify])
    if layer.startswith("B."):
        t.SetMirrored(True)
    board.Add(t)


def build(netlist, out):
    comps, nets = read_netlist(netlist)
    board = pcbnew.NewBoard(out)
    ds = board.GetDesignSettings()
    ds.SetCopperLayerCount(2)
    board.SetCopperLayerCount(2)
    # drill/place origin = bottom-left board corner, so Gerber, drill and CPL coordinates start at 0,0
    ds.SetAuxOrigin(pt(0, H))
    ds.SetGridOrigin(pt(0, H))

    netinfo = {}
    for name, _ in nets:
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        netinfo[name] = ni
    pin_net = {}
    for name, nodes in nets:
        for ref, pin in nodes:
            pin_net[(ref, pin)] = name

    for ref, c in sorted(comps.items()):
        lib, name = c["footprint"].split(":")
        libdir = PROJDIR if lib == LOCAL_LIB else FPDIR
        fp = pcbnew.FootprintLoad(os.path.join(libdir, lib + ".pretty"), name)
        if fp is None:
            raise SystemExit("footprint not found: " + c["footprint"])
        fp.SetFPID(pcbnew.LIB_ID(lib, name))
        fp.SetReference(ref)
        fp.SetValue(c["value"])
        fp.SetPath(pcbnew.KIID_PATH(c["sheet"] + c["tstamp"]))
        fp.SetSheetname("/")
        fp.SetSheetfile("ac_ir_s3.kicad_sch")
        x, y, rot = PLACE[ref]
        fp.SetPosition(pt(x, y))
        fp.SetOrientationDegrees(rot)
        for pad in fp.Pads():
            n = pin_net.get((ref, pad.GetNumber()))
            if n:
                pad.SetNet(netinfo[n])
        board.Add(fp)
        if ref in BOTTOM:                   # programming pads: flip to the bottom side
            fp.Flip(pt(x, y), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
            fp.SetPosition(pt(x, y))
        # References live on the assembly drawing (F.Fab ${REFERENCE} text); the silkscreen keeps only
        # functional labels so the compact board stays readable.
        fp.Reference().SetVisible(False)
        fp.Value().SetVisible(False)

    add_outline(board)
    gnd = netinfo["GND"]
    add_zone(board, gnd, pcbnew.F_Cu)
    add_zone(board, gnd, pcbnew.B_Cu)
    for text, x, y, size, layer, just in SILK:
        add_text(board, text, x, y, size, layer, just)
    pcbnew.SaveBoard(out, board)
    print("wrote", out, "-", len(comps), "footprints,", len(nets), "nets")


if __name__ == "__main__":
    build(sys.argv[1], sys.argv[2])
