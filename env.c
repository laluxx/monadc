#include "env.h"
#include "infer.h"
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
    t->parent  = NULL;
    t->infer_env = NULL;
    return t;
}

Env *env_create_child(Env *parent) {
    Env *t    = env_create();
    t->parent = parent;
    return t;
}

static void free_entry_fields(EnvEntry *e) {
    free(e->name);          e->name        = NULL;
    free(e->docstring);     e->docstring   = NULL;
    free(e->module_name);   e->module_name = NULL;
    free(e->source_text);   e->source_text = NULL;
    free(e->llvm_name);     e->llvm_name   = NULL;
    type_free(e->type);     e->type        = NULL;
    type_free(e->return_type); e->return_type = NULL;
    if (e->scheme) {
        scheme_free(e->scheme);
        e->scheme = NULL;
    }
    if (e->params) {
        for (int i = 0; i < e->param_count; i++) {
            free(e->params[i].name);
            e->params[i].name = NULL;
            type_free(e->params[i].type);
            e->params[i].type = NULL;
        }
        free(e->params);
        e->params = NULL;
    }
    /* source_ast is owned by the define path and freed separately — do not free here */
    /* llvm_name already freed above */
}

// TODO Free llvm_name
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
    if (table->infer_env) {
        infer_env_free(table->infer_env);
        table->infer_env = NULL;
    }
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

bool env_is_local(Env *table, const char *name) {
    while (table) {
        EnvEntry *e = find(table, name);
        if (e) {
            if (!e->value) return (table->parent != NULL);
            return LLVMGetValueKind(e->value) != LLVMGlobalVariableValueKind;
        }
        table = table->parent;
    }
    return false;
}

static EnvEntry *new_entry(const char *name) {
    EnvEntry *e  = calloc(1, sizeof(EnvEntry));
    e->name      = strdup(name);
    e->arity_min = -1;
    e->arity_max = -1;
    e->adt_tag   = -1;
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

void env_insert_layout(Env *table, const char *name, Type *layout_type,
                       const char *source_text) {
    EnvEntry *e = find(table, name);
    if (e) {
        type_free(e->type);
        free(e->source_text);
        e->kind        = ENV_LAYOUT;
        e->type        = layout_type;
        e->source_text = source_text ? strdup(source_text) : NULL;
        e->is_exported = true;
        return;
    }
    e = new_entry(name);
    e->kind        = ENV_LAYOUT;
    e->type        = layout_type;
    e->source_text = source_text ? strdup(source_text) : NULL;
    e->is_exported = true;
    chain(table, e);
}

Type *env_lookup_layout(Env *table, const char *name) {
    EnvEntry *e = env_lookup(table, name);
    if (e && e->kind == ENV_LAYOUT) return e->type;
    return NULL;
}

Type *env_find_layout_with_field(Env *table, const char *field_name) {
    for (Env *e = table; e; e = e->parent) {
        for (size_t i = 0; i < e->size; i++) {
            for (EnvEntry *entry = e->buckets[i]; entry; entry = entry->next) {
                if (entry->kind != ENV_LAYOUT || !entry->type) continue;
                Type *t = entry->type;
                for (int f = 0; f < t->layout_field_count; f++) {
                    if (strcmp(t->layout_fields[f].name, field_name) == 0)
                        return t;
                }
            }
        }
    }
    return NULL;
}

void env_insert_adt_ctor(Env *table, const char *name, int tag,
                         Type *data_type, LLVMValueRef func_ref) {
    EnvEntry *e = find(table, name);
    if (e) {
        type_free(e->type);
        e->kind     = ENV_ADT_CTOR;
        e->adt_tag  = tag;
        e->type     = data_type;
        e->func_ref = func_ref;
        e->is_exported = true;
        return;
    }
    e = new_entry(name);
    e->kind     = ENV_ADT_CTOR;
    e->adt_tag  = tag;
    e->type     = data_type;
    e->func_ref = func_ref;
    e->is_exported = true;
    chain(table, e);
}

EnvEntry *env_lookup_adt_ctor(Env *table, const char *name) {
    EnvEntry *e = env_lookup(table, name);
    if (e && e->kind == ENV_ADT_CTOR) return e;
    return NULL;
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
        char *saved_source = e->source_text;
        e->source_text = NULL;
        type_free(e->type);
        free(e->docstring);
        free(e->module_name);
        e->kind        = ENV_VAR;
        e->type        = type;
        e->value       = value;
        e->module_name = module_name ? strdup(module_name) : NULL;
        e->is_exported = is_exported;
        e->source_text = saved_source;
        return;
    }
    e = new_entry(name);
    e->kind        = ENV_VAR;
    e->type        = type;
    e->value       = value;
    e->module_name = module_name ? strdup(module_name) : NULL;
    e->is_exported = is_exported;
    chain(table, e);
}

