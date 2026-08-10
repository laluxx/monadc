#include "module.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const QttCoreNode *core;
    QttFunctionSignature *signature;
    bool external;
} QttModuleDefinition;

struct QttModule {
    uint64_t module_id;
    QttTypeArena *types;
    QttSignatureEnv *signatures;
    QttModuleDefinition *definitions;
    size_t count;
    size_t capacity;
};

static const QttFunctionSignature *module_effect_lookup(
    void *context, uint64_t module_id, const char *name) {
    QttModule *module = context;
    if (!module || module_id != module->module_id) return NULL;
    return qtt_signature_env_lookup(
        module->signatures, module_id, name);
}

QttEffectFixedPoint qtt_module_solve_effects(QttModule *module) {
    QttEffectFixedPoint summary = {0};
    if (!module) return summary;

    /* Kleene iteration begins at each body's intrinsic row.  Declarations
     * already contain that row; seeding internal completeness to true treats
     * recursive edges as equations, while missing or incomplete external
     * contracts monotonically propagate false through the call graph. */
    for (size_t i = 0; i < module->count; i++)
        if (!module->definitions[i].external) {
            if (!module->definitions[i].signature->effects_complete)
                summary.changed_count++;
            module->definitions[i].signature->effects_complete = true;
        }

    size_t limit = 1024 + module->count * module->count;
    bool changed;
    do {
        changed = false;
        summary.iterations++;
        for (size_t i = 0; i < module->count; i++) {
            QttModuleDefinition *definition = &module->definitions[i];
            if (definition->external) continue;
            QttSignatureEffectRefresh refresh =
                qtt_signature_refresh_effects(
                    definition->signature, module->module_id,
                    module_effect_lookup, module);
            if (refresh == QTT_SIGNATURE_EFFECT_REFRESH_OUT_OF_MEMORY)
                return summary;
            if (refresh == QTT_SIGNATURE_EFFECT_REFRESH_CHANGED) {
                changed = true;
                summary.changed_count++;
            }
        }
        if (summary.iterations >= limit && changed)
            return summary;
    } while (changed);
    summary.converged = true;
    for (size_t i = 0; i < module->count; i++) {
        QttModuleDefinition *definition = &module->definitions[i];
        if (!definition->external &&
            !definition->signature->effects_complete)
            summary.incomplete_count++;
        definition->signature->contract_fingerprint =
            qtt_signature_contract_fingerprint(definition->signature);
    }
    summary.complete = summary.incomplete_count == 0;
    return summary;
}

static bool tail_consumes(
    const QttCoreNode *core, QttCoreVar var,
    const QttSignatureEnv *signatures, uint64_t module_id) {
    if (!core) return false;
    if (core->kind == QTT_CORE_VAR)
        return qtt_core_var_equal(core->var, var);
    if (core->kind == QTT_CORE_LET)
        return tail_consumes(
            core->let.body, var, signatures, module_id);
    if (core->kind == QTT_CORE_SEQUENCE && core->sequence.count)
        return tail_consumes(
            core->sequence.items[core->sequence.count - 1],
            var, signatures, module_id);
    if (core->kind == QTT_CORE_IF)
        return tail_consumes(
                   core->conditional.then_branch, var,
                   signatures, module_id) &&
               tail_consumes(
                   core->conditional.else_branch, var,
                   signatures, module_id);
    if (core->kind != QTT_CORE_APPLY || !core->apply.callee ||
        core->apply.callee->kind != QTT_CORE_GLOBAL)
        return false;
    QttFunctionSignature *callee = qtt_signature_env_lookup(
        signatures, module_id, core->apply.callee->global.name);
    if (!callee ||
        callee->parameter_count != core->apply.argument_count)
        return false;
    for (size_t i = 0; i < callee->parameter_count; i++)
        if (callee->parameters[i].mode == QTT_OWNERSHIP_CONSUMED &&
            tail_consumes(core->apply.arguments[i], var,
                          signatures, module_id))
            return true;
    return false;
}

