#!/usr/bin/env python3
"""Hook patches/0009 into apply.sh (idempotent)."""
import os
import sys

ROOT = "/home/helios/Desktop/orangepi-build/ar0234-port"
P = os.path.join(ROOT, "apply.sh")
NL = chr(10)
TAB = chr(9)
BS = chr(92)

ANCHOR = ('patch -p1 -d "$K/bsp/drivers/vin" --forward < '
          '"$DIR/patches/0008-vin-second-channel-and-pipeline-guards.patch" ' + BS + NL
          + TAB + '|| echo "0008 already applied?"' + NL)

ADD = NL + \
    '# a capture node opened and closed *without* streaming must still run the' + NL + \
    '# teardown tail of vin_close(), otherwise the streaming sibling node on the' + NL + \
    '# shared mipi0/csi0/tdm_rx0/isp0 pipe dies: 120 fps -> 29 fps plus' + NL + \
    '# "5831000.vinc: Runtime PM usage count underflow" (HANDOFF 3.27).  The' + NL + \
    '# vin_pipeline_call(vinc, close, ...) in that tail is skipped for a node' + NL + \
    '# that never did S_INPUT, whose pipeline was never prepared.' + NL + \
    'patch -p1 -d "$K/bsp/drivers/vin" --forward < ' + \
    '"$DIR/patches/0009-vin-close-complete-rollback.patch" ' + BS + NL + \
    TAB + '|| echo "0009 already applied?"' + NL


def main():
    with open(P, newline="") as f:
        s = f.read()
    if "0009-vin-close-complete-rollback.patch" in s:
        print("apply.sh already references 0009")
        return 0
    if s.count(ANCHOR) != 1:
        print("anchor matched %d times, expected 1" % s.count(ANCHOR))
        return 1
    s = s.replace(ANCHOR, ANCHOR + ADD)
    with open(P, "w", newline="") as f:
        f.write(s)
    print("apply.sh updated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
