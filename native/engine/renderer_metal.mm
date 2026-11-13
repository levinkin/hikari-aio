#import <Metal/Metal.h>
#include <simd/simd.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include "hfw.h"
#include "shaders/types.h"
#include "mask_side.inc"

static const int kSamples = 4;

static const char under_shader[] = R"(
struct under_out
{
    float4 position [[position]];
};
vertex under_out under_vertex(uint id [[vertex_id]])
{
    under_out out;
    out.position = float4(float2((id << 1) & 2, id & 2) * 2.0 - 1.0, 0.0, 1.0);
    return out;
}
float3 cover(texture2d<float> back, float2 position, float4 size)
{
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    if (size.z <= 0.0)
    {
        return float3(0.0);
    }
    float2 back_size = float2(back.get_width(), back.get_height());
    float scale = max(size.x / back_size.x, size.y / back_size.y);
    return back.sample(linear, ((position - size.xy * 0.5) / scale + back_size * 0.5) / back_size).rgb;
}
fragment float4 under_fragment(under_out in [[stage_in]], texture2d<float> back [[texture(0)]], constant float4& size [[buffer(0)]])
{
    return float4(cover(back, in.position.xy, size), 1.0);
}
struct share_out
{
    float4 window [[color(0)]];
    float4 share [[color(1)]];
};
fragment share_out share_fragment(under_out in [[stage_in]], texture2d<float> model [[texture(0)]], texture2d<float> back [[texture(1)]], constant float4& size [[buffer(0)]])
{
    float4 color = model.read(uint2(in.position.xy));
    share_out out;
    out.window = float4(color.rgb + cover(back, in.position.xy, size) * (1.0 - color.a), 1.0);
    out.share = color.a > 0.0 ? float4(color.rgb / color.a, color.a) : float4(0.0);
    return out;
}
)";

static const char blend_functions[] = {
#embed "blend.metal"
    , 0};

static const char layer_shader[] = R"(
struct layer_uniforms
{
    float4x4 matrix;
    float4x4 clip_matrix;
    float4 channel;
    float4 base;
    float4 multiply;
    float4 screen;
    float4 tile;
};
struct layer_in
{
    float4 position [[position]];
    float2 uv;
    float4 clip;
};
struct layer_out
{
    float4 color [[color(TARGET)]];
};
vertex layer_in layer_vertex(uint id [[vertex_id]], constant float2* positions [[buffer(0)]], constant float2* uvs [[buffer(1)]], constant layer_uniforms& u [[buffer(2)]])
{
    layer_in out;
    float4 position = float4(positions[id], 0.0, 1.0);
    out.position = u.matrix * position;
    out.uv = float2(uvs[id].x, 1.0 - uvs[id].y);
    out.clip = u.clip_matrix * position;
    return out;
}
fragment layer_out clear_fragment()
{
    layer_out out;
    out.color = float4(0.0);
    return out;
}
fragment layer_out layer_fragment(layer_in in [[stage_in]], texture2d<float> texture [[texture(0)]], texture2d<float> mask [[texture(1)]], sampler texture_sampler [[sampler(0)]], sampler mask_sampler [[sampler(1)]], constant layer_uniforms& u [[buffer(2)]]
#if SOURCE > 0
    , float4 source [[color(SOURCE)]]
#endif
#if BLEND
    , float4 destination [[color(TARGET)]]
#endif
    )
{
#if SOURCE > 0
    float4 color = source;
#else
    float4 color = texture.sample(texture_sampler, in.uv);
#endif
    color.rgb = color.rgb * u.multiply.rgb;
    color.rgb = (color.rgb + u.screen.rgb * color.a) - (color.rgb * u.screen.rgb);
    color = color * u.base;
#if MASK > 0
    float2 at = in.clip.xy / in.clip.w;
    float4 covered = (1.0 - mask.sample(mask_sampler, float2(at.x, (u.tile.x + 1.0 - at.y) / u.tile.y))) * u.channel;
    float value = covered.r + covered.g + covered.b + covered.a;
    color = color * (MASK == 2 ? 1.0 - value : value);
#endif
    layer_out out;
#if BLEND
    float4 straight = hfw_to_straight(color);
    float4 behind = hfw_to_straight(destination);
    out.color = hfw_blend_alpha(hfw_blend_color(straight.rgb, behind.rgb), straight, behind);
#else
    out.color = color;
#endif
    return out;
}
)";

struct layer_uniforms
{
    hfw_uniforms shared;
    simd::float4 tile;
};

