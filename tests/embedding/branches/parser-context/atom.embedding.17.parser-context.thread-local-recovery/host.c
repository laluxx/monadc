/*
:TEST-ID tests.embedding.parser-context.thread-local-recovery
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.17.parser-context.thread-local-recovery
:TEST-QUALITY concurrency-unit
:TEST-SUBSET parser-context
:TEST-CATEGORY 17-thread-local-recovery
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.diagnostic-result-algebra
:TEST-PURPOSE Parser failures recover through nested per-thread contexts without output or process termination.
:TEST-ATOM two concurrent contexts -> isolated diagnostics and longjmp targets -> clean context stacks
:TEST-EXPECT compile, run, exact-stdout
:TEST-COVERAGE reader_diagnostic.c context stack, synchronous sink, thread isolation, recovery
:TEST-USES-ATOM atom.embedding.category.17.thread-local-recovery
:TEST-MENU branches/parser-context/atom.embedding.17.parser-context.thread-local-recovery
:TEST-MENU-PATH embedding/branches/parser-context/atom.embedding.17.parser-context.thread-local-recovery
:TEST-ATOM-LEAF atom.embedding.17.parser-context.thread-local-recovery
:TEST-CLI-TARGET embedding branches parser-context
*/
#include "reader_diagnostic.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef struct capture {
    const char *expected;
    int seen;
    int line;
    int column;
} capture_t;

static void capture(void *opaque, const ReaderDiagnostic *diagnostic) {
    capture_t *state = opaque;
    assert(strcmp(diagnostic->message, state->expected) == 0);
    assert(strcmp(diagnostic->filename, "buffer.mon") == 0);
    state->seen++;
    state->line = diagnostic->line;
    state->column = diagnostic->column;
}

static void *worker(void *opaque) {
    capture_t *state = opaque;
    ReaderDiagnosticContext context;
    reader_diagnostic_context_push(&context, capture, state);
    if (setjmp(context.escape) == 0) {
        reader_diagnostic_context_arm(&context);
        reader_diagnostic_raise("buffer.mon", 7, 3, 4, state->expected, NULL);
        assert(0 && "raise must recover");
    }
    assert(state->seen == 1 && state->line == 7 && state->column == 3);
    assert(!reader_diagnostic_context_is_active());
    return NULL;
}

int main(void) {
    capture_t first = {"first failure", 0, 0, 0};
    capture_t second = {"second failure", 0, 0, 0};
    pthread_t a, b;
    assert(pthread_create(&a, NULL, worker, &first) == 0);
    assert(pthread_create(&b, NULL, worker, &second) == 0);
    assert(pthread_join(a, NULL) == 0);
    assert(pthread_join(b, NULL) == 0);
    assert(first.seen == 1 && second.seen == 1);

    ReaderDiagnosticContext outer, inner;
    reader_diagnostic_context_push(&outer, capture, &first);
    reader_diagnostic_context_push(&inner, capture, &second);
    reader_diagnostic_context_pop(&inner);
    assert(reader_diagnostic_context_is_active());
    reader_diagnostic_context_pop(&outer);
    assert(!reader_diagnostic_context_is_active());

    puts("thread-local parser recovery ok");
    return 0;
}
