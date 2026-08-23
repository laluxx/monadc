/*
:TEST-ID tests.embedding.surface.source-hooks
:TEST-CONTEXT dec.embedding.guile-facility-map
:TEST-PURPOSE Prove C strings, byte views, and files share one owned source boundary.
:TEST-ATOM atom.embedding.37.surface.source-hooks
:TEST-EXPECT all source hooks prepare independently of caller buffers and file lifetime
:TEST-MENU-PATH embedding/branches/surface/atom.embedding.37.surface.source-hooks
*/
#include <monad/monad.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void require_valid(monad_source_result_t *result) {
    assert(result && monad_source_result_is_valid(result));
    assert(monad_source_result_definition_count(result) == 1);
    monad_source_result_destroy(result);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    monad_t *monad = NULL;
    monad_error_t error = {0};
    monad_source_result_t *result = NULL;
    char source[] = "define identity :: Int -> Int\n  value -> value\n";
    assert(monad_open(&monad, &error) == MONAD_OK);

    assert(monad_prepare_string(
        monad, "memory://string.mon", source, &result, &error) == MONAD_OK);
    memset(source, 'x', strlen(source));
    require_valid(result);

    static const char bytes[] =
        "define answer :: Int\n  42\nignored trailing storage";
    size_t byte_count = strlen("define answer :: Int\n  42\n");
    result = NULL;
    assert(monad_prepare_source(
        monad, "memory://bytes.mon", bytes, byte_count,
        &result, &error) == MONAD_OK);
    require_valid(result);

    result = NULL;
    assert(monad_prepare_file(monad, argv[1], &result, &error) == MONAD_OK);
    require_valid(result);
    assert(monad_close(monad, &error) == MONAD_OK);
    puts("C string, byte view, and file share one source transaction");
    return 0;
}
