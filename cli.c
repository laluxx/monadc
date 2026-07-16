#include "cli.h"
#include "config.h"
#include "completion.h"
#include "lsp_repl.h"
#include "debugger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/types.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#endif

/// Internal helpers

static int host_mkdir(const char *path) {
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static char *cli_strndup(const char *s, size_t n) {
    char *r = malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static void host_self_path(char *buf, size_t size) {
    if (!buf || size == 0) return;
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
    if (n == 0 || n >= size) buf[0] = '\0';
#else
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n > 0) buf[n] = '\0';
    else buf[0] = '\0';
#endif
}

/* Quote one argument for the host shell used by system().  Native Windows
 * uses cmd.exe (even when launched by MSYS2), while Unix hosts use sh. */
static bool shell_quote_arg(const char *arg, char *out, size_t size) {
    if (!arg || !out || size < 3) return false;

    size_t used = 0;
#if defined(_WIN32)
    out[used++] = '"';
    for (const char *p = arg; *p; p++) {
        if (used + (*p == '"' ? 2 : 1) + 2 > size) return false;
        if (*p == '"') out[used++] = '\\';
        out[used++] = *p;
    }
    out[used++] = '"';
#else
    out[used++] = '\'';
    for (const char *p = arg; *p; p++) {
        const char *escaped = *p == '\'' ? "'\\''" : NULL;
        size_t needed = escaped ? 4 : 1;
        if (used + needed + 2 > size) return false;
        if (escaped) {
            memcpy(out + used, escaped, needed);
            used += needed;
        } else {
            out[used++] = *p;
        }
    }
    out[used++] = '\'';
#endif
    out[used] = '\0';
    return true;
}

static void make_dir(const char *path) {
    if (host_mkdir(path) != 0 && errno != EEXIST) {
        fprintf(stderr, "Error: cannot create directory '%s': %s\n", path, strerror(errno));
        exit(1);
    }
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "Error: cannot write '%s': %s\n", path, strerror(errno)); exit(1); }
    fputs(content, f);
    fclose(f);
}

static const char *host_exe_suffix(void) {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    return ".exe";
#else
    return "";
#endif
}

static int host_system_success(int status) {
#if defined(_WIN32)
    return status == 0;
#else
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

static int host_system_exit_code(int status) {
#if defined(_WIN32)
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static int host_system_signal(int status) {
#if defined(_WIN32)
    (void)status;
    return 0;
#else
    return WIFSIGNALED(status) ? WTERMSIG(status) : 0;
#endif
}

static char *git_config(const char *key) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "git config --global %s 2>/dev/null", key);
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    char buf[256] = {0};
    if (!fgets(buf, sizeof(buf), p)) { pclose(p); return NULL; }
    pclose(p);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    return len > 0 ? strdup(buf) : NULL;
}

static char *current_year(void) {
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char *buf = malloc(8);
    snprintf(buf, 8, "%d", tm_info->tm_year + 1900);
    return buf;
}

static char *read_file_str(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    size_t n = fread(buf, 1, sz, f);
    buf[n] = '\0'; fclose(f);
    return buf;
}

/// YAML parsing

static char *yaml_scalar(const char *content, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "%s:", key);
    const char *p = content;
    while (p && *p) {
        if (strncmp(p, search, strlen(search)) == 0) {
            p += strlen(search);
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0' || *p == '\n') break;
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (!end) return NULL;
                return cli_strndup(p, end - p);
            }
            const char *end = p;
            while (*end && *end != '\n' && *end != '\r') end++;
            while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
            return cli_strndup(p, end - p);
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
}

static char **yaml_list(const char *content, const char *key, int *count) {
    *count = 0;
    char search[128];
    snprintf(search, sizeof(search), "%s:", key);
    const char *p = content;
    bool found_key = false;
    while (p && *p) {
        if (strncmp(p, search, strlen(search)) == 0) { found_key = true; break; }
        p = strchr(p, '\n'); if (p) p++;
    }
    if (!found_key) return NULL;
    p = strchr(p, '\n'); if (!p) return NULL; p++;

    int cap = 8;
    char **items = malloc(sizeof(char *) * cap);
    while (*p) {
        const char *line = p;
        while (*line == ' ' || *line == '\t') line++;
        if (strncmp(line, "- ", 2) == 0) {
            line += 2;
            const char *end = line;
            while (*end && *end != '\n' && *end != '\r') end++;
            while (end > line && (end[-1] == ' ' || end[-1] == '\t')) end--;
            if (*count >= cap) { cap *= 2; items = realloc(items, sizeof(char *) * cap); }
            items[(*count)++] = cli_strndup(line, end - line);
        } else if (*line != '\0' && *line != '\n' && *line != '\r') {
            break;
        }
        p = strchr(p, '\n'); if (!p) break; p++;
    }
    if (*count == 0) { free(items); return NULL; }
    return items;
}

static char *yaml_exe_field(const char *content, const char *exe_name, const char *field) {
    char needle[256];
    snprintf(needle, sizeof(needle), "  %s:", exe_name);
    const char *p = content;
    while (p && *p) {
        if (strncmp(p, needle, strlen(needle)) == 0) break;
        p = strchr(p, '\n'); if (p) p++;
    }
    if (!p || !*p) return NULL;
    p = strchr(p, '\n'); if (!p) return NULL; p++;

    char fn[128];
    snprintf(fn, sizeof(fn), "%s:", field);
    while (*p) {
        const char *line = p;
        if (*line != ' ' && *line != '\t') break;
        while (*line == ' ' || *line == '\t') line++;
        if (strncmp(line, fn, strlen(fn)) == 0) {
            const char *val = line + strlen(fn);
            while (*val == ' ' || *val == '\t') val++;
            const char *end = val;
            while (*end && *end != '\n' && *end != '\r') end++;
            while (end > val && (end[-1] == ' ' || end[-1] == '\t')) end--;
            return cli_strndup(val, end - val);
        }
        p = strchr(p, '\n'); if (!p) break; p++;
    }
    return NULL;
}

static char *yaml_first_exe(const char *content) {
    const char *p = content;
    while (p && *p) {
        if (strncmp(p, "executables:", 12) == 0) break;
        p = strchr(p, '\n'); if (p) p++;
    }
    if (!p || !*p) return NULL;
    p = strchr(p, '\n'); if (!p) return NULL; p++;
    const char *line = p;
    while (*line == ' ' || *line == '\t') line++;
    const char *end = line;
    while (*end && *end != ':' && *end != '\n') end++;
    if (*end != ':') return NULL;
    return cli_strndup(line, end - line);
}

