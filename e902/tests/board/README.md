# 上板测试脚本

板上脚本（先推过去：`cat e902/tests/board/X.py | ar0234-port/tools/ssh_board.sh 'cat > ~/e902v2/X.py'`，
再 `sudo python3 ~/e902v2/X.py`）：

- `rate.py`      —— 以 CLOCK_MONOTONIC 为基准，测 S_TIMER1 计数速率、共享时间戳速率、E902 tick 频率
- `mbping.py`    —— 系统空闲时从 Linux 发 200 个同步 ping 到 E902，核对回包、统计往返时间
- `mbw.py <val>` —— 往 ARM→E902 通道 3 写一个 32 位字（用于测试组包超时对齐）

主机脚本：

- `uarttest-host.py` —— 在 TL101 上直接对 /dev/ttyUSB1 做回显与按键命令测试（先停 e902-console-recorder.py）

所有 /dev/mem 访问 FIFO 寄存器都用 `memoryview(...).cast('I')`，保证单次 32 位读写。
