#include "signature.h"

#include <stdlib.h>
#include <string.h>

static uint64_t contract_mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    hash *= UINT64_C(1099511628211);
    return hash;
}

uint64_t qtt_signature_contract_fingerprint(
    const QttFunctionSignature *signature) {
    if (!signature) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = contract_mix(hash, signature->parameter_count);
    for (size_t i = 0; i < signature->parameter_count; i++) {
        const QttParameterContract *parameter =
            &signature->parameters[i];
        hash = contract_mix(hash, parameter->type_id.value);
        hash = contract_mix(hash, parameter->representation);
        hash = contract_mix(hash, parameter->mode);
        hash = contract_mix(hash, parameter->nominal_authority.domain);
        hash = contract_mix(hash, parameter->nominal_authority.identity);
        hash = contract_mix(hash, parameter->quantity.is_omega);
        hash = contract_mix(hash, parameter->quantity.finite);
        hash = contract_mix(hash, parameter->observed.is_omega);
        hash = contract_mix(hash, parameter->observed.finite);
    }
    hash = contract_mix(hash, signature->result.type_id.value);
    hash = contract_mix(hash, signature->result.representation);
    hash = contract_mix(hash, signature->result.mode);
    hash = contract_mix(hash, signature->result.origin);
    hash = contract_mix(hash, signature->result.nominal_authority.domain);
    hash = contract_mix(hash, signature->result.nominal_authority.identity);
    hash = contract_mix(hash, signature->effects_complete);
    hash = contract_mix(hash, signature->effects_environment_solved);
    hash = contract_mix(hash, signature->effect_fingerprint);
    return hash ? hash : 1;
}

/* Closed latent-effect interpretation owned by a function contract.  This is
 * deliberately independent of Semantic IR construction: validation derives
 * the judgment again from typed Core.  Unknown higher-order/global calls make
 * the summary incomplete rather than silently certifying purity. */
static QttEffectRow *derive_latent_effects(
    QttEffectArena *arena, QttEffectSolver *solver,
    const QttCoreNode *node, bool *complete, uint64_t module_id,
    QttSignatureEffectLookup lookup, void *lookup_context) {
    if (!arena || !solver || !node || !complete) return NULL;
    QttEffectRow *summary = qtt_effect_empty(arena);
    if (!summary) return NULL;
    if (node->kind == QTT_CORE_WRITE)
        summary = qtt_effect_write_place(
            arena, node->write.place, summary);
    else if (node->kind == QTT_CORE_PLACE)
        summary = qtt_effect_read_place(arena, node->place, summary);
    if (!summary || node->kind == QTT_CORE_LAMBDA) return summary;

    const QttCoreNode *children[3] = {0};
    size_t count = 0;
    switch (node->kind) {
    case QTT_CORE_APPLY:
        children[count++] = node->apply.callee;
        break;
    case QTT_CORE_LET:
        children[count++] = node->let.value;
        children[count++] = node->let.body;
        break;
    case QTT_CORE_IF:
        children[count++] = node->conditional.condition;
        children[count++] = node->conditional.then_branch;
        children[count++] = node->conditional.else_branch;
        break;
    case QTT_CORE_WRITE:
        children[count++] = node->write.value;
        break;
    case QTT_CORE_BORROW:
        children[count++] = node->borrow.body;
        break;
    default:
        break;
    }
    for (size_t i = 0; i < count; i++) {
        QttEffectRow *child = derive_latent_effects(
            arena, solver, children[i], complete, module_id,
            lookup, lookup_context);
        if (!child) return NULL;
        summary = qtt_effect_join_closed(arena, solver, summary, child);
        if (!summary) return NULL;
    }
    if (node->kind == QTT_CORE_SEQUENCE)
        for (size_t i = 0; i < node->sequence.count; i++) {
            QttEffectRow *child = derive_latent_effects(
                arena, solver, node->sequence.items[i], complete,
                module_id, lookup, lookup_context);
            if (!child) return NULL;
            summary = qtt_effect_join_closed(arena, solver, summary, child);
            if (!summary) return NULL;
        }
    if (node->kind == QTT_CORE_APPLY) {
        for (size_t i = 0; i < node->apply.argument_count; i++) {
            QttEffectRow *argument = derive_latent_effects(
                arena, solver, node->apply.arguments[i], complete,
                module_id, lookup, lookup_context);
            if (!argument) return NULL;
            summary = qtt_effect_join_closed(
                arena, solver, summary, argument);
            if (!summary) return NULL;
        }
        if (node->apply.callee &&
            node->apply.callee->kind == QTT_CORE_LAMBDA) {
            QttEffectRow *latent = derive_latent_effects(
                arena, solver, node->apply.callee->lambda.body, complete,
                module_id, lookup, lookup_context);
            if (!latent) return NULL;
            summary = qtt_effect_join_closed(
                arena, solver, summary, latent);
        } else if (node->apply.callee &&
                   node->apply.callee->kind == QTT_CORE_GLOBAL && lookup) {
            const QttFunctionSignature *callee = lookup(
                lookup_context, module_id,
                node->apply.callee->global.name);
            if (!callee || !callee->latent_effects ||
                !callee->effect_solver) {
                *complete = false;
            } else {
                QttEffectRow *latent = qtt_effect_clone_closed(
                    arena, callee->effect_solver, callee->latent_effects);
                if (!latent) return NULL;
                summary = qtt_effect_join_closed(
                    arena, solver, summary, latent);
                if (!summary) return NULL;
                if (!callee->effects_complete) *complete = false;
            }
        } else {
            *complete = false;
        }
    }
    return summary;
}

