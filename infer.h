#ifndef INFER_H
#define INFER_H

#include <stddef.h>
#include <stdbool.h>
#include "types.h"
#include "reader.h"
#include "env.h"

// Forward declaration
typedef struct InferCtx InferCtx;
struct QttGradeArena;
struct QttGradeExpr;
struct QttGradeScheme;
struct QttEffectArena;
struct QttEffectSolver;
struct QttEffectRow;
struct QttEffectScheme;
struct QttClosureEnvironment;
struct QttEffectConstraintSet;

typedef struct {
    struct QttEffectScheme *effect_scheme;
    bool effects_complete;
    struct QttEffectScheme **arrow_effect_schemes;
    bool *arrow_effects_complete;
    size_t arrow_effect_count;
    size_t *effect_trait_predicate_stages;
    char **effect_trait_predicate_names;
    size_t effect_trait_predicate_count;
} InferCallableContract;

typedef struct {
    Type *type;
    struct QttEffectScheme *effects;
    bool effects_complete;
    struct QttEffectScheme **arrow_effect_schemes;
    bool *arrow_effects_complete;
    size_t arrow_effect_count;
    size_t *effect_trait_predicate_stages;
    char **effect_trait_predicate_names;
    size_t effect_trait_predicate_count;
    bool recursively_fused;
} InferExpressionJudgment;

/// Type Variables
//
//  Type variables are the heart of HM inference.  Each fresh variable
//  gets a unique integer ID.  During unification a variable is either
//  free (bound == NULL) or resolved (bound points to a concrete type or
//  another variable).  The union-find structure lives in the Substitution.
//
#define INFER_MAX_VARS  4096
#define INFER_MAX_HOLES 256

typedef struct InferHole {
    int line;
    int col;
    int var_id;   /* the fresh type variable ID assigned to this hole */
} InferHole;

typedef struct InferGradeApplication {
    const AST *application;
    struct QttGradeExpr **domain_grades;
    size_t applied_count;
    uint64_t *closure_module_ids;
    uint64_t *closure_ids;
    uint64_t *closure_binder_ids;
    size_t *closure_slots;
    size_t *closure_parameter_indices;
    int *closure_origin_kinds;
    uint64_t *closure_origin_ids;
    struct QttGradeExpr **closure_grades;
    size_t closure_grade_count;
    uint64_t *closure_domain_module_ids;
    uint64_t *closure_domain_ids;
    size_t *closure_domain_indices;
    struct QttGradeExpr **closure_domain_grades;
    size_t closure_domain_grade_count;
    uint64_t *result_closure_module_ids;
    uint64_t *result_closure_ids;
    uint64_t *result_closure_instance_ids;
    struct QttClosureEnvironment **result_closure_environments;
    size_t result_closure_count;
    size_t *callable_parameter_indices;
    struct QttGradeExpr **callable_invocation_grades;
    size_t callable_parameter_count;
    size_t *callable_domain_parameter_indices;
    size_t *callable_domain_indices;
    struct QttGradeExpr **callable_domain_grades;
    size_t callable_domain_count;
} InferGradeApplication;


/// Type Schemes
//
//  A type scheme ∀a b ... T represents a polymorphic type.
//  quantified[] lists the IDs of the universally-quantified variables.
//  type is the body of the scheme (may contain TYPE_VAR nodes whose IDs
//  are in quantified[]).
//
//  Example:
//    identity :: ∀a. a -> a
//      .quantified     = {0}
//      .quantified_count = 1
//      .type           = Arrow(Var(0), Var(0))
typedef struct TypeScheme {
    int   *quantified;
    int    quantified_count;
    Type  *type;
    struct QttGradeScheme *grade_scheme;
    struct QttEffectScheme *effect_scheme;
    bool effects_complete;
    struct QttEffectScheme **arrow_effect_schemes;
    bool *arrow_effects_complete;
    size_t arrow_effect_count;
    size_t *effect_trait_predicate_stages;
    char **effect_trait_predicate_names;
    size_t effect_trait_predicate_count;
    bool owns_type;
} TypeScheme;


