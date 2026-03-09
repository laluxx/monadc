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
} CommandMode;

typedef struct {
    CommandMode mode;
    bool emit_ir;
    bool emit_bc;
    bool emit_asm;
    bool emit_obj;
    bool start_repl;
    char *output_name;
    char *input_file;
    char *package_name;
} CompilerFlags;

void print_usage(const char *prog);
CompilerFlags parse_flags(int argc, char **argv);
char *get_base_executable_name(const char *path);

void cmd_new(const char *package_name);
void cmd_build(void);
void cmd_run(void);
void cmd_clean(void);
void cmd_install(void);

#endif
