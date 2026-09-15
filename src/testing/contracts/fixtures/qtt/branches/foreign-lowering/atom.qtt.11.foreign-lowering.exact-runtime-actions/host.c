/*
:TEST-ID tests.qtt.foreign-lowering.exact-runtime-actions
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-QTT-ATOM atom.qtt.11.foreign-lowering.exact-runtime-actions
:TEST-QUALITY integration-assembly
:TEST-SUBSET foreign-lowering
:TEST-CATEGORY 11-exact-runtime-actions
:TEST-SECTION qtt
:TEST-CONTEXT monadc.context.embedding.nominal-call-cycle-9,monadc.context.qtt.ownership-anf
:TEST-PURPOSE Certified foreign move/drop/dup lower to zero/release/retain runtime actions.
:TEST-ATOM verify ownership block -> backend selects exact action -> assembly contains exact call count
:TEST-EXPECT compile, run, inspect assembly
:TEST-COVERAGE qtt/foreign_lowering.c resource proof gate and runtime symbol selection
:TEST-USES-ATOM atom.qtt.category.11.exact-runtime-actions
:TEST-MENU branches/foreign-lowering/atom.qtt.11.foreign-lowering.exact-runtime-actions
:TEST-MENU-PATH qtt/branches/foreign-lowering/atom.qtt.11.foreign-lowering.exact-runtime-actions
:TEST-ATOM-LEAF atom.qtt.11.foreign-lowering.exact-runtime-actions
:TEST-CLI-TARGET qtt branches foreign-lowering
*/
#include "qtt/foreign_lowering.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static QttCoreVar var(uint64_t id) {
    return (QttCoreVar){.module_id = 91, .binder_id = id};
}

int main(void) {
    QttResourceOp ops[] = {
        qtt_resource_alloc_typed(var(1), QTT_REP_FOREIGN),
        qtt_resource_dup(var(1), var(2)),
        qtt_resource_drop(var(1)),
        qtt_resource_move(var(2)),
    };
    QttResourceBlock block = {ops, 4};
    QttForeignLowering lowering;
    QttHeapExecution execution;
    assert(qtt_resource_verify(&block, NULL, 0).error == QTT_RESOURCE_VALID);
    execution = qtt_resource_execute(&block, NULL, 0);
    assert(execution.error == QTT_RESOURCE_VALID);
    assert(execution.retains == 1 && execution.releases == 1);
    assert(execution.moved_out == 1 && execution.live_owned == 0);
    assert(qtt_foreign_lower_certified(&block, 1, &lowering) ==
           QTT_FOREIGN_LOWER_OK);
    assert(lowering.action == QTT_FOREIGN_RUNTIME_RETAIN_SHARED);
    assert(strcmp(lowering.symbol,
                  "monad_foreign_object_retain_shared") == 0);
    assert(qtt_foreign_lower_certified(&block, 2, &lowering) ==
           QTT_FOREIGN_LOWER_OK);
    assert(lowering.action == QTT_FOREIGN_RUNTIME_RELEASE);
    assert(qtt_foreign_lower_certified(&block, 3, &lowering) ==
           QTT_FOREIGN_LOWER_OK);
    assert(lowering.action == QTT_FOREIGN_RUNTIME_NONE);
    puts("exact foreign lowering ok");
    return 0;
}
