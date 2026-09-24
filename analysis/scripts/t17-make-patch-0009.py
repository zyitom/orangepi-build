#!/usr/bin/env python3
"""Apply patch 0009 to build/vin-d3d-lbc/vin-video/vin_video.c and emit the diff.

Run from the project root (ar0234-port).  Writes
  build/vin-d3d-lbc/vin-video/vin_video.c      (edited)
  patches/0009-vin-close-complete-rollback.patch
"""
import difflib
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "build/vin-d3d-lbc/vin-video/vin_video.c")
ORIG = "/tmp/vin_video.c.0009-orig"
PATCH = os.path.join(ROOT, "patches/0009-vin-close-complete-rollback.patch")

T = chr(9)
NL = chr(10)


def blk(lines):
    """lines: list of (indent, text); text may be None for a blank line."""
    out = []
    for ind, txt in lines:
        out.append(T * ind + txt if txt is not None else "")
    return NL.join(out) + NL

OLD = """	mutex_lock(&cap->vdev.entity.graph_obj.mdev->graph_mutex);
	if (!cap->pipe.sd[VIN_IND_SENSOR] || !cap->pipe.sd[VIN_IND_SENSOR]->entity.use_count) {
		mutex_unlock(&cap->vdev.entity.graph_obj.mdev->graph_mutex);
		vb2_fop_release(file);
		set_bit(VIN_LPM, &cap->state);
		clear_bit(VIN_BUSY, &cap->state);
		clear_bit(VIN_STREAM, &cap->state);
		if (cap->pipe.sd[VIN_IND_SENSOR])
			vin_warn("%s is not used, video%d cannot be close!\\n", cap->pipe.sd[VIN_IND_SENSOR]->name, vinc->id);
		return -1;
	}
"""

NEW = """	mutex_lock(&cap->vdev.entity.graph_obj.mdev->graph_mutex);
	if (!cap->pipe.sd[VIN_IND_SENSOR] || !cap->pipe.sd[VIN_IND_SENSOR]->entity.use_count) {
		/*
		 * Opened but never streamed, so there is no uplink to tear down.
		 * Do not leave through the front door though: the tail of this
		 * function is what undoes the rest of the open path, and leaving
		 * it out takes the *streaming* sibling node down with it because
		 * both capture nodes share mipi0/csi0/tdm_rx0/isp0.
		 *
		 * Measured on the Orange Pi Zero 3W (2026-09-16, module 0008,
		 * /dev/video0 streaming 1920x1200@120, 3DNR off):
		 *   video0 alone                          120.00 fps,  0 timeouts
		 *   video0 + one open()/close() of video4  29.59 fps, 23 timeouts
		 *   vi0 frame counter frozen at 908, dmesg got
		 *     "sensor_read error! sensor is not used!" (x3) and
		 *     "sunxi-vin-core 5831000.vinc: Runtime PM usage count
		 *      underflow!"
		 *
		 * vin_pipeline_call(vinc, close, ...) must be left out here: when
		 * the node never did S_INPUT the pipeline was never prepared, so
		 * p->sd[VIN_IND_SENSOR] is NULL and __vin_pipeline_close() only
		 * fires its own WARN_ON (vin.c:1287) - which is exactly what happens
		 * to the udev helper v4l_id at every boot.  Every other block in
		 * between is guarded by vin_streaming()/vin_lpm() and is a no-op for
		 * a node that never streamed, so jump past them.
		 */
		set_bit(VIN_LPM, &cap->state);
		clear_bit(VIN_STREAM, &cap->state);
		if (cap->pipe.sd[VIN_IND_SENSOR])
			vin_warn("%s is not used, video%d cannot be close!\n", cap->pipe.sd[VIN_IND_SENSOR]->name, vinc->id);
		else
			skip_pipeline_close = 1;
		goto shared_teardown;
	}
"""

ANCHOR_OLD = """	if (cap->pipe.sd[VIN_IND_ACTUATOR] != NULL) {
		ret = __vin_actuator_set_power(cap->pipe.sd[VIN_IND_ACTUATOR], 0);
"""
ANCHOR_NEW = """shared_teardown:
	if (cap->pipe.sd[VIN_IND_ACTUATOR] != NULL) {
		ret = __vin_actuator_set_power(cap->pipe.sd[VIN_IND_ACTUATOR], 0);
"""


