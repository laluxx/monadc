#ifndef MONAD_QTT_SEMANTIC_ANF_H
#define MONAD_QTT_SEMANTIC_ANF_H

#include "anf.h"
#include "semantic_ir.h"

typedef enum {
    QTT_SEMANTIC_ANF_OK,
    QTT_SEMANTIC_ANF_INVALID_IR,
    QTT_SEMANTIC_ANF_UNSUPPORTED_CFG,
    QTT_SEMANTIC_ANF_OWNERSHIP_MISMATCH,
    QTT_SEMANTIC_ANF_CALL_MISMATCH,
    QTT_SEMANTIC_ANF_ENVIRONMENT_MISMATCH,
    QTT_SEMANTIC_ANF_PROJECTION_MISMATCH,
    QTT_SEMANTIC_ANF_EFFECT_MISMATCH,
    QTT_SEMANTIC_ANF_LOAN_MISMATCH,
    QTT_SEMANTIC_ANF_LOWERING_FAILED,
} QttSemanticAnfError;

QttSemanticAnfError qtt_semantic_anf_verify_correspondence(
    QttSemanticFunction *semantic,
    const QttCoreNode *core,
    const QttAnfProgram *program);
QttSemanticAnfError qtt_semantic_anf_verify_calls(
    QttSemanticFunction *semantic,
    const QttCoreNode *core,
    const QttAnfProgram *program);
QttSemanticAnfError qtt_semantic_anf_materialize_environment(
    QttSemanticFunction *semantic,
    const QttCoreNode *lambda,
    QttAnfProgram *program);
QttSemanticAnfError qtt_semantic_anf_verify_environment(
    QttSemanticFunction *semantic,
    const QttCoreNode *lambda,
    const QttAnfProgram *program);
QttSemanticAnfError qtt_semantic_anf_verify_projections(
    QttSemanticFunction *semantic,
    const QttCoreNode *lambda,
    const QttAnfProgram *program);
QttAnfProgram *qtt_semantic_anf_lower_closure_body(
    QttSemanticFunction *semantic,
    const QttCoreNode *lambda,
    QttSemanticAnfError *error,
    QttAnfLowerError *lower_error);
QttAnfProgram *qtt_semantic_anf_lower_closure_body_in_env(
    QttSemanticFunction *semantic,
    const QttCoreNode *lambda,
    const QttSignatureEnv *signatures,
    uint64_t module_id,
    QttSemanticAnfError *error,
    QttAnfLowerError *lower_error);
QttAnfProgram *qtt_semantic_anf_lower(
    QttSemanticFunction *semantic,
    const QttCoreNode *core,
    QttSemanticAnfError *error,
    QttAnfLowerError *lower_error);

#endif
