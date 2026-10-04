// LiquidAPK (apk-on-iphone): Android apps on the iPhone, in an AArch64 interpreter (no JIT, no
// debugger). A launcher of installed APKs (each with its own /data, snapshot and
// home-screen link liquidapk://open?app=<package>; aoi:// too), the app's screen fitted to the safe
// area, a back gesture from the left edge, and the developer tests. Built without an
// Xcode project by .github/workflows/ios.yml.
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <AVFoundation/AVFoundation.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "../core/apk.h"
#include "gmptest.h"
#include "vmprobe.h"
#include "androidtest.h"

#define APP_NAME @"LiquidAPK"

static void log_cb(void *ctx, const char *line);
static void frame_cb(void *ctx, const unsigned char *px, unsigned w, unsigned h);
static void home_cb(void *ctx);

/* ---------- looks ---------- */

/* iOS 26's Liquid Glass when this build has the iOS 26 SDK (built with an older one, iOS
 * draws a glass effect as an opaque white fallback), else a thin blur material. */
static UIVisualEffect *glass_effect(BOOL interactive) {
#if defined(__IPHONE_26_0) && __IPHONE_OS_VERSION_MAX_ALLOWED >= __IPHONE_26_0
    if (@available(iOS 26.0, *)) {
        UIGlassEffect *e = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
        e.interactive = interactive;
        return e;
    }
#endif
    (void)interactive;
    return [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark];
}

static UIVisualEffectView *glass_view(CGFloat radius, BOOL interactive) {
    UIVisualEffectView *v = [[UIVisualEffectView alloc] initWithEffect:glass_effect(interactive)];
    v.layer.cornerRadius = radius;
    v.layer.cornerCurve = kCACornerCurveContinuous;
    v.clipsToBounds = YES;
    return v;
}

/* A round icon for an app that has none of ours: its initial on a colour from its name. */
static UIImage *app_avatar(NSString *label, NSString *key, CGFloat size) {
    CGFloat hue = (CGFloat)(key.hash % 360) / 360.0;
    UIColor *a = [UIColor colorWithHue:hue saturation:0.55 brightness:0.95 alpha:1];
    UIColor *b = [UIColor colorWithHue:fmod(hue + 0.12, 1.0) saturation:0.75 brightness:0.75 alpha:1];
    UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(size, size)];
    return [r imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
        CGContextRef c = ctx.CGContext;
        UIBezierPath *clip = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, 0, size, size) cornerRadius:size * 0.225];
        [clip addClip];
        NSArray *colors = @[ (__bridge id)a.CGColor, (__bridge id)b.CGColor ];
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGGradientRef grad = CGGradientCreateWithColors(cs, (__bridge CFArrayRef)colors, NULL);
        CGContextDrawLinearGradient(c, grad, CGPointZero, CGPointMake(size, size), 0);
        CGGradientRelease(grad); CGColorSpaceRelease(cs);
        NSString *t = label.length ? [[label substringToIndex:1] uppercaseString] : @"?";
        NSDictionary *at = @{ NSFontAttributeName : [UIFont systemFontOfSize:size * 0.5 weight:UIFontWeightBold],
                              NSForegroundColorAttributeName : UIColor.whiteColor };
        CGSize ts = [t sizeWithAttributes:at];
        [t drawAtPoint:CGPointMake((size - ts.width) / 2, (size - ts.height) / 2) withAttributes:at];
    }];
}

/* ---------- installed apps ---------- */

@interface AoiApp : NSObject
@property(nonatomic, copy) NSString *pkg, *label;
- (NSString *)dir;                                   /* its /data */
- (NSString *)compileLog;                            /* dex2oat's output */
- (NSString *)compileState;
- (BOOL)compiled;
@end

@implementation AoiApp
+ (NSString *)appsDir {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    return [docs stringByAppendingPathComponent:@"apps"];
}
- (NSString *)dir { return [[AoiApp appsDir] stringByAppendingPathComponent:self.pkg]; }
- (NSString *)infoPath { return [self.dir stringByAppendingPathExtension:@"plist"]; }
- (NSString *)apkPath { return [self.dir stringByAppendingPathComponent:@"app/apk/base.apk"]; }
- (NSURL *)link { return [NSURL URLWithString:[NSString stringWithFormat:@"liquidapk://open?app=%@", self.pkg]]; }

/* Every installed app: a directory under Documents/apps with an APK in it. */
+ (NSArray<AoiApp *> *)all {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSMutableArray *out = [NSMutableArray array];
    [self migrate];
    for (NSString *name in [[fm contentsOfDirectoryAtPath:[self appsDir] error:nil] sortedArrayUsingSelector:@selector(compare:)]) {
        AoiApp *a = [AoiApp new];
        a.pkg = name;
        BOOL dir = NO;
        if (![fm fileExistsAtPath:a.dir isDirectory:&dir] || !dir || ![fm fileExistsAtPath:a.apkPath]) continue;
        NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:a.infoPath];
        a.label = info[@"label"] ?: name;
        [out addObject:a];
    }
    return out;
}

+ (AoiApp *)withPackage:(NSString *)pkg {
    for (AoiApp *a in [self all]) if ([a.pkg isEqualToString:pkg]) return a;
    return nil;
}

/* 0.17 and before kept the one app in Documents/adata: it becomes apps/<package>. */
+ (void)migrate {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *old = [docs stringByAppendingPathComponent:@"adata"];
    NSData *apk = [NSData dataWithContentsOfFile:[old stringByAppendingPathComponent:@"app/apk/base.apk"]
                                         options:NSDataReadingMappedIfSafe error:nil];
    char pkg[256], label[256];
    if (!apk || aoi_apk_manifest(apk.bytes, apk.length, pkg, sizeof pkg, label, sizeof label)) return;
    AoiApp *a = [AoiApp new];
    a.pkg = @(pkg); a.label = @(label);
    [fm createDirectoryAtPath:[self appsDir] withIntermediateDirectories:YES attributes:nil error:nil];
    if ([fm fileExistsAtPath:a.dir]) return;
    if (![fm moveItemAtPath:old toPath:a.dir error:nil]) return;
    for (NSString *ext in @[ @".snap", @".snap.key" ])
        [fm moveItemAtPath:[old stringByAppendingString:ext] toPath:[a.dir stringByAppendingString:ext] error:nil];
    [@{ @"label" : a.label } writeToFile:a.infoPath atomically:YES];
}

/* Installs an APK (or updates it): its /data from the bundle's, the APK in it. */
+ (AoiApp *)install:(NSData *)apk error:(NSString **)err {
    char pkg[256], label[256];
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *bundled = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"aroot/data"];
    NSError *e = nil;
    if (aoi_apk_manifest(apk.bytes, apk.length, pkg, sizeof pkg, label, sizeof label)) { *err = @"Bu bir Android APK'sı değil."; return nil; }
    AoiApp *a = [AoiApp new];
    a.pkg = @(pkg); a.label = @(label);
    [fm createDirectoryAtPath:[self appsDir] withIntermediateDirectories:YES attributes:nil error:nil];
    if (![fm fileExistsAtPath:a.dir] && ![fm copyItemAtPath:bundled toPath:a.dir error:&e]) {
        *err = [NSString stringWithFormat:@"/data hazırlanamadı: %@", e.localizedDescription];
        return nil;
    }
    [fm createDirectoryAtPath:a.apkPath.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    if (![[NSData dataWithContentsOfFile:a.apkPath] isEqualToData:apk]) [apk writeToFile:a.apkPath atomically:NO];
    [@{ @"label" : a.label } writeToFile:a.infoPath atomically:YES];
    return a;
}

/* What each launch refreshes from this build: aoi.dex, the classpath, ART's cache dir. */
+ (void)prepareData:(NSString *)dir {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *bundled = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"aroot/data"];
    for (NSString *f in @[ @"local/tmp/aoi.dex", @"system/environ/classpath" ]) {
        NSString *dst = [dir stringByAppendingPathComponent:f];
        [fm createDirectoryAtPath:dst.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
        [fm removeItemAtPath:dst error:nil];
        [fm copyItemAtPath:[bundled stringByAppendingPathComponent:f] toPath:dst error:nil];
    }
    [fm createDirectoryAtPath:[dir stringByAppendingPathComponent:@"dalvik-cache/arm64"]
  withIntermediateDirectories:YES attributes:nil error:nil];
}
- (void)prepare { [AoiApp prepareData:self.dir]; }

/* The warm process's /data (aoi_android_warm): the bundle's, no APK. */
+ (NSString *)warmDir {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *dir = [docs stringByAppendingPathComponent:@"warm"];
    NSString *bundled = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"aroot/data"];
    if (![fm fileExistsAtPath:dir] && ![fm copyItemAtPath:bundled toPath:dir error:nil]) return nil;
    [self prepareData:dir];
    return dir;
}

- (void)remove {
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *p in @[ self.dir, self.infoPath, [self.dir stringByAppendingString:@".snap"],
                           [self.dir stringByAppendingString:@".snap.key"], [self.dir stringByAppendingString:@".snap0"],
                           [self.dir stringByAppendingString:@".snap0.key"], [self.dir stringByAppendingString:@".nosnap"],
                           [self.dir stringByAppendingString:@".log"],
                           [self.dir stringByAppendingString:@".log.1"], self.compileLog ])
        [fm removeItemAtPath:p error:nil];
}

- (NSString *)compileLog { return [self.dir stringByAppendingString:@".dex2oat.log"]; }

/* What dex2oat left for this APK: "speed", "verify", "speed-profile", "failed", or "". */
- (NSString *)compileState {
    char st[32];
    aoi_android_compile_state(self.dir.UTF8String, st, sizeof st);
    return @(st);
}
- (BOOL)compiled {
    NSString *st = self.compileState;
    return st.length && ![st isEqualToString:@"failed"];
}

- (void)forgetSnapshot {
    [NSFileManager.defaultManager removeItemAtPath:[self.dir stringByAppendingString:@".snap"] error:nil];
}
@end

/* ---------- the back gesture: a glass drop pulled from the left edge ---------- */

@interface BackBubble : UIView
@property(nonatomic, strong) UIVisualEffectView *glass;
@property(nonatomic, strong) UIImageView *chevron;
@property(nonatomic) BOOL armed;
@end

