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
    struct Obj { VkBuffer vb=VK_NULL_HANDLE, ib=VK_NULL_HANDLE; VkDeviceMemory vm=VK_NULL_HANDLE, im=VK_NULL_HANDLE; uint32_t idxCount=0; std::vector<float> pc; std::vector<float> model; int texSlot=0; VkDescriptorSet descSet=VK_NULL_HANDLE; std::string diffName="shields"; bool sky=false; bool charAnim=false; bool ground=false; int charPart=-1; };
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
    pc.resize(24, 0.f);
    if (model16) {
        std::vector<float> VM;
        matMul44(V.data(), model16, VM);
        matMul44(P.data(), VM.data(), pc);
    } else {
        matMul44(P.data(), V.data(), pc); // pc[0..15] = mvp
    }
    pc[16] = eye[0]; pc[17] = eye[1]; pc[18] = eye[2];
    pc[20] = 70.f; pc[21] = 300.f;    // 雾起点/终点（世界距离）
}

static void setTitleUtf8(HWND h, const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return;
    std::vector<wchar_t> w((size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    SetWindowTextW(h, w.data());
}
static POINT g_click{-1,-1};
static bool g_paused = false; // 空格暂停/继续游戏时间
static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_CLOSE || (msg == WM_KEYDOWN && w == VK_ESCAPE)) PostQuitMessage(0);
    if (msg == WM_KEYDOWN && w == VK_SPACE) g_paused = !g_paused;
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

static void initTextures(App& A, const std::string& ddsDir, const std::vector<std::string>& names) {
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
        dds::Image img;
        if (n == "white") {
            // 程序化 4x4 纯白贴图：徽标/标记用，颜色完全由顶点色决定
            img.width = 4; img.height = 4;
            img.rgba.assign(4*4*4, 255);
        } else {
            std::string path = ddsDir + "\\" + n + ".dds";
            try { img = dds::load(path); }
            catch (const std::exception& e) { std::fprintf(stderr, "贴图缺失 %s（%s），跳过\n", path.c_str(), e.what()); continue; }
        }
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

// ================= vk_map：原版大地图（campaign map）渲染 =================
#include "map.h"
#include <queue>
#include <unordered_map>

// ---------- CV War 地图数据：map_icons / parties / party_templates ----------
// 字段序来自 Taleworlds 官方 Module System 的 process 脚本：
//   map_icons：  name flags mesh scale [menu/trigger 续行]
//   parties：    1 <i> <i> p_<id> <name> <flags> <menu> <template> <faction>
//                <personality> <personality> <ai_bhvr> <ai_target> <ai_target>
//                <默认x> <默认y> <默认x> <默认y> <x> <y> 0.0 <stacks...>
//   图标 = flags & 0xff（pf_icon_mask），坐标 = 第 18/19 个 token
struct IconDef { std::string mesh; float scale=0.35f; };

static std::vector<IconDef> loadMapIcons(const std::string& moduleDir) {
    std::vector<IconDef> out;
    std::ifstream f(moduleDir + "\\map_icons.txt");
    std::string line; std::getline(f, line); // map_icons_file version 1
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        char c = line[0];
        // 入口行以字母/下划线开头；-70 开头的触发器续行直接跳过
        if (!(c=='_' || (c>='a'&&c<='z') || (c>='A'&&c<='Z'))) continue;
        std::istringstream ss(line);
        IconDef ic; std::string name; int flags;
        if (!(ss >> name >> flags >> ic.mesh)) continue;
        ss >> ic.scale;
        out.push_back(ic);
    }
    return out;
}

struct PartyRec {
    std::string id, name;
    uint64_t flags = 0;
    int icon = 0, templateNo = 0, faction = 0, personality = 0, aiBehavior = 0;
    float x = 0, y = 0;
};
static std::vector<PartyRec> loadParties(const std::string& moduleDir) {
    std::vector<PartyRec> out;
    std::ifstream f(moduleDir + "\\parties.txt");
    std::string line; std::getline(f, line); // partiesfile version 1
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::vector<std::string> tok; std::string t;
        while (ss >> t) tok.push_back(t);
        if (tok.size() < 20) continue;
        PartyRec p;
        p.id = tok[3]; p.name = tok[4];
        p.flags = strtoull(tok[5].c_str(), nullptr, 10);
        p.icon = (int)(p.flags & 0xff);            // pf_icon_mask
        p.templateNo = atoi(tok[7].c_str());
        p.faction = atoi(tok[8].c_str());
        p.personality = atoi(tok[9].c_str());
        p.aiBehavior = atoi(tok[11].c_str());
        p.x = (float)atof(tok[18].c_str());        // 真实坐标是第 18/19 个 token
        p.y = (float)atof(tok[19].c_str());
        out.push_back(std::move(p));
    }
    return out;
}

struct TemplateDef { std::string id, name; int icon=0; };
static std::vector<TemplateDef> loadPartyTemplates(const std::string& moduleDir) {
    std::vector<TemplateDef> out;
    std::ifstream f(moduleDir + "\\party_templates.txt");
    std::string line; std::getline(f, line); // partytemplatesfile version 1
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        TemplateDef t; uint64_t flags = 0;
        if (!(ss >> t.id >> t.name >> flags)) continue;
        t.icon = (int)(flags & 0xff);              // 模板图标同样在 flags 低 8 位
        out.push_back(std::move(t));
    }
    return out;
}

// 定居点 = parties.txt 里的 town/castle/village；场景名用作者自己的 CV War 布局
struct Settlement {
    std::string id, name, scene;
    int icon = 0;
    float x = 0, y = 0;
    size_t iconIdx = 0;
};
static std::vector<Settlement> loadSettlements(const std::vector<PartyRec>& parties) {
    std::vector<Settlement> out;
    for (const auto& p : parties) {
        std::string scene;
        if (p.id.rfind("p_town_", 0) == 0) {
            scene = "scn_town_" + p.id.substr(7) + "_center";
        } else if (p.id.rfind("p_castle_", 0) == 0) {
            scene = "scn_castle_" + p.id.substr(9) + "_exterior";
        } else if (p.id.rfind("p_village_", 0) == 0) {
            scene = "scn_village_" + p.id.substr(10);
        } else continue;
        Settlement s;
        s.id = p.id; s.name = p.name; s.scene = scene;
        s.icon = p.icon; s.x = p.x; s.y = p.y;
        out.push_back(std::move(s));
    }
    return out;
}

static std::string exeDir() {
    char buf[MAX_PATH]; GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    size_t pos = p.find_last_of("\\/");
    return pos == std::string::npos ? "." : p.substr(0, pos);
}
static void launchAndExit(const std::string& exeName, const std::string& argline) {
    std::string exe = exeDir() + "\\" + exeName;
    std::string cmd = "\"" + exe + "\" " + argline;
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, (LPSTR)cmd.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, exeDir().c_str(), &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        std::exit(0);
    }
    std::fprintf(stderr, "启动失败: %s\n", cmd.c_str());
}

