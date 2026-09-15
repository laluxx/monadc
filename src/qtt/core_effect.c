#include "core_effect.h"
#include "../infer.h"

#include <stdlib.h>
#include <string.h>

extern Type *type_from_name(const char *name) __attribute__((weak));
extern bool types_equal(Type *left, Type *right) __attribute__((weak));
extern bool type_is_subtype(Type *sub, Type *sup) __attribute__((weak));
extern bool infer_operation_scheme_accepts(
    const char *portable_scheme, Type *payload_type,
    Type *result_type) __attribute__((weak));
extern TypeScheme *infer_type_scheme_deserialize(
    const char *text) __attribute__((weak));
extern void scheme_free(TypeScheme *scheme) __attribute__((weak));
extern bool infer_callable_contract_deserialize(
    InferCallableContract *contract,
    const char *text) __attribute__((weak));
extern uint64_t infer_callable_contract_fingerprint(
    const InferCallableContract *contract) __attribute__((weak));
extern void infer_callable_contract_free(
    InferCallableContract *contract) __attribute__((weak));

static bool qtt_core_global_authority_valid(const QttCoreNode *global) {
    if (!global || global->kind != QTT_CORE_GLOBAL ||
        !global->global.callable_contract) return true;
    if (!global->global.callable_contract_fingerprint ||
        !infer_callable_contract_deserialize ||
        !infer_callable_contract_fingerprint ||
        !infer_callable_contract_free) return false;
    InferCallableContract contract = {0};
    bool valid = infer_callable_contract_deserialize(
            &contract, global->global.callable_contract) &&
        infer_callable_contract_fingerprint(&contract) ==
            global->global.callable_contract_fingerprint;
    infer_callable_contract_free(&contract);
    return valid;
}

static bool qtt_core_global_has_consumed_effect_predicate(
    const QttCoreNode *global, size_t applied_count) {
    if (!global || global->kind != QTT_CORE_GLOBAL ||
        !global->global.callable_contract ||
        !infer_callable_contract_deserialize ||
        !infer_callable_contract_free) return false;
    InferCallableContract contract = {0};
    if (!infer_callable_contract_deserialize(
            &contract, global->global.callable_contract))
        return true;
    bool consumed = false;
    for (size_t i = 0;
         i < contract.effect_trait_predicate_count; i++)
        if (contract.effect_trait_predicate_stages[i] < applied_count) {
            consumed = true;
            break;
        }
    infer_callable_contract_free(&contract);
    return consumed;
}

static bool type_admits(const Type *actual, Type *expected) {
    return actual && expected &&
        (types_equal((Type *)actual, expected) ||
         (type_is_subtype && type_is_subtype((Type *)actual, expected)));
}

typedef struct {
    QttCoreEffectResult *result;
    QttQuantity capture_allowance;
} Elaboration;

static QttCoreResumptionClass classify_resumption(QttQuantity quantity) {
    if (quantity.is_omega) return QTT_CORE_RESUMPTION_UNRESTRICTED;
    if (quantity.finite == 0) return QTT_CORE_RESUMPTION_ABORTIVE;
    if (quantity.finite == 1) return QTT_CORE_RESUMPTION_ONE_SHOT;
    return QTT_CORE_RESUMPTION_MULTI_SHOT;
}

static QttEffectAtom declared_atom(const QttEffectDeclaration *declaration,
                                   uint64_t capability_id) {
    return (QttEffectAtom){
        .traits = declaration->traits,
        .kind = declaration->kind,
        .constructor_id = declaration->constructor_id,
        .capability_id = capability_id,
        .name = declaration->name,
        .operation = declaration->operation,
        .resumption = declaration->resumption,
        .scoped = declaration->scoped,
    };
}

static QttEffectRow *elaborate_node(Elaboration *elaboration,
                                    QttCoreNode *node);

static QttEffectRow *empty(Elaboration *elaboration) {
    return qtt_effect_empty(elaboration->result->arena);
}