// Maps quantified type variable IDs to their concrete types at a call site.
// Used by codegen to drive monomorphization.
typedef struct TypeSubst {
    int   *from;    // quantified var IDs from the scheme
    Type **to;      // concrete types they were bound to
    int    count;
} TypeSubst;

// Like infer_instantiate but also returns the substitution mapping.
// Caller must free ts->from and ts->to (but not the Type* they point to).
Type *infer_instantiate_with_subst(InferCtx *ctx, TypeScheme *scheme,
                                    TypeSubst *ts);
void infer_set_trace(bool enabled);


/// Type Inference Environment
//
//  Maps names to their type schemes.  Separate from the codegen Env —
//  the infer env is purely for type-level bookkeeping and is discarded
//  after inference.  It forms a singly-linked scope chain exactly like
//  the codegen Env.
//
typedef struct InferEnvEntry {
    char                 *name;
    TypeScheme           *scheme;
    struct InferEnvEntry *next;
} InferEnvEntry;

typedef struct InferEnv {
    InferEnvEntry  **buckets;
    size_t           size;
    struct InferEnv *parent;
} InferEnv;

InferEnv *infer_env_create(void);
InferEnv *infer_env_create_child(InferEnv *parent);
void      infer_env_free(InferEnv *env);
void      infer_env_insert(InferEnv *env, const char *name, TypeScheme *scheme);
TypeScheme *infer_env_lookup(InferCtx *ctx, const char *name);


/// Substitution
//
//  The substitution is a mapping from type-variable IDs to types.
//  Implemented as a flat array (dense, since IDs are small integers).
//  subst_apply walks a type and replaces all resolved variables.
//
//  union_find[id] = id means the variable is its own root (free).
//  union_find[id] = other_id means the variable is aliased to other_id.
//  bound[id] is non-NULL only at root nodes, and holds the concrete type.
//
typedef struct Substitution {
    int   *union_find;   // parent pointers for union-find
    Type **bound;        // concrete type bound at each root
    int    capacity;     // number of slots allocated
    int    next_id;      // next fresh variable ID
} Substitution;

Substitution *subst_create(void);
void          subst_free(Substitution *s);
int           subst_fresh(Substitution *s);          // allocate a new free variable
int           subst_find(Substitution *s, int id);   // path-compressed find
bool          subst_union(Substitution *s, int a, int b); // union two var roots
void          subst_bind(Substitution *s, int id, Type *t); // bind root to concrete t
Type         *subst_apply(Substitution *s, Type *t);  // walk & substitute
Type         *infer_substitute_type_vars(Type *t, int *from, Type **to,
                                         int count);
Type         *subst_apply_shallow(Substitution *s, Type *t); // one-level dereference


/// Constraint
//
//  Unification constraints T1 ~ T2 are collected during inference and
//  solved in a second pass.  Keeping them separate from the walk makes
//  error reporting easier and allows constraint reordering in the future.
//
typedef struct TypeConstraint {
    Type *lhs;
    Type *rhs;
    int   line;   /* source location for error messages */
    int   col;
} TypeConstraint;


/// Inference Context
//
//  Carries all mutable state needed during a single inference run.
//  One InferCtx is created per top-level definition (or REPL expression)
//  and discarded afterwards.
//
typedef struct InferCtx {
    Substitution    *subst;
    TypeConstraint  *constraints;
    size_t           constraint_count;
    size_t           constraint_cap;
    InferEnv        *env;            // local HM environment
    struct DepCtx   *dctx;           // TT Master Global Scope
    const char      *filename;       // for error messages
    bool             had_error;
    char             error_msg[512];
    /* Hole tracking — explicit ? in expressions */
    bool             has_holes;
    int              hole_count;
    InferHole        hole_positions[INFER_MAX_HOLES];
    struct QttGradeArena *grade_arena;
    struct QttEffectArena *effect_arena;
    struct QttEffectSolver *effect_solver;
    size_t last_effect_constraint_count;
    uint64_t last_effect_certificate_fingerprint;
    bool last_effect_constraints_residual;
    InferGradeApplication *grade_applications;
    size_t grade_application_count;
    size_t grade_application_cap;
    uint64_t next_closure_instance_id;
} InferCtx;