static const Type *find_var_type(const QttCoreNode *core, QttCoreVar var) {
    if (!core) return NULL;
    switch (core->kind) {
    case QTT_CORE_VAR:
        return qtt_core_var_equal(core->var, var) ? core->type : NULL;
    case QTT_CORE_PLACE:
        return qtt_core_var_equal(core->place.root, var)
            ? core->type : NULL;
    case QTT_CORE_LET: {
        const Type *type = find_var_type(core->let.value, var);
        return type ? type : find_var_type(core->let.body, var);
    }
    case QTT_CORE_IF: {
        const Type *type =
            find_var_type(core->conditional.condition, var);
        if (!type) type =
            find_var_type(core->conditional.then_branch, var);
        return type ? type :
            find_var_type(core->conditional.else_branch, var);
    }
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < core->sequence.count; i++) {
            const Type *type = find_var_type(core->sequence.items[i], var);
            if (type) return type;
        }
        return NULL;
    case QTT_CORE_APPLY: {
        const Type *type = find_var_type(core->apply.callee, var);
        for (size_t i = 0; !type && i < core->apply.argument_count; i++)
            type = find_var_type(core->apply.arguments[i], var);
        return type;
    }
    case QTT_CORE_WRITE:
        return qtt_core_var_equal(core->write.place.root, var)
            ? core->write.value->type
            : find_var_type(core->write.value, var);
    default:
        return NULL;
    }
}

static bool tail_escapes_var(const QttCoreNode *core, QttCoreVar var) {
    if (!core) return false;
    if (core->kind == QTT_CORE_VAR)
        return qtt_core_var_equal(core->var, var);
    if (core->kind == QTT_CORE_LET)
        return tail_escapes_var(core->let.body, var);
    if (core->kind == QTT_CORE_SEQUENCE && core->sequence.count)
        return tail_escapes_var(
            core->sequence.items[core->sequence.count - 1], var);
    if (core->kind == QTT_CORE_IF)
        return tail_escapes_var(core->conditional.then_branch, var) ||
               tail_escapes_var(core->conditional.else_branch, var);
    if (core->kind == QTT_CORE_APPLY && core->apply.callee &&
        core->apply.callee->kind == QTT_CORE_LAMBDA &&
        core->apply.callee->lambda.param_count ==
            core->apply.argument_count) {
        const QttCoreNode *callee = core->apply.callee;
        for (size_t i = 0; i < core->apply.argument_count; i++)
            if (tail_escapes_var(
                    callee->lambda.body, callee->lambda.params[i]) &&
                tail_escapes_var(core->apply.arguments[i], var))
                return true;
    }
    return false;
}

bool qtt_signature_fresh_result_primitive(const char *name) {
    if (!name) return false;
    return strcmp(name, "concat") == 0 ||
           strcmp(name, "__rt_concat") == 0 ||
           strcmp(name, "__rt_string_take") == 0 ||
           strcmp(name, "__rt_string_drop") == 0 ||
           strcmp(name, "list-copy") == 0;
}

