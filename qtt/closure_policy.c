#include "closure_policy.h"

#include "call.h"

#include <stdlib.h>

typedef struct {
    const QttCoreNode *target;
    QttCoreVar target_binding;
    bool scan_binding_uses;
    size_t occurrences;
    size_t direct_callees;
} PolicyScan;

static void scan(const QttCoreNode *node, bool direct_callee,
                 PolicyScan *result) {
    if (!node) return;
    if ((!result->scan_binding_uses && node == result->target) ||
        (result->scan_binding_uses && node->kind == QTT_CORE_VAR &&
         qtt_core_var_equal(node->var, result->target_binding))) {
        result->occurrences++;
        if (direct_callee) result->direct_callees++;
    }
    switch (node->kind) {
    case QTT_CORE_LAMBDA:
        scan(node->lambda.body, false, result);
        break;
    case QTT_CORE_APPLY:
        scan(node->apply.callee, true, result);
        for (size_t i = 0; i < node->apply.argument_count; i++)
            scan(node->apply.arguments[i], false, result);
        break;
    case QTT_CORE_LET:
        scan(node->let.value, false, result);
        scan(node->let.body, false, result);
        break;
    case QTT_CORE_IF:
        scan(node->conditional.condition, false, result);
        scan(node->conditional.then_branch, false, result);
        scan(node->conditional.else_branch, false, result);
        break;
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < node->sequence.count; i++)
            scan(node->sequence.items[i], false, result);
        break;
    case QTT_CORE_WRITE:
        scan(node->write.value, false, result);
        break;
    case QTT_CORE_BORROW:
        scan(node->borrow.body, false, result);
        break;
    case QTT_CORE_PERFORM:
        scan(node->perform.argument, false, result);
        break;
    case QTT_CORE_HANDLE:
        scan(node->handle.computation, false, result);
        scan(node->handle.clause, false, result);
        break;
    case QTT_CORE_LITERAL: case QTT_CORE_GLOBAL: case QTT_CORE_VAR:
    case QTT_CORE_PLACE:
    case QTT_CORE_QUOTE:
        break;
    }
}

static bool find_lambda_binding(
    const QttCoreNode *node, const QttCoreNode *target,
    QttCoreVar *binding) {
    if (!node) return false;
    if (node->kind == QTT_CORE_LET && node->let.value == target) {
        *binding = node->let.binding;
        return true;
    }
    switch (node->kind) {
    case QTT_CORE_LAMBDA:
        return find_lambda_binding(node->lambda.body, target, binding);
    case QTT_CORE_APPLY:
        if (find_lambda_binding(node->apply.callee, target, binding))
            return true;
        for (size_t i = 0; i < node->apply.argument_count; i++)
            if (find_lambda_binding(
                    node->apply.arguments[i], target, binding))
                return true;
        return false;
    case QTT_CORE_LET:
        return find_lambda_binding(node->let.value, target, binding) ||
            find_lambda_binding(node->let.body, target, binding);
    case QTT_CORE_IF:
        return find_lambda_binding(
                   node->conditional.condition, target, binding) ||
            find_lambda_binding(
                   node->conditional.then_branch, target, binding) ||
            find_lambda_binding(
                   node->conditional.else_branch, target, binding);
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < node->sequence.count; i++)
            if (find_lambda_binding(node->sequence.items[i], target, binding))
                return true;
        return false;
    case QTT_CORE_WRITE:
        return find_lambda_binding(node->write.value, target, binding);
    case QTT_CORE_BORROW:
        return find_lambda_binding(node->borrow.body, target, binding);
    case QTT_CORE_PERFORM:
        return find_lambda_binding(node->perform.argument, target, binding);
    case QTT_CORE_HANDLE:
        return find_lambda_binding(node->handle.computation, target, binding) ||
            find_lambda_binding(node->handle.clause, target, binding);
    default:
        return false;
    }
}

static bool captured_var(
    QttCoreVar var, const QttCoreVar *captures, size_t capture_count) {
    for (size_t i = 0; i < capture_count; i++)
        if (qtt_core_var_equal(var, captures[i])) return true;
    return false;
}

