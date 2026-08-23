/*
:TEST-ID tests.embedding.surface.prepared-source
:TEST-CONTEXT dec.embedding.prepared-source-surface
:TEST-PURPOSE Prove source checking is one owned operation without false publication.
:TEST-ATOM atom.embedding.33.surface.prepared-source
:TEST-EXPECT valid source is prepared; invalid source returns an owned diagnostic
:TEST-MENU-PATH embedding/branches/surface/atom.embedding.33.surface.prepared-source
*/
#include <monad/monad.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    monad_t *monad = NULL;
    monad_error_t error = {0};
    monad_source_result_t *result = NULL;
    char valid[] = "define identity :: Int -> Int\n  value -> value\n";

    assert(monad_open(&monad, &error) == MONAD_OK);
    assert(monad_prepare_string(
        monad, "memory://valid.mon", valid, &result, &error) == MONAD_OK);
    memset(valid, 'x', strlen(valid));
    assert(monad_source_result_is_valid(result));
    assert(monad_source_result_diagnostic_count(result) == 0);
    assert(monad_source_result_definition_count(result) == 1);
    monad_source_result_destroy(result);

    result = NULL;
    assert(monad_prepare_string(
        monad, "memory://invalid.mon",
        "[1 \"two\"]\n", &result, &error) == MONAD_OK);
    assert(!monad_source_result_is_valid(result));
    assert(monad_source_result_diagnostic_count(result) == 1);
    assert(strcmp(monad_source_result_diagnostic_code(result, 0),
                  "MONAD-C0003") == 0);
    assert(monad_source_result_diagnostic_message(result, 0));
    monad_source_result_destroy(result);

    assert(monad_close(monad, &error) == MONAD_OK);
    puts("prepared source owns validation results");
    return 0;
}
