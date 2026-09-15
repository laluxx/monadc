#include "compiler.h"

#include "core.h"
#include "interface.h"
#include "module.h"
#include "pipeline.h"
#include "../effects/effect.h"
#include "../infer.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct QttCompilerDefinition {
    const AST *source;
    char *name;
    char *provider_module;
    char *provider_name;
    QttCoreNode *core;
    const QttFunctionSignature *external_contract;
    char *callable_contract;
    uint64_t callable_contract_fingerprint;
    char *hm_scheme;
    uint64_t effect_row_fingerprint;
    uint64_t effect_constraint_fingerprint;
    int effect_constraint_result;
    size_t *effect_predicate_stages;
    char **effect_predicate_names;
    size_t effect_predicate_count;
    struct QttCompilerDefinition *next;
} QttCompilerDefinition;

typedef struct QttCompilerModule {
    uint64_t module_id;
    bool valid;
    QttModule *module;
    QttCompilerDefinition *definitions;
    QttEffectDeclaration *effect_declarations;
    size_t effect_declaration_count;
    size_t effect_declaration_capacity;
    QttEffectHandlerProfile *handler_profiles;
    size_t handler_profile_count;
    size_t handler_profile_capacity;
    QttEffectTraitImplication *trait_implications;
    size_t trait_implication_count;
    size_t trait_implication_capacity;
    struct QttCompilerModule *next;
} QttCompilerModule;

static QttCompilerModule *compiler_modules;

static void clear_effect_judgment(QttCompilerDefinition *definition);

static void compiler_definition_free(QttCompilerDefinition *definition) {
    if (!definition) return;
    free(definition->name);
    free(definition->provider_module);
    free(definition->provider_name);
    free(definition->callable_contract);
    free(definition->hm_scheme);
    clear_effect_judgment(definition);
    qtt_core_free(definition->core);
    free(definition);
}

static void compiler_module_free(QttCompilerModule *entry) {
    if (!entry) return;
    QttCompilerDefinition *definition = entry->definitions;
    while (definition) {
        QttCompilerDefinition *next = definition->next;
        compiler_definition_free(definition);
        definition = next;
    }
    for (size_t i = 0; i < entry->effect_declaration_count; i++) {
        QttEffectDeclaration *declaration = &entry->effect_declarations[i];
        free((char *)declaration->name);
        free((char *)declaration->traits);
        free((char *)declaration->operation);
        free((char *)declaration->payload_type);
        free((char *)declaration->result_type);
        free((char *)declaration->operation_scheme);
    }
    free(entry->effect_declarations);
    for (size_t i = 0; i < entry->handler_profile_count; i++) {
        free((char *)entry->handler_profiles[i].name);
        free((char *)entry->handler_profiles[i].effect_name);
    }
    free(entry->handler_profiles);
    for (size_t i = 0; i < entry->trait_implication_count; i++) {
        free(entry->trait_implications[i].premise);
        free(entry->trait_implications[i].consequence);
    }
    free(entry->trait_implications);
    qtt_module_free(entry->module);
    free(entry);
}

void qtt_compiler_reset(void) {
    QttCompilerModule *entry = compiler_modules;
    compiler_modules = NULL;
    while (entry) {
        QttCompilerModule *next = entry->next;
        compiler_module_free(entry);
        entry = next;
    }
}

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static void clear_effect_judgment(QttCompilerDefinition *definition) {
    for (size_t i = 0; i < definition->effect_predicate_count; i++)
        free(definition->effect_predicate_names[i]);
    free(definition->effect_predicate_names);
    free(definition->effect_predicate_stages);
    definition->effect_predicate_names = NULL;
    definition->effect_predicate_stages = NULL;
    definition->effect_predicate_count = 0;
    definition->effect_row_fingerprint = 0;
    definition->effect_constraint_fingerprint = 0;
    definition->effect_constraint_result = 0;
}

static uint64_t effect_authority_mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static uint64_t effect_authority_text(uint64_t hash, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; p && *p; p++)
        hash = effect_authority_mix(hash, *p);
    return hash;
}

static uint64_t callable_effect_commitment(
    uint64_t row_fingerprint, const InferCallableContract *contract) {
    uint64_t commitment = UINT64_C(1469598103934665603);
    size_t count = contract ? contract->effect_trait_predicate_count : 0;
    commitment = effect_authority_mix(commitment, row_fingerprint);
    commitment = effect_authority_mix(commitment, count);
    for (size_t i = 0; i < count; i++) {
        commitment = effect_authority_mix(
            commitment, contract->effect_trait_predicate_stages[i]);
        commitment = effect_authority_text(
            commitment, contract->effect_trait_predicate_names[i]);
    }
    return commitment ? commitment : UINT64_C(1);
}

static bool export_effect_judgment(
    QttInterface *interface, const QttCompilerDefinition *definition) {
    if (!definition->callable_contract) return true;
    if (!definition->effect_row_fingerprint ||
        !definition->effect_constraint_fingerprint)
        return false;
    return qtt_interface_set_effect_judgment(
        interface, definition->name,
        definition->effect_row_fingerprint,
        definition->effect_constraint_fingerprint,
        (QttEffectConstraintResult)definition->effect_constraint_result,
        definition->effect_predicate_stages,
        (const char *const *)definition->effect_predicate_names,
        definition->effect_predicate_count);
}

static QttCompilerModule *compiler_module(
    const char *identity, bool create) {
    uint64_t module_id = qtt_compiler_module_id(identity);
    for (QttCompilerModule *entry = compiler_modules;
         entry; entry = entry->next)
        if (entry->module_id == module_id) return entry;
    if (!create) return NULL;
    QttCompilerModule *entry = calloc(1, sizeof(*entry));
    if (!entry) return NULL;
    entry->module_id = module_id;
    entry->valid = true;
    entry->module = qtt_module_new(module_id);
    if (!entry->module) {
        free(entry);
        return NULL;
    }
    entry->next = compiler_modules;
    compiler_modules = entry;
    return entry;
}

/*
 * Source definitions are authoritative over imported aliases.  Rebuild the
 * small contract environment transactionally so a local definition can
 * replace an earlier import without leaving stale interprocedural evidence.
 */
static bool replace_import_with_local(
    QttCompilerModule *entry, QttCompilerDefinition *definition,
    const AST *lambda) {
    QttCoreError core_error = QTT_CORE_OK;
    QttCoreNode *replacement =
        qtt_core_lower(lambda, entry->module_id, &core_error);
    if (!replacement) return false;
    QttModule *rebuilt = qtt_module_new(entry->module_id);
    if (!rebuilt) {
        qtt_core_free(replacement);
        return false;
    }
    for (QttCompilerDefinition *item = entry->definitions;
         item; item = item->next) {
        const QttCoreNode *core =
            item == definition ? replacement : item->core;
        QttModuleError declared = item == definition || core
            ? qtt_module_declare(rebuilt, item->name, core)
            : qtt_module_declare_external(
                  rebuilt, item->name, item->external_contract);
        if (declared != QTT_MODULE_OK) {
            qtt_module_free(rebuilt);
            qtt_core_free(replacement);
            return false;
        }
    }
    QttModule *old_module = entry->module;
    QttCoreNode *old_core = definition->core;
    entry->module = rebuilt;
    definition->source = lambda;
    definition->core = replacement;
    definition->external_contract = NULL;
    free(definition->provider_module);
    free(definition->provider_name);
    free(definition->callable_contract);
    free(definition->hm_scheme);
    clear_effect_judgment(definition);
    definition->provider_module = NULL;
    definition->provider_name = NULL;
    definition->callable_contract = NULL;
    definition->callable_contract_fingerprint = 0;
    definition->hm_scheme = NULL;
    qtt_module_free(old_module);
    qtt_core_free(old_core);
    return true;
}

