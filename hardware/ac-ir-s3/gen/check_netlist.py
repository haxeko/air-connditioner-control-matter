#!/usr/bin/env python3
"""Compare a KiCad netlist (kicadsexpr) with netdef.py. Exit 1 on any difference."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netdef  # noqa: E402
from sexp import find, first, parse  # noqa: E402

net = parse(open(sys.argv[1], encoding="utf-8").read())
got = {}
for n in find(first(net, "nets"), "net"):
    name = first(n, "name")[1]
    name = name[1:] if name.startswith("/") else name  # local labels are prefixed with the sheet path
    nodes = sorted((first(x, "ref")[1], first(x, "pin")[1]) for x in find(n, "node"))
    got[name] = nodes
want = netdef.nets()
bad = 0
for name, nodes in want.items():
    if got.get(name) != nodes:
        print("MISMATCH", name, "want", nodes, "got", got.get(name))
        bad += 1
for name, nodes in got.items():
    if name not in want:
        # unconnected pins appear as single-node nets named unconnected-(...)
        if name.startswith("unconnected-") and len(nodes) == 1:
            continue
        print("EXTRA", name, nodes)
        bad += 1
comps = {first(c, "ref")[1]: (first(c, "value")[1], first(c, "footprint")[1] if first(c, "footprint") else "")
         for c in find(first(net, "components"), "comp")}
for ref, p in netdef.PARTS.items():
    if comps.get(ref) != (p["value"], p["fp"]):
        print("COMPONENT", ref, comps.get(ref), "want", (p["value"], p["fp"]))
        bad += 1
print("nets ok" if not bad else "%d problems" % bad, "-", len(want), "nets,", len(comps), "components")
sys.exit(1 if bad else 0)
