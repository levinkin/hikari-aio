use crate::format::{COLOR_POOLS, Moc};
use crate::ffi::{fmaf};
use crate::model::State;

pub fn blend(moc: &Moc, state: &mut State) {
    let owners = moc.section::<i32>(155);
    let part_bindings = moc.section::<i32>(4);
    let part_begins = moc.section::<i32>(5);
    let part_counts = moc.section::<i32>(6);
    let part_to_offscreen = moc.section::<i32>(160);
    let opacities = moc.section::<f32>(161);
    let color_begins = moc.section::<i32>(162);
    let pools = COLOR_POOLS.map(|entry| moc.section::<f32>(entry));
    let keyform_total = moc.counts[36] as i64;
    let color_total = moc.counts[23].min(moc.counts[24]) as i64;
    for (offscreen, &owner) in owners.iter().enumerate() {
        let owner = owner as usize;
        let keyform_count = part_counts[owner];
        if state.part_active[owner] == 0 || keyform_count < 1 {
            continue;
        }
        let begin = part_to_offscreen[part_begins[owner] as usize];
        if begin == -1 || begin as i64 + keyform_count as i64 > keyform_total {
            continue;
        }
        let color_begin = color_begins[begin as usize];
        if color_begin as i64 + keyform_count as i64 > color_total {
            continue;
        }
        let binding = part_bindings[owner] as usize;
        let first = state.keyform_combination_begin[binding] as usize;
        let count = state.keyform_combination_count[binding].max(0) as usize;
        let mut opacity = 0.0f32;
        let mut colors = [0.0f32; 6];
        for combination in first..first + count {
            let index = state.combination_indices[combination] as usize;
            let weight = state.combination_weights[combination];
            opacity += opacities[begin as usize + index] * weight;
            for channel in 0..6 {
                colors[channel] += pools[channel][color_begin as usize + index] * weight;
            }
        }
        state.offscreen_opacities[offscreen] = opacity;
        state.offscreen_colors[offscreen * 6..offscreen * 6 + 6].copy_from_slice(&colors);
    }
    for offscreen in 0..moc.counts[35] as usize {
        let colors = &state.offscreen_colors[offscreen * 6..offscreen * 6 + 6];
        state.offscreen_multiply[offscreen * 4..offscreen * 4 + 3].copy_from_slice(&colors[..3]);
        state.offscreen_screen[offscreen * 4..offscreen * 4 + 3].copy_from_slice(&colors[3..]);
    }
}

fn sources(keyform: usize, opacities: &[f32], maps: [&[i32]; 2], pools: &[&[f32]; 6]) -> [f32; 7] {
    let multiply = maps[0][keyform] as usize;
    let screen = maps[1][keyform] as usize;
    [
        opacities[keyform],
        pools[0][multiply],
        pools[1][multiply],
        pools[2][multiply],
        pools[3][screen],
        pools[4][screen],
        pools[5][screen],
    ]
}

pub fn blend_shapes(moc: &Moc, state: &mut State) {
    if moc.counts[35] == 0 {
        return;
    }
    let targets = moc.section::<i32>(143);
    let record_begins = moc.section::<i32>(144);
    let record_counts = moc.section::<i32>(145);
    let keyform_begins = moc.section::<i32>(121);
    let part_to_offscreen = moc.section::<i32>(160);
    let opacities = moc.section::<f32>(161);
    let maps = [moc.section::<i32>(162), moc.section::<i32>(163)];
    let pools = COLOR_POOLS.map(|entry| moc.section::<f32>(entry));
    for record in 0..moc.counts[32] as usize {
        let offscreen = state.part_offscreens[targets[record] as usize];
        if offscreen == -1 {
            continue;
        }
        let offscreen = offscreen as usize;
        let mut values = [
            state.offscreen_opacities[offscreen],
            state.offscreen_multiply[offscreen * 4],
            state.offscreen_multiply[offscreen * 4 + 1],
            state.offscreen_multiply[offscreen * 4 + 2],
            state.offscreen_screen[offscreen * 4],
            state.offscreen_screen[offscreen * 4 + 1],
            state.offscreen_screen[offscreen * 4 + 2],
        ];
        let begin = record_begins[record];
        for entry in begin..begin + record_counts[record] {
            let entry = entry as usize;
            let kind = state.shape_kind[entry];
            if kind != 1 && kind != 2 {
                continue;
            }
            let base = keyform_begins[entry];
            let first = part_to_offscreen[(base + state.shape_first[entry]) as usize];
            let second = if kind == 2 { part_to_offscreen[(base + state.shape_second[entry]) as usize] } else { first };
            if first == -1 || second == -1 {
                continue;
            }
            let shared = second == first;
            let first = sources(first as usize, opacities, maps, &pools);
            let second = if shared { first } else { sources(second as usize, opacities, maps, &pools) };
            let first_weight = state.shape_first_weight[entry];
            let second_weight = state.shape_second_weight[entry];
            let weight = state.shape_weight[entry];
            for channel in 0..7 {
                let term = if kind == 1 {
                    first[channel] * first_weight
                } else {
                    fmaf(first[channel], first_weight, second[channel] * second_weight)
                };
                values[channel] = fmaf(weight, term, values[channel]);
            }
        }
        state.offscreen_opacities[offscreen] = values[0].clamp(0.0, 1.0);
        for channel in 0..3 {
            state.offscreen_multiply[offscreen * 4 + channel] = values[1 + channel].clamp(0.0, 1.0);
            state.offscreen_screen[offscreen * 4 + channel] = values[4 + channel].clamp(0.0, 1.0);
        }
    }
}

pub fn finish(moc: &Moc, state: &mut State) {
    for (offscreen, &owner) in moc.section::<i32>(155).iter().enumerate() {
        if state.part_active[owner as usize] == 0 {
            state.offscreen_opacities[offscreen] = 0.0;
        }
    }
}
