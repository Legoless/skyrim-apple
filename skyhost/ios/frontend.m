#import "frontend.h"
#import "TouchControlsView.h"
#import "../startup.h"
#import "../hid.h"
#import "../skyhost.h"
#import "../audio_backend.h"
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <AVFAudio/AVFAudio.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>

static CAMetalLayer *g_layer;
static SkyTouchControlsView *g_controls;
static int (*g_run_guest)(void);
static BOOL g_guest_started;
static BOOL g_guest_finished;
static BOOL g_app_started;
static BOOL g_controls_test_mode;
static BOOL g_audio_interrupted;
static NSString *g_setup_message;
static dispatch_queue_t g_audio_queue;
static _Atomic uint64_t g_audio_generation;
static const char *(*g_setup_check)(void);
static UIView *g_setup_view;
static UILabel *g_setup_label;
static UIButton *g_setup_button;
static NSTimer *g_ui_state_timer;

@class SkyKeyboardSession;
static SkyKeyboardSession *g_keyboard;
static void cancel_keyboard(void);
static void update_setup_view(void);
static void sync_controls_visibility(void);
static BOOL touch_can_resume(void);

@interface SkyMetalView : UIView
@end
@implementation SkyMetalView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

@interface SkyGameViewController : UIViewController
@property(nonatomic, strong) SkyMetalView *metalView;
- (void)retrySetup:(id)sender;
@end
static SkyGameViewController *g_game_controller;

static void forward_menu_touch_point(unsigned phase, uint64_t finger, CGPoint view_point) {
    if (!g_game_controller || !g_game_controller.metalView) return;
    CGPoint pt = [g_game_controller.view convertPoint:view_point toView:g_game_controller.metalView];
    CGRect bounds = g_game_controller.metalView.bounds;
    float nx = bounds.size.width > 0 ? (float)(pt.x / bounds.size.width) : 0.5f;
    float ny = bounds.size.height > 0 ? (float)(pt.y / bounds.size.height) : 0.5f;
    sky_hid_menu_touch(phase, finger, nx, ny);
}

@implementation SkyGameViewController
- (void)loadView {
    UIView *view = [[UIView alloc] initWithFrame:UIScreen.mainScreen.bounds];
    view.backgroundColor = UIColor.blackColor;
    view.multipleTouchEnabled = YES;
    self.view = view;
    self.metalView = [[SkyMetalView alloc] initWithFrame:view.bounds];
    self.metalView.backgroundColor = UIColor.blackColor;
    self.metalView.multipleTouchEnabled = YES;
    [view addSubview:self.metalView];
    g_layer = (CAMetalLayer *)self.metalView.layer;
    g_layer.device = MTLCreateSystemDefaultDevice();
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_layer.framebufferOnly = NO;
    /* The game renders 1280x720 inside this backing. The existing NVN present
     * crop scales only the game's viewport to the layer's drawable. */
    g_layer.drawableSize = CGSizeMake(1600, 900);
    g_layer.contentsGravity = kCAGravityResizeAspect;
    g_layer.opaque = YES;
    g_controls = [[SkyTouchControlsView alloc] initWithFrame:view.bounds];
    g_controls.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    g_controls.userInteractionEnabled = NO;
    g_controls.hidden = !g_controls_test_mode;
    [view addSubview:g_controls];
    if (!g_ui_state_timer) {
        g_ui_state_timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer *timer) {
            (void)timer;
            sync_controls_visibility();
        }];
    }
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGSize size = self.view.bounds.size;
    CGFloat scale = MIN(size.width / 16.0, size.height / 9.0);
    CGSize game = CGSizeMake(scale * 16, scale * 9);
    self.metalView.frame = CGRectMake((size.width - game.width) * .5,
        (size.height - game.height) * .5, game.width, game.height);
    /* Keep the guest's pixel dimensions fixed when UIKit changes view scale. */
    g_layer.drawableSize = CGSizeMake(1600, 900);
    update_setup_view();
}
- (void)retrySetup:(id)sender {
    (void)sender;
    if (sky_ios_retry_setup())
        [UIApplication.sharedApplication.delegate applicationDidBecomeActive:UIApplication.sharedApplication];
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (void)viewWillTransitionToSize:(CGSize)size withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator {
    [g_controls cancelAllTouches];
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
}
- (void)dispatchMenuTouches:(NSSet<UITouch *> *)touches phase:(unsigned)phase {
    if (g_controls_test_mode || sky_ui_is_in_game() == 1 || !touch_can_resume()) return;
    for (UITouch *touch in touches)
        forward_menu_touch_point(phase, (uint64_t)(uintptr_t)touch, [touch locationInView:self.view]);
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self dispatchMenuTouches:touches phase:0];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self dispatchMenuTouches:touches phase:1];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self dispatchMenuTouches:touches phase:2];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self dispatchMenuTouches:touches phase:3];
}
@end

