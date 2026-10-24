#import "TouchControlsView.h"
#import "../hid.h"
#import "../skyhost.h"
#import <QuartzCore/QuartzCore.h>
#include <math.h>

/* Skyrim's interface draws thin ivory lines on translucent black. */
static UIColor *ivory(CGFloat alpha) {
    return [UIColor colorWithRed:.93 green:.91 blue:.86 alpha:alpha];
}
static void fill_diamond(CGContextRef context, CGFloat x, CGFloat y, CGFloat half) {
    CGContextMoveToPoint(context, x, y - half);
    CGContextAddLineToPoint(context, x + half, y);
    CGContextAddLineToPoint(context, x, y + half);
    CGContextAddLineToPoint(context, x - half, y);
    CGContextClosePath(context);
    CGContextFillPath(context);
}

@class SkyTouchControlsView;
@interface SkyTouchAccessibilityElement : UIAccessibilityElement
@property(nonatomic, weak) SkyTouchControlsView *owner;
@property(nonatomic, assign) sky_touch_control_t control;
@end

@interface SkyTouchControlsView () {
    sky_touch_t _touch;
    CGSize _layoutSize;
    UIEdgeInsets _layoutInsets;
    NSArray<SkyTouchAccessibilityElement *> *_controlElements;
    UIImage *_useIcon;
    CGFloat _gameOpacity; /* Gameplay buttons, plus the sticks while touched. */
    CGFloat _backOpacity; /* Back in menus and the generic controller layout. */
    BOOL _wasInMenu;
    CFTimeInterval _lastVisibilityTick;
}
- (void)publishTouch;
- (BOOL)usesGenericControllerUI;
- (void)updateTouchMode;
@end

@implementation SkyTouchAccessibilityElement
- (BOOL)accessibilityActivate {
    if (self.control <= SKY_TOUCH_LOOK || !self.owner.userInteractionEnabled) return NO;
    sky_touch_t snapshot;
    [self.owner touchSnapshot:&snapshot];
    if (snapshot.menu != 2 && (self.control == SKY_TOUCH_BACK) != (snapshot.menu == 1)) return NO;
    const sky_touch_region_t *region = sky_touch_region(&snapshot, self.control);
    if (!region || !snapshot.enabled) return NO;
    uint64_t identifier = UINT64_MAX - self.control;
    CGPoint point = CGPointMake(region->x, region->y);
    BOOL accepted = [self.owner injectTouchPhase:0 identifier:identifier point:point];
    [self.owner injectTouchPhase:2 identifier:identifier point:point];
    return accepted;
}
@end

