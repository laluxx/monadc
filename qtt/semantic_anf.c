#include "semantic_anf.h"

#include <stdlib.h>
#include <string.h>

static QttSemanticCapability environment_capability(
    const QttSemanticClosureFieldEvidence *field) {
    if (field->representation == QTT_REP_IMMEDIATE ||
        field->representation == QTT_REP_INLINE)
        return QTT_SEMANTIC_CAPABILITY_NONE;
    return field->storage == QTT_CLOSURE_STORAGE_UNIQUE
        ? QTT_SEMANTIC_CAPABILITY_OWNED
        : QTT_SEMANTIC_CAPABILITY_SHARED;
}

static bool environment_slot_matches(
    const QttSemanticClosureFieldEvidence *field,
    const QttAnfEnvironmentSlot *slot) {
    return field->closure_id == slot->closure_id &&
        field->ordinal == slot->ordinal &&
        qtt_core_var_equal(field->source, slot->source) &&
        qtt_type_id_equal(field->type_id, slot->type_id) &&
        field->representation == slot->representation &&
        environment_capability(field) == slot->capability &&
        field->exit == slot->exit &&
        (field->descriptor != NULL) == slot->has_destructor &&
        (!field->descriptor ||
         qtt_destructor_id_equal(
             field->descriptor->id, slot->destructor_id));
}

static bool closure_identity(
    QttSemanticFunction *semantic, const QttCoreNode *lambda,
    uint64_t *closure_id) {
    QttSemanticNode *nodes = qtt_semantic_ir_nodes(semantic);
    size_t count = qtt_semantic_ir_node_count(semantic);
    for (size_t i = 0; i < count; i++)
        if (nodes[i].source == lambda &&
            nodes[i].kind == QTT_CORE_LAMBDA &&
            nodes[i].closure_id) {
            *closure_id = nodes[i].closure_id;
            return true;
        }
    return false;
}

QttSemanticAnfError qtt_semantic_anf_verify_environment(
    QttSemanticFunction *semantic,
    const QttCoreNode *lambda,
    const QttAnfProgram *program) {
    if (!semantic || !lambda || !program ||
        qtt_semantic_ir_verify(
            semantic, qtt_semantic_ir_source(semantic)) !=
            QTT_SEMANTIC_IR_VALID)
        return QTT_SEMANTIC_ANF_INVALID_IR;
    uint64_t closure_id = 0;
    if (!closure_identity(semantic, lambda, &closure_id))
        return QTT_SEMANTIC_ANF_ENVIRONMENT_MISMATCH;
    QttSemanticClosureFieldEvidence *fields =
        qtt_semantic_ir_closure_fields(semantic);
    size_t field_count =
        qtt_semantic_ir_closure_field_count(semantic);
    size_t expected = 0;
    for (size_t i = 0; i < field_count; i++)
        if (fields[i].closure_id == closure_id)
            expected++;
    if (program->environment_slot_count != expected ||
        (expected && !program->environment_slots))
        return QTT_SEMANTIC_ANF_ENVIRONMENT_MISMATCH;
    size_t slot = 0;
    for (size_t i = 0; i < field_count; i++) {
        if (fields[i].closure_id != closure_id) continue;
        if (!environment_slot_matches(
                &fields[i], &program->environment_slots[slot++]))
            return QTT_SEMANTIC_ANF_ENVIRONMENT_MISMATCH;
    }
    return QTT_SEMANTIC_ANF_OK;
}

QttSemanticAnfError qtt_semantic_anf_materialize_environment(
    QttSemanticFunction *semantic,
    const QttCoreNode *lambda,
    QttAnfProgram *program) {
    if (!semantic || !lambda || !program ||
        program->environment_slots ||
        program->environment_slot_count)
        return QTT_SEMANTIC_ANF_ENVIRONMENT_MISMATCH;
    if (qtt_semantic_ir_verify(
            semantic, qtt_semantic_ir_source(semantic)) !=
        QTT_SEMANTIC_IR_VALID)
        return QTT_SEMANTIC_ANF_INVALID_IR;
    uint64_t closure_id = 0;
    if (!closure_identity(semantic, lambda, &closure_id))
        return QTT_SEMANTIC_ANF_ENVIRONMENT_MISMATCH;
    QttSemanticClosureFieldEvidence *fields =
        qtt_semantic_ir_closure_fields(semantic);
    size_t field_count =
        qtt_semantic_ir_closure_field_count(semantic);
    size_t count = 0;
    for (size_t i = 0; i < field_count; i++)
        if (fields[i].closure_id == closure_id)
            count++;
    QttAnfEnvironmentSlot *slots =
        calloc(count ? count : 1, sizeof(*slots));
    if (!slots) return QTT_SEMANTIC_ANF_ENVIRONMENT_MISMATCH;
    size_t slot = 0;
    for (size_t i = 0; i < field_count; i++) {
        if (fields[i].closure_id != closure_id) continue;
        slots[slot++] = (QttAnfEnvironmentSlot){
            .closure_id = fields[i].closure_id,
            .ordinal = fields[i].ordinal,
            .source = fields[i].source,
            .type_id = fields[i].type_id,
            .representation = fields[i].representation,
            .capability = environment_capability(&fields[i]),
            .exit = fields[i].exit,
            .has_destructor = fields[i].descriptor != NULL,
            .destructor_id = fields[i].descriptor
                ? fields[i].descriptor->id
                : (QttDestructorId){0},
        };
    }
    program->environment_slots = slots;
    program->environment_slot_count = count;
    QttSemanticAnfError result =
        qtt_semantic_anf_verify_environment(
            semantic, lambda, program);
    if (result != QTT_SEMANTIC_ANF_OK) {
        free(program->environment_slots);
        program->environment_slots = NULL;
        program->environment_slot_count = 0;
    }
    return result;
}

typedef struct ClosureLocalBinding {
    QttCoreVar var;
    QttAnfValue value;
    QttAnfType type;
    struct ClosureLocalBinding *parent;
} ClosureLocalBinding;

typedef struct {
    QttCoreVar source;
    QttControlPathStep *path;
    size_t path_count;
    bool matched;
} ExpectedEnvironmentProjection;

typedef struct {
    ExpectedEnvironmentProjection *items;
    size_t count;
    size_t branch_count;
    bool valid;
} ExpectedProjectionSet;

static bool projection_path_equal(
    const QttControlPathStep *left, size_t left_count,
    const QttControlPathStep *right, size_t right_count) {
    if (left_count != right_count ||
        (left_count && (!left || !right)))
        return false;
    for (size_t i = 0; i < left_count; i++)
        if (left[i].branch_ordinal != right[i].branch_ordinal ||
            left[i].else_arm != right[i].else_arm)
            return false;
    return true;
}

static bool projection_is_capture(
    const QttSemanticClosureFieldEvidence *fields,
    size_t field_count, uint64_t closure_id,
    QttCoreVar source) {
    for (size_t i = 0; i < field_count; i++)
        if (fields[i].closure_id == closure_id &&
            qtt_core_var_equal(fields[i].source, source))
            return true;
    return false;
}

