/* Terminals of the Linux userland mode: pseudo-terminal slaves with Linux's line
 * discipline (canonical editing, echo, Ctrl-C/Ctrl-\/Ctrl-Z as signals to the
 * foreground process group, CR/NL mapping, output NL -> CR NL), kept in-process.
 *
 * Two kinds of master side:
 *  - the host's: the iOS terminal view (or tools/lxrun) feeds keys with aoi_tty_input
 *    and gets output through the `out` callback (aoi_tty_new; the first one is
 *    aoi_console, /dev/pts/0);
 *  - a guest's: open("/dev/ptmx") makes a pty whose master is a guest fd (tmux, script,
 *    sshd); its output waits in a buffer for aoi_tty_master_read.
 *
 * Guest fds refer to a terminal through their host fd: every terminal owns a unique host
 * object (a socket pair), and its fds are dups of one end, so the terminal is found from
 * any copy (fork, SCM_RIGHTS) by the host fd's inode (aoi_tty_of_host). */
#ifndef AOI_TTY_H
#define AOI_TTY_H

#include <stddef.h>
#include <stdint.h>

#define AOI_TTYS 32

struct aoi_tty;
extern struct aoi_tty *aoi_console;

/* A terminal with a host-side master (the view). The first is aoi_console. out is called
 * from guest threads with what the guest writes and what is echoed, post-processed. */
struct aoi_tty *aoi_tty_new(void (*out)(void *ctx, const char *s, size_t n), void *ctx, int cols, int rows);
/* A terminal whose master is a guest fd (/dev/ptmx), or NULL if all are in use. */
struct aoi_tty *aoi_tty_new_pty(void);
/* Keys typed by the user, or what a guest writes to the master side. Any host thread. */
void aoi_tty_input(struct aoi_tty *t, const char *s, size_t n);
/* A new window size: TIOCGWINSZ answers it, the foreground group gets SIGWINCH. */
void aoi_tty_resize(struct aoi_tty *t, int cols, int rows);

/* For core/proc.c. */
int aoi_tty_index(struct aoi_tty *t);                     /* N of /dev/pts/N */
struct aoi_tty *aoi_tty_get_index(int n);
/* A new host fd for a guest fd of this terminal (slave, or master if master), or -1. */
int aoi_tty_host_fd(struct aoi_tty *t, int master);
/* The terminal a guest fd's host fd belongs to (NULL: none); *master: its master side. */
struct aoi_tty *aoi_tty_of_host(int hostfd, int *master);
/* read: bytes, 0 at end of file (Ctrl-D), or -1 if nothing is there yet. */
long aoi_tty_read(struct aoi_tty *t, void *buf, size_t n);
/* write from the slave side: bytes taken (a guest master's buffer may be full: then 0). */
size_t aoi_tty_write(struct aoi_tty *t, const void *buf, size_t n);
int aoi_tty_readable(struct aoi_tty *t);
long aoi_tty_master_read(struct aoi_tty *t, void *buf, size_t n);   /* -1: nothing yet */
/* Every slave fd was closed (after one was opened): the master reads EIO, as Linux. */
int aoi_tty_hungup(struct aoi_tty *t);
int aoi_tty_master_readable(struct aoi_tty *t);
/* termios as Linux's struct termios2 (44 bytes; struct termios is its first 36). */
void aoi_tty_get(struct aoi_tty *t, uint8_t tio[44]);
void aoi_tty_set(struct aoi_tty *t, const uint8_t *tio, size_t n, int flush);
void aoi_tty_winsize(struct aoi_tty *t, uint16_t ws[4]);
void aoi_tty_set_winsize(struct aoi_tty *t, const uint16_t ws[4]);
size_t aoi_tty_pending(struct aoi_tty *t);
void aoi_tty_flush(struct aoi_tty *t);
/* The foreground process group (TIOCGPGRP/TIOCSPGRP; ISIG signals go to it). */
int aoi_tty_fg(struct aoi_tty *t);
void aoi_tty_set_fg(struct aoi_tty *t, int pgrp);

#endif
