#!/usr/bin/env python3
"""Grid router for the 2-layer AC IR board (negotiated congestion), GND drop vias and zone fill.

Usage (system python3 with the KiCad 9 'pcbnew' module and numpy):
  python3 route.py <board.kicad_pcb>
The board is modified in place.

How it works
- The board is a 0.05 mm grid on two layers. Pads, holes, the board edge and component bodies that sit
  on the top copper are hard obstacles, inflated by clearance + half the track (or via) width.
- Nets are routed with A* (8 directions, vias between layers, bottom layer and vias cost more so the
  bottom stays a ground plane). Copper of other nets is not a hard obstacle at first: overlapping it
  costs a penalty that grows every iteration, plus a history cost on cells that were contested before
  (PathFinder). Nets in conflict are ripped up and rerouted until no copper of two nets is closer than
  the clearance. A final pass routes any remaining net with all other copper as hard obstacles.
- GND is not routed as a net: it is the copper pour on both layers, and every surface-mount GND pad
  gets a short track to its own via into the pour of the other layer.
- GND stitching: tracks on the bottom layer cut the ground plane into pieces. Every piece of pour that
  has no connection to the other layer gets a via (so no copper is thrown away as an island and return
  currents can change layer next to the cut), then a coarse grid of vias ties both pours together.
"""
import heapq
import math
import sys
import time

import numpy as np
import pcbnew

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netdef  # noqa: E402
from make_pcb import PLACE  # noqa: E402

OX, OY = 100.0, 100.0
W, H = netdef.BOARD_W, netdef.BOARD_H
CORNER = 1.0
G = 0.05                        # grid pitch (mm)
NX = int(round(W / G)) + 1
NY = int(round(H / G)) + 1
NC = NX * NY
CLEAR = 0.2                     # copper clearance (mm)
EDGE = 0.3                      # copper to board edge (mm)
MARGIN = 0.03                   # grid discretisation margin (mm)
CLASSES = {"Default": dict(w=0.25, via=0.6, drill=0.3), "Power": dict(w=0.5, via=0.8, drill=0.4)}
POWER_NETS = {"VBUS", "/PH_5V", "/VIN", "+5V", "+3V3", "/IR_A", "/IR_M", "/IR_K"}
LAYER_COST = (1.0, 1.5)         # prefer the top layer: the bottom layer is the GND plane
VIA_COST = 40.0                 # in grid steps
TURN_COST = 0.4
PRES0, PRES_MULT, HIST_INC = 0.6, 1.7, 1.5
MAX_ITER = 40
# Areas where a component body sits on the top copper: no top-layer signal tracks or vias there.
_ux, _uy, _ = PLACE["U1"]
_jx, _jy, _ = PLACE["J1"]
NO_TOP_MODULE = (_ux - 8.0, -1.0, _ux + 8.0, _uy + 11.75)   # module body inside its pad ring
NO_TOP_SHELL = (-1.0, _jy - 4.52, _jx + 2.4, _jy + 4.52)    # USB-C shell (pads start at x + 2.48)
# Under the module the top layer is one solid GND pour: GND vias (covered by solder mask) are welcome
# there, they tie the pour to the bottom plane on both sides of the tracks that cross under the module.
STITCH_PITCH = 3.0              # GND stitching grid (mm), rows staggered
STITCH_GAP = 2.2                # a stitching via stays this far from other GND vias and plated GND pads
STITCH_PAD_GAP = 0.25           # stitching via copper to GND pad copper (no via in or against a pad)
STITCH_MIN_AREA = 1.0           # pieces of pour smaller than this (mm2) are not worth a via
# GND via sites kept free of other nets before routing (offset from a footprint position): where a
# GND pad is boxed in by power tracks unless its drop via has a reserved place.
# LDO GND pin: it sits between the two +5 V pins, its via goes under the body next to the 2-pin row.
GND_VIA_SITES = [("U2", 0.0, -0.5)]
ORDER = ["/CC1", "/CC2", "VBUS", "/PH_5V", "/VIN", "+5V", "/EN", "/IR_TX", "/IR_RX",
         "/IR_A", "/IR_M", "/IR_K", "+3V3", "/IO0", "/IR_GATE", "/IR_VS",
         "/RGB", "/RGB_DIN", "/LED_VDD", "/TXD0", "/RXD0"]

