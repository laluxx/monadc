#ifndef MONAD_QTT_RULES_H
#define MONAD_QTT_RULES_H

#include "quantity.h"

#include <stdbool.h>
#include <stddef.h>

/* Canonical solved instances of Monad's quantitative typing rules. */
static inline QttQuantity qtt_rule_sequence(
    QttQuantity left, QttQuantity right) {
    return qtt_quantity_add(left, right);
}

static inline QttQuantity qtt_rule_choice(
    QttQuantity left, QttQuantity right) {
    if (left.is_omega || right.is_omega) return qtt_quantity_omega();
    return left.finite >= right.finite ? left : right;
}

static inline QttQuantity qtt_rule_let(
    QttQuantity value, QttQuantity body, QttQuantity binder_uses,
    bool subject_is_binder) {
    QttQuantity remaining = subject_is_binder
        ? qtt_quantity_finite(0) : body;
    return qtt_rule_sequence(
        remaining, qtt_quantity_multiply(binder_uses, value));
}

static inline QttQuantity qtt_rule_capture(
    QttQuantity latent, bool subject_is_parameter) {
    if (subject_is_parameter || qtt_quantity_is_zero(latent))
        return qtt_quantity_finite(0);
    return qtt_quantity_finite(1);
}

static inline QttQuantity qtt_rule_direct_application(
    QttQuantity construction, QttQuantity latent,
    const QttQuantity *arguments, const QttQuantity *domains,
    size_t argument_count) {
    QttQuantity result = qtt_rule_sequence(construction, latent);
    for (size_t i = 0; i < argument_count; i++)
        result = qtt_rule_sequence(
            result, qtt_quantity_multiply(domains[i], arguments[i]));
    return result;
}

#endif