bool qtt_compiler_register_definition(
    const char *identity, const char *name, const AST *lambda) {
    if (!qtt_compiler_analysis_enabled() ||
        !name || !lambda || lambda->type != AST_LAMBDA)
        return false;
    QttCompilerModule *entry = compiler_module(identity, true);
    if (!entry || !entry->valid) return false;
    for (QttCompilerDefinition *definition = entry->definitions;
         definition; definition = definition->next) {
        if (strcmp(definition->name, name) != 0) continue;
        if (definition->source == lambda ||
            (definition->source &&
             definition->source->line == lambda->line &&
             definition->source->column == lambda->column))
            return true;
        if (!definition->source && !definition->provider_module) {
            QttCoreError core_error = QTT_CORE_OK;
            QttCoreNode *core =
                qtt_core_lower(lambda, entry->module_id, &core_error);
            if (!core) return false;
            QttModuleError declared =
                qtt_module_declare(entry->module, name, core);
            if (declared != QTT_MODULE_OK) {
                qtt_core_free(core);
                return false;
            }
            definition->source = lambda;
            definition->core = core;
            return true;
        }
        if (definition->provider_module)
            return replace_import_with_local(entry, definition, lambda);
        if (qtt_compiler_trace_detailed())
            fprintf(stderr,
                "[qtt] duplicate local definition authority for '%s' in '%s' "
                "at %d:%d versus %d:%d\n",
                name, identity,
                definition->source ? definition->source->line : -1,
                definition->source ? definition->source->column : -1,
                lambda->line, lambda->column);
        entry->valid = false;
        return false;
    }

    QttCompilerDefinition *definition = calloc(1, sizeof(*definition));
    if (!definition) return false;
    definition->name = copy_text(name);
    if (!definition->name) {
        free(definition);
        return false;
    }
    QttCoreError core_error = QTT_CORE_OK;
    definition->core =
        qtt_core_lower(lambda, entry->module_id, &core_error);
    if (!definition->core) {
        free(definition->name);
        free(definition);
        return false;
    }
    QttModuleError declared =
        qtt_module_declare(entry->module, name, definition->core);
    if (declared != QTT_MODULE_OK) {
        qtt_core_free(definition->core);
        free(definition->name);
        free(definition);
        return false;
    }
    definition->source = lambda;
    definition->next = entry->definitions;
    entry->definitions = definition;
    if (qtt_compiler_trace_detailed())
        fprintf(stderr, "[qtt] registered metadata-only HM scheme %s in %s\n",
                name, identity);
    return true;
}

bool qtt_compiler_register_callable_contract(
    const char *identity, const char *name, const char *portable_contract,
    uint64_t semantic_fingerprint) {
    if (!identity || !name || !portable_contract || !semantic_fingerprint)
        return false;
    QttCompilerModule *entry = compiler_module(identity, false);
    if (!entry || !entry->valid) return false;
    for (QttCompilerDefinition *definition = entry->definitions;
         definition; definition = definition->next) {
        if (strcmp(definition->name, name)) continue;
        char *copy = copy_text(portable_contract);
        if (!copy) return false;
        free(definition->callable_contract);
        definition->callable_contract = copy;
        definition->callable_contract_fingerprint = semantic_fingerprint;
        InferCallableContract decoded = {0};
        bool coherent = infer_callable_contract_deserialize(
            &decoded, portable_contract);
        uint64_t commitment = coherent
            ? callable_effect_commitment(semantic_fingerprint, &decoded) : 0;
        if (coherent)
            coherent = qtt_compiler_register_effect_judgment(
                identity, name, semantic_fingerprint, commitment,
                decoded.effect_trait_predicate_count
                    ? QTT_EFFECT_CONSTRAINT_RESIDUAL
                    : QTT_EFFECT_CONSTRAINT_SOLVED,
                decoded.effect_trait_predicate_stages,
                (const char *const *)decoded.effect_trait_predicate_names,
                decoded.effect_trait_predicate_count);
        infer_callable_contract_free(&decoded);
        if (!coherent) {
            free(definition->callable_contract);
            definition->callable_contract = NULL;
            definition->callable_contract_fingerprint = 0;
            clear_effect_judgment(definition);
            return false;
        }
        return true;
    }
    return false;
}

bool qtt_compiler_register_effect_judgment(
    const char *identity, const char *name,
    uint64_t row_fingerprint, uint64_t constraint_fingerprint,
    int constraint_result, const size_t *predicate_stages,
    const char *const *predicate_names, size_t predicate_count) {
    if (!identity || !name || !row_fingerprint ||
        !constraint_fingerprint ||
        (constraint_result != QTT_EFFECT_CONSTRAINT_SOLVED &&
         constraint_result != QTT_EFFECT_CONSTRAINT_RESIDUAL) ||
        (predicate_count && (!predicate_stages || !predicate_names)))
        return false;
    for (size_t i = 0; i < predicate_count; i++)
        if (!predicate_names[i] || !*predicate_names[i]) return false;
    QttCompilerModule *entry = compiler_module(identity, false);
    if (!entry || !entry->valid) return false;
    for (QttCompilerDefinition *definition = entry->definitions;
         definition; definition = definition->next) {
        if (strcmp(definition->name, name)) continue;
        if (definition->callable_contract) {
            InferCallableContract decoded = {0};
            bool coherent = row_fingerprint ==
                    definition->callable_contract_fingerprint &&
                infer_callable_contract_deserialize(
                    &decoded, definition->callable_contract) &&
                decoded.effect_trait_predicate_count == predicate_count;
            uint64_t expected_commitment = coherent
                ? callable_effect_commitment(row_fingerprint, &decoded) : 0;
            coherent = coherent &&
                expected_commitment == constraint_fingerprint;
            size_t decoded_count = decoded.effect_trait_predicate_count;
            for (size_t i = 0; coherent && i < predicate_count; i++)
                coherent =
                    decoded.effect_trait_predicate_stages[i] ==
                        predicate_stages[i] &&
                    strcmp(decoded.effect_trait_predicate_names[i],
                           predicate_names[i]) == 0;
            infer_callable_contract_free(&decoded);
            if (!coherent) {
                if (qtt_compiler_trace_detailed())
                    fprintf(stderr,
                        "[effects] authority mismatch %s: row=%016llx/%016llx "
                        "constraints=%016llx/%016llx predicates=%zu/%zu\n",
                        name,
                        (unsigned long long)row_fingerprint,
                        (unsigned long long)
                            definition->callable_contract_fingerprint,
                        (unsigned long long)constraint_fingerprint,
                        (unsigned long long)expected_commitment,
                        predicate_count,
                        decoded_count);
                return false;
            }
        }
        size_t *stages = predicate_count
            ? malloc(predicate_count * sizeof(*stages)) : NULL;
        char **names = predicate_count
            ? calloc(predicate_count, sizeof(*names)) : NULL;
        if (predicate_count && (!stages || !names)) {
            free(stages); free(names); return false;
        }
        bool copied = true;
        for (size_t i = 0; i < predicate_count; i++) {
            if (!predicate_names[i] || !*predicate_names[i]) {
                copied = false;
                break;
            }
            stages[i] = predicate_stages[i];
            names[i] = copy_text(predicate_names[i]);
            if (!names[i]) { copied = false; break; }
        }
        if (!copied) {
            for (size_t i = 0; i < predicate_count; i++) free(names[i]);
            free(names); free(stages); return false;
        }
        clear_effect_judgment(definition);
        definition->effect_row_fingerprint = row_fingerprint;
        definition->effect_constraint_fingerprint = constraint_fingerprint;
        definition->effect_constraint_result = constraint_result;
        definition->effect_predicate_stages = stages;
        definition->effect_predicate_names = names;
        definition->effect_predicate_count = predicate_count;
        return true;
    }
    return false;
}

bool qtt_compiler_lookup_effect_judgment(
    uint64_t module_id, const char *visible_name,
    uint64_t *row_fingerprint, uint64_t *constraint_fingerprint,
    int *constraint_result, const size_t **predicate_stages,
    const char *const **predicate_names, size_t *predicate_count) {
    QttCompilerModule *entry = compiler_modules;
    while (entry && entry->module_id != module_id) entry = entry->next;
    if (!entry || !entry->valid || !visible_name) return false;
    for (QttCompilerDefinition *definition = entry->definitions;
         definition; definition = definition->next) {
        if (strcmp(definition->name, visible_name)) continue;
        if (!definition->effect_row_fingerprint ||
            !definition->effect_constraint_fingerprint) return false;
        if (row_fingerprint) *row_fingerprint = definition->effect_row_fingerprint;
        if (constraint_fingerprint)
            *constraint_fingerprint = definition->effect_constraint_fingerprint;
        if (constraint_result)
            *constraint_result = definition->effect_constraint_result;
        if (predicate_stages)
            *predicate_stages = definition->effect_predicate_stages;
        if (predicate_names)
            *predicate_names = (const char *const *)definition->effect_predicate_names;
        if (predicate_count)
            *predicate_count = definition->effect_predicate_count;
        return true;
    }
    return false;
}

bool qtt_compiler_lookup_callable_authority(
    uint64_t module_id, const char *visible_name,
    const char **portable_contract, uint64_t *contract_fingerprint,
    const char **portable_hm_scheme) {
    if (portable_contract) *portable_contract = NULL;
    if (contract_fingerprint) *contract_fingerprint = 0;
    if (portable_hm_scheme) *portable_hm_scheme = NULL;
    if (!module_id || !visible_name || !*visible_name) return false;
    QttCompilerModule *entry = compiler_modules;
    while (entry && entry->module_id != module_id) entry = entry->next;
    if (!entry || !entry->valid) return false;
    for (QttCompilerDefinition *definition = entry->definitions;
         definition; definition = definition->next) {
        if (strcmp(definition->name, visible_name)) continue;
        if (portable_contract)
            *portable_contract = definition->callable_contract;
        if (contract_fingerprint)
            *contract_fingerprint =
                definition->callable_contract_fingerprint;
        if (portable_hm_scheme)
            *portable_hm_scheme = definition->hm_scheme;
        return definition->callable_contract || definition->hm_scheme;
    }
    return false;
}

