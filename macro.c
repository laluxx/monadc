#include "macro.h"
#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/// Utilities

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    memcpy(r, s, n);
    return r;
}

/* Global gensym counter — never resets so every expansion site gets
 * a globally unique suffix even across multiple calls to macro_expand_all. */
static unsigned long g_macro_gensym = 0;

/* Produce "macroname__varname__N" */
static char *gensym(const char *macro_name, const char *var_name) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s__%s__%lu",
             macro_name ? macro_name : "mac",
             var_name   ? var_name   : "v",
             g_macro_gensym++);
    return xstrdup(buf);
}

/// BindTable
//
// pattern variable -> bound AST node (or splice)
// Each binding maps a pattern-variable name to either:
//   - a single AST node  (is_splice == false): ordinary parameter
//   - an array of nodes  (is_splice == true):  rest/variadic parameter
//  * The nodes are *not* owned by the BindTable — they point into the
// original call-site argument list and are cloned during substitution.
//
typedef struct {
    char  *name;
    bool   is_splice;
    AST   *value;        // single value when !is_splice
    AST  **splice;       // pointer array when is_splice (not owned)
    int    splice_count;
} Binding;

typedef struct {
    Binding *entries;
    int      count;
    int      cap;
} BindTable;

static void bt_init(BindTable *bt) {
    bt->entries = NULL;
    bt->count   = 0;
    bt->cap     = 0;
}

static void bt_free(BindTable *bt) {
    for (int i = 0; i < bt->count; i++) {
        free(bt->entries[i].name);
        if (bt->entries[i].is_splice)
            free(bt->entries[i].splice); /* free the pointer array, not the nodes */
    }
    free(bt->entries);
}

static void bt_push(BindTable *bt, const char *name, AST *value) {
    if (bt->count >= bt->cap) {
        bt->cap = bt->cap ? bt->cap * 2 : 8;
        bt->entries = realloc(bt->entries, sizeof(Binding) * bt->cap);
    }
    Binding *b   = &bt->entries[bt->count++];
    b->name         = xstrdup(name);
    b->is_splice    = false;
    b->value        = value;
    b->splice       = NULL;
    b->splice_count = 0;
}

static void bt_push_splice(BindTable *bt, const char *name,
                            AST **nodes, int n) {
    if (bt->count >= bt->cap) {
        bt->cap = bt->cap ? bt->cap * 2 : 8;
        bt->entries = realloc(bt->entries, sizeof(Binding) * bt->cap);
    }
    /* Copy the pointer array so the BindTable owns it */
    AST **copy = malloc(sizeof(AST*) * (n ? n : 1));
    for (int i = 0; i < n; i++) copy[i] = nodes[i];

    Binding *b   = &bt->entries[bt->count++];
    b->name         = xstrdup(name);
    b->is_splice    = true;
    b->value        = NULL;
    b->splice       = copy;
    b->splice_count = n;
}

static Binding *bt_lookup(BindTable *bt, const char *name) {
    for (int i = 0; i < bt->count; i++)
        if (strcmp(bt->entries[i].name, name) == 0)
            return &bt->entries[i];
    return NULL;
}

/// RenameTable
//
// introduced-name -> fresh hygienic name
//
typedef struct RenameEntry {
    char *orig;
    char *fresh;
    struct RenameEntry *next;
} RenameEntry;

typedef struct {
    RenameEntry *head;
} RenameTable;

static void rt_init(RenameTable *rt)  { rt->head = NULL; }

static void rt_free(RenameTable *rt) {
    RenameEntry *e = rt->head;
    while (e) {
        RenameEntry *next = e->next;
        free(e->orig);
        free(e->fresh);
        free(e);
        e = next;
    }
    rt->head = NULL;
}

/* Look up a fresh name for 'orig'; create one if not seen yet. */
static const char *rt_rename(RenameTable *rt, const char *orig,
                              const char *macro_name) {
    for (RenameEntry *e = rt->head; e; e = e->next)
        if (strcmp(e->orig, orig) == 0)
            return e->fresh;

    RenameEntry *e = malloc(sizeof(RenameEntry));
    e->orig  = xstrdup(orig);
    e->fresh = gensym(macro_name, orig);  // "macroname__varname__N"
    e->next  = rt->head;
    rt->head = e;
    return e->fresh;
}

