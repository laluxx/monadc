#ifndef MONAD_FRONTEND_TRANSACTION_H
#define MONAD_FRONTEND_TRANSACTION_H

#include "../reader.h"
#include "../reader_diagnostic.h"
#include "../wisp.h"
#include "../qtt/quantity.h"

typedef struct MonadFrontendState MonadFrontendState;
typedef struct MonadCompilationUnit MonadCompilationUnit;
struct TypeScheme;
MonadFrontendState *monad_frontend_state_create(void);
void monad_frontend_state_destroy(MonadFrontendState *state);
bool monad_frontend_state_register_arity(
    MonadFrontendState *state, const char *name, int arity);
bool monad_frontend_state_register_nominal(
    MonadFrontendState *state, const char *name);
bool monad_frontend_state_has_nominal(
    MonadFrontendState *state, const char *name);
bool monad_frontend_state_register_alias(
    MonadFrontendState *state, const char *alias, const char *target);
bool monad_frontend_state_alias_targets(
    MonadFrontendState *state, const char *alias, const char *target);
bool monad_frontend_state_register_refinement(
    MonadFrontendState *state, const char *name, const char *predicate,
    const char *base_type);
bool monad_frontend_state_has_refinement(
    MonadFrontendState *state, const char *name, const char *predicate);
bool monad_frontend_state_register_finite_member(
    MonadFrontendState *state, const char *name, const char *member);
bool monad_frontend_state_finite_contains(
    MonadFrontendState *state, const char *name, const char *member);
bool monad_frontend_state_register_layout(
    MonadFrontendState *state, const char *layout, const char *field);
bool monad_frontend_state_has_layout_field(
    MonadFrontendState *state, const char *layout, const char *field);
bool monad_frontend_state_add_feature(
    MonadFrontendState *state, const char *feature);
bool monad_frontend_state_has_feature(
    MonadFrontendState *state, const char *feature);
WispInputStatus monad_frontend_classify_input(
    MonadFrontendState *state, const char *source,
    bool terminated_by_blank_line);

typedef struct MonadFrontendDiagnostic {
    int line;
    int column;
    int end_column;
    char message[512];
} MonadFrontendDiagnostic;

typedef struct MonadFrontendResult {
    ASTList ast;
    bool has_diagnostic;
    MonadFrontendDiagnostic diagnostic;
} MonadFrontendResult;

/* Internal compilation lifetime. The state remains directly bound from begin
 * through destruction; the unit owns any successfully parsed AST. */
MonadCompilationUnit *monad_compilation_unit_begin(MonadFrontendState *state);
bool monad_compilation_unit_parse(
    MonadCompilationUnit *unit, const char *source, const char *filename,
    MonadFrontendResult *result);
bool monad_compilation_unit_infer(
    MonadCompilationUnit *unit, MonadFrontendResult *result);
bool monad_compilation_unit_inference_committed(
    const MonadCompilationUnit *unit);
bool monad_compilation_unit_has_committed_binding(
    const MonadCompilationUnit *unit, const char *name);
size_t monad_compilation_unit_definition_count(
    const MonadCompilationUnit *unit);
bool monad_compilation_unit_definition_at(
    const MonadCompilationUnit *unit, size_t index, const char **name,
    const struct TypeScheme **scheme, const AST **source_ast,
    const char **docstring);
bool monad_compilation_unit_definition_quantities(
    const MonadCompilationUnit *unit, size_t index,
    const QttQuantity **parameters, size_t *parameter_count,
    const QttQuantity **closures, size_t *closure_count);
typedef struct MonadQttResourceSummary {
    bool verified;
    bool destructor_plan_available;
    int result_representation;
    size_t block_count;
    size_t instruction_count;
    uint64_t certificate_fingerprint;
} MonadQttResourceSummary;
bool monad_compilation_unit_definition_resource(
    const MonadCompilationUnit *unit, size_t index,
    MonadQttResourceSummary *summary);
size_t monad_compilation_unit_ast_count(const MonadCompilationUnit *unit);
void monad_compilation_unit_destroy(MonadCompilationUnit *unit);

bool monad_frontend_parse(
    const char *source, const char *filename, MonadFrontendResult *result);
bool monad_frontend_parse_in_state(
    MonadFrontendState *state, const char *source, const char *filename,
    MonadFrontendResult *result);
void monad_frontend_result_destroy(MonadFrontendResult *result);
typedef void (*MonadFrontendEntryProbe)(void *context);
void monad_frontend_set_entry_probe(
    MonadFrontendEntryProbe probe, void *context);

#endif
