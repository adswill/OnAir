// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The small part of the Vulkan 1.0 API that the compute LDPC decoder uses, declared by hand so that building needs neither the Vulkan SDK nor
// its headers. Types, values and structure layouts are those of the Vulkan specification (vulkan_core.h); the loader (libvulkan.so.1,
// vulkan-1.dll, libvulkan.1.dylib / MoltenVK) is opened at run time and every entry point is looked up through vkGetInstanceProcAddr /
// vkGetDeviceProcAddr. Private to gpu_ldpc_vulkan.cpp: nothing else should include this.
#pragma once
#include <cstddef>
#include <cstdint>

#if defined(_WIN32) && !defined(_WIN64)
#define DECT2_VKAPI __stdcall
#else
#define DECT2_VKAPI
#endif

namespace dect2 {
namespace vkm {

typedef uint32_t Flags;
typedef uint32_t Bool32;
typedef uint64_t DeviceSize;
typedef int32_t Result;
// dispatchable handles are pointers, the others 64-bit integers on every platform (what the specification's macros expand to on 64-bit)
typedef struct Instance_T* Instance;
typedef struct PhysicalDevice_T* PhysicalDevice;
typedef struct Device_T* Device;
typedef struct Queue_T* Queue;
typedef struct CommandBuffer_T* CommandBuffer;
typedef uint64_t Buffer, DeviceMemory, ShaderModule, PipelineLayout, Pipeline, DescriptorSetLayout, DescriptorPool, DescriptorSet, CommandPool, Fence, PipelineCache;

enum : int32_t {
    SUCCESS = 0, NOT_READY = 1, TIMEOUT = 2, ERROR_OUT_OF_HOST_MEMORY = -1, ERROR_OUT_OF_DEVICE_MEMORY = -2, ERROR_INITIALIZATION_FAILED = -3,
    ERROR_DEVICE_LOST = -4,
};
enum : int32_t {   // VkStructureType
    ST_APPLICATION_INFO = 0, ST_INSTANCE_CREATE_INFO = 1, ST_DEVICE_QUEUE_CREATE_INFO = 2, ST_DEVICE_CREATE_INFO = 3, ST_SUBMIT_INFO = 4,
    ST_MEMORY_ALLOCATE_INFO = 5, ST_FENCE_CREATE_INFO = 8, ST_BUFFER_CREATE_INFO = 12, ST_SHADER_MODULE_CREATE_INFO = 16,
    ST_PIPELINE_SHADER_STAGE_CREATE_INFO = 18, ST_COMPUTE_PIPELINE_CREATE_INFO = 29, ST_PIPELINE_LAYOUT_CREATE_INFO = 30,
    ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32, ST_DESCRIPTOR_POOL_CREATE_INFO = 33, ST_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
    ST_WRITE_DESCRIPTOR_SET = 35, ST_COMMAND_POOL_CREATE_INFO = 39, ST_COMMAND_BUFFER_ALLOCATE_INFO = 40, ST_COMMAND_BUFFER_BEGIN_INFO = 42,
    ST_MEMORY_BARRIER = 46,
};
enum : uint32_t {
    PHYSICAL_DEVICE_TYPE_OTHER = 0, PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1, PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
    PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU = 3, PHYSICAL_DEVICE_TYPE_CPU = 4,
    QUEUE_COMPUTE_BIT = 0x2,
    MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x1, MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x2, MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x4,
    MEMORY_PROPERTY_HOST_CACHED_BIT = 0x8,
    BUFFER_USAGE_TRANSFER_SRC_BIT = 0x1, BUFFER_USAGE_TRANSFER_DST_BIT = 0x2, BUFFER_USAGE_STORAGE_BUFFER_BIT = 0x20,
    SHARING_MODE_EXCLUSIVE = 0,
    DESCRIPTOR_TYPE_STORAGE_BUFFER = 7,
    SHADER_STAGE_COMPUTE_BIT = 0x20,
    PIPELINE_BIND_POINT_COMPUTE = 1,
    COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 0x2,
    COMMAND_BUFFER_LEVEL_PRIMARY = 0,
    COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x1,
    PIPELINE_STAGE_TOP_OF_PIPE_BIT = 0x1, PIPELINE_STAGE_COMPUTE_SHADER_BIT = 0x800, PIPELINE_STAGE_TRANSFER_BIT = 0x1000,
    PIPELINE_STAGE_HOST_BIT = 0x4000,
    ACCESS_SHADER_READ_BIT = 0x20, ACCESS_SHADER_WRITE_BIT = 0x40, ACCESS_TRANSFER_READ_BIT = 0x800, ACCESS_TRANSFER_WRITE_BIT = 0x1000,
    ACCESS_HOST_READ_BIT = 0x2000, ACCESS_HOST_WRITE_BIT = 0x4000,
    INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR = 0x1,
};
constexpr uint32_t API_VERSION_1_0 = (1u << 22);
constexpr uint64_t WHOLE_SIZE = ~0ull;

struct ApplicationInfo { int32_t sType; const void* pNext; const char* pApplicationName; uint32_t applicationVersion; const char* pEngineName; uint32_t engineVersion; uint32_t apiVersion; };
struct InstanceCreateInfo { int32_t sType; const void* pNext; Flags flags; const ApplicationInfo* pApplicationInfo; uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames; uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames; };
struct ExtensionProperties { char extensionName[256]; uint32_t specVersion; };

struct PhysicalDeviceLimits {
    uint32_t maxImageDimension1D, maxImageDimension2D, maxImageDimension3D, maxImageDimensionCube, maxImageArrayLayers, maxTexelBufferElements,
        maxUniformBufferRange, maxStorageBufferRange, maxPushConstantsSize, maxMemoryAllocationCount, maxSamplerAllocationCount;
    DeviceSize bufferImageGranularity, sparseAddressSpaceSize;
    uint32_t maxBoundDescriptorSets, maxPerStageDescriptorSamplers, maxPerStageDescriptorUniformBuffers, maxPerStageDescriptorStorageBuffers,
        maxPerStageDescriptorSampledImages, maxPerStageDescriptorStorageImages, maxPerStageDescriptorInputAttachments, maxPerStageResources,
        maxDescriptorSetSamplers, maxDescriptorSetUniformBuffers, maxDescriptorSetUniformBuffersDynamic, maxDescriptorSetStorageBuffers,
        maxDescriptorSetStorageBuffersDynamic, maxDescriptorSetSampledImages, maxDescriptorSetStorageImages, maxDescriptorSetInputAttachments,
        maxVertexInputAttributes, maxVertexInputBindings, maxVertexInputAttributeOffset, maxVertexInputBindingStride, maxVertexOutputComponents,
        maxTessellationGenerationLevel, maxTessellationPatchSize, maxTessellationControlPerVertexInputComponents,
        maxTessellationControlPerVertexOutputComponents, maxTessellationControlPerPatchOutputComponents, maxTessellationControlTotalOutputComponents,
        maxTessellationEvaluationInputComponents, maxTessellationEvaluationOutputComponents, maxGeometryShaderInvocations,
        maxGeometryInputComponents, maxGeometryOutputComponents, maxGeometryOutputVertices, maxGeometryTotalOutputComponents,
        maxFragmentInputComponents, maxFragmentOutputAttachments, maxFragmentDualSrcAttachments, maxFragmentCombinedOutputResources,
        maxComputeSharedMemorySize, maxComputeWorkGroupCount[3], maxComputeWorkGroupInvocations, maxComputeWorkGroupSize[3],
        subPixelPrecisionBits, subTexelPrecisionBits, mipmapPrecisionBits, maxDrawIndexedIndexValue, maxDrawIndirectCount;
    float maxSamplerLodBias, maxSamplerAnisotropy;
    uint32_t maxViewports, maxViewportDimensions[2];
    float viewportBoundsRange[2];
    uint32_t viewportSubPixelBits;
    size_t minMemoryMapAlignment;
    DeviceSize minTexelBufferOffsetAlignment, minUniformBufferOffsetAlignment, minStorageBufferOffsetAlignment;
    int32_t minTexelOffset;
    uint32_t maxTexelOffset;
    int32_t minTexelGatherOffset;
    uint32_t maxTexelGatherOffset;
    float minInterpolationOffset, maxInterpolationOffset;
    uint32_t subPixelInterpolationOffsetBits, maxFramebufferWidth, maxFramebufferHeight, maxFramebufferLayers;
    Flags framebufferColorSampleCounts, framebufferDepthSampleCounts, framebufferStencilSampleCounts, framebufferNoAttachmentsSampleCounts;
    uint32_t maxColorAttachments;
    Flags sampledImageColorSampleCounts, sampledImageIntegerSampleCounts, sampledImageDepthSampleCounts, sampledImageStencilSampleCounts,
        storageImageSampleCounts;
    uint32_t maxSampleMaskWords;
    Bool32 timestampComputeAndGraphics;
    float timestampPeriod;
    uint32_t maxClipDistances, maxCullDistances, maxCombinedClipAndCullDistances, discreteQueuePriorities;
    float pointSizeRange[2], lineWidthRange[2], pointSizeGranularity, lineWidthGranularity;
    Bool32 strictLines, standardSampleLocations;
    DeviceSize optimalBufferCopyOffsetAlignment, optimalBufferCopyRowPitchAlignment, nonCoherentAtomSize;
};
struct PhysicalDeviceSparseProperties { Bool32 residencyStandard2DBlockShape, residencyStandard2DMultisampleBlockShape, residencyStandard3DBlockShape, residencyAlignedMipSize, residencyNonResidentStrict; };
struct PhysicalDeviceProperties {
    uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    PhysicalDeviceLimits limits;
    PhysicalDeviceSparseProperties sparseProperties;
};
struct Extent3D { uint32_t width, height, depth; };
struct QueueFamilyProperties { Flags queueFlags; uint32_t queueCount, timestampValidBits; Extent3D minImageTransferGranularity; };
struct MemoryType { Flags propertyFlags; uint32_t heapIndex; };
struct MemoryHeap { DeviceSize size; Flags flags; };
struct PhysicalDeviceMemoryProperties { uint32_t memoryTypeCount; MemoryType memoryTypes[32]; uint32_t memoryHeapCount; MemoryHeap memoryHeaps[16]; };

struct DeviceQueueCreateInfo { int32_t sType; const void* pNext; Flags flags; uint32_t queueFamilyIndex, queueCount; const float* pQueuePriorities; };
struct DeviceCreateInfo { int32_t sType; const void* pNext; Flags flags; uint32_t queueCreateInfoCount; const DeviceQueueCreateInfo* pQueueCreateInfos; uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames; uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames; const void* pEnabledFeatures; };
struct BufferCreateInfo { int32_t sType; const void* pNext; Flags flags; DeviceSize size; Flags usage; uint32_t sharingMode; uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices; };
struct MemoryRequirements { DeviceSize size, alignment; uint32_t memoryTypeBits; };
struct MemoryAllocateInfo { int32_t sType; const void* pNext; DeviceSize allocationSize; uint32_t memoryTypeIndex; };
struct ShaderModuleCreateInfo { int32_t sType; const void* pNext; Flags flags; size_t codeSize; const uint32_t* pCode; };
struct DescriptorSetLayoutBinding { uint32_t binding, descriptorType, descriptorCount; Flags stageFlags; const void* pImmutableSamplers; };
struct DescriptorSetLayoutCreateInfo { int32_t sType; const void* pNext; Flags flags; uint32_t bindingCount; const DescriptorSetLayoutBinding* pBindings; };
struct PushConstantRange { Flags stageFlags; uint32_t offset, size; };
struct PipelineLayoutCreateInfo { int32_t sType; const void* pNext; Flags flags; uint32_t setLayoutCount; const DescriptorSetLayout* pSetLayouts; uint32_t pushConstantRangeCount; const PushConstantRange* pPushConstantRanges; };
struct SpecializationMapEntry { uint32_t constantID, offset; size_t size; };
struct SpecializationInfo { uint32_t mapEntryCount; const SpecializationMapEntry* pMapEntries; size_t dataSize; const void* pData; };
struct PipelineShaderStageCreateInfo { int32_t sType; const void* pNext; Flags flags; uint32_t stage; ShaderModule module; const char* pName; const SpecializationInfo* pSpecializationInfo; };
struct ComputePipelineCreateInfo { int32_t sType; const void* pNext; Flags flags; PipelineShaderStageCreateInfo stage; PipelineLayout layout; Pipeline basePipelineHandle; int32_t basePipelineIndex; };
struct DescriptorPoolSize { uint32_t type, descriptorCount; };
struct DescriptorPoolCreateInfo { int32_t sType; const void* pNext; Flags flags; uint32_t maxSets, poolSizeCount; const DescriptorPoolSize* pPoolSizes; };
struct DescriptorSetAllocateInfo { int32_t sType; const void* pNext; DescriptorPool descriptorPool; uint32_t descriptorSetCount; const DescriptorSetLayout* pSetLayouts; };
struct DescriptorBufferInfo { Buffer buffer; DeviceSize offset, range; };
struct WriteDescriptorSet { int32_t sType; const void* pNext; DescriptorSet dstSet; uint32_t dstBinding, dstArrayElement, descriptorCount, descriptorType; const void* pImageInfo; const DescriptorBufferInfo* pBufferInfo; const void* pTexelBufferView; };
struct CommandPoolCreateInfo { int32_t sType; const void* pNext; Flags flags; uint32_t queueFamilyIndex; };
struct CommandBufferAllocateInfo { int32_t sType; const void* pNext; CommandPool commandPool; uint32_t level, commandBufferCount; };
struct CommandBufferBeginInfo { int32_t sType; const void* pNext; Flags flags; const void* pInheritanceInfo; };
struct SubmitInfo { int32_t sType; const void* pNext; uint32_t waitSemaphoreCount; const void* pWaitSemaphores; const Flags* pWaitDstStageMask; uint32_t commandBufferCount; const CommandBuffer* pCommandBuffers; uint32_t signalSemaphoreCount; const void* pSignalSemaphores; };
struct FenceCreateInfo { int32_t sType; const void* pNext; Flags flags; };
struct MemoryBarrier { int32_t sType; const void* pNext; Flags srcAccessMask, dstAccessMask; };
struct BufferCopy { DeviceSize srcOffset, dstOffset, size; };

typedef void (DECT2_VKAPI* PFN_VoidFunction)();
typedef PFN_VoidFunction (DECT2_VKAPI* PFN_GetInstanceProcAddr)(Instance, const char*);
typedef PFN_VoidFunction (DECT2_VKAPI* PFN_GetDeviceProcAddr)(Device, const char*);
typedef Result (DECT2_VKAPI* PFN_CreateInstance)(const InstanceCreateInfo*, const void*, Instance*);
typedef Result (DECT2_VKAPI* PFN_EnumerateInstanceExtensionProperties)(const char*, uint32_t*, ExtensionProperties*);
typedef void (DECT2_VKAPI* PFN_DestroyInstance)(Instance, const void*);
typedef Result (DECT2_VKAPI* PFN_EnumeratePhysicalDevices)(Instance, uint32_t*, PhysicalDevice*);
typedef void (DECT2_VKAPI* PFN_GetPhysicalDeviceProperties)(PhysicalDevice, PhysicalDeviceProperties*);
typedef void (DECT2_VKAPI* PFN_GetPhysicalDeviceQueueFamilyProperties)(PhysicalDevice, uint32_t*, QueueFamilyProperties*);
typedef void (DECT2_VKAPI* PFN_GetPhysicalDeviceMemoryProperties)(PhysicalDevice, PhysicalDeviceMemoryProperties*);
typedef Result (DECT2_VKAPI* PFN_EnumerateDeviceExtensionProperties)(PhysicalDevice, const char*, uint32_t*, ExtensionProperties*);
typedef Result (DECT2_VKAPI* PFN_CreateDevice)(PhysicalDevice, const DeviceCreateInfo*, const void*, Device*);
typedef void (DECT2_VKAPI* PFN_DestroyDevice)(Device, const void*);
typedef void (DECT2_VKAPI* PFN_GetDeviceQueue)(Device, uint32_t, uint32_t, Queue*);
typedef Result (DECT2_VKAPI* PFN_DeviceWaitIdle)(Device);
typedef Result (DECT2_VKAPI* PFN_CreateBuffer)(Device, const BufferCreateInfo*, const void*, Buffer*);
typedef void (DECT2_VKAPI* PFN_DestroyBuffer)(Device, Buffer, const void*);
typedef void (DECT2_VKAPI* PFN_GetBufferMemoryRequirements)(Device, Buffer, MemoryRequirements*);
typedef Result (DECT2_VKAPI* PFN_AllocateMemory)(Device, const MemoryAllocateInfo*, const void*, DeviceMemory*);
typedef void (DECT2_VKAPI* PFN_FreeMemory)(Device, DeviceMemory, const void*);
typedef Result (DECT2_VKAPI* PFN_BindBufferMemory)(Device, Buffer, DeviceMemory, DeviceSize);
typedef Result (DECT2_VKAPI* PFN_MapMemory)(Device, DeviceMemory, DeviceSize, DeviceSize, Flags, void**);
typedef void (DECT2_VKAPI* PFN_UnmapMemory)(Device, DeviceMemory);
typedef Result (DECT2_VKAPI* PFN_CreateShaderModule)(Device, const ShaderModuleCreateInfo*, const void*, ShaderModule*);
typedef void (DECT2_VKAPI* PFN_DestroyShaderModule)(Device, ShaderModule, const void*);
typedef Result (DECT2_VKAPI* PFN_CreateDescriptorSetLayout)(Device, const DescriptorSetLayoutCreateInfo*, const void*, DescriptorSetLayout*);
typedef void (DECT2_VKAPI* PFN_DestroyDescriptorSetLayout)(Device, DescriptorSetLayout, const void*);
typedef Result (DECT2_VKAPI* PFN_CreatePipelineLayout)(Device, const PipelineLayoutCreateInfo*, const void*, PipelineLayout*);
typedef void (DECT2_VKAPI* PFN_DestroyPipelineLayout)(Device, PipelineLayout, const void*);
typedef Result (DECT2_VKAPI* PFN_CreateComputePipelines)(Device, PipelineCache, uint32_t, const ComputePipelineCreateInfo*, const void*, Pipeline*);
typedef void (DECT2_VKAPI* PFN_DestroyPipeline)(Device, Pipeline, const void*);
typedef Result (DECT2_VKAPI* PFN_CreateDescriptorPool)(Device, const DescriptorPoolCreateInfo*, const void*, DescriptorPool*);
typedef void (DECT2_VKAPI* PFN_DestroyDescriptorPool)(Device, DescriptorPool, const void*);
typedef Result (DECT2_VKAPI* PFN_AllocateDescriptorSets)(Device, const DescriptorSetAllocateInfo*, DescriptorSet*);
typedef void (DECT2_VKAPI* PFN_UpdateDescriptorSets)(Device, uint32_t, const WriteDescriptorSet*, uint32_t, const void*);
typedef Result (DECT2_VKAPI* PFN_CreateCommandPool)(Device, const CommandPoolCreateInfo*, const void*, CommandPool*);
typedef void (DECT2_VKAPI* PFN_DestroyCommandPool)(Device, CommandPool, const void*);
typedef Result (DECT2_VKAPI* PFN_AllocateCommandBuffers)(Device, const CommandBufferAllocateInfo*, CommandBuffer*);
typedef Result (DECT2_VKAPI* PFN_BeginCommandBuffer)(CommandBuffer, const CommandBufferBeginInfo*);
typedef Result (DECT2_VKAPI* PFN_EndCommandBuffer)(CommandBuffer);
typedef Result (DECT2_VKAPI* PFN_ResetCommandBuffer)(CommandBuffer, Flags);
typedef void (DECT2_VKAPI* PFN_CmdBindPipeline)(CommandBuffer, uint32_t, Pipeline);
typedef void (DECT2_VKAPI* PFN_CmdBindDescriptorSets)(CommandBuffer, uint32_t, PipelineLayout, uint32_t, uint32_t, const DescriptorSet*, uint32_t, const uint32_t*);
typedef void (DECT2_VKAPI* PFN_CmdPushConstants)(CommandBuffer, PipelineLayout, Flags, uint32_t, uint32_t, const void*);
typedef void (DECT2_VKAPI* PFN_CmdDispatch)(CommandBuffer, uint32_t, uint32_t, uint32_t);
typedef void (DECT2_VKAPI* PFN_CmdPipelineBarrier)(CommandBuffer, Flags, Flags, Flags, uint32_t, const MemoryBarrier*, uint32_t, const void*, uint32_t, const void*);
typedef void (DECT2_VKAPI* PFN_CmdCopyBuffer)(CommandBuffer, Buffer, Buffer, uint32_t, const BufferCopy*);
typedef Result (DECT2_VKAPI* PFN_QueueSubmit)(Queue, uint32_t, const SubmitInfo*, Fence);
typedef Result (DECT2_VKAPI* PFN_CreateFence)(Device, const FenceCreateInfo*, const void*, Fence*);
typedef void (DECT2_VKAPI* PFN_DestroyFence)(Device, Fence, const void*);
typedef Result (DECT2_VKAPI* PFN_WaitForFences)(Device, uint32_t, const Fence*, Bool32, uint64_t);
typedef Result (DECT2_VKAPI* PFN_ResetFences)(Device, uint32_t, const Fence*);

} // namespace vkm
} // namespace dect2
