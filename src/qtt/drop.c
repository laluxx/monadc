#include "drop.h"

#include <stdlib.h>
#include <string.h>

typedef struct PlanMemo {
    const Type *type;
    QttDropPlan *plan;
    struct PlanMemo *next;
} PlanMemo;

static QttDropPlan *plan_new(QttDropKind kind, size_t child_count,
                             QttDropPlanError *error) {
    QttDropPlan *plan = calloc(1, sizeof(*plan));
    if (!plan) {
        *error = QTT_DROP_PLAN_OUT_OF_MEMORY;
        return NULL;
    }
    plan->kind = kind;
    plan->child_count = child_count;
    if (child_count) {
        plan->children = calloc(child_count, sizeof(*plan->children));
        if (!plan->children) {
            free(plan);
            *error = QTT_DROP_PLAN_OUT_OF_MEMORY;
            return NULL;
        }
    }
    return plan;
}

void qtt_drop_plan_free(QttDropPlan *plan) {
    if (!plan) return;
    if (plan->kind == QTT_DROP_BACKREF) {
        free(plan);
        return;
    }
    for (size_t i = 0; i < plan->child_count; i++)
        qtt_drop_plan_free(plan->children[i]);
    free(plan->children);
    free(plan);
}

static QttDropPlan *memo_backref(const PlanMemo *memo, const Type *type,
                                 QttDropPlanError *error) {
    for (; memo; memo = memo->next) {
        if (memo->type != type) continue;
        QttDropPlan *backref = plan_new(QTT_DROP_BACKREF, 0, error);
        if (backref) backref->backref_target = memo->plan;
        return backref;
    }
    return NULL;
}

static bool memo_push(PlanMemo **memo, const Type *type, QttDropPlan *plan) {
    PlanMemo *entry = malloc(sizeof(*entry));
    if (!entry) return false;
    entry->type = type;
    entry->plan = plan;
    entry->next = *memo;
    *memo = entry;
    return true;
}

static QttDropPlan *build_plan(const Type *type, PlanMemo **memo,
                               QttDropPlanError *error) {
    QttRepresentation representation = qtt_resource_classify_type(type);
    if (representation == QTT_REP_UNKNOWN) {
        *error = QTT_DROP_PLAN_UNKNOWN_REPRESENTATION;
        return NULL;
    }
    if (representation == QTT_REP_IMMEDIATE ||
        representation == QTT_REP_FOREIGN)
        return plan_new(QTT_DROP_NOOP, 0, error);
    /*
     * Inline is a storage representation, not proof that destruction is a
     * no-op. Inline layouts and fixed arrays can contain owned children and
     * therefore need a structural plan even though their outer cell is not
     * separately allocated.
     */
    if (representation == QTT_REP_INLINE &&
        type->kind != TYPE_LAYOUT && type->kind != TYPE_ARR)
        return plan_new(QTT_DROP_NOOP, 0, error);
    QttDropPlan *backref = memo_backref(*memo, type, error);
    if (backref) return backref;
    if (type->kind == TYPE_LAYOUT && type->layout_field_count > 0) {
        QttDropPlan *plan =
            plan_new(QTT_DROP_STRUCT, (size_t)type->layout_field_count, error);
        if (!plan) return NULL;
        if (!memo_push(memo, type, plan)) {
            *error = QTT_DROP_PLAN_OUT_OF_MEMORY;
            qtt_drop_plan_free(plan);
            return NULL;
        }
        for (size_t i = 0; i < plan->child_count; i++) {
            plan->children[i] =
                build_plan(type->layout_fields[i].type, memo, error);
            if (!plan->children[i]) {
                qtt_drop_plan_free(plan);
                return NULL;
            }
        }
        return plan;
    }
    if (type->kind == TYPE_OPTIONAL && type->element_type) {
        QttDropPlan *plan = plan_new(QTT_DROP_OPTIONAL, 1, error);
        if (!plan) return NULL;
        if (!memo_push(memo, type, plan)) {
            *error = QTT_DROP_PLAN_OUT_OF_MEMORY;
            qtt_drop_plan_free(plan);
            return NULL;
        }
        plan->children[0] = build_plan(type->element_type, memo, error);
        if (!plan->children[0]) {
            qtt_drop_plan_free(plan);
            return NULL;
        }
        return plan;
    }
    const Type *element = NULL;
    if (type->kind == TYPE_LIST && type->list_count > 0)
        element = type->list_types[0];
    else if (type->kind == TYPE_ARR)
        element = type->arr_element_type;
    else if (type->kind == TYPE_SET || type->kind == TYPE_COLL)
        element = type->element_type;
    if (element) {
        QttDropPlan *plan = plan_new(QTT_DROP_SEQUENCE, 1, error);
        if (!plan) return NULL;
        if (!memo_push(memo, type, plan)) {
            *error = QTT_DROP_PLAN_OUT_OF_MEMORY;
            qtt_drop_plan_free(plan);
            return NULL;
        }
        plan->children[0] = build_plan(element, memo, error);
        if (!plan->children[0]) {
            qtt_drop_plan_free(plan);
            return NULL;
        }
        return plan;
    }
    return plan_new(QTT_DROP_OWNED_LEAF, 0, error);
}

