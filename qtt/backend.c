#include "backend.h"

#include "bindings.h"
#include "anf.h"
#include "semantic_ir.h"
#include "signature_env.h"
#include "compiler.h"
#include "core_effect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

QttBackendEffectStatus qtt_backend_certify_abortive_handler(
    const QttCoreNode *handle, const QttCoreEffectResult *effects,
    QttBackendAbortiveHandlerPlan *plan) {
    if (plan) memset(plan, 0, sizeof(*plan));
    if (!handle || handle->kind != QTT_CORE_HANDLE || !effects || !plan ||
        !handle->handle.computation || !handle->handle.selected_operation ||
        handle->handle.selected_operation->kind != QTT_CORE_PERFORM ||
        !handle->handle.clause)
        return QTT_BACKEND_EFFECT_INVALID_CORE;
    if (qtt_core_effect_runtime_policy(handle) !=
        QTT_CORE_EFFECT_RUNTIME_ABORTIVE_DIRECT)
        return QTT_BACKEND_EFFECT_UNSUPPORTED_POLICY;
    if (!handle->handle.abortive_context_safe)
        return QTT_BACKEND_EFFECT_EFFECTFUL_CONTEXT;
    if (!qtt_core_effect_verify_handle(handle, effects))
        return QTT_BACKEND_EFFECT_INVALID_PROOF;
    if (!qtt_core_effect_verify_constraints(effects))
        return QTT_BACKEND_EFFECT_INVALID_PROOF;
    QttEffectHandlerProof proof = {0};
    if (!qtt_effect_handler_proof_deserialize(
            handle->handle.portable_proof, &proof))
        return QTT_BACKEND_EFFECT_INVALID_PROOF;
    const QttCoreNode *operation = handle->handle.selected_operation;
    if (!operation->perform.constructor_id ||
        !operation->perform.capability_id ||
        !qtt_quantity_is_zero(operation->perform.resumption))
        return QTT_BACKEND_EFFECT_INVALID_CORE;
    plan->constructor_id = operation->perform.constructor_id;
    plan->capability_id = operation->perform.capability_id;
    plan->authority_fingerprint =
        qtt_core_effect_authority_fingerprint(handle);
    plan->proof_fingerprint = proof.proof_fingerprint;
    plan->constraint_fingerprint = effects->constraint_fingerprint;
    plan->operation = operation;
    plan->clause = handle->handle.clause;
    return plan->authority_fingerprint && plan->proof_fingerprint &&
            plan->constraint_fingerprint
        ? QTT_BACKEND_EFFECT_OK : QTT_BACKEND_EFFECT_INVALID_PROOF;
}

QttEffectDispatchResult qtt_backend_execute_abortive_handler(
    const QttCoreNode *handle, const QttCoreEffectResult *effects,
    const QttBackendAbortiveHandlerPlan *plan,
    QttEffectRuntime *runtime, QttAbortiveEffectClause clause,
    void *environment, void *argument, void **output) {
    if (!plan || !runtime || !clause || !output)
        return QTT_EFFECT_DISPATCH_INVALID;
    QttBackendAbortiveHandlerPlan replay = {0};
    if (qtt_backend_certify_abortive_handler(
            handle, effects, &replay) != QTT_BACKEND_EFFECT_OK ||
        replay.constructor_id != plan->constructor_id ||
        replay.capability_id != plan->capability_id ||
        replay.authority_fingerprint != plan->authority_fingerprint ||
        replay.proof_fingerprint != plan->proof_fingerprint ||
        replay.constraint_fingerprint != plan->constraint_fingerprint ||
        replay.operation != plan->operation || replay.clause != plan->clause)
        return QTT_EFFECT_DISPATCH_INVALID;
    QttEffectHandlerFrame frame = {0};
    if (!qtt_effect_runtime_push(
            runtime, &frame, plan->constructor_id, plan->capability_id,
            clause, environment))
        return QTT_EFFECT_DISPATCH_INVALID;
    QttEffectDispatchResult dispatched = qtt_effect_runtime_perform_abortive(
        runtime, plan->constructor_id, plan->capability_id, argument, output);
    if (!qtt_effect_runtime_pop(runtime, &frame))
        return QTT_EFFECT_DISPATCH_INVALID;
    return dispatched;
}

void qtt_backend_cleanup_plan_free(QttBackendCleanupPlan *cleanups) {
    if (!cleanups) return;
    for (size_t i = 0; i < cleanups->count; i++)
        free(cleanups->items[i].evacuated);
    free(cleanups->items);
    memset(cleanups, 0, sizeof(*cleanups));
}

void qtt_backend_replacement_plan_free(
    QttBackendReplacementPlan *replacements) {
    if (!replacements) return;
    free(replacements->items);
    memset(replacements, 0, sizeof(*replacements));
}

static QttSemanticDestructorEvidence *find_destructor(
    QttSemanticDestructorEvidence *destructors,
    size_t destructor_count, QttCoreVar resource) {
    for (size_t i = 0; i < destructor_count; i++) {
        if (qtt_core_var_equal(destructors[i].var, resource))
            return &destructors[i];
    }
    return NULL;
}