static BOOL touch_can_resume(void) {
    return !g_setup_message && !g_keyboard && !g_audio_interrupted && !g_guest_finished &&
        UIApplication.sharedApplication.applicationState == UIApplicationStateActive &&
        sky_oe_get_current_focus_state() == 1;
}

static void sync_controls_visibility(void) {
    if (!g_controls) return;
    [g_controls updateVisibilityInGame:sky_ui_is_in_game() != 0 testMode:g_controls_test_mode];
    if (g_setup_message) {
        if (!g_controls.hidden || g_controls.userInteractionEnabled) {
            [g_controls cancelAllTouches];
            g_controls.hidden = YES;
            g_controls.userInteractionEnabled = NO;
        }
        return;
    }
    if (g_controls_test_mode) {
        g_controls.hidden = NO;
        g_controls.userInteractionEnabled = touch_can_resume();
        return;
    }
    if (g_guest_finished) {
        g_controls.hidden = NO;
        g_controls.userInteractionEnabled = NO;
        return;
    }
    BOOL in_game = sky_ui_is_in_game() != 0;
    BOOL can_touch = in_game && touch_can_resume();
    if (g_controls.hidden != !in_game || g_controls.userInteractionEnabled != can_touch) {
        if (!can_touch) [g_controls cancelAllTouches];
        g_controls.hidden = !in_game;
        g_controls.userInteractionEnabled = can_touch;
    }
}

@interface SkyKeyboardSession : NSObject <UITextFieldDelegate>
@property(nonatomic, strong) dispatch_semaphore_t finished;
@property(nonatomic, strong) UIAlertController *alert;
@property(nonatomic, strong) UIAlertAction *confirm;
@property(nonatomic, strong) UITextField *field;
@property(nonatomic, copy) NSString *header, *message, *initial, *result;
@property(nonatomic, assign) size_t minimum, maximum, capacity;
@property(nonatomic, assign) BOOL completed, closing, accepted;
- (BOOL)validText;
- (void)textChanged:(UITextField *)field;
- (void)finishAccepted:(BOOL)accepted animated:(BOOL)animated;
@end