static bool contains_var(const QttCoreNode *node, QttCoreVar var) {
    if (!node) return false;
    if (node->kind == QTT_CORE_VAR)
        return qtt_core_var_equal(node->var, var);
    switch (node->kind) {
    case QTT_CORE_LAMBDA:
        return contains_var(node->lambda.body, var);
    case QTT_CORE_APPLY:
        if (contains_var(node->apply.callee, var)) return true;
        for (size_t i = 0; i < node->apply.argument_count; i++)
            if (contains_var(node->apply.arguments[i], var)) return true;
        return false;
    case QTT_CORE_LET:
        return contains_var(node->let.value, var) ||
            contains_var(node->let.body, var);
    case QTT_CORE_IF:
        return contains_var(node->conditional.condition, var) ||
            contains_var(node->conditional.then_branch, var) ||
            contains_var(node->conditional.else_branch, var);
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < node->sequence.count; i++)
            if (contains_var(node->sequence.items[i], var)) return true;
        return false;
    case QTT_CORE_WRITE:
        return qtt_core_var_equal(node->write.place.root, var) ||
               contains_var(node->write.value, var);
    case QTT_CORE_BORROW:
        return qtt_core_var_equal(node->borrow.place.root, var) ||
               contains_var(node->borrow.body, var);
    case QTT_CORE_PERFORM:
        return contains_var(node->perform.argument, var);
    case QTT_CORE_HANDLE:
        return contains_var(node->handle.computation, var) ||
            contains_var(node->handle.clause, var);
    case QTT_CORE_PLACE:
        return qtt_core_var_equal(node->place.root, var);
    case QTT_CORE_LITERAL: case QTT_CORE_GLOBAL: case QTT_CORE_QUOTE:
    case QTT_CORE_VAR:
        return false;
    }
    return false;
}

static QttClosureFieldExit exit_at(
    const QttCoreNode *node, QttCoreVar capture) {
    if (!node) return QTT_CLOSURE_FIELD_RELEASE;
    switch (node->kind) {
    case QTT_CORE_VAR:
        return qtt_core_var_equal(node->var, capture)
            ? QTT_CLOSURE_FIELD_MOVE_OUT
            : QTT_CLOSURE_FIELD_RELEASE;
    case QTT_CORE_LET:
        if (contains_var(node->let.value, capture))
            return QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
        return exit_at(node->let.body, capture);
    case QTT_CORE_SEQUENCE:
        return node->sequence.count
            ? exit_at(
                node->sequence.items[node->sequence.count - 1], capture)
            : QTT_CLOSURE_FIELD_RELEASE;
    case QTT_CORE_IF: {
        QttClosureFieldExit then_exit =
            exit_at(node->conditional.then_branch, capture);
        QttClosureFieldExit else_exit =
            exit_at(node->conditional.else_branch, capture);
        return then_exit == else_exit
            ? then_exit : QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
    }
    case QTT_CORE_APPLY:
    case QTT_CORE_LAMBDA:
    case QTT_CORE_WRITE:
    case QTT_CORE_PLACE:
    case QTT_CORE_BORROW:
    case QTT_CORE_PERFORM:
    case QTT_CORE_HANDLE:
        return contains_var(node, capture)
            ? QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED
            : QTT_CLOSURE_FIELD_RELEASE;
    case QTT_CORE_LITERAL: case QTT_CORE_GLOBAL: case QTT_CORE_QUOTE:
        return QTT_CLOSURE_FIELD_RELEASE;
    }
    return QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
}

