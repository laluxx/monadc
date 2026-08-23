#ifndef MONAD_READER_DIAGNOSTIC_H
#define MONAD_READER_DIAGNOSTIC_H

#include <stdbool.h>
#include <setjmp.h>

typedef struct ReaderDiagnostic {
    const char *filename;
    int line;
    int column;
    int end_column;
    const char *message;
    const char *hint;
} ReaderDiagnostic;

typedef void (*ReaderDiagnosticSink)(
    void *context, const ReaderDiagnostic *diagnostic);

typedef void (*ReaderDiagnosticCleanupFn)(void *context);

typedef struct ReaderDiagnosticCleanup {
    ReaderDiagnosticCleanupFn function;
    void *context;
    struct ReaderDiagnosticCleanup *previous;
} ReaderDiagnosticCleanup;

typedef struct ReaderDiagnosticContext {
    jmp_buf escape;
    ReaderDiagnosticSink sink;
    void *sink_context;
    struct ReaderDiagnosticContext *previous;
    ReaderDiagnosticCleanup *cleanup;
    bool armed;
} ReaderDiagnosticContext;

void reader_diagnostic_context_push(
    ReaderDiagnosticContext *context, ReaderDiagnosticSink sink,
    void *sink_context);
void reader_diagnostic_context_arm(ReaderDiagnosticContext *context);
bool reader_diagnostic_context_pop(ReaderDiagnosticContext *context);
bool reader_diagnostic_context_is_active(void);
bool reader_diagnostic_cleanup_defer(
    ReaderDiagnosticContext *context, ReaderDiagnosticCleanup *cleanup,
    ReaderDiagnosticCleanupFn function, void *cleanup_context);
bool reader_diagnostic_cleanup_cancel(
    ReaderDiagnosticContext *context, ReaderDiagnosticCleanup *cleanup);
bool reader_diagnostic_cleanup_defer_current(
    ReaderDiagnosticCleanup *cleanup, ReaderDiagnosticCleanupFn function,
    void *cleanup_context);
bool reader_diagnostic_cleanup_cancel_current(
    ReaderDiagnosticCleanup *cleanup);
/* Returns false when no armed structured context exists. With an active
 * context this function invokes its sink and does not return. */
bool reader_diagnostic_raise(
    const char *filename, int line, int column, int end_column,
    const char *message, const char *hint);

#endif