@implementation BackBubble
- (instancetype)init {
    if ((self = [super initWithFrame:CGRectZero])) {
        self.userInteractionEnabled = NO;
        self.glass = glass_view(32, NO);
        [self addSubview:self.glass];
        UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:22 weight:UIImageSymbolWeightBold];
        self.chevron = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"chevron.left" withConfiguration:cfg]];
        self.chevron.tintColor = UIColor.whiteColor;
        self.chevron.contentMode = UIViewContentModeCenter;
        [self.glass.contentView addSubview:self.chevron];
        self.layer.shadowColor = UIColor.blackColor.CGColor;
        self.layer.shadowOpacity = 0.25;
        self.layer.shadowRadius = 12;
    }
    return self;
}

/* t: how far the finger has come from the edge. The drop grows and thins like jelly,
 * then keeps stretching a little past the point where letting go means "back". */
- (void)pull:(CGFloat)t atY:(CGFloat)y {
    const CGFloat full = 96;
    CGFloat p = MIN(MAX(t, 0) / full, 1), over = MAX(t - full, 0);
    CGFloat w = 18 + 46 * p + over * 0.12, h = 72 - 14 * p + over * 0.04;
    self.frame = CGRectMake(-24 * (1 - p), y - h / 2, w + 8, h);
    self.glass.frame = self.bounds;
    self.glass.layer.cornerRadius = MIN(h, w + 8) / 2;
    self.chevron.frame = CGRectMake(self.bounds.size.width - 44, 0, 36, h);
    self.chevron.alpha = p * p;
    self.chevron.transform = CGAffineTransformMakeScale(0.6 + 0.4 * p, 0.6 + 0.4 * p);
    BOOL armed = p >= 1;
    if (armed && !self.armed) {
        [[[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleMedium] impactOccurred];
        [UIView animateWithDuration:0.35 delay:0 usingSpringWithDamping:0.45 initialSpringVelocity:0.8 options:0 animations:^{
            self.glass.transform = CGAffineTransformMakeScale(1.08, 1.08);
        } completion:^(BOOL f) { [UIView animateWithDuration:0.2 animations:^{ self.glass.transform = CGAffineTransformIdentity; }]; }];
    }
    self.armed = armed;
}

- (void)letGo:(BOOL)back {
    [UIView animateWithDuration:back ? 0.3 : 0.45 delay:0 usingSpringWithDamping:back ? 0.9 : 0.55 initialSpringVelocity:0.5
                        options:UIViewAnimationOptionBeginFromCurrentState animations:^{
        if (back) { self.alpha = 0; self.transform = CGAffineTransformMakeScale(0.4, 0.4); }
        else { CGRect f = self.frame; f.origin.x = -f.size.width; self.frame = f; }
    } completion:^(BOOL f) { [self removeFromSuperview]; }];
}
@end

/* ---------- the app's screen ---------- */

@interface AoiScreen : UIImageView <UIKeyInput>
@property(nonatomic) UITextAutocorrectionType autocorrectionType;
@property(nonatomic) UITextAutocapitalizationType autocapitalizationType;
@property(nonatomic) UITextSpellCheckingType spellCheckingType;
@property(nonatomic) UITextSmartQuotesType smartQuotesType;
@property(nonatomic) UITextSmartDashesType smartDashesType;
@property(nonatomic, strong) UIView *keyBar;
@end

static __weak AoiScreen *current_screen;                /* the one showing an app: the keyboard's */

/* One-finger touches go to the app in its pixels (aoi_android_touch). The iPhone's
 * keyboard types into the app's text field (aoi.InputMethodManager): the screen is a
 * UIKeyInput, first responder while the app wants the keyboard; each character, a
 * backspace or return goes to aoi_android_key, and "Kapat" closes the keyboard. The
 * app's text field keeps the text (its own editing: autocorrection is off here). */
@implementation AoiScreen
- (BOOL)canBecomeFirstResponder { return YES; }
- (BOOL)hasText { return YES; }                         /* backspace always reaches the app */
- (void)insertText:(NSString *)text {
    NSData *d = [text dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    const uint32_t *c = d.bytes;
    for (NSUInteger i = 0; i < d.length / 4; i++) aoi_android_key(c[i] == '\n' ? 8 : 6, (int)c[i]);
}
- (void)deleteBackward { aoi_android_key(7, 0); }
- (UIView *)inputAccessoryView {
    if (!self.keyBar) {
        UIToolbar *bar = [[UIToolbar alloc] initWithFrame:CGRectMake(0, 0, 320, 44)];
        UIBarButtonItem *close = [[UIBarButtonItem alloc] initWithTitle:@"Kapat" style:UIBarButtonItemStyleDone
                                                                 target:self action:@selector(closeKeyboard)];
        bar.items = @[ [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace
                                                                     target:nil action:nil], close ];
        self.keyBar = bar;
    }
    return self.keyBar;
}
- (void)closeKeyboard {
    [self resignFirstResponder];
    aoi_android_key(9, 0);
}
- (BOOL)send:(int)action touches:(NSSet<UITouch *> *)touches {
    UITouch *t = touches.anyObject;
    CGSize img = self.image.size, v = self.bounds.size;
    if (!t || img.width <= 0) return NO;
    CGFloat k = MIN(v.width / img.width, v.height / img.height);
    CGFloat ox = (v.width - img.width * k) / 2, oy = (v.height - img.height * k) / 2;
    CGPoint pt = [t locationInView:self];
    aoi_android_touch(action, (float)((pt.x - ox) / k * self.image.scale), (float)((pt.y - oy) / k * self.image.scale));
    return YES;
}
- (void)touchesBegan:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e { if (e.allTouches.count == 1) [self send:0 touches:t]; }
- (void)touchesMoved:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e { if (e.allTouches.count == 1) [self send:2 touches:t]; }
- (void)touchesEnded:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e { [self send:1 touches:t]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e { [self send:4 touches:t]; }
@end

@interface ScreenVC : UIViewController
@property(nonatomic, strong) AoiApp *app;
@property(nonatomic, strong) AoiScreen *screen;
@property(nonatomic, strong) UIVisualEffectView *loading;
@property(nonatomic, strong) UILabel *loadingText;
@property(nonatomic, strong) BackBubble *bubble;
@property(nonatomic, copy) NSString *pendingStatus;
@end

@implementation ScreenVC
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.screen = [AoiScreen new];
    self.screen.autocorrectionType = UITextAutocorrectionTypeNo;
    self.screen.autocapitalizationType = UITextAutocapitalizationTypeNone;
    self.screen.spellCheckingType = UITextSpellCheckingTypeNo;
    self.screen.smartQuotesType = UITextSmartQuotesTypeNo;
    self.screen.smartDashesType = UITextSmartDashesTypeNo;
    current_screen = self.screen;
    self.screen.contentMode = UIViewContentModeScaleAspectFit;
    self.screen.userInteractionEnabled = YES;
    self.screen.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:self.screen];
    UILayoutGuide *g = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [self.screen.leadingAnchor constraintEqualToAnchor:g.leadingAnchor],
        [self.screen.trailingAnchor constraintEqualToAnchor:g.trailingAnchor],
        [self.screen.topAnchor constraintEqualToAnchor:g.topAnchor],
        [self.screen.bottomAnchor constraintEqualToAnchor:g.bottomAnchor],
    ]];
    UIScreenEdgePanGestureRecognizer *e = [[UIScreenEdgePanGestureRecognizer alloc] initWithTarget:self action:@selector(edge:)];
    e.edges = UIRectEdgeLeft;
    [self.view addGestureRecognizer:e];
    UITapGestureRecognizer *two = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(close)];
    two.numberOfTouchesRequired = 2;                    /* two fingers: back to the launcher */
    two.cancelsTouchesInView = NO;
    [self.screen addGestureRecognizer:two];

    self.loading = glass_view(28, NO);
    self.loading.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:self.loading];
    UIImageView *icon = [[UIImageView alloc] initWithImage:app_avatar(self.app.label, self.app.pkg, 72)];
    UIActivityIndicatorView *spin = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    spin.color = UIColor.whiteColor;
    [spin startAnimating];
    UILabel *name = [UILabel new];
    name.text = self.app.label;
    name.font = [UIFont systemFontOfSize:22 weight:UIFontWeightSemibold];
    name.textColor = UIColor.whiteColor;
    self.loadingText = [UILabel new];
    self.loadingText.text = self.pendingStatus ?: @"Açılıyor…";
    self.loadingText.font = [UIFont systemFontOfSize:14];
    self.loadingText.textColor = [UIColor colorWithWhite:1 alpha:0.75];
    self.loadingText.numberOfLines = 0;
    self.loadingText.textAlignment = NSTextAlignmentCenter;
    UIStackView *st = [[UIStackView alloc] initWithArrangedSubviews:@[ icon, name, spin, self.loadingText ]];
    st.axis = UILayoutConstraintAxisVertical;
    st.alignment = UIStackViewAlignmentCenter;
    st.spacing = 12;
    st.translatesAutoresizingMaskIntoConstraints = NO;
    [self.loading.contentView addSubview:st];
    [NSLayoutConstraint activateConstraints:@[
        [self.loading.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [self.loading.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [self.loading.widthAnchor constraintEqualToConstant:280],
        [st.topAnchor constraintEqualToAnchor:self.loading.contentView.topAnchor constant:28],
        [st.bottomAnchor constraintEqualToAnchor:self.loading.contentView.bottomAnchor constant:-28],
        [st.leadingAnchor constraintEqualToAnchor:self.loading.contentView.leadingAnchor constant:20],
        [st.trailingAnchor constraintEqualToAnchor:self.loading.contentView.trailingAnchor constant:-20],
        [icon.widthAnchor constraintEqualToConstant:72], [icon.heightAnchor constraintEqualToConstant:72],
    ]];
}

- (UIStatusBarStyle)preferredStatusBarStyle { return UIStatusBarStyleLightContent; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }

- (void)showFrame:(UIImage *)img {
    self.screen.image = img;
    if (self.loading.alpha > 0)
        [UIView animateWithDuration:0.3 animations:^{ self.loading.alpha = 0; }];
}

- (void)status:(NSString *)text {
    self.pendingStatus = text;
    self.loadingText.text = text;
}

- (void)edge:(UIScreenEdgePanGestureRecognizer *)g {
    CGPoint at = [g locationInView:self.view];
    CGFloat t = [g translationInView:self.view].x;
    switch (g.state) {
    case UIGestureRecognizerStateBegan:
        [self.bubble removeFromSuperview];
        self.bubble = [BackBubble new];
        [self.view addSubview:self.bubble];
        [self.bubble pull:t atY:at.y];
        break;
    case UIGestureRecognizerStateChanged:
        [self.bubble pull:t atY:at.y];
        break;
    case UIGestureRecognizerStateEnded: {
        BOOL back = self.bubble.armed || [g velocityInView:self.view].x > 900;
        if (back) aoi_android_back();
        [self.bubble letGo:back];
        self.bubble = nil;
        break;
    }
    default:
        [self.bubble letGo:NO];
        self.bubble = nil;
        break;
    }
}

- (void)close { [self.presentingViewController dismissViewControllerAnimated:YES completion:nil]; }
@end

/* ---------- the developer page: tests and logs ---------- */

@interface DevVC : UIViewController
@property(nonatomic, strong) UITextView *logView;
@property(nonatomic, strong) UITextField *nField;
@end

/* ---------- the launcher ---------- */

@interface Launcher : UIViewController <UIDocumentPickerDelegate, UIContextMenuInteractionDelegate>
@property(nonatomic, strong) UIStackView *list;
@property(nonatomic, strong) UILabel *empty;
@property(nonatomic, strong) NSMutableString *log;
@property(nonatomic) dispatch_queue_t work, appQueue;
@property(nonatomic, strong) ScreenVC *screenVC;
@property(nonatomic, copy) NSString *runningPkg;      /* the app whose process is alive (on appQueue) */
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, AoiApp *> *tiles;   /* tile tag -> app */
@property(nonatomic, weak) DevVC *dev;
@property(nonatomic, strong) NSMutableDictionary<NSString *, UILabel *> *subs;          /* package -> its card's line */
@property(nonatomic, strong) NSMutableDictionary<NSString *, UIProgressView *> *bars;
@property(nonatomic, strong) NSMutableDictionary<NSString *, UIButton *> *acts;
@property(nonatomic, strong) NSMutableArray<NSDictionary *> *queue;     /* apps waiting for dex2oat: {pkg, faster} */
@property(nonatomic, strong) NSTimer *ticker;
@property(nonatomic) BOOL warmRunning;                /* Android up without an app (aoi_android_warm), on appQueue */
@property(nonatomic, copy) NSString *warmDisplay;
@property(nonatomic, strong) AoiApp *warmApp;         /* the app it became (aoi_android_go) */
@property(nonatomic, strong) ScreenVC *warmTarget;
@property(nonatomic) void *warmCtx;
@property(nonatomic, strong) AoiApp *preparing;       /* started out of sight after its compile, to be saved */
@property(nonatomic) BOOL preparingShown, preparingCancelled;
@property(nonatomic, strong) ScreenVC *preparingTarget;
@property(nonatomic) void *preparingCtx;
@property(nonatomic, strong) NSMutableArray<NSString *> *prepareQueue;
@property(nonatomic) BOOL inBackground;               /* LiquidAPK is not on screen */
- (void)openPackage:(NSString *)pkg;
- (void)append:(NSString *)line;
- (void)compileEnded:(NSString *)dir state:(NSString *)state;
- (void)tick;
- (NSString *)root;
- (NSString *)display;
- (void)startWarm;
- (void)prepareApp:(AoiApp *)a;
- (void)idleNext;
- (void)appEnded:(AoiApp *)a target:(ScreenVC *)target log:(NSString *)logPath;
- (void)maybeOpen:(AoiApp *)a;
- (void)openApp:(AoiApp *)a;
- (void)compile:(AoiApp *)a faster:(BOOL)faster;
- (void)cancelCompile:(AoiApp *)a;
- (void)tell:(NSString *)title what:(NSString *)msg;
- (BOOL)queued:(AoiApp *)a;
- (void)resumeCompiles;
@end

static Launcher *launcher;

/* dex2oat waits while the phone is very hot; or hot (or in Low Power Mode) while an app
 * is in use next to it. Alone (nothing else on screen, or LiquidAPK in the background)
 * the compile is the one job and goes on. */
static void compile_hold_update(void)
{
    NSProcessInfo *pi = NSProcessInfo.processInfo;
    BOOL in_use = launcher.runningPkg && !launcher.inBackground;
    aoi_android_compile_hold(pi.thermalState >= NSProcessInfoThermalStateCritical
                             || (in_use && (pi.thermalState >= NSProcessInfoThermalStateSerious || pi.lowPowerModeEnabled)));
}

/* A compile started is noted (<dir>.compiling: "faster tries") until it ends, so one
 * that iOS cut short (LiquidAPK ended in the background) starts again at the next
 * launch: twice at most, in case it was what ended it. */
static NSString *compile_mark(AoiApp *a) { return [a.dir stringByAppendingString:@".compiling"]; }

@implementation Launcher

- (void)viewDidLoad {
    [super viewDidLoad];
    launcher = self;
    self.log = [NSMutableString string];
    self.tiles = [NSMutableDictionary dictionary];
    self.subs = [NSMutableDictionary dictionary];
    self.bars = [NSMutableDictionary dictionary];
    self.acts = [NSMutableDictionary dictionary];
    self.queue = [NSMutableArray array];
    self.prepareQueue = [NSMutableArray array];
    self.work = dispatch_queue_create("aoi.work", DISPATCH_QUEUE_SERIAL);
    self.appQueue = dispatch_queue_create("aoi.app", DISPATCH_QUEUE_SERIAL);
    self.view.backgroundColor = UIColor.blackColor;

    CAGradientLayer *bg = [CAGradientLayer layer];      /* the logo's colours, deep, under the glass */
    bg.colors = @[ (__bridge id)[UIColor colorWithRed:0.05 green:0.35 blue:0.25 alpha:1].CGColor,
                   (__bridge id)[UIColor colorWithRed:0.02 green:0.18 blue:0.45 alpha:1].CGColor,
                   (__bridge id)[UIColor colorWithRed:0.05 green:0.05 blue:0.12 alpha:1].CGColor ];
    bg.startPoint = CGPointMake(0, 0); bg.endPoint = CGPointMake(1, 1);
    bg.frame = UIScreen.mainScreen.bounds;
    [self.view.layer addSublayer:bg];

    UIImageView *logo = [[UIImageView alloc] initWithImage:[UIImage imageNamed:@"AppIcon60x60"]];
    logo.layer.cornerRadius = 14; logo.layer.cornerCurve = kCACornerCurveContinuous; logo.clipsToBounds = YES;
    [logo.widthAnchor constraintEqualToConstant:56].active = YES;
    [logo.heightAnchor constraintEqualToConstant:56].active = YES;
    UILabel *title = [UILabel new];
    title.text = APP_NAME;
    title.font = [UIFont systemFontOfSize:28 weight:UIFontWeightBold];
    title.textColor = UIColor.whiteColor;
    UILabel *sub = [UILabel new];
    sub.text = @"Android uygulamaları";
    sub.font = [UIFont systemFontOfSize:15];
    sub.textColor = [UIColor colorWithWhite:1 alpha:0.7];
    UIStackView *titles = [[UIStackView alloc] initWithArrangedSubviews:@[ title, sub ]];
    titles.axis = UILayoutConstraintAxisVertical;
    UIStackView *head = [[UIStackView alloc] initWithArrangedSubviews:@[ logo, titles ]];
    head.spacing = 14; head.alignment = UIStackViewAlignmentCenter;

    self.list = [UIStackView new];
    self.list.axis = UILayoutConstraintAxisVertical;
    self.list.spacing = 14;
    self.empty = [UILabel new];
    self.empty.text = @"Henüz uygulama yok.\nAşağıdaki “APK ekle” ile bir Android uygulaması seçin.";
    self.empty.numberOfLines = 0;
    self.empty.textAlignment = NSTextAlignmentCenter;
    self.empty.textColor = [UIColor colorWithWhite:1 alpha:0.7];

    UIScrollView *scroll = [UIScrollView new];
    scroll.alwaysBounceVertical = YES;
    UIStackView *content = [[UIStackView alloc] initWithArrangedSubviews:@[ head, self.list, self.empty ]];
    content.axis = UILayoutConstraintAxisVertical;
    content.spacing = 26;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:content];
    [self.view addSubview:scroll];

    UIView *add = [self pill:@"APK ekle" symbol:@"plus" action:@selector(pick)];
    UIView *dev = [self pill:@"Geliştirici" symbol:@"wrench.and.screwdriver" action:@selector(openDev)];
    UIStackView *bar = [[UIStackView alloc] initWithArrangedSubviews:@[ add, dev ]];
    bar.spacing = 12; bar.distribution = UIStackViewDistributionFillEqually;
    bar.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:bar];

    UILayoutGuide *g = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:g.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:bar.topAnchor constant:-8],
        [content.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:20],
        [content.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-20],
        [content.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:20],
        [content.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-20],
        [content.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-40],
        [bar.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:20],
        [bar.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-20],
        [bar.bottomAnchor constraintEqualToAnchor:g.bottomAnchor constant:-8],
        [bar.heightAnchor constraintEqualToConstant:54],
    ]];

    char machine[64] = "?";
    size_t len = sizeof machine;
    sysctlbyname("hw.machine", machine, &len, NULL, 0);
    [self append:[NSString stringWithFormat:@"%@ %@, cihaz %s, iOS %@", APP_NAME,
                  [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"], machine,
                  UIDevice.currentDevice.systemVersion]];
    [self reload];
}

