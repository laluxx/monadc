"""Inferred effect rows normalize, unify, handle duplicates, and instantiate."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttEffectTests(unittest.TestCase):
    def test_open_row_unification_handling_and_polymorphism(self):
        source = r'''
#include "effects/effect.h"
#include "qtt/place.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    QttEffectArena *arena = qtt_effect_arena_new();
    QttEffectRow *mu = qtt_effect_fresh(arena);
    QttEffectRow *left = qtt_effect_extend(
        arena, "exn:Parse", qtt_effect_extend(arena, "io", mu));
    QttEffectRow *right = qtt_effect_extend(
        arena, "state:world",
        qtt_effect_extend(arena, "io",
            qtt_effect_extend(arena, "exn:Parse",
                qtt_effect_empty(arena))));
    QttEffectSolver *solver = qtt_effect_solver_new(arena);
    assert(arena && mu && left && right && solver);
    assert(qtt_effect_unify(solver, left, right) == QTT_EFFECT_UNIFY_OK);
    assert(qtt_effect_is_closed(solver, left));
    assert(qtt_effect_label_count(solver, left, "io") == 1);
    assert(qtt_effect_label_count(
               solver, left, "state:world") == 1);
    assert(qtt_effect_rows_equal(solver, left, right));

    QttPlace root = qtt_place_root(
        (QttCoreVar){.module_id = 7, .binder_id = 11});
    QttPlace field_a = qtt_place_project(root, 101);
    QttPlace field_b = qtt_place_project(root, 102);
    assert(qtt_place_overlaps(root, field_a));
    assert(qtt_place_overlaps(field_a, field_a));
    assert(!qtt_place_overlaps(field_a, field_b));
    LayoutField layout_fields[] = {
        {.name = "left", .type = NULL},
        {.name = "right", .type = NULL},
    };
    Type layout = {
        .kind = TYPE_LAYOUT,
        .layout_fields = layout_fields,
        .layout_field_count = 2,
    };
    QttPlace typed_left = {0};
    QttPlace typed_right = {0};
    assert(qtt_place_layout_field(
        root, &layout, "left", &typed_left));
    assert(qtt_place_layout_field(
        root, &layout, "right", &typed_right));
    assert(typed_left.projection_id == 1);
    assert(typed_right.projection_id == 2);
    assert(!qtt_place_overlaps(typed_left, typed_right));
    assert(!qtt_place_layout_field(
        root, &layout, "missing", &typed_left));
    LayoutField transform_fields[] = {
        {.name = "position", .type = NULL},
        {.name = "rotation", .type = NULL},
    };
    Type transform_layout = {
        .kind = TYPE_LAYOUT,
        .layout_fields = transform_fields,
        .layout_field_count = 2,
    };
    QttPlace position = {0};
    QttPlace rotation = {0};
    assert(qtt_place_layout_field(
        typed_left, &transform_layout, "position", &position));
    assert(qtt_place_layout_field(
        typed_left, &transform_layout, "rotation", &rotation));
    assert(position.projection_depth == 2);
    assert(position.projection_path[0] == 1);
    assert(position.projection_path[1] == 1);
    assert(qtt_place_overlaps(typed_left, position));
    assert(!qtt_place_overlaps(position, rotation));
    assert(!qtt_place_overlaps(typed_right, position));
    char nested_label[96];
    assert(qtt_place_effect_label(
        nested_label, sizeof(nested_label), "write", position));
    assert(strcmp(nested_label, "write:7:11:1.1") == 0);
    QttEffectRow *placed = qtt_effect_write_place(
        arena, field_a,
        qtt_effect_read_place(
            arena, field_b, qtt_effect_empty(arena)));
    assert(placed);
    assert(qtt_effect_label_count(
               solver, placed, "write:7:11:101") == 1);
    assert(qtt_effect_label_count(
               solver, placed, "read:7:11:102") == 1);

    QttEffectRow *twice = qtt_effect_extend(
        arena, "exn:E",
        qtt_effect_extend(arena, "exn:E",
            qtt_effect_empty(arena)));
    QttEffectRow *once = qtt_effect_handle_one(
        arena, solver, twice, "exn:E");
    assert(once);
    assert(qtt_effect_label_count(solver, twice, "exn:E") == 2);
    assert(qtt_effect_label_count(solver, once, "exn:E") == 1);
    assert(!qtt_effect_handle_one(arena, solver, once, "io"));

    QttEffectRow *alpha = qtt_effect_fresh(arena);
    QttEffectRow *polymorphic =
        qtt_effect_extend(arena, "io", alpha);
    QttEffectScheme *scheme =
        qtt_effect_generalize(arena, solver, polymorphic);
    assert(scheme && qtt_effect_scheme_quantified_count(scheme) == 1);
    QttEffectRow *first = qtt_effect_instantiate(arena, scheme);
    QttEffectRow *second = qtt_effect_instantiate(arena, scheme);
    assert(first && second);
    assert(qtt_effect_tail_variable(first) !=
           qtt_effect_tail_variable(second));
    QttEffectRow *replacement = qtt_effect_extend(
        arena, "network", qtt_effect_empty(arena));
    QttEffectRow *specialized = qtt_effect_instantiate_with_tail(
        arena, scheme, replacement);
    assert(specialized && qtt_effect_is_closed(solver, specialized));
    assert(qtt_effect_label_count(solver, specialized, "io") == 1);
    assert(qtt_effect_label_count(solver, specialized, "network") == 1);
    QttEffectScheme *depth_one = qtt_effect_generalize(
        arena, solver, polymorphic);
    QttEffectScheme *depth_two = qtt_effect_generalize(
        arena, solver, polymorphic);
    size_t callable_index[] = {3};
    size_t arity_one[] = {1};
    size_t arity_two[] = {2};
    assert(depth_one && depth_two);
    assert(qtt_effect_scheme_set_callable_parameter_contracts(
        depth_one, callable_index, arity_one, 1));
    assert(qtt_effect_scheme_set_callable_parameter_contracts(
        depth_two, callable_index, arity_two, 1));
    assert(qtt_effect_scheme_callable_parameter_arity(depth_one, 0) == 1);
    assert(qtt_effect_scheme_callable_parameter_arity(depth_two, 0) == 2);
    assert(qtt_effect_scheme_fingerprint(depth_one) !=
           qtt_effect_scheme_fingerprint(depth_two));
    char *portable_depth = qtt_effect_scheme_serialize(depth_two);
    assert(portable_depth && strstr(
        portable_depth, "monad-effect-scheme-v1|") == portable_depth);
    QttEffectScheme *restored_depth =
        qtt_effect_scheme_deserialize(portable_depth);
    assert(restored_depth);
    assert(qtt_effect_scheme_fingerprint(restored_depth) ==
           qtt_effect_scheme_fingerprint(depth_two));
    assert(qtt_effect_scheme_callable_parameter_count(restored_depth) == 1);
    assert(qtt_effect_scheme_callable_parameter_index(restored_depth, 0) == 3);
    assert(qtt_effect_scheme_callable_parameter_arity(restored_depth, 0) == 2);
    portable_depth[strlen(portable_depth) - 1] =
        portable_depth[strlen(portable_depth) - 1] == '2' ? '3' : '2';
    assert(!qtt_effect_scheme_deserialize(portable_depth));
    free(portable_depth);
    qtt_effect_scheme_free(restored_depth);
    qtt_effect_scheme_free(depth_one);
    qtt_effect_scheme_free(depth_two);

    /* Open composition preserves the semilattice of known atoms and
     * existentially abstracts the unknown remainder. */
    QttEffectRow *open_io = qtt_effect_extend(
        arena, "io", qtt_effect_fresh(arena));
    QttEffectRow *open_state = qtt_effect_extend(
        arena, "state.write", qtt_effect_fresh(arena));
    QttEffectRow *open_join = qtt_effect_join(
        arena, solver, open_io, open_state);
    assert(open_join && !qtt_effect_is_closed(solver, open_join));
    assert(qtt_effect_label_count(solver, open_join, "io") == 1);
    assert(qtt_effect_label_count(
               solver, open_join, "state.write") == 1);
    assert(qtt_effect_tail_variable(open_join) != 0);
    QttEffectRow *open_idempotent = qtt_effect_join(
        arena, solver, open_io, open_io);
    assert(open_idempotent &&
           qtt_effect_label_count(solver, open_idempotent, "io") == 1);
    assert(qtt_effect_tail_variable(open_idempotent) ==
           qtt_effect_tail_variable(open_io));
    QttEffectRow *open_with_closed = qtt_effect_join(
        arena, solver, open_io, once);
    assert(open_with_closed &&
           qtt_effect_tail_variable(open_with_closed) ==
               qtt_effect_tail_variable(open_io));
    QttEffectRow *closed_join_via_general = qtt_effect_join(
        arena, solver, twice, once);
    assert(closed_join_via_general &&
           qtt_effect_is_closed(solver, closed_join_via_general));
    assert(qtt_effect_label_count(
               solver, closed_join_via_general, "exn:E") == 2);

    QttEffectSolver *occurs = qtt_effect_solver_new(arena);
    QttEffectRow *rho = qtt_effect_fresh(arena);
    QttEffectRow *recursive =
        qtt_effect_extend(arena, "io", rho);
    assert(qtt_effect_unify(occurs, rho, recursive) ==
           QTT_EFFECT_UNIFY_OCCURS);

    char *formatted = qtt_effect_format(solver, right);
    assert(formatted);
    assert(strcmp(formatted, "<exn:Parse,io,state:world>") == 0);
    free(formatted);

    QttEffectAtom state_a = {
        .traits = "state,read",
        .kind = QTT_EFFECT_STATE,
        .constructor_id = 0x5354415445,
        .type_id = 17,
        .capability_id = 44,
        .operation = "get",
        .resumption = qtt_quantity_finite(1),
    };
    qtt_effect_trait_implications_clear();
    assert(qtt_effect_trait_implication_register("state", "io"));
    QttEffectAtom state_a_copy = state_a;
    state_a_copy.traits = "read,state,read";
    QttEffectAtom state_b = state_a;
    state_b.capability_id = 45;
    assert(qtt_effect_atom_equal(&state_a, &state_a_copy));
    assert(!qtt_effect_atom_equal(&state_a, &state_b));
    assert(qtt_effect_atom_fingerprint(&state_a) ==
           qtt_effect_atom_fingerprint(&state_a_copy));
    assert(qtt_effect_atom_fingerprint(&state_a) !=
           qtt_effect_atom_fingerprint(&state_b));
    QttEffectAtom state_other_traits = state_a;
    state_other_traits.traits = "state,write";
    assert(!qtt_effect_atom_equal(&state_a, &state_other_traits));
    assert(qtt_effect_atom_fingerprint(&state_a) !=
           qtt_effect_atom_fingerprint(&state_other_traits));
    assert(qtt_effect_atom_has_trait(&state_a, "read"));
    assert(!qtt_effect_atom_has_trait(&state_a, "write"));
    QttEffectAtom state_multishot = state_a;
    state_multishot.resumption = qtt_quantity_omega();
    assert(!qtt_effect_atom_equal(&state_a, &state_multishot));
    assert(qtt_effect_atom_fingerprint(&state_a) !=
           qtt_effect_atom_fingerprint(&state_multishot));

    QttEffectRow *scoped = qtt_effect_extend_atom(
        arena, &state_a,
        qtt_effect_extend_atom(
            arena, &state_b, qtt_effect_empty(arena)));
    assert(scoped);
    char *trait_witness = NULL;
    assert(qtt_effect_row_satisfies_trait(
        solver, scoped, "io", &trait_witness) == QTT_EFFECT_RELATION_PROVED);
    assert(trait_witness && strcmp(trait_witness, "state -> io") == 0);
    free(trait_witness);
    assert(qtt_effect_row_satisfies_trait(
        solver, scoped, "telemetry", NULL) == QTT_EFFECT_RELATION_REFUTED);
    QttEffectRow *trait_open = qtt_effect_extend_atom(
        arena, &state_a, qtt_effect_fresh(arena));
    assert(qtt_effect_row_satisfies_trait(
        solver, trait_open, "telemetry", NULL) == QTT_EFFECT_RELATION_UNKNOWN);
    assert(qtt_effect_atom_count(solver, scoped, &state_a) == 1);
    assert(qtt_effect_atom_count(solver, scoped, &state_b) == 1);
    QttEffectRow *handled_a = qtt_effect_handle_atom(
        arena, solver, scoped, &state_a);
    assert(handled_a);
    assert(qtt_effect_atom_count(solver, handled_a, &state_a) == 0);
    assert(qtt_effect_atom_count(solver, handled_a, &state_b) == 1);
    assert(!qtt_effect_rows_equal(solver, scoped, handled_a));
    QttEffectHandlerResult linear_handler = qtt_effect_elaborate_handler(
        arena, solver, scoped, &state_a,
        qtt_quantity_finite(1), qtt_quantity_finite(1));
    assert(linear_handler.status == QTT_EFFECT_HANDLER_OK);
    assert(linear_handler.residual);
    assert(qtt_effect_atom_count(
               solver, linear_handler.residual, &state_a) == 0);
    assert(qtt_effect_atom_count(
               solver, linear_handler.residual, &state_b) == 1);

    QttEffectRow *multishot_row = qtt_effect_extend_atom(
        arena, &state_multishot, qtt_effect_empty(arena));
    QttEffectHandlerResult rejected_multishot =
        qtt_effect_elaborate_handler(
            arena, solver, multishot_row, &state_multishot,
            qtt_quantity_finite(1), qtt_quantity_finite(1));
    assert(rejected_multishot.status == QTT_EFFECT_HANDLER_GRADE_VIOLATION);
    assert(!rejected_multishot.residual);
    QttEffectHandlerResult unrestricted_multishot =
        qtt_effect_elaborate_handler(
            arena, solver, multishot_row, &state_multishot,
            qtt_quantity_finite(1), qtt_quantity_omega());
    assert(unrestricted_multishot.status == QTT_EFFECT_HANDLER_OK);

    QttEffectAtom abortive = state_a;
    abortive.capability_id = 46;
    abortive.resumption = qtt_quantity_finite(0);
    QttEffectRow *abortive_row = qtt_effect_extend_atom(
        arena, &abortive, qtt_effect_empty(arena));
    QttEffectHandlerResult abortive_handler = qtt_effect_elaborate_handler(
        arena, solver, abortive_row, &abortive,
        qtt_quantity_omega(), qtt_quantity_finite(0));
    assert(abortive_handler.status == QTT_EFFECT_HANDLER_OK);

    QttEffectHandlerResult absent_handler = qtt_effect_elaborate_handler(
        arena, solver, scoped, &abortive,
        qtt_quantity_finite(1), qtt_quantity_finite(1));
    assert(absent_handler.status == QTT_EFFECT_HANDLER_ABSENT);
    assert(!absent_handler.residual);

    QttEffectAtom telemetry = {
        .traits = "telemetry",
        .kind = QTT_EFFECT_CUSTOM,
        .constructor_id = 0x5452414345,
        .type_id = 19,
        .capability_id = 71,
        .operation = "emit",
        .resumption = qtt_quantity_finite(1),
    };
    QttEffectRow *telemetry_effect = qtt_effect_extend_atom(
        arena, &telemetry, qtt_effect_empty(arena));
    QttEffectHandlerClause clauses[] = {
        {
            .atom = &state_a,
            .continuation_usage = {1, false},
            .capture_allowance = {1, false},
            .clause_effects = telemetry_effect,
        },
        {
            .atom = &state_b,
            .continuation_usage = {1, false},
            .capture_allowance = {1, false},
            .clause_effects = qtt_effect_empty(arena),
        },
    };
    QttEffectHandlerSetResult handled_set = qtt_effect_elaborate_handler_set(
        arena, solver, scoped, clauses, 2);
    assert(handled_set.status == QTT_EFFECT_HANDLER_OK);
    assert(handled_set.handled_count == 2);
    assert(handled_set.residual && handled_set.output_effects);
    assert(qtt_effect_atom_count(
               solver, handled_set.residual, &state_a) == 0);
    assert(qtt_effect_atom_count(
               solver, handled_set.residual, &state_b) == 0);
    assert(qtt_effect_atom_count(
               solver, handled_set.output_effects, &telemetry) == 1);
    assert(handled_set.proof_fingerprint != 0);
    QttEffectHandlerSetResult handled_set_again =
        qtt_effect_elaborate_handler_set(arena, solver, scoped, clauses, 2);
    assert(handled_set_again.status == QTT_EFFECT_HANDLER_OK);
    assert(handled_set_again.proof_fingerprint ==
           handled_set.proof_fingerprint);
    QttEffectHandlerClause permuted_clauses[] = {clauses[1], clauses[0]};
    QttEffectHandlerSetResult permuted_set =
        qtt_effect_elaborate_handler_set(
            arena, solver, scoped, permuted_clauses, 2);
    assert(permuted_set.status == QTT_EFFECT_HANDLER_OK);
    assert(permuted_set.proof_fingerprint == handled_set.proof_fingerprint);
    assert(qtt_effect_rows_equal(
        solver, permuted_set.output_effects, handled_set.output_effects));
    assert(qtt_effect_verify_handler_set(
        arena, solver, scoped, clauses, 2, handled_set.proof_fingerprint));
    assert(!qtt_effect_verify_handler_set(
        arena, solver, scoped, clauses, 2,
        handled_set.proof_fingerprint ^ 1));
    char *portable_handler_proof =
        qtt_effect_handler_proof_serialize(solver, &handled_set);
    assert(portable_handler_proof);
    assert(strstr(portable_handler_proof, "monad-handler-proof-v2|") ==
           portable_handler_proof);
    QttEffectHandlerProof restored_handler_proof;
    assert(qtt_effect_handler_proof_deserialize(
        portable_handler_proof, &restored_handler_proof));
    assert(restored_handler_proof.handled_count == 2);
    assert(restored_handler_proof.format_version == 2);
    assert(restored_handler_proof.proof_fingerprint ==
           handled_set.proof_fingerprint);
    assert(qtt_effect_verify_portable_handler_proof(
        arena, solver, scoped, clauses, 2, portable_handler_proof));
    portable_handler_proof[21] = '1';
    assert(qtt_effect_handler_proof_deserialize(
        portable_handler_proof, &restored_handler_proof));
    assert(restored_handler_proof.format_version == 1);
    assert(qtt_effect_verify_portable_handler_proof(
        arena, solver, scoped, clauses, 2, portable_handler_proof));
    portable_handler_proof[21] = '2';
    portable_handler_proof[strlen(portable_handler_proof) - 1] =
        portable_handler_proof[strlen(portable_handler_proof) - 1] == '0'
            ? '1' : '0';
    assert(!qtt_effect_verify_portable_handler_proof(
        arena, solver, scoped, clauses, 2, portable_handler_proof));
    free(portable_handler_proof);

    QttEffectHandlerClause duplicate_clauses[] = {clauses[0], clauses[0]};
    QttEffectHandlerSetResult duplicate_set =
        qtt_effect_elaborate_handler_set(
            arena, solver, scoped, duplicate_clauses, 2);
    assert(duplicate_set.status == QTT_EFFECT_HANDLER_DUPLICATE_CLAUSE);
    assert(!duplicate_set.residual && !duplicate_set.output_effects);

    QttEffectAtom scoped_local = state_a;
    scoped_local.capability_id = 81;
    scoped_local.scoped = true;
    QttEffectRow *scoped_local_row = qtt_effect_extend_atom(
        arena, &scoped_local, qtt_effect_empty(arena));
    QttEffectHandlerClause escaping_clause = {
        .atom = &scoped_local,
        .continuation_usage = {1, false},
        .capture_allowance = {1, false},
        .clause_effects = scoped_local_row,
    };
    QttEffectHandlerSetResult escaping_set =
        qtt_effect_elaborate_handler_set(
            arena, solver, scoped_local_row, &escaping_clause, 1);
    assert(escaping_set.status == QTT_EFFECT_HANDLER_SCOPED_ESCAPE);
    assert(!escaping_set.residual && !escaping_set.output_effects);
    QttEffectScheme *typed_scheme = qtt_effect_generalize(
        arena, solver, scoped);
    assert(typed_scheme);
    char *portable_typed = qtt_effect_scheme_serialize(typed_scheme);
    assert(portable_typed && strstr(
        portable_typed, "monad-effect-scheme-v3|") == portable_typed);
    QttEffectScheme *restored_typed =
        qtt_effect_scheme_deserialize(portable_typed);
    assert(restored_typed);
    assert(qtt_effect_scheme_fingerprint(restored_typed) ==
           qtt_effect_scheme_fingerprint(typed_scheme));
    QttEffectRow *restored_typed_row = qtt_effect_instantiate(
        arena, restored_typed);
    assert(restored_typed_row);
    assert(qtt_effect_atom_count(
               solver, restored_typed_row, &state_a) == 1);
    assert(qtt_effect_atom_count(
               solver, restored_typed_row, &state_b) == 1);
    portable_typed[strlen(portable_typed) - 1] =
        portable_typed[strlen(portable_typed) - 1] == '0' ? '1' : '0';
    assert(!qtt_effect_scheme_deserialize(portable_typed));
    free(portable_typed);
    qtt_effect_scheme_free(restored_typed);
    qtt_effect_scheme_free(typed_scheme);
    QttEffectRow *typed_tail = qtt_effect_fresh(arena);
    QttEffectRow *typed_open = qtt_effect_extend_atom(
        arena, &state_a, typed_tail);
    QttEffectRow *typed_closed = qtt_effect_extend_atom(
        arena, &state_b,
        qtt_effect_extend_atom(
            arena, &state_a, qtt_effect_empty(arena)));
    QttEffectSolver *typed_solver = qtt_effect_solver_new(arena);
    assert(typed_open && typed_closed && typed_solver);
    assert(qtt_effect_unify(typed_solver, typed_open, typed_closed) ==
           QTT_EFFECT_UNIFY_OK);
    assert(qtt_effect_rows_equal(typed_solver, typed_open, typed_closed));
    QttEffectRow *join_ab = qtt_effect_join_closed(
        arena, solver,
        qtt_effect_extend_atom(arena, &state_a, qtt_effect_empty(arena)),
        qtt_effect_extend_atom(arena, &state_b, qtt_effect_empty(arena)));
    QttEffectRow *join_ba = qtt_effect_join_closed(
        arena, solver,
        qtt_effect_extend_atom(arena, &state_b, qtt_effect_empty(arena)),
        qtt_effect_extend_atom(arena, &state_a, qtt_effect_empty(arena)));
    assert(join_ab && join_ba);
    assert(qtt_effect_rows_equal(solver, join_ab, join_ba));
    QttEffectRow *join_idempotent = qtt_effect_join_closed(
        arena, solver, join_ab, join_ab);
    assert(join_idempotent);
    assert(qtt_effect_rows_equal(solver, join_ab, join_idempotent));
    QttEffectRow *left_assoc = qtt_effect_join_closed(
        arena, solver, join_ab,
        qtt_effect_extend_atom(
            arena, &state_multishot, qtt_effect_empty(arena)));
    QttEffectRow *right_assoc = qtt_effect_join_closed(
        arena, solver,
        qtt_effect_extend_atom(arena, &state_a, qtt_effect_empty(arena)),
        qtt_effect_join_closed(
            arena, solver,
            qtt_effect_extend_atom(arena, &state_b, qtt_effect_empty(arena)),
            qtt_effect_extend_atom(
                arena, &state_multishot, qtt_effect_empty(arena))));
    assert(left_assoc && right_assoc);
    assert(qtt_effect_rows_equal(solver, left_assoc, right_assoc));
    assert(!qtt_effect_join_closed(arena, solver, typed_tail, join_ab));
    formatted = qtt_effect_format(solver, scoped);
    assert(formatted);
    assert(strcmp(
        formatted,
        "<state#44:get[type=#0000000000000011,resume=1],"
        "state#45:get[type=#0000000000000011,resume=1]>") == 0);
    free(formatted);
    qtt_effect_solver_free(typed_solver);

    qtt_effect_solver_free(occurs);
    qtt_effect_scheme_free(scheme);
    qtt_effect_solver_free(solver);
    qtt_effect_arena_free(arena);
    qtt_effect_trait_implications_clear();
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_effect_test.c"
            executable = directory / "qtt_effect_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "effects" / "effect.c"),
                    str(ROOT / "qtt" / "place.c"),
                    str(ROOT / "qtt" / "quantity.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            # LeakSanitizer cannot run under the repository's traced test
            # environment; AddressSanitizer and UBSan remain active.
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
