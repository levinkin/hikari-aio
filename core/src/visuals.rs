use crate::format::Moc;
use crate::ffi::{fmaf};
use crate::model::State;

pub fn compose_parts(moc: &Moc, state: &mut State) {
    let part_parents = moc.section::<i32>(9);
    for (part, &parent) in part_parents.iter().enumerate() {
        if state.part_active[part] == 0 {
            continue;
        }
        let mut opacity = state.part_opacities[part];
        if parent != -1 && state.part_offscreens[parent as usize] == -1 {
            opacity *= state.part_composed_opacities[parent as usize];
        }
        state.part_composed_opacities[part] = opacity;
        let offscreen = state.part_offscreens[part];
        if offscreen != -1 {
            let current = state.offscreen_opacities[offscreen as usize];
            state.offscreen_opacities[offscreen as usize] = opacity * current;
        }
    }
}

pub fn select(moc: &Moc, state: &mut State) {
    let deformer_parents = moc.section::<i32>(16);
    let deformer_types = moc.section::<i32>(17);
    let deformer_targets = moc.section::<i32>(18);
    for deformer in 0..deformer_parents.len() {
        if state.deformer_active[deformer] == 0 || state.deformer_dirty[deformer] == 0 {
            continue;
        }
        let target = deformer_targets[deformer] as usize;
        let mut opacity = match deformer_types[deformer] {
            0 => state.warp_opacities[target],
            1 => state.rotation_opacities[target],
            _ => continue,
        };
        if deformer_parents[deformer] != -1 {
            opacity *= state.deformer_opacities[deformer_parents[deformer] as usize];
        }
        state.deformer_opacities[deformer] = opacity;
    }

    let drawable_parts = moc.section::<i32>(39);
    let drawable_deformers = moc.section::<i32>(40);
    let mut spreading = false;
    for drawable in 0..moc.counts[4] as usize {
        if state.drawable_active[drawable] == 0 || state.drawable_dirty[drawable] | state.drawable_stale[drawable] == 0 {
            continue;
        }
        let mut opacity = state.drawable_opacities[drawable];
        let deformer = drawable_deformers[drawable];
        if deformer != -1 {
            opacity *= state.deformer_opacities[deformer as usize];
        }
        let part = drawable_parts[drawable];
        if part != -1 && state.part_offscreens[part as usize] == -1 {
            opacity *= state.part_composed_opacities[part as usize];
        }
        let positioned = (opacity != 0.0 || state.drawable_pinned[drawable] != 0) as u8;
        spreading |= state.drawable_dirty[drawable] != positioned;
        state.drawable_dirty[drawable] = positioned;
        state.drawable_stale[drawable] = 1 - positioned;
    }

    let first_meshes = moc.section::<i32>(94);
    let second_meshes = moc.section::<i32>(95);
    while spreading {
        spreading = false;
        for glue in 0..first_meshes.len() {
            let first = first_meshes[glue] as usize;
            let second = second_meshes[glue] as usize;
            if state.drawable_dirty[first] != state.drawable_dirty[second] {
                for mesh in [first, second] {
                    state.drawable_dirty[mesh] = 1;
                    state.drawable_stale[mesh] = 0;
                }
                spreading = true;
            }
        }
    }

    state.deformer_needed.fill(0);
    for (drawable, &parent) in drawable_deformers.iter().enumerate() {
        if state.drawable_active[drawable] == 0 || state.drawable_dirty[drawable] == 0 {
            continue;
        }
        let mut deformer = parent;
        while deformer != -1 && state.deformer_needed[deformer as usize] == 0 {
            state.deformer_needed[deformer as usize] = 1;
            deformer = deformer_parents[deformer as usize];
        }
    }
    for deformer in 0..deformer_parents.len() {
        if state.deformer_needed[deformer] != 0 {
            state.deformer_dirty[deformer] |= state.deformer_stale[deformer];
            state.deformer_stale[deformer] = 0;
        } else if state.deformer_active[deformer] != 0 && state.deformer_dirty[deformer] != 0 {
            state.deformer_stale[deformer] = 1;
        }
    }
}

