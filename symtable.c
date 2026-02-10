#include "symtable.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define INITIAL_SIZE 16

static unsigned int hash(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

SymbolTable *symtable_create(void) {
    SymbolTable *table = malloc(sizeof(SymbolTable));
    table->size = INITIAL_SIZE;
    table->count = 0;
    table->buckets = calloc(table->size, sizeof(SymbolEntry*));
    return table;
}

void symtable_free(SymbolTable *table) {
    if (!table) return;

    for (size_t i = 0; i < table->size; i++) {
        SymbolEntry *entry = table->buckets[i];
        while (entry) {
            SymbolEntry *next = entry->next;
            free(entry->name);
            type_free(entry->type);
            free(entry);
            entry = next;
        }
    }
    free(table->buckets);
    free(table);
}

void symtable_insert(SymbolTable *table, const char *name, Type *type, LLVMValueRef value) {
    unsigned int index = hash(name) % table->size;

    // Check if already exists and update
    SymbolEntry *entry = table->buckets[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            // Update existing entry
            type_free(entry->type);
            entry->type = type;
            entry->value = value;
            return;
        }
        entry = entry->next;
    }

    // Create new entry
    SymbolEntry *new_entry = malloc(sizeof(SymbolEntry));
    new_entry->name = strdup(name);
    new_entry->type = type;
    new_entry->value = value;
    new_entry->is_mutable = false;
    new_entry->next = table->buckets[index];
    table->buckets[index] = new_entry;
    table->count++;
}

SymbolEntry *symtable_lookup(SymbolTable *table, const char *name) {
    unsigned int index = hash(name) % table->size;
    SymbolEntry *entry = table->buckets[index];

    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

void symtable_print(SymbolTable *table) {
    printf("Symbol Table (%zu entries):\n", table->count);
    for (size_t i = 0; i < table->size; i++) {
        SymbolEntry *entry = table->buckets[i];
        while (entry) {
            printf("  %s :: %s\n", entry->name, type_to_string(entry->type));
            entry = entry->next;
        }
    }
}