static bool backend_accepts_destructor(
    const QttSemanticDestructorEvidence *evidence,
    const Type *type, bool final_drop) {
    if (!evidence || !evidence->descriptor ||
        !qtt_destructor_descriptor_verify(evidence->descriptor, type))
        return false;
    if (!final_drop)
        return !evidence->mask ||
            (qtt_drop_mask_verify(
                 evidence->descriptor, evidence->mask) ==
                 QTT_DROP_MASK_VALID &&
             evidence->mask->evacuated_count == 0);
    if (!evidence->mask ||
        qtt_drop_mask_verify(evidence->descriptor, evidence->mask) !=
            QTT_DROP_MASK_VALID)
        return false;
    if (evidence->mask->evacuated_count == 0) return true;
    if (evidence->descriptor->plan->kind != QTT_DROP_STRUCT)
        return false;
    for (size_t i = 0; i < evidence->descriptor->plan->child_count; i++) {
        QttDropKind kind = evidence->descriptor->plan->children[i]->kind;
        if (kind != QTT_DROP_NOOP && kind != QTT_DROP_OWNED_LEAF)
            return false;
    }
    for (size_t i = 0; i < evidence->mask->evacuated_count; i++)
        if (evidence->mask->evacuated[i].projection_depth != 1)
            return false;
    return true;
}

static bool reject_cleanup(const char *reason) {
    if (qtt_compiler_trace_detailed())
        printf("[qtt] cleanup fallback: %s\n", reason);
    return false;
}

static bool initializer_contract(
    AST *value, const QttSignatureEnv *signatures,
    uint64_t module_id, QttBackendMaterialization *materialization,
    const Type **result_type) {
    if (value && value->type == AST_STRING) {
        *materialization = QTT_BACKEND_MATERIALIZE_STATIC_COPY;
        *result_type = value->inferred_type;
        return *result_type != NULL;
    }
    if (value && value->type == AST_LIST && value->list.count &&
        value->list.items[0] &&
        value->list.items[0]->type == AST_SYMBOL) {
        const Type *layout = qtt_bindings_layout_type(
            value->list.items[0]->symbol);
        if (layout && layout->kind == TYPE_LAYOUT &&
            layout->layout_field_count > 0 &&
            value->list.count == (size_t)layout->layout_field_count + 1) {
            for (size_t i = 1; i < value->list.count; i++)
                if (!value->list.items[i] ||
                    (value->list.items[i]->type != AST_STRING &&
                     value->list.items[i]->type != AST_NUMBER &&
                     value->list.items[i]->type != AST_CHAR))
                    return false;
            *materialization = QTT_BACKEND_MATERIALIZE_LAYOUT_LITERAL;
            *result_type = layout;
            return true;
        }
    }
    if (!value || value->type != AST_LIST ||
        value->list.count == 0 || !signatures)
        return false;
    AST *callee = value->list.items[0];
    if (!callee || callee->type != AST_SYMBOL) return false;
    QttFunctionSignature *signature = NULL;
    if (!qtt_signature_env_resolve(
            signatures, module_id, callee->symbol,
            NULL, &signature) ||
        !signature ||
        signature->parameter_count + 1 != value->list.count ||
        signature->result.mode != QTT_RESULT_OWNED ||
        signature->result.origin != QTT_RESULT_ORIGIN_FRESH ||
        !signature->result.type ||
        signature->result.type->kind != TYPE_STRING ||
        signature->result.representation != QTT_REP_OWNED_HEAP)
        return false;
    *materialization = QTT_BACKEND_MATERIALIZE_OWNED_RESULT;
    *result_type = signature->result.type;
    return true;
}

typedef struct {
    AST *name;
    AST *value;
} LifetimeBinding;

typedef struct {
    LifetimeBinding *items;
    size_t count;
    size_t capacity;
} LifetimeBindings;

static bool collect_lifetime_bindings(AST *node, LifetimeBindings *out) {
    if (!node) return true;
    if (node->type == AST_LIST) {
        bool lexical = node->list.count >= 3 && node->list.items[0] &&
            node->list.items[0]->type == AST_SYMBOL &&
            (!strcmp(node->list.items[0]->symbol, "with") ||
             !strcmp(node->list.items[0]->symbol, "let")) &&
            node->list.items[1] && node->list.items[1]->type == AST_ARRAY;
        if (lexical) {
            AST *bindings = node->list.items[1];
            for (size_t i = 0; i + 1 < bindings->array.element_count;
                 i += 2) {
                if (out->count == out->capacity) {
                    size_t capacity = out->capacity ? out->capacity * 2 : 8;
                    LifetimeBinding *grown = realloc(
                        out->items, capacity * sizeof(*grown));
                    if (!grown) return false;
                    out->items = grown;
                    out->capacity = capacity;
                }
                out->items[out->count++] = (LifetimeBinding){
                    .name = bindings->array.elements[i],
                    .value = bindings->array.elements[i + 1],
                };
                if (!collect_lifetime_bindings(
                        bindings->array.elements[i + 1], out))
                    return false;
            }
            for (size_t i = 2; i < node->list.count; i++)
                if (!collect_lifetime_bindings(node->list.items[i], out))
                    return false;
            return true;
        }
        for (size_t i = 0; i < node->list.count; i++)
            if (!collect_lifetime_bindings(node->list.items[i], out))
                return false;
    } else if (node->type == AST_ARRAY) {
        for (size_t i = 0; i < node->array.element_count; i++)
            if (!collect_lifetime_bindings(node->array.elements[i], out))
                return false;
    } else if (node->type == AST_LAMBDA) {
        for (int i = 0; i < node->lambda.body_count; i++)
            if (!collect_lifetime_bindings(node->lambda.body_exprs[i], out))
                return false;
    }
    return true;
}

