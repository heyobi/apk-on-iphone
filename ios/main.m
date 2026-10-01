// apk-on-iphone test app: pick an APK and run its libgmp.so in the interpreter
// (no JIT, no debugger: works on any iPhone). Built without an Xcode project
// by .github/workflows/ios.yml.
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <sys/sysctl.h>

#include "../core/apk.h"
#include "gmptest.h"
#include "vmprobe.h"
#include "androidtest.h"

/* The Android app's screen: its frames, aspect-fit; one-finger touches go to the app
 * in its pixels (aoi_android_touch). A swipe from the left edge is Android's back. */
@interface AoiScreen : UIImageView
@end

@implementation AoiScreen
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

@interface VC : UIViewController <UIDocumentPickerDelegate>
@property(nonatomic, strong) UITextView *logView;
@property(nonatomic, strong) UITextField *nField;
@property(nonatomic, strong) NSData *apk;
@property(nonatomic, strong) NSMutableString *log;
@property(nonatomic) dispatch_queue_t work;
@property(nonatomic, strong) AoiScreen *screen;      /* the Android app's frames */
@property(atomic) BOOL appRunning;                   /* its process is alive (on self.appQueue) */
@property(nonatomic) dispatch_queue_t appQueue;      /* the app's process: apart from the tests */
@end

static void log_cb(void *ctx, const char *line);
static void frame_cb(void *ctx, const unsigned char *px, unsigned w, unsigned h);
static int list_cb(const char *name, size_t len, void *ctx);

@implementation VC

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.log = [NSMutableString string];
    self.work = dispatch_queue_create("aoi.work", DISPATCH_QUEUE_SERIAL);
    self.appQueue = dispatch_queue_create("aoi.app", DISPATCH_QUEUE_SERIAL);

    UILabel *title = [UILabel new];
    title.text = @"apk-on-iphone";
    title.font = [UIFont boldSystemFontOfSize:22];

    self.nField = [UITextField new];
    self.nField.text = @"20000";
    self.nField.keyboardType = UIKeyboardTypeNumberPad;
    self.nField.borderStyle = UITextBorderStyleRoundedRect;

    UIStackView *row1 = [self row:@[ [self button:@"APK seç" action:@selector(pick)], self.nField ]];
    UIStackView *row2 = [self row:@[ [self button:@"Çalıştır" action:@selector(run)],
                                     [self button:@"Android" action:@selector(android)],
                                     [self button:@"Uygulama" action:@selector(openApp)],
                                     [self button:@"Logu kopyala" action:@selector(copyLog)] ]];

    self.logView = [UITextView new];
    self.logView.editable = NO;
    self.logView.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.logView.backgroundColor = UIColor.secondarySystemBackgroundColor;
    self.logView.layer.cornerRadius = 8;

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[ title, row1, row2, self.logView ]];
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

    char machine[64] = "?";
    size_t len = sizeof machine;
    sysctlbyname("hw.machine", machine, &len, NULL, 0);
    [self append:[NSString stringWithFormat:@"Cihaz: %s, iOS %@", machine, UIDevice.currentDevice.systemVersion]];
    if ([NSFileManager.defaultManager fileExistsAtPath:[self installedApk]]) {   /* the app from last time */
        [self append:@"Yüklü uygulama açılıyor ..."];
        dispatch_async(dispatch_get_main_queue(), ^{ [self openApp]; });
        return;
    }
    [self append:@"Bir APK seçin (APK seç); uygulama açılır. Sonraki açılışlarda kendiliğinden başlar."];
    [self append:@"Adres alanı testi (Android programları için 64 GiB, seyrek) ..."];
    dispatch_async(self.work, ^{ aoi_vm_probe(log_cb, (__bridge void *)self); });
}

/* Where the installed APK lives: the guest's /data/app/apk/base.apk. */
- (NSString *)installedApk {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    return [docs stringByAppendingPathComponent:@"adata/app/apk/base.apk"];
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
    s.axis = UILayoutConstraintAxisHorizontal;
    s.spacing = 8;
    s.distribution = UIStackViewDistributionFillEqually;
    return s;
}

