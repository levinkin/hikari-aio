#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "engine.h"
#include "texture_bc7.inc"
#include "hfw.h"
#include "window_metal.h"

static const char metal_shaders[] = {
#embed "draw.metal"
    , 0};

struct metal_renderer;
bool metal_renderer_start(id<MTLDevice> device, const std::string& shader_source);
metal_renderer* metal_renderer_create(hfw::model* model, const std::vector<id<MTLTexture>>& textures, const char* name);
metal_renderer* metal_renderer_view(metal_renderer* source);
void metal_renderer_destroy(metal_renderer* r);

struct engine_gpu
{
    metal_renderer* own = NULL;
    bool view = false;
    id<MTLTexture> color_texture = nil;
    uint64_t background_serial = 0;
};

#include "engine_common.inc"

static id<MTLDevice> device = nil;
static id<MTLCommandQueue> queue = nil;
static std::map<std::string, std::vector<id<MTLTexture>>> texture_cache;
static std::mutex texture_lock;

static bool start_metal()
{
    if (device != nil)
    {
        return true;
    }
    device = MTLCreateSystemDefaultDevice();
    if (device == nil)
    {
        return false;
    }
    queue = [device newCommandQueue];
    gpu_name = std::string([[device name] UTF8String]);
    return metal_renderer_start(device, metal_shaders);
}

static void upload_level(id<MTLTexture> texture, id<MTLBuffer> staging, const uint8_t* data, uint32_t level, size_t row_bytes, uint32_t rows, uint32_t row_height)
{
    uint32_t w = std::max((uint32_t)texture.width >> level, 1u);
    uint32_t h = std::max((uint32_t)texture.height >> level, 1u);
    uint32_t chunk_rows = (uint32_t)std::max(staging.length / row_bytes, (NSUInteger)1);
    for (uint32_t row = 0; row < rows; row += chunk_rows)
    {
        uint32_t count = std::min(chunk_rows, rows - row);
        memcpy([staging contents], data + row * row_bytes, count * row_bytes);
        uint32_t y = row * row_height;
        id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
        [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:row_bytes sourceBytesPerImage:count * row_bytes sourceSize:MTLSizeMake(w, std::min(count * row_height, h - y), 1) toTexture:texture destinationSlice:0 destinationLevel:level destinationOrigin:MTLOriginMake(0, y, 0)];
        [blit endEncoding];
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
    }
}

static id<MTLTexture> load_bc7(FILE* file, const bc7_header& header)
{
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC7_RGBAUnorm width:header.width height:header.height mipmapped:YES];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    id<MTLBuffer> staging = [device newBufferWithLength:std::min(bc7_level_bytes(header.width, header.height, 0), staging_size) options:MTLResourceStorageModeShared];
    std::vector<uint8_t> level_bytes;
    bool ok = texture != nil && staging != nil;
    for (uint32_t level = 0; ok && level < header.levels; level++)
    {
        level_bytes.resize(bc7_level_bytes(header.width, header.height, level));
        ok = fread(level_bytes.data(), 1, level_bytes.size(), file) == level_bytes.size();
        uint32_t blocks_w = (std::max(header.width >> level, 1u) + 3) / 4;
        if (ok)
        {
            upload_level(texture, staging, level_bytes.data(), level, (size_t)blocks_w * 16, (uint32_t)(level_bytes.size() / (blocks_w * 16)), 4);
        }
    }
    fclose(file);
    return ok ? texture : nil;
}

