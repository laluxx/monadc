/*
:TEST-ID tests.embedding.live-source.atomic-redefinition
:TEST-CONTEXT dec.embedding.live-image
:TEST-PURPOSE Prove source reload replaces a complete native generation.
:TEST-ATOM atom.embedding.39.live-source.atomic-redefinition
:TEST-EXPECT two definitions reload together; rejected generation changes neither
:TEST-MENU-PATH embedding/branches/live-source/atom.embedding.39.live-source.atomic-redefinition
*/
#include <monad/monad.h>

#include <assert.h>
#include <stdio.h>

static void load(monad_t *monad, const char *name, const char *source) {
    monad_error_t error = {0};
    monad_source_result_t *result = NULL;
    monad_status_t status = monad_load_string(monad, name, source, &result, &error);
    if (status != MONAD_OK) fprintf(stderr, "%s: %s\n", name, error.message);
    assert(status == MONAD_OK);
    assert(monad_source_result_is_valid(result));
    assert(monad_source_result_is_published(result));
    monad_source_result_destroy(result);
}

int main(void) {
    monad_t *monad = NULL;
    monad_error_t error = {0};
    monad_i64_function_t left = NULL, right = NULL;
    assert(monad_open(&monad, &error) == MONAD_OK);

    load(monad, "memory://generation-1.mon",
         "define left :: Int -> Int\n  value -> value + 1\n"
         "define right :: Int -> Int\n  value -> value + 10\n");
    assert(monad_find_i64_function(monad, "left", &left, &error) == MONAD_OK);
    assert(monad_find_i64_function(monad, "right", &right, &error) == MONAD_OK);
    assert(left(1) == 2 && right(1) == 11);

    load(monad, "memory://generation-2.mon",
         "define left :: Int -> Int\n  value -> value + 100\n"
         "define right :: Int -> Int\n  value -> value + 1000\n");
    assert(monad_find_i64_function(monad, "left", &left, &error) == MONAD_OK);
    assert(monad_find_i64_function(monad, "right", &right, &error) == MONAD_OK);
    assert(left(1) == 101 && right(1) == 1001);

    monad_source_result_t *rejected = NULL;
    assert(monad_load_string(
        monad, "memory://rejected.mon",
        "define left :: Int -> Int\n  value -> value + 7\n"
        "[1 \"two\"]\n",
        &rejected, &error) == MONAD_OK);
    assert(!monad_source_result_is_valid(rejected));
    assert(!monad_source_result_is_published(rejected));
    monad_source_result_destroy(rejected);
    assert(left(1) == 101 && right(1) == 1001);

    assert(monad_close(monad, &error) == MONAD_OK);
    puts("native source generation replaced atomically");
    return 0;
}
