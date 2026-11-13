use crate::format::Moc;
use crate::{deformers, glue, ids, keyforms, offscreen, visuals};
use core::slice;

#[repr(C)]
pub struct Header {
    pub moc: Moc,
    pub force: bool,
    pub pending: bool,
    pub released: bool,
    pub slot: usize,
    pub slots: [*mut f32; 2],
    offsets: [usize; FIELDS],
}

macro_rules! model_state {
    ($moc:ident; $($name:ident: $kind:ty = $length:expr;)*) => {
        pub struct State<'m> {
            $(pub $name: &'m mut [$kind],)*
            pub positions: &'m mut [f32],
        }

        const FIELDS: usize = [$(stringify!($name)),*].len();

        pub fn layout($moc: &Moc) -> ([usize; FIELDS], usize) {
            let mut offsets = [0; FIELDS];
            let mut cursor = size_of::<Header>().next_multiple_of(16);
            for (offset, bytes) in offsets.iter_mut().zip([$(($length) as usize * size_of::<$kind>()),*]) {
                *offset = cursor;
                cursor += bytes.next_multiple_of(16);
            }
            (offsets, cursor)
        }

        #[inline(always)]
        unsafe fn carve<'m>(header: &Header, base: *mut u8) -> State<'m> {
            let $moc = &header.moc;
            let mut offsets = header.offsets.iter();
            State {
                $($name: unsafe { slice::from_raw_parts_mut(base.add(*offsets.next().unwrap()).cast::<$kind>(), ($length) as usize) },)*
                positions: unsafe { slice::from_raw_parts_mut(header.slots[header.slot], $moc.position_floats) },
            }
        }
    };
}

model_state! { moc;
    parameter_values: f32 = moc.counts[5];
    parameter_effective: f32 = moc.counts[5];
    parameter_changed: u8 = moc.counts[5];
    parameter_types: i32 = moc.counts[5];
    parameter_ids: *const u8 = moc.counts[5];
    binding_interval: i32 = moc.counts[13];
    binding_fraction: f32 = moc.counts[13];
    binding_flags: u8 = moc.counts[13];
    shape_binding_interval: i32 = moc.counts[25];
    shape_binding_fraction: f32 = moc.counts[25];
    shape_binding_flags: u8 = moc.counts[25];
    keyform_combination_begin: i32 = moc.counts[12];
    keyform_combination_count: i32 = moc.counts[12];
    keyform_flags: u8 = moc.counts[12];
    combination_indices: i32 = moc.combination_total;
    combination_weights: f32 = moc.combination_total;
    shape_kind: u8 = moc.counts[26];
    shape_first: i32 = moc.counts[26];
    shape_second: i32 = moc.counts[26];
    shape_first_weight: f32 = moc.counts[26];
    shape_second_weight: f32 = moc.counts[26];
    shape_weight: f32 = moc.counts[26];
    constraint_values: f32 = moc.counts[30];
    part_opacities: f32 = moc.counts[0];
    part_active: u8 = moc.counts[0];
    part_draw_orders: i32 = moc.counts[0];
    part_composed_opacities: f32 = moc.counts[0];
    part_offscreens: i32 = moc.counts[0];
    part_ids: *const u8 = moc.counts[0];
    deformer_active: u8 = moc.counts[1];
    deformer_opacities: f32 = moc.counts[1];
    deformer_scales: f32 = moc.counts[1];
    deformer_multiply: f32 = moc.counts[1] * 3;
    deformer_screen: f32 = moc.counts[1] * 3;
    warp_opacities: f32 = moc.counts[2];
    warp_grid_begin: i32 = moc.counts[2];
    warp_grids: f32 = moc.grid_floats;
    warp_multiply: f32 = moc.counts[2] * 3;
    warp_screen: f32 = moc.counts[2] * 3;
    rotation_opacities: f32 = moc.counts[3];
    rotation_angles: f32 = moc.counts[3];
    rotation_origin_x: f32 = moc.counts[3];
    rotation_origin_y: f32 = moc.counts[3];
    rotation_scales: f32 = moc.counts[3];
    rotation_reflect_x: u8 = moc.counts[3];
    rotation_reflect_y: u8 = moc.counts[3];
    rotation_multiply: f32 = moc.counts[3] * 3;
    rotation_screen: f32 = moc.counts[3] * 3;
    rotation_matrix: f32 = moc.counts[3] * 4;
    drawable_active: u8 = moc.counts[4];
    drawable_opacities: f32 = moc.counts[4];
    drawable_draw_orders: i32 = moc.counts[4];
    drawable_position_begin: i32 = moc.counts[4];
    drawable_multiply: f32 = moc.counts[4] * 4;
    drawable_screen: f32 = moc.counts[4] * 4;
    dynamic_flags: u8 = moc.counts[4];
    render_orders: i32 = moc.counts[4] + moc.counts[35];
    drawable_blend_modes: i32 = moc.counts[4];
    drawable_ids: *const u8 = moc.counts[4];
    drawable_masks: *const i32 = moc.counts[4];
    drawable_positions: *const f32 = moc.counts[4];
    drawable_uvs: *const f32 = moc.counts[4];
    drawable_indices: *const u16 = moc.counts[4];
    glue_weights: f32 = moc.counts[20];
    offscreen_opacities: f32 = moc.counts[35];
    offscreen_multiply: f32 = moc.counts[35] * 4;
    offscreen_screen: f32 = moc.counts[35] * 4;
    offscreen_masks: *const i32 = moc.counts[35];
    group_keys: i32 = moc.counts[19];
    group_order: i32 = moc.counts[19];

    offscreen_colors: f32 = moc.counts[35] * 6;

    group_bases: i32 = moc.counts[18];

    shape_dirty: u8 = moc.counts[26];
    warp_dirty: u8 = moc.counts[2];
    rotation_dirty: u8 = moc.counts[3];
    deformer_dirty: u8 = moc.counts[1];
    drawable_dirty: u8 = moc.counts[4];
    glue_dirty: u8 = moc.counts[20];

    drawable_pinned: u8 = moc.counts[4];
    drawable_stale: u8 = moc.counts[4];
    drawable_written: u8 = moc.counts[4];
    deformer_needed: u8 = moc.counts[1];
    deformer_stale: u8 = moc.counts[1];

    drawable_blended: f32 = moc.counts[4] * 6;
}

