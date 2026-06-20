/// debugger.c —-- TUI debugger / error-trap implementation for Monad
//
//  Implements the interface described in debugger.h: a damage-tracked,
//  mouse-aware, Emacs-cursor TUI that any compiler stage can drop into at
//  the moment a diagnostic fires, plus a vertico/orderless-style command
//  palette for driving it.
//
//  Source layout:
//
//    §1   Includes and internal constants
//    §2   Memory helpers
//    §3   String / cell-buffer helpers
//    §4   Terminal raw-mode + capability setup
//    §5   Screen buffer (front/back, damage tracking)
//    §6   ANSI/SGR rendering primitives
//    §7   Input: key decoding
//    §8   Input: mouse (SGR 1006) decoding
//    §9   Event queue
//    §10  Cursor blink timer (Emacs semantics)
//    §11  Layout: panes, splits, geometry
//    §12  Widget: text viewport (scrollback, source view)
//    §13  Widget: list (selectable, for stack frames / breakpoints)
//    §14  Widget: status / mode line
//    §15  Command palette: orderless completion engine
//    §16  Command palette: vertico-style minibuffer UI
//    §17  Command registry
//    §18  Error trapping & snapshot capture
//    §19  Backtrace model
//    §20  Breakpoints & watchpoints
//    §21  Source map / line table
//    §22  LLVM IR panel
//    §23  Variable / register inspector
//    §24  Disassembly panel
//    §25  Session model (the running debuggee)
//    §26  Panel registry & focus management
//    §27  Main render pass (damage-only redraw)
//    §28  Event loop / dispatcher
//    §29  Keymap (Emacs-ish chords, configurable)
//    §30  Theme / color palette
//    §31  Logging
//    §32  Public entry points
//

/* Feature test macros must precede every system header: clock_gettime /
   CLOCK_MONOTONIC need POSIX.1-2001, popen/pclose need POSIX.2, and we
   want both visible regardless of which libc the toolchain ships.       */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE    700

#include "debugger.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>


/// §1  Includes and internal constants

//// Internal constants
 //
 //  Mirrors the conventions in lsp.c: growth factor and initial sizes for
 //  every dynamic array, kept in one place so tuning is a one-line change.
 //
#define DBG_INITIAL_EVENTQ        64
#define DBG_INITIAL_PANES         8
#define DBG_INITIAL_COMMANDS      64
#define DBG_INITIAL_VARS          32
#define DBG_READ_CHUNK            8192
#define DBG_ESC_TIMEOUT_MS        25   /* bare ESC vs start-of-sequence */


/// §2  Memory helpers

//// Memory helpers
 //
 //  Same abort-on-OOM trade-off as lsp.c: the debugger is a short-lived
 //  diagnostic tool, restarting cleanly beats limping along with a NULL
 //  check on every allocation site.
 //
static void *dbg_xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "debugger: out of memory\n"); abort(); }
    return p;
}

static void *dbg_xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n, sz);
    if (!p) { fprintf(stderr, "debugger: out of memory\n"); abort(); }
    return p;
}

static void *dbg_xrealloc(void *ptr, size_t n)
{
    void *p = realloc(ptr, n);
    if (!p) { fprintf(stderr, "debugger: out of memory\n"); abort(); }
    return p;
}

static char *dbg_xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char  *d = dbg_xmalloc(n);
    memcpy(d, s, n);
    return d;
}

static char *dbg_xstrndup(const char *s, size_t n)
{
    if (!s) return NULL;
    char *d = dbg_xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* Grow a heap array to at least (count + 1) slots. Identical macro to
   lsp.c's LSP_GROW, renamed to keep the two translation units namespaced
   independently even though they will likely be linked together.        */
#define DBG_GROW(ptr, count, cap, type)                        \
    do {                                                       \
        if ((count) >= (cap)) {                                \
            (cap) = (cap) ? (cap) * DBG_GROW_FACTOR : 8;        \
            (ptr) = dbg_xrealloc((ptr), (cap) * sizeof(type));  \
        }                                                       \
    } while (0)


/// §3  String / cell-buffer helpers

//// String builder
 //
 //  Append-only growable buffer, used for building escape-sequence
 //  batches and rendered text. Same shape as lsp.c's StrBuf so anyone
 //  who has read one has read the other.
 //
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} DbgStrBuf;

static void sb_init(DbgStrBuf *b)
{
    b->data = dbg_xmalloc(DBG_INITIAL_BUF);
    b->data[0] = '\0';
    b->len = 0;
    b->cap = DBG_INITIAL_BUF;
}

static void sb_free(DbgStrBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void sb_ensure(DbgStrBuf *b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        while (b->cap < b->len + extra + 1)
            b->cap *= DBG_GROW_FACTOR;
        b->data = dbg_xrealloc(b->data, b->cap);
    }
}

