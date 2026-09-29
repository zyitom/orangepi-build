/* vkcomp.c — Vulkan 计算完整性测试：SPIR-V compute shader 分派 + 结果校验。
 * shader: vals[i] = i*2 + 42  (256 线程)
 * 通过 = 所有值精确匹配 → GPU 真实执行了 shader。
 * 编译（板上）：gcc -O2 -I/tmp/vkinc -o vkcomp vkcomp.c -lvulkan
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const unsigned char spv[] = {
#include "comp_spv_inline.h"
};

#define N 256

int main(void)
{
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vkcomp", 0, 0, 0, VK_API_VERSION_1_0};
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, 0, 0};
	VkInstance inst;
	VkResult r = vkCreateInstance(&ici, 0, &inst);
	if (r) { printf("instance: %d\n", r); return 1; }

	uint32_t nd = 0;
	vkEnumeratePhysicalDevices(inst, &nd, 0);
	if (!nd) { printf("no physical devices\n"); return 2; }
	VkPhysicalDevice pd[4]; nd = 4; vkEnumeratePhysicalDevices(inst, &nd, pd);
	VkPhysicalDevice pd0 = pd[0];
	VkPhysicalDeviceProperties pp;
	vkGetPhysicalDeviceProperties(pd0, &pp);
	printf("GPU: %s (api 0x%x)\n", pp.deviceName, pp.apiVersion);

	float qf = 0;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, qf, 1, 0};
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qci, 0, 0, 0, 0};
	VkDevice dev;
	if (vkCreateDevice(pd0, &dci, 0, &dev)) { printf("device create failed\n"); return 3; }

	VkQueue queue; vkGetDeviceQueue(dev, qf, 0, &queue);

	/* buffer: N floats, HOST_VISIBLE|HOST_COHERENT */
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, N*sizeof(float),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
	VkBuffer buf; vkCreateBuffer(dev, &bci, 0, &buf);
	VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd0, &mp);
	uint32_t mi = 0;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((mr.memoryTypeBits & (1u<<i)) &&
		    (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
		    (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mi = i; break; }
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mi};
	VkDeviceMemory mem; if (vkAllocateMemory(dev, &mai, 0, &mem)) { printf("alloc failed\n"); return 4; }
	vkBindBufferMemory(dev, buf, mem, 0);
	void *map; vkMapMemory(dev, mem, 0, N*sizeof(float), 0, &map);
	memset(map, 0, N*sizeof(float));

	/* descriptor */
	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
	VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 1, &ps};
	VkDescriptorPool pool; vkCreateDescriptorPool(dev, &pci, 0, &pool);
	VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE, 0};
	VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &lb};
	VkDescriptorSetLayout dsl; vkCreateDescriptorSetLayout(dev, &lci, 0, &dsl);
	VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, pool, 1, &dsl};
	VkDescriptorSet ds; vkAllocateDescriptorSets(dev, &dai, &ds);
	VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 0, 0, 1,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &(VkDescriptorBufferInfo){buf, 0, VK_WHOLE_SIZE}};
	vkUpdateDescriptorSets(dev, 1, &w, 0, 0);

	/* shader module from embedded SPIR-V */
	VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(spv), spv};
	VkShaderModule sm; vkCreateShaderModule(dev, &smci, 0, &sm);
	VkPipelineShaderStageCreateInfo ss = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		0, 0, VK_SHADER_STAGE_COMPUTE, sm, "main", 0};
	VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 0, 0};
	VkPipelineLayout pl; vkCreatePipelineLayout(dev, &plci, 0, &pl);
	VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0, ss, pl, 0, 0};
	VkPipeline pipe; if (vkCreateComputePipelines(dev, 0, 1, &cpi, 0, &pipe)) { printf("pipeline failed\n"); return 5; }

	VkCommandPoolCreateInfo cpoli = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, qf};
	VkCommandPool cpool; vkCreateCommandPool(dev, &cpoli, 0, &cpool);
	VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer cb; vkAllocateCommandBuffers(dev, &cbai, &cb);
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 0};
	vkBeginCommandBuffer(cb, &bi);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, 0);
	vkCmdDispatch(cb, N/64, 1, 1);
	vkEndCommandBuffer(cb);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb, 0, 0};
	vkQueueSubmit(queue, 1, &si, 0);
	vkQueueWaitIdle(queue);

	float *out = map;
	int bad = 0;
	for (int i = 0; i < N; i++)
		if (fabsf(out[i] - (i*2.0f + 42.0f)) > 0.5f) bad++;
	printf("%s: %d/%d correct, sample[0]=%.1f [255]=%.1f\n",
	       bad ? "FAIL" : "COMPUTE OK", N-bad, N, out[0], out[N-1]);
	return bad != 0;
}
