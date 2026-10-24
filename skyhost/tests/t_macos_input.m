#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include "skyhost.h"
#include "hid.h"
#include <assert.h>
#include <stdatomic.h>

void *guest_sym(const char *name) {
    (void)name;
    assert(!"AppKit input test must not enter the guest SDK");
    return NULL;
}

/* Exercise the real AppKit event adapter without taking focus or changing the
 * user's cursor. OS capture itself is a separate live verification step. */
static unsigned associates, hides, unhides;
static CGError capture_error;
static uint64_t test_time = UINT64_C(1000000000);
static CGError test_associate(boolean_t associated) {
    associates++;
    return associated ? kCGErrorSuccess : capture_error;
}
static uint64_t test_now(void) { return test_time; }
@interface SkyTestCursor : NSObject
+ (void)hide;
+ (void)unhide;
@end
@implementation SkyTestCursor
+ (void)hide { hides++; }
+ (void)unhide { unhides++; }
@end
#define CGAssociateMouseAndMouseCursorPosition test_associate
#define NSCursor SkyTestCursor
#define now_ns test_now
#import "../macos_window.m"
#undef now_ns
#undef NSCursor
#undef CGAssociateMouseAndMouseCursorPosition

@interface SkyInputTestApplication : NSApplication
@end
@implementation SkyInputTestApplication
- (BOOL)isActive { return YES; }
@end
@interface SkyInputTestWindow : NSWindow
@property(nonatomic, strong) NSWindow *simulatedSheet;
@end
@implementation SkyInputTestWindow
- (BOOL)isKeyWindow { return YES; }
- (NSWindow *)attachedSheet { return self.simulatedSheet ?: super.attachedSheet; }
@end
@interface SkyInputTestMotion : NSEvent
@end
@implementation SkyInputTestMotion
- (CGFloat)deltaX { return 8; }
- (CGFloat)deltaY { return -4; }
- (CGFloat)scrollingDeltaY { return 1; }
- (BOOL)hasPreciseScrollingDeltas { return NO; }
@end