static const char *rt_lookup(RenameTable *rt, const char *name) {
    for (RenameEntry *e = rt->head; e; e = e->next)
        if (strcmp(e->orig, name) == 0)
            return e->fresh;
    return NULL;
}

/// MacroRegistry
//
// A MacroDef stores everything needed to expand one call site:
//   - the parameter list (copied from the lambda)
//   - the body AST       (pointer into the lambda; lambda is kept alive
//                          in g_macro_asts[] so the pointer stays valid)
//
typedef struct {
    char     *name;
    ASTParam *params;
    int       param_count;
    AST      *body;       // NOT owned — points into a live lambda node
} MacroDef;

#define MACRO_BUCKETS 64

typedef struct MacroEntry {
    MacroDef           def;
    struct MacroEntry *next;
} MacroEntry;

typedef struct {
    MacroEntry *buckets[MACRO_BUCKETS];
    /* Keep the original lambda AST nodes alive so body pointers stay valid */
    AST       **lambdas;
    int         lambda_count;
    int         lambda_cap;
} MacroRegistry;

static MacroRegistry g_reg = {0};

static unsigned int macro_hash(const char *s) {
    unsigned int h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h % MACRO_BUCKETS;
}

void macro_clear(void) {
    for (int i = 0; i < MACRO_BUCKETS; i++) {
        MacroEntry *e = g_reg.buckets[i];
        while (e) {
            MacroEntry *next = e->next;
            free(e->def.name);
            for (int j = 0; j < e->def.param_count; j++) {
                free(e->def.params[j].name);
                free(e->def.params[j].type_name);
            }
            free(e->def.params);
            free(e);
            e = next;
        }
        g_reg.buckets[i] = NULL;
    }
    /* Free the kept lambda ASTs */
    for (int i = 0; i < g_reg.lambda_count; i++)
        ast_free(g_reg.lambdas[i]);
    free(g_reg.lambdas);
    g_reg.lambdas      = NULL;
    g_reg.lambda_count = 0;
    g_reg.lambda_cap   = 0;
}

static void registry_keep_lambda(AST *lam) {
    if (g_reg.lambda_count >= g_reg.lambda_cap) {
        g_reg.lambda_cap = g_reg.lambda_cap ? g_reg.lambda_cap * 2 : 8;
        g_reg.lambdas = realloc(g_reg.lambdas, sizeof(AST*) * g_reg.lambda_cap);
    }
    g_reg.lambdas[g_reg.lambda_count++] = lam;
}

static void registry_add(const char *name, AST *lambda) {
    if (!lambda || lambda->type != AST_LAMBDA) return;

    /* Copy params so the MacroDef is self-contained */
    int n = lambda->lambda.param_count;
    ASTParam *params = malloc(sizeof(ASTParam) * (n ? n : 1));
    for (int i = 0; i < n; i++) {
        params[i].name      = xstrdup(lambda->lambda.params[i].name);
        params[i].type_name = xstrdup(lambda->lambda.params[i].type_name);
        params[i].is_rest   = lambda->lambda.params[i].is_rest;
        params[i].is_anon   = lambda->lambda.params[i].is_anon;
    }

    unsigned int h = macro_hash(name);
    MacroEntry *e  = malloc(sizeof(MacroEntry));
    e->def.name        = xstrdup(name);
    e->def.params      = params;
    e->def.param_count = n;
    e->def.body        = lambda->lambda.body; // pointer into lambda; lambda kept alive
    e->next            = g_reg.buckets[h];
    g_reg.buckets[h]   = e;

    /* Keep the lambda alive so body stays valid */
    registry_keep_lambda(lambda);
    fprintf(stderr, "[macro] registered '%s' (%d param%s%s)\n",
            name, n, n == 1 ? "" : "s",
            (n > 0 && params[n-1].is_rest) ? " + rest" : "");
}

