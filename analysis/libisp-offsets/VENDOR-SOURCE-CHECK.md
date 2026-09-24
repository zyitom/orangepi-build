# libisp 参数布局：用厂商源码核对（2026-09-23）

## 源码来源与版本

- 全志公开 GitLab（无 NDA）：`gitlab.com/tina5.0_aiot/product/linux/external/libAWIspApi`，
  分支 product-aiot-stable（aiot-linux-v1.5.0，2026-07-15）；A733 的 ISP602 在 `isp6xx/`。
  本地稀疏克隆：`ar0234-port/vendor-ref/libAWIspApi`（只含 isp6xx）。
- **板上 `/usr/lib/aarch64-linux-gnu/libisp.so` 内嵌的构建信息 = commit `665dc2e9613716c25abb6b64af145a73b989271e`
  （libisp-dev，2024-12-18）—— 与源码 `libisp/isp_version.h` 完全相同。** 源码就是编出板上库的那份。
- 开源部分：框架（isp.c、isp_manage、isp_dev、isp_tuning、ini 解析、tuning_app 调参服务端）。
  闭源部分：3A 算法 `libisp_algo.a/.so`（有 `out/isp602_debian/` 的 gcc-10.2 / 14.2 版本），接口头 `include/isp_3a_*.h`。

## 布局结论

必须按 `-DISP_VERSION=602` 编译头文件（602 有条件字段，AWB 光源表移出 3A 结构）：

| | 601（内核头、旧 offs.txt） | 602（板上真实） |
|---|---|---|
| sizeof(isp_param_config) | 119204 | **116284**（参数文件 116358 = 116284 + 74 字节头） |
| isp_3a_param | 5760 | 2724 |
| isp_dynamic_param | 13936 | 13996 |
| 调参结构起点 | — | 2848 |

`tools/make_isp_bin.py` 使用的全部偏移都与 602 源码逐一一致：
LSC 3124/3130/21562、MSC 21574/21576/21598/21620/21632、CCM 87210/87282、GAMMA 56480、
AWB CT 1088/1090、AWB light 1098/1104、dynamic cfg 102480 × 986（black_level +242）、模块开关 88.. 。
→ 以往生成的 isp_param_*.bin 没有写错位置；"bayer_gain 前插 2848 字节"其实是 602 下调参结构的起点。
`offs.txt` 是 601 布局，已加警告，不要再用它的 3A 偏移。

复现：`gcc -DISP_VERSION=602 -I<isp6xx>/libisp/isp_tuning -I<isp6xx>/libisp/include -I<isp6xx>/libisp abs602.c`

## 对 AR0234 各任务的用处

- T20 标定：结构体定义可直接替代逆向偏移；LSC 通道顺序、中心点等语义可查 `isp_rolloff.h` / isp_tuning 源码。
- T3 工业固定模式：正式接口 `isp_set_attr_cfg/isp_get_attr_cfg`、`isp_set_fps`、`isp_get_3a_parameters`、`isp_stats_req`。
- 调参：`tuning_app/server` 是 PC 调参工具的板端服务源码（板上原先没有这个程序）。
- 日志：`include/isp_debug.h` 的 ISP_LOG_AE=0x1、ISP_LOG_AWB=0x2…（与之前 isp_log_param=0x3 一致）。
- 仓库没有 LICENSE 文件：只作内部参考，不要放进公开仓库。
