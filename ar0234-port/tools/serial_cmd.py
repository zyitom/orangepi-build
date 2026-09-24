#!/usr/bin/env python3
"""One-shot serial console runner for the Orange Pi board.

Usage: tools/serial_cmd.py "cmd1" "cmd2" ...
Logs in (orangepi/orangepi), runs each command, prints output with return code.
"""
import re
import sys
import time

import serial

PORT = "/dev/ttyUSB0"
BAUD = 115200
USER = "orangepi"
PASS = "orangepi"
RC_MARK = "__RC="


class Console:
    def __init__(self):
        self.s = serial.Serial(PORT, BAUD, timeout=0.2)

    def read_all(self):
        data = b""
        while True:
            chunk = self.s.read(4096)
            if not chunk:
                break
            data += chunk
        return data.decode("utf-8", "replace")

    def expect(self, pattern, timeout=20):
        rx = re.compile(pattern)
        buf = ""
        end = time.time() + timeout
        while time.time() < end:
            buf += self.read_all()
            m = rx.search(buf)
            if m:
                return m, buf
            time.sleep(0.05)
        return None, buf

    def send(self, line):
        self.s.write(line.encode() + b"\r")

    def run(self, cmd, timeout=60):
        self.send(f"{cmd}; echo {RC_MARK}$?__END__")
        m, buf = self.expect(RC_MARK + r"(\d+)__END__", timeout)
        if not m:
            return None, buf
        rc = int(m.group(1))
        # drop echoed command line and marker noise
        body = buf[: m.start()]
        body = re.sub(r"^\s*\r?\n", "", body)
        return rc, body

    def login(self):
        self.s.reset_input_buffer()
        self.send("")
        m, buf = self.expect(r"login:|[#\$] \s*$", 15)
        if m is None:
            return None, buf
        if "login:" in buf.split("\r\n")[-2] if len(buf.split("\r\n")) > 1 else buf:
            pass
        if re.search(r"login:", buf[-2000:]):
            self.send(USER)
            m, buf = self.expect(r"Password:", 15)
            if m is None:
                return None, buf
            self.send(PASS)
            time.sleep(1)
            self.send("stty -echo 2>/dev/null")
            time.sleep(0.5)
            m, buf = self.expect(r"[$#] $|[$#]\s*$", 15)
            return ("login_ok" if m else None), buf
        # already at a shell prompt
        return ("already", buf)


def main():
    cmds = sys.argv[1:]
    c = Console()
    time.sleep(0.3)
    print(c.read_all(), end="")  # residual buffer
    status, buf = c.login()
    print(buf, end="")
    if status is None:
        print("\n[!!] could not reach a login prompt / shell")
        return 2
    print(f"\n[**] session status: {status}")
    fail = 0
    for cmd in cmds:
        print(f"\n===== $ {cmd}")
        rc, out = c.run(cmd)
        if rc is None:
            print("[!!] timeout / no output:")
            print(out[-2000:])
            fail += 1
            continue
        print(out)
        print(f"----- rc={rc}")
        if rc != 0:
            fail += 1
    c.send("exit")
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
