/*
:TEST-ID tests.embedding.lifecycle.opaque-runtime
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.01.lifecycle.opaque-runtime
:TEST-QUALITY integration
:TEST-SUBSET lifecycle
:TEST-CATEGORY 01-public-abi
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.public-abi
:TEST-PURPOSE The public C ABI provides opaque runtimes, explicit thread attachment, allocator control, and non-aborting errors.
:TEST-ATOM create -> attach -> busy destroy -> detach -> destroy
:TEST-EXPECT compile, run
:TEST-COVERAGE embed.c public lifecycle ABI
:TEST-USES-ATOM atom.embedding.category.01.public-abi
:TEST-MENU branches/lifecycle/atom.embedding.01.lifecycle.opaque-runtime
:TEST-MENU-PATH embedding/branches/lifecycle/atom.embedding.01.lifecycle.opaque-runtime
:TEST-ATOM-LEAF atom.embedding.01.lifecycle.opaque-runtime
:TEST-CLI-TARGET embedding branches lifecycle
*/
#include <monad/embed.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

typedef struct allocation_count { size_t live; } allocation_count_t;

static void *counted_allocate(void *context, size_t size) {
    allocation_count_t *count = context;
    void *pointer = malloc(size);
    if (pointer) count->live++;
    return pointer;
}

static void counted_free(void *context, void *pointer) {
    allocation_count_t *count = context;
    if (pointer) count->live--;
    free(pointer);
}

typedef struct detach_attempt {
    monad_thread_t *thread;
    monad_status_t status;
} detach_attempt_t;

static void *detach_on_wrong_thread(void *argument) {
    detach_attempt_t *attempt = argument;
    attempt->status = monad_thread_detach(attempt->thread, NULL);
    return NULL;
}

int main(void) {
    monad_runtime_config_t config;
    monad_runtime_t *runtime = NULL;
    monad_thread_t *thread = NULL;
    monad_error_t error;
    allocation_count_t allocations = {0};

    monad_runtime_config_init(&config);
    assert(config.abi_version == monad_embed_abi_version());
    config.allocator.context = &allocations;
    config.allocator.allocate = counted_allocate;
    config.allocator.deallocate = counted_free;

    assert(monad_runtime_create(&config, &runtime, &error) == MONAD_OK);
    assert(monad_thread_attach(runtime, &thread, &error) == MONAD_OK);
    assert(monad_thread_runtime(thread) == runtime);
    detach_attempt_t attempt = {thread, MONAD_OK};
    pthread_t other;
    assert(pthread_create(&other, NULL, detach_on_wrong_thread, &attempt) == 0);
    assert(pthread_join(other, NULL) == 0);
    assert(attempt.status == MONAD_ERROR_WRONG_THREAD);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_ERROR_BUSY);
    assert(error.message != NULL);
    assert(monad_thread_detach(thread, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    assert(allocations.live == 0);
    puts("embedding lifecycle ok");
    return 0;
}
