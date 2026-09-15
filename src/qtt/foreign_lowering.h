#ifndef MONAD_QTT_FOREIGN_LOWERING_H
#define MONAD_QTT_FOREIGN_LOWERING_H

#include "resource.h"

typedef enum {
    QTT_FOREIGN_RUNTIME_NONE,
    QTT_FOREIGN_RUNTIME_RETAIN_SHARED,
    QTT_FOREIGN_RUNTIME_RELEASE,
} QttForeignRuntimeAction;

typedef struct {
    QttForeignRuntimeAction action;
    const char *symbol;
} QttForeignLowering;

typedef enum {
    QTT_FOREIGN_LOWER_OK,
    QTT_FOREIGN_LOWER_INVALID_PROOF,
    QTT_FOREIGN_LOWER_NOT_FOREIGN,
    QTT_FOREIGN_LOWER_UNSUPPORTED_OPERATION,
    QTT_FOREIGN_LOWER_OUT_OF_MEMORY,
} QttForeignLowerError;

QttForeignLowerError qtt_foreign_lower_certified(
    const QttResourceBlock *block, size_t operation_index,
    QttForeignLowering *lowering);

#endif