static QttResultOrigin result_origin(
    const QttCoreNode *core,
    const QttCoreVar *parameters, size_t parameter_count) {
    if (!core) return QTT_RESULT_ORIGIN_UNKNOWN;
    if (qtt_resource_classify_type(core->type) ==
        QTT_REP_IMMEDIATE)
        return QTT_RESULT_ORIGIN_IMMEDIATE;
    switch (core->kind) {
    case QTT_CORE_LITERAL:
        if (core->type && core->type->kind == TYPE_LAYOUT &&
            core->literal.source &&
            core->literal.source->type == AST_LIST)
            return QTT_RESULT_ORIGIN_FRESH;
        return QTT_RESULT_ORIGIN_STATIC;
    case QTT_CORE_QUOTE:
    case QTT_CORE_GLOBAL:
        return QTT_RESULT_ORIGIN_STATIC;
    case QTT_CORE_VAR:
        for (size_t i = 0; i < parameter_count; i++)
            if (qtt_core_var_equal(core->var, parameters[i]))
                return QTT_RESULT_ORIGIN_TRANSFERRED;
        return QTT_RESULT_ORIGIN_UNKNOWN;
    case QTT_CORE_PLACE:
        for (size_t i = 0; i < parameter_count; i++)
            if (qtt_core_var_equal(core->place.root, parameters[i]))
                return QTT_RESULT_ORIGIN_TRANSFERRED;
        return QTT_RESULT_ORIGIN_UNKNOWN;
    case QTT_CORE_LAMBDA:
        return QTT_RESULT_ORIGIN_FRESH;
    case QTT_CORE_SEQUENCE:
        return core->sequence.count
            ? result_origin(
                  core->sequence.items[core->sequence.count - 1],
                  parameters, parameter_count)
            : QTT_RESULT_ORIGIN_UNKNOWN;
    case QTT_CORE_BORROW:
        return result_origin(
            core->borrow.body, parameters, parameter_count);
    case QTT_CORE_PERFORM:
        /* An operation result is supplied by dynamically selected authority. */
        return QTT_RESULT_ORIGIN_UNKNOWN;
    case QTT_CORE_HANDLE:
        /* A handler may select either the computation or clause result. */
        return QTT_RESULT_ORIGIN_UNKNOWN;
    case QTT_CORE_IF: {
        QttResultOrigin then_origin = result_origin(
            core->conditional.then_branch,
            parameters, parameter_count);
        QttResultOrigin else_origin = result_origin(
            core->conditional.else_branch,
            parameters, parameter_count);
        return then_origin == else_origin
            ? then_origin : QTT_RESULT_ORIGIN_UNKNOWN;
    }
    case QTT_CORE_LET:
        if (core->let.body &&
            core->let.body->kind == QTT_CORE_VAR &&
            qtt_core_var_equal(
                core->let.body->var, core->let.binding))
            return result_origin(
                core->let.value, parameters, parameter_count);
        if (core->let.body &&
            core->let.body->kind == QTT_CORE_PLACE &&
            qtt_core_var_equal(
                core->let.body->place.root, core->let.binding))
            return result_origin(
                core->let.value, parameters, parameter_count);
        return result_origin(
            core->let.body, parameters, parameter_count);
    case QTT_CORE_APPLY:
        return core->apply.callee &&
               core->apply.callee->kind == QTT_CORE_GLOBAL &&
               qtt_signature_fresh_result_primitive(
                   core->apply.callee->global.name)
            ? QTT_RESULT_ORIGIN_FRESH
            : QTT_RESULT_ORIGIN_UNKNOWN;
    case QTT_CORE_WRITE:
        return QTT_RESULT_ORIGIN_UNKNOWN;
    }
    return QTT_RESULT_ORIGIN_UNKNOWN;
}

