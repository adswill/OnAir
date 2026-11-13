// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Vulkan compute LDPC decoder (Linux: NVIDIA, AMD, Intel and any other Vulkan 1.0 driver; no CUDA). The kernels are the Metal ones ported to
// GLSL (core/src/dsp/vulkan/*.comp) and compiled once to SPIR-V that is committed (vulkan/ldpc_spirv.h, tools/gen_vulkan_spirv.sh), so the
// build needs neither a shader compiler nor the Vulkan SDK: the loader (libvulkan.so.1) is opened at run time and the few declarations used
// are in vulkan/vk_min.h. Without a loader, a driver, a device with a compute queue and the limits the kernels need, or when the self-test
// fails, the decoder reports itself unavailable (with one log line) and the CPU decoder is used.
//
// Environment: DECT2_NOGPU or DECT2_VULKAN=0 force the CPU decoder; DECT2_VULKAN=1 also accepts a CPU implementation of Vulkan (lavapipe,
// SwiftShader: slower than the CPU decoder, skipped otherwise; for tests); DECT2_GPU_ADAPTER=<index> picks a device by its Vulkan index.
#include "dect2/dsp/gpu_ldpc.h"
#include "dect2/common/stable_random.h"
#include "dect2/t2/t2fec.h"
#include "vulkan/ldpc_spirv.h"
#include "vulkan/vk_min.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dect2 {

namespace {

using namespace vkm;

constexpr int kChunk = 64;                         // blocks per dispatch (also bounded by the device's buffer range)
constexpr uint64_t kBudgetBytes = 640ull << 20;    // device memory the batch buffers of all codes together may keep
constexpr uint64_t kFenceTimeoutNs = 30ull * 1000000000ull;

void* openLoader() {
#ifdef _WIN32
    return (void*)LoadLibraryA("vulkan-1.dll");
#elif defined(__APPLE__)
    for (const char* n : {"libvulkan.1.dylib", "libMoltenVK.dylib"}) if (void* h = dlopen(n, RTLD_NOW | RTLD_LOCAL)) return h;
    return nullptr;
#else
    // the plain names first (the dynamic linker's search path), then the usual distribution directories for a loader outside it (an AppImage or
    // a Flatpak sandbox changes the search path; a 64-bit Debian, Fedora or Arch layout)
    for (const char* n : {"libvulkan.so.1", "libvulkan.so", "/usr/lib/x86_64-linux-gnu/libvulkan.so.1", "/usr/lib/aarch64-linux-gnu/libvulkan.so.1",
                          "/usr/lib64/libvulkan.so.1", "/usr/lib/libvulkan.so.1", "/lib/x86_64-linux-gnu/libvulkan.so.1", "/usr/lib/x86_64-linux-gnu/GL/default/lib/libvulkan.so.1"})
        if (void* h = dlopen(n, RTLD_NOW | RTLD_LOCAL)) return h;
    return nullptr;
#endif
}
void* loaderSym(void* lib, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

// the push constants of the two kernels (std430 layout of the GLSL blocks)
struct T2Push { int32_t k, n, q, G, maxIter; float alpha; uint32_t msgPer, postPer; };
struct QcPush { int32_t z, rows, cols, punct, sent, info, maxIter, gu0, gd0, gu1, gd1; float alpha, clip; uint32_t msgPer; };

// The DTMB arithmetic on the CPU, operation for operation as in ldpc_qc.comp (and dtmb_ldpc.cpp): the reference of the self-test.
void qcReference(const QcLdpc& c, const float* llr, int maxIter, float alpha, std::vector<uint8_t>& info, bool& ok, int& iters) {
    const int z = c.z;
    std::vector<float> lv((size_t)c.blockCols * z), msg(c.col.size() * (size_t)z, 0.f);
    for (int v = 0; v < c.blockCols * z; v++) lv[(size_t)v] = v < c.punctured ? 0.f : std::max(-c.clip, std::min(c.clip, llr[v - c.punctured]));
    auto checks = [&] {
        int w = 0;
        for (int r = 0; r < c.blockRows; r++)
            for (int m = 0; m < z; m++) {
                int par = 0;
                for (int e = c.rowStart[(size_t)r]; e < c.rowStart[(size_t)r + 1]; e++) { int sh = m + c.shift[(size_t)e]; if (sh >= z) sh -= z; par ^= lv[(size_t)(c.col[(size_t)e] * z + sh)] < 0.f; }
                w += par;
            }
        return w;
    };
    ok = false; iters = 0;
    if (checks() == 0) { ok = true; iters = 1; }
    else for (int iter = 1; iter <= maxIter; iter++) {
        for (int r = 0; r < c.blockRows; r++) {
            const int e0 = c.rowStart[(size_t)r], e1 = c.rowStart[(size_t)r + 1];
            for (int m = 0; m < z; m++) {   // the checks of a block row touch distinct variables: their order does not matter
                float min1 = 0.f, min2 = 3.4028235e38f;
                uint32_t sg = 0;
                for (int e = e0; e < e1; e++) {
                    int sh = m + c.shift[(size_t)e]; if (sh >= z) sh -= z;
                    const float q = lv[(size_t)(c.col[(size_t)e] * z + sh)] - msg[(size_t)e * z + m];
                    const float a = std::fabs(q);
                    if (e == e0) min1 = a; else { min2 = std::min(min2, std::max(a, min1)); min1 = std::min(min1, a); }
                    uint32_t u; std::memcpy(&u, &q, 4); sg ^= u & 0x80000000u;
                }
                for (int e = e0; e < e1; e++) {
                    int sh = m + c.shift[(size_t)e]; if (sh >= z) sh -= z;
                    const size_t vi = (size_t)(c.col[(size_t)e] * z + sh);
                    const float q = lv[vi] - msg[(size_t)e * z + m];
                    const float a = std::fabs(q);
                    const float mg = (a == min1 ? min2 : min1) * alpha;
                    uint32_t um, uq; std::memcpy(&um, &mg, 4); std::memcpy(&uq, &q, 4);
                    um |= sg ^ (uq & 0x80000000u);
                    float v; std::memcpy(&v, &um, 4);
                    msg[(size_t)e * z + m] = v;
                    const float t = q + v;
                    lv[vi] = std::max(-c.clip, std::min(c.clip, t));
                }
            }
        }
        const int w = checks();
        iters = iter;
        if (w == 0) { ok = true; break; }
        const int gu = iter == c.giveUpIter[0] ? c.giveUpDiv[0] : iter == c.giveUpIter[1] ? c.giveUpDiv[1] : 0;
        if (gu > 0 && w > c.blockRows * z / gu) break;
    }
    info.resize((size_t)c.infoBits());
    for (int v = 0; v < c.infoBits(); v++) info[(size_t)v] = lv[(size_t)(c.blockRows * z + v)] < 0.f;
}

} // namespace

struct GpuLdpc::Impl {
    void* lib = nullptr;
    PFN_GetInstanceProcAddr gipa = nullptr;
    Instance inst = nullptr;
    PhysicalDevice phys = nullptr;
    Device dev = nullptr;
    Queue queue = nullptr;
    uint32_t family = 0;
    PhysicalDeviceProperties props{};
    PhysicalDeviceMemoryProperties memProps{};
    std::string name = "none";
    std::string reason = "the GPU decoder was not started";   // why it is not usable, for the Settings line
    std::string deviceLog;                                      // one entry per Vulkan device seen, with why it was skipped
    void note(const std::string& r) { reason = r; fprintf(stderr, "GpuLdpc: %s, using the CPU decoder\n", r.c_str()); }
    std::mutex mu;                     // every use of the queue and the buffers
    std::atomic<bool> ready{false};    // the T2/S2 kernel works (cleared for good when the device is lost)
    std::atomic<bool> qcReady{false};  // the DTMB kernel works
    uint64_t tick = 0;

