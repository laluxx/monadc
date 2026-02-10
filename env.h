#ifndef ENV_H
#define ENV_H

#include <llvm-c/Core.h>
#include "types.h"

/// Symbol Table Entry

typedef struct EnvEntry {
    char *name;
    Type *type;
    LLVMValueRef value;
    bool is_mutable;
    struct EnvEntry *next;
} EnvEntry;

/// Symbol Table

typedef struct Env {
    EnvEntry **buckets;
    size_t size;
    size_t count;
} Env;

/// Symbol table operations

Env *env_create(void);
void env_free(Env *table);
void env_insert(Env *table, const char *name, Type *type, LLVMValueRef value);
EnvEntry *env_lookup(Env *table, const char *name);
void env_print(Env *table);

#endif
