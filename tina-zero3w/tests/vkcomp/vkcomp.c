/* vkcomp.c — Vulkan 计算完整性测试：SPIR-V compute shader 分派 + 结果校验。
 * shader（comp.comp → comp.spv → comp_spv_inline.h）：vals[i] = i*2 + 42，256 线程
 * 通过 = 所有值精确匹配 → GPU 真实执行了 shader。每一步失败都打印在哪一步、VkResult 多少。
 *
 * 交叉编译（宿主机，Buildroot 工具链 + 任意 Vulkan 头文件）：
 *   aarch64-buildroot-linux-gnu-gcc -O2 -I<vulkan 头目录> -o vkcomp vkcomp.c -lvulkan -lm
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* SPIR-V 是 32 位字流：数组按 4 字节对齐，codeSize 按字节给 */
static const unsigned char spv[] __attribute__((aligned(4))) = {
#include "comp_spv_inline.h"
};

#define N 256

#define CHECK(what, call) do { VkResult r_ = (call); \
	if (r_ != VK_SUCCESS) { printf("FAIL at %s: VkResult %d\n", what, r_); return 1; } } while (0)

int main(void)
{
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vkcomp", 0, 0, 0, VK_API_VERSION_1_0};
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, 0, 0};
	VkInstance inst;
	CHECK("vkCreateInstance", vkCreateInstance(&ici, 0, &inst));

	uint32_t nd = 1;
	VkPhysicalDevice pd;
	VkResult r = vkEnumeratePhysicalDevices(inst, &nd, &pd);
	if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || nd == 0) { printf("FAIL: no physical devices\n"); return 1; }
	VkPhysicalDeviceProperties pp;
	vkGetPhysicalDeviceProperties(pd, &pp);
	printf("GPU: %s (api %u.%u.%u)\n", pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion),
	       VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion));

	/* 找一个支持 compute 的队列族 */
	VkQueueFamilyProperties qfp[8];
	uint32_t nq = 8, qf = UINT32_MAX;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qfp);
	for (uint32_t i = 0; i < nq; i++)
		if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }
	if (qf == UINT32_MAX) { printf("FAIL: no compute queue family\n"); return 1; }

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, qf, 1, &prio};
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qci, 0, 0, 0, 0, 0};
	VkDevice dev;
	CHECK("vkCreateDevice", vkCreateDevice(pd, &dci, 0, &dev));
	VkQueue queue;
	vkGetDeviceQueue(dev, qf, 0, &queue);

	/* buffer: N floats, HOST_VISIBLE|HOST_COHERENT */
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, N * sizeof(float),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
	VkBuffer buf;
	CHECK("vkCreateBuffer", vkCreateBuffer(dev, &bci, 0, &buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t mi = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { mi = i; break; }
	if (mi == UINT32_MAX) { printf("FAIL: no host-visible coherent memory type\n"); return 1; }
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mi};
	VkDeviceMemory mem;
	CHECK("vkAllocateMemory", vkAllocateMemory(dev, &mai, 0, &mem));
	CHECK("vkBindBufferMemory", vkBindBufferMemory(dev, buf, mem, 0));
	void *map;
	CHECK("vkMapMemory", vkMapMemory(dev, mem, 0, N * sizeof(float), 0, &map));
	memset(map, 0, N * sizeof(float));

	/* descriptor */
	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
	VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 1, &ps};
	VkDescriptorPool pool;
	CHECK("vkCreateDescriptorPool", vkCreateDescriptorPool(dev, &pci, 0, &pool));
	VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0};
	VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &lb};
	VkDescriptorSetLayout dsl;
	CHECK("vkCreateDescriptorSetLayout", vkCreateDescriptorSetLayout(dev, &lci, 0, &dsl));
	VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, pool, 1, &dsl};
	VkDescriptorSet ds;
	CHECK("vkAllocateDescriptorSets", vkAllocateDescriptorSets(dev, &dai, &ds));
	VkDescriptorBufferInfo dbi = {buf, 0, VK_WHOLE_SIZE};
	VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 0, 0, 1,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &dbi, 0};
	vkUpdateDescriptorSets(dev, 1, &w, 0, 0);

	/* pipeline */
	VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0,
		sizeof(spv), (const uint32_t *)spv};
	VkShaderModule sm;
	CHECK("vkCreateShaderModule", vkCreateShaderModule(dev, &smci, 0, &sm));
	VkPipelineShaderStageCreateInfo ss = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		0, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", 0};
	VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 0, 0};
	VkPipelineLayout pl;
	CHECK("vkCreatePipelineLayout", vkCreatePipelineLayout(dev, &plci, 0, &pl));
	VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0, ss, pl, 0, 0};
	VkPipeline pipe;
	CHECK("vkCreateComputePipelines", vkCreateComputePipelines(dev, 0, 1, &cpi, 0, &pipe));

	/* 录制、提交、等待 */
	VkCommandPoolCreateInfo cpoli = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, qf};
	VkCommandPool cpool;
	CHECK("vkCreateCommandPool", vkCreateCommandPool(dev, &cpoli, 0, &cpool));
	VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cpool,
		VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer cb;
	CHECK("vkAllocateCommandBuffers", vkAllocateCommandBuffers(dev, &cbai, &cb));
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0,
		VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 0};
	CHECK("vkBeginCommandBuffer", vkBeginCommandBuffer(cb, &bi));
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, 0);
	vkCmdDispatch(cb, N / 64, 1, 1);	/* local_size_x = 64 */
	CHECK("vkEndCommandBuffer", vkEndCommandBuffer(cb));
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb, 0, 0};
	CHECK("vkQueueSubmit", vkQueueSubmit(queue, 1, &si, 0));
	CHECK("vkQueueWaitIdle", vkQueueWaitIdle(queue));

	float *out = map;
	int bad = 0;
	for (int i = 0; i < N; i++)
		if (fabsf(out[i] - (i * 2.0f + 42.0f)) > 0.5f)
			bad++;
	printf("%s: %d/%d correct, bad=%d, sample[0]=%.1f [255]=%.1f\n",
	       bad ? "FAIL" : "COMPUTE OK", N - bad, N, bad, out[0], out[N - 1]);
	return bad != 0;
}