QttTransferFixedPoint qtt_module_solve_transfers(
    QttModule *module) {
    QttTransferFixedPoint summary = {0};
    if (!module) return summary;
    size_t parameter_count = 0;
    for (size_t i = 0; i < module->count; i++)
        parameter_count +=
            module->definitions[i].signature->parameter_count;
    bool *candidates = calloc(
        parameter_count ? parameter_count : 1, sizeof(*candidates));
    if (candidates) {
        summary.complete = true;
        size_t ordinal = 0;
        for (size_t i = 0; i < module->count; i++) {
            QttModuleDefinition *definition = &module->definitions[i];
            for (size_t parameter = 0;
                 parameter < definition->signature->parameter_count;
                 parameter++, ordinal++) {
                QttParameterContract *contract =
                    &definition->signature->parameters[parameter];
                candidates[ordinal] = !definition->external &&
                    contract->mode == QTT_OWNERSHIP_BORROWED &&
                    contract->representation == QTT_REP_OWNED_HEAP;
                if (candidates[ordinal])
                    contract->mode = QTT_OWNERSHIP_CONSUMED;
                if (candidates[ordinal]) summary.candidate_count++;
            }
        }

        /*
         * Must-transfer is a greatest-fixed-point property.  Start every
         * eligible unique parameter as transferred, then monotonically
         * remove candidates whose tail does not hand the capability to a
         * consumed parameter.  A closed recursive cycle therefore proves
         * itself coinductively; any retaining/non-tail edge breaks the cycle.
         */
        bool changed;
        do {
            summary.iterations++;
            changed = false;
            ordinal = 0;
            for (size_t i = 0; i < module->count; i++) {
                QttModuleDefinition *definition = &module->definitions[i];
                for (size_t parameter = 0;
                     parameter < definition->signature->parameter_count;
                     parameter++, ordinal++) {
                    if (!candidates[ordinal]) continue;
                    QttParameterContract *contract =
                        &definition->signature->parameters[parameter];
                    if (tail_consumes(
                            definition->core->lambda.body, contract->var,
                            module->signatures, module->module_id))
                        continue;
                    contract->mode = QTT_OWNERSHIP_BORROWED;
                    candidates[ordinal] = false;
                    summary.rejected_count++;
                    changed = true;
                }
            }
        } while (changed);
        summary.proven_count =
            summary.candidate_count - summary.rejected_count;
        free(candidates);
    }
    for (size_t i = 0; i < module->count; i++)
        module->definitions[i].signature->contract_fingerprint =
            qtt_signature_contract_fingerprint(
                module->definitions[i].signature);
    return summary;
}

QttModule *qtt_module_new(uint64_t module_id) {
    if (!module_id) return NULL;
    QttModule *module = calloc(1, sizeof(*module));
    if (!module) return NULL;
    module->module_id = module_id;
    module->types = qtt_type_arena_new();
    if (module->types)
        module->signatures = qtt_signature_env_new(module->types);
    if (!module->types || !module->signatures) {
        qtt_module_free(module);
        return NULL;
    }
    return module;
}

