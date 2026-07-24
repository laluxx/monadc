#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "infer.h"
#include "types.h"
#include "reader.h"
#include "dep.h"

extern int g_trace_depth;
extern bool g_trace_enabled;
extern void shared_trace_indent(void);

static bool g_infer_trace_enabled = false;

void infer_set_trace(bool enabled) {
    g_infer_trace_enabled = enabled;
}

static bool infer_runtime_collection_helper(const char *name) {
    return name &&
           (strcmp(name, "rt_coll_concat") == 0 ||
            strcmp(name, "rt_coll_drop") == 0 ||
            strcmp(name, "rt_coll_empty") == 0 ||
            strcmp(name, "rt_coll_wrap") == 0);
}

static bool infer_is_cons_literal_tail(AST *ast) {
    if (!ast) return false;
    if (ast->type == AST_ARRAY) return true;
    if (ast->type == AST_LIST &&
        ast->list.count > 0 &&
        ast->list.items[0] &&
        ast->list.items[0]->type == AST_SYMBOL &&
        strcmp(ast->list.items[0]->symbol, "rt_coll_concat") == 0)
        return true;
    return false;
}

Type *infer_freshen_annotation_vars(InferCtx *ctx, Type *t,
                                    int *from, Type **to, int *count) {
    if (!t) return NULL;

    if (t->kind == TYPE_VAR) {
        for (int i = 0; i < *count; i++) {
            if (from[i] == t->var_id)
                return type_clone(to[i]);
        }
        if (*count < 64) {
            from[*count] = t->var_id;
            to[*count] = infer_fresh(ctx);
            (*count)++;
            return type_clone(to[*count - 1]);
        }
        return type_clone(t);
    }

    switch (t->kind) {
    case TYPE_LIST: {
        Type **items = NULL;
        if (t->list_count > 0) {
            items = malloc(sizeof(Type *) * t->list_count);
            for (int i = 0; i < t->list_count; i++)
                items[i] = infer_freshen_annotation_vars(ctx, t->list_types[i],
                                                         from, to, count);
        }
        Type *ret = type_list(items, t->list_count);
        free(items);
        if (t->list_elem)
            ret->list_elem = infer_freshen_annotation_vars(ctx, t->list_elem,
                                                           from, to, count);
        return ret;
    }
    case TYPE_COLL: {
        Type *ret = type_coll();
        ret->element_type = t->element_type
            ? infer_freshen_annotation_vars(ctx, t->element_type, from, to, count)
            : type_unknown();
        return ret;
    }
    case TYPE_ARR: {
        Type *ret = type_arr(t->arr_element_type
                                 ? infer_freshen_annotation_vars(ctx, t->arr_element_type,
                                                                 from, to, count)
                                 : NULL,
                             t->arr_size);
        ret->arr_is_fat = t->arr_is_fat;
        ret->arr_is_heap = t->arr_is_heap;
        return ret;
    }
    case TYPE_MAP:
        return type_map_of(
            t->map_key_type
                ? infer_freshen_annotation_vars(ctx, t->map_key_type, from, to, count)
                : NULL,
            t->map_value_type
                ? infer_freshen_annotation_vars(ctx, t->map_value_type, from, to, count)
                : NULL);
    case TYPE_ARROW:
        return type_arrow(infer_freshen_annotation_vars(ctx, t->arrow_param,
                                                        from, to, count),
                          infer_freshen_annotation_vars(ctx, t->arrow_ret,
                                                        from, to, count));
    case TYPE_APP:
        return type_app(t->app_constructor,
                        t->app_arg
                            ? infer_freshen_annotation_vars(ctx, t->app_arg,
                                                            from, to, count)
                            : type_unknown());
    case TYPE_OPTIONAL:
        return type_optional(t->element_type
                                 ? infer_freshen_annotation_vars(ctx, t->element_type,
                                                                 from, to, count)
                                 : type_unknown());
    case TYPE_PTR:
        return type_ptr(t->element_type
                            ? infer_freshen_annotation_vars(ctx, t->element_type,
                                                            from, to, count)
                            : NULL);
    default:
        return type_clone(t);
    }
}

static Type *infer_normalize_annotation_type(Type *t) {
    if (!t) return NULL;

    while (t &&
           t->kind == TYPE_LIST &&
           t->list_count == 1 &&
           t->list_types &&
           t->list_types[0]) {
        Type *inner = infer_normalize_annotation_type(t->list_types[0]);

        if (inner &&
            (inner->kind == TYPE_ARROW ||
             inner->kind == TYPE_FN)) {
            t = inner;
            continue;
        }

        break;
    }

    return t;
}

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

TypeScheme *infer_env_lookup(InferCtx *ctx, const char *name) {
    if (!ctx || !name) return NULL;
    // 1. Check local HM environment (for lambda params, local lets)
    for (InferEnv *cur = ctx->env; cur; cur = cur->parent) {
        size_t idx = infer_env_hash(name);
        InferEnvEntry *e = cur->buckets[idx];
        while (e) {
            if (strcmp(e->name, name) == 0) return e->scheme;
            e = e->next;
        }
    }
    // 2. Fallback to TT Global Environment (The Idris Bridge)
    // DISABLED: The LLVM Codegen phase occasionally passes a stale/freed dctx pointer,
    // leading to severe Use-After-Free segfaults.
    // This bridge is no longer necessary because the Dependent Elaborator now
    // pre-seeds `ast->inferred_type` on all nodes, giving HM inference the exact
    // mathematical types without needing global environment lookups.
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
    int root = subst_find(s, id);
    /* Never overwrite an existing concrete binding — doing so loses
     * the original type and can create substitution cycles where
     * subst_apply_depth chases var -> type -> var -> type forever. */
    if (s->bound[root]) return;
    s->bound[root] = t;
}

bool subst_union(Substitution *s, int a, int b) {
    int ra = subst_find(s, a);
    int rb = subst_find(s, b);
    if (ra == rb) return true;
    /* If both roots have concrete bindings, do not merge — the caller
     * (infer_unify_one_internal) will handle structural unification.
     * Merging here would silently overwrite one binding with the other,
     * which can create dangling references and substitution cycles.   */
    if (s->bound[ra] && s->bound[rb]) return true;
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
    /* Path compression directly on the AST node to massively improve performance */
    t->var_id = root;
    return t;
}

static Type *subst_apply_depth(Substitution *s, Type *t, int depth) {
    if (!t) return NULL;
    if (depth > 64) return t;  /* cycle guard: stop recursion */
    t = subst_apply_shallow(s, t);
    if (!t) return NULL;

    switch (t->kind) {
    case TYPE_VAR:
        return t;   /* unresolved free variable — leave as-is */

    case TYPE_LIST: {
        if (t->list_count == 0) return t;
        bool changed = false;
        Type **new_types = malloc(t->list_count * sizeof(Type*));
        for (int i = 0; i < t->list_count; i++) {
            new_types[i] = subst_apply_depth(s, t->list_types[i], depth + 1);
            if (new_types[i] != t->list_types[i]) changed = true;
        }
        if (!changed) {
            free(new_types);
            return t;
        }
        Type *ret = type_list(new_types, t->list_count);
        free(new_types);
        return ret;
    }

    case TYPE_OPTIONAL:
    case TYPE_PTR:
    case TYPE_COLL: {
        if (!t->element_type) return t;
        Type *inner = subst_apply_depth(s, t->element_type, depth + 1);
        if (inner == t->element_type) return t;
        Type *ret = calloc(1, sizeof(Type));
        ret->kind = t->kind;
        ret->element_type = inner;
        return ret;
    }

    case TYPE_ARR: {
        if (!t->arr_element_type) return t;
        Type *inner = subst_apply_depth(s, t->arr_element_type, depth + 1);
        if (inner == t->arr_element_type) return t;
        Type *ret = type_arr(inner, t->arr_size);
        ret->arr_is_fat = t->arr_is_fat;
        return ret;
    }

    case TYPE_MAP: {
        Type *key = t->map_key_type
            ? subst_apply_depth(s, t->map_key_type, depth + 1) : NULL;
        Type *value = t->map_value_type
            ? subst_apply_depth(s, t->map_value_type, depth + 1) : NULL;
        if (key == t->map_key_type && value == t->map_value_type) return t;
        return type_map_of(key, value);
    }

    case TYPE_ARROW: {
        Type *p = subst_apply_depth(s, t->arrow_param, depth + 1);
        Type *r = subst_apply_depth(s, t->arrow_ret,   depth + 1);
        if (p == t->arrow_param && r == t->arrow_ret) return t;
        return type_arrow(p, r);
    }

    case TYPE_APP: {
        if (!t->app_arg) return t;
        Type *arg = subst_apply_depth(s, t->app_arg, depth + 1);
        if (arg == t->app_arg) return t;
        return type_app(t->app_constructor, arg);
    }

    default:
        return t;
    }
}

Type *subst_apply(Substitution *s, Type *t) {
    return subst_apply_depth(s, t, 0);
}


/// Inference Context

InferCtx *infer_ctx_create(InferEnv *env, struct DepCtx *dctx, const char *filename) {
    InferCtx *ctx       = calloc(1, sizeof(InferCtx));
    ctx->subst          = subst_create();
    ctx->constraint_cap = 256;
    ctx->constraints    = malloc(sizeof(TypeConstraint) * ctx->constraint_cap);
    ctx->constraint_count = 0;
    ctx->env            = env;
    ctx->dctx           = dctx;
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
        for (int i = 0; i < t->list_count; i++) {
            if (infer_occurs(s, var_id, t->list_types[i])) return true;
        }
        return false;
    case TYPE_OPTIONAL:
    case TYPE_PTR:
    case TYPE_COLL:
        return infer_occurs(s, var_id, t->element_type);
    case TYPE_ARR:
        return infer_occurs(s, var_id, t->arr_element_type);
    case TYPE_ARROW:
        return infer_occurs(s, var_id, t->arrow_param)
            || infer_occurs(s, var_id, t->arrow_ret);
    case TYPE_APP:
        return infer_occurs(s, var_id, t->app_arg);
    case TYPE_MAP:
        return infer_occurs(s, var_id, t->map_key_type) ||
               infer_occurs(s, var_id, t->map_value_type);
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
    if (getenv("MONAD_INFER_CONSTRAINT_DEBUG")) {
        fprintf(stderr, "[infer-constraint] %s:%d:%d %s ~ %s\n",
                ctx && ctx->filename ? ctx->filename : "<unknown>",
                line, col, type_to_string(a), type_to_string(b));
    }
    ctx->constraints[ctx->constraint_count++] = (TypeConstraint){a, b, line, col};
}


/// Unification

static bool infer_unify_one_internal(InferCtx *ctx, Type *a, Type *b, int line, int col);

bool infer_unify_one(InferCtx *ctx, Type *a, Type *b, int line, int col) {
    if (g_trace_enabled) {
        shared_trace_indent();
        fprintf(stderr, "├─ \033[36mHM Unify\033[0m ");
        infer_print_type(a, ctx->subst);
        fprintf(stderr, " ~ ");
        infer_print_type(b, ctx->subst);
        fprintf(stderr, "\n");
        g_trace_depth++;
    }

    bool ok = infer_unify_one_internal(ctx, a, b, line, col);

    if (g_trace_enabled) {
        g_trace_depth--;
        shared_trace_indent();
        if (ok) fprintf(stderr, "└─ \033[32mOK\033[0m\n");
        else fprintf(stderr, "└─ \033[31mFAIL\033[0m\n");
    }
    return ok;
}

