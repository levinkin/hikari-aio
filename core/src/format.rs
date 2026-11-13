use core::slice;

const ENTRIES: usize = 167;
const FAMILIES: usize = 37;
pub const COLOR_POOLS: [usize; 6] = [108, 109, 110, 111, 112, 113];

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    None,
    Slot,
    Id,
    I32,
    F32,
    U8,
    U16,
}

const fn kind_size(kind: Kind) -> usize {
    match kind {
        Kind::None => 0,
        Kind::Slot => 8,
        Kind::Id => 64,
        Kind::I32 | Kind::F32 => 4,
        Kind::U8 => 1,
        Kind::U16 => 2,
    }
}

const fn section(entry: usize) -> (usize, Kind) {
    match entry {
        2 => (0, Kind::Slot),
        3 => (0, Kind::Id),
        4..=9 | 152 => (0, Kind::I32),
        10 => (1, Kind::Slot),
        11 => (1, Kind::Id),
        12..=18 => (1, Kind::I32),
        19..=24 | 101 | 105 => (2, Kind::I32),
        25..=27 | 106 => (3, Kind::I32),
        28 => (3, Kind::F32),
        29..=32 => (4, Kind::Slot),
        33 => (4, Kind::Id),
        34..=41 | 43..=48 | 107 | 153 => (4, Kind::I32),
        42 => (4, Kind::U8),
        49 | 102 => (5, Kind::Slot),
        50 => (5, Kind::Id),
        51..=53 => (5, Kind::F32),
        54..=57 | 103 | 104 | 114..=116 => (5, Kind::I32),
        58 => (6, Kind::F32),
        160 => (6, Kind::I32),
        59 => (7, Kind::F32),
        60 | 137 | 138 => (7, Kind::I32),
        61..=65 => (8, Kind::F32),
        66 | 67 | 139 | 140 => (8, Kind::I32),
        68 | 69 => (9, Kind::F32),
        70 | 141 | 142 => (9, Kind::I32),
        71 => (10, Kind::F32),
        72 => (11, Kind::I32),
        73 | 74 => (12, Kind::I32),
        75 | 76 => (13, Kind::I32),
        77 => (14, Kind::F32),
        78 => (15, Kind::F32),
        79 => (16, Kind::U16),
        80 => (17, Kind::I32),
        81..=85 => (18, Kind::I32),
        86..=88 => (19, Kind::I32),
        89 => (20, Kind::Slot),
        90 => (20, Kind::Id),
        91..=97 => (20, Kind::I32),
        98 => (21, Kind::F32),
        99 => (21, Kind::U16),
        100 => (22, Kind::F32),
        108..=110 => (23, Kind::F32),
        111..=113 => (24, Kind::F32),
        117..=119 => (25, Kind::I32),
        120..=124 => (26, Kind::I32),
        125..=127 => (27, Kind::I32),
        128..=130 => (28, Kind::I32),
        131 => (29, Kind::I32),
        132..=134 => (30, Kind::I32),
        135 | 136 => (31, Kind::F32),
        143..=145 => (32, Kind::I32),
        146..=148 => (33, Kind::I32),
        149..=151 => (34, Kind::I32),
        154 => (35, Kind::Slot),
        155 | 157..=159 => (35, Kind::I32),
        156 => (35, Kind::U8),
        161 => (36, Kind::F32),
        162 | 163 => (36, Kind::I32),
        _ => (0, Kind::None),
    }
}

#[derive(Clone, Copy)]
pub struct Moc {
    base: *const u8,
    pub size: usize,
    pub version: u8,
    pub counts: [u32; FAMILIES],
    entry_count: usize,
    pub offsets: [u32; ENTRIES],
    pub combination_total: usize,
    pub position_floats: usize,
    pub grid_floats: usize,
}

impl Moc {
    pub fn section<T>(&self, entry: usize) -> &[T] {
        let (family, kind) = section(entry);
        assert!(size_of::<T>() == kind_size(kind));
        let count = self.counts[family] as usize;
        if entry >= self.entry_count || count == 0 {
            return &[];
        }
        unsafe { slice::from_raw_parts(self.base.add(self.offsets[entry] as usize).cast::<T>(), count) }
    }

