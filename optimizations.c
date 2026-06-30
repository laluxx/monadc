/*  optimizations.c — AST Optimization Pipeline
 *
 *  Pipeline shape (per fixed-point pass):
 *    1.  collect_constants      — scan top-level "::" bindings
 *    2.  optimize_expr          — bottom-up rewrite of every expression
 *    3.  repeat until !changed or pass budget exhausted
 *
 *  Once per call to optimize_ast_list (not per pass):
 *    · opt_data_registry_build  — scan top-level (data …) declarations
 *    · pmatch analysis          — reachability + exhaustiveness diagnostics
 *
 *  Memory ownership:
 *    Every AST node replaced by replace_node() is freed via ast_free().
 *    Newly produced nodes (number_ast, bool_ast, string_ast, ast_clone)
 *    are owned by their parent list or by the ASTList.  The constant
 *    table owns clones of each registered value.  The data registry owns
 *    its own strdup'd strings, independent of the AST it was built from.
 *
 *  Exhaustiveness of optimize_expr:
 *    optimize_expr's switch statement intentionally has no `default:`
 *    case.  Every ASTType declared in reader.h has an explicit case,
 *    even when that case is a documented no-op (e.g. AST_PATH).  This
 *    means that if reader.h gains a new ASTType, the switch becomes
 *    non-exhaustive and -Wswitch (enabled by most C compilers for enums)
 *    will flag it here, rather than the new node type silently skipping
 *    every pass.  Do not add a default case to this switch.
 *
 *  Thread safety: not thread-safe.  The global snapshot pointer
 *  (g_last_snapshot) is process-global and must be used from one thread.
 */

/// TODO [0/1]
// - [ ] Do semantical analysis to make sure that ADT and layouts
//       are covered in their entirety when pattern matched against.

#define _POSIX_C_SOURCE 200809L
#include "optimizations.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/// Internal data structures
//
//  ConstantEntry — one name → literal value mapping.
//  ConstantTable — flat growable array of entries.
//  NameStack     — shadow stack: names that block constant lookup while
//                  the optimizer is inside a binding scope (lambda params).
//

typedef struct ConstantEntry {
    char *name;   /* owned, strdup'd */
    AST  *value;  /* owned, ast_clone'd; always is_literal() */
} ConstantEntry;

typedef struct ConstantTable {
    ConstantEntry *items;
    size_t         count;
    size_t         capacity;
} ConstantTable;

typedef struct NameStack {
    char **items;
    size_t count;
    size_t capacity;
} NameStack;


/// Top-level optimizer state
//
//  One Optimizer is stack-allocated per optimize_ast_list call and is
//  threaded through all recursive helpers via a pointer.
//
typedef struct Optimizer {
    ConstantTable               constants;
    NameStack                   shadows;
    OptimizationStats          *stats;
    const OptimizationOptions  *options;
    const OptDataType          *data_types;   /* borrowed; may be NULL */
    size_t                      data_type_count;
    bool                        changed;       /* any rewrite fired this pass */
} Optimizer;


/// Global snapshot (last run)

static OptConstantSnapshot *g_last_snapshot = NULL;


/// Public: optimization_options_default

OptimizationOptions optimization_options_default(void) {
    OptimizationOptions o = {0};
    o.level              = OPT_LEVEL_NONE;
    o.print_stats        = false;
    o.trace_semantic     = false;
    o.fold_strings       = false;
    o.warn_unreachable   = false;
    o.warn_nonexhaustive = false;
    o.source_name        = NULL;
    return o;
}


/// Public: optimization_level_name

const char *optimization_level_name(OptimizationLevel level) {
    switch (level) {
    case OPT_LEVEL_NONE:       return "none";
    case OPT_LEVEL_BASIC:      return "basic";
    case OPT_LEVEL_AGGRESSIVE: return "aggressive";
    }
    return "unknown";
}


/// Public: opt_stats_print / opt_stats_reset

void opt_stats_print(const OptimizationStats *stats,
                     const OptimizationOptions *options,
                     FILE *fp) {
    if (!stats || !fp) return;
    fprintf(fp,
        "[opt] level=%-10s passes=%zu constants=%zu propagated=%zu "
        "folded=%zu branches=%zu strings=%zu identities=%zu "
        "ctor_eq=%zu switch_candidates=%zu "
        "pmatch_clauses=%zu unreachable=%zu nonexhaustive=%zu "
        "tail_calls=%zu\n",
        options ? optimization_level_name(options->level) : "?",
        stats->passes_run,
        stats->constants_registered,
        stats->constants_propagated,
        stats->expressions_folded,
        stats->branches_folded,
        stats->strings_folded,
        stats->identities_elided,
        stats->ctor_equalities_folded,
        stats->switch_candidates,
        stats->pmatch_clauses_seen,
        stats->pmatch_unreachable,
        stats->pmatch_nonexhaustive,
        stats->tail_calls_marked);
}

void opt_stats_reset(OptimizationStats *stats) {
    if (stats) memset(stats, 0, sizeof *stats);
}


/// AST helpers — predicates and constructors

static bool is_symbol(AST *ast, const char *name) {
    return ast && ast->type == AST_SYMBOL && strcmp(ast->symbol, name) == 0;
}

static bool is_literal(AST *ast) {
    if (!ast) return false;
    return ast->type == AST_NUMBER  ||
           ast->type == AST_STRING  ||
           ast->type == AST_CHAR    ||
           ast->type == AST_KEYWORD ||
           ast->type == AST_RATIO   ||
           is_symbol(ast, "True")   ||
           is_symbol(ast, "False")  ||
           is_symbol(ast, "nil");
}

/*  is_falsey: if the truth value of ast is statically known, set *known =
 *  true and return whether the value is falsey (False, nil, or 0.0).
 *  If the value cannot be determined statically, set *known = false.
 */
static bool is_falsey(AST *ast, bool *known) {
    *known = true;
    if (is_symbol(ast, "False") || is_symbol(ast, "nil")) return true;
    if (is_symbol(ast, "True"))  return false;
    if (ast && ast->type == AST_NUMBER) return ast->number == 0.0;
    *known = false;
    return false;
}

static bool number_is_integer(double value) {
    return isfinite(value) && value == floor(value);
}

static AST *number_ast(double value) {
    char buf[64];
    if (number_is_integer(value))
        snprintf(buf, sizeof buf, "%.0f", value);
    else
        snprintf(buf, sizeof buf, "%.17g", value);
    return ast_new_number(value, buf);
}

static AST *bool_ast(bool value) {
    return ast_new_symbol(value ? "True" : "False");
}

/*  string_ast: construct a new AST_STRING node from a C string.
 *  str may be NULL, which produces an empty-string node.
 */
static AST *string_ast(const char *str) {
    return ast_new_string(str ? str : "");
}

/*  ast_is_pure: conservative purity check.
 *  Returns true if ast has no observable side effects and is safe to
 *  duplicate or discard.  Literals and symbols are always pure.
 */
static bool ast_is_pure(AST *ast) {
    if (!ast) return true;
    switch (ast->type) {
    case AST_NUMBER:
    case AST_STRING:
    case AST_PATH:
    case AST_CHAR:
    case AST_KEYWORD:
    case AST_RATIO:
    case AST_SYMBOL:
        return true;
    case AST_ARRAY:
        for (size_t i = 0; i < ast->array.element_count; i++)
            if (!ast_is_pure(ast->array.elements[i])) return false;
        return true;
    default:
        return false;
    }
}


/// Public: ast_node_count

