use crate::format::{self, Moc};
use crate::model;
#[cfg(target_arch = "aarch64")]
use core::arch::aarch64::{vdup_n_f32, vfmas_lane_f32};
use core::ffi::{c_char, c_int, c_uint, c_void};
use core::ptr::{null, null_mut};
use core::slice;

#[repr(C)]
pub struct Vector2 {
    pub x: f32,
    pub y: f32,
}

#[repr(C)]
pub struct Vector4 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub w: f32,
}

#[cfg_attr(target_vendor = "apple", link(name = "System"))]
#[cfg_attr(not(any(target_vendor = "apple", target_env = "msvc")), link(name = "m"))]
unsafe extern "C" {
    pub safe fn floorf(value: f32) -> f32;
    #[cfg(target_vendor = "apple")]
    pub safe fn __sincosf_stret(value: f32) -> Vector2;
    #[cfg(not(target_vendor = "apple"))]
    pub safe fn sinf(value: f32) -> f32;
    #[cfg(not(target_vendor = "apple"))]
    pub safe fn cosf(value: f32) -> f32;
    pub safe fn atan2f(y: f32, x: f32) -> f32;
}

#[cfg(target_arch = "aarch64")]
pub fn fmaf(value: f32, factor: f32, addend: f32) -> f32 {
    unsafe { vfmas_lane_f32::<0>(addend, value, vdup_n_f32(factor)) }
}

#[cfg(not(target_arch = "aarch64"))]
pub fn fmaf(value: f32, factor: f32, addend: f32) -> f32 {
    value * factor + addend
}

macro_rules! getters {
    ($($name:ident -> $output:ty = $failure:expr, |$header:pat_param, $state:pat_param| $value:expr;)*) => {
        $(
            #[unsafe(no_mangle)]
            pub unsafe extern "C" fn $name(model: *const c_void) -> $output {
                if model.is_null() {
                    return $failure;
                }
                let ($header, $state) = unsafe { model::open(model.cast_mut().cast()) };
                $value
            }
        )*
    };
}

