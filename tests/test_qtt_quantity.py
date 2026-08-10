"""Regression contract for exact QTT quantity arithmetic.

The compiler keeps exact finite usage counts and widens to omega only when a
count is genuinely unbounded or cannot be represented safely.
"""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class QttQuantityTests(unittest.TestCase):
    def test_exact_finite_arithmetic_and_omega(self):
        source = r'''
#include "qtt/quantity.h"
#include <assert.h>
#include <stdint.h>

int main(void) {
    QttQuantity zero = qtt_quantity_finite(0);
    QttQuantity one = qtt_quantity_finite(1);
    QttQuantity seven = qtt_quantity_finite(7);
    QttQuantity nine = qtt_quantity_finite(9);
    QttQuantity omega = qtt_quantity_omega();

    assert(qtt_quantity_is_zero(zero));
    assert(qtt_quantity_equal(qtt_quantity_add(seven, nine),
                              qtt_quantity_finite(16)));
    assert(qtt_quantity_equal(qtt_quantity_multiply(seven, nine),
                              qtt_quantity_finite(63)));
    assert(qtt_quantity_equal(qtt_quantity_add(seven, omega), omega));
    assert(qtt_quantity_equal(qtt_quantity_multiply(zero, omega), zero));
    assert(qtt_quantity_equal(qtt_quantity_multiply(one, omega), omega));
    assert(qtt_quantity_equal(
        qtt_quantity_add(qtt_quantity_finite(UINT64_MAX), one), omega));
    assert(qtt_quantity_equal(
        qtt_quantity_multiply(qtt_quantity_finite(UINT64_MAX),
                              qtt_quantity_finite(2)),
        omega));

    QttQuantity values[] = {
        qtt_quantity_finite(0),
        qtt_quantity_finite(1),
        qtt_quantity_finite(2),
        qtt_quantity_finite(7),
        omega,
    };
    const int count = (int)(sizeof(values) / sizeof(values[0]));
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < count; j++) {
            assert(qtt_quantity_equal(
                qtt_quantity_add(values[i], values[j]),
                qtt_quantity_add(values[j], values[i])));
            assert(qtt_quantity_equal(
                qtt_quantity_multiply(values[i], values[j]),
                qtt_quantity_multiply(values[j], values[i])));
            for (int k = 0; k < count; k++) {
                assert(qtt_quantity_equal(
                    qtt_quantity_add(
                        qtt_quantity_add(values[i], values[j]), values[k]),
                    qtt_quantity_add(
                        values[i], qtt_quantity_add(values[j], values[k]))));
                assert(qtt_quantity_equal(
                    qtt_quantity_multiply(
                        qtt_quantity_multiply(values[i], values[j]), values[k]),
                    qtt_quantity_multiply(
                        values[i],
                        qtt_quantity_multiply(values[j], values[k]))));
                assert(qtt_quantity_equal(
                    qtt_quantity_multiply(
                        values[i], qtt_quantity_add(values[j], values[k])),
                    qtt_quantity_add(
                        qtt_quantity_multiply(values[i], values[j]),
                        qtt_quantity_multiply(values[i], values[k]))));
            }
        }
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_quantity_test.c"
            executable = directory / "qtt_quantity_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc",
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-iquote",
                    str(ROOT),
                    str(harness),
                    str(ROOT / "qtt" / "quantity.c"),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable)], check=True, cwd=ROOT)

    def test_symbolic_path_sensitive_usage(self):
        source = r'''
#include "qtt/usage.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static AST symbol(char *name) {
    AST ast = {0};
    ast.type = AST_SYMBOL;
    ast.symbol = name;
    return ast;
}

static AST list(AST **items, size_t count) {
    AST ast = {0};
    ast.type = AST_LIST;
    ast.list.items = items;
    ast.list.count = count;
    return ast;
}

