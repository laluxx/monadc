#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include "compat.h"
#include "infer.h"
#include "qtt/constraints.h"
#include "qtt/environment.h"
#include "qtt/type_identity.h"
#include "effects/effect.h"
#include "effects/constraints.h"
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
    case TYPE_SET:
        return type_set_of(t->element_type
            ? infer_freshen_annotation_vars(ctx, t->element_type,
                                             from, to, count)
            : NULL);
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

void infer_env_free_owned_schemes(InferEnv *env) {
    if (!env) return;
    TypeScheme **owned = NULL;
    size_t count = 0;
    size_t capacity = 0;
    for (size_t i = 0; i < env->size; i++) {
        for (InferEnvEntry *entry = env->buckets[i]; entry;
             entry = entry->next) {
            bool seen = false;
            for (size_t j = 0; j < count; j++)
                if (owned[j] == entry->scheme) { seen = true; break; }
            if (seen || !entry->scheme) continue;
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 32;
                TypeScheme **grown = realloc(owned, next * sizeof(*grown));
                if (!grown) {
                    /* Preserve safety under allocation failure: leaking an
                     * unrecorded scheme is preferable to double-freeing it. */
                    continue;
                }
                owned = grown;
                capacity = next;
            }
            owned[count++] = entry->scheme;
        }
    }
    for (size_t i = 0; i < count; i++) scheme_free(owned[i]);
    free(owned);
    infer_env_free(env);
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
    /* Type nodes may belong to an imported principal scheme shared by many
     * inference contexts. Union-find owns path compression; rewriting the
     * node itself corrupts immutable scheme metadata when local variable IDs
     * happen to overlap its canonical quantifiers. */
    return root == t->var_id ? t : type_var(root);
}