InferCtx *infer_ctx_create(InferEnv *env, struct DepCtx *dctx, const char *filename);
void      infer_ctx_free(InferCtx *ctx);


/// Fresh Type Variables

Type *infer_fresh(InferCtx *ctx);          // allocate a fresh TYPE_VAR
Type *infer_fresh_named(InferCtx *ctx, const char *hint); // same, with debug name
Type *infer_freshen_annotation_vars(InferCtx *ctx, Type *t,
                                    int *from, Type **to, int *count);


/// Constraint Generation

void infer_constrain(InferCtx *ctx, Type *a, Type *b, int line, int col);


/// Occurs Check

bool infer_occurs(Substitution *s, int var_id, Type *t);


/// Unification
//
//  Runs Robinson's unification algorithm over all collected constraints.
//  Mutates ctx->subst in place.  Returns false and sets ctx->error_msg
//  on the first type error.
//
bool infer_unify_all(InferCtx *ctx);
bool infer_unify_one(InferCtx *ctx, Type *a, Type *b, int line, int col);


/// Type Inference — Expression Walk
//
//  infer_expr walks an AST node and returns its inferred type.
//  It also annotates ast->inferred_type on every node (requires the
//  AST struct to have an inferred_type field — see reader.h).
//
//  All constraints are deferred into ctx->constraints; actual solving
//  happens in infer_unify_all after the full expression is walked.
//
Type *infer_expr(InferCtx *ctx, AST *ast);


/// Generalisation and Instantiation
//
//  generalise takes a type and the outer environment and returns a
//  TypeScheme that universally quantifies all type variables that are
//  free in the type but not free in the environment.
//
//  instantiate takes a TypeScheme and replaces each quantified variable
//  with a fresh type variable, returning a new monomorphic Type.
//
TypeScheme *infer_generalise(InferCtx *ctx, Type *t, InferEnv *outer_env);
TypeScheme *infer_generalise_excluding(
    InferCtx *ctx, Type *t, InferEnv *outer_env, const char *excluded_name);
Type       *infer_instantiate(InferCtx *ctx, TypeScheme *scheme);

/*
 * A quantitative instantiation is one coherent view of an HM instance:
 * every leading arrow domain is paired with exactly one freshly-instantiated
 * grade.  Advancing the view consumes both components together, preventing
 * partial application from silently shifting or dropping grade evidence.
 */
typedef enum InferQuantitativeResult {
    INFER_QUANTITATIVE_OK,
    INFER_QUANTITATIVE_INVALID_ARGUMENT,
    INFER_QUANTITATIVE_GRADE_ARITY_MISMATCH,
    INFER_QUANTITATIVE_EFFECT_ARITY_MISMATCH,
    INFER_QUANTITATIVE_OUT_OF_MEMORY,
} InferQuantitativeResult;