static MacroDef *registry_lookup(const char *name) {
    unsigned int h = macro_hash(name);
    for (MacroEntry *e = g_reg.buckets[h]; e; e = e->next)
        if (strcmp(e->def.name, name) == 0)
            return &e->def;
    return NULL;
}

int macro_is_registered(const char *name) {
    return registry_lookup(name) != NULL ? 1 : 0;
}

/// Introduced-name scan
//
// Collect every name *introduced* by a macro body — lambda params,
// let-binding names, inner define names — that is NOT a pattern variable
// (i.e. not in bt).  These names get renamed for hygiene.
//
// We only scan one level of introduced binders; nested lambdas / lets
// introduce their own scopes and are handled recursively by subst().
//
static void collect_introduced(AST *node, BindTable *bt,
                                char ***out, int *count, int *cap) {
    if (!node) return;

    /* Add 'name' to *out if it is not a pattern variable and not already listed */
    #define MAYBE_ADD(name) do { \
        const char *_n = (name); \
        if (!_n) break; \
        if (bt_lookup(bt, _n)) break; \
        bool _found = false; \
        for (int _i = 0; _i < *count; _i++) \
            if (strcmp((*out)[_i], _n) == 0) { _found = true; break; } \
        if (_found) break; \
        if (*count >= *cap) { \
            *cap = *cap ? *cap * 2 : 8; \
            *out = realloc(*out, sizeof(char*) * *cap); \
        } \
        (*out)[(*count)++] = xstrdup(_n); \
    } while(0)

    switch (node->type) {

    case AST_LAMBDA:
        for (int i = 0; i < node->lambda.param_count; i++)
            MAYBE_ADD(node->lambda.params[i].name);
        for (int i = 0; i < node->lambda.body_count; i++)
            collect_introduced(node->lambda.body_exprs[i], bt, out, count, cap);
        break;

    case AST_LIST: {
        if (node->list.count < 1) break;
        AST *head = node->list.items[0];

        /* (define name …) — name is introduced */
        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "define") == 0 &&
            node->list.count >= 2) {
            AST *target = node->list.items[1];
            if (target->type == AST_SYMBOL)
                MAYBE_ADD(target->symbol);
            else if (target->type == AST_LIST && target->list.count >= 1 &&
                     target->list.items[0]->type == AST_SYMBOL)
                MAYBE_ADD(target->list.items[0]->symbol);
        }

        /* (let ([x …] …) …) — binding names are introduced.
         * The reader desugars let into a lambda call, so at AST level
         * we also see the desugared form; handle both. */
        if (head->type == AST_SYMBOL &&
            strcmp(head->symbol, "let") == 0 &&
            node->list.count >= 2) {
            AST *binds = node->list.items[1];
            if (binds->type == AST_LIST)
                for (size_t i = 0; i < binds->list.count; i++) {
                    AST *pair = binds->list.items[i];
                    if (pair->type == AST_LIST && pair->list.count >= 1 &&
                        pair->list.items[0]->type == AST_SYMBOL)
                        MAYBE_ADD(pair->list.items[0]->symbol);
                }
        }

        /* Recurse into all children */
        for (size_t i = 0; i < node->list.count; i++)
            collect_introduced(node->list.items[i], bt, out, count, cap);
        break;
    }

    default:
        break;
    }

    #undef MAYBE_ADD
}

/// Substitution engine
//
// subst(node, bt, rt, macro_name)
//
// Deep-clone `node` with three simultaneous rewrites:
//   1. Pattern-variable symbols → clone of the bound call-site AST
//   2. Splice bindings in list position → flat insertion of all nodes
//   3. Introduced names (in rt) → their fresh hygienic names
//
// #(…) quasi-output: a list whose first element is the symbol "#" is
// treated as an output marker; only the second element is returned
// (with full substitution applied).  This strips the #() wrapper and
// gives the macro output subtree.
//
// Forward declaration
static AST *subst(AST *node, BindTable *bt, RenameTable *rt,
                  const char *macro_name);

