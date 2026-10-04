/* libaoi_media.so: Android's software codecs and MediaPlayer's service in the app's
 * own process. On a phone the codecs live in the media.swcodec process, whose main()
 * calls RegisterCodecServices() (libmedia_codecserviceregistrant.so): the Codec2
 * component store registers itself as "android.hardware.media.c2.IComponentStore/
 * software". MediaPlayer's service lives in mediaserver, whose main() calls
 * MediaPlayerService::instantiate(): it registers "media.player". Here there is one
 * process, so aoi.Main loads this library (System.load) and its JNI_OnLoad makes both
 * calls; the services are then local objects of the app.
 *
 * The libraries are loaded into the system's ("default") namespace, not the app's
 * class-loader namespace this library is in: through the linker's own
 * __loader_android_dlopen_ext with that namespace (ANDROID_DLEXT_USE_NAMESPACE), so
 * their dependencies resolve as in a platform process. Guest code; dlsym and
 * __android_log_print come from libdl and liblog. */
#define EXPORT __attribute__((visibility("default")))

void *dlopen(const char *path, int flags);
void *dlsym(void *handle, const char *name);
char *dlerror(void);
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
int pthread_create(void *thread, const void *attr, void *(*fn)(void *), void *arg);
int pthread_detach(unsigned long thread);
int prctl(int option, ...);

struct dlextinfo {                                      /* android_dlextinfo */
    unsigned long flags;
    void *reserved_addr;
    unsigned long reserved_size;
    int relro_fd, library_fd;
    long library_fd_offset;
    void *library_namespace;
};
#define DLEXT_USE_NAMESPACE 0x200

static void *(*ns_dlopen)(const char *, int, const struct dlextinfo *, const void *);
static struct dlextinfo ext;

/* A platform library and one of its functions, from the default namespace. */
static void *platform(const char *lib, const char *fn)
{
    void *h;
    if (!ns_dlopen) return 0;
    if (!(h = ns_dlopen(lib, 2, &ext, (const void *)platform))) {   /* RTLD_NOW */
        __android_log_print(6, "aoi", "media: %s: %s", lib, dlerror());
        return 0;
    }
    return dlsym(h, fn);
}

/* The services' own start, as their processes' main() functions make it, on a native
 * thread of its own (named for the binder driver, which gives it no app calls: it has
 * no Java). Native, so the services it makes reach the app's Java services as relays
 * (core/binder.c), as they would from another process. Registering the codec store
 * comes last: it does not return (it serves the binder thread pool, as main() would). */
static void *start(void *unused)
{
    void *f;
    (void)unused;
    prctl(15, "aoi-codecs", 0, 0, 0);                   /* PR_SET_NAME */
    /* MediaPlayer's service (NuPlayer): it registers as "media.player" (replacing
     * aoi.MediaPlayerService) and plays with these codecs, our AudioFlinger and, for
     * video, our SurfaceFlinger */
    if ((f = platform("libmediaplayerservice.so", "_ZN7android18MediaPlayerService11instantiateEv")))
        ((void (*)(void))f)();
    /* the extractors (MP4, MP3, Ogg, WAV, MKV...), which media.extractor's service loads
     * when it starts: MediaPlayer and MediaExtractor use them in this process */
    if ((f = platform("libstagefright.so", "_ZN7android21MediaExtractorFactory14LoadExtractorsEv")))
        ((void (*)(void))f)();
    if ((f = platform("libmedia_codecserviceregistrant.so", "RegisterCodecServices")))
        ((void (*)(void))f)();
    return 0;
}

EXPORT int JNI_OnLoad(void *vm, void *reserved)
{
    void *ld = dlopen("libdl.so", 2);                 /* its dependency ld-android.so: the linker's own calls */
    void *(*get_ns)(const char *) = (void *(*)(const char *))dlsym(ld, "__loader_android_get_exported_namespace");
    unsigned long th;
    (void)vm; (void)reserved;
    ns_dlopen = (void *(*)(const char *, int, const struct dlextinfo *, const void *))dlsym(ld, "__loader_android_dlopen_ext");
    if (!get_ns || !ns_dlopen || !(ext.library_namespace = get_ns("default"))) {
        __android_log_print(6, "aoi", "media: no access to the system namespace");
        return 0x00010006;
    }
    ext.flags = DLEXT_USE_NAMESPACE;
    if (pthread_create(&th, 0, start, 0)) __android_log_print(6, "aoi", "media: no thread");
    else pthread_detach(th);
    return 0x00010006;                                  /* JNI_VERSION_1_6 */
}
