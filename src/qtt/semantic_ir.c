#include "semantic_ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
extern bool qtt_core_effect_verify_constraints(
    const QttCoreEffectResult *result) __attribute__((weak));
extern uint64_t qtt_core_effect_authority_fingerprint(
    const QttCoreNode *node) __attribute__((weak));
extern size_t qtt_effect_constraints_count(
    const QttEffectConstraintSet *set) __attribute__((weak));
#endif

QttCoreVar qtt_semantic_owned_result_resource(
    uint64_t module_id, size_t call_ordinal) {
    if (!module_id ||
        call_ordinal > UINT64_MAX -
            UINT64_C(0x8000000000000000))
        return (QttCoreVar){0};
    return (QttCoreVar){
        .module_id = module_id,
        .binder_id =
            UINT64_C(0x8000000000000000) +
            (uint64_t)call_ordinal,
    };
}

struct QttSemanticFunction {
    const QttCoreNode *source;
    QttSemanticNode *nodes;
    size_t node_count;
    QttGradeArena *grades;
    QttTypeArena *types;
    QttEffectArena *effects;
    QttEffectSolver *effect_solver;
    QttEffectRow *pure_effect;
    QttUsageContext *usage;
    QttSemanticGradeEvidence *grade_evidence;
    size_t grade_evidence_count;
    uint64_t module_id;
    QttResourceBlock *resources;
    QttResourceElaborationError resource_status;
    QttSemanticDestructorEvidence *destructors;
    size_t destructor_count;
    QttSemanticCapabilityTransition *transitions;
    size_t transition_count;
    QttSemanticCapabilityEdge *edges;
    size_t edge_count;
    const QttSignatureEnv *signatures;
    QttSemanticCallEvidence *calls;
    size_t call_count;
    QttSemanticClosureFieldEvidence *closure_fields;
    size_t closure_field_count;
    QttSemanticEffectJudgment effect_judgment;
};

typedef struct {
    QttSemanticCapabilityEdge *items;
    size_t count;
    size_t capacity;
} EdgeProjection;

typedef struct {
    QttCoreVar *items;
    size_t count;
    size_t capacity;
} EdgeState;

static size_t child_count(const QttCoreNode *node);
static const QttCoreNode *child_at(
    const QttCoreNode *node, size_t index);
static QttEffectRow *derive_effects(
    const QttSemanticFunction *function, const QttCoreNode *node,
    const QttCallableBinding *callables);

static QttEffectRow *named_latent_effects(
    const QttSemanticFunction *function, const QttCoreNode *node) {
    if (!function || !function->signatures || !node ||
        node->kind != QTT_CORE_APPLY || !node->apply.callee ||
        node->apply.callee->kind != QTT_CORE_GLOBAL)
        return NULL;
    QttFunctionSignature *signature = NULL;
    if (!qtt_signature_env_resolve(
            function->signatures, function->module_id,
            node->apply.callee->global.name, NULL, &signature) ||
        !signature || !signature->effects_complete ||
        !signature->latent_effects || !signature->effect_solver)
        return NULL;
    return qtt_effect_clone_closed(
        function->effects, signature->effect_solver,
        signature->latent_effects);
}

static bool control_path_copy(
    QttControlPathStep **destination,
    const QttControlPathStep *path, size_t count) {
    *destination = NULL;
    if (!count) return true;
    *destination = malloc(count * sizeof(**destination));
    if (!*destination) return false;
    memcpy(*destination, path, count * sizeof(**destination));
    return true;
}

static bool collect_calls(
    QttSemanticFunction *function, const QttCoreNode *node,
    uint64_t module_id, const QttControlPathStep *path,
    size_t path_count, size_t *node_cursor,
    size_t *branch_cursor, size_t *call_cursor) {
    if (!node) return false;
    size_t node_index = (*node_cursor)++;
    if (node->kind == QTT_CORE_IF) {
        if (!collect_calls(
                function, node->conditional.condition, module_id,
                path, path_count, node_cursor,
                branch_cursor, call_cursor))
            return false;
        size_t branch = (*branch_cursor)++;
        QttControlPathStep *then_path = malloc(
            (path_count + 1) * sizeof(*then_path));
        QttControlPathStep *else_path = malloc(
            (path_count + 1) * sizeof(*else_path));
        if (!then_path || !else_path) {
            free(then_path);
            free(else_path);
            return false;
        }
        if (path_count) {
            memcpy(then_path, path, path_count * sizeof(*path));
            memcpy(else_path, path, path_count * sizeof(*path));
        }
        then_path[path_count] = (QttControlPathStep){
            .branch_ordinal = branch, .else_arm = false};
        else_path[path_count] = (QttControlPathStep){
            .branch_ordinal = branch, .else_arm = true};
        bool valid =
            collect_calls(
                function, node->conditional.then_branch, module_id,
                then_path, path_count + 1, node_cursor,
                branch_cursor, call_cursor) &&
            collect_calls(
                function, node->conditional.else_branch, module_id,
                else_path, path_count + 1, node_cursor,
                branch_cursor, call_cursor);
        free(then_path);
        free(else_path);
        return valid;
    }
    for (size_t i = 0; i < child_count(node); i++)
        if (!collect_calls(
                function, child_at(node, i), module_id,
                path, path_count, node_cursor,
                branch_cursor, call_cursor))
            return false;
    if (node->kind != QTT_CORE_APPLY || !node->apply.callee ||
        node->apply.callee->kind != QTT_CORE_GLOBAL)
        return true;
    QttCallableId callable = {0};
    QttFunctionSignature *signature = NULL;
    if (!function->signatures ||
        !qtt_signature_env_resolve(
            function->signatures, module_id,
            node->apply.callee->global.name,
            &callable, &signature) ||
        !signature ||
        signature->parameter_count != node->apply.argument_count)
        return false;
    size_t count = function->call_count + 1;
    QttSemanticCallEvidence *grown = realloc(
        function->calls, count * sizeof(*grown));
    if (!grown) return false;
    function->calls = grown;
    QttSemanticCallEvidence *call =
        &function->calls[function->call_count];
    size_t call_ordinal = (*call_cursor)++;
    *call = (QttSemanticCallEvidence){
        .node_index = node_index,
        .call_ordinal = call_ordinal,
        .callable = callable,
        .contract_fingerprint = signature->contract_fingerprint,
        .argument_count = signature->parameter_count,
        .result = signature->result,
        .control_path_count = path_count,
        .result_resource =
            signature->result.mode == QTT_RESULT_OWNED
                ? qtt_semantic_owned_result_resource(
                    module_id, call_ordinal)
                : (QttCoreVar){0},
    };
    function->call_count = count;
    if (!control_path_copy(
            &call->control_path, path, path_count))
        return false;
    size_t arguments = call->argument_count;
    if (arguments) {
        call->argument_transfers = calloc(
            arguments, sizeof(*call->argument_transfers));
        call->argument_representations = calloc(
            arguments, sizeof(*call->argument_representations));
        call->argument_type_ids = calloc(
            arguments, sizeof(*call->argument_type_ids));
        call->argument_nominal_authorities = calloc(
            arguments, sizeof(*call->argument_nominal_authorities));
        call->source_vars = calloc(
            arguments, sizeof(*call->source_vars));
        if (!call->argument_transfers ||
            !call->argument_representations ||
            !call->argument_type_ids ||
            !call->argument_nominal_authorities || !call->source_vars)
            return false;
    }
    for (size_t i = 0; i < arguments; i++) {
        const QttParameterContract *parameter =
            &signature->parameters[i];
        call->argument_transfers[i] =
            qtt_call_transfer_for_parameter(parameter);
        call->argument_representations[i] =
            parameter->representation;
        call->argument_type_ids[i] = parameter->type_id;
        call->argument_nominal_authorities[i] =
            parameter->nominal_authority;
        if (call->argument_transfers[i] != QTT_CALL_VALUE &&
            node->apply.arguments[i]->kind == QTT_CORE_VAR)
            call->source_vars[i] =
                node->apply.arguments[i]->var;
    }
    return true;
}

static bool collect_closure_fields(
    QttSemanticFunction *function, const QttCoreNode *node,
    size_t *node_cursor) {
    if (!node) return false;
    size_t node_index = (*node_cursor)++;
    if (node->kind == QTT_CORE_LAMBDA) {
        QttCoreVar *captures = NULL;
        size_t capture_count = 0;
        if (qtt_core_lambda_captures(
                node, &captures, &capture_count) != QTT_CORE_OK)
            return false;
        for (size_t i = 0; i < capture_count; i++) {
            const Type *type =
                qtt_core_var_type(node->lambda.body, captures[i]);
            QttRepresentation representation =
                qtt_resource_classify_type(type);
            QttTypeIdentityError type_error = QTT_TYPE_IDENTITY_OK;
            QttTypeId type_id =
                qtt_type_intern(function->types, type, &type_error);
            if (!type || !type_id.value ||
                representation == QTT_REP_UNKNOWN) {
                free(captures);
                return false;
            }
            size_t count = function->closure_field_count + 1;
            QttSemanticClosureFieldEvidence *grown = realloc(
                function->closure_fields, count * sizeof(*grown));
            if (!grown) {
                free(captures);
                return false;
            }
            function->closure_fields = grown;
            QttSemanticClosureFieldEvidence *field =
                &function->closure_fields[
                    function->closure_field_count];
            *field = (QttSemanticClosureFieldEvidence){
                .node_index = node_index,
                .closure_id = function->nodes[node_index].closure_id,
                .ordinal = i,
                .source = captures[i],
                .type_id = type_id,
                .type = type,
                .representation = representation,
                .storage =
                    function->nodes[node_index].closure_policy.storage,
                .exit =
                    function->nodes[node_index].closure_policy.storage ==
                        QTT_CLOSURE_STORAGE_UNIQUE
                        ? function->signatures
                            ? qtt_closure_capture_exit_in_env(
                                node, captures[i],
                                function->signatures,
                                function->module_id)
                            : qtt_closure_capture_exit(
                                node, captures[i])
                        : QTT_CLOSURE_FIELD_RELEASE,
            };
            if (representation == QTT_REP_OWNED_HEAP) {
                QttDropPlanError drop_error = QTT_DROP_PLAN_OK;
                field->descriptor =
                    qtt_destructor_descriptor_build(type, &drop_error);
                if (!field->descriptor) {
                    free(captures);
                    return false;
                }
            }
            function->closure_field_count = count;
        }
        free(captures);
    }
    for (size_t i = 0; i < child_count(node); i++)
        if (!collect_closure_fields(
                function, child_at(node, i), node_cursor))
            return false;
    return true;
}

