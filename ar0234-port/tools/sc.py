#!/usr/bin/env python3
"""Run commands on the board over the serial console.
Usage: sc.py [-t timeout] "cmd" ...
Assumes a logged-in shell (logs in if it sees login:). sudo password = ' '.
"""
import re, sys, time, serial

PORT, BAUD = "/dev/ttyUSB0", 115200
USER, PASSWDS = "orangepi", ["orangepi", " "]

ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")


def rd(s):
    d = b""
    while True:
        c = s.read(65536)
        if not c:
            return d.decode("utf-8", "replace")
        d += c


def expect(s, pat, to):
    buf, end = "", time.time() + to
    while time.time() < end:
        buf += rd(s)
        m = re.search(pat, buf)
        if m:
            return m, buf
    return None, buf


def main():
    args = sys.argv[1:]
    to = 60
    if args and args[0] == "-t":
        to = int(args[1]); args = args[2:]
    s = serial.Serial(PORT, BAUD, timeout=0.1)
    s.write(b"\x03\r"); time.sleep(0.5); buf = rd(s)
    if "login:" in buf:
        for pw in PASSWDS:
            s.write(USER.encode() + b"\r"); expect(s, "Password:", 10)
            s.write(pw.encode() + b"\r")
            m, b = expect(s, r"\$ |login:", 15)
            if m and m.group(0) == "$ ":
                break
    s.write(b"stty -echo; bind 'set enable-bracketed-paste off' 2>/dev/null; export PAGER=cat SYSTEMD_PAGER=cat TERM=dumb\r")
    time.sleep(0.6); rd(s)
    rc_all = 0
    for i, cmd in enumerate(args):
        tag = f"__E{i}_{int(time.time())}"
        s.write(f"{cmd}; echo {tag}=$?=\r".encode())
        m, buf = expect(s, tag + r"=(\d+)=", to)
        if "[sudo] password" in buf:
            pass
        out = ANSI.sub("", buf[: m.start()] if m else buf).replace("\r", "")
        print(f"===== $ {cmd}")
        print(out.strip("\n"))
        if m:
            print(f"----- rc={m.group(1)}")
            rc_all |= int(m.group(1)) != 0
        else:
            print("----- TIMEOUT"); s.write(b"\x03\r"); rc_all = 1
    s.write(b"stty echo\r"); time.sleep(0.2); rd(s)
    sys.exit(rc_all)


main()
