#ifndef MONAD_QTT_EFFECT_H
#define MONAD_QTT_EFFECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../qtt/place.h"
#include "../qtt/quantity.h"

typedef struct QttEffectArena QttEffectArena;
typedef struct QttEffectRow QttEffectRow;
typedef struct QttEffectSolver QttEffectSolver;
typedef struct QttEffectScheme QttEffectScheme;

typedef enum {
    QTT_EFFECT_EVIDENCE_NONE,
    QTT_EFFECT_EVIDENCE_SOLVED,
    QTT_EFFECT_EVIDENCE_RESIDUAL,
} QttEffectEvidenceStatus;

typedef enum {
    QTT_EFFECT_COVERAGE_NONE = 0,
    QTT_EFFECT_COVERAGE_OPEN_ROW = 1u << 0,
    QTT_EFFECT_COVERAGE_UNKNOWN_CALLEE = 1u << 1,
    QTT_EFFECT_COVERAGE_INCOMPLETE_CALLEE = 1u << 2,
    QTT_EFFECT_COVERAGE_PARTIAL_LEGACY_CALL = 1u << 3,
    QTT_EFFECT_COVERAGE_ARITY_MISMATCH = 1u << 4,
    QTT_EFFECT_COVERAGE_RESIDUAL_OBLIGATION = 1u << 5,
    QTT_EFFECT_COVERAGE_MISSING_ARROW_CONTRACT = 1u << 6,
} QttEffectCoverageGap;

typedef enum {
    QTT_EFFECT_CUSTOM,
    QTT_EFFECT_IO,
    QTT_EFFECT_EXCEPTION,
    QTT_EFFECT_STATE,
    QTT_EFFECT_READ,
    QTT_EFFECT_WRITE,
    QTT_EFFECT_ALLOCATE,
    QTT_EFFECT_FOREIGN,
    QTT_EFFECT_DIVERGE,
    QTT_EFFECT_ASYNC,
    QTT_EFFECT_CONTROL,
} QttEffectKind;

/* Open, declaration-owned effect vocabulary. The compiler interprets kind as
 * a lowering/ownership trait; name and operation identities are supplied by
 * core declarations and are not a closed compiler list. */
typedef struct QttEffectDeclaration {
    const char *name;
    const char *traits;
    QttEffectKind kind;
    uint64_t constructor_id;
    const char *operation;
    /* Portable operation ABI names. NULL denotes a legacy untyped operation. */
    const char *payload_type;
    const char *result_type;
    /* Alpha-canonical HM arrow scheme; authoritative when present. */
    const char *operation_scheme;
    QttQuantity resumption;
    bool scoped;
} QttEffectDeclaration;

typedef struct QttEffectHandlerProfile {
    const char *name;
    const char *effect_name;
    QttQuantity continuation_usage;
    bool deep;
} QttEffectHandlerProfile;

/* A canonical effect occurrence. Capability identities are generative handler
 * names; type_id is a canonical type identity; resumption is an exact QTT
 * quantity (0 abortive, 1 linear, n finite multi-shot, omega unrestricted).
 *
 * Duplicate rows and principal inference follow Koka's row discipline:
 * https://arxiv.org/abs/1406.2061
 * Generative identities prepare capability/evidence-passing elaboration:
 * https://doi.org/10.1145/3428194
 */
typedef struct {
    const char *traits;
    QttEffectKind kind;
    uint64_t constructor_id;
    uint64_t type_id;
    uint64_t capability_id;
    const char *name;
    const char *operation;
    QttQuantity resumption;
    bool scoped;
} QttEffectAtom;

typedef enum {
    QTT_EFFECT_UNIFY_OK,
    QTT_EFFECT_UNIFY_MISMATCH,
    QTT_EFFECT_UNIFY_OCCURS,
    QTT_EFFECT_UNIFY_OUT_OF_MEMORY,
} QttEffectUnifyResult;