static const float quad[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
static const MTLBlendFactor fixed_blends[5][4] = {
    {MTLBlendFactorOne, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorOne, MTLBlendFactorOneMinusSourceAlpha},
    {MTLBlendFactorOne, MTLBlendFactorOne, MTLBlendFactorZero, MTLBlendFactorOne},
    {MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorZero, MTLBlendFactorOne},
    {MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorOne, MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorOne},
    {MTLBlendFactorZero, MTLBlendFactorOneMinusSourceColor, MTLBlendFactorZero, MTLBlendFactorOneMinusSourceAlpha},
};

struct metal_renderer
{
    hfw::model* model = NULL;
    std::vector<id<MTLTexture>> textures;
    id<MTLBuffer> moc = nil;
    id<MTLBuffer> positions = nil;
    id<MTLCommandBuffer> previous = nil;
    size_t slot_offset = 0;
    std::vector<size_t> vertex_offset;
    std::vector<size_t> uv_offset;
    std::vector<size_t> index_offset;
    std::vector<int> sorted;
    hfw::masks* masks = NULL;
    hfw::masks* offscreen_masks = NULL;
    std::vector<uint32_t> mask_tile;
    std::vector<uint32_t> offscreen_mask_tile;
    id<MTLTexture> mask_texture = nil;
    id<MTLTexture> msaa = nil;
    bool blend = false;
    bool tiled = false;
    uint32_t mask_tiles = 1;
    uint32_t mask_side = 0;
    uint32_t mask_updates = UINT32_MAX;
    int32_t mask_used[2] = {0, 0};
    uint32_t shrink_frames = 0;
    uint32_t shrink_side = 0;
    std::vector<id<MTLTexture>> layers;
    std::vector<int> offscreen_parent;
    std::vector<int> offscreen_layer;
    std::vector<int> drawable_owner;
    std::vector<id<MTLRenderPipelineState>> drawable_state;
    std::vector<id<MTLRenderPipelineState>> offscreen_state;
    std::vector<id<MTLRenderPipelineState>> offscreen_clear;
    id<MTLRenderPipelineState> under = nil;
    MTLRenderPassDescriptor* mask_pass = nil;
    MTLRenderPassDescriptor* pass = nil;
    int current = -1;
    bool view = false;
};

static id<MTLDevice> renderer_device = nil;
static id<MTLLibrary> renderer_library = nil;
static id<MTLRenderPipelineState> draw_pipelines[9];
static id<MTLRenderPipelineState> mask_pipeline = nil;
static id<MTLRenderPipelineState> share_pipelines[2];
static id<MTLSamplerState> texture_sampler = nil;
static id<MTLSamplerState> mask_sampler = nil;
static bool fetch_supported = false;
static uint32_t max_texture_side = 8192;
static const int max_attachments = 8;

static simd::float4x4 simd_matrix(const hfw::mat4& matrix)
{
    const float* m = matrix.m;
    return simd::float4x4(simd::float4{m[0], m[1], m[2], m[3]}, simd::float4{m[4], m[5], m[6], m[7]}, simd::float4{m[8], m[9], m[10], m[11]}, simd::float4{m[12], m[13], m[14], m[15]});
}

static id<MTLRenderPipelineState> make_pipeline(id<MTLLibrary> library, NSString* vertex, NSString* fragment, int samples, int attachments, int target, const MTLBlendFactor* factors)
{
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:vertex];
    descriptor.fragmentFunction = [library newFunctionWithName:fragment];
    descriptor.rasterSampleCount = samples;
    for (int i = 0; i < attachments; i++)
    {
        descriptor.colorAttachments[i].pixelFormat = MTLPixelFormatBGRA8Unorm;
        descriptor.colorAttachments[i].writeMask = i == target ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
    }
    descriptor.colorAttachments[target].blendingEnabled = factors != NULL;
    if (factors != NULL)
    {
        descriptor.colorAttachments[target].sourceRGBBlendFactor = factors[0];
        descriptor.colorAttachments[target].destinationRGBBlendFactor = factors[1];
        descriptor.colorAttachments[target].sourceAlphaBlendFactor = factors[2];
        descriptor.colorAttachments[target].destinationAlphaBlendFactor = factors[3];
    }
    NSError* error = nil;
    id<MTLRenderPipelineState> state = [renderer_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (state == nil)
    {
        NSLog(@"engine: renderer pipeline %@ failed %@", fragment, error);
    }
    return state;
}

static id<MTLRenderPipelineState> layer_pipeline(std::unordered_map<uint64_t, id<MTLRenderPipelineState>>& cache, int attachments, int target, int source, int mask, int color, int alpha, int fixed)
{
    color = std::clamp(color, 0, 15);
    alpha = std::clamp(alpha, 0, 4);
    uint64_t key = (((((uint64_t)attachments * 8 + target) * 8 + source) * 3 + mask) * 16 + color) * 5 + alpha;
    key = key * 6 + (uint64_t)(fixed + 1);
    __strong id<MTLRenderPipelineState>& state = cache[key];
    if (state != nil)
    {
        return state;
    }
    if (fixed == 3)
    {
        state = make_pipeline(renderer_library, @"under_vertex", @"under_fragment", kSamples, attachments, 0, fixed_blends[3]);
        return state;
    }
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.preprocessorMacros = @{@"HFW_COLOR_BLEND": @(color), @"HFW_ALPHA_BLEND": @(alpha), @"TARGET": @(target), @"SOURCE": @(source), @"MASK": @(mask), @"BLEND": @(fixed < 0 ? 1 : 0)};
    NSError* error = nil;
    id<MTLLibrary> library = [renderer_device newLibraryWithSource:[NSString stringWithUTF8String:(std::string(blend_functions) + layer_shader).c_str()] options:options error:&error];
    if (library == nil)
    {
        NSLog(@"engine: layer shader compile failed %@", error);
        return nil;
    }
    state = make_pipeline(library, @"layer_vertex", fixed == 4 ? @"clear_fragment" : @"layer_fragment", kSamples, attachments, target, fixed >= 0 && fixed < 3 ? fixed_blends[fixed] : NULL);
    return state;
}

bool metal_renderer_start(id<MTLDevice> device, const std::string& shader_source)
{
    if (renderer_device != nil)
    {
        return true;
    }
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:(shader_source + under_shader).c_str()] options:nil error:&error];
    if (library == nil)
    {
        NSLog(@"engine: renderer shader compile failed %@", error);
        return false;
    }
    renderer_device = device;
    renderer_library = library;
    fetch_supported = [device supportsFamily:MTLGPUFamilyApple1];
    max_texture_side = [device supportsFamily:MTLGPUFamilyApple3] || [device supportsFamily:MTLGPUFamilyMac2] ? 16384 : 8192;
    NSString* fragments[3] = {@"hfw_fs_plain", @"hfw_fs_masked", @"hfw_fs_masked_inverted"};
    for (int blend = 0; blend < 3; blend++)
    {
        for (int masked = 0; masked < 3; masked++)
        {
            draw_pipelines[blend * 3 + masked] = make_pipeline(library, masked == 0 ? @"hfw_vs_plain" : @"hfw_vs_masked", fragments[masked], kSamples, 1, 0, fixed_blends[blend]);
        }
    }
    mask_pipeline = make_pipeline(library, @"hfw_vs_mask", @"hfw_fs_mask", 1, 1, 0, fixed_blends[4]);
    MTLSamplerDescriptor* sampler = [MTLSamplerDescriptor new];
    sampler.rAddressMode = MTLSamplerAddressModeRepeat;
    sampler.sAddressMode = MTLSamplerAddressModeRepeat;
    sampler.tAddressMode = MTLSamplerAddressModeRepeat;
    sampler.minFilter = MTLSamplerMinMagFilterLinear;
    sampler.magFilter = MTLSamplerMinMagFilterLinear;
    sampler.mipFilter = MTLSamplerMipFilterLinear;
    texture_sampler = [device newSamplerStateWithDescriptor:sampler];
    sampler.minFilter = MTLSamplerMinMagFilterNearest;
    sampler.magFilter = MTLSamplerMinMagFilterNearest;
    sampler.mipFilter = MTLSamplerMipFilterNotMipmapped;
    mask_sampler = [device newSamplerStateWithDescriptor:sampler];
    for (id<MTLRenderPipelineState> state : draw_pipelines)
    {
        if (state == nil)
        {
            return false;
        }
    }
    return mask_pipeline != nil;
}

