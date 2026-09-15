/*
:TEST-ID tests.embedding.parser-context.lifo-unwind
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.18.parser-context.lifo-unwind
:TEST-QUALITY resource-safety
:TEST-SUBSET parser-context
:TEST-CATEGORY 18-lifo-unwind
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.parser-recovery-lifetime
:TEST-PURPOSE Parser recovery runs owned-resource cleanup exactly once in reverse acquisition order.
:TEST-ATOM defer three resources -> transfer one -> raise -> remaining two unwind LIFO before diagnostic sink
:TEST-EXPECT compile, run, exact-stdout
:TEST-COVERAGE reader_diagnostic.c cleanup registration, cancellation, LIFO unwind, sink ordering
:TEST-USES-ATOM atom.embedding.category.18.lifo-unwind
:TEST-MENU branches/parser-context/atom.embedding.18.parser-context.lifo-unwind
:TEST-MENU-PATH embedding/branches/parser-context/atom.embedding.18.parser-context.lifo-unwind
:TEST-ATOM-LEAF atom.embedding.18.parser-context.lifo-unwind
:TEST-CLI-TARGET embedding branches parser-context
*/
#include "reader_diagnostic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct log { int values[4]; size_t count; } log_t;
typedef struct item { log_t *log; int value; } item_t;

static void cleanup(void *opaque) {
    item_t *item = opaque;
    item->log->values[item->log->count++] = item->value;
}

static void sink(void *opaque, const ReaderDiagnostic *diagnostic) {
    log_t *log = opaque;
    assert(strcmp(diagnostic->message, "unwind") == 0);
    assert(log->count == 2 && log->values[0] == 2 && log->values[1] == 1);
}

int main(void) {
    log_t log = {{0}, 0};
    item_t one = {&log, 1}, two = {&log, 2}, transferred = {&log, 3};
    ReaderDiagnosticCleanup first, second, third;
    ReaderDiagnosticContext context;
    reader_diagnostic_context_push(&context, sink, &log);
    assert(reader_diagnostic_cleanup_defer(&context, &first, cleanup, &one));
    assert(reader_diagnostic_cleanup_defer(&context, &second, cleanup, &two));
    assert(reader_diagnostic_cleanup_defer(&context, &third, cleanup, &transferred));
    assert(reader_diagnostic_cleanup_cancel(&context, &third));
    if (setjmp(context.escape) == 0) {
        reader_diagnostic_context_arm(&context);
        reader_diagnostic_raise("buffer.mon", 9, 1, 1, "unwind", NULL);
        assert(0);
    }
    assert(log.count == 2 && !reader_diagnostic_context_is_active());
    cleanup(&transferred);
    assert(log.count == 3 && log.values[2] == 3);
    puts("parser resources unwind exactly once");
    return 0;
}