/// Usage
//
// Levenshtein distance for "did you mean?" suggestions.
// Works on short strings (subcommand names) so a stack matrix is fine.
//
static int levenshtein(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    /* cap to avoid VLA abuse on garbage input */
    if (la > 32) la = 32;
    if (lb > 32) lb = 32;
    int d[33][33];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int del  = d[i-1][j] + 1;
            int ins  = d[i][j-1] + 1;
            int sub  = d[i-1][j-1] + cost;
            d[i][j]  = del < ins ? (del < sub ? del : sub)
                                 : (ins < sub ? ins : sub);
        }
    return d[la][lb];
}

static const char *SUBCOMMANDS[] = {
    "new", "build", "run", "clean", "install",
    "test", "check", "trace", "debug", "lsp", "eval",
    "repl", "jit", "menu", "flags", "help", NULL
};

static bool parse_optimization_flag(const char *arg, int *level)
{
    if (strcmp(arg, "-O") == 0 || strcmp(arg, "optimize") == 0 ||
        strcmp(arg, "--optimize") == 0) {
        *level = 1;
        return true;
    }
    if (arg[0] == '-' && arg[1] == 'O' &&
        arg[2] >= '0' && arg[2] <= '2' && arg[3] == '\0') {
        *level = arg[2] - '0';
        return true;
    }
    if (strncmp(arg, "optimize=", 9) == 0 &&
        arg[9] >= '0' && arg[9] <= '2' && arg[10] == '\0') {
        *level = arg[9] - '0';
        return true;
    }
    if (strncmp(arg, "--optimize=", 11) == 0 &&
        arg[11] >= '0' && arg[11] <= '2' && arg[12] == '\0') {
        *level = arg[11] - '0';
        return true;
    }
    return false;
}

static void trace_set_all(CompilerFlags *flags, bool enabled)
{
    flags->trace_ast = enabled;
    flags->trace_semantic = enabled;
    flags->trace_dep = enabled;
    flags->trace_codegen = enabled;
}

static bool trace_apply_item(const char *item, CompilerFlags *flags)
{
    if (strcmp(item, "all") == 0) {
        trace_set_all(flags, true);
        return true;
    }
    if (strcmp(item, "none") == 0 || strcmp(item, "off") == 0) {
        trace_set_all(flags, false);
        return true;
    }
    if (strcmp(item, "ast") == 0 || strcmp(item, "reader") == 0) {
        flags->trace_ast = true;
        return true;
    }
    if (strcmp(item, "semantic") == 0 || strcmp(item, "opt") == 0) {
        flags->trace_semantic = true;
        return true;
    }
    if (strcmp(item, "dep") == 0 || strcmp(item, "type") == 0) {
        flags->trace_dep = true;
        return true;
    }
    if (strcmp(item, "codegen") == 0 || strcmp(item, "ir") == 0) {
        flags->trace_codegen = true;
        return true;
    }
    return false;
}

static bool parse_trace_flag(const char *arg, CompilerFlags *flags)
{
    if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0 ||
        strcmp(arg, "verbose") == 0) {
        flags->verbose_level++;
        return true;
    }
    if (strcmp(arg, "-vv") == 0 || strcmp(arg, "--very-verbose") == 0) {
        flags->verbose_level += 2;
        return true;
    }
    if (strncmp(arg, "--verbose=", 10) == 0 &&
        arg[10] >= '0' && arg[10] <= '9' && arg[11] == '\0') {
        flags->verbose_level = arg[10] - '0';
        return true;
    }
    if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0 ||
        strcmp(arg, "quiet") == 0) {
        flags->verbose_level = 0;
        trace_set_all(flags, false);
        return true;
    }
    if (strcmp(arg, "--trace-all") == 0 || strcmp(arg, "trace-all") == 0) {
        trace_set_all(flags, true);
        return true;
    }
    if (strcmp(arg, "--trace-off") == 0 || strcmp(arg, "--no-trace") == 0 ||
        strcmp(arg, "trace-off") == 0 || strcmp(arg, "trace-none") == 0) {
        trace_set_all(flags, false);
        return true;
    }
    if (strcmp(arg, "--trace-ast") == 0 || strcmp(arg, "--trace-reader") == 0) {
        flags->trace_ast = true;
        return true;
    }
    if (strcmp(arg, "--trace-semantic") == 0 || strcmp(arg, "--trace-opt") == 0) {
        flags->trace_semantic = true;
        return true;
    }
    if (strcmp(arg, "--trace-dep") == 0 || strcmp(arg, "--trace-type") == 0) {
        flags->trace_dep = true;
        return true;
    }
    if (strcmp(arg, "--trace-codegen") == 0 || strcmp(arg, "--trace-ir") == 0) {
        flags->trace_codegen = true;
        return true;
    }
    if (strncmp(arg, "--trace=", 8) != 0) return false;

    const char *p = arg + 8;
    char item[32];
    while (*p) {
        size_t n = 0;
        while (p[n] && p[n] != ',') n++;
        size_t copy_n = n < sizeof(item) - 1 ? n : sizeof(item) - 1;
        memcpy(item, p, copy_n);
        item[copy_n] = '\0';

        if (!trace_apply_item(item, flags)) {
            fprintf(stderr, "Unknown trace pass: %s\n", item);
            exit(1);
        }

        p += n;
        if (*p == ',') p++;
    }
    return true;
}

static bool parse_trace_value(const char *value, CompilerFlags *flags)
{
    char arg[96];
    snprintf(arg, sizeof(arg), "--trace=%s", value);
    return parse_trace_flag(arg, flags);
}

static bool parse_bytecode_flag(const char *arg, CompilerFlags *flags)
{
    if (!strcmp(arg, "--emit-bytecode") || !strcmp(arg, "emit-bytecode") ||
        !strcmp(arg, "--bytecode") || !strcmp(arg, "bytecode")) {
        flags->emit_bytecode = true;
        return true;
    }
    if (!strcmp(arg, "--bytecode-verify") || !strcmp(arg, "bytecode-verify")) {
        flags->bytecode_verify = true;
        return true;
    }
    if (!strcmp(arg, "--bytecode-disassemble") || !strcmp(arg, "bytecode-disassemble") ||
        !strcmp(arg, "--bytecode-dump") || !strcmp(arg, "bytecode-dump")) {
        flags->bytecode_disassemble = true;
        return true;
    }
    if (!strcmp(arg, "--bytecode-decompile") || !strcmp(arg, "bytecode-decompile")) {
        flags->bytecode_decompile = true;
        return true;
    }
    if (!strcmp(arg, "--bytecode-sections") || !strcmp(arg, "bytecode-sections")) {
        flags->bytecode_dump_sections = true;
        return true;
    }
    if (!strcmp(arg, "--bytecode-trace") || !strcmp(arg, "bytecode-trace")) {
        flags->bytecode_trace = true;
        return true;
    }
    if (!strcmp(arg, "--bytecode-baseline-jit") || !strcmp(arg, "bytecode-baseline-jit")) {
        flags->bytecode_baseline_jit = true;
        return true;
    }
    if (!strcmp(arg, "-jit") || !strcmp(arg, "jit") || !strcmp(arg, "--jit")) {
        flags->jit = true;
        flags->emit_bytecode = true;
        flags->bytecode_baseline_jit = true;
        return true;
    }
    return false;
}