static int owning_offscreen(hfw::model* model, int32_t part)
{
    for (; part != -1; part = hfw::part_parent(model, part))
    {
        for (int32_t j = 0; j < hfw::offscreen_count(model); j++)
        {
            if (hfw::offscreen_owners(model)[j] == part)
            {
                return j;
            }
        }
    }
    return -1;
}

static int fixed_blend(int32_t color, int32_t alpha)
{
    if (color == 1)
    {
        return 1;
    }
    if (color == 2)
    {
        return 2;
    }
    return color == 0 && alpha == 0 ? 0 : -1;
}

static id<MTLRenderPipelineState> mode_pipeline(std::unordered_map<uint64_t, id<MTLRenderPipelineState>>& cache, metal_renderer* r, int owner, int source, int mask, int32_t color, int32_t alpha)
{
    int target = owner >= 0 ? r->offscreen_layer[owner] : 0;
    return layer_pipeline(cache, (int)r->layers.size() + 1, target, source, mask, color - 2, alpha, fixed_blend(color, alpha));
}

void metal_renderer_destroy(metal_renderer* r);

static bool compile_layers(metal_renderer* r)
{
    hfw::model* model = r->model;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> cache;
    int attachments = (int)r->layers.size() + 1;
    r->under = layer_pipeline(cache, attachments, 0, 0, 0, 0, 0, 3);
    bool ok = r->under != nil;
    for (int32_t i = 0; r->blend && i < hfw::drawable_count(model); i++)
    {
        bool clipped = r->masks != NULL && hfw::masks_group_of(r->masks, i) >= 0;
        r->drawable_state.push_back(mode_pipeline(cache, r, r->drawable_owner[i], 0, !clipped ? 0 : (hfw::drawable_inverted_mask(model, i) ? 2 : 1), hfw::drawable_color_blend(model, i), hfw::drawable_alpha_blend(model, i)));
        ok = r->drawable_state.back() != nil && ok;
    }
    for (int32_t i = 0; r->blend && i < hfw::offscreen_count(model); i++)
    {
        bool clipped = r->offscreen_masks != NULL && hfw::masks_group_of(r->offscreen_masks, i) >= 0;
        r->offscreen_state.push_back(mode_pipeline(cache, r, r->offscreen_parent[i], r->offscreen_layer[i], !clipped ? 0 : (hfw::offscreen_inverted_mask(model, i) ? 2 : 1), hfw::offscreen_color_blend(model, i), hfw::offscreen_alpha_blend(model, i)));
        r->offscreen_clear.push_back(layer_pipeline(cache, attachments, r->offscreen_layer[i], 0, 0, 0, 0, 4));
        ok = r->offscreen_state.back() != nil && r->offscreen_clear.back() != nil && ok;
    }
    return ok;
}