- (void)append:(NSString *)line {
    NSLog(@"%@", line);
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.log appendFormat:@"%@\n", line];
        self.logView.text = self.log;
        [self.logView scrollRangeToVisible:NSMakeRange(self.log.length, 0)];
        NSURL *doc = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
        [self.log writeToURL:[doc URLByAppendingPathComponent:@"log.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    });
}

- (void)copyLog {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *app = [NSString stringWithContentsOfFile:[docs stringByAppendingPathComponent:@"app.log"]
                                              encoding:NSUTF8StringEncoding error:nil];
    NSString *all = self.log;
    if (app.length) {                                           /* plus the app's own log, its last 60 kB */
        NSString *tail = app.length > 60000 ? [app substringFromIndex:app.length - 60000] : app;
        all = [NSString stringWithFormat:@"%@\n--- app.log ---\n%@", self.log, tail];
    }
    UIPasteboard.generalPasteboard.string = all;
    [self append:@"(log panoya kopyalandı)"];
}

- (void)pick {
    UIDocumentPickerViewController *p =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeItem ] asCopy:YES];
    p.delegate = self;
    [self presentViewController:p animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *u = urls.firstObject;
    NSData *d = [NSData dataWithContentsOfURL:u options:NSDataReadingMappedIfSafe error:nil];
    if (!d) { [self append:@"APK okunamadı."]; return; }
    self.apk = d;
    NSMutableString *libs = [NSMutableString string];
    aoi_apk_list(d.bytes, d.length, list_cb, (__bridge void *)libs);
    [self append:[NSString stringWithFormat:@"APK: %@ (%.1f MB)\n  arm64-v8a kütüphaneleri:\n%@",
                  u.lastPathComponent, d.length / 1e6, libs.length ? libs : @"    yok\n"]];
    if (!self.appRunning) [self openApp];                       /* install it and start */
    else [self append:@"Önceki uygulama hâlâ çalışıyor; yenisi iPhone uygulaması yeniden açılınca başlar."];
}

static int list_cb(const char *name, size_t len, void *ctx) {
    NSString *s = [[NSString alloc] initWithBytes:name length:len encoding:NSUTF8StringEncoding];
    if ([s hasPrefix:@"lib/arm64-v8a/"] && [s hasSuffix:@".so"])
        [(__bridge NSMutableString *)ctx appendFormat:@"    %@\n", [s substringFromIndex:14]];
    return 0;
}

// Step 1 on the device: Android's own linker64 runs toybox and mksh from the
// guest files bundled in aroot/ (ios/android-files.txt), in the interpreter.
- (void)android {
    NSString *root = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"aroot"];
    if (![NSFileManager.defaultManager fileExistsAtPath:[root stringByAppendingPathComponent:@"system/bin/toybox"]]) {
        [self append:@"Bu IPA'da Android dosyaları (aroot) yok."];
        return;
    }
    NSString *tmp = NSTemporaryDirectory();
    dispatch_async(self.work, ^{
        static const char *const echo[] = { "/system/bin/toybox", "echo", "merhaba, ben Android toybox" };
        static const char *const sh[] = { "/system/bin/sh", "-c", "echo mksh: $((6*7)); x=Android; echo ${#x} harf" };
        static const char *const ls[] = { "/system/bin/toybox", "ls", "/system/lib64" };
        [self append:@"Android linker64 + toybox echo ..."];
        aoi_android_run(root.UTF8String, tmp.UTF8String, 3, echo, log_cb, (__bridge void *)self);
        [self append:@"Android mksh ..."];
        aoi_android_run(root.UTF8String, tmp.UTF8String, 3, sh, log_cb, (__bridge void *)self);
        [self append:@"Android toybox ls /system/lib64 ..."];
        aoi_android_run(root.UTF8String, tmp.UTF8String, 3, ls, log_cb, (__bridge void *)self);
        [self append:@"Android ART (dalvikvm64) hello.dex ..."];
        aoi_android_art_hello(root.UTF8String, tmp.UTF8String, log_cb, (__bridge void *)self);
        [self append:@"Android ART GC (20 MB çöp, Runtime.gc) ..."];
        aoi_android_art_gc(root.UTF8String, tmp.UTF8String, log_cb, (__bridge void *)self);
    });
}

