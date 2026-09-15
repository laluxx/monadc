"""Module checking collects all contracts before checking bodies."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class QttModuleTests(unittest.TestCase):
    def test_forward_reference_is_order_independent(self):
        source = r'''
#include "qtt/module.h"
#include <assert.h>
#include <stdio.h>

static QttCoreNode function(
    Type *string_type, QttCoreVar *parameter, const char *callee_name,
    QttCoreNode *var, QttCoreNode *global, QttCoreNode **arguments,
    QttCoreNode *apply, const Type **parameter_types) {
    *var = (QttCoreNode){
        .kind = QTT_CORE_VAR, .type = string_type, .var = *parameter};
    *global = (QttCoreNode){
        .kind = QTT_CORE_GLOBAL, .type = string_type,
        .global = {.name = callee_name}};
    arguments[0] = var;
    *apply = (QttCoreNode){
        .kind = QTT_CORE_APPLY, .type = string_type,
        .apply = {.callee = global, .arguments = arguments,
                  .argument_count = 1}};
    parameter_types[0] = string_type;
    return (QttCoreNode){
        .kind = QTT_CORE_LAMBDA, .type = string_type,
        .lambda = {.params = parameter, .param_types = parameter_types,
                   .param_count = 1, .body = apply}};
}

int main(void) {
    Type string_type = {.kind = TYPE_STRING};
    Type bool_type = {.kind = TYPE_BOOL};
    QttCoreVar ap = {41, 1}, bp = {41, 2};
    QttCoreNode av, ag, aa, bv;
    QttCoreNode *aargs[1];
    const Type *atypes[1], *btypes[1];
    QttCoreNode a = function(
        &string_type, &ap, "Main.b",
        &av, &ag, aargs, &aa, atypes);
    QttCoreNode ag2 = {
        .kind = QTT_CORE_GLOBAL, .type = &string_type,
        .global = {.name = "Main.b"}};
    QttCoreNode *aargs2[] = {&av};
    QttCoreNode aa2 = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &ag2, .arguments = aargs2,
                  .argument_count = 1}};
    QttCoreNode condition = {
        .kind = QTT_CORE_GLOBAL, .type = &bool_type,
        .global = {.name = "True"}};
    QttCoreNode a_branch = {
        .kind = QTT_CORE_IF, .type = &string_type,
        .conditional = {.condition = &condition,
                        .then_branch = &aa,
                        .else_branch = &aa2}};
    a.lambda.body = &a_branch;
    bv = (QttCoreNode){
        .kind = QTT_CORE_VAR, .type = &string_type, .var = bp};
    btypes[0] = &string_type;
    QttCoreNode b = {
        .kind = QTT_CORE_LAMBDA, .type = &string_type,
        .lambda = {.params = &bp, .param_types = btypes,
                   .param_count = 1, .body = &bv}};

    QttModule *module = qtt_module_new(41);
    assert(module);
    assert(qtt_module_declare(module, "Main.a", &a) == QTT_MODULE_OK);
    assert(qtt_module_declare(module, "Main.b", &b) == QTT_MODULE_OK);
    QttModuleVerification verification = qtt_module_verify(module);
    if (verification.error != QTT_MODULE_OK)
        fprintf(stderr, "module=%d lower=%d anf=%d semantic=%d/%d definition=%zu\n",
                verification.error, verification.lower_error,
                verification.anf_error, verification.semantic_error,
                verification.semantic_validation, verification.definition);
    assert(verification.error == QTT_MODULE_OK);
    assert(verification.verified_count == 2);
    assert(verification.transfers.complete);
    QttSemanticIrError semantic_error = QTT_SEMANTIC_IR_OK;
    QttSemanticFunction *semantic = qtt_semantic_ir_lower_in_env(
        &a, qtt_module_signatures(module), 41, &semantic_error);
    assert(semantic && semantic_error == QTT_SEMANTIC_IR_OK);
    assert(qtt_semantic_ir_call_count(semantic) == 2);
    QttSemanticCallEvidence *calls =
        qtt_semantic_ir_calls(semantic);
    assert(calls[0].call_ordinal == 0);
    assert(calls[1].call_ordinal == 1);
    assert(calls[0].control_path_count == 1);
    assert(!calls[0].control_path[0].else_arm);
    assert(calls[1].control_path_count == 1);
    assert(calls[1].control_path[0].else_arm);
    assert(calls[0].argument_count == 1);
    assert(calls[0].argument_transfers[0] == QTT_CALL_MOVE);
    assert(qtt_core_var_equal(calls[0].source_vars[0], ap));
    assert(calls[0].contract_fingerprint != 0);
    assert(qtt_semantic_ir_verify(semantic, &a) ==
           QTT_SEMANTIC_IR_VALID);
    QttFunctionSignature *a_signature = qtt_signature_env_lookup(
        qtt_module_signatures(module), 41, "Main.a");
    QttAnfLowerError lower_error = QTT_ANF_LOWER_OK;
    QttAnfProgram *program = qtt_anf_lower_function_in_env(
        &a, a_signature, qtt_module_signatures(module), 41,
        &lower_error);
    assert(program && lower_error == QTT_ANF_LOWER_OK);
    assert(qtt_semantic_anf_verify_calls(
               semantic, &a, program) == QTT_SEMANTIC_ANF_OK);
    QttAnfInstruction *call_instruction = NULL;
    for (size_t i = 0; i < program->block_count; i++)
        for (size_t j = 0;
             j < program->blocks[i].instruction_count; j++)
            if (program->blocks[i].instructions[j].kind ==
                QTT_ANF_CALL)
                call_instruction =
                    &program->blocks[i].instructions[j];
    assert(call_instruction);
    assert(call_instruction->has_semantic_call_identity);
    assert(call_instruction->semantic_call_ordinal < 2);
    assert(call_instruction->control_path_count == 1);
    assert(call_instruction->call_arguments[0].source_resource.binder_id ==
           ap.binder_id);
    call_instruction->semantic_call_ordinal = 2;
    assert(qtt_anf_verify(program).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_calls(
               semantic, &a, program) ==
           QTT_SEMANTIC_ANF_CALL_MISMATCH);
    qtt_anf_program_free(program);
    calls[0].contract_fingerprint ^= 1;
    assert(qtt_semantic_ir_verify(semantic, &a) ==
           QTT_SEMANTIC_IR_CALL_EVIDENCE_MISMATCH);
    qtt_semantic_ir_free(semantic);
    qtt_module_free(module);

    /*
     * Must-transfer is coinductive.  Each edge in this closed tail-recursive
     * SCC hands the same unique capability to the next activation; there is
     * no non-tail or retaining edge that can break the ownership invariant.
     */
    QttCoreVar cp = {42, 1}, dp = {42, 2};
    QttCoreNode cv, cg, ca, dv, dg, da;
    QttCoreNode *cargs[1], *dargs[1];
    const Type *ctypes[1], *dtypes[1];
    QttCoreNode c = function(
        &string_type, &cp, "Cycle.d",
        &cv, &cg, cargs, &ca, ctypes);
    QttCoreNode d = function(
        &string_type, &dp, "Cycle.c",
        &dv, &dg, dargs, &da, dtypes);
    QttModule *cycle = qtt_module_new(42);
    assert(cycle);
    assert(qtt_module_declare(cycle, "Cycle.c", &c) == QTT_MODULE_OK);
    assert(qtt_module_declare(cycle, "Cycle.d", &d) == QTT_MODULE_OK);
    QttFunctionSignature *c_signature = qtt_signature_env_lookup(
        qtt_module_signatures(cycle), 42, "Cycle.c");
    QttFunctionSignature *d_signature = qtt_signature_env_lookup(
        qtt_module_signatures(cycle), 42, "Cycle.d");
    assert(c_signature && d_signature);
    assert(c_signature->parameters[0].mode == QTT_OWNERSHIP_BORROWED);
    assert(d_signature->parameters[0].mode == QTT_OWNERSHIP_BORROWED);
    verification = qtt_module_verify(cycle);
    assert(verification.error == QTT_MODULE_OK);
    assert(verification.verified_count == 2);
    assert(verification.transfers.candidate_count == 2);
    assert(verification.transfers.proven_count == 2);
    assert(verification.transfers.rejected_count == 0);
    assert(verification.transfers.iterations == 1);
    assert(c_signature->parameters[0].mode == QTT_OWNERSHIP_CONSUMED);
    assert(d_signature->parameters[0].mode == QTT_OWNERSHIP_CONSUMED);
    qtt_module_free(cycle);

    QttModule *reversed = qtt_module_new(42);
    assert(reversed);
    assert(qtt_module_declare(reversed, "Cycle.d", &d) == QTT_MODULE_OK);
    assert(qtt_module_declare(reversed, "Cycle.c", &c) == QTT_MODULE_OK);
    verification = qtt_module_verify(reversed);
    assert(verification.error == QTT_MODULE_OK);
    assert(verification.transfers.complete);
    assert(verification.transfers.candidate_count == 2);
    assert(verification.transfers.proven_count == 2);
    assert(verification.transfers.rejected_count == 0);
    assert(qtt_signature_env_lookup(
        qtt_module_signatures(reversed), 42, "Cycle.c")
        ->parameters[0].mode == QTT_OWNERSHIP_CONSUMED);
    assert(qtt_signature_env_lookup(
        qtt_module_signatures(reversed), 42, "Cycle.d")
        ->parameters[0].mode == QTT_OWNERSHIP_CONSUMED);
    qtt_module_free(reversed);

    /* One retaining alternative invalidates the entire cyclic proof. */
    QttCoreVar ep = {43, 1}, fp = {43, 2};
    QttCoreNode ev, eg, ea, fv, fg, fa;
    QttCoreNode *eargs[1], *fargs[1];
    const Type *etypes[1], *ftypes[1];
    QttCoreNode e = function(
        &string_type, &ep, "Broken.f",
        &ev, &eg, eargs, &ea, etypes);
    QttCoreNode f = function(
        &string_type, &fp, "Broken.e",
        &fv, &fg, fargs, &fa, ftypes);
    AST fallback_ast = {.type = AST_STRING, .string = "fallback"};
    QttCoreNode fallback = {
        .kind = QTT_CORE_LITERAL, .type = &string_type,
        .literal = {.source = &fallback_ast}};
    QttCoreNode broken_condition = {
        .kind = QTT_CORE_GLOBAL, .type = &bool_type,
        .global = {.name = "True"}};
    QttCoreNode broken_branch = {
        .kind = QTT_CORE_IF, .type = &string_type,
        .conditional = {.condition = &broken_condition,
                        .then_branch = &ea,
                        .else_branch = &fallback}};
    e.lambda.body = &broken_branch;
    QttModule *broken = qtt_module_new(43);
    assert(broken);
    assert(qtt_module_declare(broken, "Broken.e", &e) == QTT_MODULE_OK);
    assert(qtt_module_declare(broken, "Broken.f", &f) == QTT_MODULE_OK);
    QttFunctionSignature *e_signature = qtt_signature_env_lookup(
        qtt_module_signatures(broken), 43, "Broken.e");
    QttFunctionSignature *f_signature = qtt_signature_env_lookup(
        qtt_module_signatures(broken), 43, "Broken.f");
    assert(e_signature && f_signature);
    verification = qtt_module_verify(broken);
    assert(verification.error == QTT_MODULE_OK);
    assert(verification.verified_count == 2);
    assert(verification.transfers.complete);
    assert(verification.transfers.candidate_count == 2);
    assert(verification.transfers.proven_count == 0);
    assert(verification.transfers.rejected_count == 2);
    assert(verification.transfers.iterations >= 2);
    assert(e_signature->parameters[0].mode == QTT_OWNERSHIP_BORROWED);
    assert(f_signature->parameters[0].mode == QTT_OWNERSHIP_BORROWED);
    qtt_module_free(broken);

    /* Least fixed-point effects cross a mutually recursive SCC.  Only the
     * second function contains the write syntactically; both contracts must
     * expose it after module verification, independent of declaration order. */
    Type number_type = {.kind = TYPE_INT};
    QttCoreVar gp = {44, 1}, hp = {44, 2};
    QttCoreNode gv, gg, ga, hv, hg, ha;
    QttCoreNode *gargs[1], *hargs[1];
    const Type *gtypes[1], *htypes[1];
    QttCoreNode g = function(
        &number_type, &gp, "Effects.h",
        &gv, &gg, gargs, &ga, gtypes);
    QttCoreNode h_call = function(
        &number_type, &hp, "Effects.g",
        &hv, &hg, hargs, &ha, htypes);
    QttCoreNode replacement = {
        .kind = QTT_CORE_LITERAL, .type = &number_type};
    QttCoreNode h_write = {
        .kind = QTT_CORE_WRITE, .type = &number_type,
        .write = {
            .place = {.root = hp},
            .value = &replacement,
        },
    };
    QttCoreNode *h_items[] = {&h_write, &ha};
    QttCoreNode h_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &number_type,
        .sequence = {.items = h_items, .count = 2}};
    h_call.lambda.body = &h_body;
    QttModule *effects = qtt_module_new(44);
    assert(effects);
    assert(qtt_module_declare(effects, "Effects.g", &g) == QTT_MODULE_OK);
    assert(qtt_module_declare(effects, "Effects.h", &h_call) ==
           QTT_MODULE_OK);
    QttFunctionSignature *g_signature = qtt_signature_env_lookup(
        qtt_module_signatures(effects), 44, "Effects.g");
    QttFunctionSignature *h_signature = qtt_signature_env_lookup(
        qtt_module_signatures(effects), 44, "Effects.h");
    assert(g_signature && h_signature);
    assert(!g_signature->effects_complete);
    QttEffectFixedPoint effect_fixed_point =
        qtt_module_solve_effects(effects);
    assert(effect_fixed_point.converged);
    assert(effect_fixed_point.complete);
    assert(effect_fixed_point.iterations >= 2);
    assert(effect_fixed_point.changed_count >= 2);
    assert(g_signature->effects_complete);
    assert(h_signature->effects_complete);
    assert(qtt_effect_label_count(
               g_signature->effect_solver, g_signature->latent_effects,
               "write:44:2:0") == 1);
    assert(qtt_effect_label_count(
               h_signature->effect_solver, h_signature->latent_effects,
               "write:44:2:0") == 1);
    assert(g_signature->contract_fingerprint ==
           qtt_signature_contract_fingerprint(g_signature));
    assert(h_signature->contract_fingerprint ==
           qtt_signature_contract_fingerprint(h_signature));
    uint64_t g_effect_fingerprint = g_signature->effect_fingerprint;
    uint64_t h_effect_fingerprint = h_signature->effect_fingerprint;
    qtt_module_free(effects);

    QttModule *effects_reversed = qtt_module_new(44);
    assert(effects_reversed);
    assert(qtt_module_declare(
               effects_reversed, "Effects.h", &h_call) == QTT_MODULE_OK);
    assert(qtt_module_declare(
               effects_reversed, "Effects.g", &g) == QTT_MODULE_OK);
    effect_fixed_point = qtt_module_solve_effects(effects_reversed);
    assert(effect_fixed_point.converged && effect_fixed_point.complete);
    g_signature = qtt_signature_env_lookup(
        qtt_module_signatures(effects_reversed), 44, "Effects.g");
    h_signature = qtt_signature_env_lookup(
        qtt_module_signatures(effects_reversed), 44, "Effects.h");
    assert(g_signature->effect_fingerprint == g_effect_fingerprint);
    assert(h_signature->effect_fingerprint == h_effect_fingerprint);
    qtt_module_free(effects_reversed);

    QttCoreVar missing_parameter = {45, 1};
    QttCoreNode missing_var, missing_global, missing_apply;
    QttCoreNode *missing_arguments[1];
    const Type *missing_types[1];
    QttCoreNode missing = function(
        &number_type, &missing_parameter, "Missing.foreign",
        &missing_var, &missing_global, missing_arguments,
        &missing_apply, missing_types);
    QttModule *incomplete = qtt_module_new(45);
    assert(incomplete);
    assert(qtt_module_declare(
               incomplete, "Incomplete.entry", &missing) ==
           QTT_MODULE_OK);
    effect_fixed_point = qtt_module_solve_effects(incomplete);
    assert(effect_fixed_point.converged);
    assert(!effect_fixed_point.complete);
    assert(effect_fixed_point.incomplete_count == 1);
    assert(!qtt_signature_env_lookup(
                qtt_module_signatures(incomplete), 45,
                "Incomplete.entry")->effects_complete);
    qtt_module_free(incomplete);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_module_test.c"
            executable = directory / "qtt_module_test"
            harness.write_text(source)
            sources = [
                "core.c", "quantity.c", "demand.c", "graded.c", "resource.c",
                "type_identity.c", "signature.c", "signature_env.c",
                "call.c", "anf.c", "core_usage.c", "../effects/effect.c", "drop.c",
                "environment.c", "elaboration.c", "constraints.c", "eval.c",
                "closure_policy.c", "semantic_ir.c",
                "semantic_anf.c", "module.c",
            ]
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT / "src"), str(harness),
                    *(str(ROOT / "src" / "qtt" / source) for source in sources),
                    "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