static void make_passes(metal_renderer* r)
{
    r->mask_pass = [MTLRenderPassDescriptor renderPassDescriptor];
    r->mask_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    r->mask_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    r->mask_pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 1, 1, 1);
    r->pass = [MTLRenderPassDescriptor renderPassDescriptor];
    for (int i = 0; i < (int)r->layers.size() + 1; i++)
    {
        r->pass.colorAttachments[i].loadAction = MTLLoadActionClear;
        r->pass.colorAttachments[i].storeAction = i == 0 ? MTLStoreActionMultisampleResolve : MTLStoreActionDontCare;
        r->pass.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 0);
    }
}

metal_renderer* metal_renderer_create(hfw::model* model, const std::vector<id<MTLTexture>>& textures, const char* name)
{
    metal_renderer* r = new metal_renderer();
    r->model = model;
    r->textures = textures;
    int32_t count = hfw::drawable_count(model);
    size_t moc_length = 0;
    const uint8_t* moc = hfw::moc_bytes(model, &moc_length);
    for (int32_t i = 0; i < count; i++)
    {
        r->vertex_offset.push_back((hfw::drawable_vertices(model, i) - hfw::drawable_vertices(model, 0)) * sizeof(float));
        r->uv_offset.push_back((const uint8_t*)hfw::drawable_uvs(model, i) - moc);
        r->index_offset.push_back((const uint8_t*)hfw::drawable_indices(model, i) - moc);
        r->drawable_owner.push_back(owning_offscreen(model, hfw::drawable_parent_part(model, i)));
    }
    r->moc = [renderer_device newBufferWithBytesNoCopy:(void*)moc length:moc_length options:MTLResourceStorageModeShared deallocator:nil];
    r->positions = [renderer_device newBufferWithLength:std::max<size_t>(hfw::vertex_bytes(model), 16) * 2 options:MTLResourceStorageModeShared];
    if (r->moc == nil || r->positions == nil)
    {
        metal_renderer_destroy(r);
        return NULL;
    }

    r->sorted.assign(count + hfw::offscreen_count(model), 0);
    r->blend = hfw::layered(model);
    hfw::status drawable_mask_status = hfw::status::ok;
    hfw::status offscreen_mask_status = hfw::status::ok;
    r->masks = hfw::masks_create(model, 0, &drawable_mask_status);
    r->offscreen_masks = hfw::masks_create(model, 1, &offscreen_mask_status);
    r->tiled = r->blend;
    for (int kind = 0; kind < 2; kind++)
    {
        hfw::masks* masks = kind == 0 ? r->masks : r->offscreen_masks;
        r->tiled = r->tiled || (masks != NULL && hfw::masks_group_count(masks) > 36);
    }
    uint32_t tiles = 0;
    for (int kind = 0; kind < 2; kind++)
    {
        hfw::masks* masks = kind == 0 ? r->masks : r->offscreen_masks;
        std::vector<uint32_t>& tile = kind == 0 ? r->mask_tile : r->offscreen_mask_tile;
        uint32_t groups = masks == NULL ? 0 : (uint32_t)hfw::masks_group_count(masks);
        for (uint32_t i = 0; i < groups; i++)
        {
            tile.push_back(r->tiled ? tiles + i : tiles);
        }
        tiles += masks == NULL ? 0 : (r->tiled ? groups : 1);
    }
    r->mask_tiles = std::max<uint32_t>(tiles, 1);
    int32_t offscreens = hfw::offscreen_count(model);
    for (int32_t i = 0; i < offscreens; i++)
    {
        r->offscreen_parent.push_back(owning_offscreen(model, hfw::part_parent(model, hfw::offscreen_owners(model)[i])));
    }
    int depth = 0;
    for (int32_t i = 0; i < offscreens; i++)
    {
        int layer = 1;
        for (int parent = r->offscreen_parent[i]; parent >= 0; parent = r->offscreen_parent[parent])
        {
            layer++;
        }
        r->offscreen_layer.push_back(layer);
        depth = std::max(depth, layer);
    }
    r->layers.resize(r->blend ? depth : 0);
    const char* reason = NULL;
    bool textures_valid = !textures.empty();
    for (int32_t i = 0; i < count; i++)
    {
        int32_t texture = hfw::drawable_texture(model, i);
        textures_valid = textures_valid && texture >= 0 && texture < (int32_t)textures.size();
    }
    if (!textures_valid)
    {
        reason = "a drawable names a texture the model3 does not list";
    }
    else if (drawable_mask_status != hfw::status::ok || offscreen_mask_status != hfw::status::ok)
    {
        reason = "clipping masks failed to build";
    }
    else if (r->blend && !fetch_supported)
    {
        reason = "blend modes need framebuffer fetch, which this gpu lacks";
    }
    else if (r->blend && depth + 1 > max_attachments)
    {
        reason = "offscreen nesting exceeds the color attachment limit";
    }
    else if (r->tiled && r->mask_tiles * 256 > max_texture_side)
    {
        reason = "mask atlas exceeds the texture size limit";
    }
    else if (!compile_layers(r))
    {
        reason = "layer pipelines failed to compile";
    }
    if (reason != NULL)
    {
        fprintf(stderr, "engine: model %s unsupported: %s (offscreen depth %d, attachments %d of %d)\n", name, reason, depth, depth + 1, max_attachments);
        metal_renderer_destroy(r);
        return NULL;
    }
    make_passes(r);
    hfw::vertices_attach(model, (float*)r->positions.contents, (float*)((uint8_t*)r->positions.contents + r->positions.length / 2));
    return r;
}

