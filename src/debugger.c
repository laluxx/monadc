/// debugger.c — Monad debugger engine + terminal client implementation
//
//  The reusable DbgEngine is the authority for execution semantics.  It owns
//  process/thread/frame selection, logical breakpoints, values, stepping,
//  reverse-execution capabilities and asynchronous backend events.  The TUI
//  below is intentionally only one client of that engine; compiler diagnostics
//  may still enter through dbg_trap_error() on the zero-cost no-error path.
//
//  Source layout:
//
//    §1   Includes and internal constants
//    §2   Memory helpers
//    §3   String / cell-buffer helpers
//    §E   Reusable debug engine / backend ABI implementation
//    §4   Terminal raw-mode + capability setup
//    §5   Screen buffer (front/back, output-damage tracking)
//    §6   ANSI/SGR rendering primitives
//    §7   Input: key decoding
//    §8   Input: mouse (SGR 1006) decoding
//    §9   Event queue
//    §10  Cursor blink timer (Emacs semantics)
//    §11  Layout: panes, splits, geometry
//    §12  Widget: text viewport (scrollback, source view)
//    §13  Widget: list (frames / variables / breakpoints)
//    §14  Widget: status / mode line
//    §15  Command palette: orderless completion engine
//    §16  Command palette: vertico-style minibuffer UI
//    §17  Command registry
//    §18  Compiler error trapping & snapshot capture
//    §19  Compiler backtrace model
//    §20  TUI breakpoint/watch helpers
//    §21  Source text cache / line table
//    §22  LLVM IR panel
//    §23  Lazy variable inspector
//    §24  Disassembly panel
//    §25  TUI session model / engine synchronization
//    §26  Panel registry & focus management
//    §27  Main render pass
//    §28  Multiplexed terminal/backend event loop
//    §29  Keymap (Emacs-ish chords, configurable)
//    §30  Theme / color palette
//    §31  Logging
//    §32  Public standalone entry points
//

/* Feature test macros must precede every system header: clock_gettime /
   CLOCK_MONOTONIC need POSIX.1-2001, popen/pclose need POSIX.2, and we
   want both visible regardless of which libc the toolchain ships.       */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE    700

#include "debugger.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <termios.h>
#include <poll.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

static void dbg_console_appendf(DbgSession *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static const char *dbg_process_state_label(DbgProcessState state);
static const char *dbg_stop_reason_label(DbgStopReason reason);


/// §1  Includes and internal constants

//// Internal constants
 //
 //  Mirrors the conventions in lsp.c: growth factor and initial sizes for
 //  every dynamic array, kept in one place so tuning is a one-line change.
 //
#define DBG_GROW_FACTOR           2u
#define DBG_INITIAL_BUF           4096u
#define DBG_INITIAL_EVENTQ        64u
#define DBG_INITIAL_COMMANDS      64u
#define DBG_READ_CHUNK            8192u
#define DBG_ESC_TIMEOUT_MS        25u   /* bare ESC vs start-of-sequence */
#define DBG_BLINK_PERIOD_MS       500u
#define DBG_BLINK_MAX_COUNT       10u
#define DBG_PALETTE_MAX_RESULTS   256u
#define DBG_MAX_BREAKPOINTS       4096u
#define DBG_MAX_FRAMES            65536u
#define DBG_MAX_PANELS            16u
#define DBG_DOUBLE_CLICK_MS       350u
#define DBG_DEFAULT_VARIABLE_PAGE_SIZE   256u
#define DBG_DEFAULT_VARIABLE_CHILD_LIMIT 65536u
#define DBG_DEFAULT_DISASSEMBLY_COUNT    64u

/* ------------------------------------------------------------------------- */
/* Private terminal/TUI types. None of these leak through debugger.h.         */
/* ------------------------------------------------------------------------- */

typedef struct { int16_t row, col; } DbgPoint;
typedef struct DbgRect { int16_t row, col, height, width; } DbgRect;
typedef struct { uint8_t r, g, b; } DbgColor;
typedef enum {
    DBG_ATTR_NONE      = 0,
    DBG_ATTR_BOLD      = 1 << 0,
    DBG_ATTR_DIM       = 1 << 1,
    DBG_ATTR_ITALIC    = 1 << 2,
    DBG_ATTR_UNDERLINE = 1 << 3,
    DBG_ATTR_REVERSE   = 1 << 4,
    DBG_ATTR_STRIKE    = 1 << 5,
} DbgAttr;
typedef struct { DbgColor fg, bg; uint8_t attrs; } DbgStyle;
typedef struct {
    bool truecolor, mouse_sgr, bracketed_paste, kitty_keyboard;
    int cols, rows;
} DbgTermCaps;

typedef enum {
    DBG_EVENT_NONE = 0, DBG_EVENT_KEY, DBG_EVENT_MOUSE, DBG_EVENT_RESIZE,
    DBG_EVENT_TICK, DBG_EVENT_PASTE, DBG_EVENT_QUIT,
} DbgEventKind;
typedef enum {
    DBG_KEY_CHAR = 0,
    DBG_KEY_UP, DBG_KEY_DOWN, DBG_KEY_LEFT, DBG_KEY_RIGHT,
    DBG_KEY_HOME, DBG_KEY_END, DBG_KEY_PGUP, DBG_KEY_PGDN,
    DBG_KEY_TAB, DBG_KEY_BACKTAB, DBG_KEY_ENTER, DBG_KEY_ESCAPE,
    DBG_KEY_BACKSPACE, DBG_KEY_DELETE, DBG_KEY_INSERT,
    DBG_KEY_F1, DBG_KEY_F2, DBG_KEY_F3, DBG_KEY_F4, DBG_KEY_F5,
    DBG_KEY_F6, DBG_KEY_F7, DBG_KEY_F8, DBG_KEY_F9, DBG_KEY_F10,
    DBG_KEY_F11, DBG_KEY_F12,
} DbgKeySym;
typedef struct { DbgKeySym sym; uint32_t codepoint; bool ctrl, meta, shift; } DbgKeyEvent;
typedef enum {
    DBG_MOUSE_MOVE = 0, DBG_MOUSE_DOWN, DBG_MOUSE_UP, DBG_MOUSE_DRAG,
    DBG_MOUSE_WHEEL_UP, DBG_MOUSE_WHEEL_DOWN,
} DbgMouseKind;
typedef enum { DBG_BTN_NONE = 0, DBG_BTN_LEFT, DBG_BTN_MIDDLE, DBG_BTN_RIGHT } DbgMouseButton;
typedef struct {
    DbgMouseKind kind; DbgMouseButton button; int16_t row, col;
    bool ctrl, meta, shift;
} DbgMouseEvent;
typedef struct DbgEvent {
    DbgEventKind kind;
    union {
        DbgKeyEvent key; DbgMouseEvent mouse;
        struct { int cols, rows; } resize;
        char *paste_text;
    } as;
} DbgEvent;
static void dbg_event_free(DbgEvent *ev);

typedef struct {
    bool visible, solid; uint32_t blink_count;
    uint64_t last_toggle_ms, last_activity_ms;
} DbgCursorBlink;

typedef struct { uint32_t start, len; } DbgMatchSpan;
typedef struct {
    const char *candidate; void *user_data; double score;
    DbgMatchSpan *spans; size_t span_count;
} DbgCompletionResult;

typedef enum {
    DBG_PANEL_SOURCE = 0, DBG_PANEL_IR, DBG_PANEL_DISASM, DBG_PANEL_LOCALS,
    DBG_PANEL_BACKTRACE, DBG_PANEL_BREAKPOINTS, DBG_PANEL_CONSOLE,
} DbgPanelKind;

typedef struct DbgInputState DbgInputState;
typedef struct DbgScreen DbgScreen;
typedef struct DbgCell DbgCell;
typedef struct DbgTextView DbgTextView;
typedef struct DbgListView DbgListView;
typedef struct DbgPalette DbgPalette;
typedef struct DbgKeymap DbgKeymap;
typedef struct DbgTheme DbgTheme;
typedef struct DbgSourceFile DbgSourceFile;
typedef struct DbgIrPanel DbgIrPanel;
typedef struct DbgVarInspector DbgVarInspector;
typedef struct DbgDisasmPanel DbgDisasmPanel;

typedef void (*DbgCommandFn)(DbgSession *, const char *);
typedef struct {
    const char *name, *keywords, *summary; DbgCommandFn run;
    bool needs_arg; DbgCapabilities required_capabilities;
} DbgCommand;

typedef struct DbgFrame {
    char *function, *file; uint32_t line, column; char *ir_value;
} DbgFrame;
typedef struct { DbgFrame *frames; size_t frame_count; } DbgBacktrace;
struct DbgErrorSnapshot {
    DbgSeverity severity; char *original_message, *code, *file;
    uint32_t line, column; DbgBacktrace backtrace;
    char *source_context; uint64_t captured_at_ms;
};

struct DbgSourceFile {
    char *path, *contents; size_t contents_len;
    uint32_t *line_offsets; const char **lines; uint32_t line_count;
};
typedef DbgSourceFile DbgSourceMap; /* private legacy implementation spelling */

typedef enum {
    DBG_IR_TOK_PLAIN = 0, DBG_IR_TOK_KEYWORD, DBG_IR_TOK_TYPE,
    DBG_IR_TOK_GLOBAL, DBG_IR_TOK_LOCAL, DBG_IR_TOK_LITERAL,
    DBG_IR_TOK_COMMENT, DBG_IR_TOK_LABEL,
} DbgIrTokenKind;
typedef struct { uint32_t start, len; DbgIrTokenKind kind; } DbgIrToken;
struct DbgIrPanel {
    char *ir_text; size_t ir_len; DbgIrToken *tokens; size_t token_count;
    uint32_t *line_offsets; uint32_t line_count;
    int32_t cursor_line, scroll_line; char *highlighted_value;
};

typedef enum {
    DBG_VAL_SCALAR = 0, DBG_VAL_AGGREGATE, DBG_VAL_POINTER, DBG_VAL_FUNCTION,
} DbgValueKind;
typedef struct DbgVarEntry {
    DbgValueInfo value; DbgValueKind kind; bool expanded;
    struct DbgVarEntry **children; size_t child_count;
} DbgVarEntry;
struct DbgVarInspector {
    DbgVarEntry **locals; size_t local_count;
    DbgVarEntry **globals; size_t global_count;
    int32_t selected_index, scroll_offset;
};

typedef struct {
    uint64_t address; char *bytes_hex, *mnemonic, *operands;
    bool is_current_pc, has_breakpoint;
} DbgDisasmLine;
struct DbgDisasmPanel {
    DbgDisasmLine *lines; size_t line_count; int32_t scroll_offset;
};

typedef void (*DbgActionFn)(DbgSession *);
typedef struct {
    DbgKeySym sym; uint32_t codepoint; bool ctrl, meta, shift;
    DbgActionFn action; const char *description;
} DbgKeyBinding;
struct DbgKeymap { DbgKeyBinding *bindings; size_t count, cap; };
struct DbgTheme {
    DbgStyle base, status_line, cursor, selection, error_banner, warning_banner;
    DbgStyle gutter, gutter_breakpoint;
    DbgStyle ir_keyword, ir_type, ir_global, ir_local, ir_literal, ir_comment;
    DbgStyle palette_match, palette_border;
};

struct DbgSession {
    DbgScreen *screen; DbgTermCaps caps; struct termios saved_termios;
    bool raw_mode_active; DbgInputState *input_state;
    DbgCursorBlink blink; DbgKeymap *keymap; DbgTheme *theme; DbgConfig *config;
    DbgEngine *engine; bool owns_engine; DbgCodeMap *code_map;
    DbgErrorSnapshot *active_error; DbgErrorSnapshot **error_stack;
    size_t error_stack_count, error_stack_cap;
    DbgSourceFile *source; DbgIrPanel *ir_panel; DbgVarInspector *vars;
    DbgDisasmPanel *disasm;
    DbgThreadInfo *threads; size_t thread_count;
    DbgStackFrameInfo *runtime_frames; size_t runtime_frame_count;
    uint64_t runtime_total_frames;
    DbgPanelKind focused_panel; DbgRect panel_rects[DBG_MAX_PANELS];
    int32_t source_scroll, source_cursor_line;
    int32_t locals_scroll, locals_selected;
    int32_t backtrace_scroll, backtrace_selected;
    int32_t breakpoints_scroll, breakpoints_selected;
    int32_t console_scroll;
    DbgPalette *palette; bool palette_open;
    char *console_log; size_t console_log_len, console_log_cap;
    char *target_stdout_log; size_t target_stdout_len, target_stdout_cap;
    char *target_stderr_log; size_t target_stderr_len, target_stderr_cap;
    DbgStopReason last_stop_reason; uint64_t last_stop_address;
    bool running, dirty;
};

struct DbgCodeMap {
    DbgCodeLocation *entries; size_t count, cap;
    DbgCodeLocation **address_index; uint64_t *address_prefix_max_end;
    DbgCodeLocation **source_index; bool finalized;
};

static DbgKeymap *dbg_keymap_create_default(void);
static void dbg_keymap_free(DbgKeymap *km);
static DbgTheme *dbg_theme_default(void);
static void dbg_theme_free(DbgTheme *theme);
static DbgSourceMap *dbg_source_map_load(const char *path);
static void dbg_source_map_free(DbgSourceMap *map);
static DbgIrPanel *dbg_ir_panel_emit(const char *emit_ir_command, const char *source_path, char **error_out);
static void dbg_ir_panel_free(DbgIrPanel *panel);


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
        if (b->cap == 0) b->cap = DBG_INITIAL_BUF;
        while (b->cap < b->len + extra + 1)
            b->cap *= DBG_GROW_FACTOR;
        b->data = dbg_xrealloc(b->data, b->cap);
        if (b->len == 0) b->data[0] = '\0';
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
    if (n < 0) return;
    sb_ensure(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

static char *sb_take(DbgStrBuf *b)
{
    /* True ownership transfer: leave the builder empty instead of allocating
       a replacement buffer that direct-return call sites could accidentally
       leak.  sb_ensure() also supports cap==0, so the builder remains reusable. */
    char *s = b->data;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return s;
}

static size_t dbg_encode_utf8(uint32_t cp, char out[4])
{
    /* Reject surrogate code points and values outside Unicode scalar range. */
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu;
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

static size_t dbg_utf8_prev_boundary(const char *s, size_t pos)
{
    if (!s || pos == 0) return 0;
    size_t p = pos - 1;
    while (p > 0 && (((unsigned char)s[p] & 0xC0u) == 0x80u)) p--;
    return p;
}

static size_t dbg_utf8_next_boundary(const char *s, size_t len, size_t pos)
{
    if (!s || pos >= len) return len;
    size_t p = pos + 1;
    while (p < len && (((unsigned char)s[p] & 0xC0u) == 0x80u)) p++;
    return p;
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


/// §E  Debug engine core — reusable independently of the terminal UI

/*
 * The engine is the semantic boundary of the debugger.  Backends provide
 * target-specific operations; clients (the TUI, a future DAP adapter, tests)
 * consume one normalized model.  Stop-scoped objects are tagged with a
 * monotonically increasing snapshot_epoch so frontends can cheaply discard
 * stale frames/scopes/values after execution resumes or memory mutates.
 */
typedef struct {
    uint64_t       id;
    DbgThreadState state;
} DbgTrackedThread;

struct DbgEngine {
    const DbgBackendOps *ops;          /* borrowed, normally static          */
    void                *backend_ctx;
    bool                 owns_backend;
    DbgCapabilities      capabilities;

    DbgProcessState      process_state;
    uint64_t             process_id;
    uint64_t             selected_thread_id;
    uint64_t             selected_frame_id;
    uint64_t             snapshot_epoch;
    uint64_t             state_epoch;
    uint64_t             next_event_sequence;

    DbgTrackedThread    *threads;
    size_t               thread_count;
    size_t               thread_cap;

    DbgBreakpoint       *breakpoints;
    size_t               breakpoint_count;
    size_t               breakpoint_cap;
    uint32_t             next_breakpoint_id;

    DbgWatch            *watches;
    size_t               watch_count;
    size_t               watch_cap;
    uint32_t             next_watch_id;
};

static void dbg_error_out_reset(char **error_out)
{
    if (error_out) *error_out = NULL;
}

static void dbg_error_out_set(char **error_out, const char *message)
{
    if (!error_out) return;
    *error_out = dbg_xstrdup(message ? message : "debugger operation failed");
}

static void dbg_error_out_setf(char **error_out, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void dbg_error_out_setf(char **error_out, const char *fmt, ...)
{
    if (!error_out) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        dbg_error_out_set(error_out, "debugger operation failed");
        return;
    }
    char *text = dbg_xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(text, (size_t)n + 1, fmt, ap);
    va_end(ap);
    *error_out = text;
}

static inline bool dbg_engine_require(DbgEngine *e, DbgCapabilities capability,
                               bool callback_present, char **error_out,
                               const char *operation)
{
    dbg_error_out_reset(error_out);
    if (!e) {
        dbg_error_out_set(error_out, "debugger engine is NULL");
        return false;
    }
    if (capability && ((e->capabilities & capability) != capability)) {
        dbg_error_out_setf(error_out, "%s is not supported by backend '%s'",
                           operation,
                           (e->ops && e->ops->name) ? e->ops->name : "none");
        return false;
    }
    if (!callback_present) {
        dbg_error_out_setf(error_out, "%s has no backend implementation",
                           operation);
        return false;
    }
    return true;
}

typedef uint32_t DbgProcessStateMask;
#define DBG_STATE_BIT(state_) (UINT32_C(1) << (unsigned)(state_))

static const char *dbg_process_state_name(DbgProcessState state)
{
    switch (state) {
    case DBG_PROCESS_NONE:              return "no-target";
    case DBG_PROCESS_LAUNCHING:         return "launching";
    case DBG_PROCESS_ATTACHING:         return "attaching";
    case DBG_PROCESS_STOPPED:           return "stopped";
    case DBG_PROCESS_PARTIALLY_STOPPED: return "partially-stopped";
    case DBG_PROCESS_RUNNING:           return "running";
    case DBG_PROCESS_EXITED:            return "exited";
    case DBG_PROCESS_DETACHED:          return "detached";
    case DBG_PROCESS_CRASHED:           return "crashed";
    }
    return "unknown";
}

static bool dbg_engine_require_process_state(DbgEngine *e,
                                             DbgProcessStateMask allowed,
                                             char **error_out,
                                             const char *operation)
{
    dbg_error_out_reset(error_out);
    if (!e) {
        dbg_error_out_set(error_out, "debugger engine is NULL");
        return false;
    }
    if ((unsigned)e->process_state >= 32u ||
        !(allowed & DBG_STATE_BIT(e->process_state))) {
        dbg_error_out_setf(error_out, "%s is invalid while target is %s",
                           operation, dbg_process_state_name(e->process_state));
        return false;
    }
    return true;
}

static const DbgProcessStateMask DBG_STATES_IDLE =
    DBG_STATE_BIT(DBG_PROCESS_NONE) |
    DBG_STATE_BIT(DBG_PROCESS_EXITED) |
    DBG_STATE_BIT(DBG_PROCESS_DETACHED);

static const DbgProcessStateMask DBG_STATES_STOPPED =
    DBG_STATE_BIT(DBG_PROCESS_STOPPED) |
    DBG_STATE_BIT(DBG_PROCESS_PARTIALLY_STOPPED) |
    DBG_STATE_BIT(DBG_PROCESS_CRASHED);

static const DbgProcessStateMask DBG_STATES_RUNNING =
    DBG_STATE_BIT(DBG_PROCESS_RUNNING) |
    DBG_STATE_BIT(DBG_PROCESS_PARTIALLY_STOPPED);

static const DbgProcessStateMask DBG_STATES_ACTIVE =
    DBG_STATE_BIT(DBG_PROCESS_LAUNCHING) |
    DBG_STATE_BIT(DBG_PROCESS_ATTACHING) |
    DBG_STATE_BIT(DBG_PROCESS_STOPPED) |
    DBG_STATE_BIT(DBG_PROCESS_PARTIALLY_STOPPED) |
    DBG_STATE_BIT(DBG_PROCESS_RUNNING) |
    DBG_STATE_BIT(DBG_PROCESS_CRASHED);

static DbgTrackedThread *dbg_engine_tracked_thread(DbgEngine *e,
                                                   uint64_t thread_id,
                                                   bool create)
{
    if (!e || thread_id == 0 || thread_id == DBG_INVALID_ID) return NULL;
    for (size_t i = 0; i < e->thread_count; i++)
        if (e->threads[i].id == thread_id) return &e->threads[i];
    if (!create) return NULL;
    DBG_GROW(e->threads, e->thread_count, e->thread_cap, DbgTrackedThread);
    DbgTrackedThread *tracked = &e->threads[e->thread_count++];
    tracked->id = thread_id;
    tracked->state = DBG_THREAD_UNKNOWN;
    return tracked;
}

static void dbg_engine_track_thread_state(DbgEngine *e, uint64_t thread_id,
                                          DbgThreadState state)
{
    DbgTrackedThread *tracked = dbg_engine_tracked_thread(e, thread_id, true);
    if (tracked) tracked->state = state;
}

static void dbg_engine_track_all_threads(DbgEngine *e, DbgThreadState state)
{
    if (!e) return;
    for (size_t i = 0; i < e->thread_count; i++)
        if (e->threads[i].state != DBG_THREAD_EXITED)
            e->threads[i].state = state;
}

static void dbg_engine_clear_thread_tracking(DbgEngine *e)
{
    if (!e) return;
    e->thread_count = 0;
    e->selected_thread_id = DBG_INVALID_ID;
    e->selected_frame_id = DBG_INVALID_ID;
}

static bool dbg_engine_require_stopped_thread(DbgEngine *e,
                                              uint64_t thread_id,
                                              char **error_out,
                                              const char *operation)
{
    if (!dbg_engine_require_process_state(e, DBG_STATES_STOPPED,
                                          error_out, operation))
        return false;
    if (thread_id == 0 || thread_id == DBG_INVALID_ID) {
        dbg_error_out_setf(error_out, "%s requires a valid thread id", operation);
        return false;
    }
    if (e->process_state == DBG_PROCESS_STOPPED ||
        e->process_state == DBG_PROCESS_CRASHED)
        return true;

    DbgTrackedThread *tracked = dbg_engine_tracked_thread(e, thread_id, false);
    if (tracked && tracked->state == DBG_THREAD_RUNNING) {
        dbg_error_out_setf(error_out,
            "%s requires a stopped thread; thread %llu is running",
            operation, (unsigned long long)thread_id);
        return false;
    }
    if (tracked && tracked->state == DBG_THREAD_EXITED) {
        dbg_error_out_setf(error_out,
            "%s cannot use exited thread %llu", operation,
            (unsigned long long)thread_id);
        return false;
    }
    return true;
}

static inline bool dbg_engine_snapshot_query_allowed(DbgEngine *e,
                                                      char **error_out,
                                                      const char *operation)
{
    if (!e) {
        dbg_error_out_reset(error_out);
        dbg_error_out_set(error_out, "debugger engine is NULL");
        return false;
    }

    if (DBG_STATES_STOPPED & DBG_STATE_BIT(e->process_state)) return true;

    const DbgProcessStateMask transitional_or_running =
        DBG_STATE_BIT(DBG_PROCESS_RUNNING) |
        DBG_STATE_BIT(DBG_PROCESS_LAUNCHING) |
        DBG_STATE_BIT(DBG_PROCESS_ATTACHING);
    if ((transitional_or_running & DBG_STATE_BIT(e->process_state)) &&
        (e->capabilities & DBG_CAP_QUERY_WHILE_RUNNING))
        return true;

    dbg_error_out_reset(error_out);
    dbg_error_out_setf(error_out, "%s requires an inspectable target; target is %s",
                       operation, dbg_process_state_name(e->process_state));
    return false;
}

static bool dbg_engine_stopped_operation_allowed(DbgEngine *e,
                                                 char **error_out,
                                                 const char *operation)
{
    return dbg_engine_require_process_state(e, DBG_STATES_STOPPED,
                                            error_out, operation);
}

static bool dbg_engine_thread_query_allowed(DbgEngine *e, uint64_t thread_id,
                                            char **error_out,
                                            const char *operation)
{
    if (!dbg_engine_snapshot_query_allowed(e, error_out, operation))
        return false;
    if (e->process_state != DBG_PROCESS_PARTIALLY_STOPPED) return true;
    DbgTrackedThread *tracked = dbg_engine_tracked_thread(e, thread_id, false);
    if (tracked && tracked->state == DBG_THREAD_RUNNING) {
        dbg_error_out_setf(error_out,
            "%s requires a stopped thread; thread %llu is running",
            operation, (unsigned long long)thread_id);
        return false;
    }
    return true;
}

static void dbg_engine_touch(DbgEngine *e, bool invalidate_snapshot)
{
    if (!e) return;
    e->state_epoch++;
    if (invalidate_snapshot) e->snapshot_epoch++;
}

static bool dbg_backend_validate(const DbgBackendOps *ops, char **error_out)
{
    dbg_error_out_reset(error_out);
    if (!ops) return true; /* model-only engine is valid */
    if (ops->abi_version != DBG_BACKEND_ABI_VERSION) {
        dbg_error_out_setf(error_out,
            "backend '%s' uses ABI %u; debugger requires ABI %u",
            ops->name ? ops->name : "<unnamed>", ops->abi_version,
            DBG_BACKEND_ABI_VERSION);
        return false;
    }
    if (ops->struct_size != sizeof(*ops)) {
        dbg_error_out_setf(error_out,
            "backend '%s' uses ops size %u; ABI %u requires %zu bytes",
            ops->name ? ops->name : "<unnamed>", ops->struct_size,
            DBG_BACKEND_ABI_VERSION, sizeof(*ops));
        return false;
    }

#define REQUIRE_CB(cap_, member_) do {                                         \
        if ((ops->capabilities & (cap_)) && !ops->member_) {                    \
            dbg_error_out_setf(error_out,                                       \
                "backend '%s' advertises %s but does not implement %s",         \
                ops->name ? ops->name : "<unnamed>", #cap_, #member_);          \
            return false;                                                       \
        }                                                                       \
    } while (0)
    REQUIRE_CB(DBG_CAP_TARGET_INFO, target_info);
    REQUIRE_CB(DBG_CAP_LAUNCH, launch);
    REQUIRE_CB(DBG_CAP_ATTACH, attach);
    REQUIRE_CB(DBG_CAP_RESTART, restart);
    REQUIRE_CB(DBG_CAP_TERMINATE, terminate);
    REQUIRE_CB(DBG_CAP_DETACH, detach);
    REQUIRE_CB(DBG_CAP_PAUSE, pause);
    if (ops->capabilities & DBG_CAP_EXECUTION_CONTROL) {
        REQUIRE_CB(DBG_CAP_EXECUTION_CONTROL, resume);
        REQUIRE_CB(DBG_CAP_EXECUTION_CONTROL, step);
    }
    REQUIRE_CB(DBG_CAP_RESTART_FRAME, restart_frame);
    REQUIRE_CB(DBG_CAP_STEP_IN_TARGETS, step_in_targets);
    if (ops->capabilities & DBG_CAP_GOTO) {
        REQUIRE_CB(DBG_CAP_GOTO, goto_targets);
        REQUIRE_CB(DBG_CAP_GOTO, goto_target);
    }
    if (ops->capabilities & (DBG_CAP_SOURCE_BREAKPOINTS |
                             DBG_CAP_FUNCTION_BREAKPOINTS |
                             DBG_CAP_INSTRUCTION_BREAKPOINTS |
                             DBG_CAP_DATA_BREAKPOINTS |
                             DBG_CAP_EXCEPTION_BREAKPOINTS)) {
        if (!ops->set_breakpoint || !ops->remove_breakpoint) {
            dbg_error_out_setf(error_out,
                "backend '%s' advertises breakpoints without set/remove callbacks",
                ops->name ? ops->name : "<unnamed>");
            return false;
        }
    }
    REQUIRE_CB(DBG_CAP_BREAKPOINT_LOCATIONS, breakpoint_locations);
    REQUIRE_CB(DBG_CAP_DATA_BREAKPOINTS, data_breakpoint_info);
    REQUIRE_CB(DBG_CAP_EVALUATE, evaluate);
    REQUIRE_CB(DBG_CAP_SET_EXPRESSION, set_expression);
    REQUIRE_CB(DBG_CAP_SET_VARIABLE, set_variable);
    REQUIRE_CB(DBG_CAP_COMPLETIONS, completions);
    REQUIRE_CB(DBG_CAP_READ_MEMORY, read_memory);
    REQUIRE_CB(DBG_CAP_WRITE_MEMORY, write_memory);
    REQUIRE_CB(DBG_CAP_DISASSEMBLE, disassemble);
    REQUIRE_CB(DBG_CAP_REGISTERS, registers);
    REQUIRE_CB(DBG_CAP_MODULES, modules);
    REQUIRE_CB(DBG_CAP_CORE_DUMP, load_core);
    REQUIRE_CB(DBG_CAP_REMOTE, connect_remote);
    REQUIRE_CB(DBG_CAP_LOADED_SOURCES, loaded_sources);
    REQUIRE_CB(DBG_CAP_SOURCE_CONTENT, source_content);
    REQUIRE_CB(DBG_CAP_LOGICAL_TASKS, tasks);
    if (ops->capabilities & DBG_CAP_COMPILER_PIPELINE) {
        REQUIRE_CB(DBG_CAP_COMPILER_PIPELINE, compiler_stages);
        REQUIRE_CB(DBG_CAP_COMPILER_PIPELINE, semantic_node);
        REQUIRE_CB(DBG_CAP_COMPILER_PIPELINE, semantic_children);
    }
    if (ops->capabilities & DBG_CAP_CHECKPOINTS) {
        REQUIRE_CB(DBG_CAP_CHECKPOINTS, checkpoint);
        REQUIRE_CB(DBG_CAP_CHECKPOINTS, restore_checkpoint);
    }
    if ((ops->capabilities & DBG_CAP_REVERSE_CONTINUE) && !ops->resume) {
        dbg_error_out_set(error_out,
            "backend advertises reverse-continue without resume callback");
        return false;
    }
    if ((ops->capabilities & DBG_CAP_STEP_BACK) && !ops->step) {
        dbg_error_out_set(error_out,
            "backend advertises step-back without step callback");
        return false;
    }

    const DbgCapabilities breakpoint_modifiers =
        DBG_CAP_CONDITIONAL_BREAKPOINTS | DBG_CAP_HIT_CONDITIONS |
        DBG_CAP_LOGPOINTS;
    const DbgCapabilities breakpoint_kinds =
        DBG_CAP_SOURCE_BREAKPOINTS | DBG_CAP_FUNCTION_BREAKPOINTS |
        DBG_CAP_INSTRUCTION_BREAKPOINTS | DBG_CAP_DATA_BREAKPOINTS |
        DBG_CAP_EXCEPTION_BREAKPOINTS;
    if ((ops->capabilities & breakpoint_modifiers) &&
        !(ops->capabilities & breakpoint_kinds)) {
        dbg_error_out_set(error_out,
            "backend advertises breakpoint modifiers without a breakpoint kind");
        return false;
    }
    if ((ops->capabilities & (DBG_CAP_NON_STOP | DBG_CAP_SINGLE_THREAD_EXEC |
                              DBG_CAP_REVERSE_CONTINUE | DBG_CAP_STEP_BACK)) &&
        !(ops->capabilities & DBG_CAP_EXECUTION_CONTROL)) {
        dbg_error_out_set(error_out,
            "backend advertises advanced execution without execution-control");
        return false;
    }

    /* Live execution is event-driven by contract. Request callbacks only
       initiate transitions; STOPPED/CONTINUED/EXITED events commit them. */
    const DbgCapabilities event_driven =
        DBG_CAP_LAUNCH | DBG_CAP_ATTACH | DBG_CAP_RESTART |
        DBG_CAP_TERMINATE | DBG_CAP_EXECUTION_CONTROL | DBG_CAP_PAUSE |
        DBG_CAP_RESTART_FRAME | DBG_CAP_GOTO;
    if ((ops->capabilities & event_driven) && !ops->poll_event) {
        dbg_error_out_set(error_out,
            "backend advertises live execution but has no event stream");
        return false;
    }
#undef REQUIRE_CB
    return true;
}

/* ---- Owned model copy/free helpers -------------------------------------- */

static void dbg_source_location_copy(DbgSourceLocation *dst,
                                     const DbgSourceLocation *src)
{
    memset(dst, 0, sizeof(*dst));
    if (!src) return;
    *dst = *src;
    dst->file = src->file ? dbg_xstrdup(src->file) : NULL;
    dst->checksum = src->checksum ? dbg_xstrdup(src->checksum) : NULL;
}

static void dbg_code_location_copy(DbgCodeLocation *dst,
                                   const DbgCodeLocation *src)
{
    memset(dst, 0, sizeof(*dst));
    if (!src) return;
    *dst = *src;
    dbg_source_location_copy(&dst->source, &src->source);
    dst->function = src->function ? dbg_xstrdup(src->function) : NULL;
    dst->ir_function = src->ir_function ? dbg_xstrdup(src->ir_function) : NULL;
    dst->ir_value = src->ir_value ? dbg_xstrdup(src->ir_value) : NULL;
}

static void dbg_value_info_copy(DbgValueInfo *dst, const DbgValueInfo *src)
{
    memset(dst, 0, sizeof(*dst));
    if (!src) return;
    *dst = *src;
    dst->name = src->name ? dbg_xstrdup(src->name) : NULL;
    dst->type_name = src->type_name ? dbg_xstrdup(src->type_name) : NULL;
    dst->value = src->value ? dbg_xstrdup(src->value) : NULL;
    dst->summary = src->summary ? dbg_xstrdup(src->summary) : NULL;
    dst->evaluate_name = src->evaluate_name ? dbg_xstrdup(src->evaluate_name) : NULL;
    dst->memory_reference = src->memory_reference
                          ? dbg_xstrdup(src->memory_reference) : NULL;
    dbg_source_location_copy(&dst->declaration, &src->declaration);
}

void dbg_source_location_free(DbgSourceLocation *location)
{
    if (!location) return;
    free(location->file);
    free(location->checksum);
    memset(location, 0, sizeof(*location));
}

void dbg_code_location_free(DbgCodeLocation *location)
{
    if (!location) return;
    dbg_source_location_free(&location->source);
    free(location->function);
    free(location->ir_function);
    free(location->ir_value);
    memset(location, 0, sizeof(*location));
}

void dbg_target_info_free(DbgTargetInfo *target)
{
    if (!target) return;
    free(target->name);
    free(target->executable);
    free(target->architecture);
    free(target->triple);
    free(target->os_abi);
    memset(target, 0, sizeof(*target));
}

void dbg_thread_infos_free(DbgThreadInfo *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].name);
        free(items[i].stop_description);
    }
    free(items);
}

void dbg_stack_frame_infos_free(DbgStackFrameInfo *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].name);
        dbg_code_location_free(&items[i].location);
    }
    free(items);
}

