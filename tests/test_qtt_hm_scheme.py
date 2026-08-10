"""HM TypeScheme owns and freshens its quantitative grade scheme."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttHmSchemeTests(unittest.TestCase):
    def test_clone_free_and_polymorphic_grade_instantiation(self):
        source = r'''
#include "infer.h"
#include "effects/effect.h"
#include "qtt/constraints.h"
#include "effects/constraints.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

bool parse_int_type(const char *name, int line, int col,
                    int *width, bool *is_signed) {
    (void)name; (void)line; (void)col; (void)width; (void)is_signed;
    return false;
}

int main(void) {
    Type *effect_syntax = type_from_name(
        "Int -e-> String -io.read-> Unit");
    assert(effect_syntax && effect_syntax->kind == TYPE_ARROW);
    assert(effect_syntax->arrow_effect_name &&
           strcmp(effect_syntax->arrow_effect_name, "e") == 0);
    assert(effect_syntax->arrow_ret->kind == TYPE_ARROW);
    assert(effect_syntax->arrow_ret->arrow_effect_name &&
           strcmp(effect_syntax->arrow_ret->arrow_effect_name,
                  "io.read") == 0);
    assert(strcmp(type_to_string(effect_syntax),
                  "Int -e-> String -io.read-> ()") == 0);
    Type *effect_syntax_clone = type_clone(effect_syntax);
    assert(types_equal(effect_syntax, effect_syntax_clone));
    type_free(effect_syntax_clone);
    type_free(effect_syntax);

    QttGradeArena *source = qtt_grade_arena_new();
    QttGradeExpr *alpha = qtt_grade_fresh(source);
    QttGradeExpr *domains[] = {alpha};
    QttGradeScheme *grades = qtt_grade_generalize(source, domains, 1);
    TypeScheme *scheme = scheme_mono(NULL);
    assert(source && alpha && grades && scheme);
    scheme_set_grade_scheme(scheme, grades);

    TypeScheme *clone = scheme_clone(scheme);
    assert(clone && scheme_grade_count(clone) == 1);
    scheme_free(scheme);
    qtt_grade_scheme_free(grades);
    qtt_grade_arena_free(source);

    QttGradeArena *target = qtt_grade_arena_new();
    size_t first_count = 0, second_count = 0;
    QttGradeExpr **first =
        scheme_instantiate_grades(clone, target, &first_count);
    QttGradeExpr **second =
        scheme_instantiate_grades(clone, target, &second_count);
    assert(first && second && first_count == 1 && second_count == 1);
    assert(qtt_grade_variable_id(first[0]) !=
           qtt_grade_variable_id(second[0]));

    InferCtx *ctx = infer_ctx_create(NULL, NULL, "grade-test");
    size_t ctx_first_count = 0, ctx_second_count = 0;
    QttGradeExpr **ctx_first = infer_instantiate_scheme_grades(
        ctx, clone, &ctx_first_count);
    QttGradeExpr **ctx_second = infer_instantiate_scheme_grades(
        ctx, clone, &ctx_second_count);
    assert(ctx_first && ctx_second);
    assert(ctx_first_count == 1 && ctx_second_count == 1);
    assert(qtt_grade_variable_id(ctx_first[0]) !=
           qtt_grade_variable_id(ctx_second[0]));

    free(ctx_first);
    free(ctx_second);
    infer_ctx_free(ctx);
    free(first);
    free(second);
    scheme_free(clone);
    qtt_grade_arena_free(target);

    QttEffectArena *effect_arena = qtt_effect_arena_new();
    QttEffectSolver *effect_solver = qtt_effect_solver_new(effect_arena);
    QttEffectScheme *pure = qtt_effect_generalize(
        effect_arena, effect_solver, qtt_effect_empty(effect_arena));
    QttEffectScheme *io = qtt_effect_generalize(
        effect_arena, effect_solver,
        qtt_effect_extend(
            effect_arena, "io", qtt_effect_empty(effect_arena)));
    TypeScheme *callable = scheme_mono(type_arrow(
        type_int(), type_arrow(type_string(), type_unit())));
    QttEffectScheme *stages[] = {pure, io};
    bool complete[] = {true, true};
    assert(callable && pure && io);
    assert(scheme_set_arrow_effect_schemes(
        callable, stages, complete, 2));
    size_t predicate_stages[] = {1, 0, 1};
    const char *predicate_traits[] = {"io", "state", "io"};
    assert(scheme_set_effect_trait_predicates(
        callable, predicate_stages, predicate_traits, 3));
    assert(scheme_effect_trait_predicate_count(callable) == 2);
    assert(scheme_effect_trait_predicate_stage(callable, 0) == 0);
    assert(strcmp(scheme_effect_trait_predicate_name(callable, 0),
                  "state") == 0);
    assert(callable->type->arrow_effect_scheme);
    assert(callable->type->arrow_effect_complete);
    assert(callable->type->arrow_ret->arrow_effect_scheme);
    assert(callable->type->arrow_ret->arrow_effect_complete);
    char *pure_text = qtt_effect_scheme_serialize(pure);
    char *io_text = qtt_effect_scheme_serialize(io);
    assert(strcmp(callable->type->arrow_effect_scheme, pure_text) == 0);
    assert(strcmp(
        callable->type->arrow_ret->arrow_effect_scheme, io_text) == 0);

    TypeScheme *callable_clone = scheme_clone(callable);
    scheme_free(callable);
    assert(callable_clone->type->arrow_effect_scheme);
    assert(scheme_effect_trait_predicate_count(callable_clone) == 2);
    assert(strcmp(callable_clone->type->arrow_effect_scheme, pure_text) == 0);
    assert(strcmp(
        callable_clone->type->arrow_ret->arrow_effect_scheme, io_text) == 0);
    char *hm_text = infer_type_scheme_serialize(callable_clone);
    TypeScheme *roundtrip = infer_type_scheme_deserialize(hm_text);
    assert(hm_text && roundtrip);
    assert(strstr(hm_text, "monad-hm-scheme-v2|") == hm_text);
    assert(scheme_effect_trait_predicate_count(roundtrip) == 2);
    assert(scheme_effect_trait_predicate_stage(roundtrip, 1) == 1);
    assert(strcmp(scheme_effect_trait_predicate_name(roundtrip, 1), "io") == 0);
    QttEffectConstraintSet *trait_constraints = qtt_effect_constraints_new();
    assert(scheme_instantiate_effect_trait_constraints(
        roundtrip, effect_arena, trait_constraints));
    QttEffectConstraintCertificate *trait_certificate = NULL;
    assert(qtt_effect_constraints_solve(
        trait_constraints, effect_arena, effect_solver,
        &trait_certificate) == QTT_EFFECT_CONSTRAINT_REJECTED);
    assert(qtt_effect_certificate_count(trait_certificate) == 2);
    qtt_effect_certificate_free(trait_certificate);
    qtt_effect_constraints_free(trait_constraints);
    assert(roundtrip->arrow_effect_count == 2);
    assert(roundtrip->arrow_effects_complete[0]);
    assert(roundtrip->arrow_effects_complete[1]);
    assert(roundtrip->type->arrow_effect_scheme);
    assert(strcmp(roundtrip->type->arrow_effect_scheme, pure_text) == 0);
    assert(strcmp(
        roundtrip->type->arrow_ret->arrow_effect_scheme, io_text) == 0);
    scheme_set_effect_scheme(roundtrip, pure, true);
    InferCallableContract contract = {0};
    assert(infer_callable_contract_from_scheme(&contract, roundtrip));
    assert(contract.effect_trait_predicate_count == 2);
    assert(contract.effect_trait_predicate_stages[1] == 1);
    char *contract_text = infer_callable_contract_serialize(&contract);
    InferCallableContract restored_contract = {0};
    assert(contract_text && strstr(
        contract_text, "monad-callable-contract-v2|") == contract_text);
    assert(infer_callable_contract_deserialize(
        &restored_contract, contract_text));
    assert(restored_contract.effect_trait_predicate_count == 2);
    assert(strcmp(
        restored_contract.effect_trait_predicate_names[0], "state") == 0);
    TypeScheme *contract_scheme = scheme_mono(type_arrow(
        type_int(), type_arrow(type_string(), type_unit())));
    assert(scheme_set_callable_contract(contract_scheme, &restored_contract));
    assert(scheme_effect_trait_predicate_count(contract_scheme) == 2);
    infer_callable_contract_free(&restored_contract);
    contract_text[strlen(contract_text) - 1] = 'x';
    assert(!infer_callable_contract_deserialize(
        &restored_contract, contract_text));
    infer_callable_contract_free(&contract);
    scheme_free(contract_scheme);
    free(contract_text);
    roundtrip->quantified = malloc(sizeof(*roundtrip->quantified));
    roundtrip->quantified[0] = 700;
    roundtrip->quantified_count = 1;
    type_free(roundtrip->type->arrow_param);
    roundtrip->type->arrow_param = type_var(700);
    InferCtx *effect_ctx = infer_ctx_create(NULL, NULL, "effect-arrow-test");
    Type *effect_instance = infer_instantiate(effect_ctx, roundtrip);
    assert(effect_instance && effect_instance->kind == TYPE_ARROW);
    assert(effect_instance->arrow_effect_scheme);
    assert(strcmp(effect_instance->arrow_effect_scheme, pure_text) == 0);
    infer_ctx_free(effect_ctx);
    free(hm_text);
    scheme_free(roundtrip);
    free(pure_text);
    free(io_text);
    scheme_free(callable_clone);
    qtt_effect_scheme_free(pure);
    qtt_effect_scheme_free(io);
    qtt_effect_solver_free(effect_solver);
    qtt_effect_arena_free(effect_arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_hm_scheme_test.c"
            executable = directory / "qtt_hm_scheme_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-D_GNU_SOURCE",
                    "-Wall", "-Wextra", "-Werror", "-Wno-switch",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-ffunction-sections", "-fdata-sections",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "infer.c"),
                    str(ROOT / "types.c"),
                    str(ROOT / "qtt" / "quantity.c"),
                    str(ROOT / "qtt" / "constraints.c"),
                    str(ROOT / "qtt" / "environment.c"),
                    str(ROOT / "effects" / "effect.c"),
                    str(ROOT / "effects" / "constraints.c"),
                    str(ROOT / "qtt" / "place.c"),
                    str(ROOT / "qtt" / "type_identity.c"),
                    "-Wl,--gc-sections", "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
