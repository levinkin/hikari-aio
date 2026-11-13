#include "internal.h"

#include <cmath>

namespace hfw {

struct breath {
    std::vector<breath_wave> waves;
    float clock;
};

breath* breath_create()
{
    breath* result = new breath();
    result->waves.reserve(8);
    result->clock = 0.0f;
    return result;
}

void breath_destroy(breath* target)
{
    delete target;
}

void breath_set(breath* target, const breath_wave* waves, int32_t count)
{
    target->waves.assign(waves, waves + (count > 0 ? count : 0));
}

void breath_update(breath* target, model* owner, float delta_time)
{
    target->clock = target->clock + delta_time;
    float phase = (target->clock * 2.0f) * pi;
    for (const breath_wave& wave : target->waves) {
        float value = std::fma(wave.peak, sinf(phase / wave.cycle), wave.offset);
        param_set(owner, wave.param, std::fma(value, wave.weight, param_get(owner, wave.param)));
    }
}

}
