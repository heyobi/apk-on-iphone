#ifndef AOI_UNTAR_H
#define AOI_UNTAR_H
/* Unpacks archive (a .tar.gz) into dest. NULL, or what went wrong; *nfiles: regular files. */
const char *aoi_untar_gz(const char *archive, const char *dest, unsigned long *nfiles);
#endif
