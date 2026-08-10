#include "constraints.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    EFFECT_CONSTRAINT_EQUAL,
    EFFECT_CONSTRAINT_SUBROW,
    EFFECT_CONSTRAINT_JOIN,
    EFFECT_CONSTRAINT_HAS_TRAIT,
    EFFECT_CONSTRAINT_LACKS,
} EffectConstraintKind;

typedef struct {
    EffectConstraintKind kind;
    QttEffectRow *left;
    QttEffectRow *right;
    QttEffectRow *result;
    char *trait;
} EffectConstraint;

struct QttEffectConstraintSet {
    EffectConstraint *items;
    size_t count;
    size_t capacity;
};

struct QttEffectConstraintCertificate {
    EffectConstraint *items;
    QttEffectObligationStatus *statuses;
    size_t count;
};

static char *constraint_text_copy(const char *text) {
    if (!text) return NULL;
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

QttEffectConstraintSet *qtt_effect_constraints_new(void) {
    return calloc(1, sizeof(QttEffectConstraintSet));
}

void qtt_effect_constraints_free(QttEffectConstraintSet *set) {
    if (!set) return;
    for (size_t i = 0; i < set->count; i++) free(set->items[i].trait);
    free(set->items);
    free(set);
}

char *qtt_effect_certificate_trait_witness(
    const QttEffectConstraintCertificate *certificate, size_t index,
    const QttEffectSolver *solver) {
    if (!certificate || index >= certificate->count || !solver ||
        certificate->items[index].kind != EFFECT_CONSTRAINT_HAS_TRAIT ||
        certificate->statuses[index] != QTT_EFFECT_OBLIGATION_PROVED)
        return NULL;
    char *witness = NULL;
    return qtt_effect_row_satisfies_trait(
        solver, certificate->items[index].left,
        certificate->items[index].trait, &witness) == QTT_EFFECT_RELATION_PROVED
        ? witness : NULL;
}

bool qtt_effect_constrain_has_trait(
    QttEffectConstraintSet *set, QttEffectRow *row, const char *trait) {
    if (!set || !row || !trait || strchr(trait, ',')) return false;
    if (set->count == set->capacity) {
        size_t capacity = set->capacity ? set->capacity * 2 : 8;
        EffectConstraint *grown = realloc(set->items, capacity * sizeof(*grown));
        if (!grown) return false;
        set->items = grown; set->capacity = capacity;
    }
    char *copy = constraint_text_copy(trait); if (!copy) return false;
    set->items[set->count++] = (EffectConstraint){
        .kind = EFFECT_CONSTRAINT_HAS_TRAIT, .left = row, .trait = copy};
    return true;
}

bool qtt_effect_constrain_lacks(
    QttEffectConstraintSet *set, QttEffectRow *row, const char *label) {
    if (!set || !row || !label || !*label || strchr(label, ',')) return false;
    if (set->count == set->capacity) {
        size_t capacity = set->capacity ? set->capacity * 2 : 8;
        EffectConstraint *grown = realloc(set->items, capacity * sizeof(*grown));
        if (!grown) return false;
        set->items = grown;
        set->capacity = capacity;
    }
    char *copy = constraint_text_copy(label);
    if (!copy) return false;
    set->items[set->count++] = (EffectConstraint){
        .kind = EFFECT_CONSTRAINT_LACKS, .left = row, .trait = copy};
    return true;
}

size_t qtt_effect_constraints_count(const QttEffectConstraintSet *set) {
    return set ? set->count : 0;
}

static bool append(
    QttEffectConstraintSet *set, EffectConstraintKind kind,
    QttEffectRow *left, QttEffectRow *right, QttEffectRow *result) {
    if (!set || !left || !right ||
        (kind == EFFECT_CONSTRAINT_JOIN && !result)) return false;
    if (set->count == set->capacity) {
        size_t capacity = set->capacity ? set->capacity * 2 : 8;
        EffectConstraint *grown = realloc(
            set->items, capacity * sizeof(*grown));
        if (!grown) return false;
        set->items = grown;
        set->capacity = capacity;
    }
    set->items[set->count++] = (EffectConstraint){
        .kind = kind, .left = left, .right = right, .result = result};
    return true;
}

bool qtt_effect_constrain_equal(
    QttEffectConstraintSet *set, QttEffectRow *left,
    QttEffectRow *right) {
    return append(set, EFFECT_CONSTRAINT_EQUAL, left, right, NULL);
}

bool qtt_effect_constrain_subrow(
    QttEffectConstraintSet *set, QttEffectRow *sub,
    QttEffectRow *super) {
    return append(set, EFFECT_CONSTRAINT_SUBROW, sub, super, NULL);
}

bool qtt_effect_constrain_join(
    QttEffectConstraintSet *set, QttEffectRow *left,
    QttEffectRow *right, QttEffectRow *result) {
    return append(set, EFFECT_CONSTRAINT_JOIN, left, right, result);
}

static QttEffectObligationStatus evaluate(
    const EffectConstraint *item, QttEffectArena *arena,
    QttEffectSolver *solver, bool may_unify, bool *out_of_memory) {
    if (item->kind == EFFECT_CONSTRAINT_HAS_TRAIT ||
        item->kind == EFFECT_CONSTRAINT_LACKS) {
        QttEffectRelation relation = item->kind == EFFECT_CONSTRAINT_HAS_TRAIT
            ? qtt_effect_row_satisfies_trait(
                  solver, item->left, item->trait, NULL)
            : qtt_effect_row_lacks(solver, item->left, item->trait);
        return relation == QTT_EFFECT_RELATION_PROVED
            ? QTT_EFFECT_OBLIGATION_PROVED
            : relation == QTT_EFFECT_RELATION_REFUTED
                ? QTT_EFFECT_OBLIGATION_REFUTED
                : QTT_EFFECT_OBLIGATION_RESIDUAL;
    }
    if (item->kind == EFFECT_CONSTRAINT_EQUAL) {
        if (may_unify && qtt_effect_unify(
                solver, item->left, item->right) != QTT_EFFECT_UNIFY_OK)
            return QTT_EFFECT_OBLIGATION_REFUTED;
        return qtt_effect_rows_equal(solver, item->left, item->right)
            ? QTT_EFFECT_OBLIGATION_PROVED
            : QTT_EFFECT_OBLIGATION_REFUTED;
    }
    if (item->kind == EFFECT_CONSTRAINT_SUBROW) {
        QttEffectRelation relation;
        QttEffectUnifyResult refined = may_unify
            ? qtt_effect_refine_subrow(
                  solver, item->left, item->right, &relation)
            : QTT_EFFECT_UNIFY_OK;
        if (!may_unify)
            relation = qtt_effect_subrow(
                solver, item->left, item->right);
        if (refined == QTT_EFFECT_UNIFY_OUT_OF_MEMORY) {
            if (out_of_memory) *out_of_memory = true;
            return QTT_EFFECT_OBLIGATION_RESIDUAL;
        }
        return relation == QTT_EFFECT_RELATION_PROVED
            ? QTT_EFFECT_OBLIGATION_PROVED
            : relation == QTT_EFFECT_RELATION_REFUTED
                ? QTT_EFFECT_OBLIGATION_REFUTED
                : QTT_EFFECT_OBLIGATION_RESIDUAL;
    }
    if (may_unify && qtt_effect_is_closed(solver, item->left) &&
        qtt_effect_is_closed(solver, item->right)) {
        QttEffectRow *joined = qtt_effect_join_closed(
            arena, solver, item->left, item->right);
        if (!joined) {
            if (out_of_memory) *out_of_memory = true;
            return QTT_EFFECT_OBLIGATION_RESIDUAL;
        }
        QttEffectUnifyResult unified = qtt_effect_unify(
            solver, item->result, joined);
        if (unified == QTT_EFFECT_UNIFY_OUT_OF_MEMORY) {
            if (out_of_memory) *out_of_memory = true;
            return QTT_EFFECT_OBLIGATION_RESIDUAL;
        }
        if (unified != QTT_EFFECT_UNIFY_OK)
            return QTT_EFFECT_OBLIGATION_REFUTED;
    }
    QttEffectRelation left = qtt_effect_subrow(
        solver, item->left, item->result);
    QttEffectRelation right = qtt_effect_subrow(
        solver, item->right, item->result);
    if (left == QTT_EFFECT_RELATION_REFUTED ||
        right == QTT_EFFECT_RELATION_REFUTED)
        return QTT_EFFECT_OBLIGATION_REFUTED;
    if (left == QTT_EFFECT_RELATION_UNKNOWN ||
        right == QTT_EFFECT_RELATION_UNKNOWN ||
        !qtt_effect_is_closed(solver, item->left) ||
        !qtt_effect_is_closed(solver, item->right) ||
        !qtt_effect_is_closed(solver, item->result))
        return QTT_EFFECT_OBLIGATION_RESIDUAL;
    QttEffectRow *joined = qtt_effect_join_closed(
        arena, solver, item->left, item->right);
    return joined && qtt_effect_rows_equal(solver, joined, item->result)
        ? QTT_EFFECT_OBLIGATION_PROVED
        : QTT_EFFECT_OBLIGATION_REFUTED;
}

static bool same_constraint(
    const EffectConstraint *left, const EffectConstraint *right,
    const QttEffectSolver *solver) {
    if (left->kind != right->kind) return false;
    if (left->kind == EFFECT_CONSTRAINT_HAS_TRAIT ||
        left->kind == EFFECT_CONSTRAINT_LACKS)
        return !strcmp(left->trait, right->trait) &&
            qtt_effect_rows_equal(solver, left->left, right->left);
    if (left->kind == EFFECT_CONSTRAINT_JOIN) {
        bool operands =
            (qtt_effect_rows_equal(solver, left->left, right->left) &&
             qtt_effect_rows_equal(solver, left->right, right->right)) ||
            (qtt_effect_rows_equal(solver, left->left, right->right) &&
             qtt_effect_rows_equal(solver, left->right, right->left));
        return operands && qtt_effect_rows_equal(
            solver, left->result, right->result);
    }
    return qtt_effect_rows_equal(solver, left->left, right->left) &&
           qtt_effect_rows_equal(solver, left->right, right->right);
}

static QttEffectConstraintResult collapse_inclusion_sccs(
    const QttEffectConstraintSet *set, QttEffectSolver *solver) {
    size_t capacity = set->count > SIZE_MAX / 2 ? 0 : set->count * 2;
    if (set->count && !capacity)
        return QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY;
    QttEffectRow **nodes = capacity
        ? malloc(capacity * sizeof(*nodes)) : NULL;
    if (capacity && !nodes)
        return QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY;
    size_t count = 0;
    for (size_t i = 0; i < set->count; i++) {
        if (set->items[i].kind != EFFECT_CONSTRAINT_SUBROW) continue;
        QttEffectRow *ends[] = {
            set->items[i].left, set->items[i].right};
        for (size_t e = 0; e < 2; e++) {
            size_t j = 0;
            while (j < count && nodes[j] != ends[e]) j++;
            if (j == count) nodes[count++] = ends[e];
        }
    }
    if (count && count > SIZE_MAX / count) {
        free(nodes);
        return QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY;
    }
    bool *reach = count ? calloc(count * count, sizeof(*reach)) : NULL;
    if (count && !reach) {
        free(nodes);
        return QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < count; i++) reach[i * count + i] = true;
    for (size_t i = 0; i < set->count; i++) {
        if (set->items[i].kind != EFFECT_CONSTRAINT_SUBROW) continue;
        size_t from = 0, to = 0;
        while (nodes[from] != set->items[i].left) from++;
        while (nodes[to] != set->items[i].right) to++;
        reach[from * count + to] = true;
    }
    for (size_t k = 0; k < count; k++)
        for (size_t i = 0; i < count; i++)
            if (reach[i * count + k])
                for (size_t j = 0; j < count; j++)
                    reach[i * count + j] = reach[i * count + j] ||
                        reach[k * count + j];
    QttEffectConstraintResult result = QTT_EFFECT_CONSTRAINT_SOLVED;
    for (size_t i = 0; i < count; i++) {
        bool has_earlier_representative = false;
        for (size_t earlier = 0; earlier < i; earlier++)
            if (reach[i * count + earlier] &&
                reach[earlier * count + i]) {
                has_earlier_representative = true;
                break;
            }
        if (has_earlier_representative) continue;
        for (size_t j = i + 1; j < count; j++)
            if (reach[i * count + j] && reach[j * count + i]) {
                QttEffectUnifyResult unified = qtt_effect_unify(
                    solver, nodes[i], nodes[j]);
                if (unified == QTT_EFFECT_UNIFY_OUT_OF_MEMORY)
                    result = QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY;
                else if (unified != QTT_EFFECT_UNIFY_OK &&
                         result != QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY)
                    result = QTT_EFFECT_CONSTRAINT_REJECTED;
            }
    }
    free(reach);
    free(nodes);
    return result;
}

QttEffectConstraintResult qtt_effect_constraints_solve(
    const QttEffectConstraintSet *set, QttEffectArena *arena,
    QttEffectSolver *solver, QttEffectConstraintCertificate **out) {
    if (out) *out = NULL;
    if (!set || !arena || !solver || !out)
        return QTT_EFFECT_CONSTRAINT_REJECTED;
    QttEffectConstraintCertificate *certificate =
        calloc(1, sizeof(*certificate));
    if (!certificate) return QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY;
    certificate->count = set->count;
    certificate->items = set->count
        ? malloc(set->count * sizeof(*certificate->items)) : NULL;
    certificate->statuses = set->count
        ? malloc(set->count * sizeof(*certificate->statuses)) : NULL;
    if (set->count && (!certificate->items || !certificate->statuses)) {
        qtt_effect_certificate_free(certificate);
        return QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY;
    }
    if (set->count) {
        memcpy(certificate->items, set->items,
               set->count * sizeof(*certificate->items));
        for (size_t i = 0; i < set->count; i++)
            certificate->items[i].trait = NULL;
        for (size_t i = 0; i < set->count; i++)
            if (set->items[i].trait) {
                certificate->items[i].trait =
                    constraint_text_copy(set->items[i].trait);
                if (!certificate->items[i].trait) {
                    qtt_effect_certificate_free(certificate);
                    return QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY;
                }
            }
    }
    QttEffectConstraintResult scc_result =
        collapse_inclusion_sccs(set, solver);
    for (size_t i = 0; i < set->count; i++)
        certificate->statuses[i] = QTT_EFFECT_OBLIGATION_RESIDUAL;
    bool out_of_memory = false;
    /* Row bindings only become more informative. Revisit earlier residuals
     * until a pass changes no logical status; at most one newly proved
     * obligation is needed per productive pass. */
    for (size_t pass = 0; pass <= set->count; pass++) {
        bool changed = false;
        for (size_t i = 0; i < set->count; i++) {
            QttEffectObligationStatus status = evaluate(
                &set->items[i], arena, solver, true, &out_of_memory);
            if (status != certificate->statuses[i]) changed = true;
            certificate->statuses[i] = status;
        }
        if (out_of_memory || !changed) break;
    }
    QttEffectConstraintResult result = QTT_EFFECT_CONSTRAINT_SOLVED;
    for (size_t i = 0; i < set->count; i++) {
        if (certificate->statuses[i] == QTT_EFFECT_OBLIGATION_REFUTED)
            result = QTT_EFFECT_CONSTRAINT_REJECTED;
        else if (certificate->statuses[i] ==
                     QTT_EFFECT_OBLIGATION_RESIDUAL &&
                 result != QTT_EFFECT_CONSTRAINT_REJECTED)
            result = QTT_EFFECT_CONSTRAINT_RESIDUAL;
    }
    if (out_of_memory ||
        scc_result == QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY)
        result = QTT_EFFECT_CONSTRAINT_OUT_OF_MEMORY;
    else if (scc_result == QTT_EFFECT_CONSTRAINT_REJECTED)
        result = QTT_EFFECT_CONSTRAINT_REJECTED;
    /* Canonical residual graph quotient: identical propositions share one
     * certificate node. Join operands are commutative. Stable first
     * occurrence preserves useful source order without graph inflation. */
    size_t unique = 0;
    for (size_t i = 0; i < certificate->count; i++) {
        bool duplicate = false;
        for (size_t j = 0;
             certificate->statuses[i] == QTT_EFFECT_OBLIGATION_RESIDUAL &&
             j < unique; j++)
            if (certificate->statuses[j] ==
                    QTT_EFFECT_OBLIGATION_RESIDUAL &&
                same_constraint(
                    &certificate->items[i], &certificate->items[j],
                    solver)) {
                duplicate = true;
                break;
            }
        if (duplicate) {
            free(certificate->items[i].trait);
            certificate->items[i].trait = NULL;
            continue;
        }
        certificate->items[unique] = certificate->items[i];
        if (unique != i) certificate->items[i].trait = NULL;
        certificate->statuses[unique] = certificate->statuses[i];
        unique++;
    }
    certificate->count = unique;
    *out = certificate;
    return result;
}

size_t qtt_effect_certificate_count(
    const QttEffectConstraintCertificate *certificate) {
    return certificate ? certificate->count : 0;
}

QttEffectObligationStatus qtt_effect_certificate_status(
    const QttEffectConstraintCertificate *certificate, size_t index) {
    return !certificate || index >= certificate->count
        ? QTT_EFFECT_OBLIGATION_REFUTED : certificate->statuses[index];
}

bool qtt_effect_certificate_verify(
    const QttEffectConstraintCertificate *certificate,
    QttEffectArena *arena, QttEffectSolver *solver) {
    if (!certificate || !arena || !solver) return false;
    for (size_t i = 0; i < certificate->count; i++) {
        if (certificate->statuses[i] == QTT_EFFECT_OBLIGATION_REFUTED ||
            evaluate(&certificate->items[i], arena, solver, false, NULL) !=
                certificate->statuses[i])
            return false;
    }
    return true;
}

typedef struct {
    char *text;
    size_t length;
    size_t capacity;
} EffectText;

typedef struct {
    uint64_t *actual;
    size_t count;
    size_t capacity;
} EffectAlphaNames;

static bool text_append(EffectText *out, const char *text, size_t length) {
    if (!out || (!text && length)) return false;
    if (length > SIZE_MAX - out->length - 1) return false;
    size_t required = out->length + length + 1;
    if (required > out->capacity) {
        size_t capacity = out->capacity ? out->capacity : 128;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        char *grown = realloc(out->text, capacity);
        if (!grown) return false;
        out->text = grown;
        out->capacity = capacity;
    }
    if (length) memcpy(out->text + out->length, text, length);
    out->length += length;
    out->text[out->length] = '\0';
    return true;
}

static bool text_word(EffectText *out, const char *text) {
    return text_append(out, text, strlen(text));
}

static bool alpha_name(
    EffectAlphaNames *names, uint64_t actual, size_t *canonical) {
    size_t index = 0;
    while (index < names->count && names->actual[index] != actual) index++;
    if (index == names->count) {
        if (names->count == names->capacity) {
            size_t capacity = names->capacity ? names->capacity * 2 : 8;
            uint64_t *grown = realloc(
                names->actual, capacity * sizeof(*grown));
            if (!grown) return false;
            names->actual = grown;
            names->capacity = capacity;
        }
        names->actual[names->count++] = actual;
    }
    *canonical = index;
    return true;
}

static bool append_normalized_row(
    EffectText *out, EffectAlphaNames *names,
    const QttEffectSolver *solver, const QttEffectRow *row) {
    char *raw = qtt_effect_format(solver, row);
    if (!raw) return false;
    char *tail = strstr(raw, "|e");
    bool ok = true;
    if (!tail) {
        ok = text_word(out, raw);
    } else {
        char *digits = tail + 2;
        char *end = NULL;
        uint64_t actual = strtoull(digits, &end, 10);
        size_t canonical = 0;
        char name[48];
        ok = end != digits && alpha_name(names, actual, &canonical) &&
            text_append(out, raw, (size_t)(digits - raw)) &&
            snprintf(name, sizeof(name), "%zu", canonical) > 0 &&
            text_word(out, name) && text_word(out, end);
    }
    free(raw);
    return ok;
}

static char *row_shape(
    const QttEffectSolver *solver, const QttEffectRow *row) {
    char *raw = qtt_effect_format(solver, row);
    if (!raw) return NULL;
    char *tail = strstr(raw, "|e");
    if (!tail) return raw;
    char *digits = tail + 2;
    char *end = digits;
    while (*end >= '0' && *end <= '9') end++;
    size_t prefix = (size_t)(digits - raw);
    size_t suffix = strlen(end);
    char *shape = malloc(prefix + 1 + suffix + 1);
    if (shape) {
        memcpy(shape, raw, prefix);
        shape[prefix] = '*';
        memcpy(shape + prefix + 1, end, suffix + 1);
    }
    free(raw);
    return shape;
}

typedef struct {
    size_t index;
    char *key;
    bool swap_join;
} EffectCertificateOrder;

static int compare_certificate_order(const void *left, const void *right) {
    const EffectCertificateOrder *a = left;
    const EffectCertificateOrder *b = right;
    int order = strcmp(a->key, b->key);
    if (order) return order;
    return a->index < b->index ? -1 : a->index != b->index;
}

static const char *obligation_status_name(QttEffectObligationStatus status) {
    return status == QTT_EFFECT_OBLIGATION_PROVED
        ? "proved" : status == QTT_EFFECT_OBLIGATION_RESIDUAL
            ? "residual" : "refuted";
}

static const char *constraint_kind_name(EffectConstraintKind kind) {
    return kind == EFFECT_CONSTRAINT_EQUAL
        ? "equal" : kind == EFFECT_CONSTRAINT_SUBROW
            ? "subrow" : kind == EFFECT_CONSTRAINT_JOIN
                ? "join" : kind == EFFECT_CONSTRAINT_HAS_TRAIT
                    ? "has-trait" : "lacks";
}

static bool build_certificate_order(
    const QttEffectConstraintCertificate *certificate,
    const QttEffectSolver *solver, EffectCertificateOrder *order) {
    for (size_t i = 0; i < certificate->count; i++) {
        const EffectConstraint *item = &certificate->items[i];
        char *left = row_shape(solver, item->left);
        bool textual = item->kind == EFFECT_CONSTRAINT_HAS_TRAIT ||
            item->kind == EFFECT_CONSTRAINT_LACKS;
        char *right = textual
            ? constraint_text_copy(item->trait)
            : row_shape(solver, item->right);
        char *result = item->kind == EFFECT_CONSTRAINT_JOIN
            ? row_shape(solver, item->result) : NULL;
        if (!left || !right ||
            (item->kind == EFFECT_CONSTRAINT_JOIN && !result)) {
            free(left); free(right); free(result);
            for (size_t j = 0; j < i; j++) free(order[j].key);
            return false;
        }
        bool swap = item->kind == EFFECT_CONSTRAINT_JOIN &&
            strcmp(left, right) > 0;
        const char *first = swap ? right : left;
        const char *second = swap ? left : right;
        size_t size = strlen(constraint_kind_name(item->kind)) +
            strlen(obligation_status_name(certificate->statuses[i])) +
            strlen(first) + strlen(second) +
            (result ? strlen(result) : 0) + 8;
        char *key = malloc(size);
        if (!key) {
            free(left); free(right); free(result);
            for (size_t j = 0; j < i; j++) free(order[j].key);
            return false;
        }
        snprintf(key, size, "%s %s %s %s %s",
                 constraint_kind_name(item->kind),
                 obligation_status_name(certificate->statuses[i]),
                 first, second, result ? result : "");
        order[i] = (EffectCertificateOrder){
            .index = i, .key = key, .swap_join = swap};
        free(left); free(right); free(result);
    }
    qsort(order, certificate->count, sizeof(*order),
          compare_certificate_order);
    return true;
}

char *qtt_effect_certificate_format(
    const QttEffectConstraintCertificate *certificate,
    const QttEffectSolver *solver) {
    if (!certificate || !solver) return NULL;
    EffectText out = {0};
    EffectAlphaNames names = {0};
    EffectCertificateOrder *order = certificate->count
        ? calloc(certificate->count, sizeof(*order)) : NULL;
    if (certificate->count &&
        (!order || !build_certificate_order(certificate, solver, order))) {
        free(order);
        return NULL;
    }
    bool has_trait = false;
    bool has_lacks = false;
    for (size_t i = 0; i < certificate->count; i++) {
        has_trait = has_trait ||
            certificate->items[i].kind == EFFECT_CONSTRAINT_HAS_TRAIT;
        has_lacks = has_lacks ||
            certificate->items[i].kind == EFFECT_CONSTRAINT_LACKS;
    }
    bool valid = text_word(&out, has_lacks
        ? "monad-effect-certificate-v3\n"
        : has_trait ? "monad-effect-certificate-v2\n"
                    : "monad-effect-certificate-v1\n");
    for (size_t position = 0;
         valid && position < certificate->count; position++) {
        size_t i = order[position].index;
        const EffectConstraint *item = &certificate->items[i];
        const char *status = obligation_status_name(certificate->statuses[i]);
        const char *kind = constraint_kind_name(item->kind);
        const QttEffectRow *left = order[position].swap_join
            ? item->right : item->left;
        const QttEffectRow *right = order[position].swap_join
            ? item->left : item->right;
        bool ok = text_word(&out, kind) && text_word(&out, " ") &&
            text_word(&out, status) && text_word(&out, " ") &&
            append_normalized_row(&out, &names, solver, left);
        if (ok && (item->kind == EFFECT_CONSTRAINT_HAS_TRAIT ||
                   item->kind == EFFECT_CONSTRAINT_LACKS))
            ok = text_word(&out, " : ") && text_word(&out, item->trait);
        else if (ok) ok =
            text_word(&out, item->kind == EFFECT_CONSTRAINT_EQUAL
                ? " = " : item->kind == EFFECT_CONSTRAINT_SUBROW
                    ? " <= " : " join ") &&
            append_normalized_row(&out, &names, solver, right);
        if (ok && item->kind == EFFECT_CONSTRAINT_JOIN)
            ok = text_word(&out, " = ") && append_normalized_row(
                &out, &names, solver, item->result);
        if (ok) ok = text_word(&out, "\n");
        if (!ok) {
            valid = false;
            break;
        }
    }
    for (size_t i = 0; i < certificate->count; i++) free(order[i].key);
    free(order);
    free(names.actual);
    if (!valid) {
        free(out.text);
        return NULL;
    }
    return out.text;
}

uint64_t qtt_effect_certificate_fingerprint(
    const QttEffectConstraintCertificate *certificate,
    const QttEffectSolver *solver) {
    char *text = qtt_effect_certificate_format(certificate, solver);
    if (!text) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    free(text);
    return hash ? hash : 1;
}

void qtt_effect_certificate_free(
    QttEffectConstraintCertificate *certificate) {
    if (!certificate) return;
    for (size_t i = 0; i < certificate->count; i++)
        free(certificate->items[i].trait);
    free(certificate->items);
    free(certificate->statuses);
    free(certificate);
}
