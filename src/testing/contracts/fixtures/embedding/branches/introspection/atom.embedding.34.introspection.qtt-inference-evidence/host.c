/*
:TEST-ID tests.embedding.introspection.qtt-inference-evidence
:TEST-CONTEXT dec.embedding.qtt-evidence-staging
:TEST-PURPOSE Expose only QTT judgments production inference actually established.
:TEST-ATOM atom.embedding.34.introspection.qtt-inference-evidence
:TEST-EXPECT generalized usage/effects are owned; resource proof remains unavailable
:TEST-MENU-PATH embedding/branches/introspection/atom.embedding.34.introspection.qtt-inference-evidence
*/
#include <monad/monad.h>
#include <monad/qtt.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    monad_t *monad = NULL;
    monad_error_t error = {0};
    monad_source_result_t *source = NULL;
    assert(monad_open(&monad, &error) == MONAD_OK);
    assert(monad_prepare_string(
        monad, "memory://qtt.mon",
        "define identity :: Int -> Int\n  value -> value\n"
        "define first :: Int -> Int -> Int\n  left right -> left\n",
        &source, &error) == MONAD_OK);
    assert(monad_source_result_is_valid(source));

    const monad_qtt_report_t *report = monad_source_result_qtt_report(source);
    assert(report && monad_qtt_report_count(report) == 2);
    const monad_qtt_definition_evidence_t *identity =
        monad_qtt_report_at(report, 0);
    assert(identity && strcmp(identity->name, "identity") == 0);
    assert(monad_qtt_report_find_id(report, identity->definition_id) == identity);
    assert(identity->stage >= MONAD_QTT_EVIDENCE_INFERRED);
    assert(identity->quantitative_usage_available);
    assert(identity->parameter_usage_count == 1);
    const monad_qtt_quantity_t *usage =
        monad_qtt_definition_parameter_usage(identity, 0);
    assert(usage && !usage->is_omega && usage->finite == 1);
    assert(!monad_qtt_definition_parameter_usage(identity, 1));
    assert(identity->effects_complete);
    assert(identity->effect_fingerprint != 0);
    assert(identity->portable_effect_contract);

    const monad_qtt_definition_evidence_t *first =
        monad_qtt_report_at(report, 1);
    assert(first && strcmp(first->name, "first") == 0);
    assert(first->parameter_usage_count == 2);
    const monad_qtt_quantity_t *left =
        monad_qtt_definition_parameter_usage(first, 0);
    const monad_qtt_quantity_t *right =
        monad_qtt_definition_parameter_usage(first, 1);
    assert(left && left->finite == 1 && !left->is_omega);
    assert(right && right->finite == 0 && !right->is_omega);

    monad_source_result_destroy(source);
    assert(monad_close(monad, &error) == MONAD_OK);
    puts("QTT report distinguishes inference from resource proof");
    return 0;
}
