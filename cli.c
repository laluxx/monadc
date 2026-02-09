#include "cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>

void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <file.mon> [options]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o <file>      Output file name (default: input name)\n");
    fprintf(stderr, "  --emit-ir      Emit LLVM IR (.ll)\n");
    fprintf(stderr, "  --emit-bc      Emit LLVM bitcode (.bc)\n");
    fprintf(stderr, "  --emit-asm     Emit assembly (.s)\n");
    fprintf(stderr, "  --emit-obj     Emit object file (.o)\n");
    fprintf(stderr, "Default: emit executable (ELF)\n");
}

CompilerFlags parse_flags(int argc, char **argv) {
    CompilerFlags flags = {0};

    if (argc < 2) {
        print_usage(argv[0]);
        exit(1);
    }

    flags.input_file = argv[1];

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--emit-ir") == 0) {
            flags.emit_ir = true;
        } else if (strcmp(argv[i], "--emit-bc") == 0) {
            flags.emit_bc = true;
        } else if (strcmp(argv[i], "--emit-asm") == 0) {
            flags.emit_asm = true;
        } else if (strcmp(argv[i], "--emit-obj") == 0) {
            flags.emit_obj = true;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "-o requires an argument\n");
                exit(1);
            }
            flags.output_name = argv[++i];
        } else {
            fprintf(stderr, "Unknown flag: %s\n", argv[i]);
            print_usage(argv[0]);
            exit(1);
        }
    }

    return flags;
}

char *get_base_executable_name(const char *path) {
    char *path_copy = strdup(path);
    char *base = basename(path_copy);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    char *result = strdup(base);
    free(path_copy);
    return result;
}
