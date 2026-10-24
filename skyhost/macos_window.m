#import "macos_window.h"
#import "input.h"
#import "hid.h"
#import "startup.h"
#import "skyhost.h"
#import "keyboard.h"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CAMetalLayer *g_layer;
static int (*g_run_guest)(void);
static int g_guest_result;
static NSWindow *g_keyboard_window;

@interface SkyGameView : NSView
@property(nonatomic, assign) sky_input_t input;
@property(nonatomic, strong) NSTimer *inputTimer;
@property(nonatomic, assign) BOOL mouseCaptured;
@property(nonatomic, assign) BOOL cursorHidden;
@property(nonatomic, assign) BOOL cursorDisassociated;
- (BOOL)setMouseLook:(BOOL)enabled;
- (void)resetInput;
@end

@interface SkyKeyboardSession : NSObject <NSTextFieldDelegate>
@property(nonatomic, strong) NSAlert *alert;
@property(nonatomic, strong) NSTextField *field;
@property(nonatomic, assign) size_t minimum;
@property(nonatomic, assign) size_t maximum;
@property(nonatomic, assign) size_t capacity;
- (BOOL)validText;
@end

@implementation SkyKeyboardSession
- (BOOL)validText {
    NSString *text = self.field.stringValue;
    NSText *editor = self.field.currentEditor;
    if ([editor isKindOfClass:NSTextView.class] && [(NSTextView *)editor hasMarkedText]) return NO;
    NSData *utf8 = [text dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
    for (NSUInteger index = 0; index < text.length; index++)
        if ([text characterAtIndex:index] == 0) return NO;
    return utf8 && text.length >= self.minimum && text.length <= self.maximum &&
        utf8.length < self.capacity;
}
- (void)controlTextDidChange:(NSNotification *)notification {
    (void)notification;
    self.alert.buttons.firstObject.enabled = [self validText];
}
@end

/* The SDK calls this on the guest thread. Only that thread waits; AppKit keeps
 * processing the sheet, focus changes and cancellation on the main thread. */
static int present_keyboard(const sky_keyboard_request_t *request, char *output, size_t capacity) {
    if (NSThread.isMainThread || !request || !output || !capacity) return 0;
    @autoreleasepool {
        NSString *header = request->header ? [NSString stringWithUTF8String:request->header] : nil;
        NSString *subtext = request->subtext ? [NSString stringWithUTF8String:request->subtext] : nil;
        NSString *guide = request->guide ? [NSString stringWithUTF8String:request->guide] : nil;
        NSString *initial = request->initial_text ? [NSString stringWithUTF8String:request->initial_text] : nil;
        size_t minimum = request->min_length, maximum = request->max_length;
        dispatch_semaphore_t finished = dispatch_semaphore_create(0);
        __block int accepted = 0;
        dispatch_async(dispatch_get_main_queue(), ^{
            NSWindow *window = g_keyboard_window;
            if (!window || !NSApp.isActive || window.attachedSheet) {
                dispatch_semaphore_signal(finished);
                return;
            }
            if ([window.contentView isKindOfClass:SkyGameView.class])
                [(SkyGameView *)window.contentView resetInput];
            SkyKeyboardSession *session = [SkyKeyboardSession new];
            session.minimum = minimum; session.maximum = maximum; session.capacity = capacity;
            session.alert = [NSAlert new];
            session.alert.messageText = header.length ? header : @"Enter text";
            NSMutableArray<NSString *> *lines = [NSMutableArray new];
            if (subtext.length) [lines addObject:subtext];
            if (guide.length) [lines addObject:guide];
            session.alert.informativeText = [lines componentsJoinedByString:@"\n"];
            [session.alert addButtonWithTitle:@"OK"];
            [session.alert addButtonWithTitle:@"Cancel"];
            session.field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 360, 24)];
            session.field.stringValue = initial ?: @"";
            session.field.delegate = session;
            session.field.accessibilityIdentifier = @"sky.keyboard.text";
            session.alert.accessoryView = session.field;
            session.alert.buttons.firstObject.enabled = [session validText];
            NSAlert *alert = session.alert;
            id resign = [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidResignActiveNotification
                object:NSApp queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
                    (void)notification;
                    [window endSheet:alert.window returnCode:NSModalResponseCancel];
                }];
            [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
                [NSNotificationCenter.defaultCenter removeObserver:resign];
                if (response == NSAlertFirstButtonReturn && [session validText]) {
                    NSData *utf8 = [session.field.stringValue dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
                    memcpy(output, utf8.bytes, utf8.length);
                    output[utf8.length] = 0;
                    accepted = 1;
                }
                session.alert = nil;
                dispatch_semaphore_signal(finished);
            }];
            [alert.window makeFirstResponder:session.field];
        });
        dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);
        return accepted;
    }
}