QttDropPlan *qtt_drop_plan_build(const Type *type, QttDropPlanError *error) {
    QttDropPlanError local = QTT_DROP_PLAN_OK;
    PlanMemo *memo = NULL;
    QttDropPlan *plan = build_plan(type, &memo, &local);
    while (memo) {
        PlanMemo *next = memo->next;
        free(memo);
        memo = next;
    }
    if (error) *error = local;
    return plan;
}

static uint64_t destructor_mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t plan_fingerprint_at(
    const QttDropPlan *plan, const QttDropPlan **stack,
    size_t depth) {
    if (!plan || depth >= 128)
        return UINT64_C(0x9e3779b97f4a7c15);
    uint64_t hash = destructor_mix(
        UINT64_C(1469598103934665603),
        (uint64_t)plan->kind + 1);
    if (plan->kind == QTT_DROP_BACKREF) {
        size_t distance = 0;
        for (size_t i = depth; i > 0; i--)
            if (stack[i - 1] == plan->backref_target) {
                distance = depth - (i - 1);
                break;
            }
        return destructor_mix(hash, distance);
    }
    hash = destructor_mix(hash, plan->child_count);
    stack[depth] = plan;
    for (size_t i = 0; i < plan->child_count; i++)
        hash = destructor_mix(
            hash, plan_fingerprint_at(
                      plan->children[i], stack, depth + 1));
    return hash;
}

uint64_t qtt_drop_plan_fingerprint(const QttDropPlan *plan) {
    const QttDropPlan *stack[128] = {0};
    return plan_fingerprint_at(plan, stack, 0);
}

QttDestructorDescriptor *qtt_destructor_descriptor_build(
    const Type *type, QttDropPlanError *error) {
    QttDropPlan *plan = qtt_drop_plan_build(type, error);
    if (!plan) return NULL;
    QttDestructorDescriptor *descriptor =
        malloc(sizeof(*descriptor));
    if (!descriptor) {
        qtt_drop_plan_free(plan);
        if (error) *error = QTT_DROP_PLAN_OUT_OF_MEMORY;
        return NULL;
    }
    descriptor->plan = plan;
    descriptor->id = (QttDestructorId){
        .type_fingerprint = qtt_type_fingerprint(type),
        .plan_fingerprint =
            qtt_drop_plan_fingerprint(plan),
    };
    if (error) *error = QTT_DROP_PLAN_OK;
    return descriptor;
}

bool qtt_destructor_descriptor_verify(
    const QttDestructorDescriptor *descriptor, const Type *type) {
    return descriptor && descriptor->plan && type &&
        descriptor->id.type_fingerprint ==
            qtt_type_fingerprint(type) &&
        descriptor->id.plan_fingerprint ==
            qtt_drop_plan_fingerprint(descriptor->plan);
}

bool qtt_destructor_id_equal(
    QttDestructorId left, QttDestructorId right) {
    return left.type_fingerprint == right.type_fingerprint &&
           left.plan_fingerprint == right.plan_fingerprint;
}

