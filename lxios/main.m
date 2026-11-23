/* LxTerm: a real aarch64 Linux userland (Alpine, Debian) on a non-jailbroken iPhone, in
 * LiquidAPK's no-JIT interpreter (core/: cpu, vm, proc in Linux userland mode, tty).
 * One screen: a terminal (xterm.js in a WKWebView, term.html) with a key row; the
 * root file system is bundled as a .tar.gz and unpacked into the app's container on
 * first launch.
 *
 * Launch arguments (for the simulator and measurements):
 *   -distro alpine|debian   which root to boot (default: the last one, else a chooser)
 *   -demo                   type a short demo session once the prompt is up
 *   -measure                time boot, process start, python3, a CPU loop and a
 *                           snapshot before the session; Documents/measure-<distro>.txt
 *   -resume                 resume the shell from its snapshot, if there is one */
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>

#include "../core/proc.h"
#include "../core/tty.h"
#include "untar.h"

#include <resolv.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <time.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static NSString *docs(void) { return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0]; }
static NSString *libdir(void) { return NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES)[0]; }

static FILE *mlog;                                    /* Documents/measure-<distro>.txt */
static void M(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void M(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (mlog) { vfprintf(mlog, fmt, ap); fflush(mlog); }
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

@class TermVC;
static TermVC *g_vc;
static double t_exec, t_first_out;

/* ---------- output from the guest to the view ---------- */

static NSMutableData *pending;
static pthread_mutex_t pend_mu = PTHREAD_MUTEX_INITIALIZER;
static int flush_queued;

@interface TermVC : UIViewController <WKScriptMessageHandler, WKNavigationDelegate>
@property (nonatomic, strong) WKWebView *web;
@property (nonatomic, copy) NSString *distro;
- (void)flush;
@end

static void tty_out(void *ctx, const char *s, size_t n)
{
    (void)ctx;
    if (!t_first_out) t_first_out = now_s();
    pthread_mutex_lock(&pend_mu);
    if (!pending) pending = [NSMutableData data];
    [pending appendBytes:s length:n];
    if (!flush_queued) {
        flush_queued = 1;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ [g_vc flush]; });
    }
    pthread_mutex_unlock(&pend_mu);
}

/* ---------- the guest ---------- */

static struct aoi_proc *g_proc;
static char g_root[1024], g_snap[1024];

static void write_resolv(const char *root)
{
    char path[1100];
    FILE *f;
    struct __res_state rs;
    int n = 0;
    snprintf(path, sizeof path, "%s/etc/resolv.conf", root);
    unlink(path);                                     /* (Debian's image has it as a dangling symlink) */
    if (!(f = fopen(path, "w"))) return;
    memset(&rs, 0, sizeof rs);
    if (res_ninit(&rs) == 0) {
        union res_sockaddr_union sv[4];
        int i, k = res_getservers(&rs, sv, 4);
        for (i = 0; i < k; i++) {
            char a[64];
            if (sv[i].sin.sin_family == AF_INET && inet_ntop(AF_INET, &sv[i].sin.sin_addr, a, sizeof a)) { fprintf(f, "nameserver %s\n", a); n++; }
        }
        res_ndestroy(&rs);
    }
    if (!n) fprintf(f, "nameserver 1.1.1.1\nnameserver 8.8.8.8\n");
    fclose(f);
}

static const char *const *shell_argv(NSString *distro)
{
    static const char *alp[] = { "/bin/sh", "-l", NULL };
    static const char *deb[] = { "/bin/bash", "-l", NULL };
    return [distro isEqualToString:@"debian"] ? deb : alp;
}

static const char *const *base_env(void)
{
    static const char *env[] = { "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root",
                                 "USER=root", "LOGNAME=root", "TERM=xterm-256color", "LANG=C.UTF-8", "COLORTERM=truecolor", NULL };
    return env;
}

/* Runs argv in the root to its end, without a terminal: its wall time, or -1. */
static double run_once(const char *const *argv, int *code)
{
    struct aoi_proc *p = calloc(1, sizeof *p);
    int argc = 0;
    double t0 = now_s(), t;
    enum aoi_stop st;
    while (argv[argc]) argc++;
    if (!p || aoi_proc_exec(p, g_root, argv[0], argc, argv, base_env())) { free(p); return -1; }
    aoi_proc_linux(p, "/root");
    st = aoi_proc_run(p, 0);
    t = now_s() - t0;
    if (code) *code = st == AOI_STOP_EXIT ? p->cpu.exit_code : -1;
    aoi_proc_free(p);
    free(p);
    return t;
}

