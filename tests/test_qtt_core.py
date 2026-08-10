"""Typed Core lowering contracts for resource analysis."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class QttCoreTests(unittest.TestCase):
    def test_lowering_preserves_types_identity_and_control_flow(self):
        source = r'''
#include "qtt/core.h"
#include "qtt/place.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

static AST symbol(char *name, uint64_t binder_id, Type *type) {
    AST ast = {0};
    ast.type = AST_SYMBOL;
    ast.symbol = name;
    ast.resolved_binder_id = binder_id;
    ast.inferred_type = type;
    return ast;
}

int main(void) {
    Type integer = {.kind = TYPE_INT};
    Type arrow = {
        .kind = TYPE_ARROW,
        .arrow_param = &integer,
        .arrow_ret = &integer,
    };
    Type *int_type = &integer;
    Type *bool_type = (Type *)(uintptr_t)0x20;
    AST if_head = symbol("if", 0, 0);
    AST condition = symbol("condition", 0, bool_type);
    AST then_use = symbol("x", 7, int_type);
    AST else_use = symbol("x", 7, int_type);
    AST *if_items[] = {&if_head, &condition, &then_use, &else_use};
    AST branch = {0};
    branch.type = AST_LIST;
    branch.list.items = if_items;
    branch.list.count = 4;
    branch.inferred_type = int_type;

    ASTParam param = {.name = "x", .binder_id = 7};
    AST *body[] = {&branch};
    AST lambda = {0};
    lambda.type = AST_LAMBDA;
    lambda.lambda.params = &param;
    lambda.lambda.param_count = 1;
    lambda.lambda.body_exprs = body;
    lambda.lambda.body_count = 1;
    lambda.inferred_type = &arrow;

    QttCoreError error = QTT_CORE_OK;
    QttCoreNode *core = qtt_core_lower(&lambda, 42, &error);
    assert(core && error == QTT_CORE_OK);
    assert(core->kind == QTT_CORE_LAMBDA);
    assert(core->type == &arrow);
    assert(core->lambda.param_count == 1);
    assert(core->lambda.param_types);
    assert(core->lambda.param_types[0] == int_type);
    assert(core->lambda.params[0].module_id == 42);
    assert(core->lambda.params[0].binder_id == 7);
    assert(core->lambda.body->kind == QTT_CORE_IF);
    assert(core->lambda.body->type == int_type);
    assert(core->lambda.body->conditional.condition->kind == QTT_CORE_GLOBAL);
    assert(core->lambda.body->conditional.then_branch->kind == QTT_CORE_VAR);
    assert(core->lambda.body->conditional.else_branch->kind == QTT_CORE_VAR);
    assert(qtt_core_var_equal(
        core->lambda.body->conditional.then_branch->var,
        core->lambda.body->conditional.else_branch->var));
    assert(qtt_core_validate(core, 42) == QTT_CORE_VALID);
    qtt_core_free(core);

    Type string = {.kind = TYPE_STRING};
    LayoutField person_fields[] = {
        {.name = "name", .type = &string},
        {.name = "age", .type = &integer},
    };
    Type person = {
        .kind = TYPE_LAYOUT,
        .layout_fields = person_fields,
        .layout_field_count = 2,
    };
    AST projected_head = symbol("with", 0, 0);
    AST projected_name = symbol("person", 20, &person);
    AST projected_value = {0};
    projected_value.type = AST_STRING;
    projected_value.string = "Ada";
    projected_value.inferred_type = &person;
    AST *projected_binding_items[] = {
        &projected_name, &projected_value};
    AST projected_bindings = {0};
    projected_bindings.type = AST_ARRAY;
    projected_bindings.array.elements = projected_binding_items;
    projected_bindings.array.element_count = 2;
    AST projected_use = symbol("person.name", 20, &string);
    AST *projected_items[] = {
        &projected_head, &projected_bindings, &projected_use};
    AST projected_source = {0};
    projected_source.type = AST_LIST;
    projected_source.list.items = projected_items;
    projected_source.list.count = 3;
    projected_source.inferred_type = &string;
    core = qtt_core_lower(&projected_source, 42, &error);
    assert(core && error == QTT_CORE_OK);
    assert(core->kind == QTT_CORE_LET);
    assert(core->let.body->kind == QTT_CORE_PLACE);
    assert(core->let.body->place.projection_depth == 1);
    assert(core->let.body->place.projection_path[0] == 1);
    assert(qtt_core_var_equal(
        core->let.body->place.root, core->let.binding));
    assert(qtt_core_validate(core, 42) == QTT_CORE_VALID);
    qtt_core_free(core);

    AST projected_set_head = symbol("set!", 0, 0);
    AST projected_target = symbol("person.name", 20, &string);
    AST projected_replacement = {0};
    projected_replacement.type = AST_STRING;
    projected_replacement.string = "Grace";
    projected_replacement.inferred_type = &string;
    AST *projected_set_items[] = {
        &projected_set_head, &projected_target, &projected_replacement};
    AST projected_set = {0};
    projected_set.type = AST_LIST;
    projected_set.list.items = projected_set_items;
    projected_set.list.count = 3;
    projected_set.inferred_type = &string;
    AST *projected_mutation_items[] = {
        &projected_head, &projected_bindings, &projected_set};
    AST projected_mutation = {0};
    projected_mutation.type = AST_LIST;
    projected_mutation.list.items = projected_mutation_items;
    projected_mutation.list.count = 3;
    /* Legacy inference may leave with unannotated; Core inherits body type. */
    projected_mutation.inferred_type = NULL;
    core = qtt_core_lower(&projected_mutation, 42, &error);
    assert(core && error == QTT_CORE_OK);
    assert(core->kind == QTT_CORE_LET);
    assert(core->type == &string);
    QttCoreNode *projected_loan = core->let.body;
    assert(projected_loan->kind == QTT_CORE_BORROW);
    assert(projected_loan->borrow.loan_kind == QTT_LOAN_EXCLUSIVE);
    assert(projected_loan->borrow.parent.binder_id == 0);
    assert(projected_loan->borrow.binding.module_id == 42);
    assert(projected_loan->borrow.binding.binder_id != 20);
    assert(projected_loan->borrow.place.projection_depth == 1);
    assert(projected_loan->borrow.place.projection_path[0] == 1);
    assert(projected_loan->borrow.body->kind == QTT_CORE_WRITE);
    assert(qtt_place_equal(projected_loan->borrow.body->write.place,
                           projected_loan->borrow.place));
    assert(qtt_core_validate(core, 42) == QTT_CORE_VALID);
    qtt_core_free(core);

    AST with_head = symbol("with", 0, 0);
    AST local_name = symbol("local", 8, int_type);
    AST outer_use = symbol("x", 7, int_type);
    AST local_use = symbol("local", 8, int_type);
    AST *binding_items[] = {&local_name, &outer_use};
    AST bindings = {0};
    bindings.type = AST_ARRAY;
    bindings.array.elements = binding_items;
    bindings.array.element_count = 2;
    AST *with_items[] = {&with_head, &bindings, &local_use};
    AST with = {0};
    with.type = AST_LIST;
    with.list.items = with_items;
    with.list.count = 3;
    with.inferred_type = int_type;

    core = qtt_core_lower(&with, 42, &error);
    assert(core && core->kind == QTT_CORE_LET);
    assert(core->let.binding.module_id == 42);
    assert(core->let.binding.binder_id == 8);
    assert(core->let.value->kind == QTT_CORE_VAR);
    assert(core->let.value->var.binder_id == 7);
    assert(core->let.body->kind == QTT_CORE_VAR);
    assert(core->let.body->var.binder_id == 8);
    assert(qtt_core_validate(core, 42) == QTT_CORE_UNBOUND_VAR);
    qtt_core_free(core);

    AST set_head = symbol("set!", 0, 0);
    AST set_target = symbol("x", 7, int_type);
    AST set_value = {0};
    set_value.type = AST_NUMBER;
    set_value.number = 9;
    set_value.inferred_type = int_type;
    AST *set_items[] = {&set_head, &set_target, &set_value};
    AST set = {0};
    set.type = AST_LIST;
    set.list.items = set_items;
    set.list.count = 3;
    set.inferred_type = int_type;
    AST *mutation_body[] = {&set, &then_use};
    lambda.lambda.body_exprs = mutation_body;
    lambda.lambda.body_count = 2;
    core = qtt_core_lower(&lambda, 42, &error);
    assert(core && error == QTT_CORE_OK);
    assert(core->lambda.body->kind == QTT_CORE_SEQUENCE);
    QttCoreNode *write = core->lambda.body->sequence.items[0];
    assert(write->kind == QTT_CORE_WRITE);
    assert(write->write.place.root.module_id == 42);
    assert(write->write.place.root.binder_id == 7);
    assert(write->write.place.projection_id == 0);
    assert(write->write.value->kind == QTT_CORE_LITERAL);
    assert(qtt_core_validate(core, 42) == QTT_CORE_VALID);
    qtt_core_free(core);

    set_target.resolved_binder_id = 0;
    core = qtt_core_lower(&set, 42, &error);
    assert(!core && error == QTT_CORE_MALFORMED_AST);

    QttCoreNode rogue = {0};
    rogue.kind = QTT_CORE_VAR;
    rogue.var = (QttCoreVar){.module_id = 99, .binder_id = 1};
    assert(qtt_core_validate(&rogue, 42) == QTT_CORE_WRONG_MODULE);

    QttCoreNode captured_use = {
        .kind = QTT_CORE_VAR,
        .var = {.module_id = 42, .binder_id = 70},
    };
    QttCoreNode captured_twice_items[] = {captured_use, captured_use};
    QttCoreNode *captured_item_ptrs[] = {
        &captured_twice_items[0], &captured_twice_items[1],
    };
    QttCoreNode captured_body = {
        .kind = QTT_CORE_SEQUENCE,
        .sequence = {.items = captured_item_ptrs, .count = 2},
    };
    QttCoreNode closure = {
        .kind = QTT_CORE_LAMBDA,
        .lambda = {.body = &captured_body},
    };
    QttCoreVar *captures = NULL;
    size_t capture_count = 0;
    assert(qtt_core_lambda_captures(
        &closure, &captures, &capture_count) == QTT_CORE_OK);
    assert(capture_count == 1);
    assert(captures[0].module_id == 42 && captures[0].binder_id == 70);
    free(captures);

    /* Effect computations are semantic Core nodes, never disguised calls. */
    AST perform_head = symbol("perform", 0, 0);
    AST effect_name = symbol("Core.Console.read", 0, 0);
    AST perform_arg = {0};
    perform_arg.type = AST_STRING;
    perform_arg.string = "prompt";
    perform_arg.inferred_type = &string;
    AST *perform_items[] = {&perform_head, &effect_name, &perform_arg};
    AST perform = {0};
    perform.type = AST_LIST;
    perform.list.items = perform_items;
    perform.list.count = 3;
    perform.inferred_type = &string;
    perform.line = 31;
    perform.column = 7;
    core = qtt_core_lower(&perform, 42, &error);
    assert(core && error == QTT_CORE_OK);
    assert(core->kind == QTT_CORE_PERFORM);
    assert(core->perform.effect_name == effect_name.symbol);
    assert(core->perform.capability_id != 0);
    assert(core->perform.argument->kind == QTT_CORE_LITERAL);
    assert(qtt_core_validate(core, 42) == QTT_CORE_VALID);
    uint64_t capability = core->perform.capability_id;
    qtt_core_free(core);
    core = qtt_core_lower(&perform, 42, &error);
    assert(core && core->perform.capability_id == capability);
    qtt_core_free(core);
    core = qtt_core_lower(&perform, 43, &error);
    assert(core && core->perform.capability_id != capability);
    qtt_core_free(core);

    AST handle_head = symbol("handle", 0, 0);
    AST profile_name = symbol("Core.Console.read-once", 0, 0);
    AST clause = symbol("read-clause", 0, &arrow);
    AST *handle_items[] = {
        &handle_head, &profile_name, &perform, &clause};
    AST handle = {0};
    handle.type = AST_LIST;
    handle.list.items = handle_items;
    handle.list.count = 4;
    handle.inferred_type = &string;
    handle.line = 30;
    handle.column = 3;
    core = qtt_core_lower(&handle, 42, &error);
    assert(core && error == QTT_CORE_OK);
    assert(core->kind == QTT_CORE_HANDLE);
    assert(core->handle.profile_name == profile_name.symbol);
    assert(core->handle.capability_id != 0);
    assert(core->handle.computation->kind == QTT_CORE_PERFORM);
    assert(core->handle.clause->kind == QTT_CORE_GLOBAL);
    assert(qtt_core_validate(core, 42) == QTT_CORE_VALID);
    qtt_core_free(core);

    perform.list.count = 2;
    core = qtt_core_lower(&perform, 42, &error);
    assert(!core && error == QTT_CORE_MALFORMED_AST);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_core_test.c"
            executable = directory / "qtt_core_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "core.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