typedef enum {
    QTT_EFFECT_RELATION_PROVED,
    QTT_EFFECT_RELATION_REFUTED,
    QTT_EFFECT_RELATION_UNKNOWN,
} QttEffectRelation;

typedef enum {
    QTT_EFFECT_HANDLER_OK,
    QTT_EFFECT_HANDLER_ABSENT,
    QTT_EFFECT_HANDLER_GRADE_VIOLATION,
    QTT_EFFECT_HANDLER_INVALID,
    QTT_EFFECT_HANDLER_DUPLICATE_CLAUSE,
    QTT_EFFECT_HANDLER_SCOPED_ESCAPE,
} QttEffectHandlerStatus;

typedef struct {
    QttEffectHandlerStatus status;
    QttEffectRow *residual;
    QttQuantity continuation_demand;
} QttEffectHandlerResult;

typedef struct {
    const QttEffectAtom *atom;
    QttQuantity continuation_usage;
    QttQuantity capture_allowance;
    const QttEffectRow *clause_effects;
} QttEffectHandlerClause;

typedef struct {
    QttEffectHandlerStatus status;
    QttEffectRow *residual;
    QttEffectRow *output_effects;
    size_t handled_count;
    uint64_t proof_fingerprint;
} QttEffectHandlerSetResult;

typedef struct {
    unsigned format_version;
    size_t handled_count;
    uint64_t proof_fingerprint;
    uint64_t residual_fingerprint;
    uint64_t output_fingerprint;
} QttEffectHandlerProof;

QttEffectArena *qtt_effect_arena_new(void);
void qtt_effect_arena_free(QttEffectArena *arena);
QttEffectRow *qtt_effect_empty(QttEffectArena *arena);
QttEffectRow *qtt_effect_fresh(QttEffectArena *arena);
QttEffectRow *qtt_effect_extend(
    QttEffectArena *arena, const char *label, QttEffectRow *tail);
QttEffectRow *qtt_effect_extend_atom(
    QttEffectArena *arena, const QttEffectAtom *atom, QttEffectRow *tail);
bool qtt_effect_declaration_register(
    const QttEffectDeclaration *declaration);
const QttEffectDeclaration *qtt_effect_declaration_lookup(const char *name);
const QttEffectDeclaration *qtt_effect_declaration_resolve(
    const char *reference, bool *ambiguous);
bool qtt_effect_declaration_has_trait(
    const QttEffectDeclaration *declaration, const char *trait);
/* True when Core has established the trait through at least one declaration
 * or implication. Used by surface elaboration; this is an open registry
 * query, not a compiler-owned enumeration. */
bool qtt_effect_trait_is_declared(const char *trait);
char *qtt_effect_traits_normalize(const char *traits);
bool qtt_effect_trait_implication_register(
    const char *premise, const char *consequence);
bool qtt_effect_trait_implies(
    const char *premise, const char *consequence);
char *qtt_effect_trait_implication_witness(
    const char *premise, const char *consequence);
void qtt_effect_trait_implications_clear(void);
void qtt_effect_declarations_clear(void);
bool qtt_effect_handler_profile_register(
    const QttEffectHandlerProfile *profile);
const QttEffectHandlerProfile *qtt_effect_handler_profile_lookup(
    const char *name);
void qtt_effect_handler_profiles_clear(void);
QttEffectRow *qtt_effect_extend_declared(
    QttEffectArena *arena, const char *name,
    uint64_t capability_id, uint64_t type_id, QttEffectRow *tail);
bool qtt_effect_atom_equal(
    const QttEffectAtom *left, const QttEffectAtom *right);
uint64_t qtt_effect_atom_fingerprint(const QttEffectAtom *atom);
bool qtt_effect_atom_has_trait(
    const QttEffectAtom *atom, const char *trait);
bool qtt_effect_atom_satisfies_trait(
    const QttEffectAtom *atom, const char *trait);