void dbg_scope_infos_free(DbgScopeInfo *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].name);
        free(items[i].presentation_hint);
    }
    free(items);
}

void dbg_value_info_free(DbgValueInfo *value)
{
    if (!value) return;
    free(value->name);
    free(value->type_name);
    free(value->value);
    free(value->summary);
    free(value->evaluate_name);
    free(value->memory_reference);
    dbg_source_location_free(&value->declaration);
    memset(value, 0, sizeof(*value));
}

void dbg_value_infos_free(DbgValueInfo *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) dbg_value_info_free(&items[i]);
    free(items);
}

void dbg_completion_items_free(DbgCompletionItem *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].label);
        free(items[i].text);
        free(items[i].type);
        free(items[i].detail);
    }
    free(items);
}

void dbg_module_infos_free(DbgModuleInfo *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].name);
        free(items[i].path);
        free(items[i].uuid);
    }
    free(items);
}

void dbg_source_infos_free(DbgSourceInfo *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].name);
        free(items[i].path);
        free(items[i].origin);
        free(items[i].checksum);
        free(items[i].mime_type);
    }
    free(items);
}

void dbg_register_infos_free(DbgRegisterInfo *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].name);
        free(items[i].value);
    }
    free(items);
}

void dbg_instruction_infos_free(DbgInstructionInfo *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].bytes_hex);
        free(items[i].mnemonic);
        free(items[i].operands);
        dbg_code_location_free(&items[i].location);
    }
    free(items);
}

void dbg_task_infos_free(DbgTaskInfo *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].name);
        dbg_code_location_free(&items[i].spawn_location);
    }
    free(items);
}

void dbg_compiler_stage_infos_free(DbgCompilerStageInfo *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].name);
        free(items[i].kind);
        dbg_source_location_free(&items[i].location);
    }
    free(items);
}

void dbg_semantic_node_info_free(DbgSemanticNodeInfo *node)
{
    if (!node) return;
    free(node->kind);
    free(node->name);
    free(node->type_name);
    free(node->rendered);
    dbg_source_location_free(&node->location);
    memset(node, 0, sizeof(*node));
}

void dbg_semantic_node_infos_free(DbgSemanticNodeInfo *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) dbg_semantic_node_info_free(&items[i]);
    free(items);
}

void dbg_step_in_targets_free(DbgStepInTarget *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].label);
        dbg_code_location_free(&items[i].location);
    }
    free(items);
}

void dbg_goto_targets_free(DbgGotoTarget *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].label);
        dbg_code_location_free(&items[i].location);
    }
    free(items);
}

void dbg_data_breakpoint_info_free(DbgDataBreakpointInfo *info)
{
    if (!info) return;
    free(info->data_id);
    free(info->description);
    memset(info, 0, sizeof(*info));
}

void dbg_breakpoint_candidates_free(DbgBreakpointCandidate *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        dbg_code_location_free(&items[i].location);
        free(items[i].message);
    }
    free(items);
}

void dbg_engine_event_free(DbgEngineEvent *event)
{
    if (!event) return;
    switch (event->kind) {
    case DBG_ENGINE_EVENT_STOPPED:
        free(event->as.stopped.description);
        dbg_code_location_free(&event->as.stopped.location);
        break;
    case DBG_ENGINE_EVENT_OUTPUT:
        free(event->as.output.text);
        dbg_code_location_free(&event->as.output.location);
        break;
    case DBG_ENGINE_EVENT_MODULE_LOADED:
    case DBG_ENGINE_EVENT_MODULE_UNLOADED:
        free(event->as.module.path);
        break;
    case DBG_ENGINE_EVENT_BREAKPOINT:
        free(event->as.breakpoint.message);
        break;
    case DBG_ENGINE_EVENT_COMPILER:
        free(event->as.compiler.message);
        break;
    default:
        break;
    }
    memset(event, 0, sizeof(*event));
}

/* ---- Breakpoint ownership and synchronization --------------------------- */

static void dbg_breakpoint_site_free(DbgBreakpointSite *site)
{
    if (!site) return;
    dbg_code_location_free(&site->location);
    free(site->message);
    memset(site, 0, sizeof(*site));
}

static void dbg_breakpoint_sites_free(DbgBreakpointSite *sites, size_t count)
{
    if (!sites) return;
    for (size_t i = 0; i < count; i++) dbg_breakpoint_site_free(&sites[i]);
    free(sites);
}

static void dbg_breakpoint_clear_sites(DbgBreakpoint *bp)
{
    if (!bp) return;
    dbg_breakpoint_sites_free(bp->sites, bp->site_count);
    bp->sites = NULL;
    bp->site_count = 0;
    bp->verified = false;
    free(bp->verification_message);
    bp->verification_message = NULL;
}

static void dbg_breakpoint_free_fields(DbgBreakpoint *bp)
{
    if (!bp) return;
    free(bp->condition);
    free(bp->hit_condition);
    free(bp->log_message);
    free(bp->verification_message);
    switch (bp->kind) {
    case DBG_BP_SOURCE:
        dbg_source_location_free(&bp->as.source.location);
        break;
    case DBG_BP_FUNCTION:
        free(bp->as.function.symbol);
        break;
    case DBG_BP_IR_VALUE:
        free(bp->as.ir.function);
        free(bp->as.ir.value);
        break;
    case DBG_BP_INSTRUCTION:
        break;
    case DBG_BP_DATA:
        free(bp->as.data.data_id);
        break;
    case DBG_BP_EXCEPTION:
        free(bp->as.exception.filter);
        break;
    }
    dbg_breakpoint_sites_free(bp->sites, bp->site_count);
    memset(bp, 0, sizeof(*bp));
}

static DbgCapabilities dbg_breakpoint_capability(DbgBreakpointKind kind)
{
    switch (kind) {
    case DBG_BP_SOURCE:      return DBG_CAP_SOURCE_BREAKPOINTS;
    case DBG_BP_FUNCTION:    return DBG_CAP_FUNCTION_BREAKPOINTS;
    case DBG_BP_IR_VALUE:    return DBG_CAP_SOURCE_BREAKPOINTS;
    case DBG_BP_INSTRUCTION: return DBG_CAP_INSTRUCTION_BREAKPOINTS;
    case DBG_BP_DATA:        return DBG_CAP_DATA_BREAKPOINTS;
    case DBG_BP_EXCEPTION:   return DBG_CAP_EXCEPTION_BREAKPOINTS;
    }
    return 0;
}

static DbgBreakpoint *dbg_engine_find_breakpoint(DbgEngine *e, uint32_t id)
{
    if (!e) return NULL;
    for (size_t i = 0; i < e->breakpoint_count; i++)
        if (e->breakpoints[i].id == id) return &e->breakpoints[i];
    return NULL;
}

static bool dbg_engine_sync_breakpoint(DbgEngine *e, DbgBreakpoint *bp,
                                       char **error_out)
{
    dbg_error_out_reset(error_out);
    if (!e || !bp) {
        dbg_error_out_set(error_out, "invalid breakpoint synchronization request");
        return false;
    }
    if (!bp->enabled) return true;

    DbgCapabilities cap = dbg_breakpoint_capability(bp->kind);
    if (!e->ops || !e->ops->set_breakpoint ||
        (cap && !(e->capabilities & cap))) {
        /* Model-only and pre-launch breakpoints remain valid pending requests. */
        bp->verified = false;
        return true;
    }

    /* Transactional replacement: a failed re-resolution must not destroy a
       previously valid location set. */
    DbgBreakpointSite *old_sites = bp->sites;
    size_t old_site_count = bp->site_count;
    bool old_verified = bp->verified;
    char *old_message = bp->verification_message;
    bp->sites = NULL;
    bp->site_count = 0;
    bp->verified = false;
    bp->verification_message = NULL;

    bool ok = e->ops->set_breakpoint(e->backend_ctx, bp, error_out);
    if (ok) {
        dbg_breakpoint_sites_free(old_sites, old_site_count);
        free(old_message);
        if (bp->site_count > 0 && !bp->verified) bp->verified = true;
        return true;
    }

    dbg_breakpoint_sites_free(bp->sites, bp->site_count);
    free(bp->verification_message);
    bp->sites = old_sites;
    bp->site_count = old_site_count;
    bp->verified = old_verified;
    bp->verification_message = old_message;
    return false;
}

static void dbg_engine_resolve_breakpoints(DbgEngine *e)
{
    if (!e) return;
    for (size_t i = 0; i < e->breakpoint_count; i++) {
        DbgBreakpoint *bp = &e->breakpoints[i];
        if (!bp->enabled) continue;
        char *err = NULL;
        (void)dbg_engine_sync_breakpoint(e, bp, &err);
        free(err); /* asynchronous topology events have no synchronous sink */
    }
}

static void dbg_engine_note_breakpoint_hit(DbgEngine *e, uint32_t id,
                                           uint64_t address)
{
    if (!e || id == 0) return;
    DbgBreakpoint *bp = dbg_engine_find_breakpoint(e, id);
    if (!bp) return;
    bp->hit_count++;
    for (size_t i = 0; i < bp->site_count; i++) {
        if (bp->sites[i].resolved &&
            (address == 0 || address == DBG_INVALID_ADDRESS ||
             bp->sites[i].location.address == address)) {
            bp->sites[i].hit_count++;
            if (address != 0 && address != DBG_INVALID_ADDRESS) break;
        }
    }
}

/* ---- Engine construction / state ---------------------------------------- */

DbgEngine *dbg_engine_create_checked(const DbgBackendOps *ops, void *backend_ctx,
                                     bool take_backend_ownership,
                                     char **error_out)
{
    if (!dbg_backend_validate(ops, error_out)) return NULL;
    DbgEngine *e = dbg_xcalloc(1, sizeof(*e));
    e->ops = ops;
    e->backend_ctx = backend_ctx;
    e->owns_backend = take_backend_ownership;
    e->capabilities = ops ? ops->capabilities : 0;
    e->process_state = DBG_PROCESS_NONE;
    e->process_id = DBG_INVALID_ID;
    e->selected_thread_id = DBG_INVALID_ID;
    e->selected_frame_id = DBG_INVALID_ID;
    e->snapshot_epoch = 1;
    e->state_epoch = 1;
    e->next_event_sequence = 1;
    e->next_breakpoint_id = 1;
    e->next_watch_id = 1;
    return e;
}

DbgEngine *dbg_engine_create(const DbgBackendOps *ops, void *backend_ctx,
                             bool take_backend_ownership)
{
    return dbg_engine_create_checked(ops, backend_ctx, take_backend_ownership,
                                     NULL);
}

void dbg_engine_free(DbgEngine *e)
{
    if (!e) return;
    for (size_t i = 0; i < e->breakpoint_count; i++)
        dbg_breakpoint_free_fields(&e->breakpoints[i]);
    free(e->breakpoints);
    for (size_t i = 0; i < e->watch_count; i++) {
        free(e->watches[i].expression);
        free(e->watches[i].last_value);
        free(e->watches[i].last_error);
    }
    free(e->watches);
    free(e->threads);
    if (e->owns_backend && e->ops && e->ops->destroy)
        e->ops->destroy(e->backend_ctx);
    free(e);
}

