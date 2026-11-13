use crate::format::{COLOR_POOLS, Moc, span};
use crate::ffi::{fmaf};
use crate::keyforms::{INDEX, WEIGHT};
use crate::model::State;
use core::ops::Range;

pub fn shape_weights(moc: &Moc, state: &mut State, force: bool) {
    let constraint_parameters = moc.section::<i32>(132);
    let value_begins = moc.section::<i32>(133);
    let value_counts = moc.section::<i32>(134);
    let key_pool = moc.section::<f32>(135);
    let value_pool = moc.section::<f32>(136);
    for constraint in 0..constraint_parameters.len() {
        let parameter = constraint_parameters[constraint];
        if parameter != -1 && !force && state.parameter_changed[parameter as usize] == 0 {
            continue;
        }
        let keys = span(key_pool, value_begins[constraint], value_counts[constraint]);
        let values = span(value_pool, value_begins[constraint], value_counts[constraint]);
        state.constraint_values[constraint] = if parameter == -1 || keys.is_empty() {
            1.0
        } else {
            let value = state.parameter_effective[parameter as usize];
            let mut result = values[keys.len() - 1];
            if keys.len() == 1 || value <= keys[0] {
                result = values[0];
            } else {
                for key in 1..keys.len() {
                    if value < keys[key] {
                        let fraction = (value - keys[key - 1]) / (keys[key] - keys[key - 1]);
                        result = fmaf(values[key - 1], 1.0 - fraction, fraction * values[key]);
                        break;
                    }
                }
            }
            result
        };
    }

    let parameter_bindings = moc.section::<i32>(120);
    let key_counts = moc.section::<i32>(118);
    let base_keys = moc.section::<i32>(119);
    let constraint_begins = moc.section::<i32>(123);
    let constraint_counts = moc.section::<i32>(124);
    let constraint_list = moc.section::<i32>(131);
    for binding in 0..parameter_bindings.len() {
        let parameter_binding = parameter_bindings[binding] as usize;
        let flags = state.shape_binding_flags[parameter_binding];
        let mut weights_changed = force || flags & WEIGHT != 0;
        let mut indices_changed = force || flags & INDEX != 0;
        let mut weight: f32 = 1.0;
        for &constraint in span(constraint_list, constraint_begins[binding], constraint_counts[binding]) {
            let value = state.constraint_values[constraint as usize];
            weight = if weight < value { weight } else { value };
        }
        state.shape_dirty[binding] = (weights_changed || indices_changed || weight.to_bits() != state.shape_weight[binding].to_bits()) as u8;
        state.shape_weight[binding] = weight;
        if !weights_changed && !indices_changed {
            continue;
        }
        let mut interval = state.shape_binding_interval[parameter_binding];
        let mut fraction = state.shape_binding_fraction[parameter_binding];
        let base = base_keys[parameter_binding];
        state.shape_kind[binding] = if key_counts[parameter_binding] < 1 {
            0
        } else if fraction == 0.0 {
            (interval != base) as u8
        } else if interval == base {
            weights_changed = true;
            indices_changed = true;
            fraction = 1.0 - fraction;
            interval += 1;
            1
        } else if interval + 1 != base {
            2
        } else {
            1
        };
        if weights_changed {
            state.shape_first_weight[binding] = 1.0 - fraction;
            state.shape_second_weight[binding] = fraction;
        }
        if indices_changed {
            state.shape_first[binding] = interval;
            state.shape_second[binding] = interval + 1;
        }
    }
}

fn accumulate(state: &State, bases: &[i32], entries: Range<usize>, pool: &[f32], map: &[i32], values: &mut [f32]) {
    for entry in entries {
        let kind = state.shape_kind[entry];
        if kind == 0 {
            continue;
        }
        let mut first = (bases[entry] + state.shape_first[entry]) as usize;
        let mut second = if kind == 2 { (bases[entry] + state.shape_second[entry]) as usize } else { first };
        let weight = state.shape_weight[entry];
        let first_weight = state.shape_first_weight[entry];
        let second_weight = state.shape_second_weight[entry];
        if map.is_empty() {
            let term = if kind == 1 { pool[first] * first_weight } else { fmaf(pool[first], first_weight, pool[second] * second_weight) };
            for value in values.iter_mut() {
                *value = fmaf(weight, term, *value);
            }
            continue;
        }
        first = map[first] as usize;
        second = map[second] as usize;
        let firsts = &pool[first..first + values.len()];
        if kind == 1 {
            for (value, &source) in values.iter_mut().zip(firsts) {
                *value = fmaf(weight, source * first_weight, *value);
            }
        } else {
            let seconds = &pool[second..second + values.len()];
            for ((value, &source), &other) in values.iter_mut().zip(firsts).zip(seconds) {
                *value = fmaf(weight, fmaf(source, first_weight, other * second_weight), *value);
            }
        }
    }
}

