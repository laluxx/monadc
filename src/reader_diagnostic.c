#include "reader_diagnostic.h"

#include <stddef.h>

/* Recovery targets are caller-owned stack objects because longjmp may only
 * return to a still-live setjmp invocation:
 * https://pubs.opengroup.org/onlinepubs/9799919799/functions/longjmp.html
 * Thread-local context stacks isolate simultaneous host compilation requests;
 * no process-global jump target or diagnostic sink is shared.
 */
#if defined(_MSC_VER)
#define MONAD_THREAD_LOCAL __declspec(thread)
#else
#define MONAD_THREAD_LOCAL __thread
#endif

static MONAD_THREAD_LOCAL ReaderDiagnosticContext *current_context;

void reader_diagnostic_context_push(
    ReaderDiagnosticContext *context, ReaderDiagnosticSink sink,
    void *sink_context) {
    if (!context) return;
    context->sink = sink;
    context->sink_context = sink_context;
    context->previous = current_context;
    context->cleanup = NULL;
    context->armed = false;
    current_context = context;
}

void reader_diagnostic_context_arm(ReaderDiagnosticContext *context) {
    if (context && current_context == context) context->armed = true;
}

bool reader_diagnostic_context_pop(ReaderDiagnosticContext *context) {
    if (!context || current_context != context || context->cleanup) return false;
    current_context = context->previous;
    context->previous = NULL;
    context->armed = false;
    return true;
}

bool reader_diagnostic_context_is_active(void) {
    return current_context != NULL;
}

bool reader_diagnostic_cleanup_defer(
    ReaderDiagnosticContext *context, ReaderDiagnosticCleanup *cleanup,
    ReaderDiagnosticCleanupFn function, void *cleanup_context) {
    if (!context || current_context != context || !cleanup || !function)
        return false;
    cleanup->function = function;
    cleanup->context = cleanup_context;
    cleanup->previous = context->cleanup;
    context->cleanup = cleanup;
    return true;
}

bool reader_diagnostic_cleanup_cancel(
    ReaderDiagnosticContext *context, ReaderDiagnosticCleanup *cleanup) {
    if (!context || current_context != context || !cleanup) return false;
    ReaderDiagnosticCleanup **link = &context->cleanup;
    while (*link && *link != cleanup) link = &(*link)->previous;
    if (!*link) return false;
    *link = cleanup->previous;
    cleanup->previous = NULL;
    cleanup->function = NULL;
    cleanup->context = NULL;
    return true;
}

bool reader_diagnostic_cleanup_defer_current(
    ReaderDiagnosticCleanup *cleanup, ReaderDiagnosticCleanupFn function,
    void *cleanup_context) {
    return reader_diagnostic_cleanup_defer(
        current_context, cleanup, function, cleanup_context);
}

bool reader_diagnostic_cleanup_cancel_current(
    ReaderDiagnosticCleanup *cleanup) {
    return reader_diagnostic_cleanup_cancel(current_context, cleanup);
}

bool reader_diagnostic_raise(
    const char *filename, int line, int column, int end_column,
    const char *message, const char *hint) {
    ReaderDiagnosticContext *context = current_context;
    if (!context || !context->armed) return false;
    ReaderDiagnostic diagnostic = {
        filename ? filename : "<input>", line, column, end_column,
        message ? message : "parser error", hint,
    };
    ReaderDiagnosticSink sink = context->sink;
    void *sink_context = context->sink_context;
    ReaderDiagnosticCleanup *cleanup = context->cleanup;
    current_context = context->previous;
    context->previous = NULL;
    context->cleanup = NULL;
    context->armed = false;
    while (cleanup) {
        ReaderDiagnosticCleanup *previous = cleanup->previous;
        ReaderDiagnosticCleanupFn function = cleanup->function;
        void *cleanup_context = cleanup->context;
        cleanup->previous = NULL;
        cleanup->function = NULL;
        cleanup->context = NULL;
        function(cleanup_context);
        cleanup = previous;
    }
    if (sink) sink(sink_context, &diagnostic);
    longjmp(context->escape, 1);
    return true;
}
