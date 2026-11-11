// vk_mesh.cpp —— 自研最小闭环：解析 BRF 网格并用 Vulkan 渲染
// 依赖：brf.h（同目录）、mesh.vert.spv / mesh.frag.spv（tools/shader-tool 生成）
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include "brf.h"
#include "dds.h"
#include "sco.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
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
    std::vector<VkFence> fences;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipeline pipeSky = VK_NULL_HANDLE;
    struct Obj { VkBuffer vb=VK_NULL_HANDLE, ib=VK_NULL_HANDLE; VkDeviceMemory vm=VK_NULL_HANDLE, im=VK_NULL_HANDLE; uint32_t idxCount=0; std::vector<float> pc; int texSlot=0; VkDescriptorSet descSet=VK_NULL_HANDLE; std::string diffName="shields"; bool sky=false; };
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
static void computeSceneMVP(std::vector<float>& pc, float cx, float cz) {
    std::vector<float> V, P;
    // 渲染坐标：x=世界x, y=世界z(南北), z=世界y(高度)
    float eye[3] = {cx, cz+85.f, 55.f};
    lookAt(V, eye[0], eye[1], eye[2], cx, cz-18.f, 22.f);
    perspective(P, 1.0f, 1280.0f/720.0f, 0.5f, 800.0f);
    pc.resize(24, 0.f);
    matMul44(P.data(), V.data(), pc); // pc[0..15] = mvp
    pc[16] = eye[0]; pc[17] = eye[1]; pc[18] = eye[2];
    pc[20] = 70.f; pc[21] = 300.f;    // 雾起点/终点（世界距离）
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_CLOSE || (msg == WM_KEYDOWN && w == VK_ESCAPE)) PostQuitMessage(0);
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
        std::string path = ddsDir + "\\" + n + ".dds";
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
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vs; stages[0].pName="main";
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fs; stages[1].pName="main";
    VkPipelineShaderStageCreateInfo skyStages[2]{};
    skyStages[0].sType=skyStages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    skyStages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; skyStages[0].module=vsSky; skyStages[0].pName="main";
    skyStages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; skyStages[1].module=fsSky; skyStages[1].pName="main";

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
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
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
    VK_DEV_FN(A.dev, vkDestroyShaderModule);
    vkDestroyShaderModule(A.dev, vs, nullptr); vkDestroyShaderModule(A.dev, fs, nullptr);
    vkDestroyShaderModule(A.dev, vsSky, nullptr); vkDestroyShaderModule(A.dev, fsSky, nullptr);
}

// 网格库：基础名 -> 该基础名下的所有网格片段（与 OpenBRF AnalyzeName 规则一致）
static std::map<std::string, std::vector<brf::Mesh>> g_meshGroups;
static std::map<std::string, std::string> g_materialDiffuse;

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

// 生成一个场景物件实例的 GPU 缓冲与 MVP
static void makeSceneObj(App& A, const brf::Mesh& mesh, const sco::Prop& prop, float cx, float cz) {
    std::vector<float> verts; verts.reserve(mesh.verts.size()*11);
    for (auto& v : mesh.verts) {
        if (v.index < 0 || (size_t)v.index >= mesh.positions.size()) continue;
        auto& p = mesh.positions[v.index];
        // SCO 3x3 row-major 矩阵（BRF 坐标空间）
        float bx = prop.m[0]*p.x + prop.m[1]*p.y + prop.m[2]*p.z;
        float by = prop.m[3]*p.x + prop.m[4]*p.y + prop.m[5]*p.z;
        float bz = prop.m[6]*p.x + prop.m[7]*p.y + prop.m[8]*p.z;
        bx *= prop.scale[0]; by *= prop.scale[1]; bz *= prop.scale[2];
        float wx = prop.pos[0]+bx, wy = prop.pos[1]+by, wz = prop.pos[2]+bz;
        // BRF -> 渲染坐标：x'=x, y'=z, z'=-y
        verts.push_back(wx); verts.push_back(wz); verts.push_back(-wy);
        float r = ((v.color) & 0xFF) / 255.0f;
        float g = ((v.color >> 8) & 0xFF) / 255.0f;
        float b = ((v.color >> 16) & 0xFF) / 255.0f;
        verts.push_back(1); verts.push_back(0); verts.push_back(0);
        // 法线：同一旋转矩阵变换 + 坐标映射，归一化
        float nx = prop.m[0]*v.normal.x + prop.m[1]*v.normal.y + prop.m[2]*v.normal.z;
        float ny = prop.m[3]*v.normal.x + prop.m[4]*v.normal.y + prop.m[5]*v.normal.z;
        float nz = prop.m[6]*v.normal.x + prop.m[7]*v.normal.y + prop.m[8]*v.normal.z;
        float nl = sqrtf(nx*nx+ny*ny+nz*nz); if (nl < 1e-6f) nl = 1.f;
        verts.push_back(nx/nl); verts.push_back(nz/nl); verts.push_back(-ny/nl);
        verts.push_back(v.uv.u); verts.push_back(v.uv.v);
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
    computeSceneMVP(obj.pc, cx, cz);
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
struct GroundSpec { std::string name, mat, diffuse; float tiling = 4.f; };
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
        v.push_back(std::move(g));
    }
    if (v.size() != 11) { std::fprintf(stderr, "ground_specs 行数异常 %zu\n", v.size()); v.clear(); }
    return v;
}

