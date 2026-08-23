/*
:TEST-ID tests.embedding.frontend-state.same-thread-isolation
:TEST-CONTEXT dec.embedding.compiler-owned-frontend-state
:TEST-PURPOSE Prove persistent frontend knowledge follows state, not host thread.
:TEST-ATOM atom.embedding.24.frontend-state.same-thread-isolation
:TEST-EXPECT alternating states retain distinct Wisp arity classifications
:TEST-MENU-PATH embedding/branches/frontend-state/atom.embedding.24.frontend-state.same-thread-isolation
*/
#include "embed/frontend_transaction.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    MonadFrontendState *first = monad_frontend_state_create();
    MonadFrontendState *second = monad_frontend_state_create();
    assert(first && second);
    assert(monad_frontend_state_register_arity(first, "paint", 2));

    for (int i = 0; i < 32; i++) {
        assert(monad_frontend_classify_input(first, "paint 1", false) ==
               WISP_INPUT_INCOMPLETE);
        assert(monad_frontend_classify_input(second, "paint 1", false) ==
               WISP_INPUT_COMPLETE);
        MonadFrontendResult failed;
        assert(!monad_frontend_parse_in_state(
            first, "define bad :: Int -> Int\n  value -> [value\n",
            "bad.mon", &failed));
        assert(failed.has_diagnostic);
    }

    monad_frontend_state_destroy(first);
    monad_frontend_state_destroy(second);
    puts("frontend state follows its compiler session");
    return 0;
}
