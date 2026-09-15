/*
:TEST-ID tests.embedding.introspection.resource-certificate
:TEST-CONTEXT dec.embedding.typed-core-resource-certificate
:TEST-PURPOSE Prove only verified typed Core advances public QTT evidence stage.
:TEST-ATOM atom.embedding.36.introspection.resource-certificate
:TEST-EXPECT identity has validated immediate representation and destructor plan
:TEST-MENU-PATH embedding/branches/introspection/atom.embedding.36.introspection.resource-certificate
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
        monad, "memory://resource.mon",
        "define identity :: Int -> Int\n  value -> value\n"
        "define staged :: Int -> Int\n"
        "  value ->\n"
        "    def copy value\n"
        "    copy\n",
        &source, &error) == MONAD_OK);
    const monad_qtt_definition_evidence_t *identity =
        monad_qtt_report_at(monad_source_result_qtt_report(source), 0);
    assert(identity);
    assert(identity->stage == MONAD_QTT_EVIDENCE_RESOURCE_VERIFIED);
    assert(identity->resource_verified);
    assert(identity->destructor_plan_available);
    assert(identity->result_representation == MONAD_QTT_REP_IMMEDIATE);
    assert(identity->resource_block_count == 1);
    assert(identity->resource_certificate_fingerprint != 0);
    const monad_qtt_definition_evidence_t *staged =
        monad_qtt_report_at(monad_source_result_qtt_report(source), 1);
    assert(staged);
    assert(staged->stage == MONAD_QTT_EVIDENCE_INFERRED);
    assert(!staged->resource_verified);
    assert(!staged->destructor_plan_available);
    monad_source_result_destroy(source);
    assert(monad_close(monad, &error) == MONAD_OK);
    puts("typed Core resource certificate is explicit and owned");
    return 0;
}