static void measure(NSString *distro)
{
    int code = 0;
    double t;
    const char *py_start[] = { "/usr/bin/python3", "-c", "pass", NULL };
    const char *py_loop[] = { "/usr/bin/python3", "-c",
        "import time\nt=time.time()\ns=0\nfor i in range(1000000): s+=i\nprint(time.time()-t)", NULL };
    const char *sh_loop[] = { "/bin/sh", "-c", "i=0; while [ $i -lt 100000 ]; do i=$((i+1)); done", NULL };
    const char *sh_true[] = { "/bin/sh", "-c", "true", NULL };
    const char *sha[] = { "/bin/sh", "-c", "dd if=/dev/zero bs=1M count=16 2>/dev/null | sha256sum", NULL };
    t = run_once(sh_true, &code); M("process start (sh -c true): %.3f s (exit %d)\n", t, code);
    t = run_once(py_start, &code); M("python3 -c pass: %.3f s (exit %d)\n", t, code);
    t = run_once(py_loop, &code); M("python3 1M-iteration loop (wall, incl. start): %.3f s (exit %d)\n", t, code);
    t = run_once(sh_loop, &code); M("sh while-loop 100k: %.3f s (exit %d)\n", t, code);
    t = run_once(sha, &code); M("sha256sum of 16 MiB: %.3f s (exit %d)\n", t, code);
    (void)distro;
}

/* The demo session: typed as a user would, once the prompt is up. */
static void *demo(void *arg)
{
    NSString *distro = (__bridge_transfer NSString *)arg;
    int deb = [distro isEqualToString:@"debian"];
    const char *alp_cmds[] = { "cat /etc/os-release | head -n 2\r", "uname -a\r", "apk --version\r",
                               "time python3 -c 'print(sum(range(10**6)))'\r", "ls /\r", NULL };
    const char *deb_cmds[] = { "head -n 2 /etc/os-release\r", "uname -a\r", "dpkg -l | tail -n 3\r",
                               "time python3 -c 'print(sum(range(10**6)))'\r", "apt list --installed 2>/dev/null | wc -l\r", NULL };
    const char *const *c = deb ? deb_cmds : alp_cmds;
    int i;
    for (i = 0; i < 100 && !t_first_out; i++) usleep(100000);
    usleep(800000);
    for (; *c; c++) {
        size_t k;
        for (k = 0; (*c)[k]; k++) { aoi_tty_input(aoi_console, *c + k, 1); usleep(25000); }
        sleep(2);
    }
    if (g_snap[0] && g_proc) {                         /* save the idle shell: the next launch resumes it */
        snprintf(g_proc->snap_path, sizeof g_proc->snap_path, "%s", g_snap);
        g_proc->snap_request = 1;
        aoi_tty_input(aoi_console, "\r", 1);           /* (a slice runs: the save happens) */
    }
    return NULL;
}