static QttClosureFieldExit exit_at_in_env(
    const QttCoreNode *node, QttCoreVar capture,
    const QttSignatureEnv *signatures, uint64_t module_id) {
    if (!node) return QTT_CLOSURE_FIELD_RELEASE;
    switch (node->kind) {
    case QTT_CORE_VAR:
        return qtt_core_var_equal(node->var, capture)
            ? QTT_CLOSURE_FIELD_MOVE_OUT
            : QTT_CLOSURE_FIELD_RELEASE;
    case QTT_CORE_LET:
        if (contains_var(node->let.value, capture))
            return QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
        return exit_at_in_env(
            node->let.body, capture, signatures, module_id);
    case QTT_CORE_SEQUENCE:
        return node->sequence.count
            ? exit_at_in_env(
                node->sequence.items[node->sequence.count - 1],
                capture, signatures, module_id)
            : QTT_CLOSURE_FIELD_RELEASE;
    case QTT_CORE_IF: {
        QttClosureFieldExit then_exit = exit_at_in_env(
            node->conditional.then_branch, capture,
            signatures, module_id);
        QttClosureFieldExit else_exit = exit_at_in_env(
            node->conditional.else_branch, capture,
            signatures, module_id);
        return then_exit == else_exit
            ? then_exit : QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
    }
    case QTT_CORE_BORROW:
        return exit_at_in_env(
            node->borrow.body, capture, signatures, module_id);
    case QTT_CORE_APPLY: {
        if (!contains_var(node, capture))
            return QTT_CLOSURE_FIELD_RELEASE;
        if (!signatures || !node->apply.callee ||
            node->apply.callee->kind != QTT_CORE_GLOBAL ||
            contains_var(node->apply.callee, capture))
            return QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
        QttFunctionSignature *signature = NULL;
        if (!qtt_signature_env_resolve(
                signatures, module_id,
                node->apply.callee->global.name, NULL, &signature) ||
            !signature ||
            !signature->contract_fingerprint ||
            signature->contract_fingerprint !=
                qtt_signature_contract_fingerprint(signature) ||
            signature->parameter_count != node->apply.argument_count)
            return QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
        size_t matched = 0;
        QttCallTransfer transfer = QTT_CALL_VALUE;
        for (size_t i = 0; i < node->apply.argument_count; i++) {
            const QttCoreNode *argument = node->apply.arguments[i];
            if (!contains_var(argument, capture)) continue;
            if (argument->kind != QTT_CORE_VAR ||
                !qtt_core_var_equal(argument->var, capture) ||
                ++matched != 1)
                return QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
            QttCallArgument actual = {
                .type = argument->type,
                .type_id = signature->parameters[i].type_id,
                .representation =
                    qtt_resource_classify_type(argument->type),
                .transfer = qtt_call_transfer_for_parameter(
                    &signature->parameters[i]),
            };
            QttCallPlan plan = {0};
            if (qtt_call_plan(
                    &(QttFunctionSignature){
                        .parameters = &signature->parameters[i],
                        .parameter_count = 1,
                        .result_type = signature->result_type,
                        .result = signature->result,
                    },
                    &actual, 1, &plan) != QTT_CALL_VALID)
                return QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
            qtt_call_plan_free(&plan);
            transfer = actual.transfer;
        }
        if (matched != 1)
            return QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
        return transfer == QTT_CALL_MOVE
            ? QTT_CLOSURE_FIELD_MOVE_OUT
            : transfer == QTT_CALL_BORROW ||
              transfer == QTT_CALL_VALUE
                ? QTT_CLOSURE_FIELD_RELEASE
                : QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
    }
    case QTT_CORE_LAMBDA:
    case QTT_CORE_WRITE:
    case QTT_CORE_PLACE:
    case QTT_CORE_PERFORM:
    case QTT_CORE_HANDLE:
        return contains_var(node, capture)
            ? QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED
            : QTT_CLOSURE_FIELD_RELEASE;
    case QTT_CORE_LITERAL: case QTT_CORE_GLOBAL: case QTT_CORE_QUOTE:
        return QTT_CLOSURE_FIELD_RELEASE;
    }
    return QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
}

QttClosureFieldExit qtt_closure_capture_exit(
    const QttCoreNode *lambda, QttCoreVar capture) {
    if (!lambda || lambda->kind != QTT_CORE_LAMBDA)
        return QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
    return exit_at(lambda->lambda.body, capture);
}

QttClosureFieldExit qtt_closure_capture_exit_in_env(
    const QttCoreNode *lambda, QttCoreVar capture,
    const QttSignatureEnv *signatures, uint64_t module_id) {
    if (!lambda || lambda->kind != QTT_CORE_LAMBDA ||
        !signatures || !module_id)
        return QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED;
    return exit_at_in_env(
        lambda->lambda.body, capture, signatures, module_id);
}

