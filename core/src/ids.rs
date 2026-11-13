use crate::format::{Moc, span};
use crate::model::State;

pub fn initialize(moc: &Moc, state: &mut State) {
    let parameter_ids = moc.section::<[u8; 64]>(50);
    state.parameter_values.copy_from_slice(moc.section(53));
    for (slot, id) in state.parameter_ids.iter_mut().zip(parameter_ids) {
        *slot = id.as_ptr();
    }
    if moc.version >= 4 {
        state.parameter_types.copy_from_slice(moc.section(114));
    }

    let part_ids = moc.section::<[u8; 64]>(3);
    let part_offscreens = moc.section::<i32>(152);
    for part in 0..part_ids.len() {
        state.part_ids[part] = part_ids[part].as_ptr();
        state.part_opacities[part] = 1.0;
        state.part_offscreens[part] = if moc.version >= 6 { part_offscreens[part] } else { -1 };
    }

    let mask_pool = moc.section::<i32>(80);
    let masks = mask_pool.as_ptr();
    let uvs = moc.section::<f32>(78).as_ptr();
    let indices = moc.section::<u16>(79).as_ptr();
    let drawable_ids = moc.section::<[u8; 64]>(33);
    let flags = moc.section::<u8>(42);
    let blend_modes = moc.section::<i32>(153);
    let vertex_counts = moc.section::<i32>(43);
    let uv_begins = moc.section::<i32>(44);
    let index_begins = moc.section::<i32>(45);
    let mask_begins = moc.section::<i32>(47);
    let mask_counts = moc.section::<i32>(48);
    let mut position = 0;
    for drawable in 0..drawable_ids.len() {
        state.drawable_ids[drawable] = drawable_ids[drawable].as_ptr();
        state.drawable_masks[drawable] = masks.wrapping_add(mask_begins[drawable].max(0) as usize);
        state.drawable_pinned[drawable] = (mask_counts[drawable] > 0) as u8;
        state.drawable_uvs[drawable] = uvs.wrapping_add(uv_begins[drawable].max(0) as usize);
        state.drawable_indices[drawable] = indices.wrapping_add(index_begins[drawable].max(0) as usize);
        state.drawable_position_begin[drawable] = position as i32;
        state.drawable_positions[drawable] = state.positions[position..].as_ptr();
        position += (vertex_counts[drawable] as usize * 2).next_multiple_of(4);
        state.drawable_blend_modes[drawable] = if moc.version >= 6 {
            blend_modes[drawable]
        } else if flags[drawable] & 1 != 0 {
            1
        } else if flags[drawable] & 2 != 0 {
            2
        } else {
            0
        };
    }
    let multiply = if moc.version >= 4 { 0.0 } else { 1.0 };
    for color in state.drawable_multiply.as_chunks_mut::<4>().0 {
        *color = [multiply, multiply, multiply, 1.0];
    }
    for color in state.drawable_screen.as_chunks_mut::<4>().0 {
        color[3] = 1.0;
    }
    for color in state.offscreen_multiply.as_chunks_mut::<4>().0 {
        *color = [1.0; 4];
    }
    for color in state.offscreen_screen.as_chunks_mut::<4>().0 {
        color[3] = 1.0;
    }

    let offscreen_mask_begins = moc.section::<i32>(158);
    for (mask, &begin) in state.offscreen_masks.iter_mut().zip(offscreen_mask_begins) {
        *mask = masks.wrapping_add(begin.max(0) as usize);
    }
    let drawable_masks = mask_begins.iter().zip(mask_counts);
    let offscreen_masks = offscreen_mask_begins.iter().zip(moc.section::<i32>(159));
    for (&begin, &count) in drawable_masks.chain(offscreen_masks) {
        for &drawable in span(mask_pool, begin, count) {
            if let Some(pinned) = state.drawable_pinned.get_mut(drawable as usize) {
                *pinned = 1;
            }
        }
    }

    let mut grid = 0;
    for (begin, &points) in state.warp_grid_begin.iter_mut().zip(moc.section::<i32>(22)) {
        *begin = grid;
        grid += points * 2;
    }
}
