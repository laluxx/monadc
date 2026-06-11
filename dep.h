#ifndef DEP_H
#define DEP_H

/*  dep.h — Dependent Type Checker
 *
 *  Implements a kernel of Intensional Type Theory (ITT) with:
 *    · Π-types  (dependent functions)
 *    · Σ-types  (dependent pairs)
 *    · Universe hierarchy  (Type 0 : Type 1 : ...)
 *    · Inductive families (W-types, used to encode Nat, Vec, Fin, …)
 *    · Metavariables (holes) solved by unification of normal forms
 *    · Bidirectional type checking  (check / infer / elaborate)
 *    · Definitional equality via β,δ,η reduction to WHNF
 *    · Locally nameless representation
 *        — bound variables  ->  De Bruijn indices (no α-equivalence bugs)
 *        — free  variables  ->  global names (readable error messages)
 *    · Interop with the existing ground-type system (types.h)
 *        — TERM_EMBED wraps a Type* so ground types are valid terms
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "types.h"    // Type*, TypeKind — ground types are embedded as terms
#include "reader.h"   // AST — surface syntax elaborated into Term

/// Forward declarations

typedef struct Term        Term;
typedef struct DepCtx      DepCtx;
typedef struct DepEnv      DepEnv;
typedef struct MetaCtx     MetaCtx;
typedef struct Spine       Spine;
typedef struct Value       Value;
typedef struct Closure     Closure;
typedef struct DepError    DepError;


/// Universe levels
//
//  Level expressions track universe polymorphism.
//
//  A level is either:
//    · A concrete non-negative integer  (LEVEL_CONST)
//    · A level variable (for universe-polymorphic definitions)
//    · max(u, v)  — the join of two levels
//    · u + k      — a level successor offset
//
//  Concrete levels 0, 1, 2 … correspond to Type₀, Type₁, Type₂ …
//  The predicative universe rule is:
//
//    ──────────────────────────────────────  (universe)
//          Γ ⊢  Type u  :  Type (u+1)
//
//     Γ ⊢ A : Type u    Γ, x:A ⊢ B : Type v
//    ───────────────────────────────────────  (Π-formation)
//        Γ ⊢ Π(x:A).B  :  Type (max u v)
//
#define DEP_LEVEL_MAX 64   // hard ceiling; prevents runaway universe ladders

typedef enum {
    LEVEL_CONST,   // a concrete integer ≥ 0
    LEVEL_VAR,     // level variable (for universe-polymorphic defs)
    LEVEL_MAX,     // max(left, right)
    LEVEL_SUCC,    // left + offset
} LevelKind;

typedef struct Level {
    LevelKind    kind;
    int          value;     // LEVEL_CONST: the integer; LEVEL_SUCC: offset
    int          var_id;    // LEVEL_VAR: unique ID
    struct Level *left;     // LEVEL_MAX, LEVEL_SUCC: base expression
    struct Level *right;    // LEVEL_MAX: second argument
} Level;

Level *level_const(int n);
Level *level_var(int id);
Level *level_max(Level *u, Level *v);
Level *level_succ(Level *u);            // u + 1
Level *level_clone(Level *u);
void   level_free(Level *u);
int    level_eval(Level *u);            // reduce to a concrete int or -1
bool   level_leq(Level *u, Level *v);   // u ≤ v after evaluation
bool   levels_equal(Level *u, Level *v);
const char *level_to_string(Level *u);


/// Term  — core language
//
//  Terms are the unified representation of both programs and types.
//  This is the defining property of dependent type theory: the same
//  syntactic objects are used for values and their types.
//
//  Binding convention — Locally Nameless
//  --------------------------------------
//  Bound variables (introduced by TERM_LAM, TERM_PI, TERM_SIG, TERM_LET)
//  are represented as De Bruijn indices (TERM_BVAR).  The index n refers to
//  the n-th enclosing binder (0 = innermost).  Free variables (globals,
//  definitions) are represented by name (TERM_FVAR).
//
//    Example:  λ(x:A). λ(y:B). x
//    Core:     Lam A (Lam (B↑) (BVar 1))
//                          ↑ shifted because it lives under one extra binder
//
//  Metavariables (TERM_META) are placeholders that the elaborator fills in.
//  They carry a unique ID into a global MetaCtx.
//
//  Elimination terms are represented in spine form:
//    TERM_APP  f  [a₁, a₂, …, aₙ]  — multi-argument application
//  This flattens nested left-associative applications and speeds up WHNF.
//
typedef enum {
    // ── Variables ────────────────────────────────────────────────────
    TERM_BVAR,      // bound variable via De Bruijn index
    TERM_FVAR,      // free variable / global name

    // ── Universe ─────────────────────────────────────────────────────
    TERM_TYPE,      // Type u  — the universe at level u
    TERM_KIND,      // Kind    — the sort of Type (used internally only)

    // ── Function types / lambdas ─────────────────────────────────────
    TERM_PI,        // Π(x:A). B  — dependent function type
    TERM_LAM,       // λ(x:A). t  — lambda abstraction
    TERM_APP,       // f a₁ a₂ …  — application (flat spine)

    // ── Σ-types / pairs ──────────────────────────────────────────────
    TERM_SIGMA,     // Σ(x:A). B  — dependent pair type
    TERM_PAIR,      // ⟨t, s⟩     — pair introduction
    TERM_FST,       // fst p      — first projection
    TERM_SND,       // snd p      — second projection

    // ── Identity / equality ───────────────────────────────────────────
    TERM_EQ,        // t ≡ s : A   — propositional equality type
    TERM_REFL,      // refl : t ≡ t
    TERM_SUBST,     // subst (h : a ≡ b) P pa — transport along equality

    // ── Inductive / W-types ───────────────────────────────────────────
    TERM_NAT,       // Nat type (primitive builtin, avoids bootstrapping)
    TERM_ZERO,      // zero : Nat
    TERM_SUCC_T,    // succ n : Nat
    TERM_NUM_LIT,   // n : Nat (fast path)
    TERM_NAT_ELIM,  // Nat-elim P pz ps n — primitive recursion on Nat

    // ── Let-bindings & Control Flow ───────────────────────────────────
    TERM_LET,       // let x : A = t in body
    TERM_IF,        // if cond then t else e

    // ── Metavariables ────────────────────────────────────────────────
    TERM_META,      // ?₁, ?₂, … — holes to be solved by elaboration
    TERM_HOLE,      // _ — anonymous hole, elaborator must fill

    // ── Interop with ground-type system ───────────────────────────────
    TERM_EMBED,     // wraps a Type* from types.h as an atomic term/type

    // ── Annotations ───────────────────────────────────────────────────
    TERM_ANN,       // (t : A) — type ascription
} TermKind;

//  Implicitness tag for Π-binders.
//  Implicit arguments ({x:A}) are filled by the elaborator automatically.
//  Explicit arguments ((x:A)) must be supplied by the programmer.
typedef enum {
    IMPLICIT_EXPLICIT = 0,  // (x : A) in source
    IMPLICIT_IMPLICIT = 1,  // {x : A} — elaborator fills
    IMPLICIT_INSTANCE = 2,  // ⦃x : A⦄ — resolved by instance search
} Implicitness;

struct Term {
    TermKind kind;

    // ── TERM_BVAR ──────────────────────────────────────────────────
    int        bvar_index;        // De Bruijn index

    // ── TERM_FVAR ──────────────────────────────────────────────────
    char      *fvar_name;         // global / free variable name

    // ── TERM_TYPE ──────────────────────────────────────────────────
    Level     *type_level;        // universe level

    // ── TERM_PI / TERM_LAM / TERM_SIGMA / TERM_LET ────────────────
    char       *binder_name;      // source name (for error messages)
    struct Term *binder_dom;      // domain type A
    struct Term *binder_body;     // body B (TERM_LET: the value too)
    struct Term *let_type;        // TERM_LET: declared type
    struct Term *let_val;         // TERM_LET: bound value
    Implicitness implicit;        // TERM_PI: implicitness of argument

    // ── TERM_APP ───────────────────────────────────────────────────
    struct Term  *app_fn;         // function term
    struct Term **app_args;       // argument spine (owned array)
    int           app_argc;       // number of arguments

    // ── TERM_PAIR / TERM_FST / TERM_SND ───────────────────────────
    struct Term  *pair_fst;       // first component  / source term
    struct Term  *pair_snd;       // second component
    struct Term  *pair_type;      // TERM_PAIR: type annotation Σ(x:A).B

    // ── TERM_EQ ────────────────────────────────────────────────────
    struct Term  *eq_lhs;         // left-hand side of equality
    struct Term  *eq_rhs;         // right-hand side
    struct Term  *eq_type;        // the type A in t ≡ s : A

    // ── TERM_REFL ──────────────────────────────────────────────────
    struct Term  *refl_val;       // the term t in refl {A} t

    // ── TERM_SUBST ─────────────────────────────────────────────────
    struct Term  *subst_proof;    // proof  h : a ≡ b
    struct Term  *subst_motive;   // motive P : A -> Type
    struct Term  *subst_base;     // proof  pa : P a

    // ── TERM_SUCC_T / TERM_NUM_LIT ─────────────────────────────────
    struct Term  *succ_pred;      // predecessor
    unsigned long long num_lit;   // primitive integer value

    // ── TERM_IF ────────────────────────────────────────────────────
    struct Term  *if_cond;
    struct Term  *if_then;
    struct Term  *if_else;

    // ── TERM_NAT_ELIM ──────────────────────────────────────────────
    struct Term  *nelim_motive;   // P  : Nat -> Type
    struct Term  *nelim_zero;     // pz : P zero
    struct Term  *nelim_succ;     // ps : Π(n:Nat). P n -> P (succ n)
    struct Term  *nelim_arg;      // n  : Nat

    // ── TERM_META ──────────────────────────────────────────────────
    int           meta_id;        // index into MetaCtx

    // ── TERM_EMBED ─────────────────────────────────────────────────
    Type         *embed_type;     // ground Type* from types.h

    // ── TERM_ANN ───────────────────────────────────────────────────
    struct Term  *ann_term;       // annotated term
    struct Term  *ann_type;       // type annotation

    // ── Source location and Interop ───────────────────────────────
    int           line;
    int           col;
    struct AST   *source_ast;     // For back-propagating types to HM
};


/// Constructors

Term *term_bvar(int index);
Term *term_fvar(const char *name);
Term *term_type(Level *level);
Term *term_type_n(int n);             // Type n — convenience
Term *term_kind(void);
Term *term_pi(const char *name, Term *dom, Term *body, Implicitness impl);
Term *term_lam(const char *name, Term *dom, Term *body);
Term *term_app(Term *fn, Term **args, int argc);   // takes ownership
Term *term_app1(Term *fn, Term *arg);              // single-arg sugar
Term *term_sigma(const char *name, Term *dom, Term *body);
Term *term_pair(Term *fst, Term *snd, Term *type);
Term *term_fst(Term *pair);
Term *term_snd(Term *pair);
Term *term_eq(Term *lhs, Term *rhs, Term *type);
Term *term_refl(Term *val);
Term *term_subst(Term *proof, Term *motive, Term *base);
Term *term_nat(void);
Term *term_zero(void);
Term *term_succ(Term *pred);
Term *term_num_lit(unsigned long long n);
Term *term_nat_elim(Term *motive, Term *pz, Term *ps, Term *n);
Term *term_if(Term *cond, Term *t, Term *e);
Term *term_let(const char *name, Term *type, Term *val, Term *body);
Term *term_meta(int id);
Term *term_hole(void);
Term *term_embed(Type *ground_type);
Term *term_ann(Term *t, Term *ty);


/// Operations

Term       *term_clone(Term *t);
void        term_free(Term *t);
const char *term_to_string(Term *t);
bool        terms_syntactically_equal(Term *a, Term *b);  // α-eq, no reduction


/// Substitution on core terms
//
//  These are the *structural* substitution operations on Term trees.
//  They are used by the evaluator (dep_whnf / dep_eval) and during
//  elaboration.  They do NOT interact with the MetaCtx.
//
//  dep_subst_bvar: replace De Bruijn index 0 in body with s, shifting.
//    This is the standard β-reduction step:
//      (λx.body)[x↦s]  ≡  dep_subst_bvar(body, s)
//
//  dep_shift: adjust all free De Bruijn indices in t by delta,
//    skipping indices < cutoff (which refer to binders inside t).
//    Required whenever you move a term under a new binder.
//
//  dep_subst_fvar: replace all occurrences of the free variable `name`
//    with replacement.  Used when unfolding global definitions (δ-reduction).
//
Term *dep_subst_bvar(Term *body, Term *s);
Term *dep_shift(Term *t, int delta, int cutoff);
Term *dep_subst_fvar(Term *t, const char *name, Term *replacement);


// Values and closures  — semantic domain for the evaluator
//
//  Evaluation uses a closure-based NbE (Normalisation by Evaluation)
//  approach.  Terms evaluate to Values; Values are compared for
//  definitional equality without revisiting the syntactic term.
//
//  A Closure captures an unevaluated body together with an environment.
//  When a lambda is applied, the closure is instantiated with the argument
//  producing a new value.
//
//  Neutral terms (stuck applications) are preserved so that we can quote
//  (reify) values back into terms for error messages and for the
//  definitional equality check.
//
//  This NbE model follows Abel, "Normalization by Evaluation: Dependent
//  Types and Impredicativity" (2013), simplified to avoid levitation.
//
typedef struct Closure {
    struct EvalEnv *env;    // the environment at closure creation time
    Term           *body;   // the unevaluated body (De Bruijn term)
} Closure;

typedef enum {
    VAL_UNIVERSE,   // Type u
    VAL_PI,         // Π(x:A).B — domain value + closure for codomain
    VAL_SIGMA,      // Σ(x:A).B — domain value + closure for second
    VAL_LAM,        // λ(x:A).t — domain value + closure for body
    VAL_PAIR,       // ⟨v₁, v₂⟩
    VAL_NAT,        // Nat
    VAL_ZERO,       // zero
    VAL_SUCC,       // succ v
    VAL_NUM_LIT,    // primitive integer value
    VAL_EQ,         // a ≡ b : A (all values)
    VAL_REFL,       // refl v
    VAL_IF,         // stuck if expression
    VAL_NEUTRAL,    // stuck term: a free var or meta applied to a spine
    VAL_EMBED,      // ground Type* from types.h
    VAL_META,       // unsolved metavariable ?n applied to a spine
} ValKind;

//  A Spine is a (possibly empty) list of arguments applied to a neutral.
//  neutral(f)[a₁, a₂, …] means f applied to a₁, then a₂, etc.
struct Spine {
    Value        **args;
    int            count;
    int            cap;
};

struct Value {
    ValKind kind;

    // VAL_UNIVERSE
    Level   *level;

    // VAL_PI / VAL_SIGMA / VAL_LAM
    char     *binder_name;
    Value    *domain;     // type of the bound variable
    Closure   closure;    // body as a closure waiting for an argument
    Implicitness implicit; // VAL_PI only

    // VAL_PAIR
    Value    *fst;
    Value    *snd;

    // VAL_SUCC / VAL_NUM_LIT
    Value    *pred;
    unsigned long long num_lit;

    // VAL_EQ
    Value    *eq_lhs;
    Value    *eq_rhs;
    Value    *eq_type_val;

    // VAL_REFL
    Value    *refl_val;

    // VAL_IF
    Value    *if_cond_val;
    Value    *if_then_val;
    Value    *if_else_val;

    // VAL_NEUTRAL / VAL_META
    char     *neutral_name;   // VAL_NEUTRAL: free variable name
    int       neutral_level;  // VAL_NEUTRAL: binding level (-1 for globals)
    int       meta_id;        // VAL_META: metavariable ID
    Spine     spine;          // arguments accumulated on the neutral

    // VAL_EMBED
    Type     *embed_type;
};

Value *val_universe(Level *level);
Value *val_universe_n(int n);
Value *val_pi(const char *name, Value *dom, Closure body, Implicitness impl);
Value *val_sigma(const char *name, Value *dom, Closure body);
Value *val_lam(const char *name, Value *dom, Closure body);
Value *val_pair(Value *fst, Value *snd);
Value *val_nat(void);
Value *val_zero(void);
Value *val_succ(Value *pred);
Value *val_num_lit(unsigned long long n);
Value *val_eq(Value *lhs, Value *rhs, Value *type);
Value *val_refl(Value *v);
Value *val_if(Value *c, Value *t, Value *e);
Value *val_neutral(const char *name, int level, Spine spine);
Value *val_meta(int id, Spine spine);
Value *val_embed(Type *t);
void   val_free(Value *v);
void   val_spine_push(Spine *sp, Value *arg);   // append one argument
Spine  val_spine_empty(void);
Spine  val_spine_clone(Spine sp);
void   val_spine_free(Spine sp);


/// Environment  — semantic runtime environment for NbE
//
//  The semantic environment maps De Bruijn levels (not indices!) to Values.
//  Using levels (counting from the outside in) rather than indices
//  (counting from the inside out) means that values in the environment
//  never need to be shifted when we extend the environment.
//
//  Convention:  env->level is the count of binders entered so far.
//    The value at level k is accessed via env_lookup(env, k).
//    A fresh variable at the current level is val_neutral(name, empty_spine).
//    When we instantiate a closure c with value v:
//      dep_closure_apply(c, v)  extends env with v at the next level.
//
typedef struct EvalEnvEntry {
    Value               *val;
    struct EvalEnvEntry *next;
} EvalEnvEntry;

typedef struct EvalEnv {
    EvalEnvEntry *head;    // stack of bound values, innermost first
    int           level;   // number of binders entered (= depth)
} EvalEnv;

EvalEnv *eval_env_empty(void);
EvalEnv *eval_env_extend(EvalEnv *e, Value *v);  // push v; returns new EvalEnv (owned)
Value   *eval_env_lookup(EvalEnv *e, int index); // De Bruijn index lookup
void     eval_env_free(EvalEnv *e);              // frees the chain but not values
void     eval_env_discard_top(EvalEnv *e);       // frees only the top entry
EvalEnv *eval_env_clone(EvalEnv *e);


/// Closure application

Value *dep_closure_apply(Closure c, Value *arg);


/// Evaluation (NbE)
//
//  dep_eval:  Term × Env -> Value
//    Evaluate a closed term in an environment to a Value.
//    This is the "meaning" function of the NbE model.
//    Terminates for strongly-normalising terms (total programs).
//
//  dep_quote: Value × depth -> Term
//    Reify a value back into a De Bruijn term at the given binding depth.
//    Fresh variables are introduced for neutral applications.
//    The result is in β-normal η-long form.
//
//  dep_whnf: Term × Env -> Value (then immediately quoted back)
//    Reduce to weak-head normal form.  Cheaper than full normalisation
//    and sufficient for the conversion check.
//
//  dep_normalise: Term × Env -> Term
//    Full normalisation: eval then quote.  Used for error messages.
//
Value *dep_eval(Term *t, EvalEnv *env, MetaCtx *mctx);
Term  *dep_quote(Value *v, int depth, MetaCtx *mctx);
Term  *dep_normalise(Term *t, EvalEnv *env, MetaCtx *mctx);
Value *dep_force(Value *v, MetaCtx *mctx); // resolve metas at top level


/// Metavariable context
//
//  Metavariables (?₀, ?₁, …) are generated during elaboration whenever
//  the programmer writes _ or omits an implicit argument.  Each meta has
//  a type and is either UNSOLVED or SOLVED with a closed Term.
//
//  Constraint solving during unification (dep_conv) fills metas.
//  After elaboration all metas must be solved; unsolved metas are errors.
//
//  The "occurs check" for metas prevents infinite-type loops just as in HM,
//  but now operates on Values rather than on the union-find tree.
//
typedef enum {
    META_UNSOLVED,
    META_SOLVED,
} MetaState;

typedef struct MetaEntry {
    int        id;
    MetaState  state;
    Value     *type;     // the expected type of this meta
    Term      *solution; // SOLVED: the closed term (no free De Bruijn)
    int        depth;    // binding depth at creation — for occurs check
    char      *hint;     // debug name (may be NULL)
} MetaEntry;

struct MetaCtx {
    MetaEntry *entries;
    int        count;
    int        cap;
};

MetaCtx  *meta_ctx_create(void);
void      meta_ctx_free(MetaCtx *mctx);
int       meta_fresh(MetaCtx *mctx, Value *type, int depth, const char *hint);
bool      meta_solve(MetaCtx *mctx, int id, Term *solution); // false = occurs
MetaEntry *meta_lookup(MetaCtx *mctx, int id);
bool      term_occurs_meta(int id, Term *t);
bool      meta_occurs(MetaCtx *mctx, int id, Value *v);      // occurs check
bool      meta_all_solved(MetaCtx *mctx);                    // post-elab check


/// Definitional equality  — the heart of dependent type checking
//
//  Definitional equality replaces syntactic unification from infer.c.
//
//  Two terms are definitionally equal if they reduce to the same normal
//  form (modulo α-equivalence, which De Bruijn indices handle for free).
//
//  The algorithm is:
//    1. Evaluate both terms to Values under the current environment.
//    2. Force any solved metavariables to their solutions.
//    3. Compare structurally:
//       · Same constructor -> recurse on subterms (instantiating closures
//         with a fresh neutral value at the current depth).
//       · One side is an unsolved meta -> try to unify (solve the meta).
//       · Different constructors -> not equal (type error).
//
//  Η-expansion is applied for functions (compare (λx.f x) and f by
//  applying both to the same fresh neutral) and for pairs.
//
//  This check is algorithmic and decidable for *total* programs.
//  Non-termination in the user's code can cause the checker to loop;
//  a fuel/step counter (DEP_CONV_FUEL) is provided as a safeguard.
//
#define DEP_CONV_FUEL 100000  // max reduction steps before timeout

typedef struct ConvCtx {
    DepCtx  *dctx;
    int      depth;    // current binding depth (for fresh variable gen)
    int      fuel;     // remaining reduction steps
    bool     had_error;
    char     error_msg[512];
} ConvCtx;

ConvCtx conv_ctx_make(DepCtx *dctx, int depth);

//  dep_conv: check that v₁ and v₂ are definitionally equal at type ty.
//  Returns true and may solve metavariables as a side-effect.
//  On failure writes into cctx->error_msg.
bool dep_conv(ConvCtx *cctx, Value *v1, Value *v2, Value *ty);

//  dep_conv_terms: convenience wrapper that evaluates t1 and t2 first.
bool dep_conv_terms(DepCtx *dctx, Term *t1, Term *t2, int depth);


/// Typing context  — Γ in Γ ⊢ t : T
//
//  The typing context tracks:
//    · Local binders (x : A) introduced by Π, λ, Σ, let — as a stack.
//    · Global definitions (f := t : A) from the top-level environment.
//
//  Local binders are stored as a linked list (telescope).  Each entry
//  carries the *value* of the type (a Value*) so that the conversion
//  checker can compare it against expected types directly.
//
//  Global definitions are stored in DepEnv (a separate hash table) so
//  that δ-reduction (unfolding) can look up definitions by name.
//
typedef struct DepCtxEntry {
    char              *name;    // source-level name (for error messages)
    Value             *type;    // the type of this variable
    Value             *val;     // optional: defined value (for δ-red.)
    struct DepCtxEntry *next;
} DepCtxEntry;

struct DepCtx {
    DepCtxEntry  *locals;   // telescope: innermost entry first
    int           depth;    // number of local binders = current level
    DepEnv       *globals;  // global definitions
    MetaCtx      *mctx;     // metavariable table (shared across ctx)
    EvalEnv      *env;      // semantic environment for NbE
    const char   *filename; // for error messages
    bool          had_error;
    char          error_msg[512];
};

DepCtx *dep_ctx_create(const char *filename);
DepCtx *dep_ctx_child(DepCtx *parent);          // push a new scope
void    dep_ctx_free(DepCtx *ctx);
//
// Push a local binding (x : A) into the context.
// Internally:
//   · adds a DepCtxEntry to ctx->locals
//   · extends ctx->env with a fresh neutral Val for x
//   · increments ctx->depth
//
void    dep_ctx_push(DepCtx *ctx, const char *name, Value *type);
void    dep_ctx_push_def(DepCtx *ctx, const char *name, Value *type, Value *val);
void    dep_ctx_pop(DepCtx *ctx);

DepCtxEntry *dep_ctx_lookup_local(DepCtx *ctx, const char *name);
Value       *dep_ctx_type_at_level(DepCtx *ctx, int level);


/// Global environment  — top-level definitions
//
//  DepEnv maps names to their types and (optionally) their definitions.
//  It uses the same hash-table layout as InferEnv for consistency.
//
//  dep_env_declare  — adds a name with its type but no definition yet
//                     (for forward-referencing axioms / parameters).
//  dep_env_define   — adds a name with both type and definition
//                     (enabling δ-reduction / unfolding).
//
#define DEP_ENV_BUCKETS 128

typedef struct DepEnvEntry {
    char             *name;
    Value            *type;     // type of the definition
    Term             *def_term; // the definition's core term (or NULL)
    Value            *def_val;  // the definition's value (or NULL)
    bool              opaque;   // if true, never δ-unfold
    struct DepEnvEntry *next;
} DepEnvEntry;

struct DepEnv {
    DepEnvEntry    **buckets;
    size_t           size;
    struct DepEnv   *parent;
};

DepEnv     *dep_env_create(void);
DepEnv     *dep_env_create_child(DepEnv *parent);
void        dep_env_free(DepEnv *env);
void        dep_env_declare(DepEnv *env, const char *name, Value *type);
void        dep_env_define(DepEnv *env, const char *name,
                           Value *type, Term *def_term, Value *def_val,
                           bool opaque);
DepEnvEntry *dep_env_lookup(DepEnv *env, const char *name);


/// Bidirectional type checker
//
//  Bidirectional type checking splits the judgment into two modes:
//
//    INFER (synthesis):   Γ ⊢ t ⇒ A
//      The type A is *computed* from t.  Used for:
//        · Variables (look up in context)
//        · Annotations  (t : A)
//        · Applications (if the function type is known)
//        · Type constants
//
//    CHECK (analysis):    Γ ⊢ t ⇐ A
//      The type A is *known* and t is verified against it.  Used for:
//        · Lambda bodies (type flows in from Π)
//        · Pairs (type flows in from Σ)
//        · refl (forces both sides to be definitionally equal)
//
//  The algorithm alternates between the two modes following the recipe
//  of Löh, McBride, Swierstra "A Tutorial Implementation of a
//  Dependently Typed Lambda Calculus" (2010), extended with Σ, equality,
//  and metavariables.
//
//  Both functions return NULL on error (ctx->had_error is set).
//
Value *dep_infer(DepCtx *ctx, Term *t);
bool   dep_check(DepCtx *ctx, Term *t, Value *expected_type);
//
//  dep_infer_type: infer the *kind* of A (where A is already known to
//  be a type).  Returns the universe level, or -1 on error.
//
int    dep_infer_level(DepCtx *ctx, Term *ty);


/// Elaboration  — surface AST -> core Term
//
//  Elaboration translates the surface-syntax AST (from reader.h) into
//  well-typed core Terms.  It:
//    · Inserts implicit arguments (where Π-types demand them).
//    · Generates fresh metavariables for _ holes.
//    · Resolves overloaded names.
//    · Translates pattern matching into eliminators.
//    · Reports scope and arity errors.
//
//  dep_elab_term: main entry — elaborates ast into a Term, checking
//    against expected_type (or inferring it if expected_type is NULL).
//
//  dep_elab_type: elaborate ast as a type expression specifically
//    (must produce something of type Type u for some u).
//
Term  *dep_elab_term(DepCtx *ctx, AST *ast, Value *expected_type);
Term  *dep_elab_type(DepCtx *ctx, AST *ast);
Value *dep_elab_and_infer(DepCtx *ctx, AST *ast);   // elab then infer


/// Interop: surface AST <-> core Term
//
//  dep_term_of_ast: translate a surface AST into a pre-elaboration Term.
//  This is a syntactic translation only — it does not type-check.
//  Call dep_elab_term (or dep_infer) afterwards for type checking.
//
//  dep_term_of_type: translate a type annotation AST specifically,
//  recognising Π-notation, Σ-notation, equality types, and the universes.
//
//  dep_term_of_ground: wrap a types.h Type* as a TERM_EMBED so that
//  existing ground-typed code can be used as terms/types in the
//  dependent checker without rewriting.
//
Term *dep_term_of_ast(DepCtx *ctx, AST *ast);
Term *dep_term_of_type_ast(DepCtx *ctx, AST *ast);
Term *dep_term_of_ground(Type *ground_type);
//
//  dep_ground_of_value: attempt to extract a ground Type* from a
//  normalised Value.  Returns NULL if the value is not a ground type.
//  Used by codegen to bridge back to the existing backend.
//
Type  *dep_ground_of_value(Value *v, MetaCtx *mctx);
Type  *dep_ground_of_value_env(Value *v, MetaCtx *mctx, DepEnv *globals);


/// Top-level elaboration pipeline
//
//  dep_toplevel: main entry point called by the compiler/REPL.
//
//  Pipeline:
//    1. dep_term_of_ast       — surface -> pre-term (syntactic)
//    2. dep_elab_term         — pre-term -> core term (elaborate)
//    3. dep_infer / dep_check — type checking (bidirectional)
//    4. meta_all_solved       — verify all holes are filled
//    5. dep_normalise         — normalise result type for printing
//
//  Returns the fully-checked core Term, or NULL on error.
//  The type of the result is placed in *out_type (if non-NULL).
//
Term *dep_toplevel(DepCtx *ctx, AST *ast, Term **out_type);


/// Builtins bootstrap
//
//  dep_register_builtins: populate the global DepEnv with the types and
//  definitions of all built-in constants.  Must be called once after
//  dep_ctx_create before any elaboration.
//
//  Registered builtins:
//    · Nat  : Type 0
//    · zero : Nat
//    · succ : Nat -> Nat
//    · Nat-elim : Π(P : Nat -> Type u). P zero -> (Π(n:Nat). P n -> P (succ n))
//                 -> Π(n:Nat). P n
//    · Bool / True / False / Bool-elim (via Nat encoding)
//    · Σ-types: fst / snd
//    · Equality: refl / subst / sym / trans / cong
//    · Embedding of all types.h ground types as Type 0 constants
//
void dep_register_builtins(DepCtx *ctx);


/// Error reporting

struct DepError {
    char  message[1024];
    int   line;
    int   col;
    Term *expected;   // may be NULL
    Term *got;        // may be NULL
};

void dep_error_set(DepCtx *ctx, int line, int col, const char *fmt, ...);
void dep_error_print(DepCtx *ctx);
void dep_report_holes(DepCtx *ctx);


/// Pretty printing

void dep_print_term(Term *t);
void dep_print_value(Value *v, MetaCtx *mctx);
void dep_print_ctx(DepCtx *ctx);
void dep_print_meta_ctx(MetaCtx *mctx);


#endif /* DEP_H */
