"""Structural destruction contracts for finite unique ownership trees."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class QttDropTests(unittest.TestCase):
    def test_nested_owned_fields_are_reclaimed_transitively(self):
        source = r'''
#include "qtt/drop.h"
#include <assert.h>

int main(void) {
    Type integer = {.kind = TYPE_INT};
    Type string = {.kind = TYPE_STRING};
    LayoutField fields[] = {
        {.name = "name", .type = &string},
        {.name = "age", .type = &integer},
    };
    Type person = {
        .kind = TYPE_LAYOUT,
        .layout_fields = fields,
        .layout_field_count = 2,
    };

    QttDropPlanError plan_error = QTT_DROP_PLAN_OK;
    QttDropPlan *plan = qtt_drop_plan_build(&person, &plan_error);
    assert(plan && plan_error == QTT_DROP_PLAN_OK);
    assert(plan->kind == QTT_DROP_STRUCT);
    assert(plan->child_count == 2);
    assert(plan->children[0]->kind == QTT_DROP_OWNED_LEAF);
    assert(plan->children[1]->kind == QTT_DROP_NOOP);
    QttDestructorDescriptor *descriptor =
        qtt_destructor_descriptor_build(&person, &plan_error);
    assert(descriptor && qtt_destructor_descriptor_verify(
        descriptor, &person));
    LayoutField equivalent_fields[] = {
        {.name = "name", .type = &string},
        {.name = "age", .type = &integer},
    };
    Type equivalent_person = {
        .kind = TYPE_LAYOUT,
        .layout_fields = equivalent_fields,
        .layout_field_count = 2,
    };
    QttDestructorDescriptor *equivalent_descriptor =
        qtt_destructor_descriptor_build(
            &equivalent_person, &plan_error);
    assert(equivalent_descriptor);
    assert(qtt_destructor_id_equal(
        descriptor->id, equivalent_descriptor->id));
    descriptor->plan->kind = QTT_DROP_OWNED_LEAF;
    assert(!qtt_destructor_descriptor_verify(
        descriptor, &person));
    descriptor->plan->kind = QTT_DROP_STRUCT;

    QttPlace person_place = qtt_place_root(
        (QttCoreVar){.module_id = 9, .binder_id = 4});
    QttPlace moved_name = {0};
    assert(qtt_place_layout_field(
        person_place, &person, "name", &moved_name));
    QttDropMaskError mask_error = QTT_DROP_MASK_VALID;
    QttDropMaskCertificate *mask = qtt_drop_mask_build(
        descriptor, person_place, &moved_name, 1, &mask_error);
    assert(mask && mask_error == QTT_DROP_MASK_VALID);
    assert(qtt_drop_mask_verify(
        descriptor, mask) == QTT_DROP_MASK_VALID);

    QttCoreVar field_alias = {.module_id = 9, .binder_id = 5};
    QttResourceOp certified_ops[] = {
        qtt_resource_alloc_typed(
            person_place.root, QTT_REP_OWNED_HEAP),
        qtt_resource_alias_place(
            field_alias, moved_name, QTT_LOAN_EXCLUSIVE),
        qtt_resource_move_alias_place(field_alias, moved_name),
        qtt_resource_end_alias_place(
            field_alias, moved_name, QTT_LOAN_EXCLUSIVE),
        qtt_resource_drop(person_place.root),
    };
    QttResourceBlock certified_resources = {
        certified_ops,
        sizeof(certified_ops) / sizeof(certified_ops[0]),
    };
    QttDropMaskCertificate *derived_mask =
        qtt_drop_mask_build_for_resource(
            descriptor, &certified_resources, person_place.root,
            NULL, 0, &mask_error);
    assert(derived_mask && mask_error == QTT_DROP_MASK_VALID);
    assert(derived_mask->evacuated_count == 1);
    assert(qtt_place_equal(derived_mask->evacuated[0], moved_name));
    qtt_drop_mask_free(derived_mask);

    QttCoreVar then_alias = {.module_id = 9, .binder_id = 6};
    QttCoreVar else_alias = {.module_id = 9, .binder_id = 7};
    QttResourceOp then_ops[] = {
        qtt_resource_alias_place(
            then_alias, moved_name, QTT_LOAN_EXCLUSIVE),
        qtt_resource_move_alias_place(then_alias, moved_name),
        qtt_resource_end_alias_place(
            then_alias, moved_name, QTT_LOAN_EXCLUSIVE),
    };
    QttResourceOp else_ops[] = {
        qtt_resource_alias_place(
            else_alias, moved_name, QTT_LOAN_EXCLUSIVE),
        qtt_resource_move_alias_place(else_alias, moved_name),
        qtt_resource_end_alias_place(
            else_alias, moved_name, QTT_LOAN_EXCLUSIVE),
    };
    QttResourceBlock then_block = {then_ops, 3};
    QttResourceBlock else_block = {else_ops, 3};
    QttResourceOp joined_ops[] = {
        qtt_resource_alloc_typed(
            person_place.root, QTT_REP_OWNED_HEAP),
        qtt_resource_branch(&then_block, &else_block),
        qtt_resource_drop(person_place.root),
    };
    QttResourceBlock joined_resources = {joined_ops, 3};
    derived_mask = qtt_drop_mask_build_for_resource(
        descriptor, &joined_resources, person_place.root,
        NULL, 0, &mask_error);
    assert(derived_mask && derived_mask->evacuated_count == 1);
    assert(qtt_place_equal(derived_mask->evacuated[0], moved_name));
    qtt_drop_mask_free(derived_mask);

    QttOwnedObject *moved_name_object = qtt_owned_object_new(32, 0);
    QttOwnedObject *partial_root = qtt_owned_object_new(31, 2);
    assert(moved_name_object && partial_root);
    partial_root->children[0] = moved_name_object;
    QttDropExecution partial_execution = qtt_drop_execute_masked(
        descriptor, mask, partial_root);
    assert(partial_execution.error == QTT_DROP_VALID);
    assert(partial_execution.visited == 1);
    assert(partial_execution.reclaimed == 1);
    assert(partial_root->dropped);
    assert(!moved_name_object->dropped);

    mask->evacuated[0].projection_path[0] = 3;
    assert(qtt_drop_mask_verify(
        descriptor, mask) == QTT_DROP_MASK_INVALID_PATH);
    mask->evacuated[0] = moved_name;
    mask->destructor.plan_fingerprint++;
    assert(qtt_drop_mask_verify(
        descriptor, mask) == QTT_DROP_MASK_INVALID_DESTRUCTOR);
    mask->destructor = descriptor->id;
    qtt_drop_mask_free(mask);
    qtt_owned_object_free_storage(partial_root);
    qtt_owned_object_free_storage(moved_name_object);

    /* Nested certificates preserve prefixes and reject ambiguous masks. */
    LayoutField transform_fields[] = {
        {.name = "position", .type = &string},
        {.name = "rotation", .type = &string},
    };
    Type transform = {
        .kind = TYPE_LAYOUT,
        .layout_fields = transform_fields,
        .layout_field_count = 2,
    };
    LayoutField entity_fields[] = {
        {.name = "transform", .type = &transform},
        {.name = "label", .type = &string},
    };
    Type entity = {
        .kind = TYPE_LAYOUT,
        .layout_fields = entity_fields,
        .layout_field_count = 2,
    };
    QttDestructorDescriptor *entity_descriptor =
        qtt_destructor_descriptor_build(&entity, &plan_error);
    assert(entity_descriptor);
    QttPlace entity_place = qtt_place_root(
        (QttCoreVar){.module_id = 10, .binder_id = 5});
    QttPlace transform_place = {0};
    QttPlace position_place = {0};
    QttPlace label_place = {0};
    assert(qtt_place_layout_field(
        entity_place, &entity, "transform", &transform_place));
    assert(qtt_place_layout_field(
        transform_place, &transform, "position", &position_place));
    assert(qtt_place_layout_field(
        entity_place, &entity, "label", &label_place));

    QttPlace overlapping[] = {transform_place, position_place};
    assert(!qtt_drop_mask_build(entity_descriptor, entity_place,
                                overlapping, 2, &mask_error));
    assert(mask_error == QTT_DROP_MASK_OVERLAP);
    QttPlace unordered[] = {label_place, position_place};
    QttDropMaskCertificate *canonical = qtt_drop_mask_build(
        entity_descriptor, entity_place, unordered, 2, &mask_error);
    assert(canonical && mask_error == QTT_DROP_MASK_VALID);
    assert(canonical->evacuated[0].projection_path[0] == 1);
    assert(canonical->evacuated[1].projection_path[0] == 2);
    qtt_drop_mask_free(canonical);
    QttPlace invalid_root = transform_place;
    assert(!qtt_drop_mask_build(entity_descriptor, invalid_root,
                                &position_place, 1, &mask_error));
    assert(mask_error == QTT_DROP_MASK_INVALID_ROOT);

    QttDropMaskCertificate *nested_mask = qtt_drop_mask_build(
        entity_descriptor, entity_place, &position_place, 1, &mask_error);
    assert(nested_mask && mask_error == QTT_DROP_MASK_VALID);
    QttOwnedObject *position_object = qtt_owned_object_new(42, 0);
    QttOwnedObject *rotation_object = qtt_owned_object_new(43, 0);
    QttOwnedObject *transform_object = qtt_owned_object_new(41, 2);
    QttOwnedObject *label_object = qtt_owned_object_new(44, 0);
    QttOwnedObject *entity_object = qtt_owned_object_new(40, 2);
    assert(position_object && rotation_object && transform_object &&
           label_object && entity_object);
    transform_object->children[0] = position_object;
    transform_object->children[1] = rotation_object;
    entity_object->children[0] = transform_object;
    entity_object->children[1] = label_object;
    partial_execution = qtt_drop_execute_masked(
        entity_descriptor, nested_mask, entity_object);
    assert(partial_execution.error == QTT_DROP_VALID);
    assert(partial_execution.reclaimed == 4);
    assert(!position_object->dropped);
    assert(rotation_object->dropped && transform_object->dropped &&
           label_object->dropped && entity_object->dropped);
    qtt_drop_mask_free(nested_mask);
    qtt_owned_object_free_storage(position_object);
    qtt_owned_object_free_storage(rotation_object);
    qtt_owned_object_free_storage(transform_object);
    qtt_owned_object_free_storage(label_object);
    qtt_owned_object_free_storage(entity_object);
    qtt_destructor_descriptor_free(entity_descriptor);

    QttOwnedObject *name = qtt_owned_object_new(2, 0);
    QttOwnedObject *root = qtt_owned_object_new(1, 2);
    assert(name && root);
    root->children[0] = name;
    root->children[1] = NULL;

    QttDropExecution execution = qtt_drop_execute(plan, root);
    assert(execution.error == QTT_DROP_VALID);
    assert(execution.visited == 2);
    assert(execution.reclaimed == 2);
    assert(execution.live == 0);
    assert(root->dropped && name->dropped);

    execution = qtt_drop_execute(plan, root);
    assert(execution.error == QTT_DROP_DOUBLE);

    QttOwnedObject *shared = qtt_owned_object_new(4, 0);
    QttOwnedObject *bad_root = qtt_owned_object_new(3, 2);
    assert(shared && bad_root);
    bad_root->children[0] = shared;
    bad_root->children[1] = shared;
    Type pair_fields_type = {
        .kind = TYPE_LAYOUT,
        .layout_fields = (LayoutField[]){
            {.type = &string}, {.type = &string},
        },
        .layout_field_count = 2,
    };
    QttDropPlan *pair_plan =
        qtt_drop_plan_build(&pair_fields_type, &plan_error);
    execution = qtt_drop_execute(pair_plan, bad_root);
    assert(execution.error == QTT_DROP_DOUBLE);

    QttOwnedObject *legitimate = qtt_owned_object_new(6, 0);
    QttOwnedObject *shared_root = qtt_owned_object_new(5, 2);
    assert(legitimate && shared_root);
    assert(qtt_owned_object_retain(legitimate));
    shared_root->children[0] = legitimate;
    shared_root->children[1] = legitimate;
    execution = qtt_drop_execute(pair_plan, shared_root);
    assert(execution.error == QTT_DROP_VALID);
    assert(execution.reclaimed == 2);
    assert(legitimate->references == 0 && legitimate->dropped);
    assert(shared_root->dropped);

    qtt_owned_object_free_storage(root);
    qtt_owned_object_free_storage(name);
    qtt_owned_object_free_storage(bad_root);
    qtt_owned_object_free_storage(shared);
    qtt_owned_object_free_storage(shared_root);
    qtt_owned_object_free_storage(legitimate);
    qtt_drop_plan_free(pair_plan);
    qtt_drop_plan_free(plan);
    qtt_destructor_descriptor_free(equivalent_descriptor);
    qtt_destructor_descriptor_free(descriptor);

    Type node = {.kind = TYPE_LAYOUT};
    Type maybe_node = {.kind = TYPE_OPTIONAL, .element_type = &node};
    LayoutField node_fields[] = {
        {.name = "value", .type = &integer},
        {.name = "next", .type = &maybe_node},
    };
    node.layout_fields = node_fields;
    node.layout_field_count = 2;
    QttDropPlan *recursive = qtt_drop_plan_build(&node, &plan_error);
    assert(recursive && plan_error == QTT_DROP_PLAN_OK);
    assert(recursive->kind == QTT_DROP_STRUCT);
    assert(recursive->children[1]->kind == QTT_DROP_OPTIONAL);
    assert(recursive->children[1]->children[0]->kind == QTT_DROP_BACKREF);

    QttOwnedObject *third = qtt_owned_object_new(13, 2);
    QttOwnedObject *second = qtt_owned_object_new(12, 2);
    QttOwnedObject *first = qtt_owned_object_new(11, 2);
    assert(first && second && third);
    first->children[1] = second;
    second->children[1] = third;
    third->children[1] = NULL;
    execution = qtt_drop_execute(recursive, first);
    assert(execution.error == QTT_DROP_VALID);
    assert(execution.reclaimed == 3);
    assert(first->dropped && second->dropped && third->dropped);
    qtt_owned_object_free_storage(first);
    qtt_owned_object_free_storage(second);
    qtt_owned_object_free_storage(third);
    qtt_drop_plan_free(recursive);

    LayoutField inline_fields[] = {
        {.name = "name", .type = &string},
        {.name = "age", .type = &integer},
    };
    Type inline_record = {
        .kind = TYPE_LAYOUT, .layout_name = "InlineRecord",
        .layout_fields = inline_fields, .layout_field_count = 2,
        .layout_is_inline = true,
    };
    QttDropPlan *inline_plan =
        qtt_drop_plan_build(&inline_record, &plan_error);
    assert(inline_plan && inline_plan->kind == QTT_DROP_STRUCT);
    assert(inline_plan->children[0]->kind == QTT_DROP_OWNED_LEAF);
    assert(inline_plan->children[1]->kind == QTT_DROP_NOOP);
    qtt_drop_plan_free(inline_plan);

    Type string_list = {
        .kind = TYPE_LIST,
        .list_types = (Type *[]){&string},
        .list_count = 1,
    };
    QttDropPlan *list_plan =
        qtt_drop_plan_build(&string_list, &plan_error);
    assert(list_plan && list_plan->kind == QTT_DROP_SEQUENCE);
    QttOwnedObject *item_a = qtt_owned_object_new(22, 0);
    QttOwnedObject *item_b = qtt_owned_object_new(23, 0);
    QttOwnedObject *list = qtt_owned_object_new(21, 2);
    assert(item_a && item_b && list);
    list->children[0] = item_a;
    list->children[1] = item_b;
    execution = qtt_drop_execute(list_plan, list);
    assert(execution.error == QTT_DROP_VALID);
    assert(execution.reclaimed == 3);
    qtt_owned_object_free_storage(item_a);
    qtt_owned_object_free_storage(item_b);
    qtt_owned_object_free_storage(list);
    qtt_drop_plan_free(list_plan);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_drop_test.c"
            executable = directory / "qtt_drop_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "core.c"),
                    str(ROOT / "qtt" / "resource.c"),
                    str(ROOT / "qtt" / "type_identity.c"),
                    str(ROOT / "qtt" / "drop.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
