#include "internal.h"
#include "json.h"

#include <new>

namespace hfw {

static std::string field_text(const json_document& document, int32_t node)
{
    std::string text;
    json_text(document, node, text);
    return text;
}

static void read_texts(const json_document& document, int32_t array, std::vector<std::string>& output)
{
    int32_t count = json_size(document, array);
    output.reserve(count);
    for (int32_t i = 0; i < count; i++) {
        output.push_back(field_text(document, json_at(document, array, i)));
    }
}

static void read_motions(const json_document& document, int32_t motions, std::vector<motion_group>& output)
{
    if (motions < 0 || document.nodes[motions].kind != json_kind::object) {
        return;
    }
    const json_node& groups = document.nodes[motions];
    output.resize(groups.count);
    for (int32_t group_index = 0; group_index < groups.count; group_index++) {
        int32_t group = document.children[groups.first + group_index];
        output[group_index].name = json_key(document, group);
        int32_t count = json_size(document, group);
        output[group_index].entries.resize(count);
        for (int32_t i = 0; i < count; i++) {
            int32_t entry = json_at(document, group, i);
            motion_entry& target = output[group_index].entries[i];
            target.file = field_text(document, json_find(document, entry, "File"));
            target.fade_in = json_float(document, json_find(document, entry, "FadeInTime"), -1.0f, 0.0f);
            target.fade_out = json_float(document, json_find(document, entry, "FadeOutTime"), -1.0f, 0.0f);
        }
    }
}

static void read_groups(const json_document& document, int32_t groups, setting& target)
{
    bool eye_blink_found = false;
    bool lip_sync_found = false;
    int32_t count = json_size(document, groups);
    for (int32_t i = 0; i < count; i++) {
        int32_t entry = json_at(document, groups, i);
        if (document.nodes[entry].kind != json_kind::object) {
            continue;
        }
        std::string name = field_text(document, json_find(document, entry, "Name"));
        int32_t identifiers = json_find(document, entry, "Ids");
        if (name == "EyeBlink" && !eye_blink_found) {
            eye_blink_found = true;
            read_texts(document, identifiers, target.eye_blink_ids);
        } else if (name == "LipSync" && !lip_sync_found) {
            lip_sync_found = true;
            read_texts(document, identifiers, target.lip_sync_ids);
        }
    }
}

setting* setting_parse(const uint8_t* bytes, size_t length, status* output_status)
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
    setting* result = new (std::nothrow) setting();
    if (!result) {
        report(output_status, status::memory);
        return nullptr;
    }
    int32_t root = json_root(document);
    int32_t references = json_find(document, root, "FileReferences");
    result->moc = field_text(document, json_find(document, references, "Moc"));
    result->physics = field_text(document, json_find(document, references, "Physics"));
    result->pose = field_text(document, json_find(document, references, "Pose"));
    read_texts(document, json_find(document, references, "Textures"), result->textures);
    int32_t expressions = json_find(document, references, "Expressions");
    int32_t expression_count = json_size(document, expressions);
    result->expression_files.reserve(expression_count);
    for (int32_t i = 0; i < expression_count; i++) {
        result->expression_files.push_back(field_text(document, json_find(document, json_at(document, expressions, i), "File")));
    }
    read_motions(document, json_find(document, references, "Motions"), result->motion_groups);
    read_groups(document, json_find(document, root, "Groups"), *result);
    report(output_status, status::ok);
    return result;
}

void setting_destroy(setting* target)
{
    delete target;
}

const char* setting_moc(const setting* target)
{
    return target->moc.c_str();
}

int32_t setting_texture_count(const setting* target)
{
    return static_cast<int32_t>(target->textures.size());
}

const char* setting_texture(const setting* target, int32_t index)
{
    return target->textures[index].c_str();
}

const char* setting_physics(const setting* target)
{
    return target->physics.c_str();
}

const char* setting_pose(const setting* target)
{
    return target->pose.c_str();
}

int32_t setting_expression_count(const setting* target)
{
    return static_cast<int32_t>(target->expression_files.size());
}

const char* setting_expression_file(const setting* target, int32_t index)
{
    return target->expression_files[index].c_str();
}

int32_t setting_motion_group_count(const setting* target)
{
    return static_cast<int32_t>(target->motion_groups.size());
}

const char* setting_motion_group(const setting* target, int32_t group)
{
    return target->motion_groups[group].name.c_str();
}

int32_t setting_motion_count(const setting* target, int32_t group)
{
    return static_cast<int32_t>(target->motion_groups[group].entries.size());
}

const char* setting_motion_file(const setting* target, int32_t group, int32_t index)
{
    return target->motion_groups[group].entries[index].file.c_str();
}

}