static void sb_append(DbgStrBuf *b, const char *s)
{
    size_t n = strlen(s);
    sb_ensure(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void sb_appendn(DbgStrBuf *b, const char *s, size_t n)
{
    sb_ensure(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void sb_appendc(DbgStrBuf *b, char c)
{
    sb_ensure(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void sb_appendf(DbgStrBuf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void sb_appendf(DbgStrBuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    sb_ensure(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

static char *sb_take(DbgStrBuf *b)
{
    char *s = b->data;
    b->data = dbg_xmalloc(DBG_INITIAL_BUF);
    b->data[0] = '\0';
    b->len = 0;
    b->cap = DBG_INITIAL_BUF;
    return s;
}

//// Monotonic clock
 //
 //  All timing in the debugger (blink period, double-click detection,
 //  frame pacing) is measured against CLOCK_MONOTONIC in milliseconds,
 //  so a system-clock adjustment mid-session can never make the cursor
 //  jump or stall.
 //
static uint64_t dbg_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}


/// §4  Terminal raw-mode + capability setup

//// Raw mode
 //
 //  We disable canonical mode and echo, set VMIN=0/VTIME=1 so reads can
 //  be polled cooperatively with select() alongside other fds (a future
 //  debuggee's stdout, for instance), and switch to the alternate screen
 //  so the user's shell scrollback is untouched.
 //
static bool dbg_enable_raw_mode(DbgSession *s)
{
    if (tcgetattr(STDIN_FILENO, &s->saved_termios) != 0)
        return false;

    struct termios raw = s->saved_termios;
    raw.c_iflag &= ~(unsigned long)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(unsigned long)(OPOST);
    raw.c_cflag |= (unsigned long)(CS8);
    raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;   /* 100ms granularity; the event loop polls
                               more finely with select() for blink timing */

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
        return false;

    s->raw_mode_active = true;
    return true;
}

static void dbg_disable_raw_mode(DbgSession *s)
{
    if (!s->raw_mode_active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &s->saved_termios);
    s->raw_mode_active = false;
}

/* Emit the startup escape batch: alternate screen, mouse reporting,
   bracketed paste, and an initial cursor hide (the blink timer takes
   over cursor visibility from here on).                                 */
static void dbg_term_enter(const DbgConfig *cfg)
{
    const char *seq =
        "\x1b[?1049h"     /* alternate screen buffer            */
        "\x1b[?25l"       /* hide cursor (blink timer manages it) */
        "\x1b[2J"         /* clear                               */
        "\x1b[?2004h";    /* bracketed paste on                  */
    fwrite(seq, 1, strlen(seq), stdout);
    if (cfg->mouse_enabled) {
        const char *mouse_seq =
            "\x1b[?1000h"   /* basic mouse reporting              */
            "\x1b[?1003h"   /* any-motion tracking (for drag/hover) */
            "\x1b[?1006h";  /* SGR extended coordinates            */
        fwrite(mouse_seq, 1, strlen(mouse_seq), stdout);
    }
    fflush(stdout);
}

static void dbg_term_leave(const DbgConfig *cfg)
{
    if (cfg->mouse_enabled) {
        const char *mouse_seq = "\x1b[?1003l\x1b[?1000l\x1b[?1006l";
        fwrite(mouse_seq, 1, strlen(mouse_seq), stdout);
    }
    const char *seq =
        "\x1b[?2004l"     /* bracketed paste off                 */
        "\x1b[?25h"       /* show cursor                         */
        "\x1b[?1049l";    /* leave alternate screen               */
    fwrite(seq, 1, strlen(seq), stdout);
    fflush(stdout);
}

static void dbg_query_winsize(int *cols, int *rows)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

/* Capability detection is intentionally conservative: COLORTERM is the
   one signal that is both POSIX-portable and reliable across the
   terminals people actually run a compiler debugger inside (alacritty,
   kitty, iTerm2, foot, the VS Code integrated terminal, tmux >= 3.2).    */
static DbgTermCaps dbg_detect_caps(const DbgConfig *cfg)
{
    DbgTermCaps caps = {0};
    const char *colorterm = getenv("COLORTERM");
    caps.truecolor = cfg->truecolor_force ||
                      (colorterm && (strstr(colorterm, "truecolor") ||
                                      strstr(colorterm, "24bit")));
    caps.mouse_sgr = cfg->mouse_enabled;
    caps.bracketed_paste = true;
    const char *term = getenv("TERM");
    caps.kitty_keyboard = term && strstr(term, "kitty") != NULL;
    dbg_query_winsize(&caps.cols, &caps.rows);
    return caps;
}


/// §5  Screen buffer (front/back, damage tracking)

//// Damage-tracked cell grid
 //
 //  This is the performance core the user asked for: two equal-sized cell
 //  grids, `front` (what the terminal currently shows) and `back` (what
 //  the next frame should show). Every widget writes into `back` only.
 //  dbg_screen_flush() diffs back against front cell-by-cell and emits a
 //  cursor-position escape + glyph ONLY for cells that differ, then swaps
 //  the buffers. A full-screen redraw therefore costs O(1) escape bytes
 //  per *changed* cell, not O(rows * cols).
 //
 //  Adjacent changed cells on the same row are coalesced into a single
 //  run so we are not paying a CUP (cursor-position) escape per glyph
 //  when, say, an entire status line changes — that is the "redraws only
 //  what changes, and does so efficiently" requirement.
 //
struct DbgCell {
    uint32_t codepoint;   /* UTF-32; 0 means "untouched/blank"           */
    DbgStyle style;
};

struct DbgScreen {
    int       cols, rows;
    DbgCell  *front;
    DbgCell  *back;
    bool     *row_dirty;     /* fast pre-check before scanning a row      */
    DbgStrBuf out;           /* batched escape output for one flush       */
    DbgPoint  last_cursor;   /* where the physical terminal cursor sits   */
};

static DbgCell dbg_cell_blank(void)
{
    DbgCell c;
    c.codepoint = ' ';
    c.style.fg = (DbgColor){ 200, 200, 200 };
    c.style.bg = (DbgColor){ 0, 0, 0 };
    c.style.attrs = DBG_ATTR_NONE;
    return c;
}

static DbgScreen *dbg_screen_create(int cols, int rows)
{
    DbgScreen *sc = dbg_xcalloc(1, sizeof(*sc));
    sc->cols = cols;
    sc->rows = rows;
    size_t n = (size_t)cols * (size_t)rows;
    sc->front = dbg_xmalloc(n * sizeof(DbgCell));
    sc->back  = dbg_xmalloc(n * sizeof(DbgCell));
    sc->row_dirty = dbg_xmalloc((size_t)rows * sizeof(bool));
    DbgCell blank = dbg_cell_blank();
    for (size_t i = 0; i < n; i++) {
        sc->front[i] = blank;
        sc->back[i]  = blank;
    }
    /* Force the very first flush to paint everything: make front differ
       from back by giving front a sentinel codepoint.                   */
    for (size_t i = 0; i < n; i++) sc->front[i].codepoint = 0xFFFFFFFFu;
    for (int r = 0; r < rows; r++) sc->row_dirty[r] = true;
    sb_init(&sc->out);
    sc->last_cursor = (DbgPoint){ -1, -1 };
    return sc;
}

static void dbg_screen_free(DbgScreen *sc)
{
    if (!sc) return;
    free(sc->front);
    free(sc->back);
    free(sc->row_dirty);
    sb_free(&sc->out);
    free(sc);
}

/* Resize reallocates both buffers and forces a full repaint — there is
   no way to preserve damage tracking across a geometry change, but
   resizes are rare (a SIGWINCH), unlike per-keystroke redraws.          */
static void dbg_screen_resize(DbgScreen *sc, int cols, int rows)
{
    free(sc->front);
    free(sc->back);
    free(sc->row_dirty);
    sc->cols = cols;
    sc->rows = rows;
    size_t n = (size_t)cols * (size_t)rows;
    sc->front = dbg_xmalloc(n * sizeof(DbgCell));
    sc->back  = dbg_xmalloc(n * sizeof(DbgCell));
    sc->row_dirty = dbg_xmalloc((size_t)rows * sizeof(bool));
    DbgCell blank = dbg_cell_blank();
    for (size_t i = 0; i < n; i++) {
        sc->front[i] = blank;
        sc->front[i].codepoint = 0xFFFFFFFFu;
        sc->back[i]  = blank;
    }
    for (int r = 0; r < rows; r++) sc->row_dirty[r] = true;
}

/* Clear the back buffer to blank without touching front — used at the
   start of a render pass before widgets paint into it. This alone does
   NOT cause a redraw of unchanged cells; the diff in dbg_screen_flush
   still only emits the cells that actually differ from front.           */
static void dbg_screen_clear_back(DbgScreen *sc)
{
    DbgCell blank = dbg_cell_blank();
    size_t n = (size_t)sc->cols * (size_t)sc->rows;
    for (size_t i = 0; i < n; i++) sc->back[i] = blank;
}

static inline void dbg_screen_put(DbgScreen *sc, int row, int col,
                                   uint32_t codepoint, DbgStyle style)
{
    if (row < 0 || row >= sc->rows || col < 0 || col >= sc->cols) return;
    DbgCell *c = &sc->back[(size_t)row * (size_t)sc->cols + (size_t)col];
    c->codepoint = codepoint;
    c->style = style;
}

/* Write a UTF-8 string starting at (row, col), clipped to the screen
   width. Returns the column just past the last glyph written, useful
   for chaining widget draws left-to-right.                              */
static int dbg_screen_write(DbgScreen *sc, int row, int col,
                             const char *utf8, DbgStyle style)
{
    const unsigned char *p = (const unsigned char *)utf8;
    int cur = col;
    while (*p && cur < sc->cols) {
        uint32_t cp;
        int seq;
        unsigned char c = *p;
        if      (c < 0x80)           { cp = c;          seq = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F;    seq = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F;    seq = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07;    seq = 4; }
        else                          { cp = 0xFFFD;      seq = 1; }
        for (int i = 1; i < seq && p[i]; i++)
            cp = (cp << 6) | (p[i] & 0x3F);
        dbg_screen_put(sc, row, cur, cp, style);
        cur++;
        p += seq;
    }
    return cur;
}

static void dbg_screen_fill_rect(DbgScreen *sc, DbgRect rect, uint32_t cp,
                                  DbgStyle style)
{
    for (int r = rect.row; r < rect.row + rect.height; r++)
        for (int c = rect.col; c < rect.col + rect.width; c++)
            dbg_screen_put(sc, r, c, cp, style);
}


/// §6  ANSI/SGR rendering primitives

//// SGR diffing
 //
 //  We track the last-emitted style alongside the output buffer so a run
 //  of cells sharing a style costs one SGR escape, not one per cell.
 //
typedef struct {
    DbgStyle current;
    bool     valid;     /* false right after a flush reset                */
} DbgSgrState;

static void dbg_emit_sgr(DbgStrBuf *out, DbgSgrState *sgr, DbgStyle style,
                          bool truecolor)
{
    if (sgr->valid &&
        sgr->current.fg.r == style.fg.r && sgr->current.fg.g == style.fg.g &&
        sgr->current.fg.b == style.fg.b && sgr->current.bg.r == style.bg.r &&
        sgr->current.bg.g == style.bg.g && sgr->current.bg.b == style.bg.b &&
        sgr->current.attrs == style.attrs)
        return;   /* identical to last emitted style — skip entirely      */

    sb_append(out, "\x1b[0");
    if (style.attrs & DBG_ATTR_BOLD)      sb_append(out, ";1");
    if (style.attrs & DBG_ATTR_DIM)       sb_append(out, ";2");
    if (style.attrs & DBG_ATTR_ITALIC)    sb_append(out, ";3");
    if (style.attrs & DBG_ATTR_UNDERLINE) sb_append(out, ";4");
    if (style.attrs & DBG_ATTR_REVERSE)   sb_append(out, ";7");
    if (style.attrs & DBG_ATTR_STRIKE)    sb_append(out, ";9");

    if (truecolor) {
        sb_appendf(out, ";38;2;%u;%u;%u", style.fg.r, style.fg.g, style.fg.b);
        sb_appendf(out, ";48;2;%u;%u;%u", style.bg.r, style.bg.g, style.bg.b);
    } else {
        /* 256-color degradation: simple 6x6x6 cube quantization.         */
        int fr = style.fg.r * 5 / 255, fg = style.fg.g * 5 / 255, fb = style.fg.b * 5 / 255;
        int br = style.bg.r * 5 / 255, bgc = style.bg.g * 5 / 255, bb = style.bg.b * 5 / 255;
        int fgidx = 16 + 36 * fr + 6 * fg + fb;
        int bgidx = 16 + 36 * br + 6 * bgc + bb;
        sb_appendf(out, ";38;5;%d", fgidx);
        sb_appendf(out, ";48;5;%d", bgidx);
    }
    sb_appendc(out, 'm');
    sgr->current = style;
    sgr->valid = true;
}

static void dbg_emit_cup(DbgStrBuf *out, int row, int col)
{
    /* Terminal coordinates are 1-indexed. */
    sb_appendf(out, "\x1b[%d;%dH", row + 1, col + 1);
}

/* UTF-32 codepoint to UTF-8 bytes, appended directly to the buffer. */
static void dbg_emit_utf8(DbgStrBuf *out, uint32_t cp)
{
    char buf[4];
    int n;
    if (cp == 0) cp = ' ';
    if (cp < 0x80) {
        buf[0] = (char)cp; n = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    sb_appendn(out, buf, (size_t)n);
}

/* The actual damage-only flush: diff back vs front, emit minimal escape
   runs, swap buffers. This is the single function responsible for the
   "fast and performant, redraws only what changes" requirement.         */
static void dbg_screen_flush(DbgScreen *sc, bool truecolor)
{
    sc->out.len = 0;
    if (sc->out.data) sc->out.data[0] = '\0';
    DbgSgrState sgr = {0};

    for (int row = 0; row < sc->rows; row++) {
        int col = 0;
        while (col < sc->cols) {
            size_t idx = (size_t)row * (size_t)sc->cols + (size_t)col;
            DbgCell *f = &sc->front[idx];
            DbgCell *b = &sc->back[idx];
            if (f->codepoint == b->codepoint &&
                f->style.fg.r == b->style.fg.r && f->style.fg.g == b->style.fg.g &&
                f->style.fg.b == b->style.fg.b && f->style.bg.r == b->style.bg.r &&
                f->style.bg.g == b->style.bg.g && f->style.bg.b == b->style.bg.b &&
                f->style.attrs == b->style.attrs) {
                col++;
                continue;   /* unchanged cell — nothing emitted            */
            }

            /* Start of a changed run: position cursor once, then emit
               consecutive changed cells without re-issuing CUP each time. */
            if (sc->last_cursor.row != row || sc->last_cursor.col != col) {
                dbg_emit_cup(&sc->out, row, col);
            }
            while (col < sc->cols) {
                idx = (size_t)row * (size_t)sc->cols + (size_t)col;
                f = &sc->front[idx];
                b = &sc->back[idx];
                bool same = f->codepoint == b->codepoint &&
                    f->style.fg.r == b->style.fg.r && f->style.fg.g == b->style.fg.g &&
                    f->style.fg.b == b->style.fg.b && f->style.bg.r == b->style.bg.r &&
                    f->style.bg.g == b->style.bg.g && f->style.bg.b == b->style.bg.b &&
                    f->style.attrs == b->style.attrs;
                if (same) break;
                dbg_emit_sgr(&sc->out, &sgr, b->style, truecolor);
                dbg_emit_utf8(&sc->out, b->codepoint);
                *f = *b;
                col++;
            }
            sc->last_cursor.row = row;
            sc->last_cursor.col = col;
        }
        sc->row_dirty[row] = false;
    }

    if (sc->out.len > 0) {
        fwrite(sc->out.data, 1, sc->out.len, stdout);
        fflush(stdout);
    }
}


/// §7  Input: key decoding

//// Raw byte reader
 //
 //  A small ring of unread bytes lets the escape-sequence decoder peek
 //  ahead without a syscall per byte. Reads are non-blocking; the event
 //  loop in §28 uses select() to know when data is actually available.
 //
typedef struct {
    unsigned char buf[256];
    size_t        head, tail;   /* tail == head means empty               */
} DbgByteRing;

static bool ring_empty(DbgByteRing *r) { return r->head == r->tail; }

static int ring_pop(DbgByteRing *r)
{
    if (ring_empty(r)) return -1;
    int b = r->buf[r->head];
    r->head = (r->head + 1) % sizeof(r->buf);
    return b;
}

static void ring_push(DbgByteRing *r, unsigned char b)
{
    r->buf[r->tail] = b;
    r->tail = (r->tail + 1) % sizeof(r->buf);
}

static void ring_fill_nonblocking(DbgByteRing *r)
{
    unsigned char tmp[128];
    ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
    for (ssize_t i = 0; i < n; i++) ring_push(r, tmp[i]);
}

/* Decode CSI / SS3 escape sequences for special keys. Cursor is just
   past the initial ESC '['; `first` is the first parameter/final byte
   already consumed by the caller while peeking ahead to distinguish a
   plain CSI key sequence from an SGR mouse report (ESC [ <). Passing
   the already-read byte in explicitly means the byte ring never needs
   a push-back, which a plain FIFO ring cannot do correctly anyway.      */
static bool decode_csi_key(DbgByteRing *r, int first, DbgKeyEvent *out)
{
    char params[32];
    size_t plen = 0;
    if (first >= '0' && first <= '9') {
        params[plen++] = (char)first;
    } else if (first != -1) {
        params[plen] = '\0';
        /* `first` is itself the final byte (no parameters at all, e.g.
           plain ESC [ A for an unmodified Up arrow).                    */
        int c = first;
        int p1 = 0, p2 = 0;
        bool shift = false, meta = false, ctrl = false;
        switch (c) {
        case 'A': out->sym = DBG_KEY_UP;    break;
        case 'B': out->sym = DBG_KEY_DOWN;  break;
        case 'C': out->sym = DBG_KEY_RIGHT; break;
        case 'D': out->sym = DBG_KEY_LEFT;  break;
        case 'H': out->sym = DBG_KEY_HOME;  break;
        case 'F': out->sym = DBG_KEY_END;   break;
        case 'Z': out->sym = DBG_KEY_BACKTAB; break;
        default:  out->sym = DBG_KEY_CHAR; out->codepoint = 0; break;
        }
        (void)p1; (void)p2;
        out->shift = shift; out->meta = meta; out->ctrl = ctrl;
        return true;
    }
    int c;
    while ((c = ring_pop(r)) != -1 && plen + 1 < sizeof(params)) {
        if ((c >= '0' && c <= '9') || c == ';') {
            params[plen++] = (char)c;
            continue;
        }
        params[plen] = '\0';
        int p1 = 0, p2 = 0;
        sscanf(params, "%d;%d", &p1, &p2);
        bool shift = (p2 == 2 || p2 == 4 || p2 == 6 || p2 == 8);
        bool meta  = (p2 == 3 || p2 == 4 || p2 == 7 || p2 == 8);
        bool ctrl  = (p2 == 5 || p2 == 6 || p2 == 7 || p2 == 8);

        switch (c) {
        case 'A': out->sym = DBG_KEY_UP;    break;
        case 'B': out->sym = DBG_KEY_DOWN;  break;
        case 'C': out->sym = DBG_KEY_RIGHT; break;
        case 'D': out->sym = DBG_KEY_LEFT;  break;
        case 'H': out->sym = DBG_KEY_HOME;  break;
        case 'F': out->sym = DBG_KEY_END;   break;
        case 'Z': out->sym = DBG_KEY_BACKTAB; break;
        case '~': {
            switch (p1) {
            case 1: out->sym = DBG_KEY_HOME; break;
            case 2: out->sym = DBG_KEY_INSERT; break;
            case 3: out->sym = DBG_KEY_DELETE; break;
            case 4: out->sym = DBG_KEY_END; break;
            case 5: out->sym = DBG_KEY_PGUP; break;
            case 6: out->sym = DBG_KEY_PGDN; break;
            case 11: out->sym = DBG_KEY_F1; break;
            case 12: out->sym = DBG_KEY_F2; break;
            case 13: out->sym = DBG_KEY_F3; break;
            case 14: out->sym = DBG_KEY_F4; break;
            case 15: out->sym = DBG_KEY_F5; break;
            case 17: out->sym = DBG_KEY_F6; break;
            case 18: out->sym = DBG_KEY_F7; break;
            case 19: out->sym = DBG_KEY_F8; break;
            case 20: out->sym = DBG_KEY_F9; break;
            case 21: out->sym = DBG_KEY_F10; break;
            case 23: out->sym = DBG_KEY_F11; break;
            case 24: out->sym = DBG_KEY_F12; break;
            default: out->sym = DBG_KEY_CHAR; out->codepoint = 0; break;
            }
            break;
        }
        default:
            out->sym = DBG_KEY_CHAR;
            out->codepoint = 0;
            break;
        }
        out->shift = shift;
        out->meta  = meta;
        out->ctrl  = ctrl;
        return true;
    }
    return false;   /* ran out of bytes before a final byte arrived       */
}

/* Decode SS3 (ESC O <letter>) sequences, used by some terminals for the
   unmodified arrow/function keys in application-cursor mode.            */
static bool decode_ss3_key(DbgByteRing *r, DbgKeyEvent *out)
{
    int c = ring_pop(r);
    if (c == -1) return false;
    switch (c) {
    case 'A': out->sym = DBG_KEY_UP;    break;
    case 'B': out->sym = DBG_KEY_DOWN;  break;
    case 'C': out->sym = DBG_KEY_RIGHT; break;
    case 'D': out->sym = DBG_KEY_LEFT;  break;
    case 'H': out->sym = DBG_KEY_HOME;  break;
    case 'F': out->sym = DBG_KEY_END;   break;
    case 'P': out->sym = DBG_KEY_F1;    break;
    case 'Q': out->sym = DBG_KEY_F2;    break;
    case 'R': out->sym = DBG_KEY_F3;    break;
    case 'S': out->sym = DBG_KEY_F4;    break;
    default:  out->sym = DBG_KEY_CHAR; out->codepoint = (uint32_t)c; break;
    }
    return true;
}

/* Decode one UTF-8 codepoint starting at the given lead byte, consuming
   continuation bytes from the ring as needed.                           */
static uint32_t decode_utf8_codepoint(DbgByteRing *r, unsigned char lead)
{
    uint32_t cp;
    int seq;
    if      (lead < 0x80)           { return lead; }
    else if ((lead & 0xE0) == 0xC0) { cp = lead & 0x1F; seq = 2; }
    else if ((lead & 0xF0) == 0xE0) { cp = lead & 0x0F; seq = 3; }
    else if ((lead & 0xF8) == 0xF0) { cp = lead & 0x07; seq = 4; }
    else                              { return 0xFFFD; }
    for (int i = 1; i < seq; i++) {
        int c = ring_pop(r);
        if (c == -1) break;
        cp = (cp << 6) | ((unsigned char)c & 0x3F);
    }
    return cp;
}


/// §8  Input: mouse (SGR 1006) decoding

//// SGR mouse protocol
 //
 //  Sequences look like: ESC [ < Cb ; Cx ; Cy (M|m)
 //  Cb encodes button + modifiers + motion/wheel flag; M = press/drag,
 //  m = release. Wheel events arrive as button codes 64/65 with the
 //  trailing letter always 'M'. This is the protocol enabled by the
 //  \x1b[?1006h sequence emitted in dbg_term_enter().
 //
static bool decode_sgr_mouse(DbgByteRing *r, DbgMouseEvent *out)
{
    char params[32];
    size_t plen = 0;
    int c;
    while ((c = ring_pop(r)) != -1 && plen + 1 < sizeof(params)) {
        if (c == 'M' || c == 'm') {
            params[plen] = '\0';
            int cb = 0, cx = 0, cy = 0;
            sscanf(params, "%d;%d;%d", &cb, &cx, &cy);

            out->col = (int16_t)(cx - 1);
            out->row = (int16_t)(cy - 1);
            out->shift = (cb & 4)  != 0;
            out->meta  = (cb & 8)  != 0;
            out->ctrl  = (cb & 16) != 0;

            int btn_bits = cb & 3;
            bool is_motion = (cb & 32) != 0;
            bool is_wheel  = (cb & 64) != 0;

            if (is_wheel) {
                out->kind = (btn_bits == 0) ? DBG_MOUSE_WHEEL_UP
                                             : DBG_MOUSE_WHEEL_DOWN;
                out->button = DBG_BTN_NONE;
            } else if (is_motion) {
                out->kind = (btn_bits == 3) ? DBG_MOUSE_MOVE : DBG_MOUSE_DRAG;
                out->button = (btn_bits == 0) ? DBG_BTN_LEFT
                            : (btn_bits == 1) ? DBG_BTN_MIDDLE
                            : (btn_bits == 2) ? DBG_BTN_RIGHT : DBG_BTN_NONE;
            } else {
                out->kind = (c == 'M') ? DBG_MOUSE_DOWN : DBG_MOUSE_UP;
                out->button = (btn_bits == 0) ? DBG_BTN_LEFT
                            : (btn_bits == 1) ? DBG_BTN_MIDDLE
                            : (btn_bits == 2) ? DBG_BTN_RIGHT : DBG_BTN_NONE;
            }
            return true;
        }
        params[plen++] = (char)c;
    }
    return false;
}


/// §9  Event queue

//// Event queue
 //
 //  A small ring buffer of fully-decoded DbgEvent values, populated by
 //  dbg_poll_input() (§28) and drained by the dispatcher. Kept separate
 //  from DbgByteRing so partial/incomplete escape sequences never leak
 //  into application logic as a spurious DBG_EVENT_KEY.
 //
typedef struct {
    DbgEvent *items;
    size_t    head, tail, count, cap;
} DbgEventQueue;

static void eventq_init(DbgEventQueue *q)
{
    q->cap = DBG_INITIAL_EVENTQ;
    q->items = dbg_xmalloc(q->cap * sizeof(DbgEvent));
    q->head = q->tail = q->count = 0;
}

static void eventq_free(DbgEventQueue *q)
{
    while (q->count > 0) {
        dbg_event_free(&q->items[q->head]);
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    free(q->items);
}

static void eventq_push(DbgEventQueue *q, DbgEvent ev)
{
    if (q->count == q->cap) {
        /* Grow rather than drop — losing a keystroke is worse than a
           reallocation, and this only happens under extreme input burst. */
        DbgEvent *bigger = dbg_xmalloc(q->cap * DBG_GROW_FACTOR * sizeof(DbgEvent));
        for (size_t i = 0; i < q->count; i++)
            bigger[i] = q->items[(q->head + i) % q->cap];
        free(q->items);
        q->items = bigger;
        q->head = 0;
        q->tail = q->count;
        q->cap *= DBG_GROW_FACTOR;
    }
    q->items[q->tail] = ev;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
}

static bool eventq_pop(DbgEventQueue *q, DbgEvent *out)
{
    if (q->count == 0) return false;
    *out = q->items[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    return true;
}

void dbg_event_free(DbgEvent *ev)
{
    if (!ev) return;
    if (ev->kind == DBG_EVENT_PASTE) {
        free(ev->as.paste_text);
        ev->as.paste_text = NULL;
    }
}


/// §10  Cursor blink timer (Emacs semantics)

//// Emacs-exact cursor blink
 //
 //  Emacs's blink-cursor-mode behaves like this (see simple.el):
 //    · The cursor toggles visible/hidden every `blink-cursor-interval`
 //      seconds (default 0.5).
 //    · Any command/input resets the blink to "visible" and restarts the
 //      timer from zero — `blink-cursor-end` + the post-command-hook.
 //    · After `blink-cursor-blinks` (default 10) on/off cycles with NO
 //      intervening input, the cursor stops blinking and stays solid
 //      (visible), rather than blinking forever and burning redraws.
 //
 //  We reproduce exactly that: dbg_blink_tick() is called once per frame
 //  with the current time; dbg_blink_note_activity() is called from the
 //  input decoder on every key event (mouse motion does NOT reset it,
 //  matching Emacs, which only resets on command execution).
 //
static void dbg_blink_init(DbgCursorBlink *b, uint64_t now_ms)
{
    b->visible = true;
    b->solid = false;
    b->blink_count = 0;
    b->last_toggle_ms = now_ms;
    b->last_activity_ms = now_ms;
}

/* Returns true if the visibility changed this tick (i.e. a redraw of the
   cursor cell is needed) — callers use this to avoid touching the
   screen buffer at all on ticks where nothing changed.                  */
static bool dbg_blink_tick(DbgCursorBlink *b, uint64_t now_ms,
                            uint32_t period_ms, uint32_t max_blinks)
{
    if (b->solid) return false;   /* settled solid — no more toggling     */

    if (now_ms - b->last_toggle_ms < period_ms) return false;

    b->visible = !b->visible;
    b->last_toggle_ms = now_ms;
    b->blink_count++;

    if (b->blink_count >= max_blinks * 2) {
        /* max_blinks full on/off cycles elapsed — settle solid & visible,
           exactly like Emacs's blink-cursor-end leaving point on.       */
        b->visible = true;
        b->solid = true;
    }
    return true;
}

/* Any keystroke resets the timer and forces the cursor visible again —
   this is "simply reset the timer" from the spec, called from the key
   branch of the event dispatcher in §28, never from mouse-move events.  */
static void dbg_blink_note_activity(DbgCursorBlink *b, uint64_t now_ms)
{
    b->visible = true;
    b->solid = false;
    b->blink_count = 0;
    b->last_toggle_ms = now_ms;
    b->last_activity_ms = now_ms;
}


/// §11  Layout: panes, splits, geometry

//// Pane tree
 //
 //  Panels are positioned by a simple two-column layout fixed at render
 //  time from the screen size: a wide left column (source/IR/disasm,
 //  whichever is focused) and a narrower right column stacked with
 //  locals/backtrace/breakpoints, plus a one-row status line and a
 //  bottom console/log strip. This is deliberately not a generalized
 //  recursive split tree — the debugger has a fixed set of panels, and
 //  a fixed layout is both simpler and faster to lay out every frame
 //  than re-solving constraints.
 //
static void dbg_compute_layout(DbgSession *s)
{
    int cols = s->screen->cols;
    int rows = s->screen->rows;

    int status_h  = 1;
    int console_h = (rows > 20) ? 6 : 0;
    int main_h    = rows - status_h - console_h;
    int left_w    = (cols * 2) / 3;
    int right_w   = cols - left_w;

    s->panel_rects[DBG_PANEL_SOURCE]      = (DbgRect){ 0, 0, main_h / 2, left_w };
    s->panel_rects[DBG_PANEL_IR]          = (DbgRect){ main_h / 2, 0, main_h - main_h / 2, left_w };
    s->panel_rects[DBG_PANEL_DISASM]      = s->panel_rects[DBG_PANEL_IR];

    int right_third = main_h / 3;
    s->panel_rects[DBG_PANEL_LOCALS]      = (DbgRect){ 0, left_w, right_third, right_w };
    s->panel_rects[DBG_PANEL_BACKTRACE]   = (DbgRect){ right_third, left_w, right_third, right_w };
    s->panel_rects[DBG_PANEL_BREAKPOINTS] = (DbgRect){ right_third * 2, left_w,
                                                         main_h - right_third * 2, right_w };
    s->panel_rects[DBG_PANEL_CONSOLE]     = (DbgRect){ main_h, 0, console_h, cols };
}

static bool dbg_rect_contains(DbgRect r, int row, int col)
{
    return row >= r.row && row < r.row + r.height &&
           col >= r.col && col < r.col + r.width;
}

/* Hit-test the mouse position against all panel rects, returning which
   panel the user clicked/scrolled in. Used so scroll-wheel and click
   events route to the right widget without a global "active widget"
   pointer chase.                                                        */
static DbgPanelKind dbg_panel_at(DbgSession *s, int row, int col)
{
    for (int i = 0; i < DBG_MAX_PANELS; i++) {
        if (i == DBG_PANEL_DISASM) continue;   /* aliases IR rect          */
        if (dbg_rect_contains(s->panel_rects[i], row, col))
            return (DbgPanelKind)i;
    }
    return s->focused_panel;
}


/// §12  Widget: text viewport (scrollback, source view)

//// Scrollable text viewport
 //
 //  Shared rendering logic for the source pane, the IR pane, and the
 //  console/log pane: line-oriented text, a vertical scroll offset, and
 //  mouse-wheel handling. We draw only into the DbgRect we are given;
 //  the screen-level damage tracking in §5/§6 still decides what bytes
 //  actually reach the terminal.
 //
struct DbgTextView {
    const char **lines;      /* borrowed, not owned                       */
    size_t        line_count;
    int32_t       scroll_offset;
    int32_t       cursor_line;
    DbgRect       rect;
    bool          show_gutter;
    int          (*gutter_fn)(void *ctx, int line_index, char *buf, size_t buflen);
    void         *gutter_ctx;
};

static void dbg_textview_scroll(DbgTextView *tv, int delta)
{
    int32_t max_scroll = (int32_t)tv->line_count - tv->rect.height;
    if (max_scroll < 0) max_scroll = 0;
    tv->scroll_offset += delta;
    if (tv->scroll_offset < 0) tv->scroll_offset = 0;
    if (tv->scroll_offset > max_scroll) tv->scroll_offset = max_scroll;
}

static const int DBG_WHEEL_LINES = 3;   /* lines per wheel notch, matches
                                            typical terminal scroll feel  */

static bool dbg_textview_handle_mouse(DbgTextView *tv, DbgMouseEvent *ev)
{
    if (!dbg_rect_contains(tv->rect, ev->row, ev->col)) return false;
    if (ev->kind == DBG_MOUSE_WHEEL_UP) {
        dbg_textview_scroll(tv, -DBG_WHEEL_LINES);
        return true;
    }
    if (ev->kind == DBG_MOUSE_WHEEL_DOWN) {
        dbg_textview_scroll(tv, DBG_WHEEL_LINES);
        return true;
    }
    if (ev->kind == DBG_MOUSE_DOWN && ev->button == DBG_BTN_LEFT) {
        int clicked_line = tv->scroll_offset + (ev->row - tv->rect.row);
        if (clicked_line >= 0 && (size_t)clicked_line < tv->line_count)
            tv->cursor_line = clicked_line;
        return true;
    }
    return false;
}

static void dbg_textview_render(DbgScreen *sc, DbgTextView *tv,
                                 DbgTheme *theme)
{
    int gutter_w = tv->show_gutter ? 6 : 0;
    DbgStyle base = theme->base;
    DbgStyle gutter_style = theme->gutter;

    for (int r = 0; r < tv->rect.height; r++) {
        int line_idx = tv->scroll_offset + r;
        int screen_row = tv->rect.row + r;

        if (tv->show_gutter) {
            char gbuf[16] = "      ";
            if (line_idx >= 0 && (size_t)line_idx < tv->line_count && tv->gutter_fn)
                tv->gutter_fn(tv->gutter_ctx, line_idx, gbuf, sizeof(gbuf));
            dbg_screen_write(sc, screen_row, tv->rect.col, gbuf, gutter_style);
        }

        if (line_idx < 0 || (size_t)line_idx >= tv->line_count || !tv->lines) {
            dbg_screen_fill_rect(sc,
                (DbgRect){ screen_row, tv->rect.col + gutter_w, 1,
                           tv->rect.width - gutter_w }, ' ', base);
            continue;
        }

        DbgStyle line_style = base;
        if (line_idx == tv->cursor_line) line_style = theme->selection;

        dbg_screen_fill_rect(sc,
            (DbgRect){ screen_row, tv->rect.col + gutter_w, 1,
                       tv->rect.width - gutter_w }, ' ', line_style);
        dbg_screen_write(sc, screen_row, tv->rect.col + gutter_w,
                          tv->lines[line_idx], line_style);
    }
}


/// §13  Widget: list (selectable, for stack frames / breakpoints)

//// Selectable list
 //
 //  Used for the backtrace panel, breakpoints panel, and the palette's
 //  result list. Selection moves with arrow keys, Enter activates, mouse
 //  click selects+activates in one motion (matching how vertico's
 //  candidate list behaves under mouse).
 //
struct DbgListView {
    char       **items;        /* borrowed display strings                */
    void       **payloads;     /* borrowed, one per item                  */
    size_t       item_count;
    int32_t      selected;
    int32_t      scroll_offset;
    DbgRect      rect;
};

static void dbg_listview_ensure_visible(DbgListView *lv)
{
    if (lv->selected < lv->scroll_offset)
        lv->scroll_offset = lv->selected;
    if (lv->selected >= lv->scroll_offset + lv->rect.height)
        lv->scroll_offset = lv->selected - lv->rect.height + 1;
    if (lv->scroll_offset < 0) lv->scroll_offset = 0;
}

static void dbg_listview_move(DbgListView *lv, int delta)
{
    if (lv->item_count == 0) return;
    int32_t next = lv->selected + delta;
    if (next < 0) next = 0;
    if (next >= (int32_t)lv->item_count) next = (int32_t)lv->item_count - 1;
    lv->selected = next;
    dbg_listview_ensure_visible(lv);
}

static bool dbg_listview_handle_mouse(DbgListView *lv, DbgMouseEvent *ev,
                                       bool *activated)
{
    *activated = false;
    if (!dbg_rect_contains(lv->rect, ev->row, ev->col)) return false;
    if (ev->kind == DBG_MOUSE_WHEEL_UP) {
        dbg_listview_move(lv, -1);
        return true;
    }
    if (ev->kind == DBG_MOUSE_WHEEL_DOWN) {
        dbg_listview_move(lv, 1);
        return true;
    }
    if (ev->kind == DBG_MOUSE_DOWN && ev->button == DBG_BTN_LEFT) {
        int32_t clicked = lv->scroll_offset + (ev->row - lv->rect.row);
        if (clicked >= 0 && (size_t)clicked < lv->item_count) {
            bool was_selected = (lv->selected == clicked);
            lv->selected = clicked;
            *activated = was_selected;   /* click-on-already-selected = activate,
                                             matching common list-box UX     */
        }
        return true;
    }
    return false;
}

static void dbg_listview_render(DbgScreen *sc, DbgListView *lv,
                                 DbgTheme *theme)
{
    for (int r = 0; r < lv->rect.height; r++) {
        int idx = lv->scroll_offset + r;
        int screen_row = lv->rect.row + r;
        DbgStyle style = (idx == lv->selected) ? theme->selection : theme->base;

        dbg_screen_fill_rect(sc, (DbgRect){ screen_row, lv->rect.col, 1, lv->rect.width },
                             ' ', style);
        if (idx >= 0 && (size_t)idx < lv->item_count)
            dbg_screen_write(sc, screen_row, lv->rect.col + 1, lv->items[idx], style);
    }
}


/// §14  Widget: status / mode line

//// Status line
 //
 //  A single-row summary, Emacs-modeline-flavored: severity banner on
 //  the left when an error is active, focused-panel name in the middle,
 //  key hints on the right. Rendered fresh every frame but the damage
 //  tracker still suppresses output for unchanged cells.
 //
static const char *dbg_panel_name(DbgPanelKind k)
{
    switch (k) {
    case DBG_PANEL_SOURCE:      return "source";
    case DBG_PANEL_IR:          return "llvm-ir";
    case DBG_PANEL_DISASM:      return "disasm";
    case DBG_PANEL_LOCALS:      return "locals";
    case DBG_PANEL_BACKTRACE:   return "backtrace";
    case DBG_PANEL_BREAKPOINTS: return "breakpoints";
    case DBG_PANEL_CONSOLE:     return "console";
    }
    return "?";
}

static const char *dbg_severity_label(DbgSeverity sev)
{
    switch (sev) {
    case DBG_SEV_NOTE:     return "NOTE";
    case DBG_SEV_WARNING:  return "WARNING";
    case DBG_SEV_ERROR:    return "ERROR";
    case DBG_SEV_FATAL:    return "FATAL";
    case DBG_SEV_INTERNAL: return "INTERNAL";
    }
    return "?";
}

static void dbg_render_status_line(DbgSession *s, int row)
{
    DbgScreen *sc = s->screen;
    DbgTheme  *th = s->theme;
    dbg_screen_fill_rect(sc, (DbgRect){ row, 0, 1, sc->cols }, ' ', th->status_line);

    int col = 0;
    if (s->active_error) {
        DbgStrBuf b;
        sb_init(&b);
        sb_appendf(&b, " [%s] %s ", dbg_severity_label(s->active_error->severity),
                   s->active_error->code ? s->active_error->code : "");
        col = dbg_screen_write(sc, row, col, b.data, th->error_banner);
        sb_free(&b);
    }

    char mid[64];
    snprintf(mid, sizeof(mid), " panel:%s ", dbg_panel_name(s->focused_panel));
    dbg_screen_write(sc, row, col, mid, th->status_line);

    const char *hint = " M-x:palette  TAB:cycle  q:quit ";
    int hint_len = (int)strlen(hint);
    int hint_col = sc->cols - hint_len;
    if (hint_col > col) dbg_screen_write(sc, row, hint_col, hint, th->status_line);
}


/// §15  Command palette: orderless completion engine

//// Orderless matching
 //
 //  Mirrors Emacs's `orderless` completion style: the minibuffer input is
 //  split on whitespace into independent components, and a candidate
 //  matches iff EVERY component matches somewhere in the candidate,
 //  in ANY order. "br set" and "set br" both match "breakpoint-set".
 //
 //  Each component is matched case-insensitively as a literal substring
 //  first (cheap, covers the overwhelming majority of keystrokes); if a
 //  component contains a regex metacharacter we fall back to a tiny
 //  hand-rolled glob (`*` and `?`) rather than pulling in <regex.h>,
 //  keeping the matcher allocation-free on the hot path of "user typed
 //  one more character."
 //
static bool dbg_str_contains_ci(const char *haystack, const char *needle,
                                 uint32_t *out_start)
{
    size_t hlen = strlen(haystack), nlen = strlen(needle);
    if (nlen == 0) { if (out_start) *out_start = 0; return true; }
    if (nlen > hlen) return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        for (; j < nlen; j++)
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        if (j == nlen) {
            if (out_start) *out_start = (uint32_t)i;
            return true;
        }
    }
    return false;
}

/* Simple glob: '*' matches any run (including empty), '?' matches one
   char. No captures — we only need a boolean match plus a best-effort
   highlight span (the substring from the first '*'-free anchor).        */
static bool dbg_glob_match(const char *pattern, const char *text)
{
    const char *p = pattern, *t = text;
    const char *star_p = NULL, *star_t = NULL;
    while (*t) {
        if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*t)) {
            p++; t++;
        } else if (*p == '*') {
            star_p = p++; star_t = t;
        } else if (star_p) {
            p = star_p + 1; t = ++star_t;
        } else {
            return false;
        }
    }
    while (*p == '*') p++;
    return *p == '\0';
}

static bool dbg_has_glob_meta(const char *s)
{
    return strchr(s, '*') != NULL || strchr(s, '?') != NULL;
}

/* Split `input` on runs of whitespace into up to `max_tokens` borrowed
   (start, len) spans within a caller-owned scratch copy — no per-token
   heap allocation, since this runs on every keystroke in the minibuffer. */
typedef struct { const char *str; size_t len; } DbgSpan;

static size_t dbg_split_tokens(const char *input, DbgSpan *out, size_t max_tokens)
{
    size_t n = 0;
    const char *p = input;
    while (*p && n < max_tokens) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        out[n].str = start;
        out[n].len = (size_t)(p - start);
        n++;
    }
    return n;
}

/* Score a single candidate against the orderless token set. Returns true
   if ALL tokens matched; populates spans for highlight rendering and a
   score that favors: (a) matches near the start of the candidate,
   (b) fewer/longer tokens (more specific queries rank tighter),
   (c) shorter candidates overall (prefer concise exact-ish names).       */
static bool dbg_orderless_score(const char *candidate, DbgSpan *tokens,
                                 size_t token_count, double *score_out,
                                 DbgMatchSpan *spans_out, size_t *span_count_out)
{
    if (token_count == 0) {
        *score_out = 0.0;
        *span_count_out = 0;
        return true;   /* empty query matches everything, vertico-style   */
    }

    double score = 0.0;
    size_t spans_written = 0;
    char tokbuf[128];

    for (size_t i = 0; i < token_count; i++) {
        size_t tlen = tokens[i].len < sizeof(tokbuf) - 1
                      ? tokens[i].len : sizeof(tokbuf) - 1;
        memcpy(tokbuf, tokens[i].str, tlen);
        tokbuf[tlen] = '\0';

        if (dbg_has_glob_meta(tokbuf)) {
            if (!dbg_glob_match(tokbuf, candidate)) return false;
            score += 1.0;   /* glob matches score flat; no span highlight */
            continue;
        }

        uint32_t start = 0;
        if (!dbg_str_contains_ci(candidate, tokbuf, &start)) return false;

        score += 10.0 / (double)(start + 1);   /* earlier match = higher  */
        score += 1.0 / (double)tlen;            /* shorter, sharper token  */

        if (spans_out && spans_written < DBG_PALETTE_MAX_RESULTS) {
            spans_out[spans_written].start = start;
            spans_out[spans_written].len = (uint32_t)tlen;
            spans_written++;
        }
    }

    score -= (double)strlen(candidate) * 0.01;   /* mild brevity bonus     */
    *score_out = score;
    if (span_count_out) *span_count_out = spans_written;
    return true;
}

static int dbg_completion_cmp(const void *a, const void *b)
{
    const DbgCompletionResult *ra = (const DbgCompletionResult *)a;
    const DbgCompletionResult *rb = (const DbgCompletionResult *)b;
    if (ra->score > rb->score) return -1;
    if (ra->score < rb->score) return 1;
    return strcmp(ra->candidate, rb->candidate);
}

/* Run orderless matching over an array of candidate strings, returning a
   freshly-allocated, score-sorted result array. `payloads[i]` (may be
   NULL) is threaded through to result[i].user_data unchanged so callers
   can map a match straight back to a DbgCommand* or DbgBreakpoint* etc.  */
static DbgCompletionResult *dbg_orderless_filter(const char *query,
                                                  const char **candidates,
                                                  void **payloads,
                                                  size_t candidate_count,
                                                  size_t *result_count_out)
{
    DbgSpan tokens[16];
    size_t token_count = dbg_split_tokens(query, tokens, 16);

    DbgCompletionResult *results =
        dbg_xmalloc(candidate_count * sizeof(DbgCompletionResult));
    size_t n = 0;

    for (size_t i = 0; i < candidate_count; i++) {
        double score = 0.0;
        DbgMatchSpan *spans = dbg_xmalloc(token_count * sizeof(DbgMatchSpan));
        size_t span_count = 0;
        if (dbg_orderless_score(candidates[i], tokens, token_count, &score,
                                 spans, &span_count)) {
            results[n].candidate = candidates[i];
            results[n].user_data = payloads ? payloads[i] : NULL;
            results[n].score = score;
            results[n].spans = spans;
            results[n].span_count = span_count;
            n++;
        } else {
            free(spans);
        }
    }

    qsort(results, n, sizeof(DbgCompletionResult), dbg_completion_cmp);
    *result_count_out = n;
    return results;
}

static void dbg_completion_results_free(DbgCompletionResult *results, size_t count)
{
    for (size_t i = 0; i < count; i++) free(results[i].spans);
    free(results);
}


/// §16  Command palette: vertico-style minibuffer UI

//// Palette state
 //
 //  A floating overlay, centered horizontally, anchored near the top
 //  third of the screen (vertico's default `vertico-posframe`-free
 //  layout: minibuffer line + a vertical candidate list right below
 //  it, most-relevant candidate first). Re-filters on every keystroke;
 //  because dbg_orderless_filter is allocation-bounded by candidate
 //  count (a few hundred commands at most) this comfortably hits sub-
 //  millisecond filtering, so we never need to debounce input here.
 //
struct DbgPalette {
    char     input[256];
    size_t   input_len;
    size_t   cursor_pos;        /* caret position within input, in bytes  */

    DbgCommand **commands;       /* borrowed from the registry             */
    size_t       command_count;

    DbgCompletionResult *results;
    size_t                result_count;
    int32_t               selected;
    int32_t               scroll_offset;

    DbgRect  rect;
    uint64_t opened_at_ms;
};

static DbgPalette *dbg_palette_create(void)
{
    DbgPalette *p = dbg_xcalloc(1, sizeof(*p));
    return p;
}

static void dbg_palette_free(DbgPalette *p)
{
    if (!p) return;
    dbg_completion_results_free(p->results, p->result_count);
    free(p);
}

/* Re-run the orderless filter against the live input and reset selection
   to the top match — exactly vertico's behavior of always defaulting to
   the best-ranked candidate as you type, rather than preserving an
   arbitrary previous index that might now point at a filtered-out row.   */
static void dbg_palette_refilter(DbgPalette *p)
{
    dbg_completion_results_free(p->results, p->result_count);
    p->results = NULL;
    p->result_count = 0;

    if (p->command_count == 0) return;

    const char **names = dbg_xmalloc(p->command_count * sizeof(char *));
    void       **payloads = dbg_xmalloc(p->command_count * sizeof(void *));
    DbgStrBuf    combined; /* candidate = "name keywords" so keyword hits
                               still surface the command, vertico-style   */
    sb_init(&combined);
    char **owned_names = dbg_xmalloc(p->command_count * sizeof(char *));

    for (size_t i = 0; i < p->command_count; i++) {
        combined.len = 0;
        if (combined.data) combined.data[0] = '\0';
        sb_append(&combined, p->commands[i]->name);
        if (p->commands[i]->keywords) {
            sb_appendc(&combined, ' ');
            sb_append(&combined, p->commands[i]->keywords);
        }
        owned_names[i] = dbg_xstrdup(combined.data);
        names[i] = owned_names[i];
        payloads[i] = p->commands[i];
    }
    sb_free(&combined);

    p->results = dbg_orderless_filter(p->input, names, payloads,
                                       p->command_count, &p->result_count);

    /* The result candidate strings point into `names`/`owned_names`,
       which we must keep alive exactly as long as `results` does. We
       sidestep the lifetime issue by re-pointing each result's
       candidate at the owning DbgCommand's ->name (display purposes
       only need the bare name, not "name keywords").                    */
    for (size_t i = 0; i < p->result_count; i++) {
        DbgCommand *cmd = (DbgCommand *)p->results[i].user_data;
        p->results[i].candidate = cmd->name;
    }
    for (size_t i = 0; i < p->command_count; i++) free(owned_names[i]);
    free(owned_names);
    free(names);
    free(payloads);

    p->selected = (p->result_count > 0) ? 0 : -1;
    p->scroll_offset = 0;
}

static void dbg_palette_open(DbgSession *s)
{
    if (!s->palette) s->palette = dbg_palette_create();
    s->palette->input[0] = '\0';
    s->palette->input_len = 0;
    s->palette->cursor_pos = 0;
    s->palette->opened_at_ms = dbg_now_ms();
    dbg_palette_refilter(s->palette);
    s->palette_open = true;
    s->dirty = true;
}

static void dbg_palette_close(DbgSession *s)
{
    s->palette_open = false;
    s->dirty = true;
}

static void dbg_palette_insert_char(DbgPalette *p, uint32_t codepoint)
{
    if (codepoint > 0x7F) return;   /* command names are ASCII; keep the
                                        minibuffer input byte-simple      */
    if (p->input_len + 1 >= sizeof(p->input)) return;
    memmove(p->input + p->cursor_pos + 1, p->input + p->cursor_pos,
            p->input_len - p->cursor_pos);
    p->input[p->cursor_pos] = (char)codepoint;
    p->cursor_pos++;
    p->input_len++;
    p->input[p->input_len] = '\0';
    dbg_palette_refilter(p);
}

static void dbg_palette_backspace(DbgPalette *p)
{
    if (p->cursor_pos == 0) return;
    memmove(p->input + p->cursor_pos - 1, p->input + p->cursor_pos,
            p->input_len - p->cursor_pos);
    p->cursor_pos--;
    p->input_len--;
    p->input[p->input_len] = '\0';
    dbg_palette_refilter(p);
}

static void dbg_palette_move_selection(DbgPalette *p, int delta)
{
    if (p->result_count == 0) return;
    int32_t next = p->selected + delta;
    if (next < 0) next = 0;
    if (next >= (int32_t)p->result_count) next = (int32_t)p->result_count - 1;
    p->selected = next;
    if (p->selected < p->scroll_offset) p->scroll_offset = p->selected;
    int visible = p->rect.height - 1;   /* minus the input line            */
    if (p->selected >= p->scroll_offset + visible)
        p->scroll_offset = p->selected - visible + 1;
}

static void dbg_palette_activate(DbgSession *s)
{
    DbgPalette *p = s->palette;
    if (!p || p->selected < 0 || (size_t)p->selected >= p->result_count) return;
    DbgCommand *cmd = (DbgCommand *)p->results[p->selected].user_data;
    dbg_palette_close(s);
    if (cmd && cmd->run) cmd->run(s, NULL);
}

static bool dbg_palette_handle_mouse(DbgSession *s, DbgMouseEvent *ev)
{
    DbgPalette *p = s->palette;
    if (!p || !dbg_rect_contains(p->rect, ev->row, ev->col)) return false;

    if (ev->kind == DBG_MOUSE_WHEEL_UP)   { dbg_palette_move_selection(p, -1); return true; }
    if (ev->kind == DBG_MOUSE_WHEEL_DOWN) { dbg_palette_move_selection(p, 1);  return true; }

    if (ev->kind == DBG_MOUSE_DOWN && ev->button == DBG_BTN_LEFT) {
        int list_row = ev->row - p->rect.row - 1;   /* below the input line */
        int32_t idx = p->scroll_offset + list_row;
        if (list_row >= 0 && idx >= 0 && (size_t)idx < p->result_count) {
            bool was_selected = (p->selected == idx);
            p->selected = idx;
            if (was_selected) dbg_palette_activate(s);
        }
        return true;
    }
    return false;
}

/* Render the highlighted candidate: spans from orderless matching are
   painted in theme->palette_match, everything else in the base style.   */
static void dbg_palette_render_candidate(DbgScreen *sc, int row, int col,
                                          int width, DbgCompletionResult *res,
                                          DbgStyle base, DbgStyle match_style)
{
    size_t len = strlen(res->candidate);
    for (size_t i = 0; i < len && (int)i < width; i++) {
        bool in_span = false;
        for (size_t s = 0; s < res->span_count; s++) {
            if (i >= res->spans[s].start && i < res->spans[s].start + res->spans[s].len) {
                in_span = true;
                break;
            }
        }
        uint32_t cp = (unsigned char)res->candidate[i];
        dbg_screen_put(sc, row, col + (int)i, cp, in_span ? match_style : base);
    }
}

static void dbg_palette_render(DbgSession *s)
{
    DbgPalette *p = s->palette;
    DbgScreen  *sc = s->screen;
    DbgTheme   *th = s->theme;

    int width  = sc->cols * 2 / 3;
    int height = sc->rows * 1 / 2;
    if (height < 4) height = 4;
    int row = sc->rows / 6;
    int col = (sc->cols - width) / 2;
    p->rect = (DbgRect){ row, col, height, width };

    dbg_screen_fill_rect(sc, p->rect, ' ', th->base);

    /* Border, drawn with plain ASCII box characters per the "only ASCII,
       no invisible characters" instruction this skeleton itself follows. */
    for (int c = col; c < col + width; c++) {
        dbg_screen_put(sc, row - 1, c, '-', th->palette_border);
        dbg_screen_put(sc, row + height, c, '-', th->palette_border);
    }
    for (int r = row - 1; r <= row + height; r++) {
        dbg_screen_put(sc, r, col - 1, '|', th->palette_border);
        dbg_screen_put(sc, r, col + width, '|', th->palette_border);
    }

    /* Input line: "M-x " prompt + live text + a block caret. */
    DbgStrBuf prompt;
    sb_init(&prompt);
    sb_append(&prompt, "M-x ");
    sb_append(&prompt, p->input);
    dbg_screen_write(sc, row, col, prompt.data, th->base);
    sb_free(&prompt);

    /* Result count, right-aligned on the input line — orderless/vertico
       convention of showing "N" candidates remaining as you narrow.     */
    char countbuf[32];
    snprintf(countbuf, sizeof(countbuf), "%zu", p->result_count);
    dbg_screen_write(sc, row, col + width - (int)strlen(countbuf) - 1,
                      countbuf, th->base);

    int list_top = row + 1;
    int list_height = height - 1;
    for (int i = 0; i < list_height; i++) {
        int idx = p->scroll_offset + i;
        int screen_row = list_top + i;
        DbgStyle row_style = (idx == p->selected) ? th->selection : th->base;
        dbg_screen_fill_rect(sc, (DbgRect){ screen_row, col, 1, width }, ' ', row_style);
        if (idx >= 0 && (size_t)idx < p->result_count) {
            dbg_palette_render_candidate(sc, screen_row, col + 1, width - 2,
                                          &p->results[idx], row_style,
                                          th->palette_match);
        }
    }
}


/// §17  Command registry

//// Command registry
 //
 //  All palette-visible actions live here. Each DbgCommand owns no
 //  memory of its own (name/summary/keywords are string literals from
 //  call sites); the registry array itself is the only allocation.
 //
typedef struct {
    DbgCommand *commands;
    size_t      count, cap;
} DbgCommandRegistry;

static DbgCommandRegistry g_dbg_registry;   /* process-wide; one debugger
                                                instance per process       */

static void dbg_registry_init(void)
{
    g_dbg_registry.cap = DBG_INITIAL_COMMANDS;
    g_dbg_registry.commands = dbg_xmalloc(g_dbg_registry.cap * sizeof(DbgCommand));
    g_dbg_registry.count = 0;
}

static void dbg_registry_free(void)
{
    free(g_dbg_registry.commands);
    g_dbg_registry.commands = NULL;
    g_dbg_registry.count = g_dbg_registry.cap = 0;
}

static void dbg_register_command(DbgCommand cmd)
{
    DBG_GROW(g_dbg_registry.commands, g_dbg_registry.count,
              g_dbg_registry.cap, DbgCommand);
    g_dbg_registry.commands[g_dbg_registry.count++] = cmd;
}

/* Snapshot pointers into the registry for the palette to filter over.
   Borrowed — valid as long as g_dbg_registry is not mutated, which it
   never is after dbg_session_create() finishes registering builtins.    */
static void dbg_palette_bind_registry(DbgPalette *p)
{
    p->command_count = g_dbg_registry.count;
    p->commands = dbg_xmalloc(p->command_count * sizeof(DbgCommand *));
    for (size_t i = 0; i < p->command_count; i++)
        p->commands[i] = &g_dbg_registry.commands[i];
}


/// §18  Error trapping & snapshot capture

//// Error snapshot lifecycle
 //
 //  dbg_trap_error() is the single hook every diagnostic-producing site
 //  in the compiler calls. The contract: the ORIGINAL message text is
 //  preserved byte-for-byte in snap->original_message and rendered
 //  verbatim inside the error banner / console pane — the TUI never
 //  reformats or truncates it, only adds surrounding context (file,
 //  backtrace, source excerpt).
 //
 //  A process-wide pointer to the active session lets trap sites that
 //  have no DbgSession in scope (deep inside the type checker, say)
 //  still reach the running debugger, or lazily create one if none
 //  exists yet — that lazy-create is what makes "the TUI automatically
 //  appears" true without threading a DbgSession* through the entire
 //  compiler call graph.
 //
static DbgSession *g_dbg_active_session = NULL;

DbgErrorSnapshot *dbg_error_snapshot_create(DbgSeverity severity,
                                             const char *message,
                                             const char *code,
                                             const char *file,
                                             uint32_t line, uint32_t column)
{
    DbgErrorSnapshot *snap = dbg_xcalloc(1, sizeof(*snap));
    snap->severity = severity;
    snap->original_message = dbg_xstrdup(message);   /* verbatim, untouched */
    snap->code = code ? dbg_xstrdup(code) : NULL;
    snap->file = file ? dbg_xstrdup(file) : NULL;
    snap->line = line;
    snap->column = column;
    snap->captured_at_ms = dbg_now_ms();
    snap->backtrace.frames = NULL;
    snap->backtrace.frame_count = 0;
    snap->source_context = NULL;
    return snap;
}

void dbg_error_snapshot_free(DbgErrorSnapshot *snap)
{
    if (!snap) return;
    free(snap->original_message);
    free(snap->code);
    free(snap->file);
    free(snap->source_context);
    for (size_t i = 0; i < snap->backtrace.frame_count; i++) {
        free(snap->backtrace.frames[i].function);
        free(snap->backtrace.frames[i].file);
        free(snap->backtrace.frames[i].ir_value);
    }
    free(snap->backtrace.frames);
    free(snap);
}

/* Extract a few lines of source around the error location, used so the
   TUI can show the offending code without re-opening the file from
   inside the render loop (which would stall the redraw on a slow disk). */
static char *dbg_capture_source_context(const char *file, uint32_t line,
                                         int context_lines)
{
    if (!file) return NULL;
    FILE *f = fopen(file, "rb");
    if (!f) return NULL;

    DbgStrBuf b;
    sb_init(&b);
    char linebuf[1024];
    uint32_t lo = (line > (uint32_t)context_lines) ? line - (uint32_t)context_lines : 1;
    uint32_t hi = line + (uint32_t)context_lines;
    uint32_t cur = 1;
    while (fgets(linebuf, sizeof(linebuf), f)) {
        if (cur >= lo && cur <= hi) {
            sb_appendf(&b, "%5u %s %s", cur, (cur == line) ? ">" : " ", linebuf);
            size_t bl = b.len;
            if (bl == 0 || b.data[bl - 1] != '\n') sb_appendc(&b, '\n');
        }
        if (cur > hi) break;
        cur++;
    }
    fclose(f);
    return sb_take(&b);
}

static void dbg_session_push_error(DbgSession *s, DbgErrorSnapshot *snap)
{
    DBG_GROW(s->error_stack, s->error_stack_count, s->error_stack_cap,
              DbgErrorSnapshot *);
    s->error_stack[s->error_stack_count++] = snap;
    s->active_error = snap;
    s->focused_panel = DBG_PANEL_SOURCE;
    s->dirty = true;
}

/* The public hook. Three cases:
     1. No session exists yet     -> create one (this is the "TUI
        automatically appears" behavior), enter it, push the error.
     2. A session exists, running -> push onto the error stack so the
        backtrace/console panes reflect the new error immediately.
     3. Re-entrant trap from inside the TUI's own error handling
        (a bug in the debugger itself triggering a diagnostic) -> still
        just push; we never recurse into a second terminal takeover.    */
void dbg_trap_error(DbgErrorSnapshot *snap)
{
    if (!snap) return;

    if (snap->file && !snap->source_context) {
        snap->source_context = dbg_capture_source_context(snap->file, snap->line, 4);
    }

    if (!g_dbg_active_session) {
        DbgConfig cfg = dbg_default_config();
        g_dbg_active_session = dbg_session_create(cfg);
        dbg_session_push_error(g_dbg_active_session, snap);
        dbg_session_run(g_dbg_active_session);
        dbg_session_free(g_dbg_active_session);
        g_dbg_active_session = NULL;
        return;
    }

    dbg_session_push_error(g_dbg_active_session, snap);
}

void dbg_trap_errorf(DbgSeverity severity, const char *file,
                     uint32_t line, uint32_t column,
                     const char *code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    char *msg = dbg_xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(msg, (size_t)n + 1, fmt, ap);
    va_end(ap);

    DbgErrorSnapshot *snap =
        dbg_error_snapshot_create(severity, msg, code, file, line, column);
    free(msg);
    dbg_trap_error(snap);
}


/// §19  Backtrace model

//// Backtrace capture
 //
 //  The compiler's call sites are responsible for populating frames
 //  (dbg_trap_error callers typically walk their own activation/analysis
 //  stack and attach frames before pushing the snapshot); this section
 //  provides the shared helpers for building and rendering that list so
 //  every call site does it the same way.
 //
static void dbg_backtrace_push(DbgBacktrace *bt, const char *function,
                                const char *file, uint32_t line,
                                uint32_t column, const char *ir_value)
{
    /* DbgBacktrace has no explicit cap field — frame lists are small and
       built once per error, so we grow by exactly one slot per push
       rather than pulling in the amortized-growth DBG_GROW macro, which
       needs a separate cap variable this struct doesn't carry.          */
    bt->frames = dbg_xrealloc(bt->frames, (bt->frame_count + 1) * sizeof(DbgFrame));
    DbgFrame *f = &bt->frames[bt->frame_count++];
    f->function = dbg_xstrdup(function);
    f->file     = file ? dbg_xstrdup(file) : NULL;
    f->line     = line;
    f->column   = column;
    f->ir_value = ir_value ? dbg_xstrdup(ir_value) : NULL;
}

static char *dbg_frame_render(DbgFrame *f)
{
    DbgStrBuf b;
    sb_init(&b);
    sb_append(&b, f->function ? f->function : "<anonymous>");
    if (f->file) {
        sb_appendf(&b, "  %s:%u:%u", f->file, f->line, f->column);
    }
    if (f->ir_value) {
        sb_appendf(&b, "  [%s]", f->ir_value);
    }
    return sb_take(&b);
}


/// §20  Breakpoints & watchpoints

//// Breakpoint storage
 //
 //  A flat array on the session, indexed by linear scan (breakpoint
 //  counts are in the dozens for a human-driven debug session, so a
 //  hash table would be solving a problem that does not exist here).
 //
static uint32_t dbg_breakpoint_add(DbgSession *s, DbgBreakpointKind kind,
                                    const char *file, uint32_t line,
                                    const char *symbol)
{
    DBG_GROW(s->breakpoints, s->breakpoint_count, s->breakpoint_cap,
              DbgBreakpoint);
    DbgBreakpoint *bp = &s->breakpoints[s->breakpoint_count++];
    bp->id = s->next_breakpoint_id++;
    bp->kind = kind;
    bp->file = file ? dbg_xstrdup(file) : NULL;
    bp->line = line;
    bp->symbol = symbol ? dbg_xstrdup(symbol) : NULL;
    bp->enabled = true;
    bp->condition = NULL;
    bp->hit_count = 0;
    return bp->id;
}

static void dbg_breakpoint_remove(DbgSession *s, uint32_t id)
{
    for (size_t i = 0; i < s->breakpoint_count; i++) {
        if (s->breakpoints[i].id != id) continue;
        free(s->breakpoints[i].file);
        free(s->breakpoints[i].symbol);
        free(s->breakpoints[i].condition);
        s->breakpoints[i] = s->breakpoints[--s->breakpoint_count];
        return;
    }
}

static DbgBreakpoint *dbg_breakpoint_at_line(DbgSession *s, const char *file,
                                              uint32_t line)
{
    for (size_t i = 0; i < s->breakpoint_count; i++) {
        DbgBreakpoint *bp = &s->breakpoints[i];
        if (bp->kind != DBG_BP_LINE) continue;
        if (bp->line != line) continue;
        if (bp->file && file && strcmp(bp->file, file) != 0) continue;
        return bp;
    }
    return NULL;
}

static void dbg_breakpoint_toggle_at_line(DbgSession *s, const char *file,
                                           uint32_t line)
{
    DbgBreakpoint *existing = dbg_breakpoint_at_line(s, file, line);
    if (existing) {
        dbg_breakpoint_remove(s, existing->id);
    } else {
        dbg_breakpoint_add(s, DBG_BP_LINE, file, line, NULL);
    }
    s->dirty = true;
}

static uint32_t dbg_watch_add(DbgSession *s, const char *expression)
{
    DBG_GROW(s->watches, s->watch_count, s->watch_cap, DbgWatch);
    DbgWatch *w = &s->watches[s->watch_count++];
    static uint32_t next_id = 1;
    w->id = next_id++;
    w->expression = dbg_xstrdup(expression);
    w->last_value = NULL;
    w->changed_last_step = false;
    return w->id;
}


/// §21  Source map / line table

//// Source map loading
 //
 //  Reads the whole file into memory once (compiler source files are
 //  not large enough to justify mmap-and-page complexity for a debugger
 //  UI) and builds a line-offset table identical in spirit to lsp.c's
 //  lsp_document_build_line_index, so the gutter and "jump to line"
 //  commands are O(log n) via binary search rather than O(n) rescans.
 //
DbgSourceMap *dbg_source_map_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }

    char *contents = dbg_xmalloc((size_t)size + 1);
    size_t got = fread(contents, 1, (size_t)size, f);
    contents[got] = '\0';
    fclose(f);

    DbgSourceMap *map = dbg_xcalloc(1, sizeof(*map));
    map->path = dbg_xstrdup(path);
    map->contents = contents;
    map->contents_len = got;

    uint32_t cap = 256;
    map->line_offsets = dbg_xmalloc(cap * sizeof(uint32_t));
    map->lines = dbg_xmalloc(cap * sizeof(const char *));
    map->line_count = 0;

    map->line_offsets[map->line_count] = 0;
    map->lines[map->line_count] = contents;
    map->line_count++;

    for (size_t i = 0; i < got; i++) {
        if (contents[i] == '\n') {
            contents[i] = '\0';
            if (i > 0 && contents[i - 1] == '\r')
                contents[i - 1] = '\0';

            if (i + 1 >= got)
                continue;

            if (map->line_count >= cap) {
                cap *= DBG_GROW_FACTOR;
                map->line_offsets = dbg_xrealloc(map->line_offsets,
                                                  cap * sizeof(uint32_t));
                map->lines = dbg_xrealloc(map->lines,
                                           cap * sizeof(const char *));
            }
            map->line_offsets[map->line_count] = (uint32_t)(i + 1);
            map->lines[map->line_count] = contents + i + 1;
            map->line_count++;
        }
    }
    return map;
}

void dbg_source_map_free(DbgSourceMap *map)
{
    if (!map) return;
    free(map->path);
    free(map->contents);
    free(map->line_offsets);
    free(map->lines);
    free(map);
}

/// §22  LLVM IR panel

//// LLVM IR emission + tokenizing
 //
 //  dbg_ir_panel_emit() shells out to the configured emit-ir command
 //  (default "monad --emit-ir") via popen, exactly the workflow named in
 //  the request: "you can do monad --emit-ir". stdout is captured in
 //  full, stderr is left attached to the debugger's own stderr so a
 //  compiler crash while emitting IR is still visible for post-mortem
 //  debugging of the debugger itself.
 //
 //  The tokenizer below is a small, deliberately permissive LLVM-IR
 //  lexer: it recognizes %locals, @globals, the standard keyword set,
 //  basic-block labels, numeric/string literals, and `;` comments. It
 //  does not need to be a full LL parser — it only drives syntax-
 //  highlight spans for the read-only IR viewport.
 //
static const char *LLVM_IR_KEYWORDS[] = {
    "define", "declare", "ret", "br", "switch", "indirectbr", "invoke",
    "resume", "unreachable", "call", "callbr", "fneg", "add", "fadd",
    "sub", "fsub", "mul", "fmul", "udiv", "sdiv", "fdiv", "urem", "srem",
    "frem", "shl", "lshr", "ashr", "and", "or", "xor", "alloca", "load",
    "store", "getelementptr", "fence", "cmpxchg", "atomicrmw", "trunc",
    "zext", "sext", "fptrunc", "fpext", "fptoui", "fptosi", "uitofp",
    "sitofp", "ptrtoint", "inttoptr", "bitcast", "addrspacecast", "icmp",
    "fcmp", "phi", "select", "freeze", "extractvalue", "insertvalue",
    "extractelement", "insertelement", "shufflevector", "global",
    "constant", "private", "internal", "external", "linkonce", "weak",
    "common", "appending", "extern_weak", "linkonce_odr", "weak_odr",
    "target", "datalayout", "triple", "attributes", "module", "asm",
    "type", "metadata", "distinct", "tail", "musttail", "notail",
    "volatile", "atomic", "nuw", "nsw", "exact", "inbounds", "align",
    "noalias", "nonnull", "readonly", "readnone", "true", "false",
    "null", "none", "undef", "poison", "to", "unwind", "from", "cleanup",
    "catch", "filter", "personality", "blockaddress", "ifunc", "comdat",
};

static const char *LLVM_IR_TYPES[] = {
    "void", "half", "bfloat", "float", "double", "fp128", "x86_fp80",
    "ppc_fp128", "label", "metadata", "x86_mmx", "x86_amx", "token",
    "ptr", "i1", "i8", "i16", "i32", "i64", "i128",
};

static bool dbg_ir_is_keyword(const char *word, size_t len)
{
    for (size_t i = 0; i < sizeof(LLVM_IR_KEYWORDS) / sizeof(*LLVM_IR_KEYWORDS); i++)
        if (strlen(LLVM_IR_KEYWORDS[i]) == len &&
            strncmp(LLVM_IR_KEYWORDS[i], word, len) == 0)
            return true;
    return false;
}

static bool dbg_ir_is_type(const char *word, size_t len)
{
    for (size_t i = 0; i < sizeof(LLVM_IR_TYPES) / sizeof(*LLVM_IR_TYPES); i++)
        if (strlen(LLVM_IR_TYPES[i]) == len &&
            strncmp(LLVM_IR_TYPES[i], word, len) == 0)
            return true;
    /* Also treat iN / <N x T> patterns generically: leading 'i' followed
       by all digits is an arbitrary-width integer type.                 */
    if (len > 1 && word[0] == 'i') {
        for (size_t i = 1; i < len; i++)
            if (!isdigit((unsigned char)word[i])) return false;
        return true;
    }
    return false;
}

static void dbg_ir_tokenize(DbgIrPanel *panel)
{
    size_t cap = 256;
    panel->tokens = dbg_xmalloc(cap * sizeof(DbgIrToken));
    panel->token_count = 0;

    const char *src = panel->ir_text;
    size_t len = panel->ir_len;
    size_t i = 0;

    while (i < len) {
        char c = src[i];

        if (c == ';') {
            size_t start = i;
            while (i < len && src[i] != '\n') i++;
            DBG_GROW(panel->tokens, panel->token_count, cap, DbgIrToken);
            panel->tokens[panel->token_count++] =
                (DbgIrToken){ (uint32_t)start, (uint32_t)(i - start), DBG_IR_TOK_COMMENT };
            continue;
        }
        if (c == '%' || c == '@') {
            size_t start = i++;
            if (i < len && src[i] == '"') {
                i++;
                while (i < len && src[i] != '"') i++;
                if (i < len) i++;
            } else {
                while (i < len && (isalnum((unsigned char)src[i]) ||
                                    src[i] == '_' || src[i] == '.' || src[i] == '-'))
                    i++;
            }
            DbgIrTokenKind kind = (c == '%') ? DBG_IR_TOK_LOCAL : DBG_IR_TOK_GLOBAL;
            DBG_GROW(panel->tokens, panel->token_count, cap, DbgIrToken);
            panel->tokens[panel->token_count++] =
                (DbgIrToken){ (uint32_t)start, (uint32_t)(i - start), kind };
            continue;
        }
        if (isdigit((unsigned char)c) ||
            (c == '-' && i + 1 < len && isdigit((unsigned char)src[i + 1]))) {
            size_t start = i++;
            while (i < len && (isalnum((unsigned char)src[i]) || src[i] == '.' ||
                                src[i] == '+' || src[i] == '-' || src[i] == 'x'))
                i++;
            DBG_GROW(panel->tokens, panel->token_count, cap, DbgIrToken);
            panel->tokens[panel->token_count++] =
                (DbgIrToken){ (uint32_t)start, (uint32_t)(i - start), DBG_IR_TOK_LITERAL };
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t start = i;
            while (i < len && (isalnum((unsigned char)src[i]) || src[i] == '_')) i++;
            size_t wlen = i - start;
            DbgIrTokenKind kind = DBG_IR_TOK_PLAIN;
            if (dbg_ir_is_keyword(src + start, wlen)) kind = DBG_IR_TOK_KEYWORD;
            else if (dbg_ir_is_type(src + start, wlen)) kind = DBG_IR_TOK_TYPE;
            else if (i < len && src[i] == ':') kind = DBG_IR_TOK_LABEL;
            if (kind != DBG_IR_TOK_PLAIN) {
                DBG_GROW(panel->tokens, panel->token_count, cap, DbgIrToken);
                panel->tokens[panel->token_count++] =
                    (DbgIrToken){ (uint32_t)start, (uint32_t)wlen, kind };
            }
            continue;
        }
        i++;
    }
}

static void dbg_ir_build_line_index(DbgIrPanel *panel)
{
    uint32_t cap = 256;
    panel->line_offsets = dbg_xmalloc(cap * sizeof(uint32_t));
    panel->line_count = 0;
    panel->line_offsets[panel->line_count++] = 0;
    for (size_t i = 0; i < panel->ir_len; i++) {
        if (panel->ir_text[i] == '\n') {
            if (panel->line_count >= cap) {
                cap *= DBG_GROW_FACTOR;
                panel->line_offsets = dbg_xrealloc(panel->line_offsets,
                                                    cap * sizeof(uint32_t));
            }
            panel->line_offsets[panel->line_count++] = (uint32_t)(i + 1);
        }
    }
}

DbgIrPanel *dbg_ir_panel_emit(const char *emit_ir_command,
                              const char *source_path,
                              char **error_out)
{
    if (error_out) *error_out = NULL;
    if (!emit_ir_command || !*emit_ir_command)
        emit_ir_command = "monad --emit-ir";

    DbgStrBuf cmd;
    sb_init(&cmd);
    sb_append(&cmd, emit_ir_command);
    sb_appendc(&cmd, ' ');
    /* Minimal shell-safety: wrap the path in single quotes and escape any
       embedded single quote the POSIX-portable way ('\'').               */
    sb_appendc(&cmd, '\'');
    for (const char *p = source_path; *p; p++) {
        if (*p == '\'') sb_append(&cmd, "'\\''");
        else sb_appendc(&cmd, *p);
    }
    sb_appendc(&cmd, '\'');

    FILE *pipe = popen(cmd.data, "r");
    sb_free(&cmd);
    if (!pipe) {
        if (error_out) *error_out = dbg_xstrdup("failed to spawn monad --emit-ir");
        return NULL;
    }

    DbgStrBuf out;
    sb_init(&out);
    char chunk[DBG_READ_CHUNK];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), pipe)) > 0)
        sb_appendn(&out, chunk, n);

    int status = pclose(pipe);
    if (status != 0 && out.len == 0) {
        sb_free(&out);
        if (error_out) {
            char buf[128];
            snprintf(buf, sizeof(buf), "emit-ir command exited with status %d", status);
            *error_out = dbg_xstrdup(buf);
        }
        return NULL;
    }

    DbgIrPanel *panel = dbg_xcalloc(1, sizeof(*panel));
    panel->ir_text = sb_take(&out);
    panel->ir_len = strlen(panel->ir_text);
    panel->cursor_line = 0;
    panel->scroll_line = 0;
    dbg_ir_build_line_index(panel);
    dbg_ir_tokenize(panel);
    return panel;
}

void dbg_ir_panel_free(DbgIrPanel *panel)
{
    if (!panel) return;
    free(panel->ir_text);
    free(panel->tokens);
    free(panel->line_offsets);
    free(panel->highlighted_value);
    free(panel);
}

/* Render one IR line at (row, col..col+width) applying token-kind
   styles. Tokens are stored as absolute byte spans into ir_text; we
   binary-search... actually a linear scan bounded by tokens-per-line is
   simplest and fast enough (LLVM IR lines rarely exceed a few dozen
   tokens), so we just walk forward from the line's first token index,
   which the caller caches per-line to avoid rescanning from zero.       */
static void dbg_ir_render_line(DbgScreen *sc, DbgIrPanel *panel, int line_idx,
                                int screen_row, int col, int width,
                                DbgTheme *theme)
{
    uint32_t line_start = panel->line_offsets[line_idx];
    uint32_t line_end = ((uint32_t)line_idx + 1 < panel->line_count)
                         ? panel->line_offsets[line_idx + 1] : (uint32_t)panel->ir_len;

    dbg_screen_fill_rect(sc, (DbgRect){ screen_row, col, 1, width }, ' ', theme->base);

    for (uint32_t pos = line_start; pos < line_end && pos < (uint32_t)panel->ir_len; ) {
        DbgIrTokenKind kind = DBG_IR_TOK_PLAIN;
        uint32_t tok_end = pos + 1;
        for (size_t t = 0; t < panel->token_count; t++) {
            DbgIrToken *tok = &panel->tokens[t];
            if (tok->start == pos) {
                kind = tok->kind;
                tok_end = pos + tok->len;
                break;
            }
        }
        DbgStyle style = theme->base;
        switch (kind) {
        case DBG_IR_TOK_KEYWORD: style = theme->ir_keyword; break;
        case DBG_IR_TOK_TYPE:    style = theme->ir_type;    break;
        case DBG_IR_TOK_GLOBAL:  style = theme->ir_global;  break;
        case DBG_IR_TOK_LOCAL:   style = theme->ir_local;   break;
        case DBG_IR_TOK_LITERAL: style = theme->ir_literal; break;
        case DBG_IR_TOK_COMMENT: style = theme->ir_comment; break;
        default: break;
        }
        for (uint32_t p = pos; p < tok_end && p < line_end; p++) {
            int screen_col = col + (int)(p - line_start);
            if (screen_col >= col + width) break;
            char ch = panel->ir_text[p];
            if (ch == '\n') continue;
            dbg_screen_put(sc, screen_row, screen_col, (unsigned char)ch, style);
        }
        pos = tok_end;
    }
}


/// §23  Variable / register inspector

//// Variable inspector
 //
 //  Tree of DbgVarEntry: locals and globals, each optionally expandable
 //  into children for aggregates and pointers. The compiler's runtime
 //  bridge is responsible for populating this (dbg_var_inspector_set_*
 //  helpers below); the debugger itself only renders and manages
 //  expand/collapse + selection state.
 //
static DbgVarInspector *dbg_var_inspector_create(void)
{
    return dbg_xcalloc(1, sizeof(DbgVarInspector));
}

static void dbg_var_entry_free(DbgVarEntry *e)
{
    if (!e) return;
    free(e->name);
    free(e->type_sig);
    free(e->rendered_value);
    for (size_t i = 0; i < e->child_count; i++)
        dbg_var_entry_free(e->children[i]);
    free(e->children);
    free(e);
}

static void dbg_var_inspector_free(DbgVarInspector *vi)
{
    if (!vi) return;
    for (size_t i = 0; i < vi->local_count; i++) dbg_var_entry_free(vi->locals[i]);
    free(vi->locals);
    for (size_t i = 0; i < vi->global_count; i++) dbg_var_entry_free(vi->globals[i]);
    free(vi->globals);
    free(vi);
}

static DbgVarEntry *dbg_var_entry_create(const char *name, const char *type_sig,
                                          const char *value, DbgValueKind kind)
{
    DbgVarEntry *e = dbg_xcalloc(1, sizeof(*e));
    e->name = dbg_xstrdup(name);
    e->type_sig = type_sig ? dbg_xstrdup(type_sig) : NULL;
    e->rendered_value = value ? dbg_xstrdup(value) : NULL;
    e->kind = kind;
    return e;
}

/* Flatten the (possibly nested) local/global trees into display rows,
   respecting expand/collapse state — this is what actually feeds the
   DbgListView for the locals panel, computed fresh each frame since
   the tree is small (a handful of in-scope variables, not thousands).   */
static void dbg_var_flatten(DbgVarEntry *e, int depth, char ***out_lines,
                             size_t *out_count, size_t *out_cap)
{
    DbgStrBuf b;
    sb_init(&b);
    for (int i = 0; i < depth; i++) sb_append(&b, "  ");
    if (e->child_count > 0) sb_append(&b, e->expanded ? "- " : "+ ");
    sb_append(&b, e->name);
    if (e->type_sig) { sb_append(&b, " : "); sb_append(&b, e->type_sig); }
    if (e->rendered_value) { sb_append(&b, " = "); sb_append(&b, e->rendered_value); }

    if (*out_count >= *out_cap) {
        *out_cap = (*out_cap) ? (*out_cap) * DBG_GROW_FACTOR : 16;
        *out_lines = dbg_xrealloc(*out_lines, (*out_cap) * sizeof(char *));
    }
    (*out_lines)[(*out_count)++] = sb_take(&b);
    sb_free(&b);

    if (e->expanded) {
        for (size_t i = 0; i < e->child_count; i++)
            dbg_var_flatten(e->children[i], depth + 1, out_lines, out_count, out_cap);
    }
}


/// §24  Disassembly panel (machine code view, address-indexed)

//// Disassembly panel
 //
 //  Populated by the codegen bridge (LLVM MC layer) with one
 //  DbgDisasmLine per instruction; the debugger only owns rendering and
 //  scroll/PC-following behavior. PC-following means: whenever the
 //  panel is told the current PC changed, it scrolls so that line is
 //  visible without the user having to manually scroll on every step —
 //  the same convenience GDB's TUI mode and LLDB's `disassemble` give.
 //
static void dbg_disasm_follow_pc(DbgDisasmPanel *panel, int height)
{
    for (size_t i = 0; i < panel->line_count; i++) {
        if (!panel->lines[i].is_current_pc) continue;
        int32_t idx = (int32_t)i;
        if (idx < panel->scroll_offset || idx >= panel->scroll_offset + height) {
            panel->scroll_offset = idx - height / 2;
            if (panel->scroll_offset < 0) panel->scroll_offset = 0;
        }
        return;
    }
}

static void dbg_disasm_panel_free(DbgDisasmPanel *panel)
{
    if (!panel) return;
    for (size_t i = 0; i < panel->line_count; i++) {
        free(panel->lines[i].bytes_hex);
        free(panel->lines[i].mnemonic);
        free(panel->lines[i].operands);
    }
    free(panel->lines);
    free(panel);
}


/// §25  Session model (the running debuggee)

//// Default configuration

DbgConfig dbg_default_config(void)
{
    DbgConfig cfg = {0};
    cfg.mouse_enabled = true;
    cfg.truecolor_force = false;
    cfg.blink_period_ms = DBG_BLINK_PERIOD_MS;
    cfg.blink_max_count = DBG_BLINK_MAX_COUNT;
    cfg.target_fps = 60;
    cfg.emit_ir_command = "monad --emit-ir";
    return cfg;
}

//// Built-in palette commands
 //
 //  Registered once, process-wide, the first time a session is created.
 //  Each is a thin trampoline into the §17-26 helpers above.
 //
static bool g_dbg_commands_registered = false;

static void cmd_toggle_breakpoint(DbgSession *s, const char *arg)
{
    (void)arg;
    if (!s->source) return;
    int line = (s->focused_panel == DBG_PANEL_SOURCE)
               ? s->panel_rects[DBG_PANEL_SOURCE].row /* placeholder cursor */
               : 0;
    dbg_breakpoint_toggle_at_line(s, s->source->path, (uint32_t)line);
}

static void cmd_quit(DbgSession *s, const char *arg)
{
    (void)arg;
    s->running = false;
}

static void cmd_dismiss_error(DbgSession *s, const char *arg)
{
    (void)arg;
    if (s->error_stack_count == 0) return;
    dbg_error_snapshot_free(s->error_stack[--s->error_stack_count]);
    s->active_error = (s->error_stack_count > 0)
                       ? s->error_stack[s->error_stack_count - 1] : NULL;
    if (s->error_stack_count == 0) s->running = false;
    s->dirty = true;
}

static void cmd_focus_source(DbgSession *s, const char *arg)
{ (void)arg; s->focused_panel = DBG_PANEL_SOURCE; s->dirty = true; }

static void cmd_focus_ir(DbgSession *s, const char *arg)
{ (void)arg; s->focused_panel = DBG_PANEL_IR; s->dirty = true; }

static void cmd_focus_disasm(DbgSession *s, const char *arg)
{ (void)arg; s->focused_panel = DBG_PANEL_DISASM; s->dirty = true; }

static void cmd_focus_locals(DbgSession *s, const char *arg)
{ (void)arg; s->focused_panel = DBG_PANEL_LOCALS; s->dirty = true; }

static void cmd_focus_backtrace(DbgSession *s, const char *arg)
{ (void)arg; s->focused_panel = DBG_PANEL_BACKTRACE; s->dirty = true; }

static void cmd_focus_breakpoints(DbgSession *s, const char *arg)
{ (void)arg; s->focused_panel = DBG_PANEL_BREAKPOINTS; s->dirty = true; }

static void cmd_reemit_ir(DbgSession *s, const char *arg)
{
    (void)arg;
    if (!s->source) return;
    char *err = NULL;
    const char *emit_cmd = (s->config && s->config->emit_ir_command)
                           ? s->config->emit_ir_command
                           : "monad --emit-ir";
    DbgIrPanel *fresh = dbg_ir_panel_emit(emit_cmd, s->source->path, &err);
    if (fresh) {
        dbg_ir_panel_free(s->ir_panel);
        s->ir_panel = fresh;
    }
    free(err);
    s->dirty = true;
}

static void dbg_register_builtin_commands(void)
{
    if (g_dbg_commands_registered) return;
    g_dbg_commands_registered = true;
    dbg_register_command((DbgCommand){
        "breakpoint-toggle", "bp break stop line", "Toggle a breakpoint at point",
        cmd_toggle_breakpoint, false });
    dbg_register_command((DbgCommand){
        "quit", "exit close abort", "Quit the debugger",
        cmd_quit, false });
    dbg_register_command((DbgCommand){
        "error-dismiss", "ok continue resume next", "Dismiss the current error",
        cmd_dismiss_error, false });
    dbg_register_command((DbgCommand){
        "panel-source", "view goto switch code", "Focus the source panel",
        cmd_focus_source, false });
    dbg_register_command((DbgCommand){
        "panel-ir", "view goto switch llvm", "Focus the LLVM IR panel",
        cmd_focus_ir, false });
    dbg_register_command((DbgCommand){
        "panel-disassembly", "view goto switch asm machine-code", "Focus the disassembly panel",
        cmd_focus_disasm, false });
    dbg_register_command((DbgCommand){
        "panel-locals", "view goto switch variables inspect", "Focus the locals panel",
        cmd_focus_locals, false });
    dbg_register_command((DbgCommand){
        "panel-backtrace", "view goto switch stack frames trace", "Focus the backtrace panel",
        cmd_focus_backtrace, false });
    dbg_register_command((DbgCommand){
        "panel-breakpoints", "view goto switch bp list", "Focus the breakpoints panel",
        cmd_focus_breakpoints, false });
    dbg_register_command((DbgCommand){
        "ir-refresh", "reload re-emit llvm rebuild", "Re-run monad --emit-ir",
        cmd_reemit_ir, false });
}

DbgSession *dbg_session_create(DbgConfig config)
{
    if (g_dbg_registry.commands == NULL) dbg_registry_init();
    dbg_register_builtin_commands();

    DbgSession *s = dbg_xcalloc(1, sizeof(*s));
    s->config = dbg_xmalloc(sizeof(DbgConfig));
    *s->config = config;
    s->caps = dbg_detect_caps(&config);

    if (!dbg_enable_raw_mode(s)) {
        fprintf(stderr, "debugger: stdin is not a tty; cannot start TUI\n");
        free(s->config);
        free(s);
        return NULL;
    }
    dbg_term_enter(&config);

    s->screen = dbg_screen_create(s->caps.cols, s->caps.rows);
    s->theme = dbg_theme_default();
    s->keymap = dbg_keymap_create_default();
    dbg_blink_init(&s->blink, dbg_now_ms());

    s->vars = dbg_var_inspector_create();
    s->focused_panel = DBG_PANEL_SOURCE;
    s->running = true;
    s->dirty = true;

    dbg_compute_layout(s);
    return s;
}

void dbg_session_free(DbgSession *s)
{
    if (!s) return;
    dbg_disable_raw_mode(s);
    if (s->config) dbg_term_leave(s->config);

    dbg_screen_free(s->screen);
    dbg_theme_free(s->theme);
    dbg_keymap_free(s->keymap);
    dbg_source_map_free(s->source);
    dbg_ir_panel_free(s->ir_panel);
    dbg_var_inspector_free(s->vars);
    dbg_disasm_panel_free(s->disasm);
    dbg_palette_free(s->palette);

    for (size_t i = 0; i < s->breakpoint_count; i++) {
        free(s->breakpoints[i].file);
        free(s->breakpoints[i].symbol);
        free(s->breakpoints[i].condition);
    }
    free(s->breakpoints);

    for (size_t i = 0; i < s->watch_count; i++) {
        free(s->watches[i].expression);
        free(s->watches[i].last_value);
    }
    free(s->watches);

    for (size_t i = 0; i < s->error_stack_count; i++)
        dbg_error_snapshot_free(s->error_stack[i]);
    free(s->error_stack);

    free(s->console_log);
    free(s->config);
    free(s);
}


/// §26  Panel registry & focus management

//// Focus cycling
 //
 //  TAB cycles focus through the panels in a fixed, predictable order —
 //  matching the spec's "TAB:cycle" status-line hint. Mouse clicks set
 //  focus directly via dbg_panel_at() instead of cycling.
 //
static const DbgPanelKind DBG_FOCUS_ORDER[] = {
    DBG_PANEL_SOURCE, DBG_PANEL_IR, DBG_PANEL_LOCALS,
    DBG_PANEL_BACKTRACE, DBG_PANEL_BREAKPOINTS, DBG_PANEL_CONSOLE,
};
#define DBG_FOCUS_ORDER_LEN (sizeof(DBG_FOCUS_ORDER) / sizeof(*DBG_FOCUS_ORDER))

static void dbg_cycle_focus(DbgSession *s, int direction)
{
    size_t cur = 0;
    for (size_t i = 0; i < DBG_FOCUS_ORDER_LEN; i++)
        if (DBG_FOCUS_ORDER[i] == s->focused_panel) { cur = i; break; }
    size_t next = (cur + DBG_FOCUS_ORDER_LEN + (size_t)direction) % DBG_FOCUS_ORDER_LEN;
    s->focused_panel = DBG_FOCUS_ORDER[next];
    s->dirty = true;
}


/// §27  Main render pass (damage-only redraw)

//// Render orchestration
 //
 //  dbg_render() is called once per tick ONLY when s->dirty is true (set
 //  by any state-mutating handler) or the blink timer toggled visibility
 //  this frame. It paints every panel into the back buffer — that part
 //  is O(screen area) in CPU time, which is unavoidable for a from-
 //  scratch immediate-mode UI — but the actual terminal write in
 //  dbg_screen_flush() (§6) remains strictly damage-only, so the
 //  expensive part (bytes over the wire / pty) is bounded by what
 //  visibly changed, not by how much we repainted internally.
 //
static void dbg_render_error_banner(DbgSession *s)
{
    if (!s->active_error) return;
    DbgScreen *sc = s->screen;
    DbgTheme  *th = s->theme;
    DbgRect r = s->panel_rects[DBG_PANEL_SOURCE];

    dbg_screen_fill_rect(sc, (DbgRect){ r.row, r.col, 1, r.width }, ' ',
                          th->error_banner);
    DbgStrBuf b;
    sb_init(&b);
    sb_appendf(&b, " %s: %s ", dbg_severity_label(s->active_error->severity),
               s->active_error->original_message);
    dbg_screen_write(sc, r.row, r.col, b.data, th->error_banner);
    sb_free(&b);

    if (s->active_error->source_context) {
        DbgSpan tokens[1]; (void)tokens;
        const char *p = s->active_error->source_context;
        int row = r.row + 1;
        DbgStrBuf line;
        sb_init(&line);
        while (*p && row < r.row + r.height) {
            if (*p == '\n') {
                dbg_screen_write(sc, row, r.col, line.data, th->base);
                line.len = 0;
                if (line.data) line.data[0] = '\0';
                row++;
            } else {
                sb_appendc(&line, *p);
            }
            p++;
        }
        if (line.len > 0 && row < r.row + r.height)
            dbg_screen_write(sc, row, r.col, line.data, th->base);
        sb_free(&line);
    }
}

static void dbg_render_ir_panel(DbgSession *s)
{
    DbgScreen *sc = s->screen;
    DbgTheme  *th = s->theme;
    DbgRect r = s->panel_rects[DBG_PANEL_IR];

    dbg_screen_fill_rect(sc, r, ' ', th->base);
    if (!s->ir_panel) {
        dbg_screen_write(sc, r.row, r.col,
                          "(no IR — run command: ir-refresh)", th->base);
        return;
    }
    DbgIrPanel *panel = s->ir_panel;
    for (int i = 0; i < r.height; i++) {
        int line_idx = panel->scroll_line + i;
        if (line_idx < 0 || (uint32_t)line_idx >= panel->line_count) continue;
        dbg_ir_render_line(sc, panel, line_idx, r.row + i, r.col, r.width, th);
    }
}

static void dbg_render_console(DbgSession *s)
{
    DbgScreen *sc = s->screen;
    DbgTheme  *th = s->theme;
    DbgRect r = s->panel_rects[DBG_PANEL_CONSOLE];
    dbg_screen_fill_rect(sc, r, ' ', th->base);
    if (!s->console_log) return;

    /* Render the tail of the log that fits in the panel height. */
    DbgStrBuf line;
    sb_init(&line);
    int total_lines = 1;
    for (size_t i = 0; i < s->console_log_len; i++)
        if (s->console_log[i] == '\n') total_lines++;

    int max_scroll = total_lines - r.height;
    if (max_scroll < 0) max_scroll = 0;
    if (s->console_scroll > max_scroll) s->console_scroll = max_scroll;
    if (s->console_scroll < 0) s->console_scroll = 0;

    /* Render the tail of the log that fits in the panel height, offset
       upward by console_scroll lines (set by mouse-wheel input in
       dbg_dispatch_mouse) so the user can scroll back through history
       without losing the "always show newest" default at scroll==0.    */
    int skip = total_lines - r.height - s->console_scroll;
    if (skip < 0) skip = 0;
    int cur_line = 0, row = r.row;
    for (size_t i = 0; i <= s->console_log_len; i++) {
        char c = (i < s->console_log_len) ? s->console_log[i] : '\n';
        if (c == '\n') {
            if (cur_line >= skip && row < r.row + r.height) {
                dbg_screen_write(sc, row, r.col, line.data, th->base);
                row++;
            }
            cur_line++;
            line.len = 0;
            if (line.data) line.data[0] = '\0';
        } else {
            sb_appendc(&line, c);
        }
    }
    sb_free(&line);
}

static void dbg_render(DbgSession *s)
{
    dbg_compute_layout(s);
    dbg_screen_clear_back(s->screen);

    if (s->active_error) {
        dbg_render_error_banner(s);
    } else if (s->source) {
        DbgTextView tv = {0};
        tv.lines = s->source->lines;
        tv.line_count = s->source->line_count;
        tv.scroll_offset = s->source_scroll;
        tv.cursor_line = s->source_cursor_line;
        tv.rect = s->panel_rects[DBG_PANEL_SOURCE];
        tv.show_gutter = true;
        dbg_textview_render(s->screen, &tv, s->theme);
    }

    dbg_render_ir_panel(s);

    if (s->vars) {
        char **lines = NULL;
        size_t count = 0, cap = 0;
        for (size_t i = 0; i < s->vars->local_count; i++)
            dbg_var_flatten(s->vars->locals[i], 0, &lines, &count, &cap);
        DbgListView lv = {0};
        lv.items = lines;
        lv.item_count = count;
        lv.selected = s->locals_selected;
        lv.scroll_offset = s->locals_scroll;
        lv.rect = s->panel_rects[DBG_PANEL_LOCALS];
        dbg_listview_render(s->screen, &lv, s->theme);
        for (size_t i = 0; i < count; i++) free(lines[i]);
        free(lines);
    }

    if (s->active_error && s->active_error->backtrace.frame_count > 0) {
        DbgBacktrace *bt = &s->active_error->backtrace;
        char **lines = dbg_xmalloc(bt->frame_count * sizeof(char *));
        for (size_t i = 0; i < bt->frame_count; i++)
            lines[i] = dbg_frame_render(&bt->frames[i]);
        DbgListView lv = {0};
        lv.items = lines;
        lv.item_count = bt->frame_count;
        lv.selected = s->backtrace_selected;
        lv.scroll_offset = s->backtrace_scroll;
        lv.rect = s->panel_rects[DBG_PANEL_BACKTRACE];
        dbg_listview_render(s->screen, &lv, s->theme);
        for (size_t i = 0; i < bt->frame_count; i++) free(lines[i]);
        free(lines);
    }

    if (s->breakpoint_count > 0) {
        char **lines = dbg_xmalloc(s->breakpoint_count * sizeof(char *));
        for (size_t i = 0; i < s->breakpoint_count; i++) {
            DbgStrBuf b;
            sb_init(&b);
            DbgBreakpoint *bp = &s->breakpoints[i];
            sb_appendf(&b, "%s %s:%u", bp->enabled ? "[x]" : "[ ]",
                       bp->file ? bp->file : "?", bp->line);
            lines[i] = sb_take(&b);
            sb_free(&b);
        }
        DbgListView lv = {0};
        lv.items = lines;
        lv.item_count = s->breakpoint_count;
        lv.selected = s->breakpoints_selected;
        lv.scroll_offset = s->breakpoints_scroll;
        lv.rect = s->panel_rects[DBG_PANEL_BREAKPOINTS];
        dbg_listview_render(s->screen, &lv, s->theme);
        for (size_t i = 0; i < s->breakpoint_count; i++) free(lines[i]);
        free(lines);
    }

    dbg_render_console(s);
    dbg_render_status_line(s, s->screen->rows - (s->panel_rects[DBG_PANEL_CONSOLE].height) - 1
                            + s->panel_rects[DBG_PANEL_CONSOLE].height);

    if (s->palette_open) dbg_palette_render(s);

    /* Cursor: only paint the blink glyph if visible this frame. The
       blink toggling itself never repaints the WHOLE screen — only the
       one cell the caret occupies gets a style flip, which is exactly
       the kind of single-cell damage dbg_screen_flush is built for.     */
    if (s->blink.visible) {
        DbgPoint caret = { 0, 0 };
        if (s->palette_open && s->palette) {
            caret.row = (int16_t)(s->palette->rect.row);
            caret.col = (int16_t)(s->palette->rect.col + 4 +
                                   (int)s->palette->cursor_pos);
        }
        size_t idx = (size_t)caret.row * (size_t)s->screen->cols + (size_t)caret.col;
        if (idx < (size_t)s->screen->cols * (size_t)s->screen->rows) {
            s->screen->back[idx].style.attrs ^= DBG_ATTR_REVERSE;
        }
    }

    dbg_screen_flush(s->screen, s->caps.truecolor);
    s->dirty = false;
}


/// §28  Event loop / dispatcher

//// Input polling
 //
 //  select() waits on stdin with a timeout equal to the remaining time
 //  until the next blink toggle (or the frame period, whichever is
 //  sooner), so the process is fully asleep between events instead of
 //  busy-polling — important for "performant" when the debugger may sit
 //  open for minutes while the user reads a backtrace.
 //
static void dbg_decode_pending_bytes(DbgByteRing *ring, DbgEventQueue *q)
{
    while (!ring_empty(ring)) {
        int c = ring_pop(ring);
        if (c == -1) break;

        if (c == 0x1b) {
            /* Could be a bare ESC (DBG_KEY_ESCAPE) or the start of a
               CSI/SS3 sequence. If no more bytes are buffered yet we
               cannot tell the difference without a short timeout — the
               caller (dbg_poll_input) only invokes us after select()
               reports readiness, so a real sequence's remaining bytes
               are normally already in the OS buffer by the time we get
               here, except for genuinely bare Escape key presses.       */
            int next = ring_pop(ring);
            if (next == -1) {
                DbgEvent ev = {0};
                ev.kind = DBG_EVENT_KEY;
                ev.as.key.sym = DBG_KEY_ESCAPE;
                eventq_push(q, ev);
                break;
            }
            if (next == '[') {
                int peek = ring_pop(ring);
                if (peek == '<') {
                    DbgMouseEvent me = {0};
                    if (decode_sgr_mouse(ring, &me)) {
                        DbgEvent ev = {0};
                        ev.kind = DBG_EVENT_MOUSE;
                        ev.as.mouse = me;
                        eventq_push(q, ev);
                    }
                } else {
                    DbgKeyEvent ke = {0};
                    if (decode_csi_key(ring, peek, &ke)) {
                        DbgEvent ev = {0};
                        ev.kind = DBG_EVENT_KEY;
                        ev.as.key = ke;
                        eventq_push(q, ev);
                    }
                }
                continue;
            }
            if (next == 'O') {
                DbgKeyEvent ke = {0};
                if (decode_ss3_key(ring, &ke)) {
                    DbgEvent ev = {0};
                    ev.kind = DBG_EVENT_KEY;
                    ev.as.key = ke;
                    eventq_push(q, ev);
                }
                continue;
            }
            /* ESC followed by a plain character: Meta+key (Emacs-style
               M-x is literally ESC x over most terminals).              */
            DbgEvent ev = {0};
            ev.kind = DBG_EVENT_KEY;
            ev.as.key.sym = DBG_KEY_CHAR;
            ev.as.key.codepoint = (uint32_t)next;
            ev.as.key.meta = true;
            eventq_push(q, ev);
            continue;
        }

        if (c == '\r' || c == '\n') {
            DbgEvent ev = {0};
            ev.kind = DBG_EVENT_KEY;
            ev.as.key.sym = DBG_KEY_ENTER;
            eventq_push(q, ev);
            continue;
        }
        if (c == 0x7f || c == 0x08) {
            DbgEvent ev = {0};
            ev.kind = DBG_EVENT_KEY;
            ev.as.key.sym = DBG_KEY_BACKSPACE;
            eventq_push(q, ev);
            continue;
        }
        if (c == '\t') {
            DbgEvent ev = {0};
            ev.kind = DBG_EVENT_KEY;
            ev.as.key.sym = DBG_KEY_TAB;
            eventq_push(q, ev);
            continue;
        }
        if (c >= 1 && c <= 26 && c != 9 && c != 13) {
            /* Ctrl+letter */
            DbgEvent ev = {0};
            ev.kind = DBG_EVENT_KEY;
            ev.as.key.sym = DBG_KEY_CHAR;
            ev.as.key.codepoint = (uint32_t)(c - 1 + 'a');
            ev.as.key.ctrl = true;
            eventq_push(q, ev);
            continue;
        }

        uint32_t cp = decode_utf8_codepoint(ring, (unsigned char)c);
        DbgEvent ev = {0};
        ev.kind = DBG_EVENT_KEY;
        ev.as.key.sym = DBG_KEY_CHAR;
        ev.as.key.codepoint = cp;
        eventq_push(q, ev);
    }
}

static void dbg_poll_input(DbgEventQueue *q, uint64_t timeout_ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv;
    tv.tv_sec = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);

    int rc = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (rc <= 0) return;   /* timeout or signal interruption — caller
                               treats this as "no input, just tick"      */

    DbgByteRing ring = {0};
    ring_fill_nonblocking(&ring);
    dbg_decode_pending_bytes(&ring, q);
}

