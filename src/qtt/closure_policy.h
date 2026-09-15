#ifndef MONAD_QTT_CLOSURE_POLICY_H
#define MONAD_QTT_CLOSURE_POLICY_H

#include "core.h"
#include "signature_env.h"

typedef enum {
    QTT_CLOSURE_STORAGE_UNKNOWN,
    QTT_CLOSURE_STORAGE_UNIQUE,
    QTT_CLOSURE_STORAGE_SHARED,
} QttClosureStorage;

typedef enum {
    QTT_CLOSURE_POLICY_NONE,
    QTT_CLOSURE_POLICY_SINGLE_DIRECT_CALL,
    QTT_CLOSURE_POLICY_ESCAPES,
    QTT_CLOSURE_POLICY_MULTIPLE_OCCURRENCES,
    QTT_CLOSURE_POLICY_CAPTURE_USED_OUTSIDE,
    QTT_CLOSURE_POLICY_INVOCATION_DISPOSITION_UNSUPPORTED,
    QTT_CLOSURE_POLICY_ANALYSIS_FAILED,
    QTT_CLOSURE_POLICY_NOT_FOUND,
} QttClosurePolicyReason;

typedef enum {
    QTT_CLOSURE_FIELD_RELEASE,
    QTT_CLOSURE_FIELD_MOVE_OUT,
    QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED,
} QttClosureFieldExit;

typedef struct {
    QttClosureStorage storage;
    QttClosurePolicyReason reason;
    size_t occurrence_count;
    size_t direct_callee_count;
    size_t external_capture_use_count;
    size_t live_after_capture_use_count;
    size_t released_capture_count;
    size_t moved_out_capture_count;
    size_t unsupported_exit_capture_count;
} QttClosurePolicyEvidence;

/*
 * A closure is unique only when its sole use is the callee of one application.
 * This includes an exact lambda held by one lexical BinderId. Values that can
 * be returned, passed as arguments, aliased, or invoked repeatedly are shared.
 */
QttClosurePolicyEvidence qtt_closure_policy_infer(
    const QttCoreNode *root, const QttCoreNode *lambda);
QttClosurePolicyEvidence qtt_closure_policy_infer_in_env(
    const QttCoreNode *root, const QttCoreNode *lambda,
    const QttSignatureEnv *signatures, uint64_t module_id);
bool qtt_closure_policy_verify(
    const QttCoreNode *root, const QttCoreNode *lambda,
    QttClosurePolicyEvidence evidence);
bool qtt_closure_policy_verify_in_env(
    const QttCoreNode *root, const QttCoreNode *lambda,
    const QttSignatureEnv *signatures, uint64_t module_id,
    QttClosurePolicyEvidence evidence);
QttClosureFieldExit qtt_closure_capture_exit(
    const QttCoreNode *lambda, QttCoreVar capture);
QttClosureFieldExit qtt_closure_capture_exit_in_env(
    const QttCoreNode *lambda, QttCoreVar capture,
    const QttSignatureEnv *signatures, uint64_t module_id);

#endif