static void call_evidence_free(QttSemanticCallEvidence *call) {
    if (!call) return;
    free(call->argument_transfers);
    free(call->argument_representations);
    free(call->argument_type_ids);
    free(call->argument_nominal_authorities);
    free(call->source_vars);
    free(call->control_path);
}

static bool call_evidence_equal(
    const QttSemanticCallEvidence *left,
    const QttSemanticCallEvidence *right) {
    if (left->node_index != right->node_index ||
        left->call_ordinal != right->call_ordinal ||
        !qtt_callable_id_equal(left->callable, right->callable) ||
        left->contract_fingerprint != right->contract_fingerprint ||
        left->argument_count != right->argument_count ||
        left->control_path_count != right->control_path_count ||
        left->result.mode != right->result.mode ||
        left->result.representation != right->result.representation ||
        !qtt_core_var_equal(
            left->result_resource, right->result_resource) ||
        !qtt_type_id_equal(
            left->result.type_id, right->result.type_id))
        return false;
    for (size_t i = 0; i < left->argument_count; i++)
        if (left->argument_transfers[i] !=
                right->argument_transfers[i] ||
            left->argument_representations[i] !=
                right->argument_representations[i] ||
            !qtt_type_id_equal(
                left->argument_type_ids[i],
                right->argument_type_ids[i]) ||
            ((left->argument_representations[i] == QTT_REP_FOREIGN ||
              right->argument_representations[i] == QTT_REP_FOREIGN) &&
             !qtt_nominal_authority_equal(
                 left->argument_nominal_authorities[i],
                 right->argument_nominal_authorities[i])) ||
            !qtt_core_var_equal(
                left->source_vars[i], right->source_vars[i]))
            return false;
    for (size_t i = 0; i < left->control_path_count; i++)
        if (left->control_path[i].branch_ordinal !=
                right->control_path[i].branch_ordinal ||
            left->control_path[i].else_arm !=
                right->control_path[i].else_arm)
            return false;
    return true;
}

static ptrdiff_t edge_state_find(
    const EdgeState *state, QttCoreVar var) {
    for (size_t i = 0; i < state->count; i++)
        if (qtt_core_var_equal(state->items[i], var))
            return (ptrdiff_t)i;
    return -1;
}

static bool edge_state_push(EdgeState *state, QttCoreVar var) {
    if (state->count == state->capacity) {
        size_t next = state->capacity ? state->capacity * 2 : 8;
        QttCoreVar *grown = realloc(
            state->items, next * sizeof(*grown));
        if (!grown) return false;
        state->items = grown;
        state->capacity = next;
    }
    state->items[state->count++] = var;
    return true;
}

static bool edge_state_clone(
    EdgeState *destination, const EdgeState *source) {
    *destination = (EdgeState){0};
    if (!source->count) return true;
    destination->items =
        malloc(source->count * sizeof(*destination->items));
    if (!destination->items) return false;
    for (size_t i = 0; i < source->count; i++)
        destination->items[i] = source->items[i];
    destination->count = source->count;
    destination->capacity = source->count;
    return true;
}

static bool edge_state_equal(
    const EdgeState *left, const EdgeState *right) {
    if (left->count != right->count) return false;
    for (size_t i = 0; i < left->count; i++)
        if (edge_state_find(right, left->items[i]) < 0)
            return false;
    return true;
}

static uint64_t edge_context_fingerprint(const EdgeState *state) {
    uint64_t sum = UINT64_C(0x9e3779b97f4a7c15) ^ state->count;
    uint64_t product = UINT64_C(1099511628211);
    for (size_t i = 0; i < state->count; i++) {
        uint64_t item = state->items[i].module_id *
            UINT64_C(0x9e3779b185ebca87);
        item ^= state->items[i].binder_id *
            UINT64_C(0xc2b2ae3d27d4eb4f);
        sum += item;
        product *= item | UINT64_C(1);
    }
    return sum ^ product;
}

static size_t child_count(const QttCoreNode *node) {
    switch (node->kind) {
    case QTT_CORE_LAMBDA: return 1;
    case QTT_CORE_APPLY: return 1 + node->apply.argument_count;
    case QTT_CORE_LET: return 2;
    case QTT_CORE_IF: return 3;
    case QTT_CORE_SEQUENCE: return node->sequence.count;
    case QTT_CORE_WRITE: return 1;
    case QTT_CORE_BORROW: return 1;
    case QTT_CORE_PERFORM: return 1;
    case QTT_CORE_HANDLE: return 2;
    default: return 0;
    }
}

static const QttCoreNode *child_at(
    const QttCoreNode *node, size_t index) {
    switch (node->kind) {
    case QTT_CORE_LAMBDA: return node->lambda.body;
    case QTT_CORE_APPLY:
        return index ? node->apply.arguments[index - 1]
                     : node->apply.callee;
    case QTT_CORE_LET: return index ? node->let.body : node->let.value;
    case QTT_CORE_IF:
        if (!index) return node->conditional.condition;
        return index == 1 ? node->conditional.then_branch
                          : node->conditional.else_branch;
    case QTT_CORE_SEQUENCE: return node->sequence.items[index];
    case QTT_CORE_WRITE: return node->write.value;
    case QTT_CORE_BORROW: return node->borrow.body;
    case QTT_CORE_PERFORM: return node->perform.argument;
    case QTT_CORE_HANDLE:
        return index ? node->handle.clause : node->handle.computation;
    default: return NULL;
    }
}

static size_t count_tree(const QttCoreNode *node) {
    if (!node) return 0;
    size_t count = 1;
    for (size_t i = 0; i < child_count(node); i++)
        count += count_tree(child_at(node, i));
    return count;
}

static QttSemanticGradeRole grade_role(const QttCoreNode *node) {
    switch (node->kind) {
    case QTT_CORE_LET: return QTT_SEMANTIC_GRADE_ALIAS_TRANSFER;
    case QTT_CORE_LAMBDA: return QTT_SEMANTIC_GRADE_CONSTRUCTION;
    case QTT_CORE_APPLY: return QTT_SEMANTIC_GRADE_INVOCATION;
    default: return QTT_SEMANTIC_GRADE_RESULT_DEMAND;
    }
}

static QttSemanticCapability capability_for(
    QttRepresentation representation) {
    switch (representation) {
    case QTT_REP_IMMEDIATE:
    case QTT_REP_INLINE:
        return QTT_SEMANTIC_CAPABILITY_NONE;
    case QTT_REP_OWNED_HEAP:
    case QTT_REP_FOREIGN:
    case QTT_REP_UNKNOWN:
        return QTT_SEMANTIC_CAPABILITY_UNKNOWN;
    }
    return QTT_SEMANTIC_CAPABILITY_UNKNOWN;
}

