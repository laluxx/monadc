#include "core_usage.h"

#include "constraints.h"
#include "elaboration.h"
#include "callable_env.h"

#include <stdlib.h>

static bool same_module(
    const QttCoreNode *core, uint64_t *module_id, bool *seen) {
    if (!core) return false;
    switch (core->kind) {
    case QTT_CORE_VAR:
        if (!core->var.module_id) return false;
        if (*seen && *module_id != core->var.module_id) return false;
        *module_id = core->var.module_id;
        *seen = true;
        return true;
    case QTT_CORE_PLACE:
        if (!core->place.root.module_id) return false;
        if (*seen && *module_id != core->place.root.module_id) return false;
        *module_id = core->place.root.module_id;
        *seen = true;
        return true;
    case QTT_CORE_LET:
        if (!core->let.binding.module_id ||
            (*seen && *module_id != core->let.binding.module_id))
            return false;
        *module_id = core->let.binding.module_id;
        *seen = true;
        return same_module(core->let.value, module_id, seen) &&
               same_module(core->let.body, module_id, seen);
    case QTT_CORE_LAMBDA:
        for (size_t i = 0; i < core->lambda.param_count; i++) {
            if (!core->lambda.params[i].module_id ||
                (*seen &&
                 *module_id != core->lambda.params[i].module_id))
                return false;
            *module_id = core->lambda.params[i].module_id;
            *seen = true;
        }
        return same_module(core->lambda.body, module_id, seen);
    case QTT_CORE_APPLY:
        if (!same_module(core->apply.callee, module_id, seen))
            return false;
        for (size_t i = 0; i < core->apply.argument_count; i++)
            if (!same_module(
                    core->apply.arguments[i], module_id, seen))
                return false;
        return true;
    case QTT_CORE_IF:
        return same_module(
                   core->conditional.condition, module_id, seen) &&
               same_module(
                   core->conditional.then_branch, module_id, seen) &&
               same_module(
                   core->conditional.else_branch, module_id, seen);
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < core->sequence.count; i++)
            if (!same_module(
                    core->sequence.items[i], module_id, seen))
                return false;
        return true;
    case QTT_CORE_WRITE:
        if (!core->write.place.root.module_id ||
            (*seen &&
             *module_id != core->write.place.root.module_id))
            return false;
        *module_id = core->write.place.root.module_id;
        *seen = true;
        return same_module(core->write.value, module_id, seen);
    default:
        return true;
    }
}

static const QttCoreNode *find_boundary(
    const QttCoreNode *core, QttCoreUsageBoundary *boundary,
    const QttSignatureEnv *signatures, uint64_t module_id,
    const QttCallableBinding *callables) {
    if (!core) return NULL;
    switch (core->kind) {
    case QTT_CORE_APPLY: {
        const QttCoreNode *callee = core->apply.callee;
        if (callee && callee->kind == QTT_CORE_VAR)
            callee = qtt_callable_env_lookup(callables, callee->var);
        if (callee && callee->kind == QTT_CORE_LAMBDA &&
            core->apply.argument_count ==
                callee->lambda.param_count) {
            const QttCoreNode *found =
                find_boundary(
                    callee, boundary,
                    signatures, module_id, callables);
            for (size_t i = 0;
                 !found && i < core->apply.argument_count; i++)
                found = find_boundary(
                    core->apply.arguments[i], boundary,
                    signatures, module_id, callables);
            return found;
        }
        if (signatures && core->apply.callee &&
            core->apply.callee->kind == QTT_CORE_GLOBAL) {
            QttFunctionSignature *signature =
                qtt_signature_env_lookup(
                    signatures, module_id,
                    core->apply.callee->global.name);
            if (signature &&
                signature->parameter_count ==
                    core->apply.argument_count) {
                const QttCoreNode *found = find_boundary(
                    core->apply.callee, boundary,
                    signatures, module_id, callables);
                for (size_t i = 0;
                     !found && i < core->apply.argument_count; i++)
                    found = find_boundary(
                        core->apply.arguments[i], boundary,
                        signatures, module_id, callables);
                return found;
            }
        }
        *boundary = QTT_CORE_USAGE_BOUNDARY_APPLICATION;
        return core;
    }
    case QTT_CORE_LAMBDA:
        return find_boundary(
            core->lambda.body, boundary, signatures, module_id,
            callables);
    case QTT_CORE_LET: {
        const QttCoreNode *found =
            find_boundary(
                core->let.value, boundary, signatures, module_id,
                callables);
        if (found) return found;
        const QttCoreNode *lambda = qtt_callable_env_value(
            callables, core->let.value);
        QttCallableBinding binding = {
            .var = core->let.binding, .lambda = lambda,
            .parent = callables};
        return find_boundary(
            core->let.body, boundary, signatures, module_id,
            lambda ? &binding : callables);
    }
    case QTT_CORE_IF: {
        const QttCoreNode *found = find_boundary(
            core->conditional.condition, boundary,
            signatures, module_id, callables);
        if (!found)
            found = find_boundary(
                core->conditional.then_branch, boundary,
                signatures, module_id, callables);
        if (!found)
            found = find_boundary(
                core->conditional.else_branch, boundary,
                signatures, module_id, callables);
        return found;
    }
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < core->sequence.count; i++) {
            const QttCoreNode *found =
                find_boundary(
                    core->sequence.items[i], boundary,
                    signatures, module_id, callables);
            if (found) return found;
        }
        return NULL;
    case QTT_CORE_WRITE:
        return find_boundary(
            core->write.value, boundary, signatures, module_id,
            callables);
    case QTT_CORE_PERFORM:
        return find_boundary(core->perform.argument, boundary,
                             signatures, module_id, callables);
    case QTT_CORE_HANDLE: {
        const QttCoreNode *found = find_boundary(
            core->handle.computation, boundary, signatures,
            module_id, callables);
        return found ? found : find_boundary(
            core->handle.clause, boundary, signatures,
            module_id, callables);
    }
    default:
        return NULL;
    }
}