void qtt_destructor_descriptor_free(
    QttDestructorDescriptor *descriptor) {
    if (!descriptor) return;
    qtt_drop_plan_free(descriptor->plan);
    free(descriptor);
}

static int place_path_compare(const void *left_pointer,
                              const void *right_pointer) {
    const QttPlace *left = left_pointer;
    const QttPlace *right = right_pointer;
    uint8_t common = left->projection_depth < right->projection_depth
        ? left->projection_depth : right->projection_depth;
    for (uint8_t i = 0; i < common; i++) {
        if (left->projection_path[i] < right->projection_path[i]) return -1;
        if (left->projection_path[i] > right->projection_path[i]) return 1;
    }
    if (left->projection_depth < right->projection_depth) return -1;
    if (left->projection_depth > right->projection_depth) return 1;
    return 0;
}

static uint64_t mask_fingerprint(const QttDropMaskCertificate *certificate) {
    uint64_t hash = destructor_mix(
        certificate->destructor.plan_fingerprint,
        certificate->destructor.type_fingerprint);
    hash = destructor_mix(hash, certificate->root.root.module_id);
    hash = destructor_mix(hash, certificate->root.root.binder_id);
    hash = destructor_mix(hash, certificate->evacuated_count);
    for (size_t i = 0; i < certificate->evacuated_count; i++) {
        const QttPlace *place = &certificate->evacuated[i];
        hash = destructor_mix(hash, place->projection_depth);
        for (uint8_t j = 0; j < place->projection_depth; j++)
            hash = destructor_mix(hash, place->projection_path[j]);
    }
    return hash;
}

static bool mask_path_follows_plan(const QttDropPlan *plan,
                                   const QttPlace *place) {
    for (uint8_t depth = 0; depth < place->projection_depth; depth++) {
        while (plan && plan->kind == QTT_DROP_BACKREF)
            plan = plan->backref_target;
        if (!plan || plan->kind != QTT_DROP_STRUCT) return false;
        uint32_t ordinal = place->projection_path[depth];
        if (ordinal == 0 || ordinal > plan->child_count) return false;
        plan = plan->children[ordinal - 1];
    }
    return true;
}

QttDropMaskError qtt_drop_mask_verify(
    const QttDestructorDescriptor *descriptor,
    const QttDropMaskCertificate *certificate) {
    if (!descriptor || !descriptor->plan || !certificate ||
        descriptor->id.plan_fingerprint !=
            qtt_drop_plan_fingerprint(descriptor->plan) ||
        !qtt_destructor_id_equal(descriptor->id,
                                 certificate->destructor))
        return QTT_DROP_MASK_INVALID_DESTRUCTOR;
    if (certificate->root.projection_id != 0 ||
        certificate->root.projection_depth != 0)
        return QTT_DROP_MASK_INVALID_ROOT;
    if (certificate->evacuated_count && !certificate->evacuated)
        return QTT_DROP_MASK_INVALID_PATH;
    for (size_t i = 0; i < certificate->evacuated_count; i++) {
        const QttPlace *place = &certificate->evacuated[i];
        if (place->root.module_id != certificate->root.root.module_id ||
            place->root.binder_id != certificate->root.root.binder_id ||
            place->projection_depth == 0 ||
            place->projection_depth > QTT_PLACE_MAX_DEPTH ||
            place->projection_id != place->projection_path[0] ||
            !mask_path_follows_plan(descriptor->plan, place))
            return QTT_DROP_MASK_INVALID_PATH;
        if (i > 0) {
            int order = place_path_compare(&certificate->evacuated[i - 1],
                                           place);
            if (order > 0) return QTT_DROP_MASK_INVALID_PATH;
            if (qtt_place_overlaps(certificate->evacuated[i - 1], *place))
                return QTT_DROP_MASK_OVERLAP;
        }
    }
    if (certificate->certificate_fingerprint !=
        mask_fingerprint(certificate))
        return QTT_DROP_MASK_INVALID_PATH;
    return QTT_DROP_MASK_VALID;
}