static void collect_expected_projections(
    ExpectedProjectionSet *set, const QttCoreNode *node,
    const QttSemanticClosureFieldEvidence *fields,
    size_t field_count, uint64_t closure_id,
    const QttControlPathStep *path, size_t path_count) {
    if (!set->valid || !node) {
        set->valid = false;
        return;
    }
    if (node->kind == QTT_CORE_VAR &&
        projection_is_capture(
            fields, field_count, closure_id, node->var)) {
        ExpectedEnvironmentProjection *grown = realloc(
            set->items, (set->count + 1) * sizeof(*grown));
        if (!grown) {
            set->valid = false;
            return;
        }
        set->items = grown;
        ExpectedEnvironmentProjection *expected =
            &set->items[set->count++];
        *expected = (ExpectedEnvironmentProjection){
            .source = node->var,
            .path_count = path_count,
        };
        if (path_count) {
            expected->path =
                malloc(path_count * sizeof(*expected->path));
            if (!expected->path) {
                set->valid = false;
                return;
            }
            memcpy(
                expected->path, path,
                path_count * sizeof(*expected->path));
        }
        return;
    }
    if (node->kind == QTT_CORE_IF) {
        collect_expected_projections(
            set, node->conditional.condition,
            fields, field_count, closure_id, path, path_count);
        size_t ordinal = set->branch_count++;
        QttControlPathStep *then_path = malloc(
            (path_count + 1) * sizeof(*then_path));
        QttControlPathStep *else_path = malloc(
            (path_count + 1) * sizeof(*else_path));
        if (!then_path || !else_path) {
            free(then_path);
            free(else_path);
            set->valid = false;
            return;
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
            .branch_ordinal = ordinal, .else_arm = false};
        else_path[path_count] = (QttControlPathStep){
            .branch_ordinal = ordinal, .else_arm = true};
        collect_expected_projections(
            set, node->conditional.then_branch,
            fields, field_count, closure_id,
            then_path, path_count + 1);
        collect_expected_projections(
            set, node->conditional.else_branch,
            fields, field_count, closure_id,
            else_path, path_count + 1);
        free(then_path);
        free(else_path);
        return;
    }
    if (node->kind == QTT_CORE_APPLY) {
        collect_expected_projections(
            set, node->apply.callee, fields, field_count,
            closure_id, path, path_count);
        for (size_t i = 0; i < node->apply.argument_count; i++)
            collect_expected_projections(
                set, node->apply.arguments[i],
                fields, field_count, closure_id,
                path, path_count);
    } else if (node->kind == QTT_CORE_SEQUENCE)
        for (size_t i = 0; i < node->sequence.count; i++)
            collect_expected_projections(
                set, node->sequence.items[i],
                fields, field_count, closure_id,
                path, path_count);
    else if (node->kind == QTT_CORE_LET) {
        collect_expected_projections(
            set, node->let.value, fields, field_count,
            closure_id, path, path_count);
        collect_expected_projections(
            set, node->let.body, fields, field_count,
            closure_id, path, path_count);
    }
}

static void expected_projections_free(
    ExpectedProjectionSet *set) {
    for (size_t i = 0; i < set->count; i++)
        free(set->items[i].path);
    free(set->items);
}

static bool projection_branches_match(
    const QttAnfProgram *program, size_t expected_count) {
    bool *seen = calloc(
        expected_count ? expected_count : 1, sizeof(*seen));
    if (!seen) return false;
    size_t found = 0;
    bool valid = true;
    for (size_t i = 0; i < program->block_count; i++) {
        const QttAnfTerminator *terminator =
            &program->blocks[i].terminator;
        if (terminator->kind != QTT_ANF_BRANCH) continue;
        if (!terminator->has_branch_identity ||
            terminator->branch_ordinal >= expected_count ||
            seen[terminator->branch_ordinal]) {
            valid = false;
            break;
        }
        seen[terminator->branch_ordinal] = true;
        found++;
    }
    free(seen);
    return valid && found == expected_count;
}

QttSemanticAnfError qtt_semantic_anf_verify_projections(
    QttSemanticFunction *semantic,
    const QttCoreNode *lambda,
    const QttAnfProgram *program) {
    QttSemanticAnfError environment =
        qtt_semantic_anf_verify_environment(
            semantic, lambda, program);
    if (environment != QTT_SEMANTIC_ANF_OK)
        return environment;
    uint64_t closure_id = 0;
    if (!closure_identity(semantic, lambda, &closure_id))
        return QTT_SEMANTIC_ANF_PROJECTION_MISMATCH;
    QttDemandError demand_error = QTT_DEMAND_OK;
    QttDemandCertificate *demand =
        qtt_demand_derive(lambda->lambda.body, &demand_error);
    if (!demand)
        return QTT_SEMANTIC_ANF_PROJECTION_MISMATCH;
    QttSemanticClosureFieldEvidence *fields =
        qtt_semantic_ir_closure_fields(semantic);
    size_t field_count =
        qtt_semantic_ir_closure_field_count(semantic);
    ExpectedProjectionSet expected = {.valid = true};
    collect_expected_projections(
        &expected, lambda->lambda.body, fields, field_count,
        closure_id, NULL, 0);
    bool valid = expected.valid &&
        projection_branches_match(
            program, expected.branch_count);
    for (size_t field_index = 0;
         field_index < field_count && valid; field_index++) {
        const QttSemanticClosureFieldEvidence *field =
            &fields[field_index];
        if (field->closure_id != closure_id) continue;
        size_t projections = 0;
        size_t moves = 0;
        size_t releases = 0;
        for (size_t block = 0; block < program->block_count; block++)
            for (size_t i = 0;
                 i < program->blocks[block].instruction_count; i++) {
                const QttAnfInstruction *instruction =
                    &program->blocks[block].instructions[i];
                if (instruction->kind != QTT_ANF_ENV_PROJECT ||
                    instruction->environment_closure_id != closure_id ||
                    instruction->environment_ordinal != field->ordinal)
                    continue;
                if (instruction->environment_action ==
                    QTT_ANF_ENV_RELEASE)
                    releases++;
                else {
                    projections++;
                    bool matched = false;
                    for (size_t expected_index = 0;
                         expected_index < expected.count;
                         expected_index++) {
                        ExpectedEnvironmentProjection *candidate =
                            &expected.items[expected_index];
                        if (candidate->matched ||
                            !qtt_core_var_equal(
                                candidate->source,
                                instruction->resource) ||
                            !projection_path_equal(
                                candidate->path,
                                candidate->path_count,
                                instruction->control_path,
                                instruction->control_path_count))
                            continue;
                        candidate->matched = true;
                        matched = true;
                        break;
                    }
                    if (!matched) valid = false;
                }
                moves += instruction->environment_action ==
                    QTT_ANF_ENV_MOVE;
                if (instruction->environment_action ==
                        QTT_ANF_ENV_RELEASE &&
                    instruction->control_path_count != 0)
                    valid = false;
                if (!qtt_core_var_equal(
                        instruction->resource, field->source))
                    valid = false;
            }
        size_t uses = qtt_demand_occurrences(
            demand, lambda->lambda.body, field->source);
        if (projections != uses ||
            (field->storage == QTT_CLOSURE_STORAGE_UNIQUE &&
             field->exit == QTT_CLOSURE_FIELD_MOVE_OUT &&
             (moves < 1 || releases != 0)) ||
            (field->storage == QTT_CLOSURE_STORAGE_UNIQUE &&
             field->exit == QTT_CLOSURE_FIELD_RELEASE &&
             (moves != 0 || releases != 1)) ||
            (field->storage != QTT_CLOSURE_STORAGE_UNIQUE &&
             (moves != 0 || releases != 0)))
            valid = false;
    }
    for (size_t block = 0; block < program->block_count && valid; block++)
        for (size_t i = 0;
             i < program->blocks[block].instruction_count; i++) {
            const QttAnfInstruction *instruction =
                &program->blocks[block].instructions[i];
            if (instruction->kind != QTT_ANF_ENV_PROJECT) continue;
            bool found = false;
            for (size_t field_index = 0;
                 field_index < field_count; field_index++)
                if (fields[field_index].closure_id == closure_id &&
                    fields[field_index].ordinal ==
                        instruction->environment_ordinal &&
                    instruction->environment_closure_id == closure_id)
                    found = true;
            if (!found) valid = false;
        }
    for (size_t i = 0; i < expected.count; i++)
        if (!expected.items[i].matched)
            valid = false;
    expected_projections_free(&expected);
    qtt_demand_certificate_free(demand);
    return valid
        ? QTT_SEMANTIC_ANF_OK
        : QTT_SEMANTIC_ANF_PROJECTION_MISMATCH;
}

typedef struct {
    QttAnfProgram *program;
    const QttDemandCertificate *demand;
    size_t *remaining;
    QttAnfValue next_value;
    size_t current;
    size_t next_branch_ordinal;
    size_t next_call_ordinal;
    const QttSignatureEnv *signatures;
    uint64_t module_id;
    QttControlPathStep *control_path;
    size_t control_path_count;
    size_t control_path_capacity;
    ClosureLocalBinding *locals;
    QttAnfLowerError error;
} ClosureBodyLowering;