static bool parse_positive_int(const char *value, int *out)
{
    if (!value || !*value) return false;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (*end != '\0' || parsed <= 0 || parsed > 1000000) return false;
    *out = (int)parsed;
    return true;
}

static bool parse_debug_flag(int argc, char **argv, int *index, CompilerFlags *flags)
{
    const char *arg = argv[*index];

    if (!strcmp(arg, "--debug-no-mouse")) {
        flags->debug_no_mouse = true;
        return true;
    }
    if (!strcmp(arg, "--debug-truecolor")) {
        flags->debug_truecolor = true;
        return true;
    }
    if (!strcmp(arg, "--debug-fps")) {
        if (*index + 1 >= argc || !parse_positive_int(argv[*index + 1], &flags->debug_target_fps)) {
            fprintf(stderr, "%s requires a positive integer\n", arg);
            exit(1);
        }
        (*index)++;
        return true;
    }
    if (!strcmp(arg, "--debug-blink-ms")) {
        if (*index + 1 >= argc || !parse_positive_int(argv[*index + 1], &flags->debug_blink_ms)) {
            fprintf(stderr, "%s requires a positive integer\n", arg);
            exit(1);
        }
        (*index)++;
        return true;
    }
    if (!strcmp(arg, "--debug-blinks")) {
        if (*index + 1 >= argc || !parse_positive_int(argv[*index + 1], &flags->debug_blink_count)) {
            fprintf(stderr, "%s requires a positive integer\n", arg);
            exit(1);
        }
        (*index)++;
        return true;
    }
    return false;
}

static bool parse_common_flag(int argc, char **argv, int *index, CompilerFlags *flags)
{
    const char *arg = argv[*index];

    if      (!strcmp(arg, "--emit-ir"   ) || !strcmp(arg, "emit-ir"   ) ||
             !strcmp(arg, "--emit-llvm" ) || !strcmp(arg, "emit-llvm" )) flags->emit_ir    = true;
    else if (!strcmp(arg, "--emit-bc"   ) || !strcmp(arg, "emit-bc"   ) ||
             !strcmp(arg, "--bitcode"   ) || !strcmp(arg, "bitcode"   )) flags->emit_bc    = true;
    else if (!strcmp(arg, "--emit-asm"  ) || !strcmp(arg, "emit-asm"  ) ||
             !strcmp(arg, "-S"          ) || !strcmp(arg, "asm"       )) flags->emit_asm   = true;
    else if (!strcmp(arg, "--emit-obj"  ) || !strcmp(arg, "emit-obj"  ) ||
             !strcmp(arg, "-c"          ) || !strcmp(arg, "obj"       )) flags->emit_obj   = true;
    else if (!strcmp(arg, "--emit-json" ) || !strcmp(arg, "emit-json" )) flags->emit_json  = true;
    else if (!strcmp(arg, "--emit-typst") || !strcmp(arg, "emit-typst")) flags->emit_typst = true;
    else if (parse_bytecode_flag(arg, flags)) {}
    else if (parse_debug_flag(argc, argv, index, flags)) {}
    else if (!strcmp(arg, "--test"      ) || !strcmp(arg, "test"      )) flags->test_mode  = true;
    else if (!strcmp(arg, "-i"          ) || !strcmp(arg, "repl"      ) ||
             !strcmp(arg, "--repl"      ) || !strcmp(arg, "--interactive") ||
             !strcmp(arg, "interactive" )) flags->start_repl = true;
    else if (!strcmp(arg, "-Wall"       ) || !strcmp(arg, "Wall"      ) ||
             !strcmp(arg, "warnings"    )) {}
    else if (!strcmp(arg, "-Wextra"     ) || !strcmp(arg, "Wextra"    ) ||
             !strcmp(arg, "extra-warnings")) {}
    else if (parse_optimization_flag(arg, &flags->optimization_level)) {}
    else if (parse_trace_flag(arg, flags)) {}
    else if (!strcmp(arg, "trace")) {
        if (*index + 1 >= argc) { fprintf(stderr, "trace requires an argument\n"); exit(1); }
        parse_trace_value(argv[++(*index)], flags);
    } else if (!strcmp(arg, "-o") || !strcmp(arg, "--output") || !strcmp(arg, "output")) {
        if (*index + 1 >= argc) { fprintf(stderr, "%s requires an argument\n", arg); exit(1); }
        flags->output_name = argv[++(*index)];
    } else {
        return false;
    }
    return true;
}

static bool is_dispatch_word(const char *arg)
{
    if (!arg) return false;
    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0 ||
        strcmp(arg, "-d") == 0 || strcmp(arg, "--debug") == 0 ||
        strcmp(arg, "-jit") == 0 || strcmp(arg, "--jit") == 0 ||
        strcmp(arg, "-i") == 0 ||
        strcmp(arg, "-e") == 0 || strcmp(arg, "--eval") == 0 ||
        strcmp(arg, "--test") == 0 || strcmp(arg, "--test-run") == 0)
        return true;
    for (int i = 0; SUBCOMMANDS[i]; i++)
        if (strcmp(arg, SUBCOMMANDS[i]) == 0)
            return true;
    return false;
}

static bool is_test_suite_word(const char *arg)
{
    return arg &&
           (strcmp(arg, "list") == 0 ||
            strcmp(arg, "runner") == 0 ||
            strcmp(arg, "core") == 0 ||
            strcmp(arg, "how-to") == 0 ||
            strcmp(arg, "windows") == 0 ||
            strcmp(arg, "cmake") == 0 ||
            strcmp(arg, "readme") == 0 ||
            strcmp(arg, "bytecode") == 0 ||
            strcmp(arg, "all") == 0);
}

static bool is_help_word(const char *arg)
{
    return arg &&
           (strcmp(arg, "-h") == 0 ||
            strcmp(arg, "--help") == 0 ||
            strcmp(arg, "help") == 0);
}

