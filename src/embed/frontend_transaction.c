#include "frontend_transaction.h"

#include "../wisp.h"
#include "../features.h"
#include "../types.h"
#include "../infer.h"
#include "../effects/effect.h"
#include "../qtt/pipeline.h"
#include "../qtt/bindings.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A protected host-language call turns source rejection into data instead of
 * process control. Lua documents the mature analogue of this boundary here:
 * https://www.lua.org/manual/5.4/manual.html#4.4
 */

static MonadFrontendEntryProbe frontend_entry_probe;
static void *frontend_entry_probe_context;
static const char *frontend_definition_name(const AST *ast);
static const AST *frontend_definition_lambda(const AST *ast);

static uint64_t frontend_module_id(const char *identity) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)identity; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}

static void frontend_hash_word(uint64_t *hash, uint64_t word) {
    for (size_t i = 0; i < sizeof(word); i++) {
        *hash ^= (unsigned char)(word >> (i * 8));
        *hash *= UINT64_C(1099511628211);
    }
}

static uint64_t frontend_resource_evidence_fingerprint(
    const QttShadowEvidence *evidence) {
    if (!evidence || evidence->summary.status != QTT_SHADOW_VERIFIED) return 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    frontend_hash_word(&hash, evidence->contract_fingerprint);
    frontend_hash_word(&hash, evidence->summary.block_count);
    frontend_hash_word(&hash, evidence->summary.instruction_count);
    frontend_hash_word(&hash, evidence->result_representation);
    frontend_hash_word(&hash, evidence->result_type_id.value);
    for (size_t i = 0; i < evidence->parameter_count; i++) {
        frontend_hash_word(&hash, evidence->parameters[i].mode);
        frontend_hash_word(&hash, evidence->parameters[i].representation);
        frontend_hash_word(&hash, evidence->parameters[i].type_id.value);
        frontend_hash_word(&hash, evidence->parameters[i].allowance.finite);
        frontend_hash_word(&hash, evidence->parameters[i].allowance.is_omega);
        frontend_hash_word(&hash, evidence->parameters[i].observed.finite);
        frontend_hash_word(&hash, evidence->parameters[i].observed.is_omega);
    }
    for (size_t i = 0; i < evidence->block_count; i++)
        for (size_t j = 0; j < evidence->blocks[i].instruction_count; j++) {
            const QttShadowInstructionEvidence *instruction =
                &evidence->blocks[i].instructions[j];
            frontend_hash_word(&hash, instruction->kind);
            frontend_hash_word(&hash, instruction->representation);
            frontend_hash_word(&hash, instruction->resource.module_id);
            frontend_hash_word(&hash, instruction->resource.binder_id);
        }
    return hash ? hash : 1;
}

struct MonadFrontendState {
    WispPersistentState *wisp;
    ReaderPersistentState *reader;
    TypesPersistentState *types;
    FeaturePersistentState *features;
};

struct MonadCompilationUnit {
    /* One owner couples semantic context and generated resources, following
     * LLVM's context isolation and ORCv2 resource-tracker lifetime model:
     * https://llvm.org/doxygen/classllvm_1_1LLVMContext.html
     * https://llvm.org/docs/ORCv2.html#resource-trackers
     */
    MonadFrontendState *state;
    ASTList ast;
    /* Γ₀ is stable builtins; source bindings accumulate only in child Δ.
     * Δ becomes publishable iff every judgment succeeds, mirroring ORCv2's
     * all-or-fail materialization responsibility:
     * https://llvm.org/docs/ORCv2.html#materialization-units-and-materialization-responsibility
     */
    InferEnv *infer_base_env;
    InferEnv *infer_env;
    /* Inferred AST annotations borrow context-owned substitution/effect data,
     * so contexts have the unit's arena-like lifetime. LLVM documents the
     * corresponding allocate-together/free-together discipline here:
     * https://llvm.org/doxygen/classllvm_1_1BumpPtrAllocatorImpl.html
     */
    InferCtx **infer_contexts;
    size_t infer_context_count;
    QttQuantity **parameter_usages;
    size_t *parameter_usage_counts;
    QttQuantity **closure_usages;
    size_t *closure_usage_counts;
    MonadQttResourceSummary *resource_summaries;
    char *filename;
    bool parsed;
    bool inferred;
    bool inference_committed;
};

