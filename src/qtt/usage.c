#include "usage.h"

#include "bindings.h"
#include "quantity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    QTT_USAGE_FINITE,
    QTT_USAGE_OMEGA,
    QTT_USAGE_ADD,
    QTT_USAGE_CHOICE,
    QTT_USAGE_CAPTURE,
    QTT_USAGE_REPEAT,
} QttUsageKind;

typedef struct QttUsageExpr {
    QttUsageKind kind;
    QttQuantity finite;
    struct QttUsageExpr *left;
    struct QttUsageExpr *right;
} QttUsageExpr;

typedef struct {
    char *name;
    uint64_t binder_id;
    char *formatted;
} QttUsageEntry;

struct QttUsageReport {
    QttUsageEntry *entries;
    size_t count;
    size_t capacity;
};

static char *qtt_strdup(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static QttUsageExpr *usage_expr(QttUsageKind kind, QttUsageExpr *left,
                                QttUsageExpr *right) {
    QttUsageExpr *expr = calloc(1, sizeof(*expr));
    if (!expr) {
        free(left);
        free(right);
        return NULL;
    }
    expr->kind = kind;
    expr->left = left;
    expr->right = right;
    return expr;
}

static QttUsageExpr *usage_finite(uint64_t value) {
    QttUsageExpr *expr = usage_expr(QTT_USAGE_FINITE, NULL, NULL);
    if (expr) expr->finite = qtt_quantity_finite(value);
    return expr;
}

static void usage_free(QttUsageExpr *expr) {
    if (!expr) return;
    usage_free(expr->left);
    usage_free(expr->right);
    free(expr);
}

static bool usage_equal(const QttUsageExpr *left, const QttUsageExpr *right) {
    if (!left || !right || left->kind != right->kind) return left == right;
    if (left->kind == QTT_USAGE_FINITE)
        return qtt_quantity_equal(left->finite, right->finite);
    if (left->kind == QTT_USAGE_OMEGA) return true;
    return usage_equal(left->left, right->left) &&
           usage_equal(left->right, right->right);
}

static QttUsageExpr *usage_add(QttUsageExpr *left, QttUsageExpr *right) {
    if (!left || !right) {
        usage_free(left);
        usage_free(right);
        return NULL;
    }
    if (left->kind == QTT_USAGE_FINITE &&
        qtt_quantity_is_zero(left->finite)) {
        free(left);
        return right;
    }
    if (right->kind == QTT_USAGE_FINITE &&
        qtt_quantity_is_zero(right->finite)) {
        free(right);
        return left;
    }
    if (left->kind == QTT_USAGE_FINITE && right->kind == QTT_USAGE_FINITE) {
        QttQuantity sum = qtt_quantity_add(left->finite, right->finite);
        usage_free(left);
        usage_free(right);
        if (sum.is_omega) return usage_expr(QTT_USAGE_OMEGA, NULL, NULL);
        return usage_finite(sum.finite);
    }
    return usage_expr(QTT_USAGE_ADD, left, right);
}

static QttUsageExpr *usage_choice(QttUsageExpr *left, QttUsageExpr *right) {
    if (!left || !right) {
        usage_free(left);
        usage_free(right);
        return NULL;
    }
    if (usage_equal(left, right)) {
        usage_free(right);
        return left;
    }
    return usage_expr(QTT_USAGE_CHOICE, left, right);
}

static bool usage_is_zero(const QttUsageExpr *expr) {
    return expr && expr->kind == QTT_USAGE_FINITE &&
           qtt_quantity_is_zero(expr->finite);
}

static QttUsageExpr *usage_unary(QttUsageKind kind, QttUsageExpr *body) {
    if (!body) return NULL;
    if (usage_is_zero(body)) return body;
    return usage_expr(kind, body, NULL);
}

static bool list_head_is(const AST *ast, const char *name) {
    return ast && ast->type == AST_LIST && ast->list.count &&
           ast->list.items[0] &&
           ast->list.items[0]->type == AST_SYMBOL &&
           strcmp(ast->list.items[0]->symbol, name) == 0;
}

static QttUsageExpr *analyze(const AST *ast, uint64_t binder_id,
                             const char *fallback_name) {
    if (!ast) return usage_finite(0);

    switch (ast->type) {
    case AST_SYMBOL:
        return usage_finite(
            binder_id != QTT_BINDER_UNRESOLVED
                ? ast->resolved_binder_id == binder_id
                : strcmp(ast->symbol, fallback_name) == 0);

    case AST_LIST: {
        if (list_head_is(ast, "quote")) return usage_finite(0);
        if (list_head_is(ast, "if") && ast->list.count >= 3) {
            QttUsageExpr *condition =
                analyze(ast->list.items[1], binder_id, fallback_name);
            QttUsageExpr *then_usage =
                analyze(ast->list.items[2], binder_id, fallback_name);
            QttUsageExpr *else_usage = ast->list.count >= 4
                ? analyze(ast->list.items[3], binder_id, fallback_name)
                : usage_finite(0);
            return usage_add(condition,
                             usage_choice(then_usage, else_usage));
        }
        if (list_head_is(ast, "while") && ast->list.count >= 2) {
            QttUsageExpr *iteration = usage_finite(0);
            for (size_t i = 1; iteration && i < ast->list.count; i++)
                iteration = usage_add(iteration,
                                      analyze(ast->list.items[i], binder_id,
                                              fallback_name));
            return usage_unary(QTT_USAGE_REPEAT, iteration);
        }
        if ((list_head_is(ast, "with") || list_head_is(ast, "let")) &&
            ast->list.count >= 2 &&
            ast->list.items[1]->type == AST_ARRAY) {
            const AST *bindings = ast->list.items[1];
            QttUsageExpr *total = usage_finite(0);
            bool shadowed = false;
            for (size_t i = 0;
                 total && i + 1 < bindings->array.element_count;
                 i += 2) {
                if (!shadowed)
                    total = usage_add(
                        total,
                        analyze(bindings->array.elements[i + 1], binder_id,
                                fallback_name));
                const AST *name = bindings->array.elements[i];
                if (binder_id == QTT_BINDER_UNRESOLVED &&
                    name && name->type == AST_SYMBOL &&
                    strcmp(name->symbol, fallback_name) == 0)
                    shadowed = true;
            }
            if (!shadowed) {
                for (size_t i = 2; total && i < ast->list.count; i++)
                    total = usage_add(total,
                                      analyze(ast->list.items[i], binder_id,
                                              fallback_name));
            }
            return total;
        }
        QttUsageExpr *total = usage_finite(0);
        for (size_t i = 0; total && i < ast->list.count; i++)
            total = usage_add(total, analyze(ast->list.items[i], binder_id,
                                             fallback_name));
        return total;
    }

    case AST_LAMBDA: {
        for (int i = 0; i < ast->lambda.param_count; i++) {
            if (binder_id == QTT_BINDER_UNRESOLVED &&
                strcmp(ast->lambda.params[i].name, fallback_name) == 0)
                return usage_finite(0);
        }
        QttUsageExpr *total = usage_finite(0);
        for (int i = 0; total && i < ast->lambda.body_count; i++)
            total = usage_add(total,
                              analyze(ast->lambda.body_exprs[i], binder_id,
                                      fallback_name));
        if (usage_is_zero(total)) return total;
        usage_free(total);
        return usage_unary(QTT_USAGE_CAPTURE, usage_finite(1));
    }

    case AST_ARRAY: {
        QttUsageExpr *total = usage_finite(0);
        for (size_t i = 0; total && i < ast->array.element_count; i++)
            total = usage_add(total, analyze(ast->array.elements[i], binder_id,
                                             fallback_name));
        return total;
    }

    case AST_SET: {
        QttUsageExpr *total = usage_finite(0);
        for (size_t i = 0; total && i < ast->set.element_count; i++)
            total = usage_add(total, analyze(ast->set.elements[i], binder_id,
                                             fallback_name));
        return total;
    }

    case AST_MAP: {
        QttUsageExpr *total = usage_finite(0);
        for (size_t i = 0; total && i < ast->map.count; i++) {
            total = usage_add(total, analyze(ast->map.keys[i], binder_id,
                                             fallback_name));
            if (total)
                total = usage_add(total, analyze(ast->map.vals[i], binder_id,
                                                 fallback_name));
        }
        return total;
    }

    case AST_RANGE: {
        QttUsageExpr *total =
            analyze(ast->range.start, binder_id, fallback_name);
        if (ast->range.step)
            total = usage_add(total, analyze(ast->range.step, binder_id,
                                             fallback_name));
        if (ast->range.end)
            total = usage_add(total, analyze(ast->range.end, binder_id,
                                             fallback_name));
        return total;
    }

    case AST_ADDRESS_OF:
        return ast->list.count
            ? analyze(ast->list.items[0], binder_id, fallback_name)
                               : usage_finite(0);

    default:
        return usage_finite(0);
    }
}

static size_t format_expr(char *buffer, size_t capacity,
                          const QttUsageExpr *expr) {
    if (!capacity) return 0;
    if (!expr) return (size_t)snprintf(buffer, capacity, "?");
    if (expr->kind == QTT_USAGE_FINITE)
        return (size_t)snprintf(buffer, capacity, "%llu",
                                (unsigned long long)expr->finite.finite);
    if (expr->kind == QTT_USAGE_OMEGA)
        return (size_t)snprintf(buffer, capacity, "omega");

    if (expr->kind == QTT_USAGE_CAPTURE ||
        expr->kind == QTT_USAGE_REPEAT) {
        const char *operator_name =
            expr->kind == QTT_USAGE_CAPTURE ? "capture" : "repeat";
        size_t used = (size_t)snprintf(buffer, capacity, "%s(",
                                       operator_name);
        if (used >= capacity) return used;
        used += format_expr(buffer + used, capacity - used, expr->left);
        if (used >= capacity) return used;
        used += (size_t)snprintf(buffer + used, capacity - used, ")");
        return used;
    }

    const char *operator_name =
        expr->kind == QTT_USAGE_CHOICE ? "choice" : "add";
    size_t used = (size_t)snprintf(buffer, capacity, "%s(", operator_name);
    if (used >= capacity) return used;
    used += format_expr(buffer + used, capacity - used, expr->left);
    if (used >= capacity) return used;
    used += (size_t)snprintf(buffer + used, capacity - used, ",");
    if (used >= capacity) return used;
    used += format_expr(buffer + used, capacity - used, expr->right);
    if (used >= capacity) return used;
    used += (size_t)snprintf(buffer + used, capacity - used, ")");
    return used;
}

static bool report_append(QttUsageReport *report, const char *name,
                          uint64_t binder_id, QttUsageExpr *usage) {
    if (!report || !usage) {
        usage_free(usage);
        return false;
    }
    if (report->count == report->capacity) {
        size_t next_capacity = report->capacity ? report->capacity * 2 : 8;
        QttUsageEntry *next =
            realloc(report->entries, next_capacity * sizeof(*next));
        if (!next) {
            usage_free(usage);
            return false;
        }
        report->entries = next;
        report->capacity = next_capacity;
    }
    char formatted[512];
    format_expr(formatted, sizeof(formatted), usage);
    QttUsageEntry *entry = &report->entries[report->count++];
    entry->name = qtt_strdup(name);
    entry->binder_id = binder_id;
    entry->formatted = qtt_strdup(formatted);
    usage_free(usage);
    return entry->name && entry->formatted;
}

static void collect_local_binders(const AST *ast, QttUsageReport *report) {
    if (!ast || !report) return;

    if (ast->type == AST_LAMBDA) return;

    if (ast->type == AST_LIST &&
        (list_head_is(ast, "with") || list_head_is(ast, "let")) &&
        ast->list.count >= 2 && ast->list.items[1] &&
        ast->list.items[1]->type == AST_ARRAY) {
        const AST *bindings = ast->list.items[1];
        for (size_t i = 0; i + 1 < bindings->array.element_count; i += 2) {
            const AST *name = bindings->array.elements[i];
            const AST *value = bindings->array.elements[i + 1];
            collect_local_binders(value, report);
            if (!name || name->type != AST_SYMBOL ||
                name->resolved_binder_id == QTT_BINDER_UNRESOLVED)
                continue;

            QttUsageExpr *usage = usage_finite(0);
            for (size_t j = i + 2;
                 usage && j + 1 < bindings->array.element_count;
                 j += 2) {
                usage = usage_add(
                    usage,
                    analyze(bindings->array.elements[j + 1],
                            name->resolved_binder_id, name->symbol));
            }
            for (size_t body = 2; usage && body < ast->list.count; body++) {
                usage = usage_add(
                    usage,
                    analyze(ast->list.items[body],
                            name->resolved_binder_id, name->symbol));
            }
            report_append(report, name->symbol,
                          name->resolved_binder_id, usage);
        }
        for (size_t body = 2; body < ast->list.count; body++)
            collect_local_binders(ast->list.items[body], report);
        return;
    }

    switch (ast->type) {
    case AST_LIST:
        if (list_head_is(ast, "quote")) return;
        for (size_t i = 0; i < ast->list.count; i++)
            collect_local_binders(ast->list.items[i], report);
        break;
    case AST_ARRAY:
        for (size_t i = 0; i < ast->array.element_count; i++)
            collect_local_binders(ast->array.elements[i], report);
        break;
    case AST_SET:
        for (size_t i = 0; i < ast->set.element_count; i++)
            collect_local_binders(ast->set.elements[i], report);
        break;
    case AST_MAP:
        for (size_t i = 0; i < ast->map.count; i++) {
            collect_local_binders(ast->map.keys[i], report);
            collect_local_binders(ast->map.vals[i], report);
        }
        break;
    case AST_RANGE:
        collect_local_binders(ast->range.start, report);
        collect_local_binders(ast->range.step, report);
        collect_local_binders(ast->range.end, report);
        break;
    case AST_ADDRESS_OF:
        if (ast->list.count)
            collect_local_binders(ast->list.items[0], report);
        break;
    default:
        break;
    }
}

QttUsageReport *qtt_usage_analyze_lambda(const AST *lambda) {
    if (!lambda || lambda->type != AST_LAMBDA) return NULL;
    QttUsageReport *report = calloc(1, sizeof(*report));
    if (!report) return NULL;
    report->capacity = (size_t)lambda->lambda.param_count + 4;
    report->entries = calloc(report->capacity, sizeof(*report->entries));
    if (report->capacity && !report->entries) {
        free(report);
        return NULL;
    }

    for (int i = 0; i < lambda->lambda.param_count; i++) {
        const char *name = lambda->lambda.params[i].name;
        uint64_t binder_id = lambda->lambda.params[i].binder_id;
        QttUsageExpr *usage = usage_finite(0);
        for (int body = 0; usage && body < lambda->lambda.body_count; body++)
            usage = usage_add(usage,
                              analyze(lambda->lambda.body_exprs[body],
                                      binder_id, name));
        if (!report_append(report, name, binder_id, usage)) {
            qtt_usage_report_free(report);
            return NULL;
        }
    }
    for (int body = 0; body < lambda->lambda.body_count; body++)
        collect_local_binders(lambda->lambda.body_exprs[body], report);
    return report;
}

const char *qtt_usage_format(const QttUsageReport *report,
                             const char *binder_name) {
    if (!report || !binder_name) return NULL;
    for (size_t i = 0; i < report->count; i++) {
        if (strcmp(report->entries[i].name, binder_name) == 0)
            return report->entries[i].formatted;
    }
    return NULL;
}

size_t qtt_usage_report_count(const QttUsageReport *report) {
    return report ? report->count : 0;
}

const char *qtt_usage_report_name(const QttUsageReport *report, size_t index) {
    return report && index < report->count ? report->entries[index].name : NULL;
}

uint64_t qtt_usage_report_binder_id(const QttUsageReport *report,
                                    size_t index) {
    return report && index < report->count
        ? report->entries[index].binder_id
        : QTT_BINDER_UNRESOLVED;
}

const char *qtt_usage_report_format(const QttUsageReport *report,
                                    size_t index) {
    return report && index < report->count
        ? report->entries[index].formatted
        : NULL;
}

void qtt_usage_report_free(QttUsageReport *report) {
    if (!report) return;
    for (size_t i = 0; i < report->count; i++) {
        free(report->entries[i].name);
        free(report->entries[i].formatted);
    }
    free(report->entries);
    free(report);
}
