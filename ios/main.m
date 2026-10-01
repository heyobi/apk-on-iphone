// apk-on-iphone test app: pick an APK, run its libgmp.so in the interpreter and
// natively from JIT memory, and show what this iPhone allows. Built without an
// Xcode project by .github/workflows/ios.yml.
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <sys/sysctl.h>

#include "../core/apk.h"
#include "gmptest.h"
#include "jitmem.h"

static NSString *const kProbing = @"aoi.probing";        // strategy being probed (crash marker)
static NSString *const kBad = @"aoi.badStrategies";      // strategies that crashed the app before
static const int kTxmInit = 100;                          // crash-marker value for the TXM handshake
static const size_t kTxmPool = 64u << 20;                 // executable pool requested from StikDebug

@interface VC : UIViewController <UIDocumentPickerDelegate>
@property(nonatomic, strong) UITextView *logView;
@property(nonatomic, strong) UITextField *nField;
@property(nonatomic, strong) NSData *apk;
@property(nonatomic, strong) NSString *apkName;
@property(nonatomic, strong) NSMutableString *log;
@property(nonatomic) dispatch_queue_t work;
@property(nonatomic) double lastNative;
@end

static void log_cb(void *ctx, const char *line);
static int list_cb(const char *name, size_t len, void *ctx);

@implementation VC

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.log = [NSMutableString string];
    self.work = dispatch_queue_create("aoi.work", DISPATCH_QUEUE_SERIAL);

    UILabel *title = [UILabel new];
    title.text = @"apk-on-iphone";
    title.font = [UIFont boldSystemFontOfSize:22];

    self.nField = [UITextField new];
    self.nField.text = @"20000";
    self.nField.keyboardType = UIKeyboardTypeNumberPad;
    self.nField.borderStyle = UITextBorderStyleRoundedRect;
    self.nField.placeholder = @"n (n! hesaplanir)";

    UIStackView *row1 = [self row:@[ [self button:@"APK seç" action:@selector(pick)], self.nField ]];
    UIStackView *row2 = [self row:@[ [self button:@"JIT al" action:@selector(requestJIT)],
                                     [self button:@"Hepsini çalıştır" action:@selector(runAll)],
                                     [self button:@"Logu kopyala" action:@selector(copyLog)] ]];
    UIStackView *row3 = [self row:@[ [self button:@"1 Yorumlayıcı" action:@selector(runInterp)],
                                     [self button:@"2 JIT yokla" action:@selector(runProbe)],
                                     [self button:@"3 Native" action:@selector(runNative)] ]];

    self.logView = [UITextView new];
    self.logView.editable = NO;
    self.logView.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.logView.backgroundColor = UIColor.secondarySystemBackgroundColor;
    self.logView.layer.cornerRadius = 8;

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[ title, row1, row2, row3, self.logView ]];
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

    [self deviceInfo];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(becameActive)
                                               name:UIApplicationDidBecomeActiveNotification object:nil];
    dispatch_async(self.work, ^{ [self ensureTXM]; });
}

// ---- JIT acquisition ----

- (NSString *)strategyName:(int)s { return s == kTxmInit ? @"TXM hazırlığı (StikDebug script)" : @(aoi_jit_name(s)); }

- (BOOL)isBad:(int)s { return [[NSUserDefaults.standardUserDefaults arrayForKey:kBad] containsObject:@(s)]; }

- (void)requestJIT {
    if (aoi_jit_debugged() && (!aoi_jit_txm_likely() || aoi_jit_txm_ready())) { [self append:@"JIT zaten açık."]; return; }
    NSURLComponents *c = [NSURLComponents new];
    c.scheme = @"stikdebug";
    c.host = @"enable-jit";
    NSMutableArray *q = [NSMutableArray arrayWithObjects:
        [NSURLQueryItem queryItemWithName:@"bundle-id" value:NSBundle.mainBundle.bundleIdentifier],
        [NSURLQueryItem queryItemWithName:@"pid" value:[NSString stringWithFormat:@"%d", getpid()]], nil];
    if (aoi_jit_txm_likely()) {
        [q addObject:[NSURLQueryItem queryItemWithName:@"script-name" value:@"universal.js"]];
        [NSUserDefaults.standardUserDefaults removeObjectForKey:kBad];   /* a fresh, correct attach: retry everything */
    }
    c.queryItems = q;
    [self append:[NSString stringWithFormat:@"StikDebug açılıyor: %@", c.URL.absoluteString]];
    [UIApplication.sharedApplication openURL:c.URL options:@{} completionHandler:^(BOOL ok) {
        if (!ok) [self append:@"StikDebug açılamadı (kurulu mu?)."];
    }];
}

