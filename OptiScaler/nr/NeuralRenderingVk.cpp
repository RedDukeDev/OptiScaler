#include <pch.h>

#include "NeuralRenderingVk.h"
#include "NeuralRendering.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <misc/IdentifyGpu.h>
#include <proxies/DXGI_Proxy.h>
#include <with_dx12/with_dx12.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <map>
#include <vector>

namespace
{

constexpr uint32_t kSlots = 2;

VkInstance _instance = VK_NULL_HANDLE;
VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
VkDevice _device = VK_NULL_HANDLE;
std::map<VkQueue, uint32_t> _queueFamilies;

struct SwapchainInfo
{
    VkFormat format;
    VkExtent2D extent;
    std::vector<VkImage> images;
};
std::map<VkSwapchainKHR, SwapchainInfo> _swapchains;

PFN_vkGetMemoryWin32HandlePropertiesKHR _getMemoryWin32HandleProperties = nullptr;
PFN_vkImportSemaphoreWin32HandleKHR _importSemaphoreWin32Handle = nullptr;

// The Direct3D 12 side: the device and queue OptiScaler shares between its bridges, and two fences
// shared with Vulkan, one each way, whose values only ever grow.
ID3D12Device* _device12 = nullptr;
ID3D12CommandQueue* _queue12 = nullptr;
ID3D12Fence* _fenceTo12 = nullptr;
ID3D12Fence* _fenceFrom12 = nullptr;
VkSemaphore _semaphoreTo12 = VK_NULL_HANDLE;
VkSemaphore _semaphoreFrom12 = VK_NULL_HANDLE;
UINT64 _valueTo12 = 0;
UINT64 _valueFrom12 = 0;

// An image both APIs have: the Direct3D 12 resource and the Vulkan image over its memory.
struct SharedImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    ID3D12Resource* resource = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    bool inUse = false; // false until the first transition, which takes it from undefined
};
SharedImage _in;
SharedImage _out;

// What the Vulkan work of a frame uses, one set per frame in flight.
struct Slot
{
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer toShared = VK_NULL_HANDLE;
    VkCommandBuffer fromShared = VK_NULL_HANDLE;
    VkSemaphore done = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
};
Slot _slots[kSlots];
uint32_t _slotsFamily = UINT32_MAX;
uint32_t _frame = 0;
bool _prepared = false;
bool _failed = false;