    CommandPool pool = 0;
    CommandBuffer cmd = nullptr;
    Fence fence = 0;
    DescriptorSetLayout t2Dsl = 0, qcDsl = 0;
    PipelineLayout t2Layout = 0, qcLayout = 0;
    ShaderModule t2Module = 0, qcModule = 0;
    Pipeline t2Pipe = 0;
    std::map<std::pair<int, int>, Pipeline> qcPipes;   // per (z, shared floats)

#define VKF(n) PFN_##n n = nullptr;
    VKF(DestroyInstance) VKF(EnumeratePhysicalDevices) VKF(GetPhysicalDeviceProperties) VKF(GetPhysicalDeviceQueueFamilyProperties)
    VKF(GetPhysicalDeviceMemoryProperties) VKF(CreateDevice) VKF(GetDeviceProcAddr) VKF(EnumerateDeviceExtensionProperties)
    VKF(DestroyDevice) VKF(GetDeviceQueue) VKF(DeviceWaitIdle) VKF(CreateBuffer) VKF(DestroyBuffer) VKF(GetBufferMemoryRequirements)
    VKF(AllocateMemory) VKF(FreeMemory) VKF(BindBufferMemory) VKF(MapMemory) VKF(UnmapMemory) VKF(CreateShaderModule) VKF(DestroyShaderModule)
    VKF(CreateDescriptorSetLayout) VKF(DestroyDescriptorSetLayout) VKF(CreatePipelineLayout) VKF(DestroyPipelineLayout)
    VKF(CreateComputePipelines) VKF(DestroyPipeline) VKF(CreateDescriptorPool) VKF(DestroyDescriptorPool) VKF(AllocateDescriptorSets)
    VKF(UpdateDescriptorSets) VKF(CreateCommandPool) VKF(DestroyCommandPool) VKF(AllocateCommandBuffers) VKF(BeginCommandBuffer)
    VKF(EndCommandBuffer) VKF(ResetCommandBuffer) VKF(CmdBindPipeline) VKF(CmdBindDescriptorSets) VKF(CmdPushConstants) VKF(CmdDispatch)
    VKF(CmdPipelineBarrier) VKF(CmdCopyBuffer) VKF(QueueSubmit) VKF(CreateFence) VKF(DestroyFence) VKF(WaitForFences) VKF(ResetFences)
#undef VKF

    struct Buf {
        Buffer b = 0;
        DeviceMemory m = 0;
        void* map = nullptr;   // host-visible buffers stay mapped
        uint64_t size = 0;
    };

    struct T2State {   // one LDPC code (DVB-T2 / DVB-S2)
        bool ok = true;
        uint32_t msgPer = 0, postPer = 0;
        Buf lStart, cGrp, cSh, mBase, cRank, layerR;   // device-local tables
        int cap = 0;
        uint64_t bytes = 0, used = 0;
        Buf post, msg, llr, hard, info;                // batch buffers (post/msg device-local, the others host-visible)
        DescriptorPool dpool = 0;
        DescriptorSet set = 0;
    };
    std::map<const LdpcCode*, T2State> codes;

    struct QcState {
        Buf rowStart, col, shift;
        uint32_t msgPer = 0;
        int cap = 0;
        uint64_t bytes = 0, used = 0;
        Buf msg, llr, info, res;
        DescriptorPool dpool = 0;
        DescriptorSet set = 0;
    };
    std::map<std::vector<int>, QcState> qcCodes;