typedef struct InferQuantitativeType {
    Type *type;
    struct QttGradeExpr **domain_grades;
    size_t domain_count;
    size_t domain_offset;
    uint64_t *closure_module_ids;
    uint64_t *closure_ids;
    uint64_t *closure_binder_ids;
    size_t *closure_slots;
    size_t *closure_parameter_indices;
    int *closure_origin_kinds;
    uint64_t *closure_origin_ids;
    struct QttGradeExpr **closure_grades;
    size_t closure_grade_count;
    uint64_t *closure_domain_module_ids;
    uint64_t *closure_domain_ids;
    size_t *closure_domain_indices;
    struct QttGradeExpr **closure_domain_grades;
    size_t closure_domain_grade_count;
    uint64_t *result_closure_module_ids;
    uint64_t *result_closure_ids;
    size_t result_closure_count;
    size_t *callable_parameter_indices;
    struct QttGradeExpr **callable_invocation_grades;
    size_t callable_parameter_count;
    size_t *callable_domain_parameter_indices;
    size_t *callable_domain_indices;
    struct QttGradeExpr **callable_domain_grades;
    size_t callable_domain_count;
    struct QttEffectArena *effect_arena;
    struct QttEffectSolver *effect_solver;
    struct QttEffectRow *latent_effects;
    bool effects_complete;
    struct QttEffectRow **domain_effects;
    bool *domain_effects_complete;
    size_t domain_effect_count;
    size_t domain_effect_offset;
} InferQuantitativeType;

InferQuantitativeResult infer_instantiate_quantitative(
    InferCtx *ctx, TypeScheme *scheme, InferQuantitativeType *out);
bool infer_quantitative_take_domain(
    InferQuantitativeType *instance,
    Type **parameter,
    struct QttGradeExpr **grade);
bool infer_quantitative_take_domain_contract(
    InferQuantitativeType *instance,
    Type **parameter,
    struct QttGradeExpr **grade,
    struct QttEffectRow **effects,
    bool *effects_complete);
struct QttGradeExpr *infer_quantitative_closure_grade(
    const InferQuantitativeType *instance,
    uint64_t module_id,
    uint64_t closure_id,
    uint64_t binder_id);
struct QttGradeExpr *infer_quantitative_closure_slot_grade(
    const InferQuantitativeType *instance,
    uint64_t module_id,
    uint64_t closure_id,
    size_t slot);
struct QttGradeExpr *infer_quantitative_closure_domain_grade(
    const InferQuantitativeType *instance,
    uint64_t module_id,
    uint64_t closure_id,
    size_t parameter_index);
struct QttGradeExpr *infer_quantitative_callable_domain_grade(
    const InferQuantitativeType *instance,
    size_t callable_parameter_index,
    size_t domain_index);
void infer_quantitative_type_free(InferQuantitativeType *instance);
size_t infer_grade_application_count(const InferCtx *ctx);
const InferGradeApplication *infer_grade_application(
    const InferCtx *ctx, size_t index);


/// Scheme Constructors

TypeScheme *scheme_mono(Type *t);            /* trivial scheme with no quantifiers */
TypeScheme *scheme_clone(TypeScheme *s);
void        scheme_free(TypeScheme *s);
char *infer_type_scheme_serialize(const TypeScheme *scheme);
TypeScheme *infer_type_scheme_deserialize(const char *text);
char *infer_operation_scheme_serialize(
    const char *payload_type, const char *result_type);
bool infer_operation_scheme_instantiate(
    InferCtx *ctx, const char *portable_scheme,
    Type **payload_type, Type **result_type);
bool infer_operation_scheme_accepts(
    const char *portable_scheme,
    Type *payload_type, Type *result_type);
bool scheme_set_effect_trait_predicates(
    TypeScheme *scheme, const size_t *stages,
    const char *const *traits, size_t count);
size_t scheme_effect_trait_predicate_count(const TypeScheme *scheme);
size_t scheme_effect_trait_predicate_stage(
    const TypeScheme *scheme, size_t index);
const char *scheme_effect_trait_predicate_name(
    const TypeScheme *scheme, size_t index);
bool scheme_instantiate_effect_trait_constraints(
    const TypeScheme *scheme, struct QttEffectArena *arena,
    struct QttEffectConstraintSet *constraints);
void scheme_set_grade_scheme(
    TypeScheme *scheme, struct QttGradeScheme *grades);