struct MapObj {
    VkBuffer vb=VK_NULL_HANDLE, ib=VK_NULL_HANDLE;
    VkDeviceMemory vm=VK_NULL_HANDLE, im=VK_NULL_HANDLE;
    uint32_t idxCount=0;
    VkDescriptorSet descSet=VK_NULL_HANDLE;
    std::vector<float> pc;   // mvp 16 floats
    std::vector<float> model; // 可选 16 floats（标记物）
    float iconBaseScale=1.f, iconDim=1.f; // 图标基础缩放与网格最大水平尺寸（动态最小屏占）
    bool isIcon=false;       // 调试 VK_MAP_ICONS_ONLY 时只画图标
};

static void initMapPipeline(App& A) {
    VK_DEV_FN(A.dev, vkCreateShaderModule); VK_DEV_FN(A.dev, vkCreatePipelineLayout);
    VK_DEV_FN(A.dev, vkCreateGraphicsPipelines); VK_DEV_FN(A.dev, vkDestroyShaderModule);
    VK_DEV_FN(A.dev, vkCreateDescriptorSetLayout); VK_DEV_FN(A.dev, vkCreateDescriptorPool);
    VK_DEV_FN(A.dev, vkAllocateDescriptorSets); VK_DEV_FN(A.dev, vkUpdateDescriptorSets);
    auto makeShader = [&](const char* path) {
        auto code = readFile(path);
        VkShaderModuleCreateInfo sm{}; sm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sm.codeSize=code.size(); sm.pCode=(const uint32_t*)code.data();
        VkShaderModule m; CHECK(vkCreateShaderModule(A.dev, &sm, nullptr, &m), "shaderModule");
        return m;
    };
    VkShaderModule vs = makeShader("mesh.vert.spv"), fs = makeShader("mesh.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vs; stages[0].pName="main";
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fs; stages[1].pName="main";

    VkVertexInputBindingDescription bind{}; bind.binding=0; bind.stride=44; bind.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location=0; attrs[0].binding=0; attrs[0].format=VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset=0;
    attrs[1].location=1; attrs[1].binding=0; attrs[1].format=VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset=12;
    attrs[2].location=2; attrs[2].binding=0; attrs[2].format=VK_FORMAT_R32G32_SFLOAT; attrs[2].offset=36;
    VkPipelineVertexInputStateCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount=1; vi.pVertexBindingDescriptions=&bind;
    vi.vertexAttributeDescriptionCount=3; vi.pVertexAttributeDescriptions=attrs;

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
    VkPipelineColorBlendAttachmentState cb{}; cb.colorWriteMask=0xF; cb.blendEnable=VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount=1; cbs.pAttachments=&cb;
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};

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

    VkPipelineLayoutCreateInfo pl{}; pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount=1; pl.pSetLayouts=&A.descLayout;
    pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&pc;
    CHECK(vkCreatePipelineLayout(A.dev, &pl, nullptr, &A.pipeLayout), "pipeLayout");
    VkGraphicsPipelineCreateInfo gp{}; gp.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount=2; gp.pStages=stages; gp.pVertexInputState=&vi; gp.pInputAssemblyState=&ia;
    gp.pViewportState=&vsi; gp.pRasterizationState=&rs; gp.pMultisampleState=&ms; gp.pDepthStencilState=&ds;
    gp.pColorBlendState=&cbs; gp.layout=A.pipeLayout; gp.renderPass=A.rp; gp.subpass=0;
    CHECK(vkCreateGraphicsPipelines(A.dev, VK_NULL_HANDLE, 1, &gp, nullptr, &A.pipe), "mapPipeline");
    vkDestroyShaderModule(A.dev, vs, nullptr); vkDestroyShaderModule(A.dev, fs, nullptr);
}

static void buildMapObjs(App& A, const MapData& md, std::vector<MapObj>& objs) {
    for (size_t t=0;t<md.meshes.size();t++) {
        const auto& m = md.meshes[t];
        if (m.verts.empty() || m.idx.empty()) continue;
        MapObj o;
        makeBuffer(A, m.verts.size()*4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, o.vb, o.vm);
        upload(A, o.vb, o.vm, m.verts.data(), m.verts.size()*4);
        o.idxCount = (uint32_t)m.idx.size();
        makeBuffer(A, o.idxCount*4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, o.ib, o.im);
        upload(A, o.ib, o.im, m.idx.data(), o.idxCount*4);
        auto ti = A.texIndex.find(mapTexture((int)t).first);
        o.descSet = A.texSets[ti != A.texIndex.end() ? ti->second : 0];
        o.pc.resize(16, 0.f);
        objs.push_back(std::move(o));
        std::fprintf(stderr, "地形[%d] %s 三角=%u\n", (int)t, mapTexture((int)t).first.c_str(), o.idxCount/3);
    }
}

// 地图图标：把 moe_map_icons.brf 的网格平铺在地图平面上（x'=x*s, y'=z*s）
static bool loadIconMeshes(const std::string& moduleDir, std::map<std::string, brf::Mesh>& meshes) {
    try {
        brf::BrfFile bf = brf::parse(moduleDir + "\\Resource\\moe_map_icons.brf");
        for (auto& m : bf.meshes) meshes[m.name] = std::move(m);
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "图标 brf 解析失败: %s\n", e.what());
        return false;
    }
}
static size_t makeIconObj(App& A, const std::map<std::string, brf::Mesh>& meshes,
                          VkDescriptorSet desc, const std::string& meshName, float scale,
                          float x, float y, float z, std::vector<MapObj>& objs) {
    auto it = meshes.find(meshName);
    if (it == meshes.end()) { std::fprintf(stderr, "缺图标网格 %s\n", meshName.c_str()); return SIZE_MAX; }
    auto& m = it->second;
    std::vector<float> verts; std::vector<uint32_t> idx;
    verts.reserve(m.verts.size()*11);
    float minx=1e30f, maxx=-1e30f, miny=1e30f, maxy=-1e30f;
    for (auto& v : m.verts) {
        if (v.index < 0 || (size_t)v.index >= m.positions.size()) continue;
        auto& p = m.positions[(size_t)v.index];
        minx=std::min(minx,p.x); maxx=std::max(maxx,p.x);
        miny=std::min(miny,p.z); maxy=std::max(maxy,p.z);
    }
    // 把网格质心挪到原点：动态放大时图标始终围绕“地图坐标点”长，不会越放越偏
    float cxm = (minx+maxx)*0.5f, cym = (miny+maxy)*0.5f;
    for (auto& v : m.verts) {
        if (v.index < 0 || (size_t)v.index >= m.positions.size()) continue;
        auto& p = m.positions[(size_t)v.index];
        verts.push_back((p.x-cxm)*scale); verts.push_back((p.z-cym)*scale); verts.push_back(z);
        verts.push_back(1.f); verts.push_back(1.f); verts.push_back(1.f);
        verts.push_back(0.f); verts.push_back(0.f); verts.push_back(1.f);
        verts.push_back(v.uv.u); verts.push_back(v.uv.v);
    }
    idx.assign(m.indices.begin(), m.indices.end()); // 顶点顺序一致，直接沿用网格索引
    MapObj o;
    makeBuffer(A, verts.size()*4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, o.vb, o.vm);
    upload(A, o.vb, o.vm, verts.data(), verts.size()*4);
    o.idxCount = (uint32_t)idx.size();
    makeBuffer(A, o.idxCount*4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, o.ib, o.im);
    upload(A, o.ib, o.im, idx.data(), o.idxCount*4);
    o.descSet = desc;
    o.pc.resize(16, 0.f);
    float M[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1};
    o.model.assign(M, M+16);
    o.iconBaseScale = scale;
    o.iconDim = std::max(maxx-minx, maxy-miny);
    o.isIcon = true;
    size_t idxOut = objs.size();
    objs.push_back(std::move(o));
    return idxOut;
}
// 图标动态缩放：保证屏幕上至少 minPix 像素（远了放大），近处维持作者原始尺寸
static void setIconModel(MapObj& o, float x, float y, float z,
                         float dist, float viewH, float minPix) {
    float base = o.iconBaseScale * o.iconDim;
    float need = minPix * 2.f * dist * tanf(0.45f) / std::max(1.f, viewH);
    float s = (base > 1e-6f) ? std::max(1.f, need / base) : 1.f;
    o.model[0] = s; o.model[5] = s; o.model[10] = 1.f; // 只放大水平，不抬高度（否则远视图像“升天”消失）
    o.model[12] = x; o.model[13] = y; o.model[14] = z;
}

