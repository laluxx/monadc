"""Executable contracts for the linear Resource IR checker."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class QttResourceTests(unittest.TestCase):
    def test_resource_state_machine_and_automatic_lexical_drop(self):
        source = r'''
#include "qtt/resource.h"
#include <assert.h>
#include <string.h>

static QttCoreVar var(uint64_t binder) {
    return (QttCoreVar){.module_id = 1, .binder_id = binder};
}

int main(void) {
    QttResourceOp valid_ops[] = {
        qtt_resource_alloc(var(1)),
        qtt_resource_borrow(var(1)),
        qtt_resource_drop(var(1)),
    };
    QttResourceBlock valid = {valid_ops, 3};
    assert(qtt_resource_verify(&valid, NULL, 0).error == QTT_RESOURCE_VALID);

    QttResourceOp double_ops[] = {
        qtt_resource_alloc(var(1)),
        qtt_resource_drop(var(1)),
        qtt_resource_drop(var(1)),
    };
    QttResourceBlock double_drop = {double_ops, 3};
    assert(qtt_resource_verify(&double_drop, NULL, 0).error ==
           QTT_RESOURCE_DOUBLE_CONSUME);

    QttResourceOp use_after_ops[] = {
        qtt_resource_alloc(var(1)),
        qtt_resource_move(var(1)),
        qtt_resource_borrow(var(1)),
    };
    QttResourceBlock use_after = {use_after_ops, 3};
    assert(qtt_resource_verify(&use_after, NULL, 0).error ==
           QTT_RESOURCE_USE_AFTER_CONSUME);

    QttResourceOp leak_ops[] = {qtt_resource_alloc(var(1))};
    QttResourceBlock leak = {leak_ops, 1};
    assert(qtt_resource_verify(&leak, NULL, 0).error == QTT_RESOURCE_LEAK);

    QttResourceOp shared_ops[] = {
        qtt_resource_alloc(var(20)),
        qtt_resource_dup(var(20), var(21)),
        qtt_resource_drop(var(20)),
        qtt_resource_drop(var(21)),
    };
    QttResourceBlock shared_block = {shared_ops, 4};
    assert(qtt_resource_verify(&shared_block, NULL, 0).error ==
           QTT_RESOURCE_VALID);

    QttResourceOp rehome_ops[] = {
        qtt_resource_alloc(var(30)),
        qtt_resource_rehome(var(30), var(31)),
        qtt_resource_drop(var(31)),
    };
    QttResourceBlock rehome_block = {rehome_ops, 3};
    assert(qtt_resource_verify(&rehome_block, NULL, 0).error ==
           QTT_RESOURCE_VALID);
    QttHeapExecution rehome_execution =
        qtt_resource_execute(&rehome_block, NULL, 0);
    assert(rehome_execution.error == QTT_RESOURCE_VALID);
    assert(rehome_execution.allocated == 1);
    assert(rehome_execution.rehomes == 1);
    assert(rehome_execution.retains == 0);
    assert(rehome_execution.releases == 0);
    assert(rehome_execution.dropped == 1);
    assert(rehome_execution.live_owned == 0);

    rehome_ops[1] = qtt_resource_rehome(var(30), var(30));
    assert(qtt_resource_verify(&rehome_block, NULL, 0).error ==
           QTT_RESOURCE_DUPLICATE_ALLOC);

    QttResourceOp then_ops[] = {qtt_resource_drop(var(1))};
    QttResourceOp else_ops[] = {qtt_resource_drop(var(1))};
    QttResourceBlock then_block = {then_ops, 1};
    QttResourceBlock else_block = {else_ops, 1};
    QttResourceOp branch_ops[] = {
        qtt_resource_alloc(var(1)),
        qtt_resource_branch(&then_block, &else_block),
    };
    QttResourceBlock balanced = {branch_ops, 2};
    assert(qtt_resource_verify(&balanced, NULL, 0).error ==
           QTT_RESOURCE_VALID);

    QttResourceBlock empty = {0};
    branch_ops[1] = qtt_resource_branch(&then_block, &empty);
    assert(qtt_resource_verify(&balanced, NULL, 0).error ==
           QTT_RESOURCE_BRANCH_MISMATCH);

    Type string_type = {.kind = TYPE_STRING};
    QttCoreNode literal = {
        .kind = QTT_CORE_LITERAL,
        .type = &string_type,
    };
    QttCoreNode unused = {
        .kind = QTT_CORE_LET,
        .let = {
            .binding = {.module_id = 1, .binder_id = 9},
            .value = &literal,
            .body = &literal,
        },
    };
    QttResourceElaborationError elaboration_error = QTT_RESOURCE_ELABORATE_OK;
    QttResourceBlock *automatic =
        qtt_resource_elaborate_lexical(&unused, &elaboration_error);
    assert(automatic && elaboration_error == QTT_RESOURCE_ELABORATE_OK);
    assert(automatic->count == 2);
    assert(automatic->ops[0].kind == QTT_RESOURCE_ALLOC);
    assert(automatic->ops[1].kind == QTT_RESOURCE_DROP);
    assert(qtt_resource_verify(automatic, NULL, 0).error ==
           QTT_RESOURCE_VALID);
    assert(qtt_resource_verify_elaboration(
               automatic, &unused).error == QTT_RESOURCE_VALID);
    automatic->ops[1] = qtt_resource_move(var(9));
    /* Linear balance alone cannot distinguish return from destruction. */
    assert(qtt_resource_verify(automatic, NULL, 0).error ==
           QTT_RESOURCE_VALID);
    assert(qtt_resource_verify_elaboration(
               automatic, &unused).error ==
           QTT_RESOURCE_PLAN_MISMATCH);
    qtt_resource_block_free(automatic);

    QttCoreNode condition = {.kind = QTT_CORE_GLOBAL};
    QttCoreNode conditional = {
        .kind = QTT_CORE_IF,
        .conditional = {
            .condition = &condition,
            .then_branch = &literal,
            .else_branch = &literal,
        },
    };
    QttCoreNode conditional_let = {
        .kind = QTT_CORE_LET,
        .let = {
            .binding = {.module_id = 1, .binder_id = 11},
            .value = &literal,
            .body = &conditional,
        },
    };
    automatic = qtt_resource_elaborate_lexical(
        &conditional_let, &elaboration_error);
    assert(automatic && automatic->count == 3);
    assert(automatic->ops[0].kind == QTT_RESOURCE_ALLOC);
    assert(automatic->ops[1].kind == QTT_RESOURCE_BRANCH);
    assert(automatic->ops[2].kind == QTT_RESOURCE_DROP);
    assert(qtt_resource_verify(automatic, NULL, 0).error ==
           QTT_RESOURCE_VALID);
    qtt_resource_block_free(automatic);

    QttCoreNode asymmetric_var = {
        .kind = QTT_CORE_VAR,
        .var = {.module_id = 1, .binder_id = 10},
    };
    QttCoreNode asymmetric = {
        .kind = QTT_CORE_IF,
        .conditional = {
            .condition = &condition,
            .then_branch = &asymmetric_var,
            .else_branch = &literal,
        },
    };
    QttCoreNode asymmetric_let = {
        .kind = QTT_CORE_LET,
        .let = {
            .binding = {.module_id = 1, .binder_id = 10},
            .value = &literal,
            .body = &asymmetric,
        },
    };
    automatic = qtt_resource_elaborate_lexical(
        &asymmetric_let, &elaboration_error);
    assert(!automatic);
    assert(elaboration_error ==
           QTT_RESOURCE_ELABORATE_UNBALANCED_CONTROL_FLOW);

    QttCoreNode returned_var = {
        .kind = QTT_CORE_VAR,
        .var = {.module_id = 1, .binder_id = 10},
    };
    QttCoreNode returned = {
        .kind = QTT_CORE_LET,
        .let = {
            .binding = {.module_id = 1, .binder_id = 10},
            .value = &literal,
            .body = &returned_var,
        },
    };
    automatic = qtt_resource_elaborate_lexical(
        &returned, &elaboration_error);
    assert(automatic && automatic->count == 2);
    assert(automatic->ops[0].kind == QTT_RESOURCE_ALLOC);
    assert(automatic->ops[1].kind == QTT_RESOURCE_MOVE);
    assert(qtt_resource_verify(automatic, NULL, 0).error ==
           QTT_RESOURCE_VALID);
    qtt_resource_block_free(automatic);

    QttCoreNode owner_ref = {
        .kind = QTT_CORE_VAR,
        .type = &string_type,
        .var = {.module_id = 1, .binder_id = 40},
    };
    QttCoreNode alias_ref = {
        .kind = QTT_CORE_VAR,
        .type = &string_type,
        .var = {.module_id = 1, .binder_id = 41},
    };
    QttCoreNode alias_unused = {
        .kind = QTT_CORE_LET,
        .type = &string_type,
        .let = {
            .binding = {.module_id = 1, .binder_id = 41},
            .value = &owner_ref,
            .body = &literal,
        },
    };
    QttCoreNode owner_with_alias = {
        .kind = QTT_CORE_LET,
        .type = &string_type,
        .let = {
            .binding = {.module_id = 1, .binder_id = 40},
            .value = &literal,
            .body = &alias_unused,
        },
    };
    automatic = qtt_resource_elaborate_lexical(
        &owner_with_alias, &elaboration_error);
    assert(automatic && automatic->count == 5);
    assert(automatic->ops[0].kind == QTT_RESOURCE_ALLOC);
    assert(qtt_core_var_equal(automatic->ops[0].var, var(40)));
    assert(automatic->ops[1].kind == QTT_RESOURCE_BORROW);
    assert(qtt_core_var_equal(automatic->ops[1].var, var(40)));
    assert(automatic->ops[2].kind == QTT_RESOURCE_ALIAS);
    assert(qtt_core_var_equal(automatic->ops[2].var, var(41)));
    assert(qtt_core_var_equal(automatic->ops[2].target, var(40)));
    assert(automatic->ops[3].kind == QTT_RESOURCE_END_ALIAS);
    assert(qtt_core_var_equal(automatic->ops[3].var, var(41)));
    assert(automatic->ops[4].kind == QTT_RESOURCE_DROP);
    assert(qtt_core_var_equal(automatic->ops[4].var, var(40)));
    assert(qtt_resource_verify_elaboration(
               automatic, &owner_with_alias).error ==
           QTT_RESOURCE_VALID);
    qtt_resource_block_free(automatic);

    alias_unused.let.body = &alias_ref;
    automatic = qtt_resource_elaborate_lexical(
        &owner_with_alias, &elaboration_error);
    assert(automatic && automatic->count == 5);
    assert(automatic->ops[0].kind == QTT_RESOURCE_ALLOC);
    assert(automatic->ops[1].kind == QTT_RESOURCE_BORROW);
    assert(automatic->ops[2].kind == QTT_RESOURCE_ALIAS);
    assert(automatic->ops[3].kind == QTT_RESOURCE_MOVE_ALIAS);
    assert(qtt_core_var_equal(automatic->ops[3].var, var(41)));
    assert(qtt_core_var_equal(automatic->ops[3].target, var(40)));
    assert(automatic->ops[4].kind == QTT_RESOURCE_END_ALIAS);
    assert(qtt_resource_verify_elaboration(
               automatic, &owner_with_alias).error ==
           QTT_RESOURCE_VALID);
    qtt_resource_block_free(automatic);

    QttResourceOp invalid_alias_ops[] = {
        qtt_resource_alias(var(41), var(40)),
    };
    QttResourceBlock invalid_alias = {invalid_alias_ops, 1};
    assert(qtt_resource_verify(&invalid_alias, NULL, 0).error ==
           QTT_RESOURCE_UNKNOWN_VAR);

    QttResourceOp duplicate_alias_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias(var(41), var(40)),
        qtt_resource_alias(var(41), var(40)),
        qtt_resource_end_alias(var(41), var(40)),
        qtt_resource_drop(var(40)),
    };
    QttResourceBlock duplicate_alias = {duplicate_alias_ops, 5};
    assert(qtt_resource_verify(&duplicate_alias, NULL, 0).error ==
           QTT_RESOURCE_DUPLICATE_ALIAS);

    QttResourceOp wrong_end_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias(var(41), var(40)),
        qtt_resource_end_alias(var(41), var(42)),
        qtt_resource_drop(var(40)),
    };
    QttResourceBlock wrong_end = {wrong_end_ops, 4};
    assert(qtt_resource_verify(&wrong_end, NULL, 0).error ==
           QTT_RESOURCE_ALIAS_TARGET_MISMATCH);

    QttResourceOp open_alias_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias(var(41), var(40)),
        qtt_resource_drop(var(40)),
    };
    QttResourceBlock open_alias = {open_alias_ops, 3};
    assert(qtt_resource_verify(&open_alias, NULL, 0).error ==
           QTT_RESOURCE_ACCESS_CONFLICT);

    open_alias_ops[2] =
        qtt_resource_move_alias(var(41), var(40));
    assert(qtt_resource_verify(&open_alias, NULL, 0).error ==
           QTT_RESOURCE_UNKNOWN_ALIAS);

    QttResourceOp branch_alias_then_ops[] = {
        qtt_resource_alias(var(41), var(40)),
    };
    QttResourceBlock branch_alias_then = {
        branch_alias_then_ops, 1};
    QttResourceBlock branch_alias_else = {0};
    QttResourceOp branch_alias_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_branch(
            &branch_alias_then, &branch_alias_else),
        qtt_resource_drop(var(40)),
    };
    QttResourceBlock branch_alias = {branch_alias_ops, 3};
    assert(qtt_resource_verify(&branch_alias, NULL, 0).error ==
           QTT_RESOURCE_BRANCH_MISMATCH);

    QttResourceOp two_shared_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias(var(41), var(40)),
        qtt_resource_alias(var(42), var(40)),
        qtt_resource_end_alias(var(42), var(40)),
        qtt_resource_end_alias(var(41), var(40)),
        qtt_resource_drop(var(40)),
    };
    QttResourceBlock two_shared = {two_shared_ops, 6};
    assert(qtt_resource_verify(&two_shared, NULL, 0).error ==
           QTT_RESOURCE_VALID);

    QttResourceOp shared_then_exclusive_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias(var(41), var(40)),
        qtt_resource_alias_exclusive(var(42), var(40)),
        qtt_resource_end_alias_exclusive(var(41), var(40)),
        qtt_resource_drop(var(40)),
    };
    QttResourceBlock shared_then_exclusive = {
        shared_then_exclusive_ops, 5};
    assert(qtt_resource_verify(
               &shared_then_exclusive, NULL, 0).error ==
           QTT_RESOURCE_LOAN_CONFLICT);

    QttResourceOp exclusive_then_shared_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias_exclusive(var(41), var(40)),
        qtt_resource_alias(var(42), var(40)),
        qtt_resource_end_alias_exclusive(var(41), var(40)),
        qtt_resource_drop(var(40)),
    };
    QttResourceBlock exclusive_then_shared = {
        exclusive_then_shared_ops, 5};
    assert(qtt_resource_verify(
               &exclusive_then_shared, NULL, 0).error ==
           QTT_RESOURCE_LOAN_CONFLICT);

    QttResourceOp read_during_exclusive_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias_exclusive(var(41), var(40)),
        qtt_resource_borrow(var(40)),
        qtt_resource_end_alias(var(41), var(40)),
        qtt_resource_drop(var(40)),
    };
    QttResourceBlock read_during_exclusive = {
        read_during_exclusive_ops, 5};
    assert(qtt_resource_verify(
               &read_during_exclusive, NULL, 0).error ==
           QTT_RESOURCE_ACCESS_CONFLICT);

    QttResourceOp exclusive_lifecycle_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias_exclusive(var(41), var(40)),
        qtt_resource_end_alias_exclusive(var(41), var(40)),
        qtt_resource_drop(var(40)),
    };
    QttResourceBlock exclusive_lifecycle = {
        exclusive_lifecycle_ops, 4};
    assert(qtt_resource_verify(
               &exclusive_lifecycle, NULL, 0).error ==
           QTT_RESOURCE_VALID);
    exclusive_lifecycle_ops[2] =
        qtt_resource_end_alias(var(41), var(40));
    assert(qtt_resource_verify(
               &exclusive_lifecycle, NULL, 0).error ==
           QTT_RESOURCE_LOAN_KIND_MISMATCH);

    exclusive_lifecycle_ops[2] =
        qtt_resource_write_alias(var(41), var(40));
    exclusive_lifecycle_ops[3] =
        qtt_resource_end_alias_exclusive(var(41), var(40));
    QttResourceOp exclusive_write_drop =
        qtt_resource_drop(var(40));
    QttResourceOp exclusive_write_ops[] = {
        exclusive_lifecycle_ops[0],
        exclusive_lifecycle_ops[1],
        exclusive_lifecycle_ops[2],
        exclusive_lifecycle_ops[3],
        exclusive_write_drop,
    };
    QttResourceBlock exclusive_write = {
        exclusive_write_ops, 5};
    assert(qtt_resource_verify(
               &exclusive_write, NULL, 0).error ==
           QTT_RESOURCE_VALID);

    QttPlace reborrow_field =
        qtt_place_project(qtt_place_root(var(40)), 100);
    QttResourceOp reborrow_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias_exclusive(var(41), var(40)),
        qtt_resource_reborrow_place(
            var(42), var(41), reborrow_field, QTT_LOAN_EXCLUSIVE),
        qtt_resource_write_alias_place(var(42), reborrow_field),
        qtt_resource_end_alias_place(
            var(42), reborrow_field, QTT_LOAN_EXCLUSIVE),
        qtt_resource_write_alias_place(
            var(41), qtt_place_root(var(40))),
        qtt_resource_end_alias_exclusive(var(41), var(40)),
        qtt_resource_drop(var(40)),
    };
    QttResourceBlock reborrow = {reborrow_ops, 8};
    assert(qtt_resource_verify(&reborrow, NULL, 0).error ==
           QTT_RESOURCE_VALID);
    QttResourceOp saved_child_write = reborrow_ops[3];
    reborrow_ops[3] = qtt_resource_write_alias_place(
        var(41), qtt_place_root(var(40)));
    assert(qtt_resource_verify(&reborrow, NULL, 0).error ==
           QTT_RESOURCE_ACCESS_CONFLICT);
    reborrow_ops[3] = qtt_resource_end_alias_exclusive(var(41), var(40));
    assert(qtt_resource_verify(&reborrow, NULL, 0).error ==
           QTT_RESOURCE_LOAN_CONFLICT);
    reborrow_ops[3] = saved_child_write;

    QttPlace reborrow_sibling =
        qtt_place_project(qtt_place_root(var(40)), 101);
    reborrow_ops[1] = qtt_resource_alias_place(
        var(41), reborrow_field, QTT_LOAN_EXCLUSIVE);
    reborrow_ops[2] = qtt_resource_reborrow_place(
        var(42), var(41), reborrow_sibling, QTT_LOAN_EXCLUSIVE);
    assert(qtt_resource_verify(&reborrow, NULL, 0).error ==
           QTT_RESOURCE_LOAN_CONFLICT);
    reborrow_ops[1] = qtt_resource_alias(var(41), var(40));
    reborrow_ops[2] = qtt_resource_reborrow_place(
        var(42), var(41), reborrow_field, QTT_LOAN_EXCLUSIVE);
    assert(qtt_resource_verify(&reborrow, NULL, 0).error ==
           QTT_RESOURCE_LOAN_CONFLICT);
    QttResourceOp shared_reborrow_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias(var(41), var(40)),
        qtt_resource_reborrow_place(
            var(42), var(41), reborrow_field, QTT_LOAN_SHARED),
        qtt_resource_borrow(var(40)),
        qtt_resource_end_alias_place(
            var(42), reborrow_field, QTT_LOAN_SHARED),
        qtt_resource_end_alias(var(41), var(40)),
        qtt_resource_drop(var(40)),
    };
    QttResourceBlock shared_reborrow = {shared_reborrow_ops, 7};
    assert(qtt_resource_verify(&shared_reborrow, NULL, 0).error ==
           QTT_RESOURCE_VALID);

    QttPlace field_a =
        qtt_place_project(qtt_place_root(var(40)), 101);
    QttPlace field_b =
        qtt_place_project(qtt_place_root(var(40)), 102);
    QttResourceOp disjoint_field_loans_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias_place(
            var(41), field_a, QTT_LOAN_EXCLUSIVE),
        qtt_resource_alias_place(
            var(42), field_b, QTT_LOAN_EXCLUSIVE),
        qtt_resource_write_alias_place(var(41), field_a),
        qtt_resource_write_alias_place(var(42), field_b),
        qtt_resource_end_alias_place(
            var(42), field_b, QTT_LOAN_EXCLUSIVE),
        qtt_resource_end_alias_place(
            var(41), field_a, QTT_LOAN_EXCLUSIVE),
        qtt_resource_drop(var(40)),
    };
    QttResourceBlock disjoint_field_loans = {
        disjoint_field_loans_ops, 8};
    assert(qtt_resource_verify(
               &disjoint_field_loans, NULL, 0).error ==
           QTT_RESOURCE_VALID);
    assert(qtt_place_equal(
        disjoint_field_loans_ops[1].place, field_a));
    char authority_effect[96];
    assert(qtt_resource_op_effect_label(
        &disjoint_field_loans_ops[3], authority_effect,
        sizeof(authority_effect)));
    assert(strcmp(authority_effect, "write:1:40:101") == 0);
    QttResourceOp placed_read =
        qtt_resource_alias_place(
            var(43), field_b, QTT_LOAN_SHARED);
    assert(qtt_resource_op_effect_label(
        &placed_read, authority_effect, sizeof(authority_effect)));
    assert(strcmp(authority_effect, "read:1:40:102") == 0);

    QttResourceOp partial_drop_ops[] = {
        qtt_resource_alloc(var(50)),
        qtt_resource_alias_place(
            var(51), qtt_place_project(qtt_place_root(var(50)), 201),
            QTT_LOAN_EXCLUSIVE),
        qtt_resource_move_alias_place(
            var(51), qtt_place_project(qtt_place_root(var(50)), 201)),
        qtt_resource_end_alias_place(
            var(51), qtt_place_project(qtt_place_root(var(50)), 201),
            QTT_LOAN_EXCLUSIVE),
        qtt_resource_alias_place(
            var(52), qtt_place_project(qtt_place_root(var(50)), 202),
            QTT_LOAN_EXCLUSIVE),
        qtt_resource_write_alias_place(
            var(52), qtt_place_project(qtt_place_root(var(50)), 202)),
        qtt_resource_end_alias_place(
            var(52), qtt_place_project(qtt_place_root(var(50)), 202),
            QTT_LOAN_EXCLUSIVE),
        qtt_resource_drop(var(50)),
    };
    QttResourceBlock partial_drop = {partial_drop_ops, 8};
    assert(qtt_resource_verify(
               &partial_drop, NULL, 0).error == QTT_RESOURCE_VALID);
    QttHeapExecution partial_execution =
        qtt_resource_execute(&partial_drop, NULL, 0);
    assert(partial_execution.error == QTT_RESOURCE_VALID);
    assert(partial_execution.allocated == 1);
    assert(partial_execution.projected_moves == 1);
    assert(partial_execution.moved_out == 0);
    assert(partial_execution.dropped == 1);
    assert(partial_execution.live_owned == 0);
    assert(qtt_resource_op_effect_label(
        &partial_drop_ops[2], authority_effect,
        sizeof(authority_effect)));
    assert(strcmp(authority_effect, "write:1:50:201") == 0);

    partial_drop_ops[1] = qtt_resource_alias_place(
        var(51), qtt_place_project(qtt_place_root(var(50)), 201),
        QTT_LOAN_SHARED);
    partial_drop_ops[3] = qtt_resource_end_alias_place(
        var(51), qtt_place_project(qtt_place_root(var(50)), 201),
        QTT_LOAN_SHARED);
    assert(qtt_resource_verify(
               &partial_drop, NULL, 0).error ==
           QTT_RESOURCE_ACCESS_CONFLICT);

    QttResourceOp partial_reuse_ops[] = {
        qtt_resource_alloc(var(50)),
        qtt_resource_alias_place(
            var(51), qtt_place_project(qtt_place_root(var(50)), 201),
            QTT_LOAN_EXCLUSIVE),
        qtt_resource_move_alias_place(
            var(51), qtt_place_project(qtt_place_root(var(50)), 201)),
        qtt_resource_end_alias_place(
            var(51), qtt_place_project(qtt_place_root(var(50)), 201),
            QTT_LOAN_EXCLUSIVE),
        qtt_resource_alias_place(
            var(52), qtt_place_project(qtt_place_root(var(50)), 201),
            QTT_LOAN_SHARED),
    };
    QttResourceBlock partial_reuse = {partial_reuse_ops, 5};
    assert(qtt_resource_verify(
               &partial_reuse, NULL, 0).error ==
           QTT_RESOURCE_USE_AFTER_CONSUME);

    partial_reuse_ops[4] = qtt_resource_move(var(50));
    assert(qtt_resource_verify(
               &partial_reuse, NULL, 0).error ==
           QTT_RESOURCE_INVALID_CAPABILITY);

    QttResourceOp branch_partial_then_ops[] = {
        qtt_resource_alias_place(
            var(51), qtt_place_project(qtt_place_root(var(50)), 201),
            QTT_LOAN_EXCLUSIVE),
        qtt_resource_move_alias_place(
            var(51), qtt_place_project(qtt_place_root(var(50)), 201)),
        qtt_resource_end_alias_place(
            var(51), qtt_place_project(qtt_place_root(var(50)), 201),
            QTT_LOAN_EXCLUSIVE),
    };
    QttResourceBlock branch_partial_then = {
        branch_partial_then_ops, 3};
    QttResourceBlock branch_partial_else = {NULL, 0};
    QttResourceOp branch_partial_ops[] = {
        qtt_resource_alloc(var(50)),
        qtt_resource_branch(
            &branch_partial_then, &branch_partial_else),
    };
    QttResourceBlock branch_partial = {branch_partial_ops, 2};
    assert(qtt_resource_verify(
               &branch_partial, NULL, 0).error ==
           QTT_RESOURCE_BRANCH_MISMATCH);

    QttResourceOp branch_partial_else_ops[] = {
        qtt_resource_alias_place(
            var(52), qtt_place_project(qtt_place_root(var(50)), 201),
            QTT_LOAN_EXCLUSIVE),
        qtt_resource_move_alias_place(
            var(52), qtt_place_project(qtt_place_root(var(50)), 201)),
        qtt_resource_end_alias_place(
            var(52), qtt_place_project(qtt_place_root(var(50)), 201),
            QTT_LOAN_EXCLUSIVE),
    };
    branch_partial_else.ops = branch_partial_else_ops;
    branch_partial_else.count = 3;
    QttResourceOp branch_partial_drop = qtt_resource_drop(var(50));
    QttResourceOp branch_partial_balanced_ops[] = {
        branch_partial_ops[0], branch_partial_ops[1], branch_partial_drop,
    };
    QttResourceBlock branch_partial_balanced = {
        branch_partial_balanced_ops, 3};
    assert(qtt_resource_verify(
               &branch_partial_balanced, NULL, 0).error ==
           QTT_RESOURCE_VALID);

    Type projected_string = {.kind = TYPE_STRING};
    LayoutField projected_fields[] = {
        {.name = "name", .type = &projected_string},
    };
    Type projected_layout = {
        .kind = TYPE_LAYOUT,
        .layout_fields = projected_fields,
        .layout_field_count = 1,
    };
    QttCoreNode projected_initializer = {
        .kind = QTT_CORE_LITERAL, .type = &projected_layout};
    QttPlace projected_name = {0};
    assert(qtt_place_layout_field(
        qtt_place_root(var(65)), &projected_layout,
        "name", &projected_name));
    QttCoreNode projected_body = {
        .kind = QTT_CORE_PLACE, .type = &projected_string,
        .place = projected_name,
    };
    QttCoreNode projected_let = {
        .kind = QTT_CORE_LET, .type = &projected_string,
        .let = {
            .binding = {1, 65},
            .value = &projected_initializer,
            .body = &projected_body,
        },
    };
    QttResourceElaborationError projected_error =
        QTT_RESOURCE_ELABORATE_OK;
    QttResourceBlock *projected_resources =
        qtt_resource_elaborate_lexical(
            &projected_let, &projected_error);
    assert(projected_resources &&
           projected_error == QTT_RESOURCE_ELABORATE_OK);
    assert(projected_resources->count == 3);
    assert(projected_resources->ops[0].kind == QTT_RESOURCE_ALLOC);
    assert(projected_resources->ops[1].kind == QTT_RESOURCE_MOVE_PLACE);
    assert(qtt_place_equal(
        projected_resources->ops[1].place, projected_name));
    assert(projected_resources->ops[2].kind == QTT_RESOURCE_DROP);
    assert(qtt_resource_verify(
        projected_resources, NULL, 0).error == QTT_RESOURCE_VALID);
    qtt_resource_block_free(projected_resources);

    disjoint_field_loans_ops[2] =
        qtt_resource_alias_place(
            var(42), field_a, QTT_LOAN_SHARED);
    assert(qtt_resource_verify(
               &disjoint_field_loans, NULL, 0).error ==
           QTT_RESOURCE_LOAN_CONFLICT);

    disjoint_field_loans_ops[2] =
        qtt_resource_alias_place(
            var(42), qtt_place_root(var(40)),
            QTT_LOAN_SHARED);
    assert(qtt_resource_verify(
               &disjoint_field_loans, NULL, 0).error ==
           QTT_RESOURCE_LOAN_CONFLICT);

    disjoint_field_loans_ops[2] =
        qtt_resource_alias_place(
            var(42), field_b, QTT_LOAN_EXCLUSIVE);
    disjoint_field_loans_ops[3] =
        qtt_resource_write_alias_place(var(41), field_b);
    assert(qtt_resource_verify(
               &disjoint_field_loans, NULL, 0).error ==
           QTT_RESOURCE_ALIAS_TARGET_MISMATCH);

    disjoint_field_loans_ops[3] =
        qtt_resource_write_alias_place(var(41), field_a);
    disjoint_field_loans_ops[5] =
        qtt_resource_end_alias_place(
            var(42), field_a, QTT_LOAN_EXCLUSIVE);
    assert(qtt_resource_verify(
               &disjoint_field_loans, NULL, 0).error ==
           QTT_RESOURCE_ALIAS_TARGET_MISMATCH);

    QttResourceOp root_read_during_field_write_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias_place(
            var(41), field_a, QTT_LOAN_EXCLUSIVE),
        qtt_resource_borrow(var(40)),
    };
    QttResourceBlock root_read_during_field_write = {
        root_read_during_field_write_ops, 3};
    assert(qtt_resource_verify(
               &root_read_during_field_write, NULL, 0).error ==
           QTT_RESOURCE_ACCESS_CONFLICT);

    Type immediate_type = {.kind = TYPE_INT};
    QttCoreNode immediate_initializer = {
        .kind = QTT_CORE_LITERAL, .type = &immediate_type};
    QttCoreNode immediate_replacement = {
        .kind = QTT_CORE_LITERAL, .type = &immediate_type};
    QttCoreNode immediate_write = {
        .kind = QTT_CORE_WRITE, .type = &immediate_type,
        .write = {
            .place = {.root = {1, 70}},
            .value = &immediate_replacement,
        },
    };
    QttCoreNode immediate_result = {
        .kind = QTT_CORE_VAR, .type = &immediate_type,
        .var = {1, 70}};
    QttCoreNode *immediate_body_items[] = {
        &immediate_write, &immediate_result};
    QttCoreNode immediate_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &immediate_type,
        .sequence = {
            .items = immediate_body_items, .count = 2}};
    QttCoreNode immediate_let = {
        .kind = QTT_CORE_LET, .type = &immediate_type,
        .let = {
            .binding = {1, 70},
            .value = &immediate_initializer,
            .body = &immediate_body,
        },
    };
    automatic = qtt_resource_elaborate_lexical(
        &immediate_let, &elaboration_error);
    assert(automatic && automatic->count == 0);
    assert(qtt_resource_verify_elaboration(
               automatic, &immediate_let).error ==
           QTT_RESOURCE_VALID);
    qtt_resource_block_free(automatic);

    QttCoreNode heap_replacement = {
        .kind = QTT_CORE_LITERAL, .type = &string_type};
    QttCoreNode heap_write = {
        .kind = QTT_CORE_WRITE, .type = &string_type,
        .write = {
            .place = {.root = {1, 71}},
            .value = &heap_replacement,
        },
    };
    QttCoreNode heap_initializer = {
        .kind = QTT_CORE_LITERAL, .type = &string_type};
    QttCoreNode heap_mutation = {
        .kind = QTT_CORE_LET, .type = &string_type,
        .let = {
            .binding = {1, 71},
            .value = &heap_initializer,
            .body = &heap_write,
        },
    };
    automatic = qtt_resource_elaborate_lexical(
        &heap_mutation, &elaboration_error);
    assert(automatic && elaboration_error == QTT_RESOURCE_ELABORATE_OK);
    assert(automatic->count == 3);
    assert(automatic->ops[0].kind == QTT_RESOURCE_ALLOC);
    assert(automatic->ops[1].kind == QTT_RESOURCE_REPLACE);
    assert(automatic->ops[2].kind == QTT_RESOURCE_DROP);
    assert(qtt_core_var_equal(
        automatic->ops[1].var, (QttCoreVar){1, 71}));
    assert(automatic->ops[1].representation == QTT_REP_OWNED_HEAP);
    assert(qtt_resource_verify_elaboration(
               automatic, &heap_mutation).error ==
           QTT_RESOURCE_VALID);
    QttHeapExecution replacement_heap =
        qtt_resource_execute(automatic, NULL, 0);
    assert(replacement_heap.error == QTT_RESOURCE_VALID);
    assert(replacement_heap.allocated == 2);
    assert(replacement_heap.dropped == 2);
    assert(replacement_heap.live_owned == 0);
    assert(replacement_heap.peak_live_owned == 1);
    automatic->ops[1].representation = QTT_REP_INLINE;
    assert(qtt_resource_verify(
               automatic, NULL, 0).error ==
           QTT_RESOURCE_INVALID_CAPABILITY);
    assert(qtt_resource_verify_elaboration(
               automatic, &heap_mutation).error ==
           QTT_RESOURCE_INVALID_CAPABILITY);
    qtt_resource_block_free(automatic);

    Type bool_type = {.kind = TYPE_BOOL};
    QttCoreNode branch_condition = {
        .kind = QTT_CORE_VAR, .type = &bool_type,
        .var = {1, 99}};
    QttCoreNode branch_then_replacement = {
        .kind = QTT_CORE_LITERAL, .type = &string_type};
    QttCoreNode branch_else_replacement = {
        .kind = QTT_CORE_LITERAL, .type = &string_type};
    QttCoreNode branch_then_write = {
        .kind = QTT_CORE_WRITE, .type = &string_type,
        .write = {
            .place = {.root = {1, 72}},
            .value = &branch_then_replacement,
        },
    };
    QttCoreNode branch_else_write = {
        .kind = QTT_CORE_WRITE, .type = &string_type,
        .write = {
            .place = {.root = {1, 72}},
            .value = &branch_else_replacement,
        },
    };
    QttCoreNode branch_replace = {
        .kind = QTT_CORE_IF, .type = &string_type,
        .conditional = {
            .condition = &branch_condition,
            .then_branch = &branch_then_write,
            .else_branch = &branch_else_write,
        },
    };
    QttCoreNode branch_read = {
        .kind = QTT_CORE_VAR, .type = &string_type,
        .var = {1, 72}};
    QttCoreNode *branch_body_items[] = {
        &branch_replace, &branch_read};
    QttCoreNode branch_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &string_type,
        .sequence = {
            .items = branch_body_items, .count = 2}};
    QttCoreNode branch_mutation = {
        .kind = QTT_CORE_LET, .type = &string_type,
        .let = {
            .binding = {1, 72},
            .value = &heap_initializer,
            .body = &branch_body,
        },
    };
    automatic = qtt_resource_elaborate_lexical(
        &branch_mutation, &elaboration_error);
    assert(automatic && elaboration_error == QTT_RESOURCE_ELABORATE_OK);
    assert(automatic->count == 3);
    assert(automatic->ops[0].kind == QTT_RESOURCE_ALLOC);
    assert(automatic->ops[1].kind == QTT_RESOURCE_BRANCH);
    assert(automatic->ops[1].branch.then_block->count == 1);
    assert(automatic->ops[1].branch.else_block->count == 1);
    assert(automatic->ops[1].branch.then_block->ops[0].kind ==
           QTT_RESOURCE_REPLACE);
    assert(automatic->ops[1].branch.else_block->ops[0].kind ==
           QTT_RESOURCE_REPLACE);
    assert(automatic->ops[2].kind == QTT_RESOURCE_MOVE);
    assert(qtt_resource_verify_elaboration(
               automatic, &branch_mutation).error ==
           QTT_RESOURCE_VALID);
    bool choose_then[] = {true};
    bool choose_else[] = {false};
    QttHeapExecution then_heap =
        qtt_resource_execute(automatic, choose_then, 1);
    QttHeapExecution else_heap =
        qtt_resource_execute(automatic, choose_else, 1);
    assert(then_heap.error == QTT_RESOURCE_VALID);
    assert(else_heap.error == QTT_RESOURCE_VALID);
    assert(then_heap.allocated == 2 && then_heap.dropped == 1);
    assert(else_heap.allocated == 2 && else_heap.dropped == 1);
    assert(then_heap.moved_out == 1 && else_heap.moved_out == 1);
    assert(then_heap.live_owned == 0 && else_heap.live_owned == 0);
    qtt_resource_block_free(automatic);

    QttResourceOp replace_during_loan_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias(var(41), var(40)),
        qtt_resource_replace(var(40), QTT_REP_OWNED_HEAP),
        qtt_resource_end_alias(var(41), var(40)),
        qtt_resource_drop(var(40)),
    };
    QttResourceBlock replace_during_loan = {
        replace_during_loan_ops, 5};
    assert(qtt_resource_verify(
               &replace_during_loan, NULL, 0).error ==
           QTT_RESOURCE_ACCESS_CONFLICT);

    exclusive_write_ops[1] =
        qtt_resource_alias(var(41), var(40));
    exclusive_write_ops[3] =
        qtt_resource_end_alias(var(41), var(40));
    assert(qtt_resource_verify(
               &exclusive_write, NULL, 0).error ==
           QTT_RESOURCE_ACCESS_CONFLICT);

    QttResourceOp drop_during_loan_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias(var(41), var(40)),
        qtt_resource_drop(var(40)),
        qtt_resource_end_alias(var(41), var(40)),
    };
    QttResourceBlock drop_during_loan = {
        drop_during_loan_ops, 4};
    assert(qtt_resource_verify(
               &drop_during_loan, NULL, 0).error ==
           QTT_RESOURCE_ACCESS_CONFLICT);

    QttResourceOp move_alias_ops[] = {
        qtt_resource_alloc(var(40)),
        qtt_resource_alias(var(41), var(40)),
        qtt_resource_move_alias(var(41), var(40)),
        qtt_resource_end_alias(var(41), var(40)),
    };
    QttResourceBlock move_alias = {move_alias_ops, 4};
    assert(qtt_resource_verify(&move_alias, NULL, 0).error ==
           QTT_RESOURCE_VALID);

    move_alias_ops[2] =
        qtt_resource_move_alias(var(42), var(40));
    assert(qtt_resource_verify(&move_alias, NULL, 0).error ==
           QTT_RESOURCE_UNKNOWN_ALIAS);

    QttCoreNode alias2_ref = {
        .kind = QTT_CORE_VAR,
        .type = &string_type,
        .var = {.module_id = 1, .binder_id = 42},
    };
    QttCoreNode alias1_to_alias2 = {
        .kind = QTT_CORE_LET,
        .type = &string_type,
        .let = {
            .binding = {.module_id = 1, .binder_id = 42},
            .value = &alias_ref,
            .body = &alias2_ref,
        },
    };
    alias_unused.let.body = &alias1_to_alias2;
    automatic = qtt_resource_elaborate_lexical(
        &owner_with_alias, &elaboration_error);
    assert(automatic && automatic->count == 8);
    assert(automatic->ops[0].kind == QTT_RESOURCE_ALLOC);
    assert(automatic->ops[1].kind == QTT_RESOURCE_BORROW);
    assert(automatic->ops[2].kind == QTT_RESOURCE_ALIAS);
    assert(qtt_core_var_equal(automatic->ops[2].target, var(40)));
    assert(automatic->ops[3].kind == QTT_RESOURCE_BORROW);
    assert(qtt_core_var_equal(automatic->ops[3].var, var(40)));
    assert(automatic->ops[4].kind == QTT_RESOURCE_ALIAS);
    assert(qtt_core_var_equal(automatic->ops[4].target, var(40)));
    assert(automatic->ops[5].kind == QTT_RESOURCE_MOVE_ALIAS);
    assert(qtt_core_var_equal(automatic->ops[5].var, var(42)));
    assert(qtt_core_var_equal(automatic->ops[5].target, var(40)));
    assert(automatic->ops[6].kind == QTT_RESOURCE_END_ALIAS);
    assert(qtt_core_var_equal(automatic->ops[6].var, var(42)));
    assert(automatic->ops[7].kind == QTT_RESOURCE_END_ALIAS);
    assert(qtt_core_var_equal(automatic->ops[7].var, var(41)));
    assert(qtt_resource_verify_elaboration(
               automatic, &owner_with_alias).error ==
           QTT_RESOURCE_VALID);
    qtt_resource_block_free(automatic);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_resource_test.c"
            executable = directory / "qtt_resource_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "core.c"),
                    str(ROOT / "qtt" / "place.c"),
                    str(ROOT / "qtt" / "resource.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
