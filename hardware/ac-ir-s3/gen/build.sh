#!/bin/bash
# Regenerate the whole design: schematic -> ERC -> netlist -> board -> routing -> DRC -> outputs.
# Needs KiCad 9 (kicad-cli) and a python3 that has KiCad's 'pcbnew' module plus numpy.
# Usage: gen/build.sh   (from hardware/ac-ir-s3)
set -euo pipefail
cd "$(dirname "$0")/.."
PY=${PYTHON:-python3}
P=ac_ir_s3
$PY gen/local_lib.py
$PY gen/make_sch.py $P.kicad_sch
kicad-cli sch erc --severity-all --exit-code-violations -o gen/erc.rpt $P.kicad_sch
kicad-cli sch export netlist --format kicadsexpr -o gen/$P.net $P.kicad_sch
$PY gen/check_netlist.py gen/$P.net
$PY gen/make_pcb.py gen/$P.net $P.kicad_pcb
$PY gen/project.py $P.kicad_pro
$PY gen/route.py $P.kicad_pcb
kicad-cli pcb drc --schematic-parity --severity-all --format json -o gen/drc.json $P.kicad_pcb || true
$PY gen/fix_thermals.py $P.kicad_pcb gen/drc.json
gen/outputs.sh