// Rename your existing function
static bool infer_unify_one_internal(InferCtx *ctx, Type *a, Type *b, int line, int col) {
    Substitution *s = ctx->subst;
    a = subst_apply_shallow(s, a);
    b = subst_apply_shallow(s, b);

    if (!a || !b) return true;  /* NULL ~ anything: allow for now */

    if (a->kind == TYPE_UNKNOWN || b->kind == TYPE_UNKNOWN) return true;

    /* Both free variables — merge their roots */
    if (a->kind == TYPE_VAR && b->kind == TYPE_VAR) {
        int ra = subst_find(s, a->var_id);
        int rb = subst_find(s, b->var_id);
        if (ra == rb) return true;
        /* If either root is already bound to a concrete type, unify
         * those concrete types structurally rather than merging roots.
         * Merging two bound roots silently drops one binding.         */
        if (s->bound[ra] && s->bound[rb])
            return infer_unify_one_internal(ctx, s->bound[ra], s->bound[rb], line, col);
        if (s->bound[ra]) { subst_bind(s, rb, s->bound[ra]); return true; }
        if (s->bound[rb]) { subst_bind(s, ra, s->bound[rb]); return true; }
        subst_union(s, a->var_id, b->var_id);
        return true;
    }

    /* Left is a free variable — bind it */
    if (a->kind == TYPE_VAR) {
        int root = subst_find(s, a->var_id);
        /* If already bound, unify the existing binding with b */
        if (s->bound[root])
            return infer_unify_one_internal(ctx, s->bound[root], b, line, col);
        if (infer_occurs(s, root, b)) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "%s:%d:%d: type error: infinite type (occurs check failed)",
                     ctx->filename, line, col);
            ctx->had_error = true;
            return false;
        }
        /* Never bind a free variable directly to Nil — Nil is not a
         * concrete element type, it only makes sense as Optional(a).
         * Binding 'x ~ Nil poisons any later constraint 'x ~ element_type
         * because Nil wins the race and element_type never gets a chance.
         * Instead bind to Optional(fresh) so subsequent constraints can
         * still resolve the inner element type correctly.               */
        if (b->kind == TYPE_NIL) {
            Type *inner = infer_fresh(ctx);
            subst_bind(s, root, type_optional(inner));
            return true;
        }
        subst_bind(s, root, b);
        return true;
    }

    /* Right is a free variable — bind it */
    if (b->kind == TYPE_VAR) {
        int root = subst_find(s, b->var_id);
        /* If already bound, unify the existing binding with a */
        if (s->bound[root])
            return infer_unify_one_internal(ctx, a, s->bound[root], line, col);
        if (infer_occurs(s, root, a)) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "%s:%d:%d: type error: infinite type (occurs check failed)",
                     ctx->filename, line, col);
            ctx->had_error = true;
            return false;
        }
        /* Same Nil-binding guard for the right-hand variable */
        if (a->kind == TYPE_NIL) {
            Type *inner = infer_fresh(ctx);
            subst_bind(s, root, type_optional(inner));
            return true;
        }
        subst_bind(s, root, a);
        return true;
    }

    /* Source-level Bool is the core finite set; TYPE_BOOL is the optimized
     * compiler truth representation used by primitive operations. */
    if (type_is_bool(a) && type_is_bool(b)) return true;

    // Both ground — must match structurally
    if (a->kind != b->kind) {
        /* A literal's ground representation may be checked against a finite
         * set here; infer_validate_calls performs the value-level membership
         * proof and annotates the literal with the finite type. */
        if ((a->kind == TYPE_FINITE_SET &&
             (b->kind == TYPE_INT || b->kind == TYPE_FLOAT ||
              b->kind == TYPE_CHAR || b->kind == TYPE_STRING ||
              b->kind == TYPE_KEYWORD)) ||
            (b->kind == TYPE_FINITE_SET &&
             (a->kind == TYPE_INT || a->kind == TYPE_FLOAT ||
              a->kind == TYPE_CHAR || a->kind == TYPE_STRING ||
              a->kind == TYPE_KEYWORD)))
            return true;
        /* TYPE_FN (unannotated Fn) is compatible with any arrow type */
        if (a->kind == TYPE_FN && b->kind == TYPE_ARROW) return true;

        /* Collection acting as a function: (Coll -> 'a) ~ (Int -> 'a)
         * SAFETY GUARD: only allow this coercion when the arrow param is
         * already a concrete integer/char type or an unbound fresh var.
         * Never allow it when the arrow param is itself an arrow type —
         * that means we are inside a polymorphic instantiation and the
         * coercion would bind a shared type var to both String and an
         * arrow type, creating a cycle in subst_apply.                  */
        if ((a->kind == TYPE_COLL || a->kind == TYPE_LIST || a->kind == TYPE_ARR) &&
            b->kind == TYPE_ARROW) {
            /* Collection indexing coercion: Coll a ~ (Int -> a)
             * Only applies to actual collection types, never to String.
             * String indexing is handled entirely in codegen; exposing it
             * here as an arrow type causes cascading unification explosions
             * when String appears as an argument to polymorphic operators
             * like = and >= which have type forall a. a -> a -> Bool.
             * Guard: skip coercion only when the arrow param is a complex
             * type (another arrow, collection, bool, float, string) that
             * can never be an index. Allow when param is Int, Char, or an
             * unbound fresh variable — those are all valid index types.  */
            Type *bp = subst_apply_shallow(ctx->subst, b->arrow_param);
            if (bp && (bp->kind == TYPE_ARROW || bp->kind == TYPE_FN ||
                       bp->kind == TYPE_COLL  || bp->kind == TYPE_LIST ||
                       bp->kind == TYPE_ARR   || bp->kind == TYPE_STRING ||
                       type_is_bool(bp)        || bp->kind == TYPE_FLOAT)) {
                goto skip_coll_as_fn_ab;
            }
            {
                bool ok = infer_unify_one(ctx, type_int(), b->arrow_param, line, col);
                if (!ok) return false;
                if (a->kind == TYPE_COLL) return infer_unify_one(ctx, a->element_type, b->arrow_ret, line, col);
                if (a->kind == TYPE_ARR) return infer_unify_one(ctx, a->arr_element_type, b->arrow_ret, line, col);
                return true;
            }
            skip_coll_as_fn_ab:;
        }
        if ((b->kind == TYPE_COLL || b->kind == TYPE_LIST || b->kind == TYPE_ARR) &&
            a->kind == TYPE_ARROW) {
            Type *ap = subst_apply_shallow(ctx->subst, a->arrow_param);
            if (ap && (ap->kind == TYPE_ARROW || ap->kind == TYPE_FN ||
                       ap->kind == TYPE_COLL  || ap->kind == TYPE_LIST ||
                       ap->kind == TYPE_ARR   || ap->kind == TYPE_STRING ||
                       type_is_bool(ap)        || ap->kind == TYPE_FLOAT)) {
                goto skip_coll_as_fn_ba;
            }
            {
                bool ok = infer_unify_one(ctx, a->arrow_param, type_int(), line, col);
                if (!ok) return false;
                if (b->kind == TYPE_COLL) return infer_unify_one(ctx, a->arrow_ret, b->element_type, line, col);
                if (b->kind == TYPE_ARR) return infer_unify_one(ctx, a->arrow_ret, b->arr_element_type, line, col);
                return true;
            }
            skip_coll_as_fn_ba:;
        }

        if (a->kind == TYPE_ARROW && b->kind == TYPE_FN) return true;
        /* TYPE_FN ~ TYPE_FN always ok */
        if (a->kind == TYPE_FN && b->kind == TYPE_FN) return true;
        /* TYPE_INT_ARBITRARY ~ TYPE_INT (and vice versa) — widen to i64 */
        if (a->kind == TYPE_INT_ARBITRARY && b->kind == TYPE_INT) return true;
        if (a->kind == TYPE_INT && b->kind == TYPE_INT_ARBITRARY) return true;
        /* TYPE_INT_ARBITRARY ~ TYPE_INT_ARBITRARY — must match width+sign */
        if (a->kind == TYPE_INT_ARBITRARY && b->kind == TYPE_INT_ARBITRARY)
            return (a->numeric_width == b->numeric_width &&
                    a->numeric_signed == b->numeric_signed);
        /* TYPE_F80 ~ TYPE_FLOAT */
        if (a->kind == TYPE_F80 && b->kind == TYPE_FLOAT) return true;
        if (a->kind == TYPE_FLOAT && b->kind == TYPE_F80) return true;
        /* Implicit Int -> Float coercion */
        if (a->kind == TYPE_INT && b->kind == TYPE_FLOAT) return true;
        if (a->kind == TYPE_FLOAT && b->kind == TYPE_INT) return true;
        if (a->kind == TYPE_INT_ARBITRARY && b->kind == TYPE_FLOAT) return true;
        if (a->kind == TYPE_FLOAT && b->kind == TYPE_INT_ARBITRARY) return true;
        /* Ratio is a numeric type — compatible with Int and Float */
        if (a->kind == TYPE_RATIO && b->kind == TYPE_INT) return true;
        if (a->kind == TYPE_INT && b->kind == TYPE_RATIO) return true;
        if (a->kind == TYPE_RATIO && b->kind == TYPE_FLOAT) return true;
        if (a->kind == TYPE_FLOAT && b->kind == TYPE_RATIO) return true;
        if (a->kind == TYPE_RATIO && b->kind == TYPE_INT_ARBITRARY) return true;
        if (a->kind == TYPE_INT_ARBITRARY && b->kind == TYPE_RATIO) return true;
        /* Ratio ~ Ratio always ok */
        if (a->kind == TYPE_RATIO && b->kind == TYPE_RATIO) return true;
        /* TYPE_INT ~ TYPE_CHAR — chars are small integers, coercion is always valid */
        if (a->kind == TYPE_INT && b->kind == TYPE_CHAR) return true;
        if (a->kind == TYPE_CHAR && b->kind == TYPE_INT) return true;
        /* TYPE_BYTE ~ TYPE_CHAR — byte and char share the same runtime domain. */
        if (a->kind == TYPE_BYTE && b->kind == TYPE_CHAR) return true;
        if (a->kind == TYPE_CHAR && b->kind == TYPE_BYTE) return true;
        /* TYPE_INT_ARBITRARY ~ TYPE_CHAR — same reasoning */
        if (a->kind == TYPE_INT_ARBITRARY && b->kind == TYPE_CHAR) return true;
        if (a->kind == TYPE_CHAR && b->kind == TYPE_INT_ARBITRARY) return true;
        /* Single-element TYPE_LIST is a parenthesized scalar type, e.g. (Float)
         * or (Coll :: a). Unwrap it before collection compatibility below;
         * otherwise Coll ~ (Coll :: a) is mistaken for collection element
         * unification and tries to solve a ~ Coll :: a. */
        if (a->kind == TYPE_LIST && a->list_count == 1 && b->kind != TYPE_LIST)
            return infer_unify_one_internal(ctx, a->list_types[0], b, line, col);
        if (b->kind == TYPE_LIST && b->list_count == 1 && a->kind != TYPE_LIST)
            return infer_unify_one_internal(ctx, a, b->list_types[0], line, col);

        /* TYPE_COLL is compatible with any collection type.  A TYPE_LIST with
         * more than one item is a tuple/product value, so Coll element
         * unification must treat it as one element instead of flattening it.
         * This preserves collection-of-pair results such as zip :: [a] -> [b]
         * -> [(a, b)]. */
        if (a->kind == TYPE_COLL && b->kind == TYPE_LIST) {
            if (b->list_count == 1) {
                return infer_unify_one(ctx, a->element_type, b->list_types[0], line, col);
            }
            if (!a->element_type || a->element_type->kind == TYPE_UNKNOWN) return true;
            return infer_unify_one(ctx, a->element_type, b, line, col);
        }
        if (b->kind == TYPE_COLL && a->kind == TYPE_LIST) {
            if (a->list_count == 1) {
                return infer_unify_one(ctx, b->element_type, a->list_types[0], line, col);
            }
            if (!b->element_type || b->element_type->kind == TYPE_UNKNOWN) return true;
            return infer_unify_one(ctx, b->element_type, a, line, col);
        }
        if (a->kind == TYPE_COLL && b->kind == TYPE_ARR) {
            return infer_unify_one(ctx, a->element_type, b->arr_element_type, line, col);
        }
        if (b->kind == TYPE_COLL && a->kind == TYPE_ARR) {
            return infer_unify_one(ctx, b->element_type, a->arr_element_type, line, col);
        }
        if (a->kind == TYPE_COLL && b->kind == TYPE_STRING) {
            Type *tc = type_char();
            /* Substitutions retain concrete type pointers, so TC becomes
             * owned by the inference context when an element variable binds
             * to it. Freeing it here leaves the substitution dangling. */
            return infer_unify_one(ctx, a->element_type, tc, line, col);
        }
        if (b->kind == TYPE_COLL && a->kind == TYPE_STRING) {
            Type *tc = type_char();
            return infer_unify_one(ctx, b->element_type, tc, line, col);
        }
        if (a->kind == TYPE_COLL && b->kind == TYPE_SET   ) return true;
        if (b->kind == TYPE_COLL && a->kind == TYPE_SET   ) return true;
        if (a->kind == TYPE_APP && b->kind  == TYPE_LAYOUT) return true;
        if (b->kind == TYPE_APP && a->kind  == TYPE_LAYOUT) return true;

        /* TYPE_APP ~ TYPE_APP: both are type constructor applications (e.g. Maybe a ~ Maybe b) */
        if (a->kind == TYPE_APP && b->kind == TYPE_APP) {
            if (a->app_constructor && b->app_constructor &&
                strcmp(a->app_constructor, b->app_constructor) == 0) {
                /* Same constructor: unify arguments */
                if (a->app_arg && b->app_arg)
                    return infer_unify_one(ctx, a->app_arg, b->app_arg, line, col);
                return true;
            }
            char a_str[64], b_str[64];
            snprintf(a_str, sizeof(a_str), "%s", type_to_string(a));
            snprintf(b_str, sizeof(b_str), "%s", type_to_string(b));
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "%s:%d:%d: type error: cannot unify %s with %s",
                     ctx->filename, line, col, a_str, b_str);
            ctx->had_error = true;
            return false;
        }

        /* TYPE_APP ~ TYPE_LAYOUT: e.g. (? a) ~ Maybe
         * This happens when a monomorphic ADT (Maybe) is unified with
         * a polymorphic instantiation (Maybe a). Since codegen erases
         * type parameters to opaque pointers, we accept this by checking
         * that the constructor name matches the layout name. */
        if (a->kind == TYPE_APP && b->kind == TYPE_LAYOUT) {
            if (a->app_constructor && b->layout_name &&
                strcmp(a->app_constructor, b->layout_name) == 0)
                return true;
            /* Also accept __type_ prefix stripping */
            const char *bname = b->layout_name ? b->layout_name : "";
            if (strncmp(bname, "__type_", 7) == 0 && a->app_constructor &&
                strcmp(a->app_constructor, bname + 7) == 0)
                return true;
        }
        if (b->kind == TYPE_APP && a->kind == TYPE_LAYOUT) {
            if (b->app_constructor && a->layout_name &&
                strcmp(b->app_constructor, a->layout_name) == 0)
                return true;
            const char *aname = a->layout_name ? a->layout_name : "";
            if (strncmp(aname, "__type_", 7) == 0 && b->app_constructor &&
                strcmp(b->app_constructor, aname + 7) == 0)
                return true;
        }

        /* Hard error: Coll cannot be a scalar primitive */
        /* TYPE_OPTIONAL ~ TYPE_OPTIONAL */
        if (a->kind == TYPE_OPTIONAL && b->kind == TYPE_OPTIONAL)
            return infer_unify_one(ctx, a->element_type, b->element_type, line, col);

        /* nil strictly unifies ONLY with TYPE_OPTIONAL or itself */
        if (a->kind == TYPE_NIL && b->kind == TYPE_OPTIONAL) return true;
        if (a->kind == TYPE_OPTIONAL && b->kind == TYPE_NIL) return true;
        if (a->kind == TYPE_NIL && b->kind == TYPE_NIL) return true;

        if ((a->kind == TYPE_COLL && (b->kind == TYPE_INT || b->kind == TYPE_FLOAT || b->kind == TYPE_BOOL)) ||
            (b->kind == TYPE_COLL && (a->kind == TYPE_INT || a->kind == TYPE_FLOAT || a->kind == TYPE_BOOL))) {
            char a_str[64], b_str[64];
            snprintf(a_str, sizeof(a_str), "%s", type_to_string(a));
            snprintf(b_str, sizeof(b_str), "%s", type_to_string(b));
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "%s:%d:%d: type error: cannot use %s as a Collection (%s)",
                     ctx->filename, line, col,
                     (a->kind == TYPE_COLL ? b_str : a_str),
                     (a->kind == TYPE_COLL ? a_str : b_str));
            ctx->had_error = true;
            return false;
        }

        char a_str[64], b_str[64];
        snprintf(a_str, sizeof(a_str), "%s", type_to_string(a));
        snprintf(b_str, sizeof(b_str), "%s", type_to_string(b));

        if (a->kind == TYPE_NIL || b->kind == TYPE_NIL || a->kind == TYPE_OPTIONAL || b->kind == TYPE_OPTIONAL) {
            Type *non_opt = (a->kind != TYPE_OPTIONAL && a->kind != TYPE_NIL) ? a : b;
            char req_str[64];
            snprintf(req_str, sizeof(req_str), "%s", type_to_string(non_opt));
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "%s:%d:%d: type error: cannot unify %s with %s.\n"
                     "  - Hint: If a function can return nil, its return type should end with '?' (e.g. %s?)",
                     ctx->filename, line, col, a_str, b_str, req_str);
        } else {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "%s:%d:%d: type error: cannot unify %s with %s",
                     ctx->filename, line, col, a_str, b_str);
        }
        ctx->had_error = true;
        return false;
    }


    /* Recurse for compound types */
    switch (a->kind) {
    case TYPE_LIST:
        if (a->list_count == 1 && b->list_count > 1) {
            for (int i = 0; i < b->list_count; i++) {
                if (!infer_unify_one(ctx, a->list_types[0], b->list_types[i], line, col)) return false;
            }
            return true;
        }
        if (b->list_count == 1 && a->list_count > 1) {
            for (int i = 0; i < a->list_count; i++) {
                if (!infer_unify_one(ctx, a->list_types[i], b->list_types[0], line, col)) return false;
            }
            return true;
        }
        if (a->list_count != b->list_count) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "%s:%d:%d: type error: tuple size mismatch (%d vs %d)",
                     ctx->filename, line, col, a->list_count, b->list_count);
            ctx->had_error = true;
            return false;
        }
        for (int i = 0; i < a->list_count; i++) {
            if (!infer_unify_one(ctx, a->list_types[i], b->list_types[i], line, col)) return false;
        }
        return true;

    case TYPE_APP:
        if (a->app_constructor && b->app_constructor &&
            strcmp(a->app_constructor, b->app_constructor) != 0) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "%s:%d:%d: type error: cannot unify %s with %s",
                     ctx->filename, line, col, type_to_string(a), type_to_string(b));
            ctx->had_error = true;
            return false;
        }
        if (a->app_arg && b->app_arg)
            return infer_unify_one(ctx, a->app_arg, b->app_arg, line, col);
        return true;

    case TYPE_MAP:
        if (a->map_key_type && b->map_key_type &&
            !infer_unify_one(ctx, a->map_key_type, b->map_key_type, line, col))
            return false;
        if (a->map_value_type && b->map_value_type)
            return infer_unify_one(ctx, a->map_value_type, b->map_value_type,
                                   line, col);
        return true;

    case TYPE_COLL:
        return infer_unify_one(ctx, a->element_type, b->element_type, line, col);

    case TYPE_OPTIONAL:
        return infer_unify_one(ctx, a->element_type, b->element_type, line, col);

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
        for (int i = 0; i < t->list_count; i++) {
            infer_free_vars_type(s, t->list_types[i], out, count, cap);
        }
        break;
    case TYPE_OPTIONAL:
    case TYPE_PTR:
    case TYPE_COLL:
        infer_free_vars_type(s, t->element_type, out, count, cap);
        break;
    case TYPE_ARR:
        infer_free_vars_type(s, t->arr_element_type, out, count, cap);
        break;
    case TYPE_APP:
        infer_free_vars_type(s, t->app_arg, out, count, cap);
        break;
    case TYPE_MAP:
        infer_free_vars_type(s, t->map_key_type, out, count, cap);
        infer_free_vars_type(s, t->map_value_type, out, count, cap);
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
    if (!env) return;
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