unsafe fn revived(moc: *const c_void) -> Option<Moc> {
    if moc.is_null() || !(moc as usize).is_multiple_of(64) {
        return None;
    }
    let size = unsafe { moc.cast::<u32>().add(2).read() } as usize;
    format::parse(unsafe { slice::from_raw_parts(moc.cast::<u8>(), size) })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hc_moc_consistent(address: *mut c_void, size: c_uint) -> c_int {
    if address.is_null() {
        return 0;
    }
    format::parse(unsafe { slice::from_raw_parts(address.cast::<u8>(), size as usize) }).is_some() as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hc_revive_moc(address: *mut c_void, size: c_uint) -> *mut c_void {
    if address.is_null() {
        return null_mut();
    }
    let Some(moc) = format::parse(unsafe { slice::from_raw_parts(address.cast::<u8>(), size as usize) }) else {
        return null_mut();
    };
    let uv_pointer = unsafe { address.cast::<u8>().add(moc.offsets[78] as usize) }.cast::<f32>();
    for index in (1..moc.section::<f32>(78).len()).step_by(2) {
        unsafe { *uv_pointer.add(index) = 1.0 - *uv_pointer.add(index) };
    }
    let indices = unsafe { address.cast::<u8>().add(moc.offsets[79] as usize) }.cast::<u16>();
    for (&begin, &count) in moc.section::<i32>(45).iter().zip(moc.section::<i32>(46)) {
        for triangle in (0..count.max(0) as usize).step_by(3) {
            unsafe { indices.add(begin as usize + triangle).swap(indices.add(begin as usize + triangle + 2)) };
        }
    }
    let masks = unsafe { address.cast::<u8>().add(moc.offsets[80] as usize) }.cast::<i32>();
    let mask_counts = unsafe { address.cast::<u8>().add(moc.offsets[48] as usize) }.cast::<i32>();
    for (drawable, &begin) in moc.section::<i32>(47).iter().enumerate() {
        let mut kept = 0;
        for slot in 0..unsafe { *mask_counts.add(drawable) }.max(0) as usize {
            let mask = unsafe { *masks.add(begin as usize + slot) };
            if mask >= 0 {
                unsafe { *masks.add(begin as usize + kept) = mask };
                kept += 1;
            }
        }
        unsafe { *mask_counts.add(drawable) = kept as i32 };
    }
    unsafe { address.cast::<u32>().add(2).write(size) };
    address
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hc_model_size(moc: *const c_void) -> c_uint {
    match unsafe { revived(moc) } {
        Some(moc) => c_uint::try_from(model::layout(&moc).1).unwrap_or(0),
        None => 0,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hc_vertex_position_size(moc: *const c_void) -> c_uint {
    match unsafe { revived(moc) } {
        Some(moc) => c_uint::try_from(moc.position_floats * size_of::<f32>()).unwrap_or(0),
        None => 0,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hc_initialize_model(moc: *const c_void, address: *mut c_void, size: c_uint, positions: *mut f32) -> *mut c_void {
    if address.is_null() || !(address as usize).is_multiple_of(16) || positions.is_null() || !positions.is_aligned() {
        return null_mut();
    }
    let Some(moc) = (unsafe { revived(moc) }) else {
        return null_mut();
    };
    if (size as usize) < model::layout(&moc).1 {
        return null_mut();
    }
    unsafe { model::initialize(&moc, address.cast(), positions) };
    address
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hc_attach_vertex_positions(model: *mut c_void, first: *mut f32, second: *mut f32) {
    if model.is_null() || first.is_null() || second.is_null() || !first.is_aligned() || !second.is_aligned() {
        return;
    }
    let (header, mut state) = unsafe { model::open(model.cast()) };
    unsafe { model::attach(header, &mut state, first, second) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hc_release_vertex_slot(model: *mut c_void) {
    if model.is_null() {
        return;
    }
    let (header, _) = unsafe { model::open(model.cast()) };
    header.released = true;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hc_update_model(model: *mut c_void) {
    if model.is_null() {
        return;
    }
    let (header, mut state) = unsafe { model::open(model.cast()) };
    model::update(header, &mut state);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hc_read_canvas(model: *const c_void, size: *mut Vector2, origin: *mut Vector2, pixels_per_unit: *mut f32) {
    if model.is_null() {
        return;
    }
    let (header, _) = unsafe { model::open(model.cast_mut().cast()) };
    let ([units, origin_x, origin_y, width, height], _) = header.moc.canvas();
    unsafe {
        if !size.is_null() {
            size.write(Vector2 { x: width, y: height });
        }
        if !origin.is_null() {
            origin.write(Vector2 { x: origin_x, y: origin_y });
        }
        if !pixels_per_unit.is_null() {
            pixels_per_unit.write(units);
        }
    }
}

getters! {
    hc_parameter_values -> *mut f32 = null_mut(), |_, state| state.parameter_values.as_mut_ptr();
    hc_part_opacities -> *mut f32 = null_mut(), |_, state| state.part_opacities.as_mut_ptr();
    hc_parameter_count -> c_int = -1, |header, _| header.moc.counts[5] as c_int;
    hc_parameter_ids -> *const *const c_char = null(), |_, state| state.parameter_ids.as_ptr().cast();
    hc_parameter_minimum_values -> *const f32 = null(), |header, _| header.moc.section::<f32>(52).as_ptr();
    hc_parameter_maximum_values -> *const f32 = null(), |header, _| header.moc.section::<f32>(51).as_ptr();
    hc_parameter_default_values -> *const f32 = null(), |header, _| header.moc.section::<f32>(53).as_ptr();
    hc_part_count -> c_int = -1, |header, _| header.moc.counts[0] as c_int;
    hc_part_ids -> *const *const c_char = null(), |_, state| state.part_ids.as_ptr().cast();
    hc_part_parent_indices -> *const c_int = null(), |header, _| header.moc.section::<i32>(9).as_ptr();
    hc_drawable_count -> c_int = -1, |header, _| header.moc.counts[4] as c_int;
    hc_drawable_constant_flags -> *const u8 = null(), |header, _| header.moc.section::<u8>(42).as_ptr();
    hc_drawable_dynamic_flags -> *const u8 = null(), |_, state| state.dynamic_flags.as_ptr();
    hc_drawable_blend_modes -> *const c_int = null(), |_, state| state.drawable_blend_modes.as_ptr();
    hc_drawable_texture_indices -> *const c_int = null(), |header, _| header.moc.section::<i32>(41).as_ptr();
    hc_drawable_opacities -> *const f32 = null(), |_, state| state.drawable_opacities.as_ptr();
    hc_drawable_mask_counts -> *const c_int = null(), |header, _| header.moc.section::<i32>(48).as_ptr();
    hc_drawable_masks -> *const *const c_int = null(), |_, state| state.drawable_masks.as_ptr();
    hc_drawable_vertex_counts -> *const c_int = null(), |header, _| header.moc.section::<i32>(43).as_ptr();
    hc_drawable_vertex_positions -> *const *const Vector2 = null(), |_, state| state.drawable_positions.as_ptr().cast();
    hc_vertex_slot -> c_int = -1, |header, _| header.slot as c_int;
    hc_drawable_vertex_uvs -> *const *const Vector2 = null(), |_, state| state.drawable_uvs.as_ptr().cast();
    hc_drawable_index_counts -> *const c_int = null(), |header, _| header.moc.section::<i32>(46).as_ptr();
    hc_drawable_indices -> *const *const u16 = null(), |_, state| state.drawable_indices.as_ptr();
    hc_drawable_multiply_colors -> *const Vector4 = null(), |_, state| state.drawable_multiply.as_ptr().cast();
    hc_drawable_screen_colors -> *const Vector4 = null(), |_, state| state.drawable_screen.as_ptr().cast();
    hc_drawable_parent_part_indices -> *const c_int = null(), |header, _| header.moc.section::<i32>(39).as_ptr();
    hc_render_orders -> *const c_int = null(), |_, state| state.render_orders.as_ptr();
    hc_offscreen_count -> c_int = -1, |header, _| header.moc.counts[35] as c_int;
}

macro_rules! offscreen_getters {
    ($($name:ident -> $output:ty, |$header:pat_param, $state:pat_param| $value:expr;)*) => {
        $(
            #[unsafe(no_mangle)]
            pub unsafe extern "C" fn $name(model: *const c_void) -> $output {
                if model.is_null() {
                    return null();
                }
                let (header, $state) = unsafe { model::open(model.cast_mut().cast()) };
                if header.moc.version < 6 {
                    return null();
                }
                let $header = header;
                $value
            }
        )*
    };
}

offscreen_getters! {
    hc_offscreen_owner_indices -> *const c_int, |header, _| header.moc.section::<i32>(155).as_ptr();
    hc_offscreen_blend_modes -> *const c_int, |header, _| header.moc.section::<i32>(157).as_ptr();
    hc_offscreen_constant_flags -> *const u8, |header, _| header.moc.section::<u8>(156).as_ptr();
    hc_offscreen_mask_counts -> *const c_int, |header, _| header.moc.section::<i32>(159).as_ptr();
    hc_offscreen_masks -> *const *const c_int, |_, state| state.offscreen_masks.as_ptr();
    hc_offscreen_opacities -> *const f32, |_, state| state.offscreen_opacities.as_ptr();
    hc_offscreen_multiply_colors -> *const Vector4, |_, state| state.offscreen_multiply.as_ptr().cast();
    hc_offscreen_screen_colors -> *const Vector4, |_, state| state.offscreen_screen.as_ptr().cast();
}
