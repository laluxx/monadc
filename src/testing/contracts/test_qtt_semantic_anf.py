"""Semantic-IR-authorized ownership ANF lowering."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class QttSemanticAnfTests(unittest.TestCase):
    def test_anf_consumes_verified_capability_transitions(self):
        source = r'''
#include "qtt/semantic_anf.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void split_loan_region_across_cfg(QttAnfProgram *program) {
    assert(program && program->block_count == 1);
    QttAnfBlock original = program->blocks[0];
    size_t borrow = original.instruction_count;
    size_t write = original.instruction_count;
    size_t end = original.instruction_count;
    for (size_t i = 0; i < original.instruction_count; i++) {
        if (original.instructions[i].kind == QTT_ANF_BORROW &&
            original.instructions[i].loan_kind == QTT_LOAN_EXCLUSIVE)
            borrow = i;
        else if (original.instructions[i].kind == QTT_ANF_WRITE_PLACE)
            write = i;
        else if (original.instructions[i].kind == QTT_ANF_END_BORROW)
            end = i;
    }
    assert(borrow < write && write < end);

    QttAnfBlock *blocks = calloc(3, sizeof(*blocks));
    assert(blocks);
    size_t cuts[] = {borrow + 1, write + 1, original.instruction_count};
    size_t starts[] = {0, borrow + 1, write + 1};
    for (size_t block = 0; block < 3; block++) {
        size_t count = cuts[block] - starts[block];
        blocks[block].instructions = calloc(count, sizeof(QttAnfInstruction));
        assert(blocks[block].instructions);
        memcpy(blocks[block].instructions,
               original.instructions + starts[block],
               count * sizeof(QttAnfInstruction));
        blocks[block].instruction_count = count;
    }

    /* Every prior SSA result is an explicit block argument.  Reusing its
     * numeric name is valid because ANF names are scoped by their block. */
    size_t carried = 0;
    for (size_t i = 0; i <= borrow; i++)
        if (original.instructions[i].result) carried++;
    QttAnfValue *values = calloc(carried + 1, sizeof(*values));
    QttAnfType *types = calloc(carried + 1, sizeof(*types));
    QttAnfLoanId *loans = calloc(carried + 1, sizeof(*loans));
    assert(values && types && loans);
    size_t cursor = 0;
    for (size_t i = 0; i <= borrow; i++)
        if (original.instructions[i].result) {
            values[cursor] = original.instructions[i].result;
            types[cursor++] = original.instructions[i].result_type;
            if (original.instructions[i].kind == QTT_ANF_BORROW)
                loans[cursor - 1] = original.instructions[i].loan_id;
        }
    blocks[0].terminator = qtt_anf_jump(1, values, carried);
    blocks[1].parameters = malloc(carried * sizeof(*values));
    blocks[1].parameter_types = malloc(carried * sizeof(*types));
    blocks[1].parameter_loans = malloc(carried * sizeof(*loans));
    assert(blocks[1].parameters && blocks[1].parameter_types &&
           blocks[1].parameter_loans);
    memcpy(blocks[1].parameters, values, carried * sizeof(*values));
    memcpy(blocks[1].parameter_types, types, carried * sizeof(*types));
    memcpy(blocks[1].parameter_loans, loans, carried * sizeof(*loans));
    blocks[1].parameter_count = carried;

    QttAnfValue write_result = original.instructions[write].result;
    QttAnfType write_type = original.instructions[write].result_type;
    QttAnfValue *after_write = calloc(carried + 1, sizeof(*after_write));
    QttAnfType *after_types = calloc(carried + 1, sizeof(*after_types));
    QttAnfLoanId *after_loans = calloc(carried + 1, sizeof(*after_loans));
    assert(after_write && after_types && after_loans);
    memcpy(after_write, values, carried * sizeof(*values));
    memcpy(after_types, types, carried * sizeof(*types));
    memcpy(after_loans, loans, carried * sizeof(*loans));
    after_write[carried] = write_result;
    after_types[carried] = write_type;
    blocks[1].terminator = qtt_anf_jump(2, after_write, carried + 1);
    blocks[2].parameters = malloc((carried + 1) * sizeof(*after_write));
    blocks[2].parameter_types = malloc((carried + 1) * sizeof(*after_types));
    blocks[2].parameter_loans = malloc((carried + 1) * sizeof(*after_loans));
    assert(blocks[2].parameters && blocks[2].parameter_types &&
           blocks[2].parameter_loans);
    memcpy(blocks[2].parameters, after_write,
           (carried + 1) * sizeof(*after_write));
    memcpy(blocks[2].parameter_types, after_types,
           (carried + 1) * sizeof(*after_types));
    memcpy(blocks[2].parameter_loans, after_loans,
           (carried + 1) * sizeof(*after_loans));
    blocks[2].parameter_count = carried + 1;
    blocks[2].terminator = original.terminator;

    free(types);
    free(loans);
    free(after_types);
    free(after_loans);
    free(original.instructions);
    free(program->blocks);
    program->blocks = blocks;
    program->block_count = 3;
}

int main(void) {
    Type string_type = {.kind = TYPE_STRING};
    Type int_type = {.kind = TYPE_INT};
    Type bool_type = {.kind = TYPE_BOOL};
    AST string_ast = {.type = AST_STRING, .string = "owned"};
    AST number_ast = {.type = AST_NUMBER, .number = 42};
    QttCoreNode initializer = {
        .kind = QTT_CORE_LITERAL, .type = &string_type,
        .literal = {.source = &string_ast}};
    QttCoreNode body = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreVar x = {.module_id = 2, .binder_id = 7};
    QttCoreNode let = {
        .kind = QTT_CORE_LET, .type = &int_type,
        .let = {.binding = x, .value = &initializer, .body = &body}};

    QttSemanticIrError ir_error = QTT_SEMANTIC_IR_OK;
    QttSemanticFunction *semantic =
        qtt_semantic_ir_lower(&let, &ir_error);
    assert(semantic && qtt_semantic_ir_transition_count(semantic) == 2);
    QttSemanticAnfError bridge_error = QTT_SEMANTIC_ANF_OK;
    QttAnfLowerError lower_error = QTT_ANF_LOWER_OK;
    QttAnfProgram *anf = qtt_semantic_anf_lower(
        semantic, &let, &bridge_error, &lower_error);
    assert(anf && bridge_error == QTT_SEMANTIC_ANF_OK);
    assert(lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(anf).error == QTT_ANF_VALID);
    QttAnfEvaluation evaluated = qtt_anf_evaluate(anf);
    assert(evaluated.error == QTT_ANF_EVAL_OK);
    assert(evaluated.value.number == 42);
    assert(evaluated.heap.allocated == 1);
    assert(evaluated.heap.dropped == 1);
    qtt_anf_program_free(anf);

    qtt_semantic_ir_transitions(semantic)[1].after =
        QTT_SEMANTIC_CAPABILITY_OWNED;
    anf = qtt_semantic_anf_lower(
        semantic, &let, &bridge_error, &lower_error);
    assert(!anf &&
           bridge_error == QTT_SEMANTIC_ANF_INVALID_IR);
    qtt_semantic_ir_free(semantic);

    AST one_ast = {.type = AST_NUMBER, .number = 1};
    AST two_ast = {.type = AST_NUMBER, .number = 2};
    QttCoreVar mutable = {.module_id = 2, .binder_id = 80};
    QttCoreNode one = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &one_ast}};
    QttCoreNode two = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &two_ast}};
    QttCoreNode write = {
        .kind = QTT_CORE_WRITE, .type = &int_type,
        .write = {
            .place = {.root = mutable},
            .value = &two,
        },
    };
    QttCoreNode read_after_write = {
        .kind = QTT_CORE_VAR, .type = &int_type, .var = mutable};
    QttCoreNode *mutation_items[] = {&write, &read_after_write};
    QttCoreNode mutation_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {.items = mutation_items, .count = 2}};
    QttCoreNode mutation = {
        .kind = QTT_CORE_LET, .type = &int_type,
        .let = {
            .binding = mutable, .value = &one,
            .body = &mutation_body,
        },
    };
    semantic = qtt_semantic_ir_lower(&mutation, &ir_error);
    assert(semantic);
    anf = qtt_semantic_anf_lower(
        semantic, &mutation, &bridge_error, &lower_error);
    assert(anf && bridge_error == QTT_SEMANTIC_ANF_OK);
    assert(qtt_semantic_anf_verify_correspondence(
               semantic, &mutation, anf) == QTT_SEMANTIC_ANF_OK);
    assert(anf->mutable_place_count == 1);
    QttPlace forged_place =
        qtt_place_root((QttCoreVar){.module_id = 2, .binder_id = 99});
    anf->mutable_places[0].place = forged_place;
    for (size_t i = 0;
         i < anf->blocks[0].instruction_count; i++)
        if (anf->blocks[0].instructions[i].kind ==
                QTT_ANF_WRITE_PLACE)
            anf->blocks[0].instructions[i].place = forged_place;
    assert(qtt_anf_verify(anf).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_correspondence(
               semantic, &mutation, anf) ==
           QTT_SEMANTIC_ANF_EFFECT_MISMATCH);
    qtt_anf_program_free(anf);
    qtt_semantic_ir_free(semantic);

    Type aggregate_type = {.kind = TYPE_LAYOUT};
    AST aggregate_ast = {.type = AST_LIST};
    QttCoreVar aggregate_var = {.module_id = 2, .binder_id = 180};
    QttPlace aggregate_field = qtt_place_project(
        qtt_place_root(aggregate_var), 1);
    QttCoreNode aggregate_value = {
        .kind = QTT_CORE_LITERAL, .type = &aggregate_type,
        .literal = {.source = &aggregate_ast}};
    QttCoreNode projected_write = {
        .kind = QTT_CORE_WRITE, .type = &int_type,
        .write = {.place = aggregate_field, .value = &two}};
    QttCoreNode aggregate_read = {
        .kind = QTT_CORE_VAR, .type = &aggregate_type,
        .var = aggregate_var};
    QttCoreNode *projected_items[] = {
        &projected_write, &aggregate_read};
    QttCoreNode projected_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &aggregate_type,
        .sequence = {.items = projected_items, .count = 2}};
    QttCoreNode projected_mutation = {
        .kind = QTT_CORE_LET, .type = &aggregate_type,
        .let = {.binding = aggregate_var,
                .value = &aggregate_value,
                .body = &projected_body}};
    semantic = qtt_semantic_ir_lower(&projected_mutation, &ir_error);
    assert(semantic);
    char write_label[96];
    assert(qtt_place_effect_label(
        write_label, sizeof(write_label), "write", aggregate_field));
    bool found_write_effect = false;
    QttSemanticNode *semantic_nodes = qtt_semantic_ir_nodes(semantic);
    for (size_t i = 0; i < qtt_semantic_ir_node_count(semantic); i++)
        if (semantic_nodes[i].source == &projected_write) {
            assert(qtt_semantic_ir_effect_label_count(
                semantic, i, write_label) == 1);
            found_write_effect = true;
        }
    assert(found_write_effect);
    anf = qtt_semantic_anf_lower(
        semantic, &projected_mutation, &bridge_error, &lower_error);
    assert(anf && bridge_error == QTT_SEMANTIC_ANF_OK);
    QttAnfInstruction *authorized_write = NULL;
    for (size_t i = 0; i < anf->blocks[0].instruction_count; i++)
        if (anf->blocks[0].instructions[i].kind == QTT_ANF_WRITE_PLACE)
            authorized_write = &anf->blocks[0].instructions[i];
    assert(authorized_write && authorized_write->loan_id);
    split_loan_region_across_cfg(anf);
    assert(qtt_anf_verify(anf).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_correspondence(
               semantic, &projected_mutation, anf) ==
           QTT_SEMANTIC_ANF_OK);
    authorized_write = NULL;
    for (size_t block = 0; block < anf->block_count; block++)
        for (size_t i = 0; i < anf->blocks[block].instruction_count; i++)
            if (anf->blocks[block].instructions[i].kind ==
                    QTT_ANF_WRITE_PLACE)
                authorized_write = &anf->blocks[block].instructions[i];
    assert(authorized_write);
    authorized_write->loan_id++;
    assert(qtt_semantic_anf_verify_correspondence(
               semantic, &projected_mutation, anf) ==
           QTT_SEMANTIC_ANF_EFFECT_MISMATCH);
    qtt_anf_program_free(anf);
    qtt_semantic_ir_free(semantic);

    AST replacement_ast = {
        .type = AST_STRING, .string = "replacement"};
    QttCoreVar replaceable = {
        .module_id = 2, .binder_id = 81};
    QttCoreNode replacement = {
        .kind = QTT_CORE_LITERAL, .type = &string_type,
        .literal = {.source = &replacement_ast}};
    QttCoreNode replace = {
        .kind = QTT_CORE_WRITE, .type = &string_type,
        .write = {
            .place = {.root = replaceable},
            .value = &replacement,
        },
    };
    QttCoreNode read_after_replace = {
        .kind = QTT_CORE_VAR, .type = &string_type,
        .var = replaceable};
    QttCoreNode *replacement_items[] = {
        &replace, &read_after_replace};
    QttCoreNode replacement_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &string_type,
        .sequence = {
            .items = replacement_items, .count = 2}};
    QttCoreNode replacement_let = {
        .kind = QTT_CORE_LET, .type = &string_type,
        .let = {
            .binding = replaceable,
            .value = &initializer,
            .body = &replacement_body,
        },
    };
    semantic = qtt_semantic_ir_lower(
        &replacement_let, &ir_error);
    assert(semantic);
    anf = qtt_semantic_anf_lower(
        semantic, &replacement_let,
        &bridge_error, &lower_error);
    assert(anf && bridge_error == QTT_SEMANTIC_ANF_OK);
    assert(qtt_semantic_anf_verify_correspondence(
               semantic, &replacement_let, anf) ==
           QTT_SEMANTIC_ANF_OK);
    QttAnfInstruction *replace_instruction = NULL;
    for (size_t i = 0;
         i < anf->blocks[0].instruction_count; i++)
        if (anf->blocks[0].instructions[i].kind ==
                QTT_ANF_REPLACE)
            replace_instruction =
                &anf->blocks[0].instructions[i];
    assert(replace_instruction);
    QttAnfEvaluation replacement_result =
        qtt_anf_evaluate(anf);
    assert(replacement_result.error == QTT_ANF_EVAL_OK);
    assert(replacement_result.value.kind ==
           QTT_ANF_VALUE_STRING);
    assert(replacement_result.value.string ==
           replacement_ast.string);
    replace_instruction->place.root.binder_id++;
    assert(qtt_anf_verify(anf).error != QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_correspondence(
               semantic, &replacement_let, anf) ==
           QTT_SEMANTIC_ANF_EFFECT_MISMATCH);
    qtt_anf_program_free(anf);
    qtt_semantic_ir_free(semantic);

    AST three_ast = {.type = AST_NUMBER, .number = 3};
    QttCoreNode three = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &three_ast}};
    QttCoreNode write_three = {
        .kind = QTT_CORE_WRITE, .type = &int_type,
        .write = {
            .place = {.root = mutable},
            .value = &three,
        },
    };
    QttCoreNode write_condition = {
        .kind = QTT_CORE_GLOBAL, .type = &bool_type,
        .global = {.name = "True"}};
    QttCoreNode write_branch = {
        .kind = QTT_CORE_IF, .type = &int_type,
        .conditional = {
            .condition = &write_condition,
            .then_branch = &write,
            .else_branch = &write_three,
        },
    };
    QttCoreNode *branch_mutation_items[] = {
        &write_branch, &read_after_write};
    QttCoreNode branch_mutation_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {
            .items = branch_mutation_items, .count = 2}};
    QttCoreNode branch_mutation = mutation;
    branch_mutation.let.body = &branch_mutation_body;
    semantic = qtt_semantic_ir_lower(&branch_mutation, &ir_error);
    assert(semantic);
    anf = qtt_semantic_anf_lower(
        semantic, &branch_mutation, &bridge_error, &lower_error);
    assert(anf && bridge_error == QTT_SEMANTIC_ANF_OK);
    assert(qtt_semantic_anf_verify_correspondence(
               semantic, &branch_mutation, anf) ==
           QTT_SEMANTIC_ANF_OK);
    assert(anf->blocks[3].parameter_count == 2);
    evaluated = qtt_anf_evaluate(anf);
    assert(evaluated.error == QTT_ANF_EVAL_OK);
    assert(evaluated.value.number == 2);
    qtt_anf_program_free(anf);
    qtt_semantic_ir_free(semantic);

    QttCoreNode condition = {
        .kind = QTT_CORE_GLOBAL, .type = &int_type,
        .global = {.name = "True"}};
    QttCoreNode then_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = x};
    QttCoreNode else_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = x};
    QttCoreNode branch = {
        .kind = QTT_CORE_IF, .type = &string_type,
        .conditional = {.condition = &condition,
                        .then_branch = &then_use,
                        .else_branch = &else_use}};
    let.type = &string_type;
    let.let.body = &branch;
    semantic = qtt_semantic_ir_lower(&let, &ir_error);
    assert(semantic && qtt_semantic_ir_edge_count(semantic) == 2);
    QttSemanticCapabilityEdge *edges =
        qtt_semantic_ir_edges(semantic);
    assert(edges[0].entry_live_count == 1);
    assert(edges[0].exit_live_count == 0);
    assert(edges[1].entry_live_count == 1);
    assert(edges[1].exit_live_count == 0);
    assert(edges[0].entry_context_fingerprint ==
           edges[1].entry_context_fingerprint);
    assert(edges[0].exit_context_fingerprint ==
           edges[1].exit_context_fingerprint);
    anf = qtt_semantic_anf_lower(
        semantic, &let, &bridge_error, &lower_error);
    assert(anf && bridge_error == QTT_SEMANTIC_ANF_OK);
    assert(anf->block_count == 4);
    assert(qtt_anf_verify(anf).error == QTT_ANF_VALID);
    qtt_anf_program_free(anf);
    edges[1].exit_context_fingerprint ^= 1;
    anf = qtt_semantic_anf_lower(
        semantic, &let, &bridge_error, &lower_error);
    assert(!anf &&
           bridge_error == QTT_SEMANTIC_ANF_INVALID_IR);
    qtt_semantic_ir_free(semantic);

    /* Nested control flow must use explicit branch identity, not block order. */
    QttCoreNode nested_then = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreNode nested_else = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreNode nested = {
        .kind = QTT_CORE_IF, .type = &int_type,
        .conditional = {.condition = &condition,
                        .then_branch = &nested_then,
                        .else_branch = &nested_else}};
    QttCoreNode outer_else = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreNode outer = {
        .kind = QTT_CORE_IF, .type = &int_type,
        .conditional = {.condition = &condition,
                        .then_branch = &nested,
                        .else_branch = &outer_else}};
    semantic = qtt_semantic_ir_lower(&outer, &ir_error);
    assert(semantic && qtt_semantic_ir_edge_count(semantic) == 4);
    anf = qtt_semantic_anf_lower(
        semantic, &outer, &bridge_error, &lower_error);
    assert(anf && bridge_error == QTT_SEMANTIC_ANF_OK);
    assert(anf->block_count == 7);
    assert(qtt_anf_verify(anf).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_correspondence(
               semantic, &outer, anf) == QTT_SEMANTIC_ANF_OK);
    size_t nested_branch_block = anf->block_count;
    for (size_t i = 0; i < anf->block_count; i++)
        if (anf->blocks[i].terminator.kind == QTT_ANF_BRANCH &&
            anf->blocks[i].terminator.branch_ordinal == 1)
            nested_branch_block = i;
    assert(nested_branch_block < anf->block_count);
    anf->blocks[nested_branch_block].terminator.branch_ordinal = 0;
    assert(qtt_semantic_anf_verify_correspondence(
               semantic, &outer, anf) ==
           QTT_SEMANTIC_ANF_UNSUPPORTED_CFG);
    qtt_anf_program_free(anf);
    qtt_semantic_ir_free(semantic);

    QttCoreNode owned_then = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = x};
    QttCoreNode owned_else = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = x};
    QttCoreNode owned_nested = {
        .kind = QTT_CORE_IF, .type = &string_type,
        .conditional = {.condition = &condition,
                        .then_branch = &owned_then,
                        .else_branch = &owned_else}};
    QttCoreNode owned_outer_else = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = x};
    QttCoreNode owned_outer = {
        .kind = QTT_CORE_IF, .type = &string_type,
        .conditional = {.condition = &condition,
                        .then_branch = &owned_nested,
                        .else_branch = &owned_outer_else}};
    let.type = &string_type;
    let.let.body = &owned_outer;
    semantic = qtt_semantic_ir_lower(&let, &ir_error);
    assert(semantic && qtt_semantic_ir_edge_count(semantic) == 4);
    assert(qtt_semantic_ir_transition_count(semantic) == 4);
    anf = qtt_semantic_anf_lower(
        semantic, &let, &bridge_error, &lower_error);
    assert(anf && bridge_error == QTT_SEMANTIC_ANF_OK);
    assert(qtt_anf_verify(anf).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_correspondence(
               semantic, &let, anf) == QTT_SEMANTIC_ANF_OK);
    QttAnfInstruction *nested_move = NULL;
    for (size_t i = 0; i < anf->block_count; i++)
        for (size_t j = 0; j < anf->blocks[i].instruction_count; j++) {
            QttAnfInstruction *candidate =
                &anf->blocks[i].instructions[j];
            if (candidate->kind == QTT_ANF_MOVE &&
                candidate->control_path_count == 2)
                nested_move = candidate;
        }
    assert(nested_move && nested_move->control_path);
    nested_move->control_path[1].else_arm =
        !nested_move->control_path[1].else_arm;
    assert(qtt_anf_verify(anf).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_correspondence(
               semantic, &let, anf) ==
           QTT_SEMANTIC_ANF_OWNERSHIP_MISMATCH);
    qtt_anf_program_free(anf);
    qtt_semantic_ir_free(semantic);

    /*
     * A closure environment is not an untyped list of hidden parameters.
     * Semantic IR fixes slot order, provenance, type, capability, and exit
     * disposition; ownership ANF must carry that exact interface.
     */
    QttCoreVar captured = {.module_id = 2, .binder_id = 70};
    QttCoreNode captured_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode closure = {
        .kind = QTT_CORE_LAMBDA, .type = &string_type,
        .lambda = {.body = &captured_use}};
    QttCoreNode closure_call = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &closure}};
    semantic = qtt_semantic_ir_lower(&closure_call, &ir_error);
    assert(semantic && qtt_semantic_ir_closure_field_count(semantic) == 1);
    anf = calloc(1, sizeof(*anf));
    assert(anf);
    assert(qtt_semantic_anf_materialize_environment(
               semantic, &closure, anf) == QTT_SEMANTIC_ANF_OK);
    assert(anf->environment_slot_count == 1);
    assert(anf->environment_slots);
    assert(anf->environment_slots[0].closure_id == 2);
    assert(anf->environment_slots[0].ordinal == 0);
    assert(qtt_core_var_equal(
        anf->environment_slots[0].source, captured));
    assert(anf->environment_slots[0].type_id.value != 0);
    assert(anf->environment_slots[0].representation ==
           QTT_REP_OWNED_HEAP);
    assert(anf->environment_slots[0].capability ==
           QTT_SEMANTIC_CAPABILITY_OWNED);
    assert(anf->environment_slots[0].exit ==
           QTT_CLOSURE_FIELD_MOVE_OUT);
    assert(anf->environment_slots[0].has_destructor);
    assert(anf->environment_slots[0].destructor_id.type_fingerprint != 0);
    assert(qtt_semantic_anf_verify_environment(
               semantic, &closure, anf) == QTT_SEMANTIC_ANF_OK);

    anf->blocks = calloc(1, sizeof(*anf->blocks));
    anf->block_count = 1;
    anf->initial_resources =
        calloc(1, sizeof(*anf->initial_resources));
    anf->initial_representations =
        calloc(1, sizeof(*anf->initial_representations));
    anf->initial_resource_count = 1;
    anf->initial_resources[0] = captured;
    anf->initial_representations[0] = QTT_REP_OWNED_HEAP;
    anf->blocks[0].instructions =
        calloc(1, sizeof(*anf->blocks[0].instructions));
    anf->blocks[0].instruction_count = 1;
    anf->blocks[0].instructions[0] = qtt_anf_project_environment(
        1, QTT_ANF_TYPE_STRING, 2, 0, captured,
        QTT_ANF_ENV_MOVE);
    anf->blocks[0].terminator = qtt_anf_return(1);
    assert(qtt_anf_verify(anf).error == QTT_ANF_VALID);
    assert(qtt_anf_evaluate(anf).error ==
           QTT_ANF_EVAL_UNSUPPORTED_ENVIRONMENT);
    assert(qtt_semantic_anf_verify_projections(
               semantic, &closure, anf) == QTT_SEMANTIC_ANF_OK);
    anf->blocks[0].instructions[0].environment_ordinal = 1;
    assert(qtt_semantic_anf_verify_projections(
               semantic, &closure, anf) ==
           QTT_SEMANTIC_ANF_PROJECTION_MISMATCH);
    anf->blocks[0].instructions[0].environment_ordinal = 0;

    QttCoreNode captured_first = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode captured_last = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode *captured_sequence_items[] = {
        &captured_first, &captured_last};
    QttCoreNode captured_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &string_type,
        .sequence = {.items = captured_sequence_items, .count = 2}};
    QttCoreNode lowering_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &string_type,
        .lambda = {.body = &captured_sequence}};
    QttCoreNode lowering_call = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &lowering_closure}};
    QttSemanticFunction *lowering_semantic =
        qtt_semantic_ir_lower(&lowering_call, &ir_error);
    assert(lowering_semantic);
    QttAnfProgram *lowered_closure =
        qtt_semantic_anf_lower_closure_body(
            lowering_semantic, &lowering_closure,
            &bridge_error, &lower_error);
    assert(lowered_closure);
    assert(bridge_error == QTT_SEMANTIC_ANF_OK);
    assert(lower_error == QTT_ANF_LOWER_OK);
    assert(lowered_closure->block_count == 1);
    assert(lowered_closure->blocks[0].instruction_count == 2);
    assert(lowered_closure->blocks[0].instructions[0].kind ==
           QTT_ANF_ENV_PROJECT);
    assert(lowered_closure->blocks[0].instructions[0].environment_action ==
           QTT_ANF_ENV_BORROW);
    assert(lowered_closure->blocks[0].instructions[1].environment_action ==
           QTT_ANF_ENV_MOVE);
    assert(qtt_anf_verify(lowered_closure).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &lowering_closure,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    QttCoreNode release_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode release_result = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreNode *release_items[] = {&release_use, &release_result};
    QttCoreNode release_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {.items = release_items, .count = 2}};
    QttCoreNode release_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {.body = &release_sequence}};
    QttCoreNode release_call = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &release_closure}};
    lowering_semantic = qtt_semantic_ir_lower(
        &release_call, &ir_error);
    assert(lowering_semantic);
    lowered_closure = qtt_semantic_anf_lower_closure_body(
        lowering_semantic, &release_closure,
        &bridge_error, &lower_error);
    assert(lowered_closure);
    assert(lowered_closure->blocks[0].instruction_count == 3);
    assert(lowered_closure->blocks[0].instructions[0].kind ==
           QTT_ANF_ENV_PROJECT);
    assert(lowered_closure->blocks[0].instructions[0].environment_action ==
           QTT_ANF_ENV_BORROW);
    assert(lowered_closure->blocks[0].instructions[1].kind ==
           QTT_ANF_CONST_NUMBER);
    assert(lowered_closure->blocks[0].instructions[2].kind ==
           QTT_ANF_ENV_PROJECT);
    assert(lowered_closure->blocks[0].instructions[2].environment_action ==
           QTT_ANF_ENV_RELEASE);
    assert(lowered_closure->blocks[0].instructions[2].result == 0);
    assert(qtt_anf_verify(lowered_closure).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &release_closure,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    QttCoreVar local_number = {.module_id = 2, .binder_id = 72};
    QttCoreNode lexical_capture_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode lexical_initializer = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreNode lexical_result = {
        .kind = QTT_CORE_VAR, .type = &int_type, .var = local_number};
    QttCoreNode lexical_let = {
        .kind = QTT_CORE_LET, .type = &int_type,
        .let = {.binding = local_number,
                .value = &lexical_initializer,
                .body = &lexical_result}};
    QttCoreNode *lexical_items[] = {
        &lexical_capture_use, &lexical_let};
    QttCoreNode lexical_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {.items = lexical_items, .count = 2}};
    QttCoreNode lexical_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {.body = &lexical_sequence}};
    QttCoreNode lexical_call = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &lexical_closure}};
    lowering_semantic = qtt_semantic_ir_lower(
        &lexical_call, &ir_error);
    assert(lowering_semantic);
    lowered_closure = qtt_semantic_anf_lower_closure_body(
        lowering_semantic, &lexical_closure,
        &bridge_error, &lower_error);
    assert(lowered_closure);
    assert(lowered_closure->blocks[0].instruction_count == 4);
    assert(lowered_closure->blocks[0].instructions[1].kind ==
           QTT_ANF_CONST_NUMBER);
    assert(lowered_closure->blocks[0].instructions[2].kind ==
           QTT_ANF_ALIAS);
    assert(lowered_closure->blocks[0].instructions[3].kind ==
           QTT_ANF_ENV_PROJECT);
    assert(lowered_closure->blocks[0].instructions[3].environment_action ==
           QTT_ANF_ENV_RELEASE);
    assert(qtt_anf_verify(lowered_closure).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &lexical_closure,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    QttCoreVar call_parameter = {.module_id = 2, .binder_id = 80};
    const Type *call_parameter_types[] = {&int_type};
    QttCoreNode call_callee_body = {
        .kind = QTT_CORE_VAR, .type = &int_type,
        .var = call_parameter};
    QttCoreNode call_callee = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {.params = &call_parameter,
                   .param_types = call_parameter_types,
                   .param_count = 1,
                   .body = &call_callee_body}};
    QttSignatureError signature_error = QTT_SIGNATURE_OK;
    QttFunctionSignature *call_signature =
        qtt_signature_derive(&call_callee, &signature_error);
    QttTypeArena *call_types = qtt_type_arena_new();
    QttSignatureEnv *call_env =
        qtt_signature_env_new(call_types);
    assert(call_signature && call_types && call_env);
    assert(qtt_signature_canonicalize(
               call_signature, call_types) ==
           QTT_SIGNATURE_CANONICAL);
    assert(qtt_signature_env_register(
               call_env, 2, "Main.number-id",
               call_signature, NULL) == QTT_SIGNATURE_ENV_OK);
    QttCoreNode closure_call_global = {
        .kind = QTT_CORE_GLOBAL, .type = &int_type,
        .global = {.name = "Main.number-id"}};
    QttCoreNode closure_call_argument = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreNode *closure_call_arguments[] = {
        &closure_call_argument};
    QttCoreNode closure_named_call = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &closure_call_global,
                  .arguments = closure_call_arguments,
                  .argument_count = 1}};
    QttCoreNode call_capture_use = {
        .kind = QTT_CORE_VAR, .type = &string_type,
        .var = captured};
    QttCoreNode *call_body_items[] = {
        &call_capture_use, &closure_named_call};
    QttCoreNode closure_call_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {.items = call_body_items, .count = 2}};
    QttCoreNode closure_call_lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {.body = &closure_call_body}};
    QttCoreNode closure_call_invoke = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &closure_call_lambda}};
    lowering_semantic = qtt_semantic_ir_lower_in_env(
        &closure_call_invoke, call_env, 2, &ir_error);
    assert(lowering_semantic);
    lowered_closure = qtt_semantic_anf_lower_closure_body_in_env(
        lowering_semantic, &closure_call_lambda,
        call_env, 2, &bridge_error, &lower_error);
    assert(lowered_closure);
    assert(lowered_closure->signatures == call_env);
    size_t known_calls = 0;
    for (size_t block = 0; block < lowered_closure->block_count; block++)
        for (size_t i = 0;
             i < lowered_closure->blocks[block].instruction_count; i++)
            known_calls +=
                lowered_closure->blocks[block].instructions[i].kind ==
                QTT_ANF_CALL;
    assert(known_calls == 1);
    assert(qtt_anf_verify(lowered_closure).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_calls(
               lowering_semantic, &closure_call_invoke,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &closure_call_lambda,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    QttAnfInstruction *known_call_instruction = NULL;
    for (size_t i = 0;
         i < lowered_closure->blocks[0].instruction_count; i++)
        if (lowered_closure->blocks[0].instructions[i].kind ==
            QTT_ANF_CALL)
            known_call_instruction =
                &lowered_closure->blocks[0].instructions[i];
    assert(known_call_instruction);
    known_call_instruction->contract_fingerprint ^= 1;
    assert(qtt_semantic_anf_verify_calls(
               lowering_semantic, &closure_call_invoke,
               lowered_closure) ==
           QTT_SEMANTIC_ANF_OWNERSHIP_MISMATCH);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    QttCoreNode branch_call_condition = {
        .kind = QTT_CORE_GLOBAL, .type = &bool_type,
        .global = {.name = "True"}};
    QttCoreNode branch_call_then_capture = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode branch_call_else_capture = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode branch_call_then_global = {
        .kind = QTT_CORE_GLOBAL, .type = &int_type,
        .global = {.name = "Main.number-id"}};
    QttCoreNode branch_call_else_global = {
        .kind = QTT_CORE_GLOBAL, .type = &int_type,
        .global = {.name = "Main.number-id"}};
    QttCoreNode branch_call_then_argument = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreNode branch_call_else_argument = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreNode *branch_call_then_arguments[] = {
        &branch_call_then_argument};
    QttCoreNode *branch_call_else_arguments[] = {
        &branch_call_else_argument};
    QttCoreNode branch_call_then_apply = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &branch_call_then_global,
                  .arguments = branch_call_then_arguments,
                  .argument_count = 1}};
    QttCoreNode branch_call_else_apply = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &branch_call_else_global,
                  .arguments = branch_call_else_arguments,
                  .argument_count = 1}};
    QttCoreNode *branch_call_then_items[] = {
        &branch_call_then_capture, &branch_call_then_apply};
    QttCoreNode *branch_call_else_items[] = {
        &branch_call_else_capture, &branch_call_else_apply};
    QttCoreNode branch_call_then_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {
            .items = branch_call_then_items, .count = 2}};
    QttCoreNode branch_call_else_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {
            .items = branch_call_else_items, .count = 2}};
    QttCoreNode branch_call_if = {
        .kind = QTT_CORE_IF, .type = &int_type,
        .conditional = {
            .condition = &branch_call_condition,
            .then_branch = &branch_call_then_sequence,
            .else_branch = &branch_call_else_sequence}};
    QttCoreNode branch_call_lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {.body = &branch_call_if}};
    QttCoreNode branch_call_invoke = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &branch_call_lambda}};
    lowering_semantic = qtt_semantic_ir_lower_in_env(
        &branch_call_invoke, call_env, 2, &ir_error);
    assert(lowering_semantic);
    lowered_closure = qtt_semantic_anf_lower_closure_body_in_env(
        lowering_semantic, &branch_call_lambda,
        call_env, 2, &bridge_error, &lower_error);
    assert(lowered_closure);
    size_t path_calls = 0;
    for (size_t block = 0; block < lowered_closure->block_count; block++)
        for (size_t i = 0;
             i < lowered_closure->blocks[block].instruction_count; i++) {
            QttAnfInstruction *instruction =
                &lowered_closure->blocks[block].instructions[i];
            if (instruction->kind == QTT_ANF_CALL) {
                path_calls++;
                assert(instruction->control_path_count == 1);
            }
        }
    assert(path_calls == 2);
    assert(qtt_anf_verify(lowered_closure).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_calls(
               lowering_semantic, &branch_call_invoke,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &branch_call_lambda,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    QttTypeIdentityError consume_type_error =
        QTT_TYPE_IDENTITY_OK;
    QttParameterContract consume_parameter = {
        .var = {.module_id = 2, .binder_id = 81},
        .quantity = qtt_quantity_finite(1),
        .observed = qtt_quantity_finite(1),
        .mode = QTT_OWNERSHIP_CONSUMED,
        .type = &string_type,
        .type_id = qtt_type_intern(
            call_types, &string_type, &consume_type_error),
        .representation = QTT_REP_OWNED_HEAP,
    };
    QttFunctionSignature consume_signature = {
        .parameters = &consume_parameter,
        .parameter_count = 1,
        .result_type = &int_type,
        .result = {
            .mode = QTT_RESULT_IMMEDIATE,
            .type = &int_type,
            .type_id = qtt_type_intern(
                call_types, &int_type, &consume_type_error),
            .representation = QTT_REP_IMMEDIATE,
        },
    };
    consume_signature.contract_fingerprint =
        qtt_signature_contract_fingerprint(&consume_signature);
    assert(qtt_signature_env_register(
               call_env, 2, "Main.consume-string",
               &consume_signature, NULL) == QTT_SIGNATURE_ENV_OK);
    QttCoreNode consume_global = {
        .kind = QTT_CORE_GLOBAL, .type = &int_type,
        .global = {.name = "Main.consume-string"}};
    QttCoreNode consume_capture = {
        .kind = QTT_CORE_VAR, .type = &string_type,
        .var = captured};
    QttCoreNode *consume_args[] = {&consume_capture};
    QttCoreNode consume_apply = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &consume_global,
                  .arguments = consume_args,
                  .argument_count = 1}};
    QttCoreNode consume_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {.body = &consume_apply}};
    QttCoreNode consume_invoke = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &consume_closure}};
    lowering_semantic = qtt_semantic_ir_lower_in_env(
        &consume_invoke, call_env, 2, &ir_error);
    assert(lowering_semantic);
    QttSemanticClosureFieldEvidence *consume_fields =
        qtt_semantic_ir_closure_fields(lowering_semantic);
    assert(qtt_semantic_ir_closure_field_count(
               lowering_semantic) == 1);
    assert(consume_fields[0].storage ==
           QTT_CLOSURE_STORAGE_UNIQUE);
    assert(consume_fields[0].exit ==
           QTT_CLOSURE_FIELD_MOVE_OUT);
    lowered_closure = qtt_semantic_anf_lower_closure_body_in_env(
        lowering_semantic, &consume_closure,
        call_env, 2, &bridge_error, &lower_error);
    assert(lowered_closure);
    QttAnfInstruction *consume_call = NULL;
    for (size_t i = 0;
         i < lowered_closure->blocks[0].instruction_count; i++)
        if (lowered_closure->blocks[0].instructions[i].kind ==
            QTT_ANF_CALL)
            consume_call =
                &lowered_closure->blocks[0].instructions[i];
    assert(consume_call);
    assert(consume_call->call_argument_count == 1);
    assert(consume_call->call_arguments[0].transfer ==
           QTT_CALL_MOVE);
    assert(qtt_core_var_equal(
        consume_call->call_arguments[0].source_resource,
        captured));
    assert(qtt_anf_verify(lowered_closure).error ==
           QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_calls(
               lowering_semantic, &consume_invoke,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &consume_closure,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    consume_call->call_arguments[0].transfer =
        QTT_CALL_BORROW;
    assert(qtt_anf_verify(lowered_closure).error ==
           QTT_ANF_CALL_TRANSFER_MISMATCH);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    consume_parameter.mode = QTT_OWNERSHIP_BORROWED;
    consume_signature.contract_fingerprint =
        qtt_signature_contract_fingerprint(&consume_signature);
    lowering_semantic = qtt_semantic_ir_lower_in_env(
        &consume_invoke, call_env, 2, &ir_error);
    assert(lowering_semantic);
    consume_fields =
        qtt_semantic_ir_closure_fields(lowering_semantic);
    assert(consume_fields[0].storage ==
           QTT_CLOSURE_STORAGE_UNIQUE);
    assert(consume_fields[0].exit ==
           QTT_CLOSURE_FIELD_RELEASE);
    lowered_closure = qtt_semantic_anf_lower_closure_body_in_env(
        lowering_semantic, &consume_closure,
        call_env, 2, &bridge_error, &lower_error);
    assert(lowered_closure);
    size_t borrow_projects = 0;
    size_t release_projects = 0;
    consume_call = NULL;
    for (size_t i = 0;
         i < lowered_closure->blocks[0].instruction_count; i++) {
        QttAnfInstruction *instruction =
            &lowered_closure->blocks[0].instructions[i];
        if (instruction->kind == QTT_ANF_CALL)
            consume_call = instruction;
        if (instruction->kind == QTT_ANF_ENV_PROJECT &&
            instruction->environment_action == QTT_ANF_ENV_BORROW)
            borrow_projects++;
        if (instruction->kind == QTT_ANF_ENV_PROJECT &&
            instruction->environment_action == QTT_ANF_ENV_RELEASE)
            release_projects++;
    }
    assert(consume_call);
    assert(consume_call->call_arguments[0].transfer ==
           QTT_CALL_BORROW);
    assert(borrow_projects == 1);
    assert(release_projects == 1);
    assert(qtt_anf_verify(lowered_closure).error ==
           QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_calls(
               lowering_semantic, &consume_invoke,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &consume_closure,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    QttFunctionSignature owned_result_signature = {
        .result_type = &string_type,
        .result = {
            .mode = QTT_RESULT_OWNED,
            .type = &string_type,
            .type_id = qtt_type_intern(
                call_types, &string_type, &consume_type_error),
            .representation = QTT_REP_OWNED_HEAP,
        },
    };
    owned_result_signature.contract_fingerprint =
        qtt_signature_contract_fingerprint(
            &owned_result_signature);
    assert(qtt_signature_env_register(
               call_env, 2, "Main.make-string",
               &owned_result_signature, NULL) ==
           QTT_SIGNATURE_ENV_OK);
    QttCoreNode make_string_global = {
        .kind = QTT_CORE_GLOBAL, .type = &string_type,
        .global = {.name = "Main.make-string"}};
    QttCoreNode make_string_call = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &make_string_global}};
    QttCoreNode make_string_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &string_type,
        .lambda = {.body = &make_string_call}};
    QttCoreNode make_string_invoke = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &make_string_closure}};
    lowering_semantic = qtt_semantic_ir_lower_in_env(
        &make_string_invoke, call_env, 2, &ir_error);
    assert(lowering_semantic);
    QttSemanticCallEvidence *owned_call_evidence =
        qtt_semantic_ir_calls(lowering_semantic);
    assert(qtt_semantic_ir_call_count(lowering_semantic) == 1);
    assert(owned_call_evidence[0].result_resource.module_id == 2);
    assert(owned_call_evidence[0].result_resource.binder_id ==
           UINT64_C(0x8000000000000000));
    owned_call_evidence[0].result_resource.binder_id++;
    assert(qtt_semantic_ir_verify(
               lowering_semantic, &make_string_invoke) ==
           QTT_SEMANTIC_IR_CALL_EVIDENCE_MISMATCH);
    owned_call_evidence[0].result_resource.binder_id--;
    lowered_closure = qtt_semantic_anf_lower_closure_body_in_env(
        lowering_semantic, &make_string_closure,
        call_env, 2, &bridge_error, &lower_error);
    assert(lowered_closure);
    QttAnfInstruction *owned_call = NULL;
    QttAnfInstruction *owned_move = NULL;
    for (size_t i = 0;
         i < lowered_closure->blocks[0].instruction_count; i++) {
        QttAnfInstruction *instruction =
            &lowered_closure->blocks[0].instructions[i];
        if (instruction->kind == QTT_ANF_CALL)
            owned_call = instruction;
        if (instruction->kind == QTT_ANF_MOVE)
            owned_move = instruction;
    }
    assert(owned_call && owned_move);
    assert(owned_call->result_mode == QTT_RESULT_OWNED);
    assert(owned_call->result_resource.module_id == 2);
    assert(owned_call->result_resource.binder_id >=
           UINT64_C(0x8000000000000000));
    assert(qtt_core_var_equal(
        owned_call->result_resource, owned_move->resource));
    assert(lowered_closure->blocks[0].terminator.value ==
           owned_move->result);
    assert(qtt_anf_verify(lowered_closure).error ==
           QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_calls(
               lowering_semantic, &make_string_invoke,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    owned_call->result_resource.binder_id++;
    owned_move->resource = owned_call->result_resource;
    assert(qtt_anf_verify(lowered_closure).error ==
           QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_calls(
               lowering_semantic, &make_string_invoke,
               lowered_closure) ==
           QTT_SEMANTIC_ANF_CALL_MISMATCH);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    QttCoreNode ignored_result = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreNode *ignored_owned_items[] = {
        &make_string_call, &ignored_result};
    QttCoreNode ignored_owned_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {.items = ignored_owned_items, .count = 2}};
    QttCoreNode ignored_owned_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {.body = &ignored_owned_body}};
    QttCoreNode ignored_owned_invoke = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &ignored_owned_closure}};
    lowering_semantic = qtt_semantic_ir_lower_in_env(
        &ignored_owned_invoke, call_env, 2, &ir_error);
    assert(lowering_semantic);
    lowered_closure = qtt_semantic_anf_lower_closure_body_in_env(
        lowering_semantic, &ignored_owned_closure,
        call_env, 2, &bridge_error, &lower_error);
    assert(lowered_closure);
    size_t ignored_owned_drops = 0;
    for (size_t i = 0;
         i < lowered_closure->blocks[0].instruction_count; i++)
        ignored_owned_drops +=
            lowered_closure->blocks[0].instructions[i].kind ==
            QTT_ANF_DROP;
    assert(ignored_owned_drops == 1);
    assert(qtt_anf_verify(lowered_closure).error ==
           QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_calls(
               lowering_semantic, &ignored_owned_invoke,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    QttCoreNode owned_condition = {
        .kind = QTT_CORE_GLOBAL, .type = &bool_type,
        .global = {.name = "True"}};
    QttCoreNode owned_then_global = {
        .kind = QTT_CORE_GLOBAL, .type = &string_type,
        .global = {.name = "Main.make-string"}};
    QttCoreNode owned_else_global = {
        .kind = QTT_CORE_GLOBAL, .type = &string_type,
        .global = {.name = "Main.make-string"}};
    QttCoreNode owned_then_call = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &owned_then_global}};
    QttCoreNode owned_else_call = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &owned_else_global}};
    QttCoreNode owned_if = {
        .kind = QTT_CORE_IF, .type = &string_type,
        .conditional = {
            .condition = &owned_condition,
            .then_branch = &owned_then_call,
            .else_branch = &owned_else_call}};
    QttCoreNode owned_if_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &string_type,
        .lambda = {.body = &owned_if}};
    QttCoreNode owned_if_invoke = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &owned_if_closure}};
    lowering_semantic = qtt_semantic_ir_lower_in_env(
        &owned_if_invoke, call_env, 2, &ir_error);
    assert(lowering_semantic);
    lowered_closure = qtt_semantic_anf_lower_closure_body_in_env(
        lowering_semantic, &owned_if_closure,
        call_env, 2, &bridge_error, &lower_error);
    assert(lowered_closure);
    size_t owned_branch_calls = 0;
    size_t owned_branch_moves = 0;
    for (size_t block = 0;
         block < lowered_closure->block_count; block++)
        for (size_t i = 0;
             i < lowered_closure->blocks[block].instruction_count; i++) {
            QttAnfInstruction *instruction =
                &lowered_closure->blocks[block].instructions[i];
            owned_branch_calls +=
                instruction->kind == QTT_ANF_CALL;
            owned_branch_moves +=
                instruction->kind == QTT_ANF_MOVE;
        }
    assert(owned_branch_calls == 2);
    assert(owned_branch_moves == 2);
    assert(qtt_anf_verify(lowered_closure).error ==
           QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_calls(
               lowering_semantic, &owned_if_invoke,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    QttCoreNode *ignored_owned_if_items[] = {
        &owned_if, &ignored_result};
    QttCoreNode ignored_owned_if_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {
            .items = ignored_owned_if_items, .count = 2}};
    QttCoreNode ignored_owned_if_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {.body = &ignored_owned_if_body}};
    QttCoreNode ignored_owned_if_invoke = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &ignored_owned_if_closure}};
    lowering_semantic = qtt_semantic_ir_lower_in_env(
        &ignored_owned_if_invoke, call_env, 2, &ir_error);
    assert(lowering_semantic);
    lowered_closure = qtt_semantic_anf_lower_closure_body_in_env(
        lowering_semantic, &ignored_owned_if_closure,
        call_env, 2, &bridge_error, &lower_error);
    assert(lowered_closure);
    size_t branch_owned_drops = 0;
    for (size_t block = 0;
         block < lowered_closure->block_count; block++)
        for (size_t i = 0;
             i < lowered_closure->blocks[block].instruction_count; i++)
            branch_owned_drops +=
                lowered_closure->blocks[block].instructions[i].kind ==
                QTT_ANF_DROP;
    assert(branch_owned_drops == 2);
    assert(qtt_anf_verify(lowered_closure).error ==
           QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_calls(
               lowering_semantic, &ignored_owned_if_invoke,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    qtt_signature_env_free(call_env);
    qtt_type_arena_free(call_types);
    qtt_signature_free(call_signature);

    QttCoreNode closure_condition = {
        .kind = QTT_CORE_GLOBAL, .type = &bool_type,
        .global = {.name = "True"}};
    QttCoreNode move_then = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode move_else = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode move_if = {
        .kind = QTT_CORE_IF, .type = &string_type,
        .conditional = {.condition = &closure_condition,
                        .then_branch = &move_then,
                        .else_branch = &move_else}};
    QttCoreNode move_if_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &string_type,
        .lambda = {.body = &move_if}};
    QttCoreNode move_if_call = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &move_if_closure}};
    lowering_semantic = qtt_semantic_ir_lower(
        &move_if_call, &ir_error);
    assert(lowering_semantic);
    lowered_closure = qtt_semantic_anf_lower_closure_body(
        lowering_semantic, &move_if_closure,
        &bridge_error, &lower_error);
    assert(lowered_closure);
    assert(lowered_closure->block_count == 4);
    assert(lowered_closure->blocks[0].terminator.kind ==
           QTT_ANF_BRANCH);
    assert(lowered_closure->blocks[0].terminator.has_branch_identity);
    size_t branch_moves = 0;
    for (size_t block = 0; block < lowered_closure->block_count; block++)
        for (size_t i = 0;
             i < lowered_closure->blocks[block].instruction_count; i++) {
            QttAnfInstruction *instruction =
                &lowered_closure->blocks[block].instructions[i];
            if (instruction->kind == QTT_ANF_ENV_PROJECT &&
                instruction->environment_action == QTT_ANF_ENV_MOVE) {
                branch_moves++;
                assert(instruction->control_path_count == 1);
            }
        }
    assert(branch_moves == 2);
    assert(qtt_anf_verify(lowered_closure).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &move_if_closure,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    QttAnfInstruction *tampered_projection = NULL;
    for (size_t block = 0; block < lowered_closure->block_count; block++)
        for (size_t i = 0;
             i < lowered_closure->blocks[block].instruction_count; i++)
            if (lowered_closure->blocks[block].instructions[i].kind ==
                QTT_ANF_ENV_PROJECT) {
                tampered_projection =
                    &lowered_closure->blocks[block].instructions[i];
                break;
            }
    assert(tampered_projection && tampered_projection->control_path);
    tampered_projection->control_path[0].else_arm =
        !tampered_projection->control_path[0].else_arm;
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &move_if_closure,
               lowered_closure) ==
           QTT_SEMANTIC_ANF_PROJECTION_MISMATCH);
    tampered_projection->control_path[0].else_arm =
        !tampered_projection->control_path[0].else_arm;
    lowered_closure->blocks[0].terminator.branch_ordinal = 4;
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &move_if_closure,
               lowered_closure) ==
           QTT_SEMANTIC_ANF_PROJECTION_MISMATCH);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    QttCoreNode nested_move_then = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode nested_move_else = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode nested_move_if = {
        .kind = QTT_CORE_IF, .type = &string_type,
        .conditional = {.condition = &closure_condition,
                        .then_branch = &nested_move_then,
                        .else_branch = &nested_move_else}};
    QttCoreNode outer_move_else = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode nested_outer_if = {
        .kind = QTT_CORE_IF, .type = &string_type,
        .conditional = {.condition = &closure_condition,
                        .then_branch = &nested_move_if,
                        .else_branch = &outer_move_else}};
    QttCoreNode nested_if_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &string_type,
        .lambda = {.body = &nested_outer_if}};
    QttCoreNode nested_if_call = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &nested_if_closure}};
    lowering_semantic = qtt_semantic_ir_lower(
        &nested_if_call, &ir_error);
    assert(lowering_semantic);
    lowered_closure = qtt_semantic_anf_lower_closure_body(
        lowering_semantic, &nested_if_closure,
        &bridge_error, &lower_error);
    assert(lowered_closure);
    assert(lowered_closure->block_count == 7);
    size_t nested_moves = 0, depth_two_moves = 0;
    for (size_t block = 0; block < lowered_closure->block_count; block++)
        for (size_t i = 0;
             i < lowered_closure->blocks[block].instruction_count; i++) {
            QttAnfInstruction *instruction =
                &lowered_closure->blocks[block].instructions[i];
            if (instruction->kind == QTT_ANF_ENV_PROJECT &&
                instruction->environment_action == QTT_ANF_ENV_MOVE) {
                nested_moves++;
                depth_two_moves +=
                    instruction->control_path_count == 2;
            }
        }
    assert(nested_moves == 3);
    assert(depth_two_moves == 2);
    assert(qtt_anf_verify(lowered_closure).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &nested_if_closure,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    QttCoreVar then_local = {.module_id = 2, .binder_id = 73};
    QttCoreVar else_local = {.module_id = 2, .binder_id = 74};
    QttCoreNode branch_let_then_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode branch_let_else_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode branch_let_then_init = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreNode branch_let_else_init = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreNode branch_let_then_var = {
        .kind = QTT_CORE_VAR, .type = &int_type, .var = then_local};
    QttCoreNode branch_let_else_var = {
        .kind = QTT_CORE_VAR, .type = &int_type, .var = else_local};
    QttCoreNode branch_then_let = {
        .kind = QTT_CORE_LET, .type = &int_type,
        .let = {.binding = then_local,
                .value = &branch_let_then_init,
                .body = &branch_let_then_var}};
    QttCoreNode branch_else_let = {
        .kind = QTT_CORE_LET, .type = &int_type,
        .let = {.binding = else_local,
                .value = &branch_let_else_init,
                .body = &branch_let_else_var}};
    QttCoreNode *branch_let_then_items[] = {
        &branch_let_then_use, &branch_then_let};
    QttCoreNode *branch_let_else_items[] = {
        &branch_let_else_use, &branch_else_let};
    QttCoreNode branch_let_then_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {
            .items = branch_let_then_items, .count = 2}};
    QttCoreNode branch_let_else_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {
            .items = branch_let_else_items, .count = 2}};
    QttCoreNode branch_let_if = {
        .kind = QTT_CORE_IF, .type = &int_type,
        .conditional = {
            .condition = &closure_condition,
            .then_branch = &branch_let_then_sequence,
            .else_branch = &branch_let_else_sequence}};
    QttCoreNode branch_let_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {.body = &branch_let_if}};
    QttCoreNode branch_let_call = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &branch_let_closure}};
    lowering_semantic = qtt_semantic_ir_lower(
        &branch_let_call, &ir_error);
    assert(lowering_semantic);
    lowered_closure = qtt_semantic_anf_lower_closure_body(
        lowering_semantic, &branch_let_closure,
        &bridge_error, &lower_error);
    assert(lowered_closure);
    size_t branch_aliases = 0;
    for (size_t block = 0; block < lowered_closure->block_count; block++)
        for (size_t i = 0;
             i < lowered_closure->blocks[block].instruction_count; i++)
            branch_aliases +=
                lowered_closure->blocks[block].instructions[i].kind ==
                QTT_ANF_ALIAS;
    assert(branch_aliases == 2);
    assert(qtt_anf_verify(lowered_closure).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &branch_let_closure,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    QttCoreVar captured_aux = {.module_id = 2, .binder_id = 71};
    QttCoreNode mixed_then_aux = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured_aux};
    QttCoreNode mixed_then_result = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode mixed_else_aux = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured_aux};
    QttCoreNode mixed_else_result = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode *mixed_then_items[] = {
        &mixed_then_aux, &mixed_then_result};
    QttCoreNode *mixed_else_items[] = {
        &mixed_else_aux, &mixed_else_result};
    QttCoreNode mixed_then_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &string_type,
        .sequence = {.items = mixed_then_items, .count = 2}};
    QttCoreNode mixed_else_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &string_type,
        .sequence = {.items = mixed_else_items, .count = 2}};
    QttCoreNode mixed_if = {
        .kind = QTT_CORE_IF, .type = &string_type,
        .conditional = {.condition = &closure_condition,
                        .then_branch = &mixed_then_sequence,
                        .else_branch = &mixed_else_sequence}};
    QttCoreNode mixed_if_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &string_type,
        .lambda = {.body = &mixed_if}};
    QttCoreNode mixed_if_call = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &mixed_if_closure}};
    lowering_semantic = qtt_semantic_ir_lower(
        &mixed_if_call, &ir_error);
    assert(lowering_semantic);
    assert(qtt_semantic_ir_closure_field_count(
               lowering_semantic) == 2);
    lowered_closure = qtt_semantic_anf_lower_closure_body(
        lowering_semantic, &mixed_if_closure,
        &bridge_error, &lower_error);
    assert(lowered_closure);
    assert(lowered_closure->environment_slot_count == 2);
    size_t mixed_moves = 0, mixed_borrows = 0, mixed_releases = 0;
    for (size_t block = 0; block < lowered_closure->block_count; block++)
        for (size_t i = 0;
             i < lowered_closure->blocks[block].instruction_count; i++) {
            QttAnfInstruction *instruction =
                &lowered_closure->blocks[block].instructions[i];
            if (instruction->kind != QTT_ANF_ENV_PROJECT) continue;
            mixed_moves += instruction->environment_action ==
                QTT_ANF_ENV_MOVE;
            mixed_borrows += instruction->environment_action ==
                QTT_ANF_ENV_BORROW;
            mixed_releases += instruction->environment_action ==
                QTT_ANF_ENV_RELEASE;
        }
    assert(mixed_moves == 2);
    assert(mixed_borrows == 2);
    assert(mixed_releases == 1);
    assert(qtt_anf_verify(lowered_closure).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &mixed_if_closure,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    QttCoreVar forbidden_alias = {
        .module_id = 2, .binder_id = 75};
    QttCoreNode forbidden_alias_value = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode forbidden_alias_result = {
        .kind = QTT_CORE_VAR, .type = &string_type,
        .var = forbidden_alias};
    QttCoreNode forbidden_alias_let = {
        .kind = QTT_CORE_LET, .type = &string_type,
        .let = {.binding = forbidden_alias,
                .value = &forbidden_alias_value,
                .body = &forbidden_alias_result}};
    QttCoreNode forbidden_alias_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &string_type,
        .lambda = {.body = &forbidden_alias_let}};
    QttCoreNode forbidden_alias_call = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &forbidden_alias_closure}};
    lowering_semantic = qtt_semantic_ir_lower(
        &forbidden_alias_call, &ir_error);
    assert(lowering_semantic);
    QttSemanticClosureFieldEvidence *forbidden_fields =
        qtt_semantic_ir_closure_fields(lowering_semantic);
    assert(forbidden_fields[0].storage ==
           QTT_CLOSURE_STORAGE_SHARED);
    lowered_closure = qtt_semantic_anf_lower_closure_body(
        lowering_semantic, &forbidden_alias_closure,
        &bridge_error, &lower_error);
    assert(!lowered_closure);
    assert(bridge_error != QTT_SEMANTIC_ANF_OK);
    qtt_semantic_ir_free(lowering_semantic);

    QttCoreNode release_then_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode release_else_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode release_then_result = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreNode release_else_result = {
        .kind = QTT_CORE_LITERAL, .type = &int_type,
        .literal = {.source = &number_ast}};
    QttCoreNode *release_then_items[] = {
        &release_then_use, &release_then_result};
    QttCoreNode *release_else_items[] = {
        &release_else_use, &release_else_result};
    QttCoreNode release_then_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {.items = release_then_items, .count = 2}};
    QttCoreNode release_else_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {.items = release_else_items, .count = 2}};
    QttCoreNode release_if = {
        .kind = QTT_CORE_IF, .type = &int_type,
        .conditional = {.condition = &closure_condition,
                        .then_branch = &release_then_sequence,
                        .else_branch = &release_else_sequence}};
    QttCoreNode release_if_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {.body = &release_if}};
    QttCoreNode release_if_call = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &release_if_closure}};
    lowering_semantic = qtt_semantic_ir_lower(
        &release_if_call, &ir_error);
    assert(lowering_semantic);
    lowered_closure = qtt_semantic_anf_lower_closure_body(
        lowering_semantic, &release_if_closure,
        &bridge_error, &lower_error);
    assert(lowered_closure);
    size_t branch_borrows = 0, joined_releases = 0;
    for (size_t block = 0; block < lowered_closure->block_count; block++)
        for (size_t i = 0;
             i < lowered_closure->blocks[block].instruction_count; i++) {
            QttAnfInstruction *instruction =
                &lowered_closure->blocks[block].instructions[i];
            if (instruction->kind != QTT_ANF_ENV_PROJECT) continue;
            branch_borrows += instruction->environment_action ==
                QTT_ANF_ENV_BORROW;
            joined_releases += instruction->environment_action ==
                QTT_ANF_ENV_RELEASE;
        }
    assert(branch_borrows == 2);
    assert(joined_releases == 1);
    assert(qtt_anf_verify(lowered_closure).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_projections(
               lowering_semantic, &release_if_closure,
               lowered_closure) == QTT_SEMANTIC_ANF_OK);
    qtt_anf_program_free(lowered_closure);
    qtt_semantic_ir_free(lowering_semantic);

    anf->environment_slots[0].ordinal = 1;
    assert(qtt_semantic_anf_verify_environment(
               semantic, &closure, anf) ==
           QTT_SEMANTIC_ANF_ENVIRONMENT_MISMATCH);
    anf->environment_slots[0].ordinal = 0;
    anf->environment_slots[0].capability =
        QTT_SEMANTIC_CAPABILITY_SHARED;
    assert(qtt_semantic_anf_verify_environment(
               semantic, &closure, anf) ==
           QTT_SEMANTIC_ANF_ENVIRONMENT_MISMATCH);
    qtt_anf_program_free(anf);
    qtt_semantic_ir_free(semantic);

    semantic = qtt_semantic_ir_lower(&closure, &ir_error);
    assert(semantic);
    anf = calloc(1, sizeof(*anf));
    assert(anf);
    assert(qtt_semantic_anf_materialize_environment(
               semantic, &closure, anf) == QTT_SEMANTIC_ANF_OK);
    assert(anf->environment_slot_count == 1);
    assert(anf->environment_slots[0].capability ==
           QTT_SEMANTIC_CAPABILITY_SHARED);
    assert(anf->environment_slots[0].exit ==
           QTT_CLOSURE_FIELD_RELEASE);
    lowered_closure = qtt_semantic_anf_lower_closure_body(
        semantic, &closure, &bridge_error, &lower_error);
    assert(!lowered_closure);
    assert(bridge_error == QTT_SEMANTIC_ANF_PROJECTION_MISMATCH);
    assert(qtt_semantic_anf_materialize_environment(
               semantic, &closure, anf) ==
           QTT_SEMANTIC_ANF_ENVIRONMENT_MISMATCH);
    anf->environment_slots[0].destructor_id.plan_fingerprint++;
    assert(qtt_semantic_anf_verify_environment(
               semantic, &closure, anf) ==
           QTT_SEMANTIC_ANF_ENVIRONMENT_MISMATCH);
    qtt_anf_program_free(anf);
    qtt_semantic_ir_free(semantic);

    Type projected_string = {.kind = TYPE_STRING};
    LayoutField projected_fields[] = {
        {.name = "name", .type = &projected_string},
    };
    Type projected_layout = {
        .kind = TYPE_LAYOUT,
        .layout_fields = projected_fields,
        .layout_field_count = 1,
    };
    AST projected_literal_ast = {
        .type = AST_STRING, .string = "Ada",
        .inferred_type = &projected_layout};
    QttCoreVar projected_root = {.module_id = 1, .binder_id = 95};
    QttCoreNode projected_initializer = {
        .kind = QTT_CORE_LITERAL, .type = &projected_layout,
        .literal = {.source = &projected_literal_ast},
    };
    QttPlace projected_name = {0};
    assert(qtt_place_layout_field(
        qtt_place_root(projected_root), &projected_layout,
        "name", &projected_name));
    QttCoreNode projected_use = {
        .kind = QTT_CORE_PLACE, .type = &projected_string,
        .place = projected_name,
    };
    QttCoreNode projected_let = {
        .kind = QTT_CORE_LET, .type = &projected_string,
        .let = {
            .binding = projected_root,
            .value = &projected_initializer,
            .body = &projected_use,
        },
    };
    semantic = qtt_semantic_ir_lower(&projected_let, &ir_error);
    assert(semantic);
    anf = qtt_semantic_anf_lower(
        semantic, &projected_let, &bridge_error, &lower_error);
    assert(anf && bridge_error == QTT_SEMANTIC_ANF_OK &&
           lower_error == QTT_ANF_LOWER_OK);
    QttAnfInstruction *projected_move = NULL;
    for (size_t block = 0; block < anf->block_count; block++)
        for (size_t i = 0; i < anf->blocks[block].instruction_count; i++)
            if (anf->blocks[block].instructions[i].kind ==
                QTT_ANF_MOVE_PLACE)
                projected_move = &anf->blocks[block].instructions[i];
    assert(projected_move && qtt_place_equal(
        projected_move->place, projected_name));
    assert(projected_move->result_type == QTT_ANF_TYPE_STRING);
    assert(qtt_anf_verify(anf).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_correspondence(
        semantic, &projected_let, anf) == QTT_SEMANTIC_ANF_OK);
    size_t projected_ordinal = projected_move->place.projection_path[0];
    projected_move->place.projection_path[0] = projected_ordinal + 1;
    assert(qtt_semantic_anf_verify_correspondence(
        semantic, &projected_let, anf) ==
        QTT_SEMANTIC_ANF_OWNERSHIP_MISMATCH);
    projected_move->place.projection_path[0] = projected_ordinal;
    QttAnfEvaluation projected_evaluation = qtt_anf_evaluate(anf);
    assert(projected_evaluation.error == QTT_ANF_EVAL_OK);
    assert(projected_evaluation.heap.projected_moves == 1);
    assert(projected_evaluation.heap.dropped == 1);
    qtt_anf_program_free(anf);
    qtt_semantic_ir_free(semantic);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "semantic_anf.c"
            executable = directory / "semantic_anf"
            harness.write_text(source)
            subprocess.run([
                "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                "-iquote", str(ROOT / "src"), str(harness),
                *[str(ROOT / "src" / "qtt" / name) for name in [
                    "core.c", "quantity.c", "demand.c", "graded.c",
                    "resource.c", "eval.c", "signature.c", "call.c",
                    "type_identity.c", "signature_env.c", "constraints.c",
                    "environment.c", "elaboration.c", "core_usage.c",
                    "../effects/effect.c", "drop.c", "closure_policy.c",
                    "semantic_ir.c", "anf.c",
                    "semantic_anf.c",
                ]],
                "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
