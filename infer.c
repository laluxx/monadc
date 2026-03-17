#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "infer.h"
#include "types.h"
#include "reader.h"


/// InferEnv

#define INFER_ENV_BUCKETS 64

static size_t infer_env_hash(const char *name) {
    size_t h = 5381;
    for (; *name; name++) h = h * 33 ^ (unsigned char)*name;
    return h % INFER_ENV_BUCKETS;
}

InferEnv *infer_env_create(void) {
    InferEnv *e  = calloc(1, sizeof(InferEnv));
    e->buckets   = calloc(INFER_ENV_BUCKETS, sizeof(InferEnvEntry *));
    e->size      = INFER_ENV_BUCKETS;
    e->parent    = NULL;
    return e;
}

InferEnv *infer_env_create_child(InferEnv *parent) {
    InferEnv *e = infer_env_create();
    e->parent   = parent;
    return e;
}

void infer_env_free(InferEnv *env) {
    if (!env) return;
    for (size_t i = 0; i < env->size; i++) {
        InferEnvEntry *e = env->buckets[i];
        while (e) {
            InferEnvEntry *next = e->next;
            free(e->name);
            /* schemes are owned by the context — do not free here */
            free(e);
            e = next;
        }
    }
    free(env->buckets);
    free(env);
}

void infer_env_insert(InferEnv *env, const char *name, TypeScheme *scheme) {
    size_t         idx = infer_env_hash(name);
    /* Overwrite existing entry for this name if present */
    for (InferEnvEntry *e = env->buckets[idx]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            e->scheme = scheme;   /* old scheme freed by caller or ctx   */
            return;
        }
    }
    /* Not found — prepend new entry */
    InferEnvEntry *e = calloc(1, sizeof(InferEnvEntry));
    e->name          = strdup(name);
    e->scheme        = scheme;
    e->next          = env->buckets[idx];
    env->buckets[idx] = e;
}

TypeScheme *infer_env_lookup(InferEnv *env, const char *name) {
    for (InferEnv *cur = env; cur; cur = cur->parent) {
        size_t         idx = infer_env_hash(name);
        InferEnvEntry *e   = cur->buckets[idx];
        while (e) {
            if (strcmp(e->name, name) == 0) return e->scheme;
            e = e->next;
        }
    }
    return NULL;
}


/// Substitution

Substitution *subst_create(void) {
    Substitution *s = calloc(1, sizeof(Substitution));
    s->capacity     = INFER_MAX_VARS;
    s->union_find   = malloc(sizeof(int)   * INFER_MAX_VARS);
    s->bound        = calloc(INFER_MAX_VARS, sizeof(Type *));
    s->next_id      = 0;
    for (int i = 0; i < INFER_MAX_VARS; i++)
        s->union_find[i] = i;          /* each var is its own root         */
    return s;
}

void subst_free(Substitution *s) {
    if (!s) return;
    free(s->union_find);
    free(s->bound);
    free(s);
}

int subst_fresh(Substitution *s) {
    if (s->next_id >= s->capacity) {
        fprintf(stderr, "infer: type variable limit (%d) exceeded\n", s->capacity);
        exit(1);
    }
    int id            = s->next_id++;
    s->union_find[id] = id;
    s->bound[id]      = NULL;
    return id;
}

int subst_find(Substitution *s, int id) {
    /* path compression */
    while (s->union_find[id] != id) {
        s->union_find[id] = s->union_find[s->union_find[id]]; /* path halving */
        id = s->union_find[id];
    }
    return id;
}

void subst_bind(Substitution *s, int id, Type *t) {
    int root   = subst_find(s, id);
    s->bound[root] = t;
}

bool subst_union(Substitution *s, int a, int b) {
    int ra = subst_find(s, a);
    int rb = subst_find(s, b);
    if (ra == rb) return true;
    /* merge rb into ra */
    s->union_find[rb] = ra;
    /* if rb had a concrete binding, carry it to ra */
    if (!s->bound[ra] && s->bound[rb])
        s->bound[ra] = s->bound[rb];
    return true;
}

Type *subst_apply_shallow(Substitution *s, Type *t) {
    if (!t) return NULL;
    if (t->kind != TYPE_VAR) return t;
    int root = subst_find(s, t->var_id);
    if (s->bound[root]) return s->bound[root];
    /* return a canonical var node for this root */
    if (root == t->var_id) return t;
    Type *canonical = type_var(root);
    return canonical;
}

Type *subst_apply(Substitution *s, Type *t) {
    if (!t) return NULL;
    t = subst_apply_shallow(s, t);
    if (!t) return NULL;

    switch (t->kind) {
    case TYPE_VAR:
        return t;   /* unresolved free variable — leave as-is */

    case TYPE_LIST:
        if (t->list_elem)
            return type_list(subst_apply(s, t->list_elem));
        return t;

    case TYPE_ARR:
        if (t->arr_element_type)
            return type_arr(subst_apply(s, t->arr_element_type), t->arr_size);
        return t;

    case TYPE_ARROW: {
        /* function type: param → return */
        Type *p = subst_apply(s, t->arrow_param);
        Type *r = subst_apply(s, t->arrow_ret);
        return type_arrow(p, r);
    }

    default:
        return t;   /* ground types are already fully concrete */
    }
}