int ast_node_count(AST *t) {
    if (!t) return 0;
    int n = 1;
    switch (t->type) {
    case AST_LIST:
        for (size_t i = 0; i < t->list.count; i++)
            n += ast_node_count(t->list.items[i]);
        break;
    case AST_ARRAY:
        for (size_t i = 0; i < t->array.element_count; i++)
            n += ast_node_count(t->array.elements[i]);
        break;
    case AST_MAP:
        for (size_t i = 0; i < t->map.count; i++) {
            n += ast_node_count(t->map.keys[i]);
            n += ast_node_count(t->map.vals[i]);
        }
        break;
    case AST_SET:
        for (size_t i = 0; i < t->set.element_count; i++)
            n += ast_node_count(t->set.elements[i]);
        break;
    case AST_LAMBDA:
        for (int i = 0; i < t->lambda.body_count; i++)
            n += ast_node_count(t->lambda.body_exprs[i]);
        break;
    case AST_TESTS:
        for (int i = 0; i < t->tests.count; i++)
            n += ast_node_count(t->tests.assertions[i]);
        break;
    case AST_RANGE:
        n += ast_node_count(t->range.start);
        n += ast_node_count(t->range.step);
        n += ast_node_count(t->range.end);
        break;
    case AST_REFINEMENT:
        n += ast_node_count(t->refinement.predicate);
        break;
    case AST_ASM:
        for (size_t i = 0; i < t->asm_block.instruction_count; i++)
            n += ast_node_count(t->asm_block.instructions[i]);
        break;
    default:
        break;
    }
    return n;
}


/// Public: opt_fv_count
//
//  Count free occurrences of name in ast, respecting lambda binders.
//  This is a syntactic approximation: it walks every rewritable subterm
//  but does not attempt full scope resolution for every AST variant
//  (pmatch pattern variables, for instance, are not tracked as binders
//  here — callers analyzing pmatch bodies should account for that
//  separately, since pattern-bound names are a distinct concept from
//  the lambda/top-level names this query targets).
//

int opt_fv_count(AST *ast, const char *name) {
    if (!ast || !name) return 0;
    switch (ast->type) {
    case AST_SYMBOL:
        return strcmp(ast->symbol, name) == 0 ? 1 : 0;
    case AST_LIST: {
        int total = 0;
        for (size_t i = 0; i < ast->list.count; i++)
            total += opt_fv_count(ast->list.items[i], name);
        return total;
    }
    case AST_ARRAY: {
        int total = 0;
        for (size_t i = 0; i < ast->array.element_count; i++)
            total += opt_fv_count(ast->array.elements[i], name);
        return total;
    }
    case AST_SET: {
        int total = 0;
        for (size_t i = 0; i < ast->set.element_count; i++)
            total += opt_fv_count(ast->set.elements[i], name);
        return total;
    }
    case AST_MAP: {
        int total = 0;
        for (size_t i = 0; i < ast->map.count; i++) {
            total += opt_fv_count(ast->map.keys[i], name);
            total += opt_fv_count(ast->map.vals[i], name);
        }
        return total;
    }
    case AST_LAMBDA: {
        for (int i = 0; i < ast->lambda.param_count; i++)
            if (ast->lambda.params[i].name &&
                strcmp(ast->lambda.params[i].name, name) == 0)
                return 0; /* fully shadowed */
        int total = 0;
        for (int i = 0; i < ast->lambda.body_count; i++)
            total += opt_fv_count(ast->lambda.body_exprs[i], name);
        return total;
    }
    case AST_RANGE:
        return opt_fv_count(ast->range.start, name) +
               opt_fv_count(ast->range.step,  name) +
               opt_fv_count(ast->range.end,   name);
    case AST_REFINEMENT:
        if (ast->refinement.var && strcmp(ast->refinement.var, name) == 0)
            return 0; /* refinement variable shadows */
        return opt_fv_count(ast->refinement.predicate, name);
    case AST_TESTS: {
        int total = 0;
        for (int i = 0; i < ast->tests.count; i++)
            total += opt_fv_count(ast->tests.assertions[i], name);
        return total;
    }
    default:
        return 0;
    }
}


/// Data-constructor registry

OptDataType *opt_data_registry_build(ASTList *exprs, size_t *out_count) {
    if (out_count) *out_count = 0;
    if (!exprs) return NULL;

    size_t cap = 0, n = 0;
    OptDataType *types = NULL;

    for (size_t i = 0; i < exprs->count; i++) {
        AST *e = exprs->exprs[i];
        if (!e || e->type != AST_DATA) continue;

        if (n >= cap) {
            cap = cap ? cap * 2 : 8;
            types = realloc(types, sizeof(OptDataType) * cap);
        }
        OptDataType *t = &types[n++];
        t->type_name   = strdup(e->data.name ? e->data.name : "");
        t->ctor_count  = e->data.constructor_count;
        t->ctor_names  = malloc(sizeof(char *) * (size_t)t->ctor_count);
        t->ctor_arities = malloc(sizeof(int)   * (size_t)t->ctor_count);
        for (int c = 0; c < t->ctor_count; c++) {
            ASTDataConstructor *ctor = &e->data.constructors[c];
            t->ctor_names[c]   = strdup(ctor->name ? ctor->name : "");
            t->ctor_arities[c] = ctor->field_count;
        }
    }

    if (out_count) *out_count = n;
    return types;
}

void opt_data_registry_free(OptDataType *types, size_t count) {
    if (!types) return;
    for (size_t i = 0; i < count; i++) {
        free(types[i].type_name);
        for (int c = 0; c < types[i].ctor_count; c++)
            free(types[i].ctor_names[c]);
        free(types[i].ctor_names);
        free(types[i].ctor_arities);
    }
    free(types);
}

const OptDataType *opt_data_registry_find(const OptDataType *types,
                                           size_t count,
                                           const char *type_name) {
    if (!types || !type_name) return NULL;
    for (size_t i = 0; i < count; i++)
        if (strcmp(types[i].type_name, type_name) == 0)
            return &types[i];
    return NULL;
}

const OptDataType *opt_data_registry_find_by_ctor(const OptDataType *types,
                                                   size_t count,
                                                   const char *ctor_name) {
    if (!types || !ctor_name) return NULL;
    for (size_t i = 0; i < count; i++)
        for (int c = 0; c < types[i].ctor_count; c++)
            if (strcmp(types[i].ctor_names[c], ctor_name) == 0)
                return &types[i];
    return NULL;
}

/*  data_registry_ctor_is_nullary: true if ctor_name is registered with
 *  arity 0 in any type in the registry.  Used by constructor-equality
 *  folding.  Returns false (conservative) if the registry is NULL or the
 *  constructor is unknown — an unrecognised symbol used as `=`'s operand
 *  is left alone, since we cannot be sure it denotes a value constructor
 *  at all rather than, say, a shadowed local.
 */
static bool data_registry_ctor_is_nullary(const OptDataType *types,
                                          size_t count,
                                          const char *ctor_name) {
    const OptDataType *t = opt_data_registry_find_by_ctor(types, count,
                                                          ctor_name);
    if (!t) return false;
    for (int c = 0; c < t->ctor_count; c++)
        if (strcmp(t->ctor_names[c], ctor_name) == 0)
            return t->ctor_arities[c] == 0;
    return false;
}


/// Pattern-match analysis — public query interface

bool pattern_covers_constructor(const ASTPattern *p, const char *ctor_name) {
    if (!p || !ctor_name) return false;
    switch (p->kind) {
    case PAT_WILDCARD:
    case PAT_VAR:
        return true;
    case PAT_CONSTRUCTOR:
        return p->var_name && strcmp(p->var_name, ctor_name) == 0;
    default:
        return false;
    }
}

bool pattern_subsumes(const ASTPattern *a, const ASTPattern *b) {
    if (!a || !b) return false;

    /* Wildcard and bound-variable patterns match everything. */
    if (a->kind == PAT_WILDCARD || a->kind == PAT_VAR) return true;

    /* Beyond that, only identical shapes can subsume one another, since
     * a refutable pattern only rules out values it itself would reject.
     */
    if (a->kind != b->kind) return false;

    switch (a->kind) {
    case PAT_LITERAL_INT:
    case PAT_LITERAL_FLOAT:
        return a->lit_value == b->lit_value;
    case PAT_LITERAL_STRING:
        /* var_name doubles as the literal text store for PAT_VAR only in
         * this header's layout; string literal patterns do not carry a
         * dedicated field here, so we cannot compare values we were not
         * given. Treat as non-subsuming rather than guess at a field
         * that may not represent what we think it does.
         */
        return false;
    case PAT_LIST_EMPTY:
        return true; /* [] subsumes [] */
    case PAT_CONSTRUCTOR:
        if (!a->var_name || !b->var_name) return false;
        if (strcmp(a->var_name, b->var_name) != 0) return false;
        if (a->ctor_field_count != b->ctor_field_count) return false;
        for (int i = 0; i < a->ctor_field_count; i++)
            if (!pattern_subsumes(&a->ctor_fields[i], &b->ctor_fields[i]))
                return false;
        return true;
    case PAT_LIST:
        if (a->element_count != b->element_count) return false;
        for (int i = 0; i < a->element_count; i++)
            if (!pattern_subsumes(&a->elements[i], &b->elements[i]))
                return false;
        if ((a->tail == NULL) != (b->tail == NULL)) return false;
        if (a->tail && !pattern_subsumes(a->tail, b->tail)) return false;
        return true;
    default:
        return false;
    }
}

