use crate::format::{COLOR_POOLS, Moc, span};
use crate::ffi::{fmaf, floorf};
use crate::model::State;

pub const OUT: u8 = 1;
pub const INDEX: u8 = 2;
pub const WEIGHT: u8 = 4;

const SNAP: f32 = 0.001;
const WIDTH: f32 = 0.0015;

struct Combinations<'a> {
    begins: &'a [i32],
    counts: &'a [i32],
    indices: &'a [i32],
    weights: &'a [f32],
}

impl<'a> Combinations<'a> {
    fn select(&self, binding: i32) -> (&'a [i32], &'a [f32]) {
        let begin = self.begins[binding as usize] as usize;
        let end = begin + self.counts[binding as usize] as usize;
        (&self.indices[begin..end], &self.weights[begin..end])
    }
}

fn settle(flags: &mut u8, interval: &mut i32, weight: &mut f32, index: i32, fraction: f32, out_of_range: bool) {
    *flags = if !out_of_range && *flags & OUT != 0 {
        INDEX | WEIGHT
    } else {
        let index_changed = if fraction == *weight { index != *interval } else { fraction == 0.0 || *weight == 0.0 || index != *interval };
        (out_of_range as u8 * OUT) | (index_changed as u8 * INDEX) | ((fraction != *weight) as u8 * WEIGHT)
    };
    *interval = index;
    *weight = fraction;
}

pub fn parameters(moc: &Moc, state: &mut State, force: bool) {
    let maximums = moc.section::<f32>(51);
    let minimums = moc.section::<f32>(52);
    let repeats = moc.section::<i32>(54);
    let binding_begins = moc.section::<i32>(56);
    let binding_counts = moc.section::<i32>(57);
    let key_begins = moc.section::<i32>(75);
    let key_counts = moc.section::<i32>(76);
    let keys = moc.section::<f32>(77);
    let types = moc.section::<i32>(114);
    let shape_begins = moc.section::<i32>(115);
    let shape_counts = moc.section::<i32>(116);
    let shape_key_begins = moc.section::<i32>(117);
    let shape_key_counts = moc.section::<i32>(118);
    for parameter in 0..moc.counts[5] as usize {
        let minimum = minimums[parameter];
        let maximum = maximums[parameter];
        let mut value = state.parameter_values[parameter];
        if repeats[parameter] != 0 {
            let range = maximum - minimum;
            let cycles = (value - minimum) / range;
            value = fmaf(cycles - floorf(cycles) as i32 as f32, range, minimum);
        } else {
            if value > maximum {
                value = maximum;
            }
            if value < minimum {
                value = minimum;
            }
            state.parameter_values[parameter] = value;
        }
        let changed = value != state.parameter_effective[parameter];
        if changed {
            state.parameter_effective[parameter] = value;
        }
        state.parameter_changed[parameter] = changed as u8;
        let shape = types.get(parameter) == Some(&1);
        let (begin, count) = if shape { (shape_begins[parameter], shape_counts[parameter]) } else { (binding_begins[parameter], binding_counts[parameter]) };
        for binding in begin.max(0) as usize..(begin + count).max(0) as usize {
            if shape {
                if !force && !changed {
                    state.shape_binding_flags[binding] &= OUT;
                    continue;
                }
                let keys = span(keys, shape_key_begins[binding], shape_key_counts[binding]);
                let mut index = 0;
                let mut fraction = 0.0;
                if keys.len() >= 2 && (value > keys[0] || value.is_nan()) {
                    index = keys.len() - 1;
                    for key in 1..keys.len() {
                        if value < keys[key] {
                            index = key - 1;
                            fraction = (value - keys[key - 1]) / (keys[key] - keys[key - 1]);
                            break;
                        }
                    }
                }
                settle(&mut state.shape_binding_flags[binding], &mut state.shape_binding_interval[binding], &mut state.shape_binding_fraction[binding], index as i32, fraction, false);
            } else {
                if !force && !changed {
                    state.binding_flags[binding] &= OUT;
                    continue;
                }
                let keys = span(keys, key_begins[binding], key_counts[binding]);
                let mut index = 0;
                let mut fraction = 0.0;
                let out_of_range = match keys.len() {
                    0 => false,
                    1 => !(keys[0] - SNAP < value && value < keys[0] + SNAP),
                    _ if value < keys[0] - SNAP => true,
                    _ if value < keys[0] + SNAP => false,
                    length => {
                        index = length - 1;
                        let mut out_of_range = true;
                        for key in 1..length {
                            if value < keys[key] + SNAP {
                                let width = keys[key] - keys[key - 1];
                                index = key - 1;
                                if keys[key] - SNAP < value {
                                    index = key;
                                } else if width >= WIDTH {
                                    fraction = (value - keys[key - 1]) / width;
                                }
                                out_of_range = false;
                                break;
                            }
                        }
                        out_of_range
                    }
                };
                settle(&mut state.binding_flags[binding], &mut state.binding_interval[binding], &mut state.binding_fraction[binding], index as i32, fraction, out_of_range);
            }
        }
    }
}

