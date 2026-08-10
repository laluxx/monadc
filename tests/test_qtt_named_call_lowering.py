"""Named Core calls lower through certified module signatures."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttNamedCallLoweringTests(unittest.TestCase):
    def test_named_owned_call_lowers_to_verified_call_ir(self):
        source = r'''
#include "qtt/anf.h"
#include <assert.h>

int main(void) {
    Type string_type = {.kind = TYPE_STRING};
    const Type *parameter_types[] = {&string_type};
    QttCoreVar parameter = {.module_id = 31, .binder_id = 1};
    QttCoreNode callee_body = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = parameter};
    QttCoreNode callee = {
        .kind = QTT_CORE_LAMBDA, .type = &string_type,
        .lambda = {.params = &parameter, .param_types = parameter_types,
                   .param_count = 1, .body = &callee_body}};

    AST literal_ast = {.type = AST_STRING, .string = "payload"};
    QttCoreNode literal = {
        .kind = QTT_CORE_LITERAL, .type = &string_type,
        .literal = {.source = &literal_ast}};
    QttCoreNode global = {
        .kind = QTT_CORE_GLOBAL, .type = &string_type,
        .global = {.name = "Main.consume"}};
    QttCoreNode *arguments[] = {&literal};
    QttCoreNode apply = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &global, .arguments = arguments,
                  .argument_count = 1}};
    QttCoreNode caller = {
        .kind = QTT_CORE_LAMBDA, .type = &string_type,
        .lambda = {.body = &apply}};

    QttSignatureError error = QTT_SIGNATURE_OK;
    QttFunctionSignature *callee_signature =
        qtt_signature_derive(&callee, &error);
    QttFunctionSignature *caller_signature =
        qtt_signature_derive(&caller, &error);
    QttTypeArena *types = qtt_type_arena_new();
    QttSignatureEnv *env = qtt_signature_env_new(types);
    assert(callee_signature && caller_signature && types && env);
    assert(qtt_signature_canonicalize(callee_signature, types) ==
           QTT_SIGNATURE_CANONICAL);
    assert(qtt_signature_canonicalize(caller_signature, types) ==
           QTT_SIGNATURE_CANONICAL);
    assert(qtt_signature_env_register(
        env, 31, "Main.consume", callee_signature, NULL) ==
        QTT_SIGNATURE_ENV_OK);

    QttAnfLowerError lower_error = QTT_ANF_LOWER_OK;
    QttAnfProgram *program = qtt_anf_lower_function_in_env(
        &caller, caller_signature, env, 31, &lower_error);
    assert(program && lower_error == QTT_ANF_LOWER_OK);
    assert(program->signatures == env);
    bool saw_call = false;
    for (size_t i = 0; i < program->blocks[0].instruction_count; i++)
        saw_call |= program->blocks[0].instructions[i].kind == QTT_ANF_CALL;
    assert(saw_call);
    assert(qtt_anf_verify(program).error == QTT_ANF_VALID);

    qtt_anf_program_free(program);

    /* Backend fragments lower expressions rather than whole lambda bodies.
     * They must retain the same checked module environment at named calls. */
    program = qtt_anf_lower_core_in_env(
        &apply, env, 31, &lower_error);
    assert(program && lower_error == QTT_ANF_LOWER_OK);
    assert(program->signatures == env);
    saw_call = false;
    for (size_t i = 0; i < program->blocks[0].instruction_count; i++)
        saw_call |= program->blocks[0].instructions[i].kind == QTT_ANF_CALL;
    assert(saw_call);
    assert(qtt_anf_verify(program).error == QTT_ANF_VALID);
    qtt_anf_program_free(program);

    qtt_signature_env_free(env);
    qtt_type_arena_free(types);
    qtt_signature_free(caller_signature);
    qtt_signature_free(callee_signature);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_named_call_lowering_test.c"
            executable = directory / "qtt_named_call_lowering_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "core.c"),
                    str(ROOT / "qtt" / "quantity.c"),
                    str(ROOT / "qtt" / "demand.c"),
                    str(ROOT / "qtt" / "graded.c"),
                    str(ROOT / "qtt" / "resource.c"),
                    str(ROOT / "qtt" / "type_identity.c"),
                    str(ROOT / "qtt" / "signature.c"),
                    str(ROOT / "effects" / "effect.c"),
                    str(ROOT / "qtt" / "place.c"),
                    str(ROOT / "qtt" / "signature_env.c"),
                    str(ROOT / "qtt" / "call.c"),
                    str(ROOT / "qtt" / "anf.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