QttDropMaskCertificate *qtt_drop_mask_build(
    const QttDestructorDescriptor *descriptor, QttPlace root,
    const QttPlace *evacuated, size_t evacuated_count,
    QttDropMaskError *error) {
    QttDropMaskError local = QTT_DROP_MASK_VALID;
    QttDropMaskCertificate *certificate = calloc(1, sizeof(*certificate));
    if (!certificate) {
        local = QTT_DROP_MASK_OUT_OF_MEMORY;
        goto failed;
    }
    if (descriptor) certificate->destructor = descriptor->id;
    certificate->root = root;
    certificate->evacuated_count = evacuated_count;
    if (evacuated_count) {
        if (!evacuated) {
            local = QTT_DROP_MASK_INVALID_PATH;
            goto failed;
        }
        certificate->evacuated =
            malloc(evacuated_count * sizeof(*certificate->evacuated));
        if (!certificate->evacuated) {
            local = QTT_DROP_MASK_OUT_OF_MEMORY;
            goto failed;
        }
        memcpy(certificate->evacuated, evacuated,
               evacuated_count * sizeof(*evacuated));
        qsort(certificate->evacuated, evacuated_count,
              sizeof(*certificate->evacuated), place_path_compare);
    }
    certificate->certificate_fingerprint = mask_fingerprint(certificate);
    local = qtt_drop_mask_verify(descriptor, certificate);
    if (local != QTT_DROP_MASK_VALID) goto failed;
    if (error) *error = local;
    return certificate;

failed:
    if (certificate) {
        free(certificate->evacuated);
        free(certificate);
    }
    if (error) *error = local;
    return NULL;
}

typedef struct {
    QttPlace *places;
    size_t count;
    size_t capacity;
    bool found_drop;
} EvacuationTrace;

static void evacuation_trace_free(EvacuationTrace *trace) {
    free(trace->places);
    *trace = (EvacuationTrace){0};
}

static bool evacuation_trace_clone(EvacuationTrace *destination,
                                    const EvacuationTrace *source) {
    *destination = (EvacuationTrace){.found_drop = source->found_drop};
    if (!source->count) return true;
    destination->places = malloc(
        source->count * sizeof(*destination->places));
    if (!destination->places) return false;
    memcpy(destination->places, source->places,
           source->count * sizeof(*destination->places));
    destination->count = source->count;
    destination->capacity = source->count;
    return true;
}

static bool evacuation_trace_equal(const EvacuationTrace *left,
                                    const EvacuationTrace *right) {
    if (left->count != right->count ||
        left->found_drop != right->found_drop)
        return false;
    for (size_t i = 0; i < left->count; i++) {
        bool found = false;
        for (size_t j = 0; j < right->count; j++)
            if (qtt_place_equal(left->places[i], right->places[j])) {
                found = true;
                break;
            }
        if (!found) return false;
    }
    return true;
}

static bool evacuation_trace_push(EvacuationTrace *trace, QttPlace place) {
    if (trace->count == trace->capacity) {
        size_t next = trace->capacity ? trace->capacity * 2 : 4;
        QttPlace *grown = realloc(
            trace->places, next * sizeof(*grown));
        if (!grown) return false;
        trace->places = grown;
        trace->capacity = next;
    }
    trace->places[trace->count++] = place;
    return true;
}