static void *boot(void *arg)
{
    NSString *distro = (__bridge_transfer NSString *)arg;
    NSArray *args = [NSProcessInfo processInfo].arguments;
    NSString *rootDir = [libdir() stringByAppendingPathComponent:[@"rootfs/" stringByAppendingString:distro]];
    NSString *ok = [rootDir stringByAppendingPathComponent:@".lxterm-ok"];
    NSString *tarball = [[NSBundle mainBundle] pathForResource:[@"rootfs-" stringByAppendingString:distro] ofType:@"tar.gz"];
    const char *err;
    double t0;
    struct rlimit rl;

    if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur < 4096) { rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096; setrlimit(RLIMIT_NOFILE, &rl); }
    mlog = fopen([[docs() stringByAppendingPathComponent:[NSString stringWithFormat:@"measure-%@.txt", distro]] fileSystemRepresentation], "a");
    M("--- %s %s, %s\n", [distro UTF8String], [[[NSDate date] description] UTF8String],
      [[[UIDevice currentDevice] model] UTF8String]);
    [[NSFileManager defaultManager] createDirectoryAtPath:rootDir withIntermediateDirectories:YES attributes:nil error:nil];
    snprintf(g_root, sizeof g_root, "%s", [rootDir fileSystemRepresentation]);
    snprintf(g_snap, sizeof g_snap, "%s", [[docs() stringByAppendingPathComponent:[NSString stringWithFormat:@"shell-%@.snap", distro]] fileSystemRepresentation]);
    if (![[NSFileManager defaultManager] fileExistsAtPath:ok]) {
        unsigned long n = 0;
        char msg[200];
        snprintf(msg, sizeof msg, "\r\nUnpacking %s...\r\n", [distro UTF8String]);
        tty_out(NULL, msg, strlen(msg));
        t0 = now_s();
        if (!tarball || (err = aoi_untar_gz([tarball fileSystemRepresentation], g_root, &n))) {
            snprintf(msg, sizeof msg, "cannot unpack the root: %s\r\n", tarball ? err : "not bundled");
            tty_out(NULL, msg, strlen(msg));
            return NULL;
        }
        [[NSFileManager defaultManager] createFileAtPath:ok contents:nil attributes:nil];
        M("unpack: %lu files in %.2f s\n", n, now_s() - t0);
        t_first_out = 0;
    }
    write_resolv(g_root);
    if ([args containsObject:@"-measure"]) measure(distro);

    for (;;) {
        struct aoi_proc *p = calloc(1, sizeof *p);
        const char *const *argv = shell_argv(distro);
        int resumed = 0;
        g_proc = p;
        t_first_out = 0;
        if (!aoi_console) aoi_tty_new(tty_out, NULL, 80, 24);
        if ([args containsObject:@"-resume"] && !access(g_snap, R_OK)) {
            t0 = now_s();
            if (!(err = aoi_snap_load(p, g_snap, g_root, NULL))) {
                resumed = 1;
                M("snapshot resume: %.3f s\n", now_s() - t0);
                aoi_tty_input(aoi_console, "\r", 1);          /* a fresh prompt */
            } else M("snapshot resume failed: %s\n", err);
        }
        if (!resumed) {
            memset(p, 0, sizeof *p);
            t_exec = now_s();
            if ((err = aoi_proc_exec(p, g_root, argv[0], 2, argv, base_env()))) {
                char msg[300];
                snprintf(msg, sizeof msg, "cannot start %s: %s\r\n", argv[0], err);
                tty_out(NULL, msg, strlen(msg));
                return NULL;
            }
            aoi_proc_linux(p, "/root");
            aoi_proc_attach_tty(p);
            {
                char lp[1100];
                snprintf(lp, sizeof lp, "%s/lxterm-%s.log", [docs() fileSystemRepresentation], [distro UTF8String]);
                p->log = fopen(lp, "a");
            }
            if ([args containsObject:@"-demo"] || [args containsObject:@"-measure"]) {
                pthread_t th;
                pthread_create(&th, NULL, demo, (__bridge_retained void *)distro);
                pthread_detach(th);
            }
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{
                if (t_first_out) M("boot to prompt: %.3f s\n", t_first_out - t_exec);
            });
        }
        aoi_proc_run(p, 0);
        {
            const char *m = "\r\n[the shell exited: starting a new one]\r\n";
            tty_out(NULL, m, strlen(m));
        }
        if (p->log) fclose(p->log);
        p->log = NULL;
        aoi_proc_free(p);
        g_proc = NULL;
        free(p);
        sleep(1);
    }
    return NULL;
}

/* ---------- the view ---------- */

@implementation TermVC

- (void)viewDidLoad
{
    [super viewDidLoad];
    WKWebViewConfiguration *cfg = [WKWebViewConfiguration new];
    [cfg.userContentController addScriptMessageHandler:self name:@"lx"];
    self.view.backgroundColor = [UIColor colorWithRed:0.063 green:0.063 blue:0.078 alpha:1];
    self.web = [[WKWebView alloc] initWithFrame:self.view.bounds configuration:cfg];
    self.web.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.web.opaque = NO;
    self.web.backgroundColor = self.view.backgroundColor;
    self.web.scrollView.scrollEnabled = NO;
    self.web.scrollView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
    [self.view addSubview:self.web];
    NSURL *u = [[NSBundle mainBundle] URLForResource:@"term" withExtension:@"html"];
    [self.web loadFileURL:u allowingReadAccessToURL:[u URLByDeletingLastPathComponent]];
}

