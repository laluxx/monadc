/*
:TEST-ID tests.embedding.codegen-convergence.production-conditional
:TEST-CONTEXT dec.embedding.live-image dec.embedding.compiler-boundary
:TEST-PURPOSE Prove runtime source uses the production expression generator.
:TEST-ATOM atom.embedding.43.codegen-convergence.production-conditional
:TEST-EXPECT a production-supported conditional and comparison execute through ORC
:TEST-MENU-PATH embedding/branches/codegen-convergence/atom.embedding.43.codegen-convergence.production-conditional
*/
#include <monad/monad.h>

#include <assert.h>
#include <stdio.h>

int main(void) {
    monad_t *monad = NULL;
    monad_error_t error = {0};
    monad_source_result_t *result = NULL;
    monad_i64_function_t classify = NULL;

    assert(monad_open(&monad, &error) == MONAD_OK);
    assert(monad_load_string(
        monad, "memory://production-conditional.mon",
        "define classify :: Int -> Int\n"
        "  value -> if value > 0 then value * 2 else 0\n",
        &result, &error) == MONAD_OK);
    assert(monad_source_result_is_valid(result));
    assert(monad_source_result_is_published(result));
    assert(monad_find_i64_function(monad, "classify", &classify, &error) == MONAD_OK);
    assert(classify(-7) == 0);
    assert(classify(0) == 0);
    assert(classify(9) == 18);

    monad_source_result_destroy(result);
    assert(monad_close(monad, &error) == MONAD_OK);
    puts("production conditional lowered once");
    return 0;
}