static bool frontend_state_enter(MonadFrontendState *state) {
    if (!state || !wisp_persistent_state_enter(state->wisp)) return false;
    if (!reader_persistent_state_enter(state->reader)) goto reader_failed;
    if (!types_persistent_state_enter(state->types)) goto types_failed;
    if (!feature_persistent_state_enter(state->features)) goto features_failed;
    return true;
features_failed:
    types_persistent_state_leave(state->types);
types_failed:
    reader_persistent_state_leave(state->reader);
reader_failed:
    wisp_persistent_state_leave(state->wisp);
    return false;
}

static void frontend_state_leave(MonadFrontendState *state) {
    if (!state) return;
    feature_persistent_state_leave(state->features);
    types_persistent_state_leave(state->types);
    reader_persistent_state_leave(state->reader);
    wisp_persistent_state_leave(state->wisp);
}

MonadFrontendState *monad_frontend_state_create(void) {
    MonadFrontendState *state = calloc(1, sizeof(*state));
    if (!state) return NULL;
    state->wisp = wisp_persistent_state_create();
    state->reader = reader_persistent_state_create();
    state->types = types_persistent_state_create();
    state->features = feature_persistent_state_create();
    if (!state->wisp || !state->reader || !state->types || !state->features) {
        wisp_persistent_state_destroy(state->wisp);
        reader_persistent_state_destroy(state->reader);
        types_persistent_state_destroy(state->types);
        feature_persistent_state_destroy(state->features);
        free(state);
        return NULL;
    }
    return state;
}

void monad_frontend_state_destroy(MonadFrontendState *state) {
    if (!state) return;
    wisp_persistent_state_destroy(state->wisp);
    reader_persistent_state_destroy(state->reader);
    types_persistent_state_destroy(state->types);
    feature_persistent_state_destroy(state->features);
    free(state);
}

bool monad_frontend_state_register_arity(
    MonadFrontendState *state, const char *name, int arity) {
    if (!state || !name || !frontend_state_enter(state))
        return false;
    wisp_register_arity(name, arity);
    frontend_state_leave(state);
    return true;
}

bool monad_frontend_state_register_nominal(
    MonadFrontendState *state, const char *name) {
    if (!frontend_state_enter(state)) return false;
    bool result = type_nominal_register(name);
    frontend_state_leave(state);
    return result;
}

bool monad_frontend_state_has_nominal(
    MonadFrontendState *state, const char *name) {
    if (!frontend_state_enter(state)) return false;
    bool result = type_nominal_is_registered(name);
    frontend_state_leave(state);
    return result;
}

bool monad_frontend_state_register_alias(
    MonadFrontendState *state, const char *alias, const char *target) {
    if (!alias || !target || !frontend_state_enter(state)) return false;
    type_alias_register(alias, target);
    frontend_state_leave(state);
    return true;
}

bool monad_frontend_state_alias_targets(
    MonadFrontendState *state, const char *alias, const char *target) {
    if (!alias || !target || !frontend_state_enter(state)) return false;
    bool result = type_name_is_subtype(alias, target);
    frontend_state_leave(state);
    return result;
}

bool monad_frontend_state_register_refinement(
    MonadFrontendState *state, const char *name, const char *predicate,
    const char *base_type) {
    if (!name || !predicate || !base_type || !frontend_state_enter(state))
        return false;
    refinement_register(name, predicate, base_type, NULL, NULL);
    frontend_state_leave(state);
    return true;
}

bool monad_frontend_state_has_refinement(
    MonadFrontendState *state, const char *name, const char *predicate) {
    if (!name || !predicate || !frontend_state_enter(state)) return false;
    const char *stored = refinement_pred_name(name);
    bool result = stored && strcmp(stored, predicate) == 0;
    frontend_state_leave(state);
    return result;
}

bool monad_frontend_state_register_finite_member(
    MonadFrontendState *state, const char *name, const char *member) {
    if (!name || !member || !frontend_state_enter(state)) return false;
    const char *members[] = {member};
    bool result = finite_type_set_register(name, members, 1);
    frontend_state_leave(state);
    return result;
}

bool monad_frontend_state_finite_contains(
    MonadFrontendState *state, const char *name, const char *member) {
    if (!name || !member || !frontend_state_enter(state)) return false;
    bool result = finite_type_set_contains_symbol(name, member, NULL);
    frontend_state_leave(state);
    return result;
}

bool monad_frontend_state_register_layout(
    MonadFrontendState *state, const char *layout, const char *field) {
    if (!layout || !field || !frontend_state_enter(state)) return false;
    ASTLayoutField value = {.name = (char *)field};
    register_layout_fields(layout, &value, 1);
    frontend_state_leave(state);
    return true;
}