static size_t emit_tree(
    const QttCoreNode *node, QttSemanticFunction *function,
    size_t *cursor, bool *valid,
    const QttCallableBinding *callables) {
    size_t index = (*cursor)++;
    if (node->kind == QTT_CORE_VAR)
        function->module_id = node->var.module_id;
    else if (node->kind == QTT_CORE_LET)
        function->module_id = node->let.binding.module_id;
    else if (node->kind == QTT_CORE_LAMBDA &&
             node->lambda.param_count)
        function->module_id =
            node->lambda.params[0].module_id;
    else if (node->kind == QTT_CORE_WRITE)
        function->module_id =
            node->write.place.root.module_id;
    else if (node->kind == QTT_CORE_BORROW)
        function->module_id = node->borrow.binding.module_id;
    else if (node->kind == QTT_CORE_PLACE)
        function->module_id = node->place.root.module_id;
    QttTypeIdentityError type_error = QTT_TYPE_IDENTITY_OK;
    QttTypeId type_id =
        qtt_type_intern(function->types, node->type, &type_error);
    if (!type_id.value) *valid = false;
    QttRepresentation representation =
        qtt_resource_classify_type(node->type);
    QttEffectRow *node_effect = function->pure_effect;
    if (node->kind == QTT_CORE_WRITE)
        node_effect = qtt_effect_write_place(
            function->effects, node->write.place,
            function->pure_effect);
    else if (node->kind == QTT_CORE_PLACE)
        node_effect = qtt_effect_read_place(
            function->effects, node->place,
            function->pure_effect);
    if (!node_effect) *valid = false;
    function->nodes[index] = (QttSemanticNode){
        .kind = node->kind,
        .source = node,
        .type_id = type_id,
        .grade_role = grade_role(node),
        .effects = node_effect,
        .representation = representation,
        .capability = capability_for(representation),
        .effect_constructor_id = node->kind == QTT_CORE_PERFORM
            ? node->perform.constructor_id
            : node->kind == QTT_CORE_HANDLE &&
              node->handle.selected_operation
                ? node->handle.selected_operation->perform.constructor_id : 0,
        .effect_capability_id = node->kind == QTT_CORE_PERFORM
            ? node->perform.capability_id
            : node->kind == QTT_CORE_HANDLE &&
              node->handle.selected_operation
                ? node->handle.selected_operation->perform.capability_id : 0,
        .var = node->kind == QTT_CORE_VAR ? node->var
             : node->kind == QTT_CORE_LET ? node->let.binding
             : node->kind == QTT_CORE_BORROW ? node->borrow.binding
             : node->kind == QTT_CORE_WRITE
                 ? node->write.place.root
             : node->kind == QTT_CORE_PLACE
                 ? node->place.root
             : (QttCoreVar){0},
        .place = node->kind == QTT_CORE_WRITE
            ? node->write.place
            : node->kind == QTT_CORE_BORROW ? node->borrow.place
            : node->kind == QTT_CORE_PLACE ? node->place
            : (QttPlace){
                .root = {.module_id = 0, .binder_id = 0},
                .projection_id = 0,
            },
        .parent_loan = node->kind == QTT_CORE_BORROW
            ? node->borrow.parent : (QttCoreVar){0},
        .loan_kind = node->kind == QTT_CORE_BORROW
            ? node->borrow.loan_kind : QTT_LOAN_SHARED,
    };
    if (node->kind == QTT_CORE_LAMBDA) {
        function->nodes[index].closure_id = index + 1;
        function->nodes[index].closure_policy =
            function->signatures
                ? qtt_closure_policy_infer_in_env(
                    function->source, node, function->signatures,
                    function->module_id)
                : qtt_closure_policy_infer(function->source, node);
    }
    if (node->kind == QTT_CORE_APPLY) {
        function->nodes[index].instance_id = index + 1;
        function->nodes[index].domain_count =
            node->apply.argument_count;
        if (node->apply.callee &&
            node->apply.callee->kind == QTT_CORE_LAMBDA)
            function->nodes[index].callee_closure_id = index + 2;
    }
    size_t total = 1;
    QttEffectRow *summary = node_effect;
    for (size_t i = 0; i < child_count(node); i++) {
        size_t child_index = *cursor;
        const QttCallableBinding *child_environment = callables;
        QttCallableBinding binding = {0};
        if (node->kind == QTT_CORE_LET && i == 1) {
            const QttCoreNode *known = qtt_callable_env_value(
                callables, node->let.value);
            if (known) {
                binding = (QttCallableBinding){
                    .var = node->let.binding, .lambda = known,
                    .parent = callables};
                child_environment = &binding;
            }
        }
        total += emit_tree(
            child_at(node, i), function, cursor, valid,
            child_environment);
        if (node->kind != QTT_CORE_LAMBDA) {
            summary = qtt_effect_join_closed(
                function->effects, function->effect_solver,
                summary, function->nodes[child_index].effects);
            if (!summary) *valid = false;
        }
        if (node->kind == QTT_CORE_APPLY && i == 0 &&
            child_at(node, i)->kind == QTT_CORE_LAMBDA) {
            summary = qtt_effect_join_closed(
                function->effects, function->effect_solver,
                summary, function->nodes[child_index + 1].effects);
            if (!summary) *valid = false;
        }
    }
    if (node->kind == QTT_CORE_APPLY && node->apply.callee &&
        node->apply.callee->kind == QTT_CORE_GLOBAL) {
        QttEffectRow *latent = named_latent_effects(function, node);
        if (latent)
            summary = qtt_effect_join_closed(
                function->effects, function->effect_solver,
                summary, latent);
        if (!summary) *valid = false;
    }
    if (node->kind == QTT_CORE_APPLY && node->apply.callee &&
        node->apply.callee->kind == QTT_CORE_VAR) {
        const QttCoreNode *known = qtt_callable_env_lookup(
            callables, node->apply.callee->var);
        QttEffectRow *latent = known
            ? derive_effects(function, known->lambda.body, callables)
            : NULL;
        if (latent)
            summary = qtt_effect_join_closed(
                function->effects, function->effect_solver,
                summary, latent);
        else
            *valid = false;
        if (!summary) *valid = false;
    }
    function->nodes[index].effects = summary;
    function->nodes[index].subtree_size = total;
    return total;
}

static bool append_evidence(
    QttSemanticFunction *function, size_t node_index,
    QttSemanticGradeRole role, const QttUsageContext *usage) {
    size_t count = qtt_usage_binding_count(usage);
    function->nodes[node_index].grade_offset =
        function->grade_evidence_count;
    function->nodes[node_index].grade_count = count;
    if (!count) return true;
    size_t total = function->grade_evidence_count + count;
    QttSemanticGradeEvidence *grown = realloc(
        function->grade_evidence, total * sizeof(*grown));
    if (!grown) return false;
    function->grade_evidence = grown;
    for (size_t i = 0; i < count; i++)
        function->grade_evidence[
            function->grade_evidence_count + i] =
            (QttSemanticGradeEvidence){
                .node_index = node_index,
                .module_id = function->module_id,
                .binder_id = qtt_usage_binding_id(usage, i),
                .role = role,
                .expression = qtt_usage_binding_grade(usage, i),
            };
    function->grade_evidence_count = total;
    return true;
}

static const Type *semantic_expression_type(
    const QttSemanticFunction *function,
    const QttCoreNode *node) {
    if (node && node->type &&
        qtt_resource_classify_type(node->type) != QTT_REP_UNKNOWN)
        return node->type;
    if (!function || !function->signatures || !node ||
        node->kind != QTT_CORE_APPLY || !node->apply.callee ||
        node->apply.callee->kind != QTT_CORE_GLOBAL)
        return node ? node->type : NULL;
    QttFunctionSignature *signature = NULL;
    return qtt_signature_env_resolve(
               function->signatures, function->module_id,
               node->apply.callee->global.name,
               NULL, &signature) &&
           signature
        ? signature->result.type
        : node->type;
}

static bool resource_contains_drop(const QttResourceBlock *block,
                                   QttCoreVar root) {
    if (!block) return false;
    for (size_t i = 0; i < block->count; i++) {
        const QttResourceOp *op = &block->ops[i];
        if (op->kind == QTT_RESOURCE_DROP &&
            qtt_core_var_equal(op->var, root))
            return true;
        if (op->kind == QTT_RESOURCE_BRANCH &&
            (resource_contains_drop(op->branch.then_block, root) ||
             resource_contains_drop(op->branch.else_block, root)))
            return true;
    }
    return false;
}

static bool drop_masks_equal(const QttDropMaskCertificate *left,
                             const QttDropMaskCertificate *right) {
    if (!left || !right ||
        !qtt_destructor_id_equal(left->destructor, right->destructor) ||
        !qtt_place_equal(left->root, right->root) ||
        left->evacuated_count != right->evacuated_count ||
        left->certificate_fingerprint != right->certificate_fingerprint)
        return false;
    for (size_t i = 0; i < left->evacuated_count; i++)
        if (!qtt_place_equal(left->evacuated[i], right->evacuated[i]))
            return false;
    return true;
}

static bool collect_destructors(
    QttSemanticFunction *function, const QttCoreNode *node) {
    const Type *value_type = node->kind == QTT_CORE_LET
        ? semantic_expression_type(function, node->let.value)
        : NULL;
    if (node->kind == QTT_CORE_LET && value_type &&
        qtt_resource_classify_type(value_type) ==
            QTT_REP_OWNED_HEAP) {
        QttDropPlanError drop_error = QTT_DROP_PLAN_OK;
        QttDestructorDescriptor *descriptor =
            qtt_destructor_descriptor_build(
                value_type, &drop_error);
        QttTypeIdentityError type_error =
            QTT_TYPE_IDENTITY_OK;
        QttTypeId type_id = qtt_type_intern(
            function->types, value_type,
            &type_error);
        bool has_drop = resource_contains_drop(
            function->resources, node->let.binding);
        QttDropMaskError mask_error = QTT_DROP_MASK_VALID;
        QttDropMaskCertificate *mask = descriptor && has_drop
            ? qtt_drop_mask_build_for_resource(
                descriptor, function->resources, node->let.binding,
                NULL, 0, &mask_error)
            : NULL;
        if (!descriptor || !type_id.value || (has_drop && !mask)) {
            qtt_drop_mask_free(mask);
            qtt_destructor_descriptor_free(descriptor);
            return false;
        }
        size_t count = function->destructor_count + 1;
        QttSemanticDestructorEvidence *grown = realloc(
            function->destructors, count * sizeof(*grown));
        if (!grown) {
            qtt_drop_mask_free(mask);
            qtt_destructor_descriptor_free(descriptor);
            return false;
        }
        function->destructors = grown;
        function->destructors[
            function->destructor_count++] =
            (QttSemanticDestructorEvidence){
                .var = node->let.binding,
                .type_id = type_id,
                .descriptor = descriptor,
                .mask = mask,
            };
    }
    for (size_t i = 0; i < child_count(node); i++)
        if (!collect_destructors(
                function, child_at(node, i)))
            return false;
    return true;
}

static void transition_capabilities(
    QttResourceOpKind kind, QttSemanticCapability *before,
    QttSemanticCapability *after,
    QttSemanticCapability *target_after) {
    *target_after = QTT_SEMANTIC_CAPABILITY_NONE;
    switch (kind) {
    case QTT_RESOURCE_ALLOC:
        *before = QTT_SEMANTIC_CAPABILITY_NONE;
        *after = QTT_SEMANTIC_CAPABILITY_OWNED;
        break;
    case QTT_RESOURCE_BORROW:
        *before = QTT_SEMANTIC_CAPABILITY_OWNED;
        *after = QTT_SEMANTIC_CAPABILITY_OWNED;
        break;
    case QTT_RESOURCE_ALIAS:
        *before = QTT_SEMANTIC_CAPABILITY_NONE;
        *after = QTT_SEMANTIC_CAPABILITY_BORROWED;
        *target_after = QTT_SEMANTIC_CAPABILITY_OWNED;
        break;
    case QTT_RESOURCE_END_ALIAS:
        *before = QTT_SEMANTIC_CAPABILITY_BORROWED;
        *after = QTT_SEMANTIC_CAPABILITY_NONE;
        *target_after = QTT_SEMANTIC_CAPABILITY_OWNED;
        break;
    case QTT_RESOURCE_MOVE:
    case QTT_RESOURCE_DROP:
        *before = QTT_SEMANTIC_CAPABILITY_OWNED;
        *after = QTT_SEMANTIC_CAPABILITY_NONE;
        break;
    case QTT_RESOURCE_MOVE_PLACE:
        *before = QTT_SEMANTIC_CAPABILITY_OWNED;
        *after = QTT_SEMANTIC_CAPABILITY_OWNED;
        break;
    case QTT_RESOURCE_MOVE_ALIAS:
        *before = QTT_SEMANTIC_CAPABILITY_BORROWED;
        *after = QTT_SEMANTIC_CAPABILITY_NONE;
        *target_after = QTT_SEMANTIC_CAPABILITY_NONE;
        break;
    case QTT_RESOURCE_WRITE_ALIAS:
        *before = QTT_SEMANTIC_CAPABILITY_BORROWED;
        *after = QTT_SEMANTIC_CAPABILITY_BORROWED;
        *target_after = QTT_SEMANTIC_CAPABILITY_OWNED;
        break;
    case QTT_RESOURCE_REHOME:
        *before = QTT_SEMANTIC_CAPABILITY_OWNED;
        *after = QTT_SEMANTIC_CAPABILITY_NONE;
        *target_after = QTT_SEMANTIC_CAPABILITY_OWNED;
        break;
    case QTT_RESOURCE_REPLACE:
        *before = QTT_SEMANTIC_CAPABILITY_OWNED;
        *after = QTT_SEMANTIC_CAPABILITY_OWNED;
        break;
    case QTT_RESOURCE_DUP:
        *before = QTT_SEMANTIC_CAPABILITY_OWNED;
        *after = QTT_SEMANTIC_CAPABILITY_SHARED;
        *target_after = QTT_SEMANTIC_CAPABILITY_SHARED;
        break;
    case QTT_RESOURCE_BRANCH:
        *before = QTT_SEMANTIC_CAPABILITY_NONE;
        *after = QTT_SEMANTIC_CAPABILITY_NONE;
        break;
    }
}

