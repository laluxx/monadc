"""Differential semantics for computational Resource IR."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class QttRirTests(unittest.TestCase):
    def test_resource_ir_independently_preserves_core_values(self):
        source = r'''
#include "qtt/rir.h"
#include <assert.h>
#include <string.h>

int main(void) {
    Type string_type = {.kind = TYPE_STRING};
    AST payload_ast = {.type = AST_STRING, .string = "resource-value"};
    QttCoreNode payload = {
        .kind = QTT_CORE_LITERAL,
        .type = &string_type,
        .literal = {.source = &payload_ast},
    };
    QttCoreNode condition = {
        .kind = QTT_CORE_GLOBAL,
        .global = {.name = "False"},
    };
    QttCoreVar value_var = {.module_id = 1, .binder_id = 30};
    QttCoreNode use_value = {.kind = QTT_CORE_VAR, .var = value_var};
    QttCoreNode branch = {
        .kind = QTT_CORE_IF,
        .conditional = {
            .condition = &condition,
            .then_branch = &use_value,
            .else_branch = &use_value,
        },
    };
    QttCoreNode program = {
        .kind = QTT_CORE_LET,
        .let = {
            .binding = value_var,
            .value = &payload,
            .body = &branch,
        },
    };

    QttRirError lower_error = QTT_RIR_OK;
    QttRirProgram *resource = qtt_rir_lower(&program, &lower_error);
    assert(resource && lower_error == QTT_RIR_OK);
    assert(resource->root->kind == QTT_RIR_LET);
    assert(qtt_rir_erases_to_core(resource->root, &program));

    QttEvalResult core = qtt_core_evaluate(&program);
    QttRirEvaluation rir = qtt_rir_evaluate(resource);
    assert(core.error == QTT_EVAL_OK);
    assert(rir.error == QTT_RIR_EVAL_OK);
    assert(core.value.kind == QTT_VALUE_STRING);
    assert(rir.value.kind == QTT_VALUE_STRING);
    assert(strcmp(core.value.string, rir.value.string) == 0);
    assert(rir.branch_count == 1 && !rir.branches[0]);
    assert(rir.heap.error == QTT_RESOURCE_VALID);
    assert(rir.heap.allocated == 1);
    assert(rir.heap.moved_out == 1);
    assert(rir.heap.live_owned == 0);
    qtt_rir_evaluation_free(&rir);
    qtt_rir_program_free(resource);

    QttCoreNode missing = {
        .kind = QTT_CORE_VAR,
        .var = {.module_id = 1, .binder_id = 404},
    };
    resource = qtt_rir_lower(&missing, &lower_error);
    assert(resource);
    rir = qtt_rir_evaluate(resource);
    assert(rir.error == QTT_RIR_EVAL_UNBOUND_VAR);
    qtt_rir_evaluation_free(&rir);
    qtt_rir_program_free(resource);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_rir_test.c"
            executable = directory / "qtt_rir_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "core.c"),
                    str(ROOT / "qtt" / "resource.c"),
                    str(ROOT / "qtt" / "eval.c"),
                    str(ROOT / "qtt" / "rir.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT,
                check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