static QttEffectRow *join(Elaboration *elaboration,
                          QttEffectRow *left, QttEffectRow *right) {
    if (!left || !right) {
        if (elaboration->result->status == QTT_CORE_EFFECT_OK)
            elaboration->result->status = QTT_CORE_EFFECT_OUT_OF_MEMORY;
        return NULL;
    }
    QttEffectRow *row = qtt_effect_join(
        elaboration->result->arena, elaboration->result->solver,
        left, right);
    if (!row) elaboration->result->status = QTT_CORE_EFFECT_OUT_OF_MEMORY;
    return row;
}

static QttEffectRow *elaborate_many(Elaboration *elaboration,
                                    QttCoreNode *const *nodes,
                                    size_t count) {
    QttEffectRow *effects = empty(elaboration);
    for (size_t i = 0; effects && i < count; i++)
        effects = join(elaboration, effects,
                       elaborate_node(elaboration, nodes[i]));
    return effects;
}

static QttEffectRow *elaborate_apply_latent(
    Elaboration *elaboration, const QttCoreNode *node) {
    QttEffectRow *effects = empty(elaboration);
    if (node && node->apply.callee &&
        !qtt_core_global_authority_valid(node->apply.callee)) {
        elaboration->result->status = QTT_CORE_EFFECT_INVALID_CALL_EFFECT;
        return NULL;
    }
    TypeScheme *authority = node && node->apply.callee &&
        node->apply.callee->kind == QTT_CORE_GLOBAL &&
        node->apply.callee->global.hm_scheme &&
        infer_type_scheme_deserialize && scheme_free
        ? infer_type_scheme_deserialize(
            node->apply.callee->global.hm_scheme) : NULL;
    const Type *stage = authority ? authority->type
        : node && node->apply.callee ? node->apply.callee->type : NULL;
    for (size_t i = 0; effects && i < node->apply.argument_count; i++) {
        if (!stage || stage->kind != TYPE_ARROW) break;
        if (stage->arrow_effect_scheme) {
            QttEffectScheme *scheme = qtt_effect_scheme_deserialize(
                stage->arrow_effect_scheme);
            QttEffectRow *latent = NULL;
            if (scheme)
                latent = stage->arrow_effect_complete
                    ? qtt_effect_instantiate(
                        elaboration->result->arena, scheme)
                    : qtt_effect_instantiate_with_tail(
                        elaboration->result->arena, scheme,
                        qtt_effect_fresh(elaboration->result->arena));
            qtt_effect_scheme_free(scheme);
            if (!latent) {
                elaboration->result->status =
                    QTT_CORE_EFFECT_INVALID_CALL_EFFECT;
                if (authority) scheme_free(authority);
                return NULL;
            }
            effects = join(elaboration, effects, latent);
            if (effects && node->apply.callee->kind == QTT_CORE_GLOBAL)
                for (size_t p = 0;
                     p < node->apply.callee->global.effect_predicate_count;
                     p++)
                    if (node->apply.callee->global
                            .effect_predicate_stages[p] == i &&
                        !qtt_effect_constrain_has_trait(
                            elaboration->result->constraints, latent,
                            node->apply.callee->global
                                .effect_predicate_names[p])) {
                        elaboration->result->status =
                            QTT_CORE_EFFECT_OUT_OF_MEMORY;
                        if (authority) scheme_free(authority);
                        return NULL;
                    }
        }
        stage = stage->arrow_ret;
    }
    if (authority) scheme_free(authority);
    return effects;
}