QttFunctionSignature *qtt_signature_derive(
    const QttCoreNode *lambda,
    QttSignatureError *error) {
    if (!lambda || lambda->kind != QTT_CORE_LAMBDA ||
        !lambda->lambda.body) {
        if (error) *error = QTT_SIGNATURE_NOT_A_FUNCTION;
        return NULL;
    }
    QttFunctionSignature *signature = calloc(1, sizeof(*signature));
    if (!signature) {
        if (error) *error = QTT_SIGNATURE_OUT_OF_MEMORY;
        return NULL;
    }
    signature->source = lambda;
    signature->effect_arena = qtt_effect_arena_new();
    signature->effect_solver = signature->effect_arena
        ? qtt_effect_solver_new(signature->effect_arena) : NULL;
    signature->effects_complete = true;
    signature->latent_effects = signature->effect_solver
        ? derive_latent_effects(
              signature->effect_arena, signature->effect_solver,
              lambda->lambda.body, &signature->effects_complete,
              0, NULL, NULL)
        : NULL;
    if (!signature->latent_effects) {
        qtt_signature_free(signature);
        if (error) *error = QTT_SIGNATURE_OUT_OF_MEMORY;
        return NULL;
    }
    signature->effect_fingerprint = qtt_effect_row_fingerprint(
        signature->effect_solver, signature->latent_effects);
    signature->parameter_count = lambda->lambda.param_count;
    signature->result_type = lambda->lambda.body->type;
    signature->result.type = signature->result_type;
    signature->result.representation =
        qtt_resource_classify_type(signature->result_type);
    signature->result.origin = result_origin(
        lambda->lambda.body, lambda->lambda.params,
        lambda->lambda.param_count);
    if (signature->result.representation == QTT_REP_IMMEDIATE) {
        signature->result.mode = QTT_RESULT_IMMEDIATE;
        signature->result.origin = QTT_RESULT_ORIGIN_IMMEDIATE;
    } else if (signature->result.origin ==
               QTT_RESULT_ORIGIN_FRESH ||
               signature->result.origin ==
               QTT_RESULT_ORIGIN_TRANSFERRED) {
        signature->result.mode = QTT_RESULT_OWNED;
    } else if (signature->result.origin ==
               QTT_RESULT_ORIGIN_STATIC) {
        signature->result.mode = QTT_RESULT_BORROWED;
    } else {
        signature->result.mode = QTT_RESULT_UNKNOWN;
    }
    if (signature->parameter_count)
        signature->parameters = calloc(
            signature->parameter_count, sizeof(*signature->parameters));
    QttDemandError demand_error = QTT_DEMAND_OK;
    signature->demand =
        qtt_demand_derive(lambda->lambda.body, &demand_error);
    if ((signature->parameter_count && !signature->parameters) ||
        !signature->demand) {
        qtt_signature_free(signature);
        if (error) *error = QTT_SIGNATURE_OUT_OF_MEMORY;
        return NULL;
    }
    for (size_t i = 0; i < signature->parameter_count; i++) {
        QttCoreVar var = lambda->lambda.params[i];
        QttQuantity runtime = qtt_demand_runtime(
            signature->demand, lambda->lambda.body, var);
        size_t count = runtime.is_omega
            ? SIZE_MAX : (size_t)runtime.finite;
        QttParameterContract *parameter = &signature->parameters[i];
        parameter->var = var;
        parameter->quantity = runtime;
        parameter->observed = runtime;
        parameter->type = lambda->lambda.param_types
            ? lambda->lambda.param_types[i]
            : find_var_type(lambda->lambda.body, var);
        parameter->representation =
            qtt_resource_classify_type(parameter->type);
        if (!count)
            parameter->mode = QTT_OWNERSHIP_ERASED;
        else if (tail_escapes_var(lambda->lambda.body, var))
            parameter->mode = QTT_OWNERSHIP_CONSUMED;
        else
            parameter->mode = count > 1
                ? QTT_OWNERSHIP_SHARED : QTT_OWNERSHIP_BORROWED;
    }
    if (error) *error = QTT_SIGNATURE_OK;
    return signature;
}