static void print_test_help(void)
{
    printf("Usage: monad test [list|runner|windows|how-to|file.mon] [options]\n\n");
    printf("Run repository test suites or compile and run tests in one .mon file.\n\n");
    printf("Available test suites:\n");
    printf("  list      Show this suite menu\n");
    printf("  runner    Python harness contracts, portability checks, examples, and bytecode tests\n");
    printf("  core      Core and prelude module tests\n");
    printf("  how-to    README-listed how_to example smokes\n");
    printf("  windows   MSYS2/Windows portability contracts\n");
    printf("  cmake     CMake and CI contract tests\n");
    printf("  readme    Human-facing README product contract\n");
    printf("  bytecode  Bytecode VM, verifier, serialization, and visual diagnostics\n");
    printf("  all       Runner plus core suites\n\n");
    printf("Examples:\n");
    printf("  monad test list\n");
    printf("  monad test runner\n");
    printf("  monad test core/prelude/Data/Enum.mon\n");
}

static bool is_common_option_word(const char *arg)
{
    if (!arg) return false;
    return arg[0] == '-' ||
           strcmp(arg, "verbose") == 0 ||
           strcmp(arg, "quiet") == 0 ||
           strcmp(arg, "trace") == 0 ||
           strcmp(arg, "trace-all") == 0 ||
           strcmp(arg, "trace-off") == 0 ||
           strcmp(arg, "trace-none") == 0 ||
           strcmp(arg, "optimize") == 0 ||
           strncmp(arg, "optimize=", 9) == 0 ||
           strcmp(arg, "output") == 0 ||
           strcmp(arg, "emit-ir") == 0 ||
           strcmp(arg, "emit-llvm") == 0 ||
           strcmp(arg, "emit-bc") == 0 ||
           strcmp(arg, "bitcode") == 0 ||
           strcmp(arg, "emit-asm") == 0 ||
           strcmp(arg, "asm") == 0 ||
           strcmp(arg, "emit-obj") == 0 ||
           strcmp(arg, "obj") == 0 ||
           strcmp(arg, "emit-json") == 0 ||
           strcmp(arg, "emit-typst") == 0 ||
           strcmp(arg, "emit-bytecode") == 0 ||
           strcmp(arg, "bytecode") == 0 ||
           strcmp(arg, "bytecode-verify") == 0 ||
           strcmp(arg, "bytecode-disassemble") == 0 ||
           strcmp(arg, "bytecode-dump") == 0 ||
           strcmp(arg, "bytecode-decompile") == 0 ||
           strcmp(arg, "bytecode-sections") == 0 ||
           strcmp(arg, "bytecode-trace") == 0 ||
           strcmp(arg, "bytecode-baseline-jit") == 0 ||
           strcmp(arg, "Wall") == 0 ||
           strcmp(arg, "warnings") == 0 ||
           strcmp(arg, "Wextra") == 0 ||
           strcmp(arg, "extra-warnings") == 0;
}

static void suggest_subcommand(const char *unknown)
{
    const char *best  = NULL;
    int         best_d = 4;   /* only suggest if distance <= 3 */
    for (int i = 0; SUBCOMMANDS[i]; i++) {
        int d = levenshtein(unknown, SUBCOMMANDS[i]);
        if (d < best_d) { best_d = d; best = SUBCOMMANDS[i]; }
    }
    if (best)
        fprintf(stderr, "\n  Did you mean \x1b[33m%s\x1b[0m?\n", best);
}