const char *dbg_engine_backend_name(const DbgEngine *e)
{
    return (e && e->ops && e->ops->name) ? e->ops->name : "model-only";
}

DbgCapabilities dbg_engine_capabilities(const DbgEngine *e)
{ return e ? e->capabilities : 0; }

bool dbg_engine_has_capability(const DbgEngine *e, DbgCapabilities cap)
{ return e && ((e->capabilities & cap) == cap); }

DbgProcessState dbg_engine_process_state(const DbgEngine *e)
{ return e ? e->process_state : DBG_PROCESS_NONE; }

uint64_t dbg_engine_process_id(const DbgEngine *e)
{ return e ? e->process_id : DBG_INVALID_ID; }

uint64_t dbg_engine_selected_thread(const DbgEngine *e)
{ return e ? e->selected_thread_id : DBG_INVALID_ID; }

uint64_t dbg_engine_selected_frame(const DbgEngine *e)
{ return e ? e->selected_frame_id : DBG_INVALID_ID; }

DbgThreadState dbg_engine_thread_state(const DbgEngine *e, uint64_t thread_id)
{
    if (!e || thread_id == 0 || thread_id == DBG_INVALID_ID)
        return DBG_THREAD_UNKNOWN;
    for (size_t i = 0; i < e->thread_count; i++)
        if (e->threads[i].id == thread_id) return e->threads[i].state;
    return DBG_THREAD_UNKNOWN;
}

uint64_t dbg_engine_snapshot_epoch(const DbgEngine *e)
{ return e ? e->snapshot_epoch : 0; }

uint64_t dbg_engine_state_epoch(const DbgEngine *e)
{ return e ? e->state_epoch : 0; }

void dbg_engine_select_thread(DbgEngine *e, uint64_t thread_id)
{
    if (!e || e->selected_thread_id == thread_id) return;
    e->selected_thread_id = thread_id;
    e->selected_frame_id = DBG_INVALID_ID;
    dbg_engine_touch(e, false);
}

void dbg_engine_select_frame(DbgEngine *e, uint64_t frame_id)
{
    if (!e || e->selected_frame_id == frame_id) return;
    e->selected_frame_id = frame_id;
    dbg_engine_touch(e, false);
}

bool dbg_engine_target_info(DbgEngine *e, DbgTargetInfo *out,
                            char **error_out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!dbg_engine_require(e, DBG_CAP_TARGET_INFO,
                            e && e->ops && e->ops->target_info,
                            error_out, "target-info")) return false;
    if (!out) {
        dbg_error_out_set(error_out, "target-info output is NULL");
        return false;
    }
    return e->ops->target_info(e->backend_ctx, out, error_out);
}

bool dbg_engine_launch(DbgEngine *e, const DbgLaunchSpec *spec, char **error_out)
{
    if (!dbg_engine_require(e, DBG_CAP_LAUNCH,
                            e && e->ops && e->ops->launch,
                            error_out, "launch")) return false;
    if (!dbg_engine_require_process_state(e, DBG_STATES_IDLE,
                                          error_out, "launch")) return false;
    if (!spec || !spec->program || !*spec->program) {
        dbg_error_out_set(error_out, "launch requires a program");
        return false;
    }
    DbgProcessState old = e->process_state;
    e->process_state = DBG_PROCESS_LAUNCHING;
    dbg_engine_touch(e, true);
    if (!e->ops->launch(e->backend_ctx, spec, error_out)) {
        e->process_state = old;
        dbg_engine_touch(e, false);
        return false;
    }
    dbg_engine_clear_thread_tracking(e);
    return true;
}

bool dbg_engine_attach(DbgEngine *e, const DbgAttachSpec *spec, char **error_out)
{
    if (!dbg_engine_require(e, DBG_CAP_ATTACH,
                            e && e->ops && e->ops->attach,
                            error_out, "attach")) return false;
    if (!dbg_engine_require_process_state(e, DBG_STATES_IDLE,
                                          error_out, "attach")) return false;
    if (!spec || spec->pid == 0 || spec->pid == DBG_INVALID_ID) {
        dbg_error_out_set(error_out, "attach requires a valid process id");
        return false;
    }
    DbgProcessState old = e->process_state;
    e->process_state = DBG_PROCESS_ATTACHING;
    dbg_engine_touch(e, true);
    if (!e->ops->attach(e->backend_ctx, spec, error_out)) {
        e->process_state = old;
        dbg_engine_touch(e, false);
        return false;
    }
    dbg_engine_clear_thread_tracking(e);
    return true;
}

bool dbg_engine_load_core(DbgEngine *e, const DbgCoreSpec *spec, char **error_out)
{
    if (!dbg_engine_require(e, DBG_CAP_CORE_DUMP,
                            e && e->ops && e->ops->load_core,
                            error_out, "load-core")) return false;
    if (!dbg_engine_require_process_state(e, DBG_STATES_IDLE,
                                          error_out, "load-core")) return false;
    if (!spec || !spec->core_path || !*spec->core_path) {
        dbg_error_out_set(error_out, "load-core requires a core path");
        return false;
    }
    bool ok = e->ops->load_core(e->backend_ctx, spec, error_out);
    if (ok) {
        dbg_engine_clear_thread_tracking(e);
        e->process_state = DBG_PROCESS_STOPPED;
        dbg_engine_touch(e, true);
        dbg_engine_resolve_breakpoints(e);
    }
    return ok;
}

bool dbg_engine_connect_remote(DbgEngine *e, const DbgRemoteSpec *spec,
                               char **error_out)
{
    if (!dbg_engine_require(e, DBG_CAP_REMOTE,
                            e && e->ops && e->ops->connect_remote,
                            error_out, "connect-remote")) return false;
    if (!dbg_engine_require_process_state(e, DBG_STATES_IDLE,
                                          error_out, "connect-remote")) return false;
    if (!spec || !spec->url || !*spec->url) {
        dbg_error_out_set(error_out, "connect-remote requires a URL");
        return false;
    }
    bool ok = e->ops->connect_remote(e->backend_ctx, spec, error_out);
    if (ok) {
        dbg_engine_clear_thread_tracking(e);
        dbg_engine_touch(e, true);
    }
    return ok;
}

bool dbg_engine_restart(DbgEngine *e, char **error_out)
{
    if (!dbg_engine_require(e, DBG_CAP_RESTART,
                            e && e->ops && e->ops->restart,
                            error_out, "restart")) return false;
    if (!dbg_engine_require_process_state(
            e, DBG_STATES_ACTIVE | DBG_STATE_BIT(DBG_PROCESS_EXITED),
            error_out, "restart")) return false;
    bool ok = e->ops->restart(e->backend_ctx, error_out);
    if (ok) {
        dbg_engine_clear_thread_tracking(e);
        e->process_state = DBG_PROCESS_LAUNCHING;
        dbg_engine_touch(e, true);
    }
    return ok;
}

bool dbg_engine_detach(DbgEngine *e, char **error_out)
{
    if (!dbg_engine_require(e, DBG_CAP_DETACH,
                            e && e->ops && e->ops->detach,
                            error_out, "detach")) return false;
    if (!dbg_engine_require_process_state(e, DBG_STATES_ACTIVE,
                                          error_out, "detach")) return false;
    bool ok = e->ops->detach(e->backend_ctx, error_out);
    if (ok) {
        dbg_engine_clear_thread_tracking(e);
        e->process_state = DBG_PROCESS_DETACHED;
        dbg_engine_touch(e, true);
    }
    return ok;
}

bool dbg_engine_terminate(DbgEngine *e, char **error_out)
{
    if (!dbg_engine_require(e, DBG_CAP_TERMINATE,
                            e && e->ops && e->ops->terminate,
                            error_out, "terminate")) return false;
    if (!dbg_engine_require_process_state(e, DBG_STATES_ACTIVE,
                                          error_out, "terminate")) return false;
    bool ok = e->ops->terminate(e->backend_ctx, error_out);
    if (ok) dbg_engine_touch(e, true);
    return ok;
}

bool dbg_engine_pause(DbgEngine *e, uint64_t thread_id, bool all_threads,
                      char **error_out)
{
    if (!dbg_engine_require(e, DBG_CAP_PAUSE,
                            e && e->ops && e->ops->pause,
                            error_out, "pause")) return false;
    if (!dbg_engine_require_process_state(e, DBG_STATES_RUNNING,
                                          error_out, "pause")) return false;
    if (!all_threads && !(e->capabilities & DBG_CAP_SINGLE_THREAD_EXEC)) {
        dbg_error_out_set(error_out,
            "backend does not support single-thread pause");
        return false;
    }
    if (!all_threads) {
        if (thread_id == 0 || thread_id == DBG_INVALID_ID) {
            dbg_error_out_set(error_out, "pause requires a valid thread id");
            return false;
        }
        DbgTrackedThread *tracked = dbg_engine_tracked_thread(e, thread_id, false);
        if (tracked && tracked->state == DBG_THREAD_EXITED) {
            dbg_error_out_set(error_out, "pause cannot target an exited thread");
            return false;
        }
        if (tracked && tracked->state == DBG_THREAD_STOPPED) {
            dbg_error_out_set(error_out, "pause cannot target an already stopped thread");
            return false;
        }
    }
    return e->ops->pause(e->backend_ctx, thread_id, all_threads, error_out);
}

bool dbg_engine_continue(DbgEngine *e, uint64_t thread_id, DbgRunMode mode,
                         DbgExecutionDirection direction, char **error_out)
{
    DbgCapabilities cap = DBG_CAP_EXECUTION_CONTROL;
    if (direction == DBG_DIR_REVERSE) cap |= DBG_CAP_REVERSE_CONTINUE;
    if (mode == DBG_RUN_SINGLE_THREAD) cap |= DBG_CAP_SINGLE_THREAD_EXEC;
    const char *operation = direction == DBG_DIR_REVERSE
                          ? "reverse-continue" : "continue";
    if (!dbg_engine_require(e, cap, e && e->ops && e->ops->resume,
                            error_out, operation)) return false;
    if (mode == DBG_RUN_SINGLE_THREAD) {
        if (!dbg_engine_require_stopped_thread(e, thread_id, error_out,
                                               operation)) return false;
    } else if (!dbg_engine_stopped_operation_allowed(e, error_out, operation)) {
        return false;
    }
    bool ok = e->ops->resume(e->backend_ctx, thread_id, mode, direction,
                             error_out);
    if (ok) {
        e->process_state = (mode == DBG_RUN_SINGLE_THREAD &&
                            (e->capabilities & DBG_CAP_NON_STOP))
                         ? DBG_PROCESS_PARTIALLY_STOPPED
                         : DBG_PROCESS_RUNNING;
        e->selected_frame_id = DBG_INVALID_ID;
        dbg_engine_touch(e, true);
    }
    return ok;
}

bool dbg_engine_execute_step_plan(DbgEngine *e, const DbgStepPlan *plan,
                                  char **error_out)
{
    if (!plan) {
        dbg_error_out_reset(error_out);
        dbg_error_out_set(error_out, "step plan is NULL");
        return false;
    }
    DbgCapabilities cap = DBG_CAP_EXECUTION_CONTROL;
    if (plan->direction == DBG_DIR_REVERSE) cap |= DBG_CAP_STEP_BACK;
    if (plan->run_mode == DBG_RUN_SINGLE_THREAD) cap |= DBG_CAP_SINGLE_THREAD_EXEC;
    if (plan->step_in_target_id != 0) cap |= DBG_CAP_STEP_IN_TARGETS;
    const char *operation = plan->direction == DBG_DIR_REVERSE
                          ? "step-back" : "step";
    if (!dbg_engine_require(e, cap, e && e->ops && e->ops->step,
                            error_out, operation)) return false;
    if (!dbg_engine_require_stopped_thread(e, plan->thread_id, error_out,
                                           operation)) return false;
    bool ok = e->ops->step(e->backend_ctx, plan, error_out);
    if (ok) {
        e->process_state = (plan->run_mode == DBG_RUN_SINGLE_THREAD &&
                            (e->capabilities & DBG_CAP_NON_STOP))
                         ? DBG_PROCESS_PARTIALLY_STOPPED
                         : DBG_PROCESS_RUNNING;
        e->selected_frame_id = DBG_INVALID_ID;
        dbg_engine_touch(e, true);
    }
    return ok;
}

bool dbg_engine_step(DbgEngine *e, uint64_t thread_id,
                     DbgStepAction action, DbgStepGranularity granularity,
                     DbgRunMode mode, DbgExecutionDirection direction,
                     char **error_out)
{
    DbgStepPlan plan;
    memset(&plan, 0, sizeof(plan));
    plan.thread_id = thread_id;
    plan.frame_id = e ? e->selected_frame_id : DBG_INVALID_ID;
    plan.action = action;
    plan.granularity = granularity;
    plan.run_mode = mode;
    plan.direction = direction;
    plan.skip_no_debug = true;
    return dbg_engine_execute_step_plan(e, &plan, error_out);
}

bool dbg_engine_restart_frame(DbgEngine *e, uint64_t frame_id,
                              char **error_out)
{
    if (!dbg_engine_require(e, DBG_CAP_RESTART_FRAME,
                            e && e->ops && e->ops->restart_frame,
                            error_out, "restart-frame")) return false;
    if (!dbg_engine_stopped_operation_allowed(e, error_out, "restart-frame"))
        return false;
    bool ok = e->ops->restart_frame(e->backend_ctx, frame_id, error_out);
    if (ok) {
        e->process_state = DBG_PROCESS_RUNNING;
        e->selected_frame_id = DBG_INVALID_ID;
        dbg_engine_touch(e, true);
    }
    return ok;
}

bool dbg_engine_step_in_targets(DbgEngine *e, uint64_t frame_id,
                                DbgStepInTarget **out, size_t *count,
                                char **error_out)
{
    if (out) *out = NULL;
    if (count) *count = 0;
    if (!dbg_engine_require(e, DBG_CAP_STEP_IN_TARGETS,
                            e && e->ops && e->ops->step_in_targets,
                            error_out, "step-in-targets")) return false;
    if (!dbg_engine_snapshot_query_allowed(e, error_out, "step-in-targets"))
        return false;
    return e->ops->step_in_targets(e->backend_ctx, frame_id, out, count,
                                   error_out);
}

bool dbg_engine_goto_targets(DbgEngine *e, const DbgSourceLocation *source,
                             DbgGotoTarget **out, size_t *count,
                             char **error_out)
{
    if (out) *out = NULL;
    if (count) *count = 0;
    if (!dbg_engine_require(e, DBG_CAP_GOTO,
                            e && e->ops && e->ops->goto_targets,
                            error_out, "goto-targets")) return false;
    if (!dbg_engine_snapshot_query_allowed(e, error_out, "goto-targets"))
        return false;
    return e->ops->goto_targets(e->backend_ctx, source, out, count, error_out);
}

bool dbg_engine_goto_target(DbgEngine *e, uint64_t thread_id,
                            uint64_t target_id, char **error_out)
{
    if (!dbg_engine_require(e, DBG_CAP_GOTO,
                            e && e->ops && e->ops->goto_target,
                            error_out, "goto")) return false;
    if (!dbg_engine_require_stopped_thread(e, thread_id, error_out, "goto"))
        return false;
    if (target_id == 0 || target_id == DBG_INVALID_ID) {
        dbg_error_out_set(error_out, "goto requires a valid target id");
        return false;
    }
    bool ok = e->ops->goto_target(e->backend_ctx, thread_id, target_id,
                                  error_out);
    if (ok) {
        e->process_state = DBG_PROCESS_RUNNING;
        e->selected_frame_id = DBG_INVALID_ID;
        dbg_engine_touch(e, true);
    }
    return ok;
}

int dbg_engine_event_fd(DbgEngine *e)
{
    return (e && e->ops && e->ops->event_fd)
         ? e->ops->event_fd(e->backend_ctx) : -1;
}

static void dbg_engine_apply_event(DbgEngine *e, DbgEngineEvent *ev)
{
    if (!e || !ev) return;
    bool invalidate_snapshot = false;

    switch (ev->kind) {
    case DBG_ENGINE_EVENT_PROCESS:
        if (ev->as.process.process_id &&
            ev->as.process.process_id != DBG_INVALID_ID)
            e->process_id = ev->as.process.process_id;
        e->process_state = ev->as.process.state;
        if (e->process_state == DBG_PROCESS_STOPPED ||
            e->process_state == DBG_PROCESS_CRASHED)
            dbg_engine_track_all_threads(e, DBG_THREAD_STOPPED);
        else if (e->process_state == DBG_PROCESS_RUNNING)
            dbg_engine_track_all_threads(e, DBG_THREAD_RUNNING);
        else if (e->process_state == DBG_PROCESS_EXITED ||
                 e->process_state == DBG_PROCESS_DETACHED)
            dbg_engine_track_all_threads(e, DBG_THREAD_EXITED);
        invalidate_snapshot = true;
        break;

    case DBG_ENGINE_EVENT_STOPPED:
        if (ev->as.stopped.process_id &&
            ev->as.stopped.process_id != DBG_INVALID_ID)
            e->process_id = ev->as.stopped.process_id;
        if (ev->as.stopped.thread_id) {
            e->selected_thread_id = ev->as.stopped.thread_id;
            dbg_engine_track_thread_state(e, ev->as.stopped.thread_id,
                                          DBG_THREAD_STOPPED);
        }
        e->selected_frame_id = DBG_INVALID_ID;
        e->process_state = (ev->as.stopped.all_threads ||
                            !(e->capabilities & DBG_CAP_NON_STOP))
                         ? DBG_PROCESS_STOPPED
                         : DBG_PROCESS_PARTIALLY_STOPPED;
        if (e->process_state == DBG_PROCESS_STOPPED)
            dbg_engine_track_all_threads(e, DBG_THREAD_STOPPED);
        dbg_engine_note_breakpoint_hit(e, ev->as.stopped.breakpoint_id,
                                       ev->as.stopped.address);
        invalidate_snapshot = true;
        break;

    case DBG_ENGINE_EVENT_CONTINUED:
        if (ev->as.continued.process_id &&
            ev->as.continued.process_id != DBG_INVALID_ID)
            e->process_id = ev->as.continued.process_id;
        e->selected_frame_id = DBG_INVALID_ID;
        e->process_state = (ev->as.continued.all_threads ||
                            !(e->capabilities & DBG_CAP_NON_STOP))
                         ? DBG_PROCESS_RUNNING
                         : DBG_PROCESS_PARTIALLY_STOPPED;
        if (ev->as.continued.all_threads ||
            !(e->capabilities & DBG_CAP_NON_STOP))
            dbg_engine_track_all_threads(e, DBG_THREAD_RUNNING);
        else if (ev->as.continued.thread_id)
            dbg_engine_track_thread_state(e, ev->as.continued.thread_id,
                                          DBG_THREAD_RUNNING);
        invalidate_snapshot = true;
        break;

    case DBG_ENGINE_EVENT_EXITED:
        if (ev->as.exited.process_id &&
            ev->as.exited.process_id != DBG_INVALID_ID)
            e->process_id = ev->as.exited.process_id;
        e->process_state = DBG_PROCESS_EXITED;
        dbg_engine_track_all_threads(e, DBG_THREAD_EXITED);
        e->selected_thread_id = DBG_INVALID_ID;
        e->selected_frame_id = DBG_INVALID_ID;
        invalidate_snapshot = true;
        break;

    case DBG_ENGINE_EVENT_THREAD_CREATED:
        if (ev->as.thread.thread_id)
            dbg_engine_track_thread_state(
                e, ev->as.thread.thread_id,
                e->process_state == DBG_PROCESS_RUNNING
                    ? DBG_THREAD_RUNNING : DBG_THREAD_UNKNOWN);
        break;

    case DBG_ENGINE_EVENT_THREAD_EXITED:
        if (ev->as.thread.thread_id) {
            dbg_engine_track_thread_state(e, ev->as.thread.thread_id,
                                          DBG_THREAD_EXITED);
            if (e->selected_thread_id == ev->as.thread.thread_id) {
                e->selected_thread_id = DBG_INVALID_ID;
                e->selected_frame_id = DBG_INVALID_ID;
            }
        }
        break;

    case DBG_ENGINE_EVENT_MODULE_LOADED:
    case DBG_ENGINE_EVENT_MODULE_UNLOADED:
        /* A logical breakpoint survives module/JIT topology changes; only its
           concrete locations are recomputed. */
        dbg_engine_resolve_breakpoints(e);
        invalidate_snapshot = true;
        break;

    case DBG_ENGINE_EVENT_BREAKPOINT: {
        DbgBreakpoint *bp = dbg_engine_find_breakpoint(
            e, ev->as.breakpoint.breakpoint_id);
        if (bp) {
            if (ev->as.breakpoint.reason == DBG_BREAKPOINT_RESOLVED)
                bp->verified = true;
            else if (ev->as.breakpoint.reason == DBG_BREAKPOINT_INVALIDATED)
                bp->verified = false;
            if (ev->as.breakpoint.message) {
                free(bp->verification_message);
                bp->verification_message = dbg_xstrdup(ev->as.breakpoint.message);
            }
        }
        break;
    }

    case DBG_ENGINE_EVENT_MEMORY:
        invalidate_snapshot = true;
        break;

    case DBG_ENGINE_EVENT_REPLAY_POSITION:
        e->process_state = DBG_PROCESS_STOPPED;
        e->selected_frame_id = DBG_INVALID_ID;
        invalidate_snapshot = true;
        break;

    case DBG_ENGINE_EVENT_INVALIDATED:
        if (ev->as.invalidated.areas &
            (DBG_INVALIDATE_STACKS | DBG_INVALIDATE_SCOPES |
             DBG_INVALIDATE_VARIABLES | DBG_INVALIDATE_REGISTERS |
             DBG_INVALIDATE_MEMORY | DBG_INVALIDATE_TASKS))
            invalidate_snapshot = true;
        break;

    case DBG_ENGINE_EVENT_CAPABILITIES:
        /* The static backend table is the implementation ceiling. Dynamic
           capability events may narrow/re-enable that set, never advertise
           callbacks the backend did not validate at construction time. */
        e->capabilities = ev->as.capabilities.capabilities &
                          (e->ops ? e->ops->capabilities : 0);
        break;

    case DBG_ENGINE_EVENT_OUTPUT:
    case DBG_ENGINE_EVENT_COMPILER:
    case DBG_ENGINE_EVENT_NONE:
        break;
    }

    dbg_engine_touch(e, invalidate_snapshot);
}

bool dbg_engine_poll_event(DbgEngine *e, DbgEngineEvent *event_out,
                           char **error_out)
{
    dbg_error_out_reset(error_out);
    if (!e || !e->ops || !e->ops->poll_event || !event_out) return false;
    memset(event_out, 0, sizeof(*event_out));
    bool got = e->ops->poll_event(e->backend_ctx, event_out, error_out);
    if (!got) return false;
    if (!event_out->timestamp_ms) event_out->timestamp_ms = dbg_now_ms();
    /* Sequence numbers belong to the engine, not to transports. This keeps
       one strict monotonic order even when a backend merges worker streams. */
    event_out->sequence = e->next_event_sequence++;
    dbg_engine_apply_event(e, event_out);
    return true;
}

