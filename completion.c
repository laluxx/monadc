#include "completion.h"

#if defined(_WIN32)

#include <stdio.h>

void print_usage(const char *prog)
{
    printf("Usage: %s <command> [options]\n", prog ? prog : "monad");
}

void print_subcommand_menu(const char *subcmd)
{
    printf("Monad %s\n", subcmd ? subcmd : "commands");
}

int completion_menu_main(const char *prog)
{
    print_usage(prog);
    return 0;
}

#else

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RESET   "\x1b[0m"
#define BOLD    "\x1b[1m"
#define DIM     "\x1b[2m"
#define REV     "\x1b[7m"
#define CYAN    "\x1b[36m"
#define YELLOW  "\x1b[33m"
#define MAGENTA "\x1b[35m"
#define GREEN   "\x1b[32m"
#define RED     "\x1b[31m"
#define WHITE   "\x1b[97m"
#define BG      "\x1b[48;5;236m"
#define BG_HI   "\x1b[48;5;238m"

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define QUERY_CAP 256
#define KILL_CAP 256
#define MAX_RESULTS 256
#define CURSOR_DELAY_MS 500
#define CURSOR_BLINK_MS 500

typedef enum {
    ENTRY_COMMAND,
    ENTRY_FLAG
} EntryKind;

typedef struct {
    EntryKind kind;
    const char *section;
    char section_key;
    const char *keys;
    const char *name;
    const char *args;
    const char *usage;
    const char *desc;
    const char *detail;
} Entry;

typedef struct {
    char key;
    const char *name;
    const char *title;
} Section;

static const Section SECTIONS[] = {
    {'a', "all",      "All"},
    {'c', "commands", "Commands"},
    {'g', "general",  "General"},
    {'e', "emit",     "Emit"},
    {'t', "trace",    "Trace"},
    {'b', "bytecode", "Bytecode"},
    {'d', "debugger", "Debugger"},
    {'p', "project",  "Project"},
};