static AST *subst_list(AST *tmpl, BindTable *bt, RenameTable *rt,
                        const char *macro_name) {
    /* #(output) quasi-output marker: strip wrapper, return substituted child */
    if (tmpl->list.count >= 2) {
        AST *h = tmpl->list.items[0];
        if (h->type == AST_SYMBOL && strcmp(h->symbol, "#") == 0)
            return subst(tmpl->list.items[1], bt, rt, macro_name);
    }

    AST *result = ast_new_list();
    result->line       = tmpl->line;
    result->column     = tmpl->column;
    result->end_column = tmpl->end_column;

    for (size_t i = 0; i < tmpl->list.count; i++) {
        AST *elem = tmpl->list.items[i];

        /* Dot-splice: (head . rest-var) where rest-var is a splice binding.
         * Handles macro templates like (begin . body) → (begin e1 e2 ...) */
        if (elem->type == AST_SYMBOL && strcmp(elem->symbol, ".") == 0
            && i + 1 < tmpl->list.count) {
            AST *next = tmpl->list.items[i + 1];
            if (next->type == AST_SYMBOL) {
                Binding *b = bt_lookup(bt, next->symbol);
                if (b && b->is_splice) {
                    for (int j = 0; j < b->splice_count; j++)
                        ast_list_append(result, ast_clone(b->splice[j]));
                    i++; /* skip the splice-var too */
                    continue;
                }
            }
        }

        /* Check for a splice binding in list-element position */
        if (elem->type == AST_SYMBOL) {
            Binding *b = bt_lookup(bt, elem->symbol);
            if (b && b->is_splice) {
                /* Insert all captured nodes flat */
                for (int j = 0; j < b->splice_count; j++)
                    ast_list_append(result, ast_clone(b->splice[j]));
                continue;
            }
        }

        ast_list_append(result, subst(elem, bt, rt, macro_name));
    }
    return result;
}

