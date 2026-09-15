"""Certified effect elaboration for typed-Core computations."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class QttCoreEffectTests(unittest.TestCase):
    def test_application_imports_latent_arrow_effect_into_handler_context(self):
        source = r'''
#include "qtt/core.h"
#include "qtt/core_effect.h"
#include "effects/effect.h"
#include <assert.h>
#include <stdlib.h>

int main(void) {
    qtt_effect_declarations_clear();
    QttEffectDeclaration raise = {
        .name = "Demo.raise", .kind = QTT_EFFECT_EXCEPTION,
        .constructor_id = 810, .operation = "raise",
        .resumption = {0, false},
    };
    QttEffectDeclaration trace = {
        .name = "Demo.trace", .kind = QTT_EFFECT_IO,
        .constructor_id = 811, .operation = "trace",
        .resumption = {0, false},
    };
    QttEffectHandlerProfile catch = {
        .name = "Demo.catch", .effect_name = "Demo.raise",
        .continuation_usage = {0, false}, .deep = true,
    };
    assert(qtt_effect_declaration_register(&raise));
    assert(qtt_effect_declaration_register(&trace));
    assert(qtt_effect_handler_profile_register(&catch));

    QttEffectArena *scheme_arena = qtt_effect_arena_new();
    QttEffectSolver *scheme_solver = qtt_effect_solver_new(scheme_arena);
    QttEffectAtom trace_atom = {
        .kind = QTT_EFFECT_IO, .constructor_id = 811,
        .capability_id = 991, .name = "Demo.trace",
        .operation = "trace", .traits = "io", .resumption = {0, false},
    };
    QttEffectRow *trace_row = qtt_effect_extend_atom(
        scheme_arena, &trace_atom, qtt_effect_empty(scheme_arena));
    QttEffectScheme *trace_scheme = qtt_effect_generalize(
        scheme_arena, scheme_solver, trace_row);
    char *portable = qtt_effect_scheme_serialize(trace_scheme);
    assert(portable);

    Type integer = {.kind = TYPE_INT};
    Type trace_arrow = {
        .kind = TYPE_ARROW, .arrow_param = &integer, .arrow_ret = &integer,
        .arrow_effect_scheme = portable, .arrow_effect_complete = true,
    };
    size_t predicate_stages[] = {0};
    char *predicate_names[] = {"io"};
    QttCoreNode trace_global = {
        .kind = QTT_CORE_GLOBAL, .type = &trace_arrow,
        .global = {.name = "Demo.trace-call",
                   .effect_predicate_stages = predicate_stages,
                   .effect_predicate_names = predicate_names,
                   .effect_predicate_count = 1},
    };
    QttCoreNode one = {.kind = QTT_CORE_LITERAL, .type = &integer};
    QttCoreNode *trace_arguments[] = {&one};
    QttCoreNode trace_call = {
        .kind = QTT_CORE_APPLY, .type = &integer,
        .apply = {.callee = &trace_global, .arguments = trace_arguments,
                  .argument_count = 1},
    };
    QttCoreNode two = {.kind = QTT_CORE_LITERAL, .type = &integer};
    QttCoreNode perform = {
        .kind = QTT_CORE_PERFORM, .type = &integer,
        .perform = {.effect_name = "Demo.raise", .capability_id = 992,
                    .argument = &two},
    };
    QttCoreNode plus = {
        .kind = QTT_CORE_GLOBAL, .global = {.name = "+"}};
    QttCoreNode *sum_arguments[] = {&trace_call, &perform};
    QttCoreNode sum = {
        .kind = QTT_CORE_APPLY, .type = &integer,
        .apply = {.callee = &plus, .arguments = sum_arguments,
                  .argument_count = 2},
    };
    QttCoreNode clause = {.kind = QTT_CORE_GLOBAL, .type = &trace_arrow};
    QttCoreNode handle = {
        .kind = QTT_CORE_HANDLE, .type = &integer,
        .handle = {.profile_name = "Demo.catch", .computation = &sum,
                   .clause = &clause},
    };

    QttCoreEffectResult result = qtt_core_effect_elaborate(
        &handle, qtt_quantity_finite(0));
    assert(result.status == QTT_CORE_EFFECT_OK);
    assert(qtt_effect_atom_count(
        result.solver, result.effects, &trace_atom) == 1);
    assert(!handle.handle.abortive_context_safe);
    assert(qtt_core_effect_verify_handle(&handle, &result));
    assert(result.constraint_result == QTT_EFFECT_CONSTRAINT_SOLVED);
    assert(result.constraint_certificate);
    assert(qtt_effect_certificate_count(result.constraint_certificate) == 1);
    assert(qtt_effect_certificate_status(
        result.constraint_certificate, 0) == QTT_EFFECT_OBLIGATION_PROVED);
    assert(result.constraint_fingerprint != 0);
    assert(qtt_core_effect_verify_constraints(&result));

    qtt_core_effect_result_free(&result);
    free(handle.handle.portable_proof);
    handle.handle.portable_proof = NULL;

    /* A closed row that cannot entail its predicate is definitionally bad. */
    predicate_names[0] = "telemetry";
    result = qtt_core_effect_elaborate(
        &trace_call, qtt_quantity_finite(0));
    assert(result.status == QTT_CORE_EFFECT_INVALID_CALL_EFFECT);
    assert(result.constraint_result == QTT_EFFECT_CONSTRAINT_REJECTED);
    qtt_core_effect_result_free(&result);
    predicate_names[0] = "io";

    /* A malformed canonical arrow contract is not silently treated as pure. */
    trace_arrow.arrow_effect_scheme = "not-an-effect-scheme";
    result = qtt_core_effect_elaborate(
        &trace_call, qtt_quantity_finite(0));
    assert(result.status == QTT_CORE_EFFECT_INVALID_CALL_EFFECT);
    qtt_core_effect_result_free(&result);
    trace_arrow.arrow_effect_scheme = portable;

    QttEffectScheme *empty_scheme = qtt_effect_generalize(
        scheme_arena, scheme_solver, qtt_effect_empty(scheme_arena));
    char *empty_portable = qtt_effect_scheme_serialize(empty_scheme);
    assert(empty_portable);
    /* Incompleteness itself prevents erasure, even when the known row is empty. */
    trace_arrow.arrow_effect_scheme = empty_portable;
    trace_arrow.arrow_effect_complete = false;
    result = qtt_core_effect_elaborate(
        &handle, qtt_quantity_finite(0));
    assert(result.status == QTT_CORE_EFFECT_OK);
    assert(!handle.handle.abortive_context_safe);
    assert(result.constraint_result == QTT_EFFECT_CONSTRAINT_RESIDUAL);
    assert(qtt_effect_certificate_status(
        result.constraint_certificate, 0) == QTT_EFFECT_OBLIGATION_RESIDUAL);
    assert(qtt_core_effect_verify_handle(&handle, &result));
    assert(qtt_core_effect_verify_constraints(&result));
    qtt_core_effect_result_free(&result);
    free(handle.handle.portable_proof);
    handle.handle.portable_proof = NULL;

    qtt_effect_scheme_free(empty_scheme);
    free(empty_portable);
    qtt_effect_scheme_free(trace_scheme);
    qtt_effect_solver_free(scheme_solver);
    qtt_effect_arena_free(scheme_arena);
    free(portable);
    qtt_effect_declarations_clear();
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "core_latent_effect.c"
            executable = directory / "core_latent_effect"
            harness.write_text(source)
            subprocess.run([
                "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-iquote", str(ROOT / "src"), str(harness),
                str(ROOT / "qtt/core.c"), str(ROOT / "qtt/core_effect.c"),
                str(ROOT / "qtt/place.c"), str(ROOT / "qtt/quantity.c"),
                str(ROOT / "effects/effect.c"),
                str(ROOT / "effects/constraints.c"),
                "-o", str(executable),
            ], cwd=ROOT, check=True)
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)

    def test_perform_and_handle_resolve_core_authority_and_attach_proof(self):
        source = r'''
#include "qtt/core.h"
#include "qtt/core_effect.h"
#include "effects/effect.h"
#include <assert.h>
#include <string.h>

static AST symbol(char *name) {
    AST ast = {0}; ast.type = AST_SYMBOL; ast.symbol = name; return ast;
}

int main(void) {
    qtt_effect_declarations_clear();
    QttEffectDeclaration read = {
        .name = "Core.Console.read", .traits = "io,read",
        .kind = QTT_EFFECT_READ, .constructor_id = 91,
        .operation = "read", .resumption = {1, false},
    };
    assert(qtt_effect_declaration_register(&read));
    QttEffectHandlerProfile once = {
        .name = "Core.Console.read-once",
        .effect_name = "Core.Console.read",
        .continuation_usage = {1, false}, .deep = true,
    };
    assert(qtt_effect_handler_profile_register(&once));

    AST perform_head = symbol("perform");
    AST effect_name = symbol("Core.Console.read");
    AST unit = {0}; unit.type = AST_STRING; unit.string = "prompt";
    AST *perform_items[] = {&perform_head, &effect_name, &unit};
    AST perform = {0}; perform.type = AST_LIST;
    perform.list.items = perform_items; perform.list.count = 3;
    perform.line = 8; perform.column = 4;

    QttCoreError error = QTT_CORE_OK;
    QttCoreNode *core = qtt_core_lower(&perform, 77, &error);
    assert(core && error == QTT_CORE_OK);
    QttCoreEffectResult result = qtt_core_effect_elaborate(
        core, qtt_quantity_finite(1));
    assert(result.status == QTT_CORE_EFFECT_OK);
    assert(result.effects && result.fingerprint != 0);
    assert(core->perform.constructor_id == 91);
    assert(core->perform.resumption.finite == 1);
    assert(qtt_effect_atom_count(
        result.solver, result.effects, &result.handled_atom) == 1);
    qtt_core_effect_result_free(&result);
    qtt_core_free(core);

    AST handle_head = symbol("handle");
    AST profile_name = symbol("Core.Console.read-once");
    AST clause = symbol("read-clause");
    AST *handle_items[] = {
        &handle_head, &profile_name, &perform, &clause};
    AST handle = {0}; handle.type = AST_LIST;
    handle.list.items = handle_items; handle.list.count = 4;
    handle.line = 7; handle.column = 2;
    core = qtt_core_lower(&handle, 77, &error);
    assert(core && error == QTT_CORE_OK);
    result = qtt_core_effect_elaborate(core, qtt_quantity_finite(1));
    assert(result.status == QTT_CORE_EFFECT_OK);
    assert(qtt_effect_atom_count(
        result.solver, result.effects, &result.handled_atom) == 0);
    assert(core->handle.resumption_class == QTT_CORE_RESUMPTION_ONE_SHOT);
    assert(core->handle.deep);
    assert(core->handle.portable_proof);
    assert(strstr(core->handle.portable_proof,
                  "monad-handler-proof-v2|") ==
           core->handle.portable_proof);
    assert(qtt_core_effect_verify_handle(core, &result));
    uint64_t authority = qtt_core_effect_authority_fingerprint(core);
    assert(authority != 0);
    core->handle.portable_proof[strlen(core->handle.portable_proof)-1] ^= 1;
    assert(!qtt_core_effect_verify_handle(core, &result));
    assert(qtt_core_effect_authority_fingerprint(core) != authority);
    qtt_core_effect_result_free(&result);
    /* Rechecking under weaker authority must revoke stale evidence. */
    result = qtt_core_effect_elaborate(core, qtt_quantity_finite(0));
    assert(result.status == QTT_CORE_EFFECT_GRADE_VIOLATION);
    assert(!core->handle.portable_proof);
    qtt_core_effect_result_free(&result);
    qtt_core_free(core);

    core = qtt_core_lower(&handle, 77, &error);
    assert(core);
    result = qtt_core_effect_elaborate(core, qtt_quantity_finite(0));
    assert(result.status == QTT_CORE_EFFECT_GRADE_VIOLATION);
    assert(!core->handle.portable_proof);
    qtt_core_effect_result_free(&result);
    qtt_core_free(core);

    AST missing_profile = symbol("Core.Console.no-handler");
    handle_items[1] = &missing_profile;
    core = qtt_core_lower(&handle, 77, &error);
    assert(core);
    result = qtt_core_effect_elaborate(core, qtt_quantity_finite(1));
    assert(result.status == QTT_CORE_EFFECT_UNKNOWN_PROFILE);
    qtt_core_effect_result_free(&result);
    qtt_core_free(core);
    handle_items[1] = &profile_name;

    QttEffectHandlerProfile shallow = {
        .name = "Core.Console.read-shallow",
        .effect_name = "Core.Console.read",
        .continuation_usage = {1, false}, .deep = false,
    };
    assert(qtt_effect_handler_profile_register(&shallow));
    AST shallow_name = symbol("Core.Console.read-shallow");
    handle_items[1] = &shallow_name;
    core = qtt_core_lower(&handle, 77, &error);
    assert(core);
    result = qtt_core_effect_elaborate(core, qtt_quantity_finite(1));
    assert(result.status == QTT_CORE_EFFECT_UNSUPPORTED_SHALLOW);
    qtt_core_effect_result_free(&result);
    qtt_core_free(core);
    handle_items[1] = &profile_name;

    AST missing_name = symbol("Core.Console.missing");
    perform_items[1] = &missing_name;
    core = qtt_core_lower(&perform, 77, &error);
    assert(core);
    result = qtt_core_effect_elaborate(core, qtt_quantity_finite(1));
    assert(result.status == QTT_CORE_EFFECT_UNKNOWN_EFFECT);
    qtt_core_effect_result_free(&result);
    qtt_core_free(core);

    perform_items[1] = &effect_name;
    QttEffectHandlerProfile multi = {
        .name = "Core.Console.multi", .effect_name = "Core.Console.read",
        .continuation_usage = {2, false}, .deep = true,
    };
    /* Profile registration itself rejects use beyond declaration authority. */
    assert(!qtt_effect_handler_profile_register(&multi));

    QttEffectDeclaration choose = {
        .name = "Core.Search.choose", .traits = "control",
        .kind = QTT_EFFECT_CONTROL, .constructor_id = 92,
        .operation = "choose", .resumption = {2, false},
    };
    assert(qtt_effect_declaration_register(&choose));
    QttEffectHandlerProfile choose_profile = {
        .name = "Core.Search.choose-many",
        .effect_name = "Core.Search.choose",
        .continuation_usage = {1, false}, .deep = true,
    };
    assert(qtt_effect_handler_profile_register(&choose_profile));
    AST choose_name = symbol("Core.Search.choose");
    AST choose_profile_name = symbol("Core.Search.choose-many");
    perform_items[1] = &choose_name;
    handle_items[1] = &choose_profile_name;
    core = qtt_core_lower(&handle, 77, &error);
    assert(core);
    result = qtt_core_effect_elaborate(core, qtt_quantity_finite(2));
    assert(result.status == QTT_CORE_EFFECT_OK);
    assert(core->handle.resumption_class == QTT_CORE_RESUMPTION_MULTI_SHOT);
    assert(qtt_core_effect_runtime_policy(core) ==
           QTT_CORE_EFFECT_RUNTIME_UNSUPPORTED_MULTI_SHOT);
    qtt_core_effect_result_free(&result);
    qtt_core_free(core);
    qtt_effect_declarations_clear();
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "core_effect.c"
            executable = directory / "core_effect"
            harness.write_text(source)
            subprocess.run([
                "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-iquote", str(ROOT / "src"), str(harness),
                str(ROOT / "qtt/core.c"), str(ROOT / "qtt/core_effect.c"),
                str(ROOT / "qtt/place.c"), str(ROOT / "qtt/quantity.c"),
                str(ROOT / "effects/effect.c"),
                str(ROOT / "effects/constraints.c"),
                "-o", str(executable),
            ], cwd=ROOT, check=True)
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