bool monad_frontend_state_has_layout_field(
    MonadFrontendState *state, const char *layout, const char *field) {
    if (!field || !frontend_state_enter(state)) return false;
    const char *stored = layout_get_field_name(layout, 0);
    bool result = stored && strcmp(stored, field) == 0;
    frontend_state_leave(state);
    return result;
}

bool monad_frontend_state_add_feature(
    MonadFrontendState *state, const char *feature) {
    if (!frontend_state_enter(state)) return false;
    bool result = feature_register(feature);
    frontend_state_leave(state);
    return result;
}

bool monad_frontend_state_has_feature(
    MonadFrontendState *state, const char *feature) {
    if (!frontend_state_enter(state)) return false;
    bool result = has_feature(feature) != 0;
    frontend_state_leave(state);
    return result;
}

WispInputStatus monad_frontend_classify_input(
    MonadFrontendState *state, const char *source,
    bool terminated_by_blank_line) {
    if (!state || !frontend_state_enter(state))
        return WISP_INPUT_INVALID;
    WispInputStatus status =
        wisp_classify_input(source, terminated_by_blank_line);
    frontend_state_leave(state);
    return status;
}

void monad_frontend_set_entry_probe(
    MonadFrontendEntryProbe probe, void *context) {
    frontend_entry_probe = probe;
    frontend_entry_probe_context = context;
}

static void frontend_capture(
    void *opaque, const ReaderDiagnostic *diagnostic) {
    MonadFrontendResult *result = opaque;
    result->has_diagnostic = true;
    result->diagnostic.line = diagnostic->line;
    result->diagnostic.column = diagnostic->column;
    result->diagnostic.end_column = diagnostic->end_column;
    snprintf(result->diagnostic.message, sizeof(result->diagnostic.message),
             "%s", diagnostic->message ? diagnostic->message : "parser error");
}

static bool frontend_parse_bound(
    const char *source, const char *filename, MonadFrontendResult *result) {
    if (!result) return false;
    memset(result, 0, sizeof(*result));
    if (frontend_entry_probe)
        frontend_entry_probe(frontend_entry_probe_context);

    ReaderDiagnosticContext diagnostic;
    ReaderDiagnosticCleanup ast_cleanup;
    ReaderAstLedger ledger;
    reader_diagnostic_context_push(&diagnostic, frontend_capture, result);
    reader_ast_ledger_begin(&ledger);
    if (!reader_diagnostic_cleanup_defer(
            &diagnostic, &ast_cleanup, reader_ast_ledger_abort, &ledger)) {
        reader_ast_ledger_commit(&ledger);
        reader_diagnostic_context_pop(&diagnostic);
        return false;
    }

    if (setjmp(diagnostic.escape) != 0) return false;
    reader_diagnostic_context_arm(&diagnostic);
    result->ast = wisp_parse_all(source ? source : "", filename);
    if (!reader_diagnostic_cleanup_cancel(&diagnostic, &ast_cleanup)) abort();
    reader_ast_ledger_commit(&ledger);
    if (!reader_diagnostic_context_pop(&diagnostic)) abort();
    return true;
}

MonadCompilationUnit *monad_compilation_unit_begin(MonadFrontendState *state) {
    if (!state || !frontend_state_enter(state)) return NULL;
    MonadCompilationUnit *unit = calloc(1, sizeof(*unit));
    if (!unit) {
        frontend_state_leave(state);
        return NULL;
    }
    unit->state = state;
    unit->infer_base_env = infer_env_create();
    unit->infer_env = unit->infer_base_env
        ? infer_env_create_child(unit->infer_base_env) : NULL;
    if (!unit->infer_base_env || !unit->infer_env) {
        infer_env_free(unit->infer_env);
        infer_env_free(unit->infer_base_env);
        frontend_state_leave(state);
        free(unit);
        return NULL;
    }
    return unit;
}

bool monad_compilation_unit_parse(
    MonadCompilationUnit *unit, const char *source, const char *filename,
    MonadFrontendResult *result) {
    if (!unit || unit->parsed || !result) return false;
    unit->parsed = true;
    const char *unit_filename = filename ? filename : "<input>";
    size_t filename_size = strlen(unit_filename) + 1;
    unit->filename = malloc(filename_size);
    if (!unit->filename) {
        memset(result, 0, sizeof(*result));
        return false;
    }
    memcpy(unit->filename, unit_filename, filename_size);
    bool accepted = frontend_parse_bound(source, filename, result);
    if (accepted) {
        unit->ast = result->ast;
        memset(&result->ast, 0, sizeof(result->ast));
    }
    return accepted;
}