bool qtt_compiler_register_hm_scheme(
    const char *identity, const char *name, const char *portable_scheme) {
    if (!identity || !name || !portable_scheme) return false;
    QttCompilerModule *entry = compiler_module(identity, true);
    if (!entry || !entry->valid) return false;
    for (QttCompilerDefinition *definition = entry->definitions;
         definition; definition = definition->next) {
        if (strcmp(definition->name, name)) continue;
        char *copy = copy_text(portable_scheme);
        if (!copy) return false;
        free(definition->hm_scheme);
        definition->hm_scheme = copy;
        return true;
    }
    QttCompilerDefinition *definition = calloc(1, sizeof(*definition));
    if (!definition) return false;
    definition->name = copy_text(name);
    definition->hm_scheme = copy_text(portable_scheme);
    if (!definition->name || !definition->hm_scheme) {
        free(definition->name);
        free(definition->hm_scheme);
        free(definition);
        return false;
    }
    definition->next = entry->definitions;
    entry->definitions = definition;
    return true;
}

bool qtt_compiler_register_effect_declaration(
    const char *identity, const QttEffectDeclaration *declaration) {
    if (!identity || !declaration || !declaration->name ||
        !declaration->operation)
        return false;
    QttCompilerModule *entry = compiler_module(identity, true);
    if (!entry || !entry->valid) return false;
    for (size_t i = 0; i < entry->effect_declaration_count; i++) {
        QttEffectDeclaration *existing = &entry->effect_declarations[i];
        if (strcmp(existing->name, declaration->name)) continue;
        bool canonical_scheme = existing->operation_scheme &&
            declaration->operation_scheme &&
            !strcmp(existing->operation_scheme,
                    declaration->operation_scheme);
        return existing->kind == declaration->kind &&
            ((!existing->traits && !declaration->traits) ||
             (existing->traits && declaration->traits &&
              strcmp(existing->traits, declaration->traits) == 0)) &&
            existing->constructor_id == declaration->constructor_id &&
            strcmp(existing->operation, declaration->operation) == 0 &&
            (canonical_scheme ||
             ((!existing->payload_type && !declaration->payload_type) ||
             (existing->payload_type && declaration->payload_type &&
              !strcmp(existing->payload_type, declaration->payload_type)))) &&
            (canonical_scheme ||
             ((!existing->result_type && !declaration->result_type) ||
             (existing->result_type && declaration->result_type &&
              !strcmp(existing->result_type, declaration->result_type)))) &&
            ((!existing->operation_scheme &&
              !declaration->operation_scheme) ||
             (existing->operation_scheme && declaration->operation_scheme &&
              !strcmp(existing->operation_scheme,
                      declaration->operation_scheme))) &&
            qtt_quantity_equal(existing->resumption, declaration->resumption) &&
            existing->scoped == declaration->scoped;
    }
    if (entry->effect_declaration_count ==
            entry->effect_declaration_capacity) {
        size_t next = entry->effect_declaration_capacity
            ? entry->effect_declaration_capacity * 2 : 8;
        QttEffectDeclaration *grown = realloc(
            entry->effect_declarations, next * sizeof(*grown));
        if (!grown) return false;
        entry->effect_declarations = grown;
        entry->effect_declaration_capacity = next;
    }
    QttEffectDeclaration copy = *declaration;
    copy.name = copy_text(declaration->name);
    copy.traits = declaration->traits ? copy_text(declaration->traits) : NULL;
    copy.operation = copy_text(declaration->operation);
    copy.payload_type = declaration->payload_type
        ? copy_text(declaration->payload_type) : NULL;
    copy.result_type = declaration->result_type
        ? copy_text(declaration->result_type) : NULL;
    copy.operation_scheme = declaration->operation_scheme
        ? copy_text(declaration->operation_scheme) : NULL;
    if (!copy.name || (declaration->traits && !copy.traits) ||
        !copy.operation ||
        (declaration->payload_type && !copy.payload_type) ||
        (declaration->result_type && !copy.result_type) ||
        (declaration->operation_scheme && !copy.operation_scheme)) {
        free((char *)copy.name);
        free((char *)copy.traits);
        free((char *)copy.operation);
        free((char *)copy.payload_type);
        free((char *)copy.result_type);
        free((char *)copy.operation_scheme);
        return false;
    }
    entry->effect_declarations[entry->effect_declaration_count++] = copy;
    return true;
}

bool qtt_compiler_register_trait_implication(
    const char *identity, const char *premise, const char *consequence) {
    if (!identity || !premise || !consequence) return false;
    QttCompilerModule *entry = compiler_module(identity, true);
    if (!entry || !entry->valid) return false;
    for (size_t i = 0; i < entry->trait_implication_count; i++)
        if (!strcmp(entry->trait_implications[i].premise, premise) &&
            !strcmp(entry->trait_implications[i].consequence, consequence))
            return true;
    if (entry->trait_implication_count == entry->trait_implication_capacity) {
        size_t next = entry->trait_implication_capacity
            ? entry->trait_implication_capacity * 2 : 8;
        QttEffectTraitImplication *grown = realloc(
            entry->trait_implications, next * sizeof(*grown));
        if (!grown) return false;
        entry->trait_implications = grown;
        entry->trait_implication_capacity = next;
    }
    QttEffectTraitImplication edge = {
        .premise = copy_text(premise),
        .consequence = copy_text(consequence)};
    if (!edge.premise || !edge.consequence) {
        free(edge.premise); free(edge.consequence); return false;
    }
    entry->trait_implications[entry->trait_implication_count++] = edge;
    return true;
}

bool qtt_compiler_register_handler_profile(
    const char *identity, const QttEffectHandlerProfile *profile) {
    if (!identity || !profile || !profile->name || !profile->effect_name)
        return false;
    QttCompilerModule *entry = compiler_module(identity, true);
    if (!entry || !entry->valid) return false;
    for (size_t i = 0; i < entry->handler_profile_count; i++) {
        QttEffectHandlerProfile *existing = &entry->handler_profiles[i];
        if (strcmp(existing->name, profile->name)) continue;
        return !strcmp(existing->effect_name, profile->effect_name) &&
            qtt_quantity_equal(existing->continuation_usage,
                               profile->continuation_usage) &&
            existing->deep == profile->deep;
    }
    if (entry->handler_profile_count == entry->handler_profile_capacity) {
        size_t next = entry->handler_profile_capacity
            ? entry->handler_profile_capacity * 2 : 8;
        QttEffectHandlerProfile *grown = realloc(
            entry->handler_profiles, next * sizeof(*grown));
        if (!grown) return false;
        entry->handler_profiles = grown;
        entry->handler_profile_capacity = next;
    }
    QttEffectHandlerProfile copy = *profile;
    copy.name = copy_text(profile->name);
    copy.effect_name = copy_text(profile->effect_name);
    if (!copy.name || !copy.effect_name) {
        free((char *)copy.name); free((char *)copy.effect_name); return false;
    }
    entry->handler_profiles[entry->handler_profile_count++] = copy;
    return true;
}

const QttSignatureEnv *qtt_compiler_signatures(
    const char *identity) {
    QttCompilerModule *entry = compiler_module(identity, false);
    return entry && entry->valid
        ? qtt_module_signatures(entry->module) : NULL;
}

QttCompilerPipelineResult qtt_compiler_run_pipeline(
    const char *identity) {
    QttCompilerPipelineResult result = {
        .status = QTT_COMPILER_PIPELINE_DISABLED};
    if (!qtt_compiler_analysis_enabled()) return result;
    QttCompilerModule *entry = compiler_module(identity, false);
    if (!entry || !entry->valid) {
        result.status = QTT_COMPILER_PIPELINE_INTERNAL_ERROR;
        result.internal_error_count = 1;
        return result;
    }
    for (QttCompilerDefinition *definition = entry->definitions;
         definition; definition = definition->next) {
        if (definition->provider_module) continue;
        QttShadowResult verification =
            qtt_shadow_verify_lambda(
                definition->source, entry->module_id);
        if (verification.status == QTT_SHADOW_VERIFIED)
            result.verified_count++;
        else if (verification.status == QTT_SHADOW_UNSUPPORTED)
            result.unsupported_count++;
        else
            result.internal_error_count++;
    }
    if (result.internal_error_count) {
        result.status = QTT_COMPILER_PIPELINE_INTERNAL_ERROR;
    } else if (result.unsupported_count) {
        result.status = QTT_COMPILER_PIPELINE_PARTIAL;
    } else {
        result.status = QTT_COMPILER_PIPELINE_VERIFIED;
    }
    if (qtt_compiler_trace_enabled() ||
        qtt_compiler_diagnostics_enabled())
        printf("[qtt] pipeline: verified=%zu unsupported=%zu internal=%zu\n",
               result.verified_count, result.unsupported_count,
               result.internal_error_count);
    return result;
}