static const Entry ENTRIES[] = {
    {ENTRY_COMMAND, "commands", 'c', "n", "new", "<name>", "monad new <name>",
     "Create a fresh package", "Scaffolds package.yaml, src/Main.mon, README.md, and .gitignore."},
    {ENTRY_COMMAND, "commands", 'c', "b", "build", "", "monad build",
     "Build the current package", "Reads package.yaml and writes the executable under build/."},
    {ENTRY_COMMAND, "commands", 'c', "r", "run", "", "monad run",
     "Build and run the package", "Runs the binary immediately after a successful build."},
    {ENTRY_COMMAND, "commands", 'c', "x", "clean", "", "monad clean",
     "Remove build artifacts", "Deletes build/ and generated object/IR/assembly files."},
    {ENTRY_COMMAND, "commands", 'c', "i", "install", "", "monad install",
     "Install into ~/.local/bin", "Builds if needed, then installs the executable."},
    {ENTRY_COMMAND, "commands", 'c', "t", "test", "[file.mon]", "monad test [file.mon]",
     "Run tests", "Without a file, runs the default regression suite. With a file, builds and runs that test binary."},
    {ENTRY_COMMAND, "commands", 'c', "k", "check", "[file.mon]", "monad check file.mon",
     "Type-check only", "Useful for editors because the exit status is the diagnostic result."},
    {ENTRY_COMMAND, "commands", 'c', "e", "eval", "<code>", "monad eval \"3 + 3\"",
     "Evaluate one expression", "Runs the REPL evaluator once and exits."},
    {ENTRY_COMMAND, "commands", 'c', "R", "repl", "", "monad repl",
     "Start the interactive REPL", "Equivalent to monad -i."},
    {ENTRY_COMMAND, "commands", 'c', "j", "jit", "<file.mon>", "monad jit file.mon",
     "Use bytecode JIT path", "Requests the bytecode baseline JIT instead of the normal LLVM path."},
    {ENTRY_COMMAND, "commands", 'c', "d", "debug", "<file.mon>", "monad debug file.mon",
     "Open compiler debugger", "Starts the compiler debugger TUI for one source file."},
    {ENTRY_COMMAND, "commands", 'c', "l", "lsp", "", "monad lsp",
     "Start language server", "Starts the LSP server, or the LSP REPL when attached to a terminal."},
    {ENTRY_COMMAND, "commands", 'c', "h", "help", "[topic]", "monad help build",
     "Show help", "Prints static help for a command or the top-level menu."},
    {ENTRY_COMMAND, "commands", 'c', "m", "menu", "", "monad menu",
     "Open this flag browser", "Interactive orderless-style browser for commands and flags."},
    {ENTRY_COMMAND, "commands", 'c', "f", "flags", "", "monad flags",
     "Alias for menu", "Same as monad menu."},

    {ENTRY_FLAG, "general", 'g', "h", "-h, --help", "", "monad --help",
     "Show top-level help", "Prints all commands and common options."},
    {ENTRY_FLAG, "general", 'g', "i", "-i, --repl", "", "monad -i",
     "Start interactive REPL", "Starts the normal interactive prompt."},
    {ENTRY_FLAG, "general", 'g', "e", "-e <code>, eval <code>", "", "monad -e \"3 + 3\"",
     "Evaluate one expression", "Shortcut for monad eval <code>."},
    {ENTRY_FLAG, "general", 'g', "d", "-d, --debug", "<file>", "monad -d file.mon",
     "Open debugger", "Shortcut for monad debug <file.mon>."},
    {ENTRY_FLAG, "general", 'g', "o", "-o, --output", "<file>", "monad file.mon -o out",
     "Set output path", "Overrides the executable or artifact output path."},
    {ENTRY_FLAG, "general", 'g', "O", "-O, optimize", "", "monad file.mon -O2",
     "Enable optimizations", "Controls compiler optimization level."},
    {ENTRY_FLAG, "general", 'g', "q", "-q, --quiet", "", "monad file.mon -q",
     "Disable chatter", "Turns off progress output and trace flags."},
    {ENTRY_FLAG, "general", 'g', "v", "-v", "", "monad file.mon -v",
     "Increase verbosity", "Adds one level of compile progress output."},
    {ENTRY_FLAG, "general", 'g', "V", "-vv", "", "monad file.mon -vv",
     "Increase verbosity twice", "Adds two levels of compile progress output."},
    {ENTRY_FLAG, "general", 'g', "0", "--verbose=N", "", "monad file.mon --verbose=0",
     "Set verbosity exactly", "Use N from 0 to 9."},
    {ENTRY_FLAG, "general", 'g', "w", "-Wall", "", "monad file.mon -Wall",
     "Enable all warnings", "Currently accepted as a common compiler flag."},
    {ENTRY_FLAG, "general", 'g', "W", "-Wextra", "", "monad file.mon -Wextra",
     "Enable extra warnings", "Currently accepted as a common compiler flag."},

    {ENTRY_FLAG, "emit", 'e', "i", "--emit-ir, emit-ir", "", "monad file.mon --emit-ir",
     "Emit LLVM IR", "Writes a .ll file for inspection or debugger workflows."},
    {ENTRY_FLAG, "emit", 'e', "b", "--emit-bc, emit-bc", "", "monad file.mon --emit-bc",
     "Emit LLVM bitcode", "Writes LLVM bitcode (.bc)."},
    {ENTRY_FLAG, "emit", 'e', "a", "--emit-asm, -S, asm", "", "monad file.mon -S",
     "Emit assembly", "Writes target assembly (.s)."},
    {ENTRY_FLAG, "emit", 'e', "o", "--emit-obj, -c, obj", "", "monad file.mon -c",
     "Emit object file", "Writes an object file without necessarily keeping a final binary."},
    {ENTRY_FLAG, "emit", 'e', "j", "--emit-json, emit-json", "", "monad file.mon --emit-json",
     "Emit AST JSON", "Useful for tools, tests, and visualizers."},
    {ENTRY_FLAG, "emit", 'e', "t", "--emit-typst, emit-typst", "", "monad file.mon --emit-typst",
     "Emit Typst", "Writes a Typst document and can compile it to PDF."},
    {ENTRY_FLAG, "emit", 'e', "y", "--emit-bytecode, emit-bytecode", "", "monad file.mon --emit-bytecode",
     "Emit bytecode", "Writes Monad register bytecode."},

    {ENTRY_FLAG, "trace", 't', "a", "--trace-all", "", "monad file.mon --trace-all",
     "Enable all traces", "Turns on AST, semantic, dependency, and codegen tracing."},
    {ENTRY_FLAG, "trace", 't', "x", "--trace-off, --no-trace", "", "monad file.mon --trace-off",
     "Disable all traces", "Turns off every trace pass."},
    {ENTRY_FLAG, "trace", 't', "r", "--trace-ast", "", "monad file.mon --trace-ast",
     "Trace reader and AST", "Shows reader output, Wisp expansion, and desugared AST."},
    {ENTRY_FLAG, "trace", 't', "s", "--trace-semantic", "", "monad file.mon --trace-semantic",
     "Trace semantic passes", "Shows semantic lowering and optimization details."},
    {ENTRY_FLAG, "trace", 't', "d", "--trace-dep", "", "monad file.mon --trace-dep",
     "Trace type/dependency pass", "Shows dependency/type-checking progress."},
    {ENTRY_FLAG, "trace", 't', "c", "--trace-codegen", "", "monad file.mon --trace-codegen",
     "Trace codegen", "Shows LLVM/codegen details."},
    {ENTRY_FLAG, "trace", 't', "m", "--trace=<list>", "", "monad file.mon --trace=ast,dep",
     "Trace selected passes", "Comma list: ast, semantic, dep, codegen, all, off."},

    {ENTRY_FLAG, "bytecode", 'b', "v", "--bytecode-verify", "", "monad file.mon --bytecode-verify",
     "Verify bytecode", "Runs the bytecode linear verifier."},
    {ENTRY_FLAG, "bytecode", 'b', "d", "--bytecode-disassemble, bytecode-disassemble", "", "monad file.mon --bytecode-disassemble",
     "Disassemble bytecode", "Prints bytecode instruction listing."},
    {ENTRY_FLAG, "bytecode", 'b', "p", "--bytecode-decompile", "", "monad file.mon --bytecode-decompile",
     "Decompile bytecode", "Prints a Monad-like bytecode view."},
    {ENTRY_FLAG, "bytecode", 'b', "s", "--bytecode-sections", "", "monad file.mon --bytecode-sections",
     "Dump bytecode sections", "Prints bytecode binary sections."},
    {ENTRY_FLAG, "bytecode", 'b', "t", "--bytecode-trace", "", "monad file.mon --bytecode-trace",
     "Trace bytecode", "Traces bytecode execution/tooling."},
    {ENTRY_FLAG, "bytecode", 'b', "j", "--bytecode-baseline-jit", "", "monad file.mon --bytecode-baseline-jit",
     "Request baseline JIT", "Selects the baseline JIT bytecode tier."},
    {ENTRY_FLAG, "bytecode", 'b', "J", "-jit, jit", "", "monad -jit file.mon",
     "Use JIT path", "Shortcut that enables bytecode and baseline JIT."},

    {ENTRY_FLAG, "debugger", 'd', "m", "--debug-no-mouse", "", "monad debug file.mon --debug-no-mouse",
     "Disable mouse", "Turns off mouse reporting in the debugger TUI."},
    {ENTRY_FLAG, "debugger", 'd', "t", "--debug-truecolor", "", "monad debug file.mon --debug-truecolor",
     "Force truecolor", "Forces truecolor output in the debugger."},
    {ENTRY_FLAG, "debugger", 'd', "f", "--debug-fps", "N", "monad debug file.mon --debug-fps 120",
     "Set debugger FPS", "Controls debugger render tick rate."},
    {ENTRY_FLAG, "debugger", 'd', "b", "--debug-blink-ms", "N", "monad debug file.mon --debug-blink-ms 500",
     "Set blink period", "Controls debugger cursor blink period."},
    {ENTRY_FLAG, "debugger", 'd', "n", "--debug-blinks", "N", "monad debug file.mon --debug-blinks 4",
     "Set blink count", "Controls idle cursor blink count."},

    {ENTRY_FLAG, "project", 'p', "T", "--test", "<file>", "monad --test file.mon",
     "Embed tests", "Compiles tests into the binary and keeps the binary."},
    {ENTRY_FLAG, "project", 'p', "t", "test", "<file>", "monad test file.mon",
     "Run tests", "Builds, runs, and deletes the test binary."},
    {ENTRY_FLAG, "project", 'p', "c", "check", "[file]", "monad check file.mon",
     "Editor check mode", "Type-checks without keeping a final binary."},
};

