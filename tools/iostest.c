#define _POSIX_C_SOURCE 200809L
/* iostest: run the iOS app's test sequence on the host. usage: iostest app.apk [n]
 * AOI_ANDROID_ROOT=root AOI_APP_DATA=dir: the app button instead (dir is the guest's /data;
 * tools/app-install.sh makes one; AOI_APP_LOG: the app's log, /tmp/aoi-app.log). */
#include "../core/apk.h"
#include "../ios/gmptest.h"
#include "../ios/vmprobe.h"
#include "../ios/androidtest.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void out(void *ctx, const char *line) { (void)ctx; printf("%s\n", line); }

/* AOI_APP_TAPS="x,y;x,y;...": taps in screen pixels, 4 s apart, after the first frame;
 * then with AOI_APP_SNAPSHOT=1 a snapshot (aoi_android_snapshot). */
static void *taps(void *arg)
{
    const char *s = arg;
    float x, y;
    int n;
    for (;;) {
        if (!strncmp(s, "back", 4)) {                   /* "back": Android's back (aoi_android_back) */
            sleep(4);
            printf("back\n");
            fflush(stdout);
            aoi_android_back();
            s += 4;
            if (*s == ';') s++;
            continue;
        }
        if (sscanf(s, "%f,%f%n", &x, &y, &n) != 2) break;
        sleep(4);
        printf("tap %.0f,%.0f\n", x, y);
        fflush(stdout);
        aoi_android_touch(0, x, y);
        { struct timespec ts = { 0, 120000000 }; nanosleep(&ts, NULL); }   /* a finger stays ~0.1 s */
        aoi_android_touch(1, x, y);
        s += n;
        if (*s == ';') s++;
    }
    if (getenv("AOI_APP_SNAPSHOT")) {               /* then as when iOS sends the app to the background */
        sleep(4);
        printf("snapshot: %d\n", aoi_android_snapshot(25));
        fflush(stdout);
    }
    return NULL;
}

/* The app button's frames: the newest one as ctx.ppm (AOI_APP_ALL_FRAMES: each as ctx.N.ppm). */
static void frame(void *ctx, const unsigned char *px, unsigned w, unsigned h)
{
    static int frames;
    char path[600];
    FILE *f;
    size_t i;
    if (frames++ == 0 && getenv("AOI_APP_TAPS")) {
        pthread_t t;
        pthread_create(&t, NULL, taps, getenv("AOI_APP_TAPS"));
        pthread_detach(t);
    }
    if (getenv("AOI_APP_ALL_FRAMES"))                                      /* every frame kept */
        snprintf(path, sizeof path, "%s.%d.ppm", (const char *)ctx, frames);
    else                                                                    /* the newest only */
        snprintf(path, sizeof path, "%s.ppm", (const char *)ctx);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (i = 0; i < (size_t)w * h; i++) fwrite(px + 4 * i, 1, 3, f);
    fclose(f);
    if (getenv("AOI_APP_ALL_FRAMES") || frames == 1 || frames % 100 == 0) {
        printf("frame %d %ux%u -> %s\n", frames, w, h, path);
        fflush(stdout);
    }
}

/* The app left for the launcher (AOI_APP_DISPLAY="w h dpi" sets its display). */
static void home(void *ctx)
{
    (void)ctx;
    printf("home\n");
    fflush(stdout);
}

static void *compile_let_go(void *arg)
{
    sleep((unsigned)atoi(arg));
    aoi_android_compile_hold(0);
    return NULL;
}

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    size_t sz = 0;
    unsigned long fac = argc > 2 ? strtoul(argv[2], NULL, 10) : 1000;
    const char *err = NULL;
    void *apk, *so;
    char *r;
    double t = 0;

    if (getenv("AOI_COMPILE_HOLD")) {               /* as when the phone is hot: dex2oat waits */
        pthread_t t;                                /* for the first N s */
        aoi_android_compile_hold(1);
        pthread_create(&t, NULL, compile_let_go, getenv("AOI_COMPILE_HOLD"));
    }
    if (getenv("AOI_ANDROID_ROOT") && getenv("AOI_APP_DATA")) {   /* the app's "Uygulama" button */
        const char *png = getenv("AOI_APP_FRAME") ? getenv("AOI_APP_FRAME") : "/tmp/aoi-frame.ppm";
        const char *log = getenv("AOI_APP_LOG") ? getenv("AOI_APP_LOG") : "/tmp/aoi-app.log";
        int rc = aoi_android_app(getenv("AOI_ANDROID_ROOT"), getenv("AOI_APP_DATA"), log,
                                 getenv("AOI_APP_DISPLAY"), frame, home, (void *)png, out, NULL) == 0 ? 0 : 1;
        while (aoi_android_compiling()) sleep(1);  /* a background dex2oat finishes (the phone's keeps running) */
        return rc;
    }
    aoi_vm_probe(out, NULL);
    if (getenv("AOI_ANDROID_ROOT")) {               /* the app's "Android" button */
        static const char *const echo[] = { "/system/bin/toybox", "echo", "merhaba, ben Android toybox" };
        static const char *const sh[] = { "/system/bin/sh", "-c", "echo mksh: $((6*7)); x=Android; echo ${#x} harf" };
        aoi_android_run(getenv("AOI_ANDROID_ROOT"), "/tmp", 3, echo, out, NULL);
        aoi_android_run(getenv("AOI_ANDROID_ROOT"), "/tmp", 3, sh, out, NULL);
        aoi_android_art_hello(getenv("AOI_ANDROID_ROOT"), "/tmp", out, NULL);
        aoi_android_art_gc(getenv("AOI_ANDROID_ROOT"), "/tmp", out, NULL);
    }
    if (argc < 2) { fprintf(stderr, "usage: %s app.apk [n]\n", argv[0]); return 2; }
    if (!(f = fopen(argv[1], "rb")) || fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) ||
        !(apk = malloc((size_t)n)) || fread(apk, 1, (size_t)n, f) != (size_t)n) { perror(argv[1]); return 1; }
    fclose(f);
    if (!(so = aoi_apk_extract(apk, (size_t)n, "lib/arm64-v8a/libgmp.so", &sz, &err))) { fprintf(stderr, "%s\n", err); return 1; }
    printf("libgmp.so: %zu bytes from the APK\n", sz);
    if ((r = aoi_gmp_interp(so, sz, fac, &t, out, NULL))) printf("interp: %lu! has %zu digits, %.3f s\n", fac, strlen(r), t);
    return r ? 0 : 1;
}
