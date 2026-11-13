#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "embed.h"

static int log_pipe[2];

static void* forward_log(void* unused)
{
    char line[1024];
    FILE* input = fdopen(log_pipe[0], "r");
    while (fgets(line, sizeof line, input) != NULL)
    {
        __android_log_write(ANDROID_LOG_INFO, "hikari", line);
    }
    return NULL;
}

static int extract_file(AAssetManager* assets, const char* data_directory, const char* name)
{
    char path[1024];
    char part[1100];
    snprintf(path, sizeof path, "%s/%s", data_directory, name);
    if (access(path, F_OK) == 0)
    {
        return 0;
    }
    for (char* slash = strchr(path + strlen(data_directory) + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/'))
    {
        *slash = 0;
        mkdir(path, 0700);
        *slash = '/';
    }
    AAsset* asset = AAssetManager_open(assets, name, AASSET_MODE_STREAMING);
    if (asset == NULL)
    {
        return -1;
    }
    snprintf(part, sizeof part, "%s.part", path);
    FILE* output = fopen(part, "wb");
    if (output == NULL)
    {
        AAsset_close(asset);
        return -1;
    }
    char buffer[65536];
    int length;
    int failed = 0;
    while (!failed && (length = AAsset_read(asset, buffer, sizeof buffer)) > 0)
    {
        failed = fwrite(buffer, 1, (size_t)length, output) != (size_t)length;
    }
    failed |= length < 0;
    failed |= fclose(output) != 0;
    AAsset_close(asset);
    if (failed || rename(part, path) != 0)
    {
        unlink(part);
        return -1;
    }
    return 0;
}

static int extract_assets(AAssetManager* assets, const char* data_directory)
{
    AAsset* list = AAssetManager_open(assets, "files", AASSET_MODE_BUFFER);
    if (list == NULL)
    {
        return -1;
    }
    size_t length = (size_t)AAsset_getLength(list);
    const void* buffer = AAsset_getBuffer(list);
    char* names = buffer == NULL ? NULL : malloc(length + 1);
    if (names == NULL)
    {
        AAsset_close(list);
        return -1;
    }
    memcpy(names, buffer, length);
    names[length] = 0;
    AAsset_close(list);
    int failed = 0;
    char* position = NULL;
    for (char* name = strtok_r(names, "\n", &position); name != NULL && !failed; name = strtok_r(NULL, "\n", &position))
    {
        failed = extract_file(assets, data_directory, name) != 0;
        if (failed)
        {
            __android_log_print(ANDROID_LOG_ERROR, "hikari", "extract failed: %s", name);
        }
    }
    free(names);
    return failed ? -1 : 0;
}

static void handle_command(struct android_app* app, int32_t command)
{
    int* status = app->userData;
    if (*status != 0 || app->window == NULL)
    {
        return;
    }
    if (command == APP_CMD_INIT_WINDOW && !hikari_surface(app->window, ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window)))
    {
        *status = 1;
    }
    if (command == APP_CMD_WINDOW_RESIZED || command == APP_CMD_CONFIG_CHANGED)
    {
        hikari_resize(ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window));
    }
}

static jobject power_manager(struct android_app* app, JNIEnv* env)
{
    jclass context = (*env)->FindClass(env, "android/content/Context");
    jmethodID get_service = (*env)->GetMethodID(env, context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject service = (*env)->CallObjectMethod(env, app->activity->clazz, get_service, (*env)->NewStringUTF(env, "power"));
    return (*env)->NewGlobalRef(env, service);
}

static int wanted_rate(struct android_app* app, JNIEnv* env, jobject power, jmethodID power_save, int rate)
{
    static double checked = -1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double seconds = now.tv_sec + now.tv_nsec / 1e9;
    if (app->window == NULL || app->activityState != APP_CMD_RESUME)
    {
        checked = -1;
        return 0;
    }
    if (rate > 0 && seconds - checked < 1)
    {
        return rate;
    }
    checked = seconds;
    return (*env)->CallBooleanMethod(env, power, power_save) ? 30 : 60;
}

void android_main(struct android_app* app)
{
    pipe(log_pipe);
    dup2(log_pipe[1], STDOUT_FILENO);
    dup2(log_pipe[1], STDERR_FILENO);
    pthread_t logger;
    pthread_create(&logger, NULL, forward_log, NULL);
    const char* data_directory = app->activity->internalDataPath;
    int status = extract_assets(app->activity->assetManager, data_directory) != 0 ? 2 : hikari_start(data_directory, 9397);
    if (status != 0)
    {
        __android_log_print(ANDROID_LOG_ERROR, "hikari", "start failed (%d)", status);
    }
    app->userData = &status;
    app->onAppCmd = handle_command;
    JNIEnv* env = NULL;
    (*app->activity->vm)->AttachCurrentThread(app->activity->vm, &env, NULL);
    jobject power = power_manager(app, env);
    jmethodID power_save = (*env)->GetMethodID(env, (*env)->FindClass(env, "android/os/PowerManager"), "isPowerSaveMode", "()Z");
    int rate = -1;
    while (!app->destroyRequested)
    {
        bool drawing = status == 0 && rate > 0;
        struct android_poll_source* source = NULL;
        if (ALooper_pollOnce(drawing ? 0 : -1, NULL, NULL, (void**)&source) >= 0 && source != NULL)
        {
            source->process(app, source);
        }
        int wanted = wanted_rate(app, env, power, power_save, rate);
        if (wanted != rate)
        {
            rate = wanted;
            hikari_rate(rate);
        }
        if (status == 0 && rate > 0)
        {
            hikari_frame();
        }
    }
    _exit(0);
}
