#!/bin/sh
# start2.sh <script> <outfile> <points...>
S=$1; shift
OUT=$1; shift
exec systemd-run --unit=t17iso --collect --setenv=OUT="$OUT" /bin/sh "$S" "$@"
