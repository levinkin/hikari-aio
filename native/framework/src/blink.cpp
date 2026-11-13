#include "internal.h"

#include <cmath>
#include <cstdlib>

namespace hfw {

enum class blink_phase { start, waiting, lowering, shut, raising };

struct blink {
    std::vector<int32_t> parameters;
    std::vector<int32_t> distinct;
    blink_phase phase;
    float clock;
    float phase_start;
    float next_blink;
};

static const float interval = 4.0f;
static const float lowering_time = 0.1f;
static const float shut_time = 0.05f;
static const float raising_time = 0.15f;

static float draw_next(float clock)
{
    float ratio = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
    return std::fma(ratio, std::fma(interval, 2.0f, -1.0f), clock);
}

static float advance(blink* target, float delta_time)
{
    target->clock = target->clock + delta_time;
    float clock = target->clock;
    float value = 1.0f;
    float progress;
    switch (target->phase) {
    case blink_phase::start:
        target->phase = blink_phase::waiting;
        target->next_blink = draw_next(clock);
        break;
    case blink_phase::waiting:
        if (target->next_blink < clock) {
            target->phase = blink_phase::lowering;
            target->phase_start = clock;
        }
        break;
    case blink_phase::lowering:
        progress = (clock - target->phase_start) / lowering_time;
        if (progress >= 1.0f) {
            progress = 1.0f;
            target->phase = blink_phase::shut;
            target->phase_start = clock;
        }
        value = 1.0f - progress;
        break;
    case blink_phase::shut:
        progress = (clock - target->phase_start) / shut_time;
        if (progress >= 1.0f) {
            target->phase = blink_phase::raising;
            target->phase_start = clock;
        }
        value = 0.0f;
        break;
    case blink_phase::raising:
        progress = (clock - target->phase_start) / raising_time;
        if (progress >= 1.0f) {
            progress = 1.0f;
            target->phase = blink_phase::waiting;
            target->next_blink = draw_next(clock);
        }
        value = progress;
        break;
    }
    return value;
}

blink* blink_create(model* target, const setting* source)
{
    if (source->eye_blink_ids.empty()) {
        return nullptr;
    }
    blink* result = new blink();
    for (const std::string& identifier : source->eye_blink_ids) {
        int32_t index = param_index(target, identifier.c_str());
        result->parameters.push_back(index);
        bool seen = false;
        for (int32_t earlier : result->distinct) {
            seen = seen || earlier == index;
        }
        if (!seen) {
            result->distinct.push_back(index);
        }
    }
    result->phase = blink_phase::start;
    result->clock = 0.0f;
    result->phase_start = 0.0f;
    result->next_blink = 0.0f;
    return result;
}

void blink_destroy(blink* target)
{
    delete target;
}

int32_t blink_param_count(const blink* target)
{
    return static_cast<int32_t>(target->parameters.size());
}

const int32_t* blink_params(const blink* target)
{
    return target->parameters.data();
}

void blink_update(blink* target, model* owner, float delta_time)
{
    float value = advance(target, delta_time);
    for (int32_t index : target->distinct) {
        param_set(owner, index, value);
    }
}

void blink_update_scaled(blink* target, model* owner, float delta_time)
{
    float value = advance(target, delta_time);
    for (int32_t index : target->distinct) {
        param_set(owner, index, param_get(owner, index) * value);
    }
}

}
