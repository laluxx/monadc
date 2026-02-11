#include "env.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define INITIAL_SIZE 16

static unsigned int hash(const char *str) {
    unsigned int h = 5381;
    int c;
    while ((c = *str++))
        h = ((h << 5) + h) + c;
    return h;
}

Env *env_create(void) {
    Env *t    = malloc(sizeof(Env));
    t->size   = INITIAL_SIZE;
    t->count  = 0;
    t->buckets = calloc(t->size, sizeof(EnvEntry *));
    return t;
}

static void free_entry_fields(EnvEntry *e) {
    free(e->name);
    free(e->docstring);
    type_free(e->type);
    type_free(e->return_type);
    if (e->params) {
        for (int i = 0; i < e->param_count; i++) {
            free(e->params[i].name);
            type_free(e->params[i].type);
        }
        free(e->params);
    }
}

void env_free(Env *table) {
    if (!table) return;
    for (size_t i = 0; i < table->size; i++) {
        EnvEntry *e = table->buckets[i];
        while (e) {
            EnvEntry *next = e->next;
            free_entry_fields(e);
            free(e);
            e = next;
        }
    }
    free(table->buckets);
    free(table);
}

static EnvEntry *find(Env *table, const char *name) {
    unsigned int idx = hash(name) % table->size;
    EnvEntry *e = table->buckets[idx];
    while (e) {
        if (strcmp(e->name, name) == 0) return e;
        e = e->next;
    }
    return NULL;
}

static EnvEntry *new_entry(const char *name) {
    EnvEntry *e  = calloc(1, sizeof(EnvEntry));
    e->name      = strdup(name);
    e->arity_min = -1;
    e->arity_max = -1;
    return e;
}

static void chain(Env *table, EnvEntry *e) {
    unsigned int idx = hash(e->name) % table->size;
    e->next = table->buckets[idx];
    table->buckets[idx] = e;
    table->count++;
}

void env_insert(Env *table, const char *name, Type *type, LLVMValueRef value) {
    env_insert_with_doc(table, name, type, value, NULL);
}

void env_insert_with_doc(Env *table, const char *name, Type *type,
                          LLVMValueRef value, const char *docstring) {
    EnvEntry *e = find(table, name);
    if (e) {
        type_free(e->type);
        free(e->docstring);
        e->kind      = ENV_VAR;
        e->type      = type;
        e->value     = value;
        e->docstring = docstring ? strdup(docstring) : NULL;
        return;
    }
    e = new_entry(name);
    e->kind      = ENV_VAR;
    e->type      = type;
    e->value     = value;
    e->docstring = docstring ? strdup(docstring) : NULL;
    chain(table, e);
}

void env_insert_builtin(Env *table, const char *name,
                         int arity_min, int arity_max) {
    EnvEntry *e = find(table, name);
    if (e) {
        e->kind      = ENV_BUILTIN;
        e->arity_min = arity_min;
        e->arity_max = arity_max;
        return;
    }
    e = new_entry(name);
    e->kind      = ENV_BUILTIN;
    e->arity_min = arity_min;
    e->arity_max = arity_max;
    chain(table, e);
}

void env_insert_func(Env *table, const char *name,
                     EnvParam *params, int param_count,
                     Type *return_type, LLVMValueRef func_ref,
                     const char *docstring) {
    EnvEntry *e = find(table, name);
    if (e) {
        free_entry_fields(e);
        e->name = strdup(name);
    } else {
        e = new_entry(name);
        chain(table, e);
    }
    e->kind        = ENV_FUNC;
    e->params      = params;
    e->param_count = param_count;
    e->return_type = return_type;
    e->func_ref    = func_ref;
    e->arity_min   = param_count;
    e->arity_max   = param_count;
    e->docstring   = docstring ? strdup(docstring) : NULL;
    e->type        = type_fn(NULL, 0, NULL); // placeholder Fn type
}

EnvEntry *env_lookup(Env *table, const char *name) {
    return find(table, name);
}

/// env_print_entry  — the Scheme-style arity display

void env_print_entry(EnvEntry *e) {
    switch (e->kind) {

    case ENV_VAR:
        printf("[%s :: %s]", e->name,
               e->type ? type_to_string(e->type) : "?");
        if (e->docstring)
            printf("  ; %s", e->docstring);
        printf("\n");
        break;

    case ENV_BUILTIN: {
        // Build arity signature from arity_min / arity_max
        // arity_max == -1  means variadic
        char sig[256] = {0};

        if (e->arity_min <= 0 && e->arity_max == -1) {
            // fully variadic: list-style  Fn _
            strcpy(sig, "_");
        } else {
            // required args
            for (int i = 0; i < e->arity_min; i++) {
                if (i > 0) strcat(sig, " ");
                strcat(sig, "_");
            }
            if (e->arity_max == -1) {
                // has required args + variadic rest
                if (e->arity_min > 0) strcat(sig, " ");
                strcat(sig, ". _");
            } else if (e->arity_max > e->arity_min) {
                // optional args
                strcat(sig, " #:optional");
                for (int i = e->arity_min; i < e->arity_max; i++)
                    strcat(sig, " _");
            }
        }

        printf("[%s :: Fn (%s)]", e->name, sig);
        if (e->docstring) printf("  ; %s", e->docstring);
        printf("\n");
        break;
    }

    case ENV_FUNC: {
        char sig[256] = {0};
        for (int i = 0; i < e->param_count; i++) {
            if (i > 0) strcat(sig, " ");
            if (e->params[i].name) {
                strcat(sig, e->params[i].name);
            } else {
                strcat(sig, "_");
            }
        }
        // append return type
        const char *ret = e->return_type ? type_to_string(e->return_type) : "?";
        printf("[%s :: Fn (%s) -> %s]", e->name, sig, ret);
        if (e->docstring) printf("  ; %s", e->docstring);
        printf("\n");
        break;
    }
    }
}

void env_print(Env *table) {
    printf("Env (%zu entries):\n", table->count);
    for (size_t i = 0; i < table->size; i++) {
        EnvEntry *e = table->buckets[i];
        while (e) {
            printf("  ");
            env_print_entry(e);
            e = e->next;
        }
    }
}