static bool qtt_core_apply_has_latent_effect(const QttCoreNode *node) {
    if (node && node->kind == QTT_CORE_APPLY && node->apply.callee &&
        !qtt_core_global_authority_valid(node->apply.callee)) return true;
    if (node && node->kind == QTT_CORE_APPLY && node->apply.callee &&
        node->apply.callee->kind == QTT_CORE_GLOBAL)
        for (size_t i = 0;
             i < node->apply.callee->global.effect_predicate_count; i++)
            if (node->apply.callee->global.effect_predicate_stages[i] <
                node->apply.argument_count) return true;
    if (node && node->kind == QTT_CORE_APPLY &&
        qtt_core_global_has_consumed_effect_predicate(
            node->apply.callee, node->apply.argument_count)) return true;
    TypeScheme *authority = node && node->kind == QTT_CORE_APPLY &&
        node->apply.callee && node->apply.callee->kind == QTT_CORE_GLOBAL &&
        node->apply.callee->global.hm_scheme &&
        infer_type_scheme_deserialize && scheme_free
        ? infer_type_scheme_deserialize(
            node->apply.callee->global.hm_scheme) : NULL;
    const Type *stage = authority ? authority->type
        : node && node->kind == QTT_CORE_APPLY && node->apply.callee
            ? node->apply.callee->type : NULL;
    bool effectful = false;
    for (size_t i = 0; stage && i < node->apply.argument_count; i++) {
        if (stage->kind != TYPE_ARROW) break;
        if (stage->arrow_effect_scheme) {
            QttEffectScheme *scheme = qtt_effect_scheme_deserialize(
                stage->arrow_effect_scheme);
            effectful = !scheme || !stage->arrow_effect_complete ||
                !qtt_effect_scheme_is_empty(scheme);
            qtt_effect_scheme_free(scheme);
            if (effectful) break;
        }
        stage = stage->arrow_ret;
    }
    if (authority) scheme_free(authority);
    return effectful;
}

static QttEffectRow *elaborate_perform(Elaboration *elaboration,
                                       QttCoreNode *node) {
    bool ambiguous = false;
    const QttEffectDeclaration *declaration =
        qtt_effect_declaration_resolve(node->perform.effect_name, &ambiguous);
    if (!declaration || ambiguous) {
        elaboration->result->status = QTT_CORE_EFFECT_UNKNOWN_EFFECT;
        return NULL;
    }
    if (declaration->operation_scheme &&
        infer_operation_scheme_accepts && node->perform.argument->type &&
        node->type && !infer_operation_scheme_accepts(
            declaration->operation_scheme,
            (Type *)node->perform.argument->type, (Type *)node->type)) {
        elaboration->result->status =
            QTT_CORE_EFFECT_OPERATION_TYPE_MISMATCH;
        return NULL;
    }
    if (!declaration->operation_scheme && declaration->payload_type &&
        declaration->result_type &&
        type_from_name && types_equal) {
        Type *payload = type_from_name(declaration->payload_type);
        Type *result = type_from_name(declaration->result_type);
        if (!payload || !result ||
            (node->perform.argument->type &&
             !type_admits(node->perform.argument->type, payload)) ||
            (node->type && !type_admits(node->type, result))) {
            elaboration->result->status =
                QTT_CORE_EFFECT_OPERATION_TYPE_MISMATCH;
            return NULL;
        }
    }
    QttEffectRow *argument = elaborate_node(
        elaboration, node->perform.argument);
    if (!argument) return NULL;
    QttEffectAtom atom = declared_atom(
        declaration, node->perform.capability_id);
    elaboration->result->handled_atom = atom;
    node->perform.effect_name = declaration->name;
    node->perform.constructor_id = declaration->constructor_id;
    node->perform.resumption = declaration->resumption;
    node->perform.scoped = declaration->scoped;
    QttEffectRow *row = qtt_effect_extend_atom(
        elaboration->result->arena, &atom, argument);
    if (!row) elaboration->result->status = QTT_CORE_EFFECT_OUT_OF_MEMORY;
    return row;
}