    // ---- setup
    bool loadInstance() {
        lib = openLoader();
        if (!lib) { note("no Vulkan loader found (libvulkan.so.1 is not installed: install your distribution's libvulkan1 / vulkan-icd-loader package)"); return false; }
        gipa = (PFN_GetInstanceProcAddr)loaderSym(lib, "vkGetInstanceProcAddr");
        if (!gipa) { note("the Vulkan loader has no vkGetInstanceProcAddr"); return false; }
        auto createInstance = (PFN_CreateInstance)gipa(nullptr, "vkCreateInstance");
        auto enumExt = (PFN_EnumerateInstanceExtensionProperties)gipa(nullptr, "vkEnumerateInstanceExtensionProperties");
        if (!createInstance) return false;
        // MoltenVK and other layered implementations are only listed with the portability enumeration extension
        bool portability = false;
        if (enumExt) {
            uint32_t n = 0;
            if (enumExt(nullptr, &n, nullptr) == SUCCESS && n) {
                std::vector<ExtensionProperties> e(n);
                if (enumExt(nullptr, &n, e.data()) == SUCCESS)
                    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, "VK_KHR_portability_enumeration")) portability = true;
            }
        }
        const char* exts[] = {"VK_KHR_portability_enumeration"};
        ApplicationInfo app{ST_APPLICATION_INFO, nullptr, "OnAir", 1, "OnAir", 1, API_VERSION_1_0};
        InstanceCreateInfo ci{ST_INSTANCE_CREATE_INFO, nullptr, portability ? (Flags)INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0u, &app, 0, nullptr,
                              portability ? 1u : 0u, portability ? exts : nullptr};
        const Result r = createInstance(&ci, nullptr, &inst);
        if (r != SUCCESS) { note("vkCreateInstance failed (" + std::to_string((int)r) + ((int)r == -9 ? ": no Vulkan driver (ICD) could be loaded; for an AMD GPU install mesa-vulkan-drivers" : "") + ")"); inst = nullptr; return false; }
#define VKI(n) n = (PFN_##n)gipa(inst, "vk" #n); if (!n) return false;
        VKI(DestroyInstance) VKI(EnumeratePhysicalDevices) VKI(GetPhysicalDeviceProperties) VKI(GetPhysicalDeviceQueueFamilyProperties)
        VKI(GetPhysicalDeviceMemoryProperties) VKI(CreateDevice) VKI(GetDeviceProcAddr) VKI(EnumerateDeviceExtensionProperties)
#undef VKI
        return true;
    }

    bool pickDevice(bool allowCpu) {
        uint32_t n = 0;
        if (EnumeratePhysicalDevices(inst, &n, nullptr) != SUCCESS || n == 0) { note("the Vulkan loader lists no device (no Vulkan driver for this GPU; for AMD install mesa-vulkan-drivers, and check /dev/dri access)"); return false; }
        std::vector<PhysicalDevice> pds(n);
        if (EnumeratePhysicalDevices(inst, &n, pds.data()) < 0) return false;
        const char* fe = getenv("DECT2_GPU_ADAPTER");
        const int forced = fe ? atoi(fe) : -1;
        int bestScore = -1;
        for (uint32_t i = 0; i < n; i++) {
            PhysicalDeviceProperties p{};
            GetPhysicalDeviceProperties(pds[i], &p);
            uint32_t nf = 0;
            GetPhysicalDeviceQueueFamilyProperties(pds[i], &nf, nullptr);
            std::vector<QueueFamilyProperties> qf(nf);
            GetPhysicalDeviceQueueFamilyProperties(pds[i], &nf, qf.data());
            int fam = -1;
            for (uint32_t f = 0; f < nf; f++) if ((qf[f].queueFlags & QUEUE_COMPUTE_BIT) && qf[f].queueCount > 0) { fam = (int)f; break; }
            const PhysicalDeviceLimits& L = p.limits;
            const bool limitsOk = L.maxComputeWorkGroupInvocations >= 360 && L.maxComputeWorkGroupSize[0] >= 360 && L.maxPushConstantsSize >= sizeof(QcPush) &&
                                  L.maxPerStageDescriptorStorageBuffers >= 11 && L.maxBoundDescriptorSets >= 1;
            const bool cpu = p.deviceType == PHYSICAL_DEVICE_TYPE_CPU;
            const char* why = fam < 0 ? "no compute queue" : !limitsOk ? "limits too small" : (cpu && !allowCpu) ? "a CPU implementation (DECT2_VULKAN=1 to use it)" : nullptr;
            fprintf(stderr, "GpuLdpc: Vulkan device %u: %s (type %u)%s%s\n", i, p.deviceName, p.deviceType, why ? ": skipped, " : "", why ? why : "");
            if (why) deviceLog += (deviceLog.empty() ? "" : "; ") + std::string(p.deviceName) + ": " + why;
            if (why && forced != (int)i) continue;
            if (fam < 0 || !limitsOk) continue;
            static const int typeScore[5] = {1, 3, 4, 2, 0};   // other, integrated, discrete, virtual, CPU
            int score = forced == (int)i ? 100 : typeScore[std::min<uint32_t>(p.deviceType, 4)];
            if (forced >= 0 && forced != (int)i) score = -1;
            if (score > bestScore) { bestScore = score; phys = pds[i]; props = p; family = (uint32_t)fam; }
        }
        if (!phys) { note("no usable Vulkan device (" + deviceLog + ")"); return false; }
        GetPhysicalDeviceMemoryProperties(phys, &memProps);
        name = props.deviceName;
        return true;
    }

    bool createDevice() {
        const float prio = 1.f;
        DeviceQueueCreateInfo qi{ST_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &prio};
        // a portability implementation (MoltenVK) requires its subset extension to be enabled
        std::vector<const char*> exts;
        uint32_t ne = 0;
        if (EnumerateDeviceExtensionProperties(phys, nullptr, &ne, nullptr) == SUCCESS && ne) {
            std::vector<ExtensionProperties> e(ne);
            if (EnumerateDeviceExtensionProperties(phys, nullptr, &ne, e.data()) == SUCCESS)
                for (uint32_t i = 0; i < ne; i++) if (!strcmp(e[i].extensionName, "VK_KHR_portability_subset")) exts.push_back("VK_KHR_portability_subset");
        }
        DeviceCreateInfo dci{ST_DEVICE_CREATE_INFO, nullptr, 0, 1, &qi, 0, nullptr, (uint32_t)exts.size(), exts.empty() ? nullptr : exts.data(), nullptr};
        const Result r = CreateDevice(phys, &dci, nullptr, &dev);
        if (r != SUCCESS) { note("vkCreateDevice failed on \"" + name + "\" (" + std::to_string((int)r) + ")"); dev = nullptr; return false; }
#define VKD(n) n = (PFN_##n)GetDeviceProcAddr(dev, "vk" #n); if (!n) { note("the driver lacks Vulkan entry point vk" #n); return false; }
        VKD(DestroyDevice) VKD(GetDeviceQueue) VKD(DeviceWaitIdle) VKD(CreateBuffer) VKD(DestroyBuffer) VKD(GetBufferMemoryRequirements)
        VKD(AllocateMemory) VKD(FreeMemory) VKD(BindBufferMemory) VKD(MapMemory) VKD(UnmapMemory) VKD(CreateShaderModule) VKD(DestroyShaderModule)
        VKD(CreateDescriptorSetLayout) VKD(DestroyDescriptorSetLayout) VKD(CreatePipelineLayout) VKD(DestroyPipelineLayout)
        VKD(CreateComputePipelines) VKD(DestroyPipeline) VKD(CreateDescriptorPool) VKD(DestroyDescriptorPool) VKD(AllocateDescriptorSets)
        VKD(UpdateDescriptorSets) VKD(CreateCommandPool) VKD(DestroyCommandPool) VKD(AllocateCommandBuffers) VKD(BeginCommandBuffer)
        VKD(EndCommandBuffer) VKD(ResetCommandBuffer) VKD(CmdBindPipeline) VKD(CmdBindDescriptorSets) VKD(CmdPushConstants) VKD(CmdDispatch)
        VKD(CmdPipelineBarrier) VKD(CmdCopyBuffer) VKD(QueueSubmit) VKD(CreateFence) VKD(DestroyFence) VKD(WaitForFences) VKD(ResetFences)
#undef VKD
        GetDeviceQueue(dev, family, 0, &queue);
        CommandPoolCreateInfo pci{ST_COMMAND_POOL_CREATE_INFO, nullptr, COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
        if (CreateCommandPool(dev, &pci, nullptr, &pool) != SUCCESS) return false;
        CommandBufferAllocateInfo cai{ST_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        if (AllocateCommandBuffers(dev, &cai, &cmd) != SUCCESS) return false;
        FenceCreateInfo fci{ST_FENCE_CREATE_INFO, nullptr, 0};
        return CreateFence(dev, &fci, nullptr, &fence) == SUCCESS;
    }

    bool layout(uint32_t nBindings, uint32_t pushBytes, DescriptorSetLayout& dsl, PipelineLayout& pl) {
        std::vector<DescriptorSetLayoutBinding> b(nBindings);
        for (uint32_t i = 0; i < nBindings; i++) b[i] = {i, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_COMPUTE_BIT, nullptr};
        DescriptorSetLayoutCreateInfo dci{ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, nBindings, b.data()};
        if (CreateDescriptorSetLayout(dev, &dci, nullptr, &dsl) != SUCCESS) return false;
        PushConstantRange pr{SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
        PipelineLayoutCreateInfo pci{ST_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &dsl, 1, &pr};
        return CreatePipelineLayout(dev, &pci, nullptr, &pl) == SUCCESS;
    }
    bool makeModule(const uint32_t* words, size_t bytes, ShaderModule& m) {
        ShaderModuleCreateInfo ci{ST_SHADER_MODULE_CREATE_INFO, nullptr, 0, bytes, words};
        return CreateShaderModule(dev, &ci, nullptr, &m) == SUCCESS;
    }
    Pipeline pipeline(ShaderModule m, PipelineLayout pl, const SpecializationInfo* spec) {
        ComputePipelineCreateInfo ci{ST_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
                                     {ST_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, SHADER_STAGE_COMPUTE_BIT, m, "main", spec}, pl, 0, -1};
        Pipeline p = 0;
        const Result r = CreateComputePipelines(dev, 0, 1, &ci, nullptr, &p);
        if (r != SUCCESS) { fprintf(stderr, "GpuLdpc: Vulkan pipeline creation failed (%d)\n", (int)r); return 0; }
        return p;
    }
    Pipeline qcPipeline(int z, int lvFloats) {
        auto it = qcPipes.find({z, lvFloats});
        if (it != qcPipes.end()) return it->second;
        const int32_t data[2] = {z, lvFloats};
        const SpecializationMapEntry me[2] = {{0, 0, 4}, {1, 4, 4}};
        SpecializationInfo si{2, me, sizeof data, data};
        Pipeline p = pipeline(qcModule, qcLayout, &si);
        qcPipes[{z, lvFloats}] = p;   // also a failure (0): not tried again
        return p;
    }

    // ---- memory
    int memType(uint32_t bits, Flags want, Flags prefer) const {
        int best = -1;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if (!(bits & (1u << i)) || (memProps.memoryTypes[i].propertyFlags & want) != want) continue;
            if (best < 0 || ((memProps.memoryTypes[i].propertyFlags & prefer) == prefer && (memProps.memoryTypes[(size_t)best].propertyFlags & prefer) != prefer)) best = (int)i;
        }
        return best;
    }
    // host: host-visible and coherent (mapped for good), else device-local
    bool alloc(Buf& b, uint64_t bytes, bool host, Flags usage = BUFFER_USAGE_STORAGE_BUFFER_BIT | BUFFER_USAGE_TRANSFER_DST_BIT) {
        bytes = std::max<uint64_t>((bytes + 3) & ~3ull, 4);
        BufferCreateInfo ci{ST_BUFFER_CREATE_INFO, nullptr, 0, bytes, usage, SHARING_MODE_EXCLUSIVE, 0, nullptr};
        if (CreateBuffer(dev, &ci, nullptr, &b.b) != SUCCESS) { b.b = 0; return false; }
        MemoryRequirements mr{};
        GetBufferMemoryRequirements(dev, b.b, &mr);
        const int t = host ? memType(mr.memoryTypeBits, MEMORY_PROPERTY_HOST_VISIBLE_BIT | MEMORY_PROPERTY_HOST_COHERENT_BIT, MEMORY_PROPERTY_HOST_CACHED_BIT)
                           : memType(mr.memoryTypeBits, MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        const int t2 = (t < 0 && !host) ? memType(mr.memoryTypeBits, 0, 0) : t;   // no device-local type for it (some integrated GPUs): any type
        if (t2 < 0) { free(b); return false; }
        MemoryAllocateInfo ai{ST_MEMORY_ALLOCATE_INFO, nullptr, mr.size, (uint32_t)t2};
        if (AllocateMemory(dev, &ai, nullptr, &b.m) != SUCCESS) { b.m = 0; free(b); return false; }
        if (BindBufferMemory(dev, b.b, b.m, 0) != SUCCESS) { free(b); return false; }
        if (host && MapMemory(dev, b.m, 0, WHOLE_SIZE, 0, &b.map) != SUCCESS) { b.map = nullptr; free(b); return false; }
        b.size = bytes;
        return true;
    }
    void free(Buf& b) {
        if (!dev) return;
        if (b.map) UnmapMemory(dev, b.m);
        if (b.b) DestroyBuffer(dev, b.b, nullptr);
        if (b.m) FreeMemory(dev, b.m, nullptr);
        b = Buf{};
    }

    // one command buffer: begin, record, submit, wait. A lost device or a timeout disables the GPU decoder for good.
    bool begin() {
        ResetCommandBuffer(cmd, 0);
        CommandBufferBeginInfo bi{ST_COMMAND_BUFFER_BEGIN_INFO, nullptr, COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
        return BeginCommandBuffer(cmd, &bi) == SUCCESS;
    }
    bool submitWait(const char* where) {
        if (EndCommandBuffer(cmd) != SUCCESS) return fail(where, -1);
        ResetFences(dev, 1, &fence);
        SubmitInfo si{ST_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd, 0, nullptr};
        Result r = QueueSubmit(queue, 1, &si, fence);
        if (r != SUCCESS) return fail(where, r);
        r = WaitForFences(dev, 1, &fence, 1, kFenceTimeoutNs);
        if (r != SUCCESS) return fail(where, r);
        return true;
    }
    bool fail(const char* where, Result r) {
        if (r == ERROR_DEVICE_LOST || r == TIMEOUT || r == -1) {
            const bool wasT2 = ready.exchange(false), wasQc = qcReady.exchange(false);
            if (wasT2 || wasQc)
                fprintf(stderr, "GpuLdpc: the Vulkan device failed (%s, %d), using the CPU decoder from now on\n", where, (int)r);
        }
        return false;
    }
    void barrier(Flags srcStage, Flags srcAccess, Flags dstStage, Flags dstAccess) {
        MemoryBarrier mb{ST_MEMORY_BARRIER, nullptr, srcAccess, dstAccess};
        CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    // a device-local buffer with these contents (through a host-visible staging buffer)
    bool upload(Buf& dst, const void* data, size_t bytes) {
        if (!alloc(dst, bytes, false)) return false;
        Buf st;
        if (!alloc(st, bytes, true, BUFFER_USAGE_TRANSFER_SRC_BIT)) { free(dst); return false; }
        if (bytes) std::memcpy(st.map, data, bytes);
        bool good = begin();
        if (good) {
            BufferCopy bc{0, 0, st.size};
            CmdCopyBuffer(cmd, st.b, dst.b, 1, &bc);
            good = submitWait("upload");
        }
        free(st);
        if (!good) free(dst);
        return good;
    }
    template <class T> bool upload(Buf& dst, const std::vector<T>& v) { return upload(dst, v.data(), v.size() * sizeof(T)); }

    bool descriptorSet(DescriptorPool& dp, DescriptorSet& set, DescriptorSetLayout dsl, uint32_t nBindings) {
        if (dp) return true;
        DescriptorPoolSize ps{DESCRIPTOR_TYPE_STORAGE_BUFFER, nBindings};
        DescriptorPoolCreateInfo pci{ST_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 1, &ps};
        if (CreateDescriptorPool(dev, &pci, nullptr, &dp) != SUCCESS) { dp = 0; return false; }
        DescriptorSetAllocateInfo ai{ST_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, dp, 1, &dsl};
        if (AllocateDescriptorSets(dev, &ai, &set) != SUCCESS) { DestroyDescriptorPool(dev, dp, nullptr); dp = 0; return false; }
        return true;
    }
    void writeSet(DescriptorSet set, std::initializer_list<const Buf*> bufs) {
        std::vector<DescriptorBufferInfo> bi;
        for (const Buf* b : bufs) bi.push_back({b->b, 0, WHOLE_SIZE});
        std::vector<WriteDescriptorSet> w(bi.size());
        for (size_t i = 0; i < bi.size(); i++) w[i] = {ST_WRITE_DESCRIPTOR_SET, nullptr, set, (uint32_t)i, 0, 1, DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi[i], nullptr};
        UpdateDescriptorSets(dev, (uint32_t)w.size(), w.data(), 0, nullptr);
    }

    // ---- batch buffers of all codes within a budget (as the Direct3D 11 backend: a scan meets every code rate one after the other)
    void releaseBatch(T2State& s) { free(s.post); free(s.msg); free(s.llr); free(s.hard); free(s.info); s.cap = 0; s.bytes = 0; }
    void releaseBatch(QcState& s) { free(s.msg); free(s.llr); free(s.info); free(s.res); s.cap = 0; s.bytes = 0; }
    void trim(const void* keep, uint64_t budget) {
        for (;;) {
            uint64_t total = 0, oldestUse = ~0ull;
            T2State* o1 = nullptr;
            QcState* o2 = nullptr;
            for (auto& kv : codes) { total += kv.second.bytes; if (&kv.second != keep && kv.second.bytes && kv.second.used < oldestUse) { oldestUse = kv.second.used; o1 = &kv.second; o2 = nullptr; } }
            for (auto& kv : qcCodes) { total += kv.second.bytes; if (&kv.second != keep && kv.second.bytes && kv.second.used < oldestUse) { oldestUse = kv.second.used; o2 = &kv.second; o1 = nullptr; } }
            if (total <= budget || (!o1 && !o2)) return;
            if (o1) releaseBatch(*o1); else releaseBatch(*o2);
        }
    }

    // blocks per dispatch: every buffer must fit the device's storage buffer range
    int chunkFor(uint64_t largestPerBlock) const {
        const uint64_t range = props.limits.maxStorageBufferRange;
        int c = (int)std::min<uint64_t>(kChunk, largestPerBlock ? range / largestPerBlock : kChunk);
        return std::min<int>(c, (int)std::min<uint32_t>(props.limits.maxComputeWorkGroupCount[0], 65535));
    }

    T2State* t2State(const LdpcCode& c) {
        auto it = codes.find(&c);
        if (it != codes.end()) return &it->second;
        T2State s;
        const auto& L = c.layers();
        std::vector<int> ls(1, 0), cg, cs, rk, lr;
        std::vector<uint32_t> mb;
        uint32_t off = 0;
        for (auto& l : L) {
            int maxRank = 0;
            for (size_t i = 0; i < l.size(); i++) {
                int rank = 0;
                for (size_t k = 0; k < i; k++) if (l[k].group == l[i].group) rank++;
                maxRank = std::max(maxRank, rank);
                cg.push_back(l[i].group); cs.push_back(l[i].shift); rk.push_back(rank);
            }
            lr.push_back(maxRank);
            if (l.size() + 2 > 64) s.ok = false;   // the kernel keeps one increment per edge in a fixed array
            ls.push_back((int)cg.size());
            mb.push_back(off);
            off += (uint32_t)(l.size() + 2) * 360;
        }
        s.msgPer = off;
        s.postPer = (uint32_t)(c.groups() * 360 + c.q() * 361);
        s.ok = s.ok && upload(s.lStart, ls) && upload(s.cGrp, cg) && upload(s.cSh, cs) && upload(s.mBase, mb) && upload(s.cRank, rk) && upload(s.layerR, lr) &&
               descriptorSet(s.dpool, s.set, t2Dsl, 11);
        return &codes.emplace(&c, std::move(s)).first->second;
    }

    bool init() {
        const char* ev = getenv("DECT2_VULKAN");
        if (getenv("DECT2_NOGPU") || (ev && (!strcmp(ev, "0") || !strcmp(ev, "cpu") || !strcmp(ev, "off")))) {
            note("switched off by the environment (DECT2_NOGPU or DECT2_VULKAN=0)");
            return false;
        }
        const bool force = ev && (!strcmp(ev, "1") || !strcmp(ev, "force") || !strcmp(ev, "on"));
        if (!loadInstance() || !pickDevice(force) || !createDevice()) return false;
        if (!layout(11, sizeof(T2Push), t2Dsl, t2Layout) || !layout(7, sizeof(QcPush), qcDsl, qcLayout) ||
            !makeModule(k_ldpc_t2_spv, sizeof k_ldpc_t2_spv, t2Module) || !makeModule(k_ldpc_qc_spv, sizeof k_ldpc_qc_spv, qcModule)) {
            note("Vulkan shader setup failed on \"" + name + "\"");
            return false;
        }
        t2Pipe = pipeline(t2Module, t2Layout, nullptr);
        if (!t2Pipe) { note("Vulkan pipeline creation failed on \"" + name + "\" (the driver rejected the decoder kernel)"); return false; }
        return true;
    }

    ~Impl() {
        if (dev) {
            DeviceWaitIdle(dev);
            for (auto& kv : codes) {
                T2State& s = kv.second;
                releaseBatch(s);
                free(s.lStart); free(s.cGrp); free(s.cSh); free(s.mBase); free(s.cRank); free(s.layerR);
                if (s.dpool) DestroyDescriptorPool(dev, s.dpool, nullptr);
            }
            for (auto& kv : qcCodes) {
                QcState& s = kv.second;
                releaseBatch(s);
                free(s.rowStart); free(s.col); free(s.shift);
                if (s.dpool) DestroyDescriptorPool(dev, s.dpool, nullptr);
            }
            for (auto& kv : qcPipes) if (kv.second) DestroyPipeline(dev, kv.second, nullptr);
            if (t2Pipe) DestroyPipeline(dev, t2Pipe, nullptr);
            if (t2Module) DestroyShaderModule(dev, t2Module, nullptr);
            if (qcModule) DestroyShaderModule(dev, qcModule, nullptr);
            if (t2Layout) DestroyPipelineLayout(dev, t2Layout, nullptr);
            if (qcLayout) DestroyPipelineLayout(dev, qcLayout, nullptr);
            if (t2Dsl) DestroyDescriptorSetLayout(dev, t2Dsl, nullptr);
            if (qcDsl) DestroyDescriptorSetLayout(dev, qcDsl, nullptr);
            if (fence) DestroyFence(dev, fence, nullptr);
            if (pool) DestroyCommandPool(dev, pool, nullptr);
            DestroyDevice(dev, nullptr);
        }
        if (inst && DestroyInstance) DestroyInstance(inst, nullptr);
        // the loader stays loaded: drivers may keep threads that outlive this object at program exit
    }
};

GpuLdpc::GpuLdpc() : p_(new Impl) {
    Impl& d = *p_;
    if (!d.init()) return;
    d.ready = true;
    d.qcReady = true;
    // Self-test: drivers and their shader compilers differ, and a wrong decoder is far worse than a slow one. Known noisy blocks are decoded
    // on the GPU; a wrong result switches the GPU decoder off and the CPU decoder is used.
    if (!selfTest()) {
        d.ready = false;
        d.qcReady = false;
        d.note("the decoder self-test failed on \"" + d.name + "\" (the driver gave a wrong result, so the CPU is used to be safe)");
        return;
    }
    fprintf(stderr, "GpuLdpc: using Vulkan device \"%s\"%s\n", d.name.c_str(), d.qcReady ? "" : " (DTMB on the CPU)");
}

bool GpuLdpc::selfTest() {
    Impl& d = *p_;
    {   // DVB-T2 short frame, rate 2/3: the transmitted bits come back
        PlpFec f; f.shortFrame = true; f.rate = 2; f.mod = 2; f.rotation = false;
        const LdpcCode& c = ldpcFor(f);
        const int n = c.n(), k = c.k(), nb = 4;
        std::mt19937 rng(12345);
        dect2::normal_distribution<float> nd(0.f, 1.f);
        const double sigma = 0.55;
        std::vector<float> llr((size_t)nb * n);
        std::vector<std::vector<uint8_t>> cw((size_t)nb);
        for (int b = 0; b < nb; b++) {
            cw[(size_t)b].resize((size_t)n);
            for (int i = 0; i < k; i++) cw[(size_t)b][(size_t)i] = rng() & 1;
            c.encode(cw[(size_t)b]);
            for (int i = 0; i < n; i++) { const float x = (cw[(size_t)b][(size_t)i] ? -1.f : 1.f) + (float)sigma * nd(rng); llr[(size_t)b * n + i] = 2.f * x / (float)(sigma * sigma); }
        }
        std::vector<uint8_t> hard((size_t)nb * n), ok((size_t)nb);
        std::vector<int> it((size_t)nb);
        if (!decode(c, llr.data(), nb, 50, hard.data(), ok.data(), it.data())) return false;
        for (int b = 0; b < nb; b++) {
            if (!ok[(size_t)b]) return false;
            for (int i = 0; i < n; i++) if (hard[(size_t)b * n + i] != cw[(size_t)b][(size_t)i]) return false;
        }
    }
    {   // a small random quasi-cyclic code (z = 127 as DTMB) against the CPU arithmetic, to the bit; a failure here only moves DTMB to the CPU
        QcLdpc q;
        q.z = 127; q.blockRows = 4; q.blockCols = 12; q.punctured = 3; q.clip = 1000.f;
        q.giveUpIter[0] = 4; q.giveUpDiv[0] = 4;
        std::mt19937 rng(77);
        q.rowStart.push_back(0);
        for (int r = 0; r < q.blockRows; r++) {
            std::vector<int> cols(q.blockCols);
            for (int i = 0; i < q.blockCols; i++) cols[(size_t)i] = i;
            std::shuffle(cols.begin(), cols.end(), rng);
            for (int e = 0; e < 7; e++) { q.col.push_back(cols[(size_t)e]); q.shift.push_back((int)(rng() % 127)); }
            q.rowStart.push_back((int)q.col.size());
        }
        const int sent = q.sentBits(), nb = 3;
        std::vector<float> llr((size_t)nb * sent);
        dect2::normal_distribution<float> nd(0.f, 1.f);
        for (auto& x : llr) x = 2.f * (1.f + 0.9f * nd(rng));
        std::vector<uint8_t> info((size_t)nb * q.infoBits()), ok((size_t)nb);
        std::vector<int> it((size_t)nb);
        bool good = decodeQc(q, llr.data(), nb, 12, info.data(), ok.data(), it.data(), 0.6f);
        for (int b = 0; good && b < nb; b++) {
            std::vector<uint8_t> ri; bool rok; int rit;
            qcReference(q, &llr[(size_t)b * sent], 12, 0.6f, ri, rok, rit);
            good = rok == (bool)ok[(size_t)b] && rit == it[(size_t)b] && std::memcmp(ri.data(), &info[(size_t)b * q.infoBits()], ri.size()) == 0;
        }
        if (!good) { d.qcReady = false; fprintf(stderr, "GpuLdpc: DTMB kernel self-test failed on \"%s\": DTMB decodes on the CPU\n", d.name.c_str()); }
    }
    return d.ready;
}

GpuLdpc::~GpuLdpc() = default;
GpuLdpc& GpuLdpc::instance() { static GpuLdpc g; return g; }
bool GpuLdpc::available() const { return p_->ready; }
const char* GpuLdpc::unavailableReason() const { return p_->ready ? "" : p_->reason.c_str(); }
const char* GpuLdpc::deviceName() const { return p_->name.c_str(); }

bool GpuLdpc::decode(const LdpcCode& code, const float* llr, int nb, int maxIter, uint8_t* hard, uint8_t* ok, int* iters, float alpha, bool /*boxCorr: plain min-sum here*/) {
    if (!available() || code.q() * 360 != code.n() - code.k()) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& d = *p_;
    Impl::T2State* sp = d.t2State(code);
    if (!sp || !sp->ok) return false;
    Impl::T2State& s = *sp;
    const int n = code.n();
    const int chunk = d.chunkFor((uint64_t)std::max<uint32_t>(s.postPer, s.msgPer) * 4);
    if (chunk < 1) return false;
    for (int b0 = 0; b0 < nb; b0 += chunk) {
        const int cnt = std::min(chunk, nb - b0);
        if (s.cap < chunk) {
            d.releaseBatch(s);
            d.trim(&s, kBudgetBytes);
            auto make = [&] {
                return d.alloc(s.post, (uint64_t)chunk * s.postPer * 4, false) && d.alloc(s.msg, (uint64_t)chunk * s.msgPer * 4, false) &&
                       d.alloc(s.llr, (uint64_t)chunk * n * 4, true) && d.alloc(s.hard, (uint64_t)chunk * n * 4, true) && d.alloc(s.info, (uint64_t)chunk * 8, true);
            };
            if (!make()) { d.releaseBatch(s); d.trim(&s, 0); if (!make()) { d.releaseBatch(s); return false; } }   // out of memory: give back the other codes' buffers, once
            s.cap = chunk;
            s.bytes = (uint64_t)chunk * ((uint64_t)s.postPer + s.msgPer + 2ull * n + 2) * 4;
            d.writeSet(s.set, {&s.lStart, &s.cGrp, &s.cSh, &s.mBase, &s.cRank, &s.layerR, &s.post, &s.msg, &s.llr, &s.hard, &s.info});
        }
        s.used = ++d.tick;
        std::memcpy(s.llr.map, llr + (size_t)b0 * n, (size_t)cnt * n * 4);
        const T2Push P = {code.k(), n, code.q(), code.groups(), maxIter, alpha, s.msgPer, s.postPer};
        if (!d.begin()) return false;
        d.CmdBindPipeline(d.cmd, PIPELINE_BIND_POINT_COMPUTE, d.t2Pipe);
        d.CmdBindDescriptorSets(d.cmd, PIPELINE_BIND_POINT_COMPUTE, d.t2Layout, 0, 1, &s.set, 0, nullptr);
        d.CmdPushConstants(d.cmd, d.t2Layout, SHADER_STAGE_COMPUTE_BIT, 0, sizeof P, &P);
        d.CmdDispatch(d.cmd, (uint32_t)cnt, 1, 1);
        d.barrier(PIPELINE_STAGE_COMPUTE_SHADER_BIT, ACCESS_SHADER_WRITE_BIT, PIPELINE_STAGE_HOST_BIT, ACCESS_HOST_READ_BIT);
        if (!d.submitWait("decode")) return false;
        const uint32_t* hw = (const uint32_t*)s.hard.map;
        uint8_t* out = hard + (size_t)b0 * n;
        for (size_t i = 0; i < (size_t)cnt * n; i++) out[i] = (uint8_t)hw[i];
        const int32_t* inf = (const int32_t*)s.info.map;
        for (int i = 0; i < cnt; i++) { ok[b0 + i] = (uint8_t)inf[i * 2]; iters[b0 + i] = inf[i * 2 + 1]; }
    }
    return true;
}

bool GpuLdpc::qcAvailable() const { return p_->qcReady; }

bool GpuLdpc::boxCorrKernels() const { return false; }   // the GLSL kernels run plain normalised min-sum (no shader compiler to rebuild the SPIR-V yet)
bool GpuLdpc::decodeQc(const QcLdpc& code, const float* llr, int nb, int maxIter, uint8_t* info, uint8_t* ok, int* iters, float alpha, bool /*boxCorr: plain min-sum here*/) {
    if (!qcAvailable() || nb <= 0) return qcAvailable() && nb == 0;
    const int z = code.z, rows = code.blockRows, cols = code.blockCols;
    if (z <= 0 || z > 1024 || rows <= 0 || cols <= rows || (int)code.rowStart.size() != rows + 1 || code.col.size() != code.shift.size() || (int)code.col.size() != code.rowStart.back()) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& d = *p_;
    const PhysicalDeviceLimits& L = d.props.limits;
    const uint64_t shared = (uint64_t)cols * z * 4 + 16;   // the totals plus the check counter
    if ((uint32_t)z > L.maxComputeWorkGroupInvocations || (uint32_t)z > L.maxComputeWorkGroupSize[0] || shared > L.maxComputeSharedMemorySize) return false;
    // a block column twice in a block row would make two invocations write one variable
    for (int r = 0; r < rows; r++)
        for (int a = code.rowStart[(size_t)r]; a < code.rowStart[(size_t)r + 1]; a++) {
            if (code.col[(size_t)a] < 0 || code.col[(size_t)a] >= cols || code.shift[(size_t)a] < 0 || code.shift[(size_t)a] >= z) return false;
            for (int b = a + 1; b < code.rowStart[(size_t)r + 1]; b++) if (code.col[(size_t)a] == code.col[(size_t)b]) return false;
        }
    const Pipeline pipe = d.qcPipeline(z, cols * z);
    if (!pipe) return false;
    std::vector<int> key = {z, rows, cols, code.punctured};
    key.insert(key.end(), code.rowStart.begin(), code.rowStart.end());
    key.insert(key.end(), code.col.begin(), code.col.end());
    key.insert(key.end(), code.shift.begin(), code.shift.end());
    auto it = d.qcCodes.find(key);
    if (it == d.qcCodes.end()) {
        Impl::QcState s;
        s.msgPer = (uint32_t)code.col.size() * (uint32_t)z;
        const bool good = d.upload(s.rowStart, code.rowStart) && d.upload(s.col, code.col) && d.upload(s.shift, code.shift) && d.descriptorSet(s.dpool, s.set, d.qcDsl, 7);
        if (!good) { d.free(s.rowStart); d.free(s.col); d.free(s.shift); if (s.dpool) d.DestroyDescriptorPool(d.dev, s.dpool, nullptr); return false; }
        it = d.qcCodes.emplace(key, std::move(s)).first;
    }
    Impl::QcState& s = it->second;
    const int sent = code.sentBits(), infoN = code.infoBits();
    const int chunk = d.chunkFor((uint64_t)s.msgPer * 4);
    if (chunk < 1) return false;
    for (int b0 = 0; b0 < nb; b0 += chunk) {
        const int cnt = std::min(chunk, nb - b0);
        if (s.cap < chunk) {
            d.releaseBatch(s);
            d.trim(&s, kBudgetBytes);
            auto make = [&] {
                return d.alloc(s.msg, (uint64_t)chunk * s.msgPer * 4, false) && d.alloc(s.llr, (uint64_t)chunk * sent * 4, true) &&
                       d.alloc(s.info, (uint64_t)chunk * infoN * 4, true) && d.alloc(s.res, (uint64_t)chunk * 8, true);
            };
            if (!make()) { d.releaseBatch(s); d.trim(&s, 0); if (!make()) { d.releaseBatch(s); return false; } }
            s.cap = chunk;
            s.bytes = (uint64_t)chunk * ((uint64_t)s.msgPer + sent + infoN + 2) * 4;
            d.writeSet(s.set, {&s.rowStart, &s.col, &s.shift, &s.msg, &s.llr, &s.info, &s.res});
        }
        s.used = ++d.tick;
        std::memcpy(s.llr.map, llr + (size_t)b0 * sent, (size_t)cnt * sent * 4);
        const QcPush P = {z, rows, cols, code.punctured, sent, infoN, maxIter, code.giveUpIter[0], code.giveUpDiv[0], code.giveUpIter[1], code.giveUpDiv[1], alpha, code.clip, s.msgPer};
        if (!d.begin()) return false;
        d.CmdBindPipeline(d.cmd, PIPELINE_BIND_POINT_COMPUTE, pipe);
        d.CmdBindDescriptorSets(d.cmd, PIPELINE_BIND_POINT_COMPUTE, d.qcLayout, 0, 1, &s.set, 0, nullptr);
        d.CmdPushConstants(d.cmd, d.qcLayout, SHADER_STAGE_COMPUTE_BIT, 0, sizeof P, &P);
        d.CmdDispatch(d.cmd, (uint32_t)cnt, 1, 1);
        d.barrier(PIPELINE_STAGE_COMPUTE_SHADER_BIT, ACCESS_SHADER_WRITE_BIT, PIPELINE_STAGE_HOST_BIT, ACCESS_HOST_READ_BIT);
        if (!d.submitWait("decodeQc")) return false;
        const uint32_t* iw = (const uint32_t*)s.info.map;
        uint8_t* out = info + (size_t)b0 * infoN;
        for (size_t i = 0; i < (size_t)cnt * infoN; i++) out[i] = (uint8_t)iw[i];
        const int32_t* rs = (const int32_t*)s.res.map;
        for (int i = 0; i < cnt; i++) { ok[b0 + i] = (uint8_t)rs[i * 2]; iters[b0 + i] = rs[i * 2 + 1]; }
    }
    return true;
}

} // namespace dect2
