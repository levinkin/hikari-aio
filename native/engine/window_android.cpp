#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#include "window_vk.h"

struct window
{
    ANativeWindow* native_window;
    int width;
    int height;
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

extern "C" window* window_attach(ANativeWindow* native_window, int width, int height)
{
    ANativeWindow_acquire(native_window);
    window* w = new window();
    w->native_window = native_window;
    w->width = width;
    w->height = height;
    return w;
}

extern "C" void window_resize(window* w, int width, int height)
{
    w->width = width;
    w->height = height;
}

void window_size(window* w, int* width, int* height)
{
    *width = w->width;
    *height = w->height;
}

void window_close(window* w)
{
    window_release_target(w);
    ANativeWindow_release(w->native_window);
    delete w;
}

const char* window_surface_extension()
{
    return VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
}

VkSurfaceKHR window_surface(window* w, VkInstance instance)
{
    VkAndroidSurfaceCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    info.window = w->native_window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateAndroidSurfaceKHR(instance, &info, NULL, &surface) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    return surface;
}