QttSignatureValidation qtt_signature_validate_in_effect_env(
    const QttFunctionSignature *signature,
    const QttCoreNode *lambda, uint64_t module_id,
    QttSignatureEffectLookup lookup, void *context) {
    if (!signature || signature->source != lambda ||
        !lambda || lambda->kind != QTT_CORE_LAMBDA ||
        qtt_demand_validate(signature->demand, lambda->lambda.body) !=
            QTT_DEMAND_VALID)
        return QTT_SIGNATURE_SOURCE_MISMATCH;
    if (signature->effect_arena || signature->effect_solver ||
        signature->latent_effects || signature->effects_complete) {
        QttEffectArena *arena = qtt_effect_arena_new();
        QttEffectSolver *solver = arena
            ? qtt_effect_solver_new(arena) : NULL;
        bool complete = true;
        QttEffectRow *derived = solver
            ? derive_latent_effects(
                  arena, solver, lambda->lambda.body, &complete,
                  module_id, lookup, context)
            : NULL;
        char *expected_text = derived
            ? qtt_effect_format(solver, derived) : NULL;
        char *actual_text = signature->latent_effects &&
                signature->effect_solver
            ? qtt_effect_format(
                  signature->effect_solver, signature->latent_effects)
            : NULL;
        bool valid = derived && expected_text && actual_text &&
            complete == signature->effects_complete &&
            signature->latent_effects && signature->effect_solver &&
            qtt_effect_row_fingerprint(solver, derived) ==
                signature->effect_fingerprint &&
            strcmp(expected_text, actual_text) == 0 &&
            qtt_effect_row_fingerprint(
                signature->effect_solver, signature->latent_effects) ==
                signature->effect_fingerprint;
        free(expected_text);
        free(actual_text);
        qtt_effect_solver_free(solver);
        qtt_effect_arena_free(arena);
        if (!valid) return QTT_SIGNATURE_EFFECT_MISMATCH;
    }
    return QTT_SIGNATURE_VALID;
}

QttSignatureValidation qtt_signature_validate(
    const QttFunctionSignature *signature,
    const QttCoreNode *lambda) {
    return qtt_signature_validate_in_effect_env(
        signature, lambda, 0, NULL, NULL);
}

QttSignatureCanonicalization qtt_signature_canonicalize(
    QttFunctionSignature *signature,
    QttTypeArena *arena) {
    if (!signature || !arena)
        return QTT_SIGNATURE_MISSING_TYPE;
    if (!signature->result.type)
        signature->result.type = signature->result_type;
    if (!signature->result.type)
        return QTT_SIGNATURE_MISSING_TYPE;
    QttTypeIdentityError error = QTT_TYPE_IDENTITY_OK;
    signature->result.type_id =
        qtt_type_intern(arena, signature->result.type, &error);
    if (!signature->result.type_id.value)
        return error == QTT_TYPE_IDENTITY_OUT_OF_MEMORY
            ? QTT_SIGNATURE_CANONICAL_OUT_OF_MEMORY
            : QTT_SIGNATURE_MISSING_TYPE;
    for (size_t i = 0; i < signature->parameter_count; i++) {
        if (!signature->parameters[i].type)
            return QTT_SIGNATURE_MISSING_TYPE;
        signature->parameters[i].type_id = qtt_type_intern(
            arena, signature->parameters[i].type, &error);
        if (!signature->parameters[i].type_id.value)
            return error == QTT_TYPE_IDENTITY_OUT_OF_MEMORY
                ? QTT_SIGNATURE_CANONICAL_OUT_OF_MEMORY
                : QTT_SIGNATURE_MISSING_TYPE;
    }
    signature->contract_fingerprint =
        qtt_signature_contract_fingerprint(signature);
    if (!signature->source ||
        signature->source->kind != QTT_CORE_LAMBDA)
        return QTT_SIGNATURE_CANONICAL;
    QttGradedBinder *binders = signature->parameter_count
        ? calloc(signature->parameter_count, sizeof(*binders)) : NULL;
    if (signature->parameter_count && !binders)
        return QTT_SIGNATURE_CANONICAL_OUT_OF_MEMORY;
    for (size_t i = 0; i < signature->parameter_count; i++)
        binders[i] = (QttGradedBinder){
            .var = signature->parameters[i].var,
            .type_id = signature->parameters[i].type_id,
            .allowance = signature->parameters[i].quantity,
        };
    QttGradedContext context = {
        .binders = binders,
        .count = signature->parameter_count,
    };
    QttGradedError graded_error = QTT_GRADED_OK;
    qtt_graded_certificate_free(signature->graded);
    signature->graded = qtt_graded_check(
        signature->source->lambda.body, signature->result.type_id,
        &context, arena, &graded_error);
    free(binders);
    if (!signature->graded)
        return graded_error == QTT_GRADED_OUT_OF_MEMORY
            ? QTT_SIGNATURE_CANONICAL_OUT_OF_MEMORY
            : QTT_SIGNATURE_GRADED_INVALID;
    for (size_t i = 0; i < signature->parameter_count; i++)
        if (!qtt_quantity_equal(
                signature->parameters[i].observed,
                qtt_graded_observed(
                    signature->graded,
                    signature->parameters[i].var)))
            return QTT_SIGNATURE_GRADED_INVALID;
    return QTT_SIGNATURE_CANONICAL;
}