static bool trace_resource_evacuations(const QttResourceBlock *block,
                                       QttCoreVar root,
                                       EvacuationTrace *trace) {
    if (!block) return false;
    for (size_t i = 0; i < block->count; i++) {
        const QttResourceOp *op = &block->ops[i];
        if (op->kind == QTT_RESOURCE_BRANCH) {
            EvacuationTrace then_trace = {0};
            EvacuationTrace else_trace = {0};
            bool valid =
                evacuation_trace_clone(&then_trace, trace) &&
                evacuation_trace_clone(&else_trace, trace) &&
                trace_resource_evacuations(
                    op->branch.then_block, root, &then_trace) &&
                trace_resource_evacuations(
                    op->branch.else_block, root, &else_trace) &&
                evacuation_trace_equal(&then_trace, &else_trace);
            if (valid) {
                evacuation_trace_free(trace);
                *trace = then_trace;
                then_trace = (EvacuationTrace){0};
            }
            evacuation_trace_free(&then_trace);
            evacuation_trace_free(&else_trace);
            if (!valid) return false;
            continue;
        }
        if ((op->kind == QTT_RESOURCE_MOVE_ALIAS ||
             op->kind == QTT_RESOURCE_MOVE_PLACE) &&
            qtt_core_var_equal(op->place.root, root) &&
            op->place.projection_depth > 0) {
            if (trace->found_drop ||
                !evacuation_trace_push(trace, op->place))
                return false;
            continue;
        }
        if ((op->kind == QTT_RESOURCE_MOVE ||
             op->kind == QTT_RESOURCE_DROP) &&
            qtt_core_var_equal(op->var, root)) {
            if (trace->found_drop) return false;
            if (op->kind == QTT_RESOURCE_MOVE) return false;
            trace->found_drop = true;
        }
    }
    return true;
}

QttDropMaskCertificate *qtt_drop_mask_build_for_resource(
    const QttDestructorDescriptor *descriptor,
    const QttResourceBlock *resources, QttCoreVar root,
    const QttCoreVar *initially_owned, size_t initially_owned_count,
    QttDropMaskError *error) {
    QttResourceVerification verification = qtt_resource_verify(
        resources, initially_owned, initially_owned_count);
    if (verification.error != QTT_RESOURCE_VALID) {
        if (error) *error = QTT_DROP_MASK_INVALID_PATH;
        return NULL;
    }
    EvacuationTrace trace = {0};
    if (!trace_resource_evacuations(resources, root, &trace) ||
        !trace.found_drop) {
        evacuation_trace_free(&trace);
        if (error) *error = QTT_DROP_MASK_INVALID_PATH;
        return NULL;
    }
    QttDropMaskCertificate *certificate = qtt_drop_mask_build(
        descriptor, qtt_place_root(root), trace.places,
        trace.count, error);
    evacuation_trace_free(&trace);
    return certificate;
}

void qtt_drop_mask_free(QttDropMaskCertificate *certificate) {
    if (!certificate) return;
    free(certificate->evacuated);
    free(certificate);
}

QttOwnedObject *qtt_owned_object_new(uint64_t identity, size_t child_count) {
    QttOwnedObject *object = calloc(1, sizeof(*object));
    if (!object) return NULL;
    object->identity = identity;
    object->references = 1;
    object->child_count = child_count;
    if (child_count) {
        object->children = calloc(child_count, sizeof(*object->children));
        if (!object->children) {
            free(object);
            return NULL;
        }
    }
    return object;
}

bool qtt_owned_object_retain(QttOwnedObject *object) {
    if (!object || object->dropped || object->references == UINT64_MAX)
        return false;
    object->references++;
    return true;
}

void qtt_owned_object_free_storage(QttOwnedObject *object) {
    if (!object) return;
    free(object->children);
    free(object);
}

static void execute_plan(const QttDropPlan *plan, QttOwnedObject *object,
                         QttDropExecution *execution) {
    if (execution->error != QTT_DROP_VALID || plan->kind == QTT_DROP_NOOP)
        return;
    if (plan->kind == QTT_DROP_OPTIONAL) {
        if (object)
            execute_plan(plan->children[0], object, execution);
        return;
    }
    if (plan->kind == QTT_DROP_BACKREF) {
        execute_plan(plan->backref_target, object, execution);
        return;
    }
    if (!object) {
        execution->error = QTT_DROP_SHAPE_MISMATCH;
        return;
    }
    if (object->dropped || object->references == 0) {
        execution->error = QTT_DROP_DOUBLE;
        return;
    }
    if (plan->kind == QTT_DROP_STRUCT &&
        object->child_count != plan->child_count) {
        execution->error = QTT_DROP_SHAPE_MISMATCH;
        return;
    }
    object->references--;
    if (object->references > 0) return;
    object->dropped = true;
    execution->visited++;
    execution->reclaimed++;
    if (plan->kind == QTT_DROP_SEQUENCE) {
        for (size_t i = 0; i < object->child_count; i++)
            execute_plan(plan->children[0], object->children[i], execution);
        return;
    }
    for (size_t i = 0; i < plan->child_count; i++)
        execute_plan(plan->children[i], object->children[i], execution);
}