# Pad order for nets whose tree should start at the source and reach the main load first.
PAD_ORDER = {"+3V3": [("U2", "5"), ("C3", "1"), ("U1", "2"), ("C4", "1")],
             "+5V": [("F1", "2"), ("C2", "1"), ("U2", "1")],
             "/VIN": [("F1", "1"), ("D5", "1"), ("D6", "1")],
             "VBUS": [("D5", "2"), ("J1", "B9"), ("J1", "A9")]}

XS = np.arange(NX) * G
YS = np.arange(NY) * G


def mm(v):
    return pcbnew.FromMM(v)


def to_mm(v):
    return pcbnew.ToMM(v)


def cls_of(net):
    return "Power" if net in POWER_NETS else "Default"


# ------------------------------------------------------------------ geometry items
class Item:
    """Copper (or hole) on some layers: kind rect(cx,cy,hx,hy) / circle(cx,cy,r) / seg(x1,y1,x2,y2,hw)."""

    def __init__(self, net, layers, kind, geo, pad=None):
        self.net, self.layers, self.kind, self.geo, self.pad = net, layers, kind, geo, pad

    def bbox(self):
        g = self.geo
        if self.kind == "rect":
            return g[0] - g[2], g[1] - g[3], g[0] + g[2], g[1] + g[3]
        if self.kind == "circle":
            return g[0] - g[2], g[1] - g[2], g[0] + g[2], g[1] + g[2]
        return min(g[0], g[2]) - g[4], min(g[1], g[3]) - g[4], max(g[0], g[2]) + g[4], max(g[1], g[3]) + g[4]

    def dist(self, X, Y):
        """Distance from points to the copper outline (<= 0 inside)."""
        g = self.geo
        if self.kind == "rect":
            dx = np.abs(X - g[0]) - g[2]
            dy = np.abs(Y - g[1]) - g[3]
            return np.hypot(np.maximum(dx, 0), np.maximum(dy, 0)) + np.minimum(np.maximum(dx, dy), 0)
        if self.kind == "circle":
            return np.hypot(X - g[0], Y - g[1]) - g[2]
        x1, y1, x2, y2, hw = g
        vx, vy = x2 - x1, y2 - y1
        L2 = vx * vx + vy * vy
        t = np.zeros_like(X) if L2 == 0 else np.clip(((X - x1) * vx + (Y - y1) * vy) / L2, 0, 1)
        return np.hypot(X - (x1 + t * vx), Y - (y1 + t * vy)) - hw

    def window(self, r):
        x0, y0, x1, y1 = self.bbox()
        return (max(0, int(math.floor((x0 - r) / G))), min(NX - 1, int(math.ceil((x1 + r) / G))),
                max(0, int(math.floor((y0 - r) / G))), min(NY - 1, int(math.ceil((y1 + r) / G))))

    def cells(self, r, layers=None):
        """Flat cell ids within distance r of the copper, per layer."""
        i0, i1, j0, j1 = self.window(r)
        X, Y = np.meshgrid(XS[i0:i1 + 1], YS[j0:j1 + 1])
        jj, ii = np.nonzero(self.dist(X, Y) < r)
        flat = (jj + j0) * NX + (ii + i0)
        return {l: flat for l in (layers if layers is not None else self.layers)}


def pad_item(pad):
    """Pad copper as a rect/circle/oval item in board-local mm (pads here are axis-aligned)."""
    pos = pad.GetPosition()
    cx, cy = to_mm(pos.x) - OX, to_mm(pos.y) - OY
    size = pad.GetSize(pcbnew.F_Cu)
    sx, sy = to_mm(size.x), to_mm(size.y)
    ang = round(pad.GetOrientation().AsDegrees()) % 180
    if ang == 90:
        sx, sy = sy, sx
    elif ang != 0:
        raise SystemExit("unexpected pad angle %s on %s" % (ang, pad.GetParentFootprint().GetReference()))
    layers = [i for i, l in enumerate((pcbnew.F_Cu, pcbnew.B_Cu)) if pad.IsOnLayer(l)]
    shape = pad.GetShape(pcbnew.F_Cu)
    net = pad.GetNetname()
    if pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
        return Item("#hole", [0, 1], "circle", (cx, cy, to_mm(pad.GetDrillSize().x) / 2), pad)
    if shape == pcbnew.PAD_SHAPE_CIRCLE:
        return Item(net, layers, "circle", (cx, cy, sx / 2), pad)
    if shape == pcbnew.PAD_SHAPE_OVAL and abs(sx - sy) > 1e-6:
        if sx > sy:
            return Item(net, layers, "seg", (cx - (sx - sy) / 2, cy, cx + (sx - sy) / 2, cy, sy / 2), pad)
        return Item(net, layers, "seg", (cx, cy - (sy - sx) / 2, cx, cy + (sy - sx) / 2, sx / 2), pad)
    return Item(net, layers, "rect", (cx, cy, sx / 2, sy / 2), pad)


