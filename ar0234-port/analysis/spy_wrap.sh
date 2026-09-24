#!/bin/sh
# 3A helper wrapper for ar0234_rec: preload the register spy, keep stdout for "ready"
ISPSPY_LOG=/tmp/an/spy.log LD_PRELOAD=/home/orangepi/ar0234test/an/ispreg_spy.so exec /home/orangepi/ar0234test/ar0234_3a "$@"