static const char *core_error_name(QttCoreError error) {
    switch (error) {
    case QTT_CORE_OK: return "ok";
    case QTT_CORE_OUT_OF_MEMORY: return "out-of-memory";
    case QTT_CORE_UNSUPPORTED_AST: return "unsupported-source-form";
    case QTT_CORE_MALFORMED_AST: return "malformed-source";
    }
    return "unknown-core-error";
}

static const char *core_validation_name(QttCoreValidation validation) {
    switch (validation) {
    case QTT_CORE_VALID: return "valid";
    case QTT_CORE_UNBOUND_VAR: return "unbound-variable";
    case QTT_CORE_WRONG_MODULE: return "wrong-module";
    case QTT_CORE_DUPLICATE_BINDER: return "duplicate-binder";
    case QTT_CORE_VALIDATION_OUT_OF_MEMORY: return "out-of-memory";
    }
    return "unknown-core-validation";
}

static const char *anf_lower_error_name(QttAnfLowerError error) {
    switch (error) {
    case QTT_ANF_LOWER_OK: return "valid";
    case QTT_ANF_LOWER_OUT_OF_MEMORY: return "out-of-memory";
    case QTT_ANF_LOWER_UNSUPPORTED_CORE: return "unsupported-core-form";
    case QTT_ANF_LOWER_MALFORMED_LITERAL: return "malformed-literal";
    case QTT_ANF_LOWER_INVALID_DEMAND: return "invalid-demand";
    }
    return "unknown-lowering-error";
}

static const char *anf_verification_error_name(QttAnfError error) {
    switch (error) {
    case QTT_ANF_VALID: return "valid";
    case QTT_ANF_OUT_OF_MEMORY: return "out-of-memory";
    case QTT_ANF_INVALID_BLOCK: return "invalid-block";
    case QTT_ANF_UNDEFINED_VALUE: return "undefined-value";
    case QTT_ANF_DUPLICATE_VALUE: return "duplicate-value";
    case QTT_ANF_INVALID_RESOURCE: return "invalid-resource";
    case QTT_ANF_INVALID_LOAN: return "invalid-loan";
    case QTT_ANF_BLOCK_ARGUMENT_MISMATCH: return "block-argument-mismatch";
    case QTT_ANF_OWNERSHIP_JOIN_MISMATCH: return "ownership-join-mismatch";
    case QTT_ANF_RESOURCE_LEAK: return "resource-leak";
    case QTT_ANF_TYPE_MISMATCH: return "type-mismatch";
    case QTT_ANF_NON_BOOLEAN_BRANCH: return "non-boolean-branch";
    case QTT_ANF_REPRESENTATION_MISMATCH:
        return "representation-mismatch";
    case QTT_ANF_INVALID_PLACE: return "invalid-place";
    case QTT_ANF_UNKNOWN_CALLABLE: return "unknown-callable";
    case QTT_ANF_CALL_ARITY_MISMATCH: return "call-arity-mismatch";
    case QTT_ANF_CALL_TYPE_MISMATCH: return "call-type-mismatch";
    case QTT_ANF_CALL_TRANSFER_MISMATCH: return "call-transfer-mismatch";
    case QTT_ANF_CALL_PROVENANCE_MISMATCH:
        return "call-provenance-mismatch";
    case QTT_ANF_CALL_RESULT_MISMATCH: return "call-result-mismatch";
    case QTT_ANF_CALL_CONTRACT_MISMATCH: return "call-contract-mismatch";
    }
    return "unknown-verification-error";
}

static const char *pipeline_status_name(QttCompilerPipelineStatus status) {
    switch (status) {
    case QTT_COMPILER_PIPELINE_DISABLED: return "DISABLED";
    case QTT_COMPILER_PIPELINE_VERIFIED: return "VERIFIED";
    case QTT_COMPILER_PIPELINE_PARTIAL: return "PARTIAL";
    case QTT_COMPILER_PIPELINE_INTERNAL_ERROR: return "INTERNAL ERROR";
    }
    return "UNKNOWN";
}

typedef struct {
    const char *reset;
    const char *bold;
    const char *dim;
    const char *cyan;
    const char *magenta;
    const char *green;
    const char *yellow;
    const char *red;
    const char *bold_green;
    const char *bold_yellow;
    const char *bold_red;
} QttTraceTheme;

static bool qtt_trace_color_enabled(void) {
    const char *setting = getenv("MONAD_QTT_COLOR");
    if (setting && setting[0]) {
        if (strcmp(setting, "always") == 0 ||
            strcmp(setting, "1") == 0 ||
            strcmp(setting, "yes") == 0)
            return true;
        if (strcmp(setting, "never") == 0 ||
            strcmp(setting, "0") == 0 ||
            strcmp(setting, "no") == 0)
            return false;
    }
    if (getenv("NO_COLOR")) return false;
    const char *term = getenv("TERM");
    if (term && strcmp(term, "dumb") == 0) return false;
    return isatty(fileno(stdout));
}

static QttTraceTheme qtt_trace_theme(void) {
    if (!qtt_trace_color_enabled())
        return (QttTraceTheme){"", "", "", "", "", "", "", "", "", "", ""};
    return (QttTraceTheme){
        .reset = "\033[0m",
        .bold = "\033[1m",
        .dim = "\033[2m",
        .cyan = "\033[36m",
        .magenta = "\033[35m",
        .green = "\033[32m",
        .yellow = "\033[33m",
        .red = "\033[31m",
        .bold_green = "\033[1;32m",
        .bold_yellow = "\033[1;33m",
        .bold_red = "\033[1;31m",
    };
}

static const char *evidence_status_color(
    const QttTraceTheme *theme, bool reached, bool valid) {
    if (!reached) return theme->dim;
    return valid ? theme->green : theme->yellow;
}

static const char *ownership_mode_name(QttOwnershipMode mode) {
    switch (mode) {
    case QTT_OWNERSHIP_ERASED: return "erased";
    case QTT_OWNERSHIP_BORROWED: return "borrowed";
    case QTT_OWNERSHIP_CONSUMED: return "consumed";
    case QTT_OWNERSHIP_SHARED: return "shared";
    }
    return "unknown";
}

static const char *evidence_result_mode_name(QttResultMode mode) {
    switch (mode) {
    case QTT_RESULT_IMMEDIATE: return "immediate";
    case QTT_RESULT_OWNED: return "owned";
    case QTT_RESULT_BORROWED: return "borrowed";
    case QTT_RESULT_SHARED: return "shared";
    case QTT_RESULT_UNKNOWN: return "unknown";
    }
    return "unknown";
}

static const char *evidence_result_origin_name(QttResultOrigin origin) {
    switch (origin) {
    case QTT_RESULT_ORIGIN_IMMEDIATE: return "immediate";
    case QTT_RESULT_ORIGIN_STATIC: return "static";
    case QTT_RESULT_ORIGIN_TRANSFERRED: return "transferred";
    case QTT_RESULT_ORIGIN_FRESH: return "fresh";
    case QTT_RESULT_ORIGIN_UNKNOWN: return "unknown";
    }
    return "unknown";
}

static const char *evidence_representation_name(
    QttRepresentation representation) {
    switch (representation) {
    case QTT_REP_IMMEDIATE: return "immediate";
    case QTT_REP_INLINE: return "inline";
    case QTT_REP_OWNED_HEAP: return "owned-heap";
    case QTT_REP_FOREIGN: return "foreign";
    case QTT_REP_UNKNOWN: return "unknown";
    }
    return "unknown";
}

static const char *anf_instruction_name(QttAnfInstructionKind kind) {
    switch (kind) {
    case QTT_ANF_CONST_NUMBER: return "const-number";
    case QTT_ANF_CONST_BOOL: return "const-bool";
    case QTT_ANF_CONST_STRING: return "const-string";
    case QTT_ANF_CONST_AGGREGATE: return "const-aggregate";
    case QTT_ANF_ALIAS: return "alias";
    case QTT_ANF_WRITE_PLACE: return "write-place";
    case QTT_ANF_REPLACE: return "replace";
    case QTT_ANF_ALLOC: return "alloc";
    case QTT_ANF_BORROW: return "borrow";
    case QTT_ANF_END_BORROW: return "end-borrow";
    case QTT_ANF_MOVE: return "move";
    case QTT_ANF_MOVE_PLACE: return "move-place";
    case QTT_ANF_DROP: return "drop";
    case QTT_ANF_ENV_PROJECT: return "environment-project";
    case QTT_ANF_CALL: return "call";
    }
    return "unknown-op";
}

static void format_quantity(
    char *buffer, size_t capacity, QttQuantity quantity) {
    if (quantity.is_omega)
        snprintf(buffer, capacity, "ω");
    else
        snprintf(buffer, capacity, "%llu",
                 (unsigned long long)quantity.finite);
}