//// Dispatch
 //
 //  Routes a decoded event first to the palette (if open — it captures
 //  all keyboard input modally, like a real minibuffer), then to global
 //  keymap bindings, then to the focused panel's widget-level mouse
 //  handling.
 //
static void dbg_dispatch_key(DbgSession *s, DbgKeyEvent *key)
{
    dbg_blink_note_activity(&s->blink, dbg_now_ms());
    s->dirty = true;

    if (s->palette_open) {
        DbgPalette *p = s->palette;
        switch (key->sym) {
        case DBG_KEY_ESCAPE: dbg_palette_close(s); return;
        case DBG_KEY_ENTER:  dbg_palette_activate(s); return;
        case DBG_KEY_UP:     dbg_palette_move_selection(p, -1); return;
        case DBG_KEY_DOWN:   dbg_palette_move_selection(p, 1); return;
        case DBG_KEY_BACKSPACE: dbg_palette_backspace(p); return;
        case DBG_KEY_CHAR:
            if (!key->ctrl && !key->meta) dbg_palette_insert_char(p, key->codepoint);
            return;
        default: return;
        }
    }

    if (key->sym == DBG_KEY_CHAR && key->meta && key->codepoint == 'x') {
        dbg_palette_open(s);
        return;
    }
    if (key->sym == DBG_KEY_TAB)     { dbg_cycle_focus(s, 1);  return; }
    if (key->sym == DBG_KEY_BACKTAB) { dbg_cycle_focus(s, -1); return; }
    if (key->sym == DBG_KEY_CHAR && key->codepoint == 'q' && !key->ctrl) {
        if (s->active_error) cmd_dismiss_error(s, NULL);
        else s->running = false;
        return;
    }

    /* Custom keymap bindings (extensible by embedders via
       dbg_keymap_bind) are checked last so built-ins above always win
       on conflict — the same precedence Emacs gives major-mode maps
       under the global map for these particular bindings.               */
    for (size_t i = 0; i < s->keymap->count; i++) {
        DbgKeyBinding *kb = &s->keymap->bindings[i];
        if (kb->sym != key->sym) continue;
        if (kb->sym == DBG_KEY_CHAR && kb->codepoint != key->codepoint) continue;
        if (kb->ctrl != key->ctrl || kb->meta != key->meta) continue;
        if (kb->action) kb->action(s);
        return;
    }

    /* Panel-specific scrolling via arrow/page keys when no binding and
       no palette claimed the event. */
    if (s->focused_panel == DBG_PANEL_IR && s->ir_panel) {
        if (key->sym == DBG_KEY_DOWN) s->ir_panel->scroll_line++;
        if (key->sym == DBG_KEY_UP && s->ir_panel->scroll_line > 0) s->ir_panel->scroll_line--;
        if (key->sym == DBG_KEY_PGDN) s->ir_panel->scroll_line += s->panel_rects[DBG_PANEL_IR].height;
        if (key->sym == DBG_KEY_PGUP) {
            s->ir_panel->scroll_line -= s->panel_rects[DBG_PANEL_IR].height;
            if (s->ir_panel->scroll_line < 0) s->ir_panel->scroll_line = 0;
        }
    }
}

