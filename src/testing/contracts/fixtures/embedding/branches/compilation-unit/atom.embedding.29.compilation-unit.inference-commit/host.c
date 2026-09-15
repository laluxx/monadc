/*
:TEST-ID tests.embedding.compilation-unit.inference-commit
:TEST-CONTEXT dec.embedding.transactional-inference-commit
:TEST-PURPOSE Prove W-by-W definitions publish only after whole-unit success.
:TEST-ATOM atom.embedding.29.compilation-unit.inference-commit
:TEST-EXPECT later expressions see provisional bindings; failed units publish none
:TEST-MENU-PATH embedding/branches/compilation-unit/atom.embedding.29.compilation-unit.inference-commit
*/
#include "embed/frontend_transaction.h"

#include <assert.h>
#include <stdio.h>

static MonadCompilationUnit *parse_unit(
    MonadFrontendState *state, const char *source, MonadFrontendResult *result) {
    MonadCompilationUnit *unit = monad_compilation_unit_begin(state);
    assert(unit);
    assert(monad_compilation_unit_parse(unit, source, "transaction.mon", result));
    return unit;
}

int main(void) {
    MonadFrontendState *state = monad_frontend_state_create();
    assert(state);
    MonadFrontendResult result;

    MonadCompilationUnit *valid = parse_unit(state,
        "define identity :: Int -> Int\n"
        "  value -> value\n"
        "identity 7\n", &result);
    assert(monad_compilation_unit_infer(valid, &result));
    assert(monad_compilation_unit_inference_committed(valid));
    assert(monad_compilation_unit_has_committed_binding(valid, "identity"));
    monad_compilation_unit_destroy(valid);

    MonadCompilationUnit *failed = parse_unit(state,
        "define staged :: Int -> Int\n"
        "  value -> value\n"
        "[1 \"bad\"]\n", &result);
    assert(!monad_compilation_unit_infer(failed, &result));
    assert(result.has_diagnostic);
    assert(!monad_compilation_unit_inference_committed(failed));
    assert(!monad_compilation_unit_has_committed_binding(failed, "staged"));
    monad_compilation_unit_destroy(failed);

    MonadCompilationUnit *fresh = parse_unit(state, "staged 1\n", &result);
    assert(monad_compilation_unit_infer(fresh, &result));
    assert(!monad_compilation_unit_has_committed_binding(fresh, "staged"));
    monad_compilation_unit_destroy(fresh);

    monad_frontend_state_destroy(state);
    puts("top-level inference commits atomically");
    return 0;
}