// 一个 cell 的所属贴图层：非零层按 path > village > turf 优先；全 0 用 turf 兜底
static int pickGroundLayer(const sco::Terrain& t, uint32_t i, uint32_t j) {
    size_t idx = (size_t)j*t.width+i;
    auto val = [&](int32_t li) -> uint8_t {
        auto* l = t.findLayer(li);
        return (l && idx < l->cells.size()) ? l->cells[idx] : 0;
    };
    if (val(10)) return 10;
    if (val(9)) return 9;
    if (val(2)) return 2;
    for (int32_t li=0; li<=10; li++) if (li!=2 && li!=9 && li!=10 && val(li)) return (int)li;
    return 2; // 全 0：turf 草地兜底
}

// 把 terrain 生成地面网格：按 ground layer 分贴图，每层一个 Obj；cell=2m，原点(0,0)
static void makeGroundObj(App& A, const sco::Terrain& t, const std::vector<GroundSpec>& specs,
                          float cx, float cz, std::set<std::string>& needDds) {
    uint32_t w = t.width, h = t.height;
    if (w < 2 || h < 2 || t.elevation.empty()) return;
    const float cell = 2.0f;
    auto hAt = [&](uint32_t i, uint32_t j) -> float {
        size_t idx = (size_t)j*w+i;
        return idx < t.elevation.size() ? t.elevation[idx] : 0.f;
    };
    std::array<std::vector<float>, 11> verts;
    std::array<std::vector<uint32_t>, 11> idxs;
    uint64_t totalQuads = 0;
    for (uint32_t j=0;j<h-1;j++) for (uint32_t i=0;i<w-1;i++) {
        int li = pickGroundLayer(t, i, j);
        if (li < 0 || li > 10) li = 2;
        auto& V = verts[li];
        auto& I = idxs[li];
        uint32_t base = (uint32_t)(V.size()/11);
        // 4 角顶点（世界坐标 x=i*cell, z=j*cell；渲染坐标 y'=z, z'=-y）
        uint32_t cs[4][2] = {{i,j},{i+1,j},{i,j+1},{i+1,j+1}};
        for (auto& c : cs) {
            float x = c[0]*cell, z = c[1]*cell, y = hAt(c[0], c[1]);
            V.push_back(x); V.push_back(z); V.push_back(-y);
            V.push_back(1); V.push_back(1); V.push_back(1);
            float lx = hAt(c[0]>0?c[0]-1:0, c[1]);
            float rx = hAt(c[0]+1<w?c[0]+1:w-1, c[1]);
            float ty = hAt(c[0], c[1]>0?c[1]-1:0);
            float by = hAt(c[0], c[1]+1<h?c[1]+1:h-1);
            float dhdx = (rx-lx)/(2*cell), dhdz = (by-ty)/(2*cell);
            float fx = -dhdx, fy = -dhdz, fz = 1.0f;
            float fl = sqrtf(fx*fx+fy*fy+fz*fz); if (fl<1e-6f) fl=1;
            V.push_back(fx/fl); V.push_back(fy/fl); V.push_back(fz/fl);
            float tile = specs.empty() || (size_t)li >= specs.size() ? 4.f : specs[li].tiling;
            V.push_back(x/tile); V.push_back(z/tile);
        }
        I.push_back(base); I.push_back(base+2); I.push_back(base+1);
        I.push_back(base+1); I.push_back(base+2); I.push_back(base+3);
        totalQuads++;
    }
    size_t made = 0;
    for (int li=0; li<=10; li++) {
        if (idxs[li].empty()) continue;
        App::Obj obj;
        std::string diff = "ground_path";
        if ((size_t)li < specs.size() && !specs[li].diffuse.empty()) diff = specs[li].diffuse;
        obj.diffName = diff;
        needDds.insert(diff);
        auto ti = A.texIndex.find(diff);
        obj.texSlot = (ti != A.texIndex.end()) ? ti->second : 0;
        obj.descSet = A.texSets.empty() ? VK_NULL_HANDLE : A.texSets[obj.texSlot];
        makeBuffer(A, verts[li].size()*4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, obj.vb, obj.vm);
        upload(A, obj.vb, obj.vm, verts[li].data(), verts[li].size()*4);
        obj.idxCount = (uint32_t)idxs[li].size();
        makeBuffer(A, obj.idxCount*4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, obj.ib, obj.im);
        upload(A, obj.ib, obj.im, idxs[li].data(), obj.idxCount*4);
        computeSceneMVP(obj.pc, cx, cz);
        A.objs.push_back(std::move(obj));
        made++;
    }
    std::fprintf(stderr, "地面 %ux%u cell=%.0fm 总quad=%llu 分层对象=%zu 层分布:", w, h, cell, totalQuads, made);
    for (int li=0; li<=10; li++) if (!idxs[li].empty()) std::fprintf(stderr, " L%d:%zu", li, idxs[li].size()/6);
    std::fprintf(stderr, "\n");
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
    computeSceneMVP(obj.pc, cx, cz);
    A.objs.push_back(std::move(obj));
    std::fprintf(stderr, "天空穹顶 %s 顶点=%zu 三角形=%u 半径=%.0f\n", mesh.name.c_str(), verts.size()/11, obj.idxCount/3, S);
}