static AST *subst(AST *node, BindTable *bt, RenameTable *rt,
                  const char *macro_name) {
    if (!node) return NULL;

    switch (node->type) {

    /* ---- Symbol: three-way lookup --------------------------------- */
    case AST_SYMBOL: {
        /* 1. Pattern variable → clone the bound call-site node */
        Binding *b = bt_lookup(bt, node->symbol);
        if (b) {
            if (b->is_splice) {
                /* Splice in non-list position: wrap in (begin …) unless single */
                if (b->splice_count == 1)
                    return ast_clone(b->splice[0]);
                AST *begin = ast_new_list();
                ast_list_append(begin, ast_new_symbol("begin"));
                for (int j = 0; j < b->splice_count; j++)
                    ast_list_append(begin, ast_clone(b->splice[j]));
                begin->line   = node->line;
                begin->column = node->column;
                return begin;
            }
            return ast_clone(b->value);
        }
        /* 2. Introduced name → hygienic rename */
        const char *fresh = rt_lookup(rt, node->symbol);
        if (fresh) {
            AST *r = ast_new_symbol(fresh);
            r->line   = node->line;
            r->column = node->column;
            return r;
        }
        /* 3. Free reference → pass through */
        return ast_clone(node);
    }

    /* ---- List: handle splice + #() -------------------------------- */
    case AST_LIST:
        return subst_list(node, bt, rt, macro_name);

    /* ---- Lambda: rename introduced params, substitute body -------- */
    case AST_LAMBDA: {
        /* Shallow copy first so all scalar fields are right */
        AST *lam = calloc(1, sizeof(AST));
        *lam = *node;
        lam->literal_str = node->literal_str ? xstrdup(node->literal_str) : NULL;

        /* Params: rename any that appear in the rename table */
        lam->lambda.params = malloc(sizeof(ASTParam)
                             * (node->lambda.param_count ? node->lambda.param_count : 1));
        for (int i = 0; i < node->lambda.param_count; i++) {
            ASTParam *op = &node->lambda.params[i];
            ASTParam *np = &lam->lambda.params[i];
            const char *fresh = rt_lookup(rt, op->name);
            np->name      = fresh ? xstrdup(fresh) : xstrdup(op->name);
            np->type_name = op->type_name ? xstrdup(op->type_name) : NULL;
            np->is_rest   = op->is_rest;
            np->is_anon   = op->is_anon;
        }

        lam->lambda.return_type = node->lambda.return_type
                                ? xstrdup(node->lambda.return_type) : NULL;
        lam->lambda.docstring   = node->lambda.docstring
                                ? xstrdup(node->lambda.docstring)   : NULL;
        lam->lambda.alias_name  = node->lambda.alias_name
                                ? xstrdup(node->lambda.alias_name)  : NULL;

        /* Body expressions */
        int bc = node->lambda.body_count;
        lam->lambda.body_exprs = malloc(sizeof(AST*) * (bc ? bc : 1));
        for (int i = 0; i < bc; i++)
            lam->lambda.body_exprs[i] = subst(node->lambda.body_exprs[i],
                                               bt, rt, macro_name);
        lam->lambda.body_count = bc;
        lam->lambda.body = bc > 0 ? lam->lambda.body_exprs[bc - 1] : NULL;
        return lam;
    }

    /* ---- Containers: recurse -------------------------------------- */
    case AST_ARRAY: {
        AST *arr = ast_new_array();
        arr->line = node->line; arr->column = node->column;
        for (size_t i = 0; i < node->array.element_count; i++)
            ast_array_append(arr, subst(node->array.elements[i], bt, rt, macro_name));
        return arr;
    }

    case AST_SET: {
        AST *s = ast_new_set();
        s->line = node->line; s->column = node->column;
        for (size_t i = 0; i < node->set.element_count; i++) {
            AST *e = subst(node->set.elements[i], bt, rt, macro_name);
            if (s->set.element_count >= s->set.element_capacity) {
                s->set.element_capacity *= 2;
                s->set.elements = realloc(s->set.elements,
                    sizeof(AST*) * s->set.element_capacity);
            }
            s->set.elements[s->set.element_count++] = e;
        }
        return s;
    }

    case AST_MAP: {
        AST *m = ast_new_map();
        m->line = node->line; m->column = node->column;
        for (size_t i = 0; i < node->map.count; i++) {
            AST *k = subst(node->map.keys[i], bt, rt, macro_name);
            AST *v = subst(node->map.vals[i], bt, rt, macro_name);
            if (m->map.count >= m->map.capacity) {
                m->map.capacity *= 2;
                m->map.keys = realloc(m->map.keys, sizeof(AST*) * m->map.capacity);
                m->map.vals = realloc(m->map.vals, sizeof(AST*) * m->map.capacity);
            }
            m->map.keys[m->map.count] = k;
            m->map.vals[m->map.count] = v;
            m->map.count++;
        }
        return m;
    }

    case AST_RANGE:
        return ast_new_range(
            subst(node->range.start, bt, rt, macro_name),
            subst(node->range.step,  bt, rt, macro_name),
            subst(node->range.end,   bt, rt, macro_name),
            node->range.is_array);

    case AST_REFINEMENT: {
        AST *r = calloc(1, sizeof(AST));
        *r = *node;
        r->refinement.name       = xstrdup(node->refinement.name);
        r->refinement.var        = xstrdup(node->refinement.var);
        r->refinement.base_type  = xstrdup(node->refinement.base_type);
        r->refinement.docstring  = xstrdup(node->refinement.docstring);
        r->refinement.alias_name = xstrdup(node->refinement.alias_name);
        r->refinement.predicate  = subst(node->refinement.predicate,
                                         bt, rt, macro_name);
        return r;
    }

    /* ---- Atoms: straight clone ------------------------------------ */
    default:
        return ast_clone(node);
    }
}

/// Argument binding
//
// Bind call-site arguments args[0..argc-1] to the macro's parameter list.
// Fixed params get individual bindings; the rest param (if any) gets a
// splice binding covering all remaining args.
// Returns false and prints an error on arity mismatch.
//
static bool bind_args(MacroDef *def, AST **args, int argc, BindTable *bt) {
    /* Split params into fixed and optional rest */
    int  fixed   = 0;
    int  rest_at = -1;
    for (int i = 0; i < def->param_count; i++) {
        if (def->params[i].is_rest) { rest_at = i; break; }
        fixed++;
    }

    if (argc < fixed) {
        fprintf(stderr,
                "[macro] '%s': expected at least %d arg(s), got %d\n",
                def->name, fixed, argc);
        return false;
    }
    if (rest_at < 0 && argc != fixed) {
        fprintf(stderr,
                "[macro] '%s': expected exactly %d arg(s), got %d\n",
                def->name, fixed, argc);
        return false;
    }

    /* Bind fixed params */
    for (int i = 0; i < fixed; i++)
        bt_push(bt, def->params[i].name, args[i]);

    /* Bind rest param as splice */
    if (rest_at >= 0) {
        const char *rest_name = def->params[rest_at].name;
        int n_rest = argc - fixed;
        bt_push_splice(bt, rest_name, args + fixed, n_rest);
    }

    return true;
}