static bool infer_optional_like(InferCtx *ctx, Type *t) {
    Type *resolved = ctx && ctx->subst ? subst_apply_shallow(ctx->subst, t) : t;
    return resolved &&
        (resolved->kind == TYPE_OPTIONAL || resolved->kind == TYPE_NIL);
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
    case TYPE_COLL:
    case TYPE_SET: {
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
        Type *arrow = type_arrow(p, r);
        arrow->arrow_effect_complete = t->arrow_effect_complete;
        arrow->arrow_effect_name = t->arrow_effect_name
            ? strdup(t->arrow_effect_name) : NULL;
        if (t->arrow_effect_scheme) {
            arrow->arrow_effect_scheme = strdup(t->arrow_effect_scheme);
            arrow->arrow_effect_scheme_owned =
                arrow->arrow_effect_scheme != NULL;
        }
        return arrow;
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
    if (!ctx) return NULL;
    ctx->subst          = subst_create();
    ctx->constraint_cap = 256;
    ctx->constraints    = malloc(sizeof(TypeConstraint) * ctx->constraint_cap);
    ctx->constraint_count = 0;
    ctx->env            = env;
    ctx->dctx           = dctx;
    ctx->filename       = filename ? filename : "<unknown>";
    ctx->had_error      = false;
    ctx->next_closure_instance_id = 1;
    ctx->grade_arena    = qtt_grade_arena_new();
    ctx->effect_arena   = qtt_effect_arena_new();
    ctx->effect_solver  = ctx->effect_arena
        ? qtt_effect_solver_new(ctx->effect_arena) : NULL;
    if (!ctx->subst || !ctx->constraints || !ctx->grade_arena ||
        !ctx->effect_solver) {
        infer_ctx_free(ctx);
        return NULL;
    }
    return ctx;
}

void infer_ctx_free(InferCtx *ctx) {
    if (!ctx) return;
    subst_free(ctx->subst);
    free(ctx->constraints);
    for (size_t i = 0; i < ctx->grade_application_count; i++) {
        free(ctx->grade_applications[i].domain_grades);
        free(ctx->grade_applications[i].closure_module_ids);
        free(ctx->grade_applications[i].closure_ids);
        free(ctx->grade_applications[i].closure_binder_ids);
        free(ctx->grade_applications[i].closure_slots);
        free(ctx->grade_applications[i].closure_parameter_indices);
        free(ctx->grade_applications[i].closure_origin_kinds);
        free(ctx->grade_applications[i].closure_origin_ids);
        free(ctx->grade_applications[i].closure_domain_module_ids);
        free(ctx->grade_applications[i].closure_domain_ids);
        free(ctx->grade_applications[i].closure_domain_indices);
        free(ctx->grade_applications[i].result_closure_module_ids);
        free(ctx->grade_applications[i].result_closure_ids);
        free(ctx->grade_applications[i].result_closure_instance_ids);
        if (ctx->grade_applications[i].result_closure_environments) {
            for (size_t j = 0;
                 j < ctx->grade_applications[i].result_closure_count; j++)
                qtt_environment_free(
                    ctx->grade_applications[i]
                        .result_closure_environments[j]);
            free(ctx->grade_applications[i].result_closure_environments);
        }
        free(ctx->grade_applications[i].callable_parameter_indices);
        free(ctx->grade_applications[i]
                 .callable_domain_parameter_indices);
        free(ctx->grade_applications[i].callable_domain_indices);
    }
    free(ctx->grade_applications);
    qtt_grade_arena_free(ctx->grade_arena);
    qtt_effect_solver_free(ctx->effect_solver);
    qtt_effect_arena_free(ctx->effect_arena);
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
            Type *inner = b->kind == TYPE_OPTIONAL
                ? subst_apply_shallow(s, b->element_type) : NULL;
            if (inner && inner->kind == TYPE_VAR &&
                subst_find(s, inner->var_id) == root)
                return true;
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
            Type *inner = a->kind == TYPE_OPTIONAL
                ? subst_apply_shallow(s, a->element_type) : NULL;
            if (inner && inner->kind == TYPE_VAR &&
                subst_find(s, inner->var_id) == root)
                return true;
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

    /* The runtime set representation and the surface constructor application
     * `{a}` denote the same type. Preserve and relate the element index rather
     * than erasing a bound set literal to the unparameterized ground Set. */
    if (a->kind == TYPE_SET && b->kind == TYPE_APP &&
        b->app_constructor && strcmp(b->app_constructor, "Set") == 0) {
        return !a->element_type || !b->app_arg ||
            infer_unify_one(ctx, a->element_type, b->app_arg, line, col);
    }
    if (b->kind == TYPE_SET && a->kind == TYPE_APP &&
        a->app_constructor && strcmp(a->app_constructor, "Set") == 0) {
        return !b->element_type || !a->app_arg ||
            infer_unify_one(ctx, b->element_type, a->app_arg, line, col);
    }

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

        /* Core String has the byte-address representation accepted by the
         * low-level FFI boundary. Preserve that established ABI when a fresh
         * portable interface makes Pointer U8 explicit instead of relying on
         * an unknown legacy import type. */
        if (a->kind == TYPE_STRING && b->kind == TYPE_PTR &&
            b->element_type &&
            (b->element_type->kind == TYPE_U8 ||
             b->element_type->kind == TYPE_BYTE ||
             b->element_type->kind == TYPE_CHAR))
            return true;
        if (b->kind == TYPE_STRING && a->kind == TYPE_PTR &&
            a->element_type &&
            (a->element_type->kind == TYPE_U8 ||
             a->element_type->kind == TYPE_BYTE ||
             a->element_type->kind == TYPE_CHAR))
            return true;

        /* Path is the portable filesystem name type and has the same raw,
         * NUL-terminated byte-address representation at a POSIX boundary.
         * Keep this compatibility restricted to byte/character pointers. */
        if (a->kind == TYPE_PATH && b->kind == TYPE_PTR &&
            b->element_type &&
            (b->element_type->kind == TYPE_U8 ||
             b->element_type->kind == TYPE_BYTE ||
             b->element_type->kind == TYPE_CHAR))
            return true;
        if (b->kind == TYPE_PATH && a->kind == TYPE_PTR &&
            a->element_type &&
            (a->element_type->kind == TYPE_U8 ||
             a->element_type->kind == TYPE_BYTE ||
             a->element_type->kind == TYPE_CHAR))
            return true;

        /* Collection acting as a function: (Coll -> 'a) ~ (Int -> 'a)
         * SAFETY GUARD: only allow this coercion when the arrow param is
         * already a concrete integer/char type or an unbound fresh var.
         * Never allow it when the arrow param is itself an arrow type —
         * that means we are inside a polymorphic instantiation and the
         * coercion would bind a shared type var to both String and an
         * arrow type, creating a cycle in subst_apply.                  */
        if ((a->kind == TYPE_COLL || a->kind == TYPE_LIST ||
             a->kind == TYPE_ARR || a->kind == TYPE_PTR ||
             a->kind == TYPE_STRING) &&
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
            /* Indexing a collection is a single, pure Int -> element
             * coercion.  A residual multi-stage or effect-annotated arrow is
             * a partially applied callable, never an indexing operation.
             * Rejecting it here preserves the original type mismatch and
             * prevents diagnostics such as `Int ~ [Int] -e-> [Int]`. */
            if (b->arrow_effect_name ||
                (b->arrow_ret && b->arrow_ret->kind == TYPE_ARROW))
                goto skip_coll_as_fn_ab;
            if (bp && (bp->kind == TYPE_ARROW || bp->kind == TYPE_FN ||
                       bp->kind == TYPE_COLL  || bp->kind == TYPE_LIST ||
                       bp->kind == TYPE_ARR   || bp->kind == TYPE_PTR ||
                       bp->kind == TYPE_STRING ||
                       type_is_bool(bp)        || bp->kind == TYPE_FLOAT)) {
                goto skip_coll_as_fn_ab;
            }
            {
                bool ok = infer_unify_one(ctx, type_int(), b->arrow_param, line, col);
                if (!ok) return false;
                if (a->kind == TYPE_COLL) return infer_unify_one(ctx, a->element_type, b->arrow_ret, line, col);
                if (a->kind == TYPE_ARR) return infer_unify_one(ctx, a->arr_element_type, b->arrow_ret, line, col);
                if (a->kind == TYPE_PTR) return infer_unify_one(ctx, a->element_type, b->arrow_ret, line, col);
                if (a->kind == TYPE_STRING) return infer_unify_one(ctx, type_char(), b->arrow_ret, line, col);
                return true;
            }
            skip_coll_as_fn_ab:;
        }
        if ((b->kind == TYPE_COLL || b->kind == TYPE_LIST ||
             b->kind == TYPE_ARR || b->kind == TYPE_PTR ||
             b->kind == TYPE_STRING) &&
            a->kind == TYPE_ARROW) {
            Type *ap = subst_apply_shallow(ctx->subst, a->arrow_param);
            if (a->arrow_effect_name ||
                (a->arrow_ret && a->arrow_ret->kind == TYPE_ARROW))
                goto skip_coll_as_fn_ba;
            if (ap && (ap->kind == TYPE_ARROW || ap->kind == TYPE_FN ||
                       ap->kind == TYPE_COLL  || ap->kind == TYPE_LIST ||
                       ap->kind == TYPE_ARR   || ap->kind == TYPE_PTR ||
                       ap->kind == TYPE_STRING ||
                       type_is_bool(ap)        || ap->kind == TYPE_FLOAT)) {
                goto skip_coll_as_fn_ba;
            }
            {
                bool ok = infer_unify_one(ctx, a->arrow_param, type_int(), line, col);
                if (!ok) return false;
                if (b->kind == TYPE_COLL) return infer_unify_one(ctx, a->arrow_ret, b->element_type, line, col);
                if (b->kind == TYPE_ARR) return infer_unify_one(ctx, a->arrow_ret, b->arr_element_type, line, col);
                if (b->kind == TYPE_PTR) return infer_unify_one(ctx, a->arrow_ret, b->element_type, line, col);
                if (b->kind == TYPE_STRING) return infer_unify_one(ctx, a->arrow_ret, type_char(), line, col);
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
        Type *a_single = a->kind == TYPE_LIST && a->list_count == 1
            ? a->list_types[0] : NULL;
        Type *b_single = b->kind == TYPE_LIST && b->list_count == 1
            ? b->list_types[0] : NULL;
        bool a_single_is_collection = a_single &&
            (a_single->kind == TYPE_COLL || a_single->kind == TYPE_LIST ||
             a_single->kind == TYPE_ARR || a_single->kind == TYPE_SET ||
             a_single->kind == TYPE_MAP || a_single->kind == TYPE_STRING);
        bool b_single_is_collection = b_single &&
            (b_single->kind == TYPE_COLL || b_single->kind == TYPE_LIST ||
             b_single->kind == TYPE_ARR || b_single->kind == TYPE_SET ||
             b_single->kind == TYPE_MAP || b_single->kind == TYPE_STRING);
        if (a_single && b->kind != TYPE_LIST &&
            (b->kind != TYPE_COLL || a_single_is_collection))
            return infer_unify_one_internal(ctx, a->list_types[0], b, line, col);
        if (b_single && a->kind != TYPE_LIST &&
            (a->kind != TYPE_COLL || b_single_is_collection))
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
                if (a->app_arg && b->app_arg && ctx->dctx &&
                    dep_is_indexed_family(ctx->dctx, a->app_constructor) &&
                    a->app_arg->kind == TYPE_LIST &&
                    b->app_arg->kind == TYPE_LIST &&
                    a->app_arg->list_count > 0 && b->app_arg->list_count > 0) {
                    /* Indices are compile-time evidence and are checked by
                     * dependent unification.  HM only relates the final,
                     * runtime type parameter. */
                    return infer_unify_one(
                        ctx,
                        a->app_arg->list_types[a->app_arg->list_count - 1],
                        b->app_arg->list_types[b->app_arg->list_count - 1],
                        line, col);
                }
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
    case TYPE_SET:
        if (a->element_type && b->element_type)
            return infer_unify_one(ctx, a->element_type, b->element_type,
                                   line, col);
        return true;
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

static void infer_free_vars_env_excluding(
    InferCtx *ctx, InferEnv *env, const char *excluded_name,
    int *out, int *count, int cap) {
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
            if (excluded_name && !strcmp(e->name, excluded_name)) continue;
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

void infer_free_vars_env(
    InferCtx *ctx, InferEnv *env, int *out, int *count, int cap) {
    infer_free_vars_env_excluding(ctx, env, NULL, out, count, cap);
}

/// Generalisation and Instantiation

TypeScheme *infer_generalise_excluding(
    InferCtx *ctx, Type *t, InferEnv *outer_env,
    const char *excluded_name) {
    /* Apply substitution fully first */
    t = subst_apply(ctx->subst, t);

    /* Collect free vars in t */
    int type_free[INFER_MAX_VARS];
    int type_free_count = 0;
    infer_free_vars_type(ctx->subst, t, type_free, &type_free_count, INFER_MAX_VARS);

    /* Collect free vars in the outer environment */
    int env_free[INFER_MAX_VARS];
    int env_free_count = 0;
    infer_free_vars_env_excluding(
        ctx, outer_env, excluded_name,
        env_free, &env_free_count, INFER_MAX_VARS);

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

TypeScheme *infer_generalise(InferCtx *ctx, Type *t, InferEnv *outer_env) {
    return infer_generalise_excluding(ctx, t, outer_env, NULL);
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
    case TYPE_ARROW: {
        Type *arrow = type_arrow(
            infer_substitute_vars(t->arrow_param, from, to, count),
            infer_substitute_vars(t->arrow_ret, from, to, count));
        arrow->arrow_effect_complete = t->arrow_effect_complete;
        arrow->arrow_effect_name = t->arrow_effect_name
            ? strdup(t->arrow_effect_name) : NULL;
        if (t->arrow_effect_scheme) {
            arrow->arrow_effect_scheme = strdup(t->arrow_effect_scheme);
            arrow->arrow_effect_scheme_owned =
                arrow->arrow_effect_scheme != NULL;
        }
        return arrow;
    }

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

Type *infer_substitute_type_vars(Type *t, int *from, Type **to, int count) {
    return infer_substitute_vars(t, from, to, count);
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

static size_t infer_arrow_domain_count(Type *type) {
    size_t count = 0;
    while (type && type->kind == TYPE_ARROW) {
        count++;
        type = type->arrow_ret;
    }
    return count;
}

/* Canonical arrows are the primary source of per-stage latent effects.
 * The TypeScheme vectors remain a migration oracle for legacy producers. */
static size_t infer_scheme_arrow_effect_count(const TypeScheme *scheme) {
    if (!scheme) return 0;
    size_t domains = 0;
    bool canonical = true;
    for (Type *arrow = scheme->type;
         arrow && arrow->kind == TYPE_ARROW; arrow = arrow->arrow_ret) {
        domains++;
        canonical = canonical && arrow->arrow_effect_scheme != NULL;
    }
    return domains && canonical ? domains : scheme->arrow_effect_count;
}

static QttEffectScheme *infer_scheme_arrow_effect_at(
    const TypeScheme *scheme, size_t index, bool *complete,
    bool *from_canonical) {
    if (complete) *complete = false;
    if (from_canonical) *from_canonical = false;
    if (!scheme) return NULL;
    Type *arrow = scheme->type;
    for (size_t i = 0;
         arrow && arrow->kind == TYPE_ARROW && i < index; i++)
        arrow = arrow->arrow_ret;
    if (arrow && arrow->kind == TYPE_ARROW &&
        arrow->arrow_effect_scheme) {
        QttEffectScheme *effect = qtt_effect_scheme_deserialize(
            arrow->arrow_effect_scheme);
        if (effect) {
            if (complete) *complete = arrow->arrow_effect_complete;
            if (from_canonical) *from_canonical = true;
            return effect;
        }
    }
    if (index >= scheme->arrow_effect_count ||
        !scheme->arrow_effect_schemes[index])
        return NULL;
    if (complete) *complete = scheme->arrow_effects_complete[index];
    return qtt_effect_scheme_retain(scheme->arrow_effect_schemes[index]);
}

static const char *infer_scheme_arrow_effect_name_at(
    const TypeScheme *scheme, size_t index) {
    Type *arrow = scheme ? scheme->type : NULL;
    for (size_t i = 0;
         arrow && arrow->kind == TYPE_ARROW && i < index; i++)
        arrow = arrow->arrow_ret;
    return arrow && arrow->kind == TYPE_ARROW
        ? arrow->arrow_effect_name : NULL;
}

bool scheme_instantiate_effect_trait_constraints(
    const TypeScheme *scheme, QttEffectArena *arena,
    QttEffectConstraintSet *constraints) {
    if (!scheme || !arena || !constraints) return false;
    for (size_t i = 0; i < scheme->effect_trait_predicate_count; i++) {
        bool complete = false;
        QttEffectScheme *stage = infer_scheme_arrow_effect_at(
            scheme, scheme->effect_trait_predicate_stages[i],
            &complete, NULL);
        QttEffectRow *row = stage
            ? qtt_effect_instantiate(arena, stage) : NULL;
        qtt_effect_scheme_free(stage);
        if (!row || !qtt_effect_constrain_has_trait(
                constraints, row, scheme->effect_trait_predicate_names[i]))
            return false;
    }
    return true;
}

void infer_quantitative_type_free(InferQuantitativeType *instance) {
    if (!instance) return;
    free(instance->domain_grades);
    free(instance->domain_effects);
    free(instance->domain_effects_complete);
    free(instance->closure_module_ids);
    free(instance->closure_ids);
    free(instance->closure_binder_ids);
    free(instance->closure_slots);
    free(instance->closure_parameter_indices);
    free(instance->closure_origin_kinds);
    free(instance->closure_origin_ids);
    free(instance->closure_domain_module_ids);
    free(instance->closure_domain_ids);
    free(instance->closure_domain_indices);
    free(instance->result_closure_module_ids);
    free(instance->result_closure_ids);
    free(instance->callable_parameter_indices);
    free(instance->callable_domain_parameter_indices);
    free(instance->callable_domain_indices);
    memset(instance, 0, sizeof(*instance));
}

InferQuantitativeResult infer_instantiate_quantitative(
    InferCtx *ctx, TypeScheme *scheme, InferQuantitativeType *out) {
    if (!ctx || !scheme || !out)
        return INFER_QUANTITATIVE_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    Type *type = infer_instantiate(ctx, scheme);
    size_t arrow_count = infer_arrow_domain_count(type);
    size_t grade_count = scheme_grade_count(scheme);

    if (!scheme->grade_scheme || grade_count != arrow_count)
        return INFER_QUANTITATIVE_GRADE_ARITY_MISMATCH;
    size_t arrow_effect_count = infer_scheme_arrow_effect_count(scheme);
    if (arrow_effect_count && arrow_effect_count != arrow_count)
        return INFER_QUANTITATIVE_EFFECT_ARITY_MISMATCH;

    size_t closure_count = scheme_closure_grade_count(scheme);
    size_t result_count =
        qtt_grade_scheme_result_closure_count(scheme->grade_scheme);
    size_t closure_domain_count =
        qtt_grade_scheme_closure_domain_count(scheme->grade_scheme);
    size_t callable_count =
        qtt_grade_scheme_callable_parameter_count(scheme->grade_scheme);
    size_t callable_domain_count =
        qtt_grade_scheme_callable_domain_count(scheme->grade_scheme);
    QttGradeExpr **grades =
        qtt_grade_instantiate(ctx->grade_arena, scheme->grade_scheme);
    if ((grade_count + closure_count + closure_domain_count) && !grades)
        return INFER_QUANTITATIVE_OUT_OF_MEMORY;
    uint64_t *closure_modules = closure_count
        ? malloc(closure_count * sizeof(*closure_modules)) : NULL;
    uint64_t *closure_ids = closure_count
        ? malloc(closure_count * sizeof(*closure_ids)) : NULL;
    uint64_t *closure_binders = closure_count
        ? malloc(closure_count * sizeof(*closure_binders)) : NULL;
    size_t *closure_slots = closure_count
        ? malloc(closure_count * sizeof(*closure_slots)) : NULL;
    size_t *closure_parameters = closure_count
        ? malloc(closure_count * sizeof(*closure_parameters)) : NULL;
    int *closure_origin_kinds = closure_count
        ? malloc(closure_count * sizeof(*closure_origin_kinds)) : NULL;
    uint64_t *closure_origin_ids = closure_count
        ? malloc(closure_count * sizeof(*closure_origin_ids)) : NULL;
    uint64_t *closure_domain_modules = closure_domain_count
        ? malloc(closure_domain_count *
                 sizeof(*closure_domain_modules)) : NULL;
    uint64_t *closure_domain_ids = closure_domain_count
        ? malloc(closure_domain_count * sizeof(*closure_domain_ids)) : NULL;
    size_t *closure_domain_indices = closure_domain_count
        ? malloc(closure_domain_count *
                 sizeof(*closure_domain_indices)) : NULL;
    uint64_t *result_modules = result_count
        ? malloc(result_count * sizeof(*result_modules)) : NULL;
    uint64_t *result_ids = result_count
        ? malloc(result_count * sizeof(*result_ids)) : NULL;
    size_t *callable_parameters = callable_count
        ? malloc(callable_count * sizeof(*callable_parameters)) : NULL;
    size_t *callable_domain_parameters = callable_domain_count
        ? malloc(callable_domain_count *
                 sizeof(*callable_domain_parameters)) : NULL;
    size_t *callable_domain_indices = callable_domain_count
        ? malloc(callable_domain_count *
                 sizeof(*callable_domain_indices)) : NULL;
    if (closure_count &&
        (!closure_modules || !closure_ids || !closure_binders ||
         !closure_slots || !closure_parameters ||
         !closure_origin_kinds || !closure_origin_ids)) {
        free(result_modules);
        free(result_ids);
        free(closure_modules);
        free(closure_ids);
        free(closure_binders);
        free(closure_slots);
        free(closure_parameters);
        free(closure_origin_kinds);
        free(closure_origin_ids);
        free(callable_parameters);
        free(callable_domain_parameters);
        free(callable_domain_indices);
        free(closure_domain_modules);
        free(closure_domain_ids);
        free(closure_domain_indices);
        free(grades);
        return INFER_QUANTITATIVE_OUT_OF_MEMORY;
    }
    if ((result_count && (!result_modules || !result_ids)) ||
        (callable_count && !callable_parameters) ||
        (callable_domain_count &&
         (!callable_domain_parameters || !callable_domain_indices))) {
        free(result_modules);
        free(result_ids);
        free(closure_modules);
        free(closure_ids);
        free(closure_binders);
        free(closure_slots);
        free(closure_parameters);
        free(closure_origin_kinds);
        free(closure_origin_ids);
        free(callable_parameters);
        free(callable_domain_parameters);
        free(callable_domain_indices);
        free(closure_domain_modules);
        free(closure_domain_ids);
        free(closure_domain_indices);
        free(grades);
        return INFER_QUANTITATIVE_OUT_OF_MEMORY;
    }
    if (closure_domain_count &&
        (!closure_domain_modules || !closure_domain_ids ||
         !closure_domain_indices)) {
        free(result_modules);
        free(result_ids);
        free(closure_modules);
        free(closure_ids);
        free(closure_binders);
        free(closure_slots);
        free(closure_parameters);
        free(closure_origin_kinds);
        free(closure_origin_ids);
        free(callable_parameters);
        free(callable_domain_parameters);
        free(callable_domain_indices);
        free(closure_domain_modules);
        free(closure_domain_ids);
        free(closure_domain_indices);
        free(grades);
        return INFER_QUANTITATIVE_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < closure_count; i++) {
        closure_modules[i] =
            qtt_grade_scheme_closure_module(scheme->grade_scheme, i);
        closure_ids[i] =
            qtt_grade_scheme_closure_id(scheme->grade_scheme, i);
        closure_binders[i] =
            qtt_grade_scheme_closure_binder(scheme->grade_scheme, i);
        closure_slots[i] =
            qtt_grade_scheme_closure_slot(scheme->grade_scheme, i);
        closure_parameters[i] =
            qtt_grade_scheme_closure_parameter_index(
                scheme->grade_scheme, i);
        closure_origin_kinds[i] =
            qtt_grade_scheme_closure_origin_kind(
                scheme->grade_scheme, i);
        closure_origin_ids[i] =
            qtt_grade_scheme_closure_origin_id(
                scheme->grade_scheme, i);
    }
    for (size_t i = 0; i < result_count; i++) {
        result_modules[i] = qtt_grade_scheme_result_closure_module(
            scheme->grade_scheme, i);
        result_ids[i] = qtt_grade_scheme_result_closure_id(
            scheme->grade_scheme, i);
    }
    for (size_t i = 0; i < callable_count; i++)
        callable_parameters[i] =
            qtt_grade_scheme_callable_parameter_index(
                scheme->grade_scheme, i);
    for (size_t i = 0; i < callable_domain_count; i++) {
        callable_domain_parameters[i] =
            qtt_grade_scheme_callable_domain_parameter_index(
                scheme->grade_scheme, i);
        callable_domain_indices[i] =
            qtt_grade_scheme_callable_domain_index(
                scheme->grade_scheme, i);
    }
    for (size_t i = 0; i < closure_domain_count; i++) {
        closure_domain_modules[i] =
            qtt_grade_scheme_closure_domain_module(
                scheme->grade_scheme, i);
        closure_domain_ids[i] = qtt_grade_scheme_closure_domain_id(
            scheme->grade_scheme, i);
        closure_domain_indices[i] =
            qtt_grade_scheme_closure_domain_index(
                scheme->grade_scheme, i);
    }

    out->type = type;
    out->domain_grades = grades;
    out->domain_count = grade_count;
    out->closure_module_ids = closure_modules;
    out->closure_ids = closure_ids;
    out->closure_binder_ids = closure_binders;
    out->closure_slots = closure_slots;
    out->closure_parameter_indices = closure_parameters;
    out->closure_origin_kinds = closure_origin_kinds;
    out->closure_origin_ids = closure_origin_ids;
    out->closure_grades = grades ? grades + grade_count : NULL;
    out->closure_grade_count = closure_count;
    out->closure_domain_module_ids = closure_domain_modules;
    out->closure_domain_ids = closure_domain_ids;
    out->closure_domain_indices = closure_domain_indices;
    out->closure_domain_grades = grades
        ? grades + grade_count + closure_count : NULL;
    out->closure_domain_grade_count = closure_domain_count;
    out->result_closure_module_ids = result_modules;
    out->result_closure_ids = result_ids;
    out->result_closure_count = result_count;
    out->callable_parameter_indices = callable_parameters;
    out->callable_invocation_grades = grades
        ? grades + grade_count + closure_count + closure_domain_count
        : NULL;
    out->callable_parameter_count = callable_count;
    out->callable_domain_parameter_indices =
        callable_domain_parameters;
    out->callable_domain_indices = callable_domain_indices;
    out->callable_domain_grades = grades
        ? grades + grade_count + closure_count + closure_domain_count +
              callable_count
        : NULL;
    out->callable_domain_count = callable_domain_count;
    out->effect_arena = ctx->effect_arena;
    out->effect_solver = ctx->effect_solver;
    out->latent_effects = out->effect_solver
        ? (scheme->effect_scheme
            ? qtt_effect_instantiate(
                  out->effect_arena, scheme->effect_scheme)
            : qtt_effect_empty(out->effect_arena))
        : NULL;
    out->effects_complete = scheme->effects_complete;
    if (!out->latent_effects) {
        infer_quantitative_type_free(out);
        return INFER_QUANTITATIVE_OUT_OF_MEMORY;
    }
    out->domain_effect_count = arrow_effect_count;
    if (out->domain_effect_count) {
        out->domain_effects = calloc(
            out->domain_effect_count, sizeof(*out->domain_effects));
        out->domain_effects_complete = malloc(
            out->domain_effect_count *
            sizeof(*out->domain_effects_complete));
        if (!out->domain_effects || !out->domain_effects_complete) {
            infer_quantitative_type_free(out);
            return INFER_QUANTITATIVE_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < out->domain_effect_count; i++) {
            bool effect_complete = false;
            QttEffectScheme *effect = infer_scheme_arrow_effect_at(
                scheme, i, &effect_complete, NULL);
            out->domain_effects[i] = qtt_effect_instantiate(
                out->effect_arena, effect);
            out->domain_effects_complete[i] = effect_complete;
            qtt_effect_scheme_free(effect);
            if (!out->domain_effects[i]) {
                infer_quantitative_type_free(out);
                return INFER_QUANTITATIVE_OUT_OF_MEMORY;
            }
        }
    }
    return INFER_QUANTITATIVE_OK;
}

bool infer_quantitative_take_domain(
    InferQuantitativeType *instance,
    Type **parameter,
    QttGradeExpr **grade) {
    return infer_quantitative_take_domain_contract(
        instance, parameter, grade, NULL, NULL);
}

bool infer_quantitative_take_domain_contract(
    InferQuantitativeType *instance, Type **parameter,
    QttGradeExpr **grade, QttEffectRow **effects,
    bool *effects_complete) {
    if (!instance || !instance->type ||
        instance->type->kind != TYPE_ARROW ||
        instance->domain_offset >= instance->domain_count)
        return false;

    if (parameter) *parameter = instance->type->arrow_param;
    if (grade)
        *grade = instance->domain_grades[instance->domain_offset];
    if (effects)
        *effects = instance->domain_effect_offset <
                instance->domain_effect_count
            ? instance->domain_effects[instance->domain_effect_offset]
            : NULL;
    if (effects_complete)
        *effects_complete = instance->domain_effect_offset <
                instance->domain_effect_count
            ? instance->domain_effects_complete[
                  instance->domain_effect_offset]
            : false;
    instance->domain_offset++;
    if (instance->domain_effect_offset < instance->domain_effect_count)
        instance->domain_effect_offset++;
    instance->type = instance->type->arrow_ret;
    return true;
}

QttGradeExpr *infer_quantitative_closure_grade(
    const InferQuantitativeType *instance,
    uint64_t module_id,
    uint64_t closure_id,
    uint64_t binder_id) {
    if (!instance || !module_id || !closure_id || !binder_id) return NULL;
    for (size_t i = 0; i < instance->closure_grade_count; i++)
        if (instance->closure_module_ids[i] == module_id &&
            instance->closure_ids[i] == closure_id &&
            instance->closure_binder_ids[i] == binder_id)
            return instance->closure_grades[i];
    return NULL;
}

QttGradeExpr *infer_quantitative_closure_slot_grade(
    const InferQuantitativeType *instance,
    uint64_t module_id,
    uint64_t closure_id,
    size_t slot) {
    if (!instance || !module_id || !closure_id) return NULL;
    for (size_t i = 0; i < instance->closure_grade_count; i++)
        if (instance->closure_module_ids[i] == module_id &&
            instance->closure_ids[i] == closure_id &&
            instance->closure_slots[i] == slot)
            return instance->closure_grades[i];
    return NULL;
}

QttGradeExpr *infer_quantitative_closure_domain_grade(
    const InferQuantitativeType *instance,
    uint64_t module_id,
    uint64_t closure_id,
    size_t parameter_index) {
    if (!instance || !module_id || !closure_id) return NULL;
    for (size_t i = 0; i < instance->closure_domain_grade_count; i++)
        if (instance->closure_domain_module_ids[i] == module_id &&
            instance->closure_domain_ids[i] == closure_id &&
            instance->closure_domain_indices[i] == parameter_index)
            return instance->closure_domain_grades[i];
    return NULL;
}

QttGradeExpr *infer_quantitative_callable_domain_grade(
    const InferQuantitativeType *instance,
    size_t callable_parameter_index, size_t domain_index) {
    if (!instance) return NULL;
    for (size_t i = 0; i < instance->callable_domain_count; i++)
        if (instance->callable_domain_parameter_indices[i] ==
                callable_parameter_index &&
            instance->callable_domain_indices[i] == domain_index)
            return instance->callable_domain_grades[i];
    return NULL;
}

size_t infer_grade_application_count(const InferCtx *ctx) {
    return ctx ? ctx->grade_application_count : 0;
}

const InferGradeApplication *infer_grade_application(
    const InferCtx *ctx, size_t index) {
    if (!ctx || index >= ctx->grade_application_count) return NULL;
    return &ctx->grade_applications[index];
}

static bool infer_retain_quantitative_application(
    InferCtx *ctx,
    const AST *application,
    InferQuantitativeType *instance,
    size_t applied_count) {
    if (!ctx || !instance || applied_count > instance->domain_count)
        return false;
    if (ctx->grade_application_count == ctx->grade_application_cap) {
        size_t capacity = ctx->grade_application_cap
            ? ctx->grade_application_cap * 2 : 8;
        InferGradeApplication *grown = realloc(
            ctx->grade_applications, capacity * sizeof(*grown));
        if (!grown) return false;
        ctx->grade_applications = grown;
        ctx->grade_application_cap = capacity;
    }
    InferGradeApplication *record =
        &ctx->grade_applications[ctx->grade_application_count++];
    memset(record, 0, sizeof(*record));
    uint64_t *result_instances = NULL;
    QttClosureEnvironment **result_environments = NULL;
    if (applied_count == instance->domain_count &&
        instance->result_closure_count) {
        if (!ctx->next_closure_instance_id ||
            instance->result_closure_count - 1 >
                UINT64_MAX - ctx->next_closure_instance_id) {
            ctx->grade_application_count--;
            return false;
        }
        result_instances = malloc(
            instance->result_closure_count * sizeof(*result_instances));
        result_environments = calloc(
            instance->result_closure_count, sizeof(*result_environments));
        if (!result_instances || !result_environments) {
            free(result_instances);
            free(result_environments);
            ctx->grade_application_count--;
            return false;
        }
        for (size_t i = 0; i < instance->result_closure_count; i++) {
            result_instances[i] = ctx->next_closure_instance_id++;
            size_t slot_count = 0;
            for (size_t j = 0; j < instance->closure_grade_count; j++)
                if (instance->closure_module_ids[j] ==
                        instance->result_closure_module_ids[i] &&
                    instance->closure_ids[j] ==
                        instance->result_closure_ids[i]) {
                    if (instance->closure_slots[j] >= slot_count)
                        slot_count = instance->closure_slots[j] + 1;
                }
            QttEnvironmentOrigin *origins = slot_count
                ? calloc(slot_count, sizeof(*origins)) : NULL;
            QttEnvironmentOrigin *arguments = applied_count
                ? calloc(applied_count, sizeof(*arguments)) : NULL;
            if ((slot_count && !origins) ||
                (applied_count && !arguments)) {
                free(origins);
                free(arguments);
                goto environment_failure;
            }
            for (size_t j = 0; j < applied_count; j++) {
                const AST *argument = application->list.items[j + 1];
                arguments[j] = argument->type == AST_SYMBOL &&
                        argument->resolved_binder_id
                    ? qtt_environment_local(argument->resolved_binder_id)
                    : qtt_environment_expression(
                          (uint64_t)(uintptr_t)argument);
            }
            for (size_t j = 0; j < instance->closure_grade_count; j++)
                if (instance->closure_module_ids[j] ==
                        instance->result_closure_module_ids[i] &&
                    instance->closure_ids[j] ==
                        instance->result_closure_ids[i])
                    origins[instance->closure_slots[j]] =
                        instance->closure_origin_kinds[j] ==
                                QTT_ENVIRONMENT_PARAMETER
                            ? qtt_environment_parameter(
                                  instance->closure_origin_ids[j])
                            : instance->closure_origin_kinds[j] ==
                                    QTT_ENVIRONMENT_EXPRESSION
                                ? qtt_environment_expression(
                                      instance->closure_origin_ids[j])
                                : qtt_environment_local(
                                      instance->closure_origin_ids[j]);
            QttClosureEnvironment *environment = qtt_environment_new(
                result_instances[i],
                instance->result_closure_module_ids[i],
                instance->result_closure_ids[i], origins, slot_count,
                instance->domain_count);
            result_environments[i] = qtt_environment_substitute(
                environment, arguments, applied_count);
            qtt_environment_free(environment);
            free(origins);
            free(arguments);
            if (!result_environments[i]) goto environment_failure;
        }
    }
    record->application = application;
    record->domain_grades = instance->domain_grades;
    record->applied_count = applied_count;
    record->closure_module_ids = instance->closure_module_ids;
    record->closure_ids = instance->closure_ids;
    record->closure_binder_ids = instance->closure_binder_ids;
    record->closure_slots = instance->closure_slots;
    record->closure_parameter_indices =
        instance->closure_parameter_indices;
    record->closure_origin_kinds = instance->closure_origin_kinds;
    record->closure_origin_ids = instance->closure_origin_ids;
    record->closure_grades = instance->closure_grades;
    record->closure_grade_count = instance->closure_grade_count;
    record->closure_domain_module_ids =
        instance->closure_domain_module_ids;
    record->closure_domain_ids = instance->closure_domain_ids;
    record->closure_domain_indices = instance->closure_domain_indices;
    record->closure_domain_grades = instance->closure_domain_grades;
    record->closure_domain_grade_count =
        instance->closure_domain_grade_count;
    record->callable_parameter_indices =
        instance->callable_parameter_indices;
    record->callable_invocation_grades =
        instance->callable_invocation_grades;
    record->callable_parameter_count =
        instance->callable_parameter_count;
    record->callable_domain_parameter_indices =
        instance->callable_domain_parameter_indices;
    record->callable_domain_indices =
        instance->callable_domain_indices;
    record->callable_domain_grades =
        instance->callable_domain_grades;
    record->callable_domain_count =
        instance->callable_domain_count;
    if (applied_count == instance->domain_count) {
        record->result_closure_module_ids =
            instance->result_closure_module_ids;
        record->result_closure_ids = instance->result_closure_ids;
        record->result_closure_instance_ids = result_instances;
        record->result_closure_environments = result_environments;
        record->result_closure_count = instance->result_closure_count;
        instance->result_closure_module_ids = NULL;
        instance->result_closure_ids = NULL;
        instance->result_closure_count = 0;
    }
    instance->domain_grades = NULL;
    instance->closure_module_ids = NULL;
    instance->closure_ids = NULL;
    instance->closure_binder_ids = NULL;
    instance->closure_slots = NULL;
    instance->closure_parameter_indices = NULL;
    instance->closure_origin_kinds = NULL;
    instance->closure_origin_ids = NULL;
    instance->closure_grades = NULL;
    instance->closure_grade_count = 0;
    instance->closure_domain_module_ids = NULL;
    instance->closure_domain_ids = NULL;
    instance->closure_domain_indices = NULL;
    instance->closure_domain_grades = NULL;
    instance->closure_domain_grade_count = 0;
    instance->callable_parameter_indices = NULL;
    instance->callable_invocation_grades = NULL;
    instance->callable_parameter_count = 0;
    instance->callable_domain_parameter_indices = NULL;
    instance->callable_domain_indices = NULL;
    instance->callable_domain_grades = NULL;
    instance->callable_domain_count = 0;
    return true;

environment_failure:
    for (size_t i = 0; i < instance->result_closure_count; i++)
        qtt_environment_free(result_environments[i]);
    free(result_environments);
    free(result_instances);
    ctx->grade_application_count--;
    return false;
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
    if (!s) return NULL;
    TypeScheme *c       = calloc(1, sizeof(TypeScheme));
    if (!c) return NULL;
    c->quantified_count = s->quantified_count;
    if (s->quantified_count) {
        c->quantified = malloc(sizeof(int) * s->quantified_count);
        if (!c->quantified) {
            free(c);
            return NULL;
        }
        memcpy(c->quantified, s->quantified,
               sizeof(int) * s->quantified_count);
    }
    c->type = s->owns_type ? type_clone(s->type) : s->type;
    c->owns_type = s->owns_type;
    if (s->owns_type && !c->type) {
        free(c->quantified);
        free(c);
        return NULL;
    }
    c->grade_scheme = qtt_grade_scheme_retain(s->grade_scheme);
    c->effect_scheme = qtt_effect_scheme_retain(s->effect_scheme);
    c->effects_complete = s->effects_complete;
    if (!scheme_set_arrow_effect_schemes(
            c, s->arrow_effect_schemes, s->arrow_effects_complete,
            s->arrow_effect_count)) {
        scheme_free(c);
        return NULL;
    }
    if (!scheme_set_effect_trait_predicates(c,
            s->effect_trait_predicate_stages,
            (const char *const *)s->effect_trait_predicate_names,
            s->effect_trait_predicate_count)) {
        scheme_free(c); return NULL;
    }
    return c;
}

void scheme_free(TypeScheme *s) {
    if (!s) return;
    free(s->quantified);
    qtt_grade_scheme_free(s->grade_scheme);
    qtt_effect_scheme_free(s->effect_scheme);
    for (size_t i = 0; i < s->arrow_effect_count; i++)
        qtt_effect_scheme_free(s->arrow_effect_schemes[i]);
    free(s->arrow_effect_schemes);
    free(s->arrow_effects_complete);
    for (size_t i = 0; i < s->effect_trait_predicate_count; i++)
        free(s->effect_trait_predicate_names[i]);
    free(s->effect_trait_predicate_names);
    free(s->effect_trait_predicate_stages);
    if (s->owns_type) type_free(s->type);
    free(s);
}

typedef struct { size_t stage; char *trait; } SchemeTraitPredicate;
static int scheme_trait_predicate_compare(const void *a, const void *b) {
    const SchemeTraitPredicate *x = a, *y = b;
    if (x->stage != y->stage) return x->stage < y->stage ? -1 : 1;
    return strcmp(x->trait, y->trait);
}

bool scheme_set_effect_trait_predicates(
    TypeScheme *scheme, const size_t *stages,
    const char *const *traits, size_t count) {
    if (!scheme || (count && (!stages || !traits))) return false;
    size_t stage_count = 0;
    for (Type *arrow = scheme->type;
         arrow && arrow->kind == TYPE_ARROW; arrow = arrow->arrow_ret)
        stage_count++;
    SchemeTraitPredicate *items = count ? calloc(count, sizeof(*items)) : NULL;
    if (count && !items) return false;
    for (size_t i = 0; i < count; i++) {
        if (stages[i] >= stage_count) {
            for (size_t j = 0; j < i; j++) free(items[j].trait);
            free(items); return false;
        }
        items[i].stage = stages[i];
        items[i].trait = qtt_effect_traits_normalize(traits[i]);
        if (!items[i].trait || strchr(items[i].trait, ',')) {
            for (size_t j = 0; j <= i; j++) free(items[j].trait);
            free(items); return false;
        }
    }
    if (count)
        qsort(items, count, sizeof(*items), scheme_trait_predicate_compare);
    size_t unique = 0;
    for (size_t i = 0; i < count; i++) {
        if (unique && items[unique-1].stage == items[i].stage &&
            !strcmp(items[unique-1].trait, items[i].trait)) {
            free(items[i].trait); continue;
        }
        items[unique++] = items[i];
    }
    size_t *owned_stages = unique ? malloc(unique * sizeof(*owned_stages)) : NULL;
    char **owned_traits = unique ? malloc(unique * sizeof(*owned_traits)) : NULL;
    if (unique && (!owned_stages || !owned_traits)) {
        for (size_t i = 0; i < unique; i++) free(items[i].trait);
        free(items); free(owned_stages); free(owned_traits); return false;
    }
    for (size_t i = 0; i < unique; i++) {
        owned_stages[i] = items[i].stage; owned_traits[i] = items[i].trait;
    }
    free(items);
    for (size_t i = 0; i < scheme->effect_trait_predicate_count; i++)
        free(scheme->effect_trait_predicate_names[i]);
    free(scheme->effect_trait_predicate_names);
    free(scheme->effect_trait_predicate_stages);
    scheme->effect_trait_predicate_stages = owned_stages;
    scheme->effect_trait_predicate_names = owned_traits;
    scheme->effect_trait_predicate_count = unique;
    return true;
}

size_t scheme_effect_trait_predicate_count(const TypeScheme *scheme) {
    return scheme ? scheme->effect_trait_predicate_count : 0;
}
size_t scheme_effect_trait_predicate_stage(const TypeScheme *scheme, size_t i) {
    return scheme && i < scheme->effect_trait_predicate_count
        ? scheme->effect_trait_predicate_stages[i] : SIZE_MAX;
}
const char *scheme_effect_trait_predicate_name(const TypeScheme *scheme, size_t i) {
    return scheme && i < scheme->effect_trait_predicate_count
        ? scheme->effect_trait_predicate_names[i] : NULL;
}

static int scheme_quantifier_index(
    const TypeScheme *scheme, int variable) {
    for (int i = 0; i < scheme->quantified_count; i++)
        if (scheme->quantified[i] == variable) return i;
    return -1;
}

static bool scheme_canonicalize_type(
    Type *type, const TypeScheme *scheme, unsigned depth) {
    if (!type || !scheme || depth > 128) return false;
    if (type->kind == TYPE_VAR) {
        int canonical = scheme_quantifier_index(scheme, type->var_id);
        if (canonical < 0) return false;
        type->var_id = canonical;
        return true;
    }
#define WALK(child) \
    do { if ((child) && !scheme_canonicalize_type( \
             (child), scheme, depth + 1)) return false; } while (0)
    switch (type->kind) {
    case TYPE_ARROW: WALK(type->arrow_param); WALK(type->arrow_ret); break;
    case TYPE_FN:
        for (int i = 0; i < type->param_count; i++) WALK(type->params[i].type);
        WALK(type->return_type);
        break;
    case TYPE_LIST:
        for (int i = 0; i < type->list_count; i++) WALK(type->list_types[i]);
        break;
    case TYPE_OPTIONAL: case TYPE_PTR: case TYPE_COLL: case TYPE_VARIADIC:
        WALK(type->element_type); break;
    case TYPE_ARR: WALK(type->arr_element_type); break;
    case TYPE_MAP: WALK(type->map_key_type); WALK(type->map_value_type); break;
    case TYPE_LAYOUT:
        for (int i = 0; i < type->layout_field_count; i++)
            WALK(type->layout_fields[i].type);
        break;
    case TYPE_APP: WALK(type->app_arg); break;
    default: break;
    }
#undef WALK
    return type->kind != TYPE_UNKNOWN;
}

static uint64_t scheme_text_fingerprint(const char *text) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}

char *infer_type_scheme_serialize(const TypeScheme *scheme) {
    if (!scheme || !scheme->type || scheme->quantified_count < 0 ||
        scheme->quantified_count > 4096) return NULL;
    for (int i = 0; i < scheme->quantified_count; i++)
        for (int j = i + 1; j < scheme->quantified_count; j++)
            if (scheme->quantified[i] == scheme->quantified[j]) return NULL;
    Type *canonical = type_clone(scheme->type);
    if (!canonical || !scheme_canonicalize_type(canonical, scheme, 0)) {
        type_free(canonical);
        return NULL;
    }
    char *body = qtt_type_serialize(canonical);
    type_free(canonical);
    if (!body) return NULL;
    size_t predicate_size = 32;
    for (size_t i = 0; i < scheme->effect_trait_predicate_count; i++)
        predicate_size += strlen(scheme->effect_trait_predicate_names[i]) + 32;
    size_t size = strlen(body) + predicate_size + 96;
    char *text = malloc(size);
    if (text && !scheme->effect_trait_predicate_count)
        snprintf(text, size, "monad-hm-scheme-v1|%016llx|%d|%s",
            (unsigned long long)scheme_text_fingerprint(body),
            scheme->quantified_count, body);
    else if (text) {
        char *payload = malloc(strlen(body) + predicate_size);
        if (!payload) { free(text); text = NULL; }
        else {
            size_t used = (size_t)snprintf(payload,
                strlen(body) + predicate_size, "%s|%zu|", body,
                scheme->effect_trait_predicate_count);
            for (size_t i = 0; i < scheme->effect_trait_predicate_count; i++)
                used += (size_t)snprintf(payload + used,
                    strlen(body) + predicate_size - used, "%s%zu:%s",
                    i ? "," : "", scheme->effect_trait_predicate_stages[i],
                    scheme->effect_trait_predicate_names[i]);
            snprintf(text, size, "monad-hm-scheme-v2|%016llx|%d|%s",
                (unsigned long long)scheme_text_fingerprint(payload),
                scheme->quantified_count, payload);
            free(payload);
        }
    }
    free(body);
    return text;
}

TypeScheme *infer_type_scheme_deserialize(const char *text) {
    static const char header_v1[] = "monad-hm-scheme-v1|";
    static const char header_v2[] = "monad-hm-scheme-v2|";
    bool v2 = text && !strncmp(text, header_v2, sizeof(header_v2) - 1);
    if (!v2 && (!text || strncmp(text, header_v1, sizeof(header_v1) - 1)))
        return NULL;
    const char *fingerprint_text = text + (v2
        ? sizeof(header_v2) - 1 : sizeof(header_v1) - 1);
    char *fingerprint_end = NULL;
    errno = 0;
    unsigned long long fingerprint =
        strtoull(fingerprint_text, &fingerprint_end, 16);
    if (errno == ERANGE || !fingerprint_end ||
        fingerprint_end != fingerprint_text + 16 ||
        *fingerprint_end != '|') return NULL;
    const char *count_text = fingerprint_end + 1;
    char *count_end = NULL;
    errno = 0;
    long count = strtol(count_text, &count_end, 10);
    if (errno == ERANGE || !count_end || count_end == count_text ||
        *count_end != '|' || count < 0 || count > 4096)
        return NULL;
    const char *payload = count_end + 1;
    if (!*payload || scheme_text_fingerprint(payload) != (uint64_t)fingerprint)
        return NULL;
    char *owned_payload = v2 ? strdup(payload) : NULL;
    char *body = v2 ? owned_payload : (char *)payload;
    char *predicate_count_text = NULL, *predicate_text = NULL;
    if (v2) {
        char *separator = body ? strchr(body, '|') : NULL;
        if (!separator) { free(owned_payload); return NULL; }
        *separator = '\0'; predicate_count_text = separator + 1;
        separator = strchr(predicate_count_text, '|');
        if (!separator) { free(owned_payload); return NULL; }
        *separator = '\0'; predicate_text = separator + 1;
    }
    Type *type = qtt_type_deserialize(body);
    TypeScheme *scheme = type ? scheme_mono(type) : NULL;
    if (!scheme) {
        qtt_type_free_owned(type);
        free(owned_payload); return NULL;
    }
    scheme->owns_type = true;
    scheme->quantified_count = (int)count;
    scheme->quantified = count
        ? malloc((size_t)count * sizeof(*scheme->quantified)) : NULL;
    if (count && !scheme->quantified) {
        scheme_free(scheme);
        return NULL;
    }
    for (int i = 0; i < scheme->quantified_count; i++)
        scheme->quantified[i] = i;
    if (v2) {
        char *end = NULL; errno = 0;
        unsigned long declared_count = strtoul(predicate_count_text, &end, 10);
        if (errno || !end || *end || declared_count > 4096) {
            free(owned_payload); scheme_free(scheme); return NULL;
        }
        size_t *stages = declared_count ? malloc(declared_count * sizeof(*stages)) : NULL;
        const char **traits = declared_count ? malloc(declared_count * sizeof(*traits)) : NULL;
        size_t parsed = 0;
        for (char *token = predicate_text; token && *token;) {
            char *next = strchr(token, ','); if (next) *next++ = '\0';
            char *colon = strchr(token, ':');
            if (!colon || parsed >= declared_count) break;
            *colon = '\0'; errno = 0; char *stage_end = NULL;
            unsigned long stage = strtoul(token, &stage_end, 10);
            if (errno || !stage_end || *stage_end) break;
            stages[parsed] = stage; traits[parsed] = colon + 1; parsed++;
            token = next;
        }
        bool predicates_ok = parsed == declared_count &&
            scheme_set_effect_trait_predicates(scheme, stages, traits, parsed);
        free(stages); free(traits);
        if (!predicates_ok) {
            free(owned_payload); scheme_free(scheme); return NULL;
        }
    }
    free(owned_payload);
    TypeScheme canonical_view = *scheme;
    if (!scheme_canonicalize_type(scheme->type, &canonical_view, 0)) {
        scheme_free(scheme);
        return NULL;
    }
    size_t arrow_count = 0;
    bool has_arrow_effects = false;
    for (Type *arrow = scheme->type;
         arrow && arrow->kind == TYPE_ARROW; arrow = arrow->arrow_ret) {
        arrow_count++;
        has_arrow_effects = has_arrow_effects ||
            arrow->arrow_effect_scheme != NULL;
    }
    if (has_arrow_effects) {
        QttEffectScheme **effects = calloc(
            arrow_count, sizeof(*effects));
        bool *complete = malloc(arrow_count * sizeof(*complete));
        bool valid = effects && complete;
        Type *arrow = scheme->type;
        for (size_t i = 0; valid && i < arrow_count; i++) {
            valid = arrow->arrow_effect_scheme != NULL;
            if (valid) {
                effects[i] = qtt_effect_scheme_deserialize(
                    arrow->arrow_effect_scheme);
                complete[i] = arrow->arrow_effect_complete;
                valid = effects[i] != NULL;
            }
            arrow = arrow->arrow_ret;
        }
        if (valid)
            valid = scheme_set_arrow_effect_schemes(
                scheme, effects, complete, arrow_count);
        for (size_t i = 0; i < arrow_count; i++)
            qtt_effect_scheme_free(effects ? effects[i] : NULL);
        free(effects);
        free(complete);
        if (!valid) {
            scheme_free(scheme);
            return NULL;
        }
    }
    return scheme;
}

char *infer_operation_scheme_serialize(
    const char *payload_name, const char *result_name) {
    Type *payload = type_from_name(payload_name);
    Type *result = type_from_name(result_name);
    Type *arrow = payload && result ? type_arrow(payload, result) : NULL;
    if (!arrow) return NULL;
    int variables[INFER_MAX_VARS];
    int count = 0;
    /* Collect the implicit forall binders without coupling canonicalization
     * to the caller's live inference substitution. */
    Substitution *substitution = subst_create();
    if (substitution)
        infer_free_vars_type(substitution, arrow, variables, &count,
                             INFER_MAX_VARS);
    TypeScheme scheme = {
        .type = arrow,
        .quantified = variables,
        .quantified_count = count,
    };
    char *portable = substitution
        ? infer_type_scheme_serialize(&scheme) : NULL;
    subst_free(substitution);
    type_free(arrow);
    return portable;
}

bool infer_operation_scheme_instantiate(
    InferCtx *ctx, const char *portable_scheme,
    Type **payload_type, Type **result_type) {
    if (!ctx || !portable_scheme || !payload_type || !result_type)
        return false;
    *payload_type = NULL;
    *result_type = NULL;
    TypeScheme *scheme = infer_type_scheme_deserialize(portable_scheme);
    Type *instance = scheme ? infer_instantiate(ctx, scheme) : NULL;
    bool ok = instance && instance->kind == TYPE_ARROW &&
        instance->arrow_param && instance->arrow_ret;
    if (ok) {
        *payload_type = instance->arrow_param;
        *result_type = instance->arrow_ret;
    }
    scheme_free(scheme);
    return ok;
}

bool infer_operation_scheme_accepts(
    const char *portable_scheme,
    Type *payload_type, Type *result_type) {
    if (!portable_scheme || !payload_type || !result_type) return false;
    InferCtx *ctx = infer_ctx_create(NULL, NULL, "<operation-scheme>");
    Type *expected_payload = NULL, *expected_result = NULL;
    bool ok = ctx && infer_operation_scheme_instantiate(
        ctx, portable_scheme, &expected_payload, &expected_result);
    if (ok)
        ok = infer_unify_one(ctx, expected_payload, payload_type, 0, 0) &&
             infer_unify_one(ctx, expected_result, result_type, 0, 0);
    infer_ctx_free(ctx);
    return ok;
}

void scheme_set_grade_scheme(
    TypeScheme *scheme, QttGradeScheme *grades) {
    if (!scheme || scheme->grade_scheme == grades) return;
    qtt_grade_scheme_free(scheme->grade_scheme);
    scheme->grade_scheme = qtt_grade_scheme_retain(grades);
}

void scheme_set_effect_scheme(
    TypeScheme *scheme, QttEffectScheme *effects, bool complete) {
    if (!scheme) return;
    if (scheme->effect_scheme != effects) {
        qtt_effect_scheme_free(scheme->effect_scheme);
        scheme->effect_scheme = qtt_effect_scheme_retain(effects);
    }
    scheme->effects_complete = complete;
}

bool scheme_set_arrow_effect_schemes(
    TypeScheme *scheme, QttEffectScheme *const *effects,
    const bool *complete, size_t count) {
    if (!scheme || (count && (!effects || !complete))) return false;
    size_t arrow_depth = 0;
    for (Type *cursor = scheme->type;
         cursor && cursor->kind == TYPE_ARROW; cursor = cursor->arrow_ret)
        arrow_depth++;
    bool structural = count && arrow_depth == count;
    if (structural && !scheme->owns_type) {
        Type *owned_type = type_clone(scheme->type);
        if (!owned_type) return false;
        scheme->type = owned_type;
        scheme->owns_type = true;
    }
    char **portable = structural
        ? calloc(count, sizeof(*portable)) : NULL;
    if (structural && !portable) return false;
    if (structural) {
        for (size_t i = 0; i < count; i++) {
            portable[i] = effects[i]
                ? qtt_effect_scheme_serialize(effects[i]) : NULL;
            if (!portable[i]) {
                for (size_t j = 0; j < i; j++) free(portable[j]);
                free(portable);
                return false;
            }
        }
    }
    QttEffectScheme **owned = count
        ? calloc(count, sizeof(*owned)) : NULL;
    bool *owned_complete = count
        ? malloc(count * sizeof(*owned_complete)) : NULL;
    if (count && (!owned || !owned_complete)) {
        for (size_t i = 0; portable && i < count; i++) free(portable[i]);
        free(portable);
        free(owned);
        free(owned_complete);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (!effects[i]) {
            for (size_t j = 0; j < i; j++)
                qtt_effect_scheme_free(owned[j]);
            free(owned);
            free(owned_complete);
            for (size_t j = 0; portable && j < count; j++) free(portable[j]);
            free(portable);
            return false;
        }
        owned[i] = qtt_effect_scheme_retain(effects[i]);
        owned_complete[i] = complete[i];
    }
    for (size_t i = 0; i < scheme->arrow_effect_count; i++)
        qtt_effect_scheme_free(scheme->arrow_effect_schemes[i]);
    free(scheme->arrow_effect_schemes);
    free(scheme->arrow_effects_complete);
    scheme->arrow_effect_schemes = owned;
    scheme->arrow_effects_complete = owned_complete;
    scheme->arrow_effect_count = count;
    if (structural) {
        Type *arrow = scheme->type;
        for (size_t i = 0; i < count; i++) {
            if (arrow->arrow_effect_scheme_owned)
                free(arrow->arrow_effect_scheme);
            arrow->arrow_effect_scheme = portable[i];
            arrow->arrow_effect_scheme_owned = true;
            arrow->arrow_effect_complete = complete[i];
            arrow = arrow->arrow_ret;
        }
    }
    free(portable);
    return true;
}

void infer_callable_contract_free(InferCallableContract *contract) {
    if (!contract) return;
    qtt_effect_scheme_free(contract->effect_scheme);
    for (size_t i = 0; i < contract->arrow_effect_count; i++)
        qtt_effect_scheme_free(contract->arrow_effect_schemes[i]);
    free(contract->arrow_effect_schemes);
    free(contract->arrow_effects_complete);
    for (size_t i = 0; i < contract->effect_trait_predicate_count; i++)
        free(contract->effect_trait_predicate_names[i]);
    free(contract->effect_trait_predicate_names);
    free(contract->effect_trait_predicate_stages);
    memset(contract, 0, sizeof(*contract));
}

static bool infer_callable_contract_copy(
    InferCallableContract *contract, QttEffectScheme *effect_scheme,
    bool effects_complete, QttEffectScheme *const *arrow_effects,
    const bool *arrow_complete, size_t arrow_count,
    const size_t *predicate_stages, char *const *predicate_names,
    size_t predicate_count) {
    if (!contract || !effect_scheme ||
            (arrow_count && (!arrow_effects || !arrow_complete)) ||
            (predicate_count && (!predicate_stages || !predicate_names)))
        return false;
    InferCallableContract copied = {
        .effect_scheme = qtt_effect_scheme_retain(effect_scheme),
        .effects_complete = effects_complete,
        .arrow_effect_count = arrow_count,
    };
    copied.arrow_effect_schemes = arrow_count
        ? calloc(arrow_count, sizeof(*copied.arrow_effect_schemes)) : NULL;
    copied.arrow_effects_complete = arrow_count
        ? malloc(arrow_count * sizeof(*copied.arrow_effects_complete)) : NULL;
    copied.effect_trait_predicate_stages = predicate_count
        ? malloc(predicate_count *
            sizeof(*copied.effect_trait_predicate_stages)) : NULL;
    copied.effect_trait_predicate_names = predicate_count
        ? calloc(predicate_count,
            sizeof(*copied.effect_trait_predicate_names)) : NULL;
    if (arrow_count &&
            (!copied.arrow_effect_schemes ||
             !copied.arrow_effects_complete)) {
        infer_callable_contract_free(&copied);
        return false;
    }
    if (predicate_count &&
            (!copied.effect_trait_predicate_stages ||
             !copied.effect_trait_predicate_names)) {
        infer_callable_contract_free(&copied);
        return false;
    }
    for (size_t i = 0; i < arrow_count; i++) {
        if (!arrow_effects[i]) {
            infer_callable_contract_free(&copied);
            return false;
        }
        copied.arrow_effect_schemes[i] =
            qtt_effect_scheme_retain(arrow_effects[i]);
        copied.arrow_effects_complete[i] = arrow_complete[i];
    }
    for (size_t i = 0; i < predicate_count; i++) {
        if (predicate_stages[i] >= arrow_count || !predicate_names[i]) {
            infer_callable_contract_free(&copied);
            return false;
        }
        copied.effect_trait_predicate_stages[i] = predicate_stages[i];
        copied.effect_trait_predicate_names[i] = strdup(predicate_names[i]);
        copied.effect_trait_predicate_count++;
        if (!copied.effect_trait_predicate_names[i]) {
            infer_callable_contract_free(&copied);
            return false;
        }
    }
    infer_callable_contract_free(contract);
    *contract = copied;
    return true;
}

bool infer_callable_contract_from_judgment(
    InferCallableContract *contract,
    const InferExpressionJudgment *judgment) {
    return judgment && infer_callable_contract_copy(
        contract, judgment->effects, judgment->effects_complete,
        judgment->arrow_effect_schemes,
        judgment->arrow_effects_complete,
        judgment->arrow_effect_count,
        judgment->effect_trait_predicate_stages,
        judgment->effect_trait_predicate_names,
        judgment->effect_trait_predicate_count);
}

bool infer_callable_contract_from_scheme(
    InferCallableContract *contract, const TypeScheme *scheme) {
    return scheme && infer_callable_contract_copy(
        contract, scheme->effect_scheme, scheme->effects_complete,
        scheme->arrow_effect_schemes, scheme->arrow_effects_complete,
        scheme->arrow_effect_count,
        scheme->effect_trait_predicate_stages,
        scheme->effect_trait_predicate_names,
        scheme->effect_trait_predicate_count);
}

bool scheme_set_callable_contract(
    TypeScheme *scheme, const InferCallableContract *contract) {
    if (!scheme || !contract || !contract->effect_scheme) return false;
    if (!scheme_set_arrow_effect_schemes(
            scheme, contract->arrow_effect_schemes,
            contract->arrow_effects_complete,
            contract->arrow_effect_count))
        return false;
    scheme_set_effect_scheme(
        scheme, contract->effect_scheme, contract->effects_complete);
    return scheme_set_effect_trait_predicates(
        scheme, contract->effect_trait_predicate_stages,
        (const char *const *)contract->effect_trait_predicate_names,
        contract->effect_trait_predicate_count);
}

uint64_t infer_callable_contract_fingerprint(
    const InferCallableContract *contract) {
    if (!contract) return 0;
    uint64_t hash = contract->effect_scheme
        ? qtt_effect_scheme_fingerprint(contract->effect_scheme) : 0;
    hash ^= contract->effects_complete
        ? UINT64_C(0xc3c3c3c3c3c3c3c3)
        : UINT64_C(0x3c3c3c3c3c3c3c3c);
    for (size_t i = 0; i < contract->arrow_effect_count; i++) {
        uint64_t part = qtt_effect_scheme_fingerprint(
            contract->arrow_effect_schemes[i]);
        hash ^= part + UINT64_C(0x9e3779b97f4a7c15) +
                (hash << 6) + (hash >> 2);
        hash ^= contract->arrow_effects_complete[i]
            ? UINT64_C(0xa5a5a5a5a5a5a5a5)
            : UINT64_C(0x5a5a5a5a5a5a5a5a);
    }
    for (size_t i = 0; i < contract->effect_trait_predicate_count; i++) {
        hash ^= contract->effect_trait_predicate_stages[i] +
            UINT64_C(0x517cc1b727220a95) + (hash << 6) + (hash >> 2);
        const unsigned char *at = (const unsigned char *)
            contract->effect_trait_predicate_names[i];
        while (at && *at) {
            hash ^= *at++;
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

static char infer_contract_hex_digit(unsigned value) {
    return "0123456789abcdef"[value & 15u];
}

static char *infer_contract_hex_encode(const char *input) {
    size_t length = strlen(input);
    char *output = malloc(length * 2 + 1);
    if (!output) return NULL;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)input[i];
        output[i * 2] = infer_contract_hex_digit(byte >> 4);
        output[i * 2 + 1] = infer_contract_hex_digit(byte);
    }
    output[length * 2] = '\0';
    return output;
}

static int infer_contract_hex_value(char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

static char *infer_contract_hex_decode(const char *input) {
    size_t length = strlen(input);
    if (length & 1u) return NULL;
    char *output = malloc(length / 2 + 1);
    if (!output) return NULL;
    for (size_t i = 0; i < length; i += 2) {
        int high = infer_contract_hex_value(input[i]);
        int low = infer_contract_hex_value(input[i + 1]);
        if (high < 0 || low < 0) {
            free(output);
            return NULL;
        }
        output[i / 2] = (char)((high << 4) | low);
    }
    output[length / 2] = '\0';
    return output;
}

char *infer_callable_contract_serialize(
    const InferCallableContract *contract) {
    if (!contract || !contract->effect_scheme) return NULL;
    char *effect_text = qtt_effect_scheme_serialize(contract->effect_scheme);
    char *effect_hex = effect_text
        ? infer_contract_hex_encode(effect_text) : NULL;
    free(effect_text);
    if (!effect_hex) return NULL;
    char **stage_hex = contract->arrow_effect_count
        ? calloc(contract->arrow_effect_count, sizeof(*stage_hex)) : NULL;
    size_t capacity = 128 + strlen(effect_hex);
    bool valid = !contract->arrow_effect_count || stage_hex;
    for (size_t i = 0; valid && i < contract->arrow_effect_count; i++) {
        char *stage_text = qtt_effect_scheme_serialize(
            contract->arrow_effect_schemes[i]);
        stage_hex[i] = stage_text
            ? infer_contract_hex_encode(stage_text) : NULL;
        free(stage_text);
        valid = stage_hex[i] != NULL;
        if (valid) capacity += strlen(stage_hex[i]) + 4;
    }
    for (size_t i = 0; i < contract->effect_trait_predicate_count; i++)
        capacity += strlen(contract->effect_trait_predicate_names[i]) + 32;
    char *text = valid ? malloc(capacity) : NULL;
    if (text) {
        size_t used = (size_t)snprintf(
            text, capacity, "monad-callable-contract-%s|%016llx|%u|%s|",
            contract->effect_trait_predicate_count ? "v2" : "v1",
            (unsigned long long)
                infer_callable_contract_fingerprint(contract),
            contract->effects_complete ? 1u : 0u, effect_hex);
        for (size_t i = 0; i < contract->arrow_effect_count; i++)
            used += (size_t)snprintf(
                text + used, capacity - used, "%s%u:%s",
                i ? "," : "",
                contract->arrow_effects_complete[i] ? 1u : 0u,
                stage_hex[i]);
        if (contract->effect_trait_predicate_count) {
            used += (size_t)snprintf(text + used, capacity - used, "|");
            for (size_t i = 0;
                 i < contract->effect_trait_predicate_count; i++)
                used += (size_t)snprintf(
                    text + used, capacity - used, "%s%zu:%s",
                    i ? "," : "",
                    contract->effect_trait_predicate_stages[i],
                    contract->effect_trait_predicate_names[i]);
        }
    }
    for (size_t i = 0; i < contract->arrow_effect_count; i++)
        free(stage_hex ? stage_hex[i] : NULL);
    free(stage_hex);
    free(effect_hex);
    return text;
}

bool infer_callable_contract_deserialize(
    InferCallableContract *contract, const char *text) {
    static const char header_v1[] = "monad-callable-contract-v1|";
    static const char header_v2[] = "monad-callable-contract-v2|";
    if (!contract || !text) return false;
    bool version2 = strncmp(
        text, header_v2, sizeof(header_v2) - 1) == 0;
    const char *payload = version2 ? text + sizeof(header_v2) - 1
        : strncmp(text, header_v1, sizeof(header_v1) - 1) == 0
            ? text + sizeof(header_v1) - 1 : NULL;
    if (!payload)
        return false;
    char *copy = malloc(strlen(payload) + 1);
    if (!copy) return false;
    strcpy(copy, payload);
    char *fields[4] = {copy, NULL, NULL, NULL};
    bool valid = true;
    for (size_t i = 0; i < 3; i++) {
        char *separator = strchr(fields[i], '|');
        if (!separator) { valid = false; break; }
        *separator = '\0';
        fields[i + 1] = separator + 1;
    }
    unsigned long long declared = 0;
    char extra = '\0';
    if (!valid || sscanf(fields[0], "%llx%c", &declared, &extra) != 1 ||
            (strcmp(fields[1], "0") != 0 && strcmp(fields[1], "1") != 0)) {
        free(copy);
        return false;
    }
    char *effect_text = infer_contract_hex_decode(fields[2]);
    InferCallableContract decoded = {0};
    decoded.effect_scheme = effect_text
        ? qtt_effect_scheme_deserialize(effect_text) : NULL;
    decoded.effects_complete = fields[1][0] == '1';
    free(effect_text);
    valid = decoded.effect_scheme != NULL;
    char *predicate_text = NULL;
    if (version2) {
        predicate_text = strchr(fields[3], '|');
        if (predicate_text) *predicate_text++ = '\0';
        else valid = false;
    }
    for (char *token = valid && *fields[3] ? fields[3] : NULL;
         token && valid;) {
        char *next = strchr(token, ',');
        if (next) *next++ = '\0';
        char *colon = strchr(token, ':');
        if (!colon || colon == token || colon[1] == '\0') {
            valid = false;
            break;
        }
        *colon = '\0';
        if ((strcmp(token, "0") != 0 && strcmp(token, "1") != 0)) {
            valid = false;
            break;
        }
        char *stage_text = infer_contract_hex_decode(colon + 1);
        QttEffectScheme *stage = stage_text
            ? qtt_effect_scheme_deserialize(stage_text) : NULL;
        free(stage_text);
        if (!stage) { valid = false; break; }
        size_t count = decoded.arrow_effect_count;
        QttEffectScheme **grown_stages = malloc(
            (count + 1) * sizeof(*grown_stages));
        bool *grown_complete = malloc(
            (count + 1) * sizeof(*grown_complete));
        if (!grown_stages || !grown_complete) {
            qtt_effect_scheme_free(stage);
            free(grown_stages);
            free(grown_complete);
            valid = false;
            break;
        }
        if (count) {
            memcpy(grown_stages, decoded.arrow_effect_schemes,
                   count * sizeof(*grown_stages));
            memcpy(grown_complete, decoded.arrow_effects_complete,
                   count * sizeof(*grown_complete));
        }
        free(decoded.arrow_effect_schemes);
        free(decoded.arrow_effects_complete);
        decoded.arrow_effect_schemes = grown_stages;
        decoded.arrow_effects_complete = grown_complete;
        decoded.arrow_effect_schemes[count] = stage;
        decoded.arrow_effects_complete[count] = token[0] == '1';
        decoded.arrow_effect_count++;
        token = next;
    }
    if (version2) {
        for (char *token = valid && predicate_text && *predicate_text
                 ? predicate_text : NULL;
             token && valid;) {
            char *next = strchr(token, ',');
            if (next) *next++ = '\0';
            char *colon = strchr(token, ':');
            unsigned long stage = 0;
            char extra_stage = '\0';
            if (!colon || colon == token || colon[1] == '\0') {
                valid = false; break;
            }
            *colon = '\0';
            if (sscanf(token, "%lu%c", &stage, &extra_stage) != 1 ||
                    stage >= decoded.arrow_effect_count) {
                valid = false; break;
            }
            size_t count = decoded.effect_trait_predicate_count;
            size_t *grown_stages = realloc(
                decoded.effect_trait_predicate_stages,
                (count + 1) * sizeof(*grown_stages));
            if (!grown_stages) { valid = false; break; }
            decoded.effect_trait_predicate_stages = grown_stages;
            char **grown_names = realloc(
                decoded.effect_trait_predicate_names,
                (count + 1) * sizeof(*grown_names));
            if (!grown_names) { valid = false; break; }
            decoded.effect_trait_predicate_names = grown_names;
            decoded.effect_trait_predicate_stages[count] = (size_t)stage;
            decoded.effect_trait_predicate_names[count] = strdup(colon + 1);
            if (!decoded.effect_trait_predicate_names[count]) {
                valid = false; break;
            }
            decoded.effect_trait_predicate_count++;
            token = next;
        }
    }
    if (!valid || infer_callable_contract_fingerprint(&decoded) !=
            (uint64_t)declared) {
        infer_callable_contract_free(&decoded);
        free(copy);
        return false;
    }
    infer_callable_contract_free(contract);
    *contract = decoded;
    free(copy);
    return true;
}

uint64_t scheme_effect_fingerprint(const TypeScheme *scheme) {
    if (!scheme) return 0;
    InferCallableContract view = {
        .effect_scheme = scheme->effect_scheme,
        .effects_complete = scheme->effects_complete,
        .arrow_effect_schemes = scheme->arrow_effect_schemes,
        .arrow_effects_complete = scheme->arrow_effects_complete,
        .arrow_effect_count = scheme->arrow_effect_count,
    };
    return infer_callable_contract_fingerprint(&view);
}

typedef struct {
    InferCtx *ctx;
    QttEffectConstraintSet *constraints;
    uint32_t coverage_gaps;
    bool append_failed;
    TypeScheme *lambda_parameters[64];
    size_t lambda_parameter_count;
    uint64_t callable_parameter_mask;
    size_t callable_parameter_arities[64];
    QttEffectScheme **returned_arrow_effects;
    QttEffectRow **returned_arrow_rows;
    char **returned_arrow_effect_names;
    bool *returned_arrow_effects_complete;
    size_t returned_arrow_effect_count;
    size_t *returned_effect_trait_predicate_stages;
    char **returned_effect_trait_predicate_names;
    size_t returned_effect_trait_predicate_count;
} SourceEffectJudgment;

static void infer_source_effect_clear_returned(
    SourceEffectJudgment *judgment) {
    for (size_t i = 0; i < judgment->returned_arrow_effect_count; i++)
        qtt_effect_scheme_free(judgment->returned_arrow_effects[i]);
    free(judgment->returned_arrow_effects);
    free(judgment->returned_arrow_rows);
    if (judgment->returned_arrow_effect_names)
        for (size_t i = 0; i < judgment->returned_arrow_effect_count; i++)
            free(judgment->returned_arrow_effect_names[i]);
    free(judgment->returned_arrow_effect_names);
    free(judgment->returned_arrow_effects_complete);
    for (size_t i = 0;
         i < judgment->returned_effect_trait_predicate_count; i++)
        free(judgment->returned_effect_trait_predicate_names[i]);
    free(judgment->returned_effect_trait_predicate_names);
    free(judgment->returned_effect_trait_predicate_stages);
    judgment->returned_arrow_effects = NULL;
    judgment->returned_arrow_rows = NULL;
    judgment->returned_arrow_effect_names = NULL;
    judgment->returned_arrow_effects_complete = NULL;
    judgment->returned_arrow_effect_count = 0;
    judgment->returned_effect_trait_predicate_names = NULL;
    judgment->returned_effect_trait_predicate_stages = NULL;
    judgment->returned_effect_trait_predicate_count = 0;
}

static QttEffectRow *infer_source_effect_join(
    SourceEffectJudgment *judgment,
    QttEffectRow *left, QttEffectRow *right) {
    if (!left || !right) return NULL;
    InferCtx *ctx = judgment->ctx;
    if (!qtt_effect_is_closed(ctx->effect_solver, left) ||
        !qtt_effect_is_closed(ctx->effect_solver, right))
        judgment->coverage_gaps |= QTT_EFFECT_COVERAGE_OPEN_ROW;
    QttEffectRow *joined = qtt_effect_join(
        ctx->effect_arena, ctx->effect_solver, left, right);
    if (joined &&
        !qtt_effect_constrain_join(
            judgment->constraints, left, right, joined))
        judgment->append_failed = true;
    return joined;
}

/* Instantiate qualified scheme predicates at the same application boundary
 * as their arrow row.  Re-instantiating the scheme independently here would
 * create a different open tail and prove a proposition about the wrong row. */
static bool infer_source_constrain_stage_traits(
    SourceEffectJudgment *judgment, const TypeScheme *scheme,
    size_t stage, QttEffectRow *row) {
    if (!judgment || !scheme || !row) return false;
    for (size_t i = 0; i < scheme->effect_trait_predicate_count; i++) {
        if (scheme->effect_trait_predicate_stages[i] != stage) continue;
        if (!qtt_effect_constrain_has_trait(
                judgment->constraints, row,
                scheme->effect_trait_predicate_names[i])) {
            judgment->append_failed = true;
            return false;
        }
    }
    return true;
}

/* Arrow effect names are lexical row binders.  Every occurrence of one name
 * in a single instantiated application denotes the same row, so later
 * occurrences contribute equality obligations against the exact earlier
 * stage instances (not independently re-instantiated copies). */
static bool infer_source_constrain_named_stage(
    SourceEffectJudgment *judgment, const char *name, QttEffectRow *row,
    const char *const *prior_names, QttEffectRow *const *prior_rows,
    size_t prior_count) {
    if (!judgment || !row) return false;
    if (!name) return true;
    for (size_t i = 0; i < prior_count; i++) {
        if (!prior_names[i] || strcmp(name, prior_names[i]) != 0) continue;
        if (!qtt_effect_constrain_equal(
                judgment->constraints, prior_rows[i], row)) {
            judgment->append_failed = true;
            return false;
        }
        /* Equality is transitive: one representative gives a minimal
         * spanning basis instead of a quadratic clique of obligations. */
        return true;
    }
    return true;
}

/* Xi classifies a compact arrow label from Core-owned declarations. Exact
 * names win; a unique trait.operation projection is accepted as shorthand.
 * Unresolved simple identifiers are implicit lexical row binders. Dotted
 * references are reserved for declarations and fail closed when absent or
 * ambiguous. */
static bool infer_source_constrain_stage_label(
    SourceEffectJudgment *judgment, const char *name, QttEffectRow *row,
    const char *const *prior_names, QttEffectRow *const *prior_rows,
    size_t prior_count) {
    if (!judgment || !row) return false;
    bool ambiguous = false;
    const QttEffectDeclaration *declaration =
        qtt_effect_declaration_resolve(name, &ambiguous);
    if (declaration) {
        InferCtx *ctx = judgment->ctx;
        QttEffectRow *declared = qtt_effect_extend_declared(
            ctx->effect_arena, declaration->name, 0, 0,
            qtt_effect_empty(ctx->effect_arena));
        if (!declared || !qtt_effect_constrain_equal(
                judgment->constraints, row, declared)) {
            judgment->append_failed = true;
            return false;
        }
    } else if (ambiguous || (name && strchr(name, '.'))) {
        /* Foundational effects may be validated before Core.Effect has
         * populated the nominal declaration registry. Their established
         * dotted surface names still denote the same bootstrap atoms. */
        if (name && strcmp(name, "state.write") == 0) {
            InferCtx *ctx = judgment->ctx;
            QttEffectRow *declared = qtt_effect_extend(
                ctx->effect_arena, name,
                qtt_effect_empty(ctx->effect_arena));
            if (!declared || !qtt_effect_constrain_equal(
                    judgment->constraints, row, declared)) {
                judgment->append_failed = true;
                return false;
            }
        } else {
            judgment->append_failed = true;
            return false;
        }
    }
    return infer_source_constrain_named_stage(
        judgment, name, row, prior_names, prior_rows, prior_count);
}

bool infer_validate_effect_annotations(
    InferCtx *ctx, const TypeScheme *scheme,
    size_t *failing_stage, const char **failing_label) {
    if (failing_stage) *failing_stage = SIZE_MAX;
    if (failing_label) *failing_label = NULL;
    if (!ctx || !scheme) return false;
    size_t count = infer_scheme_arrow_effect_count(scheme);
    if (!count) return true;
    QttEffectConstraintSet *constraints = qtt_effect_constraints_new();
    const char **names = calloc(count, sizeof(*names));
    QttEffectRow **rows = calloc(count, sizeof(*rows));
    if (!constraints || !names || !rows) {
        qtt_effect_constraints_free(constraints);
        free(names);
        free(rows);
        return false;
    }
    SourceEffectJudgment judgment = {
        .ctx = ctx, .constraints = constraints,
    };
    bool valid = true;
    for (size_t i = 0; valid && i < count; i++) {
        bool complete = false;
        QttEffectScheme *stage_scheme = infer_scheme_arrow_effect_at(
            scheme, i, &complete, NULL);
        QttEffectRow *row = stage_scheme
            ? qtt_effect_instantiate(ctx->effect_arena, stage_scheme) : NULL;
        qtt_effect_scheme_free(stage_scheme);
        names[i] = infer_scheme_arrow_effect_name_at(scheme, i);
        rows[i] = row;
        valid = row &&
            infer_source_constrain_stage_traits(
                &judgment, scheme, i, row) &&
            infer_source_constrain_stage_label(
                &judgment, names[i], row, names, rows, i);
        if (!valid) {
            if (failing_stage) *failing_stage = i;
            if (failing_label) *failing_label = names[i];
        }
    }
    QttEffectConstraintCertificate *certificate = NULL;
    QttEffectConstraintResult solved = valid
        ? qtt_effect_constraints_solve(
            constraints, ctx->effect_arena, ctx->effect_solver,
            &certificate)
        : QTT_EFFECT_CONSTRAINT_REJECTED;
    valid = valid && certificate &&
        solved != QTT_EFFECT_CONSTRAINT_REJECTED &&
        solved != QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY &&
        qtt_effect_certificate_verify(
            certificate, ctx->effect_arena, ctx->effect_solver);
    if (!valid && failing_stage && *failing_stage == SIZE_MAX) {
        for (size_t i = 0; i < count; i++)
            if (names[i]) {
                *failing_stage = i;
                if (failing_label) *failing_label = names[i];
                break;
            }
    }
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(constraints);
    free(names);
    free(rows);
    return valid;
}

bool infer_elaborate_effect_row_binders(
    InferCtx *ctx, TypeScheme *scheme) {
    if (!ctx || !scheme) return false;
    size_t count = infer_scheme_arrow_effect_count(scheme);
    if (!count) return true;
    QttEffectScheme **stages = calloc(count, sizeof(*stages));
    bool *complete = malloc(count * sizeof(*complete));
    const char **names = calloc(count, sizeof(*names));
    QttEffectRow **binders = calloc(count, sizeof(*binders));
    size_t **dependency_indices = calloc(
        count, sizeof(*dependency_indices));
    size_t **dependency_arities = calloc(
        count, sizeof(*dependency_arities));
    size_t *dependency_counts = calloc(
        count, sizeof(*dependency_counts));
    QttEffectConstraintSet *constraints = qtt_effect_constraints_new();
    if (!stages || !complete || !names || !binders ||
            !dependency_indices || !dependency_arities ||
            !dependency_counts || !constraints) {
        free(stages); free(complete); free(names); free(binders);
        free(dependency_indices); free(dependency_arities);
        free(dependency_counts);
        qtt_effect_constraints_free(constraints);
        return false;
    }
    bool valid = true;
    bool has_lexical = false;
    for (size_t i = 0; valid && i < count; i++) {
        bool stage_complete = false;
        QttEffectScheme *source = infer_scheme_arrow_effect_at(
            scheme, i, &stage_complete, NULL);
        names[i] = infer_scheme_arrow_effect_name_at(scheme, i);
        bool ambiguous = false;
        const QttEffectDeclaration *declaration =
            qtt_effect_declaration_resolve(names[i], &ambiguous);
        bool lexical = names[i] && !declaration && !ambiguous &&
            !strchr(names[i], '.');
        if (!source) {
            qtt_effect_scheme_free(source);
            valid = false;
            break;
        }
        if (!lexical) {
            stages[i] = source;
            complete[i] = stage_complete;
            continue;
        }
        has_lexical = true;
        dependency_counts[i] =
            qtt_effect_scheme_callable_parameter_count(source);
        if (dependency_counts[i]) {
            dependency_indices[i] = malloc(
                dependency_counts[i] *
                    sizeof(*dependency_indices[i]));
            dependency_arities[i] = malloc(
                dependency_counts[i] *
                    sizeof(*dependency_arities[i]));
            if (!dependency_indices[i] || !dependency_arities[i]) {
                qtt_effect_scheme_free(source);
                valid = false;
                break;
            }
            for (size_t j = 0; j < dependency_counts[i]; j++) {
                dependency_indices[i][j] =
                    qtt_effect_scheme_callable_parameter_index(source, j);
                dependency_arities[i][j] =
                    qtt_effect_scheme_callable_parameter_arity(source, j);
            }
        }
        QttEffectRow *implementation = qtt_effect_instantiate(
            ctx->effect_arena, source);
        QttEffectRow *binder = NULL;
        for (size_t j = 0; j < i; j++)
            if (names[j] && strcmp(names[j], names[i]) == 0) {
                binder = binders[j];
                break;
            }
        if (!binder) binder = qtt_effect_fresh(ctx->effect_arena);
        binders[i] = binder;
        valid = implementation && binder && qtt_effect_constrain_subrow(
            constraints, implementation, binder);
        qtt_effect_scheme_free(source);
        complete[i] = false;
    }
    if (valid && !has_lexical) {
        for (size_t i = 0; i < count; i++)
            qtt_effect_scheme_free(stages[i]);
        qtt_effect_constraints_free(constraints);
        free(stages); free(complete); free(names); free(binders);
        free(dependency_indices); free(dependency_arities);
        free(dependency_counts);
        return true;
    }
    QttEffectConstraintCertificate *certificate = NULL;
    QttEffectConstraintResult solved = valid
        ? qtt_effect_constraints_solve(
            constraints, ctx->effect_arena, ctx->effect_solver,
            &certificate)
        : QTT_EFFECT_CONSTRAINT_REJECTED;
    valid = valid && certificate &&
        solved != QTT_EFFECT_CONSTRAINT_REJECTED &&
        solved != QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY &&
        qtt_effect_certificate_verify(
            certificate, ctx->effect_arena, ctx->effect_solver);
    for (size_t i = 0; valid && i < count; i++) {
        if (!binders[i]) continue;
        stages[i] = qtt_effect_generalize(
            ctx->effect_arena, ctx->effect_solver, binders[i]);
        valid = stages[i] != NULL &&
            (!dependency_counts[i] ||
             qtt_effect_scheme_set_callable_parameter_contracts(
                stages[i], dependency_indices[i], dependency_arities[i],
                dependency_counts[i]));
    }
    if (valid)
        valid = scheme_set_arrow_effect_schemes(
            scheme, stages, complete, count);
    for (size_t i = 0; i < count; i++)
        qtt_effect_scheme_free(stages[i]);
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(constraints);
    free(stages); free(complete); free(names); free(binders);
    for (size_t i = 0; i < count; i++) {
        free(dependency_indices[i]);
        free(dependency_arities[i]);
    }
    free(dependency_indices); free(dependency_arities);
    free(dependency_counts);
    return valid;
}

static QttEffectRow *infer_source_argument_invocation_effect(
    SourceEffectJudgment *judgment, TypeScheme *argument,
    size_t invoked, bool *complete) {
    if (!argument || !invoked || !complete) return NULL;
    InferCtx *ctx = judgment->ctx;
    QttEffectRow *summary = qtt_effect_empty(ctx->effect_arena);
    *complete = true;
    size_t arrow_effect_count = infer_scheme_arrow_effect_count(argument);
    if (arrow_effect_count) {
        if (invoked > arrow_effect_count) return NULL;
        for (size_t stage = 0; summary && stage < invoked; stage++) {
            bool stage_complete = false;
            QttEffectScheme *stage_scheme =
                infer_scheme_arrow_effect_at(
                    argument, stage, &stage_complete, NULL);
            QttEffectRow *effect = qtt_effect_instantiate(
                ctx->effect_arena, stage_scheme);
            qtt_effect_scheme_free(stage_scheme);
            summary = qtt_effect_join(
                ctx->effect_arena, ctx->effect_solver, summary, effect);
            *complete = *complete && stage_complete;
        }
        return summary;
    }
    if (!argument->effect_scheme) return NULL;
    *complete = argument->effects_complete;
    return qtt_effect_instantiate(
        ctx->effect_arena, argument->effect_scheme);
}

static QttEffectScheme *infer_source_specialize_residual_stage(
    SourceEffectJudgment *judgment, QttEffectScheme *stage,
    AST *call, size_t supplied, bool original_complete,
    bool *complete) {
    size_t dependency_count =
        qtt_effect_scheme_callable_parameter_count(stage);
    if (!dependency_count) {
        *complete = original_complete;
        return qtt_effect_scheme_retain(stage);
    }
    InferCtx *ctx = judgment->ctx;
    QttEffectRow *replacement = qtt_effect_empty(ctx->effect_arena);
    size_t remaining_indices[64];
    size_t remaining_arities[64];
    size_t remaining_count = 0;
    bool known_complete = true;
    for (size_t i = 0; replacement && i < dependency_count; i++) {
        size_t parameter =
            qtt_effect_scheme_callable_parameter_index(stage, i);
        size_t arity =
            qtt_effect_scheme_callable_parameter_arity(stage, i);
        if (parameter >= supplied) {
            if (remaining_count == 64) return NULL;
            remaining_indices[remaining_count] = parameter - supplied;
            remaining_arities[remaining_count++] = arity;
            continue;
        }
        AST *argument_ast = call->list.items[parameter + 1];
        TypeScheme *argument = argument_ast->type == AST_SYMBOL
            ? infer_env_lookup(ctx, argument_ast->symbol) : NULL;
        bool argument_complete = false;
        QttEffectRow *argument_effect =
            infer_source_argument_invocation_effect(
                judgment, argument, arity, &argument_complete);
        replacement = qtt_effect_join(
            ctx->effect_arena, ctx->effect_solver,
            replacement, argument_effect);
        known_complete = known_complete && argument_complete;
    }
    if (!replacement) return NULL;
    if (remaining_count)
        replacement = qtt_effect_join(
            ctx->effect_arena, ctx->effect_solver, replacement,
            qtt_effect_fresh(ctx->effect_arena));
    QttEffectRow *row = qtt_effect_instantiate_with_tail(
        ctx->effect_arena, stage, replacement);
    QttEffectScheme *specialized = row
        ? qtt_effect_generalize(ctx->effect_arena, ctx->effect_solver, row)
        : NULL;
    if (specialized &&
            !qtt_effect_scheme_set_callable_parameter_contracts(
                specialized, remaining_indices,
                remaining_arities, remaining_count)) {
        qtt_effect_scheme_free(specialized);
        specialized = NULL;
    }
    *complete = specialized && known_complete && !remaining_count;
    return specialized;
}

static bool infer_source_special_form(const char *name) {
    static const char *forms[] = {
        "if", "with", "let", "begin", "do", "quote", "quasiquote",
        "unquote", "set!", "define", "lambda", "match", "tests",
        "perform", "handle",
    };
    if (!name) return false;
    for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++)
        if (strcmp(name, forms[i]) == 0) return true;
    return false;
}

static QttEffectRow *infer_source_effect_expr(
    SourceEffectJudgment *judgment, const AST *ast) {
    InferCtx *ctx = judgment->ctx;
    QttEffectRow *summary = qtt_effect_empty(ctx->effect_arena);
    if (!summary || !ast) return summary;
    if (ast->type == AST_LAMBDA)
        return summary; /* Closure construction is pure; its body is latent. */
    if (ast->type == AST_ASM) {
        QttEffectRow *declared = qtt_effect_extend_declared(
            ctx->effect_arena, "Core.Foreign.asm", 0, 0, summary);
        return declared ? declared : qtt_effect_extend(
            ctx->effect_arena, "ffi", summary);
    }

    if (ast->type == AST_ARRAY) {
        if (ast->array.is_heap) {
            QttEffectRow *declared = qtt_effect_extend_declared(
                ctx->effect_arena, "Core.Alloc.heap", 0, 0, summary);
            summary = declared ? declared : qtt_effect_extend(
                ctx->effect_arena, "alloc", summary);
        }
        for (size_t i = 0; summary && i < ast->array.element_count; i++) {
            QttEffectRow *item = infer_source_effect_expr(
                judgment, ast->array.elements[i]);
            summary = infer_source_effect_join(judgment, summary, item);
        }
        return summary;
    }
    if (ast->type == AST_SET) {
        for (size_t i = 0; summary && i < ast->set.element_count; i++) {
            QttEffectRow *item = infer_source_effect_expr(
                judgment, ast->set.elements[i]);
            summary = infer_source_effect_join(judgment, summary, item);
        }
        return summary;
    }
    if (ast->type == AST_MAP) {
        for (size_t i = 0; summary && i < ast->map.count; i++) {
            QttEffectRow *key = infer_source_effect_expr(
                judgment, ast->map.keys[i]);
            QttEffectRow *value = infer_source_effect_expr(
                judgment, ast->map.vals[i]);
            summary = infer_source_effect_join(judgment, summary, key);
            summary = infer_source_effect_join(judgment, summary, value);
        }
        return summary;
    }
    if (ast->type != AST_LIST) return summary;

    const char *head = ast->list.count && ast->list.items[0] &&
            ast->list.items[0]->type == AST_SYMBOL
        ? ast->list.items[0]->symbol : NULL;

    /* Constructing a lambda is pure, but fully applying a syntactic lambda
     * executes its body. Guard-clause lowering uses exactly this IIFE shape;
     * treating the head as merely a pure child used to hide state.write from
     * guarded definitions while the equivalent plain clause was rejected. */
    if (ast->list.count > 0 && ast->list.items[0] &&
        ast->list.items[0]->type == AST_LAMBDA) {
        const AST *lambda = ast->list.items[0];
        size_t supplied = ast->list.count - 1;
        for (size_t i = 1; summary && i < ast->list.count; i++) {
            QttEffectRow *argument = infer_source_effect_expr(
                judgment, ast->list.items[i]);
            summary = infer_source_effect_join(judgment, summary, argument);
        }
        if (supplied >= (size_t)lambda->lambda.param_count) {
            for (int i = 0; summary && i < lambda->lambda.body_count; i++) {
                QttEffectRow *body = infer_source_effect_expr(
                    judgment, lambda->lambda.body_exprs[i]);
                summary = infer_source_effect_join(judgment, summary, body);
            }
        }
        return summary;
    }

    if (head && strcmp(head, "perform") == 0) {
        if (ast->list.count != 3 || !ast->list.items[1] ||
            ast->list.items[1]->type != AST_SYMBOL ||
            !ast->list.items[1]->symbol ||
            !qtt_effect_declaration_resolve(
                ast->list.items[1]->symbol, NULL)) {
            judgment->append_failed = true;
            return NULL;
        }
        QttEffectRow *argument = infer_source_effect_expr(
            judgment, ast->list.items[2]);
        return argument ? qtt_effect_extend_declared(
            ctx->effect_arena, ast->list.items[1]->symbol,
            0, 0, argument) : NULL;
    }
    if (head && strcmp(head, "set!") == 0) {
        QttEffectRow *declared = qtt_effect_extend_declared(
            ctx->effect_arena, "Core.State.write", 0, 0, summary);
        summary = declared ? declared : qtt_effect_extend(
            ctx->effect_arena, "state.write", summary);
    }
    if (head && strcmp(head, "__rt_directory_names") == 0) {
        QttEffectRow *declared = qtt_effect_extend_declared(
            ctx->effect_arena, "Core.IO.read", 0, 0, summary);
        summary = declared ? declared : qtt_effect_extend(
            ctx->effect_arena, "io", summary);
    }
    for (size_t i = head ? 1 : 0;
         summary && i < ast->list.count; i++) {
        QttEffectRow *item = infer_source_effect_expr(
            judgment, ast->list.items[i]);
        summary = infer_source_effect_join(judgment, summary, item);
    }
    if (!head || infer_source_special_form(head)) return summary;

    TypeScheme *callee = infer_env_lookup(ctx, head);
    if (!callee) {
        Type *head_type = ast->list.items[0]->inferred_type;
        judgment->coverage_gaps |= head_type &&
                head_type->kind == TYPE_ARROW
            ? QTT_EFFECT_COVERAGE_MISSING_ARROW_CONTRACT
            : QTT_EFFECT_COVERAGE_UNKNOWN_CALLEE;
        return summary;
    }
    size_t domains = 0;
    for (Type *type = callee->type;
         type && type->kind == TYPE_ARROW; type = type->arrow_ret)
        domains++;
    size_t supplied = ast->list.count ? ast->list.count - 1 : 0;
    size_t arrow_effect_count = infer_scheme_arrow_effect_count(callee);
    if (arrow_effect_count) {
        if (arrow_effect_count != domains) {
            judgment->coverage_gaps |= QTT_EFFECT_COVERAGE_ARITY_MISMATCH;
            return summary;
        }
        size_t reached = supplied < domains ? supplied : domains;
        const char **stage_names = reached
            ? calloc(reached, sizeof(*stage_names)) : NULL;
        QttEffectRow **stage_rows = reached
            ? calloc(reached, sizeof(*stage_rows)) : NULL;
        if (reached && (!stage_names || !stage_rows)) {
            free(stage_names);
            free(stage_rows);
            return NULL;
        }
        for (size_t i = 0; i < reached; i++) {
            bool stage_complete = false;
            QttEffectScheme *stage_scheme = infer_scheme_arrow_effect_at(
                callee, i, &stage_complete, NULL);
            if (!stage_scheme) {
                judgment->coverage_gaps |=
                    QTT_EFFECT_COVERAGE_MISSING_ARROW_CONTRACT;
                free(stage_names);
                free(stage_rows);
                return summary;
            }
            if (!stage_complete)
                judgment->coverage_gaps |=
                    QTT_EFFECT_COVERAGE_INCOMPLETE_CALLEE;
            QttEffectRow *stage = qtt_effect_instantiate(
                ctx->effect_arena, stage_scheme);
            qtt_effect_scheme_free(stage_scheme);
            if (!infer_source_constrain_stage_traits(
                    judgment, callee, i, stage) ||
                !infer_source_constrain_stage_label(
                    judgment,
                    infer_scheme_arrow_effect_name_at(callee, i), stage,
                    stage_names, stage_rows, i)) {
                free(stage_names);
                free(stage_rows);
                return NULL;
            }
            stage_names[i] = infer_scheme_arrow_effect_name_at(callee, i);
            stage_rows[i] = stage;
            summary = infer_source_effect_join(judgment, summary, stage);
            if (!summary) {
                free(stage_names);
                free(stage_rows);
                return NULL;
            }
        }
        free(stage_names);
        free(stage_rows);
        return summary;
    }
    if (!callee->effects_complete || supplied < domains) {
        /* Compatibility schemes carry only their saturated projection.
         * They remain conservative under partial application. */
        if (!callee->effects_complete)
            judgment->coverage_gaps |=
                QTT_EFFECT_COVERAGE_INCOMPLETE_CALLEE;
        if (!callee->effects_complete && !callee->effect_scheme &&
            !arrow_effect_count)
            judgment->coverage_gaps |=
                QTT_EFFECT_COVERAGE_MISSING_ARROW_CONTRACT;
        if (supplied < domains)
            judgment->coverage_gaps |=
                QTT_EFFECT_COVERAGE_PARTIAL_LEGACY_CALL;
        return summary;
    }
    QttEffectRow *latent = callee->effect_scheme
        ? qtt_effect_instantiate(ctx->effect_arena, callee->effect_scheme)
        : qtt_effect_empty(ctx->effect_arena);
    return infer_source_effect_join(judgment, summary, latent);
}

static QttEffectScheme *infer_source_effect_finish(
    SourceEffectJudgment *judgment, QttEffectRow *summary,
    bool *complete) {
    InferCtx *ctx = judgment->ctx;
    QttEffectConstraintSet *constraints = judgment->constraints;
    if (!summary || judgment->append_failed) {
        qtt_effect_constraints_free(constraints);
        return NULL;
    }
    QttEffectConstraintCertificate *certificate = NULL;
    QttEffectConstraintResult constraint_result =
        qtt_effect_constraints_solve(
            constraints, ctx->effect_arena, ctx->effect_solver,
            &certificate);
    ctx->last_effect_constraint_count =
        qtt_effect_constraints_count(constraints);
    ctx->last_effect_constraints_residual =
        constraint_result == QTT_EFFECT_CONSTRAINT_RESIDUAL;
    bool constraint_valid = certificate &&
        constraint_result != QTT_EFFECT_CONSTRAINT_REJECTED &&
        constraint_result != QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY &&
        qtt_effect_certificate_verify(
            certificate, ctx->effect_arena, ctx->effect_solver);
    char *portable_certificate = NULL;
    if (constraint_valid) {
        ctx->last_effect_certificate_fingerprint =
            qtt_effect_certificate_fingerprint(
                certificate, ctx->effect_solver);
        portable_certificate = qtt_effect_certificate_format(
            certificate, ctx->effect_solver);
        if (!portable_certificate ||
            !ctx->last_effect_certificate_fingerprint)
            constraint_valid = false;
    }
    if (constraint_result == QTT_EFFECT_CONSTRAINT_RESIDUAL)
        judgment->coverage_gaps |=
            QTT_EFFECT_COVERAGE_RESIDUAL_OBLIGATION;
    if (constraint_valid) {
        for (size_t i = 0;
             i < judgment->returned_arrow_effect_count; i++) {
            QttEffectScheme *old = judgment->returned_arrow_effects[i];
            QttEffectScheme *captured = qtt_effect_generalize(
                ctx->effect_arena, ctx->effect_solver,
                judgment->returned_arrow_rows[i]);
            size_t dependency_count =
                qtt_effect_scheme_callable_parameter_count(old);
            size_t *indices = dependency_count
                ? malloc(dependency_count * sizeof(*indices)) : NULL;
            size_t *arities = dependency_count
                ? malloc(dependency_count * sizeof(*arities)) : NULL;
            if (dependency_count && (!indices || !arities)) {
                free(indices);
                free(arities);
                qtt_effect_scheme_free(captured);
                constraint_valid = false;
                break;
            }
            for (size_t j = 0; j < dependency_count; j++) {
                indices[j] = qtt_effect_scheme_callable_parameter_index(
                    old, j);
                arities[j] = qtt_effect_scheme_callable_parameter_arity(
                    old, j);
            }
            if (!captured || (dependency_count &&
                    !qtt_effect_scheme_set_callable_parameter_contracts(
                        captured, indices, arities, dependency_count))) {
                qtt_effect_scheme_free(captured);
                captured = NULL;
            }
            free(indices);
            free(arities);
            if (!captured) {
                constraint_valid = false;
                break;
            }
            qtt_effect_scheme_set_coverage_gaps(
                captured, qtt_effect_scheme_coverage_gaps(old));
            judgment->returned_arrow_effects[i] = captured;
            qtt_effect_scheme_free(old);
        }
    }
    qtt_effect_constraints_free(constraints);
    if (!constraint_valid) {
        free(portable_certificate);
        qtt_effect_certificate_free(certificate);
        return NULL;
    }
    QttEffectScheme *scheme = qtt_effect_generalize(
        ctx->effect_arena, ctx->effect_solver, summary);
    if (scheme && !qtt_effect_scheme_set_evidence(
            scheme, portable_certificate,
            ctx->last_effect_constraint_count,
            ctx->last_effect_certificate_fingerprint,
            constraint_result == QTT_EFFECT_CONSTRAINT_RESIDUAL
                ? QTT_EFFECT_EVIDENCE_RESIDUAL
                : QTT_EFFECT_EVIDENCE_SOLVED)) {
        qtt_effect_scheme_free(scheme);
        scheme = NULL;
    }
    qtt_effect_scheme_set_coverage_gaps(
        scheme, judgment->coverage_gaps);
    if (scheme && judgment->callable_parameter_mask) {
        size_t indices[64];
        size_t arities[64];
        size_t count = 0;
        for (size_t i = 0; i < 64; i++)
            if (judgment->callable_parameter_mask & (UINT64_C(1) << i)) {
                indices[count++] = i;
                arities[count - 1] = judgment->callable_parameter_arities[i];
            }
        if (!qtt_effect_scheme_set_callable_parameter_contracts(
                scheme, indices, arities, count)) {
            qtt_effect_scheme_free(scheme);
            scheme = NULL;
        }
    }
    free(portable_certificate);
    qtt_effect_certificate_free(certificate);
    if (complete) *complete = scheme && judgment->coverage_gaps == 0;
    return scheme;
}

static QttEffectScheme *infer_effect_scheme_for_roots(
    InferCtx *ctx, const AST *const *roots, size_t root_count,
    bool *complete) {
    if (complete) *complete = false;
    if (!ctx || (root_count && !roots) ||
        !ctx->effect_arena || !ctx->effect_solver)
        return NULL;
    QttEffectConstraintSet *constraints = qtt_effect_constraints_new();
    if (!constraints) return NULL;
    ctx->last_effect_constraint_count = 0;
    ctx->last_effect_certificate_fingerprint = 0;
    ctx->last_effect_constraints_residual = false;
    SourceEffectJudgment judgment = {
        .ctx = ctx,
        .constraints = constraints,
    };
    QttEffectRow *summary = qtt_effect_empty(ctx->effect_arena);
    for (size_t i = 0; summary && i < root_count; i++) {
        QttEffectRow *body = infer_source_effect_expr(
            &judgment, roots[i]);
        summary = infer_source_effect_join(&judgment, summary, body);
    }
    return infer_source_effect_finish(&judgment, summary, complete);
}

QttEffectScheme *infer_effect_scheme_for_lambda(
    InferCtx *ctx, const AST *lambda, bool *complete) {
    if (!lambda || lambda->type != AST_LAMBDA) {
        if (complete) *complete = false;
        return NULL;
    }
    return infer_effect_scheme_for_roots(
        ctx, (const AST *const *)lambda->lambda.body_exprs,
        (size_t)lambda->lambda.body_count, complete);
}

QttEffectScheme *infer_effect_scheme_for_expression(
    InferCtx *ctx, const AST *expression, bool *complete) {
    if (!expression) {
        if (complete) *complete = false;
        return NULL;
    }
    const AST *root = expression;
    return infer_effect_scheme_for_roots(ctx, &root, 1, complete);
}

size_t infer_effect_constraint_count(const InferCtx *ctx) {
    return ctx ? ctx->last_effect_constraint_count : 0;
}

uint64_t infer_effect_certificate_fingerprint(const InferCtx *ctx) {
    return ctx ? ctx->last_effect_certificate_fingerprint : 0;
}

bool infer_effect_constraints_residual(const InferCtx *ctx) {
    return ctx && ctx->last_effect_constraints_residual;
}

size_t scheme_grade_count(const TypeScheme *scheme) {
    return scheme
        ? qtt_grade_scheme_domain_count(scheme->grade_scheme) : 0;
}

size_t scheme_closure_grade_count(const TypeScheme *scheme) {
    return scheme
        ? qtt_grade_scheme_closure_count(scheme->grade_scheme) : 0;
}

QttGradeExpr **scheme_instantiate_grades(
    const TypeScheme *scheme, QttGradeArena *target, size_t *count) {
    if (count) *count = scheme_grade_count(scheme);
    if (!scheme || !scheme->grade_scheme) return NULL;
    QttGradeExpr **all =
        qtt_grade_instantiate(target, scheme->grade_scheme);
    size_t domain_count = scheme_grade_count(scheme);
    size_t closure_count = scheme_closure_grade_count(scheme);
    if (!closure_count) return all;
    QttGradeExpr **domains =
        domain_count ? malloc(domain_count * sizeof(*domains))
                     : calloc(1, sizeof(*domains));
    if (!domains) {
        free(all);
        return NULL;
    }
    if (domain_count)
        memcpy(domains, all, domain_count * sizeof(*domains));
    free(all);
    return domains;
}

QttGradeExpr **infer_instantiate_scheme_grades(
    InferCtx *ctx, const TypeScheme *scheme, size_t *count) {
    if (!ctx) {
        if (count) *count = 0;
        return NULL;
    }
    return scheme_instantiate_grades(
        scheme, ctx->grade_arena, count);
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

static bool judgment_name_is_bound(const AST *node, const char *name) {
    if (!node || !name || node->type != AST_SET) return false;
    for (size_t i = 0; i < node->set.element_count; i++) {
        AST *entry = node->set.elements[i];
        if (!entry || entry->type != AST_SYMBOL || !entry->symbol) continue;
        size_t n = strlen(entry->symbol);
        if (n && entry->symbol[n - 1] == ':' &&
            strlen(name) == n - 1 && strncmp(entry->symbol, name, n - 1) == 0)
            return true;
    }
    return false;
}

static bool judgment_term_is_closed(const AST *term, const AST *gamma,
                                    const AST *lambda_scope, bool head) {
    if (!term) return true;
    if (term->type == AST_SYMBOL) {
        if (head || judgment_name_is_bound(gamma, term->symbol)) return true;
        if (strcmp(term->symbol, "True") == 0 ||
            strcmp(term->symbol, "False") == 0 ||
            strcmp(term->symbol, "nil") == 0 ||
            strcmp(term->symbol, "undefined") == 0) return true;
        if (lambda_scope && lambda_scope->type == AST_LAMBDA)
            for (int i = 0; i < lambda_scope->lambda.param_count; i++)
                if (strcmp(lambda_scope->lambda.params[i].name, term->symbol) == 0)
                    return true;
        return false;
    }
    if (term->type == AST_LAMBDA) {
        for (int i = 0; i < term->lambda.body_count; i++)
            if (!judgment_term_is_closed(term->lambda.body_exprs[i], gamma,
                                         term, false)) return false;
        return true;
    }
    if (term->type == AST_LIST) {
        for (size_t i = 0; i < term->list.count; i++)
            if (!judgment_term_is_closed(term->list.items[i], gamma,
                                         lambda_scope, i == 0)) return false;
    } else if (term->type == AST_ARRAY) {
        for (size_t i = 0; i < term->array.element_count; i++)
            if (!judgment_term_is_closed(term->array.elements[i], gamma,
                                         lambda_scope, false)) return false;
    }
    return true;
}

typedef struct JudgmentIntBinding {
    const char *name;
    long long value;
    const struct JudgmentIntBinding *parent;
} JudgmentIntBinding;

static bool judgment_eval_int(const AST *term, const JudgmentIntBinding *env,
                              long long *out) {
    if (!term || !out) return false;
    if (term->type == AST_NUMBER &&
        (!term->literal_str || (!strchr(term->literal_str, '.') &&
                               !strchr(term->literal_str, 'e') &&
                               !strchr(term->literal_str, 'E')))) {
        *out = term->has_raw_int ? (long long)term->raw_int
                                 : (long long)term->number;
        return true;
    }
    if (term->type == AST_SYMBOL) {
        for (const JudgmentIntBinding *b = env; b; b = b->parent)
            if (strcmp(b->name, term->symbol) == 0) {
                *out = b->value;
                return true;
            }
        return false;
    }
    if (term->type != AST_LIST || term->list.count == 0) return false;
    AST *head = term->list.items[0];
    if (head->type == AST_LAMBDA && term->list.count == 2 &&
        head->lambda.param_count == 1) {
        long long argument;
        if (!judgment_eval_int(term->list.items[1], env, &argument)) return false;
        JudgmentIntBinding binding = {
            head->lambda.params[0].name, argument, env
        };
        return judgment_eval_int(head->lambda.body, &binding, out);
    }
    if (head->type == AST_SYMBOL && term->list.count == 3) {
        long long left, right;
        if (!judgment_eval_int(term->list.items[1], env, &left) ||
            !judgment_eval_int(term->list.items[2], env, &right)) return false;
        if (strcmp(head->symbol, "+") == 0) *out = left + right;
        else if (strcmp(head->symbol, "-") == 0) *out = left - right;
        else if (strcmp(head->symbol, "*") == 0) *out = left * right;
        else if (strcmp(head->symbol, "/") == 0 && right != 0) *out = left / right;
        else return false;
        return true;
    }
    return false;
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
        TypeScheme *sc = infer_env_lookup(ctx, ast->symbol);
        if (strcmp(ast->symbol, "*context*") == 0) {
            result = type_set();
            break;
        }
        const FiniteTypeSetEntry *named_finite =
            finite_type_set_lookup(ast->symbol);
        if (named_finite && !sc) {
            result = type_set_of(type_finite_set(named_finite->name,
                                                  named_finite->member_count));
            break;
        }
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
        result = type_set_of(elem_t);
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
            if (ast->inferred_type &&
                (ast->inferred_type->kind == TYPE_APP ||
                 ast->inferred_type->kind == TYPE_LAYOUT)) {
                result = type_clone(ast->inferred_type);
                break;
            }
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

    case AST_JUDGMENT: {
        char judgment_error[2048] = {0};
        if (!ctx->dctx || !dep_check_surface_judgment(
                ctx->dctx, ast, judgment_error, sizeof(judgment_error))) {
            ctx->had_error = true;
            snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s",
                     judgment_error[0] ? judgment_error
                                       : "typing judgment is not derivable");
            result = type_unknown();
            break;
        }
        DepDerivation *derivation = dep_take_surface_derivation(ctx->dctx);
        const char *conclusion = dep_derivation_conclusion(derivation);
        result = type_app("Proof", type_finite_set(
            conclusion ? conclusion : "?", 1));
        dep_derivation_free(derivation);
        break;
    }

    case AST_LIST: {
        if (ast->list.count == 0) {
            /* () is the canonical inhabitant of Unit.  It is deliberately
             * distinct from the empty list constructor, (list). */
            result = type_unit();
            break;
        }

        AST *head = ast->list.items[0];

        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "__judgment") == 0) {
            if ((ast->list.count != 4 && ast->list.count != 5) ||
                ast->list.items[1]->type != AST_SET ||
                ast->list.items[ast->list.count - 1]->type != AST_SYMBOL) {
                ctx->had_error = true;
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                         "%s:%d:%d: error: malformed explicit judgment",
                         ctx->filename, ast->line, ast->column);
                result = type_unknown();
                break;
            }
            AST *gamma = ast->list.items[1];
            AST *left = ast->list.items[2];
            AST *right = ast->list.count == 5 ? ast->list.items[3] : NULL;
            AST *type_node = ast->list.items[ast->list.count - 1];
            if (!judgment_term_is_closed(left, gamma, NULL, false) ||
                (right && !judgment_term_is_closed(right, gamma, NULL, false))) {
                ctx->had_error = true;
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                         "%s:%d:%d: error: explicit context does not bind every free variable",
                         ctx->filename, ast->line, ast->column);
                result = type_unknown();
                break;
            }
            InferEnv *saved = ctx->env;
            InferEnv *local = infer_env_create_child(saved);
            ctx->env = local;
            for (size_t i = 0; i + 1 < gamma->set.element_count; i += 2) {
                AST *name = gamma->set.elements[i];
                AST *annotation = gamma->set.elements[i + 1];
                if (!name || name->type != AST_SYMBOL || !name->symbol ||
                    !annotation || annotation->type != AST_SYMBOL) continue;
                size_t n = strlen(name->symbol);
                if (!n || name->symbol[n - 1] != ':') continue;
                char *plain = strndup(name->symbol, n - 1);
                Type *binding_type = type_from_name(annotation->symbol);
                if (!binding_type) binding_type = type_unknown();
                infer_env_insert(local, plain, scheme_mono(binding_type));
                free(plain);
            }
            Type *left_type = infer_expr(ctx, left);
            Type *right_type = right ? infer_expr(ctx, right) : NULL;
            ctx->env = saved;
            infer_env_free(local);
            Type *claimed = type_from_name(type_node->symbol);
            if (!claimed) claimed = type_unknown();
            infer_constrain(ctx, left_type, claimed, ast->line, ast->column);
            if (right) infer_constrain(ctx, right_type, claimed,
                                       ast->line, ast->column);
            if (right) {
                long long lv, rv;
                if (!judgment_eval_int(left, NULL, &lv) ||
                    !judgment_eval_int(right, NULL, &rv) || lv != rv) {
                    ctx->had_error = true;
                    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                             "%s:%d:%d: error: terms are not definitionally equal",
                             ctx->filename, ast->line, ast->column);
                }
            }
            result = claimed;
            break;
        }

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
        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "perform") == 0) {
            if (ast->list.count != 3 ||
                ast->list.items[1]->type != AST_SYMBOL)
                READER_ERROR(ast->line, ast->column,
                    "perform expects an effect name and one payload");
            Type *payload = infer_expr(ctx, ast->list.items[2]);
            const QttEffectDeclaration *declaration =
                qtt_effect_declaration_lookup(ast->list.items[1]->symbol);
            if (declaration && declaration->operation_scheme) {
                Type *expected = NULL, *operation_result = NULL;
                if (!infer_operation_scheme_instantiate(
                        ctx, declaration->operation_scheme,
                        &expected, &operation_result))
                    READER_ERROR(ast->line, ast->column,
                        "effect '%s' has an invalid portable operation scheme",
                        declaration->name);
                infer_constrain(ctx, payload, expected,
                    ast->list.items[2]->line,
                    ast->list.items[2]->column);
                result = operation_result;
            } else if (declaration && declaration->payload_type &&
                declaration->result_type) {
                Type *expected = type_from_name(declaration->payload_type);
                Type *operation_result =
                    type_from_name(declaration->result_type);
                if (!expected || !operation_result)
                    READER_ERROR(ast->line, ast->column,
                        "effect '%s' has a non-resolvable operation signature",
                        declaration->name);
                infer_constrain(ctx, payload, expected,
                    ast->list.items[2]->line,
                    ast->list.items[2]->column);
                result = operation_result;
            } else {
                /* Compatibility for declarations predating typed operation
                 * signatures. New Core declarations must use the typed ABI. */
                result = payload;
            }
            break;
        }

        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "handle") == 0) {
            if (ast->list.count != 4 ||
                ast->list.items[1]->type != AST_SYMBOL)
                READER_ERROR(ast->line, ast->column,
                    "handle expects a profile, computation, and clause");
            Type *computation = infer_expr(ctx, ast->list.items[2]);
            Type *clause = infer_expr(ctx, ast->list.items[3]);
            Type *clause_payload = computation;
            const QttEffectHandlerProfile *profile =
                qtt_effect_handler_profile_lookup(
                    ast->list.items[1]->symbol);
            const QttEffectDeclaration *declaration = profile
                ? qtt_effect_declaration_lookup(profile->effect_name) : NULL;
            if (declaration && declaration->operation_scheme) {
                Type *declared_result = NULL;
                if (!infer_operation_scheme_instantiate(
                        ctx, declaration->operation_scheme,
                        &clause_payload, &declared_result))
                    READER_ERROR(ast->line, ast->column,
                        "handler '%s' refers to an invalid portable operation "
                        "scheme", ast->list.items[1]->symbol);
                infer_constrain(ctx, computation, declared_result,
                    ast->list.items[2]->line,
                    ast->list.items[2]->column);
                computation = declared_result;
            } else if (declaration && declaration->payload_type &&
                declaration->result_type) {
                clause_payload = type_from_name(declaration->payload_type);
                Type *declared_result =
                    type_from_name(declaration->result_type);
                if (!clause_payload || !declared_result)
                    READER_ERROR(ast->line, ast->column,
                        "handler '%s' refers to a non-resolvable operation "
                        "signature", ast->list.items[1]->symbol);
                infer_constrain(ctx, computation, declared_result,
                    ast->list.items[2]->line,
                    ast->list.items[2]->column);
                computation = declared_result;
            }
            infer_constrain(ctx, clause,
                type_arrow(clause_payload, computation),
                ast->list.items[3]->line, ast->list.items[3]->column);
            result = computation;
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
                    bool then_opt = infer_optional_like(ctx, then_t);
                    bool else_opt = infer_optional_like(ctx, else_t);

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

        /* Primitive type names are conversion forms at expression heads.
         * Core may also export values named String/Char, but those bindings
         * must not replace the language's cast semantics during call
         * validation.  Code generation already lowers these four forms. */
        if (head->type == AST_SYMBOL && ast->list.count == 2 &&
            (!strcmp(head->symbol, "Int") ||
             !strcmp(head->symbol, "Float") ||
             !strcmp(head->symbol, "Char") ||
             !strcmp(head->symbol, "String"))) {
            (void)infer_expr(ctx, ast->list.items[1]);
            if (!strcmp(head->symbol, "Int"))
                result = type_int();
            else if (!strcmp(head->symbol, "Float"))
                result = type_float();
            else if (!strcmp(head->symbol, "Char"))
                result = type_char();
            else
                result = type_string();
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
            strcmp(head->symbol, "__rt_directory_names") == 0 &&
            ast->list.count == 3) {
            (void)infer_expr(ctx, ast->list.items[1]);
            (void)infer_expr(ctx, ast->list.items[2]);
            result = type_coll();
            result->element_type = type_string();
            break;
        }

        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "__rt_join_path") == 0 &&
            ast->list.count == 3) {
            (void)infer_expr(ctx, ast->list.items[1]);
            (void)infer_expr(ctx, ast->list.items[2]);
            result = type_path();
            break;
        }

        if (head->type == AST_SYMBOL &&
            (strcmp(head->symbol, "__rt_path_text") == 0 ||
             strcmp(head->symbol, "__rt_text_path") == 0) &&
            ast->list.count == 2) {
            (void)infer_expr(ctx, ast->list.items[1]);
            result = strcmp(head->symbol, "__rt_path_text") == 0
                ? type_string() : type_path();
            break;
        }

        if (head->type == AST_SYMBOL &&
            (strcmp(head->symbol, "__rt_string_take") == 0 ||
             strcmp(head->symbol, "__rt_string_drop") == 0) &&
            ast->list.count == 3) {
            Type *text_t = infer_expr(ctx, ast->list.items[1]);
            Type *count_t = infer_expr(ctx, ast->list.items[2]);
            infer_constrain(ctx, text_t, type_string(), ast->line, ast->column);
            infer_constrain(ctx, count_t, type_int(), ast->line, ast->column);
            result = type_string();
            break;
        }

        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "__rt_string_byte") == 0 &&
            ast->list.count == 3) {
            Type *text_t = infer_expr(ctx, ast->list.items[1]);
            Type *index_t = infer_expr(ctx, ast->list.items[2]);
            infer_constrain(ctx, text_t, type_string(), ast->line, ast->column);
            infer_constrain(ctx, index_t, type_int(), ast->line, ast->column);
            result = type_int();
            break;
        }

        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "__rt_char_string") == 0 &&
            ast->list.count == 2) {
            Type *char_t = infer_expr(ctx, ast->list.items[1]);
            infer_constrain(ctx, char_t, type_char(), ast->line, ast->column);
            result = type_string();
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
            /* One supplied operand denotes the section captured by codegen:
             * (+ 3) :: Num a => a -> a.  Treating it as `a` made higher-order
             * consumers reject it and later encouraged ABI-unsafe fallbacks. */
            result = ast->list.count == 2
                ? type_arrow(type_clone(num_t), num_t)
                : num_t;
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
                        head_t->kind == TYPE_PTR ||
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

                    if (head_t->kind == TYPE_PTR && head_t->element_type) {
                        result = subst_apply(ctx->subst,
                                             head_t->element_type);
                        break;
                    }

                    if (head_t->kind == TYPE_SET ||
                        head_t->kind == TYPE_MAP ||
                        head_t->kind == TYPE_COLL ||
                        head_t->kind == TYPE_ARR ||
                        head_t->kind == TYPE_PTR ||
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
                               head_t->kind == TYPE_COLL    ||
                               head_t->kind == TYPE_PTR)) {
                    infer_expr(ctx, ast->list.items[1]);

                    if (head_t->kind == TYPE_STRING)
                        result = type_char();
                    else if (head_t->kind == TYPE_COLL && head_t->element_type)
                        result = subst_apply(ctx->subst, head_t->element_type);
                    else if (head_t->kind == TYPE_ARR && head_t->arr_element_type)
                        result = subst_apply(ctx->subst, head_t->arr_element_type);
                    else if (head_t->kind == TYPE_PTR && head_t->element_type)
                        result = subst_apply(ctx->subst, head_t->element_type);
                    else
                        result = infer_fresh(ctx);
                    break;
                }
            }
        }

        /* A finite Relation is a predicate when both endpoints are ground.
         * Its algorithms remain Core-owned; this is only callable-value
         * typing, analogous to the existing Set/Map callable protocol. */
        if (head->type == AST_SYMBOL && ast->list.count == 3) {
            TypeScheme *relation_sc = infer_env_lookup(ctx, head->symbol);
            Type *relation_t = relation_sc
                ? subst_apply(ctx->subst, relation_sc->type) : NULL;
            if (relation_t && relation_t->kind == TYPE_APP &&
                relation_t->app_constructor &&
                strcmp(relation_t->app_constructor, "Relation") == 0) {
                bool left_hole = ast->list.items[1]->type == AST_SYMBOL &&
                    !infer_env_lookup(ctx, ast->list.items[1]->symbol);
                bool right_hole = ast->list.items[2]->type == AST_SYMBOL &&
                    !infer_env_lookup(ctx, ast->list.items[2]->symbol);
                Type *args = relation_t->app_arg;
                if (left_hole != right_hole && args &&
                    args->kind == TYPE_LIST && args->list_count == 2) {
                    infer_expr(ctx, ast->list.items[left_hole ? 2 : 1]);
                    Type *element = type_clone(
                        args->list_types[left_hole ? 0 : 1]);
                    result = type_list(&element, 1);
                } else {
                    infer_expr(ctx, ast->list.items[1]);
                    infer_expr(ctx, ast->list.items[2]);
                    result = type_bool();
                }
                break;
            }
        }

        InferQuantitativeType quantitative_head = {0};
        bool has_quantitative_head = false;
        Type *fn_t = NULL;
        if (head->type == AST_SYMBOL) {
            TypeScheme *head_sc = infer_env_lookup(ctx, head->symbol);
            if (head_sc && head_sc->grade_scheme) {
                InferQuantitativeResult quantitative_result =
                    infer_instantiate_quantitative(
                        ctx, head_sc, &quantitative_head);
                if (quantitative_result == INFER_QUANTITATIVE_OK) {
                    fn_t = infer_normalize_annotation_type(
                        quantitative_head.type);
                    head->inferred_type = fn_t;
                    has_quantitative_head = true;
                } else {
                    ctx->had_error = true;
                    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                             "%s:%d:%d: quantitative type scheme for '%s' "
                             "has %zu grades but its type has a different "
                             "arrow arity",
                             ctx->filename, head->line, head->column,
                             head->symbol, scheme_grade_count(head_sc));
                }
            }
        }
        if (!fn_t)
            fn_t = infer_normalize_annotation_type(infer_expr(ctx, head));
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
        if (has_quantitative_head) {
            size_t supplied = ast->list.count - 1;
            size_t applied = supplied < quantitative_head.domain_count
                ? supplied : quantitative_head.domain_count;
            for (size_t i = 0; i < applied; i++) {
                Type *domain_type = NULL;
                QttGradeExpr *domain_grade = NULL;
                if (!infer_quantitative_take_domain(
                        &quantitative_head, &domain_type, &domain_grade) ||
                    !domain_type || !domain_grade) {
                    ctx->had_error = true;
                    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                             "%s:%d:%d: quantitative application evidence "
                             "lost arrow-domain alignment",
                             ctx->filename, ast->line, ast->column);
                    break;
                }
            }
            if (!infer_retain_quantitative_application(
                    ctx, ast, &quantitative_head, applied)) {
                ctx->had_error = true;
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                         "%s:%d:%d: out of memory retaining quantitative "
                         "application evidence",
                         ctx->filename, ast->line, ast->column);
            }
            infer_quantitative_type_free(&quantitative_head);
        }
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
    Type *directory_names = type_coll();
    directory_names->element_type = type_string();
    infer_env_insert(ctx->env, "__rt_directory_names",
                     scheme_mono(type_arrow(type_path(),
                         type_arrow(type_ptr(type_int()), directory_names))));
    infer_env_insert(ctx->env, "__rt_join_path",
                     scheme_mono(type_arrow(type_path(),
                         type_arrow(type_string(), type_path()))));
    infer_env_insert(ctx->env, "__rt_path_text",
                     scheme_mono(type_arrow(type_path(), type_string())));
    infer_env_insert(ctx->env, "__rt_text_path",
                     scheme_mono(type_arrow(type_string(), type_path())));
    Type *string_slice_type = type_arrow(
        type_string(), type_arrow(type_int(), type_string()));
    infer_env_insert(ctx->env, "__rt_string_take",
                     scheme_mono(type_clone(string_slice_type)));
    infer_env_insert(ctx->env, "__rt_string_drop",
                     scheme_mono(string_slice_type));
    infer_env_insert(ctx->env, "__rt_string_byte",
                     scheme_mono(type_arrow(type_string(),
                                            type_arrow(type_int(), type_int()))));
    infer_env_insert(ctx->env, "__rt_char_string",
                     scheme_mono(type_arrow(type_char(), type_string())));
    infer_env_insert(ctx->env, "__rt_set_singleton",
                     scheme_mono(type_arrow(type_set(), type_bool())));
    /* The sole Set representation eliminator exposed to Data.Set:
     * forall a. Set a -> [a].  Public operations remain ordinary Monad code. */
    Type *set_elements_a = infer_fresh(ctx);
    Type *set_elements_in = type_set_of(type_clone(set_elements_a));
    Type *set_elements_out = type_list(&set_elements_a, 1);
    TypeScheme *set_elements_sc = infer_generalise(
        ctx, type_arrow(set_elements_in, set_elements_out), ctx->env);
    infer_env_insert(ctx->env, "__rt_set_elements", set_elements_sc);

    /* forall a b. (a -> b) -> [a] -> [b].  Public map remains the Functor
     * method; this is the single representation boundary for lazy lists. */
    Type *map_a = infer_fresh(ctx);
    Type *map_b = infer_fresh(ctx);
    Type *map_in = type_list(&map_a, 1);
    Type *map_out = type_list(&map_b, 1);
    TypeScheme *map_closure_sc = infer_generalise(ctx,
        type_arrow(type_arrow(type_clone(map_a), type_clone(map_b)),
                   type_arrow(map_in, map_out)), ctx->env);
    infer_env_insert(ctx->env, "__rt_coll_map_closure", map_closure_sc);

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

    Type *write_elem = infer_fresh(ctx);
    Type *write_arr = type_arr_fat(write_elem);
    infer_env_insert(ctx->env, "__rt_write_array", infer_generalise(ctx,
        type_arrow(type_int(), type_arrow(write_arr,
            type_arrow(type_int(), type_int()))), ctx->env));
}


