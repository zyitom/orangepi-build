#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "t", 0, 0, 0, VK_API_VERSION_1_0};
    /* 参数：无 = 1.0；13 = 1.3.0；其它数字 N = 1.3.N（如 277、280） */
    if (argc > 1 && !strcmp(argv[1], "13")) app.apiVersion = VK_API_VERSION_1_3;
    else if (argc > 1 && atoi(argv[1]) > 13) app.apiVersion = VK_MAKE_API_VERSION(0,1,3,atoi(argv[1]));
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, 0, 0};
    VkInstance inst;
    VkResult r = vkCreateInstance(&ci, 0, &inst);
    printf("apiVersion=0x%x -> VkResult=%d\n", app.apiVersion, r);
    if (r == VK_SUCCESS) {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(inst, &n, 0);
        printf("physical devices: %u\n", n);
        VkPhysicalDevice devs[8]; vkEnumeratePhysicalDevices(inst, &n, devs);
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devs[i], &p);
            printf("  [%u] %s (api 0x%x vendor %04x)\n", i, p.deviceName, p.apiVersion, p.vendorID);
        }
    }
    return r != VK_SUCCESS;
}
