// apk-on-iphone test app: pick an APK and run its libgmp.so in the interpreter
// (no JIT, no debugger: works on any iPhone). Built without an Xcode project
// by .github/workflows/ios.yml.
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <sys/sysctl.h>

#include "../core/apk.h"
#include "gmptest.h"

@interface VC : UIViewController <UIDocumentPickerDelegate>
@property(nonatomic, strong) UITextView *logView;
@property(nonatomic, strong) UITextField *nField;
@property(nonatomic, strong) NSData *apk;
@property(nonatomic, strong) NSMutableString *log;
@property(nonatomic) dispatch_queue_t work;
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

    UIStackView *row1 = [self row:@[ [self button:@"APK seç" action:@selector(pick)], self.nField ]];
    UIStackView *row2 = [self row:@[ [self button:@"Çalıştır" action:@selector(run)],
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
    [self append:@"Qalculate APK'sını seçin, sonra 'Çalıştır'."];
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
    UIPasteboard.generalPasteboard.string = self.log;
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
}

static int list_cb(const char *name, size_t len, void *ctx) {
    NSString *s = [[NSString alloc] initWithBytes:name length:len encoding:NSUTF8StringEncoding];
    if ([s hasPrefix:@"lib/arm64-v8a/"] && [s hasSuffix:@".so"])
        [(__bridge NSMutableString *)ctx appendFormat:@"    %@\n", [s substringFromIndex:14]];
    return 0;
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
        double t = 0;
        [self append:[NSString stringWithFormat:@"Yorumlayıcı: Android libgmp.so ile %lu! ...", n]];
        char *r = aoi_gmp_interp(lib.bytes, lib.length, n, &t, log_cb, (__bridge void *)self);
        if (!r) { [self append:@"başarısız"]; return; }
        [self append:[NSString stringWithFormat:@"%lu! = %.20s… (%zu basamak), %.3f s", n, r, strlen(r), t]];
        free(r);
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