metal_renderer* metal_renderer_view(metal_renderer* source)
{
    metal_renderer* r = new metal_renderer(*source);
    r->view = true;
    r->previous = nil;
    r->msaa = nil;
    r->mask_texture = nil;
    r->mask_side = 0;
    r->layers.assign(source->layers.size(), nil);
    make_passes(r);
    return r;
}

void metal_renderer_destroy(metal_renderer* r)
{
    if (r == NULL)
    {
        return;
    }
    if (r->masks != NULL && !r->view)
    {
        hfw::masks_destroy(r->masks);
    }
    if (r->offscreen_masks != NULL && !r->view)
    {
        hfw::masks_destroy(r->offscreen_masks);
    }
    [r->previous waitUntilCompleted];
    delete r;
}

static void encode_mesh(metal_renderer* r, id<MTLRenderCommandEncoder> encoder, id<MTLBuffer> positions, int32_t index, const void* uniforms, size_t length)
{
    [encoder setCullMode:hfw::drawable_culling(r->model, index) ? MTLCullModeBack : MTLCullModeNone];
    [encoder setVertexBuffer:positions offset:r->slot_offset + r->vertex_offset[index] atIndex:hfw_slot_positions];
    [encoder setVertexBuffer:r->moc offset:r->uv_offset[index] atIndex:hfw_slot_uvs];
    [encoder setVertexBytes:uniforms length:length atIndex:hfw_slot_uniforms];
    [encoder setFragmentBytes:uniforms length:length atIndex:hfw_slot_uniforms];
    [encoder setFragmentTexture:r->textures[hfw::drawable_texture(r->model, index)] atIndex:0];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:hfw::drawable_index_count(r->model, index) indexType:MTLIndexTypeUInt16 indexBuffer:r->moc indexBufferOffset:r->index_offset[index]];
}

static void set_channel(const hfw::mask_group* group, hfw_uniforms& uniforms)
{
    const float* channel = group->channel_rgba;
    uniforms.channel = (vector_float4){channel[0], channel[1], channel[2], channel[3]};
}

static void set_colors(hfw_uniforms& uniforms, float opacity, const float* multiply, const float* screen)
{
    uniforms.base = (vector_float4){opacity, opacity, opacity, opacity};
    uniforms.multiply = (vector_float4){multiply[0], multiply[1], multiply[2], multiply[3]};
    uniforms.screen = (vector_float4){screen[0], screen[1], screen[2], screen[3]};
}

