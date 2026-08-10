"""Representation-aware execution of verified Resource IR."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class QttHeapTests(unittest.TestCase):
    def test_representation_classes_and_zero_live_heap(self):
        source = r'''
#include "qtt/resource.h"
#include <assert.h>

static QttCoreVar var(uint64_t id) {
    return (QttCoreVar){.module_id = 1, .binder_id = id};
}

int main(void) {
    Type integer = {.kind = TYPE_INT};
    Type string = {.kind = TYPE_STRING};
    Type pointer = {.kind = TYPE_PTR};
    Type fixed_array = {.kind = TYPE_ARR, .arr_is_heap = false};
    Type heap_array = {.kind = TYPE_ARR, .arr_is_heap = true};
    Type unknown = {.kind = TYPE_UNKNOWN};
    assert(qtt_resource_classify_type(&integer) == QTT_REP_IMMEDIATE);
    assert(qtt_resource_classify_type(&string) == QTT_REP_OWNED_HEAP);
    assert(qtt_resource_classify_type(&pointer) == QTT_REP_FOREIGN);
    assert(qtt_resource_classify_type(&fixed_array) == QTT_REP_INLINE);
    assert(qtt_resource_classify_type(&heap_array) == QTT_REP_OWNED_HEAP);
    assert(qtt_resource_classify_type(&unknown) == QTT_REP_UNKNOWN);

    QttResourceOp then_ops[] = {qtt_resource_drop(var(1))};
    QttResourceOp else_ops[] = {qtt_resource_drop(var(1))};
    QttResourceBlock then_block = {then_ops, 1};
    QttResourceBlock else_block = {else_ops, 1};
    QttResourceOp ops[] = {
        qtt_resource_alloc_typed(var(1), QTT_REP_OWNED_HEAP),
        qtt_resource_borrow(var(1)),
        qtt_resource_branch(&then_block, &else_block),
    };
    QttResourceBlock program = {ops, 3};
    bool choose_then[] = {true};
    bool choose_else[] = {false};
    QttHeapExecution first =
        qtt_resource_execute(&program, choose_then, 1);
    QttHeapExecution second =
        qtt_resource_execute(&program, choose_else, 1);
    assert(first.error == QTT_RESOURCE_VALID);
    assert(second.error == QTT_RESOURCE_VALID);
    assert(first.allocated == 1 && first.dropped == 1);
    assert(second.allocated == 1 && second.dropped == 1);
    assert(first.live_owned == 0 && second.live_owned == 0);
    assert(first.peak_live_owned == 1 && second.peak_live_owned == 1);
    assert(first.borrows == 1 && second.borrows == 1);

    QttResourceOp immediate_ops[] = {
        qtt_resource_alloc_typed(var(2), QTT_REP_IMMEDIATE),
        qtt_resource_drop(var(2)),
    };
    QttResourceBlock immediate_program = {immediate_ops, 2};
    assert(qtt_resource_verify(&immediate_program, NULL, 0).error ==
           QTT_RESOURCE_INVALID_CAPABILITY);

    QttResourceOp inline_ops[] = {
        qtt_resource_alloc_typed(var(4), QTT_REP_INLINE)};
    QttResourceBlock inline_program = {inline_ops, 1};
    assert(qtt_resource_verify(&inline_program, NULL, 0).error ==
           QTT_RESOURCE_INVALID_CAPABILITY);

    QttResourceOp foreign_ops[] = {
        qtt_resource_alloc_typed(var(5), QTT_REP_FOREIGN)};
    QttResourceBlock foreign_program = {foreign_ops, 1};
    assert(qtt_resource_verify(&foreign_program, NULL, 0).error ==
           QTT_RESOURCE_INVALID_CAPABILITY);

    QttResourceOp shared_ops[] = {
        qtt_resource_alloc_typed(var(7), QTT_REP_OWNED_HEAP),
        qtt_resource_dup(var(7), var(8)),
        qtt_resource_drop(var(7)),
        qtt_resource_drop(var(8)),
    };
    QttResourceBlock shared_program = {shared_ops, 4};
    QttHeapExecution shared_run =
        qtt_resource_execute(&shared_program, NULL, 0);
    assert(shared_run.error == QTT_RESOURCE_VALID);
    assert(shared_run.allocated == 1);
    assert(shared_run.retains == 1);
    assert(shared_run.releases == 1);
    assert(shared_run.dropped == 1);
    assert(shared_run.live_owned == 0);

    QttCoreNode string_value = {
        .kind = QTT_CORE_LITERAL,
        .type = &string,
    };
    QttCoreNode unit_value = {.kind = QTT_CORE_LITERAL};
    QttCoreNode lexical = {
        .kind = QTT_CORE_LET,
        .let = {
            .binding = {.module_id = 1, .binder_id = 3},
            .value = &string_value,
            .body = &unit_value,
        },
    };
    QttResourceElaborationError error = QTT_RESOURCE_ELABORATE_OK;
    QttResourceBlock *elaborated =
        qtt_resource_elaborate_lexical(&lexical, &error);
    assert(elaborated && elaborated->ops[0].representation ==
           QTT_REP_OWNED_HEAP);
    QttHeapExecution lexical_run =
        qtt_resource_execute(elaborated, NULL, 0);
    assert(lexical_run.error == QTT_RESOURCE_VALID);
    assert(lexical_run.allocated == 1);
    assert(lexical_run.dropped == 1);
    assert(lexical_run.live_owned == 0);
    qtt_resource_block_free(elaborated);

    QttCoreVar scalar_var = {.module_id = 1, .binder_id = 40};
    QttCoreNode integer_value = {
        .kind = QTT_CORE_LITERAL, .type = &integer};
    QttCoreNode scalar_use = {
        .kind = QTT_CORE_VAR, .type = &integer, .var = scalar_var};
    QttCoreNode scalar_let = {
        .kind = QTT_CORE_LET, .type = &integer,
        .let = {.binding = scalar_var, .value = &integer_value,
                .body = &scalar_use}};
    elaborated = qtt_resource_elaborate_lexical(
        &scalar_let, &error);
    assert(elaborated && error == QTT_RESOURCE_ELABORATE_OK);
    assert(elaborated->count == 0);
    assert(qtt_resource_verify(elaborated, NULL, 0).error ==
           QTT_RESOURCE_VALID);
    qtt_resource_block_free(elaborated);

    QttCoreVar foreign_var = {.module_id = 1, .binder_id = 41};
    QttCoreNode foreign_value = {
        .kind = QTT_CORE_LITERAL, .type = &pointer};
    QttCoreNode foreign_use = {
        .kind = QTT_CORE_VAR, .type = &pointer, .var = foreign_var};
    QttCoreNode foreign_let = {
        .kind = QTT_CORE_LET, .type = &pointer,
        .let = {.binding = foreign_var, .value = &foreign_value,
                .body = &foreign_use}};
    elaborated = qtt_resource_elaborate_lexical(
        &foreign_let, &error);
    assert(elaborated && elaborated->count == 0);
    qtt_resource_block_free(elaborated);

    QttCoreNode unknown_value = {
        .kind = QTT_CORE_LITERAL, .type = &unknown};
    scalar_let.let.value = &unknown_value;
    assert(!qtt_resource_elaborate_lexical(
        &scalar_let, &error));
    assert(error ==
           QTT_RESOURCE_ELABORATE_INVALID_RESOURCE_STATE);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_heap_test.c"
            executable = directory / "qtt_heap_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "core.c"),
                    str(ROOT / "qtt" / "resource.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
