#include "environment.h"

#include <stdlib.h>
#include <string.h>

QttEnvironmentOrigin qtt_environment_parameter(size_t index) {
    QttEnvironmentOrigin origin = {
        QTT_ENVIRONMENT_PARAMETER, (uint64_t)index};
    return origin;
}

QttEnvironmentOrigin qtt_environment_local(uint64_t binder_id) {
    QttEnvironmentOrigin origin = {
        QTT_ENVIRONMENT_LOCAL, binder_id};
    return origin;
}

QttEnvironmentOrigin qtt_environment_expression(uint64_t expression_id) {
    QttEnvironmentOrigin origin = {
        QTT_ENVIRONMENT_EXPRESSION, expression_id};
    return origin;
}

bool qtt_environment_validate(const QttClosureEnvironment *environment) {
    if (!environment || !environment->instance_id ||
        !environment->module_id || !environment->closure_id ||
        (environment->slot_count && !environment->origins))
        return false;
    for (size_t i = 0; i < environment->slot_count; i++) {
        QttEnvironmentOrigin origin = environment->origins[i];
        if (origin.kind == QTT_ENVIRONMENT_PARAMETER) {
            if (origin.identity >= environment->parameter_count) return false;
        } else if ((origin.kind == QTT_ENVIRONMENT_LOCAL ||
                    origin.kind == QTT_ENVIRONMENT_EXPRESSION)) {
            if (!origin.identity) return false;
        } else {
            return false;
        }
    }
    return true;
}

QttClosureEnvironment *qtt_environment_new(
    uint64_t instance_id, uint64_t module_id, uint64_t closure_id,
    const QttEnvironmentOrigin *origins, size_t slot_count,
    size_t parameter_count) {
    if (slot_count && !origins) return NULL;
    QttClosureEnvironment *environment =
        calloc(1, sizeof(*environment));
    if (!environment) return NULL;
    environment->instance_id = instance_id;
    environment->module_id = module_id;
    environment->closure_id = closure_id;
    environment->slot_count = slot_count;
    environment->parameter_count = parameter_count;
    if (slot_count) {
        environment->origins =
            malloc(slot_count * sizeof(*environment->origins));
        if (!environment->origins) {
            free(environment);
            return NULL;
        }
        memcpy(environment->origins, origins,
               slot_count * sizeof(*environment->origins));
    }
    if (!qtt_environment_validate(environment)) {
        qtt_environment_free(environment);
        return NULL;
    }
    return environment;
}

QttClosureEnvironment *qtt_environment_substitute(
    const QttClosureEnvironment *environment,
    const QttEnvironmentOrigin *arguments, size_t argument_count) {
    if (!qtt_environment_validate(environment) ||
        (argument_count && !arguments))
        return NULL;
    QttEnvironmentOrigin *origins = environment->slot_count
        ? malloc(environment->slot_count * sizeof(*origins)) : NULL;
    if (environment->slot_count && !origins) return NULL;
    for (size_t i = 0; i < environment->slot_count; i++) {
        origins[i] = environment->origins[i];
        if (origins[i].kind == QTT_ENVIRONMENT_PARAMETER) {
            if (origins[i].identity >= argument_count) {
                free(origins);
                return NULL;
            }
            origins[i] = arguments[origins[i].identity];
        }
    }
    QttClosureEnvironment *result = qtt_environment_new(
        environment->instance_id, environment->module_id,
        environment->closure_id, origins, environment->slot_count, 0);
    free(origins);
    return result;
}

void qtt_environment_free(QttClosureEnvironment *environment) {
    if (!environment) return;
    free(environment->origins);
    free(environment);
}