@implementation SkyKeyboardSession
- (BOOL)validText {
    NSString *text = self.field.text ?: @"";
    if (self.field.markedTextRange || text.length < self.minimum || text.length > self.maximum) return NO;
    if ([text rangeOfCharacterFromSet:[NSCharacterSet characterSetWithRange:NSMakeRange(0, 1)]].location != NSNotFound) return NO;
    NSData *encoded = [text dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
    return encoded && encoded.length < self.capacity;
}
- (void)textChanged:(UITextField *)field { (void)field; self.confirm.enabled = [self validText]; }
- (BOOL)textFieldShouldReturn:(UITextField *)field {
    (void)field;
    if (![self validText]) return NO;
    [self finishAccepted:YES animated:YES];
    return YES;
}
- (void)completeSession {
    if (self.completed) return;
    self.completed = YES;
    if (g_keyboard == self) g_keyboard = nil;
    self.field.delegate = nil;
    self.field = nil;
    self.confirm = nil;
    self.alert = nil;
    sync_controls_visibility();
    dispatch_semaphore_signal(self.finished);
}
- (void)finishAccepted:(BOOL)accepted animated:(BOOL)animated {
    if (self.completed || self.closing || (accepted && ![self validText])) return;
    self.closing = YES;
    self.accepted = accepted;
    self.result = accepted ? self.field.text : nil;
    UIAlertController *alert = self.alert;
    if (animated && alert.presentingViewController) {
        [alert dismissViewControllerAnimated:YES completion:^{ [self completeSession]; }];
    } else {
        [alert dismissViewControllerAnimated:NO completion:nil];
        [self completeSession];
    }
}
@end

static void cancel_keyboard(void) {
    /* Also finish an in-flight animated dismissal before background suspension. */
    SkyKeyboardSession *session = g_keyboard;
    if (!session) return;
    session.accepted = NO;
    session.result = nil;
    session.closing = NO;
    [session finishAccepted:NO animated:NO];
}

static void present_keyboard(SkyKeyboardSession *session) {
    UIViewController *controller = g_game_controller;
    if (g_keyboard || g_setup_message || !controller.view.window || controller.presentedViewController ||
        g_audio_interrupted || g_guest_finished ||
        UIApplication.sharedApplication.applicationState != UIApplicationStateActive) {
        [session completeSession];
        return;
    }
    g_keyboard = session;
    [g_controls cancelAllTouches];
    g_controls.userInteractionEnabled = NO;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:session.header.length ? session.header : @"Enter text"
        message:session.message preferredStyle:UIAlertControllerStyleAlert];
    alert.view.accessibilityIdentifier = @"sky.keyboard.dialog";
    session.alert = alert;
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        session.field = field;
        field.text = session.initial;
        field.delegate = session;
        field.accessibilityIdentifier = @"sky.keyboard.text";
        field.autocorrectionType = UITextAutocorrectionTypeNo;
        field.spellCheckingType = UITextSpellCheckingTypeNo;
        field.autocapitalizationType = UITextAutocapitalizationTypeNone;
        field.returnKeyType = UIReturnKeyDone;
        field.clearButtonMode = UITextFieldViewModeWhileEditing;
        [field addTarget:session action:@selector(textChanged:) forControlEvents:UIControlEventEditingChanged];
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action) {
        (void)action; [session finishAccepted:NO animated:YES];
    }]];
    session.confirm = [UIAlertAction actionWithTitle:@"Done" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        (void)action; [session finishAccepted:YES animated:YES];
    }];
    [alert addAction:session.confirm];
    [session textChanged:session.field];
    [controller presentViewController:alert animated:YES completion:^{
        if (!session.completed && !session.closing) [session.field becomeFirstResponder];
    }];
}

