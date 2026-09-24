#!/usr/bin/env python3
# bootpkg.py -- inspect / replace the "scp" item of the sunxi boot package
# (toc1, "sunxi-package") on an SD card or an image of its first MiBs.
#
#   bootpkg.py info    <dev-or-img>
#   bootpkg.py set-scp <dev-or-img> <payload.bin>            # dry run
#   bootpkg.py set-scp <dev-or-img> <payload.bin> --write    # apply
#
# Unlike the old fixed-slot flow (dd 105912 bytes + fix-bootpkg-sum.py), this
# RESIZES the item, so payloads of any size fit -- the vendor SCP built from
# Tina 1.5.0 is 120856 bytes, the image shipped on the card 105912 bytes.
# That only works because scp is the LAST item and the card is blank after
# the package; both are checked before anything is written.
#
# What gets written (and nothing else):
#   * the payload at the item's data_offset
#   * zeros over the rest of the old item / package tail
#   * item data_len, header valid_len (end of last item rounded up to 16 KiB,
#     which reproduces the vendor value 0x154000 for the shipped image)
#   * header add_sum, recomputed the way boot0 checks it (see
#     tests/fix-bootpkg-sum.py for how that algorithm was derived)
# Restoring the shipped image is `set-scp <dev> external/packages/pack-uboot/sun60iw2/bin/scp.fex`;
# the result is byte-identical to the original package.
import os, struct, sys

PKG_OFF = 0x1004000
MAGIC = 0x89119800
STAMP = 0x5F0A6C39
HDR_ADD_SUM, HDR_ITEMS, HDR_VALID_LEN = 0x14, 0x20, 0x24
ITEM0, ITEM_SZ = 0x40, 0x170		# item: name[64], data_offset, data_len, ...
VALID_ALIGN = 0x4000
MAX_SCP = 0x30000			# SRAM A2 0x40004000..0x40034000


def words_sum(b):
	return sum(struct.unpack_from('<%dI' % (len(b) // 4), b)) & 0xFFFFFFFF


def read_at(f, off, n):
	f.seek(off)
	b = f.read(n)
	if len(b) != n:
		sys.exit('short read at %#x' % off)
	return b


def parse(f):
	head = read_at(f, PKG_OFF, ITEM0)
	magic, add_sum = struct.unpack_from('<II', head, 0x10)
	if magic != MAGIC:
		sys.exit('no sunxi-package at %#x (magic %08x)' % (PKG_OFF, magic))
	n, valid_len = struct.unpack_from('<II', head, HDR_ITEMS)
	items = []
	for i in range(n):
		e = read_at(f, PKG_OFF + ITEM0 + i * ITEM_SZ, ITEM_SZ)
		name = e[:64].split(b'\0')[0].decode(errors='replace')
		off, ln = struct.unpack_from('<II', e, 0x40)
		items.append((i, name, off, ln))
	return add_sum, valid_len, items


def checksum_ok(f):
	add_sum, valid_len, _ = parse(f)
	pkg = read_at(f, PKG_OFF, valid_len)
	need = (words_sum(pkg) - add_sum + STAMP) & 0xFFFFFFFF
	return need == add_sum, need


def info(f):
	add_sum, valid_len, items = parse(f)
	ok, need = checksum_ok(f)
	print('package @%#x  valid_len %#x  add_sum %08x  checksum %s'
	      % (PKG_OFF, valid_len, add_sum, 'PASS' if ok else 'FAIL (need %08x)' % need))
	for i, name, off, ln in items:
		print('  [%d] %-8s off %#8x  len %#7x (%d)  abs %#x..%#x'
		      % (i, name, off, ln, ln, PKG_OFF + off, PKG_OFF + off + ln))
	return 0 if ok else 1


def set_scp(f, payload, write):
	add_sum, valid_len, items = parse(f)
	ok, _ = checksum_ok(f)
	if not ok:
		sys.exit('refusing: package checksum is already invalid (run info)')
	scp = [it for it in items if it[1] == 'scp']
	if len(scp) != 1:
		sys.exit('no unique scp item')
	idx, _, off, old_len = scp[0]
	if any(it[2] > off for it in items):
		sys.exit('refusing: scp is not the last item, it cannot be resized')
	if len(payload) % 4 or len(payload) > MAX_SCP:
		sys.exit('bad payload size %d (must be a multiple of 4, <= %#x)' % (len(payload), MAX_SCP))

	new_valid = (off + len(payload) + VALID_ALIGN - 1) & ~(VALID_ALIGN - 1)
	if new_valid > valid_len:
		tail = read_at(f, PKG_OFF + valid_len, new_valid - valid_len)
		if any(tail):
			sys.exit('refusing: %#x..%#x after the package is not blank'
			         % (PKG_OFF + valid_len, PKG_OFF + new_valid))
	span = max(valid_len, new_valid) - off
	print('scp item [%d]: len %d -> %d, valid_len %#x -> %#x'
	      % (idx, old_len, len(payload), valid_len, new_valid))
	if not write:
		print('DRY RUN: add --write to apply')
		return 0

	# new region = payload + zeros up to the larger of old/new package end
	f.seek(PKG_OFF + off)
	f.write(payload + bytes(span - len(payload)))
	f.seek(PKG_OFF + ITEM0 + idx * ITEM_SZ + 0x44)
	f.write(struct.pack('<I', len(payload)))
	f.seek(PKG_OFF + HDR_VALID_LEN)
	f.write(struct.pack('<I', new_valid))
	f.flush()
	# add_sum sits inside the summed range, so the fix is "what boot0 computes now"
	_, need = checksum_ok(f)
	f.seek(PKG_OFF + HDR_ADD_SUM)
	f.write(struct.pack('<I', need))
	f.flush()
	os.fsync(f.fileno())

	ok, _ = checksum_ok(f)
	back = read_at(f, PKG_OFF + off, len(payload))
	print('write done: checksum %s, payload readback %s'
	      % ('PASS' if ok else 'FAIL', 'PASS' if back == payload else 'FAIL'))
	return 0 if ok and back == payload else 2


def main(argv):
	if len(argv) < 3 or argv[1] not in ('info', 'set-scp'):
		sys.exit(__doc__ if __doc__ else 'usage: bootpkg.py info|set-scp ...')
	write = '--write' in argv
	with open(argv[2], 'r+b' if write else 'rb') as f:
		if argv[1] == 'info':
			return info(f)
		if len(argv) < 4:
			sys.exit('set-scp needs a payload file')
		return set_scp(f, open(argv[3], 'rb').read(), write)


if __name__ == '__main__':
	sys.exit(main(sys.argv))