/// Inference Context

InferCtx *infer_ctx_create(InferEnv *env, const char *filename) {
    InferCtx *ctx       = calloc(1, sizeof(InferCtx));
    ctx->subst          = subst_create();
    ctx->constraint_cap = 256;
    ctx->constraints    = malloc(sizeof(TypeConstraint) * ctx->constraint_cap);
    ctx->constraint_count = 0;
    ctx->env            = env;
    ctx->filename       = filename ? filename : "<unknown>";
    ctx->had_error      = false;
    return ctx;
}

void infer_ctx_free(InferCtx *ctx) {
    if (!ctx) return;
    subst_free(ctx->subst);
    free(ctx->constraints);
    free(ctx);
}


/// Fresh Type Variables

Type *infer_fresh(InferCtx *ctx) {
    int id = subst_fresh(ctx->subst);
    return type_var(id);
}

Type *infer_fresh_named(InferCtx *ctx, const char *hint) {
    (void)hint;  /* stored in debug builds only — ignored for now */
    return infer_fresh(ctx);
}


/// Occurs Check

bool infer_occurs(Substitution *s, int var_id, Type *t) {
    if (!t) return false;
    t = subst_apply_shallow(s, t);
    if (!t) return false;

    switch (t->kind) {
    case TYPE_VAR:
        return subst_find(s, t->var_id) == subst_find(s, var_id);
    case TYPE_LIST:
        return infer_occurs(s, var_id, t->list_elem);
    case TYPE_ARR:
        return infer_occurs(s, var_id, t->arr_element_type);
    case TYPE_ARROW:
        return infer_occurs(s, var_id, t->arrow_param)
            || infer_occurs(s, var_id, t->arrow_ret);
    default:
        return false;
    }
}


/// Constraint Generation

void infer_constrain(InferCtx *ctx, Type *a, Type *b, int line, int col) {
    if (ctx->constraint_count >= ctx->constraint_cap) {
        ctx->constraint_cap *= 2;
        ctx->constraints = realloc(ctx->constraints,
                                   sizeof(TypeConstraint) * ctx->constraint_cap);
    }
    ctx->constraints[ctx->constraint_count++] = (TypeConstraint){a, b, line, col};
}


/// Unification

bool infer_unify_one(InferCtx *ctx, Type *a, Type *b, int line, int col) {
    Substitution *s = ctx->subst;
    a = subst_apply_shallow(s, a);
    b = subst_apply_shallow(s, b);

    if (!a || !b) return true;  /* NULL ~ anything: allow for now */

    /* Both free variables — merge their roots */
    if (a->kind == TYPE_VAR && b->kind == TYPE_VAR) {
        subst_union(s, a->var_id, b->var_id);
        return true;
    }

    /* Left is a free variable — bind it */
    if (a->kind == TYPE_VAR) {
        int root = subst_find(s, a->var_id);
        if (infer_occurs(s, root, b)) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "%s:%d:%d: type error: infinite type (occurs check failed)",
                     ctx->filename, line, col);
            ctx->had_error = true;
            return false;
        }
        subst_bind(s, root, b);
        return true;
    }

    /* Right is a free variable — bind it */
    if (b->kind == TYPE_VAR) {
        int root = subst_find(s, b->var_id);
        if (infer_occurs(s, root, a)) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "%s:%d:%d: type error: infinite type (occurs check failed)",
                     ctx->filename, line, col);
            ctx->had_error = true;
            return false;
        }
        subst_bind(s, root, a);
        return true;
    }

    // Both ground — must match structurally
    if (a->kind != b->kind) {
        /* TYPE_FN (unannotated Fn) is compatible with any arrow type */
        if (a->kind == TYPE_FN && b->kind == TYPE_ARROW) return true;
        if (a->kind == TYPE_ARROW && b->kind == TYPE_FN) return true;
        /* TYPE_FN ~ TYPE_FN always ok */
        if (a->kind == TYPE_FN && b->kind == TYPE_FN) return true;
        /* TYPE_COLL is compatible with any collection type */
        if (a->kind == TYPE_COLL && (b->kind == TYPE_LIST ||
                                     b->kind == TYPE_SET  ||
                                     b->kind == TYPE_ARR)) return true;
        if (b->kind == TYPE_COLL && (a->kind == TYPE_LIST ||
                                     a->kind == TYPE_SET  ||
                                     a->kind == TYPE_ARR)) return true;
        char a_str[64], b_str[64];
        snprintf(a_str, sizeof(a_str), "%s", type_to_string(a));
        snprintf(b_str, sizeof(b_str), "%s", type_to_string(b));
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "%s:%d:%d: type warning: cannot unify %s with %s",
                 ctx->filename, line, col, a_str, b_str);
        ctx->had_error = true;
        return false;
    }


    /* Recurse for compound types */
    switch (a->kind) {
    case TYPE_LIST:
        return infer_unify_one(ctx, a->list_elem, b->list_elem, line, col);

    case TYPE_ARR:
        return infer_unify_one(ctx, a->arr_element_type,
                                     b->arr_element_type, line, col);

    case TYPE_ARROW:
        return infer_unify_one(ctx, a->arrow_param, b->arrow_param, line, col)
            && infer_unify_one(ctx, a->arrow_ret,   b->arrow_ret,   line, col);

    default:
        return true;  /* ground types with matching kinds are equal */
    }
}