QttModuleError qtt_module_declare(
    QttModule *module, const char *name, const QttCoreNode *lambda) {
    if (!module || !name || !lambda ||
        lambda->kind != QTT_CORE_LAMBDA ||
        qtt_core_validate(lambda, module->module_id) != QTT_CORE_VALID)
        return QTT_MODULE_INVALID_CORE;
    if (module->count == module->capacity) {
        size_t next = module->capacity ? module->capacity * 2 : 16;
        QttModuleDefinition *grown = realloc(
            module->definitions, next * sizeof(*grown));
        if (!grown) return QTT_MODULE_OUT_OF_MEMORY;
        module->definitions = grown;
        module->capacity = next;
    }
    QttSignatureError signature_error = QTT_SIGNATURE_OK;
    QttFunctionSignature *signature =
        qtt_signature_derive(lambda, &signature_error);
    if (!signature)
        return signature_error == QTT_SIGNATURE_OUT_OF_MEMORY
            ? QTT_MODULE_OUT_OF_MEMORY : QTT_MODULE_INVALID_SIGNATURE;
    QttSignatureCanonicalization canonical =
        qtt_signature_canonicalize(signature, module->types);
    if (canonical != QTT_SIGNATURE_CANONICAL) {
        qtt_signature_free(signature);
        return canonical == QTT_SIGNATURE_CANONICAL_OUT_OF_MEMORY
            ? QTT_MODULE_OUT_OF_MEMORY : QTT_MODULE_INVALID_SIGNATURE;
    }
    QttSignatureEnvError registration = qtt_signature_env_register(
        module->signatures, module->module_id, name, signature, NULL);
    if (registration != QTT_SIGNATURE_ENV_OK) {
        qtt_signature_free(signature);
        if (registration == QTT_SIGNATURE_ENV_OUT_OF_MEMORY)
            return QTT_MODULE_OUT_OF_MEMORY;
        if (registration == QTT_SIGNATURE_ENV_DUPLICATE)
            return QTT_MODULE_DUPLICATE;
        return QTT_MODULE_INVALID_SIGNATURE;
    }
    module->definitions[module->count++] =
        (QttModuleDefinition){lambda, signature, false};
    return QTT_MODULE_OK;
}

QttModuleError qtt_module_declare_external(
    QttModule *module, const char *name,
    const QttFunctionSignature *contract) {
    if (!module || !name || !contract || !contract->result.type)
        return QTT_MODULE_INVALID_SIGNATURE;
    if (module->count == module->capacity) {
        size_t next = module->capacity ? module->capacity * 2 : 16;
        QttModuleDefinition *grown = realloc(
            module->definitions, next * sizeof(*grown));
        if (!grown) return QTT_MODULE_OUT_OF_MEMORY;
        module->definitions = grown;
        module->capacity = next;
    }
    QttFunctionSignature *signature = calloc(1, sizeof(*signature));
    if (!signature) return QTT_MODULE_OUT_OF_MEMORY;
    *signature = *contract;
    signature->source = NULL;
    signature->demand = NULL;
    signature->graded = NULL;
    signature->effect_arena = NULL;
    signature->effect_solver = NULL;
    signature->latent_effects = NULL;
    signature->parameters = NULL;
    if (contract->effects_complete) {
        signature->effect_arena = qtt_effect_arena_new();
        signature->effect_solver = signature->effect_arena
            ? qtt_effect_solver_new(signature->effect_arena) : NULL;
        signature->latent_effects = signature->effect_solver &&
                contract->effect_solver && contract->latent_effects
            ? qtt_effect_clone_closed(
                  signature->effect_arena, contract->effect_solver,
                  contract->latent_effects)
            : NULL;
        if (!signature->latent_effects) {
            qtt_signature_free(signature);
            return QTT_MODULE_OUT_OF_MEMORY;
        }
        signature->effect_fingerprint = qtt_effect_row_fingerprint(
            signature->effect_solver, signature->latent_effects);
    }
    if (contract->parameter_count) {
        signature->parameters = malloc(
            contract->parameter_count * sizeof(*signature->parameters));
        if (!signature->parameters) {
            qtt_signature_free(signature);
            return QTT_MODULE_OUT_OF_MEMORY;
        }
        memcpy(signature->parameters, contract->parameters,
               contract->parameter_count * sizeof(*signature->parameters));
    }
    QttSignatureCanonicalization canonical =
        qtt_signature_canonicalize(signature, module->types);
    if (canonical != QTT_SIGNATURE_CANONICAL) {
        qtt_signature_free(signature);
        return canonical == QTT_SIGNATURE_CANONICAL_OUT_OF_MEMORY
            ? QTT_MODULE_OUT_OF_MEMORY : QTT_MODULE_INVALID_SIGNATURE;
    }
    QttSignatureEnvError registered = qtt_signature_env_register(
        module->signatures, module->module_id, name, signature, NULL);
    if (registered != QTT_SIGNATURE_ENV_OK) {
        qtt_signature_free(signature);
        return registered == QTT_SIGNATURE_ENV_OUT_OF_MEMORY
            ? QTT_MODULE_OUT_OF_MEMORY
            : registered == QTT_SIGNATURE_ENV_DUPLICATE
                ? QTT_MODULE_DUPLICATE
                : QTT_MODULE_INVALID_SIGNATURE;
    }
    module->definitions[module->count++] =
        (QttModuleDefinition){NULL, signature, true};
    return QTT_MODULE_OK;
}