static QttUsageContext *symbolic_usage(
    QttGradeArena *arena, const QttCoreNode *core,
    const QttSignatureEnv *signatures, uint64_t module_id,
    const QttCallableBinding *callables) {
    if (!arena || !core) return NULL;
    switch (core->kind) {
    case QTT_CORE_VAR:
        return qtt_usage_singleton(arena, core->var.binder_id);
    case QTT_CORE_BORROW: {
        QttUsageContext *authority = qtt_usage_singleton(
            arena, core->borrow.place.root.binder_id);
        QttUsageContext *body = symbolic_usage(
            arena, core->borrow.body, signatures, module_id, callables);
        QttUsageContext *result = authority && body
            ? qtt_usage_sequence(arena, authority, body) : NULL;
        qtt_usage_context_free(authority);
        qtt_usage_context_free(body);
        return result;
    }
    case QTT_CORE_PLACE:
        return qtt_usage_singleton(
            arena, core->place.root.binder_id);
    case QTT_CORE_IF: {
        QttUsageContext *condition =
            symbolic_usage(
                arena, core->conditional.condition,
                signatures, module_id, callables);
        QttUsageContext *then_usage =
            symbolic_usage(
                arena, core->conditional.then_branch,
                signatures, module_id, callables);
        QttUsageContext *else_usage =
            symbolic_usage(
                arena, core->conditional.else_branch,
                signatures, module_id, callables);
        QttUsageContext *choice =
            then_usage && else_usage
                ? qtt_usage_choice(arena, then_usage, else_usage) : NULL;
        QttUsageContext *result =
            condition && choice
                ? qtt_usage_sequence(arena, condition, choice) : NULL;
        qtt_usage_context_free(condition);
        qtt_usage_context_free(then_usage);
        qtt_usage_context_free(else_usage);
        qtt_usage_context_free(choice);
        return result;
    }
    case QTT_CORE_LAMBDA: {
        QttUsageContext *body =
            symbolic_usage(
                arena, core->lambda.body, signatures, module_id,
                callables);
        if (!body) return NULL;
        uint64_t *parameters = core->lambda.param_count
            ? malloc(core->lambda.param_count * sizeof(*parameters))
            : NULL;
        if (core->lambda.param_count && !parameters) {
            qtt_usage_context_free(body);
            return NULL;
        }
        for (size_t i = 0; i < core->lambda.param_count; i++)
            parameters[i] = core->lambda.params[i].binder_id;
        QttUsageClosure *closure = qtt_usage_closure(
            arena, parameters, core->lambda.param_count, body);
        free(parameters);
        qtt_usage_context_free(body);
        if (!closure) return NULL;
        QttUsageContext *empty = qtt_usage_empty(arena);
        QttUsageContext *result = empty
            ? qtt_usage_sequence(
                  arena, qtt_usage_closure_captures(closure), empty)
            : NULL;
        qtt_usage_context_free(empty);
        qtt_usage_closure_free(closure);
        return result;
    }
    case QTT_CORE_SEQUENCE: {
        QttUsageContext *result = qtt_usage_empty(arena);
        if (!result) return NULL;
        for (size_t i = 0; i < core->sequence.count; i++) {
            QttUsageContext *item =
                symbolic_usage(
                    arena, core->sequence.items[i],
                    signatures, module_id, callables);
            QttUsageContext *next =
                item ? qtt_usage_sequence(arena, result, item) : NULL;
            qtt_usage_context_free(item);
            qtt_usage_context_free(result);
            if (!next) return NULL;
            result = next;
        }
        return result;
    }
    case QTT_CORE_LET: {
        QttUsageContext *value =
            symbolic_usage(
                arena, core->let.value, signatures, module_id,
                callables);
        const QttCoreNode *known = qtt_callable_env_value(
            callables, core->let.value);
        QttCallableBinding binding = {
            .var = core->let.binding, .lambda = known,
            .parent = callables};
        QttUsageContext *body = symbolic_usage(
            arena, core->let.body, signatures, module_id,
            known ? &binding : callables);
        QttUsageContext *result =
            value && body
                ? qtt_usage_let(
                      arena, core->let.binding.binder_id, value, body)
                : NULL;
        qtt_usage_context_free(value);
        qtt_usage_context_free(body);
        return result;
    }
    case QTT_CORE_WRITE: {
        QttUsageContext *target = qtt_usage_singleton(
            arena, core->write.place.root.binder_id);
        QttUsageContext *value = symbolic_usage(
            arena, core->write.value, signatures, module_id,
            callables);
        QttUsageContext *result = target && value
            ? qtt_usage_sequence(arena, target, value) : NULL;
        qtt_usage_context_free(target);
        qtt_usage_context_free(value);
        return result;
    }
    case QTT_CORE_PERFORM:
        return symbolic_usage(arena, core->perform.argument, signatures,
                              module_id, callables);
    case QTT_CORE_HANDLE: {
        QttUsageContext *computation = symbolic_usage(
            arena, core->handle.computation, signatures, module_id,
            callables);
        QttUsageContext *clause = symbolic_usage(
            arena, core->handle.clause, signatures, module_id, callables);
        QttUsageContext *result = computation && clause
            ? qtt_usage_sequence(arena, computation, clause) : NULL;
        qtt_usage_context_free(computation);
        qtt_usage_context_free(clause);
        return result;
    }
    case QTT_CORE_APPLY: {
        const QttCoreNode *callee_source = core->apply.callee;
        const QttCoreNode *lambda = callee_source;
        if (lambda && lambda->kind == QTT_CORE_GLOBAL &&
            signatures) {
            QttFunctionSignature *signature =
                qtt_signature_env_lookup(
                    signatures, module_id, lambda->global.name);
            if (!signature ||
                signature->parameter_count !=
                    core->apply.argument_count)
                return NULL;
            size_t count = core->apply.argument_count;
            QttUsageContext *callee =
                symbolic_usage(
                    arena, lambda, signatures, module_id, callables);
            QttUsageContext **arguments = count
                ? calloc(count, sizeof(*arguments)) : NULL;
            QttGradeExpr **domains = count
                ? calloc(count, sizeof(*domains)) : NULL;
            bool valid = callee &&
                (!count || (arguments && domains));
            for (size_t i = 0; valid && i < count; i++) {
                arguments[i] = symbolic_usage(
                    arena, core->apply.arguments[i],
                    signatures, module_id, callables);
                domains[i] = qtt_grade_constant(
                    arena, signature->parameters[i].observed);
                valid = arguments[i] && domains[i];
            }
            QttUsageContext *result = valid
                ? qtt_usage_application(
                    arena, callee,
                    (const QttUsageContext *const *)arguments,
                    domains, count)
                : NULL;
            for (size_t i = 0; i < count; i++)
                qtt_usage_context_free(arguments[i]);
            free(arguments);
            free(domains);
            qtt_usage_context_free(callee);
            return result;
        }
        if (lambda && lambda->kind == QTT_CORE_VAR)
            lambda = qtt_callable_env_lookup(callables, lambda->var);
        if (!lambda || lambda->kind != QTT_CORE_LAMBDA ||
            core->apply.argument_count != lambda->lambda.param_count)
            return NULL;
        size_t count = core->apply.argument_count;
        QttUsageContext *body =
            symbolic_usage(
                arena, lambda->lambda.body, signatures, module_id,
                callables);
        QttUsageContext *callee =
            symbolic_usage(
                arena, callee_source, signatures, module_id,
                callables);
        uint64_t *parameters =
            count ? malloc(count * sizeof(*parameters)) : NULL;
        QttUsageContext **arguments =
            count ? calloc(count, sizeof(*arguments)) : NULL;
        QttGradeExpr **domains =
            count ? calloc(count, sizeof(*domains)) : NULL;
        if (!body || !callee ||
            (count && (!parameters || !arguments || !domains))) {
            free(parameters);
            free(arguments);
            free(domains);
            qtt_usage_context_free(body);
            qtt_usage_context_free(callee);
            return NULL;
        }
        QttGradeExpr *zero =
            qtt_grade_constant(arena, qtt_quantity_finite(0));
        QttGradeExpr *one =
            qtt_grade_constant(arena, qtt_quantity_finite(1));
        bool valid = zero && one;
        for (size_t i = 0; valid && i < count; i++) {
            parameters[i] = lambda->lambda.params[i].binder_id;
            arguments[i] =
                symbolic_usage(
                    arena, core->apply.arguments[i],
                    signatures, module_id, callables);
            domains[i] = qtt_usage_grade(body, parameters[i]);
            if (!domains[i]) domains[i] = zero;
            valid = arguments[i] != NULL;
        }
        QttUsageClosure *closure = valid
            ? qtt_usage_closure(arena, parameters, count, body) : NULL;
        QttUsageContext *applied = closure
            ? qtt_usage_application(
                  arena, callee,
                  (const QttUsageContext *const *)arguments,
                  domains, count)
            : NULL;
        QttUsageContext *invoked = closure
            ? qtt_usage_closure_invoke(arena, closure, one) : NULL;
        QttUsageContext *result = applied && invoked
            ? qtt_usage_sequence(arena, applied, invoked) : NULL;
        for (size_t i = 0; i < count; i++)
            qtt_usage_context_free(arguments[i]);
        qtt_usage_context_free(invoked);
        qtt_usage_context_free(applied);
        qtt_usage_closure_free(closure);
        qtt_usage_context_free(callee);
        qtt_usage_context_free(body);
        free(domains);
        free(arguments);
        free(parameters);
        return result;
    }
    case QTT_CORE_LITERAL:
    case QTT_CORE_GLOBAL:
    case QTT_CORE_QUOTE:
        return qtt_usage_empty(arena);
    }
    return NULL;
}

