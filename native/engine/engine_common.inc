#include <filesystem>

static const float kBackgroundStepMs = 2000.0f / 24.0f;

struct physics_scale
{
    int32_t index;
    float strength;
    float wind_multiplier;
};

struct shared_moc
{
    hfw::moc* moc = NULL;
    int users = 0;
};

static std::map<std::string, shared_moc> moc_cache;
static std::mutex moc_lock;

struct engine : public engine_gpu
{
    hfw::setting* setting = NULL;
    hfw::model* model = NULL;
    hfw::queue* motion_queue = NULL;
    hfw::layer* main_layer = NULL;
    hfw::layer* pose_layer = NULL;
    std::vector<hfw::layer*> layers;
    hfw::blink* eye_blink = NULL;
    hfw::breath* breathing = NULL;
    hfw::physics* simulation = NULL;
    hfw::pose* pose_state = NULL;
    hfw::rig rig = {};
    std::map<std::string, hfw::expression*> expressions;
    std::vector<hfw::expression*> retired_expressions;
    hfw::breath_wave breath_waves[5] = {};
    hfw::mat4 mvp = {};
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    std::vector<float> shown_params;
    std::vector<float> shown_parts;
    bool update_pending = false;
    std::vector<float> carry_params;
    std::vector<float> carry_parts;
    float carry_left_ms = 0.0f;
    float carry_total_ms = 0.0f;
    std::vector<std::vector<uint8_t>> background_frames;
    std::vector<float> background_ms;
    float background_total_ms = 0;
    float view_zoom = 1;
    float view_x = 0;
    float view_y = 0;
    float view_rot = 0;
    float physics_weight = 1;
    float wind = 0;
    double wind_clock = 0;
    float wind_phase[4] = {0, 0, 0, 0};
    float gust = 0;
    std::vector<physics_scale> physics_scales;
    std::vector<float> pre_physics;
    std::vector<float> ahead_parameters;
    std::vector<float> ahead_parts;
    std::string moc_path;
    std::map<std::string, std::vector<hfw::motion*>> motions;
    std::vector<hfw::motion*>* idle = NULL;
    bool idle_paused = false;
    uint32_t motion_seed = 0;
    unsigned int blink_seed = 1;
    std::map<const hfw::motion*, std::vector<int32_t>> motion_params;
    std::vector<char> rest_marks;
    float frame_ms = 0;
    engine_hook after_motion_update = NULL;
    engine_hook before_model_update = NULL;
    void* hook_context = NULL;
};

const hfw::model* engine_model(engine* target)
{
    return target->model;
}

static std::string gpu_name;

static void engine_release(engine* e)
{
    hfw::queue_destroy(e->motion_queue);
    for (auto& group : e->motions)
    {
        for (hfw::motion* motion : group.second)
        {
            hfw::motion_destroy(motion);
        }
    }
    for (hfw::layer* layer : e->layers)
    {
        hfw::layer_destroy(layer);
    }
    for (auto& item : e->expressions)
    {
        hfw::expression_destroy(item.second);
    }
    for (hfw::expression* expression : e->retired_expressions)
    {
        hfw::expression_destroy(expression);
    }
    if (e->eye_blink != NULL)
    {
        hfw::blink_destroy(e->eye_blink);
    }
    hfw::breath_destroy(e->breathing);
    if (e->pose_state != NULL)
    {
        hfw::pose_destroy(e->pose_state);
    }
    if (e->simulation != NULL)
    {
        hfw::physics_destroy(e->simulation);
    }
    {
        std::lock_guard<std::mutex> guard(moc_lock);
        hfw::model_destroy(e->model);
        shared_moc& shared = moc_cache[e->moc_path];
        if (--shared.users == 0)
        {
            hfw::moc_destroy(shared.moc);
            moc_cache.erase(e->moc_path);
        }
    }
    hfw::setting_destroy(e->setting);
    delete e;
}