typedef enum {
    CLOSURE_RESULT_ESCAPE,
    CLOSURE_RESULT_DISCARD,
} ClosureResultUse;

static QttAnfType closure_body_type(const QttCoreNode *node) {
    if (!node || !node->type) return QTT_ANF_TYPE_UNKNOWN;
    switch (node->type->kind) {
    case TYPE_STRING: return QTT_ANF_TYPE_STRING;
    case TYPE_BOOL: return QTT_ANF_TYPE_BOOL;
    case TYPE_UNIT:
    case TYPE_NIL: return QTT_ANF_TYPE_UNIT;
    case TYPE_INT:
    case TYPE_FLOAT:
    case TYPE_F32:
    case TYPE_I8:
    case TYPE_U8:
    case TYPE_I16:
    case TYPE_U16:
    case TYPE_I32:
    case TYPE_U32:
    case TYPE_I64:
    case TYPE_U64:
    case TYPE_I128:
    case TYPE_U128:
    case TYPE_INT_ARBITRARY:
    case TYPE_F80:
        return QTT_ANF_TYPE_NUMBER;
    default:
        return QTT_ANF_TYPE_UNKNOWN;
    }
}

static bool closure_body_emit(
    ClosureBodyLowering *lowering,
    QttAnfInstruction instruction) {
    if ((instruction.kind == QTT_ANF_ENV_PROJECT ||
         instruction.kind == QTT_ANF_CALL) &&
        lowering->control_path_count) {
        instruction.control_path = malloc(
            lowering->control_path_count *
            sizeof(*instruction.control_path));
        if (!instruction.control_path) {
            lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
            return false;
        }
        memcpy(
            instruction.control_path, lowering->control_path,
            lowering->control_path_count *
            sizeof(*instruction.control_path));
        instruction.control_path_count =
            lowering->control_path_count;
    }
    QttAnfBlock *block =
        &lowering->program->blocks[lowering->current];
    QttAnfInstruction *grown = realloc(
        block->instructions,
        (block->instruction_count + 1) * sizeof(*grown));
    if (!grown) {
        free(instruction.control_path);
        lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return false;
    }
    block->instructions = grown;
    block->instructions[block->instruction_count++] = instruction;
    return true;
}

static size_t closure_body_add_block(
    ClosureBodyLowering *lowering) {
    size_t index = lowering->program->block_count;
    QttAnfBlock *grown = realloc(
        lowering->program->blocks,
        (index + 1) * sizeof(*grown));
    if (!grown) {
        lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return 0;
    }
    lowering->program->blocks = grown;
    lowering->program->blocks[index] = (QttAnfBlock){0};
    lowering->program->block_count++;
    return index;
}

static bool closure_body_push_path(
    ClosureBodyLowering *lowering,
    size_t branch_ordinal, bool else_arm) {
    if (lowering->control_path_count ==
        lowering->control_path_capacity) {
        size_t next = lowering->control_path_capacity
            ? lowering->control_path_capacity * 2 : 4;
        QttControlPathStep *grown = realloc(
            lowering->control_path, next * sizeof(*grown));
        if (!grown) {
            lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
            return false;
        }
        lowering->control_path = grown;
        lowering->control_path_capacity = next;
    }
    lowering->control_path[lowering->control_path_count++] =
        (QttControlPathStep){
            .branch_ordinal = branch_ordinal,
            .else_arm = else_arm,
        };
    return true;
}

static void closure_body_pop_path(
    ClosureBodyLowering *lowering) {
    if (lowering->control_path_count)
        lowering->control_path_count--;
}

static ptrdiff_t closure_body_slot(
    const QttAnfProgram *program, QttCoreVar source) {
    for (size_t i = 0; i < program->environment_slot_count; i++)
        if (qtt_core_var_equal(
                program->environment_slots[i].source, source))
            return (ptrdiff_t)i;
    return -1;
}

static ClosureLocalBinding *closure_body_local(
    ClosureBodyLowering *lowering, QttCoreVar var) {
    for (ClosureLocalBinding *binding = lowering->locals;
         binding; binding = binding->parent)
        if (qtt_core_var_equal(binding->var, var))
            return binding;
    return NULL;
}

