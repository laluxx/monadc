/*
:TEST-ID tests.embedding.introspection.solved-quantities
:TEST-CONTEXT dec.embedding.solved-quantity-snapshots
:TEST-PURPOSE Prove production grade solving reaches immutable installed reports.
:TEST-ATOM atom.embedding.35.introspection.solved-quantities
:TEST-EXPECT used and erased parameters expose exact finite quantities 1 and 0
:TEST-MENU-PATH embedding/branches/introspection/atom.embedding.35.introspection.solved-quantities
*/
#include <monad/monad.h>
#include <monad/qtt.h>

#include <assert.h>
#include <stdio.h>

int main(void) {
    monad_t *monad = NULL;
    monad_error_t error = {0};
    monad_source_result_t *source = NULL;
    assert(monad_open(&monad, &error) == MONAD_OK);
    assert(monad_prepare_string(
        monad, "memory://quantities.mon",
        "define first :: Int -> Int -> Int\n  left right -> left\n",
        &source, &error) == MONAD_OK);
    const monad_qtt_definition_evidence_t *first =
        monad_qtt_report_at(monad_source_result_qtt_report(source), 0);
    assert(first && first->quantitative_usage_available);
    assert(first->parameter_usage_count == 2);
    const monad_qtt_quantity_t *left =
        monad_qtt_definition_parameter_usage(first, 0);
    const monad_qtt_quantity_t *right =
        monad_qtt_definition_parameter_usage(first, 1);
    assert(left && !left->is_omega && left->finite == 1);
    assert(right && !right->is_omega && right->finite == 0);
    monad_source_result_destroy(source);
    assert(monad_close(monad, &error) == MONAD_OK);
    puts("solved QTT quantities preserve used and erased parameters");
    return 0;
}
