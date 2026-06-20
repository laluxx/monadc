#ifndef CLI_H
#define CLI_H
#include <stdbool.h>

typedef enum {
    CMD_COMPILE,
    CMD_REPL,
    CMD_NEW,
    CMD_BUILD,
    CMD_RUN,
    CMD_CLEAN,
    CMD_INSTALL,
    CMD_TEST,
    CMD_CHECK,
    CMD_LSP,
    CMD_EVAL,
    CMD_DEBUG,
} CommandMode;

typedef struct {
    CommandMode mode;
    bool emit_ir;
    bool emit_bc;
    bool emit_asm;
    bool emit_obj;
    bool emit_json;
    bool emit_typst;
    bool emit_bytecode;
    bool bytecode_verify;
    bool bytecode_disassemble;
    bool bytecode_decompile;
    bool bytecode_dump_sections;
    bool bytecode_trace;
    bool bytecode_baseline_jit;
    bool jit;
    int optimization_level;
    int verbose_level;
    bool trace_ast;
    bool trace_semantic;
    bool trace_dep;
    bool trace_codegen;
    bool debug_no_mouse;
    bool debug_truecolor;
    int debug_target_fps;
    int debug_blink_ms;
    int debug_blink_count;
    bool start_repl;    bool test_mode;      // emit test blocks
    bool test_run;       // run and delete test binary (monad test)
    char *output_name;
    char *input_file;
    char *eval_code;
    char *package_name;
} CompilerFlags;

/* print_usage is now defined in completion.c */
CompilerFlags parse_flags(int argc, char **argv);
char *get_base_executable_name(const char *path);

void cmd_new(const char *package_name);
void cmd_build(const CompilerFlags *flags);
void cmd_run(const CompilerFlags *flags);
void cmd_clean(void);
void cmd_install(void);
void cmd_test(const char *input_file);
void cmd_check(const char *input_file);
void cmd_lsp(void);
void cmd_eval(const char *code);
void cmd_debug(const CompilerFlags *flags);
#endif
