#!/bin/sh
# ssh_board.sh [-s] "cmd" -- run a command on the Orange Pi board over WiFi
# without sshpass. Password comes from $BOARD_PASS (default: the Orange Pi
# image default "orangepi"), host from $BOARD (default 172.16.0.193).
# -s runs the command under sudo, feeding the same password. Examples:
#   tools/ssh_board.sh "uname -a"
#   tools/ssh_board.sh -s "dmesg | tail"
#   tar -czf - userspace | tools/ssh_board.sh "tar -xzf - -C ~/ar0234test"
PASS=${BOARD_PASS-orangepi}
if [ "$1" = "-s" ]; then
	shift
	# base64 on the wire: no quoting of the command or password can break
	q=$(printf '%s' "$*" | base64 -w0)
	p=$(printf '%s\n' "$PASS" | base64 -w0)
	set -- "echo $p | base64 -d | sudo -S -p '' sh -c \"\$(echo $q | base64 -d)\""
fi
A=$(mktemp)
printf '#!/bin/sh\nprintf "%%s\\n" "%s"\n' "$PASS" > "$A"
chmod 700 "$A"
SSH_ASKPASS=$A SSH_ASKPASS_REQUIRE=force DISPLAY=:0 ssh \
	-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
	-o PreferredAuthentications=password -o PubkeyAuthentication=no \
	-o NumberOfPasswordPrompts=1 -o ConnectTimeout=10 \
	"orangepi@${BOARD:-172.16.0.193}" "$@"
rc=$?
rm -f "$A"
exit $rc