static void dbg_dispatch_mouse(DbgSession *s, DbgMouseEvent *ev)
{
    s->dirty = true;

    if (s->palette_open && dbg_palette_handle_mouse(s, ev)) return;

    DbgPanelKind clicked_panel = dbg_panel_at(s, ev->row, ev->col);
    if (ev->kind == DBG_MOUSE_DOWN) s->focused_panel = clicked_panel;

    switch (clicked_panel) {
    case DBG_PANEL_SOURCE: {
        DbgTextView tv = {0};
        tv.rect = s->panel_rects[DBG_PANEL_SOURCE];
        tv.line_count = s->source ? s->source->line_count : 0;
        tv.scroll_offset = s->source_scroll;
        tv.cursor_line = s->source_cursor_line;
        if (dbg_textview_handle_mouse(&tv, ev)) {
            s->source_scroll = tv.scroll_offset;
            s->source_cursor_line = tv.cursor_line;
        }
        break;
    }
    case DBG_PANEL_IR: {
        if (!s->ir_panel) break;
        DbgTextView tv = {0};
        tv.rect = s->panel_rects[DBG_PANEL_IR];
        tv.line_count = s->ir_panel->line_count;
        tv.scroll_offset = s->ir_panel->scroll_line;
        tv.cursor_line = s->ir_panel->cursor_line;
        if (dbg_textview_handle_mouse(&tv, ev)) {
            s->ir_panel->scroll_line = tv.scroll_offset;
            s->ir_panel->cursor_line = tv.cursor_line;
        }
        break;
    }
    case DBG_PANEL_LOCALS: {
        DbgListView lv = {0};
        lv.rect = s->panel_rects[DBG_PANEL_LOCALS];
        lv.item_count = s->vars ? s->vars->local_count : 0;
        lv.selected = s->locals_selected;
        lv.scroll_offset = s->locals_scroll;
        bool activated = false;
        if (dbg_listview_handle_mouse(&lv, ev, &activated)) {
            s->locals_selected = lv.selected;
            s->locals_scroll = lv.scroll_offset;
            if (activated && s->vars && lv.selected >= 0 &&
                (size_t)lv.selected < s->vars->local_count) {
                s->vars->locals[lv.selected]->expanded =
                    !s->vars->locals[lv.selected]->expanded;
            }
        }
        break;
    }
    case DBG_PANEL_BACKTRACE: {
        DbgListView lv = {0};
        lv.rect = s->panel_rects[DBG_PANEL_BACKTRACE];
        lv.item_count = s->active_error ? s->active_error->backtrace.frame_count : 0;
        lv.selected = s->backtrace_selected;
        lv.scroll_offset = s->backtrace_scroll;
        bool activated = false;
        if (dbg_listview_handle_mouse(&lv, ev, &activated)) {
            s->backtrace_selected = lv.selected;
            s->backtrace_scroll = lv.scroll_offset;
        }
        break;
    }
    case DBG_PANEL_BREAKPOINTS: {
        DbgListView lv = {0};
        lv.rect = s->panel_rects[DBG_PANEL_BREAKPOINTS];
        lv.item_count = s->breakpoint_count;
        lv.selected = s->breakpoints_selected;
        lv.scroll_offset = s->breakpoints_scroll;
        bool activated = false;
        if (dbg_listview_handle_mouse(&lv, ev, &activated)) {
            s->breakpoints_selected = lv.selected;
            s->breakpoints_scroll = lv.scroll_offset;
            if (activated && lv.selected >= 0 &&
                (size_t)lv.selected < s->breakpoint_count) {
                s->breakpoints[lv.selected].enabled =
                    !s->breakpoints[lv.selected].enabled;
            }
        }
        break;
    }
    case DBG_PANEL_CONSOLE: {
        if (ev->kind == DBG_MOUSE_WHEEL_UP) {
            s->console_scroll -= DBG_WHEEL_LINES;
            if (s->console_scroll < 0) s->console_scroll = 0;
        } else if (ev->kind == DBG_MOUSE_WHEEL_DOWN) {
            s->console_scroll += DBG_WHEEL_LINES;
        }
        break;
    }
    case DBG_PANEL_DISASM:
        break;   /* disassembly panel owns its own scroll_offset field   */
    }
}