static std::vector<uint8_t> read_file(const std::string& path)
{
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

static void motion_part_opacities(engine* e, const std::string& dir)
{
    std::vector<std::string> files;
    for (int32_t group_index = 0; group_index < hfw::setting_motion_group_count(e->setting); group_index++)
    {
        for (int32_t i = 0; i < hfw::setting_motion_count(e->setting, group_index); i++)
        {
            files.push_back(dir + hfw::setting_motion_file(e->setting, group_index, i));
        }
    }
    if (files.empty())
    {
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(std::filesystem::u8path(dir), error), end; !error && it != end; it.increment(error))
        {
            std::string file = it->path().u8string();
            if (file.size() > 13 && file.compare(file.size() - 13, 13, ".motion3.json") == 0)
            {
                files.push_back(file);
            }
        }
        std::sort(files.begin(), files.end());
    }
    std::map<int32_t, std::pair<float, bool>> parts;
    int32_t part_count = hfw::part_count(e->model);
    for (const std::string& file : files)
    {
        std::vector<uint8_t> bytes = read_file(file);
        if (bytes.empty())
        {
            continue;
        }
        hfw::motion* motion = hfw::motion_load(e->model, e->setting, bytes.data(), bytes.size(), -1, -1, NULL);
        if (motion == NULL)
        {
            continue;
        }
        for (int32_t curve_index = 0; curve_index < hfw::motion_curve_count(motion); curve_index++)
        {
            if (hfw::motion_curve_target(motion, curve_index) != 2 || hfw::motion_curve_number_count(motion, curve_index) < 2)
            {
                continue;
            }
            int32_t index = hfw::motion_curve_part(motion, e->model, curve_index);
            if (index >= part_count)
            {
                continue;
            }
            float value = hfw::motion_curve_number(motion, curve_index, 1);
            auto found = parts.emplace(index, std::make_pair(value, false));
            found.first->second.second |= found.first->second.first != value;
        }
        hfw::motion_destroy(motion);
    }
    for (const auto& part : parts)
    {
        if (part.second.second)
        {
            hfw::part_set_opacity(e->model, part.first, part.second.first);
        }
    }
}

static hfw::motion* motion_load(engine* e, const std::string& path, int32_t group, int32_t entry)
{
    std::vector<uint8_t> bytes = read_file(path);
    if (bytes.empty())
    {
        return NULL;
    }
    hfw::motion* motion = hfw::motion_load(e->model, e->setting, bytes.data(), bytes.size(), group, entry, NULL);
    if (motion == NULL)
    {
        fprintf(stderr, "hikari: skipping inconsistent motion %s\n", path.c_str());
        return NULL;
    }
    std::vector<int32_t>& parameter_indices = e->motion_params[motion];
    int32_t parameter_count = hfw::param_count(e->model);
    for (int32_t curve = 0; curve < hfw::motion_curve_count(motion); curve++)
    {
        if (hfw::motion_curve_target(motion, curve) != 1)
        {
            continue;
        }
        int32_t index = hfw::motion_curve_param(motion, e->model, curve);
        if (index < parameter_count)
        {
            parameter_indices.push_back(index);
        }
    }
    return motion;
}

static hfw::motion* motion_pick(engine* e, const std::vector<hfw::motion*>& group)
{
    e->motion_seed = e->motion_seed * 1664525u + 1013904223u;
    return group[(e->motion_seed >> 8) % group.size()];
}

static void view_matrix(engine* e)
{
    float canvas_h = hfw::canvas_height_pixels(e->model);
    float ppu = hfw::pixels_per_unit(e->model);
    float fit = (float)std::min(e->width, e->height);
    float base = fit / (0.315f * canvas_h);
    float scale = ppu * base * e->view_zoom;
    float lift = e->view_zoom * (1.0f - canvas_h * base / fit) * fit * 0.5f + (e->height - fit) * 0.5f;
    float c = cosf(e->view_rot * (float)M_PI / 180.0f);
    float s = sinf(e->view_rot * (float)M_PI / 180.0f);
    e->mvp = {{
        2.0f * c * scale / e->width, 2.0f * s * scale / e->height, 0, 0,
        -2.0f * s * scale / e->width, 2.0f * c * scale / e->height, 0, 0,
        0, 0, 1, 0,
        -2.0f * s * lift / e->width + 2.0f * e->view_x, 2.0f * c * lift / e->height + 2.0f * e->view_y, 0, 1}};
}