bool pmatch_clause_unreachable(const ASTPattern *candidate,
                                const ASTPattern *const *prior_patterns,
                                const bool *prior_has_guard,
                                int prior_count) {
    if (!candidate || !prior_patterns) return false;
    for (int i = 0; i < prior_count; i++) {
        if (prior_has_guard && prior_has_guard[i]) continue; /* may fail */
        if (pattern_subsumes(prior_patterns[i], candidate)) return true;
    }
    return false;
}

bool pmatch_is_exhaustive(const ASTPattern *const *patterns,
                          int pattern_count,
                          const OptDataType *type) {
    if (!type) return false; /* unknown scrutinee type: cannot prove it */

    for (int i = 0; i < pattern_count; i++) {
        if (!patterns[i]) continue;
        if (patterns[i]->kind == PAT_WILDCARD || patterns[i]->kind == PAT_VAR)
            return true; /* catch-all clause covers every remaining case */
    }

    for (int c = 0; c < type->ctor_count; c++) {
        bool covered = false;
        for (int i = 0; i < pattern_count && !covered; i++)
            if (patterns[i] &&
                pattern_covers_constructor(patterns[i], type->ctor_names[c]))
                covered = true;
        if (!covered) return false;
    }
    return true;
}


/// Range analysis

bool opt_range_literal_length(AST *range_ast, long long *out_length) {
    if (!range_ast || range_ast->type != AST_RANGE) return false;
    AST *start = range_ast->range.start;
    AST *step  = range_ast->range.step;
    AST *end   = range_ast->range.end;

    if (!end) return false; /* infinite range */
    if (!start || start->type != AST_NUMBER) return false;
    if (end->type != AST_NUMBER) return false;

    double step_val = 1.0;
    if (step) {
        if (step->type != AST_NUMBER) return false;
        step_val = step->number;
    }
    if (step_val == 0.0) return false;

    double span = (end->number - start->number) / step_val;
    if (span < 0.0) {
        if (out_length) *out_length = 0;
        return true;
    }
    if (out_length) *out_length = (long long)floor(span) + 1;
    return true;
}


/// Constant table — open-addressing hash map
//
//  Mathematical basis:
//    Load factor <= 0.5 (grow when count >= capacity/2) guarantees that
//    linear probing finds an empty slot in at most 2 expected probes
//    (by the coupon-collector bound for open addressing at alpha=0.5).
//    Power-of-two capacity lets the modulo reduction be a bitmask: O(1)
//    with no division instruction.
//
//    djb2 hash: h(0)=5381, h(i) = h(i-1)*33 ^ c_i.
//    Multiplier 33 = 2^5+1 maps well to a single LEA on x86/ARM.
//    Chosen to match env.c so the two subsystems use the same hash
//    quality characteristics and can be compared directly in profiling.
//
//  Tombstones: we use name==NULL to mean "empty", name==(char*)1 to mean
//  "deleted" (tombstone).  Lookup probes past tombstones; insert reuses
//  the first tombstone it passes.  Since constants are only ever added
//  (never deleted) within one optimizer pass, tombstones never occur in
//  practice — the slot is reserved for future use if we add eviction.
//

#define CT_TOMB ((char *)1)  /* tombstone sentinel, distinct from NULL */

static unsigned int ct_hash(const char *s) {
    unsigned int h = 5381;
    unsigned char c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) ^ c;
    return h;
}

static void constants_free(ConstantTable *table) {
    if (!table->items) return;
    for (size_t i = 0; i < table->capacity; i++) {
        ConstantEntry *e = &table->items[i];
        if (e->name && e->name != CT_TOMB) {
            free(e->name);
            ast_free(e->value);
        }
    }
    free(table->items);
    table->items    = NULL;
    table->count    = 0;
    table->capacity = 0;
}

/*  ct_find_slot: return the index where name lives or should be inserted.
 *  Records the first tombstone seen so insert can reuse it.
 *  Never called on a full table (load <= 0.5 is maintained by constant_put).
 */
static size_t ct_find_slot(ConstantTable *table, const char *name,
                            unsigned int h, size_t *tomb_out) {
    size_t mask = table->capacity - 1;
    size_t i    = h & mask;
    size_t tomb = SIZE_MAX;
    for (;;) {
        char *n = table->items[i].name;
        if (!n)                              { if (tomb_out) *tomb_out = tomb; return i; }
        if (n == CT_TOMB)                    { if (tomb == SIZE_MAX) tomb = i; }
        else if (strcmp(n, name) == 0)       { if (tomb_out) *tomb_out = tomb; return i; }
        i = (i + 1) & mask;
    }
}

static void constant_put(Optimizer *opt, const char *name, AST *value) {
    if (!name || !value || !is_literal(value)) return;

    /* Grow before inserting if load factor would exceed 0.5. */
    if (!opt->constants.items ||
        (opt->constants.count + 1) * 2 > opt->constants.capacity) {
        size_t new_cap = opt->constants.capacity
                         ? opt->constants.capacity * 2 : 32;
        ConstantEntry *new_items = calloc(new_cap, sizeof(ConstantEntry));
        /* Rehash existing live entries into the new table. */
        for (size_t i = 0; i < opt->constants.capacity; i++) {
            ConstantEntry *old = &opt->constants.items[i];
            if (!old->name || old->name == CT_TOMB) continue;
            size_t j = ct_hash(old->name) & (new_cap - 1);
            while (new_items[j].name) j = (j + 1) & (new_cap - 1);
            new_items[j] = *old;
        }
        free(opt->constants.items);
        opt->constants.items    = new_items;
        opt->constants.capacity = new_cap;
    }

    unsigned int h    = ct_hash(name);
    size_t       tomb = SIZE_MAX;
    size_t       slot = ct_find_slot(&opt->constants, name, h, &tomb);

    if (opt->constants.items[slot].name &&
        opt->constants.items[slot].name != CT_TOMB) {
        /* Update existing entry. */
        ast_free(opt->constants.items[slot].value);
        opt->constants.items[slot].value = ast_clone(value);
        return;
    }

    /* Insert into tombstone slot if available, else into empty slot. */
    size_t ins = (tomb != SIZE_MAX) ? tomb : slot;
    opt->constants.items[ins].name  = strdup(name);
    opt->constants.items[ins].value = ast_clone(value);
    opt->constants.count++;
    if (opt->stats) opt->stats->constants_registered++;
}


/// Shadow stack — internal helpers

static void shadows_free(NameStack *stack) {
    free(stack->items);
    stack->items    = NULL;
    stack->count    = 0;
    stack->capacity = 0;
}

static void shadow_push(NameStack *stack, char *name) {
    if (!name) return;
    if (stack->count >= stack->capacity) {
        stack->capacity = stack->capacity ? stack->capacity * 2 : 8;
        stack->items = realloc(stack->items,
                               sizeof(char *) * stack->capacity);
    }
    stack->items[stack->count++] = name;
}

static size_t shadow_mark(NameStack *stack) { return stack->count; }

static void shadow_pop_to(NameStack *stack, size_t mark) {
    stack->count = mark;
}

/* is_shadowed defined below alongside constant_lookup — see that section. */
static bool is_shadowed(NameStack *stack, const char *name);

/// Constant lookup

