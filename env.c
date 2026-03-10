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
    Env *t     = malloc(sizeof(Env));
    t->size    = INITIAL_SIZE;
    t->count   = 0;
    t->buckets = calloc(t->size, sizeof(EnvEntry *));
    t->parent  = NULL;  // add this
    return t;
}

Env *env_create_child(Env *parent) {
    Env *t    = env_create();
    t->parent = parent;
    return t;
}

static void free_entry_fields(EnvEntry *e) {
    free(e->name);
    free(e->docstring);
    free(e->module_name);
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
    /* Check for duplicate name in this bucket before inserting */
    for (EnvEntry *existing = table->buckets[idx]; existing; existing = existing->next) {
        if (strcmp(existing->name, e->name) == 0) {
            /* Name already exists — free the new entry and return */
            free_entry_fields(e);
            free(e);
            return;
        }
    }
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
        free(e->module_name);
        e->kind      = ENV_VAR;
        e->type      = type;
        e->value     = value;
        e->docstring = docstring ? strdup(docstring) : NULL;
        e->module_name = NULL;
        e->is_exported = true; //  local symbols are "exported" by default
        return;
    }
    e = new_entry(name);
    e->kind      = ENV_VAR;
    e->type      = type;
    e->value     = value;
    e->docstring = docstring ? strdup(docstring) : NULL;
    e->module_name = NULL;
    e->is_exported = true;
    chain(table, e);
}

void env_insert_from_module(Env *table, const char *name, const char *module_name,
                            Type *type, LLVMValueRef value, bool is_exported) {
    EnvEntry *e = find(table, name);
    if (e) {
        type_free(e->type);
        free(e->docstring);
        free(e->module_name);
        e->kind      = ENV_VAR;
        e->type      = type;
        e->value     = value;
        e->module_name = module_name ? strdup(module_name) : NULL;
        e->is_exported = is_exported;
        return;
    }
    e = new_entry(name);
    e->kind      = ENV_VAR;
    e->type      = type;
    e->value     = value;
    e->module_name = module_name ? strdup(module_name) : NULL;
    e->is_exported = is_exported;
    chain(table, e);
}

void env_insert_builtin(Env *table, const char *name,
                         int arity_min, int arity_max,
                         const char *docstring) {
    EnvEntry *e = find(table, name);
    if (e) {
        e->kind      = ENV_BUILTIN;
        e->arity_min = arity_min;
        e->arity_max = arity_max;
        free(e->docstring);
        e->docstring = docstring ? strdup(docstring) : NULL;
        return;
    }
    e = new_entry(name);
    e->kind      = ENV_BUILTIN;
    e->arity_min = arity_min;
    e->arity_max = arity_max;
    e->docstring = docstring ? strdup(docstring) : NULL;
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
    e->type        = type_fn(NULL, 0, NULL);
    e->module_name = NULL;
    e->is_exported = true;
}

EnvEntry *env_lookup(Env *table, const char *name) {
    while (table) {
        EnvEntry *e = find(table, name);
        if (e) return e;
        table = table->parent;
    }
    return NULL;
}

static void build_bracket(EnvEntry *e, char *buf, size_t sz) {
    char sig[256] = {0};

    /* Name: if the name already starts with "ModuleName." don't prepend module again */
    char name_buf[256];
    if (e->module_name) {
        /* Check if name is already prefixed with "ModuleName." */
        size_t mlen = strlen(e->module_name);
        if (strncmp(e->name, e->module_name, mlen) == 0 && e->name[mlen] == '.') {
            /* name is already "Module.foo" — just use it as-is */
            snprintf(name_buf, sizeof(name_buf), "%s", e->name);
        } else {
            snprintf(name_buf, sizeof(name_buf), "%s.%s", e->module_name, e->name);
        }
    } else {
        snprintf(name_buf, sizeof(name_buf), "%s", e->name);
    }

    switch (e->kind) {

    case ENV_VAR:
        snprintf(buf, sz, "[%s :: %s]",
                 name_buf,
                 e->type ? type_to_string(e->type) : "?");
        return;

    case ENV_BUILTIN: {
        int mn = e->arity_min > 0 ? e->arity_min : 0;
        int mx = e->arity_max;
        if (mn == 0 && mx == -1) {
            snprintf(buf, sz, "[%s :: Fn _]", name_buf);
            return;
        }
        for (int i = 0; i < mn; i++) {
            if (i > 0) strncat(sig, " ", sizeof(sig) - strlen(sig) - 1);
            strncat(sig, "_", sizeof(sig) - strlen(sig) - 1);
        }
        if (mx > mn && mx != -1) {
            strncat(sig, mn > 0 ? " =>" : "=>", sizeof(sig) - strlen(sig) - 1);
            for (int i = mn; i < mx; i++)
                strncat(sig, " _", sizeof(sig) - strlen(sig) - 1);
        }
        if (mx == -1 && mn > 0)
            strncat(sig, " => _ . _", sizeof(sig) - strlen(sig) - 1);
        snprintf(buf, sz, "[%s :: Fn (%s)]", name_buf, sig);
        return;
    }

    case ENV_FUNC: {
        for (int i = 0; i < e->param_count; i++) {
            if (i > 0) strncat(sig, " ", sizeof(sig) - strlen(sig) - 1);
            strncat(sig, (e->params[i].name && e->params[i].name[0])
                         ? e->params[i].name : "_",
                    sizeof(sig) - strlen(sig) - 1);
        }
        const char *ret = e->return_type ? type_to_string(e->return_type) : "?";
        if (sig[0])
            snprintf(buf, sz, "[%s :: Fn (%s) -> %s]", name_buf, sig, ret);
        else
            snprintf(buf, sz, "[%s :: Fn -> %s]", name_buf, ret);
        return;
    }
    }
}

