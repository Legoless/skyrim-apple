#import <UIKit/UIKit.h>
#import "../touch.h"

/* Every event stays on UIKit's main thread. This view only changes the Touch
 * HID source; losing focus never releases a desktop or external controller. */
@interface SkyTouchControlsView : UIView
- (void)cancelAllTouches;
/* Per-group fade state, stepped by the frontend's 60 Hz UI timer on the main
 * thread. In game, the action buttons and Pause fade in and stay visible; the
 * Move and Look sticks are drawn only while a finger holds them, centered
 * where that finger landed. In-game menus show only Back, in Pause's spot.
 * Test mode snaps to the gameplay set for deterministic screenshots. */
- (void)updateVisibilityInGame:(BOOL)inGame testMode:(BOOL)testMode;
- (void)touchSnapshot:(sky_touch_t *)snapshot;
/* Deterministic simulator/UI-test hook using the same path as real UITouch.
 * phase: 0 began, 1 moved, 2 ended, 3 cancelled. Main thread only. */
- (BOOL)injectTouchPhase:(unsigned)phase identifier:(uint64_t)identifier point:(CGPoint)point;
@end
