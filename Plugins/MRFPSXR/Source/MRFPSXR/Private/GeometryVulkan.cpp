#include "GeometryVulkan.h"

#if PLATFORM_ANDROID && !UE_SERVER
#include "GeometryCaptureSubsystem.h"
#include "EnvironmentGeometrySpirv.h"

namespace
{
constexpr uint32 PointCount = MRFPSGeometry::GridWidth * MRFPSGeometry::GridHeight;
constexpr VkDeviceSize OutputBytes = PointCount * sizeof(FVector4f);
constexpr uint32 MaximumSwapchainImages = 16;
static_assert(sizeof(FVector4f) == 16, "std430 vec4 readback layout must match");
TAtomic<bool> DeviceQuarantined(false);
DEFINE_LOG_CATEGORY_STATIC(LogMRFPSGeometryVulkan, Log, All);

#define MRFPS_GEOMETRY_VK_FUNCTIONS(X) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
    X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(InvalidateMappedMemoryRanges) \
    X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateComputePipelines) X(DestroyPipeline) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandPool) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBindPipeline) X(CmdBindDescriptorSets) \
    X(CmdPushConstants) X(CmdDispatch) X(CmdPipelineBarrier) \
    X(CreateFence) X(DestroyFence) X(ResetFences) X(GetFenceStatus) X(WaitForFences) X(QueueSubmit)
}

struct FMRFPSGeometryVulkan::FImpl
{
#define MRFPS_GEOMETRY_DECLARE_VK_FUNCTION(Name) PFN_vk##Name Name = nullptr;
    MRFPS_GEOMETRY_VK_FUNCTIONS(MRFPS_GEOMETRY_DECLARE_VK_FUNCTION)
#undef MRFPS_GEOMETRY_DECLARE_VK_FUNCTION
    VkDevice Device = VK_NULL_HANDLE;
    VkQueue Queue = VK_NULL_HANDLE;
    VkBuffer Buffer = VK_NULL_HANDLE;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
    void* Mapped = nullptr;
    bool bCoherent = false;
    bool bPending = false;
    bool bFaulted = false;
    VkDescriptorSetLayout SetLayout = VK_NULL_HANDLE;
    VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
    VkPipeline Pipeline = VK_NULL_HANDLE;
    VkShaderModule Shader = VK_NULL_HANDLE;
    VkSampler Sampler = VK_NULL_HANDLE;
    VkDescriptorPool DescriptorPool = VK_NULL_HANDLE;
    VkCommandPool CommandPool = VK_NULL_HANDLE;
    VkCommandBuffer Command = VK_NULL_HANDLE;
    VkFence Fence = VK_NULL_HANDLE;
    TArray<VkImageView> Views;
    TArray<VkDescriptorSet> Sets;

    bool Load(IVulkanDynamicRHI& RHI)
    {
#define LOAD_FUNCTION(Name) \
        Name = reinterpret_cast<PFN_vk##Name>(RHI.RHIGetVkDeviceProcAddr("vk" #Name)); \
        if (!Name) return false;
        MRFPS_GEOMETRY_VK_FUNCTIONS(LOAD_FUNCTION)
#undef LOAD_FUNCTION
        return true;
    }

    bool Release()
    {
        if (!Device) return true;
        // Called only after the extension has stopped accepting work. The kernel
        // has a fixed 48x36 bound and no waits on UE/game-thread work. Do not free
        // a buffer/command pool or OpenXR image view while its dispatch is live.
        if (bPending && Fence)
        {
            const VkResult Result = WaitForFences(Device, 1, &Fence, VK_TRUE, 2000000000ull);
            if (Result != VK_SUCCESS && Result != VK_ERROR_DEVICE_LOST)
            {
                // Exactly one bounded set can enter quarantine: all future
                // initialization is disabled. GPU/device teardown ultimately
                // reclaims it. Never destroy an image view/buffer still in use.
                DeviceQuarantined.Store(true);
                UE_LOG(LogMRFPSGeometryVulkan, Error, TEXT("Owned geometry fence did not complete during teardown (%d); depth resources quarantined"), int32(Result));
                return false;
            }
        }
        bPending = false;
        if (Fence) DestroyFence(Device, Fence, nullptr);
        if (CommandPool) DestroyCommandPool(Device, CommandPool, nullptr);
        if (DescriptorPool) DestroyDescriptorPool(Device, DescriptorPool, nullptr);
        for (VkImageView View : Views) if (View) DestroyImageView(Device, View, nullptr);
        if (Sampler) DestroySampler(Device, Sampler, nullptr);
        if (Pipeline) DestroyPipeline(Device, Pipeline, nullptr);
        if (Shader) DestroyShaderModule(Device, Shader, nullptr);
        if (PipelineLayout) DestroyPipelineLayout(Device, PipelineLayout, nullptr);
        if (SetLayout) DestroyDescriptorSetLayout(Device, SetLayout, nullptr);
        if (Mapped) UnmapMemory(Device, Memory);
        if (Buffer) DestroyBuffer(Device, Buffer, nullptr);
        if (Memory) FreeMemory(Device, Memory, nullptr);
        Device = VK_NULL_HANDLE;
        return true;
    }
};

