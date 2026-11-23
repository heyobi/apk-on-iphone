/* The terminal of the Linux userland mode: the slave side of a pseudo-terminal with
 * Linux's line discipline (canonical editing, echo, Ctrl-C/Ctrl-\ as signals to the
 * foreground process group, CR/NL mapping, output NL -> CR NL), kept in-process.
 *
 * The host (the iOS terminal view, or tools/lxrun) feeds the user's keys with
 * aoi_tty_input and gets the guest's output through the `out` callback. Guest fds of
 * kind AOI_FD_TTY (core/proc.c: fds 0-2 of the first process, /dev/tty, /dev/pts/0)
 * read, write and ioctl (termios, window size, foreground group) through here. One
 * terminal per host process (aoi_console). */
#ifndef AOI_TTY_H
#define AOI_TTY_H

#include <stddef.h>
#include <stdint.h>

struct aoi_tty;
extern struct aoi_tty *aoi_console;

/* Makes the terminal (and aoi_console). out is called from guest threads with what
 * the guest writes and what is echoed, already post-processed (NL -> CR NL). */
struct aoi_tty *aoi_tty_new(void (*out)(void *ctx, const char *s, size_t n), void *ctx, int cols, int rows);
/* Keys typed by the user (UTF-8, control characters as themselves: Ctrl-C is 3, Enter
 * is 13, Backspace 127, arrows as ESC sequences). Any host thread. */
void aoi_tty_input(struct aoi_tty *t, const char *s, size_t n);
/* A new window size: TIOCGWINSZ answers it, the foreground group gets SIGWINCH. */
void aoi_tty_resize(struct aoi_tty *t, int cols, int rows);

/* For core/proc.c. read: bytes, 0 at end of file (Ctrl-D), or -1 if nothing is there yet. */
long aoi_tty_read(struct aoi_tty *t, void *buf, size_t n);
void aoi_tty_write(struct aoi_tty *t, const void *buf, size_t n);
int aoi_tty_readable(struct aoi_tty *t);
/* termios as Linux's struct termios2 (44 bytes; struct termios is its first 36). */
void aoi_tty_get(struct aoi_tty *t, uint8_t tio[44]);
void aoi_tty_set(struct aoi_tty *t, const uint8_t *tio, size_t n, int flush);
void aoi_tty_winsize(struct aoi_tty *t, uint16_t ws[4]);
void aoi_tty_set_winsize(struct aoi_tty *t, const uint16_t ws[4]);
size_t aoi_tty_pending(struct aoi_tty *t);
void aoi_tty_flush(struct aoi_tty *t);

#endif