void env_insert_builtin(Env *table, const char *name,
                         int arity_min, int arity_max,
                         const char *docstring,
                         const ParamKind *param_kinds) {
    EnvEntry *e = find(table, name);
    if (e) {
        e->kind      = ENV_BUILTIN;
        e->arity_min = arity_min;
        e->arity_max = arity_max;
        free(e->docstring);
        e->docstring = docstring ? strdup(docstring) : NULL;
    } else {
        e = new_entry(name);
        e->kind      = ENV_BUILTIN;
        e->arity_min = arity_min;
        e->arity_max = arity_max;
        e->docstring = docstring ? strdup(docstring) : NULL;
        chain(table, e);
    }
    memset(e->param_kinds, PARAM_VALUE, sizeof(e->param_kinds));
    if (param_kinds) {
        int n = arity_max > 0 ? arity_max : arity_min;
        for (int i = 0; i < n && i < WISP_MAX_PARAMS; i++)
            e->param_kinds[i] = param_kinds[i];
    }
}

void env_insert_func(Env *table, const char *name,
                     EnvParam *params, int param_count,
                     Type *return_type, LLVMValueRef func_ref,
                     const char *docstring,
                     const ParamKind *param_kinds) {
    EnvEntry *e = find(table, name);
    if (e) {
        char *saved_source  = e->source_text;
        char *saved_llvm    = e->llvm_name;
        AST  *saved_ast     = e->source_ast;
        TypeScheme *saved_scheme = e->scheme;
        bool saved_is_ffi   = e->is_ffi;
        e->source_text = NULL;
        e->llvm_name   = NULL;
        e->source_ast  = NULL;
        e->scheme      = NULL;
        free_entry_fields(e);
        e->name        = strdup(name);
        e->source_text = saved_source;
        e->llvm_name   = saved_llvm;
        e->source_ast  = saved_ast;
        e->scheme      = saved_scheme;
        e->is_ffi      = saved_is_ffi;
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
    memset(e->param_kinds, PARAM_VALUE, sizeof(e->param_kinds));
    if (param_kinds) {
        for (int i = 0; i < param_count && i < WISP_MAX_PARAMS; i++)
            e->param_kinds[i] = param_kinds[i];
    }
}

EnvEntry *env_lookup(Env *table, const char *name) {
    while (table) {
        EnvEntry *e = find(table, name);
        if (e) return e;
        table = table->parent;
    }
    return NULL;
}

void env_remove(Env *table, const char *name) {
    unsigned int idx = hash(name) % table->size;
    EnvEntry **pp = &table->buckets[idx];
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            EnvEntry *dead = *pp;
            *pp = dead->next;
            free_entry_fields(dead);
            free(dead);
            table->count--;
            return;
        }
        pp = &(*pp)->next;
    }
    /* Not in this table — check parent */
    if (table->parent) env_remove(table->parent, name);
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

    case ENV_LAYOUT: {
        int field_count = e->type ? e->type->layout_field_count : 0;
        snprintf(buf, sz, "[%s :: Layout (%d fields)]", name_buf, field_count);
        return;
    }

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

/// HM integration

static struct DepCtx *env_get_dep(Env *env) {
    while (env) {
        if (env->dep_ctx) return env->dep_ctx;
        env = env->parent;
    }
    return NULL;
}

void env_init_infer(Env *root) {
    if (root->infer_env) return;   /* idempotent */
    root->infer_env = infer_env_create();
    /* Bootstrap built-in type schemes into the infer env */
    InferCtx *bctx = infer_ctx_create(root->infer_env, env_get_dep(root), "<builtins>");
    infer_register_builtins(bctx);
    infer_ctx_free(bctx);
}

struct InferEnv *env_get_infer(Env *env) {
    while (env) {
        if (env->infer_env) return env->infer_env;
        env = env->parent;
    }
    return NULL;
}

void env_set_scheme(Env *env, const char *name, struct TypeScheme *scheme) {
    EnvEntry *e = env_lookup(env, name);
    if (!e) return;
    if (e->scheme) {
        scheme_free(e->scheme);
        e->scheme = NULL;
    }
    e->scheme = scheme ? scheme_clone(scheme) : NULL;
}

static void normalize_vars(Type *t, int *map, int *next_id) {
    if (!t) return;
    if (t->kind == TYPE_VAR) {
        if (t->var_id >= 2000) return;
        for (int i=0; i<*next_id; i++) {
            if (map[i*2] == t->var_id) {
                t->var_id = map[i*2 + 1];
                return;
            }
        }
        map[*next_id * 2] = t->var_id;
        t->var_id = 2000 + *next_id;
        map[*next_id * 2 + 1] = t->var_id;
        (*next_id)++;
        return;
    }
    if (t->kind == TYPE_ARROW) {
        normalize_vars(t->arrow_param, map, next_id);
        normalize_vars(t->arrow_ret, map, next_id);
    }
    if (t->kind == TYPE_LIST) {
        for (int i=0; i<t->list_count; i++) normalize_vars(t->list_types[i], map, next_id);
    }
    if (t->kind == TYPE_COLL || t->kind == TYPE_PTR || t->kind == TYPE_OPTIONAL) {
        normalize_vars(t->element_type, map, next_id);
    }
    if (t->kind == TYPE_ARR) {
        normalize_vars(t->arr_element_type, map, next_id);
    }
}

