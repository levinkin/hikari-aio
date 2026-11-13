#import <AppKit/AppKit.h>
#include <cmath>
#include "window_metal.h"
#import "SyphonServerBase.h"

@interface SyphonServerBase (SyphonSubclassing)
- (IOSurfaceRef)newSurfaceForWidth:(size_t)width height:(size_t)height options:(NSDictionary*)options;
- (void)publish;
@end

@interface hikari_window : NSWindow
@property(nonatomic) BOOL close_requested;
@end

@implementation hikari_window
- (void)performClose:(id)sender
{
    self.close_requested = YES;
}
- (BOOL)canBecomeKeyWindow
{
    return YES;
}
- (BOOL)canBecomeMainWindow
{
    return YES;
}
@end

struct window
{
    hikari_window* ns = nil;
    NSView* view = nil;
    metal_target target;
    id monitor = nil;
    id close_observer = nil;
    id resign_observer = nil;
    NSTimer* timer = nil;
    engine_hook frame = NULL;
    void* context = NULL;
    int* poll_width = NULL;
    int* poll_height = NULL;
    bool closed = false;
    bool fullscreen = false;
    NSRect saved_frame = NSZeroRect;
    NSWindowStyleMask saved_style = 0;
    bool panning = false;
    float pan_x = 0;
    float pan_y = 0;
    float zoom = 1;
    float cursor_x = 0;
    float cursor_y = 0;
    bool turn_ccw = false;
    bool turn_cw = false;
    int keys = 0;
    SyphonServerBase* share = nil;
    id<NSObject> share_activity = nil;
    id<MTLTexture> model_texture = nil;
    id<MTLTexture> share_texture = nil;
    dispatch_source_t share_tick = nil;
    dispatch_semaphore_t share_wake = nil;
    bool share_off = false;
};

static void backing_size(window* w, int* width, int* height)
{
    NSSize size = [w->view convertSizeToBacking:w->view.bounds.size];
    *width = (int)size.width;
    *height = (int)size.height;
}

static void window_mouse(window* w, NSEvent* event)
{
    NSSize size = w->view.bounds.size;
    NSPoint at = [w->view convertPoint:event.locationInWindow fromView:nil];
    if (size.width <= 0 || size.height <= 0)
    {
        return;
    }
    if (event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeLeftMouseUp)
    {
        w->panning = event.type == NSEventTypeLeftMouseDown && NSPointInRect(at, w->view.bounds);
        return;
    }
    if (event.type == NSEventTypeLeftMouseDragged)
    {
        if (w->panning)
        {
            w->pan_x += event.deltaX / size.width;
            w->pan_y -= event.deltaY / size.height;
        }
        return;
    }
    if (event.type == NSEventTypeScrollWheel)
    {
        w->zoom *= expf(event.scrollingDeltaY * (event.hasPreciseScrollingDeltas ? 0.005f : 0.1f));
    }
    else
    {
        w->zoom *= 1 + event.magnification;
    }
    w->cursor_x = 2 * at.x / size.width - 1;
    w->cursor_y = 2 * at.y / size.height - 1;
}

#include "window_input.inc"

static void toggle_fullscreen(window* w)
{
    if ((w->ns.styleMask & NSWindowStyleMaskFullScreen) != 0)
    {
        [w->ns toggleFullScreen:nil];
        return;
    }
    if (w->fullscreen)
    {
        [w->ns setStyleMask:w->saved_style];
        [w->ns setFrame:w->saved_frame display:YES];
        [NSApp setPresentationOptions:NSApplicationPresentationDefault];
        [w->ns makeKeyAndOrderFront:nil];
        w->fullscreen = false;
        return;
    }
    NSScreen* screen = w->ns.screen != nil ? w->ns.screen : [NSScreen mainScreen];
    w->saved_style = w->ns.styleMask;
    w->saved_frame = w->ns.frame;
    [NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar];
    [w->ns setStyleMask:NSWindowStyleMaskBorderless];
    [w->ns setFrame:screen.frame display:YES];
    [w->ns makeKeyAndOrderFront:nil];
    w->fullscreen = true;
}

