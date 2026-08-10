"""Collision-safe canonical identities for zonked QTT types."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttTypeIdentityTests(unittest.TestCase):
    def test_structural_interning_resolves_fingerprint_collisions(self):
        source = r'''
#include "qtt/type_identity.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    Type int_a = {.kind = TYPE_INT};
    Type int_b = {.kind = TYPE_INT};
    Type string_type = {.kind = TYPE_STRING};
    Type arrow_a = {
        .kind = TYPE_ARROW,
        .arrow_param = &int_a,
        .arrow_ret = &string_type,
    };
    Type arrow_b = {
        .kind = TYPE_ARROW,
        .arrow_param = &int_b,
        .arrow_ret = &string_type,
    };
    Type effectful_arrow = {
        .kind = TYPE_ARROW,
        .arrow_param = &int_b,
        .arrow_ret = &string_type,
        .arrow_effect_scheme = "effect-scheme-io",
        .arrow_effect_complete = true,
        .arrow_effect_name = "io.read",
    };

    QttTypeArena *arena = qtt_type_arena_new();
    assert(arena);
    QttTypeIdentityError error = QTT_TYPE_IDENTITY_OK;
    QttTypeId first = qtt_type_intern(arena, &arrow_a, &error);
    QttTypeId second = qtt_type_intern(arena, &arrow_b, &error);
    assert(error == QTT_TYPE_IDENTITY_OK);
    assert(first.value && qtt_type_id_equal(first, second));
    QttTypeId effectful = qtt_type_intern(arena, &effectful_arrow, &error);
    assert(effectful.value && !qtt_type_id_equal(first, effectful));

    char *encoded = qtt_type_serialize(&effectful_arrow);
    Type *decoded = qtt_type_deserialize(encoded);
    assert(encoded && decoded && decoded->kind == TYPE_ARROW);
    assert(decoded->arrow_effect_complete);
    assert(decoded->arrow_effect_scheme);
    assert(strcmp(decoded->arrow_effect_scheme, "effect-scheme-io") == 0);
    assert(decoded->arrow_effect_name);
    assert(strcmp(decoded->arrow_effect_name, "io.read") == 0);
    assert(qtt_type_fingerprint(decoded) ==
           qtt_type_fingerprint(&effectful_arrow));
    free(encoded);
    qtt_type_free_owned(decoded);

    QttTypeId collided_int = qtt_type_intern_with_fingerprint(
        arena, &int_a, 7, &error);
    QttTypeId collided_string = qtt_type_intern_with_fingerprint(
        arena, &string_type, 7, &error);
    assert(collided_int.value && collided_string.value);
    assert(!qtt_type_id_equal(collided_int, collided_string));
    QttTypeId same_int = qtt_type_intern_with_fingerprint(
        arena, &int_b, 7, &error);
    assert(qtt_type_id_equal(collided_int, same_int));
    assert(qtt_type_arena_count(arena) == 4);
    assert(qtt_type_lookup(arena, first) == &arrow_a);

    qtt_type_arena_free(arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_type_identity_test.c"
            executable = directory / "qtt_type_identity_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "type_identity.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