- (UIStatusBarStyle)preferredStatusBarStyle { return UIStatusBarStyleLightContent; }

- (void)viewDidAppear:(BOOL)animated {
    static BOOL once;
    [super viewDidAppear:animated];
    if (!once) {                                         /* compiled apps without a fitting snapshot: saved now */
        once = YES;
        [self resumeCompiles];
        for (AoiApp *a in [AoiApp all])
            if (a.compiled && !aoi_android_snapshot_fits(a.dir.UTF8String)
                && ![NSFileManager.defaultManager fileExistsAtPath:[a.dir stringByAppendingString:@".nosnap"]])
                [self.prepareQueue addObject:a.pkg];
    }
    [self idleNext];
}

/* The app's display: the safe area, in points at 2x (aoi.DisplayManager). */
- (NSString *)display {
    UIEdgeInsets in = self.view.window.safeAreaInsets;
    CGSize s = self.view.window.bounds.size;
    long w = lround((s.width - in.left - in.right) * 2), h = lround((s.height - in.top - in.bottom) * 2);
    return [NSString stringWithFormat:@"%ld %ld 320", w & ~1L, h & ~1L];
}

/* Android, started before an app is chosen, while none runs: a tap then only loads the
 * app (7 s less). Its first start is saved, later ones resume in about a second. */
