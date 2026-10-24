#import <XCTest/XCTest.h>
#import <UIKit/UIKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import "GameFrameReadiness.h"

/* Optional production-game smoke test. It uses only XCUITest device gestures:
 * no guest fixtures, presenter injection, HID hooks, or controls-test mode.
 * Screenshots require human review; surviving these taps does not establish
 * that a title/menu/intro transition actually occurred. Existing saves can
 * change which menu action the same sequence selects. */
@interface GameControlsUITests : XCTestCase
@property(nonatomic, strong) XCUIApplication *game;
@property(nonatomic, assign) NSTimeInterval deadline;
@end

@implementation GameControlsUITests
- (void)setUp {
    [super setUp];
    self.continueAfterFailure = NO;
    XCUIDevice.sharedDevice.orientation = UIDeviceOrientationLandscapeLeft;
    /* Enable test timeouts and a 95-second maximum in the xcodebuild command. */
    self.executionTimeAllowance = 95;
}
- (void)capture:(NSString *)stage {
    UIImage *image = sky_smoke_normalized_image(XCUIScreen.mainScreen.screenshot.image);
    XCTAttachment *attachment = [XCTAttachment attachmentWithImage:image];
    attachment.name = stage;
    attachment.lifetime = XCTAttachmentLifetimeKeepAlways;
    [self addAttachment:attachment];
}
- (BOOL)requireRunningGame {
    if (self.game.state != XCUIApplicationStateRunningForeground) {
        [self capture:@"failure_app_not_foreground"];
        XCTFail(@"The normal game app crashed, exited, or left the foreground.");
        return NO;
    }
    XCUIElement *setup = [self.game descendantsMatchingType:XCUIElementTypeAny]
        [@"sky.setup.message"];
    if (setup.exists) {
        [self capture:@"failure_game_files_or_setup_blocked"];
        XCTFail(@"The app is showing setup instructions; the real game files must be present before this smoke test.");
        return NO;
    }
    if (self.game.staticTexts[@"The game stopped."].exists || self.game.staticTexts[@"The game finished."].exists) {
        [self capture:@"failure_guest_stopped"];
        XCTFail(@"The guest stopped during the production-game smoke test.");
        return NO;
    }
    if (NSProcessInfo.processInfo.systemUptime >= self.deadline) {
        [self capture:@"failure_smoke_time_budget_exhausted"];
        XCTFail(@"The configured smoke-test time budget was exhausted.");
        return NO;
    }
    return YES;
}
- (BOOL)observeGameFor:(NSTimeInterval)duration {
    NSTimeInterval until = NSProcessInfo.processInfo.systemUptime + duration;
    while (NSProcessInfo.processInfo.systemUptime < until) {
        if (![self requireRunningGame]) return NO;
        NSTimeInterval remaining = until - NSProcessInfo.processInfo.systemUptime;
        if (remaining > 0) [NSThread sleepForTimeInterval:MIN(.5, remaining)];
    }
    return [self requireRunningGame];
}
- (BOOL)tapControl:(NSString *)identifier {
    if (![self requireRunningGame]) return NO;
    XCUIElement *control = [self.game descendantsMatchingType:XCUIElementTypeAny][identifier];
    if (![control waitForExistenceWithTimeout:3] || !control.isHittable) {
        [self capture:[@"failure_control_unavailable_" stringByAppendingString:identifier]];
        XCTFail(@"The native on-screen control %@ is absent or cannot be touched.", identifier);
        return NO;
    }
    /* A coordinate gesture explicitly sends real touches through UIKit. It
     * does not invoke the custom accessibilityActivate implementation. */
    XCUICoordinate *coordinate = [control coordinateWithNormalizedOffset:CGVectorMake(.5, .5)];
    if ([identifier isEqualToString:@"sky.touch.pause"]) [coordinate pressForDuration:0.3];
    else [coordinate tap];
    return [self requireRunningGame];
}
- (BOOL)waitForRenderedGameplay {
    XCUIElement *pause = [self.game descendantsMatchingType:XCUIElementTypeAny][@"sky.touch.pause"];
    NSInteger lastOrientation = -1;
    CGSize lastRawSize = CGSizeZero;
    for (;;) {
        if (![self requireRunningGame]) return NO;
        if (pause.exists && pause.isHittable) {
            UIImage *image = XCUIScreen.mainScreen.screenshot.image;
            CGImageRef raw = image.CGImage;
            size_t width = raw ? CGImageGetWidth(raw) : 0, height = raw ? CGImageGetHeight(raw) : 0;
            if (lastOrientation != image.imageOrientation || lastRawSize.width != width || lastRawSize.height != height) {
                UIImage *normalized = sky_smoke_normalized_image(image);
                NSLog(@"Smoke readiness frame: orientation=%ld image=%@ raw=%zux%zu normalized=%zux%zu",
                    (long)image.imageOrientation, NSStringFromCGSize(image.size), width, height,
                    CGImageGetWidth(normalized.CGImage), CGImageGetHeight(normalized.CGImage));
                lastOrientation = image.imageOrientation;
                lastRawSize = CGSizeMake(width, height);
            }
            if (sky_smoke_image_has_scene_content(image)) return YES;
        }
        if (![self observeGameFor:2]) return NO;
    }
}
- (BOOL)requireOpenGameMenu {
    if (![self requireRunningGame]) return NO;
    XCUIElement *back = [self.game descendantsMatchingType:XCUIElementTypeAny][@"sky.touch.back"];
    if (back.exists && back.isHittable) return YES;
    [self capture:@"failure_game_menu_not_open"];
    XCTFail(@"The guest has not exposed its in-game menu Back control; tab taps would target the world.");
    return NO;
}
- (BOOL)waitForOpenGameMenu {
    XCUIElement *back = [self.game descendantsMatchingType:XCUIElementTypeAny][@"sky.touch.back"];
    for (;;) {
        if (![self requireRunningGame]) return NO;
        if (back.exists && back.isHittable) return YES;
        if (![self observeGameFor:.5]) return NO;
    }
}
- (void)testNativeTouchSmokeCapturesForVisualReview {
    NSDictionary<NSString *, NSString *> *environment = NSProcessInfo.processInfo.environment;
    XCTAssertFalse([environment[@"SKY_CONTROLS_TEST_MODE"] isEqualToString:@"1"],
        @"Run the separate production-game smoke scheme, not the controls-test host scheme.");
    NSString *bundleIdentifier = environment[@"SKY_GAME_APP_BUNDLE_ID"];
    if (!bundleIdentifier.length)
        bundleIdentifier = [[NSBundle bundleForClass:self.class] objectForInfoDictionaryKey:@"SkyGameAppBundleIdentifier"];
    XCTAssertTrue(bundleIdentifier.length > 0, @"The app-under-test bundle identifier must be configured.");
    self.game = [[XCUIApplication alloc] initWithBundleIdentifier:bundleIdentifier];
    self.game.launchArguments = @[];
    self.game.launchEnvironment = @{};
    self.deadline = NSProcessInfo.processInfo.systemUptime + 90;
    [self.game launch];
    if (![self requireRunningGame]) return;
    [self capture:@"00_normal_app_launched"];

    /* The game image is GPU-rendered and has no native title/menu accessibility
     * markers. These are bounded observation periods, not stage assertions. */
    if (![self observeGameFor:30]) return;
    [self capture:@"01_after_30s_startup_wait_review_title"];
    if (![self tapControl:@"sky.touch.use"] || ![self observeGameFor:6]) return;
    [self capture:@"02_after_first_use_review_menu"];
    if (![self tapControl:@"sky.touch.use"] || ![self observeGameFor:6]) return;
    [self capture:@"03_after_second_use_review_confirmation"];
    if (![self tapControl:@"sky.touch.use"] || ![self observeGameFor:30]) return;
    [self capture:@"04_after_third_use_and_30s_review_game"];

    XCTAttachment *limits = [XCTAttachment attachmentWithString:
        @"The normal app stayed in the foreground and accepted three native coordinate gestures. "
         "Inspect every screenshot to determine the actual game stages and whether the guest responded. "
         "This test does not classify rendered images or prove main-menu, New Game, intro, or gameplay success."];
    limits.name = @"Smoke-test interpretation";
    limits.lifetime = XCTAttachmentLifetimeKeepAlways;
    [self addAttachment:limits];
}

