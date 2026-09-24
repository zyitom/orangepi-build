#!/bin/bash
cd /home/helios/Desktop/orangepi-build/u-boot/v2018.05-sun60iw2 || exit 1
echo "########## monitor.fex (bl31) 里全部 [SCP] / arisc 相关字符串 ##########"
strings -n 6 monitor.fex | grep -iE 'SCP|arisc|ar100|cpus|riscv|mailbox|msgbox|syn|ready|reset' | sort -u

echo
echo "########## bl31 里其它可能的线索（分段） ##########"
strings -n 8 monitor.fex | grep -iE 'load .* image|para|de-assert|release|wait |notify|handshake|version' | sort -u | head -40

echo
echo "########## monitor.fex 基本信息 ##########"
ls -la monitor.fex
python3 - <<'PY'
b=open('/home/helios/Desktop/orangepi-build/u-boot/v2018.05-sun60iw2/monitor.fex','rb').read()
print("size:", len(b))
for magic in [b'[SCP]', b'arisc', b'BL31', b'ARISC']:
    print(magic, "count:", b.count(magic))
PY

echo
echo "########## 对照：scp.fex 里的握手相关字符串 ##########"
strings -n 6 scp.fex | grep -iE 'syn|notify|feedback|startup|ready|version|ac327' | sort -u
