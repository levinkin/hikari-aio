#pragma once
#include <stdbool.h>
#include <stdint.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
int hikari_start(const char* data_directory, uint16_t port);
void hikari_stop(void);
#if defined(__APPLE__) && TARGET_OS_IOS
bool hikari_surface(void* metal_layer, int width, int height);
#elif defined(__ANDROID__)
struct ANativeWindow;
bool hikari_surface(struct ANativeWindow* native_window, int width, int height);
#endif
#if (defined(__APPLE__) && TARGET_OS_IOS) || defined(__ANDROID__)
void hikari_frame(void);
void hikari_resize(int width, int height);
void hikari_rate(int fps);
#endif
#ifdef __cplusplus
}
#endif