QttCoreUsageResult qtt_core_usage_lower_lexical(
    QttGradeArena *arena, const QttCoreNode *core,
    const QttSignatureEnv *signatures, uint64_t requested_module_id,
    const QttCallableBinding *callables) {
    QttCoreUsageResult result = {
        .status = QTT_CORE_USAGE_INVALID,
        .boundary = QTT_CORE_USAGE_BOUNDARY_NONE,
    };
    if (!arena || !core) return result;
    uint64_t module_id = 0;
    bool seen_module = false;
    if (!same_module(core, &module_id, &seen_module)) return result;
    if (requested_module_id) {
        if (seen_module && module_id != requested_module_id)
            return result;
        module_id = requested_module_id;
    }
    result.offending = find_boundary(
        core, &result.boundary, signatures, module_id, callables);
    if (result.offending) {
        result.status = QTT_CORE_USAGE_UNSUPPORTED;
        return result;
    }
    result.usage = symbolic_usage(
        arena, core, signatures, module_id, callables);
    result.status = result.usage
        ? QTT_CORE_USAGE_OK : QTT_CORE_USAGE_OUT_OF_MEMORY;
    return result;
}

QttCoreUsageResult qtt_core_usage_lower_in_env(
    QttGradeArena *arena, const QttCoreNode *core,
    const QttSignatureEnv *signatures, uint64_t requested_module_id) {
    return qtt_core_usage_lower_lexical(
        arena, core, signatures, requested_module_id, NULL);
}

QttCoreUsageResult qtt_core_usage_lower(
    QttGradeArena *arena, const QttCoreNode *core) {
    return qtt_core_usage_lower_in_env(
        arena, core, NULL, 0);
}