CompilerFlags parse_flags(int argc, char **argv) {
    CompilerFlags flags = {0};
    flags.mode = CMD_COMPILE;
    config_apply_default_flags(&flags);

    if (argc < 2) { flags.mode = CMD_REPL; flags.start_repl = true; return flags; }

    /* Accept common options before the subcommand, e.g.
     *   monad verbose test file.mon
     *   monad -v test file.mon
     * The rest of the parser is intentionally subcommand-first, so normalize
     * leading option words/flags by applying them, then dispatching from the
     * first real command or input path. */
    char **normalized_argv = NULL;
    int first = 1;
    while (first < argc && !is_dispatch_word(argv[first])) {
        int before = first;
        if (!parse_common_flag(argc, argv, &first, &flags))
            break;
        first++;
        if (first <= before)
            break;
    }
    if (first >= argc) {
        flags.mode = CMD_REPL;
        flags.start_repl = true;
        return flags;
    }
    if (first > 1) {
        int normalized_argc = argc - first + 1;
        normalized_argv = malloc(sizeof(char *) * (normalized_argc + 1));
        normalized_argv[0] = argv[0];
        for (int i = first; i < argc; i++)
            normalized_argv[i - first + 1] = argv[i];
        normalized_argv[normalized_argc] = NULL;
        argv = normalized_argv;
        argc = normalized_argc;
    }

    if (strcmp(argv[1], "trace") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s trace <pass> [file.mon] [options]\n", argv[0]);
            exit(1);
        }
        parse_trace_value(argv[2], &flags);
        if (argc == 3) {
            flags.mode = CMD_REPL;
            flags.start_repl = true;
            return flags;
        }
        flags.input_file = argv[3];
        for (int i = 4; i < argc; i++) {
            if (!parse_common_flag(argc, argv, &i, &flags)) {
                fprintf(stderr, "Unknown flag: %s\n", argv[i]);
                print_usage(argv[0]); exit(1);
            }
        }
        return flags;
    }

    // Help
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]); exit(0);
    }
    if (strcmp(argv[1], "help") == 0) {
        if (argc >= 3)
            print_subcommand_menu(argv[2]);
        else
            print_usage(argv[0]);
        exit(0);
    }
    if (strcmp(argv[1], "menu") == 0 || strcmp(argv[1], "flags") == 0) {
        exit(completion_menu_main(argv[0]));
    }

    if (strcmp(argv[1], "-d") == 0 || strcmp(argv[1], "--debug") == 0 ||
        strcmp(argv[1], "debug") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s debug <file.mon> [options]\n", argv[0]); exit(1); }
        flags.mode = CMD_DEBUG;
        flags.input_file = argv[2];
        for (int i = 3; i < argc; i++) {
            if (!parse_common_flag(argc, argv, &i, &flags)) {
                fprintf(stderr, "Unknown debug flag: %s\n", argv[i]);
                print_usage(argv[0]); exit(1);
            }
        }
        return flags;
    }

    if (strcmp(argv[1], "-jit") == 0 || strcmp(argv[1], "--jit") == 0 ||
        strcmp(argv[1], "jit") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s jit <file.mon> [options]\n", argv[0]); exit(1); }
        flags.jit = true;
        flags.emit_bytecode = true;
        flags.bytecode_baseline_jit = true;
        flags.input_file = argv[2];
        for (int i = 3; i < argc; i++) {
            if (!parse_common_flag(argc, argv, &i, &flags)) {
                fprintf(stderr, "Unknown flag: %s\n", argv[i]);
                print_usage(argv[0]); exit(1);
            }
        }
        return flags;
    }

    // Subcommands
    if (strcmp(argv[1], "new") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s new <name>\n", argv[0]); exit(1); }
        flags.mode         = CMD_NEW;
        flags.package_name = argv[2];
        return flags;
    }
    if (strcmp(argv[1], "run") == 0 || strcmp(argv[1], "build") == 0) {
        flags.mode = strcmp(argv[1], "run") == 0 ? CMD_RUN : CMD_BUILD;
        for (int i = 2; i < argc; i++) {
            if (!parse_common_flag(argc, argv, &i, &flags)) {
                fprintf(stderr, "Unknown flag: %s\n", argv[i]);
                print_usage(argv[0]); exit(1);
            }
        }
        return flags;
    }
    if (strcmp(argv[1], "clean")   == 0) { flags.mode = CMD_CLEAN;   return flags; }
    if (strcmp(argv[1], "install") == 0) { flags.mode = CMD_INSTALL; return flags; }
    if (strcmp(argv[1], "check")   == 0) {
        flags.mode = CMD_CHECK;
        if (argc >= 3) flags.input_file = argv[2];
        for (int i = 3; i < argc; i++) {
            if (!parse_common_flag(argc, argv, &i, &flags)) {
                fprintf(stderr, "Unknown check flag: %s\n", argv[i]);
                print_usage(argv[0]); exit(1);
            }
        }
        return flags;
    }
    if (strcmp(argv[1], "-i") == 0 || strcmp(argv[1], "repl") == 0) {
        flags.mode = CMD_REPL; flags.start_repl = true; return flags;
    }
    if (strcmp(argv[1], "lsp")     == 0) { flags.mode = CMD_LSP;  return flags; }
    if (strcmp(argv[1], "-e") == 0 || strcmp(argv[1], "--eval") == 0 ||
        strcmp(argv[1], "eval") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s eval <code>\n", argv[0]); exit(1); }
        flags.mode = CMD_EVAL;
        flags.eval_code = argv[2];
        return flags;
    }

    // monad test <suite>     ->  run unified repository test suite
    // monad test <file.mon>  ->  compile with tests, run _test binary, delete it
    if (strcmp(argv[1], "test") == 0) {
        flags.mode       = CMD_TEST;
        flags.test_mode  = true;
        flags.test_run   = true;
        if (argc >= 3 && is_help_word(argv[2])) {
            flags.test_suite = "help";
            return flags;
        }
        int option_start = 2;
        if (argc >= 3 && !is_common_option_word(argv[2])) {
            if (is_test_suite_word(argv[2]))
                flags.test_suite = argv[2];
            else
                flags.input_file = argv[2];
            option_start = 3;
        }
        for (int i = option_start; i < argc; i++) {
            if (!parse_common_flag(argc, argv, &i, &flags)) {
                fprintf(stderr, "Unknown test flag: %s\n", argv[i]);
                print_usage(argv[0]); exit(1);
            }
        }
        return flags;
    }

    // monad --test <file.mon>  ->  compile with tests embedded, keep binary
    if (strcmp(argv[1], "--test") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s --test <file.mon>\n", argv[0]); exit(1); }
        flags.test_mode  = true;
        flags.input_file = argv[2];
        int start = 3;
        for (int i = start; i < argc; i++) {
            if (!parse_common_flag(argc, argv, &i, &flags)) {
                fprintf(stderr, "Unknown flag: %s\n", argv[i]);
                print_usage(argv[0]); exit(1);
            }
        }
        return flags;
    }

    // Internal flag used by cmd_test — compile with tests + _test suffix
    if (strcmp(argv[1], "--test-run") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s --test-run <file.mon>\n", argv[0]); exit(1); }
        flags.test_mode  = true;
        flags.test_run   = true;
        flags.input_file = argv[2];
        for (int i = 3; i < argc; i++) {
            if (!parse_common_flag(argc, argv, &i, &flags)) {
                fprintf(stderr, "Unknown flag: %s\n", argv[i]);
                print_usage(argv[0]); exit(1);
            }
        }
        return flags;
    }

    /* If the first argument looks like a subcommand (no dot, no slash, no dash)
       but wasn't recognised, offer a Levenshtein suggestion before falling
       through to treat it as a filename.                                       */
    {
        const char *a1 = argv[1];
        bool looks_like_subcmd = (a1[0] != '-' && a1[0] != '.'
                                  && !strchr(a1, '/') && !strchr(a1, '.'));
        if (looks_like_subcmd) {
            bool known = false;
            for (int k = 0; SUBCOMMANDS[k]; k++)
                if (strcmp(a1, SUBCOMMANDS[k]) == 0) { known = true; break; }
            if (!known) {
                fprintf(stderr, "\x1b[31merror:\x1b[0m unknown subcommand '%s'\n", a1);
                suggest_subcommand(a1);
                fprintf(stderr, "\n");
                print_usage(argv[0]);
                exit(1);
            }
        }
    }

    /* monad <file.mon> [flags...]  ->  normal compile */
    flags.input_file = argv[1];
    for (int i = 2; i < argc; i++) {
        if (!parse_common_flag(argc, argv, &i, &flags)) {
            fprintf(stderr, "Unknown flag: %s\n", argv[i]);
            print_usage(argv[0]); exit(1);
        }
    }
    return flags;
}

char *get_base_executable_name(const char *path) {
    char *copy = strdup(path);
    char *base = basename(copy);
    char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
    char *result = strdup(base); free(copy);
    return result;
}