- (void)becameActive { dispatch_async(self.work, ^{ [self ensureTXM]; }); }

/* Waits briefly for CS_DEBUGGED; on TXM devices then does the region handshake once. */
- (void)ensureTXM {
    for (int i = 0; i < 40 && !aoi_jit_debugged(); i++) usleep(250000);
    if (!aoi_jit_debugged()) return;
    if (!aoi_jit_txm_likely() || aoi_jit_txm_ready()) return;
    if ([self isBad:kTxmInit]) {
        [self append:@"TXM hazırlığı daha önce çöktü: uygulamayı 'JIT al' ile (universal script) yeniden açın."];
        return;
    }
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    [d setObject:@(kTxmInit) forKey:kProbing];
    [d synchronize];
    const char *err = aoi_jit_txm_init(kTxmPool);
    [d removeObjectForKey:kProbing];
    [d synchronize];
    [self append:err ? [NSString stringWithFormat:@"TXM hazırlığı başarısız: %s", err]
                     : [NSString stringWithFormat:@"TXM: %zu MB çalıştırılabilir alan hazır ✅", kTxmPool >> 20]];
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

- (void)deviceInfo {
    char machine[64] = "?";
    size_t len = sizeof machine;
    sysctlbyname("hw.machine", machine, &len, NULL, 0);
    [self append:[NSString stringWithFormat:@"Cihaz: %s, iOS %@", machine, UIDevice.currentDevice.systemVersion]];
    [self append:[NSString stringWithFormat:@"JIT (CS_DEBUGGED): %@", aoi_jit_debugged() ? @"VAR" : @"YOK - 'JIT al' ile StikDebug'ı açın"]];
    [self append:[NSString stringWithFormat:@"TXM (iOS 26+ yeni JIT kuralı): %@", aoi_jit_txm_likely() ? @"bu cihazda var" : @"yok"]];
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    NSNumber *crashed = [d objectForKey:kProbing];
    if (crashed) {
        NSMutableArray *bad = [[d arrayForKey:kBad] mutableCopy] ?: [NSMutableArray array];
        [bad addObject:crashed];
        [d setObject:bad forKey:kBad];
        [d removeObjectForKey:kProbing];
        [self append:[NSString stringWithFormat:@"Önceki çalıştırmada '%@' uygulamayı kapattı; artık atlanıyor.",
                      [self strategyName:crashed.intValue]]];
    }
    [self append:@"APK seçin (ör. Qalculate), sonra 'Hepsini çalıştır'."];
}

- (void)copyLog {
    UIPasteboard.generalPasteboard.string = self.log;
    [self append:@"(log panoya kopyalandı)"];
}

// ---- APK picking ----

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
    self.apkName = u.lastPathComponent;
    [self append:[NSString stringWithFormat:@"APK: %@ (%.1f MB)", self.apkName, d.length / 1e6]];
    int nlibs = 0;
    NSMutableString *libs = [NSMutableString string];
    aoi_apk_list(d.bytes, d.length, list_cb, (__bridge void *)libs);
    for (NSString *l in [libs componentsSeparatedByString:@"\n"]) if (l.length) nlibs++;
    [self append:[NSString stringWithFormat:@"  arm64-v8a kütüphaneleri (%d):%@", nlibs, libs.length ? [@"\n" stringByAppendingString:libs] : @" yok"]];
}

static int list_cb(const char *name, size_t len, void *ctx) {
    NSString *s = [[NSString alloc] initWithBytes:name length:len encoding:NSUTF8StringEncoding];
    if ([s hasPrefix:@"lib/arm64-v8a/"] && [s hasSuffix:@".so"])
        [(__bridge NSMutableString *)ctx appendFormat:@"    %@\n", [s substringFromIndex:14]];
    return 0;
}

- (NSData *)gmp {
    if (!self.apk) { [self append:@"Önce bir APK seçin."]; return nil; }
    size_t sz = 0;
    const char *err = NULL;
    void *so = aoi_apk_extract(self.apk.bytes, self.apk.length, "lib/arm64-v8a/libgmp.so", &sz, &err);
    if (!so) { [self append:[NSString stringWithFormat:@"libgmp.so yok (%s). Bu test Qalculate APK'sı ister.", err]]; return nil; }
    return [NSData dataWithBytesNoCopy:so length:sz freeWhenDone:YES];
}

- (unsigned long)n { unsigned long n = strtoul(self.nField.text.UTF8String, NULL, 10); return n ? n : 20000; }