static void row(const char *color, const char *name, const char *desc)
{
    fprintf(stderr, "    %s%-28s" RESET "  " DIM "%s" RESET "\n", color, name, desc);
}

static void row3(const char *color, const char *name, const char *args, const char *desc)
{
    char buf[96];
    if (args && args[0])
        snprintf(buf, sizeof(buf), "%s %s", name, args);
    else
        snprintf(buf, sizeof(buf), "%s", name);
    row(color, buf, desc);
}

static void section(const char *title)
{
    fprintf(stderr, "\n  " DIM "%s" RESET "\n", title);
}

static void gap(void)
{
    fprintf(stderr, "\n");
}

static bool streq(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

static const Entry *find_command(const char *name)
{
    for (int i = 0; i < ARRAY_LEN(ENTRIES); i++)
        if (ENTRIES[i].kind == ENTRY_COMMAND && streq(ENTRIES[i].name, name))
            return &ENTRIES[i];
    return NULL;
}

static void print_entries_for_section(const char *section_name)
{
    for (int i = 0; i < ARRAY_LEN(ENTRIES); i++) {
        const Entry *e = &ENTRIES[i];
        if (!streq(e->section, section_name))
            continue;
        row3(e->kind == ENTRY_COMMAND ? CYAN : YELLOW, e->name, e->args, e->desc);
    }
}

void print_usage(const char *prog)
{
    fprintf(stderr,
            "\n" BOLD "  Monad" RESET "  " DIM "v0.1" RESET "\n\n"
            "  " DIM "Usage:" RESET "  %s " CYAN "<subcommand>" RESET
            " " YELLOW "[options]" RESET " " DIM "[file]" RESET "\n",
            prog ? prog : "monad");

    section("subcommands");
    print_entries_for_section("commands");

    section("common options");
    print_entries_for_section("general");

    section("fast browser");
    row(CYAN, "menu, flags", "Open the orderless-style interactive flag browser");

    gap();
    fprintf(stderr,
            "  " DIM "Run " RESET CYAN "monad menu" RESET DIM
            " for the interactive browser, or " RESET CYAN "monad help <subcommand>" RESET DIM
            " for detailed help." RESET "\n\n");
}

static void print_command_header(const Entry *cmd)
{
    fprintf(stderr, "\n" BOLD "  monad %s" RESET, cmd->name);
    if (cmd->args && cmd->args[0])
        fprintf(stderr, "  " MAGENTA "%s" RESET, cmd->args);
    fprintf(stderr, "\n  " DIM "%s" RESET "\n", cmd->detail ? cmd->detail : cmd->desc);
    section("usage");
    row(CYAN, cmd->usage, cmd->desc);
}

void print_subcommand_menu(const char *subcmd)
{
    if (!subcmd) {
        print_usage("monad");
        return;
    }

    if (streq(subcmd, "flags") || streq(subcmd, "menu")) {
        fprintf(stderr,
                "\n" BOLD "  monad menu" RESET "\n"
                "  " DIM "Interactive orderless-style command and flag browser." RESET "\n");
        section("keys");
        row(CYAN, "C-n/C-p, arrows", "move through candidates");
        row(CYAN, "C-a/C-e", "beginning/end of prompt");
        row(CYAN, "C-b/C-f", "move backward/forward one character");
        row(CYAN, "M-b/M-f", "move backward/forward one word");
        row(CYAN, "M-d, C-w, C-k", "kill word forward, word backward, or line");
        row(CYAN, "C-y", "yank last killed text");
        row(CYAN, "TAB, 1-8", "switch sections");
        row(CYAN, "RET", "accept selected usage and print it");
        row(CYAN, "q, C-g", "quit");
        gap();
        return;
    }

    const Entry *cmd = find_command(subcmd);
    if (!cmd) {
        fprintf(stderr, "\n  " DIM "No detailed help for '" RESET "%s" DIM "'." RESET "\n", subcmd);
        print_usage("monad");
        return;
    }

    print_command_header(cmd);

    if (streq(subcmd, "build") || streq(subcmd, "run")) {
        section("emit flags");
        print_entries_for_section("emit");
        section("trace flags");
        print_entries_for_section("trace");
        section("project flags");
        print_entries_for_section("project");
    } else if (streq(subcmd, "test") || streq(subcmd, "check")) {
        section("test/check flags");
        print_entries_for_section("project");
        section("quiet and trace flags");
        print_entries_for_section("trace");
    } else if (streq(subcmd, "debug")) {
        section("debugger flags");
        print_entries_for_section("debugger");
        section("trace flags");
        print_entries_for_section("trace");
    } else if (streq(subcmd, "jit")) {
        section("bytecode flags");
        print_entries_for_section("bytecode");
    } else if (streq(subcmd, "eval") || streq(subcmd, "repl")) {
        section("general flags");
        print_entries_for_section("general");
    }

    gap();
}

typedef enum {
    KEY_NONE = 0,
    KEY_ESC = 27,
    KEY_ENTER = 1000,
    KEY_BACKSPACE,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_BACKTAB,
    KEY_META_BASE = 2000
} KeyCode;

typedef struct {
    struct termios old_termios;
    bool raw_enabled;
    bool alt_enabled;
    int rows;
    int cols;
    char query[QUERY_CAP];
    int point;
    char kill[KILL_CAP];
    int section_index;
    int selected;
    int scroll;
    bool show_help;
    bool running;
    bool accepted;
    char accepted_usage[256];
    char status[256];
    long long last_input_ms;
} MenuState;

static MenuState *ACTIVE_MENU = NULL;

static long long now_ms(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    ts.tv_sec = time(NULL);
    ts.tv_nsec = 0;
#endif
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static void get_term_size(MenuState *m)
{
    struct winsize ws;
    m->rows = 30;
    m->cols = 100;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) m->rows = ws.ws_row;
        if (ws.ws_col > 0) m->cols = ws.ws_col;
    }
}

