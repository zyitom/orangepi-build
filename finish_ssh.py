#!/usr/bin/env python3
"""OPI5 kernel install via SSH (10.0.0.2 over the direct ethernet link).

Prereq: board eth0 cable plugged into the host's enp44s0 (static-172 profile,
10.0.0.1/30) and the host pubkey already authorized on the board.
Shares the same lock/done markers as the serial finisher.
Exit codes: 0 done/nothing-to-do, 3 network not available, 2/4/5 failures.
"""
import subprocess, os, sys, time, hashlib, fcntl

HOST = 'orangepi@10.0.0.2'
LOG = '/home/zyi/Desktop/orangepi5-build/finish-install.log'
DONE = '/tmp/opi5_finish.done'
LOCK = '/tmp/opi5_finish.lock'
DEBS = ['/home/zyi/Desktop/orangepi5-build/output/debs/linux-image-current-rockchip-rk3588_1.2.4_arm64.deb',
        '/home/zyi/Desktop/orangepi5-build/output/debs/linux-dtb-current-rockchip-rk3588_1.2.4_arm64.deb',
        '/home/zyi/Desktop/orangepi5-build/output/debs/linux-headers-current-rockchip-rk3588_1.2.4_arm64.deb']
KVER = '6.1.99-rt36-rockchip-rk3588'
STMMAC_KO = '/lib/modules/%s/kernel/drivers/net/ethernet/stmicro/stmmac/stmmac.ko' % KVER

def log(msg):
    line = '[%s][ssh] %s' % (time.strftime('%H:%M:%S'), msg)
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')

def sh(cmd, timeout=60):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr

def main():
    try:
        lf = open(LOCK, 'w'); fcntl.flock(lf, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        log('another run holds the lock; skip'); return 0
    if os.path.exists(DONE):
        return 0

    # bring up the host side of the link
    sh('nmcli con up static-172', 30)
    time.sleep(1)
    rc, out = sh('ping -c2 -W2 10.0.0.2', 30)
    if rc != 0:
        return 3                      # no link — silent for cron
    rc, out = sh('ssh -o BatchMode=yes -o ConnectTimeout=5 %s echo ok' % HOST, 20)
    if rc != 0:
        log('ping ok but ssh failed: %s' % out.strip()[:120]); return 3
    log('ssh channel up')

    # stage 1: already =m?
    rc, out = sh('ssh -o BatchMode=yes %s "test -e %s && echo 1 || echo 0"' % (HOST, STMMAC_KO), 20)
    installed = out.strip().endswith('1')
    if not installed:
        log('copying debs (gigabit link)...')
        rc, out = sh('scp -o BatchMode=yes %s %s:/tmp/' % (' '.join(DEBS), HOST), 300)
        if rc != 0:
            log('scp failed: %s' % out.strip()[:150]); return 4
        for d in DEBS:
            want = hashlib.md5(open(d, 'rb').read()).hexdigest()
            rc, out = sh('ssh -o BatchMode=yes %s "md5sum /tmp/$(basename %s)"' % (HOST, d), 30)
            if want not in out:
                log('md5 mismatch on board for %s' % d); return 4
        log('debs on board, md5 ok; installing...')
        rc, out = sh('ssh -o BatchMode=yes %s "echo orangepi | sudo -S dpkg -i /tmp/linux-image-current-rockchip-rk3588_1.2.4_arm64.deb /tmp/linux-dtb-current-rockchip-rk3588_1.2.4_arm64.deb /tmp/linux-headers-current-rockchip-rk3588_1.2.4_arm64.deb > /tmp/dpkg.log 2>&1; echo RC=\\$?"' % HOST, 420)
        if 'RC=0' not in out:
            log('dpkg failed: %s' % out.strip()[:200]); return 2
        log('dpkg ok; rebooting...')
        sh('ssh -o BatchMode=yes %s "echo orangepi | sudo -S reboot"' % HOST, 20)
        time.sleep(20)
    else:
        log('kernel already =m; skipping install/reboot')

    # wait for ssh to come back
    up = False
    for i in range(40):
        time.sleep(8)
        rc, out = sh('ssh -o BatchMode=yes -o ConnectTimeout=5 %s echo ok' % HOST, 20)
        if rc == 0: up = True; break
    if not up:
        log('board did not come back via ssh in ~5 min'); return 5
    log('board back up')
    time.sleep(5)

    report = []
    rc, out = sh('ssh -o BatchMode=yes %s "uname -r"' % HOST, 20)
    report.append('uname: %s' % out.strip())
    rc, out = sh('ssh -o BatchMode=yes %s "lsmod | grep -c dwmac"' % HOST, 20)
    report.append('dwmac modules loaded: %s' % out.strip())
    rc, out = sh('ssh -o BatchMode=yes %s "ip -br addr show eth0"' % HOST, 20)
    report.append('eth0: %s' % out.strip().replace('\n', ' | '))
    rc, out = sh('ssh -o BatchMode=yes %s "echo orangepi | sudo -S /usr/local/sbin/ecat-native start >/tmp/ecatstart.log 2>&1; echo RC=\\$?"' % HOST, 90)
    if 'RC=0' in out:
        time.sleep(5)
        rc, out = sh('ssh -o BatchMode=yes %s "/usr/local/bin/ethercat slaves 2>&1 | head -2"' % HOST, 30)
        report.append('slaves: %s' % out.strip().replace('\n', ' / '))
        rc, out = sh('ssh -o BatchMode=yes %s "test -x /tmp/regression.sh && echo 1 || echo 0"' % HOST, 20)
        if out.strip().endswith('1'):
            log('running regression.sh 20 (~3 min)...')
            rc, out = sh('ssh -o BatchMode=yes %s "echo orangepi | sudo -S sh -c \\"cd /tmp && ./regression.sh 20\\" 2>&1 | tail -6"' % HOST, 900)
            report.append('regression tail: ' + out.strip()[-500:].replace('\n', ' | '))
        sh('ssh -o BatchMode=yes %s "echo orangepi | sudo -S /usr/local/sbin/ecat-native stop"' % HOST, 90)
        time.sleep(3)
        rc, out = sh('ssh -o BatchMode=yes %s "ip -br addr show eth0"' % HOST, 20)
        report.append('eth0 after ecat stop: %s' % out.strip().replace('\n', ' | '))
    else:
        report.append('ecat-native start failed (board /tmp/ecatstart.log)')

    sh('ssh -o BatchMode=yes %s "echo orangepi | sudo -S touch /tmp/.install_done"' % HOST, 20)
    open(DONE, 'w').write('ok\n')
    log('=== FINISH REPORT ===\n%s\n=====================' % '\n'.join(report))
    log('ALL DONE (marker %s)' % DONE)
    return 0

sys.exit(main())