QttEffectRelation qtt_effect_row_satisfies_trait(
    const QttEffectSolver *solver, const QttEffectRow *row,
    const char *trait, char **witness);
QttEffectRow *qtt_effect_read_place(
    QttEffectArena *arena, QttPlace place, QttEffectRow *tail);
QttEffectRow *qtt_effect_write_place(
    QttEffectArena *arena, QttPlace place, QttEffectRow *tail);

QttEffectSolver *qtt_effect_solver_new(QttEffectArena *arena);
void qtt_effect_solver_free(QttEffectSolver *solver);
QttEffectUnifyResult qtt_effect_unify(
    QttEffectSolver *solver, QttEffectRow *left, QttEffectRow *right);
bool qtt_effect_is_closed(
    const QttEffectSolver *solver, const QttEffectRow *row);
bool qtt_effect_rows_equal(
    const QttEffectSolver *solver,
    const QttEffectRow *left,
    const QttEffectRow *right);
QttEffectRelation qtt_effect_subrow(
    const QttEffectSolver *solver,
    const QttEffectRow *sub,
    const QttEffectRow *super);
QttEffectUnifyResult qtt_effect_refine_subrow(
    QttEffectSolver *solver,
    QttEffectRow *sub,
    QttEffectRow *super,
    QttEffectRelation *relation);
size_t qtt_effect_label_count(
    const QttEffectSolver *solver,
    const QttEffectRow *row,
    const char *label);
/* Constructive negative membership. Known occurrence refutes; absence proves
 * only for a closed row and otherwise remains unknown. */
QttEffectRelation qtt_effect_row_lacks(
    const QttEffectSolver *solver,
    const QttEffectRow *row,
    const char *label);
size_t qtt_effect_atom_count(
    const QttEffectSolver *solver,
    const QttEffectRow *row,
    const QttEffectAtom *atom);
QttEffectRow *qtt_effect_handle_one(
    QttEffectArena *arena,
    const QttEffectSolver *solver,
    const QttEffectRow *row,
    const char *label);
QttEffectRow *qtt_effect_handle_atom(
    QttEffectArena *arena,
    const QttEffectSolver *solver,
    const QttEffectRow *row,
    const QttEffectAtom *atom);
/* Certified handler elimination. If an operation resumes q times and its
 * continuation consumes a captured resource u times per resumption, the
 * capture allowance a must prove q*u <= a. On success exactly one structural
 * atom (including its generative capability identity) is subtracted. */
QttEffectHandlerResult qtt_effect_elaborate_handler(
    QttEffectArena *arena,
    const QttEffectSolver *solver,
    const QttEffectRow *row,
    const QttEffectAtom *atom,
    QttQuantity continuation_usage,
    QttQuantity capture_allowance);
/* Transactional deep-handler judgment for a set of exact operation clauses.
 * Duplicate clauses are incoherent. Clause effects join with the residual
 * computation row; a scoped capability may not reappear in its clause row.
 * Failure returns no residual/output authority. */
QttEffectHandlerSetResult qtt_effect_elaborate_handler_set(
    QttEffectArena *arena,
    const QttEffectSolver *solver,
    const QttEffectRow *computation_effects,
    const QttEffectHandlerClause *clauses,
    size_t clause_count);
bool qtt_effect_verify_handler_set(
    QttEffectArena *arena,
    const QttEffectSolver *solver,
    const QttEffectRow *computation_effects,
    const QttEffectHandlerClause *clauses,
    size_t clause_count,
    uint64_t expected_proof_fingerprint);
char *qtt_effect_handler_proof_serialize(
    const QttEffectSolver *solver,
    const QttEffectHandlerSetResult *result);
bool qtt_effect_handler_proof_deserialize(
    const char *portable,
    QttEffectHandlerProof *proof);
bool qtt_effect_verify_portable_handler_proof(
    QttEffectArena *arena,
    const QttEffectSolver *solver,
    const QttEffectRow *computation_effects,
    const QttEffectHandlerClause *clauses,
    size_t clause_count,
    const char *portable);