static void menu_restore(void)
{
    MenuState *m = ACTIVE_MENU;
    if (!m)
        return;
    if (m->raw_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &m->old_termios);
        m->raw_enabled = false;
    }
    if (m->alt_enabled) {
        fprintf(stderr, "\x1b[?25h\x1b[0m\x1b[?1049l");
        fflush(stderr);
        m->alt_enabled = false;
    } else {
        fprintf(stderr, "\x1b[?25h\x1b[0m");
        fflush(stderr);
    }
}

static bool menu_raw_mode(MenuState *m)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO))
        return false;
    if (tcgetattr(STDIN_FILENO, &m->old_termios) != 0)
        return false;

    struct termios raw = m->old_termios;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
        return false;

    m->raw_enabled = true;
    fprintf(stderr, "\x1b[?1049h\x1b[?25l");
    fflush(stderr);
    m->alt_enabled = true;
    ACTIVE_MENU = m;
    atexit(menu_restore);
    return true;
}

static bool stdin_ready(int timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = select(STDIN_FILENO + 1, &set, NULL, NULL, &tv);
    return rc > 0 && FD_ISSET(STDIN_FILENO, &set);
}

static int read_byte_now(void)
{
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1)
        return (int)c;
    return KEY_NONE;
}

static int read_key(void)
{
    if (!stdin_ready(60))
        return KEY_NONE;

    int c = read_byte_now();
    if (c == KEY_NONE)
        return KEY_NONE;

    if (c == 27) {
        if (!stdin_ready(25))
            return KEY_ESC;
        int a = read_byte_now();
        if (a == '[') {
            if (!stdin_ready(25))
                return KEY_ESC;
            int b = read_byte_now();
            if (b == 'A') return KEY_UP;
            if (b == 'B') return KEY_DOWN;
            if (b == 'C') return KEY_RIGHT;
            if (b == 'D') return KEY_LEFT;
            if (b == 'H') return KEY_HOME;
            if (b == 'F') return KEY_END;
            if (b == 'Z') return KEY_BACKTAB;
            if (b >= '0' && b <= '9') {
                int c2 = KEY_NONE;
                if (stdin_ready(25))
                    c2 = read_byte_now();
                if (c2 == '~') {
                    if (b == '1' || b == '7') return KEY_HOME;
                    if (b == '4' || b == '8') return KEY_END;
                    if (b == '5') return KEY_PGUP;
                    if (b == '6') return KEY_PGDN;
                }
            }
            return KEY_ESC;
        }
        if (a == 'O') {
            if (!stdin_ready(25))
                return KEY_ESC;
            int b = read_byte_now();
            if (b == 'H') return KEY_HOME;
            if (b == 'F') return KEY_END;
            return KEY_ESC;
        }
        return KEY_META_BASE + a;
    }

    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == 127 || c == 8) return KEY_BACKSPACE;
    if (c == 1) return KEY_HOME;
    if (c == 2) return KEY_LEFT;
    if (c == 4) return 4;
    if (c == 5) return KEY_END;
    if (c == 6) return KEY_RIGHT;
    if (c == 7) return 7;
    if (c == 9) return '\t';
    if (c == 11) return 11;
    if (c == 12) return 12;
    if (c == 14) return KEY_DOWN;
    if (c == 16) return KEY_UP;
    if (c == 21) return 21;
    if (c == 23) return 23;
    if (c == 25) return 25;
    return c;
}

