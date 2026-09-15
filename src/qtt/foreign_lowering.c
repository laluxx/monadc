#include "foreign_lowering.h"

#include <stdlib.h>

/* Foreign ARC lowering follows the explicit dup/drop discipline of Perceus:
 * https://www.microsoft.com/en-us/research/publication/perceus-garbage-free-reference-counting-with-reuse/
 */

typedef struct {
    QttCoreVar var;
    QttRepresentation representation;
} ForeignValue;

static ptrdiff_t find_value(
    const ForeignValue *values, size_t count, QttCoreVar var) {
    for (size_t i = 0; i < count; i++)
        if (qtt_core_var_equal(values[i].var, var)) return (ptrdiff_t)i;
    return -1;
}

QttForeignLowerError qtt_foreign_lower_certified(
    const QttResourceBlock *block, size_t operation_index,
    QttForeignLowering *lowering) {
    if (lowering) *lowering = (QttForeignLowering){0};
    if (!block || !lowering || operation_index >= block->count ||
        qtt_resource_verify(block, NULL, 0).error != QTT_RESOURCE_VALID)
        return QTT_FOREIGN_LOWER_INVALID_PROOF;

    ForeignValue *values = calloc(
        block->count ? block->count : 1, sizeof(*values));
    if (!values) return QTT_FOREIGN_LOWER_OUT_OF_MEMORY;
    size_t count = 0;
    QttRepresentation representation = QTT_REP_UNKNOWN;
    for (size_t i = 0; i <= operation_index; i++) {
        const QttResourceOp *op = &block->ops[i];
        ptrdiff_t found = find_value(values, count, op->var);
        if (i == operation_index && found >= 0)
            representation = values[(size_t)found].representation;
        if (op->kind == QTT_RESOURCE_ALLOC) {
            values[count++] = (ForeignValue){op->var, op->representation};
        } else if (op->kind == QTT_RESOURCE_DUP && found >= 0) {
            if (i == operation_index)
                representation = values[(size_t)found].representation;
            values[count++] = (ForeignValue){
                op->target, values[(size_t)found].representation};
        } else if ((op->kind == QTT_RESOURCE_MOVE ||
                    op->kind == QTT_RESOURCE_DROP) && found >= 0) {
            values[(size_t)found] = values[--count];
        }
    }
    free(values);
    if (representation != QTT_REP_FOREIGN)
        return QTT_FOREIGN_LOWER_NOT_FOREIGN;

    switch (block->ops[operation_index].kind) {
    case QTT_RESOURCE_MOVE:
        lowering->action = QTT_FOREIGN_RUNTIME_NONE;
        lowering->symbol = NULL;
        return QTT_FOREIGN_LOWER_OK;
    case QTT_RESOURCE_DROP:
        lowering->action = QTT_FOREIGN_RUNTIME_RELEASE;
        lowering->symbol = "monad_foreign_object_release";
        return QTT_FOREIGN_LOWER_OK;
    case QTT_RESOURCE_DUP:
        lowering->action = QTT_FOREIGN_RUNTIME_RETAIN_SHARED;
        lowering->symbol = "monad_foreign_object_retain_shared";
        return QTT_FOREIGN_LOWER_OK;
    default:
        return QTT_FOREIGN_LOWER_UNSUPPORTED_OPERATION;
    }
}
