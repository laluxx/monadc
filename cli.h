#ifndef CLI_H
#define CLI_H

#include <stdbool.h>

/// Compiler Flags

typedef struct {
    bool emit_ir;
    bool emit_bc;
    bool emit_asm;
    bool emit_obj;
    char *output_name;
    char *input_file;
} CompilerFlags;

void print_usage(const char *prog);
CompilerFlags parse_flags(int argc, char **argv);
char *get_base_executable_name(const char *path);

#endif