static Type *infer_substitute_vars(Type *t, int *from, Type **to, int count) {
    if (!t) return NULL;

    if (t->kind == TYPE_VAR) {
        for (int i = 0; i < count; i++) {
            if (t->var_id == from[i])
                return type_clone(to[i]);
        }
        return type_var(t->var_id);
    }

    switch (t->kind) {
    case TYPE_ARROW:
        return type_arrow(infer_substitute_vars(t->arrow_param, from, to, count),
                          infer_substitute_vars(t->arrow_ret,   from, to, count));

    case TYPE_LIST: {
        if (t->list_count > 0 && t->list_types) {
            Type **items = malloc(sizeof(Type *) * t->list_count);
            for (int i = 0; i < t->list_count; i++)
                items[i] = infer_substitute_vars(t->list_types[i], from, to, count);
            Type *ret = type_list(items, t->list_count);
            for (int i = 0; i < t->list_count; i++) type_free(items[i]);
            free(items);
            return ret;
        }
        Type *ret = type_list(NULL, 0);
        if (t->list_elem)
            ret->list_elem = infer_substitute_vars(t->list_elem, from, to, count);
        return ret;
    }

    case TYPE_COLL: {
        Type *ret = type_coll();
        if (t->element_type)
            ret->element_type = infer_substitute_vars(t->element_type, from, to, count);
        return ret;
    }

    case TYPE_ARR: {
        Type *elem = t->arr_element_type
            ? infer_substitute_vars(t->arr_element_type, from, to, count)
            : NULL;
        Type *ret = type_arr(elem, t->arr_size);
        ret->arr_is_fat = t->arr_is_fat;
        ret->arr_is_heap = t->arr_is_heap;
        return ret;
    }

    case TYPE_PTR:
        return type_ptr(t->element_type
            ? infer_substitute_vars(t->element_type, from, to, count)
            : NULL);

    case TYPE_OPTIONAL:
        return type_optional(t->element_type
            ? infer_substitute_vars(t->element_type, from, to, count)
            : type_unknown());

    case TYPE_APP:
        return type_app(t->app_constructor,
                        t->app_arg
                            ? infer_substitute_vars(t->app_arg, from, to, count)
                            : type_unknown());

    case TYPE_MAP:
        return type_map_of(
            t->map_key_type
                ? infer_substitute_vars(t->map_key_type, from, to, count) : NULL,
            t->map_value_type
                ? infer_substitute_vars(t->map_value_type, from, to, count) : NULL);

    case TYPE_FN: {
        FnParam *params = NULL;
        if (t->param_count > 0 && t->params) {
            params = calloc((size_t)t->param_count, sizeof(FnParam));
            for (int i = 0; i < t->param_count; i++) {
                params[i].name = t->params[i].name ? strdup(t->params[i].name) : NULL;
                params[i].type = t->params[i].type
                    ? infer_substitute_vars(t->params[i].type, from, to, count)
                    : NULL;
                params[i].optional = t->params[i].optional;
                params[i].rest = t->params[i].rest;
            }
        }
        return type_fn(params, t->param_count,
                       t->return_type
                           ? infer_substitute_vars(t->return_type, from, to, count)
                           : NULL);
    }

    default:
        return type_clone(t);
    }
}

