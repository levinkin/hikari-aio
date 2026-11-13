use crate::format::Moc;
#[cfg(target_vendor = "apple")]
use crate::ffi::{Vector2, __sincosf_stret};
#[cfg(not(target_vendor = "apple"))]
use crate::ffi::{cosf, sinf};
use crate::ffi::{atan2f, fmaf};
use crate::model::State;
use core::f32::consts::PI;

enum Transform {
    Warp { begin: usize, rows: usize, columns: usize, quad: bool },
    Rotation { matrix: [f32; 4], origin: [f32; 2] },
    Identity,
}

impl Transform {
    fn new(moc: &Moc, state: &State, deformer: usize) -> Transform {
        let target = moc.section::<i32>(18)[deformer] as usize;
        match moc.section::<i32>(17)[deformer] {
            0 => Transform::Warp {
                begin: state.warp_grid_begin[target] as usize,
                rows: moc.section::<i32>(23)[target] as usize,
                columns: moc.section::<i32>(24)[target] as usize,
                quad: matches!(moc.section::<i32>(101).get(target), Some(&quad) if quad != 0),
            },
            1 => Transform::Rotation {
                matrix: state.rotation_matrix.as_chunks::<4>().0[target],
                origin: [state.rotation_origin_x[target], state.rotation_origin_y[target]],
            },
            _ => Transform::Identity,
        }
    }

    #[inline(always)]
    fn apply(&self, grids: &[f32], point: [f32; 2]) -> [f32; 2] {
        match *self {
            Transform::Warp { begin, rows, columns, quad } => warp(&grids[begin..], rows, columns, quad, point),
            Transform::Rotation { matrix, origin } => [
                origin[0] + fmaf(matrix[0], point[0], matrix[1] * point[1]),
                origin[1] + fmaf(matrix[2], point[0], matrix[3] * point[1]),
            ],
            Transform::Identity => point,
        }
    }
}

#[inline(always)]
fn warp(grid: &[f32], rows: usize, columns: usize, quad: bool, point: [f32; 2]) -> [f32; 2] {
    let [x, y] = point;
    if !((0.0..1.0).contains(&x) && (0.0..1.0).contains(&y)) {
        return extrapolate(grid, rows, columns, point);
    }
    let stride = (columns + 1) * 2;
    let scaled_x = x * columns as f32;
    let scaled_y = y * rows as f32;
    let row = (scaled_y as usize).min(rows - 1);
    let column = (scaled_x as usize).min(columns - 1);
    let fraction_x = scaled_x - column as f32;
    let fraction_y = scaled_y - row as f32;
    let index = row * stride + column * 2;
    let upper = &grid[index..index + 4];
    let lower = &grid[index + stride..index + stride + 4];
    let mut coord = [0.0; 2];
    for lane in 0..2 {
        let corner = upper[lane];
        let right = upper[2 + lane];
        let below = lower[lane];
        let diagonal = lower[2 + lane];
        coord[lane] = if quad {
            let rest_x = 1.0 - fraction_x;
            let rest_y = 1.0 - fraction_y;
            let top = fmaf(rest_x * corner, rest_y, rest_y * (fraction_x * right));
            let bottom = fmaf(rest_x * below, fraction_y, top);
            fmaf(fraction_x * diagonal, fraction_y, bottom)
        } else if fraction_x + fraction_y <= 1.0 {
            fmaf(below, fraction_y, fmaf(corner, (1.0 - fraction_x) - fraction_y, fraction_x * right))
        } else {
            fmaf(right, 1.0 - fraction_y, fmaf(diagonal, (fraction_x - 1.0) + fraction_y, (1.0 - fraction_x) * below))
        };
    }
    coord
}