static void updateMapMVP(std::vector<MapObj>& objs, const float eye[3], const float target[3], float aspect) {
    std::vector<float> V, P, mvp;
    lookAt(V, eye[0], eye[1], eye[2], target[0], target[1], target[2]);
    perspective(P, 0.9f, aspect, 5.0f, 4000.0f);
    for (auto& o : objs) {
        if (o.model.empty()) {
            matMul44(P.data(), V.data(), o.pc);
        } else {
            std::vector<float> VM;
            matMul44(V.data(), o.model.data(), VM);
            matMul44(P.data(), VM.data(), o.pc);
        }
    }
}

// ================= 大地图点击寻路 =================
static bool mapWalkable(int t) { return t != 0 && t != 8; } // 水与河不可走
static uint64_t mapEdgeKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return ((uint64_t)a << 32) | b;
}
static void mapFaceCenter(const MapData& md, int f, float& x, float& y) {
    x = y = 0.f;
    if (f < 0 || f >= (int)md.tris.size()) return;
    const auto& t = md.tris[(size_t)f];
    for (uint32_t v : t) { x += md.pos[v][0]; y += md.pos[v][1]; }
    x /= 3.f; y /= 3.f;
}
static int nearestWalkableFace(const MapData& md, float x, float y) {
    int best = -1; float bestD = 1e30f;
    for (size_t i = 0; i < md.tris.size(); i++) {
        if (!mapWalkable(md.faceTerrain[i])) continue;
        float cx, cy; mapFaceCenter(md, (int)i, cx, cy);
        float d = (cx-x)*(cx-x) + (cy-y)*(cy-y);
        if (d < bestD) { bestD = d; best = (int)i; }
    }
    return best;
}
static std::vector<int> mapBfsPath(const MapData& md, int from, int to) {
    std::vector<int> out;
    if (from < 0 || to < 0) return out;
    if (from == to) { out.push_back(from); return out; }
    size_t n = md.tris.size();
    std::unordered_map<uint64_t, std::vector<int>> edgeFaces;
    edgeFaces.reserve(n * 3);
    for (size_t i = 0; i < n; i++) {
        if (!mapWalkable(md.faceTerrain[i])) continue;
        const auto& t = md.tris[i];
        edgeFaces[mapEdgeKey(t[0], t[1])].push_back((int)i);
        edgeFaces[mapEdgeKey(t[1], t[2])].push_back((int)i);
        edgeFaces[mapEdgeKey(t[2], t[0])].push_back((int)i);
    }
    std::vector<int> prev(n, -2);
    std::queue<int> q;
    prev[(size_t)from] = -1; q.push(from);
    while (!q.empty()) {
        int u = q.front(); q.pop();
        if (u == to) break;
        const auto& t = md.tris[(size_t)u];
        for (int k = 0; k < 3; k++) {
            uint64_t key = mapEdgeKey(t[(size_t)k], t[(size_t)((k+1)%3)]);
            auto it = edgeFaces.find(key);
            if (it == edgeFaces.end()) continue;
            for (int nb : it->second) {
                if (nb == u || prev[(size_t)nb] != -2) continue;
                if (!mapWalkable(md.faceTerrain[(size_t)nb])) continue;
                prev[(size_t)nb] = u;
                q.push(nb);
            }
        }
    }
    if (prev[(size_t)to] == -2) return out;
    for (int u = to; u != -1; u = prev[(size_t)u]) out.push_back(u);
    std::reverse(out.begin(), out.end());
    return out;
}
static int rayPlaneHit(float ex,float ey,float ez, float tx,float ty,float tz,
                       float fovY, float aspect, float px, float py, float vpW, float vpH,
                       float& hx, float& hy) {
    float fx=tx-ex, fy=ty-ey, fz=tz-ez;
    float fl=sqrtf(fx*fx+fy*fy+fz*fz); if (fl<1e-6f) return 0;
    fx/=fl; fy/=fl; fz/=fl;
    // 相机 right = up × forward；上 = forward × right
    float rx=-fy, ry=fx, rz=0.f;            // up=(0,0,1) × (fx,fy,~)
    float rl=sqrtf(rx*rx+ry*ry); if (rl<1e-6f) return 0;
    rx/=rl; ry/=rl;
    float ux=fy*rz-fz*ry, uy=fz*rx-fx*rz, uz=fx*ry-fy*rx; // forward × right
    float ndcx = 2.f*px/vpW - 1.f;
    float ndcy = 1.f - 2.f*py/vpH;
    float th = tanf(fovY*0.5f);
    float dx=fx+rx*ndcx*th*aspect+ux*ndcy*th;
    float dy=fy+ry*ndcx*th*aspect+uy*ndcy*th;
    float dz=fz+rz*ndcx*th*aspect+uz*ndcy*th;
    if (fabsf(dz) < 1e-6f) return 0;
    float t = -ez/dz;
    if (t <= 0.f) return 0;
    hx = ex + dx*t; hy = ey + dy*t;
    return 1;
}
static void setMarkerModel(MapObj& mk, float x, float y, float z = 20.f) {
    float M[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1};
    mk.model.assign(M, M+16);
}

// ---------- 玩家队伍与 AI 野队（v0.5 地图队伍系统） ----------
struct MapParty {
    std::string name;
    int icon = -1;
    float x = 0, y = 0;
    float speed = 3.f;
    float homeX = 0, homeY = 0;
    float wait = 0.f;
    size_t objIdx = 0;
    size_t badgeIdx = (size_t)-1; // 红色可见性徽标
    std::vector<std::array<float,2>> way;
    size_t wayIdx = 0;
    bool isPlayer = false;
    bool hold = false;   // 遭遇中停在原地
    bool dead = false;   // 战斗中已被消灭（不显示不巡逻）
};

