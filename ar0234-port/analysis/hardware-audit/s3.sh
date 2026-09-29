#!/bin/sh
# Format/rate matrix, one variable at a time. Prints a compact summary per case.
# Usage: s3.sh <seconds> <case-label>=<dev>,<fmt>,<w>,<h>,<fps> ...
T=${1:-8}; shift
V=/home/orangepi/ar0234test/vfr
vic() { sed -n "/^$1:/,/^\*\*\*/p" /sys/kernel/debug/mpp/vi | grep -E 'frame =>|prs_in|output =>' | tr -s ' ' | tr '\n' '|'; }
dm() { dmesg | awk -v m="$1" '$0 ~ m {f=1} f'; }

for spec in "$@"; do
	label=${spec%%=*}; rest=${spec#*=}
	dev=$(echo "$rest" | cut -d, -f1); fmt=$(echo "$rest" | cut -d, -f2)
	w=$(echo "$rest" | cut -d, -f3); h=$(echo "$rest" | cut -d, -f4); fps=$(echo "$rest" | cut -d, -f5)
	vn=$(echo "${dev#/dev/video}" | sed 's/^/vi/')
	MARK="S3-${label}-$$"
	echo "=================================================================="
	echo "CASE $label  dev=$dev fmt=$fmt ${w}x${h} fps=$fps  t=${T}s"
	echo "  BEFORE $vn: $(vic $vn)"
	echo $MARK > /dev/kmsg
	$V -d $dev -w $w -h $h -f $fmt -t $T -p 1/$fps >/tmp/s3.$$ 2>&1
	rc=$?
	echo "  vfr rc=$rc"
	grep -E 'fmt |input |RESULT|GAPS|ERROR|error|failed|Invalid' /tmp/s3.$$ | head -6 | sed 's/^/  | /'
	tail -3 /tmp/s3.$$ | sed 's/^/  > /'
	echo "  AFTER  $vn: $(vic $vn)"
	E=$(dm "$MARK")
	printf '  dmesg-since-marker: frame_lost=%s hblank_short=%s isp_reset=%s not_mapped=%s oops=%s warn=%s lines=%s\n' \
		"$(echo "$E" | grep -c 'frame lost')" \
		"$(echo "$E" | grep -ci 'hblank short')" \
		"$(echo "$E" | grep -ci 'sunxi_isp_reset\|isp reset')" \
		"$(echo "$E" | grep -c 'is not mapped')" \
		"$(echo "$E" | grep -ci 'oops\|Kernel panic')" \
		"$(echo "$E" | grep -ci 'WARNING:')" \
		"$(echo "$E" | grep -c .)"
	echo "$E" | grep -v '^$' | head -8 | sed 's/^/  ! /'
done
rm -f /tmp/s3.$$
echo "########## mpp/mipi during idle (after all cases)"
cat /sys/kernel/debug/mpp/mipi
echo "########## DONE3"
