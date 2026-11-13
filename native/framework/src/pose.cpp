#include "internal.h"
#include "json.h"

#include <string>

namespace hfw {

struct pose_member {
    int32_t parameter;
    int32_t part;
    int32_t link_first;
    int32_t link_count;
};

struct pose_group {
    int32_t first;
    int32_t count;
};

struct pose {
    std::vector<pose_group> groups;
    std::vector<pose_member> members;
    std::vector<int32_t> link_parameters;
    std::vector<int32_t> link_parts;
    float fade_time;
    bool started;
};

pose* pose_load(model* target, const uint8_t* bytes, size_t length, status* output_status)
{
    if (length == 0) {
        report(output_status, status::empty);
        return nullptr;
    }
    json_document document;
    if (!json_parse(document, bytes, length)) {
        report(output_status, status::json);
        return nullptr;
    }
    int32_t root = json_root(document);
    pose* result = new pose();
    result->fade_time = json_float(document, json_find(document, root, "FadeInTime"), 0.5f, 0.5f);
    if (result->fade_time < 0.0f) {
        result->fade_time = 0.5f;
    }
    result->started = false;
    std::string identifier;
    int32_t groups = json_find(document, root, "Groups");
    int32_t group_count = json_size(document, groups);
    for (int32_t group_index = 0; group_index < group_count; group_index++) {
        int32_t group = json_at(document, groups, group_index);
        int32_t member_count = json_size(document, group);
        result->groups.push_back({static_cast<int32_t>(result->members.size()), member_count});
        for (int32_t i = 0; i < member_count; i++) {
            int32_t entry = json_at(document, group, i);
            identifier.clear();
            json_text(document, json_find(document, entry, "Id"), identifier);
            pose_member member;
            member.parameter = param_index(target, identifier.c_str());
            member.part = part_index(target, identifier.c_str());
            member.link_first = static_cast<int32_t>(result->link_parameters.size());
            int32_t links = json_find(document, entry, "Link");
            member.link_count = json_size(document, links);
            for (int32_t link = 0; link < member.link_count; link++) {
                identifier.clear();
                json_text(document, json_at(document, links, link), identifier);
                result->link_parameters.push_back(param_index(target, identifier.c_str()));
                result->link_parts.push_back(part_index(target, identifier.c_str()));
            }
            result->members.push_back(member);
        }
    }
    report(output_status, status::ok);
    return result;
}

void pose_destroy(pose* target)
{
    delete target;
}

static void reset(pose* target, model* owner)
{
    for (const pose_group& group : target->groups) {
        for (int32_t i = 0; i < group.count; i++) {
            const pose_member& member = target->members[group.first + i];
            float value = i == 0 ? 1.0f : 0.0f;
            part_set_opacity(owner, member.part, value);
            param_set(owner, member.parameter, value);
            for (int32_t link = 0; link < member.link_count; link++) {
                param_set(owner, target->link_parameters[member.link_first + link], 1.0f);
            }
        }
    }
}

void pose_update(pose* target, model* owner, float delta_time)
{
    if (!target->started) {
        reset(target, owner);
        target->started = true;
    }
    if (delta_time < 0.0f) {
        delta_time = 0.0f;
    }
    int32_t group_count = static_cast<int32_t>(target->groups.size());
    for (int32_t group_index = 0; group_index < group_count; group_index++) {
        const pose_group& group = target->groups[group_index];
        if (group.count == 0) {
            continue;
        }
        const pose_member* members = target->members.data() + group.first;
        int32_t visible = -1;
        for (int32_t i = 0; i < group.count; i++) {
            if (param_get(owner, members[i].parameter) > 0.001f) {
                visible = i;
                break;
            }
        }
        float shown = 1.0f;
        if (visible < 0) {
            visible = group_index == 0 ? 0 : -1;
        } else if (target->fade_time != 0.0f) {
            shown = part_opacity(owner, members[visible].part) + delta_time / target->fade_time;
            if (shown > 1.0f) {
                shown = 1.0f;
            }
        }
        if (visible >= 0) {
            part_set_opacity(owner, members[visible].part, shown);
        }
        float cap;
        if (shown < 0.5f) {
            cap = (shown * -0.5f) / 0.5f + 1.0f;
        } else {
            cap = ((1.0f - shown) * 0.5f) / 0.5f;
        }
        float hidden = 1.0f - shown;
        if ((1.0f - cap) * hidden > 0.15f) {
            cap = 1.0f - 0.15f / hidden;
        }
        for (int32_t i = 0; i < group.count; i++) {
            if (i != visible && part_opacity(owner, members[i].part) > cap) {
                part_set_opacity(owner, members[i].part, cap);
            }
        }
    }
    for (const pose_member& member : target->members) {
        float opacity = part_opacity(owner, member.part);
        for (int32_t link = 0; link < member.link_count; link++) {
            part_set_opacity(owner, target->link_parts[member.link_first + link], opacity);
        }
    }
}

}