QttModuleVerification qtt_module_verify(QttModule *module) {
    QttModuleVerification result = {0};
    if (!module) {
        result.error = QTT_MODULE_INVALID_CORE;
        return result;
    }
    result.effects = qtt_module_solve_effects(module);
    if (!result.effects.converged) {
        result.error = QTT_MODULE_OUT_OF_MEMORY;
        return result;
    }
    result.transfers = qtt_module_solve_transfers(module);
    if (!result.transfers.complete) {
        result.error = QTT_MODULE_OUT_OF_MEMORY;
        return result;
    }
    for (size_t i = 0; i < module->count; i++) {
        const QttModuleDefinition *definition =
            &module->definitions[i];
        if (definition->external) continue;
        QttSemanticFunction *semantic =
            qtt_semantic_ir_lower_in_env(
                definition->core, module->signatures,
                module->module_id, &result.semantic_error);
        if (!semantic) {
            result.error =
                QTT_MODULE_SEMANTIC_VERIFICATION_FAILED;
            result.definition = i;
            return result;
        }
        result.semantic_validation =
            qtt_semantic_ir_verify(
                semantic, definition->core);
        if (result.semantic_validation !=
            QTT_SEMANTIC_IR_VALID) {
            qtt_semantic_ir_free(semantic);
            result.error =
                QTT_MODULE_SEMANTIC_VERIFICATION_FAILED;
            result.definition = i;
            return result;
        }
        QttAnfProgram *program = qtt_anf_lower_function_in_env(
            definition->core, definition->signature,
            module->signatures, module->module_id,
            &result.lower_error);
        if (!program) {
            qtt_semantic_ir_free(semantic);
            result.error = QTT_MODULE_LOWERING_FAILED;
            result.definition = i;
            return result;
        }
        QttAnfVerification verification = qtt_anf_verify(program);
        result.correspondence_error =
            verification.error == QTT_ANF_VALID
                ? qtt_semantic_anf_verify_calls(
                      semantic, definition->core, program)
                : QTT_SEMANTIC_ANF_OWNERSHIP_MISMATCH;
        qtt_semantic_ir_free(semantic);
        qtt_anf_program_free(program);
        if (verification.error != QTT_ANF_VALID) {
            result.error = QTT_MODULE_VERIFICATION_FAILED;
            result.definition = i;
            result.anf_error = verification.error;
            return result;
        }
        if (result.correspondence_error !=
            QTT_SEMANTIC_ANF_OK) {
            result.error = QTT_MODULE_CORRESPONDENCE_FAILED;
            result.definition = i;
            return result;
        }
        result.verified_count++;
    }
    result.error = QTT_MODULE_OK;
    return result;
}

size_t qtt_module_count(const QttModule *module) {
    return module ? module->count : 0;
}

const QttSignatureEnv *qtt_module_signatures(const QttModule *module) {
    return module ? module->signatures : NULL;
}

void qtt_module_free(QttModule *module) {
    if (!module) return;
    qtt_signature_env_free(module->signatures);
    for (size_t i = 0; i < module->count; i++)
        qtt_signature_free(module->definitions[i].signature);
    free(module->definitions);
    qtt_type_arena_free(module->types);
    free(module);
}