static std::vector<std::array<float,2>> g_way;
static size_t g_wayIdx = 0;
static float g_mx = 0.f, g_my = 0.f;
static int g_pendingSettlement = -1;
static size_t g_playerObjIdx = (size_t)-1; // 调试：VK_MAP_PLAYER_ONLY
static std::string g_encounter;
static std::string g_encounterPrev;
static float g_battleTimer = 0.f;      // 遭遇后停留 1.5 秒自动进战斗
static bool g_battleLaunched = false;
static std::string g_battleResult;     // 战斗结算："胜"/"败"（vk_scene 回传）
static std::string g_deadParty;        // 被消灭的野队名
static float g_hour = 6.f;      // 游戏时间：第 g_day 天 g_hour 时
static int g_day = 1;

// 精确包含测试：点在哪个地图面内（可走性另判）
static int faceAt(const MapData& md, float x, float y) {
    for (size_t i = 0; i < md.tris.size(); i++) {
        const auto& t = md.tris[i];
        float ax = md.pos[t[0]][0], ay = md.pos[t[0]][1];
        float bx = md.pos[t[1]][0], by = md.pos[t[1]][1];
        float cx = md.pos[t[2]][0], cy = md.pos[t[2]][1];
        auto side = [](float px,float py,float qx,float qy,float rx,float ry) {
            return (qx-px)*(ry-py) - (qy-py)*(rx-px);
        };
        float s0 = side(ax,ay,bx,by,x,y);
        float s1 = side(bx,by,cx,cy,x,y);
        float s2 = side(cx,cy,ax,ay,x,y);
        bool neg = (s0 < 0) || (s1 < 0) || (s2 < 0);
        bool pos = (s0 > 0) || (s1 > 0) || (s2 > 0);
        if (!(neg && pos)) return (int)i;
    }
    return -1;
}
static bool posWalkable(const MapData& md, float x, float y) {
    int f = faceAt(md, x, y);
    return f >= 0 && mapWalkable(md.faceTerrain[(size_t)f]);
}
static int currentFace(const MapData& md, float x, float y) {
    int f = faceAt(md, x, y);
    return f >= 0 ? f : nearestWalkableFace(md, x, y);
}
// 带滑动：整步可走 → 走；否则分别试 x / y 轴（贴河岸不卡死）
static void movePartySlide(const MapData& md, float& x, float& y, float dx, float dy) {
    if (dx == 0.f && dy == 0.f) return;
    float nx = x + dx, ny = y + dy;
    if (posWalkable(md, nx, ny)) { x = nx; y = ny; return; }
    bool moved = false;
    if (dx != 0.f && posWalkable(md, nx, y)) { x = nx; moved = true; }
    if (dy != 0.f && posWalkable(md, x, ny)) { y = ny; moved = true; }
    (void)moved;
}
static void makeWaypoints(const MapData& md, int fromFace, int toFace,
                          float endX, float endY,
                          std::vector<std::array<float,2>>& way) {
    way.clear();
    auto path = mapBfsPath(md, fromFace, toFace);
    if (path.empty()) return;
    way.push_back({0,0}); // 占位，起点由调用方填
    for (size_t i = 1; i < path.size(); i++) {
        if (i + 1 < path.size()) {
            float wx, wy; mapFaceCenter(md, path[i], wx, wy);
            way.push_back({wx, wy});
        } else {
            way.push_back({endX, endY});
        }
    }
}
// 沿 g_way 推进玩家队伍；返回是否还有路在走
static bool stepPlayerWay(const MapData& md, float step) {
    while (g_wayIdx < g_way.size()) {
        float dx = g_way[g_wayIdx][0] - g_mx, dy = g_way[g_wayIdx][1] - g_my;
        float dl = sqrtf(dx*dx + dy*dy);
        if (dl <= step) {
            g_mx = g_way[g_wayIdx][0]; g_my = g_way[g_wayIdx][1];
            g_wayIdx++;
            continue;
        }
        g_mx += dx/dl*step; g_my += dy/dl*step;
        return true;
    }
    return false;
}
static void stepAiParty(MapParty& p, float step) {
    if (p.wayIdx >= p.way.size()) return;
    float dx = p.way[p.wayIdx][0] - p.x, dy = p.way[p.wayIdx][1] - p.y;
    float dl = sqrtf(dx*dx + dy*dy);
    if (dl <= step) { p.x = p.way[p.wayIdx][0]; p.y = p.way[p.wayIdx][1]; p.wayIdx++; return; }
    p.x += dx/dl*step; p.y += dy/dl*step;
}
static void chooseAiPath(const MapData& md, MapParty& p, float range) {
    float tx = p.homeX + ((rand()%2000)/1000.f - 1.f)*range;
    float ty = p.homeY + ((rand()%2000)/1000.f - 1.f)*range;
    int ff = currentFace(md, p.x, p.y);
    int tf = nearestWalkableFace(md, tx, ty);
    if (ff < 0 || tf < 0) { p.wait = 3.f; return; }
    float ex, ey; mapFaceCenter(md, tf, ex, ey);
    makeWaypoints(md, ff, tf, ex, ey, p.way);
    if (p.way.size() >= 2) { p.way[0] = {p.x, p.y}; p.wayIdx = 1; p.wait = 0.f; }
    else p.wait = 3.f;
}

