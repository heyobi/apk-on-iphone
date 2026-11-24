/* The Linux userland mode's terminal: a pty slave with Linux's line discipline.
 * See tty.h. Written for this project; the flag values are Linux's ABI. */
#include "tty.h"
#include "proc.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* c_iflag */
#define IGNCR_  0200
#define ICRNL_  0400
#define INLCR_  0100
#define ISTRIP_ 040
#define IUTF8_  040000
/* c_oflag */
#define OPOST_  1
#define ONLCR_  4
#define OCRNL_  010
/* c_lflag */
#define ISIG_   1
#define ICANON_ 2
#define ECHO_   010
#define ECHOE_  020
#define ECHOK_  040
#define ECHONL_ 0100
#define NOFLSH_ 0200
#define ECHOCTL_ 01000
#define ECHOKE_ 04000
#define IEXTEN_ 0100000
/* c_cc */
enum { VINTR_ = 0, VQUIT_ = 1, VERASE_ = 2, VKILL_ = 3, VEOF_ = 4, VTIME_ = 5, VMIN_ = 6, VSTART_ = 8,
       VSTOP_ = 9, VSUSP_ = 10, VEOL_ = 11, VREPRINT_ = 12, VWERASE_ = 14, VLNEXT_ = 15, VEOL2_ = 16, NCC_ = 19 };

#define IN_MAX 65536

struct aoi_tty {
    pthread_mutex_t mu;
    uint32_t iflag, oflag, cflag, lflag, ispeed, ospeed;
    uint8_t line, cc[NCC_];
    uint16_t ws[4];                     /* rows, cols, xpixel, ypixel */
    char in[IN_MAX];                    /* what read() gets */
    size_t nin;
    int eofs;                           /* Ctrl-Ds with an empty line: each ends one read() with 0 */
    size_t eof_at[16];                  /* (where in `in` each falls) */
    char edit[4096];                    /* the line being edited (canonical mode) */
    size_t nedit;
    int lnext;                          /* the next key is taken literally (Ctrl-V) */
    void (*out)(void *ctx, const char *s, size_t n);
    void *ctx;
    int index;                          /* N of /dev/pts/N */
    int ph[2], phm[2];                  /* host objects whose dups are its slave / master guest fds */
    /* every open of the slave gets a socket pair of its own: the guest fd (and its dups,
     * its copies in forked children) are that pair's end 0, the terminal keeps end 1; when
     * every end 0 is closed, end 1 hangs up: the slave is closed (master reads EIO) */
    int tok[64];
    dev_t tokdev[64];
    ino_t tokino[64];
    int ntok, opened;
    int fg;                             /* foreground process group (0: none yet) */
    char *mbuf;                         /* a guest master's side: output waiting for it */
    size_t mlen;
};

#define MBUF 65536

struct aoi_tty *aoi_console;
static struct aoi_tty *ttys[AOI_TTYS];
static pthread_mutex_t ttys_mu = PTHREAD_MUTEX_INITIALIZER;

/* A host object with an identity of its own: an unlinked temp file (sockets and pipes have
 * no dependable inode on Darwin). sv[0] is what guest fds dup, holding a shared flock;
 * sv[1] is a second open of the same file: an exclusive flock through it succeeds only
 * once every dup of sv[0] is closed. */
static int pair(int sv[2])
{
    char path[1024];
    const char *tmp = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/aoitty-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if ((sv[0] = mkstemp(path)) < 0) return -1;
    if ((sv[1] = open(path, O_RDONLY)) < 0) { close(sv[0]); unlink(path); return -1; }
    unlink(path);
    fcntl(sv[0], F_SETFD, FD_CLOEXEC); fcntl(sv[1], F_SETFD, FD_CLOEXEC);
    flock(sv[0], LOCK_SH);
    return 0;
}

/* Every dup of sv[0] is closed. */
static int pair_released(int keep)
{
    if (flock(keep, LOCK_EX | LOCK_NB)) return 0;
    flock(keep, LOCK_UN);
    return 1;
}

/* A new terminal in the first free slot (or NULL). */
static struct aoi_tty *tty_alloc(void)
{
    struct aoi_tty *t = calloc(1, sizeof *t);
    int i;
    if (!t) return NULL;
    if (pair(t->ph) || pair(t->phm)) { free(t); return NULL; }
    pthread_mutex_lock(&ttys_mu);
    for (i = 0; i < AOI_TTYS && ttys[i]; i++) {}
    if (i < AOI_TTYS) { ttys[i] = t; t->index = i; }
    pthread_mutex_unlock(&ttys_mu);
    if (i == AOI_TTYS) { close(t->ph[0]); close(t->ph[1]); close(t->phm[0]); close(t->phm[1]); free(t); return NULL; }
    return t;
}

