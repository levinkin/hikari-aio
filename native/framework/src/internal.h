#pragma once

#include "../include/hfw.h"
#include "hikari_core.h"

#include <string>
#include <vector>

namespace hfw {

constexpr float pi = 3.14159274f;

struct moc {
    void* buffer;
    size_t length;
    core_moc* handle;
    uint32_t model_size;
};

struct render_state {
    int32_t color_blend;
    int32_t alpha_blend;
    bool culling;
    bool inverted_mask;
};

struct side_slots {
    std::vector<std::string> identifiers;
    std::vector<float> values;
};

struct model {
    void* buffer;
    core_model* handle;

    float canvas_width_pixels;
    float canvas_height_pixels;
    float pixels_per_unit;

    int32_t parameter_count;
    const char** parameter_ids;
    const float* parameter_minimums;
    const float* parameter_maximums;
    const float* parameter_defaults;
    float* parameter_values;
    side_slots parameter_side;
    std::vector<float> saved_parameters;

    int32_t part_count;
    const char** part_ids;
    float* part_opacities;
    const int32_t* part_parents;
    side_slots part_side;

    int32_t drawable_count;
    const uint8_t* drawable_dynamic_flags;
    uint32_t updates;
    const int32_t* drawable_textures;
    const float* drawable_opacities;
    const int32_t* drawable_mask_counts;
    const int32_t** drawable_masks;
    const int32_t* drawable_vertex_counts;
    const float** drawable_vertices;
    const float** drawable_texture_coordinates;
    const int32_t* drawable_index_counts;
    const uint16_t** drawable_indices;
    const float* drawable_multiply;
    const float* drawable_screen;
    const int32_t* drawable_parents;

    int32_t offscreen_count;
    const int32_t* offscreen_owners;
    const int32_t* offscreen_mask_counts;
    const int32_t** offscreen_masks;
    const float* offscreen_opacities;
    const float* offscreen_multiply;
    const float* offscreen_screen;

    const int32_t* render_orders;
    std::vector<render_state> states;
    bool layered;
    float opacity;
    const moc* source;
    std::vector<float> positions;
    size_t vertex_bytes;
};

struct motion_entry {
    std::string file;
    float fade_in;
    float fade_out;
};

struct motion_group {
    std::string name;
    std::vector<motion_entry> entries;
};

struct setting {
    std::string moc;
    std::string physics;
    std::string pose;
    std::vector<std::string> textures;
    std::vector<std::string> expression_files;
    std::vector<motion_group> motion_groups;
    std::vector<std::string> eye_blink_ids;
    std::vector<std::string> lip_sync_ids;
};

void report(status* output_status, status value);
float ease(float progress);
void trigger_fade_end(float clock, float fade_out_time, float* end);
bool motion_evaluate(motion* source, model* owner, float time, bool* started, float* start, float* fade_start, float* end);
void blink_update_scaled(blink* target, model* owner, float delta_time);

}