static const QttResourceOp *find_resource_op(
    const QttResourceBlock *block, QttResourceOpKind kind, QttCoreVar var) {
    if (!block) return NULL;
    for (size_t i = 0; i < block->count; i++) {
        const QttResourceOp *op = &block->ops[i];
        if (op->kind == kind && qtt_core_var_equal(op->var, var))
            return op;
        if (op->kind == QTT_RESOURCE_BRANCH) {
            const QttResourceOp *found = find_resource_op(
                op->branch.then_block, kind, var);
            if (!found)
                found = find_resource_op(op->branch.else_block, kind, var);
            if (found) return found;
        }
    }
    return NULL;
}

static bool anf_contains_drop(
    const QttAnfProgram *program, QttCoreVar var) {
    if (!program) return false;
    for (size_t block = 0; block < program->block_count; block++)
        for (size_t i = 0;
             i < program->blocks[block].instruction_count; i++) {
            const QttAnfInstruction *op =
                &program->blocks[block].instructions[i];
            if (op->kind == QTT_ANF_DROP &&
                qtt_core_var_equal(op->resource, var))
                return true;
        }
    return false;
}

static const QttAnfInstruction *anf_find_alloc(
    const QttAnfProgram *program, QttCoreVar var) {
    if (!program) return NULL;
    for (size_t block = 0; block < program->block_count; block++)
        for (size_t i = 0;
             i < program->blocks[block].instruction_count; i++) {
            const QttAnfInstruction *op =
                &program->blocks[block].instructions[i];
            if (op->kind == QTT_ANF_ALLOC &&
                qtt_core_var_equal(op->resource, var))
                return op;
        }
    return NULL;
}

static QttAnfType anf_value_type(
    const QttAnfProgram *program, QttAnfValue value) {
    if (!program || !value) return QTT_ANF_TYPE_UNKNOWN;
    for (size_t block = 0; block < program->block_count; block++)
        for (size_t i = 0;
             i < program->blocks[block].instruction_count; i++)
            if (program->blocks[block].instructions[i].result == value)
                return program->blocks[block].instructions[i].result_type;
    return QTT_ANF_TYPE_UNKNOWN;
}

static const QttCoreNode *find_let_value(
    const QttCoreNode *node, QttCoreVar binding) {
    if (!node) return NULL;
    if (node->kind == QTT_CORE_LET &&
        qtt_core_var_equal(node->let.binding, binding))
        return node->let.value;
    switch (node->kind) {
    case QTT_CORE_LAMBDA:
        return find_let_value(node->lambda.body, binding);
    case QTT_CORE_APPLY: {
        const QttCoreNode *found = find_let_value(
            node->apply.callee, binding);
        for (size_t i = 0; !found && i < node->apply.argument_count; i++)
            found = find_let_value(node->apply.arguments[i], binding);
        return found;
    }
    case QTT_CORE_LET: {
        const QttCoreNode *found = find_let_value(node->let.value, binding);
        return found ? found : find_let_value(node->let.body, binding);
    }
    case QTT_CORE_IF: {
        const QttCoreNode *found = find_let_value(
            node->conditional.condition, binding);
        if (!found) found = find_let_value(
            node->conditional.then_branch, binding);
        return found ? found : find_let_value(
            node->conditional.else_branch, binding);
    }
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < node->sequence.count; i++) {
            const QttCoreNode *found = find_let_value(
                node->sequence.items[i], binding);
            if (found) return found;
        }
        return NULL;
    case QTT_CORE_WRITE:
        return find_let_value(node->write.value, binding);
    default:
        return NULL;
    }
}

static const QttSemanticNode *find_semantic_node(
    QttSemanticFunction *function, const QttCoreNode *source) {
    QttSemanticNode *nodes = qtt_semantic_ir_nodes(function);
    size_t count = qtt_semantic_ir_node_count(function);
    for (size_t i = 0; nodes && i < count; i++)
        if (nodes[i].source == source) return &nodes[i];
    return NULL;
}

static bool certify_unique_string_closure(
    QttSemanticFunction *semantic, const QttCoreNode *lambda,
    QttBackendCleanup *item) {
    if (!semantic || !lambda || lambda->kind != QTT_CORE_LAMBDA || !item)
        return false;
    const QttSemanticNode *node = find_semantic_node(semantic, lambda);
    if (!node || node->closure_policy.storage != QTT_CLOSURE_STORAGE_UNIQUE ||
        node->closure_policy.reason != QTT_CLOSURE_POLICY_SINGLE_DIRECT_CALL)
        return false;
    QttSemanticClosureFieldEvidence *fields =
        qtt_semantic_ir_closure_fields(semantic);
    size_t field_count = qtt_semantic_ir_closure_field_count(semantic);
    size_t matched = 0;
    uint64_t moved = 0;
    QttDestructorId witness = {0};
    for (size_t i = 0; i < field_count; i++) {
        QttSemanticClosureFieldEvidence *field = &fields[i];
        if (field->closure_id != node->closure_id) continue;
        if (field->ordinal >= 64 || field->representation != QTT_REP_OWNED_HEAP ||
            !field->type || field->type->kind != TYPE_STRING ||
            !field->descriptor ||
            !qtt_destructor_descriptor_verify(field->descriptor, field->type) ||
            (field->exit != QTT_CLOSURE_FIELD_MOVE_OUT &&
             field->exit != QTT_CLOSURE_FIELD_RELEASE))
            return false;
        if (!matched) witness = field->descriptor->id;
        if (field->exit == QTT_CLOSURE_FIELD_MOVE_OUT)
            moved |= UINT64_C(1) << field->ordinal;
        matched++;
    }
    if (!matched || matched != node->closure_policy.moved_out_capture_count +
                                node->closure_policy.released_capture_count)
        return false;
    item->destructor = witness;
    item->representation = QTT_REP_INLINE;
    item->materialization = QTT_BACKEND_MATERIALIZE_UNIQUE_CLOSURE;
    item->destroy = true;
    item->closure_moved_mask = moved;
    return true;
}