FMRFPSGeometryVulkan::FMRFPSGeometryVulkan() = default;
FMRFPSGeometryVulkan::~FMRFPSGeometryVulkan() { Shutdown(); }

bool FMRFPSGeometryVulkan::Initialize(IVulkanDynamicRHI& RHI, TConstArrayView<VkImage> Images, FString& SafeError)
{
    if (!Shutdown() || !IsDeviceUsable()) return false;
    SafeError = TEXT("Vulkan geometry resources unavailable.");
    if (Images.IsEmpty() || Images.Num() > int32(MaximumSwapchainImages)) return false;
    Impl = MakeUnique<FImpl>();
    auto& V = *Impl;
    V.Device = RHI.RHIGetVkDevice();
    V.Queue = RHI.RHIGetGraphicsVkQueue();
    if (!V.Device || !V.Queue || !V.Load(RHI)) { Shutdown(); return false; }

    const auto GetMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(RHI.RHIGetVkInstanceProcAddr("vkGetPhysicalDeviceMemoryProperties"));
    const auto GetFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(RHI.RHIGetVkInstanceProcAddr("vkGetPhysicalDeviceFormatProperties"));
    const auto GetQueueProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(RHI.RHIGetVkInstanceProcAddr("vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!GetMemoryProperties || !GetFormatProperties || !GetQueueProperties) { Shutdown(); return false; }
    VkFormatProperties Format{};
    GetFormatProperties(RHI.RHIGetVkPhysicalDevice(), VK_FORMAT_D16_UNORM, &Format);
    if (!(Format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { Shutdown(); return false; }
    uint32 QueueCount = 0;
    GetQueueProperties(RHI.RHIGetVkPhysicalDevice(), &QueueCount, nullptr);
    if (QueueCount == 0 || QueueCount > 64 || RHI.RHIGetGraphicsQueueFamilyIndex() >= QueueCount) { Shutdown(); return false; }
    TArray<VkQueueFamilyProperties> QueueProperties;
    QueueProperties.SetNumUninitialized(QueueCount);
    GetQueueProperties(RHI.RHIGetVkPhysicalDevice(), &QueueCount, QueueProperties.GetData());
    if (!(QueueProperties[RHI.RHIGetGraphicsQueueFamilyIndex()].queueFlags & VK_QUEUE_COMPUTE_BIT)) { Shutdown(); return false; }

    VkBufferCreateInfo BufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    BufferInfo.size = OutputBytes;
    BufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    BufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (V.CreateBuffer(V.Device, &BufferInfo, nullptr, &V.Buffer) != VK_SUCCESS) { Shutdown(); return false; }
    VkMemoryRequirements Requirements{};
    V.GetBufferMemoryRequirements(V.Device, V.Buffer, &Requirements);
    if (Requirements.size > 1024 * 1024) { Shutdown(); return false; }
    VkPhysicalDeviceMemoryProperties MemoryProperties{};
    GetMemoryProperties(RHI.RHIGetVkPhysicalDevice(), &MemoryProperties);
    uint32 MemoryType = MAX_uint32;
    for (int32 Pass = 0; Pass < 2 && MemoryType == MAX_uint32; ++Pass)
        for (uint32 I = 0; I < MemoryProperties.memoryTypeCount; ++I)
        {
            const VkMemoryPropertyFlags Flags = MemoryProperties.memoryTypes[I].propertyFlags;
            if ((Requirements.memoryTypeBits & (1u << I)) && (Flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                (Pass != 0 || (Flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
            { MemoryType = I; V.bCoherent = (Flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0; break; }
        }
    if (MemoryType == MAX_uint32) { Shutdown(); return false; }
    VkMemoryAllocateInfo Allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    Allocate.allocationSize = Requirements.size;
    Allocate.memoryTypeIndex = MemoryType;
    if (V.AllocateMemory(V.Device, &Allocate, nullptr, &V.Memory) != VK_SUCCESS ||
        V.BindBufferMemory(V.Device, V.Buffer, V.Memory, 0) != VK_SUCCESS ||
        V.MapMemory(V.Device, V.Memory, 0, VK_WHOLE_SIZE, 0, &V.Mapped) != VK_SUCCESS)
    { Shutdown(); return false; }

    const VkDescriptorSetLayoutBinding Bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
    };
    VkDescriptorSetLayoutCreateInfo SetInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    SetInfo.bindingCount = UE_ARRAY_COUNT(Bindings); SetInfo.pBindings = Bindings;
    if (V.CreateDescriptorSetLayout(V.Device, &SetInfo, nullptr, &V.SetLayout) != VK_SUCCESS) { Shutdown(); return false; }
    VkPushConstantRange Range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FMRFPSGeometryGpuParameters)};
    VkPipelineLayoutCreateInfo LayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    LayoutInfo.setLayoutCount = 1; LayoutInfo.pSetLayouts = &V.SetLayout;
    LayoutInfo.pushConstantRangeCount = 1; LayoutInfo.pPushConstantRanges = &Range;
    if (V.CreatePipelineLayout(V.Device, &LayoutInfo, nullptr, &V.PipelineLayout) != VK_SUCCESS) { Shutdown(); return false; }
    VkShaderModuleCreateInfo ShaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ShaderInfo.codeSize = sizeof(GMRFPSEnvironmentGeometrySpirv); ShaderInfo.pCode = GMRFPSEnvironmentGeometrySpirv;
    if (V.CreateShaderModule(V.Device, &ShaderInfo, nullptr, &V.Shader) != VK_SUCCESS) { Shutdown(); return false; }
    VkComputePipelineCreateInfo PipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    PipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    PipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; PipelineInfo.stage.module = V.Shader; PipelineInfo.stage.pName = "main";
    PipelineInfo.layout = V.PipelineLayout;
    if (V.CreateComputePipelines(V.Device, VK_NULL_HANDLE, 1, &PipelineInfo, nullptr, &V.Pipeline) != VK_SUCCESS) { Shutdown(); return false; }
    VkSamplerCreateInfo SamplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    SamplerInfo.magFilter = SamplerInfo.minFilter = VK_FILTER_NEAREST;
    SamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    SamplerInfo.addressModeU = SamplerInfo.addressModeV = SamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    SamplerInfo.maxLod = 0.0f;
    if (V.CreateSampler(V.Device, &SamplerInfo, nullptr, &V.Sampler) != VK_SUCCESS) { Shutdown(); return false; }
    VkDescriptorPoolSize PoolSizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32(Images.Num())},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, uint32(Images.Num())}
    };
    VkDescriptorPoolCreateInfo PoolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    PoolInfo.maxSets = Images.Num(); PoolInfo.poolSizeCount = UE_ARRAY_COUNT(PoolSizes); PoolInfo.pPoolSizes = PoolSizes;
    if (V.CreateDescriptorPool(V.Device, &PoolInfo, nullptr, &V.DescriptorPool) != VK_SUCCESS) { Shutdown(); return false; }
    TArray<VkDescriptorSetLayout> Layouts; Layouts.Init(V.SetLayout, Images.Num());
    V.Sets.SetNumZeroed(Images.Num()); V.Views.SetNumZeroed(Images.Num());
    VkDescriptorSetAllocateInfo SetsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    SetsInfo.descriptorPool = V.DescriptorPool; SetsInfo.descriptorSetCount = Images.Num(); SetsInfo.pSetLayouts = Layouts.GetData();
    if (V.AllocateDescriptorSets(V.Device, &SetsInfo, V.Sets.GetData()) != VK_SUCCESS) { Shutdown(); return false; }
    for (int32 I = 0; I < Images.Num(); ++I)
    {
        VkImageViewCreateInfo ViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ViewInfo.image = Images[I]; ViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; ViewInfo.format = VK_FORMAT_D16_UNORM;
        ViewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 2};
        if (V.CreateImageView(V.Device, &ViewInfo, nullptr, &V.Views[I]) != VK_SUCCESS) { Shutdown(); return false; }
        VkDescriptorImageInfo ImageInfo{V.Sampler, V.Views[I], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo OutputInfo{V.Buffer, 0, OutputBytes};
        VkWriteDescriptorSet Writes[2]{};
        Writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; Writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        Writes[0].dstSet = V.Sets[I]; Writes[0].dstBinding = 0; Writes[0].descriptorCount = 1;
        Writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; Writes[0].pImageInfo = &ImageInfo;
        Writes[1].dstSet = V.Sets[I]; Writes[1].dstBinding = 1; Writes[1].descriptorCount = 1;
        Writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; Writes[1].pBufferInfo = &OutputInfo;
        V.UpdateDescriptorSets(V.Device, 2, Writes, 0, nullptr);
    }
    VkCommandPoolCreateInfo CommandInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    CommandInfo.queueFamilyIndex = RHI.RHIGetGraphicsQueueFamilyIndex();
    if (V.CreateCommandPool(V.Device, &CommandInfo, nullptr, &V.CommandPool) != VK_SUCCESS) { Shutdown(); return false; }
    VkCommandBufferAllocateInfo CommandAllocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    CommandAllocate.commandPool = V.CommandPool; CommandAllocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; CommandAllocate.commandBufferCount = 1;
    if (V.AllocateCommandBuffers(V.Device, &CommandAllocate, &V.Command) != VK_SUCCESS) { Shutdown(); return false; }
    VkFenceCreateInfo FenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (V.CreateFence(V.Device, &FenceInfo, nullptr, &V.Fence) != VK_SUCCESS) { Shutdown(); return false; }
    SafeError.Reset();
    return true;
}

bool FMRFPSGeometryVulkan::Submit(uint32 ImageIndex, const FMRFPSGeometryGpuParameters& Parameters)
{
    if (!Impl || Impl->bPending || Impl->bFaulted || ImageIndex >= uint32(Impl->Sets.Num())) return false;
    auto& V = *Impl;
    if (V.ResetCommandPool(V.Device, V.CommandPool, 0) != VK_SUCCESS || V.ResetFences(V.Device, 1, &V.Fence) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo Begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; Begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (V.BeginCommandBuffer(V.Command, &Begin) != VK_SUCCESS) return false;
    VkBufferMemoryBarrier Before{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    Before.srcAccessMask = VK_ACCESS_HOST_READ_BIT; Before.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    Before.srcQueueFamilyIndex = Before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    Before.buffer = V.Buffer; Before.size = OutputBytes;
    V.CmdPipelineBarrier(V.Command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &Before, 0, nullptr);
    V.CmdBindPipeline(V.Command, VK_PIPELINE_BIND_POINT_COMPUTE, V.Pipeline);
    V.CmdBindDescriptorSets(V.Command, VK_PIPELINE_BIND_POINT_COMPUTE, V.PipelineLayout, 0, 1, &V.Sets[ImageIndex], 0, nullptr);
    V.CmdPushConstants(V.Command, V.PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters), &Parameters);
    V.CmdDispatch(V.Command, (MRFPSGeometry::GridWidth + 7) / 8, (MRFPSGeometry::GridHeight + 7) / 8, 1);
    VkBufferMemoryBarrier After{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    After.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; After.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    After.srcQueueFamilyIndex = After.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    After.buffer = V.Buffer; After.size = OutputBytes;
    V.CmdPipelineBarrier(V.Command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &After, 0, nullptr);
    if (V.EndCommandBuffer(V.Command) != VK_SUCCESS) return false;
    VkSubmitInfo SubmitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO}; SubmitInfo.commandBufferCount = 1; SubmitInfo.pCommandBuffers = &V.Command;
    const VkResult Result = V.QueueSubmit(V.Queue, 1, &SubmitInfo, V.Fence);
    V.bPending = Result == VK_SUCCESS; V.bFaulted = Result != VK_SUCCESS;
    return V.bPending;
}

FMRFPSGeometryVulkan::EPoll FMRFPSGeometryVulkan::Poll(TArray<FVector4f>& Points)
{
    Points.Reset();
    if (!Impl || !Impl->bPending) return EPoll::Empty;
    auto& V = *Impl;
    const VkResult Result = V.GetFenceStatus(V.Device, V.Fence);
    if (Result == VK_NOT_READY) return EPoll::Pending;
    if (Result != VK_SUCCESS) { V.bFaulted = true; return EPoll::Error; }
    if (!V.bCoherent)
    {
        VkMappedMemoryRange Range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; Range.memory = V.Memory; Range.offset = 0; Range.size = VK_WHOLE_SIZE;
        if (V.InvalidateMappedMemoryRanges(V.Device, 1, &Range) != VK_SUCCESS) { V.bFaulted = true; return EPoll::Error; }
    }
    Points.SetNumUninitialized(PointCount);
    FMemory::Memcpy(Points.GetData(), V.Mapped, OutputBytes);
    V.bPending = false;
    return EPoll::Ready;
}

bool FMRFPSGeometryVulkan::HasPending() const { return Impl && Impl->bPending; }

bool FMRFPSGeometryVulkan::IsDeviceUsable() { return !DeviceQuarantined.Load(); }

bool FMRFPSGeometryVulkan::Shutdown()
{
    if (!Impl) return true;
    if (!Impl->Release())
    {
        // Deliberate bounded retention after a pathological GPU failure. Do not
        // invoke a destructor that could free native objects before completion.
        (void)Impl.Release();
        return false;
    }
    Impl.Reset();
    return true;
}
#undef MRFPS_GEOMETRY_VK_FUNCTIONS
#endif