DXGI_FORMAT ToDxgi(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

uint32_t FindMemoryType(uint32_t allowedBits, VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties properties {};
    vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &properties);

    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((allowedBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;

    return UINT32_MAX;
}

void DestroyShared(SharedImage& shared)
{
    if (shared.image != VK_NULL_HANDLE)
        vkDestroyImage(_device, shared.image, nullptr);
    if (shared.memory != VK_NULL_HANDLE)
        vkFreeMemory(_device, shared.memory, nullptr);
    if (shared.resource != nullptr)
        shared.resource->Release();

    shared = {};
}

bool CreateShared(SharedImage& shared, VkFormat format, uint32_t width, uint32_t height)
{
    DestroyShared(shared);

    const DXGI_FORMAT dxgiFormat = ToDxgi(format);
    if (dxgiFormat == DXGI_FORMAT_UNKNOWN)
    {
        LOG_ERROR("Neural rendering in Vulkan: swapchain format {} is not supported", (int) format);
        return false;
    }

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = dxgiFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    if (_device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                           IID_PPV_ARGS(&shared.resource)) != S_OK)
    {
        LOG_ERROR("Neural rendering in Vulkan: can't create the shared Direct3D 12 image");
        return false;
    }

    HANDLE handle = nullptr;
    if (_device12->CreateSharedHandle(shared.resource, nullptr, GENERIC_ALL, nullptr, &handle) != S_OK)
    {
        LOG_ERROR("Neural rendering in Vulkan: can't share the Direct3D 12 image");
        DestroyShared(shared);
        return false;
    }

    VkExternalMemoryImageCreateInfo external {};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

    VkImageCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.pNext = &external;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { width, height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    bool ok = vkCreateImage(_device, &info, nullptr, &shared.image) == VK_SUCCESS;

    VkMemoryRequirements requirements {};
    VkMemoryWin32HandlePropertiesKHR handleProperties {};
    handleProperties.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;

    if (ok)
    {
        vkGetImageMemoryRequirements(_device, shared.image, &requirements);
        ok = _getMemoryWin32HandleProperties(_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle,
                                             &handleProperties) == VK_SUCCESS;
    }

    if (ok)
    {
        VkMemoryDedicatedAllocateInfo dedicated {};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image = shared.image;

        VkImportMemoryWin32HandleInfoKHR import {};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
        import.pNext = &dedicated;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        import.handle = handle;

        VkMemoryAllocateInfo allocate {};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.pNext = &import;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits & handleProperties.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        ok = allocate.memoryTypeIndex != UINT32_MAX &&
             vkAllocateMemory(_device, &allocate, nullptr, &shared.memory) == VK_SUCCESS &&
             vkBindImageMemory(_device, shared.image, shared.memory, 0) == VK_SUCCESS;
    }

    CloseHandle(handle);

    if (!ok)
    {
        LOG_ERROR("Neural rendering in Vulkan: can't share a {}x{} image (format {}) with Direct3D 12", width, height,
                  (int) format);
        DestroyShared(shared);
        return false;
    }

    shared.format = format;
    shared.width = width;
    shared.height = height;
    return true;
}

// A Direct3D 12 fence and the Vulkan timeline semaphore over it.
bool CreateSharedFence(ID3D12Fence*& fence, VkSemaphore& semaphore)
{
    if (_device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)) != S_OK)
        return false;

    HANDLE handle = nullptr;
    if (_device12->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &handle) != S_OK)
        return false;

    VkSemaphoreTypeCreateInfo type {};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    VkSemaphoreCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type;

    bool ok = vkCreateSemaphore(_device, &info, nullptr, &semaphore) == VK_SUCCESS;

    if (ok)
    {
        VkImportSemaphoreWin32HandleInfoKHR import {};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
        import.semaphore = semaphore;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        import.handle = handle;
        ok = _importSemaphoreWin32Handle(_device, &import) == VK_SUCCESS;
    }

    CloseHandle(handle);
    return ok;
}

// The Direct3D 12 device and the fences, once.
bool Prepare()
{
    if (_prepared)
        return true;

    if (_failed || _device == VK_NULL_HANDLE)
        return false;

    _failed = true;

    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
    ScopedSkipVulkanHooks skipVulkanHooks {};

    auto getDeviceProcAddr = (PFN_vkGetDeviceProcAddr) vkGetInstanceProcAddr(_instance, "vkGetDeviceProcAddr");
    if (getDeviceProcAddr != nullptr)
    {
        _getMemoryWin32HandleProperties =
            (PFN_vkGetMemoryWin32HandlePropertiesKHR) getDeviceProcAddr(_device, "vkGetMemoryWin32HandlePropertiesKHR");
        _importSemaphoreWin32Handle =
            (PFN_vkImportSemaphoreWin32HandleKHR) getDeviceProcAddr(_device, "vkImportSemaphoreWin32HandleKHR");
    }

    if (_getMemoryWin32HandleProperties == nullptr || _importSemaphoreWin32Handle == nullptr)
    {
        LOG_ERROR("Neural rendering in Vulkan: the driver has no external memory or semaphore sharing with Windows");
        return false;
    }

    // The adapter the Vulkan device is on.
    IDXGIAdapter* adapter = nullptr;
    VkPhysicalDeviceIDProperties id {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties {};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &id;
    vkGetPhysicalDeviceProperties2(_physicalDevice, &properties);

    if (id.deviceLUIDValid == VK_TRUE)
    {
        IDXGIFactory2* factory = nullptr;
        const HRESULT made = DxgiProxy::Module() == nullptr
                                 ? CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))
                                 : DxgiProxy::CreateDxgiFactory2_()(0, __uuidof(factory), &factory);

        if (made == S_OK && factory != nullptr)
        {
            IDXGIFactory4* factory4 = nullptr;
            LUID luid {};
            memcpy(&luid, id.deviceLUID, sizeof(luid));
            if (factory->QueryInterface(IID_PPV_ARGS(&factory4)) == S_OK)
            {
                factory4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
                factory4->Release();
            }
            factory->Release();
        }
    }

    _device12 = WithDx12::RequestD3D12Device(D3D_FEATURE_LEVEL_12_0, adapter);
    if (adapter != nullptr)
        adapter->Release();

    _queue12 = WithDx12::GetD3D12CommandQueue();
    if (_device12 == nullptr || _queue12 == nullptr)
    {
        LOG_ERROR("Neural rendering in Vulkan: no Direct3D 12 device");
        return false;
    }

    if (!CreateSharedFence(_fenceTo12, _semaphoreTo12) || !CreateSharedFence(_fenceFrom12, _semaphoreFrom12))
    {
        LOG_ERROR("Neural rendering in Vulkan: can't share a fence with Direct3D 12");
        return false;
    }

    _failed = false;
    _prepared = true;
    LOG_INFO("Neural rendering in Vulkan: the swapchain image goes through Direct3D 12 at present");
    return true;
}

