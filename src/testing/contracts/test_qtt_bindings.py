"""Regression tests for persistent lexical BinderId resolution."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


class QttBindingTests(unittest.TestCase):
    def test_nested_shadowing_resolves_by_identity(self):
        source = r'''
#include "qtt/bindings.h"
#include <assert.h>

static AST symbol(char *name) {
    AST ast = {0};
    ast.type = AST_SYMBOL;
    ast.symbol = name;
    return ast;
}
static AST list(AST **items, size_t count) {
    AST ast = {0};
    ast.type = AST_LIST;
    ast.list.items = items;
    ast.list.count = count;
    return ast;
}
static AST lambda1(char *name, AST *body, ASTParam *param, AST **body_slot) {
    AST ast = {0};
    param->name = name;
    body_slot[0] = body;
    ast.type = AST_LAMBDA;
    ast.lambda.params = param;
    ast.lambda.param_count = 1;
    ast.lambda.body_exprs = body_slot;
    ast.lambda.body_count = 1;
    return ast;
}

int main(void) {
    AST outer_use = symbol("x");
    AST inner_use = symbol("x");
    ASTParam inner_param = {0};
    AST *inner_body[1];
    AST inner = lambda1("x", &inner_use, &inner_param, inner_body);

    AST begin_symbol = symbol("begin");
    AST *sequence_items[] = {&begin_symbol, &outer_use, &inner};
    AST sequence = {0};
    sequence.type = AST_LIST;
    sequence.list.items = sequence_items;
    sequence.list.count = 3;

    ASTParam outer_param = {0};
    AST *outer_body[1];
    AST outer = lambda1("x", &sequence, &outer_param, outer_body);

    QttBindingSummary summary = qtt_bindings_resolve(&outer);
    assert(summary.error == QTT_BINDINGS_OK);
    assert(summary.binder_count == 2);
    assert(outer_param.binder_id != QTT_BINDER_UNRESOLVED);
    assert(inner_param.binder_id != QTT_BINDER_UNRESOLVED);
    assert(outer_param.binder_id != inner_param.binder_id);
    assert(outer.qtt_closure_id != 0);
    assert(inner.qtt_closure_id != 0);
    assert(outer.qtt_closure_id != inner.qtt_closure_id);
    uint64_t outer_closure_id = outer.qtt_closure_id;
    uint64_t inner_closure_id = inner.qtt_closure_id;
    assert(outer_use.resolved_binder_id == outer_param.binder_id);
    assert(inner_use.resolved_binder_id == inner_param.binder_id);
    assert(begin_symbol.resolved_binder_id == QTT_BINDER_UNRESOLVED);
    summary = qtt_bindings_resolve(&outer);
    assert(summary.error == QTT_BINDINGS_OK);
    assert(outer.qtt_closure_id == outer_closure_id);
    assert(inner.qtt_closure_id == inner_closure_id);

    AST with_symbol = symbol("with");
    AST local_name = symbol("x");
    AST initializer_use = symbol("x");
    AST local_use = symbol("x");
    AST local_field_use = symbol("x.name");
    AST *binding_elements[] = {&local_name, &initializer_use};
    AST bindings = {0};
    bindings.type = AST_ARRAY;
    bindings.array.elements = binding_elements;
    bindings.array.element_count = 2;
    AST begin_local_symbol = symbol("begin");
    AST *local_sequence_items[] = {
        &begin_local_symbol, &local_use, &local_field_use};
    AST local_sequence = list(local_sequence_items, 3);
    AST *with_items[] = {&with_symbol, &bindings, &local_sequence};
    AST with = list(with_items, 3);
    ASTParam with_param = {0};
    AST *with_body[1];
    AST with_lambda = lambda1("x", &with, &with_param, with_body);
    summary = qtt_bindings_resolve(&with_lambda);
    assert(summary.error == QTT_BINDINGS_OK);
    assert(summary.binder_count == 2);
    assert(initializer_use.resolved_binder_id == with_param.binder_id);
    assert(local_name.resolved_binder_id != with_param.binder_id);
    assert(local_use.resolved_binder_id == local_name.resolved_binder_id);
    assert(local_field_use.resolved_binder_id ==
           local_name.resolved_binder_id);

    AST pattern_use = symbol("item");
    ASTPattern pattern = {0};
    pattern.kind = PAT_VAR;
    pattern.var_name = "item";
    ASTPMatchClause clause = {0};
    clause.patterns = &pattern;
    clause.pattern_count = 1;
    clause.body = &pattern_use;
    AST match = {0};
    match.type = AST_PMATCH;
    match.pmatch.clauses = &clause;
    match.pmatch.clause_count = 1;
    summary = qtt_bindings_resolve(&match);
    assert(summary.error == QTT_BINDINGS_OK);
    assert(summary.binder_count == 1);
    assert(pattern.binder_id != QTT_BINDER_UNRESOLVED);
    assert(pattern_use.resolved_binder_id == pattern.binder_id);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_bindings_test.c"
            executable = directory / "qtt_bindings_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-Wno-switch", "-D_GNU_SOURCE",
                    "-iquote", str(ROOT / "src"), str(harness),
                    str(ROOT / "src" / "qtt" / "bindings.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