- (void)startWarm {
    if (self.warmRunning || self.runningPkg || self.preparing || !self.view.window || self.inBackground) return;
    NSString *dir = [AoiApp warmDir], *display = self.display;
    if (!dir || ![NSFileManager.defaultManager fileExistsAtPath:[self.root stringByAppendingPathComponent:@"system/bin/app_process64"]])
        return;
    self.warmRunning = YES;
    self.warmDisplay = display;
    self.warmApp = nil;
    NSString *root = self.root;
    dispatch_async(self.appQueue, ^{
        int rc = aoi_android_warm(root.UTF8String, dir.UTF8String, display.UTF8String, log_cb, (__bridge void *)self);
        dispatch_async(dispatch_get_main_queue(), ^{
            self.warmRunning = NO;
            if (rc != -2 && self.warmApp) {              /* it was an app: that app ended */
                AoiApp *a = self.warmApp;
                ScreenVC *target = self.warmTarget;
                CFRelease(self.warmCtx);
                self.warmApp = nil; self.warmTarget = nil; self.warmCtx = NULL;
                [self appEnded:a target:target log:[a.dir stringByAppendingString:@".log"]];
            }
        });
    });
}

/* An app's process ended (by itself, or for another app). */
- (void)appEnded:(AoiApp *)a target:(ScreenVC *)target log:(NSString *)logPath {
    if (self.screenVC == target) {                   /* it ended by itself, not for another app */
        self.runningPkg = nil;
        self.screenVC = nil;
        compile_hold_update();
        [self append:[NSString stringWithFormat:@"%@ kapandı; log: %@", a.label, logPath]];
        if (self.presentedViewController == target) [self dismissViewControllerAnimated:YES completion:nil];
    }
    [self idleNext];
}

/* Nothing shown any more: the next app to save, else the warm process. */
- (void)idleNext {
    [self reload];
    if (self.runningPkg || self.preparing) return;
    while (self.prepareQueue.count) {                    /* compiled while it ran: saved now */
        AoiApp *n = [AoiApp withPackage:self.prepareQueue.firstObject];
        [self.prepareQueue removeObjectAtIndex:0];
        if (n) { [self prepareApp:n]; return; }
    }
    [self startWarm];
}

/* After its compile an app's snapshot no longer fits: it is started out of sight and
 * saved (aoi_android_app_hidden), so the first tap resumes it in a second. Apps that
 * cannot be saved (GL games, WebView) are marked (.nosnap) and left alone. */
- (void)prepareApp:(AoiApp *)a {
    NSFileManager *fm = NSFileManager.defaultManager;
    if (aoi_android_snapshot_fits(a.dir.UTF8String) || [fm fileExistsAtPath:[a.dir stringByAppendingString:@".nosnap"]]
        || !a.compiled)
        return;
    if (self.runningPkg || self.preparing) {
        if (![self.prepareQueue containsObject:a.pkg]) [self.prepareQueue addObject:a.pkg];
        return;
    }
    if (self.warmRunning && !self.warmApp) aoi_android_warm_stop();
    self.preparing = a;
    self.preparingShown = NO; self.preparingCancelled = NO;
    [a prepare];
    [self append:[NSString stringWithFormat:@"%@: ilk açılış arka planda hazırlanıyor", a.label]];
    NSString *root = self.root, *display = self.display, *logPath = [a.dir stringByAppendingString:@".log"];
    dispatch_async(self.appQueue, ^{
        int rc = aoi_android_app_hidden(root.UTF8String, a.dir.UTF8String, logPath.UTF8String, display.UTF8String,
                                        log_cb, (__bridge void *)self);
        dispatch_async(dispatch_get_main_queue(), ^{
            BOOL shown = self.preparingShown, cancelled = self.preparingCancelled;
            ScreenVC *target = self.preparingTarget;
            if (self.preparingCtx) CFRelease(self.preparingCtx);
            self.preparing = nil; self.preparingTarget = nil; self.preparingCtx = NULL;
            self.preparingShown = NO; self.preparingCancelled = NO;
            if (shown) { [self appEnded:a target:target log:logPath]; return; }
            if (cancelled) {
                if (![self.prepareQueue containsObject:a.pkg]) [self.prepareQueue addObject:a.pkg];
            } else if (rc == 1) {
                [self append:[NSString stringWithFormat:@"%@ hazır: anında açılır", a.label]];
            } else {
                [@"" writeToFile:[a.dir stringByAppendingString:@".nosnap"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
            }
            [self idleNext];
        });
    });
    [self tick];
}

/* A glass capsule button. */
- (UIView *)pill:(NSString *)text symbol:(NSString *)sym action:(SEL)a {
    UIVisualEffectView *v = glass_view(27, YES);
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:[@" " stringByAppendingString:text] forState:UIControlStateNormal];
    [b setImage:[UIImage systemImageNamed:sym] forState:UIControlStateNormal];
    b.tintColor = UIColor.whiteColor;
    b.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    [b addTarget:self action:a forControlEvents:UIControlEventTouchUpInside];
    b.translatesAutoresizingMaskIntoConstraints = NO;
    [v.contentView addSubview:b];
    [NSLayoutConstraint activateConstraints:@[
        [b.leadingAnchor constraintEqualToAnchor:v.contentView.leadingAnchor constant:12],
        [b.trailingAnchor constraintEqualToAnchor:v.contentView.trailingAnchor constant:-12],
        [b.topAnchor constraintEqualToAnchor:v.contentView.topAnchor],
        [b.bottomAnchor constraintEqualToAnchor:v.contentView.bottomAnchor],
    ]];
    return v;
}

/* The app tiles: a glass card each, tap to open, hold for more. */
- (void)reload {
    for (UIView *v in self.list.arrangedSubviews) [v removeFromSuperview];
    [self.tiles removeAllObjects];
    NSArray<AoiApp *> *apps = [AoiApp all];
    self.empty.hidden = apps.count > 0;
    NSInteger tag = 1;
    NSMutableArray *items = [NSMutableArray array];
    for (AoiApp *a in apps) {
        UIVisualEffectView *card = glass_view(24, YES);
        card.tag = tag;
        self.tiles[@(tag)] = a;
        tag++;
        UIImageView *icon = [[UIImageView alloc] initWithImage:app_avatar(a.label, a.pkg, 60)];
        UILabel *name = [UILabel new];
        name.text = a.label;
        name.font = [UIFont systemFontOfSize:19 weight:UIFontWeightSemibold];
        name.textColor = UIColor.whiteColor;
        UILabel *pkg = [UILabel new];
        pkg.font = [UIFont systemFontOfSize:13];
        pkg.textColor = [UIColor colorWithWhite:1 alpha:0.65];
        pkg.numberOfLines = 2;
        UIProgressView *bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleBar];
        bar.progressTintColor = [UIColor colorWithRed:0.35 green:0.85 blue:0.6 alpha:1];
        bar.trackTintColor = [UIColor colorWithWhite:1 alpha:0.15];
        bar.hidden = YES;
        UIStackView *texts = [[UIStackView alloc] initWithArrangedSubviews:@[ name, pkg, bar ]];
        texts.axis = UILayoutConstraintAxisVertical;
        texts.spacing = 4;
        UIImageView *chev = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"chevron.right"]];
        chev.tintColor = [UIColor colorWithWhite:1 alpha:0.5];
        [chev setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        UIButton *act = [UIButton buttonWithType:UIButtonTypeSystem];      /* Derle / İptal */
        act.tintColor = UIColor.whiteColor;
        act.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
        act.tag = card.tag;
        act.hidden = YES;
        [act setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [act setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [act addTarget:self action:@selector(tapAction:) forControlEvents:UIControlEventTouchUpInside];
        self.subs[a.pkg] = pkg; self.bars[a.pkg] = bar; self.acts[a.pkg] = act;
        UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[ icon, texts, act, chev ]];
        row.spacing = 14; row.alignment = UIStackViewAlignmentCenter;
        row.translatesAutoresizingMaskIntoConstraints = NO;
        [card.contentView addSubview:row];
        [NSLayoutConstraint activateConstraints:@[
            [row.leadingAnchor constraintEqualToAnchor:card.contentView.leadingAnchor constant:14],
            [row.trailingAnchor constraintEqualToAnchor:card.contentView.trailingAnchor constant:-16],
            [row.topAnchor constraintEqualToAnchor:card.contentView.topAnchor constant:14],
            [row.bottomAnchor constraintEqualToAnchor:card.contentView.bottomAnchor constant:-14],
            [icon.widthAnchor constraintEqualToConstant:60], [icon.heightAnchor constraintEqualToConstant:60],
        ]];
        [card addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapTile:)]];
        [card addInteraction:[[UIContextMenuInteraction alloc] initWithDelegate:self]];
        [self.list addArrangedSubview:card];
        if (items.count < 4)
            [items addObject:[[UIApplicationShortcutItem alloc] initWithType:@"open" localizedTitle:a.label
                                                           localizedSubtitle:nil
                                                                        icon:[UIApplicationShortcutIcon iconWithSystemImageName:@"app.fill"]
                                                                    userInfo:@{ @"app" : a.pkg }]];
    }
    UIApplication.sharedApplication.shortcutItems = items;     /* hold the app icon: these apps */
    [self tick];
}

/* ---------- compiling (dex2oat, once per APK): the cards show it ---------- */

static NSString *duration_text(double s) {
    if (s == -2) return @"az";
    if (s < 0) return @"süre hesaplanıyor";
    if (s < 90) return [NSString stringWithFormat:@"~%.0f sn", fmax(5, round(s / 5) * 5)];
    if (s < 3600) return [NSString stringWithFormat:@"~%.0f dk", ceil(s / 60)];
    return [NSString stringWithFormat:@"~%.0f sa %.0f dk", floor(s / 3600), fmod(ceil(s / 60), 60)];
}

- (NSString *)root { return [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"aroot"]; }

- (BOOL)queued:(AoiApp *)a {
    for (NSDictionary *q in self.queue) if ([q[@"pkg"] isEqualToString:a.pkg]) return YES;
    return NO;
}

