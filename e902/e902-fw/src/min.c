/*
 * min.c - 最小可观测固件：判断 E902 是否真的在取指执行。
 *
 * 不初始化任何外设，只把自增计数器写到 SRAM_A2 的 0x40030000
 * （ARM 侧视图 0x00070000）。装载后从 ARM 侧读这个地址：
 *   值在变、形如 0xE902xxxx 递增 -> 核心确实在执行我们的代码
 *   值不变 / 还是旧内容          -> 核心没执行（问题在启动/复位序列）
 *
 * 为什么不写寄存器：避免"固件写寄存器失败"与"核心没跑"两种可能混淆。
 * SRAM_A2 两侧都能访问，写 SRAM 是最干净的探针。
 *
 * 注意：start.S 的 __start 会先调 soc_early_init() 再调 main()，
 * 所以这里给一个空实现，把外设初始化完全排除在这个测试之外。
 */
#define SCRATCH 0x40030000u	/* E902 视图；ARM 视图 = 0x00070000 */

void soc_early_init(void)
{
	/* 故意留空：本测试只在 SRAM 里留痕，不碰任何外设寄存器 */
}

void main(void)
{
	volatile unsigned int *p = (volatile unsigned int *)SCRATCH;
	unsigned int n = 0;

	for (;;)
		*p = 0xE9020000u | (n++ & 0xFFFFu);
}