static void query_insert(MenuState *m, char c)
{
    int len = (int)strlen(m->query);
    if (len >= QUERY_CAP - 1)
        return;
    if (m->point < 0) m->point = 0;
    if (m->point > len) m->point = len;
    memmove(m->query + m->point + 1, m->query + m->point, (size_t)(len - m->point + 1));
    m->query[m->point++] = c;
}

static void query_delete_range(MenuState *m, int start, int end, bool save_kill)
{
    int len = (int)strlen(m->query);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end)
        return;
    if (save_kill) {
        int n = end - start;
        if (n >= KILL_CAP) n = KILL_CAP - 1;
        memcpy(m->kill, m->query + start, (size_t)n);
        m->kill[n] = '\0';
    }
    memmove(m->query + start, m->query + end, (size_t)(len - end + 1));
    m->point = start;
}

static void query_backspace(MenuState *m)
{
    if (m->point <= 0)
        return;
    query_delete_range(m, m->point - 1, m->point, false);
}

static void query_delete_char(MenuState *m)
{
    int len = (int)strlen(m->query);
    if (m->point >= len)
        return;
    query_delete_range(m, m->point, m->point + 1, false);
}

static int word_forward(const char *s, int p)
{
    int len = (int)strlen(s);
    while (p < len && isspace((unsigned char)s[p])) p++;
    while (p < len && !isspace((unsigned char)s[p])) p++;
    return p;
}

static int word_backward(const char *s, int p)
{
    while (p > 0 && isspace((unsigned char)s[p - 1])) p--;
    while (p > 0 && !isspace((unsigned char)s[p - 1])) p--;
    return p;
}

static void query_yank(MenuState *m)
{
    for (int i = 0; m->kill[i]; i++)
        query_insert(m, m->kill[i]);
}