/* The whole APK: framework, our services and the app's own code, in the interpreter.
 * Its frames are shown full screen (tap twice to see the log again). */
- (void)openApp {
    if (self.appRunning) {                                       /* still running: just show it again */
        if (self.screen.image) [self showFrame:self.screen.image];
        else [self append:@"Uygulama hâlâ açılıyor, ilk kareyi bekleyin."];
        return;
    }
    NSFileManager *fm = NSFileManager.defaultManager;
    if (!self.apk && ![fm fileExistsAtPath:[self installedApk]]) { [self append:@"Önce bir APK seçin."]; return; }
    NSString *root = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"aroot"];
    if (![fm fileExistsAtPath:[root stringByAppendingPathComponent:@"system/bin/app_process64"]]) {
        [self append:@"Bu IPA'da uygulama dosyaları (framework) yok."];
        return;
    }
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *data = [docs stringByAppendingPathComponent:@"adata"];
    NSString *logPath = [docs stringByAppendingPathComponent:@"app.log"];
    /* /data lives on between runs (the app's settings, history, ART's caches); what the
     * bundle provides in it (aoi.dex, the classpath) is refreshed from this build */
    NSError *e = nil;
    NSString *bundled = [root stringByAppendingPathComponent:@"data"];
    if (![fm fileExistsAtPath:data] && ![fm copyItemAtPath:bundled toPath:data error:&e]) {
        [self append:[NSString stringWithFormat:@"/data hazırlanamadı: %@", e.localizedDescription]];
        return;
    }
    for (NSString *f in @[ @"local/tmp/aoi.dex", @"system/environ/classpath" ]) {
        NSString *dst = [data stringByAppendingPathComponent:f];
        [fm createDirectoryAtPath:dst.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
        [fm removeItemAtPath:dst error:nil];
        [fm copyItemAtPath:[bundled stringByAppendingPathComponent:f] toPath:dst error:nil];
    }
    [fm createDirectoryAtPath:[data stringByAppendingPathComponent:@"dalvik-cache/arm64"]
  withIntermediateDirectories:YES attributes:nil error:nil];   /* ART's oat files for the app and aoi.dex */
    NSString *apkDir = [data stringByAppendingPathComponent:@"app/apk"];
    [fm createDirectoryAtPath:apkDir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *apkPath = [apkDir stringByAppendingPathComponent:@"base.apk"];
    if (self.apk && ![[NSData dataWithContentsOfFile:apkPath] isEqualToData:self.apk])   /* a new APK: compiled again */
        [self.apk writeToFile:apkPath atomically:NO];

    if (!self.screen) {
        self.screen = [[AoiScreen alloc] initWithFrame:self.view.bounds];
        self.screen.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        self.screen.contentMode = UIViewContentModeScaleAspectFit;
        self.screen.backgroundColor = UIColor.blackColor;
        self.screen.userInteractionEnabled = YES;
        UITapGestureRecognizer *t = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(hideScreen)];
        t.numberOfTouchesRequired = 2;                          /* two fingers: back to the log */
        t.cancelsTouchesInView = NO;
        [self.screen addGestureRecognizer:t];
        UIScreenEdgePanGestureRecognizer *e = [[UIScreenEdgePanGestureRecognizer alloc] initWithTarget:self
                                                                                               action:@selector(edgeSwipe:)];
        e.edges = UIRectEdgeLeft;                               /* from the left edge: Android's back */
        [self.screen addGestureRecognizer:e];
    }
    [self append:@"Uygulama başlıyor: ilk açılış ~1 dakika, sonrakiler kayıttan (snapshot) birkaç saniye. Soldan kaydırmak: geri. İki parmakla dokunmak: bu ekran."];
    self.appRunning = YES;
    dispatch_async(self.appQueue, ^{
        aoi_android_app(root.UTF8String, data.UTF8String, logPath.UTF8String, frame_cb, (__bridge void *)self,
                        log_cb, (__bridge void *)self);
        self.appRunning = NO;
        [self append:[NSString stringWithFormat:@"Uygulamanın logu: %@", logPath]];
    });
}