size_t monad_compilation_unit_ast_count(const MonadCompilationUnit *unit) {
    return unit ? unit->ast.count : 0;
}

bool monad_compilation_unit_infer(
    MonadCompilationUnit *unit, MonadFrontendResult *result) {
    if (!unit || !unit->parsed || unit->inferred || !result) return false;
    unit->inferred = true;
    memset(result, 0, sizeof(*result));
    if (!unit->ast.count) {
        unit->inference_committed = true;
        return true;
    }
    QttBindingSummary bindings = qtt_bindings_resolve_many(
        unit->ast.exprs, unit->ast.count);
    if (bindings.error != QTT_BINDINGS_OK) {
        result->has_diagnostic = true;
        result->diagnostic.line = 1;
        result->diagnostic.column = 1;
        result->diagnostic.end_column = 1;
        snprintf(result->diagnostic.message, sizeof(result->diagnostic.message),
                 "quantitative binding resolution failed%s%s",
                 bindings.duplicate_name ? " for '" : "",
                 bindings.duplicate_name ? bindings.duplicate_name : "");
        return false;
    }
    unit->infer_contexts = calloc(
        unit->ast.count + 1, sizeof(*unit->infer_contexts));
    unit->parameter_usages = calloc(
        unit->ast.count, sizeof(*unit->parameter_usages));
    unit->parameter_usage_counts = calloc(
        unit->ast.count, sizeof(*unit->parameter_usage_counts));
    unit->closure_usages = calloc(
        unit->ast.count, sizeof(*unit->closure_usages));
    unit->closure_usage_counts = calloc(
        unit->ast.count, sizeof(*unit->closure_usage_counts));
    unit->resource_summaries = calloc(
        unit->ast.count, sizeof(*unit->resource_summaries));
    if (!unit->infer_contexts || !unit->parameter_usages ||
        !unit->parameter_usage_counts || !unit->closure_usages ||
        !unit->closure_usage_counts || !unit->resource_summaries)
        return false;
    ReaderDiagnosticContext diagnostic;
    reader_diagnostic_context_push(&diagnostic, frontend_capture, result);
    if (setjmp(diagnostic.escape) != 0) return false;
    reader_diagnostic_context_arm(&diagnostic);
    InferCtx *bootstrap = infer_ctx_create(
        unit->infer_base_env, NULL, unit->filename);
    if (!bootstrap) {
        if (!reader_diagnostic_context_pop(&diagnostic)) abort();
        return false;
    }
    unit->infer_contexts[unit->infer_context_count++] = bootstrap;
    infer_register_builtins(bootstrap);
    for (size_t i = 0; i < unit->ast.count; i++) {
        InferCtx *context = infer_ctx_create(
            unit->infer_env, NULL, unit->filename);
        if (!context) {
            if (!reader_diagnostic_context_pop(&diagnostic)) abort();
            return false;
        }
        unit->infer_contexts[unit->infer_context_count++] = context;
        infer_toplevel(context, unit->ast.exprs[i]);
        if (context->had_error) {
            result->has_diagnostic = true;
            result->diagnostic.line = unit->ast.exprs[i]->line;
            result->diagnostic.column = unit->ast.exprs[i]->column;
            result->diagnostic.end_column = unit->ast.exprs[i]->column;
            snprintf(result->diagnostic.message,
                     sizeof(result->diagnostic.message), "%s",
                     context->error_msg[0]
                         ? context->error_msg : "type inference failed");
            if (!reader_diagnostic_context_pop(&diagnostic)) abort();
            return false;
        }
        const char *definition_name =
            frontend_definition_name(unit->ast.exprs[i]);
        const AST *lambda = frontend_definition_lambda(unit->ast.exprs[i]);
        TypeScheme *scheme = definition_name
            ? infer_env_lookup(context, definition_name) : NULL;
        if (scheme && lambda) {
            QttSourceGradeResult grades = qtt_source_grade_scheme(
                lambda, context, frontend_module_id(unit->filename));
            if (grades.status == QTT_SOURCE_GRADES_OK) {
                scheme_set_grade_scheme(scheme, grades.scheme);
                if (grades.count) {
                    unit->parameter_usages[i] = malloc(
                        grades.count * sizeof(*grades.solved));
                    if (unit->parameter_usages[i]) {
                        memcpy(unit->parameter_usages[i], grades.solved,
                               grades.count * sizeof(*grades.solved));
                        unit->parameter_usage_counts[i] = grades.count;
                    }
                }
                if (grades.closure_count) {
                    unit->closure_usages[i] = malloc(
                        grades.closure_count * sizeof(*grades.closure_solved));
                    if (unit->closure_usages[i]) {
                        memcpy(unit->closure_usages[i], grades.closure_solved,
                               grades.closure_count *
                                   sizeof(*grades.closure_solved));
                        unit->closure_usage_counts[i] = grades.closure_count;
                    }
                }
            }
            qtt_source_grade_result_free(&grades);
            bool complete = false;
            QttEffectScheme *effects = infer_effect_scheme_for_lambda(
                context, lambda, &complete);
            if (effects) {
                scheme_set_effect_scheme(scheme, effects, complete);
                qtt_effect_scheme_free(effects);
            }
            QttShadowEvidence resource = qtt_shadow_collect_lambda(
                lambda, frontend_module_id(unit->filename));
            if (resource.summary.status == QTT_SHADOW_VERIFIED) {
                unit->resource_summaries[i] = (MonadQttResourceSummary){
                    .verified = true,
                    .destructor_plan_available = resource.details_complete,
                    .result_representation = resource.result_representation,
                    .block_count = resource.summary.block_count,
                    .instruction_count = resource.summary.instruction_count,
                    .certificate_fingerprint =
                        frontend_resource_evidence_fingerprint(&resource),
                };
            }
            qtt_shadow_evidence_free(&resource);
        }
    }
    if (!reader_diagnostic_context_pop(&diagnostic)) abort();
    unit->inference_committed = true;
    return true;
}

