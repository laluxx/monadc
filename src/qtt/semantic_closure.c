#include "semantic_closure.h"

QttSemanticClosureError qtt_semantic_closure_verify_plan(
    QttSemanticFunction *semantic, const QttCoreNode *lambda,
    const QttClosurePlan *plan) {
    const QttCoreNode *source = qtt_semantic_ir_source(semantic);
    if (!source ||
        qtt_semantic_ir_verify(semantic, source) != QTT_SEMANTIC_IR_VALID)
        return QTT_SEMANTIC_CLOSURE_INVALID_IR;
    if (!lambda || lambda->kind != QTT_CORE_LAMBDA || !plan)
        return QTT_SEMANTIC_CLOSURE_NOT_FOUND;
    QttClosureError closure_error = QTT_CLOSURE_OK;
    const QttSignatureEnv *signatures =
        qtt_semantic_ir_signatures(semantic);
    uint64_t module_id =
        qtt_semantic_ir_module_id(semantic);
    bool plan_valid = signatures
        ? qtt_closure_plan_verify_in_env(
            plan, lambda, signatures, module_id,
            &closure_error)
        : qtt_closure_plan_verify(
            plan, lambda, &closure_error);
    if (!plan_valid)
        return QTT_SEMANTIC_CLOSURE_FIELD_MISMATCH;
    QttSemanticNode *nodes = qtt_semantic_ir_nodes(semantic);
    size_t node_count = qtt_semantic_ir_node_count(semantic);
    size_t node_index = node_count;
    for (size_t i = 0; i < node_count; i++)
        if (nodes[i].source == lambda) {
            node_index = i;
            break;
        }
    if (node_index == node_count)
        return QTT_SEMANTIC_CLOSURE_NOT_FOUND;
    QttSemanticClosureFieldEvidence *fields =
        qtt_semantic_ir_closure_fields(semantic);
    size_t field_count =
        qtt_semantic_ir_closure_field_count(semantic);
    size_t matched = 0;
    for (size_t i = 0; i < field_count; i++) {
        if (fields[i].node_index != node_index) continue;
        if (matched >= plan->capture_count)
            return QTT_SEMANTIC_CLOSURE_FIELD_MISMATCH;
        const QttClosureCapture *capture = &plan->captures[matched];
        bool descriptors_equal =
            (fields[i].descriptor == NULL &&
             capture->destructor == NULL) ||
            (fields[i].descriptor && capture->destructor &&
             qtt_destructor_id_equal(
                 fields[i].descriptor->id,
                 capture->destructor->id));
        if (fields[i].closure_id != nodes[node_index].closure_id ||
            fields[i].ordinal != matched ||
            !qtt_core_var_equal(fields[i].source, capture->source) ||
            fields[i].type != capture->type ||
            fields[i].representation != capture->representation ||
            fields[i].storage != plan->storage ||
            fields[i].exit != capture->exit ||
            fields[i].type_id.value == 0 ||
            ((fields[i].descriptor != NULL) !=
             (capture->destructor != NULL)) ||
            !descriptors_equal)
            return QTT_SEMANTIC_CLOSURE_FIELD_MISMATCH;
        matched++;
    }
    return matched == plan->capture_count
        ? QTT_SEMANTIC_CLOSURE_OK
        : QTT_SEMANTIC_CLOSURE_FIELD_MISMATCH;
}