/*  is_shadowed: scan the shadow stack for name.
 *
 *  The shadow stack is almost always tiny (0-8 entries: lambda params of
 *  the current nesting level).  A linear scan is optimal here — no hash
 *  overhead for 0-8 comparisons, and the stack is hot in L1 cache.
 *  We compare first chars before calling strcmp to short-circuit the
 *  common case where names differ at the first character.
 */
static bool is_shadowed(NameStack *stack, const char *name) {
    char first = name[0];
    for (size_t i = stack->count; i > 0; i--) {
        const char *s = stack->items[i - 1];
        if (s[0] == first && strcmp(s, name) == 0) return true;
    }
    return false;
}

static AST *constant_lookup(Optimizer *opt, const char *name) {
    /* Check shadow stack first — tiny, so linear is fine (see above). */
    if (is_shadowed(&opt->shadows, name)) return NULL;

    /* O(1) hash map lookup for the constant table. */
    if (!opt->constants.items || opt->constants.count == 0) return NULL;
    size_t slot = ct_find_slot(&opt->constants, name,
                               ct_hash(name), NULL);
    ConstantEntry *e = &opt->constants.items[slot];
    if (!e->name || e->name == CT_TOMB) return NULL;
    return e->value;
}


/// define_constant_name — identify typed top-level constant bindings
//
//  Recognises the desugared form:
//    (define [name :: Type] value)
//
//  Returns true and sets *out_name / *out_value if the expression is
//  such a binding.  The caller is responsible for checking is_literal.
//
static bool define_constant_name(AST *expr,
                                  const char **out_name,
                                  AST **out_value) {
    if (!expr || expr->type != AST_LIST || expr->list.count < 3) return false;
    if (!is_symbol(expr->list.items[0], "define")) return false;

    AST *target = expr->list.items[1];
    if (!target || target->type != AST_LIST || target->list.count < 3)
        return false;
    if (!target->list.items[0] ||
        target->list.items[0]->type != AST_SYMBOL)
        return false;

    bool has_type_marker = false;
    for (size_t i = 1; i < target->list.count; i++) {
        if (is_symbol(target->list.items[i], "::")) {
            has_type_marker = true;
            break;
        }
    }
    if (!has_type_marker) return false;

    *out_name  = target->list.items[0]->symbol;
    *out_value = expr->list.items[2];
    return true;
}


/// replace_node — unified rewrite helper
//
//  Frees old, sets opt->changed, and returns next.
//  If next == old or next == NULL, returns old unchanged.
//

static AST *replace_node(AST *old, AST *next, Optimizer *opt) {
    if (!next || next == old) return old;
    ast_free(old);
    opt->changed = true;
    return next;
}


/// Forward declaration

static AST *optimize_expr(Optimizer *opt, AST *ast);


/// optimize_lambda — rewrite body under param shadows, mark tail calls

static bool is_call_form(AST *ast) {
    return ast && ast->type == AST_LIST && ast->list.count >= 1 &&
           ast->list.items[0] && ast->list.items[0]->type == AST_SYMBOL;
}

/*  mark_tail_positions: walk an expression known to be in tail position
 *  and count call-shaped nodes that are themselves in tail position.
 *  (if c t e) propagates tail position into both t and e.  Everything
 *  else that is not a direct call is not marked (its sub-expressions are
 *  not in tail position with respect to the enclosing lambda, except the
 *  if-branches handled here).  This intentionally does not rewrite the
 *  AST — codegen can read AST_LIST.list and recompute the same property
 *  cheaply, but counting here gives early visibility via stats.
 */
static void mark_tail_positions(AST *ast, OptimizationStats *stats) {
    if (!ast) return;
    if (ast->type == AST_LIST && ast->list.count == 4 &&
        is_symbol(ast->list.items[0], "if")) {
        mark_tail_positions(ast->list.items[2], stats);
        mark_tail_positions(ast->list.items[3], stats);
        return;
    }
    if (is_call_form(ast)) {
        if (stats) stats->tail_calls_marked++;
    }
}

static void optimize_lambda(Optimizer *opt, AST *ast) {
    size_t mark = shadow_mark(&opt->shadows);
    for (int i = 0; i < ast->lambda.param_count; i++)
        shadow_push(&opt->shadows, ast->lambda.params[i].name);
    for (int i = 0; i < ast->lambda.body_count; i++)
        ast->lambda.body_exprs[i] =
            optimize_expr(opt, ast->lambda.body_exprs[i]);
    if (ast->lambda.body_count > 0) {
        ast->lambda.body =
            ast->lambda.body_exprs[ast->lambda.body_count - 1];
        if (opt->options->level >= OPT_LEVEL_AGGRESSIVE)
            mark_tail_positions(ast->lambda.body, opt->stats);
    }
    shadow_pop_to(&opt->shadows, mark);
}


/// Folding passes
//
//  Each fold_* function receives an AST_LIST node whose children have
//  already been recursively optimized by optimize_list.
//

/*  op_tag: integer token for each dispatchable operator.
 *
 *  Resolved by head_op_tag() in O(1) via first-character trie + length
 *  check.  No strcmp needed for single-char ops or ops with unique first
 *  chars.  The compiler turns the outer switch on first_char into a jump
 *  table; the inner length/second-char checks are single integer compares.
 *
 *  The special forms (define, set!, quote, include) are handled before
 *  head_op_tag() is called and are not members of this enum.
 */
typedef enum {
    OP_UNKNOWN = 0,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV,
    OP_EQ, OP_NEQ, OP_LT, OP_LTE, OP_GT, OP_GTE,
    OP_AND, OP_OR, OP_NOT,
    OP_IF,
    OP_STR_APPEND
} OpTag;

/*  fold_numeric_call: constant-fold (+, -, *, /) on numeric literals.
 *
 *  Mathematical basis:
 *    For associative+commutative ops (+, *) with identity e:
 *      op(a1..an) = op(op(literals), op(unknowns))
 *                 = op(folded_literal, u1, u2, ..., um)
 *    If folded_literal == e  -> drop it, return op(unknowns)
 *    If unknowns is empty    -> return folded_literal (full fold)
 *    Otherwise               -> return op(folded_literal, unknowns)
 *                               (partial fold: fewer nodes, fewer passes)
 *
 *  For non-commutative ops (-, /):
 *    The first operand is positionally significant so only a full fold
 *    is safe.  We still catch the unary-negate form (- x) as before.
 *
 *  Partial folding eliminates fixed-point iterations that existed solely
 *  to collapse (+ 1 (+ 2 x)) -> (+ 3 x) -> result.  After flattening
 *  and one partial-fold pass the literals are already grouped.
 */
/*  fold_numeric_call: constant-fold arithmetic operators.
 *  Receives OpTag — zero strcmp calls inside.
 *  See previous comment block for the partial-fold mathematical basis.
 */