bool qtt_backend_certify_string_lifetimes_in_env(
    AST *source, const QttSignatureEnv *signatures, uint64_t module_id,
    QttBackendCleanupPlan *lifetimes) {
    if (lifetimes) memset(lifetimes, 0, sizeof(*lifetimes));
    if (!source || !module_id) return false;
    LifetimeBindings bindings = {0};
    if (!collect_lifetime_bindings(source, &bindings) || !bindings.count) {
        free(bindings.items);
        return false;
    }
    bool lifetime_candidate = false;
    for (size_t i = 0; i < bindings.count; i++) {
        AST *value = bindings.items[i].value;
        QttBackendMaterialization materialization;
        const Type *type = NULL;
        bool direct = initializer_contract(
            value, signatures, module_id, &materialization, &type);
        if ((value && value->type == AST_LAMBDA) ||
            (value && value->type == AST_LIST && !direct)) {
            lifetime_candidate = true;
            break;
        }
    }
    if (!lifetime_candidate) {
        free(bindings.items);
        return false;
    }
    QttCoreError core_error = QTT_CORE_OK;
    QttCoreNode *core = qtt_core_lower(source, module_id, &core_error);
    QttSemanticIrError ir_error = QTT_SEMANTIC_IR_OK;
    QttSemanticFunction *semantic = core
        ? qtt_semantic_ir_lower_in_env(
              core, signatures, module_id, &ir_error)
        : NULL;
    QttSemanticIrValidation semantic_validation = semantic
        ? qtt_semantic_ir_verify(semantic, core)
        : QTT_SEMANTIC_IR_SOURCE_MISMATCH;
    bool valid = semantic && ir_error == QTT_SEMANTIC_IR_OK &&
        semantic_validation == QTT_SEMANTIC_IR_VALID;
    bool resource_valid = valid &&
        qtt_semantic_ir_resource_status(semantic) ==
            QTT_RESOURCE_ELABORATE_OK;
    QttAnfLowerError anf_error = QTT_ANF_LOWER_OK;
    QttAnfProgram *anf = valid
        ? qtt_anf_lower_core(core, &anf_error) : NULL;
    valid = valid && anf && anf_error == QTT_ANF_LOWER_OK &&
        qtt_anf_verify(anf).error == QTT_ANF_VALID;
    if (!valid && qtt_compiler_trace_detailed())
        printf("[qtt] lifetime certificate boundary: semantic=%s(%d) "
               "resource=%s anf=%s\n",
               semantic && ir_error == QTT_SEMANTIC_IR_OK &&
                       semantic_validation == QTT_SEMANTIC_IR_VALID
                   ? "verified" : "unverified",
               (int)semantic_validation,
               resource_valid ? "valid" : "unsupported",
               anf && anf_error == QTT_ANF_LOWER_OK
                   ? "available" : "unavailable");
    QttResourceBlock *resources = resource_valid
        ? qtt_semantic_ir_resources(semantic) : NULL;
    QttSemanticDestructorEvidence *destructors = resource_valid
        ? qtt_semantic_ir_destructors(semantic) : NULL;
    size_t destructor_count = resource_valid
        ? qtt_semantic_ir_destructor_count(semantic) : 0;
    QttBackendCleanup *items = valid
        ? calloc(bindings.count, sizeof(*items)) : NULL;
    valid = valid && items;
    size_t count = 0;
    bool has_cross_scope_drop = false;
    for (size_t i = 0; valid && i < bindings.count; i++) {
        AST *name = bindings.items[i].name;
        AST *value = bindings.items[i].value;
        if (!name || name->type != AST_SYMBOL ||
            !name->resolved_binder_id)
            continue;
        QttCoreVar var = {module_id, name->resolved_binder_id};
        const QttCoreNode *let_value = find_let_value(core, var);
        if (value && value->type == AST_LAMBDA &&
            certify_unique_string_closure(
                semantic, let_value, &items[count])) {
            items[count].resource = var;
            count++;
            continue;
        }
        const QttResourceOp *allocation = find_resource_op(
            resources, QTT_RESOURCE_ALLOC, var);
        const QttAnfInstruction *anf_allocation =
            anf_find_alloc(anf, var);
        QttRepresentation representation = allocation
            ? allocation->representation
            : anf_allocation ? anf_allocation->representation
                             : QTT_REP_UNKNOWN;
        if (representation != QTT_REP_OWNED_HEAP)
            continue;
        QttSemanticDestructorEvidence *destructor = find_destructor(
            destructors, destructor_count, var);
        static Type string_type = {.kind = TYPE_STRING};
        QttDestructorDescriptor *anf_descriptor = NULL;
        if ((!destructor || !destructor->descriptor) && anf_allocation &&
            anf_value_type(anf, anf_allocation->operand) ==
                QTT_ANF_TYPE_STRING) {
            QttDropPlanError drop_error = QTT_DROP_PLAN_OK;
            anf_descriptor = qtt_destructor_descriptor_build(
                &string_type, &drop_error);
        }
        const QttDestructorDescriptor *descriptor = destructor &&
                destructor->descriptor
            ? destructor->descriptor : anf_descriptor;
        if (!descriptor || descriptor->plan->kind != QTT_DROP_OWNED_LEAF) {
            qtt_destructor_descriptor_free(anf_descriptor);
            valid = false;
            break;
        }
        QttBackendMaterialization materialization =
            QTT_BACKEND_MATERIALIZE_OWNED_RESULT;
        const Type *initializer_type = NULL;
        bool explicit_materialization = initializer_contract(
            value, signatures, module_id,
            &materialization, &initializer_type);
        bool call_result = value && value->type == AST_LIST;
        if (!explicit_materialization && !call_result) {
            valid = false;
            break;
        }
        bool destroy = anf_contains_drop(anf, var);
        if (destructor && !backend_accepts_destructor(
                destructor,
                initializer_type ? initializer_type : name->inferred_type,
                destroy)) {
            /* Stale source call types are not identity evidence.  Semantic
             * IR already tied an owned-leaf descriptor to this BinderId. */
            if (explicit_materialization || !call_result ||
                (destroy && descriptor->plan->kind !=
                    QTT_DROP_OWNED_LEAF)) {
                valid = false;
                qtt_destructor_descriptor_free(anf_descriptor);
                break;
            }
        }
        items[count++] = (QttBackendCleanup){
            .resource = var,
            .destructor = descriptor->id,
            .representation = representation,
            .materialization = materialization,
            .destroy = destroy,
            .evacuated_count = destructor && destructor->mask
                ? destructor->mask->evacuated_count : 0,
        };
        has_cross_scope_drop |=
            !explicit_materialization && call_result && destroy;
        qtt_destructor_descriptor_free(anf_descriptor);
    }
    if (!count || !has_cross_scope_drop) valid = false;
    if (valid && lifetimes) {
        lifetimes->items = items;
        lifetimes->count = count;
        items = NULL;
        if (qtt_compiler_trace_detailed())
            for (size_t i = 0; i < count; i++)
                printf("[qtt] lifetime κ#%llu materialization=%s "
                       "destroy=%s\n",
                       (unsigned long long)
                           lifetimes->items[i].resource.binder_id,
                       lifetimes->items[i].materialization ==
                               QTT_BACKEND_MATERIALIZE_STATIC_COPY
                           ? "static-copy"
                           : lifetimes->items[i].materialization ==
                                     QTT_BACKEND_MATERIALIZE_UNIQUE_CLOSURE
                               ? "unique-closure" : "owned-result",
                       lifetimes->items[i].destroy ? "yes" : "no");
    }
    free(items);
    free(bindings.items);
    qtt_semantic_ir_free(semantic);
    qtt_anf_program_free(anf);
    qtt_core_free(core);
    if (!valid && qtt_compiler_trace_detailed())
        printf("[qtt] lifetime fallback: bindings=%zu certified=%zu "
               "core=%d semantic=%d anf=%d\n",
               bindings.count, count, (int)core_error,
               (int)ir_error, (int)anf_error);
    return valid;
}