/* ---- Query wrappers ------------------------------------------------------ */

#define DBG_ENGINE_SIMPLE_QUERY(name_, cap_, member_, signature_, callargs_)   \
    bool name_ signature_ {                                                     \
        if (!dbg_engine_require(engine, (cap_),                                  \
                engine && engine->ops && engine->ops->member_,                   \
                error_out, #member_)) return false;                             \
        return engine->ops->member_ callargs_;                                  \
    }

bool dbg_engine_threads(DbgEngine *engine, DbgThreadInfo **out, size_t *count,
                        char **error_out)
{
    if (out) *out = NULL;
    if (count) *count = 0;
    if (!dbg_engine_require(engine, 0,
                            engine && engine->ops && engine->ops->threads,
                            error_out, "threads")) return false;
    return engine->ops->threads(engine->backend_ctx, out, count, error_out);
}

bool dbg_engine_stack_trace(DbgEngine *engine, uint64_t thread_id, size_t start,
                            size_t count, DbgStackFrameInfo **out,
                            size_t *out_count, size_t *total, char **error_out)
{
    if (out) *out = NULL;
    if (out_count) *out_count = 0;
    if (total) *total = 0;
    if (!dbg_engine_snapshot_query_allowed(engine, error_out, "stack-trace"))
        return false;
    if (!dbg_engine_require(engine, 0,
                            engine && engine->ops && engine->ops->stack_trace,
                            error_out, "stack-trace")) return false;
    bool ok = engine->ops->stack_trace(engine->backend_ctx, thread_id, start,
                                       count, out, out_count, total, error_out);
    if (ok && out && *out && out_count)
        for (size_t i = 0; i < *out_count; i++)
            (*out)[i].snapshot_epoch = engine->snapshot_epoch;
    return ok;
}

bool dbg_engine_scopes(DbgEngine *engine, uint64_t frame_id, DbgScopeInfo **out,
                       size_t *count, char **error_out)
{
    if (out) *out = NULL;
    if (count) *count = 0;
    if (!dbg_engine_snapshot_query_allowed(engine, error_out, "scopes"))
        return false;
    if (!dbg_engine_require(engine, 0,
                            engine && engine->ops && engine->ops->scopes,
                            error_out, "scopes")) return false;
    bool ok = engine->ops->scopes(engine->backend_ctx, frame_id, out, count,
                                  error_out);
    if (ok && out && *out && count)
        for (size_t i = 0; i < *count; i++)
            (*out)[i].snapshot_epoch = engine->snapshot_epoch;
    return ok;
}

bool dbg_engine_variables(DbgEngine *engine, uint64_t variables_reference,
                          size_t start, size_t count, DbgValueInfo **out,
                          size_t *out_count, char **error_out)
{
    if (out) *out = NULL;
    if (out_count) *out_count = 0;
    if (!dbg_engine_snapshot_query_allowed(engine, error_out, "variables"))
        return false;
    if (!dbg_engine_require(engine, 0,
                            engine && engine->ops && engine->ops->variables,
                            error_out, "variables")) return false;
    bool ok = engine->ops->variables(engine->backend_ctx, variables_reference,
                                     start, count, out, out_count, error_out);
    if (ok && out && *out && out_count)
        for (size_t i = 0; i < *out_count; i++)
            (*out)[i].snapshot_epoch = engine->snapshot_epoch;
    return ok;
}

bool dbg_engine_evaluate(DbgEngine *engine, uint64_t frame_id,
                         const char *expression,
                         const DbgEvaluationOptions *options,
                         DbgValueInfo *out, char **error_out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!dbg_engine_snapshot_query_allowed(engine, error_out, "evaluate"))
        return false;
    if (!dbg_engine_require(engine, DBG_CAP_EVALUATE,
                            engine && engine->ops && engine->ops->evaluate,
                            error_out, "evaluate")) return false;
    if (!expression || !*expression || !out) {
        dbg_error_out_set(error_out, "evaluate requires an expression and output");
        return false;
    }
    DbgEvaluationOptions defaults = {
        .context = DBG_EVAL_REPL,
        .format = DBG_FORMAT_NATURAL,
        .allow_side_effects = false,
        .allow_function_calls = false,
        .prefer_dynamic = true,
        .prefer_synthetic = true,
    };
    bool ok = engine->ops->evaluate(engine->backend_ctx, frame_id, expression,
                                    options ? options : &defaults, out,
                                    error_out);
    if (ok) out->snapshot_epoch = engine->snapshot_epoch;
    return ok;
}

bool dbg_engine_set_expression(DbgEngine *engine, uint64_t frame_id,
                               const char *expression, const char *value,
                               DbgValueInfo *out, char **error_out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!dbg_engine_snapshot_query_allowed(engine, error_out, "set-expression"))
        return false;
    if (!dbg_engine_require(engine, DBG_CAP_SET_EXPRESSION,
                            engine && engine->ops && engine->ops->set_expression,
                            error_out, "set-expression")) return false;
    bool ok = engine->ops->set_expression(engine->backend_ctx, frame_id,
                                          expression, value, out, error_out);
    if (ok) {
        dbg_engine_touch(engine, true);
        if (out) out->snapshot_epoch = engine->snapshot_epoch;
    }
    return ok;
}

bool dbg_engine_set_variable(DbgEngine *engine, uint64_t variables_reference,
                             const char *name, const char *value,
                             DbgValueInfo *out, char **error_out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!dbg_engine_snapshot_query_allowed(engine, error_out, "set-variable"))
        return false;
    if (!dbg_engine_require(engine, DBG_CAP_SET_VARIABLE,
                            engine && engine->ops && engine->ops->set_variable,
                            error_out, "set-variable")) return false;
    bool ok = engine->ops->set_variable(engine->backend_ctx,
                                        variables_reference, name, value,
                                        out, error_out);
    if (ok) {
        dbg_engine_touch(engine, true);
        if (out) out->snapshot_epoch = engine->snapshot_epoch;
    }
    return ok;
}

DBG_ENGINE_SIMPLE_QUERY(dbg_engine_completions, DBG_CAP_COMPLETIONS, completions,
    (DbgEngine *engine, uint64_t frame_id, const char *text, uint32_t cursor,
     DbgCompletionItem **out, size_t *count, char **error_out),
    (engine->backend_ctx, frame_id, text, cursor, out, count, error_out))

DBG_ENGINE_SIMPLE_QUERY(dbg_engine_read_memory, DBG_CAP_READ_MEMORY, read_memory,
    (DbgEngine *engine, uint64_t address, size_t count, uint8_t **out,
     size_t *out_count, char **error_out),
    (engine->backend_ctx, address, count, out, out_count, error_out))

bool dbg_engine_write_memory(DbgEngine *engine, uint64_t address,
                             const uint8_t *bytes, size_t count,
                             size_t *written, char **error_out)
{
    if (written) *written = 0;
    if (!dbg_engine_require(engine, DBG_CAP_WRITE_MEMORY,
                            engine && engine->ops && engine->ops->write_memory,
                            error_out, "write-memory")) return false;
    bool ok = engine->ops->write_memory(engine->backend_ctx, address, bytes,
                                        count, written, error_out);
    if (ok) dbg_engine_touch(engine, true);
    return ok;
}

DBG_ENGINE_SIMPLE_QUERY(dbg_engine_registers, DBG_CAP_REGISTERS, registers,
    (DbgEngine *engine, uint64_t thread_id, DbgRegisterInfo **out, size_t *count,
     char **error_out),
    (engine->backend_ctx, thread_id, out, count, error_out))

DBG_ENGINE_SIMPLE_QUERY(dbg_engine_disassemble, DBG_CAP_DISASSEMBLE, disassemble,
    (DbgEngine *engine, uint64_t address, int64_t instruction_offset,
     size_t instruction_count, DbgInstructionInfo **out, size_t *count,
     char **error_out),
    (engine->backend_ctx, address, instruction_offset, instruction_count,
     out, count, error_out))

DBG_ENGINE_SIMPLE_QUERY(dbg_engine_modules, DBG_CAP_MODULES, modules,
    (DbgEngine *engine, DbgModuleInfo **out, size_t *count, char **error_out),
    (engine->backend_ctx, out, count, error_out))

DBG_ENGINE_SIMPLE_QUERY(dbg_engine_loaded_sources, DBG_CAP_LOADED_SOURCES,
    loaded_sources,
    (DbgEngine *engine, DbgSourceInfo **out, size_t *count, char **error_out),
    (engine->backend_ctx, out, count, error_out))

DBG_ENGINE_SIMPLE_QUERY(dbg_engine_source_content, DBG_CAP_SOURCE_CONTENT,
    source_content,
    (DbgEngine *engine, uint64_t source_id, const char *path, char **content,
     size_t *content_len, char **mime_type, char **error_out),
    (engine->backend_ctx, source_id, path, content, content_len, mime_type,
     error_out))

DBG_ENGINE_SIMPLE_QUERY(dbg_engine_tasks, DBG_CAP_LOGICAL_TASKS, tasks,
    (DbgEngine *engine, DbgTaskInfo **out, size_t *count, char **error_out),
    (engine->backend_ctx, out, count, error_out))

DBG_ENGINE_SIMPLE_QUERY(dbg_engine_compiler_stages, DBG_CAP_COMPILER_PIPELINE,
    compiler_stages,
    (DbgEngine *engine, DbgCompilerStageInfo **out, size_t *count,
     char **error_out),
    (engine->backend_ctx, out, count, error_out))

DBG_ENGINE_SIMPLE_QUERY(dbg_engine_semantic_node, DBG_CAP_COMPILER_PIPELINE,
    semantic_node,
    (DbgEngine *engine, uint64_t node_id, DbgSemanticNodeInfo *out,
     char **error_out),
    (engine->backend_ctx, node_id, out, error_out))

DBG_ENGINE_SIMPLE_QUERY(dbg_engine_semantic_children, DBG_CAP_COMPILER_PIPELINE,
    semantic_children,
    (DbgEngine *engine, uint64_t children_reference, size_t start, size_t count,
     DbgSemanticNodeInfo **out, size_t *out_count, char **error_out),
    (engine->backend_ctx, children_reference, start, count, out, out_count,
     error_out))

DBG_ENGINE_SIMPLE_QUERY(dbg_engine_checkpoint, DBG_CAP_CHECKPOINTS, checkpoint,
    (DbgEngine *engine, uint64_t *checkpoint_id, char **error_out),
    (engine->backend_ctx, checkpoint_id, error_out))

#undef DBG_ENGINE_SIMPLE_QUERY

bool dbg_engine_restore_checkpoint(DbgEngine *engine, uint64_t checkpoint_id,
                                   char **error_out)
{
    if (!dbg_engine_require(engine, DBG_CAP_CHECKPOINTS,
                            engine && engine->ops && engine->ops->restore_checkpoint,
                            error_out, "restore-checkpoint")) return false;
    bool ok = engine->ops->restore_checkpoint(engine->backend_ctx,
                                              checkpoint_id, error_out);
    if (ok) {
        engine->process_state = DBG_PROCESS_STOPPED;
        engine->selected_frame_id = DBG_INVALID_ID;
        dbg_engine_touch(engine, true);
    }
    return ok;
}

/* ---- Logical breakpoints ------------------------------------------------ */

static DbgBreakpoint *dbg_engine_new_breakpoint(DbgEngine *e,
                                                DbgBreakpointKind kind)
{
    if (!e || e->breakpoint_count >= DBG_MAX_BREAKPOINTS) return NULL;
    DBG_GROW(e->breakpoints, e->breakpoint_count, e->breakpoint_cap,
             DbgBreakpoint);
    DbgBreakpoint *bp = &e->breakpoints[e->breakpoint_count++];
    memset(bp, 0, sizeof(*bp));
    bp->id = e->next_breakpoint_id++;
    if (bp->id == 0) bp->id = e->next_breakpoint_id++;
    bp->kind = kind;
    bp->enabled = true;
    dbg_engine_touch(e, false);
    return bp;
}

static void dbg_engine_try_initial_sync(DbgEngine *e, DbgBreakpoint *bp)
{
    char *err = NULL;
    (void)dbg_engine_sync_breakpoint(e, bp, &err);
    if (err && !bp->verification_message)
        bp->verification_message = dbg_xstrdup(err);
    free(err);
}

uint32_t dbg_engine_add_source_breakpoint(DbgEngine *e, const char *file,
                                          uint32_t line, uint32_t column)
{
    if (!e || !file || !*file || line == 0) return 0;
    DbgBreakpoint *bp = dbg_engine_new_breakpoint(e, DBG_BP_SOURCE);
    if (!bp) return 0;
    bp->as.source.location.file = dbg_xstrdup(file);
    bp->as.source.location.line = line;
    bp->as.source.location.column = column;
    dbg_engine_try_initial_sync(e, bp);
    return bp->id;
}

uint32_t dbg_engine_add_function_breakpoint(DbgEngine *e, const char *symbol)
{
    if (!e || !symbol || !*symbol) return 0;
    DbgBreakpoint *bp = dbg_engine_new_breakpoint(e, DBG_BP_FUNCTION);
    if (!bp) return 0;
    bp->as.function.symbol = dbg_xstrdup(symbol);
    dbg_engine_try_initial_sync(e, bp);
    return bp->id;
}

uint32_t dbg_engine_add_ir_breakpoint(DbgEngine *e, const char *function,
                                      const char *value)
{
    if (!e || ((!function || !*function) && (!value || !*value))) return 0;
    DbgBreakpoint *bp = dbg_engine_new_breakpoint(e, DBG_BP_IR_VALUE);
    if (!bp) return 0;
    bp->as.ir.function = function && *function ? dbg_xstrdup(function) : NULL;
    bp->as.ir.value = value && *value ? dbg_xstrdup(value) : NULL;
    dbg_engine_try_initial_sync(e, bp);
    return bp->id;
}

uint32_t dbg_engine_add_instruction_breakpoint(DbgEngine *e,
                                               uint64_t address,
                                               int64_t offset)
{
    if (!e || address == DBG_INVALID_ADDRESS) return 0;
    DbgBreakpoint *bp = dbg_engine_new_breakpoint(e, DBG_BP_INSTRUCTION);
    if (!bp) return 0;
    bp->as.instruction.address = address;
    bp->as.instruction.offset = offset;
    dbg_engine_try_initial_sync(e, bp);
    return bp->id;
}

uint32_t dbg_engine_add_address_breakpoint(DbgEngine *e, uint64_t address)
{
    return dbg_engine_add_instruction_breakpoint(e, address, 0);
}

uint32_t dbg_engine_add_data_breakpoint(DbgEngine *e,
                                        const DbgDataBreakpointInfo *info,
                                        DbgDataAccess access)
{
    if (!e || !info) return 0;
    if ((!info->data_id || !*info->data_id) &&
        (info->address == 0 || info->address == DBG_INVALID_ADDRESS)) return 0;
    if (info->supported_access && !(info->supported_access & access)) return 0;
    DbgBreakpoint *bp = dbg_engine_new_breakpoint(e, DBG_BP_DATA);
    if (!bp) return 0;
    bp->as.data.data_id = info->data_id ? dbg_xstrdup(info->data_id) : NULL;
    bp->as.data.address = info->address;
    bp->as.data.size = info->size;
    bp->as.data.access = access;
    dbg_engine_try_initial_sync(e, bp);
    return bp->id;
}

uint32_t dbg_engine_add_exception_breakpoint(DbgEngine *e,
                                             const char *filter)
{
    if (!e || !filter || !*filter) return 0;
    DbgBreakpoint *bp = dbg_engine_new_breakpoint(e, DBG_BP_EXCEPTION);
    if (!bp) return 0;
    bp->as.exception.filter = dbg_xstrdup(filter);
    dbg_engine_try_initial_sync(e, bp);
    return bp->id;
}

bool dbg_engine_remove_breakpoint(DbgEngine *e, uint32_t id, char **error_out)
{
    dbg_error_out_reset(error_out);
    if (!e) {
        dbg_error_out_set(error_out, "debugger engine is NULL");
        return false;
    }
    for (size_t i = 0; i < e->breakpoint_count; i++) {
        DbgBreakpoint *bp = &e->breakpoints[i];
        if (bp->id != id) continue;
        if (e->ops && e->ops->remove_breakpoint && bp->enabled &&
            !e->ops->remove_breakpoint(e->backend_ctx, bp, error_out))
            return false;
        dbg_breakpoint_free_fields(bp);
        if (i + 1 < e->breakpoint_count)
            memmove(&e->breakpoints[i], &e->breakpoints[i + 1],
                    (e->breakpoint_count - i - 1) * sizeof(*e->breakpoints));
        e->breakpoint_count--;
        dbg_engine_touch(e, false);
        return true;
    }
    dbg_error_out_set(error_out, "breakpoint id not found");
    return false;
}

bool dbg_engine_set_breakpoint_enabled(DbgEngine *e, uint32_t id, bool enabled,
                                       char **error_out)
{
    dbg_error_out_reset(error_out);
    DbgBreakpoint *bp = dbg_engine_find_breakpoint(e, id);
    if (!bp) {
        dbg_error_out_set(error_out, "breakpoint id not found");
        return false;
    }
    if (bp->enabled == enabled) return true;
    if (!enabled && e->ops && e->ops->remove_breakpoint &&
        !e->ops->remove_breakpoint(e->backend_ctx, bp, error_out)) return false;
    bp->enabled = enabled;
    if (!enabled) {
        dbg_breakpoint_clear_sites(bp);
        dbg_engine_touch(e, false);
        return true;
    }
    bool ok = dbg_engine_sync_breakpoint(e, bp, error_out);
    if (!ok) {
        bp->enabled = false;
        return false;
    }
    dbg_engine_touch(e, false);
    return true;
}

bool dbg_engine_set_breakpoint_condition(DbgEngine *e, uint32_t id,
                                         const char *condition,
                                         const char *hit_condition,
                                         const char *log_message,
                                         char **error_out)
{
    dbg_error_out_reset(error_out);
    DbgBreakpoint *bp = dbg_engine_find_breakpoint(e, id);
    if (!bp) {
        dbg_error_out_set(error_out, "breakpoint id not found");
        return false;
    }
    if (e->ops && condition && *condition &&
        !(e->capabilities & DBG_CAP_CONDITIONAL_BREAKPOINTS)) {
        dbg_error_out_set(error_out,
            "backend does not support conditional breakpoints");
        return false;
    }
    if (e->ops && hit_condition && *hit_condition &&
        !(e->capabilities & DBG_CAP_HIT_CONDITIONS)) {
        dbg_error_out_set(error_out, "backend does not support hit conditions");
        return false;
    }
    if (e->ops && log_message && *log_message &&
        !(e->capabilities & DBG_CAP_LOGPOINTS)) {
        dbg_error_out_set(error_out, "backend does not support logpoints");
        return false;
    }

    char *new_condition = condition && *condition ? dbg_xstrdup(condition) : NULL;
    char *new_hit = hit_condition && *hit_condition
                  ? dbg_xstrdup(hit_condition) : NULL;
    char *new_log = log_message && *log_message ? dbg_xstrdup(log_message) : NULL;
    char *old_condition = bp->condition;
    char *old_hit = bp->hit_condition;
    char *old_log = bp->log_message;
    bp->condition = new_condition;
    bp->hit_condition = new_hit;
    bp->log_message = new_log;
    bool ok = !bp->enabled || dbg_engine_sync_breakpoint(e, bp, error_out);
    if (!ok) {
        free(bp->condition); free(bp->hit_condition); free(bp->log_message);
        bp->condition = old_condition;
        bp->hit_condition = old_hit;
        bp->log_message = old_log;
        return false;
    }
    free(old_condition); free(old_hit); free(old_log);
    dbg_engine_touch(e, false);
    return true;
}

size_t dbg_engine_breakpoint_count(const DbgEngine *e)
{ return e ? e->breakpoint_count : 0; }

DbgBreakpoint *dbg_engine_breakpoint_at(DbgEngine *e, size_t index)
{ return (e && index < e->breakpoint_count) ? &e->breakpoints[index] : NULL; }

const DbgBreakpoint *dbg_engine_breakpoint_at_const(const DbgEngine *e,
                                                     size_t index)
{ return (e && index < e->breakpoint_count) ? &e->breakpoints[index] : NULL; }

const DbgCodeLocation *dbg_breakpoint_primary_location(const DbgBreakpoint *bp)
{
    if (!bp) return NULL;
    for (size_t i = 0; i < bp->site_count; i++)
        if (bp->sites[i].resolved) return &bp->sites[i].location;
    return NULL;
}

bool dbg_engine_breakpoint_locations(DbgEngine *engine,
                                     const DbgSourceLocation *range,
                                     DbgBreakpointCandidate **out,
                                     size_t *count, char **error_out)
{
    if (out) *out = NULL;
    if (count) *count = 0;
    if (!dbg_engine_require(engine, DBG_CAP_BREAKPOINT_LOCATIONS,
            engine && engine->ops && engine->ops->breakpoint_locations,
            error_out, "breakpoint-locations")) return false;
    return engine->ops->breakpoint_locations(engine->backend_ctx, range, out,
                                             count, error_out);
}

bool dbg_engine_data_breakpoint_info(DbgEngine *engine, uint64_t frame_id,
                                     const char *expression,
                                     DbgDataBreakpointInfo *out,
                                     char **error_out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!dbg_engine_require(engine, DBG_CAP_DATA_BREAKPOINTS,
            engine && engine->ops && engine->ops->data_breakpoint_info,
            error_out, "data-breakpoint-info")) return false;
    if (!out || !expression || !*expression) {
        dbg_error_out_set(error_out,
            "data-breakpoint-info requires an expression and output");
        return false;
    }
    return engine->ops->data_breakpoint_info(engine->backend_ctx, frame_id,
                                             expression, out, error_out);
}

/* ---- Watch expressions -------------------------------------------------- */

uint32_t dbg_engine_add_watch(DbgEngine *e, const char *expression)
{
    if (!e || !expression || !*expression) return 0;
    DBG_GROW(e->watches, e->watch_count, e->watch_cap, DbgWatch);
    DbgWatch *w = &e->watches[e->watch_count++];
    memset(w, 0, sizeof(*w));
    w->id = e->next_watch_id++;
    if (w->id == 0) w->id = e->next_watch_id++;
    w->expression = dbg_xstrdup(expression);
    dbg_engine_touch(e, false);
    return w->id;
}

bool dbg_engine_remove_watch(DbgEngine *e, uint32_t id)
{
    if (!e) return false;
    for (size_t i = 0; i < e->watch_count; i++) {
        if (e->watches[i].id != id) continue;
        free(e->watches[i].expression);
        free(e->watches[i].last_value);
        free(e->watches[i].last_error);
        if (i + 1 < e->watch_count)
            memmove(&e->watches[i], &e->watches[i + 1],
                    (e->watch_count - i - 1) * sizeof(*e->watches));
        e->watch_count--;
        dbg_engine_touch(e, false);
        return true;
    }
    return false;
}