static bool mask_evacuates(const QttDropMaskCertificate *certificate,
                           const uint32_t *path, uint8_t depth) {
    for (size_t i = 0; i < certificate->evacuated_count; i++) {
        const QttPlace *place = &certificate->evacuated[i];
        if (place->projection_depth != depth) continue;
        bool equal = true;
        for (uint8_t j = 0; j < depth; j++)
            if (place->projection_path[j] != path[j]) {
                equal = false;
                break;
            }
        if (equal) return true;
    }
    return false;
}

static void execute_plan_masked(
    const QttDropPlan *plan, QttOwnedObject *object,
    const QttDropMaskCertificate *certificate,
    uint32_t path[QTT_PLACE_MAX_DEPTH], uint8_t depth,
    QttDropExecution *execution) {
    if (depth && mask_evacuates(certificate, path, depth)) return;
    if (execution->error != QTT_DROP_VALID || plan->kind == QTT_DROP_NOOP)
        return;
    if (plan->kind == QTT_DROP_OPTIONAL) {
        if (object)
            execute_plan_masked(plan->children[0], object, certificate,
                                path, depth, execution);
        return;
    }
    if (plan->kind == QTT_DROP_BACKREF) {
        execute_plan_masked(plan->backref_target, object, certificate,
                            path, depth, execution);
        return;
    }
    if (!object) {
        execution->error = QTT_DROP_SHAPE_MISMATCH;
        return;
    }
    if (object->dropped || object->references == 0) {
        execution->error = QTT_DROP_DOUBLE;
        return;
    }
    if (plan->kind == QTT_DROP_STRUCT &&
        object->child_count != plan->child_count) {
        execution->error = QTT_DROP_SHAPE_MISMATCH;
        return;
    }
    object->references--;
    if (object->references > 0) return;
    object->dropped = true;
    execution->visited++;
    execution->reclaimed++;
    if (plan->kind == QTT_DROP_SEQUENCE) {
        for (size_t i = 0; i < object->child_count; i++)
            execute_plan_masked(plan->children[0], object->children[i],
                                certificate, path, depth, execution);
        return;
    }
    for (size_t i = 0; i < plan->child_count; i++) {
        if (depth >= QTT_PLACE_MAX_DEPTH) {
            execution->error = QTT_DROP_SHAPE_MISMATCH;
            return;
        }
        path[depth] = (uint32_t)i + 1;
        execute_plan_masked(plan->children[i], object->children[i],
                            certificate, path, depth + 1, execution);
    }
}

QttDropExecution qtt_drop_execute(const QttDropPlan *plan,
                                  QttOwnedObject *root) {
    QttDropExecution execution = {.error = QTT_DROP_VALID};
    if (!plan) {
        execution.error = QTT_DROP_SHAPE_MISMATCH;
        return execution;
    }
    execute_plan(plan, root, &execution);
    execution.live =
        execution.error == QTT_DROP_VALID ? 0 : 1;
    return execution;
}

QttDropExecution qtt_drop_execute_masked(
    const QttDestructorDescriptor *descriptor,
    const QttDropMaskCertificate *certificate,
    QttOwnedObject *root) {
    QttDropExecution execution = {.error = QTT_DROP_VALID};
    if (qtt_drop_mask_verify(descriptor, certificate) !=
        QTT_DROP_MASK_VALID) {
        execution.error = QTT_DROP_INVALID_MASK;
        execution.live = root && !root->dropped ? 1 : 0;
        return execution;
    }
    uint32_t path[QTT_PLACE_MAX_DEPTH] = {0};
    execute_plan_masked(descriptor->plan, root, certificate,
                        path, 0, &execution);
    execution.live = execution.error == QTT_DROP_VALID ? 0 : 1;
    return execution;
}