bool infer_unify_all(InferCtx *ctx) {
    for (size_t i = 0; i < ctx->constraint_count; i++) {
        TypeConstraint *c = &ctx->constraints[i];
        if (!infer_unify_one(ctx, c->lhs, c->rhs, c->line, c->col))
            return false;
    }
    return true;
}


/// Free Variables

static bool int_array_contains(int *arr, int count, int v) {
    for (int i = 0; i < count; i++)
        if (arr[i] == v) return true;
    return false;
}

void infer_free_vars_type(Substitution *s, Type *t, int *out, int *count, int cap) {
    if (!t) return;
    if (!t || (uintptr_t)t < 0x1000) return;
    t = subst_apply_shallow(s, t);
    if (!t) return;

    switch (t->kind) {
    case TYPE_VAR: {
        int root = subst_find(s, t->var_id);
        if (!int_array_contains(out, *count, root) && *count < cap)
            out[(*count)++] = root;
        break;
    }
    case TYPE_LIST:
        infer_free_vars_type(s, t->list_elem, out, count, cap);
        break;
    case TYPE_ARR:
        infer_free_vars_type(s, t->arr_element_type, out, count, cap);
        break;
    case TYPE_ARROW:
        infer_free_vars_type(s, t->arrow_param, out, count, cap);
        infer_free_vars_type(s, t->arrow_ret,   out, count, cap);
        break;
    default:
        break;
    }
}

void infer_free_vars_env(InferCtx *ctx, InferEnv *env, int *out, int *count, int cap) {
    /* Only walk the single env level passed — do NOT follow parent chain.
     * Parent envs contain schemes from previous InferCtx runs whose type
     * var IDs belong to foreign substitutions and cannot be safely
     * dereferenced with the current ctx->subst.
     * This is safe because:
     * - Built-in schemes are fully generalised (quantified_count > 0)
     * - Previously defined schemes are fully generalised
     * - Only the current definition's child scope has monomorphic
     *   pre-bind schemes that matter for generalisation                */
    for (size_t i = 0; i < env->size; i++) {
        for (InferEnvEntry *e = env->buckets[i]; e; e = e->next) {
            TypeScheme *sc = e->scheme;
            /* Guard against NULL or corrupt pointers from stale entries */
            if (!sc || (uintptr_t)sc < 0x1000) continue;
            if (!sc->type || (uintptr_t)sc->type < 0x1000) continue;
            /* Skip fully generalised schemes — their type vars belong to
             * outer or previous substitutions and must not be examined   */
            if (sc->quantified_count > 0) continue;
            /* Only consider type vars within the current substitution range */
            int body_free[INFER_MAX_VARS];
            int body_count = 0;
            infer_free_vars_type(ctx->subst, sc->type,
                                 body_free, &body_count, INFER_MAX_VARS);
            for (int j = 0; j < body_count; j++) {
                if (body_free[j] < ctx->subst->next_id &&
                    !int_array_contains(out, *count, body_free[j]) &&
                    *count < cap) {
                    out[(*count)++] = body_free[j];
                }
            }
        }
    }
}

/// Generalisation and Instantiation

TypeScheme *infer_generalise(InferCtx *ctx, Type *t, InferEnv *outer_env) {
    /* Apply substitution fully first */
    t = subst_apply(ctx->subst, t);

    /* Collect free vars in t */
    int type_free[INFER_MAX_VARS];
    int type_free_count = 0;
    infer_free_vars_type(ctx->subst, t, type_free, &type_free_count, INFER_MAX_VARS);

    /* Collect free vars in the outer environment */
    int env_free[INFER_MAX_VARS];
    int env_free_count = 0;
    infer_free_vars_env(ctx, outer_env, env_free, &env_free_count, INFER_MAX_VARS);

    /* Quantify vars that are free in t but not free in the environment */
    TypeScheme *sc       = calloc(1, sizeof(TypeScheme));
    sc->quantified       = malloc(sizeof(int) * type_free_count);
    sc->quantified_count = 0;
    sc->type             = t;

    for (int i = 0; i < type_free_count; i++) {
        if (!int_array_contains(env_free, env_free_count, type_free[i]))
            sc->quantified[sc->quantified_count++] = type_free[i];
    }

    return sc;
}

