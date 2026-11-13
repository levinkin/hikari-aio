#include "internal.h"

#include <limits>
#include <new>

namespace hfw {

constexpr int32_t atlas_capacity = 36;
constexpr int32_t never_assigned = -1;
constexpr int32_t tile_assigned = -2;
constexpr int32_t over_capacity_assigned = atlas_capacity + 1;

struct masks {
    std::vector<int32_t> group_of;
    std::vector<mask_group> groups;
    std::vector<std::vector<int32_t>> identifiers;
    std::vector<std::vector<int32_t>> bounded;
    std::vector<float> bounds;
    int32_t assigned;
};

static bool matches(const std::vector<int32_t>& group_identifiers, const int32_t* list, int32_t count)
{
    if (static_cast<int32_t>(group_identifiers.size()) != count) {
        return false;
    }
    for (int32_t identifier : group_identifiers) {
        bool found = false;
        for (int32_t i = 0; i < count && !found; i++) {
            found = list[i] == identifier;
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static bool under_part(const model* source, int32_t drawable, int32_t owner)
{
    int32_t part = source->drawable_parents[drawable];
    for (int32_t steps = 0; part >= 0 && part < source->part_count && steps <= source->part_count; steps++) {
        if (part == owner) {
            return true;
        }
        part = source->part_parents[part];
    }
    return false;
}

static void assign(mask_group& group, int32_t channel, float horizontal, float vertical, float width, float height)
{
    group.channel = channel;
    for (int32_t lane = 0; lane < 4; lane++) {
        group.channel_rgba[lane] = lane == channel ? 1.0f : 0.0f;
    }
    group.slot[0] = horizontal;
    group.slot[1] = vertical;
    group.slot[2] = width;
    group.slot[3] = height;
}

static void assign_atlas(masks* target, int32_t used_count)
{
    int32_t base = used_count / 4;
    int32_t extra = used_count % 4;
    int32_t position = 0;
    for (int32_t channel = 0; channel < 4; channel++) {
        int32_t held = base + (channel < extra ? 1 : 0);
        for (int32_t i = 0; i < held; i++, position++) {
            mask_group& group = target->groups[position];
            if (held == 1) {
                assign(group, channel, 0.0f, 0.0f, 1.0f, 1.0f);
            } else if (held == 2) {
                assign(group, channel, 0.5f * static_cast<float>(i % 2), 0.0f, 0.5f, 1.0f);
            } else if (held <= 4) {
                assign(group, channel, 0.5f * static_cast<float>(i % 2), 0.5f * static_cast<float>(i / 2), 0.5f, 0.5f);
            } else {
                float third = 1.0f / 3.0f;
                assign(group, channel, static_cast<float>(i % 3) / 3.0f, static_cast<float>(i / 3) / 3.0f, third, third);
            }
        }
    }
}

masks* masks_create(const model* source, int kind, status* result)
{
    if (kind != 0 && kind != 1) {
        report(result, status::bad_value);
        return nullptr;
    }
    int32_t count = kind == 0 ? source->drawable_count : source->offscreen_count;
    const int32_t* counts = kind == 0 ? source->drawable_mask_counts : source->offscreen_mask_counts;
    const int32_t* const* lists = kind == 0 ? source->drawable_masks : source->offscreen_masks;
    bool any = false;
    for (int32_t object = 0; object < count; object++) {
        for (int32_t i = 0; i < counts[object]; i++) {
            if (lists[object][i] < 0 || lists[object][i] >= source->drawable_count) {
                report(result, status::bad_value);
                return nullptr;
            }
        }
        any = any || counts[object] > 0;
    }
    if (!any) {
        report(result, status::ok);
        return nullptr;
    }
    masks* target = new (std::nothrow) masks();
    if (!target) {
        report(result, status::memory);
        return nullptr;
    }
    target->assigned = never_assigned;
    target->group_of.assign(count, -1);
    std::vector<std::vector<int32_t>> members;
    for (int32_t object = 0; object < count; object++) {
        if (counts[object] == 0) {
            continue;
        }
        int32_t group_count = static_cast<int32_t>(target->identifiers.size());
        int32_t group = 0;
        while (group < group_count && !matches(target->identifiers[group], lists[object], counts[object])) {
            group++;
        }
        if (group == group_count) {
            target->identifiers.emplace_back(lists[object], lists[object] + counts[object]);
            members.emplace_back();
        }
        members[group].push_back(object);
        target->group_of[object] = group;
    }
    int32_t group_count = static_cast<int32_t>(target->identifiers.size());
    target->bounded.resize(group_count);
    target->bounds.assign(group_count * 4, 0.0f);
    target->groups.resize(group_count);
    for (int32_t group = 0; group < group_count; group++) {
        for (int32_t object : members[group]) {
            if (kind == 0) {
                target->bounded[group].push_back(object);
                continue;
            }
            for (int32_t drawable = 0; drawable < source->drawable_count; drawable++) {
                if (under_part(source, drawable, source->offscreen_owners[object])) {
                    target->bounded[group].push_back(drawable);
                }
            }
        }
        mask_group& entry = target->groups[group];
        entry.used = false;
        assign(entry, 0, 0.0f, 0.0f, 1.0f, 1.0f);
        for (int32_t i = 0; i < 16; i++) {
            entry.mask_draw.m[i] = i % 5 == 0 ? 1.0f : 0.0f;
            entry.sample.m[i] = i % 5 == 0 ? 1.0f : 0.0f;
        }
        entry.mask_drawables = target->identifiers[group].data();
        entry.mask_drawable_count = static_cast<int32_t>(target->identifiers[group].size());
    }
    report(result, status::ok);
    return target;
}

void masks_destroy(masks* target)
{
    delete target;
}

static int32_t measure(masks* target, const model* source)
{
    const float largest = std::numeric_limits<float>::max();
    int32_t used_count = 0;
    int32_t group_count = static_cast<int32_t>(target->groups.size());
    for (int32_t group = 0; group < group_count; group++) {
        float min_x = largest, min_y = largest, max_x = -largest, max_y = -largest;
        bool used = false;
        for (int32_t drawable : target->bounded[group]) {
            const float* vertices = source->drawable_vertices[drawable];
            int32_t vertex_count = source->drawable_vertex_counts[drawable];
            float low_x = largest, low_y = largest, high_x = -largest, high_y = -largest;
            for (int32_t i = 0; i < vertex_count; i++) {
                float horizontal = vertices[2 * i];
                float vertical = vertices[2 * i + 1];
                if (horizontal < low_x) {
                    low_x = horizontal;
                }
                if (horizontal > high_x) {
                    high_x = horizontal;
                }
                if (vertical < low_y) {
                    low_y = vertical;
                }
                if (vertical > high_y) {
                    high_y = vertical;
                }
            }
            if (low_x == largest) {
                continue;
            }
            used = true;
            if (low_x < min_x) {
                min_x = low_x;
            }
            if (high_x > max_x) {
                max_x = high_x;
            }
            if (low_y < min_y) {
                min_y = low_y;
            }
            if (high_y > max_y) {
                max_y = high_y;
            }
        }
        float* bounds = &target->bounds[group * 4];
        bounds[0] = used ? min_x : 0.0f;
        bounds[1] = used ? min_y : 0.0f;
        bounds[2] = used ? max_x - min_x : 0.0f;
        bounds[3] = used ? max_y - min_y : 0.0f;
        target->groups[group].used = used;
        used_count += used ? 1 : 0;
    }
    return used_count;
}

int32_t masks_layout(masks* target, const model* source, bool tile_path)
{
    int32_t used_count = measure(target, source);
    int32_t group_count = static_cast<int32_t>(target->groups.size());
    int32_t key = tile_path ? tile_assigned : (used_count > atlas_capacity ? over_capacity_assigned : used_count);
    if (key != target->assigned) {
        if (key > 0 && key <= atlas_capacity) {
            assign_atlas(target, used_count);
        } else {
            for (mask_group& group : target->groups) {
                assign(group, 0, 0.0f, 0.0f, 1.0f, 1.0f);
            }
        }
        target->assigned = key;
    }
    float pixels_per_unit = source->pixels_per_unit;
    for (int32_t group = 0; group < group_count; group++) {
        mask_group& entry = target->groups[group];
        const float* bounds = &target->bounds[group * 4];
        float origin[2];
        float scale[2];
        for (int32_t axis = 0; axis < 2; axis++) {
            float start = bounds[axis];
            float extent = bounds[axis + 2];
            float slot_extent = entry.slot[axis + 2];
            float pixels = slot_extent * 256.0f;
            if (!tile_path || extent * pixels_per_unit > pixels) {
                float margin = extent * 0.05f;
                origin[axis] = start - margin;
                scale[axis] = slot_extent / (extent + 2.0f * margin);
            } else {
                origin[axis] = start;
                scale[axis] = pixels_per_unit / pixels;
            }
        }
        for (int32_t axis = 0; axis < 2; axis++) {
            float slot_start = entry.slot[axis];
            entry.mask_draw.m[axis * 5] = 2.0f * scale[axis];
            entry.mask_draw.m[12 + axis] = -(origin[axis] * 2.0f * scale[axis]) + (2.0f * slot_start - 1.0f);
            entry.sample.m[axis * 5] = scale[axis];
            entry.sample.m[12 + axis] = -(origin[axis] * scale[axis]) + slot_start;
        }
    }
    return used_count;
}

int32_t masks_group_count(const masks* target)
{
    return static_cast<int32_t>(target->groups.size());
}

int32_t masks_group_of(const masks* target, int32_t object)
{
    return target->group_of[object];
}

const mask_group* masks_group(const masks* target, int32_t group)
{
    return &target->groups[group];
}

}