#[inline(never)]
fn extrapolate(grid: &[f32], rows: usize, columns: usize, point: [f32; 2]) -> [f32; 2] {
    let [x, y] = point;
    let stride = (columns + 1) * 2;
    let scaled_x = x * columns as f32;
    let scaled_y = y * rows as f32;
    let row = (scaled_y as usize).min(rows - 1);
    let column = (scaled_x as usize).min(columns - 1);
    let mut coord = [0.0; 2];
    let beyond = !(-2.0 < x && x < 3.0 && -2.0 < y && y < 3.0);
    let horizontal = if x >= 1.0 { 2 } else if x <= 0.0 { 0 } else { 1 };
    let vertical = if y >= 1.0 { 2 } else if y <= 0.0 { 0 } else { 1 };
    let row_start = row as f32 / rows as f32;
    let row_end = (row + 1) as f32 / rows as f32;
    let column_start = column as f32 / columns as f32;
    let column_end = (column + 1) as f32 / columns as f32;
    let weight_x = match horizontal {
        2 => (x - 1.0) * 0.5,
        0 => (x + 2.0) * 0.5,
        _ => scaled_x - column as f32,
    };
    let weight_y = match vertical {
        2 => (y - 1.0) * 0.5,
        0 => (y + 2.0) * 0.5,
        _ => scaled_y - row as f32,
    };
    let last_row = rows * stride;
    for lane in 0..2 {
        let first = grid[lane];
        let first_end = grid[columns * 2 + lane];
        let last = grid[last_row + lane];
        let last_end = grid[last_row + columns * 2 + lane];
        let center = (((first + first_end) + last) + last_end) * 0.25;
        let rising = first_end - last;
        let falling = last_end - first;
        let edge_x = (rising + falling) * 0.5;
        let edge_y = (falling - rising) * 0.5;
        let offset = fmaf(-0.5f32, falling, center);
        if beyond {
            coord[lane] = fmaf(edge_y, y, fmaf(x, edge_x, offset));
            continue;
        }
        let far = fmaf(3.0f32, edge_x, offset);
        let near = fmaf(-2.0f32, edge_x, offset);
        let (start, along_u, along_v, opposite) = match (horizontal, vertical) {
            (2, 2) => (last_end, edge_y + far, fmaf(3.0f32, edge_y, edge_x + offset), fmaf(3.0f32, edge_y, far)),
            (2, 0) => (fmaf(-2.0f32, edge_y, edge_x + offset), fmaf(-2.0f32, edge_y, far), first_end, far),
            (2, _) => (
                grid[row * stride + columns * 2 + lane],
                fmaf(row_start, edge_y, far),
                grid[(row + 1) * stride + columns * 2 + lane],
                fmaf(row_end, edge_y, far),
            ),
            (0, 2) => (edge_y + near, last, fmaf(3.0f32, edge_y, near), fmaf(3.0f32, edge_y, offset)),
            (0, 0) => (fmaf(-2.0f32, edge_y, near), fmaf(-2.0f32, edge_y, offset), near, first),
            (0, _) => (fmaf(row_start, edge_y, near), grid[row * stride + lane], fmaf(row_end, edge_y, near), grid[(row + 1) * stride + lane]),
            (_, 2) => (
                grid[last_row + column * 2 + lane],
                grid[last_row + column * 2 + 2 + lane],
                fmaf(3.0f32, edge_y, fmaf(column_start, edge_x, offset)),
                fmaf(3.0f32, edge_y, fmaf(column_end, edge_x, offset)),
            ),
            _ => (
                fmaf(-2.0f32, edge_y, fmaf(column_start, edge_x, offset)),
                fmaf(-2.0f32, edge_y, fmaf(column_end, edge_x, offset)),
                grid[column * 2 + lane],
                grid[column * 2 + 2 + lane],
            ),
        };
        coord[lane] = if weight_x + weight_y <= 1.0 {
            fmaf(along_v - start, weight_y, fmaf(along_u - start, weight_x, start))
        } else {
            fmaf(along_u - opposite, 1.0 - weight_y, fmaf(along_v - opposite, 1.0 - weight_x, opposite))
        };
    }
    coord
}

