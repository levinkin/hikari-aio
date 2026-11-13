#include "internal.h"
#include "json.h"

#include <cmath>
#include <new>

namespace hfw {

enum blend_kind : int32_t { blend_add, blend_multiply, blend_overwrite };

struct expression_item {
    int32_t parameter;
    int32_t blend;
    float value;
};

struct expression {
    float fade_in;
    float fade_out;
    std::vector<expression_item> items;
    int32_t references;
    bool released;
};

struct layer_entry {
    expression* source;
    std::vector<int32_t> item_of_slot;
    bool started;
    bool triggered;
    float fade_start;
    float end;
    float fade_out_time;
};

struct layer_slot {
    int32_t parameter;
    float add;
    float multiply;
    float overwrite;
};

struct layer {
    float clock;
    std::vector<layer_entry> entries;
    std::vector<layer_slot> slots;
};

expression* expression_load(model* target, const uint8_t* bytes, size_t length, status* output_status)
{
    if (!bytes || length == 0) {
        report(output_status, status::empty);
        return nullptr;
    }
    expression* result = new (std::nothrow) expression();
    if (!result) {
        report(output_status, status::memory);
        return nullptr;
    }
    json_document document;
    if (!json_parse(document, bytes, length)) {
        result->fade_in = -1.0f;
        result->fade_out = -1.0f;
        report(output_status, status::json);
        return result;
    }
    int32_t root = json_root(document);
    result->fade_in = json_float(document, json_find(document, root, "FadeInTime"), 1.0f, 1.0f);
    result->fade_out = json_float(document, json_find(document, root, "FadeOutTime"), 1.0f, 1.0f);
    int32_t parameters = json_find(document, root, "Parameters");
    int32_t count = json_size(document, parameters);
    result->items.resize(count);
    for (int32_t i = 0; i < count; i++) {
        int32_t node = json_at(document, parameters, i);
        std::string text;
        json_text(document, json_find(document, node, "Id"), text);
        expression_item& item = result->items[i];
        item.parameter = param_index(target, text.c_str());
        item.value = json_float(document, json_find(document, node, "Value"), 0.0f, 0.0f);
        text.clear();
        json_text(document, json_find(document, node, "Blend"), text);
        item.blend = text == "Multiply" ? blend_multiply : (text == "Overwrite" ? blend_overwrite : blend_add);
    }
    report(output_status, status::ok);
    return result;
}

static void expression_release(expression* target)
{
    target->references--;
    if (target->released && target->references == 0) {
        delete target;
    }
}

void expression_destroy(expression* target)
{
    target->released = true;
    if (target->references == 0) {
        delete target;
    }
}

void expression_set_fades(expression* target, float fade_in, float fade_out)
{
    target->fade_in = fade_in;
    target->fade_out = fade_out;
}

layer* layer_create()
{
    return new (std::nothrow) layer();
}

void layer_destroy(layer* target)
{
    if (!target) {
        return;
    }
    for (layer_entry& entry : target->entries) {
        expression_release(entry.source);
    }
    delete target;
}

void layer_start(layer* target, expression* source)
{
    for (layer_entry& entry : target->entries) {
        entry.triggered = true;
        entry.fade_out_time = entry.source->fade_out;
    }
    std::vector<int32_t> slot_of_item;
    for (const expression_item& item : source->items) {
        int32_t slot = 0;
        int32_t slot_count = static_cast<int32_t>(target->slots.size());
        while (slot < slot_count && target->slots[slot].parameter != item.parameter) {
            slot++;
        }
        if (slot == slot_count) {
            target->slots.push_back({ item.parameter, 0.0f, 1.0f, 0.0f });
        }
        slot_of_item.push_back(slot);
    }
    layer_entry entry = {};
    entry.source = source;
    source->references++;
    entry.end = -1.0f;
    entry.item_of_slot.assign(target->slots.size(), -1);
    for (int32_t i = static_cast<int32_t>(slot_of_item.size()) - 1; i >= 0; i--) {
        entry.item_of_slot[slot_of_item[i]] = i;
    }
    target->entries.push_back(std::move(entry));
}

void layer_update(layer* target, model* owner, float delta_time)
{
    target->clock = target->clock + delta_time;
    float clock = target->clock;
    float entry_weight = 0.0f;
    float newest_weight = 0.0f;
    int32_t entry_index = 0;
    for (layer_entry& entry : target->entries) {
        const expression& source = *entry.source;
        if (!entry.started) {
            entry.started = true;
            entry.fade_start = clock;
            entry.end = -1.0f;
        }
        float fade_in = source.fade_in == 0.0f ? 1.0f : ease((clock - entry.fade_start) / source.fade_in);
        float fade_out = source.fade_out == 0.0f || entry.end < 0.0f ? 1.0f : ease((entry.end - clock) / source.fade_out);
        float weight = fade_in * fade_out;
        float rest = 1.0f - weight;
        int32_t slot_count = static_cast<int32_t>(target->slots.size());
        int32_t mapped = static_cast<int32_t>(entry.item_of_slot.size());
        for (int32_t slot_index = 0; slot_index < slot_count; slot_index++) {
            layer_slot& slot = target->slots[slot_index];
            float current = param_get(owner, slot.parameter);
            float add = 0.0f;
            float multiply = 1.0f;
            float overwrite = current;
            int32_t item = slot_index < mapped ? entry.item_of_slot[slot_index] : -1;
            if (item >= 0) {
                const expression_item& named = source.items[item];
                if (named.blend == blend_add) {
                    add = named.value;
                } else if (named.blend == blend_multiply) {
                    multiply = named.value;
                } else {
                    overwrite = named.value;
                }
            }
            if (entry_index == 0) {
                slot.add = add;
                slot.multiply = multiply;
                slot.overwrite = overwrite;
            } else {
                slot.add = std::fma(slot.add, rest, add * weight);
                slot.multiply = std::fma(slot.multiply, rest, multiply * weight);
                slot.overwrite = std::fma(current, rest, overwrite * weight);
            }
        }
        entry_weight = entry_weight + fade_in;
        if (entry.triggered) {
            trigger_fade_end(clock, entry.fade_out_time, &entry.end);
        }
        newest_weight = weight;
        entry_index++;
    }
    if (target->entries.size() > 1 && newest_weight >= 1.0f) {
        for (auto entry = target->entries.begin(); entry != target->entries.end() - 1; ++entry) {
            expression_release(entry->source);
        }
        target->entries.erase(target->entries.begin(), target->entries.end() - 1);
    }
    entry_weight = entry_weight < 1.0f ? entry_weight : 1.0f;
    for (layer_slot& slot : target->slots) {
        float value = (slot.overwrite + slot.add) * slot.multiply;
        float current = param_get(owner, slot.parameter);
        if (slot.parameter < owner->parameter_count) {
            float low = owner->parameter_minimums[slot.parameter];
            float high = owner->parameter_maximums[slot.parameter];
            value = value < low ? low : (value > high ? high : value);
            owner->parameter_values[slot.parameter] = entry_weight == 1.0f ? value : std::fma(current, 1.0f - entry_weight, value * entry_weight);
        } else {
            owner->parameter_side.values[slot.parameter - owner->parameter_count] = entry_weight == 1.0f ? value : std::fma(current, 1.0f - entry_weight, value * entry_weight);
        }
        slot.add = 0.0f;
        slot.multiply = 1.0f;
    }
}

}
