#!/bin/sh
# E902 / SCP 现状体检 —— 只读，不改任何寄存器
# 在 A733 板子上以 root 运行：sudo sh check-e902.sh
#
# 依赖只有 python3（官方镜像自带）。原来用 devmem2 / busybox devmem，
# 这台板子两个都没装；而且读 SRAM_A2 必须走 mmap，dd 的 lseek+write
# 会被内核拒绝（Bad address）。地址换算见 awdevmem.py 的说明：
# SRAM_A2 在 E902 视角是 0x40000000，在 ARM 视角是 0x00040000，
# 而 ARM 的 0x40000000 是 DRAM。

# awdevmem.py 与本脚本同目录，或在 e902-fw/scripts/ 下
MEM=""
for c in "$(dirname "$0")/awdevmem.py" \
         "$(dirname "$0")/../../e902-fw/scripts/awdevmem.py" \
         /tmp/awdevmem.py; do
    [ -f "$c" ] && { MEM="$c"; break; }
done

RD() {  # RD <addr> <label>
    if [ -n "$MEM" ] && command -v python3 >/dev/null 2>&1; then
        v=$(python3 "$MEM" read "$1" 2>/dev/null | sed 's/.*: //')
        [ -n "$v" ] || v="(读取失败)"
    else
        v="(需要 python3 + awdevmem.py)"
    fi
    printf '  %-14s %-12s %s\n' "$1" "$v" "$2"
}

echo "===== 1. 内核编了什么 ====="
if [ -r /proc/config.gz ]; then
    zcat /proc/config.gz | grep -E \
      "^CONFIG_(AW_REMOTEPROC|AW_MSGBOX|AW_HWSPINLOCK|AW_RPMSG|AW_RPBUF|AW_DMC_DEVFREQ|AW_WAKEUPGEN|AW_STANDBY|SUSPEND|HIBERNATION)" \
      || echo "  (相关项全部未设置)"
else
    echo "  /proc/config.gz 不可读"
fi

echo
echo "===== 2. remoteproc 实例（空=Linux 没管 E902）====="
ls /sys/class/remoteproc/ 2>/dev/null || echo "  (无)"

echo
echo "===== 3. 设备树里有没有 E902 节点 ====="
find /proc/device-tree -maxdepth 3 \
     \( -iname "*riscv*" -o -iname "*e902*" -o -iname "*rproc*" \) 2>/dev/null \
     || echo "  (无)"

echo
echo "===== 4. msgbox / hwspinlock 驱动绑上了吗 ====="
for d in msgbox hwspinlock; do
    printf '  %-12s ' "$d:"
    ls -d /sys/bus/platform/drivers/*$d* 2>/dev/null | tr '\n' ' '
    echo
done
echo "  --- 已绑定的设备 ---"
# 注意：未绑定时 $n/driver 这个符号链接根本不存在，readlink -f 会回显
# 输入本身（".../driver"），basename 于是打印 "driver" —— 看起来像绑上了。
# 必须先用 -L 判断链接是否存在。
for n in /sys/bus/platform/devices/*msgbox* /sys/bus/platform/devices/*spinlock*; do
    [ -e "$n" ] || continue
    if [ -L "$n/driver" ]; then
        drv=$(basename "$(readlink -f "$n/driver")")
    else
        drv="<未绑定>"
    fi
    printf '  %-28s driver=%s\n' "$(basename "$n")" "$drv"
done
echo "  --- 节点 compatible（驱动匹配靠它）---"
for n in /proc/device-tree/soc@3000000/msgbox@3004000 \
         /proc/device-tree/soc@3000000/hwspinlock@3005000; do
    [ -e "$n/compatible" ] || continue
    printf '  %-22s %s\n' "$(basename "$n")" "$(tr -d '\0' < "$n/compatible")"
done

echo
echo "===== 5. E902 寄存器实际状态 ====="
RD 0x0701021C "RISCV_BGR: bit16=复位释放 bit1/0=时钟"
RD 0x07032204 "E902 起始 PC (厂商实测 0x40004000)"
RD 0x07032000 "E902_VER"
RD 0x07032004 "E902_AUTO_GATING"
RD 0x07032020 "E902_DDR_REMAP"
RD 0x07032064 "WAKEUP_MASK0 (bit0=irq16)"
RD 0x07032068 "WAKEUP_MASK1 (bit0=irq48)"
RD 0x07032080 "PAD_LPMD (3=正常 0=睡眠)"

echo
echo "===== 6. 内存变频（走 E902）====="
if [ -d /sys/class/devfreq ] && [ -n "$(ls /sys/class/devfreq 2>/dev/null)" ]; then
    for d in /sys/class/devfreq/*/; do
        echo "  $d"
        for f in cur_freq target_freq governor available_frequencies trans_stat; do
            [ -r "$d$f" ] && printf '    %-24s %s\n' "$f:" "$(head -c 200 $d$f | tr '\n' ' ')"
        done
    done
else
    echo "  /sys/class/devfreq 为空 → DMC 驱动没加载，内存变频本来就没在用"
fi
printf '  dmc 模块: '; lsmod 2>/dev/null | grep -i "dmc\|devfreq" || echo "(未加载)"

echo
echo "===== 7. CPU 变频（不走 E902，应该正常）====="
for p in /sys/devices/system/cpu/cpufreq/policy*/; do
    [ -d "$p" ] || continue
    printf '  %s  cur=%s  gov=%s\n' "$(basename $p)" \
        "$(cat $p/scaling_cur_freq 2>/dev/null)" \
        "$(cat $p/scaling_governor 2>/dev/null)"
done

echo
echo "===== 8. suspend 支持的模式 ====="
printf '  /sys/power/state: '; cat /sys/power/state 2>/dev/null
printf '  mem_sleep:        '; cat /sys/power/mem_sleep 2>/dev/null

echo
echo "===== 9. 相关 dmesg ====="
dmesg 2>/dev/null | grep -iE "msgbox|rproc|remoteproc|arisc|scp|dmcfreq|devfreq|dram clock|wakeupgen" \
    | tail -25 || echo "  (无匹配)"

echo
echo "===== 10. SRAM_A2 里 E902 固件的前 64 字节 ====="
echo "  (看到 81 40 01 41 … 说明 scp.fex 已加载并在跑)"
echo "  地址按 E902 视角给出，awdevmem.py 内部换算到 ARM 侧 0x00040000+"
echo "  --- 复位向量处 0x40004000（厂商 scp.fex 实际入口）---"
if [ -n "$MEM" ] && command -v python3 >/dev/null 2>&1; then
    python3 "$MEM" dump --e902 0x40004000 --count 64 2>&1 | sed 's/^/  /'
    echo "  --- 0x40014000（我们自己固件的位置，在厂商镜像内部）---"
    python3 "$MEM" dump --e902 0x40014000 --count 32 2>&1 | sed 's/^/  /'
else
    echo "  (需要 python3 + awdevmem.py)"
fi
echo
echo "  注意：绝不要用 dd if=/dev/mem skip=\$((0x40014000)) 去读/写这块。"
echo "  ARM 侧 0x40014000 是普通系统内存（/proc/iomem: System RAM 从"
echo "  0x40000000 起），本机 CONFIG_STRICT_DEVMEM 未开启，写进去会直接"
echo "  破坏内核正在用的页，而且 dd 会返回成功。"
