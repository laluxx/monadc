/*
:TEST-ID tests.embedding.compilation-unit.continuous-state
:TEST-CONTEXT dec.embedding.continuous-compilation-unit
:TEST-PURPOSE Prove semantic state and AST share one explicit unit lifetime.
:TEST-ATOM atom.embedding.27.compilation-unit.continuous-state
:TEST-EXPECT success and rejection retain binding until deterministic destruction
:TEST-MENU-PATH embedding/branches/compilation-unit/atom.embedding.27.compilation-unit.continuous-state
*/
#include "embed/frontend_transaction.h"
#include "types.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    MonadFrontendState *first = monad_frontend_state_create();
    MonadFrontendState *second = monad_frontend_state_create();
    assert(first && second);
    assert(monad_frontend_state_register_alias(first, "Meters", "Int"));

    MonadCompilationUnit *valid = monad_compilation_unit_begin(first);
    assert(valid);
    MonadFrontendResult result;
    assert(monad_compilation_unit_parse(
        valid, "define identity :: Int -> Int\n  value -> value\n",
        "valid.mon", &result));
    assert(monad_compilation_unit_ast_count(valid) > 0);
    assert(type_name_is_subtype("Meters", "Int"));
    assert(!monad_frontend_state_alias_targets(second, "Meters", "Int"));
    monad_compilation_unit_destroy(valid);
    assert(!type_name_is_subtype("Meters", "Int"));
    assert(monad_frontend_state_alias_targets(first, "Meters", "Int"));

    MonadCompilationUnit *invalid = monad_compilation_unit_begin(first);
    assert(invalid);
    assert(!monad_compilation_unit_parse(
        invalid, "define broken :: Int -> Int\n  value -> [value\n",
        "invalid.mon", &result));
    assert(result.has_diagnostic);
    assert(type_name_is_subtype("Meters", "Int"));
    monad_compilation_unit_destroy(invalid);
    assert(!type_name_is_subtype("Meters", "Int"));

    monad_frontend_state_destroy(first);
    monad_frontend_state_destroy(second);
    puts("compilation unit owns continuous semantic state");
    return 0;
}