pub fn compose(moc: &Moc, state: &mut State) {
    let drawable_parts = moc.section::<i32>(39);
    let drawable_deformers = moc.section::<i32>(40);
    let colors = moc.version >= 4;
    for drawable in 0..moc.counts[4] as usize {
        if state.drawable_active[drawable] == 0 {
            continue;
        }
        let part = drawable_parts[drawable];
        if part != -1 && state.part_offscreens[part as usize] == -1 {
            let current = state.drawable_opacities[drawable];
            state.drawable_opacities[drawable] = state.part_composed_opacities[part as usize] * current;
        }
        let deformer = drawable_deformers[drawable];
        if !colors || deformer == -1 {
            continue;
        }
        let deformer = deformer as usize;
        for channel in 0..3 {
            let multiply = &mut state.drawable_multiply[drawable * 4 + channel];
            *multiply = (*multiply * state.deformer_multiply[deformer * 3 + channel]).clamp(0.0, 1.0);
            let screen = &mut state.drawable_screen[drawable * 4 + channel];
            let composed = state.deformer_screen[deformer * 3 + channel];
            *screen = fmaf(-*screen, composed, *screen + composed).clamp(0.0, 1.0);
        }
    }
}

pub fn finish(moc: &Moc, state: &mut State, force: bool, pending: bool) {
    let drawables = moc.counts[4] as usize;
    let vertex_counts = moc.section::<i32>(43);
    if moc.canvas().1 & 1 == 0 {
        for (drawable, &vertices) in vertex_counts.iter().enumerate() {
            if state.drawable_active[drawable] == 0 || state.drawable_dirty[drawable] == 0 {
                continue;
            }
            let begin = state.drawable_position_begin[drawable] as usize;
            let end = begin + vertices as usize * 2;
            for vertex in state.positions[begin..end].as_chunks_mut::<2>().0 {
                vertex[1] = -vertex[1];
            }
        }
    }

    let groups = moc.counts[18] as usize;
    let group_begins = moc.section::<i32>(81);
    let group_counts = moc.section::<i32>(82);
    let group_sizes = moc.section::<i32>(83);
    let group_minimums = moc.section::<i32>(85);
    let object_types = moc.section::<i32>(86);
    let object_indices = moc.section::<i32>(87);
    let object_groups = moc.section::<i32>(88);
    for group in 0..groups {
        let begin = group_begins[group] as usize;
        let end = begin + group_counts[group] as usize;
        for object in begin..end {
            let index = object_indices[object] as usize;
            let (active, order) = if object_types[object] == 0 {
                (state.drawable_active[index], state.drawable_draw_orders[index])
            } else {
                (state.part_active[index], state.part_draw_orders[index])
            };
            state.group_keys[object] = if active != 0 { order } else { group_minimums[group] };
            if force {
                state.group_order[object] = object as i32;
            }
        }
        let keys = &*state.group_keys;
        state.group_order[begin..end].sort_unstable_by_key(|&object| (keys[object as usize], object));
        state.group_bases[group] = -1;
    }
    state.group_bases[0] = 0;
    for group in 0..groups {
        let mut next = state.group_bases[group];
        if next < 0 {
            continue;
        }
        let begin = group_begins[group] as usize;
        for position in begin..begin + group_counts[group] as usize {
            let object = state.group_order[position] as usize;
            let index = object_indices[object] as usize;
            if object_types[object] == 0 {
                state.render_orders[index] = next;
                next += 1;
                continue;
            }
            let offscreen = state.part_offscreens[index];
            if offscreen != -1 {
                state.render_orders[drawables + offscreen as usize] = next;
                next += 1;
            }
            let opened = object_groups[object] as usize;
            state.group_bases[opened] = next;
            next += group_sizes[opened];
        }
    }

    for drawable in 0..drawables {
        let active = state.drawable_active[drawable] != 0;
        let visible = (active && state.drawable_opacities[drawable] != 0.0) as u8;
        let moved = force || if pending { active } else { state.dynamic_flags[drawable] & 32 != 0 };
        state.dynamic_flags[drawable] = visible + 32 * moved as u8;
    }
}