int main(void) {
    AST plus = symbol("+");
    AST if_symbol = symbol("if");
    AST x1 = symbol("x");
    AST x2 = symbol("x");
    AST x3 = symbol("x");
    AST c = symbol("c");
    AST *sum_items[] = {&plus, &x2, &x3};
    AST sum = list(sum_items, 3);
    AST *if_items[] = {&if_symbol, &c, &x1, &sum};
    AST conditional = list(if_items, 4);

    ASTParam params[] = {
        {.name = "x"},
        {.name = "c"},
        {.name = "unused"},
    };
    AST *body[] = {&conditional};
    AST lambda = {0};
    lambda.type = AST_LAMBDA;
    lambda.lambda.params = params;
    lambda.lambda.param_count = 3;
    lambda.lambda.body_exprs = body;
    lambda.lambda.body_count = 1;

    QttUsageReport *report = qtt_usage_analyze_lambda(&lambda);
    assert(report);
    assert(strcmp(qtt_usage_format(report, "x"), "choice(1,2)") == 0);
    assert(strcmp(qtt_usage_format(report, "c"), "1") == 0);
    assert(strcmp(qtt_usage_format(report, "unused"), "0") == 0);
    qtt_usage_report_free(report);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_usage_test.c"
            executable = directory / "qtt_usage_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc",
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-iquote",
                    str(ROOT),
                    str(harness),
                    str(ROOT / "qtt" / "quantity.c"),
                    str(ROOT / "qtt" / "usage.c"),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable)], check=True, cwd=ROOT)

    def test_capture_loop_and_lexical_shadowing_are_distinct(self):
        source = r'''
#include "qtt/usage.h"
#include <assert.h>
#include <string.h>

static AST symbol(char *name) {
    AST ast = {0};
    ast.type = AST_SYMBOL;
    ast.symbol = name;
    return ast;
}
static AST list(AST **items, size_t count) {
    AST ast = {0};
    ast.type = AST_LIST;
    ast.list.items = items;
    ast.list.count = count;
    return ast;
}
static AST lambda1(char *name, AST *expression, ASTParam *storage,
                   AST **body_storage) {
    AST ast = {0};
    storage[0].name = name;
    body_storage[0] = expression;
    ast.type = AST_LAMBDA;
    ast.lambda.params = storage;
    ast.lambda.param_count = 1;
    ast.lambda.body_exprs = body_storage;
    ast.lambda.body_count = 1;
    return ast;
}

int main(void) {
    AST plus = symbol("+");
    AST x1 = symbol("x");
    AST x2 = symbol("x");
    AST *sum_items[] = {&plus, &x1, &x2};
    AST sum = list(sum_items, 3);

    ASTParam inner_params[1] = {0};
    AST *inner_body[1];
    AST inner = lambda1("y", &sum, inner_params, inner_body);
    ASTParam outer_params[1] = {0};
    AST *outer_body[1];
    AST outer = lambda1("x", &inner, outer_params, outer_body);
    QttUsageReport *capture = qtt_usage_analyze_lambda(&outer);
    assert(strcmp(qtt_usage_format(capture, "x"), "capture(1)") == 0);
    qtt_usage_report_free(capture);

    AST while_symbol = symbol("while");
    AST loop_x1 = symbol("x");
    AST loop_x2 = symbol("x");
    AST *while_items[] = {&while_symbol, &loop_x1, &loop_x2};
    AST loop = list(while_items, 3);
    ASTParam loop_params[1] = {0};
    AST *loop_body[1];
    AST loop_lambda = lambda1("x", &loop, loop_params, loop_body);
    QttUsageReport *repeated = qtt_usage_analyze_lambda(&loop_lambda);
    assert(strcmp(qtt_usage_format(repeated, "x"), "repeat(2)") == 0);
    qtt_usage_report_free(repeated);

    AST with_symbol = symbol("with");
    AST local_x = symbol("x");
    AST initializer_x = symbol("x");
    AST body_x = symbol("x");
    AST *bindings_elements[] = {&local_x, &initializer_x};
    AST bindings = {0};
    bindings.type = AST_ARRAY;
    bindings.array.elements = bindings_elements;
    bindings.array.element_count = 2;
    AST *with_items[] = {&with_symbol, &bindings, &body_x};
    AST with = list(with_items, 3);
    ASTParam with_params[1] = {0};
    AST *with_body[1];
    AST with_lambda = lambda1("x", &with, with_params, with_body);
    QttUsageReport *shadowed = qtt_usage_analyze_lambda(&with_lambda);
    assert(strcmp(qtt_usage_format(shadowed, "x"), "1") == 0);
    qtt_usage_report_free(shadowed);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_scope_test.c"
            executable = directory / "qtt_scope_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "quantity.c"),
                    str(ROOT / "qtt" / "usage.c"),
                    "-o", str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