fn float_blend(source: &[f32], begin: i32, (indices, weights): (&[i32], &[f32])) -> f32 {
    let mut sum = 0.0;
    for (index, weight) in indices.iter().zip(weights) {
        sum += source[(begin + index) as usize] * weight;
    }
    sum
}

fn vector_blend(pool: &[f32], position_begins: &[i32], begin: i32, (indices, weights): (&[i32], &[f32]), out: &mut [f32]) {
    let mut sources = indices.iter().map(|index| &pool[position_begins[(begin + index) as usize] as usize..]);
    match *weights {
        [first_weight] => {
            for (target, &value) in out.iter_mut().zip(sources.next().unwrap_or_default()) {
                *target = fmaf(value, first_weight, 0.0);
            }
        }
        [first_weight, second_weight] => {
            let first = sources.next().unwrap_or_default();
            let second = sources.next().unwrap_or_default();
            for ((target, &value), &next) in out.iter_mut().zip(first).zip(second) {
                *target = fmaf(next, second_weight, fmaf(value, first_weight, 0.0));
            }
        }
        _ => {
            out.fill(0.0);
            for (source, &weight) in sources.zip(weights) {
                for (target, &value) in out.iter_mut().zip(source) {
                    *target = fmaf(value, weight, *target);
                }
            }
        }
    }
}

fn color_blend(pools: &[&[f32]; 6], begin: i32, combination: (&[i32], &[f32]), multiply: &mut [f32], screen: &mut [f32]) {
    for channel in 0..3 {
        multiply[channel] = float_blend(pools[channel], begin, combination);
        screen[channel] = float_blend(pools[channel + 3], begin, combination);
    }
}