    pub fn canvas(&self) -> ([f32; 5], u8) {
        let start = self.offsets[1] as usize;
        let bytes = unsafe { slice::from_raw_parts(self.base.add(start), 21) };
        let mut values = [0.0; 5];
        for (index, value) in values.iter_mut().enumerate() {
            *value = f32::from_le_bytes(bytes[index * 4..index * 4 + 4].try_into().unwrap());
        }
        (values, bytes[20])
    }
}

pub fn read_version(bytes: &[u8]) -> u8 {
    if bytes.len() < 5 || &bytes[..4] != b"MOC3" {
        return 0;
    }
    bytes[4]
}

pub fn parse(bytes: &[u8]) -> Option<Moc> {
    let size = bytes.len();
    let version = read_version(bytes);
    if size < 64 || !(bytes.as_ptr() as usize).is_multiple_of(64) || !(1..=6).contains(&version) || bytes[5] != 0 {
        return None;
    }
    let (entry_count, minimum) = match version {
        1 => (101, 2112),
        2 | 3 => (102, 2112),
        4 => (137, 2112),
        5 => (152, 2112),
        _ => (167, 3392),
    };
    if size < minimum {
        return None;
    }
    let mut offsets = [0u32; ENTRIES];
    let mut previous = 0;
    for (entry, offset) in offsets.iter_mut().enumerate().take(entry_count) {
        let start = 64 + entry * 4;
        *offset = u32::from_le_bytes(bytes[start..start + 4].try_into().unwrap());
        if *offset < previous || *offset as usize > size {
            return None;
        }
        previous = *offset;
    }
    let table = offsets[0] as usize;
    let table_bytes = if version >= 5 { 256 } else { 128 };
    if !table.is_multiple_of(4) || table + table_bytes > size || !offsets[1].is_multiple_of(4) || offsets[1] as usize + 21 > size {
        return None;
    }
    let mut counts = [0u32; FAMILIES];
    for (family, count) in counts.iter_mut().enumerate() {
        let gate = match family {
            23..=31 => 4,
            32..=34 => 5,
            35 | 36 => 6,
            _ => 1,
        };
        if version < gate {
            continue;
        }
        let start = table + family * 4;
        *count = u32::from_le_bytes(bytes[start..start + 4].try_into().unwrap());
        if *count > i32::MAX as u32 {
            return None;
        }
    }
    for (entry, offset) in offsets.iter().enumerate().take(entry_count).skip(2) {
        let (family, kind) = section(entry);
        let element = kind_size(kind);
        let length = counts[family] as u64 * element as u64;
        if kind == Kind::None || length == 0 {
            continue;
        }
        if *offset as u64 + length > size as u64 || !(*offset as usize).is_multiple_of(element.min(4)) {
            return None;
        }
        if kind == Kind::Id {
            for record in bytes[*offset as usize..*offset as usize + length as usize].as_chunks::<64>().0 {
                if !record.contains(&0) {
                    return None;
                }
            }
        }
    }
    let mut moc = Moc {
        base: bytes.as_ptr(),
        size,
        version,
        counts,
        entry_count,
        offsets,
        combination_total: 0,
        position_floats: 0,
        grid_floats: 0,
    };
    if !validate(&mut moc) {
        return None;
    }
    Some(moc)
}

#[inline(never)]
fn indices_within(values: &[i32], limit: u32, allow_none: bool) -> bool {
    let shift = allow_none as i32;
    let bound = limit + allow_none as u32;
    values.iter().fold(true, |within, &value| within & ((value.wrapping_add(shift) as u32) < bound))
}

pub fn span<T>(array: &[T], begin: i32, count: i32) -> &[T] {
    if count == 0 {
        return &[];
    }
    &array[begin as usize..begin as usize + count as usize]
}

#[inline(never)]
fn ranges_within(begins: &[i32], counts: &[i32], total: u32) -> bool {
    begins.iter().zip(counts).fold(true, |within, (&begin, &count)| {
        let inside = (begin | count) >= 0 && begin as i64 + count as i64 <= total as i64;
        within & (inside | (count == 0 && begin == -1))
    })
}

