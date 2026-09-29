# npu-run —— 不依赖 OpenCV 的 A733 NPU 推理小程序

`npurun` 用 vip_lite API 把任意 `.nb`（NBG）模型跑在 A733 NPU 上，输入文件原样灌进
输入张量（vpm_run 的 `input_0.dat` 可以直接用），打印每轮耗时。板上实测 2.95 ms，
与厂家 vpm_run 一致。

```sh
./npurun model.nb [input.dat [loops]]
```

板上需要 NPU 用户态库（libNBGlinker + libVIPhal + vipcore 驱动，镜像里已有）；
libVIPlite 只给老的 vip_lite 编译流程用，推理不需要。

## 构建（宿主机交叉编译）

头文件和库来自 orangepi-build 的厂家包缓存（`./build.sh` 跑过一次就有）：

```sh
P=external/cache/sources/sun60iw2_packages/npu/usr
aarch64-linux-gnu-gcc -O2 -Wall -I$P/include -o npu-run/npurun npu-run/npurun.c \
    -L$P/lib -lNBGlinker -lVIPhal -Wl,--allow-shlib-undefined
```

`npurun` 是按上面命令编好的 aarch64 二进制，拷到板上即可运行。

坑：`VIP_BUFFER_PROP_SIZES_OF_DIMENSION` 的值是调用方提供的数组本身，不是返回的
指针——传 `&ptr` 会段错误。