static engine* load_model(const std::string& path, const std::string& dir, int width, int height)
{
    std::vector<uint8_t> json = read_file(path);
    hfw::setting* setting = hfw::setting_parse(json.data(), json.size(), NULL);
    if (setting == NULL)
    {
        return NULL;
    }
    engine* e = new engine();
    e->setting = setting;
    std::string moc_path = dir + hfw::setting_moc(setting);
    {
        std::lock_guard<std::mutex> guard(moc_lock);
        shared_moc& shared = moc_cache[moc_path];
        if (shared.moc == NULL)
        {
            std::vector<uint8_t> moc = read_file(moc_path);
            shared.moc = hfw::moc_load(moc.data(), moc.size(), NULL);
        }
        e->model = shared.moc == NULL ? NULL : hfw::model_create(shared.moc, NULL);
        if (e->model == NULL)
        {
            if (shared.users == 0)
            {
                hfw::moc_destroy(shared.moc);
                moc_cache.erase(moc_path);
            }
        }
        else
        {
            shared.users++;
            e->moc_path = moc_path;
        }
    }
    if (e->model == NULL)
    {
        hfw::setting_destroy(setting);
        delete e;
        return NULL;
    }
    hfw::params_save(e->model);
    for (int32_t i = 0; i < hfw::setting_expression_count(setting); i++)
    {
        std::string file = hfw::setting_expression_file(setting, i);
        std::string name = file.substr(file.find_last_of("/\\") + 1);
        name = name.substr(0, name.find(".exp3.json"));
        std::vector<uint8_t> bytes = read_file(dir + file);
        hfw::expression* expression = hfw::expression_load(e->model, bytes.data(), bytes.size(), NULL);
        if (expression != NULL)
        {
            hfw::expression*& slot = e->expressions[name];
            if (slot != NULL)
            {
                hfw::expression_destroy(slot);
            }
            slot = expression;
        }
    }
    static const char empty_expression[] = "{\"FadeInTime\":0.3,\"FadeOutTime\":0.3,\"Parameters\":[]}";
    e->expressions[""] = hfw::expression_load(e->model, (const uint8_t*)empty_expression, strlen(empty_expression), NULL);
    e->pose_layer = hfw::layer_create();
    e->main_layer = hfw::layer_create();
    e->layers = {e->pose_layer, e->main_layer};
    e->motion_queue = hfw::queue_create();
    if (strcmp(hfw::setting_physics(setting), "") != 0)
    {
        std::vector<uint8_t> bytes = read_file(dir + hfw::setting_physics(setting));
        e->simulation = hfw::physics_load(e->model, bytes.data(), bytes.size(), NULL);
    }
    if (strcmp(hfw::setting_pose(setting), "") != 0)
    {
        std::vector<uint8_t> bytes = read_file(dir + hfw::setting_pose(setting));
        e->pose_state = hfw::pose_load(e->model, bytes.data(), bytes.size(), NULL);
    }
    else
    {
        motion_part_opacities(e, dir);
    }
    e->eye_blink = hfw::blink_create(e->model, setting);
    for (int32_t group_index = 0; group_index < hfw::setting_motion_group_count(setting); group_index++)
    {
        const char* group = hfw::setting_motion_group(setting, group_index);
        for (int32_t i = 0; i < hfw::setting_motion_count(setting, group_index); i++)
        {
            hfw::motion* motion = motion_load(e, dir + hfw::setting_motion_file(setting, group_index, i), group_index, i);
            if (motion != NULL)
            {
                e->motions[group].push_back(motion);
            }
        }
    }
    e->breathing = hfw::breath_create();
    e->breath_waves[0] = {hfw::param_index(e->model, "ParamAngleX"), 0.0f, 15.0f, 6.5345f, 0.5f};
    e->breath_waves[1] = {hfw::param_index(e->model, "ParamAngleY"), 0.0f, 8.0f, 3.5345f, 0.5f};
    e->breath_waves[2] = {hfw::param_index(e->model, "ParamAngleZ"), 0.0f, 10.0f, 5.5345f, 0.5f};
    e->breath_waves[3] = {hfw::param_index(e->model, "ParamBodyAngleX"), 0.0f, 4.0f, 15.5345f, 0.5f};
    e->breath_waves[4] = {hfw::param_index(e->model, "ParamBreath"), 0.0f, 0.5f, 3.2345f, 0.5f};
    hfw::breath_set(e->breathing, e->breath_waves, 5);
    e->rig = {e->model, e->motion_queue, e->layers.data(), (int32_t)e->layers.size(), e->eye_blink, e->breathing, e->simulation, e->pose_state, !e->motions.empty()};
    e->width = width;
    e->height = height;
    uint32_t seed = 2166136261u;
    for (char c : path)
    {
        seed = (seed ^ (uint8_t)c) * 16777619u;
    }
    for (float& phase : e->wind_phase)
    {
        seed = seed * 1664525u + 1013904223u;
        phase = (seed >> 8) * (6.2831853f / 16777216.0f);
    }
    e->motion_seed = seed;
    e->blink_seed = seed * 1664525u + 1013904223u;
    int32_t parameter_count = hfw::param_count(e->model);
    int32_t part_count = hfw::part_count(e->model);
    e->shown_params.reserve(parameter_count);
    e->pre_physics.reserve(parameter_count);
    e->ahead_parameters.reserve(parameter_count);
    e->rest_marks.reserve(parameter_count);
    e->shown_parts.reserve(part_count);
    e->ahead_parts.reserve(part_count);
    return e;
}