// ---- tests ----

- (NSString *)doInterp:(NSData *)so n:(unsigned long)n {
    double t = 0;
    [self append:[NSString stringWithFormat:@"[1] Yorumlayıcı: %lu! ...", n]];
    char *r = aoi_gmp_interp(so.bytes, so.length, n, &t, log_cb, (__bridge void *)self);
    if (!r) { [self append:@"[1] başarısız"]; return nil; }
    NSString *s = [NSString stringWithUTF8String:r];
    free(r);
    [self append:[NSString stringWithFormat:@"[1] %lu basamak, %.3f s", (unsigned long)s.length, t]];
    return s;
}

- (int)doProbe {
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    NSArray *bad = [d arrayForKey:kBad] ?: @[];
    int first = -1;
    [self append:@"[2] JIT yöntemleri:"];
    if (!aoi_jit_debugged()) { [self append:@"  CS_DEBUGGED yok: önce 'JIT al'. (Denemek uygulamayı öldürürdü, atlandı.)"]; return -1; }
    BOOL txm = aoi_jit_txm_likely();
    if (txm) {
        [self ensureTXM];
        [self append:@"  TXM cihazı: yalnızca StikDebug havuzu denenir (eski yöntemler bu cihazda çalışmaz)."];
    }
    for (int s = 0; s < AOI_JIT_COUNT; s++) {
        if ((s == AOI_JIT_TXM) != txm) continue;
        if ([bad containsObject:@(s)]) { [self append:[NSString stringWithFormat:@"  %s: daha önce çöktü, atlandı", aoi_jit_name(s)]]; continue; }
        [d setObject:@(s) forKey:kProbing];
        [d synchronize];
        int ok = aoi_jit_probe(s, log_cb, (__bridge void *)self);
        [d removeObjectForKey:kProbing];
        [d synchronize];
        if (ok && first < 0) first = s;
    }
    [self append:first >= 0 ? [NSString stringWithFormat:@"[2] kullanılacak: %s", aoi_jit_name(first)]
                            : @"[2] çalışan JIT yöntemi yok"];
    return first;
}

- (NSString *)doNative:(NSData *)so n:(unsigned long)n strategy:(int)s {
    double t = 0;
    [self append:[NSString stringWithFormat:@"[3] Native (%s): %lu! ...", aoi_jit_name(s), n]];
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    [d setObject:@(s) forKey:kProbing];
    [d synchronize];
    char *r = aoi_gmp_native(so.bytes, so.length, s, n, 1, &t, log_cb, (__bridge void *)self);
    [d removeObjectForKey:kProbing];
    [d synchronize];
    if (!r) { [self append:@"[3] başarısız"]; return nil; }
    NSString *str = [NSString stringWithUTF8String:r];
    free(r);
    [self append:[NSString stringWithFormat:@"[3] %lu basamak, %.4f s", (unsigned long)str.length, t]];
    self.lastNative = t;
    return str;
}

- (void)runInterp { NSData *so = [self gmp]; if (!so) return; unsigned long n = [self n]; dispatch_async(self.work, ^{ [self doInterp:so n:n]; }); }
- (void)runProbe { dispatch_async(self.work, ^{ [self doProbe]; }); }
- (void)runNative {
    NSData *so = [self gmp]; if (!so) return;
    unsigned long n = [self n];
    dispatch_async(self.work, ^{ int s = [self doProbe]; if (s >= 0) [self doNative:so n:n strategy:s]; });
}

- (void)runAll {
    NSData *so = [self gmp]; if (!so) return;
    unsigned long n = [self n];
    dispatch_async(self.work, ^{
        double t0 = CACurrentMediaTime();
        NSString *a = [self doInterp:so n:n];
        double ti = CACurrentMediaTime() - t0;
        int s = [self doProbe];
        NSString *b = s >= 0 ? [self doNative:so n:n strategy:s] : nil;
        if (a && b) {
            double tn = self.lastNative;
            [self append:[NSString stringWithFormat:@"SONUÇ: sonuçlar %@; native %.0fx daha hızlı (yorumlayıcı ~%.2f s, native %.4f s)",
                          [a isEqualToString:b] ? @"AYNI ✅" : @"FARKLI ❌", tn > 0 ? ti / tn : 0, ti, tn]];
        }
        [self append:@"Bitti. 'Logu kopyala' ile sonucu paylaşabilirsiniz."];
    });
}

@end

static void log_cb(void *ctx, const char *line) {
    [(__bridge VC *)ctx append:[NSString stringWithUTF8String:line]];
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
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
