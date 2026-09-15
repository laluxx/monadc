#ifndef MONAD_QTT_ENVIRONMENT_H
#define MONAD_QTT_ENVIRONMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    QTT_ENVIRONMENT_PARAMETER,
    QTT_ENVIRONMENT_LOCAL,
    QTT_ENVIRONMENT_EXPRESSION,
} QttEnvironmentOriginKind;

typedef struct {
    QttEnvironmentOriginKind kind;
    uint64_t identity;
} QttEnvironmentOrigin;

typedef struct QttClosureEnvironment {
    uint64_t instance_id;
    uint64_t module_id;
    uint64_t closure_id;
    QttEnvironmentOrigin *origins;
    size_t slot_count;
    size_t parameter_count;
} QttClosureEnvironment;

QttEnvironmentOrigin qtt_environment_parameter(size_t index);
QttEnvironmentOrigin qtt_environment_local(uint64_t binder_id);
QttEnvironmentOrigin qtt_environment_expression(uint64_t expression_id);
QttClosureEnvironment *qtt_environment_new(
    uint64_t instance_id, uint64_t module_id, uint64_t closure_id,
    const QttEnvironmentOrigin *origins, size_t slot_count,
    size_t parameter_count);
bool qtt_environment_validate(const QttClosureEnvironment *environment);
QttClosureEnvironment *qtt_environment_substitute(
    const QttClosureEnvironment *environment,
    const QttEnvironmentOrigin *arguments, size_t argument_count);
void qtt_environment_free(QttClosureEnvironment *environment);

#endif
