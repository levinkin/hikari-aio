#include "internal.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace hfw {

void report(status* output_status, status value)
{
    if (output_status) {
        *output_status = value;
    }
}

static int32_t slot_index(const char* const* identifiers, int32_t count, side_slots& side, const char* identifier)
{
    for (int32_t i = 0; i < count; i++) {
        if (std::strcmp(identifiers[i], identifier) == 0) {
            return i;
        }
    }
    int32_t side_count = static_cast<int32_t>(side.identifiers.size());
    for (int32_t i = 0; i < side_count; i++) {
        if (side.identifiers[i] == identifier) {
            return count + i;
        }
    }
    side.identifiers.emplace_back(identifier);
    side.values.push_back(0.0f);
    return count + side_count;
}

static void* map_pages(size_t length)
{
#if defined(_WIN32)
    return VirtualAlloc(nullptr, length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* pages = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return pages == MAP_FAILED ? nullptr : pages;
#endif
}

static void unmap_pages(void* pages, size_t length)
{
    if (!pages) {
        return;
    }
#if defined(_WIN32)
    VirtualFree(pages, 0, MEM_RELEASE);
#else
    munmap(pages, length);
#endif
}

static render_state decode_state(int32_t blend_word, uint8_t flags)
{
    render_state state;
    state.color_blend = blend_word & 0xff;
    state.alpha_blend = (blend_word >> 8) & 0xff;
    state.culling = (flags & 4) == 0;
    state.inverted_mask = (flags & 8) != 0;
    return state;
}

moc* moc_load(const uint8_t* bytes, size_t length, status* output_status)
{
    if (!bytes || length == 0) {
        report(output_status, status::empty);
        return nullptr;
    }
    if (length > UINT32_MAX - 63) {
        report(output_status, status::core);
        return nullptr;
    }
    size_t padded = (length + 16383) & ~static_cast<size_t>(16383);
    void* buffer = map_pages(padded);
    moc* result = buffer ? new (std::nothrow) moc : nullptr;
    if (!result) {
        unmap_pages(buffer, padded);
        report(output_status, status::memory);
        return nullptr;
    }
    std::memcpy(buffer, bytes, length);
    unsigned int size = static_cast<unsigned int>(length);
    core_moc* handle = hc_moc_consistent(buffer, size) ? hc_revive_moc(buffer, size) : nullptr;
    uint32_t model_size = handle ? hc_model_size(handle) : 0;
    if (model_size == 0) {
        unmap_pages(buffer, padded);
        delete result;
        report(output_status, status::core);
        return nullptr;
    }
    result->buffer = buffer;
    result->length = padded;
    result->handle = handle;
    result->model_size = model_size;
    report(output_status, status::ok);
    return result;
}

void moc_destroy(moc* target)
{
    if (target) {
        unmap_pages(target->buffer, target->length);
        delete target;
    }
}

model* model_create(const moc* source, status* output_status)
{
    uint32_t size = (source->model_size + 15) & ~15u;
    void* buffer = ::operator new(size, std::align_val_t{16}, std::nothrow);
    model* result = buffer ? new (std::nothrow) model() : nullptr;
    if (!result) {
        ::operator delete(buffer, std::align_val_t{16});
        report(output_status, status::memory);
        return nullptr;
    }
    result->vertex_bytes = hc_vertex_position_size(source->handle);
    result->positions.resize(std::max<size_t>(result->vertex_bytes / sizeof(float), 1));
    result->source = source;
    core_model* handle = hc_initialize_model(source->handle, buffer, size, result->positions.data());
    if (!handle) {
        ::operator delete(buffer, std::align_val_t{16});
        delete result;
        report(output_status, status::core);
        return nullptr;
    }
    result->buffer = buffer;
    result->handle = handle;

    float canvas_size[2];
    float origin[2];
    hc_read_canvas(handle, canvas_size, origin, &result->pixels_per_unit);
    result->canvas_width_pixels = canvas_size[0];
    result->canvas_height_pixels = canvas_size[1];

    result->parameter_count = hc_parameter_count(handle);
    result->parameter_ids = hc_parameter_ids(handle);
    result->parameter_minimums = hc_parameter_minimum_values(handle);
    result->parameter_maximums = hc_parameter_maximum_values(handle);
    result->parameter_defaults = hc_parameter_default_values(handle);
    result->parameter_values = hc_parameter_values(handle);

    result->part_count = hc_part_count(handle);
    result->part_ids = hc_part_ids(handle);
    result->part_opacities = hc_part_opacities(handle);
    result->part_parents = hc_part_parent_indices(handle);

    result->drawable_count = hc_drawable_count(handle);
    result->drawable_dynamic_flags = hc_drawable_dynamic_flags(handle);
    result->drawable_textures = hc_drawable_texture_indices(handle);
    result->drawable_opacities = hc_drawable_opacities(handle);
    result->drawable_mask_counts = hc_drawable_mask_counts(handle);
    result->drawable_masks = hc_drawable_masks(handle);
    result->drawable_vertex_counts = hc_drawable_vertex_counts(handle);
    result->drawable_vertices = hc_drawable_vertex_positions(handle);
    result->drawable_texture_coordinates = hc_drawable_vertex_uvs(handle);
    result->drawable_index_counts = hc_drawable_index_counts(handle);
    result->drawable_indices = hc_drawable_indices(handle);
    result->drawable_multiply = hc_drawable_multiply_colors(handle);
    result->drawable_screen = hc_drawable_screen_colors(handle);
    result->drawable_parents = hc_drawable_parent_part_indices(handle);

    result->offscreen_count = hc_offscreen_count(handle);
    result->offscreen_owners = hc_offscreen_owner_indices(handle);
    result->offscreen_mask_counts = hc_offscreen_mask_counts(handle);
    result->offscreen_masks = hc_offscreen_masks(handle);
    result->offscreen_opacities = hc_offscreen_opacities(handle);
    result->offscreen_multiply = hc_offscreen_multiply_colors(handle);
    result->offscreen_screen = hc_offscreen_screen_colors(handle);

    result->render_orders = hc_render_orders(handle);

    if (result->parameter_count < 0 || result->part_count < 0 || result->drawable_count < 0 || result->offscreen_count < 0) {
        model_destroy(result);
        report(output_status, status::core);
        return nullptr;
    }

    result->saved_parameters.assign(result->parameter_values, result->parameter_values + result->parameter_count);

    const int32_t* drawable_blends = hc_drawable_blend_modes(handle);
    const uint8_t* drawable_flags = hc_drawable_constant_flags(handle);
    const int32_t* offscreen_blends = hc_offscreen_blend_modes(handle);
    const uint8_t* offscreen_flags = hc_offscreen_constant_flags(handle);
    result->states.reserve(static_cast<size_t>(result->drawable_count) + static_cast<size_t>(result->offscreen_count));
    result->layered = result->offscreen_count > 0;
    for (int32_t drawable = 0; drawable < result->drawable_count; drawable++) {
        render_state state = decode_state(drawable_blends[drawable], drawable_flags[drawable]);
        bool fixed = (state.color_blend == 0 && state.alpha_blend == 0) || state.color_blend == 1 || state.color_blend == 2;
        if (!fixed) {
            result->layered = true;
        }
        result->states.push_back(state);
    }
    for (int32_t offscreen = 0; offscreen < result->offscreen_count; offscreen++) {
        result->states.push_back(decode_state(offscreen_blends ? offscreen_blends[offscreen] : 0, offscreen_flags[offscreen]));
    }

    result->opacity = 1.0f;
    report(output_status, status::ok);
    return result;
}

void model_destroy(model* target)
{
    if (target) {
        ::operator delete(target->buffer, std::align_val_t{16});
        delete target;
    }
}

float canvas_height_pixels(const model* target)
{
    return target->canvas_height_pixels;
}

float pixels_per_unit(const model* target)
{
    return target->pixels_per_unit;
}

int32_t param_count(const model* target)
{
    return target->parameter_count;
}

int32_t param_index(model* target, const char* identifier)
{
    return slot_index(target->parameter_ids, target->parameter_count, target->parameter_side, identifier);
}

float param_get(const model* target, int32_t index)
{
    if (index < target->parameter_count) {
        return target->parameter_values[index];
    }
    return target->parameter_side.values[index - target->parameter_count];
}

float param_default(const model* target, int32_t index)
{
    return target->parameter_defaults[index];
}

const char* param_at(const model* target, int32_t index, float* minimum, float* maximum)
{
    *minimum = target->parameter_minimums[index];
    *maximum = target->parameter_maximums[index];
    return target->parameter_ids[index];
}

void param_set(model* target, int32_t index, float value)
{
    if (index >= target->parameter_count) {
        target->parameter_side.values[index - target->parameter_count] = value;
        return;
    }
    if (value < target->parameter_minimums[index]) {
        value = target->parameter_minimums[index];
    } else if (value > target->parameter_maximums[index]) {
        value = target->parameter_maximums[index];
    }
    target->parameter_values[index] = value;
}

const float* param_values(const model* target)
{
    return target->parameter_values;
}

void params_save(model* target)
{
    std::memcpy(target->saved_parameters.data(), target->parameter_values, sizeof(float) * target->saved_parameters.size());
}

void params_restore(model* target)
{
    std::memcpy(target->parameter_values, target->saved_parameters.data(), sizeof(float) * target->saved_parameters.size());
}

int32_t part_count(const model* target)
{
    return target->part_count;
}

int32_t part_index(model* target, const char* identifier)
{
    return slot_index(target->part_ids, target->part_count, target->part_side, identifier);
}

float part_opacity(const model* target, int32_t index)
{
    if (index < target->part_count) {
        return target->part_opacities[index];
    }
    return target->part_side.values[index - target->part_count];
}

void part_set_opacity(model* target, int32_t index, float value)
{
    if (index < target->part_count) {
        target->part_opacities[index] = value;
    } else {
        target->part_side.values[index - target->part_count] = value;
    }
}

const float* part_opacities(const model* target)
{
    return target->part_opacities;
}

int32_t part_parent(const model* target, int32_t part)
{
    return target->part_parents[part];
}

float model_opacity(const model* target)
{
    return target->opacity;
}

void model_set_opacity(model* target, float value)
{
    target->opacity = value;
}

void model_update(model* target)
{
    hc_update_model(target->handle);
    target->updates++;
}

uint32_t model_updates(const model* target)
{
    return target->updates;
}

size_t vertex_bytes(const model* target)
{
    return target->vertex_bytes;
}

void vertices_attach(model* target, float* first, float* second)
{
    hc_attach_vertex_positions(target->handle, first, second);
    std::vector<float>().swap(target->positions);
}

void vertices_release(model* target)
{
    hc_release_vertex_slot(target->handle);
}

int32_t vertex_slot(const model* target)
{
    return hc_vertex_slot(target->handle);
}

const uint8_t* moc_bytes(const model* target, size_t* length)
{
    *length = target->source->length;
    return static_cast<const uint8_t*>(target->source->buffer);
}

int32_t drawable_count(const model* target)
{
    return target->drawable_count;
}

int32_t drawable_vertex_count(const model* target, int32_t index)
{
    return target->drawable_vertex_counts[index];
}

int32_t drawable_index_count(const model* target, int32_t index)
{
    return target->drawable_index_counts[index];
}

const float* drawable_vertices(const model* target, int32_t index)
{
    return target->drawable_vertices[index];
}

const float* drawable_uvs(const model* target, int32_t index)
{
    return target->drawable_texture_coordinates[index];
}

const uint16_t* drawable_indices(const model* target, int32_t index)
{
    return target->drawable_indices[index];
}

int32_t drawable_texture(const model* target, int32_t index)
{
    return target->drawable_textures[index];
}

int32_t drawable_parent_part(const model* target, int32_t index)
{
    return target->drawable_parents[index];
}

float drawable_opacity(const model* target, int32_t index)
{
    return target->drawable_opacities[index];
}

const float* drawable_multiply(const model* target, int32_t index)
{
    return target->drawable_multiply + 4 * index;
}

const float* drawable_screen(const model* target, int32_t index)
{
    return target->drawable_screen + 4 * index;
}

bool drawable_visible(const model* target, int32_t index)
{
    return (target->drawable_dynamic_flags[index] & 1) != 0;
}

bool drawable_moved(const model* target, int32_t index)
{
    return (target->drawable_dynamic_flags[index] & 32) != 0;
}

int32_t drawable_color_blend(const model* target, int32_t index)
{
    return target->states[index].color_blend;
}

int32_t drawable_alpha_blend(const model* target, int32_t index)
{
    return target->states[index].alpha_blend;
}

bool drawable_culling(const model* target, int32_t index)
{
    return target->states[index].culling;
}

bool drawable_inverted_mask(const model* target, int32_t index)
{
    return target->states[index].inverted_mask;
}

int32_t offscreen_count(const model* target)
{
    return target->offscreen_count;
}

const int32_t* offscreen_owners(const model* target)
{
    return target->offscreen_owners;
}

float offscreen_opacity(const model* target, int32_t index)
{
    if (index < 0 || index >= target->offscreen_count) {
        return 1.0f;
    }
    return target->offscreen_opacities[index];
}

const float* offscreen_multiply(const model* target, int32_t index)
{
    return target->offscreen_multiply + 4 * index;
}

const float* offscreen_screen(const model* target, int32_t index)
{
    return target->offscreen_screen + 4 * index;
}

int32_t offscreen_color_blend(const model* target, int32_t index)
{
    return target->states[target->drawable_count + index].color_blend;
}

int32_t offscreen_alpha_blend(const model* target, int32_t index)
{
    return target->states[target->drawable_count + index].alpha_blend;
}

bool offscreen_culling(const model* target, int32_t index)
{
    return target->states[target->drawable_count + index].culling;
}

bool offscreen_inverted_mask(const model* target, int32_t index)
{
    return target->states[target->drawable_count + index].inverted_mask;
}

const int32_t* render_orders(const model* target)
{
    return target->render_orders;
}

bool layered(const model* target)
{
    return target->layered;
}

}