static const char *demand_rule_name(QttDemandRule rule) {
    switch (rule) {
    case QTT_DEMAND_RULE_ZERO: return "Zero";
    case QTT_DEMAND_RULE_VAR: return "Var";
    case QTT_DEMAND_RULE_LET: return "Let";
    case QTT_DEMAND_RULE_IF: return "If";
    case QTT_DEMAND_RULE_SEQUENCE: return "Sequence";
    case QTT_DEMAND_RULE_APPLY: return "Apply";
    case QTT_DEMAND_RULE_LAMBDA: return "Lambda";
    case QTT_DEMAND_RULE_WRITE: return "Write";
    case QTT_DEMAND_RULE_BORROW: return "Borrow";
    }
    return "Unknown";
}

static const char *demand_premise_role(
    QttDemandRule parent, size_t index) {
    switch (parent) {
    case QTT_DEMAND_RULE_IF:
        return index == 0 ? "condition"
             : index == 1 ? "then" : "else";
    case QTT_DEMAND_RULE_LET:
        return index == 0 ? "value"
             : index == 1 ? "body" : "bound-demand";
    case QTT_DEMAND_RULE_APPLY:
        return index == 0 ? "callee" : "argument/latent";
    case QTT_DEMAND_RULE_LAMBDA:
        return "body";
    case QTT_DEMAND_RULE_WRITE:
        return "value";
    case QTT_DEMAND_RULE_BORROW:
        return "body";
    case QTT_DEMAND_RULE_SEQUENCE:
        return "item";
    default:
        return "premise";
    }
}

static void print_demand_derivation(
    const QttShadowDemandEvidence *demand,
    const char *prefix, bool last, const char *role) {
    if (!demand) {
        printf("%s%s ⚠ unavailable\n", prefix, last ? "└─" : "├─");
        return;
    }
    char syntactic[32], runtime[32];
    format_quantity(syntactic, sizeof(syntactic), demand->syntactic);
    format_quantity(runtime, sizeof(runtime), demand->runtime);
    printf("%s%s %s%s%s core=#%016llx [ρˢ=%s ρʳ=%s]\n",
           prefix, last ? "└─" : "├─",
           role ? role : "", role ? ": " : "",
           demand_rule_name(demand->rule),
           (unsigned long long)demand->node_id,
           syntactic, runtime);
    char child_prefix[512];
    snprintf(child_prefix, sizeof(child_prefix), "%s%s",
             prefix, last ? "   " : "│  ");
    if (demand->rule == QTT_DEMAND_RULE_IF)
        printf("%s├─ choice equation: ρʳ = ρc + (ρt ⊔ ρe)\n",
               child_prefix);
    else if (demand->rule == QTT_DEMAND_RULE_SEQUENCE)
        printf("%s├─ sequence equation: ρʳ = Σᵢ ρᵢ\n",
               child_prefix);
    for (size_t i = 0; i < demand->premise_count; i++)
        print_demand_derivation(
            demand->premises[i], child_prefix,
            i + 1 == demand->premise_count,
            demand_premise_role(demand->rule, i));
}

static void print_anf_instruction(
    const char *prefix, size_t index,
    const QttShadowInstructionEvidence *instruction) {
    const char *name = anf_instruction_name(instruction->kind);
    switch (instruction->kind) {
    case QTT_ANF_CONST_NUMBER:
    case QTT_ANF_CONST_BOOL:
    case QTT_ANF_CONST_STRING:
    case QTT_ANF_CONST_AGGREGATE:
        printf("%s├─ op i%zu: %s  %%v%u ← literal\n",
               prefix, index, name, instruction->result);
        break;
    case QTT_ANF_ALIAS:
        printf("%s├─ op i%zu: alias  %%v%u ← %%v%u\n",
               prefix, index, instruction->result,
               instruction->operand);
        break;
    case QTT_ANF_WRITE_PLACE:
        printf("%s├─ op i%zu: write-place  κ#%llu.%llu ← %%v%u",
               prefix, index,
               (unsigned long long)instruction->place.root.binder_id,
               (unsigned long long)instruction->place.projection_id,
               instruction->operand);
        if (instruction->loan_id)
            printf("  authority=ℓ#%llu",
                   (unsigned long long)instruction->loan_id);
        printf("\n");
        break;
    case QTT_ANF_REPLACE:
        printf("%s├─ op i%zu: replace  κ#%llu ← %%v%u  [%s]\n",
               prefix, index,
               (unsigned long long)instruction->resource.binder_id,
               instruction->operand,
               evidence_representation_name(
                   instruction->representation));
        break;
    case QTT_ANF_ALLOC:
        printf("%s├─ op i%zu: alloc  Γ ⇒ Γ ⊎ {κ#%llu} [%s]\n",
               prefix, index,
               (unsigned long long)instruction->resource.binder_id,
               evidence_representation_name(
                   instruction->representation));
        break;
    case QTT_ANF_BORROW:
        printf("%s├─ op i%zu: borrow-%s  %%v%u ← &κ#%llu",
               prefix, index,
               instruction->loan_kind == QTT_LOAN_EXCLUSIVE
                   ? "exclusive" : "shared",
               instruction->result,
               (unsigned long long)instruction->resource.binder_id);
        for (uint8_t depth = 0;
             depth < instruction->place.projection_depth; depth++)
            printf(".%u", instruction->place.projection_path[depth]);
        if (!instruction->place.projection_depth &&
            instruction->place.projection_id)
            printf(".%llu", (unsigned long long)
                   instruction->place.projection_id);
        if (instruction->loan_id)
            printf("  loan=ℓ#%llu [Λ ⇒ Λ ⊎ {ℓ}]",
                   (unsigned long long)instruction->loan_id);
        else
            printf("  [legacy lexical borrow]");
        if (instruction->parent_loan_id)
            printf("  parent=ℓ#%llu [reborrow]",
                   (unsigned long long)instruction->parent_loan_id);
        else if (instruction->loan_id)
            printf("  parent=root");
        printf("\n");
        break;
    case QTT_ANF_END_BORROW:
        printf("%s├─ op i%zu: end-borrow  ℓ#%llu  "
               "[Λ,ℓ ⇒ Λ]\n",
               prefix, index,
               (unsigned long long)instruction->loan_id);
        break;
    case QTT_ANF_MOVE:
        printf("%s├─ op i%zu: move  %%v%u ← κ#%llu  "
               "[Γ,κ ⇒ Γ]\n",
               prefix, index, instruction->result,
               (unsigned long long)instruction->resource.binder_id);
        break;
    case QTT_ANF_MOVE_PLACE:
        printf("%s├─ op i%zu: move-place  %%v%u ← κ#%llu",
               prefix, index, instruction->result,
               (unsigned long long)instruction->place.root.binder_id);
        for (uint8_t depth = 0;
             depth < instruction->place.projection_depth; depth++)
            printf(".%u", instruction->place.projection_path[depth]);
        printf("  [Γ,κ ⇒ Γ,κ∖p]\n");
        break;
    case QTT_ANF_DROP:
        printf("%s├─ op i%zu: drop  κ#%llu  [Γ,κ ⇒ Γ]\n",
               prefix, index,
               (unsigned long long)instruction->resource.binder_id);
        break;
    case QTT_ANF_ENV_PROJECT:
        printf("%s├─ op i%zu: environment-project  "
               "%%v%u ← env[κ#%llu]\n",
               prefix, index, instruction->result,
               (unsigned long long)instruction->resource.binder_id);
        break;
    case QTT_ANF_CALL:
        printf("%s├─ op i%zu: call  %%v%u ← %s#%016llx/%zu\n",
               prefix, index, instruction->result,
               instruction->callable.name
                   ? instruction->callable.name : "f",
               (unsigned long long)instruction->callable.name_hash,
               instruction->call_argument_count);
        break;
    }
}

static void print_anf_values(
    const QttAnfValue *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (i) printf(", ");
        printf("%%v%u", values[i]);
    }
}

