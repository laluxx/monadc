/*
:TEST-ID tests.qtt.foreign-llvm.verified-direct-calls
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-QTT-ATOM atom.qtt.12.foreign-llvm.verified-direct-calls
:TEST-QUALITY integration-llvm-verifier
:TEST-SUBSET foreign-llvm
:TEST-CATEGORY 12-verified-direct-calls
:TEST-SECTION qtt
:TEST-CONTEXT monadc.context.embedding.nominal-call-cycle-9,monadc.context.qtt.ownership-anf
:TEST-PURPOSE Certified foreign move/drop/dup emit zero/direct-release/direct-retain LLVM calls.
:TEST-ATOM verify ownership block -> select certified action -> emit exact LLVM instruction
:TEST-EXPECT compile, run, verify module, inspect IR
:TEST-COVERAGE qtt/foreign_llvm.c proof gate, declaration validation, and direct emission
:TEST-USES-ATOM atom.qtt.category.12.verified-direct-calls
:TEST-MENU branches/foreign-llvm/atom.qtt.12.foreign-llvm.verified-direct-calls
:TEST-MENU-PATH qtt/branches/foreign-llvm/atom.qtt.12.foreign-llvm.verified-direct-calls
:TEST-ATOM-LEAF atom.qtt.12.foreign-llvm.verified-direct-calls
:TEST-CLI-TARGET qtt branches foreign-llvm
*/
#include "qtt/foreign_llvm.h"
#include <assert.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <stdio.h>

static QttCoreVar var(uint64_t id) {
    return (QttCoreVar){.module_id = 92, .binder_id = id};
}

static LLVMValueRef function(LLVMModuleRef module, const char *name,
                             LLVMTypeRef pointer, LLVMTypeRef result) {
    LLVMTypeRef parameters[] = {pointer};
    LLVMTypeRef type = LLVMFunctionType(result, parameters, 1, 0);
    return LLVMAddFunction(module, name, type);
}

int main(void) {
    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("foreign", context);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef void_type = LLVMVoidTypeInContext(context);
    LLVMValueRef move = function(module, "foreign_move", pointer, pointer);
    LLVMValueRef drop = function(module, "foreign_drop", pointer, void_type);
    LLVMValueRef dup = function(module, "foreign_dup", pointer, pointer);
    QttResourceOp ops[] = {
        qtt_resource_alloc_typed(var(1), QTT_REP_FOREIGN),
        qtt_resource_dup(var(1), var(2)),
        qtt_resource_drop(var(1)),
        qtt_resource_move(var(2)),
    };
    QttResourceBlock block = {ops, 4};
    QttForeignLlvmEmission emission;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context, move, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);
    assert(qtt_foreign_llvm_emit_certified(module, builder, LLVMGetParam(move, 0),
                                           &block, 3, &emission) == QTT_FOREIGN_LLVM_OK);
    assert(emission.instruction == NULL);
    LLVMBuildRet(builder, emission.value);

    entry = LLVMAppendBasicBlockInContext(context, drop, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);
    assert(qtt_foreign_llvm_emit_certified(module, builder, LLVMGetParam(drop, 0),
                                           &block, 2, &emission) == QTT_FOREIGN_LLVM_OK);
    assert(emission.instruction != NULL);
    LLVMBuildRetVoid(builder);

    entry = LLVMAppendBasicBlockInContext(context, dup, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);
    assert(qtt_foreign_llvm_emit_certified(module, builder, LLVMGetParam(dup, 0),
                                           &block, 1, &emission) == QTT_FOREIGN_LLVM_OK);
    assert(emission.instruction != NULL);
    LLVMBuildRet(builder, emission.value);

    LLVMModuleRef malformed = LLVMModuleCreateWithNameInContext("malformed", context);
    LLVMTypeRef wrong_parameters[] = {LLVMInt32TypeInContext(context)};
    LLVMTypeRef wrong_type = LLVMFunctionType(void_type, wrong_parameters, 1, 0);
    LLVMAddFunction(malformed, "monad_foreign_object_release", wrong_type);
    assert(qtt_foreign_llvm_emit_certified(malformed, builder, LLVMGetParam(drop, 0),
                                           &block, 2, &emission) ==
           QTT_FOREIGN_LLVM_DECLARATION_MISMATCH);

    char *error = NULL;
    assert(LLVMVerifyModule(module, LLVMReturnStatusAction, &error) == 0);
    LLVMDisposeMessage(error);
    char *ir = LLVMPrintModuleToString(module);
    fputs(ir, stdout);
    LLVMDisposeMessage(ir);
    puts("verified foreign llvm emission ok");
    LLVMDisposeModule(malformed);
    LLVMDisposeBuilder(builder);
    LLVMDisposeModule(module);
    LLVMContextDispose(context);
    return 0;
}
