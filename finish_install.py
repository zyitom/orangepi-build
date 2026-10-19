#!/usr/bin/env python3
"""OPI5 STMMAC=m kernel install — self-healing finisher.

Idempotent: safe to run repeatedly. Skips stages already done.
Flow: console-alive? -> push rx2.py -> ensure 3 debs on board (md5) ->
dpkg -i -> reboot (only if installed this run) -> post-boot verify
(eth0 auto-load) -> ecat-native start + regression + stop -> markers.

Exit codes: 0 done/nothing-to-do, 3 board not alive (silent retry later),
2/4/5 stage failures (see log).
"""
import serial, os, sys, time, re, binascii, base64, hashlib, fcntl

ANSI_RE = re.compile(rb'\x1b\[[0-9;?]*[a-zA-Z]')

PORT = '/dev/ttyUSB0'; BAUD = 1500000
LOG = '/home/zyi/Desktop/orangepi5-build/finish-install.log'
DONE = '/tmp/opi5_finish.done'
LOCK = '/tmp/opi5_finish.lock'
RX2_LOCAL = '/tmp/rx2_board.py'
DEBS = {
    'linux-image':   '/home/zyi/Desktop/orangepi5-build/output/debs/linux-image-current-rockchip-rk3588_1.2.4_arm64.deb',
    'linux-dtb':     '/home/zyi/Desktop/orangepi5-build/output/debs/linux-dtb-current-rockchip-rk3588_1.2.4_arm64.deb',
    'linux-headers': '/home/zyi/Desktop/orangepi5-build/output/debs/linux-headers-current-rockchip-rk3588_1.2.4_arm64.deb',
}
BOARD_TMP = {'linux-image': '/tmp/linux-image.deb',
             'linux-dtb': '/tmp/linux-dtb.deb',
             'linux-headers': '/tmp/linux-headers.deb'}
KVER = '6.1.99-rt36-rockchip-rk3588'
STMMAC_KO = '/lib/modules/%s/kernel/drivers/net/ethernet/stmicro/stmmac/stmmac.ko' % KVER

def log(msg):
    line = '[%s] %s' % (time.strftime('%H:%M:%S'), msg)
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')