bool engine_view_set(engine* e, float zoom, float x, float y, float rot)
{
    float before[4] = {e->view_zoom, e->view_x, e->view_y, e->view_rot};
    if (std::isfinite(zoom) && zoom > 0)
    {
        e->view_zoom = zoom;
    }
    if (std::isfinite(x))
    {
        e->view_x = x;
    }
    if (std::isfinite(y))
    {
        e->view_y = y;
    }
    if (std::isfinite(rot))
    {
        e->view_rot = rot;
    }
    if (before[0] == e->view_zoom && before[1] == e->view_x && before[2] == e->view_y && before[3] == e->view_rot)
    {
        return false;
    }
    view_matrix(e);
    return true;
}

const char* engine_gpu_name(engine* e)
{
    return gpu_name.c_str();
}

bool engine_has_param(engine* e, const char* id)
{
    return hfw::param_index(e->model, id) < hfw::param_count(e->model);
}

int32_t engine_param_index(engine* target, const char* identifier)
{
    return hfw::param_index(target->model, identifier);
}

const char* engine_param_at(engine* e, int index, float* minimum, float* maximum)
{
    if (index < 0 || index >= hfw::param_count(e->model))
    {
        return NULL;
    }
    return hfw::param_at(e->model, index, minimum, maximum);
}

float engine_param_get(engine* e, int32_t index)
{
    return hfw::param_get(e->model, index);
}

void engine_param_set(engine* e, int32_t index, float value)
{
    hfw::param_set(e->model, index, value);
}

bool engine_expression(engine* e, int layer, const char* name)
{
    auto found = e->expressions.find(name);
    if (found == e->expressions.end())
    {
        return false;
    }
    hfw::layer_start(layer == 0 ? e->main_layer : e->pose_layer, found->second);
    return true;
}

bool engine_expression_add(engine* e, const char* name, const char* path)
{
    std::vector<uint8_t> bytes = read_file(path);
    hfw::expression* expression = bytes.empty() ? NULL : hfw::expression_load(e->model, bytes.data(), bytes.size(), NULL);
    if (expression == NULL)
    {
        return false;
    }
    auto found = e->expressions.find(name);
    if (found != e->expressions.end())
    {
        e->retired_expressions.push_back(found->second);
    }
    e->expressions[name] = expression;
    return true;
}

bool engine_expression_keep(engine* e, const char* name)
{
    auto found = e->expressions.find(name);
    if (found == e->expressions.end())
    {
        return false;
    }
    e->layers.push_back(hfw::layer_create());
    e->rig.layers = e->layers.data();
    e->rig.layer_count = (int32_t)e->layers.size();
    hfw::layer_start(e->layers.back(), found->second);
    return true;
}

void engine_expression_fade(engine* e, const char* name, float seconds)
{
    auto found = e->expressions.find(name);
    if (found == e->expressions.end())
    {
        return;
    }
    hfw::expression_set_fades(found->second, seconds, seconds);
}