static void size_masks(metal_renderer* renderer, id<MTLTexture> target, const hfw::mat4& projection)
{
    if (renderer->masks == NULL && renderer->offscreen_masks == NULL)
    {
        return;
    }
    uint32_t side = 256;
    if (renderer->tiled)
    {
        while (side < std::max(target.width, target.height) && side < 2048)
        {
            side *= 2;
        }
        while (side > 256 && ((uint64_t)renderer->mask_tiles * side * side * 4 > (64ull << 20) || renderer->mask_tiles * side > max_texture_side))
        {
            side /= 2;
        }
    }
    else
    {
        side = screen_mask_side(renderer->masks, renderer->offscreen_masks, renderer->mask_used, projection.m, (uint32_t)target.width, (uint32_t)target.height, renderer->mask_side, renderer->shrink_frames, renderer->shrink_side);
    }
    if (side == renderer->mask_side)
    {
        return;
    }
    renderer->mask_side = side;
    renderer->mask_updates = UINT32_MAX;
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:side height:renderer->tiled ? side * renderer->mask_tiles : side mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    renderer->mask_texture = [renderer_device newTextureWithDescriptor:descriptor];
    renderer->mask_pass.colorAttachments[0].texture = renderer->mask_texture;
}

static void draw_masks(metal_renderer* r, id<MTLCommandBuffer> command_buffer, id<MTLBuffer> positions)
{
    id<MTLRenderCommandEncoder> encoder = nil;
    for (int kind = 0; kind < 2; kind++)
    {
        hfw::masks* masks = kind == 0 ? r->masks : r->offscreen_masks;
        if (masks == NULL)
        {
            continue;
        }
        if (!r->tiled && r->mask_used[kind] == 0)
        {
            continue;
        }
        if (encoder == nil)
        {
            encoder = [command_buffer renderCommandEncoderWithDescriptor:r->mask_pass];
            [encoder setRenderPipelineState:mask_pipeline];
            [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
            [encoder setFragmentSamplerState:texture_sampler atIndex:0];
        }
        const std::vector<uint32_t>& tile = kind == 0 ? r->mask_tile : r->offscreen_mask_tile;
        for (int32_t group_index = 0; group_index < hfw::masks_group_count(masks); group_index++)
        {
            const hfw::mask_group* group = hfw::masks_group(masks, group_index);
            if (r->tiled && !group->used)
            {
                continue;
            }
            if (r->tiled)
            {
                [encoder setViewport:(MTLViewport){0, (double)(tile[group_index] * r->mask_side), (double)r->mask_side, (double)r->mask_side, 0, 1}];
            }
            hfw_uniforms uniforms = {};
            set_channel(group, uniforms);
            uniforms.clip_matrix = simd_matrix(group->mask_draw);
            const float* rect = group->slot;
            uniforms.base = (vector_float4){rect[0] * 2.0f - 1.0f, rect[1] * 2.0f - 1.0f, (rect[0] + rect[2]) * 2.0f - 1.0f, (rect[1] + rect[3]) * 2.0f - 1.0f};
            for (int32_t i = 0; i < group->mask_drawable_count; i++)
            {
                int32_t index = group->mask_drawables[i];
                if (!hfw::drawable_moved(r->model, index) || hfw::drawable_index_count(r->model, index) <= 0 || hfw::drawable_vertex_count(r->model, index) <= 0 || r->textures[hfw::drawable_texture(r->model, index)] == nil)
                {
                    continue;
                }
                encode_mesh(r, encoder, positions, index, &uniforms, sizeof(uniforms));
            }
        }
    }
    [encoder endEncoding];
}

id<MTLTexture> metal_renderer_msaa(metal_renderer* r, int width, int height)
{
    if (r->msaa == nil || (int)r->msaa.width != width || (int)r->msaa.height != height)
    {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
        descriptor.textureType = MTLTextureType2DMultisample;
        descriptor.sampleCount = kSamples;
        descriptor.storageMode = fetch_supported ? MTLStorageModeMemoryless : MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageRenderTarget;
        r->msaa = [renderer_device newTextureWithDescriptor:descriptor];
        for (__strong id<MTLTexture>& layer : r->layers)
        {
            layer = [renderer_device newTextureWithDescriptor:descriptor];
        }
    }
    return r->msaa;
}

static void draw_quad(id<MTLRenderCommandEncoder> encoder, const layer_uniforms& uniforms)
{
    [encoder setVertexBytes:quad length:sizeof(quad) atIndex:hfw_slot_positions];
    [encoder setVertexBytes:quad length:sizeof(quad) atIndex:hfw_slot_uvs];
    [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:hfw_slot_uniforms];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:hfw_slot_uniforms];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

static void set_clip(metal_renderer* r, layer_uniforms& uniforms, const hfw::mask_group* group, uint32_t tile)
{
    set_channel(group, uniforms.shared);
    uniforms.shared.clip_matrix = simd_matrix(group->sample);
    uniforms.tile = (simd::float4){(float)tile, (float)r->mask_tiles, 0.0f, 0.0f};
}

static void draw_offscreen(metal_renderer* r, id<MTLRenderCommandEncoder> encoder, int offscreen)
{
    hfw::model* model = r->model;
    r->current = r->offscreen_parent[offscreen];
    int32_t clip = r->offscreen_masks != NULL ? hfw::masks_group_of(r->offscreen_masks, offscreen) : -1;
    layer_uniforms uniforms = {};
    uniforms.shared.matrix = matrix_identity_float4x4;
    set_colors(uniforms.shared, hfw::offscreen_opacity(model, offscreen), hfw::offscreen_multiply(model, offscreen), hfw::offscreen_screen(model, offscreen));
    if (clip >= 0)
    {
        set_clip(r, uniforms, hfw::masks_group(r->offscreen_masks, clip), r->offscreen_mask_tile[clip]);
    }
    [encoder setRenderPipelineState:r->offscreen_state[offscreen]];
    [encoder setCullMode:hfw::offscreen_culling(model, offscreen) ? MTLCullModeBack : MTLCullModeNone];
    draw_quad(encoder, uniforms);
}

static void submit_to_parent(metal_renderer* r, id<MTLRenderCommandEncoder> encoder, int owner)
{
    while (r->current >= 0)
    {
        for (int parent = owner; parent >= 0; parent = r->offscreen_parent[parent])
        {
            if (parent == r->current)
            {
                return;
            }
        }
        draw_offscreen(r, encoder, r->current);
    }
}

static void add_offscreen(metal_renderer* r, id<MTLRenderCommandEncoder> encoder, int offscreen)
{
    if (r->current != offscreen)
    {
        submit_to_parent(r, encoder, r->offscreen_parent[offscreen]);
    }
    layer_uniforms uniforms = {};
    uniforms.shared.matrix = matrix_identity_float4x4;
    r->current = offscreen;
    [encoder setRenderPipelineState:r->offscreen_clear[offscreen]];
    [encoder setCullMode:MTLCullModeNone];
    draw_quad(encoder, uniforms);
}

static void draw_layer_drawable(metal_renderer* r, id<MTLRenderCommandEncoder> encoder, id<MTLBuffer> positions, simd::float4x4 matrix, int32_t index)
{
    hfw::model* model = r->model;
    if (!hfw::drawable_visible(model, index))
    {
        return;
    }
    submit_to_parent(r, encoder, r->drawable_owner[index]);
    int32_t clip = r->masks != NULL ? hfw::masks_group_of(r->masks, index) : -1;
    if (hfw::drawable_index_count(model, index) <= 0 || hfw::drawable_opacity(model, index) <= 0.0f || r->textures[hfw::drawable_texture(model, index)] == nil)
    {
        return;
    }
    layer_uniforms uniforms = {};
    uniforms.shared.matrix = matrix;
    set_colors(uniforms.shared, hfw::drawable_opacity(model, index), hfw::drawable_multiply(model, index), hfw::drawable_screen(model, index));
    if (clip >= 0)
    {
        set_clip(r, uniforms, hfw::masks_group(r->masks, clip), r->mask_tile[clip]);
    }
    [encoder setRenderPipelineState:r->drawable_state[index]];
    encode_mesh(r, encoder, positions, index, &uniforms, sizeof(uniforms));
}

void metal_renderer_draw(metal_renderer* r, id<MTLCommandBuffer> command_buffer, id<MTLTexture> target, const hfw::mat4& mvp, id<MTLTexture> back, bool opaque)
{
    hfw::model* model = r->model;
    int32_t count = hfw::drawable_count(model);
    int32_t total = count + hfw::offscreen_count(model);
    id<MTLBuffer> positions = r->positions;
    r->slot_offset = hfw::vertex_slot(model) * (positions.length / 2);
    const int32_t* orders = hfw::render_orders(model);
    for (int32_t i = 0; i < total; i++)
    {
        r->sorted[orders[i]] = i;
    }
    uint32_t updates = hfw::model_updates(model);
    for (int kind = 0; r->mask_updates != updates && kind < 2; kind++)
    {
        hfw::masks* masks = kind == 0 ? r->masks : r->offscreen_masks;
        r->mask_used[kind] = masks == NULL ? 0 : hfw::masks_layout(masks, model, r->tiled);
    }
    size_masks(r, target, mvp);
    if (r->mask_updates != updates)
    {
        draw_masks(r, command_buffer, positions);
        r->mask_updates = updates;
    }
    r->pass.colorAttachments[0].texture = metal_renderer_msaa(r, (int)target.width, (int)target.height);
    r->pass.colorAttachments[0].resolveTexture = target;
    for (size_t i = 0; i < r->layers.size(); i++)
    {
        r->pass.colorAttachments[i + 1].texture = r->layers[i];
    }
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:r->pass];
    [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    [encoder setFragmentSamplerState:texture_sampler atIndex:0];
    [encoder setFragmentSamplerState:mask_sampler atIndex:1];
    if (r->mask_texture != nil)
    {
        [encoder setFragmentTexture:r->mask_texture atIndex:1];
    }
    simd::float4x4 matrix = simd_matrix(mvp);
    if (r->blend)
    {
        r->current = -1;
        for (int32_t n = 0; n < total; n++)
        {
            if (r->sorted[n] < count)
            {
                draw_layer_drawable(r, encoder, positions, matrix, r->sorted[n]);
            }
            else
            {
                add_offscreen(r, encoder, r->sorted[n] - count);
            }
        }
        while (r->current >= 0)
        {
            draw_offscreen(r, encoder, r->current);
        }
    }
    else
    {
        for (int32_t n = 0; n < count; n++)
        {
            int32_t index = r->sorted[n];
            if (!hfw::drawable_visible(model, index) || hfw::drawable_index_count(model, index) <= 0 || r->textures[hfw::drawable_texture(model, index)] == nil)
            {
                continue;
            }
            int32_t clip = r->masks != NULL ? hfw::masks_group_of(r->masks, index) : -1;
            int blend = std::max(fixed_blend(hfw::drawable_color_blend(model, index), hfw::drawable_alpha_blend(model, index)), 0);
            int masked = clip < 0 ? 0 : (hfw::drawable_inverted_mask(model, index) ? 2 : 1);
            hfw_uniforms uniforms = {};
            if (clip >= 0)
            {
                const hfw::mask_group* group = hfw::masks_group(r->masks, clip);
                set_channel(group, uniforms);
                uniforms.clip_matrix = simd_matrix(group->sample);
                for (int column = 0; r->tiled && column < 4; column++)
                {
                    uniforms.clip_matrix.columns[column].y = (uniforms.clip_matrix.columns[column].y + uniforms.clip_matrix.columns[column].w * (r->mask_tiles - r->mask_tile[clip] - 1)) / r->mask_tiles;
                }
            }
            uniforms.matrix = matrix;
            set_colors(uniforms, hfw::drawable_opacity(model, index), hfw::drawable_multiply(model, index), hfw::drawable_screen(model, index));
            [encoder setRenderPipelineState:draw_pipelines[blend * 3 + masked]];
            encode_mesh(r, encoder, positions, index, &uniforms, sizeof(uniforms));
        }
    }
    if (opaque)
    {
        vector_float4 size = {(float)target.width, (float)target.height, back != nil ? 1.0f : 0.0f, 0.0f};
        [encoder setRenderPipelineState:r->under];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setFragmentTexture:back != nil ? back : r->textures[0] atIndex:0];
        [encoder setFragmentBytes:&size length:sizeof(size) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];
    if (r->previous != command_buffer)
    {
        [r->previous waitUntilCompleted];
        r->previous = command_buffer;
    }
    hfw::vertices_release(model);
}

bool metal_renderer_share(id<MTLCommandBuffer> command_buffer, id<MTLTexture> model, id<MTLTexture> back, id<MTLTexture> window_target, id<MTLTexture> share_target)
{
    int shown = window_target != nil ? 0 : 1;
    if (share_pipelines[shown] == nil)
    {
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [renderer_library newFunctionWithName:@"under_vertex"];
        descriptor.fragmentFunction = [renderer_library newFunctionWithName:@"share_fragment"];
        descriptor.colorAttachments[0].pixelFormat = window_target != nil ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatInvalid;
        descriptor.colorAttachments[1].pixelFormat = MTLPixelFormatBGRA8Unorm;
        NSError* error = nil;
        share_pipelines[shown] = [renderer_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (share_pipelines[shown] == nil)
        {
            NSLog(@"engine: share pipeline failed, drawing the window only %@", error);
            return false;
        }
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = window_target;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[1].texture = share_target;
    pass.colorAttachments[1].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[1].storeAction = MTLStoreActionStore;
    vector_float4 size = {(float)share_target.width, (float)share_target.height, back != nil ? 1.0f : 0.0f, 0.0f};
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:share_pipelines[shown]];
    [encoder setFragmentTexture:model atIndex:0];
    [encoder setFragmentTexture:back != nil ? back : model atIndex:1];
    [encoder setFragmentBytes:&size length:sizeof(size) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    return true;
}