static int infer_fresh_not_quantified(InferCtx *ctx, int *quantified, int count) {
    int id = subst_fresh(ctx->subst);
    while (int_array_contains(quantified, count, id))
        id = subst_fresh(ctx->subst);
    return id;
}

Type *infer_instantiate(InferCtx *ctx, TypeScheme *scheme) {
    if (scheme->quantified_count == 0)
        return subst_apply(ctx->subst, scheme->type);

    Type *fresh[INFER_MAX_VARS];
    for (int i = 0; i < scheme->quantified_count; i++) {
        fresh[i] = type_var(infer_fresh_not_quantified(ctx, scheme->quantified,
                                                       scheme->quantified_count));
    }

    return infer_substitute_vars(scheme->type, scheme->quantified, fresh,
                                 scheme->quantified_count);
}

Type *infer_instantiate_with_subst(InferCtx *ctx, TypeScheme *scheme,
                                    TypeSubst *ts) {
    ts->count = scheme->quantified_count;
    ts->from  = malloc(sizeof(int)   * (ts->count ? ts->count : 1));
    ts->to    = malloc(sizeof(Type*) * (ts->count ? ts->count : 1));

    if (scheme->quantified_count == 0) {
        return subst_apply(ctx->subst, scheme->type);
    }

    for (int i = 0; i < scheme->quantified_count; i++) {
        ts->from[i]  = scheme->quantified[i];
        ts->to[i]    = type_var(infer_fresh_not_quantified(ctx, scheme->quantified,
                                                           scheme->quantified_count));
    }

    return infer_substitute_vars(scheme->type, ts->from, ts->to, ts->count);
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

    /* Resolve symbol types from env so validate_calls can see them */
    if (ast->type == AST_SYMBOL && !ast->inferred_type) {
        TypeScheme *sc = infer_env_lookup(ctx, ast->symbol);
        if (sc) ast->inferred_type = infer_instantiate(ctx, sc);
    }

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
    case AST_LAMBDA: {
        InferEnv *child = infer_env_create_child(ctx->env);
        InferEnv *saved = ctx->env;
        ctx->env = child;
        for (int i = 0; i < ast->lambda.param_count; i++) {
            infer_env_insert(child, ast->lambda.params[i].name, scheme_mono(type_unknown()));
        }
        for (int i = 0; i < ast->lambda.body_count; i++)
            infer_zonk_ast(ctx, ast->lambda.body_exprs[i]);
        ctx->env = saved;
        infer_env_free(child);
        break;
    }
    default:
        break;
    }
}


/// Type Inference — Expression Walk

static bool infer_ast_numeric_index(AST *ast, int *out_index) {
    if (!ast || ast->type != AST_NUMBER)
        return false;

    long long n = (long long)ast->number;
    if ((double)n != ast->number)
        return false;

    if (out_index)
        *out_index = (int)n;
    return true;
}

static bool infer_ast_was_postfix_index(AST *call, AST *receiver) {
    if (!call || !receiver)
        return false;

    return call->line == receiver->line &&
           call->column == receiver->end_column;
}

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

static bool infer_type_contains_unknown_or_var(Type *t) {
    if (!t) return true;

    switch (t->kind) {
    case TYPE_UNKNOWN:
    case TYPE_VAR:
        return true;
    case TYPE_ARROW:
        return infer_type_contains_unknown_or_var(t->arrow_param) ||
               infer_type_contains_unknown_or_var(t->arrow_ret);
    case TYPE_FN:
        for (int i = 0; i < t->param_count; i++)
            if (infer_type_contains_unknown_or_var(t->params[i].type))
                return true;
        return infer_type_contains_unknown_or_var(t->return_type);
    case TYPE_LIST:
        if (t->list_elem && infer_type_contains_unknown_or_var(t->list_elem))
            return true;
        for (int i = 0; i < t->list_count; i++)
            if (infer_type_contains_unknown_or_var(t->list_types[i]))
                return true;
        return false;
    case TYPE_COLL:
        return infer_type_contains_unknown_or_var(t->element_type);
    case TYPE_ARR:
        return infer_type_contains_unknown_or_var(t->arr_element_type);
    case TYPE_SET:
        return infer_type_contains_unknown_or_var(t->element_type);
    case TYPE_MAP:
        return infer_type_contains_unknown_or_var(t->map_key_type) ||
               infer_type_contains_unknown_or_var(t->map_value_type);
    case TYPE_PTR:
    case TYPE_OPTIONAL:
        return infer_type_contains_unknown_or_var(t->element_type);
    case TYPE_APP:
        return infer_type_contains_unknown_or_var(t->app_arg);
    default:
        return false;
    }
}

