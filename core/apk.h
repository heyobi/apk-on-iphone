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

/* The package name and application label from AndroidManifest.xml (binary XML). A
 * label that is a resource reference is not resolved: the package's last part, capitalised,
 * stands in. 0, or -1 if there is no manifest or package. */
int aoi_apk_manifest(const void *zip, size_t size, char *pkg, size_t pkgn, char *label, size_t labeln);

#endif
