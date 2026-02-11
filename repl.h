#ifndef REPL_H
#define REPL_H

#include <stdbool.h>
#include <llvm-c/ExecutionEngine.h>
#include "env.h"

typedef struct {
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMContextRef context;
    LLVMExecutionEngineRef engine;
    Env *env;

    // Format strings (lazy initialization)
    LLVMValueRef fmt_str;
    LLVMValueRef fmt_char;
    LLVMValueRef fmt_int;
    LLVMValueRef fmt_float;

    // Expression counter for unique naming
    unsigned int expr_count;
} REPLContext;

void repl_init(REPLContext *ctx);
void repl_dispose(REPLContext *ctx);
bool repl_eval_line(REPLContext *ctx, const char *line);
void repl_run(void);
char **repl_completion(const char *text, int start, int end);
char *repl_completion_generator(const char *text, int state);

#endif
