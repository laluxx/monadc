#include "quantity.h"

#include <stdint.h>

QttQuantity qtt_quantity_finite(uint64_t value) {
    QttQuantity quantity = {value, false};
    return quantity;
}

QttQuantity qtt_quantity_omega(void) {
    QttQuantity quantity = {0, true};
    return quantity;
}

bool qtt_quantity_is_zero(QttQuantity quantity) {
    return !quantity.is_omega && quantity.finite == 0;
}

bool qtt_quantity_equal(QttQuantity left, QttQuantity right) {
    if (left.is_omega || right.is_omega)
        return left.is_omega == right.is_omega;
    return left.finite == right.finite;
}

bool qtt_quantity_leq(QttQuantity left, QttQuantity right) {
    if (right.is_omega) return true;
    if (left.is_omega) return false;
    return left.finite <= right.finite;
}

QttQuantity qtt_quantity_add(QttQuantity left, QttQuantity right) {
    if (left.is_omega || right.is_omega)
        return qtt_quantity_omega();
    if (UINT64_MAX - left.finite < right.finite)
        return qtt_quantity_omega();
    return qtt_quantity_finite(left.finite + right.finite);
}

QttQuantity qtt_quantity_multiply(QttQuantity left, QttQuantity right) {
    if (qtt_quantity_is_zero(left) || qtt_quantity_is_zero(right))
        return qtt_quantity_finite(0);
    if (left.is_omega || right.is_omega)
        return qtt_quantity_omega();
    if (UINT64_MAX / left.finite < right.finite)
        return qtt_quantity_omega();
    return qtt_quantity_finite(left.finite * right.finite);
}
