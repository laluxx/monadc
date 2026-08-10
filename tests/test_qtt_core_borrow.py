import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class QttCoreBorrowTests(unittest.TestCase):
    def test_nested_reborrow_survives_core_semantic_and_resource_ir(self):
        source = r'''
#include "qtt/core.h"
#include "qtt/place.h"
#include "qtt/resource.h"
#include "qtt/semantic_ir.h"
#include "qtt/semantic_anf.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    Type aggregate = {.kind = TYPE_LAYOUT};
    Type number = {.kind = TYPE_INT};
    AST literal_ast = {.type = AST_NUMBER, .number = 7,
                       .inferred_type = &number};
    QttCoreVar owner = {77, 1};
    QttCoreVar parent = {77, 2};
    QttCoreVar child = {77, 3};
    QttPlace whole = qtt_place_root(owner);
    QttPlace field = whole;
    field.projection_depth = 1;
    field.projection_path[0] = 1;
    field.projection_id = 1;
    QttCoreNode value = {.kind = QTT_CORE_LITERAL, .type = &number,
                         .literal.source = &literal_ast};
    QttCoreNode write = {.kind = QTT_CORE_WRITE, .type = &number,
                         .write = {.place = field, .value = &value}};
    QttCoreNode inner = {
        .kind = QTT_CORE_BORROW, .type = &number,
        .borrow = {.binding = child, .parent = parent, .place = field,
                   .loan_kind = QTT_LOAN_EXCLUSIVE, .body = &write},
    };
    QttCoreNode outer = {
        .kind = QTT_CORE_BORROW, .type = &number,
        .borrow = {.binding = parent, .place = whole,
                   .loan_kind = QTT_LOAN_EXCLUSIVE, .body = &inner},
    };
    AST aggregate_ast = {.type = AST_LIST, .inferred_type = &aggregate};
    QttCoreNode initial = {.kind = QTT_CORE_LITERAL, .type = &aggregate,
                           .literal.source = &aggregate_ast};
    QttCoreNode root = {.kind = QTT_CORE_LET, .type = &number,
                        .let = {.binding = owner, .value = &initial,
                                .body = &outer}};

    assert(qtt_core_validate(&root, 77) == QTT_CORE_VALID);
    QttDemandError demand_error;
    QttDemandCertificate *demand = qtt_demand_derive(&root, &demand_error);
    assert(demand && demand_error == QTT_DEMAND_OK);
    assert(qtt_demand_occurrences(demand, &outer, owner) == 3);
    QttQuantity owner_runtime = qtt_demand_runtime(demand, &outer, owner);
    assert(!owner_runtime.is_omega && owner_runtime.finite == 3);
    qtt_demand_certificate_free(demand);
    QttResourceElaborationError resource_error;
    QttResourceBlock *resources = qtt_resource_elaborate_lexical(
        &root, &resource_error);
    if (!resources) fprintf(stderr, "resource error=%d\n", resource_error);
    assert(resources && resource_error == QTT_RESOURCE_ELABORATE_OK);
    assert(qtt_resource_verify(resources, NULL, 0).error == QTT_RESOURCE_VALID);
    size_t parent_at = (size_t)-1, child_at = (size_t)-1;
    size_t parent_end = (size_t)-1, child_end = (size_t)-1;
    for (size_t i = 0; i < resources->count; ++i) {
        QttResourceOp *op = &resources->ops[i];
        if (op->kind == QTT_RESOURCE_ALIAS &&
            qtt_core_var_equal(op->var, parent)) parent_at = i;
        if (op->kind == QTT_RESOURCE_ALIAS &&
            qtt_core_var_equal(op->var, child)) {
            child_at = i;
            assert(qtt_core_var_equal(op->parent_alias, parent));
            assert(qtt_place_equal(op->place, field));
        }
        if (op->kind == QTT_RESOURCE_END_ALIAS &&
            qtt_core_var_equal(op->var, parent)) parent_end = i;
        if (op->kind == QTT_RESOURCE_END_ALIAS &&
            qtt_core_var_equal(op->var, child)) child_end = i;
    }
    assert(parent_at < child_at && child_at < child_end &&
           child_end < parent_end);

    QttSemanticIrError semantic_error;
    QttSemanticFunction *semantic = qtt_semantic_ir_lower(
        &root, &semantic_error);
    assert(semantic && semantic_error == QTT_SEMANTIC_IR_OK);
    assert(qtt_semantic_ir_verify(semantic, &root) ==
           QTT_SEMANTIC_IR_VALID);
    QttSemanticNode *nodes = qtt_semantic_ir_nodes(semantic);
    assert(nodes[2].kind == QTT_CORE_BORROW);
    assert(qtt_core_var_equal(nodes[2].var, parent));
    assert(nodes[2].loan_kind == QTT_LOAN_EXCLUSIVE);
    assert(nodes[3].kind == QTT_CORE_BORROW);
    assert(qtt_core_var_equal(nodes[3].parent_loan, parent));
    assert(qtt_place_equal(nodes[3].place, field));
    nodes[3].parent_loan = owner;
    assert(qtt_semantic_ir_verify(semantic, &root) ==
           QTT_SEMANTIC_IR_PROVENANCE_MISMATCH);
    nodes[3].parent_loan = parent;

    QttSemanticAnfError bridge_error;
    QttAnfLowerError lower_error;
    QttAnfProgram *anf = qtt_semantic_anf_lower(
        semantic, &root, &bridge_error, &lower_error);
    assert(anf && bridge_error == QTT_SEMANTIC_ANF_OK &&
           lower_error == QTT_ANF_LOWER_OK);
    QttAnfInstruction *parent_borrow = NULL, *child_borrow = NULL;
    for (size_t block = 0; block < anf->block_count; ++block)
        for (size_t i = 0; i < anf->blocks[block].instruction_count; ++i) {
            QttAnfInstruction *op = &anf->blocks[block].instructions[i];
            if (op->kind != QTT_ANF_BORROW ||
                !op->has_semantic_loan_identity) continue;
            if (op->semantic_loan_ordinal == 0) parent_borrow = op;
            if (op->semantic_loan_ordinal == 1) child_borrow = op;
        }
    assert(parent_borrow && child_borrow);
    assert(child_borrow->parent_loan_id == parent_borrow->loan_id);
    assert(qtt_place_equal(child_borrow->place, field));
    assert(qtt_anf_verify(anf).error == QTT_ANF_VALID);
    assert(qtt_semantic_anf_verify_correspondence(
        semantic, &root, anf) == QTT_SEMANTIC_ANF_OK);
    child_borrow->parent_loan_id = 0;
    assert(qtt_semantic_anf_verify_correspondence(
        semantic, &root, anf) == QTT_SEMANTIC_ANF_LOAN_MISMATCH);
    child_borrow->parent_loan_id = parent_borrow->loan_id;
    child_borrow->has_semantic_loan_identity = false;
    assert(qtt_semantic_anf_verify_correspondence(
        semantic, &root, anf) == QTT_SEMANTIC_ANF_LOAN_MISMATCH);
    child_borrow->has_semantic_loan_identity = true;
    child_borrow->loan_kind = QTT_LOAN_SHARED;
    assert(qtt_semantic_anf_verify_correspondence(
        semantic, &root, anf) == QTT_SEMANTIC_ANF_LOAN_MISMATCH);
    child_borrow->loan_kind = QTT_LOAN_EXCLUSIVE;
    child_borrow->place = whole;
    assert(qtt_semantic_anf_verify_correspondence(
        semantic, &root, anf) == QTT_SEMANTIC_ANF_LOAN_MISMATCH);
    child_borrow->place = field;
    assert(qtt_semantic_anf_verify_correspondence(
        semantic, &root, anf) == QTT_SEMANTIC_ANF_OK);
    qtt_anf_program_free(anf);
    qtt_semantic_ir_free(semantic);
    qtt_resource_block_free(resources);

    inner.borrow.parent = owner;
    assert(qtt_core_validate(&root, 77) == QTT_CORE_UNBOUND_VAR);
    inner.borrow.parent = parent;
    inner.borrow.loan_kind = QTT_LOAN_SHARED;
    resources = qtt_resource_elaborate_lexical(&root, &resource_error);
    assert(!resources && resource_error ==
           QTT_RESOURCE_ELABORATE_INVALID_RESOURCE_STATE);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "core_borrow.c"
            executable = directory / "core_borrow"
            harness.write_text(source)
            subprocess.run([
                "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-iquote", str(ROOT), str(harness),
                *[str(ROOT / "qtt" / name) for name in [
                    "core.c", "quantity.c", "demand.c", "graded.c",
                    "resource.c", "eval.c", "signature.c", "call.c",
                    "type_identity.c", "signature_env.c", "constraints.c",
                    "environment.c", "elaboration.c", "core_usage.c",
                    "../effects/effect.c", "drop.c", "closure_policy.c",
                    "semantic_ir.c", "anf.c", "semantic_anf.c",
                ]],
                "-o", str(executable),
            ], cwd=ROOT, check=True)
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
