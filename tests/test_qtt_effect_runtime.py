"""Executable proof-gated abortive effect dispatch."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttEffectRuntimeTests(unittest.TestCase):
    def test_abortive_plan_dispatches_exact_nested_capability(self):
        source = r'''
#include "qtt/core.h"
#include "qtt/core_effect.h"
#include "qtt/effect_runtime.h"
#include "qtt/backend.h"
#include "effects/effect.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

static AST symbol(char *name) {
    AST ast = {0}; ast.type = AST_SYMBOL; ast.symbol = name; return ast;
}

const Type *qtt_bindings_layout_type(const char *name) {
    (void)name; return NULL;
}

static void *clause(void *argument, void *environment) {
    *(int *)environment += 1;
    return (void *)((uintptr_t)argument + 10);
}

int main(void) {
    qtt_effect_declarations_clear();
    QttEffectDeclaration raise = {
        .name = "Core.Exception.raise", .traits = "exception",
        .kind = QTT_EFFECT_EXCEPTION, .constructor_id = 700,
        .operation = "raise", .resumption = {0, false},
    };
    QttEffectHandlerProfile abort = {
        .name = "Core.Exception.abort",
        .effect_name = "Core.Exception.raise",
        .continuation_usage = {0, false}, .deep = true,
    };
    assert(qtt_effect_declaration_register(&raise));
    assert(qtt_effect_handler_profile_register(&abort));

    AST perform_head = symbol("perform");
    AST effect_name = symbol("Core.Exception.raise");
    AST argument = {0}; argument.type = AST_NUMBER; argument.number = 3;
    AST *perform_items[] = {&perform_head, &effect_name, &argument};
    AST perform = {.type = AST_LIST};
    perform.list.items = perform_items; perform.list.count = 3;
    perform.line = 12; perform.column = 5;
    AST add_head = symbol("+");
    AST prefix = {0}; prefix.type = AST_NUMBER; prefix.number = 100;
    AST *nested_items[] = {&add_head, &prefix, &perform};
    AST nested = {.type = AST_LIST};
    nested.list.items = nested_items; nested.list.count = 3;
    AST handle_head = symbol("handle");
    AST profile_name = symbol("Core.Exception.abort");
    AST clause_name = symbol("abort-clause");
    AST *handle_items[] = {
        &handle_head, &profile_name, &nested, &clause_name};
    AST handle = {.type = AST_LIST};
    handle.list.items = handle_items; handle.list.count = 4;
    handle.line = 11; handle.column = 2;

    QttCoreError core_error = QTT_CORE_OK;
    QttCoreNode *core = qtt_core_lower(&handle, 88, &core_error);
    assert(core && core_error == QTT_CORE_OK);
    QttCoreEffectResult effects = qtt_core_effect_elaborate(
        core, qtt_quantity_finite(0));
    assert(effects.status == QTT_CORE_EFFECT_OK);
    QttBackendAbortiveHandlerPlan plan = {0};
    assert(qtt_backend_certify_abortive_handler(core, &effects, &plan) ==
           QTT_BACKEND_EFFECT_OK);
    assert(core->handle.selected_operation != core->handle.computation);
    assert(core->handle.abortive_context_safe);
    assert(plan.constructor_id == 700 && plan.capability_id != 0);
    assert(plan.authority_fingerprint != 0);
    assert(plan.constraint_fingerprint == effects.constraint_fingerprint);
    core->handle.abortive_context_safe = false;
    assert(qtt_backend_certify_abortive_handler(core, &effects, &plan) ==
           QTT_BACKEND_EFFECT_EFFECTFUL_CONTEXT);
    core->handle.abortive_context_safe = true;
    assert(qtt_backend_certify_abortive_handler(core, &effects, &plan) ==
           QTT_BACKEND_EFFECT_OK);

    QttEffectRuntime runtime;
    qtt_effect_runtime_init(&runtime);
    int calls = 0;
    void *output = NULL;
    assert(qtt_backend_execute_abortive_handler(
        core, &effects, &plan, &runtime, clause, &calls,
        (void *)(uintptr_t)2, &output) == QTT_EFFECT_DISPATCH_HANDLED);
    assert((uintptr_t)output == 12 && calls == 1);
    assert(runtime.top == NULL);

    QttEffectHandlerFrame outer = {0};
    assert(qtt_effect_runtime_push(
        &runtime, &outer, 700, plan.capability_id, clause, &calls));
    assert(qtt_effect_runtime_perform_abortive(
        &runtime, 700, plan.capability_id, (void *)(uintptr_t)5, &output) ==
        QTT_EFFECT_DISPATCH_HANDLED);
    assert((uintptr_t)output == 15 && calls == 2);

    /* A nearer nonmatching frame cannot intercept nominal authority. */
    QttEffectHandlerFrame inner = {0};
    assert(qtt_effect_runtime_push(
        &runtime, &inner, 700, plan.capability_id + 1, clause, &calls));
    assert(qtt_effect_runtime_perform_abortive(
        &runtime, 700, plan.capability_id, (void *)(uintptr_t)7, &output) ==
        QTT_EFFECT_DISPATCH_HANDLED);
    assert((uintptr_t)output == 17 && calls == 3);
    assert(!qtt_effect_runtime_pop(&runtime, &outer));
    assert(qtt_effect_runtime_pop(&runtime, &inner));
    assert(qtt_effect_runtime_pop(&runtime, &outer));
    assert(qtt_effect_runtime_perform_abortive(
        &runtime, 700, plan.capability_id, NULL, &output) ==
        QTT_EFFECT_DISPATCH_UNHANDLED);
    assert(!qtt_effect_runtime_push(
        &runtime, &outer, 0, plan.capability_id, clause, &calls));

    QttBackendAbortiveHandlerPlan altered = plan;
    altered.capability_id++;
    assert(qtt_backend_execute_abortive_handler(
        core, &effects, &altered, &runtime, clause, &calls,
        NULL, &output) == QTT_EFFECT_DISPATCH_INVALID);
    altered = plan;
    altered.constraint_fingerprint ^= 1;
    assert(qtt_backend_execute_abortive_handler(
        core, &effects, &altered, &runtime, clause, &calls,
        NULL, &output) == QTT_EFFECT_DISPATCH_INVALID);

    core->handle.portable_proof[
        strlen(core->handle.portable_proof) - 1] ^= 1;
    assert(qtt_backend_certify_abortive_handler(core, &effects, &plan) ==
           QTT_BACKEND_EFFECT_INVALID_PROOF);
    qtt_core_effect_result_free(&effects);
    qtt_core_free(core);

    QttEffectDeclaration read = {
        .name = "Core.IO.read", .traits = "io",
        .kind = QTT_EFFECT_IO, .constructor_id = 701,
        .operation = "read", .resumption = {1, false},
    };
    QttEffectHandlerProfile read_once = {
        .name = "Core.IO.read-once", .effect_name = "Core.IO.read",
        .continuation_usage = {1, false}, .deep = true,
    };
    assert(qtt_effect_declaration_register(&read));
    assert(qtt_effect_handler_profile_register(&read_once));
    effect_name.symbol = "Core.IO.read";
    profile_name.symbol = "Core.IO.read-once";
    core = qtt_core_lower(&handle, 88, &core_error);
    assert(core);
    effects = qtt_core_effect_elaborate(core, qtt_quantity_finite(1));
    assert(effects.status == QTT_CORE_EFFECT_OK);
    assert(qtt_backend_certify_abortive_handler(core, &effects, &plan) ==
           QTT_BACKEND_EFFECT_UNSUPPORTED_POLICY);
    qtt_core_effect_result_free(&effects);
    qtt_core_free(core);
    qtt_effect_declarations_clear();
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "effect_runtime.c"
            executable = directory / "effect_runtime"
            harness.write_text(source)
            subprocess.run([
                "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-ffunction-sections", "-fdata-sections",
                "-iquote", str(ROOT), str(harness),
                str(ROOT / "qtt/core.c"), str(ROOT / "qtt/core_effect.c"),
                str(ROOT / "qtt/effect_runtime.c"),
                str(ROOT / "qtt/backend.c"), str(ROOT / "qtt/place.c"),
                str(ROOT / "qtt/quantity.c"),
                str(ROOT / "effects/effect.c"),
                str(ROOT / "effects/constraints.c"),
                "-Wl,--gc-sections", "-o", str(executable),
            ], cwd=ROOT, check=True)
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