struct aoi_tty *aoi_tty_new(void (*out)(void *ctx, const char *s, size_t n), void *ctx, int cols, int rows)
{
    struct aoi_tty *t = tty_alloc();
    static const uint8_t cc[NCC_] = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0 };
    if (!t) return NULL;
    pthread_mutex_init(&t->mu, NULL);
    t->iflag = ICRNL_ | 02000 /* IXON */ | IUTF8_;
    t->oflag = OPOST_ | ONLCR_;
    t->cflag = 017 /* B38400 */ | 060 /* CS8 */ | 0200 /* CREAD */;
    t->lflag = ISIG_ | ICANON_ | ECHO_ | ECHOE_ | ECHOK_ | ECHOCTL_ | ECHOKE_ | IEXTEN_;
    t->ispeed = t->ospeed = 38400;
    memcpy(t->cc, cc, sizeof cc);
    t->ws[0] = (uint16_t)(rows > 0 ? rows : 24);
    t->ws[1] = (uint16_t)(cols > 0 ? cols : 80);
    t->out = out;
    t->ctx = ctx;
    if (!aoi_console) aoi_console = t;
    return t;
}

/* A guest master: output goes to its buffer. */
static void master_out(void *ctx, const char *s, size_t n)
{
    struct aoi_tty *t = ctx;
    pthread_mutex_lock(&t->mu);
    if (n > MBUF - t->mlen) n = MBUF - t->mlen;
    memcpy(t->mbuf + t->mlen, s, n);
    t->mlen += n;
    pthread_mutex_unlock(&t->mu);
}

struct aoi_tty *aoi_tty_new_pty(void)
{
    char *b = malloc(MBUF);
    struct aoi_tty *t;
    if (!b) return NULL;
    if (!(t = aoi_tty_new(master_out, NULL, 80, 24))) { free(b); return NULL; }
    t->ctx = t;
    t->mbuf = b;
    if (aoi_console == t) aoi_console = NULL;                       /* (never the console) */
    return t;
}

int aoi_tty_index(struct aoi_tty *t) { return t->index; }

struct aoi_tty *aoi_tty_get_index(int n)
{
    struct aoi_tty *t;
    if (n < 0 || n >= AOI_TTYS) return NULL;
    pthread_mutex_lock(&ttys_mu);
    t = ttys[n];
    pthread_mutex_unlock(&ttys_mu);
    return t;
}

/* Closed slave opens go (lock held). */
static void tok_prune(struct aoi_tty *t)
{
    int i;
    for (i = 0; i < t->ntok; i++) {
        if (pair_released(t->tok[i])) {
            close(t->tok[i]);
            t->tok[i] = t->tok[--t->ntok]; t->tokdev[i] = t->tokdev[t->ntok]; t->tokino[i] = t->tokino[t->ntok];
            i--;
        }
    }
}

int aoi_tty_host_fd(struct aoi_tty *t, int master)
{
    int sv[2];
    struct stat st;
    if (master) {
        int d = dup(t->phm[0]);
        if (d >= 0) fcntl(d, F_SETFD, FD_CLOEXEC);
        return d;
    }
#ifdef __EMSCRIPTEN__
    {   /* no flock in the web view (it always succeeds), so no tokens to count closed copies by:
         * every slave fd is a dup of ph[0], found by its inode (a hang-up is never seen) */
        int d = dup(t->ph[0]);
        (void)sv; (void)st;
        if (d >= 0) fcntl(d, F_SETFD, FD_CLOEXEC);
        t->opened = 1;
        return d;
    }
#endif
    pthread_mutex_lock(&t->mu);
    tok_prune(t);
    if (t->ntok == 64 || pair(sv) || fstat(sv[0], &st)) { pthread_mutex_unlock(&t->mu); return -1; }
    t->tok[t->ntok] = sv[1]; t->tokdev[t->ntok] = st.st_dev; t->tokino[t->ntok] = st.st_ino;
    t->ntok++;
    t->opened = 1;
    pthread_mutex_unlock(&t->mu);
    return sv[0];
}

int aoi_tty_hungup(struct aoi_tty *t)
{
    int r;
#ifdef __EMSCRIPTEN__
    (void)t;
    return 0;
#endif
    pthread_mutex_lock(&t->mu);
    tok_prune(t);
    r = t->opened && !t->ntok;
    pthread_mutex_unlock(&t->mu);
    return r;
}