static void print_shadow_evidence(const AST *name, const AST *lambda,
                                  uint64_t module_id,
                                  const QttShadowEvidence *evidence) {
    QttTraceTheme theme = qtt_trace_theme();
    QttShadowResult verification = evidence->summary;
    const char *definition =
        name && name->type == AST_SYMBOL ? name->symbol : "<anonymous>";
    printf("├─ %sdefinition %s%s%s %s@ %d:%d%s\n",
           theme.bold, theme.cyan, definition, theme.reset,
           theme.dim, lambda->line, lambda->column, theme.reset);
    printf("│  ├─ %sjudgment:%s %sΣ ; Ψ | Γ ⊢ℓ %s : A ! ε ▷ Δ%s\n",
           theme.bold, theme.reset, theme.magenta, definition, theme.reset);
    printf("│  ├─ source shape: parameters=%zu body-forms=%zu\n",
           verification.parameter_count, verification.body_count);
    printf("│  ├─ typing contexts\n");
    printf("│  │  ├─ Σ global signature: module=#%016llx\n",
           (unsigned long long)module_id);
    printf("│  │  ├─ Ψ modal/meta context: layer=0 staged-binders=0\n");
    printf("│  │  └─ Γ quantitative context: binders=%zu\n",
           verification.graded_parameter_count);
    const char *core_color = evidence_status_color(
        &theme, verification.core_lowered, verification.core_validated);
    printf("│  ├─ typed Core: %s%s%s\n",
           core_color,
           verification.core_validated
               ? "valid"
               : verification.core_lowered ? "invalid" : "not constructed",
           theme.reset);
    printf("│  │  ├─ lowering: %s%s%s\n",
           verification.core_error == QTT_CORE_OK
               ? theme.green : theme.yellow,
           core_error_name(verification.core_error), theme.reset);
    printf("│  │  └─ binding validation: %s%s%s\n",
           evidence_status_color(
               &theme, verification.core_lowered,
               verification.core_validation == QTT_CORE_VALID),
           verification.core_lowered
               ? core_validation_name(verification.core_validation)
               : "not reached",
           theme.reset);
    if (verification.signature_validated)
        printf("│  ├─ quantitative signature: %sgraded=%zu erased=%zu%s\n",
               theme.cyan,
               verification.graded_parameter_count,
               verification.erased_parameter_count, theme.reset);
    else
        printf("│  ├─ quantitative signature: %snot reached%s\n",
               theme.dim, theme.reset);
    printf("│  │  └─ canonical runtime types=%zu\n",
           verification.canonical_type_count);
    printf("│  ├─ binder contracts\n");
    if (!evidence->details_complete &&
        verification.graded_parameter_count) {
        printf("│  │  └─ ⚠ binder details unavailable\n");
    } else if (!evidence->parameter_count) {
        printf("│  │  └─ ∅ (closed term)\n");
    } else {
        for (size_t i = 0; i < evidence->parameter_count; i++) {
            const QttShadowParameterEvidence *parameter =
                &evidence->parameters[i];
            const char *parameter_name =
                i < (size_t)lambda->lambda.param_count &&
                lambda->lambda.params[i].name
                    ? lambda->lambda.params[i].name : "<unnamed>";
            const char *edge =
                i + 1 == evidence->parameter_count ? "└─" : "├─";
            const char *continuation =
                i + 1 == evidence->parameter_count ? "   " : "│  ";
            char allowance[32], observed[32];
            format_quantity(
                allowance, sizeof(allowance), parameter->allowance);
            format_quantity(
                observed, sizeof(observed), parameter->observed);
            printf("│  │  %s %s #%llu\n", edge, parameter_name,
                   (unsigned long long)parameter->var.binder_id);
            printf("│  │  %s├─ %squantity:%s ρ_allowed=%s ρ_observed=%s\n",
                   continuation, theme.cyan, theme.reset,
                   allowance, observed);
            printf("│  │  %s│  └─ %ssubusage proof:%s %s ≤ %s  %s✓%s\n",
                   continuation, theme.cyan, theme.reset,
                   observed, allowance, theme.green, theme.reset);
            printf("│  │  %s├─ demand derivation\n", continuation);
            char demand_prefix[128];
            snprintf(demand_prefix, sizeof(demand_prefix),
                     "│  │  %s│  ", continuation);
            QttShadowDemandValidation demand_validation =
                qtt_shadow_demand_validate(parameter->demand);
            printf("%s├─ %sportable certificate:%s %s%s%s %s%s%s\n",
                   demand_prefix, theme.cyan, theme.reset,
                   demand_validation == QTT_SHADOW_DEMAND_VALID
                       ? theme.green : theme.red,
                   demand_validation == QTT_SHADOW_DEMAND_VALID
                       ? "valid-after-Core-free" : "INVALID",
                   theme.reset,
                   demand_validation == QTT_SHADOW_DEMAND_VALID
                       ? theme.green : "",
                   demand_validation == QTT_SHADOW_DEMAND_VALID ? "✓" : "",
                   theme.reset);
            print_demand_derivation(
                parameter->demand, demand_prefix, true, NULL);
            printf("│  │  %s├─ %sownership:%s %s\n", continuation,
                   theme.cyan, theme.reset,
                   ownership_mode_name(parameter->mode));
            printf("│  │  %s├─ %srepresentation:%s %s\n", continuation,
                   theme.cyan, theme.reset,
                   evidence_representation_name(
                       parameter->representation));
            printf("│  │  %s└─ %scanonical type:%s #%016llx\n",
                   continuation, theme.cyan, theme.reset,
                   (unsigned long long)parameter->type_id.value);
        }
    }
    if (!evidence->details_complete && evidence->parameter_count)
        printf("│  │  └─ ⚠ auxiliary detail allocation incomplete\n");
    printf("│  ├─ result contract\n");
    if (!verification.signature_validated ||
        !evidence->result_type_id.value) {
        printf("│  │  └─ ∅ (not available in this fragment)\n");
    } else {
        printf("│  │  ├─ %scapability:%s %s\n", theme.cyan, theme.reset,
               evidence_result_mode_name(evidence->result_mode));
        printf("│  │  ├─ %sprovenance:%s %s\n", theme.cyan, theme.reset,
               evidence_result_origin_name(evidence->result_origin));
        printf("│  │  ├─ %srepresentation:%s %s\n", theme.cyan, theme.reset,
               evidence_representation_name(
                   evidence->result_representation));
        printf("│  │  ├─ %scanonical type:%s #%016llx\n",
               theme.cyan, theme.reset,
               (unsigned long long)evidence->result_type_id.value);
        printf("│  │  └─ %scontract digest:%s #%016llx\n",
               theme.cyan, theme.reset,
               (unsigned long long)evidence->contract_fingerprint);
    }
    const char *anf_status = verification.anf_lowered
        ? "valid"
        : verification.lower_error != QTT_ANF_LOWER_OK
            ? anf_lower_error_name(verification.lower_error)
            : "not reached";
    printf("│  ├─ ownership ANF: %s%s%s blocks=%zu instructions=%zu\n",
           evidence_status_color(
               &theme,
               verification.anf_lowered ||
                   verification.lower_error != QTT_ANF_LOWER_OK,
               verification.anf_lowered),
           anf_status, theme.reset, verification.block_count,
           verification.instruction_count);
    printf("│  │  %s %slowering obligation%s: %s%s%s\n",
           evidence->block_count ? "├─" : "└─",
           theme.dim, theme.reset,
           evidence_status_color(
               &theme,
               verification.anf_lowered ||
                   verification.lower_error != QTT_ANF_LOWER_OK,
               verification.anf_lowered),
           verification.anf_lowered ||
               verification.lower_error != QTT_ANF_LOWER_OK
               ? anf_lower_error_name(verification.lower_error)
               : "not reached",
           theme.reset);
    for (size_t i = 0; i < evidence->block_count; i++) {
        const QttShadowBlockEvidence *block = &evidence->blocks[i];
        const char *edge =
            i + 1 == evidence->block_count ? "└─" : "├─";
        const char *continuation =
            i + 1 == evidence->block_count ? "   " : "│  ";
        printf("│  │  %s block b%zu (φ-parameters=%zu)\n",
               edge, i, block->parameter_count);
        if (block->parameter_count) {
            printf("│  │  %s├─ φ(", continuation);
            print_anf_values(block->parameters, block->parameter_count);
            printf(")\n");
        }
        for (size_t j = 0; j < block->instruction_count; j++)
            {
                char prefix[64];
                snprintf(prefix, sizeof(prefix), "│  │  %s",
                         continuation);
                print_anf_instruction(
                    prefix, j, &block->instructions[j]);
            }
        if (block->terminator == QTT_ANF_RETURN) {
            printf("│  │  %s└─ terminator: return %%v%u\n",
                   continuation, block->terminator_value);
        } else if (block->terminator == QTT_ANF_JUMP) {
            printf("│  │  %s└─ terminator: jump\n", continuation);
            printf("│  │  %s   └─ edge ⇒ b%zu(", continuation,
                   block->target);
            print_anf_values(block->arguments, block->argument_count);
            printf(")\n");
        } else {
            printf("│  │  %s└─ terminator: branch %%v%u\n",
                   continuation, block->terminator_value);
            printf("│  │  %s   ├─ then ⇒ b%zu(", continuation,
                   block->target);
            print_anf_values(block->arguments, block->argument_count);
            printf(")\n");
            printf("│  │  %s   └─ else ⇒ b%zu(", continuation,
                   block->else_target);
            print_anf_values(
                block->else_arguments, block->else_argument_count);
            printf(")\n");
        }
    }
    printf("│  ├─ certificate check: %s%s%s\n",
           evidence_status_color(
               &theme, verification.verification_ran,
               verification.verification_error == QTT_ANF_VALID),
           verification.verification_ran
               ? anf_verification_error_name(
                     verification.verification_error)
               : "not reached",
           theme.reset);
    printf("│  │  └─ %sresource/control-flow obligations%s: %s%s%s\n",
           theme.dim, theme.reset,
           evidence_status_color(
               &theme, verification.verification_ran,
               verification.verification_error == QTT_ANF_VALID),
           !verification.verification_ran
               ? "pending"
               : verification.verification_error == QTT_ANF_VALID
                   ? "discharged" : "not discharged",
           theme.reset);
    if (verification.status == QTT_SHADOW_VERIFIED)
        printf("│  └─ verdict: %sVERIFIED%s — %ssafe for certified QTT consumers%s\n",
               theme.bold_green, theme.reset, theme.green, theme.reset);
    else if (verification.status == QTT_SHADOW_UNSUPPORTED)
        printf("│  └─ verdict: %sUNSUPPORTED%s — %sconservative codegen retained%s\n",
               theme.bold_yellow, theme.reset, theme.yellow, theme.reset);
    else
        printf("│  └─ verdict: %sINTERNAL ERROR%s — %scertificate rejected%s\n",
               theme.bold_red, theme.reset, theme.red, theme.reset);
    /*
     * Imported modules may be compiled concurrently into one pipe.  Flush one
     * complete, sub-PIPE_BUF derivation at a time so UTF-8 tree edges cannot
     * be split and interleaved between compiler processes.
     */
    fflush(stdout);
}