Type *infer_expr(InferCtx *ctx, AST *ast) {
    if (!ast) return type_unknown();

    Type *result = NULL;

    switch (ast->type) {

    case AST_NUMBER: {
        bool is_float = false;
        if (ast->literal_str) {
            bool radix_literal =
                ast->literal_str[0] == '0' &&
                (ast->literal_str[1] == 'x' || ast->literal_str[1] == 'X' ||
                 ast->literal_str[1] == 'b' || ast->literal_str[1] == 'B' ||
                 ast->literal_str[1] == 'o' || ast->literal_str[1] == 'O');
            if (!radix_literal) {
                for (const char *p = ast->literal_str; *p; p++)
                    if (*p == '.' || *p == 'e' || *p == 'E') { is_float = true; break; }
            }
        } else {
            is_float = (ast->number != (double)(int64_t)ast->number);
        }
        /* Additional check: if the dep checker already determined this is
         * a Float from its type annotation, respect that even if the literal
         * looks like an integer (e.g. 2.0 desugared to 2 losing the dot). */
        if (!is_float && ast->inferred_type &&
            ast->inferred_type->kind == TYPE_FLOAT) {
            is_float = true;
        }
        result = is_float ? type_float() : type_int();
        break;
    }

    case AST_STRING:
        result = type_string();
        break;

    case AST_PATH:
        result = type_path();
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
        if (finite_type_set_member_type_count(ast->symbol) > 1) {
            /* Overlapping singleton sets are resolved by the expected type
             * at the call/check site, not by global declaration order. */
            result = infer_fresh(ctx);
            break;
        }
        const FiniteTypeSetEntry *finite =
            finite_type_set_lookup_member(ast->symbol, NULL);
        if (finite) {
            result = type_finite_set(finite->name, finite->member_count);
            break;
        }
        if (strcmp(ast->symbol, "nil")   == 0) { result = type_nil();  break; }
        if (strcmp(ast->symbol, "undefined") == 0) {
            result = infer_fresh(ctx);
            break;
        }
        /* Explicit expression hole — ? becomes a fresh type variable.
         * The solved type will be printed after inference completes.  */
        if (strcmp(ast->symbol, "?") == 0) {
            result = infer_fresh(ctx);
            ctx->has_holes = true;
            ctx->hole_positions[ctx->hole_count % INFER_MAX_HOLES] =
                (InferHole){ .line = ast->line, .col = ast->column,
                             .var_id = result->var_id };
            ctx->hole_count++;
            break;
        }

        TypeScheme *sc = infer_env_lookup(ctx, ast->symbol);
        if (!sc) {
            /* Unbound variable — allocate a fresh type var but mark the
             * AST node so that if unification later fails we can emit a
             * precise "unbound variable" diagnostic instead of a cryptic
             * unification error.  We store the name in error_msg only if
             * no prior error has been recorded.                          */
            result = infer_fresh(ctx);
            if (!ctx->had_error && ctx->error_msg[0] == '\0') {
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                         "%s:%d:%d: warning: unbound variable '%s' "
                         "(type inferred from context)",
                         ctx->filename, ast->line, ast->column,
                         ast->symbol);
            }
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

    case AST_TYPE_SET: {
        if (!finite_type_set_register_ast(ast->type_set.name,
                                          ast->type_set.members,
                                          ast->type_set.member_count)) {
            ctx->had_error = true;
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "%s:%d:%d: error: finite type set '%s' has invalid or duplicate members",
                     ctx->filename, ast->line, ast->column,
                     ast->type_set.name);
        }
        result = type_unit();
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
        result = type_map_of(key_t, val_t);
        break;
    }

    case AST_ARRAY: {
        if (ast->array.element_count == 0) {
            result = type_coll();
            result->element_type = infer_fresh(ctx);
            break;
        }

        Type *elem_t = infer_fresh(ctx);
        for (size_t i = 0; i < ast->array.element_count; i++) {
            Type *et = infer_expr(ctx, ast->array.elements[i]);
            infer_constrain(ctx, et, elem_t, ast->line, ast->column);
        }
        result = ast->array.is_heap
            ? type_arr_heap(elem_t)
            : type_arr(elem_t, (int)ast->array.element_count);
        break;
    }

    case AST_LAMBDA: {
        InferEnv *child = infer_env_create_child(ctx->env);
        InferEnv *saved = ctx->env;
        ctx->env        = child;

        Type **param_types = malloc(sizeof(Type *) * (ast->lambda.param_count + 1));
        int ann_from[64];
        Type *ann_to[64];
        int ann_count = 0;
        for (int i = 0; i < ast->lambda.param_count; i++) {
            ASTParam *p = &ast->lambda.params[i];
            Type *pt;
            if (p->is_rest) {
                Type *elem = NULL;
                if (p->type_name) {
                    elem = type_from_name(p->type_name);
                    if (elem)
                        elem = infer_freshen_annotation_vars(ctx, elem, ann_from,
                                                             ann_to, &ann_count);
                    if (!elem && p->type_name[0] >= 'A' && p->type_name[0] <= 'Z')
                        elem = type_layout(p->type_name, NULL, 0, 0, false, 0);
                }
                if (!elem) elem = infer_fresh(ctx);
                pt = type_list(&elem, 1);
            } else if (p->type_name) {
                /* Strip surrounding parens from type names like "(Float)"
                 * produced by bracketed annotations [x : Float].
                 * type_from_name("(Float)") returns TYPE_LIST(Float) which
                 * poisons HM with spurious single-element tuple types.    */
                const char *tname = p->type_name;
                char tname_buf[256];
                (void)tname_buf;
                /* Do not strip ordinary parentheses here.
                 * Tuple types and tuple-argument function types depend on
                 * them, e.g. (a, b) and ((a, b) -> c). Only unwrap the old
                 * Pointer wrapper case below. */
                /* Strip pointer qualifiers — HM works on value types only;
                 * pointer-vs-value distinction is handled in codegen.
                 * Keep Fn :: (...) intact so type_from_name can recover the
                 * callback's full arrow type.                                */
                if (strncmp(tname, "Pointer :: ", 11) == 0) tname += 11;
                pt = type_from_name(tname);
                if (pt)
                    pt = infer_freshen_annotation_vars(ctx, pt, ann_from, ann_to,
                                                       &ann_count);
                /* type_from_name only knows builtin scalars. For user-defined
                 * layout types like Vec3 it returns NULL. If the name starts
                 * with an uppercase letter, treat it as a layout type so HM
                 * gets a concrete type instead of a fresh variable.          */
                if (!pt && tname[0] >= 'A' && tname[0] <= 'Z')
                    pt = type_layout(tname, NULL, 0, 0, false, 0);
                if (!pt) pt = infer_fresh(ctx);
            } else {
                /* Bare unannotated parameter — always a fresh type variable.
                 * TYPE_COLL was wrong here: it poisoned polymorphic functions
                 * like (define double x -> (* 2 x)) by making x a collection,
                 * which then failed to unify with Int at the call site.
                 * Only named params that explicitly carry a collection type_name
                 * like "(a)" or "[a]" should get TYPE_COLL — bare names get 'a. */
                if (p->type_name) {
                    pt = type_from_name(p->type_name);
                    if (pt) {
                        pt = infer_freshen_annotation_vars(ctx, pt, ann_from,
                                                           ann_to, &ann_count);
                        pt = infer_normalize_annotation_type(pt);
                    }
                    if (!pt) pt = infer_fresh(ctx);
                } else {
                    pt = infer_fresh(ctx);
                }
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
                    if (ann) {
                        ann = infer_freshen_annotation_vars(ctx, ann, ann_from,
                                                            ann_to, &ann_count);
                        ann = infer_normalize_annotation_type(ann);
                    }
                    if (ann) {
                        infer_constrain(ctx, bt, ann, ast->line, ast->column);
                        bt = ann;
                    }
                }
                ret_t = bt;
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

        /* check-laws is a compile-time test directive.  Its class and type
         * operands are names, not runtime values, and the code generator
         * expands it into exhaustive or generated property evaluations. */
        if (head->type == AST_SYMBOL &&
            (strcmp(head->symbol, "check-laws") == 0 ||
             strcmp(head->symbol, "check-laws-seeded") == 0)) {
            bool seeded = strcmp(head->symbol, "check-laws-seeded") == 0;
            if (ast->list.count != (seeded ? 5u : 3u) ||
                ast->list.items[1]->type != AST_SYMBOL ||
                ast->list.items[2]->type != AST_SYMBOL ||
                (seeded && (ast->list.items[3]->type != AST_NUMBER ||
                            ast->list.items[4]->type != AST_NUMBER))) {
                READER_ERROR(ast->line, ast->column,
                             "%s expects a class name, a type name%s",
                             head->symbol,
                             seeded ? ", a positive case count, and a seed"
                                    : "");
            }
            result = type_int();
            break;
        }

        bool is_tuple_expr = false;
        for (size_t i = 0; i < ast->list.count; i++) {
            AST *item = ast->list.items[i];
            if (item && item->type == AST_SYMBOL && item->symbol &&
                strcmp(item->symbol, ",") == 0) {
                is_tuple_expr = true;
                break;
            }
        }
        if (is_tuple_expr) {
            Type **elems = malloc(sizeof(Type *) * ast->list.count);
            int elem_count = 0;
            for (size_t i = 0; i < ast->list.count; i++) {
                AST *item = ast->list.items[i];
                if (item && item->type == AST_SYMBOL && item->symbol &&
                    strcmp(item->symbol, ",") == 0) {
                    continue;
                }
                elems[elem_count++] = infer_expr(ctx, item);
            }
            result = type_list(elems, elem_count);
            free(elems);
            break;
        }

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

            /* Solve all pending constraints BEFORE generalising.
             * This is critical for recursive definitions: if we generalise
             * before unification, free type vars that should resolve to
             * concrete types (e.g. the return type of a recursive function)
             * get universally quantified, producing unsound polymorphic
             * schemes and infinite-type occurs-check failures at call sites.
             * W-by-W (constraint generation then solving per binding)
             * is the correct architecture for let-generalisation in
             * constraint-based HM (see Heeren et al, "Generalizing
             * Hindley-Milner Type Inference Algorithms", 2002).          */

            /* Solve pending constraints before generalising so that type
             * variables in the body get resolved to concrete types before
             * we decide what to quantify.  We do NOT abort on failure here
             * because the dep-bridge may have already resolved ambiguous
             * literals (e.g. 2.0 stored as integer 2) via inferred_type,
             * and the real error will surface at the top-level unify pass
             * with a proper error message.                               */
            bool inner_ok = infer_unify_all(ctx);
            (void)inner_ok;
            /* Reset constraint buffer so the next define starts fresh.
             * Constraints are solved; substitution retains all bindings. */
            ctx->constraint_count = 0;
            ctx->had_error = false;

            /* Remove stale scheme for this name from outer env before generalising
             * to prevent old type vars from corrupting infer_free_vars_env        */
            infer_env_remove(saved_env, name);
            TypeScheme *sc = infer_generalise(ctx, val_t, saved_env);
            ctx->env = saved_env;
            infer_env_free(def_child);
            /* Insert the fully-solved generalised scheme into the real outer env */
            infer_env_insert(ctx->env, name, sc);
            break;
        }

        /* ---- lambda (desugared inline) ------------------------------- */
        if (head->type == AST_SYMBOL && (strcmp(head->symbol, "lambda") == 0 || strcmp(head->symbol, "λ") == 0) && ast->list.count >= 3) {
            InferEnv *child = infer_env_create_child(ctx->env);
            InferEnv *saved = ctx->env;
            ctx->env        = child;

            AST *params_ast = ast->list.items[1];
            int param_count = 0;
            Type *param_types[32]; // Max 32 params for inline lambdas
            if (params_ast->type == AST_LIST) {
                param_count = params_ast->list.count;
                for (size_t i = 0; i < params_ast->list.count && i < 32; i++) {
                    AST *p = params_ast->list.items[i];
                    Type *pt = infer_fresh(ctx);
                    param_types[i] = pt;
                    if (p->type == AST_SYMBOL) {
                        infer_env_insert(child, p->symbol, scheme_mono(pt));
                    } else if (p->type == AST_LIST && p->list.count > 0 && p->list.items[0]->type == AST_SYMBOL) {
                        infer_env_insert(child, p->list.items[0]->symbol, scheme_mono(pt));
                    }
                }
            } else if (params_ast->type == AST_SYMBOL) {
                param_count = 1;
                Type *pt = infer_fresh(ctx);
                param_types[0] = pt;
                infer_env_insert(child, params_ast->symbol, scheme_mono(pt));
            }

            Type *ret_t = infer_fresh(ctx);
            for (size_t i = 2; i < ast->list.count; i++) {
                Type *bt = infer_expr(ctx, ast->list.items[i]);
                if (i == ast->list.count - 1) ret_t = bt;
            }

            ctx->env = saved;
            infer_env_free(child);

            Type *arrow = ret_t;
            for (int i = param_count - 1; i >= 0; i--) {
                arrow = type_arrow(param_types[i], arrow);
            }
            result = arrow;
            break;
        }

        /* ---- if ------------------------------------------------------ */
        if (head->type == AST_SYMBOL && strcmp(head->symbol, "if") == 0) {
            if (ast->list.count >= 3) {
                bool infix_subtype_cond =
                    ast->list.count >= 5 &&
                    ast->list.items[2]->type == AST_SYMBOL &&
                    strcmp(ast->list.items[2]->symbol, "<:") == 0;
                Type *cond_t = infix_subtype_cond ? type_bool() : infer_expr(ctx, ast->list.items[1]);
                infer_constrain(ctx, cond_t, type_bool(),
                                ast->list.items[1]->line,
                                ast->list.items[1]->column);
                size_t then_idx = infix_subtype_cond ? 4 : 2;
                size_t else_idx = infix_subtype_cond ? 5 : 3;
                Type *then_t = ast->list.count > then_idx
                    ? infer_expr(ctx, ast->list.items[then_idx])
                    : infer_fresh(ctx);
                if (ast->list.count > else_idx) {
                    Type *else_t = infer_expr(ctx, ast->list.items[else_idx]);

                    /* Smart Optional Promotion for Branches */
                    bool then_opt = (then_t->kind == TYPE_OPTIONAL || then_t->kind == TYPE_NIL);
                    bool else_opt = (else_t->kind == TYPE_OPTIONAL || else_t->kind == TYPE_NIL);

                    if (then_opt && !else_opt) {
                        else_t = type_optional(else_t);
                    } else if (else_opt && !then_opt) {
                        then_t = type_optional(then_t);
                    }

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

        if (head->type == AST_SYMBOL && strcmp(head->symbol, "<:") == 0 && ast->list.count == 3) {
            result = type_bool();
            break;
        }

        /* ---- reader/runtime collection concat -------------------------- */
        if (head->type == AST_SYMBOL &&
            (strcmp(head->symbol, "rt_coll_concat") == 0 ||
             strcmp(head->symbol, "__rt_concat") == 0) &&
            ast->list.count == 3) {
            Type *left_t = infer_expr(ctx, ast->list.items[1]);
            Type *right_t = infer_expr(ctx, ast->list.items[2]);
            Type *left_applied = subst_apply(ctx->subst, left_t);

            bool left_is_collection =
                left_applied &&
                (left_applied->kind == TYPE_COLL ||
                 left_applied->kind == TYPE_ARR  ||
                 left_applied->kind == TYPE_SET  ||
                 left_applied->kind == TYPE_MAP  ||
                 left_applied->kind == TYPE_STRING);

            if (!left_is_collection &&
                ast->list.items[1]->type == AST_LIST &&
                ast->list.items[1]->list.count > 0 &&
                ast->list.items[1]->list.items[0]->type == AST_SYMBOL) {
                TypeScheme *left_sc = infer_env_lookup(
                    ctx, ast->list.items[1]->list.items[0]->symbol);
                if (left_sc) {
                    Type *left_sig = infer_instantiate(ctx, left_sc);
                    bool saw_arrow = false;
                    while (left_sig && left_sig->kind == TYPE_ARROW) {
                        saw_arrow = true;
                        left_sig = left_sig->arrow_ret;
                    }
                    left_sig = subst_apply(ctx->subst, left_sig);
                    if (left_sig && left_sig->kind == TYPE_LIST &&
                        left_sig->list_count == 1)
                        left_sig = left_sig->list_types[0];
                    left_is_collection =
                        saw_arrow &&
                        left_sig &&
                        (left_sig->kind == TYPE_COLL ||
                         left_sig->kind == TYPE_ARR  ||
                         left_sig->kind == TYPE_SET  ||
                         left_sig->kind == TYPE_MAP  ||
                         left_sig->kind == TYPE_STRING);
                }
            }

            bool right_is_cons_literal_tail =
                infer_is_cons_literal_tail(ast->list.items[2]);

            if (left_is_collection && !right_is_cons_literal_tail) {
                infer_constrain(ctx, left_t, right_t, ast->line, ast->column);
                result = right_t;
            } else {
                Type *tail_t = type_coll();
                tail_t->element_type = type_clone(left_t);
                infer_constrain(ctx, right_t, tail_t, ast->line, ast->column);
                Type *out_t = type_coll();
                out_t->element_type = type_clone(left_t);
                result = out_t;
            }
            break;
        }

        /* ---- runtime collection helpers -------------------------------- */
        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "rt_coll_is_empty") == 0 &&
            ast->list.count == 2) {
            Type *arg_t = infer_expr(ctx, ast->list.items[1]);
            infer_constrain(ctx, arg_t, type_coll(), ast->line, ast->column);
            result = type_bool();
            break;
        }

        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "__rt_count") == 0 &&
            ast->list.count == 2) {
            (void)infer_expr(ctx, ast->list.items[1]);
            result = type_int();
            break;
        }

        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "rt_coll_drop") == 0 &&
            ast->list.count == 3) {
            Type *coll_t = infer_expr(ctx, ast->list.items[1]);
            Type *idx_t = infer_expr(ctx, ast->list.items[2]);
            infer_constrain(ctx, idx_t, type_int(), ast->list.items[2]->line,
                            ast->list.items[2]->column);

            Type *coll_applied = subst_apply(ctx->subst, coll_t);
            Type *elem_t = NULL;
            if (coll_applied && coll_applied->kind == TYPE_COLL)
                elem_t = coll_applied->element_type;
            else if (coll_applied && coll_applied->kind == TYPE_ARR)
                elem_t = coll_applied->arr_element_type;
            if (!elem_t)
                elem_t = infer_fresh(ctx);

            Type *expected_coll = type_coll();
            expected_coll->element_type = type_clone(elem_t);
            infer_constrain(ctx, coll_t, expected_coll, ast->line, ast->column);

            result = type_coll();
            result->element_type = type_clone(elem_t);
            break;
        }

        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "rt_coll_head") == 0 &&
            ast->list.count == 2) {
            Type *arg_t = infer_expr(ctx, ast->list.items[1]);
            Type *arg_applied = subst_apply(ctx->subst, arg_t);
            Type *elem_t = NULL;

            if (arg_applied) {
                if (arg_applied->kind == TYPE_COLL)
                    elem_t = arg_applied->element_type;
                else if (arg_applied->kind == TYPE_ARR)
                    elem_t = arg_applied->arr_element_type;
                else if (arg_applied->kind == TYPE_LIST) {
                    if (arg_applied->list_elem)
                        elem_t = arg_applied->list_elem;
                    else if (arg_applied->list_count == 1 &&
                             arg_applied->list_types)
                        elem_t = arg_applied->list_types[0];
                } else if (arg_applied->kind == TYPE_STRING) {
                    elem_t = type_char();
                }
            }

            if (!elem_t) elem_t = infer_fresh(ctx);
            if (!arg_applied || (arg_applied->kind != TYPE_STRING &&
                                 arg_applied->kind != TYPE_LIST &&
                                 arg_applied->kind != TYPE_COLL &&
                                 arg_applied->kind != TYPE_ARR)) {
                Type *expected_coll = type_coll();
                expected_coll->element_type = type_clone(elem_t);
                infer_constrain(ctx, arg_t, expected_coll,
                                ast->line, ast->column);
            }
            result = elem_t;
            break;
        }

        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "rt_coll_empty") == 0 &&
            ast->list.count == 2) {
            Type *arg_t = infer_expr(ctx, ast->list.items[1]);
            infer_constrain(ctx, arg_t, type_coll(), ast->line, ast->column);

            result = type_coll();
            result->element_type = infer_fresh(ctx);
            break;
        }

        /* ---- begin --------------------------------------------------- */
        if (head->type == AST_SYMBOL && strcmp(head->symbol, "begin") == 0) {
            if (ast->list.count < 2) {
                result = type_int();
                break;
            }
            for (size_t i = 1; i < ast->list.count - 1; i++) {
                infer_expr(ctx, ast->list.items[i]);
            }
            result = infer_expr(ctx, ast->list.items[ast->list.count - 1]);
            break;
        }

        /* ---- let ----------------------------------------------------- */
        if (head->type == AST_SYMBOL && strcmp(head->symbol, "let") == 0) {
            result = infer_fresh(ctx);
            break;
        }

        /* ---- quote --------------------------------------------------- */
        if (head->type == AST_SYMBOL && strcmp(head->symbol, "quote") == 0) {
            Type *fresh_t = infer_fresh(ctx);
            result = type_list(&fresh_t, 1);
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

        /* ---- n-ary logic - (and a b c ...) / (or a b c ...) --------- */
        if (head->type == AST_SYMBOL &&
            (strcmp(head->symbol, "and") == 0 ||
             strcmp(head->symbol, "or")  == 0) &&
            ast->list.count >= 2) {
            for (size_t i = 1; i < ast->list.count; i++) {
                Type *at = infer_expr(ctx, ast->list.items[i]);
                infer_constrain(ctx, at, type_bool(),
                                ast->list.items[i]->line,
                                ast->list.items[i]->column);
            }
            result = type_bool();
            break;
        }

        /* ---- variadic call: look up if head is a variadic function --- */
        if (head->type == AST_SYMBOL) {
            TypeScheme *sc = infer_env_lookup(ctx, head->symbol);
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
                    Type *list_elem = infer_fresh(ctx);
                    if (ft && ft->kind == TYPE_ARROW &&
                        ft->arrow_param && ft->arrow_param->kind == TYPE_LIST) {
                        Type *rest_list = ft->arrow_param;
                        if (rest_list->list_elem)
                            list_elem = rest_list->list_elem;
                        else if (rest_list->list_count == 1 && rest_list->list_types)
                            list_elem = rest_list->list_types[0];
                    }
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

        /* ---- explicit internal indexing ------------------------------ */
        /* Wisp lowers pair[0] to (__index pair 0). Treat this as a
         * primitive HM operation, not as an ordinary function call. */
        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "__index") == 0 &&
            ast->list.count == 3) {
            Type *base_t = subst_apply(ctx->subst,
                                        infer_expr(ctx, ast->list.items[1]));
            int index_value = -1;

            if (infer_ast_numeric_index(ast->list.items[2], &index_value)) {
                infer_expr(ctx, ast->list.items[2]);

                if (base_t &&
                    base_t->kind == TYPE_LIST &&
                    index_value >= 0 &&
                    index_value < base_t->list_count &&
                    base_t->list_types &&
                    base_t->list_types[index_value]) {
                    result = subst_apply(ctx->subst,
                                         base_t->list_types[index_value]);
                    break;
                }

                if (base_t &&
                    base_t->kind == TYPE_LIST &&
                    base_t->list_count == 1 &&
                    base_t->list_types &&
                    base_t->list_types[0]) {
                    result = subst_apply(ctx->subst,
                                         base_t->list_types[0]);
                    break;
                }

                if (base_t && base_t->kind == TYPE_STRING) {
                    result = type_char();
                    break;
                }

                if (base_t && base_t->kind == TYPE_COLL && base_t->element_type) {
                    result = subst_apply(ctx->subst, base_t->element_type);
                    break;
                }

                if (base_t && base_t->kind == TYPE_ARR && base_t->arr_element_type) {
                    result = subst_apply(ctx->subst, base_t->arr_element_type);
                    break;
                }

                result = infer_fresh(ctx);
                break;
            }
        }

        /* ---- typed indexing / product projection ---------------------- */
        /* Indexing must be typed, not purely syntactic.
         *
         * The reader may represent both:
         *
         *   pair[0]
         *   (pair 0)
         *
         * as the same application shape. That is fine: if the callee has a
         * product, list, string, collection, array, set, or map type and the
         * argument is a numeric index, this is indexing/projection, not a
         * normal function call.
         *
         * This also lets Wisp safely canonicalize pair[0] to (pair 0)
         * without depending on fragile source-column metadata.
         */
        if (ast->list.count == 2 &&
            head->type == AST_SYMBOL) {
            TypeScheme *head_sc = infer_env_lookup(ctx, head->symbol);
            if (head_sc) {
                Type *head_t = subst_apply(ctx->subst, head_sc->type);
                bool syntax_index = infer_ast_was_postfix_index(ast, head);
                bool typed_index = false;

                if (head_t) {
                    typed_index =
                        head_t->kind == TYPE_LIST ||
                        head_t->kind == TYPE_STRING ||
                        head_t->kind == TYPE_COLL ||
                        head_t->kind == TYPE_ARR ||
                        head_t->kind == TYPE_SET ||
                        head_t->kind == TYPE_MAP;
                }

                int index_value = -1;
                if (head_t &&
                    (syntax_index || typed_index) &&
                    infer_ast_numeric_index(ast->list.items[1], &index_value)) {
                    infer_expr(ctx, ast->list.items[1]);

                    if (head_t->kind == TYPE_LIST &&
                        index_value >= 0 &&
                        index_value < head_t->list_count &&
                        head_t->list_types &&
                        head_t->list_types[index_value]) {
                        result = subst_apply(ctx->subst,
                                             head_t->list_types[index_value]);
                        break;
                    }

                    if (head_t->kind == TYPE_LIST &&
                        head_t->list_count == 1 &&
                        head_t->list_types &&
                        head_t->list_types[0]) {
                        result = subst_apply(ctx->subst,
                                             head_t->list_types[0]);
                        break;
                    }

                    if (head_t->kind == TYPE_LIST &&
                        head_t->list_elem) {
                        result = subst_apply(ctx->subst,
                                             head_t->list_elem);
                        break;
                    }

                    if (head_t->kind == TYPE_STRING) {
                        result = type_char();
                        break;
                    }

                    if (head_t->kind == TYPE_COLL && head_t->element_type) {
                        result = subst_apply(ctx->subst, head_t->element_type);
                        break;
                    }

                    if (head_t->kind == TYPE_ARR && head_t->arr_element_type) {
                        result = subst_apply(ctx->subst,
                                             head_t->arr_element_type);
                        break;
                    }

                    if (head_t->kind == TYPE_SET ||
                        head_t->kind == TYPE_MAP ||
                        head_t->kind == TYPE_COLL ||
                        head_t->kind == TYPE_ARR ||
                        head_t->kind == TYPE_LIST) {
                        result = infer_fresh(ctx);
                        break;
                    }
                }
            }
        }

        /* ---- collection indexing ------------------------------------- */
        /* A collection can look like a call in the AST:
         *
         *   (xs 0)
         *
         * but this shortcut must only fire for a real numeric index.
         * Previously every TYPE_LIST with one element was treated as an
         * indexable collection. That is wrong for parenthesized function
         * annotations: (a -> Bool) can arrive as a singleton TYPE_LIST
         * wrapper, and then (p x) incorrectly returns the arrow type itself
         * instead of applying p.
         */
        if (head->type == AST_SYMBOL && ast->list.count == 2) {
            TypeScheme *head_sc = infer_env_lookup(ctx, head->symbol);
            int index_value = -1;

            if (head_sc &&
                infer_ast_numeric_index(ast->list.items[1], &index_value)) {
                Type *head_t = subst_apply(ctx->subst, head_sc->type);

                if (head_t &&
                    head_t->kind == TYPE_LIST &&
                    head_t->list_count == 1 &&
                    head_t->list_types &&
                    head_t->list_types[0]) {
                    infer_expr(ctx, ast->list.items[1]);
                    result = subst_apply(ctx->subst, head_t->list_types[0]);
                    break;
                }

                if (head_t && (head_t->kind == TYPE_ARR     ||
                               head_t->kind == TYPE_SET     ||
                               head_t->kind == TYPE_MAP     ||
                               head_t->kind == TYPE_STRING  ||
                               head_t->kind == TYPE_COLL)) {
                    infer_expr(ctx, ast->list.items[1]);

                    if (head_t->kind == TYPE_STRING)
                        result = type_char();
                    else if (head_t->kind == TYPE_COLL && head_t->element_type)
                        result = subst_apply(ctx->subst, head_t->element_type);
                    else if (head_t->kind == TYPE_ARR && head_t->arr_element_type)
                        result = subst_apply(ctx->subst, head_t->arr_element_type);
                    else
                        result = infer_fresh(ctx);
                    break;
                }
            }
        }

        Type *fn_t  = infer_normalize_annotation_type(infer_expr(ctx, head));
        Type *ret_t = infer_fresh(ctx);

        /* Compile-time refinement check for literal arguments.
         * Walk the parameter type annotations of the called function
         * and check literal args against refinement predicates.       */
        if (head->type == AST_SYMBOL) {
            TypeScheme *head_sc = infer_env_lookup(ctx, head->symbol);
            if (head_sc) {
                Type *ft = subst_apply(ctx->subst, head_sc->type);
                if (head_sc->quantified_count > 0)
                    ft = infer_instantiate(ctx, head_sc);
                /* Walk arrow chain and check each arg */
                for (int i = 1; i < (int)ast->list.count && ft && ft->kind == TYPE_ARROW; i++) {
                    AST *arg = ast->list.items[i];
                    /* Get the parameter type name from the arrow */
                    Type *param_t = subst_apply(ctx->subst, ft->arrow_param);
                    if (param_t) {
                        const char *tname = type_to_string(param_t);
                        const char *pred  = refinement_pred_name(tname);
                        if (pred && arg->type == AST_NUMBER) {
                            /* Look up the predicate function's lambda in env
                             * and evaluate it on the literal at compile time.
                             * We do this by checking the predicate name and
                             * emitting a compile-time error.               */
                            /* For now: emit a warning — full static eval
                             * requires interpreter. Mark as needing check. */
                            fprintf(stderr, "%s:%d:%d: note: argument %d to '%s' "
                                    "must satisfy %s (checked at runtime)\n",
                                    ctx->filename, arg->line, arg->column, i,
                                    head->symbol, tname);
                        }
                    }
                    ft = ft->arrow_ret;
                }
            }
        }

        Type *expected = ret_t;
        for (int i = (int)ast->list.count - 1; i >= 1; i--) {
            Type *arg_t = infer_normalize_annotation_type(
                infer_expr(ctx, ast->list.items[i]));
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

    if (ast->inferred_type && ast->inferred_type->kind != TYPE_UNKNOWN) {
        /* Bridge dep->HM only for ground non-function types that carry
         * genuine value-level information.  We must NOT bridge:
         *   - Arrow / Fn types  (HM infers these better from call sites)
         *   - TYPE_UNKNOWN      (no information)
         *   - TYPE_LIST with >1 elements: these are tuple/product types from
         *     the dep checker representing (Json String) etc. — they must
         *     never be unified against HM scalar or arrow types, which would
         *     create a TYPE_ARROW -> TYPE_LIST cycle causing infinite recursion
         *     in subst_apply when the list element contains an arrow type var.
         */
        TypeKind k = ast->inferred_type->kind;

        /* Reject multi-element LIST hints: (Json String) is a product type
         * in the dep world but HM has no product types — unifying would bind
         * a type var 'a to TYPE_LIST(Json,String) which subst_apply would then
         * chase through every arrow in >=, creating an infinite TYPE_ARROW cycle. */
        bool is_multi_tuple = (k == TYPE_LIST &&
                               ast->inferred_type->list_count > 1);

        bool has_unknown_or_var =
            infer_type_contains_unknown_or_var(ast->inferred_type);

        bool is_ground = (k != TYPE_ARROW && k != TYPE_FN && k != TYPE_UNKNOWN
                          && k != TYPE_VAR && !is_multi_tuple &&
                          !has_unknown_or_var);

        /* Extra safety: reject universe-level leakage and symbol-scope mismatches. */
        bool dep_hint_trustworthy = is_ground;
        if (is_ground && ast->type == AST_SYMBOL) {
            TypeScheme *sc = infer_env_lookup(ctx, ast->symbol);
            if (!sc) {
                dep_hint_trustworthy = false;
            } else if (result && result->kind == TYPE_VAR && sc->quantified_count == 0) {
                /* Local params/pattern bindings are represented as bare HM
                 * variables. Dep hints on those symbols can be artifacts of
                 * pattern lowering and can over-specialize polymorphic
                 * annotations, e.g. forcing safe-head :: [a] -> Maybe a into
                 * [Bool] -> Maybe Bool. Let HM context and explicit signatures
                 * solve local variables instead. */
                dep_hint_trustworthy = false;
            }
        }

        if (dep_hint_trustworthy && ast->type == AST_LIST &&
            ast->list.count >= 2 &&
            ast->list.items[0] &&
            ast->list.items[0]->type == AST_SYMBOL) {
            if (infer_runtime_collection_helper(ast->list.items[0]->symbol)) {
                dep_hint_trustworthy = false;
            }
            TypeScheme *head_sc = infer_env_lookup(ctx, ast->list.items[0]->symbol);
            if (!head_sc) {
                dep_hint_trustworthy = false;
            } else {
                Type *head_t = subst_apply(ctx->subst, head_sc->type);
                if (head_t && (head_t->kind == TYPE_COLL ||
                               head_t->kind == TYPE_ARROW ||
                               head_t->kind == TYPE_FN ||
                               head_t->kind == TYPE_ARR ||
                               head_t->kind == TYPE_SET ||
                               head_t->kind == TYPE_MAP ||
                               head_t->kind == TYPE_STRING ||
                               head_t->kind == TYPE_UNKNOWN ||
                               head_t->kind == TYPE_VAR)) {
                    dep_hint_trustworthy = false;
                }
            }
        }

        /* Additional guard: if HM already inferred a concrete arrow/structured
         * type for this node, never overwrite it with a dep scalar hint.
         * This prevents Bool seeded on a `>=` function symbol from being
         * unified against the arrow type `'a -> 'a -> Bool`, which would
         * bind 'a to Bool and then Bool -> Bool -> Bool against (Json String). */
        if (dep_hint_trustworthy && result->kind != TYPE_VAR) {
            bool result_is_arrow = (result->kind == TYPE_ARROW ||
                                    result->kind == TYPE_FN);
            bool hint_is_scalar  = (type_is_bool(ast->inferred_type) || k == TYPE_INT ||
                                    k == TYPE_FLOAT || k == TYPE_CHAR ||
                                    k == TYPE_STRING);
            if (result_is_arrow && hint_is_scalar) {
                /* dep annotated the *call result* Bool on the function node
                 * itself; HM correctly sees the full arrow type — trust HM. */
                dep_hint_trustworthy = false;
            }
            if (result_is_arrow && k != TYPE_ARROW && k != TYPE_FN) {
                /* The dep checker may attach a constructor result type such as
                 * Maybe a to the constructor symbol `Just`; HM has the usable
                 * function type a -> Maybe a and must keep it. */
                dep_hint_trustworthy = false;
            }
        }

        if (dep_hint_trustworthy) {
            if (g_infer_trace_enabled)
                fprintf(stderr, "├─ \033[34mInterop\033[0m HM unifies with prior Dependent Type check: %s\n", type_to_string(ast->inferred_type));
            infer_constrain(ctx, result, ast->inferred_type, ast->line, ast->column);
        } else {
            ast->inferred_type = result;
        }
    } else {
        ast->inferred_type = result;
    }

    return result;
}