bool monad_compilation_unit_inference_committed(
    const MonadCompilationUnit *unit) {
    return unit && unit->inference_committed;
}

bool monad_compilation_unit_has_committed_binding(
    const MonadCompilationUnit *unit, const char *name) {
    if (!unit || !unit->inference_committed || !name) return false;
    for (size_t i = 0; i < unit->infer_env->size; i++)
        for (InferEnvEntry *entry = unit->infer_env->buckets[i]; entry;
             entry = entry->next)
            if (strcmp(entry->name, name) == 0) return true;
    return false;
}

static const char *frontend_definition_name(const AST *ast) {
    if (!ast || ast->type != AST_LIST || ast->list.count < 3 ||
        !ast->list.items[0] || ast->list.items[0]->type != AST_SYMBOL ||
        strcmp(ast->list.items[0]->symbol, "define") != 0)
        return NULL;
    const AST *binding = ast->list.items[1];
    if (binding->type == AST_SYMBOL) return binding->symbol;
    if (binding->type == AST_LIST && binding->list.count &&
        binding->list.items[0]->type == AST_SYMBOL)
        return binding->list.items[0]->symbol;
    return NULL;
}

static const char *frontend_definition_docstring(const AST *ast) {
    if (!ast || ast->type != AST_LIST) return NULL;
    for (size_t i = 0; i < ast->list.count; i++) {
        const AST *item = ast->list.items[i];
        if (item && item->type == AST_LAMBDA && item->lambda.docstring)
            return item->lambda.docstring;
    }
    return NULL;
}

static const AST *frontend_definition_lambda(const AST *ast) {
    if (!ast || ast->type != AST_LIST) return NULL;
    for (size_t i = 0; i < ast->list.count; i++) {
        const AST *item = ast->list.items[i];
        if (item && item->type == AST_LAMBDA) return item;
    }
    return NULL;
}

size_t monad_compilation_unit_definition_count(
    const MonadCompilationUnit *unit) {
    if (!unit || !unit->inference_committed) return 0;
    size_t count = 0;
    for (size_t i = 0; i < unit->ast.count; i++)
        if (frontend_definition_name(unit->ast.exprs[i])) count++;
    return count;
}