window* window_open(const char* title, int width, int height)
{
    @autoreleasepool
    {
        if (![NSThread isMainThread] || width <= 0 || height <= 0)
        {
            return NULL;
        }
        static bool app_started = false;
        if (!app_started)
        {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp finishLaunching];
            app_started = true;
        }
        window* w = new window();
        w->ns = [[hikari_window alloc] initWithContentRect:NSMakeRect(0, 0, width, height) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
        if (w->ns == nil)
        {
            delete w;
            return NULL;
        }
        w->ns.releasedWhenClosed = NO;
        w->ns.title = [NSString stringWithUTF8String:title];
        w->ns.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        w->target.layer = [CAMetalLayer layer];
        metal_layer_setup(w->target.layer);
        w->target.layer.displaySyncEnabled = YES;
        w->view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
        w->view.layer = w->target.layer;
        w->view.wantsLayer = YES;
        w->ns.contentView = w->view;
        [w->ns center];
        [w->ns makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        hikari_window* ns = w->ns;
        w->close_observer = [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:ns queue:nil usingBlock:^(NSNotification* note) {
            w->closed = true;
        }];
        w->resign_observer = [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResignKeyNotification object:ns queue:nil usingBlock:^(NSNotification* note) {
            w->turn_ccw = false;
            w->turn_cw = false;
        }];
        NSEventMask mouse = NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskLeftMouseDragged | NSEventMaskScrollWheel | NSEventMaskMagnify;
        w->monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp | mouse handler:^NSEvent*(NSEvent* event) {
            if (event.window != ns)
            {
                return event;
            }
            if (event.type != NSEventTypeKeyDown && event.type != NSEventTypeKeyUp)
            {
                window_mouse(w, event);
                return event;
            }
            if (event.type == NSEventTypeKeyUp)
            {
                if (event.keyCode == 12 || event.keyCode == 14)
                {
                    w->keys |= event.keyCode == 12 ? 4 : 8;
                    return nil;
                }
                return event;
            }
            if (event.keyCode == 12 && (event.modifierFlags & NSEventModifierFlagCommand) != 0)
            {
                ns.close_requested = YES;
                return nil;
            }
            if (event.keyCode == 12 || event.keyCode == 14)
            {
                (event.keyCode == 12 ? w->turn_ccw : w->turn_cw) = true;
                w->keys &= event.keyCode == 12 ? ~4 : ~8;
                return nil;
            }
            if (event.type == NSEventTypeKeyDown && (event.keyCode == 49 || (event.keyCode == 1 && (event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0)))
            {
                w->keys |= event.keyCode == 49 ? 1 : 2;
                return nil;
            }
            bool option = (event.modifierFlags & NSEventModifierFlagOption) != 0;
            if ((event.keyCode == 36 || event.keyCode == 76) && option)
            {
                toggle_fullscreen(w);
                return nil;
            }
            if (event.keyCode == 53)
            {
                if (w->fullscreen || (ns.styleMask & NSWindowStyleMaskFullScreen) != 0)
                {
                    toggle_fullscreen(w);
                }
                else
                {
                    ns.close_requested = YES;
                }
                return nil;
            }
            return event;
        }];
        w->timer = [NSTimer timerWithTimeInterval:0.001 repeats:YES block:^(NSTimer* timer) {
            if (w->frame != NULL)
            {
                backing_size(w, w->poll_width, w->poll_height);
                w->frame(w->context);
            }
        }];
        [[NSRunLoop currentRunLoop] addTimer:w->timer forMode:NSEventTrackingRunLoopMode];
        return w;
    }
}

bool window_poll(window* w, int* width, int* height, engine_hook frame, void* context)
{
    @autoreleasepool
    {
        w->frame = frame;
        w->context = context;
        w->poll_width = width;
        w->poll_height = height;
        NSEvent* event = nil;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES]) != nil)
        {
            [NSApp sendEvent:event];
        }
        w->frame = NULL;
        backing_size(w, width, height);
        return !w->closed && !w->ns.close_requested;
    }
}

int window_ask_save(window* w, const char* model)
{
    @autoreleasepool
    {
        if (w->closed)
        {
            return 0;
        }
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = [NSString stringWithFormat:@"Save changes to %s's view?", model];
        [alert addButtonWithTitle:@"Save"];
        [alert addButtonWithTitle:@"Don't Save"];
        [alert addButtonWithTitle:@"Cancel"];
        NSModalResponse response = [alert runModal];
        w->ns.close_requested = NO;
        [w->ns makeKeyAndOrderFront:nil];
        if (response == NSAlertFirstButtonReturn)
        {
            return 1;
        }
        if (response == NSAlertSecondButtonReturn)
        {
            return 0;
        }
        return -1;
    }
}