void cmd_new(const char *package_name) {
    char *author = git_config("user.name");
    if (!author) { char *u = getenv("USER"); author = u ? strdup(u) : strdup("unknown"); }
    char *email = git_config("user.email");
    if (!email) email = strdup("");
    char *year = current_year();

    make_dir(package_name);
    char src_path[512]; snprintf(src_path, sizeof(src_path), "%s/src", package_name);
    make_dir(src_path);

    {
        char path[512]; snprintf(path, sizeof(path), "%s/package.yaml", package_name);
        char content[2048];
        snprintf(content, sizeof(content),
            "name:                %s\n"
            "version:             0.1.0.0\n"
            "github:              \"%s/%s\"\n"
            "license:             MIT\n"
            "author:              \"%s\"\n"
            "maintainer:          \"%s\"\n"
            "copyright:           \"%s %s\"\n"
            "\nextra-source-files:\n"
            "\ndescription:         A Monad package\n"
            "\ndependencies:\n- core >= 0.1 && < 1.0\n"
            "\nmonad-options:\n- -Wall\n- -Wextra\n"
            "\nexecutables:\n  %s:\n    main: Main.mon\n    source-dirs: src\n",
            package_name, author, package_name, author, email, year, author, package_name);
        write_file(path, content);
    }
    {
        char path[512]; snprintf(path, sizeof(path), "%s/src/Main.mon", package_name);
        char content[256];
        snprintf(content, sizeof(content),
            "(module Main)\n\n(show \"Say Hello to %s!\")\n", package_name);
        write_file(path, content);
    }
    {
        char path[512]; snprintf(path, sizeof(path), "%s/.gitignore", package_name);
        write_file(path, "build/\n*.o\n*.ll\n*.s\n");
    }
    {
        char path[512]; snprintf(path, sizeof(path), "%s/README.md", package_name);
        char content[512];
        snprintf(content, sizeof(content),
            "# %s\n\nA [Monad](https://github.com/nytrix-lang) project.\n\n"
            "## Build\n\n```sh\nmonad run\n```\n", package_name);
        write_file(path, content);
    }

    printf("\n");
    printf("  ╭─ %s/\n",     package_name);
    printf("  │  ├── src/\n");
    printf("  │  │   ╰── Main.mon\n");
    printf("  │  ├── package.yaml\n");
    printf("  │  ├── README.md\n");
    printf("  │  ╰── .gitignore\n");
    printf("  │\n");
    printf("  ╰─ \x1b[32m✓\x1b[0m Ready!\n\n");
    printf("  cd %s && monad run\n\n", package_name);

    free(author); free(email); free(year);
}

/// Build helpers

static bool file_has_main_module(const char *path) {
    char *src = read_file_str(path);
    if (!src) return false;
    const char *p = src;
    bool found = false;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == ';') { while (*p && *p != '\n') p++; continue; }
        if (*p != '(') break;
        if (strncmp(p, "(module", 7) == 0) {
            const char *q = p + 7;
            while (*q == ' ' || *q == '\t') q++;
            if (strncmp(q, "Main", 4) == 0 &&
                (q[4] == ' ' || q[4] == '\t' || q[4] == '\n' || q[4] == '\r' || q[4] == ')'))
                { found = true; break; }
        }
        break;
    }
    free(src); return found;
}

static char *find_main_recursive(const char *dir) {
    char candidate[1024];
    snprintf(candidate, sizeof(candidate), "%s/Main.mon", dir);
    if (access(candidate, F_OK) == 0 && file_has_main_module(candidate))
        return strdup(candidate);

    DIR *d = opendir(dir); if (!d) return NULL;
    char *result = NULL;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && !result) {
        if (ent->d_name[0] == '.') continue;
        char full[1024]; snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (strcmp(full, candidate) == 0) continue;
        struct stat st; if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            result = find_main_recursive(full);
        } else if (S_ISREG(st.st_mode)) {
            size_t len = strlen(ent->d_name);
            if (len > 4 && strcmp(ent->d_name + len - 4, ".mon") == 0)
                if (file_has_main_module(full)) result = strdup(full);
        }
    }
    closedir(d); return result;
}

typedef struct {
    char *pkg_name;
    char *exe_name;
    char *main_file;
    char *build_dir;
    char *out_path;
    char *project_root;
    char *src_dirs;       /* preserved for clean */
    char *monad_options;
} BuildInfo;

static BuildInfo resolve_build_info(void) {
    BuildInfo bi = {0};

    char yaml_path[1024] = "package.yaml";
    bool found = false;
    for (int lvl = 0; lvl < 5; lvl++) {
        if (access(yaml_path, F_OK) == 0) { found = true; break; }
        char tmp[1024]; snprintf(tmp, sizeof(tmp), "../%s", yaml_path);
        strncpy(yaml_path, tmp, sizeof(yaml_path) - 1);
    }
    if (!found) {
        fprintf(stderr, "Error: package.yaml not found\n"); exit(1);
    }

    char proj_root[1024] = ".";
    {
        char tmp[1024]; strncpy(tmp, yaml_path, sizeof(tmp) - 1);
        char *sl = strrchr(tmp, '/');
        if (sl) { *sl = '\0'; strncpy(proj_root, tmp, sizeof(proj_root) - 1); }
    }
    bi.project_root = strdup(proj_root);

    char *yaml = read_file_str(yaml_path);
    if (!yaml) { fprintf(stderr, "Error: cannot read %s\n", yaml_path); exit(1); }

    bi.pkg_name = yaml_scalar(yaml, "name");
    if (!bi.pkg_name) { fprintf(stderr, "Error: 'name' not in package.yaml\n"); exit(1); }

    bi.exe_name = yaml_first_exe(yaml);
    if (!bi.exe_name) bi.exe_name = strdup(bi.pkg_name);

    char *src_dirs = yaml_exe_field(yaml, bi.exe_name, "source-dirs");
    if (!src_dirs) src_dirs = yaml_scalar(yaml, "source-dirs");
    if (!src_dirs) src_dirs = strdup("src");
    bi.src_dirs = src_dirs;  /* owned by bi */

    char *main_val = yaml_exe_field(yaml, bi.exe_name, "main");
    if (!main_val) main_val = strdup("Main.mon");

    char try_path[1024];
    snprintf(try_path, sizeof(try_path), "%s/%s/%s", proj_root, src_dirs, main_val);
    if (access(try_path, F_OK) == 0 && file_has_main_module(try_path)) {
        bi.main_file = strdup(try_path);
    } else {
        char search_dir[1024];
        snprintf(search_dir, sizeof(search_dir), "%s/%s", proj_root, src_dirs);
        bi.main_file = find_main_recursive(search_dir);
        if (!bi.main_file) {
            fprintf(stderr, "Error: cannot find main file '%s' in '%s'\n",
                    main_val, search_dir);
            free(yaml); free(main_val); exit(1);
        }
    }
    free(main_val);

    char build_dir[1024];
    snprintf(build_dir, sizeof(build_dir), "%s/build", proj_root);
    make_dir(build_dir);
    bi.build_dir = strdup(build_dir);

    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/%s", build_dir, bi.exe_name);
    bi.out_path = strdup(out_path);

    int opt_count = 0;
    char **opts = yaml_list(yaml, "monad-options", &opt_count);
    if (opts && opt_count > 0) {
        size_t total = 1;
        for (int i = 0; i < opt_count; i++) total += strlen(opts[i]) + 1;
        bi.monad_options = malloc(total); bi.monad_options[0] = '\0';
        for (int i = 0; i < opt_count; i++) {
            if (i > 0) strcat(bi.monad_options, " ");
            strcat(bi.monad_options, opts[i]);
            free(opts[i]);
        }
        free(opts);
    } else {
        bi.monad_options = strdup("");
    }

    free(yaml);
    return bi;
}