static void qtt_core_effect_select_operation(
    QttCoreNode *node, const char *effect_name,
    QttCoreNode **selected, size_t *matching,
    bool *unsupported_context, bool *effectful_context) {
    if (!node || !effect_name || !selected || !matching ||
        !unsupported_context || !effectful_context) return;
    if (node->kind == QTT_CORE_PERFORM) {
        if (node->perform.effect_name &&
            strcmp(node->perform.effect_name, effect_name) == 0) {
            if (!*matching) *selected = node;
            (*matching)++;
        } else *effectful_context = true;
        return;
    }
    /* Nested lambdas and handlers are effect-scope boundaries, not strict
     * children of this handler's computation. */
    if (node->kind == QTT_CORE_LAMBDA || node->kind == QTT_CORE_HANDLE)
        return;
    switch (node->kind) {
    case QTT_CORE_APPLY:
        if (qtt_core_apply_has_latent_effect(node))
            *effectful_context = true;
        qtt_core_effect_select_operation(
            node->apply.callee, effect_name, selected, matching,
            unsupported_context, effectful_context);
        for (size_t i = 0; i < node->apply.argument_count; i++)
            qtt_core_effect_select_operation(
                node->apply.arguments[i], effect_name, selected, matching,
                unsupported_context, effectful_context);
        break;
    case QTT_CORE_LET:
        qtt_core_effect_select_operation(
            node->let.value, effect_name, selected, matching,
            unsupported_context, effectful_context);
        qtt_core_effect_select_operation(
            node->let.body, effect_name, selected, matching,
            unsupported_context, effectful_context);
        break;
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < node->sequence.count; i++)
            qtt_core_effect_select_operation(
                node->sequence.items[i], effect_name, selected, matching,
                unsupported_context, effectful_context);
        break;
    case QTT_CORE_WRITE:
        *effectful_context = true;
        qtt_core_effect_select_operation(
            node->write.value, effect_name, selected, matching,
            unsupported_context, effectful_context);
        break;
    case QTT_CORE_BORROW:
        qtt_core_effect_select_operation(
            node->borrow.body, effect_name, selected, matching,
            unsupported_context, effectful_context);
        break;
    case QTT_CORE_IF: {
        qtt_core_effect_select_operation(
            node->conditional.condition, effect_name, selected, matching,
            unsupported_context, effectful_context);
        size_t before = *matching;
        qtt_core_effect_select_operation(
            node->conditional.then_branch, effect_name, selected, matching,
            unsupported_context, effectful_context);
        qtt_core_effect_select_operation(
            node->conditional.else_branch, effect_name, selected, matching,
            unsupported_context, effectful_context);
        if (*matching != before) *unsupported_context = true;
        break;
    }
    default:
        break;
    }
}

static QttEffectRow *elaborate_handle(Elaboration *elaboration,
                                      QttCoreNode *node) {
    /* Evidence is affine authority: every recheck consumes the previous
     * witness before attempting to derive a replacement. */
    free(node->handle.portable_proof);
    node->handle.portable_proof = NULL;
    node->handle.selected_operation = NULL;
    node->handle.abortive_context_safe = false;
    node->handle.deep = false;
    const QttEffectHandlerProfile *profile =
        qtt_effect_handler_profile_lookup(node->handle.profile_name);
    if (!profile) {
        elaboration->result->status = QTT_CORE_EFFECT_UNKNOWN_PROFILE;
        return NULL;
    }
    if (!profile->deep) {
        elaboration->result->status = QTT_CORE_EFFECT_UNSUPPORTED_SHALLOW;
        return NULL;
    }
    QttEffectRow *computation = elaborate_node(
        elaboration, node->handle.computation);
    QttEffectRow *clause_effects = computation
        ? elaborate_node(elaboration, node->handle.clause) : NULL;
    if (!computation || !clause_effects) return NULL;

    const QttEffectDeclaration *declaration =
        qtt_effect_declaration_lookup(profile->effect_name);
    if (!declaration) {
        elaboration->result->status = QTT_CORE_EFFECT_PROFILE_MISMATCH;
        return NULL;
    }
    QttCoreNode *operation = NULL;
    size_t matching = 0;
    bool unsupported_context = false;
    bool effectful_context = false;
    qtt_core_effect_select_operation(
        node->handle.computation, declaration->name,
        &operation, &matching, &unsupported_context, &effectful_context);
    if (!matching) {
        elaboration->result->status = QTT_CORE_EFFECT_OPERATION_ABSENT;
        return NULL;
    }
    if (matching > 1) {
        elaboration->result->status = QTT_CORE_EFFECT_MULTIPLE_OPERATIONS;
        return NULL;
    }
    if (unsupported_context) {
        elaboration->result->status = QTT_CORE_EFFECT_UNSUPPORTED_CONTEXT;
        return NULL;
    }
    node->handle.selected_operation = operation;
    node->handle.abortive_context_safe = !effectful_context;
    if (!declaration->operation_scheme && declaration->payload_type &&
        declaration->result_type &&
        type_from_name && types_equal) {
        Type *payload = type_from_name(declaration->payload_type);
        Type *result = type_from_name(declaration->result_type);
        Type *clause_type = (Type *)node->handle.clause->type;
        if (!payload || !result ||
            (clause_type &&
             (clause_type->kind != TYPE_ARROW ||
              !type_admits(payload, clause_type->arrow_param) ||
              !type_admits(clause_type->arrow_ret, result))) ||
            (node->type && !type_admits(node->type, result))) {
            elaboration->result->status =
                QTT_CORE_EFFECT_OPERATION_TYPE_MISMATCH;
            return NULL;
        }
    }
    QttEffectAtom atom = declared_atom(
        declaration, operation->perform.capability_id);
    QttEffectHandlerClause clause = {
        .atom = &atom,
        .continuation_usage = profile->continuation_usage,
        .capture_allowance = elaboration->capture_allowance,
        .clause_effects = clause_effects,
    };
    QttEffectHandlerSetResult handled = qtt_effect_elaborate_handler_set(
        elaboration->result->arena, elaboration->result->solver,
        computation, &clause, 1);
    if (handled.status != QTT_EFFECT_HANDLER_OK) {
        elaboration->result->status =
            handled.status == QTT_EFFECT_HANDLER_GRADE_VIOLATION
                ? QTT_CORE_EFFECT_GRADE_VIOLATION
                : handled.status == QTT_EFFECT_HANDLER_SCOPED_ESCAPE
                    ? QTT_CORE_EFFECT_SCOPED_ESCAPE
                    : handled.status == QTT_EFFECT_HANDLER_ABSENT
                        ? QTT_CORE_EFFECT_OPERATION_ABSENT
                        : QTT_CORE_EFFECT_INVALID_CORE;
        return NULL;
    }
    char *proof = qtt_effect_handler_proof_serialize(
        elaboration->result->solver, &handled);
    if (!proof) {
        elaboration->result->status = QTT_CORE_EFFECT_OUT_OF_MEMORY;
        return NULL;
    }
    node->handle.portable_proof = proof;
    node->handle.profile_name = profile->name;
    node->handle.deep = profile->deep;
    node->handle.resumption_class = classify_resumption(
        declaration->resumption);
    elaboration->result->computation_effects = computation;
    elaboration->result->clause_effects = clause_effects;
    elaboration->result->handled_atom = atom;
    elaboration->result->continuation_usage = profile->continuation_usage;
    elaboration->result->capture_allowance = elaboration->capture_allowance;
    return handled.output_effects;
}