/* Every card's line, button and bar; once a second while dex2oat runs. */
- (void)tick {
    struct aoi_compile_info ci;
    aoi_android_compile_info(&ci);
    for (AoiApp *a in self.tiles.allValues) {
        UILabel *sub = self.subs[a.pkg];
        UIProgressView *bar = self.bars[a.pkg];
        UIButton *act = self.acts[a.pkg];
        BOOL mine = ci.active && !strcmp(ci.datadir, a.dir.UTF8String);
        NSString *st = a.compileState, *text, *btn = nil, *sym = nil;
        if (mine) {
            text = ci.held ? [NSString stringWithFormat:@"Derleme %%%.0f · duraklatıldı (telefon çok sıcak, ya da sıcak ve bir uygulama açık)", ci.progress * 100]
                           : [NSString stringWithFormat:@"Derleniyor %%%.0f · %@ kaldı", ci.progress * 100, duration_text(ci.eta)];
            btn = @"İptal"; sym = @"xmark.circle.fill";
        } else if ([self queued:a]) {
            text = @"Derleme sırada";
            btn = @"İptal"; sym = @"xmark.circle.fill";
        } else if ([self.preparing.pkg isEqualToString:a.pkg] && !self.preparingShown) {
            text = @"Derlendi ✓ · ilk açılış arka planda hazırlanıyor…";
        } else if ([self.prepareQueue containsObject:a.pkg]) {
            text = @"Derlendi ✓ · ilk açılışı hazırlanacak";
        } else if (a.compiled && aoi_android_snapshot_fits(a.dir.UTF8String)) {
            text = [st isEqualToString:@"verify"] ? @"Hazır ✓ · anında açılır (temel derleme)" : @"Hazır ✓ · anında açılır";
        } else if ([st isEqualToString:@"speed"] || [st isEqualToString:@"speed-profile"]) {
            text = @"Derlendi ✓";
        } else if ([st isEqualToString:@"verify"]) {
            text = @"Derlendi (temel) ✓ · basılı tutup “Hızlandır”";
        } else if ([st isEqualToString:@"failed"]) {
            text = @"Derlenemedi · derlenmeden çalışır";
            btn = @"Tekrar"; sym = @"arrow.clockwise";
        } else {
            text = [NSString stringWithFormat:@"Derlenmedi · derleme %@", duration_text(aoi_android_compile_estimate(a.dir.UTF8String, 0))];
            btn = @"Derle"; sym = @"hammer.fill";
        }
        if ([a.pkg isEqualToString:self.runningPkg]) text = [@"Çalışıyor · " stringByAppendingString:text];
        sub.text = text;
        bar.hidden = !mine;
        if (mine) bar.progress = (float)ci.progress;
        act.hidden = btn == nil;
        if (btn) {
            [act setTitle:[@" " stringByAppendingString:btn] forState:UIControlStateNormal];
            [act setImage:[UIImage systemImageNamed:sym] forState:UIControlStateNormal];
        }
    }
    if (ci.active && !self.ticker)
        self.ticker = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *t) { [self tick]; }];
    if (!ci.active && !self.queue.count) { [self.ticker invalidate]; self.ticker = nil; }
    UIApplication.sharedApplication.idleTimerDisabled = ci.active;   /* a locked phone suspends us, and dex2oat */
}

- (void)tapAction:(UIButton *)b {
    AoiApp *a = self.tiles[@(b.tag)];
    struct aoi_compile_info ci;
    if (!a) return;
    aoi_android_compile_info(&ci);
    if (ci.active && !strcmp(ci.datadir, a.dir.UTF8String)) [self cancelCompile:a];
    else if ([self queued:a]) [self cancelCompile:a];
    else [self compile:a faster:NO];
}

/* Starts it, or queues it behind the one that runs. */
- (void)compile:(AoiApp *)a faster:(BOOL)faster {
    if ([self queued:a]) return;
    int rc = aoi_android_compile(self.root.UTF8String, a.dir.UTF8String, a.compileLog.UTF8String, faster,
                                 log_cb, (__bridge void *)self);
    if (rc == 0 || rc == -1) {
        NSString *m = compile_mark(a), *was = [NSString stringWithContentsOfFile:m encoding:NSUTF8StringEncoding error:nil];
        int tries = was ? [[was componentsSeparatedByString:@" "].lastObject intValue] : 0;
        [[NSString stringWithFormat:@"%d %d", faster ? 1 : 0, tries] writeToFile:m atomically:YES encoding:NSUTF8StringEncoding error:nil];
    }
    if (rc == -1) [self.queue addObject:@{ @"pkg" : a.pkg, @"faster" : @(faster) }];
    else if (rc == 1 && faster) [self tell:@"Hızlandırılamıyor"
                                       what:@"Uygulamanın sık kullandığı kod henüz bilinmiyor: önce onu bir süre kullanın."];
    else if (rc == -2) [self tell:@"Şimdi derlenemiyor"
                             what:@"Açık uygulamanın yanında bellek yetmiyor. Uygulamayı kapatıp (iki parmakla dokunun) tekrar deneyin."];
    if (rc == 0) [self append:[NSString stringWithFormat:@"%@ derleniyor (tahmini %@).", a.label,
                               duration_text(aoi_android_compile_estimate(a.dir.UTF8String, faster))]];
    [self tick];
}

- (void)cancelCompile:(AoiApp *)a {
    struct aoi_compile_info ci;
    for (NSDictionary *q in [self.queue copy]) if ([q[@"pkg"] isEqualToString:a.pkg]) [self.queue removeObject:q];
    [NSFileManager.defaultManager removeItemAtPath:compile_mark(a) error:nil];
    aoi_android_compile_info(&ci);
    if (ci.active && !strcmp(ci.datadir, a.dir.UTF8String)) aoi_android_compile_cancel();
    [self tick];
}

/* dex2oat ended (compile_done_cb): the next in the queue. */
- (void)compileEnded:(NSString *)dir state:(NSString *)state {
    AoiApp *a = nil;
    for (AoiApp *x in [AoiApp all]) if ([x.dir isEqualToString:dir]) a = x;
    if (a) [NSFileManager.defaultManager removeItemAtPath:compile_mark(a) error:nil];
    if (a) [self append:[NSString stringWithFormat:@"%@: %@", a.label,
                         [state isEqualToString:@"failed"] ? @"derlenemedi" : state.length ? @"derlendi" : @"derleme durdu"]];
    if (a && state.length && ![state isEqualToString:@"failed"]) {
        [NSFileManager.defaultManager removeItemAtPath:[a.dir stringByAppendingString:@".nosnap"] error:nil];
        [self prepareApp:a];                             /* its first start, out of sight: then a tap resumes it */
    }
    while (self.queue.count) {
        NSDictionary *q = self.queue.firstObject;
        [self.queue removeObjectAtIndex:0];
        AoiApp *n = [AoiApp withPackage:q[@"pkg"]];
        if (n) { [self compile:n faster:[q[@"faster"] boolValue]]; break; }
    }
    [self tick];
}

