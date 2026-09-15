/*
:TEST-ID tests.embedding.frontend-state.compilation-unit-types
:TEST-CONTEXT dec.embedding.compilation-unit-type-state
:TEST-PURPOSE Prove aliases, refinements, and finite sets follow compiler state.
:TEST-ATOM atom.embedding.26.frontend-state.compilation-unit-types
:TEST-EXPECT alternating states remain disjoint across diagnostic unwind
:TEST-MENU-PATH embedding/branches/frontend-state/atom.embedding.26.frontend-state.compilation-unit-types
*/
#include "embed/frontend_transaction.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    MonadFrontendState *first = monad_frontend_state_create();
    MonadFrontendState *second = monad_frontend_state_create();
    assert(first && second);
    assert(monad_frontend_state_register_alias(first, "Meters", "Int"));
    assert(monad_frontend_state_register_refinement(
        first, "Positive", "positive?", "Int"));
    assert(monad_frontend_state_register_finite_member(
        first, "TrafficLight", "green"));

    for (int i = 0; i < 32; i++) {
        assert(monad_frontend_state_alias_targets(first, "Meters", "Int"));
        assert(!monad_frontend_state_alias_targets(second, "Meters", "Int"));
        assert(monad_frontend_state_has_refinement(first, "Positive", "positive?"));
        assert(!monad_frontend_state_has_refinement(second, "Positive", "positive?"));
        assert(monad_frontend_state_finite_contains(first, "TrafficLight", "green"));
        assert(!monad_frontend_state_finite_contains(second, "TrafficLight", "green"));
        MonadFrontendResult failed;
        assert(!monad_frontend_parse_in_state(
            first, "define bad :: Int -> Int\n  value -> [value\n",
            "bad.mon", &failed));
    }
    monad_frontend_state_destroy(first);
    monad_frontend_state_destroy(second);
    puts("compilation-unit type registries have deterministic owners");
    return 0;
}
