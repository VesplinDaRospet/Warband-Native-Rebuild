// vk_mesh.cpp —— 自研最小闭环：解析 BRF 网格并用 Vulkan 渲染
// 依赖：brf.h（同目录）、mesh.vert.spv / mesh.frag.spv（tools/shader-tool 生成）
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include "brf.h"
#include "dds.h"
#include "sco.h"
#include "skin.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include <string>
#include <fstream>

// 崩溃日志：写 vk_scene_crash.txt（异常码+地址+栈返回地址），便于下次定位“进去就崩”
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    FILE* f = fopen("vk_scene_crash.txt", "a");
    if (f) {
        std::fprintf(f, "exception code=0x%08lX addr=%p\n",
                     (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
        void* stack[32];
        USHORT n = CaptureStackBackTrace(0, 32, stack, nullptr);
        for (USHORT i = 0; i < n; i++) std::fprintf(f, "  [%d] %p\n", (int)i, stack[i]);
        std::fflush(f); fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---------- Vulkan 函数动态加载 ----------
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
static PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
static PFN_vkCreateInstance vkCreateInstance;
static PFN_vkCreateDevice vkCreateDevice;
static PFN_vkDestroyInstance vkDestroyInstance;
#define VK_INST_FN(inst, name) auto name = (PFN_##name)vkGetInstanceProcAddr(inst, #name)
#define VK_DEV_FN(dev, name)  auto name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)

struct App {
    HINSTANCE hInst = nullptr; HWND hwnd = nullptr;
    VkInstance inst = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qf = 0;
    VkSwapchainKHR swap = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent{1280, 720};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    VkRenderPass rp = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> fbs;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds;
    std::vector<VkFence> fences;      // 每帧提交完成
    std::vector<VkFence> acqFences;   // acquire 完成（与提交分开，避免 acquire 失败卡死等待）
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipeline pipeSky = VK_NULL_HANDLE;
    VkPipelineLayout groundPipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeGround = VK_NULL_HANDLE;
    struct Obj { VkBuffer vb=VK_NULL_HANDLE, ib=VK_NULL_HANDLE; VkDeviceMemory vm=VK_NULL_HANDLE, im=VK_NULL_HANDLE; uint32_t idxCount=0; std::vector<float> pc; std::vector<float> model; int texSlot=0; VkDescriptorSet descSet=VK_NULL_HANDLE; std::string diffName="shields"; bool sky=false; bool charAnim=false; bool ground=false; int charPart=-1; int charGroup=-1; float tintR=1.f,tintG=1.f,tintB=1.f; };
    std::vector<Obj> objs;
    std::vector<VkImageView> texViews;
    std::vector<std::string> texNames;
    std::map<std::string, int> texIndex;
    std::vector<VkDescriptorSet> texSets;
    VkImage depthImg = VK_NULL_HANDLE; VkImageView depthView = VK_NULL_HANDLE; VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkImage texImg = VK_NULL_HANDLE; VkDeviceMemory texMem = VK_NULL_HANDLE; VkImageView texView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout descLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout groundDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool groundDescPool = VK_NULL_HANDLE;
    VkDescriptorSet groundDescSet = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    std::vector<float> mvp; // column-major mat4
};

static void CHECK(VkResult r, const char* what) {
    if (r != VK_SUCCESS) { std::printf("FAIL %s = %d\n", what, r); std::exit(1); }
}

static std::vector<char> readFile(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::printf("FAIL 打不开 %s\n", path); std::exit(1); }
    std::vector<char> b((size_t)f.tellg());
    f.seekg(0); f.read(b.data(), b.size());
    return b;
}

static void matMul44(const float* A, const float* B, std::vector<float>& out) {
    out.assign(16, 0.f);
    for (int c=0;c<4;c++) for (int r=0;r<4;r++) {
        float s=0; for (int k=0;k<4;k++) s += A[k*4+r]*B[c*4+k];
        out[c*4+r]=s;
    }
}
static void lookAt(std::vector<float>& V, float ex,float ey,float ez, float tx,float ty,float tz) {
    float fx=tx-ex, fy=ty-ey, fz=tz-ez;
    float fl=sqrtf(fx*fx+fy*fy+fz*fz); fx/=fl; fy/=fl; fz/=fl;
    float ux=0, uy=0, uz=1; // 渲染坐标高度轴是 z
    float sx=uy*fz-uz*fy, sy=uz*fx-ux*fz, sz=ux*fy-uy*fx;
    float sl=sqrtf(sx*sx+sy*sy+sz*sz); sx/=sl; sy/=sl; sz/=sl;
    float rx=sy*fz-sz*fy, ry=sz*fx-sx*fz, rz=sx*fy-sy*fx;
    float M[16] = {
        sx, rx, -fx, 0,
        sy, ry, -fy, 0,
        sz, rz, -fz, 0,
        -(sx*ex+sy*ey+sz*ez), -(rx*ex+ry*ey+rz*ez), (fx*ex+fy*ey+fz*ez), 1
    };
    V.assign(M, M+16);
}
static void perspective(std::vector<float>& P, float fov, float aspect, float zn, float zf) {
    float y=1.0f/tanf(fov/2), x=y/aspect, q=zf/(zn-zf);
    float M[16] = { x,0,0,0, 0,y,0,0, 0,0,q,-1, 0,0,q*zn,0 };
    P.assign(M, M+16);
}
static void camFrame(float cx, float cz, float eyeH, float yaw, float pitch, float eye[3], float target[3]) {
    eye[0]=cx; eye[1]=cz; eye[2]=eyeH;
    float fhx = -sinf(yaw), fhz = -cosf(yaw); // yaw=0 朝世界 -z（场景中心方向）
    target[0] = cx + fhx*103.f;
    target[1] = cz + fhz*103.f;
    target[2] = eyeH + tanf(pitch)*103.f;
}
static void computeSceneMVP(std::vector<float>& pc, const float eye[3], const float target[3], const float* model16=nullptr) {
    std::vector<float> V, P;
    // 渲染坐标：x=世界x, y=世界z(南北), z=世界y(高度)；up=高度轴
    lookAt(V, eye[0], eye[1], eye[2], target[0], target[1], target[2]);
    perspective(P, 1.0f, 1280.0f/720.0f, 0.5f, 800.0f);
    pc.resize(40, 0.f);
    if (model16) {
        std::vector<float> VM;
        matMul44(V.data(), model16, VM);
        matMul44(P.data(), VM.data(), pc);
    } else {
        matMul44(P.data(), V.data(), pc); // pc[0..15] = mvp
    }
    pc[16] = eye[0]; pc[17] = eye[1]; pc[18] = eye[2];
    pc[20] = 70.f; pc[21] = 300.f;    // 雾起点/终点（世界距离）
    // pc[24..39] = model 矩阵（雾距离用世界坐标；场景物件=单位矩阵）
    if (model16) std::copy(model16, model16 + 16, pc.begin() + 24);
    else { pc[24] = pc[29] = pc[34] = pc[39] = 1.f; }
}

static POINT g_click{-1,-1};   // 战斗模式：左键 = 攻击
static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_CLOSE || (msg == WM_KEYDOWN && w == VK_ESCAPE)) PostQuitMessage(0);
    if (msg == WM_LBUTTONDOWN) { g_click.x = (short)(l & 0xFFFF); g_click.y = (short)((l >> 16) & 0xFFFF); }
    return DefWindowProcA(h, msg, w, l);
}