static QttAnfValue lower_closure_body_node(
    ClosureBodyLowering *lowering,
    const QttCoreNode *node,
    ClosureResultUse result_use) {
    if (!node || lowering->error != QTT_ANF_LOWER_OK) {
        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }
    if (node->kind == QTT_CORE_SEQUENCE) {
        if (!node->sequence.count) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        QttAnfValue result = 0;
        for (size_t i = 0; i < node->sequence.count; i++)
            result = lower_closure_body_node(
                lowering, node->sequence.items[i],
                i + 1 == node->sequence.count
                    ? result_use
                    : CLOSURE_RESULT_DISCARD);
        return result;
    }
    if (node->kind == QTT_CORE_LITERAL && node->literal.source) {
        QttAnfValue result = lowering->next_value++;
        if (node->literal.source->type == AST_NUMBER)
            return closure_body_emit(
                lowering, qtt_anf_const_number(
                    result, node->literal.source->number))
                ? result : 0;
        if (node->literal.source->type == AST_STRING)
            return closure_body_emit(
                lowering, qtt_anf_const_string(
                    result, node->literal.source->string))
                ? result : 0;
        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }
    if (node->kind == QTT_CORE_GLOBAL && node->global.name) {
        bool value;
        if (!strcmp(node->global.name, "True"))
            value = true;
        else if (!strcmp(node->global.name, "False"))
            value = false;
        else {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        QttAnfValue result = lowering->next_value++;
        return closure_body_emit(
            lowering, qtt_anf_const_bool(result, value))
            ? result : 0;
    }
    if (node->kind == QTT_CORE_LET) {
        QttAnfValue value = lower_closure_body_node(
            lowering, node->let.value,
            CLOSURE_RESULT_ESCAPE);
        if (!value) return 0;
        QttAnfType type = closure_body_type(node->let.value);
        QttRepresentation representation =
            qtt_resource_classify_type(node->let.value->type);
        /*
         * Lexical SSA aliases are safe for immediate and inline values.
         * Heap aliases need an explicit ownership/provenance rule and are
         * rejected rather than hiding a captured capability behind a name.
         */
        if (type == QTT_ANF_TYPE_UNKNOWN ||
            (representation != QTT_REP_IMMEDIATE &&
             representation != QTT_REP_INLINE)) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        ClosureLocalBinding binding = {
            .var = node->let.binding,
            .value = value,
            .type = type,
            .parent = lowering->locals,
        };
        lowering->locals = &binding;
        QttAnfValue result = lower_closure_body_node(
            lowering, node->let.body, result_use);
        lowering->locals = binding.parent;
        return result;
    }
    if (node->kind == QTT_CORE_APPLY &&
        node->apply.callee &&
        node->apply.callee->kind == QTT_CORE_GLOBAL) {
        QttCallableId callable = {0};
        QttFunctionSignature *signature = NULL;
        if (!lowering->signatures ||
            !qtt_signature_env_resolve(
                lowering->signatures, lowering->module_id,
                node->apply.callee->global.name,
                &callable, &signature) ||
            !signature ||
            signature->parameter_count !=
                node->apply.argument_count) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        size_t count = signature->parameter_count;
        QttAnfCallOperand *operands = calloc(
            count ? count : 1, sizeof(*operands));
        QttCallArgument *arguments = calloc(
            count ? count : 1, sizeof(*arguments));
        if (!operands || !arguments) {
            free(operands);
            free(arguments);
            lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
            return 0;
        }
        for (size_t i = 0; i < count; i++) {
            const QttParameterContract *parameter =
                &signature->parameters[i];
            QttCallTransfer transfer =
                qtt_call_transfer_for_parameter(parameter);
            /*
             * Heap transfer is admitted only for an exact captured variable.
             * The verified, signature-aware closure policy determines whether
             * its environment projection is a borrow or a final move.
             */
            bool captured_heap =
                parameter->representation == QTT_REP_OWNED_HEAP &&
                node->apply.arguments[i] &&
                node->apply.arguments[i]->kind == QTT_CORE_VAR &&
                closure_body_slot(
                    lowering->program,
                    node->apply.arguments[i]->var) >= 0;
            if ((parameter->representation == QTT_REP_OWNED_HEAP &&
                 !captured_heap) ||
                (transfer != QTT_CALL_VALUE &&
                 transfer != QTT_CALL_BORROW &&
                 transfer != QTT_CALL_MOVE)) {
                lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
                break;
            }
            QttAnfValue value = lower_closure_body_node(
                lowering, node->apply.arguments[i],
                CLOSURE_RESULT_ESCAPE);
            if (!value) break;
            QttAnfType type =
                closure_body_type(node->apply.arguments[i]);
            arguments[i] = (QttCallArgument){
                .type = node->apply.arguments[i]->type,
                .type_id = parameter->type_id,
                .representation = parameter->representation,
                .transfer = transfer,
            };
            operands[i] = (QttAnfCallOperand){
                .value = value,
                .type = type,
                .type_id = parameter->type_id,
                .representation = parameter->representation,
                .transfer = transfer,
                .source_resource =
                    captured_heap
                        ? node->apply.arguments[i]->var
                        : (QttCoreVar){0},
            };
        }
        QttCallPlan plan = {0};
        QttCallValidation validation =
            lowering->error == QTT_ANF_LOWER_OK
            ? qtt_call_plan(signature, arguments, count, &plan)
            : QTT_CALL_TRANSFER_MISMATCH;
        free(arguments);
        if (validation != QTT_CALL_VALID) {
            qtt_call_plan_free(&plan);
            free(operands);
            if (lowering->error == QTT_ANF_LOWER_OK)
                lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        qtt_call_plan_free(&plan);
        QttAnfType result_type = closure_body_type(node);
        if (result_type == QTT_ANF_TYPE_UNKNOWN) {
            free(operands);
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        size_t call_ordinal =
            lowering->next_call_ordinal++;
        QttAnfValue result = lowering->next_value++;
        QttCoreVar result_resource = {0};
        if (signature->result.mode == QTT_RESULT_OWNED)
            result_resource =
                qtt_semantic_owned_result_resource(
                    lowering->module_id, call_ordinal);
        QttAnfInstruction call = qtt_anf_call(
            result, result_resource, callable,
            operands, count, result_type,
            signature->result.type_id,
            signature->result.mode,
            signature->result.representation);
        call.contract_fingerprint =
            signature->contract_fingerprint;
        call.owns_call_arguments = true;
        call.has_semantic_call_identity = true;
        call.semantic_call_ordinal = call_ordinal;
        if (!closure_body_emit(lowering, call)) {
            free(operands);
            return 0;
        }
        if (signature->result.mode == QTT_RESULT_OWNED) {
            if (result_use == CLOSURE_RESULT_DISCARD)
                return closure_body_emit(
                    lowering, qtt_anf_drop(result_resource))
                    ? result : 0;
            QttAnfValue moved = lowering->next_value++;
            QttAnfInstruction move =
                qtt_anf_move_value(moved, result_resource);
            move.result_type = result_type;
            return closure_body_emit(lowering, move)
                ? moved : 0;
        }
        return result;
    }
    if (node->kind == QTT_CORE_IF) {
        QttAnfValue condition = lower_closure_body_node(
            lowering, node->conditional.condition,
            CLOSURE_RESULT_ESCAPE);
        if (!condition) return 0;
        size_t slot_count =
            lowering->program->environment_slot_count;
        size_t *after = calloc(
            slot_count ? slot_count : 1, sizeof(*after));
        size_t *then_remaining = calloc(
            slot_count ? slot_count : 1,
            sizeof(*then_remaining));
        size_t *else_remaining = calloc(
            slot_count ? slot_count : 1,
            sizeof(*else_remaining));
        if (!after || !then_remaining || !else_remaining) {
            free(after);
            free(then_remaining);
            free(else_remaining);
            lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
            return 0;
        }
        for (size_t i = 0; i < slot_count; i++) {
            QttCoreVar source =
                lowering->program->environment_slots[i].source;
            size_t then_uses = qtt_demand_occurrences(
                lowering->demand,
                node->conditional.then_branch, source);
            size_t else_uses = qtt_demand_occurrences(
                lowering->demand,
                node->conditional.else_branch, source);
            if (then_uses + else_uses > lowering->remaining[i]) {
                lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
                free(after);
                free(then_remaining);
                free(else_remaining);
                return 0;
            }
            after[i] =
                lowering->remaining[i] - then_uses - else_uses;
            then_remaining[i] = after[i] + then_uses;
            else_remaining[i] = after[i] + else_uses;
        }
        size_t branch_block = lowering->current;
        size_t branch_ordinal =
            lowering->next_branch_ordinal++;
        size_t then_block = closure_body_add_block(lowering);
        size_t else_block = closure_body_add_block(lowering);
        size_t join_block = closure_body_add_block(lowering);
        if (lowering->error != QTT_ANF_LOWER_OK) {
            free(after);
            free(then_remaining);
            free(else_remaining);
            return 0;
        }
        lowering->program->blocks[branch_block].terminator =
            qtt_anf_branch(condition, then_block, else_block);
        lowering->program->blocks[
            branch_block].terminator.has_branch_identity = true;
        lowering->program->blocks[
            branch_block].terminator.branch_ordinal = branch_ordinal;

        memcpy(
            lowering->remaining, then_remaining,
            slot_count * sizeof(*then_remaining));
        lowering->current = then_block;
        QttAnfValue then_value = 0;
        if (closure_body_push_path(
                lowering, branch_ordinal, false)) {
            then_value = lower_closure_body_node(
                lowering, node->conditional.then_branch,
                result_use);
            closure_body_pop_path(lowering);
        }
        size_t then_exit = lowering->current;
        bool then_valid = then_value != 0;
        for (size_t i = 0; i < slot_count; i++)
            then_valid = then_valid &&
                lowering->remaining[i] == after[i];

        memcpy(
            lowering->remaining, else_remaining,
            slot_count * sizeof(*else_remaining));
        lowering->current = else_block;
        QttAnfValue else_value = 0;
        if (then_valid && closure_body_push_path(
                lowering, branch_ordinal, true)) {
            else_value = lower_closure_body_node(
                lowering, node->conditional.else_branch,
                result_use);
            closure_body_pop_path(lowering);
        }
        size_t else_exit = lowering->current;
        bool else_valid = else_value != 0;
        for (size_t i = 0; i < slot_count; i++)
            else_valid = else_valid &&
                lowering->remaining[i] == after[i];
        memcpy(
            lowering->remaining, after,
            slot_count * sizeof(*after));
        free(after);
        free(then_remaining);
        free(else_remaining);
        if (!then_valid || !else_valid) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        QttAnfValue *then_arguments =
            malloc(sizeof(*then_arguments));
        QttAnfValue *else_arguments =
            malloc(sizeof(*else_arguments));
        QttAnfValue *join_parameter =
            malloc(sizeof(*join_parameter));
        QttAnfType *join_type =
            malloc(sizeof(*join_type));
        if (!then_arguments || !else_arguments ||
            !join_parameter || !join_type) {
            free(then_arguments);
            free(else_arguments);
            free(join_parameter);
            free(join_type);
            lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
            return 0;
        }
        then_arguments[0] = then_value;
        else_arguments[0] = else_value;
        lowering->program->blocks[then_exit].terminator =
            qtt_anf_jump(join_block, then_arguments, 1);
        lowering->program->blocks[else_exit].terminator =
            qtt_anf_jump(join_block, else_arguments, 1);
        QttAnfValue result = lowering->next_value++;
        join_parameter[0] = result;
        join_type[0] = closure_body_type(node);
        if (join_type[0] == QTT_ANF_TYPE_UNKNOWN) {
            free(join_parameter);
            free(join_type);
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        lowering->program->blocks[join_block].parameters =
            join_parameter;
        lowering->program->blocks[join_block].parameter_types =
            join_type;
        lowering->program->blocks[join_block].parameter_count = 1;
        lowering->current = join_block;
        return result;
    }
    if (node->kind != QTT_CORE_VAR) {
        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }
    ClosureLocalBinding *local =
        closure_body_local(lowering, node->var);
    if (local) {
        QttAnfType type = closure_body_type(node);
        if (type == QTT_ANF_TYPE_UNKNOWN || type != local->type) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        QttAnfValue result = lowering->next_value++;
        QttAnfInstruction alias =
            qtt_anf_alias(result, local->value);
        alias.result_type = type;
        return closure_body_emit(lowering, alias)
            ? result : 0;
    }
    ptrdiff_t found =
        closure_body_slot(lowering->program, node->var);
    if (found < 0 || !lowering->remaining[(size_t)found]) {
        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }
    QttAnfEnvironmentSlot *slot =
        &lowering->program->environment_slots[(size_t)found];
    lowering->remaining[(size_t)found]--;
    QttAnfEnvironmentAction action =
        !lowering->remaining[(size_t)found] &&
        slot->exit == QTT_CLOSURE_FIELD_MOVE_OUT
        ? QTT_ANF_ENV_MOVE
        : QTT_ANF_ENV_BORROW;
    QttAnfType type = closure_body_type(node);
    if (type == QTT_ANF_TYPE_UNKNOWN) {
        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }
    QttAnfValue result = lowering->next_value++;
    return closure_body_emit(
        lowering, qtt_anf_project_environment(
            result, type, slot->closure_id, slot->ordinal,
            slot->source, action))
        ? result : 0;
}

QttAnfProgram *qtt_semantic_anf_lower_closure_body_in_env(
    QttSemanticFunction *semantic,
    const QttCoreNode *lambda,
    const QttSignatureEnv *signatures,
    uint64_t module_id,
    QttSemanticAnfError *error,
    QttAnfLowerError *lower_error) {
    if (error) *error = QTT_SEMANTIC_ANF_INVALID_IR;
    if (lower_error) *lower_error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
    if (!semantic || !lambda || lambda->kind != QTT_CORE_LAMBDA ||
        qtt_semantic_ir_verify(
            semantic, qtt_semantic_ir_source(semantic)) !=
            QTT_SEMANTIC_IR_VALID)
        return NULL;
    QttAnfProgram *program = calloc(1, sizeof(*program));
    if (!program) {
        if (lower_error) *lower_error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return NULL;
    }
    QttSemanticAnfError materialized =
        qtt_semantic_anf_materialize_environment(
            semantic, lambda, program);
    if (materialized != QTT_SEMANTIC_ANF_OK) {
        qtt_anf_program_free(program);
        if (error) *error = materialized;
        return NULL;
    }
    size_t count = program->environment_slot_count;
    program->blocks = calloc(1, sizeof(*program->blocks));
    program->initial_resources =
        calloc(count ? count : 1, sizeof(*program->initial_resources));
    program->initial_representations = calloc(
        count ? count : 1, sizeof(*program->initial_representations));
    size_t *remaining =
        calloc(count ? count : 1, sizeof(*remaining));
    QttDemandError demand_error = QTT_DEMAND_OK;
    QttDemandCertificate *demand =
        qtt_demand_derive(lambda->lambda.body, &demand_error);
    if (!program->blocks || !program->initial_resources ||
        !program->initial_representations || !remaining || !demand) {
        free(remaining);
        qtt_demand_certificate_free(demand);
        qtt_anf_program_free(program);
        if (lower_error) *lower_error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return NULL;
    }
    program->block_count = 1;
    program->signatures = signatures;
    for (size_t i = 0; i < count; i++) {
        QttAnfEnvironmentSlot *slot = &program->environment_slots[i];
        /*
         * Shared projections require retain/release state, and RELEASE
         * fields require a projection-qualified final drop. Refuse both
         * until those transitions are represented explicitly.
         */
        if (slot->capability != QTT_SEMANTIC_CAPABILITY_OWNED ||
            slot->representation != QTT_REP_OWNED_HEAP ||
            (slot->exit != QTT_CLOSURE_FIELD_MOVE_OUT &&
             slot->exit != QTT_CLOSURE_FIELD_RELEASE)) {
            free(remaining);
            qtt_demand_certificate_free(demand);
            qtt_anf_program_free(program);
            if (error) *error = QTT_SEMANTIC_ANF_PROJECTION_MISMATCH;
            return NULL;
        }
        program->initial_resources[i] = slot->source;
        program->initial_representations[i] = slot->representation;
        remaining[i] = qtt_demand_occurrences(
            demand, lambda->lambda.body, slot->source);
    }
    program->initial_resource_count = count;
    ClosureBodyLowering lowering = {
        .program = program,
        .demand = demand,
        .remaining = remaining,
        .next_value = 1,
        .current = 0,
        .signatures = signatures,
        .module_id = module_id,
        .error = QTT_ANF_LOWER_OK,
    };
    QttAnfValue result =
        lower_closure_body_node(
            &lowering, lambda->lambda.body,
            CLOSURE_RESULT_ESCAPE);
    for (size_t i = 0;
         i < count && lowering.error == QTT_ANF_LOWER_OK; i++)
        if (program->environment_slots[i].exit ==
            QTT_CLOSURE_FIELD_RELEASE) {
            if (remaining[i] ||
                !closure_body_emit(
                    &lowering, qtt_anf_project_environment(
                        0, QTT_ANF_TYPE_UNKNOWN,
                        program->environment_slots[i].closure_id,
                        program->environment_slots[i].ordinal,
                        program->environment_slots[i].source,
                        QTT_ANF_ENV_RELEASE)))
                lowering.error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
        }
    if (lowering.error == QTT_ANF_LOWER_OK)
        program->blocks[lowering.current].terminator =
            qtt_anf_return(result);
    free(lowering.control_path);
    free(remaining);
    qtt_demand_certificate_free(demand);
    if (lowering.error != QTT_ANF_LOWER_OK ||
        qtt_anf_verify(program).error != QTT_ANF_VALID ||
        qtt_semantic_anf_verify_projections(
            semantic, lambda, program) != QTT_SEMANTIC_ANF_OK) {
        qtt_anf_program_free(program);
        if (error) *error = QTT_SEMANTIC_ANF_PROJECTION_MISMATCH;
        if (lower_error)
            *lower_error = lowering.error == QTT_ANF_LOWER_OK
                ? QTT_ANF_LOWER_UNSUPPORTED_CORE
                : lowering.error;
        return NULL;
    }
    if (error) *error = QTT_SEMANTIC_ANF_OK;
    if (lower_error) *lower_error = QTT_ANF_LOWER_OK;
    return program;
}

QttAnfProgram *qtt_semantic_anf_lower_closure_body(
    QttSemanticFunction *semantic,
    const QttCoreNode *lambda,
    QttSemanticAnfError *error,
    QttAnfLowerError *lower_error) {
    return qtt_semantic_anf_lower_closure_body_in_env(
        semantic, lambda, NULL, 0, error, lower_error);
}

static bool ownership_kind(
    const QttAnfInstruction *instruction,
    QttResourceOpKind *resource_kind) {
    switch (instruction->kind) {
    case QTT_ANF_ALLOC:
        *resource_kind = QTT_RESOURCE_ALLOC;
        return true;
    case QTT_ANF_BORROW:
        if (instruction->loan_id) return false;
        *resource_kind = QTT_RESOURCE_BORROW;
        return true;
    case QTT_ANF_MOVE:
        *resource_kind = QTT_RESOURCE_MOVE;
        return true;
    case QTT_ANF_MOVE_PLACE:
        *resource_kind = QTT_RESOURCE_MOVE_PLACE;
        return true;
    case QTT_ANF_DROP:
        *resource_kind = QTT_RESOURCE_DROP;
        return true;
    case QTT_ANF_REPLACE:
        *resource_kind = QTT_RESOURCE_REPLACE;
        return true;
    default:
        return false;
    }
}

static bool administrative_loan_transition(QttResourceOpKind kind) {
    return kind == QTT_RESOURCE_ALIAS ||
           kind == QTT_RESOURCE_END_ALIAS ||
           kind == QTT_RESOURCE_WRITE_ALIAS;
}

static const QttSemanticCapabilityTransition *relevant_transition(
    const QttSemanticCapabilityTransition *transitions, size_t count,
    size_t ordinal) {
    size_t relevant = 0;
    for (size_t i = 0; i < count; i++) {
        if (administrative_loan_transition(transitions[i].kind)) continue;
        if (relevant++ == ordinal) return &transitions[i];
    }
    return NULL;
}

static bool transitions_match(
    QttSemanticFunction *semantic,
    const QttAnfProgram *program) {
    if (!program) return false;
    QttSemanticCapabilityTransition *expected =
        qtt_semantic_ir_transitions(semantic);
    size_t expected_count =
        qtt_semantic_ir_transition_count(semantic);
    size_t relevant_count = 0;
    for (size_t i = 0; i < expected_count; i++)
        if (!administrative_loan_transition(expected[i].kind))
            relevant_count++;
    bool *seen = calloc(
        relevant_count ? relevant_count : 1, sizeof(*seen));
    if (!seen) return false;
    bool valid = true;
    size_t found = 0;
    for (size_t block_index = 0;
         block_index < program->block_count && valid; block_index++) {
        const QttAnfBlock *block =
            &program->blocks[block_index];
        for (size_t i = 0; i < block->instruction_count; i++) {
            const QttAnfInstruction *instruction =
                &block->instructions[i];
            QttResourceOpKind kind;
            if (!ownership_kind(
                    instruction, &kind))
                continue;
            size_t ordinal =
                instruction->semantic_transition_ordinal;
            const QttSemanticCapabilityTransition *transition =
                relevant_transition(expected, expected_count, ordinal);
            if (!instruction->has_semantic_transition_identity ||
                ordinal >= relevant_count || seen[ordinal] ||
                !transition || transition->kind != kind ||
                !qtt_core_var_equal(
                    transition->var,
                    instruction->resource) ||
                ((kind == QTT_RESOURCE_ALLOC ||
                  kind == QTT_RESOURCE_REPLACE) &&
                 transition->representation !=
                     instruction->representation) ||
                ((kind == QTT_RESOURCE_REPLACE ||
                  kind == QTT_RESOURCE_MOVE_PLACE) &&
                 !qtt_place_equal(
                     transition->place,
                     instruction->place)) ||
                transition->control_path_count !=
                    instruction->control_path_count) {
                valid = false;
                break;
            }
            for (size_t step = 0;
                 step < instruction->control_path_count; step++)
                if (!instruction->control_path ||
                    transition->control_path[step].branch_ordinal !=
                        instruction->control_path[step].branch_ordinal ||
                    transition->control_path[step].else_arm !=
                        instruction->control_path[step].else_arm) {
                    valid = false;
                    break;
                }
            if (!valid) break;
            seen[ordinal] = true;
            found++;
        }
    }
    free(seen);
    return valid && found == relevant_count;
}

static bool branch_identities_match(
    QttSemanticFunction *semantic,
    const QttAnfProgram *program,
    size_t *branch_count) {
    size_t edge_count = qtt_semantic_ir_edge_count(semantic);
    if (edge_count % 2 != 0) return false;
    size_t expected_branches = edge_count / 2;
    bool *seen = calloc(
        expected_branches ? expected_branches : 1, sizeof(*seen));
    if (!seen) return false;
    size_t found = 0;
    bool valid = true;
    for (size_t i = 0; i < program->block_count && valid; i++) {
        const QttAnfTerminator *terminator =
            &program->blocks[i].terminator;
        if (terminator->kind != QTT_ANF_BRANCH) continue;
        size_t ordinal = terminator->branch_ordinal;
        if (!terminator->has_branch_identity ||
            ordinal >= expected_branches || seen[ordinal]) {
            valid = false;
            break;
        }
        seen[ordinal] = true;
        found++;
    }
    QttSemanticCapabilityEdge *edges =
        qtt_semantic_ir_edges(semantic);
    for (size_t ordinal = 0;
         ordinal < expected_branches && valid; ordinal++) {
        bool have_then = false, have_else = false;
        for (size_t i = 0; i < edge_count; i++) {
            if (edges[i].branch_ordinal != ordinal) continue;
            if (edges[i].arm == QTT_SEMANTIC_EDGE_THEN) {
                if (have_then) valid = false;
                have_then = true;
            } else if (edges[i].arm == QTT_SEMANTIC_EDGE_ELSE) {
                if (have_else) valid = false;
                have_else = true;
            } else {
                valid = false;
            }
        }
        if (!seen[ordinal] || !have_then || !have_else)
            valid = false;
    }
    free(seen);
    if (branch_count) *branch_count = found;
    return valid && found == expected_branches;
}

static bool control_paths_equal(
    const QttControlPathStep *left,
    const QttControlPathStep *right, size_t count) {
    if (count && (!left || !right)) return false;
    for (size_t i = 0; i < count; i++)
        if (left[i].branch_ordinal != right[i].branch_ordinal ||
            left[i].else_arm != right[i].else_arm)
            return false;
    return true;
}

static bool call_matches(
    const QttSemanticCallEvidence *expected,
    const QttAnfInstruction *actual) {
    if (!expected || !actual || actual->kind != QTT_ANF_CALL ||
        !actual->has_semantic_call_identity ||
        actual->semantic_call_ordinal != expected->call_ordinal ||
        !qtt_callable_id_equal(
            actual->callable, expected->callable) ||
        actual->contract_fingerprint !=
            expected->contract_fingerprint ||
        actual->call_argument_count != expected->argument_count ||
        (actual->call_argument_count && !actual->call_arguments) ||
        actual->result_mode != expected->result.mode ||
        actual->representation != expected->result.representation ||
        !qtt_type_id_equal(
            actual->canonical_result_type,
            expected->result.type_id) ||
        !qtt_core_var_equal(
            actual->result_resource,
            expected->result_resource) ||
        actual->control_path_count != expected->control_path_count ||
        !control_paths_equal(
            actual->control_path, expected->control_path,
            expected->control_path_count))
        return false;
    for (size_t i = 0; i < expected->argument_count; i++) {
        const QttAnfCallOperand *operand =
            &actual->call_arguments[i];
        QttCoreVar source = expected->source_vars[i];
        if (operand->transfer != expected->argument_transfers[i] ||
            operand->representation !=
                expected->argument_representations[i] ||
            !qtt_type_id_equal(
                operand->type_id,
                expected->argument_type_ids[i]))
            return false;
        if (source.module_id || source.binder_id) {
            if (!qtt_core_var_equal(
                    operand->source_resource, source))
                return false;
        } else if (operand->transfer == QTT_CALL_VALUE) {
            if (operand->source_resource.module_id ||
                operand->source_resource.binder_id)
                return false;
        } else if (!operand->source_resource.module_id ||
                   !operand->source_resource.binder_id) {
            return false;
        }
    }
    return true;
}

QttSemanticAnfError qtt_semantic_anf_verify_calls(
    QttSemanticFunction *semantic,
    const QttCoreNode *core,
    const QttAnfProgram *program) {
    if (!semantic || !program ||
        qtt_semantic_ir_verify(semantic, core) !=
            QTT_SEMANTIC_IR_VALID)
        return QTT_SEMANTIC_ANF_INVALID_IR;
    if (qtt_anf_verify(program).error != QTT_ANF_VALID)
        return QTT_SEMANTIC_ANF_OWNERSHIP_MISMATCH;
    size_t expected_count =
        qtt_semantic_ir_call_count(semantic);
    QttSemanticCallEvidence *expected =
        qtt_semantic_ir_calls(semantic);
    bool *seen = calloc(
        expected_count ? expected_count : 1, sizeof(*seen));
    if (!seen) return QTT_SEMANTIC_ANF_CALL_MISMATCH;
    size_t found = 0;
    bool valid = true;
    for (size_t block = 0;
         block < program->block_count && valid; block++)
        for (size_t i = 0;
             i < program->blocks[block].instruction_count; i++) {
            const QttAnfInstruction *instruction =
                &program->blocks[block].instructions[i];
            if (instruction->kind != QTT_ANF_CALL) continue;
            size_t ordinal =
                instruction->semantic_call_ordinal;
            if (!instruction->has_semantic_call_identity ||
                ordinal >= expected_count || seen[ordinal] ||
                !call_matches(&expected[ordinal], instruction)) {
                valid = false;
                break;
            }
            seen[ordinal] = true;
            found++;
        }
    free(seen);
    return valid && found == expected_count
        ? QTT_SEMANTIC_ANF_OK
        : QTT_SEMANTIC_ANF_CALL_MISMATCH;
}

typedef struct {
    const QttCoreNode *source;
    QttPlace place;
    QttAnfType type;
    QttRepresentation representation;
} SemanticWrite;

typedef struct {
    SemanticWrite *items;
    size_t count;
    size_t capacity;
    bool valid;
} SemanticWrites;

static QttAnfType semantic_write_type(const Type *type) {
    if (!type) return QTT_ANF_TYPE_UNKNOWN;
    switch (type->kind) {
    case TYPE_INT: case TYPE_FLOAT: case TYPE_CHAR: case TYPE_BYTE:
    case TYPE_HEX: case TYPE_BIN: case TYPE_OCT: case TYPE_RATIO:
    case TYPE_F32: case TYPE_I8: case TYPE_U8: case TYPE_I16:
    case TYPE_U16: case TYPE_I32: case TYPE_U32: case TYPE_I64:
    case TYPE_U64: case TYPE_I128: case TYPE_U128:
    case TYPE_INT_ARBITRARY: case TYPE_F80:
        return QTT_ANF_TYPE_NUMBER;
    case TYPE_BOOL:
        return QTT_ANF_TYPE_BOOL;
    case TYPE_STRING:
        return QTT_ANF_TYPE_STRING;
    case TYPE_UNIT: case TYPE_NIL:
        return QTT_ANF_TYPE_UNIT;
    default:
        return QTT_ANF_TYPE_UNKNOWN;
    }
}

static void semantic_writes_push(
    SemanticWrites *writes, const QttCoreNode *source,
    QttPlace place, QttAnfType type,
    QttRepresentation representation) {
    if (!writes->valid) return;
    if (writes->count == writes->capacity) {
        size_t next = writes->capacity ? writes->capacity * 2 : 4;
        SemanticWrite *grown = realloc(
            writes->items, next * sizeof(*grown));
        if (!grown) {
            writes->valid = false;
            return;
        }
        writes->items = grown;
        writes->capacity = next;
    }
    writes->items[writes->count++] =
        (SemanticWrite){
            .source = source,
            .place = place,
            .type = type,
            .representation = representation,
        };
}

static void collect_semantic_writes(
    const QttCoreNode *node, SemanticWrites *writes) {
    if (!node || !writes->valid) return;
    switch (node->kind) {
    case QTT_CORE_WRITE:
        collect_semantic_writes(node->write.value, writes);
        semantic_writes_push(
            writes, node, node->write.place,
            semantic_write_type(node->write.value->type),
            qtt_resource_classify_type(
                node->write.value->type));
        return;
    case QTT_CORE_LAMBDA:
        collect_semantic_writes(node->lambda.body, writes);
        return;
    case QTT_CORE_APPLY:
        collect_semantic_writes(node->apply.callee, writes);
        for (size_t i = 0; i < node->apply.argument_count; i++)
            collect_semantic_writes(
                node->apply.arguments[i], writes);
        return;
    case QTT_CORE_LET:
        collect_semantic_writes(node->let.value, writes);
        collect_semantic_writes(node->let.body, writes);
        return;
    case QTT_CORE_IF:
        collect_semantic_writes(
            node->conditional.condition, writes);
        collect_semantic_writes(
            node->conditional.then_branch, writes);
        collect_semantic_writes(
            node->conditional.else_branch, writes);
        return;
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < node->sequence.count; i++)
            collect_semantic_writes(
                node->sequence.items[i], writes);
        return;
    case QTT_CORE_BORROW:
        collect_semantic_writes(node->borrow.body, writes);
        return;
    case QTT_CORE_PERFORM:
        collect_semantic_writes(node->perform.argument, writes);
        return;
    case QTT_CORE_HANDLE:
        collect_semantic_writes(node->handle.computation, writes);
        collect_semantic_writes(node->handle.clause, writes);
        return;
    case QTT_CORE_LITERAL: case QTT_CORE_GLOBAL:
    case QTT_CORE_VAR: case QTT_CORE_PLACE: case QTT_CORE_QUOTE:
        return;
    }
}

static bool semantic_write_effect(
    QttSemanticFunction *semantic, const SemanticWrite *write) {
    char label[160];
    if (!qtt_place_effect_label(
            label, sizeof(label), "write", write->place))
        return false;
    QttSemanticNode *nodes = qtt_semantic_ir_nodes(semantic);
    size_t count = qtt_semantic_ir_node_count(semantic);
    for (size_t i = 0; i < count; i++)
        if (nodes[i].source == write->source)
            return qtt_semantic_ir_effect_label_count(
                       semantic, i, label) == 1;
    return false;
}

static bool block_reachable(
    const QttAnfProgram *program, size_t source, size_t target) {
    if (source >= program->block_count || target >= program->block_count)
        return false;
    bool *seen = calloc(program->block_count, sizeof(*seen));
    size_t *work = malloc(program->block_count * sizeof(*work));
    if (!seen || !work) {
        free(seen);
        free(work);
        return false;
    }
    size_t count = 0;
    work[count++] = source;
    seen[source] = true;
    while (count) {
        size_t block = work[--count];
        if (block == target) {
            free(seen);
            free(work);
            return true;
        }
        const QttAnfTerminator *term =
            &program->blocks[block].terminator;
        size_t successors[2];
        size_t successor_count = 0;
        if (term->kind == QTT_ANF_JUMP)
            successors[successor_count++] = term->target;
        else if (term->kind == QTT_ANF_BRANCH) {
            successors[successor_count++] = term->target;
            successors[successor_count++] = term->else_target;
        }
        for (size_t i = 0; i < successor_count; i++)
            if (successors[i] < program->block_count &&
                !seen[successors[i]]) {
                seen[successors[i]] = true;
                work[count++] = successors[i];
            }
    }
    free(seen);
    free(work);
    return false;
}

static bool instruction_precedes(
    const QttAnfProgram *program,
    size_t from_block, size_t from_instruction,
    size_t to_block, size_t to_instruction) {
    return from_block == to_block
        ? from_instruction < to_instruction
        : block_reachable(program, from_block, to_block);
}

static bool write_has_exclusive_authority(
    const QttAnfProgram *program, size_t write_block, size_t write_index,
    const QttAnfInstruction *write) {
    if (!write->loan_id) return false;
    size_t borrow_block = program->block_count;
    size_t borrow_index = 0;
    size_t definitions = 0;
    for (size_t block = 0; block < program->block_count; block++)
        for (size_t i = 0;
             i < program->blocks[block].instruction_count; i++) {
            const QttAnfInstruction *instruction =
                &program->blocks[block].instructions[i];
            if (instruction->kind != QTT_ANF_BORROW ||
                instruction->loan_id != write->loan_id)
                continue;
            definitions++;
            if (instruction->loan_kind != QTT_LOAN_EXCLUSIVE ||
                !qtt_place_equal(instruction->place, write->place))
                return false;
            borrow_block = block;
            borrow_index = i;
        }
    if (definitions != 1 ||
        !instruction_precedes(
            program, borrow_block, borrow_index,
            write_block, write_index))
        return false;

    for (size_t block = 0; block < program->block_count; block++)
        for (size_t i = 0;
             i < program->blocks[block].instruction_count; i++) {
            const QttAnfInstruction *instruction =
                &program->blocks[block].instructions[i];
            if (instruction->kind == QTT_ANF_END_BORROW &&
                instruction->loan_id == write->loan_id &&
                instruction_precedes(
                    program, write_block, write_index, block, i))
                return true;
        }
    return false;
}

static bool program_owns_root(
    const QttAnfProgram *program, QttCoreVar root) {
    for (size_t i = 0; i < program->initial_resource_count; i++)
        if (qtt_core_var_equal(program->initial_resources[i], root) &&
            (!program->initial_resource_owned ||
             program->initial_resource_owned[i]))
            return true;
    for (size_t block = 0; block < program->block_count; block++)
        for (size_t i = 0;
             i < program->blocks[block].instruction_count; i++) {
            const QttAnfInstruction *instruction =
                &program->blocks[block].instructions[i];
            if (instruction->kind == QTT_ANF_ALLOC &&
                instruction->representation == QTT_REP_OWNED_HEAP &&
                qtt_core_var_equal(instruction->resource, root))
                return true;
        }
    return false;
}

static bool writes_match(
    QttSemanticFunction *semantic,
    const QttCoreNode *core, const QttAnfProgram *program) {
    SemanticWrites expected = {.valid = true};
    collect_semantic_writes(core, &expected);
    if (!expected.valid) {
        free(expected.items);
        return false;
    }
    size_t cursor = 0;
    bool valid = true;
    for (size_t block = 0;
         block < program->block_count && valid; block++)
        for (size_t i = 0;
             i < program->blocks[block].instruction_count; i++) {
            const QttAnfInstruction *instruction =
                &program->blocks[block].instructions[i];
            if (instruction->kind != QTT_ANF_WRITE_PLACE &&
                instruction->kind != QTT_ANF_REPLACE)
                continue;
            if (cursor >= expected.count) {
                valid = false;
                break;
            }
            const SemanticWrite *write = &expected.items[cursor];
            if (!semantic_write_effect(semantic, write)) {
                valid = false;
                break;
            }
            bool owned_replace =
                write->representation == QTT_REP_OWNED_HEAP;
            if (instruction->kind !=
                    (owned_replace
                         ? QTT_ANF_REPLACE
                         : QTT_ANF_WRITE_PLACE) ||
                !qtt_place_equal(
                    instruction->place,
                    write->place) ||
                instruction->result_type !=
                    write->type ||
                (owned_replace &&
                 (!qtt_core_var_equal(
                      instruction->resource,
                      write->place.root) ||
                  instruction->representation !=
                      write->representation)) ||
                (instruction->kind == QTT_ANF_WRITE_PLACE &&
                 program_owns_root(program, instruction->place.root) &&
                 !write_has_exclusive_authority(
                     program, block, i, instruction))) {
                valid = false;
                break;
            }
            cursor++;
        }
    if (cursor != expected.count) valid = false;
    for (size_t i = 0;
         i < program->mutable_place_count && valid; i++) {
        bool found = false;
        for (size_t j = 0; j < expected.count; j++)
            if (expected.items[j].representation !=
                    QTT_REP_OWNED_HEAP &&
                qtt_place_equal(
                    program->mutable_places[i].place,
                    expected.items[j].place) &&
                program->mutable_places[i].type ==
                    expected.items[j].type) {
                found = true;
                break;
            }
        if (!found) valid = false;
    }
    for (size_t i = 0; i < expected.count && valid; i++) {
        if (expected.items[i].representation ==
                QTT_REP_OWNED_HEAP)
            continue;
        bool found = false;
        for (size_t j = 0;
             j < program->mutable_place_count; j++)
            if (qtt_place_equal(
                    expected.items[i].place,
                    program->mutable_places[j].place) &&
                expected.items[i].type ==
                    program->mutable_places[j].type) {
                found = true;
                break;
            }
        if (!found) valid = false;
    }
    free(expected.items);
    return valid;
}

static bool loans_match(
    QttSemanticFunction *semantic, const QttAnfProgram *program) {
    size_t expected_count = 0;
    QttSemanticNode *nodes = qtt_semantic_ir_nodes(semantic);
    size_t node_count = qtt_semantic_ir_node_count(semantic);
    for (size_t i = 0; i < node_count; i++)
        if (nodes[i].kind == QTT_CORE_BORROW) expected_count++;
    const QttAnfInstruction **actual = calloc(
        expected_count ? expected_count : 1, sizeof(*actual));
    if (!actual) return false;
    bool valid = true;
    size_t found = 0;
    for (size_t block = 0; block < program->block_count && valid; block++)
        for (size_t i = 0;
             i < program->blocks[block].instruction_count; i++) {
            const QttAnfInstruction *op =
                &program->blocks[block].instructions[i];
            if (!op->has_semantic_loan_identity) continue;
            if (op->kind != QTT_ANF_BORROW || !op->loan_id ||
                op->semantic_loan_ordinal >= expected_count ||
                actual[op->semantic_loan_ordinal]) {
                valid = false;
                break;
            }
            actual[op->semantic_loan_ordinal] = op;
            found++;
        }
    size_t ordinal = 0;
    for (size_t i = 0; i < node_count && valid; i++) {
        if (nodes[i].kind != QTT_CORE_BORROW) continue;
        const QttAnfInstruction *op = actual[ordinal];
        if (!op || !qtt_place_equal(op->place, nodes[i].place) ||
            op->loan_kind != nodes[i].loan_kind) {
            valid = false;
            break;
        }
        QttAnfLoanId expected_parent = 0;
        if (nodes[i].parent_loan.binder_id) {
            size_t prior_ordinal = 0;
            bool parent_found = false;
            for (size_t j = 0; j < i; j++) {
                if (nodes[j].kind != QTT_CORE_BORROW) continue;
                if (qtt_core_var_equal(
                        nodes[j].var, nodes[i].parent_loan)) {
                    expected_parent = actual[prior_ordinal]
                        ? actual[prior_ordinal]->loan_id : 0;
                    parent_found = expected_parent != 0;
                    break;
                }
                prior_ordinal++;
            }
            if (!parent_found) {
                valid = false;
                break;
            }
        }
        if (op->parent_loan_id != expected_parent) valid = false;
        ordinal++;
    }
    free(actual);
    return valid && found == expected_count && ordinal == expected_count;
}

QttSemanticAnfError qtt_semantic_anf_verify_correspondence(
    QttSemanticFunction *semantic,
    const QttCoreNode *core,
    const QttAnfProgram *program) {
    if (!semantic || !program ||
        qtt_semantic_ir_verify(semantic, core) !=
            QTT_SEMANTIC_IR_VALID)
        return QTT_SEMANTIC_ANF_INVALID_IR;
    if (!branch_identities_match(
            semantic, program, NULL))
        return QTT_SEMANTIC_ANF_UNSUPPORTED_CFG;
    if (!loans_match(semantic, program))
        return QTT_SEMANTIC_ANF_LOAN_MISMATCH;
    if (!writes_match(semantic, core, program))
        return QTT_SEMANTIC_ANF_EFFECT_MISMATCH;
    if (!transitions_match(semantic, program) ||
        qtt_anf_verify(program).error != QTT_ANF_VALID)
        return QTT_SEMANTIC_ANF_OWNERSHIP_MISMATCH;
    return QTT_SEMANTIC_ANF_OK;
}

QttAnfProgram *qtt_semantic_anf_lower(
    QttSemanticFunction *semantic,
    const QttCoreNode *core,
    QttSemanticAnfError *error,
    QttAnfLowerError *lower_error) {
    if (error) *error = QTT_SEMANTIC_ANF_INVALID_IR;
    if (lower_error) *lower_error = QTT_ANF_LOWER_INVALID_DEMAND;
    if (!semantic ||
        qtt_semantic_ir_verify(semantic, core) !=
            QTT_SEMANTIC_IR_VALID)
        return NULL;
    QttAnfLowerError local = QTT_ANF_LOWER_OK;
    QttAnfProgram *program =
        qtt_anf_lower_core(core, &local);
    if (lower_error) *lower_error = local;
    if (!program) {
        if (error) *error = QTT_SEMANTIC_ANF_LOWERING_FAILED;
        return NULL;
    }
    if (qtt_anf_infer_loan_regions(program).error != QTT_ANF_VALID) {
        qtt_anf_program_free(program);
        if (error) *error = QTT_SEMANTIC_ANF_OWNERSHIP_MISMATCH;
        return NULL;
    }
    QttSemanticAnfError verification =
        qtt_semantic_anf_verify_correspondence(
            semantic, core, program);
    if (verification != QTT_SEMANTIC_ANF_OK) {
        qtt_anf_program_free(program);
        if (error) *error = verification;
        return NULL;
    }
    if (error) *error = QTT_SEMANTIC_ANF_OK;
    return program;
}