bool qtt_backend_certify_string_cleanups_in_env(
    AST *source, const QttSignatureEnv *signatures,
    uint64_t module_id,
    QttBackendCleanupPlan *cleanups) {
    if (cleanups) memset(cleanups, 0, sizeof(*cleanups));
    if (!source || !module_id || source->type != AST_LIST ||
        source->list.count < 3)
        return reject_cleanup("unsupported lexical source shape");
    AST *binding = source->list.items[1];
    if (!binding || binding->type != AST_ARRAY ||
        binding->array.element_count == 0 ||
        (binding->array.element_count % 2) != 0)
        return reject_cleanup("malformed lexical bindings");
    size_t binding_count = binding->array.element_count / 2;
    QttBackendMaterialization *materializations =
        calloc(binding_count, sizeof(*materializations));
    const Type **initializer_types =
        calloc(binding_count, sizeof(*initializer_types));
    bool *candidates = calloc(binding_count, sizeof(*candidates));
    if (!materializations || !initializer_types || !candidates) {
        free(materializations);
        free(initializer_types);
        free(candidates);
        return reject_cleanup("out of memory for materialization evidence");
    }
    size_t candidate_count = 0;
    for (size_t i = 0; i < binding_count; i++) {
        AST *name = binding->array.elements[i * 2];
        AST *value = binding->array.elements[i * 2 + 1];
        if (!name || name->type != AST_SYMBOL) {
            free(materializations);
            free(initializer_types);
            free(candidates);
            return reject_cleanup("lexical binder is not a symbol");
        }
        candidates[i] = initializer_contract(
            value, signatures, module_id,
            &materializations[i], &initializer_types[i]);
        if (candidates[i]) candidate_count++;
    }
    if (candidate_count == 0) {
        free(materializations);
        free(initializer_types);
        free(candidates);
        return reject_cleanup("no fresh owned String binding");
    }

    QttCoreError core_error = QTT_CORE_OK;
    QttCoreNode *core =
        qtt_core_lower(source, module_id, &core_error);
    if (!core) {
        free(materializations);
        free(initializer_types);
        free(candidates);
        return reject_cleanup("source did not lower to typed Core");
    }
    QttSemanticIrError ir_error = QTT_SEMANTIC_IR_OK;
    QttSemanticFunction *semantic = signatures
        ? qtt_semantic_ir_lower_in_env(
              core, signatures, module_id, &ir_error)
        : qtt_semantic_ir_lower(core, &ir_error);
    bool valid = semantic &&
        ir_error == QTT_SEMANTIC_IR_OK &&
        qtt_semantic_ir_verify(semantic, core) ==
            QTT_SEMANTIC_IR_VALID &&
        qtt_semantic_ir_resource_status(semantic) ==
            QTT_RESOURCE_ELABORATE_OK &&
        qtt_semantic_ir_destructor_count(semantic) >= candidate_count;
    QttAnfLowerError anf_error = QTT_ANF_LOWER_OK;
    QttAnfProgram *anf = valid
        ? qtt_anf_lower_core_in_env(
              core, signatures, module_id, &anf_error)
        : NULL;
    bool anf_valid = anf && anf_error == QTT_ANF_LOWER_OK &&
        qtt_anf_verify(anf).error == QTT_ANF_VALID;
    bool all_anf_items_match = anf_valid;
    QttResourceBlock *plan =
        valid ? qtt_semantic_ir_resources(semantic) : NULL;
    QttSemanticDestructorEvidence *destructors =
        valid ? qtt_semantic_ir_destructors(semantic) : NULL;
    size_t destructor_count = valid
        ? qtt_semantic_ir_destructor_count(semantic) : 0;
    valid = valid && plan && destructors &&
        plan->count >= candidate_count * 2;
    if (!valid && qtt_compiler_trace_detailed())
        printf("[qtt] cleanup fallback: certified ownership evidence "
               "unavailable (ir=%d resource=%d anf=%d)\n",
               (int)ir_error,
               semantic
                   ? (int)qtt_semantic_ir_resource_status(semantic)
                   : -1, (int)anf_error);

    QttBackendCleanup *items = valid
        ? calloc(candidate_count, sizeof(*items)) : NULL;
    valid = valid && items;
    /*
     * Every allocation for a direct binder in this lexical scope must have a
     * backend materialization contract.  Allocations for nested lexical
     * scopes are certified independently when those scopes are emitted.
     * BORROW operations for aliases may occur between allocation and drops.
     */
    for (size_t i = 0; valid && i < plan->count; i++) {
        QttResourceOp *op = &plan->ops[i];
        if (op->kind != QTT_RESOURCE_ALLOC) continue;
        bool direct = false;
        bool accounted = false;
        for (size_t j = 0; j < binding_count; j++) {
            AST *name = binding->array.elements[j * 2];
            if (name->resolved_binder_id != op->var.binder_id ||
                op->var.module_id != module_id)
                continue;
            direct = true;
            accounted = candidates[j];
            break;
        }
        valid = !direct || accounted;
    }
    size_t cleanup_index = 0;
    for (size_t binding_index = binding_count;
         valid && binding_index-- > 0;) {
        if (!candidates[binding_index]) continue;
        QttResourceOp *allocation = NULL;
        AST *name = binding->array.elements[binding_index * 2];
        for (size_t j = 0; j < plan->count - candidate_count; j++) {
            if (plan->ops[j].kind == QTT_RESOURCE_ALLOC &&
                plan->ops[j].var.module_id == module_id &&
                plan->ops[j].var.binder_id ==
                    name->resolved_binder_id) {
                if (allocation) {
                    valid = false;
                    break;
                }
                allocation = &plan->ops[j];
            }
        }
        if (!valid || !allocation) {
            valid = false;
            break;
        }
        QttResourceOp *drop =
            &plan->ops[
                plan->count - candidate_count + cleanup_index];
        QttSemanticDestructorEvidence *destructor =
            find_destructor(
                destructors, destructor_count, drop->var);
        const QttAnfInstruction *anf_allocation =
            anf_find_alloc(anf, allocation->var);
        bool anf_item_matches = anf_valid && anf_allocation &&
            anf_allocation->representation ==
                allocation->representation &&
            anf_contains_drop(anf, allocation->var);
        if (anf_valid && !anf_item_matches)
            all_anf_items_match = false;
        valid =
            allocation->kind == QTT_RESOURCE_ALLOC &&
            allocation->representation == QTT_REP_OWNED_HEAP &&
            drop->kind == QTT_RESOURCE_DROP &&
            qtt_core_var_equal(allocation->var, drop->var) &&
            name->resolved_binder_id == allocation->var.binder_id &&
            allocation->var.module_id == module_id &&
            backend_accepts_destructor(
                destructor, initializer_types[binding_index], true);
        if (valid) {
            items[cleanup_index].resource = allocation->var;
            items[cleanup_index].destructor =
                destructor->descriptor->id;
            items[cleanup_index].representation =
                allocation->representation;
            items[cleanup_index].materialization =
                materializations[binding_index];
            items[cleanup_index].binding_index = binding_index;
            items[cleanup_index].destroy = true;
            items[cleanup_index].evacuated_count =
                destructor->mask->evacuated_count;
            if (destructor->mask->evacuated_count) {
                items[cleanup_index].evacuated = malloc(
                    destructor->mask->evacuated_count *
                    sizeof(*items[cleanup_index].evacuated));
                if (!items[cleanup_index].evacuated) {
                    valid = false;
                    break;
                }
                memcpy(items[cleanup_index].evacuated,
                       destructor->mask->evacuated,
                       destructor->mask->evacuated_count *
                       sizeof(*items[cleanup_index].evacuated));
            }
        }
        cleanup_index++;
    }
    if (valid && cleanups) {
        cleanups->items = items;
        cleanups->count = candidate_count;
        cleanups->ownership_anf_certified = all_anf_items_match;
        items = NULL;
    }
    if (items)
        for (size_t i = 0; i < candidate_count; i++)
            free(items[i].evacuated);
    free(items);
    free(materializations);
    free(initializer_types);
    free(candidates);
    qtt_semantic_ir_free(semantic);
    qtt_anf_program_free(anf);
    qtt_core_free(core);
    return valid ? true
                 : reject_cleanup(
                       "cleanup plan did not match source bindings");
}