/* Compiles cut short by the end of LiquidAPK (compile_mark): started again. */
- (void)resumeCompiles {
    NSFileManager *fm = NSFileManager.defaultManager;
    for (AoiApp *a in [AoiApp all]) {
        NSString *m = compile_mark(a), *was = [NSString stringWithContentsOfFile:m encoding:NSUTF8StringEncoding error:nil];
        if (!was) continue;
        NSArray *f = [was componentsSeparatedByString:@" "];
        BOOL faster = [f.firstObject intValue] != 0;
        int tries = [f.lastObject intValue];
        if ((a.compiled && !faster) || tries >= 2) { [fm removeItemAtPath:m error:nil]; continue; }
        [[NSString stringWithFormat:@"%d %d", faster ? 1 : 0, tries + 1] writeToFile:m atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [self append:[NSString stringWithFormat:@"%@: yarım kalan derleme yeniden başlıyor.", a.label]];
        [self compile:a faster:faster];
    }
}

- (void)tell:(NSString *)title what:(NSString *)msg {
    UIAlertController *al = [UIAlertController alertControllerWithTitle:title message:msg preferredStyle:UIAlertControllerStyleAlert];
    [al addAction:[UIAlertAction actionWithTitle:@"Tamam" style:UIAlertActionStyleCancel handler:nil]];
    [(self.presentedViewController ?: self) presentViewController:al animated:YES completion:nil];
}

/* A tap on an app that is not compiled: compile it first, or open it as it is. */
- (void)maybeOpen:(AoiApp *)a {
    struct aoi_compile_info ci;
    aoi_android_compile_info(&ci);
    BOOL mine = ci.active && !strcmp(ci.datadir, a.dir.UTF8String);
    BOOL snap = [NSFileManager.defaultManager fileExistsAtPath:[a.dir stringByAppendingString:@".snap"]];
    if ([a.pkg isEqualToString:self.runningPkg] || snap || (a.compiled && !mine) || [a.compileState isEqualToString:@"failed"]) {
        [self openApp:a];
        return;
    }
    NSString *msg = mine
        ? [NSString stringWithFormat:@"Derleniyor: %%%.0f, %@ kaldı. Bitince hızlı açılır; şimdi açarsanız derlenmeden (yavaş) çalışır.",
                                     ci.progress * 100, duration_text(ci.eta)]
        : [NSString stringWithFormat:@"Derlenmeden de açılır ama çok daha yavaş çalışır. Derleme bir kez yapılır, %@ sürer; "
                                     @"LiquidAPK'yı kapatmayın (arka plana almak olur).", duration_text(aoi_android_compile_estimate(a.dir.UTF8String, 0))];
    UIAlertController *al = [UIAlertController alertControllerWithTitle:mine ? [NSString stringWithFormat:@"%@ derleniyor", a.label]
                                                                             : [NSString stringWithFormat:@"%@ henüz derlenmedi", a.label]
                                                                message:msg preferredStyle:UIAlertControllerStyleAlert];
    if (!mine)
        [al addAction:[UIAlertAction actionWithTitle:@"Derle" style:UIAlertActionStyleDefault
                                             handler:^(UIAlertAction *x) { [self compile:a faster:NO]; }]];
    [al addAction:[UIAlertAction actionWithTitle:@"Yine de aç" style:UIAlertActionStyleDefault
                                         handler:^(UIAlertAction *x) { [self openApp:a]; }]];
    [al addAction:[UIAlertAction actionWithTitle:mine ? @"Bekle" : @"Vazgeç" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:al animated:YES completion:nil];
}

- (void)tapTile:(UITapGestureRecognizer *)g {
    AoiApp *a = self.tiles[@(g.view.tag)];
    if (a) [self maybeOpen:a];
}

- (UIContextMenuConfiguration *)contextMenuInteraction:(UIContextMenuInteraction *)i
                        configurationForMenuAtLocation:(CGPoint)location {
    AoiApp *a = self.tiles[@(i.view.tag)];
    if (!a) return nil;
    return [UIContextMenuConfiguration configurationWithIdentifier:nil previewProvider:nil
                                                    actionProvider:^UIMenu *(NSArray<UIMenuElement *> *s) {
        UIAction *home = [UIAction actionWithTitle:@"Ana ekrana ekle" image:[UIImage systemImageNamed:@"plus.app"]
                                        identifier:nil handler:^(UIAction *x) { [self addToHome:a]; }];
        UIAction *fresh = [UIAction actionWithTitle:@"Baştan başlat" image:[UIImage systemImageNamed:@"arrow.clockwise"]
                                         identifier:nil handler:^(UIAction *x) { [self restart:a]; }];
        UIAction *del = [UIAction actionWithTitle:@"Kaldır" image:[UIImage systemImageNamed:@"trash"]
                                       identifier:nil handler:^(UIAction *x) { [self confirmRemove:a]; }];
        del.attributes = UIMenuElementAttributesDestructive;
        NSMutableArray *items = [NSMutableArray arrayWithObjects:home, fresh, nil];
        struct aoi_compile_info ci;
        aoi_android_compile_info(&ci);
        NSString *st = a.compileState;
        if ((ci.active && !strcmp(ci.datadir, a.dir.UTF8String)) || [self queued:a])
            [items addObject:[UIAction actionWithTitle:@"Derlemeyi iptal et" image:[UIImage systemImageNamed:@"xmark.circle"]
                                            identifier:nil handler:^(UIAction *x) { [self cancelCompile:a]; }]];
        else if (!a.compiled)
            [items addObject:[UIAction actionWithTitle:@"Derle" image:[UIImage systemImageNamed:@"hammer"]
                                            identifier:nil handler:^(UIAction *x) { [self compile:a faster:NO]; }]];
        else {
            if ([st isEqualToString:@"verify"])
                [items addObject:[UIAction actionWithTitle:@"Hızlandır (sık kullanılan kodu derle)" image:[UIImage systemImageNamed:@"bolt"]
                                                identifier:nil handler:^(UIAction *x) { [self compile:a faster:YES]; }]];
            UIAction *rm = [UIAction actionWithTitle:@"Derlemeyi sil" image:[UIImage systemImageNamed:@"hammer.circle"]
                                          identifier:nil handler:^(UIAction *x) {
                if ([a.pkg isEqualToString:self.runningPkg]) aoi_android_stop();
                dispatch_async(self.appQueue, ^{                 /* after its process has ended */
                    aoi_android_compile_remove(a.dir.UTF8String);
                    dispatch_async(dispatch_get_main_queue(), ^{ [self tick]; });
                });
            }];
            [items addObject:rm];
            [items addObject:[UIAction actionWithTitle:@"Yeniden derle" image:[UIImage systemImageNamed:@"hammer"]
                                            identifier:nil handler:^(UIAction *x) { [self recompile:a]; }]];
        }
        [items addObject:del];
        return [UIMenu menuWithTitle:a.label children:items];
    }];
}

/* A home-screen icon of its own: iOS lets only Shortcuts make one, so the link is copied
 * and the steps are shown (the icon can go to Photos, to pick it there). */
- (void)addToHome:(AoiApp *)a {
    UIPasteboard.generalPasteboard.URL = a.link;
    NSString *msg = [NSString stringWithFormat:
        @"Bağlantı kopyalandı:\n%@\n\n1. Kestirmeler'de + ile yeni kestirme oluşturun.\n"
        @"2. “URL'leri Aç” eylemini ekleyip bağlantıyı yapıştırın.\n"
        @"3. Paylaş › “Ana Ekrana Ekle”: adı “%@” yapın, simge için Fotoğraflar'daki görseli seçin.",
        a.link.absoluteString, a.label];
    UIAlertController *al = [UIAlertController alertControllerWithTitle:@"Ana ekrana ekle" message:msg
                                                         preferredStyle:UIAlertControllerStyleAlert];
    NSURL *shortcuts = [NSURL URLWithString:@"shortcuts://create-shortcut"];
    [al addAction:[UIAlertAction actionWithTitle:@"Simgeyi kaydet ve Kestirmeler'i aç" style:UIAlertActionStyleDefault
                                         handler:^(UIAlertAction *x) {
        UIImageWriteToSavedPhotosAlbum(app_avatar(a.label, a.pkg, 512), nil, NULL, NULL);
        [UIApplication.sharedApplication openURL:shortcuts options:@{} completionHandler:nil];
    }]];
    [al addAction:[UIAlertAction actionWithTitle:@"Kestirmeler'i aç" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
        [UIApplication.sharedApplication openURL:shortcuts options:@{} completionHandler:nil];
    }]];
    [al addAction:[UIAlertAction actionWithTitle:@"Tamam" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:al animated:YES completion:nil];
}

/* Its compiled code and snapshots go, then it is compiled again (and saved again). */
- (void)recompile:(AoiApp *)a {
    if ([a.pkg isEqualToString:self.runningPkg]) aoi_android_stop();
    if ([self.preparing.pkg isEqualToString:a.pkg]) { self.preparingCancelled = YES; aoi_android_stop(); }
    [self.prepareQueue removeObject:a.pkg];
    dispatch_async(self.appQueue, ^{                     /* after its process has ended */
        aoi_android_compile_remove(a.dir.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{ [self compile:a faster:NO]; });
    });
}

/* Its saved state goes: the next launch is the app as it first started (the clean
 * snapshot taken after its compile, <dir>.snap0: still in a second), else from nothing. */
- (void)restart:(AoiApp *)a {
    if ([a.pkg isEqualToString:self.runningPkg]) aoi_android_stop();
    [a forgetSnapshot];
    BOOL clean = aoi_android_snapshot_fits(a.dir.UTF8String);
    [self append:[NSString stringWithFormat:clean ? @"%@: kayıt silindi, bir sonraki açılış baştan (yine hızlı)."
                                                  : @"%@: kayıt silindi, bir sonraki açılış baştan.", a.label]];
}

- (void)confirmRemove:(AoiApp *)a {
    UIAlertController *al = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"%@ kaldırılsın mı?", a.label]
                                                                message:@"Uygulamanın verileri de silinir."
                                                         preferredStyle:UIAlertControllerStyleAlert];
    [al addAction:[UIAlertAction actionWithTitle:@"Kaldır" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
        if ([a.pkg isEqualToString:self.runningPkg]) aoi_android_stop();
        [self cancelCompile:a];
        dispatch_async(self.appQueue, ^{                 /* after its process has ended, and dex2oat's */
            while (aoi_android_compiling()) {
                struct aoi_compile_info ci;
                aoi_android_compile_info(&ci);
                if (!ci.active || strcmp(ci.datadir, a.dir.UTF8String)) break;
                usleep(100000);
            }
            dispatch_async(dispatch_get_main_queue(), ^{ [a remove]; [self reload]; });
        });
    }]];
    [al addAction:[UIAlertAction actionWithTitle:@"Vazgeç" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:al animated:YES completion:nil];
}

- (void)pick {
    UIDocumentPickerViewController *p =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeItem ] asCopy:YES];
    p.delegate = self;
    [self presentViewController:p animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSData *d = [NSData dataWithContentsOfURL:urls.firstObject options:NSDataReadingMappedIfSafe error:nil];
    NSString *err = nil;
    AoiApp *a = d ? [AoiApp install:d error:&err] : nil;
    if (!a) {
        UIAlertController *al = [UIAlertController alertControllerWithTitle:@"Yüklenemedi" message:err ?: @"Dosya okunamadı."
                                                             preferredStyle:UIAlertControllerStyleAlert];
        [al addAction:[UIAlertAction actionWithTitle:@"Tamam" style:UIAlertActionStyleCancel handler:nil]];
        [self presentViewController:al animated:YES completion:nil];
        return;
    }
    [self append:[NSString stringWithFormat:@"Yüklendi: %@ (%@, %.1f MB)", a.label, a.pkg, d.length / 1e6]];
    if ([a.pkg isEqualToString:self.runningPkg]) {       /* updated while it runs: start it afresh */
        aoi_android_stop();
        [a forgetSnapshot];
        self.runningPkg = nil;
        self.screenVC = nil;
    }
    [self reload];
    if (!a.compiled) {                                   /* once, now: the card shows how far it is */
        [self compile:a faster:NO];
        [self tell:[NSString stringWithFormat:@"%@ yüklendi", a.label]
              what:[NSString stringWithFormat:@"Şimdi bir kez derleniyor (%@); ilerlemesi kartında. Bitince hızlı açılır. "
                                               @"LiquidAPK'yı kapatmayın; arka plana alırsanız derleme sürer. İsterseniz derlenmeden de açabilirsiniz.",
                                               duration_text(aoi_android_compile_estimate(a.dir.UTF8String, 0))]];
    } else {
        [self openApp:a];
    }
}

- (void)openPackage:(NSString *)pkg {
    AoiApp *a = [AoiApp withPackage:pkg];
    if (a) [self maybeOpen:a];
    else [self append:[NSString stringWithFormat:@"Yüklü değil: %@", pkg]];
}

/* The app's screen; its process is started unless it already runs. Another app that
 * runs is saved (snapshot) and ended first: one process at a time. */