Type *infer_instantiate(InferCtx *ctx, TypeScheme *scheme) {
    if (scheme->quantified_count == 0)
        return subst_apply(ctx->subst, scheme->type);

    /* Build a local substitution: quantified vars → fresh vars */
    int fresh_ids[INFER_MAX_VARS];
    for (int i = 0; i < scheme->quantified_count; i++)
        fresh_ids[i] = subst_fresh(ctx->subst);

    /* Walk the scheme type and substitute quantified vars */
    /* We do this by temporarily binding each quantified var
     * to a fresh one in the substitution, applying, then clearing */
    Substitution *s = ctx->subst;

    /* Save old bindings (should be NULL since they're quantified) */
    for (int i = 0; i < scheme->quantified_count; i++) {
        int qv   = scheme->quantified[i];
        int root = subst_find(s, qv);
        subst_bind(s, root, type_var(fresh_ids[i]));
    }

    Type *result = subst_apply(s, scheme->type);

    /* Clear the temporary bindings */
    for (int i = 0; i < scheme->quantified_count; i++) {
        int qv   = scheme->quantified[i];
        int root = subst_find(s, qv);
        s->bound[root] = NULL;
    }

    return result;
}

Type *infer_instantiate_with_subst(InferCtx *ctx, TypeScheme *scheme,
                                    TypeSubst *ts) {
    ts->count = scheme->quantified_count;
    ts->from  = malloc(sizeof(int)   * (ts->count ? ts->count : 1));
    ts->to    = malloc(sizeof(Type*) * (ts->count ? ts->count : 1));

    if (scheme->quantified_count == 0) {
        return subst_apply(ctx->subst, scheme->type);
    }

    Substitution *s = ctx->subst;
    int fresh_ids[INFER_MAX_VARS];

    for (int i = 0; i < scheme->quantified_count; i++) {
        fresh_ids[i] = subst_fresh(s);
        ts->from[i]  = scheme->quantified[i];
    }

    /* Temporarily bind quantified vars to fresh vars */
    for (int i = 0; i < scheme->quantified_count; i++) {
        int qv   = scheme->quantified[i];
        int root = subst_find(s, qv);
        subst_bind(s, root, type_var(fresh_ids[i]));
    }

    Type *result = subst_apply(s, scheme->type);

    /* Clear temporary bindings */
    for (int i = 0; i < scheme->quantified_count; i++) {
        int qv   = scheme->quantified[i];
        int root = subst_find(s, qv);
        s->bound[root] = NULL;
    }

    /* Record what each quantified var mapped to after unification.
     * At call site, after unification runs, fresh_ids[i] will be
     * bound to the concrete type — we record that mapping.        */
    for (int i = 0; i < scheme->quantified_count; i++) {
        ts->from[i] = scheme->quantified[i];
        ts->to[i]   = type_var(fresh_ids[i]);  /* will be resolved after unification */
    }

    return result;
}


/// Scheme Constructors

TypeScheme *scheme_mono(Type *t) {
    TypeScheme *sc       = calloc(1, sizeof(TypeScheme));
    sc->quantified       = NULL;
    sc->quantified_count = 0;
    sc->type             = t;
    return sc;
}

TypeScheme *scheme_clone(TypeScheme *s) {
    TypeScheme *c       = calloc(1, sizeof(TypeScheme));
    c->quantified_count = s->quantified_count;
    c->quantified       = malloc(sizeof(int) * s->quantified_count);
    memcpy(c->quantified, s->quantified, sizeof(int) * s->quantified_count);
    c->type             = s->type;  /* type nodes are shared — not deep-copied */
    return c;
}

void scheme_free(TypeScheme *s) {
    if (!s) return;
    free(s->quantified);
    free(s);
}


/// Zonking
//
//  Walk the AST and replace every node's inferred_type with its
//  fully-substituted form.  After this pass, no TYPE_VAR nodes
//  remain in any inferred_type (unless the type is genuinely
//  polymorphic in a let-binding, which codegen handles via
//  monomorphization).
//
void infer_zonk_ast(InferCtx *ctx, AST *ast) {
    if (!ast) return;

    if (ast->inferred_type)
        ast->inferred_type = subst_apply(ctx->subst, ast->inferred_type);

    switch (ast->type) {
    case AST_LIST:
        for (size_t i = 0; i < ast->list.count; i++)
            infer_zonk_ast(ctx, ast->list.items[i]);
        break;
    case AST_ARRAY:
        for (size_t i = 0; i < ast->array.element_count; i++)
            infer_zonk_ast(ctx, ast->array.elements[i]);
        break;
    case AST_SET:
        for (size_t i = 0; i < ast->set.element_count; i++)
            infer_zonk_ast(ctx, ast->set.elements[i]);
        break;
    case AST_MAP:
        for (size_t i = 0; i < ast->map.count; i++) {
            infer_zonk_ast(ctx, ast->map.keys[i]);
            infer_zonk_ast(ctx, ast->map.vals[i]);
        }
        break;
    case AST_LAMBDA:
        for (int i = 0; i < ast->lambda.body_count; i++)
            infer_zonk_ast(ctx, ast->lambda.body_exprs[i]);
        break;
    default:
        break;
    }
}


