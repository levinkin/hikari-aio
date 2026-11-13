#pragma once
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>
#include "hfw.h"

struct gpu_image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct renderer
{
    hfw::model* model = NULL;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[2] = {};
    VkFence fences[2] = {};
    uint32_t slot = 0;
    VkBuffer static_buffer = VK_NULL_HANDLE;
    VkDeviceMemory static_memory = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    VkBuffer ring = VK_NULL_HANDLE;
    VkDeviceMemory ring_memory = VK_NULL_HANDLE;
    uint8_t* ring_mapped = NULL;
    VkDeviceSize slot_size = 0;
    VkBuffer uniforms = VK_NULL_HANDLE;
    VkDeviceMemory uniform_memory = VK_NULL_HANDLE;
    uint8_t* uniform_mapped = NULL;
    VkDeviceSize uniform_slot = 0;
    VkDeviceSize uniform_offset = 0;
    uint32_t uniform_stride = 0;
    uint32_t draw_count = 0;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> target_sets;
    std::vector<uint32_t> first_index;
    std::vector<int32_t> vertex_offset;
    uint32_t quad_vertex = 0;
    uint32_t quad_index = 0;
    hfw::masks* drawable_masks = NULL;
    hfw::masks* offscreen_masks = NULL;
    std::vector<uint32_t> drawable_tiles;
    std::vector<uint32_t> offscreen_tiles;
    gpu_image mask;
    uint32_t mask_tiles = 1;
    uint32_t mask_side = 0;
    uint32_t mask_updates = UINT32_MAX;
    int32_t mask_used[2] = {0, 0};
    uint32_t shrink_frames = 0;
    uint32_t shrink_side = 0;
    bool blend = false;
    bool tiled = false;
    VkExtent2D extent{};
    gpu_image color;
    gpu_image resolved;
    gpu_image snapshot;
    std::vector<gpu_image> layers;
    bool fresh = false;
    uint64_t generation = 0;
    std::vector<int32_t> sorted;
    std::vector<int32_t> offscreen_parent;
    std::vector<int32_t> offscreen_layer;
    int32_t current = -1;
    int32_t open = -2;
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    int32_t bound_cull = -1;
    bool view = false;
};

void renderer_start(VkPhysicalDevice physical, VkDevice logical, VkQueue graphics, uint32_t family, VkCommandPool pool, VkFormat format, std::mutex* lock);
VkCommandBuffer renderer_once_begin();
void renderer_once_end(VkCommandBuffer commands);
VkShaderModule shader_module(const uint32_t* code, size_t size);
renderer* renderer_create(hfw::model* model, const std::vector<VkDescriptorImageInfo>& textures);
renderer* renderer_view(renderer* source);
void renderer_destroy(renderer* r);
VkCommandBuffer renderer_begin(renderer* r);
void renderer_draw(renderer* r, VkCommandBuffer commands, VkExtent2D extent, const float* mvp, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access);
void renderer_submit(renderer* r, VkSemaphore wait, VkPipelineStageFlags wait_stage, VkSemaphore signal);