static void makeMarkerObj(App& A, MapObj& mk, const std::vector<std::string>& texNames) {
    float v[] = {
        -0.7f, 0.f, 0.f,  1.f,1.f,0.f,  0.f,0.f,1.f,  0.5f,0.5f,
         0.7f, 0.f, 0.f,  1.f,1.f,0.f,  0.f,0.f,1.f,  0.5f,0.5f,
         0.f,  1.4f, 0.f,  1.f,1.f,0.f,  0.f,0.f,1.f,  0.5f,0.5f,
    };
    uint32_t idx[] = {0,1,2};
    makeBuffer(A, sizeof(v), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, mk.vb, mk.vm);
    upload(A, mk.vb, mk.vm, v, sizeof(v));
    mk.idxCount = 3;
    makeBuffer(A, sizeof(idx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mk.ib, mk.im);
    upload(A, mk.ib, mk.im, idx, sizeof(idx));
    auto ti = A.texIndex.find("white");
    mk.descSet = A.texSets[ti != A.texIndex.end() ? ti->second : 0];
    mk.pc.resize(16, 0.f);
    mk.iconBaseScale = 1.f; mk.iconDim = 1.4f; // 让目的地黄标也随缩放保底可见
    mk.isIcon = true;
}

// 队伍可见性徽标：圆形/菱形色块，动态保底屏占，和目的地标记同管线（保证任何缩放都看得见）
static size_t makeBadgeObj(App& A, VkDescriptorSet desc, float r, float g, float b,
                           float radius, float x, float y, float z, std::vector<MapObj>& objs) {
    const int seg = 16;
    std::vector<float> verts;
    verts.reserve((size_t)(seg+2)*11);
    auto push = [&](float px, float py) {
        verts.push_back(px); verts.push_back(py); verts.push_back(0.f);
        verts.push_back(r); verts.push_back(g); verts.push_back(b);
        verts.push_back(0.f); verts.push_back(0.f); verts.push_back(1.f);
        verts.push_back(0.5f); verts.push_back(0.5f);
    };
    push(0.f, 0.f);
    for (int i = 0; i <= seg; i++) {
        float a = (float)i / (float)seg * 6.2831853f;
        push(cosf(a)*radius, sinf(a)*radius);
    }
    std::vector<uint32_t> idx;
    for (int i = 0; i < seg; i++) {
        idx.push_back(0); idx.push_back((uint32_t)i+1); idx.push_back((uint32_t)i+2);
    }
    MapObj o;
    makeBuffer(A, verts.size()*4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, o.vb, o.vm);
    upload(A, o.vb, o.vm, verts.data(), verts.size()*4);
    o.idxCount = (uint32_t)idx.size();
    makeBuffer(A, o.idxCount*4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, o.ib, o.im);
    upload(A, o.ib, o.im, idx.data(), o.idxCount*4);
    o.descSet = desc;
    o.pc.resize(16, 0.f);
    float M[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1};
    o.model.assign(M, M+16);
    o.iconBaseScale = 1.f;
    o.iconDim = radius*2.f;
    o.isIcon = true;
    size_t out = objs.size();
    objs.push_back(std::move(o));
    return out;
}

static void recordMapCmd(App& A, const std::vector<MapObj>& objs, size_t img) {
    VK_DEV_FN(A.dev, vkBeginCommandBuffer);
    VK_DEV_FN(A.dev, vkCmdBeginRenderPass); VK_DEV_FN(A.dev, vkCmdBindPipeline);
    VK_DEV_FN(A.dev, vkCmdBindDescriptorSets); VK_DEV_FN(A.dev, vkCmdPushConstants);
    VK_DEV_FN(A.dev, vkCmdBindVertexBuffers); VK_DEV_FN(A.dev, vkCmdBindIndexBuffer); VK_DEV_FN(A.dev, vkCmdDrawIndexed);
    VK_DEV_FN(A.dev, vkCmdEndRenderPass); VK_DEV_FN(A.dev, vkEndCommandBuffer);
    VkCommandBufferBeginInfo bb{}; bb.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(A.cmds[img], &bb);
    VkClearValue cv[2]{}; cv[0].color={{0.10f,0.14f,0.22f,1}}; cv[1].depthStencil={1.0f,0};
    VkRenderPassBeginInfo rp{}; rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass=A.rp; rp.framebuffer=A.fbs[img]; rp.renderArea={{0,0},A.extent};
    rp.clearValueCount=2; rp.pClearValues=cv;
    vkCmdBeginRenderPass(A.cmds[img], &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(A.cmds[img], VK_PIPELINE_BIND_POINT_GRAPHICS, A.pipe);
    bool iconsOnly = getenv("VK_MAP_ICONS_ONLY") != nullptr;
    bool playerOnly = getenv("VK_MAP_PLAYER_ONLY") != nullptr;
    for (size_t oi = 0; oi < objs.size(); oi++) {
        auto& o = objs[oi];
        if (iconsOnly && !o.isIcon) continue;
        if (playerOnly && oi != g_playerObjIdx) continue;
        vkCmdBindDescriptorSets(A.cmds[img], VK_PIPELINE_BIND_POINT_GRAPHICS, A.pipeLayout, 0, 1, &o.descSet, 0, nullptr);
        vkCmdPushConstants(A.cmds[img], A.pipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, o.pc.data());
        VkDeviceSize off=0;
        vkCmdBindVertexBuffers(A.cmds[img], 0, 1, &o.vb, &off);
        vkCmdBindIndexBuffer(A.cmds[img], o.ib, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(A.cmds[img], o.idxCount, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(A.cmds[img]);
    vkEndCommandBuffer(A.cmds[img]);
}

static void allocMapCmds(App& A) {
    VK_DEV_FN(A.dev, vkAllocateCommandBuffers);
    VkCommandBufferAllocateInfo ca{}; ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool=A.cmdPool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=(uint32_t)A.images.size();
    A.cmds.resize(A.images.size());
    CHECK(vkAllocateCommandBuffers(A.dev, &ca, A.cmds.data()), "allocCmds");
}
static LONG WINAPI mapCrashFilter(EXCEPTION_POINTERS* ep) {
    FILE* f = fopen("vk_map_crash.txt", "a");
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

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0); setvbuf(stderr, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(mapCrashFilter);
    if (argc < 3) { std::printf("usage: vk_map <map.txt> <Textures目录>\n"); return 1; }
    App A;
    A.hInst = GetModuleHandleA(nullptr);
    WNDCLASSA wc{}; wc.lpfnWndProc=wndProc; wc.hInstance=A.hInst; wc.lpszClassName="vkMapWnd";
    RegisterClassA(&wc);
    A.hwnd = CreateWindowExA(0, "vkMapWnd", "CV War Campaign Map (loading)",
        WS_OVERLAPPEDWINDOW|WS_VISIBLE, 120, 60, 1280, 720, nullptr, nullptr, A.hInst, nullptr);
    if (!A.hwnd) { std::printf("FAIL 建窗\n"); return 1; }
    setTitleUtf8(A.hwnd, "CV War 大地图 | WASD行军 点击寻路/进城 空格暂停 QE缩放 方向键视角 ESC退出");
    initVk(A);

    MapData md;
    try { md = loadMap(argv[1]); }
    catch (const std::exception& e) { std::fprintf(stderr, "地图加载失败: %s\n", e.what()); return 1; }
    std::filesystem::path mapPath(argv[1]);
    std::string moduleDir = mapPath.parent_path().string();          // Modules\CV War
    std::string gameRoot = mapPath.parent_path().parent_path().parent_path().string(); // 游戏根
    auto icons = loadMapIcons(moduleDir);
    auto templates = loadPartyTemplates(moduleDir);
    auto partyRecs = loadParties(moduleDir);
    auto settlements = loadSettlements(partyRecs);
    std::fprintf(stderr, "map_icons %zu 个 / parties %zu 条 / 定居点 %zu 个（module=%s）\n",
                 icons.size(), partyRecs.size(), settlements.size(), moduleDir.c_str());
    std::set<std::string> texNames;
    for (size_t t=0;t<md.meshes.size();t++) if (!md.meshes[t].verts.empty()) texNames.insert(mapTexture((int)t).first);
    texNames.insert("fake_houses"); // 地图图标贴图（根 Textures）
    texNames.insert("white");       // 程序化纯白贴图：徽标/标记的顶点色直出
    std::vector<std::string> names(texNames.begin(), texNames.end());
    std::fprintf(stderr, "地图 %zu 顶点 范围 x[%.0f..%.0f] y[%.0f..%.0f]\n",
        md.pos.size(), md.minX, md.maxX, md.minY, md.maxY);
    initTextures(A, argv[2], names);
    initMapPipeline(A);
    std::vector<MapObj> mapObjs;
    buildMapObjs(A, md, mapObjs);

    std::map<std::string, brf::Mesh> iconMeshes;
    loadIconMeshes(moduleDir, iconMeshes);
    auto ti = A.texIndex.find("fake_houses");
    VkDescriptorSet iconDesc = A.texSets[ti != A.texIndex.end() ? ti->second : 0];
    auto tw = A.texIndex.find("white");
    VkDescriptorSet whiteDesc = A.texSets[tw != A.texIndex.end() ? tw->second : 0];

    // 定居点图标：作者在 parties.txt 里指定的 icon（flags 低 8 位）
    size_t builtIcons = 0;
    for (auto& s : settlements) {
        if ((size_t)s.icon >= icons.size()) { std::fprintf(stderr, "定居点 %s 图标越界 %d\n", s.id.c_str(), s.icon); continue; }
        s.iconIdx = makeIconObj(A, iconMeshes, iconDesc, icons[(size_t)s.icon].mesh, icons[(size_t)s.icon].scale,
                                s.x, s.y, 2.f, mapObjs);
        if (s.iconIdx != (size_t)-1) builtIcons++;
    }
    std::fprintf(stderr, "定居点图标 %zu/%zu 个已上屏\n", builtIcons, settlements.size());

    // 玩家队伍：p_main_party（坐标 tokens 18/19；图标 = flags & 0xff）
    // 从场景按 M 回来时，vk_map 的 argv[3]/argv[4] 带出城时的队伍坐标
    g_mx = (md.minX+md.maxX)/2; g_my = (md.minY+md.maxY)/2;
    int playerIcon = 0;
    for (auto& p : partyRecs) if (p.id == "p_main_party") {
        g_mx = p.x; g_my = p.y; playerIcon = p.icon; break;
    }
    if (argc >= 5) { g_mx = (float)atof(argv[3]); g_my = (float)atof(argv[4]); }
    if (argc >= 7) { // 战斗结算回传（ASCII：win/lose + 野队名）
        g_battleResult = (std::string(argv[5]) == "win") ? "胜" : (std::string(argv[5]) == "lose" ? "败" : std::string(argv[5]));
        g_deadParty = argv[6];
    }
    if (!posWalkable(md, g_mx, g_my)) {
        int f = nearestWalkableFace(md, g_mx, g_my);
        mapFaceCenter(md, f, g_mx, g_my);
    }
    size_t playerIdx = (size_t)-1;
    if ((size_t)playerIcon < icons.size())
        playerIdx = makeIconObj(A, iconMeshes, iconDesc, icons[(size_t)playerIcon].mesh,
                                icons[(size_t)playerIcon].scale, g_mx, g_my, 3.f, mapObjs);
    g_playerObjIdx = playerIdx;
    // 玩家青色徽标：无论缩放到哪里都一眼看见（作者图标保留在徽标下方）
    size_t playerBadgeIdx = makeBadgeObj(A, whiteDesc, 0.1f, 0.95f, 1.f, 3.f, g_mx, g_my, 20.f, mapObjs);
    std::fprintf(stderr, "玩家队伍起点 (%.1f,%.1f) icon=%d objIdx=%zu tri=%u\n",
                 g_mx, g_my, playerIcon, playerIdx,
                 playerIdx != (size_t)-1 ? mapObjs[playerIdx].idxCount : 0);

    // AI 野队：作者放的 bandit spawn point 当出生点，party template 决定名字和图标
    std::map<std::string, TemplateDef> tplById;
    for (auto& t : templates) tplById[t.id] = t;
    struct SpawnMap { const char* pid; const char* tpl; };
    const SpawnMap spawns[] = {
        {"p_looter_spawn_point",         "pt_looters"},
        {"p_steppe_bandit_spawn_point",  "pt_steppe_bandits"},
        {"p_forest_bandit_spawn_point",  "pt_forest_bandits"},
        {"p_mountain_bandit_spawn_point","pt_mountain_bandits"},
        {"p_sea_raider_spawn_point_1",   "pt_sea_raiders"},
        {"p_desert_bandit_spawn_point",  "pt_desert_bandits"},
    };
    std::vector<MapParty> ai;
    for (auto& sm : spawns) {
        for (auto& p : partyRecs) if (p.id == sm.pid) {
            auto tIt = tplById.find(sm.tpl);
            if (tIt == tplById.end()) continue;
            MapParty ap;
            ap.name = tIt->second.name; ap.icon = tIt->second.icon;
            ap.x = p.x; ap.y = p.y; ap.homeX = p.x; ap.homeY = p.y;
            ap.speed = 1.8f;
            if (!posWalkable(md, ap.x, ap.y)) {
                int f = nearestWalkableFace(md, ap.x, ap.y);
                mapFaceCenter(md, f, ap.x, ap.y);
                ap.homeX = ap.x; ap.homeY = ap.y;
            }
            if ((size_t)ap.icon < icons.size())
                ap.objIdx = makeIconObj(A, iconMeshes, iconDesc, icons[(size_t)ap.icon].mesh,
                                        icons[(size_t)ap.icon].scale, ap.x, ap.y, 3.f, mapObjs);
            else ap.objIdx = (size_t)-1;
            ap.badgeIdx = makeBadgeObj(A, whiteDesc, 1.f, 0.12f, 0.12f, 2.2f, ap.x, ap.y, 15.f, mapObjs);
            if (!g_deadParty.empty() && ap.name == g_deadParty) {
                ap.dead = true;
                std::fprintf(stderr, "[结算] %s 已被消灭\n", ap.name.c_str());
            }
            if (ap.dead) { // 被消灭的野队不显示
                if (ap.objIdx != (size_t)-1) mapObjs[ap.objIdx].model[14] = -999.f;
                if (ap.badgeIdx != (size_t)-1) mapObjs[ap.badgeIdx].model[14] = -999.f;
            }
            ai.push_back(std::move(ap));
        }
    }
    std::fprintf(stderr, "AI 野队 %zu 支（bandit spawn point + party template 图标）\n", ai.size());

    // 黄色标记 = 点击目的地（无路径时藏到地下）
    size_t markerIdx = mapObjs.size();
    mapObjs.emplace_back();
    makeMarkerObj(A, mapObjs[markerIdx], names);
    setMarkerModel(mapObjs[markerIdx], g_mx, g_my, -999.f);
    allocMapCmds(A);

    VK_DEV_FN(A.dev, vkAcquireNextImageKHR); VK_DEV_FN(A.dev, vkQueueSubmit); VK_DEV_FN(A.dev, vkQueuePresentKHR);
    VK_DEV_FN(A.dev, vkCreateFence); VK_DEV_FN(A.dev, vkWaitForFences); VK_DEV_FN(A.dev, vkResetFences); VK_DEV_FN(A.dev, vkResetCommandBuffer);
    A.fences.resize(A.images.size());
    A.acqFences.resize(A.images.size());
    for (size_t i=0;i<A.images.size();i++) {
        VkFenceCreateInfo fi{}; fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(A.dev, &fi, nullptr, &A.fences[i]);
        VkFenceCreateInfo af{}; af.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(A.dev, &af, nullptr, &A.acqFences[i]);
    }

    float cx=g_mx, cy=g_my;
    float yaw=0.f, dist=150.f, height=130.f; // 默认贴近队伍，远处图标会自动放大保底
    auto k = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    auto enterSettlement = [&](size_t idx) {
        if (idx >= settlements.size()) return;
        auto& s = settlements[idx];
        std::string sco = moduleDir + "\\SceneObj\\" + s.scene + ".sco";
        if (!std::filesystem::exists(sco)) {
            std::fprintf(stderr, "%s 场景缺 %s，不进城\n", s.id.c_str(), s.scene.c_str());
            return;
        }
        std::string argline =
            "\"" + sco + "\" \"" + moduleDir + "\\scene_props.txt\" \"" +
            gameRoot + "\\CommonRes\" \"" + moduleDir + "\\Textures\" \"" +
            std::string(argv[1]) + "\" \"" + std::string(argv[2]) + "\" \"" +
            gameRoot + "\\Textures\"";
        char posbuf[64];
        std::snprintf(posbuf, sizeof(posbuf), " %.2f %.2f", g_mx, g_my);
        argline += posbuf;
        std::fprintf(stderr, "进入 %s (%s，CV War 作者布局)\n", s.name.c_str(), s.scene.c_str());
        launchAndExit("vk_scene.exe", argline);
    };
    // 遭遇野队 → 1.5 秒后进入 CV War 作者自己的战斗场景（v0.6-1）
    auto enterBattle = [&]() {
        std::string sco = moduleDir + "\\SceneObj\\scn_field_1.sco";
        if (!std::filesystem::exists(sco)) sco = moduleDir + "\\SceneObj\\scn_quick_battle_1.sco";
        if (!std::filesystem::exists(sco)) { std::fprintf(stderr, "没有可用战斗场景\n"); return; }
        std::string argline =
            "\"--battle\" \"" + sco + "\" \"" + moduleDir + "\\scene_props.txt\" \"" +
            gameRoot + "\\CommonRes\" \"" + moduleDir + "\\Textures\" \"" +
            std::string(argv[1]) + "\" \"" + std::string(argv[2]) + "\" \"" +
            gameRoot + "\\Textures\"";
        char tail[160];
        const char* nm = g_encounter.empty() ? "野队" : g_encounter.c_str();
        std::snprintf(tail, sizeof(tail), " 5 %.2f %.2f \"%s\"", g_mx, g_my, nm);
        argline += tail;
        std::fprintf(stderr, "[战斗] 进入 %s（CV War 作者布局，敌方 5 人）\n", sco.c_str());
        launchAndExit("vk_scene.exe", argline);
    };
    MSG msg;
    uint32_t frame=0;
    LARGE_INTEGER qf, lastQ;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&lastQ);
    int lastHour=-1, lastDay=-1; bool lastPaused=false; std::string lastEnc;
    while (true) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message==WM_QUIT) return 0;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        LARGE_INTEGER nowQ;
        QueryPerformanceCounter(&nowQ);
        float dt=std::min(0.1f,(float)((double)(nowQ.QuadPart-lastQ.QuadPart)/(double)qf.QuadPart));
        lastQ=nowQ;
        float simDt = g_paused ? 0.f : dt;

        // 游戏时间：1 秒 = 1 小时
        if (simDt > 0.f) { g_hour += simDt; while (g_hour >= 24.f) { g_hour -= 24.f; g_day++; } }

        // 相机围绕玩家队伍，QE 缩放 / 方向键视角
        // 缩放时高度跟着比例走（俯角不变），拉到最远时地图仍铺满屏幕而不是压成一条线
        if (k('E')) { dist=std::max(120.f, dist-180.f*dt); height=std::max(60.f, dist*0.866f); }
        if (k('Q')) { dist=std::min(520.f, dist+180.f*dt); height=std::max(60.f, dist*0.866f); }
        if (k(VK_LEFT))  yaw+=0.8f*dt;
        if (k(VK_RIGHT)) yaw-=0.8f*dt;
        if (k(VK_UP))   height=std::min(1800.f, height+240.f*dt);
        if (k(VK_DOWN)) height=std::max(120.f, height-240.f*dt);
        cx += (g_mx-cx)*std::min(1.f, dt*4.f);
        cy += (g_my-cy)*std::min(1.f, dt*4.f);

        float fwx=sinf(yaw), fwy=cosf(yaw);
        float rx=cosf(yaw), ry=-sinf(yaw);

        // WASD = 玩家队伍实时行军（相对屏幕：W 朝屏幕上方，Shift 加速）
        float mdx=0.f, mdy=0.f;
        if (k('W')) { mdx -= fwx; mdy -= fwy; }
        if (k('S')) { mdx += fwx; mdy += fwy; }
        if (k('D')) { mdx += rx; mdy += ry; }
        if (k('A')) { mdx -= rx; mdy -= ry; }
        if ((mdx != 0.f || mdy != 0.f) && simDt > 0.f) {
            float len = sqrtf(mdx*mdx + mdy*mdy);
            float speed = k(VK_SHIFT) ? 12.f : 4.5f;
            movePartySlide(md, g_mx, g_my, mdx/len*speed*simDt, mdy/len*speed*simDt);
            g_way.clear(); g_wayIdx = 0; g_pendingSettlement = -1;
            setMarkerModel(mapObjs[markerIdx], g_mx, g_my, -999.f);
        }

        float eye[3]={cx+sinf(yaw)*dist, cy+cosf(yaw)*dist, height};
        float tgt[3]={cx, cy, 0.f};
        float aspect=(float)A.extent.width/std::max(1u,(uint32_t)A.extent.height);

        // 点击：射线打 z=0 地图平面 → 点定居点=进城/行军进城，点空地=寻路
        if (g_click.x >= 0) {
            float hx=0, hy=0;
            int hit = rayPlaneHit(eye[0], eye[1], eye[2], tgt[0], tgt[1], tgt[2],
                                  0.9f, aspect, (float)g_click.x, (float)g_click.y,
                                  (float)A.extent.width, (float)A.extent.height, hx, hy);
            g_click.x = -1; g_click.y = -1;
            if (hit) {
                bool handled = false;
                for (size_t si=0; si<settlements.size(); si++) {
                    auto& s = settlements[si];
                    float ddx = hx - s.x, ddy = hy - s.y;
                    if (ddx*ddx + ddy*ddy > 9.f) continue;
                    float pdx = g_mx - s.x, pdy = g_my - s.y;
                    int ff = currentFace(md, g_mx, g_my);
                    int tf = nearestWalkableFace(md, s.x, s.y);
                    if (pdx*pdx + pdy*pdy <= 9.f || ff < 0 || tf < 0) {
                        enterSettlement(si); // 已在城下：直接进城
                    } else {
                        makeWaypoints(md, ff, tf, s.x, s.y, g_way);
                        if (g_way.size() >= 2) {
                            g_way[0] = {g_mx, g_my}; g_wayIdx = 1; g_pendingSettlement = (int)si;
                            setIconModel(mapObjs[markerIdx], s.x, s.y, 20.f, dist, (float)A.extent.height, 14.f);
                            std::fprintf(stderr, "向 %s 行军，到达自动进城\n", s.name.c_str());
                        } else {
                            std::fprintf(stderr, "%s 无路可通，直接进城（保持旧行为）\n", s.name.c_str());
                            enterSettlement(si);
                        }
                    }
                    handled = true;
                    break;
                }
                if (!handled) {
                    int ff = currentFace(md, g_mx, g_my);
                    int tf = nearestWalkableFace(md, hx, hy);
                    makeWaypoints(md, ff, tf, hx, hy, g_way);
                    if (g_way.size() >= 2) {
                        g_way[0] = {g_mx, g_my}; g_wayIdx = 1; g_pendingSettlement = -1;
                        setIconModel(mapObjs[markerIdx], hx, hy, 20.f, dist, (float)A.extent.height, 14.f);
                        std::fprintf(stderr, "点击(%.0f,%.0f) 路径 %zu 段\n", hx, hy, g_way.size()-1);
                    } else {
                        std::fprintf(stderr, "点击(%.0f,%.0f) 无路（起点面%d 目标面%d）\n", hx, hy, ff, tf);
                    }
                }
            }
        }

        // 玩家沿点击路径前进
        if (simDt > 0.f && g_wayIdx < g_way.size()) {
            float speed = k(VK_SHIFT) ? 12.f : 4.5f;
            bool moving = stepPlayerWay(md, speed*simDt);
            if (!moving) {
                if (g_pendingSettlement >= 0) {
                    auto& s = settlements[(size_t)g_pendingSettlement];
                    float pdx = g_mx - s.x, pdy = g_my - s.y;
                    if (pdx*pdx + pdy*pdy <= 9.f) enterSettlement((size_t)g_pendingSettlement);
                }
                g_pendingSettlement = -1;
                setMarkerModel(mapObjs[markerIdx], g_mx, g_my, -999.f);
            }
        }

        // AI 野队巡逻：围绕出生点随机选可走面，BFS 行走
        if (simDt > 0.f) for (auto& ap : ai) {
            if (ap.hold || ap.dead) continue;
            if (ap.wayIdx < ap.way.size()) stepAiParty(ap, ap.speed*simDt);
            else { ap.wait -= simDt; if (ap.wait <= 0.f) chooseAiPath(md, ap, 30.f); }
            if (ap.objIdx != (size_t)-1) setIconModel(mapObjs[ap.objIdx], ap.x, ap.y, 3.f,
                                                      dist, (float)A.extent.height, 16.f);
            if (ap.badgeIdx != (size_t)-1) setIconModel(mapObjs[ap.badgeIdx], ap.x, ap.y, 15.f,
                                                        dist, (float)A.extent.height, 14.f);
        }

        // 遭遇：玩家撞上野队 → 双方停下，标题显示（战斗下一版接）
        if (simDt > 0.f) {
        g_encounter.clear();
        for (auto& ap : ai) {
            if (ap.dead) continue;
            float dx = ap.x - g_mx, dy = ap.y - g_my;
            float d2 = dx*dx + dy*dy;
            if (d2 < 1.44f) {
                g_encounter = ap.name;
                ap.hold = true;
                g_way.clear(); g_wayIdx = 0; g_pendingSettlement = -1;
                setMarkerModel(mapObjs[markerIdx], g_mx, g_my, -999.f);
                g_battleTimer += simDt;
            } else if (ap.hold && d2 > 4.84f) {
                ap.hold = false; ap.wait = 1.5f;
            }
        }
        if (g_battleTimer >= 1.5f && !g_battleLaunched) {
            g_battleLaunched = true;
            enterBattle();
        }
        }

        if (g_encounter != g_encounterPrev) {
            if (!g_encounter.empty()) std::fprintf(stderr, "[遭遇] %s！\n", g_encounter.c_str());
            else { std::fprintf(stderr, "[离开] %s\n", g_encounterPrev.c_str()); g_battleTimer = 0.f; g_battleLaunched = false; }
            g_encounterPrev = g_encounter;
        }

        // 玩家图标跟手（比野队/定居点稍大，永远看得见）
        if (playerIdx != (size_t)-1) setIconModel(mapObjs[playerIdx], g_mx, g_my, 3.f,
                                                  dist, (float)A.extent.height, 22.f);
        if (playerBadgeIdx != (size_t)-1) setIconModel(mapObjs[playerBadgeIdx], g_mx, g_my, 20.f,
                                                       dist, (float)A.extent.height, 18.f);
        for (auto& s : settlements)
            if (s.iconIdx != (size_t)-1) setIconModel(mapObjs[s.iconIdx], s.x, s.y, 2.f,
                                                      dist, (float)A.extent.height, 12.f);

        updateMapMVP(mapObjs, eye, tgt, aspect);

        // 标题栏 HUD（时间/坐标/暂停/遭遇）
        int hour = (int)g_hour;
        if (hour != lastHour || g_day != lastDay || g_paused != lastPaused || g_encounter != lastEnc) {
            std::string enc = g_encounter.empty() ? "" : " | 遭遇:" + g_encounter;
            std::string res = g_battleResult.empty() ? "" : (" | 战斗" + g_battleResult + (g_deadParty.empty() ? "" : ":" + g_deadParty));
            char title[380];
            std::snprintf(title, sizeof(title),
                "CV War 大地图 | 第%d天 %02d:00 | 队伍(%.0f,%.0f)%s%s%s | WASD行军 Shift加速 点击寻路/进城 空格暂停 QE缩放 方向键视角 ESC退出",
                g_day, hour, g_mx, g_my,
                g_paused ? " | 已暂停" : "", enc.c_str(), res.c_str());
            setTitleUtf8(A.hwnd, title);
            lastHour = hour; lastDay = g_day; lastPaused = g_paused; lastEnc = g_encounter;
        }

        size_t fi=frame%A.images.size();
        uint32_t img=0;
        VkResult ar=vkAcquireNextImageKHR(A.dev, A.swap, UINT64_MAX, VK_NULL_HANDLE, A.acqFences[fi], &img);
        if (ar!=VK_SUCCESS && ar!=VK_SUBOPTIMAL_KHR) {
            vkResetFences(A.dev,1,&A.acqFences[fi]); Sleep(10); continue;
        }
        vkWaitForFences(A.dev,1,&A.acqFences[fi],VK_TRUE,UINT64_MAX);
        vkResetFences(A.dev,1,&A.acqFences[fi]);
        vkWaitForFences(A.dev,1,&A.fences[img],VK_TRUE,UINT64_MAX);
        vkResetFences(A.dev,1,&A.fences[img]);
        vkResetCommandBuffer(A.cmds[img],0);
        recordMapCmd(A,mapObjs,img);
        VkPipelineStageFlags ws=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{}; si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount=1; si.pCommandBuffers=&A.cmds[img]; si.waitSemaphoreCount=0; si.pWaitDstStageMask=&ws;
        vkQueueSubmit(A.queue,1,&si,A.fences[img]);
        VkPresentInfoKHR pi{}; pi.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.swapchainCount=1; pi.pSwapchains=&A.swap; pi.pImageIndices=&img;
        vkQueuePresentKHR(A.queue,&pi);
        frame++;
        Sleep(1);
    }
}
