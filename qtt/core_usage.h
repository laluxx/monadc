#ifndef MONAD_QTT_CORE_USAGE_H
#define MONAD_QTT_CORE_USAGE_H

#include "core.h"
#include "elaboration.h"
#include "signature_env.h"
#include "callable_env.h"

typedef enum {
    QTT_CORE_USAGE_OK,
    QTT_CORE_USAGE_UNSUPPORTED,
    QTT_CORE_USAGE_INVALID,
    QTT_CORE_USAGE_OUT_OF_MEMORY,
} QttCoreUsageStatus;

typedef enum {
    QTT_CORE_USAGE_BOUNDARY_NONE,
    QTT_CORE_USAGE_BOUNDARY_APPLICATION,
} QttCoreUsageBoundary;

typedef struct {
    QttCoreUsageStatus status;
    QttCoreUsageBoundary boundary;
    const QttCoreNode *offending;
    QttUsageContext *usage;
} QttCoreUsageResult;

QttCoreUsageResult qtt_core_usage_lower(
    QttGradeArena *arena, const QttCoreNode *core);
QttCoreUsageResult qtt_core_usage_lower_in_env(
    QttGradeArena *arena, const QttCoreNode *core,
    const QttSignatureEnv *signatures, uint64_t module_id);
QttCoreUsageResult qtt_core_usage_lower_lexical(
    QttGradeArena *arena, const QttCoreNode *core,
    const QttSignatureEnv *signatures, uint64_t module_id,
    const QttCallableBinding *callables);

#endif