static bool collect_transitions(
    QttSemanticFunction *function,
    const QttResourceBlock *block,
    const QttControlPathStep *path,
    size_t path_count,
    size_t *branch_cursor) {
    for (size_t i = 0; i < block->count; i++) {
        const QttResourceOp *op = &block->ops[i];
        if (op->kind == QTT_RESOURCE_BRANCH) {
            size_t branch = (*branch_cursor)++;
            QttControlPathStep *then_path = malloc(
                (path_count + 1) * sizeof(*then_path));
            QttControlPathStep *else_path = malloc(
                (path_count + 1) * sizeof(*else_path));
            if (!then_path || !else_path) {
                free(then_path);
                free(else_path);
                return false;
            }
            if (path_count) {
                memcpy(
                    then_path, path,
                    path_count * sizeof(*then_path));
                memcpy(
                    else_path, path,
                    path_count * sizeof(*else_path));
            }
            then_path[path_count] = (QttControlPathStep){
                .branch_ordinal = branch, .else_arm = false};
            else_path[path_count] = (QttControlPathStep){
                .branch_ordinal = branch, .else_arm = true};
            bool collected =
                collect_transitions(
                    function, op->branch.then_block,
                    then_path, path_count + 1, branch_cursor) &&
                collect_transitions(
                    function, op->branch.else_block,
                    else_path, path_count + 1, branch_cursor);
            free(then_path);
            free(else_path);
            if (!collected) return false;
            continue;
        }
        size_t count = function->transition_count + 1;
        QttSemanticCapabilityTransition *grown = realloc(
            function->transitions, count * sizeof(*grown));
        if (!grown) return false;
        function->transitions = grown;
        QttSemanticCapabilityTransition *transition =
            &function->transitions[
                function->transition_count];
        *transition = (QttSemanticCapabilityTransition){
            .ordinal = function->transition_count,
            .kind = op->kind,
            .var = op->var,
            .target = op->target,
            .place = op->place,
            .representation = op->representation,
            .loan_kind = op->loan_kind,
            .control_path_count = path_count,
        };
        if (path_count) {
            transition->control_path = malloc(
                path_count * sizeof(*transition->control_path));
            if (!transition->control_path) return false;
            memcpy(
                transition->control_path, path,
                path_count * sizeof(*transition->control_path));
        }
        transition_capabilities(
            op->kind, &transition->before,
            &transition->after,
            &transition->target_after);
        function->transition_count = count;
    }
    return true;
}

static bool edge_projection_push(
    EdgeProjection *projection, QttSemanticCapabilityEdge edge) {
    if (projection->count == projection->capacity) {
        size_t next =
            projection->capacity ? projection->capacity * 2 : 8;
        QttSemanticCapabilityEdge *grown = realloc(
            projection->items, next * sizeof(*grown));
        if (!grown) return false;
        projection->items = grown;
        projection->capacity = next;
    }
    projection->items[projection->count++] = edge;
    return true;
}

static bool project_edges(
    const QttResourceBlock *block, EdgeState *state,
    size_t *transition_cursor, size_t *branch_cursor,
    EdgeProjection *projection) {
    for (size_t i = 0; i < block->count; i++) {
        const QttResourceOp *op = &block->ops[i];
        if (op->kind != QTT_RESOURCE_BRANCH) {
            if (op->kind == QTT_RESOURCE_ALLOC ||
                op->kind == QTT_RESOURCE_DUP) {
                QttCoreVar introduced =
                    op->kind == QTT_RESOURCE_ALLOC
                        ? op->var : op->target;
                if (edge_state_find(state, introduced) >= 0 ||
                    !edge_state_push(state, introduced))
                    return false;
            } else if (op->kind == QTT_RESOURCE_BORROW) {
                if (edge_state_find(state, op->var) < 0)
                    return false;
            }
            else if (op->kind == QTT_RESOURCE_ALIAS) {
                if (edge_state_find(state, op->target) < 0 ||
                    qtt_core_var_equal(op->var, op->target))
                    return false;
            }
            else if (op->kind == QTT_RESOURCE_END_ALIAS) {
                /*
                 * Resource verification owns the scoped alias map. Edge
                 * projection records this proof transition but only physical
                 * capabilities participate in branch live-set fingerprints.
                 */
            }
            else if (op->kind == QTT_RESOURCE_REHOME) {
                ptrdiff_t found =
                    edge_state_find(state, op->var);
                if (found < 0 ||
                    edge_state_find(state, op->target) >= 0)
                    return false;
                state->items[(size_t)found] = op->target;
            }
            else if (op->kind == QTT_RESOURCE_REPLACE) {
                if (edge_state_find(state, op->var) < 0 ||
                    op->representation != QTT_REP_OWNED_HEAP)
                    return false;
            }
            else if (op->kind == QTT_RESOURCE_MOVE ||
                     op->kind == QTT_RESOURCE_DROP) {
                ptrdiff_t found =
                    edge_state_find(state, op->var);
                if (found < 0) return false;
                state->items[(size_t)found] =
                    state->items[--state->count];
            }
            else if (op->kind == QTT_RESOURCE_MOVE_ALIAS) {
                ptrdiff_t found =
                    edge_state_find(state, op->target);
                if (found < 0) return false;
                state->items[(size_t)found] =
                    state->items[--state->count];
            }
            else if (op->kind == QTT_RESOURCE_MOVE_PLACE) {
                if (edge_state_find(state, op->var) < 0)
                    return false;
            }
            else if (op->kind == QTT_RESOURCE_WRITE_ALIAS) {
                if (edge_state_find(state, op->target) < 0)
                    return false;
            }
            (*transition_cursor)++;
            continue;
        }
        size_t branch = (*branch_cursor)++;
        size_t entry = state->count;
        uint64_t entry_fingerprint =
            edge_context_fingerprint(state);
        EdgeState then_state = {0};
        EdgeState else_state = {0};
        if (!edge_state_clone(&then_state, state) ||
            !edge_state_clone(&else_state, state)) {
            free(then_state.items);
            free(else_state.items);
            return false;
        }
        size_t then_start = *transition_cursor;
        if (!project_edges(
                op->branch.then_block, &then_state,
                transition_cursor, branch_cursor, projection) ||
            !edge_projection_push(
                projection, (QttSemanticCapabilityEdge){
                    .branch_ordinal = branch,
                    .arm = QTT_SEMANTIC_EDGE_THEN,
                    .transition_offset = then_start,
                    .transition_count =
                        *transition_cursor - then_start,
                    .entry_live_count = entry,
                    .exit_live_count = then_state.count,
                    .entry_context_fingerprint =
                        entry_fingerprint,
                    .exit_context_fingerprint =
                        edge_context_fingerprint(&then_state),
                })) {
            free(then_state.items);
            free(else_state.items);
            return false;
        }
        size_t else_start = *transition_cursor;
        if (!project_edges(
                op->branch.else_block, &else_state,
                transition_cursor, branch_cursor, projection) ||
            !edge_projection_push(
                projection, (QttSemanticCapabilityEdge){
                    .branch_ordinal = branch,
                    .arm = QTT_SEMANTIC_EDGE_ELSE,
                    .transition_offset = else_start,
                    .transition_count =
                        *transition_cursor - else_start,
                    .entry_live_count = entry,
                    .exit_live_count = else_state.count,
                    .entry_context_fingerprint =
                        entry_fingerprint,
                    .exit_context_fingerprint =
                        edge_context_fingerprint(&else_state),
                }) ||
            !edge_state_equal(&then_state, &else_state)) {
            free(then_state.items);
            free(else_state.items);
            return false;
        }
        free(state->items);
        *state = then_state;
        free(else_state.items);
    }
    return true;
}

static bool build_edges(QttSemanticFunction *function) {
    EdgeProjection projection = {0};
    EdgeState state = {0};
    size_t transition = 0, branch = 0;
    if (!project_edges(
            function->resources, &state, &transition,
            &branch, &projection)) {
        free(state.items);
        free(projection.items);
        return false;
    }
    free(state.items);
    function->edges = projection.items;
    function->edge_count = projection.count;
    return transition == function->transition_count;
}