fn keyforms_cover(moc: &Moc, bindings: &[i32], keyform_counts: &[i32]) -> bool {
    let lists_begin = moc.section::<i32>(73);
    let lists_count = moc.section::<i32>(74);
    let list = moc.section::<i32>(72);
    let key_counts = moc.section::<i32>(76);
    for (&binding, &keyform_count) in bindings.iter().zip(keyform_counts) {
        let mut product: i64 = 1;
        for &parameter_binding in span(list, lists_begin[binding as usize], lists_count[binding as usize]) {
            product = product.saturating_mul(key_counts[parameter_binding as usize].max(1) as i64);
        }
        if product > keyform_count as i64 {
            return false;
        }
    }
    true
}

#[inline(never)]
fn positions_within(moc: &Moc, position_begins: &[i32], begin: i32, count: i32, points: i32) -> bool {
    let room = moc.counts[10] as i64 - points as i64 * 2;
    span(position_begins, begin, count).iter().fold(true, |within, &position| within & (position >= 0) & (position as i64 <= room))
}

fn shapes_within(moc: &Moc, targets: &[i32], target_limit: u32, entries: [usize; 2], keyform_limit: u32, points: Option<(&[i32], &[i32])>) -> bool {
    let record_begins = moc.section::<i32>(entries[0]);
    let record_counts = moc.section::<i32>(entries[1]);
    if !indices_within(targets, target_limit, false) || !ranges_within(record_begins, record_counts, moc.counts[26]) {
        return false;
    }
    let shape_bindings = moc.section::<i32>(120);
    let keyform_begins = moc.section::<i32>(121);
    let keyform_counts = moc.section::<i32>(122);
    let key_counts = moc.section::<i32>(118);
    for record in 0..targets.len() {
        let begin = record_begins[record] as usize;
        for entry in begin..begin + record_counts[record] as usize {
            let keyform_begin = keyform_begins[entry];
            let keyform_count = keyform_counts[entry];
            if !ranges_within(&[keyform_begin], &[keyform_count], keyform_limit) || keyform_count < key_counts[shape_bindings[entry] as usize] {
                return false;
            }
            if let Some((position_begins, point_counts)) = points
                && !positions_within(moc, position_begins, keyform_begin, keyform_count, point_counts[targets[record] as usize])
            {
                return false;
            }
        }
    }
    true
}

