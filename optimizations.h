#ifndef OPTIMIZATIONS_H
#define OPTIMIZATIONS_H

/*  optimizations.h — AST Optimization Pipeline
 *
 *  Implements semantic-level optimization passes over the desugared AST,
 *  running after parsing and desugaring but before dependent checking and
 *  codegen.  The pipeline is purely rewriting-based: it never introduces
 *  new names, never changes observable semantics, and is safe jto skip
 *  entirely (OPT_LEVEL_NONE).
 *
 *  Passes (in order within each fixed-point iteration):
 *    · Constant registration   — immutable top-level "::" bindings
 *    · Constant propagation    — inline registered literals at use sites
 *    · Arithmetic folding      — (+, -, *, /) on numeric literals
 *    · Comparison folding      — (=, !=, <, <=, >, >=) on numeric literals
 *    · Nullary-constructor folding — (= Ctor Ctor) for zero-field data ctors
 *    · Boolean folding         — (and, or, not) on known truth values
 *    · Identity-element elision — (+ x 0), (* x 1), (and x True), …
 *    · Branch folding          — (if) with statically-known condition
 *    · String folding          — (str-append …) on string literals
 *    · Range analysis          — static length/emptiness on literal ranges
 *    · Switch-shape detection  — (if (= x k1) … (if (= x k2) …)) chains
 *    · Pattern-match analysis  — clause reachability + exhaustiveness
 *    · Tail-position annotation — marks tail calls inside lambda bodies
 *
 *  Coverage:
 *    Every ASTType in reader.h is visited by optimize_expr.  Forms with
 *    no rewritable sub-expressions (AST_LAYOUT, AST_PATH, literals) are
 *    explicitly listed as no-ops rather than falling through a default
 *    case, so adding a new ASTType to reader.h without updating this
 *    file produces a compiler warning (see the exhaustiveness note on
 *    optimize_expr in optimizations.c) instead of silent non-coverage.
 *
 *  Fixed-point iteration:
 *    Each pass runs bottom-up over the AST.  After all passes the loop
 *    checks whether any rewrite fired (opt.changed).  If so it repeats,
 *    up to max_passes times (2 for BASIC, 4 for AGGRESSIVE).
 *
 *  Safety invariants:
 *    · Only "name :: Type = value" (desugared "(define [name :: Type] value)")
 *      bindings whose value is a literal enter the constant table.
 *    · Plain "(define name value)" is treated as a mutable binding.
 *    · Lambda parameters shadow top-level constants for the body's scope.
 *    · set!, asm, include, class, instance, and layout are never
 *      structurally rewritten beyond their safe child expressions.
 *    · Division by zero is never folded.
 *    · Floating-point rewrites respect IEEE-754: no reassociation, no
 *      folding across NaN/Inf without an explicit, documented rule.
 *    · Pattern-match analysis is read-only: it never rewrites a pmatch,
 *      only annotates statistics and (optionally) emits diagnostics.
 *      Lowering unreachable clauses or rejecting non-exhaustive matches
 *      is a *correctness* concern that belongs to the dependent checker;
 *      this pass surfaces evidence, the checker decides what's fatal.
 *
 *  Open boundary with the surface grammar:
 *    `let` does not appear as an ASTType in reader.h; it is a surface
 *    form consumed by desugar_let_ast() before this pass runs.  This
 *    file therefore has no let-specific rewrites — by the time
 *    optimize_ast_list sees the program, `let` has already become
 *    whatever desugar_let_ast produces (most likely a lambda
 *    application), and AST_LAMBDA's existing shadow handling covers it.
 *    If a future revision of the desugarer special-cases `let` instead
 *    of lowering it, the pass for it belongs here, not invented ahead
 *    of that decision.
 */

#include <stdbool.h>
#include <stddef.h>
#include "reader.h"   /* AST*, ASTList, ASTPattern, ASTDataConstructor */
#include <stdio.h>

/// Optimization levels
//
//  OPT_LEVEL_NONE       — No passes run; pipeline is a no-op.
//  OPT_LEVEL_BASIC      — 2 fixed-point passes; safe algebraic rewrites.
//  OPT_LEVEL_AGGRESSIVE — 4 fixed-point passes; adds pattern-match
//                         analysis and tail-position annotation.
//
//  Levels map to the CLI -O flag:
//    -O0  -> NONE
//    -O1  -> BASIC
//    -O2  -> AGGRESSIVE  (default for release builds)
//
typedef enum {
    OPT_LEVEL_NONE       = 0,
    OPT_LEVEL_BASIC      = 1,
    OPT_LEVEL_AGGRESSIVE = 2,
} OptimizationLevel;

const char *optimization_level_name(OptimizationLevel level);