static AST *fold_numeric_call(AST *ast, OpTag tag, Optimizer *opt) {
    if (ast->list.count < 2) return ast;

    bool is_add = tag == OP_ADD;
    bool is_mul = tag == OP_MUL;
    bool is_sub = tag == OP_SUB;
    bool is_div = tag == OP_DIV;

    /* Unary negation: (- x) -> (-x). */
    if (is_sub && ast->list.count == 2) {
        AST *arg = ast->list.items[1];
        if (!arg || arg->type != AST_NUMBER) return ast;
        if (opt->stats) opt->stats->expressions_folded++;
        return replace_node(ast, number_ast(-arg->number), opt);
    }

    /* Non-commutative ops: full fold only (position matters). */
    if (is_sub || is_div) {
        if (!ast->list.items[1] ||
            ast->list.items[1]->type != AST_NUMBER) return ast;
        double acc = ast->list.items[1]->number;
        for (size_t i = 2; i < ast->list.count; i++) {
            AST *arg = ast->list.items[i];
            if (!arg || arg->type != AST_NUMBER) return ast;
            if (is_sub) { acc -= arg->number; }
            else        { if (arg->number == 0.0) return ast; acc /= arg->number; }
        }
        if (opt->stats) opt->stats->expressions_folded++;
        return replace_node(ast, number_ast(acc), opt);
    }

    /* Commutative ops (+, *): partial fold.
     * identity_val: the absorbing identity for the accumulator.
     *   + -> 0.0   (* -> 1.0) */
    double identity_val = is_mul ? 1.0 : 0.0;
    double acc          = identity_val;
    size_t n_unknown    = 0;

    /* First pass: accumulate literals, count unknowns. */
    for (size_t i = 1; i < ast->list.count; i++) {
        AST *arg = ast->list.items[i];
        if (arg && arg->type == AST_NUMBER) {
            if (is_add) acc += arg->number;
            else        acc *= arg->number;
        } else {
            n_unknown++;
        }
    }

    /* Full fold: every operand was a literal. */
    if (n_unknown == 0) {
        if (opt->stats) opt->stats->expressions_folded++;
        return replace_node(ast, number_ast(acc), opt);
    }

    /* Nothing to collapse: no literals at all, or acc == identity. */
    bool acc_is_identity = (acc == identity_val);
    size_t n_literals = (ast->list.count - 1) - n_unknown;
    if (n_literals == 0 || (n_literals == 1 && !acc_is_identity)) return ast;

    /* Partial fold: rewrite in-place, keeping only unknowns + folded literal.
     * Layout of new items: [op, folded_literal?, unknown0, unknown1, ...] */
    size_t out = 1; /* items[0] is the operator symbol, stays */
    if (!acc_is_identity)
        ast->list.items[out++] = number_ast(acc); /* folded literal first */

    for (size_t i = 1; i < ast->list.count; i++) {
        AST *arg = ast->list.items[i];
        if (arg && arg->type == AST_NUMBER) {
            ast_free(arg);          /* consumed into acc */
            ast->list.items[i] = NULL;
        } else {
            ast->list.items[out++] = arg;
        }
    }
    ast->list.count = out;
    opt->changed = true;
    if (opt->stats) opt->stats->expressions_folded++;
    return ast;
}

/*  fold_compare_call: constant-fold comparison operators.
 *
 *  Receives OpTag instead of string — zero strcmp calls inside.
 *  Numeric: folds any two AST_NUMBER operands.
 *  Equality: also folds two nullary ADT constructor symbols whose
 *  structural equality is trivially decidable (no fields to compare).
 */
static AST *fold_compare_call(AST *ast, OpTag tag, Optimizer *opt) {
    if (ast->list.count != 3) return ast;
    AST *lhs = ast->list.items[1];
    AST *rhs = ast->list.items[2];
    if (!lhs || !rhs) return ast;

    if (lhs->type == AST_NUMBER && rhs->type == AST_NUMBER) {
        double l = lhs->number, r = rhs->number;
        bool result;
        switch (tag) {
        case OP_EQ:  result = l == r; break;
        case OP_NEQ: result = l != r; break;
        case OP_LT:  result = l <  r; break;
        case OP_LTE: result = l <= r; break;
        case OP_GT:  result = l >  r; break;
        case OP_GTE: result = l >= r; break;
        default: return ast;
        }
        if (opt->stats) opt->stats->expressions_folded++;
        return replace_node(ast, bool_ast(result), opt);
    }

    if ((tag == OP_EQ || tag == OP_NEQ) &&
        lhs->type == AST_SYMBOL && rhs->type == AST_SYMBOL &&
        data_registry_ctor_is_nullary(opt->data_types, opt->data_type_count,
                                      lhs->symbol) &&
        data_registry_ctor_is_nullary(opt->data_types, opt->data_type_count,
                                      rhs->symbol)) {
        bool eq     = strcmp(lhs->symbol, rhs->symbol) == 0;
        bool result = (tag == OP_EQ) ? eq : !eq;
        if (opt->stats) opt->stats->ctor_equalities_folded++;
        return replace_node(ast, bool_ast(result), opt);
    }

    return ast;
}

/*  fold_bool_call: constant-fold (not, and, or).
 *
 *  Mathematical basis:
 *    `and` is commutative+associative with identity True and annihilator False.
 *    `or`  is commutative+associative with identity False and annihilator True.
 *
 *    Annihilator rule (short-circuit, order-preserving):
 *      Scan left-to-right; the first annihilator makes the whole expr
 *      statically determined regardless of later (possibly impure) args.
 *      We cannot drop args to the right of an unknown, so we stop there.
 *
 *    Identity elimination (partial fold, mirrors fold_numeric_call):
 *      Known-identity args carry no information and can be dropped.
 *      If only one non-identity arg remains, the op itself is redundant.
 *        (and True x True) -> x
 *        (or  False x)     -> x
 */
/*  fold_bool_call: constant-fold boolean operators.
 *  Receives OpTag — zero strcmp calls inside.
 *  See previous comment block for the algebraic basis.
 */
static AST *fold_bool_call(AST *ast, OpTag tag, Optimizer *opt) {
    if (tag == OP_NOT && ast->list.count == 2) {
        bool known  = false;
        bool falsey = is_falsey(ast->list.items[1], &known);
        if (!known) return ast;
        if (opt->stats) opt->stats->expressions_folded++;
        return replace_node(ast, bool_ast(falsey), opt);
    }

    bool is_and = (tag == OP_AND);
    if (tag != OP_AND && tag != OP_OR) return ast;
    if (ast->list.count < 2) return ast;

    /* Left-to-right scan: annihilator terminates, identity is stripped.
     * We stop scanning at the first unknown to preserve evaluation order. */
    size_t out = 1; /* items[0] = operator symbol */
    for (size_t i = 1; i < ast->list.count; i++) {
        AST  *arg    = ast->list.items[i];
        bool  known  = false;
        bool  falsey = is_falsey(arg, &known);
        if (!known) {
            /* Unknown: keep it and all remaining args unchanged. */
            ast->list.items[out++] = arg;
            for (size_t j = i + 1; j < ast->list.count; j++)
                ast->list.items[out++] = ast->list.items[j];
            break;
        }
        bool is_annihilator = is_and ? falsey : !falsey;
        if (is_annihilator) {
            /* Annihilator found: result is statically determined.
             * Free all remaining args only if they are pure (no effects). */
            bool rest_pure = true;
            for (size_t j = i + 1; j < ast->list.count; j++)
                if (!ast_is_pure(ast->list.items[j]))
                    { rest_pure = false; break; }
            if (!rest_pure) {
                /* Cannot discard impure tail: keep from here onward. */
                ast->list.items[out++] = arg;
                for (size_t j = i + 1; j < ast->list.count; j++)
                    ast->list.items[out++] = ast->list.items[j];
                break;
            }
            for (size_t j = i; j < ast->list.count; j++)
                ast_free(ast->list.items[j]);
            if (opt->stats) opt->stats->expressions_folded++;
            return replace_node(ast, bool_ast(!is_and), opt);
        }
        /* Identity: drop this arg (free it, don't copy to out). */
        ast_free(arg);
        opt->changed = true;
    }
    ast->list.count = out;

    /* All args were identities: return the identity value itself. */
    if (ast->list.count == 1) {
        if (opt->stats) opt->stats->expressions_folded++;
        return replace_node(ast, bool_ast(is_and), opt);
    }
    /* Single non-identity survivor: the op is now redundant. */
    if (ast->list.count == 2 && ast_is_pure(ast->list.items[1])) {
        AST *survivor = ast_clone(ast->list.items[1]);
        if (opt->stats) opt->stats->identities_elided++;
        return replace_node(ast, survivor, opt);
    }
    return ast;
}

/* fold_if: fold (if cond then else) when cond is statically known. */
static AST *fold_if(AST *ast, Optimizer *opt) {
    if (ast->list.count != 4) return ast;
    bool known  = false;
    bool falsey = is_falsey(ast->list.items[1], &known);
    if (!known) return ast;

    AST *kept = ast_clone(ast->list.items[falsey ? 3 : 2]);
    if (opt->stats) opt->stats->branches_folded++;
    return replace_node(ast, kept, opt);
}

/* fold_string_append: collapse (str-append "a" "b" …) to one string.
 *  Gated on options->fold_strings: the name may be shadowed at runtime
 *  by a user definition with side effects.
 */
