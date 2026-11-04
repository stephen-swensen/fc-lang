/* lineedit.c: the interactive session's line editor (see lineedit.h). */

/* cfmakeraw-era termios and ioctl names under -std=c11. Must precede every #include. */
#define _DEFAULT_SOURCE

#include "lineedit.h"
#include "common.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

LineResult lineedit_read(const char *prompt, char **out) {
    (void)prompt;
    *out = NULL;
    return LE_EOF;
}
void lineedit_history_add(const char *entry) { (void)entry; }
void lineedit_free(void) {}

#else

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static char **g_hist;
static int g_hist_n, g_hist_cap;

void lineedit_history_add(const char *entry) {
    if (!*entry) return;
    if (g_hist_n > 0 && strcmp(g_hist[g_hist_n - 1], entry) == 0) return;
    DA_APPEND(g_hist, g_hist_n, g_hist_cap, str_dup(entry));
}

void lineedit_free(void) {
    for (int i = 0; i < g_hist_n; i++) free(g_hist[i]);
    free(g_hist);
    g_hist = NULL;
    g_hist_n = g_hist_cap = 0;
}

/* The line being edited, NUL-terminated, and the cursor's byte offset. */
typedef struct {
    char *p;
    size_t len, cap, pos;
} Line;

static void line_reserve(Line *l, size_t extra) {
    if (l->len + extra + 1 <= l->cap) return;
    size_t nc = l->cap ? l->cap * 2 : 128;
    while (nc < l->len + extra + 1) nc *= 2;
    l->p = xrealloc(l->p, nc);
    l->cap = nc;
}

static void line_set(Line *l, const char *s) {
    size_t n = strlen(s);
    l->len = 0;
    line_reserve(l, n);
    memcpy(l->p, s, n + 1);
    l->len = l->pos = n;
}

static void line_insert(Line *l, const char *s, size_t n) {
    line_reserve(l, n);
    memmove(l->p + l->pos + n, l->p + l->pos, l->len - l->pos + 1);
    memcpy(l->p + l->pos, s, n);
    l->len += n;
    l->pos += n;
}

static void line_erase(Line *l, size_t at, size_t n) {
    if (at + n > l->len) n = l->len - at;
    memmove(l->p + at, l->p + at + n, l->len - at - n + 1);
    l->len -= n;
    if (l->pos > at + n) l->pos -= n;
    else if (l->pos > at) l->pos = at;
}

/* A UTF-8 continuation byte: part of the character before it. */
static bool is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static size_t char_left(const Line *l, size_t at) {
    if (at == 0) return 0;
    do at--; while (at > 0 && is_cont(l->p[at]));
    return at;
}

static size_t char_right(const Line *l, size_t at) {
    if (at >= l->len) return l->len;
    do at++; while (at < l->len && is_cont(l->p[at]));
    return at;
}

/* Columns `n` bytes of text take: one per character. */
static size_t columns(const char *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) if (!is_cont(s[i])) c++;
    return c;
}