/* Least upper bound of two closed effect multisets. Multiplicity joins by
 * maximum, yielding a commutative, associative, idempotent effect lattice. */
QttEffectRow *qtt_effect_join_closed(
    QttEffectArena *arena,
    const QttEffectSolver *solver,
    const QttEffectRow *left,
    const QttEffectRow *right);
/* Join the known multiset components of two rows. If either operand has an
 * unresolved tail, the result receives a fresh existential tail denoting the
 * unknown remainder. This is a sound open-row summary, not a principal union
 * constraint between the operand tails. */
QttEffectRow *qtt_effect_join(
    QttEffectArena *arena,
    const QttEffectSolver *solver,
    const QttEffectRow *left,
    const QttEffectRow *right);
QttEffectRow *qtt_effect_clone_closed(
    QttEffectArena *arena,
    const QttEffectSolver *solver,
    const QttEffectRow *row);
uint64_t qtt_effect_row_fingerprint(
    const QttEffectSolver *solver,
    const QttEffectRow *row);
char *qtt_effect_format(
    const QttEffectSolver *solver, const QttEffectRow *row);

QttEffectScheme *qtt_effect_generalize(
    QttEffectArena *arena,
    const QttEffectSolver *solver,
    const QttEffectRow *row);
QttEffectScheme *qtt_effect_scheme_retain(QttEffectScheme *scheme);
size_t qtt_effect_scheme_quantified_count(
    const QttEffectScheme *scheme);
uint64_t qtt_effect_scheme_fingerprint(
    const QttEffectScheme *scheme);
bool qtt_effect_scheme_set_evidence(
    QttEffectScheme *scheme, const char *portable_certificate,
    size_t obligation_count, uint64_t certificate_fingerprint,
    QttEffectEvidenceStatus status);
const char *qtt_effect_scheme_evidence(const QttEffectScheme *scheme);
size_t qtt_effect_scheme_evidence_count(const QttEffectScheme *scheme);
uint64_t qtt_effect_scheme_evidence_fingerprint(
    const QttEffectScheme *scheme);
QttEffectEvidenceStatus qtt_effect_scheme_evidence_status(
    const QttEffectScheme *scheme);
bool qtt_effect_scheme_evidence_intact(const QttEffectScheme *scheme);
void qtt_effect_scheme_set_coverage_gaps(
    QttEffectScheme *scheme, uint32_t gaps);
uint32_t qtt_effect_scheme_coverage_gaps(const QttEffectScheme *scheme);
char *qtt_effect_coverage_format(uint32_t gaps);
bool qtt_effect_scheme_set_callable_parameters(
    QttEffectScheme *scheme, const size_t *indices, size_t count);
bool qtt_effect_scheme_set_callable_parameter_contracts(
    QttEffectScheme *scheme, const size_t *indices,
    const size_t *arities, size_t count);
size_t qtt_effect_scheme_callable_parameter_count(
    const QttEffectScheme *scheme);
size_t qtt_effect_scheme_callable_parameter_index(
    const QttEffectScheme *scheme, size_t index);
size_t qtt_effect_scheme_callable_parameter_arity(
    const QttEffectScheme *scheme, size_t index);
char *qtt_effect_scheme_serialize(const QttEffectScheme *scheme);
QttEffectScheme *qtt_effect_scheme_deserialize(const char *text);
bool qtt_effect_scheme_is_empty(const QttEffectScheme *scheme);
QttEffectRow *qtt_effect_instantiate(
    QttEffectArena *arena, const QttEffectScheme *scheme);
QttEffectRow *qtt_effect_instantiate_with_tail(
    QttEffectArena *arena, const QttEffectScheme *scheme,
    QttEffectRow *replacement_tail);
void qtt_effect_scheme_free(QttEffectScheme *scheme);
uint64_t qtt_effect_tail_variable(const QttEffectRow *row);

#endif