static AST *fold_string_append(AST *ast, Optimizer *opt) {
    if (!opt->options->fold_strings) return ast;
    if (ast->list.count < 2) return ast;

    size_t total_len = 0;
    for (size_t i = 1; i < ast->list.count; i++) {
        AST *arg = ast->list.items[i];
        if (!arg || arg->type != AST_STRING) return ast;
        total_len += strlen(arg->string);
    }

    char *buf = malloc(total_len + 1);
    if (!buf) return ast;
    buf[0] = '\0';
    for (size_t i = 1; i < ast->list.count; i++)
        strcat(buf, ast->list.items[i]->string);

    AST *result = string_ast(buf);
    free(buf);
    if (opt->stats) opt->stats->strings_folded++;
    return replace_node(ast, result, opt);
}

/* fold_identity: eliminate identity-element applications.
 *
 *  Rules (only fires when exactly one non-identity operand remains, to
 *  avoid changing evaluation order or silently dropping side effects):
 *    (+ x 0), (+ 0 x)        -> x
 *    (- x 0)                -> x     (never (- 0 x), that negates x)
 *    (* x 1), (* 1 x)        -> x
 *    (and x True), (and True x)  -> x
 *    (or  x False), (or False x) -> x
 */
/*  fold_identity: eliminate identity-element applications.
 *  Receives OpTag — zero strcmp calls inside.
 *  See previous comment block for the algebraic rules.
 */
static AST *fold_identity(AST *ast, OpTag tag, Optimizer *opt) {
    if (ast->list.count < 3) return ast;

    bool   is_num_op    = false;
    bool   is_bool_op   = false;
    double identity_num  = 0.0;
    bool   identity_bool = false;

    switch (tag) {
    case OP_ADD:                 is_num_op  = true; identity_num  = 0.0;  break;
    case OP_SUB:                 is_num_op  = true; identity_num  = 0.0;  break;
    case OP_MUL:                 is_num_op  = true; identity_num  = 1.0;  break;
    case OP_AND: is_bool_op = true; identity_bool = true;  break;
    case OP_OR:  is_bool_op = true; identity_bool = false; break;
    default: return ast;
    }

    size_t survivors = 0;
    AST   *survivor   = NULL;
    for (size_t i = 1; i < ast->list.count; i++) {
        AST *arg = ast->list.items[i];
        bool is_id = false;
        if (is_num_op && arg && arg->type == AST_NUMBER) {
            is_id = (arg->number == identity_num);
        } else if (is_bool_op && arg) {
            bool known = false;
            bool falsey = is_falsey(arg, &known);
            if (known) is_id = (identity_bool ? !falsey : falsey);
        }
        if (!is_id) { survivors++; survivor = arg; }
    }
    if (survivors != 1 || !survivor) return ast;

    /* Subtraction is non-commutative: only (- x 0) qualifies, never the
     * symmetric (- 0 x), which negates x rather than being a no-op. */
    if (tag == OP_SUB) {
        if (ast->list.count != 3) return ast;
        AST *second = ast->list.items[2];
        if (survivor != ast->list.items[1] ||
            !second || second->type != AST_NUMBER || second->number != 0.0)
            return ast;
    }
    if (!ast_is_pure(survivor)) return ast;

    AST *result = ast_clone(survivor);
    if (opt->stats) opt->stats->identities_elided++;
    return replace_node(ast, result, opt);
}


/// if-chain analysis — switch candidate detection
//
//  Walks an (if …) chain and checks whether it has the shape:
//    (if (= x k1) r1 (if (= x k2) r2 … else_expr))
//  where x is always the same symbol and k_i are distinct literals.
//  Only counts; does not rewrite.  Lowering to an actual switch/jump
//  table is a codegen decision once it has target-specific cost data.
//

static void detect_switch_candidate(AST *ast, Optimizer *opt) {
    if (!opt->stats || !ast) return;
    if (!is_symbol(ast->list.items[0], "if") || ast->list.count != 4) return;

    AST *cond = ast->list.items[1];
    if (!cond || cond->type != AST_LIST || cond->list.count != 3) return;
    if (!is_symbol(cond->list.items[0], "=")) return;
    AST *discriminant = cond->list.items[1];
    if (!discriminant || discriminant->type != AST_SYMBOL) return;
    if (!is_literal(cond->list.items[2])) return;

    int arms = 1;
    AST *rest = ast->list.items[3];
    while (rest && rest->type == AST_LIST &&
           rest->list.count == 4 &&
           is_symbol(rest->list.items[0], "if")) {
        AST *c = rest->list.items[1];
        if (!c || c->type != AST_LIST || c->list.count != 3) break;
        if (!is_symbol(c->list.items[0], "=")) break;
        if (!is_symbol(c->list.items[1], discriminant->symbol)) break;
        if (!is_literal(c->list.items[2])) break;
        arms++;
        rest = rest->list.items[3];
    }
    if (arms >= 3) opt->stats->switch_candidates++;
}


/// Pattern-match analysis — pass driver
//
//  analyze_pmatch: for each matched parameter column, walk clauses in
//  order checking reachability against all unguarded preceding clauses
//  in that column, and (if a data registry is available and the column
//  appears to scrutinize a known type) check exhaustiveness.
//
//  This is read-only: it never mutates the AST_PMATCH node. It updates
//  stats and, if the corresponding warn_* option is set, prints a
//  diagnostic to stderr with the best location information available
//  (the body's line/col, since ASTPattern carries none of its own).
//

static const OptDataType *infer_column_type(Optimizer *opt,
                                            ASTPMatchClause *clauses,
                                            int clause_count,
                                            int column) {
    for (int i = 0; i < clause_count; i++) {
        if (column >= clauses[i].pattern_count) continue;
        ASTPattern *p = &clauses[i].patterns[column];
        if (p->kind != PAT_CONSTRUCTOR || !p->var_name) continue;
        const OptDataType *t = opt_data_registry_find_by_ctor(
            opt->data_types, opt->data_type_count, p->var_name);
        if (t) return t;
    }
    return NULL;
}

static void analyze_pmatch(Optimizer *opt, AST *ast) {
    if (ast->type != AST_PMATCH) return;
    ASTPMatchClause *clauses = ast->pmatch.clauses;
    int clause_count = ast->pmatch.clause_count;
    if (clause_count == 0) return;

    if (opt->stats) opt->stats->pmatch_clauses_seen += (size_t)clause_count;

    int max_columns = 0;
    for (int i = 0; i < clause_count; i++)
        if (clauses[i].pattern_count > max_columns)
            max_columns = clauses[i].pattern_count;

    for (int col = 0; col < max_columns; col++) {
        /* Build the per-column pattern + guard-flag arrays. */
        const ASTPattern **col_patterns =
            malloc(sizeof(ASTPattern *) * (size_t)clause_count);
        bool *col_guarded = malloc(sizeof(bool) * (size_t)clause_count);
        for (int i = 0; i < clause_count; i++) {
            col_patterns[i] = (col < clauses[i].pattern_count)
                              ? &clauses[i].patterns[col] : NULL;
            col_guarded[i]  = clauses[i].guard_count > 0;
        }

        for (int i = 0; i < clause_count; i++) {
            if (!col_patterns[i]) continue;
            if (pmatch_clause_unreachable(col_patterns[i], col_patterns,
                                          col_guarded, i)) {
                if (opt->stats) opt->stats->pmatch_unreachable++;
                if (opt->options->warn_unreachable) {
                    fprintf(stderr,
                        "%s: warning: pmatch clause %d (column %d) is "
                        "unreachable: an earlier unguarded pattern "
                        "already matches every value it would match\n",
                        opt->options->source_name
                            ? opt->options->source_name : "<input>",
                        i + 1, col);
                }
            }
        }

        const OptDataType *col_type =
            infer_column_type(opt, clauses, clause_count, col);
        if (col_type &&
            !pmatch_is_exhaustive(col_patterns, clause_count, col_type)) {
            if (opt->stats) opt->stats->pmatch_nonexhaustive++;
            if (opt->options->warn_nonexhaustive) {
                fprintf(stderr,
                    "%s: warning: pmatch over `%s` (column %d) does not "
                    "cover all constructors and has no wildcard clause\n",
                    opt->options->source_name
                        ? opt->options->source_name : "<input>",
                    col_type->type_name, col);
            }
        }

        free(col_patterns);
        free(col_guarded);
    }
}