size_t dbg_engine_watch_count(const DbgEngine *e)
{ return e ? e->watch_count : 0; }

DbgWatch *dbg_engine_watch_at(DbgEngine *e, size_t index)
{ return (e && index < e->watch_count) ? &e->watches[index] : NULL; }

/* ---- Cross-layer source -> semantic -> IR -> machine provenance --------- */

static uint64_t dbg_location_end(const DbgCodeLocation *loc)
{
    if (!loc || loc->address == DBG_INVALID_ADDRESS) return 0;
    if (loc->address_end && loc->address_end != DBG_INVALID_ADDRESS &&
        loc->address_end > loc->address) return loc->address_end;
    return loc->address == UINT64_MAX ? UINT64_MAX : loc->address + 1;
}

DbgCodeMap *dbg_code_map_create(void)
{ return dbg_xcalloc(1, sizeof(DbgCodeMap)); }

static void dbg_code_map_drop_indexes(DbgCodeMap *map)
{
    if (!map) return;
    free(map->address_index);
    free(map->address_prefix_max_end);
    free(map->source_index);
    map->address_index = NULL;
    map->address_prefix_max_end = NULL;
    map->source_index = NULL;
    map->finalized = false;
}

void dbg_code_map_free(DbgCodeMap *map)
{
    if (!map) return;
    for (size_t i = 0; i < map->count; i++)
        dbg_code_location_free(&map->entries[i]);
    free(map->entries);
    dbg_code_map_drop_indexes(map);
    free(map);
}

bool dbg_code_map_add(DbgCodeMap *map, const DbgCodeLocation *location)
{
    if (!map || !location) return false;
    dbg_code_map_drop_indexes(map);
    DBG_GROW(map->entries, map->count, map->cap, DbgCodeLocation);
    dbg_code_location_copy(&map->entries[map->count], location);
    map->count++;
    return true;
}

static int dbg_code_location_address_ptr_cmp(const void *a, const void *b)
{
    const DbgCodeLocation *aa = *(DbgCodeLocation *const *)a;
    const DbgCodeLocation *bb = *(DbgCodeLocation *const *)b;
    if (aa->address == DBG_INVALID_ADDRESS)
        return bb->address == DBG_INVALID_ADDRESS ? 0 : 1;
    if (bb->address == DBG_INVALID_ADDRESS) return -1;
    if (aa->address != bb->address)
        return aa->address < bb->address ? -1 : 1;
    if (aa->inline_depth != bb->inline_depth)
        return aa->inline_depth < bb->inline_depth ? -1 : 1;
    return 0;
}

static int dbg_nullable_strcmp(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    return strcmp(a, b);
}

static int dbg_code_location_source_ptr_cmp(const void *a, const void *b)
{
    const DbgCodeLocation *aa = *(DbgCodeLocation *const *)a;
    const DbgCodeLocation *bb = *(DbgCodeLocation *const *)b;
    int sc = dbg_nullable_strcmp(aa->source.file, bb->source.file);
    if (sc) return sc;
    if (aa->source.line != bb->source.line)
        return aa->source.line < bb->source.line ? -1 : 1;
    if (aa->source.column != bb->source.column)
        return aa->source.column < bb->source.column ? -1 : 1;
    if (aa->inline_depth != bb->inline_depth)
        return aa->inline_depth < bb->inline_depth ? -1 : 1;
    if (aa->address != bb->address)
        return aa->address < bb->address ? -1 : 1;
    return 0;
}

void dbg_code_map_finalize(DbgCodeMap *map)
{
    if (!map || map->finalized) return;
    dbg_code_map_drop_indexes(map);
    if (map->count == 0) {
        map->finalized = true;
        return;
    }
    map->address_index = dbg_xmalloc(map->count * sizeof(*map->address_index));
    map->source_index = dbg_xmalloc(map->count * sizeof(*map->source_index));
    map->address_prefix_max_end = dbg_xmalloc(
        map->count * sizeof(*map->address_prefix_max_end));
    for (size_t i = 0; i < map->count; i++) {
        map->address_index[i] = &map->entries[i];
        map->source_index[i] = &map->entries[i];
    }
    qsort(map->address_index, map->count, sizeof(*map->address_index),
          dbg_code_location_address_ptr_cmp);
    qsort(map->source_index, map->count, sizeof(*map->source_index),
          dbg_code_location_source_ptr_cmp);
    uint64_t max_end = 0;
    for (size_t i = 0; i < map->count; i++) {
        uint64_t end = dbg_location_end(map->address_index[i]);
        if (end > max_end) max_end = end;
        map->address_prefix_max_end[i] = max_end;
    }
    map->finalized = true;
}

static void dbg_code_map_ensure_finalized(const DbgCodeMap *map)
{
    if (map && !map->finalized) dbg_code_map_finalize((DbgCodeMap *)map);
}

const DbgCodeLocation *dbg_code_map_find_address(const DbgCodeMap *map,
                                                 uint64_t address)
{
    if (!map || map->count == 0 || address == DBG_INVALID_ADDRESS) return NULL;
    dbg_code_map_ensure_finalized(map);
    size_t lo = 0, hi = map->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint64_t start = map->address_index[mid]->address;
        if (start == DBG_INVALID_ADDRESS || start > address) hi = mid;
        else lo = mid + 1;
    }
    size_t pos = lo;
    while (pos > 0) {
        size_t i = pos - 1;
        if (map->address_prefix_max_end[i] <= address) break;
        const DbgCodeLocation *loc = map->address_index[i];
        uint64_t end = dbg_location_end(loc);
        if (loc->address <= address && address < end) return loc;
        pos = i;
    }
    return NULL;
}

static int dbg_source_key_cmp(const DbgCodeLocation *loc, const char *file,
                              uint32_t line)
{
    int sc = dbg_nullable_strcmp(loc->source.file, file);
    if (sc) return sc;
    if (loc->source.line == line) return 0;
    return loc->source.line < line ? -1 : 1;
}

const DbgCodeLocation *dbg_code_map_find_source(const DbgCodeMap *map,
                                                const char *file,
                                                uint32_t line,
                                                uint32_t column)
{
    if (!map || !file || !*file || line == 0 || map->count == 0) return NULL;
    dbg_code_map_ensure_finalized(map);
    size_t lo = 0, hi = map->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = dbg_source_key_cmp(map->source_index[mid], file, line);
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    const DbgCodeLocation *best = NULL;
    for (size_t i = lo; i < map->count; i++) {
        const DbgCodeLocation *loc = map->source_index[i];
        if (dbg_source_key_cmp(loc, file, line) != 0) break;
        if (column && loc->source.column && loc->source.column > column) continue;
        if (!best || loc->source.column > best->source.column ||
            (loc->source.column == best->source.column &&
             loc->inline_depth > best->inline_depth)) best = loc;
    }
    return best;
}

const DbgCodeLocation *dbg_code_map_find_ir(const DbgCodeMap *map,
                                            const char *function,
                                            uint32_t instruction,
                                            const char *value)
{
    if (!map) return NULL;
    const DbgCodeLocation *best = NULL;
    for (size_t i = 0; i < map->count; i++) {
        const DbgCodeLocation *loc = &map->entries[i];
        if (function && *function &&
            (!loc->ir_function || strcmp(loc->ir_function, function) != 0))
            continue;
        if (instruction && loc->ir_instruction != instruction) continue;
        if (value && *value && (!loc->ir_value || strcmp(loc->ir_value, value) != 0))
            continue;
        if (!best || loc->inline_depth > best->inline_depth) best = loc;
    }
    return best;
}

const DbgCodeLocation *dbg_code_map_find_semantic(const DbgCodeMap *map,
                                                  uint64_t stage_id,
                                                  uint64_t node_id)
{
    if (!map || node_id == 0 || node_id == DBG_INVALID_ID) return NULL;
    const DbgCodeLocation *best = NULL;
    for (size_t i = 0; i < map->count; i++) {
        const DbgCodeLocation *loc = &map->entries[i];
        if (loc->semantic_node_id != node_id) continue;
        if (stage_id && loc->compiler_stage_id != stage_id) continue;
        if (!best || loc->inline_depth > best->inline_depth) best = loc;
    }
    return best;
}



/// §4  Terminal raw-mode + capability setup

//// Raw mode
 //
 //  We disable canonical mode and echo, set VMIN=0/VTIME=1 so reads can
 //  be polled cooperatively with poll() alongside other fds (a future
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
                               more finely with poll() for blink timing */

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
 //  dbg_screen_flush() scans only rows whose final back-buffer cells differ
 //  from front, then emits a
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
    size_t   *row_damage;    /* cells whose back value differs from front */
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


static bool dbg_cell_equal(const DbgCell *a, const DbgCell *b)
{
    return a->codepoint == b->codepoint &&
           a->style.fg.r == b->style.fg.r && a->style.fg.g == b->style.fg.g &&
           a->style.fg.b == b->style.fg.b && a->style.bg.r == b->style.bg.r &&
           a->style.bg.g == b->style.bg.g && a->style.bg.b == b->style.bg.b &&
           a->style.attrs == b->style.attrs;
}

/* Maintain the exact number of front/back differences in each row. Rendering
   may touch the whole logical back buffer, but a row that returns to the same
   final pixels becomes damage-free and the terminal diff never scans it. */
static inline void dbg_screen_assign(DbgScreen *sc, int row, int col, DbgCell value)
{
    if (!sc || row < 0 || row >= sc->rows || col < 0 || col >= sc->cols) return;
    size_t idx = (size_t)row * (size_t)sc->cols + (size_t)col;
    bool before = !dbg_cell_equal(&sc->back[idx], &sc->front[idx]);
    bool after  = !dbg_cell_equal(&value, &sc->front[idx]);
    if (before != after) {
        if (after) sc->row_damage[row]++;
        else if (sc->row_damage[row] > 0) sc->row_damage[row]--;
    }
    sc->back[idx] = value;
}

static DbgScreen *dbg_screen_create(int cols, int rows)
{
    DbgScreen *sc = dbg_xcalloc(1, sizeof(*sc));
    sc->cols = cols;
    sc->rows = rows;
    size_t n = (size_t)cols * (size_t)rows;
    sc->front = dbg_xmalloc(n * sizeof(DbgCell));
    sc->back  = dbg_xmalloc(n * sizeof(DbgCell));
    sc->row_damage = dbg_xcalloc((size_t)rows, sizeof(*sc->row_damage));
    DbgCell blank = dbg_cell_blank();
    for (size_t i = 0; i < n; i++) {
        sc->front[i] = blank;
        sc->back[i]  = blank;
    }
    /* Force the very first flush to paint everything: make front differ
       from back by giving front a sentinel codepoint.                   */
    for (size_t i = 0; i < n; i++) sc->front[i].codepoint = 0xFFFFFFFFu;
    for (int r = 0; r < rows; r++) sc->row_damage[r] = (size_t)cols;
    sb_init(&sc->out);
    sc->last_cursor = (DbgPoint){ -1, -1 };
    return sc;
}

static void dbg_screen_free(DbgScreen *sc)
{
    if (!sc) return;
    free(sc->front);
    free(sc->back);
    free(sc->row_damage);
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
    free(sc->row_damage);
    sc->cols = cols;
    sc->rows = rows;
    size_t n = (size_t)cols * (size_t)rows;
    sc->front = dbg_xmalloc(n * sizeof(DbgCell));
    sc->back  = dbg_xmalloc(n * sizeof(DbgCell));
    sc->row_damage = dbg_xcalloc((size_t)rows, sizeof(*sc->row_damage));
    DbgCell blank = dbg_cell_blank();
    for (size_t i = 0; i < n; i++) {
        sc->front[i] = blank;
        sc->front[i].codepoint = 0xFFFFFFFFu;
        sc->back[i]  = blank;
    }
    for (int r = 0; r < rows; r++) sc->row_damage[r] = (size_t)cols;
}

/* Clear the back buffer to blank without touching front — used at the
   start of a render pass before widgets paint into it. This alone does
   NOT cause a redraw of unchanged cells; the diff in dbg_screen_flush
   still only emits the cells that actually differ from front.           */
static void dbg_screen_clear_back(DbgScreen *sc)
{
    DbgCell blank = dbg_cell_blank();
    for (int row = 0; row < sc->rows; row++)
        for (int col = 0; col < sc->cols; col++)
            dbg_screen_assign(sc, row, col, blank);
}

