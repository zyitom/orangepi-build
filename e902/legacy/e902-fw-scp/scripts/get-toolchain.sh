#!/bin/bash
# Fetch the T-Head Xuantie bare-metal RISC-V toolchain.
#
# URL and sha256 taken from the AUR package xuantie-900-series-toolchain-bin
# (PKGBUILD, pkgver 3.2.0). It is the same tarball the vendor serves from
# xrvm.cn, just without the web login. Verified reachable (HTTP 206).
set -euo pipefail

VER=3.2.0
DATE=20250627
TAR="Xuantie-900-gcc-elf-newlib-x86_64-V${VER}-${DATE}.tar.gz"
URL="https://occ-oss-prod.oss-cn-hangzhou.aliyuncs.com/resource//1751370399722/${TAR}"
SHA256=80c174c6445f7565bc082d328045021862a63beddfad8c393c534e2d9523dc3b

DEST="${1:-$(cd "$(dirname "$0")/../.." && pwd)/toolchains}"

mkdir -p "$DEST"
cd "$DEST"

if [ ! -f "$TAR" ]; then
	echo "downloading $TAR (~1 GB) ..."
	curl -L --fail -o "$TAR" "$URL"
fi

echo "verifying checksum ..."
echo "${SHA256}  ${TAR}" | sha256sum -c -

echo "extracting ..."
tar xzf "$TAR"

# The tarball's top-level directory name has varied between releases; find it.
DIR=$(tar tzf "$TAR" | head -1 | cut -d/ -f1)
BIN="$DEST/$DIR/bin"

echo
echo "installed to: $DEST/$DIR"
echo "toolchain prefixes found:"
ls "$BIN" | grep -E 'gcc$' | sed 's/^/  /'
echo
echo "Build with, for example:"
PREFIX=$(ls "$BIN" | grep -E 'gcc$' | head -1 | sed 's/gcc$//')
echo "  make CROSS=$BIN/$PREFIX"
echo
echo "Sanity check RV32E support:"
echo "  echo 'int main(){return 0;}' > /tmp/t.c"
echo "  $BIN/${PREFIX}gcc -march=rv32emc -mabi=ilp32e -c /tmp/t.c -o /tmp/t.o && echo OK"
