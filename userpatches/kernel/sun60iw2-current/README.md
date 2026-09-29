# userpatches/kernel/sun60iw2-current — kernel patch series (RT + AR0234 + E902)

**All kernel changes live here.** orangepi-build runs `git checkout -f` +
`git clean -qdf` on `kernel/orange-pi-6.6-sun60iw2` before every build
(scripts/general.sh fetch_from_repo), so anything edited directly in that tree
is wiped. Never edit the tree in place -- add a patch here.

Base: gitee/github orangepi-xunlong `orange-pi-6.6-sun60iw2` @ `2ac08e8c7`
(aiot v1.5.0 merge + uart8/pcie dts fix). Verified 2026-09-24: 0000–0015 apply
with no reject; the only fuzz is 0006 hunk 1 (a comment block after the
MAX_IN_* defines, placement checked). Re-verified 2026-09-29 with the full
series (0000–0019, `patch -p1 -N` as the build does): no reject. Compared
file by file with the vendor kernel worktree and ~/rt-kernel-test, the only
differences are the experimental/ diffs and 0019 (not yet in a built image),
so no kernel change lives outside this directory.

| file | what |
|---|---|
| 0000-rt58.patch | PREEMPT_RT (patch-6.6.99-rt58, dry-run clean) |
| 0001–0013 | AR0234 sensor + vin fixes (table below) |
| 0014-misc-add-amp-timestamp-driver.patch | E902/ARM shared 24 MHz timestamp driver + dtsi node (e902/linux) |
| 0015-bsp-fix-lradc-rpmsg-trng-build-on-6.6.patch | 6.6 API fixes; the config enables AW_LRADC/AW_TRNG, which do not build without it |
| 0016-dts-zero3w-disable-phantom-devices-and-empty-pinmux.patch | board DTS cleanup: phantom AXP515@34, hym8563, gt9271, empty uart0/uart5 pinmux (one error line per boot each) |
| 0019-rtla-poll-on-every-commit.patch | tools/tracing/rtla: `buffer_percent=0`, fixes the osnoise/timerlat "hang" (e902/doc/e902/FINDINGS-LEDGER.md T38) |
| experimental/ | NOT applied by the build (see below) |

Numbering gaps are deliberate, so older notes still resolve: 0002/0004 were
config patches (now in the config override, see "AR0234 series"), 0017/0018
were merged into 0016 on 2026-09-29, 0020–0023 moved to experimental/.

## experimental/ — not applied

`advanced_patch` (scripts/compilation.sh) only picks up `*.patch` directly in
this directory (plus `target_*/board_*/branch_*` subdirectories), so files in
`experimental/` are kept for reference and never reach a build. They are from
the 2026-09-27 Vulkan investigation. Its conclusion ("the vendor ICD
rejects the device on this kernel stack") was wrong: the real cause was a
missing libxshmfence in the Buildroot rootfs, a user-space dlopen failure
(tina-zero3w/docs/VULKAN-HANDOFF.md). None of these kernel changes is
needed; they stay here only as a record of what was tried.

| file | what |
|---|---|
| 0020–0022 | rogue: log RGXInitMultiCoreInfo / GetMultiCoreInfo / every failing bridge dispatch |
| 0023 | rogue: force a 4-core multicore report — **diagnostic only, wrong on real hardware** |
| prime-import-radxa-fex.patch | DRM PRIME import from the Radxa a733-powervr-fex stack (buffer sharing only) |

Previously these last two existed only as direct edits (0015 in the vendor
tree, 0014 in ~/rt-kernel-test); 0013 was rebased on 2026-09-24 because on the
new base its old context applied into vin_pin_enable() with fuzz.

## AR0234 series

Formal home (NEXT-TASKS C12) of the AR0234 kernel changes, so a rebuild with

    ./build.sh BOARD=orangepizero3w BRANCH=current BUILD_OPT=kernel REVISION=1.0.1

reproduces the field-proven camera stack instead of silently dropping it.
The build system applies everything here with `patch -p1 -N` from the kernel
source root, in file-name order (scripts/compilation.sh, advanced_patch /
process_patch_file). A full kernel config override sits next to this
directory: `userpatches/linux-sun60iw2-current-a733.config`.

## Provenance

Every file is a copy of `ar0234-port/patches/` of the same name, with ONE
mechanical difference: the vin-relative patches were rewritten from
`bsp/drivers/vin`-relative paths to kernel-root-relative paths
(`--- a/vin-video/...` → `--- a/bsp/drivers/vin/vin-video/...`), because
ar0234-port's `apply.sh` applies those from inside `bsp/drivers/vin` while
the build system always patches from the kernel root. Content is otherwise
byte-identical; re-derive with:

    sed -e 's|^--- a/|--- a/bsp/drivers/vin/|' -e 's|^+++ b/|+++ b/bsp/drivers/vin/|'

| file | same as | note |
|---|---|---|
| 0001-vin-sun60iw2-add-ar0234-sensor.patch | identical | sensor driver + Kconfig + Makefile + DTS (mname, sensor2 disabled) |
| 0003-vin-auto-s_input-streamon-rollback-parm-type.patch | identical | kernel-root relative already |
| 0005-vin-fix-second-vinc-pipeline-binding.patch | paths rewritten | |
| 0006-vin-isp-3dnr-blanking-interlock.patch | paths rewritten | |
| 0007-vin-fix-sensor-setup-link-null-deref.patch | paths rewritten | |
| 0008-vin-second-channel-and-pipeline-guards.patch | identical | kernel-root relative already |
| 0009-vin-close-complete-rollback.patch | paths rewritten | |
| 0010-lbc-output-refused-in-scaler.patch | paths rewritten | |
| 0011-vin-csi-bandwidth-fix.patch | paths rewritten | |
| 0012-vin-pipeline-close-refcount.patch | paths rewritten | |

`ar0234-port/patches/0002` (SENSOR_AR0234) and `0004` (D3D LBC) are NOT here:
they patch orangepi-build's own config files, which the single config override
`userpatches/linux-sun60iw2-current-a733.config` replaces wholesale. They stay
in ar0234-port for the apply.sh flow.

## Board DTB parity

With 0001 applied, a rebuilt DTB matches the running board's DTB
(`d4ee5b68…` state, 2026-09-17) on every camera-relevant property:

- `sensor@5812000`: `sensor0_mname = "ar0234_mipi"` (0001), `isp_used = 1`
  (already upstream), cci 11 / mclk 1 / pwdn PE6 (already upstream).
- `sensor@5812010` (sensor1): `disabled` (already upstream).
- `sensor@5812020` (sensor2, MIPI-B): `disabled` (0001). When the second
  module is installed, re-enable + set `sensor2_mname = "ar0234_mipi"` per
  ar0234-port NEXT-TASKS A1 — keep that as a board-side `fdtput` step, it must
  never be enabled without the module (probe panic, HANDOFF §3.26).
- `vinc@5832000` (vinc20, second capture path): `okay` (already upstream).
- `tdm@5908000 work_mode = 0`, `csi_isp = csi_top = 324 MHz` (board DTS
  overrides the dtsi), `vind resets = <&ccu RST_BUS_CSI>, <>`: already
  upstream, byte-identical after compile.

Deliberately NOT in this series: `RST_BUS_VIDEO_IN` on `vind` (NEXT-TASKS B6 —
needs its own verified round) and any `csi_isp` clock bump (B7).
