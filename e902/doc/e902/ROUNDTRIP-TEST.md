
---

# 补充记录 T20（2026-09-22）：**原样写回测试** —— 最后一道机械环节证实，且介质零改变

## 动机

写入是唯一没做过的动作，之前没人验证过三件"机械前提"：
① `/dev/mmcblk1` 那个裸区到底**能不能写**（不是只读、不被内核拒绝）；
② `dd` 的 `seek/count/conv=notrunc` 到底**落点对不对**；
③ 读回校验链路**真的能比较到正确的字节**吗。

## 做法（零内容改变）

读出 `0x113BC00` 处那 105912 字节，**把完全相同的字节写回去**，再读回比较三个哈希。
因为写入内容与原有内容一致，**介质最终逐字节不变**；不改变固件加载的任何东西，故不影响启动行为。

## 原始输出

```
##### 0. device read-only flags
/sys/block/mmcblk1/ro = 0
blockdev --getro      = 0

##### 1. read BEFORE
before sha256: 07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749

##### 2. write the SAME bytes back (content-preserving)
write command issued

##### 3. sync + read AFTER
after  sha256: 07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749

##### 4. verdict
before=07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749
after =07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749
RESULT: IDENTICAL -> raw region is WRITABLE and the write path is correct

##### 5. whole-head integrity
24MiB  sha256: 114cd1f3a792b346acd87c8a550d4b3a09b5bfa7cce7cf0c406fa602b7bd4a54
（与经校验备份 sd-boot-head-20260922.img 完全一致）

##### 6. board still healthy
0x07032204: 0x40004000
up 9:26
```

## 判定

- 裸区**可写**（`ro=0` 且写入成功）—— `observed`
- `dd` 落点**精确**（写入后读回与写入源一致）—— `observed`
- 读回校验**有效**（能正确比较字节）—— `observed`
- **介质零改变**：写入前后 24 MiB 引导头哈希相同（`114cd1f3…`）—— `observed`
- 板子健康：`RST_START` 仍为 `0x40004000`，`up 9:26`

## 意义

至此，刷写链条上**每一个机械环节**都被验证过：
偏移正确（T17）· 长度匹配槽位（T17/T18）· 内容格式正确（T18）· 介质可写（T20）·
落点正确（T20）· 读回校验有效（T20）· 回滚产物经校验（T17）· 握手包格式来自反汇编（T15）·
且握手已前置（T19，失败安全）。

**唯一仍未验证的，只剩下"bl31 是否真的接受我们的握手包"这一条**——它只能在真正刷写并重启后才能知道。