/// Expansion engine

/* Forward declaration */
static AST *expand_node(AST *node, bool *changed);

/*
 * try_expand(node, changed)
 *
 * If node is a list call whose head is a registered macro, expand it:
 *   1. bind call-site args → BindTable
 *   2. collect introduced names → RenameTable
 *   3. subst() the body
 *   4. free the original call node (carefully — args are referenced by bt)
 *
 * Returns the expansion result (a new AST).
 * Returns node unchanged if it's not a macro call.
 */
static AST *try_expand(AST *node, bool *changed) {
    if (!node || node->type != AST_LIST || node->list.count < 1)
        return node;

    AST *head = node->list.items[0];
    if (!head || head->type != AST_SYMBOL)
        return node;

    MacroDef *def = registry_lookup(head->symbol);
    if (!def) return node;

    int   argc = (int)node->list.count - 1;
    AST **args = node->list.items + 1;   /* pointers into node's items array */

    /* §7 — bind args */
    BindTable bt;
    bt_init(&bt);
    if (!bind_args(def, args, argc, &bt)) {
        bt_free(&bt);
        return node;
    }

    /* §5 — collect introduced names */
    char **intro     = NULL;
    int    intro_n   = 0;
    int    intro_cap = 0;
    collect_introduced(def->body, &bt, &intro, &intro_n, &intro_cap);

    /* §3 — build rename table from collected names */
    RenameTable rt;
    rt_init(&rt);
    for (int i = 0; i < intro_n; i++) {
        rt_rename(&rt, intro[i], def->name);
        free(intro[i]);
    }
    free(intro);

    /* §6 — substitute */
    AST *result = subst(def->body, &bt, &rt, def->name);

    bt_free(&bt);
    rt_free(&rt);

    /*
     * Free the call-site node carefully.
     * The items array contains:
     *   items[0] = head symbol  → free normally via ast_free
     *   items[1..] = args       → they have been *cloned* inside subst(),
     *                             so we must free the originals too.
     * We free the whole node via ast_free which recursively frees children.
     */
    ast_free(node);

    *changed = true;
    fprintf(stderr, "[macro] expanded '%s'\n", def->name);
    return result;
}

/*
 * expand_node(node, changed)
 *
 * Recursively walk node, expanding any macro call sites found.
 * May return a different pointer if the node itself was a macro call.
 * Mutates list/lambda children in place for everything else.
 */
static AST *expand_node(AST *node, bool *changed) {
    if (!node) return NULL;

    /* Try expanding this node first; if it was a macro call we get
     * a fresh node back and immediately recurse into it. */
    AST *expanded = try_expand(node, changed);
    if (expanded != node)
        return expand_node(expanded, changed);

    /* Not a macro call — descend into children */
    switch (node->type) {

    case AST_LIST:
        for (size_t i = 0; i < node->list.count; i++)
            node->list.items[i] = expand_node(node->list.items[i], changed);
        break;

    case AST_LAMBDA:
        for (int i = 0; i < node->lambda.body_count; i++)
            node->lambda.body_exprs[i] =
                expand_node(node->lambda.body_exprs[i], changed);
        if (node->lambda.body_count > 0)
            node->lambda.body =
                node->lambda.body_exprs[node->lambda.body_count - 1];
        break;

    case AST_ARRAY:
        for (size_t i = 0; i < node->array.element_count; i++)
            node->array.elements[i] =
                expand_node(node->array.elements[i], changed);
        break;

    case AST_SET:
        for (size_t i = 0; i < node->set.element_count; i++)
            node->set.elements[i] =
                expand_node(node->set.elements[i], changed);
        break;

    case AST_MAP:
        for (size_t i = 0; i < node->map.count; i++) {
            node->map.keys[i] = expand_node(node->map.keys[i], changed);
            node->map.vals[i] = expand_node(node->map.vals[i], changed);
        }
        break;

    case AST_RANGE:
        node->range.start = expand_node(node->range.start, changed);
        node->range.step  = expand_node(node->range.step,  changed);
        node->range.end   = expand_node(node->range.end,   changed);
        break;

    case AST_REFINEMENT:
        node->refinement.predicate =
            expand_node(node->refinement.predicate, changed);
        break;

    case AST_TESTS:
        for (int i = 0; i < node->tests.count; i++)
            node->tests.assertions[i] =
                expand_node(node->tests.assertions[i], changed);
        break;

    default:
        break;
    }

    return node;
}

