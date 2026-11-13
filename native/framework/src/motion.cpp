#include "internal.h"
#include "json.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace hfw {

enum curve_target : int32_t { target_model, target_parameter, target_part };
enum model_curve : int32_t { model_other, model_eye_blink, model_lip_sync };
enum segment_kind : int32_t { kind_linear, kind_bezier, kind_stepped, kind_inverse_stepped };

constexpr int32_t list_limit = 64;
constexpr float epsilon = 0.00001f;

struct cubic {
    float cubic_coefficient, quadratic_coefficient, linear_coefficient;
    float quadratic_ratio, depressed_linear, depressed_constant_base, shift, radius, root_scale;
};

struct segment {
    float end;
    int32_t start;
    int32_t kind;
    int32_t solve;
};

struct curve {
    int32_t target;
    int32_t model_kind;
    int32_t parameter;
    int32_t part;
    float fade_in;
    float fade_out;
    bool own_fade;
    bool eye_blink;
    bool lip_sync;
    bool sorted;
    int32_t number_first;
    int32_t number_count;
    int32_t segment_first;
    int32_t segment_count;
};

struct motion {
    float duration;
    float loop_length;
    float fade_in;
    float fade_out;
    bool restricted;
    bool loop;
    bool loop_fade_in;
    bool evaluated_loop;
    bool has_eye_blink;
    bool has_lip_sync;
    int32_t model_end;
    int32_t parameter_end;
    int32_t part_end;
    std::vector<curve> curves;
    std::vector<segment> segments;
    std::vector<cubic> cubics;
    std::vector<float> numbers;
    std::vector<int32_t> eye_blink_missing;
    std::vector<int32_t> lip_sync_missing;
};

float ease(float progress)
{
    if (progress < 0.0f) {
        return 0.0f;
    }
    if (progress > 1.0f) {
        return 1.0f;
    }
    return 0.5f - 0.5f * cosf(progress * pi);
}

static cubic prepare_cubic(float start_time, float first_handle_time, float second_handle_time, float end_time)
{
    cubic solve = {};
    solve.cubic_coefficient = std::fma(first_handle_time, 3.0f, std::fma(second_handle_time, -3.0f, end_time)) - start_time;
    solve.quadratic_coefficient = std::fma(start_time, 3.0f, std::fma(second_handle_time, 3.0f, first_handle_time * -6.0f));
    solve.linear_coefficient = std::fma(first_handle_time, 3.0f, start_time * -3.0f);
    if (std::fabs(solve.cubic_coefficient) < epsilon) {
        return solve;
    }
    solve.quadratic_ratio = solve.quadratic_coefficient / solve.cubic_coefficient;
    float linear_ratio = solve.linear_coefficient / solve.cubic_coefficient;
    solve.depressed_linear = std::fma(linear_ratio, 3.0f, -(solve.quadratic_ratio * solve.quadratic_ratio)) / 3.0f;
    solve.depressed_constant_base = std::fma((solve.quadratic_ratio + solve.quadratic_ratio) * solve.quadratic_ratio, solve.quadratic_ratio, (solve.quadratic_ratio * -9.0f) * linear_ratio);
    solve.shift = solve.quadratic_ratio / -3.0f;
    float negative_third_linear = solve.depressed_linear / -3.0f;
    solve.radius = sqrtf(negative_third_linear * (negative_third_linear * negative_third_linear));
    solve.root_scale = cbrtf(solve.radius) * 2.0f;
    return solve;
}

