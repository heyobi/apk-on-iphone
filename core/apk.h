/* Reading files out of an APK (a zip archive): stored and deflated entries. */
#ifndef AOI_APK_H
#define AOI_APK_H

#include <stddef.h>

/* Extracts entry `name` (e.g. "lib/arm64-v8a/libgmp.so") from the zip in
 * [zip, zip+size). Returns a malloc'ed buffer (caller frees) and its size, or
 * NULL with *err set. Needs zlib for deflated entries. */
void *aoi_apk_extract(const void *zip, size_t size, const char *name, size_t *out_size, const char **err);

/* Calls fn for each entry name; stops early if fn returns nonzero. */
void aoi_apk_list(const void *zip, size_t size, int (*fn)(const char *name, size_t len, void *ctx), void *ctx);

/* Writes the APK's compressed arm64 native libraries (lib/arm64-v8a/NAME.so, not
 * stored) to dir/NAME.so, as the package manager does at install
 * (extractNativeLibs): the linker can load a stored library from inside the APK,
 * not a deflated one. The number written, or -1 with *err set. */
int aoi_apk_extract_libs(const void *zip, size_t size, const char *dir, const char **err);

/* The uncompressed size of the APK's dex code (classes*.dex at its top). */
size_t aoi_apk_dex_bytes(const void *zip, size_t size);

/* The package name and application label from AndroidManifest.xml (binary XML). A
 * label that is a resource reference is not resolved: the package's last part, capitalised,
 * stands in. 0, or -1 if there is no manifest or package. */
int aoi_apk_manifest(const void *zip, size_t size, char *pkg, size_t pkgn, char *label, size_t labeln);

#endif
