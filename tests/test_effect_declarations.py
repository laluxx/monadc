"""Core-owned effect declarations construct structural effect atoms."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EffectDeclarationTests(unittest.TestCase):
    def test_open_declarations_construct_structural_atoms(self):
        source = r'''
#include "effects/effect.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    qtt_effect_declarations_clear();
    qtt_effect_trait_implications_clear();
    assert(qtt_effect_trait_implication_register("allocate", "state"));
    assert(qtt_effect_trait_implication_register("state", "io"));
    assert(qtt_effect_trait_implies("allocate", "allocate"));
    assert(qtt_effect_trait_implies("allocate", "io"));
    assert(!qtt_effect_trait_implies("io", "allocate"));
    char *witness = qtt_effect_trait_implication_witness("allocate", "io");
    assert(witness && strcmp(witness, "allocate -> state -> io") == 0);
    free(witness);
    /* Cycles deliberately form preorder equivalence classes. */
    assert(qtt_effect_trait_implication_register("io", "state"));
    assert(qtt_effect_trait_implies("io", "state"));
    assert(qtt_effect_trait_implies("state", "io"));
    QttEffectDeclaration io_read = {
        .name = "Core.IO.read",
        .traits = "io,ambient,io",
        .kind = QTT_EFFECT_IO,
        .operation = "read",
        .payload_type = "Int",
        .result_type = "String",
        .operation_scheme = "monad-hm-scheme-v1|78f3bd7a3897a80a|0|k31(k0;)(k4;)",
        .resumption = {.finite = 1},
    };
    assert(qtt_effect_declaration_register(&io_read));
    const QttEffectDeclaration *found =
        qtt_effect_declaration_lookup("Core.IO.read");
    assert(found && found->constructor_id != 0);
    assert(found->kind == QTT_EFFECT_IO);
    assert(strcmp(found->traits, "ambient,io") == 0);
    assert(qtt_effect_declaration_has_trait(found, "io"));
    assert(qtt_effect_declaration_has_trait(found, "ambient"));
    assert(!qtt_effect_declaration_has_trait(found, "write"));
    assert(strcmp(found->operation, "read") == 0);
    assert(strcmp(found->payload_type, "Int") == 0);
    assert(strcmp(found->result_type, "String") == 0);
    assert(strcmp(found->operation_scheme,
                  "monad-hm-scheme-v1|78f3bd7a3897a80a|0|k31(k0;)(k4;)") == 0);
    bool ambiguous = true;
    assert(qtt_effect_declaration_resolve("Core.IO.read", &ambiguous) ==
           found);
    assert(!ambiguous);
    assert(qtt_effect_declaration_resolve("io.read", &ambiguous) == found);
    assert(!ambiguous);
    assert(!qtt_effect_declaration_resolve("io.missing", &ambiguous));
    assert(!ambiguous);

    QttEffectDeclaration alternate_read = {
        .name = "Library.IO.read",
        .traits = "io",
        .kind = QTT_EFFECT_IO,
        .operation = "read",
        .resumption = {.finite = 1},
    };
    assert(qtt_effect_declaration_register(&alternate_read));
    assert(!qtt_effect_declaration_resolve("io.read", &ambiguous));
    assert(ambiguous);

    /* Idempotent declarations are accepted; conflicting redeclarations are
     * rejected so imported core modules cannot change an effect's meaning. */
    QttEffectDeclaration reordered = io_read;
    reordered.traits = "ambient,io";
    assert(qtt_effect_declaration_register(&reordered));
    QttEffectDeclaration conflict = io_read;
    conflict.kind = QTT_EFFECT_FOREIGN;
    assert(!qtt_effect_declaration_register(&conflict));
    conflict = io_read;
    conflict.operation_scheme = "monad-hm-scheme-v1|different";
    assert(!qtt_effect_declaration_register(&conflict));
    QttEffectDeclaration half_typed = io_read;
    half_typed.name = "Core.IO.invalid";
    half_typed.result_type = NULL;
    assert(!qtt_effect_declaration_register(&half_typed));

    QttEffectArena *arena = qtt_effect_arena_new();
    QttEffectSolver *solver = qtt_effect_solver_new(arena);
    QttEffectRow *row = qtt_effect_extend_declared(
        arena, "Core.IO.read", 7, 11, qtt_effect_empty(arena));
    QttEffectAtom expected = {
        .traits = "ambient,io",
        .kind = QTT_EFFECT_IO,
        .constructor_id = found->constructor_id,
        .type_id = 11,
        .capability_id = 7,
        .name = "Core.IO.read",
        .operation = "read",
        .resumption = {.finite = 1},
    };
    assert(row && qtt_effect_atom_count(solver, row, &expected) == 1);
    QttEffectAtom allocation = {.traits = "allocate"};
    assert(qtt_effect_atom_satisfies_trait(&allocation, "state"));
    assert(qtt_effect_atom_satisfies_trait(&allocation, "io"));
    assert(!qtt_effect_atom_satisfies_trait(&allocation, "foreign"));
    assert(!qtt_effect_extend_declared(
        arena, "Core.IO.missing", 0, 0, qtt_effect_empty(arena)));

    qtt_effect_solver_free(solver);
    qtt_effect_arena_free(arena);
    qtt_effect_declarations_clear();
    qtt_effect_trait_implications_clear();
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "effect_declarations.c"
            executable = directory / "effect_declarations"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-D_GNU_SOURCE",
                    "-Wall", "-Wextra", "-Werror",
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
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
