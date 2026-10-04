/* libaoi_media.so: Android's software codecs in the app's own process. On a phone
 * they live in the media.swcodec process, whose main() calls RegisterCodecServices()
 * (libmedia_codecserviceregistrant.so): the Codec2 component store registers itself
 * with hwservicemanager as "software". Here there is one process, so aoi.Main loads
 * this library (System.load) and its JNI_OnLoad makes the same call; the store is
 * then a local object of the app, found through hwservicemanager (core/binder.c).
 * Guest code; dlopen/dlsym/__android_log_print come from libdl and liblog. */
#define EXPORT __attribute__((visibility("default")))

void *dlopen(const char *path, int flags);
void *dlsym(void *handle, const char *name);
char *dlerror(void);
int __android_log_print(int prio, const char *tag, const char *fmt, ...);

#define SWCODEC "libmedia_codecserviceregistrant.so"     /* by name: through the default namespace (tools/swcodec-ns.py) */

EXPORT int JNI_OnLoad(void *vm, void *reserved)
{
    void *h = dlopen(SWCODEC, 2);                       /* RTLD_NOW */
    void (*reg)(void);
    (void)vm; (void)reserved;
    if (!h) { __android_log_print(6, "aoi", "codecs: %s", dlerror()); return 0x00010006; }
    reg = (void (*)(void))dlsym(h, "RegisterCodecServices");
    if (!reg) { __android_log_print(6, "aoi", "codecs: no RegisterCodecServices"); return 0x00010006; }
    reg();
    __android_log_print(4, "aoi", "codecs: the software codecs are registered");
    return 0x00010006;                                  /* JNI_VERSION_1_6 */
}
