/*
:TEST-ID tests.embedding.foreign-execution.orc-ownership
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.14.foreign-execution.orc-ownership
:TEST-QUALITY end-to-end-native
:TEST-SUBSET foreign-execution
:TEST-CATEGORY 14-orc-ownership
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.foreign-llvm-direct-calls,monadc.context.embedding.qtt-foreign-import
:TEST-PURPOSE A C-created foreign object crosses compiler import, QTT proof, LLVM emission, ORCv2, and native execution.
:TEST-ATOM C object -> compiler nominal import -> certified DUP/DROP/MOVE -> ORC call -> deterministic destructor
:TEST-EXPECT compile, verify-ir, jit, run
:TEST-COVERAGE runtime foreign object, compiler import, QTT resource proof, LLVM emitter, ORCv2 process symbols
:TEST-USES-ATOM atom.embedding.category.14.orc-ownership
:TEST-MENU branches/foreign-execution/atom.embedding.14.foreign-execution.orc-ownership
:TEST-MENU-PATH embedding/branches/foreign-execution/atom.embedding.14.foreign-execution.orc-ownership
:TEST-ATOM-LEAF atom.embedding.14.foreign-execution.orc-ownership
:TEST-CLI-TARGET embedding branches foreign-execution
*/
#include <monad/compiler.h>
#include "qtt/foreign_llvm.h"

#include <assert.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Target.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ORCv2 module/context ownership and eager lookup semantics:
 * https://llvm.org/docs/ORCv2.html
 * C LLJIT ownership contract:
 * https://llvm.org/doxygen/group__LLVMCExecutionEngineLLJIT.html
 */

typedef monad_foreign_object_t *(*ownership_fn)(monad_foreign_object_t *);

static QttCoreVar var(uint64_t id) {
    return (QttCoreVar){.module_id = 94, .binder_id = id};
}

static void destroy_payload(void *payload, void *context) {
    int *destructions = context;
    ++*destructions;
    free(payload);
}

static size_t occurrences(const char *text, const char *needle) {
    size_t count = 0;
    while ((text = strstr(text, needle)) != NULL) {
        ++count;
        text += strlen(needle);
    }
    return count;
}