fn validate(moc: &mut Moc) -> bool {
    let counts = moc.counts;
    let parts = counts[0];
    let deformers = counts[1];
    let warps = counts[2];
    let rotations = counts[3];
    let meshes = counts[4];
    let parameters = counts[5];
    let bindings = counts[12];
    let glues = counts[20];
    let offscreens = counts[35];
    let groups = counts[18];

    let warp_rows = moc.section::<i32>(23);
    let warp_columns = moc.section::<i32>(24);
    let grid_points = moc.section::<i32>(22);
    let vertex_counts = moc.section::<i32>(43);
    for warp in 0..warps as usize {
        let rows = warp_rows[warp] as i64;
        let columns = warp_columns[warp] as i64;
        if rows < 1 || columns < 1 || (rows + 1) * (columns + 1) != grid_points[warp] as i64 {
            return false;
        }
    }
    if !indices_within(vertex_counts, u16::MAX as u32 + 1, false) {
        return false;
    }

    let checks = [
        indices_within(moc.section(4), bindings, false),
        ranges_within(moc.section(5), moc.section(6), counts[6]),
        indices_within(moc.section(9), parts, true),
        indices_within(moc.section(152), offscreens, true),
        indices_within(moc.section(12), bindings, false),
        indices_within(moc.section(15), parts, true),
        indices_within(moc.section(16), deformers, true),
        indices_within(moc.section(19), bindings, false),
        ranges_within(moc.section(20), moc.section(21), counts[7]),
        indices_within(moc.section(25), bindings, false),
        ranges_within(moc.section(26), moc.section(27), counts[8]),
        indices_within(moc.section(34), bindings, false),
        ranges_within(moc.section(35), moc.section(36), counts[9]),
        indices_within(moc.section(39), parts, true),
        indices_within(moc.section(40), deformers, true),
        ranges_within(moc.section(45), moc.section(46), counts[16]),
        ranges_within(moc.section(47), moc.section(48), counts[17]),
        indices_within(moc.section(80), meshes, true),
        ranges_within(moc.section(56), moc.section(57), counts[13]),
        ranges_within(moc.section(103), moc.section(104), counts[14]),
        ranges_within(moc.section(115), moc.section(116), counts[25]),
        ranges_within(moc.section(75), moc.section(76), counts[14]),
        ranges_within(moc.section(73), moc.section(74), counts[11]),
        indices_within(moc.section(72), counts[13], false),
        ranges_within(moc.section(117), moc.section(118), counts[14]),
        indices_within(moc.section(120), counts[25], false),
        ranges_within(moc.section(123), moc.section(124), counts[29]),
        indices_within(moc.section(131), counts[30], false),
        indices_within(moc.section(132), parameters, true),
        ranges_within(moc.section(133), moc.section(134), counts[31]),
        indices_within(moc.section(91), bindings, false),
        ranges_within(moc.section(92), moc.section(93), counts[22]),
        indices_within(moc.section(94), meshes, false),
        indices_within(moc.section(95), meshes, false),
        ranges_within(moc.section(96), moc.section(97), counts[21]),
        indices_within(moc.section(137), counts[23], false),
        indices_within(moc.section(139), counts[23], false),
        indices_within(moc.section(141), counts[23], false),
        indices_within(moc.section(138), counts[24], false),
        indices_within(moc.section(140), counts[24], false),
        indices_within(moc.section(142), counts[24], false),
        indices_within(moc.section(155), parts, false),
        ranges_within(moc.section(158), moc.section(159), counts[17]),
        indices_within(moc.section(160), counts[36], true),
        indices_within(moc.section(162), counts[23], false),
        indices_within(moc.section(163), counts[24], false),
        ranges_within(moc.section(81), moc.section(82), counts[19]),
    ];
    if checks.contains(&false) {
        return false;
    }

    let deformer_types = moc.section::<i32>(17);
    let deformer_targets = moc.section::<i32>(18);
    for deformer in 0..deformers as usize {
        let limit = match deformer_types[deformer] {
            0 => warps,
            1 => rotations,
            _ => continue,
        };
        if !indices_within(&deformer_targets[deformer..deformer + 1], limit, false) {
            return false;
        }
    }

    let families = [(4, 6), (19, 21), (25, 27), (34, 36), (91, 93)];
    for (binding_entry, count_entry) in families {
        if !keyforms_cover(moc, moc.section(binding_entry), moc.section(count_entry)) {
            return false;
        }
    }

    let warp_begins = moc.section::<i32>(20);
    let warp_counts = moc.section::<i32>(21);
    let warp_positions = moc.section::<i32>(60);
    for warp in 0..warps as usize {
        if !positions_within(moc, warp_positions, warp_begins[warp], warp_counts[warp], grid_points[warp]) {
            return false;
        }
    }
    let mesh_begins = moc.section::<i32>(35);
    let mesh_counts = moc.section::<i32>(36);
    let mesh_positions = moc.section::<i32>(70);
    let uv_begins = moc.section::<i32>(44);
    let index_begins = moc.section::<i32>(45);
    let index_counts = moc.section::<i32>(46);
    let indices = moc.section::<u16>(79);
    for mesh in 0..meshes as usize {
        let vertices = vertex_counts[mesh];
        if !positions_within(moc, mesh_positions, mesh_begins[mesh], mesh_counts[mesh], vertices)
            || !ranges_within(&uv_begins[mesh..mesh + 1], &[vertices * 2], counts[15])
            || index_counts[mesh] % 3 != 0
        {
            return false;
        }
        if !span(indices, index_begins[mesh], index_counts[mesh]).iter().fold(true, |within, &index| within & ((index as i32) < vertices)) {
            return false;
        }
    }

    if moc.version >= 4 {
        let color_pool = counts[23].min(counts[24]);
        let color_begins = [(105, 21), (106, 27), (107, 36)];
        for (color_entry, count_entry) in color_begins {
            if !ranges_within(moc.section(color_entry), moc.section(count_entry), color_pool) {
                return false;
            }
        }
    }

    let shapes = [
        shapes_within(moc, moc.section(125), warps, [126, 127], counts[7], Some((warp_positions, grid_points))),
        shapes_within(moc, moc.section(128), meshes, [129, 130], counts[9], Some((mesh_positions, vertex_counts))),
        shapes_within(moc, moc.section(143), parts, [144, 145], counts[6], None),
        shapes_within(moc, moc.section(146), rotations, [147, 148], counts[8], None),
        shapes_within(moc, moc.section(149), glues, [150, 151], counts[22], None),
    ];
    if shapes.contains(&false) {
        return false;
    }

    let info_begins = moc.section::<i32>(96);
    let info_counts = moc.section::<i32>(97);
    let info_vertices = moc.section::<u16>(99);
    let glue_first = moc.section::<i32>(94);
    let glue_second = moc.section::<i32>(95);
    for glue in 0..glues as usize {
        if info_counts[glue] % 2 != 0 {
            return false;
        }
        let begin = info_begins[glue] as usize;
        for pair in 0..info_counts[glue] as usize / 2 {
            let first = (info_vertices[begin + pair * 2] & 0x7fff) as i32;
            let second = (info_vertices[begin + pair * 2 + 1] & 0x7fff) as i32;
            if first >= vertex_counts[glue_first[glue] as usize] || second >= vertex_counts[glue_second[glue] as usize] {
                return false;
            }
        }
    }

    let group_begins = moc.section::<i32>(81);
    let group_counts = moc.section::<i32>(82);
    let group_sizes = moc.section::<i32>(83);
    let object_types = moc.section::<i32>(86);
    let object_indices = moc.section::<i32>(87);
    let object_groups = moc.section::<i32>(88);
    let part_offscreens = moc.section::<i32>(152);
    if groups == 0 || group_sizes[0] < 0 || group_sizes[0] as i64 > meshes as i64 + offscreens as i64 {
        return false;
    }
    for group in 0..groups as usize {
        let begin = group_begins[group] as usize;
        let mut size: i64 = 0;
        for object in begin..begin + group_counts[group] as usize {
            match object_types[object] {
                0 if (object_indices[object] as u32) < meshes => size += 1,
                1 if (object_indices[object] as u32) < parts
                    && object_groups[object] as i64 > group as i64
                    && (object_groups[object] as u32) < groups =>
                {
                    let opened = object_groups[object] as usize;
                    let owns = match part_offscreens.get(object_indices[object] as usize) {
                        Some(&offscreen) => offscreen != -1,
                        None => false,
                    };
                    size += owns as i64 + group_sizes[opened] as i64;
                }
                _ => return false,
            }
        }
        if size != group_sizes[group] as i64 {
            return false;
        }
    }

    let mut combination_total: usize = 0;
    let lists_begin = moc.section::<i32>(73);
    let lists_count = moc.section::<i32>(74);
    let list = moc.section::<i32>(72);
    let key_counts = moc.section::<i32>(76);
    for binding in 0..bindings as usize {
        let mut varying = 0;
        for &parameter_binding in span(list, lists_begin[binding], lists_count[binding]) {
            varying += (key_counts[parameter_binding as usize] >= 2) as u32;
        }
        if varying > 20 {
            return false;
        }
        combination_total += 1 << varying;
    }
    if combination_total > moc.size {
        return false;
    }
    let mut position_floats = 0;
    for &vertices in vertex_counts {
        position_floats += (vertices as usize * 2).next_multiple_of(4);
    }
    let mut grid_floats = 0;
    for &points in grid_points {
        grid_floats += points as usize * 2;
    }
    moc.combination_total = combination_total;
    moc.position_floats = position_floats;
    moc.grid_floats = grid_floats;
    true
}
