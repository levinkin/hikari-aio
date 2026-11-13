#include "window_metal.h"

struct window
{
    metal_target target;
    int width = 0;
    int height = 0;
    float pan_x = 0;
    float pan_y = 0;
    float zoom = 1;
    float cursor_x = 0;
    float cursor_y = 0;
    bool turn_ccw = false;
    bool turn_cw = false;
    int keys = 0;
};

#include "window_input.inc"

extern "C" window* window_attach(void* layer, int width, int height)
{
    window* w = new window();
    w->target.layer = (__bridge CAMetalLayer*)layer;
    metal_layer_setup(w->target.layer);
    w->width = width;
    w->height = height;
    return w;
}

extern "C" void window_resize(window* w, int width, int height)
{
    w->width = width;
    w->height = height;
}

void window_close(window* w)
{
    if (w->target.last != nil)
    {
        [w->target.last waitUntilCompleted];
    }
    delete w;
}

metal_target* window_target(window* w, float frame_seconds, bool* share, bool* hidden, int* width, int* height)
{
    *share = false;
    *hidden = false;
    *width = w->width;
    *height = w->height;
    return w->width > 0 && w->height > 0 ? &w->target : NULL;
}

bool window_share(window* w, metal_renderer* model, id<MTLCommandBuffer> command_buffer, const hfw::mat4& mvp, id<MTLTexture> back, id<CAMetalDrawable> drawable, int width, int height, float frame_seconds)
{
    return false;
}
