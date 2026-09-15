/*
:TEST-ID tests.embedding.frontend-transaction.wisp-recovery
:TEST-CONTEXT dec.embedding.frontend-transaction-boundary
:TEST-PURPOSE Prove one entry point owns Wisp recovery, diagnostics, and AST transfer.
:TEST-ATOM atom.embedding.21.frontend-transaction.wisp-recovery
:TEST-EXPECT repeated silent malformed Wisp recovery followed by valid commit
:TEST-MENU-PATH embedding/branches/frontend-transaction/atom.embedding.21.frontend-transaction.wisp-recovery
*/
#include "embed/frontend_transaction.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const char *bad = "define broken :: Int -> Int\n  value -> [value\n";
    for (int i = 0; i < 32; i++) {
        MonadFrontendResult result;
        if (monad_frontend_parse(bad, "bad.mon", &result)) abort();
        if (!result.has_diagnostic || result.diagnostic.line < 1 ||
            result.diagnostic.message[0] == '\0') abort();
    }

    MonadFrontendResult result;
    const char *good = "define identity :: Int -> Int\n  value -> value\n";
    if (!monad_frontend_parse(good, "good.mon", &result)) abort();
    if (result.has_diagnostic || result.ast.count == 0) abort();
    monad_frontend_result_destroy(&result);
    puts("frontend transaction recovers and commits");
    return 0;
}