static float cubic_root(const cubic& solve, float constant_term)
{
    if (std::fabs(solve.cubic_coefficient) < epsilon) {
        if (std::fabs(solve.quadratic_coefficient) < epsilon) {
            return std::clamp(std::fabs(solve.linear_coefficient) < epsilon ? -constant_term : -constant_term / solve.linear_coefficient, 0.0f, 1.0f);
        }
        return std::clamp(-(solve.linear_coefficient + sqrtf(std::fma(solve.linear_coefficient, solve.linear_coefficient, (solve.quadratic_coefficient * -4.0f) * constant_term))) / (solve.quadratic_coefficient + solve.quadratic_coefficient), 0.0f, 1.0f);
    }
    float normalized_constant = constant_term / solve.cubic_coefficient;
    float third_depressed_linear = solve.depressed_linear / 3.0f;
    float depressed_constant = std::fma(normalized_constant, 27.0f, solve.depressed_constant_base) / 27.0f;
    float half_depressed_constant = depressed_constant * 0.5f;
    float discriminant = std::fma(half_depressed_constant, half_depressed_constant, third_depressed_linear * (third_depressed_linear * third_depressed_linear));
    if (discriminant < 0.0f) {
        float cosine_angle = -depressed_constant / (solve.radius + solve.radius);
        cosine_angle = cosine_angle < -1.0f ? -1.0f : (cosine_angle > 1.0f ? 1.0f : cosine_angle);
        float angle = acosf(cosine_angle);
        float candidate = std::fma(solve.root_scale, cosf(angle / 3.0f), solve.shift);
        if (std::fabs(candidate - 0.5f) < 0.51f) {
            return std::clamp(candidate, 0.0f, 1.0f);
        }
        candidate = std::fma(solve.root_scale, cosf((angle + 6.28318548f) / 3.0f), solve.shift);
        if (std::fabs(candidate - 0.5f) < 0.51f) {
            return std::clamp(candidate, 0.0f, 1.0f);
        }
        return std::clamp(std::fma(solve.root_scale, cosf((angle + 12.566371f) / 3.0f), solve.shift), 0.0f, 1.0f);
    }
    if (discriminant == 0.0f) {
        float root = half_depressed_constant < 0.0f ? cbrtf(-half_depressed_constant) : -cbrtf(half_depressed_constant);
        float candidate = std::fma(2.0f, root, -(solve.quadratic_ratio / 3.0f));
        if (std::fabs(candidate - 0.5f) < 0.51f) {
            return std::clamp(candidate, 0.0f, 1.0f);
        }
        return std::clamp(-root - solve.quadratic_ratio / 3.0f, 0.0f, 1.0f);
    }
    float discriminant_root = sqrtf(discriminant);
    return std::clamp((cbrtf(discriminant_root - half_depressed_constant) - cbrtf(half_depressed_constant + discriminant_root)) + solve.shift, 0.0f, 1.0f);
}

static float segment_value(const motion& source, const segment& piece, float sample_time)
{
    const float* point = source.numbers.data() + piece.start;
    if (piece.kind == kind_stepped) {
        return point[1];
    }
    if (piece.kind == kind_inverse_stepped) {
        return point[4];
    }
    if (piece.kind == kind_linear) {
        float ratio = (sample_time - point[0]) / (point[3] - point[0]);
        return std::fma(point[4] - point[1], ratio < 0.0f ? 0.0f : ratio, point[1]);
    }
    float ratio;
    if (source.restricted) {
        ratio = (sample_time - point[0]) / (point[7] - point[0]);
        ratio = ratio < 0.0f ? 0.0f : ratio;
    } else {
        ratio = cubic_root(source.cubics[piece.solve], point[0] - sample_time);
    }
    float first_blend = std::fma(point[4] - point[1], ratio, point[1]);
    float second_blend = std::fma(point[6] - point[4], ratio, point[4]);
    float third_blend = std::fma(point[8] - point[6], ratio, point[6]);
    float left = std::fma(second_blend - first_blend, ratio, first_blend);
    float right = std::fma(third_blend - second_blend, ratio, second_blend);
    return std::fma(right - left, ratio, left);
}

static float curve_value(const motion& source, const curve& line, float sample_time)
{
    const segment* first = source.segments.data() + line.segment_first;
    const segment* last = first + line.segment_count;
    const segment* found = first;
    if (line.sorted) {
        found = std::upper_bound(first, last, sample_time, [](float time, const segment& piece) { return time < piece.end; });
    } else {
        while (found != last && !(found->end > sample_time)) {
            ++found;
        }
    }
    if (found != last) {
        return segment_value(source, *found, sample_time);
    }
    const segment& tail = last[-1];
    int32_t end = tail.start + (tail.kind == kind_bezier ? 7 : 3);
    float tail_time = source.numbers[end];
    float tail_value = source.numbers[end + 1];
    if (!source.loop || !(sample_time < source.loop_length)) {
        return tail_value;
    }
    float first_value = source.numbers[line.number_first + 1];
    if (tail.kind == kind_stepped) {
        return tail_value;
    }
    if (tail.kind == kind_inverse_stepped) {
        return first_value;
    }
    float ratio = (sample_time - tail_time) / (source.loop_length - tail_time);
    return std::fma(first_value - tail_value, ratio < 0.0f ? 0.0f : ratio, tail_value);
}