static uint32_t findMemType(VkInstance inst, VkPhysicalDevice gpu, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    auto fn = (PFN_vkGetPhysicalDeviceMemoryProperties)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceMemoryProperties");
    fn(gpu, &mp);
    for (uint32_t i=0;i<mp.memoryTypeCount;i++)
        if ((typeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    std::printf("FAIL 无合适内存类型\n"); std::exit(1);
    return 0;
}

static void makeBuffer(App& A, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem) {
    VK_DEV_FN(A.dev, vkCreateBuffer);
    VK_DEV_FN(A.dev, vkAllocateMemory);
    VK_DEV_FN(A.dev, vkBindBufferMemory);
    VK_DEV_FN(A.dev, vkGetBufferMemoryRequirements);
    VkBufferCreateInfo bi{}; bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size=size; bi.usage=usage;
    CHECK(vkCreateBuffer(A.dev, &bi, nullptr, &buf), "createBuffer");
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(A.dev, buf, &mr);
    VkMemoryAllocateInfo ai{}; ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize=mr.size;
    ai.memoryTypeIndex=findMemType(A.inst, A.gpu, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(A.dev, &ai, nullptr, &mem), "allocBufferMem");
    vkBindBufferMemory(A.dev, buf, mem, 0);
}

static void upload(App& A, VkBuffer buf, VkDeviceMemory mem, const void* data, VkDeviceSize size) {
    VK_DEV_FN(A.dev, vkMapMemory); VK_DEV_FN(A.dev, vkUnmapMemory);
    void* p; CHECK(vkMapMemory(A.dev, mem, 0, size, 0, &p), "map");
    memcpy(p, data, (size_t)size); vkUnmapMemory(A.dev, mem);
}

// 一次性提交命令（纹理上传/布局转换用）
static void submitOnce(App& A, const std::function<void(VkCommandBuffer)>& fn) {
    VK_DEV_FN(A.dev, vkAllocateCommandBuffers); VK_DEV_FN(A.dev, vkBeginCommandBuffer);
    VK_DEV_FN(A.dev, vkEndCommandBuffer); VK_DEV_FN(A.dev, vkQueueSubmit); VK_DEV_FN(A.dev, vkQueueWaitIdle);
    VK_DEV_FN(A.dev, vkFreeCommandBuffers); VK_DEV_FN(A.dev, vkCreateFence); VK_DEV_FN(A.dev, vkWaitForFences); VK_DEV_FN(A.dev, vkDestroyFence);
    VkCommandBufferAllocateInfo ca{}; ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool=A.cmdPool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(A.dev, &ca, &cb), "allocOnce");
    VkCommandBufferBeginInfo bb{}; bb.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bb.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bb);
    fn(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{}; si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount=1; si.pCommandBuffers=&cb;
    CHECK(vkQueueSubmit(A.queue, 1, &si, VK_NULL_HANDLE), "submitOnce");
    vkQueueWaitIdle(A.queue);
    vkFreeCommandBuffers(A.dev, A.cmdPool, 1, &cb);
}

static void initTextures(App& A, const std::string& ddsDir, const std::vector<std::string>& names,
                         const std::string& fallbackDdsDir = "") {
    VK_DEV_FN(A.dev, vkCreateImage); VK_DEV_FN(A.dev, vkAllocateMemory); VK_DEV_FN(A.dev, vkBindImageMemory);
    VK_DEV_FN(A.dev, vkGetImageMemoryRequirements); VK_DEV_FN(A.dev, vkCreateImageView);
    VK_DEV_FN(A.dev, vkCreateSampler); VK_DEV_FN(A.dev, vkCmdCopyBufferToImage); VK_DEV_FN(A.dev, vkCmdPipelineBarrier);
    VK_DEV_FN(A.dev, vkDestroyBuffer); VK_DEV_FN(A.dev, vkFreeMemory);
    // 共享 sampler
    VkSamplerCreateInfo sc{}; sc.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sc.magFilter=VK_FILTER_LINEAR; sc.minFilter=VK_FILTER_LINEAR;
    sc.addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT; sc.addressModeV=VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sc.maxLod=1.0f;
    CHECK(vkCreateSampler(A.dev, &sc, nullptr, &A.sampler), "sampler");
    for (auto& n : names) {
        std::string path = ddsDir + "\\" + n + ".dds";
        if (!std::filesystem::exists(path) && !fallbackDdsDir.empty())
            path = fallbackDdsDir + "\\" + n + ".dds";
        dds::Image img;
        try { img = dds::load(path); }
        catch (const std::exception& e) { std::fprintf(stderr, "贴图缺失 %s（%s），跳过\n", path.c_str(), e.what()); continue; }
        VkBuffer stage; VkDeviceMemory smem;
        makeBuffer(A, img.rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stage, smem);
        upload(A, stage, smem, img.rgba.data(), img.rgba.size());
        VkImage image;
        VkImageCreateInfo ii{}; ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType=VK_IMAGE_TYPE_2D;
        ii.format=VK_FORMAT_R8G8B8A8_UNORM; ii.extent={img.width,img.height,1}; ii.mipLevels=1; ii.arrayLayers=1;
        ii.samples=VK_SAMPLE_COUNT_1_BIT; ii.tiling=VK_IMAGE_TILING_OPTIMAL;
        ii.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT; ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        CHECK(vkCreateImage(A.dev, &ii, nullptr, &image), "texImage");
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(A.dev, image, &mr);
        VkMemoryAllocateInfo ma{}; ma.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ma.allocationSize=mr.size;
        ma.memoryTypeIndex=findMemType(A.inst, A.gpu, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory mem;
        CHECK(vkAllocateMemory(A.dev, &ma, nullptr, &mem), "texMem");
        vkBindImageMemory(A.dev, image, mem, 0);
        A.texImg = image;
        submitOnce(A, [&](VkCommandBuffer cb) {
            VkBufferImageCopy bc{}; bc.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            bc.imageSubresource.layerCount=1; bc.imageExtent={img.width,img.height,1};
            vkCmdCopyBufferToImage(cb, stage, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
            VkImageMemoryBarrier bar{}; bar.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            bar.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bar.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; bar.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            bar.image=image; bar.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            bar.subresourceRange.levelCount=1; bar.subresourceRange.layerCount=1;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &bar);
        });
        vkDestroyBuffer(A.dev, stage, nullptr); vkFreeMemory(A.dev, smem, nullptr);
        VkImageView view;
        VkImageViewCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image=image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount=1; vi.subresourceRange.layerCount=1;
        CHECK(vkCreateImageView(A.dev, &vi, nullptr, &view), "texView");
        A.texNames.push_back(n);
        A.texIndex[n] = (int)A.texViews.size();
        A.texViews.push_back(view);
        std::printf("贴图[%d] %s %ux%u\n", (int)A.texViews.size()-1, n.c_str(), img.width, img.height);
    }
    if (A.texViews.empty()) { std::printf("FAIL 一张贴图都没加载成功\n"); std::exit(1); }
}

static void initVk(App& A) {
    HMODULE dll = LoadLibraryA("vulkan-1.dll");
    if (!dll) { std::printf("FAIL vulkan-1.dll\n"); std::exit(1); }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(dll, "vkGetInstanceProcAddr");
    vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)GetProcAddress(dll, "vkGetDeviceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)GetProcAddress(dll, "vkCreateInstance");
    vkCreateDevice = (PFN_vkCreateDevice)GetProcAddress(dll, "vkCreateDevice");
    vkDestroyInstance = (PFN_vkDestroyInstance)GetProcAddress(dll, "vkDestroyInstance");

    VkApplicationInfo ai{}; ai.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.apiVersion=VK_API_VERSION_1_0;
    const char* exts[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo=&ai; ci.enabledExtensionCount=2; ci.ppEnabledExtensionNames=exts;
    CHECK(vkCreateInstance(&ci, nullptr, &A.inst), "createInstance");

    VK_INST_FN(A.inst, vkCreateWin32SurfaceKHR);
    VkWin32SurfaceCreateInfoKHR si{}; si.sType=VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    si.hinstance=A.hInst; si.hwnd=A.hwnd;
    CHECK(vkCreateWin32SurfaceKHR(A.inst, &si, nullptr, &A.surface), "win32surface");

    VK_INST_FN(A.inst, vkEnumeratePhysicalDevices);
    uint32_t n=0; vkEnumeratePhysicalDevices(A.inst, &n, nullptr);
    if (!n) { std::printf("FAIL 无GPU\n"); std::exit(1); }
    std::vector<VkPhysicalDevice> gpus(n); vkEnumeratePhysicalDevices(A.inst, &n, gpus.data());
    A.gpu = gpus[0];

    VK_INST_FN(A.inst, vkGetPhysicalDeviceQueueFamilyProperties);
    uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(A.gpu, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qps(qn); vkGetPhysicalDeviceQueueFamilyProperties(A.gpu, &qn, qps.data());
    A.qf = UINT32_MAX;
    for (uint32_t i=0;i<qn;i++) {
        if (!(qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VK_INST_FN(A.inst, vkGetPhysicalDeviceWin32PresentationSupportKHR);
        if (!vkGetPhysicalDeviceWin32PresentationSupportKHR(A.gpu, i)) continue;
        A.qf = i; break;
    }
    if (A.qf==UINT32_MAX) { std::printf("FAIL 无图形+呈现队列\n"); std::exit(1); }

    float prio=1.0f;
    VkDeviceQueueCreateInfo qi{}; qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex=A.qf; qi.queueCount=1; qi.pQueuePriorities=&prio;
    const char* devExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo di{}; di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
    di.enabledExtensionCount=1; di.ppEnabledExtensionNames=&devExt;
    CHECK(vkCreateDevice(A.gpu, &di, nullptr, &A.dev), "createDevice");
    VK_DEV_FN(A.dev, vkGetDeviceQueue); vkGetDeviceQueue(A.dev, A.qf, 0, &A.queue);

    // swapchain
    VK_DEV_FN(A.dev, vkGetSwapchainImagesKHR); VK_DEV_FN(A.dev, vkCreateSwapchainKHR); VK_DEV_FN(A.dev, vkCreateImageView);
    VkSurfaceCapabilitiesKHR cap;
    VK_INST_FN(A.inst, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(A.gpu, A.surface, &cap);
    if (cap.currentExtent.width < UINT32_MAX) A.extent = cap.currentExtent;
    std::fprintf(stderr, "swapchain extent = %ux%u\n", A.extent.width, A.extent.height);
    VkSwapchainCreateInfoKHR sc{}; sc.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sc.surface=A.surface; sc.minImageCount=2; sc.imageFormat=A.fmt;
    sc.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sc.imageExtent=A.extent; sc.imageArrayLayers=1; sc.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE; sc.preTransform=cap.currentTransform;
    sc.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; sc.presentMode=VK_PRESENT_MODE_FIFO_KHR; sc.clipped=VK_TRUE;
    CHECK(vkCreateSwapchainKHR(A.dev, &sc, nullptr, &A.swap), "swapchain");
    uint32_t in=0; vkGetSwapchainImagesKHR(A.dev, A.swap, &in, nullptr);
    A.images.resize(in); vkGetSwapchainImagesKHR(A.dev, A.swap, &in, A.images.data());
    for (auto img : A.images) {
        VkImageViewCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image=img; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=A.fmt;
        vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount=1; vi.subresourceRange.layerCount=1;
        VkImageView v; CHECK(vkCreateImageView(A.dev, &vi, nullptr, &v), "imageView");
        A.views.push_back(v);
    }
    // depth
    VK_DEV_FN(A.dev, vkCreateImage); VK_DEV_FN(A.dev, vkAllocateMemory); VK_DEV_FN(A.dev, vkBindImageMemory);
    VkImageCreateInfo ii{}; ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType=VK_IMAGE_TYPE_2D;
    ii.format=VK_FORMAT_D32_SFLOAT; ii.extent={A.extent.width,A.extent.height,1}; ii.mipLevels=1; ii.arrayLayers=1;
    ii.samples=VK_SAMPLE_COUNT_1_BIT; ii.tiling=VK_IMAGE_TILING_OPTIMAL;
    ii.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(A.dev, &ii, nullptr, &A.depthImg), "depthImage");
    VK_DEV_FN(A.dev, vkGetImageMemoryRequirements);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(A.dev, A.depthImg, &mr);
    VkMemoryAllocateInfo ma{}; ma.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ma.allocationSize=mr.size;
    ma.memoryTypeIndex=findMemType(A.inst, A.gpu, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkAllocateMemory(A.dev, &ma, nullptr, &A.depthMem), "depthMem");
    vkBindImageMemory(A.dev, A.depthImg, A.depthMem, 0);
    VkImageViewCreateInfo dv{}; dv.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    dv.image=A.depthImg; dv.viewType=VK_IMAGE_VIEW_TYPE_2D; dv.format=VK_FORMAT_D32_SFLOAT;
    dv.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT; dv.subresourceRange.levelCount=1; dv.subresourceRange.layerCount=1;
    CHECK(vkCreateImageView(A.dev, &dv, nullptr, &A.depthView), "depthView");

    // render pass
    VK_DEV_FN(A.dev, vkCreateRenderPass);
    VkAttachmentDescription at[2]{};
    at[0].format=A.fmt; at[0].samples=VK_SAMPLE_COUNT_1_BIT; at[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
    at[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE; at[0].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE; at[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    at[0].finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    at[1].format=VK_FORMAT_D32_SFLOAT; at[1].samples=VK_SAMPLE_COUNT_1_BIT; at[1].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
    at[1].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE; at[1].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[1].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE; at[1].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    at[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{}; sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount=1; sp.pColorAttachments=&colorRef; sp.pDepthStencilAttachment=&depthRef;
    VkRenderPassCreateInfo rp{}; rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount=2; rp.pAttachments=at; rp.subpassCount=1; rp.pSubpasses=&sp;
    CHECK(vkCreateRenderPass(A.dev, &rp, nullptr, &A.rp), "renderPass");

    VK_DEV_FN(A.dev, vkCreateFramebuffer);
    for (auto view : A.views) {
        VkImageView atv[2]={view, A.depthView};
        VkFramebufferCreateInfo fb{}; fb.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass=A.rp; fb.attachmentCount=2; fb.pAttachments=atv; fb.width=A.extent.width; fb.height=A.extent.height; fb.layers=1;
        VkFramebuffer f; CHECK(vkCreateFramebuffer(A.dev, &fb, nullptr, &f), "framebuffer");
        A.fbs.push_back(f);
    }
    // command pool
    VK_DEV_FN(A.dev, vkCreateCommandPool);
    VkCommandPoolCreateInfo cp{}; cp.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.queueFamilyIndex=A.qf; cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    CHECK(vkCreateCommandPool(A.dev, &cp, nullptr, &A.cmdPool), "cmdPool");
}

static std::string g_groundDiffs[3] = {"ground_turf", "ground_village", "ground_path"};
static int g_groundLayers[3] = {2, 9, 10};

static void initPipeline(App& A) {
    VK_DEV_FN(A.dev, vkCreateShaderModule); VK_DEV_FN(A.dev, vkCreatePipelineLayout); VK_DEV_FN(A.dev, vkCreateGraphicsPipelines);
    auto makeShader = [&](const char* path) {
        auto code = readFile(path);
        VkShaderModuleCreateInfo sm{}; sm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sm.codeSize=code.size(); sm.pCode=(const uint32_t*)code.data();
        VkShaderModule m; CHECK(vkCreateShaderModule(A.dev, &sm, nullptr, &m), "shaderModule");
        return m;
    };
    VkShaderModule vs = makeShader("lit.vert.spv"), fs = makeShader("lit.frag.spv");
    VkShaderModule vsSky = makeShader("mesh.vert.spv"), fsSky = makeShader("mesh.frag.spv");
    VkShaderModule vsG = makeShader("ground.vert.spv"), fsG = makeShader("ground.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vs; stages[0].pName="main";
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fs; stages[1].pName="main";
    VkPipelineShaderStageCreateInfo skyStages[2]{};
    skyStages[0].sType=skyStages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    skyStages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; skyStages[0].module=vsSky; skyStages[0].pName="main";
    skyStages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; skyStages[1].module=fsSky; skyStages[1].pName="main";
    VkPipelineShaderStageCreateInfo groundStages[2]{};
    groundStages[0].sType=groundStages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    groundStages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; groundStages[0].module=vsG; groundStages[0].pName="main";
    groundStages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; groundStages[1].module=fsG; groundStages[1].pName="main";

    VkVertexInputBindingDescription bind{}; bind.binding=0; bind.stride=44; bind.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0].location=0; attrs[0].binding=0; attrs[0].format=VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset=0;
    attrs[1].location=1; attrs[1].binding=0; attrs[1].format=VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset=12;
    attrs[2].location=2; attrs[2].binding=0; attrs[2].format=VK_FORMAT_R32G32B32_SFLOAT; attrs[2].offset=24;
    attrs[3].location=3; attrs[3].binding=0; attrs[3].format=VK_FORMAT_R32G32_SFLOAT; attrs[3].offset=36;
    VkPipelineVertexInputStateCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount=1; vi.pVertexBindingDescriptions=&bind;
    vi.vertexAttributeDescriptionCount=4; vi.pVertexAttributeDescriptions=attrs;
    // 天空 shader（mesh.vert/frag）是 3 属性版（pos+color+uv），顶点缓冲同为 44B stride，uv 在 offset 36
    VkVertexInputAttributeDescription skyAttrs[3]{};
    skyAttrs[0].location=0; skyAttrs[0].binding=0; skyAttrs[0].format=VK_FORMAT_R32G32B32_SFLOAT; skyAttrs[0].offset=0;
    skyAttrs[1].location=1; skyAttrs[1].binding=0; skyAttrs[1].format=VK_FORMAT_R32G32B32_SFLOAT; skyAttrs[1].offset=12;
    skyAttrs[2].location=2; skyAttrs[2].binding=0; skyAttrs[2].format=VK_FORMAT_R32G32_SFLOAT; skyAttrs[2].offset=36;
    VkPipelineVertexInputStateCreateInfo viSky{}; viSky.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    viSky.vertexBindingDescriptionCount=1; viSky.pVertexBindingDescriptions=&bind;
    viSky.vertexAttributeDescriptionCount=3; viSky.pVertexAttributeDescriptions=skyAttrs;
    // 地面混合 shader：stride=72（18 floats：pos+权重+leveling色+法线+3 组 uv）
    VkVertexInputBindingDescription bindG{}; bindG.binding=0; bindG.stride=72; bindG.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription gattrs[7]{};
    gattrs[0].location=0; gattrs[0].binding=0; gattrs[0].format=VK_FORMAT_R32G32B32_SFLOAT; gattrs[0].offset=0;
    gattrs[1].location=1; gattrs[1].binding=0; gattrs[1].format=VK_FORMAT_R32G32B32_SFLOAT; gattrs[1].offset=12;
    gattrs[2].location=2; gattrs[2].binding=0; gattrs[2].format=VK_FORMAT_R32G32B32_SFLOAT; gattrs[2].offset=24;
    gattrs[3].location=3; gattrs[3].binding=0; gattrs[3].format=VK_FORMAT_R32G32B32_SFLOAT; gattrs[3].offset=36;
    gattrs[4].location=4; gattrs[4].binding=0; gattrs[4].format=VK_FORMAT_R32G32_SFLOAT; gattrs[4].offset=48;
    gattrs[5].location=5; gattrs[5].binding=0; gattrs[5].format=VK_FORMAT_R32G32_SFLOAT; gattrs[5].offset=56;
    gattrs[6].location=6; gattrs[6].binding=0; gattrs[6].format=VK_FORMAT_R32G32_SFLOAT; gattrs[6].offset=64;
    VkPipelineVertexInputStateCreateInfo viGround{}; viGround.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    viGround.vertexBindingDescriptionCount=1; viGround.pVertexBindingDescriptions=&bindG;
    viGround.vertexAttributeDescriptionCount=7; viGround.pVertexAttributeDescriptions=gattrs;

    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0,0,(float)A.extent.width,(float)A.extent.height,0,1};
    VkRect2D sc{{0,0},A.extent};
    VkPipelineViewportStateCreateInfo vsi{}; vsi.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vsi.viewportCount=1; vsi.pViewports=&vp; vsi.scissorCount=1; vsi.pScissors=&sc;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{}; ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable=VK_TRUE; ds.depthWriteEnable=VK_TRUE; ds.depthCompareOp=VK_COMPARE_OP_LESS;
    VkPipelineDepthStencilStateCreateInfo dsSky{}; dsSky.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dsSky.depthTestEnable=VK_FALSE; dsSky.depthWriteEnable=VK_FALSE;
    VkPipelineColorBlendAttachmentState cb{}; cb.colorWriteMask=0xF; cb.blendEnable=VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount=1; cbs.pAttachments=&cb;
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 160};
    // descriptor set：binding 0 = combined image sampler
    VK_DEV_FN(A.dev, vkCreateDescriptorSetLayout); VK_DEV_FN(A.dev, vkCreateDescriptorPool);
    VK_DEV_FN(A.dev, vkAllocateDescriptorSets); VK_DEV_FN(A.dev, vkUpdateDescriptorSets);
    VkDescriptorSetLayoutBinding dsb{}; dsb.binding=0;
    dsb.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; dsb.descriptorCount=1;
    dsb.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl{}; dl.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount=1; dl.pBindings=&dsb;
    CHECK(vkCreateDescriptorSetLayout(A.dev, &dl, nullptr, &A.descLayout), "descLayout");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)A.texViews.size()};
    VkDescriptorPoolCreateInfo dp{}; dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets=(uint32_t)A.texViews.size(); dp.poolSizeCount=1; dp.pPoolSizes=&ps;
    CHECK(vkCreateDescriptorPool(A.dev, &dp, nullptr, &A.descPool), "descPool");
    A.texSets.resize(A.texViews.size());
    for (size_t i=0;i<A.texViews.size();i++) {
        VkDescriptorSetAllocateInfo da{}; da.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        da.descriptorPool=A.descPool; da.descriptorSetCount=1; da.pSetLayouts=&A.descLayout;
        CHECK(vkAllocateDescriptorSets(A.dev, &da, &A.texSets[i]), "allocDescSet");
        VkDescriptorImageInfo di{}; di.sampler=A.sampler; di.imageView=A.texViews[i]; di.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wd{}; wd.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wd.dstSet=A.texSets[i]; wd.dstBinding=0; wd.descriptorCount=1;
        wd.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd.pImageInfo=&di;
        vkUpdateDescriptorSets(A.dev, 1, &wd, 0, nullptr);
    }
    A.descSet = A.texSets.empty() ? VK_NULL_HANDLE : A.texSets[0];

    // 地面三贴图混合 descriptor set：binding 0/1/2 = 三个 sampler2D
    {
        VkDescriptorSetLayoutBinding gb[3]{};
        for (int i=0;i<3;i++) {
            gb[i].binding=(uint32_t)i; gb[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            gb[i].descriptorCount=1; gb[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo gdl{}; gdl.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        gdl.bindingCount=3; gdl.pBindings=gb;
        CHECK(vkCreateDescriptorSetLayout(A.dev, &gdl, nullptr, &A.groundDescLayout), "groundDescLayout");
        VkDescriptorPoolSize gps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
        VkDescriptorPoolCreateInfo gdp{}; gdp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        gdp.maxSets=1; gdp.poolSizeCount=1; gdp.pPoolSizes=&gps;
        CHECK(vkCreateDescriptorPool(A.dev, &gdp, nullptr, &A.groundDescPool), "groundDescPool");
        VkDescriptorSetAllocateInfo gda{}; gda.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        gda.descriptorPool=A.groundDescPool; gda.descriptorSetCount=1; gda.pSetLayouts=&A.groundDescLayout;
        CHECK(vkAllocateDescriptorSets(A.dev, &gda, &A.groundDescSet), "allocGroundDescSet");
        VkDescriptorImageInfo gdi[3]{};
        VkWriteDescriptorSet gwd[3]{};
        for (int i=0;i<3;i++) {
            int slot = 0;
            auto it = A.texIndex.find(g_groundDiffs[i]);
            if (it != A.texIndex.end()) slot = it->second;
            gdi[i].sampler=A.sampler; gdi[i].imageView=A.texViews[(size_t)slot];
            gdi[i].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            gwd[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            gwd[i].dstSet=A.groundDescSet; gwd[i].dstBinding=(uint32_t)i; gwd[i].descriptorCount=1;
            gwd[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; gwd[i].pImageInfo=&gdi[i];
        }
        vkUpdateDescriptorSets(A.dev, 3, gwd, 0, nullptr);
    }

    VkPipelineLayoutCreateInfo pl{}; pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount=1; pl.pSetLayouts=&A.descLayout;
    pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&pc;
    CHECK(vkCreatePipelineLayout(A.dev, &pl, nullptr, &A.pipeLayout), "pipeLayout");
    VkGraphicsPipelineCreateInfo gp{}; gp.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount=2; gp.pStages=stages; gp.pVertexInputState=&vi; gp.pInputAssemblyState=&ia;
    gp.pViewportState=&vsi; gp.pRasterizationState=&rs; gp.pMultisampleState=&ms; gp.pDepthStencilState=&ds;
    gp.pColorBlendState=&cbs; gp.layout=A.pipeLayout; gp.renderPass=A.rp; gp.subpass=0;
    CHECK(vkCreateGraphicsPipelines(A.dev, VK_NULL_HANDLE, 1, &gp, nullptr, &A.pipe), "pipeline");
    VkGraphicsPipelineCreateInfo gpSky{}; gpSky.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpSky.stageCount=2; gpSky.pStages=skyStages; gpSky.pVertexInputState=&viSky; gpSky.pInputAssemblyState=&ia;
    gpSky.pViewportState=&vsi; gpSky.pRasterizationState=&rs; gpSky.pMultisampleState=&ms; gpSky.pDepthStencilState=&dsSky;
    gpSky.pColorBlendState=&cbs; gpSky.layout=A.pipeLayout; gpSky.renderPass=A.rp; gpSky.subpass=0;
    CHECK(vkCreateGraphicsPipelines(A.dev, VK_NULL_HANDLE, 1, &gpSky, nullptr, &A.pipeSky), "skyPipeline");
    // 地面混合 pipeline：顶点布局 stride=72，descriptor set 绑 3 张贴图
    VkPipelineLayoutCreateInfo gpl{}; gpl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    gpl.setLayoutCount=1; gpl.pSetLayouts=&A.groundDescLayout;
    gpl.pushConstantRangeCount=1; gpl.pPushConstantRanges=&pc;
    CHECK(vkCreatePipelineLayout(A.dev, &gpl, nullptr, &A.groundPipeLayout), "groundPipeLayout");
    VkGraphicsPipelineCreateInfo gpGround{}; gpGround.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpGround.stageCount=2; gpGround.pStages=groundStages; gpGround.pVertexInputState=&viGround; gpGround.pInputAssemblyState=&ia;
    gpGround.pViewportState=&vsi; gpGround.pRasterizationState=&rs; gpGround.pMultisampleState=&ms; gpGround.pDepthStencilState=&ds;
    gpGround.pColorBlendState=&cbs; gpGround.layout=A.groundPipeLayout; gpGround.renderPass=A.rp; gpGround.subpass=0;
    CHECK(vkCreateGraphicsPipelines(A.dev, VK_NULL_HANDLE, 1, &gpGround, nullptr, &A.pipeGround), "groundPipeline");
    VK_DEV_FN(A.dev, vkDestroyShaderModule);
    vkDestroyShaderModule(A.dev, vs, nullptr); vkDestroyShaderModule(A.dev, fs, nullptr);
    vkDestroyShaderModule(A.dev, vsSky, nullptr); vkDestroyShaderModule(A.dev, fsSky, nullptr);
    vkDestroyShaderModule(A.dev, vsG, nullptr); vkDestroyShaderModule(A.dev, fsG, nullptr);
}

// 网格库：基础名 -> 该基础名下的所有网格片段（与 OpenBRF AnalyzeName 规则一致）
static std::map<std::string, std::vector<brf::Mesh>> g_meshGroups;
static std::map<std::string, std::string> g_materialDiffuse;
static sco::Terrain g_terrain;
struct Char { float x=0, z=0, yaw=0.f; float hp=8.f, atkCd=0.f; bool alive=true; };
static Char g_char;
static std::vector<Char> g_chars;   // 战斗模式：g_chars[0]=玩家，后面是敌方 troops
static float g_atkCd = 0.f;         // 玩家攻击冷却
static float g_wpnLen = 60.f;       // 当前武器 weapon_length（cm，来自 item_kinds1.txt）
static float g_wpnSpeed = 97.f;     // 当前武器 speed_rating
static int   g_wpnSwing = 23;       // 当前武器 swing_damage（v0.7-6：攻击伤害原值直用）
// v0.7-6：敌兵种数据（loadEnemyGear 从 troops/item_kinds 直读；判定数值一律来自模块数据）
static float g_enemySwing = 23.f;   // 敌武器 swing_damage（选兵种 swing 最小的近战武器，避开 mod 神器件）
static float g_enemyHp = 89.f;      // 敌 HP = 35 + 2*(str + ironflesh)（原版公式；CV War 数值膨胀故偏高）
static int   g_playerHp = 100;      // 玩家 HP 占位：无模块兵种数据可读；手感不对时女王拍板调这里
static int   g_lastMoveDir = 0;     // 攻击方向 = 最后移动方向：0=righttoleft 1=lefttoright 2=overswing 3=thrust
// v0.7 第 4 步：判定数值从 CV War item_kinds 数据来，不用占位
static void loadWeaponStats(const std::string& propsPath) {
    try {
        std::filesystem::path moduleDir = std::filesystem::path(propsPath).parent_path();
        std::ifstream f(moduleDir / "item_kinds1.txt");
        if (!f) { std::fprintf(stderr, "[判定] 找不到 item_kinds1.txt，用占位数值\n"); return; }
        std::string line;
        while (std::getline(f, line)) {
            size_t p0 = line.find_first_not_of(" \t\r\n");
            if (p0 == std::string::npos) continue;
            line = line.substr(p0); // item_kinds 行首有前导空格
            if (line.rfind("itm_hatchet ", 0) != 0) continue;
            std::istringstream ss(line);
            std::vector<std::string> t; std::string tok;
            while (ss >> tok) t.push_back(tok);
            if (t.size() < 8) break;
            int mc = std::atoi(t[3].c_str());
            size_t base = 4 + (size_t)mc * 2;
            if (t.size() >= base + 17) {
                g_wpnSpeed = (float)std::atof(t[base + 11].c_str());
                g_wpnLen   = (float)std::atof(t[base + 13].c_str());
                g_wpnSwing = std::atoi(t[base + 16].c_str());
                std::fprintf(stderr, "[判定] hatchet: wlen=%g speed=%g swing=%d\n", g_wpnLen, g_wpnSpeed, g_wpnSwing);
            }
            break;
        }
    } catch (...) { std::fprintf(stderr, "[判定] item 解析失败，用占位数值\n"); }
}
static float g_attackT = -1.f;      // 攻击动画播放进度（帧计数），-1=未播放；素材=CV War 原版 ani_attacks
static float g_endTimer = -1.f;     // 战斗结束后停留倒计时，-1=未结束
static std::string g_endResult;     // "胜" / "败"
static std::string g_battleName = "野队";  // 遭遇的野队名，回地图时带回
static std::vector<float> g_hitFlash;      // 每组受击闪红计时
static brf::SkeletonAnim g_attackMirrors[4]; // 四向攻击动画播放副本（根位移清零；素材原版数据在 attackFile 里不动）
static brf::AnimFrame g_mixFrame;          // 攻击起始混合帧（idle→攻击姿态 0.18s 过渡，防瞬移）
static brf::AnimFrame g_attackBase;        // 攻击触发瞬间的姿态快照
static bool g_battleFast() { static bool v = getenv("VK_BATTLE_FAST") != nullptr; return v; } // 速测：1 血敌方+血厚玩家
static sco::AiMesh g_aiMesh;
// 原版角色蒙皮资源（skeletons.brf / ani_man_walk.brf / body_meshes.brf / meshes_face_gen.brf）
struct CharSkin {
    bool ok = false;
    brf::BrfFile skelFile, animFile, bodyFile, faceFile, standFile, runFile, armorFile, weaponFile, attackFile;
    brf::SkeletonAnim* attackAnim = nullptr;   // 攻击动画（ani_attacks.brf，CV War 原版；相机在背后=画面右=角色右，原样播即可）
    brf::Skeleton* skel = nullptr;
    brf::SkeletonAnim* anim = nullptr;      // man_walk（走路）
    brf::SkeletonAnim* standAnim = nullptr; // stand_man（静止）
    brf::SkeletonAnim* runAnim = nullptr;   // run_man_forward（跑步）
    brf::SkeletonAnim* curAnim = nullptr;
    brf::Mesh* body = nullptr;
    // 部件列表：0=body 蒙皮；其余为无蒙皮的附属部件，整体跟随 bone（骨骼全局 × 固定旋转）
    struct Part { const brf::Mesh* mesh = nullptr; int bone = -1; std::string diff = "snow"; bool mirror = false; bool isHead = false;
                  bool fixSet = false; skin::M44 fix = skin::identity(); };
    std::vector<Part> parts;
    std::vector<skin::M44> bind;
    float t = 0.f;
    size_t frame = 0;
    std::string diffuse = "snow";
};
static CharSkin g_charSkin;
// 肢体部件（小腿/手）约定：part x→骨+z，part y→骨-y（脚趾/掌心翻正），part z→骨+x（沿肢体长轴）
// v0.7-3：敌方按野队名（party template name = troops plural）穿自己兵种的衣甲；v0.7-6 加本兵种武器
static skin::M44 g_partFixLimb = []() {
    skin::M44 m = skin::identity();
    m.m[0] = 0.f; m.m[1] = 0.f; m.m[2] = 1.f;   // col0=(0,0,1)
    m.m[4] = 0.f; m.m[5] = -1.f; m.m[6] = 0.f;  // col1=(0,-1,0)
    m.m[8] = 1.f; m.m[9] = 0.f; m.m[10] = 0.f;  // col2=(1,0,0)
    return m;
}();
static int g_attackDir = 0; // 攻击触发时锁存的方向（日志/后续机制用）
static skin::M44 g_partFixWeapon = []() {
    skin::M44 m = skin::identity();
    m.m[0] = 1.f; m.m[1] = 0.f; m.m[2] = 0.f;   // part x(刃) → 骨+x（手指方向/前）
    m.m[4] = 0.f; m.m[5] = 1.f; m.m[6] = 0.f;   // part y → 骨+y（正手：绕手指轴翻 180°，斧头转拇指侧）
    m.m[8] = 0.f; m.m[9] = 0.f; m.m[10] = -1.f; // part z(斧柄) → 骨-z（横贯手掌）
    m.m[12] = 0.07f;                            // 沿骨+x 平移到掌心（纯旋转会挂在手腕上）
    return m;
}();
static std::vector<CharSkin::Part> g_enemyParts;
static brf::BrfFile g_enemyArmorFile;
static brf::BrfFile g_enemyWeaponFile;
static brf::Mesh* g_enemyWeaponMesh = nullptr;
static void loadEnemyGear(const std::string& propsPath, const std::string& commonRes, const std::string& partyName) {
    g_enemyWeaponMesh = nullptr; g_enemySwing = 23.f; g_enemyHp = 89.f; // 每次进战斗重置
    if (partyName.empty() || partyName == "野队") { g_enemyParts.clear(); return; }
    try {
        std::filesystem::path moduleDir = std::filesystem::path(propsPath).parent_path();
        // 1) troops.txt：找 plural == partyName 的兵种，读装备 item 编号
        std::vector<int> eqItems;
        {
            std::ifstream f(moduleDir / "troops.txt");
            std::string line; int afterEq = 0; // 0=等装备行 1=属性行(str agi int cha) 2=技能行(ironflesh 第一)
            bool wantEq = false, got = false;
            while (std::getline(f, line)) {
                if (line.empty()) continue;
                size_t p0 = line.find_first_not_of(" \t");
                if (p0 == std::string::npos) continue;
                line = line.substr(p0);
                if (line.rfind("trp_", 0) == 0) {
                    std::istringstream ss(line);
                    std::string id, nm, pl;
                    if (ss >> id >> nm >> pl) wantEq = (!got && pl == partyName); // 取第一个同 plural 兵种（普通兵优先于 leader）
                    afterEq = 0;
                    continue;
                }
                if (wantEq && afterEq == 0 && std::isdigit((unsigned char)line[0])) { // 紧跟主行的装备行
                    std::istringstream ss(line);
                    int a, b;
                    while (ss >> a >> b) { if (a < 0) break; eqItems.push_back(a); }
                    wantEq = false; got = true; afterEq = 1;
                    continue;
                }
                if (afterEq == 1 && std::isdigit((unsigned char)line[0])) { // 属性行：str 第一
                    g_enemyHp = 35.f + 2.f * (float)std::atoi(line.c_str()); afterEq = 2;
                    continue;
                }
                if (afterEq == 2 && std::isdigit((unsigned char)line[0])) { // 技能行：ironflesh 第一
                    g_enemyHp += 2.f * (float)std::atoi(line.c_str()); afterEq = 0;
                    std::fprintf(stderr, "[兵种] %s HP=%.0f（35+2*(str+ironflesh)，读 troops 属性/技能行）\n", partyName.c_str(), g_enemyHp);
                    continue;
                }
            }
        }
        if (eqItems.empty()) { std::fprintf(stderr, "[兵种] 找不到 %s 的装备，敌方与玩家同款\n", partyName.c_str()); return; }
        // 2) item_kinds1.txt：编号 → 网格名 + 尾字段（判定数值一律直读模块数据）
        struct ItemInfo { std::string mesh; int body = 0, swing = 0, thrust = 0, speed = 0, wlen = 0; };
        std::map<int, ItemInfo> items;
        {
            std::ifstream f(moduleDir / "item_kinds1.txt");
            std::string line; int no = 0;
            while (std::getline(f, line)) {
                size_t p0 = line.find_first_not_of(" \t\r\n");
                if (p0 == std::string::npos) continue;
                line = line.substr(p0);
                if (line.rfind("itm_", 0) != 0) continue;
                std::istringstream ss(line);
                std::vector<std::string> t; std::string tok;
                while (ss >> tok) t.push_back(tok);
                if (t.size() < 8) { no++; continue; }
                int mc = std::atoi(t[3].c_str());
                size_t base = 4 + (size_t)mc * 2;
                if (t.size() >= base + 17) {
                    ItemInfo inf;
                    if (mc > 0) inf.mesh = t[4];
                    inf.body = std::atoi(t[base + 7].c_str());
                    inf.swing = std::atoi(t[base + 16].c_str());
                    inf.thrust = std::atoi(t[base + 15].c_str());
                    inf.speed = std::atoi(t[base + 11].c_str());
                    inf.wlen = std::atoi(t[base + 13].c_str());
                    if ((inf.body > 0 || inf.wlen > 0) && !inf.mesh.empty()) items[no] = inf;
                }
                no++;
            }
        }
        // 3) 选衣甲：body_armor 最高的 mesh 名
        std::string armorMesh; int bestBody = 0;
        for (int it : eqItems) {
            auto it2 = items.find(it);
            if (it2 != items.end() && it2->second.body > bestBody) { bestBody = it2->second.body; armorMesh = it2->second.mesh; }
        }
        if (armorMesh.empty()) { std::fprintf(stderr, "[兵种] %s 无衣甲 item，敌方与玩家同款\n", partyName.c_str()); return; }
        // 4) 找网格：先模块 Resource 全扫，再去 CommonRes 找
        brf::Mesh* armor = nullptr;
        std::string lowerName; for (char c : armorMesh) lowerName += (char)std::tolower((unsigned char)c);
        for (auto& p : std::filesystem::directory_iterator(moduleDir / "Resource")) {
            if (p.path().extension() != ".brf") continue;
            try {
                g_enemyArmorFile = brf::parse(p.path().string());
            } catch (...) { continue; }
            for (auto& m : g_enemyArmorFile.meshes) {
                std::string ln; for (char c : m.name) ln += (char)std::tolower((unsigned char)c);
                if (ln == lowerName) { armor = &m; break; }
            }
            if (armor) break;
        }
        if (!armor) {
            try {
                g_enemyArmorFile = brf::parse(commonRes + "/costumes_b.brf");
                for (auto& m : g_enemyArmorFile.meshes) {
                    std::string ln; for (char c : m.name) ln += (char)std::tolower((unsigned char)c);
                    if (ln == lowerName) { armor = &m; break; }
                }
            } catch (...) {}
        }
        if (!armor) { std::fprintf(stderr, "[兵种] 衣甲网格 %s 找不到，敌方与玩家同款\n", armorMesh.c_str()); return; }
        // 4.5) 武器：本兵种 swing 最小的近战武器（swing>0 且 wlen>0；避开 mod 神器件：Looters→hatchet、Sea_Raiders→war_axe_b）
        std::string wpnMesh; int bestSwing = 1 << 30;
        for (int it : eqItems) {
            auto it2 = items.find(it);
            if (it2 != items.end() && it2->second.wlen > 0 && it2->second.swing > 0 && it2->second.swing < bestSwing) {
                bestSwing = it2->second.swing; wpnMesh = it2->second.mesh;
            }
        }
        if (!wpnMesh.empty()) {
            g_enemySwing = (float)bestSwing;
            std::string lowerW; for (char c : wpnMesh) lowerW += (char)std::tolower((unsigned char)c);
            brf::Mesh* wpn = nullptr;
            for (auto& p : std::filesystem::directory_iterator(moduleDir / "Resource")) {
                if (p.path().extension() != ".brf") continue;
                try { g_enemyWeaponFile = brf::parse(p.path().string()); } catch (...) { continue; }
                for (auto& m : g_enemyWeaponFile.meshes) {
                    std::string ln; for (char c : m.name) ln += (char)std::tolower((unsigned char)c);
                    if (ln == lowerW) { wpn = &m; break; }
                }
                if (wpn) break;
            }
            if (!wpn) { // CommonRes 武器包兜底
                try {
                    g_enemyWeaponFile = brf::parse(commonRes + "/weapon_meshes1.brf");
                    for (auto& m : g_enemyWeaponFile.meshes) {
                        std::string ln; for (char c : m.name) ln += (char)std::tolower((unsigned char)c);
                        if (ln == lowerW) { wpn = &m; break; }
                    }
                } catch (...) {}
            }
            if (wpn) {
                g_enemyWeaponMesh = wpn;
                for (auto& mt : g_enemyWeaponFile.materials) // 武器所在 brf 材料表并入全局（贴图名解析用）
                    if (!g_materialDiffuse.count(mt.name)) g_materialDiffuse[mt.name] = mt.diffuseA;
                std::fprintf(stderr, "[兵种] %s 武器=%s swing=%g（反击伤害原值）\n", partyName.c_str(), wpnMesh.c_str(), g_enemySwing);
            } else std::fprintf(stderr, "[兵种] 武器网格 %s 找不到，敌方兜底 hatchet\n", wpnMesh.c_str());
        } else std::fprintf(stderr, "[兵种] %s 无近战武器 item，敌方兜底 hatchet\n", partyName.c_str());
        for (auto& mt : g_enemyArmorFile.materials) // 衣甲所在 brf 的材料表并入全局（贴图名解析用）
            if (!g_materialDiffuse.count(mt.name)) g_materialDiffuse[mt.name] = mt.diffuseA;
        // 5) 敌方部件 = 玩家基础 6 部件 + 本兵种衣甲 + hatchet（复用玩家武器网格）
        g_enemyParts.clear();
        for (size_t i = 0; i < g_charSkin.parts.size(); i++) {
            const auto& p = g_charSkin.parts[i];
            if (p.mesh->name == "nobleman_outfit_b") continue; // 玩家护甲剔除
            g_enemyParts.push_back(p); // body + 肢体 + 头 + hatchet 全保留
        }
        // 重新按序：先基础 body+肢体，再敌方衣甲，再武器
        std::vector<CharSkin::Part> re;
        for (auto& p : g_enemyParts) if (p.bone < 0 && p.mesh->name == "man_body") re.push_back(p);
        for (auto& p : g_enemyParts) if (p.mesh->name != "man_body" && p.mesh->name != "hatchet") re.push_back(p);
        auto wdiff = [&](const brf::Mesh* m, const std::string& fb) {
            auto dit = g_materialDiffuse.find(m->material);
            return (dit != g_materialDiffuse.end() && !dit->second.empty() && dit->second != "none") ? dit->second : fb;
        };
        re.push_back({armor, -1, wdiff(armor, g_charSkin.diffuse), false, false});
        if (g_enemyWeaponMesh) { // v0.7-6：敌兵种武器挂 hand.R，与玩家 hatchet 同约定
            int hr = -1;
            if (g_charSkin.skel)
                for (size_t bi = 0; bi < g_charSkin.skel->bones.size(); bi++)
                    if (g_charSkin.skel->bones[bi].name == "hand.R") { hr = (int)bi; break; }
            if (hr >= 0) {
                CharSkin::Part w{g_enemyWeaponMesh, hr, wdiff(g_enemyWeaponMesh, "snow"), false, false};
                w.fixSet = true; w.fix = g_partFixWeapon; // 武器专用握持：斧柄横贯手掌，刃朝前（与玩家同约定）
                re.push_back(w);
            } else std::fprintf(stderr, "[兵种] hand.R 骨找不到，敌方武器跳过\n");
        }
        if (!g_enemyWeaponMesh) for (auto& p : g_enemyParts) if (p.mesh->name == "hatchet") re.push_back(p); // 兜底：玩家同款 hatchet
        g_enemyParts = std::move(re);
        std::fprintf(stderr, "[兵种] %s 衣甲=%s(防%d) 敌方部件=%zu\n", partyName.c_str(), armor->name.c_str(), bestBody, g_enemyParts.size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[兵种] 装备解析失败: %s，敌方与玩家同款\n", e.what());
        g_enemyParts.clear();
    }
}

// 头部约定（male_head 是半脸网格，z=朝上、y=脸正前方、x=半宽）：
// part x→骨-z（世界左侧），part y→骨-y（世界 +z 脸），part z→骨+x（世界 +y 头顶）
static skin::M44 g_partFixHead = []() {
    skin::M44 H = skin::identity();
    H.m[0] = 0.f; H.m[1] = 0.f; H.m[2] = -1.f;  // col0=(0,0,-1)
    H.m[4] = 0.f; H.m[5] = -1.f; H.m[6] = 0.f;   // col1=(0,-1,0)
    H.m[8] = 1.f; H.m[9] = 0.f; H.m[10] = 0.f;   // col2=(1,0,0)
    return H;
}();
static int32_t g_lastFace = -1;
static std::vector<int32_t> g_path; static size_t g_pathIdx = 0; static bool g_wasG = false;
// v0.2-1：prop 碰撞 = 游戏原生 bo_xxx 碰撞体（sphere/capsule/manifold）投影到渲染 x/z 平面
struct ColShape {
    int type = 0;             // 0=圆 1=胶囊(线段) 2=多边形
    float r = 0.f;
    float ax = 0, az = 0;     // 圆心或线段端点 A
    float bx = 0, bz = 0;     // 线段端点 B
    float yMin = -1e9f, yMax = 1e9f; // 世界 y 垂直范围：只挡与角色身高范围有重叠的
    std::vector<std::pair<float,float>> pts; // 多边形
    std::string owner;        // 诊断：哪个 mesh 的哪个 body
};
static std::map<uint32_t, std::vector<ColShape>> g_propColMap;
static std::map<std::string, brf::Body> g_bodies;
static const float g_charRadius = 0.35f;

// 点是否在导航面多边形内（凸多边形叉积同号）
static bool pointInFace(const sco::AiMesh& m, int32_t fi, float x, float z) {
    if (fi < 0 || (size_t)fi >= m.faces.size()) return false;
    const auto& fc = m.faces[fi];
    if (fc.verts.size() < 3) return false;
    int sign = 0;
    for (size_t i=0;i<fc.verts.size();i++) {
        size_t j = (i+1)%fc.verts.size();
        uint32_t a = fc.verts[i], b = fc.verts[j];
        if (a >= m.vertX.size() || b >= m.vertX.size()) return false;
        float ax = m.vertX[a], az = m.vertY[a], bx = m.vertX[b], bz = m.vertY[b];
        float cross = (bx-ax)*(z-az) - (bz-az)*(x-ax);
        int s = cross > 1e-6f ? 1 : (cross < -1e-6f ? -1 : 0);
        if (s != 0) {
            if (sign == 0) sign = s;
            else if (sign != s) return false;
        }
    }
    return true;
}
static int32_t findFace(float x, float z) {
    if (g_lastFace >= 0 && pointInFace(g_aiMesh, g_lastFace, x, z)) return g_lastFace;
    for (size_t i=0;i<g_aiMesh.faces.size();i++)
        if (pointInFace(g_aiMesh, (int32_t)i, x, z)) { g_lastFace = (int32_t)i; return g_lastFace; }
    return -1;
}
// 碰撞：先整体移动；不行则分别只动 x / 只动 z（沿墙滑动）；都不行原地不动
static float pointSegDist2(float x, float z, float ax, float az, float bx, float bz) {
    float dx = bx-ax, dz = bz-az;
    float len2 = dx*dx + dz*dz;
    float t = len2 > 1e-9f ? ((x-ax)*dx + (z-az)*dz) / len2 : 0.f;
    t = std::max(0.f, std::min(1.f, t));
    float px = ax + t*dx, pz = az + t*dz;
    return (x-px)*(x-px) + (z-pz)*(z-pz);
}
static bool pointInPoly(float x, float z, const std::vector<std::pair<float,float>>& pts) {
    bool inside = false;
    for (size_t i = 0, j = pts.size()-1; i < pts.size(); j = i++) {
        float xi = pts[i].first, zi = pts[i].second, xj = pts[j].first, zj = pts[j].second;
        if (((zi > z) != (zj > z)) && (x < (xj-xi)*(z-zi)/(zj-zi+1e-9f)+xi)) inside = !inside;
    }
    return inside;
}
static bool circleHitsShape(float x, float z, const ColShape& s) {
    float rr = g_charRadius;
    if (s.type == 0) {
        float dx = x-s.ax, dz = z-s.az;
        float R = s.r + rr;
        return dx*dx + dz*dz < R*R;
    }
    if (s.type == 1) {
        float R = s.r + rr;
        return pointSegDist2(x, z, s.ax, s.az, s.bx, s.bz) < R*R;
    }
    if (pointInPoly(x, z, s.pts)) return true;
    for (size_t i = 0, j = s.pts.size()-1; i < s.pts.size(); j = i++)
        if (pointSegDist2(x, z, s.pts[i].first, s.pts[i].second, s.pts[j].first, s.pts[j].second) < rr*rr) return true;
    return false;
}
static bool verticalOverlap(const ColShape& s, float y0, float y1) {
    return s.yMax >= y0 && s.yMin <= y1;
}
static const ColShape* blockedByProps(float x, float z, float y0, float y1) {
    for (auto& kv : g_propColMap)
        for (auto& s : kv.second)
            if (verticalOverlap(s, y0, y1) && circleHitsShape(x, z, s)) return &s;
    return nullptr;
}
static float terrainHeightAt(float x, float z);
// v0.2-1 关键修正：ai_mesh 是给 NPC 寻路用的，不是玩家活动边界。
// 玩家在场景里应像原版一样只被真实墙体/道具挡住，不再被导航面边界做成空气墙。
static bool freeAt(float x, float z) {
    float foot = terrainHeightAt(x, z);
    return blockedByProps(x, z, foot, foot + 1.9f) == nullptr;
}
static float clampToTerrain(float v, bool xAxis) {
    if (g_aiMesh.ok) { // 以导航网格真实范围为准（场地图地形网格可能只是局部）
        static float mx0 = 1e9f, mx1 = -1e9f, mz0 = 1e9f, mz1 = -1e9f; static bool done = false;
        if (!done) {
            for (size_t i = 0; i < g_aiMesh.faces.size(); i++)
                for (uint32_t vi : g_aiMesh.faces[i].verts) {
                    if (vi >= g_aiMesh.vertX.size()) continue;
                    mx0 = std::min(mx0, g_aiMesh.vertX[vi]); mx1 = std::max(mx1, g_aiMesh.vertX[vi]);
                    mz0 = std::min(mz0, g_aiMesh.vertY[vi]); mz1 = std::max(mz1, g_aiMesh.vertY[vi]);
                }
            done = true;
        }
        float lo = xAxis ? mx0 : mz0, hi = xAxis ? mx1 : mz1;
        if (hi > lo) return std::max(lo + 0.5f, std::min(v, hi - 0.5f));
    }
    if (g_terrain.width < 2 || g_terrain.height < 2) return v;
    float lim = xAxis ? g_terrain.width*2.0f : g_terrain.height*2.0f;
    return std::max(0.f, std::min(v, lim));
}
static std::string g_lastHit;
static void logBlocked(const ColShape* s, float x, float z) {
    static double last = -1e9; static float lx=1e9f, lz=1e9f;
    double now = (double)clock() / CLOCKS_PER_SEC;
    float dx = x-lx, dz = z-lz;
    if (s && (now-last > 0.5 || dx*dx+dz*dz > 4.f)) {
        last = now; lx=x; lz=z;
        char buf[160];
        snprintf(buf, sizeof(buf), "%s @(%.0f,%.0f)", s->owner.c_str(), x, z);
        g_lastHit = buf;
        std::fprintf(stderr, "[撞] %s type=%d r=%.2f\n", buf, s->type, s->r);
    }
}
static void moveCharBlockedC(Char& c, float nx, float nz) {
    nx = clampToTerrain(nx, true); nz = clampToTerrain(nz, false);
    if (freeAt(nx, nz)) { c.x=nx; c.z=nz; return; }
    float foot = terrainHeightAt(nx, nz);
    const ColShape* hit = blockedByProps(nx, nz, foot, foot + 1.9f); logBlocked(hit, nx, nz);
    if (freeAt(nx, c.z)) { c.x=nx; return; }
    if (freeAt(c.x, nz)) { c.z=nz; return; }
}
static void moveCharBlocked(float nx, float nz) { moveCharBlockedC(g_char, nx, nz); }
// 安全出生点：离首选点最近的、圆碰撞体不与任何 prop 盒相交的导航面中心
static void safeSpawnFor(Char& c, float preferX, float preferZ) {
    if (!g_aiMesh.ok) { c.x = preferX; c.z = preferZ; return; }
    float bestD = 1e18f; bool found = false; float bx = preferX, bz = preferZ;
    for (auto& fc : g_aiMesh.faces) {
        float cx = 0, cz = 0; int n = 0;
        for (uint32_t vi : fc.verts) if (vi < g_aiMesh.vertX.size()) { cx += g_aiMesh.vertX[vi]; cz += g_aiMesh.vertY[vi]; n++; }
        if (!n) continue;
        cx /= n; cz /= n;
        if (!freeAt(cx, cz)) continue;
        float d = (cx-preferX)*(cx-preferX) + (cz-preferZ)*(cz-preferZ);
        if (d < bestD) { bestD = d; bx = cx; bz = cz; found = true; }
    }
    c.x = bx; c.z = bz;
    if (!found) std::fprintf(stderr, "警告：没找到无 prop 的导航面，出生在 (%.1f,%.1f)\n", preferX, preferZ);
}
static void safeSpawn(float preferX, float preferZ) { safeSpawnFor(g_char, preferX, preferZ); }
// 队伍站位：尽量按给的偏移精确站（不要导航面中心吸附，否则一大片人叠在同一点）
static void spawnNear(Char& c, float preferX, float preferZ) {
    if (freeAt(preferX, preferZ)) { c.x = preferX; c.z = preferZ; return; }
    for (float r = 1.f; r <= 6.f; r += 1.f) {
        for (int k = 0; k < 16; k++) {
            float a = (float)k / 16.f * 6.2831853f;
            float x = preferX + cosf(a)*r, z = preferZ + sinf(a)*r;
            if (freeAt(x, z)) { c.x = x; c.z = z; return; }
        }
    }
    safeSpawnFor(c, preferX, preferZ);
}
static void faceCenter(int32_t fi, float& x, float& z) {
    x=z=0; int n=0;
    for (uint32_t vi : g_aiMesh.faces[fi].verts) if (vi < g_aiMesh.vertX.size()) { x+=g_aiMesh.vertX[vi]; z+=g_aiMesh.vertY[vi]; n++; }
    if (n) { x/=n; z/=n; }
}
// BFS 面邻接寻路（导航网格；边表 face 引用在此文件不可靠，按共享顶点建邻接）
static std::vector<int32_t> findFacePath(int32_t from, int32_t to) {
    std::vector<int32_t> out;
    if (from < 0 || to < 0) return out;
    if (from == to) { out.push_back(from); return out; }
    size_t nf = g_aiMesh.faces.size();
    std::vector<std::vector<int32_t>> adj(nf);
    for (size_t i=0;i<nf;i++) {
        std::set<uint32_t> vi(g_aiMesh.faces[i].verts.begin(), g_aiMesh.faces[i].verts.end());
        for (size_t j=i+1;j<nf;j++) {
            for (uint32_t v : g_aiMesh.faces[j].verts)
                if (vi.count(v)) { adj[i].push_back((int32_t)j); adj[j].push_back((int32_t)i); break; }
        }
    }
    std::vector<int32_t> prev(nf, -2); std::vector<int32_t> q;
    q.push_back(from); prev[from]=-1;
    bool found=false;
    for (size_t h=0;h<q.size();h++) {
        int32_t c=q[h];
        if (c==to) { found=true; break; }
        for (int32_t nb : adj[c]) if (prev[nb] == -2) { prev[nb]=c; q.push_back(nb); }
    }
    if (!found) return out;
    int32_t c=to; out.push_back(c);
    while (prev[c] != -1) { c=prev[c]; out.push_back(c); }
    std::reverse(out.begin(), out.end());
    return out;
}

struct Cam { float yaw=0.f, height=5.f, dist=9.f; };          // 第三人称相机：绕角色的水平角/高度/距离
static float terrainHeightAt(float x, float z) {
    if (g_terrain.width < 2 || g_terrain.height < 2 || g_terrain.elevation.empty()) return 0.f;
    float fi = x/2.f, fj = z/2.f; // cell=2m，原点(0,0)
    int i0 = (int)floorf(fi), j0 = (int)floorf(fj);
    i0 = std::max(0, std::min(i0, (int)g_terrain.width-1));
    j0 = std::max(0, std::min(j0, (int)g_terrain.height-1));
    int i1 = std::min(i0+1, (int)g_terrain.width-1), j1 = std::min(j0+1, (int)g_terrain.height-1);
    float fx = std::max(0.f, std::min(fi-i0, 1.f)), fz = std::max(0.f, std::min(fj-j0, 1.f));
    auto e = [&](int i, int j) -> float {
        size_t idx = (size_t)j*g_terrain.width+i;
        return idx < g_terrain.elevation.size() ? g_terrain.elevation[idx] : 0.f;
    };
    float h0 = e(i0,j0)*(1-fx) + e(i1,j0)*fx;
    float h1 = e(i0,j1)*(1-fx) + e(i1,j1)*fx;
    return h0*(1-fz) + h1*fz;
}
// 第三人称相机 eye/target（渲染坐标）
static void playerFrame(const Char& ch, const Cam& cam, float eye[3], float target[3]) {
    float a = cam.yaw;
    eye[0] = ch.x + sinf(a)*cam.dist;
    eye[1] = ch.z + cosf(a)*cam.dist;
    eye[2] = terrainHeightAt(eye[0], eye[1]) + cam.height;
    target[0] = ch.x; target[1] = ch.z; target[2] = terrainHeightAt(ch.x, ch.z) + 1.5f;
}
// 更新角色移动与第三人称相机参数；返回是否有输入
static bool updatePlayer(Cam& cam, float dt) {
    float sx=0, sz=0, dy=0, dp=0, dd=0;
    auto k = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (k('W')) { sz += 1; g_lastMoveDir = 2; }   // 前→上劈（骑砍手感：攻击方向=最后移动方向）
    if (k('S')) { sz -= 1; g_lastMoveDir = 3; }   // 后→刺
    if (k('D')) { sx += 1; g_lastMoveDir = 0; }   // 右→righttoleft（画面从右往左挥）
    if (k('A')) { sx -= 1; g_lastMoveDir = 1; }   // 左→lefttoright
    if (k(VK_LEFT)) dy += 1; if (k(VK_RIGHT)) dy -= 1;
    if (k(VK_UP)) dp += 1; if (k(VK_DOWN)) dp -= 1;
    if (k('E')) dd -= 1; if (k('Q')) dd += 1;   // QE 拉近/拉远相机
    bool gDown = k('G');
    if (gDown && !g_wasG) { // G：A* 寻路到离当前最远的可走面，自动走
        int32_t from = findFace(g_char.x, g_char.z);
        float fx0=0, fz0=0; if (from >= 0) faceCenter(from, fx0, fz0);
        int32_t to = from; float best = -1.f;
        for (size_t i=0;i<g_aiMesh.faces.size();i++) {
            float cx=0, cz=0; faceCenter((int32_t)i, cx, cz);
            float d = (cx-fx0)*(cx-fx0) + (cz-fz0)*(cz-fz0);
            if (d > best) { best = d; to = (int32_t)i; }
        }
        g_path = findFacePath(from, to);
        g_pathIdx = 0;
        std::fprintf(stderr, "寻路: 起点face=%d 目标face=%d 路径长度=%zu\n", from, to, g_path.size());
    }
    g_wasG = gDown;
    cam.yaw += dy*1.6f*dt;
    cam.height = std::max(2.0f, std::min(20.f, cam.height + dp*6.f*dt));
    cam.dist = std::max(4.0f, std::min(30.f, cam.dist + dd*10.f*dt));
    float speed = (k(VK_SHIFT) ? 12.f : 5.5f) * dt;
    if (sx || sz) g_path.clear(); // 手动接管
    if (!g_path.empty() && g_pathIdx < g_path.size()) {
        float wx=0, wz=0;
        faceCenter(g_path[g_pathIdx], wx, wz);
        float dx = wx - g_char.x, dz = wz - g_char.z;
        float dl = sqrtf(dx*dx + dz*dz);
        if (dl < 0.6f) { g_pathIdx++; }
        else {
            float step = std::min(dl, speed);
            moveCharBlocked(g_char.x + dx/dl*step, g_char.z + dz/dl*step);
            float ty = atan2f(-dx/dl, -dz/dl);
            float dyy = ty - g_char.yaw;
            while (dyy > 3.14159f) dyy -= 6.28318f;
            while (dyy < -3.14159f) dyy += 6.28318f;
            g_char.yaw += dyy * std::min(1.f, 8.f*dt);
        }
        return true;
    }
    if (!(sx||sz||dy||dp||dd)) return false;
    // 以相机方向为基准：相机看向角色，前方 = 从相机到角色的水平方向
    float fx = -sinf(cam.yaw), fz = -cosf(cam.yaw);
    float rx = cosf(cam.yaw), rz = -sinf(cam.yaw);
    float mx = fx*sz + rx*sx;
    float mz = fz*sz + rz*sx;
    float ml = sqrtf(mx*mx + mz*mz);
    if (ml > 1e-6f) {
        moveCharBlocked(g_char.x + mx/ml * speed, g_char.z + mz/ml * speed);
        if (sz != 0.f) { // 纯 A/D 横移不转身（strafe），W/S 才转
            float targetYaw = atan2f(-mx/ml, mz/ml); // 引擎约定：前向量=(-sin yaw, cos yaw)，0°=+Y
            float dyaw = targetYaw - g_char.yaw;
            while (dyaw > 3.14159f) dyaw -= 6.28318f;
            while (dyaw < -3.14159f) dyaw += 6.28318f;
            g_char.yaw += dyaw * std::min(1.f, 8.f*dt);
        }
    }
    return true;
}

// "arena_barrier_b.4" -> "arena_barrier_b"；"name.lod2.1" 是 LOD 变体（lod>0 跳过）
static std::string meshBaseName(const std::string& name) {
    size_t dot = name.find('.');
    if (dot == std::string::npos) return name;
    return name.substr(0, dot);
}
static bool meshIsLodVariant(const std::string& name) {
    size_t dot = name.find('.');
    if (dot == std::string::npos || dot+5 >= name.size()) return false;
    const char* s = name.c_str()+dot;
    return (s[1]=='l'||s[1]=='L') && (s[2]=='o'||s[2]=='O') && (s[3]=='d'||s[3]=='D') && isdigit((unsigned char)s[4]);
}

// 生成一个场景物件实例的 GPU 缓冲与 MVP（boxIdx = 实例序号；bodyName = bo_xxx 碰撞体名，空则不参与碰撞）
static void makeSceneObj(App& A, const brf::Mesh& mesh, const sco::Prop& prop, float cx, float cz,
                         uint32_t boxIdx, const std::string& bodyName) {
    std::vector<float> verts; verts.reserve(mesh.verts.size()*11);
    for (auto& v : mesh.verts) {
        if (v.index < 0 || (size_t)v.index >= mesh.positions.size()) continue;
        auto& p = mesh.positions[v.index];
        // SCO 3x3 矩阵的约定：BRF 场景物件按“z 为摆放置向”参与旋转，即先 y/z 互换，
        // 乘矩阵后再换回（等价 S·M·S）。直接按 (x,y,z) 乘会把房子绕错轴、翻个儿。
        float bx = prop.m[0]*p.x + prop.m[1]*p.z + prop.m[2]*p.y;
        float by = prop.m[6]*p.x + prop.m[7]*p.z + prop.m[8]*p.y; // 世界高度 = M 第三行作用在 (x,z,y)
        float bz = prop.m[3]*p.x + prop.m[4]*p.z + prop.m[5]*p.y; // 世界水平 = M 第二行
        bx *= prop.scale[0]; by *= prop.scale[1]; bz *= prop.scale[2];
        float wx = prop.pos[0]+bx, wy = prop.pos[1]+by, wz = prop.pos[2]+bz;
        // BRF -> 渲染坐标：x'=x, y'=z, z'=+y（场景物件经 S·M·S 后 y 即世界高度，取正）
        verts.push_back(wx); verts.push_back(wz); verts.push_back(wy);
        float r = ((v.color) & 0xFF) / 255.0f;
        float g = ((v.color >> 8) & 0xFF) / 255.0f;
        float b = ((v.color >> 16) & 0xFF) / 255.0f;
        verts.push_back(r); verts.push_back(g); verts.push_back(b);
        // 法线：同一 S·M·S 变换 + 坐标映射，归一化
        float nx = prop.m[0]*v.normal.x + prop.m[1]*v.normal.z + prop.m[2]*v.normal.y;
        float ny = prop.m[6]*v.normal.x + prop.m[7]*v.normal.z + prop.m[8]*v.normal.y;
        float nz = prop.m[3]*v.normal.x + prop.m[4]*v.normal.z + prop.m[5]*v.normal.y;
        float nl = sqrtf(nx*nx+ny*ny+nz*nz); if (nl < 1e-6f) nl = 1.f;
        verts.push_back(nx/nl); verts.push_back(nz/nl); verts.push_back(ny/nl);
        verts.push_back(v.uv.u); verts.push_back(v.uv.v);
    }
    if (!bodyName.empty() && bodyName != "0") {
        auto bit = g_bodies.find(bodyName);
        if (bit != g_bodies.end()) {
            auto xf = [&](brf::Vec3 p) {
                float bx = prop.m[0]*p.x + prop.m[1]*p.z + prop.m[2]*p.y;
                float bz = prop.m[3]*p.x + prop.m[4]*p.z + prop.m[5]*p.y;
                bx *= prop.scale[0]; bz *= prop.scale[2];
                return std::make_pair(prop.pos[0] + bx, prop.pos[2] + bz);
            };
            auto xfY = [&](brf::Vec3 p) {
                float by = prop.m[6]*p.x + prop.m[7]*p.z + prop.m[8]*p.y;
                by *= prop.scale[1];
                return prop.pos[1] + by;
            };
            float row1n = sqrtf(prop.m[6]*prop.m[6] + prop.m[7]*prop.m[7] + prop.m[8]*prop.m[8]);
            float maxScale = std::max({fabsf(prop.scale[0]), fabsf(prop.scale[1]), fabsf(prop.scale[2])});
            float rScale = std::max(prop.scale[0], prop.scale[2]);
            std::string owner = mesh.name + "[" + bodyName + "]";
            for (auto& part : bit->second.parts) {
                if (part.type == 0) {
                    ColShape s; s.type = 0; s.owner = owner;
                    auto p = xf(part.c); s.ax = p.first; s.az = p.second; s.r = part.radius*rScale;
                    float cy = xfY(part.c), ry = part.radius * row1n * maxScale;
                    s.yMin = cy - ry; s.yMax = cy + ry;
                    g_propColMap[boxIdx].push_back(s);
                } else if (part.type == 1) {
                    ColShape s; s.type = 1; s.owner = owner;
                    auto a = xf(part.c), b = xf(part.d);
                    s.ax=a.first; s.az=a.second; s.bx=b.first; s.bz=b.second; s.r = part.radius*rScale;
                    float ry = part.radius * row1n * maxScale;
                    float y0 = xfY(part.c), y1 = xfY(part.d);
                    s.yMin = std::min(y0, y1) - ry; s.yMax = std::max(y0, y1) + ry;
                    g_propColMap[boxIdx].push_back(s);
                } else {
                    auto addFace = [&](const std::vector<std::pair<float,float>>& fp, float y0, float y1) {
                        if (fp.size() < 3) return;
                        ColShape s; s.type = 2; s.owner = owner; s.pts = fp;
                        s.yMin = y0; s.yMax = y1;
                        g_propColMap[boxIdx].push_back(s);
                    };
                    auto faceYs = [&](const std::vector<int32_t>& face, const std::vector<std::pair<float,float>>& fp) {
                        float y0 = 1e9f, y1 = -1e9f;
                        for (int32_t fi : face)
                            if (fi >= 0 && (size_t)fi < part.pts.size()) {
                                float y = xfY(part.pts[(size_t)fi]);
                                y0 = std::min(y0, y); y1 = std::max(y1, y);
                            }
                        if (y0 > y1) { y0 = y1 = 0.f; }
                        addFace(fp, y0, y1);
                    };
                    if (part.faces.empty()) {
                        std::vector<std::pair<float,float>> fp;
                        float y0 = 1e9f, y1 = -1e9f;
                        for (auto& p : part.pts) { auto q = xf(p); fp.push_back(q); y0 = std::min(y0, xfY(p)); y1 = std::max(y1, xfY(p)); }
                        if (y0 > y1) { y0 = y1 = 0.f; }
                        addFace(fp, y0, y1);
                    } else {
                        for (auto& face : part.faces) {
                            std::vector<std::pair<float,float>> fp;
                            for (int32_t fi : face)
                                if (fi >= 0 && (size_t)fi < part.pts.size()) { auto q = xf(part.pts[(size_t)fi]); fp.push_back(q); }
                            faceYs(face, fp);
                        }
                    }
                }
            }
        }
    }
    App::Obj obj;
    std::string diff = "shields";
    auto dit = g_materialDiffuse.find(mesh.material);
    if (dit != g_materialDiffuse.end() && !dit->second.empty() && dit->second != "none") diff = dit->second;
    auto ti = A.texIndex.find(diff);
    obj.texSlot = (ti != A.texIndex.end()) ? ti->second : 0;
    obj.descSet = A.texSets.empty() ? VK_NULL_HANDLE : A.texSets[obj.texSlot];
    obj.diffName = diff;
    makeBuffer(A, verts.size()*4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, obj.vb, obj.vm);
    upload(A, obj.vb, obj.vm, verts.data(), verts.size()*4);
    obj.idxCount = (uint32_t)mesh.indices.size();
    makeBuffer(A, obj.idxCount*4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, obj.ib, obj.im);
    upload(A, obj.ib, obj.im, mesh.indices.data(), obj.idxCount*4);
    float eye0[3], tgt0[3]; camFrame(cx, cz, 55.f, 0.f, -0.32f, eye0, tgt0);
    computeSceneMVP(obj.pc, eye0, tgt0);
    if (A.objs.size() < 6) {
        float v[4] = {prop.pos[0], prop.pos[2], -prop.pos[1], 1.f};
        float c[4] = {};
        for (int r=0;r<4;r++) for (int k=0;k<4;k++) c[r] += obj.pc[k*4+r]*v[k];
        std::fprintf(stderr, "  obj[%zu] mesh=%s 顶点=%zu 索引=%u pos=(%.1f %.1f %.1f) clip=(%.2f %.2f %.2f %.2f) ndc=(%.2f %.2f)\n",
            A.objs.size(), mesh.name.c_str(), verts.size()/11, obj.idxCount, prop.pos[0], prop.pos[1], prop.pos[2],
            c[0], c[1], c[2], c[3], c[3]!=0?c[0]/c[3]:9, c[3]!=0?c[1]/c[3]:9);
    }
    A.objs.push_back(std::move(obj));
}

// 地面层规格：与 Data/ground_specs.txt 行序一致，index 0..10
struct GroundSpec { int index = 0; std::string name, mat, diffuse; float tiling = 4.f; };
static std::vector<GroundSpec> loadGroundSpecs(const std::string& path) {
    std::vector<GroundSpec> v;
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "无 ground_specs.txt，地面退化为单贴图\n"); return v; }
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        GroundSpec g;
        std::string flags;
        if (!(ss >> g.name >> flags >> g.mat >> g.tiling)) continue;
        g.index = (int)v.size();
        v.push_back(std::move(g));
    }
    if (v.size() != 11) { std::fprintf(stderr, "ground_specs 行数异常 %zu\n", v.size()); v.clear(); }
    return v;
}

// 地面：单网格三贴图真混合。每个 cell 的 1-3 个非零图层强度归一化成权重
// （RGB），leveling 顶点色作 tint，3 组 uv 分别按各层 tiling 平铺。
static void makeGroundObj(App& A, const sco::Terrain& t, const std::vector<GroundSpec>& specs,
                          float cx, float cz, std::set<std::string>& needDds) {
    uint32_t w = t.width, h = t.height;
    if (w < 2 || h < 2 || t.elevation.empty()) return;
    const float cell = 2.0f;
    size_t blockCount = (size_t)w * h;
    auto hAt = [&](uint32_t i, uint32_t j) -> float {
        size_t idx = (size_t)j*w+i;
        return idx < t.elevation.size() ? t.elevation[idx] : 0.f;
    };
    // 选 3 个贴图层：非零 cell 数从大到小
    std::vector<std::pair<int,uint64_t>> ranks;
    for (auto& l : t.layers) {
        if (l.index < 0 || l.index > 10 || !l.enabled || l.cells.size() != blockCount) continue;
        uint64_t nz = 0; for (uint8_t v : l.cells) if (v) nz++;
        if (nz) ranks.push_back({l.index, nz});
    }
    std::sort(ranks.begin(), ranks.end(), [](auto& a, auto& b){ return a.second > b.second; });
    for (int k=0;k<3;k++) g_groundLayers[k] = k < (int)ranks.size() ? ranks[k].first : g_groundLayers[k];
    float tiles[3] = {4.f, 4.f, 4.f};
    for (int k=0;k<3;k++) {
        g_groundDiffs[k] = "ground_turf";
        for (auto& g : specs) if (g.index == g_groundLayers[k]) {
            if (!g.diffuse.empty()) g_groundDiffs[k] = g.diffuse;
            tiles[k] = g.tiling;
            break;
        }
        needDds.insert(g_groundDiffs[k]);
    }
    auto* lvl = t.findLayer(-12565);
    const bool hasLeveling = lvl && lvl->colors.size() == blockCount;
    std::vector<float> V; std::vector<uint32_t> I;
    V.reserve(blockCount * 4 * 18);
    I.reserve(blockCount * 6);
    uint64_t mixedCells = 0;
    auto layerVal = [&](int li, size_t idx) -> uint8_t {
        auto* l = t.findLayer(li);
        return (l && idx < l->cells.size()) ? l->cells[idx] : 0;
    };
    for (uint32_t j=0;j<h-1;j++) for (uint32_t i=0;i<w-1;i++) {
        size_t idx = (size_t)j*w+i;
        int nz = (layerVal(g_groundLayers[0], idx)>0?1:0) + (layerVal(g_groundLayers[1], idx)>0?1:0) + (layerVal(g_groundLayers[2], idx)>0?1:0);
        if (nz > 1) mixedCells++;
        uint32_t base = (uint32_t)(V.size()/18);
        uint32_t cs[4][2] = {{i,j},{i+1,j},{i,j+1},{i+1,j+1}};
        for (auto& c : cs) {
            // 权重与 leveling 颜色都是逐顶点数据（width*height），角点各取各的，跨格平滑
            size_t cidx = (size_t)c[1]*w + c[0];
            float s[3] = {0,0,0};
            for (int k=0;k<3;k++) s[k] = layerVal(g_groundLayers[k], cidx);
            float sum = s[0]+s[1]+s[2];
            if (sum <= 0.f) { s[0] = 1.f; sum = 1.f; } // 全 0：第一层兜底（按惯例是 turf）
            float wt[3] = {s[0]/sum, s[1]/sum, s[2]/sum};
            float tint[3] = {1.f, 1.f, 1.f};
            if (hasLeveling) {
                uint32_t col = lvl->colors[cidx];
                tint[0] = ((col >> 16) & 0xFF) / 255.0f;
                tint[1] = ((col >> 8) & 0xFF) / 255.0f;
                tint[2] = (col & 0xFF) / 255.0f;
            }
            float x = c[0]*cell, z = c[1]*cell, y = hAt(c[0], c[1]);
            V.push_back(x); V.push_back(z); V.push_back(y);
            V.push_back(wt[0]); V.push_back(wt[1]); V.push_back(wt[2]);
            V.push_back(tint[0]); V.push_back(tint[1]); V.push_back(tint[2]);
            float lx = hAt(c[0]>0?c[0]-1:0, c[1]);
            float rx = hAt(c[0]+1<w?c[0]+1:w-1, c[1]);
            float ty = hAt(c[0], c[1]>0?c[1]-1:0);
            float by = hAt(c[0], c[1]+1<h?c[1]+1:h-1);
            float dhdx = (rx-lx)/(2*cell), dhdz = (by-ty)/(2*cell);
            float fx = -dhdx, fy = -dhdz, fz = 1.0f;
            float fl = sqrtf(fx*fx+fy*fy+fz*fz); if (fl<1e-6f) fl=1;
            V.push_back(fx/fl); V.push_back(fy/fl); V.push_back(fz/fl);
            for (int k=0;k<3;k++) { V.push_back(x/tiles[k]); V.push_back(z/tiles[k]); }
        }
        I.push_back(base); I.push_back(base+2); I.push_back(base+1);
        I.push_back(base+1); I.push_back(base+2); I.push_back(base+3);
    }
    App::Obj obj;
    obj.ground = true;
    obj.diffName = g_groundDiffs[0];
    obj.descSet = A.groundDescSet; // initPipeline 之后会统一再指
    makeBuffer(A, V.size()*4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, obj.vb, obj.vm);
    upload(A, obj.vb, obj.vm, V.data(), V.size()*4);
    obj.idxCount = (uint32_t)I.size();
    makeBuffer(A, obj.idxCount*4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, obj.ib, obj.im);
    upload(A, obj.ib, obj.im, I.data(), obj.idxCount*4);
    float eye0[3], tgt0[3]; camFrame(cx, cz, 55.f, 0.f, -0.32f, eye0, tgt0);
    computeSceneMVP(obj.pc, eye0, tgt0);
    A.objs.push_back(std::move(obj));
    std::fprintf(stderr, "地面 %ux%u cell=%.0fm 单网格三贴图: L%d:%s L%d:%s L%d:%s 混合cell=%llu 顶点=%zu\n",
        w, h, cell,
        g_groundLayers[0], g_groundDiffs[0].c_str(),
        g_groundLayers[1], g_groundDiffs[1].c_str(),
        g_groundLayers[2], g_groundDiffs[2].c_str(),
        (unsigned long long)mixedCells, V.size()/18);
}
// 简易人形占位角色：程序化 box 组成（脚在 z=0，头在 z≈1.7，前方 +y）
static void boxPart(std::vector<float>& V, std::vector<uint32_t>& I,
                    float x0,float x1,float y0,float y1,float z0,float z1,
                    float r,float g,float b) {
    uint32_t base=(uint32_t)(V.size()/11);
    float cs[8][3]={{x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}};
    // 6 面，法线朝外
    const int faces[6][4]={{0,3,2,1},{4,5,6,7},{0,1,5,4},{2,3,7,6},{1,2,6,5},{0,4,7,3}};
    const float ns[6][3]={{0,0,-1},{0,0,1},{0,-1,0},{0,1,0},{1,0,0},{-1,0,0}};
    for (int f=0;f<6;f++) {
        for (int q=0;q<4;q++) {
            const float* c=cs[faces[f][q]];
            V.push_back(c[0]); V.push_back(c[1]); V.push_back(c[2]);
            V.push_back(r); V.push_back(g); V.push_back(b);
            V.push_back(ns[f][0]); V.push_back(ns[f][1]); V.push_back(ns[f][2]);
            V.push_back(0); V.push_back(0);
        }
        uint32_t b0=base+f*4;
        I.push_back(b0); I.push_back(b0+2); I.push_back(b0+1);
        I.push_back(b0); I.push_back(b0+3); I.push_back(b0+2);
    }
}
// ---------- 原版角色：skeleton + skinning（CPU 蒙皮，walk 循环动画） ----------
static void loadCharSkin(const std::string& commonRes) {
    if (g_charSkin.ok) return;
    try {
        g_charSkin.skelFile = brf::parse(commonRes + "/skeletons.brf");
        g_charSkin.animFile = brf::parse(commonRes + "/ani_man_walk.brf");
        g_charSkin.bodyFile = brf::parse(commonRes + "/body_meshes.brf");
        g_charSkin.faceFile = brf::parse(commonRes + "/meshes_face_gen.brf");
        g_charSkin.standFile = brf::parse(commonRes + "/ani_stand.brf");
        g_charSkin.runFile = brf::parse(commonRes + "/ani_run_p.brf");
        for (auto& s : g_charSkin.skelFile.skeletons) if (s.name == "skel_human") { g_charSkin.skel = &s; break; }
        for (auto& a : g_charSkin.animFile.anims) if (a.name == "man_walk") { g_charSkin.anim = &a; break; }
        for (auto& a : g_charSkin.standFile.anims) if (a.name == "stand_man") { g_charSkin.standAnim = &a; break; }
        for (auto& a : g_charSkin.runFile.anims) if (a.name == "run_man_forward") { g_charSkin.runAnim = &a; break; }
        for (auto& m : g_charSkin.bodyFile.meshes) if (m.name == "man_body") { g_charSkin.body = &m; break; }
        g_charSkin.curAnim = g_charSkin.standAnim;
        if (!g_charSkin.skel || !g_charSkin.anim || !g_charSkin.standAnim || !g_charSkin.runAnim || !g_charSkin.body) {
            std::fprintf(stderr, "原版角色资源不完整（skel/anim/body），回退占位 box 人\n");
            g_charSkin.ok = false;
            return;
        }
        skin::bindGlobals(*g_charSkin.skel, g_charSkin.bind);
        auto diffFor = [&](const std::string& mat, const std::string& fallback) -> std::string {
            auto dit = g_materialDiffuse.find(mat);
            if (dit != g_materialDiffuse.end() && !dit->second.empty() && dit->second != "none") return dit->second;
            return fallback;
        };
        auto boneIdx = [&](const std::string& name) -> int {
            for (size_t i = 0; i < g_charSkin.skel->bones.size(); i++)
                if (g_charSkin.skel->bones[i].name == name) return (int)i;
            return -1;
        };
        auto findMesh = [&](std::vector<brf::Mesh>& vec, const std::string& name) -> brf::Mesh* {
            for (auto& m : vec) if (m.name == name) return &m;
            return nullptr;
        };
        g_charSkin.diffuse = diffFor(g_charSkin.body->material, "snow");
        g_charSkin.parts.clear();
        g_charSkin.parts.push_back({g_charSkin.body, -1, g_charSkin.diffuse});
        // 附属部件：无蒙皮，整体跟随对应骨骼（部件空间长轴 +z → 骨局部 +x）
        auto addPart = [&](std::vector<brf::Mesh>& vec, const std::string& mesh, const std::string& bone,
                           const std::string& fallbackDiff, bool mirror = false, bool isHead = false) {
            brf::Mesh* m = findMesh(vec, mesh);
            int bi = boneIdx(bone);
            if (!m || bi < 0) { std::fprintf(stderr, "缺部件 %s/%s，跳过\n", mesh.c_str(), bone.c_str()); return; }
            g_charSkin.parts.push_back({m, bi, diffFor(m->material, fallbackDiff), mirror, isHead});
        };
        addPart(g_charSkin.bodyFile.meshes, "man_calf_l", "calf.L", g_charSkin.diffuse);
        addPart(g_charSkin.bodyFile.meshes, "man_calf_r", "calf.R", g_charSkin.diffuse);
        addPart(g_charSkin.bodyFile.meshes, "m_handL", "hand.L", g_charSkin.diffuse);
        addPart(g_charSkin.bodyFile.meshes, "m_handR", "hand.R", g_charSkin.diffuse);
        addPart(g_charSkin.faceFile.meshes, "male_head", "head", "manface_young", true, true); // 头网格是半脸，引擎镜像补全
        // v0.7 装备（素材全用游戏自带）：护甲=蒙皮网格（bone=-1 走蒙皮），武器=刚体网格挂右手骨
        try {
            g_charSkin.armorFile = brf::parse(commonRes + "/costumes_b.brf");
            brf::Mesh* armor = nullptr;
            for (auto& m : g_charSkin.armorFile.meshes) if (m.name == "nobleman_outfit_b") { armor = &m; break; }
            if (armor) {
                g_charSkin.parts.push_back({armor, -1, diffFor(armor->material, g_charSkin.diffuse), false, false});
            } else std::fprintf(stderr, "缺护甲网格 nobleman_outfit_b，跳过（不影响运行）\n");
            g_charSkin.weaponFile = brf::parse(commonRes + "/weapon_meshes1.brf");
            brf::Mesh* wpn = nullptr;
            for (auto& m : g_charSkin.weaponFile.meshes) if (m.name == "hatchet") { wpn = &m; break; }
            int hr = boneIdx("hand.R"); // 相机已在角色背后（画面右=角色右），BRF 语义恢复：右手武器挂 hand.R
            if (wpn && hr >= 0) {
                CharSkin::Part wp{wpn, hr, diffFor(wpn->material, "snow"), false, false};
                wp.fixSet = true; wp.fix = g_partFixWeapon; // 武器专用握持：斧柄横贯手掌，刃朝前（与手部件约定分开）
                g_charSkin.parts.push_back(wp);
            } else std::fprintf(stderr, "缺武器网格 hatchet 或 hand.R 骨，跳过\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "装备加载失败: %s（继续跑）\n", e.what());
        }
        // 攻击动画四向（CV War 原版素材；方向 = 最后移动方向，静止默认 righttoleft）
        try {
            g_charSkin.attackFile = brf::parse(commonRes + "/ani_attacks.brf");
            static const char* kAtk[4] = {
                "attacks_single_righttoleft", "attacks_single_lefttoright",
                "attacks_single_overswing", "attacks_thrust_onehanded" };
            for (int d = 0; d < 4; d++) {
                for (auto& a : g_charSkin.attackFile.anims)
                    if (a.name == kAtk[d]) {
                        g_attackMirrors[d] = a;   // 副本播放：根位移清零，素材数据不动
                        for (auto& fr : g_attackMirrors[d].frames) fr.tra = {0.f, 0.f, 0.f};
                        break;
                    }
                if (g_attackMirrors[d].frames.empty())
                    std::fprintf(stderr, "缺攻击动画 %s，该方向退化为默认\n", kAtk[d]);
                else
                    std::fprintf(stderr, "攻击动画[%d]=%s (%zu 帧，根位移清零)\n", d, kAtk[d], g_attackMirrors[d].frames.size());
            }
            g_charSkin.attackAnim = g_attackMirrors[0].frames.empty() ? nullptr : &g_attackMirrors[0];
        } catch (const std::exception& e) {
            std::fprintf(stderr, "攻击动画加载失败: %s（继续跑）\n", e.what());
        }
        g_charSkin.ok = true;
        std::fprintf(stderr, "原版角色: %s(%zu骨) stand=%s(%zu帧) walk=%s(%zu帧) run=%s(%zu帧) 部件%zu: ",
            g_charSkin.skel->name.c_str(), g_charSkin.skel->bones.size(),
            g_charSkin.standAnim->name.c_str(), g_charSkin.standAnim->frames.size(),
            g_charSkin.anim->name.c_str(), g_charSkin.anim->frames.size(),
            g_charSkin.runAnim->name.c_str(), g_charSkin.runAnim->frames.size(),
            g_charSkin.parts.size());
        for (auto& p : g_charSkin.parts)
            std::fprintf(stderr, "[%s/%s] ", p.mesh->name.c_str(), p.diff.c_str());
        std::fprintf(stderr, "\n");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "原版角色加载失败: %s，回退占位 box 人\n", e.what());
        g_charSkin.ok = false;
    }
}

// 生成一帧全部部件的 11-float 渲染顶点（角色本地：前方 +y，脚底统一贴地 z≈0）
static void buildAllCharVerts(const std::vector<CharSkin::Part>& parts, std::vector<std::vector<float>>& out, bool tPose, const brf::AnimFrame* forced = nullptr) {
    out.assign(parts.size(), {});
    std::vector<skin::M44> ag, skm;
    if (!tPose) {
        const brf::AnimFrame& fr = forced ? *forced
            : ((g_charSkin.attackAnim && g_charSkin.curAnim == g_charSkin.attackAnim)
                ? g_mixFrame
                : g_charSkin.curAnim->frames[g_charSkin.frame % g_charSkin.curAnim->frames.size()]);
        skin::animGlobals(*g_charSkin.skel, fr, ag);
        skin::skinMatrices(g_charSkin.bind, ag, skm);
    }
    struct Staged { std::vector<brf::Vec3> pos, nrm; };
    std::vector<Staged> st(parts.size());
    float minY = 1e9f;
    for (size_t pi = 0; pi < parts.size(); pi++) {
        const brf::Mesh& m = *parts[pi].mesh;
        Staged& s = st[pi];
        s.nrm.resize(m.verts.size());
        if (parts[pi].bone < 0) {
            s.pos.resize(m.positions.size());
            for (size_t i = 0; i < m.positions.size(); i++) s.pos[i] = m.positions[i];
            if (!tPose) {
                std::vector<brf::Vec3> skpos, sknrm;
                skin::skinMesh(m, skm, skpos, sknrm);
                s.pos = std::move(skpos); s.nrm = std::move(sknrm);
            } else {
                for (size_t i = 0; i < m.verts.size(); i++) s.nrm[i] = m.verts[i].normal;
            }
        } else {
            const skin::M44& fix = parts[pi].fixSet ? parts[pi].fix
                                                   : (parts[pi].isHead ? g_partFixHead : g_partFixLimb);
            skin::M44 M = tPose ? skin::mul(g_charSkin.bind[parts[pi].bone], fix)
                                : skin::mul(ag[parts[pi].bone], fix);
            if (!parts[pi].mirror) {
                s.pos.resize(m.positions.size());
                for (size_t i = 0; i < m.positions.size(); i++) { s.pos[i] = m.positions[i]; skin::xformPos(M, s.pos[i]); }
                for (size_t i = 0; i < m.verts.size(); i++) {
                    s.nrm[i] = m.verts[i].normal;
                    skin::xformDir(M, s.nrm[i]);
                    float nl = sqrtf(s.nrm[i].x*s.nrm[i].x + s.nrm[i].y*s.nrm[i].y + s.nrm[i].z*s.nrm[i].z);
                    if (nl > 1e-6f) { s.nrm[i].x/=nl; s.nrm[i].y/=nl; s.nrm[i].z/=nl; }
                }
            } else {
                // 镜像必须在部件空间完成，再一起跟随骨骼动画：
                // 前半块 = M*(-x)（原半脸在 -x，绕序需反），后半块 = M*(x)（+x 侧）
                s.pos.resize(m.positions.size() * 2);
                for (size_t i = 0; i < m.positions.size(); i++) {
                    brf::Vec3 q = m.positions[i]; q.x = -q.x; skin::xformPos(M, q); s.pos[i] = q;
                    q = m.positions[i]; skin::xformPos(M, q); s.pos[m.positions.size() + i] = q;
                }
                s.nrm.resize(m.verts.size() * 2);
                for (size_t i = 0; i < m.verts.size(); i++) {
                    brf::Vec3 n = m.verts[i].normal; n.x = -n.x; skin::xformDir(M, n);
                    float nl = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);
                    if (nl > 1e-6f) { n.x/=nl; n.y/=nl; n.z/=nl; }
                    s.nrm[i] = n;
                    n = m.verts[i].normal; skin::xformDir(M, n);
                    nl = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);
                    if (nl > 1e-6f) { n.x/=nl; n.y/=nl; n.z/=nl; }
                    s.nrm[m.verts.size() + i] = n;
                }
            }
        }
        for (auto& p : s.pos) minY = std::min(minY, p.y);
    }
    for (size_t pi = 0; pi < parts.size(); pi++) {
        const brf::Mesh& m = *parts[pi].mesh;
        Staged& s = st[pi];
        std::vector<float>& V = out[pi];
        V.clear();
        size_t nHalf = parts[pi].mirror ? 2u : 1u;
        V.reserve(m.verts.size() * 11 * nHalf);
        for (size_t h = 0; h < nHalf; h++) {
            for (size_t i = 0; i < m.verts.size(); i++) {
                int idx = m.verts[i].index;
                if (idx < 0 || (size_t)idx >= m.positions.size()) continue;
                brf::Vec3& p = s.pos[h * m.positions.size() + idx];
                brf::Vec3 n = (h * m.verts.size() + i < s.nrm.size()) ? s.nrm[h * m.verts.size() + i]
                                                                      : brf::Vec3{0.f, 0.f, 1.f};
                V.push_back(-p.x); V.push_back(p.z); V.push_back(p.y - minY); // x 翻负：渲染输出与蒙皮坐标反号（女王 W/S 基准实测锁定）
                uint32_t c = m.verts[i].color;
                V.push_back(((c)&0xFF)/255.f); V.push_back(((c>>8)&0xFF)/255.f); V.push_back(((c>>16)&0xFF)/255.f);
                V.push_back(-n.x); V.push_back(n.z); V.push_back(n.y);
                V.push_back(m.verts[i].uv.u); V.push_back(m.verts[i].uv.v);
            }
        }
    }
}

static void makeBoxCharObj(App& A) {
    std::vector<float> V; std::vector<uint32_t> I;
    boxPart(V,I, -0.16f,0.16f, -0.12f,0.12f, 0.0f,0.72f, 0.30f,0.22f,0.12f); // 左腿? 用一对
    boxPart(V,I, -0.16f,0.16f, -0.12f,0.12f, 0.0f,0.72f, 0.30f,0.22f,0.12f);
    boxPart(V,I, -0.38f,0.38f, -0.18f,0.18f, 0.66f,1.30f, 0.20f,0.34f,0.55f); // 躯干
    boxPart(V,I, -0.24f,0.24f, -0.20f,0.20f, 1.28f,1.66f, 0.86f,0.68f,0.50f); // 头
    boxPart(V,I, -0.52f,-0.24f, -0.12f,0.12f, 0.95f,1.32f, 0.20f,0.34f,0.55f); // 左臂
    boxPart(V,I,  0.24f,0.52f, -0.12f,0.12f, 0.95f,1.32f, 0.20f,0.34f,0.55f); // 右臂
    App::Obj obj;
    obj.diffName = "snow"; // 白色贴图兜底，角色主要靠顶点色
    auto ti = A.texIndex.find(obj.diffName);
    obj.texSlot = (ti != A.texIndex.end()) ? ti->second : 0;
    obj.descSet = A.texSets.empty() ? VK_NULL_HANDLE : A.texSets[obj.texSlot];
    obj.model.assign(16, 0.f);
    makeBuffer(A, V.size()*4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, obj.vb, obj.vm);
    upload(A, obj.vb, obj.vm, V.data(), V.size()*4);
    obj.idxCount = (uint32_t)I.size();
    makeBuffer(A, obj.idxCount*4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, obj.ib, obj.im);
    upload(A, obj.ib, obj.im, I.data(), obj.idxCount*4);
    A.objs.push_back(std::move(obj));
    std::fprintf(stderr, "占位角色 顶点=%zu 三角形=%zu\n", V.size()/11, I.size()/3);
}
static bool charTposeDebug() { static bool v = getenv("VK_TPOSE") != nullptr; return v; }
static void makeCharObj(App& A, std::set<std::string>& needDds, int group=0,
                         float tintR=1.f, float tintG=1.f, float tintB=1.f) {
    if (!g_charSkin.ok) {
        makeBoxCharObj(A);
        if (!A.objs.empty()) { A.objs.back().charGroup = group; A.objs.back().tintR=tintR; A.objs.back().tintG=tintG; A.objs.back().tintB=tintB; }
        return;
    }
    const std::vector<CharSkin::Part>& parts = (group > 0 && !g_enemyParts.empty()) ? g_enemyParts : g_charSkin.parts;
    std::vector<std::vector<float>> V;
    buildAllCharVerts(parts, V, charTposeDebug()); // 正常走蒙皮；VK_TPOSE=1 调试时用 T-pose
    if (tintR != 1.f || tintG != 1.f || tintB != 1.f) { // 敌方部队顶点色染色，便于区分阵营
        for (auto& part : V)
            for (size_t i = 3; i + 7 < part.size(); i += 11) {
                part[i] *= tintR; part[i+1] *= tintG; part[i+2] *= tintB;
            }

    }
    float mnx=1e9f,mny=1e9f,mnz=1e9f,mxx=-1e9f,mxy=-1e9f,mxz=-1e9f;
    for (size_t pi = 0; pi < parts.size(); pi++) {
        const brf::Mesh& mesh = *parts[pi].mesh;
        App::Obj obj;
        obj.diffName = parts[pi].diff;
        needDds.insert(obj.diffName);
        obj.charAnim = true;
        obj.charPart = (int)pi;
        obj.charGroup = group;
        obj.tintR = tintR; obj.tintG = tintG; obj.tintB = tintB;
        obj.model.assign(16, 0.f);
        makeBuffer(A, V[pi].size()*4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, obj.vb, obj.vm);
        upload(A, obj.vb, obj.vm, V[pi].data(), V[pi].size()*4);
        std::vector<uint32_t> idxs;
        if (parts[pi].mirror) { // H(det=-1) 整体翻转绕序：base 半脸(-x)用原绕序，镜像半脸(+x)逆序
            uint32_t base = (uint32_t)(V[pi].size() / 22); // 原半边顶点数
            idxs.reserve(mesh.indices.size() * 2);
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                idxs.push_back(mesh.indices[i]);
                idxs.push_back(mesh.indices[i+1]);
                idxs.push_back(mesh.indices[i+2]);
                idxs.push_back(base + mesh.indices[i]);
                idxs.push_back(base + mesh.indices[i+2]);
                idxs.push_back(base + mesh.indices[i+1]);
            }
        } else {
            idxs = mesh.indices;
        }
        obj.idxCount = (uint32_t)idxs.size();
        makeBuffer(A, obj.idxCount*4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, obj.ib, obj.im);
        upload(A, obj.ib, obj.im, idxs.data(), obj.idxCount*4);
        A.objs.push_back(std::move(obj));
        for (size_t i = 0; i + 2 < V[pi].size(); i += 11) {
            mnx=std::min(mnx,V[pi][i]); mxx=std::max(mxx,V[pi][i]);
            mny=std::min(mny,V[pi][i+1]); mxy=std::max(mxy,V[pi][i+1]);
            mnz=std::min(mnz,V[pi][i+2]); mxz=std::max(mxz,V[pi][i+2]);
        }
    }
    std::fprintf(stderr, "角色部件上屏 部件=%zu bbox x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f]\n",
        parts.size(), mnx,mxx,mny,mxy,mnz,mxz);
}
static void updateCharAnim(App& A, float dt, bool moving) {
    if (!g_charSkin.ok) return;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool attacking = g_attackT >= 0.f && g_charSkin.attackAnim != nullptr;
    brf::SkeletonAnim* want = attacking ? g_charSkin.attackAnim
                            : (moving ? (shift ? g_charSkin.runAnim : g_charSkin.anim) : g_charSkin.standAnim);
    if (want != g_charSkin.curAnim) { g_charSkin.curAnim = want; g_charSkin.t = 0.f; }
    if (attacking) {
        g_attackT += dt * 30.f; // 30fps 采样（与原版动画帧率一致）
        if ((size_t)g_attackT >= g_charSkin.attackAnim->frames.size()) g_attackT = -1.f; // 播完回 idle
        g_charSkin.t = std::max(0.f, g_attackT);
        if (g_attackT >= 0.f) {
            const brf::AnimFrame& fr = g_charSkin.attackAnim->frames[(size_t)g_attackT % g_charSkin.attackAnim->frames.size()];
            g_mixFrame = fr;
            float k = std::min(1.f, g_attackT / (30.f * 0.18f)); // 起手 0.18s 从当前姿态混合到攻击姿态（防瞬移）
            if (k < 1.f) {
                size_t n = std::min(fr.rots.size(), g_attackBase.rots.size());
                for (size_t i = 0; i < n; i++) {
                    brf::Quat a = g_attackBase.rots[i], b = fr.rots[i];
                    float d = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
                    if (d < 0.f) { b.x=-b.x; b.y=-b.y; b.z=-b.z; b.w=-b.w; } // 半球一致
                    brf::Quat q;
                    q.x = a.x + (b.x - a.x)*k; q.y = a.y + (b.y - a.y)*k;
                    q.z = a.z + (b.z - a.z)*k; q.w = a.w + (b.w - a.w)*k;
                    float l = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
                    if (l > 1e-6f) { q.x/=l; q.y/=l; q.z/=l; q.w/=l; }
                    g_mixFrame.rots[i] = q;
                }
            }
        }
    } else if (moving) {
        float speed = shift ? 1.9f : 1.0f; // run 比 walk 快；stand 不播放（静止停 frame0）
        g_charSkin.t += dt * 30.f * speed;
    } else {
        g_charSkin.t = 0.f;
    }
    g_charSkin.frame = (size_t)g_charSkin.t % g_charSkin.curAnim->frames.size();
    std::vector<std::vector<float>> Vp, Ve;
    buildAllCharVerts(g_charSkin.parts, Vp, charTposeDebug(), nullptr); // 玩家部件顶点（武器握持 fix 已含刃朝向，攻击不另翻转）
    // 敌方永远站姿，不跟玩家动画走（此前共用 curAnim：玩家挥刀敌人也挥、玩家跑步敌人也跑，斧头握持乱的真凶）
    const brf::AnimFrame* enemyFr = (g_charSkin.standAnim && !g_charSkin.standAnim->frames.empty())
                                   ? &g_charSkin.standAnim->frames[0] : nullptr;
    buildAllCharVerts(g_enemyParts.empty() ? g_charSkin.parts : g_enemyParts, Ve, charTposeDebug(), enemyFr); // 敌方部件顶点
    for (auto& o : A.objs) {
        const std::vector<std::vector<float>>& V = (o.charGroup > 0) ? Ve : Vp;
        if (!o.charAnim || o.charPart < 0 || (size_t)o.charPart >= V.size()) continue;
        float tr = o.tintR, tg = o.tintG, tb = o.tintB;
        if (o.charGroup > 0 && (size_t)o.charGroup < g_hitFlash.size() && g_hitFlash[(size_t)o.charGroup] > 0.f) {
            tr = 1.f; tg = 0.12f; tb = 0.12f; // 受击闪红
        }
        if (tr == 1.f && tg == 1.f && tb == 1.f) {
            upload(A, o.vb, o.vm, V[o.charPart].data(), V[o.charPart].size()*4);
        } else {
            std::vector<float> tv = V[o.charPart]; // 敌方染色副本（每队各自独立缓冲）
            for (size_t i = 3; i + 7 < tv.size(); i += 11) {
                tv[i] *= tr; tv[i+1] *= tg; tv[i+2] *= tb;
            }
            upload(A, o.vb, o.vm, tv.data(), tv.size()*4);
        }
    }
}
static void setCharModel(App::Obj& o, const Char& ch) {
    float cy = cosf(ch.yaw), sy = sinf(ch.yaw);
    float h = terrainHeightAt(ch.x, ch.z);
    // 列主序 model：先绕高度轴转 yaw，再平移（顶点已含 180° 修正）
    float M[16] = { cy, sy, 0, 0,  -sy, cy, 0, 0,  0,0,1,0,  ch.x, ch.z, h, 1 };
    o.model.assign(M, M+16);
}
// 战斗模式：charGroup 决定用哪个角色的位姿（0=玩家，1..N=敌方）
static void setCharModelGrouped(App::Obj& o) {
    if (o.charGroup >= 0 && (size_t)o.charGroup < g_chars.size()) setCharModel(o, g_chars[(size_t)o.charGroup]);
    else setCharModel(o, g_char);
}

// 原版天空穹顶：skyboxes.brf 的单位球网格，放大到远平面内并跟随相机水平位置
static void makeSkyObj(App& A, const brf::Mesh& mesh, float cx, float cz, std::set<std::string>& needDds) {
    const float S = 380.f; // 网格半径~1.87，缩放后~710m < zf=800
    const float eye[3] = {cx, cz+85.f, 55.f};
    std::vector<float> verts; verts.reserve(mesh.verts.size()*11);
    for (auto& v : mesh.verts) {
        if (v.index < 0 || (size_t)v.index >= mesh.positions.size()) continue;
        auto& p = mesh.positions[v.index];
        // BRF -> 渲染坐标：x'=x, y'=z, z'=-y；球心=相机位置
        verts.push_back(eye[0] + p.x*S); verts.push_back(eye[1] + p.z*S); verts.push_back(eye[2] - p.y*S);
        float r = ((v.color) & 0xFF) / 255.0f;
        float g = ((v.color >> 8) & 0xFF) / 255.0f;
        float b = ((v.color >> 16) & 0xFF) / 255.0f;
        verts.push_back(r); verts.push_back(g); verts.push_back(b);
        float nl = sqrtf(v.normal.x*v.normal.x + v.normal.y*v.normal.y + v.normal.z*v.normal.z); if (nl<1e-6f) nl=1;
        verts.push_back(v.normal.x/nl); verts.push_back(v.normal.z/nl); verts.push_back(-v.normal.y/nl);
        verts.push_back(v.uv.u); verts.push_back(v.uv.v);
    }
    App::Obj obj;
    obj.sky = true;
    std::string diff = "skybox_clearday";
    auto dit = g_materialDiffuse.find(mesh.material);
    if (dit != g_materialDiffuse.end() && !dit->second.empty()) diff = dit->second;
    obj.diffName = diff;
    needDds.insert(diff);
    auto ti = A.texIndex.find(diff);
    obj.texSlot = (ti != A.texIndex.end()) ? ti->second : 0;
    obj.descSet = A.texSets.empty() ? VK_NULL_HANDLE : A.texSets[obj.texSlot];
    makeBuffer(A, verts.size()*4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, obj.vb, obj.vm);
    upload(A, obj.vb, obj.vm, verts.data(), verts.size()*4);
    obj.idxCount = (uint32_t)mesh.indices.size();
    makeBuffer(A, obj.idxCount*4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, obj.ib, obj.im);
    upload(A, obj.ib, obj.im, mesh.indices.data(), obj.idxCount*4);
    float eye0[3], tgt0[3]; camFrame(cx, cz, 55.f, 0.f, -0.32f, eye0, tgt0);
    computeSceneMVP(obj.pc, eye0, tgt0);
    A.objs.push_back(std::move(obj));
    std::fprintf(stderr, "天空穹顶 %s 顶点=%zu 三角形=%u 半径=%.0f\n", mesh.name.c_str(), verts.size()/11, obj.idxCount/3, S);
}

static void loadSceneAssets(App& A, const std::string& scoPath, const std::string& propsPath,
                            const std::string& commonRes, float& sceneCX, float& sceneCZ,
                            std::set<std::string>& needDds) {
    // 1. scene_props.txt：spr id -> mesh 名 / 碰撞体名
    std::map<uint32_t, std::string> sprMesh, sprBody;
    {
        std::ifstream pf(propsPath);
        std::string line; std::getline(pf, line); int n; pf >> n;
        uint32_t idx = 0;
        while (std::getline(pf, line)) {
            if (line.empty() || line.rfind("spr_",0)!=0) continue;
            std::istringstream ss(line);
            std::string idstr; int flags, x, y;
            std::string mesh, body;
            ss >> idstr >> flags >> x >> mesh >> body >> y;
            sprMesh[idx] = mesh;
            sprBody[idx] = body;
            idx++;
        }
    }
    // 2. SCO 物件
    sco::Scene scene = sco::load(scoPath);
    std::set<std::string> need, needBodies;
    for (auto& o : scene.objects) if (o.type==0) {
        auto it = sprMesh.find(o.id);
        if (it != sprMesh.end()) need.insert(it->second);
        auto bit = sprBody.find(o.id);
        if (bit != sprBody.end() && bit->second != "0") needBodies.insert(bit->second);
    }
    std::fprintf(stderr, "需要网格 %zu 个 碰撞体 %zu 个\n", need.size(), needBodies.size());
    // 大文件预过滤：只解析“文件里确实出现所需名字”的 brf（CV War 有大量高清材质大包，全扫会白屏几十秒）
    auto fileContainsAny = [](const std::filesystem::path& p, const std::set<std::string>& keys) {
        if (keys.empty()) return true;
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::string hay((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        for (auto& k : keys)
            if (k.size() >= 3 && hay.find(k) != std::string::npos) return true;
        return false;
    };
    // 2.5. 地面规格：游戏根 Data/ground_specs.txt（材质名稍后由 materials.brf 解析成 dds）
    std::filesystem::path gameRoot = std::filesystem::path(scoPath).parent_path().parent_path().parent_path().parent_path();
    std::vector<GroundSpec> gspecs = loadGroundSpecs((gameRoot / "Data" / "ground_specs.txt").string());
    // 3. 先扫 CommonRes；仍缺的网格/碰撞体再去模组 Resource 按需找
    //    （CV War Resource 有 16-36MB 高清材质大包，无差别全扫会白屏几十秒）
    std::filesystem::path moduleDir = std::filesystem::path(scoPath).parent_path().parent_path();
    std::vector<std::filesystem::path> resDirs = {
        std::filesystem::path(commonRes),
        moduleDir / "Resource",
    };
    std::set<std::string> missingMeshes = need, missingBodies = needBodies;
    int filesOk=0, filesFail=0;
    for (size_t di = 0; di < resDirs.size(); di++) {
        auto& resDir = resDirs[di];
        if (!std::filesystem::exists(resDir)) continue;
        bool isModuleRes = di == 1;
        if (di == 1) {
            bool groundMatsDone = true;
            for (auto& g : gspecs) if (!g_materialDiffuse.count(g.mat)) { groundMatsDone = false; break; }
            if (missingMeshes.empty() && missingBodies.empty() && groundMatsDone) {
                std::fprintf(stderr, "CommonRes 已满足全部网格/碰撞体/地面材质，跳过模块 Resource\n");
                break;
            }
            std::set<std::string> searchKeys = missingMeshes;
            searchKeys.insert(missingBodies.begin(), missingBodies.end());
            for (auto& de : std::filesystem::directory_iterator(resDir)) {
                if (de.path().extension() != ".brf") continue;
                if (!fileContainsAny(de.path(), searchKeys)) continue;
                std::fprintf(stderr, "按需解析模块 brf: %s\n", de.path().filename().string().c_str());
                try {
                    brf::BrfFile bf = brf::parse(de.path().string());
                    filesOk++;
                    for (auto& m : bf.meshes) {
                        if (meshIsLodVariant(m.name)) continue;
                        std::string base = meshBaseName(m.name);
                        if (need.count(base)) { g_meshGroups[base].push_back(std::move(m)); missingMeshes.erase(base); }
                    }
                    for (auto& mt : bf.materials) if (!g_materialDiffuse.count(mt.name)) g_materialDiffuse[mt.name] = mt.diffuseA;
                    for (auto& bd : bf.bodies) if (!g_bodies.count(bd.name)) { g_bodies[bd.name] = std::move(bd); missingBodies.erase(bd.name); }
                } catch (const std::exception&) { filesFail++; }
            }
            continue;
        }
        for (auto& de : std::filesystem::directory_iterator(resDir)) {
            if (de.path().extension() != ".brf") continue;
            try {
                brf::BrfFile bf = brf::parse(de.path().string());
                filesOk++;
                for (auto& m : bf.meshes) {
                    if (meshIsLodVariant(m.name)) continue;
                    std::string base = meshBaseName(m.name);
                    if (need.count(base)) { g_meshGroups[base].push_back(std::move(m)); missingMeshes.erase(base); }
                }
                for (auto& mt : bf.materials) if (!g_materialDiffuse.count(mt.name)) g_materialDiffuse[mt.name] = mt.diffuseA;
                for (auto& bd : bf.bodies) if (!g_bodies.count(bd.name)) { g_bodies[bd.name] = std::move(bd); missingBodies.erase(bd.name); }
            } catch (const std::exception&) { filesFail++; }
        }
    }
    std::fprintf(stderr, "扫描 brf：成功 %d 失败 %d，收集到基础网格 %zu / %zu，仍缺网格 %zu 碰撞体 %zu\n",
                 filesOk, filesFail, g_meshGroups.size(), need.size(), missingMeshes.size(), missingBodies.size());
    for (auto& g : gspecs) {
        auto it = g_materialDiffuse.find(g.mat);
        if (it != g_materialDiffuse.end()) g.diffuse = it->second;
        else std::fprintf(stderr, "地面材质未找到: %s\n", g.mat.c_str());
    }
    // 4. 场景中心：prop 包围盒与地形网格中心各占一半（terrain 网格 cell=2m，原点0,0）
    float mnx=1e9f, mnz=1e9f, mxx=-1e9f, mxz=-1e9f;
    for (auto& o : scene.objects) if (o.type==0) {
        mnx=std::min(mnx,o.pos[0]); mxx=std::max(mxx,o.pos[0]);
        mnz=std::min(mnz,o.pos[2]); mxz=std::max(mxz,o.pos[2]);
    }
    sco::Terrain terr;
    bool hasTerrain = sco::loadTerrain(scoPath, terr);
    sco::AiMesh aim;
    try { sco::loadAiMesh(scoPath, aim); } catch (const std::exception& e) { std::fprintf(stderr, "ai_mesh 解析失败: %s\n", e.what()); }
    g_aiMesh = std::move(aim);
    std::fprintf(stderr, "ai_mesh ok=%d verts=%zu edges=%zu faces=%zu\n", (int)g_aiMesh.ok, g_aiMesh.vertX.size(), g_aiMesh.edges.size(), g_aiMesh.faces.size());
    if (hasTerrain) g_terrain = std::move(terr);
    float tcx=0, tcz=0;
    if (hasTerrain) { tcx = g_terrain.width*2.0f/2; tcz = g_terrain.height*2.0f/2; }
    sceneCX=(mnx+mxx+tcx)/3; sceneCZ=(mnz+mxz+tcz)/3;
    std::fprintf(stderr, "中心: props=(%.1f,%.1f) terrain=(%.1f,%.1f) -> (%.1f,%.1f)\n",
                 (mnx+mxx)/2, (mnz+mxz)/2, tcx, tcz, sceneCX, sceneCZ);
    // 5. 生成物件
    size_t made=0;
    g_propColMap.clear();
    for (auto& o : scene.objects) {
        if (o.type!=0) continue;
        auto it = sprMesh.find(o.id);
        if (it==sprMesh.end()) continue;
        auto git = g_meshGroups.find(it->second);
        if (git==g_meshGroups.end()) {
            // 降级容错：缺网格用品红替代块渲染（坏资产可视化，不闪退、不隐身）
            std::fprintf(stderr, "缺网格 %s (spr %u)，用品红替代块\n", it->second.c_str(), o.id);
            static brf::Mesh missing = []() {
                brf::Mesh m;
                m.name = "@missing"; m.material = "none";
                const float c[8][3] = {{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                       {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
                for (auto& p : c) m.positions.push_back({p[0],p[1],p[2]});
                const int faces[12][3] = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{1,2,6},{1,6,5},
                                          {0,4,7},{0,7,3},{3,7,6},{3,6,2},{0,1,5},{0,5,4}};
                for (int fi = 0; fi < 12; fi++) {
                    for (int vi = 0; vi < 3; vi++) {
                        brf::Vertex vt;
                        vt.index = faces[fi][vi];
                        vt.color = 0xFFFF00FF; // AABBGGRR 品红
                        vt.normal = {0,0,1};
                        m.verts.push_back(vt);
                        m.indices.push_back((uint32_t)(fi*3+vi));
                    }
                }
                return m;
            }();
            uint32_t boxIdx = (uint32_t)(&o - scene.objects.data());
            makeSceneObj(A, missing, o, sceneCX, sceneCZ, boxIdx, "0");
            continue;
        }
        for (auto& piece : git->second) {
            auto dit2 = g_materialDiffuse.find(piece.material);
            if (dit2 != g_materialDiffuse.end() && !dit2->second.empty() && dit2->second != "none") needDds.insert(dit2->second);
            uint32_t boxIdx = (uint32_t)(&o - scene.objects.data());
            auto bit = sprBody.find(o.id);
            std::string bodyName = (bit != sprBody.end()) ? bit->second : "0";
            makeSceneObj(A, piece, o, sceneCX, sceneCZ, boxIdx, bodyName);
        }
        made++;
    }
    size_t colCount = 0;
    for (auto& kv : g_propColMap) colCount += kv.second.size();
    std::fprintf(stderr, "场景物件实例 %zu 个已建缓冲，prop 碰撞体 %zu 个（%zu 实例，bo 碰撞体 %zu 个）\n",
                 made, colCount, g_propColMap.size(), g_bodies.size());
    if (hasTerrain) {
        makeGroundObj(A, g_terrain, gspecs, sceneCX, sceneCZ, needDds);
    } else {
        std::fprintf(stderr, "无地形数据，跳过地面网格\n");
    }
    // 6. 天空穹顶（原版 skyboxes.brf 的 clearday 网格）
    try {
        brf::BrfFile sky = brf::parse((std::filesystem::path(commonRes) / "skyboxes.brf").string());
        for (auto& m : sky.meshes) if (m.name == "skybox_clearday") { makeSkyObj(A, m, sceneCX, sceneCZ, needDds); break; }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "天空加载失败（%s），跳过\n", e.what());
    }
}

static void allocCmds(App& A) {
    VK_DEV_FN(A.dev, vkAllocateCommandBuffers);
    VkCommandBufferAllocateInfo ca{}; ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool=A.cmdPool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=(uint32_t)A.images.size();
    A.cmds.resize(A.images.size());
    CHECK(vkAllocateCommandBuffers(A.dev, &ca, A.cmds.data()), "allocCmds");
}

static void updateAllMVP(App& A, const float eye[3], const float target[3]) {
    for (auto& o : A.objs) computeSceneMVP(o.pc, eye, target, o.model.empty()?nullptr:o.model.data());
}

static void recordCmdOne(App& A, size_t img) {
    VK_DEV_FN(A.dev, vkBeginCommandBuffer);
    VK_DEV_FN(A.dev, vkCmdBeginRenderPass); VK_DEV_FN(A.dev, vkCmdBindPipeline);
    VK_DEV_FN(A.dev, vkCmdBindDescriptorSets);
    VK_DEV_FN(A.dev, vkCmdPushConstants); VK_DEV_FN(A.dev, vkCmdBindVertexBuffers);
    VK_DEV_FN(A.dev, vkCmdBindIndexBuffer); VK_DEV_FN(A.dev, vkCmdDrawIndexed);
    VK_DEV_FN(A.dev, vkCmdEndRenderPass); VK_DEV_FN(A.dev, vkEndCommandBuffer);
    VkCommandBufferBeginInfo bb{}; bb.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(A.cmds[img], &bb);
    VkClearValue cv[2]{}; cv[0].color={{0.47f,0.64f,1.0f,1}}; cv[1].depthStencil={1.0f,0};
    VkRenderPassBeginInfo rp{}; rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass=A.rp; rp.framebuffer=A.fbs[img]; rp.renderArea={{0,0},A.extent};
    rp.clearValueCount=2; rp.pClearValues=cv;
    vkCmdBeginRenderPass(A.cmds[img], &rp, VK_SUBPASS_CONTENTS_INLINE);
    // 天空先画（无深度），随后场景正常深度覆盖
    vkCmdBindPipeline(A.cmds[img], VK_PIPELINE_BIND_POINT_GRAPHICS, A.pipeSky);
    for (auto& o : A.objs) {
        if (!o.sky) continue;
        vkCmdBindDescriptorSets(A.cmds[img], VK_PIPELINE_BIND_POINT_GRAPHICS, A.pipeLayout, 0, 1, &o.descSet, 0, nullptr);
        vkCmdPushConstants(A.cmds[img], A.pipeLayout, VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 160, o.pc.data());
        VkDeviceSize off=0;
        vkCmdBindVertexBuffers(A.cmds[img], 0, 1, &o.vb, &off);
        vkCmdBindIndexBuffer(A.cmds[img], o.ib, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(A.cmds[img], o.idxCount, 1, 0, 0, 0);
    }
    for (auto& o : A.objs) {
        if (o.sky) continue;
        if (o.charGroup > 0 && (size_t)o.charGroup < g_chars.size() && !g_chars[(size_t)o.charGroup].alive) continue; // 已击倒的敌方不画
        VkPipeline pipe = o.ground ? A.pipeGround : A.pipe;
        VkPipelineLayout layout = o.ground ? A.groundPipeLayout : A.pipeLayout;
        VkDescriptorSet desc = o.ground ? A.groundDescSet : o.descSet;
        vkCmdBindPipeline(A.cmds[img], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(A.cmds[img], VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &desc, 0, nullptr);
        vkCmdPushConstants(A.cmds[img], layout, VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 160, o.pc.data());
        VkDeviceSize off=0;
        vkCmdBindVertexBuffers(A.cmds[img], 0, 1, &o.vb, &off);
        vkCmdBindIndexBuffer(A.cmds[img], o.ib, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(A.cmds[img], o.idxCount, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(A.cmds[img]);
    vkEndCommandBuffer(A.cmds[img]);
}

static std::string sceneExeDir() {
    char buf[MAX_PATH]; GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    size_t pos = p.find_last_of("\\/");
    return pos == std::string::npos ? "." : p.substr(0, pos);
}
static void sceneLaunchAndExit(const std::string& exeName, const std::string& argline) {
    std::string exe = sceneExeDir() + "\\" + exeName;
    std::string cmd = "\"" + exe + "\" " + argline;
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, (LPSTR)cmd.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, sceneExeDir().c_str(), &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        std::exit(0);
    }
    std::fprintf(stderr, "启动失败: %s\n", cmd.c_str());
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0); setvbuf(stderr, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(crashFilter);
    if (argc < 5) { std::printf("usage: vk_scene <scene.sco> <scene_props.txt> <CommonRes目录> <Textures目录> [map.txt mapTextures fallbackTextures] | vk_scene --battle <同左...> <敌人数> [返回坐标x y]\n"); return 1; }
    // 战斗模式：--battle <sco> <scene_props> <CommonRes> <module Textures> <map.txt> <root Textures> <fallback> <敌人数> [x y]
    bool battle = (argc >= 2) && (std::string(argv[1]) == "--battle");
    int a0 = battle ? 2 : 1;
    if (argc < a0 + 4) { std::printf("参数不足\n"); return 1; }
    int enemyCount = 0;
    if (battle) enemyCount = (argc >= a0 + 8) ? atoi(argv[a0 + 7]) : 4;
    if (battle && argc >= a0 + 11) g_battleName = argv[a0 + 10]; // 遭遇的野队名（回地图结算用）
    std::string mapReturnArgs;
    if (battle) {
        if (argc >= a0 + 6) mapReturnArgs = "\"" + std::string(argv[a0+4]) + "\" \"" + std::string(argv[a0+5]) + "\"";
        if (argc >= a0 + 10) mapReturnArgs += " \"" + std::string(argv[a0+8]) + "\" \"" + std::string(argv[a0+9]) + "\"";
    } else {
        if (argc >= 7) mapReturnArgs = "\"" + std::string(argv[5]) + "\" \"" + std::string(argv[6]) + "\"";
        if (argc >= 10) mapReturnArgs += " \"" + std::string(argv[8]) + "\" \"" + std::string(argv[9]) + "\"";
    }
    std::fprintf(stderr, "A: 解析场景 %s%s\n", argv[a0], battle ? "（战斗模式）" : "");
    App A; A.hInst = GetModuleHandleA(nullptr);
    WNDCLASSA wc{}; wc.lpfnWndProc=wndProc; wc.hInstance=A.hInst; wc.lpszClassName="vkSceneWnd";
    RegisterClassA(&wc);
    A.hwnd = CreateWindowA("vkSceneWnd", battle ? "CV War 战斗场景自研渲染" : "M&B 原版场景自研渲染",
                           WS_OVERLAPPEDWINDOW, 200, 100, 1280, 720, nullptr, nullptr, A.hInst, nullptr);
    std::fprintf(stderr, "C: initVk\n");
    initVk(A);
    float sceneCX=0, sceneCZ=0;
    std::set<std::string> needDds;
    std::fprintf(stderr, "E: 装配场景物件\n");
    loadWeaponStats(argv[a0+1]); // 判定数值从模块 item_kinds 来（v0.7 第 4 步）
    loadSceneAssets(A, argv[a0], argv[a0+1], argv[a0+2], sceneCX, sceneCZ, needDds);
    loadCharSkin(argv[a0+2]);
    loadEnemyGear(argv[a0+1], argv[a0+2], g_battleName); // v0.7-3：敌方按野队兵种穿衣甲
    makeCharObj(A, needDds, 0); // 玩家（charGroup 0）
    if (battle && enemyCount > 0) {
        g_chars.resize((size_t)enemyCount + 1);
        for (int e = 0; e < enemyCount; e++) {
            float r = 0.25f + 0.04f * e; // 敌方偏红染色，方便区分阵营
            makeCharObj(A, needDds, e + 1, 0.95f, 0.28f, 0.28f); // 敌方染色：暗红（衣甲素色贴图 × 染色 = 敌我一眼区分）
        }
    }
    if (getenv("VK_CHAR_ONLY")) { // 调试：只画角色，便于检查蒙皮姿态
        A.objs.erase(std::remove_if(A.objs.begin(), A.objs.end(),
            [](const App::Obj& o) { return !o.charAnim; }), A.objs.end());
    }
    std::vector<std::string> ddsNames(needDds.begin(), needDds.end());
    if (ddsNames.empty()) ddsNames.push_back("shields");
    std::fprintf(stderr, "C2: 加载 %zu 张贴图\n", ddsNames.size());
    initTextures(A, argv[a0+3], ddsNames, argc >= a0 + 7 ? argv[a0+6] : "");
    std::fprintf(stderr, "D: initPipeline\n");
    initPipeline(A);
    for (auto& o : A.objs) {
        if (o.ground) { o.descSet = A.groundDescSet; continue; }
        auto ti = A.texIndex.find(o.diffName);
        o.texSlot = (ti != A.texIndex.end()) ? ti->second : 0;
        o.descSet = A.texSets[o.texSlot];

    }
    std::fprintf(stderr, "F: allocCmds\n");
    allocCmds(A);
    safeSpawn(sceneCX, sceneCZ);
    if (battle) {
        g_chars[0] = g_char; // 玩家
        g_chars[0].hp = g_battleFast() ? 999.f : (float)g_playerHp; // v0.7-6：占位（无模块兵种数据），值见 g_playerHp
        for (size_t e = 1; e < g_chars.size(); e++) {
            float offX = -10.f + (float)(e - 1) * 4.f;              // 横向排开一列，正对镜头，避开与玩家重叠
            float offZ = 0.f;
            spawnNear(g_chars[e], g_char.x + offX, g_char.z + offZ);
            float dx = g_char.x - g_chars[e].x, dz = g_char.z - g_chars[e].z;
            g_chars[e].yaw = atan2f(-dx, dz); // 面向玩家（引擎朝向约定）
            g_chars[e].hp = g_battleFast() ? 1.f : g_enemyHp; // 35+2*(str+ironflesh)，读本兵种 troops
            g_chars[e].atkCd = 1.5f + (float)(e % 3) * 0.5f;
        }
        g_hitFlash.assign(g_chars.size(), 0.f);
        std::fprintf(stderr, "战斗：玩家 1 + 敌方 %d，场景布局用模组作者自己的%s\n", enemyCount, g_battleFast() ? "（速测模式）" : "");
    }
    std::fprintf(stderr, "出生点 (%.1f,%.1f)（原中心 %.1f,%.1f）\n", g_char.x, g_char.z, sceneCX, sceneCZ);
    for (auto& o : A.objs) {
        if (!o.model.empty()) setCharModelGrouped(o);
        if (o.ground) continue;
        auto ti = A.texIndex.find(o.diffName);
        o.texSlot = (ti != A.texIndex.end()) ? ti->second : 0;
        o.descSet = A.texSets[o.texSlot];

    }
    Cam cam; // 第三人称相机：骑砍标准 = 角色背后（cam.yaw 与角色 yaw 同相，eye 偏移 = 前向量反向）
    cam.yaw = g_char.yaw + 3.14159265f; // 初始转到角色正后方；之前 yaw=0 相机在角色正前方，画面左右与角色左右镜面（一切“左右反”的根因）
    cam.dist = 10.f; cam.height = 8.f; // 初始拉高一点，避免被近处建筑挡住角色
    float eye[3], tgt[3];
    playerFrame(g_char, cam, eye, tgt);
    updateAllMVP(A, eye, tgt);
    for (size_t i=0;i<A.cmds.size();i++) recordCmdOne(A, i);

    VK_DEV_FN(A.dev, vkAcquireNextImageKHR); VK_DEV_FN(A.dev, vkQueueSubmit); VK_DEV_FN(A.dev, vkQueuePresentKHR);
    VK_DEV_FN(A.dev, vkCreateFence); VK_DEV_FN(A.dev, vkWaitForFences); VK_DEV_FN(A.dev, vkResetFences); VK_DEV_FN(A.dev, vkResetCommandBuffer);
    A.fences.resize(A.images.size());
    A.acqFences.resize(A.images.size());
    for (size_t i=0;i<A.images.size();i++) {
        VkFenceCreateInfo fi{}; fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(A.dev, &fi, nullptr, &A.fences[i]);
        VkFenceCreateInfo af{}; af.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; // 初始 unsignaled，交给 acquire 使用
        vkCreateFence(A.dev, &af, nullptr, &A.acqFences[i]);
    }
    ShowWindow(A.hwnd, SW_SHOW); UpdateWindow(A.hwnd); SetForegroundWindow(A.hwnd);
    std::fprintf(stderr, "G: 进入主循环（WASD 走角色，方向键绕角色转头，QE 拉近拉远，Shift 跑，ESC 退出）\n");
    MSG msg;
    uint32_t frame = 0;
    ULONGLONG last = GetTickCount64();
    while (true) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        ULONGLONG now = GetTickCount64();
        float dt = std::min(0.1f, (float)(now - last)/1000.f);
        last = now;
        size_t fi = frame % A.images.size();
        uint32_t img=0;
        VkResult ar = vkAcquireNextImageKHR(A.dev, A.swap, UINT64_MAX, VK_NULL_HANDLE, A.acqFences[fi], &img);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            // 出错（如 out-of-date）：复位本次 acquire fence，避免下轮死等；窗口无 resize 时很少走到这里
            vkResetFences(A.dev, 1, &A.acqFences[fi]);
            Sleep(10);
            continue;
        }
        vkWaitForFences(A.dev, 1, &A.acqFences[fi], VK_TRUE, UINT64_MAX);
        vkResetFences(A.dev, 1, &A.acqFences[fi]);
        // 等该 image 上一次的提交完成，再安全重录
        vkWaitForFences(A.dev, 1, &A.fences[img], VK_TRUE, UINT64_MAX);
        vkResetFences(A.dev, 1, &A.fences[img]);
        bool moving = updatePlayer(cam, dt);
        if (battle && !g_chars.empty()) {
            g_chars[0].x = g_char.x; g_chars[0].z = g_char.z; g_chars[0].yaw = g_char.yaw;
            if (g_atkCd > 0.f) g_atkCd -= dt;
            for (size_t e = 1; e < g_chars.size(); e++) {
                Char& c = g_chars[e];
                if (!c.alive) continue;
                float dx = g_char.x - c.x, dz = g_char.z - c.z;
                float dist = sqrtf(dx*dx + dz*dz);
                c.yaw = atan2f(-dx, dz);
                if (dist > 1.2f) {          // 压上玩家
                    float step = std::min(1.1f*dt, dist - 1.2f);
                    moveCharBlockedC(c, c.x + dx/dist*step, c.z + dz/dist*step);
                } else if (g_endTimer < 0.f && c.atkCd <= 0.f) { // 近身攻击玩家（战斗结束即停手）
                    c.atkCd = 2.0f;
                    g_chars[0].hp -= g_enemySwing; // 反击伤害 = 敌兵种武器 swing 原值（item_kinds 数据）
                    std::fprintf(stderr, "[被击] -%g 玩家 HP %.0f\n", g_enemySwing, g_chars[0].hp);
                }
                if (c.atkCd > 0.f) c.atkCd -= dt;
                if (g_hitFlash[e] > 0.f) g_hitFlash[e] -= dt;
            }
            // 玩家攻击：鼠标左键 → 面前 2.2 米、60° 扇区内的最近敌人
            if (g_click.x >= 0) {
                g_click.x = -1; g_click.y = -1;
                if (g_atkCd <= 0.f) {
                    // 攻击方向 = 最后移动方向（0 右挥 1 左挥 2 上劈 3 刺；静止默认右挥）
                    if (!g_attackMirrors[g_lastMoveDir].frames.empty())
                        g_charSkin.attackAnim = &g_attackMirrors[g_lastMoveDir];
                    else g_charSkin.attackAnim = &g_attackMirrors[0];
                    g_attackDir = g_lastMoveDir; // 锁存：攻击中走位不改变本次攻击方向
                    static const char* kDirName[4] = {"righttoleft", "lefttoright", "overswing", "thrust"};
                    std::fprintf(stderr, "[挥击] 方向=%s\n", kDirName[g_lastMoveDir]);
                    // 冷却 = 攻击动画时长 / (speed_rating/100)，与 CV War 攻速数据一致
                    float clipLen = (float)g_charSkin.attackAnim->frames.size() / 30.f;
                    g_atkCd = clipLen / std::max(20.f, g_wpnSpeed) * 100.f;
                    g_attackT = 0.f; // 挥刀动画（命中/挥空都挥，与骑砍手感一致）
                    g_attackBase = g_charSkin.curAnim->frames[g_charSkin.frame % g_charSkin.curAnim->frames.size()];
                    float fx = -sinf(g_char.yaw), fz = cosf(g_char.yaw);
                    int best = -1; float bestA = 1e9f;
                    for (size_t e = 1; e < g_chars.size(); e++) {
                        if (!g_chars[e].alive) continue;
                        float ex = g_chars[e].x - g_char.x, ez = g_chars[e].z - g_char.z;
                        float d = sqrtf(ex*ex + ez*ez);
                        if (d > g_wpnLen / 100.f + 1.0f) continue; // 攻击距离 = 武器长度(m) + 臂展 1m，来自 item 数据
                        float dot = (ex*fx + ez*fz) / d;
                        if (dot < -0.2f) continue; // 约 101° 扇区，两侧近身的也能砍到
                        float ang = acosf(std::min(1.f, std::max(-1.f, dot)));
                        if (ang < bestA) { bestA = ang; best = (int)e; }
                    }
                    if (best > 0) {
                        g_chars[(size_t)best].hp -= (float)g_wpnSwing; // 伤害 = 玩家武器 swing 原值（item_kinds 数据）
                        g_hitFlash[(size_t)best] = 0.25f;
                        std::fprintf(stderr, "[击中] %s 组%d -%d 剩 HP %.0f\n", g_battleName.c_str(), best, g_wpnSwing, g_chars[(size_t)best].hp);
                        if (g_chars[(size_t)best].hp <= 0.f) {
                            g_chars[(size_t)best].alive = false;
                            std::fprintf(stderr, "[击倒] %s 组%d\n", g_battleName.c_str(), best);
                        }
                    } else {
                        std::fprintf(stderr, "[挥空]\n");
                    }
                }
            }
            // 结束判定 → 1.6 秒后回大地图结算
            if (g_endTimer < 0.f) {
                bool allDead = true;
                for (size_t e = 1; e < g_chars.size(); e++) if (g_chars[e].alive) { allDead = false; break; }
                if (g_chars[0].hp <= 0.f) { g_endResult = "lose"; g_endTimer = 1.6f; std::fprintf(stderr, "[战败] 返回大地图\n"); }
                else if (allDead)        { g_endResult = "win";  g_endTimer = 1.6f; std::fprintf(stderr, "[胜利] 返回大地图\n"); }
            } else {
                g_endTimer -= dt;
                if (g_endTimer <= 0.f && !mapReturnArgs.empty()) {
                    sceneLaunchAndExit("vk_map.exe", mapReturnArgs + " \"" + g_endResult + "\" \"" + g_battleName + "\"");
                }
            }
        }
        if (!mapReturnArgs.empty() && (GetAsyncKeyState('M') & 0x8000)) {
            std::fprintf(stderr, "M 返回大地图\n");
            sceneLaunchAndExit("vk_map.exe", mapReturnArgs);
        }
        if (battle) {
            static int lastAlive = -1; static float lastHp = -1.f; static std::string lastRes;
            int alive = 0;
            for (size_t e = 1; e < g_chars.size(); e++) if (g_chars[e].alive) alive++;
            if (alive != lastAlive || g_chars[0].hp != lastHp || g_endResult != lastRes) {
                char t[200];
                std::snprintf(t, sizeof(t), "CV War Battle | HP %.0f | Enemies %d%s | LMB=attack",
                              g_chars[0].hp, alive,
                              g_endResult.empty() ? "" : (g_endResult == "win" ? " | VICTORY" : " | DEFEAT"));
                SetWindowTextA(A.hwnd, t);
                lastAlive = alive; lastHp = g_chars[0].hp; lastRes = g_endResult;
            }
        } else if (!g_lastHit.empty()) {
            std::string title = "M&B 原生场景渲染 | 撞到: " + g_lastHit;
            SetWindowTextA(A.hwnd, title.c_str());
        }
        updateCharAnim(A, dt, moving);
        for (auto& o : A.objs) if (!o.model.empty()) setCharModelGrouped(o);
        playerFrame(g_char, cam, eye, tgt);
        updateAllMVP(A, eye, tgt);
        vkResetCommandBuffer(A.cmds[img], 0);
        recordCmdOne(A, img);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{}; si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount=1; si.pCommandBuffers=&A.cmds[img]; si.waitSemaphoreCount=0; si.pWaitDstStageMask=&ws;
        vkQueueSubmit(A.queue, 1, &si, A.fences[img]);
        VkPresentInfoKHR pi{}; pi.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.swapchainCount=1; pi.pSwapchains=&A.swap; pi.pImageIndices=&img;
        vkQueuePresentKHR(A.queue, &pi);
        frame++;
        Sleep(1);
    }
}