/*  head_op_tag: map a symbol string to an OpTag in O(1).
 *
 *  Implementation: trie on first character, disambiguated by length and
 *  (where needed) second character.  Cost: 1 array-index + at most 2
 *  integer comparisons + 0-1 strcmp (only for str-append).
 *  All single-char operators and ops with unique initials cost exactly
 *  one switch case with no further comparison.
 */
static OpTag head_op_tag(const char *s) {
    if (!s || !s[0]) return OP_UNKNOWN;
    switch (s[0]) {
    case '+': return s[1] ? OP_UNKNOWN : OP_ADD;
    case '-': return s[1] ? OP_UNKNOWN : OP_SUB;
    case '*': return s[1] ? OP_UNKNOWN : OP_MUL;
    case '/': return s[1] ? OP_UNKNOWN : OP_DIV;
    case '=': return s[1] ? OP_UNKNOWN : OP_EQ;
    case '!': return (s[1]=='=' && !s[2]) ? OP_NEQ : OP_UNKNOWN;
    case '<': return !s[1] ? OP_LT : (s[1]=='=' && !s[2]) ? OP_LTE : OP_UNKNOWN;
    case '>': return !s[1] ? OP_GT : (s[1]=='=' && !s[2]) ? OP_GTE : OP_UNKNOWN;
    case 'a': return (s[1]=='n' && s[2]=='d' && !s[3]) ? OP_AND : OP_UNKNOWN;
    case 'o': return (s[1]=='r' && !s[2])              ? OP_OR  : OP_UNKNOWN;
    case 'n': return (s[1]=='o' && s[2]=='t' && !s[3]) ? OP_NOT : OP_UNKNOWN;
    case 'i': return (s[1]=='f' && !s[2])              ? OP_IF  : OP_UNKNOWN;
    case 's': return strcmp(s, "str-append") == 0 ? OP_STR_APPEND : OP_UNKNOWN;
    default:  return OP_UNKNOWN;
    }
}

/// optimize_list — the main rewrite dispatcher for list forms

static AST *optimize_list(Optimizer *opt, AST *ast) {
    if (ast->list.count == 0) return ast;

    AST        *head     = ast->list.items[0];
    const char *head_sym = (head && head->type == AST_SYMBOL)
                           ? head->symbol : NULL;
    if (!head_sym) goto optimize_children;

    /* Special forms: guard before touching children. */
    switch (head_sym[0]) {
    case 'q':
        if (head_sym[1]=='u' && strcmp(head_sym,"quote")==0)   return ast;
        break;
    case 'i':
        if (head_sym[1]=='n' && strcmp(head_sym,"include")==0) return ast;
        break;
    case 'd':
        if (head_sym[1]=='e' && strcmp(head_sym,"define")==0 &&
            ast->list.count >= 3) {
            ast->list.items[2] = optimize_expr(opt, ast->list.items[2]);
            return ast;
        }
        break;
    case 's':
        if (head_sym[1]=='e' && strcmp(head_sym,"set!")==0) {
            for (size_t i = 2; i < ast->list.count; i++)
                ast->list.items[i] = optimize_expr(opt, ast->list.items[i]);
            return ast;
        }
        break;
    }

optimize_children:
    /* Bottom-up: rewrite all children before folding this node. */
    for (size_t i = 0; i < ast->list.count; i++)
        ast->list.items[i] = optimize_expr(opt, ast->list.items[i]);

    head     = ast->list.items[0];
    head_sym = (head && head->type == AST_SYMBOL) ? head->symbol : NULL;
    if (!head_sym) return ast;

    OpTag tag = head_op_tag(head_sym);
    switch (tag) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: {
        AST *r = fold_numeric_call(ast, tag, opt);
        return (r != ast) ? r : fold_identity(ast, tag, opt);
    }
    case OP_EQ: case OP_NEQ:
    case OP_LT: case OP_LTE:
    case OP_GT: case OP_GTE:
        return fold_compare_call(ast, tag, opt);
    case OP_AND: case OP_OR: {
        AST *r = fold_bool_call(ast, tag, opt);
        return (r != ast) ? r : fold_identity(ast, tag, opt);
    }
    case OP_NOT:
        return fold_bool_call(ast, tag, opt);
    case OP_IF:
        detect_switch_candidate(ast, opt);
        return fold_if(ast, opt);
    case OP_STR_APPEND:
        return fold_string_append(ast, opt);
    case OP_UNKNOWN:
        return ast;
    }
    return ast;
}

/// optimize_expr — main recursive descent
//
//  See the file header note on exhaustiveness: every ASTType has an
//  explicit case below.  Do not add a `default:` label to this switch.
//

static AST *optimize_expr(Optimizer *opt, AST *ast) {
    if (!ast) return NULL;

    switch (ast->type) {

    case AST_SYMBOL: {
        AST *val = constant_lookup(opt, ast->symbol);
        if (!val) return ast;
        if (opt->stats) opt->stats->constants_propagated++;
        return replace_node(ast, ast_clone(val), opt);
    }

    case AST_LIST:
        return optimize_list(opt, ast);

    case AST_ARRAY:
        for (size_t i = 0; i < ast->array.element_count; i++)
            ast->array.elements[i] =
                optimize_expr(opt, ast->array.elements[i]);
        return ast;

    case AST_SET:
        for (size_t i = 0; i < ast->set.element_count; i++)
            ast->set.elements[i] =
                optimize_expr(opt, ast->set.elements[i]);
        return ast;

    case AST_MAP:
        for (size_t i = 0; i < ast->map.count; i++) {
            ast->map.keys[i] = optimize_expr(opt, ast->map.keys[i]);
            ast->map.vals[i] = optimize_expr(opt, ast->map.vals[i]);
        }
        return ast;

    case AST_LAMBDA:
        optimize_lambda(opt, ast);
        return ast;

    case AST_REFINEMENT:
        ast->refinement.predicate =
            optimize_expr(opt, ast->refinement.predicate);
        return ast;

    case AST_TESTS:
        for (int i = 0; i < ast->tests.count; i++)
            ast->tests.assertions[i] =
                optimize_expr(opt, ast->tests.assertions[i]);
        return ast;

    case AST_RANGE:
        ast->range.start = optimize_expr(opt, ast->range.start);
        ast->range.step  = optimize_expr(opt, ast->range.step);
        ast->range.end   = optimize_expr(opt, ast->range.end);
        return ast;

    case AST_PMATCH:
        /* Optimize every clause body and guard, then analyze the whole
         * match for reachability/exhaustiveness once children are
         * settled (analysis reads patterns, which rewriting never
         * touches, so order relative to the body rewrite doesn't matter
         * for correctness — we do it after for up-to-date stats). */
        for (int i = 0; i < ast->pmatch.clause_count; i++) {
            ASTPMatchClause *cl = &ast->pmatch.clauses[i];
            cl->body = optimize_expr(opt, cl->body);
            for (int g = 0; g < cl->guard_count; g++) {
                cl->guard_conds[g]  = optimize_expr(opt, cl->guard_conds[g]);
                cl->guard_bodies[g] = optimize_expr(opt, cl->guard_bodies[g]);
            }
        }
        if (opt->options->level >= OPT_LEVEL_AGGRESSIVE)
            analyze_pmatch(opt, ast);
        return ast;

    case AST_ASM:
        /* Inline assembly operands are not semantically rewritten: an
         * instruction's operand list has machine-specific meaning that
         * this pass has no model of. */
        return ast;

    case AST_LAYOUT:
        /* No rewritable sub-expressions: ASTLayoutField carries only
         * names and type strings. */
        return ast;

    case AST_DATA:
        /* Constructors carry only names/type-name strings, not
         * expressions. */
        return ast;

    case AST_CLASS:
        /* Default-method bodies are real code; optimize them. Method
         * *signatures* (method_types) are type strings, not exprs. */
        for (int i = 0; i < ast->class_decl.default_count; i++)
            ast->class_decl.default_bodies[i] =
                optimize_expr(opt, ast->class_decl.default_bodies[i]);
        return ast;

    case AST_INSTANCE:
        /* Method implementations are lambda ASTs; optimize them. Typeclass
         * *dispatch resolution* is a dependent-checker concern, not ours;
         * we only rewrite what's already known to be the chosen body. */
        for (int i = 0; i < ast->instance_decl.method_count; i++)
            ast->instance_decl.method_bodies[i] =
                optimize_expr(opt, ast->instance_decl.method_bodies[i]);
        return ast;

    case AST_ADDRESS_OF:
        /* The operand's identity/lvalue-ness must be preserved exactly;
         * rewriting it (even to an equal value) could change which
         * address is taken. */
        return ast;

    case AST_NUMBER:
    case AST_STRING:
    case AST_PATH:
    case AST_CHAR:
    case AST_KEYWORD:
    case AST_RATIO:
    case TOK_LAMBDA_LIT:
        /* Atomic literals (or, for TOK_LAMBDA_LIT, a literal-like leaf
         * per its placement in ASTType): nothing to recurse into. */
        return ast;
    }

    /* Unreachable if the switch above is kept exhaustive over ASTType. */
    return ast;
}