bool motion_evaluate(motion* source, model* owner, float time, bool* started, float* start, float* fade_start, float* end)
{
    if (!*started) {
        *started = true;
        *start = time;
        *fade_start = time;
        *end = !source->loop && source->duration > 0.0f ? time + source->duration : -1.0f;
    }
    float fade_in = source->fade_in == 0.0f ? 1.0f : ease((time - *fade_start) / source->fade_in);
    float fade_out = source->fade_out == 0.0f || *end < 0.0f ? 1.0f : ease((*end - time) / source->fade_out);
    float weight = fade_in * fade_out;
    if (source->evaluated_loop != source->loop) {
        *end = !source->loop && source->duration > 0.0f ? *start + source->duration : -1.0f;
        source->evaluated_loop = source->loop;
    }
    float offset = time - *start;
    offset = offset < 0.0f ? 0.0f : offset;
    float sample_time = offset;
    if (source->loop) {
        while (sample_time > source->loop_length) {
            sample_time -= source->loop_length;
        }
    }
    float eye_blink = 0.0f;
    float lip_sync = 0.0f;
    for (int32_t curve_index = 0; curve_index < source->model_end; curve_index++) {
        const curve& line = source->curves[curve_index];
        if (line.model_kind == model_eye_blink) {
            eye_blink = curve_value(*source, line, sample_time);
        } else if (line.model_kind == model_lip_sync) {
            lip_sync = curve_value(*source, line, sample_time);
        }
    }
    for (int32_t curve_index = source->model_end; curve_index < source->parameter_end; curve_index++) {
        const curve& line = source->curves[curve_index];
        float value = curve_value(*source, line, sample_time);
        if (line.eye_blink) {
            value *= eye_blink;
        }
        if (line.lip_sync) {
            value += lip_sync;
        }
        float curve_weight = weight;
        if (line.own_fade) {
            float curve_in = line.fade_in < 0.0f ? fade_in : (line.fade_in == 0.0f ? 1.0f : ease((time - *fade_start) / line.fade_in));
            float curve_out = line.fade_out < 0.0f ? fade_out : (line.fade_out == 0.0f || *end < 0.0f ? 1.0f : ease((*end - time) / line.fade_out));
            curve_weight = curve_in * curve_out;
        }
        float current = param_get(owner, line.parameter);
        param_set(owner, line.parameter, std::fma(value - current, curve_weight, current));
    }
    if (source->has_eye_blink) {
        for (int32_t parameter : source->eye_blink_missing) {
            float current = param_get(owner, parameter);
            param_set(owner, parameter, std::fma(eye_blink - current, weight, current));
        }
    }
    if (source->has_lip_sync) {
        for (int32_t parameter : source->lip_sync_missing) {
            float current = param_get(owner, parameter);
            param_set(owner, parameter, std::fma(lip_sync - current, weight, current));
        }
    }
    for (int32_t curve_index = source->parameter_end; curve_index < source->part_end; curve_index++) {
        const curve& line = source->curves[curve_index];
        param_set(owner, line.parameter, curve_value(*source, line, sample_time));
    }
    if (source->loop && offset >= source->loop_length) {
        *start = time - sample_time;
        if (source->loop_fade_in) {
            *fade_start = time - sample_time;
        }
    }
    return (!source->loop && offset >= source->duration) || (*end > 0.0f && *end < time);
}

static bool tag_kind(float tag, int32_t* kind)
{
    if (!(tag > -1.0f && tag < 4.0f)) {
        return false;
    }
    *kind = static_cast<int32_t>(tag);
    return true;
}

static float meta_fade(const json_document& document, int32_t meta, const char* key)
{
    float fade = json_float(document, json_find(document, meta, key), 1.0f, 0.0f);
    return fade < 0.0f ? 1.0f : fade;
}

static void bind_list(model* owner, const std::vector<std::string>& identifiers, std::vector<int32_t>& parameters)
{
    int32_t count = std::min(static_cast<int32_t>(identifiers.size()), list_limit);
    for (int32_t i = 0; i < count; i++) {
        parameters.push_back(param_index(owner, identifiers[i].c_str()));
    }
}

static void missing_identifiers(const motion& source, const std::vector<int32_t>& parameters, std::vector<int32_t>& missing)
{
    for (int32_t parameter : parameters) {
        bool owned = false;
        for (int32_t curve_index = source.model_end; curve_index < source.parameter_end && !owned; curve_index++) {
            owned = source.curves[curve_index].parameter == parameter;
        }
        if (!owned) {
            missing.push_back(parameter);
        }
    }
}