static bool append_tree_evidence(
    QttSemanticFunction *function, const QttCoreNode *node,
    const QttCallableBinding *callables, size_t *cursor) {
    if (!node || !function || !cursor ||
        *cursor >= function->node_count)
        return false;
    size_t index = (*cursor)++;
    QttCoreUsageResult usage = qtt_core_usage_lower_lexical(
        function->grades, node, function->signatures,
        function->module_id, callables);
    bool valid = usage.status == QTT_CORE_USAGE_OK &&
        append_evidence(
            function, index, function->nodes[index].grade_role,
            usage.usage);
    qtt_usage_context_free(usage.usage);
    if (!valid) return false;
    for (size_t i = 0; i < child_count(node); i++) {
        const QttCallableBinding *child_environment = callables;
        QttCallableBinding binding = {0};
        if (node->kind == QTT_CORE_LET && i == 1) {
            const QttCoreNode *known = qtt_callable_env_value(
                callables, node->let.value);
            if (known) {
                binding = (QttCallableBinding){
                    .var = node->let.binding, .lambda = known,
                    .parent = callables};
                child_environment = &binding;
            }
        }
        if (!append_tree_evidence(
                function, child_at(node, i), child_environment,
                cursor))
            return false;
    }
    return true;
}

static QttSemanticFunction *semantic_ir_lower_common(
    const QttCoreNode *core, const QttSignatureEnv *signatures,
    uint64_t module_id, QttSemanticIrError *error) {
    if (error) *error = QTT_SEMANTIC_IR_INVALID_CORE;
    if (!core) return NULL;
    QttSemanticFunction *function = calloc(1, sizeof(*function));
    if (!function) {
        if (error) *error = QTT_SEMANTIC_IR_OUT_OF_MEMORY;
        return NULL;
    }
    function->grades = qtt_grade_arena_new();
    function->types = qtt_type_arena_new();
    function->effects = qtt_effect_arena_new();
    function->effect_solver =
        qtt_effect_solver_new(function->effects);
    function->pure_effect =
        qtt_effect_empty(function->effects);
    QttCoreUsageResult usage =
        qtt_core_usage_lower_in_env(
            function->grades, core, signatures, module_id);
    if (!function->grades || !function->types || !function->effects ||
        !function->effect_solver || !function->pure_effect ||
        usage.status != QTT_CORE_USAGE_OK) {
        if (error)
            *error = usage.status == QTT_CORE_USAGE_UNSUPPORTED
                ? QTT_SEMANTIC_IR_UNSUPPORTED
                : usage.status == QTT_CORE_USAGE_OUT_OF_MEMORY
                    ? QTT_SEMANTIC_IR_OUT_OF_MEMORY
                    : QTT_SEMANTIC_IR_INVALID_CORE;
        qtt_semantic_ir_free(function);
        return NULL;
    }
    function->source = core;
    function->signatures = signatures;
    if (module_id) function->module_id = module_id;
    function->usage = usage.usage;
    function->node_count = count_tree(core);
    function->nodes = calloc(
        function->node_count, sizeof(*function->nodes));
    if (!function->nodes) {
        if (error) *error = QTT_SEMANTIC_IR_OUT_OF_MEMORY;
        qtt_semantic_ir_free(function);
        return NULL;
    }
    size_t cursor = 0;
    bool valid = true;
    emit_tree(core, function, &cursor, &valid, NULL);
    size_t closure_node_cursor = 0;
    valid = valid && collect_closure_fields(
        function, core, &closure_node_cursor);
    size_t evidence_cursor = 0;
    valid = valid && append_tree_evidence(
        function, core, NULL, &evidence_cursor) &&
        evidence_cursor == function->node_count;
    function->resources = signatures
        ? qtt_resource_elaborate_lexical_in_env(
              core, signatures, module_id,
              &function->resource_status)
        : qtt_resource_elaborate_lexical(
              core, &function->resource_status);
    if (function->resources) {
        size_t branch_cursor = 0;
        valid = collect_destructors(function, core) &&
            collect_transitions(
                function, function->resources,
                NULL, 0, &branch_cursor) &&
            build_edges(function);
    }
    if (!valid) {
        if (error) *error = QTT_SEMANTIC_IR_INVALID_CORE;
        qtt_semantic_ir_free(function);
        return NULL;
    }
    if (error) *error = QTT_SEMANTIC_IR_OK;
    return function;
}

QttSemanticFunction *qtt_semantic_ir_lower(
    const QttCoreNode *core, QttSemanticIrError *error) {
    return semantic_ir_lower_common(core, NULL, 0, error);
}

static QttSemanticEffectJudgment semantic_effect_judgment(
    const QttCoreNode *core, const QttCoreEffectResult *effects) {
    return (QttSemanticEffectJudgment){
        .present = true,
        .authority_fingerprint =
            qtt_core_effect_authority_fingerprint(core),
        .row_fingerprint = effects->fingerprint,
        .constraint_fingerprint = effects->constraint_fingerprint,
        .constraint_count =
            qtt_effect_constraints_count(effects->constraints),
        .constraint_result = effects->constraint_result,
    };
}

QttSemanticFunction *qtt_semantic_ir_lower_certified(
    const QttCoreNode *core, const QttCoreEffectResult *effects,
    QttSemanticIrError *error) {
    if (!qtt_core_effect_verify_constraints ||
        !qtt_core_effect_authority_fingerprint ||
        !qtt_effect_constraints_count || !core || !effects ||
        effects->status != QTT_CORE_EFFECT_OK ||
        !effects->effects || !effects->fingerprint ||
        !effects->constraint_fingerprint ||
        !qtt_core_effect_verify_constraints(effects)) {
        if (error) *error = QTT_SEMANTIC_IR_INVALID_CORE;
        return NULL;
    }
    QttSemanticFunction *function =
        semantic_ir_lower_common(core, NULL, 0, error);
    if (!function) return NULL;
    function->effect_judgment =
        semantic_effect_judgment(core, effects);
    return function;
}

QttSemanticFunction *qtt_semantic_ir_lower_in_env(
    const QttCoreNode *core, const QttSignatureEnv *signatures,
    uint64_t module_id, QttSemanticIrError *error) {
    if (!signatures || !module_id) {
        if (error) *error = QTT_SEMANTIC_IR_INVALID_CORE;
        return NULL;
    }
    QttSemanticFunction *function =
        semantic_ir_lower_common(
            core, signatures, module_id, error);
    if (!function) return NULL;
    size_t node = 0, branch = 0, call = 0;
    if (!collect_calls(
            function, core, module_id, NULL, 0,
            &node, &branch, &call)) {
        if (error) *error = QTT_SEMANTIC_IR_UNSUPPORTED;
        qtt_semantic_ir_free(function);
        return NULL;
    }
    return function;
}

/* Declarative effect interpretation for the currently certified Core
 * fragment. Lambda construction is pure; direct application exposes the
 * latent body effect. Composition uses the closed-row least upper bound. */
static QttEffectRow *derive_effects(
    const QttSemanticFunction *function, const QttCoreNode *node,
    const QttCallableBinding *callables) {
    QttEffectRow *summary = qtt_effect_empty(function->effects);
    if (!summary) return NULL;
    if (node->kind == QTT_CORE_WRITE)
        summary = qtt_effect_write_place(
            function->effects, node->write.place, summary);
    else if (node->kind == QTT_CORE_PLACE)
        summary = qtt_effect_read_place(
            function->effects, node->place, summary);
    if (!summary || node->kind == QTT_CORE_LAMBDA) return summary;
    for (size_t i = 0; i < child_count(node); i++) {
        const QttCoreNode *child = child_at(node, i);
        const QttCallableBinding *child_environment = callables;
        QttCallableBinding binding = {0};
        if (node->kind == QTT_CORE_LET && i == 1) {
            const QttCoreNode *known = qtt_callable_env_value(
                callables, node->let.value);
            if (known) {
                binding = (QttCallableBinding){
                    .var = node->let.binding, .lambda = known,
                    .parent = callables};
                child_environment = &binding;
            }
        }
        QttEffectRow *child_summary = derive_effects(
            function, child, child_environment);
        if (!child_summary) return NULL;
        summary = qtt_effect_join_closed(
            function->effects, function->effect_solver,
            summary, child_summary);
        if (!summary) return NULL;
        if (node->kind == QTT_CORE_APPLY && i == 0 &&
            child->kind == QTT_CORE_LAMBDA) {
            QttEffectRow *latent = derive_effects(
                function, child->lambda.body, callables);
            if (!latent) return NULL;
            summary = qtt_effect_join_closed(
                function->effects, function->effect_solver,
                summary, latent);
            if (!summary) return NULL;
        }
    }
    if (node->kind == QTT_CORE_APPLY && node->apply.callee &&
        node->apply.callee->kind == QTT_CORE_GLOBAL) {
        QttEffectRow *latent = named_latent_effects(function, node);
        if (latent)
            summary = qtt_effect_join_closed(
                function->effects, function->effect_solver,
                summary, latent);
    }
    if (node->kind == QTT_CORE_APPLY && node->apply.callee &&
        node->apply.callee->kind == QTT_CORE_VAR) {
        const QttCoreNode *known = qtt_callable_env_lookup(
            callables, node->apply.callee->var);
        if (!known) return NULL;
        QttEffectRow *latent = derive_effects(
            function, known->lambda.body, callables);
        if (!latent) return NULL;
        summary = qtt_effect_join_closed(
            function->effects, function->effect_solver,
            summary, latent);
    }
    return summary;
}