static NSEvent *key(unsigned code, BOOL down, NSEventModifierFlags modifiers, BOOL repeat) {
    return [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp
        location:NSZeroPoint modifierFlags:modifiers timestamp:0 windowNumber:0
        context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:repeat keyCode:code];
}
static sky_hid_npad_state_t read_state(void) {
    sky_hid_npad_state_t state; uint32_t id = 0;
    sky_hid_get_full_key_state(&state, &id);
    assert(state.attributes == 1);
    return state;
}
typedef struct { _Atomic int done; int accepted; char text[16]; } keyboard_call_t;
static void *rejected_keyboard(void *context) {
    keyboard_call_t *call = context;
    sky_keyboard_request_t request = {.min_length = 0, .max_length = 4};
    call->accepted = present_keyboard(&request, call->text, sizeof call->text);
    atomic_store(&call->done, 1);
    return NULL;
}
int main(void) {
    @autoreleasepool {
        [SkyInputTestApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        SkyKeyboardSession *keyboard = [SkyKeyboardSession new];
        keyboard.field = [NSTextField new];
        keyboard.minimum = 2; keyboard.maximum = 4; keyboard.capacity = 16;
        keyboard.field.stringValue = @"a"; assert(![keyboard validText]);
        keyboard.field.stringValue = @"龙裔"; assert([keyboard validText]);
        keyboard.field.stringValue = @"😀😀"; assert([keyboard validText]);
        keyboard.field.stringValue = @"😀😀a"; assert(![keyboard validText]);
        keyboard.field.stringValue = @"龙裔"; keyboard.capacity = 6;
        assert(![keyboard validText]); /* Six bytes still need a terminator. */
        keyboard.minimum = 0; keyboard.capacity = 16;
        unichar invalid[] = {0xd800}, embeddedNul[] = {'a', 0, 'b'};
        keyboard.field.stringValue = [NSString stringWithCharacters:invalid length:1];
        assert(![keyboard validText]);
        keyboard.field.stringValue = [NSString stringWithCharacters:embeddedNul length:3];
        assert(![keyboard validText]);
        char text[16] = "unchanged";
        sky_keyboard_request_t request = {.min_length = 0, .max_length = 4};
        assert(!present_keyboard(&request, text, sizeof text)); /* Never wait on the UI thread. */
        assert(!strcmp(text, "unchanged"));
        SkyInputTestWindow *window = [[SkyInputTestWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 360)
            styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        window.simulatedSheet = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 320, 120)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        g_keyboard_window = window;
        keyboard_call_t call = {.text = "unchanged"};
        pthread_t requester;
        assert(!pthread_create(&requester, NULL, rejected_keyboard, &call));
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:2];
        while (!atomic_load(&call.done) && deadline.timeIntervalSinceNow > 0)
            [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
        assert(atomic_load(&call.done));
        assert(!pthread_join(requester, NULL));
        assert(!call.accepted && !strcmp(call.text, "unchanged"));
        window.simulatedSheet = nil; g_keyboard_window = nil;
        SkyGameView *view = [[SkyGameView alloc] initWithFrame:NSMakeRect(0, 0, 640, 360)];
        window.contentView = view;
        NSEvent *mouse = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:NSZeroPoint
            modifierFlags:0 timestamp:0 windowNumber:0 context:nil eventNumber:0 clickCount:1 pressure:1];
        assert(window.acceptsMouseMovedEvents && ![view acceptsFirstMouse:mouse]);
        sky_hid_set_host_state(0, 0, 0, 0, 0);
        sky_hid_set_source_state(SKY_HID_SOURCE_TOUCH, UINT64_C(1) << 8, 100, 200, -300, 400);
        [view keyDown:key(13, YES, 0, NO)]; /* W */
        [view keyDown:key(14, YES, NSEventModifierFlagShift, NO)]; /* E + sprint */
        sky_hid_npad_state_t state = read_state();
        assert(state.buttons == ((UINT64_C(1) << 8) | 1 | (UINT64_C(1) << 6)));
        assert(state.lx == 0 && state.ly == 32767 && state.rx == -300 && state.ry == 400);
        [view resetInput]; state = read_state();
        assert(state.buttons == (UINT64_C(1) << 8) && state.lx == 100 && state.ly == 200);
        sky_hid_set_source_state(SKY_HID_SOURCE_TOUCH, 0, 0, 0, 0, 0);

        [view mouseDown:mouse]; assert(read_state().buttons == 1);
        [view mouseUp:mouse]; assert(!read_state().buttons);
        [view rightMouseDown:mouse]; assert(read_state().buttons == 2);
        [view rightMouseUp:mouse]; assert(!read_state().buttons);
        [view keyDown:key(122, YES, 0, NO)];
        assert(view.mouseCaptured && associates == 1 && hides == 1);
        [view keyDown:key(122, YES, 0, YES)];
        assert(view.mouseCaptured && associates == 1);
        [view mouseDown:mouse]; assert(read_state().buttons == (UINT64_C(1) << 9));
        [view mouseUp:mouse]; assert(!read_state().buttons);
        SkyInputTestMotion *motion = [SkyInputTestMotion new];
        [view mouseDragged:motion];
        sky_input_t input = view.input;
        test_time += 16666667; sky_input_tick(&input, test_time); view.input = input; [view publishInput];
        state = read_state(); assert(state.rx > 0 && state.ry > 0);
        test_time += 16666667; sky_input_tick(&input, test_time); view.input = input; [view publishInput];
        state = read_state(); assert(!state.rx && !state.ry);
        [view keyDown:key(53, YES, 0, NO)];
        assert(!view.mouseCaptured && read_state().buttons == (UINT64_C(1) << 10));
        [view keyUp:key(53, NO, 0, NO)]; assert(!read_state().buttons);
        assert(hides == unhides && associates == 2);

        capture_error = kCGErrorFailure;
        assert(![view setMouseLook:YES] && !view.mouseCaptured && hides == 1);
        capture_error = kCGErrorSuccess;
        assert([view setMouseLook:YES]);
        [view keyDown:key(48, YES, 0, NO)];
        assert(!view.mouseCaptured && read_state().buttons == 2);
        [view keyUp:key(48, NO, 0, NO)]; assert(!read_state().buttons);
        assert([view setMouseLook:YES]);
        [view flagsChanged:key(56, YES, NSEventModifierFlagCommand, NO)];
        assert(!view.mouseCaptured && !read_state().buttons);
        assert(hides == unhides);
        [view flagsChanged:key(56, NO, 0, NO)];
        [view scrollWheel:motion]; assert(read_state().buttons == (UINT64_C(1) << 13));
        assert([view resignFirstResponder]); assert(!read_state().buttons);
        [view.inputTimer invalidate]; view.inputTimer = nil;
        window.contentView = nil;
        puts("T_MACOS_INPUT OK real-event-adapter modes-modifiers-source-isolation-motion-idle-capture-cleanup");
    }
    return 0;
}