static void scan_external_capture_uses(
    const QttCoreNode *node, const QttCoreNode *target,
    const QttCoreVar *captures, size_t capture_count, size_t *uses) {
    if (!node || node == target) return;
    if (node->kind == QTT_CORE_VAR &&
        captured_var(node->var, captures, capture_count))
        (*uses)++;
    switch (node->kind) {
    case QTT_CORE_LAMBDA:
        scan_external_capture_uses(
            node->lambda.body, target, captures, capture_count, uses);
        break;
    case QTT_CORE_WRITE:
        if (captured_var(
                node->write.place.root, captures, capture_count))
            (*uses)++;
        scan_external_capture_uses(
            node->write.value, target,
            captures, capture_count, uses);
        break;
    case QTT_CORE_PLACE:
        if (captured_var(node->place.root, captures, capture_count))
            (*uses)++;
        break;
    case QTT_CORE_APPLY:
        scan_external_capture_uses(
            node->apply.callee, target, captures, capture_count, uses);
        for (size_t i = 0; i < node->apply.argument_count; i++)
            scan_external_capture_uses(
                node->apply.arguments[i], target,
                captures, capture_count, uses);
        break;
    case QTT_CORE_BORROW:
        if (captured_var(node->borrow.place.root,
                         captures, capture_count))
            (*uses)++;
        scan_external_capture_uses(
            node->borrow.body, target, captures, capture_count, uses);
        break;
    case QTT_CORE_LET:
        scan_external_capture_uses(
            node->let.value, target, captures, capture_count, uses);
        scan_external_capture_uses(
            node->let.body, target, captures, capture_count, uses);
        break;
    case QTT_CORE_IF:
        scan_external_capture_uses(
            node->conditional.condition, target,
            captures, capture_count, uses);
        scan_external_capture_uses(
            node->conditional.then_branch, target,
            captures, capture_count, uses);
        scan_external_capture_uses(
            node->conditional.else_branch, target,
            captures, capture_count, uses);
        break;
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < node->sequence.count; i++)
            scan_external_capture_uses(
                node->sequence.items[i], target,
                captures, capture_count, uses);
        break;
    case QTT_CORE_PERFORM:
        scan_external_capture_uses(
            node->perform.argument, target, captures, capture_count, uses);
        break;
    case QTT_CORE_HANDLE:
        scan_external_capture_uses(
            node->handle.computation, target, captures, capture_count, uses);
        scan_external_capture_uses(
            node->handle.clause, target, captures, capture_count, uses);
        break;
    case QTT_CORE_LITERAL: case QTT_CORE_GLOBAL: case QTT_CORE_VAR:
    case QTT_CORE_QUOTE:
        break;
    }
}

enum {
    POLICY_PATH_BEFORE = 1,
    POLICY_PATH_AFTER = 2,
};

static size_t lambda_capture_overlap(
    const QttCoreNode *lambda,
    const QttCoreVar *captures, size_t capture_count) {
    QttCoreVar *nested = NULL;
    size_t nested_count = 0;
    if (qtt_core_lambda_captures(
            lambda, &nested, &nested_count) != QTT_CORE_OK)
        return capture_count ? 1 : 0;
    size_t overlap = 0;
    for (size_t i = 0; i < nested_count; i++)
        if (captured_var(nested[i], captures, capture_count))
            overlap++;
    free(nested);
    return overlap;
}