static QttSemanticIrValidation verify_tree(
    const QttSemanticFunction *function, const QttCoreNode *node,
    QttGradeSolver *evidence_solver, size_t *cursor,
    const QttCallableBinding *callables) {
    if (*cursor >= function->node_count)
        return QTT_SEMANTIC_IR_SHAPE_MISMATCH;
    size_t index = (*cursor)++;
    const QttSemanticNode *actual = &function->nodes[index];
    if (actual->source != node) return QTT_SEMANTIC_IR_SOURCE_MISMATCH;
    if (actual->kind != node->kind) return QTT_SEMANTIC_IR_KIND_MISMATCH;
    QttCoreVar expected = node->kind == QTT_CORE_VAR ? node->var
        : node->kind == QTT_CORE_LET ? node->let.binding
        : node->kind == QTT_CORE_BORROW ? node->borrow.binding
        : node->kind == QTT_CORE_WRITE
            ? node->write.place.root
        : node->kind == QTT_CORE_PLACE ? node->place.root
        : (QttCoreVar){0};
    if (!qtt_core_var_equal(actual->var, expected))
        return QTT_SEMANTIC_IR_PROVENANCE_MISMATCH;
    QttPlace expected_place = node->kind == QTT_CORE_WRITE
        ? node->write.place
        : node->kind == QTT_CORE_BORROW ? node->borrow.place
        : node->kind == QTT_CORE_PLACE ? node->place
        : (QttPlace){0};
    if (!qtt_place_equal(actual->place, expected_place))
        return QTT_SEMANTIC_IR_PROVENANCE_MISMATCH;
    QttCoreVar expected_parent = node->kind == QTT_CORE_BORROW
        ? node->borrow.parent : (QttCoreVar){0};
    if (!qtt_core_var_equal(actual->parent_loan, expected_parent) ||
        (node->kind == QTT_CORE_BORROW &&
         actual->loan_kind != node->borrow.loan_kind) ||
        (node->kind != QTT_CORE_BORROW &&
         actual->loan_kind != QTT_LOAN_SHARED))
        return QTT_SEMANTIC_IR_PROVENANCE_MISMATCH;
    QttTypeIdentityError type_error = QTT_TYPE_IDENTITY_OK;
    QttTypeId expected_type =
        qtt_type_intern(function->types, node->type, &type_error);
    if (!expected_type.value ||
        !qtt_type_id_equal(actual->type_id, expected_type))
        return QTT_SEMANTIC_IR_TYPE_MISMATCH;
    QttRepresentation expected_representation =
        qtt_resource_classify_type(node->type);
    if (actual->representation != expected_representation)
        return QTT_SEMANTIC_IR_REPRESENTATION_MISMATCH;
    if (actual->capability !=
        capability_for(expected_representation))
        return QTT_SEMANTIC_IR_CAPABILITY_MISMATCH;
    uint64_t expected_effect_constructor = node->kind == QTT_CORE_PERFORM
        ? node->perform.constructor_id
        : node->kind == QTT_CORE_HANDLE && node->handle.selected_operation
            ? node->handle.selected_operation->perform.constructor_id : 0;
    uint64_t expected_effect_capability = node->kind == QTT_CORE_PERFORM
        ? node->perform.capability_id
        : node->kind == QTT_CORE_HANDLE && node->handle.selected_operation
            ? node->handle.selected_operation->perform.capability_id : 0;
    if (actual->effect_constructor_id != expected_effect_constructor ||
        actual->effect_capability_id != expected_effect_capability)
        return QTT_SEMANTIC_IR_EFFECT_AUTHORITY_MISMATCH;
    if (actual->grade_role != grade_role(node))
        return QTT_SEMANTIC_IR_GRADE_ROLE_MISMATCH;
    if ((node->kind == QTT_CORE_LAMBDA &&
         actual->closure_id != index + 1) ||
        (node->kind != QTT_CORE_LAMBDA && actual->closure_id) ||
        (node->kind == QTT_CORE_APPLY &&
         (!actual->instance_id ||
          actual->domain_count != node->apply.argument_count ||
          (node->apply.callee->kind == QTT_CORE_LAMBDA &&
           actual->callee_closure_id != index + 2))) ||
        (node->kind != QTT_CORE_APPLY &&
         (actual->instance_id || actual->callee_closure_id ||
          actual->domain_count)))
        return QTT_SEMANTIC_IR_CALL_EVIDENCE_MISMATCH;
    if (node->kind == QTT_CORE_LAMBDA) {
        bool policy_valid = function->signatures
            ? qtt_closure_policy_verify_in_env(
                function->source, node, function->signatures,
                function->module_id, actual->closure_policy)
            : qtt_closure_policy_verify(
                function->source, node, actual->closure_policy);
        if (!policy_valid)
            return QTT_SEMANTIC_IR_CLOSURE_POLICY_MISMATCH;
    } else if (actual->closure_policy.storage !=
                   QTT_CLOSURE_STORAGE_UNKNOWN ||
               actual->closure_policy.reason !=
                   QTT_CLOSURE_POLICY_NONE ||
               actual->closure_policy.occurrence_count ||
               actual->closure_policy.direct_callee_count ||
               actual->closure_policy.external_capture_use_count ||
               actual->closure_policy.live_after_capture_use_count ||
               actual->closure_policy.released_capture_count ||
               actual->closure_policy.moved_out_capture_count ||
               actual->closure_policy.unsupported_exit_capture_count) {
        return QTT_SEMANTIC_IR_CLOSURE_POLICY_MISMATCH;
    }
    if (!actual->effects)
        return QTT_SEMANTIC_IR_EFFECT_MISMATCH;
    QttEffectRow *expected_effects = derive_effects(
        function, node, callables);
    if (!expected_effects ||
        !qtt_effect_is_closed(function->effect_solver, actual->effects) ||
        !qtt_effect_rows_equal(
            function->effect_solver, actual->effects, expected_effects)) {
        return QTT_SEMANTIC_IR_EFFECT_MISMATCH;
    }
    if (actual->grade_offset + actual->grade_count >
            function->grade_evidence_count)
        return QTT_SEMANTIC_IR_GRADE_EVIDENCE_MISMATCH;
    QttGradeArena *expected_grades = qtt_grade_arena_new();
    if (!expected_grades)
        return QTT_SEMANTIC_IR_GRADE_EVIDENCE_MISMATCH;
    QttCoreUsageResult expected_usage = qtt_core_usage_lower_lexical(
        expected_grades, node, function->signatures,
        function->module_id, callables);
    if (expected_usage.status != QTT_CORE_USAGE_OK ||
        qtt_usage_binding_count(expected_usage.usage) !=
            actual->grade_count) {
        qtt_usage_context_free(expected_usage.usage);
        qtt_grade_arena_free(expected_grades);
        return QTT_SEMANTIC_IR_GRADE_EVIDENCE_MISMATCH;
    }
    QttGradeSolver *expected_solver =
        qtt_grade_solver_new(expected_grades);
    if (!expected_solver ||
        qtt_grade_solve(expected_solver) != QTT_GRADE_SOLVED) {
        qtt_grade_solver_free(expected_solver);
        qtt_usage_context_free(expected_usage.usage);
        qtt_grade_arena_free(expected_grades);
        return QTT_SEMANTIC_IR_GRADE_EVIDENCE_MISMATCH;
    }
    for (size_t i = 0; i < actual->grade_count; i++) {
        const QttSemanticGradeEvidence *evidence =
            &function->grade_evidence[actual->grade_offset + i];
        if (evidence->node_index != index ||
            evidence->module_id != function->module_id ||
            evidence->role != actual->grade_role ||
            !evidence->binder_id || !evidence->expression ||
            evidence->binder_id !=
                qtt_usage_binding_id(expected_usage.usage, i) ||
            !qtt_quantity_equal(
                qtt_grade_solution(
                    evidence_solver, evidence->expression),
                qtt_grade_solution(
                    expected_solver,
                    qtt_usage_binding_grade(expected_usage.usage, i)))) {
            qtt_grade_solver_free(expected_solver);
            qtt_usage_context_free(expected_usage.usage);
            qtt_grade_arena_free(expected_grades);
            return QTT_SEMANTIC_IR_GRADE_EVIDENCE_MISMATCH;
        }
    }
    qtt_grade_solver_free(expected_solver);
    qtt_usage_context_free(expected_usage.usage);
    qtt_grade_arena_free(expected_grades);
    size_t start = index;
    for (size_t i = 0; i < child_count(node); i++) {
        const QttCallableBinding *child_environment = callables;
        QttCallableBinding binding = {0};
        if (node->kind == QTT_CORE_LET && i == 1) {
            const QttCoreNode *known = qtt_callable_env_value(
                callables, node->let.value);
            if (known) {
                binding = (QttCallableBinding){
                    .var = node->let.binding, .lambda = known,
                    .parent = callables};
                child_environment = &binding;
            }
        }
        QttSemanticIrValidation checked =
            verify_tree(
                function, child_at(node, i),
                evidence_solver, cursor, child_environment);
        if (checked != QTT_SEMANTIC_IR_VALID) return checked;
    }
    if (actual->subtree_size != *cursor - start)
        return QTT_SEMANTIC_IR_SHAPE_MISMATCH;
    return QTT_SEMANTIC_IR_VALID;
}

static bool verify_destructors(
    const QttSemanticFunction *function,
    const QttCoreNode *node, size_t *cursor) {
    const Type *value_type = node->kind == QTT_CORE_LET
        ? semantic_expression_type(function, node->let.value)
        : NULL;
    if (node->kind == QTT_CORE_LET && value_type &&
        qtt_resource_classify_type(value_type) ==
            QTT_REP_OWNED_HEAP) {
        if (*cursor >= function->destructor_count)
            return false;
        const QttSemanticDestructorEvidence *evidence =
            &function->destructors[(*cursor)++];
        QttTypeIdentityError error = QTT_TYPE_IDENTITY_OK;
        QttTypeId expected = qtt_type_intern(
            function->types, value_type, &error);
        bool has_drop = resource_contains_drop(
            function->resources, node->let.binding);
        QttDropMaskError mask_error = QTT_DROP_MASK_VALID;
        QttDropMaskCertificate *expected_mask = has_drop
            ? qtt_drop_mask_build_for_resource(
                evidence->descriptor, function->resources,
                node->let.binding, NULL, 0, &mask_error)
            : NULL;
        bool mask_matches = has_drop
            ? expected_mask && drop_masks_equal(
                evidence->mask, expected_mask)
            : !evidence->mask;
        qtt_drop_mask_free(expected_mask);
        if (!qtt_core_var_equal(
                evidence->var, node->let.binding) ||
            !qtt_type_id_equal(evidence->type_id, expected) ||
            !qtt_destructor_descriptor_verify(
                evidence->descriptor, value_type) ||
            (has_drop &&
             (qtt_drop_mask_verify(
                  evidence->descriptor, evidence->mask) !=
                  QTT_DROP_MASK_VALID ||
              !qtt_place_equal(
                  evidence->mask->root,
                  qtt_place_root(node->let.binding)))) ||
            !mask_matches)
            return false;
    }
    for (size_t i = 0; i < child_count(node); i++)
        if (!verify_destructors(
                function, child_at(node, i), cursor))
            return false;
    return true;
}