void scheme_set_effect_scheme(
    TypeScheme *scheme, struct QttEffectScheme *effects,
    bool complete);
bool scheme_set_arrow_effect_schemes(
    TypeScheme *scheme,
    struct QttEffectScheme *const *effects,
    const bool *complete,
    size_t count);
bool infer_callable_contract_from_judgment(
    InferCallableContract *contract,
    const InferExpressionJudgment *judgment);
bool infer_callable_contract_from_scheme(
    InferCallableContract *contract, const TypeScheme *scheme);
bool scheme_set_callable_contract(
    TypeScheme *scheme, const InferCallableContract *contract);
bool infer_validate_effect_annotations(
    InferCtx *ctx, const TypeScheme *scheme,
    size_t *failing_stage, const char **failing_label);
bool infer_elaborate_effect_row_binders(
    InferCtx *ctx, TypeScheme *scheme);
uint64_t infer_callable_contract_fingerprint(
    const InferCallableContract *contract);
char *infer_callable_contract_serialize(
    const InferCallableContract *contract);
bool infer_callable_contract_deserialize(
    InferCallableContract *contract, const char *text);
void infer_callable_contract_free(InferCallableContract *contract);
uint64_t scheme_effect_fingerprint(const TypeScheme *scheme);
struct QttEffectScheme *infer_effect_scheme_for_lambda(
    InferCtx *ctx, const struct AST *lambda, bool *complete);
struct QttEffectScheme *infer_effect_scheme_for_expression(
    InferCtx *ctx, const struct AST *expression, bool *complete);
size_t infer_effect_constraint_count(const InferCtx *ctx);
uint64_t infer_effect_certificate_fingerprint(const InferCtx *ctx);
bool infer_effect_constraints_residual(const InferCtx *ctx);
size_t scheme_grade_count(const TypeScheme *scheme);
size_t scheme_closure_grade_count(const TypeScheme *scheme);
struct QttGradeExpr **scheme_instantiate_grades(
    const TypeScheme *scheme,
    struct QttGradeArena *target,
    size_t *count);
struct QttGradeExpr **infer_instantiate_scheme_grades(
    InferCtx *ctx, const TypeScheme *scheme, size_t *count);


/// Free Variables
//
//  Collect the set of free type-variable IDs in a type or environment.
//  Used by generalise to determine which variables to quantify.
//
void infer_free_vars_type(Substitution *s, Type *t, int *out, int *count, int cap);
void infer_free_vars_env(InferCtx *ctx, InferEnv *env, int *out, int *count, int cap);


/// Apply Substitution to AST
//
//  After unification, walk the AST and replace every inferred_type with
//  its fully-substituted form.  This is the "zonking" pass.
//
void infer_zonk_ast(InferCtx *ctx, AST *ast);


/// Primitives Bootstrap
//
//  Populate an InferEnv with the type schemes of all built-in functions
//  and operators so the inferencer can type-check calls to them.
//
void infer_register_builtins(InferCtx *ctx);


/// Top-level Entry Points
//
//  infer_toplevel is the main entry point called by the REPL and compiler.
//  It runs the full pipeline:
//    1. infer_expr          — constraint generation
//    2. infer_unify_all     — constraint solving
//    3. infer_zonk_ast      — substitution application
//
//  Returns the fully-solved type of the expression, or NULL on error.
//  Sets ctx->had_error and ctx->error_msg on failure.

Type *infer_toplevel(InferCtx *ctx, AST *ast);
bool infer_toplevel_judgment(
    InferCtx *ctx, AST *ast, InferExpressionJudgment *judgment);
void infer_expression_judgment_free(InferExpressionJudgment *judgment);


/// Pretty Printing (debug)

void infer_print_type(Type *t, Substitution *s);
void infer_print_scheme(TypeScheme *s);
void infer_print_constraints(InferCtx *ctx);
void infer_report_holes(InferCtx *ctx);


#endif // INFER_H
