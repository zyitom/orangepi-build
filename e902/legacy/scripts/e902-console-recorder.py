import serial, time, os, sys
LOG='/home/helios/Desktop/orangepi-build/e902/verify-logs/e902-console-live-20260922.log'
BYID='/dev/serial/by-id/usb-FTDI_USB__-__Serial_Converter_FTBB8C3B-if00-port0'
f=open(LOG,'ab',buffering=0)
f.write(b'\n===== E902 daemon v4 %s =====\n' % time.strftime('%Y-%m-%d %H:%M:%S').encode())
last=0.0
while True:
    try:
        s=serial.Serial(os.path.realpath(BYID),115200,timeout=1)
    except Exception:
        time.sleep(2); continue
    f.write(('\n-- port opened %s --\n'%time.strftime('%H:%M:%S')).encode())
    while True:
        try: d=s.read(4096)
        except Exception:
            try: s.close()
            except Exception: pass
            f.write(b'\n-- device lost --\n'); break
        now=time.time()
        if d:
            if last and now-last>3: f.write(('\n[%s]\n'%time.strftime('%H:%M:%S')).encode())
            elif not last: f.write(('\n[%s]\n'%time.strftime('%H:%M:%S')).encode())
            last=now
            f.write(d)