class Board:
    def __init__(self):
        self.ser = serial.Serial(PORT, BAUD, timeout=0.2)
        self.n = 0
    def close(self):
        try: self.ser.close()
        except Exception: pass
    def drain(self, t):
        end = time.time() + t; buf = b''
        while time.time() < end:
            d = self.ser.read(8192)
            if d: buf += d
        return buf
    def connect(self):
        """Robust session start: exit any rx2 receiver holding the tty, escape
        any debug capture, then probe for shell/login. Returns like alive()."""
        for t in range(3):
            self.ser.write(b'Z\n'); time.sleep(0.6); self.drain(0.3)
            self.ser.write(b'\nconsole\n'); time.sleep(0.5); self.drain(0.3)
            self.ser.write(b'stty sane\n'); time.sleep(0.4); self.drain(0.3)
            st = self.alive()
            if st: return st
        return None
    def alive(self):
        self.ser.write(b'\r\n'); buf = self.drain(2.5)
        if b'login:' in buf: return 'login'
        if b'orangepi@' in buf or b'$ ' in buf or b'# ' in buf: return 'shell'
        self.ser.write(b'\r\n'); buf = self.drain(2.5)
        if b'login:' in buf: return 'login'
        if b'orangepi@' in buf or b'$ ' in buf or b'# ' in buf: return 'shell'
        return None
    def exit_rx2(self):
        """If an rx2.py receiver holds the tty (raw mode), 'Z' makes it exit
        cleanly and restore termios; harmless if none is running. The leading
        newline flushes any partial line stuck in the receiver's buffer —
        without it the 'Z' glues onto residue and never matches."""
        self.ser.write(b'\nZ\n\n'); time.sleep(0.8); self.drain(0.3)
        self.run('stty sane', 8)
        self.run('pkill -f rx2.py', 8)   # belt-and-braces; no-op if none left
    def setup_console(self):
        """Minimal console reset: escape any fresh debug capture ('console'
        is a no-op/"not found" in normal shell mode), then stty sane.

        NOTE: do NOT synthesize BREAKs here (baud-drop 0x00 trick) — the
        resulting board-side break condition poisons the console input path
        (the tty ring stops draining after ~1KB, freezing the receiver).
        Empirically the plain console survives ~181 chunks per tty session;
        the windowed receiver restart in xfer() handles that."""
        self.ser.write(b'\r\n'); time.sleep(0.4); self.drain(0.3)
        self.ser.write(b'\nconsole\n'); time.sleep(0.4); self.drain(0.3)
        self.ser.write(b'stty sane\n'); time.sleep(0.3); self.drain(0.3)
    def run_checked(self, cmd, timeout=30, tries=3):
        """run() with resend on marker loss (console drops chars randomly)."""
        for t in range(tries):
            rc, out = self.run(cmd, timeout)
            if rc is not None:
                return rc, out
            log('  run_checked retry %d: %s' % (t+1, cmd[:50]))
        return rc, out
    def login(self):
        self.ser.write(b'orangepi\n')
        buf = self.drain(2.0)
        if b'assword' in buf:
            self.ser.write(b'orangepi\n'); self.drain(2.5)
    def run(self, cmd, timeout=30):
        """Run cmd; return (rc_str_or_None, output).

        Marker trick: the command is sent as 'n=<k>; <cmd>; echo Z${n}D' so the
        shell ECHO contains the literal 'Z${n}D' while the real output line is
        'Z<k>D' — the echo can never match the marker.
        """
        self.n += 1
        m = b'Z%dD' % self.n
        self.ser.reset_input_buffer()
        self.ser.write(b'n=%d; ' % self.n + cmd.encode() + b'; echo Z${n}D\n')
        buf = b''; end = time.time() + timeout
        while time.time() < end:
            d = self.ser.read(8192)
            if d:
                buf += d
                clean = ANSI_RE.sub(b'', buf.replace(b'\r', b''))
                if m in clean:
                    out = clean.split(m, 1)[0]
                    rc = None
                    lines = out.decode(errors='replace').strip().splitlines()
                    for ln in reversed(lines):
                        if ln.strip().startswith('RC='):
                            rc = ln.strip()[3:]; break
                    return rc, out.decode(errors='replace')
        return None, buf.decode(errors='replace')
    def md5(self, path):
        rc, out = self.run('md5sum %s' % path, 20)
        for ln in out.splitlines():
            ln = ln.strip()
            if len(ln) >= 33 and ln[32] == ' ':
                try:
                    int(ln[:32], 16)
                    return ln[:32]
                except ValueError:
                    pass
        return None
    def flag(self, cmd):
        """For commands ending in '&& echo 1 || echo 0'; parse output, not rc."""
        rc, out = self.run(cmd, 15)
        for ln in reversed([l.strip() for l in out.splitlines()]):
            if ln in ('0', '1'):
                return ln == '1'
        return False
    def push_small(self, local, remote):
        src = open(local, 'rb').read()
        want = hashlib.md5(src).hexdigest()
        self.exit_rx2()
        b64 = base64.b64encode(src).decode()
        lines = [b64[i:i+55] for i in range(0, len(b64), 55)]
        self.run('rm -f %s.b64' % remote, 10)
        for ln in lines:
            self.ser.write(('echo %s >>%s.b64' % (ln, remote)).encode() + b'\n')
            time.sleep(0.22)
        time.sleep(0.5); self.ser.reset_input_buffer()
        self.run('base64 -d %s.b64 >%s' % (remote, remote), 15)
        return self.md5(remote) == want
    def xfer(self, local, remote):
        data = open(local, 'rb').read()
        want = hashlib.md5(data).hexdigest()
        CH = 768
        WIN = 150   # chunks per receiver session; the fiq debugger console
        # wedges hard after ~181 chunks (~141KB) of sustained TX in one tty
        # session (kernel bug, no in-band recovery). The receiver supports
        # append/resume (seq recovered from file size), so each session is
        # closed with 'Z' and restarted — file state is the single source of
        # truth, making the whole transfer resumable across sessions AND runs.
        b64 = base64.b64encode(data).decode()
        nch = (len(b64) + CH - 1) // CH
        t0 = time.time(); i = 0
        while i < nch:
            # --- start a receiver session, learn its seq from RX2READY ---
            seq = None
            for t in range(5):
                self.exit_rx2()
                self.setup_console()
                self.ser.write(b'\n'); time.sleep(0.4); self.drain(0.3)  # settle after console restore
                self.ser.reset_input_buffer()
                self.ser.write(('python3 /tmp/rx2.py %s a\n' % remote).encode())
                buf = b''; end = time.time() + 3.0
                while time.time() < end:
                    d = self.ser.read(256)
                    if d:
                        buf += d
                        m = re.search(rb'RX2READY\s+(\d+)', buf.replace(b'\r', b''))
                        if m:
                            seq = int(m.group(1)); break
                if seq is not None: break
                log('  rx2 start attempt %d failed; got %r' % (t+1, bytes(buf)[-120:]))
            if seq is None:
                log('xfer FAIL: receiver would not start')
                return False
            if seq != i:
                log('  session start: receiver at chunk %d (sender was %d)' % (seq, i))
            i = seq
            self.ser.reset_input_buffer()
            session_end = min(i + WIN, nch)
            stuck = 0
            # --- transfer this session's window ---
            while i < session_end:
                last_i = i
                chunk = b64[i*CH:(i+1)*CH].encode()
                crc = binascii.crc_hqx(chunk, 0)
                line = b'S%04x:%s:%04x\n' % (i & 0xffff, chunk, crc)
                for attempt in range(10):
                    self.ser.write(line)
                    r = self.ser.read(1)
                    if r:
                        r += self.ser.read(max(1, self.ser.in_waiting))
                    # scan for authoritative receiver state: \nK%04x\n
                    nxt = None
                    for m in re.finditer(rb'\nK([0-9a-f]{4})\n', b'\n' + r + b'\n'):
                        nxt = int(m.group(1), 16)
                    if nxt is not None and nxt <= nch:
                        if nxt != i and nxt != i + 1:
                            log('  resync: sender at %d, receiver at %d' % (i+1, nxt))
                        i = nxt      # trust the receiver; resend/advance/rewind
                        break
                    if attempt % 3 == 2:  # likely debug capture: escape it
                        self.ser.write(b'\nconsole\n')
                        time.sleep(0.25)
                if i == last_i:      # no progress this round
                    stuck += 1
                    if stuck >= 5:
                        break        # abandon session; fresh one resumes
                else:
                    stuck = 0
                if i and i % 1024 == 0:
                    log('  xfer %d/%d chunks (%.0f KB/s)' % (i, nch, i*768/(time.time()-t0)/1024))
            # --- close session (flushes file), console will be reset on next start ---
            self.ser.write(b'Z\n'); time.sleep(0.8); self.drain(0.3)
        self.exit_rx2()
        got = None
        for _ in range(3):
            got = self.md5(remote)
            if got == want: break
        if got != want:
            log('  md5 mismatch after transfer: %s; dropping partial file' % got)
            self.run_checked('rm -f %s; echo RC=$?' % remote, 15)
            return False
        return True