static void build_info_free(BuildInfo *bi) {
    free(bi->pkg_name);  free(bi->exe_name);  free(bi->main_file);
    free(bi->build_dir); free(bi->out_path);  free(bi->project_root);
    free(bi->src_dirs);  free(bi->monad_options);
}

static int do_build(const BuildInfo *bi, const CompilerFlags *flags) {
    char self[1024] = "monad";
    {
        char buf[1024] = {0};
        host_self_path(buf, sizeof(buf));
        if (buf[0]) strncpy(self, buf, sizeof(self) - 1);
    }
    char cmd[4096];
    char quoted_self[2048];
    char quoted_main[2048];
    char quoted_out[2048];
    if (!shell_quote_arg(self, quoted_self, sizeof(quoted_self)) ||
        !shell_quote_arg(bi->main_file, quoted_main, sizeof(quoted_main)) ||
        !shell_quote_arg(bi->out_path, quoted_out, sizeof(quoted_out))) {
        fprintf(stderr, "Error: package build path is too long\n");
        return 1;
    }
    char opt_flag[8] = "";
    char emit_flags[512] = "";
    char trace_flags[128] = "";
    if (flags && flags->optimization_level > 0)
        snprintf(opt_flag, sizeof(opt_flag), " -O%d", flags->optimization_level);
    if (flags) {
        if (flags->emit_ir)
            strncat(emit_flags, " --emit-ir", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->emit_bc)
            strncat(emit_flags, " --emit-bc", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->emit_asm)
            strncat(emit_flags, " --emit-asm", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->emit_obj)
            strncat(emit_flags, " --emit-obj", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->emit_json)
            strncat(emit_flags, " --emit-json", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->emit_typst)
            strncat(emit_flags, " --emit-typst", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->emit_bytecode)
            strncat(emit_flags, " --emit-bytecode", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->bytecode_verify)
            strncat(emit_flags, " --bytecode-verify", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->bytecode_disassemble)
            strncat(emit_flags, " --bytecode-disassemble", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->bytecode_decompile)
            strncat(emit_flags, " --bytecode-decompile", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->bytecode_dump_sections)
            strncat(emit_flags, " --bytecode-sections", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->bytecode_trace)
            strncat(emit_flags, " --bytecode-trace", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->bytecode_baseline_jit)
            strncat(emit_flags, " --bytecode-baseline-jit", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->jit)
            strncat(emit_flags, " -jit", sizeof(emit_flags) - strlen(emit_flags) - 1);
        if (flags->trace_ast)
            strncat(trace_flags, " --trace=ast", sizeof(trace_flags) - strlen(trace_flags) - 1);
        if (flags->trace_semantic)
            strncat(trace_flags, " --trace=semantic", sizeof(trace_flags) - strlen(trace_flags) - 1);
        if (flags->trace_dep)
            strncat(trace_flags, " --trace=dep", sizeof(trace_flags) - strlen(trace_flags) - 1);
        if (flags->trace_codegen)
            strncat(trace_flags, " --trace=codegen", sizeof(trace_flags) - strlen(trace_flags) - 1);
        for (int i = 0; i < flags->verbose_level && strlen(trace_flags) + 3 < sizeof(trace_flags); i++)
            strncat(trace_flags, " -v", sizeof(trace_flags) - strlen(trace_flags) - 1);
    }

    snprintf(cmd, sizeof(cmd), "%s %s -o %s%s%s%s%s%s",
             quoted_self, quoted_main, quoted_out,
             bi->monad_options[0] ? " " : "",
             bi->monad_options[0] ? bi->monad_options : "",
             opt_flag,
             emit_flags,
             trace_flags);
    return system(cmd);
}

void cmd_build(const CompilerFlags *flags) {
    BuildInfo bi = resolve_build_info();
    printf("╭─ Build %s\n╰", bi.pkg_name);
    fflush(stdout);

    int rc = do_build(&bi, flags);

    if (rc == 0)
        printf("─ \x1b[32m✓\x1b[0m build/%s\n", bi.exe_name);
    else
        printf("─ failed\n");

    build_info_free(&bi);
    exit(rc == 0 ? 0 : 1);
}

void cmd_run(const CompilerFlags *flags) {
    BuildInfo bi = resolve_build_info();
    printf("╭─ Run %s\n╰", bi.pkg_name);
    fflush(stdout);

    int rc = do_build(&bi, flags);
    if (rc != 0) {
        printf("─ failed\n");
        build_info_free(&bi); exit(1);
    }

    /* compile() already printed [done] ./build/exe with no newline.
       We print the connecting line immediately after on the same line,
       then the program output follows on the next line.             */
    printf("\n│\n├─ \x1b[32m✓\x1b[0m Build %s\n╰─▶ ", bi.exe_name);
    fflush(stdout);

    rc = system(bi.out_path);
    printf("\n");
    build_info_free(&bi);
    exit(rc == 0 ? 0 : 1);
}

static void clean_ext_in_dir(const char *dir, const char *ext) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t nlen = strlen(ent->d_name);
        size_t elen = strlen(ext);
        if (nlen > elen && strcmp(ent->d_name + nlen - elen, ext) == 0) {
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
            remove(full);
        }
    }
    closedir(d);
}

static void rmdir_recursive(const char *path) {
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) rmdir_recursive(full);
        else remove(full);
    }
    closedir(d);
    rmdir(path);
}

void cmd_clean(void) {
    BuildInfo bi = resolve_build_info();

    printf("╭─ Clean %s\n", bi.pkg_name);

    /* Remove build/ */
    char build_dir[1024];
    snprintf(build_dir, sizeof(build_dir), "%s/build", bi.project_root);
    if (access(build_dir, F_OK) == 0) {
        rmdir_recursive(build_dir);
        printf("│  removed build/\n");
    }

    /* Remove .o, .ll, .s from source-dirs
       source-dirs is a single path for now; split on spaces if ever needed */
    char src_full[1024];
    snprintf(src_full, sizeof(src_full), "%s/%s", bi.project_root, bi.src_dirs);
    clean_ext_in_dir(src_full, ".o");
    clean_ext_in_dir(src_full, ".ll");
    clean_ext_in_dir(src_full, ".s");
    printf("│  removed *.o *.ll *.s from %s/\n", bi.src_dirs);

    printf("╰─ \x1b[32m✓\x1b[0m done\n");

    build_info_free(&bi);
}

