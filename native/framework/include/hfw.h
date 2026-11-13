#pragma once

#include <cstddef>
#include <cstdint>

namespace hfw {

enum class status : int32_t { ok, empty, json, inconsistent, bad_value, core, memory };

struct mat4 {
    float m[16];
};

struct setting;
struct moc;
struct model;
struct motion;
struct queue;
struct expression;
struct layer;
struct blink;
struct breath;
struct pose;
struct physics;
struct masks;

struct breath_wave {
    int32_t param;
    float offset, peak, cycle, weight;
};

struct mask_group {
    bool used;
    int32_t channel;
    float channel_rgba[4];
    float slot[4];
    mat4 mask_draw;
    mat4 sample;
    const int32_t* mask_drawables;
    int32_t mask_drawable_count;
};

struct rig {
    model* target;
    queue* motions;
    layer* const* layers;
    int32_t layer_count;
    blink* eye_blink;
    breath* breathing;
    physics* simulation;
    pose* pose_state;
    bool save_before_hook;
};

struct rig_hooks {
    void* context;
    void (*idle)(void* context);
    void (*before_motion)(void* context);
    void (*after_motion)(void* context);
    void (*before_physics)(void* context, float delta_time);
    void (*after_physics)(void* context, float delta_time);
    void (*before_model)(void* context);
};

setting* setting_parse(const uint8_t* bytes, size_t length, status* output_status);
void setting_destroy(setting* target);
const char* setting_moc(const setting* target);
int32_t setting_texture_count(const setting* target);
const char* setting_texture(const setting* target, int32_t index);
const char* setting_physics(const setting* target);
const char* setting_pose(const setting* target);
int32_t setting_expression_count(const setting* target);
const char* setting_expression_file(const setting* target, int32_t index);
int32_t setting_motion_group_count(const setting* target);
const char* setting_motion_group(const setting* target, int32_t group);
int32_t setting_motion_count(const setting* target, int32_t group);
const char* setting_motion_file(const setting* target, int32_t group, int32_t index);

moc* moc_load(const uint8_t* bytes, size_t length, status* output_status);
void moc_destroy(moc* target);
model* model_create(const moc* source, status* output_status);
void model_destroy(model* target);

float canvas_height_pixels(const model* target);
float pixels_per_unit(const model* target);

int32_t param_count(const model* target);
int32_t param_index(model* target, const char* identifier);
float param_get(const model* target, int32_t index);
float param_default(const model* target, int32_t index);
const char* param_at(const model* target, int32_t index, float* minimum, float* maximum);
void param_set(model* target, int32_t index, float value);
const float* param_values(const model* target);
void params_save(model* target);
void params_restore(model* target);

int32_t part_count(const model* target);
int32_t part_index(model* target, const char* identifier);
float part_opacity(const model* target, int32_t index);
void part_set_opacity(model* target, int32_t index, float value);
const float* part_opacities(const model* target);
int32_t part_parent(const model* target, int32_t part);

float model_opacity(const model* target);
void model_set_opacity(model* target, float value);
void model_update(model* target);
uint32_t model_updates(const model* target);
size_t vertex_bytes(const model* target);
void vertices_attach(model* target, float* first, float* second);
void vertices_release(model* target);
int32_t vertex_slot(const model* target);
const uint8_t* moc_bytes(const model* target, size_t* length);

int32_t drawable_count(const model* target);
int32_t drawable_vertex_count(const model* target, int32_t index);
int32_t drawable_index_count(const model* target, int32_t index);
const float* drawable_vertices(const model* target, int32_t index);
const float* drawable_uvs(const model* target, int32_t index);
const uint16_t* drawable_indices(const model* target, int32_t index);
int32_t drawable_texture(const model* target, int32_t index);
int32_t drawable_parent_part(const model* target, int32_t index);
float drawable_opacity(const model* target, int32_t index);
const float* drawable_multiply(const model* target, int32_t index);
const float* drawable_screen(const model* target, int32_t index);
bool drawable_visible(const model* target, int32_t index);
bool drawable_moved(const model* target, int32_t index);
int32_t drawable_color_blend(const model* target, int32_t index);
int32_t drawable_alpha_blend(const model* target, int32_t index);
bool drawable_culling(const model* target, int32_t index);
bool drawable_inverted_mask(const model* target, int32_t index);

int32_t offscreen_count(const model* target);
const int32_t* offscreen_owners(const model* target);
float offscreen_opacity(const model* target, int32_t index);
const float* offscreen_multiply(const model* target, int32_t index);
const float* offscreen_screen(const model* target, int32_t index);
int32_t offscreen_color_blend(const model* target, int32_t index);
int32_t offscreen_alpha_blend(const model* target, int32_t index);
bool offscreen_culling(const model* target, int32_t index);
bool offscreen_inverted_mask(const model* target, int32_t index);

const int32_t* render_orders(const model* target);
bool layered(const model* target);

motion* motion_load(model* target, const setting* source, const uint8_t* bytes, size_t length, int32_t group, int32_t entry, status* output_status);
void motion_destroy(motion* target);
float motion_duration(const motion* target);
float motion_fade_out(const motion* target);
void motion_set_loop(motion* target, bool loop, bool loop_fade_in);
int32_t motion_curve_count(const motion* target);
int32_t motion_curve_target(const motion* target, int32_t curve);
int32_t motion_curve_part(const motion* target, model* owner, int32_t curve);
int32_t motion_curve_param(const motion* target, model* owner, int32_t curve);
int32_t motion_curve_number_count(const motion* target, int32_t curve);
float motion_curve_number(const motion* target, int32_t curve, int32_t index);

queue* queue_create();
void queue_destroy(queue* target);
void queue_start(queue* target, motion* source);
bool queue_empty(const queue* target);
bool queue_update(queue* target, model* owner, float delta_time);
void queue_fade_out_all(queue* target);
int32_t queue_entry_count(const queue* target);
const motion* queue_entry_motion(const queue* target, int32_t index);
bool queue_entry_fade_out_triggered(const queue* target, int32_t index);
float queue_entry_end_time(const queue* target, int32_t index);
float queue_entry_state_time(const queue* target, int32_t index);
void queue_look_ahead(queue* target, model* owner, float seconds);

expression* expression_load(model* target, const uint8_t* bytes, size_t length, status* output_status);
void expression_destroy(expression* target);
void expression_set_fades(expression* target, float fade_in, float fade_out);
layer* layer_create();
void layer_destroy(layer* target);
void layer_start(layer* target, expression* source);
void layer_update(layer* target, model* owner, float delta_time);

blink* blink_create(model* target, const setting* source);
void blink_destroy(blink* target);
int32_t blink_param_count(const blink* target);
const int32_t* blink_params(const blink* target);
void blink_update(blink* target, model* owner, float delta_time);

breath* breath_create();
void breath_destroy(breath* target);
void breath_set(breath* target, const breath_wave* waves, int32_t count);
void breath_update(breath* target, model* owner, float delta_time);

pose* pose_load(model* target, const uint8_t* bytes, size_t length, status* output_status);
void pose_update(pose* target, model* owner, float delta_time);
void pose_destroy(pose* target);

physics* physics_load(model* target, const uint8_t* bytes, size_t length, status* output_status);
void physics_destroy(physics* target);
void physics_set_wind(physics* target, float horizontal, float vertical);
void physics_set_gravity(physics* target, float horizontal, float vertical);
void physics_evaluate(physics* target, model* owner, float delta_time);

void rig_update(rig* target, float delta_time, const rig_hooks* hooks);

masks* masks_create(const model* source, int kind, status* result);
void masks_destroy(masks* target);
int32_t masks_layout(masks* target, const model* source, bool tile_path);
int32_t masks_group_count(const masks* target);
int32_t masks_group_of(const masks* target, int32_t object);
const mask_group* masks_group(const masks* target, int32_t group);

}
