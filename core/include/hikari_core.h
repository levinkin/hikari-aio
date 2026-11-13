#pragma once

#include <stdint.h>

typedef struct core_moc core_moc;
typedef struct core_model core_model;

#ifdef __cplusplus
extern "C" {
#endif

int hc_moc_consistent(void* address, unsigned int size);
core_moc* hc_revive_moc(void* address, unsigned int size);
unsigned int hc_model_size(const core_moc* moc);
unsigned int hc_vertex_position_size(const core_moc* moc);
core_model* hc_initialize_model(const core_moc* moc, void* address, unsigned int size, float* positions);
void hc_attach_vertex_positions(core_model* model, float* first, float* second);
void hc_release_vertex_slot(core_model* model);
int hc_vertex_slot(const core_model* model);
void hc_update_model(core_model* model);
void hc_read_canvas(const core_model* model, float* size, float* origin, float* pixels_per_unit);
const int32_t* hc_render_orders(const core_model* model);

int hc_parameter_count(const core_model* model);
const char** hc_parameter_ids(const core_model* model);
const float* hc_parameter_minimum_values(const core_model* model);
const float* hc_parameter_maximum_values(const core_model* model);
const float* hc_parameter_default_values(const core_model* model);
float* hc_parameter_values(core_model* model);

int hc_part_count(const core_model* model);
const char** hc_part_ids(const core_model* model);
float* hc_part_opacities(core_model* model);
const int32_t* hc_part_parent_indices(const core_model* model);

int hc_drawable_count(const core_model* model);
const uint8_t* hc_drawable_constant_flags(const core_model* model);
const uint8_t* hc_drawable_dynamic_flags(const core_model* model);
const int32_t* hc_drawable_blend_modes(const core_model* model);
const int32_t* hc_drawable_texture_indices(const core_model* model);
const float* hc_drawable_opacities(const core_model* model);
const int32_t* hc_drawable_mask_counts(const core_model* model);
const int32_t** hc_drawable_masks(const core_model* model);
const int32_t* hc_drawable_vertex_counts(const core_model* model);
const float** hc_drawable_vertex_positions(const core_model* model);
const float** hc_drawable_vertex_uvs(const core_model* model);
const int32_t* hc_drawable_index_counts(const core_model* model);
const uint16_t** hc_drawable_indices(const core_model* model);
const float* hc_drawable_multiply_colors(const core_model* model);
const float* hc_drawable_screen_colors(const core_model* model);
const int32_t* hc_drawable_parent_part_indices(const core_model* model);

int hc_offscreen_count(const core_model* model);
const int32_t* hc_offscreen_owner_indices(const core_model* model);
const int32_t* hc_offscreen_blend_modes(const core_model* model);
const uint8_t* hc_offscreen_constant_flags(const core_model* model);
const int32_t* hc_offscreen_mask_counts(const core_model* model);
const int32_t** hc_offscreen_masks(const core_model* model);
const float* hc_offscreen_opacities(const core_model* model);
const float* hc_offscreen_multiply_colors(const core_model* model);
const float* hc_offscreen_screen_colors(const core_model* model);

#ifdef __cplusplus
}
#endif