bool monad_compilation_unit_definition_at(
    const MonadCompilationUnit *unit, size_t index, const char **name,
    const TypeScheme **scheme, const AST **source_ast,
    const char **docstring) {
    if (name) *name = NULL;
    if (scheme) *scheme = NULL;
    if (source_ast) *source_ast = NULL;
    if (docstring) *docstring = NULL;
    if (!unit || !unit->inference_committed) return false;
    for (size_t i = 0; i < unit->ast.count; i++) {
        const char *candidate = frontend_definition_name(unit->ast.exprs[i]);
        if (!candidate) continue;
        if (index) { index--; continue; }
        for (size_t bucket = 0; bucket < unit->infer_env->size; bucket++)
            for (InferEnvEntry *entry = unit->infer_env->buckets[bucket];
                 entry; entry = entry->next)
                if (strcmp(entry->name, candidate) == 0) {
                    if (name) *name = candidate;
                    if (scheme) *scheme = entry->scheme;
                    if (source_ast) *source_ast = unit->ast.exprs[i];
                    if (docstring)
                        *docstring = frontend_definition_docstring(
                            unit->ast.exprs[i]);
                    return true;
                }
        return false;
    }
    return false;
}

bool monad_compilation_unit_definition_quantities(
    const MonadCompilationUnit *unit, size_t index,
    const QttQuantity **parameters, size_t *parameter_count,
    const QttQuantity **closures, size_t *closure_count) {
    if (parameters) *parameters = NULL;
    if (parameter_count) *parameter_count = 0;
    if (closures) *closures = NULL;
    if (closure_count) *closure_count = 0;
    if (!unit || !unit->inference_committed) return false;
    for (size_t i = 0; i < unit->ast.count; i++) {
        if (!frontend_definition_name(unit->ast.exprs[i])) continue;
        if (index) { index--; continue; }
        if (parameters) *parameters = unit->parameter_usages[i];
        if (parameter_count)
            *parameter_count = unit->parameter_usage_counts[i];
        if (closures) *closures = unit->closure_usages[i];
        if (closure_count) *closure_count = unit->closure_usage_counts[i];
        return true;
    }
    return false;
}

bool monad_compilation_unit_definition_resource(
    const MonadCompilationUnit *unit, size_t index,
    MonadQttResourceSummary *summary) {
    if (summary) *summary = (MonadQttResourceSummary){0};
    if (!unit || !unit->inference_committed || !summary) return false;
    for (size_t i = 0; i < unit->ast.count; i++) {
        if (!frontend_definition_name(unit->ast.exprs[i])) continue;
        if (index) { index--; continue; }
        *summary = unit->resource_summaries[i];
        return true;
    }
    return false;
}

void monad_compilation_unit_destroy(MonadCompilationUnit *unit) {
    if (!unit) return;
    for (size_t i = 0; i < unit->ast.count; i++) {
        free(unit->parameter_usages ? unit->parameter_usages[i] : NULL);
        free(unit->closure_usages ? unit->closure_usages[i] : NULL);
    }
    free(unit->parameter_usages);
    free(unit->parameter_usage_counts);
    free(unit->closure_usages);
    free(unit->closure_usage_counts);
    free(unit->resource_summaries);
    for (size_t i = 0; i < unit->ast.count; i++) ast_free(unit->ast.exprs[i]);
    free(unit->ast.exprs);
    for (size_t i = 0; i < unit->infer_context_count; i++)
        infer_ctx_free(unit->infer_contexts[i]);
    free(unit->infer_contexts);
    infer_env_free_owned_schemes(unit->infer_env);
    infer_env_free_owned_schemes(unit->infer_base_env);
    free(unit->filename);
    frontend_state_leave(unit->state);
    free(unit);
}

bool monad_frontend_parse_in_state(
    MonadFrontendState *state, const char *source, const char *filename,
    MonadFrontendResult *result) {
    if (!state) return frontend_parse_bound(source, filename, result);
    MonadCompilationUnit *unit = monad_compilation_unit_begin(state);
    if (!unit) {
        if (result) memset(result, 0, sizeof(*result));
        return false;
    }
    bool accepted = monad_compilation_unit_parse(
        unit, source, filename, result);
    if (accepted) {
        result->ast = unit->ast;
        memset(&unit->ast, 0, sizeof(unit->ast));
    }
    monad_compilation_unit_destroy(unit);
    return accepted;
}

bool monad_frontend_parse(
    const char *source, const char *filename, MonadFrontendResult *result) {
    return monad_frontend_parse_in_state(NULL, source, filename, result);
}

void monad_frontend_result_destroy(MonadFrontendResult *result) {
    if (!result) return;
    for (size_t i = 0; i < result->ast.count; i++) ast_free(result->ast.exprs[i]);
    free(result->ast.exprs);
    memset(result, 0, sizeof(*result));
}