/// Top-level Entry Point

static void infer_validate_calls(InferCtx *ctx, AST *ast) {
    if (!ast) return;

    if (ast->type == AST_LIST && ast->list.count >= 2) {
        AST *head = ast->list.items[0];
        if (head->type == AST_SYMBOL) {
            bool primitive_cast = ast->list.count == 2 &&
                (!strcmp(head->symbol, "Int") ||
                 !strcmp(head->symbol, "Float") ||
                 !strcmp(head->symbol, "Char") ||
                 !strcmp(head->symbol, "String"));
            TypeScheme *sc = primitive_cast
                ? NULL : infer_env_lookup(ctx, head->symbol);
            if (sc) {
                /* Validate against this occurrence's instantiated, zonked
                 * callable. Applying a local substitution directly to the
                 * shared principal scheme aliases canonical quantifier IDs
                 * with unrelated local variables. */
                Type *ft = head->inferred_type
                    ? subst_apply(ctx->subst, head->inferred_type)
                    : infer_instantiate(ctx, sc);
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
                        if (asc) arg_t = infer_instantiate(ctx, asc);
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

static bool infer_fused_fragment_supported(
    InferCtx *ctx, const AST *ast, bool allow_lambda) {
    if (!ast) return true;
    switch (ast->type) {
    case AST_NUMBER: case AST_STRING: case AST_PATH: case AST_CHAR:
    case AST_KEYWORD: case AST_RATIO:
        return true;
    case AST_SYMBOL:
        return true;
    case AST_ARRAY:
        for (size_t i = 0; i < ast->array.element_count; i++)
            if (!infer_fused_fragment_supported(
                    ctx, ast->array.elements[i], false))
                return false;
        return true;
    case AST_SET:
        for (size_t i = 0; i < ast->set.element_count; i++)
            if (!infer_fused_fragment_supported(
                    ctx, ast->set.elements[i], false))
                return false;
        return true;
    case AST_MAP:
        for (size_t i = 0; i < ast->map.count; i++)
            if (!infer_fused_fragment_supported(
                    ctx, ast->map.keys[i], false) ||
                !infer_fused_fragment_supported(
                    ctx, ast->map.vals[i], false))
                return false;
        return true;
    case AST_LIST: {
        if (!ast->list.count || !ast->list.items[0] ||
            ast->list.items[0]->type != AST_SYMBOL)
            return false;
        const char *name = ast->list.items[0]->symbol;
        if (strcmp(name, "begin") == 0) {
            for (size_t i = 1; i < ast->list.count; i++)
                if (!infer_fused_fragment_supported(
                        ctx, ast->list.items[i], false))
                    return false;
            return true;
        }
        if (strcmp(name, "if") == 0) {
            if (ast->list.count < 3 || ast->list.count > 4)
                return false;
            for (size_t i = 1; i < ast->list.count; i++)
                if (!infer_fused_fragment_supported(
                        ctx, ast->list.items[i], false))
                    return false;
            return true;
        }
        if (infer_source_special_form(name)) return false;
        TypeScheme *callee = infer_env_lookup(
            ctx, name);
        if (!callee || callee->grade_scheme) return false;
        size_t domains = 0;
        for (Type *type = callee->type;
             type && type->kind == TYPE_ARROW; type = type->arrow_ret)
            domains++;
        if (callee->type->kind != TYPE_VAR &&
            ast->list.count - 1 > domains)
            return false;
        for (size_t i = 1; i < ast->list.count; i++)
            if (!infer_fused_fragment_supported(
                    ctx, ast->list.items[i], false))
                return false;
        return true;
    }
    case AST_LAMBDA:
        if (!allow_lambda) return false;
        for (int i = 0; i < ast->lambda.param_count; i++)
            if (ast->lambda.params[i].is_rest ||
                ast->lambda.params[i].type_name)
                return false;
        if (ast->lambda.return_type) return false;
        {
            InferEnv *saved = ctx->env;
            InferEnv *child = infer_env_create_child(saved);
            ctx->env = child;
            for (int i = 0; i < ast->lambda.param_count; i++) {
                TypeScheme *parameter = scheme_mono(infer_fresh(ctx));
                QttEffectScheme *open = qtt_effect_generalize(
                    ctx->effect_arena, ctx->effect_solver,
                    qtt_effect_fresh(ctx->effect_arena));
                scheme_set_effect_scheme(parameter, open, false);
                qtt_effect_scheme_free(open);
                infer_env_insert(
                    child, ast->lambda.params[i].name, parameter);
            }
            bool supported = true;
            for (int i = 0; i < ast->lambda.body_count; i++)
                if (!infer_fused_fragment_supported(
                        ctx, ast->lambda.body_exprs[i], false)) {
                    supported = false;
                    break;
                }
            ctx->env = saved;
            infer_env_free(child);
            return supported;
        }
    default:
        return false;
    }
}

static Type *infer_fused_fragment(
    SourceEffectJudgment *judgment, AST *ast, QttEffectRow **effect) {
    InferCtx *ctx = judgment->ctx;
    QttEffectRow *summary = qtt_effect_empty(ctx->effect_arena);
    Type *type = NULL;
    switch (ast->type) {
    case AST_NUMBER: {
        bool is_float = false;
        if (ast->literal_str) {
            bool radix_literal = ast->literal_str[0] == '0' &&
                (ast->literal_str[1] == 'x' || ast->literal_str[1] == 'X' ||
                 ast->literal_str[1] == 'b' || ast->literal_str[1] == 'B' ||
                 ast->literal_str[1] == 'o' || ast->literal_str[1] == 'O');
            if (!radix_literal)
                for (const char *p = ast->literal_str; *p; p++)
                    if (*p == '.' || *p == 'e' || *p == 'E') {
                        is_float = true;
                        break;
                    }
        } else {
            is_float = ast->number != (double)(int64_t)ast->number;
        }
        if (!is_float && ast->inferred_type &&
            ast->inferred_type->kind == TYPE_FLOAT)
            is_float = true;
        type = is_float ? type_float() : type_int();
        break;
    }
    case AST_STRING: type = type_string(); break;
    case AST_PATH: type = type_path(); break;
    case AST_CHAR: type = type_char(); break;
    case AST_KEYWORD: type = type_keyword(); break;
    case AST_RATIO: type = type_ratio(); break;
    case AST_SYMBOL: {
        if (strcmp(ast->symbol, "nil") == 0) {
            type = type_nil();
            break;
        }
        TypeScheme *scheme = infer_env_lookup(ctx, ast->symbol);
        type = scheme ? infer_instantiate(ctx, scheme) : infer_fresh(ctx);
        break;
    }
    case AST_ARRAY: {
        Type *element = infer_fresh(ctx);
        if (ast->array.is_heap) {
            QttEffectRow *declared = qtt_effect_extend_declared(
                ctx->effect_arena, "Core.Alloc.heap", 0, 0, summary);
            summary = declared ? declared : qtt_effect_extend(
                ctx->effect_arena, "alloc", summary);
        }
        for (size_t i = 0; summary && i < ast->array.element_count; i++) {
            QttEffectRow *child_effect = NULL;
            Type *child = infer_fused_fragment(
                judgment, ast->array.elements[i], &child_effect);
            infer_constrain(ctx, child, element, ast->line, ast->column);
            summary = infer_source_effect_join(
                judgment, summary, child_effect);
        }
        if (!ast->array.element_count) {
            type = type_coll();
            type->element_type = element;
        } else {
            type = ast->array.is_heap
                ? type_arr_heap(element)
                : type_arr(element, (int)ast->array.element_count);
        }
        break;
    }
    case AST_SET: {
        Type *element = infer_fresh(ctx);
        for (size_t i = 0; summary && i < ast->set.element_count; i++) {
            QttEffectRow *child_effect = NULL;
            Type *child = infer_fused_fragment(
                judgment, ast->set.elements[i], &child_effect);
            infer_constrain(ctx, child, element, ast->line, ast->column);
            summary = infer_source_effect_join(
                judgment, summary, child_effect);
        }
        type = type_set_of(element);
        break;
    }
    case AST_MAP: {
        Type *key = infer_fresh(ctx);
        Type *value = infer_fresh(ctx);
        for (size_t i = 0; summary && i < ast->map.count; i++) {
            QttEffectRow *key_effect = NULL;
            QttEffectRow *value_effect = NULL;
            Type *key_type = infer_fused_fragment(
                judgment, ast->map.keys[i], &key_effect);
            Type *value_type = infer_fused_fragment(
                judgment, ast->map.vals[i], &value_effect);
            infer_constrain(ctx, key_type, key, ast->line, ast->column);
            infer_constrain(ctx, value_type, value, ast->line, ast->column);
            summary = infer_source_effect_join(
                judgment, summary, key_effect);
            summary = infer_source_effect_join(
                judgment, summary, value_effect);
        }
        type = type_map_of(key, value);
        break;
    }
    case AST_LIST: {
        AST *head = ast->list.items[0];
        if (strcmp(head->symbol, "begin") == 0) {
            type = type_int();
            for (size_t i = 1; summary && i < ast->list.count; i++) {
                QttEffectRow *item_effect = NULL;
                type = infer_fused_fragment(
                    judgment, ast->list.items[i], &item_effect);
                summary = infer_source_effect_join(
                    judgment, summary, item_effect);
            }
            break;
        }
        if (strcmp(head->symbol, "if") == 0) {
            QttEffectRow *condition_effect = NULL;
            QttEffectRow *then_effect = NULL;
            QttEffectRow *else_effect = NULL;
            Type *condition = infer_fused_fragment(
                judgment, ast->list.items[1], &condition_effect);
            Type *then_type = infer_fused_fragment(
                judgment, ast->list.items[2], &then_effect);
            infer_constrain(
                ctx, condition, type_bool(),
                ast->list.items[1]->line,
                ast->list.items[1]->column);
            summary = infer_source_effect_join(
                judgment, summary, condition_effect);
            summary = infer_source_effect_join(
                judgment, summary, then_effect);
            if (ast->list.count == 4) {
                Type *else_type = infer_fused_fragment(
                    judgment, ast->list.items[3], &else_effect);
                bool then_optional = infer_optional_like(ctx, then_type);
                bool else_optional = infer_optional_like(ctx, else_type);
                if (then_optional && !else_optional)
                    else_type = type_optional(else_type);
                else if (else_optional && !then_optional)
                    then_type = type_optional(then_type);
                infer_constrain(
                    ctx, then_type, else_type,
                    ast->list.items[3]->line,
                    ast->list.items[3]->column);
                summary = infer_source_effect_join(
                    judgment, summary, else_effect);
            }
            type = then_type;
            break;
        }
        TypeScheme *callee = infer_env_lookup(ctx, head->symbol);
        size_t supplied = ast->list.count - 1;
        for (size_t i = 0; i < judgment->lambda_parameter_count; i++)
            if (callee == judgment->lambda_parameters[i]) {
                judgment->callable_parameter_mask |= UINT64_C(1) << i;
                if (supplied > judgment->callable_parameter_arities[i])
                    judgment->callable_parameter_arities[i] = supplied;
            }
        Type *function = infer_instantiate(ctx, callee);
        head->inferred_type = function;
        size_t domains = 0;
        for (Type *cursor = function;
             cursor && cursor->kind == TYPE_ARROW; cursor = cursor->arrow_ret)
            domains++;
        Type *cursor = function;
        bool existential_arrow = cursor && cursor->kind == TYPE_VAR;
        Type **argument_types = existential_arrow && supplied
            ? malloc(supplied * sizeof(*argument_types)) : NULL;
        if (existential_arrow && supplied && !argument_types) return NULL;
        for (size_t i = 0; summary && i < supplied; i++) {
            QttEffectRow *argument_effect = NULL;
            Type *argument = infer_fused_fragment(
                judgment, ast->list.items[i + 1], &argument_effect);
            if (existential_arrow) {
                argument_types[i] = argument;
            } else {
                if (!cursor || cursor->kind != TYPE_ARROW) {
                    free(argument_types);
                    return NULL;
                }
                infer_constrain(
                    ctx, argument, cursor->arrow_param,
                    ast->line, ast->column);
                cursor = cursor->arrow_ret;
            }
            summary = infer_source_effect_join(
                judgment, summary, argument_effect);
        }
        if (existential_arrow) {
            Type *return_type = infer_fresh(ctx);
            Type *expected = return_type;
            for (size_t i = supplied; i > 0; i--)
                expected = type_arrow(argument_types[i - 1], expected);
            infer_constrain(
                ctx, function, expected, ast->line, ast->column);
            cursor = return_type;
            domains = supplied;
        }
        free(argument_types);
        infer_source_effect_clear_returned(judgment);
        size_t arrow_effect_count = infer_scheme_arrow_effect_count(callee);
        if (supplied < domains && arrow_effect_count == domains) {
            size_t residual_count = domains - supplied;
            judgment->returned_arrow_effects = calloc(
                residual_count, sizeof(*judgment->returned_arrow_effects));
            judgment->returned_arrow_rows = calloc(
                residual_count, sizeof(*judgment->returned_arrow_rows));
            judgment->returned_arrow_effect_names = calloc(
                residual_count,
                sizeof(*judgment->returned_arrow_effect_names));
            judgment->returned_arrow_effects_complete = malloc(
                residual_count *
                    sizeof(*judgment->returned_arrow_effects_complete));
            if (!judgment->returned_arrow_effects ||
                    !judgment->returned_arrow_rows ||
                    !judgment->returned_arrow_effect_names ||
                    !judgment->returned_arrow_effects_complete) {
                infer_source_effect_clear_returned(judgment);
                return NULL;
            }
            judgment->returned_arrow_effect_count = residual_count;
            for (size_t i = 0; i < residual_count; i++) {
                bool source_complete = false;
                QttEffectScheme *source_stage =
                    infer_scheme_arrow_effect_at(
                        callee, supplied + i, &source_complete, NULL);
                judgment->returned_arrow_effects[i] =
                    source_stage ? infer_source_specialize_residual_stage(
                        judgment,
                        source_stage, ast, supplied, source_complete,
                        &judgment->returned_arrow_effects_complete[i])
                    : NULL;
                qtt_effect_scheme_free(source_stage);
                if (!judgment->returned_arrow_effects[i]) {
                    infer_source_effect_clear_returned(judgment);
                    return NULL;
                }
                judgment->returned_arrow_rows[i] = qtt_effect_instantiate(
                    ctx->effect_arena,
                    judgment->returned_arrow_effects[i]);
                const char *effect_name =
                    infer_scheme_arrow_effect_name_at(callee, supplied + i);
                judgment->returned_arrow_effect_names[i] = effect_name
                    ? strdup(effect_name) : NULL;
                if (!judgment->returned_arrow_rows[i] ||
                        (effect_name &&
                         !judgment->returned_arrow_effect_names[i])) {
                    infer_source_effect_clear_returned(judgment);
                    return NULL;
                }
                if (!infer_source_constrain_stage_label(
                        judgment,
                        judgment->returned_arrow_effect_names[i],
                        judgment->returned_arrow_rows[i],
                        (const char *const *)
                            judgment->returned_arrow_effect_names,
                        judgment->returned_arrow_rows, i)) {
                    infer_source_effect_clear_returned(judgment);
                    return NULL;
                }
            }
            size_t residual_predicates = 0;
            for (size_t i = 0;
                 i < callee->effect_trait_predicate_count; i++)
                if (callee->effect_trait_predicate_stages[i] >= supplied)
                    residual_predicates++;
            judgment->returned_effect_trait_predicate_stages =
                residual_predicates ? malloc(residual_predicates *
                    sizeof(*judgment->
                        returned_effect_trait_predicate_stages)) : NULL;
            judgment->returned_effect_trait_predicate_names =
                residual_predicates ? calloc(residual_predicates,
                    sizeof(*judgment->
                        returned_effect_trait_predicate_names)) : NULL;
            if (residual_predicates &&
                    (!judgment->returned_effect_trait_predicate_stages ||
                     !judgment->returned_effect_trait_predicate_names)) {
                infer_source_effect_clear_returned(judgment);
                return NULL;
            }
            for (size_t i = 0;
                 i < callee->effect_trait_predicate_count; i++) {
                size_t source_stage =
                    callee->effect_trait_predicate_stages[i];
                if (source_stage < supplied) continue;
                size_t at =
                    judgment->returned_effect_trait_predicate_count;
                judgment->returned_effect_trait_predicate_stages[at] =
                    source_stage - supplied;
                judgment->returned_effect_trait_predicate_names[at] =
                    strdup(callee->effect_trait_predicate_names[i]);
                if (!judgment->returned_effect_trait_predicate_names[at]) {
                    infer_source_effect_clear_returned(judgment);
                    return NULL;
                }
                judgment->returned_effect_trait_predicate_count++;
            }
        }
        if (arrow_effect_count) {
            if (arrow_effect_count != domains) {
                judgment->coverage_gaps |=
                    QTT_EFFECT_COVERAGE_ARITY_MISMATCH;
            } else {
                const char **stage_names = supplied
                    ? calloc(supplied, sizeof(*stage_names)) : NULL;
                QttEffectRow **stage_rows = supplied
                    ? calloc(supplied, sizeof(*stage_rows)) : NULL;
                if (supplied && (!stage_names || !stage_rows)) {
                    free(stage_names);
                    free(stage_rows);
                    return NULL;
                }
                for (size_t i = 0; summary && i < supplied; i++) {
                    bool stage_complete = false;
                    QttEffectScheme *stage_scheme =
                        infer_scheme_arrow_effect_at(
                            callee, i, &stage_complete, NULL);
                    if (!stage_scheme) {
                        judgment->coverage_gaps |=
                            QTT_EFFECT_COVERAGE_MISSING_ARROW_CONTRACT;
                        break;
                    }
                    QttEffectRow *stage = NULL;
                    size_t callable_count =
                        qtt_effect_scheme_callable_parameter_count(
                            stage_scheme);
                    if (callable_count) {
                        QttEffectRow *replacement =
                            qtt_effect_empty(ctx->effect_arena);
                        bool substitution_ready = replacement != NULL;
                        bool arguments_complete = true;
                        for (size_t dependency = 0;
                             substitution_ready &&
                                 dependency < callable_count;
                             dependency++) {
                            size_t parameter =
                                qtt_effect_scheme_callable_parameter_index(
                                    stage_scheme, dependency);
                            AST *argument_ast = parameter < supplied
                                ? ast->list.items[parameter + 1] : NULL;
                            TypeScheme *argument_scheme = argument_ast &&
                                    argument_ast->type == AST_SYMBOL
                                ? infer_env_lookup(
                                    ctx, argument_ast->symbol)
                                : NULL;
                            size_t invoked =
                                qtt_effect_scheme_callable_parameter_arity(
                                    stage_scheme, dependency);
                            if (!argument_scheme || !invoked) {
                                substitution_ready = false;
                                break;
                            }
                            size_t argument_effect_count =
                                infer_scheme_arrow_effect_count(
                                    argument_scheme);
                            if (argument_effect_count) {
                                if (invoked > argument_effect_count) {
                                    substitution_ready = false;
                                    break;
                                }
                                for (size_t reached = 0;
                                     substitution_ready && reached < invoked;
                                     reached++) {
                                    bool argument_complete = false;
                                    QttEffectScheme *argument_stage =
                                        infer_scheme_arrow_effect_at(
                                            argument_scheme, reached,
                                            &argument_complete, NULL);
                                    QttEffectRow *instantiated =
                                        argument_stage
                                        ? qtt_effect_instantiate(
                                            ctx->effect_arena,
                                            argument_stage) : NULL;
                                    qtt_effect_scheme_free(argument_stage);
                                    replacement = infer_source_effect_join(
                                        judgment, replacement, instantiated);
                                    substitution_ready = replacement != NULL;
                                    arguments_complete = arguments_complete &&
                                        argument_complete;
                                }
                            } else {
                                QttEffectScheme *argument_effect =
                                    argument_scheme->effect_scheme;
                                if (!argument_effect) {
                                    substitution_ready = false;
                                    break;
                                }
                                QttEffectRow *instantiated =
                                    qtt_effect_instantiate(
                                        ctx->effect_arena, argument_effect);
                                replacement = infer_source_effect_join(
                                    judgment, replacement, instantiated);
                                substitution_ready = replacement != NULL;
                                arguments_complete = arguments_complete &&
                                    argument_scheme->effects_complete;
                            }
                        }
                        if (substitution_ready) {
                            stage = qtt_effect_instantiate_with_tail(
                                ctx->effect_arena, stage_scheme,
                                replacement);
                            stage_complete = stage && arguments_complete;
                        }
                    }
                    if (!stage_complete)
                        judgment->coverage_gaps |=
                            QTT_EFFECT_COVERAGE_INCOMPLETE_CALLEE;
                    if (!stage)
                        stage = qtt_effect_instantiate(
                            ctx->effect_arena, stage_scheme);
                    if (!infer_source_constrain_stage_traits(
                            judgment, callee, i, stage) ||
                        !infer_source_constrain_stage_label(
                            judgment,
                            infer_scheme_arrow_effect_name_at(callee, i),
                            stage, stage_names, stage_rows, i) ||
                        !infer_source_constrain_named_stage(
                            judgment,
                            infer_scheme_arrow_effect_name_at(callee, i),
                            stage,
                            (const char *const *)
                                judgment->returned_arrow_effect_names,
                            judgment->returned_arrow_rows,
                            judgment->returned_arrow_effect_count)) {
                        qtt_effect_scheme_free(stage_scheme);
                        free(stage_names);
                        free(stage_rows);
                        return NULL;
                    }
                    stage_names[i] =
                        infer_scheme_arrow_effect_name_at(callee, i);
                    stage_rows[i] = stage;
                    summary = infer_source_effect_join(
                        judgment, summary, stage);
                    qtt_effect_scheme_free(stage_scheme);
                }
                free(stage_names);
                free(stage_rows);
            }
        } else if (supplied < domains) {
            judgment->coverage_gaps |=
                QTT_EFFECT_COVERAGE_PARTIAL_LEGACY_CALL;
            if (!callee->effects_complete)
                judgment->coverage_gaps |=
                    QTT_EFFECT_COVERAGE_INCOMPLETE_CALLEE;
        } else {
            if (!callee->effects_complete)
                judgment->coverage_gaps |=
                    QTT_EFFECT_COVERAGE_INCOMPLETE_CALLEE;
            QttEffectRow *latent = callee->effect_scheme
                ? qtt_effect_instantiate(
                    ctx->effect_arena, callee->effect_scheme)
                : qtt_effect_empty(ctx->effect_arena);
            summary = infer_source_effect_join(
                judgment, summary, latent);
        }
        type = cursor;
        break;
    }
    case AST_LAMBDA: {
        InferEnv *child = infer_env_create_child(ctx->env);
        InferEnv *saved = ctx->env;
        ctx->env = child;
        Type **parameters = ast->lambda.param_count
            ? malloc((size_t)ast->lambda.param_count * sizeof(*parameters))
            : NULL;
        if (ast->lambda.param_count && !parameters) {
            ctx->env = saved;
            infer_env_free(child);
            return NULL;
        }
        for (int i = 0; i < ast->lambda.param_count; i++) {
            parameters[i] = infer_fresh(ctx);
            TypeScheme *parameter = scheme_mono(parameters[i]);
            QttEffectScheme *open = qtt_effect_generalize(
                ctx->effect_arena, ctx->effect_solver,
                qtt_effect_fresh(ctx->effect_arena));
            scheme_set_effect_scheme(parameter, open, false);
            qtt_effect_scheme_free(open);
            infer_env_insert(
                child, ast->lambda.params[i].name,
                parameter);
            if (i < 64) {
                judgment->lambda_parameters[i] = parameter;
                judgment->lambda_parameter_count = (size_t)i + 1;
            }
        }
        Type *return_type = infer_fresh(ctx);
        for (int i = 0; summary && i < ast->lambda.body_count; i++) {
            QttEffectRow *body_effect = NULL;
            Type *body = infer_fused_fragment(
                judgment, ast->lambda.body_exprs[i], &body_effect);
            summary = infer_source_effect_join(
                judgment, summary, body_effect);
            if (i + 1 == ast->lambda.body_count) return_type = body;
        }
        ctx->env = saved;
        infer_env_free(child);
        type = return_type;
        for (int i = ast->lambda.param_count - 1; i >= 0; i--)
            type = type_arrow(parameters[i], type);
        free(parameters);
        break;
    }
    default:
        return NULL;
    }
    ast->inferred_type = type;
    *effect = summary;
    return type;
}

static bool infer_toplevel_fused_fragment(
    InferCtx *ctx, AST *ast, InferExpressionJudgment *result) {
    QttEffectConstraintSet *constraints = qtt_effect_constraints_new();
    if (!constraints) return false;
    ctx->last_effect_constraint_count = 0;
    ctx->last_effect_certificate_fingerprint = 0;
    ctx->last_effect_constraints_residual = false;
    SourceEffectJudgment effect_judgment = {
        .ctx = ctx, .constraints = constraints,
    };
    QttEffectRow *effect = NULL;
    Type *type = infer_fused_fragment(&effect_judgment, ast, &effect);
    if (!type || ctx->had_error || !infer_unify_all(ctx)) {
        qtt_effect_constraints_free(constraints);
        infer_source_effect_clear_returned(&effect_judgment);
        result->type = type ? subst_apply(ctx->subst, type) : NULL;
        return false;
    }
    infer_zonk_ast(ctx, ast);
    result->type = subst_apply(ctx->subst, type);
    result->effects = infer_source_effect_finish(
        &effect_judgment, effect, &result->effects_complete);
    size_t lambda_domains = ast->type == AST_LAMBDA
        ? (size_t)ast->lambda.param_count : 0;
    size_t residual_domains = effect_judgment.returned_arrow_effect_count;
    size_t arrow_count = lambda_domains + residual_domains;
    if (result->effects && arrow_count) {
        result->arrow_effect_schemes = calloc(
            arrow_count, sizeof(*result->arrow_effect_schemes));
        result->arrow_effects_complete = malloc(
            arrow_count * sizeof(*result->arrow_effects_complete));
        QttEffectScheme *pure = lambda_domains > 1
            ? qtt_effect_generalize(
                ctx->effect_arena, ctx->effect_solver,
                qtt_effect_empty(ctx->effect_arena))
            : NULL;
        if (!result->arrow_effect_schemes ||
                !result->arrow_effects_complete ||
                (lambda_domains > 1 && !pure)) {
            qtt_effect_scheme_free(pure);
            infer_expression_judgment_free(result);
            return false;
        }
        for (size_t i = 0; i < lambda_domains; i++) {
            QttEffectScheme *stage = i + 1 == lambda_domains
                ? result->effects : pure;
            result->arrow_effect_schemes[i] =
                qtt_effect_scheme_retain(stage);
            result->arrow_effects_complete[i] = i + 1 == lambda_domains
                ? result->effects_complete : true;
        }
        for (size_t i = 0; i < residual_domains; i++) {
            result->arrow_effect_schemes[lambda_domains + i] =
                qtt_effect_scheme_retain(
                    effect_judgment.returned_arrow_effects[i]);
            result->arrow_effects_complete[lambda_domains + i] =
                effect_judgment.returned_arrow_effects_complete[i];
        }
        qtt_effect_scheme_free(pure);
        result->arrow_effect_count = arrow_count;
    }
    result->effect_trait_predicate_stages =
        effect_judgment.returned_effect_trait_predicate_stages;
    result->effect_trait_predicate_names =
        effect_judgment.returned_effect_trait_predicate_names;
    result->effect_trait_predicate_count =
        effect_judgment.returned_effect_trait_predicate_count;
    effect_judgment.returned_effect_trait_predicate_stages = NULL;
    effect_judgment.returned_effect_trait_predicate_names = NULL;
    effect_judgment.returned_effect_trait_predicate_count = 0;
    infer_source_effect_clear_returned(&effect_judgment);
    result->recursively_fused = result->effects != NULL;
    return result->effects != NULL;
}

static bool infer_toplevel_inline_lambda_call(
    InferCtx *ctx, AST *ast, InferExpressionJudgment *judgment,
    bool *handled) {
    *handled = false;
    if (ast->type != AST_LIST || ast->list.count < 2) return false;
    size_t inline_count = 0;
    for (size_t i = 1; i < ast->list.count; i++)
        if (ast->list.items[i]->type == AST_LAMBDA) inline_count++;
    if (!inline_count) return false;
    *handled = true;

    AST **originals = calloc(ast->list.count, sizeof(*originals));
    AST *symbols = calloc(ast->list.count, sizeof(*symbols));
    char **names = calloc(ast->list.count, sizeof(*names));
    InferEnv *saved_env = ctx->env;
    InferEnv *inline_env = infer_env_create_child(saved_env);
    bool ok = originals && symbols && names && inline_env;
    if (inline_env) ctx->env = inline_env;
    for (size_t i = 1; ok && i < ast->list.count; i++) {
        AST *argument = ast->list.items[i];
        if (argument->type != AST_LAMBDA) continue;
        InferExpressionJudgment inferred = {0};
        ok = infer_toplevel_judgment(ctx, argument, &inferred);
        TypeScheme *scheme = ok ? scheme_mono(inferred.type) : NULL;
        InferCallableContract contract = {0};
        if (ok && scheme) {
            ok = infer_callable_contract_from_judgment(
                    &contract, &inferred) &&
                scheme_set_callable_contract(scheme, &contract);
        } else {
            ok = false;
        }
        if (ok) {
            char generated[96];
            snprintf(
                generated, sizeof(generated),
                "__effect_inline_%p_%zu", (void *)argument, i);
            names[i] = strdup(generated);
            ok = names[i] != NULL;
        }
        if (ok) {
            infer_env_insert(ctx->env, names[i], scheme);
            originals[i] = argument;
            symbols[i].type = AST_SYMBOL;
            symbols[i].symbol = names[i];
            symbols[i].line = argument->line;
            symbols[i].column = argument->column;
            ast->list.items[i] = &symbols[i];
        } else {
            scheme_free(scheme);
        }
        infer_callable_contract_free(&contract);
        infer_expression_judgment_free(&inferred);
    }
    if (ok && infer_fused_fragment_supported(ctx, ast, true))
        ok = infer_toplevel_fused_fragment(ctx, ast, judgment);
    else
        ok = false;
    for (size_t i = 1; i < ast->list.count; i++) {
        if (originals && originals[i]) ast->list.items[i] = originals[i];
        free(names ? names[i] : NULL);
    }
    free(names);
    free(symbols);
    free(originals);
    ctx->env = saved_env;
    infer_env_free(inline_env);
    return ok;
}

bool infer_toplevel_judgment(
    InferCtx *ctx, AST *ast, InferExpressionJudgment *judgment) {
    if (!ctx || !ast || !judgment) return false;
    memset(judgment, 0, sizeof(*judgment));
    bool handled_inline = false;
    bool inline_result = infer_toplevel_inline_lambda_call(
        ctx, ast, judgment, &handled_inline);
    if (handled_inline) return inline_result;
    if (infer_fused_fragment_supported(ctx, ast, true))
        return infer_toplevel_fused_fragment(ctx, ast, judgment);
    judgment->type = infer_toplevel(ctx, ast);
    if (!judgment->type || ctx->had_error) return false;
    judgment->effects = ast->type == AST_LAMBDA
        ? infer_effect_scheme_for_lambda(
            ctx, ast, &judgment->effects_complete)
        : infer_effect_scheme_for_expression(
            ctx, ast, &judgment->effects_complete);
    if (!judgment->effects) {
        judgment->type = NULL;
        return false;
    }
    return true;
}

void infer_expression_judgment_free(InferExpressionJudgment *judgment) {
    if (!judgment) return;
    for (size_t i = 0; i < judgment->arrow_effect_count; i++)
        qtt_effect_scheme_free(judgment->arrow_effect_schemes[i]);
    free(judgment->arrow_effect_schemes);
    free(judgment->arrow_effects_complete);
    for (size_t i = 0; i < judgment->effect_trait_predicate_count; i++)
        free(judgment->effect_trait_predicate_names[i]);
    free(judgment->effect_trait_predicate_names);
    free(judgment->effect_trait_predicate_stages);
    qtt_effect_scheme_free(judgment->effects);
    memset(judgment, 0, sizeof(*judgment));
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
