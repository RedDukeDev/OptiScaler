#pragma once

// DLSS 5 Neural Rendering in Vulkan games: the finished image, at vkQueuePresentKHR.
//
// The runtime is a Direct3D 12 one, so the swapchain image goes to it through images shared with
// OptiScaler's Direct3D 12 device (the one the Vulkan upscalers on Direct3D 12 use too), with the
// D3D12 fences imported as timeline semaphores for the order between the two APIs:
//
//   Vulkan   wait the game's present semaphores, copy the swapchain image into the shared one,
//            signal semaphore A
//   Direct3D wait A, compose the image through the runtime, signal fence B
//   Vulkan   wait B, copy the result back over the swapchain image, signal what present waits on
//
// The menu is drawn after this, over the result. Like the Direct3D 12 path without an upscaler
// there are no depth or motion vectors, and the game's interface is part of the image.

#include <vulkan/vulkan.h>

class NeuralRenderingVk
{
  public:
    // From the hooks. Pointers the game's own objects came from; nothing here keeps them alive.
    static void DeviceCreated(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                              const VkDeviceCreateInfo* createInfo);
    static void SwapchainCreated(VkDevice device, const VkSwapchainCreateInfoKHR* createInfo, VkSwapchainKHR swapchain);
    static void SwapchainDestroyed(VkSwapchainKHR swapchain);

    // Usage the swapchain's images need for the copies; none while neural rendering is off.
    static VkImageUsageFlags SwapchainUsage();

    // At vkQueuePresentKHR, before the menu: enhances the image about to be presented when no
    // upscaler is doing it, and makes presentInfo wait for the result.
    static void QueuePresent(VkQueue queue, VkPresentInfoKHR* presentInfo);
};
