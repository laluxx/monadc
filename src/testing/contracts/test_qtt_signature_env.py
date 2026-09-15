"""Module-level ownership signature identities and stale-proof rejection."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class QttSignatureEnvTests(unittest.TestCase):
    def test_registry_is_collision_safe_and_rejects_stale_signatures(self):
        source = r'''
#include "qtt/signature_env.h"
#include <assert.h>

static QttFunctionSignature *identity(
    QttCoreNode *lambda, QttCoreNode *body, QttCoreVar *parameter,
    const Type **parameter_types, Type *type) {
    *body = (QttCoreNode){
        .kind = QTT_CORE_VAR, .type = type, .var = *parameter};
    *lambda = (QttCoreNode){
        .kind = QTT_CORE_LAMBDA,
        .type = type,
        .lambda = {
            .params = parameter,
            .param_types = parameter_types,
            .param_count = 1,
            .body = body,
        },
    };
    QttSignatureError error = QTT_SIGNATURE_OK;
    return qtt_signature_derive(lambda, &error);
}

int main(void) {
    Type integer = {.kind = TYPE_INT};
    const Type *parameter_types[] = {&integer};
    QttCoreVar x = {.module_id = 17, .binder_id = 1};
    QttCoreVar y = {.module_id = 17, .binder_id = 2};
    QttCoreNode x_body, x_lambda, y_body, y_lambda;
    QttFunctionSignature *x_signature =
        identity(&x_lambda, &x_body, &x, parameter_types, &integer);
    QttFunctionSignature *y_signature =
        identity(&y_lambda, &y_body, &y, parameter_types, &integer);
    assert(x_signature && y_signature);

    QttTypeArena *types = qtt_type_arena_new();
    QttSignatureEnv *env = qtt_signature_env_new(types);
    assert(types && env);
    assert(qtt_signature_canonicalize(x_signature, types) ==
           QTT_SIGNATURE_CANONICAL);
    assert(qtt_signature_canonicalize(y_signature, types) ==
           QTT_SIGNATURE_CANONICAL);

    QttCallableId x_id = {0};
    assert(qtt_signature_env_register_with_hash(
        env, 17, "Main.id", 5, x_signature, &x_id) ==
        QTT_SIGNATURE_ENV_OK);
    assert(x_id.module_id == 17 && x_id.name_hash == 5);
    assert(qtt_signature_env_lookup(env, 17, "Main.id") == x_signature);
    assert(qtt_signature_env_register_with_hash(
        env, 17, "Main.id", 5, x_signature, NULL) ==
        QTT_SIGNATURE_ENV_DUPLICATE);

    QttCallableId y_id = {0};
    assert(qtt_signature_env_register_with_hash(
        env, 17, "Main.other", 5, y_signature, &y_id) ==
        QTT_SIGNATURE_ENV_OK);
    assert(!qtt_callable_id_equal(x_id, y_id));
    assert(qtt_signature_env_lookup_id(env, y_id) == y_signature);
    assert(qtt_signature_env_count(env) == 2);

    x_body.var.binder_id = 99;
    QttSignatureEnv *stale_env = qtt_signature_env_new(types);
    assert(stale_env);
    assert(qtt_signature_env_register(
        stale_env, 17, "Main.stale", x_signature, NULL) ==
        QTT_SIGNATURE_ENV_STALE_SIGNATURE);

    qtt_signature_env_free(stale_env);
    qtt_signature_env_free(env);
    qtt_type_arena_free(types);
    qtt_signature_free(x_signature);
    qtt_signature_free(y_signature);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_signature_env_test.c"
            executable = directory / "qtt_signature_env_test"
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
                    "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