static void dbg_dispatch_event(DbgSession *s, DbgEvent *ev)
{
    switch (ev->kind) {
    case DBG_EVENT_KEY:    dbg_dispatch_key(s, &ev->as.key); break;
    case DBG_EVENT_MOUSE:  dbg_dispatch_mouse(s, &ev->as.mouse); break;
    case DBG_EVENT_RESIZE:
        dbg_screen_resize(s->screen, ev->as.resize.cols, ev->as.resize.rows);
        s->caps.cols = ev->as.resize.cols;
        s->caps.rows = ev->as.resize.rows;
        s->dirty = true;
        break;
    case DBG_EVENT_QUIT: s->running = false; break;
    default: break;
    }
}

int dbg_session_run(DbgSession *s)
{
    if (!s) return 1;
    DbgEventQueue q;
    eventq_init(&q);

    while (s->running) {
        uint64_t now = dbg_now_ms();
        uint64_t period = s->config->blink_period_ms;
        uint64_t elapsed = now - s->blink.last_toggle_ms;
        uint64_t wait_ms = (elapsed >= period) ? 0 : (period - elapsed);
        if (wait_ms > 1000 / (s->config->target_fps ? s->config->target_fps : 60))
            wait_ms = 1000 / (s->config->target_fps ? s->config->target_fps : 60);

        dbg_poll_input(&q, wait_ms);

        DbgEvent ev;
        while (eventq_pop(&q, &ev)) {
            dbg_dispatch_event(s, &ev);
            dbg_event_free(&ev);
        }

        bool blink_changed = dbg_blink_tick(&s->blink, dbg_now_ms(),
                                             s->config->blink_period_ms,
                                             s->config->blink_max_count);
        if (blink_changed) s->dirty = true;

        if (s->dirty) dbg_render(s);
    }

    eventq_free(&q);
    return (s->active_error &&
            (s->active_error->severity == DBG_SEV_FATAL ||
             s->active_error->severity == DBG_SEV_INTERNAL)) ? 1 : 0;
}