struct aoi_tty *aoi_tty_of_host(int hostfd, int *master)
{
    struct stat a, b;
    int i;
    if (hostfd < 0 || fstat(hostfd, &a)) return NULL;
    pthread_mutex_lock(&ttys_mu);
    for (i = 0; i < AOI_TTYS; i++) {
        struct aoi_tty *t = ttys[i];
        if (!t) continue;
        int k, hit = 0;
        for (k = 0; k < t->ntok; k++) if (a.st_ino == t->tokino[k] && a.st_dev == t->tokdev[k]) { hit = 1; break; }
#ifdef __EMSCRIPTEN__
        if (!hit && !fstat(t->ph[0], &b) && a.st_ino == b.st_ino && a.st_dev == b.st_dev) hit = 1;
#endif
        if (hit) { if (master) *master = 0; break; }
        if (!fstat(t->phm[0], &b) && a.st_ino == b.st_ino && a.st_dev == b.st_dev) { if (master) *master = 1; break; }
    }
    pthread_mutex_unlock(&ttys_mu);
    return i < AOI_TTYS ? ttys[i] : NULL;
}

long aoi_tty_master_read(struct aoi_tty *t, void *buf, size_t n)
{
    long r = -1;
    pthread_mutex_lock(&t->mu);
    if (t->mlen) {
        size_t k = t->mlen < n ? t->mlen : n;
        memcpy(buf, t->mbuf, k);
        memmove(t->mbuf, t->mbuf + k, t->mlen - k);
        t->mlen -= k;
        r = (long)k;
    }
    pthread_mutex_unlock(&t->mu);
    return r;
}

int aoi_tty_master_readable(struct aoi_tty *t)
{
    int r;
    pthread_mutex_lock(&t->mu);
    r = t->mlen > 0;
    pthread_mutex_unlock(&t->mu);
    return r;
}

int aoi_tty_fg(struct aoi_tty *t) { return t == aoi_console && aoi_tty_pgrp ? aoi_tty_pgrp : t->fg; }

void aoi_tty_set_fg(struct aoi_tty *t, int pgrp)
{
    t->fg = pgrp;
    if (t == aoi_console) aoi_tty_pgrp = pgrp;
}

/* Output processing (OPOST) into o; returns its length. */
static size_t post(struct aoi_tty *t, const char *s, size_t n, char *o, size_t cap)
{
    size_t i, k = 0;
    for (i = 0; i < n && k + 2 <= cap; i++) {
        char ch = s[i];
        if (t->oflag & OPOST_) {
            if (ch == '\n' && (t->oflag & ONLCR_)) { o[k++] = '\r'; o[k++] = '\n'; continue; }
            if (ch == '\r' && (t->oflag & OCRNL_)) ch = '\n';
        }
        o[k++] = ch;
    }
    return k;
}

/* Sends output to the host, in pieces, without holding the lock. */
static void emit(struct aoi_tty *t, const char *s, size_t n, int locked)
{
    char o[2048];
    while (n) {
        size_t take = n > 1000 ? 1000 : n, k;
        uint32_t of;
        if (!locked) pthread_mutex_lock(&t->mu);
        of = t->oflag;
        (void)of;
        k = post(t, s, take, o, sizeof o);
        if (!locked) pthread_mutex_unlock(&t->mu);
        if (t->out) t->out(t->ctx, o, k);
        s += take; n -= take;
    }
}

size_t aoi_tty_write(struct aoi_tty *t, const void *buf, size_t n)
{
    if (t->mbuf) {                                                 /* a guest master: not past its buffer */
        size_t room;
        pthread_mutex_lock(&t->mu);
        room = MBUF - t->mlen;
        pthread_mutex_unlock(&t->mu);
        room = room > 16 ? (room - 16) / 2 : 0;                    /* (NL may become CR NL) */
        if (n > room) n = room;
    }
    if (n) emit(t, buf, n, 0);
    return n;
}

/* Echo of one input byte (lock held). Control characters as ^X with ECHOCTL. */
static void echo_byte(struct aoi_tty *t, unsigned char ch, char *e, size_t *ne)
{
    if (*ne + 4 > 256) return;
    if ((t->lflag & ECHOCTL_) && ch < 32 && ch != '\n' && ch != '\t' && ch != '\r') {
        e[(*ne)++] = '^'; e[(*ne)++] = (char)(ch + 64);
    } else if ((t->lflag & ECHOCTL_) && ch == 127) {
        e[(*ne)++] = '^'; e[(*ne)++] = '?';
    } else e[(*ne)++] = (char)ch;
}

