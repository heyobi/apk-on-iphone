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

/* AOI_APP_TAPS="x,y;x,y;...": taps in screen pixels, 4 s apart, after the first frame
 * ("back": Android's back; "type:TEXT": typed, then return);
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
        if (!strncmp(s, "type:", 5)) {                 /* "type:TEXT" (to the next ';'): typed on the */
            const char *e = strchr(s + 5, ';');        /* host keyboard, then return */
            size_t k, len = e ? (size_t)(e - s - 5) : strlen(s + 5);
            sleep(4);
            printf("type %.*s\n", (int)len, s + 5);
            fflush(stdout);
            for (k = 0; k < len; k++) {
                aoi_android_key(6, (unsigned char)s[5 + k]);
                { struct timespec ts = { 0, 50000000 }; nanosleep(&ts, NULL); }
            }
            aoi_android_key(8, 0);
            s += 5 + len;
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

static void *stop_after(void *arg)
{
    sleep((unsigned)atoi(arg));
    aoi_android_stop();                             /* the app stops: where each thread was goes to the log */
    return NULL;
}

static void *compile_let_go(void *arg)
{
    sleep((unsigned)atoi(arg));
    aoi_android_compile_hold(0);
    return NULL;
}

static void *show_after(void *png)
{
    sleep((unsigned)atoi(getenv("AOI_APP_SHOW_AFTER")));
    printf("show: %d\n", aoi_android_show(frame, home, png));
    fflush(stdout);
    return NULL;
}

struct warm_arg { const char *dir, *png, *log; int secs; };

static void *warm_go_after(void *arg)
{
    struct warm_arg *w = arg;
    sleep((unsigned)w->secs);
    printf("warm: go %d\n", aoi_android_go(getenv("AOI_ANDROID_ROOT"), getenv("AOI_APP_DATA"), w->log,
                                           getenv("AOI_APP_DISPLAY"), frame, home, (void *)w->png, out, NULL));
    fflush(stdout);
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
        int rc;
        if (getenv("AOI_APP_COMPILE")) {            /* the card's "Derle" first: its progress, then the app */
            char dlog[1100];
            struct aoi_compile_info ci;
            snprintf(dlog, sizeof dlog, "%s.dex2oat", log);
            rc = aoi_android_compile(getenv("AOI_ANDROID_ROOT"), getenv("AOI_APP_DATA"), dlog,
                                     !strcmp(getenv("AOI_APP_COMPILE"), "faster"), out, NULL);
            printf("compile: %d\n", rc);
            while (aoi_android_compiling()) {
                sleep(5);
                aoi_android_compile_info(&ci);
                if (ci.active) printf("compile: %s %.0f%% after %.0f s, %.0f s to go%s\n", ci.filter, ci.progress * 100,
                                      ci.elapsed, ci.eta, ci.held ? " (held)" : "");
                fflush(stdout);
            }
            {
                char st[32];
                aoi_android_compile_state(getenv("AOI_APP_DATA"), st, sizeof st);
                printf("compile: state \"%s\"\n", st);
            }
        }
        if (getenv("AOI_APP_STOP_AFTER")) {         /* stop the app N s after it starts (after a */
            pthread_t t;                            /* compile): a hang shows its threads */
            pthread_create(&t, NULL, stop_after, getenv("AOI_APP_STOP_AFTER"));
        }
        if (getenv("AOI_APP_HIDDEN")) {            /* out of sight until saved (after a compile); */
            if (getenv("AOI_APP_SHOW_AFTER")) {     /* shown after N s (a tap meanwhile) */
                pthread_t t;
                pthread_create(&t, NULL, show_after, (void *)png);
            }
            rc = aoi_android_app_hidden(getenv("AOI_ANDROID_ROOT"), getenv("AOI_APP_DATA"), log,
                                        getenv("AOI_APP_DISPLAY"), out, NULL);
            printf("hidden: %d\n", rc);
            return rc == 1 ? 0 : 1;
        }
        if (getenv("AOI_APP_WARM")) {              /* "dir N": Android up in dir first, the app N s later */
            static char wdir[1024];
            static struct warm_arg wa;
            pthread_t t;
            int secs = 0;
            sscanf(getenv("AOI_APP_WARM"), "%1023s %d", wdir, &secs);
            wa.dir = wdir; wa.secs = secs; wa.png = png; wa.log = log;
            pthread_create(&t, NULL, warm_go_after, &wa);
            rc = aoi_android_warm(getenv("AOI_ANDROID_ROOT"), wdir, getenv("AOI_APP_DISPLAY"), out, NULL);
            printf("warm: %d\n", rc);
            return rc < 0 && rc != -2 ? 1 : 0;
        }
        rc = aoi_android_app(getenv("AOI_ANDROID_ROOT"), getenv("AOI_APP_DATA"), log,
                             getenv("AOI_APP_DISPLAY"), frame, home, (void *)png, out, NULL) == 0 ? 0 : 1;
        while (aoi_android_compiling()) sleep(1);  /* one started meanwhile finishes */
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
