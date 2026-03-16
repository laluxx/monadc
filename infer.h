#ifndef INFER_H
#define INFER_H

#include <stddef.h>
#include <stdbool.h>
#include "types.h"
#include "reader.h"
#include "env.h"

// Forward declaration
typedef struct InferCtx InferCtx;

/// Type Variables
//
//  Type variables are the heart of HM inference.  Each fresh variable
//  gets a unique integer ID.  During unification a variable is either
//  free (bound == NULL) or resolved (bound points to a concrete type or
//  another variable).  The union-find structure lives in the Substitution.

#define INFER_MAX_VARS 4096


/// Type Schemes
//
//  A type scheme ∀a b … T represents a polymorphic type.
//  quantified[] lists the IDs of the universally-quantified variables.
//  type is the body of the scheme (may contain TYPE_VAR nodes whose IDs
//  are in quantified[]).
//
//  Example:
//    identity :: ∀a. a → a
//      .quantified     = {0}
//      .quantified_count = 1
//      .type           = Arrow(Var(0), Var(0))

typedef struct TypeScheme {
    int   *quantified;
    int    quantified_count;
    Type  *type;
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




/// Type Inference Environment
//
//  Maps names to their type schemes.  Separate from the codegen Env —
//  the infer env is purely for type-level bookkeeping and is discarded
//  after inference.  It forms a singly-linked scope chain exactly like
//  the codegen Env.

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
TypeScheme *infer_env_lookup(InferEnv *env, const char *name);


/// Substitution
//
//  The substitution is a mapping from type-variable IDs to types.
//  Implemented as a flat array (dense, since IDs are small integers).
//  subst_apply walks a type and replaces all resolved variables.
//
//  union_find[id] = id means the variable is its own root (free).
//  union_find[id] = other_id means the variable is aliased to other_id.
//  bound[id] is non-NULL only at root nodes, and holds the concrete type.

typedef struct Substitution {
    int   *union_find;   /* parent pointers for union-find          */
    Type **bound;        /* concrete type bound at each root         */
    int    capacity;     /* number of slots allocated                */
    int    next_id;      /* next fresh variable ID                   */
} Substitution;

Substitution *subst_create(void);
void          subst_free(Substitution *s);
int           subst_fresh(Substitution *s);          /* allocate a new free variable   */
int           subst_find(Substitution *s, int id);   /* path-compressed find           */
bool          subst_union(Substitution *s, int a, int b); /* union two var roots       */
void          subst_bind(Substitution *s, int id, Type *t); /* bind root to concrete t */
Type         *subst_apply(Substitution *s, Type *t);  /* walk & substitute             */
Type         *subst_apply_shallow(Substitution *s, Type *t); /* one-level dereference  */


/// Constraint
//
//  Unification constraints T1 ~ T2 are collected during inference and
//  solved in a second pass.  Keeping them separate from the walk makes
//  error reporting easier and allows constraint reordering in the future.

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

typedef struct InferCtx {
    Substitution    *subst;
    TypeConstraint  *constraints;
    size_t           constraint_count;
    size_t           constraint_cap;
    InferEnv        *env;            /* current type environment          */
    const char      *filename;       /* for error messages                */
    bool             had_error;
    char             error_msg[512];
} InferCtx;

InferCtx *infer_ctx_create(InferEnv *env, const char *filename);
void      infer_ctx_free(InferCtx *ctx);


/// Fresh Type Variables

Type *infer_fresh(InferCtx *ctx);          /* allocate a fresh TYPE_VAR         */
Type *infer_fresh_named(InferCtx *ctx, const char *hint); /* same, with debug name */


/// Constraint Generation

void infer_constrain(InferCtx *ctx, Type *a, Type *b, int line, int col);


/// Occurs Check

bool infer_occurs(Substitution *s, int var_id, Type *t);


/// Unification
//
//  Runs Robinson's unification algorithm over all collected constraints.
//  Mutates ctx->subst in place.  Returns false and sets ctx->error_msg
//  on the first type error.

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

Type *infer_expr(InferCtx *ctx, AST *ast);


/// Generalisation and Instantiation
//
//  generalise takes a type and the outer environment and returns a
//  TypeScheme that universally quantifies all type variables that are
//  free in the type but not free in the environment.
//
//  instantiate takes a TypeScheme and replaces each quantified variable
//  with a fresh type variable, returning a new monomorphic Type.

TypeScheme *infer_generalise(InferCtx *ctx, Type *t, InferEnv *outer_env);
Type       *infer_instantiate(InferCtx *ctx, TypeScheme *scheme);


/// Scheme Constructors

TypeScheme *scheme_mono(Type *t);            /* trivial scheme with no quantifiers */
TypeScheme *scheme_clone(TypeScheme *s);
void        scheme_free(TypeScheme *s);


/// Free Variables
//
//  Collect the set of free type-variable IDs in a type or environment.
//  Used by generalise to determine which variables to quantify.

void infer_free_vars_type(Substitution *s, Type *t, int *out, int *count, int cap);
void infer_free_vars_env(InferCtx *ctx, InferEnv *env, int *out, int *count, int cap);


/// Apply Substitution to AST
//
//  After unification, walk the AST and replace every inferred_type with
//  its fully-substituted form.  This is the "zonking" pass.

void infer_zonk_ast(InferCtx *ctx, AST *ast);


/// Primitives Bootstrap
//
//  Populate an InferEnv with the type schemes of all built-in functions
//  and operators so the inferencer can type-check calls to them.

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


/// Pretty Printing (debug)

void infer_print_type(Type *t, Substitution *s);
void infer_print_scheme(TypeScheme *s);
void infer_print_constraints(InferCtx *ctx);


#endif // INFER_H
