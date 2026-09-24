#!/bin/bash
M=/tmp/opi_zero3w_man.txt
echo "=== locate section 3.14 in the manual text ==="
grep -n '3\.14\.' "$M" | head -10
grep -n '40 pin 接口引脚说明' "$M" | head -10
grep -n '40pin 接口引脚说明' "$M" | head -10

echo
echo "=== print the region around the section heading ==="
N=$(grep -n '40 pin 接口引脚说明\|40pin 接口引脚说明' "$M" | tail -1 | cut -d: -f1)
echo "heading at line: $N"
if [ -n "$N" ]; then
    sed -n "${N},$((N+150))p" "$M"
fi
