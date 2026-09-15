/*
:TEST-ID tests.embedding.parser-transaction.ast-ledger
:TEST-CONTEXT dec.embedding.parser-transaction-ownership
:TEST-PURPOSE Prove partial AST failure unwinds exactly once and success transfers roots.
:TEST-ATOM atom.embedding.20.parser-transaction.ast-ledger
:TEST-EXPECT silent repeated recovery followed by a valid transferred AST
:TEST-MENU-PATH embedding/branches/parser-transaction/atom.embedding.20.parser-transaction.ast-ledger
*/
#include "reader.h"
#include "reader_diagnostic.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

static void ignore_diagnostic(void *state, const ReaderDiagnostic *diagnostic) {
    int *count = state;
    if (!diagnostic || !diagnostic->message) abort();
    (*count)++;
}

int main(void) {
    int diagnostics = 0;
    for (int attempt = 0; attempt < 32; attempt++) {
        ReaderDiagnosticContext context;
        ReaderDiagnosticCleanup cleanup;
        ReaderAstLedger ledger;
        reader_diagnostic_context_push(&context, ignore_diagnostic, &diagnostics);
        reader_ast_ledger_begin(&ledger);
        if (!reader_diagnostic_cleanup_defer(
                &context, &cleanup, reader_ast_ledger_abort, &ledger)) abort();
        if (setjmp(context.escape) == 0) {
            reader_diagnostic_context_arm(&context);
            (void)parse_all("(outer (inner 1 2)");
            abort();
        }
        if (reader_ast_ledger_live_count(&ledger) != 0) abort();
    }

    ReaderAstLedger ledger;
    reader_ast_ledger_begin(&ledger);
    ASTList valid = parse_all("(ok 1 2)");
    if (valid.count != 1 || reader_ast_ledger_live_count(&ledger) == 0) abort();
    reader_ast_ledger_commit(&ledger);
    if (reader_ast_ledger_live_count(&ledger) != 0) abort();
    ast_free(valid.exprs[0]);
    free(valid.exprs);
    printf("parser AST transactions are exact\n");
    return diagnostics == 32 ? 0 : 1;
}