pub fn blend(moc: &Moc, state: &mut State, force: bool) {
    let list = moc.section::<i32>(72);
    let list_begins = moc.section::<i32>(73);
    let list_counts = moc.section::<i32>(74);
    let key_counts = moc.section::<i32>(76);
    let mut start = 0;
    for binding in 0..moc.counts[12] as usize {
        let members = span(list, list_begins[binding], list_counts[binding]);
        let mut out_of_range = false;
        let mut index_changed = force;
        let mut weight_changed = force;
        let mut varying = 0;
        for &member in members {
            let flags = state.binding_flags[member as usize];
            out_of_range |= flags & OUT != 0;
            index_changed |= flags & INDEX != 0;
            weight_changed |= flags & WEIGHT != 0;
            varying += (key_counts[member as usize] >= 2) as u32;
        }
        let begin = start;
        start += 1 << varying;
        state.keyform_combination_begin[binding] = begin as i32;
        if out_of_range {
            state.keyform_flags[binding] = OUT;
            continue;
        }
        if !index_changed && !weight_changed {
            state.keyform_flags[binding] = 0;
            continue;
        }
        let mut count = 1;
        for &member in members {
            if state.binding_fraction[member as usize] != 0.0 {
                count <<= 1;
            }
        }
        for combination in 0..count {
            let mut index = 0;
            let mut weight = 1.0f32;
            let mut stride = 1;
            let mut bit = 0;
            for &member in members {
                let member = member as usize;
                let fraction = state.binding_fraction[member];
                let mut key = state.binding_interval[member];
                if fraction != 0.0 {
                    if combination >> bit & 1 != 0 {
                        key += 1;
                        weight *= fraction;
                    } else {
                        weight *= 1.0 - fraction;
                    }
                    bit += 1;
                }
                index += key * stride;
                stride *= key_counts[member];
            }
            state.combination_indices[begin + combination] = index;
            state.combination_weights[begin + combination] = weight;
        }
        state.keyform_combination_count[binding] = count as i32;
        state.keyform_flags[binding] = (index_changed as u8 * INDEX) | (weight_changed as u8 * WEIGHT);
    }

    let combinations = Combinations {
        begins: state.keyform_combination_begin,
        counts: state.keyform_combination_count,
        indices: state.combination_indices,
        weights: state.combination_weights,
    };
    let invalid = &*state.keyform_flags;
    for (entry, dirty) in [(125, &mut *state.warp_dirty), (146, &mut *state.rotation_dirty), (128, &mut *state.drawable_dirty), (149, &mut *state.glue_dirty)] {
        dirty.fill(force as u8);
        let targets = moc.section::<i32>(entry);
        let begins = moc.section::<i32>(entry + 1);
        let counts = moc.section::<i32>(entry + 2);
        for record in 0..targets.len() {
            if span(state.shape_dirty, begins[record], counts[record]).contains(&1) {
                dirty[targets[record] as usize] = 1;
            }
        }
    }
    let colored = moc.version >= 4;
    let pools = COLOR_POOLS.map(|entry| moc.section::<f32>(entry));
    let pool = moc.section::<f32>(71);

    let part_bindings = moc.section::<i32>(4);
    let part_begins = moc.section::<i32>(5);
    let part_enabled = moc.section::<i32>(8);
    let part_parents = moc.section::<i32>(9);
    let part_draw_orders = moc.section::<f32>(58);
    for part in 0..moc.counts[0] as usize {
        state.part_opacities[part] = state.part_opacities[part].clamp(0.0, 1.0);
        let parent = part_parents[part];
        let binding = part_bindings[part];
        let active = part_enabled[part] != 0 && (parent == -1 || state.part_active[parent as usize] != 0) && invalid[binding as usize] & OUT == 0;
        state.part_active[part] = active as u8;
        if active {
            state.part_draw_orders[part] = (float_blend(part_draw_orders, part_begins[part], combinations.select(binding)) + 0.001) as i32;
        }
    }

    let deformer_bindings = moc.section::<i32>(12);
    let deformer_enabled = moc.section::<i32>(14);
    let deformer_parts = moc.section::<i32>(15);
    let deformer_parents = moc.section::<i32>(16);
    let deformer_types = moc.section::<i32>(17);
    let deformer_targets = moc.section::<i32>(18);
    let warp_bindings = moc.section::<i32>(19);
    let warp_begins = moc.section::<i32>(20);
    let warp_points = moc.section::<i32>(22);
    let warp_colors = moc.section::<i32>(105);
    let warp_opacities = moc.section::<f32>(59);
    let warp_positions = moc.section::<i32>(60);
    let rotation_bindings = moc.section::<i32>(25);
    let rotation_begins = moc.section::<i32>(26);
    let rotation_colors = moc.section::<i32>(106);
    let rotation_pools = [61, 62, 63, 64, 65].map(|entry| moc.section::<f32>(entry));
    let rotation_reflect_x = moc.section::<i32>(66);
    let rotation_reflect_y = moc.section::<i32>(67);
    for deformer in 0..moc.counts[1] as usize {
        let part = deformer_parts[deformer];
        let parent = deformer_parents[deformer];
        let binding = deformer_bindings[deformer] as usize;
        let active = deformer_enabled[deformer] != 0
            && (part == -1 || state.part_active[part as usize] != 0)
            && (parent == -1 || state.deformer_active[parent as usize] != 0)
            && invalid[binding] & OUT == 0;
        let fresh = !active
            || state.deformer_active[deformer] == 0
            || invalid[binding] != 0
            || (parent != -1 && state.deformer_dirty[parent as usize] != 0);
        state.deformer_active[deformer] = active as u8;
        state.deformer_dirty[deformer] = 1;
        let target = deformer_targets[deformer] as usize;
        match deformer_types[deformer] {
            0 => {
                let dirty = &mut state.warp_dirty[target];
                *dirty |= (fresh || invalid[warp_bindings[target] as usize] != 0) as u8;
                state.deformer_dirty[deformer] = *dirty;
                if !active || *dirty == 0 {
                    continue;
                }
                let combination = combinations.select(warp_bindings[target]);
                let begin = warp_begins[target];
                state.warp_opacities[target] = float_blend(warp_opacities, begin, combination);
                let grid = state.warp_grid_begin[target] as usize;
                vector_blend(pool, warp_positions, begin, combination, &mut state.warp_grids[grid..grid + warp_points[target] as usize * 2]);
                if colored {
                    let colors = target * 3..target * 3 + 3;
                    color_blend(&pools, warp_colors[target], combination, &mut state.warp_multiply[colors.clone()], &mut state.warp_screen[colors]);
                }
            }
            1 => {
                let dirty = &mut state.rotation_dirty[target];
                *dirty |= (fresh || invalid[rotation_bindings[target] as usize] != 0) as u8;
                state.deformer_dirty[deformer] = *dirty;
                if !active || *dirty == 0 {
                    continue;
                }
                let combination = combinations.select(rotation_bindings[target]);
                let begin = rotation_begins[target];
                let mut values = [0.0f32; 5];
                for (&index, &weight) in combination.0.iter().zip(combination.1) {
                    for (value, pool) in values.iter_mut().zip(rotation_pools) {
                        *value += pool[(begin + index) as usize] * weight;
                    }
                }
                [
                    state.rotation_opacities[target],
                    state.rotation_angles[target],
                    state.rotation_origin_x[target],
                    state.rotation_origin_y[target],
                    state.rotation_scales[target],
                ] = values;
                if let Some(&first) = combination.0.first() {
                    state.rotation_reflect_x[target] = (rotation_reflect_x[(begin + first) as usize] != 0) as u8;
                    state.rotation_reflect_y[target] = (rotation_reflect_y[(begin + first) as usize] != 0) as u8;
                }
                if colored {
                    let colors = target * 3..target * 3 + 3;
                    color_blend(&pools, rotation_colors[target], combination, &mut state.rotation_multiply[colors.clone()], &mut state.rotation_screen[colors]);
                }
            }
            _ => {}
        }
    }

    let drawable_bindings = moc.section::<i32>(34);
    let drawable_begins = moc.section::<i32>(35);
    let drawable_enabled = moc.section::<i32>(38);
    let drawable_parts = moc.section::<i32>(39);
    let drawable_deformers = moc.section::<i32>(40);
    let drawable_colors = moc.section::<i32>(107);
    let drawable_opacities = moc.section::<f32>(68);
    let drawable_draw_orders = moc.section::<f32>(69);
    for drawable in 0..moc.counts[4] as usize {
        let part = drawable_parts[drawable];
        let deformer = drawable_deformers[drawable];
        let binding = drawable_bindings[drawable];
        let active = drawable_enabled[drawable] != 0
            && (part == -1 || state.part_active[part as usize] != 0)
            && (deformer == -1 || state.deformer_active[deformer as usize] != 0)
            && invalid[binding as usize] & OUT == 0;
        state.drawable_dirty[drawable] |= (!active
            || state.drawable_active[drawable] == 0
            || invalid[binding as usize] != 0
            || (deformer != -1 && state.deformer_dirty[deformer as usize] != 0)) as u8;
        state.drawable_active[drawable] = active as u8;
        let colors = drawable * 4..drawable * 4 + 3;
        let blended = &mut state.drawable_blended[drawable * 6..drawable * 6 + 6];
        if !active {
            if colored {
                state.drawable_multiply[colors.clone()].copy_from_slice(&blended[..3]);
                state.drawable_screen[colors].copy_from_slice(&blended[3..]);
            }
            continue;
        }
        let combination = combinations.select(binding);
        let begin = drawable_begins[drawable];
        state.drawable_opacities[drawable] = float_blend(drawable_opacities, begin, combination);
        state.drawable_draw_orders[drawable] = (float_blend(drawable_draw_orders, begin, combination) + 0.001) as i32;
        if colored {
            color_blend(&pools, drawable_colors[drawable], combination, &mut state.drawable_multiply[colors.clone()], &mut state.drawable_screen[colors.clone()]);
            blended[..3].copy_from_slice(&state.drawable_multiply[colors.clone()]);
            blended[3..].copy_from_slice(&state.drawable_screen[colors]);
        }
    }

    let glue_bindings = moc.section::<i32>(91);
    let glue_begins = moc.section::<i32>(92);
    let glue_intensities = moc.section::<f32>(100);
    let first_meshes = moc.section::<i32>(94);
    let second_meshes = moc.section::<i32>(95);
    for glue in 0..moc.counts[20] as usize {
        state.glue_weights[glue] = float_blend(glue_intensities, glue_begins[glue], combinations.select(glue_bindings[glue]));
        state.glue_dirty[glue] |= (invalid[glue_bindings[glue] as usize] != 0) as u8;
    }
    let mut spreading = true;
    while spreading {
        spreading = false;
        for glue in 0..first_meshes.len() {
            let first = first_meshes[glue] as usize;
            let second = second_meshes[glue] as usize;
            if (state.glue_dirty[glue] | state.drawable_dirty[first] | state.drawable_dirty[second]) != 0 && (state.drawable_dirty[first] & state.drawable_dirty[second]) == 0 {
                state.drawable_dirty[first] = 1;
                state.drawable_dirty[second] = 1;
                spreading = true;
            }
        }
    }
}

pub fn positions(moc: &Moc, state: &mut State) {
    let combinations = Combinations {
        begins: state.keyform_combination_begin,
        counts: state.keyform_combination_count,
        indices: state.combination_indices,
        weights: state.combination_weights,
    };
    let pool = moc.section::<f32>(71);
    let drawable_bindings = moc.section::<i32>(34);
    let drawable_begins = moc.section::<i32>(35);
    let drawable_vertices = moc.section::<i32>(43);
    let drawable_positions = moc.section::<i32>(70);
    for drawable in 0..moc.counts[4] as usize {
        if state.drawable_active[drawable] == 0 || state.drawable_dirty[drawable] == 0 {
            continue;
        }
        let begin = state.drawable_position_begin[drawable] as usize;
        vector_blend(pool, drawable_positions, drawable_begins[drawable], combinations.select(drawable_bindings[drawable]), &mut state.positions[begin..begin + drawable_vertices[drawable] as usize * 2]);
    }
}
