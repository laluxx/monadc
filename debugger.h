/// debugger.h — TUI debugger / error-trap interface for Monad
//
//  Public contract for dbg.c.  Any compiler stage (parser, type checker,
//  the `error` builtin, the LLVM lowering pass) can call dbg_trap_error()
//  at the moment a diagnostic becomes fatal; the debugger takes over the
//  terminal, shows the original message plus full context, and returns
//  control (or exits) based on what the user does inside the TUI.
//
//  Design goals, in priority order:
//
//    1. Zero allocation on the hot "no error occurred" path.
//    2. A redraw is O(cells changed), never O(cells on screen).
//    3. The cursor blinks exactly like Emacs: 0.5s period, resets on any
//       keystroke, stops blinking (goes solid) after 10 blinks of idle
//       time, identical to `blink-cursor-blinks` = 10 in Emacs core.
//    4. Every public result type has exactly one matching free function.
//    5. No ncurses / terminfo library dependency — we speak raw ANSI/SGR
//       and parse our own input escape sequences, so the binary stays
//       link-compatible with the rest of the freestanding-ish toolchain.
//
//  Source layout (see debugger.c):
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

#ifndef MONAD_DEBUGGER_H
#define MONAD_DEBUGGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <termios.h>

#ifdef __cplusplus
extern "C" {
#endif


/// §1  Includes and internal constants

#define DBG_GROW_FACTOR          2
#define DBG_INITIAL_BUF          4096
#define DBG_MAX_COLS             1024
#define DBG_MAX_ROWS             512
#define DBG_BLINK_PERIOD_MS      500   /* Emacs blink-cursor-interval default */
#define DBG_BLINK_MAX_COUNT      10    /* Emacs blink-cursor-blinks default   */
#define DBG_PALETTE_MAX_RESULTS  256
#define DBG_MAX_BREAKPOINTS      512
#define DBG_MAX_FRAMES           1024
#define DBG_MAX_PANELS           16
#define DBG_DOUBLE_CLICK_MS      350


/// Forward declarations (opaque handles; definitions live in debugger.c)

typedef struct DbgScreen        DbgScreen;
typedef struct DbgCell          DbgCell;
typedef struct DbgRect          DbgRect;
typedef struct DbgEvent         DbgEvent;
typedef struct DbgPane          DbgPane;
typedef struct DbgTextView      DbgTextView;
typedef struct DbgListView      DbgListView;
typedef struct DbgPalette       DbgPalette;
typedef struct DbgCommand       DbgCommand;
typedef struct DbgErrorSnapshot DbgErrorSnapshot;
typedef struct DbgFrame         DbgFrame;
typedef struct DbgBacktrace     DbgBacktrace;
typedef struct DbgBreakpoint    DbgBreakpoint;
typedef struct DbgWatch         DbgWatch;
typedef struct DbgSourceMap     DbgSourceMap;
typedef struct DbgIrPanel       DbgIrPanel;
typedef struct DbgVarInspector  DbgVarInspector;
typedef struct DbgDisasmPanel   DbgDisasmPanel;
typedef struct DbgSession       DbgSession;
typedef struct DbgKeymap        DbgKeymap;
typedef struct DbgKeyBinding    DbgKeyBinding;
typedef struct DbgTheme         DbgTheme;
typedef struct DbgConfig        DbgConfig;


/// §2 / §3 — Geometry and color primitives exposed to callers

typedef struct {
    int16_t row;
    int16_t col;
} DbgPoint;

struct DbgRect {
    int16_t row, col;      /* top-left, 0-indexed                       */
    int16_t height, width;
};

/* 24-bit truecolor with graceful 256-color degradation decided at
   render time based on detected terminal capability.                  */
typedef struct {
    uint8_t r, g, b;
} DbgColor;

typedef enum {
    DBG_ATTR_NONE      = 0,
    DBG_ATTR_BOLD      = 1 << 0,
    DBG_ATTR_DIM       = 1 << 1,
    DBG_ATTR_ITALIC    = 1 << 2,
    DBG_ATTR_UNDERLINE = 1 << 3,
    DBG_ATTR_REVERSE   = 1 << 4,
    DBG_ATTR_STRIKE    = 1 << 5,
} DbgAttr;

typedef struct {
    DbgColor fg;
    DbgColor bg;
    uint8_t  attrs;   /* bitmask of DbgAttr */
} DbgStyle;


/// §4 — Terminal capability flags, detected once at startup

typedef struct {
    bool truecolor;
    bool mouse_sgr;
    bool bracketed_paste;
    bool kitty_keyboard;
    int  cols;
    int  rows;
} DbgTermCaps;


/// §7 / §8 — Input events

typedef enum {
    DBG_EVENT_NONE = 0,
    DBG_EVENT_KEY,
    DBG_EVENT_MOUSE,
    DBG_EVENT_RESIZE,
    DBG_EVENT_TICK,        /* periodic timer tick, drives cursor blink  */
    DBG_EVENT_PASTE,
    DBG_EVENT_QUIT,
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

typedef struct {
    DbgKeySym sym;
    uint32_t  codepoint;   /* valid when sym == DBG_KEY_CHAR, UTF-32     */
    bool      ctrl, meta, shift;
} DbgKeyEvent;

typedef enum {
    DBG_MOUSE_MOVE = 0,
    DBG_MOUSE_DOWN,
    DBG_MOUSE_UP,
    DBG_MOUSE_DRAG,
    DBG_MOUSE_WHEEL_UP,
    DBG_MOUSE_WHEEL_DOWN,
} DbgMouseKind;

typedef enum {
    DBG_BTN_NONE = 0, DBG_BTN_LEFT, DBG_BTN_MIDDLE, DBG_BTN_RIGHT,
} DbgMouseButton;

typedef struct {
    DbgMouseKind   kind;
    DbgMouseButton button;
    int16_t        row, col;
    bool           ctrl, meta, shift;
} DbgMouseEvent;

struct DbgEvent {
    DbgEventKind kind;
    union {
        DbgKeyEvent   key;
        DbgMouseEvent mouse;
        struct { int cols, rows; } resize;
        char         *paste_text;   /* owned; freed by dbg_event_free */
    } as;
};

void dbg_event_free(DbgEvent *ev);


/// §10 — Cursor blink state, observable for status-line debugging

typedef struct {
    bool     visible;
    bool     solid;          /* true once DBG_BLINK_MAX_COUNT reached    */
    uint32_t blink_count;
    uint64_t last_toggle_ms;
    uint64_t last_activity_ms;
} DbgCursorBlink;


/// §15 — Orderless completion: one match = which spans of the candidate
//        each input token hit, used to render highlighted completions.

typedef struct {
    uint32_t start, len;
} DbgMatchSpan;

typedef struct {
    const char   *candidate;   /* borrowed pointer, owned by source       */
    void         *user_data;   /* opaque payload (DbgCommand*, file, …)   */
    double        score;
    DbgMatchSpan *spans;
    size_t        span_count;
} DbgCompletionResult;


/// §17 — Command registry entry. Every palette action is one of these.

typedef void (*DbgCommandFn)(DbgSession *session, const char *arg);

struct DbgCommand {
    const char   *name;          /* e.g. "breakpoint-toggle"              */
    const char   *keywords;      /* extra space-separated search terms    */
    const char   *summary;       /* one-line help shown in palette        */
    DbgCommandFn  run;
    bool          needs_arg;
};


/// §18 / §19 — Error snapshot and backtrace, the data captured at the
//              moment an error fires, before the TUI takes the screen.

typedef enum {
    DBG_SEV_NOTE = 0,
    DBG_SEV_WARNING,
    DBG_SEV_ERROR,
    DBG_SEV_FATAL,
    DBG_SEV_INTERNAL,    /* compiler-internal invariant violation        */
} DbgSeverity;

struct DbgFrame {
    char    *function;
    char    *file;
    uint32_t line;
    uint32_t column;
    char    *ir_value;     /* nearest LLVM IR value name, if known, else NULL */
};

struct DbgBacktrace {
    DbgFrame *frames;
    size_t    frame_count;
};

struct DbgErrorSnapshot {
    DbgSeverity   severity;
    char         *original_message;   /* verbatim, exactly as raised      */
    char         *code;               /* diagnostic code, e.g. "E0423"    */
    char         *file;
    uint32_t      line, column;
    DbgBacktrace  backtrace;
    char         *source_context;     /* a few lines around the error     */
    uint64_t      captured_at_ms;
};

DbgErrorSnapshot *dbg_error_snapshot_create(DbgSeverity severity,
                                             const char *message,
                                             const char *code,
                                             const char *file,
                                             uint32_t line, uint32_t column);
void dbg_error_snapshot_free(DbgErrorSnapshot *snap);

/* The hook every error-producing site calls.  If a TUI session is not
   already active, this takes over the terminal, renders the snapshot,
   and blocks until the user dismisses it or quits.  Safe to call
   re-entrantly (nested errors stack onto the same backtrace pane).      */
void dbg_trap_error(DbgErrorSnapshot *snap);

/* Convenience wrapper matching the shape of the `error` builtin:
   formats like printf, captures the call site, and traps.               */
void dbg_trap_errorf(DbgSeverity severity, const char *file,
                     uint32_t line, uint32_t column,
                     const char *code, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));


/// §20 — Breakpoints / watchpoints

typedef enum {
    DBG_BP_LINE = 0,
    DBG_BP_FUNCTION,
    DBG_BP_IR_VALUE,
} DbgBreakpointKind;

struct DbgBreakpoint {
    uint32_t          id;
    DbgBreakpointKind kind;
    char             *file;       /* for DBG_BP_LINE                     */
    uint32_t          line;
    char             *symbol;     /* for DBG_BP_FUNCTION / DBG_BP_IR_VALUE */
    bool              enabled;
    char             *condition;  /* optional expression, NULL if none   */
    uint64_t          hit_count;
};

struct DbgWatch {
    uint32_t id;
    char    *expression;
    char    *last_value;     /* cached rendering, refreshed on each stop */
    bool     changed_last_step;
};


/// §21 — Source map: byte offset / line table feeding the source viewport

struct DbgSourceMap {
    char        *path;
    char        *contents;
    size_t       contents_len;
    uint32_t    *line_offsets;
    const char **lines;
    uint32_t     line_count;
};

DbgSourceMap *dbg_source_map_load(const char *path);
void dbg_source_map_free(DbgSourceMap *map);


/// §22 — LLVM IR panel: holds the textual IR plus simple highlight spans

typedef enum {
    DBG_IR_TOK_PLAIN = 0,
    DBG_IR_TOK_KEYWORD,     /* define, declare, br, ret, call, …          */
    DBG_IR_TOK_TYPE,        /* i32, ptr, float, %struct.Foo, …            */
    DBG_IR_TOK_GLOBAL,      /* @name                                     */
    DBG_IR_TOK_LOCAL,       /* %name                                     */
    DBG_IR_TOK_LITERAL,     /* numeric / string constants                */
    DBG_IR_TOK_COMMENT,     /* ; …                                       */
    DBG_IR_TOK_LABEL,       /* basic block labels                        */
} DbgIrTokenKind;

typedef struct {
    uint32_t       start, len;   /* byte span within the IR text          */
    DbgIrTokenKind kind;
} DbgIrToken;

struct DbgIrPanel {
    char       *ir_text;        /* owned; output of `monad --emit-ir`     */
    size_t      ir_len;
    DbgIrToken *tokens;
    size_t      token_count;
    uint32_t   *line_offsets;
    uint32_t    line_count;
    int32_t     cursor_line;    /* viewport state, persists across redraws */
    int32_t     scroll_line;
    char       *highlighted_value;  /* %value or @global under cursor, NULL if none */
};

/* Invokes `monad --emit-ir <path>` (or equivalent in-process emit path),
   captures stdout, and tokenizes it for syntax highlighting.  Returns
   NULL (and leaves *error_out set) if the subprocess could not run.      */
DbgIrPanel *dbg_ir_panel_emit(const char *emit_ir_command,
                              const char *source_path,
                              char **error_out);
void dbg_ir_panel_free(DbgIrPanel *panel);


/// §23 — Variable / register inspector model

typedef enum {
    DBG_VAL_SCALAR = 0,
    DBG_VAL_AGGREGATE,
    DBG_VAL_POINTER,
    DBG_VAL_FUNCTION,
} DbgValueKind;

typedef struct DbgVarEntry {
    char               *name;
    char               *type_sig;
    char               *rendered_value;
    DbgValueKind        kind;
    bool                expanded;     /* aggregate/pointer expand state   */
    struct DbgVarEntry **children;
    size_t              child_count;
} DbgVarEntry;

struct DbgVarInspector {
    DbgVarEntry **locals;
    size_t        local_count;
    DbgVarEntry **globals;
    size_t        global_count;
    int32_t       selected_index;
    int32_t       scroll_offset;
};


/// §24 — Disassembly panel (machine code view, address-indexed)

typedef struct {
    uint64_t address;
    char    *bytes_hex;
    char    *mnemonic;
    char    *operands;
    bool     is_current_pc;
    bool     has_breakpoint;
} DbgDisasmLine;

struct DbgDisasmPanel {
    DbgDisasmLine *lines;
    size_t         line_count;
    int32_t        scroll_offset;
};


/// §25 — Session: the live debuggee + all panel state bundled together

typedef enum {
    DBG_PANEL_SOURCE = 0,
    DBG_PANEL_IR,
    DBG_PANEL_DISASM,
    DBG_PANEL_LOCALS,
    DBG_PANEL_BACKTRACE,
    DBG_PANEL_BREAKPOINTS,
    DBG_PANEL_CONSOLE,
} DbgPanelKind;

struct DbgSession {
    DbgScreen       *screen;
    DbgTermCaps      caps;
    struct termios   saved_termios;
    bool             raw_mode_active;

    DbgCursorBlink   blink;
    DbgKeymap       *keymap;
    DbgTheme        *theme;
    DbgConfig       *config;

    /* Active error context, may be NULL if launched standalone           */
    DbgErrorSnapshot *active_error;
    DbgErrorSnapshot **error_stack;   /* nested traps                     */
    size_t             error_stack_count;
    size_t             error_stack_cap;

    DbgSourceMap     *source;
    DbgIrPanel       *ir_panel;
    DbgVarInspector  *vars;
    DbgDisasmPanel   *disasm;

    DbgBreakpoint    *breakpoints;
    size_t            breakpoint_count;
    size_t            breakpoint_cap;
    uint32_t          next_breakpoint_id;

    DbgWatch         *watches;
    size_t            watch_count;
    size_t            watch_cap;

    DbgPanelKind      focused_panel;
    DbgRect           panel_rects[DBG_MAX_PANELS];

    /* Persistent scroll/selection state per panel, read by the §27
       render pass and written by the §28 mouse/keyboard dispatch — this
       is what lets dbg_textview_handle_mouse / dbg_listview_handle_mouse
       actually have a durable target instead of a throwaway per-frame
       local, so a scroll-wheel notch is remembered across redraws.      */
    int32_t           source_scroll;
    int32_t           source_cursor_line;
    int32_t           locals_scroll;
    int32_t           locals_selected;
    int32_t           backtrace_scroll;
    int32_t           backtrace_selected;
    int32_t           breakpoints_scroll;
    int32_t           breakpoints_selected;
    int32_t           console_scroll;

    DbgPalette       *palette;        /* NULL unless palette is open      */
    bool              palette_open;

    char             *console_log;    /* scrollback for status/log pane   */
    size_t            console_log_len;
    size_t            console_log_cap;

    bool              running;
    bool              dirty;          /* true if any state changed this
                                          tick and a redraw is needed     */
};


/// §29 — Keymap

typedef void (*DbgActionFn)(DbgSession *session);

struct DbgKeyBinding {
    DbgKeySym   sym;
    uint32_t    codepoint;   /* for DBG_KEY_CHAR bindings, 0 otherwise    */
    bool        ctrl, meta, shift;
    DbgActionFn action;
    const char *description;
};

struct DbgKeymap {
    DbgKeyBinding *bindings;
    size_t         count;
    size_t         cap;
};

DbgKeymap *dbg_keymap_create_default(void);
void dbg_keymap_free(DbgKeymap *km);
void dbg_keymap_bind(DbgKeymap *km, DbgKeyBinding binding);


/// §30 — Theme

struct DbgTheme {
    DbgStyle base;
    DbgStyle status_line;
    DbgStyle cursor;
    DbgStyle selection;
    DbgStyle error_banner;
    DbgStyle warning_banner;
    DbgStyle gutter;
    DbgStyle gutter_breakpoint;
    DbgStyle ir_keyword;
    DbgStyle ir_type;
    DbgStyle ir_global;
    DbgStyle ir_local;
    DbgStyle ir_literal;
    DbgStyle ir_comment;
    DbgStyle palette_match;
    DbgStyle palette_border;
};

DbgTheme *dbg_theme_default(void);
void dbg_theme_free(DbgTheme *theme);


/// §32  Public entry points

struct DbgConfig {
    bool     mouse_enabled;
    bool     truecolor_force;     /* override autodetect                 */
    uint32_t blink_period_ms;     /* default DBG_BLINK_PERIOD_MS          */
    uint32_t blink_max_count;     /* default DBG_BLINK_MAX_COUNT          */
    uint32_t target_fps;          /* render loop tick rate, default 60    */
    const char *emit_ir_command;  /* default "monad --emit-ir"            */
};

DbgConfig dbg_default_config(void);

/* Create a session bound to stdin/stdout, entering raw mode, alternate
   screen, and mouse reporting. Does not block.                          */
DbgSession *dbg_session_create(DbgConfig config);

/* Tears down: restores cursor, leaves alternate screen, restores
   termios, frees all owned state.                                       */
void dbg_session_free(DbgSession *session);

/* Runs the full event loop until the user quits. Returns an exit code
   suitable for main() — 0 on clean quit, nonzero if the active error
   was severity DBG_SEV_FATAL or DBG_SEV_INTERNAL at exit time.          */
int dbg_session_run(DbgSession *session);

/* Standalone entry point: `monad --debug <file>` wires here.            */
int dbg_main(int argc, char **argv);
int dbg_main_with_config(int argc, char **argv, DbgConfig config);

#ifdef __cplusplus
}
#endif

#endif /* MONAD_DEBUGGER_H */