static QttEffectRow *elaborate_node(Elaboration *elaboration,
                                    QttCoreNode *node) {
    if (!node || elaboration->result->status != QTT_CORE_EFFECT_OK) {
        if (!node) elaboration->result->status = QTT_CORE_EFFECT_INVALID_CORE;
        return NULL;
    }
    switch (node->kind) {
    case QTT_CORE_PERFORM: return elaborate_perform(elaboration, node);
    case QTT_CORE_HANDLE: return elaborate_handle(elaboration, node);
    case QTT_CORE_LAMBDA: return empty(elaboration);
    case QTT_CORE_APPLY: {
        QttEffectRow *effects = elaborate_node(
            elaboration, node->apply.callee);
        for (size_t i = 0; effects && i < node->apply.argument_count; i++)
            effects = join(elaboration, effects, elaborate_node(
                elaboration, node->apply.arguments[i]));
        if (effects)
            effects = join(
                elaboration, effects,
                elaborate_apply_latent(elaboration, node));
        return effects;
    }
    case QTT_CORE_LET:
        return join(elaboration, elaborate_node(elaboration, node->let.value),
                    elaborate_node(elaboration, node->let.body));
    case QTT_CORE_IF:
        return join(elaboration,
            elaborate_node(elaboration, node->conditional.condition),
            join(elaboration,
                elaborate_node(elaboration, node->conditional.then_branch),
                elaborate_node(elaboration, node->conditional.else_branch)));
    case QTT_CORE_SEQUENCE:
        return elaborate_many(elaboration, node->sequence.items,
                              node->sequence.count);
    case QTT_CORE_WRITE:
        return elaborate_node(elaboration, node->write.value);
    case QTT_CORE_BORROW:
        return elaborate_node(elaboration, node->borrow.body);
    case QTT_CORE_LITERAL: case QTT_CORE_GLOBAL: case QTT_CORE_VAR:
    case QTT_CORE_PLACE: case QTT_CORE_QUOTE:
        return empty(elaboration);
    }
    elaboration->result->status = QTT_CORE_EFFECT_INVALID_CORE;
    return NULL;
}