static bool verify_transitions(
    const QttSemanticFunction *function,
    const QttResourceBlock *block, size_t *cursor,
    const QttControlPathStep *path, size_t path_count,
    size_t *branch_cursor) {
    for (size_t i = 0; i < block->count; i++) {
        const QttResourceOp *op = &block->ops[i];
        if (op->kind == QTT_RESOURCE_BRANCH) {
            size_t branch = (*branch_cursor)++;
            QttControlPathStep *then_path = malloc(
                (path_count + 1) * sizeof(*then_path));
            QttControlPathStep *else_path = malloc(
                (path_count + 1) * sizeof(*else_path));
            if (!then_path || !else_path) {
                free(then_path);
                free(else_path);
                return false;
            }
            if (path_count) {
                memcpy(then_path, path, path_count * sizeof(*path));
                memcpy(else_path, path, path_count * sizeof(*path));
            }
            then_path[path_count] = (QttControlPathStep){
                .branch_ordinal = branch, .else_arm = false};
            else_path[path_count] = (QttControlPathStep){
                .branch_ordinal = branch, .else_arm = true};
            bool valid =
                verify_transitions(
                    function, op->branch.then_block, cursor,
                    then_path, path_count + 1, branch_cursor) &&
                verify_transitions(
                    function, op->branch.else_block, cursor,
                    else_path, path_count + 1, branch_cursor);
            free(then_path);
            free(else_path);
            if (!valid) return false;
            continue;
        }
        if (*cursor >= function->transition_count)
            return false;
        const QttSemanticCapabilityTransition *actual =
            &function->transitions[*cursor];
        QttSemanticCapability before, after, target_after;
        transition_capabilities(
            op->kind, &before, &after, &target_after);
        if (actual->ordinal != *cursor ||
            actual->kind != op->kind ||
            !qtt_core_var_equal(actual->var, op->var) ||
            !qtt_core_var_equal(actual->target, op->target) ||
            !qtt_place_equal(actual->place, op->place) ||
            actual->representation != op->representation ||
            actual->loan_kind != op->loan_kind ||
            actual->before != before ||
            actual->after != after ||
            actual->target_after != target_after ||
            actual->control_path_count != path_count ||
            (path_count && !actual->control_path))
            return false;
        for (size_t step = 0; step < path_count; step++)
            if (actual->control_path[step].branch_ordinal !=
                    path[step].branch_ordinal ||
                actual->control_path[step].else_arm !=
                    path[step].else_arm)
                return false;
        (*cursor)++;
    }
    return true;
}

static bool verify_closure_fields(
    const QttSemanticFunction *function, const QttCoreNode *node,
    size_t *node_cursor, size_t *field_cursor) {
    if (!node) return false;
    size_t node_index = (*node_cursor)++;
    if (node->kind == QTT_CORE_LAMBDA) {
        QttCoreVar *captures = NULL;
        size_t capture_count = 0;
        if (qtt_core_lambda_captures(
                node, &captures, &capture_count) != QTT_CORE_OK)
            return false;
        for (size_t i = 0; i < capture_count; i++) {
            if (*field_cursor >= function->closure_field_count) {
                free(captures);
                return false;
            }
            const QttSemanticClosureFieldEvidence *actual =
                &function->closure_fields[(*field_cursor)++];
            const Type *type =
                qtt_core_var_type(node->lambda.body, captures[i]);
            QttTypeIdentityError type_error = QTT_TYPE_IDENTITY_OK;
            QttTypeId type_id =
                qtt_type_intern(function->types, type, &type_error);
            QttRepresentation representation =
                qtt_resource_classify_type(type);
            QttClosureStorage storage =
                function->nodes[node_index].closure_policy.storage;
            QttClosureFieldExit exit =
                storage == QTT_CLOSURE_STORAGE_UNIQUE
                    ? function->signatures
                        ? qtt_closure_capture_exit_in_env(
                            node, captures[i], function->signatures,
                            function->module_id)
                        : qtt_closure_capture_exit(node, captures[i])
                    : QTT_CLOSURE_FIELD_RELEASE;
            bool descriptor_valid =
                representation == QTT_REP_OWNED_HEAP
                    ? qtt_destructor_descriptor_verify(
                        actual->descriptor, type)
                    : actual->descriptor == NULL;
            if (actual->node_index != node_index ||
                actual->closure_id !=
                    function->nodes[node_index].closure_id ||
                actual->ordinal != i ||
                !qtt_core_var_equal(actual->source, captures[i]) ||
                !qtt_type_id_equal(actual->type_id, type_id) ||
                actual->type != type ||
                actual->representation != representation ||
                actual->storage != storage ||
                actual->exit != exit ||
                !descriptor_valid) {
                free(captures);
                return false;
            }
        }
        free(captures);
    }
    for (size_t i = 0; i < child_count(node); i++)
        if (!verify_closure_fields(
                function, child_at(node, i),
                node_cursor, field_cursor))
            return false;
    return true;
}

QttSemanticIrValidation qtt_semantic_ir_verify(
    const QttSemanticFunction *function, const QttCoreNode *core) {
    if (!function || function->source != core)
        return QTT_SEMANTIC_IR_SOURCE_MISMATCH;
    size_t closure_node_cursor = 0, closure_field_cursor = 0;
    if (!verify_closure_fields(
            function, core, &closure_node_cursor,
            &closure_field_cursor) ||
        closure_field_cursor != function->closure_field_count)
        return QTT_SEMANTIC_IR_CLOSURE_FIELD_MISMATCH;
    if (function->signatures) {
        QttSemanticFunction expected = {
            .signatures = function->signatures};
        size_t node = 0, branch = 0, call = 0;
        bool valid = collect_calls(
            &expected, core, function->module_id,
            NULL, 0, &node, &branch, &call);
        if (!valid || expected.call_count != function->call_count) {
            for (size_t i = 0; i < expected.call_count; i++)
                call_evidence_free(&expected.calls[i]);
            free(expected.calls);
            return QTT_SEMANTIC_IR_CALL_EVIDENCE_MISMATCH;
        }
        for (size_t i = 0; i < expected.call_count; i++)
            if (!call_evidence_equal(
                    &function->calls[i], &expected.calls[i]))
                valid = false;
        for (size_t i = 0; i < expected.call_count; i++)
            call_evidence_free(&expected.calls[i]);
        free(expected.calls);
        if (!valid)
            return QTT_SEMANTIC_IR_CALL_EVIDENCE_MISMATCH;
    } else if (function->call_count) {
        return QTT_SEMANTIC_IR_CALL_EVIDENCE_MISMATCH;
    }
    if (function->resources) {
        if (function->resource_status !=
                QTT_RESOURCE_ELABORATE_OK ||
            (function->signatures
                ? qtt_resource_verify_elaboration_in_env(
                      function->resources, core,
                      function->signatures, function->module_id)
                : qtt_resource_verify_elaboration(
                      function->resources, core)).error !=
                QTT_RESOURCE_VALID)
            return QTT_SEMANTIC_IR_RESOURCE_EVIDENCE_MISMATCH;
    } else {
        QttResourceElaborationError expected_status =
            QTT_RESOURCE_ELABORATE_OK;
        QttResourceBlock *unexpected = function->signatures
            ? qtt_resource_elaborate_lexical_in_env(
                  core, function->signatures,
                  function->module_id, &expected_status)
            : qtt_resource_elaborate_lexical(
                  core, &expected_status);
        qtt_resource_block_free(unexpected);
        if (unexpected ||
            expected_status != function->resource_status)
            return QTT_SEMANTIC_IR_RESOURCE_EVIDENCE_MISMATCH;
    }
    size_t destructor_cursor = 0;
    if (function->resources) {
        if (!verify_destructors(
                function, core, &destructor_cursor) ||
            destructor_cursor != function->destructor_count)
            return QTT_SEMANTIC_IR_DESTRUCTOR_EVIDENCE_MISMATCH;
    } else if (function->destructor_count) {
        /* Destructor masks are projections of the lexical resource proof.
         * An unsupported (and therefore absent) resource certificate carries
         * no destructor table; other Semantic IR evidence remains
         * independently verifiable. */
        return QTT_SEMANTIC_IR_DESTRUCTOR_EVIDENCE_MISMATCH;
    }
    size_t transition_cursor = 0, transition_branch = 0;
    if (function->resources &&
        (!verify_transitions(
             function, function->resources,
             &transition_cursor, NULL, 0,
             &transition_branch) ||
         transition_cursor != function->transition_count))
        return QTT_SEMANTIC_IR_CAPABILITY_TRANSITION_MISMATCH;
    if (!function->resources && function->transition_count)
        return QTT_SEMANTIC_IR_CAPABILITY_TRANSITION_MISMATCH;
    EdgeProjection expected_edges = {0};
    EdgeState edge_state = {0};
    size_t edge_transition = 0, edge_branch = 0;
    bool edges_valid = function->resources
        ? project_edges(
              function->resources, &edge_state, &edge_transition,
              &edge_branch, &expected_edges)
        : function->edge_count == 0;
    free(edge_state.items);
    if (!edges_valid ||
        expected_edges.count != function->edge_count) {
        free(expected_edges.items);
        return QTT_SEMANTIC_IR_CAPABILITY_EDGE_MISMATCH;
    }
    for (size_t i = 0; i < expected_edges.count; i++) {
        QttSemanticCapabilityEdge expected =
            expected_edges.items[i];
        QttSemanticCapabilityEdge actual = function->edges[i];
        if (actual.branch_ordinal != expected.branch_ordinal ||
            actual.arm != expected.arm ||
            actual.transition_offset != expected.transition_offset ||
            actual.transition_count != expected.transition_count ||
            actual.entry_live_count != expected.entry_live_count ||
            actual.exit_live_count != expected.exit_live_count ||
            actual.entry_context_fingerprint !=
                expected.entry_context_fingerprint ||
            actual.exit_context_fingerprint !=
                expected.exit_context_fingerprint) {
            free(expected_edges.items);
            return QTT_SEMANTIC_IR_CAPABILITY_EDGE_MISMATCH;
        }
    }
    free(expected_edges.items);
    QttGradeSolver *evidence_solver =
        qtt_grade_solver_new(function->grades);
    if (!evidence_solver ||
        qtt_grade_solve(evidence_solver) != QTT_GRADE_SOLVED) {
        qtt_grade_solver_free(evidence_solver);
        return QTT_SEMANTIC_IR_GRADE_EVIDENCE_MISMATCH;
    }
    size_t cursor = 0;
    QttSemanticIrValidation result =
        verify_tree(function, core, evidence_solver, &cursor, NULL);
    qtt_grade_solver_free(evidence_solver);
    return result == QTT_SEMANTIC_IR_VALID &&
               cursor != function->node_count
        ? QTT_SEMANTIC_IR_SHAPE_MISMATCH : result;
}