struct TypeScheme *env_hm_infer_define(Env *env, const char *name,
                                       AST *lambda_ast, const char *filename) {
    InferEnv *ienv = env_get_infer(env);
    if (!ienv) return NULL;

    InferEnv *child = infer_env_create_child(ienv);
    InferCtx *ctx   = infer_ctx_create(child, env_get_dep(env), filename ? filename : "<unknown>");
    /* Pre-bind with a fresh var so recursive calls resolve.
     * We use a high ID so it doesn't pollute the visible var names. */
    ctx->subst->next_id = 1000;
    Type *self_t = infer_fresh(ctx);
    ctx->subst->next_id = 0;  /* reset so params get 'a, 'b, 'c... */
    infer_env_insert(child, name, scheme_mono(self_t));
    Type *inferred = infer_toplevel(ctx, lambda_ast);

    TypeScheme *scheme = NULL;

    if (ctx->had_error) {
        Type *sig = NULL;
        if (lambda_ast && lambda_ast->type == AST_LAMBDA && lambda_ast->lambda.return_type) {
            sig = type_from_name(lambda_ast->lambda.return_type);
            if (!sig) sig = type_unknown();
            for (int i = lambda_ast->lambda.param_count - 1; i >= 0; i--) {
                Type *p = NULL;
                if (lambda_ast->lambda.params[i].type_name) {
                    p = type_from_name(lambda_ast->lambda.params[i].type_name);
                }
                if (!p) p = type_unknown();
                sig = type_arrow(p, sig);
            }
        }

        if (inferred && inferred->kind == TYPE_ARROW && sig && sig->kind == TYPE_ARROW) {
            Type *inf_ret = inferred; while(inf_ret->kind == TYPE_ARROW) inf_ret = inf_ret->arrow_ret;
            Type *sig_ret = sig; while(sig_ret->kind == TYPE_ARROW) sig_ret = sig_ret->arrow_ret;
            if (inf_ret && inf_ret->kind == TYPE_COLL && sig_ret && sig_ret->kind == TYPE_LIST) {
                Type *wrap = type_coll();
                wrap->element_type = sig_ret;
                Type *cur = sig;
                while (cur->arrow_ret->kind == TYPE_ARROW) cur = cur->arrow_ret;
                cur->arrow_ret = wrap;
            }
        }

        int vmap[128];
        int next_id = 0;
        normalize_vars(inferred, vmap, &next_id);

        char *core_msg = strstr(ctx->error_msg, "type error: ");
        if (core_msg) core_msg += 12; /* skip "type error: " */
        else core_msg = ctx->error_msg;

        if (sig) {
            READER_ERROR(lambda_ast->line, lambda_ast->column,
                "\n"
                "    • Type inference failed for function ‘%s’\n"
                "    • Expected signature :: %s\n"
                "    • Provided signature :: %s\n"
                "   - Hint: %s",
                name, type_to_string(inferred), type_to_string(sig), core_msg);
        } else {
            READER_ERROR(lambda_ast->line, lambda_ast->column,
                "\n"
                "    • Type inference failed for function ‘%s’\n"
                "    • Expected signature :: %s\n"
                "   - Hint: %s",
                name, type_to_string(inferred), core_msg);
        }
    } else {
        infer_unify_one(ctx, self_t, inferred, lambda_ast->line, lambda_ast->column);

        EnvEntry *ee = env_lookup(env, name);
        if (ee) {
            /* Skip signature check for instance method implementations.
             * Instance methods are registered with params that have no
             * type annotations (type_name = NULL) because the class type
             * variable (e.g. 'm') has not been substituted with the
             * concrete type (e.g. 'Maybe'). Attempting to build a sig
             * from NULL param types produces a chain of fresh vars that
             * cannot meaningfully constrain the inferred type, and any
             * mismatch error message would be misleading. The dep checker
             * already verified the instance body is well-typed. */
            bool is_instance_method = (strncmp(name, "__impl_", 7) == 0);

            if (!is_instance_method) {
                /* Build expected signature from registered param types */
                bool has_concrete_params = true;
                for (int i = 0; i < ee->param_count; i++) {
                    if (!ee->params[i].type) { has_concrete_params = false; break; }
                    if (ee->params[i].type->kind == TYPE_UNKNOWN) { has_concrete_params = false; break; }
                }

                if (has_concrete_params || ee->return_type) {
                    Type *sig = ee->return_type ? type_clone(ee->return_type) : infer_fresh(ctx);
                    for (int i = ee->param_count - 1; i >= 0; i--) {
                        Type *p = ee->params[i].type ? type_clone(ee->params[i].type) : infer_fresh(ctx);
                        sig = type_arrow(p, sig);
                    }

                    TypeScheme *sig_sc = infer_generalise(ctx, sig, ienv);
                    Type *inst_sig = infer_instantiate(ctx, sig_sc);
                    scheme_free(sig_sc);

                    if (!infer_unify_one(ctx, inferred, inst_sig, lambda_ast->line, lambda_ast->column)) {
                        Type *show_exp = subst_apply(ctx->subst, inst_sig);
                        Type *show_inf = subst_apply(ctx->subst, inferred);
                        READER_ERROR(lambda_ast->line, lambda_ast->column,
                            "\n"
                            "    • Signature mismatch for definition '%s'\n"
                            "    • Expected type: %s\n"
                            "    • Inferred type: %s\n"
                            "    • Details: %s",
                            name, type_to_string(show_exp), type_to_string(show_inf), ctx->error_msg);
                    }
                }
            }
        }

        /* Fully apply substitution so the scheme's type nodes are concrete
         * ground types — no TYPE_VAR nodes that reference the now-dead ctx */
        scheme = infer_generalise(ctx, inferred, ienv);
        scheme->type = subst_apply(ctx->subst, scheme->type);
        {
            Type *t = scheme->type;
            while (t && t->kind == TYPE_ARROW) {
                t->arrow_param = subst_apply(ctx->subst, t->arrow_param);
                t->arrow_ret   = subst_apply(ctx->subst, t->arrow_ret);
                t = t->arrow_ret;
            }
        }

        /* Sanity check: if HM poisoned the first param to an arrow type
         * EnvEntry gets its own clone so free_entry_fields() can
         * safely call scheme_free() without double-freeing.        */
        infer_env_insert(ienv, name, scheme);
        /* with '?' but the inferred return type resolves to Nil or
         * Optional, the function can return nil without declaring it.
         * Force the user to annotate the return type with '?'.        */
        if (lambda_ast && lambda_ast->type == AST_LAMBDA &&
            lambda_ast->lambda.return_type) {
            const char *decl_ret = lambda_ast->lambda.return_type;
            size_t decl_len = strlen(decl_ret);
            bool decl_is_optional = (decl_len > 0 && decl_ret[decl_len - 1] == '?');

            if (!decl_is_optional) {
                /* Walk the inferred arrow chain to find the return type */
                Type *inf_ret = subst_apply(ctx->subst, inferred);
                while (inf_ret && inf_ret->kind == TYPE_ARROW)
                    inf_ret = inf_ret->arrow_ret;
                inf_ret = subst_apply(ctx->subst, inf_ret);

                if (inf_ret && (inf_ret->kind == TYPE_NIL ||
                                inf_ret->kind == TYPE_OPTIONAL)) {
                    READER_ERROR(lambda_ast->line, lambda_ast->column,
                        "\n"
                        "    • Function ‘%s’ can return nil but return type '%s' does not end with ‘?’\n"
                        "  - Hint: change the return type to '%s?' to allow nil returns",
                        name, decl_ret, decl_ret);
                }
            }
        }

        infer_env_insert(ienv, name, scheme);
        env_set_scheme(env, name, scheme_clone(scheme));
    }

    infer_ctx_free(ctx);
    infer_env_free(child);
    return scheme;
}

