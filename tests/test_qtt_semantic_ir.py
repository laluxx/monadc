"""First verified Semantic-IR structural slice."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttSemanticIrTests(unittest.TestCase):
    def test_flat_ir_rejects_forged_structure_and_provenance(self):
        source = r'''
#include "qtt/semantic_closure.h"
#include <assert.h>
int main(void) {
    Type int_type = {.kind = TYPE_INT};
    QttCoreVar x = {.module_id = 4, .binder_id = 8};
    QttCoreNode use = {
        .kind = QTT_CORE_VAR, .type = &int_type, .var = x};
    QttCoreNode global = {
        .kind = QTT_CORE_GLOBAL, .type = &int_type,
        .global = {.name = "True"}};
    QttCoreNode *items[] = {&global, &use};
    QttCoreNode sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {.items = items, .count = 2}};
    QttSemanticIrError error = QTT_SEMANTIC_IR_OK;
    QttSemanticFunction *function =
        qtt_semantic_ir_lower(&sequence, &error);
    assert(function && error == QTT_SEMANTIC_IR_OK);
    assert(qtt_semantic_ir_node_count(function) == 3);
    assert(qtt_semantic_ir_evidence_count(function) == 2);
    assert(qtt_semantic_ir_solved_usage(function, x.binder_id).finite == 1);
    assert(qtt_semantic_ir_verify(function, &sequence) ==
           QTT_SEMANTIC_IR_VALID);
    qtt_semantic_ir_free(function);

    QttCoreEffectResult certified_effects = qtt_core_effect_elaborate(
        &sequence, qtt_quantity_finite(1));
    assert(certified_effects.status == QTT_CORE_EFFECT_OK);
    function = qtt_semantic_ir_lower_certified(
        &sequence, &certified_effects, &error);
    assert(function && error == QTT_SEMANTIC_IR_OK);
    const QttSemanticEffectJudgment *judgment =
        qtt_semantic_ir_effect_judgment(function);
    assert(judgment && judgment->present);
    assert(judgment->row_fingerprint == certified_effects.fingerprint);
    assert(judgment->constraint_fingerprint ==
           certified_effects.constraint_fingerprint);
    assert(qtt_semantic_ir_verify_effect_judgment(
        function, &sequence, &certified_effects));
    ((QttSemanticEffectJudgment *)judgment)->constraint_count++;
    assert(!qtt_semantic_ir_verify_effect_judgment(
        function, &sequence, &certified_effects));
    ((QttSemanticEffectJudgment *)judgment)->constraint_count--;
    certified_effects.constraint_fingerprint ^= 1;
    assert(!qtt_semantic_ir_verify_effect_judgment(
        function, &sequence, &certified_effects));
    certified_effects.constraint_fingerprint ^= 1;
    qtt_semantic_ir_free(function);
    qtt_core_effect_result_free(&certified_effects);

    function = qtt_semantic_ir_lower(&sequence, &error);
    assert(function && error == QTT_SEMANTIC_IR_OK);
    QttSemanticNode *nodes = qtt_semantic_ir_nodes(function);
    assert(nodes[0].representation == QTT_REP_IMMEDIATE);
    assert(nodes[0].capability == QTT_SEMANTIC_CAPABILITY_NONE);
    nodes[0].representation = QTT_REP_OWNED_HEAP;
    assert(qtt_semantic_ir_verify(function, &sequence) ==
           QTT_SEMANTIC_IR_REPRESENTATION_MISMATCH);
    nodes[0].representation = QTT_REP_IMMEDIATE;
    nodes[0].capability = QTT_SEMANTIC_CAPABILITY_OWNED;
    assert(qtt_semantic_ir_verify(function, &sequence) ==
           QTT_SEMANTIC_IR_CAPABILITY_MISMATCH);
    nodes[0].capability = QTT_SEMANTIC_CAPABILITY_NONE;
    nodes[0].subtree_size = 2;
    assert(qtt_semantic_ir_verify(function, &sequence) ==
           QTT_SEMANTIC_IR_SHAPE_MISMATCH);
    nodes[0].subtree_size = 3;
    nodes[2].var.binder_id = 9;
    assert(qtt_semantic_ir_verify(function, &sequence) ==
           QTT_SEMANTIC_IR_PROVENANCE_MISMATCH);
    nodes[2].var = x;
    nodes[1].kind = QTT_CORE_LITERAL;
    assert(qtt_semantic_ir_verify(function, &sequence) ==
           QTT_SEMANTIC_IR_KIND_MISMATCH);
    nodes[1].kind = QTT_CORE_GLOBAL;
    nodes[1].type_id.value = 0;
    assert(qtt_semantic_ir_verify(function, &sequence) ==
           QTT_SEMANTIC_IR_TYPE_MISMATCH);
    nodes[1].type_id = nodes[0].type_id;
    nodes[1].grade_role = QTT_SEMANTIC_GRADE_INVOCATION;
    assert(qtt_semantic_ir_verify(function, &sequence) ==
           QTT_SEMANTIC_IR_GRADE_ROLE_MISMATCH);
    nodes[1].grade_role = QTT_SEMANTIC_GRADE_RESULT_DEMAND;
    nodes[1].effects = NULL;
    assert(qtt_semantic_ir_verify(function, &sequence) ==
           QTT_SEMANTIC_IR_EFFECT_MISMATCH);
    nodes[1].effects = nodes[0].effects;
    QttSemanticGradeEvidence *evidence =
        qtt_semantic_ir_evidence(function);
    assert(evidence[0].module_id == x.module_id);
    evidence[0].role = QTT_SEMANTIC_GRADE_INVOCATION;
    assert(qtt_semantic_ir_verify(function, &sequence) ==
           QTT_SEMANTIC_IR_GRADE_EVIDENCE_MISMATCH);
    evidence[0].role = QTT_SEMANTIC_GRADE_RESULT_DEMAND;
    evidence[0].module_id = 99;
    assert(qtt_semantic_ir_verify(function, &sequence) ==
           QTT_SEMANTIC_IR_GRADE_EVIDENCE_MISMATCH);
    qtt_semantic_ir_free(function);

    QttCoreNode parameter = {
        .kind = QTT_CORE_VAR, .type = &int_type, .var = x};
    QttCoreNode lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {.params = &x, .param_count = 1, .body = &parameter}};
    QttCoreNode argument = {
        .kind = QTT_CORE_GLOBAL, .type = &int_type,
        .global = {.name = "True"}};
    QttCoreNode *arguments[] = {&argument};
    QttCoreNode call = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &lambda, .arguments = arguments,
                  .argument_count = 1}};
    function = qtt_semantic_ir_lower(&call, &error);
    assert(function && qtt_semantic_ir_verify(function, &call) ==
           QTT_SEMANTIC_IR_VALID);
    nodes = qtt_semantic_ir_nodes(function);
    assert(nodes[0].instance_id && nodes[0].callee_closure_id);
    assert(nodes[0].domain_count == 1);
    assert(nodes[1].closure_id == nodes[0].callee_closure_id);
    assert(nodes[1].closure_policy.storage ==
           QTT_CLOSURE_STORAGE_UNIQUE);
    assert(nodes[1].closure_policy.reason ==
           QTT_CLOSURE_POLICY_SINGLE_DIRECT_CALL);
    nodes[1].closure_policy.storage = QTT_CLOSURE_STORAGE_SHARED;
    assert(qtt_semantic_ir_verify(function, &call) ==
           QTT_SEMANTIC_IR_CLOSURE_POLICY_MISMATCH);
    nodes[1].closure_policy.storage = QTT_CLOSURE_STORAGE_UNIQUE;
    assert(qtt_semantic_ir_verify(function, &call) ==
           QTT_SEMANTIC_IR_VALID);
    nodes[0].domain_count = 2;
    assert(qtt_semantic_ir_verify(function, &call) ==
           QTT_SEMANTIC_IR_CALL_EVIDENCE_MISMATCH);
    qtt_semantic_ir_free(function);

    Type string_type = {.kind = TYPE_STRING};
    QttCoreVar captured = {.module_id = 4, .binder_id = 90};
    QttCoreNode capture_use = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = captured};
    QttCoreNode capturing_lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &string_type,
        .lambda = {.body = &capture_use}};
    QttCoreNode capturing_call = {
        .kind = QTT_CORE_APPLY, .type = &string_type,
        .apply = {.callee = &capturing_lambda}};
    function = qtt_semantic_ir_lower(&capturing_call, &error);
    assert(function && error == QTT_SEMANTIC_IR_OK);
    assert(qtt_semantic_ir_closure_field_count(function) == 1);
    QttSemanticClosureFieldEvidence *closure_fields =
        qtt_semantic_ir_closure_fields(function);
    assert(closure_fields[0].node_index == 1);
    assert(closure_fields[0].closure_id == 2);
    assert(closure_fields[0].ordinal == 0);
    assert(qtt_core_var_equal(closure_fields[0].source, captured));
    assert(closure_fields[0].type_id.value);
    assert(closure_fields[0].representation == QTT_REP_OWNED_HEAP);
    assert(closure_fields[0].storage == QTT_CLOSURE_STORAGE_UNIQUE);
    assert(closure_fields[0].exit == QTT_CLOSURE_FIELD_MOVE_OUT);
    assert(qtt_destructor_descriptor_verify(
        closure_fields[0].descriptor, &string_type));
    assert(qtt_semantic_ir_verify(function, &capturing_call) ==
           QTT_SEMANTIC_IR_VALID);
    QttClosureError closure_error = QTT_CLOSURE_OK;
    QttClosurePolicyEvidence closure_policy = {0};
    QttClosurePlan *closure_plan = qtt_closure_plan_infer(
        &capturing_call, &capturing_lambda, 400, 900,
        &closure_policy, &closure_error);
    assert(closure_plan && closure_error == QTT_CLOSURE_OK);
    assert(qtt_semantic_closure_verify_plan(
        function, &capturing_lambda, closure_plan) ==
        QTT_SEMANTIC_CLOSURE_OK);
    closure_plan->captures[0].exit = QTT_CLOSURE_FIELD_RELEASE;
    assert(qtt_semantic_closure_verify_plan(
        function, &capturing_lambda, closure_plan) ==
        QTT_SEMANTIC_CLOSURE_FIELD_MISMATCH);
    closure_plan->captures[0].exit = QTT_CLOSURE_FIELD_MOVE_OUT;
    qtt_closure_plan_free(closure_plan);
    closure_fields[0].ordinal = 1;
    assert(qtt_semantic_ir_verify(function, &capturing_call) ==
           QTT_SEMANTIC_IR_CLOSURE_FIELD_MISMATCH);
    closure_fields[0].ordinal = 0;
    assert(qtt_semantic_ir_verify(function, &capturing_call) ==
           QTT_SEMANTIC_IR_VALID);
    closure_fields[0].descriptor->id.plan_fingerprint ^= 1;
    assert(qtt_semantic_ir_verify(function, &capturing_call) ==
           QTT_SEMANTIC_IR_CLOSURE_FIELD_MISMATCH);
    closure_fields[0].descriptor->id.plan_fingerprint ^= 1;
    qtt_semantic_ir_free(function);

    QttCoreNode consume_global = {
        .kind = QTT_CORE_GLOBAL, .type = &int_type,
        .global = {.name = "consumeString"}};
    QttCoreNode *consume_arguments[] = {&capture_use};
    QttCoreNode consume_call = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &consume_global,
                  .arguments = consume_arguments,
                  .argument_count = 1}};
    QttCoreNode consuming_lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {.body = &consume_call}};
    QttCoreNode consuming_invoke = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {.callee = &consuming_lambda}};
    QttTypeArena *call_types = qtt_type_arena_new();
    QttSignatureEnv *call_env =
        qtt_signature_env_new(call_types);
    assert(call_types && call_env);
    QttTypeIdentityError call_type_error =
        QTT_TYPE_IDENTITY_OK;
    QttParameterContract consume_parameter = {
        .var = {.module_id = 4, .binder_id = 91},
        .quantity = qtt_quantity_finite(1),
        .observed = qtt_quantity_finite(1),
        .mode = QTT_OWNERSHIP_CONSUMED,
        .type = &string_type,
        .type_id = qtt_type_intern(
            call_types, &string_type, &call_type_error),
        .representation = QTT_REP_OWNED_HEAP,
    };
    QttFunctionSignature consume_signature = {
        .parameters = &consume_parameter,
        .parameter_count = 1,
        .result_type = &int_type,
        .result = {
            .mode = QTT_RESULT_IMMEDIATE,
            .type = &int_type,
            .type_id = qtt_type_intern(
                call_types, &int_type, &call_type_error),
            .representation = QTT_REP_IMMEDIATE,
        },
    };
    consume_signature.contract_fingerprint =
        qtt_signature_contract_fingerprint(&consume_signature);
    assert(qtt_signature_env_register(
               call_env, 4, "consumeString",
               &consume_signature, NULL) == QTT_SIGNATURE_ENV_OK);
    function = qtt_semantic_ir_lower_in_env(
        &consuming_invoke, call_env, 4, &error);
    assert(function && error == QTT_SEMANTIC_IR_OK);
    closure_plan = qtt_closure_plan_infer_in_env(
        &consuming_invoke, &consuming_lambda,
        call_env, 4, 400, 950,
        &closure_policy, &closure_error);
    assert(closure_plan && closure_plan->captures[0].exit ==
           QTT_CLOSURE_FIELD_MOVE_OUT);
    assert(qtt_semantic_closure_verify_plan(
        function, &consuming_lambda, closure_plan) ==
        QTT_SEMANTIC_CLOSURE_OK);
    closure_plan->captures[0].exit =
        QTT_CLOSURE_FIELD_RELEASE;
    assert(qtt_semantic_closure_verify_plan(
        function, &consuming_lambda, closure_plan) ==
        QTT_SEMANTIC_CLOSURE_FIELD_MISMATCH);
    qtt_closure_plan_free(closure_plan);
    qtt_semantic_ir_free(function);

    QttCoreVar effect_state = {.module_id = 4, .binder_id = 92};
    QttCoreNode effect_value = {
        .kind = QTT_CORE_LITERAL, .type = &int_type};
    QttCoreNode effect_write = {
        .kind = QTT_CORE_WRITE, .type = &int_type,
        .write = {
            .place = {.root = effect_state},
            .value = &effect_value,
        },
    };
    const Type *effect_parameter_type = &int_type;
    QttCoreNode named_effect_lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {
            .params = &effect_state,
            .param_types = &effect_parameter_type,
            .param_count = 1,
            .body = &effect_write,
        },
    };
    QttSignatureError effect_signature_error = QTT_SIGNATURE_OK;
    QttFunctionSignature *effect_signature = qtt_signature_derive(
        &named_effect_lambda, &effect_signature_error);
    assert(effect_signature &&
           effect_signature_error == QTT_SIGNATURE_OK);
    assert(qtt_signature_canonicalize(effect_signature, call_types) ==
           QTT_SIGNATURE_CANONICAL);
    assert(qtt_signature_env_register(
               call_env, 4, "writeState", effect_signature, NULL) ==
           QTT_SIGNATURE_ENV_OK);
    QttCoreNode effect_global = {
        .kind = QTT_CORE_GLOBAL, .type = &int_type,
        .global = {.name = "writeState"}};
    QttCoreNode named_effect_argument = {
        .kind = QTT_CORE_LITERAL, .type = &int_type};
    QttCoreNode *named_effect_arguments[] = {&named_effect_argument};
    QttCoreNode effect_call = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {
            .callee = &effect_global,
            .arguments = named_effect_arguments,
            .argument_count = 1,
        },
    };
    function = qtt_semantic_ir_lower_in_env(
        &effect_call, call_env, 4, &error);
    assert(function && error == QTT_SEMANTIC_IR_OK);
    assert(qtt_semantic_ir_effect_label_count(
               function, 0, "write:4:92:0") == 1);
    assert(qtt_semantic_ir_verify(function, &effect_call) ==
           QTT_SEMANTIC_IR_VALID);
    qtt_semantic_ir_free(function);
    qtt_signature_env_free(call_env);
    qtt_signature_free(effect_signature);
    qtt_type_arena_free(call_types);

    QttCoreNode heap_value = {
        .kind = QTT_CORE_GLOBAL, .type = &string_type,
        .global = {.name = "heap-value"}};
    function = qtt_semantic_ir_lower(&heap_value, &error);
    assert(function);
    nodes = qtt_semantic_ir_nodes(function);
    assert(nodes[0].representation == QTT_REP_OWNED_HEAP);
    /* Storage class alone is deliberately not an ownership proof. */
    assert(nodes[0].capability == QTT_SEMANTIC_CAPABILITY_UNKNOWN);
    qtt_semantic_ir_free(function);

    QttCoreVar heap_var = {.module_id = 4, .binder_id = 50};
    QttCoreNode heap_initializer = {
        .kind = QTT_CORE_LITERAL, .type = &string_type};
    QttCoreNode heap_replacement = {
        .kind = QTT_CORE_LITERAL, .type = &string_type};
    QttCoreNode heap_write = {
        .kind = QTT_CORE_WRITE, .type = &string_type,
        .write = {
            .place = {.root = heap_var},
            .value = &heap_replacement,
        },
    };
    QttCoreNode heap_replace_let = {
        .kind = QTT_CORE_LET, .type = &string_type,
        .let = {
            .binding = heap_var,
            .value = &heap_initializer,
            .body = &heap_write,
        },
    };
    function = qtt_semantic_ir_lower(&heap_replace_let, &error);
    assert(function && error == QTT_SEMANTIC_IR_OK);
    assert(qtt_semantic_ir_verify(function, &heap_replace_let) ==
           QTT_SEMANTIC_IR_VALID);
    QttResourceBlock *replace_resources =
        qtt_semantic_ir_resources(function);
    assert(replace_resources && replace_resources->count == 3);
    assert(replace_resources->ops[1].kind == QTT_RESOURCE_REPLACE);
    QttSemanticCapabilityTransition *replace_transitions =
        qtt_semantic_ir_transitions(function);
    assert(qtt_semantic_ir_transition_count(function) == 3);
    assert(replace_transitions[1].kind == QTT_RESOURCE_REPLACE);
    assert(replace_transitions[1].before ==
           QTT_SEMANTIC_CAPABILITY_OWNED);
    assert(replace_transitions[1].after ==
           QTT_SEMANTIC_CAPABILITY_OWNED);
    replace_transitions[1].after = QTT_SEMANTIC_CAPABILITY_NONE;
    assert(qtt_semantic_ir_verify(function, &heap_replace_let) ==
           QTT_SEMANTIC_IR_CAPABILITY_TRANSITION_MISMATCH);
    qtt_semantic_ir_free(function);

    QttCoreNode replacement = {
        .kind = QTT_CORE_GLOBAL, .type = &int_type,
        .global = {.name = "replacement"}};
    QttCoreNode write = {
        .kind = QTT_CORE_WRITE, .type = &int_type,
        .write = {
            .place = {
                .root = {.module_id = 4, .binder_id = 60},
                .projection_id = 0,
            },
            .value = &replacement,
        },
    };
    function = qtt_semantic_ir_lower(&write, &error);
    assert(function);
    nodes = qtt_semantic_ir_nodes(function);
    assert(qtt_place_equal(nodes[0].place, write.write.place));
    assert(qtt_semantic_ir_effect_label_count(
               function, 0, "write:4:60:0") == 1);
    nodes[0].place.projection_id = 99;
    assert(qtt_semantic_ir_verify(function, &write) ==
           QTT_SEMANTIC_IR_PROVENANCE_MISMATCH);
    qtt_semantic_ir_free(function);

    QttCoreNode after_write = {
        .kind = QTT_CORE_GLOBAL, .type = &int_type,
        .global = {.name = "after-write"}};
    QttCoreNode *effect_items[] = {&write, &after_write};
    QttCoreNode effect_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {.items = effect_items, .count = 2}};
    function = qtt_semantic_ir_lower(&effect_sequence, &error);
    assert(function);
    assert(qtt_semantic_ir_effect_label_count(
               function, 0, "write:4:60:0") == 1);
    assert(qtt_semantic_ir_verify(function, &effect_sequence) ==
           QTT_SEMANTIC_IR_VALID);
    qtt_semantic_ir_free(function);

    QttCoreVar state_param = {.module_id = 4, .binder_id = 61};
    const Type *state_param_types[] = {&int_type};
    QttCoreNode latent_write = {
        .kind = QTT_CORE_WRITE, .type = &int_type,
        .write = {
            .place = {.root = state_param},
            .value = &replacement,
        },
    };
    QttCoreNode effect_lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &int_type,
        .lambda = {
            .params = &state_param,
            .param_types = state_param_types,
            .param_count = 1,
            .body = &latent_write,
        },
    };
    QttCoreNode effect_argument = {
        .kind = QTT_CORE_LITERAL, .type = &int_type};
    QttCoreNode *effect_arguments[] = {&effect_argument};
    QttCoreNode effect_apply = {
        .kind = QTT_CORE_APPLY, .type = &int_type,
        .apply = {
            .callee = &effect_lambda,
            .arguments = effect_arguments,
            .argument_count = 1,
        },
    };
    function = qtt_semantic_ir_lower(&effect_apply, &error);
    assert(function);
    assert(qtt_semantic_ir_effect_label_count(
               function, 0, "write:4:61:0") == 1);
    assert(qtt_semantic_ir_effect_label_count(
               function, 1, "write:4:61:0") == 0);
    assert(qtt_semantic_ir_effect_label_count(
               function, 2, "write:4:61:0") == 1);
    assert(qtt_semantic_ir_verify(function, &effect_apply) ==
           QTT_SEMANTIC_IR_VALID);
    qtt_semantic_ir_free(function);

    QttCoreNode heap_body = {
        .kind = QTT_CORE_GLOBAL, .type = &string_type,
        .global = {.name = "result"}};
    QttCoreNode heap_let = {
        .kind = QTT_CORE_LET, .type = &string_type,
        .let = {.binding = heap_var, .value = &heap_initializer,
                .body = &heap_body}};
    function = qtt_semantic_ir_lower(&heap_let, &error);
    assert(function && qtt_semantic_ir_verify(function, &heap_let) ==
           QTT_SEMANTIC_IR_VALID);
    assert(qtt_semantic_ir_resource_status(function) ==
           QTT_RESOURCE_ELABORATE_OK);
    QttResourceBlock *resources =
        qtt_semantic_ir_resources(function);
    assert(resources && resources->count == 2);
    assert(resources->ops[0].kind == QTT_RESOURCE_ALLOC);
    assert(resources->ops[1].kind == QTT_RESOURCE_DROP);
    assert(qtt_semantic_ir_destructor_count(function) == 1);
    QttSemanticDestructorEvidence *destructors =
        qtt_semantic_ir_destructors(function);
    assert(qtt_core_var_equal(destructors[0].var, heap_var));
    assert(qtt_destructor_descriptor_verify(
        destructors[0].descriptor, &string_type));
    assert(destructors[0].mask);
    assert(destructors[0].mask->evacuated_count == 0);
    assert(qtt_place_equal(
        destructors[0].mask->root, qtt_place_root(heap_var)));
    assert(qtt_drop_mask_verify(
        destructors[0].descriptor, destructors[0].mask) ==
        QTT_DROP_MASK_VALID);
    assert(qtt_semantic_ir_transition_count(function) == 2);
    QttSemanticCapabilityTransition *transitions =
        qtt_semantic_ir_transitions(function);
    assert(transitions[0].kind == QTT_RESOURCE_ALLOC);
    assert(transitions[0].before ==
           QTT_SEMANTIC_CAPABILITY_NONE);
    assert(transitions[0].after ==
           QTT_SEMANTIC_CAPABILITY_OWNED);
    assert(transitions[1].kind == QTT_RESOURCE_DROP);
    assert(transitions[1].before ==
           QTT_SEMANTIC_CAPABILITY_OWNED);
    assert(transitions[1].after ==
           QTT_SEMANTIC_CAPABILITY_NONE);
    assert(qtt_place_equal(
        transitions[1].place, resources->ops[1].place));
    transitions[1].place =
        qtt_place_project(qtt_place_root(heap_var), 77);
    assert(qtt_semantic_ir_verify(function, &heap_let) ==
           QTT_SEMANTIC_IR_CAPABILITY_TRANSITION_MISMATCH);
    transitions[1].place = resources->ops[1].place;
    transitions[1].after = QTT_SEMANTIC_CAPABILITY_OWNED;
    assert(qtt_semantic_ir_verify(function, &heap_let) ==
           QTT_SEMANTIC_IR_CAPABILITY_TRANSITION_MISMATCH);
    transitions[1].after = QTT_SEMANTIC_CAPABILITY_NONE;
    destructors[0].descriptor->id.plan_fingerprint ^= 1;
    assert(qtt_semantic_ir_verify(function, &heap_let) ==
           QTT_SEMANTIC_IR_DESTRUCTOR_EVIDENCE_MISMATCH);
    destructors[0].descriptor->id.plan_fingerprint ^= 1;
    destructors[0].mask->root.projection_id = 1;
    assert(qtt_semantic_ir_verify(function, &heap_let) ==
           QTT_SEMANTIC_IR_DESTRUCTOR_EVIDENCE_MISMATCH);
    destructors[0].mask->root.projection_id = 0;
    resources->ops[1] = qtt_resource_move(heap_var);
    assert(qtt_resource_verify(resources, NULL, 0).error ==
           QTT_RESOURCE_VALID);
    assert(qtt_semantic_ir_verify(function, &heap_let) ==
           QTT_SEMANTIC_IR_RESOURCE_EVIDENCE_MISMATCH);
    qtt_semantic_ir_free(function);

    LayoutField owner_fields[] = {
        {.name = "name", .type = &string_type},
    };
    Type owner_type = {
        .kind = TYPE_LAYOUT,
        .layout_fields = owner_fields,
        .layout_field_count = 1,
    };
    QttCoreVar owner_var = {.module_id = 1, .binder_id = 90};
    QttCoreNode owner_initializer = {
        .kind = QTT_CORE_LITERAL, .type = &owner_type};
    QttPlace owner_name = {0};
    assert(qtt_place_layout_field(
        qtt_place_root(owner_var), &owner_type, "name", &owner_name));
    QttCoreNode owner_projection = {
        .kind = QTT_CORE_PLACE, .type = &string_type,
        .place = owner_name,
    };
    QttCoreNode projected_owner = {
        .kind = QTT_CORE_LET, .type = &string_type,
        .let = {
            .binding = owner_var,
            .value = &owner_initializer,
            .body = &owner_projection,
        },
    };
    function = qtt_semantic_ir_lower(&projected_owner, &error);
    assert(function);
    QttSemanticIrValidation projected_validation =
        qtt_semantic_ir_verify(function, &projected_owner);
    assert(projected_validation == QTT_SEMANTIC_IR_VALID);
    resources = qtt_semantic_ir_resources(function);
    assert(resources && resources->count == 3);
    assert(resources->ops[1].kind == QTT_RESOURCE_MOVE_PLACE);
    destructors = qtt_semantic_ir_destructors(function);
    assert(destructors && destructors[0].mask);
    assert(destructors[0].mask->evacuated_count == 1);
    assert(qtt_place_equal(
        destructors[0].mask->evacuated[0], owner_name));
    qtt_semantic_ir_free(function);

    AST with_head = {.type = AST_SYMBOL, .symbol = "with"};
    AST source_name = {
        .type = AST_SYMBOL, .symbol = "source_owner",
        .resolved_binder_id = 91, .inferred_type = &owner_type};
    AST source_initializer = {
        .type = AST_STRING, .string = "Grace",
        .inferred_type = &owner_type};
    AST *source_binding_items[] = {
        &source_name, &source_initializer};
    AST source_bindings = {.type = AST_ARRAY};
    source_bindings.array.elements = source_binding_items;
    source_bindings.array.element_count = 2;
    AST source_projection = {
        .type = AST_SYMBOL, .symbol = "source_owner.name",
        .resolved_binder_id = 91, .inferred_type = &string_type};
    AST *source_items[] = {
        &with_head, &source_bindings, &source_projection};
    AST projected_source = {.type = AST_LIST};
    projected_source.list.items = source_items;
    projected_source.list.count = 3;
    projected_source.inferred_type = &string_type;
    QttCoreError core_error = QTT_CORE_OK;
    QttCoreNode *source_core = qtt_core_lower(
        &projected_source, 1, &core_error);
    assert(source_core && core_error == QTT_CORE_OK);
    function = qtt_semantic_ir_lower(source_core, &error);
    assert(function && qtt_semantic_ir_verify(
        function, source_core) == QTT_SEMANTIC_IR_VALID);
    resources = qtt_semantic_ir_resources(function);
    assert(resources && resources->count == 3);
    assert(resources->ops[1].kind == QTT_RESOURCE_MOVE_PLACE);
    destructors = qtt_semantic_ir_destructors(function);
    assert(destructors && destructors[0].mask &&
           destructors[0].mask->evacuated_count == 1);
    qtt_semantic_ir_free(function);
    qtt_core_free(source_core);

    QttCoreNode operation_argument = {
        .kind = QTT_CORE_GLOBAL, .type = &int_type,
        .global = {.name = "payload"}};
    QttCoreNode operation = {
        .kind = QTT_CORE_PERFORM, .type = &int_type,
        .perform = {
            .effect_name = "Core.Exception.raise",
            .capability_id = 44, .constructor_id = 9,
            .argument = &operation_argument}};
    QttCoreNode handler_clause = {
        .kind = QTT_CORE_GLOBAL, .type = &int_type,
        .global = {.name = "clause"}};
    QttCoreNode handler = {
        .kind = QTT_CORE_HANDLE, .type = &int_type,
        .handle = {
            .profile_name = "Core.Exception.abort",
            .capability_id = 45,
            .computation = &operation, .clause = &handler_clause}};
    function = qtt_semantic_ir_lower(&handler, &error);
    assert(function && error == QTT_SEMANTIC_IR_OK);
    assert(qtt_semantic_ir_node_count(function) == 4);
    assert(qtt_semantic_ir_nodes(function)[1].kind == QTT_CORE_PERFORM);
    assert(qtt_semantic_ir_nodes(function)[1].effect_constructor_id == 9);
    assert(qtt_semantic_ir_nodes(function)[1].effect_capability_id == 44);
    char *effect_trace = qtt_semantic_ir_effect_trace_format(function);
    assert(effect_trace);
    assert(strcmp(effect_trace,
        "monad-effect-trace-v1\n"
        "handle Core.Exception.abort capability=45\n"
        "perform Core.Exception.raise constructor=9 capability=44\n") == 0);
    free(effect_trace);
    assert(qtt_semantic_ir_verify(
        function, &handler) == QTT_SEMANTIC_IR_VALID);
    qtt_semantic_ir_nodes(function)[1].effect_capability_id++;
    assert(qtt_semantic_ir_verify(function, &handler) ==
           QTT_SEMANTIC_IR_EFFECT_AUTHORITY_MISMATCH);
    qtt_semantic_ir_free(function);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "semantic_ir.c"
            executable = directory / "semantic_ir"
            harness.write_text(source)
            subprocess.run([
                "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                "-iquote", str(ROOT), str(harness),
                str(ROOT / "qtt" / "core.c"),
                str(ROOT / "qtt" / "core_effect.c"),
                str(ROOT / "qtt" / "quantity.c"),
                str(ROOT / "qtt" / "demand.c"),
                str(ROOT / "qtt" / "graded.c"),
                str(ROOT / "qtt" / "environment.c"),
                str(ROOT / "qtt" / "constraints.c"),
                str(ROOT / "qtt" / "elaboration.c"),
                str(ROOT / "qtt" / "core_usage.c"),
                str(ROOT / "qtt" / "type_identity.c"),
                str(ROOT / "qtt" / "signature.c"),
                str(ROOT / "qtt" / "signature_env.c"),
                str(ROOT / "qtt" / "call.c"),
                str(ROOT / "effects" / "effect.c"),
                str(ROOT / "effects" / "constraints.c"),
                str(ROOT / "qtt" / "place.c"),
                str(ROOT / "qtt" / "resource.c"),
                str(ROOT / "qtt" / "drop.c"),
                str(ROOT / "qtt" / "closure_policy.c"),
                str(ROOT / "qtt" / "closure.c"),
                str(ROOT / "qtt" / "semantic_ir.c"),
                str(ROOT / "qtt" / "semantic_closure.c"),
                "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