QttCoreEffectResult qtt_core_effect_elaborate(
    QttCoreNode *root, QttQuantity capture_allowance) {
    QttCoreEffectResult result = {.status = QTT_CORE_EFFECT_OK};
    result.arena = qtt_effect_arena_new();
    result.solver = result.arena ? qtt_effect_solver_new(result.arena) : NULL;
    result.constraints = qtt_effect_constraints_new();
    if (!result.arena || !result.solver || !result.constraints) {
        result.status = QTT_CORE_EFFECT_OUT_OF_MEMORY;
        return result;
    }
    Elaboration elaboration = {
        .result = &result, .capture_allowance = capture_allowance};
    result.effects = elaborate_node(&elaboration, root);
    if (result.status == QTT_CORE_EFFECT_OK && result.effects)
        result.fingerprint = qtt_effect_row_fingerprint(
            result.solver, result.effects);
    else
        result.effects = NULL;
    if (result.status == QTT_CORE_EFFECT_OK && result.effects) {
        result.constraint_result = qtt_effect_constraints_solve(
            result.constraints, result.arena, result.solver,
            &result.constraint_certificate);
        if (result.constraint_result == QTT_EFFECT_CONSTRAINT_REJECTED)
            result.status = QTT_CORE_EFFECT_INVALID_CALL_EFFECT;
        else if (result.constraint_result ==
                 QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY)
            result.status = QTT_CORE_EFFECT_OUT_OF_MEMORY;
        else if (result.constraint_certificate)
            result.constraint_fingerprint =
                qtt_effect_certificate_fingerprint(
                    result.constraint_certificate, result.solver);
    }
    return result;
}

bool qtt_core_effect_verify_handle(
    const QttCoreNode *handle, const QttCoreEffectResult *result) {
    if (!handle || handle->kind != QTT_CORE_HANDLE || !result ||
        result->status != QTT_CORE_EFFECT_OK ||
        !handle->handle.portable_proof || !result->computation_effects ||
        !result->clause_effects) return false;
    const QttEffectHandlerProfile *profile =
        qtt_effect_handler_profile_lookup(handle->handle.profile_name);
    const QttEffectDeclaration *declaration = profile
        ? qtt_effect_declaration_lookup(profile->effect_name) : NULL;
    QttCoreNode *selected = NULL;
    size_t matching = 0;
    bool unsupported_context = false;
    bool effectful_context = false;
    if (!declaration) return false;
    qtt_core_effect_select_operation(
        handle->handle.computation, declaration->name,
        &selected, &matching, &unsupported_context, &effectful_context);
    if (matching != 1 || unsupported_context ||
        selected != handle->handle.selected_operation ||
        handle->handle.abortive_context_safe == effectful_context)
        return false;
    QttEffectHandlerClause clause = {
        .atom = &result->handled_atom,
        .continuation_usage = result->continuation_usage,
        .capture_allowance = result->capture_allowance,
        .clause_effects = result->clause_effects,
    };
    return qtt_effect_verify_portable_handler_proof(
        result->arena, result->solver, result->computation_effects,
        &clause, 1, handle->handle.portable_proof);
}

bool qtt_core_effect_verify_constraints(
    const QttCoreEffectResult *result) {
    if (!result || result->status != QTT_CORE_EFFECT_OK ||
        !result->constraints || !result->constraint_certificate ||
        (result->constraint_result != QTT_EFFECT_CONSTRAINT_SOLVED &&
         result->constraint_result != QTT_EFFECT_CONSTRAINT_RESIDUAL))
        return false;
    return qtt_effect_certificate_fingerprint(
               result->constraint_certificate, result->solver) ==
               result->constraint_fingerprint &&
        qtt_effect_certificate_verify(
            result->constraint_certificate, result->arena, result->solver);
}

