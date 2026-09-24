#!/bin/bash
# build.sh -- build the Allwinner (vendor) SCP firmware for the Orange Pi
# Zero 3W (A733 / sun60iw2p1) from the public Tina 5.0 source.
#
#   bash e902/vendor-scp/build.sh          # fetch (if needed), patch, build
#   bash e902/vendor-scp/build.sh --clean  # also reset the vendor tree first
#
# Output: e902/fw-out/vendor-scp.bin (+ entry in fw-out/SHA256SUMS)
#
# The vendor sources are fetched into e902/vendor-ref/ (git-ignored: the
# arisc repo carries no license file, so it is not redistributed here).
# Everything we change lives in patches/ and is applied on top of a pinned
# upstream commit, so the result is reproducible.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
E=$(dirname "$HERE")
REF=$E/vendor-ref
OUT=$E/fw-out

ARISC_URL=https://gitlab.com/tina5.0_aiot/lichee/arisc.git
ARISC_REV=0170020e421772d892d569dad5ef8b0faffc120a	# "import Tina 1.5.0 snapshot"
DRAMLIB_URL=https://gitlab.com/tina5.0_aiot/lichee/dramlib.git
DRAMLIB_REV=7142734a28641811a3a12dc094da1ed1ff38e2b6	# "aiot-linux-v1.5.0 release"
BRANCH=product-aiot-stable
DEFCONFIG=sun60iw2p1_opi_zero3w_defconfig

# Xuantie GCC (rv32emc needs _zicsr_zifencei spelled out on GCC >= 12)
TC=${TC:-$E/toolchains/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0}
MARCH="-mtune=e906 -mcmodel=medany -mabi=ilp32e -march=rv32emc_zicsr_zifencei -fsingle-precision-constant"

[ -x "$TC/bin/riscv64-unknown-elf-gcc" ] || {
	echo "toolchain not found: $TC (set TC=...; see e902/vendor-scp/README.md)"; exit 1; }
export PATH=$TC/bin:$PATH

fetch() {	# fetch <url> <dir> <rev>
	if [ ! -d "$2/.git" ]; then
		git clone --depth 1 --branch "$BRANCH" "$1" "$2"
	fi
	if [ "$(git -C "$2" rev-parse HEAD)" != "$3" ]; then
		git -C "$2" fetch --depth 1 origin "$3"
		git -C "$2" checkout -q "$3"
	fi
}

mkdir -p "$REF" "$OUT"
fetch "$ARISC_URL" "$REF/arisc" "$ARISC_REV"
fetch "$DRAMLIB_URL" "$REF/dramlib" "$DRAMLIB_REV"

A=$REF/arisc
if [ "${1:-}" = "--clean" ]; then
	git -C "$A" reset -q --hard "$ARISC_REV"
	git -C "$A" clean -qfdx ar100s
fi

# apply our patches once (idempotent: skip a patch that is already in)
for p in "$HERE"/patches/*.patch; do
	if git -C "$A" apply --check "$p" 2>/dev/null; then
		git -C "$A" apply "$p"
		echo "applied  $(basename "$p")"
	elif git -C "$A" apply --reverse --check "$p" 2>/dev/null; then
		echo "present  $(basename "$p")"
	else
		echo "ERROR: $(basename "$p") neither applies nor is present -- run with --clean"; exit 1
	fi
done

cd "$A/ar100s"
make clean >/dev/null 2>&1 || true
make "$DEFCONFIG"
make -j"$(nproc)" LICHEE_DRAMLIB_PATH="$REF/dramlib" MARCH_FLAGS="$MARCH"

cp scp.bin "$OUT/vendor-scp.bin"
cp scp.elf "$OUT/vendor-scp.elf"
cd "$OUT"
{ grep -v ' vendor-scp.bin$' SHA256SUMS 2>/dev/null || true; sha256sum vendor-scp.bin; } > SHA256SUMS.tmp
mv SHA256SUMS.tmp SHA256SUMS

sz=$(stat -c%s vendor-scp.bin)
echo
echo "built $OUT/vendor-scp.bin: $sz bytes, sha256 $(sha256sum vendor-scp.bin | cut -d' ' -f1)"
# SRAM A2 budget: image is loaded at 0x40004000, stack sits at 0x4002EC00
end=$(riscv64-unknown-elf-nm vendor-scp.elf | awk '$3=="_end"||$3=="__bss_end"||$3=="_ebss"{print $1; exit}')
[ -n "$end" ] && echo "bss end 0x$end (must stay below the stack at 0x4002ec00)"
