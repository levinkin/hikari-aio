#pragma once

#include <simd/simd.h>

enum hfw_buffer_slot {
    hfw_slot_positions = 0,
    hfw_slot_uvs = 1,
    hfw_slot_uniforms = 2,
};

struct hfw_uniforms {
    matrix_float4x4 matrix;
    matrix_float4x4 clip_matrix;
    vector_float4 channel;
    vector_float4 base;
    vector_float4 multiply;
    vector_float4 screen;
};