QttCoreEffectRuntimePolicy qtt_core_effect_runtime_policy(
    const QttCoreNode *handle) {
    if (!handle || handle->kind != QTT_CORE_HANDLE ||
        !handle->handle.portable_proof || !handle->handle.deep)
        return QTT_CORE_EFFECT_RUNTIME_INVALID;
    switch (handle->handle.resumption_class) {
    case QTT_CORE_RESUMPTION_ABORTIVE:
        return QTT_CORE_EFFECT_RUNTIME_ABORTIVE_DIRECT;
    case QTT_CORE_RESUMPTION_ONE_SHOT:
        return QTT_CORE_EFFECT_RUNTIME_ONE_SHOT_LINEAR;
    case QTT_CORE_RESUMPTION_MULTI_SHOT:
    case QTT_CORE_RESUMPTION_UNRESTRICTED:
        return QTT_CORE_EFFECT_RUNTIME_UNSUPPORTED_MULTI_SHOT;
    }
    return QTT_CORE_EFFECT_RUNTIME_INVALID;
}

static uint64_t authority_mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t authority_text(uint64_t hash, const char *value) {
    if (!value) return authority_mix(hash, 0);
    while (*value) hash = authority_mix(hash, (unsigned char)*value++);
    return authority_mix(hash, 0xff);
}

uint64_t qtt_core_effect_authority_fingerprint(const QttCoreNode *node) {
    if (!node) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = authority_mix(hash, node->kind);
    if (node->kind == QTT_CORE_PERFORM) {
        hash = authority_text(hash, node->perform.effect_name);
        hash = authority_mix(hash, node->perform.capability_id);
        hash = authority_mix(hash, node->perform.constructor_id);
        hash = authority_mix(hash, node->perform.resumption.is_omega);
        hash = authority_mix(hash, node->perform.resumption.finite);
        hash = authority_mix(hash, node->perform.scoped);
    } else if (node->kind == QTT_CORE_GLOBAL) {
        hash = authority_text(hash, node->global.name);
        hash = authority_text(hash, node->global.callable_contract);
        hash = authority_mix(
            hash, node->global.callable_contract_fingerprint);
        hash = authority_text(hash, node->global.hm_scheme);
        hash = authority_mix(hash, node->global.effect_row_fingerprint);
        hash = authority_mix(
            hash, node->global.effect_constraint_fingerprint);
        hash = authority_mix(hash, node->global.effect_constraint_result);
        hash = authority_mix(hash, node->global.effect_predicate_count);
        for (size_t i = 0; i < node->global.effect_predicate_count; i++) {
            hash = authority_mix(
                hash, node->global.effect_predicate_stages[i]);
            hash = authority_text(
                hash, node->global.effect_predicate_names[i]);
        }
    } else if (node->kind == QTT_CORE_APPLY) {
        hash = authority_mix(hash,
            qtt_core_effect_authority_fingerprint(node->apply.callee));
        hash = authority_mix(hash, node->apply.argument_count);
        for (size_t i = 0; i < node->apply.argument_count; i++)
            hash = authority_mix(hash,
                qtt_core_effect_authority_fingerprint(
                    node->apply.arguments[i]));
    } else if (node->kind == QTT_CORE_HANDLE) {
        hash = authority_text(hash, node->handle.profile_name);
        hash = authority_mix(hash, node->handle.capability_id);
        hash = authority_mix(hash, node->handle.resumption_class);
        hash = authority_mix(hash, node->handle.deep);
        hash = authority_mix(hash, node->handle.abortive_context_safe);
        hash = authority_text(hash, node->handle.portable_proof);
        hash = authority_mix(hash,
            qtt_core_effect_authority_fingerprint(
                node->handle.selected_operation));
        hash = authority_mix(hash,
            qtt_core_effect_authority_fingerprint(
                node->handle.computation));
    } else {
        return 0;
    }
    return hash ? hash : 1;
}

void qtt_core_effect_result_free(QttCoreEffectResult *result) {
    if (!result) return;
    qtt_effect_certificate_free(result->constraint_certificate);
    qtt_effect_constraints_free(result->constraints);
    qtt_effect_solver_free(result->solver);
    qtt_effect_arena_free(result->arena);
    memset(result, 0, sizeof(*result));
}