- (void)edgeSwipe:(UIScreenEdgePanGestureRecognizer *)g {
    if (g.state == UIGestureRecognizerStateEnded && [g translationInView:g.view].x > 40) aoi_android_back();
}

- (void)hideScreen {
    [self.screen removeFromSuperview];
    [self setNeedsStatusBarAppearanceUpdate];
    [self setNeedsUpdateOfHomeIndicatorAutoHidden];
}

/* The app's own status bar is in its frames: iOS's would cover it. */
- (BOOL)prefersStatusBarHidden { return self.screen.superview != nil; }
- (BOOL)prefersHomeIndicatorAutoHidden { return self.screen.superview != nil; }

- (void)showFrame:(UIImage *)img {
    self.screen.image = img;
    if (!self.screen.superview) {
        self.screen.frame = self.view.bounds;
        [self.view addSubview:self.screen];
        [self setNeedsStatusBarAppearanceUpdate];
        [self setNeedsUpdateOfHomeIndicatorAutoHidden];
    }
}

- (void)run {
    if (!self.apk) { [self append:@"Önce bir APK seçin."]; return; }
    size_t sz = 0;
    const char *err = NULL;
    void *so = aoi_apk_extract(self.apk.bytes, self.apk.length, "lib/arm64-v8a/libgmp.so", &sz, &err);
    if (!so) { [self append:[NSString stringWithFormat:@"libgmp.so yok (%s). Bu test Qalculate APK'sı ister.", err]]; return; }
    NSData *lib = [NSData dataWithBytesNoCopy:so length:sz freeWhenDone:YES];
    unsigned long n = strtoul(self.nField.text.UTF8String, NULL, 10);
    if (!n) n = 20000;
    dispatch_async(self.work, ^{
        double best = 0;
        [self append:[NSString stringWithFormat:@"Yorumlayıcı: Android libgmp.so ile %lu!, 3 tur ...", n]];
        for (int round = 1; round <= 3; round++) {
            double t = 0;
            char *r = aoi_gmp_interp(lib.bytes, lib.length, n, &t, log_cb, (__bridge void *)self);
            if (!r) { [self append:@"başarısız"]; return; }
            if (round == 1) [self append:[NSString stringWithFormat:@"%lu! = %.20s… (%zu basamak)", n, r, strlen(r)]];
            if (best == 0 || t < best) best = t;
            free(r);
        }
        [self append:[NSString stringWithFormat:@"en iyi tur: %.3f s", best]];
    });
}

@end

static void log_cb(void *ctx, const char *line) {
    [(__bridge VC *)ctx append:[NSString stringWithUTF8String:line]];
}

/* A frame from the guest's SurfaceFlinger (worker thread): RGBX rows -> UIImage. */
static void frame_cb(void *ctx, const unsigned char *px, unsigned w, unsigned h) {
    CFDataRef d = CFDataCreate(NULL, px, (CFIndex)w * h * 4);
    CGDataProviderRef prov = CGDataProviderCreateWithCFData(d);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGImageRef cg = CGImageCreate(w, h, 8, 32, w * 4, cs, kCGBitmapByteOrderDefault | kCGImageAlphaNoneSkipLast,
                                  prov, NULL, false, kCGRenderingIntentDefault);
    UIImage *img = [UIImage imageWithCGImage:cg];
    CGImageRelease(cg); CGColorSpaceRelease(cs); CGDataProviderRelease(prov); CFRelease(d);
    VC *vc = (__bridge VC *)ctx;
    dispatch_async(dispatch_get_main_queue(), ^{ [vc showFrame:img]; });
}

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [VC new];
    [self.window makeKeyAndVisible];
    return YES;
}

/* Going to the background (iOS may end us there): the running app is saved as it is,
 * so the next launch resumes it with what was typed since its first snapshot. */
- (void)applicationDidEnterBackground:(UIApplication *)app {
    __block UIBackgroundTaskIdentifier task = [app beginBackgroundTaskWithExpirationHandler:^{
        [app endBackgroundTask:task];
        task = UIBackgroundTaskInvalid;
    }];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        aoi_android_snapshot(25);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (task != UIBackgroundTaskInvalid) [app endBackgroundTask:task];
            task = UIBackgroundTaskInvalid;
        });
    });
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
