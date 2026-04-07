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

void env_init_infer(Env *root) {
    if (root->infer_env) return;   /* idempotent */
    root->infer_env = infer_env_create();
    /* Bootstrap built-in type schemes into the infer env */
    InferCtx *bctx = infer_ctx_create(root->infer_env, "<builtins>");
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

struct TypeScheme *env_hm_infer_define(Env *env, const char *name,
                                       AST *lambda_ast, const char *filename) {
    InferEnv *ienv = env_get_infer(env);
    if (!ienv) return NULL;

    InferEnv *child = infer_env_create_child(ienv);
    InferCtx *ctx   = infer_ctx_create(child, filename ? filename : "<unknown>");
    /* Pre-bind with a fresh var so recursive calls resolve.
     * We use a high ID so it doesn't pollute the visible var names. */
    ctx->subst->next_id = 1000;
    Type *self_t = infer_fresh(ctx);
    ctx->subst->next_id = 0;  /* reset so params get 'a, 'b, 'c... */
    infer_env_insert(child, name, scheme_mono(self_t));
    Type *inferred = infer_toplevel(ctx, lambda_ast);


    TypeScheme *scheme = NULL;

    if (!inferred || ctx->had_error) {
        fprintf(stderr, "[hm] inference failed for '%s': %s\n",
                name, ctx->error_msg);
    } else {
        infer_unify_one(ctx, self_t, inferred, lambda_ast->line, lambda_ast->column);
        /* Fully apply substitution so the scheme's type nodes are concrete
         * ground types — no TYPE_VAR nodes that reference the now-dead ctx */
        scheme = infer_generalise(ctx, inferred, ienv);
        /* Walk the full type tree applying the substitution deeply so no
         * TYPE_VAR nodes remain that reference the now-dead substitution */
        scheme->type = subst_apply(ctx->subst, scheme->type);
        /* Also walk arrow chain and concretize each node */
        {
            Type *t = scheme->type;
            while (t && t->kind == TYPE_ARROW) {
                t->arrow_param = subst_apply(ctx->subst, t->arrow_param);
                t->arrow_ret   = subst_apply(ctx->subst, t->arrow_ret);
                t = t->arrow_ret;
            }
        }
        /* ienv owns the original scheme for future instantiation.
         * EnvEntry gets its own clone so free_entry_fields() can
         * safely call scheme_free() without double-freeing.        */
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
    if (t->kind == TYPE_ARR   && t->arr_element_type)   return t->arr_element_type;
    /* untyped Coll — element type unknown */
    return NULL;
}

bool env_hm_check_call(Env *env, const char *name, Type **arg_types, int n,
                       const char *filename, int line, int col) {
    InferEnv *ienv = env_get_infer(env);
    if (!ienv) return true;

    TypeScheme *sc = infer_env_lookup(ienv, name);
    if (!sc) return true;

    InferCtx *ctx = infer_ctx_create(ienv, filename ? filename : "<check>");
    Type *inst = infer_instantiate(ctx, sc);
    Type *cursor = inst;
    fprintf(stderr, "DEBUG hm_check_call entry: name=%s sc->quantified_count=%d sc->type_kind=%d inst_kind=%d\n",
            name,
            sc->quantified_count,
            sc->type ? sc->type->kind : -1,
            inst ? inst->kind : -1);
    bool ok = true;

    for (int i = 0; i < n && cursor && cursor->kind == TYPE_ARROW; i++) {
        Type *param     = cursor->arrow_param;
        Type *arg       = arg_types[i];
        Type *orig_param = param;
        Type *orig_arg   = arg;
        fprintf(stderr, "DEBUG hm_check_call: name=%s i=%d param_kind=%d arg_kind=%d\n",
                name, i,
                param ? (int)param->kind : -1,
                arg   ? (int)arg->kind   : -1);
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
                    if (!an || !pn) break;
                    aa = an; ap = aa->kind == TYPE_ARROW ? aa->arrow_param : aa;
                    pa = pn; pp = pa->kind == TYPE_ARROW ? pa->arrow_param : pa;
                }
            } else if (!arg_is_fn && !param_is_fn && arg->kind != param->kind) {
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
                if (!(arg_is_int && param_is_int) &&
                    !(arg_is_coll && param_is_coll))
                    mismatch = true;
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

                if (!(dom_is_int && elem_is_int)) {
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

    TypeScheme *sc = infer_env_lookup(ienv, name);
    if (!sc) return false;

    InferCtx *ctx  = infer_ctx_create(ienv, "<instantiate>");
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