# ------------------------------------------------------------------ occupancy maps
class Layer:
    """Per (layer, class, track|via) inflation counts: total and per net."""

    def __init__(self):
        self.tot = np.zeros((NY, NX), np.int16)
        self.per = {}

    def add(self, net, flat, sign):
        self.tot.ravel()[flat] += sign
        a = self.per.setdefault(net, np.zeros((NY, NX), np.int16))
        a.ravel()[flat] += sign

    def other(self, net):
        own = self.per.get(net)
        return self.tot - own if own is not None else self.tot


class Maps:
    def __init__(self):
        self.st = {(l, k, m): Layer() for l in (0, 1) for k in CLASSES for m in "tv"}   # pads, holes
        self.dy = {(l, k, m): Layer() for l in (0, 1) for k in CLASSES for m in "tv"}   # tracks, vias
        X, Y = np.meshgrid(XS, YS)
        x0, y0, x1, y1 = NO_TOP_SHELL
        self.noshell = (X >= x0) & (X <= x1) & (Y >= y0) & (Y <= y1)
        x0, y0, x1, y1 = NO_TOP_MODULE
        self.notop = self.noshell | ((X >= x0) & (X <= x1) & (Y >= y0) & (Y <= y1))
        ed = self.edge_dist(X, Y)
        self.edge_t = {k: ed < EDGE + c["w"] / 2 + MARGIN for k, c in CLASSES.items()}
        self.edge_v = {k: ed < EDGE + c["via"] / 2 + MARGIN for k, c in CLASSES.items()}

    @staticmethod
    def edge_dist(X, Y):
        d = np.minimum(np.minimum(X, W - X), np.minimum(Y, H - Y))
        for cx, cy in ((CORNER, CORNER), (W - CORNER, CORNER), (CORNER, H - CORNER), (W - CORNER, H - CORNER)):
            inq = (np.abs(X - cx) <= CORNER) & (np.abs(Y - cy) <= CORNER) & \
                  (((X < cx) if cx < W / 2 else (X > cx)) & ((Y < cy) if cy < H / 2 else (Y > cy)))
            d = np.where(inq, CORNER - np.hypot(X - cx, Y - cy), d)
        return d

    def paint(self, item, dyn, sign=1):
        maps = self.dy if dyn else self.st
        for k, c in CLASSES.items():
            for m, rad in (("t", c["w"] / 2), ("v", c["via"] / 2)):
                for l, flat in item.cells(CLEAR + rad + MARGIN).items():
                    maps[(l, k, m)].add(item.net, flat, sign)

    def hard(self, net, k, dyn_hard):
        """Hard-blocked track cells per layer and via cells for 'net' of class k."""
        notop = self.noshell if net == "GND" else self.notop
        tb = []
        for l in (0, 1):
            b = (self.st[(l, k, "t")].other(net) > 0) | self.edge_t[k]
            if dyn_hard:
                b |= self.dy[(l, k, "t")].other(net) > 0
            if l == 0:
                b |= notop
            tb.append(b)
        vb = self.edge_v[k] | notop
        for l in (0, 1):
            vb |= self.st[(l, k, "v")].other(net) > 0
            if dyn_hard:
                vb |= self.dy[(l, k, "v")].other(net) > 0
        return tb, vb

    def soft(self, net, k):
        """Number of other nets' tracks/vias too close, for a track per layer and for a via."""
        t = [self.dy[(l, k, "t")].other(net) for l in (0, 1)]
        v = self.dy[(0, k, "v")].other(net) + self.dy[(1, k, "v")].other(net)
        return t, v


# ------------------------------------------------------------------ A*
DIRS = [(1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.0), (0, -1, 1.0),
        (1, 1, 1.4142), (1, -1, 1.4142), (-1, 1, 1.4142), (-1, -1, 1.4142)]


