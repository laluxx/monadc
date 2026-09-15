#ifndef MONAD_QTT_QUANTITY_H
#define MONAD_QTT_QUANTITY_H

/* Solved quantities and their semiring operations. */

#include <stdbool.h>
#include <stdint.h>

/*
 * A solved quantitative usage in N union {omega}.
 *
 * Symbolic quantities live in the future constraint layer; this deliberately
 * small value type is the exact arithmetic used by constants and solutions.
 *
 * Theory:
 * - Atkey, "The Syntax and Semantics of Quantitative Type Theory" (LICS 2018)
 *   https://bentnib.org/quantitative-type-theory.pdf
 * - Brady, "Idris 2: Quantitative Type Theory in Practice" (ECOOP 2021)
 *   https://arxiv.org/abs/2104.00480
 */
typedef struct {
    uint64_t finite;
    bool is_omega;
} QttQuantity;

QttQuantity qtt_quantity_finite(uint64_t value);
QttQuantity qtt_quantity_omega(void);

bool qtt_quantity_is_zero(QttQuantity quantity);
bool qtt_quantity_equal(QttQuantity left, QttQuantity right);
bool qtt_quantity_leq(QttQuantity left, QttQuantity right);

QttQuantity qtt_quantity_add(QttQuantity left, QttQuantity right);
QttQuantity qtt_quantity_multiply(QttQuantity left, QttQuantity right);

#endif
