#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include "embed.h"

@interface hikari_view : UIView
@end

@implementation hikari_view
+ (Class)layerClass
{
    return [CAMetalLayer class];
}
- (void)layoutSubviews
{
    [super layoutSubviews];
    hikari_resize((int)(self.bounds.size.width * self.contentScaleFactor), (int)(self.bounds.size.height * self.contentScaleFactor));
}
@end

@interface hikari_controller : UIViewController
@end

@implementation hikari_controller
- (BOOL)prefersStatusBarHidden
{
    return YES;
}
- (BOOL)prefersHomeIndicatorAutoHidden
{
    return YES;
}
@end

@interface hikari_scene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic) UIWindow* window;
@property(nonatomic) CADisplayLink* link;
@property(nonatomic) BOOL background;
@end

@implementation hikari_scene
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options
{
    UIWindowScene* window_scene = (UIWindowScene*)scene;
    self.window = [[UIWindow alloc] initWithWindowScene:window_scene];
    hikari_view* view = [[hikari_view alloc] initWithFrame:self.window.bounds];
    view.contentScaleFactor = window_scene.screen.nativeScale;
    hikari_controller* controller = [hikari_controller new];
    controller.view = view;
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
    int status = hikari_start(NSBundle.mainBundle.resourcePath.fileSystemRepresentation, 9396);
    CGSize size = view.bounds.size;
    if (status != 0 || !hikari_surface((__bridge void*)view.layer, (int)(size.width * view.contentScaleFactor), (int)(size.height * view.contentScaleFactor)))
    {
        NSLog(@"hikari: start failed (%d)", status);
        return;
    }
    self.link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame)];
    [self.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(pace) name:NSProcessInfoPowerStateDidChangeNotification object:nil];
    [self pace];
}
- (void)pace
{
    if (!NSThread.isMainThread)
    {
        [self performSelectorOnMainThread:@selector(pace) withObject:nil waitUntilDone:NO];
        return;
    }
    int fps = self.background ? 0 : NSProcessInfo.processInfo.lowPowerModeEnabled ? 30 : 60;
    self.link.paused = fps == 0;
    if (fps > 0)
    {
        self.link.preferredFrameRateRange = CAFrameRateRangeMake(fps, fps, fps);
    }
    hikari_rate(fps);
}
- (void)sceneDidEnterBackground:(UIScene*)scene
{
    self.background = YES;
    [self pace];
}
- (void)sceneWillEnterForeground:(UIScene*)scene
{
    self.background = NO;
    [self pace];
}
- (void)frame
{
    hikari_frame();
}
@end

@interface hikari_app : UIResponder <UIApplicationDelegate>
@end

@implementation hikari_app
@end

int main(int argc, char* argv[])
{
    @autoreleasepool
    {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([hikari_app class]));
    }
}
