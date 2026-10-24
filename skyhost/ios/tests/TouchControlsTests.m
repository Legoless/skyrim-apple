#import <XCTest/XCTest.h>
#import "../TouchControlsView.h"
#import "../../hid.h"
#import "../../skyhost.h"
#import "../../startup.h"
#import "../frontend.h"
#import "GameFrameReadiness.h"
#include <math.h>

extern const char *sky_ios_check_game_files(void);

static void on_main(dispatch_block_t block) {
    if (NSThread.isMainThread) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

static SkyTouchControlsView *find_controls(UIView *view) {
    if ([view isKindOfClass:SkyTouchControlsView.class]) return (SkyTouchControlsView *)view;
    for (UIView *child in view.subviews) {
        SkyTouchControlsView *found = find_controls(child);
        if (found) return found;
    }
    return nil;
}

@interface GenericControllerTouchControlsView : SkyTouchControlsView
@end

@implementation GenericControllerTouchControlsView
- (BOOL)usesGenericControllerUI { return YES; }
@end

@interface TouchControlsTests : XCTestCase
@property(nonatomic, strong) UIWindow *testWindow;
@property(nonatomic, strong) SkyTouchControlsView *controls;
@end

@implementation TouchControlsTests
- (void)testGameSmokeFrameReadinessRejectsBlankViewportAndNativeControls {
    XCTAssertFalse(sky_smoke_frame_has_scene_content(NULL));
    const size_t width = 320, height = 240;
    CGColorSpaceRef colour = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(NULL, width, height, 8, width * 4,
        colour, (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(colour);
    XCTAssertNotEqual(context, NULL);
    if (!context) return;
    CGContextSetRGBFillColor(context, 0, 0, 0, 1);
    CGContextFillRect(context, CGRectMake(0, 0, width, height));
    CGImageRef image = CGBitmapContextCreateImage(context);
    XCTAssertFalse(sky_smoke_frame_has_scene_content(image), @"Black loading frame is not ready");
    CGImageRelease(image);

    CGContextSetRGBFillColor(context, 1, 1, 1, 1);
    CGContextFillRect(context, CGRectMake(0, 0, width, height));
    image = CGBitmapContextCreateImage(context);
    XCTAssertFalse(sky_smoke_frame_has_scene_content(image), @"Uniform white startup frame is not ready");
    CGImageRelease(image);

    CGContextSetRGBFillColor(context, 0, 0, 0, 1);
    CGContextFillRect(context, CGRectMake(0, 0, width, height));
    CGContextSetRGBFillColor(context, 1, 1, 1, 1);
    CGContextFillRect(context, CGRectMake(0, 210, 32, 30));
    for (int y = 35; y < 205; y += 40)
        CGContextFillRect(context, CGRectMake(290, y, 30, 28));
    CGContextFillRect(context, CGRectMake(0, 0, width, 20));
    image = CGBitmapContextCreateImage(context);
    XCTAssertFalse(sky_smoke_frame_has_scene_content(image), @"Corner controls and letterboxing cannot establish game pixels");
    CGImageRelease(image);

    /* Authored texture-like variation inside the central game viewport. */
    for (int row = 0; row < 6; row++) {
        for (int column = 0; column < 12; column++) {
            CGFloat shade = (row + column) % 2 ? .18 : .65;
            CGContextSetRGBFillColor(context, shade, shade * .8, shade * .6, 1);
            CGContextFillRect(context, CGRectMake(64 + column * 16, 66 + row * 18, 16, 18));
        }
    }
    image = CGBitmapContextCreateImage(context);
    XCTAssertTrue(sky_smoke_frame_has_scene_content(image), @"Visible nonuniform game pixels permit the gesture phase");
    UIGraphicsBeginImageContextWithOptions(CGSizeMake(height, width), YES, 1);
    CGContextRef portraitContext = UIGraphicsGetCurrentContext();
    CGContextTranslateCTM(portraitContext, 0, width);
    CGContextRotateCTM(portraitContext, -M_PI_2);
    CGContextDrawImage(portraitContext, CGRectMake(0, 0, width, height), image);
    UIImage *portrait = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    UIImage *rotated = [UIImage imageWithCGImage:portrait.CGImage scale:1 orientation:UIImageOrientationRight];
    XCTAssertFalse(sky_smoke_frame_has_scene_content(rotated.CGImage));
    UIImage *normalized = sky_smoke_normalized_image(rotated);
    XCTAssertEqual(normalized.imageOrientation, UIImageOrientationUp);
    XCTAssertGreaterThan(normalized.size.width, normalized.size.height);
    XCTAssertTrue(sky_smoke_image_has_scene_content(rotated), @"Readiness applies screenshot orientation");
    CGImageRelease(image);
    CGContextRelease(context);
}
- (void)setUp {
    [super setUp];
    on_main(^{
        sky_hid_initialize_npad();
        uint32_t id = 0;
        sky_hid_set_supported_npad_style_set(1);
        sky_hid_set_supported_npad_id_type(&id, 1);
        for (int source = SKY_HID_SOURCE_EXTERNAL; source <= SKY_HID_SOURCE_TOUCH; ++source)
            sky_hid_set_source_state(source, 0, 0, 0, 0, 0);
        UIWindowScene *scene = (UIWindowScene *)UIApplication.sharedApplication.delegate.window.windowScene;
        XCTAssertTrue([scene isKindOfClass:UIWindowScene.class]);
        self.testWindow = [[UIWindow alloc] initWithWindowScene:scene];
        self.testWindow.rootViewController = [UIViewController new];
        self.controls = [[SkyTouchControlsView alloc] initWithFrame:self.testWindow.bounds];
        self.controls.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        [self.testWindow.rootViewController.view addSubview:self.controls];
        [self.testWindow makeKeyAndVisible];
        [self.testWindow layoutIfNeeded];
        [self.controls layoutIfNeeded];
    });
    [self waitForHostInputReady];
}
- (void)tearDown {
    on_main(^{
        [self.controls cancelAllTouches];
        for (int source = SKY_HID_SOURCE_EXTERNAL; source <= SKY_HID_SOURCE_TOUCH; ++source)
            sky_hid_set_source_state(source, 0, 0, 0, 0, 0);
        self.testWindow.hidden = YES;
        self.controls = nil;
        self.testWindow = nil;
    });
    [super tearDown];
}
- (void)waitForHostInputReady {
    NSPredicate *ready = [NSPredicate predicateWithBlock:^BOOL(id object, NSDictionary *bindings) {
        (void)object; (void)bindings;
        __block BOOL active = NO;
        on_main(^{
            id<UIApplicationDelegate> delegate = UIApplication.sharedApplication.delegate;
            SkyTouchControlsView *host = find_controls(delegate.window.rootViewController.view);
            active = host.userInteractionEnabled && sky_oe_get_current_focus_state() == 1;
        });
        return active;
    }];
    XCTNSPredicateExpectation *expectation = [[XCTNSPredicateExpectation alloc] initWithPredicate:ready object:nil];
    [self waitForExpectations:@[expectation] timeout:10];
}
- (sky_hid_npad_state_t)controllerState {
    uint32_t id = 0;
    sky_hid_npad_state_t state;
    sky_hid_get_full_key_state(&state, &id);
    return state;
}
- (void)stepVisibilityInGame:(BOOL)inGame forSeconds:(NSTimeInterval)seconds {
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];
    for (;;) {
        [self.controls updateVisibilityInGame:inGame testMode:NO];
        if ([NSDate.date compare:end] != NSOrderedAscending) break;
        [NSThread sleepForTimeInterval:0.01];
    }
}
- (CGFloat)renderedBrightnessForControl:(sky_touch_control_t)control {
    sky_touch_t model;
    [self.controls touchSnapshot:&model];
    const sky_touch_region_t *region = sky_touch_region(&model, control);
    XCTAssertNotEqual(region, NULL);
    if (!region) return 1;
    CGPoint point = CGPointMake(region->x, region->y + region->radius * 0.68f);
    UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat new];
    format.scale = 1;
    format.opaque = YES;
    format.preferredRange = UIGraphicsImageRendererFormatRangeStandard;
    UIGraphicsImageRenderer *renderer =
        [[UIGraphicsImageRenderer alloc] initWithBounds:self.controls.bounds format:format];
    UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
        CGContextSetFillColorWithColor(context.CGContext, UIColor.whiteColor.CGColor);
        CGContextFillRect(context.CGContext, self.controls.bounds);
        [self.controls.layer renderInContext:context.CGContext];
    }];
    CGImageRef cgImage = image.CGImage;
    CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(cgImage));
    const UInt8 *bytes = CFDataGetBytePtr(data);
    size_t bytesPerRow = CGImageGetBytesPerRow(cgImage);
    size_t bytesPerPixel = CGImageGetBitsPerPixel(cgImage) / 8;
    XCTAssertEqual(bytesPerPixel, (size_t)4);
    if (bytesPerPixel != 4) {
        CFRelease(data);
        return 1;
    }
    size_t x = MIN((size_t)MAX(point.x, 0), CGImageGetWidth(cgImage) - 1);
    size_t y = MIN((size_t)MAX(point.y, 0), CGImageGetHeight(cgImage) - 1);
    size_t offset = y * bytesPerRow + x * bytesPerPixel;
    BOOL littleEndian = (CGImageGetBitmapInfo(cgImage) & kCGBitmapByteOrderMask) == kCGBitmapByteOrder32Little;
    CGFloat red = bytes[offset + (littleEndian ? 2 : 0)] / 255.0;
    CFRelease(data);
    return red;
}
- (UIAccessibilityElement *)accessibilityElementNamed:(NSString *)name {
    for (UIAccessibilityElement *element in self.controls.accessibilityElements)
        if ([element.accessibilityIdentifier isEqualToString:name]) return element;
    return nil;
}
- (void)attachControlsLayout:(NSString *)name {
    UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat preferredFormat];
    format.opaque = NO;
    UIGraphicsImageRenderer *renderer =
        [[UIGraphicsImageRenderer alloc] initWithBounds:self.controls.bounds format:format];
    UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
        [self.controls.layer renderInContext:context.CGContext];
    }];
    XCTAttachment *attachment = [XCTAttachment attachmentWithData:UIImagePNGRepresentation(image)
                                             uniformTypeIdentifier:@"public.png"];
    attachment.name = name;
    attachment.lifetime = XCTAttachmentLifetimeKeepAlways;
    [self addAttachment:attachment];
}
- (void)testGameplayShowsButtonsSticksFollowTouchAndMenusShowOnlyBack {
    on_main(^{
        [self.controls updateVisibilityInGame:NO testMode:NO];
        XCTAssertGreaterThan([self renderedBrightnessForControl:SKY_TOUCH_USE], 0.96);
        XCTAssertGreaterThan([self renderedBrightnessForControl:SKY_TOUCH_ATTACK], 0.96);
        XCTAssertGreaterThan([self renderedBrightnessForControl:SKY_TOUCH_JUMP], 0.96);

        [self stepVisibilityInGame:YES forSeconds:0.4];
        for (int i = SKY_TOUCH_LOOK + 1; i < SKY_TOUCH_CONTROL_COUNT; ++i)
            if (i != SKY_TOUCH_BACK)
                XCTAssertLessThan([self renderedBrightnessForControl:i], 0.9,
                                  @"Gameplay buttons must stay visible without a touch (control %d)", i);
        XCTAssertGreaterThan([self renderedBrightnessForControl:SKY_TOUCH_MOVE], 0.96,
                             @"Move must stay hidden until the screen is touched");
        XCTAssertGreaterThan([self renderedBrightnessForControl:SKY_TOUCH_LOOK], 0.96,
                             @"Look must stay hidden until the screen is touched");

        sky_touch_t model;
        [self.controls touchSnapshot:&model];
        const sky_touch_region_t *pause = sky_touch_region(&model, SKY_TOUCH_PAUSE);
        const sky_touch_region_t *back = sky_touch_region(&model, SKY_TOUCH_BACK);
        const CGSize size = self.controls.bounds.size;
        XCTAssertLessThan(pause->x, size.width * 0.25, @"Pause button must be in the top-left of the screen");
        XCTAssertLessThan(pause->y, size.height * 0.25, @"Pause button must be in the top-left of the screen");
        XCTAssertEqual(pause->x, back->x, @"Pause must sit where Back closes menus");
        XCTAssertEqual(pause->y, back->y, @"Pause must sit where Back closes menus");

        const CGPoint moveTouch = CGPointMake(size.width * 0.2, size.height * 0.4);
        const CGPoint lookTouch = CGPointMake(size.width * 0.85, size.height * 0.82);
        XCTAssertTrue([self.controls injectTouchPhase:0 identifier:41 point:moveTouch]);
        XCTAssertTrue([self.controls injectTouchPhase:0 identifier:42 point:lookTouch]);
        [self.controls touchSnapshot:&model];
        XCTAssertEqualWithAccuracy(sky_touch_region(&model, SKY_TOUCH_MOVE)->x, moveTouch.x, .01);
        XCTAssertEqualWithAccuracy(sky_touch_region(&model, SKY_TOUCH_MOVE)->y, moveTouch.y, .01);
        XCTAssertEqualWithAccuracy(sky_touch_region(&model, SKY_TOUCH_LOOK)->x, lookTouch.x, .01);
        XCTAssertEqualWithAccuracy(sky_touch_region(&model, SKY_TOUCH_LOOK)->y, lookTouch.y, .01);
        XCTAssertLessThan([self renderedBrightnessForControl:SKY_TOUCH_MOVE], 0.9,
                          @"Move must appear where the left-side finger landed");
        XCTAssertLessThan([self renderedBrightnessForControl:SKY_TOUCH_LOOK], 0.9,
                          @"Look must appear where the right-side finger landed");
        const sky_touch_region_t *move = sky_touch_region(&model, SKY_TOUCH_MOVE);
        [self.controls injectTouchPhase:1 identifier:41 point:CGPointMake(moveTouch.x + move->radius * .7, moveTouch.y - move->radius * .3)];
        XCTAssertTrue([self.controls injectTouchPhase:0 identifier:43
            point:CGPointMake(sky_touch_region(&model, SKY_TOUCH_ATTACK)->x, sky_touch_region(&model, SKY_TOUCH_ATTACK)->y)]);
        [self attachControlsLayout:@"Gameplay touch controls: Move and Look held, Attack pressed"];
        [self.controls injectTouchPhase:2 identifier:43 point:CGPointZero];
        [self.controls injectTouchPhase:2 identifier:41 point:moveTouch];
        [self.controls injectTouchPhase:2 identifier:42 point:lookTouch];
        XCTAssertGreaterThan([self renderedBrightnessForControl:SKY_TOUCH_MOVE], 0.96,
                             @"Move must disappear when its finger lifts");
        XCTAssertGreaterThan([self renderedBrightnessForControl:SKY_TOUCH_LOOK], 0.96,
                             @"Look must disappear when its finger lifts");
        XCTAssertLessThan([self renderedBrightnessForControl:SKY_TOUCH_ATTACK], 0.9);
        XCTAssertNil([self accessibilityElementNamed:@"sky.touch.back"]);
        XCTAssertNotNil([self accessibilityElementNamed:@"sky.touch.pause"]);

        sky_set_ui_in_game(2);
        [self stepVisibilityInGame:YES forSeconds:0.4];
        [self.controls touchSnapshot:&model];
        const sky_touch_region_t *use = sky_touch_region(&model, SKY_TOUCH_USE);
        XCTAssertLessThan([self renderedBrightnessForControl:SKY_TOUCH_BACK], 0.9,
                          @"In-game menus must show the Back button in the top-left");
        for (int i = SKY_TOUCH_LOOK + 1; i < SKY_TOUCH_CONTROL_COUNT; ++i)
            if (i != SKY_TOUCH_BACK && i != SKY_TOUCH_PAUSE)
                XCTAssertGreaterThan([self renderedBrightnessForControl:i], 0.96,
                                     @"In-game menus must hide gameplay controls other than Back (control %d)", i);
        NSArray *menuElements = self.controls.accessibilityElements;
        XCTAssertEqual(menuElements.count, (NSUInteger)1);
        XCTAssertEqualObjects([menuElements.firstObject accessibilityIdentifier], @"sky.touch.back");
        XCTAssertFalse([self.controls injectTouchPhase:0 identifier:44 point:CGPointMake(use->x, use->y)]);
        XCTAssertTrue([self.controls injectTouchPhase:0 identifier:45 point:CGPointMake(back->x, back->y)]);
        XCTAssertEqual([self controllerState].buttons, UINT64_C(1) << 1);
        [self.controls injectTouchPhase:2 identifier:45 point:CGPointMake(back->x, back->y)];
        sky_set_ui_in_game(0);
        [self.controls cancelAllTouches];
    });
}
- (void)testSimultaneousMoveLookAttackAndIndependentRelease {
    on_main(^{
        sky_touch_t model;
        [self.controls touchSnapshot:&model];
        XCTAssertTrue(model.enabled);
        const sky_touch_region_t *move = sky_touch_region(&model, SKY_TOUCH_MOVE);
        const sky_touch_region_t *look = sky_touch_region(&model, SKY_TOUCH_LOOK);
        const sky_touch_region_t *attack = sky_touch_region(&model, SKY_TOUCH_ATTACK);
        XCTAssertTrue([self.controls injectTouchPhase:0 identifier:1 point:CGPointMake(move->x, move->y)]);
        XCTAssertTrue([self.controls injectTouchPhase:0 identifier:2 point:CGPointMake(look->x, look->y)]);
        XCTAssertTrue([self.controls injectTouchPhase:0 identifier:3 point:CGPointMake(attack->x, attack->y)]);
        [self.controls injectTouchPhase:1 identifier:1 point:CGPointMake(move->x, move->y - move->radius)];
        [self.controls injectTouchPhase:1 identifier:2 point:CGPointMake(look->x + look->radius, look->y)];
        sky_hid_npad_state_t state = [self controllerState];
        XCTAssertEqual(state.buttons, (UINT64_C(1) << 9) | SKY_TOUCH_SPRINT_BUTTON);
        XCTAssertEqual(state.lx, 0); XCTAssertEqual(state.ly, 32767);
        XCTAssertEqual(state.rx, 32767); XCTAssertEqual(state.ry, 0);
        [self.controls injectTouchPhase:2 identifier:1 point:CGPointZero];
        state = [self controllerState];
        XCTAssertEqual(state.ly, 0); XCTAssertEqual(state.rx, 32767);
        XCTAssertEqual(state.buttons, UINT64_C(1) << 9);
        [self.controls injectTouchPhase:2 identifier:3 point:CGPointZero];
        state = [self controllerState];
        XCTAssertEqual(state.buttons, 0u); XCTAssertEqual(state.rx, 32767);
        [self.controls injectTouchPhase:2 identifier:2 point:CGPointZero];
        state = [self controllerState];
        XCTAssertEqual(state.rx, 0);
    });
}
- (void)testTouchCancelPreservesDesktopAndExternalSources {
    on_main(^{
        sky_hid_set_source_state(SKY_HID_SOURCE_DESKTOP, UINT64_C(1) << 7, 1234, -4321, 0, 0);
        sky_hid_set_source_state(SKY_HID_SOURCE_EXTERNAL, UINT64_C(1) << 11, 0, 0, -999, 999);
        sky_touch_t model;
        [self.controls touchSnapshot:&model];
        const sky_touch_region_t *use = sky_touch_region(&model, SKY_TOUCH_USE);
        [self.controls injectTouchPhase:0 identifier:1 point:CGPointMake(use->x, use->y)];
        [self.controls cancelAllTouches]; /* pending touch press also cancelled */
        sky_hid_npad_state_t state = [self controllerState];
        XCTAssertEqual(state.buttons, (UINT64_C(1) << 7) | (UINT64_C(1) << 11));
        XCTAssertEqual(state.lx, 1234); XCTAssertEqual(state.ly, -4321);
        XCTAssertEqual(state.rx, -999); XCTAssertEqual(state.ry, 999);
        self.controls.userInteractionEnabled = NO;
        XCTAssertFalse([self.controls injectTouchPhase:0 identifier:2 point:CGPointMake(use->x, use->y)]);
    });
}
- (void)testButtonMultiOwnershipResizeAndAccessibility {
    on_main(^{
        sky_touch_t model;
        [self.controls touchSnapshot:&model];
        const sky_touch_region_t *use = sky_touch_region(&model, SKY_TOUCH_USE);
        CGPoint point = CGPointMake(use->x, use->y);
        [self.controls injectTouchPhase:0 identifier:0 point:point];
        [self.controls injectTouchPhase:0 identifier:1 point:point];
        [self controllerState];
        [self.controls injectTouchPhase:2 identifier:0 point:point];
        XCTAssertEqual([self controllerState].buttons, 1u);
        [self.controls injectTouchPhase:2 identifier:1 point:point];
        XCTAssertEqual([self controllerState].buttons, 0u);
        [self.controls injectTouchPhase:0 identifier:1 point:point];
        CGRect frame = self.controls.frame;
        frame.size.width -= 17;
        self.controls.frame = frame;
        [self.controls layoutIfNeeded];
        XCTAssertEqual([self controllerState].buttons, 0u);
        [self.controls touchSnapshot:&model];
        UIEdgeInsets safe = self.controls.safeAreaInsets;
        static NSString * const names[SKY_TOUCH_CONTROL_COUNT] = {
            @"move", @"look", @"use", @"back", @"jump", @"sneak", @"attack", @"pause",
        };
        XCTAssertEqual(self.controls.accessibilityElements.count, (NSUInteger)SKY_TOUCH_CONTROL_COUNT - 1,
                       @"Gameplay exposes every control except Back, which shares Pause's spot");
        for (int i = 0; i < SKY_TOUCH_CONTROL_COUNT; ++i) {
            UIAccessibilityElement *element =
                [self accessibilityElementNamed:[@"sky.touch." stringByAppendingString:names[i]]];
            const sky_touch_region_t *r = sky_touch_region(&model, i);
            XCTAssertGreaterThanOrEqual(r->x - r->radius, safe.left - .001);
            XCTAssertLessThanOrEqual(r->x + r->radius, frame.size.width - safe.right + .001);
            XCTAssertGreaterThanOrEqual(r->y - r->radius, safe.top - .001);
            XCTAssertLessThanOrEqual(r->y + r->radius, frame.size.height - safe.bottom + .001);
            if (i == SKY_TOUCH_BACK) {
                XCTAssertNil(element);
            } else {
                XCTAssertNotNil(element);
                XCTAssertEqualWithAccuracy(CGRectGetMidX(element.accessibilityFrameInContainerSpace), r->x, .01);
                XCTAssertEqualWithAccuracy(CGRectGetMidY(element.accessibilityFrameInContainerSpace), r->y, .01);
            }
            for (int j = i + 1; j < SKY_TOUCH_CONTROL_COUNT; ++j) {
                const sky_touch_region_t *b = sky_touch_region(&model, j);
                if (i == SKY_TOUCH_BACK && j == SKY_TOUCH_PAUSE) continue;
                XCTAssertGreaterThan(hypotf(r->x - b->x, r->y - b->y), r->radius + b->radius);
            }
        }
        UIAccessibilityElement *useElement = [self accessibilityElementNamed:@"sky.touch.use"];
        XCTAssertTrue([useElement accessibilityActivate]);
        XCTAssertEqual([self controllerState].buttons, 1u); /* short accessibility press preserved */
        XCTAssertEqual([self controllerState].buttons, 0u);
        [self.controls cancelAllTouches];
    });
}
- (void)testGenericControllerShowsSeparateBackPauseAndIndependentABButtons {
    on_main(^{
        [self.controls cancelAllTouches];
        CGRect frame = self.controls.frame;
        [self.controls removeFromSuperview];
        self.controls = [[GenericControllerTouchControlsView alloc] initWithFrame:frame];
        self.controls.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        [self.testWindow.rootViewController.view addSubview:self.controls];
        [self.controls setNeedsLayout];
        [self.controls layoutIfNeeded];
        [self stepVisibilityInGame:YES forSeconds:0.4];

        sky_touch_t model;
        [self.controls touchSnapshot:&model];
        XCTAssertTrue(model.enabled);
        XCTAssertEqual(model.menu, 2u, @"Generic controller input must expose every control together");
        XCTAssertEqual(self.controls.accessibilityElements.count, (NSUInteger)SKY_TOUCH_CONTROL_COUNT);
        for (int i = SKY_TOUCH_USE; i < SKY_TOUCH_CONTROL_COUNT; ++i)
            XCTAssertLessThan([self renderedBrightnessForControl:i], 0.9,
                              @"Generic controller buttons must remain visible (control %d)", i);

        const sky_touch_region_t *back = sky_touch_region(&model, SKY_TOUCH_BACK);
        const sky_touch_region_t *pause = sky_touch_region(&model, SKY_TOUCH_PAUSE);
        const sky_touch_region_t *use = sky_touch_region(&model, SKY_TOUCH_USE);
        CGPoint backPoint = CGPointMake(back->x, back->y);
        CGPoint pausePoint = CGPointMake(pause->x, pause->y);
        CGPoint usePoint = CGPointMake(use->x, use->y);
        XCTAssertGreaterThan(hypotf(back->x - pause->x, back->y - pause->y), back->radius + pause->radius);
        XCTAssertEqual(sky_touch_hit_test(&model, back->x, back->y), SKY_TOUCH_BACK);
        XCTAssertEqual(sky_touch_hit_test(&model, pause->x, pause->y), SKY_TOUCH_PAUSE);
        XCTAssertTrue([self.controls pointInside:backPoint withEvent:nil]);
        XCTAssertTrue([self.controls pointInside:pausePoint withEvent:nil]);
        UIAccessibilityElement *backElement = [self accessibilityElementNamed:@"sky.touch.back"];
        UIAccessibilityElement *pauseElement = [self accessibilityElementNamed:@"sky.touch.pause"];
        XCTAssertNotNil(backElement);
        XCTAssertNotNil(pauseElement);
        XCTAssertEqualWithAccuracy(CGRectGetMidX(backElement.accessibilityFrameInContainerSpace), back->x, .01);
        XCTAssertEqualWithAccuracy(CGRectGetMidY(backElement.accessibilityFrameInContainerSpace), back->y, .01);
        XCTAssertEqualWithAccuracy(CGRectGetMidX(pauseElement.accessibilityFrameInContainerSpace), pause->x, .01);
        XCTAssertEqualWithAccuracy(CGRectGetMidY(pauseElement.accessibilityFrameInContainerSpace), pause->y, .01);

        XCTAssertTrue([self.controls injectTouchPhase:0 identifier:201 point:pausePoint]);
        XCTAssertEqual([self controllerState].buttons, UINT64_C(1) << 10);
        [self.controls injectTouchPhase:2 identifier:201 point:pausePoint];
        XCTAssertEqual([self controllerState].buttons, 0u);
        XCTAssertTrue([self.controls injectTouchPhase:0 identifier:202 point:usePoint]);
        XCTAssertTrue([self.controls injectTouchPhase:0 identifier:203 point:backPoint]);
        XCTAssertEqual([self controllerState].buttons, (UINT64_C(1) << 0) | (UINT64_C(1) << 1));
        [self.controls injectTouchPhase:2 identifier:202 point:usePoint];
        XCTAssertEqual([self controllerState].buttons, UINT64_C(1) << 1);
        [self.controls injectTouchPhase:2 identifier:203 point:backPoint];
        XCTAssertEqual([self controllerState].buttons, 0u);
        XCTAssertTrue([self.controls injectTouchPhase:0 identifier:204 point:usePoint]);
        XCTAssertTrue([self.controls injectTouchPhase:0 identifier:205 point:backPoint]);
        XCTAssertEqual([self controllerState].buttons, (UINT64_C(1) << 0) | (UINT64_C(1) << 1));
        [self.controls injectTouchPhase:2 identifier:205 point:backPoint];
        XCTAssertEqual([self controllerState].buttons, UINT64_C(1) << 0);
        [self.controls injectTouchPhase:2 identifier:204 point:usePoint];
        XCTAssertEqual([self controllerState].buttons, 0u);

        XCTAssertTrue([backElement accessibilityActivate]);
        XCTAssertEqual([self controllerState].buttons, UINT64_C(1) << 1);
        XCTAssertEqual([self controllerState].buttons, 0u);
        XCTAssertTrue([pauseElement accessibilityActivate]);
        XCTAssertEqual([self controllerState].buttons, UINT64_C(1) << 10);
        XCTAssertEqual([self controllerState].buttons, 0u);
        [self attachControlsLayout:@"Generic controller: separate Back, Pause and gameplay buttons"];
        [self.controls cancelAllTouches];
    });
}
- (void)testFrontendLifecycleReleasesOnlyTouchAndRestoresInput {
    on_main(^{
        BOOL testMode = [NSProcessInfo.processInfo.arguments containsObject:@"--controls-test-mode"] ||
            [NSProcessInfo.processInfo.environment[@"SKY_CONTROLS_TEST_MODE"] isEqualToString:@"1"];
        XCTAssertTrue(testMode, @"The host must explicitly launch in controls test mode.");
        XCTAssertTrue(UIApplication.sharedApplication.idleTimerDisabled);
        id<UIApplicationDelegate> delegate = UIApplication.sharedApplication.delegate;
        SkyTouchControlsView *hostControls = find_controls(delegate.window.rootViewController.view);
        XCTAssertNotNil(hostControls);
        sky_touch_t model;
        [hostControls touchSnapshot:&model];
        const sky_touch_region_t *attack = sky_touch_region(&model, SKY_TOUCH_ATTACK);
        sky_hid_set_source_state(SKY_HID_SOURCE_DESKTOP, UINT64_C(1) << 7, 1234, -4321, 0, 0);
        sky_hid_set_source_state(SKY_HID_SOURCE_EXTERNAL, UINT64_C(1) << 11, 0, 0, -999, 999);
        XCTAssertTrue(sky_ios_touch_inject(0, 77, attack->x, attack->y));
        [delegate applicationWillResignActive:UIApplication.sharedApplication];
        XCTAssertFalse(UIApplication.sharedApplication.idleTimerDisabled);
        sky_hid_npad_state_t state = [self controllerState];
        XCTAssertEqual(state.buttons, (UINT64_C(1) << 7) | (UINT64_C(1) << 11));
        XCTAssertEqual(state.lx, 1234); XCTAssertEqual(state.ly, -4321);
        XCTAssertEqual(state.rx, -999); XCTAssertEqual(state.ry, 999);
        XCTAssertEqual(sky_oe_get_current_focus_state(), 2);
        XCTAssertFalse(sky_ios_touch_inject(0, 78, attack->x, attack->y));
        [delegate applicationDidEnterBackground:UIApplication.sharedApplication];
        XCTAssertFalse(UIApplication.sharedApplication.idleTimerDisabled);
        [delegate applicationDidBecomeActive:UIApplication.sharedApplication];
        [self waitForHostInputReady];
        XCTAssertTrue(UIApplication.sharedApplication.idleTimerDisabled);
        XCTAssertEqual(sky_oe_get_current_focus_state(), 1);
        XCTAssertTrue(sky_ios_touch_inject(0, 79, attack->x, attack->y));
        XCTAssertEqual([self controllerState].buttons, (UINT64_C(1) << 7) | (UINT64_C(1) << 11) | (UINT64_C(1) << 9));
        [hostControls cancelAllTouches];
    });
}
- (void)makeHostWindowKey {
    on_main(^{
        self.testWindow.hidden = YES;
        [UIApplication.sharedApplication.delegate.window makeKeyAndVisible];
    });
}
- (void)attachNativeKeyboardLayout {
    on_main(^{
        UIWindow *window = UIApplication.sharedApplication.delegate.window;
        UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithBounds:window.bounds];
        UIImage *snapshot = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
            (void)context;
            [window drawViewHierarchyInRect:window.bounds afterScreenUpdates:YES];
        }];
        XCTAttachment *attachment = [XCTAttachment attachmentWithImage:snapshot];
        attachment.name = @"Native keyboard alert layout";
        attachment.lifetime = XCTAttachmentLifetimeKeepAlways;
        [self addAttachment:attachment];
    });
}
- (void)waitForKeyboard {
    NSPredicate *shown = [NSPredicate predicateWithBlock:^BOOL(id object, NSDictionary *bindings) {
        (void)object; (void)bindings;
        __block BOOL ready;
        on_main(^{ ready = sky_ios_keyboard_is_presented(); });
        return ready;
    }];
    XCTNSPredicateExpectation *expectation = [[XCTNSPredicateExpectation alloc] initWithPredicate:shown object:nil];
    [self waitForExpectations:@[expectation] timeout:10];
}
- (void)testKeyboardConfirmUsesNativeFieldAndUTF16Limits {
    [self makeHostWindowKey];
    sky_keyboard_request_t request = {"Character name", "Choose a name", "", "Old", 2, 4};
    XCTestExpectation *finished = [self expectationWithDescription:@"Guest receives confirmed Unicode text"];
    __block int accepted = -1;
    __block NSString *result;
    on_main(^{
        sky_hid_set_source_state(SKY_HID_SOURCE_DESKTOP, UINT64_C(1) << 7, 0, 0, 0, 0);
        sky_touch_t model;
        [find_controls(UIApplication.sharedApplication.delegate.window.rootViewController.view) touchSnapshot:&model];
        const sky_touch_region_t *use = sky_touch_region(&model, SKY_TOUCH_USE);
        XCTAssertTrue(sky_ios_touch_inject(0, 33, use->x, use->y));
    });
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char output[64];
        accepted = sky_ios_present_keyboard(&request, output, sizeof output);
        result = [NSString stringWithUTF8String:output];
        [finished fulfill];
    });
    [self waitForKeyboard];
    [self attachNativeKeyboardLayout];
    on_main(^{
        UIViewController *root = UIApplication.sharedApplication.delegate.window.rootViewController;
        XCTAssertTrue([root.presentedViewController isKindOfClass:UIAlertController.class]);
        UIAlertController *alert = (UIAlertController *)root.presentedViewController;
        XCTAssertEqualObjects(alert.textFields.firstObject.text, @"Old");
        XCTAssertEqual([self controllerState].buttons, UINT64_C(1) << 7); /* pending Touch cleared */
        XCTAssertFalse(sky_ios_keyboard_respond("A", 1));
        XCTAssertFalse(sky_ios_keyboard_respond("😀😀A", 1)); /* five UTF-16 units */
        XCTAssertTrue(sky_ios_keyboard_is_presented());
        UITextField *field = alert.textFields.firstObject;
        field.text = @"A😀B"; /* four UTF-16 units, three graphemes */
        [field sendActionsForControlEvents:UIControlEventEditingChanged];
        XCTAssertTrue(alert.actions.lastObject.enabled);
        XCTAssertTrue([field.delegate textFieldShouldReturn:field]); /* actual native Return path */
    });
    [self waitForExpectations:@[finished] timeout:10];
    XCTAssertEqual(accepted, 1);
    XCTAssertEqualObjects(result, @"A😀B");
    [self waitForHostInputReady];
}
- (void)testKeyboardCancelAndUTF8ByteCapacity {
    [self makeHostWindowKey];
    sky_keyboard_request_t request = {"Text", "", "", "", 0, 10};
    XCTestExpectation *finished = [self expectationWithDescription:@"Guest receives native Cancel"];
    __block int accepted = -1;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char output[5];
        accepted = sky_ios_present_keyboard(&request, output, sizeof output);
        [finished fulfill];
    });
    [self waitForKeyboard];
    on_main(^{
        XCTAssertFalse(sky_ios_keyboard_respond("😀A", 1)); /* five bytes plus NUL exceed capacity */
        XCTAssertTrue(sky_ios_keyboard_is_presented());
        XCTAssertTrue(sky_ios_keyboard_respond(NULL, 0));
    });
    [self waitForExpectations:@[finished] timeout:10];
    XCTAssertEqual(accepted, 0);
    [self waitForHostInputReady];

    sky_keyboard_request_t empty = {"Optional text", "", "", "", 0, 0};
    XCTestExpectation *emptyFinished = [self expectationWithDescription:@"Capacity one permits only empty text"];
    __block int emptyAccepted = -1;
    __block char emptyOutput = 'X';
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char output[1];
        emptyAccepted = sky_ios_present_keyboard(&empty, output, sizeof output);
        emptyOutput = output[0];
        [emptyFinished fulfill];
    });
    [self waitForKeyboard];
    on_main(^{
        XCTAssertFalse(sky_ios_keyboard_respond("A", 1));
        XCTAssertTrue(sky_ios_keyboard_respond("", 1));
    });
    [self waitForExpectations:@[emptyFinished] timeout:10];
    XCTAssertEqual(emptyAccepted, 1); XCTAssertEqual(emptyOutput, 0);
    [self waitForHostInputReady];
}
- (void)testKeyboardBackgroundCancellationAndConcurrentRequest {
    [self makeHostWindowKey];
    sky_keyboard_request_t request = {"Name", "", "", "", 0, 10};
    on_main(^{ char output[8]; XCTAssertEqual(sky_ios_present_keyboard(&request, output, sizeof output), 0); });
    XCTestExpectation *first = [self expectationWithDescription:@"Background cancels the waiting guest"];
    __block int firstResult = -1;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char output[64]; firstResult = sky_ios_present_keyboard(&request, output, sizeof output); [first fulfill];
    });
    [self waitForKeyboard];
    XCTestExpectation *second = [self expectationWithDescription:@"Concurrent keyboard request cancels without replacing the first"];
    __block int secondResult = -1;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char output[64]; secondResult = sky_ios_present_keyboard(&request, output, sizeof output); [second fulfill];
    });
    [self waitForExpectations:@[second] timeout:10];
    XCTAssertEqual(secondResult, 0);
    on_main(^{
        XCTAssertTrue(sky_ios_keyboard_is_presented());
        [UIApplication.sharedApplication.delegate applicationWillResignActive:UIApplication.sharedApplication];
        XCTAssertFalse(sky_ios_keyboard_is_presented());
        XCTAssertFalse(sky_ios_keyboard_respond("late", 1));
    });
    [self waitForExpectations:@[first] timeout:10];
    XCTAssertEqual(firstResult, 0);
    on_main(^{ [UIApplication.sharedApplication.delegate applicationDidBecomeActive:UIApplication.sharedApplication]; });
    [self waitForHostInputReady];
}
- (void)testCopiedRomFsIsRecheckedByRetryAndForeground {
    [self makeHostWindowKey];
    NSString *temporary = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSString *romfs = [temporary stringByAppendingPathComponent:@"RomFS"];
    NSString *save = [temporary stringByAppendingPathComponent:@"save"];
    const char *oldRomfsValue = getenv("SKY_ROMFS"), *oldSaveValue = getenv("SKY_SAVE_DIR"), *oldShaderValue = getenv("SKY_SHADER_CACHE_DIR");
    NSString *oldRomfs = oldRomfsValue ? [NSString stringWithUTF8String:oldRomfsValue] : nil;
    NSString *oldSave = oldSaveValue ? [NSString stringWithUTF8String:oldSaveValue] : nil;
    NSString *oldShader = oldShaderValue ? [NSString stringWithUTF8String:oldShaderValue] : nil;
    on_main(^{
        setenv("SKY_ROMFS", romfs.fileSystemRepresentation, 1);
        setenv("SKY_SAVE_DIR", save.fileSystemRepresentation, 1);
        sky_ios_set_setup_message(sky_ios_check_game_files());
        XCTAssertFalse(sky_ios_retry_setup());
        XCTAssertFalse(UIApplication.sharedApplication.idleTimerDisabled);
        SkyTouchControlsView *host = find_controls(UIApplication.sharedApplication.delegate.window.rootViewController.view);
        XCTAssertTrue(host.hidden);
        NSFileManager *files = NSFileManager.defaultManager;
        XCTAssertTrue([files createDirectoryAtPath:[romfs stringByAppendingPathComponent:@"Data"] withIntermediateDirectories:YES attributes:nil error:NULL]);
        XCTAssertTrue([@"; synthetic setup-check fixture, never executed as a game" writeToFile:[romfs stringByAppendingPathComponent:@"Skyrim.ini"] atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
        UIButton *retry = nil;
        for (UIView *container in UIApplication.sharedApplication.delegate.window.rootViewController.view.subviews)
            for (UIView *view in container.subviews)
                if ([view.accessibilityIdentifier isEqualToString:@"sky.setup.retry"]) retry = (UIButton *)view;
        XCTAssertNotNil(retry);
        [retry sendActionsForControlEvents:UIControlEventTouchUpInside];
    });
    [self waitForHostInputReady];
    on_main(^{
        XCTAssertFalse(find_controls(UIApplication.sharedApplication.delegate.window.rootViewController.view).hidden);
        NSFileManager *files = NSFileManager.defaultManager;
        [files removeItemAtPath:[romfs stringByAppendingPathComponent:@"Data"] error:NULL];
        sky_ios_set_setup_message(sky_ios_check_game_files());
        XCTAssertTrue(find_controls(UIApplication.sharedApplication.delegate.window.rootViewController.view).hidden);
        XCTAssertTrue([files createDirectoryAtPath:[romfs stringByAppendingPathComponent:@"Data"] withIntermediateDirectories:YES attributes:nil error:NULL]);
        [UIApplication.sharedApplication.delegate applicationWillResignActive:UIApplication.sharedApplication];
        [UIApplication.sharedApplication.delegate applicationDidBecomeActive:UIApplication.sharedApplication];
    });
    [self waitForHostInputReady];
    on_main(^{
        XCTAssertFalse(find_controls(UIApplication.sharedApplication.delegate.window.rootViewController.view).hidden);
        if (oldRomfs) setenv("SKY_ROMFS", oldRomfs.UTF8String, 1); else unsetenv("SKY_ROMFS");
        if (oldSave) setenv("SKY_SAVE_DIR", oldSave.UTF8String, 1); else unsetenv("SKY_SAVE_DIR");
        if (oldShader) setenv("SKY_SHADER_CACHE_DIR", oldShader.UTF8String, 1); else unsetenv("SKY_SHADER_CACHE_DIR");
        [NSFileManager.defaultManager removeItemAtPath:temporary error:NULL];
        sky_ios_set_setup_message(NULL);
    });
}
- (void)testKeyboardCancellationDuringPresentationAllowsANewRequest {
    [self makeHostWindowKey];
    sky_keyboard_request_t request = {"Interrupted name", "", "", "", 0, 20};
    XCTestExpectation *interrupted = [self expectationWithDescription:@"Background reaches an in-flight UIKit presentation"];
    XCTestExpectation *first = [self expectationWithDescription:@"First guest request cancels without waiting for animation"];
    __block int firstResult = -1;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char output[64]; firstResult = sky_ios_present_keyboard(&request, output, sizeof output); [first fulfill];
    });
    __block unsigned polls = 0;
    __block dispatch_block_t observePresentation;
    observePresentation = ^{
        UIViewController *root = UIApplication.sharedApplication.delegate.window.rootViewController;
        if ([root.presentedViewController isKindOfClass:UIAlertController.class] && root.presentedViewController.isBeingPresented) {
            [UIApplication.sharedApplication.delegate applicationWillResignActive:UIApplication.sharedApplication];
            [UIApplication.sharedApplication.delegate applicationDidEnterBackground:UIApplication.sharedApplication];
            [UIApplication.sharedApplication.delegate applicationDidBecomeActive:UIApplication.sharedApplication];
            [interrupted fulfill];
            observePresentation = nil;
        } else if (++polls >= 400) {
            XCTFail(@"No native alert presentation transition was observed");
            sky_ios_keyboard_respond(NULL, 0);
            [interrupted fulfill];
            observePresentation = nil;
        } else dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), dispatch_get_main_queue(), observePresentation);
    };
    dispatch_async(dispatch_get_main_queue(), observePresentation);
    [self waitForExpectations:@[interrupted, first] timeout:8];
    XCTAssertEqual(firstResult, 0);
    [self waitForHostInputReady];
    NSPredicate *dismissed = [NSPredicate predicateWithBlock:^BOOL(id object, NSDictionary *bindings) {
        (void)object; (void)bindings;
        __block BOOL gone;
        on_main(^{ gone = UIApplication.sharedApplication.delegate.window.rootViewController.presentedViewController == nil; });
        return gone;
    }];
    XCTNSPredicateExpectation *noOrphan = [[XCTNSPredicateExpectation alloc] initWithPredicate:dismissed object:nil];
    [self waitForExpectations:@[noOrphan] timeout:4];
    XCTestExpectation *second = [self expectationWithDescription:@"A subsequent native dialog can complete"];
    __block int secondResult = -1;
    __block NSString *secondText;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char output[64]; secondResult = sky_ios_present_keyboard(&request, output, sizeof output);
        secondText = [NSString stringWithUTF8String:output]; [second fulfill];
    });
    [self waitForKeyboard];
    on_main(^{ XCTAssertTrue(sky_ios_keyboard_respond("New name", 1)); });
    [self waitForExpectations:@[second] timeout:8];
    XCTAssertEqual(secondResult, 1); XCTAssertEqualObjects(secondText, @"New name");
    [self waitForHostInputReady];
}
@end
