/*
:TEST-ID tests.qtt.foreign-call.nominal-transfer
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-QTT-ATOM atom.qtt.09.foreign-call.nominal-transfer
:TEST-QUALITY sanitizer
:TEST-SUBSET foreign-call
:TEST-CATEGORY 09-nominal-transfer
:TEST-SECTION qtt
:TEST-CONTEXT monadc.context.embedding.foreign-nominal-authority,monadc.context.qtt.call-contract
:TEST-PURPOSE Structurally identical foreign values require equal nominal authority and QTT-proven move/share transfer.
:TEST-ATOM two authorities + one structural TypeId -> reject confusion -> consumed moves -> shared borrows
:TEST-EXPECT compile, run
:TEST-COVERAGE qtt/call.c foreign nominal equality, consumed move, shared borrow
:TEST-USES-ATOM atom.qtt.category.09.nominal-transfer
:TEST-MENU branches/foreign-call/atom.qtt.09.foreign-call.nominal-transfer
:TEST-MENU-PATH qtt/branches/foreign-call/atom.qtt.09.foreign-call.nominal-transfer
:TEST-ATOM-LEAF atom.qtt.09.foreign-call.nominal-transfer
:TEST-CLI-TARGET qtt branches foreign-call
*/
#include "qtt/call.h"
#include "qtt/foreign_type.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    Type byte = {.kind = TYPE_BYTE};
    Type pointer = {.kind = TYPE_PTR, .element_type = &byte};
    QttTypeArena *types = qtt_type_arena_new();
    QttNominalAuthority buffer = {11, 1};
    QttNominalAuthority image = {11, 2};
    QttForeignTypeEnv *foreign_types = qtt_foreign_type_env_new(types);
    const QttForeignType *buffer_type;
    const QttForeignType *image_type;
    assert(foreign_types);
    assert(qtt_foreign_type_import(
        foreign_types, "EditorBuffer", buffer, &pointer,
        QTT_OWNERSHIP_CONSUMED, &buffer_type) == QTT_FOREIGN_TYPE_OK);
    assert(qtt_foreign_type_import(
        foreign_types, "VulkanImage", image, &pointer,
        QTT_OWNERSHIP_CONSUMED, &image_type) == QTT_FOREIGN_TYPE_OK);
    assert(qtt_type_id_equal(buffer_type->type_id, image_type->type_id));
    assert(!qtt_nominal_authority_equal(
        buffer_type->authority, image_type->authority));
    QttParameterContract parameter = {
        .mode = buffer_type->ownership,
        .type = buffer_type->type,
        .type_id = buffer_type->type_id,
        .representation = buffer_type->representation,
        .nominal_authority = buffer_type->authority,
    };
    QttFunctionSignature signature = {
        .parameters = &parameter,
        .parameter_count = 1,
        .result_type = &byte,
    };
    QttCallArgument argument = {
        .type = &pointer,
        .type_id = image_type->type_id,
        .representation = QTT_REP_FOREIGN,
        .transfer = QTT_CALL_MOVE,
        .nominal_authority = image,
    };
    QttCallPlan plan;

    assert(qtt_call_plan(&signature, &argument, 1, &plan) ==
           QTT_CALL_NOMINAL_MISMATCH);
    argument.nominal_authority = buffer;
    assert(qtt_call_plan(&signature, &argument, 1, &plan) == QTT_CALL_VALID);
    qtt_call_plan_free(&plan);

    parameter.mode = QTT_OWNERSHIP_SHARED;
    argument.transfer = QTT_CALL_BORROW;
    assert(qtt_call_plan(&signature, &argument, 1, &plan) == QTT_CALL_VALID);
    qtt_call_plan_free(&plan);
    argument.transfer = QTT_CALL_MOVE;
    assert(qtt_call_plan(&signature, &argument, 1, &plan) ==
           QTT_CALL_TRANSFER_MISMATCH);

    qtt_foreign_type_env_free(foreign_types);
    qtt_type_arena_free(types);
    puts("nominal foreign transfer ok");
    return 0;
}