- (void)openApp:(AoiApp *)a {
    NSString *root = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"aroot"];
    if (![NSFileManager.defaultManager fileExistsAtPath:[root stringByAppendingPathComponent:@"system/bin/app_process64"]]) {
        [self append:@"Bu IPA'da uygulama dosyaları (framework) yok."];
        return;
    }
    BOOL same = [a.pkg isEqualToString:self.runningPkg] && self.screenVC && [self.screenVC.app.pkg isEqualToString:a.pkg];
    if (self.presentedViewController && self.presentedViewController != self.screenVC)
        [self dismissViewControllerAnimated:NO completion:nil];
    if (!same) {
        if (self.presentedViewController) [self dismissViewControllerAnimated:NO completion:nil];
        self.screenVC = [ScreenVC new];
        self.screenVC.app = a;
        self.screenVC.modalPresentationStyle = UIModalPresentationFullScreen;
    }
    if (self.presentedViewController != self.screenVC) [self presentViewController:self.screenVC animated:YES completion:nil];
    if (same) return;

    NSString *display = self.display;
    NSString *logPath = [a.dir stringByAppendingString:@".log"];
    NSString *prev = self.runningPkg;
    if (prev) {                                          /* one process at a time: save that one, end it */
        [self append:[NSString stringWithFormat:@"%@ kaydedilip kapatılıyor …", prev]];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ aoi_android_snapshot(20); aoi_android_stop(); });
    }
    self.runningPkg = a.pkg;
    compile_hold_update();
    BOOL snap = aoi_android_snapshot_fits(a.dir.UTF8String);   /* (the clean one put back, if the last is gone) */
    [self.screenVC status:snap
         ? @"Kayıttan açılıyor…" : a.compiled ? @"Açılıyor…" : @"Derlenmeden açılıyor (yavaş)…"];
    [a prepare];
    [self append:[NSString stringWithFormat:@"%@ açılıyor (ekran %@)", a.label, display]];
    ScreenVC *target = self.screenVC;
    void *ctx = (__bridge_retained void *)target;        /* the process's frames go to it while it runs */
    if ([self.preparing.pkg isEqualToString:a.pkg] && !self.preparingShown
        && aoi_android_show(frame_cb, home_cb, ctx) == 0) {             /* it is starting already: show it */
        self.preparingShown = YES; self.preparingTarget = target; self.preparingCtx = ctx;
        [self reload];
        return;
    }
    if (self.preparing && !self.preparingShown) {         /* another app now: that one is saved later */
        self.preparingCancelled = YES;
        aoi_android_stop();
    }
    if (!prev && !snap && self.warmRunning && !self.warmApp
        && aoi_android_go(root.UTF8String, a.dir.UTF8String, logPath.UTF8String, display.UTF8String, frame_cb, home_cb,
                          ctx, log_cb, (__bridge void *)self) == 0) {
        self.warmApp = a; self.warmTarget = target; self.warmCtx = ctx;   /* Android is up already: only the app loads */
        [self reload];
        return;
    }
    if (self.warmRunning && !self.warmApp) aoi_android_warm_stop();    /* (its snapshot is faster, or it does not fit) */
    dispatch_async(self.appQueue, ^{
        aoi_android_app(root.UTF8String, a.dir.UTF8String, logPath.UTF8String, display.UTF8String, frame_cb, home_cb,
                        ctx, log_cb, (__bridge void *)self);
        CFRelease(ctx);
        dispatch_async(dispatch_get_main_queue(), ^{ [self appEnded:a target:target log:logPath]; });
    });
    [self reload];
}

- (void)openDev {
    DevVC *d = [DevVC new];
    self.dev = d;
    [self presentViewController:[[UINavigationController alloc] initWithRootViewController:d] animated:YES completion:nil];
}

- (void)append:(NSString *)line {
    NSLog(@"%@", line);
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.log appendFormat:@"%@\n", line];
        self.dev.logView.text = self.log;
        [self.dev.logView scrollRangeToVisible:NSMakeRange(self.log.length, 0)];
        NSURL *doc = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
        [self.log writeToURL:[doc URLByAppendingPathComponent:@"log.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    });
}
@end

/* ---------- the developer page ---------- */

@implementation DevVC
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Geliştirici";
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                                                           target:self action:@selector(done)];
    self.nField = [UITextField new];
    self.nField.text = @"20000";
    self.nField.keyboardType = UIKeyboardTypeNumberPad;
    self.nField.borderStyle = UITextBorderStyleRoundedRect;
    UIStackView *row1 = [self row:@[ [self button:@"Logu kopyala" action:@selector(copyLog)],
                                     [self button:@"Adres alanı" action:@selector(probe)],
                                     [self button:@"Lisanslar" action:@selector(licenses)] ]];
    UIStackView *row2 = [self row:@[ [self button:@"GMP n!" action:@selector(gmp)], self.nField,
                                     [self button:@"Android" action:@selector(android)] ]];
    self.logView = [UITextView new];
    self.logView.editable = NO;
    self.logView.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.logView.backgroundColor = UIColor.secondarySystemBackgroundColor;
    self.logView.layer.cornerRadius = 12;
    self.logView.text = launcher.log;
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[ row1, row2, self.logView ]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 10;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    UILayoutGuide *g = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:16],
        [stack.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-16],
        [stack.topAnchor constraintEqualToAnchor:g.topAnchor constant:8],
        [stack.bottomAnchor constraintEqualToAnchor:g.bottomAnchor constant:-8],
    ]];
}

- (void)done { [self dismissViewControllerAnimated:YES completion:nil]; }

/* LiquidAPK's license (GPL-3.0-or-later) and the bundled components' (licenses/; the
 * Android parts' notices are in aroot/system/etc/NOTICE.xml.gz). */
- (void)licenses {
    NSString *dir = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"licenses"];
    NSMutableString *t = [NSMutableString string];
    NSArray *names = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]
                         sortedArrayUsingSelector:@selector(compare:)];
    for (NSString *n in @[ @"NOTICE.md", @"LICENSE" ]) {
        NSString *s = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:n] encoding:NSUTF8StringEncoding error:nil];
        if (s) [t appendFormat:@"%@\n\n", s];
    }
    for (NSString *n in names) {
        if ([n isEqualToString:@"NOTICE.md"] || [n isEqualToString:@"LICENSE"]) continue;
        NSString *s = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:n] encoding:NSUTF8StringEncoding error:nil];
        if (s) [t appendFormat:@"──── %@ ────\n%@\n\n", n, s];
    }
    self.logView.text = t.length ? t : @"Lisans dosyaları bu derlemede yok.";
    [self.logView scrollRangeToVisible:NSMakeRange(0, 0)];
}

- (UIButton *)button:(NSString *)t action:(SEL)a {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    UIButtonConfiguration *c = [UIButtonConfiguration tintedButtonConfiguration];
    c.title = t;
    b.configuration = c;
    [b addTarget:self action:a forControlEvents:UIControlEventTouchUpInside];
    return b;
}

- (UIStackView *)row:(NSArray *)views {
    UIStackView *s = [[UIStackView alloc] initWithArrangedSubviews:views];
    s.spacing = 8;
    s.distribution = UIStackViewDistributionFillEqually;
    return s;
}

/* The log, and each installed app's own log (its last 60 kB). */
- (void)copyLog {
    NSMutableString *all = [launcher.log mutableCopy];
    for (AoiApp *a in [AoiApp all])
        for (NSString *ext in @[ @".log.1", @".log" ]) {          /* the previous run, then this one */
            NSString *app = [NSString stringWithContentsOfFile:[a.dir stringByAppendingString:ext]
                                                      encoding:NSUTF8StringEncoding error:nil];
            if (!app.length) continue;
            NSString *tail = app.length > 40000 ? [app substringFromIndex:app.length - 40000] : app;
            [all appendFormat:@"\n--- %@%@ ---\n%@", a.pkg, ext, tail];
        }
    UIPasteboard.generalPasteboard.string = all;
    [launcher append:@"(log panoya kopyalandı)"];
}

- (void)probe {
    [launcher append:@"Adres alanı testi (Android programları için 64 GiB, seyrek) ..."];
    dispatch_async(launcher.work, ^{ aoi_vm_probe(log_cb, (__bridge void *)launcher); });
}

/* Step 1 on the device: Android's own linker64 runs toybox and mksh from the guest
 * files bundled in aroot/ (ios/android-files.txt), in the interpreter. */
- (void)android {
    NSString *root = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"aroot"];
    if (![NSFileManager.defaultManager fileExistsAtPath:[root stringByAppendingPathComponent:@"system/bin/toybox"]]) {
        [launcher append:@"Bu IPA'da Android dosyaları (aroot) yok."];
        return;
    }
    NSString *tmp = NSTemporaryDirectory();
    dispatch_async(launcher.work, ^{
        static const char *const echo[] = { "/system/bin/toybox", "echo", "merhaba, ben Android toybox" };
        static const char *const sh[] = { "/system/bin/sh", "-c", "echo mksh: $((6*7)); x=Android; echo ${#x} harf" };
        static const char *const ls[] = { "/system/bin/toybox", "ls", "/system/lib64" };
        void *ctx = (__bridge void *)launcher;
        [launcher append:@"Android linker64 + toybox echo ..."];
        aoi_android_run(root.UTF8String, tmp.UTF8String, 3, echo, log_cb, ctx);
        [launcher append:@"Android mksh ..."];
        aoi_android_run(root.UTF8String, tmp.UTF8String, 3, sh, log_cb, ctx);
        [launcher append:@"Android toybox ls /system/lib64 ..."];
        aoi_android_run(root.UTF8String, tmp.UTF8String, 3, ls, log_cb, ctx);
        [launcher append:@"Android ART (dalvikvm64) hello.dex ..."];
        aoi_android_art_hello(root.UTF8String, tmp.UTF8String, log_cb, ctx);
        [launcher append:@"Android ART GC (20 MB çöp, Runtime.gc) ..."];
        aoi_android_art_gc(root.UTF8String, tmp.UTF8String, log_cb, ctx);
    });
}

/* libgmp.so of an installed Qalculate computes n!, in the interpreter. */
- (void)gmp {
    for (AoiApp *a in [AoiApp all]) {
        NSData *apk = [NSData dataWithContentsOfFile:a.apkPath options:NSDataReadingMappedIfSafe error:nil];
        size_t sz = 0;
        const char *err = NULL;
        void *so = apk ? aoi_apk_extract(apk.bytes, apk.length, "lib/arm64-v8a/libgmp.so", &sz, &err) : NULL;
        if (!so) continue;
        NSData *lib = [NSData dataWithBytesNoCopy:so length:sz freeWhenDone:YES];
        unsigned long n = strtoul(self.nField.text.UTF8String, NULL, 10);
        if (!n) n = 20000;
        dispatch_async(launcher.work, ^{
            double best = 0;
            [launcher append:[NSString stringWithFormat:@"Yorumlayıcı: Android libgmp.so ile %lu!, 3 tur ...", n]];
            for (int round = 1; round <= 3; round++) {
                double t = 0;
                char *r = aoi_gmp_interp(lib.bytes, lib.length, n, &t, log_cb, (__bridge void *)launcher);
                if (!r) { [launcher append:@"başarısız"]; return; }
                if (round == 1) [launcher append:[NSString stringWithFormat:@"%lu! = %.20s… (%zu basamak)", n, r, strlen(r)]];
                if (best == 0 || t < best) best = t;
                free(r);
            }
            [launcher append:[NSString stringWithFormat:@"en iyi tur: %.3f s", best]];
        });
        return;
    }
    [launcher append:@"libgmp.so olan bir uygulama (Qalculate) yüklü değil."];
}
@end

