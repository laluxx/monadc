#include "call.h"

#include <stdlib.h>
#include <string.h>

static bool call_type_equal(const Type *expected, const Type *actual) {
    if (expected == actual) return true;
    if (!expected || !actual || expected->kind != actual->kind) return false;

    /*
     * Primitive types are canonical by kind. Composite types require the same
     * zonked identity until typed Core carries stable interned type IDs.
     */
    switch (expected->kind) {
    case TYPE_UNIT: case TYPE_NIL: case TYPE_BOOL:
    case TYPE_INT: case TYPE_FLOAT: case TYPE_F32: case TYPE_F80:
    case TYPE_CHAR: case TYPE_BYTE: case TYPE_STRING:
    case TYPE_I8: case TYPE_U8: case TYPE_I16: case TYPE_U16:
    case TYPE_I32: case TYPE_U32: case TYPE_I64: case TYPE_U64:
    case TYPE_I128: case TYPE_U128: case TYPE_INT_ARBITRARY:
        return true;
    default:
        return false;
    }
}

QttCallTransfer qtt_call_transfer_for_parameter(
    const QttParameterContract *parameter) {
    if (!parameter ||
        (parameter->representation != QTT_REP_OWNED_HEAP &&
         parameter->representation != QTT_REP_FOREIGN))
        return QTT_CALL_VALUE;
    if (parameter->mode == QTT_OWNERSHIP_CONSUMED)
        return QTT_CALL_MOVE;
    if (parameter->mode == QTT_OWNERSHIP_BORROWED ||
        parameter->mode == QTT_OWNERSHIP_SHARED)
        return QTT_CALL_BORROW;
    return QTT_CALL_VALUE;
}

QttCallValidation qtt_call_plan(
    const QttFunctionSignature *signature,
    const QttCallArgument *arguments,
    size_t argument_count,
    QttCallPlan *plan) {
    if (plan) memset(plan, 0, sizeof(*plan));
    if (!signature || !plan ||
        argument_count != signature->parameter_count)
        return QTT_CALL_ARITY_MISMATCH;
    if (argument_count && !arguments)
        return QTT_CALL_ARITY_MISMATCH;

    for (size_t i = 0; i < argument_count; i++) {
        const QttParameterContract *parameter = &signature->parameters[i];
        if (parameter->type_id.value || arguments[i].type_id.value) {
            if (!qtt_type_id_equal(
                    parameter->type_id, arguments[i].type_id) ||
                !call_type_equal(parameter->type, arguments[i].type))
                return QTT_CALL_TYPE_MISMATCH;
        } else if (!call_type_equal(parameter->type, arguments[i].type)) {
            return QTT_CALL_TYPE_MISMATCH;
        }
        if (parameter->representation != arguments[i].representation)
            return QTT_CALL_REPRESENTATION_MISMATCH;
        if (parameter->representation == QTT_REP_FOREIGN &&
            !qtt_nominal_authority_equal(
                parameter->nominal_authority,
                arguments[i].nominal_authority))
            return QTT_CALL_NOMINAL_MISMATCH;
        if (qtt_call_transfer_for_parameter(parameter) !=
            arguments[i].transfer)
            return QTT_CALL_TRANSFER_MISMATCH;
    }

    if (argument_count) {
        plan->arguments = malloc(argument_count * sizeof(*plan->arguments));
        if (!plan->arguments) return QTT_CALL_OUT_OF_MEMORY;
        memcpy(plan->arguments, arguments,
               argument_count * sizeof(*plan->arguments));
    }
    plan->argument_count = argument_count;
    plan->result_type = signature->result_type;
    plan->result_representation =
        signature->result.type
            ? signature->result.representation
            : qtt_resource_classify_type(signature->result_type);
    plan->result_mode = signature->result.type
        ? signature->result.mode
        : plan->result_representation == QTT_REP_IMMEDIATE
            ? QTT_RESULT_IMMEDIATE
            : plan->result_representation == QTT_REP_OWNED_HEAP
                ? QTT_RESULT_OWNED
                : QTT_RESULT_UNKNOWN;
    plan->result_nominal_authority = signature->result.nominal_authority;
    return QTT_CALL_VALID;
}

void qtt_call_plan_free(QttCallPlan *plan) {
    if (!plan) return;
    free(plan->arguments);
    memset(plan, 0, sizeof(*plan));
}
