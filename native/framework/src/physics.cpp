#include "internal.h"
#include "json.h"

#include <cmath>
#include <new>

namespace hfw {

static const float two_pi = 6.28318530717959f;

enum class physics_kind : int32_t { horizontal, vertical, angle };

struct physics_input {
    int32_t slot;
    float weight;
    float low;
    float high;
    physics_kind kind;
    bool reflect;
};

struct physics_output {
    int32_t parameter;
    int32_t vertex;
    float scale;
    float weight;
    float minimum;
    float maximum;
    int32_t slot;
    physics_kind kind;
    bool reflect;
    float now;
    float previous;
};

struct physics_particle {
    float mobility;
    float delay;
    float acceleration;
    float radius;
    float position_x;
    float position_y;
    float previous_x;
    float previous_y;
    float velocity_x;
    float velocity_y;
};

struct physics_setting {
    float position_minimum;
    float position_maximum;
    float position_default;
    float angle_minimum;
    float angle_maximum;
    float angle_default;
    float threshold;
    int32_t first_input;
    int32_t input_count;
    int32_t first_output;
    int32_t output_count;
    int32_t first_particle;
    int32_t particle_count;
    float gravity_previous_angle;
};

struct physics {
    std::vector<physics_setting> settings;
    std::vector<physics_input> inputs;
    std::vector<physics_output> outputs;
    std::vector<physics_particle> particles;
    std::vector<int32_t> input_parameters;
    std::vector<float> step_values;
    std::vector<float> smoothed_inputs;
    float frames_per_second;
    float carry_time;
    float gravity_x;
    float gravity_y;
    float wind_x;
    float wind_y;
    bool smoothed_ready;
    bool stepped;
};

static float wrap_angle(float result)
{
    while (result < -pi) {
        result += two_pi;
    }
    while (result > pi) {
        result -= two_pi;
    }
    return result;
}

static void normalize(float& horizontal, float& vertical)
{
    float length = sqrtf(std::fma(horizontal, horizontal, vertical * vertical));
    horizontal = horizontal / length;
    vertical = vertical / length;
}

static bool read_kind(const json_document& document, int32_t node, physics_kind& kind)
{
    std::string text;
    if (!json_text(document, json_find(document, node, "Type"), text)) {
        return false;
    }
    if (text == "X") {
        kind = physics_kind::horizontal;
    } else if (text == "Y") {
        kind = physics_kind::vertical;
    } else if (text == "Angle") {
        kind = physics_kind::angle;
    } else {
        return false;
    }
    return true;
}

static int32_t find_parameter(const model* owner, const json_document& document, int32_t node, const char* key)
{
    std::string text;
    if (!json_text(document, json_find(document, node, key), text)) {
        return -1;
    }
    for (int32_t i = 0; i < owner->parameter_count; i++) {
        if (text == owner->parameter_ids[i]) {
            return i;
        }
    }
    return -1;
}

static bool read_settings(physics& result, const model* owner, const json_document& document)
{
    int32_t root = json_root(document);
    int32_t meta = json_find(document, root, "Meta");
    int32_t list = json_find(document, root, "PhysicsSettings");
    int32_t setting_count = json_int(document, json_find(document, meta, "PhysicsSettingCount"), 0);
    if (setting_count > json_size(document, list)) {
        return false;
    }
    int64_t input_total = 0;
    int64_t output_total = 0;
    int64_t particle_total = 0;
    for (int32_t i = 0; i < setting_count; i++) {
        int32_t entry = json_at(document, list, i);
        input_total += json_size(document, json_find(document, entry, "Input"));
        output_total += json_size(document, json_find(document, entry, "Output"));
        particle_total += json_size(document, json_find(document, entry, "Vertices"));
    }
    if (input_total > json_int(document, json_find(document, meta, "TotalInputCount"), 0) || output_total > json_int(document, json_find(document, meta, "TotalOutputCount"), 0) || particle_total > json_int(document, json_find(document, meta, "VertexCount"), 0)) {
        return false;
    }
    result.frames_per_second = json_float(document, json_find(document, meta, "Fps"), 0.0f, 0.0f);
    if (result.frames_per_second > 1000.0f) {
        return false;
    }
    result.settings.reserve(setting_count > 0 ? setting_count : 0);
    result.inputs.reserve(input_total);
    result.outputs.reserve(output_total);
    result.particles.reserve(particle_total);
    std::vector<int32_t> slot_of(owner->parameter_count, -1);

    for (int32_t i = 0; i < setting_count; i++) {
        int32_t entry = json_at(document, list, i);
        int32_t normalization = json_find(document, entry, "Normalization");
        int32_t position = json_find(document, normalization, "Position");
        int32_t angle = json_find(document, normalization, "Angle");
        physics_setting setting;
        setting.position_minimum = json_float(document, json_find(document, position, "Minimum"), 0.0f, 0.0f);
        setting.position_maximum = json_float(document, json_find(document, position, "Maximum"), 0.0f, 0.0f);
        setting.position_default = json_float(document, json_find(document, position, "Default"), 0.0f, 0.0f);
        setting.angle_minimum = json_float(document, json_find(document, angle, "Minimum"), 0.0f, 0.0f);
        setting.angle_maximum = json_float(document, json_find(document, angle, "Maximum"), 0.0f, 0.0f);
        setting.angle_default = json_float(document, json_find(document, angle, "Default"), 0.0f, 0.0f);
        setting.threshold = 0.001f * setting.position_maximum;
        setting.gravity_previous_angle = atan2f(1.0f, 0.0f);

        int32_t input_list = json_find(document, entry, "Input");
        setting.first_input = static_cast<int32_t>(result.inputs.size());
        setting.input_count = json_size(document, input_list);
        for (int32_t input_index = 0; input_index < setting.input_count; input_index++) {
            int32_t node = json_at(document, input_list, input_index);
            physics_input input;
            int32_t parameter = find_parameter(owner, document, json_find(document, node, "Source"), "Id");
            if (parameter < 0 || !read_kind(document, node, input.kind)) {
                return false;
            }
            input.weight = json_float(document, json_find(document, node, "Weight"), 0.0f, 0.0f) / 100.0f;
            input.reflect = json_bool(document, json_find(document, node, "Reflect"), false);
            float minimum = owner->parameter_minimums[parameter];
            float maximum = owner->parameter_maximums[parameter];
            input.high = minimum > maximum ? minimum : maximum;
            input.low = minimum > maximum ? maximum : minimum;
            if (slot_of[parameter] < 0) {
                slot_of[parameter] = static_cast<int32_t>(result.input_parameters.size());
                result.input_parameters.push_back(parameter);
            }
            input.slot = slot_of[parameter];
            result.inputs.push_back(input);
        }

        int32_t output_list = json_find(document, entry, "Output");
        setting.first_output = static_cast<int32_t>(result.outputs.size());
        setting.output_count = json_size(document, output_list);
        for (int32_t output_index = 0; output_index < setting.output_count; output_index++) {
            int32_t node = json_at(document, output_list, output_index);
            physics_output output;
            output.parameter = find_parameter(owner, document, json_find(document, node, "Destination"), "Id");
            if (output.parameter < 0 || !read_kind(document, node, output.kind)) {
                return false;
            }
            output.vertex = json_int(document, json_find(document, node, "VertexIndex"), 0);
            output.scale = output.kind == physics_kind::angle ? json_float(document, json_find(document, node, "Scale"), 0.0f, 0.0f) : 0.0f;
            output.weight = json_float(document, json_find(document, node, "Weight"), 0.0f, 0.0f) / 100.0f;
            output.reflect = json_bool(document, json_find(document, node, "Reflect"), false);
            output.minimum = owner->parameter_minimums[output.parameter];
            output.maximum = owner->parameter_maximums[output.parameter];
            output.now = 0.0f;
            output.previous = 0.0f;
            result.outputs.push_back(output);
        }

        int32_t particle_list = json_find(document, entry, "Vertices");
        setting.first_particle = static_cast<int32_t>(result.particles.size());
        setting.particle_count = json_size(document, particle_list);
        float height = 0.0f;
        for (int32_t particle_index = 0; particle_index < setting.particle_count; particle_index++) {
            int32_t node = json_at(document, particle_list, particle_index);
            physics_particle particle;
            particle.mobility = json_float(document, json_find(document, node, "Mobility"), 0.0f, 0.0f);
            particle.delay = json_float(document, json_find(document, node, "Delay"), 0.0f, 0.0f);
            particle.acceleration = json_float(document, json_find(document, node, "Acceleration"), 0.0f, 0.0f);
            particle.radius = json_float(document, json_find(document, node, "Radius"), 0.0f, 0.0f);
            if (particle_index > 0) {
                height = height + particle.radius;
            }
            particle.position_x = 0.0f;
            particle.position_y = height;
            particle.previous_x = 0.0f;
            particle.previous_y = height;
            particle.velocity_x = 0.0f;
            particle.velocity_y = 0.0f;
            result.particles.push_back(particle);
        }
        result.settings.push_back(setting);
    }

    for (physics_output& output : result.outputs) {
        output.slot = slot_of[output.parameter];
    }
    return true;
}

physics* physics_load(model* target, const uint8_t* bytes, size_t length, status* output_status)
{
    if (!bytes || length == 0) {
        report(output_status, status::empty);
        return nullptr;
    }
    json_document document;
    if (!json_parse(document, bytes, length)) {
        report(output_status, status::json);
        return nullptr;
    }
    physics* result = new (std::nothrow) physics;
    if (!result) {
        report(output_status, status::memory);
        return nullptr;
    }
    if (!read_settings(*result, target, document)) {
        delete result;
        report(output_status, status::bad_value);
        return nullptr;
    }
    result->step_values.assign(result->input_parameters.size(), 0.0f);
    result->smoothed_inputs.assign(result->input_parameters.size(), 0.0f);
    result->carry_time = 0.0f;
    result->gravity_x = 0.0f;
    result->gravity_y = -1.0f;
    result->wind_x = 0.0f;
    result->wind_y = 0.0f;
    result->smoothed_ready = false;
    result->stepped = false;
    report(output_status, status::ok);
    return result;
}

void physics_destroy(physics* target)
{
    delete target;
}

void physics_set_wind(physics* target, float horizontal, float vertical)
{
    target->wind_x = horizontal;
    target->wind_y = vertical;
}

void physics_set_gravity(physics* target, float horizontal, float vertical)
{
    target->gravity_x = horizontal;
    target->gravity_y = vertical;
}

static void write_value(const physics_output& output, float& target, float value)
{
    float scaled = value * output.scale;
    if (scaled < output.minimum) {
        scaled = output.minimum;
    } else if (scaled > output.maximum) {
        scaled = output.maximum;
    }
    if (output.weight >= 1.0f) {
        target = scaled;
    } else {
        target = std::fma(target, 1.0f - output.weight, scaled * output.weight);
    }
}

static float normalize_input(const physics_setting& setting, const physics_input& input, float value)
{
    bool is_angle = input.kind == physics_kind::angle;
    float normal_minimum = is_angle ? setting.angle_minimum : setting.position_minimum;
    float normal_maximum = is_angle ? setting.angle_maximum : setting.position_maximum;
    float normal_default = is_angle ? setting.angle_default : setting.position_default;
    if (value > input.high) {
        value = input.high;
    }
    if (value < input.low) {
        value = input.low;
    }
    float normal_high = normal_minimum > normal_maximum ? normal_minimum : normal_maximum;
    float normal_low = normal_minimum > normal_maximum ? normal_maximum : normal_minimum;
    float middle = input.low + (std::fabs(input.high - input.low) / 2.0f);
    float distance = value - middle;
    float result = normal_default;
    if (distance > 0.0f) {
        float span = input.high - middle;
        result = span == 0.0f ? 0.0f : (distance * ((normal_high - normal_default) / span)) + normal_default;
    } else if (distance < 0.0f) {
        float span = input.low - middle;
        result = span == 0.0f ? 0.0f : (distance * ((normal_low - normal_default) / span)) + normal_default;
    }
    return input.reflect ? result : -result;
}

static void step_setting(physics& target, physics_setting& setting, float step)
{
    float translation_x = 0.0f;
    float translation_y = 0.0f;
    float angle = 0.0f;
    for (int32_t i = 0; i < setting.input_count; i++) {
        const physics_input& input = target.inputs[setting.first_input + i];
        float result = normalize_input(setting, input, target.step_values[input.slot]);
        if (input.kind == physics_kind::horizontal) {
            translation_x = std::fma(result, input.weight, translation_x);
        } else if (input.kind == physics_kind::vertical) {
            translation_y = std::fma(result, input.weight, translation_y);
        } else {
            angle = std::fma(result, input.weight, angle);
        }
    }
    float radians = (angle / 180.0f) * pi;
    float angle_sin = sinf(radians);
    float angle_cos = cosf(radians);
    float rotation_sin = -angle_sin;
    float root_x = std::fma(translation_x, angle_cos, -(translation_y * rotation_sin));
    float root_y = std::fma(root_x, rotation_sin, translation_y * angle_cos);

    physics_particle* chain = target.particles.data() + setting.first_particle;
    if (setting.particle_count > 0) {
        float gravity_x = angle_sin;
        float gravity_y = angle_cos;
        normalize(gravity_x, gravity_y);
        chain[0].position_x = root_x;
        chain[0].position_y = root_y;
        if (setting.particle_count > 1) {
            float gravity_angle = atan2f(gravity_y, gravity_x);
            float turn = wrap_angle(gravity_angle - setting.gravity_previous_angle) / 5.0f;
            float turn_cos = cosf(turn);
            float turn_sin = sinf(turn);
            for (int32_t i = 1; i < setting.particle_count; i++) {
                physics_particle& particle = chain[i];
                const physics_particle& parent = chain[i - 1];
                float force_x = (gravity_x * particle.acceleration) + target.wind_x;
                float force_y = (gravity_y * particle.acceleration) + target.wind_y;
                particle.previous_x = particle.position_x;
                particle.previous_y = particle.position_y;
                float delay = (particle.delay * step) * 30.0f;
                float offset_x = particle.position_x - parent.position_x;
                float offset_y = particle.position_y - parent.position_y;
                float turned_x = std::fma(turn_cos, offset_x, -(offset_y * turn_sin));
                float turned_y = std::fma(turn_sin, turned_x, offset_y * turn_cos);
                particle.position_x = parent.position_x + turned_x;
                particle.position_y = parent.position_y + turned_y;
                particle.position_x = (particle.position_x + (particle.velocity_x * delay)) + ((force_x * delay) * delay);
                particle.position_y = (particle.position_y + (particle.velocity_y * delay)) + ((force_y * delay) * delay);
                float direction_x = particle.position_x - parent.position_x;
                float direction_y = particle.position_y - parent.position_y;
                normalize(direction_x, direction_y);
                particle.position_x = parent.position_x + (direction_x * particle.radius);
                particle.position_y = parent.position_y + (direction_y * particle.radius);
                if (std::fabs(particle.position_x) < setting.threshold) {
                    particle.position_x = 0.0f;
                }
                if (delay != 0.0f) {
                    particle.velocity_x = ((particle.position_x - particle.previous_x) / delay) * particle.mobility;
                    particle.velocity_y = ((particle.position_y - particle.previous_y) / delay) * particle.mobility;
                }
            }
            setting.gravity_previous_angle = gravity_angle;
        }
    }

    for (int32_t i = 0; i < setting.output_count; i++) {
        physics_output& output = target.outputs[setting.first_output + i];
        int32_t vertex = output.vertex;
        if (vertex < 1 || vertex >= setting.particle_count) {
            continue;
        }
        float link_x = chain[vertex].position_x - chain[vertex - 1].position_x;
        float link_y = chain[vertex].position_y - chain[vertex - 1].position_y;
        float value = link_x;
        if (output.kind == physics_kind::vertical) {
            value = link_y;
        } else if (output.kind == physics_kind::angle) {
            float parent_x = -target.gravity_x;
            float parent_y = -target.gravity_y;
            if (vertex >= 2) {
                parent_x = chain[vertex - 1].position_x - chain[vertex - 2].position_x;
                parent_y = chain[vertex - 1].position_y - chain[vertex - 2].position_y;
            }
            value = wrap_angle(atan2f(link_y, link_x) - atan2f(parent_y, parent_x));
        }
        if (output.reflect) {
            value = -value;
        }
        output.now = value;
        if (output.slot >= 0) {
            write_value(output, target.step_values[output.slot], value);
        }
    }
}

void physics_evaluate(physics* target, model* owner, float delta_time)
{
    if (!(delta_time > 0.0f)) {
        return;
    }
    target->carry_time = target->carry_time + delta_time;
    if (target->carry_time > 5.0f) {
        target->carry_time = 0.0f;
    }
    float* values = owner->parameter_values;
    if (!target->smoothed_ready) {
        for (size_t slot = 0; slot < target->input_parameters.size(); slot++) {
            target->smoothed_inputs[slot] = values[target->input_parameters[slot]];
        }
        target->smoothed_ready = true;
    }
    float step = target->frames_per_second > 0.0f ? 1.0f / target->frames_per_second : delta_time;
    while (target->carry_time >= step) {
        for (physics_output& output : target->outputs) {
            output.previous = output.now;
        }
        float input_weight = step / target->carry_time;
        float keep_weight = 1.0f - input_weight;
        for (size_t slot = 0; slot < target->input_parameters.size(); slot++) {
            float blended = std::fma(target->smoothed_inputs[slot], keep_weight, values[target->input_parameters[slot]] * input_weight);
            target->step_values[slot] = blended;
            target->smoothed_inputs[slot] = blended;
        }
        for (physics_setting& setting : target->settings) {
            step_setting(*target, setting, step);
        }
        target->stepped = true;
        target->carry_time = target->carry_time - step;
    }
    if (!target->stepped) {
        return;
    }
    float alpha = target->carry_time / step;
    float keep_alpha = 1.0f - alpha;
    for (const physics_output& output : target->outputs) {
        write_value(output, values[output.parameter], std::fma(output.previous, keep_alpha, output.now * alpha));
    }
}

}
