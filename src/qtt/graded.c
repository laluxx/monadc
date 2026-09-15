#include "graded.h"

#include <stdlib.h>
#include <string.h>

struct QttGradedCertificate {
    const QttCoreNode *source;
    QttTypeId result_type;
    QttGradedBinder *binders;
    QttQuantity *observed;
    QttDemandDerivation **proofs;
    size_t count;
};

static bool context_valid(const QttGradedContext *context,
                          const QttTypeArena *types) {
    if (!context || (context->count && !context->binders)) return false;
    for (size_t i = 0; i < context->count; i++) {
        if (!context->binders[i].var.module_id ||
            !context->binders[i].var.binder_id ||
            !context->binders[i].type_id.value ||
            !qtt_type_lookup(types, context->binders[i].type_id))
            return false;
        for (size_t j = 0; j < i; j++)
            if (qtt_core_var_equal(context->binders[i].var,
                                   context->binders[j].var))
                return false;
    }
    return true;
}

static bool result_type_matches(const QttCoreNode *core,
                                QttTypeId expected,
                                QttTypeArena *types) {
    if (!core || !core->type || !expected.value ||
        !qtt_type_lookup(types, expected))
        return false;
    QttTypeIdentityError error = QTT_TYPE_IDENTITY_OK;
    QttTypeId actual = qtt_type_intern(types, core->type, &error);
    return actual.value && qtt_type_id_equal(actual, expected);
}

QttGradedCertificate *qtt_graded_check(
    const QttCoreNode *core, QttTypeId expected_type,
    const QttGradedContext *context, QttTypeArena *types,
    QttGradedError *error) {
    if (error) *error = QTT_GRADED_OK;
    if (!types || !context_valid(context, types)) {
        if (error) *error = QTT_GRADED_INVALID_CONTEXT;
        return NULL;
    }
    if (!result_type_matches(core, expected_type, types)) {
        if (error) *error = QTT_GRADED_TYPE_MISMATCH;
        return NULL;
    }
    QttDemandError demand_error = QTT_DEMAND_OK;
    QttDemandCertificate *demand =
        qtt_demand_derive(core, &demand_error);
    if (!demand) {
        if (error)
            *error = demand_error == QTT_DEMAND_OUT_OF_MEMORY
                ? QTT_GRADED_OUT_OF_MEMORY : QTT_GRADED_DEMAND_ERROR;
        return NULL;
    }
    QttGradedCertificate *certificate =
        calloc(1, sizeof(*certificate));
    if (certificate && context->count) {
        certificate->binders =
            malloc(context->count * sizeof(*certificate->binders));
        certificate->observed =
            calloc(context->count, sizeof(*certificate->observed));
        certificate->proofs =
            calloc(context->count, sizeof(*certificate->proofs));
    }
    if (!certificate ||
        (context->count &&
         (!certificate->binders || !certificate->observed ||
          !certificate->proofs))) {
        qtt_demand_certificate_free(demand);
        qtt_graded_certificate_free(certificate);
        if (error) *error = QTT_GRADED_OUT_OF_MEMORY;
        return NULL;
    }
    certificate->source = core;
    certificate->result_type = expected_type;
    certificate->count = context->count;
    if (context->count)
        memcpy(certificate->binders, context->binders,
               context->count * sizeof(*certificate->binders));
    for (size_t i = 0; i < context->count; i++) {
        certificate->proofs[i] = qtt_demand_prove(
            demand, core, context->binders[i].var, &demand_error);
        if (!certificate->proofs[i]) {
            qtt_demand_certificate_free(demand);
            qtt_graded_certificate_free(certificate);
            if (error)
                *error = demand_error == QTT_DEMAND_OUT_OF_MEMORY
                    ? QTT_GRADED_OUT_OF_MEMORY
                    : QTT_GRADED_DEMAND_ERROR;
            return NULL;
        }
        certificate->observed[i] =
            certificate->proofs[i]->runtime;
        if (!qtt_quantity_leq(
                certificate->observed[i],
                context->binders[i].allowance)) {
            qtt_demand_certificate_free(demand);
            qtt_graded_certificate_free(certificate);
            if (error) *error = QTT_GRADED_USAGE_EXCEEDED;
            return NULL;
        }
    }
    qtt_demand_certificate_free(demand);
    return certificate;
}

QttGradedProofValidation qtt_graded_validate(
    const QttGradedCertificate *certificate,
    const QttCoreNode *core, QttTypeId expected_type,
    const QttGradedContext *context, QttTypeArena *types) {
    if (!certificate || certificate->source != core)
        return QTT_GRADED_PROOF_SOURCE_MISMATCH;
    if (!types || !result_type_matches(core, expected_type, types) ||
        !qtt_type_id_equal(certificate->result_type, expected_type))
        return QTT_GRADED_PROOF_TYPE_MISMATCH;
    if (!context_valid(context, types) ||
        certificate->count != context->count)
        return QTT_GRADED_PROOF_CONTEXT_MISMATCH;
    for (size_t i = 0; i < context->count; i++) {
        const QttGradedBinder *actual = &context->binders[i];
        const QttGradedBinder *stored = &certificate->binders[i];
        if (!qtt_core_var_equal(actual->var, stored->var) ||
            !qtt_type_id_equal(actual->type_id, stored->type_id) ||
            !qtt_quantity_equal(actual->allowance,
                                stored->allowance))
            return QTT_GRADED_PROOF_CONTEXT_MISMATCH;
        if (!certificate->proofs[i] ||
            qtt_demand_check(
                certificate->proofs[i], core, stored->var) !=
                QTT_DEMAND_PROOF_VALID ||
            !qtt_quantity_equal(
                certificate->observed[i],
                certificate->proofs[i]->runtime) ||
            !qtt_quantity_leq(
                certificate->observed[i], stored->allowance))
            return QTT_GRADED_PROOF_DEMAND_MISMATCH;
    }
    return QTT_GRADED_PROOF_VALID;
}

QttQuantity qtt_graded_observed(
    const QttGradedCertificate *certificate, QttCoreVar var) {
    if (certificate)
        for (size_t i = 0; i < certificate->count; i++)
            if (qtt_core_var_equal(
                    certificate->binders[i].var, var))
                return certificate->observed[i];
    return qtt_quantity_finite(0);
}

bool qtt_graded_is_erased(
    const QttGradedCertificate *certificate, QttCoreVar var) {
    if (!certificate) return false;
    for (size_t i = 0; i < certificate->count; i++)
        if (qtt_core_var_equal(certificate->binders[i].var, var))
            return qtt_quantity_is_zero(
                       certificate->binders[i].allowance) &&
                   qtt_quantity_is_zero(certificate->observed[i]);
    return false;
}

void qtt_graded_certificate_free(QttGradedCertificate *certificate) {
    if (!certificate) return;
    for (size_t i = 0; i < certificate->count; i++)
        qtt_demand_derivation_free(certificate->proofs[i]);
    free(certificate->proofs);
    free(certificate->observed);
    free(certificate->binders);
    free(certificate);
}