static int cmp_env_entry(const void *a, const void *b) {
    EnvEntry *ea = *(EnvEntry **)a;
    EnvEntry *eb = *(EnvEntry **)b;
    if (ea->kind == ENV_BUILTIN && eb->kind != ENV_BUILTIN) return  1;
    if (ea->kind != ENV_BUILTIN && eb->kind == ENV_BUILTIN) return -1;
    int ma = ea->module_name ? 1 : 0;
    int mb = eb->module_name ? 1 : 0;
    if (ma != mb) return ma - mb;
    if (ea->module_name && eb->module_name) {
        int mc = strcmp(ea->module_name, eb->module_name);
        if (mc) return mc;
    }
    return strcmp(ea->name, eb->name);
}

void env_print(Env *table) {
    /* Collect entries, skipping unqualified aliases for module symbols.
     * declare_externals inserts both "Module.foo" (qualified) and "foo"
     * (unqualified) for non-qualified imports. We only display the
     * qualified form so each symbol appears exactly once.              */
    int total = 0;
    for (size_t i = 0; i < table->size; i++)
        for (EnvEntry *e = table->buckets[i]; e; e = e->next)
            if (e->name) total++;

    if (total == 0) { printf("  (empty)\n"); return; }

    EnvEntry **entries = malloc(total * sizeof(EnvEntry *));
    int n = 0;
    for (size_t i = 0; i < table->size; i++) {
        for (EnvEntry *e = table->buckets[i]; e; e = e->next) {
            if (!e->name) continue;
            /* Skip unqualified aliases: module entries whose name does
             * not start with "ModuleName." are bare-name copies kept
             * only for unqualified lookup — don't display them.        */
            if (e->module_name) {
                size_t mlen = strlen(e->module_name);
                if (!(strncmp(e->name, e->module_name, mlen) == 0
                      && e->name[mlen] == '.'))
                    continue;
            }
            entries[n++] = e;
        }
    }

    if (n == 0) { printf("  (empty)\n"); free(entries); return; }

    qsort(entries, n, sizeof(EnvEntry *), cmp_env_entry);

    /* Build all bracket strings */
    char **brackets = malloc(n * sizeof(char *));
    for (int i = 0; i < n; i++) {
        brackets[i] = malloc(512);
        build_bracket(entries[i], brackets[i], 512);
    }

    /* Print section by section, computing max_w per section */
    int i = 0;
    while (i < n) {
        int         section_start = i;
        const char *sec_mod       = entries[i]->module_name;
        int         sec_kind      = entries[i]->kind;

        int j = i;
        while (j < n) {
            EnvEntry *ej = entries[j];
            bool same = (sec_kind == ENV_BUILTIN)
                        ? (ej->kind == ENV_BUILTIN)
                        : (ej->kind != ENV_BUILTIN &&
                           ((sec_mod == NULL && ej->module_name == NULL) ||
                            (sec_mod && ej->module_name &&
                             strcmp(sec_mod, ej->module_name) == 0)));
            if (!same) break;
            j++;
        }
        int section_end = j;

        /* Max bracket width for alignment within this section */
        size_t max_w = 0;
        for (int k = section_start; k < section_end; k++) {
            size_t w = strlen(brackets[k]);
            if (w > max_w) max_w = w;
        }

        /* Section header */
        if (sec_kind == ENV_BUILTIN)
            printf("\n  \033[2m-- builtins --\033[0m\n");
        else if (sec_mod)
            printf("\n  \033[34m-- %s --\033[0m\n", sec_mod);
        else
            printf("\n  \033[2m-- local --\033[0m\n");

        /* Print entries */
        for (int k = section_start; k < section_end; k++) {
            EnvEntry *e = entries[k];
            if (e->docstring && e->docstring[0])
                printf("  %-*s  \"%s\"\n", (int)max_w, brackets[k], e->docstring);
            else
                printf("  %s\n", brackets[k]);
            free(brackets[k]);
        }

        i = section_end;
    }

    printf("\n  \033[2m%d bindings\033[0m\n", n);
    free(brackets);
    free(entries);
}
