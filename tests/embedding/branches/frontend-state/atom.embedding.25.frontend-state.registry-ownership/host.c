/*
:TEST-ID tests.embedding.frontend-state.registry-ownership
:TEST-CONTEXT dec.embedding.frontend-module-substates
:TEST-PURPOSE Prove type, layout, and feature registries follow compiler state.
:TEST-ATOM atom.embedding.25.frontend-state.registry-ownership
:TEST-EXPECT alternating states remain disjoint across diagnostic unwind
:TEST-MENU-PATH embedding/branches/frontend-state/atom.embedding.25.frontend-state.registry-ownership
*/
#include "embed/frontend_transaction.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    MonadFrontendState *first = monad_frontend_state_create();
    MonadFrontendState *second = monad_frontend_state_create();
    assert(first && second);
    assert(monad_frontend_state_register_nominal(first, "Phoenix"));
    assert(monad_frontend_state_register_layout(first, "Point", "x"));
    assert(monad_frontend_state_add_feature(first, "VULKAN"));

    for (int i = 0; i < 32; i++) {
        assert(monad_frontend_state_has_nominal(first, "Phoenix"));
        assert(!monad_frontend_state_has_nominal(second, "Phoenix"));
        assert(monad_frontend_state_has_layout_field(first, "Point", "x"));
        assert(!monad_frontend_state_has_layout_field(second, "Point", "x"));
        assert(monad_frontend_state_has_feature(first, "VULKAN"));
        assert(!monad_frontend_state_has_feature(second, "VULKAN"));
        MonadFrontendResult failed;
        assert(!monad_frontend_parse_in_state(
            first, "define bad :: Int -> Int\n  value -> [value\n",
            "bad.mon", &failed));
    }
    monad_frontend_state_destroy(first);
    monad_frontend_state_destroy(second);
    puts("frontend registries have deterministic owners");
    return 0;
}