/* Drives the main menu with real screen taps: Touch to begin, then Continue
 * (default) or New (SKY_MENU_ACTION=new), then Yes in the confirm dialog, and
 * keeps capturing while the world loads. Coordinates are normalized to the
 * full-screen game view, matching where the StartMenu entries render. The game
 * log goes to Documents/ui-automation.log (pull it with devicectl). */
- (void)testMenuTouchStartsGame {
    NSDictionary<NSString *, NSString *> *environment = NSProcessInfo.processInfo.environment;
    NSString *bundleIdentifier = [[NSBundle bundleForClass:self.class] objectForInfoDictionaryKey:@"SkyGameAppBundleIdentifier"];
    XCTAssertTrue(bundleIdentifier.length > 0, @"The app-under-test bundle identifier must be configured.");
    BOOL newGame = [environment[@"SKY_MENU_ACTION"] isEqualToString:@"new"];
    NSTimeInterval loadObserve = environment[@"SKY_LOAD_OBSERVE"].doubleValue;
    if (loadObserve <= 0) loadObserve = 120;
    self.executionTimeAllowance = 90 + loadObserve;
    self.game = [[XCUIApplication alloc] initWithBundleIdentifier:bundleIdentifier];
    self.game.launchArguments = @[];
    self.game.launchEnvironment = @{@"SKY_LOG_FILE": @"ui-automation.log"};
    self.deadline = NSProcessInfo.processInfo.systemUptime + 80 + loadObserve;
    [self.game launch];
    if (![self requireRunningGame]) return;

    if (![self observeGameFor:45]) return;
    [self capture:@"01_title"];
    [[self.game coordinateWithNormalizedOffset:CGVectorMake(.5, .5)] tap];
    if (![self observeGameFor:8]) return;
    [self capture:@"02_main_menu"];
    CGVector entry = newGame ? CGVectorMake(.92, .77) : CGVectorMake(.92, .72);
    [[self.game coordinateWithNormalizedOffset:entry] tap];
    if (![self observeGameFor:3]) return;
    [self capture:newGame ? @"03_confirm_new" : @"03_confirm_continue"];
    [[self.game coordinateWithNormalizedOffset:CGVectorMake(.70, .89)] tap];
    for (int elapsed = 10; elapsed <= (int)loadObserve; elapsed += 10) {
        if (![self observeGameFor:10]) return;
        [self capture:[NSString stringWithFormat:@"04_after_yes_%03ds", elapsed]];
    }
}

