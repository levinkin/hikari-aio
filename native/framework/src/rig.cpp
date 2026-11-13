#include "internal.h"

namespace hfw {

void rig_update(rig* target, float delta_time, const rig_hooks* hooks)
{
    static const rig_hooks no_hooks = {};
    if (!hooks) {
        hooks = &no_hooks;
    }
    model* owner = target->target;
    if (target->motions && queue_empty(target->motions) && hooks->idle) {
        hooks->idle(hooks->context);
    }
    if (hooks->before_motion) {
        hooks->before_motion(hooks->context);
    }
    bool motion_updated = target->motions && queue_update(target->motions, owner, delta_time);
    if (target->save_before_hook) {
        params_save(owner);
    }
    if (hooks->after_motion) {
        hooks->after_motion(hooks->context);
    }
    if (!target->save_before_hook) {
        params_save(owner);
    }
    for (int32_t i = 0; i < target->layer_count; i++) {
        layer_update(target->layers[i], owner, delta_time);
    }
    if (target->eye_blink) {
        if (motion_updated) {
            blink_update_scaled(target->eye_blink, owner, delta_time);
        } else {
            blink_update(target->eye_blink, owner, delta_time);
        }
    }
    if (target->breathing) {
        breath_update(target->breathing, owner, delta_time);
    }
    if (target->simulation) {
        if (hooks->before_physics) {
            hooks->before_physics(hooks->context, delta_time);
        }
        physics_evaluate(target->simulation, owner, delta_time);
        if (hooks->after_physics) {
            hooks->after_physics(hooks->context, delta_time);
        }
    }
    if (target->pose_state) {
        pose_update(target->pose_state, owner, delta_time);
    }
    if (hooks->before_model) {
        hooks->before_model(hooks->context);
    }
    params_restore(owner);
}

}