void engine_physics_set(engine* e, float strength, float wind, float gravity_degrees)
{
    e->physics_weight = std::fmax(strength / 50.0f, 0.0f);
    e->wind = std::isfinite(wind) ? wind / 100.0f : 0.0f;
    if (e->simulation != NULL)
    {
        float radians = fmodf(gravity_degrees, 360.0f) * (float)M_PI / 180.0f;
        hfw::physics_set_gravity(e->simulation, sinf(radians), -cosf(radians));
        hfw::physics_set_wind(e->simulation, 0.0f, 0.0f);
    }
}

void engine_physics_scale(engine* e, const char* id, float strength, float wind)
{
    int32_t index = hfw::param_index(e->model, id);
    if (index >= hfw::param_count(e->model))
    {
        return;
    }
    for (physics_scale& scale : e->physics_scales)
    {
        if (scale.index == index)
        {
            scale.strength = strength;
            scale.wind_multiplier = wind;
            return;
        }
    }
    e->physics_scales.push_back({index, strength, wind});
}

void engine_expressions_stop(engine* e)
{
    hfw::layer_destroy(e->main_layer);
    hfw::layer_destroy(e->pose_layer);
    e->pose_layer = hfw::layer_create();
    e->main_layer = hfw::layer_create();
    e->layers[0] = e->pose_layer;
    e->layers[1] = e->main_layer;
}

void engine_breath_enable(engine* e, bool on)
{
    hfw::breath_set(e->breathing, e->breath_waves, on ? 5 : 0);
}

bool engine_idle_motions_enable(engine* e, bool on)
{
    e->idle_paused = !on;
    if (!on)
    {
        hfw::queue_fade_out_all(e->motion_queue);
    }
    return !e->motions.empty();
}

bool engine_motion_add(engine* e, const char* group, const char* path)
{
    hfw::motion* motion = motion_load(e, path, -1, -1);
    if (motion == NULL)
    {
        return false;
    }
    e->motions[group].push_back(motion);
    e->rig.save_before_hook = true;
    return true;
}

bool engine_motion_idle(engine* e, const char* group)
{
    auto found = e->motions.find(group);
    if (found == e->motions.end())
    {
        return false;
    }
    e->idle = &found->second;
    if (found->second.size() == 1)
    {
        hfw::motion_set_loop(found->second[0], true, false);
    }
    return true;
}

float engine_motion_play(engine* e, const char* group, bool looped)
{
    auto found = e->motions.find(group);
    if (found == e->motions.end() || &found->second == e->idle)
    {
        return -1.0f;
    }
    hfw::motion* motion = motion_pick(e, found->second);
    hfw::motion_set_loop(motion, looped, !looped);
    hfw::queue_start(e->motion_queue, motion);
    return looped ? 0.0f : hfw::motion_duration(motion);
}

static void copy_params(engine* e, std::vector<float>& out)
{
    const float* values = hfw::param_values(e->model);
    out.assign(values, values + hfw::param_count(e->model));
}

static void idle_hook(void* context)
{
    engine* target = (engine*)context;
    if (target->idle != NULL && !target->idle_paused)
    {
        hfw::queue_start(target->motion_queue, motion_pick(target, *target->idle));
    }
}

static void rest_hook(void* context)
{
    engine* target = (engine*)context;
    target->rest_marks.assign(hfw::param_count(target->model), 0);
    bool eyes_held = false;
    if (target->idle != NULL)
    {
        for (hfw::motion* motion : *target->idle)
        {
            for (int32_t index : target->motion_params[motion])
            {
                target->rest_marks[index] = 2;
            }
        }
    }
    for (int32_t i = 0; i < hfw::queue_entry_count(target->motion_queue); i++)
    {
        const hfw::motion* motion = hfw::queue_entry_motion(target->motion_queue, i);
        float end_time = hfw::queue_entry_end_time(target->motion_queue, i);
        char mark = hfw::queue_entry_fade_out_triggered(target->motion_queue, i) || (end_time >= 0.0f && hfw::queue_entry_state_time(target->motion_queue, i) >= end_time - hfw::motion_fade_out(motion)) ? 1 : 2;
        const std::vector<int32_t>& parameter_indices = target->motion_params[motion];
        if (mark == 2 && target->eye_blink != NULL && (target->idle == NULL || std::find(target->idle->begin(), target->idle->end(), motion) == target->idle->end()))
        {
            const int32_t* blink_parameters = hfw::blink_params(target->eye_blink);
            for (int32_t blink_index = 0; blink_index < hfw::blink_param_count(target->eye_blink); blink_index++)
            {
                eyes_held = eyes_held || std::find(parameter_indices.begin(), parameter_indices.end(), blink_parameters[blink_index]) != parameter_indices.end();
            }
        }
        for (int32_t index : parameter_indices)
        {
            target->rest_marks[index] = std::max(target->rest_marks[index], mark);
        }
    }
    target->rig.eye_blink = eyes_held ? NULL : target->eye_blink;
    for (int32_t i = 0; i < hfw::queue_entry_count(target->motion_queue); i++)
    {
        for (int32_t index : target->motion_params[hfw::queue_entry_motion(target->motion_queue, i)])
        {
            if (target->rest_marks[index] == 1)
            {
                hfw::param_set(target->model, index, hfw::param_default(target->model, index));
            }
        }
    }
}