/// Primitives Bootstrap

static void infer_register_legacy_collection_builtins(InferCtx *ctx) {
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

    /* forall a b. Coll(a) -> b -> Bool
     * contains? works on String, Set, Map, and any generic collection.
     * Using type_coll() mirrors how count is registered and allows the
     * same coercion rules in infer_unify_one_internal to fire correctly
     * (Coll ~ String, Coll ~ Set, Coll ~ Map all succeed there).       */
    Type *a3     = infer_fresh(ctx);
    Type *a3_col = type_coll();
    a3_col->element_type = a3;
    Type *a3_key = infer_fresh(ctx);
    TypeScheme *contains_sc = infer_generalise(ctx,
        type_arrow(a3_col, type_arrow(a3_key, type_bool())), ctx->env);
    infer_env_insert(ctx->env, "contains?", contains_sc);

    Type *rt_drop_a = infer_fresh(ctx);
    Type *rt_drop_in = type_coll();
    Type *rt_drop_out = type_coll();
    rt_drop_in->element_type = type_clone(rt_drop_a);
    rt_drop_out->element_type = type_clone(rt_drop_a);
    TypeScheme *rt_drop_sc = infer_generalise(ctx,
        type_arrow(rt_drop_in, type_arrow(type_int(), rt_drop_out)), ctx->env);
    infer_env_insert(ctx->env, "rt_coll_drop", rt_drop_sc);

    Type *rt_head_a = infer_fresh(ctx);
    Type *rt_head_in = type_coll();
    rt_head_in->element_type = type_clone(rt_head_a);
    TypeScheme *rt_head_sc = infer_generalise(
        ctx, type_arrow(rt_head_in, rt_head_a), ctx->env);
    infer_env_insert(ctx->env, "rt_coll_head", rt_head_sc);

    Type *rt_empty_ref = infer_fresh(ctx);
    Type *rt_empty_a = infer_fresh(ctx);
    Type *rt_empty_out = type_coll();
    rt_empty_out->element_type = type_clone(rt_empty_a);
    TypeScheme *rt_empty_sc = infer_generalise(ctx,
        type_arrow(rt_empty_ref, rt_empty_out), ctx->env);
    infer_env_insert(ctx->env, "rt_coll_empty", rt_empty_sc);

    TypeScheme *rt_is_empty_sc = scheme_mono(type_arrow(type_coll(), type_bool()));
    infer_env_insert(ctx->env, "rt_coll_is_empty", rt_is_empty_sc);
    infer_env_insert(ctx->env, "__rt_count",
                     scheme_mono(type_arrow(type_coll(), type_int())));
    infer_env_insert(ctx->env, "__rt_set_singleton",
                     scheme_mono(type_arrow(type_set(), type_bool())));

    /* ∀a. a → Bool */
    Type *a4 = infer_fresh(ctx);
    TypeScheme *set_pred_sc = infer_generalise(ctx,
        type_arrow(a4, type_bool()), ctx->env);
    infer_env_insert(ctx->env, "set?",        set_pred_sc);
    infer_env_insert(ctx->env, "map?",        set_pred_sc);
    infer_env_insert(ctx->env, "collection?", set_pred_sc);

    /* ∀a. a -> [a] -> [a]   (cons / .) */
    Type *cons_a = infer_fresh(ctx);
    Type *cons_list_in  = type_list(&cons_a, 1);
    Type *cons_list_out = type_list(&cons_a, 1);
    TypeScheme *cons_sc = infer_generalise(ctx,
        type_arrow(cons_a, type_arrow(cons_list_in, cons_list_out)), ctx->env);
    infer_env_insert(ctx->env, ".", cons_sc);

}

