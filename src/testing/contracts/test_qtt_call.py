"""Checked ownership transfer at function call boundaries."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class QttCallTests(unittest.TestCase):
    def test_consumed_argument_requires_matching_move(self):
        source = r'''
#include "qtt/anf.h"
#include "qtt/call.h"
#include <assert.h>

int main(void) {
    Type string_type = {.kind = TYPE_STRING};
    QttParameterContract parameter = {
        .quantity = {.finite = 1},
        .mode = QTT_OWNERSHIP_CONSUMED,
        .type = &string_type,
        .representation = QTT_REP_OWNED_HEAP,
    };
    QttFunctionSignature signature = {
        .parameters = &parameter,
        .parameter_count = 1,
        .result_type = &string_type,
    };
    QttCallArgument moved = {
        .type = &string_type,
        .representation = QTT_REP_OWNED_HEAP,
        .transfer = QTT_CALL_MOVE,
    };
    QttCallPlan plan = {0};
    assert(qtt_call_plan(&signature, &moved, 1, &plan) ==
           QTT_CALL_VALID);
    assert(plan.argument_count == 1);
    assert(plan.arguments[0].transfer == QTT_CALL_MOVE);
    assert(plan.result_type == &string_type);
    assert(plan.result_mode == QTT_RESULT_OWNED);
    assert(plan.result_representation == QTT_REP_OWNED_HEAP);
    qtt_call_plan_free(&plan);

    QttTypeArena *arena = qtt_type_arena_new();
    assert(arena);
    assert(qtt_signature_canonicalize(&signature, arena) ==
           QTT_SIGNATURE_CANONICAL);
    moved.type_id = signature.parameters[0].type_id;
    assert(qtt_call_plan(&signature, &moved, 1, &plan) ==
           QTT_CALL_VALID);
    qtt_call_plan_free(&plan);
    Type other_integer = {.kind = TYPE_INT};
    QttTypeIdentityError identity_error = QTT_TYPE_IDENTITY_OK;
    moved.type_id = qtt_type_intern(arena, &other_integer, &identity_error);
    assert(qtt_call_plan(&signature, &moved, 1, &plan) ==
           QTT_CALL_TYPE_MISMATCH);
    moved.type_id = signature.parameters[0].type_id;

    moved.transfer = QTT_CALL_BORROW;
    assert(qtt_call_plan(&signature, &moved, 1, &plan) ==
           QTT_CALL_TRANSFER_MISMATCH);
    assert(qtt_call_plan(&signature, &moved, 0, &plan) ==
           QTT_CALL_ARITY_MISMATCH);

    Type integer_type = {.kind = TYPE_INT};
    QttParameterContract immediate_parameter = {
        .quantity = {.finite = 1},
        .mode = QTT_OWNERSHIP_CONSUMED,
        .type = &integer_type,
        .representation = QTT_REP_IMMEDIATE,
    };
    QttFunctionSignature immediate_signature = {
        .parameters = &immediate_parameter,
        .parameter_count = 1,
        .result_type = &integer_type,
    };
    QttCallArgument immediate = {
        .type = &integer_type,
        .representation = QTT_REP_IMMEDIATE,
        .transfer = QTT_CALL_VALUE,
    };
    assert(qtt_call_plan(
               &immediate_signature, &immediate, 1, &plan) ==
           QTT_CALL_VALID);
    assert(plan.arguments[0].transfer == QTT_CALL_VALUE);
    qtt_call_plan_free(&plan);
    immediate.transfer = QTT_CALL_MOVE;
    assert(qtt_call_plan(
               &immediate_signature, &immediate, 1, &plan) ==
           QTT_CALL_TRANSFER_MISMATCH);

    moved.transfer = QTT_CALL_MOVE;
    moved.type = &integer_type;
    moved.representation = QTT_REP_IMMEDIATE;
    assert(qtt_call_plan(&signature, &moved, 1, &plan) ==
           QTT_CALL_TYPE_MISMATCH);

    QttCoreVar inner_parameter = {.module_id = 9, .binder_id = 1};
    QttCoreNode inner_body = {
        .kind = QTT_CORE_VAR,
        .type = &string_type,
        .var = inner_parameter,
    };
    QttCoreNode inner_lambda = {
        .kind = QTT_CORE_LAMBDA,
        .type = &string_type,
        .lambda = {
            .params = &inner_parameter,
            .param_count = 1,
            .body = &inner_body,
        },
    };
    AST text_ast = {.type = AST_STRING, .string = "transfer"};
    QttCoreNode text = {
        .kind = QTT_CORE_LITERAL,
        .type = &string_type,
        .literal = {.source = &text_ast},
    };
    QttCoreNode *call_arguments[] = {&text};
    QttCoreNode direct_call = {
        .kind = QTT_CORE_APPLY,
        .type = &string_type,
        .apply = {
            .callee = &inner_lambda,
            .arguments = call_arguments,
            .argument_count = 1,
        },
    };
    QttAnfLowerError lower_error = QTT_ANF_LOWER_OK;
    QttAnfProgram *lowered =
        qtt_anf_lower_core(&direct_call, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(lowered->blocks[0].instruction_count == 3);
    assert(lowered->blocks[0].instructions[0].kind ==
           QTT_ANF_CONST_STRING);
    assert(lowered->blocks[0].instructions[1].kind == QTT_ANF_ALLOC);
    assert(lowered->blocks[0].instructions[2].kind == QTT_ANF_MOVE);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    qtt_anf_program_free(lowered);

    QttCoreVar caller_owned = {.module_id = 9, .binder_id = 2};
    QttCoreNode caller_use = {
        .kind = QTT_CORE_VAR,
        .type = &string_type,
        .var = caller_owned,
    };
    call_arguments[0] = &caller_use;
    QttCoreNode lexical_call = {
        .kind = QTT_CORE_APPLY,
        .type = &string_type,
        .apply = {
            .callee = &inner_lambda,
            .arguments = call_arguments,
            .argument_count = 1,
        },
    };
    QttCoreNode caller = {
        .kind = QTT_CORE_LET,
        .type = &string_type,
        .let = {
            .binding = caller_owned,
            .value = &text,
            .body = &lexical_call,
        },
    };
    lowered = qtt_anf_lower_core(&caller, &lower_error);
    assert(lowered && lower_error == QTT_ANF_LOWER_OK);
    assert(lowered->blocks[0].instruction_count == 5);
    assert(lowered->blocks[0].instructions[1].kind == QTT_ANF_ALLOC);
    assert(lowered->blocks[0].instructions[2].kind == QTT_ANF_MOVE);
    assert(lowered->blocks[0].instructions[3].kind == QTT_ANF_ALLOC);
    assert(lowered->blocks[0].instructions[4].kind == QTT_ANF_MOVE);
    assert(qtt_anf_verify(lowered).error == QTT_ANF_VALID);
    qtt_anf_program_free(lowered);
    qtt_type_arena_free(arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_call_test.c"
            executable = directory / "qtt_call_test"
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
                    str(ROOT / "src" / "qtt" / "signature.c"),
                    str(ROOT / "src" / "effects" / "effect.c"),
                    str(ROOT / "src" / "qtt" / "place.c"),
                    str(ROOT / "src" / "qtt" / "call.c"),
                    str(ROOT / "src" / "qtt" / "type_identity.c"),
                    str(ROOT / "src" / "qtt" / "signature_env.c"),
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
