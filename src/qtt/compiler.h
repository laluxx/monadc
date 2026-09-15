#ifndef MONAD_QTT_COMPILER_H
#define MONAD_QTT_COMPILER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct AST AST;
typedef struct QttSignatureEnv QttSignatureEnv;
typedef struct QttFunctionSignature QttFunctionSignature;
typedef struct QttEffectDeclaration QttEffectDeclaration;
typedef struct QttEffectHandlerProfile QttEffectHandlerProfile;

typedef enum {
    QTT_COMPILER_PIPELINE_DISABLED,
    QTT_COMPILER_PIPELINE_VERIFIED,
    QTT_COMPILER_PIPELINE_PARTIAL,
    QTT_COMPILER_PIPELINE_INTERNAL_ERROR,
} QttCompilerPipelineStatus;

typedef struct {
    QttCompilerPipelineStatus status;
    size_t verified_count;
    size_t unsupported_count;
    size_t internal_error_count;
} QttCompilerPipelineResult;

/*
 * One compiler-wide policy boundary for the QTT rollout.  Analysis and
 * evidence-backed memory lowering are normal pipeline stages; diagnostics are
 * independently opt-in.  Explicit zero values remain emergency rollout
 * switches while unsupported programs continue through conservative fallback.
 */
bool qtt_compiler_analysis_enabled(void);
bool qtt_compiler_memory_enabled(void);
bool qtt_compiler_diagnostics_enabled(void);
typedef enum {
    QTT_TRACE_NONE,
    QTT_TRACE_SUMMARY,
    QTT_TRACE_PROOF,
    QTT_TRACE_ALL,
} QttTraceLevel;

void qtt_compiler_set_trace(int verbosity, int explicit_level);
void qtt_compiler_set_user_module(bool is_user_module);
/* Release compiler-local module metadata after an independent compilation.
 * REPL sessions intentionally keep this state until they are torn down. */
void qtt_compiler_reset(void);
bool qtt_compiler_trace_enabled(void);
bool qtt_compiler_trace_detailed(void);
bool qtt_compiler_trace_all(void);
uint64_t qtt_compiler_module_id(const char *identity);
bool qtt_compiler_register_definition(
    const char *identity, const char *name, const AST *lambda);
bool qtt_compiler_register_callable_contract(
    const char *identity, const char *name, const char *portable_contract,
    uint64_t semantic_fingerprint);
bool qtt_compiler_register_effect_judgment(
    const char *identity, const char *name,
    uint64_t row_fingerprint, uint64_t constraint_fingerprint,
    int constraint_result, const size_t *predicate_stages,
    const char *const *predicate_names, size_t predicate_count);
bool qtt_compiler_lookup_effect_judgment(
    uint64_t module_id, const char *visible_name,
    uint64_t *row_fingerprint, uint64_t *constraint_fingerprint,
    int *constraint_result, const size_t **predicate_stages,
    const char *const **predicate_names, size_t *predicate_count);
bool qtt_compiler_lookup_callable_authority(
    uint64_t module_id, const char *visible_name,
    const char **portable_contract, uint64_t *contract_fingerprint,
    const char **portable_hm_scheme);
bool qtt_compiler_register_hm_scheme(
    const char *identity, const char *name, const char *portable_scheme);
bool qtt_compiler_register_effect_declaration(
    const char *identity, const QttEffectDeclaration *declaration);
bool qtt_compiler_register_handler_profile(
    const char *identity, const QttEffectHandlerProfile *profile);
bool qtt_compiler_register_trait_implication(
    const char *identity, const char *premise, const char *consequence);
bool qtt_compiler_register_import(
    const char *identity, const char *visible_name,
    const char *provider_module, const char *provider_name,
    const AST *lambda);
bool qtt_compiler_register_contract(
    const char *identity, const char *visible_name,
    const char *provider_module, const char *provider_name,
    const QttFunctionSignature *contract);
bool qtt_compiler_write_interface(
    const char *identity, const char *module_name,
    const char *path, uint64_t artifact_fingerprint);
const QttSignatureEnv *qtt_compiler_signatures(
    const char *identity);
QttCompilerPipelineResult qtt_compiler_run_pipeline(
    const char *identity);
QttCompilerPipelineResult qtt_compiler_run_source_pipeline(
    const char *identity, AST *const *forms, size_t form_count);
void qtt_compiler_trace_module(const char *identity);

#endif
