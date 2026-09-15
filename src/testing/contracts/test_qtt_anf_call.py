"""Explicit ownership CALL instructions are checked against module signatures."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class QttAnfCallTests(unittest.TestCase):
    def test_call_verifier_checks_transfer_provenance_and_owned_result(self):
        source = r'''
#include "qtt/anf.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    Type string_type = {.kind = TYPE_STRING};
    const Type *parameter_types[] = {&string_type};
    QttCoreVar parameter = {.module_id = 23, .binder_id = 1};
    QttCoreNode body = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = parameter};
    QttCoreNode lambda = {
        .kind = QTT_CORE_LAMBDA,
        .type = &string_type,
        .lambda = {
            .params = &parameter,
            .param_types = parameter_types,
            .param_count = 1,
            .body = &body,
        },
    };
    QttSignatureError signature_error = QTT_SIGNATURE_OK;
    QttFunctionSignature *signature =
        qtt_signature_derive(&lambda, &signature_error);
    QttTypeArena *types = qtt_type_arena_new();
    QttSignatureEnv *signatures = qtt_signature_env_new(types);
    assert(signature && types && signatures);
    assert(qtt_signature_canonicalize(signature, types) ==
           QTT_SIGNATURE_CANONICAL);
    assert(signature->contract_fingerprint != 0);
    QttCallableId callable = {0};
    assert(qtt_signature_env_register(
        signatures, 23, "Main.consume", signature, &callable) ==
        QTT_SIGNATURE_ENV_OK);

    QttCoreVar caller_owned = {.module_id = 23, .binder_id = 2};
    QttCoreVar call_result = {.module_id = 23, .binder_id = 3};
    QttAnfCallOperand call_argument = {
        .value = 2,
        .type = QTT_ANF_TYPE_STRING,
        .type_id = signature->parameters[0].type_id,
        .representation = QTT_REP_OWNED_HEAP,
        .transfer = QTT_CALL_MOVE,
        .source_resource = caller_owned,
    };
    QttAnfInstruction operations[7] = {
        qtt_anf_const_string(1, "owned"),
        qtt_anf_bind(caller_owned, QTT_REP_OWNED_HEAP, 1),
        qtt_anf_move_value(2, caller_owned),
        qtt_anf_call(
            3, call_result, callable, &call_argument, 1,
            QTT_ANF_TYPE_STRING, signature->result.type_id,
            QTT_RESULT_OWNED, QTT_REP_OWNED_HEAP),
        qtt_anf_move_value(4, call_result),
    };
    operations[2].result_type = QTT_ANF_TYPE_STRING;
    operations[3].contract_fingerprint =
        signature->contract_fingerprint;
    operations[4].result_type = QTT_ANF_TYPE_STRING;
    QttAnfBlock block = {
        .instructions = operations,
        .instruction_count = 5,
        .terminator = qtt_anf_return(4),
    };
    QttAnfProgram program = {
        .blocks = &block,
        .block_count = 1,
        .signatures = signatures,
    };
    assert(qtt_anf_verify(&program).error == QTT_ANF_VALID);

    call_argument.source_resource.binder_id = 99;
    assert(qtt_anf_verify(&program).error ==
           QTT_ANF_CALL_PROVENANCE_MISMATCH);
    call_argument.source_resource = caller_owned;

    operations[3].contract_fingerprint ^= 1;
    assert(qtt_anf_verify(&program).error ==
           QTT_ANF_CALL_CONTRACT_MISMATCH);
    operations[3].contract_fingerprint ^=
        1;

    call_argument.transfer = QTT_CALL_BORROW;
    call_argument.loan_id = 101;
    assert(qtt_anf_verify(&program).error == QTT_ANF_CALL_TRANSFER_MISMATCH);
    call_argument.transfer = QTT_CALL_MOVE;
    call_argument.value = 1;
    assert(qtt_anf_verify(&program).error == QTT_ANF_CALL_PROVENANCE_MISMATCH);
    call_argument.value = 2;
    call_argument.type = QTT_ANF_TYPE_NUMBER;
    assert(qtt_anf_verify(&program).error == QTT_ANF_CALL_TYPE_MISMATCH);
    call_argument.type = QTT_ANF_TYPE_STRING;
    operations[3].result_type = QTT_ANF_TYPE_NUMBER;
    assert(qtt_anf_verify(&program).error == QTT_ANF_CALL_RESULT_MISMATCH);
    operations[3].result_type = QTT_ANF_TYPE_STRING;
    operations[3].call_arguments = NULL;
    assert(qtt_anf_verify(&program).error == QTT_ANF_CALL_TYPE_MISMATCH);
    operations[3].call_arguments = &call_argument;
    operations[3].callable.name = "Main.missing";
    assert(qtt_anf_verify(&program).error == QTT_ANF_UNKNOWN_CALLABLE);

    /* A borrowed call proves a live, non-transferred owner.  The loan value
     * cannot justify a call after that owner has been dropped. */
    signature->parameters[0].mode = QTT_OWNERSHIP_BORROWED;
    signature->contract_fingerprint =
        qtt_signature_contract_fingerprint(signature);
    call_argument.transfer = QTT_CALL_BORROW;
    call_argument.value = 2;
    call_argument.type = QTT_ANF_TYPE_STRING;
    call_argument.source_resource = caller_owned;
    operations[2] = qtt_anf_borrow_loan(
        2, caller_owned, 101, QTT_LOAN_SHARED);
    operations[2].result_type = QTT_ANF_TYPE_STRING;
    operations[3] = qtt_anf_call(
        3, call_result, callable, &call_argument, 1,
        QTT_ANF_TYPE_STRING, signature->result.type_id,
        QTT_RESULT_OWNED, QTT_REP_OWNED_HEAP);
    operations[3].contract_fingerprint =
        signature->contract_fingerprint;
    operations[4] = qtt_anf_end_borrow(101);
    operations[5] = qtt_anf_drop(caller_owned);
    operations[6] = qtt_anf_move_value(4, call_result);
    operations[6].result_type = QTT_ANF_TYPE_STRING;
    block.instruction_count = 7;
    assert(qtt_anf_verify(&program).error == QTT_ANF_VALID);

    QttAnfInstruction saved_call = operations[3];
    operations[3] = operations[5];
    operations[5] = saved_call;
    assert(qtt_anf_verify(&program).error ==
           QTT_ANF_INVALID_RESOURCE);
    operations[5] = operations[3];
    operations[3] = saved_call;

    /* Borrowed function-entry resources are usable but not owned: they may
     * remain live at return and cannot be moved or dropped by the callee. */
    QttCoreVar borrowed_entry = {.module_id = 23, .binder_id = 40};
    QttAnfValue entry_parameter = 10;
    QttAnfType entry_type = QTT_ANF_TYPE_STRING;
    QttRepresentation entry_representation = QTT_REP_OWNED_HEAP;
    bool entry_owned = false;
    QttAnfInstruction entry_operations[] = {
        qtt_anf_borrow_loan(
            11, borrowed_entry, 201, QTT_LOAN_SHARED),
        qtt_anf_end_borrow(201),
        qtt_anf_const_number(12, 0),
    };
    entry_operations[0].result_type = QTT_ANF_TYPE_STRING;
    QttAnfBlock entry_block = {
        .parameters = &entry_parameter,
        .parameter_types = &entry_type,
        .parameter_count = 1,
        .instructions = entry_operations,
        .instruction_count = 3,
        .terminator = qtt_anf_return(12),
    };
    QttAnfProgram entry_program = {
        .blocks = &entry_block,
        .block_count = 1,
        .initial_resources = &borrowed_entry,
        .initial_representations = &entry_representation,
        .initial_resource_owned = &entry_owned,
        .initial_resource_count = 1,
    };
    assert(qtt_anf_verify(&entry_program).error == QTT_ANF_VALID);
    entry_block.terminator = qtt_anf_return(11);
    assert(qtt_anf_verify(&entry_program).error == QTT_ANF_INVALID_LOAN);
    entry_block.terminator = qtt_anf_return(12);
    entry_operations[0] = qtt_anf_move_value(11, borrowed_entry);
    entry_operations[0].result_type = QTT_ANF_TYPE_STRING;
    assert(qtt_anf_verify(&entry_program).error == QTT_ANF_INVALID_RESOURCE);

    QttCoreVar loan_owner = {.module_id = 23, .binder_id = 45};
    QttAnfInstruction loan_operations[] = {
        qtt_anf_const_string(20, "loans"),
        qtt_anf_bind(loan_owner, QTT_REP_OWNED_HEAP, 20),
        qtt_anf_borrow_loan(21, loan_owner, 301, QTT_LOAN_SHARED),
        qtt_anf_borrow_loan(22, loan_owner, 302, QTT_LOAN_SHARED),
        qtt_anf_end_borrow(302),
        qtt_anf_end_borrow(301),
        qtt_anf_drop(loan_owner),
        qtt_anf_const_number(23, 0),
    };
    loan_operations[2].result_type = QTT_ANF_TYPE_STRING;
    loan_operations[3].result_type = QTT_ANF_TYPE_STRING;
    QttAnfBlock loan_block = {
        .instructions = loan_operations,
        .instruction_count = 8,
        .terminator = qtt_anf_return(23),
    };
    QttAnfProgram loan_program = {
        .blocks = &loan_block, .block_count = 1,
    };
    assert(qtt_anf_verify(&loan_program).error == QTT_ANF_VALID);
    loan_operations[3] = qtt_anf_borrow_loan(
        22, loan_owner, 302, QTT_LOAN_EXCLUSIVE);
    loan_operations[3].result_type = QTT_ANF_TYPE_STRING;
    assert(qtt_anf_verify(&loan_program).error == QTT_ANF_INVALID_LOAN);

    /* Loan contexts are part of control-flow state: every predecessor of a
     * join must agree on the same active loan IDs, roots, and kinds. */
    QttCoreVar branch_owner = {.module_id = 23, .binder_id = 46};
    QttAnfInstruction branch_entry_ops[] = {
        qtt_anf_const_string(30, "branch-loan"),
        qtt_anf_bind(branch_owner, QTT_REP_OWNED_HEAP, 30),
        qtt_anf_const_bool(31, true),
        qtt_anf_borrow_loan(
            32, branch_owner, 401, QTT_LOAN_SHARED),
    };
    branch_entry_ops[3].result_type = QTT_ANF_TYPE_STRING;
    QttAnfInstruction branch_end_ops[] = {
        qtt_anf_end_borrow(401),
    };
    QttAnfInstruction branch_join_ops[] = {
        qtt_anf_drop(branch_owner),
        qtt_anf_const_number(33, 0),
    };
    QttAnfBlock branch_blocks[] = {
        {.instructions = branch_entry_ops, .instruction_count = 4,
         .terminator = qtt_anf_branch(31, 1, 2)},
        {.instructions = branch_end_ops, .instruction_count = 1,
         .terminator = qtt_anf_jump(3, NULL, 0)},
        {.terminator = qtt_anf_jump(3, NULL, 0)},
        {.instructions = branch_join_ops, .instruction_count = 2,
         .terminator = qtt_anf_return(33)},
    };
    QttAnfProgram branch_program = {
        .blocks = branch_blocks, .block_count = 4,
    };
    assert(qtt_anf_verify(&branch_program).error ==
           QTT_ANF_OWNERSHIP_JOIN_MISMATCH);

    /* Borrowed SSA provenance is an explicit block-parameter contract.  A
     * jump may carry the value while its loan is live, but the destination
     * must retain that identity and reject use after the endpoint. */
    QttCoreVar region_owner = {.module_id = 23, .binder_id = 47};
    QttAnfInstruction region_entry_ops[] = {
        qtt_anf_const_string(50, "region"),
        qtt_anf_bind(region_owner, QTT_REP_OWNED_HEAP, 50),
        qtt_anf_borrow_loan(
            51, region_owner, 501, QTT_LOAN_SHARED),
    };
    region_entry_ops[2].result_type = QTT_ANF_TYPE_STRING;
    QttAnfValue region_argument[] = {51};
    QttAnfValue region_parameter[] = {52};
    QttAnfType region_parameter_type[] = {QTT_ANF_TYPE_STRING};
    QttAnfLoanId region_parameter_loan[] = {501};
    QttAnfInstruction region_exit_ops[] = {
        qtt_anf_alias(53, 52),
        qtt_anf_end_borrow(501),
        qtt_anf_drop(region_owner),
        qtt_anf_const_number(54, 0),
    };
    region_exit_ops[0].result_type = QTT_ANF_TYPE_STRING;
    QttAnfBlock region_blocks[] = {
        {.instructions = region_entry_ops, .instruction_count = 3,
         .terminator = qtt_anf_jump(1, region_argument, 1)},
        {.parameters = region_parameter,
         .parameter_types = region_parameter_type,
         .parameter_loans = region_parameter_loan,
         .parameter_count = 1,
         .instructions = region_exit_ops, .instruction_count = 4,
         .terminator = qtt_anf_return(54)},
    };
    QttAnfProgram region_program = {
        .blocks = region_blocks, .block_count = 2,
    };
    assert(qtt_anf_verify(&region_program).error == QTT_ANF_VALID);
    region_parameter_loan[0] = 0;
    assert(qtt_anf_verify(&region_program).error == QTT_ANF_INVALID_LOAN);
    region_parameter_loan[0] = 999;
    assert(qtt_anf_verify(&region_program).error == QTT_ANF_INVALID_LOAN);
    region_parameter_loan[0] = 501;
    region_exit_ops[0] = qtt_anf_end_borrow(501);
    region_exit_ops[1] = qtt_anf_alias(53, 52);
    region_exit_ops[1].result_type = QTT_ANF_TYPE_STRING;
    assert(qtt_anf_verify(&region_program).error == QTT_ANF_INVALID_LOAN);

    /* The compiler can reconstruct edge provenance and place a missing
     * endpoint after the final borrowed use.  The inferred endpoint must
     * precede destruction of the owner and make the ordinary verifier pass. */
    QttAnfBlock *inferred_blocks = calloc(2, sizeof(*inferred_blocks));
    assert(inferred_blocks);
    inferred_blocks[0].instructions = calloc(3, sizeof(QttAnfInstruction));
    inferred_blocks[1].instructions = calloc(3, sizeof(QttAnfInstruction));
    assert(inferred_blocks[0].instructions && inferred_blocks[1].instructions);
    memcpy(inferred_blocks[0].instructions, region_entry_ops,
           3 * sizeof(QttAnfInstruction));
    inferred_blocks[0].instruction_count = 3;
    inferred_blocks[0].terminator = qtt_anf_jump(1, NULL, 0);
    inferred_blocks[0].terminator.arguments = calloc(1, sizeof(QttAnfValue));
    assert(inferred_blocks[0].terminator.arguments);
    inferred_blocks[0].terminator.arguments[0] = 51;
    inferred_blocks[0].terminator.argument_count = 1;
    inferred_blocks[1].parameters = calloc(1, sizeof(QttAnfValue));
    inferred_blocks[1].parameter_types = calloc(1, sizeof(QttAnfType));
    assert(inferred_blocks[1].parameters &&
           inferred_blocks[1].parameter_types);
    inferred_blocks[1].parameters[0] = 52;
    inferred_blocks[1].parameter_types[0] = QTT_ANF_TYPE_STRING;
    inferred_blocks[1].parameter_count = 1;
    inferred_blocks[1].instructions[0] = qtt_anf_alias(53, 52);
    inferred_blocks[1].instructions[0].result_type = QTT_ANF_TYPE_STRING;
    inferred_blocks[1].instructions[1] = qtt_anf_drop(region_owner);
    inferred_blocks[1].instructions[2] = qtt_anf_const_number(54, 0);
    inferred_blocks[1].instruction_count = 3;
    inferred_blocks[1].terminator = qtt_anf_return(54);
    QttAnfProgram *inferred_program = calloc(1, sizeof(*inferred_program));
    assert(inferred_program);
    inferred_program->blocks = inferred_blocks;
    inferred_program->block_count = 2;
    assert(qtt_anf_infer_loan_regions(inferred_program).error ==
           QTT_ANF_VALID);
    assert(inferred_blocks[1].parameter_loans &&
           inferred_blocks[1].parameter_loans[0] == 501);
    assert(inferred_blocks[1].instruction_count == 4);
    assert(inferred_blocks[1].instructions[1].kind == QTT_ANF_END_BORROW);
    assert(inferred_blocks[1].instructions[1].loan_id == 501);
    assert(qtt_anf_verify(inferred_program).error == QTT_ANF_VALID);
    qtt_anf_program_free(inferred_program);

    /* Uses in both arms force the endpoint to their nearest common
     * postdominator, not into either arm or before the branch. */
    QttAnfProgram *diamond = calloc(1, sizeof(*diamond));
    assert(diamond);
    diamond->blocks = calloc(4, sizeof(*diamond->blocks));
    diamond->block_count = 4;
    assert(diamond->blocks);
    diamond->blocks[0].instructions = calloc(4, sizeof(QttAnfInstruction));
    assert(diamond->blocks[0].instructions);
    diamond->blocks[0].instructions[0] = qtt_anf_const_string(60, "diamond");
    diamond->blocks[0].instructions[1] =
        qtt_anf_bind(region_owner, QTT_REP_OWNED_HEAP, 60);
    diamond->blocks[0].instructions[2] = qtt_anf_const_bool(61, true);
    diamond->blocks[0].instructions[3] =
        qtt_anf_borrow_loan(62, region_owner, 502, QTT_LOAN_SHARED);
    diamond->blocks[0].instructions[3].result_type = QTT_ANF_TYPE_STRING;
    diamond->blocks[0].instruction_count = 4;
    diamond->blocks[0].terminator = qtt_anf_branch(61, 1, 2);
    diamond->blocks[0].terminator.arguments = calloc(1, sizeof(QttAnfValue));
    diamond->blocks[0].terminator.else_arguments =
        calloc(1, sizeof(QttAnfValue));
    assert(diamond->blocks[0].terminator.arguments &&
           diamond->blocks[0].terminator.else_arguments);
    diamond->blocks[0].terminator.arguments[0] = 62;
    diamond->blocks[0].terminator.else_arguments[0] = 62;
    diamond->blocks[0].terminator.argument_count = 1;
    diamond->blocks[0].terminator.else_argument_count = 1;
    for (size_t arm = 1; arm <= 2; arm++) {
        diamond->blocks[arm].parameters = calloc(1, sizeof(QttAnfValue));
        diamond->blocks[arm].parameter_types = calloc(1, sizeof(QttAnfType));
        diamond->blocks[arm].instructions = calloc(1, sizeof(QttAnfInstruction));
        assert(diamond->blocks[arm].parameters &&
               diamond->blocks[arm].parameter_types &&
               diamond->blocks[arm].instructions);
        diamond->blocks[arm].parameters[0] = (QttAnfValue)(62 + arm);
        diamond->blocks[arm].parameter_types[0] = QTT_ANF_TYPE_STRING;
        diamond->blocks[arm].parameter_count = 1;
        diamond->blocks[arm].instructions[0] =
            qtt_anf_alias((QttAnfValue)(65 + arm),
                          diamond->blocks[arm].parameters[0]);
        diamond->blocks[arm].instructions[0].result_type = QTT_ANF_TYPE_STRING;
        diamond->blocks[arm].instruction_count = 1;
        diamond->blocks[arm].terminator = qtt_anf_jump(3, NULL, 0);
    }
    diamond->blocks[3].instructions = calloc(2, sizeof(QttAnfInstruction));
    assert(diamond->blocks[3].instructions);
    diamond->blocks[3].instructions[0] = qtt_anf_drop(region_owner);
    diamond->blocks[3].instructions[1] = qtt_anf_const_number(68, 0);
    diamond->blocks[3].instruction_count = 2;
    diamond->blocks[3].terminator = qtt_anf_return(68);
    assert(qtt_anf_infer_loan_regions(diamond).error == QTT_ANF_VALID);
    assert(diamond->blocks[1].parameter_loans[0] == 502);
    assert(diamond->blocks[2].parameter_loans[0] == 502);
    assert(diamond->blocks[3].instructions[0].kind == QTT_ANF_END_BORROW);
    assert(diamond->blocks[3].instructions[0].loan_id == 502);
    assert(qtt_anf_verify(diamond).error == QTT_ANF_VALID);
    qtt_anf_program_free(diamond);

    /* An exclusive parent may be narrowed into an exclusive child.  The
     * parent is suspended until the child ends, then becomes usable again. */
    QttCoreVar reborrow_owner = {.module_id = 23, .binder_id = 48};
    QttPlace reborrow_root = qtt_place_root(reborrow_owner);
    QttPlace reborrow_field = qtt_place_project(reborrow_root, 1);
    QttAnfInstruction reborrow_ops[] = {
        qtt_anf_const_aggregate(70),
        qtt_anf_bind(reborrow_owner, QTT_REP_OWNED_HEAP, 70),
        qtt_anf_borrow_place(
            71, reborrow_root, 601, QTT_LOAN_EXCLUSIVE),
        qtt_anf_reborrow_place(
            72, reborrow_field, 602, 601, QTT_LOAN_EXCLUSIVE),
        qtt_anf_const_number(73, 4),
        qtt_anf_write_place_with_loan(74, reborrow_field, 73, 602),
        qtt_anf_end_borrow(602),
        qtt_anf_alias(75, 71),
        qtt_anf_end_borrow(601),
        qtt_anf_drop(reborrow_owner),
        qtt_anf_const_number(76, 0),
    };
    reborrow_ops[2].result_type = QTT_ANF_TYPE_AGGREGATE;
    reborrow_ops[3].result_type = QTT_ANF_TYPE_NUMBER;
    reborrow_ops[5].result_type = QTT_ANF_TYPE_NUMBER;
    reborrow_ops[7].result_type = QTT_ANF_TYPE_AGGREGATE;
    QttAnfPlaceDeclaration reborrow_place = {
        .place = reborrow_field, .type = QTT_ANF_TYPE_NUMBER};
    QttAnfBlock reborrow_block = {
        .instructions = reborrow_ops,
        .instruction_count = sizeof(reborrow_ops) / sizeof(reborrow_ops[0]),
        .terminator = qtt_anf_return(76),
    };
    QttAnfProgram reborrow_program = {
        .blocks = &reborrow_block, .block_count = 1,
        .mutable_places = &reborrow_place, .mutable_place_count = 1,
    };
    assert(qtt_anf_verify(&reborrow_program).error == QTT_ANF_VALID);

    /* Parent use and parent termination are both forbidden while its child
     * is live. */
    QttAnfInstruction saved_child_end = reborrow_ops[6];
    reborrow_ops[6] = reborrow_ops[7];
    assert(qtt_anf_verify(&reborrow_program).error == QTT_ANF_INVALID_LOAN);
    reborrow_ops[6] = qtt_anf_end_borrow(601);
    assert(qtt_anf_verify(&reborrow_program).error == QTT_ANF_INVALID_LOAN);
    reborrow_ops[6] = saved_child_end;

    /* Reborrows can only narrow the parent's place, and a shared parent
     * cannot manufacture exclusive authority. */
    QttPlace sibling_field = qtt_place_project(reborrow_root, 2);
    reborrow_ops[2] = qtt_anf_borrow_place(
        71, reborrow_field, 601, QTT_LOAN_EXCLUSIVE);
    reborrow_ops[2].result_type = QTT_ANF_TYPE_NUMBER;
    reborrow_ops[3] = qtt_anf_reborrow_place(
        72, sibling_field, 602, 601, QTT_LOAN_EXCLUSIVE);
    reborrow_ops[3].result_type = QTT_ANF_TYPE_NUMBER;
    assert(qtt_anf_verify(&reborrow_program).error == QTT_ANF_INVALID_LOAN);
    reborrow_ops[2] = qtt_anf_borrow_place(
        71, reborrow_root, 601, QTT_LOAN_SHARED);
    reborrow_ops[2].result_type = QTT_ANF_TYPE_AGGREGATE;
    reborrow_ops[3] = qtt_anf_reborrow_place(
        72, reborrow_field, 602, 601, QTT_LOAN_EXCLUSIVE);
    reborrow_ops[3].result_type = QTT_ANF_TYPE_NUMBER;
    assert(qtt_anf_verify(&reborrow_program).error == QTT_ANF_INVALID_LOAN);
    reborrow_ops[3] = qtt_anf_reborrow_place(
        72, reborrow_field, 602, 601, QTT_LOAN_SHARED);
    reborrow_ops[3].result_type = QTT_ANF_TYPE_NUMBER;
    reborrow_ops[5] = qtt_anf_alias(74, 71);
    reborrow_ops[5].result_type = QTT_ANF_TYPE_AGGREGATE;
    assert(qtt_anf_verify(&reborrow_program).error == QTT_ANF_VALID);

    /* Region inference closes the child before the parent, even when neither
     * endpoint was emitted by lowering. */
    QttAnfProgram *inferred_reborrow = calloc(1, sizeof(*inferred_reborrow));
    assert(inferred_reborrow);
    inferred_reborrow->blocks = calloc(1, sizeof(*inferred_reborrow->blocks));
    inferred_reborrow->block_count = 1;
    inferred_reborrow->blocks[0].instructions =
        calloc(8, sizeof(QttAnfInstruction));
    assert(inferred_reborrow->blocks &&
           inferred_reborrow->blocks[0].instructions);
    QttAnfInstruction *inferred_reborrow_ops =
        inferred_reborrow->blocks[0].instructions;
    inferred_reborrow_ops[0] = qtt_anf_const_aggregate(80);
    inferred_reborrow_ops[1] =
        qtt_anf_bind(reborrow_owner, QTT_REP_OWNED_HEAP, 80);
    inferred_reborrow_ops[2] = qtt_anf_borrow_place(
        81, reborrow_root, 701, QTT_LOAN_EXCLUSIVE);
    inferred_reborrow_ops[2].result_type = QTT_ANF_TYPE_AGGREGATE;
    inferred_reborrow_ops[3] = qtt_anf_reborrow_place(
        82, reborrow_field, 702, 701, QTT_LOAN_EXCLUSIVE);
    inferred_reborrow_ops[3].result_type = QTT_ANF_TYPE_NUMBER;
    inferred_reborrow_ops[4] = qtt_anf_const_number(83, 8);
    inferred_reborrow_ops[5] =
        qtt_anf_write_place_with_loan(84, reborrow_field, 83, 702);
    inferred_reborrow_ops[5].result_type = QTT_ANF_TYPE_NUMBER;
    inferred_reborrow_ops[6] = qtt_anf_drop(reborrow_owner);
    inferred_reborrow_ops[7] = qtt_anf_const_number(85, 0);
    inferred_reborrow->blocks[0].instruction_count = 8;
    inferred_reborrow->blocks[0].terminator = qtt_anf_return(85);
    inferred_reborrow->mutable_places =
        calloc(1, sizeof(*inferred_reborrow->mutable_places));
    assert(inferred_reborrow->mutable_places);
    inferred_reborrow->mutable_places[0] = reborrow_place;
    inferred_reborrow->mutable_place_count = 1;
    assert(qtt_anf_infer_loan_regions(inferred_reborrow).error ==
           QTT_ANF_VALID);
    assert(inferred_reborrow->blocks[0].instruction_count == 10);
    assert(inferred_reborrow->blocks[0].instructions[6].kind ==
           QTT_ANF_END_BORROW);
    assert(inferred_reborrow->blocks[0].instructions[6].loan_id == 702);
    assert(inferred_reborrow->blocks[0].instructions[7].kind ==
           QTT_ANF_END_BORROW);
    assert(inferred_reborrow->blocks[0].instructions[7].loan_id == 701);
    assert(qtt_anf_verify(inferred_reborrow).error == QTT_ANF_VALID);
    qtt_anf_program_free(inferred_reborrow);

    /* Projection-indexed loans use prefix overlap: exclusive sibling fields
     * coexist, but an overlapping shared loan and an unauthorized write do
     * not. */
    QttCoreVar aggregate_owner = {.module_id = 23, .binder_id = 47};
    QttPlace field_a = qtt_place_project(
        qtt_place_root(aggregate_owner), 1);
    QttPlace field_b = qtt_place_project(
        qtt_place_root(aggregate_owner), 2);
    QttAnfInstruction place_operations[] = {
        qtt_anf_const_aggregate(40),
        qtt_anf_bind(aggregate_owner, QTT_REP_OWNED_HEAP, 40),
        qtt_anf_borrow_place(
            41, field_a, 501, QTT_LOAN_EXCLUSIVE),
        qtt_anf_borrow_place(
            42, field_b, 502, QTT_LOAN_EXCLUSIVE),
        qtt_anf_const_number(43, 9),
        qtt_anf_write_place_with_loan(44, field_a, 43, 501),
        qtt_anf_end_borrow(502),
        qtt_anf_end_borrow(501),
        qtt_anf_drop(aggregate_owner),
        qtt_anf_const_number(45, 0),
    };
    place_operations[2].result_type = QTT_ANF_TYPE_NUMBER;
    place_operations[3].result_type = QTT_ANF_TYPE_NUMBER;
    place_operations[5].result_type = QTT_ANF_TYPE_NUMBER;
    QttAnfPlaceDeclaration place_declaration = {
        .place = field_a, .type = QTT_ANF_TYPE_NUMBER,
    };
    QttAnfBlock place_block = {
        .instructions = place_operations,
        .instruction_count = 10,
        .terminator = qtt_anf_return(45),
    };
    QttAnfProgram place_program = {
        .blocks = &place_block, .block_count = 1,
        .mutable_places = &place_declaration,
        .mutable_place_count = 1,
    };
    assert(qtt_anf_verify(&place_program).error == QTT_ANF_VALID);
    place_operations[3] = qtt_anf_borrow_place(
        42, field_a, 502, QTT_LOAN_SHARED);
    place_operations[3].result_type = QTT_ANF_TYPE_NUMBER;
    assert(qtt_anf_verify(&place_program).error == QTT_ANF_INVALID_LOAN);
    place_operations[3] = qtt_anf_borrow_place(
        42, field_b, 502, QTT_LOAN_EXCLUSIVE);
    place_operations[3].result_type = QTT_ANF_TYPE_NUMBER;
    place_operations[5] = qtt_anf_write_place_with_loan(
        44, field_a, 43, 502);
    place_operations[5].result_type = QTT_ANF_TYPE_NUMBER;
    assert(qtt_anf_verify(&place_program).error == QTT_ANF_INVALID_LOAN);
    place_operations[3] = qtt_anf_borrow_place(
        42, field_a, 0, QTT_LOAN_SHARED);
    place_operations[3].result_type = QTT_ANF_TYPE_NUMBER;
    assert(qtt_anf_verify(&place_program).error == QTT_ANF_INVALID_LOAN);

    /* Production named-call lowering materializes a temporary owner, lends
     * it to the call, ends the loan at CALL, and reclaims it afterwards. */
    AST literal_ast = {.type = AST_STRING, .string = "temporary"};
    QttCoreNode literal = {
        .kind = QTT_CORE_LITERAL, .type = &string_type,
        .literal = {.source = &literal_ast},
    };
    QttCoreNode global = {
        .kind = QTT_CORE_GLOBAL, .type = &string_type,
        .global = {.name = "Main.consume"},
    };
    QttCoreNode *call_arguments[] = {&literal};
    QttCoreNode named_call = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {
            .callee = &global,
            .arguments = call_arguments,
            .argument_count = 1,
        },
    };
    QttCoreNode caller = {
        .kind = QTT_CORE_LAMBDA, .type = &string_type,
        .lambda = {.body = &named_call},
    };
    QttSignatureError caller_error = QTT_SIGNATURE_OK;
    QttFunctionSignature *caller_signature =
        qtt_signature_derive(&caller, &caller_error);
    assert(caller_signature);
    assert(qtt_signature_canonicalize(caller_signature, types) ==
           QTT_SIGNATURE_CANONICAL);
    QttAnfLowerError lower_error = QTT_ANF_LOWER_OK;
    QttAnfProgram *lowered = qtt_anf_lower_function_in_env(
        &caller, caller_signature, signatures, 23, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    size_t borrow_at = SIZE_MAX, call_at = SIZE_MAX, drop_at = SIZE_MAX;
    for (size_t i = 0; i < lowered->blocks[0].instruction_count; i++) {
        QttAnfInstructionKind kind = lowered->blocks[0].instructions[i].kind;
        if (kind == QTT_ANF_BORROW) borrow_at = i;
        if (kind == QTT_ANF_CALL) call_at = i;
        if (kind == QTT_ANF_DROP && drop_at == SIZE_MAX) drop_at = i;
    }
    assert(borrow_at < call_at && call_at < drop_at);
    qtt_anf_program_free(lowered);
    qtt_signature_free(caller_signature);

    QttCoreVar local_owner = {.module_id = 23, .binder_id = 50};
    QttCoreNode local_argument = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = local_owner,
    };
    call_arguments[0] = &local_argument;
    QttCoreNode caller_let = {
        .kind = QTT_CORE_LET, .type = &string_type,
        .let = {
            .binding = local_owner,
            .value = &literal,
            .body = &named_call,
        },
    };
    caller.lambda.body = &caller_let;
    caller_signature = qtt_signature_derive(&caller, &caller_error);
    assert(caller_signature);
    assert(qtt_signature_canonicalize(caller_signature, types) ==
           QTT_SIGNATURE_CANONICAL);
    lowered = qtt_anf_lower_function_in_env(
        &caller, caller_signature, signatures, 23, &lower_error);
    assert(lowered && qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    borrow_at = call_at = drop_at = SIZE_MAX;
    for (size_t i = 0; i < lowered->blocks[0].instruction_count; i++) {
        QttAnfInstruction *instruction =
            &lowered->blocks[0].instructions[i];
        if (instruction->kind == QTT_ANF_BORROW) borrow_at = i;
        if (instruction->kind == QTT_ANF_CALL) call_at = i;
        if (instruction->kind == QTT_ANF_DROP &&
            qtt_core_var_equal(instruction->resource, local_owner))
            drop_at = i;
    }
    assert(borrow_at < call_at && call_at < drop_at);
    qtt_anf_program_free(lowered);
    qtt_signature_free(caller_signature);

    qtt_signature_env_free(signatures);
    qtt_type_arena_free(types);
    qtt_signature_free(signature);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_anf_call_test.c"
            executable = directory / "qtt_anf_call_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT / "src"), str(harness),
                    str(ROOT / "src" / "qtt" / "core.c"),
                    str(ROOT / "src" / "qtt" / "quantity.c"),
                    str(ROOT / "src" / "qtt" / "demand.c"),
                    str(ROOT / "src" / "qtt" / "graded.c"),
                    str(ROOT / "src" / "qtt" / "resource.c"),
                    str(ROOT / "src" / "qtt" / "type_identity.c"),
                    str(ROOT / "src" / "qtt" / "signature.c"),
                    str(ROOT / "src" / "effects" / "effect.c"),
                    str(ROOT / "src" / "qtt" / "place.c"),
                    str(ROOT / "src" / "qtt" / "signature_env.c"),
                    str(ROOT / "src" / "qtt" / "call.c"),
                    str(ROOT / "src" / "qtt" / "anf.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
