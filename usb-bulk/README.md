# usb-bulk —— A733 作为 USB Device 的 BULK 数据通道

板子通过 **FunctionFS**(内核现成,`CONFIG_USB_F_FS=y`)变成一个带
两个 BULK 端点(OUT 0x01 / IN 0x81,高速 512B)的自定义 USB 设备;
PC 端用 **libusb/pyusb** 对端点做 bulk 收发。全程不需要写内核驱动。

```
usb-bulk/
├── ffs-bulk.c       板端守护进程(描述符 + ep0 处理 + echo/stream 数据环)
├── ffs-bulk         交叉编译好的 aarch64 二进制
├── bulk-gadget.sh   板端:otg 切 device → configfs 建 gadget → 绑 UDC → 起守护
└── host/bulk-test.py PC 端:pyusb 回环压测(echo 校验 + 吞吐)
```

## 板端(两条命令)

```bash
bash usb-bulk/bulk-gadget.sh          # 起 gadget + echo 模式守护
```

前提:OTG 口(USB2 Type-C)接到 PC。脚本会先把 usbc0 切到 device 角色
(`otg_role` 接口),没有 UDC 可绑时说明没进 device 模式/VBUS 没供电。

## PC 端

```bash
pip install pyusb                     # 需要 libusb-1.0
sudo python3 usb-bulk/host/bulk-test.py 8     # 回环 8 MB,打印吞吐
```

## 预期与注意

- OTG 口是 **USB2 高速**,理论 480 Mbit/s,实测回环 **~30-40 MB/s** 上下;
  USB3 的 Type-C 口是纯 HOST,不能当 device 用。
- `ffs-bulk` 支持两种模式:`echo`(默认,回环校验)和 `stream`
  (`bash usb-bulk/bulk-gadget.sh stream`,板子单向推送,测上限)。
- 断开/重连:守护进程检测到 host 分离会退出;重插后重跑 bulk-gadget.sh。
- 改协议:echo 换成你自己的命令字/帧格式即可;PC 端仍用 libusb bulk 收发。
- 交叉编译命令(也可在板上用 gcc 13.3 直接编):
  `aarch64-linux-gnu-gcc -O2 -D_GNU_SOURCE -o ffs-bulk ffs-bulk.c`
