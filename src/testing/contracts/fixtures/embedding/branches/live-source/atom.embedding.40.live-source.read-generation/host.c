/*
:TEST-ID tests.embedding.live-source.read-generation
:TEST-CONTEXT dec.embedding.native-generation-commit
:TEST-PURPOSE Prove one token pins a coherent callable generation across reload.
:TEST-ATOM atom.embedding.40.live-source.read-generation
:TEST-EXPECT old token calls old pair; new token calls new pair; lookup stays direct
:TEST-MENU-PATH embedding/branches/live-source/atom.embedding.40.live-source.read-generation
*/
#include <monad/monad.h>

#include <assert.h>
#include <stdio.h>

static void load(monad_t *monad, const char *name, const char *source) {
    monad_error_t error = {0};
    monad_source_result_t *result = NULL;
    assert(monad_load_string(monad, name, source, &result, &error) == MONAD_OK);
    assert(monad_source_result_is_valid(result));
    assert(monad_source_result_is_published(result));
    monad_source_result_destroy(result);
}

static monad_i64_function_t find(monad_read_t *read, const char *name) {
    monad_error_t error = {0};
    monad_i64_function_t function = NULL;
    assert(monad_read_find_i64_function(
        read, name, &function, &error) == MONAD_OK);
    return function;
}

int main(void) {
    monad_t *monad = NULL;
    monad_read_t *old = NULL, *current = NULL;
    monad_error_t error = {0};
    assert(monad_open(&monad, &error) == MONAD_OK);
    load(monad, "memory://one.mon",
         "define left :: Int -> Int\n  value -> value + 1\n"
         "define right :: Int -> Int\n  value -> value + 10\n");
    assert(monad_read_begin(monad, &old, &error) == MONAD_OK);
    uint64_t old_generation = monad_read_generation(old);
    monad_i64_function_t old_left = find(old, "left");
    monad_i64_function_t old_right = find(old, "right");

    load(monad, "memory://two.mon",
         "define left :: Int -> Int\n  value -> value + 100\n"
         "define right :: Int -> Int\n  value -> value + 1000\n");
    assert(old_left(1) == 2 && old_right(1) == 11);

    assert(monad_read_begin(monad, &current, &error) == MONAD_OK);
    assert(monad_read_generation(current) > old_generation);
    assert(find(current, "left")(1) == 101);
    assert(find(current, "right")(1) == 1001);
    monad_i64_function_t missing = NULL;
    assert(monad_read_find_i64_function(
        current, "missing", &missing, &error) == MONAD_ERROR_NOT_FOUND);
    assert(monad_close(monad, &error) == MONAD_ERROR_BUSY);
    assert(find(current, "left")(1) == 101);

    monad_read_end(current);
    monad_read_end(old);
    assert(monad_close(monad, &error) == MONAD_OK);
    puts("read token pinned one native generation");
    return 0;
}