fn scalar(state: &State, bases: &[i32], entries: Range<usize>, pool: &[f32], value: f32) -> f32 {
    let mut values = [value];
    accumulate(state, bases, entries, pool, &[], &mut values);
    values[0]
}

fn colors(state: &State, bases: &[i32], entries: Range<usize>, pools: &[&[f32]; 6], maps: [&[i32]; 2], multiply: &[f32], screen: &[f32]) -> [f32; 6] {
    let mut result = [0.0; 6];
    for channel in 0..3 {
        let mut value = [multiply[channel]];
        accumulate(state, bases, entries.clone(), pools[channel], maps[0], &mut value);
        result[channel] = value[0].clamp(0.0, 1.0);
    }
    for channel in 0..3 {
        let mut value = [screen[channel]];
        accumulate(state, bases, entries.clone(), pools[3 + channel], maps[1], &mut value);
        result[3 + channel] = value[0].clamp(0.0, 1.0);
    }
    result
}

pub fn blend_shapes(moc: &Moc, state: &mut State) {
    let scalars = moc.version >= 5;
    let bases = moc.section::<i32>(121);
    let positions = moc.section::<f32>(71);
    let pools = COLOR_POOLS.map(|entry| moc.section::<f32>(entry));

    let targets = moc.section::<i32>(143);
    let begins = moc.section::<i32>(144);
    let counts = moc.section::<i32>(145);
    let part_draw_orders = moc.section::<f32>(58);
    for record in 0..targets.len() {
        let target = targets[record] as usize;
        let first = begins[record] as usize;
        let entries = first..first + counts[record] as usize;
        let order = scalar(state, bases, entries, part_draw_orders, state.part_draw_orders[target] as f32);
        state.part_draw_orders[target] = (order.clamp(0.0, 1000.0) + 0.001) as i32;
    }

    let targets = moc.section::<i32>(125);
    let begins = moc.section::<i32>(126);
    let counts = moc.section::<i32>(127);
    let grid_points = moc.section::<i32>(22);
    let warp_maps = [moc.section::<i32>(137), moc.section(138)];
    let warp_positions = moc.section::<i32>(60);
    let warp_opacities = moc.section::<f32>(59);
    for record in 0..targets.len() {
        let target = targets[record] as usize;
        if state.warp_dirty[target] == 0 {
            continue;
        }
        let first = begins[record] as usize;
        let entries = first..first + counts[record] as usize;
        let begin = state.warp_grid_begin[target] as usize;
        let grids = core::mem::take(&mut state.warp_grids);
        accumulate(state, bases, entries.clone(), positions, warp_positions, &mut grids[begin..begin + grid_points[target] as usize * 2]);
        state.warp_grids = grids;
        if scalars {
            let opacity = scalar(state, bases, entries.clone(), warp_opacities, state.warp_opacities[target]);
            state.warp_opacities[target] = opacity.clamp(0.0, 1.0);
            let start = target * 3;
            let rgb = colors(state, bases, entries, &pools, warp_maps, &state.warp_multiply[start..start + 3], &state.warp_screen[start..start + 3]);
            state.warp_multiply[start..start + 3].copy_from_slice(&rgb[..3]);
            state.warp_screen[start..start + 3].copy_from_slice(&rgb[3..]);
        }
    }

    let targets = moc.section::<i32>(146);
    let begins = moc.section::<i32>(147);
    let counts = moc.section::<i32>(148);
    let rotation_maps = [moc.section::<i32>(139), moc.section(140)];
    let rotation_opacities = moc.section::<f32>(61);
    let rotation_angles = moc.section::<f32>(62);
    let rotation_origin_x = moc.section::<f32>(63);
    let rotation_origin_y = moc.section::<f32>(64);
    let rotation_scales = moc.section::<f32>(65);
    for record in 0..targets.len() {
        let target = targets[record] as usize;
        if state.rotation_dirty[target] == 0 {
            continue;
        }
        let first = begins[record] as usize;
        let entries = first..first + counts[record] as usize;
        state.rotation_origin_x[target] = scalar(state, bases, entries.clone(), rotation_origin_x, state.rotation_origin_x[target]);
        state.rotation_origin_y[target] = scalar(state, bases, entries.clone(), rotation_origin_y, state.rotation_origin_y[target]);
        let opacity = scalar(state, bases, entries.clone(), rotation_opacities, state.rotation_opacities[target]);
        state.rotation_opacities[target] = opacity.clamp(0.0, 1.0);
        let start = target * 3;
        let rgb = colors(state, bases, entries.clone(), &pools, rotation_maps, &state.rotation_multiply[start..start + 3], &state.rotation_screen[start..start + 3]);
        state.rotation_multiply[start..start + 3].copy_from_slice(&rgb[..3]);
        state.rotation_screen[start..start + 3].copy_from_slice(&rgb[3..]);
        let angle = scalar(state, bases, entries.clone(), rotation_angles, state.rotation_angles[target]);
        state.rotation_angles[target] = angle.clamp(-3600.0, 3600.0);
        let scale = scalar(state, bases, entries, rotation_scales, state.rotation_scales[target]);
        state.rotation_scales[target] = scale.clamp(0.0001, 100.0);
    }

    let targets = moc.section::<i32>(128);
    let begins = moc.section::<i32>(129);
    let counts = moc.section::<i32>(130);
    let drawable_maps = [moc.section::<i32>(141), moc.section(142)];
    let drawable_opacities = moc.section::<f32>(68);
    let drawable_draw_orders = moc.section::<f32>(69);
    for record in 0..targets.len() {
        let target = targets[record] as usize;
        let first = begins[record] as usize;
        let entries = first..first + counts[record] as usize;
        if scalars {
            let order = scalar(state, bases, entries.clone(), drawable_draw_orders, state.drawable_draw_orders[target] as f32);
            state.drawable_draw_orders[target] = (order.clamp(0.0, 1000.0) + 0.001) as i32;
            let opacity = scalar(state, bases, entries.clone(), drawable_opacities, state.drawable_opacities[target]);
            state.drawable_opacities[target] = opacity.clamp(0.0, 1.0);
            let start = target * 4;
            let rgb = colors(state, bases, entries, &pools, drawable_maps, &state.drawable_multiply[start..start + 3], &state.drawable_screen[start..start + 3]);
            state.drawable_multiply[start..start + 3].copy_from_slice(&rgb[..3]);
            state.drawable_screen[start..start + 3].copy_from_slice(&rgb[3..]);
        }
    }

    let targets = moc.section::<i32>(149);
    let begins = moc.section::<i32>(150);
    let counts = moc.section::<i32>(151);
    let glue_intensities = moc.section::<f32>(100);
    for record in 0..targets.len() {
        let target = targets[record] as usize;
        let first = begins[record] as usize;
        let entries = first..first + counts[record] as usize;
        let weight = scalar(state, bases, entries, glue_intensities, state.glue_weights[target]);
        state.glue_weights[target] = weight.clamp(0.0, 1.0);
    }
}