int sky_ios_present_keyboard(const sky_keyboard_request_t *request, char *out_utf8, size_t capacity) {
    if (NSThread.isMainThread || !request || !out_utf8 || !capacity) return 0;
    out_utf8[0] = 0;
    @autoreleasepool {
        SkyKeyboardSession *session = [SkyKeyboardSession new];
        session.finished = dispatch_semaphore_create(0);
        session.header = request->header ? [NSString stringWithUTF8String:request->header] : @"";
        NSString *subtext = request->subtext ? [NSString stringWithUTF8String:request->subtext] : @"";
        NSString *guide = request->guide ? [NSString stringWithUTF8String:request->guide] : @"";
        session.message = [@[subtext ?: @"", guide ?: @""] componentsJoinedByString:@"\n"];
        session.initial = request->initial_text ? [NSString stringWithUTF8String:request->initial_text] : @"";
        session.minimum = request->min_length;
        session.maximum = request->max_length;
        session.capacity = capacity;
        dispatch_async(dispatch_get_main_queue(), ^{ present_keyboard(session); });
        dispatch_semaphore_wait(session.finished, DISPATCH_TIME_FOREVER);
        if (!session.accepted) return 0;
        NSData *encoded = [session.result dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
        if (!encoded || encoded.length >= capacity) return 0;
        memcpy(out_utf8, encoded.bytes, encoded.length);
        out_utf8[encoded.length] = 0;
        return 1;
    }
}
int sky_ios_keyboard_is_presented(void) {
    return NSThread.isMainThread && g_keyboard && !g_keyboard.completed && !g_keyboard.closing &&
        g_keyboard.alert.presentingViewController != nil && !g_keyboard.alert.isBeingPresented &&
        g_keyboard.alert.view.window != nil;
}
int sky_ios_keyboard_respond(const char *text_utf8, int accepted) {
    if (!sky_ios_keyboard_is_presented()) return 0;
    if (accepted) {
        NSString *text = text_utf8 ? [NSString stringWithUTF8String:text_utf8] : nil;
        if (!text) return 0;
        g_keyboard.field.text = text;
        [g_keyboard.field sendActionsForControlEvents:UIControlEventEditingChanged];
        if (![g_keyboard validText]) return 0;
    }
    [g_keyboard finishAccepted:accepted != 0 animated:NO];
    return 1;
}

static void update_setup_view(void) {
    if (!g_game_controller.isViewLoaded) return;
    if (!g_setup_message) {
        [g_setup_view removeFromSuperview];
        g_setup_view = nil; g_setup_label = nil; g_setup_button = nil;
        sync_controls_visibility();
        return;
    }
    sync_controls_visibility();
    UIApplication.sharedApplication.idleTimerDisabled = NO;
    if (!g_setup_view) {
        g_setup_view = [[UIView alloc] initWithFrame:g_game_controller.view.bounds];
        g_setup_view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        g_setup_label = [UILabel new];
        g_setup_label.numberOfLines = 0;
        g_setup_label.font = [UIFont systemFontOfSize:20];
        g_setup_label.textAlignment = NSTextAlignmentCenter;
        g_setup_label.textColor = UIColor.whiteColor;
        g_setup_label.accessibilityIdentifier = @"sky.setup.message";
        [g_setup_view addSubview:g_setup_label];
        g_setup_button = [UIButton buttonWithType:UIButtonTypeSystem];
        [g_setup_button setTitle:@"Retry" forState:UIControlStateNormal];
        g_setup_button.titleLabel.font = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold];
        g_setup_button.tintColor = UIColor.whiteColor;
        g_setup_button.accessibilityIdentifier = @"sky.setup.retry";
        [g_setup_button addTarget:g_game_controller action:@selector(retrySetup:) forControlEvents:UIControlEventTouchUpInside];
        [g_setup_view addSubview:g_setup_button];
        [g_game_controller.view addSubview:g_setup_view];
    }
    g_setup_label.text = g_setup_message;
    CGFloat width = MAX(0, MIN(760, g_game_controller.view.bounds.size.width - 96));
    CGSize text = [g_setup_label sizeThatFits:CGSizeMake(width, CGFLOAT_MAX)];
    CGFloat height = text.height + 68;
    CGFloat top = MAX(24, (g_game_controller.view.bounds.size.height - height) * .5);
    CGFloat left = (g_game_controller.view.bounds.size.width - width) * .5;
    g_setup_label.frame = CGRectMake(left, top, width, text.height);
    g_setup_button.frame = CGRectMake(left, top + text.height + 24, width, 44);
    g_setup_button.hidden = g_setup_check == NULL;
}