def main():
    try:
        lf = open(LOCK, 'w'); fcntl.flock(lf, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        log('another run holds the lock; skip'); return 0
    if os.path.exists(DONE):
        return 0

    b = Board()
    try:
        st = b.connect()
        if st is None:
            return 3          # board not alive yet — cron retries later
        log('console alive (%s)' % st)
        if st == 'login':
            b.login()
        b.setup_console()

        # stage 0: rx2.py receiver
        want_rx = hashlib.md5(open(RX2_LOCAL, 'rb').read()).hexdigest()
        if b.md5('/tmp/rx2.py') != want_rx:
            log('pushing rx2.py receiver...')
            if not b.push_small(RX2_LOCAL, '/tmp/rx2.py'):
                log('FATAL: rx2 push failed'); return 2
            log('rx2.py ok')

        # stage 1: are we ALREADY running the =m kernel? (previous run installed+rebooted)
        installed = b.flag('test -e %s && echo 1 || echo 0' % STMMAC_KO)

        # stage 2: transfer missing debs
        if not installed:
            for name, local in DEBS.items():
                remote = BOARD_TMP[name]
                want = hashlib.md5(open(local, 'rb').read()).hexdigest()
                if b.md5(remote) == want:
                    log('%s already on board (md5 ok)' % name); continue
                log('transferring %s (%.1f MB)...' % (name, os.path.getsize(local)/1e6))
                ok = b.push_small(local, remote) if name == 'linux-dtb' else b.xfer(local, remote)
                if not ok:
                    log('FATAL: transfer failed for %s' % name); return 4
                log('%s transferred + md5 ok' % name)

            # stage 3: install + reboot
            log('installing debs (dpkg)...')
            rc, out = b.run_checked('echo orangepi | sudo -S dpkg -i /tmp/linux-image.deb /tmp/linux-dtb.deb /tmp/linux-headers.deb >/tmp/dpkg.log 2>&1; echo RC=$?', 420)
            if rc != '0':
                log('FATAL: dpkg rc=%s (board /tmp/dpkg.log)' % rc); return 2
            log('dpkg ok; rebooting board...')
            b.run('echo orangepi | sudo -S reboot', 15)
            time.sleep(12)
            up = False
            for i in range(60):
                time.sleep(8)
                s = b.alive()
                if s == 'login':
                    b.login(); up = True; break
                if s == 'shell':
                    up = True; break
            if not up:
                log('FATAL: board did not come back within 8 min'); return 5
            log('board back up')
        else:
            log('kernel already =m (stmmac.ko present); skipping install/reboot')

        # stage 4: verify
        report = []
        rc, out = b.run('uname -r', 15)
        lines = [l for l in out.strip().splitlines() if l.strip()]
        report.append('uname: %s' % (lines[-1].strip() if lines else '?'))
        rc, out = b.run('lsmod | grep -c dwmac', 15)
        lines = [l for l in out.strip().splitlines() if l.strip()]
        cnt = lines[-1].strip() if lines else '?'
        report.append('dwmac modules loaded: %s' % cnt)
        if cnt in ('0', '?'):
            log('dwmac not auto-loaded; manual modprobe...')
            b.run('echo orangepi | sudo -S modprobe dwmac_rk', 20)
            time.sleep(3)
            rc, out = b.run('lsmod | grep -c dwmac', 15)
            lines = [l for l in out.strip().splitlines() if l.strip()]
            cnt = lines[-1].strip() if lines else '?'
            report.append('dwmac after modprobe: %s' % cnt)
        rc, out = b.run('ip -br addr show eth0', 15)
        lines = [l for l in out.strip().splitlines() if l.strip()]
        report.append('eth0: %s' % (lines[-1].strip() if lines else 'MISSING'))

        # stage 5: ecat-native round trip + regression
        rc, out = b.run('echo orangepi | sudo -S /usr/local/sbin/ecat-native start >/tmp/ecatstart.log 2>&1; echo RC=$?', 90)
        if rc == '0':
            time.sleep(5)
            rc, out = b.run('/usr/local/bin/ethercat slaves 2>&1 | head -2', 30)
            report.append('slaves: %s' % out.strip().replace('\n', ' / '))
            rc, out = b.run('test -x /tmp/regression.sh && echo 1 || echo 0', 10)
            has_reg = any(l.strip() == '1' for l in out.strip().splitlines())
            if has_reg:
                log('running regression.sh 20 (~3 min)...')
                rc, out = b.run('echo orangepi | sudo -S sh -c "cd /tmp && ./regression.sh 20" 2>&1 | tail -6', 900)
                report.append('regression tail: ' + out.strip()[-500:].replace('\n', ' | '))
            b.run('echo orangepi | sudo -S /usr/local/sbin/ecat-native stop', 90)
            time.sleep(3)
            rc, out = b.run('ip -br addr show eth0', 15)
            lines = [l for l in out.strip().splitlines() if l.strip()]
            report.append('eth0 after ecat stop: %s' % (lines[-1].strip() if lines else 'MISSING'))
        else:
            report.append('ecat-native start rc=%s (board /tmp/ecatstart.log)' % rc)

        # markers + report on board
        b.run('rm -f /tmp/finish_report.txt', 10)
        b.ser.reset_input_buffer()
        for ln in report:
            safe = ln.replace("'", '').replace('|', '/')
            b.ser.write(("echo '%s' >> /tmp/finish_report.txt\n" % safe).encode())
            time.sleep(0.15)
        b.run('echo orangepi | sudo -S touch /tmp/.install_done', 10)
        open(DONE, 'w').write('ok\n')
        log('=== FINISH REPORT ===\n%s\n=====================' % '\n'.join(report))
        log('ALL DONE (marker %s)' % DONE)
        return 0
    finally:
        b.close()

sys.exit(main())