motion* motion_load(model* target, const setting* source, const uint8_t* bytes, size_t length, int32_t group, int32_t entry, status* output_status)
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
    int32_t root = json_root(document);
    int32_t meta = json_find(document, root, "Meta");
    int32_t curves = json_find(document, root, "Curves");
    if (curves < 0 || document.nodes[curves].kind != json_kind::array) {
        report(output_status, status::json);
        return nullptr;
    }
    int32_t curve_count = json_size(document, curves);
    int64_t segment_total = 0;
    int64_t point_total = 0;
    int64_t number_total = 0;
    for (int32_t curve_index = 0; curve_index < curve_count; curve_index++) {
        int32_t numbers = json_find(document, json_at(document, curves, curve_index), "Segments");
        if (numbers < 0 || document.nodes[numbers].kind != json_kind::array || json_size(document, numbers) < 2) {
            report(output_status, status::json);
            return nullptr;
        }
        int32_t size = json_size(document, numbers);
        number_total += size + 3;
        point_total += 1;
        if (size == 2) {
            segment_total += 1;
            point_total += 1;
        }
        for (int32_t position = 2; position < size;) {
            int32_t kind;
            if (!tag_kind(json_float(document, json_at(document, numbers, position), 0.0f, 0.0f), &kind)) {
                report(output_status, status::json);
                return nullptr;
            }
            int32_t step = kind == kind_bezier ? 7 : 3;
            if (position + step > size) {
                report(output_status, status::json);
                return nullptr;
            }
            segment_total += 1;
            point_total += kind == kind_bezier ? 3 : 1;
            position += step;
        }
    }
    if (curve_count != json_int(document, json_find(document, meta, "CurveCount"), 0)
        || segment_total != json_int(document, json_find(document, meta, "TotalSegmentCount"), 0)
        || point_total != json_int(document, json_find(document, meta, "TotalPointCount"), 0)) {
        report(output_status, status::inconsistent);
        return nullptr;
    }

    motion* result = new (std::nothrow) motion();
    if (!result) {
        report(output_status, status::memory);
        return nullptr;
    }
    motion& loaded = *result;
    loaded.duration = json_float(document, json_find(document, meta, "Duration"), 0.0f, 0.0f);
    float frames_per_second = json_float(document, json_find(document, meta, "Fps"), 0.0f, 0.0f);
    loaded.loop_length = loaded.duration + 1.0f / frames_per_second;
    if (!(loaded.loop_length > 0.0f)) {
        loaded.loop_length = INFINITY;
    }
    if (loaded.loop_length < 0.001f) {
        delete result;
        report(output_status, status::bad_value);
        return nullptr;
    }
    loaded.restricted = json_bool(document, json_find(document, meta, "AreBeziersRestricted"), false);
    loaded.fade_in = meta_fade(document, meta, "FadeInTime");
    loaded.fade_out = meta_fade(document, meta, "FadeOutTime");
    loaded.loop_fade_in = true;
    if (source && group >= 0 && group < static_cast<int32_t>(source->motion_groups.size())) {
        const std::vector<motion_entry>& entries = source->motion_groups[group].entries;
        if (entry >= 0 && entry < static_cast<int32_t>(entries.size())) {
            if (entries[entry].fade_in >= 0.0f) {
                loaded.fade_in = entries[entry].fade_in;
            }
            if (entries[entry].fade_out >= 0.0f) {
                loaded.fade_out = entries[entry].fade_out;
            }
        }
    }

    loaded.curves.resize(curve_count);
    loaded.segments.reserve(segment_total);
    loaded.numbers.reserve(number_total);
    for (int32_t curve_index = 0; curve_index < curve_count; curve_index++) {
        int32_t node = json_at(document, curves, curve_index);
        int32_t numbers = json_find(document, node, "Segments");
        curve& line = loaded.curves[curve_index];
        std::string text;
        line.target = target_model;
        if (json_text(document, json_find(document, node, "Target"), text)) {
            line.target = text == "Parameter" ? target_parameter : (text == "PartOpacity" ? target_part : target_model);
        }
        std::string identifier;
        json_text(document, json_find(document, node, "Id"), identifier);
        line.model_kind = identifier == "EyeBlink" ? model_eye_blink : (identifier == "LipSync" ? model_lip_sync : model_other);
        line.parameter = line.target == target_model ? -1 : param_index(target, identifier.c_str());
        line.part = line.target == target_part ? part_index(target, identifier.c_str()) : -1;
        line.fade_in = json_float(document, json_find(document, node, "FadeInTime"), -1.0f, -1.0f);
        line.fade_out = json_float(document, json_find(document, node, "FadeOutTime"), -1.0f, -1.0f);
        line.own_fade = line.fade_in >= 0.0f || line.fade_out >= 0.0f;
        line.number_first = static_cast<int32_t>(loaded.numbers.size());
        line.number_count = json_size(document, numbers);
        line.segment_first = static_cast<int32_t>(loaded.segments.size());
        for (int32_t i = 0; i < line.number_count; i++) {
            loaded.numbers.push_back(json_float(document, json_at(document, numbers, i), 0.0f, 0.0f));
        }
        if (line.number_count == 2) {
            loaded.numbers.insert(loaded.numbers.end(), 3, 0.0f);
        }
        int32_t size = line.number_count == 2 ? 5 : line.number_count;
        line.sorted = true;
        for (int32_t position = 2; position < size;) {
            int32_t kind = 0;
            tag_kind(loaded.numbers[line.number_first + position], &kind);
            int32_t step = kind == kind_bezier ? 7 : 3;
            segment piece;
            piece.start = line.number_first + position - 2;
            piece.kind = kind;
            piece.end = loaded.numbers[piece.start + step];
            piece.solve = -1;
            if (kind == kind_bezier) {
                const float* point = loaded.numbers.data() + piece.start;
                piece.solve = static_cast<int32_t>(loaded.cubics.size());
                loaded.cubics.push_back(prepare_cubic(point[0], point[3], point[5], point[7]));
            }
            if (loaded.segments.size() > static_cast<size_t>(line.segment_first) && !(loaded.segments.back().end <= piece.end)) {
                line.sorted = false;
            }
            loaded.segments.push_back(piece);
            position += step;
        }
        line.segment_count = static_cast<int32_t>(loaded.segments.size()) - line.segment_first;
    }

    int32_t curve_index = 0;
    while (curve_index < curve_count && loaded.curves[curve_index].target == target_model) {
        loaded.has_eye_blink = loaded.has_eye_blink || loaded.curves[curve_index].model_kind == model_eye_blink;
        loaded.has_lip_sync = loaded.has_lip_sync || loaded.curves[curve_index].model_kind == model_lip_sync;
        curve_index++;
    }
    loaded.model_end = curve_index;
    while (curve_index < curve_count && loaded.curves[curve_index].target == target_parameter) {
        curve_index++;
    }
    loaded.parameter_end = curve_index;
    while (curve_index < curve_count && loaded.curves[curve_index].target == target_part) {
        curve_index++;
    }
    loaded.part_end = curve_index;

    std::vector<int32_t> eye_blink_parameters;
    std::vector<int32_t> lip_sync_parameters;
    if (source) {
        bind_list(target, source->eye_blink_ids, eye_blink_parameters);
        bind_list(target, source->lip_sync_ids, lip_sync_parameters);
    }
    for (int32_t i = loaded.model_end; i < loaded.parameter_end; i++) {
        curve& line = loaded.curves[i];
        line.eye_blink = loaded.has_eye_blink && std::find(eye_blink_parameters.begin(), eye_blink_parameters.end(), line.parameter) != eye_blink_parameters.end();
        line.lip_sync = loaded.has_lip_sync && std::find(lip_sync_parameters.begin(), lip_sync_parameters.end(), line.parameter) != lip_sync_parameters.end();
    }
    missing_identifiers(loaded, eye_blink_parameters, loaded.eye_blink_missing);
    missing_identifiers(loaded, lip_sync_parameters, loaded.lip_sync_missing);
    report(output_status, status::ok);
    return result;
}

void motion_destroy(motion* target)
{
    delete target;
}

float motion_duration(const motion* target)
{
    return target->duration;
}

float motion_fade_out(const motion* target)
{
    return target->fade_out;
}

void motion_set_loop(motion* target, bool loop, bool loop_fade_in)
{
    target->loop = loop;
    target->loop_fade_in = loop_fade_in;
}

int32_t motion_curve_count(const motion* target)
{
    return static_cast<int32_t>(target->curves.size());
}

int32_t motion_curve_target(const motion* target, int32_t curve_index)
{
    return target->curves[curve_index].target;
}

int32_t motion_curve_part(const motion* target, model*, int32_t curve_index)
{
    return target->curves[curve_index].part;
}

int32_t motion_curve_param(const motion* target, model*, int32_t curve_index)
{
    return target->curves[curve_index].parameter;
}

int32_t motion_curve_number_count(const motion* target, int32_t curve_index)
{
    return target->curves[curve_index].number_count;
}

float motion_curve_number(const motion* target, int32_t curve_index, int32_t index)
{
    return target->numbers[target->curves[curve_index].number_first + index];
}

}