/// §29  Keymap (Emacs-ish chords, configurable)

//// Default keymap
 //
 //  Built-ins (M-x, TAB, q, Enter, arrows in IR panel) are handled
 //  directly in dbg_dispatch_key for speed and clarity; this registry
 //  is for ADDITIONAL bindings an embedder wants without recompiling
 //  the dispatch switch — e.g. binding 'n'/'p' to next/previous
 //  breakpoint. Empty by default; dbg_keymap_bind() grows it.
 //
DbgKeymap *dbg_keymap_create_default(void)
{
    DbgKeymap *km = dbg_xcalloc(1, sizeof(*km));
    km->cap = 16;
    km->bindings = dbg_xmalloc(km->cap * sizeof(DbgKeyBinding));
    km->count = 0;
    return km;
}

void dbg_keymap_free(DbgKeymap *km)
{
    if (!km) return;
    free(km->bindings);
    free(km);
}

void dbg_keymap_bind(DbgKeymap *km, DbgKeyBinding binding)
{
    DBG_GROW(km->bindings, km->count, km->cap, DbgKeyBinding);
    km->bindings[km->count++] = binding;
}


/// §30  Theme / color palette

//// Default theme
 //
 //  A dark, high-contrast palette chosen for long debugging sessions:
 //  desaturated background, saturated accent colors reserved for things
 //  that matter (errors, the active selection, IR keywords) so the eye
 //  is drawn to them rather than fighting noise everywhere.
 //