/// collect_constants — pre-scan pass

static void collect_constants(Optimizer *opt, ASTList *exprs) {
    for (size_t i = 0; i < exprs->count; i++) {
        const char *name  = NULL;
        AST        *value = NULL;
        if (define_constant_name(exprs->exprs[i], &name, &value) &&
            is_literal(value))
            constant_put(opt, name, value);
    }
}


/// Trace helpers

static void trace_ast_line(AST *ast) {
    if (!ast) { fprintf(stderr, "<null>"); return; }
    char *json = ast_to_json(ast);
    if (json) { fprintf(stderr, "%s", json); free(json); }
    else        fprintf(stderr, "<ast>");
}

static void trace_semantic_begin(const OptimizationOptions *opts,
                                  ASTList *exprs) {
    if (!opts || !opts->trace_semantic) return;
    fprintf(stderr, "\n=== semantic analysis (%s) ===\n",
            opts->source_name ? opts->source_name : "<memory>");
    fprintf(stderr, "├─ \033[36mInput AST\033[0m\n");
    for (size_t i = 0; i < exprs->count; i++) {
        fprintf(stderr, "│  ├─ ");
        trace_ast_line(exprs->exprs[i]);
        fprintf(stderr, "\n");
    }
}

static void trace_semantic_pass(const OptimizationOptions *opts,
                                 const OptimizationStats *stats,
                                 int pass, bool changed,
                                 ASTList *exprs) {
    if (!opts || !opts->trace_semantic) return;
    fprintf(stderr,
        "├─ \033[33mPass %d\033[0m %s\n",
        pass + 1, changed ? "rewrote AST" : "reached fixed point");
    fprintf(stderr, "│  ├─ constants:    %zu\n", stats->constants_registered);
    fprintf(stderr, "│  ├─ propagated:   %zu\n", stats->constants_propagated);
    fprintf(stderr, "│  ├─ folded:       %zu\n", stats->expressions_folded);
    fprintf(stderr, "│  ├─ branches:     %zu\n", stats->branches_folded);
    fprintf(stderr, "│  ├─ strings:      %zu\n", stats->strings_folded);
    fprintf(stderr, "│  ├─ identities:   %zu\n", stats->identities_elided);
    fprintf(stderr, "│  ├─ ctor_eq:      %zu\n", stats->ctor_equalities_folded);
    fprintf(stderr, "│  └─ AST\n");
    for (size_t i = 0; i < exprs->count; i++) {
        fprintf(stderr, "│     ├─ ");
        trace_ast_line(exprs->exprs[i]);
        fprintf(stderr, "\n");
    }
}

static void trace_semantic_end(const OptimizationOptions *opts,
                                const OptimizationStats *stats) {
    if (!opts || !opts->trace_semantic) return;
    fprintf(stderr,
        "└─ \033[32mDone\033[0m level=%s passes=%zu ",
        optimization_level_name(opts->level), stats->passes_run);
    opt_stats_print(stats, opts, stderr);
    fprintf(stderr, "=== end semantic analysis ===\n\n");
}


/// Public: optimize_ast_list

void optimize_ast_list(ASTList *exprs,
                       const OptimizationOptions *options,
                       OptimizationStats *stats) {
    if (!exprs || !options || options->level == OPT_LEVEL_NONE) return;

    OptimizationStats local_stats = {0};
    if (!stats) stats = &local_stats;

    size_t       data_type_count = 0;
    OptDataType *data_types = opt_data_registry_build(exprs, &data_type_count);

    Optimizer opt = {0};
    opt.stats           = stats;
    opt.options         = options;
    opt.data_types      = data_types;
    opt.data_type_count = data_type_count;

    int max_passes = (options->level == OPT_LEVEL_AGGRESSIVE) ? 4 : 2;

    trace_semantic_begin(options, exprs);

    for (int pass = 0; pass < max_passes; pass++) {
        opt.changed = false;
        collect_constants(&opt, exprs);
        for (size_t i = 0; i < exprs->count; i++)
            exprs->exprs[i] = optimize_expr(&opt, exprs->exprs[i]);
        stats->passes_run++;
        trace_semantic_pass(options, stats, pass, opt.changed, exprs);
        if (!opt.changed) break;
    }

    trace_semantic_end(options, stats);

    if (options->print_stats)
        opt_stats_print(stats, options, stdout);

    /* Publish the constant table as a snapshot for downstream phases. */
    if (g_last_snapshot) {
        free(g_last_snapshot->names);
        free(g_last_snapshot->values);
        free(g_last_snapshot);
        g_last_snapshot = NULL;
    }
    if (opt.constants.count > 0) {
        OptConstantSnapshot *snap = malloc(sizeof *snap);
        if (snap) {
            snap->count  = opt.constants.count;
            snap->names  = malloc(sizeof(const char *) * snap->count);
            snap->values = malloc(sizeof(AST *)        * snap->count);
            if (snap->names && snap->values) {
                for (size_t i = 0; i < snap->count; i++) {
                    snap->names[i]  = opt.constants.items[i].name;
                    snap->values[i] = opt.constants.items[i].value;
                }
                g_last_snapshot = snap;
            } else {
                free(snap->names);
                free(snap->values);
                free(snap);
            }
        }
    }

    constants_free(&opt.constants);
    shadows_free(&opt.shadows);
    opt_data_registry_free(data_types, data_type_count);
}


/// Public: optimize_expr_standalone

AST *optimize_expr_standalone(AST *expr,
                               const OptimizationOptions *options,
                               OptimizationStats *stats) {
    if (!expr) return NULL;

    static const OptimizationOptions default_opts = {
        .level              = OPT_LEVEL_BASIC,
        .print_stats        = false,
        .trace_semantic     = false,
        .fold_strings       = false,
        .warn_unreachable   = false,
        .warn_nonexhaustive = false,
        .source_name        = NULL,
    };
    if (!options) options = &default_opts;
    if (options->level == OPT_LEVEL_NONE) return expr;

    OptimizationStats local_stats = {0};
    if (!stats) stats = &local_stats;

    Optimizer opt = {0};
    opt.stats   = stats;
    opt.options = options;
    /* No enclosing program: no data registry available. Constructor
     * equality folding and pmatch exhaustiveness are simply inert here,
     * which is the correct conservative behaviour for an isolated
     * expression with no visible `data` declarations. */

    int max_passes = (options->level == OPT_LEVEL_AGGRESSIVE) ? 4 : 2;
    for (int pass = 0; pass < max_passes; pass++) {
        opt.changed = false;
        expr = optimize_expr(&opt, expr);
        stats->passes_run++;
        if (!opt.changed) break;
    }

    constants_free(&opt.constants);
    shadows_free(&opt.shadows);
    return expr;
}


/// Public: opt_constant_snapshot / opt_constant_snapshot_free

OptConstantSnapshot *opt_constant_snapshot(void) {
    return g_last_snapshot;
}

void opt_constant_snapshot_free(OptConstantSnapshot *snap) {
    if (!snap || snap != g_last_snapshot) return;
    free(snap->names);
    free(snap->values);
    free(snap);
    g_last_snapshot = NULL;
}
