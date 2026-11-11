// 最小 Vulkan 链路验证：动态加载 vulkan-1.dll，枚举实例与物理设备。
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdio>
#include <windows.h>

static PFN_vkCreateInstance vkCreateInstance;
static PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;
static PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;

int main() {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { printf("FAIL: vulkan-1.dll 加载失败\n"); return 1; }
    auto load = [&](const char* n) { return (void*)GetProcAddress(vk, n); };
    vkCreateInstance = (PFN_vkCreateInstance)load("vkCreateInstance");
    vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)load("vkEnumerateInstanceVersion");
    vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)load("vkEnumeratePhysicalDevices");
    if (!vkCreateInstance || !vkEnumeratePhysicalDevices) { printf("FAIL: 核心入口缺失\n"); return 1; }

    uint32_t ver = 0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&ver);
    printf("loader instance version: %u.%u.%u\n", VK_VERSION_MAJOR(ver), VK_VERSION_MINOR(ver), VK_VERSION_PATCH(ver));

    VkApplicationInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo = &ai;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ci, nullptr, &inst);
    if (r != VK_SUCCESS || !inst) { printf("FAIL: vkCreateInstance = %d\n", r); return 1; }
    printf("instance OK\n");

    auto vkEnumeratePhysicalDevicesI = (PFN_vkEnumeratePhysicalDevices)GetProcAddress(vk, "vkEnumeratePhysicalDevices");
    // instance-level 函数用 vkGetInstanceProcAddr 更规范，这里直接枚举
    PFN_vkGetInstanceProcAddr gpa = (PFN_vkGetInstanceProcAddr)load("vkGetInstanceProcAddr");
    vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)gpa(inst, "vkEnumeratePhysicalDevices");
    uint32_t n = 0;
    r = vkEnumeratePhysicalDevices(inst, &n, nullptr);
    if (r != VK_SUCCESS) { printf("FAIL: enumerate count = %d\n", r); return 1; }
    printf("physical devices: %u\n", n);
    return (n > 0) ? 0 : 2;
}
