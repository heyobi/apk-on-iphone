/* iostest: run the iOS app's test sequence on the host. usage: iostest app.apk [n]
 * AOI_ANDROID_ROOT=root AOI_APP_DATA=dir: the app button instead (dir is the guest's /data). */
#include "../core/apk.h"
#include "../ios/gmptest.h"
#include "../ios/vmprobe.h"
#include "../ios/androidtest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void out(void *ctx, const char *line) { (void)ctx; printf("%s\n", line); }

/* The app button's frames: the newest one as a PPM file (ctx: its path). */
static void frame(void *ctx, const unsigned char *px, unsigned w, unsigned h)
{
    FILE *f = fopen(ctx, "wb");
    size_t i;
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (i = 0; i < (size_t)w * h; i++) fwrite(px + 4 * i, 1, 3, f);
    fclose(f);
    printf("frame %ux%u -> %s\n", w, h, (const char *)ctx);
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

    if (getenv("AOI_ANDROID_ROOT") && getenv("AOI_APP_DATA")) {   /* the app's "Uygulama" button */
        const char *png = getenv("AOI_APP_FRAME") ? getenv("AOI_APP_FRAME") : "/tmp/aoi-frame.ppm";
        return aoi_android_app(getenv("AOI_ANDROID_ROOT"), getenv("AOI_APP_DATA"), "/tmp/aoi-app.log", frame,
                               (void *)png, out, NULL) == 0 ? 0 : 1;
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