static void loadSceneAssets(App& A, const std::string& scoPath, const std::string& propsPath,
                            const std::string& commonRes, float& sceneCX, float& sceneCZ,
                            std::set<std::string>& needDds) {
    // 1. scene_props.txt：spr id -> mesh 名
    std::map<uint32_t, std::string> sprMesh;
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
            sprMesh[idx++] = mesh;
        }
    }
    // 2. SCO 物件
    sco::Scene scene = sco::load(scoPath);
    std::set<std::string> need;
    for (auto& o : scene.objects) if (o.type==0) {
        auto it = sprMesh.find(o.id);
        if (it != sprMesh.end()) need.insert(it->second);
    }
    std::fprintf(stderr, "需要网格 %zu 个\n", need.size());
    // 2.5. 地面规格：游戏根 Data/ground_specs.txt（材质名稍后由 materials.brf 解析成 dds）
    std::filesystem::path gameRoot = std::filesystem::path(scoPath).parent_path().parent_path().parent_path().parent_path();
    std::vector<GroundSpec> gspecs = loadGroundSpecs((gameRoot / "Data" / "ground_specs.txt").string());
    // 3. 扫描 CommonRes 所有 brf，收集需要的网格与材质定义
    int filesOk=0, filesFail=0;
    for (auto& de : std::filesystem::directory_iterator(commonRes)) {
        if (de.path().extension() != ".brf") continue;
        bool meshesDone = need.empty();
        if (!meshesDone && g_meshGroups.size() >= need.size()) {
            meshesDone = true;
            for (auto& n : need) if (!g_meshGroups.count(n)) { meshesDone=false; break; }
        }
        bool matsDone = gspecs.empty();
        if (!matsDone) {
            matsDone = true;
            for (auto& g : gspecs) if (!g_materialDiffuse.count(g.mat)) { matsDone=false; break; }
        }
        if (meshesDone && matsDone) break;
        try {
            brf::BrfFile bf = brf::parse(de.path().string());
            filesOk++;
            for (auto& m : bf.meshes) {
                if (meshIsLodVariant(m.name)) continue;
                std::string base = meshBaseName(m.name);
                if (need.count(base)) g_meshGroups[base].push_back(std::move(m));
            }
            for (auto& mt : bf.materials) if (!g_materialDiffuse.count(mt.name)) g_materialDiffuse[mt.name] = mt.diffuseA;
        } catch (const std::exception&) { filesFail++; }
    }
    std::fprintf(stderr, "扫描 brf：成功 %d 失败 %d，收集到基础网格 %zu / %zu\n", filesOk, filesFail, g_meshGroups.size(), need.size());
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
    float tcx=0, tcz=0;
    if (hasTerrain) { tcx = terr.width*2.0f/2; tcz = terr.height*2.0f/2; }
    sceneCX=(mnx+mxx+tcx)/3; sceneCZ=(mnz+mxz+tcz)/3;
    std::fprintf(stderr, "中心: props=(%.1f,%.1f) terrain=(%.1f,%.1f) -> (%.1f,%.1f)\n",
                 (mnx+mxx)/2, (mnz+mxz)/2, tcx, tcz, sceneCX, sceneCZ);
    // 5. 生成物件
    size_t made=0;
    for (auto& o : scene.objects) {
        if (o.type!=0) continue;
        auto it = sprMesh.find(o.id);
        if (it==sprMesh.end()) continue;
        auto git = g_meshGroups.find(it->second);
        if (git==g_meshGroups.end()) { std::fprintf(stderr, "缺网格 %s (spr %u)\n", it->second.c_str(), o.id); continue; }
        for (auto& piece : git->second) {
            auto dit2 = g_materialDiffuse.find(piece.material);
            if (dit2 != g_materialDiffuse.end() && !dit2->second.empty() && dit2->second != "none") needDds.insert(dit2->second);
            makeSceneObj(A, piece, o, sceneCX, sceneCZ);
        }
        made++;
    }
    std::fprintf(stderr, "场景物件实例 %zu 个已建缓冲\n", made);
    if (hasTerrain) {
        makeGroundObj(A, terr, gspecs, sceneCX, sceneCZ, needDds);
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

static void recordCmd(App& A) {
    VK_DEV_FN(A.dev, vkAllocateCommandBuffers); VK_DEV_FN(A.dev, vkBeginCommandBuffer);
    VK_DEV_FN(A.dev, vkCmdBeginRenderPass); VK_DEV_FN(A.dev, vkCmdBindPipeline);
    VK_DEV_FN(A.dev, vkCmdBindDescriptorSets);
    VK_DEV_FN(A.dev, vkCmdPushConstants); VK_DEV_FN(A.dev, vkCmdBindVertexBuffers);
    VK_DEV_FN(A.dev, vkCmdBindIndexBuffer); VK_DEV_FN(A.dev, vkCmdDrawIndexed);
    VK_DEV_FN(A.dev, vkCmdEndRenderPass); VK_DEV_FN(A.dev, vkEndCommandBuffer);
    VkCommandBufferAllocateInfo ca{}; ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool=A.cmdPool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=(uint32_t)A.images.size();
    A.cmds.resize(A.images.size());
    CHECK(vkAllocateCommandBuffers(A.dev, &ca, A.cmds.data()), "allocCmds");
    VkClearValue cv[2]{}; cv[0].color={{0.47f,0.64f,1.0f,1}}; cv[1].depthStencil={1.0f,0};
    for (size_t i=0;i<A.cmds.size();i++) {
        VkCommandBufferBeginInfo bb{}; bb.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(A.cmds[i], &bb);
        VkRenderPassBeginInfo rp{}; rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass=A.rp; rp.framebuffer=A.fbs[i]; rp.renderArea={{0,0},A.extent};
        rp.clearValueCount=2; rp.pClearValues=cv;
        vkCmdBeginRenderPass(A.cmds[i], &rp, VK_SUBPASS_CONTENTS_INLINE);
        // 天空先画（无深度），随后场景正常深度覆盖
        vkCmdBindPipeline(A.cmds[i], VK_PIPELINE_BIND_POINT_GRAPHICS, A.pipeSky);
        for (auto& o : A.objs) {
            if (!o.sky) continue;
            vkCmdBindDescriptorSets(A.cmds[i], VK_PIPELINE_BIND_POINT_GRAPHICS, A.pipeLayout, 0, 1, &o.descSet, 0, nullptr);
            vkCmdPushConstants(A.cmds[i], A.pipeLayout, VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96, o.pc.data());
            VkDeviceSize off=0;
            vkCmdBindVertexBuffers(A.cmds[i], 0, 1, &o.vb, &off);
            vkCmdBindIndexBuffer(A.cmds[i], o.ib, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(A.cmds[i], o.idxCount, 1, 0, 0, 0);
        }
        vkCmdBindPipeline(A.cmds[i], VK_PIPELINE_BIND_POINT_GRAPHICS, A.pipe);
        for (auto& o : A.objs) {
            if (o.sky) continue;
            vkCmdBindDescriptorSets(A.cmds[i], VK_PIPELINE_BIND_POINT_GRAPHICS, A.pipeLayout, 0, 1, &o.descSet, 0, nullptr);
            vkCmdPushConstants(A.cmds[i], A.pipeLayout, VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96, o.pc.data());
            VkDeviceSize off=0;
            vkCmdBindVertexBuffers(A.cmds[i], 0, 1, &o.vb, &off);
            vkCmdBindIndexBuffer(A.cmds[i], o.ib, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(A.cmds[i], o.idxCount, 1, 0, 0, 0);
        }
        vkCmdEndRenderPass(A.cmds[i]);
        vkEndCommandBuffer(A.cmds[i]);
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0); setvbuf(stderr, nullptr, _IONBF, 0);
    if (argc < 5) { std::printf("usage: vk_scene <scene.sco> <scene_props.txt> <CommonRes目录> <Textures目录>\n"); return 1; }
    std::fprintf(stderr, "A: 解析场景 %s\n", argv[1]);
    App A; A.hInst = GetModuleHandleA(nullptr);
    WNDCLASSA wc{}; wc.lpfnWndProc=wndProc; wc.hInstance=A.hInst; wc.lpszClassName="vkSceneWnd";
    RegisterClassA(&wc);
    A.hwnd = CreateWindowA("vkSceneWnd", "M&B 原版场景自研渲染", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                          200, 100, 1280, 720, nullptr, nullptr, A.hInst, nullptr);
    std::fprintf(stderr, "C: initVk\n");
    initVk(A);
    float sceneCX=0, sceneCZ=0;
    std::set<std::string> needDds;
    std::fprintf(stderr, "E: 装配场景物件\n");
    loadSceneAssets(A, argv[1], argv[2], argv[3], sceneCX, sceneCZ, needDds);
    std::vector<std::string> ddsNames(needDds.begin(), needDds.end());
    if (ddsNames.empty()) ddsNames.push_back("shields");
    std::fprintf(stderr, "C2: 加载 %zu 张贴图\n", ddsNames.size());
    initTextures(A, argv[4], ddsNames);
    std::fprintf(stderr, "D: initPipeline\n");
    initPipeline(A);
    for (auto& o : A.objs) {
        auto ti = A.texIndex.find(o.diffName);
        o.texSlot = (ti != A.texIndex.end()) ? ti->second : 0;
        o.descSet = A.texSets[o.texSlot];
    }
    std::fprintf(stderr, "F: recordCmd\n");
    recordCmd(A);

    VK_DEV_FN(A.dev, vkAcquireNextImageKHR); VK_DEV_FN(A.dev, vkQueueSubmit); VK_DEV_FN(A.dev, vkQueuePresentKHR);
    VK_DEV_FN(A.dev, vkCreateFence); VK_DEV_FN(A.dev, vkWaitForFences); VK_DEV_FN(A.dev, vkResetFences); VK_DEV_FN(A.dev, vkResetCommandBuffer);
    A.fences.resize(A.images.size());
    for (auto& f : A.fences) {
        VkFenceCreateInfo fi{}; fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(A.dev, &fi, nullptr, &f);
    }
    std::fprintf(stderr, "G: 进入主循环\n");
    std::printf("窗口已创建，ESC 退出。\n");
    MSG msg;
    uint32_t frame = 0;
    while (true) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        uint32_t img=0;
        VkResult ar = vkAcquireNextImageKHR(A.dev, A.swap, UINT64_MAX, VK_NULL_HANDLE, A.fences[frame%A.fences.size()], &img);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { std::printf("acquire=%d\n", ar); continue; }
        vkWaitForFences(A.dev, 1, &A.fences[frame%A.fences.size()], VK_TRUE, UINT64_MAX);
        vkResetFences(A.dev, 1, &A.fences[frame%A.fences.size()]);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{}; si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount=1; si.pCommandBuffers=&A.cmds[img]; si.waitSemaphoreCount=0; si.pWaitDstStageMask=&ws;
        vkQueueSubmit(A.queue, 1, &si, A.fences[frame%A.fences.size()]);
        VkPresentInfoKHR pi{}; pi.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.swapchainCount=1; pi.pSwapchains=&A.swap; pi.pImageIndices=&img;
        vkQueuePresentKHR(A.queue, &pi);
        frame++;
        Sleep(1);
    }
}