/* Return the element type of a collection type, or NULL if not a collection. */
static Type *collection_element_type(Type *t) {
    if (!t) return NULL;
    if (t->kind == TYPE_STRING)  return type_char();
    if (t->kind == TYPE_LIST  && t->list_elem)          return t->list_elem;
    if (t->kind == TYPE_LIST  && t->list_count == 1 &&
        t->list_types)                                  return t->list_types[0];
    if (t->kind == TYPE_ARR   && t->arr_element_type)   return t->arr_element_type;
    /* untyped Coll — element type unknown */
    return NULL;
}

static bool call_types_compatible(Type *param, Type *arg) {
    if (!param || !arg) return true;
    if (param->kind == TYPE_VAR || param->kind == TYPE_UNKNOWN ||
        arg->kind   == TYPE_VAR || arg->kind   == TYPE_UNKNOWN)
        return true;
    if (param->kind == arg->kind) return true;
    if ((arg->kind == TYPE_NIL && param->kind == TYPE_OPTIONAL) ||
        (arg->kind == TYPE_OPTIONAL && param->kind == TYPE_NIL))
        return true;

    bool arg_is_int   = (arg->kind == TYPE_INT || arg->kind == TYPE_HEX ||
                         arg->kind == TYPE_BIN || arg->kind == TYPE_OCT ||
                         arg->kind == TYPE_INT_ARBITRARY);
    bool param_is_int = (param->kind == TYPE_INT ||
                         param->kind == TYPE_INT_ARBITRARY);
    bool arg_is_float = (arg->kind == TYPE_FLOAT || arg->kind == TYPE_F32 ||
                         arg->kind == TYPE_F80);
    bool param_is_float = (param->kind == TYPE_FLOAT || param->kind == TYPE_F32 ||
                           param->kind == TYPE_F80);
    if ((arg_is_int && param_is_int) || (arg_is_float && param_is_float) ||
        (arg_is_int && param_is_float))
        return true;

    bool arg_is_coll  = (arg->kind == TYPE_LIST || arg->kind == TYPE_ARR ||
                         arg->kind == TYPE_SET  || arg->kind == TYPE_MAP ||
                         arg->kind == TYPE_COLL || arg->kind == TYPE_STRING);
    bool param_is_coll = (param->kind == TYPE_LIST || param->kind == TYPE_ARR ||
                          param->kind == TYPE_SET  || param->kind == TYPE_MAP ||
                          param->kind == TYPE_COLL || param->kind == TYPE_STRING);
    return arg_is_coll && param_is_coll;
}

