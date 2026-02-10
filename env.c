#include "env.h"
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

Env *env_create(void) {
    Env *table = malloc(sizeof(Env));
    table->size = INITIAL_SIZE;
    table->count = 0;
    table->buckets = calloc(table->size, sizeof(EnvEntry*));
    return table;
}

void env_free(Env *table) {
    if (!table) return;

    for (size_t i = 0; i < table->size; i++) {
        EnvEntry *entry = table->buckets[i];
        while (entry) {
            EnvEntry *next = entry->next;
            free(entry->name);
            type_free(entry->type);
            free(entry);
            entry = next;
        }
    }
    free(table->buckets);
    free(table);
}

void env_insert(Env *table, const char *name, Type *type, LLVMValueRef value) {
    unsigned int index = hash(name) % table->size;

    // Check if already exists and update
    EnvEntry *entry = table->buckets[index];
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
    EnvEntry *new_entry = malloc(sizeof(EnvEntry));
    new_entry->name = strdup(name);
    new_entry->type = type;
    new_entry->value = value;
    new_entry->is_mutable = false;
    new_entry->next = table->buckets[index];
    table->buckets[index] = new_entry;
    table->count++;
}

EnvEntry *env_lookup(Env *table, const char *name) {
    unsigned int index = hash(name) % table->size;
    EnvEntry *entry = table->buckets[index];

    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

void env_print(Env *table) {
    printf("Env (%zu entries):\n", table->count);
    for (size_t i = 0; i < table->size; i++) {
        EnvEntry *entry = table->buckets[i];
        while (entry) {
            printf("  %s :: %s\n", entry->name, type_to_string(entry->type));
            entry = entry->next;
        }
    }
}
