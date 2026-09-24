#!/usr/bin/env python3
"""Fix the two vin_warn() literals that the shell mangled into real newlines,
rebuild build/vin-d3d-lbc/vin-video/vin_video.c from the pre-0009 copy, and
regenerate patches/0009-vin-close-complete-rollback.patch.
"""
import difflib
import os
import shutil
import sys

ROOT = "/home/helios/Desktop/orangepi-build/ar0234-port"
SRC = os.path.join(ROOT, "build/vin-d3d-lbc/vin-video/vin_video.c")
PRE = "/tmp/vin_video.c.pre0009"
PATCH = os.path.join(ROOT, "patches/0009-vin-close-complete-rollback.patch")
NL = chr(10)

BROKEN = 'cannot be close!' + NL + '"'
FIXED = 'cannot be close!' + chr(92) + 'n"'


def main():
    with open(SRC, newline="") as f:
        text = f.read()

    n = text.count(BROKEN)
    print("broken literals found: %d" % n)
    if n:
        text = text.replace(BROKEN, FIXED)
        with open(SRC, "w", newline="") as f:
            f.write(text)
        print("repaired %d literals" % n)

    with open(PRE, newline="") as f:
        pre = f.read()

    if "shared_teardown:" not in text:
        print("source is not the patched version, aborting")
        return 1

    diff = difflib.unified_diff(
        pre.splitlines(keepends=True),
        text.splitlines(keepends=True),
        fromfile="a/vin-video/vin_video.c",
        tofile="b/vin-video/vin_video.c",
        n=3,
    )
    body = "".join(diff)
    header = open(PATCH, newline="").read().split("--- a/")[0]
    with open(PATCH, "w", newline="") as f:
        f.write(header + body)
    print("wrote %s (%d bytes)" % (PATCH, len(body)))
    print("hunks: %d" % body.count(NL + "@@"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