- (void)flush
{
    NSData *d;
    pthread_mutex_lock(&pend_mu);
    d = pending;
    pending = nil;
    flush_queued = 0;
    pthread_mutex_unlock(&pend_mu);
    if (!d.length) return;
    [self.web evaluateJavaScript:[NSString stringWithFormat:@"lxWrite('%@')", [d base64EncodedStringWithOptions:0]]
               completionHandler:nil];
}

- (void)userContentController:(WKUserContentController *)uc didReceiveScriptMessage:(WKScriptMessage *)msg
{
    NSDictionary *m = msg.body;
    NSString *t = m[@"t"];
    if ([t isEqualToString:@"in"]) {
        NSData *d = [m[@"d"] dataUsingEncoding:NSUTF8StringEncoding];
        if (aoi_console && d.length) aoi_tty_input(aoi_console, d.bytes, d.length);
    } else if ([t isEqualToString:@"size"]) {
        if (aoi_console) aoi_tty_resize(aoi_console, [m[@"c"] intValue], [m[@"r"] intValue]);
    } else if ([t isEqualToString:@"ready"]) {
        static int started;
        if (started++) return;
        aoi_tty_new(tty_out, NULL, [m[@"c"] intValue], [m[@"r"] intValue]);
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 8u << 20);
        pthread_create(&th, &at, boot, (__bridge_retained void *)self.distro);
        pthread_attr_destroy(&at);
        pthread_detach(th);
    }
}

- (BOOL)prefersStatusBarHidden { return NO; }
- (UIStatusBarStyle)preferredStatusBarStyle { return UIStatusBarStyleLightContent; }
@end

/* ---------- the chooser ---------- */

@interface ChooseVC : UIViewController
@end

@implementation ChooseVC
- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:0.063 green:0.063 blue:0.078 alpha:1];
    UIStackView *st = [UIStackView new];
    st.axis = UILayoutConstraintAxisVertical;
    st.spacing = 16;
    st.translatesAutoresizingMaskIntoConstraints = NO;
    UILabel *title = [UILabel new];
    title.text = @"Linux on iPhone";
    title.textColor = UIColor.whiteColor;
    title.font = [UIFont boldSystemFontOfSize:28];
    title.textAlignment = NSTextAlignmentCenter;
    [st addArrangedSubview:title];
    UILabel *sub = [UILabel new];
    sub.text = @"aarch64 userland in a no-JIT interpreter";
    sub.textColor = UIColor.lightGrayColor;
    sub.textAlignment = NSTextAlignmentCenter;
    [st addArrangedSubview:sub];
    for (NSString *d in @[ @"alpine", @"debian" ]) {
        if (![[NSBundle mainBundle] pathForResource:[@"rootfs-" stringByAppendingString:d] ofType:@"tar.gz"]) continue;
        UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
        [b setTitle:[d isEqualToString:@"alpine"] ? @"Alpine Linux (musl, apk)" : @"Debian (glibc, apt)" forState:UIControlStateNormal];
        b.titleLabel.font = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold];
        b.backgroundColor = [UIColor colorWithWhite:0.2 alpha:1];
        b.layer.cornerRadius = 12;
        [b.heightAnchor constraintEqualToConstant:56].active = YES;
        b.accessibilityIdentifier = d;
        [b addTarget:self action:@selector(pick:) forControlEvents:UIControlEventTouchUpInside];
        [st addArrangedSubview:b];
    }
    [self.view addSubview:st];
    [NSLayoutConstraint activateConstraints:@[
        [st.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [st.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor constant:32],
        [st.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor constant:-32] ]];
}
- (void)pick:(UIButton *)b
{
    TermVC *vc = [TermVC new];
    vc.distro = b.accessibilityIdentifier;
    g_vc = vc;
    [[NSUserDefaults standardUserDefaults] setObject:vc.distro forKey:@"distro"];
    self.view.window.rootViewController = vc;
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts
{
    NSArray *args = [NSProcessInfo processInfo].arguments;
    NSUInteger i = [args indexOfObject:@"-distro"];
    NSString *distro = i != NSNotFound && i + 1 < args.count ? args[i + 1] : nil;
    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    if (distro) {
        TermVC *vc = [TermVC new];
        vc.distro = distro;
        g_vc = vc;
        self.window.rootViewController = vc;
    } else self.window.rootViewController = [ChooseVC new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char *argv[])
{
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class])); }
}