bool qtt_backend_certify_string_cleanups(
    AST *source, uint64_t module_id,
    QttBackendCleanupPlan *cleanups) {
    return qtt_backend_certify_string_cleanups_in_env(
        source, NULL, module_id, cleanups);
}

static bool source_replacement_contract(
    const AST *node, uint64_t binder_id,
    const QttSignatureEnv *signatures, uint64_t module_id,
    QttBackendMaterialization *materialization) {
    if (!node || node->type != AST_LIST || node->list.count != 3 ||
        !node->list.items[0] ||
        node->list.items[0]->type != AST_SYMBOL ||
        strcmp(node->list.items[0]->symbol, "set!") != 0 ||
        !node->list.items[1] ||
        node->list.items[1]->type != AST_SYMBOL ||
        node->list.items[1]->resolved_binder_id != binder_id ||
        !node->list.items[2])
        return false;
    const Type *replacement_type = NULL;
    return initializer_contract(
               node->list.items[2], signatures, module_id,
               materialization, &replacement_type) &&
           replacement_type && replacement_type->kind == TYPE_STRING;
}

static size_t count_certified_replacements(
    const AST *node, uint64_t binder_id,
    const QttSignatureEnv *signatures, uint64_t module_id) {
    if (!node) return 0;
    if (node->type == AST_LIST) {
        if (node->list.count >= 1 &&
            node->list.items[0] &&
            node->list.items[0]->type == AST_SYMBOL &&
            strcmp(node->list.items[0]->symbol, "with") == 0)
            return 0;
        QttBackendMaterialization materialization;
        if (source_replacement_contract(
                node, binder_id, signatures, module_id,
                &materialization))
            return 1;
        size_t count = 0;
        for (size_t i = 0; i < node->list.count; i++)
            count += count_certified_replacements(
                node->list.items[i], binder_id,
                signatures, module_id);
        return count;
    }
    if (node->type == AST_ARRAY) {
        size_t count = 0;
        for (size_t i = 0; i < node->array.element_count; i++)
            count += count_certified_replacements(
                node->array.elements[i], binder_id,
                signatures, module_id);
        return count;
    }
    return 0;
}