static unsigned input_modifiers(NSEventModifierFlags flags) {
    return ((flags & NSEventModifierFlagShift) ? SKY_INPUT_MOD_SHIFT : 0) |
        ((flags & NSEventModifierFlagControl) ? SKY_INPUT_MOD_CTRL : 0) |
        ((flags & NSEventModifierFlagOption) ? SKY_INPUT_MOD_ALT : 0) |
        ((flags & NSEventModifierFlagCommand) ? SKY_INPUT_MOD_COMMAND : 0);
}

@implementation SkyGameView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return NO; }
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self.inputTimer invalidate];
    self.inputTimer = nil;
    if (!self.window) {
        [self resetInput];
        return;
    }
    self.window.acceptsMouseMovedEvents = YES;
    const char *configured = getenv("SKY_MOUSE_SENSITIVITY");
    if (configured) {
        char *end = NULL;
        double sensitivity = strtod(configured, &end);
        if (end != configured && !*end) sky_input_set_mouse_sensitivity(&_input, sensitivity);
    }
    __weak SkyGameView *weakView = self;
    self.inputTimer = [NSTimer timerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer *timer) {
        SkyGameView *view = weakView;
        if (!view) { [timer invalidate]; return; }
        sky_input_tick(&view->_input, now_ns());
        [view publishInput];
    }];
    [[NSRunLoop mainRunLoop] addTimer:self.inputTimer forMode:NSRunLoopCommonModes];
    [self updateControlsTitle];
}
- (void)updateControlsTitle {
    self.window.title = self.mouseCaptured ? @"Skyrim — Mouse look · F1/Esc releases" :
        @"Skyrim — Arrows/wheel + Enter/click select · F1 mouse look";
}
- (void)publishInput {
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    sky_input_state(&_input, &buttons, &lx, &ly, &rx, &ry);
    sky_hid_queue_source_state(SKY_HID_SOURCE_DESKTOP, buttons, lx, ly, rx, ry);
}
- (BOOL)setMouseLook:(BOOL)enabled {
    if (enabled == self.mouseCaptured) return YES;
    if (enabled) {
        if (!self.window.isKeyWindow || !NSApp.isActive) return NO;
        CGError error = CGAssociateMouseAndMouseCursorPosition(false);
        if (error != kCGErrorSuccess) {
            LOG("INPUT_CAPTURE_ERROR code=%d", error);
            return NO;
        }
        self.cursorDisassociated = YES;
        [NSCursor hide];
        self.cursorHidden = YES;
    } else {
        if (self.cursorDisassociated) CGAssociateMouseAndMouseCursorPosition(true);
        if (self.cursorHidden) [NSCursor unhide];
        self.cursorDisassociated = self.cursorHidden = NO;
    }
    self.mouseCaptured = enabled;
    sky_input_set_mouse_captured(&_input, enabled);
    [self updateControlsTitle];
    [self publishInput];
    LOG("INPUT_CAPTURE enabled=%d", enabled);
    return YES;
}
- (void)resetInput {
    [self setMouseLook:NO];
    sky_input_reset(&_input);
    sky_hid_set_source_state(SKY_HID_SOURCE_DESKTOP, 0, 0, 0, 0, 0);
}
- (void)keyDown:(NSEvent *)event {
    fprintf(stderr, "[skyhost] INPUT_KEY code=%u down=1 repeat=%d\n", event.keyCode, event.isARepeat);
    sky_input_set_modifiers(&_input, input_modifiers(event.modifierFlags));
    if (event.keyCode == 122) { /* F1 is reserved for host mouse capture. */
        if (!event.isARepeat && !(event.modifierFlags & NSEventModifierFlagCommand))
            [self setMouseLook:!self.mouseCaptured];
        return;
    }
    sky_input_key(&_input, event.keyCode, 1);
    if (event.keyCode == 53 || event.keyCode == 48) [self setMouseLook:NO];
    [self publishInput];
}
- (void)keyUp:(NSEvent *)event {
    fprintf(stderr, "[skyhost] INPUT_KEY code=%u down=0\n", event.keyCode);
    sky_input_set_modifiers(&_input, input_modifiers(event.modifierFlags));
    sky_input_key(&_input, event.keyCode, 0);
    [self publishInput];
}
- (void)flagsChanged:(NSEvent *)event {
    sky_input_set_modifiers(&_input, input_modifiers(event.modifierFlags));
    if (event.modifierFlags & NSEventModifierFlagCommand) [self setMouseLook:NO];
    [self publishInput];
}
- (void)mouseMoved:(NSEvent *)event {
    if (self.mouseCaptured) sky_input_mouse_motion(&_input, event.deltaX, event.deltaY, now_ns());
}
- (void)mouseDragged:(NSEvent *)event { [self mouseMoved:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self mouseMoved:event]; }
- (void)otherMouseDragged:(NSEvent *)event { [self mouseMoved:event]; }
- (void)mouseDown:(NSEvent *)event {
    (void)event;
    sky_input_mouse_button(&_input, 0, 1);
    [self publishInput];
}
- (void)mouseUp:(NSEvent *)event {
    (void)event;
    sky_input_mouse_button(&_input, 0, 0);
    [self publishInput];
}
- (void)rightMouseDown:(NSEvent *)event {
    (void)event;
    sky_input_mouse_button(&_input, 1, 1);
    [self publishInput];
}
- (void)rightMouseUp:(NSEvent *)event {
    (void)event;
    sky_input_mouse_button(&_input, 1, 0);
    [self publishInput];
}
- (void)otherMouseDown:(NSEvent *)event {
    if (event.buttonNumber == 2) { sky_input_mouse_button(&_input, 2, 1); [self publishInput]; }
}
- (void)otherMouseUp:(NSEvent *)event {
    if (event.buttonNumber == 2) { sky_input_mouse_button(&_input, 2, 0); [self publishInput]; }
}
- (void)scrollWheel:(NSEvent *)event {
    double lines = event.scrollingDeltaY / (event.hasPreciseScrollingDeltas ? 10.0 : 1.0);
    sky_input_scroll(&_input, lines, now_ns());
    [self publishInput];
}
- (BOOL)resignFirstResponder {
    [self resetInput];
    return YES;
}
- (void)dealloc {
    [_inputTimer invalidate];
    if (_cursorDisassociated) CGAssociateMouseAndMouseCursorPosition(true);
    if (_cursorHidden) [NSCursor unhide];
}
@end