static bool ascii_contains_ci(const char *hay, const char *needle)
{
    if (!needle || !needle[0])
        return true;
    if (!hay)
        return false;

    size_t nlen = strlen(needle);
    for (const char *h = hay; *h; h++) {
        size_t i = 0;
        while (i < nlen && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen)
            return true;
    }
    return false;
}

static bool entry_has_term(const Entry *e, const char *term)
{
    return ascii_contains_ci(e->section, term) ||
           ascii_contains_ci(e->keys, term) ||
           ascii_contains_ci(e->name, term) ||
           ascii_contains_ci(e->args, term) ||
           ascii_contains_ci(e->usage, term) ||
           ascii_contains_ci(e->desc, term) ||
           ascii_contains_ci(e->detail, term);
}

static bool entry_matches_section(const Entry *e, int section_index)
{
    if (section_index <= 0)
        return true;
    return streq(e->section, SECTIONS[section_index].name);
}

static bool entry_matches_query(const Entry *e, const char *query)
{
    char tmp[QUERY_CAP];
    snprintf(tmp, sizeof(tmp), "%s", query ? query : "");

    char *p = tmp;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        char saved = *p;
        *p = '\0';
        if (!entry_has_term(e, start))
            return false;
        if (!saved) break;
        p++;
    }
    return true;
}

static int entry_score(const Entry *e, const char *query)
{
    int score = 0;
    if (!query || !query[0])
        return 0;
    if (ascii_contains_ci(e->name, query)) score -= 40;
    if (ascii_contains_ci(e->usage, query)) score -= 20;
    if (ascii_contains_ci(e->desc, query)) score -= 10;
    if (e->kind == ENTRY_COMMAND) score -= 4;
    return score;
}

static int collect_results(MenuState *m, int *out, int cap)
{
    int count = 0;
    int scores[MAX_RESULTS];

    for (int i = 0; i < ARRAY_LEN(ENTRIES) && count < cap; i++) {
        const Entry *e = &ENTRIES[i];
        if (!entry_matches_section(e, m->section_index))
            continue;
        if (!entry_matches_query(e, m->query))
            continue;
        out[count] = i;
        scores[count] = entry_score(e, m->query);
        count++;
    }

    for (int i = 1; i < count; i++) {
        int idx = out[i];
        int sc = scores[i];
        int j = i - 1;
        while (j >= 0 && scores[j] > sc) {
            out[j + 1] = out[j];
            scores[j + 1] = scores[j];
            j--;
        }
        out[j + 1] = idx;
        scores[j + 1] = sc;
    }

    return count;
}

static void fprint_clip(FILE *f, const char *s, int width)
{
    if (width <= 0)
        return;
    if (!s) s = "";
    int n = (int)strlen(s);
    if (n <= width) {
        fprintf(f, "%-*s", width, s);
        return;
    }
    if (width <= 1) {
        fputc('.', f);
        return;
    }
    fwrite(s, 1, (size_t)(width - 1), f);
    fputc('.', f);
}

static void draw_prompt(MenuState *m)
{
    long long age = now_ms() - m->last_input_ms;
    bool visible = true;
    if (age >= CURSOR_DELAY_MS)
        visible = ((age - CURSOR_DELAY_MS) / CURSOR_BLINK_MS) % 2 == 0;

    fprintf(stderr, "  " DIM "Query" RESET "  ");
    int len = (int)strlen(m->query);
    for (int i = 0; i <= len; i++) {
        if (i == m->point) {
            char c = i < len ? m->query[i] : ' ';
            if (visible)
                fprintf(stderr, REV "%c" RESET, c);
            else
                fputc(c, stderr);
        } else if (i < len) {
            fputc(m->query[i], stderr);
        }
    }
    fprintf(stderr, "\n");
}

static void draw_tabs(MenuState *m)
{
    fprintf(stderr, "  ");
    for (int i = 0; i < ARRAY_LEN(SECTIONS); i++) {
        if (i == m->section_index)
            fprintf(stderr, BG_HI BOLD CYAN " %d/%c %s " RESET " ", i + 1, SECTIONS[i].key, SECTIONS[i].title);
        else
            fprintf(stderr, DIM " %d/%c %s " RESET " ", i + 1, SECTIONS[i].key, SECTIONS[i].title);
    }
    fprintf(stderr, "\n");
}

static void draw_help_overlay(void)
{
    fprintf(stderr,
            "\n"
            "  " BOLD "Keys" RESET "\n"
            "    C-n/C-p or arrows    move selection\n"
            "    C-a/C-e              beginning/end of prompt\n"
            "    C-b/C-f              move by character\n"
            "    M-b/M-f              move by word\n"
            "    M-d                  kill word forward\n"
            "    C-w                  kill word backward\n"
            "    C-k                  kill to end of line\n"
            "    C-u                  kill whole prompt\n"
            "    C-y                  yank last killed text\n"
            "    C-g                  clear query, then quit if already empty\n"
            "    TAB / S-TAB          next/previous section\n"
            "    1-8 or a/c/g/e/t/b/d/p switch section\n"
            "    RET                  accept and print selected usage\n"
            "    q                    quit\n");
}