def main():
    with open(SRC, newline="") as f:
        text = f.read()

    if "shared_teardown:" in text:
        print("already patched")
        return 0

    if text.count(OLD) != 1:
        print("OLD block found %d times, expected 1" % text.count(OLD))
        return 1
    if text.count(ANCHOR_OLD) != 1:
        print("ANCHOR found %d times, expected 1" % text.count(ANCHOR_OLD))
        return 1

    if not os.path.exists(ORIG):
        shutil.copy2(SRC, ORIG)

    DECL_OLD = chr(9) + "int ret;" + NL + chr(9) + "__maybe_unused struct vin_core *vinc_bind = NULL;" + NL + NL + chr(9) + "if (!vin_busy(cap)) {" + NL
    DECL_NEW = chr(9) + "int ret;" + NL + chr(9) + "int skip_pipeline_close = 0;" + NL + chr(9) + "__maybe_unused struct vin_core *vinc_bind = NULL;" + NL + NL + chr(9) + "if (!vin_busy(cap)) {" + NL
    if text.count(DECL_OLD) != 1:
        print("DECL matched %d times" % text.count(DECL_OLD))
        return 1
    text = text.replace(DECL_OLD, DECL_NEW)

    CLOSE_OLD = blk([(1, "ret = vin_pipeline_call(vinc, close, &cap->pipe);"), (1, "if (ret)"), (2, "vin_err(" + chr(34) + "vin pipeline close failed!" + chr(92) + "n" + chr(34) + ");"), (0, ""), (1, "v4l2_subdev_call(cap->pipe.sd[VIN_IND_ISP], core, init, 0);"), (0, "")])
    CLOSE_NEW = blk([(1, "if (!skip_pipeline_close) {"), (2, "ret = vin_pipeline_call(vinc, close, &cap->pipe);"), (2, "if (ret)"), (3, "vin_err(" + chr(34) + "vin pipeline close failed!" + chr(92) + "n" + chr(34) + ");"), (0, ""), (2, "v4l2_subdev_call(cap->pipe.sd[VIN_IND_ISP], core, init, 0);"), (1, "}"), (0, "")])
    if text.count(CLOSE_OLD) != 1:
        print("CLOSE matched %d times" % text.count(CLOSE_OLD))
        return 1
    text = text.replace(CLOSE_OLD, CLOSE_NEW)

    new = text.replace(OLD, NEW).replace(ANCHOR_OLD, ANCHOR_NEW)
    with open(SRC, "w", newline="") as f:
        f.write(new)

    diff = difflib.unified_diff(
        text.splitlines(keepends=True),
        new.splitlines(keepends=True),
        fromfile="a/vin-video/vin_video.c",
        tofile="b/vin-video/vin_video.c",
        n=3,
    )
    body = "".join(diff)

    header = """From: AR0234 port <local>
Subject: [PATCH 0009] vin: vin_close() must run the shared teardown when the
 node was opened but never streamed

vin_close() bailed out early when the sensor subdev's use_count was 0, i.e.
for a node that was opened and closed without ever streaming.  That path did
unlock the graph mutex, release vb2 and clear the state bits, but it skipped
everything the tail of the function does, in particular

  vin_pipeline_call(vinc, close, &cap->pipe)
    -> __vin_pipeline_close()
       -> vin_pipeline_s_power(p, 0)
       -> vin_md_set_power(vind, 0)
       -> vin_video_core_s_power(CAPTURE, 0)
            = pm_runtime_put_sync(&vinc->pdev->dev)

so the vinc-side references stayed armed.  Because the two capture nodes share
the same mipi0/csi0/tdm_rx0/isp0 subdevices, the *streaming* sibling node dies
with it:

  /dev/video0 1920x1200@120 alone                         120.00 fps,  0 timeouts
  /dev/video0 + one open()/close() of /dev/video4         29.59 fps, 23 timeouts
    vi0 frame counter frozen at 908, dmesg gets
      sensor_read error! sensor is not used!            (x3)
      sunxi-vin-core 5831000.vinc: Runtime PM usage count underflow!
      ar0234_mipi is not used, video0 cannot be close!

Reproduced deterministically with analysis/scripts/t17-openclose.c (a bare open()+close(),
no ioctl at all) and analysis/scripts/t17-repro-close.sh on 2026-09-16.

Fix: keep the diagnostic and the state cleanup, but jump to the shared teardown
(label placed at the first statement after the streaming/uplink blocks) instead
of returning.  The skipped blocks are all guarded by vin_streaming()/vin_lpm()
and stay no-ops for a node that never streamed.

"""
    with open(PATCH, "w", newline="") as f:
        f.write(header + body)

    print("patched %s" % SRC)
    print("wrote   %s (%d bytes of diff)" % (PATCH, len(body)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