/// Type Inference — Expression Walk

static void infer_env_remove(InferEnv *env, const char *name) {
    if (!env || !name) return;
    size_t idx = infer_env_hash(name);
    InferEnvEntry *prev = NULL;
    for (InferEnvEntry *e = env->buckets[idx]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            if (prev) prev->next = e->next;
            else       env->buckets[idx] = e->next;
            scheme_free(e->scheme);
            free((char*)e->name);
            free(e);
            env->size--;
            return;
        }
        prev = e;
    }
}

Type *infer_expr(InferCtx *ctx, AST *ast) {
    if (!ast) return type_unknown();

    Type *result = NULL;

    switch (ast->type) {

    case AST_NUMBER: {
        bool is_float = false;
        if (ast->literal_str) {
            for (const char *p = ast->literal_str; *p; p++)
                if (*p == '.' || *p == 'e' || *p == 'E') { is_float = true; break; }
        } else {
            is_float = (ast->number != (double)(int64_t)ast->number);
        }
        result = is_float ? type_float() : type_int();
        break;
    }

    case AST_STRING:
        result = type_string();
        break;

    case AST_CHAR:
        result = type_char();
        break;

    case AST_KEYWORD:
        result = type_keyword();
        break;

    case AST_RATIO:
        result = type_ratio();
        break;

    case AST_SYMBOL: {
        if (strcmp(ast->symbol, "True")  == 0) { result = type_bool(); break; }
        if (strcmp(ast->symbol, "False") == 0) { result = type_bool(); break; }
        if (strcmp(ast->symbol, "nil")   == 0) { result = infer_fresh(ctx); break; }

        TypeScheme *sc = infer_env_lookup(ctx->env, ast->symbol);
        if (!sc) {
            result = infer_fresh(ctx);
            break;
        }
        result = infer_instantiate(ctx, sc);
        break;
    }

    case AST_SET: {
        Type *elem_t = infer_fresh(ctx);
        for (size_t i = 0; i < ast->set.element_count; i++) {
            Type *et = infer_expr(ctx, ast->set.elements[i]);
            infer_constrain(ctx, et, elem_t, ast->line, ast->column);
        }
        result = type_set();
        break;
    }

    case AST_MAP: {
        Type *key_t = infer_fresh(ctx);
        Type *val_t = infer_fresh(ctx);
        for (size_t i = 0; i < ast->map.count; i++) {
            Type *kt = infer_expr(ctx, ast->map.keys[i]);
            Type *vt = infer_expr(ctx, ast->map.vals[i]);
            infer_constrain(ctx, kt, key_t, ast->line, ast->column);
            infer_constrain(ctx, vt, val_t, ast->line, ast->column);
        }
        result = type_map();
        break;
    }

    case AST_ARRAY: {
        Type *elem_t = infer_fresh(ctx);
        for (size_t i = 0; i < ast->array.element_count; i++) {
            Type *et = infer_expr(ctx, ast->array.elements[i]);
            infer_constrain(ctx, et, elem_t, ast->line, ast->column);
        }
        result = type_arr(elem_t, (int)ast->array.element_count);
        break;
    }

    case AST_LAMBDA: {
        InferEnv *child = infer_env_create_child(ctx->env);
        InferEnv *saved = ctx->env;
        ctx->env        = child;

        Type **param_types = malloc(sizeof(Type *) * (ast->lambda.param_count + 1));
        for (int i = 0; i < ast->lambda.param_count; i++) {
            ASTParam *p = &ast->lambda.params[i];
            Type *pt;
            if (p->is_rest) {
                // Rest param gets type List 'a — body sees it as a list
                Type *elem = infer_fresh(ctx);
                pt = type_list(elem);
            } else if (p->type_name) {
                pt = type_from_name(p->type_name);
                if (!pt) pt = infer_fresh(ctx);
            } else {
                pt = infer_fresh(ctx);
            }
            param_types[i] = pt;
            infer_env_insert(child, p->name, scheme_mono(pt));
        }

        Type *ret_t = infer_fresh(ctx);
        for (int i = 0; i < ast->lambda.body_count; i++) {
            Type *bt = infer_expr(ctx, ast->lambda.body_exprs[i]);
            if (i == ast->lambda.body_count - 1) {
                if (ast->lambda.return_type) {
                    Type *ann = type_from_name(ast->lambda.return_type);
                    if (ann) infer_constrain(ctx, bt, ann, ast->line, ast->column);
                    ret_t = ann ? ann : bt;
                } else {
                    ret_t = bt;
                }
            }
        }

        ctx->env = saved;
        infer_env_free(child);

        Type *arrow = ret_t;
        for (int i = ast->lambda.param_count - 1; i >= 0; i--)
            arrow = type_arrow(param_types[i], arrow);
        free(param_types);

        result = arrow;
        break;
    }

    case AST_LIST: {
        if (ast->list.count == 0) {
            result = infer_fresh(ctx);
            break;
        }

        AST *head = ast->list.items[0];

        /* ---- define -------------------------------------------------- */
        if (head->type == AST_SYMBOL && strcmp(head->symbol, "define") == 0) {
            result = type_unknown();
            if (ast->list.count < 3) break;

            const char *name = NULL;
            if (ast->list.items[1]->type == AST_SYMBOL) {
                name = ast->list.items[1]->symbol;
            } else if (ast->list.items[1]->type == AST_LIST &&
                       ast->list.items[1]->list.count > 0 &&
                       ast->list.items[1]->list.items[0]->type == AST_SYMBOL) {
                name = ast->list.items[1]->list.items[0]->symbol;
            }
            if (!name) break;

            /* Use a child scope for the pre-bind so the transient
             * scheme_mono never escapes into the persistent outer env. */
            InferEnv *def_child = infer_env_create_child(ctx->env);
            InferEnv *saved_env = ctx->env;
            ctx->env = def_child;

            Type *self_t = infer_fresh(ctx);
            infer_env_insert(def_child, name, scheme_mono(self_t));

            int body_idx = (int)ast->list.count - 1;
            while (body_idx > 2 &&
                   ast->list.items[body_idx]->type == AST_STRING)
                body_idx--;

            Type *val_t = infer_expr(ctx, ast->list.items[body_idx]);
            infer_constrain(ctx, self_t, val_t, ast->line, ast->column);

            /* Remove stale scheme for this name from outer env before generalising
             * to prevent old type vars from corrupting infer_free_vars_env        */
            infer_env_remove(saved_env, name);
            TypeScheme *sc = infer_generalise(ctx, val_t, saved_env);
            ctx->env = saved_env;
            infer_env_free(def_child);
            // Insert the fresh generalised scheme into the real outer env
            infer_env_insert(ctx->env, name, sc);
            break;
        }

        /* ---- if ------------------------------------------------------ */
        if (head->type == AST_SYMBOL && strcmp(head->symbol, "if") == 0) {
            if (ast->list.count >= 3) {
                Type *cond_t = infer_expr(ctx, ast->list.items[1]);
                infer_constrain(ctx, cond_t, type_bool(),
                                ast->list.items[1]->line,
                                ast->list.items[1]->column);
                Type *then_t = infer_expr(ctx, ast->list.items[2]);
                if (ast->list.count == 4) {
                    Type *else_t = infer_expr(ctx, ast->list.items[3]);
                    infer_constrain(ctx, then_t, else_t,
                                    ast->list.items[3]->line,
                                    ast->list.items[3]->column);
                }
                result = then_t;
            } else {
                result = infer_fresh(ctx);
            }
            break;
        }

        /* ---- let ----------------------------------------------------- */
        if (head->type == AST_SYMBOL && strcmp(head->symbol, "let") == 0) {
            result = infer_fresh(ctx);
            break;
        }

        /* ---- quote --------------------------------------------------- */
        if (head->type == AST_SYMBOL && strcmp(head->symbol, "quote") == 0) {
            result = type_list(infer_fresh(ctx));
            break;
        }

        /* ---- n-ary arithmetic — (+ a b c ...) ----------------------- */
        if (head->type == AST_SYMBOL &&
            (strcmp(head->symbol, "+") == 0 ||
             strcmp(head->symbol, "-") == 0 ||
             strcmp(head->symbol, "*") == 0 ||
             strcmp(head->symbol, "/") == 0) &&
            ast->list.count >= 2) {
            Type *num_t = infer_fresh(ctx);
            for (size_t i = 1; i < ast->list.count; i++) {
                Type *at = infer_expr(ctx, ast->list.items[i]);
                infer_constrain(ctx, at, num_t,
                                ast->list.items[i]->line,
                                ast->list.items[i]->column);
            }
            result = num_t;
            break;
        }

        /* ---- variadic call: look up if head is a variadic function --- */
        if (head->type == AST_SYMBOL) {
            TypeScheme *sc = infer_env_lookup(ctx->env, head->symbol);
            if (sc) {
                // Walk arrow chain to find if last param is List type
                Type *t = subst_apply(ctx->subst, sc->type);
                // Instantiate fresh
                if (sc->quantified_count > 0)
                    t = infer_instantiate(ctx, sc);
                // Count arrow params
                int arrow_count = 0;
                Type *walk = t;
                bool last_is_list = false;
                while (walk && walk->kind == TYPE_ARROW) {
                    arrow_count++;
                    if (walk->arrow_ret && walk->arrow_ret->kind != TYPE_ARROW) {
                        // last param
                        if (walk->arrow_param && walk->arrow_param->kind == TYPE_LIST)
                            last_is_list = true;
                    }
                    walk = walk->arrow_ret;
                }
                if (last_is_list && (int)ast->list.count - 1 > arrow_count - 1) {
                    // Variadic call — infer non-rest args normally,
                    // constrain all rest args to the list element type
                    Type *list_elem = infer_fresh(ctx);
                    // re-walk to get param types
                    Type *ft = t;
                    int regular = arrow_count - 1;
                    for (int i = 0; i < regular && i < (int)ast->list.count - 1; i++) {
                        if (ft->kind == TYPE_ARROW) {
                            Type *at = infer_expr(ctx, ast->list.items[i + 1]);
                            infer_constrain(ctx, at, ft->arrow_param,
                                           ast->list.items[i+1]->line,
                                           ast->list.items[i+1]->column);
                            ft = ft->arrow_ret;
                        }
                    }
                    // Rest args
                    for (int i = regular; i < (int)ast->list.count - 1; i++) {
                        Type *at = infer_expr(ctx, ast->list.items[i + 1]);
                        infer_constrain(ctx, at, list_elem,
                                       ast->list.items[i+1]->line,
                                       ast->list.items[i+1]->column);
                    }
                    // Return type is the final arrow return
                    while (ft && ft->kind == TYPE_ARROW) ft = ft->arrow_ret;
                    result = ft ? ft : infer_fresh(ctx);
                    break;
                }
            }
        }


        /* ---- function application ------------------------------------ */
        Type *fn_t  = infer_expr(ctx, head);
        Type *ret_t = infer_fresh(ctx);

        Type *expected = ret_t;
        for (int i = (int)ast->list.count - 1; i >= 1; i--) {
            Type *arg_t = infer_expr(ctx, ast->list.items[i]);
            expected = type_arrow(arg_t, expected);
        }

        infer_constrain(ctx, fn_t, expected, ast->line, ast->column);
        result = ret_t;
        break;
    }

    default:
        result = infer_fresh(ctx);
        break;
    }

    ast->inferred_type = result;
    return result;
}