@interface SkyWindowDelegate : NSObject <NSWindowDelegate, NSApplicationDelegate>
@end

@implementation SkyWindowDelegate
- (void)windowDidBecomeKey:(NSNotification *)notification {
    NSWindow *window = notification.object;
    [window makeFirstResponder:window.contentView];
    sky_oe_set_host_focus(1);
    fprintf(stderr, "[skyhost] INPUT_FOCUS focused=1 responder=%s\n",
        NSStringFromClass(window.firstResponder.class).UTF8String);
}
- (BOOL)windowShouldClose:(NSWindow *)window {
    [(SkyGameView *)window.contentView resetInput];
    [NSApp terminate:nil];
    return YES;
}
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    for (NSWindow *window in NSApp.windows)
        if ([window.contentView isKindOfClass:SkyGameView.class])
            [(SkyGameView *)window.contentView resetInput];
}
- (void)windowDidResignKey:(NSNotification *)notification {
    sky_oe_set_host_focus(0);
    fprintf(stderr, "[skyhost] INPUT_FOCUS focused=0\n");
    NSWindow *window = notification.object;
    SkyGameView *view = (SkyGameView *)window.contentView;
    [view resetInput];
}
@end

void *sky_macos_metal_layer(void) {
    return (__bridge void *)g_layer;
}

