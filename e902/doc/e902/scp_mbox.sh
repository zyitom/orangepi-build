#!/bin/bash
# Dump the SCP disassembly around its CPUX_MSGBOX usage (lui 0x3004 seen at 0x40007334).
D=/tmp/scp.dis
echo "===== all instructions referencing the 0x03004000 msgbox block ====="
grep -n -B4 -A18 'lui[[:space:]]*a[0-7],0x3004' "$D" | head -80

echo
echo "===== raw region 0x40007280..0x40007480 ====="
awk '/^40007280:/,/^40007480:/' "$D" | head -140