void cmd_install(void) {
    BuildInfo bi = resolve_build_info();

    /* Build if binary doesn't exist yet */
    if (access(bi.out_path, F_OK) != 0) {
        printf("╭─ Install %s\n│  (building first...)\n╰", bi.pkg_name);
        fflush(stdout);
        int rc = do_build(&bi, NULL);
        if (rc != 0) {
            printf("─ build failed\n");
            build_info_free(&bi); exit(1);
        }
        printf("\n");
    } else {
        printf("╭─ Install %s\n", bi.pkg_name);
    }

    const char *home = getenv("HOME");
    if (!home) { fprintf(stderr, "Error: $HOME not set\n"); exit(1); }

    /* Ensure ~/.local/bin exists */
    char local[512], local_bin[512];
    snprintf(local,     sizeof(local),     "%s/.local",     home);
    snprintf(local_bin, sizeof(local_bin), "%s/.local/bin", home);
    make_dir(local);
    make_dir(local_bin);

    char dest[512];
    snprintf(dest, sizeof(dest), "%s/%s", local_bin, bi.exe_name);

    /* Copy with install(1) to set correct permissions */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "install -m 755 %s %s", bi.out_path, dest);
    int rc = system(cmd);

    if (rc == 0) {
        printf("│  → %s\n", dest);
        printf("╰─ \x1b[32m✓\x1b[0m installed\n");

        /* Warn if ~/.local/bin not in PATH */
        const char *path_env = getenv("PATH");
        if (!path_env || !strstr(path_env, local_bin)) {
            printf("\n  hint: ~/.local/bin is not in your PATH\n");
            printf("  add to your shell rc:\n");
            printf("    export PATH=\"$HOME/.local/bin:$PATH\"\n\n");
        }
    } else {
        printf("╰─ failed\n");
    }

    build_info_free(&bi);
    exit(rc == 0 ? 0 : 1);
}


void cmd_check(const char *input_file) {
    /* If a specific file was given, check just that file (no link).
     * Otherwise find the project's main file and check the whole project.
     * Either way: compile only, no link, no binary emitted.
     * Exit 0 = no errors, exit 1 = errors (for flycheck/editors).    */
    char self[1024] = "monad";
    {
        char buf[1024] = {0};
        host_self_path(buf, sizeof(buf));
        if (buf[0]) strncpy(self, buf, sizeof(self) - 1);
    }

    const char *target = input_file;
    BuildInfo bi = {0};
    bool used_bi = false;

    if (!target) {
        /* No file given — find the project main file */
        bi = resolve_build_info();
        used_bi = true;
        target = bi.main_file;
    }

    /* Compile with --emit-obj so dependencies are resolved and type-checked,
     * but suppress the linker step by not passing -o.
     * We use a temp output name that we delete afterwards.            */
    char tmp_out[1024];
    snprintf(tmp_out, sizeof(tmp_out), "/tmp/__monad_check_%d", (int)getpid());

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s %s -o %s 2>&1", self, target, tmp_out);

    int rc = system(cmd);

    /* Clean up any binary that was produced */
    remove(tmp_out);
    if (host_exe_suffix()[0]) {
        char tmp_exe[1060];
        snprintf(tmp_exe, sizeof(tmp_exe), "%s%s", tmp_out, host_exe_suffix());
        remove(tmp_exe);
    }

    if (used_bi) build_info_free(&bi);
    exit(host_system_success(rc) ? 0 : 1);
}

void cmd_debug(const CompilerFlags *flags) {
#if defined(_WIN32)
    (void)flags;
    fprintf(stderr, "monad debug is not available on this Windows build.\n");
    exit(1);
#else
    if (!flags || !flags->input_file) {
        fprintf(stderr, "Usage: monad debug <file.mon> [options]\n");
        exit(1);
    }

    DbgConfig cfg = dbg_default_config();
    if (flags->debug_no_mouse)
        cfg.mouse_enabled = false;
    if (flags->debug_truecolor)
        cfg.truecolor_force = true;
    if (flags->debug_target_fps > 0)
        cfg.target_fps = (uint32_t)flags->debug_target_fps;
    if (flags->debug_blink_ms > 0)
        cfg.blink_period_ms = (uint32_t)flags->debug_blink_ms;
    if (flags->debug_blink_count > 0)
        cfg.blink_max_count = (uint32_t)flags->debug_blink_count;

    char emit_ir_command[256] = "monad --emit-ir";
    if (flags->optimization_level > 0) {
        char opt[16];
        snprintf(opt, sizeof(opt), " -O%d", flags->optimization_level);
        strncat(emit_ir_command, opt, sizeof(emit_ir_command) - strlen(emit_ir_command) - 1);
    }
    cfg.emit_ir_command = emit_ir_command;

    char *dbg_argv[] = { "monad debug", flags->input_file, NULL };
    int rc = dbg_main_with_config(2, dbg_argv, cfg);
    exit(rc);
#endif
}

void cmd_lsp(void) {
    extern int lsp_server_main(void);   /* defined in lsp.c */

    if (isatty(STDOUT_FILENO)) {
        /* Interactive terminal: run the REPL instead of just printing */
        int rc = lsp_repl_run();
        exit(rc);
    }

    int rc = lsp_server_main();
    exit(rc);
}

void cmd_test(const CompilerFlags *flags) {
    const char *input_file = flags ? flags->input_file : NULL;
    if (!input_file) {
        const char *suite = (flags && flags->test_suite) ? flags->test_suite : "list";
        if (is_help_word(suite)) {
            print_test_help();
            exit(0);
        }
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "python3 tests/main.py %s", suite);
        int rc = system(cmd);
        exit(host_system_success(rc) ? 0 : 1);
    }

    char self[1024] = "monad";
    {
        char buf[1024] = {0};
        host_self_path(buf, sizeof(buf));
        if (buf[0]) strncpy(self, buf, sizeof(self) - 1);
    }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s --test-run %s%s", self, input_file,
             (flags && flags->verbose_level > 0) ? "" : " --quiet");
    int build_rc = system(cmd);
    if (!host_system_success(build_rc)) {
        fprintf(stderr, "test build failed\n");
        exit(1);
    }

    char *base = get_base_executable_name(input_file);
    char test_bin[1024];
    char test_bin_name[1024];
    snprintf(test_bin_name, sizeof(test_bin_name), "%s_test%s", base, host_exe_suffix());
    snprintf(test_bin, sizeof(test_bin), "./%s", test_bin_name);

    printf("\n");
    int run_rc = system(test_bin);

    remove(test_bin_name);

    char obj[1024];
    snprintf(obj, sizeof(obj), "%s_test.o", base);
    remove(obj);

    if (!host_system_success(run_rc)) {
        int signal = host_system_signal(run_rc);
        if (signal)
            fprintf(stderr, "test failed with signal %d\n", signal);
        else
            fprintf(stderr, "test failed with exit code %d\n", host_system_exit_code(run_rc));

        free(base);
        exit(1);
    }

    printf("\n\x1b[32m✓\x1b[0m %s tests passed\n", base);
    free(base);
    exit(0);
}