static void menu_render(MenuState *m, const char *prog)
{
    get_term_size(m);
    int result_idx[MAX_RESULTS];
    int result_count = collect_results(m, result_idx, MAX_RESULTS);

    if (m->selected >= result_count) m->selected = result_count - 1;
    if (m->selected < 0) m->selected = 0;

    int list_top = 8;
    int detail_lines = 7;
    int list_rows = m->rows - list_top - detail_lines;
    if (list_rows < 5) list_rows = 5;

    if (m->selected < m->scroll) m->scroll = m->selected;
    if (m->selected >= m->scroll + list_rows) m->scroll = m->selected - list_rows + 1;
    if (m->scroll < 0) m->scroll = 0;

    fprintf(stderr, "\x1b[2J\x1b[H\x1b[?25l");
    fprintf(stderr, BOLD "  Monad" RESET "  " DIM "command and flag browser" RESET);
    fprintf(stderr, "  " DIM "%s" RESET "\n\n", prog ? prog : "monad");

    draw_tabs(m);
    fprintf(stderr, "\n");
    draw_prompt(m);

    fprintf(stderr, "\n  " DIM "%-4s  %-11s  %-28s  %s" RESET "\n", "key", "section", "candidate", "description");
    fprintf(stderr, "  " DIM "----  -----------  ----------------------------  -----------" RESET "\n");

    for (int row_i = 0; row_i < list_rows; row_i++) {
        int ri = m->scroll + row_i;
        if (ri >= result_count) {
            fprintf(stderr, "\n");
            continue;
        }
        const Entry *e = &ENTRIES[result_idx[ri]];
        bool sel = ri == m->selected;
        fprintf(stderr, "  ");
        if (sel) fprintf(stderr, BG_HI BOLD GREEN ">" RESET " ");
        else fprintf(stderr, "  ");
        fprintf(stderr, YELLOW);
        fprint_clip(stderr, e->keys, 4);
        fprintf(stderr, RESET "  " DIM);
        fprint_clip(stderr, e->section, 11);
        fprintf(stderr, RESET "  ");
        if (sel) fprintf(stderr, BOLD WHITE); else fprintf(stderr, WHITE);
        char namebuf[128];
        if (e->args && e->args[0]) snprintf(namebuf, sizeof(namebuf), "%s %s", e->name, e->args);
        else snprintf(namebuf, sizeof(namebuf), "%s", e->name);
        fprint_clip(stderr, namebuf, 28);
        fprintf(stderr, RESET "  " DIM);
        fprint_clip(stderr, e->desc, m->cols - 55);
        fprintf(stderr, RESET "\n");
    }

    fprintf(stderr, "\n");
    if (result_count > 0) {
        const Entry *e = &ENTRIES[result_idx[m->selected]];
        fprintf(stderr, "  " BOLD "%s" RESET "  " DIM "%s" RESET "\n", e->name, e->section);
        fprintf(stderr, "  " CYAN "%s" RESET "\n", e->usage);
        fprintf(stderr, "  " DIM "%s" RESET "\n", e->detail ? e->detail : e->desc);
    } else {
        fprintf(stderr, "  " RED "No matches" RESET "\n");
        fprintf(stderr, "  " DIM "Orderless search: type space-separated terms like 'trace dep' or 'emit json'." RESET "\n");
    }

    if (m->status[0])
        fprintf(stderr, "\n  " GREEN "%s" RESET "\n", m->status);
    else
        fprintf(stderr, "\n  " DIM "RET accept  TAB section  ? help  q quit" RESET "\n");

    if (m->show_help)
        draw_help_overlay();

    fflush(stderr);
}

static void section_next(MenuState *m, int delta)
{
    int n = ARRAY_LEN(SECTIONS);
    m->section_index = (m->section_index + delta + n) % n;
    m->selected = 0;
    m->scroll = 0;
}

static void section_by_key(MenuState *m, int key)
{
    if (key >= '1' && key <= '8') {
        int idx = key - '1';
        if (idx >= 0 && idx < ARRAY_LEN(SECTIONS)) {
            m->section_index = idx;
            m->selected = 0;
            m->scroll = 0;
        }
        return;
    }
    for (int i = 0; i < ARRAY_LEN(SECTIONS); i++) {
        if (SECTIONS[i].key == key) {
            m->section_index = i;
            m->selected = 0;
            m->scroll = 0;
            return;
        }
    }
}

