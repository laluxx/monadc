#ifndef REPL_H
#define REPL_H

#include <stdbool.h>
#include <llvm-c/ExecutionEngine.h>
#include "codegen.h"
#include "env.h"
#include "module.h"
#include "infer.h"

typedef struct {
    LLVMExecutionEngineRef engine;      // persistent across expressions
    CodegenContext         cg;          // module/builder replaced per expr
    unsigned int           expr_count;
    InferEnv              *infer_env;   // persistent HM type environment
} REPLContext;

void  repl_init(REPLContext *ctx);
void  repl_dispose(REPLContext *ctx);
bool  repl_eval_line(REPLContext *ctx, const char *line);
void  repl_run(void);

char **repl_completion(const char *text, int start, int end);
char  *repl_completion_generator(const char *text, int state);

/*
 * Implemented in main.c — runs compile_one() on the module named by `imp`,
 * then calls declare_externals() to inject its exports into ctx->env.
 * Returns true on success.
 */
bool repl_compile_module(CodegenContext *ctx, ImportDecl *imp);
const char **repl_get_compiled_obj_paths(void);

#endif
