#include "../include/hfw_vk.h"

#include <cstring>

namespace hfw {

static bool memory_type(VkPhysicalDevice physical_device, uint32_t allowed, VkMemoryPropertyFlags properties, uint32_t* found)
{
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        if ((allowed & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            *found = i;
            return true;
        }
    }
    return false;
}

static bool allocate(VkDevice device, VkPhysicalDevice physical_device, VkMemoryRequirements requirements, VkMemoryPropertyFlags properties, VkDeviceMemory* memory)
{
    VkMemoryAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize = requirements.size;
    return memory_type(physical_device, requirements.memoryTypeBits, properties, &info.memoryTypeIndex) && vkAllocateMemory(device, &info, nullptr, memory) == VK_SUCCESS;
}

static void layout_use(VkImageLayout layout, VkAccessFlags* access, VkPipelineStageFlags* stage)
{
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        *access = 0;
        *stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *access = VK_ACCESS_TRANSFER_WRITE_BIT;
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *access = VK_ACCESS_TRANSFER_READ_BIT;
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *access = VK_ACCESS_SHADER_READ_BIT;
        *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        break;
    default:
        *access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        break;
    }
}

bool vk_buffer::create(VkDevice device, VkPhysicalDevice physical_device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties)
{
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) {
        buffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    if (!allocate(device, physical_device, requirements, properties, &memory) || vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        destroy(device);
        return false;
    }
    return true;
}

bool vk_buffer::map(VkDevice device, VkDeviceSize size)
{
    if (vkMapMemory(device, memory, 0, size, 0, &mapped) != VK_SUCCESS) {
        mapped = nullptr;
        return false;
    }
    return true;
}

void vk_buffer::copy_in(const void* source, size_t bytes)
{
    std::memcpy(mapped, source, bytes);
}

void vk_buffer::unmap(VkDevice device)
{
    if (mapped) {
        vkUnmapMemory(device, memory);
        mapped = nullptr;
    }
}

void vk_buffer::destroy(VkDevice device)
{
    unmap(device);
    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }
}

VkBuffer vk_buffer::handle() const
{
    return buffer;
}

bool vk_image::create(VkDevice device, VkPhysicalDevice physical_device, uint32_t width, uint32_t height, uint32_t levels, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage)
{
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = tiling;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &info, nullptr, &image_handle) != VK_SUCCESS) {
        image_handle = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image_handle, &requirements);
    if (!allocate(device, physical_device, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory) || vkBindImageMemory(device, image_handle, memory, 0) != VK_SUCCESS) {
        destroy(device);
        return false;
    }
    return true;
}

void vk_image::create_view(VkDevice device, VkFormat format, VkImageAspectFlags aspect, uint32_t levels)
{
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image_handle;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {aspect, 0, levels, 0, 1};
    if (vkCreateImageView(device, &info, nullptr, &view_handle) != VK_SUCCESS) {
        view_handle = VK_NULL_HANDLE;
    }
}

void vk_image::create_sampler(VkDevice device, VkSamplerAddressMode address_mode, VkFilter magnification, VkFilter minification, VkSamplerMipmapMode mipmap_mode, float max_anisotropy, uint32_t levels)
{
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = magnification;
    info.minFilter = minification;
    info.mipmapMode = mipmap_mode;
    info.addressModeU = address_mode;
    info.addressModeV = address_mode;
    info.addressModeW = address_mode;
    info.anisotropyEnable = max_anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = max_anisotropy;
    info.compareOp = VK_COMPARE_OP_NEVER;
    info.minLod = 0.0f;
    info.maxLod = static_cast<float>(levels);
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(device, &info, nullptr, &sampler_handle) != VK_SUCCESS) {
        sampler_handle = VK_NULL_HANDLE;
    }
}

void vk_image::transition(VkCommandBuffer commands, VkImageLayout to, uint32_t levels, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = layout;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_handle;
    barrier.subresourceRange = {aspect, 0, levels, 0, 1};
    VkPipelineStageFlags source_stage;
    VkPipelineStageFlags destination_stage;
    layout_use(layout, &barrier.srcAccessMask, &source_stage);
    layout_use(to, &barrier.dstAccessMask, &destination_stage);
    vkCmdPipelineBarrier(commands, source_stage, destination_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    layout = to;
}

void vk_image::set_layout(VkImageLayout to)
{
    layout = to;
}

void vk_image::destroy(VkDevice device)
{
    if (sampler_handle != VK_NULL_HANDLE) {
        vkDestroySampler(device, sampler_handle, nullptr);
        sampler_handle = VK_NULL_HANDLE;
    }
    if (view_handle != VK_NULL_HANDLE) {
        vkDestroyImageView(device, view_handle, nullptr);
        view_handle = VK_NULL_HANDLE;
    }
    if (image_handle != VK_NULL_HANDLE) {
        vkDestroyImage(device, image_handle, nullptr);
        image_handle = VK_NULL_HANDLE;
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }
    layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

VkImage vk_image::image() const
{
    return image_handle;
}

VkImageView vk_image::view() const
{
    return view_handle;
}

VkSampler vk_image::sampler() const
{
    return sampler_handle;
}

}
