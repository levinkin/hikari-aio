#include <metal_stdlib>
using namespace metal;

struct hfw_uniforms {
    float4x4 matrix;
    float4x4 clip_matrix;
    float4 channel;
    float4 base;
    float4 multiply;
    float4 screen;
};

struct hfw_plain_out {
    float4 position [[position]];
    float2 uv;
};

struct hfw_masked_out {
    float4 position [[position]];
    float2 uv;
    float4 clip;
};

vertex hfw_plain_out hfw_vs_plain(uint index [[vertex_id]], constant float2* positions [[buffer(0)]], constant float2* texture_coordinates [[buffer(1)]], constant hfw_uniforms& uniforms [[buffer(2)]])
{
    hfw_plain_out vertex_output;
    vertex_output.position = uniforms.matrix * float4(positions[index], 0.0, 1.0);
    vertex_output.uv = float2(texture_coordinates[index].x, 1.0 - texture_coordinates[index].y);
    return vertex_output;
}

vertex hfw_masked_out hfw_vs_masked(uint index [[vertex_id]], constant float2* positions [[buffer(0)]], constant float2* texture_coordinates [[buffer(1)]], constant hfw_uniforms& uniforms [[buffer(2)]])
{
    float4 position = float4(positions[index], 0.0, 1.0);
    hfw_masked_out vertex_output;
    vertex_output.position = uniforms.matrix * position;
    vertex_output.uv = float2(texture_coordinates[index].x, 1.0 - texture_coordinates[index].y);
    vertex_output.clip = uniforms.clip_matrix * position;
    vertex_output.clip.y = 1.0 - vertex_output.clip.y;
    return vertex_output;
}

vertex hfw_masked_out hfw_vs_mask(uint index [[vertex_id]], constant float2* positions [[buffer(0)]], constant float2* texture_coordinates [[buffer(1)]], constant hfw_uniforms& uniforms [[buffer(2)]])
{
    float4 position = float4(positions[index], 0.0, 1.0);
    hfw_masked_out vertex_output;
    vertex_output.position = uniforms.clip_matrix * position;
    vertex_output.uv = float2(texture_coordinates[index].x, 1.0 - texture_coordinates[index].y);
    vertex_output.clip = uniforms.clip_matrix * position;
    return vertex_output;
}

static float4 hfw_tint(float4 texel, constant hfw_uniforms& uniforms)
{
    float4 color = texel;
    color.rgb = color.rgb * uniforms.multiply.rgb;
    color.rgb = (color.rgb + uniforms.screen.rgb * color.a) - (color.rgb * uniforms.screen.rgb);
    return color * uniforms.base;
}

static float hfw_coverage(hfw_masked_out fragment_input, texture2d<float> mask, sampler mask_sampler, constant hfw_uniforms& uniforms)
{
    float4 covered = (1.0 - mask.sample(mask_sampler, fragment_input.clip.xy / fragment_input.clip.w)) * uniforms.channel;
    return covered.r + covered.g + covered.b + covered.a;
}

fragment float4 hfw_fs_plain(hfw_plain_out fragment_input [[stage_in]], texture2d<float> texture [[texture(0)]], sampler texture_sampler [[sampler(0)]], constant hfw_uniforms& uniforms [[buffer(2)]])
{
    return hfw_tint(texture.sample(texture_sampler, fragment_input.uv), uniforms);
}

fragment float4 hfw_fs_masked(hfw_masked_out fragment_input [[stage_in]], texture2d<float> texture [[texture(0)]], texture2d<float> mask [[texture(1)]], sampler texture_sampler [[sampler(0)]], sampler mask_sampler [[sampler(1)]], constant hfw_uniforms& uniforms [[buffer(2)]])
{
    return hfw_tint(texture.sample(texture_sampler, fragment_input.uv), uniforms) * hfw_coverage(fragment_input, mask, mask_sampler, uniforms);
}

fragment float4 hfw_fs_masked_inverted(hfw_masked_out fragment_input [[stage_in]], texture2d<float> texture [[texture(0)]], texture2d<float> mask [[texture(1)]], sampler texture_sampler [[sampler(0)]], sampler mask_sampler [[sampler(1)]], constant hfw_uniforms& uniforms [[buffer(2)]])
{
    return hfw_tint(texture.sample(texture_sampler, fragment_input.uv), uniforms) * (1.0 - hfw_coverage(fragment_input, mask, mask_sampler, uniforms));
}

fragment float4 hfw_fs_mask(hfw_masked_out fragment_input [[stage_in]], texture2d<float> texture [[texture(0)]], sampler texture_sampler [[sampler(0)]], constant hfw_uniforms& uniforms [[buffer(2)]])
{
    float4 texel = texture.sample(texture_sampler, fragment_input.uv);
    float2 mask_coordinate = fragment_input.clip.xy / fragment_input.clip.w;
    float4 rectangle = uniforms.base;
    float inside = step(rectangle.x, mask_coordinate.x) * step(rectangle.y, mask_coordinate.y) * step(mask_coordinate.x, rectangle.z) * step(mask_coordinate.y, rectangle.w);
    return uniforms.channel * texel.a * inside;
}