/* Taps a point given in normalized 16:9 game coordinates; the game view is
 * aspect-fit inside the app window. */
- (BOOL)tapGameX:(CGFloat)x y:(CGFloat)y {
    if (![self requireRunningGame]) return NO;
    CGSize size = self.game.frame.size;
    if (size.width <= size.height) {
        [self capture:@"failure_game_coordinate_space_not_landscape"];
        XCTFail(@"Game-coordinate gestures require a landscape app frame.");
        return NO;
    }
    CGFloat scale = MIN(size.width / 16.0, size.height / 9.0);
    CGFloat gw = scale * 16, gh = scale * 9;
    CGVector offset = CGVectorMake(((size.width - gw) * .5 + x * gw) / size.width,
                                   ((size.height - gh) * .5 + y * gh) / size.height);
    [[self.game coordinateWithNormalizedOffset:offset] tap];
    return [self requireRunningGame];
}

/* Continues the latest save (startup auto-continue), opens the Journal with
 * the on-screen Pause button and taps each top tab. The tab positions come
 * from quest_journal.gfx on its 1280x720 stage. On a save that has not shown
 * the Journal tutorial yet, the first tap dismisses that popup. Captures need
 * review together with the menu touch lines in Documents/ui-automation.log.
 * Pause can appear before the loading fade has produced a world image, so
 * readiness also requires nonblank viewport pixels. Back proves a real guest
 * menu is open; it does not classify the Journal or its selected tab. */
- (void)testJournalTabsSwitchWithTouch {
    NSString *bundleIdentifier = [[NSBundle bundleForClass:self.class] objectForInfoDictionaryKey:@"SkyGameAppBundleIdentifier"];
    XCTAssertTrue(bundleIdentifier.length > 0, @"The app-under-test bundle identifier must be configured.");
    self.executionTimeAllowance = 420;
    self.game = [[XCUIApplication alloc] initWithBundleIdentifier:bundleIdentifier];
    self.game.launchArguments = @[];
    self.game.launchEnvironment = @{@"SKY_LOG_FILE": @"ui-automation.log"};
    self.deadline = NSProcessInfo.processInfo.systemUptime + 400;
    [self.game launch];
    if (![self requireRunningGame]) return;

    if (![self waitForRenderedGameplay]) return;
    [self capture:@"01_gameplay"];
    if (![self tapControl:@"sky.touch.pause"] || ![self waitForOpenGameMenu] ||
        ![self observeGameFor:6] || ![self requireOpenGameMenu]) return;
    [self capture:@"02_journal_opened"];

    const CGFloat tabY = 69.0 / 720.0;
    if (![self requireOpenGameMenu] || ![self tapGameX:641.0 / 1280.0 y:tabY] ||
        ![self observeGameFor:5] || ![self requireOpenGameMenu]) return;
    [self capture:@"03_after_first_tab_tap"];
    if (![self requireOpenGameMenu] || ![self tapGameX:641.0 / 1280.0 y:tabY] ||
        ![self observeGameFor:5] || ![self requireOpenGameMenu]) return;
    [self capture:@"04_after_general_stats_tap"];
    if (![self requireOpenGameMenu] || ![self tapGameX:375.0 / 1280.0 y:tabY] ||
        ![self observeGameFor:5] || ![self requireOpenGameMenu]) return;
    [self capture:@"05_after_quests_tap"];
    if (![self requireOpenGameMenu] || ![self tapGameX:905.0 / 1280.0 y:tabY] ||
        ![self observeGameFor:5] || ![self requireOpenGameMenu]) return;
    [self capture:@"06_after_system_tap"];
}
@end
