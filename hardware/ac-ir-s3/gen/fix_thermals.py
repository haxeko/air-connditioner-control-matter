#!/usr/bin/env python3
"""Give pads with a starved thermal relief (DRC 'starved_thermal') a solid connection to the pour.

Usage: python3 fix_thermals.py <board.kicad_pcb> <drc.json>
These GND pads already have their own via into the bottom pour; a solid connection on the top pour
replaces the relief spokes that tracks around the pad left no room for.
"""
import json
import re
import sys

import pcbnew


def main(board_path, drc_path):
    report = json.load(open(drc_path))
    want = set()
    for v in report["violations"]:
        if v["type"] != "starved_thermal":
            continue
        for it in v["items"]:
            m = re.match(r"Pad (\S+) \[[^\]]*\] of (\S+) on", it["description"])
            if m:
                want.add((m.group(2), m.group(1)))
    if not want:
        print("no starved thermal reliefs")
        return 0
    board = pcbnew.LoadBoard(board_path)
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if (fp.GetReference(), pad.GetNumber()) in want:
                pad.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
                print("solid zone connection: %s.%s" % (fp.GetReference(), pad.GetNumber()))
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    pcbnew.SaveBoard(board_path, board)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
