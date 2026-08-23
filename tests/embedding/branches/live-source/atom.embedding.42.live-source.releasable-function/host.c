/*
:TEST-ID tests.embedding.live-source.releasable-function
:TEST-CONTEXT dec.embedding.address-owned-code-lifetime
:TEST-PURPOSE Prove a typed handle controls one frozen pointer's ORC lifetime.
:TEST-ATOM atom.embedding.42.live-source.releasable-function
:TEST-EXPECT handle pins old code; busy close is non-destructive; release reclaims
:TEST-MENU-PATH embedding/branches/live-source/atom.embedding.42.live-source.releasable-function
*/
#include <monad/monad.h>

#include <assert.h>
#include <stdio.h>

static void load(monad_t *monad, const char *name, int addend) {
    char source[160];
    snprintf(source, sizeof(source),
             "define step :: Int -> Int\n  value -> value + %d\n", addend);
    monad_error_t error = {0};
    monad_source_result_t *result = NULL;
    assert(monad_load_string(monad, name, source, &result, &error) == MONAD_OK);
    assert(monad_source_result_is_published(result));
    monad_source_result_destroy(result);
}

int main(void) {
    monad_t *monad = NULL;
    monad_i64_handle_t *handle = NULL;
    monad_error_t error = {0};
    assert(monad_open(&monad, &error) == MONAD_OK);
    load(monad, "memory://one.mon", 1);
    assert(monad_find_i64_handle(
        monad, "step", &handle, &error) == MONAD_OK);
    monad_i64_function_t frozen = monad_i64_handle_address(handle);
    assert(frozen && frozen(10) == 11);

    load(monad, "memory://two.mon", 2);
    assert(monad_live_native_image_count(monad) == 2);
    assert(frozen(10) == 11);
    assert(monad_close(monad, &error) == MONAD_ERROR_BUSY);
    assert(frozen(20) == 21);

    monad_i64_handle_release(handle);
    assert(monad_live_native_image_count(monad) == 1);
    assert(monad_close(monad, &error) == MONAD_OK);
    puts("typed handle released its frozen ORC image");
    return 0;
}
