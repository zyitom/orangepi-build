#!/bin/sh
# ssh_board.sh "cmd" -- run a command on the Orange Pi board over WiFi without
# sshpass. Password comes from $BOARD_PASS (board default: a single space),
# host from $BOARD (default 172.16.0.193). Example:
#   tools/ssh_board.sh "uname -a"
#   tar -czf - userspace | tools/ssh_board.sh "tar -xzf - -C ~/ar0234test"
A=$(mktemp)
printf '#!/bin/sh\nprintf "%%s\\n" "%s"\n' "${BOARD_PASS- }" > "$A"
chmod 700 "$A"
SSH_ASKPASS=$A SSH_ASKPASS_REQUIRE=force DISPLAY=:0 ssh \
	-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
	-o PreferredAuthentications=password -o PubkeyAuthentication=no \
	-o NumberOfPasswordPrompts=1 -o ConnectTimeout=10 \
	"orangepi@${BOARD:-172.16.0.193}" "$@"
rc=$?
rm -f "$A"
exit $rc