int main(void) {
    monad_runtime_config_t config;
    monad_runtime_t *runtime;
    monad_thread_t *thread;
    monad_foreign_type_descriptor_t descriptor;
    monad_foreign_type_t *type;
    monad_foreign_object_t *object;
    monad_binding_filter_t filter;
    monad_binding_snapshot_t *snapshot;
    monad_compiler_session_t *compiler;
    monad_error_t error;
    int destructions = 0;
    int *payload = malloc(sizeof(*payload));
    assert(payload);
    *payload = 42;

    monad_runtime_config_init(&config);
    assert(monad_runtime_create(&config, &runtime, &error) == MONAD_OK);
    assert(monad_thread_attach(runtime, &thread, &error) == MONAD_OK);
    monad_foreign_type_descriptor_init(&descriptor);
    descriptor.name = "EditorBuffer";
    descriptor.ownership = MONAD_FOREIGN_SHARED;
    descriptor.destroy = destroy_payload;
    descriptor.destroy_context = &destructions;
    assert(monad_runtime_register_foreign_type(
        thread, &descriptor, &type, &error) == MONAD_OK);
    assert(monad_foreign_object_create(type, payload, &object, &error) == MONAD_OK);
    monad_binding_filter_init(&filter);
    filter.kind_mask = MONAD_BINDING_MASK_FOREIGN_TYPE;
    assert(monad_runtime_snapshot_bindings(
        thread, &filter, &snapshot, &error) == MONAD_OK);
    assert(monad_compiler_session_create(&compiler, &error) == MONAD_OK);
    assert(monad_compiler_import_foreign_types(compiler, snapshot, &error) == MONAD_OK);
    const monad_compiler_foreign_type_info_t *info =
        monad_compiler_foreign_type_at(compiler, 0);
    assert(info && info->ownership == MONAD_COMPILER_OWNERSHIP_SHARED);
    assert(monad_foreign_type_identity_equal(
        info->nominal_identity,
        monad_binding_snapshot_at(snapshot, 0)->foreign_type_identity));

    QttResourceOp ops[] = {
        qtt_resource_alloc_typed(var(1), QTT_REP_FOREIGN),
        qtt_resource_dup(var(1), var(2)),
        qtt_resource_drop(var(1)),
        qtt_resource_move(var(2)),
    };
    QttResourceBlock block = {ops, 4};
    assert(qtt_resource_verify(&block, NULL, 0).error == QTT_RESOURCE_VALID);

    assert(LLVMInitializeNativeTarget() == 0);
    assert(LLVMInitializeNativeAsmPrinter() == 0);
    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("foreign-cycle", context);
    LLVMBuilderRef ir_builder = LLVMCreateBuilderInContext(context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef parameters[] = {pointer};
    LLVMTypeRef function_type = LLVMFunctionType(pointer, parameters, 1, 0);
    LLVMValueRef function = LLVMAddFunction(module, "monad_cycle14", function_type);
    LLVMSetLinkage(function, LLVMExternalLinkage);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context, function, "entry");
    LLVMPositionBuilderAtEnd(ir_builder, entry);
    QttForeignLlvmEmission emission;
    assert(qtt_foreign_llvm_emit_certified(module, ir_builder,
        LLVMGetParam(function, 0), &block, 1, &emission) == QTT_FOREIGN_LLVM_OK);
    assert(qtt_foreign_llvm_emit_certified(module, ir_builder,
        LLVMGetParam(function, 0), &block, 2, &emission) == QTT_FOREIGN_LLVM_OK);
    assert(qtt_foreign_llvm_emit_certified(module, ir_builder,
        LLVMGetParam(function, 0), &block, 3, &emission) == QTT_FOREIGN_LLVM_OK);
    assert(emission.instruction == NULL && emission.value == LLVMGetParam(function, 0));
    LLVMBuildRet(ir_builder, emission.value);
    char *verify_error = NULL;
    assert(LLVMVerifyModule(module, LLVMReturnStatusAction, &verify_error) == 0);
    LLVMDisposeMessage(verify_error);
    char *ir = LLVMPrintModuleToString(module);
    assert(occurrences(ir, "call void @monad_foreign_object_retain_shared") == 1);
    assert(occurrences(ir, "call void @monad_foreign_object_release") == 1);
    LLVMDisposeMessage(ir);
    LLVMDisposeBuilder(ir_builder);

    LLVMOrcLLJITRef jit = NULL;
    assert(LLVMOrcCreateLLJIT(&jit, LLVMOrcCreateLLJITBuilder()) == NULL);
    LLVMOrcJITDylibRef dylib = LLVMOrcLLJITGetMainJITDylib(jit);
    LLVMOrcDefinitionGeneratorRef generator = NULL;
    assert(LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(
        &generator, LLVMOrcLLJITGetGlobalPrefix(jit), NULL, NULL) == NULL);
    LLVMOrcJITDylibAddGenerator(dylib, generator);
    LLVMSetDataLayout(module, LLVMOrcLLJITGetDataLayoutStr(jit));
    LLVMOrcThreadSafeContextRef safe_context =
        LLVMOrcCreateNewThreadSafeContextFromLLVMContext(context);
    LLVMOrcThreadSafeModuleRef safe_module =
        LLVMOrcCreateNewThreadSafeModule(module, safe_context);
    assert(LLVMOrcLLJITAddLLVMIRModule(jit, dylib, safe_module) == NULL);
    LLVMOrcDisposeThreadSafeContext(safe_context);
    LLVMOrcExecutorAddress address = 0;
    assert(LLVMOrcLLJITLookup(jit, &address, "monad_cycle14") == NULL);
    ownership_fn call = NULL;
    assert(sizeof(call) == sizeof(address));
    memcpy(&call, &address, sizeof(call));
    monad_foreign_object_t *returned = call(object);
    assert(returned == object && destructions == 0);
    void *observed_payload = NULL;
    assert(monad_foreign_object_payload(
        returned, type, &observed_payload, &error) == MONAD_OK);
    assert(*(int *)observed_payload == 42);
    monad_foreign_object_release(returned);
    assert(destructions == 1);
    assert(LLVMOrcDisposeLLJIT(jit) == NULL);

    monad_compiler_session_destroy(compiler);
    monad_binding_snapshot_release(snapshot);
    assert(monad_foreign_type_release(type, &error) == MONAD_OK);
    assert(monad_thread_detach(thread, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    puts("certified foreign ORC execution ok");
    return 0;
}