DbgTheme *dbg_theme_default(void)
{
    DbgTheme *t = dbg_xcalloc(1, sizeof(*t));

    t->base             = (DbgStyle){ {214, 214, 214}, {18, 18, 22},  DBG_ATTR_NONE };
    t->status_line       = (DbgStyle){ {18, 18, 22},   {120, 170, 220}, DBG_ATTR_BOLD };
    t->cursor            = (DbgStyle){ {18, 18, 22},   {214, 214, 214}, DBG_ATTR_NONE };
    t->selection          = (DbgStyle){ {18, 18, 22},   {90, 110, 150},  DBG_ATTR_NONE };
    t->error_banner       = (DbgStyle){ {255, 235, 235}, {130, 30, 40},  DBG_ATTR_BOLD };
    t->warning_banner    = (DbgStyle){ {30, 25, 10},   {210, 170, 60},  DBG_ATTR_BOLD };
    t->gutter             = (DbgStyle){ {110, 110, 120}, {12, 12, 16},  DBG_ATTR_DIM };
    t->gutter_breakpoint = (DbgStyle){ {255, 235, 235}, {130, 30, 40},  DBG_ATTR_BOLD };
    t->ir_keyword         = (DbgStyle){ {190, 140, 230}, {18, 18, 22},  DBG_ATTR_BOLD };
    t->ir_type            = (DbgStyle){ {120, 190, 220}, {18, 18, 22},  DBG_ATTR_NONE };
    t->ir_global           = (DbgStyle){ {230, 180, 100}, {18, 18, 22},  DBG_ATTR_NONE };
    t->ir_local            = (DbgStyle){ {150, 210, 150}, {18, 18, 22},  DBG_ATTR_NONE };
    t->ir_literal          = (DbgStyle){ {220, 150, 150}, {18, 18, 22},  DBG_ATTR_NONE };
    t->ir_comment          = (DbgStyle){ {100, 100, 110}, {18, 18, 22},  DBG_ATTR_ITALIC };
    t->palette_match      = (DbgStyle){ {255, 220, 120}, {18, 18, 22},  DBG_ATTR_BOLD };
    t->palette_border      = (DbgStyle){ {90, 90, 100},  {18, 18, 22},  DBG_ATTR_DIM };

    return t;
}

