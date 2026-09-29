#!/bin/sh
# broot.sh "cmd" : run cmd on the board as root (board sudo password = one space)
# Must live in <project>/analysis/t17/ (cd ../../ = project root).
cd "$(dirname "$0")/../.." || exit 1
SUDO='printf " \n" | sudo -S -p ""'
exec tools/ssh_board.sh "$SUDO sh -c \"$1\"" < /dev/null
