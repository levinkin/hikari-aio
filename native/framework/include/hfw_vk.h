#pragma once

#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace hfw {

struct vk_buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;

    bool create(VkDevice device, VkPhysicalDevice physical_device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    bool map(VkDevice device, VkDeviceSize size);
    void copy_in(const void* source, size_t bytes);
    void unmap(VkDevice device);
    void destroy(VkDevice device);
    VkBuffer handle() const;
};

struct vk_image {
    VkImage image_handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view_handle = VK_NULL_HANDLE;
    VkSampler sampler_handle = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

    bool create(VkDevice device, VkPhysicalDevice physical_device, uint32_t width, uint32_t height, uint32_t levels, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage);
    void create_view(VkDevice device, VkFormat format, VkImageAspectFlags aspect, uint32_t levels);
    void create_sampler(VkDevice device, VkSamplerAddressMode address_mode, VkFilter magnification, VkFilter minification, VkSamplerMipmapMode mipmap_mode, float max_anisotropy, uint32_t levels);
    void transition(VkCommandBuffer commands, VkImageLayout to, uint32_t levels, VkImageAspectFlags aspect);
    void set_layout(VkImageLayout to);
    void destroy(VkDevice device);
    VkImage image() const;
    VkImageView view() const;
    VkSampler sampler() const;
};

}