static bool find_replacement_materialization(
    const AST *node, uint64_t binder_id, size_t wanted,
    const QttSignatureEnv *signatures, uint64_t module_id,
    size_t *seen, QttBackendMaterialization *materialization) {
    if (!node) return false;
    if (node->type == AST_LIST) {
        if (node->list.count >= 1 &&
            node->list.items[0] &&
            node->list.items[0]->type == AST_SYMBOL &&
            strcmp(node->list.items[0]->symbol, "with") == 0)
            return false;
        QttBackendMaterialization found;
        if (source_replacement_contract(
                node, binder_id, signatures, module_id, &found)) {
            if ((*seen)++ == wanted) {
                *materialization = found;
                return true;
            }
            return false;
        }
        for (size_t i = 0; i < node->list.count; i++)
            if (find_replacement_materialization(
                    node->list.items[i], binder_id, wanted,
                    signatures, module_id, seen, materialization))
                return true;
    } else if (node->type == AST_ARRAY) {
        for (size_t i = 0; i < node->array.element_count; i++)
            if (find_replacement_materialization(
                    node->array.elements[i], binder_id, wanted,
                    signatures, module_id, seen, materialization))
                return true;
    }
    return false;
}

static size_t count_resource_replacements(
    const QttResourceBlock *block) {
    if (!block) return 0;
    size_t count = 0;
    for (size_t i = 0; i < block->count; i++) {
        const QttResourceOp *op = &block->ops[i];
        if (op->kind == QTT_RESOURCE_REPLACE)
            count++;
        else if (op->kind == QTT_RESOURCE_BRANCH) {
            count += count_resource_replacements(
                op->branch.then_block);
            count += count_resource_replacements(
                op->branch.else_block);
        }
    }
    return count;
}

static void collect_resource_replacements(
    const QttResourceBlock *block,
    const QttResourceOp **items, size_t *cursor) {
    if (!block) return;
    for (size_t i = 0; i < block->count; i++) {
        const QttResourceOp *op = &block->ops[i];
        if (op->kind == QTT_RESOURCE_REPLACE)
            items[(*cursor)++] = op;
        else if (op->kind == QTT_RESOURCE_BRANCH) {
            collect_resource_replacements(
                op->branch.then_block, items, cursor);
            collect_resource_replacements(
                op->branch.else_block, items, cursor);
        }
    }
}