static void *run_guest_thread(void *context) {
    (void)context;
    @autoreleasepool {
        fprintf(stderr, "[skyhost] GUEST_HOST_STACK size=%zu platform=ios\n", pthread_get_stacksize_np(pthread_self()));
        int result = g_run_guest();
        fprintf(stderr, "[skyhost] IOS_GUEST_EXIT result=%d\n", result);
        dispatch_async(dispatch_get_main_queue(), ^{
            g_guest_finished = YES;
            UIApplication.sharedApplication.idleTimerDisabled = NO;
            cancel_keyboard();
            atomic_fetch_add(&g_audio_generation, 1);
            dispatch_async(g_audio_queue, ^{ sky_audio_backend_set_host_active(0); });
            [g_controls cancelAllTouches];
            sky_oe_set_host_focus(0);
            sync_controls_visibility();
            UILabel *message = [[UILabel alloc] initWithFrame:g_controls.bounds];
            message.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            message.text = result ? @"The game stopped." : @"The game finished.";
            message.textAlignment = NSTextAlignmentCenter;
            message.textColor = UIColor.whiteColor;
            message.backgroundColor = [UIColor colorWithWhite:0 alpha:.7];
            [g_controls addSubview:message];
        });
    }
    return NULL;
}

@interface SkySceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@interface SkyAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation SkyAppDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)application; (void)options;
    g_audio_queue = dispatch_queue_create("dev.skyhost.audio-session", DISPATCH_QUEUE_SERIAL);
    NSError *audioError = nil;
    AVAudioSession *session = AVAudioSession.sharedInstance;
    if (![session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&audioError] ||
        ![session setPreferredSampleRate:48000 error:&audioError]) {
        fprintf(stderr, "[skyhost] IOS_AUDIO_SESSION_ERROR operation=configure reason=%s\n",
            audioError.localizedDescription.UTF8String);
        return NO;
    }
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(audioSessionInterruption:)
        name:AVAudioSessionInterruptionNotification object:session];
    return YES;
}
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)application; (void)options;
    UISceneConfiguration *configuration = [[UISceneConfiguration alloc] initWithName:@"Skyrim" sessionRole:session.role];
    configuration.delegateClass = SkySceneDelegate.class;
    return configuration;
}
- (void)activateHostAudioAndStart {
    if (g_audio_interrupted || g_guest_finished || !g_layer.device) return;
    uint64_t generation = atomic_fetch_add(&g_audio_generation, 1) + 1;
    /* Session activation and RemoteIO start can wait on audio services. Keep
     * both off UIKit's main thread, including on iOS versions before async
     * AVAudioSession activation existed. The serial queue orders pause/resume. */
    dispatch_async(g_audio_queue, ^{
        NSError *audioError = nil;
        BOOL activated = [AVAudioSession.sharedInstance setActive:YES error:&audioError];
        if (activated && atomic_load(&g_audio_generation) == generation)
            sky_audio_backend_set_host_active(1);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (atomic_load(&g_audio_generation) != generation) return;
            if (!activated) {
                UIApplication.sharedApplication.idleTimerDisabled = NO;
                fprintf(stderr, "[skyhost] IOS_AUDIO_SESSION_ERROR operation=activate reason=%s\n",
                    audioError.localizedDescription.UTF8String);
                return;
            }
            sky_oe_set_host_focus(1);
            sync_controls_visibility();
            [self startGuestIfNeeded];
            /* The intro can run for minutes without input. Keep only this app
             * awake while the foreground game (or ready controls test) runs. */
            UIApplication.sharedApplication.idleTimerDisabled = !g_setup_message &&
                !g_guest_finished && (g_guest_started || g_controls_test_mode);
        });
    });
}
- (void)pauseHostAudio {
    UIApplication.sharedApplication.idleTimerDisabled = NO;
    atomic_fetch_add(&g_audio_generation, 1);
    dispatch_async(g_audio_queue, ^{ sky_audio_backend_set_host_active(0); });
}
- (void)audioSessionInterruption:(NSNotification *)notification {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{ [self audioSessionInterruption:notification]; });
        return;
    }
    AVAudioSessionInterruptionType type = [notification.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
    if (type == AVAudioSessionInterruptionTypeBegan) {
        g_audio_interrupted = YES;
        [self pauseHostAudio];
        [g_controls cancelAllTouches];
        g_controls.userInteractionEnabled = NO;
        sky_oe_set_host_focus(0);
        cancel_keyboard();
    } else if (type == AVAudioSessionInterruptionTypeEnded) {
        g_audio_interrupted = NO;
        if (g_guest_finished || UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;
        AVAudioSessionInterruptionOptions options = [notification.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
        if (options & AVAudioSessionInterruptionOptionShouldResume) [self activateHostAudioAndStart];
        else {
            sky_oe_set_host_focus(1);
            sync_controls_visibility();
            UIApplication.sharedApplication.idleTimerDisabled = !g_setup_message &&
                (g_guest_started || g_controls_test_mode);
        }
    }
}
- (void)applicationDidBecomeActive:(UIApplication *)application {
    (void)application;
    if (g_setup_message && !sky_ios_retry_setup()) return;
    [self activateHostAudioAndStart];
}
- (void)startGuestIfNeeded {
    if (g_setup_message) { g_controls.userInteractionEnabled = NO; return; }
    if (g_controls_test_mode || g_guest_started || !g_run_guest) return;
    g_guest_started = YES;
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (!error) {
        error = pthread_attr_setstacksize(&attributes, 8u * 1024u * 1024u);
        if (!error) error = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        pthread_t guest;
        if (!error) error = pthread_create(&guest, &attributes, run_guest_thread, NULL);
        pthread_attr_destroy(&attributes);
    }
    if (error) {
        g_guest_started = NO;
        fprintf(stderr, "[skyhost] IOS_WINDOW_ERROR reason=create-game-thread code=%d\n", error);
        sky_ios_set_setup_message("Unable to start the game. Tap Retry to try again.");
    }
}
- (void)applicationWillResignActive:(UIApplication *)application {
    (void)application;
    [self pauseHostAudio];
    [g_controls cancelAllTouches];
    g_controls.userInteractionEnabled = NO;
    sky_oe_set_host_focus(0);
    cancel_keyboard();
}
- (void)applicationDidEnterBackground:(UIApplication *)application {
    (void)application;
    [self pauseHostAudio];
    [g_controls cancelAllTouches];
    sky_oe_set_host_focus(0);
    cancel_keyboard();
    dispatch_async(g_audio_queue, ^{
        NSError *error = nil;
        if (![AVAudioSession.sharedInstance setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:&error])
            fprintf(stderr, "[skyhost] IOS_AUDIO_SESSION_ERROR operation=deactivate reason=%s\n", error.localizedDescription.UTF8String);
    });
}
- (void)applicationWillTerminate:(UIApplication *)application {
    (void)application;
    [self pauseHostAudio];
    [g_controls cancelAllTouches];
    sky_oe_set_host_focus(0);
    cancel_keyboard();
}
@end

@implementation SkySceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    SkyAppDelegate *appDelegate = (SkyAppDelegate *)UIApplication.sharedApplication.delegate;
    appDelegate.window.hidden = YES;
    appDelegate.window.rootViewController = nil;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    /* A guest NVN window keeps the Metal-layer identity. Reuse its controller
     * when UIKit reconnects the single scene instead of orphaning that layer. */
    if (!g_game_controller) g_game_controller = [SkyGameViewController new];
    self.window.rootViewController = g_game_controller;
    [self.window makeKeyAndVisible];
    [self.window layoutIfNeeded];
    appDelegate.window = self.window;
    UIWindowSceneGeometryPreferencesIOS *geometry = [[UIWindowSceneGeometryPreferencesIOS alloc]
        initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
    [(UIWindowScene *)scene requestGeometryUpdateWithPreferences:geometry errorHandler:^(NSError *error) {
        fprintf(stderr, "[skyhost] IOS_ORIENTATION_ERROR reason=%s\n", error.localizedDescription.UTF8String);
    }];
    sky_oe_set_host_focus(0);
    if (!g_layer.device) {
        fprintf(stderr, "[skyhost] IOS_WINDOW_ERROR reason=no-metal-device\n");
        return;
    }
    update_setup_view();
    if (g_setup_message) fprintf(stderr, "[skyhost] IOS_SETUP_REQUIRED guest=not-started\n");
    fprintf(stderr, "[skyhost] IOS_WINDOW_READY drawable=1600x900 input=multi-touch\n");
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    (void)scene;
    [(SkyAppDelegate *)UIApplication.sharedApplication.delegate applicationDidBecomeActive:UIApplication.sharedApplication];
}
- (void)sceneWillResignActive:(UIScene *)scene {
    (void)scene;
    [(SkyAppDelegate *)UIApplication.sharedApplication.delegate applicationWillResignActive:UIApplication.sharedApplication];
}
- (void)sceneDidEnterBackground:(UIScene *)scene {
    (void)scene;
    [(SkyAppDelegate *)UIApplication.sharedApplication.delegate applicationDidEnterBackground:UIApplication.sharedApplication];
}
- (void)sceneDidDisconnect:(UIScene *)scene {
    (void)scene;
    [g_controls cancelAllTouches];
    [(SkyAppDelegate *)UIApplication.sharedApplication.delegate pauseHostAudio];
    sky_oe_set_host_focus(0);
    cancel_keyboard();
}
@end

void *sky_ios_metal_layer(void) { return (__bridge void *)g_layer; }
void sky_ios_set_setup_message(const char *message) {
    if (!g_guest_started && NSThread.isMainThread) {
        g_setup_message = message ? [NSString stringWithUTF8String:message] : nil;
        update_setup_view();
    }
}
void sky_ios_set_setup_check(const char *(*check)(void)) {
    if (!g_guest_started && NSThread.isMainThread) { g_setup_check = check; update_setup_view(); }
}
int sky_ios_retry_setup(void) {
    if (!NSThread.isMainThread || g_guest_started) return 0;
    if (g_setup_check) sky_ios_set_setup_message(g_setup_check());
    if (g_setup_message) return 0;
    return 1;
}
int sky_ios_run(int (*run_guest)(void)) {
    if (!NSThread.isMainThread || g_app_started) return 1;
    @autoreleasepool {
        g_controls_test_mode = [NSProcessInfo.processInfo.arguments containsObject:@"--controls-test-mode"] ||
            [NSProcessInfo.processInfo.environment[@"SKY_CONTROLS_TEST_MODE"] isEqualToString:@"1"];
        if (!run_guest && !g_controls_test_mode) return 1;
        g_app_started = YES;
        g_run_guest = run_guest;
        sky_keyboard_set_presenter(sky_ios_present_keyboard);
        if (g_controls_test_mode)
            fprintf(stderr, "[skyhost] IOS_CONTROLS_TEST_MODE guest=not-started scope=UIKit-controls-only\n");
        char name[] = "Skyrim";
        char *argv[] = {name, NULL};
        return UIApplicationMain(1, argv, nil, NSStringFromClass(SkyAppDelegate.class));
    }
}
int sky_ios_touch_inject(unsigned phase, unsigned long long finger, float x, float y) {
    if (!NSThread.isMainThread || !g_controls) return 0;
    sync_controls_visibility();
    if (!g_controls_test_mode && (sky_ui_is_in_game() == 0 ||
        (sky_ui_is_in_game() == 2 && ![g_controls pointInside:CGPointMake(x, y) withEvent:nil]))) {
        if (!touch_can_resume()) return 0;
        forward_menu_touch_point(phase, finger, CGPointMake(x, y));
        return 1;
    }
    return [g_controls injectTouchPhase:phase identifier:finger point:CGPointMake(x, y)];
}
int sky_ios_touch_snapshot(struct sky_ios_touch_snapshot *out) {
    if (!NSThread.isMainThread || !g_controls || !out) return 0;
    sky_touch_t state;
    [g_controls touchSnapshot:&state];
    *out = (struct sky_ios_touch_snapshot){state.buttons, state.lx, state.ly, state.rx, state.ry};
    return 1;
}