void DestroySlots()
{
    for (auto& slot : _slots)
    {
        if (slot.fence != VK_NULL_HANDLE)
            vkDestroyFence(_device, slot.fence, nullptr);
        if (slot.done != VK_NULL_HANDLE)
            vkDestroySemaphore(_device, slot.done, nullptr);
        if (slot.pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(_device, slot.pool, nullptr);

        slot = {};
    }

    _slotsFamily = UINT32_MAX;
}

bool CreateSlots(uint32_t family)
{
    if (_slotsFamily == family)
        return true;

    if (_slotsFamily != UINT32_MAX)
    {
        vkDeviceWaitIdle(_device);
        DestroySlots();
    }

    for (auto& slot : _slots)
    {
        VkCommandPoolCreateInfo poolInfo {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = family;

        VkCommandBuffer buffers[2] {};
        VkSemaphoreCreateInfo semaphoreInfo { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

        bool ok = vkCreateCommandPool(_device, &poolInfo, nullptr, &slot.pool) == VK_SUCCESS;
        if (ok)
        {
            VkCommandBufferAllocateInfo allocate {};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = slot.pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 2;
            ok = vkAllocateCommandBuffers(_device, &allocate, buffers) == VK_SUCCESS;
        }
        ok = ok && vkCreateSemaphore(_device, &semaphoreInfo, nullptr, &slot.done) == VK_SUCCESS &&
             vkCreateFence(_device, &fenceInfo, nullptr, &slot.fence) == VK_SUCCESS;

        if (!ok)
        {
            LOG_ERROR("Neural rendering in Vulkan: can't create the command buffers");
            DestroySlots();
            return false;
        }

        slot.toShared = buffers[0];
        slot.fromShared = buffers[1];
    }

    _slotsFamily = family;
    return true;
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
             VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
}

void CopyImage(VkCommandBuffer cmd, VkImage from, VkImage to, uint32_t width, uint32_t height)
{
    VkImageCopy region {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { width, height, 1 };
    vkCmdCopyImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region);
}

constexpr VkAccessFlags kAnyAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

} // namespace

VkImageUsageFlags NeuralRenderingVk::SwapchainUsage()
{
    if (!NeuralRendering::Enabled())
        return 0;

    return VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}

void NeuralRenderingVk::DeviceCreated(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                                      const VkDeviceCreateInfo* createInfo)
{
    _instance = instance;
    _physicalDevice = physicalDevice;
    _device = device;
    _queueFamilies.clear();

    for (uint32_t i = 0; createInfo != nullptr && i < createInfo->queueCreateInfoCount; i++)
    {
        auto& info = createInfo->pQueueCreateInfos[i];

        // Queues with flags need vkGetDeviceQueue2, not presented from in practice
        if (info.flags != 0)
            continue;

        for (uint32_t index = 0; index < info.queueCount; index++)
        {
            VkQueue queue = VK_NULL_HANDLE;
            vkGetDeviceQueue(device, info.queueFamilyIndex, index, &queue);
            _queueFamilies[queue] = info.queueFamilyIndex;
        }
    }
}

void NeuralRenderingVk::SwapchainCreated(VkDevice device, const VkSwapchainCreateInfoKHR* createInfo,
                                         VkSwapchainKHR swapchain)
{
    if (createInfo == nullptr || swapchain == VK_NULL_HANDLE)
        return;

    SwapchainInfo info {};
    info.format = createInfo->imageFormat;
    info.extent = createInfo->imageExtent;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    info.images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, info.images.data());

    _swapchains[swapchain] = std::move(info);
}

void NeuralRenderingVk::SwapchainDestroyed(VkSwapchainKHR swapchain) { _swapchains.erase(swapchain); }

void NeuralRenderingVk::QueuePresent(VkQueue queue, VkPresentInfoKHR* presentInfo)
{
    // Counts the present, too: whether an upscaler has been handing frames over lately.
    if (!NeuralRendering::WantsFinishedImage() || _failed || presentInfo == nullptr || presentInfo->swapchainCount != 1)
        return;

    auto swapchain = _swapchains.find(presentInfo->pSwapchains[0]);
    auto family = _queueFamilies.find(queue);
    if (swapchain == _swapchains.end() || family == _queueFamilies.end() ||
        presentInfo->pImageIndices[0] >= swapchain->second.images.size())
        return;

    if (!Prepare() || !NeuralRendering::PrepareVulkan(_device12))
        return;

    const auto& info = swapchain->second;
    const VkImage image = info.images[presentInfo->pImageIndices[0]];

    if (!CreateSlots(family->second))
        return;

    if (_in.image == VK_NULL_HANDLE || _in.format != info.format || _in.width != info.extent.width ||
        _in.height != info.extent.height)
    {
        // The Direct3D 12 side may still be using them.
        vkDeviceWaitIdle(_device);
        if (_valueFrom12 > 0 && _fenceFrom12->GetCompletedValue() < _valueFrom12)
        {
            HANDLE wait = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            _fenceFrom12->SetEventOnCompletion(_valueFrom12, wait);
            WaitForSingleObject(wait, 2000);
            CloseHandle(wait);
        }

        if (!CreateShared(_in, info.format, info.extent.width, info.extent.height) ||
            !CreateShared(_out, info.format, info.extent.width, info.extent.height))
        {
            DestroyShared(_in);
            DestroyShared(_out);
            _failed = true;
            return;
        }
    }

    Slot& slot = _slots[_frame++ % kSlots];
    if (slot.submitted)
    {
        vkWaitForFences(_device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(_device, 1, &slot.fence);
        slot.submitted = false;
    }
    vkResetCommandPool(_device, slot.pool, 0);

    // ---- Vulkan: the image into the shared one, after the game is done with it
    VkCommandBufferBeginInfo begin {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(slot.toShared, &begin);
    Barrier(slot.toShared, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kAnyAccess,
            VK_ACCESS_TRANSFER_READ_BIT);
    Barrier(slot.toShared, _in.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    CopyImage(slot.toShared, image, _in.image, info.extent.width, info.extent.height);
    Barrier(slot.toShared, _in.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, kAnyAccess);
    Barrier(slot.toShared, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_READ_BIT, kAnyAccess);
    vkEndCommandBuffer(slot.toShared);

    std::vector<VkPipelineStageFlags> waitStages(presentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
    std::vector<uint64_t> waitValues(presentInfo->waitSemaphoreCount, 0);
    const uint64_t signalTo12 = ++_valueTo12;

    VkTimelineSemaphoreSubmitInfo timelineTo12 {};
    timelineTo12.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineTo12.waitSemaphoreValueCount = presentInfo->waitSemaphoreCount;
    timelineTo12.pWaitSemaphoreValues = waitValues.data();
    timelineTo12.signalSemaphoreValueCount = 1;
    timelineTo12.pSignalSemaphoreValues = &signalTo12;

    VkSubmitInfo submitTo12 {};
    submitTo12.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitTo12.pNext = &timelineTo12;
    submitTo12.waitSemaphoreCount = presentInfo->waitSemaphoreCount;
    submitTo12.pWaitSemaphores = presentInfo->pWaitSemaphores;
    submitTo12.pWaitDstStageMask = waitStages.data();
    submitTo12.commandBufferCount = 1;
    submitTo12.pCommandBuffers = &slot.toShared;
    submitTo12.signalSemaphoreCount = 1;
    submitTo12.pSignalSemaphores = &_semaphoreTo12;

    if (vkQueueSubmit(queue, 1, &submitTo12, VK_NULL_HANDLE) != VK_SUCCESS)
    {
        LOG_ERROR("Neural rendering in Vulkan: the first submission failed");
        _failed = true;
        return;
    }

    // ---- Direct3D 12: wait for that, compose, say when done
    _queue12->Wait(_fenceTo12, signalTo12);
    const bool composed = NeuralRendering::ComposeFinished(_device12, _queue12, _in.resource, _out.resource);
    const uint64_t signalFrom12 = ++_valueFrom12;
    _queue12->Signal(_fenceFrom12, signalFrom12);

    // ---- Vulkan: wait for that, and the result over the image
    vkBeginCommandBuffer(slot.fromShared, &begin);
    if (composed)
    {
        Barrier(slot.fromShared, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                kAnyAccess, VK_ACCESS_TRANSFER_WRITE_BIT);
        Barrier(slot.fromShared, _out.image, _out.inUse ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kAnyAccess, VK_ACCESS_TRANSFER_READ_BIT);
        CopyImage(slot.fromShared, _out.image, image, info.extent.width, info.extent.height);
        Barrier(slot.fromShared, _out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_READ_BIT, kAnyAccess);
        Barrier(slot.fromShared, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, kAnyAccess);
        _out.inUse = true;
    }
    vkEndCommandBuffer(slot.fromShared);

    const VkPipelineStageFlags waitFrom12 = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const uint64_t zero = 0;

    VkTimelineSemaphoreSubmitInfo timelineFrom12 {};
    timelineFrom12.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineFrom12.waitSemaphoreValueCount = 1;
    timelineFrom12.pWaitSemaphoreValues = &signalFrom12;
    timelineFrom12.signalSemaphoreValueCount = 1;
    timelineFrom12.pSignalSemaphoreValues = &zero;

    VkSubmitInfo submitFrom12 {};
    submitFrom12.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitFrom12.pNext = &timelineFrom12;
    submitFrom12.waitSemaphoreCount = 1;
    submitFrom12.pWaitSemaphores = &_semaphoreFrom12;
    submitFrom12.pWaitDstStageMask = &waitFrom12;
    submitFrom12.commandBufferCount = 1;
    submitFrom12.pCommandBuffers = &slot.fromShared;
    submitFrom12.signalSemaphoreCount = 1;
    submitFrom12.pSignalSemaphores = &slot.done;

    if (vkQueueSubmit(queue, 1, &submitFrom12, slot.fence) != VK_SUCCESS)
    {
        LOG_ERROR("Neural rendering in Vulkan: the second submission failed");
        _failed = true;
        return;
    }

    slot.submitted = true;

    // Present now waits for the result, not for what the game signalled.
    presentInfo->waitSemaphoreCount = 1;
    presentInfo->pWaitSemaphores = &slot.done;

    NeuralRendering::Present(_queue12);
}