static unsigned scan_liveness(
    const QttCoreNode *node, const QttCoreNode *target,
    const QttCoreVar *captures, size_t capture_count,
    unsigned paths, size_t *uses_after, bool entry_root) {
    if (!node) return paths;
    if (node == target) {
        if (paths & POLICY_PATH_BEFORE) {
            paths &= ~POLICY_PATH_BEFORE;
            paths |= POLICY_PATH_AFTER;
        }
        return paths;
    }
    if (node->kind == QTT_CORE_VAR &&
        (paths & POLICY_PATH_AFTER) &&
        captured_var(node->var, captures, capture_count))
        (*uses_after)++;
    switch (node->kind) {
    case QTT_CORE_LAMBDA:
        if (entry_root)
            return scan_liveness(
                node->lambda.body, target, captures, capture_count,
                paths, uses_after, false);
        if (paths & POLICY_PATH_AFTER)
            *uses_after += lambda_capture_overlap(
                node, captures, capture_count);
        return paths;
    case QTT_CORE_APPLY:
        paths = scan_liveness(
            node->apply.callee, target, captures, capture_count,
            paths, uses_after, false);
        for (size_t i = 0; i < node->apply.argument_count; i++)
            paths = scan_liveness(
                node->apply.arguments[i], target,
                captures, capture_count, paths, uses_after, false);
        return paths;
    case QTT_CORE_LET:
        paths = scan_liveness(
            node->let.value, target, captures, capture_count,
            paths, uses_after, false);
        return scan_liveness(
            node->let.body, target, captures, capture_count,
            paths, uses_after, false);
    case QTT_CORE_IF: {
        unsigned entered = scan_liveness(
            node->conditional.condition, target,
            captures, capture_count, paths, uses_after, false);
        unsigned then_paths = scan_liveness(
            node->conditional.then_branch, target,
            captures, capture_count, entered, uses_after, false);
        unsigned else_paths = scan_liveness(
            node->conditional.else_branch, target,
            captures, capture_count, entered, uses_after, false);
        return then_paths | else_paths;
    }
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < node->sequence.count; i++)
            paths = scan_liveness(
                node->sequence.items[i], target,
                captures, capture_count, paths, uses_after, false);
        return paths;
    case QTT_CORE_WRITE:
        if ((paths & POLICY_PATH_AFTER) &&
            captured_var(
                node->write.place.root,
                captures, capture_count))
            (*uses_after)++;
        return scan_liveness(
            node->write.value, target, captures, capture_count,
            paths, uses_after, false);
    case QTT_CORE_PLACE:
        if ((paths & POLICY_PATH_AFTER) &&
            captured_var(node->place.root, captures, capture_count))
            (*uses_after)++;
        return paths;
    case QTT_CORE_BORROW:
        if ((paths & POLICY_PATH_AFTER) &&
            captured_var(node->borrow.place.root,
                         captures, capture_count))
            (*uses_after)++;
        return scan_liveness(
            node->borrow.body, target, captures, capture_count,
            paths, uses_after, false);
    case QTT_CORE_PERFORM:
        return scan_liveness(
            node->perform.argument, target, captures, capture_count,
            paths, uses_after, false);
    case QTT_CORE_HANDLE:
        paths = scan_liveness(
            node->handle.computation, target, captures, capture_count,
            paths, uses_after, false);
        return scan_liveness(
            node->handle.clause, target, captures, capture_count,
            paths, uses_after, false);
    case QTT_CORE_LITERAL: case QTT_CORE_GLOBAL: case QTT_CORE_VAR:
    case QTT_CORE_QUOTE:
        return paths;
    }
    return paths;
}

