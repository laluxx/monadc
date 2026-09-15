/*
:TEST-ID tests.embedding.executable-source.native-string
:TEST-CONTEXT dec.embedding.executable-source
:TEST-PURPOSE Prove a checked Monad string becomes a direct native C call.
:TEST-ATOM atom.embedding.38.executable-source.native-string
:TEST-EXPECT valid source publishes atomically; invalid replacement preserves old code
:TEST-MENU-PATH embedding/branches/executable-source/atom.embedding.38.executable-source.native-string
*/
#include <monad/monad.h>

#include <assert.h>
#include <stdio.h>

int main(void) {
    monad_t *monad = NULL;
    monad_error_t error = {0};
    monad_source_result_t *result = NULL;
    monad_i64_function_t increment = NULL;

    assert(monad_open(&monad, &error) == MONAD_OK);
    monad_status_t load_status = monad_load_string(
        monad, "memory://increment.mon",
        "define increment :: Int -> Int\n"
        "  value -> value + 1\n",
        &result, &error);
    if (load_status != MONAD_OK)
        fprintf(stderr, "load failed: %s\n", error.message);
    assert(load_status == MONAD_OK);
    assert(monad_source_result_is_valid(result));
    assert(monad_source_result_is_published(result));
    monad_source_result_destroy(result);

    assert(monad_find_i64_function(
        monad, "increment", &increment, &error) == MONAD_OK);
    assert(increment(41) == 42);

    result = NULL;
    assert(monad_load_string(
        monad, "memory://broken.mon",
        "[1 \"two\"]\n",
        &result, &error) == MONAD_OK);
    assert(!monad_source_result_is_valid(result));
    assert(!monad_source_result_is_published(result));
    monad_source_result_destroy(result);
    assert(increment(9) == 10);

    assert(monad_close(monad, &error) == MONAD_OK);
    puts("Monad source string compiled to a direct native call");
    return 0;
}
