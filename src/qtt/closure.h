#ifndef MONAD_QTT_CLOSURE_H
#define MONAD_QTT_CLOSURE_H

#include "drop.h"
#include "environment.h"
#include "closure_policy.h"

typedef enum {
    QTT_CLOSURE_CAPTURE_VALUE,
    QTT_CLOSURE_CAPTURE_MOVE,
    QTT_CLOSURE_CAPTURE_RETAIN,
} QttClosureCaptureTransfer;

/*
 * A closure field is qualified independently of its temporary Resource-IR
 * variable. This prevents equal slot ordinals in different closure instances
 * from being treated as the same physical location.
 */
typedef struct {
    uint64_t storage_module;
    uint64_t closure_id;
    uint64_t instance_id;
    uint64_t ordinal;
} QttClosureFieldId;

typedef struct {
    QttCoreVar source;
    QttCoreVar slot;
    QttClosureFieldId field_id;
    const Type *type; /* borrowed from typed Core */
    uint64_t type_fingerprint;
    uint64_t field_fingerprint;
    QttRepresentation representation;
    QttClosureCaptureTransfer transfer;
    QttClosureFieldExit exit;
    QttDestructorDescriptor *destructor;
} QttClosureCapture;

typedef struct {
    QttClosureCapture *captures;
    size_t capture_count;
    uint64_t environment_module;
    uint64_t first_slot;
    uint64_t source_module;
    uint64_t closure_id;
    uint64_t instance_id;
    QttClosureStorage storage;
    uint64_t contract_fingerprint;
} QttClosurePlan;

typedef enum {
    QTT_CLOSURE_OK,
    QTT_CLOSURE_OUT_OF_MEMORY,
    QTT_CLOSURE_NOT_A_LAMBDA,
    QTT_CLOSURE_SLOT_OVERFLOW,
    QTT_CLOSURE_MISSING_TYPE,
    QTT_CLOSURE_UNKNOWN_REPRESENTATION,
    QTT_CLOSURE_INVALID_PLAN,
    QTT_CLOSURE_DESTRUCTOR_ERROR,
    QTT_CLOSURE_STRUCTURAL_CAPTURE_UNSUPPORTED,
    QTT_CLOSURE_ENVIRONMENT_MISMATCH,
    QTT_CLOSURE_EXPRESSION_ORIGIN_UNSUPPORTED,
} QttClosureError;

QttClosurePlan *qtt_closure_plan_build(const QttCoreNode *lambda,
                                       uint64_t environment_module,
                                       uint64_t first_slot,
                                       QttClosureError *error);
QttClosurePlan *qtt_closure_plan_build_with_policy(
    const QttCoreNode *lambda, uint64_t environment_module,
    uint64_t first_slot, QttClosureStorage storage,
    QttClosureError *error);
QttClosurePlan *qtt_closure_plan_infer(
    const QttCoreNode *root, const QttCoreNode *lambda,
    uint64_t environment_module, uint64_t first_slot,
    QttClosurePolicyEvidence *policy, QttClosureError *error);
QttClosurePlan *qtt_closure_plan_infer_in_env(
    const QttCoreNode *root, const QttCoreNode *lambda,
    const QttSignatureEnv *signatures, uint64_t module_id,
    uint64_t environment_module, uint64_t first_slot,
    QttClosurePolicyEvidence *policy, QttClosureError *error);
QttResourceBlock *qtt_closure_emit_construction(
    const QttClosurePlan *plan, QttClosureError *error);
QttResourceBlock *qtt_closure_emit_retains(const QttClosurePlan *plan,
                                           QttClosureError *error);
QttResourceBlock *qtt_closure_emit_releases(const QttClosurePlan *plan,
                                            QttClosureError *error);
QttResourceBlock *qtt_closure_emit_finalization(
    const QttClosurePlan *plan, QttClosureError *error);
bool qtt_closure_plan_verify(const QttClosurePlan *plan,
                             const QttCoreNode *lambda,
                             QttClosureError *error);
bool qtt_closure_plan_verify_in_env(
    const QttClosurePlan *plan, const QttCoreNode *lambda,
    const QttSignatureEnv *signatures, uint64_t module_id,
    QttClosureError *error);
bool qtt_closure_plan_bind_environment(
    QttClosurePlan *plan, const QttCoreNode *lambda,
    const QttClosureEnvironment *environment,
    const QttCoreVar *parameters, size_t parameter_count,
    QttClosureError *error);
bool qtt_closure_plan_bind_environment_in_env(
    QttClosurePlan *plan, const QttCoreNode *lambda,
    const QttClosureEnvironment *environment,
    const QttCoreVar *parameters, size_t parameter_count,
    const QttSignatureEnv *signatures, uint64_t module_id,
    QttClosureError *error);
uint64_t qtt_closure_plan_fingerprint(const QttClosurePlan *plan);
bool qtt_closure_field_id_equal(
    QttClosureFieldId left, QttClosureFieldId right);
void qtt_closure_plan_free(QttClosurePlan *plan);

#endif
