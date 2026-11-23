/* A .tar.gz extractor for the root file systems the app bundles (ustar, GNU long
 * names, pax path/linkpath): regular files, directories, symlinks, hard links, with
 * their modes. Device nodes and FIFOs are skipped (the core serves /dev itself).
 * Written for this project. */
#define _DARWIN_C_SOURCE
#include "untar.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static unsigned long long oct(const char *s, size_t n)
{
    unsigned long long v = 0;
    size_t i = 0;
    if ((unsigned char)s[0] & 0x80) {                              /* base-256 (GNU, big files) */
        for (i = 1; i < n; i++) v = v << 8 | (unsigned char)s[i];
        return v;
    }
    while (i < n && (s[i] == ' ' || s[i] == 0)) i++;
    for (; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (unsigned)(s[i] - '0');
    return v;
}

static int readn(gzFile g, void *b, size_t n) { return gzread(g, b, (unsigned)n) == (int)n; }

/* mkdir -p for the parent of path. */
static void parents(char *path)
{
    char *s;
    for (s = path + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(path, 0755); *s = '/'; }
}

/* A safe relative name: no "..", no leading "/". */
static int clean(char *name)
{
    char *s = name, *d = name;
    while (*s == '/' || (s[0] == '.' && s[1] == '/')) s += *s == '/' ? 1 : 2;
    memmove(d, s, strlen(s) + 1);
    if (!strcmp(name, "..") || !strncmp(name, "../", 3) || strstr(name, "/../")) return 0;
    {
        size_t l = strlen(name);
        while (l && name[l - 1] == '/') name[--l] = 0;
    }
    return 1;
}

static void pax(const char *b, size_t n, char *path, char *link, size_t cap)
{
    size_t i = 0;
    while (i < n) {
        size_t len = 0, j = i;
        while (j < n && b[j] >= '0' && b[j] <= '9') len = len * 10 + (size_t)(b[j++] - '0');
        if (!len || i + len > n) break;
        if (j < n && b[j] == ' ') j++;
        if (!strncmp(b + j, "path=", 5)) snprintf(path, cap, "%.*s", (int)(i + len - 1 - j - 5), b + j + 5);
        else if (!strncmp(b + j, "linkpath=", 9)) snprintf(link, cap, "%.*s", (int)(i + len - 1 - j - 9), b + j + 9);
        i += len;
    }
}

const char *aoi_untar_gz(const char *archive, const char *dest, unsigned long *nfiles)
{
    gzFile g = gzopen(archive, "rb");
    char h[512], longname[4096] = "", longlink[4096] = "", name[4096], lnk[4096], out[8192], lout[8192];
    static char buf[1 << 16];
    unsigned long count = 0;
    if (!g) return "cannot open the archive";
    gzbuffer(g, 1 << 17);
    mkdir(dest, 0755);
    for (;;) {
        unsigned long long size, left;
        unsigned mode;
        char type;
        if (!readn(g, h, 512)) break;
        if (!h[0]) break;                                          /* end of archive */
        size = oct(h + 124, 12);
        mode = (unsigned)oct(h + 100, 8) & 07777;
        type = h[156];
        if (type == 'L' || type == 'K' || type == 'x' || type == 'g') {   /* metadata for the next entry */
            char *b = malloc((size_t)size + 1);
            unsigned long long pad = (512 - size % 512) % 512;
            if (!b || !readn(g, b, (size_t)size)) { free(b); gzclose(g); return "truncated archive"; }
            b[size] = 0;
            if (type == 'L') snprintf(longname, sizeof longname, "%s", b);
            else if (type == 'K') snprintf(longlink, sizeof longlink, "%s", b);
            else if (type == 'x') pax(b, (size_t)size, longname, longlink, sizeof longname);
            free(b);
            if (pad && !readn(g, buf, (size_t)pad)) { gzclose(g); return "truncated archive"; }
            continue;
        }
        if (longname[0]) snprintf(name, sizeof name, "%s", longname);
        else if (!memcmp(h + 257, "ustar", 5) && h[345]) snprintf(name, sizeof name, "%.155s/%.100s", h + 345, h);
        else snprintf(name, sizeof name, "%.100s", h);
        if (longlink[0]) snprintf(lnk, sizeof lnk, "%s", longlink);
        else snprintf(lnk, sizeof lnk, "%.100s", h + 157);
        longname[0] = longlink[0] = 0;
        left = (type == '0' || type == 0 || type == '7') ? size : 0;
        if (clean(name) && name[0]) {
            snprintf(out, sizeof out, "%s/%s", dest, name);
            parents(out);
            if (type == '5') {
                mkdir(out, 0755);
                chmod(out, mode | 0700);
            } else if (type == '2') {
                unlink(out);
                symlink(lnk, out);
            } else if (type == '1') {
                char l2[4096];
                snprintf(l2, sizeof l2, "%s", lnk);
                if (clean(l2)) {
                    snprintf(lout, sizeof lout, "%s/%s", dest, l2);
                    unlink(out);
                    if (link(lout, out)) {                        /* no hard links here: a copy */
                        int a = open(lout, O_RDONLY), b = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        ssize_t k;
                        while (a >= 0 && b >= 0 && (k = read(a, buf, sizeof buf)) > 0) if (write(b, buf, (size_t)k) != k) break;
                        if (a >= 0) close(a);
                        if (b >= 0) { fchmod(b, mode ? mode : 0644); close(b); }
                    }
                }
            } else if (type == '0' || type == 0 || type == '7') {
                int fd;
                unlink(out);
                fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                while (left) {
                    size_t k = left < sizeof buf ? (size_t)left : sizeof buf;
                    if (!readn(g, buf, k)) { if (fd >= 0) close(fd); gzclose(g); return "truncated archive"; }
                    if (fd >= 0 && write(fd, buf, k) != (ssize_t)k) { close(fd); gzclose(g); return "out of space"; }
                    left -= k;
                }
                if (fd >= 0) { fchmod(fd, mode); close(fd); }
                count++;
            }
        }
        while (left) {                                             /* skipped entry's data */
            size_t k = left < sizeof buf ? (size_t)left : sizeof buf;
            if (!readn(g, buf, k)) { gzclose(g); return "truncated archive"; }
            left -= k;
        }
        {
            unsigned long long pad = (type == '0' || type == 0 || type == '7') ? (512 - size % 512) % 512 : 0;
            if (pad && !readn(g, buf, (size_t)pad)) { gzclose(g); return "truncated archive"; }
        }
    }
    gzclose(g);
    if (nfiles) *nfiles = count;
    return NULL;
}
