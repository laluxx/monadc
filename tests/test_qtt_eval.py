"""Executable semantics and certified resource evaluation."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class QttEvalTests(unittest.TestCase):
    def test_core_values_and_resource_certificate_agree(self):
        source = r'''
#include "qtt/eval.h"
#include <assert.h>
#include <string.h>

static QttCoreNode literal(AST *source, Type *type) {
    QttCoreNode node = {.kind = QTT_CORE_LITERAL, .type = type};
    node.literal.source = source;
    return node;
}

int main(void) {
    Type integer = {.kind = TYPE_INT};
    Type string_type = {.kind = TYPE_STRING};
    AST forty_two_ast = {.type = AST_NUMBER, .number = 42};
    AST text_ast = {.type = AST_STRING, .string = "owned"};
    QttCoreNode forty_two = literal(&forty_two_ast, &integer);
    QttCoreNode text = literal(&text_ast, &string_type);
    QttCoreNode truth = {
        .kind = QTT_CORE_GLOBAL,
        .global = {.name = "True"},
    };
    QttCoreNode falsehood = {
        .kind = QTT_CORE_GLOBAL,
        .global = {.name = "False"},
    };

    QttCoreVar x = {.module_id = 1, .binder_id = 1};
    QttCoreNode use_x = {.kind = QTT_CORE_VAR, .var = x};
    QttCoreNode choose_x = {
        .kind = QTT_CORE_IF,
        .conditional = {
            .condition = &truth,
            .then_branch = &use_x,
            .else_branch = &use_x,
        },
    };
    QttCoreNode integer_program = {
        .kind = QTT_CORE_LET,
        .let = {.binding = x, .value = &forty_two, .body = &choose_x},
    };
    QttCertifiedEvaluation evaluated =
        qtt_resource_evaluate(&integer_program);
    assert(evaluated.eval_error == QTT_EVAL_OK);
    assert(evaluated.resource_error == QTT_RESOURCE_VALID);
    assert(evaluated.value.kind == QTT_VALUE_NUMBER);
    assert(evaluated.value.number == 42);
    assert(evaluated.branch_count == 1 && evaluated.branches[0]);
    assert(evaluated.heap.live_owned == 0);
    assert(evaluated.heap.allocated == 0);
    qtt_certified_evaluation_free(&evaluated);

    QttCoreVar s = {.module_id = 1, .binder_id = 2};
    QttCoreNode use_s = {.kind = QTT_CORE_VAR, .var = s};
    QttCoreNode choose_s = {
        .kind = QTT_CORE_IF,
        .conditional = {
            .condition = &falsehood,
            .then_branch = &use_s,
            .else_branch = &use_s,
        },
    };
    QttCoreNode string_program = {
        .kind = QTT_CORE_LET,
        .let = {.binding = s, .value = &text, .body = &choose_s},
    };
    evaluated = qtt_resource_evaluate(&string_program);
    assert(evaluated.eval_error == QTT_EVAL_OK);
    assert(evaluated.resource_error == QTT_RESOURCE_VALID);
    assert(evaluated.value.kind == QTT_VALUE_STRING);
    assert(strcmp(evaluated.value.string, "owned") == 0);
    assert(evaluated.branch_count == 1 && !evaluated.branches[0]);
    assert(evaluated.heap.allocated == 1);
    assert(evaluated.heap.moved_out == 1);
    assert(evaluated.heap.live_owned == 0);
    qtt_certified_evaluation_free(&evaluated);

    QttCoreNode missing = {
        .kind = QTT_CORE_VAR,
        .var = {.module_id = 1, .binder_id = 404},
    };
    QttEvalResult bad = qtt_core_evaluate(&missing);
    assert(bad.error == QTT_EVAL_UNBOUND_VAR);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_eval_test.c"
            executable = directory / "qtt_eval_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "core.c"),
                    str(ROOT / "qtt" / "resource.c"),
                    str(ROOT / "qtt" / "eval.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT,
                check=True,
            )
            env = os.environ.copy()
            # LeakSanitizer cannot inspect processes under the test harness'
            # ptrace environment; AddressSanitizer and UBSan remain active.
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
