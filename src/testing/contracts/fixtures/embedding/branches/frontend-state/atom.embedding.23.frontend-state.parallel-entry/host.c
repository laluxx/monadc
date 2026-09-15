/*
:TEST-ID tests.embedding.frontend-state.parallel-entry
:TEST-CONTEXT dec.embedding.thread-affine-frontend-state
:TEST-PURPOSE Prove two protected frontend entries overlap without a process lock.
:TEST-ATOM atom.embedding.23.frontend-state.parallel-entry
:TEST-EXPECT barrier overlap and independent successful parses
:TEST-MENU-PATH embedding/branches/frontend-state/atom.embedding.23.frontend-state.parallel-entry
*/
#include "embed/frontend_transaction.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int entered;

static void overlap(void *unused) {
    (void)unused;
    assert(pthread_mutex_lock(&mutex) == 0);
    entered++;
    if (entered == 2) assert(pthread_cond_broadcast(&condition) == 0);
    while (entered != 2) assert(pthread_cond_wait(&condition, &mutex) == 0);
    assert(pthread_mutex_unlock(&mutex) == 0);
}

static void *worker(void *name) {
    MonadFrontendResult result;
    assert(monad_frontend_parse(
        "define identity :: Int -> Int\n  value -> value\n", name, &result));
    monad_frontend_result_destroy(&result);
    return NULL;
}

int main(void) {
    monad_frontend_set_entry_probe(overlap, NULL);
    pthread_t first, second;
    assert(pthread_create(&first, NULL, worker, "thread-one.mon") == 0);
    assert(pthread_create(&second, NULL, worker, "thread-two.mon") == 0);
    assert(pthread_join(first, NULL) == 0);
    assert(pthread_join(second, NULL) == 0);
    monad_frontend_set_entry_probe(NULL, NULL);
    assert(entered == 2);
    puts("frontend transactions overlap safely");
    return 0;
}
