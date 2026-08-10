"""HM instantiation keeps quantitative domains aligned with arrow domains."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttQuantitativeInstantiationTests(unittest.TestCase):
    def test_arrow_alignment_consumption_and_malformed_scheme_rejection(self):
        source = r'''
#include "infer.h"
#include "qtt/constraints.h"
#include "effects/effect.h"
#include <assert.h>

static Type arrow(Type *parameter, Type *result) {
    Type value = {0};
    value.kind = TYPE_ARROW;
    value.arrow_param = parameter;
    value.arrow_ret = result;
    return value;
}

int main(void) {
    Type first_parameter = {.kind = TYPE_INT};
    Type second_parameter = {.kind = TYPE_STRING};
    Type result = {.kind = TYPE_BOOL};
    Type second_arrow = arrow(&second_parameter, &result);
    Type function = arrow(&first_parameter, &second_arrow);

    QttGradeArena *source = qtt_grade_arena_new();
    QttGradeExpr *alpha = qtt_grade_fresh(source);
    QttGradeExpr *beta = qtt_grade_fresh(source);
    QttGradeExpr *twice = qtt_grade_constant(source, qtt_quantity_finite(2));
    QttGradeExpr *one = qtt_grade_constant(source, qtt_quantity_finite(1));
    QttGradeExpr *latent_use = qtt_grade_add(source, alpha, one);
    QttGradeExpr *domains[] = {alpha, twice};
    uint64_t closure_modules[] = {100, 200};
    uint64_t closure_ids[] = {9, 9};
    uint64_t latent_binders[] = {77, 77};
    size_t capture_slots[] = {0, 0};
    size_t capture_parameters[] = {0, 1};
    QttEnvironmentOriginKind capture_origin_kinds[] = {
        QTT_ENVIRONMENT_PARAMETER, QTT_ENVIRONMENT_EXPRESSION};
    uint64_t capture_origin_ids[] = {0, 900};
    QttGradeExpr *latent[] = {latent_use, twice};
    uint64_t closure_domain_modules[] = {100, 200};
    uint64_t closure_domain_ids[] = {9, 9};
    size_t closure_domain_indices[] = {0, 0};
    QttGradeExpr *closure_domains[] = {twice, alpha};
    uint64_t result_modules[] = {100};
    uint64_t result_closures[] = {9};
    size_t callable_parameters[] = {0};
    QttGradeExpr *callable_invocations[] = {alpha};
    size_t callable_domain_parameters[] = {0};
    size_t callable_domain_indices[] = {0};
    QttGradeExpr *callable_domains[] = {beta};
    QttGradeSignature signature = {
        .domain_expressions = domains,
        .domain_count = 2,
        .closure_module_ids = closure_modules,
        .closure_ids = closure_ids,
        .closure_binder_ids = latent_binders,
        .closure_slots = capture_slots,
        .closure_parameter_indices = capture_parameters,
        .closure_origin_kinds = capture_origin_kinds,
        .closure_origin_ids = capture_origin_ids,
        .closure_expressions = latent,
        .closure_count = 2,
        .closure_domain_module_ids = closure_domain_modules,
        .closure_domain_ids = closure_domain_ids,
        .closure_domain_indices = closure_domain_indices,
        .closure_domain_expressions = closure_domains,
        .closure_domain_count = 2,
        .result_closure_module_ids = result_modules,
        .result_closure_ids = result_closures,
        .result_closure_count = 1,
        .callable_parameter_indices = callable_parameters,
        .callable_invocation_expressions = callable_invocations,
        .callable_parameter_count = 1,
        .callable_domain_parameter_indices =
            callable_domain_parameters,
        .callable_domain_indices = callable_domain_indices,
        .callable_domain_expressions = callable_domains,
        .callable_domain_count = 1,
    };
    QttGradeScheme *grades =
        qtt_grade_generalize_signature(source, &signature);
    TypeScheme *scheme = scheme_mono(&function);
    assert(source && alpha && twice && grades && scheme);
    scheme_set_grade_scheme(scheme, grades);
    QttEffectArena *effect_source = qtt_effect_arena_new();
    QttEffectSolver *effect_source_solver =
        qtt_effect_solver_new(effect_source);
    QttEffectRow *effect_tail = qtt_effect_fresh(effect_source);
    QttEffectRow *effect_row = qtt_effect_extend(
        effect_source, "io", effect_tail);
    QttEffectScheme *effect_scheme = qtt_effect_generalize(
        effect_source, effect_source_solver, effect_row);
    assert(effect_source && effect_source_solver && effect_tail &&
           effect_row && effect_scheme);
    scheme_set_effect_scheme(scheme, effect_scheme, true);
    QttEffectScheme *pure_effect_scheme = qtt_effect_generalize(
        effect_source, effect_source_solver,
        qtt_effect_empty(effect_source));
    TypeScheme *pure_arrow_scheme = scheme_mono(&function);
    assert(pure_effect_scheme && pure_arrow_scheme);
    QttEffectScheme *effect_stages[] = {
        pure_effect_scheme, effect_scheme};
    bool effect_stages_complete[] = {true, true};
    assert(scheme_set_arrow_effect_schemes(
        scheme, effect_stages, effect_stages_complete, 2));
    uint64_t scheme_effect_id = scheme_effect_fingerprint(scheme);
    assert(scheme_effect_id != 0);
    scheme_set_effect_scheme(pure_arrow_scheme, pure_effect_scheme, true);
    assert(scheme_effect_fingerprint(pure_arrow_scheme) !=
           scheme_effect_id);
    scheme_free(pure_arrow_scheme);
    qtt_effect_scheme_free(pure_effect_scheme);

    InferCtx *ctx = infer_ctx_create(NULL, NULL, "quantitative-instance");
    InferQuantitativeType instance = {0};
    assert(infer_instantiate_quantitative(ctx, scheme, &instance) ==
           INFER_QUANTITATIVE_OK);
    assert(instance.type && instance.type != &function);
    assert(instance.type->kind == TYPE_ARROW);
    assert(instance.type->arrow_param->kind == TYPE_INT);
    assert(instance.type->arrow_ret->kind == TYPE_ARROW);
    assert(instance.type->arrow_effect_scheme);
    assert(instance.effects_complete);
    assert(instance.effect_arena && instance.effect_solver &&
           instance.latent_effects);
    assert(qtt_effect_label_count(
               instance.effect_solver, instance.latent_effects,
               "io") == 1);
    uint64_t first_effect_tail = qtt_effect_tail_variable(
        instance.latent_effects);
    assert(first_effect_tail != 0);
    assert(instance.domain_count == 2 && instance.domain_offset == 0);
    assert(instance.domain_effect_count == 2 &&
           instance.domain_effect_offset == 0);
    assert(qtt_effect_label_count(
               instance.effect_solver, instance.domain_effects[0],
               "io") == 0);
    assert(qtt_effect_label_count(
               instance.effect_solver, instance.domain_effects[1],
               "io") == 1);
    assert(scheme_grade_count(scheme) == 2);
    assert(scheme_closure_grade_count(scheme) == 2);
    assert(instance.closure_grade_count == 2);
    assert(instance.closure_module_ids[0] == 100);
    assert(instance.closure_ids[0] == 9);
    assert(instance.closure_binder_ids[0] == 77);
    assert(instance.closure_slots[0] == 0);
    assert(instance.closure_parameter_indices[0] == 0);
    assert(instance.closure_origin_kinds[0] == QTT_ENVIRONMENT_PARAMETER);
    assert(instance.closure_origin_ids[0] == 0);
    assert(instance.closure_module_ids[1] == 200);
    assert(instance.closure_ids[1] == 9);
    assert(instance.closure_binder_ids[1] == 77);
    assert(instance.closure_slots[1] == 0);
    assert(instance.closure_parameter_indices[1] == 1);
    assert(instance.closure_origin_kinds[1] == QTT_ENVIRONMENT_EXPRESSION);
    assert(instance.closure_origin_ids[1] == 900);
    assert(instance.result_closure_count == 1);
    assert(instance.closure_domain_grade_count == 2);
    assert(infer_quantitative_closure_domain_grade(
               &instance, 100, 9, 0) ==
           instance.closure_domain_grades[0]);
    assert(infer_quantitative_closure_domain_grade(
               &instance, 200, 9, 0) ==
           instance.closure_domain_grades[1]);
    assert(!infer_quantitative_closure_domain_grade(
               &instance, 100, 9, 1));
    assert(instance.result_closure_module_ids[0] == 100);
    assert(instance.result_closure_ids[0] == 9);
    assert(instance.callable_parameter_count == 1);
    assert(instance.callable_parameter_indices[0] == 0);
    assert(instance.callable_invocation_grades[0] ==
           instance.domain_grades[0]);
    assert(instance.callable_domain_count == 1);
    assert(instance.callable_domain_parameter_indices[0] == 0);
    assert(instance.callable_domain_indices[0] == 0);
    assert(qtt_grade_expr_kind(instance.callable_domain_grades[0]) ==
           QTT_GRADE_VARIABLE);
    assert(infer_quantitative_callable_domain_grade(
               &instance, 0, 0) ==
           instance.callable_domain_grades[0]);
    assert(!infer_quantitative_callable_domain_grade(
        &instance, 0, 1));
    assert(!infer_quantitative_callable_domain_grade(
        &instance, 1, 0));
    uint64_t first_callable_domain_variable =
        qtt_grade_variable_id(instance.callable_domain_grades[0]);

    Type *parameter = NULL;
    QttGradeExpr *grade = NULL;
    QttEffectRow *domain_effect = NULL;
    bool domain_effect_complete = false;
    assert(infer_quantitative_take_domain_contract(
        &instance, &parameter, &grade, &domain_effect,
        &domain_effect_complete));
    assert(domain_effect_complete && domain_effect ==
           instance.domain_effects[0]);
    assert(instance.domain_effect_offset == 1);
    assert(parameter && parameter->kind == TYPE_INT);
    assert(qtt_grade_expr_kind(grade) == QTT_GRADE_VARIABLE);
    uint64_t first_variable = qtt_grade_variable_id(grade);

    QttGradeExpr *latent_grade = instance.closure_grades[0];
    assert(infer_quantitative_closure_grade(&instance, 100, 9, 77) ==
           latent_grade);
    assert(infer_quantitative_closure_grade(&instance, 200, 9, 77) ==
           instance.closure_grades[1]);
    assert(infer_quantitative_closure_slot_grade(&instance, 100, 9, 0) ==
           latent_grade);
    assert(infer_quantitative_closure_slot_grade(&instance, 200, 9, 0) ==
           instance.closure_grades[1]);
    assert(!infer_quantitative_closure_slot_grade(&instance, 300, 9, 0));
    assert(!infer_quantitative_closure_grade(&instance, 300, 9, 77));
    assert(qtt_grade_expr_kind(latent_grade) == QTT_GRADE_ADD);
    QttGradeSolver *solver = qtt_grade_solver_new(ctx->grade_arena);
    QttGradeExpr *three =
        qtt_grade_constant(ctx->grade_arena, qtt_quantity_finite(3));
    assert(solver && three);
    assert(qtt_grade_constrain_equal(solver, grade, three));
    assert(qtt_grade_solve(solver) == QTT_GRADE_SOLVED);
    assert(qtt_grade_solution(solver, latent_grade).finite == 4);
    assert(qtt_grade_solution(
               solver, instance.closure_domain_grades[1]).finite == 3);
    qtt_grade_solver_free(solver);

    assert(infer_quantitative_take_domain_contract(
        &instance, &parameter, &grade, &domain_effect,
        &domain_effect_complete));
    assert(domain_effect_complete && domain_effect ==
           instance.domain_effects[1]);
    assert(instance.domain_effect_offset == 2);
    assert(parameter && parameter->kind == TYPE_STRING);
    assert(qtt_grade_expr_kind(grade) == QTT_GRADE_CONSTANT);
    assert(qtt_quantity_equal(qtt_grade_constant_value(grade),
                              qtt_quantity_finite(2)));
    assert(!infer_quantitative_take_domain(&instance, &parameter, &grade));
    infer_quantitative_type_free(&instance);

    TypeScheme *cloned = scheme_clone(scheme);
    assert(cloned);
    assert(scheme_effect_fingerprint(cloned) == scheme_effect_id);
    assert(scheme_grade_count(cloned) == 2);
    assert(scheme_closure_grade_count(cloned) == 2);
    InferQuantitativeType clone_instance = {0};
    assert(infer_instantiate_quantitative(ctx, cloned, &clone_instance) ==
           INFER_QUANTITATIVE_OK);
    assert(clone_instance.closure_binder_ids[0] == 77);
    assert(clone_instance.closure_slots[0] == 0);
    assert(clone_instance.closure_parameter_indices[1] == 1);
    assert(clone_instance.closure_module_ids[1] == 200);
    assert(clone_instance.closure_ids[1] == 9);
    assert(clone_instance.result_closure_count == 1);
    assert(clone_instance.closure_domain_grade_count == 2);
    assert(clone_instance.result_closure_module_ids[0] == 100);
    assert(clone_instance.result_closure_ids[0] == 9);
    assert(clone_instance.callable_domain_count == 1);
    assert(clone_instance.callable_domain_parameter_indices[0] == 0);
    assert(clone_instance.callable_domain_indices[0] == 0);
    assert(clone_instance.effects_complete);
    assert(qtt_effect_label_count(
               clone_instance.effect_solver,
               clone_instance.latent_effects, "io") == 1);
    assert(qtt_effect_tail_variable(clone_instance.latent_effects) !=
           first_effect_tail);
    infer_quantitative_type_free(&clone_instance);
    scheme_free(cloned);

    InferQuantitativeType fresh = {0};
    assert(infer_instantiate_quantitative(ctx, scheme, &fresh) ==
           INFER_QUANTITATIVE_OK);
    assert(qtt_grade_variable_id(fresh.domain_grades[0]) != first_variable);
    assert(qtt_grade_variable_id(fresh.callable_domain_grades[0]) !=
           first_callable_domain_variable);
    assert(fresh.effects_complete);
    assert(qtt_effect_tail_variable(fresh.latent_effects) !=
           first_effect_tail);
    infer_quantitative_type_free(&fresh);

    QttGradeExpr *only[] = {alpha};
    QttGradeScheme *malformed_grades =
        qtt_grade_generalize(source, only, 1);
    TypeScheme *malformed = scheme_mono(&function);
    scheme_set_grade_scheme(malformed, malformed_grades);
    InferQuantitativeType rejected = {0};
    assert(infer_instantiate_quantitative(ctx, malformed, &rejected) ==
           INFER_QUANTITATIVE_GRADE_ARITY_MISMATCH);
    assert(rejected.type == NULL && rejected.domain_grades == NULL);
    QttEffectScheme *one_effect_stage[] = {effect_scheme};
    bool one_effect_complete[] = {true};
    assert(scheme_set_arrow_effect_schemes(
        malformed, one_effect_stage, one_effect_complete, 1));
    scheme_set_grade_scheme(malformed, grades);
    assert(infer_instantiate_quantitative(ctx, malformed, &rejected) ==
           INFER_QUANTITATIVE_EFFECT_ARITY_MISMATCH);

    uint64_t duplicate_closures[] = {9, 9};
    uint64_t duplicate_modules[] = {100, 100};
    uint64_t duplicate_binders[] = {77, 77};
    QttGradeSignature duplicate = signature;
    duplicate.closure_module_ids = duplicate_modules;
    duplicate.closure_ids = duplicate_closures;
    duplicate.closure_binder_ids = duplicate_binders;
    assert(!qtt_grade_generalize_signature(source, &duplicate));
    size_t duplicate_slots[] = {0, 0};
    uint64_t distinct_binders[] = {77, 88};
    duplicate.closure_binder_ids = distinct_binders;
    duplicate.closure_slots = duplicate_slots;
    assert(!qtt_grade_generalize_signature(source, &duplicate));
    size_t invalid_slots[] = {1, 0};
    QttGradeSignature invalid_slot = signature;
    invalid_slot.closure_slots = invalid_slots;
    assert(!qtt_grade_generalize_signature(source, &invalid_slot));
    size_t invalid_parameters[] = {2, 1};
    QttGradeSignature invalid_parameter = signature;
    invalid_parameter.closure_parameter_indices = invalid_parameters;
    assert(!qtt_grade_generalize_signature(source, &invalid_parameter));
    size_t invalid_callable_parameters[] = {2};
    QttGradeSignature invalid_callable = signature;
    invalid_callable.callable_parameter_indices =
        invalid_callable_parameters;
    assert(!qtt_grade_generalize_signature(source, &invalid_callable));
    QttGradeSignature missing_callable_grade = signature;
    missing_callable_grade.callable_invocation_expressions = NULL;
    assert(!qtt_grade_generalize_signature(
        source, &missing_callable_grade));
    size_t orphan_callable_domain_parameters[] = {1};
    QttGradeSignature orphan_callable_domain = signature;
    orphan_callable_domain.callable_domain_parameter_indices =
        orphan_callable_domain_parameters;
    assert(!qtt_grade_generalize_signature(
        source, &orphan_callable_domain));
    QttGradeSignature missing_callable_domain = signature;
    missing_callable_domain.callable_domain_expressions = NULL;
    assert(!qtt_grade_generalize_signature(
        source, &missing_callable_domain));
    size_t duplicate_callable_domain_parameters[] = {0, 0};
    size_t duplicate_callable_domain_indices[] = {0, 0};
    QttGradeExpr *duplicate_callable_domains[] = {alpha, beta};
    QttGradeSignature duplicate_callable_domain = signature;
    duplicate_callable_domain.callable_domain_parameter_indices =
        duplicate_callable_domain_parameters;
    duplicate_callable_domain.callable_domain_indices =
        duplicate_callable_domain_indices;
    duplicate_callable_domain.callable_domain_expressions =
        duplicate_callable_domains;
    duplicate_callable_domain.callable_domain_count = 2;
    assert(!qtt_grade_generalize_signature(
        source, &duplicate_callable_domain));
    QttEnvironmentOriginKind invalid_origin_kinds[] = {
        (QttEnvironmentOriginKind)99, QTT_ENVIRONMENT_EXPRESSION};
    QttGradeSignature invalid_origin = signature;
    invalid_origin.closure_origin_kinds = invalid_origin_kinds;
    assert(!qtt_grade_generalize_signature(source, &invalid_origin));
    QttGradeSignature incomplete_origins = signature;
    incomplete_origins.closure_origin_ids = NULL;
    assert(!qtt_grade_generalize_signature(source, &incomplete_origins));
    uint64_t duplicate_domain_modules[] = {100, 100};
    QttGradeSignature duplicate_domain = signature;
    duplicate_domain.closure_domain_module_ids = duplicate_domain_modules;
    assert(!qtt_grade_generalize_signature(source, &duplicate_domain));
    size_t invalid_domain_indices[] = {1, 0};
    QttGradeSignature invalid_domain = signature;
    invalid_domain.closure_domain_indices = invalid_domain_indices;
    assert(!qtt_grade_generalize_signature(source, &invalid_domain));
    uint64_t duplicate_result_modules[] = {100, 100};
    uint64_t duplicate_result_ids[] = {9, 9};
    QttGradeSignature duplicate_results = signature;
    duplicate_results.result_closure_module_ids =
        duplicate_result_modules;
    duplicate_results.result_closure_ids = duplicate_result_ids;
    duplicate_results.result_closure_count = 2;
    assert(!qtt_grade_generalize_signature(source, &duplicate_results));

    scheme_free(malformed);
    qtt_grade_scheme_free(malformed_grades);
    infer_ctx_free(ctx);
    scheme_free(scheme);
    qtt_effect_scheme_free(effect_scheme);
    qtt_effect_solver_free(effect_source_solver);
    qtt_effect_arena_free(effect_source);
    qtt_grade_scheme_free(grades);
    qtt_grade_arena_free(source);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_quantitative_instantiation_test.c"
            executable = directory / "qtt_quantitative_instantiation_test"
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
                    str(ROOT / "qtt" / "place.c"),
                    "-Wl,--gc-sections", "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
