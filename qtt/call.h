#ifndef MONAD_QTT_CALL_H
#define MONAD_QTT_CALL_H

#include "signature.h"

typedef enum {
    QTT_CALL_VALUE,
    QTT_CALL_BORROW,
    QTT_CALL_MOVE,
} QttCallTransfer;

typedef struct {
    const Type *type;
    QttTypeId type_id;
    QttRepresentation representation;
    QttCallTransfer transfer;
} QttCallArgument;

typedef struct {
    QttCallArgument *arguments;
    size_t argument_count;
    const Type *result_type;
    QttResultMode result_mode;
    QttRepresentation result_representation;
} QttCallPlan;

typedef enum {
    QTT_CALL_VALID,
    QTT_CALL_ARITY_MISMATCH,
    QTT_CALL_TYPE_MISMATCH,
    QTT_CALL_REPRESENTATION_MISMATCH,
    QTT_CALL_TRANSFER_MISMATCH,
    QTT_CALL_OUT_OF_MEMORY,
} QttCallValidation;

QttCallTransfer qtt_call_transfer_for_parameter(
    const QttParameterContract *parameter);
QttCallValidation qtt_call_plan(
    const QttFunctionSignature *signature,
    const QttCallArgument *arguments,
    size_t argument_count,
    QttCallPlan *plan);
void qtt_call_plan_free(QttCallPlan *plan);

#endif
