#ifndef ENV_H
#define ENV_H

#include <llvm-c/Core.h>
#include "types.h"

typedef enum {
    ENV_VAR,      // a variable
    ENV_BUILTIN,  // a built-in
    ENV_FUNC,     // a user-defined function
} EnvEntryKind;

typedef struct EnvParam {
    char *name;
    Type *type;
} EnvParam;

typedef struct EnvEntry {
    char *name;
    char *docstring;  // NULL if none
    EnvEntryKind kind;

    // Variables
    Type *type;
    LLVMValueRef value;
    bool is_mutable;

    // Arity  (-1 = variadic)
    int arity_min;
    int arity_max;

    // Functions (ENV_FUNC)
    LLVMValueRef func_ref;
    EnvParam *params; // array of named+typed params
    int param_count;
    Type *return_type;

    struct EnvEntry *next;
} EnvEntry;

typedef struct Env {
    EnvEntry **buckets;
    size_t size;
    size_t count;
} Env;

Env *env_create(void);
void env_free(Env *table);

void env_insert(Env *table, const char *name, Type *type,
                     LLVMValueRef value);
void env_insert_with_doc(Env *table, const char *name, Type *type,
                               LLVMValueRef value, const char *docstring);

void env_insert_builtin(Env *table, const char *name,
                              int arity_min, int arity_max);

void env_insert_func(Env *table, const char *name,
                          EnvParam *params, int param_count,
                          Type *return_type, LLVMValueRef func_ref,
                          const char *docstring);

EnvEntry *env_lookup(Env *table, const char *name);
void env_print(Env *table);

void env_print_entry(EnvEntry *e);

#endif