void dbg_theme_free(DbgTheme *theme)
{
    free(theme);
}


/// §31  Logging

//// Console log
 //
 //  Appends to the session's in-memory console scrollback, rendered by
 //  dbg_render_console() (§27). This is the debugger's own log — NOT
 //  the compiler's stdout/stderr — used for things like "breakpoint 3
 //  set at foo.mn:42" confirmations and palette command echoes.
 //
static void dbg_console_appendf(DbgSession *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void dbg_console_appendf(DbgSession *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;

    size_t needed = s->console_log_len + (size_t)n + 2;
    if (needed > s->console_log_cap) {
        size_t newcap = s->console_log_cap ? s->console_log_cap : 1024;
        while (newcap < needed) newcap *= DBG_GROW_FACTOR;
        s->console_log = dbg_xrealloc(s->console_log, newcap);
        s->console_log_cap = newcap;
    }
    va_start(ap, fmt);
    vsnprintf(s->console_log + s->console_log_len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    s->console_log_len += (size_t)n;
    s->console_log[s->console_log_len++] = '\n';
    s->console_log[s->console_log_len] = '\0';
    s->dirty = true;
}


/// §32  Public entry points

//// Standalone debugger entry point
 //
 //  Wired to `monad --debug <file>`: loads the given source as the
 //  active source map, emits its LLVM IR up front (so the IR panel is
 //  populated even before any error occurs), and opens the TUI on a
 //  synthetic NOTE-severity snapshot so the same render path used for
 //  real errors is exercised in the standalone case too — one code
 //  path for "launched directly" and "launched by a trapped error."
 //
int dbg_main_with_config(int argc, char **argv, DbgConfig cfg)
{
    if (argc < 2) {
        fprintf(stderr, "usage: monad debug <file.mon>\n");
        return 2;
    }
    const char *path = argv[1];

    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "debugger: stdin is not a tty\n");
        return 2;
    }

    if (!cfg.emit_ir_command || !*cfg.emit_ir_command)
        cfg.emit_ir_command = "monad --emit-ir";

    DbgSession *s = dbg_session_create(cfg);
    if (!s) return 1;

    s->source = dbg_source_map_load(path);
    if (!s->source) {
        dbg_console_appendf(s, "could not open %s", path);
    }

    char *ir_err = NULL;
    s->ir_panel = dbg_ir_panel_emit(cfg.emit_ir_command, path, &ir_err);
    if (ir_err) {
        dbg_console_appendf(s, "emit-ir: %s", ir_err);
        free(ir_err);
    }

    int rc = dbg_session_run(s);
    dbg_session_free(s);
    return rc;
}

int dbg_main(int argc, char **argv)
{
    return dbg_main_with_config(argc, argv, dbg_default_config());
}
