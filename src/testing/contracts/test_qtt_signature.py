"""Interprocedural ownership signatures derived from typed Core."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class QttSignatureTests(unittest.TestCase):
    def test_identity_consumes_owned_parameter_and_unused_is_erased(self):
        source = r'''
#include "qtt/anf.h"
#include <assert.h>

int main(void) {
    Type string_type = {.kind = TYPE_STRING};
    QttCoreVar owned = {.module_id = 4, .binder_id = 10};
    QttCoreVar unused = {.module_id = 4, .binder_id = 11};
    QttCoreVar params[] = {owned, unused};
    const Type *param_types[] = {&string_type, &string_type};
    QttCoreNode body = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = owned};
    QttCoreNode lambda = {
        .kind = QTT_CORE_LAMBDA,
        .lambda = {
            .params = params,
            .param_types = param_types,
            .param_count = 2,
            .body = &body,
        },
    };

    QttSignatureError error = QTT_SIGNATURE_OK;
    QttFunctionSignature *signature =
        qtt_signature_derive(&lambda, &error);
    assert(signature && error == QTT_SIGNATURE_OK);
    assert(signature->parameter_count == 2);
    assert(signature->parameters[0].mode == QTT_OWNERSHIP_CONSUMED);
    assert(signature->parameters[0].representation == QTT_REP_OWNED_HEAP);
    assert(signature->parameters[0].quantity.finite == 1);
    assert(signature->parameters[0].observed.finite == 1);
    assert(signature->parameters[1].mode == QTT_OWNERSHIP_ERASED);
    assert(signature->parameters[1].quantity.finite == 0);
    assert(signature->result.mode == QTT_RESULT_OWNED);
    assert(signature->result.origin == QTT_RESULT_ORIGIN_TRANSFERRED);
    assert(signature->result.type == &string_type);
    assert(signature->result.representation == QTT_REP_OWNED_HEAP);
    QttTypeArena *type_arena = qtt_type_arena_new();
    assert(type_arena);
    signature->parameters[0].quantity = qtt_quantity_omega();
    assert(qtt_signature_canonicalize(signature, type_arena) ==
           QTT_SIGNATURE_CANONICAL);
    assert(signature->parameters[0].type_id.value);
    assert(qtt_type_id_equal(signature->parameters[0].type_id,
                             signature->parameters[1].type_id));
    assert(qtt_type_id_equal(signature->parameters[0].type_id,
                             signature->result.type_id));
    assert(qtt_signature_validate(signature, &lambda) ==
           QTT_SIGNATURE_VALID);
    assert(signature->graded);
    assert(qtt_signature_validate_grades(signature, type_arena) ==
           QTT_GRADED_PROOF_VALID);
    signature->parameters[0].quantity = qtt_quantity_finite(0);
    assert(qtt_signature_validate_grades(signature, type_arena) ==
           QTT_GRADED_PROOF_CONTEXT_MISMATCH);
    signature->parameters[0].quantity = qtt_quantity_omega();
    QttAnfLowerError lower_error = QTT_ANF_LOWER_OK;
    QttAnfProgram *anf =
        qtt_anf_lower_function(&lambda, signature, &lower_error);
    assert(anf && lower_error == QTT_ANF_LOWER_OK);
    assert(anf->blocks[0].parameter_count == 1);
    assert(anf->initial_resource_count == 1);
    assert(anf->blocks[0].instruction_count == 1);
    assert(anf->blocks[0].instructions[0].kind == QTT_ANF_MOVE);
    assert(qtt_anf_verify(anf).error == QTT_ANF_VALID);
    qtt_anf_program_free(anf);
    qtt_signature_free(signature);

    AST static_ast = {.type = AST_STRING, .string = "static"};
    QttCoreNode static_string = {
        .kind = QTT_CORE_LITERAL,
        .type = &string_type,
        .literal = {.source = &static_ast},
    };
    QttCoreNode static_lambda = {
        .kind = QTT_CORE_LAMBDA,
        .lambda = {.body = &static_string},
    };
    signature = qtt_signature_derive(&static_lambda, &error);
    assert(signature && error == QTT_SIGNATURE_OK);
    assert(signature->result.mode == QTT_RESULT_BORROWED);
    assert(signature->result.origin == QTT_RESULT_ORIGIN_STATIC);
    qtt_signature_free(signature);

    QttCoreNode concat_global = {
        .kind = QTT_CORE_GLOBAL,
        .global = {.name = "concat"},
    };
    QttCoreNode *concat_args[] = {&static_string, &static_string};
    QttCoreNode concat_call = {
        .kind = QTT_CORE_APPLY,
        .type = &string_type,
        .apply = {
            .callee = &concat_global,
            .arguments = concat_args,
            .argument_count = 2,
        },
    };
    QttCoreNode fresh_lambda = {
        .kind = QTT_CORE_LAMBDA,
        .lambda = {.body = &concat_call},
    };
    signature = qtt_signature_derive(&fresh_lambda, &error);
    assert(signature && error == QTT_SIGNATURE_OK);
    assert(signature->result.mode == QTT_RESULT_OWNED);
    assert(signature->result.origin == QTT_RESULT_ORIGIN_FRESH);
    qtt_signature_free(signature);

    LayoutField owner_fields[] = {
        {.name = "name", .type = &string_type},
        {.name = "spare", .type = &string_type},
    };
    Type owner_type = {
        .kind = TYPE_LAYOUT,
        .layout_name = "Owner",
        .layout_fields = owner_fields,
        .layout_field_count = 2,
    };
    AST owner_source = {.type = AST_LIST, .inferred_type = &owner_type};
    QttCoreVar owner = {.module_id = 4, .binder_id = 12};
    QttCoreNode owner_literal = {
        .kind = QTT_CORE_LITERAL, .type = &owner_type,
        .literal = {.source = &owner_source},
    };
    QttPlace owner_name = {0};
    assert(qtt_place_layout_field(
        qtt_place_root(owner), &owner_type, "name", &owner_name));
    QttCoreNode owner_projection = {
        .kind = QTT_CORE_PLACE, .type = &string_type,
        .place = owner_name,
    };
    QttCoreNode projected_let = {
        .kind = QTT_CORE_LET, .type = &string_type,
        .let = {
            .binding = owner,
            .value = &owner_literal,
            .body = &owner_projection,
        },
    };
    QttCoreNode projected_lambda = {
        .kind = QTT_CORE_LAMBDA,
        .lambda = {.body = &projected_let},
    };
    signature = qtt_signature_derive(&projected_lambda, &error);
    assert(signature && error == QTT_SIGNATURE_OK);
    assert(signature->result.mode == QTT_RESULT_OWNED);
    assert(signature->result.origin == QTT_RESULT_ORIGIN_FRESH);
    assert(signature->result.representation == QTT_REP_OWNED_HEAP);
    qtt_signature_free(signature);
    qtt_type_arena_free(type_arena);

    QttCoreNode borrowed_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = owned};
    Type number_type = {.kind = TYPE_INT};
    AST result_ast = {.type = AST_NUMBER, .number = 7};
    QttCoreNode result_number = {
        .kind = QTT_CORE_LITERAL,
        .type = &number_type,
        .literal = {.source = &result_ast},
    };
    QttCoreNode *sequence_items[] = {&borrowed_use, &result_number};
    QttCoreNode borrowed_body = {
        .kind = QTT_CORE_SEQUENCE,
        .type = &number_type,
        .sequence = {.items = sequence_items, .count = 2},
    };
    QttCoreNode borrowed_lambda = {
        .kind = QTT_CORE_LAMBDA,
        .lambda = {.params = params, .param_count = 1,
                   .body = &borrowed_body},
    };
    signature = qtt_signature_derive(&borrowed_lambda, &error);
    assert(signature && error == QTT_SIGNATURE_OK);
    assert(signature->parameters[0].mode == QTT_OWNERSHIP_BORROWED);
    lower_error = QTT_ANF_LOWER_OK;
    anf = qtt_anf_lower_function(
        &borrowed_lambda, signature, &lower_error);
    assert(anf && lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(anf).error == QTT_ANF_VALID);
    assert(anf->initial_resource_count == 1);
    assert(!anf->initial_resource_owned[0]);
    assert(anf->blocks[0].instructions[0].kind == QTT_ANF_BORROW);
    qtt_anf_program_free(anf);
    qtt_signature_free(signature);

    QttCoreNode truth = {
        .kind = QTT_CORE_GLOBAL, .global = {.name = "True"}};
    QttCoreNode branch_x1 = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = owned};
    QttCoreNode branch_x2 = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = owned};
    QttCoreNode branch_x3 = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = owned};
    QttCoreNode *two_uses[] = {&branch_x2, &branch_x3};
    QttCoreNode branch_sequence = {
        .kind = QTT_CORE_SEQUENCE,
        .type = &string_type,
        .sequence = {.items = two_uses, .count = 2},
    };
    QttCoreNode branch_body = {
        .kind = QTT_CORE_IF,
        .type = &string_type,
        .conditional = {
            .condition = &truth,
            .then_branch = &branch_x1,
            .else_branch = &branch_sequence,
        },
    };
    QttCoreNode branch_lambda = {
        .kind = QTT_CORE_LAMBDA,
        .lambda = {
            .params = params,
            .param_count = 1,
            .body = &branch_body,
        },
    };
    signature = qtt_signature_derive(&branch_lambda, &error);
    assert(signature && error == QTT_SIGNATURE_OK);
    assert(!signature->parameters[0].quantity.is_omega);
    assert(signature->parameters[0].quantity.finite == 2);
    lower_error = QTT_ANF_LOWER_OK;
    anf = qtt_anf_lower_function(
        &branch_lambda, signature, &lower_error);
    assert(anf && lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_anf_verify(anf).error == QTT_ANF_VALID);
    qtt_anf_program_free(anf);
    qtt_signature_free(signature);

    QttCoreVar state = {.module_id = 4, .binder_id = 70};
    QttCoreNode replacement = {
        .kind = QTT_CORE_LITERAL, .type = &number_type};
    QttCoreNode state_write = {
        .kind = QTT_CORE_WRITE, .type = &number_type,
        .write = {.place = {.root = state}, .value = &replacement}};
    const Type *state_param_type = &number_type;
    QttCoreNode effectful_lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &number_type,
        .lambda = {
            .params = &state, .param_types = &state_param_type,
            .param_count = 1, .body = &state_write}};
    signature = qtt_signature_derive(&effectful_lambda, &error);
    assert(signature && error == QTT_SIGNATURE_OK);
    assert(signature->effects_complete);
    assert(signature->effect_arena && signature->effect_solver &&
           signature->latent_effects);
    assert(qtt_effect_label_count(
               signature->effect_solver, signature->latent_effects,
               "write:4:70:0") == 1);
    assert(signature->effect_fingerprint ==
           qtt_effect_row_fingerprint(
               signature->effect_solver, signature->latent_effects));
    QttEffectRow *saved_effects = signature->latent_effects;
    signature->latent_effects = qtt_effect_empty(signature->effect_arena);
    assert(qtt_signature_validate(signature, &effectful_lambda) ==
           QTT_SIGNATURE_EFFECT_MISMATCH);
    signature->latent_effects = saved_effects;
    assert(qtt_signature_validate(signature, &effectful_lambda) ==
           QTT_SIGNATURE_VALID);
    qtt_signature_free(signature);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_signature_test.c"
            executable = directory / "qtt_signature_test"
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
