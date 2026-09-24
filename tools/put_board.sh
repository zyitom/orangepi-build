#!/bin/sh
# put_board.sh <local_file> <remote_path>
#
# Copy a file to the board through the WiFi ssh link without sshpass.
#
# A single base64 argument (the old version) dies with "Argument list too
# long" once the file is bigger than ~100 KB, which is exactly the size of a
# stripped .ko, so the payload is now streamed in chunks through ssh stdin
# instead.  The board's `base64 -d` then reassembles it and the md5 is printed
# for both ends so the caller can check the copy.
set -e
cd "$(dirname "$0")/.."

if [ $# -ne 2 ]; then
	echo "usage: tools/put_board.sh <local_file> <remote_path>" >&2
	exit 2
fi

LOCAL=$1
REMOTE=$2
CHUNK=60000			# base64 bytes per ssh round trip

mktemp -d >/dev/null 2>&1 || true
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

base64 -w0 < "$LOCAL" > "$WORK/b64"
SIZE=$(wc -c < "$WORK/b64")

echo "local : $(md5sum "$LOCAL")"

tools/ssh_board.sh "rm -f '$REMOTE.b64'" < /dev/null

off=0
while [ "$off" -lt "$SIZE" ]; do
	dd if="$WORK/b64" bs="$CHUNK" skip=$((off / CHUNK)) count=1 2>/dev/null |
		tools/ssh_board.sh "cat >> '$REMOTE.b64'"
	off=$((off + CHUNK))
done

tools/ssh_board.sh "base64 -d '$REMOTE.b64' > '$REMOTE' && rm -f '$REMOTE.b64' && md5sum '$REMOTE' && ls -la '$REMOTE'" < /dev/null