QttGradedProofValidation qtt_signature_validate_grades(
    const QttFunctionSignature *signature, QttTypeArena *arena) {
    if (!signature || !signature->source ||
        signature->source->kind != QTT_CORE_LAMBDA)
        return QTT_GRADED_PROOF_SOURCE_MISMATCH;
    QttGradedBinder *binders = signature->parameter_count
        ? calloc(signature->parameter_count, sizeof(*binders)) : NULL;
    if (signature->parameter_count && !binders)
        return QTT_GRADED_PROOF_CONTEXT_MISMATCH;
    for (size_t i = 0; i < signature->parameter_count; i++)
        binders[i] = (QttGradedBinder){
            .var = signature->parameters[i].var,
            .type_id = signature->parameters[i].type_id,
            .allowance = signature->parameters[i].quantity,
        };
    QttGradedContext context = {
        .binders = binders,
        .count = signature->parameter_count,
    };
    QttGradedProofValidation validation = qtt_graded_validate(
        signature->graded, signature->source->lambda.body,
        signature->result.type_id, &context, arena);
    free(binders);
    if (validation != QTT_GRADED_PROOF_VALID)
        return validation;
    for (size_t i = 0; i < signature->parameter_count; i++)
        if (!qtt_quantity_equal(
                signature->parameters[i].observed,
                qtt_graded_observed(
                    signature->graded,
                    signature->parameters[i].var)))
            return QTT_GRADED_PROOF_DEMAND_MISMATCH;
    return validation;
}

QttSignatureEffectRefresh qtt_signature_refresh_effects(
    QttFunctionSignature *signature, uint64_t module_id,
    QttSignatureEffectLookup lookup, void *context) {
    if (!signature || !signature->source ||
        signature->source->kind != QTT_CORE_LAMBDA || !lookup)
        return QTT_SIGNATURE_EFFECT_REFRESH_UNCHANGED;
    QttEffectArena *arena = qtt_effect_arena_new();
    QttEffectSolver *solver = arena
        ? qtt_effect_solver_new(arena) : NULL;
    bool complete = true;
    QttEffectRow *row = solver
        ? derive_latent_effects(
              arena, solver, signature->source->lambda.body,
              &complete, module_id, lookup, context)
        : NULL;
    if (!row) {
        qtt_effect_solver_free(solver);
        qtt_effect_arena_free(arena);
        return QTT_SIGNATURE_EFFECT_REFRESH_OUT_OF_MEMORY;
    }
    uint64_t fingerprint = qtt_effect_row_fingerprint(solver, row);
    char *next_text = qtt_effect_format(solver, row);
    char *old_text = signature->latent_effects && signature->effect_solver
        ? qtt_effect_format(
              signature->effect_solver, signature->latent_effects)
        : NULL;
    if (!next_text || !old_text) {
        free(next_text);
        free(old_text);
        qtt_effect_solver_free(solver);
        qtt_effect_arena_free(arena);
        return QTT_SIGNATURE_EFFECT_REFRESH_OUT_OF_MEMORY;
    }
    bool changed = !signature->effects_environment_solved ||
        complete != signature->effects_complete ||
        fingerprint != signature->effect_fingerprint ||
        strcmp(next_text, old_text) != 0;
    free(next_text);
    free(old_text);
    if (!changed) {
        qtt_effect_solver_free(solver);
        qtt_effect_arena_free(arena);
        return QTT_SIGNATURE_EFFECT_REFRESH_UNCHANGED;
    }
    qtt_effect_solver_free(signature->effect_solver);
    qtt_effect_arena_free(signature->effect_arena);
    signature->effect_arena = arena;
    signature->effect_solver = solver;
    signature->latent_effects = row;
    signature->effects_complete = complete;
    signature->effects_environment_solved = true;
    signature->effect_fingerprint = fingerprint;
    signature->contract_fingerprint =
        qtt_signature_contract_fingerprint(signature);
    return QTT_SIGNATURE_EFFECT_REFRESH_CHANGED;
}

void qtt_signature_free(QttFunctionSignature *signature) {
    if (!signature) return;
    qtt_demand_certificate_free(signature->demand);
    qtt_graded_certificate_free(signature->graded);
    qtt_effect_solver_free(signature->effect_solver);
    qtt_effect_arena_free(signature->effect_arena);
    free(signature->parameters);
    free(signature);
}
