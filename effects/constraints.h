#ifndef MONAD_EFFECTS_CONSTRAINTS_H
#define MONAD_EFFECTS_CONSTRAINTS_H

#include "effect.h"

typedef struct QttEffectConstraintSet QttEffectConstraintSet;
typedef struct QttEffectConstraintCertificate QttEffectConstraintCertificate;

typedef enum {
    QTT_EFFECT_CONSTRAINT_SOLVED,
    QTT_EFFECT_CONSTRAINT_RESIDUAL,
    QTT_EFFECT_CONSTRAINT_REJECTED,
    QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY,
} QttEffectConstraintResult;

typedef enum {
    QTT_EFFECT_OBLIGATION_PROVED,
    QTT_EFFECT_OBLIGATION_RESIDUAL,
    QTT_EFFECT_OBLIGATION_REFUTED,
} QttEffectObligationStatus;

QttEffectConstraintSet *qtt_effect_constraints_new(void);
void qtt_effect_constraints_free(QttEffectConstraintSet *set);
size_t qtt_effect_constraints_count(const QttEffectConstraintSet *set);
bool qtt_effect_constrain_equal(
    QttEffectConstraintSet *set, QttEffectRow *left, QttEffectRow *right);
bool qtt_effect_constrain_subrow(
    QttEffectConstraintSet *set, QttEffectRow *sub, QttEffectRow *super);
bool qtt_effect_constrain_join(
    QttEffectConstraintSet *set, QttEffectRow *left, QttEffectRow *right,
    QttEffectRow *result);
bool qtt_effect_constrain_has_trait(
    QttEffectConstraintSet *set, QttEffectRow *row, const char *trait);
bool qtt_effect_constrain_lacks(
    QttEffectConstraintSet *set, QttEffectRow *row, const char *label);
char *qtt_effect_certificate_trait_witness(
    const QttEffectConstraintCertificate *certificate, size_t index,
    const QttEffectSolver *solver);
QttEffectConstraintResult qtt_effect_constraints_solve(
    const QttEffectConstraintSet *set, QttEffectArena *arena,
    QttEffectSolver *solver, QttEffectConstraintCertificate **certificate);

size_t qtt_effect_certificate_count(
    const QttEffectConstraintCertificate *certificate);
QttEffectObligationStatus qtt_effect_certificate_status(
    const QttEffectConstraintCertificate *certificate, size_t index);
bool qtt_effect_certificate_verify(
    const QttEffectConstraintCertificate *certificate,
    QttEffectArena *arena, QttEffectSolver *solver);
char *qtt_effect_certificate_format(
    const QttEffectConstraintCertificate *certificate,
    const QttEffectSolver *solver);
uint64_t qtt_effect_certificate_fingerprint(
    const QttEffectConstraintCertificate *certificate,
    const QttEffectSolver *solver);
void qtt_effect_certificate_free(QttEffectConstraintCertificate *certificate);

#endif
