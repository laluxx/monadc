#ifndef MONAD_QTT_MODULE_H
#define MONAD_QTT_MODULE_H

#include "anf.h"
#include "semantic_anf.h"

typedef struct QttModule QttModule;

typedef enum {
    QTT_MODULE_OK,
    QTT_MODULE_OUT_OF_MEMORY,
    QTT_MODULE_INVALID_CORE,
    QTT_MODULE_INVALID_SIGNATURE,
    QTT_MODULE_DUPLICATE,
    QTT_MODULE_LOWERING_FAILED,
    QTT_MODULE_SEMANTIC_VERIFICATION_FAILED,
    QTT_MODULE_CORRESPONDENCE_FAILED,
    QTT_MODULE_VERIFICATION_FAILED,
} QttModuleError;

typedef struct {
    bool complete;
    size_t candidate_count;
    size_t proven_count;
    size_t rejected_count;
    size_t iterations;
} QttTransferFixedPoint;

typedef struct {
    bool converged;
    bool complete;
    size_t iterations;
    size_t changed_count;
    size_t incomplete_count;
} QttEffectFixedPoint;

typedef struct {
    QttModuleError error;
    size_t definition;
    size_t verified_count;
    QttAnfLowerError lower_error;
    QttAnfError anf_error;
    QttSemanticIrError semantic_error;
    QttSemanticIrValidation semantic_validation;
    QttSemanticAnfError correspondence_error;
    QttTransferFixedPoint transfers;
    QttEffectFixedPoint effects;
} QttModuleVerification;

/*
 * Declaration is intentionally separate from verification. This gives every
 * body in a recursive group access to the complete certified signature
 * environment without trusting declaration order.
 *
 * The module borrows Core nodes and owns derived signatures and type IDs.
 */
QttModule *qtt_module_new(uint64_t module_id);
QttModuleError qtt_module_declare(
    QttModule *module, const char *name, const QttCoreNode *lambda);
QttModuleError qtt_module_declare_external(
    QttModule *module, const char *name,
    const QttFunctionSignature *contract);
QttTransferFixedPoint qtt_module_solve_transfers(QttModule *module);
QttEffectFixedPoint qtt_module_solve_effects(QttModule *module);
QttModuleVerification qtt_module_verify(QttModule *module);
size_t qtt_module_count(const QttModule *module);
const QttSignatureEnv *qtt_module_signatures(const QttModule *module);
void qtt_module_free(QttModule *module);

#endif