static void put(const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static void puts_raw(const char *s) { put(s, strlen(s)); }

static int term_cols(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

/* Redraw the prompt and the line. A line wider than the terminal scrolls
 * sideways to keep the cursor in view. */
static void refresh(const char *prompt, const Line *l) {
    size_t pcols = columns(prompt, strlen(prompt));
    size_t room = (size_t)term_cols();
    room = room > pcols + 2 ? room - pcols - 1 : 1;
    size_t start = 0;
    while (columns(l->p + start, l->pos - start) >= room) start = char_right(l, start);
    size_t end = start, cols = 0;
    while (end < l->len && cols < room) { end = char_right(l, end); cols++; }
    char buf[64];
    puts_raw("\r");
    puts_raw(prompt);
    put(l->p + start, end - start);
    puts_raw("\x1b[K\r");
    size_t at = pcols + columns(l->p + start, l->pos - start);
    if (at > 0) {
        snprintf(buf, sizeof buf, "\x1b[%zuC", at);
        puts_raw(buf);
    }
}

static int read_byte(void) {
    unsigned char c;
    for (;;) {
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r == 1) return c;
        if (r < 0 && errno == EINTR) continue;
        return -1;
    }
}

/* The bytes of a paste, after its opening ESC [ 200 ~ and up to its closing
 * ESC [ 201 ~, with \r\n and \r made \n. */
static char *read_paste(void) {
    Line t = { 0 };
    line_reserve(&t, 0);
    t.p[0] = 0;
    static const char end[] = "\x1b[201~";
    size_t matched = 0;
    for (;;) {
        int c = read_byte();
        if (c < 0) break;
        if ((char)c == end[matched]) {
            if (++matched == sizeof end - 1) break;
            continue;
        }
        if (matched) {     /* a false start of the end marker is text */
            line_insert(&t, end, matched);
            matched = 0;
            if ((char)c == end[0]) { matched = 1; continue; }
        }
        char ch = (char)c;
        if (ch == '\r') ch = '\n';
        if (ch == '\n' && t.len > 0 && t.p[t.len - 1] == '\r') t.len--;
        line_insert(&t, &ch, 1);
    }
    return t.p;
}

LineResult lineedit_read(const char *prompt, char **out) {
    *out = NULL;
    fflush(stdout);
    fflush(stderr);
    struct termios orig, raw;
    if (tcgetattr(STDIN_FILENO, &orig) != 0) return LE_EOF;
    raw = orig;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);
    puts_raw("\x1b[?2004h");      /* bracketed paste on */

    Line l = { 0 };
    line_reserve(&l, 0);
    l.p[0] = 0;
    char *stash = NULL;           /* the new line, while history is shown */
    int hist = g_hist_n;
    LineResult result = LE_LINE;
    refresh(prompt, &l);
    for (;;) {
        int c = read_byte();
        if (c < 0) { result = l.len ? LE_LINE : LE_EOF; break; }
        if (c == '\r' || c == '\n') { result = LE_LINE; break; }
        if (c == 3) { puts_raw("^C"); result = LE_INTERRUPT; break; }
        if (c == 4) {
            if (l.len == 0) { result = LE_EOF; break; }
            line_erase(&l, l.pos, char_right(&l, l.pos) - l.pos);
        } else if (c == 127 || c == 8) {
            size_t to = char_left(&l, l.pos);
            line_erase(&l, to, l.pos - to);
        } else if (c == 1) {
            l.pos = 0;
        } else if (c == 5) {
            l.pos = l.len;
        } else if (c == 2) {
            l.pos = char_left(&l, l.pos);
        } else if (c == 6) {
            l.pos = char_right(&l, l.pos);
        } else if (c == 11) {
            line_erase(&l, l.pos, l.len - l.pos);
        } else if (c == 21) {
            line_erase(&l, 0, l.pos);
        } else if (c == 23) {
            size_t to = l.pos;
            while (to > 0 && l.p[to - 1] == ' ') to--;
            while (to > 0 && l.p[to - 1] != ' ') to--;
            line_erase(&l, to, l.pos - to);
        } else if (c == 12) {
            puts_raw("\x1b[H\x1b[2J");
        } else if (c == 9) {
            line_insert(&l, "    ", 4);
        } else if (c == 16 || c == 14) {
            c = c == 16 ? 'A' : 'B';
            goto history;
        } else if (c == 27) {
            int a = read_byte();
            if (a == '\r' || a == '\n') { result = LE_MORE; break; }   /* Alt+Enter */
            if (a != '[' && a != 'O') continue;
            int num = 0, b;
            while ((b = read_byte()) >= '0' && b <= '9') num = num * 10 + (b - '0');
            if (b == '~') {
                if (num == 200) {
                    char *paste = read_paste();
                    char *nl = strchr(paste, '\n');
                    if (!nl) {
                        line_insert(&l, paste, strlen(paste));
                        free(paste);
                    } else {
                        /* A paste of several lines is one input, whole. */
                        size_t plen = strlen(paste);
                        while (plen > 0 && paste[plen - 1] == '\n') paste[--plen] = 0;
                        Line all = { 0 };
                        line_reserve(&all, 0);
                        all.p[0] = 0;
                        line_insert(&all, l.p, l.pos);
                        line_insert(&all, paste, plen);
                        line_insert(&all, l.p + l.pos, l.len - l.pos);
                        free(paste);
                        free(l.p);
                        l = all;
                        /* Show it as entered: each line after the first at a
                         * prompt-wide indent. */
                        puts_raw("\r");
                        puts_raw(prompt);
                        size_t pw = columns(prompt, strlen(prompt));
                        for (const char *s = l.p; *s; s++) {
                            if (*s == '\n') {
                                puts_raw("\x1b[K\r\n");
                                for (size_t i = 0; i < pw; i++) put(" ", 1);
                            } else {
                                put(s, 1);
                            }
                        }
                        puts_raw("\x1b[K");
                        result = LE_PASTE;
                        break;
                    }
                } else if (num == 3) {
                    line_erase(&l, l.pos, char_right(&l, l.pos) - l.pos);
                } else if (num == 1 || num == 7) {
                    l.pos = 0;
                } else if (num == 4 || num == 8) {
                    l.pos = l.len;
                }
            } else if (b == 'C') {
                l.pos = char_right(&l, l.pos);
            } else if (b == 'D') {
                l.pos = char_left(&l, l.pos);
            } else if (b == 'H') {
                l.pos = 0;
            } else if (b == 'F') {
                l.pos = l.len;
            } else if (b == 'A' || b == 'B') {
                c = b;
                goto history;
            }
        } else if (c >= 32) {
            char ch = (char)c;
            line_insert(&l, &ch, 1);
        }
        refresh(prompt, &l);
        continue;
    history:
        if (c == 'A' && hist > 0) {
            if (hist == g_hist_n) { free(stash); stash = str_dup(l.p); }
            line_set(&l, g_hist[--hist]);
        } else if (c == 'B' && hist < g_hist_n) {
            hist++;
            line_set(&l, hist == g_hist_n ? (stash ? stash : "") : g_hist[hist]);
        }
        refresh(prompt, &l);
    }
    puts_raw("\x1b[?2004l\r\n");  /* bracketed paste off */
    tcsetattr(STDIN_FILENO, TCSADRAIN, &orig);
    free(stash);
    if (result == LE_LINE || result == LE_MORE || result == LE_PASTE) *out = l.p;
    else free(l.p);
    return result;
}

#endif