/// Pipeline options
//
//  level              — controls max passes and which passes are enabled.
//  print_stats        — print a one-line summary to stdout after the run.
//  trace_semantic     — emit a coloured tree trace to stderr after each pass.
//  fold_strings       — enable (str-append …) folding.  Off by default in
//                        case the runtime defines its own str-append with
//                        side effects in scope.
//  warn_unreachable   — print a diagnostic for each pmatch clause found
//                        unreachable by an earlier clause (AGGRESSIVE only).
//  warn_nonexhaustive — print a diagnostic when a pmatch over a known
//                        `data` type does not cover all constructors and
//                        has no trailing wildcard/var clause.
//  source_name        — file name shown in trace/diagnostic output, NULL ok.
//
typedef struct OptimizationOptions {
    OptimizationLevel  level;
    bool               print_stats;
    bool               trace_semantic;
    bool               fold_strings;
    bool               warn_unreachable;
    bool               warn_nonexhaustive;
    const char        *source_name;
} OptimizationOptions;

/*  optimization_options_default: returns a safe zero-initialised options
 *  struct with level=NONE and all flags off.  Callers set what they need.
 */
OptimizationOptions optimization_options_default(void);


/// Pass statistics
//
//  Filled in by optimize_ast_list.  All fields are cumulative across all
//  passes in the run.  The caller may pass NULL; a local is used then.
//
//  constants_registered   — entries added to the constant table.
//  constants_propagated   — symbol references replaced by their literal.
//  expressions_folded     — arithmetic / comparison / boolean folds fired.
//  branches_folded        — (if …) nodes replaced by a branch.
//  strings_folded         — (str-append …) nodes collapsed to one string.
//  identities_elided      — identity-element rewrites: x+0, x*1, etc.
//  ctor_equalities_folded — (= Ctor Ctor) folds for nullary constructors.
//  switch_candidates      — (if …) chains identified as switch candidates.
//  pmatch_clauses_seen    — total pmatch clauses visited across all matches.
//  pmatch_unreachable     — clauses proven unreachable by an earlier clause.
//  pmatch_nonexhaustive   — pmatch expressions found non-exhaustive.
//  tail_calls_marked      — lambda-body call sites annotated as tail calls.
//  passes_run             — total fixed-point passes executed.
//
typedef struct OptimizationStats {
    size_t constants_registered;
    size_t constants_propagated;
    size_t expressions_folded;
    size_t branches_folded;
    size_t strings_folded;
    size_t identities_elided;
    size_t ctor_equalities_folded;
    size_t switch_candidates;
    size_t pmatch_clauses_seen;
    size_t pmatch_unreachable;
    size_t pmatch_nonexhaustive;
    size_t tail_calls_marked;
    size_t passes_run;
} OptimizationStats;


/// Data-constructor registry
//
//  Populated by a pre-scan of top-level (data …) declarations before the
//  main optimization passes run.  Used by:
//    · ctor_equalities_folded — to know which constructors are nullary
//    · pmatch exhaustiveness  — to know the full constructor set of a type
//
//  Intentionally separate from the dependent checker's own type tables:
//  it only needs constructor *names* and *arities*, not full type
//  information, and must be available before dependent checking runs
//  (this pass executes earlier in the pipeline).
//
typedef struct OptDataType {
    char  *type_name;      // "Color"
    char **ctor_names;     // ["Red", "Green", "Blue"]
    int   *ctor_arities;    // [0, 0, 0]
    int    ctor_count;
} OptDataType;

/*  opt_data_registry_build: scan exprs for top-level (data …) forms and
 *  build a registry of type name -> constructor names/arities.
 *  Returns a heap-allocated array of length *out_count; caller frees with
 *  opt_data_registry_free.  Returns NULL and sets *out_count = 0 if no
 *  data declarations are found.
 */
OptDataType *opt_data_registry_build(ASTList *exprs, size_t *out_count);
void         opt_data_registry_free(OptDataType *types, size_t count);

/*  opt_data_registry_find: look up a type by name. Returns NULL if absent. */
const OptDataType *opt_data_registry_find(const OptDataType *types,
                                           size_t count,
                                           const char *type_name);

/*  opt_data_registry_find_by_ctor: look up the type that declares a given
 *  constructor name (e.g. "Red" -> the "Color" entry).  Returns NULL if no
 *  registered type declares that constructor.
 */
const OptDataType *opt_data_registry_find_by_ctor(const OptDataType *types,
                                                   size_t count,
                                                   const char *ctor_name);


/// AST node-count helper
//
//  Returns the number of AST nodes in t (inclusive of t itself), or 0 if
//  t is NULL.  Exposed as a cheap size heuristic for any future pass that
//  needs to bound code growth (e.g. inlining); not used by the passes in
//  this file today, but kept here rather than duplicated elsewhere.
//
int ast_node_count(AST *t);