static void after_motion_hook(void* context)
{
    engine* target = (engine*)context;
    if (target->after_motion_update != NULL)
    {
        target->after_motion_update(target->hook_context);
    }
    if (target->rig.eye_blink != NULL)
    {
        srand(target->blink_seed);
    }
}

static void before_physics_hook(void* context, float elapsed_seconds)
{
    engine* target = (engine*)context;
    target->gust = 0.0f;
    if (target->wind != 0.0f)
    {
        target->wind_clock += elapsed_seconds;
        double time = target->wind_clock;
        const float* phases = target->wind_phase;
        double gusts = 0.45 * sin(3.1 * time + phases[0]) + 0.35 * sin(5.3 * time + phases[1]) + 0.2 * sin(1.3 * time + phases[2]);
        float wind_x = target->wind * (float)((0.45 + 0.45 * gusts) * (0.7 + 0.3 * sin(0.23 * time + phases[3])));
        float wind_y = target->wind * (float)(0.08 * sin(2.9 * time + phases[1]));
        hfw::physics_set_wind(target->simulation, wind_x, wind_y);
        if (!target->physics_scales.empty())
        {
            target->gust = std::fmin(std::hypot(wind_x, wind_y) / (0.9f * std::fabs(target->wind)), 1.0f);
        }
    }
    if (target->physics_weight != 1.0f || !target->physics_scales.empty())
    {
        copy_params(target, target->pre_physics);
    }
}

static void after_physics_hook(void* context, float)
{
    engine* target = (engine*)context;
    if (target->physics_weight == 1.0f && target->physics_scales.empty())
    {
        return;
    }
    for (const physics_scale& scale : target->physics_scales)
    {
        float before = target->pre_physics[scale.index];
        float value = hfw::param_get(target->model, scale.index);
        if (value == before)
        {
            continue;
        }
        float weight = target->physics_weight * scale.strength * (1.0f + (scale.wind_multiplier - 1.0f) * target->gust);
        hfw::param_set(target->model, scale.index, before + (value - before) * weight);
        target->pre_physics[scale.index] = hfw::param_get(target->model, scale.index);
    }
    for (size_t i = 0; i < target->pre_physics.size(); i++)
    {
        float value = hfw::param_get(target->model, (int32_t)i);
        if (value != target->pre_physics[i])
        {
            hfw::param_set(target->model, (int32_t)i, target->pre_physics[i] + (value - target->pre_physics[i]) * target->physics_weight);
        }
    }
}

static void before_model_hook(void* context)
{
    engine* target = (engine*)context;
    if (target->rig.eye_blink != NULL)
    {
        target->blink_seed = (unsigned int)rand();
    }
    if (target->carry_left_ms > 0.0f)
    {
        float progress = target->carry_left_ms / target->carry_total_ms;
        float weight = progress * progress * (3.0f - 2.0f * progress);
        for (size_t i = 0; i < target->carry_params.size(); i++)
        {
            float value = hfw::param_get(target->model, (int32_t)i);
            hfw::param_set(target->model, (int32_t)i, value + (target->carry_params[i] - value) * weight);
        }
        for (size_t i = 0; i < target->carry_parts.size(); i++)
        {
            float value = hfw::part_opacity(target->model, (int32_t)i);
            hfw::part_set_opacity(target->model, (int32_t)i, value + (target->carry_parts[i] - value) * weight);
        }
        target->carry_left_ms -= target->frame_ms;
    }
    if (target->before_model_update != NULL)
    {
        target->before_model_update(target->hook_context);
    }
    copy_params(target, target->shown_params);
    const float* parts = hfw::part_opacities(target->model);
    target->shown_parts.assign(parts, parts + hfw::part_count(target->model));
    target->update_pending = true;
}

