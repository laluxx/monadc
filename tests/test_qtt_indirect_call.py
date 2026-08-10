"""Ownership ANF for statically known lexical callable aliases."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class QttIndirectCallTests(unittest.TestCase):
    def test_capture_free_lexical_callable_is_verified_but_unknown_is_not(self):
        source = r'''
#include "qtt/anf.h"
#include "qtt/semantic_ir.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    Type string = {.kind = TYPE_STRING};
    Type arrow = {.kind = TYPE_ARROW,
                  .arrow_param = &string, .arrow_ret = &string};
    QttCoreVar parameter = {71, 1};
    const Type *parameter_types[] = {&string};
    QttCoreNode parameter_use = {
        .kind = QTT_CORE_VAR, .type = &string, .var = parameter};
    QttCoreNode identity = {
        .kind = QTT_CORE_LAMBDA, .type = &arrow,
        .lambda = {.params = &parameter,
                   .param_types = parameter_types,
                   .param_count = 1,
                   .body = &parameter_use}};

    AST payload_ast = {.type = AST_STRING, .string = "payload"};
    QttCoreNode payload = {
        .kind = QTT_CORE_LITERAL, .type = &string,
        .literal = {.source = &payload_ast}};
    QttCoreVar callable = {71, 2};
    QttCoreVar owned = {71, 3};
    QttCoreVar callable_alias = {71, 5};
    QttCoreNode callable_use = {
        .kind = QTT_CORE_VAR, .type = &arrow, .var = callable_alias};
    QttCoreNode original_callable_use = {
        .kind = QTT_CORE_VAR, .type = &arrow, .var = callable};
    QttCoreNode owned_use = {
        .kind = QTT_CORE_VAR, .type = &string, .var = owned};
    QttCoreNode *arguments[] = {&owned_use};
    QttCoreNode indirect = {
        .kind = QTT_CORE_APPLY, .type = &string,
        .apply = {.callee = &callable_use,
                  .arguments = arguments,
                  .argument_count = 1}};
    QttCoreNode bind_owned = {
        .kind = QTT_CORE_LET, .type = &string,
        .let = {.binding = owned, .value = &payload, .body = &indirect}};
    QttCoreNode bind_callable = {
        .kind = QTT_CORE_LET, .type = &string,
        .let = {.binding = callable,
                .value = &identity,
                .body = &(QttCoreNode){
                    .kind = QTT_CORE_LET, .type = &string,
                    .let = {.binding = callable_alias,
                            .value = &original_callable_use,
                            .body = &bind_owned}}}};

    QttResourceElaborationError resource_error =
        QTT_RESOURCE_ELABORATE_OK;
    QttResourceBlock *resources =
        qtt_resource_elaborate_lexical(&bind_callable, &resource_error);
    assert(resources && resource_error == QTT_RESOURCE_ELABORATE_OK);
    assert(qtt_resource_verify_elaboration(
        resources, &bind_callable).error == QTT_RESOURCE_VALID);
    qtt_resource_block_free(resources);

    QttSemanticIrError semantic_error = QTT_SEMANTIC_IR_OK;
    QttSemanticFunction *semantic = qtt_semantic_ir_lower(
        &bind_callable, &semantic_error);
    assert(semantic && semantic_error == QTT_SEMANTIC_IR_OK);
    QttSemanticIrValidation semantic_validation =
        qtt_semantic_ir_verify(semantic, &bind_callable);
    if (semantic_validation != QTT_SEMANTIC_IR_VALID)
        fprintf(stderr, "semantic validation %d\n", semantic_validation);
    assert(semantic_validation == QTT_SEMANTIC_IR_VALID);
    qtt_semantic_ir_free(semantic);

    QttAnfLowerError error = QTT_ANF_LOWER_OK;
    QttAnfProgram *program = qtt_anf_lower_core(&bind_callable, &error);
    assert(program && error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(program).error == QTT_ANF_VALID);
    bool moved = false;
    for (size_t i = 0; i < program->blocks[0].instruction_count; i++)
        moved |= program->blocks[0].instructions[i].kind == QTT_ANF_MOVE;
    assert(moved);
    qtt_anf_program_free(program);

    Type number = {.kind = TYPE_INT};
    Type effect_arrow = {.kind = TYPE_ARROW,
                         .arrow_param = &number,
                         .arrow_ret = &number};
    QttCoreVar state = {71, 6};
    const Type *effect_parameter_types[] = {&number};
    QttCoreNode replacement = {
        .kind = QTT_CORE_LITERAL, .type = &number};
    QttCoreNode state_write = {
        .kind = QTT_CORE_WRITE, .type = &number,
        .write = {
            .place = {.root = state},
            .value = &replacement,
        },
    };
    QttCoreNode effectful = {
        .kind = QTT_CORE_LAMBDA, .type = &effect_arrow,
        .lambda = {
            .params = &state,
            .param_types = effect_parameter_types,
            .param_count = 1,
            .body = &state_write,
        },
    };
    QttCoreVar effect_callable = {71, 7};
    QttCoreNode effect_callable_use = {
        .kind = QTT_CORE_VAR, .type = &effect_arrow,
        .var = effect_callable};
    QttCoreNode effect_argument = {
        .kind = QTT_CORE_LITERAL, .type = &number};
    QttCoreNode *effect_arguments[] = {&effect_argument};
    QttCoreNode effect_invoke = {
        .kind = QTT_CORE_APPLY, .type = &number,
        .apply = {
            .callee = &effect_callable_use,
            .arguments = effect_arguments,
            .argument_count = 1,
        },
    };
    QttCoreNode bind_effect_callable = {
        .kind = QTT_CORE_LET, .type = &number,
        .let = {
            .binding = effect_callable,
            .value = &effectful,
            .body = &effect_invoke,
        },
    };
    semantic = qtt_semantic_ir_lower(
        &bind_effect_callable, &semantic_error);
    assert(semantic && semantic_error == QTT_SEMANTIC_IR_OK);
    assert(qtt_semantic_ir_effect_label_count(
               semantic, 0, "write:71:6:0") == 1);
    assert(qtt_semantic_ir_verify(semantic, &bind_effect_callable) ==
           QTT_SEMANTIC_IR_VALID);
    qtt_semantic_ir_free(semantic);

    /* Same indirect syntax without lexical callable evidence fails closed. */
    QttCoreVar unknown_callable = {71, 4};
    QttCoreNode unknown_use = {
        .kind = QTT_CORE_VAR, .type = &arrow, .var = unknown_callable};
    QttCoreNode unknown_call = indirect;
    unknown_call.apply.callee = &unknown_use;
    QttCoreNode unknown_lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &string,
        .lambda = {.params = &unknown_callable,
                   .param_types = (const Type *[]){&arrow},
                   .param_count = 1,
                   .body = &unknown_call}};
    QttSignatureError signature_error = QTT_SIGNATURE_OK;
    QttFunctionSignature *signature =
        qtt_signature_derive(&unknown_lambda, &signature_error);
    assert(signature && signature_error == QTT_SIGNATURE_OK);
    semantic = qtt_semantic_ir_lower(&unknown_lambda, &semantic_error);
    assert(!semantic && semantic_error == QTT_SEMANTIC_IR_UNSUPPORTED);
    program = qtt_anf_lower_function(&unknown_lambda, signature, &error);
    assert(!program && error == QTT_ANF_LOWER_UNSUPPORTED_CORE);
    qtt_signature_free(signature);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_indirect_call_test.c"
            executable = directory / "qtt_indirect_call_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(harness),
                    *(str(ROOT / "qtt" / name) for name in (
                        "core.c", "quantity.c", "demand.c", "graded.c",
                        "environment.c", "constraints.c", "elaboration.c",
                        "core_usage.c",
                        "resource.c", "type_identity.c", "signature.c",
                        "signature_env.c", "call.c", "../effects/effect.c", "place.c",
                        "drop.c", "closure_policy.c", "closure.c",
                        "semantic_ir.c", "anf.c",
                    )),
                    "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