size_t qtt_semantic_ir_node_count(const QttSemanticFunction *function) {
    return function ? function->node_count : 0;
}

char *qtt_semantic_ir_effect_trace_format(
    const QttSemanticFunction *function) {
    if (!function) return NULL;
    size_t size = strlen("monad-effect-trace-v1\n") + 1;
    for (size_t i = 0; i < function->node_count; i++) {
        const QttSemanticNode *node = &function->nodes[i];
        const QttCoreNode *source = node->source;
        int length = 0;
        if (node->kind == QTT_CORE_PERFORM && source)
            length = snprintf(NULL, 0,
                "perform %s constructor=%llu capability=%llu\n",
                source->perform.effect_name
                    ? source->perform.effect_name : "<unknown>",
                (unsigned long long)node->effect_constructor_id,
                (unsigned long long)node->effect_capability_id);
        else if (node->kind == QTT_CORE_HANDLE && source)
            length = snprintf(NULL, 0,
                "handle %s capability=%llu\n",
                source->handle.profile_name
                    ? source->handle.profile_name : "<unknown>",
                (unsigned long long)source->handle.capability_id);
        if (length < 0 || (size_t)length > SIZE_MAX - size) return NULL;
        size += (size_t)length;
    }
    char *text = malloc(size);
    if (!text) return NULL;
    size_t used = (size_t)snprintf(
        text, size, "monad-effect-trace-v1\n");
    for (size_t i = 0; i < function->node_count; i++) {
        const QttSemanticNode *node = &function->nodes[i];
        const QttCoreNode *source = node->source;
        int length = 0;
        if (node->kind == QTT_CORE_PERFORM && source)
            length = snprintf(text + used, size - used,
                "perform %s constructor=%llu capability=%llu\n",
                source->perform.effect_name
                    ? source->perform.effect_name : "<unknown>",
                (unsigned long long)node->effect_constructor_id,
                (unsigned long long)node->effect_capability_id);
        else if (node->kind == QTT_CORE_HANDLE && source)
            length = snprintf(text + used, size - used,
                "handle %s capability=%llu\n",
                source->handle.profile_name
                    ? source->handle.profile_name : "<unknown>",
                (unsigned long long)source->handle.capability_id);
        if (length > 0) used += (size_t)length;
    }
    return text;
}
QttSemanticNode *qtt_semantic_ir_nodes(QttSemanticFunction *function) {
    return function ? function->nodes : NULL;
}
size_t qtt_semantic_ir_effect_label_count(
    const QttSemanticFunction *function, size_t node_index,
    const char *label) {
    if (!function || node_index >= function->node_count ||
        !label)
        return 0;
    return qtt_effect_label_count(
        function->effect_solver,
        function->nodes[node_index].effects, label);
}
size_t qtt_semantic_ir_evidence_count(
    const QttSemanticFunction *function) {
    return function ? function->grade_evidence_count : 0;
}
QttSemanticGradeEvidence *qtt_semantic_ir_evidence(
    QttSemanticFunction *function) {
    return function ? function->grade_evidence : NULL;
}
QttResourceBlock *qtt_semantic_ir_resources(
    QttSemanticFunction *function) {
    return function ? function->resources : NULL;
}
QttResourceElaborationError qtt_semantic_ir_resource_status(
    const QttSemanticFunction *function) {
    return function ? function->resource_status
                    : QTT_RESOURCE_ELABORATE_INVALID_RESOURCE_STATE;
}
size_t qtt_semantic_ir_destructor_count(
    const QttSemanticFunction *function) {
    return function ? function->destructor_count : 0;
}
QttSemanticDestructorEvidence *qtt_semantic_ir_destructors(
    QttSemanticFunction *function) {
    return function ? function->destructors : NULL;
}
size_t qtt_semantic_ir_transition_count(
    const QttSemanticFunction *function) {
    return function ? function->transition_count : 0;
}
QttSemanticCapabilityTransition *qtt_semantic_ir_transitions(
    QttSemanticFunction *function) {
    return function ? function->transitions : NULL;
}
size_t qtt_semantic_ir_edge_count(
    const QttSemanticFunction *function) {
    return function ? function->edge_count : 0;
}
QttSemanticCapabilityEdge *qtt_semantic_ir_edges(
    QttSemanticFunction *function) {
    return function ? function->edges : NULL;
}
size_t qtt_semantic_ir_call_count(
    const QttSemanticFunction *function) {
    return function ? function->call_count : 0;
}
QttSemanticCallEvidence *qtt_semantic_ir_calls(
    QttSemanticFunction *function) {
    return function ? function->calls : NULL;
}
size_t qtt_semantic_ir_closure_field_count(
    const QttSemanticFunction *function) {
    return function ? function->closure_field_count : 0;
}
QttSemanticClosureFieldEvidence *qtt_semantic_ir_closure_fields(
    QttSemanticFunction *function) {
    return function ? function->closure_fields : NULL;
}
QttQuantity qtt_semantic_ir_solved_usage(
    QttSemanticFunction *function, uint64_t binder_id) {
    if (!function) return qtt_quantity_omega();
    QttGradeExpr *expression =
        qtt_usage_grade(function->usage, binder_id);
    if (!expression) return qtt_quantity_finite(0);
    QttGradeSolver *solver =
        qtt_grade_solver_new(function->grades);
    if (!solver || qtt_grade_solve(solver) != QTT_GRADE_SOLVED) {
        qtt_grade_solver_free(solver);
        return qtt_quantity_omega();
    }
    QttQuantity result = qtt_grade_solution(solver, expression);
    qtt_grade_solver_free(solver);
    return result;
}
bool qtt_semantic_ir_solved_usage_for(
    QttSemanticFunction *function, QttCoreVar var,
    QttQuantity *quantity) {
    if (!function || !quantity || !var.module_id ||
        var.module_id != function->module_id)
        return false;
    *quantity = qtt_semantic_ir_solved_usage(
        function, var.binder_id);
    return true;
}
const QttUsageContext *qtt_semantic_ir_usage(
    const QttSemanticFunction *function) {
    return function ? function->usage : NULL;
}
const QttCoreNode *qtt_semantic_ir_source(
    const QttSemanticFunction *function) {
    return function ? function->source : NULL;
}
const QttSignatureEnv *qtt_semantic_ir_signatures(
    const QttSemanticFunction *function) {
    return function ? function->signatures : NULL;
}
uint64_t qtt_semantic_ir_module_id(
    const QttSemanticFunction *function) {
    return function ? function->module_id : 0;
}
const QttSemanticEffectJudgment *qtt_semantic_ir_effect_judgment(
    const QttSemanticFunction *function) {
    return function && function->effect_judgment.present
        ? &function->effect_judgment : NULL;
}
bool qtt_semantic_ir_verify_effect_judgment(
    const QttSemanticFunction *function, const QttCoreNode *core,
    const QttCoreEffectResult *effects) {
    if (!qtt_core_effect_verify_constraints ||
        !qtt_core_effect_authority_fingerprint ||
        !qtt_effect_constraints_count || !function || !core ||
        function->source != core || !effects ||
        effects->status != QTT_CORE_EFFECT_OK ||
        !qtt_core_effect_verify_constraints(effects))
        return false;
    QttSemanticEffectJudgment expected =
        semantic_effect_judgment(core, effects);
    const QttSemanticEffectJudgment *actual =
        &function->effect_judgment;
    return actual->present &&
        actual->authority_fingerprint == expected.authority_fingerprint &&
        actual->row_fingerprint == expected.row_fingerprint &&
        actual->constraint_fingerprint ==
            expected.constraint_fingerprint &&
        actual->constraint_count == expected.constraint_count &&
        actual->constraint_result == expected.constraint_result;
}
void qtt_semantic_ir_free(QttSemanticFunction *function) {
    if (!function) return;
    qtt_usage_context_free(function->usage);
    qtt_grade_arena_free(function->grades);
    qtt_type_arena_free(function->types);
    qtt_effect_solver_free(function->effect_solver);
    qtt_effect_arena_free(function->effects);
    qtt_resource_block_free(function->resources);
    for (size_t i = 0; i < function->destructor_count; i++)
        qtt_drop_mask_free(function->destructors[i].mask);
    for (size_t i = 0; i < function->destructor_count; i++)
        qtt_destructor_descriptor_free(
            function->destructors[i].descriptor);
    free(function->destructors);
    for (size_t i = 0; i < function->transition_count; i++)
        free(function->transitions[i].control_path);
    free(function->transitions);
    free(function->edges);
    for (size_t i = 0; i < function->call_count; i++)
        call_evidence_free(&function->calls[i]);
    free(function->calls);
    for (size_t i = 0; i < function->closure_field_count; i++)
        qtt_destructor_descriptor_free(
            function->closure_fields[i].descriptor);
    free(function->closure_fields);
    free(function->nodes);
    free(function->grade_evidence);
    free(function);
}
