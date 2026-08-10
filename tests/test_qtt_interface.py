"""Versioned, structurally fingerprinted QTT module interfaces."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class QttInterfaceTests(unittest.TestCase):
    def test_round_trip_and_tamper_rejection(self):
        source = r'''
#include "qtt/interface.h"
#include "effects/effect.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 6);
    Type int_type = {.kind = TYPE_INT};
    Type string_type = {.kind = TYPE_STRING};
    QttParameterContract parameter = {
        .quantity = qtt_quantity_finite(1),
        .observed = qtt_quantity_finite(1),
        .mode = QTT_OWNERSHIP_BORROWED,
        .type = &string_type,
        .representation = QTT_REP_OWNED_HEAP,
    };
    QttFunctionSignature signature = {
        .parameters = &parameter,
        .parameter_count = 1,
        .result_type = &string_type,
        .result = {
            .mode = QTT_RESULT_OWNED,
            .origin = QTT_RESULT_ORIGIN_FRESH,
            .type = &string_type,
            .representation = QTT_REP_OWNED_HEAP,
        },
    };
    QttInterface *written = qtt_interface_new("Fresh");
    assert(written);
    qtt_interface_set_artifact_fingerprint(
        written, UINT64_C(0x123456789abcdef0));
    assert(qtt_interface_add(written, "fresh-prefix", &signature));
    const char *effects =
        "monad-callable-contract-v1|0123456789abcdef|1|abc|";
    assert(qtt_interface_set_callable_contract(
        written, "fresh-prefix", effects, UINT64_C(0x0123456789abcdef)));
    size_t effect_predicate_stages[] = {0, 2};
    const char *effect_predicate_traits[] = {"io", "abortive"};
    const char *malformed_effect_traits[] = {"io,abortive"};
    assert(!qtt_interface_set_effect_judgment(
        written, "fresh-prefix", UINT64_C(1), UINT64_C(2),
        QTT_EFFECT_CONSTRAINT_SOLVED, effect_predicate_stages,
        malformed_effect_traits, 1));
    assert(!qtt_interface_set_effect_judgment(
        written, "fresh-prefix", UINT64_C(1), UINT64_C(2),
        QTT_EFFECT_CONSTRAINT_REJECTED, NULL, NULL, 0));
    assert(qtt_interface_set_effect_judgment(
        written, "fresh-prefix", UINT64_C(0x1111222233334444),
        UINT64_C(0x5555666677778888), QTT_EFFECT_CONSTRAINT_RESIDUAL,
        effect_predicate_stages, effect_predicate_traits, 2));
    const char *hm = "monad-hm-scheme-v1|0123456789abcdef|1|k40(k32:0;)(k32:0;)";
    assert(qtt_interface_set_hm_scheme(written, "fresh-prefix", hm));
    QttEffectDeclaration declaration = {
        .name = "Core.IO.read",
        .traits = "io,ambient",
        .kind = QTT_EFFECT_IO,
        .constructor_id = UINT64_C(0x91a72b3c4d5e6f70),
        .operation = "read",
        .payload_type = "Int",
        .result_type = "String",
        .operation_scheme = "monad-hm-scheme-v1|78f3bd7a3897a80a|0|k31(k0;)(k4;)",
        .resumption = {1, false},
        .scoped = false,
    };
    QttEffectDeclaration malformed_scheme = declaration;
    malformed_scheme.name = "Core.IO.invalid";
    malformed_scheme.operation_scheme =
        "monad-hm-scheme-v1|0000000000000000|0|k31(k0;)(k4;)";
    assert(!qtt_interface_add_effect_declaration(
        written, &malformed_scheme));
    assert(qtt_interface_add_effect_declaration(written, &declaration));
    QttEffectHandlerProfile handler_profile = {
        .name = "Core.IO.read-once",
        .effect_name = "Core.IO.read",
        .continuation_usage = {1, false},
        .deep = true,
    };
    assert(qtt_interface_add_handler_profile(written, &handler_profile));
    assert(qtt_interface_add_trait_implication(
        written, "state", "io", "Fresh"));
    assert(qtt_interface_add_trait_implication(
        written, "allocate", "state", "Fresh"));
    Type legacy_fn_type = {.kind = TYPE_FN};
    signature.result.type = &legacy_fn_type;
    signature.result_type = &legacy_fn_type;
    assert(!qtt_interface_add(written, "unsupported-legacy-fn", &signature));
    signature.result.type = &string_type;
    signature.result_type = &string_type;
    Type variable = {.kind = TYPE_VAR, .var_id = 7};
    Type map_type = {
        .kind = TYPE_MAP,
        .map_key_type = &string_type,
    };
    Type app_type = {
        .kind = TYPE_APP,
        .app_constructor = "Maybe",
        .app_arg = &variable,
    };
    map_type.map_value_type = &app_type;
    Type arrow_type = {
        .kind = TYPE_ARROW,
        .arrow_param = &variable,
        .arrow_ret = &map_type,
    };
    Type optional_type = {
        .kind = TYPE_OPTIONAL,
        .element_type = &string_type,
    };
    LayoutField box_fields[] = {{
        .name = "value", .type = &optional_type, .offset = 0, .size = 8,
    }};
    Type box_type = {
        .kind = TYPE_LAYOUT,
        .layout_name = "Box",
        .layout_fields = box_fields,
        .layout_field_count = 1,
        .layout_total_size = 8,
        .layout_align = 8,
        .layout_is_inline = true,
    };
    parameter.type = &arrow_type;
    signature.result.type = &box_type;
    signature.result_type = &box_type;
    assert(qtt_interface_add(written, "structural", &signature));
    const char *poly_hm =
        "monad-hm-scheme-v1|0123456789abcdef|1|k40(k32:0;)(k32:0;)";
    assert(qtt_interface_add_metadata(written, "polymorphic-only"));
    assert(qtt_interface_set_hm_scheme(
        written, "polymorphic-only", poly_hm));
    assert(qtt_interface_write(written, argv[1]) == QTT_INTERFACE_OK);
    qtt_interface_free(written);

    QttInterfaceError error = QTT_INTERFACE_OK;
    QttInterface *read = qtt_interface_read(argv[1], &error);
    assert(read && error == QTT_INTERFACE_OK);
    assert(strcmp(qtt_interface_module(read), "Fresh") == 0);
    assert(qtt_interface_artifact_fingerprint(read) ==
           UINT64_C(0x123456789abcdef0));
    assert(qtt_interface_count(read) == 3);
    assert(qtt_interface_effect_declaration_count(read) == 1);
    assert(qtt_interface_handler_profile_count(read) == 1);
    const QttEffectHandlerProfile *read_profile =
        qtt_interface_handler_profile(read, 0);
    assert(read_profile);
    assert(strcmp(read_profile->name, "Core.IO.read-once") == 0);
    assert(strcmp(read_profile->effect_name, "Core.IO.read") == 0);
    assert(read_profile->continuation_usage.finite == 1);
    assert(!read_profile->continuation_usage.is_omega);
    assert(read_profile->deep);
    assert(qtt_interface_trait_implication_count(read) == 2);
    const QttEffectTraitImplication *implication =
        qtt_interface_trait_implication(read, 0);
    assert(implication);
    assert(strcmp(implication->premise, "allocate") == 0);
    assert(strcmp(implication->consequence, "state") == 0);
    assert(strcmp(implication->provenance, "Fresh") == 0);
    const QttEffectDeclaration *read_declaration =
        qtt_interface_effect_declaration(read, 0);
    assert(read_declaration);
    assert(strcmp(read_declaration->name, "Core.IO.read") == 0);
    assert(read_declaration->kind == QTT_EFFECT_IO);
    assert(strcmp(read_declaration->traits, "ambient,io") == 0);
    assert(read_declaration->constructor_id ==
           UINT64_C(0x91a72b3c4d5e6f70));
    assert(strcmp(read_declaration->operation, "read") == 0);
    assert(strcmp(read_declaration->payload_type, "Int") == 0);
    assert(strcmp(read_declaration->result_type, "String") == 0);
    assert(strcmp(read_declaration->operation_scheme,
                  "monad-hm-scheme-v1|78f3bd7a3897a80a|0|k31(k0;)(k4;)") == 0);
    assert(!read_declaration->resumption.is_omega);
    assert(read_declaration->resumption.finite == 1);
    assert(!read_declaration->scoped);
    const QttInterfaceContract *contract =
        qtt_interface_contract(read, 0);
    assert(contract);
    assert(strcmp(contract->name, "fresh-prefix") == 0);
    assert(contract->signature.parameter_count == 1);
    assert(contract->signature.parameters[0].type->kind == TYPE_STRING);
    assert(contract->signature.result.type->kind == TYPE_STRING);
    assert(contract->signature.result.mode == QTT_RESULT_OWNED);
    assert(contract->signature.result.origin == QTT_RESULT_ORIGIN_FRESH);
    assert(contract->stable_fingerprint ==
           qtt_interface_signature_fingerprint(&contract->signature));
    assert(contract->callable_contract);
    assert(strcmp(contract->callable_contract, effects) == 0);
    assert(contract->callable_contract_fingerprint ==
           UINT64_C(0x0123456789abcdef));
    assert(contract->has_effect_judgment);
    assert(contract->effect_row_fingerprint ==
           UINT64_C(0x1111222233334444));
    assert(contract->effect_constraint_fingerprint ==
           UINT64_C(0x5555666677778888));
    assert(contract->effect_constraint_result ==
           QTT_EFFECT_CONSTRAINT_RESIDUAL);
    assert(contract->effect_predicate_count == 2);
    assert(contract->effect_predicates[0].stage == 0);
    assert(strcmp(contract->effect_predicates[0].trait, "io") == 0);
    assert(contract->effect_predicates[1].stage == 2);
    assert(strcmp(contract->effect_predicates[1].trait, "abortive") == 0);
    assert(contract->effect_judgment_fingerprint != 0);
    assert(contract->hm_scheme && strcmp(contract->hm_scheme, hm) == 0);
    const QttInterfaceContract *structural =
        qtt_interface_contract(read, 1);
    assert(structural);
    assert(structural->signature.parameters[0].type->kind == TYPE_ARROW);
    assert(structural->signature.parameters[0].type->arrow_param->kind ==
           TYPE_VAR);
    assert(structural->signature.parameters[0].type->arrow_param->var_id == 7);
    assert(structural->signature.parameters[0].type->arrow_ret->kind ==
           TYPE_MAP);
    assert(structural->signature.parameters[0].type->arrow_ret
               ->map_value_type->kind == TYPE_APP);
    assert(strcmp(structural->signature.parameters[0].type->arrow_ret
               ->map_value_type->app_constructor, "Maybe") == 0);
    assert(structural->signature.result.type->kind == TYPE_LAYOUT);
    assert(strcmp(structural->signature.result.type->layout_name, "Box") == 0);
    assert(structural->signature.result.type->layout_field_count == 1);
    assert(structural->signature.result.type->layout_fields[0].type->kind ==
           TYPE_OPTIONAL);
    assert(structural->stable_fingerprint ==
           qtt_interface_signature_fingerprint(&structural->signature));
    const QttInterfaceContract *polymorphic =
        qtt_interface_contract(read, 2);
    assert(polymorphic && !polymorphic->has_ownership_signature);
    assert(polymorphic->stable_fingerprint == 0);
    assert(polymorphic->hm_scheme &&
           strcmp(polymorphic->hm_scheme, poly_hm) == 0);
    qtt_interface_free(read);

    FILE *input = fopen(argv[1], "r");
    FILE *output = fopen(argv[2], "w");
    assert(input && output);
    char line[1024];
    while (fgets(line, sizeof(line), input)) {
        if (strncmp(line, "RESULT ", 7) == 0) {
            char *owned = strstr(line, " mode=1 ");
            assert(owned);
            memcpy(owned, " mode=2 ", 8);
        }
        fputs(line, output);
    }
    fclose(input);
    fclose(output);
    read = qtt_interface_read(argv[2], &error);
    assert(!read);
    assert(error == QTT_INTERFACE_FINGERPRINT_MISMATCH);

    input = fopen(argv[5], "w");
    assert(input);
    FILE *original = fopen(argv[1], "r");
    assert(original);
    while (fgets(line, sizeof(line), original)) {
        if (strncmp(line, "EFFECTJUDGMENT row=", 19) == 0)
            fputs("EFFECTJUDGMENT none\n", input);
        else
            fputs(line, input);
    }
    fclose(original);
    fclose(input);
    read = qtt_interface_read(argv[5], &error);
    assert(!read);
    assert(error == QTT_INTERFACE_MALFORMED);

    input = fopen(argv[1], "r");
    output = fopen(argv[4], "w");
    assert(input && output);
    while (fgets(line, sizeof(line), input)) {
        if (strncmp(line, "EFFECTJUDGMENT row=", 19) == 0) {
            char *result = strstr(line, "result=1");
            assert(result);
            result[7] = '0';
        }
        fputs(line, output);
    }
    fclose(input);
    fclose(output);
    read = qtt_interface_read(argv[4], &error);
    assert(!read);
    assert(error == QTT_INTERFACE_FINGERPRINT_MISMATCH);

    input = fopen(argv[1], "r");
    output = fopen(argv[3], "w");
    assert(input && output);
    while (fgets(line, sizeof(line), input)) {
        if (strncmp(line, "EFFECTDECL ", 11) == 0) {
            char *scheme = strstr(line,
                "scheme=monad-hm-scheme-v1|78f3bd7a3897a80a");
            assert(scheme);
            scheme[strlen("scheme=monad-hm-scheme-v1|")] = 'x';
        }
        if (strncmp(line, "TRAITIMPL ", 10) == 0) {
            char *consequence = strstr(line, "consequence=");
            assert(consequence);
            consequence[12] = consequence[12] == 'i' ? 'x' : 'i';
        }
        if (strncmp(line, "HANDLERPROFILE ", 15) == 0) {
            char *usage = strstr(line, "usage=");
            assert(usage);
            usage[8] = usage[8] == '1' ? '0' : '1';
        }
        fputs(line, output);
    }
    fclose(input);
    fclose(output);
    read = qtt_interface_read(argv[3], &error);
    assert(!read);
    assert(error == QTT_INTERFACE_FINGERPRINT_MISMATCH);

    (void)int_type;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_interface_test.c"
            executable = directory / "qtt_interface_test"
            interface = directory / "Fresh.mqti"
            tampered = directory / "Fresh-tampered.mqti"
            declaration_tampered = directory / "Fresh-declaration-tampered.mqti"
            judgment_tampered = directory / "Fresh-judgment-tampered.mqti"
            judgment_missing = directory / "Fresh-judgment-missing.mqti"
            harness.write_text(source)
            compile_result = subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "interface.c"),
                    str(ROOT / "effects" / "effect.c"),
                    str(ROOT / "qtt" / "place.c"),
                    str(ROOT / "qtt" / "quantity.c"),
                    str(ROOT / "qtt" / "type_identity.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            subprocess.run(
                [str(executable), str(interface), str(tampered),
                 str(declaration_tampered), str(judgment_tampered),
                 str(judgment_missing)],
                cwd=ROOT, check=True,
            )


if __name__ == "__main__":
    unittest.main()
