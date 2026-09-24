#!/bin/sh
# vfrrun.sh <label> <seconds> [extra vfr args...]
#
# One measurement run of tools/hwtest/vfr.c against the board, with three independent
# pieces of evidence for "did the pipeline drop frames":
#
#   1. vfr itself      - DQBUF count, wall time, inter-frame gap histogram
#   2. debugfs viN     - the VIN/ISP input counters (frame/lost_cnt/error_cnt),
#                        read *immediately* before and after the run
#   3. dmesg           - kernel messages after a unique marker written to
#                        /dev/kmsg at t0 ("frame lost", sunxi_isp_reset, ...)
#
# Everything is appended to analysis/vfr/<label>.log on the host; the serial
# console log (tools/serial_log.py -> analysis/t14/serial-*.log) is evidence #4.
#
# Example:
#   tools/hwtest/vfrrun.sh a1-1200p120 120 -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120
set -e
cd "$(dirname "$0")/../.."

LABEL=${1:?usage: vfrrun.sh <label> <seconds> [vfr args...]}
SECS=${2:?usage: vfrrun.sh <label> <seconds> [vfr args...]}
shift 2
[ $# -eq 0 ] && set -- -d /dev/video0 -w 1920 -h 1200 -f NV12 -p 1/120

LOG=analysis/vfr/$LABEL.log
mkdir -p analysis/vfr

MARK="VFRMARK-${LABEL}-$$-$(date +%s)"

# dmcount.sh / dmfilter.sh live in /tmp (tmpfs) - refresh them every run.
tools/put_board.sh tools/dmcount.sh  /tmp/dmcount.sh  >/dev/null
tools/put_board.sh tools/dmfilter.sh /tmp/dmfilter.sh >/dev/null

vfr_i() {			# viN counters for the node vfr will use
	board "sed -n '/^$1:/,/^\*\*\*/p' /sys/kernel/debug/mpp/vi | grep -E 'frame =>|input =>|output =>|prs_in|internal =>' | tr -s ' '"
}
dcount() {			# dmesg lines matching $1 since the marker
	board "sh /tmp/dmcount.sh '$MARK' '$1'"
}
board() { tools/ssh_board.sh -s "$1" < /dev/null; }

# which viN does /dev/videoN map to?  vi0 -> /dev/video0, vi4 -> /dev/video4 ...
DEV=$(printf '%s\n' "$@" | tr ' ' '\n' | sed -n '/^\/dev\/video/p' | head -1)
[ -z "$DEV" ] && DEV=/dev/video0
VNODE=$(echo "${DEV#/dev/video}" | sed 's/^/vi/')

{
	echo "===================== $LABEL ====================="
	echo "date   : $(date -Is)"
	echo "uptime : $(tools/ssh_board.sh 'cut -d" " -f1 /proc/uptime' < /dev/null) s"
	echo "module : $(board 'cat /sys/module/vin_v4l2/srcversion; md5sum /lib/modules/6.6.98-sun60iw2/updates/vin_v4l2.ko' | tr '\n' ' ')"
	echo "args   : -t $SECS $*  ($VNODE)"
	echo "marker : $MARK"
	echo "--- $VNODE counters before ---"
	vfr_i "$VNODE"
	echo "--- vfr output ---"
} | tee -a "$LOG"

board "echo $MARK > /dev/kmsg"
set +e
tools/ssh_board.sh -s "~/ar0234test/vfr -t $SECS $*" < /dev/null 2>&1 | tee -a "$LOG"
RC=$?
set -e

{
	echo "--- vfr rc=$RC ---"
	echo "--- $VNODE counters after ---"
	vfr_i "$VNODE"
	echo "--- dmesg since marker ---"
	for p in "frame lost" "sunxi_isp_reset" "vblank" "3DNR forced off" \
		 "configuration error" "height error" "stream off" "not mapped" "CSI module" \
		 "sunxi_iommu" "Oops" "BUG:" "WARNING:" "Call trace"; do
		printf '%-24s %s\n' "$p" "$(dcount "$p" 2>/dev/null || echo '?')"
	done
	echo "--- dmesg lines of interest (tail) ---"
	board "sh /tmp/dmfilter.sh '$MARK'"
	echo
} 2>&1 | tee -a "$LOG"

echo "log: $LOG"
exit $RC