bool qtt_backend_certify_string_replacements_in_env(
    AST *source, const QttSignatureEnv *signatures,
    uint64_t module_id,
    QttBackendReplacementPlan *replacements) {
    if (replacements) memset(replacements, 0, sizeof(*replacements));
    if (!source || !module_id || source->type != AST_LIST ||
        source->list.count < 3)
        return reject_cleanup("unsupported replacement source shape");
    AST *bindings = source->list.items[1];
    if (!bindings || bindings->type != AST_ARRAY ||
        !bindings->array.element_count ||
        (bindings->array.element_count % 2) != 0)
        return reject_cleanup("malformed replacement bindings");
    size_t binding_count = bindings->array.element_count / 2;
    bool source_has_replacement = false;
    for (size_t binding_index = 0;
         binding_index < binding_count && !source_has_replacement;
         binding_index++) {
        AST *name = bindings->array.elements[binding_index * 2];
        if (!name || name->type != AST_SYMBOL ||
            !name->resolved_binder_id)
            continue;
        for (size_t body = 2; body < source->list.count; body++)
            if (count_certified_replacements(
                    source->list.items[body],
                    name->resolved_binder_id,
                    signatures, module_id)) {
                source_has_replacement = true;
                break;
            }
    }
    /* Absence is not failed evidence: this certifier is inapplicable. */
    if (!source_has_replacement) return false;

    QttCoreError core_error = QTT_CORE_OK;
    QttCoreNode *core =
        qtt_core_lower(source, module_id, &core_error);
    if (!core)
        return reject_cleanup(
            "replacement source did not lower to typed Core");
    QttSemanticIrError ir_error = QTT_SEMANTIC_IR_OK;
    QttSemanticFunction *semantic = signatures
        ? qtt_semantic_ir_lower_in_env(
              core, signatures, module_id, &ir_error)
        : qtt_semantic_ir_lower(core, &ir_error);
    bool valid = semantic && ir_error == QTT_SEMANTIC_IR_OK &&
        qtt_semantic_ir_verify(semantic, core) ==
            QTT_SEMANTIC_IR_VALID &&
        qtt_semantic_ir_resource_status(semantic) ==
            QTT_RESOURCE_ELABORATE_OK;
    QttResourceBlock *resources =
        valid ? qtt_semantic_ir_resources(semantic) : NULL;
    QttSemanticDestructorEvidence *destructors =
        valid ? qtt_semantic_ir_destructors(semantic) : NULL;
    size_t destructor_count = valid
        ? qtt_semantic_ir_destructor_count(semantic) : 0;
    size_t replacement_count = valid
        ? count_resource_replacements(resources) : 0;
    valid = valid && replacement_count != 0;
    QttBackendReplacement *items = valid
        ? calloc(replacement_count, sizeof(*items)) : NULL;
    const QttResourceOp **replacement_ops = valid
        ? calloc(replacement_count, sizeof(*replacement_ops)) : NULL;
    valid = valid && items && replacement_ops;
    size_t collected = 0;
    if (valid)
        collect_resource_replacements(
            resources, replacement_ops, &collected);
    valid = valid && collected == replacement_count;
    size_t output = 0;
    for (size_t i = 0; valid && i < replacement_count; i++) {
        const QttResourceOp *op = replacement_ops[i];
        size_t binding_index = binding_count;
        QttBackendMaterialization initializer_materialization =
            QTT_BACKEND_MATERIALIZE_OWNED_RESULT;
        const Type *initializer_type = NULL;
        for (size_t j = 0; j < binding_count; j++) {
            AST *name = bindings->array.elements[j * 2];
            AST *initializer =
                bindings->array.elements[j * 2 + 1];
            if (!name || name->type != AST_SYMBOL ||
                name->resolved_binder_id != op->var.binder_id ||
                op->var.module_id != module_id)
                continue;
            if (!initializer_contract(
                    initializer, signatures, module_id,
                    &initializer_materialization,
                    &initializer_type))
                break;
            binding_index = j;
            break;
        }
        QttSemanticDestructorEvidence *destructor =
            find_destructor(
                destructors, destructor_count, op->var);
        size_t source_replacements = 0;
        if (binding_index < binding_count)
            for (size_t body = 2;
                 body < source->list.count; body++)
                source_replacements += count_certified_replacements(
                    source->list.items[body],
                    op->var.binder_id, signatures, module_id);
        size_t prior = 0;
        for (size_t j = 0; j < output; j++)
            prior += qtt_core_var_equal(
                items[j].resource, op->var);
        QttBackendMaterialization replacement_materialization =
            QTT_BACKEND_MATERIALIZE_OWNED_RESULT;
        size_t seen = 0;
        bool found_materialization = false;
        for (size_t body = 2;
             !found_materialization && body < source->list.count; body++)
            found_materialization = find_replacement_materialization(
                source->list.items[body], op->var.binder_id, prior,
                signatures, module_id, &seen,
                &replacement_materialization);
        valid =
            binding_index < binding_count &&
            source_replacements > prior &&
            found_materialization &&
            op->representation == QTT_REP_OWNED_HEAP &&
            qtt_place_equal(
                op->place, qtt_place_root(op->var)) &&
            initializer_type &&
            initializer_type->kind == TYPE_STRING &&
            backend_accepts_destructor(
                destructor, initializer_type, false);
        if (!valid) break;
        items[output++] = (QttBackendReplacement){
            .resource = op->var,
            .destructor = destructor->descriptor->id,
            .representation = op->representation,
            .initializer_materialization =
                initializer_materialization,
            .replacement_materialization =
                replacement_materialization,
            .binding_index = binding_index,
        };
    }
    if (valid && output != replacement_count) valid = false;
    if (valid && replacements) {
        replacements->items = items;
        replacements->count = replacement_count;
        items = NULL;
    }
    free(replacement_ops);
    free(items);
    qtt_semantic_ir_free(semantic);
    qtt_core_free(core);
    return valid ? true
                 : reject_cleanup(
                       "replacement plan did not match source/IR");
}