static id<MTLTexture> load_texture(const std::string& path)
{
    std::vector<uint8_t> bytes = read_file(path);
    bool use_bc7 = !bytes.empty() && [device supportsBCTextureCompression];
    std::string cache_path = use_bc7 ? bc7_cache_path(bytes) : "";
    bc7_header header;
    FILE* cached = bc7_open(cache_path, header);
    if (cached != NULL && header.levels > 0)
    {
        return load_bc7(cached, header);
    }
    if (cached != NULL)
    {
        fclose(cached);
        use_bc7 = false;
    }
    NSData* data = [NSData dataWithBytesNoCopy:bytes.data() length:bytes.size() freeWhenDone:NO];
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
    if (source == NULL)
    {
        return nil;
    }
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    CFRelease(source);
    if (image == NULL)
    {
        return nil;
    }
    size_t w = CGImageGetWidth(image);
    size_t h = CGImageGetHeight(image);
    std::vector<uint8_t> pixels(w * h * 4);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(pixels.data(), w, h, 8, w * 4, space, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    CGImageRelease(image);
    if (use_bc7 && bc7_encode(pixels.data(), (uint32_t)w, (uint32_t)h, cache_path) && (cached = bc7_open(cache_path, header)) != NULL)
    {
        return load_bc7(cached, header);
    }
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:YES];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    id<MTLBuffer> staging = [device newBufferWithLength:std::min(pixels.size(), staging_size) options:MTLResourceStorageModeShared];
    upload_level(texture, staging, pixels.data(), 0, w * 4, (uint32_t)h, 1);
    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    [blit generateMipmapsForTexture:texture];
    [blit endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    return texture;
}

engine* engine_create(const char* model3_json_path, int width, int height)
{
    @autoreleasepool
    {
        if (!start_metal())
        {
            return NULL;
        }
        std::string path(model3_json_path);
        std::string dir = path.substr(0, path.rfind('/') + 1);
        engine* e = load_model(path, dir, width, height);
        if (e == NULL)
        {
            return NULL;
        }
        std::lock_guard<std::mutex> guard(texture_lock);
        std::vector<id<MTLTexture>>& textures = texture_cache[path];
        if (textures.empty())
        {
            for (int32_t i = 0; i < hfw::setting_texture_count(e->setting); i++)
            {
                textures.push_back(load_texture(dir + hfw::setting_texture(e->setting, i)));
            }
        }
        e->own = metal_renderer_create(e->model, textures, model3_json_path);
        if (e->own == NULL)
        {
            engine_destroy(e);
            return NULL;
        }
        view_matrix(e);
        return e;
    }
}

engine* engine_view(engine* source, int width, int height)
{
    engine* e = new engine();
    e->view = true;
    e->model = source->model;
    e->width = width;
    e->height = height;
    e->own = metal_renderer_view(source->own);
    view_matrix(e);
    return e;
}

void engine_destroy(engine* e)
{
    @autoreleasepool
    {
        metal_renderer_destroy(e->own);
        if (e->view)
        {
            delete e;
            return;
        }
        engine_release(e);
    }
}

void engine_textures_release(const char* model3_json_path)
{
    std::lock_guard<std::mutex> guard(texture_lock);
    texture_cache.erase(model3_json_path);
}

int engine_background_load(engine* e, const uint8_t* bytes, size_t length, const char* expected_type)
{
    @autoreleasepool
    {
        NSDictionary* formats = @{
            @"image/png": @[@"public.png", [NSData dataWithBytes:"IEND\xae\x42\x60\x82" length:8]],
            @"image/jpeg": @[@"public.jpeg", [NSData dataWithBytes:"\xff\xd9" length:2]],
            @"image/gif": @[@"com.compuserve.gif", [NSData dataWithBytes:";" length:1]],
        };
        NSArray* format = formats[[NSString stringWithUTF8String:expected_type]];
        NSData* trailer = format[1];
        NSData* data = [NSData dataWithBytes:bytes length:length];
        CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
        if (source == NULL)
        {
            return 0;
        }
        size_t count = CGImageSourceGetCount(source);
        bool ok = count > 0 && format != nil && [(__bridge NSString*)CGImageSourceGetType(source) isEqualToString:format[0]] && CGImageSourceGetStatus(source) == kCGImageStatusComplete && length >= trailer.length && memcmp(bytes + length - trailer.length, trailer.bytes, trailer.length) == 0;
        std::vector<std::vector<uint8_t>> frames;
        std::vector<float> ms;
        float total_ms = 0;
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        for (size_t i = 0; ok && i < count; i++)
        {
            CGImageRef image = CGImageSourceCreateImageAtIndex(source, i, NULL);
            if (image == NULL)
            {
                ok = false;
                break;
            }
            NSDictionary* properties = (__bridge_transfer NSDictionary*)CGImageSourceCopyPropertiesAtIndex(source, i, NULL);
            NSNumber* delay = nil;
            NSDictionary* gif = properties[(__bridge NSString*)kCGImagePropertyGIFDictionary];
            NSDictionary* png = properties[(__bridge NSString*)kCGImagePropertyPNGDictionary];
            NSDictionary* webp = properties[(__bridge NSString*)kCGImagePropertyWebPDictionary];
            if (gif != nil)
            {
                delay = gif[(__bridge NSString*)kCGImagePropertyGIFUnclampedDelayTime] ?: gif[(__bridge NSString*)kCGImagePropertyGIFDelayTime];
            }
            if (png != nil)
            {
                delay = png[(__bridge NSString*)kCGImagePropertyAPNGUnclampedDelayTime] ?: png[(__bridge NSString*)kCGImagePropertyAPNGDelayTime];
            }
            if (webp != nil)
            {
                delay = webp[(__bridge NSString*)kCGImagePropertyWebPUnclampedDelayTime] ?: webp[(__bridge NSString*)kCGImagePropertyWebPDelayTime];
            }
            float frame_ms = delay != nil ? [delay floatValue] * 1000.0f : 100.0f;
            if (frame_ms < 20.0f)
            {
                frame_ms = 100.0f;
            }
            double iw = CGImageGetWidth(image);
            double ih = CGImageGetHeight(image);
            double scale = fmax(e->width / iw, e->height / ih);
            double bw = ceil(iw * scale);
            double bh = ceil(ih * scale);
            std::vector<uint8_t> frame((size_t)e->width * e->height * 4);
            CGContextRef context = CGBitmapContextCreate(frame.data(), e->width, e->height, 8, e->width * 4, space, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
            CGContextSetRGBFillColor(context, 0, 0, 0, 1);
            CGContextFillRect(context, CGRectMake(0, 0, e->width, e->height));
            CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
            CGContextDrawImage(context, CGRectMake((e->width - bw) / 2, (e->height - bh) / 2, bw, bh), image);
            CGContextRelease(context);
            CGImageRelease(image);
            frames.push_back(std::move(frame));
            ms.push_back(frame_ms);
            total_ms += frame_ms;
        }
        CGColorSpaceRelease(space);
        CFRelease(source);
        if (!ok)
        {
            return 0;
        }
        e->background_frames = std::move(frames);
        e->background_ms = std::move(ms);
        e->background_total_ms = total_ms;
        e->background_serial++;
        return (int)e->background_frames.size();
    }
}

const uint8_t* engine_render(engine* e, float background_clock_ms)
{
    @autoreleasepool
    {
        if (e->color_texture == nil || (int)e->color_texture.width != e->width || (int)e->color_texture.height != e->height)
        {
            MTLTextureDescriptor* color = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:e->width height:e->height mipmapped:NO];
            color.storageMode = MTLStorageModeShared;
            color.usage = MTLTextureUsageRenderTarget;
            e->color_texture = [device newTextureWithDescriptor:color];
            e->rgba.resize((size_t)e->width * e->height * 4);
        }
        update_shown(e);
        id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
        metal_renderer_draw(e->own, command_buffer, e->color_texture, e->mvp, nil, false);
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        [e->color_texture getBytes:e->rgba.data() bytesPerRow:e->width * 4 fromRegion:MTLRegionMake2D(0, 0, e->width, e->height) mipmapLevel:0];
    }
    const uint8_t* back = background_at(e, background_clock_ms);
    uint8_t* out = e->rgba.data();
    size_t count = (size_t)e->width * e->height;
    for (size_t p = 0; p < count; p++, out += 4)
    {
        int keep = 255 - out[3];
        int blue = out[0];
        int back_red = back != NULL ? back[p * 4] : 0;
        int back_green = back != NULL ? back[p * 4 + 1] : 0;
        int back_blue = back != NULL ? back[p * 4 + 2] : 0;
        out[0] = (uint8_t)((out[2] * 255 + back_red * keep + 127) / 255);
        out[1] = (uint8_t)((out[1] * 255 + back_green * keep + 127) / 255);
        out[2] = (uint8_t)((blue * 255 + back_blue * keep + 127) / 255);
        out[3] = 255;
    }
    return e->rgba.data();
}

bool engine_present(engine* e, window* w, float background_clock_ms, float frame_seconds, bool share)
{
    @autoreleasepool
    {
        int width = 0;
        int height = 0;
        bool hidden = false;
        metal_target* target = device == nil ? NULL : window_target(w, frame_seconds, &share, &hidden, &width, &height);
        if (target == NULL)
        {
            return false;
        }
        if (target->last != nil)
        {
            [target->last waitUntilCompleted];
            target->last = nil;
        }
        target->layer.device = device;
        if (target->width != width || target->height != height)
        {
            target->layer.drawableSize = CGSizeMake(width, height);
            target->width = width;
            target->height = height;
        }
        int stream_width = e->width;
        int stream_height = e->height;
        e->width = width;
        e->height = height;
        view_matrix(e);
        hfw::mat4 mvp = e->mvp;
        e->width = stream_width;
        e->height = stream_height;
        view_matrix(e);
        const uint8_t* back = background_at(e, background_clock_ms);
        if (back != NULL)
        {
            if (target->background_texture == nil || (int)target->background_texture.width != e->width || (int)target->background_texture.height != e->height)
            {
                MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:e->width height:e->height mipmapped:NO];
                descriptor.storageMode = MTLStorageModeShared;
                descriptor.usage = MTLTextureUsageShaderRead;
                target->background_texture = [device newTextureWithDescriptor:descriptor];
                target->background_uploaded = NULL;
            }
            if (back != target->background_uploaded || e->background_serial != target->background_uploaded_load)
            {
                [target->background_texture replaceRegion:MTLRegionMake2D(0, 0, e->width, e->height) mipmapLevel:0 withBytes:back bytesPerRow:e->width * 4];
                target->background_uploaded = back;
                target->background_uploaded_load = e->background_serial;
            }
        }
        update_shown(e);
        id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
        id<CAMetalDrawable> drawable = hidden ? nil : [target->layer nextDrawable];
        id<MTLTexture> back_texture = back != NULL ? target->background_texture : nil;
        if (share && window_share(w, e->own, command_buffer, mvp, back_texture, drawable, width, height, frame_seconds))
        {
            [command_buffer commit];
            target->last = command_buffer;
            return true;
        }
        if (drawable == nil)
        {
            [command_buffer commit];
            target->last = command_buffer;
            return false;
        }
        metal_renderer_draw(e->own, command_buffer, drawable.texture, mvp, back_texture, true);
#if TARGET_OS_SIMULATOR
        [command_buffer presentDrawable:drawable];
#else
        [command_buffer presentDrawable:drawable afterMinimumDuration:frame_seconds];
#endif
        [command_buffer commit];
        target->last = command_buffer;
        return true;
    }
}