static inline void dbg_screen_put(DbgScreen *sc, int row, int col,
                                   uint32_t codepoint, DbgStyle style)
{
    DbgCell c = { codepoint, style };
    dbg_screen_assign(sc, row, col, c);
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
    size_t n = dbg_encode_utf8(cp ? cp : (uint32_t)' ', buf);
    sb_appendn(out, buf, n);
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
        if (sc->row_damage[row] == 0) continue;
        int col = 0;
        while (col < sc->cols) {
            size_t idx = (size_t)row * (size_t)sc->cols + (size_t)col;
            DbgCell *f = &sc->front[idx];
            DbgCell *b = &sc->back[idx];
            if (dbg_cell_equal(f, b)) {
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
                bool same = dbg_cell_equal(f, b);
                if (same) break;
                dbg_emit_sgr(&sc->out, &sgr, b->style, truecolor);
                dbg_emit_utf8(&sc->out, b->codepoint);
                *f = *b;
                col++;
            }
            sc->last_cursor.row = row;
            sc->last_cursor.col = col;
        }
        sc->row_damage[row] = 0;
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
 //  loop in §28 uses poll() to know when data is actually available.
 //
typedef struct {
    unsigned char buf[4096];
    size_t        head, tail, count;
} DbgByteRing;

static bool ring_empty(const DbgByteRing *r) { return !r || r->count == 0; }
static size_t ring_count(const DbgByteRing *r) { return r ? r->count : 0; }

static int ring_peek_n(const DbgByteRing *r, size_t n)
{
    if (!r || n >= r->count) return -1;
    return r->buf[(r->head + n) % sizeof(r->buf)];
}

static int ring_pop(DbgByteRing *r)
{
    if (ring_empty(r)) return -1;
    int b = r->buf[r->head];
    r->head = (r->head + 1) % sizeof(r->buf);
    r->count--;
    return b;
}

static bool ring_push(DbgByteRing *r, unsigned char b)
{
    if (!r || r->count == sizeof(r->buf)) return false;
    r->buf[r->tail] = b;
    r->tail = (r->tail + 1) % sizeof(r->buf);
    r->count++;
    return true;
}

static void ring_consume(DbgByteRing *r, size_t n)
{
    while (n-- && !ring_empty(r)) (void)ring_pop(r);
}

static bool ring_prefix(const DbgByteRing *r, const unsigned char *bytes, size_t n)
{
    if (!r || !bytes || r->count < n) return false;
    for (size_t i = 0; i < n; i++)
        if (ring_peek_n(r, i) != bytes[i]) return false;
    return true;
}

static bool ring_prefix_available(const DbgByteRing *r,
                                  const unsigned char *bytes, size_t n)
{
    if (!r || !bytes) return false;
    size_t have = r->count < n ? r->count : n;
    for (size_t i = 0; i < have; i++)
        if (ring_peek_n(r, i) != bytes[i]) return false;
    return true;
}

static void ring_fill_nonblocking(DbgByteRing *r)
{
    unsigned char tmp[256];
    ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
    if (n <= 0) return;
    for (ssize_t i = 0; i < n; i++) {
        if (!ring_push(r, tmp[i])) break;
    }
}

struct DbgInputState {
    DbgByteRing ring;
    uint64_t esc_pending_since_ms;
    bool paste_mode;
    DbgStrBuf paste;
#if !defined(_WIN32)
    struct sigaction saved_winch;
    bool winch_installed;
#endif
};

static volatile sig_atomic_t g_dbg_winch_pending = 0;

static void dbg_sigwinch_handler(int signo)
{
    (void)signo;
    g_dbg_winch_pending = 1;
}

static DbgInputState *dbg_input_state_create(void)
{
    DbgInputState *in = dbg_xcalloc(1, sizeof(*in));
    sb_init(&in->paste);
#if !defined(_WIN32)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = dbg_sigwinch_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGWINCH, &sa, &in->saved_winch) == 0)
        in->winch_installed = true;
#endif
    return in;
}

static void dbg_input_state_free(DbgInputState *in)
{
    if (!in) return;
#if !defined(_WIN32)
    if (in->winch_installed) (void)sigaction(SIGWINCH, &in->saved_winch, NULL);
#endif
    sb_free(&in->paste);
    free(in);
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
    if (!q) {
        dbg_event_free(&ev);
        return;
    }
    if (q->cap == 0) {
        q->cap = DBG_INITIAL_EVENTQ;
        q->items = dbg_xmalloc(q->cap * sizeof(*q->items));
        q->head = q->tail = q->count = 0;
    }
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

static void dbg_event_free(DbgEvent *ev)
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
    if (s->focused_panel == DBG_PANEL_DISASM &&
        dbg_rect_contains(s->panel_rects[DBG_PANEL_DISASM], row, col))
        return DBG_PANEL_DISASM;
    for (int i = 0; i < (int)DBG_MAX_PANELS; i++) {
        if (i == DBG_PANEL_DISASM) continue;   /* shares the IR rectangle */
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

    char mid[160];
    DbgProcessState ps = s->engine ? dbg_engine_process_state(s->engine) : DBG_PROCESS_NONE;
    const char *reason = dbg_stop_reason_label(s->last_stop_reason);
    snprintf(mid, sizeof(mid), " panel:%s  target:%s%s%s  backend:%s ",
             dbg_panel_name(s->focused_panel), dbg_process_state_label(ps),
             *reason ? "/" : "", reason,
             s->engine ? dbg_engine_backend_name(s->engine) : "none");
    col = dbg_screen_write(sc, row, col, mid, th->status_line);

    const char *hint = " F5:continue F9:break F10:next F11:step M-x:palette ";
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
    size_t   cursor_pos;

    DbgCommand **commands;
    size_t       command_count;
    DbgCapabilities capabilities;
    DbgCommand   *pending_command;
    bool          argument_mode;

    DbgCompletionResult *results;
    size_t                result_count;
    int32_t               selected;
    int32_t               scroll_offset;

    DbgRect  rect;
    uint64_t opened_at_ms;
};

static void dbg_palette_bind_registry(DbgPalette *p);

static DbgPalette *dbg_palette_create(void)
{
    return dbg_xcalloc(1, sizeof(DbgPalette));
}

static void dbg_palette_free(DbgPalette *p)
{
    if (!p) return;
    dbg_completion_results_free(p->results, p->result_count);
    free(p->commands);
    free(p);
}

/* Capability-aware orderless filtering. Unsupported commands are absent from
   the palette instead of failing after selection; this is the same feature-
   negotiation principle used by modern debug adapters. */
static void dbg_palette_refilter(DbgPalette *p)
{
    dbg_completion_results_free(p->results, p->result_count);
    p->results = NULL;
    p->result_count = 0;

    if (p->argument_mode || p->command_count == 0) return;

    const char **names = dbg_xmalloc(p->command_count * sizeof(char *));
    void       **payloads = dbg_xmalloc(p->command_count * sizeof(void *));
    char       **owned_names = dbg_xmalloc(p->command_count * sizeof(char *));
    DbgStrBuf combined;
    sb_init(&combined);
    size_t visible_count = 0;

    for (size_t i = 0; i < p->command_count; i++) {
        DbgCommand *cmd = p->commands[i];
        if (cmd->required_capabilities &&
            ((p->capabilities & cmd->required_capabilities) != cmd->required_capabilities))
            continue;
        combined.len = 0;
        if (combined.data) combined.data[0] = '\0';
        sb_append(&combined, cmd->name);
        if (cmd->keywords) {
            sb_appendc(&combined, ' ');
            sb_append(&combined, cmd->keywords);
        }
        owned_names[visible_count] = dbg_xstrdup(combined.data);
        names[visible_count] = owned_names[visible_count];
        payloads[visible_count] = cmd;
        visible_count++;
    }
    sb_free(&combined);

    if (visible_count)
        p->results = dbg_orderless_filter(p->input, names, payloads,
                                           visible_count, &p->result_count);

    for (size_t i = 0; i < p->result_count; i++) {
        DbgCommand *cmd = (DbgCommand *)p->results[i].user_data;
        p->results[i].candidate = cmd->name;
    }
    for (size_t i = 0; i < visible_count; i++) free(owned_names[i]);
    free(owned_names);
    free(names);
    free(payloads);

    p->selected = p->result_count ? 0 : -1;
    p->scroll_offset = 0;
}

static void dbg_palette_open(DbgSession *s)
{
    if (!s->palette) s->palette = dbg_palette_create();
    if (!s->palette->commands) dbg_palette_bind_registry(s->palette);
    s->palette->capabilities = s->engine ? dbg_engine_capabilities(s->engine) : 0;
    s->palette->pending_command = NULL;
    s->palette->argument_mode = false;
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
    if (s->palette) {
        s->palette->argument_mode = false;
        s->palette->pending_command = NULL;
    }
    s->palette_open = false;
    s->dirty = true;
}

static void dbg_palette_insert_char(DbgPalette *p, uint32_t codepoint)
{
    char utf8[4];
    size_t n = dbg_encode_utf8(codepoint, utf8);
    if (p->input_len + n >= sizeof(p->input)) return;
    memmove(p->input + p->cursor_pos + n, p->input + p->cursor_pos,
            p->input_len - p->cursor_pos);
    memcpy(p->input + p->cursor_pos, utf8, n);
    p->cursor_pos += n;
    p->input_len += n;
    p->input[p->input_len] = '\0';
    if (!p->argument_mode) dbg_palette_refilter(p);
}

static void dbg_palette_backspace(DbgPalette *p)
{
    if (p->cursor_pos == 0) return;
    size_t prev = dbg_utf8_prev_boundary(p->input, p->cursor_pos);
    size_t removed = p->cursor_pos - prev;
    memmove(p->input + prev, p->input + p->cursor_pos,
            p->input_len - p->cursor_pos + 1);
    p->cursor_pos = prev;
    p->input_len -= removed;
    if (!p->argument_mode) dbg_palette_refilter(p);
}

static void dbg_palette_move_cursor(DbgPalette *p, int direction)
{
    if (!p) return;
    if (direction < 0) p->cursor_pos = dbg_utf8_prev_boundary(p->input, p->cursor_pos);
    else if (direction > 0) p->cursor_pos = dbg_utf8_next_boundary(p->input, p->input_len,
                                                                   p->cursor_pos);
}

static void dbg_palette_move_selection(DbgPalette *p, int delta)
{
    if (p->argument_mode || p->result_count == 0) return;
    int32_t next = p->selected + delta;
    if (next < 0) next = 0;
    if (next >= (int32_t)p->result_count) next = (int32_t)p->result_count - 1;
    p->selected = next;
    if (p->selected < p->scroll_offset) p->scroll_offset = p->selected;
    int visible = p->rect.height - 1;
    if (p->selected >= p->scroll_offset + visible)
        p->scroll_offset = p->selected - visible + 1;
}

static void dbg_palette_activate(DbgSession *s)
{
    DbgPalette *p = s->palette;
    if (!p) return;

    if (p->argument_mode && p->pending_command) {
        DbgCommand *cmd = p->pending_command;
        char arg[sizeof(p->input)];
        memcpy(arg, p->input, p->input_len + 1);
        dbg_palette_close(s);
        if (cmd->run) cmd->run(s, arg);
        return;
    }

    if (p->selected < 0 || (size_t)p->selected >= p->result_count) return;
    DbgCommand *cmd = (DbgCommand *)p->results[p->selected].user_data;
    if (!cmd) return;
    if (cmd->needs_arg) {
        p->pending_command = cmd;
        p->argument_mode = true;
        p->input[0] = '\0';
        p->input_len = p->cursor_pos = 0;
        dbg_completion_results_free(p->results, p->result_count);
        p->results = NULL;
        p->result_count = 0;
        p->selected = -1;
        s->dirty = true;
        return;
    }
    dbg_palette_close(s);
    if (cmd->run) cmd->run(s, NULL);
}

static bool dbg_palette_handle_mouse(DbgSession *s, DbgMouseEvent *ev)
{
    DbgPalette *p = s->palette;
    if (!p || !dbg_rect_contains(p->rect, ev->row, ev->col)) return false;
    if (p->argument_mode) return true;

    if (ev->kind == DBG_MOUSE_WHEEL_UP)   { dbg_palette_move_selection(p, -1); return true; }
    if (ev->kind == DBG_MOUSE_WHEEL_DOWN) { dbg_palette_move_selection(p, 1);  return true; }

    if (ev->kind == DBG_MOUSE_DOWN && ev->button == DBG_BTN_LEFT) {
        int list_row = ev->row - p->rect.row - 1;
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

static void dbg_palette_render_candidate(DbgScreen *sc, int row, int col,
                                          int width, DbgCompletionResult *res,
                                          DbgStyle base, DbgStyle match_style)
{
    size_t len = strlen(res->candidate);
    for (size_t i = 0; i < len && (int)i < width; i++) {
        bool in_span = false;
        for (size_t j = 0; j < res->span_count; j++) {
            if (i >= res->spans[j].start && i < res->spans[j].start + res->spans[j].len) {
                in_span = true;
                break;
            }
        }
        dbg_screen_put(sc, row, col + (int)i, (unsigned char)res->candidate[i],
                       in_span ? match_style : base);
    }
}

static void dbg_palette_render(DbgSession *s)
{
    DbgPalette *p = s->palette;
    DbgScreen  *sc = s->screen;
    DbgTheme   *th = s->theme;

    int width  = sc->cols * 2 / 3;
    int height = sc->rows / 2;
    if (height < 4) height = 4;
    int row = sc->rows / 6;
    int col = (sc->cols - width) / 2;
    p->rect = (DbgRect){ row, col, height, width };

    dbg_screen_fill_rect(sc, p->rect, ' ', th->base);
    for (int c = col; c < col + width; c++) {
        dbg_screen_put(sc, row - 1, c, '-', th->palette_border);
        dbg_screen_put(sc, row + height, c, '-', th->palette_border);
    }
    for (int r = row - 1; r <= row + height; r++) {
        dbg_screen_put(sc, r, col - 1, '|', th->palette_border);
        dbg_screen_put(sc, r, col + width, '|', th->palette_border);
    }

    DbgStrBuf prompt;
    sb_init(&prompt);
    if (p->argument_mode && p->pending_command) {
        sb_append(&prompt, p->pending_command->name);
        sb_append(&prompt, " ");
    } else {
        sb_append(&prompt, "M-x ");
    }
    sb_append(&prompt, p->input);
    dbg_screen_write(sc, row, col, prompt.data, th->base);
    sb_free(&prompt);

    if (!p->argument_mode) {
        char countbuf[32];
        snprintf(countbuf, sizeof(countbuf), "%zu", p->result_count);
        dbg_screen_write(sc, row, col + width - (int)strlen(countbuf) - 1,
                         countbuf, th->base);
    }

    int list_top = row + 1;
    int list_height = height - 1;
    for (int i = 0; i < list_height; i++) {
        int idx = p->scroll_offset + i;
        int screen_row = list_top + i;
        DbgStyle row_style = (idx == p->selected) ? th->selection : th->base;
        dbg_screen_fill_rect(sc, (DbgRect){ screen_row, col, 1, width }, ' ', row_style);
        if (!p->argument_mode && idx >= 0 && (size_t)idx < p->result_count)
            dbg_palette_render_candidate(sc, screen_row, col + 1, width - 2,
                                         &p->results[idx], row_style,
                                         th->palette_match);
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

static void dbg_registry_free(void);

static void dbg_registry_init(void)
{
    g_dbg_registry.cap = DBG_INITIAL_COMMANDS;
    g_dbg_registry.commands = dbg_xmalloc(g_dbg_registry.cap * sizeof(DbgCommand));
    g_dbg_registry.count = 0;
    atexit(dbg_registry_free);
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

void dbg_error_snapshot_add_frame(DbgErrorSnapshot *snap, const char *function,
                                  const char *file, uint32_t line,
                                  uint32_t column, const char *ir_value)
{
    if (!snap) return;
    dbg_backtrace_push(&snap->backtrace, function ? function : "<anonymous>",
                       file, line, column, ir_value);
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


/// §20  Breakpoints & watch expressions

/* Logical breakpoint/watch state is owned by DbgEngine.  The TUI keeps only
   selection/scroll state, so pending breakpoints survive target launch,
   restart and module reloads. */
static DbgBreakpoint *dbg_breakpoint_at_line(DbgSession *s, const char *file,
                                              uint32_t line)
{
    if (!s || !s->engine) return NULL;
    size_t count = dbg_engine_breakpoint_count(s->engine);
    for (size_t i = 0; i < count; i++) {
        DbgBreakpoint *bp = dbg_engine_breakpoint_at(s->engine, i);
        if (!bp || bp->kind != DBG_BP_SOURCE || bp->as.source.location.line != line) continue;
        const char *bp_file = bp->as.source.location.file;
        if (bp_file && file && strcmp(bp_file, file) != 0) continue;
        return bp;
    }
    return NULL;
}

static void dbg_breakpoint_toggle_at_line(DbgSession *s, const char *file,
                                           uint32_t line)
{
    if (!s || !s->engine || !file || line == 0) return;
    DbgBreakpoint *existing = dbg_breakpoint_at_line(s, file, line);
    char *err = NULL;
    if (existing) {
        if (!dbg_engine_remove_breakpoint(s->engine, existing->id, &err) && err)
            dbg_console_appendf(s, "breakpoint: %s", err);
    } else {
        uint32_t id = dbg_engine_add_source_breakpoint(s->engine, file, line, 0);
        if (id == 0) dbg_console_appendf(s, "breakpoint: unable to create breakpoint");
    }
    free(err);
    s->dirty = true;
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
static DbgSourceMap *dbg_source_map_load(const char *path)
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

static void dbg_source_map_free(DbgSourceMap *map)
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
    if (!panel || !panel->ir_text || panel->ir_len == 0) return;
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
    if (!panel || !panel->ir_text) return;
    uint32_t cap = 256;
    panel->line_offsets = dbg_xmalloc(cap * sizeof(uint32_t));
    panel->line_count = 0;
    panel->line_offsets[panel->line_count++] = 0;

    const char *base = panel->ir_text;
    const char *cursor = base;
    const char *end = base + panel->ir_len;
    while (cursor < end) {
        const char *nl = memchr(cursor, '\n', (size_t)(end - cursor));
        if (!nl) break;
        if (panel->line_count >= cap) {
            cap *= DBG_GROW_FACTOR;
            panel->line_offsets = dbg_xrealloc(panel->line_offsets,
                                                cap * sizeof(uint32_t));
        }
        panel->line_offsets[panel->line_count++] = (uint32_t)((nl - base) + 1);
        cursor = nl + 1;
    }
}

static DbgIrPanel *dbg_ir_panel_emit(const char *emit_ir_command,
                              const char *source_path,
                              char **error_out)
{
    if (error_out) *error_out = NULL;
    if (!emit_ir_command || !*emit_ir_command)
        emit_ir_command = "monad --emit-ir";
    if (!source_path || !*source_path) {
        dbg_error_out_set(error_out, "emit-ir source path is empty");
        return NULL;
    }

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
    char chunk[DBG_READ_CHUNK] = {0};
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

static void dbg_ir_panel_free(DbgIrPanel *panel)
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
    dbg_value_info_free(&e->value);
    for (size_t i = 0; i < e->child_count; i++)
        dbg_var_entry_free(e->children[i]);
    free(e->children);
    free(e);
}

static void dbg_var_inspector_clear(DbgVarInspector *vi)
{
    if (!vi) return;
    for (size_t i = 0; i < vi->local_count; i++) dbg_var_entry_free(vi->locals[i]);
    free(vi->locals);
    vi->locals = NULL;
    vi->local_count = 0;
    for (size_t i = 0; i < vi->global_count; i++) dbg_var_entry_free(vi->globals[i]);
    free(vi->globals);
    vi->globals = NULL;
    vi->global_count = 0;
    vi->selected_index = 0;
    vi->scroll_offset = 0;
}

static void dbg_var_inspector_free(DbgVarInspector *vi)
{
    if (!vi) return;
    dbg_var_inspector_clear(vi);
    free(vi);
}

static DbgVarEntry *dbg_var_entry_from_value(const DbgValueInfo *v)
{
    if (!v) return NULL;
    DbgVarEntry *e = dbg_xcalloc(1, sizeof(*e));
    dbg_value_info_copy(&e->value, v);
    e->kind = (v->variables_reference || v->named_children || v->indexed_children)
              ? DBG_VAL_AGGREGATE : DBG_VAL_SCALAR;
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
    bool has_children = e->child_count > 0 || e->value.variables_reference != 0 ||
                        e->value.named_children > 0 || e->value.indexed_children > 0;
    if (has_children) sb_append(&b, e->expanded ? "- " : "+ ");
    sb_append(&b, e->value.name);
    if (e->value.type_name) { sb_append(&b, " : "); sb_append(&b, e->value.type_name); }
    if (e->value.availability == DBG_VALUE_OPTIMIZED_OUT) {
        sb_append(&b, " = <optimized out>");
    } else if (e->value.availability == DBG_VALUE_UNINITIALIZED) {
        sb_append(&b, " = <uninitialized>");
    } else if (e->value.availability == DBG_VALUE_UNREADABLE) {
        sb_append(&b, " = <unreadable>");
    } else if (e->value.value) {
        sb_append(&b, " = "); sb_append(&b, e->value.value);
    }
    if (e->value.summary && *e->value.summary) { sb_append(&b, "  "); sb_append(&b, e->value.summary); }

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

/* Same traversal as dbg_var_flatten, but returns stable model pointers instead
   of allocating display text.  Keeping the two traversals structurally
   identical makes a rendered row map exactly to the value object it controls. */
static void dbg_var_collect_entries(DbgVarEntry *e, DbgVarEntry ***out,
                                    size_t *count, size_t *cap)
{
    if (!e) return;
    if (*count >= *cap) {
        *cap = *cap ? *cap * DBG_GROW_FACTOR : 16;
        *out = dbg_xrealloc(*out, *cap * sizeof(**out));
    }
    (*out)[(*count)++] = e;
    if (e->expanded) {
        for (size_t i = 0; i < e->child_count; i++)
            dbg_var_collect_entries(e->children[i], out, count, cap);
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


/// §25  Session model — TUI client of DbgEngine

static void dbg_append_buffer(char **buffer, size_t *len, size_t *cap,
                              const char *text)
{
    if (!text || !*text) return;
    size_t n = strlen(text);
    size_t need = *len + n + 1;
    if (need > *cap) {
        size_t newcap = *cap ? *cap : 1024;
        while (newcap < need) newcap *= DBG_GROW_FACTOR;
        *buffer = dbg_xrealloc(*buffer, newcap);
        *cap = newcap;
    }
    memcpy(*buffer + *len, text, n);
    *len += n;
    (*buffer)[*len] = '\0';
}

static void dbg_session_clear_runtime_snapshots(DbgSession *s)
{
    if (!s) return;
    dbg_thread_infos_free(s->threads, s->thread_count);
    s->threads = NULL;
    s->thread_count = 0;
    dbg_stack_frame_infos_free(s->runtime_frames, s->runtime_frame_count);
    s->runtime_frames = NULL;
    s->runtime_frame_count = 0;
    s->runtime_total_frames = 0;
    dbg_var_inspector_clear(s->vars);
}

static void dbg_var_inspector_append(DbgVarInspector *vi, DbgVarEntry *entry,
                                     bool global)
{
    if (!entry) return;
    if (!vi) { dbg_var_entry_free(entry); return; }
    if (global) {
        vi->globals = dbg_xrealloc(vi->globals,
                                   (vi->global_count + 1) * sizeof(*vi->globals));
        vi->globals[vi->global_count++] = entry;
    } else {
        vi->locals = dbg_xrealloc(vi->locals,
                                  (vi->local_count + 1) * sizeof(*vi->locals));
        vi->locals[vi->local_count++] = entry;
    }
}

static bool dbg_scope_looks_global(const char *name)
{
    uint32_t ignored = 0;
    return name && (dbg_str_contains_ci(name, "global", &ignored) ||
                    dbg_str_contains_ci(name, "static", &ignored));
}

static bool dbg_var_entry_load_children(DbgSession *s, DbgVarEntry *entry)
{
    if (!s || !s->engine || !entry) return false;
    if (entry->child_count > 0 || entry->value.variables_reference == 0) return true;

    size_t start = 0;
    size_t expected = entry->value.named_children + entry->value.indexed_children;
    for (;;) {
        DbgValueInfo *values = NULL;
        size_t count = 0;
        char *err = NULL;
        if (!dbg_engine_variables(s->engine, entry->value.variables_reference, start,
                                  (s->config->variable_page_size ? s->config->variable_page_size : DBG_DEFAULT_VARIABLE_PAGE_SIZE), &values, &count, &err)) {
            if (err) dbg_console_appendf(s, "expand %s: %s",
                                         entry->value.name ? entry->value.name : "value", err);
            free(err);
            dbg_value_infos_free(values, count);
            return false;
        }
        free(err);
        if (count) {
            entry->children = dbg_xrealloc(entry->children,
                (entry->child_count + count) * sizeof(*entry->children));
            for (size_t i = 0; i < count; i++)
                entry->children[entry->child_count++] = dbg_var_entry_from_value(&values[i]);
        }
        dbg_value_infos_free(values, count);
        start += count;

        if (count < (s->config->variable_page_size ? s->config->variable_page_size : DBG_DEFAULT_VARIABLE_PAGE_SIZE)) break;
        if (expected && start >= expected) break;
        if (start >= (s->config->variable_child_limit ? s->config->variable_child_limit : DBG_DEFAULT_VARIABLE_CHILD_LIMIT)) {
            dbg_console_appendf(s, "expand %s: capped at %u children",
                                entry->value.name ? entry->value.name : "value",
                                (unsigned)(s->config->variable_child_limit ? s->config->variable_child_limit : DBG_DEFAULT_VARIABLE_CHILD_LIMIT));
            break;
        }
    }
    entry->value.presentation &= ~DBG_VALUE_PRESENTATION_LAZY;
    return true;
}

static void dbg_session_append_scope_values(DbgSession *s,
                                            const DbgScopeInfo *scope)
{
    if (!s || !scope || scope->variables_reference == 0) return;
    bool global = dbg_scope_looks_global(scope->name);
    size_t start = 0;
    for (;;) {
        DbgValueInfo *values = NULL;
        size_t count = 0;
        char *err = NULL;
        if (!dbg_engine_variables(s->engine, scope->variables_reference, start,
                                  (s->config->variable_page_size ? s->config->variable_page_size : DBG_DEFAULT_VARIABLE_PAGE_SIZE), &values, &count, &err)) {
            if (err) dbg_console_appendf(s, "%s: %s",
                                         scope->name ? scope->name : "scope", err);
            free(err);
            dbg_value_infos_free(values, count);
            return;
        }
        free(err);
        for (size_t i = 0; i < count; i++)
            dbg_var_inspector_append(s->vars, dbg_var_entry_from_value(&values[i]), global);
        dbg_value_infos_free(values, count);
        start += count;
        if (count < (s->config->variable_page_size ? s->config->variable_page_size : DBG_DEFAULT_VARIABLE_PAGE_SIZE) || start >= (s->config->variable_child_limit ? s->config->variable_child_limit : DBG_DEFAULT_VARIABLE_CHILD_LIMIT)) break;
    }
}

static void dbg_session_refresh_disassembly(DbgSession *s, uint64_t address)
{
    if (!s || !s->engine || address == DBG_INVALID_ADDRESS || address == 0 ||
        !dbg_engine_has_capability(s->engine, DBG_CAP_DISASSEMBLE)) return;

    DbgInstructionInfo *insns = NULL;
    size_t count = 0;
    char *err = NULL;
    if (!dbg_engine_disassemble(s->engine, address, -16,
                               s->config->disassembly_instruction_count
                                   ? s->config->disassembly_instruction_count
                                   : DBG_DEFAULT_DISASSEMBLY_COUNT,
                               &insns, &count, &err)) {
        if (err) dbg_console_appendf(s, "disassemble: %s", err);
        free(err);
        return;
    }
    free(err);

    DbgDisasmPanel *panel = dbg_xcalloc(1, sizeof(*panel));
    panel->lines = count ? dbg_xcalloc(count, sizeof(*panel->lines)) : NULL;
    panel->line_count = count;
    for (size_t i = 0; i < count; i++) {
        panel->lines[i].address = insns[i].address;
        panel->lines[i].bytes_hex = insns[i].bytes_hex ? dbg_xstrdup(insns[i].bytes_hex) : NULL;
        panel->lines[i].mnemonic = insns[i].mnemonic ? dbg_xstrdup(insns[i].mnemonic) : NULL;
        panel->lines[i].operands = insns[i].operands ? dbg_xstrdup(insns[i].operands) : NULL;
        panel->lines[i].is_current_pc = (insns[i].address == address);
        size_t bp_count = dbg_engine_breakpoint_count(s->engine);
        for (size_t j = 0; j < bp_count; j++) {
            const DbgBreakpoint *bp = dbg_engine_breakpoint_at_const(s->engine, j);
            if (!bp || !bp->enabled) continue;
            if (bp->kind == DBG_BP_INSTRUCTION &&
                bp->as.instruction.address + (uint64_t)bp->as.instruction.offset == insns[i].address) {
                panel->lines[i].has_breakpoint = true;
                break;
            }
            for (size_t k = 0; k < bp->site_count; k++) {
                if (bp->sites[k].resolved && bp->sites[k].location.address == insns[i].address) {
                    panel->lines[i].has_breakpoint = true;
                    break;
                }
            }
        }
    }
    dbg_instruction_infos_free(insns, count);
    dbg_disasm_panel_free(s->disasm);
    s->disasm = panel;
    dbg_disasm_follow_pc(panel, s->panel_rects[DBG_PANEL_IR].height);
}

static void dbg_session_refresh_watches(DbgSession *s, uint64_t frame_id)
{
    if (!s || !s->engine || !dbg_engine_has_capability(s->engine, DBG_CAP_EVALUATE)) return;
    size_t count = dbg_engine_watch_count(s->engine);
    for (size_t i = 0; i < count; i++) {
        DbgWatch *w = dbg_engine_watch_at(s->engine, i);
        if (!w) continue;
        DbgValueInfo value = {0};
        char *err = NULL;
        DbgEvaluationOptions options = {
            .context = DBG_EVAL_WATCH, .format = DBG_FORMAT_NATURAL,
            .allow_side_effects = false, .allow_function_calls = false,
            .prefer_dynamic = true, .prefer_synthetic = true
        };
        bool ok = dbg_engine_evaluate(s->engine, frame_id, w->expression,
                                      &options, &value, &err);
        const char *rendered = ok ? (value.value ? value.value : value.summary) : NULL;
        bool changed = false;
        if (rendered) changed = !w->last_value || strcmp(w->last_value, rendered) != 0;
        else if (err) changed = !w->last_error || strcmp(w->last_error, err) != 0;
        w->changed_last_step = changed;
        free(w->last_value);
        free(w->last_error);
        w->last_value = rendered ? dbg_xstrdup(rendered) : NULL;
        w->last_error = err ? dbg_xstrdup(err) : NULL;
        if (changed) {
            if (rendered) dbg_console_appendf(s, "watch %s = %s", w->expression, rendered);
            else if (err) dbg_console_appendf(s, "watch %s: %s", w->expression, err);
        }
        free(err);
        dbg_value_info_free(&value);
    }
}

static void dbg_session_refresh_frame(DbgSession *s, DbgStackFrameInfo *frame)
{
    if (!s || !s->engine || !frame) return;
    dbg_engine_select_frame(s->engine, frame->id);

    if (frame->location.source.file && frame->location.source.line) {
        if (!s->source || !s->source->path ||
            strcmp(s->source->path, frame->location.source.file) != 0) {
            DbgSourceMap *fresh = dbg_source_map_load(frame->location.source.file);
            if (fresh) {
                dbg_source_map_free(s->source);
                s->source = fresh;
            }
        }
        s->source_cursor_line = (int32_t)frame->location.source.line - 1;
        if (s->source_cursor_line < 0) s->source_cursor_line = 0;
        int h = s->panel_rects[DBG_PANEL_SOURCE].height;
        if (h > 0) {
            s->source_scroll = s->source_cursor_line - h / 2;
            if (s->source_scroll < 0) s->source_scroll = 0;
        }
    }

    if (s->ir_panel) {
        free(s->ir_panel->highlighted_value);
        s->ir_panel->highlighted_value = frame->location.ir_value
                                       ? dbg_xstrdup(frame->location.ir_value) : NULL;
    }

    dbg_var_inspector_clear(s->vars);
    DbgScopeInfo *scopes = NULL;
    size_t scope_count = 0;
    char *err = NULL;
    if (dbg_engine_scopes(s->engine, frame->id, &scopes, &scope_count, &err)) {
        for (size_t i = 0; i < scope_count; i++)
            dbg_session_append_scope_values(s, &scopes[i]);
    } else if (err) {
        dbg_console_appendf(s, "scopes: %s", err);
    }
    free(err);
    dbg_scope_infos_free(scopes, scope_count);

    dbg_session_refresh_watches(s, frame->id);
    uint64_t pc = frame->location.address;
    if (pc == DBG_INVALID_ADDRESS || pc == 0) pc = s->last_stop_address;
    dbg_session_refresh_disassembly(s, pc);
    s->dirty = true;
}

static void dbg_session_refresh_runtime(DbgSession *s)
{
    if (!s || !s->engine || dbg_engine_process_state(s->engine) != DBG_PROCESS_STOPPED)
        return;

    dbg_session_clear_runtime_snapshots(s);
    char *err = NULL;
    if (!dbg_engine_threads(s->engine, &s->threads, &s->thread_count, &err)) {
        if (err) dbg_console_appendf(s, "threads: %s", err);
        free(err);
        return;
    }
    free(err); err = NULL;

    uint64_t thread_id = dbg_engine_selected_thread(s->engine);
    if ((thread_id == DBG_INVALID_ID || thread_id == 0) && s->thread_count) {
        thread_id = s->threads[0].id;
        for (size_t i = 0; i < s->thread_count; i++)
            if (s->threads[i].selected) { thread_id = s->threads[i].id; break; }
        dbg_engine_select_thread(s->engine, thread_id);
    }
    if (thread_id == DBG_INVALID_ID || thread_id == 0) return;

    if (!dbg_engine_stack_trace(s->engine, thread_id, 0, DBG_MAX_FRAMES,
                                &s->runtime_frames, &s->runtime_frame_count,
                                &s->runtime_total_frames, &err)) {
        if (err) dbg_console_appendf(s, "stack-trace: %s", err);
        free(err);
        return;
    }
    free(err);
    if (!s->runtime_frame_count) return;

    s->backtrace_selected = 0;
    dbg_session_refresh_frame(s, &s->runtime_frames[0]);
}

static void dbg_session_handle_engine_event(DbgSession *s, DbgEngineEvent *ev)
{
    if (!s || !ev) return;
    switch (ev->kind) {
    case DBG_ENGINE_EVENT_PROCESS:
        dbg_console_appendf(s, "process %llu: %s",
                            (unsigned long long)ev->as.process.process_id,
                            dbg_process_state_label(ev->as.process.state));
        break;
    case DBG_ENGINE_EVENT_STOPPED: {
        const DbgCodeLocation *loc = &ev->as.stopped.location;
        s->last_stop_reason = ev->as.stopped.reason;
        s->last_stop_address = ev->as.stopped.address;
        if (loc->source.file && loc->source.line) {
            if (!s->source || !s->source->path ||
                strcmp(s->source->path, loc->source.file) != 0) {
                DbgSourceMap *fresh = dbg_source_map_load(loc->source.file);
                if (fresh) { dbg_source_map_free(s->source); s->source = fresh; }
            }
            s->source_cursor_line = (int32_t)loc->source.line - 1;
        }
        if (ev->as.stopped.description)
            dbg_console_appendf(s, "stopped (%s): %s",
                                dbg_stop_reason_label(ev->as.stopped.reason),
                                ev->as.stopped.description);
        if (s->config->auto_refresh_on_stop) dbg_session_refresh_runtime(s);
        break;
    }
    case DBG_ENGINE_EVENT_CONTINUED:
        s->last_stop_reason = DBG_STOP_NONE;
        break;
    case DBG_ENGINE_EVENT_EXITED:
        dbg_console_appendf(s, "process exited with status %d", ev->as.exited.exit_code);
        break;
    case DBG_ENGINE_EVENT_THREAD_CREATED:
        dbg_console_appendf(s, "thread %llu created",
                            (unsigned long long)ev->as.thread.thread_id);
        break;
    case DBG_ENGINE_EVENT_THREAD_EXITED:
        dbg_console_appendf(s, "thread %llu exited",
                            (unsigned long long)ev->as.thread.thread_id);
        break;
    case DBG_ENGINE_EVENT_OUTPUT:
        if (ev->as.output.text) {
            if (ev->as.output.category == DBG_OUTPUT_STDOUT) {
                dbg_append_buffer(&s->target_stdout_log, &s->target_stdout_len,
                                  &s->target_stdout_cap, ev->as.output.text);
                dbg_console_appendf(s, "[stdout] %s", ev->as.output.text);
            } else if (ev->as.output.category == DBG_OUTPUT_STDERR) {
                dbg_append_buffer(&s->target_stderr_log, &s->target_stderr_len,
                                  &s->target_stderr_cap, ev->as.output.text);
                dbg_console_appendf(s, "[stderr] %s", ev->as.output.text);
            } else {
                dbg_console_appendf(s, "%s", ev->as.output.text);
            }
        }
        break;
    case DBG_ENGINE_EVENT_BREAKPOINT:
        if (ev->as.breakpoint.message)
            dbg_console_appendf(s, "breakpoint %u: %s",
                                ev->as.breakpoint.breakpoint_id,
                                ev->as.breakpoint.message);
        break;
    case DBG_ENGINE_EVENT_MODULE_LOADED:
        dbg_console_appendf(s, "module loaded: %s",
                            ev->as.module.path ? ev->as.module.path : "<unknown>");
        break;
    case DBG_ENGINE_EVENT_MODULE_UNLOADED:
        dbg_console_appendf(s, "module unloaded: %s",
                            ev->as.module.path ? ev->as.module.path : "<unknown>");
        break;
    case DBG_ENGINE_EVENT_MEMORY:
        dbg_console_appendf(s, "memory changed: 0x%llx + %zu",
                            (unsigned long long)ev->as.memory.address,
                            ev->as.memory.length);
        break;
    case DBG_ENGINE_EVENT_REPLAY_POSITION:
        dbg_console_appendf(s, "replay position: %llu",
                            (unsigned long long)ev->as.replay.position);
        break;
    case DBG_ENGINE_EVENT_INVALIDATED:
        if (ev->as.invalidated.areas & (DBG_INVALIDATE_THREADS | DBG_INVALIDATE_STACKS |
                                        DBG_INVALIDATE_SCOPES | DBG_INVALIDATE_VARIABLES))
            dbg_session_refresh_runtime(s);
        break;
    case DBG_ENGINE_EVENT_CAPABILITIES:
        dbg_console_appendf(s, "backend capabilities changed");
        break;
    case DBG_ENGINE_EVENT_COMPILER:
        if (ev->as.compiler.message)
            dbg_console_appendf(s, "compiler: %s", ev->as.compiler.message);
        break;
    case DBG_ENGINE_EVENT_NONE:
        break;
    }
    s->dirty = true;
}

static void dbg_session_pump_engine(DbgSession *s)
{
    if (!s || !s->engine) return;
    uint32_t budget = s->config->engine_event_budget ? s->config->engine_event_budget : 64;
    for (uint32_t i = 0; i < budget; i++) {
        DbgEngineEvent ev = {0};
        char *err = NULL;
        bool got = dbg_engine_poll_event(s->engine, &ev, &err);
        if (!got) {
            if (err) dbg_console_appendf(s, "backend event: %s", err);
            free(err);
            break;
        }
        free(err);
        dbg_session_handle_engine_event(s, &ev);
        dbg_engine_event_free(&ev);
    }
}

DbgConfig dbg_default_config(void)
{
    DbgConfig cfg = {0};
    cfg.mouse_enabled = true;
    cfg.truecolor_force = false;
    cfg.blink_period_ms = DBG_BLINK_PERIOD_MS;
    cfg.blink_max_count = DBG_BLINK_MAX_COUNT;
    cfg.target_fps = 60;
    cfg.emit_ir_command = "monad --emit-ir";
    cfg.auto_refresh_on_stop = true;
    cfg.default_run_mode = DBG_RUN_ALL_THREADS;
    cfg.engine_event_budget = 64;
    cfg.variable_page_size = DBG_DEFAULT_VARIABLE_PAGE_SIZE;
    cfg.variable_child_limit = DBG_DEFAULT_VARIABLE_CHILD_LIMIT;
    cfg.disassembly_instruction_count = DBG_DEFAULT_DISASSEMBLY_COUNT;
    return cfg;
}

static uint64_t dbg_session_thread_id(DbgSession *s)
{
    uint64_t id = s && s->engine ? dbg_engine_selected_thread(s->engine) : DBG_INVALID_ID;
    return (id == DBG_INVALID_ID) ? 0 : id;
}

static void dbg_command_result(DbgSession *s, const char *action, bool ok, char *err)
{
    if (!ok) dbg_console_appendf(s, "%s: %s", action, err ? err : "operation failed");
    free(err);
    s->dirty = true;
}

static void cmd_toggle_breakpoint(DbgSession *s, const char *arg)
{
    (void)arg;
    if (!s->source || !s->engine) return;
    uint32_t line = s->source_cursor_line >= 0 ? (uint32_t)s->source_cursor_line + 1 : 1;
    dbg_breakpoint_toggle_at_line(s, s->source->path, line);
}

static void cmd_continue(DbgSession *s, const char *arg)
{
    (void)arg; char *err = NULL;
    bool ok = dbg_engine_continue(s->engine, dbg_session_thread_id(s),
                                  s->config->default_run_mode, DBG_DIR_FORWARD, &err);
    dbg_command_result(s, "continue", ok, err);
}

static void cmd_pause(DbgSession *s, const char *arg)
{
    (void)arg; char *err = NULL;
    bool ok = dbg_engine_pause(s->engine, dbg_session_thread_id(s),
                               s->config->default_run_mode == DBG_RUN_ALL_THREADS, &err);
    dbg_command_result(s, "pause", ok, err);
}

static void dbg_command_step(DbgSession *s, DbgStepAction action,
                             DbgStepGranularity granularity,
                             DbgExecutionDirection direction, const char *name)
{
    char *err = NULL;
    bool ok = dbg_engine_step(s->engine, dbg_session_thread_id(s), action,
                              granularity, s->config->default_run_mode,
                              direction, &err);
    dbg_command_result(s, name, ok, err);
}

static void cmd_step_in(DbgSession *s, const char *arg)
{ (void)arg; dbg_command_step(s, DBG_STEP_INTO, DBG_GRANULARITY_STATEMENT, DBG_DIR_FORWARD, "step-in"); }
static void cmd_step_over(DbgSession *s, const char *arg)
{ (void)arg; dbg_command_step(s, DBG_STEP_OVER, DBG_GRANULARITY_STATEMENT, DBG_DIR_FORWARD, "step-over"); }
static void cmd_step_out(DbgSession *s, const char *arg)
{ (void)arg; dbg_command_step(s, DBG_STEP_OUT, DBG_GRANULARITY_STATEMENT, DBG_DIR_FORWARD, "step-out"); }
static void cmd_step_expression(DbgSession *s, const char *arg)
{ (void)arg; dbg_command_step(s, DBG_STEP_INTO, DBG_GRANULARITY_EXPRESSION, DBG_DIR_FORWARD, "step-expression"); }
static void cmd_next_expression(DbgSession *s, const char *arg)
{ (void)arg; dbg_command_step(s, DBG_STEP_OVER, DBG_GRANULARITY_EXPRESSION, DBG_DIR_FORWARD, "next-expression"); }
static void cmd_step_ir(DbgSession *s, const char *arg)
{ (void)arg; dbg_command_step(s, DBG_STEP_INTO, DBG_GRANULARITY_IR_INSTRUCTION, DBG_DIR_FORWARD, "step-ir"); }
static void cmd_next_ir(DbgSession *s, const char *arg)
{ (void)arg; dbg_command_step(s, DBG_STEP_OVER, DBG_GRANULARITY_IR_INSTRUCTION, DBG_DIR_FORWARD, "next-ir"); }
static void cmd_step_instruction(DbgSession *s, const char *arg)
{ (void)arg; dbg_command_step(s, DBG_STEP_INTO, DBG_GRANULARITY_INSTRUCTION, DBG_DIR_FORWARD, "step-instruction"); }
static void cmd_next_instruction(DbgSession *s, const char *arg)
{ (void)arg; dbg_command_step(s, DBG_STEP_OVER, DBG_GRANULARITY_INSTRUCTION, DBG_DIR_FORWARD, "next-instruction"); }
static void cmd_step_back(DbgSession *s, const char *arg)
{ (void)arg; dbg_command_step(s, DBG_STEP_OVER, DBG_GRANULARITY_STATEMENT, DBG_DIR_REVERSE, "step-back"); }

static void cmd_reverse_continue(DbgSession *s, const char *arg)
{
    (void)arg; char *err = NULL;
    bool ok = dbg_engine_continue(s->engine, dbg_session_thread_id(s),
                                  s->config->default_run_mode, DBG_DIR_REVERSE, &err);
    dbg_command_result(s, "reverse-continue", ok, err);
}

static void cmd_restart(DbgSession *s, const char *arg)
{
    (void)arg; char *err = NULL;
    bool ok = dbg_engine_restart(s->engine, &err);
    dbg_command_result(s, "restart", ok, err);
}

static void cmd_restart_frame(DbgSession *s, const char *arg)
{
    (void)arg; char *err = NULL;
    uint64_t frame = dbg_engine_selected_frame(s->engine);
    bool ok = dbg_engine_restart_frame(s->engine, frame, &err);
    dbg_command_result(s, "restart-frame", ok, err);
}

static void cmd_evaluate(DbgSession *s, const char *arg)
{
    if (!arg || !*arg) return;
    DbgValueInfo v = {0};
    char *err = NULL;
    uint64_t frame = dbg_engine_selected_frame(s->engine);
    DbgEvaluationOptions options = {
        .context = DBG_EVAL_REPL, .format = DBG_FORMAT_NATURAL,
        .allow_side_effects = false, .allow_function_calls = false,
        .prefer_dynamic = true, .prefer_synthetic = true
    };
    bool ok = dbg_engine_evaluate(s->engine, frame, arg, &options, &v, &err);
    if (ok) {
        dbg_console_appendf(s, "%s = %s%s%s", arg,
                            v.value ? v.value : "<no value>",
                            v.type_name ? " : " : "", v.type_name ? v.type_name : "");
    } else {
        dbg_console_appendf(s, "evaluate: %s", err ? err : "failed");
    }
    free(err);
    dbg_value_info_free(&v);
}

static void cmd_watch_add(DbgSession *s, const char *arg)
{
    if (!arg || !*arg) return;
    uint32_t id = dbg_engine_add_watch(s->engine, arg);
    if (id) dbg_console_appendf(s, "watch %u: %s", id, arg);
}

static void cmd_checkpoint(DbgSession *s, const char *arg)
{
    (void)arg;
    uint64_t id = 0; char *err = NULL;
    bool ok = dbg_engine_checkpoint(s->engine, &id, &err);
    if (ok) dbg_console_appendf(s, "checkpoint %llu", (unsigned long long)id);
    else dbg_console_appendf(s, "checkpoint: %s", err ? err : "failed");
    free(err);
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
    s->active_error = s->error_stack_count
                      ? s->error_stack[s->error_stack_count - 1] : NULL;
    if (s->error_stack_count == 0 && dbg_engine_process_state(s->engine) == DBG_PROCESS_NONE)
        s->running = false;
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
                           ? s->config->emit_ir_command : "monad --emit-ir";
    DbgIrPanel *fresh = dbg_ir_panel_emit(emit_cmd, s->source->path, &err);
    if (fresh) { dbg_ir_panel_free(s->ir_panel); s->ir_panel = fresh; }
    if (err) dbg_console_appendf(s, "emit-ir: %s", err);
    free(err);
    s->dirty = true;
}

static bool g_dbg_commands_registered = false;

static void dbg_register_builtin_commands(void)
{
    if (g_dbg_commands_registered) return;
    g_dbg_commands_registered = true;
#define CMD(name_, keys_, summary_, fn_, arg_, caps_) \
    dbg_register_command((DbgCommand){ name_, keys_, summary_, fn_, arg_, caps_ })
    CMD("breakpoint-toggle", "bp break stop line", "Toggle a source breakpoint at point",
        cmd_toggle_breakpoint, false, 0);
    CMD("continue", "resume run c", "Continue execution", cmd_continue, false,
        DBG_CAP_EXECUTION_CONTROL);
    CMD("pause", "interrupt stop", "Interrupt execution", cmd_pause, false, DBG_CAP_PAUSE);
    CMD("step-in", "step into s", "Step into the next source operation", cmd_step_in, false,
        DBG_CAP_EXECUTION_CONTROL);
    CMD("step-over", "next over n", "Step over the next source operation", cmd_step_over, false,
        DBG_CAP_EXECUTION_CONTROL);
    CMD("step-out", "finish return out", "Step out of the selected frame", cmd_step_out, false,
        DBG_CAP_EXECUTION_CONTROL);
    CMD("step-expression", "semantic expression se", "Step into one Monad expression", cmd_step_expression, false,
        DBG_CAP_EXECUTION_CONTROL);
    CMD("next-expression", "semantic expression ne", "Step over one Monad expression", cmd_next_expression, false,
        DBG_CAP_EXECUTION_CONTROL);
    CMD("step-ir", "llvm ir si", "Step one LLVM IR instruction", cmd_step_ir, false,
        DBG_CAP_EXECUTION_CONTROL);
    CMD("next-ir", "llvm ir ni", "Step over one LLVM IR instruction", cmd_next_ir, false,
        DBG_CAP_EXECUTION_CONTROL);
    CMD("step-instruction", "si instruction asm", "Step one machine instruction", cmd_step_instruction,
        false, DBG_CAP_EXECUTION_CONTROL);
    CMD("next-instruction", "ni instruction asm", "Step over one machine instruction", cmd_next_instruction,
        false, DBG_CAP_EXECUTION_CONTROL);
    CMD("reverse-continue", "reverse rewind rc", "Continue execution backward", cmd_reverse_continue,
        false, DBG_CAP_EXECUTION_CONTROL | DBG_CAP_REVERSE_CONTINUE);
    CMD("step-back", "reverse previous rewind", "Step backward", cmd_step_back, false,
        DBG_CAP_EXECUTION_CONTROL | DBG_CAP_STEP_BACK);
    CMD("restart", "rerun relaunch", "Restart the debuggee", cmd_restart, false, DBG_CAP_RESTART);
    CMD("restart-frame", "frame rewind retry", "Restart execution from the selected frame",
        cmd_restart_frame, false, DBG_CAP_RESTART_FRAME);
    CMD("evaluate", "print eval expression p", "Evaluate a Monad expression in the selected frame",
        cmd_evaluate, true, DBG_CAP_EVALUATE);
    CMD("watch-add", "watch expression display", "Add an expression watch", cmd_watch_add, true,
        DBG_CAP_EVALUATE);
    CMD("checkpoint", "save replay time", "Create a replay checkpoint", cmd_checkpoint, false,
        DBG_CAP_CHECKPOINTS);
    CMD("quit", "exit close abort", "Quit the debugger", cmd_quit, false, 0);
    CMD("error-dismiss", "ok diagnostic close", "Dismiss the current compiler diagnostic",
        cmd_dismiss_error, false, 0);
    CMD("panel-source", "view goto switch code", "Focus the source panel", cmd_focus_source, false, 0);
    CMD("panel-ir", "view goto switch llvm", "Focus the LLVM IR panel", cmd_focus_ir, false, 0);
    CMD("panel-disassembly", "view goto switch asm machine-code", "Focus the disassembly panel",
        cmd_focus_disasm, false, 0);
    CMD("panel-locals", "view goto switch variables inspect", "Focus the locals panel",
        cmd_focus_locals, false, 0);
    CMD("panel-backtrace", "view goto switch stack frames trace", "Focus the backtrace panel",
        cmd_focus_backtrace, false, 0);
    CMD("panel-breakpoints", "view goto switch bp list", "Focus the breakpoints panel",
        cmd_focus_breakpoints, false, 0);
    CMD("ir-refresh", "reload re-emit llvm rebuild", "Re-run monad --emit-ir", cmd_reemit_ir, false, 0);
#undef CMD
}

void dbg_session_set_engine(DbgSession *s, DbgEngine *engine, bool take_ownership)
{
    if (!s) return;
    if (s->engine == engine) { s->owns_engine = take_ownership; return; }
    if (s->owns_engine) dbg_engine_free(s->engine);
    s->engine = engine ? engine : dbg_engine_create(NULL, NULL, false);
    s->owns_engine = engine ? take_ownership : true;
    dbg_session_clear_runtime_snapshots(s);
    if (s->palette) s->palette->capabilities = dbg_engine_capabilities(s->engine);
    s->dirty = true;
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
    s->input_state = dbg_input_state_create();
    s->theme = dbg_theme_default();
    s->keymap = dbg_keymap_create_default();
    dbg_blink_init(&s->blink, dbg_now_ms());
    s->vars = dbg_var_inspector_create();
    s->code_map = dbg_code_map_create();
    s->focused_panel = DBG_PANEL_SOURCE;
    s->last_stop_address = DBG_INVALID_ADDRESS;
    s->running = true;
    s->dirty = true;

    if (config.engine) {
        s->engine = config.engine;
        s->owns_engine = config.take_engine_ownership;
    } else {
        s->engine = dbg_engine_create(NULL, NULL, false);
        s->owns_engine = true;
    }

    dbg_compute_layout(s);
    return s;
}

void dbg_session_free(DbgSession *s)
{
    if (!s) return;
    dbg_disable_raw_mode(s);
    if (s->config) dbg_term_leave(s->config);

    dbg_input_state_free(s->input_state);
    dbg_screen_free(s->screen);
    dbg_theme_free(s->theme);
    dbg_keymap_free(s->keymap);
    dbg_source_map_free(s->source);
    dbg_ir_panel_free(s->ir_panel);
    dbg_var_inspector_free(s->vars);
    dbg_disasm_panel_free(s->disasm);
    dbg_palette_free(s->palette);
    dbg_code_map_free(s->code_map);
    dbg_thread_infos_free(s->threads, s->thread_count);
    dbg_stack_frame_infos_free(s->runtime_frames, s->runtime_frame_count);

    for (size_t i = 0; i < s->error_stack_count; i++)
        dbg_error_snapshot_free(s->error_stack[i]);
    free(s->error_stack);

    if (s->owns_engine) dbg_engine_free(s->engine);
    free(s->console_log);
    free(s->target_stdout_log);
    free(s->target_stderr_log);
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
static int dbg_source_gutter(void *ctx, int line_index, char *buf, size_t buflen)
{
    DbgSession *s = ctx;
    char mark = ' ';
    if (s && s->engine && s->source && line_index >= 0) {
        DbgBreakpoint *bp = dbg_breakpoint_at_line(s, s->source->path,
                                                   (uint32_t)line_index + 1);
        if (bp) mark = bp->verified ? '*' : '?';
    }
    return snprintf(buf, buflen, "%c%4d ", mark, line_index + 1);
}

static const char *dbg_process_state_label(DbgProcessState state)
{
    switch (state) {
    case DBG_PROCESS_NONE:      return "idle";
    case DBG_PROCESS_LAUNCHING: return "launching";
    case DBG_PROCESS_ATTACHING: return "attaching";
    case DBG_PROCESS_STOPPED:   return "stopped";
    case DBG_PROCESS_PARTIALLY_STOPPED: return "partially-stopped";
    case DBG_PROCESS_RUNNING:   return "running";
    case DBG_PROCESS_EXITED:    return "exited";
    case DBG_PROCESS_DETACHED:  return "detached";
    case DBG_PROCESS_CRASHED:   return "crashed";
    }
    return "?";
}

static const char *dbg_stop_reason_label(DbgStopReason reason)
{
    switch (reason) {
    case DBG_STOP_NONE:            return "";
    case DBG_STOP_ENTRY:           return "entry";
    case DBG_STOP_BREAKPOINT:      return "breakpoint";
    case DBG_STOP_DATA_BREAKPOINT: return "watchpoint";
    case DBG_STOP_STEP:            return "step";
    case DBG_STOP_PAUSE:           return "pause";
    case DBG_STOP_SIGNAL:          return "signal";
    case DBG_STOP_EXCEPTION:       return "exception";
    case DBG_STOP_GOTO:            return "goto";
    case DBG_STOP_REPLAY:          return "replay";
    case DBG_STOP_COMPILER:        return "compiler";
    case DBG_STOP_INTERNAL:        return "internal";
    }
    return "?";
}

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

static void dbg_render_disasm_panel(DbgSession *s)
{
    DbgRect r = s->panel_rects[DBG_PANEL_DISASM];
    dbg_screen_fill_rect(s->screen, r, ' ', s->theme->base);
    if (!s->disasm || s->disasm->line_count == 0) {
        dbg_screen_write(s->screen, r.row, r.col, "(no disassembly at current stop)",
                         s->theme->base);
        return;
    }
    for (int i = 0; i < r.height; i++) {
        int32_t idx = s->disasm->scroll_offset + i;
        if (idx < 0 || (size_t)idx >= s->disasm->line_count) continue;
        DbgDisasmLine *line = &s->disasm->lines[idx];
        DbgStrBuf b;
        sb_init(&b);
        sb_appendf(&b, "%c%c %016llx  %-18s %-10s %s",
                   line->is_current_pc ? '>' : ' ',
                   line->has_breakpoint ? '*' : ' ',
                   (unsigned long long)line->address,
                   line->bytes_hex ? line->bytes_hex : "",
                   line->mnemonic ? line->mnemonic : "",
                   line->operands ? line->operands : "");
        dbg_screen_write(s->screen, r.row + i, r.col, b.data,
                         line->is_current_pc ? s->theme->selection : s->theme->base);
        sb_free(&b);
    }
}

static char *dbg_runtime_frame_render(const DbgStackFrameInfo *f)
{
    DbgStrBuf b;
    sb_init(&b);
    if (f->is_async) sb_append(&b, "async ");
    if (f->is_inline) sb_append(&b, "inline ");
    sb_append(&b, f->name ? f->name : (f->location.function ? f->location.function : "<frame>"));
    if (f->location.source.file) {
        sb_appendf(&b, "  %s:%u:%u", f->location.source.file,
                   f->location.source.line, f->location.source.column);
    } else if (f->location.address != DBG_INVALID_ADDRESS) {
        sb_appendf(&b, "  @0x%llx", (unsigned long long)f->location.address);
    }
    if (f->location.ir_value) sb_appendf(&b, "  [%s]", f->location.ir_value);
    return sb_take(&b);
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
        tv.gutter_fn = dbg_source_gutter;
        tv.gutter_ctx = s;
        dbg_textview_render(s->screen, &tv, s->theme);
    }

    if (s->focused_panel == DBG_PANEL_DISASM)
        dbg_render_disasm_panel(s);
    else
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
    } else if (s->runtime_frame_count > 0) {
        char **lines = dbg_xmalloc(s->runtime_frame_count * sizeof(char *));
        for (size_t i = 0; i < s->runtime_frame_count; i++)
            lines[i] = dbg_runtime_frame_render(&s->runtime_frames[i]);
        DbgListView lv = {0};
        lv.items = lines;
        lv.item_count = s->runtime_frame_count;
        lv.selected = s->backtrace_selected;
        lv.scroll_offset = s->backtrace_scroll;
        lv.rect = s->panel_rects[DBG_PANEL_BACKTRACE];
        dbg_listview_render(s->screen, &lv, s->theme);
        for (size_t i = 0; i < s->runtime_frame_count; i++) free(lines[i]);
        free(lines);
    }

    size_t breakpoint_count = dbg_engine_breakpoint_count(s->engine);
    if (breakpoint_count > 0) {
        char **lines = dbg_xmalloc(breakpoint_count * sizeof(char *));
        for (size_t i = 0; i < breakpoint_count; i++) {
            const DbgBreakpoint *bp = dbg_engine_breakpoint_at_const(s->engine, i);
            DbgStrBuf b;
            sb_init(&b);
            sb_appendf(&b, "%s%s #%u ", bp->enabled ? "[x]" : "[ ]",
                       bp->verified ? "" : "?", bp->id);
            switch (bp->kind) {
            case DBG_BP_SOURCE:
                sb_appendf(&b, "%s:%u",
                           bp->as.source.location.file ? bp->as.source.location.file : "?",
                           bp->as.source.location.line);
                break;
            case DBG_BP_FUNCTION:
                sb_append(&b, bp->as.function.symbol ? bp->as.function.symbol : "<function>");
                break;
            case DBG_BP_IR_VALUE:
                if (bp->as.ir.function) sb_appendf(&b, "%s::", bp->as.ir.function);
                sb_append(&b, bp->as.ir.value ? bp->as.ir.value : "<ir>");
                break;
            case DBG_BP_INSTRUCTION:
                sb_appendf(&b, "0x%llx%+lld",
                           (unsigned long long)bp->as.instruction.address,
                           (long long)bp->as.instruction.offset);
                break;
            case DBG_BP_DATA:
                sb_appendf(&b, "data %s 0x%llx/%zu",
                           bp->as.data.data_id ? bp->as.data.data_id : "",
                           (unsigned long long)bp->as.data.address, bp->as.data.size);
                break;
            case DBG_BP_EXCEPTION:
                sb_append(&b, bp->as.exception.filter ? bp->as.exception.filter : "<exception>");
                break;
            }
            if (bp->condition) sb_appendf(&b, " if %s", bp->condition);
            if (bp->hit_condition) sb_appendf(&b, " hit:%s", bp->hit_condition);
            if (bp->log_message) sb_appendf(&b, " log:%s", bp->log_message);
            lines[i] = sb_take(&b);
            sb_free(&b);
        }
        DbgListView lv = {0};
        lv.items = lines;
        lv.item_count = breakpoint_count;
        lv.selected = s->breakpoints_selected;
        lv.scroll_offset = s->breakpoints_scroll;
        lv.rect = s->panel_rects[DBG_PANEL_BREAKPOINTS];
        dbg_listview_render(s->screen, &lv, s->theme);
        for (size_t i = 0; i < breakpoint_count; i++) free(lines[i]);
        free(lines);
    }

    dbg_render_console(s);
    dbg_render_status_line(s, s->screen->rows - (s->panel_rects[DBG_PANEL_CONSOLE].height) - 1
                            + s->panel_rects[DBG_PANEL_CONSOLE].height);

    if (s->palette_open) dbg_palette_render(s);

    /* Logical caret follows the focused view; it is not hard-coded to 0,0. */
    if (s->blink.visible) {
        DbgPoint caret = { -1, -1 };
        if (s->palette_open && s->palette) {
            size_t prefix = 4; /* "M-x " */
            if (s->palette->argument_mode && s->palette->pending_command)
                prefix = strlen(s->palette->pending_command->name) + 1;
            caret.row = (int16_t)s->palette->rect.row;
            caret.col = (int16_t)(s->palette->rect.col + (int)prefix +
                                   (int)s->palette->cursor_pos);
        } else if (s->focused_panel == DBG_PANEL_SOURCE) {
            DbgRect r = s->panel_rects[DBG_PANEL_SOURCE];
            int row = r.row + (s->source_cursor_line - s->source_scroll);
            if (row >= r.row && row < r.row + r.height) {
                caret.row = (int16_t)row;
                caret.col = (int16_t)(r.col + 6);
            }
        } else if (s->focused_panel == DBG_PANEL_IR && s->ir_panel) {
            DbgRect r = s->panel_rects[DBG_PANEL_IR];
            int row = r.row + (s->ir_panel->cursor_line - s->ir_panel->scroll_line);
            if (row >= r.row && row < r.row + r.height) {
                caret.row = (int16_t)row;
                caret.col = (int16_t)r.col;
            }
        }
        if (caret.row >= 0 && caret.col >= 0) {
            size_t idx = (size_t)caret.row * (size_t)s->screen->cols + (size_t)caret.col;
            if (idx < (size_t)s->screen->cols * (size_t)s->screen->rows) {
                DbgCell c = s->screen->back[idx];
                c.style.attrs ^= DBG_ATTR_REVERSE;
                dbg_screen_assign(s->screen, caret.row, caret.col, c);
            }
        }
    }

    dbg_screen_flush(s->screen, s->caps.truecolor);
    s->dirty = false;
}


/// §28  Event loop / dispatcher

//// Input polling
 //
 //  poll() waits on terminal/backend fds with a timeout equal to the remaining time
 //  until the next blink toggle (or the frame period, whichever is
 //  sooner), so the process is fully asleep between events instead of
 //  busy-polling — important for "performant" when the debugger may sit
 //  open for minutes while the user reads a backtrace.
 //
static size_t dbg_utf8_sequence_len(unsigned char lead)
{
    if (lead < 0x80u) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 1;
}

static bool dbg_ring_has_csi_final(const DbgByteRing *ring)
{
    /* ESC [ is at offsets 0..1. CSI final bytes occupy 0x40..0x7e. */
    for (size_t i = 2; i < ring_count(ring); i++) {
        int c = ring_peek_n(ring, i);
        if (c >= 0x40 && c <= 0x7e) return true;
    }
    return false;
}

static void dbg_emit_paste_event(DbgInputState *in, DbgEventQueue *q)
{
    DbgEvent ev = {0};
    ev.kind = DBG_EVENT_PASTE;
    ev.as.paste_text = sb_take(&in->paste);
    if (!ev.as.paste_text) ev.as.paste_text = dbg_xstrdup("");
    eventq_push(q, ev);
}

static bool dbg_decode_paste(DbgInputState *in, DbgEventQueue *q)
{
    static const unsigned char end_seq[] = { 0x1b, '[', '2', '0', '1', '~' };
    DbgByteRing *ring = &in->ring;
    while (!ring_empty(ring)) {
        if (ring_peek_n(ring, 0) == 0x1b) {
            if (ring_prefix(ring, end_seq, sizeof(end_seq))) {
                ring_consume(ring, sizeof(end_seq));
                in->paste_mode = false;
                dbg_emit_paste_event(in, q);
                return true;
            }
            if (ring_count(ring) < sizeof(end_seq) &&
                ring_prefix_available(ring, end_seq, sizeof(end_seq)))
                return false; /* split end marker; wait for more bytes */
        }
        int c = ring_pop(ring);
        if (c == -1) break;
        sb_appendc(&in->paste, (char)c);
    }
    return false;
}

static void dbg_decode_pending_bytes(DbgInputState *in, DbgEventQueue *q,
                                     uint64_t now_ms)
{
    if (!in) return;
    DbgByteRing *ring = &in->ring;
    static const unsigned char paste_begin[] = { 0x1b, '[', '2', '0', '0', '~' };

    for (;;) {
        if (in->paste_mode) {
            if (!dbg_decode_paste(in, q)) break;
            continue;
        }
        if (ring_empty(ring)) break;

        int first = ring_peek_n(ring, 0);
        if (first == 0x1b) {
            if (ring_count(ring) == 1) {
                if (!in->esc_pending_since_ms) in->esc_pending_since_ms = now_ms;
                if (now_ms - in->esc_pending_since_ms < DBG_ESC_TIMEOUT_MS) break;
                (void)ring_pop(ring);
                in->esc_pending_since_ms = 0;
                DbgEvent ev = {0}; ev.kind = DBG_EVENT_KEY; ev.as.key.sym = DBG_KEY_ESCAPE;
                eventq_push(q, ev);
                continue;
            }
            in->esc_pending_since_ms = 0;
            int second = ring_peek_n(ring, 1);
            if (second == '[') {
                if (ring_count(ring) < sizeof(paste_begin) &&
                    ring_prefix_available(ring, paste_begin, sizeof(paste_begin)))
                    break;
                if (ring_prefix(ring, paste_begin, sizeof(paste_begin))) {
                    ring_consume(ring, sizeof(paste_begin));
                    in->paste_mode = true;
                    in->paste.len = 0;
                    if (in->paste.data) in->paste.data[0] = '\0';
                    continue;
                }
                if (!dbg_ring_has_csi_final(ring)) break;
                ring_consume(ring, 2); /* ESC [ */
                int peek = ring_pop(ring);
                if (peek == '<') {
                    DbgMouseEvent me = {0};
                    if (decode_sgr_mouse(ring, &me)) {
                        DbgEvent ev = {0}; ev.kind = DBG_EVENT_MOUSE; ev.as.mouse = me;
                        eventq_push(q, ev);
                    }
                } else {
                    DbgKeyEvent ke = {0};
                    if (decode_csi_key(ring, peek, &ke)) {
                        DbgEvent ev = {0}; ev.kind = DBG_EVENT_KEY; ev.as.key = ke;
                        eventq_push(q, ev);
                    }
                }
                continue;
            }
            if (second == 'O') {
                if (ring_count(ring) < 3) break;
                ring_consume(ring, 2);
                DbgKeyEvent ke = {0};
                if (decode_ss3_key(ring, &ke)) {
                    DbgEvent ev = {0}; ev.kind = DBG_EVENT_KEY; ev.as.key = ke;
                    eventq_push(q, ev);
                }
                continue;
            }

            /* ESC + UTF-8 code point is Meta+character. */
            unsigned char lead = (unsigned char)second;
            size_t need = dbg_utf8_sequence_len(lead);
            if (ring_count(ring) < need + 1) break;
            (void)ring_pop(ring); /* ESC */
            (void)ring_pop(ring); /* lead */
            uint32_t cp = decode_utf8_codepoint(ring, lead);
            DbgEvent ev = {0}; ev.kind = DBG_EVENT_KEY;
            ev.as.key.sym = DBG_KEY_CHAR; ev.as.key.codepoint = cp; ev.as.key.meta = true;
            eventq_push(q, ev);
            continue;
        }

        if (first == '\r' || first == '\n') {
            (void)ring_pop(ring);
            DbgEvent ev = {0}; ev.kind = DBG_EVENT_KEY; ev.as.key.sym = DBG_KEY_ENTER;
            eventq_push(q, ev); continue;
        }
        if (first == 0x7f || first == 0x08) {
            (void)ring_pop(ring);
            DbgEvent ev = {0}; ev.kind = DBG_EVENT_KEY; ev.as.key.sym = DBG_KEY_BACKSPACE;
            eventq_push(q, ev); continue;
        }
        if (first == '\t') {
            (void)ring_pop(ring);
            DbgEvent ev = {0}; ev.kind = DBG_EVENT_KEY; ev.as.key.sym = DBG_KEY_TAB;
            eventq_push(q, ev); continue;
        }
        if (first >= 1 && first <= 26 && first != 9 && first != 13) {
            (void)ring_pop(ring);
            DbgEvent ev = {0}; ev.kind = DBG_EVENT_KEY; ev.as.key.sym = DBG_KEY_CHAR;
            ev.as.key.codepoint = (uint32_t)(first - 1 + 'a'); ev.as.key.ctrl = true;
            eventq_push(q, ev); continue;
        }

        unsigned char lead = (unsigned char)first;
        size_t need = dbg_utf8_sequence_len(lead);
        if (ring_count(ring) < need) break;
        (void)ring_pop(ring);
        uint32_t cp = decode_utf8_codepoint(ring, lead);
        DbgEvent ev = {0}; ev.kind = DBG_EVENT_KEY; ev.as.key.sym = DBG_KEY_CHAR;
        ev.as.key.codepoint = cp; eventq_push(q, ev);
    }
}

static void dbg_push_resize_if_needed(DbgEventQueue *q)
{
    if (!g_dbg_winch_pending) return;
    g_dbg_winch_pending = 0;
    int cols = 0, rows = 0;
    dbg_query_winsize(&cols, &rows);
    DbgEvent ev = {0};
    ev.kind = DBG_EVENT_RESIZE;
    ev.as.resize.cols = cols;
    ev.as.resize.rows = rows;
    eventq_push(q, ev);
}

static void dbg_poll_input(DbgSession *s, DbgEventQueue *q, uint64_t timeout_ms)
{
    DbgInputState *in = s ? s->input_state : NULL;
    int engine_fd = s && s->engine ? dbg_engine_event_fd(s->engine) : -1;

    if (in && in->esc_pending_since_ms) {
        uint64_t now = dbg_now_ms();
        uint64_t elapsed = now - in->esc_pending_since_ms;
        uint64_t remain = elapsed >= DBG_ESC_TIMEOUT_MS ? 0 : DBG_ESC_TIMEOUT_MS - elapsed;
        if (timeout_ms > remain) timeout_ms = remain;
    }

    struct pollfd fds[2];
    nfds_t count = 0;
    fds[count++] = (struct pollfd){ .fd = STDIN_FILENO, .events = POLLIN };
    if (engine_fd >= 0 && engine_fd != STDIN_FILENO)
        fds[count++] = (struct pollfd){ .fd = engine_fd, .events = POLLIN };

    int timeout = timeout_ms > (uint64_t)INT_MAX ? INT_MAX : (int)timeout_ms;
    int rc;
    do {
        rc = poll(fds, count, timeout);
    } while (rc < 0 && errno == EINTR && !g_dbg_winch_pending);

    if (rc > 0 && (fds[0].revents & POLLIN) && in)
        ring_fill_nonblocking(&in->ring);

    dbg_push_resize_if_needed(q);
    if (in) dbg_decode_pending_bytes(in, q, dbg_now_ms());
    /* Backend readiness is intentionally consumed by dbg_session_pump_engine()
       after terminal events. poll() removes FD_SETSIZE as a backend-transport
       limit while keeping pipes, sockets and eventfd equally usable. */
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
        case DBG_KEY_LEFT:   dbg_palette_move_cursor(p, -1); return;
        case DBG_KEY_RIGHT:  dbg_palette_move_cursor(p, 1); return;
        case DBG_KEY_HOME:   p->cursor_pos = 0; return;
        case DBG_KEY_END:    p->cursor_pos = p->input_len; return;
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
    if (key->sym == DBG_KEY_F5)  { cmd_continue(s, NULL); return; }
    if (key->sym == DBG_KEY_F9)  { cmd_toggle_breakpoint(s, NULL); return; }
    if (key->sym == DBG_KEY_F10) { cmd_step_over(s, NULL); return; }
    if (key->sym == DBG_KEY_F11) {
        if (key->shift) cmd_step_out(s, NULL); else cmd_step_in(s, NULL);
        return;
    }
    if (key->sym == DBG_KEY_CHAR && key->ctrl && key->codepoint == 'c') {
        cmd_pause(s, NULL);
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
        DbgVarEntry **rows = NULL;
        size_t row_count = 0, row_cap = 0;
        if (s->vars) {
            for (size_t i = 0; i < s->vars->local_count; i++)
                dbg_var_collect_entries(s->vars->locals[i], &rows, &row_count, &row_cap);
        }
        DbgListView lv = {0};
        lv.rect = s->panel_rects[DBG_PANEL_LOCALS];
        lv.item_count = row_count;
        lv.selected = s->locals_selected;
        lv.scroll_offset = s->locals_scroll;
        bool activated = false;
        if (dbg_listview_handle_mouse(&lv, ev, &activated)) {
            s->locals_selected = lv.selected;
            s->locals_scroll = lv.scroll_offset;
            if (activated && lv.selected >= 0 && (size_t)lv.selected < row_count) {
                DbgVarEntry *entry = rows[lv.selected];
                bool has_children = entry->child_count > 0 ||
                                    entry->value.variables_reference != 0 ||
                                    entry->value.named_children > 0 ||
                                    entry->value.indexed_children > 0;
                if (has_children && !entry->expanded && entry->child_count == 0)
                    (void)dbg_var_entry_load_children(s, entry);
                if (has_children) entry->expanded = !entry->expanded;
            }
        }
        free(rows);
        break;
    }
    case DBG_PANEL_BACKTRACE: {
        DbgListView lv = {0};
        lv.rect = s->panel_rects[DBG_PANEL_BACKTRACE];
        lv.item_count = s->active_error ? s->active_error->backtrace.frame_count
                                        : s->runtime_frame_count;
        lv.selected = s->backtrace_selected;
        lv.scroll_offset = s->backtrace_scroll;
        bool activated = false;
        if (dbg_listview_handle_mouse(&lv, ev, &activated)) {
            s->backtrace_selected = lv.selected;
            s->backtrace_scroll = lv.scroll_offset;
            if (activated && !s->active_error && lv.selected >= 0 &&
                (size_t)lv.selected < s->runtime_frame_count) {
                DbgStackFrameInfo *f = &s->runtime_frames[lv.selected];
                dbg_session_refresh_frame(s, f);
            }
        }
        break;
    }
    case DBG_PANEL_BREAKPOINTS: {
        DbgListView lv = {0};
        lv.rect = s->panel_rects[DBG_PANEL_BREAKPOINTS];
        lv.item_count = dbg_engine_breakpoint_count(s->engine);
        lv.selected = s->breakpoints_selected;
        lv.scroll_offset = s->breakpoints_scroll;
        bool activated = false;
        if (dbg_listview_handle_mouse(&lv, ev, &activated)) {
            s->breakpoints_selected = lv.selected;
            s->breakpoints_scroll = lv.scroll_offset;
            if (activated && lv.selected >= 0 &&
                (size_t)lv.selected < dbg_engine_breakpoint_count(s->engine)) {
                DbgBreakpoint *bp = dbg_engine_breakpoint_at(s->engine, (size_t)lv.selected);
                char *err = NULL;
                if (bp && !dbg_engine_set_breakpoint_enabled(s->engine, bp->id,
                                                              !bp->enabled, &err) && err)
                    dbg_console_appendf(s, "breakpoint: %s", err);
                free(err);
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
        if (s->disasm) {
            if (ev->kind == DBG_MOUSE_WHEEL_UP) {
                s->disasm->scroll_offset -= DBG_WHEEL_LINES;
                if (s->disasm->scroll_offset < 0) s->disasm->scroll_offset = 0;
            } else if (ev->kind == DBG_MOUSE_WHEEL_DOWN) {
                s->disasm->scroll_offset += DBG_WHEEL_LINES;
            }
        }
        break;
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

        dbg_poll_input(s, &q, wait_ms);

        DbgEvent ev;
        while (eventq_pop(&q, &ev)) {
            dbg_dispatch_event(s, &ev);
            dbg_event_free(&ev);
        }

        dbg_session_pump_engine(s);

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
static DbgKeymap *dbg_keymap_create_default(void)
{
    DbgKeymap *km = dbg_xcalloc(1, sizeof(*km));
    km->cap = 16;
    km->bindings = dbg_xmalloc(km->cap * sizeof(DbgKeyBinding));
    km->count = 0;
    return km;
}

static void dbg_keymap_free(DbgKeymap *km)
{
    if (!km) return;
    free(km->bindings);
    free(km);
}



/// §30  Theme / color palette

//// Default theme
 //
 //  A dark, high-contrast palette chosen for long debugging sessions:
 //  desaturated background, saturated accent colors reserved for things
 //  that matter (errors, the active selection, IR keywords) so the eye
 //  is drawn to them rather than fighting noise everywhere.
 //
static DbgTheme *dbg_theme_default(void)
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

static void dbg_theme_free(DbgTheme *theme)
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