static void push_in(struct aoi_tty *t, const char *s, size_t n)
{
    if (t->nin + n > IN_MAX) n = IN_MAX - t->nin;
    memcpy(t->in + t->nin, s, n);
    t->nin += n;
}

/* Removes the last character of the line being edited (a whole UTF-8 sequence). */
static int erase_one(struct aoi_tty *t, char *e, size_t *ne)
{
    unsigned char last;
    int ctl;
    if (!t->nedit) return 0;
    do { last = (unsigned char)t->edit[--t->nedit]; } while (t->nedit && (last & 0xc0) == 0x80);
    ctl = (t->lflag & ECHOCTL_) && (last < 32 || last == 127) && last != '\t';
    if ((t->lflag & ECHO_) && (t->lflag & ECHOE_) && *ne + 6 < 256) {
        memcpy(e + *ne, "\b \b", 3); *ne += 3;
        if (ctl) { memcpy(e + *ne, "\b \b", 3); *ne += 3; }
    }
    return 1;
}

void aoi_tty_input(struct aoi_tty *t, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        char e[256];
        size_t ne = 0;
        int sig = 0;
        pthread_mutex_lock(&t->mu);
        if (t->lnext) {
            t->lnext = 0;
            goto literal;
        }
        if (t->iflag & ISTRIP_) ch &= 0x7f;
        if (ch == '\r') {
            if (t->iflag & IGNCR_) { pthread_mutex_unlock(&t->mu); continue; }
            if (t->iflag & ICRNL_) ch = '\n';
        } else if (ch == '\n' && (t->iflag & INLCR_)) ch = '\r';
        if (t->lflag & ISIG_) {
            if (ch && ch == t->cc[VINTR_]) sig = 2;
            else if (ch && ch == t->cc[VQUIT_]) sig = 3;
            else if (ch && ch == t->cc[VSUSP_]) sig = 20;
            if (sig) {
                if (!(t->lflag & NOFLSH_)) { t->nedit = 0; t->nin = 0; t->eofs = 0; }
                if (t->lflag & ECHO_) { echo_byte(t, ch, e, &ne); if (sig != 20) e[ne++] = '\n'; }
                pthread_mutex_unlock(&t->mu);
                if (ne) emit(t, e, ne, 0);
                if (aoi_tty_fg(t)) aoi_proc_kill_group(aoi_tty_fg(t), sig);
                continue;
            }
        }
        if (t->lflag & ICANON_) {
            if (ch == t->cc[VERASE_] || ch == 8) { erase_one(t, e, &ne); goto done; }
            if (ch == t->cc[VKILL_]) { while (erase_one(t, e, &ne)) {} goto done; }
            if ((t->lflag & IEXTEN_) && ch == t->cc[VWERASE_]) {
                while (t->nedit && t->edit[t->nedit - 1] == ' ') erase_one(t, e, &ne);
                while (t->nedit && t->edit[t->nedit - 1] != ' ') erase_one(t, e, &ne);
                goto done;
            }
            if ((t->lflag & IEXTEN_) && ch == t->cc[VLNEXT_]) { t->lnext = 1; goto done; }
            if (ch == t->cc[VEOF_]) {                              /* Ctrl-D: the line as it is, or end of file */
                if (!t->nedit && t->eofs < 16) t->eof_at[t->eofs++] = t->nin;
                push_in(t, t->edit, t->nedit);
                t->nedit = 0;
                goto done;
            }
            if (ch == '\n' || (ch && (ch == t->cc[VEOL_] || ch == t->cc[VEOL2_]))) {
                if (t->nedit < sizeof t->edit) t->edit[t->nedit++] = (char)ch;
                push_in(t, t->edit, t->nedit);
                t->nedit = 0;
                if ((t->lflag & ECHO_) || (t->lflag & ECHONL_)) e[ne++] = '\n';
                goto done;
            }
literal:
            if (t->lflag & ICANON_) {
                if (t->nedit < sizeof t->edit - 1) t->edit[t->nedit++] = (char)ch;
                if (t->lflag & ECHO_) echo_byte(t, ch, e, &ne);
                goto done;
            }
        }
        push_in(t, (const char *)&ch, 1);                          /* raw: straight to read() */
        if (t->lflag & ECHO_) echo_byte(t, ch, e, &ne);
done:
        pthread_mutex_unlock(&t->mu);
        if (ne) emit(t, e, ne, 0);
    }
}