pub fn shape_positions(moc: &Moc, state: &mut State) {
    let bases = moc.section::<i32>(121);
    let positions = moc.section::<f32>(71);
    let targets = moc.section::<i32>(128);
    let begins = moc.section::<i32>(129);
    let counts = moc.section::<i32>(130);
    let vertex_counts = moc.section::<i32>(43);
    let drawable_positions = moc.section::<i32>(70);
    for record in 0..targets.len() {
        let target = targets[record] as usize;
        if state.drawable_dirty[target] == 0 {
            continue;
        }
        let first = begins[record] as usize;
        let begin = state.drawable_position_begin[target] as usize;
        let vertices = core::mem::take(&mut state.positions);
        accumulate(state, bases, first..first + counts[record] as usize, positions, drawable_positions, &mut vertices[begin..begin + vertex_counts[target] as usize * 2]);
        state.positions = vertices;
    }
}

pub fn apply(moc: &Moc, state: &mut State) {
    let first_meshes = moc.section::<i32>(94);
    let second_meshes = moc.section::<i32>(95);
    let info_begins = moc.section::<i32>(96);
    let info_counts = moc.section::<i32>(97);
    let info_weights = moc.section::<f32>(98);
    let info_vertices = moc.section::<u16>(99);
    for glue in 0..first_meshes.len() {
        if state.drawable_dirty[first_meshes[glue] as usize] | state.drawable_dirty[second_meshes[glue] as usize] == 0 {
            continue;
        }
        let weight = state.glue_weights[glue];
        let first_begin = state.drawable_position_begin[first_meshes[glue] as usize] as usize / 2;
        let second_begin = state.drawable_position_begin[second_meshes[glue] as usize] as usize / 2;
        let begin = info_begins[glue] as usize;
        let entries = begin..begin + info_counts[glue] as usize / 2 * 2;
        let pairs = info_vertices[entries.clone()].as_chunks::<2>().0;
        let vertices = state.positions.as_chunks_mut::<2>().0;
        for (pair, weights) in pairs.iter().zip(info_weights[entries].as_chunks::<2>().0) {
            let first = first_begin + (pair[0] & 0x7fff) as usize;
            let second = second_begin + (pair[1] & 0x7fff) as usize;
            let [first_x, first_y] = vertices[first];
            let [second_x, second_y] = vertices[second];
            vertices[first] = [
                fmaf(weight, (second_x - first_x) * weights[0], first_x),
                fmaf(weight, (second_y - first_y) * weights[0], first_y),
            ];
            vertices[second] = [
                fmaf(weight, (first_x - second_x) * weights[1], second_x),
                fmaf(weight, (first_y - second_y) * weights[1], second_y),
            ];
        }
    }
}