bool env_hm_check_call(Env *env, const char *name, Type **arg_types, int n,
                       const char *filename, int line, int col) {
    InferEnv *ienv = env_get_infer(env);
    if (!ienv) return true;

    InferCtx *ctx = infer_ctx_create(ienv, env_get_dep(env), filename ? filename : "<check>");

    TypeScheme *sc = infer_env_lookup(ctx, name);
    if (!sc) {
        infer_ctx_free(ctx);
        return true;
    }
    Type *inst = infer_instantiate(ctx, sc);
    Type *cursor = inst;
    /* fprintf(stderr, "DEBUG hm_check_call entry: name=%s sc->quantified_count=%d sc->type_kind=%d inst_kind=%d\n", */
    /*         name, */
    /*         sc->quantified_count, */
    /*         sc->type ? sc->type->kind : -1, */
    /*         inst ? inst->kind : -1); */
    bool ok = true;

    EnvEntry *ee = env_lookup(env, name);

    for (int i = 0; i < n && cursor && cursor->kind == TYPE_ARROW; i++) {
        Type *param     = cursor->arrow_param;
        Type *arg       = arg_types[i];
        Type *orig_param = param;
        Type *orig_arg   = arg;

        bool is_rest_slot = ee && i == ee->param_count - 1 &&
            ee->source_ast &&
            ee->source_ast->type == AST_LAMBDA &&
            ee->source_ast->lambda.param_count > 0 &&
            ee->source_ast->lambda.params[
                ee->source_ast->lambda.param_count - 1
            ].is_rest;

        if (is_rest_slot) {
            Type *rest_elem = collection_element_type(ee->params[i].type);
            if (!rest_elem && param && param->kind == TYPE_LIST)
                rest_elem = collection_element_type(param);
            if (!rest_elem)
                rest_elem = type_unknown();

            for (int j = i; j < n; j++) {
                Type *rest_arg = arg_types[j];
                if (!call_types_compatible(rest_elem, rest_arg)) {
                    char param_str[256];
                    char arg_str[256];
                    strncpy(param_str, type_to_string(rest_elem), sizeof(param_str) - 1);
                    param_str[sizeof(param_str) - 1] = '\0';
                    strncpy(arg_str, type_to_string(rest_arg), sizeof(arg_str) - 1);
                    arg_str[sizeof(arg_str) - 1] = '\0';
                    READER_ERROR(line, col,
                        "\n"
                        "    • Couldn't match expected type '%s' with actual type '%s'\n"
                        "    • In argument %d of a call to ‘%s’\n"
                        "  - Hint: variadic argument %d must be %s, but you passed %s",
                        param_str, arg_str,
                        j + 1, name,
                        j + 1, param_str, arg_str);
                }
            }
            break;
        }

        if (ee && i < ee->param_count && ee->params[i].type && arg) {
            Type *decl_param = ee->params[i].type;
            if (decl_param->kind == TYPE_LIST && decl_param->list_count > 1 &&
                arg->kind == TYPE_LIST && arg->list_count > 0) {

                /* Copy strings locally since type_to_string uses static buffers */
                char exp_t_str[256];
                strncpy(exp_t_str, type_to_string(decl_param), sizeof(exp_t_str) - 1);
                exp_t_str[sizeof(exp_t_str) - 1] = '\0';

                char act_t_str[256];
                strncpy(act_t_str, type_to_string(arg), sizeof(act_t_str) - 1);
                act_t_str[sizeof(act_t_str) - 1] = '\0';

                if (decl_param->list_count != arg->list_count) {
                    READER_ERROR(line, col,
                        "\n"
                        "    • Couldn't match expected list size %d with actual size %d\n"
                        "    • In argument %d of a call to ‘%s’\n"
                        "    • Expected type: %s\n"
                        "    • Provided type: %s\n"
                        "  - Hint: the function expects exactly %d elements, but you passed a list of %d",
                        decl_param->list_count, arg->list_count,
                        i + 1, name,
                        exp_t_str,
                        act_t_str,
                        decl_param->list_count, arg->list_count);
                } else {
                    for (int j = 0; j < decl_param->list_count; j++) {
                        Type *dt = decl_param->list_types[j];
                        Type *at = arg->list_types[j];
                        bool mismatch = false;

                        if (dt->kind != at->kind) {
                            bool dt_int = (dt->kind == TYPE_INT || dt->kind == TYPE_HEX || dt->kind == TYPE_BIN || dt->kind == TYPE_OCT || dt->kind == TYPE_INT_ARBITRARY);
                            bool at_int = (at->kind == TYPE_INT || at->kind == TYPE_HEX || at->kind == TYPE_BIN || at->kind == TYPE_OCT || at->kind == TYPE_INT_ARBITRARY);
                            bool dt_float = (dt->kind == TYPE_FLOAT || dt->kind == TYPE_F32 || dt->kind == TYPE_F80);
                            bool at_float = (at->kind == TYPE_FLOAT || at->kind == TYPE_F32 || at->kind == TYPE_F80);

                            /* Allow standard numeric widening but catch hard mismatches */
                            if (!(dt_int && at_int) && !(dt_float && at_float) &&
                                !(dt_float && at_int) &&
                                !(dt->kind == TYPE_UNKNOWN || at->kind == TYPE_UNKNOWN || dt->kind == TYPE_VAR || at->kind == TYPE_VAR)) {
                                mismatch = true;
                            }
                        }

                        if (mismatch) {
                            char dt_str[128];
                            strncpy(dt_str, type_to_string(dt), sizeof(dt_str) - 1);
                            dt_str[sizeof(dt_str) - 1] = '\0';

                            char at_str[128];
                            strncpy(at_str, type_to_string(at), sizeof(at_str) - 1);
                            at_str[sizeof(at_str) - 1] = '\0';

                            const char *suffix = (j == 0) ? "st" : (j == 1) ? "nd" : (j == 2) ? "rd" : "th";

                            READER_ERROR(line, col,
                                "\n"
                                "    • Couldn't match expected type '%s' with actual type '%s' at list index %d\n"
                                "    • In argument %d of a call to ‘%s’\n"
                                "    • Expected type: %s\n"
                                "    • Provided type: %s\n"
                                "  - Hint: the function expects the %d%s element to be '%s', but you passed '%s'",
                                dt_str, at_str, j,
                                i + 1, name,
                                exp_t_str,
                                act_t_str,
                                j + 1, suffix, dt_str, at_str);
                        }
                    }
                }
            }
        }

        /* fprintf(stderr, "DEBUG hm_check_call: name=%s i=%d param_kind=%d arg_kind=%d\n", */
        /*         name, i, */
        /*         param ? (int)param->kind : -1, */
        /*         arg   ? (int)arg->kind   : -1); */
        if (param && param->kind != TYPE_VAR && param->kind != TYPE_UNKNOWN &&
            arg   && arg->kind   != TYPE_VAR && arg->kind   != TYPE_UNKNOWN) {
            /* If param is TYPE_FN, check if it carries an arrow annotation
             * via the source lambda param's type_name (e.g. "Fn :: (Int -> Int)").
             * We recover it from the InferEnv scheme's arrow chain if available. */
            bool arg_is_fn   = (arg->kind   == TYPE_ARROW || arg->kind   == TYPE_FN);
            bool param_is_fn = (param->kind == TYPE_ARROW || param->kind == TYPE_FN);
            bool mismatch = false;
            if (arg_is_fn && !param_is_fn) {
                mismatch = true;
            } else if (!arg_is_fn && param_is_fn) {
                mismatch = true;
            } else if (arg_is_fn && param_is_fn &&
                       param->kind == TYPE_ARROW && arg->kind == TYPE_ARROW) {
                /* Both have full arrow types — compare slot by slot */
                Type *ap = arg->arrow_param;
                Type *pp = param->arrow_param;
                Type *aa = arg;
                Type *pa = param;
                while (ap && pp) {
                    bool ap_c = (ap->kind != TYPE_VAR && ap->kind != TYPE_UNKNOWN);
                    bool pp_c = (pp->kind != TYPE_VAR && pp->kind != TYPE_UNKNOWN);
                    if (ap_c && pp_c && ap->kind != pp->kind) {
                        bool a_int = (ap->kind == TYPE_INT || ap->kind == TYPE_HEX ||
                                      ap->kind == TYPE_BIN || ap->kind == TYPE_OCT ||
                                      ap->kind == TYPE_INT_ARBITRARY);
                        bool p_int = (pp->kind == TYPE_INT ||
                                      pp->kind == TYPE_INT_ARBITRARY);
                        if (!(a_int && p_int)) { mismatch = true; break; }
                    }
                    Type *an = (aa->kind == TYPE_ARROW) ? aa->arrow_ret : NULL;
                    Type *pn = (pa->kind == TYPE_ARROW) ? pa->arrow_ret : NULL;
                    if (!an || !pn) {
                        /* One chain ended before the other — arity mismatch */
                        if (an != pn) mismatch = true;
                        break;
                    }
                    aa = an; ap = aa->kind == TYPE_ARROW ? aa->arrow_param : aa;
                    pa = pn; pp = pa->kind == TYPE_ARROW ? pa->arrow_param : pa;
                }
                /* After the loop, if param chain is exhausted but arg chain
                 * still has arrows (or vice versa), arities differ */
                if (!mismatch) {
                    bool aa_done = (aa->kind != TYPE_ARROW);
                    bool pa_done = (pa->kind != TYPE_ARROW);
                    if (aa_done != pa_done) mismatch = true;
                }
            } else if (!arg_is_fn && !param_is_fn && arg->kind != param->kind) {
                /* Allow nil -> Optional */
                if ((arg->kind == TYPE_NIL && param->kind == TYPE_OPTIONAL) ||
                    (arg->kind == TYPE_OPTIONAL && param->kind == TYPE_NIL)) {
                    /* nil is perfectly compatible with any Optional argument */
                } else {
                    /* Allow numeric widening: any int kind satisfies Int param */
                    bool arg_is_int   = (arg->kind   == TYPE_INT || arg->kind == TYPE_HEX ||
                                         arg->kind   == TYPE_BIN || arg->kind == TYPE_OCT ||
                                         arg->kind   == TYPE_INT_ARBITRARY);
                    bool param_is_int = (param->kind == TYPE_INT ||
                                         param->kind == TYPE_INT_ARBITRARY);
                    bool arg_is_coll  = (arg->kind == TYPE_LIST || arg->kind == TYPE_ARR ||
                                         arg->kind == TYPE_SET  || arg->kind == TYPE_MAP  ||
                                         arg->kind == TYPE_COLL || arg->kind == TYPE_STRING);
                    bool param_is_coll = (param->kind == TYPE_LIST || param->kind == TYPE_ARR ||
                                          param->kind == TYPE_SET  || param->kind == TYPE_MAP  ||
                                          param->kind == TYPE_COLL || param->kind == TYPE_STRING);
                    bool arg_is_float = (arg->kind == TYPE_FLOAT || arg->kind == TYPE_F32 ||
                                         arg->kind == TYPE_F80);
                    bool param_is_float = (param->kind == TYPE_FLOAT || param->kind == TYPE_F32 ||
                                           param->kind == TYPE_F80);
                    if (!(arg_is_int && param_is_int) &&
                        !(arg_is_float && param_is_float) &&
                        !(arg_is_int && param_is_float) &&
                        !(arg_is_coll && param_is_coll))
                        mismatch = true;
                }
            }
            if (mismatch) {
                /* Use orig_arg/orig_param — arg/param may have been advanced
                 * by the arrow-walking loop and type_to_string uses static
                 * buffers so we copy the param string before calling again. */
                char param_str[256];
                char arg_str[256];
                strncpy(param_str, type_to_string(orig_param), sizeof(param_str) - 1);
                param_str[sizeof(param_str) - 1] = '\0';
                strncpy(arg_str, type_to_string(orig_arg), sizeof(arg_str) - 1);
                arg_str[sizeof(arg_str) - 1] = '\0';
                const char *arg_desc = arg_str;
                /* Build a full signature string from the scheme type */
                char sig[256] = {0};
                Type *_walk = sc->type;
                bool _first = true;
                while (_walk && _walk->kind == TYPE_ARROW) {
                    if (!_first) strncat(sig, " -> ", sizeof(sig) - strlen(sig) - 1);
                    strncat(sig, type_to_string(_walk->arrow_param),
                            sizeof(sig) - strlen(sig) - 1);
                    _first = false;
                    _walk = _walk->arrow_ret;
                }
                if (_walk) {
                    strncat(sig, " -> ", sizeof(sig) - strlen(sig) - 1);
                    strncat(sig, type_to_string(_walk), sizeof(sig) - strlen(sig) - 1);
                }
                READER_ERROR(line, col,
                    "\n"
                    "    • Couldn't match expected type '%s' with actual type '%s'\n"
                    "    • In argument %d of a call to ‘%s’\n"
                    "    • Of type: %s :: %s\n"
                    "  - Hint: argument %d must be %s, but you passed %s",
                    param_str, arg_desc,
                    i + 1, name,
                    name, sig,
                    i + 1, param_str, arg_desc);
            }
        }
        cursor = cursor->arrow_ret;
    }

/* Cross-argument check: if argument i is a function (arrow) and argument j
     * is a collection, verify that the function's domain matches the
     * collection's element type.  This catches (my-map double "ciao") where
     * double :: Int->Int but String elements are Char.                        */
    {
        /* Find the first function arg and the first collection arg */
        int fn_idx   = -1;
        int coll_idx = -1;
        for (int i = 0; i < n; i++) {
            if (!arg_types[i]) continue;
            bool is_fn   = (arg_types[i]->kind == TYPE_ARROW ||
                            arg_types[i]->kind == TYPE_FN);
            bool is_coll = (arg_types[i]->kind == TYPE_STRING ||
                            arg_types[i]->kind == TYPE_LIST   ||
                            arg_types[i]->kind == TYPE_ARR    ||
                            arg_types[i]->kind == TYPE_COLL);
            if (is_fn   && fn_idx   < 0) fn_idx   = i;
            if (is_coll && coll_idx < 0) coll_idx = i;
        }

        if (fn_idx >= 0 && coll_idx >= 0) {
            Type *fn_t   = arg_types[fn_idx];
            Type *coll_t = arg_types[coll_idx];

            /* Extract the function's input (domain) type */
            Type *fn_domain = (fn_t->kind == TYPE_ARROW) ? fn_t->arrow_param : NULL;

            /* Extract the collection's element type */
            Type *elem_t = collection_element_type(coll_t);

            if (fn_domain && elem_t &&
                fn_domain->kind != TYPE_VAR && fn_domain->kind != TYPE_UNKNOWN &&
                elem_t->kind    != TYPE_VAR && elem_t->kind    != TYPE_UNKNOWN &&
                fn_domain->kind != elem_t->kind) {

                /* Allow nil -> Optional */
                bool is_opt_nil = ((fn_domain->kind == TYPE_NIL && elem_t->kind == TYPE_OPTIONAL) ||
                                   (fn_domain->kind == TYPE_OPTIONAL && elem_t->kind == TYPE_NIL));

                /* Allow numeric widening (HEX/BIN/OCT/arbitrary all satisfy Int) */
                bool dom_is_int  = (fn_domain->kind == TYPE_INT ||
                                    fn_domain->kind == TYPE_HEX ||
                                    fn_domain->kind == TYPE_BIN ||
                                    fn_domain->kind == TYPE_OCT ||
                                    fn_domain->kind == TYPE_INT_ARBITRARY);
                bool elem_is_int = (elem_t->kind == TYPE_INT ||
                                    elem_t->kind == TYPE_HEX ||
                                    elem_t->kind == TYPE_BIN ||
                                    elem_t->kind == TYPE_OCT ||
                                    elem_t->kind == TYPE_INT_ARBITRARY);

                if (!is_opt_nil && !(dom_is_int && elem_is_int)) {
                    char dom_str[128], elem_str[128], coll_str[128];
                    strncpy(dom_str,  type_to_string(fn_domain), sizeof(dom_str)  - 1);
                    strncpy(elem_str, type_to_string(elem_t),    sizeof(elem_str) - 1);
                    strncpy(coll_str, type_to_string(coll_t),    sizeof(coll_str) - 1);
                    dom_str[sizeof(dom_str)-1]   = '\0';
                    elem_str[sizeof(elem_str)-1] = '\0';
                    coll_str[sizeof(coll_str)-1] = '\0';
                    READER_ERROR(line, col,
                        "\n"
                        "    • Couldn't match collection element type '%s' with function domain '%s'\n"
                        "    • In call to '%s': the function (argument %d) expects '%s'\n"
                        "    • but the collection (argument %d) has element type '%s' (%s)\n"
                        "  - Hint: you cannot map a '%s -> ...' function over a %s",
                        elem_str, dom_str,
                        name, fn_idx + 1, dom_str,
                        coll_idx + 1, elem_str, coll_str,
                        dom_str, coll_str);
                }
            }
        }
    }

    infer_ctx_free(ctx);
    return ok;
}

bool env_hm_instantiate_call(Env *env, const char *name, Type **out_params,
                              int n, Type **out_ret) {
    InferEnv *ienv = env_get_infer(env);
    if (!ienv) return false;

    InferCtx *ctx  = infer_ctx_create(ienv, env_get_dep(env), "<instantiate>");

    TypeScheme *sc = infer_env_lookup(ctx, name);
    if (!sc) {
        infer_ctx_free(ctx);
        return false;
    }
    Type *inst     = infer_instantiate(ctx, sc);
    Type *cursor   = inst;

    for (int i = 0; i < n; i++) {
        if (cursor && cursor->kind == TYPE_ARROW) {
            out_params[i] = subst_apply(ctx->subst, cursor->arrow_param);
            cursor = cursor->arrow_ret;
        } else {
            out_params[i] = type_unknown();
        }
    }
    if (out_ret)
        *out_ret = subst_apply(ctx->subst, cursor ? cursor : type_unknown());

    infer_ctx_free(ctx);
    return true;
}