void aoi_tty_resize(struct aoi_tty *t, int cols, int rows)
{
    pthread_mutex_lock(&t->mu);
    t->ws[0] = (uint16_t)rows; t->ws[1] = (uint16_t)cols;
    pthread_mutex_unlock(&t->mu);
    if (aoi_tty_fg(t)) aoi_proc_kill_group(aoi_tty_fg(t), 28);     /* SIGWINCH */
}

long aoi_tty_read(struct aoi_tty *t, void *buf, size_t n)
{
    long r = -1;
    size_t k, lim;
    pthread_mutex_lock(&t->mu);
    lim = t->eofs ? t->eof_at[0] : t->nin;                         /* up to the next end of file */
    if (t->eofs && !lim) {                                         /* at an end of file: 0, once */
        memmove(t->eof_at, t->eof_at + 1, sizeof t->eof_at[0] * (size_t)--t->eofs);
        r = 0;
    } else if (lim) {
        k = lim < n ? lim : n;
        if (t->lflag & ICANON_) {                                  /* at most one line */
            size_t j;
            for (j = 0; j < k; j++) if (t->in[j] == '\n') { k = j + 1; break; }
        }
        memcpy(buf, t->in, k);
        memmove(t->in, t->in + k, t->nin - k);
        t->nin -= k;
        { int i; for (i = 0; i < t->eofs; i++) t->eof_at[i] -= k; }
        r = (long)k;
    } else if (!(t->lflag & ICANON_) && t->cc[VMIN_] == 0 && t->cc[VTIME_] == 0) r = 0;   /* polling read */
    pthread_mutex_unlock(&t->mu);
    return r;
}

int aoi_tty_readable(struct aoi_tty *t)
{
    int r;
    pthread_mutex_lock(&t->mu);
    r = t->nin > 0 || t->eofs > 0;
    pthread_mutex_unlock(&t->mu);
    return r;
}

size_t aoi_tty_pending(struct aoi_tty *t)
{
    size_t r;
    pthread_mutex_lock(&t->mu);
    r = t->nin;
    pthread_mutex_unlock(&t->mu);
    return r;
}

void aoi_tty_flush(struct aoi_tty *t)
{
    pthread_mutex_lock(&t->mu);
    t->nin = 0; t->nedit = 0; t->eofs = 0;
    pthread_mutex_unlock(&t->mu);
}

static void put32_(uint8_t *b, uint32_t v) { memcpy(b, &v, 4); }
static uint32_t get32_(const uint8_t *b) { uint32_t v; memcpy(&v, b, 4); return v; }

void aoi_tty_get(struct aoi_tty *t, uint8_t tio[44])
{
    pthread_mutex_lock(&t->mu);
    memset(tio, 0, 44);
    put32_(tio, t->iflag); put32_(tio + 4, t->oflag); put32_(tio + 8, t->cflag); put32_(tio + 12, t->lflag);
    tio[16] = t->line;
    memcpy(tio + 17, t->cc, NCC_);
    put32_(tio + 36, t->ispeed); put32_(tio + 40, t->ospeed);
    pthread_mutex_unlock(&t->mu);
}

void aoi_tty_set(struct aoi_tty *t, const uint8_t *tio, size_t n, int flush)
{
    pthread_mutex_lock(&t->mu);
    t->iflag = get32_(tio); t->oflag = get32_(tio + 4); t->cflag = get32_(tio + 8);
    {
        uint32_t lf = get32_(tio + 12);
        if ((t->lflag & ICANON_) && !(lf & ICANON_) && t->nedit) {   /* to raw: the half line becomes input */
            push_in(t, t->edit, t->nedit);
            t->nedit = 0;
        }
        t->lflag = lf;
    }
    t->line = tio[16];
    memcpy(t->cc, tio + 17, NCC_);
    if (n >= 44) { t->ispeed = get32_(tio + 36); t->ospeed = get32_(tio + 40); }
    if (flush) { t->nin = 0; t->eofs = 0; }
    pthread_mutex_unlock(&t->mu);
}

void aoi_tty_winsize(struct aoi_tty *t, uint16_t ws[4])
{
    pthread_mutex_lock(&t->mu);
    memcpy(ws, t->ws, sizeof t->ws);
    pthread_mutex_unlock(&t->mu);
}

void aoi_tty_set_winsize(struct aoi_tty *t, const uint16_t ws[4])
{
    pthread_mutex_lock(&t->mu);
    memcpy(t->ws, ws, sizeof t->ws);
    pthread_mutex_unlock(&t->mu);
}