/// Primitives Bootstrap

void infer_register_builtins(InferCtx *ctx) {
    /* ∀a. a → Set */
    Type *a  = infer_fresh(ctx);
    TypeScheme *set_sc = infer_generalise(ctx,
        type_arrow(a, type_set()), ctx->env);
    infer_env_insert(ctx->env, "set", set_sc);

    /* ∀a. Set → a → Set */
    Type *a2 = infer_fresh(ctx);
    TypeScheme *conj_sc = infer_generalise(ctx,
        type_arrow(type_set(), type_arrow(a2, type_set())), ctx->env);
    infer_env_insert(ctx->env, "conj",  conj_sc);
    infer_env_insert(ctx->env, "conj!", conj_sc);
    infer_env_insert(ctx->env, "disj",  conj_sc);
    infer_env_insert(ctx->env, "disj!", conj_sc);

    /* ∀a. Set → a → Bool */
    Type *a3 = infer_fresh(ctx);
    TypeScheme *contains_sc = infer_generalise(ctx,
        type_arrow(type_set(), type_arrow(a3, type_bool())), ctx->env);
    infer_env_insert(ctx->env, "contains?", contains_sc);

    /* Set → Int */
    infer_env_insert(ctx->env, "count",
        scheme_mono(type_arrow(type_set(), type_int())));

    /* ∀a. a → Bool */
    Type *a4 = infer_fresh(ctx);
    TypeScheme *set_pred_sc = infer_generalise(ctx,
        type_arrow(a4, type_bool()), ctx->env);
    infer_env_insert(ctx->env, "set?",        set_pred_sc);
    infer_env_insert(ctx->env, "map?",        set_pred_sc);
    infer_env_insert(ctx->env, "collection?", set_pred_sc);

    /* ∀a b. (a → b → a) → a → Coll → a   (foldl — generic collection) */
    Type *acc_t  = infer_fresh(ctx);
    Type *elem_t = infer_fresh(ctx);
    TypeScheme *foldl_sc = infer_generalise(ctx,
        type_arrow(
            type_arrow(acc_t, type_arrow(elem_t, acc_t)),
            type_arrow(acc_t,
                type_arrow(type_coll(), acc_t))),
        ctx->env);
    infer_env_insert(ctx->env, "foldl", foldl_sc);

    /* ∀a b. (a → b) → List a → List b   (map) */
    Type *ma = infer_fresh(ctx);
    Type *mb = infer_fresh(ctx);
    TypeScheme *map_sc = infer_generalise(ctx,
        type_arrow(
            type_arrow(ma, mb),
            type_arrow(type_list(ma), type_list(mb))),
        ctx->env);
    infer_env_insert(ctx->env, "map", map_sc);

    /* ∀a. (a → Bool) → List a → List a   (filter) */
    Type *fa = infer_fresh(ctx);
    TypeScheme *filter_sc = infer_generalise(ctx,
        type_arrow(
            type_arrow(fa, type_bool()),
            type_arrow(type_list(fa), type_list(fa))),
        ctx->env);
    infer_env_insert(ctx->env, "filter", filter_sc);

    // Arithmetic: ∀a. a -> List a -> a
    // The first arg is the accumulator, rest args come as a list.
    // HM treats (+ x y z) as variadic — special-cased in infer_expr.
    // Register as binary for 2-arg calls, handle n-ary in infer_expr.
    Type *aa = infer_fresh(ctx);
    TypeScheme *arith_sc = infer_generalise(ctx,
        type_arrow(aa, type_arrow(aa, aa)), ctx->env);
    infer_env_insert(ctx->env, "+", arith_sc);
    infer_env_insert(ctx->env, "-", arith_sc);
    infer_env_insert(ctx->env, "*", arith_sc);
    Type *da = infer_fresh(ctx);
    TypeScheme *div_sc = infer_generalise(ctx,
        type_arrow(da, type_arrow(da, da)), ctx->env);
    infer_env_insert(ctx->env, "/", div_sc);


    /* Comparison: ∀a. a → a → Bool */
    Type *ca = infer_fresh(ctx);
    TypeScheme *cmp_sc = infer_generalise(ctx,
        type_arrow(ca, type_arrow(ca, type_bool())), ctx->env);
    infer_env_insert(ctx->env, "=",  cmp_sc);
    infer_env_insert(ctx->env, "!=", cmp_sc);
    infer_env_insert(ctx->env, "<",  cmp_sc);
    infer_env_insert(ctx->env, ">",  cmp_sc);
    infer_env_insert(ctx->env, "<=", cmp_sc);
    infer_env_insert(ctx->env, ">=", cmp_sc);

    /* Logic */
    TypeScheme *logic_sc = scheme_mono(
        type_arrow(type_bool(), type_arrow(type_bool(), type_bool())));
    infer_env_insert(ctx->env, "and", logic_sc);
    infer_env_insert(ctx->env, "or",  logic_sc);

    /* Map builtins */
    Type *mk = infer_fresh(ctx);
    Type *mv = infer_fresh(ctx);
    TypeScheme *assoc_sc = infer_generalise(ctx,
        type_arrow(type_map(),
            type_arrow(mk, type_arrow(mv, type_map()))),
        ctx->env);
    infer_env_insert(ctx->env, "assoc",  assoc_sc);
    infer_env_insert(ctx->env, "assoc!", assoc_sc);

    Type *dk = infer_fresh(ctx);
    TypeScheme *dissoc_sc = infer_generalise(ctx,
        type_arrow(type_map(), type_arrow(dk, type_map())), ctx->env);
    infer_env_insert(ctx->env, "dissoc",  dissoc_sc);
    infer_env_insert(ctx->env, "dissoc!", dissoc_sc);

    infer_env_insert(ctx->env, "merge",
        scheme_mono(type_arrow(type_map(), type_arrow(type_map(), type_map()))));

    infer_env_insert(ctx->env, "keys",
        scheme_mono(type_arrow(type_map(), type_list(infer_fresh(ctx)))));
    infer_env_insert(ctx->env, "vals",
        scheme_mono(type_arrow(type_map(), type_list(infer_fresh(ctx)))));
}