/// Free-variable query
//
//  opt_fv_count: count syntactic occurrences of name free in ast, with
//  lambda parameters treated as shadowing binders.  Returns 0 if name or
//  ast is NULL.  This is a syntactic approximation — sufficient for the
//  passes in this file — not a full scope-resolution pass.
//
int opt_fv_count(AST *ast, const char *name);


/// Pattern-match analysis — public query interface
//
//  pattern_subsumes: returns true if pattern `a`, occurring in an earlier
//  clause, matches every value that pattern `b` (a later clause) would
//  match — i.e. b's clause is unreachable if a's clause precedes it and
//  a has no guard.  PAT_WILDCARD and PAT_VAR subsume everything (a bound
//  variable pattern matches any value, just like wildcard, modulo the
//  binding it introduces).
//
//  pattern_covers_constructor: returns true if pattern `p` matches any
//  value built with constructor `ctor_name` (PAT_WILDCARD/PAT_VAR always
//  do; PAT_CONSTRUCTOR only if the name matches).
//
bool pattern_subsumes(const ASTPattern *a, const ASTPattern *b);
bool pattern_covers_constructor(const ASTPattern *p, const char *ctor_name);

/*  pmatch_clause_unreachable: given the pattern of a candidate clause in
 *  one matched column, and the patterns + guard flags of every preceding
 *  clause in that same column, returns true if some earlier *unguarded*
 *  clause's pattern subsumes the candidate (guarded clauses never make a
 *  later clause unreachable, since the guard may fail at runtime).
 */
bool pmatch_clause_unreachable(const ASTPattern *candidate,
                                const ASTPattern *const *prior_patterns,
                                const bool *prior_has_guard,
                                int prior_count);

/*  pmatch_is_exhaustive: given the patterns in one matched column across
 *  all clauses of a pmatch, and the known constructor set of the
 *  scrutinee type, returns true if every constructor is covered by some
 *  clause (PAT_WILDCARD/PAT_VAR cover all constructors trivially).
 */
bool pmatch_is_exhaustive(const ASTPattern *const *patterns,
                          int pattern_count,
                          const OptDataType *type);


/// Range analysis
//
//  opt_range_literal_length: if start/step/end of an AST_RANGE are all
//  AST_NUMBER literals, compute the number of elements the range would
//  produce.  Returns true and sets *out_length on success; returns false
//  if any bound is non-literal, the range is infinite (end == NULL), or
//  step is zero.  Purely advisory: does not allocate or rewrite the AST.
//
bool opt_range_literal_length(AST *range_ast, long long *out_length);


/// Main entry point
//
//  optimize_ast_list: run the full optimization pipeline over a list of
//  top-level expressions in place.
//
//  Preconditions:
//    · exprs must be the output of the desugaring pass.
//    · The ASTList owns its expressions; they will be freed and replaced
//      by rewritten nodes as passes fire.
//    · options must not be NULL; use optimization_options_default() for
//      a no-op run.
//    · stats may be NULL; a local struct is used then.
//
//  After return:
//    · exprs->exprs[i] may point to newly-allocated AST nodes.
//    · All replaced nodes have been freed.
//    · stats (if non-NULL) holds cumulative counts for the run.
//
//  Thread safety: not thread-safe.  Call from a single compiler thread.
//
void optimize_ast_list(ASTList *exprs,
                       const OptimizationOptions *options,
                       OptimizationStats *stats);


/// Single-expression interface
//
//  optimize_expr_standalone: optimize a single AST node without a
//  top-level constant table or data registry.  Useful for REPL
//  evaluation and unit tests where there is no enclosing program.
//
//  Returns the (possibly replaced) root node, owned by the caller.
//  If options is NULL, OPT_LEVEL_BASIC defaults are used.
//
AST *optimize_expr_standalone(AST *expr,
                               const OptimizationOptions *options,
                               OptimizationStats *stats);


/// Constant table — public read-only snapshot
//
//  Exposes the top-level constant table built during the last
//  optimize_ast_list run, so downstream phases (e.g. the dependent
//  checker) can skip re-resolving names already known to be literals.
//  Valid until the next optimize_ast_list call or an explicit free.
//
typedef struct OptConstantSnapshot {
    const char **names;    /* interned; do not free individually */
    AST       **values;    /* read-only; valid until snapshot freed */
    size_t      count;
} OptConstantSnapshot;

OptConstantSnapshot *opt_constant_snapshot(void);
void                 opt_constant_snapshot_free(OptConstantSnapshot *snap);


/// Diagnostic helpers
//
//  opt_stats_print: print a formatted summary of stats to fp.
//  opt_stats_reset: zero all fields of stats.
//
void opt_stats_print(const OptimizationStats *stats,
                     const OptimizationOptions *options,
                     FILE *fp);
void opt_stats_reset(OptimizationStats *stats);


#endif /* OPTIMIZATIONS_H */
