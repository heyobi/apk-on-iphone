// What godot-angle-static's iOS libraries leave out and ANGLE still refers to: two
// of angle/src/common/system_utils_apple.cpp's functions (built there for macOS
// only), and the ASTC decoder (astc-encoder, which Godot links itself). Without a
// decoder ANGLE reports ASTC software decompression as unavailable; Metal on Apple
// GPUs decodes ASTC itself.
#include <mach/mach_time.h>
#include <pthread.h>
#include <stddef.h>

namespace angle {
double GetCurrentSystemTime()
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

void SetCurrentThreadName(const char *name) { pthread_setname_np(name); }
}

// astc-encoder's API (Source/astcenc.h), as ANGLE's AstcDecompressor.cpp calls it.
enum astcenc_error { ASTCENC_SUCCESS = 0, ASTCENC_ERR_BAD_CPU_FLOAT = 3 };
enum astcenc_profile { ASTCENC_PRF_LDR = 1 };
struct astcenc_config;
struct astcenc_context;
struct astcenc_image;
struct astcenc_swizzle;

astcenc_error astcenc_config_init(astcenc_profile, unsigned int, unsigned int, unsigned int, float, unsigned int,
                                  astcenc_config *)
{
    return ASTCENC_ERR_BAD_CPU_FLOAT;
}
astcenc_error astcenc_context_alloc(const astcenc_config *, unsigned int, astcenc_context **c)
{
    if (c) *c = nullptr;
    return ASTCENC_ERR_BAD_CPU_FLOAT;
}
astcenc_error astcenc_decompress_image(astcenc_context *, const unsigned char *, size_t, astcenc_image *,
                                       const astcenc_swizzle *, unsigned int)
{
    return ASTCENC_ERR_BAD_CPU_FLOAT;
}
astcenc_error astcenc_decompress_reset(astcenc_context *) { return ASTCENC_ERR_BAD_CPU_FLOAT; }
void astcenc_context_free(astcenc_context *) {}
const char *astcenc_get_error_string(astcenc_error) { return "no ASTC decoder in this build"; }