def astar(sources, targets, tblock, vblock, win, pen=None, vpen=None):
    """sources/targets: sets of state ids (layer*NC + j*NX + i). Returns the state path or None."""
    i_lo, i_hi, j_lo, j_hi = win
    tb = [bytearray(tblock[0].ravel().astype(np.uint8)), bytearray(tblock[1].ravel().astype(np.uint8))]
    vb = bytearray(vblock.ravel().astype(np.uint8))
    tcells = [(s % NC) for s in targets]
    ti = [c % NX for c in tcells]
    tj = [c // NX for c in tcells]
    bx0, bx1, by0, by1 = min(ti), max(ti), min(tj), max(tj)

    def h(i, j):
        dx = max(bx0 - i, 0, i - bx1)
        dy = max(by0 - j, 0, j - by1)
        return (max(dx, dy) + 0.4142 * min(dx, dy)) * LAYER_COST[0]

    g = {}
    parent = {}
    pdir = {}
    heap = []
    for s in sources:
        c = s % NC
        g[s] = 0.0
        parent[s] = -1
        pdir[s] = -1
        heapq.heappush(heap, (h(c % NX, c // NX), 0.0, s))
    closed = set()
    while heap:
        f, gs, s = heapq.heappop(heap)
        if s in closed:
            continue
        if s in targets:
            path = []
            while s != -1:
                path.append(s)
                s = parent[s]
            return path[::-1]
        closed.add(s)
        layer, c = divmod(s, NC)
        i, j = c % NX, c // NX
        lc = LAYER_COST[layer]
        d0 = pdir[s]
        tbl = tb[layer]
        pl = pen[layer] if pen is not None else None
        for di, (dx, dy, step) in enumerate(DIRS):
            ni, nj = i + dx, j + dy
            if ni < i_lo or ni > i_hi or nj < j_lo or nj > j_hi:
                continue
            nc = nj * NX + ni
            ns = layer * NC + nc
            if ns in closed:
                continue
            if tbl[nc] and ns not in targets:
                continue
            if dx and dy and tbl[j * NX + ni] and tbl[nj * NX + i]:
                continue  # no corner cutting between blocked cells
            ng = gs + step * lc + (TURN_COST if d0 >= 0 and d0 != di else 0.0)
            if pl is not None:
                ng += pl[nc] * step
            if ng < g.get(ns, 1e18):
                g[ns] = ng
                parent[ns] = s
                pdir[ns] = di
                heapq.heappush(heap, (ng + h(ni, nj), ng, ns))
        if not vb[c]:
            other = (1 - layer) * NC + c
            if other not in closed and not tb[1 - layer][c]:
                ng = gs + VIA_COST + (vpen[c] if vpen is not None else 0.0)
                if ng < g.get(other, 1e18):
                    g[other] = ng
                    parent[other] = s
                    pdir[other] = -1
                    heapq.heappush(heap, (ng + h(i, j), ng, other))
    return None


# ------------------------------------------------------------------ router
class Router:
    def __init__(self, board):
        self.board = board
        self.maps = Maps()
        self.pads = {}                      # net -> [Item]
        # Fixed order (reference, pad number, position): the file order follows random UUIDs, and the
        # routing result must not change from one run to the next.
        for fp in sorted(board.GetFootprints(), key=lambda f: f.GetReference()):
            for pad in sorted(fp.Pads(), key=lambda p: (p.GetNumber(), p.GetPosition().x, p.GetPosition().y)):
                it = pad_item(pad)
                self.maps.paint(it, dyn=False)
                if it.net and not it.net.startswith("#") and not it.net.startswith("unconnected-"):
                    self.pads.setdefault(it.net, []).append(it)
        for ref, dx, dy in GND_VIA_SITES:  # reserved like a GND via: other nets route around it
            x, y, _ = PLACE[ref]
            self.maps.paint(Item("GND", [0, 1], "circle", (x + dx, y + dy, CLASSES["Default"]["via"] / 2)), dyn=False)
        self.netinfo = {n.GetNetname(): n for n in board.GetNetsByName().values()}
        self.routed = {}                    # net -> [(Item, board object)]
        self.trace = {}                     # net -> [(kind, layer, cell)] centreline cells and via cells
        self.hist = [np.zeros((NY, NX), np.float32), np.zeros((NY, NX), np.float32)]
        self.pres = PRES0

    # ---- copper creation / removal
    def add_track(self, net, layer, a, b, w):
        t = pcbnew.PCB_TRACK(self.board)
        t.SetStart(pcbnew.VECTOR2I(mm(OX + a[0]), mm(OY + a[1])))
        t.SetEnd(pcbnew.VECTOR2I(mm(OX + b[0]), mm(OY + b[1])))
        t.SetWidth(mm(w))
        t.SetLayer((pcbnew.F_Cu, pcbnew.B_Cu)[layer])
        t.SetNet(self.netinfo[net])
        self.board.Add(t)
        it = Item(net, [layer], "seg", (a[0], a[1], b[0], b[1], w / 2))
        self.maps.paint(it, dyn=True)
        self.routed.setdefault(net, []).append((it, t))

    def add_via(self, net, p, k):
        c = CLASSES[k]
        v = pcbnew.PCB_VIA(self.board)
        v.SetPosition(pcbnew.VECTOR2I(mm(OX + p[0]), mm(OY + p[1])))
        v.SetWidth(mm(c["via"]))
        v.SetDrill(mm(c["drill"]))
        v.SetNet(self.netinfo[net])
        self.board.Add(v)
        it = Item(net, [0, 1], "circle", (p[0], p[1], c["via"] / 2))
        self.maps.paint(it, dyn=True)
        self.routed.setdefault(net, []).append((it, v))

    def rip(self, net):
        for it, obj in self.routed.pop(net, []):
            self.maps.paint(it, dyn=True, sign=-1)
            self.board.Remove(obj)
        self.trace.pop(net, None)

    def emit(self, net, path, k):
        w = CLASSES[k]["w"]
        tr = self.trace.setdefault(net, [])
        pts = []
        for s in path:
            l, c = divmod(s, NC)
            pts.append((l, (c % NX) * G, (c // NX) * G))
            tr.append(("t", l, c))
        run = [pts[0]]
        for idx, p in enumerate(pts[1:], 1):
            q = run[-1]
            if p[0] != q[0]:
                self.flush(net, run, w)
                self.add_via(net, (p[1], p[2]), k)
                tr.append(("v", -1, path[idx] % NC))
                run = [p]
                continue
            if len(run) >= 2:
                a = run[-2]
                d1 = (round((q[1] - a[1]) / G), round((q[2] - a[2]) / G))
                d2 = (round((p[1] - q[1]) / G), round((p[2] - q[2]) / G))
                if d1 != d2:
                    self.flush(net, run, w)
                    run = [q]
            run.append(p)
        self.flush(net, run, w)

    def flush(self, net, run, w):
        if len(run) >= 2:
            self.add_track(net, run[0][0], (run[0][1], run[0][2]), (run[-1][1], run[-1][2]), w)

    # ---- routing
    def allowed_cells(self, it, w):
        """Cells inside pad 'it' where a track of width w stays inside the pad copper."""
        i0, i1, j0, j1 = it.window(0)
        X, Y = np.meshgrid(XS[i0:i1 + 1], YS[j0:j1 + 1])
        d = it.dist(X, Y)
        ok = d <= -w / 2 + 1e-6
        if not ok.any():
            ok = d <= d.min() + 1e-6
        jj, ii = np.nonzero(ok)
        cells = (jj + j0) * NX + (ii + i0)
        return [(l, int(c)) for l in it.layers for c in cells]

    def route_net(self, net, soft):
        k = cls_of(net)
        w = CLASSES[k]["w"]
        pads = self.pads[net]
        if len(pads) < 2:
            return True
        cells = [set(l * NC + c for l, c in self.allowed_cells(p, w)) for p in pads]
        tblock, vblock = self.maps.hard(net, k, dyn_hard=not soft)
        vr = CLASSES[k]["via"] / 2
        for p in pads:                       # no vias inside surface-mount pads
            if p.pad.GetAttribute() == pcbnew.PAD_ATTRIB_SMD:
                for l, flat in p.cells(vr, [0]).items():
                    vblock.ravel()[flat] = True
        pen = vpen = None
        if soft:
            t, v = self.maps.soft(net, k)
            pen = [(self.pres * t[l] + self.hist[l]).ravel().tolist() for l in (0, 1)]
            vpen = (self.pres * v + self.hist[0] + self.hist[1]).ravel().tolist()
        names = [(p.pad.GetParentFootprint().GetReference(), p.pad.GetNumber()) for p in pads]
        prefer = [names.index(x) for x in PAD_ORDER.get(net, []) if x in names]
        cx = sum(p.geo[0] for p in pads) / len(pads)
        cy = sum(p.geo[1] for p in pads) / len(pads)
        first = prefer[0] if prefer else \
            min(range(len(pads)), key=lambda n: math.hypot(pads[n].geo[0] - cx, pads[n].geo[1] - cy))
        tree = set(cells[first])
        todo = [n for n in range(len(pads)) if n != first]
        prefer = [n for n in prefer[1:]]
        ok = True
        while todo:
            prefer = [m for m in prefer if m in todo]
            if prefer:
                n = prefer[0]
            else:
                sample = list(tree)[::5]
                tx = [((s % NC) % NX) * G for s in sample]
                ty = [((s % NC) // NX) * G for s in sample]
                n = min(todo, key=lambda m: min(math.hypot(pads[m].geo[0] - x, pads[m].geo[1] - y)
                                                for x, y in zip(tx, ty)))
            tb = [tblock[0].copy(), tblock[1].copy()]
            for s in tree | cells[n]:        # own pads and own copper are never blocked
                l, c = divmod(s, NC)
                tb[l].ravel()[c] = False
            path = None
            for pad_mm in (5.0, 12.0, 100.0):
                pr = int(pad_mm / G)
                allc = [s % NC for s in tree | cells[n]]
                ii = [c % NX for c in allc]
                jj = [c // NX for c in allc]
                win = (max(0, min(ii) - pr), min(NX - 1, max(ii) + pr), max(0, min(jj) - pr), min(NY - 1, max(jj) + pr))
                path = astar(tree, cells[n], tb, vblock, win, pen, vpen)
                if path:
                    break
            todo.remove(n)
            if not path:
                print("    failed %s -> %s.%s" % (net, pads[n].pad.GetParentFootprint().GetReference(),
                                                  pads[n].pad.GetNumber()))
                ok = False
                continue
            self.emit(net, path, k)
            tree |= set(path) | cells[n]
            for m in list(todo):            # pads crossed by the path are connected too
                if cells[m] & set(path):
                    tree |= cells[m]
                    todo.remove(m)
        return ok

    def conflicts(self, net):
        """Cells where this net's copper is closer than the clearance to another net's tracks/vias."""
        k = cls_of(net)
        t, v = self.maps.soft(net, k)
        out = []
        for kind, l, c in self.trace.get(net, []):
            if kind == "t":
                if t[l].ravel()[c] > 0:
                    out.append((l, c))
            elif v.ravel()[c] > 0:
                out.append((0, c))
                out.append((1, c))
        return out

    def negotiate(self, order):
        t0 = time.time()
        nets = [n for n in order if n in self.pads]
        todo = list(nets)
        for it in range(MAX_ITER):
            for net in todo:
                self.rip(net)
                self.route_net(net, soft=True)
            conf = {n: self.conflicts(n) for n in nets}
            bad = [n for n in nets if conf[n]]
            print("  iteration %2d: %2d nets in conflict, %5d cells  (%.0f s)" %
                  (it + 1, len(bad), sum(len(v) for v in conf.values()), time.time() - t0), flush=True)
            if not bad:
                return []
            for n in bad:
                for l, c in conf[n]:
                    self.hist[l].ravel()[c] += HIST_INC
            self.pres *= PRES_MULT
            todo = bad
        # last resort: hard obstacles for the nets still in conflict
        failed = []
        for net in bad:
            self.rip(net)
        for net in bad:
            if not self.route_net(net, soft=False):
                failed.append(net)
        return failed

    def gnd_copper(self, layer, window):
        """GND vias and plated GND pads touching 'layer' inside the window: (cells inside them, via centres)."""
        i0, i1, j0, j1 = window
        cells, centres = set(), []
        items = [it for it, obj in self.routed.get("GND", []) if it.kind == "circle"]
        centres = [(it.geo[0], it.geo[1]) for it in items]
        items += [it for it in self.pads.get("GND", []) if it.pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH]
        for it in items:
            if layer not in it.layers:
                continue
            for l, flat in it.cells(0, [layer]).items():
                for c in flat:
                    i, j = int(c) % NX, int(c) // NX
                    if i0 <= i <= i1 and j0 <= j <= j1:
                        cells.add(layer * NC + int(c))
        return cells, centres

    def gnd_drops(self, reach=3.5, via_gap=0.9):
        """Every surface-mount GND pad gets a short track on its own layer to a via into the pour of the
        other layer (a new via, or an existing GND via / plated GND pad nearby)."""
        k = "Default"
        w = CLASSES[k]["w"]
        vr = CLASSES[k]["via"] / 2
        placed = shared = failed = 0
        seen = set()
        smd = [it for it in self.pads.get("GND", []) if it.pad.GetAttribute() == pcbnew.PAD_ATTRIB_SMD]
        for it in smd:
            pad = it.pad
            layer = it.layers[0]
            key = (layer, round(it.geo[0], 3), round(it.geo[1], 3))
            if key in seen:
                continue                     # stacked pads share one via
            seen.add(key)
            ref = pad.GetParentFootprint().GetReference()
            if ref == "U1" and pad.GetNumber() == "41":
                continue                     # exposed pad has its own thermal vias
            if any(o.pad.GetParentFootprint().GetReference() == ref and
                   o.pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH for o in self.pads["GND"]):
                continue                     # connector GND pins: tied by its plated shell pins and the pour
            tblock, vblock = self.maps.hard("GND", k, dyn_hard=True)
            src = set(layer * NC + c for l, c in self.allowed_cells(it, w) if l == layer)
            for s_ in src:
                tblock[layer].ravel()[s_ % NC] = False
            win = it.window(reach)
            i0, i1, j0, j1 = win
            X, Y = np.meshgrid(XS[i0:i1 + 1], YS[j0:j1 + 1])
            d = it.dist(X, Y)
            ok = (d > vr) & (d < reach)
            ok &= ~(vblock[j0:j1 + 1, i0:i1 + 1] | tblock[0][j0:j1 + 1, i0:i1 + 1] | tblock[1][j0:j1 + 1, i0:i1 + 1])
            for o in smd:                    # no via in another GND pad either
                if o is not it:
                    ok &= o.dist(X, Y) > vr
            old, centres = self.gnd_copper(layer, win)
            for cx, cy in centres:           # keep new vias apart from existing GND vias
                ok &= np.hypot(X - cx, Y - cy) >= via_gap
            jj, ii = np.nonzero(ok)
            new_t = set(int(layer * NC + (j + j0) * NX + (i + i0)) for j, i in zip(jj, ii))
            no_via = np.ones_like(vblock)    # the track itself stays on the pad's layer
            path = astar(src, new_t | old, tblock, no_via, win) if (new_t or old) else None
            if not path:
                print("  no room for a GND via at %s.%s" % (ref, pad.GetNumber()))
                failed += 1
                continue
            end = path[-1]
            self.emit("GND", path, k)
            if end in old:
                shared += 1
            else:
                c = end % NC
                self.add_via("GND", ((c % NX) * G, (c // NX) * G), k)
                placed += 1
        print("GND drop vias: %d new, %d pads joined to an existing GND via, %d without room" % (placed, shared, failed))

    # ---- GND stitching
    def gnd_links(self):
        """Centres of everything that joins the two GND pours: GND vias and plated GND pads."""
        pts = [(it.geo[0], it.geo[1]) for it, obj in self.routed.get("GND", []) if it.kind == "circle"]
        for it in self.pads.get("GND", []):
            if it.pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH:
                x0, y0, x1, y1 = it.bbox()
                pts.append(((x0 + x1) / 2, (y0 + y1) / 2))
        return pts

    def stitch_ok(self):
        """Cells where a stitching via is clear of every other net on both layers, of GND pads, of the
        USB-C shell and of the bodies of the top-side parts (the module excepted)."""
        k = "Default"
        vr = CLASSES[k]["via"] / 2
        bad = self.maps.edge_v[k] | self.maps.noshell
        for l in (0, 1):
            bad |= self.maps.st[(l, k, "v")].other("GND") > 0
            bad |= self.maps.dy[(l, k, "v")].other("GND") > 0
        for it in self.pads.get("GND", []):
            for flat in it.cells(vr + STITCH_PAD_GAP, [0]).values():
                bad.ravel()[flat] = True
        X, Y = np.meshgrid(XS, YS)
        for fp in self.board.GetFootprints():
            if fp.GetReference() == "U1":
                continue
            side = pcbnew.F_CrtYd if fp.GetLayer() == pcbnew.F_Cu else pcbnew.B_CrtYd
            bb = fp.GetCourtyard(side).BBox()
            if bb.GetWidth() <= 0:
                continue
            x0, y0 = to_mm(bb.GetX()) - OX, to_mm(bb.GetY()) - OY
            x1, y1 = x0 + to_mm(bb.GetWidth()), y0 + to_mm(bb.GetHeight())
            bad |= (X >= x0 - vr) & (X <= x1 + vr) & (Y >= y0 - vr) & (Y <= y1 + vr)
        return ~bad

    def pour_mask(self, layer):
        """Cells the GND pour of a layer can reach: clear of the copper of every other net."""
        k = "Default"
        return ~((self.maps.st[(layer, k, "t")].other("GND") > 0) |
                 (self.maps.dy[(layer, k, "t")].other("GND") > 0) | self.maps.edge_t[k])

    @staticmethod
    def label(mask):
        """4-connected regions of a boolean grid: (labels, count); label 0 = outside the mask."""
        free = bytearray(mask.ravel().astype(np.uint8))
        lab = [0] * NC
        n = 0
        for start in np.flatnonzero(mask.ravel()).tolist():
            if lab[start]:
                continue
            n += 1
            lab[start] = n
            stack = [start]
            while stack:
                c = stack.pop()
                i = c % NX
                for nb in (c - NX, c + NX, c - 1 if i else -1, c + 1 if i < NX - 1 else -1):
                    if 0 <= nb < NC and free[nb] and not lab[nb]:
                        lab[nb] = n
                        stack.append(nb)
        return np.array(lab, np.int32).reshape(NY, NX), n

    def gnd_stitch(self):
        """Vias for the pieces of pour without a connection to the other layer, then a grid of vias."""
        k = "Default"
        X, Y = np.meshgrid(XS, YS)
        repaired = 0
        for _ in range(4):                   # a new via can leave a piece of the other layer unconnected
            added = 0
            for layer in (1, 0):
                lab, n = self.label(self.pour_mask(layer))
                area = np.bincount(lab.ravel(), minlength=n + 1) * G * G
                linked = set()
                for x, y in self.gnd_links():
                    linked.add(int(lab[int(round(y / G)), int(round(x / G))]))
                ok = self.stitch_ok()
                for r in range(1, n + 1):
                    if r in linked or area[r] < STITCH_MIN_AREA:
                        continue
                    sel = (lab == r) & ok
                    if not sel.any():
                        if not _:            # reported once
                            print("  GND pour piece of %.1f mm2 on %s around (%.1f, %.1f) has no room for a via" %
                                  (area[r], ("F.Cu", "B.Cu")[layer], X[lab == r].mean(), Y[lab == r].mean()))
                        continue
                    cx, cy = X[sel].mean(), Y[sel].mean()
                    d = np.where(sel, np.hypot(X - cx, Y - cy), 1e9)
                    j, i = np.unravel_index(np.argmin(d), d.shape)
                    self.add_via("GND", (float(i) * G, float(j) * G), k)
                    added += 1
            repaired += added
            if not added:
                break
        grid = 0
        ok = self.stitch_ok()
        links = self.gnd_links()
        snap = int(round(0.75 / G))
        row = 0
        y = 1.0
        while y < H - 0.5:
            x = 1.0 + (STITCH_PITCH / 2 if row % 2 else 0.0)
            while x < W - 0.5:
                i0, j0 = int(round(x / G)), int(round(y / G))
                a, b = max(0, i0 - snap), min(NX - 1, i0 + snap)
                c, d_ = max(0, j0 - snap), min(NY - 1, j0 + snap)
                sub = ok[c:d_ + 1, a:b + 1].copy()
                if sub.any():
                    SX, SY = X[c:d_ + 1, a:b + 1], Y[c:d_ + 1, a:b + 1]
                    for lx, ly in links:
                        sub &= np.hypot(SX - lx, SY - ly) >= STITCH_GAP
                if sub.any():
                    dist = np.where(sub, np.hypot(SX - x, SY - y), 1e9)
                    j, i = np.unravel_index(np.argmin(dist), dist.shape)
                    px, py = float(SX[j, i]), float(SY[j, i])
                    self.add_via("GND", (px, py), k)
                    links.append((px, py))
                    grid += 1
                x += STITCH_PITCH
            y += STITCH_PITCH * 0.866
            row += 1
        print("GND stitching vias: %d for unconnected pieces of pour, %d on the %.1f mm grid" %
              (repaired, grid, STITCH_PITCH))


def main(path):
    board = pcbnew.LoadBoard(path)
    for t in list(board.GetTracks()):
        board.Remove(t)
    r = Router(board)
    missing = [n for n in r.pads if n not in ORDER and n != "GND"]
    if missing:
        raise SystemExit("nets not in ORDER: %s" % missing)
    failed = r.negotiate(ORDER)
    r.gnd_drops()                            # a pad left without a via shows up in the DRC afterwards
    r.gnd_stitch()
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    pcbnew.SaveBoard(path, board)
    print("saved %s; unrouted nets: %s" % (path, failed or "none"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
