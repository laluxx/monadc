#ifndef MONAD_QTT_KIND_H
#define MONAD_QTT_KIND_H

#include <stdbool.h>
#include <stdint.h>

typedef struct QttKind QttKind;
typedef struct QttKindArena QttKindArena;
typedef struct QttKindSolver QttKindSolver;

typedef enum {
    QTT_KIND_TYPE,
    QTT_KIND_EFFECT_ROW,
    QTT_KIND_ARROW,
    QTT_KIND_META,
} QttKindTag;

typedef enum {
    QTT_KIND_SOLVED,
    QTT_KIND_MISMATCH,
    QTT_KIND_OCCURS,
    QTT_KIND_OUT_OF_MEMORY,
} QttKindResult;

QttKindArena *qtt_kind_arena_new(void);
void qtt_kind_arena_free(QttKindArena *arena);
QttKind *qtt_kind_type(QttKindArena *arena);
QttKind *qtt_kind_effect_row(QttKindArena *arena);
QttKind *qtt_kind_arrow(QttKindArena *arena, QttKind *domain, QttKind *codomain);
QttKind *qtt_kind_fresh(QttKindArena *arena);

QttKindSolver *qtt_kind_solver_new(QttKindArena *arena);
void qtt_kind_solver_free(QttKindSolver *solver);
QttKindResult qtt_kind_unify(QttKindSolver *solver, QttKind *left, QttKind *right);
bool qtt_kind_equal(QttKindSolver *solver, QttKind *left, QttKind *right);

QttKindTag qtt_kind_tag(const QttKind *kind);
const char *qtt_kind_name(const QttKind *kind);
char *qtt_kind_serialize(QttKindSolver *solver, QttKind *kind);
QttKind *qtt_kind_deserialize(QttKindArena *arena, const char *text);
uint64_t qtt_kind_fingerprint(QttKindSolver *solver, QttKind *kind);

#endif