void infer_register_builtins(InferCtx *ctx) {
    infer_register_legacy_collection_builtins(ctx);

    /* Core ADT constructors used across the prelude.  The dependent checker
     * owns full ADT validation; these HM schemes keep imported constructors
     * from collapsing to their payload types during core bootstrap. */
    Type *maybe_a = infer_fresh(ctx);
    Type *maybe_app = type_app("Maybe", type_clone(maybe_a));
    TypeScheme *nothing_sc = infer_generalise(ctx, type_clone(maybe_app), ctx->env);
    TypeScheme *just_sc = infer_generalise(ctx,
        type_arrow(maybe_a, maybe_app), ctx->env);
    infer_env_insert(ctx->env, "Nothing", nothing_sc);
    infer_env_insert(ctx->env, "Just", just_sc);

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
    Type *map_kv = type_map_of(mk, mv);
    TypeScheme *assoc_sc = infer_generalise(ctx,
        type_arrow(type_clone(map_kv),
            type_arrow(type_clone(mk),
                type_arrow(type_clone(mv), type_clone(map_kv)))),
        ctx->env);
    infer_env_insert(ctx->env, "__rt_map_assoc",  assoc_sc);
    infer_env_insert(ctx->env, "__rt_map_assoc!", assoc_sc);

    Type *dk = infer_fresh(ctx);
    Type *dv = infer_fresh(ctx);
    Type *delete_map = type_map_of(dk, dv);
    TypeScheme *dissoc_sc = infer_generalise(ctx,
        type_arrow(type_clone(delete_map),
                   type_arrow(type_clone(dk), type_clone(delete_map))), ctx->env);
    infer_env_insert(ctx->env, "__rt_map_dissoc",  dissoc_sc);
    infer_env_insert(ctx->env, "__rt_map_dissoc!", dissoc_sc);

    Type *merge_k = infer_fresh(ctx);
    Type *merge_v = infer_fresh(ctx);
    Type *merge_map = type_map_of(merge_k, merge_v);
    infer_env_insert(ctx->env, "__rt_map_merge", infer_generalise(ctx,
        type_arrow(type_clone(merge_map),
                   type_arrow(type_clone(merge_map), type_clone(merge_map))),
        ctx->env));

    Type *keys_fresh = infer_fresh(ctx);
    Type *keys_value = infer_fresh(ctx);
    Type *keys_map = type_map_of(keys_fresh, keys_value);
    Type *keys_result = type_coll();
    keys_result->element_type = type_clone(keys_fresh);
    infer_env_insert(ctx->env, "__rt_map_keys", infer_generalise(ctx,
        type_arrow(keys_map, keys_result), ctx->env));
    Type *vals_fresh = infer_fresh(ctx);
    Type *vals_key = infer_fresh(ctx);
    Type *vals_map = type_map_of(vals_key, vals_fresh);
    Type *vals_result = type_coll();
    vals_result->element_type = type_clone(vals_fresh);
    infer_env_insert(ctx->env, "__rt_map_values", infer_generalise(ctx,
        type_arrow(vals_map, vals_result), ctx->env));

    Type *find_key = infer_fresh(ctx);
    Type *find_value = infer_fresh(ctx);
    infer_env_insert(ctx->env, "__rt_map_find", infer_generalise(ctx,
        type_arrow(type_map_of(find_key, find_value),
                   type_arrow(find_key, find_value)), ctx->env));

    /* ADT internal primitives — typed by codegen_data at runtime,
     * registered here as opaque so HM doesn't reject them          */
    Type *adt_a = infer_fresh(ctx);
    TypeScheme *adt_tag_sc = infer_generalise(ctx,
        type_arrow(adt_a, type_int()), ctx->env);
    infer_env_insert(ctx->env, "__adt_tag", adt_tag_sc);
}