static QttClosurePolicyEvidence infer_policy(
    const QttCoreNode *root, const QttCoreNode *lambda,
    const QttSignatureEnv *signatures, uint64_t module_id) {
    QttClosurePolicyEvidence evidence = {
        .storage = QTT_CLOSURE_STORAGE_SHARED,
        .reason = QTT_CLOSURE_POLICY_NOT_FOUND,
    };
    if (!root || !lambda || lambda->kind != QTT_CORE_LAMBDA)
        return evidence;
    PolicyScan scan_result = {.target = lambda};
    scan_result.scan_binding_uses = find_lambda_binding(
        root, lambda, &scan_result.target_binding);
    scan(root, false, &scan_result);
    evidence.occurrence_count = scan_result.occurrences;
    evidence.direct_callee_count = scan_result.direct_callees;
    QttCoreVar *captures = NULL;
    size_t capture_count = 0;
    if (qtt_core_lambda_captures(
            lambda, &captures, &capture_count) != QTT_CORE_OK) {
        evidence.reason = QTT_CLOSURE_POLICY_ANALYSIS_FAILED;
        return evidence;
    }
    scan_external_capture_uses(
        root, lambda, captures, capture_count,
        &evidence.external_capture_use_count);
    scan_liveness(
        root, lambda, captures, capture_count,
        POLICY_PATH_BEFORE,
        &evidence.live_after_capture_use_count, true);
    for (size_t i = 0; i < capture_count; i++) {
        QttClosureFieldExit exit =
            signatures
                ? qtt_closure_capture_exit_in_env(
                    lambda, captures[i], signatures, module_id)
                : qtt_closure_capture_exit(lambda, captures[i]);
        if (exit == QTT_CLOSURE_FIELD_RELEASE)
            evidence.released_capture_count++;
        else if (exit == QTT_CLOSURE_FIELD_MOVE_OUT)
            evidence.moved_out_capture_count++;
        else
            evidence.unsupported_exit_capture_count++;
    }
    free(captures);
    if (scan_result.occurrences == 1 &&
        scan_result.direct_callees == 1 &&
        evidence.live_after_capture_use_count == 0 &&
        evidence.unsupported_exit_capture_count == 0) {
        evidence.storage = QTT_CLOSURE_STORAGE_UNIQUE;
        evidence.reason = QTT_CLOSURE_POLICY_SINGLE_DIRECT_CALL;
    } else if (scan_result.occurrences > 1) {
        evidence.reason = QTT_CLOSURE_POLICY_MULTIPLE_OCCURRENCES;
    } else if (evidence.live_after_capture_use_count) {
        evidence.reason = QTT_CLOSURE_POLICY_CAPTURE_USED_OUTSIDE;
    } else if (evidence.unsupported_exit_capture_count) {
        evidence.reason =
            QTT_CLOSURE_POLICY_INVOCATION_DISPOSITION_UNSUPPORTED;
    } else if (scan_result.occurrences == 1) {
        evidence.reason = QTT_CLOSURE_POLICY_ESCAPES;
    }
    return evidence;
}

QttClosurePolicyEvidence qtt_closure_policy_infer(
    const QttCoreNode *root, const QttCoreNode *lambda) {
    return infer_policy(root, lambda, NULL, 0);
}

QttClosurePolicyEvidence qtt_closure_policy_infer_in_env(
    const QttCoreNode *root, const QttCoreNode *lambda,
    const QttSignatureEnv *signatures, uint64_t module_id) {
    return infer_policy(
        root, lambda, signatures, module_id);
}

bool qtt_closure_policy_verify(
    const QttCoreNode *root, const QttCoreNode *lambda,
    QttClosurePolicyEvidence evidence) {
    QttClosurePolicyEvidence expected =
        qtt_closure_policy_infer(root, lambda);
    return evidence.storage == expected.storage &&
        evidence.reason == expected.reason &&
        evidence.occurrence_count == expected.occurrence_count &&
        evidence.direct_callee_count == expected.direct_callee_count &&
        evidence.external_capture_use_count ==
            expected.external_capture_use_count &&
        evidence.live_after_capture_use_count ==
            expected.live_after_capture_use_count &&
        evidence.released_capture_count ==
            expected.released_capture_count &&
        evidence.moved_out_capture_count ==
            expected.moved_out_capture_count &&
        evidence.unsupported_exit_capture_count ==
            expected.unsupported_exit_capture_count;
}

bool qtt_closure_policy_verify_in_env(
    const QttCoreNode *root, const QttCoreNode *lambda,
    const QttSignatureEnv *signatures, uint64_t module_id,
    QttClosurePolicyEvidence evidence) {
    QttClosurePolicyEvidence expected =
        qtt_closure_policy_infer_in_env(
            root, lambda, signatures, module_id);
    return evidence.storage == expected.storage &&
        evidence.reason == expected.reason &&
        evidence.occurrence_count == expected.occurrence_count &&
        evidence.direct_callee_count == expected.direct_callee_count &&
        evidence.external_capture_use_count ==
            expected.external_capture_use_count &&
        evidence.live_after_capture_use_count ==
            expected.live_after_capture_use_count &&
        evidence.released_capture_count ==
            expected.released_capture_count &&
        evidence.moved_out_capture_count ==
            expected.moved_out_capture_count &&
        evidence.unsupported_exit_capture_count ==
            expected.unsupported_exit_capture_count;
}