static void share_stop(window* w)
{
    if (w->share_tick != nil)
    {
        dispatch_source_cancel(w->share_tick);
        w->share_tick = nil;
    }
    if (w->share != nil)
    {
        [w->share stop];
        w->share = nil;
    }
    if (w->share_activity != nil)
    {
        [[NSProcessInfo processInfo] endActivity:w->share_activity];
        w->share_activity = nil;
    }
    w->model_texture = nil;
    w->share_texture = nil;
    w->share_off = true;
}

static bool share_ready(window* w, id<MTLDevice> device, int width, int height)
{
    if (w->share == nil)
    {
        w->share = [[SyphonServerBase alloc] initWithName:@"hikari" options:nil];
        if (w->share != nil)
        {
            w->share_activity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep reason:@"hikari syphon"];
        }
    }
    if (w->share != nil && (w->model_texture == nil || (int)w->model_texture.width != width || (int)w->model_texture.height != height))
    {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget;
        IOSurfaceRef surface = [w->share newSurfaceForWidth:width height:height options:nil];
        w->share_texture = surface != NULL ? [device newTextureWithDescriptor:descriptor iosurface:surface plane:0] : nil;
        if (surface != NULL)
        {
            CFRelease(surface);
        }
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModePrivate;
        w->model_texture = [device newTextureWithDescriptor:descriptor];
    }
    if (w->share == nil || w->share_texture == nil || w->model_texture == nil)
    {
        NSLog(@"engine: syphon share failed, drawing the window only");
        share_stop(w);
        return false;
    }
    return true;
}

void window_close(window* w)
{
    @autoreleasepool
    {
        if (w->target.last != nil)
        {
            [w->target.last waitUntilCompleted];
        }
        [w->timer invalidate];
        share_stop(w);
        [NSEvent removeMonitor:w->monitor];
        [[NSNotificationCenter defaultCenter] removeObserver:w->close_observer];
        [[NSNotificationCenter defaultCenter] removeObserver:w->resign_observer];
        if (w->fullscreen)
        {
            [NSApp setPresentationOptions:NSApplicationPresentationDefault];
        }
        if (!w->closed)
        {
            [w->ns close];
        }
        delete w;
        NSEvent* event = nil;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES]) != nil)
        {
            [NSApp sendEvent:event];
        }
    }
}

metal_target* window_target(window* w, float frame_seconds, bool* share, bool* hidden, int* width, int* height)
{
    backing_size(w, width, height);
    if (w->closed || *width <= 0 || *height <= 0)
    {
        return NULL;
    }
    *share = *share && !w->share_off;
    *hidden = w->ns.miniaturized || (w->ns.occlusionState & NSWindowOcclusionStateVisible) == 0;
    if (*hidden && !*share)
    {
        [NSThread sleepForTimeInterval:frame_seconds];
        return NULL;
    }
    if (*hidden && w->share_tick == nil)
    {
        dispatch_semaphore_t wake = dispatch_semaphore_create(0);
        w->share_wake = wake;
        w->share_tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, DISPATCH_TIMER_STRICT, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0));
        dispatch_source_set_event_handler(w->share_tick, ^{
            dispatch_semaphore_signal(wake);
        });
        dispatch_source_set_timer(w->share_tick, DISPATCH_TIME_NOW, (uint64_t)(frame_seconds * 1e9), 0);
        dispatch_resume(w->share_tick);
    }
    if (!*hidden && w->share_tick != nil)
    {
        dispatch_source_cancel(w->share_tick);
        w->share_tick = nil;
    }
    if (*hidden)
    {
        dispatch_semaphore_wait(w->share_wake, DISPATCH_TIME_FOREVER);
    }
    return &w->target;
}

bool window_share(window* w, metal_renderer* model, id<MTLCommandBuffer> command_buffer, const hfw::mat4& mvp, id<MTLTexture> back, id<CAMetalDrawable> drawable, int width, int height, float frame_seconds)
{
    if (!share_ready(w, command_buffer.device, width, height))
    {
        return false;
    }
    metal_renderer_draw(model, command_buffer, w->model_texture, mvp, nil, false);
    if (!metal_renderer_share(command_buffer, w->model_texture, back, drawable.texture, w->share_texture))
    {
        share_stop(w);
        return false;
    }
    SyphonServerBase* server = w->share;
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        [server publish];
    }];
    if (drawable != nil)
    {
        [command_buffer presentDrawable:drawable afterMinimumDuration:frame_seconds];
    }
    return true;
}
