"""Unified SSA/ANF computation and ownership verification."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttAnfTests(unittest.TestCase):
    def test_diamond_join_checks_ssa_and_ownership(self):
        source = r'''
#include "qtt/anf.h"
#include "qtt/eval.h"
#include <assert.h>
#include <string.h>

int main(void) {
    QttCoreVar owned = {.module_id = 1, .binder_id = 1};
    QttAnfInstruction entry_ops[] = {
        qtt_anf_const_bool(1, true),
        qtt_anf_alloc(owned, QTT_REP_OWNED_HEAP),
    };
    QttAnfInstruction then_ops[] = {
        qtt_anf_const_number(2, 10),
        qtt_anf_drop(owned),
    };
    QttAnfInstruction else_ops[] = {
        qtt_anf_const_number(3, 20),
        qtt_anf_drop(owned),
    };
    QttAnfValue join_params[] = {4};
    QttAnfType join_types[] = {QTT_ANF_TYPE_NUMBER};
    QttAnfValue then_args[] = {2};
    QttAnfValue else_args[] = {3};
    QttAnfBlock blocks[] = {
        {.instructions = entry_ops, .instruction_count = 2,
         .terminator = qtt_anf_branch(1, 1, 2)},
        {.instructions = then_ops, .instruction_count = 2,
         .terminator = qtt_anf_jump(3, then_args, 1)},
        {.instructions = else_ops, .instruction_count = 2,
         .terminator = qtt_anf_jump(3, else_args, 1)},
        {.parameters = join_params, .parameter_types = join_types,
         .parameter_count = 1,
         .terminator = qtt_anf_return(4)},
    };
    QttAnfProgram valid = {.blocks = blocks, .block_count = 4, .entry = 0};
    assert(qtt_anf_verify(&valid).error == QTT_ANF_VALID);
    QttAnfEvaluation evaluated = qtt_anf_evaluate(&valid);
    assert(evaluated.error == QTT_ANF_EVAL_OK);
    assert(evaluated.value.kind == QTT_ANF_VALUE_NUMBER);
    assert(evaluated.value.number == 10);
    assert(evaluated.heap.allocated == 1);
    assert(evaluated.heap.dropped == 1);

    join_types[0] = QTT_ANF_TYPE_BOOL;
    assert(qtt_anf_verify(&valid).error == QTT_ANF_TYPE_MISMATCH);
    join_types[0] = QTT_ANF_TYPE_NUMBER;
    entry_ops[0] = qtt_anf_const_number(1, 1);
    assert(qtt_anf_verify(&valid).error == QTT_ANF_NON_BOOLEAN_BRANCH);
    entry_ops[0] = qtt_anf_const_bool(1, false);

    QttAnfInstruction wrong_rep_ops[] = {
        qtt_anf_const_string(10, "heap"),
        qtt_anf_bind(owned, QTT_REP_IMMEDIATE, 10),
        qtt_anf_drop(owned),
    };
    QttAnfBlock wrong_rep_block = {
        .instructions = wrong_rep_ops,
        .instruction_count = 3,
        .terminator = qtt_anf_return(10),
    };
    QttAnfProgram wrong_rep = {
        .blocks = &wrong_rep_block, .block_count = 1, .entry = 0};
    assert(qtt_anf_verify(&wrong_rep).error ==
           QTT_ANF_REPRESENTATION_MISMATCH);

    QttAnfInstruction wrong_borrow_ops[] = {
        qtt_anf_const_string(11, "heap"),
        qtt_anf_bind(owned, QTT_REP_OWNED_HEAP, 11),
        qtt_anf_borrow_value(12, owned),
        qtt_anf_drop(owned),
    };
    wrong_borrow_ops[2].result_type = QTT_ANF_TYPE_NUMBER;
    QttAnfBlock wrong_borrow_block = {
        .instructions = wrong_borrow_ops,
        .instruction_count = 4,
        .terminator = qtt_anf_return(12),
    };
    QttAnfProgram wrong_borrow = {
        .blocks = &wrong_borrow_block, .block_count = 1, .entry = 0};
    assert(qtt_anf_verify(&wrong_borrow).error == QTT_ANF_TYPE_MISMATCH);
    assert(evaluated.heap.live_owned == 0);

    QttPlace mutable_place =
        qtt_place_root((QttCoreVar){.module_id = 1, .binder_id = 90});
    QttAnfInstruction write_ops[] = {
        qtt_anf_const_number(20, 7),
        qtt_anf_write_place(21, mutable_place, 20),
    };
    QttAnfPlaceDeclaration mutable_places[] = {
        {.place = mutable_place, .type = QTT_ANF_TYPE_NUMBER},
    };
    QttAnfBlock write_block = {
        .instructions = write_ops, .instruction_count = 2,
        .terminator = qtt_anf_return(21),
    };
    QttAnfProgram write_program = {
        .blocks = &write_block, .block_count = 1,
        .mutable_places = mutable_places,
        .mutable_place_count = 1,
    };
    assert(qtt_anf_verify(&write_program).error == QTT_ANF_VALID);
    QttAnfEvaluation write_evaluated =
        qtt_anf_evaluate(&write_program);
    assert(write_evaluated.error == QTT_ANF_EVAL_OK);
    assert(write_evaluated.value.kind == QTT_ANF_VALUE_NUMBER);
    assert(write_evaluated.value.number == 7);
    write_program.mutable_place_count = 0;
    assert(qtt_anf_verify(&write_program).error ==
           QTT_ANF_INVALID_PLACE);
    write_program.mutable_place_count = 1;
    mutable_places[0].type = QTT_ANF_TYPE_BOOL;
    assert(qtt_anf_verify(&write_program).error ==
           QTT_ANF_TYPE_MISMATCH);
    mutable_places[0].type = QTT_ANF_TYPE_NUMBER;

    else_ops[1] = qtt_anf_borrow(owned);
    assert(qtt_anf_verify(&valid).error == QTT_ANF_OWNERSHIP_JOIN_MISMATCH);
    else_ops[1] = qtt_anf_drop(owned);
    blocks[2].terminator = qtt_anf_jump(3, NULL, 0);
    assert(qtt_anf_verify(&valid).error == QTT_ANF_BLOCK_ARGUMENT_MISMATCH);

    blocks[2].terminator = qtt_anf_jump(3, else_args, 1);
    entry_ops[0] = qtt_anf_const_bool(1, false);
    evaluated = qtt_anf_evaluate(&valid);
    assert(evaluated.error == QTT_ANF_EVAL_OK);
    assert(evaluated.value.number == 20);
    assert(evaluated.heap.allocated == 1);
    assert(evaluated.heap.dropped == 1);

    QttAnfValue duplicate_params[] = {5, 5};
    QttAnfType duplicate_types[] = {
        QTT_ANF_TYPE_NUMBER, QTT_ANF_TYPE_NUMBER};
    QttAnfValue duplicate_then_args[] = {2, 2};
    QttAnfValue duplicate_else_args[] = {3, 3};
    blocks[3].parameters = duplicate_params;
    blocks[3].parameter_types = duplicate_types;
    blocks[3].parameter_count = 2;
    blocks[1].terminator = qtt_anf_jump(3, duplicate_then_args, 2);
    blocks[2].terminator = qtt_anf_jump(3, duplicate_else_args, 2);
    assert(qtt_anf_verify(&valid).error == QTT_ANF_DUPLICATE_VALUE);

    AST number_then = {.type = AST_NUMBER, .number = 41};
    AST number_else = {.type = AST_NUMBER, .number = 99};
    QttCoreNode core_condition = {
        .kind = QTT_CORE_GLOBAL, .global = {.name = "True"}};
    QttCoreNode core_then = {
        .kind = QTT_CORE_LITERAL, .literal = {.source = &number_then}};
    QttCoreNode core_else = {
        .kind = QTT_CORE_LITERAL, .literal = {.source = &number_else}};
    QttCoreNode core_if = {
        .kind = QTT_CORE_IF,
        .conditional = {
            .condition = &core_condition,
            .then_branch = &core_then,
            .else_branch = &core_else,
        },
    };
    QttAnfLowerError lower_error = QTT_ANF_LOWER_OK;
    QttAnfProgram *lowered = qtt_anf_lower_core(&core_if, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(lowered->block_count == 4);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    evaluated = qtt_anf_evaluate(lowered);
    QttEvalResult core_value = qtt_core_evaluate(&core_if);
    assert(evaluated.error == QTT_ANF_EVAL_OK);
    assert(core_value.error == QTT_EVAL_OK);
    assert(evaluated.value.kind == QTT_ANF_VALUE_NUMBER);
    assert(evaluated.value.number == core_value.value.number);
    qtt_anf_program_free(lowered);

    Type mutation_int_type = {.kind = TYPE_INT};
    AST one_ast = {.type = AST_NUMBER, .number = 1};
    AST two_ast = {.type = AST_NUMBER, .number = 2};
    QttCoreVar mutable_var = {.module_id = 7, .binder_id = 19};
    QttCoreNode one = {
        .kind = QTT_CORE_LITERAL, .type = &mutation_int_type,
        .literal = {.source = &one_ast}};
    QttCoreNode two = {
        .kind = QTT_CORE_LITERAL, .type = &mutation_int_type,
        .literal = {.source = &two_ast}};
    QttCoreNode write = {
        .kind = QTT_CORE_WRITE, .type = &mutation_int_type,
        .write = {
            .place = {.root = mutable_var},
            .value = &two,
        },
    };
    QttCoreNode read_after_write = {
        .kind = QTT_CORE_VAR, .type = &mutation_int_type,
        .var = mutable_var};
    QttCoreNode *mutation_items[] = {&write, &read_after_write};
    QttCoreNode mutation_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &mutation_int_type,
        .sequence = {.items = mutation_items, .count = 2}};
    QttCoreNode mutation = {
        .kind = QTT_CORE_LET, .type = &mutation_int_type,
        .let = {
            .binding = mutable_var, .value = &one,
            .body = &mutation_body,
        },
    };
    lowered = qtt_anf_lower_core(&mutation, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(lowered->mutable_place_count == 1);
    assert(qtt_place_equal(
        lowered->mutable_places[0].place,
        qtt_place_root(mutable_var)));
    assert(lowered->blocks[0].instructions[2].kind ==
           QTT_ANF_WRITE_PLACE);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    write_evaluated = qtt_anf_evaluate(lowered);
    assert(write_evaluated.error == QTT_ANF_EVAL_OK);
    assert(write_evaluated.value.number == 2);
    qtt_anf_program_free(lowered);

    Type aggregate_type = {.kind = TYPE_LAYOUT};
    AST aggregate_ast = {.type = AST_LIST};
    QttCoreVar aggregate_var = {.module_id = 7, .binder_id = 29};
    QttPlace aggregate_field = qtt_place_project(
        qtt_place_root(aggregate_var), 1);
    QttCoreNode aggregate_value = {
        .kind = QTT_CORE_LITERAL, .type = &aggregate_type,
        .literal = {.source = &aggregate_ast}};
    QttCoreNode field_write = {
        .kind = QTT_CORE_WRITE, .type = &mutation_int_type,
        .write = {.place = aggregate_field, .value = &two}};
    QttCoreNode aggregate_read = {
        .kind = QTT_CORE_VAR, .type = &aggregate_type,
        .var = aggregate_var};
    QttCoreNode *aggregate_items[] = {&field_write, &aggregate_read};
    QttCoreNode aggregate_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &aggregate_type,
        .sequence = {.items = aggregate_items, .count = 2}};
    QttCoreNode aggregate_mutation = {
        .kind = QTT_CORE_LET, .type = &aggregate_type,
        .let = {.binding = aggregate_var, .value = &aggregate_value,
                .body = &aggregate_sequence}};
    lowered = qtt_anf_lower_core(&aggregate_mutation, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    bool saw_exclusive = false, saw_authorized_write = false;
    bool saw_end = false;
    for (size_t i = 0; i < lowered->blocks[0].instruction_count; i++) {
        QttAnfInstruction *instruction =
            &lowered->blocks[0].instructions[i];
        if (instruction->kind == QTT_ANF_BORROW &&
            instruction->loan_kind == QTT_LOAN_EXCLUSIVE &&
            qtt_place_equal(instruction->place, aggregate_field))
            saw_exclusive = true;
        if (instruction->kind == QTT_ANF_WRITE_PLACE &&
            instruction->loan_id &&
            qtt_place_equal(instruction->place, aggregate_field))
            saw_authorized_write = true;
        if (instruction->kind == QTT_ANF_END_BORROW)
            saw_end = true;
    }
    assert(saw_exclusive && saw_authorized_write && saw_end);
    qtt_anf_program_free(lowered);

    /*
     * A mutable lexical binding is an SSA memory version.  Both branch exits
     * must pass their version to the join, and the post-join read must use the
     * join parameter rather than whichever arm happened to lower last.
     */
    AST three_ast = {.type = AST_NUMBER, .number = 3};
    QttCoreNode three = {
        .kind = QTT_CORE_LITERAL, .type = &mutation_int_type,
        .literal = {.source = &three_ast}};
    QttCoreNode write_three = {
        .kind = QTT_CORE_WRITE, .type = &mutation_int_type,
        .write = {
            .place = {.root = mutable_var},
            .value = &three,
        },
    };
    QttCoreNode mutation_condition = {
        .kind = QTT_CORE_GLOBAL, .type = &mutation_int_type,
        .global = {.name = "True"}};
    QttCoreNode mutation_branch = {
        .kind = QTT_CORE_IF, .type = &mutation_int_type,
        .conditional = {
            .condition = &mutation_condition,
            .then_branch = &write,
            .else_branch = &write_three,
        },
    };
    QttCoreNode *branch_mutation_items[] = {
        &mutation_branch, &read_after_write};
    QttCoreNode branch_mutation_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &mutation_int_type,
        .sequence = {
            .items = branch_mutation_items, .count = 2}};
    QttCoreNode branch_mutation = mutation;
    branch_mutation.let.body = &branch_mutation_body;
    lowered = qtt_anf_lower_core(&branch_mutation, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(lowered->block_count == 4);
    assert(lowered->blocks[3].parameter_count == 2);
    assert(lowered->blocks[1].terminator.argument_count == 2);
    assert(lowered->blocks[2].terminator.argument_count == 2);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    write_evaluated = qtt_anf_evaluate(lowered);
    assert(write_evaluated.error == QTT_ANF_EVAL_OK);
    assert(write_evaluated.value.number == 2);
    QttAnfValue saved_else_entry =
        lowered->blocks[0].terminator.else_arguments[0];
    lowered->blocks[0].terminator.else_arguments[0] = 0;
    assert(qtt_anf_verify(lowered).error == QTT_ANF_UNDEFINED_VALUE);
    lowered->blocks[0].terminator.else_arguments[0] = saved_else_entry;
    qtt_anf_program_free(lowered);

    mutation_condition.global.name = "False";
    mutation_branch.conditional.else_branch = &three;
    lowered = qtt_anf_lower_core(&branch_mutation, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(lowered->blocks[3].parameter_count == 2);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    write_evaluated = qtt_anf_evaluate(lowered);
    assert(write_evaluated.error == QTT_ANF_EVAL_OK);
    assert(write_evaluated.value.number == 1);
    qtt_anf_program_free(lowered);
    mutation_condition.global.name = "True";
    mutation_branch.conditional.else_branch = &write_three;

    QttCoreNode nested_mutation_branch = {
        .kind = QTT_CORE_IF, .type = &mutation_int_type,
        .conditional = {
            .condition = &mutation_condition,
            .then_branch = &mutation_branch,
            .else_branch = &write_three,
        },
    };
    QttCoreNode *nested_mutation_items[] = {
        &nested_mutation_branch, &read_after_write};
    QttCoreNode nested_mutation_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &mutation_int_type,
        .sequence = {
            .items = nested_mutation_items, .count = 2}};
    QttCoreNode nested_mutation = mutation;
    nested_mutation.let.body = &nested_mutation_body;
    lowered = qtt_anf_lower_core(&nested_mutation, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(lowered->block_count == 7);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    write_evaluated = qtt_anf_evaluate(lowered);
    assert(write_evaluated.error == QTT_ANF_EVAL_OK);
    assert(write_evaluated.value.number == 2);
    qtt_anf_program_free(lowered);

    Type string_type = {.kind = TYPE_STRING};
    AST string_ast = {.type = AST_STRING, .string = "owned"};
    QttCoreVar lexical_var = {.module_id = 7, .binder_id = 11};
    QttCoreNode string_literal = {
        .kind = QTT_CORE_LITERAL,
        .type = &string_type,
        .literal = {.source = &string_ast},
    };
    QttCoreNode first_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = lexical_var};
    QttCoreNode last_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = lexical_var};
    QttCoreNode *uses[] = {&first_use, &last_use};
    QttCoreNode use_sequence = {
        .kind = QTT_CORE_SEQUENCE,
        .type = &string_type,
        .sequence = {.items = uses, .count = 2},
    };
    QttCoreNode used_let = {
        .kind = QTT_CORE_LET,
        .type = &string_type,
        .let = {
            .binding = lexical_var,
            .value = &string_literal,
            .body = &use_sequence,
        },
    };
    lowered = qtt_anf_lower_core(&used_let, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    assert(lowered->blocks[0].instruction_count == 4);
    assert(lowered->blocks[0].instructions[2].kind == QTT_ANF_BORROW);
    assert(lowered->blocks[0].instructions[3].kind == QTT_ANF_MOVE);
    evaluated = qtt_anf_evaluate(lowered);
    core_value = qtt_core_evaluate(&used_let);
    assert(evaluated.error == QTT_ANF_EVAL_OK);
    assert(core_value.error == QTT_EVAL_OK);
    assert(evaluated.value.kind == QTT_ANF_VALUE_STRING);
    assert(strcmp(evaluated.value.string, core_value.value.string) == 0);
    assert(evaluated.heap.allocated == 1);
    assert(evaluated.heap.borrows == 1);
    assert(evaluated.heap.moved_out == 1);
    assert(evaluated.heap.live_owned == 0);
    qtt_anf_program_free(lowered);

    AST replacement_string_ast = {
        .type = AST_STRING, .string = "replacement"};
    QttCoreNode replacement_string = {
        .kind = QTT_CORE_LITERAL,
        .type = &string_type,
        .literal = {.source = &replacement_string_ast},
    };
    QttCoreNode replace_heap = {
        .kind = QTT_CORE_WRITE, .type = &string_type,
        .write = {
            .place = {.root = lexical_var},
            .value = &replacement_string,
        },
    };
    QttCoreNode *replace_then_read[] = {
        &replace_heap, &last_use};
    QttCoreNode replace_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &string_type,
        .sequence = {.items = replace_then_read, .count = 2},
    };
    QttCoreNode replaced_let = used_let;
    replaced_let.let.body = &replace_body;
    lowered = qtt_anf_lower_core(&replaced_let, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    bool saw_replace = false;
    for (size_t i = 0;
         i < lowered->blocks[0].instruction_count; i++)
        if (lowered->blocks[0].instructions[i].kind ==
            QTT_ANF_REPLACE)
            saw_replace = true;
    assert(saw_replace);
    evaluated = qtt_anf_evaluate(lowered);
    assert(evaluated.error == QTT_ANF_EVAL_OK);
    assert(evaluated.value.kind == QTT_ANF_VALUE_STRING);
    assert(strcmp(evaluated.value.string, "replacement") == 0);
    assert(evaluated.heap.allocated == 2);
    assert(evaluated.heap.dropped == 1);
    assert(evaluated.heap.moved_out == 1);
    assert(evaluated.heap.live_owned == 0);
    qtt_anf_program_free(lowered);

    QttCoreNode mixed_if = core_if;
    QttCoreNode mixed_string = string_literal;
    mixed_if.conditional.else_branch = &mixed_string;
    lowered = qtt_anf_lower_core(&mixed_if, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_TYPE_MISMATCH);
    qtt_anf_program_free(lowered);

    QttCoreNode *discard_then_number[] = {&first_use, &core_then};
    QttCoreNode discard_sequence = {
        .kind = QTT_CORE_SEQUENCE,
        .sequence = {.items = discard_then_number, .count = 2},
    };
    QttCoreNode discarded_let = used_let;
    discarded_let.let.body = &discard_sequence;
    lowered = qtt_anf_lower_core(&discarded_let, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    evaluated = qtt_anf_evaluate(lowered);
    core_value = qtt_core_evaluate(&discarded_let);
    assert(evaluated.error == QTT_ANF_EVAL_OK);
    assert(core_value.error == QTT_EVAL_OK);
    assert(evaluated.value.number == core_value.value.number);
    assert(evaluated.heap.allocated == 1);
    assert(evaluated.heap.moved_out == 0);
    assert(evaluated.heap.dropped == 1);
    assert(evaluated.heap.live_owned == 0);
    qtt_anf_program_free(lowered);

    QttCoreNode branch_use_then = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = lexical_var};
    QttCoreNode branch_use_else = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = lexical_var};
    QttCoreNode lexical_if = {
        .kind = QTT_CORE_IF,
        .type = &string_type,
        .conditional = {
            .condition = &core_condition,
            .then_branch = &branch_use_then,
            .else_branch = &branch_use_else,
        },
    };
    QttCoreNode after_join_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = lexical_var};
    QttCoreNode *branch_then_later[] = {&lexical_if, &after_join_use};
    QttCoreNode branch_sequence = {
        .kind = QTT_CORE_SEQUENCE,
        .type = &string_type,
        .sequence = {.items = branch_then_later, .count = 2},
    };
    QttCoreNode branch_let = used_let;
    branch_let.let.body = &branch_sequence;
    lowered = qtt_anf_lower_core(&branch_let, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    evaluated = qtt_anf_evaluate(lowered);
    core_value = qtt_core_evaluate(&branch_let);
    assert(evaluated.error == QTT_ANF_EVAL_OK);
    assert(core_value.error == QTT_EVAL_OK);
    assert(evaluated.value.kind == QTT_ANF_VALUE_STRING);
    assert(strcmp(evaluated.value.string, core_value.value.string) == 0);
    assert(evaluated.heap.allocated == 1);
    assert(evaluated.heap.borrows == 1);
    assert(evaluated.heap.moved_out == 1);
    assert(evaluated.heap.live_owned == 0);
    qtt_anf_program_free(lowered);

    QttCoreNode branch_last_let = used_let;
    branch_last_let.let.body = &lexical_if;
    lowered = qtt_anf_lower_core(&branch_last_let, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    evaluated = qtt_anf_evaluate(lowered);
    assert(evaluated.error == QTT_ANF_EVAL_OK);
    assert(strcmp(evaluated.value.string, "owned") == 0);
    assert(evaluated.heap.allocated == 1);
    assert(evaluated.heap.borrows == 0);
    assert(evaluated.heap.moved_out == 1);
    assert(evaluated.heap.live_owned == 0);
    qtt_anf_program_free(lowered);

    QttCoreNode unused_let = used_let;
    unused_let.let.body = &core_then;
    lowered = qtt_anf_lower_core(&unused_let, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    assert(lowered->blocks[0].instructions[
        lowered->blocks[0].instruction_count - 1].kind == QTT_ANF_DROP);
    evaluated = qtt_anf_evaluate(lowered);
    assert(evaluated.error == QTT_ANF_EVAL_OK);
    assert(evaluated.value.number == 41);
    assert(evaluated.heap.allocated == 1);
    assert(evaluated.heap.dropped == 1);
    assert(evaluated.heap.live_owned == 0);
    qtt_anf_program_free(lowered);

    QttCoreVar partial_root = {.module_id = 1, .binder_id = 92};
    QttPlace partial_field = {
        .root = {.module_id = 1, .binder_id = 92},
        .projection_id = 1,
        .projection_path = {1},
        .projection_depth = 1,
    };
    QttAnfInstruction repeated_place_ops[] = {
        qtt_anf_const_string(30, "aggregate"),
        qtt_anf_bind(partial_root, QTT_REP_OWNED_HEAP, 30),
        qtt_anf_move_place(31, QTT_ANF_TYPE_STRING, partial_field),
        qtt_anf_move_place(32, QTT_ANF_TYPE_STRING, partial_field),
        qtt_anf_drop(partial_root),
    };
    QttAnfBlock repeated_place_block = {
        .instructions = repeated_place_ops,
        .instruction_count = 5,
        .terminator = qtt_anf_return(31),
    };
    QttAnfProgram repeated_place = {
        .blocks = &repeated_place_block, .block_count = 1, .entry = 0};
    assert(qtt_anf_verify(&repeated_place).error ==
           QTT_ANF_INVALID_RESOURCE);

    QttAnfInstruction partial_entry_ops[] = {
        qtt_anf_const_bool(40, true),
        qtt_anf_const_string(41, "aggregate"),
        qtt_anf_bind(partial_root, QTT_REP_OWNED_HEAP, 41),
    };
    QttAnfInstruction partial_then_ops[] = {
        qtt_anf_move_place(42, QTT_ANF_TYPE_STRING, partial_field),
        qtt_anf_const_number(43, 1),
    };
    QttAnfInstruction partial_else_ops[] = {
        qtt_anf_const_number(44, 2),
    };
    QttAnfValue partial_join_parameter[] = {45};
    QttAnfType partial_join_type[] = {QTT_ANF_TYPE_NUMBER};
    QttAnfValue partial_then_argument[] = {43};
    QttAnfValue partial_else_argument[] = {44};
    QttAnfInstruction partial_join_ops[] = {
        qtt_anf_drop(partial_root),
    };
    QttAnfBlock partial_blocks[] = {
        {.instructions = partial_entry_ops, .instruction_count = 3,
         .terminator = qtt_anf_branch(40, 1, 2)},
        {.instructions = partial_then_ops, .instruction_count = 2,
         .terminator = qtt_anf_jump(3, partial_then_argument, 1)},
        {.instructions = partial_else_ops, .instruction_count = 1,
         .terminator = qtt_anf_jump(3, partial_else_argument, 1)},
        {.parameters = partial_join_parameter,
         .parameter_types = partial_join_type,
         .parameter_count = 1,
         .instructions = partial_join_ops,
         .instruction_count = 1,
         .terminator = qtt_anf_return(45)},
    };
    QttAnfProgram partial_join = {
        .blocks = partial_blocks, .block_count = 4, .entry = 0};
    assert(qtt_anf_verify(&partial_join).error ==
           QTT_ANF_OWNERSHIP_JOIN_MISMATCH);

    Type integer_type = {.kind = TYPE_INT};
    AST scalar_ast = {.type = AST_NUMBER, .number = 7};
    QttCoreVar scalar_var = {.module_id = 1, .binder_id = 91};
    QttCoreNode scalar_value = {
        .kind = QTT_CORE_LITERAL, .type = &integer_type,
        .literal = {.source = &scalar_ast}};
    QttCoreNode scalar_use = {
        .kind = QTT_CORE_VAR, .type = &integer_type,
        .var = scalar_var};
    QttCoreNode scalar_let = {
        .kind = QTT_CORE_LET, .type = &integer_type,
        .let = {.binding = scalar_var, .value = &scalar_value,
                .body = &scalar_use}};
    lowered = qtt_anf_lower_core(&scalar_let, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    for (size_t i = 0;
         i < lowered->blocks[0].instruction_count; i++) {
        QttAnfInstructionKind kind =
            lowered->blocks[0].instructions[i].kind;
        assert(kind != QTT_ANF_ALLOC &&
               kind != QTT_ANF_BORROW && kind != QTT_ANF_MOVE &&
               kind != QTT_ANF_DROP);
    }
    evaluated = qtt_anf_evaluate(lowered);
    assert(evaluated.error == QTT_ANF_EVAL_OK);
    assert(evaluated.value.number == 7);
    assert(evaluated.heap.allocated == 0);
    assert(evaluated.heap.live_owned == 0);
    qtt_anf_program_free(lowered);

    QttDemandError demand_error = QTT_DEMAND_OK;
    QttDemandCertificate *demand =
        qtt_demand_derive(&used_let, &demand_error);
    assert(demand && demand_error == QTT_DEMAND_OK);
    lowered = qtt_anf_lower_core_certified(
        &used_let, demand, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    qtt_anf_program_free(lowered);
    used_let.let.body = &core_then;
    lowered = qtt_anf_lower_core_certified(
        &used_let, demand, &lower_error);
    assert(!lowered && lower_error == QTT_ANF_LOWER_INVALID_DEMAND);
    used_let.let.body = &use_sequence;
    qtt_demand_certificate_free(demand);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_anf_test.c"
            executable = directory / "qtt_anf_test"
            harness.write_text(source)
            subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-iquote", str(ROOT), str(harness),
                 str(ROOT / "qtt" / "core.c"),
                 str(ROOT / "qtt" / "quantity.c"),
                 str(ROOT / "qtt" / "demand.c"),
                 str(ROOT / "qtt" / "graded.c"),
                 str(ROOT / "qtt" / "resource.c"),
                 str(ROOT / "qtt" / "eval.c"),
                 str(ROOT / "qtt" / "signature.c"),
                 str(ROOT / "effects" / "effect.c"),
                 str(ROOT / "qtt" / "place.c"),
                 str(ROOT / "qtt" / "call.c"),
                 str(ROOT / "qtt" / "type_identity.c"),
                 str(ROOT / "qtt" / "signature_env.c"),
                 str(ROOT / "qtt" / "anf.c"), "-o", str(executable)],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