QttCompilerPipelineResult qtt_compiler_run_source_pipeline(
    const char *identity, AST *const *forms, size_t form_count) {
    QttCompilerPipelineResult result = {
        .status = QTT_COMPILER_PIPELINE_DISABLED};
    if (!qtt_compiler_analysis_enabled()) return result;
    uint64_t module_id = qtt_compiler_module_id(identity);
    bool trace = qtt_compiler_trace_enabled() ||
                 qtt_compiler_diagnostics_enabled();
    bool detailed = qtt_compiler_trace_detailed() ||
                    qtt_compiler_diagnostics_enabled();
    QttTraceTheme theme = qtt_trace_theme();
    if (trace && detailed)
        printf("%s%s[qtt] Quantitative ownership evidence%s\n"
               "│  %smodule:%s %s%s%s\n"
               "│  %stheorem:%s %scertified definitions satisfy typed ownership ANF%s\n",
               theme.bold, theme.cyan, theme.reset,
               theme.dim, theme.reset, theme.cyan,
               identity ? identity : "<unknown>", theme.reset,
               theme.dim, theme.reset, theme.magenta, theme.reset);
    if (trace && detailed) fflush(stdout);
    /*
     * Establish Σ before checking any body. This makes same-module calls use
     * the same canonical contracts later consumed by code generation; the
     * normal definition-emission registration is idempotent by AST identity.
     */
    for (size_t i = 0; i < form_count; i++) {
        AST *form = forms ? forms[i] : NULL;
        if (!form || form->type != AST_LIST || form->list.count < 3 ||
            !form->list.items[0] ||
            form->list.items[0]->type != AST_SYMBOL ||
            strcmp(form->list.items[0]->symbol, "define") != 0 ||
            !form->list.items[1] ||
            form->list.items[1]->type != AST_SYMBOL ||
            !form->list.items[2] ||
            form->list.items[2]->type != AST_LAMBDA)
            continue;
        qtt_compiler_register_definition(
            identity, form->list.items[1]->symbol,
            form->list.items[2]);
    }
    QttCompilerModule *module_entry = compiler_module(identity, false);
    QttTransferFixedPoint transfers = module_entry && module_entry->valid
        ? qtt_module_solve_transfers(module_entry->module)
        : (QttTransferFixedPoint){0};
    if (!transfers.complete) {
        result.status = QTT_COMPILER_PIPELINE_INTERNAL_ERROR;
        result.internal_error_count = 1;
        if (trace)
            printf("│  transfer fixed point: module=%s INCOMPLETE "
                   "(out-of-memory)\n",
                   identity ? identity : "<unknown>");
        return result;
    }
    if (trace && detailed && transfers.candidate_count)
        printf("│  transfer fixed point: module=%s "
               "candidates=%zu proven=%zu "
               "rejected=%zu iterations=%zu\n",
               identity ? identity : "<unknown>",
               transfers.candidate_count, transfers.proven_count,
               transfers.rejected_count, transfers.iterations);
    const QttSignatureEnv *signatures =
        qtt_compiler_signatures(identity);
    for (size_t i = 0; i < form_count; i++) {
        AST *form = forms ? forms[i] : NULL;
        if (!form || form->type != AST_LIST ||
            form->list.count < 3)
            continue;
        AST *head = form->list.items[0];
        AST *value = form->list.items[2];
        if (!head || head->type != AST_SYMBOL ||
            strcmp(head->symbol, "define") != 0 ||
            !value || value->type != AST_LAMBDA)
            continue;
        QttShadowEvidence evidence =
            qtt_shadow_collect_lambda_in_env(
                value, module_id, signatures);
        QttShadowResult verification = evidence.summary;
        if (trace && detailed)
            print_shadow_evidence(
                form->list.items[1], value, module_id, &evidence);
        if (verification.status == QTT_SHADOW_VERIFIED)
            result.verified_count++;
        else if (verification.status == QTT_SHADOW_UNSUPPORTED)
            result.unsupported_count++;
        else
            result.internal_error_count++;
        qtt_shadow_evidence_free(&evidence);
    }
    if (result.internal_error_count)
        result.status = QTT_COMPILER_PIPELINE_INTERNAL_ERROR;
    else if (result.unsupported_count)
        result.status = QTT_COMPILER_PIPELINE_PARTIAL;
    else
        result.status = QTT_COMPILER_PIPELINE_VERIFIED;
    if (trace) {
        const char *verdict_color =
            result.status == QTT_COMPILER_PIPELINE_VERIFIED
                ? theme.bold_green
                : result.status == QTT_COMPILER_PIPELINE_PARTIAL
                    ? theme.bold_yellow : theme.bold_red;
        if (detailed)
            printf("└─ module verdict: %s%s%s\n",
                   verdict_color, pipeline_status_name(result.status),
                   theme.reset);
        const char *display = identity ? identity : "<unknown>";
        const char *slash = strrchr(display, '/');
        if (slash && slash[1]) display = slash + 1;
        size_t total = result.verified_count + result.unsupported_count +
                       result.internal_error_count;
        printf("%s%s[qtt]%s %s%s%s  %s\n",
               theme.bold, theme.cyan, theme.reset,
               verdict_color, pipeline_status_name(result.status), theme.reset,
               display);
        printf("      %scoverage%s %zu/%zu verified · "
               "%zu conservative · %zu errors\n",
               theme.cyan, theme.reset, result.verified_count, total,
               result.unsupported_count, result.internal_error_count);
        if (result.status != QTT_COMPILER_PIPELINE_VERIFIED && !detailed)
            printf("      %sinspect%s  --trace=qtt-proof\n",
                   theme.cyan, theme.reset);
        fflush(stdout);
    }
    return result;
}

static const char *result_mode_name(QttResultMode mode) {
    switch (mode) {
    case QTT_RESULT_IMMEDIATE: return "immediate";
    case QTT_RESULT_OWNED: return "owned";
    case QTT_RESULT_BORROWED: return "borrowed";
    case QTT_RESULT_SHARED: return "shared";
    case QTT_RESULT_UNKNOWN: return "unknown";
    }
    return "unknown";
}

static const char *result_origin_name(QttResultOrigin origin) {
    switch (origin) {
    case QTT_RESULT_ORIGIN_IMMEDIATE: return "immediate";
    case QTT_RESULT_ORIGIN_STATIC: return "static";
    case QTT_RESULT_ORIGIN_TRANSFERRED: return "transferred";
    case QTT_RESULT_ORIGIN_FRESH: return "fresh";
    case QTT_RESULT_ORIGIN_UNKNOWN: return "unknown";
    }
    return "unknown";
}

static const char *representation_name(QttRepresentation representation) {
    switch (representation) {
    case QTT_REP_IMMEDIATE: return "immediate";
    case QTT_REP_INLINE: return "inline";
    case QTT_REP_OWNED_HEAP: return "owned-heap";
    case QTT_REP_FOREIGN: return "foreign";
    case QTT_REP_UNKNOWN: return "unknown";
    }
    return "unknown";
}