pub unsafe fn initialize(moc: &Moc, base: *mut u8, positions: *mut f32) {
    unsafe {
        let (offsets, size) = layout(moc);
        base.write_bytes(0, size);
        positions.write_bytes(0, moc.position_floats);
        base.cast::<Header>().write(Header { moc: *moc, force: true, pending: false, released: false, slot: 0, slots: [positions; 2], offsets });
        let (header, mut state) = open(base);
        ids::initialize(&header.moc, &mut state);
        update(header, &mut state);
    }
}

#[inline(always)]
pub unsafe fn open<'m>(base: *mut u8) -> (&'m mut Header, State<'m>) {
    let header = unsafe { &mut *base.cast::<Header>() };
    let state = unsafe { carve(header, base) };
    (header, state)
}

pub fn update(header: &mut Header, state: &mut State) {
    let moc = &header.moc;
    let force = header.force;
    keyforms::parameters(moc, state, force);
    glue::shape_weights(moc, state, force);
    keyforms::blend(moc, state, force);
    offscreen::blend(moc, state);
    glue::blend_shapes(moc, state);
    offscreen::blend_shapes(moc, state);
    visuals::compose_parts(moc, state);
    visuals::select(moc, state);
    advance(moc, header.released, header.slots, &mut header.slot, state);
    header.released = false;
    keyforms::positions(moc, state);
    glue::shape_positions(moc, state);
    deformers::deform(moc, state);
    visuals::compose(moc, state);
    glue::apply(moc, state);
    visuals::finish(moc, state, force, header.pending);
    offscreen::finish(moc, state);
    header.force = false;
    header.pending = !force;
}

pub unsafe fn attach(header: &mut Header, state: &mut State, first: *mut f32, second: *mut f32) {
    unsafe {
        first.copy_from_nonoverlapping(state.positions.as_ptr(), state.positions.len());
        second.copy_from_nonoverlapping(state.positions.as_ptr(), state.positions.len());
    }
    header.slots = [first, second];
    header.slot = 0;
    header.released = false;
    state.drawable_written.fill(0);
    point(state, first);
}

fn point(state: &mut State, base: *mut f32) {
    for (pointer, &begin) in state.drawable_positions.iter_mut().zip(state.drawable_position_begin.iter()) {
        *pointer = base.wrapping_add(begin as usize);
    }
}

fn advance(moc: &Moc, released: bool, slots: [*mut f32; 2], slot: &mut usize, state: &mut State) {
    if !released || slots[0] == slots[1] {
        for (written, &dirty) in state.drawable_written.iter_mut().zip(state.drawable_dirty.iter()) {
            *written |= dirty;
        }
        return;
    }
    let previous = slots[*slot];
    *slot ^= 1;
    let current = slots[*slot];
    state.positions = unsafe { slice::from_raw_parts_mut(current, state.positions.len()) };
    point(state, current);
    let vertex_counts = moc.section::<i32>(43);
    for drawable in 0..state.drawable_written.len() {
        let dirty = state.drawable_dirty[drawable];
        if state.drawable_written[drawable] != 0 && (state.drawable_active[drawable] == 0 || dirty == 0) {
            let begin = state.drawable_position_begin[drawable] as usize;
            unsafe { current.add(begin).copy_from_nonoverlapping(previous.add(begin), vertex_counts[drawable] as usize * 2) };
        }
        state.drawable_written[drawable] = dirty;
    }
}
