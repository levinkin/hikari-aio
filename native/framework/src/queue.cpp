#include "internal.h"

#include <new>

namespace hfw {

struct queue_entry {
    motion* source;
    bool started;
    bool triggered;
    float start;
    float fade_start;
    float end;
    float state_time;
    float fade_out_time;
};

struct queue {
    float clock;
    std::vector<queue_entry> entries;
};

void trigger_fade_end(float clock, float fade_out_time, float* end)
{
    float fade_end = clock + fade_out_time;
    if (*end < 0.0f || fade_end < *end) {
        *end = fade_end;
    }
}

queue* queue_create()
{
    return new (std::nothrow) queue();
}

void queue_destroy(queue* target)
{
    delete target;
}

void queue_fade_out_all(queue* target)
{
    for (queue_entry& entry : target->entries) {
        entry.triggered = true;
        entry.fade_out_time = motion_fade_out(entry.source);
    }
}

void queue_start(queue* target, motion* source)
{
    queue_fade_out_all(target);
    queue_entry entry = {};
    entry.source = source;
    entry.start = -1.0f;
    entry.fade_start = -1.0f;
    entry.end = -1.0f;
    target->entries.push_back(entry);
}

bool queue_empty(const queue* target)
{
    return target->entries.empty();
}

bool queue_update(queue* target, model* owner, float delta_time)
{
    target->clock = target->clock + delta_time;
    float clock = target->clock;
    size_t kept = 0;
    for (queue_entry& entry : target->entries) {
        entry.state_time = clock;
        if (motion_evaluate(entry.source, owner, clock, &entry.started, &entry.start, &entry.fade_start, &entry.end)) {
            continue;
        }
        if (entry.triggered) {
            trigger_fade_end(clock, entry.fade_out_time, &entry.end);
        }
        target->entries[kept++] = entry;
    }
    bool updated = !target->entries.empty();
    target->entries.resize(kept);
    return updated;
}

int32_t queue_entry_count(const queue* target)
{
    return static_cast<int32_t>(target->entries.size());
}

const motion* queue_entry_motion(const queue* target, int32_t index)
{
    return target->entries[index].source;
}

bool queue_entry_fade_out_triggered(const queue* target, int32_t index)
{
    return target->entries[index].triggered;
}

float queue_entry_end_time(const queue* target, int32_t index)
{
    return target->entries[index].end;
}

float queue_entry_state_time(const queue* target, int32_t index)
{
    return target->entries[index].state_time;
}

void queue_look_ahead(queue* target, model* owner, float seconds)
{
    for (const queue_entry& entry : target->entries) {
        queue_entry copy = entry;
        motion_evaluate(copy.source, owner, copy.state_time + seconds, &copy.started, &copy.start, &copy.fade_start, &copy.end);
    }
}

}