bool qtt_compiler_register_import(
    const char *identity, const char *visible_name,
    const char *provider_module, const char *provider_name,
    const AST *lambda) {
    const AST *implementation = lambda;
    if (implementation && implementation->type == AST_LIST)
        for (size_t i = 0; i < implementation->list.count; i++)
            if (implementation->list.items[i] &&
                implementation->list.items[i]->type == AST_LAMBDA) {
                implementation = implementation->list.items[i];
                break;
            }
    if (!implementation || implementation->type != AST_LAMBDA)
        return false;
    QttCompilerModule *entry = compiler_module(identity, true);
    if (!entry || !entry->valid) return false;
    QttCompilerDefinition *stored = NULL;
    for (QttCompilerDefinition *definition = entry->definitions;
         definition; definition = definition->next)
        if (strcmp(definition->name, visible_name) == 0) {
            stored = definition;
            break;
        }
    if (stored) {
        if (!stored->provider_module || !stored->provider_name ||
            strcmp(stored->provider_module,
                   provider_module ? provider_module : "") != 0 ||
            strcmp(stored->provider_name,
                   provider_name ? provider_name : visible_name) != 0) {
            /*
             * Ambiguous unqualified imports are resolved by the ordinary
             * environment. They must not poison unrelated qualified QTT
             * contracts; simply withhold quantitative evidence for this
             * colliding alias.
             */
            return false;
        }
    } else {
        if (!qtt_compiler_register_definition(
                identity, visible_name, implementation))
            return false;
        for (QttCompilerDefinition *definition = entry->definitions;
             definition; definition = definition->next)
            if (strcmp(definition->name, visible_name) == 0) {
                stored = definition;
                break;
            }
        if (!stored) return false;
        stored->provider_module =
            copy_text(provider_module ? provider_module : "");
        stored->provider_name =
            copy_text(provider_name ? provider_name : visible_name);
        if (!stored->provider_module || !stored->provider_name) {
            entry->valid = false;
            return false;
        }
    }
    if (!qtt_compiler_trace_detailed()) return true;
    QttFunctionSignature *signature = entry && entry->valid
        ? qtt_signature_env_lookup(
              qtt_module_signatures(entry->module),
              entry->module_id, visible_name)
        : NULL;
    if (signature)
        printf("[qtt] import contract %s.%s as %s: "
               "result=%s/%s representation=%s "
               "fingerprint=%016llx\n",
               stored->provider_module[0]
                   ? stored->provider_module : "<module>",
               stored->provider_name,
               visible_name,
               result_mode_name(signature->result.mode),
               result_origin_name(signature->result.origin),
               representation_name(signature->result.representation),
               (unsigned long long)
                   signature->contract_fingerprint);
    return true;
}

bool qtt_compiler_register_contract(
    const char *identity, const char *visible_name,
    const char *provider_module, const char *provider_name,
    const QttFunctionSignature *contract) {
    if (!qtt_compiler_analysis_enabled() || !visible_name ||
        !provider_module || !provider_name || !contract)
        return false;
    QttCompilerModule *entry = compiler_module(identity, true);
    if (!entry || !entry->valid) return false;
    for (QttCompilerDefinition *definition = entry->definitions;
         definition; definition = definition->next) {
        if (strcmp(definition->name, visible_name) != 0) continue;
        return definition->provider_module &&
               definition->provider_name &&
               strcmp(definition->provider_module, provider_module) == 0 &&
               strcmp(definition->provider_name, provider_name) == 0;
    }
    QttCompilerDefinition *definition = calloc(1, sizeof(*definition));
    if (!definition) return false;
    definition->name = copy_text(visible_name);
    definition->provider_module = copy_text(provider_module);
    definition->provider_name = copy_text(provider_name);
    definition->external_contract = contract;
    if (!definition->name || !definition->provider_module ||
        !definition->provider_name ||
        qtt_module_declare_external(
            entry->module, visible_name, contract) != QTT_MODULE_OK) {
        free(definition->name);
        free(definition->provider_module);
        free(definition->provider_name);
        free(definition);
        return false;
    }
    definition->next = entry->definitions;
    entry->definitions = definition;
    if (qtt_compiler_trace_detailed())
        printf("[qtt] imported verified interface contract %s.%s as %s "
               "fingerprint=%016llx\n",
               provider_module, provider_name, visible_name,
               (unsigned long long)
                   qtt_interface_signature_fingerprint(contract));
    return true;
}

bool qtt_compiler_write_interface(
    const char *identity, const char *module_name,
    const char *path, uint64_t artifact_fingerprint) {
    QttCompilerModule *entry = compiler_module(identity, false);
    if (!entry || !entry->valid || !module_name || !path)
        return false;
    QttInterface *interface = qtt_interface_new(module_name);
    if (!interface) return false;
    qtt_interface_set_artifact_fingerprint(
        interface, artifact_fingerprint);
    for (size_t i = 0; i < entry->effect_declaration_count; i++)
        if (!qtt_interface_add_effect_declaration(
                interface, &entry->effect_declarations[i])) {
            if (qtt_compiler_trace_detailed())
                fprintf(stderr, "[qtt] interface rejected effect declaration '%s'\n",
                    entry->effect_declarations[i].name);
            qtt_interface_free(interface);
            return false;
        }
    for (size_t i = 0; i < entry->handler_profile_count; i++)
        if (!qtt_interface_add_handler_profile(
                interface, &entry->handler_profiles[i])) {
            if (qtt_compiler_trace_detailed())
                fprintf(stderr, "[qtt] interface rejected handler profile '%s'\n",
                    entry->handler_profiles[i].name);
            qtt_interface_free(interface);
            return false;
        }
    for (size_t i = 0; i < entry->trait_implication_count; i++)
        if (!qtt_interface_add_trait_implication(interface,
                entry->trait_implications[i].premise,
                entry->trait_implications[i].consequence, module_name)) {
            if (qtt_compiler_trace_detailed())
                fprintf(stderr, "[qtt] interface rejected trait implication '%s => %s'\n",
                    entry->trait_implications[i].premise,
                    entry->trait_implications[i].consequence);
            qtt_interface_free(interface);
            return false;
        }
    const QttSignatureEnv *signatures =
        qtt_module_signatures(entry->module);
    for (QttCompilerDefinition *definition = entry->definitions;
         definition; definition = definition->next) {
        /* Re-exporting an imported alias would forge provider provenance. */
        if (definition->provider_module) continue;
        QttFunctionSignature *signature = qtt_signature_env_lookup(
            signatures, entry->module_id, definition->name);
        /*
         * Version 1 deliberately serializes only closed, flat ABI types.
         * Unsupported contracts are absent evidence, never guessed evidence.
         */
        bool added = signature && qtt_interface_add(
            interface, definition->name, signature);
        /* A polymorphic structural signature may intentionally be outside
         * the closed ownership ABI grammar. Preserve its independently
         * checkable HM/effect metadata instead of dropping the export. */
        if (!added && definition->hm_scheme)
            added = qtt_interface_add_metadata(
                interface, definition->name);
        if (added && definition->callable_contract)
            (void)qtt_interface_set_callable_contract(
                interface, definition->name,
                definition->callable_contract,
                definition->callable_contract_fingerprint);
        if (added && definition->callable_contract &&
            !export_effect_judgment(interface, definition)) {
            if (qtt_compiler_trace_detailed())
                fprintf(stderr,
                    "[qtt] interface rejected '%s': callable contract has "
                    "no coherent effect judgment\n", definition->name);
            qtt_interface_free(interface);
            return false;
        }
        if (added && definition->hm_scheme)
            (void)qtt_interface_set_hm_scheme(
                interface, definition->name, definition->hm_scheme);
    }
    QttInterfaceError written =
        qtt_interface_write(interface, path);
    if (written != QTT_INTERFACE_OK && qtt_compiler_trace_detailed())
        fprintf(stderr,
            "[qtt] failed to write interface '%s' for module '%s': error=%d\n",
            path, module_name, (int)written);
    qtt_interface_free(interface);
    return written == QTT_INTERFACE_OK;
}

void qtt_compiler_trace_module(const char *identity) {
    if (!qtt_compiler_trace_enabled()) return;
    if (!qtt_compiler_trace_detailed()) return;
    QttCompilerModule *entry = compiler_module(identity, false);
    size_t definitions = 0;
    size_t signatures = 0;
    const QttSignatureEnv *environment = entry && entry->valid
        ? qtt_module_signatures(entry->module) : NULL;
    for (QttCompilerDefinition *definition =
             entry ? entry->definitions : NULL;
         definition; definition = definition->next) {
        if (definition->provider_module) continue;
        definitions++;
        if (environment && qtt_signature_env_lookup(
                environment, entry->module_id, definition->name))
            signatures++;
    }
    printf("[qtt] module summary: definitions=%zu contracts=%zu "
           "analysis=%s memory=%s\n",
           definitions, signatures,
           qtt_compiler_analysis_enabled() ? "enabled" : "disabled",
           qtt_compiler_memory_enabled() ? "enabled" : "disabled");
    if (!entry || !entry->valid) return;
    for (QttCompilerDefinition *definition = entry->definitions;
         definition; definition = definition->next) {
        if (definition->provider_module) continue;
        QttFunctionSignature *signature =
            qtt_signature_env_lookup(
                environment, entry->module_id, definition->name);
        if (!signature) continue;
        printf("[qtt] contract %s: result=%s/%s "
               "representation=%s parameters=%zu fingerprint=%016llx\n",
               definition->name,
               result_mode_name(signature->result.mode),
               result_origin_name(signature->result.origin),
               representation_name(signature->result.representation),
               signature->parameter_count,
               (unsigned long long)signature->contract_fingerprint);
    }
}
