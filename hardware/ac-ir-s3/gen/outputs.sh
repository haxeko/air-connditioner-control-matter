#!/bin/bash
# Fabrication and documentation outputs for ac_ir_s3 (KiCad 9 kicad-cli).
# Usage: gen/outputs.sh   (run from hardware/ac-ir-s3)
set -euo pipefail
cd "$(dirname "$0")/.."
P=ac_ir_s3
REV=v$(python3 -c "import sys; sys.path.insert(0, 'gen'); import netdef; print(netdef.REV)")
FAB=fab
DOC=docs
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$FAB" "$DOC"
rm -f "$FAB/${P}"_v* "$DOC/${P}"_v*      # only the current revision is kept

# Checks first: nothing is exported from a design with ERC/DRC errors.
kicad-cli sch erc --severity-all --format report -o "$DOC/${P}_${REV}_erc.txt" "$P.kicad_sch" >/dev/null
kicad-cli pcb drc --schematic-parity --severity-all --format report -o "$DOC/${P}_${REV}_drc.txt" "$P.kicad_pcb" >/dev/null
grep -q "ERC messages: 0  Errors 0  Warnings 0" "$DOC/${P}_${REV}_erc.txt" || { echo "ERC not clean"; exit 1; }
grep -q "^\*\* Found 0 DRC violations \*\*" "$DOC/${P}_${REV}_drc.txt" || { echo "DRC not clean"; exit 1; }
grep -q "^\*\* Found 0 unconnected pads \*\*" "$DOC/${P}_${REV}_drc.txt" || { echo "unconnected pads"; exit 1; }
grep -q "^\*\* Found 0 Footprint errors \*\*" "$DOC/${P}_${REV}_drc.txt" || { echo "schematic parity errors"; exit 1; }

# Gerbers + Excellon drill files -> one zip for the PCB maker
kicad-cli pcb export gerbers --layers "F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts" \
    --use-drill-file-origin -o "$TMP/gerber/" "$P.kicad_pcb" >/dev/null
kicad-cli pcb export drill --format excellon --excellon-units mm --excellon-separate-th --drill-origin plot \
    -o "$TMP/gerber/" "$P.kicad_pcb" >/dev/null
rm -f "$FAB/${P}_${REV}_gerbers.zip"
(cd "$TMP/gerber" && zip -q -X "$OLDPWD/$FAB/${P}_${REV}_gerbers.zip" ./*)

# Bill of materials (grouped) and pick-and-place file
kicad-cli sch export bom --fields 'Reference,Value,Spec,Footprint,MPN,LCSC,${QUANTITY}' \
    --labels 'Designator,Value,Spec,Footprint,MPN,LCSC,Qty' --group-by 'Value,Spec,Footprint,MPN,LCSC' \
    --sort-field Reference -o "$FAB/${P}_${REV}_bom.csv" "$P.kicad_sch" >/dev/null
kicad-cli pcb export pos --format csv --units mm --side both --use-drill-file-origin \
    -o "$TMP/pos.csv" "$P.kicad_pcb" >/dev/null
python3 - "$TMP/pos.csv" "$FAB/${P}_${REV}_cpl.csv" <<'EOF'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1], newline="")))
with open(sys.argv[2], "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Designator", "Val", "Package", "Mid X", "Mid Y", "Rotation", "Layer"])
    for r in rows:
        w.writerow([r["Ref"], r["Val"], r["Package"], r["PosX"] + "mm", r["PosY"] + "mm", r["Rot"],
                    "Top" if r["Side"] == "top" else "Bottom"])
EOF

# Documentation: schematic, assembly drawing, board views
kicad-cli sch export pdf -o "$DOC/${P}_${REV}_schematic.pdf" "$P.kicad_sch" >/dev/null
kicad-cli pcb export pdf --layers "F.Fab,F.SilkS,Edge.Cuts" --include-border-title \
    -o "$DOC/${P}_${REV}_assembly_top.pdf" "$P.kicad_pcb" >/dev/null
kicad-cli pcb render --side top --quality high --background opaque --zoom 1 \
    -o "$DOC/${P}_${REV}_top.png" "$P.kicad_pcb" >/dev/null
kicad-cli pcb render --side bottom --quality high --background opaque --zoom 1 \
    -o "$DOC/${P}_${REV}_bottom.png" "$P.kicad_pcb" >/dev/null
ls -la "$FAB" "$DOC"
# PNG previews for the README (optional tools)
if command -v pdftoppm >/dev/null; then
    pdftoppm -r 110 -png -singlefile "$DOC/${P}_${REV}_schematic.pdf" "$DOC/${P}_${REV}_schematic"
fi
