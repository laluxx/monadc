"""Ordinary HM source inference derives latent effect schemes."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class SourceEffectInferenceTests(unittest.TestCase):
    def test_lambda_separates_latent_effects_and_imports_callee_scheme(self):
        source = r'''
#include "infer.h"
#include "effects/effect.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int g_trace_depth = 0;
bool g_trace_enabled = false;
void shared_trace_indent(void) {}
const char *current_filename = "source-effects";
char g_reader_error_msg[512];
jmp_buf g_reader_escape;
bool g_reader_escape_set = false;
bool parse_int_type(const char *name, int line, int col,
                    int *width, bool *is_signed) {
    (void)name; (void)line; (void)col; (void)width; (void)is_signed;
    return false;
}

static AST symbol(char *name) {
    AST value = {.type = AST_SYMBOL};
    value.symbol = name;
    return value;
}

int main(void) {
    InferEnv *environment = infer_env_create();
    InferCtx *ctx = infer_ctx_create(
        environment, NULL, "source-effects");
    assert(environment && ctx);
    QttEffectDeclaration foreign_asm = {
        .name = "Core.Foreign.asm",
        .traits = "foreign",
        .kind = QTT_EFFECT_FOREIGN,
        .operation = "asm",
        .resumption = {.finite = 1},
    };
    assert(qtt_effect_declaration_register(&foreign_asm));
    QttEffectDeclaration heap_allocation = {
        .name = "Core.Alloc.heap",
        .traits = "allocate",
        .kind = QTT_EFFECT_ALLOCATE,
        .operation = "heap",
        .resumption = {.finite = 1},
    };
    QttEffectDeclaration state_write = {
        .name = "Core.State.write",
        .traits = "state",
        .kind = QTT_EFFECT_STATE,
        .operation = "write",
        .resumption = {.finite = 1},
    };
    assert(qtt_effect_declaration_register(&heap_allocation));
    assert(qtt_effect_declaration_register(&state_write));
    QttEffectDeclaration network_send = {
        .name = "Test.Network.send",
        .traits = "network",
        .kind = QTT_EFFECT_FOREIGN,
        .operation = "send",
        .resumption = {.finite = 1},
    };
    assert(qtt_effect_declaration_register(&network_send));
    QttEffectDeclaration core_io_read = {
        .name = "Core.IO.read",
        .traits = "io",
        .kind = QTT_EFFECT_IO,
        .operation = "read",
        .resumption = {.finite = 1},
    };
    assert(qtt_effect_declaration_register(&core_io_read));

    AST perform_head = symbol("perform");
    AST perform_effect = symbol("Core.IO.read");
    AST perform_unit = {.type = AST_STRING, .string = "prompt"};
    AST *perform_items[] = {
        &perform_head, &perform_effect, &perform_unit};
    AST perform_read = {
        .type = AST_LIST,
        .list = {.items = perform_items, .count = 3},
    };
    bool perform_complete = false;
    QttEffectScheme *perform_scheme = infer_effect_scheme_for_expression(
        ctx, &perform_read, &perform_complete);
    assert(perform_scheme && perform_complete);
    QttEffectArena *perform_arena = qtt_effect_arena_new();
    QttEffectSolver *perform_solver = qtt_effect_solver_new(perform_arena);
    QttEffectRow *perform_row = qtt_effect_instantiate(
        perform_arena, perform_scheme);
    const QttEffectDeclaration *read_decl =
        qtt_effect_declaration_lookup("Core.IO.read");
    QttEffectAtom perform_read_atom = {
        .traits = read_decl->traits, .kind = read_decl->kind,
        .constructor_id = read_decl->constructor_id,
        .name = read_decl->name, .operation = read_decl->operation,
        .resumption = read_decl->resumption,
    };
    assert(perform_row && qtt_effect_atom_count(
        perform_solver, perform_row, &perform_read_atom) == 1);
    qtt_effect_solver_free(perform_solver);
    qtt_effect_arena_free(perform_arena);
    qtt_effect_scheme_free(perform_scheme);

    Type number = {.kind = TYPE_INT};
    Type io_arrow = {.kind = TYPE_ARROW,
                     .arrow_param = &number,
                     .arrow_ret = &number};
    TypeScheme *io_scheme = scheme_mono(&io_arrow);
    QttEffectArena *source_arena = qtt_effect_arena_new();
    QttEffectSolver *source_solver =
        qtt_effect_solver_new(source_arena);
    QttEffectRow *io_row = qtt_effect_extend(
        source_arena, "io", qtt_effect_empty(source_arena));
    QttEffectScheme *io_effects = qtt_effect_generalize(
        source_arena, source_solver, io_row);
    assert(io_scheme && source_solver && io_effects);
    scheme_set_effect_scheme(io_scheme, io_effects, true);
    infer_env_insert(environment, "ioFn", io_scheme);
    Type canonical_number = {.kind = TYPE_INT};
    Type canonical_arrow = {.kind = TYPE_ARROW,
                            .arrow_param = &canonical_number,
                            .arrow_ret = &canonical_number};
    TypeScheme *canonical_io_scheme = scheme_mono(&canonical_arrow);
    QttEffectScheme *canonical_stages[] = {io_effects};
    bool canonical_complete[] = {true};
    assert(scheme_set_arrow_effect_schemes(
        canonical_io_scheme, canonical_stages, canonical_complete, 1));
    assert(canonical_io_scheme->type->arrow_effect_scheme);
    /* The canonical arrow must remain authoritative without the migration
     * side vector. */
    qtt_effect_scheme_free(canonical_io_scheme->arrow_effect_schemes[0]);
    free(canonical_io_scheme->arrow_effect_schemes);
    free(canonical_io_scheme->arrow_effects_complete);
    canonical_io_scheme->arrow_effect_schemes = NULL;
    canonical_io_scheme->arrow_effects_complete = NULL;
    canonical_io_scheme->arrow_effect_count = 0;
    infer_env_insert(environment, "canonicalIo", canonical_io_scheme);
    Type curried_tail = {.kind = TYPE_ARROW,
                         .arrow_param = &number,
                         .arrow_ret = &number};
    Type curried_type = {.kind = TYPE_ARROW,
                         .arrow_param = &number,
                         .arrow_ret = &curried_tail};
    TypeScheme *curried_scheme = scheme_mono(&curried_type);
    scheme_set_effect_scheme(curried_scheme, io_effects, true);
    QttEffectScheme *pure_effects = qtt_effect_generalize(
        source_arena, source_solver, qtt_effect_empty(source_arena));
    TypeScheme *pure_function_scheme = scheme_mono(&io_arrow);
    scheme_set_effect_scheme(pure_function_scheme, pure_effects, true);
    QttEffectScheme *pure_function_stages[] = {pure_effects};
    bool pure_function_complete[] = {true};
    assert(scheme_set_arrow_effect_schemes(
        pure_function_scheme, pure_function_stages,
        pure_function_complete, 1));
    infer_env_insert(environment, "pureFn", pure_function_scheme);
    QttEffectScheme *curried_stages[] = {pure_effects, io_effects};
    bool curried_complete[] = {true, true};
    assert(pure_effects && scheme_set_arrow_effect_schemes(
        curried_scheme, curried_stages, curried_complete, 2));
    assert(!qtt_effect_scheme_set_evidence(
        pure_effects, "monad-effect-certificate-v1\n", 0, 7,
        QTT_EFFECT_EVIDENCE_SOLVED));
    infer_env_insert(environment, "curriedIo", curried_scheme);
    Type canonical_curried_result = {.kind = TYPE_INT};
    Type canonical_curried_tail = {
        .kind = TYPE_ARROW,
        .arrow_param = &canonical_number,
        .arrow_ret = &canonical_curried_result};
    Type canonical_curried_type = {
        .kind = TYPE_ARROW,
        .arrow_param = &canonical_number,
        .arrow_ret = &canonical_curried_tail};
    TypeScheme *canonical_curried_scheme =
        scheme_mono(&canonical_curried_type);
    assert(scheme_set_arrow_effect_schemes(
        canonical_curried_scheme, curried_stages, curried_complete, 2));
    for (size_t i = 0;
         i < canonical_curried_scheme->arrow_effect_count; i++)
        qtt_effect_scheme_free(
            canonical_curried_scheme->arrow_effect_schemes[i]);
    free(canonical_curried_scheme->arrow_effect_schemes);
    free(canonical_curried_scheme->arrow_effects_complete);
    canonical_curried_scheme->arrow_effect_schemes = NULL;
    canonical_curried_scheme->arrow_effects_complete = NULL;
    canonical_curried_scheme->arrow_effect_count = 0;
    infer_env_insert(
        environment, "canonicalCurried", canonical_curried_scheme);
    Type shared_row_tail = {.kind = TYPE_ARROW,
                            .arrow_param = &number,
                            .arrow_ret = &number,
                            .arrow_effect_name = "e"};
    Type shared_row_type = {.kind = TYPE_ARROW,
                            .arrow_param = &number,
                            .arrow_ret = &shared_row_tail,
                            .arrow_effect_name = "e"};
    TypeScheme *shared_row_scheme = scheme_mono(&shared_row_type);
    assert(scheme_set_arrow_effect_schemes(
        shared_row_scheme, curried_stages, curried_complete, 2));
    infer_env_insert(environment, "sharedRow", shared_row_scheme);
    Type distinct_row_tail = {.kind = TYPE_ARROW,
                              .arrow_param = &number,
                              .arrow_ret = &number,
                              .arrow_effect_name = "f"};
    Type distinct_row_type = {.kind = TYPE_ARROW,
                              .arrow_param = &number,
                              .arrow_ret = &distinct_row_tail,
                              .arrow_effect_name = "e"};
    TypeScheme *distinct_row_scheme = scheme_mono(&distinct_row_type);
    assert(scheme_set_arrow_effect_schemes(
        distinct_row_scheme, curried_stages, curried_complete, 2));
    infer_env_insert(environment, "distinctRows", distinct_row_scheme);
    QttEffectScheme *open_empty_effects = qtt_effect_generalize(
        source_arena, source_solver, qtt_effect_fresh(source_arena));
    Type captured_row_tail = {.kind = TYPE_ARROW,
                              .arrow_param = &number,
                              .arrow_ret = &number,
                              .arrow_effect_name = "e"};
    Type captured_row_type = {.kind = TYPE_ARROW,
                              .arrow_param = &number,
                              .arrow_ret = &captured_row_tail,
                              .arrow_effect_name = "e"};
    TypeScheme *captured_row_scheme = scheme_mono(&captured_row_type);
    QttEffectScheme *captured_row_stages[] = {
        io_effects, open_empty_effects};
    assert(open_empty_effects && scheme_set_arrow_effect_schemes(
        captured_row_scheme, captured_row_stages, curried_complete, 2));
    infer_env_insert(environment, "capturedRow", captured_row_scheme);
    QttEffectScheme *dependent_open = qtt_effect_generalize(
        source_arena, source_solver, qtt_effect_fresh(source_arena));
    size_t dependent_parameter[] = {0};
    size_t dependent_arity[] = {1};
    assert(dependent_open &&
        qtt_effect_scheme_set_callable_parameter_contracts(
            dependent_open, dependent_parameter, dependent_arity, 1));
    Type dependent_binder_type = {.kind = TYPE_ARROW,
                                  .arrow_param = &number,
                                  .arrow_ret = &number,
                                  .arrow_effect_name = "e"};
    TypeScheme *dependent_binder = scheme_mono(&dependent_binder_type);
    QttEffectScheme *dependent_stages[] = {dependent_open};
    assert(scheme_set_arrow_effect_schemes(
        dependent_binder, dependent_stages, canonical_complete, 1));
    assert(infer_elaborate_effect_row_binders(ctx, dependent_binder));
    QttEffectScheme *elaborated_dependent =
        dependent_binder->arrow_effect_schemes[0];
    assert(elaborated_dependent);
    assert(!dependent_binder->arrow_effects_complete[0]);
    assert(qtt_effect_scheme_callable_parameter_count(
        elaborated_dependent) == 1);
    assert(qtt_effect_scheme_callable_parameter_index(
        elaborated_dependent, 0) == 0);
    assert(qtt_effect_scheme_callable_parameter_arity(
        elaborated_dependent, 0) == 1);
    scheme_free(dependent_binder);
    QttEffectRow *open_io_row = qtt_effect_extend(
        source_arena, "network", qtt_effect_fresh(source_arena));
    QttEffectScheme *open_io_effects = qtt_effect_generalize(
        source_arena, source_solver, open_io_row);
    TypeScheme *open_io_scheme = scheme_mono(&io_arrow);
    assert(open_io_effects && open_io_scheme);
    scheme_set_effect_scheme(open_io_scheme, open_io_effects, false);
    QttEffectScheme *open_stages[] = {open_io_effects};
    bool open_complete[] = {false};
    assert(scheme_set_arrow_effect_schemes(
        open_io_scheme, open_stages, open_complete, 1));
    infer_env_insert(environment, "openNetwork", open_io_scheme);
    QttEffectRow *network_row = qtt_effect_extend(
        source_arena, "network", qtt_effect_empty(source_arena));
    QttEffectScheme *network_effects = qtt_effect_generalize(
        source_arena, source_solver, network_row);
    TypeScheme *network_scheme = scheme_mono(&io_arrow);
    assert(network_effects && network_scheme);
    scheme_set_effect_scheme(network_scheme, network_effects, true);
    QttEffectScheme *network_stages[] = {network_effects};
    bool network_complete[] = {true};
    assert(scheme_set_arrow_effect_schemes(
        network_scheme, network_stages, network_complete, 1));
    infer_env_insert(environment, "networkFn", network_scheme);
    Type concrete_bad_type = {.kind = TYPE_ARROW,
                              .arrow_param = &number,
                              .arrow_ret = &number,
                              .arrow_effect_name = "io.read"};
    TypeScheme *concrete_bad = scheme_mono(&concrete_bad_type);
    QttEffectScheme *concrete_bad_stages[] = {pure_effects};
    assert(scheme_set_arrow_effect_schemes(
        concrete_bad, concrete_bad_stages, canonical_complete, 1));
    infer_env_insert(environment, "concreteBad", concrete_bad);
    QttEffectRow *declared_read_row = qtt_effect_extend_declared(
        source_arena, "Core.IO.read", 0, 0,
        qtt_effect_empty(source_arena));
    QttEffectScheme *declared_read_effects = qtt_effect_generalize(
        source_arena, source_solver, declared_read_row);
    TypeScheme *concrete_good = scheme_mono(&concrete_bad_type);
    QttEffectScheme *concrete_good_stages[] = {declared_read_effects};
    assert(declared_read_effects && scheme_set_arrow_effect_schemes(
        concrete_good, concrete_good_stages, canonical_complete, 1));
    infer_env_insert(environment, "concreteGood", concrete_good);
    QttEffectRow *qualified_network_row = qtt_effect_extend_declared(
        source_arena, "Test.Network.send", 0, 0,
        qtt_effect_empty(source_arena));
    QttEffectScheme *qualified_network_effects = qtt_effect_generalize(
        source_arena, source_solver, qualified_network_row);
    QttEffectScheme *qualified_network_stages[] = {
        qualified_network_effects};
    Type qualified_network_type = {.kind = TYPE_ARROW,
                                   .arrow_param = &number,
                                   .arrow_ret = &number};
    TypeScheme *qualified_network = scheme_mono(&qualified_network_type);
    assert(scheme_set_arrow_effect_schemes(
        qualified_network, qualified_network_stages,
        network_complete, 1));
    size_t qualified_stage[] = {0};
    const char *qualified_trait[] = {"io"};
    assert(scheme_set_effect_trait_predicates(
        qualified_network, qualified_stage, qualified_trait, 1));
    infer_env_insert(environment, "qualifiedNetwork", qualified_network);
    Type qualified_open_type = {.kind = TYPE_ARROW,
                                .arrow_param = &number,
                                .arrow_ret = &number};
    TypeScheme *qualified_open = scheme_mono(&qualified_open_type);
    QttEffectRow *qualified_open_row = qtt_effect_extend_declared(
        source_arena, "Test.Network.send", 0, 0,
        qtt_effect_fresh(source_arena));
    QttEffectScheme *qualified_open_effects = qtt_effect_generalize(
        source_arena, source_solver, qualified_open_row);
    QttEffectScheme *qualified_open_stages[] = {qualified_open_effects};
    assert(scheme_set_arrow_effect_schemes(
        qualified_open, qualified_open_stages, open_complete, 1));
    assert(scheme_set_effect_trait_predicates(
        qualified_open, qualified_stage, qualified_trait, 1));
    infer_env_insert(environment, "qualifiedOpen", qualified_open);
    Type qualified_curried_tail = {.kind = TYPE_ARROW,
                                   .arrow_param = &number,
                                   .arrow_ret = &number};
    Type qualified_curried_type = {.kind = TYPE_ARROW,
                                   .arrow_param = &number,
                                   .arrow_ret = &qualified_curried_tail};
    TypeScheme *qualified_curried = scheme_mono(&qualified_curried_type);
    QttEffectScheme *qualified_curried_stages[] = {
        pure_effects, qualified_network_effects};
    bool qualified_curried_complete[] = {true, true};
    size_t qualified_curried_stage[] = {1};
    assert(scheme_set_arrow_effect_schemes(
        qualified_curried, qualified_curried_stages,
        qualified_curried_complete, 2));
    assert(scheme_set_effect_trait_predicates(
        qualified_curried, qualified_curried_stage,
        qualified_trait, 1));
    infer_env_insert(environment, "qualifiedCurried", qualified_curried);
    TypeScheme *truth_scheme = scheme_mono(type_bool());
    infer_env_insert(environment, "truth", truth_scheme);

    AST set_head = symbol("set!");
    AST target = symbol("x");
    AST replacement = {.type = AST_NUMBER, .number = 1};
    AST *set_items[] = {&set_head, &target, &replacement};
    AST mutation = {
        .type = AST_LIST,
        .list = {.items = set_items, .count = 3}};

    AST io_head = symbol("ioFn");
    AST argument = {.type = AST_NUMBER, .number = 2};
    AST *call_items[] = {&io_head, &argument};
    AST call = {
        .type = AST_LIST,
        .list = {.items = call_items, .count = 2}};
    AST canonical_head = symbol("canonicalIo");
    AST *canonical_call_items[] = {&canonical_head, &argument};
    AST canonical_call = {
        .type = AST_LIST,
        .list = {.items = canonical_call_items, .count = 2}};
    InferExpressionJudgment canonical_typed = {0};
    assert(infer_toplevel_judgment(ctx, &canonical_call, &canonical_typed));
    assert(canonical_typed.effects && canonical_typed.effects_complete);
    QttEffectArena *canonical_arena = qtt_effect_arena_new();
    QttEffectSolver *canonical_solver =
        qtt_effect_solver_new(canonical_arena);
    QttEffectRow *canonical_row = qtt_effect_instantiate(
        canonical_arena, canonical_typed.effects);
    assert(canonical_row && qtt_effect_label_count(
        canonical_solver, canonical_row, "io") == 1);
    qtt_effect_solver_free(canonical_solver);
    qtt_effect_arena_free(canonical_arena);
    infer_expression_judgment_free(&canonical_typed);

    AST canonical_curried_head = symbol("canonicalCurried");
    AST *canonical_partial_items[] = {
        &canonical_curried_head, &argument};
    AST canonical_partial = {
        .type = AST_LIST,
        .list = {.items = canonical_partial_items, .count = 2}};
    assert(infer_toplevel_judgment(
        ctx, &canonical_partial, &canonical_typed));
    assert(canonical_typed.type &&
           canonical_typed.type->kind == TYPE_ARROW);
    assert(canonical_typed.effects && canonical_typed.effects_complete);
    assert(canonical_typed.arrow_effect_count == 1);
    canonical_arena = qtt_effect_arena_new();
    canonical_solver = qtt_effect_solver_new(canonical_arena);
    canonical_row = qtt_effect_instantiate(
        canonical_arena, canonical_typed.arrow_effect_schemes[0]);
    assert(canonical_row && qtt_effect_label_count(
        canonical_solver, canonical_row, "io") == 1);
    qtt_effect_solver_free(canonical_solver);
    qtt_effect_arena_free(canonical_arena);
    infer_expression_judgment_free(&canonical_typed);
    AST *body[] = {&mutation, &call};
    AST lambda = {
        .type = AST_LAMBDA,
        .lambda = {.body_exprs = body, .body_count = 2,
                   .body = &call}};

    ASTParam typed_param = {.name = "unused"};
    AST *typed_body[] = {&call};
    AST typed_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.params = &typed_param, .param_count = 1,
                   .body_exprs = typed_body, .body_count = 1,
                   .body = &call}};
    InferExpressionJudgment typed = {0};
    assert(infer_toplevel_judgment(ctx, &typed_lambda, &typed));
    assert(typed.type && typed.type->kind == TYPE_ARROW);
    assert(typed.effects && typed.effects_complete);
    assert(typed.recursively_fused);
    assert(qtt_effect_scheme_evidence_intact(typed.effects));
    assert(qtt_effect_scheme_coverage_gaps(typed.effects) == 0);
    infer_expression_judgment_free(&typed);

    ASTParam higher_params[] = {{.name = "f"}, {.name = "v"}};
    AST higher_head = symbol("f");
    AST higher_arg = symbol("v");
    AST *higher_call_items[] = {&higher_head, &higher_arg};
    AST higher_call = {
        .type = AST_LIST,
        .list = {.items = higher_call_items, .count = 2}};
    AST *higher_body[] = {&higher_call};
    AST higher_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.params = higher_params, .param_count = 2,
                   .body_exprs = higher_body, .body_count = 1,
                   .body = &higher_call}};
    assert(infer_toplevel_judgment(ctx, &higher_lambda, &typed));
    assert(typed.recursively_fused && !typed.effects_complete);
    assert(qtt_effect_scheme_coverage_gaps(typed.effects) &
           QTT_EFFECT_COVERAGE_OPEN_ROW);
    assert(!(qtt_effect_scheme_coverage_gaps(typed.effects) &
             QTT_EFFECT_COVERAGE_MISSING_ARROW_CONTRACT));
    assert(qtt_effect_scheme_callable_parameter_count(typed.effects) == 1);
    assert(qtt_effect_scheme_callable_parameter_index(typed.effects, 0) == 0);
    assert(qtt_effect_scheme_callable_parameter_arity(typed.effects, 0) == 1);
    TypeScheme *higher_scheme = scheme_mono(typed.type);
    scheme_set_effect_scheme(higher_scheme, typed.effects, false);
    QttEffectScheme *higher_stages[] = {pure_effects, typed.effects};
    bool higher_complete[] = {true, false};
    assert(scheme_set_arrow_effect_schemes(
        higher_scheme, higher_stages, higher_complete, 2));
    assert(higher_scheme->type->arrow_ret->arrow_effect_scheme);
    QttEffectScheme *higher_canonical_stage =
        qtt_effect_scheme_deserialize(
            higher_scheme->type->arrow_ret->arrow_effect_scheme);
    assert(higher_canonical_stage);
    assert(qtt_effect_scheme_callable_parameter_count(
        higher_canonical_stage) == 1);
    qtt_effect_scheme_free(higher_canonical_stage);
    infer_env_insert(environment, "applyHigher", higher_scheme);
    infer_expression_judgment_free(&typed);

    AST apply_higher_head = symbol("applyHigher");
    AST io_value = symbol("ioFn");
    AST *apply_higher_items[] = {
        &apply_higher_head, &io_value, &argument};
    AST apply_higher = {
        .type = AST_LIST,
        .list = {.items = apply_higher_items, .count = 3}};
    assert(infer_toplevel_judgment(ctx, &apply_higher, &typed));
    assert(typed.recursively_fused && typed.effects_complete);
    QttEffectArena *higher_arena = qtt_effect_arena_new();
    QttEffectSolver *higher_solver = qtt_effect_solver_new(higher_arena);
    QttEffectRow *higher_row = qtt_effect_instantiate(
        higher_arena, typed.effects);
    assert(higher_row && qtt_effect_is_closed(higher_solver, higher_row));
    assert(qtt_effect_label_count(higher_solver, higher_row, "io") == 1);
    qtt_effect_solver_free(higher_solver);
    qtt_effect_arena_free(higher_arena);
    infer_expression_judgment_free(&typed);

    assert(infer_toplevel_judgment(ctx, &higher_lambda, &typed));
    TypeScheme *higher_inline_scheme = scheme_mono(typed.type);
    scheme_set_effect_scheme(higher_inline_scheme, typed.effects, false);
    QttEffectScheme *higher_inline_stages[] = {pure_effects, typed.effects};
    assert(scheme_set_arrow_effect_schemes(
        higher_inline_scheme, higher_inline_stages, higher_complete, 2));
    infer_env_insert(environment, "applyHigherInline", higher_inline_scheme);
    infer_expression_judgment_free(&typed);

    ASTParam inline_param = {.name = "inlineValue"};
    AST inline_io_head = symbol("ioFn");
    AST inline_value = symbol("inlineValue");
    AST *inline_io_items[] = {&inline_io_head, &inline_value};
    AST inline_io_call = {
        .type = AST_LIST,
        .list = {.items = inline_io_items, .count = 2}};
    AST *inline_body[] = {&inline_io_call};
    AST inline_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.params = &inline_param, .param_count = 1,
                   .body_exprs = inline_body, .body_count = 1,
                   .body = &inline_io_call}};
    AST apply_inline_head = symbol("applyHigherInline");
    AST *apply_inline_items[] = {
        &apply_inline_head, &inline_lambda, &argument};
    AST apply_inline = {
        .type = AST_LIST,
        .list = {.items = apply_inline_items, .count = 3}};
    assert(infer_toplevel_judgment(ctx, &apply_inline, &typed));
    assert(apply_inline.list.items[1] == &inline_lambda);
    assert(typed.recursively_fused && typed.effects_complete);
    higher_arena = qtt_effect_arena_new();
    higher_solver = qtt_effect_solver_new(higher_arena);
    higher_row = qtt_effect_instantiate(higher_arena, typed.effects);
    assert(higher_row && qtt_effect_is_closed(higher_solver, higher_row));
    assert(qtt_effect_label_count(higher_solver, higher_row, "io") == 1);
    qtt_effect_solver_free(higher_solver);
    qtt_effect_arena_free(higher_arena);
    infer_expression_judgment_free(&typed);

    AST inline_open_head = symbol("openNetwork");
    AST *inline_open_items[] = {&inline_open_head, &inline_value};
    AST inline_open_call = {
        .type = AST_LIST,
        .list = {.items = inline_open_items, .count = 2}};
    AST *inline_open_body[] = {&inline_open_call};
    AST inline_open_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.params = &inline_param, .param_count = 1,
                   .body_exprs = inline_open_body, .body_count = 1,
                   .body = &inline_open_call}};
    AST *apply_inline_open_items[] = {
        &apply_inline_head, &inline_open_lambda, &argument};
    AST apply_inline_open = {
        .type = AST_LIST,
        .list = {.items = apply_inline_open_items, .count = 3}};
    assert(infer_toplevel_judgment(ctx, &apply_inline_open, &typed));
    assert(apply_inline_open.list.items[1] == &inline_open_lambda);
    assert(typed.recursively_fused && !typed.effects_complete);
    higher_arena = qtt_effect_arena_new();
    higher_solver = qtt_effect_solver_new(higher_arena);
    higher_row = qtt_effect_instantiate(higher_arena, typed.effects);
    assert(higher_row && !qtt_effect_is_closed(higher_solver, higher_row));
    assert(qtt_effect_label_count(higher_solver, higher_row, "network") == 1);
    qtt_effect_solver_free(higher_solver);
    qtt_effect_arena_free(higher_arena);
    infer_expression_judgment_free(&typed);

    ASTParam partial_param = {.name = "partialValue"};
    AST partial_head = symbol("curriedIo");
    AST partial_value = symbol("partialValue");
    AST *partial_call_items[] = {&partial_head, &partial_value};
    AST residual_call = {
        .type = AST_LIST,
        .list = {.items = partial_call_items, .count = 2}};
    AST *residual_body[] = {&residual_call};
    AST residual_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.params = &partial_param, .param_count = 1,
                   .body_exprs = residual_body, .body_count = 1,
                   .body = &residual_call}};
    assert(infer_toplevel_judgment(ctx, &residual_lambda, &typed));
    assert(typed.recursively_fused && typed.effects_complete);
    assert(typed.type && typed.type->kind == TYPE_ARROW &&
           typed.type->arrow_ret->kind == TYPE_ARROW);
    assert(typed.arrow_effect_count == 2);
    higher_arena = qtt_effect_arena_new();
    higher_solver = qtt_effect_solver_new(higher_arena);
    QttEffectRow *first_stage = qtt_effect_instantiate(
        higher_arena, typed.arrow_effect_schemes[0]);
    QttEffectRow *returned_stage = qtt_effect_instantiate(
        higher_arena, typed.arrow_effect_schemes[1]);
    assert(first_stage && returned_stage);
    assert(qtt_effect_label_count(higher_solver, first_stage, "io") == 0);
    assert(qtt_effect_label_count(higher_solver, returned_stage, "io") == 1);
    assert(typed.arrow_effects_complete[0]);
    assert(typed.arrow_effects_complete[1]);
    qtt_effect_solver_free(higher_solver);
    qtt_effect_arena_free(higher_arena);
    infer_expression_judgment_free(&typed);

    assert(infer_toplevel_judgment(ctx, &higher_lambda, &typed));
    TypeScheme *higher_curried_scheme = scheme_mono(typed.type);
    scheme_set_effect_scheme(higher_curried_scheme, typed.effects, false);
    QttEffectScheme *higher_curried_stages[] = {pure_effects, typed.effects};
    assert(scheme_set_arrow_effect_schemes(
        higher_curried_scheme, higher_curried_stages,
        higher_complete, 2));
    infer_env_insert(
        environment, "applyHigherCurried", higher_curried_scheme);
    infer_expression_judgment_free(&typed);

    AST apply_higher_curried_head = symbol("applyHigherCurried");
    AST curried_value = symbol("curriedIo");
    AST *apply_higher_curried_items[] = {
        &apply_higher_curried_head, &curried_value, &argument};
    AST apply_higher_curried = {
        .type = AST_LIST,
        .list = {.items = apply_higher_curried_items, .count = 3}};
    assert(infer_toplevel_judgment(ctx, &apply_higher_curried, &typed));
    assert(typed.recursively_fused && typed.effects_complete);
    assert(typed.type && typed.type->kind == TYPE_ARROW);
    higher_arena = qtt_effect_arena_new();
    higher_solver = qtt_effect_solver_new(higher_arena);
    higher_row = qtt_effect_instantiate(higher_arena, typed.effects);
    assert(higher_row && qtt_effect_is_closed(higher_solver, higher_row));
    assert(qtt_effect_label_count(higher_solver, higher_row, "io") == 0);
    qtt_effect_solver_free(higher_solver);
    qtt_effect_arena_free(higher_arena);
    infer_expression_judgment_free(&typed);

    ASTParam both_params[] = {
        {.name = "leftFn"}, {.name = "rightFn"}, {.name = "bothValue"}};
    AST left_head = symbol("leftFn");
    AST right_head = symbol("rightFn");
    AST both_value = symbol("bothValue");
    AST *left_call_items[] = {&left_head, &both_value};
    AST left_call = {
        .type = AST_LIST,
        .list = {.items = left_call_items, .count = 2}};
    AST *right_call_items[] = {&right_head, &both_value};
    AST right_call = {
        .type = AST_LIST,
        .list = {.items = right_call_items, .count = 2}};
    AST *both_body[] = {&left_call, &right_call};
    AST both_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.params = both_params, .param_count = 3,
                   .body_exprs = both_body, .body_count = 2,
                   .body = &right_call}};
    assert(infer_toplevel_judgment(ctx, &both_lambda, &typed));
    assert(typed.recursively_fused && !typed.effects_complete);
    assert(qtt_effect_scheme_callable_parameter_count(typed.effects) == 2);
    assert(qtt_effect_scheme_callable_parameter_index(typed.effects, 0) == 0);
    assert(qtt_effect_scheme_callable_parameter_index(typed.effects, 1) == 1);
    assert(qtt_effect_scheme_callable_parameter_arity(typed.effects, 0) == 1);
    assert(qtt_effect_scheme_callable_parameter_arity(typed.effects, 1) == 1);
    TypeScheme *both_scheme = scheme_mono(typed.type);
    scheme_set_effect_scheme(both_scheme, typed.effects, false);
    QttEffectScheme *both_stages[] = {
        pure_effects, pure_effects, typed.effects};
    bool both_complete[] = {true, true, false};
    assert(scheme_set_arrow_effect_schemes(
        both_scheme, both_stages, both_complete, 3));
    infer_env_insert(environment, "applyBoth", both_scheme);
    infer_expression_judgment_free(&typed);

    AST apply_both_head = symbol("applyBoth");
    AST network_value = symbol("networkFn");
    AST *apply_both_partial_items[] = {&apply_both_head, &io_value};
    AST apply_both_partial = {
        .type = AST_LIST,
        .list = {.items = apply_both_partial_items, .count = 2}};
    assert(infer_toplevel_judgment(ctx, &apply_both_partial, &typed));
    assert(typed.recursively_fused && typed.effects_complete);
    assert(typed.arrow_effect_count == 2);
    assert(qtt_effect_scheme_callable_parameter_count(
               typed.arrow_effect_schemes[1]) == 1);
    assert(qtt_effect_scheme_callable_parameter_index(
               typed.arrow_effect_schemes[1], 0) == 0);
    higher_arena = qtt_effect_arena_new();
    higher_solver = qtt_effect_solver_new(higher_arena);
    higher_row = qtt_effect_instantiate(
        higher_arena, typed.arrow_effect_schemes[1]);
    assert(higher_row && !qtt_effect_is_closed(higher_solver, higher_row));
    assert(qtt_effect_label_count(higher_solver, higher_row, "io") == 1);
    qtt_effect_solver_free(higher_solver);
    qtt_effect_arena_free(higher_arena);
    TypeScheme *partial_both_scheme = scheme_mono(typed.type);
    scheme_set_effect_scheme(
        partial_both_scheme, typed.arrow_effect_schemes[1], false);
    assert(scheme_set_arrow_effect_schemes(
        partial_both_scheme, typed.arrow_effect_schemes,
        typed.arrow_effects_complete, typed.arrow_effect_count));
    infer_env_insert(environment, "applyBothIo", partial_both_scheme);
    infer_expression_judgment_free(&typed);

    AST partial_both_head = symbol("applyBothIo");
    AST *finish_both_items[] = {
        &partial_both_head, &network_value, &argument};
    AST finish_both = {
        .type = AST_LIST,
        .list = {.items = finish_both_items, .count = 3}};
    assert(infer_toplevel_judgment(ctx, &finish_both, &typed));
    assert(typed.recursively_fused && typed.effects_complete);
    higher_arena = qtt_effect_arena_new();
    higher_solver = qtt_effect_solver_new(higher_arena);
    higher_row = qtt_effect_instantiate(higher_arena, typed.effects);
    assert(higher_row && qtt_effect_is_closed(higher_solver, higher_row));
    assert(qtt_effect_label_count(higher_solver, higher_row, "io") == 1);
    assert(qtt_effect_label_count(higher_solver, higher_row, "network") == 1);
    qtt_effect_solver_free(higher_solver);
    qtt_effect_arena_free(higher_arena);
    infer_expression_judgment_free(&typed);

    AST *apply_both_items[] = {
        &apply_both_head, &io_value, &network_value, &argument};
    AST apply_both = {
        .type = AST_LIST,
        .list = {.items = apply_both_items, .count = 4}};
    assert(infer_toplevel_judgment(ctx, &apply_both, &typed));
    assert(typed.recursively_fused && typed.effects_complete);
    higher_arena = qtt_effect_arena_new();
    higher_solver = qtt_effect_solver_new(higher_arena);
    higher_row = qtt_effect_instantiate(higher_arena, typed.effects);
    assert(higher_row && qtt_effect_is_closed(higher_solver, higher_row));
    assert(qtt_effect_label_count(higher_solver, higher_row, "io") == 1);
    assert(qtt_effect_label_count(higher_solver, higher_row, "network") == 1);
    qtt_effect_solver_free(higher_solver);
    qtt_effect_arena_free(higher_arena);
    infer_expression_judgment_free(&typed);

    AST open_value = symbol("openNetwork");
    AST *apply_both_open_items[] = {
        &apply_both_head, &io_value, &open_value, &argument};
    AST apply_both_open = {
        .type = AST_LIST,
        .list = {.items = apply_both_open_items, .count = 4}};
    assert(infer_toplevel_judgment(ctx, &apply_both_open, &typed));
    assert(typed.recursively_fused && !typed.effects_complete);
    higher_arena = qtt_effect_arena_new();
    higher_solver = qtt_effect_solver_new(higher_arena);
    higher_row = qtt_effect_instantiate(higher_arena, typed.effects);
    assert(higher_row && !qtt_effect_is_closed(higher_solver, higher_row));
    assert(qtt_effect_label_count(higher_solver, higher_row, "io") == 1);
    assert(qtt_effect_label_count(higher_solver, higher_row, "network") == 1);
    qtt_effect_solver_free(higher_solver);
    qtt_effect_arena_free(higher_arena);
    infer_expression_judgment_free(&typed);

    AST pure_value = symbol("pureFn");
    AST *apply_pure_items[] = {
        &apply_higher_head, &pure_value, &argument};
    AST apply_pure = {
        .type = AST_LIST,
        .list = {.items = apply_pure_items, .count = 3}};
    assert(infer_toplevel_judgment(ctx, &apply_pure, &typed));
    assert(typed.recursively_fused && typed.effects_complete);
    higher_arena = qtt_effect_arena_new();
    higher_solver = qtt_effect_solver_new(higher_arena);
    higher_row = qtt_effect_instantiate(higher_arena, typed.effects);
    assert(higher_row && qtt_effect_is_closed(higher_solver, higher_row));
    assert(qtt_effect_label_count(higher_solver, higher_row, "io") == 0);
    qtt_effect_solver_free(higher_solver);
    qtt_effect_arena_free(higher_arena);
    infer_expression_judgment_free(&typed);

    AST *apply_open_items[] = {
        &apply_higher_head, &open_value, &argument};
    AST apply_open = {
        .type = AST_LIST,
        .list = {.items = apply_open_items, .count = 3}};
    assert(infer_toplevel_judgment(ctx, &apply_open, &typed));
    assert(typed.recursively_fused && !typed.effects_complete);
    higher_arena = qtt_effect_arena_new();
    higher_solver = qtt_effect_solver_new(higher_arena);
    higher_row = qtt_effect_instantiate(higher_arena, typed.effects);
    assert(higher_row && !qtt_effect_is_closed(higher_solver, higher_row));
    assert(qtt_effect_label_count(higher_solver, higher_row, "network") == 1);
    qtt_effect_solver_free(higher_solver);
    qtt_effect_arena_free(higher_arena);
    infer_expression_judgment_free(&typed);

    AST typed_heap = {
        .type = AST_ARRAY,
        .array = {.elements = (AST *[]){&argument},
                  .element_count = 1, .is_heap = true}};
    assert(infer_toplevel_judgment(ctx, &typed_heap, &typed));
    assert(typed.type && typed.effects && typed.effects_complete);
    assert(typed.recursively_fused);
    QttEffectArena *typed_arena = qtt_effect_arena_new();
    QttEffectSolver *typed_solver = qtt_effect_solver_new(typed_arena);
    QttEffectRow *typed_row = qtt_effect_instantiate(
        typed_arena, typed.effects);
    const QttEffectDeclaration *allocation_declaration =
        qtt_effect_declaration_lookup("Core.Alloc.heap");
    QttEffectAtom allocation_effect = {
        .traits = "allocate",
        .kind = QTT_EFFECT_ALLOCATE,
        .constructor_id = allocation_declaration->constructor_id,
        .name = "Core.Alloc.heap", .operation = "heap",
        .resumption = {.finite = 1},
    };
    assert(typed_row && qtt_effect_atom_count(
        typed_solver, typed_row, &allocation_effect) == 1);
    qtt_effect_solver_free(typed_solver);
    qtt_effect_arena_free(typed_arena);
    infer_expression_judgment_free(&typed);

    assert(infer_toplevel_judgment(ctx, &call, &typed));
    assert(typed.recursively_fused && typed.type->kind == TYPE_INT);
    typed_arena = qtt_effect_arena_new();
    typed_solver = qtt_effect_solver_new(typed_arena);
    typed_row = qtt_effect_instantiate(typed_arena, typed.effects);
    assert(typed_row && qtt_effect_label_count(
        typed_solver, typed_row, "io") == 1);
    qtt_effect_solver_free(typed_solver);
    qtt_effect_arena_free(typed_arena);
    infer_expression_judgment_free(&typed);

    AST begin_head = symbol("begin");
    AST *begin_items[] = {&begin_head, &call, &argument};
    AST begin_expr = {
        .type = AST_LIST,
        .list = {.items = begin_items, .count = 3}};
    assert(infer_toplevel_judgment(ctx, &begin_expr, &typed));
    assert(typed.recursively_fused && typed.type->kind == TYPE_INT);
    typed_arena = qtt_effect_arena_new();
    typed_solver = qtt_effect_solver_new(typed_arena);
    typed_row = qtt_effect_instantiate(typed_arena, typed.effects);
    assert(typed_row && qtt_effect_label_count(
        typed_solver, typed_row, "io") == 1);
    qtt_effect_solver_free(typed_solver);
    qtt_effect_arena_free(typed_arena);
    infer_expression_judgment_free(&typed);

    AST if_head = symbol("if");
    AST truth = symbol("truth");
    AST *if_items[] = {&if_head, &truth, &call, &argument};
    AST if_expr = {
        .type = AST_LIST,
        .list = {.items = if_items, .count = 4}};
    assert(infer_toplevel_judgment(ctx, &if_expr, &typed));
    assert(typed.recursively_fused && typed.type->kind == TYPE_INT);
    typed_arena = qtt_effect_arena_new();
    typed_solver = qtt_effect_solver_new(typed_arena);
    typed_row = qtt_effect_instantiate(typed_arena, typed.effects);
    assert(typed_row && qtt_effect_label_count(
        typed_solver, typed_row, "io") == 1);
    qtt_effect_solver_free(typed_solver);
    qtt_effect_arena_free(typed_arena);
    infer_expression_judgment_free(&typed);

    AST radix = {
        .type = AST_NUMBER, .number = 57005,
        .literal_str = "0xdead"};
    assert(infer_toplevel_judgment(ctx, &radix, &typed));
    assert(typed.recursively_fused && typed.type->kind == TYPE_INT);
    infer_expression_judgment_free(&typed);

    AST map_key = {.type = AST_STRING};
    AST fused_map = {
        .type = AST_MAP,
        .map = {.keys = (AST *[]){&map_key},
                .vals = (AST *[]){&typed_heap}, .count = 1}};
    assert(infer_toplevel_judgment(ctx, &fused_map, &typed));
    assert(typed.recursively_fused && typed.type->kind == TYPE_MAP);
    typed_arena = qtt_effect_arena_new();
    typed_solver = qtt_effect_solver_new(typed_arena);
    typed_row = qtt_effect_instantiate(typed_arena, typed.effects);
    assert(typed_row && qtt_effect_atom_count(
        typed_solver, typed_row, &allocation_effect) == 1);
    qtt_effect_solver_free(typed_solver);
    qtt_effect_arena_free(typed_arena);
    infer_expression_judgment_free(&typed);

    bool complete = false;
    QttEffectScheme *inferred = infer_effect_scheme_for_lambda(
        ctx, &lambda, &complete);
    assert(inferred && complete);
    assert(infer_effect_constraint_count(ctx) >= 2);
    assert(infer_effect_certificate_fingerprint(ctx) != 0);
    assert(!infer_effect_constraints_residual(ctx));
    assert(qtt_effect_scheme_evidence_count(inferred) >= 2);
    assert(qtt_effect_scheme_evidence_fingerprint(inferred) ==
           infer_effect_certificate_fingerprint(ctx));
    assert(qtt_effect_scheme_evidence_status(inferred) ==
           QTT_EFFECT_EVIDENCE_SOLVED);
    assert(qtt_effect_scheme_evidence_intact(inferred));
    const char *portable = qtt_effect_scheme_evidence(inferred);
    assert(portable && strstr(portable, "monad-effect-certificate-v1") == portable);
    QttEffectScheme *retained_inferred =
        qtt_effect_scheme_retain(inferred);
    qtt_effect_scheme_free(inferred);
    inferred = retained_inferred;
    assert(qtt_effect_scheme_evidence_fingerprint(inferred) != 0);
    QttEffectArena *instance_arena = qtt_effect_arena_new();
    QttEffectSolver *instance_solver =
        qtt_effect_solver_new(instance_arena);
    QttEffectRow *instance = qtt_effect_instantiate(
        instance_arena, inferred);
    assert(instance && qtt_effect_is_closed(instance_solver, instance));
    assert(qtt_effect_label_count(instance_solver, instance, "io") == 1);
    const QttEffectDeclaration *write_declaration =
        qtt_effect_declaration_lookup("Core.State.write");
    QttEffectAtom write_effect = {
        .traits = "state",
        .kind = QTT_EFFECT_STATE,
        .constructor_id = write_declaration->constructor_id,
        .name = "Core.State.write", .operation = "write",
        .resumption = {.finite = 1},
    };
    assert(qtt_effect_atom_count(
               instance_solver, instance, &write_effect) == 1);

    AST unknown_head = symbol("dynamicForeign");
    AST *unknown_items[] = {&unknown_head, &argument};
    AST unknown_call = {
        .type = AST_LIST,
        .list = {.items = unknown_items, .count = 2}};
    AST *unknown_body[] = {&unknown_call};
    AST unknown_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.body_exprs = unknown_body, .body_count = 1,
                   .body = &unknown_call}};
    QttEffectScheme *unknown = infer_effect_scheme_for_lambda(
        ctx, &unknown_lambda, &complete);
    assert(unknown && !complete);
    assert(qtt_effect_scheme_coverage_gaps(unknown) &
           QTT_EFFECT_COVERAGE_UNKNOWN_CALLEE);
    char *unknown_gaps = qtt_effect_coverage_format(
        qtt_effect_scheme_coverage_gaps(unknown));
    assert(unknown_gaps && strstr(unknown_gaps, "unknown-callee"));
    free(unknown_gaps);
    assert(qtt_effect_scheme_evidence_status(unknown) ==
           QTT_EFFECT_EVIDENCE_SOLVED);

    AST curried_head = symbol("curriedIo");
    AST *partial_items[] = {&curried_head, &argument};
    AST partial_call = {
        .type = AST_LIST,
        .list = {.items = partial_items, .count = 2}};
    AST *partial_body[] = {&partial_call};
    AST partial_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.body_exprs = partial_body, .body_count = 1,
                   .body = &partial_call}};
    QttEffectScheme *partial = infer_effect_scheme_for_lambda(
        ctx, &partial_lambda, &complete);
    assert(partial && complete);
    QttEffectRow *partial_instance = qtt_effect_instantiate(
        instance_arena, partial);
    assert(partial_instance && qtt_effect_label_count(
        instance_solver, partial_instance, "io") == 0);
    assert(infer_toplevel_judgment(ctx, &partial_call, &typed));
    assert(typed.recursively_fused && typed.type->kind == TYPE_ARROW);
    QttEffectRow *typed_partial = qtt_effect_instantiate(
        instance_arena, typed.effects);
    assert(typed_partial && qtt_effect_label_count(
        instance_solver, typed_partial, "io") == 0);
    assert(typed.arrow_effect_count == 1);
    InferCallableContract transported = {0};
    assert(infer_callable_contract_from_judgment(&transported, &typed));
    assert(transported.arrow_effect_count == 1);
    infer_expression_judgment_free(&typed);
    QttEffectRow *typed_returned = qtt_effect_instantiate(
        instance_arena, transported.arrow_effect_schemes[0]);
    assert(typed_returned && qtt_effect_label_count(
        instance_solver, typed_returned, "io") == 1);
    assert(transported.arrow_effects_complete[0]);
    TypeScheme *transported_scheme = scheme_mono(type_int());
    assert(scheme_set_callable_contract(transported_scheme, &transported));
    assert(scheme_effect_fingerprint(transported_scheme) ==
           infer_callable_contract_fingerprint(&transported));
    InferCallableContract roundtrip = {0};
    assert(infer_callable_contract_from_scheme(
        &roundtrip, transported_scheme));
    assert(infer_callable_contract_fingerprint(&roundtrip) ==
           infer_callable_contract_fingerprint(&transported));
    infer_callable_contract_free(&roundtrip);
    char *portable_contract = infer_callable_contract_serialize(&transported);
    assert(portable_contract && strstr(
        portable_contract, "monad-callable-contract-v1|") ==
        portable_contract);
    InferCallableContract decoded_contract = {0};
    assert(infer_callable_contract_deserialize(
        &decoded_contract, portable_contract));
    assert(infer_callable_contract_fingerprint(&decoded_contract) ==
           infer_callable_contract_fingerprint(&transported));
    infer_callable_contract_free(&decoded_contract);
    portable_contract[strlen(portable_contract) - 1] =
        portable_contract[strlen(portable_contract) - 1] == 'a' ? 'b' : 'a';
    assert(!infer_callable_contract_deserialize(
        &decoded_contract, portable_contract));
    free(portable_contract);
    uint64_t complete_contract_fingerprint =
        scheme_effect_fingerprint(transported_scheme);
    scheme_set_effect_scheme(
        transported_scheme, transported.effect_scheme, false);
    assert(scheme_effect_fingerprint(transported_scheme) !=
           complete_contract_fingerprint);
    scheme_free(transported_scheme);
    infer_callable_contract_free(&transported);

    AST second_argument = {.type = AST_NUMBER, .number = 3};
    AST *full_items[] = {&curried_head, &argument, &second_argument};
    AST full_call = {
        .type = AST_LIST,
        .list = {.items = full_items, .count = 3}};
    AST *full_body[] = {&full_call};
    AST full_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.body_exprs = full_body, .body_count = 1,
                   .body = &full_call}};
    QttEffectScheme *full = infer_effect_scheme_for_lambda(
        ctx, &full_lambda, &complete);
    assert(full && complete);
    QttEffectRow *full_instance = qtt_effect_instantiate(
        instance_arena, full);
    assert(full_instance && qtt_effect_label_count(
        instance_solver, full_instance, "io") == 1);
    assert(infer_toplevel_judgment(ctx, &full_call, &typed));
    assert(typed.recursively_fused && typed.type->kind == TYPE_INT);
    QttEffectRow *typed_full = qtt_effect_instantiate(
        instance_arena, typed.effects);
    assert(typed_full && qtt_effect_label_count(
        instance_solver, typed_full, "io") == 1);
    infer_expression_judgment_free(&typed);

    /* One lexical row name denotes one row throughout a curried type.
     * Therefore e = pure at stage zero and e = io at stage one is
     * inconsistent, while distinct names impose no equality. */
    AST shared_row_head = symbol("sharedRow");
    AST *shared_row_items[] = {
        &shared_row_head, &argument, &second_argument};
    AST shared_row_call = {
        .type = AST_LIST,
        .list = {.items = shared_row_items, .count = 3}};
    assert(!infer_effect_scheme_for_expression(
        ctx, &shared_row_call, &complete));
    assert(!infer_toplevel_judgment(ctx, &shared_row_call, &typed));

    AST captured_row_head = symbol("capturedRow");
    AST *captured_partial_items[] = {&captured_row_head, &argument};
    AST captured_partial_call = {
        .type = AST_LIST,
        .list = {.items = captured_partial_items, .count = 2}};
    assert(infer_toplevel_judgment(ctx, &captured_partial_call, &typed));
    assert(typed.arrow_effect_count == 1);
    QttEffectRow *captured_residual = qtt_effect_instantiate(
        instance_arena, typed.arrow_effect_schemes[0]);
    assert(captured_residual);
    assert(qtt_effect_label_count(
        instance_solver, captured_residual, "io") == 1);
    assert(qtt_effect_is_closed(instance_solver, captured_residual));
    InferCallableContract captured_contract = {0};
    assert(infer_callable_contract_from_judgment(
        &captured_contract, &typed));
    TypeScheme *captured_alias_scheme = scheme_mono(type_clone(typed.type));
    captured_alias_scheme->owns_type = true;
    assert(scheme_set_callable_contract(
        captured_alias_scheme, &captured_contract));
    infer_env_insert(environment, "capturedAlias", captured_alias_scheme);
    infer_callable_contract_free(&captured_contract);
    infer_expression_judgment_free(&typed);
    AST captured_alias_head = symbol("capturedAlias");
    AST *captured_alias_items[] = {&captured_alias_head, &second_argument};
    AST captured_alias_call = {
        .type = AST_LIST,
        .list = {.items = captured_alias_items, .count = 2}};
    assert(infer_toplevel_judgment(ctx, &captured_alias_call, &typed));
    QttEffectRow *captured_alias_effect = qtt_effect_instantiate(
        instance_arena, typed.effects);
    assert(captured_alias_effect && qtt_effect_label_count(
        instance_solver, captured_alias_effect, "io") == 1);
    infer_expression_judgment_free(&typed);

    AST distinct_row_head = symbol("distinctRows");
    AST *distinct_row_items[] = {
        &distinct_row_head, &argument, &second_argument};
    AST distinct_row_call = {
        .type = AST_LIST,
        .list = {.items = distinct_row_items, .count = 3}};
    QttEffectScheme *distinct_rows = infer_effect_scheme_for_expression(
        ctx, &distinct_row_call, &complete);
    assert(distinct_rows && complete);
    qtt_effect_scheme_free(distinct_rows);
    assert(infer_toplevel_judgment(ctx, &distinct_row_call, &typed));
    infer_expression_judgment_free(&typed);

    AST concrete_bad_head = symbol("concreteBad");
    AST *concrete_bad_items[] = {&concrete_bad_head, &argument};
    AST concrete_bad_call = {
        .type = AST_LIST,
        .list = {.items = concrete_bad_items, .count = 2}};
    assert(!infer_effect_scheme_for_expression(
        ctx, &concrete_bad_call, &complete));
    assert(!infer_toplevel_judgment(ctx, &concrete_bad_call, &typed));

    AST concrete_good_head = symbol("concreteGood");
    AST *concrete_good_items[] = {&concrete_good_head, &argument};
    AST concrete_good_call = {
        .type = AST_LIST,
        .list = {.items = concrete_good_items, .count = 2}};
    QttEffectScheme *concrete_result = infer_effect_scheme_for_expression(
        ctx, &concrete_good_call, &complete);
    assert(concrete_result && complete);
    QttEffectRow *concrete_instance = qtt_effect_instantiate(
        instance_arena, concrete_result);
    const QttEffectDeclaration *core_read =
        qtt_effect_declaration_lookup("Core.IO.read");
    QttEffectAtom read_atom = {
        .traits = "io", .kind = QTT_EFFECT_IO,
        .constructor_id = core_read->constructor_id,
        .name = "Core.IO.read", .operation = "read",
        .resumption = {.finite = 1},
    };
    assert(qtt_effect_atom_count(
        instance_solver, concrete_instance, &read_atom) == 1);
    qtt_effect_scheme_free(concrete_result);
    assert(infer_toplevel_judgment(ctx, &concrete_good_call, &typed));
    infer_expression_judgment_free(&typed);

    AST open_head = symbol("openNetwork");
    AST *open_items[] = {&open_head, &argument};
    AST open_call = {
        .type = AST_LIST,
        .list = {.items = open_items, .count = 2}};
    AST *open_body[] = {&mutation, &open_call, &open_call};
    AST open_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.body_exprs = open_body, .body_count = 3,
                   .body = &open_call}};
    QttEffectScheme *open_inferred = infer_effect_scheme_for_lambda(
        ctx, &open_lambda, &complete);
    assert(open_inferred && !complete);
    assert(qtt_effect_scheme_coverage_gaps(open_inferred) &
           QTT_EFFECT_COVERAGE_INCOMPLETE_CALLEE);
    assert(qtt_effect_scheme_coverage_gaps(open_inferred) &
           QTT_EFFECT_COVERAGE_OPEN_ROW);
    assert(infer_effect_constraint_count(ctx) >= 3);
    assert(infer_effect_certificate_fingerprint(ctx) != 0);
    assert(infer_effect_constraints_residual(ctx));
    assert(qtt_effect_scheme_evidence_status(open_inferred) ==
           QTT_EFFECT_EVIDENCE_RESIDUAL);
    assert(qtt_effect_scheme_evidence_intact(open_inferred));
    assert(qtt_effect_scheme_evidence_fingerprint(open_inferred) ==
           infer_effect_certificate_fingerprint(ctx));
    QttEffectRow *open_instance = qtt_effect_instantiate(
        instance_arena, open_inferred);
    assert(open_instance && !qtt_effect_is_closed(
        instance_solver, open_instance));
    assert(qtt_effect_label_count(
               instance_solver, open_instance, "network") == 1);

    /* Application consumes qualified predicates on the exact instantiated
     * stage row. Closed absence refutes; an open tail keeps the obligation
     * residual and therefore the inferred judgment conservative. */
    AST qualified_head = symbol("qualifiedNetwork");
    AST *qualified_items[] = {&qualified_head, &argument};
    AST qualified_call = {
        .type = AST_LIST,
        .list = {.items = qualified_items, .count = 2}};
    AST *qualified_body[] = {&qualified_call};
    AST qualified_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.body_exprs = qualified_body, .body_count = 1,
                   .body = &qualified_call}};
    QttEffectScheme *qualified_rejected = infer_effect_scheme_for_lambda(
        ctx, &qualified_lambda, &complete);
    assert(!qualified_rejected);

    AST qualified_curried_head = symbol("qualifiedCurried");
    AST *qualified_partial_items[] = {&qualified_curried_head, &argument};
    AST qualified_partial_call = {
        .type = AST_LIST,
        .list = {.items = qualified_partial_items, .count = 2}};
    assert(infer_toplevel_judgment(ctx, &qualified_partial_call, &typed));
    assert(typed.arrow_effect_count == 1);
    assert(typed.effect_trait_predicate_count == 1);
    assert(typed.effect_trait_predicate_stages[0] == 0);
    assert(strcmp(typed.effect_trait_predicate_names[0], "io") == 0);
    InferCallableContract residual_contract = {0};
    assert(infer_callable_contract_from_judgment(
        &residual_contract, &typed));
    assert(residual_contract.effect_trait_predicate_count == 1);
    assert(residual_contract.effect_trait_predicate_stages[0] == 0);
    TypeScheme *residual_scheme = scheme_mono(type_clone(typed.type));
    residual_scheme->owns_type = true;
    assert(scheme_set_callable_contract(
        residual_scheme, &residual_contract));
    infer_env_insert(environment, "qualifiedResidual", residual_scheme);
    infer_callable_contract_free(&residual_contract);
    infer_expression_judgment_free(&typed);
    AST residual_head = symbol("qualifiedResidual");
    AST *residual_items[] = {&residual_head, &argument};
    AST qualified_residual_call = {
        .type = AST_LIST,
        .list = {.items = residual_items, .count = 2}};
    AST *qualified_residual_body[] = {&qualified_residual_call};
    AST qualified_residual_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.body_exprs = qualified_residual_body, .body_count = 1,
                   .body = &qualified_residual_call}};
    assert(!infer_effect_scheme_for_lambda(
        ctx, &qualified_residual_lambda, &complete));

    AST qualified_open_head = symbol("qualifiedOpen");
    AST *qualified_open_items[] = {&qualified_open_head, &argument};
    AST qualified_open_call = {
        .type = AST_LIST,
        .list = {.items = qualified_open_items, .count = 2}};
    AST *qualified_open_body[] = {&qualified_open_call};
    AST qualified_open_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.body_exprs = qualified_open_body, .body_count = 1,
                   .body = &qualified_open_call}};
    QttEffectScheme *qualified_residual = infer_effect_scheme_for_lambda(
        ctx, &qualified_open_lambda, &complete);
    assert(qualified_residual && !complete);
    assert(infer_effect_constraint_count(ctx) >= 2);
    assert(infer_effect_constraints_residual(ctx));
    qtt_effect_scheme_free(qualified_residual);

    /* A core-owned implication axiom turns the same application from a
     * refutation into a proved qualification, with no compiler vocabulary
     * change. */
    assert(qtt_effect_trait_implication_register(
        "network", "io"));
    QttEffectScheme *qualified_proved = infer_effect_scheme_for_lambda(
        ctx, &qualified_lambda, &complete);
    assert(qualified_proved && complete);
    assert(infer_effect_constraint_count(ctx) >= 2);
    assert(!infer_effect_constraints_residual(ctx));
    qtt_effect_scheme_free(qualified_proved);
    assert(qtt_effect_atom_count(
               instance_solver, open_instance, &write_effect) == 1);
    assert(infer_toplevel_judgment(ctx, &open_call, &typed));
    assert(typed.recursively_fused && !typed.effects_complete);
    assert(qtt_effect_scheme_coverage_gaps(typed.effects) &
           QTT_EFFECT_COVERAGE_OPEN_ROW);
    infer_expression_judgment_free(&typed);

    AST *nested_body[] = {&lambda};
    AST nested = {
        .type = AST_LAMBDA,
        .lambda = {.body_exprs = nested_body, .body_count = 1,
                   .body = &lambda}};
    QttEffectScheme *nested_effects = infer_effect_scheme_for_lambda(
        ctx, &nested, &complete);
    assert(nested_effects && complete);
    assert(qtt_effect_scheme_coverage_gaps(nested_effects) == 0);
    QttEffectRow *nested_instance = qtt_effect_instantiate(
        instance_arena, nested_effects);
    assert(nested_instance &&
           qtt_effect_label_count(
               instance_solver, nested_instance, "io") == 0 &&
           qtt_effect_atom_count(
               instance_solver, nested_instance, &write_effect) == 0);

    AST heap_array = {
        .type = AST_ARRAY,
        .array = {.elements = (AST *[]){&argument},
                  .element_count = 1, .is_heap = true}};
    AST assembly = {.type = AST_ASM};
    AST *intrinsic_body[] = {&heap_array, &assembly};
    AST intrinsic_lambda = {
        .type = AST_LAMBDA,
        .lambda = {.body_exprs = intrinsic_body, .body_count = 2,
                   .body = &assembly}};
    QttEffectScheme *intrinsics = infer_effect_scheme_for_lambda(
        ctx, &intrinsic_lambda, &complete);
    assert(intrinsics && complete);
    QttEffectRow *intrinsic_instance = qtt_effect_instantiate(
        instance_arena, intrinsics);
    assert(intrinsic_instance &&
           qtt_effect_atom_count(
               instance_solver, intrinsic_instance, &allocation_effect) == 1);
    const QttEffectDeclaration *asm_declaration =
        qtt_effect_declaration_lookup("Core.Foreign.asm");
    QttEffectAtom asm_effect = {
        .traits = "foreign",
        .kind = QTT_EFFECT_FOREIGN,
        .constructor_id = asm_declaration->constructor_id,
        .name = "Core.Foreign.asm",
        .operation = "asm",
        .resumption = {.finite = 1},
    };
    assert(qtt_effect_atom_count(
        instance_solver, intrinsic_instance, &asm_effect) == 1);

    Type a7 = {.kind = TYPE_VAR, .var_id = 7};
    Type id7 = {.kind = TYPE_ARROW, .arrow_param = &a7, .arrow_ret = &a7};
    int quantified7[] = {7};
    TypeScheme alpha7 = {
        .quantified = quantified7, .quantified_count = 1, .type = &id7,
    };
    Type a99 = {.kind = TYPE_VAR, .var_id = 99};
    Type id99 = {.kind = TYPE_ARROW, .arrow_param = &a99, .arrow_ret = &a99};
    int quantified99[] = {99};
    TypeScheme alpha99 = {
        .quantified = quantified99, .quantified_count = 1, .type = &id99,
    };
    char *portable_alpha7 = infer_type_scheme_serialize(&alpha7);
    char *portable_alpha99 = infer_type_scheme_serialize(&alpha99);
    assert(portable_alpha7 && portable_alpha99);
    assert(strcmp(portable_alpha7, portable_alpha99) == 0);
    Type b100 = {.kind = TYPE_VAR, .var_id = 100};
    Type distinct = {
        .kind = TYPE_ARROW, .arrow_param = &a99, .arrow_ret = &b100,
    };
    int quantified_distinct[] = {99, 100};
    TypeScheme alpha_distinct = {
        .quantified = quantified_distinct,
        .quantified_count = 2,
        .type = &distinct,
    };
    char *portable_distinct = infer_type_scheme_serialize(&alpha_distinct);
    assert(portable_distinct && strcmp(portable_alpha7, portable_distinct));
    TypeScheme *restored_alpha =
        infer_type_scheme_deserialize(portable_alpha7);
    assert(restored_alpha && restored_alpha->quantified_count == 1);
    assert(restored_alpha->quantified[0] == 0);
    assert(restored_alpha->type->kind == TYPE_ARROW);
    assert(restored_alpha->type->arrow_param->var_id == 0);
    assert(restored_alpha->type->arrow_ret->var_id == 0);
    assert(!infer_type_scheme_deserialize(
        "monad-hm-scheme-v1|00000000000000000|1|bad"));
    assert(!infer_type_scheme_deserialize(
        "monad-hm-scheme-v1|ffffffffffffffff|999999999999999999999|bad"));
    portable_alpha7[strlen(portable_alpha7) - 1] = 'x';
    assert(!infer_type_scheme_deserialize(portable_alpha7));
    scheme_free(restored_alpha);
    free(portable_distinct);
    free(portable_alpha99);
    free(portable_alpha7);

    qtt_effect_scheme_free(intrinsics);
    qtt_effect_scheme_free(open_inferred);
    qtt_effect_scheme_free(nested_effects);
    qtt_effect_scheme_free(full);
    qtt_effect_scheme_free(partial);
    qtt_effect_scheme_free(unknown);
    qtt_effect_scheme_free(inferred);
    qtt_effect_solver_free(instance_solver);
    qtt_effect_arena_free(instance_arena);
    infer_ctx_free(ctx);
    infer_env_free(environment);
    qtt_effect_scheme_free(io_effects);
    qtt_effect_scheme_free(open_io_effects);
    qtt_effect_scheme_free(qualified_network_effects);
    qtt_effect_scheme_free(qualified_open_effects);
    qtt_effect_scheme_free(pure_effects);
    qtt_effect_scheme_free(open_empty_effects);
    qtt_effect_scheme_free(declared_read_effects);
    qtt_effect_scheme_free(dependent_open);
    qtt_effect_solver_free(source_solver);
    qtt_effect_arena_free(source_arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "source-effects"
            harness = Path(directory) / "source-effects.c"
            harness.write_text(source)
            subprocess.run([
                "cc", "-std=c99", "-D_GNU_SOURCE", "-Wall", "-Wextra",
                "-Werror", "-Wno-switch", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-ffunction-sections",
                "-fdata-sections", "-iquote", str(ROOT / "src"), str(harness),
                str(ROOT / "src" / "infer.c"), str(ROOT / "src" / "types.c"),
                str(ROOT / "src" / "qtt" / "quantity.c"),
                str(ROOT / "src" / "qtt" / "constraints.c"),
                str(ROOT / "src" / "qtt" / "environment.c"),
                str(ROOT / "src" / "effects" / "effect.c"),
                str(ROOT / "src" / "effects" / "constraints.c"),
                str(ROOT / "src" / "qtt" / "place.c"),
                str(ROOT / "src" / "qtt" / "type_identity.c"),
                "-Wl,--gc-sections", "-o", str(executable),
            ], cwd=ROOT, check=True)
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