void engine_update(engine* e, float dt_ms, engine_hook after_motion_update, engine_hook before_model_update, void* context)
{
    hfw::rig_hooks hooks = {e, idle_hook, rest_hook, after_motion_hook, before_physics_hook, after_physics_hook, before_model_hook};
    e->frame_ms = dt_ms;
    e->after_motion_update = after_motion_update;
    e->before_model_update = before_model_update;
    e->hook_context = context;
    hfw::rig_update(&e->rig, dt_ms / 1000.0f, &hooks);
    e->rig.eye_blink = e->eye_blink;
}

static void update_shown(engine* e)
{
    if (!e->update_pending)
    {
        return;
    }
    hfw::params_save(e->model);
    for (size_t i = 0; i < e->shown_params.size(); i++)
    {
        hfw::param_set(e->model, (int32_t)i, e->shown_params[i]);
    }
    for (size_t i = 0; i < e->shown_parts.size(); i++)
    {
        hfw::part_set_opacity(e->model, (int32_t)i, e->shown_parts[i]);
    }
    hfw::model_update(e->model);
    hfw::params_restore(e->model);
    e->update_pending = false;
}

void engine_carry(engine* e, engine* source, float ms)
{
    e->carry_params = source->shown_params;
    e->carry_parts = source->shown_parts;
    e->carry_left_ms = ms;
    e->carry_total_ms = ms;
}

void engine_mirror(engine* e, engine* source)
{
    if (e->model == source->model)
    {
        update_shown(source);
        return;
    }
    for (size_t i = 0; i < source->shown_params.size(); i++)
    {
        hfw::param_set(e->model, (int32_t)i, source->shown_params[i]);
    }
    for (size_t i = 0; i < source->shown_parts.size(); i++)
    {
        hfw::part_set_opacity(e->model, (int32_t)i, source->shown_parts[i]);
    }
    hfw::model_update(e->model);
    e->update_pending = false;
}

void engine_background_clear(engine* e)
{
    e->background_frames.clear();
    e->background_ms.clear();
    e->background_total_ms = 0;
}

static const uint8_t* background_at(engine* e, float background_clock_ms)
{
    if (e->background_frames.empty())
    {
        return NULL;
    }
    float t = fmodf(floorf(background_clock_ms / kBackgroundStepMs) * kBackgroundStepMs, e->background_total_ms);
    size_t index = 0;
    while (index + 1 < e->background_frames.size() && t >= e->background_ms[index])
    {
        t -= e->background_ms[index];
        index++;
    }
    return e->background_frames[index].data();
}

void engine_params_ahead(engine* e, float ms, const int32_t* indices, float* values, int count)
{
    std::vector<float>& saved = e->ahead_parameters;
    std::vector<float>& saved_parts = e->ahead_parts;
    copy_params(e, saved);
    saved_parts.resize(hfw::part_count(e->model));
    for (size_t i = 0; i < saved_parts.size(); i++)
    {
        saved_parts[i] = hfw::part_opacity(e->model, (int32_t)i);
    }
    float saved_opacity = hfw::model_opacity(e->model);
    hfw::queue_look_ahead(e->motion_queue, e->model, ms / 1000.0f);
    for (int i = 0; i < count; i++)
    {
        values[i] = hfw::param_get(e->model, indices[i]);
    }
    for (size_t i = 0; i < saved.size(); i++)
    {
        hfw::param_set(e->model, (int32_t)i, saved[i]);
    }
    for (size_t i = 0; i < saved_parts.size(); i++)
    {
        hfw::part_set_opacity(e->model, (int32_t)i, saved_parts[i]);
    }
    hfw::model_set_opacity(e->model, saved_opacity);
}
