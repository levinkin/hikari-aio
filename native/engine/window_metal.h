#pragma once
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "engine.h"
#include "hfw.h"

struct metal_renderer;
void metal_renderer_draw(metal_renderer* r, id<MTLCommandBuffer> command_buffer, id<MTLTexture> target, const hfw::mat4& mvp, id<MTLTexture> back, bool opaque);
bool metal_renderer_share(id<MTLCommandBuffer> command_buffer, id<MTLTexture> model, id<MTLTexture> back, id<MTLTexture> window_target, id<MTLTexture> share_target);

struct metal_target
{
    CAMetalLayer* layer = nil;
    int width = 0;
    int height = 0;
    id<MTLTexture> background_texture = nil;
    const uint8_t* background_uploaded = NULL;
    uint64_t background_uploaded_load = 0;
    id<MTLCommandBuffer> last = nil;
};

static inline void metal_layer_setup(CAMetalLayer* layer)
{
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.maximumDrawableCount = 2;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    layer.colorspace = space;
    CGColorSpaceRelease(space);
}

metal_target* window_target(window* w, float frame_seconds, bool* share, bool* hidden, int* width, int* height);
bool window_share(window* w, metal_renderer* model, id<MTLCommandBuffer> command_buffer, const hfw::mat4& mvp, id<MTLTexture> back, id<CAMetalDrawable> drawable, int width, int height, float frame_seconds);