static void *run_guest(void *context) {
    (void)context;
    @autoreleasepool {
        fprintf(stderr, "[skyhost] GUEST_HOST_STACK size=%zu\n", pthread_get_stacksize_np(pthread_self()));
        g_guest_result = g_run_guest();
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp stop:nil];
            NSEvent *wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0
                context:nil subtype:0 data1:0 data2:0];
            [NSApp postEvent:wake atStart:YES];
        });
    }
    return NULL;
}

int sky_macos_run(int (*run_game)(void)) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSMenu *menu = [NSMenu new];
        NSMenuItem *application = [NSMenuItem new];
        NSMenu *applicationMenu = [NSMenu new];
        [applicationMenu addItemWithTitle:@"Quit Skyrim" action:@selector(terminate:) keyEquivalent:@"q"];
        application.submenu = applicationMenu;
        [menu addItem:application];
        NSApp.mainMenu = menu;

        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        window.title = @"Skyrim";
        __attribute__((objc_precise_lifetime)) SkyWindowDelegate *delegate = [SkyWindowDelegate new];
        window.delegate = delegate;
        NSApp.delegate = delegate;
        SkyGameView *view = [[SkyGameView alloc] initWithFrame:window.contentView.bounds];
        view.wantsLayer = YES;
        g_layer = [CAMetalLayer layer];
        g_layer.device = MTLCreateSystemDefaultDevice();
        if (!g_layer.device) {
            fprintf(stderr, "[skyhost] WINDOW_ERROR reason=no-metal-device\n");
            return 1;
        }
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.framebufferOnly = NO;
        g_layer.drawableSize = CGSizeMake(1600, 900);
        g_layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        view.layer = g_layer;
        window.contentView = view;
        g_keyboard_window = window;
        sky_keyboard_set_presenter(present_keyboard);
        window.initialFirstResponder = view;
        [window center];
        [window makeKeyAndOrderFront:nil];
        [window makeFirstResponder:view];
        [NSApp finishLaunching];
        [NSApp activate];
        [window makeFirstResponder:view];
        fprintf(stderr, "[skyhost] WINDOW_READY drawable=1600x900 input=keyboard-mouse\n");
        g_run_guest = run_game;
        pthread_t guest;
        pthread_attr_t attributes;
        int error = pthread_attr_init(&attributes);
        if (error) {
            fprintf(stderr, "[skyhost] WINDOW_ERROR reason=create-thread-attributes code=%d\n", error);
            return 1;
        }
        /* Darwin's default pthread stack is much smaller than the original
           process main stack on which nnMain used to run. */
        error = pthread_attr_setstacksize(&attributes, 8u * 1024u * 1024u);
        if (!error) error = pthread_create(&guest, &attributes, run_guest, NULL);
        pthread_attr_destroy(&attributes);
        if (error) {
            fprintf(stderr, "[skyhost] WINDOW_ERROR reason=create-game-thread\n");
            return 1;
        }
        [NSApp run];
        pthread_join(guest, NULL);
        [window orderOut:nil];
        sky_keyboard_set_presenter(NULL);
        g_keyboard_window = nil;
        g_layer = nil;
        return g_guest_result;
    }
}