pub fn deform(moc: &Moc, state: &mut State) {
    let parents = moc.section::<i32>(16);
    let types = moc.section::<i32>(17);
    let targets = moc.section::<i32>(18);
    let grid_points = moc.section::<i32>(22);
    for deformer in 0..parents.len() {
        if state.deformer_active[deformer] == 0 || state.deformer_dirty[deformer] == 0 {
            continue;
        }
        let target = targets[deformer] as usize;
        let colors = target * 3..target * 3 + 3;
        let mut multiply = [0.0f32; 3];
        let mut screen = [0.0f32; 3];
        let (mut opacity, mut scale) = match types[deformer] {
            0 => {
                multiply.copy_from_slice(&state.warp_multiply[colors.clone()]);
                screen.copy_from_slice(&state.warp_screen[colors]);
                (state.warp_opacities[target], 1.0)
            }
            1 => {
                multiply.copy_from_slice(&state.rotation_multiply[colors.clone()]);
                screen.copy_from_slice(&state.rotation_screen[colors]);
                (state.rotation_opacities[target], state.rotation_scales[target])
            }
            _ => continue,
        };
        if parents[deformer] != -1 {
            let parent = parents[deformer] as usize;
            if state.deformer_stale[deformer] == 0 {
                let transform = Transform::new(moc, state, parent);
                if types[deformer] == 0 {
                    let begin = state.warp_grid_begin[target] as usize;
                    let end = begin + grid_points[target] as usize * 2;
                    match transform {
                        Transform::Warp { begin: source, rows, columns, quad } => {
                            let size = (rows + 1) * (columns + 1) * 2;
                            let (grid, points) = if source < begin {
                                let (head, tail) = state.warp_grids.split_at_mut(begin);
                                (&head[source..source + size], &mut tail[..end - begin])
                            } else {
                                let (head, tail) = state.warp_grids.split_at_mut(source);
                                (&tail[..size], &mut head[begin..end])
                            };
                            for point in points.as_chunks_mut::<2>().0 {
                                *point = warp(grid, rows, columns, quad, *point);
                            }
                        }
                        transform => {
                            for point in state.warp_grids[begin..end].as_chunks_mut::<2>().0 {
                                *point = transform.apply(&[], *point);
                            }
                        }
                    }
                    scale = state.deformer_scales[parent];
                } else {
                    let origin_x = state.rotation_origin_x[target];
                    let origin_y = state.rotation_origin_y[target];
                    let step = if types[parent] == 1 { -10.0 } else { -0.1 };
                    let origin = transform.apply(state.warp_grids, [origin_x, origin_y]);
                    let mut direction = [0.0; 2];
                    let mut distance: f32 = 1.0;
                    for _ in 0..10 {
                        let ahead = transform.apply(state.warp_grids, [fmaf(distance, 0.0, origin_x), fmaf(distance, step, origin_y)]);
                        let delta = [ahead[0] - origin[0], ahead[1] - origin[1]];
                        if delta[0] != 0.0 || delta[1] != 0.0 {
                            direction = delta;
                            break;
                        }
                        let behind = transform.apply(state.warp_grids, [fmaf(-distance, 0.0, origin_x), fmaf(-distance, step, origin_y)]);
                        let delta = [behind[0] - origin[0], behind[1] - origin[1]];
                        if delta[0] != 0.0 || delta[1] != 0.0 {
                            direction = [-delta[0], -delta[1]];
                            break;
                        }
                        distance *= 0.1;
                    }
                    let mut turn = atan2f(step, 0.0) - atan2f(direction[1], direction[0]);
                    while turn < -PI {
                        turn += 2.0 * PI;
                    }
                    while turn > PI {
                        turn -= 2.0 * PI;
                    }
                    state.rotation_origin_x[target] = origin[0];
                    state.rotation_origin_y[target] = origin[1];
                    state.rotation_angles[target] += (turn * -180.0) / PI;
                    scale *= state.deformer_scales[parent];
                    state.rotation_scales[target] = scale;
                }
            }
            opacity *= state.deformer_opacities[parent];
            for channel in 0..3 {
                let parent_screen = state.deformer_screen[parent * 3 + channel];
                multiply[channel] *= state.deformer_multiply[parent * 3 + channel];
                screen[channel] = fmaf(-screen[channel], parent_screen, screen[channel] + parent_screen);
            }
        }
        if types[deformer] == 1 {
            let angle = ((moc.section::<f32>(28)[target] + state.rotation_angles[target]) * PI) / 180.0;
            #[cfg(target_vendor = "apple")]
            let Vector2 { x: sin, y: cos } = __sincosf_stret(angle);
            #[cfg(not(target_vendor = "apple"))]
            let (sin, cos) = (sinf(angle), cosf(angle));
            let reflect_x = if state.rotation_reflect_x[target] != 0 { -1.0 } else { 1.0 };
            let reflect_y = if state.rotation_reflect_y[target] != 0 { -1.0 } else { 1.0 };
            state.rotation_matrix.as_chunks_mut::<4>().0[target] = [scale * cos * reflect_x, -(sin * scale) * reflect_y, scale * sin * reflect_x, scale * cos * reflect_y];
        }
        state.deformer_opacities[deformer] = opacity;
        state.deformer_scales[deformer] = scale;
        state.deformer_multiply[deformer * 3..deformer * 3 + 3].copy_from_slice(&multiply);
        state.deformer_screen[deformer * 3..deformer * 3 + 3].copy_from_slice(&screen);
    }

    let drawable_parents = moc.section::<i32>(40);
    let vertex_counts = moc.section::<i32>(43);
    for drawable in 0..drawable_parents.len() {
        let parent = drawable_parents[drawable];
        if parent == -1 || state.drawable_active[drawable] == 0 {
            continue;
        }
        state.drawable_opacities[drawable] *= state.deformer_opacities[parent as usize];
        if state.drawable_dirty[drawable] == 0 {
            continue;
        }
        let transform = Transform::new(moc, state, parent as usize);
        let begin = state.drawable_position_begin[drawable] as usize;
        let vertices = state.positions[begin..begin + vertex_counts[drawable] as usize * 2].as_chunks_mut::<2>().0;
        match transform {
            Transform::Warp { begin, rows, columns, quad } => {
                let grid = &state.warp_grids[begin..begin + (rows + 1) * (columns + 1) * 2];
                for vertex in vertices {
                    *vertex = warp(grid, rows, columns, quad, *vertex);
                }
            }
            transform => {
                for vertex in vertices {
                    *vertex = transform.apply(state.warp_grids, *vertex);
                }
            }
        }
    }
}