/// Public API

/*
 * is_syntax_return(ret_type)
 *
 * A lambda is a macro if its return_type string contains "Syntax".
 * We accept "Syntax", "-> Syntax", "Pointer :: Syntax", etc.
 */
static bool is_syntax_return(const char *ret_type) {
    if (!ret_type) return false;
    return strstr(ret_type, "Syntax") != NULL;
}

/*
 * is_macro_define(node, out_name, out_lambda)
 *
 * Returns true iff node is (define <symbol> <lambda-with-Syntax-return>).
 * Fills *out_name and *out_lambda on success.
 */
static bool is_macro_define(AST *node, const char **out_name, AST **out_lambda) {
    if (!node || node->type != AST_LIST || node->list.count < 3) return false;

    AST *head = node->list.items[0];
    if (!head || head->type != AST_SYMBOL) return false;
    if (strcmp(head->symbol, "define") != 0) return false;

    AST *name_node = node->list.items[1];
    AST *val_node  = node->list.items[2];

    if (!name_node || name_node->type != AST_SYMBOL) return false;
    if (!val_node  || val_node->type  != AST_LAMBDA) return false;
    if (!is_syntax_return(val_node->lambda.return_type)) return false;

    *out_name   = name_node->symbol;
    *out_lambda = val_node;
    return true;
}

ASTList macro_expand_all(AST **exprs, size_t count) {
    /* ---- Phase 1: register macros, remove their defines from output ---- */
    AST  **kept       = malloc(sizeof(AST*) * (count ? count : 1));
    size_t kept_count = 0;

    for (size_t i = 0; i < count; i++) {
        const char *mac_name = NULL;
        AST        *mac_lam  = NULL;

        if (is_macro_define(exprs[i], &mac_name, &mac_lam)) {
            /*
             * Steal the lambda node out of the define before freeing it.
             * registry_add keeps the lambda alive in g_reg.lambdas[].
             * We then free the define list and the name symbol, but NOT
             * the lambda (registry_add took ownership).
             */
            AST *define_node = exprs[i];

            /* Register first, then detach and free the shell */
            registry_add(mac_name, mac_lam);

            /* Detach lambda from define so ast_free doesn't kill it */
            define_node->list.items[2] = NULL;

            /* Free the define shell (items[2] is now NULL so lambda is safe) */
            /* Also NULL out items[1] (the name symbol) to be safe */
            define_node->list.items[1] = NULL;
            ast_free(define_node);
        } else {
            kept[kept_count++] = exprs[i];
        }
    }

    /* ---- Phase 2: expand to fixpoint ---- */
    int  rounds  = 0;
    bool changed = true;
    while (changed && rounds < 64) {
        changed = false;
        for (size_t i = 0; i < kept_count; i++)
            kept[i] = expand_node(kept[i], &changed);
        rounds++;
    }

    if (rounds >= 64) {
        fprintf(stderr,
                "[macro] error: macro expansion did not reach fixpoint after "
                "64 rounds — recursive macro detected, aborting\n");
        exit(1);
    }

    ASTList result;
    result.exprs = kept;
    result.count = kept_count;
    return result;
}