/// Top-level Entry Point

static void infer_validate_calls(InferCtx *ctx, AST *ast) {
    if (!ast) return;

    if (ast->type == AST_LIST && ast->list.count >= 2) {
        AST *head = ast->list.items[0];
        if (head->type == AST_SYMBOL) {
            TypeScheme *sc = infer_env_lookup(ctx, head->symbol);
            if (sc) {
                Type *ft = subst_apply(ctx->subst, sc->type);
                for (int i = 1; i < (int)ast->list.count && ft && ft->kind == TYPE_ARROW; i++) {
                    Type *param_t = subst_apply(ctx->subst, ft->arrow_param);
                    AST  *arg     = ast->list.items[i];
                    Type *arg_t = arg->inferred_type
                                ? subst_apply(ctx->subst, arg->inferred_type)
                                : NULL;
                    /* If still unresolved, try env lookup for symbols */
                    if ((!arg_t || arg_t->kind == TYPE_UNKNOWN) &&
                        arg->type == AST_SYMBOL) {
                        TypeScheme *asc = infer_env_lookup(ctx, arg->symbol);
                        if (asc) arg_t = subst_apply(ctx->subst, asc->type);
                    }

                    if (param_t && arg_t &&
                        param_t->kind != TYPE_VAR &&
                        param_t->kind != TYPE_UNKNOWN &&
                        arg_t->kind   != TYPE_VAR &&
                        arg_t->kind   != TYPE_UNKNOWN) {
                        /* Unwrap single-element TYPE_LIST — (Float) is just Float */
                        if (param_t->kind == TYPE_LIST && param_t->list_count == 1)
                            param_t = param_t->list_types[0];
                        if (arg_t->kind == TYPE_LIST && arg_t->list_count == 1)
                            arg_t = arg_t->list_types[0];

                        /* Implicit Coercion: Int -> Float */
                        /* We proactively mutate the AST node's type so that subsequent
                         * strict passes and codegen see the correctly coerced type */
                        if ((param_t->kind == TYPE_FLOAT || param_t->kind == TYPE_F80) &&
                            (arg_t->kind == TYPE_INT || arg_t->kind == TYPE_INT_ARBITRARY || arg_t->kind == TYPE_CHAR)) {
                            arg->inferred_type = type_clone(param_t);
                        }

                        if (param_t->kind == TYPE_FINITE_SET &&
                            (arg->type == AST_NUMBER || arg->type == AST_CHAR ||
                             arg->type == AST_STRING || arg->type == AST_KEYWORD)) {
                            size_t ordinal = 0;
                            if (!finite_type_set_contains_literal(param_t->finite_name,
                                                                  arg, &ordinal)) {
                                READER_ERROR(arg->line, arg->column,
                                             "literal is not an inhabitant of finite type '%s'",
                                             param_t->finite_name);
                            }
                            arg->inferred_type = type_clone(param_t);
                        }

                        if (param_t->kind == TYPE_FINITE_SET &&
                            arg->type == AST_SYMBOL &&
                            finite_type_set_member_type_count(arg->symbol) > 0) {
                            size_t ordinal = 0;
                            if (!finite_type_set_contains_symbol(param_t->finite_name,
                                                                 arg->symbol,
                                                                 &ordinal)) {
                                READER_ERROR(arg->line, arg->column,
                                             "singleton '%s' is not an inhabitant of finite type '%s'",
                                             arg->symbol, param_t->finite_name);
                            }
                            arg->inferred_type = type_clone(param_t);
                        }

                        bool arg_is_fn   = (arg_t->kind   == TYPE_ARROW || arg_t->kind   == TYPE_FN);
                        bool param_is_fn = (param_t->kind == TYPE_ARROW || param_t->kind == TYPE_FN);
                        if (arg_is_fn && !param_is_fn) {
                            READER_ERROR(arg->line, arg->column,
                                         "argument %d to '%s' must be %s, but got a function",
                                         i, head->symbol, type_to_string(param_t));
                        }
                    }
                    ft = ft->arrow_ret;
                }
            }
        }
    }

    /* Recurse into all children */
    switch (ast->type) {
    case AST_LIST:
        if (ast->list.count > 0 &&
            ast->list.items[0] &&
            ast->list.items[0]->type == AST_SYMBOL &&
            (strcmp(ast->list.items[0]->symbol, "check-laws") == 0 ||
             strcmp(ast->list.items[0]->symbol, "check-laws-seeded") == 0))
            break;
        for (size_t i = 0; i < ast->list.count; i++)
            infer_validate_calls(ctx, ast->list.items[i]);
        break;
    case AST_LAMBDA: {
        InferEnv *child = infer_env_create_child(ctx->env);
        InferEnv *saved = ctx->env;
        ctx->env = child;
        for (int i = 0; i < ast->lambda.param_count; i++) {
            infer_env_insert(child, ast->lambda.params[i].name, scheme_mono(type_unknown()));
        }
        for (int i = 0; i < ast->lambda.body_count; i++)
            infer_validate_calls(ctx, ast->lambda.body_exprs[i]);
        ctx->env = saved;
        infer_env_free(child);
        break;
    }
    case AST_ARRAY:
        for (size_t i = 0; i < ast->array.element_count; i++)
            infer_validate_calls(ctx, ast->array.elements[i]);
        break;
    default:
        break;
    }
}

void infer_report_holes(InferCtx *ctx) {
    if (!ctx->has_holes || ctx->hole_count == 0) return;
    int n = ctx->hole_count < INFER_MAX_HOLES ? ctx->hole_count : INFER_MAX_HOLES;
    fprintf(stderr, "\n");
    for (int i = 0; i < n; i++) {
        InferHole *h  = &ctx->hole_positions[i];
        Type      *t  = subst_apply(ctx->subst, type_var(h->var_id));
        const char *ts = type_to_string(t);
        fprintf(stderr, "%s:%d:%d: hole has type: %s\n",
                ctx->filename ? ctx->filename : "<unknown>",
                h->line, h->col, ts);
    }
    fprintf(stderr, "\n");
}

Type *infer_toplevel(InferCtx *ctx, AST *ast) {
    /* 1. Constraint generation */
    Type *t = infer_expr(ctx, ast);
    if (ctx->had_error) return subst_apply(ctx->subst, t);

    /* 2. Constraint solving */
    if (!infer_unify_all(ctx)) return subst_apply(ctx->subst, t);

    /* Disable TT bridge for post-unification phases to prevent UAF */
    struct DepCtx *saved_dctx = ctx->dctx;
    ctx->dctx = NULL;

    /* 3. Zonking — apply substitution to all AST nodes */
    infer_zonk_ast(ctx, ast);

    /* 4. Post-zonk validation — check call sites with concrete types */
    infer_validate_calls(ctx, ast);

    ctx->dctx = saved_dctx;

    return subst_apply(ctx->subst, t);
}


/// Pretty Printing

void infer_print_type(Type *t, Substitution *s) {
    if (!t) { fprintf(stderr, "?"); return; }
    if (s) t = subst_apply(s, t);
    if (!t) { fprintf(stderr, "?"); return; }

    switch (t->kind) {
    case TYPE_VAR:
        fprintf(stderr, "'%c", 'a' + (t->var_id % 26));
        if (t->var_id >= 26) fprintf(stderr, "%d", t->var_id / 26);
        break;
    case TYPE_ARROW:
        fprintf(stderr, "(");
        infer_print_type(t->arrow_param, s);
        fprintf(stderr, " → ");
        infer_print_type(t->arrow_ret, s);
        fprintf(stderr, ")");
        break;
    case TYPE_LIST:
        fprintf(stderr, "(");
        for (int i = 0; i < t->list_count; i++) {
            if (i > 0) fprintf(stderr, " ");
            infer_print_type(t->list_types[i], s);
        }
        fprintf(stderr, ")");
        break;
    case TYPE_OPTIONAL:
        infer_print_type(t->element_type, s);
        fprintf(stderr, "?");
        break;
    default:
        fprintf(stderr, "%s", type_to_string(t));
        break;
    }
}

void infer_print_scheme(TypeScheme *sc) {
    if (!sc) { fprintf(stderr, "?"); return; }
    if (sc->quantified_count > 0) {
        fprintf(stderr, "∀");
        for (int i = 0; i < sc->quantified_count; i++) {
            if (i > 0) fprintf(stderr, " ");
            fprintf(stderr, "'%c", 'a' + (sc->quantified[i] % 26));
        }
        fprintf(stderr, ". ");
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
