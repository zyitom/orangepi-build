import serial, time, os, sys
LOG='/home/helios/Desktop/orangepi-build/e902/verify-logs/arm-console-live-20260922.log'
BYID='/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'
f=open(LOG,'ab',buffering=0)
f.write(b'\n===== ARM daemon v4 %s =====\n' % time.strftime('%Y-%m-%d %H:%M:%S').encode())
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
        if d: f.write(d)
