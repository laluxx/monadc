/*
:TEST-ID tests.embedding.live-image.snapshot-values
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.05.live-image.snapshot-values
:TEST-QUALITY integration
:TEST-SUBSET live-image
:TEST-CATEGORY 05-snapshots-values
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.public-abi,monadc.context.env.table
:TEST-PURPOSE Immutable deterministic snapshots survive mutation and support exact typed variable filtering.
:TEST-ATOM define typed cells -> snapshot/filter -> mutate/CAS -> old snapshot stable -> new generation visible
:TEST-EXPECT compile, run
:TEST-COVERAGE embed/embed.c i64 cells, generations, snapshot ownership, sorting, filters
:TEST-USES-ATOM atom.embedding.category.05.snapshots-values
:TEST-MENU branches/live-image/atom.embedding.05.live-image.snapshot-values
:TEST-MENU-PATH embedding/branches/live-image/atom.embedding.05.live-image.snapshot-values
:TEST-ATOM-LEAF atom.embedding.05.live-image.snapshot-values
:TEST-CLI-TARGET embedding branches live-image
*/
#include <monad/embed.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    monad_runtime_config_t config;
    monad_runtime_t *runtime;
    monad_thread_t *thread;
    monad_variable_t *zeta;
    monad_variable_t *alpha;
    monad_binding_snapshot_t *before;
    monad_binding_snapshot_t *after;
    monad_binding_filter_t filter;
    monad_error_t error;
    int64_t value;
    int64_t expected;
    int changed;

    monad_runtime_config_init(&config);
    assert(monad_runtime_create(&config, &runtime, &error) == MONAD_OK);
    assert(monad_thread_attach(runtime, &thread, &error) == MONAD_OK);
    assert(monad_runtime_define_i64(thread, "zeta", 9, "Last value.",
                                    &zeta, &error) == MONAD_OK);
    assert(monad_runtime_define_i64(thread, "alpha", 1, "First value.",
                                    &alpha, &error) == MONAD_OK);

    monad_binding_filter_init(&filter);
    filter.kind_mask = MONAD_BINDING_MASK_VARIABLE;
    filter.exact_type = MONAD_ABI_I64;
    assert(monad_runtime_snapshot_bindings(thread, &filter, &before, &error) ==
           MONAD_OK);
    assert(monad_binding_snapshot_count(before) == 2);
    assert(strcmp(monad_binding_snapshot_at(before, 0)->name, "alpha") == 0);
    assert(strcmp(monad_binding_snapshot_at(before, 1)->name, "zeta") == 0);
    assert(monad_binding_snapshot_at(before, 0)->i64_value == 1);
    assert(monad_binding_snapshot_at(before, 0)->generation == 1);

    assert(monad_variable_set_i64(alpha, 7, &error) == MONAD_OK);
    expected = 7;
    assert(monad_variable_compare_exchange_i64(
        alpha, &expected, 11, &changed, &error) == MONAD_OK && changed);
    assert(monad_variable_get_i64(alpha, &value, &error) == MONAD_OK && value == 11);
    assert(monad_binding_snapshot_at(before, 0)->i64_value == 1);
    assert(monad_binding_snapshot_at(before, 0)->generation == 1);

    assert(monad_runtime_snapshot_bindings(thread, &filter, &after, &error) ==
           MONAD_OK);
    assert(monad_binding_snapshot_at(after, 0)->i64_value == 11);
    assert(monad_binding_snapshot_at(after, 0)->generation == 3);

    monad_binding_snapshot_release(before);
    monad_binding_snapshot_release(after);
    assert(monad_variable_release(alpha, &error) == MONAD_OK);
    assert(monad_variable_release(zeta, &error) == MONAD_OK);
    assert(monad_thread_detach(thread, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    puts("immutable typed snapshots ok");
    return 0;
}
