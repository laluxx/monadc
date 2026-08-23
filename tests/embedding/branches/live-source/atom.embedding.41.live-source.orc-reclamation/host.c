/*
:TEST-ID tests.embedding.live-source.orc-reclamation
:TEST-CONTEXT dec.embedding.read-generation-token
:TEST-PURPOSE Prove retired ORC images die exactly after their last native reader.
:TEST-ATOM atom.embedding.41.live-source.orc-reclamation
:TEST-EXPECT token pins retired image; end reclaims it; unpinned reload stays bounded
:TEST-MENU-PATH embedding/branches/live-source/atom.embedding.41.live-source.orc-reclamation
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
    assert(monad_source_result_is_valid(result));
    assert(monad_source_result_is_published(result));
    monad_source_result_destroy(result);
}

int main(void) {
    monad_t *monad = NULL;
    monad_read_t *old = NULL;
    monad_i64_function_t old_step = NULL;
    monad_error_t error = {0};
    assert(monad_open(&monad, &error) == MONAD_OK);

    load(monad, "memory://one.mon", 1);
    assert(monad_live_native_image_count(monad) == 1);
    assert(monad_read_begin(monad, &old, &error) == MONAD_OK);
    assert(monad_read_find_i64_function(
        old, "step", &old_step, &error) == MONAD_OK);

    load(monad, "memory://two.mon", 2);
    assert(monad_live_native_image_count(monad) == 2);
    assert(old_step(10) == 11);
    monad_read_end(old);
    assert(monad_live_native_image_count(monad) == 1);

    load(monad, "memory://three.mon", 3);
    assert(monad_live_native_image_count(monad) == 1);
    load(monad, "memory://four.mon", 4);
    assert(monad_live_native_image_count(monad) == 1);

    monad_i64_function_t frozen = NULL;
    assert(monad_find_i64_function(
        monad, "step", &frozen, &error) == MONAD_OK);
    load(monad, "memory://five.mon", 5);
    assert(monad_live_native_image_count(monad) == 2);
    assert(frozen(10) == 14);

    assert(monad_close(monad, &error) == MONAD_OK);
    puts("retired ORC images reclaimed after last reader");
    return 0;
}