static void selected_shortcut(MenuState *m, int key)
{
    int result_idx[MAX_RESULTS];
    int result_count = collect_results(m, result_idx, MAX_RESULTS);
    for (int i = 0; i < result_count; i++) {
        const Entry *e = &ENTRIES[result_idx[i]];
        if (e->keys && strchr(e->keys, key)) {
            m->selected = i;
            snprintf(m->status, sizeof(m->status), "Use: %s", e->usage);
            return;
        }
    }
}

static void accept_selected(MenuState *m)
{
    int result_idx[MAX_RESULTS];
    int result_count = collect_results(m, result_idx, MAX_RESULTS);
    if (result_count <= 0)
        return;
    const Entry *e = &ENTRIES[result_idx[m->selected]];
    snprintf(m->accepted_usage, sizeof(m->accepted_usage), "%s", e->usage);
    m->accepted = true;
    m->running = false;
}

static void handle_key(MenuState *m, int key)
{
    int len = (int)strlen(m->query);
    m->last_input_ms = now_ms();
    m->status[0] = '\0';

    if (key == KEY_NONE)
        return;

    if (key == 'q' || key == KEY_ESC) {
        m->running = false;
        return;
    }

    if (key == '?') {
        m->show_help = !m->show_help;
        return;
    }

    if (key == KEY_ENTER) {
        accept_selected(m);
        return;
    }

    if (key == KEY_DOWN) {
        m->selected++;
        return;
    }
    if (key == KEY_UP) {
        m->selected--;
        return;
    }
    if (key == KEY_PGDN) {
        m->selected += 10;
        return;
    }
    if (key == KEY_PGUP) {
        m->selected -= 10;
        return;
    }

    if (key == '\t') {
        section_next(m, 1);
        return;
    }
    if (key == KEY_BACKTAB) {
        section_next(m, -1);
        return;
    }

    if ((key >= '1' && key <= '8') || key == 'a' || key == 'c' || key == 'g' ||
        key == 'e' || key == 't' || key == 'b' || key == 'd' || key == 'p') {
        section_by_key(m, key);
        return;
    }

    if (key == KEY_HOME) {
        m->point = 0;
        return;
    }
    if (key == KEY_END) {
        m->point = len;
        return;
    }
    if (key == KEY_LEFT) {
        if (m->point > 0) m->point--;
        return;
    }
    if (key == KEY_RIGHT) {
        if (m->point < len) m->point++;
        return;
    }
    if (key == KEY_BACKSPACE) {
        query_backspace(m);
        m->selected = 0;
        m->scroll = 0;
        return;
    }
    if (key == 4) {
        if (len == 0)
            m->running = false;
        else
            query_delete_char(m);
        m->selected = 0;
        m->scroll = 0;
        return;
    }
    if (key == 7) {
        if (len == 0)
            m->running = false;
        else {
            m->query[0] = '\0';
            m->point = 0;
            m->selected = 0;
            m->scroll = 0;
        }
        return;
    }
    if (key == 11) {
        query_delete_range(m, m->point, len, true);
        m->selected = 0;
        m->scroll = 0;
        return;
    }
    if (key == 12) {
        return;
    }
    if (key == 21) {
        query_delete_range(m, 0, len, true);
        m->selected = 0;
        m->scroll = 0;
        return;
    }
    if (key == 23) {
        query_delete_range(m, word_backward(m->query, m->point), m->point, true);
        m->selected = 0;
        m->scroll = 0;
        return;
    }
    if (key == 25) {
        query_yank(m);
        m->selected = 0;
        m->scroll = 0;
        return;
    }

    if (key >= KEY_META_BASE) {
        int meta = key - KEY_META_BASE;
        if (meta == 'd') {
            query_delete_range(m, m->point, word_forward(m->query, m->point), true);
        } else if (meta == 'b') {
            m->point = word_backward(m->query, m->point);
        } else if (meta == 'f') {
            m->point = word_forward(m->query, m->point);
        }
        m->selected = 0;
        m->scroll = 0;
        return;
    }

    if (key >= 32 && key <= 126) {
        selected_shortcut(m, key);
        if (m->status[0])
            return;
        query_insert(m, (char)key);
        m->selected = 0;
        m->scroll = 0;
    }
}

int completion_menu_main(const char *prog)
{
    MenuState m;
    memset(&m, 0, sizeof(m));
    m.running = true;
    m.last_input_ms = now_ms();

    if (!menu_raw_mode(&m)) {
        print_usage(prog ? prog : "monad");
        return 0;
    }

    while (m.running) {
        menu_render(&m, prog ? prog : "monad");
        int key = read_key();
        handle_key(&m, key);
    }

    menu_restore();
    ACTIVE_MENU = NULL;

    if (m.accepted)
        printf("%s\n", m.accepted_usage);

    return 0;
}

#endif
