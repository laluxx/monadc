#ifndef SYMTABLE_H
#define SYMTABLE_H

#include <llvm-c/Core.h>
#include "types.h"

/// Symbol Table Entry

typedef struct SymbolEntry {
    char *name;
    Type *type;
    LLVMValueRef value;
    bool is_mutable;
    struct SymbolEntry *next;
} SymbolEntry;

/// Symbol Table

typedef struct SymbolTable {
    SymbolEntry **buckets;
    size_t size;
    size_t count;
} SymbolTable;

/// Symbol table operations

SymbolTable *symtable_create(void);
void symtable_free(SymbolTable *table);
void symtable_insert(SymbolTable *table, const char *name, Type *type, LLVMValueRef value);
SymbolEntry *symtable_lookup(SymbolTable *table, const char *name);
void symtable_print(SymbolTable *table);

#endif