/* ---------- callbacks from the emulator (worker threads) ---------- */

/* Each callback drains its own autorelease pool: the emulator's thread runs one app
 * for as long as it is open, and its own pool would keep every frame's image (5 MB). */
static void log_cb(void *ctx, const char *line) {
    @autoreleasepool {
        [(__bridge Launcher *)ctx append:[NSString stringWithUTF8String:line]];
    }
}

/* A frame from the guest's SurfaceFlinger: RGBX rows -> UIImage (2x), to that app's screen. */
static void frame_cb(void *ctx, const unsigned char *px, unsigned w, unsigned h) {
    @autoreleasepool {
        CFDataRef d = CFDataCreate(NULL, px, (CFIndex)w * h * 4);
        CGDataProviderRef prov = CGDataProviderCreateWithCFData(d);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGImageRef cg = CGImageCreate(w, h, 8, 32, w * 4, cs, kCGBitmapByteOrderDefault | kCGImageAlphaNoneSkipLast,
                                      prov, NULL, false, kCGRenderingIntentDefault);
        UIImage *img = [UIImage imageWithCGImage:cg scale:2 orientation:UIImageOrientationUp];
        CGImageRelease(cg); CGColorSpaceRelease(cs); CGDataProviderRelease(prov); CFRelease(d);
        ScreenVC *vc = (__bridge ScreenVC *)ctx;
        dispatch_async(dispatch_get_main_queue(), ^{ [vc showFrame:img]; });
    }
}

/* The Android app's clipboard is the iPhone's (aoi.Clipboard, core/proc.h): text it
 * copies goes to UIPasteboard, a paste reads it (iOS may ask "Allow Paste"; "has
 * text" does not read it). A link the app opens with no activity of its own for it
 * ('u') goes to iOS. On the app's thread. */
static void clipboard_cb(int op, const char *path) {
    @autoreleasepool {
        UIPasteboard *pb = UIPasteboard.generalPasteboard;
        NSString *file = [NSString stringWithUTF8String:path];
        if (op == 's') {
            NSString *text = [NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil];
            if (text) pb.string = text;
        } else if (op == 'g') {
            NSString *text = pb.hasStrings ? pb.string : nil;
            if (text) [text writeToFile:file atomically:NO encoding:NSUTF8StringEncoding error:nil];
            else [NSFileManager.defaultManager removeItemAtPath:file error:nil];
        } else if (op == 'h') {
            [(pb.hasStrings ? @"1" : @"0") writeToFile:file atomically:NO encoding:NSUTF8StringEncoding error:nil];
        } else if (op == 'u') {                     /* a link the app opens: Safari (or the app iOS has for it) */
            NSString *text = [NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil];
            NSURL *url = text ? [NSURL URLWithString:text] : nil;
            if (url) dispatch_async(dispatch_get_main_queue(), ^{
                [UIApplication.sharedApplication openURL:url options:@{} completionHandler:nil];
            });
        }
    }
}

/* dex2oat ended (on its thread): the cards, and the next one in the queue. */
static void compile_done_cb(const char *datadir, const char *state) {
    NSString *d = @(datadir), *st = @(state);
    dispatch_async(dispatch_get_main_queue(), ^{ [launcher compileEnded:d state:st]; });
}

/* The app's text field wants the keyboard, or no longer (aoi.InputMethodManager). */
static void keyboard_cb(int show) {
    dispatch_async(dispatch_get_main_queue(), ^{
        AoiScreen *s = current_screen;
        if (!s) return;
        if (show) [s becomeFirstResponder];
        else [s resignFirstResponder];
    });
}

/* The app left for its launcher (back on its root screen): ours comes up. */
static void home_cb(void *ctx) {
    @autoreleasepool {
        ScreenVC *vc = (__bridge ScreenVC *)ctx;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (launcher.presentedViewController == vc) [launcher dismissViewControllerAnimated:YES completion:nil];
        });
    }
}

/* ---------- the application ---------- */

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
    aoi_android_set_clipboard(clipboard_cb);
    aoi_android_set_keyboard(keyboard_cb);
    aoi_android_set_compile_done(compile_done_cb);
    for (NSNotificationName n in @[ NSProcessInfoThermalStateDidChangeNotification,
                                    NSProcessInfoPowerStateDidChangeNotification ])
        [NSNotificationCenter.defaultCenter addObserverForName:n object:nil queue:nil
                                                    usingBlock:^(NSNotification *note) { compile_hold_update(); }];
    compile_hold_update();
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;   /* white on dark glass, in light mode too */
    self.window.rootViewController = [Launcher new];
    [self.window makeKeyAndVisible];
    UIApplicationShortcutItem *item = opts[UIApplicationLaunchOptionsShortcutItemKey];
    if (item) { [self open:(NSString *)item.userInfo[@"app"]]; return NO; }
    return YES;
}

/* liquidapk://open?app=<package> (or aoi://, 0.19's): a home-screen link (Shortcuts) or another app. */
- (BOOL)application:(UIApplication *)app openURL:(NSURL *)url options:(NSDictionary *)opts {
    NSURLComponents *c = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
    for (NSURLQueryItem *q in c.queryItems)
        if ([q.name isEqualToString:@"app"] && q.value.length) { [self open:q.value]; return YES; }
    return NO;
}

- (void)application:(UIApplication *)app performActionForShortcutItem:(UIApplicationShortcutItem *)item
  completionHandler:(void (^)(BOOL))done {
    [self open:(NSString *)item.userInfo[@"app"]];
    done(YES);
}

- (void)open:(NSString *)pkg {
    if (!pkg.length) return;
    dispatch_async(dispatch_get_main_queue(), ^{     /* after the launcher's view is in the window */
        [launcher openPackage:pkg];
    });
}

/* ---------- the compile in the background ----------
 * iOS suspends an app soon after it leaves the screen, and dex2oat with it. While a
 * compile (or the saving after it) runs, LiquidAPK plays silence, mixed with the other
 * apps' sound (UIBackgroundModes audio): iOS lets it run on, the compile ends, and the
 * player stops. Nothing plays once the work is done or LiquidAPK is back. */
static AVAudioPlayer *keep_player;
static dispatch_source_t keep_timer;

static NSData *silence_wav(void)
{
    enum { RATE = 8000, N = RATE };                     /* 1 s, 16-bit mono */
    NSMutableData *d = [NSMutableData dataWithLength:44 + N * 2];
    uint8_t *h = d.mutableBytes;
    uint32_t v;
    uint16_t u;
    memcpy(h, "RIFF", 4); v = 36 + N * 2; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4); u = 1; memcpy(h + 20, &u, 2); memcpy(h + 22, &u, 2);
    v = RATE; memcpy(h + 24, &v, 4); v = RATE * 2; memcpy(h + 28, &v, 4); u = 2; memcpy(h + 32, &u, 2);
    u = 16; memcpy(h + 34, &u, 2); memcpy(h + 36, "data", 4); v = N * 2; memcpy(h + 40, &v, 4);
    return d;
}

static BOOL background_work(void)
{
    return aoi_android_compiling() || launcher.preparing || launcher.queue.count;
}

static void keep_alive(BOOL on)
{
    if (!on) {
        if (keep_timer) { dispatch_source_cancel(keep_timer); keep_timer = nil; }
        [keep_player stop];
        keep_player = nil;
        return;
    }
    if (keep_player) return;
    AVAudioSession *as = AVAudioSession.sharedInstance;
    [as setCategory:AVAudioSessionCategoryPlayback withOptions:AVAudioSessionCategoryOptionMixWithOthers error:nil];
    [as setActive:YES error:nil];
    keep_player = [[AVAudioPlayer alloc] initWithData:silence_wav() error:nil];
    keep_player.numberOfLoops = -1;
    keep_player.volume = 0;
    [keep_player play];
    keep_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(keep_timer, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC, NSEC_PER_SEC);
    dispatch_source_set_event_handler(keep_timer, ^{
        if (!background_work()) {
            [launcher append:@"Arka plandaki iş bitti."];
            keep_alive(NO);
        } else if (!keep_player.playing) {
            [keep_player play];                         /* after a call, or another app's audio session */
        }
    });
    dispatch_resume(keep_timer);
}

/* Going to the background (iOS may end us there): the running app is saved as it is,
 * so the next launch resumes it with what was typed since its first snapshot. With a
 * compile going on it is then ended (it resumes in a second, from that snapshot): the
 * compile is the one job, iOS keeps LiquidAPK running for it (keep_alive). */
- (void)applicationDidEnterBackground:(UIApplication *)app {
    BOOL work = background_work();
    NSString *pkg = launcher.runningPkg;
    AoiApp *a = pkg ? [AoiApp withPackage:pkg] : nil;
    launcher.inBackground = YES;
    compile_hold_update();
    if (work) {
        keep_alive(YES);
        [launcher append:@"LiquidAPK arka planda: derleme sürüyor."];
    }
    __block UIBackgroundTaskIdentifier task = [app beginBackgroundTaskWithExpirationHandler:^{
        [app endBackgroundTask:task];
        task = UIBackgroundTaskInvalid;
    }];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        time_t t0 = time(NULL);
        struct stat st;
        int saved = aoi_android_snapshot(25) == 0;
        if (work && a && saved && !stat([a.dir stringByAppendingString:@".snap"].UTF8String, &st) && st.st_mtime >= t0) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (launcher.inBackground && [launcher.runningPkg isEqualToString:pkg]) {
                    [launcher append:[NSString stringWithFormat:@"%@ kaydedilip kapatıldı (derleme için); dokununca kaldığı yerden açılır.", a.label]];
                    aoi_android_stop();
                }
            });
        }
        while (aoi_android_compiling() && task != UIBackgroundTaskInvalid && !keep_player) usleep(500000);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (task != UIBackgroundTaskInvalid) [app endBackgroundTask:task];
            task = UIBackgroundTaskInvalid;
        });
    });
}

- (void)applicationWillEnterForeground:(UIApplication *)app {
    launcher.inBackground = NO;
    keep_alive(NO);
    compile_hold_update();
    [launcher idleNext];
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