@implementation SkyTouchControlsView
- (BOOL)usesGenericControllerUI {
    return sky_main_module_base() != 0 && !sky_profile_has_update_ui();
}
- (void)updateTouchMode {
    int mode = [self usesGenericControllerUI] ? 2 : sky_ui_is_in_game() == 2;
    if (_touch.menu == (unsigned)mode) return;
    sky_touch_set_menu(&_touch, mode);
    const sky_touch_region_t *back = sky_touch_region(&_touch, SKY_TOUCH_BACK);
    _controlElements[SKY_TOUCH_BACK].accessibilityFrameInContainerSpace =
        CGRectMake(back->x - back->radius, back->y - back->radius, 2 * back->radius, 2 * back->radius);
    [self setNeedsDisplay];
}
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.opaque = NO;
        self.backgroundColor = UIColor.clearColor;
        self.multipleTouchEnabled = YES;
        self.accessibilityIdentifier = @"sky.touch.controls";
        self.isAccessibilityElement = NO;
        _layoutSize = CGSizeMake(-1, -1);
        static NSString * const identifiers[SKY_TOUCH_CONTROL_COUNT] = {
            @"move", @"look", @"use", @"back", @"jump", @"sneak", @"attack", @"pause",
        };
        static NSString * const labels[SKY_TOUCH_CONTROL_COUNT] = {
            @"Move", @"Look", @"Use", @"Close menu", @"Jump", @"Sneak", @"Attack", @"Pause",
        };
        NSMutableArray *elements = [NSMutableArray arrayWithCapacity:SKY_TOUCH_CONTROL_COUNT];
        for (int i = 0; i < SKY_TOUCH_CONTROL_COUNT; ++i) {
            SkyTouchAccessibilityElement *element = [[SkyTouchAccessibilityElement alloc] initWithAccessibilityContainer:self];
            element.owner = self;
            element.control = i;
            element.accessibilityIdentifier = [@"sky.touch." stringByAppendingString:identifiers[i]];
            element.accessibilityLabel = labels[i];
            element.accessibilityTraits = i <= SKY_TOUCH_LOOK ? UIAccessibilityTraitNone : UIAccessibilityTraitButton;
            if (i <= SKY_TOUCH_LOOK) element.accessibilityHint = @"Drag to control direction";
            [elements addObject:element];
        }
        _controlElements = elements;
    }
    return self;
}
- (NSArray *)accessibilityElements {
    [self updateTouchMode];
    BOOL menu = _touch.menu == 1;
    NSMutableArray *elements = [NSMutableArray arrayWithCapacity:_controlElements.count];
    for (SkyTouchAccessibilityElement *element in _controlElements)
        if (_touch.menu == 2 || (element.control == SKY_TOUCH_BACK) == menu) [elements addObject:element];
    return elements;
}
- (void)publishTouch {
    sky_hid_queue_source_state(SKY_HID_SOURCE_TOUCH, _touch.buttons,
        _touch.lx, _touch.ly, _touch.rx, _touch.ry);
    for (int i = 0; i < SKY_TOUCH_CONTROL_COUNT; ++i)
        _controlElements[i].accessibilityValue = sky_touch_control_active(&_touch, i) ? @"Pressed" : @"Released";
    [self setNeedsDisplay];
}
- (void)cancelAllTouches {
    sky_touch_cancel(&_touch);
    /* Raw reset also removes pending presses so a background/rotation event
     * cannot leave a queued attack to play after the app becomes active. */
    sky_hid_set_source_state(SKY_HID_SOURCE_TOUCH, 0, 0, 0, 0, 0);
    for (SkyTouchAccessibilityElement *element in _controlElements) element.accessibilityValue = @"Released";
    [self setNeedsDisplay];
}
- (void)layoutSubviews {
    [super layoutSubviews];
    UIEdgeInsets safe = self.safeAreaInsets;
    if (CGSizeEqualToSize(_layoutSize, self.bounds.size) && UIEdgeInsetsEqualToEdgeInsets(_layoutInsets, safe)) return;
    _layoutSize = self.bounds.size;
    _layoutInsets = safe;
    [self cancelAllTouches];
    sky_touch_resize(&_touch, self.bounds.size.width, self.bounds.size.height,
        (sky_touch_insets_t){safe.top, safe.left, safe.bottom, safe.right});
    [self updateTouchMode];
    for (int i = 0; i < SKY_TOUCH_CONTROL_COUNT; ++i) {
        const sky_touch_region_t *r = sky_touch_region(&_touch, i);
        _controlElements[i].accessibilityFrameInContainerSpace =
            CGRectMake(r->x - r->radius, r->y - r->radius, 2 * r->radius, 2 * r->radius);
    }
    const CGFloat useRadius = sky_touch_region(&_touch, SKY_TOUCH_USE)->radius;
    _useIcon = useRadius <= 0 ? nil : [[UIImage systemImageNamed:@"hand.raised.fill"
        withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:useRadius weight:UIImageSymbolWeightSemibold]]
        imageWithTintColor:ivory(1) renderingMode:UIImageRenderingModeAlwaysOriginal];
    [self setNeedsDisplay];
}
- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    [self cancelAllTouches];
    [self setNeedsLayout];
}
- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (!self.window) [self cancelAllTouches];
}
- (void)touchSnapshot:(sky_touch_t *)snapshot {
    NSAssert(NSThread.isMainThread, @"Touch snapshots must run on the main thread");
    [self updateTouchMode];
    if (snapshot) *snapshot = _touch;
}
- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent *)event {
    if (![super pointInside:point withEvent:event]) return NO;
    [self updateTouchMode];
    if (_touch.menu != 1) return YES;
    return sky_touch_hit_test(&_touch, point.x, point.y) == SKY_TOUCH_BACK;
}
- (BOOL)injectTouchPhase:(unsigned)phase identifier:(uint64_t)identifier point:(CGPoint)point {
    if (!NSThread.isMainThread || ((phase == 0 || phase == 1) && !self.userInteractionEnabled)) return NO;
    BOOL accepted = YES;
    switch (phase) {
    case 0:
        [self updateTouchMode];
        accepted = sky_touch_begin(&_touch, identifier, point.x, point.y);
        break;
    case 1: sky_touch_move(&_touch, identifier, point.x, point.y); break;
    case 2: sky_touch_end(&_touch, identifier); break;
    case 3: [self cancelAllTouches]; return YES;
    default: return NO;
    }
    [self publishTouch];
    return accepted;
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for (UITouch *touch in touches)
        [self injectTouchPhase:0 identifier:(uintptr_t)(__bridge void *)touch point:[touch locationInView:self]];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for (UITouch *touch in touches)
        [self injectTouchPhase:1 identifier:(uintptr_t)(__bridge void *)touch point:[touch locationInView:self]];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for (UITouch *touch in touches)
        [self injectTouchPhase:2 identifier:(uintptr_t)(__bridge void *)touch point:[touch locationInView:self]];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    /* UIKit can cancel a subset while other fingers remain on the screen. */
    for (UITouch *touch in touches)
        [self injectTouchPhase:2 identifier:(uintptr_t)(__bridge void *)touch point:[touch locationInView:self]];
}
- (void)updateVisibilityInGame:(BOOL)inGame testMode:(BOOL)testMode {
    [self updateTouchMode];
    const CFTimeInterval now = CACurrentMediaTime();
    CFTimeInterval dt = _lastVisibilityTick ? now - _lastVisibilityTick : 0;
    if (dt < 0 || dt > 0.1) dt = dt < 0 ? 0 : 0.1;
    _lastVisibilityTick = now;

    BOOL generic = [self usesGenericControllerUI];
    BOOL inMenu = !generic && !testMode && inGame && sky_ui_is_in_game() == 2;
    if (inMenu != _wasInMenu) {
        _wasInMenu = inMenu;
        [self cancelAllTouches];
    }

    /* Leaving the game resets to invisible so the next entry fades in. */
    const BOOL snap = testMode || !inGame;
    const CGFloat gameTarget = (testMode || (inGame && !inMenu)) ? 1 : 0;
    const CGFloat backTarget = generic ? gameTarget : (inMenu ? 1 : 0);
    const CGFloat beforeGame = _gameOpacity, beforeBack = _backOpacity;
    if (snap) {
        _gameOpacity = gameTarget;
        _backOpacity = backTarget;
    } else {
        const CGFloat gameStep = (CGFloat)(dt / 0.30);
        const CGFloat backStep = (CGFloat)(dt / (backTarget > _backOpacity ? 0.12 : 0.35));
        _gameOpacity += MIN(MAX(gameTarget - _gameOpacity, -gameStep), gameStep);
        _backOpacity += MIN(MAX(backTarget - _backOpacity, -backStep), backStep);
    }
    if (fabs(_gameOpacity - beforeGame) > 0.001 || fabs(_backOpacity - beforeBack) > 0.001)
        [self setNeedsDisplay];
}
/* Glyphs fit a box of side s centered on c. */
- (void)drawGlyph:(sky_touch_control_t)control center:(CGPoint)c size:(CGFloat)s
            alpha:(CGFloat)alpha context:(CGContextRef)context {
    CGContextSaveGState(context);
    CGContextSetFillColorWithColor(context, ivory(alpha).CGColor);
    CGContextSetStrokeColorWithColor(context, ivory(alpha).CGColor);
    CGContextSetLineCap(context, kCGLineCapRound);
    CGContextSetLineJoin(context, kCGLineJoinRound);
    switch (control) {
    case SKY_TOUCH_ATTACK: {
        /* A sword, point up and to the right. */
        const CGFloat length = s * .62, blade = s * .075, guard = length * .2;
        CGContextTranslateCTM(context, c.x, c.y);
        CGContextRotateCTM(context, M_PI_4);
        CGContextMoveToPoint(context, 0, -length);
        CGContextAddLineToPoint(context, blade, -length + blade * 2.4);
        CGContextAddLineToPoint(context, blade, guard);
        CGContextAddLineToPoint(context, -blade, guard);
        CGContextAddLineToPoint(context, -blade, -length + blade * 2.4);
        CGContextClosePath(context);
        CGContextFillPath(context);
        CGContextFillRect(context, CGRectMake(-s * .22, guard, s * .44, s * .07));
        CGContextFillRect(context, CGRectMake(-s * .04, guard + s * .07, s * .08, s * .24));
        CGContextFillEllipseInRect(context, CGRectMake(-s * .065, guard + s * .3, s * .13, s * .13));
        break;
    }
    case SKY_TOUCH_SNEAK: {
        /* The sneak eye: a pointed almond around a round pupil. */
        const CGFloat width = s * .48, height = s * .22;
        CGContextSetLineJoin(context, kCGLineJoinMiter);
        CGContextSetLineWidth(context, s * .075);
        CGContextMoveToPoint(context, c.x - width, c.y);
        CGContextAddQuadCurveToPoint(context, c.x, c.y - height * 2.1, c.x + width, c.y);
        CGContextAddQuadCurveToPoint(context, c.x, c.y + height * 2.1, c.x - width, c.y);
        CGContextClosePath(context);
        CGContextStrokePath(context);
        CGContextFillEllipseInRect(context, CGRectMake(c.x - height * .8, c.y - height * .8, height * 1.6, height * 1.6));
        break;
    }
    case SKY_TOUCH_JUMP:
        CGContextSetLineWidth(context, s * .1);
        for (int i = 0; i < 2; ++i) {
            const CGFloat y = c.y + (i ? .16 : -.12) * s;
            CGContextMoveToPoint(context, c.x - s * .3, y + s * .12);
            CGContextAddLineToPoint(context, c.x, y - s * .16);
            CGContextAddLineToPoint(context, c.x + s * .3, y + s * .12);
            CGContextStrokePath(context);
        }
        break;
    case SKY_TOUCH_PAUSE:
        CGContextFillRect(context, CGRectMake(c.x - s * .19, c.y - s * .25, s * .13, s * .5));
        CGContextFillRect(context, CGRectMake(c.x + s * .06, c.y - s * .25, s * .13, s * .5));
        break;
    case SKY_TOUCH_BACK:
        CGContextSetLineWidth(context, s * .09);
        CGContextMoveToPoint(context, c.x - s * .24, c.y - s * .24);
        CGContextAddLineToPoint(context, c.x + s * .24, c.y + s * .24);
        CGContextMoveToPoint(context, c.x + s * .24, c.y - s * .24);
        CGContextAddLineToPoint(context, c.x - s * .24, c.y + s * .24);
        CGContextStrokePath(context);
        break;
    case SKY_TOUCH_USE:
        if (_useIcon.size.width > 0 && _useIcon.size.height > 0) {
            const CGFloat scale = MIN(s * .8 / _useIcon.size.width, s * .8 / _useIcon.size.height);
            const CGSize size = CGSizeMake(_useIcon.size.width * scale, _useIcon.size.height * scale);
            [_useIcon drawInRect:CGRectMake(c.x - size.width * .5, c.y - size.height * .5, size.width, size.height)
                       blendMode:kCGBlendModeNormal alpha:alpha];
        }
        break;
    default:
        break;
    }
    CGContextRestoreGState(context);
}
/* Square buttons with corner brackets, after Skyrim's menu frames. */
- (void)drawButton:(sky_touch_control_t)control opacity:(CGFloat)opacity context:(CGContextRef)context {
    const sky_touch_region_t *r = sky_touch_region(&_touch, control);
    const BOOL pressed = sky_touch_control_active(&_touch, control);
    const CGFloat half = r->radius * .86, line = MAX(1.5, r->radius * .055);
    const CGRect frame = CGRectMake(r->x - half, r->y - half, 2 * half, 2 * half);
    CGContextSetFillColorWithColor(context,
        (pressed ? ivory(.2 * opacity) : [UIColor colorWithWhite:0 alpha:.55 * opacity]).CGColor);
    CGContextFillRect(context, frame);
    const CGFloat inset = MAX(2.5, r->radius * .08);
    CGContextSetStrokeColorWithColor(context, ivory((pressed ? .7 : .35) * opacity).CGColor);
    CGContextSetLineWidth(context, 1);
    CGContextStrokeRect(context, CGRectInset(frame, inset, inset));

    const CGFloat corner = half + (pressed ? line : 0), arm = half * .55;
    CGContextSetStrokeColorWithColor(context, ivory((pressed ? 1 : .92) * opacity).CGColor);
    CGContextSetLineWidth(context, line);
    CGContextSetLineCap(context, kCGLineCapSquare);
    for (int i = 0; i < 4; ++i) {
        const CGFloat sx = i & 1 ? 1 : -1, sy = i & 2 ? 1 : -1;
        const CGFloat x = r->x + sx * corner, y = r->y + sy * corner;
        CGContextMoveToPoint(context, x, y - sy * arm);
        CGContextAddLineToPoint(context, x, y);
        CGContextAddLineToPoint(context, x - sx * arm, y);
        CGContextStrokePath(context);
    }
    [self drawGlyph:control center:CGPointMake(r->x, r->y) size:r->radius * (pressed ? .95 : 1.05)
              alpha:.95 * opacity context:context];
}
/* A compass-style ring with diamonds marking the four directions. */
- (void)drawStick:(sky_touch_control_t)control opacity:(CGFloat)opacity context:(CGContextRef)context {
    const sky_touch_region_t *r = sky_touch_region(&_touch, control);
    const CGFloat line = MAX(1.5, r->radius * .014), mark = MAX(4, r->radius * .055);
    const CGRect base = CGRectMake(r->x - r->radius, r->y - r->radius, 2 * r->radius, 2 * r->radius);
    CGContextSetFillColorWithColor(context, [UIColor colorWithWhite:0 alpha:.25 * opacity].CGColor);
    CGContextFillEllipseInRect(context, base);
    CGContextSetStrokeColorWithColor(context, ivory(.55 * opacity).CGColor);
    CGContextSetLineWidth(context, line);
    CGContextStrokeEllipseInRect(context, base);
    CGContextSetFillColorWithColor(context, ivory(.9 * opacity).CGColor);
    fill_diamond(context, r->x, r->y - r->radius, mark);
    fill_diamond(context, r->x + r->radius, r->y, mark);
    fill_diamond(context, r->x, r->y + r->radius, mark);
    fill_diamond(context, r->x - r->radius, r->y, mark);

    /* The knob tracks the finger, scaled so full deflection touches the rim. */
    const CGFloat knob = r->radius * .36;
    CGFloat dx = 0, dy = 0;
    for (size_t f = 0; f < SKY_TOUCH_MAX_FINGERS; ++f) {
        const sky_touch_finger_t *finger = &_touch.fingers[f];
        if (!finger->used || finger->control != control) continue;
        dx = finger->x - r->x; dy = finger->y - r->y;
    }
    const CGFloat length = hypot(dx, dy), travel = r->radius - knob;
    if (length > 0) {
        const CGFloat scale = MIN(length, r->radius) / r->radius * travel / length;
        dx *= scale; dy *= scale;
    }
    const CGRect thumb = CGRectMake(r->x + dx - knob, r->y + dy - knob, 2 * knob, 2 * knob);
    CGContextSetFillColorWithColor(context, ivory(.22 * opacity).CGColor);
    CGContextFillEllipseInRect(context, thumb);
    CGContextSetStrokeColorWithColor(context, ivory(.9 * opacity).CGColor);
    CGContextSetLineWidth(context, MAX(1.5, r->radius * .015));
    CGContextStrokeEllipseInRect(context, CGRectInset(thumb, line * .5, line * .5));
    CGContextSetFillColorWithColor(context, ivory(.9 * opacity).CGColor);
    fill_diamond(context, r->x + dx, r->y + dy, mark * .8);
}
- (void)drawRect:(CGRect)rect {
    (void)rect;
    if (!_touch.enabled || (_gameOpacity <= 0.004 && _backOpacity <= 0.004)) return;
    CGContextRef context = UIGraphicsGetCurrentContext();
    for (int i = 0; i < SKY_TOUCH_CONTROL_COUNT; ++i) {
        const CGFloat opacity = i == SKY_TOUCH_BACK ? _backOpacity : _gameOpacity;
        if (opacity <= 0.004) continue;
        if (i > SKY_TOUCH_LOOK) [self drawButton:i opacity:opacity context:context];
        else if (sky_touch_control_active(&_touch, i)) [self drawStick:i opacity:opacity context:context];
    }
}
@end