/// Top-level Entry Point

Type *infer_toplevel(InferCtx *ctx, AST *ast) {
    /* 1. Constraint generation */
    Type *t = infer_expr(ctx, ast);
    if (ctx->had_error) return NULL;

    /* 2. Constraint solving */
    if (!infer_unify_all(ctx)) return NULL;

    /* 3. Zonking — apply substitution to all AST nodes */
    infer_zonk_ast(ctx, ast);

    return subst_apply(ctx->subst, t);
}


/// Pretty Printing

void infer_print_type(Type *t, Substitution *s) {
    if (!t) { printf("?"); return; }
    if (s) t = subst_apply(s, t);
    if (!t) { printf("?"); return; }

    switch (t->kind) {
    case TYPE_VAR:
        printf("'%c", 'a' + (t->var_id % 26));
        if (t->var_id >= 26) printf("%d", t->var_id / 26);
        break;
    case TYPE_ARROW:
        printf("(");
        infer_print_type(t->arrow_param, s);
        printf(" → ");
        infer_print_type(t->arrow_ret, s);
        printf(")");
        break;
    case TYPE_LIST:
        printf("List<");
        infer_print_type(t->list_elem, s);
        printf(">");
        break;
    default:
        printf("%s", type_to_string(t));
        break;
    }
}

void infer_print_scheme(TypeScheme *sc) {
    if (!sc) { printf("?"); return; }
    if (sc->quantified_count > 0) {
        printf("∀");
        for (int i = 0; i < sc->quantified_count; i++) {
            if (i > 0) printf(" ");
            printf("'%c", 'a' + (sc->quantified[i] % 26));
        }
        printf(". ");
    }
    infer_print_type(sc->type, NULL);
}

void infer_print_constraints(InferCtx *ctx) {
    fprintf(stderr, "[infer] %zu constraints:\n", ctx->constraint_count);
    for (size_t i = 0; i < ctx->constraint_count; i++) {
        TypeConstraint *c = &ctx->constraints[i];
        fprintf(stderr, "  ");
        infer_print_type(c->lhs, ctx->subst);
        fprintf(stderr, " ~ ");
        infer_print_type(c->rhs, ctx->subst);
        fprintf(stderr, "\n");
    }
}
