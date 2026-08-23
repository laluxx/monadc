#include "foreign_llvm.h"

/* A certified ownership action becomes an ordinary direct LLVM call.  The
 * relevant LLVM call semantics are specified here:
 * https://llvm.org/docs/LangRef.html#call-instruction
 * LLVM's C construction API is documented here:
 * https://llvm.org/doxygen/group__LLVMCCoreInstructionBuilder.html
 */

static int is_runtime_function_type(LLVMTypeRef type, LLVMTypeRef expected) {
    if (!type || LLVMGetTypeKind(type) != LLVMFunctionTypeKind ||
        LLVMIsFunctionVarArg(type) || LLVMCountParamTypes(type) != 1 ||
        LLVMGetTypeKind(LLVMGetReturnType(type)) != LLVMVoidTypeKind)
        return 0;
    LLVMTypeRef parameter = NULL;
    LLVMGetParamTypes(type, &parameter);
    return parameter == expected;
}

static LLVMValueRef runtime_function(LLVMModuleRef module, const char *symbol,
                                     LLVMTypeRef expected, int *mismatch) {
    LLVMValueRef function = LLVMGetNamedFunction(module, symbol);
    *mismatch = 0;
    if (!function) return LLVMAddFunction(module, symbol, expected);
    LLVMTypeRef parameter = NULL;
    LLVMGetParamTypes(expected, &parameter);
    if (!is_runtime_function_type(LLVMGlobalGetValueType(function), parameter)) {
        *mismatch = 1;
        return NULL;
    }
    return function;
}

QttForeignLlvmError qtt_foreign_llvm_emit_certified(
    LLVMModuleRef module, LLVMBuilderRef builder, LLVMValueRef object,
    const QttResourceBlock *block, size_t operation_index,
    QttForeignLlvmEmission *emission) {
    if (emission) *emission = (QttForeignLlvmEmission){0};
    if (!module || !builder || !object || !emission ||
        LLVMGetTypeKind(LLVMTypeOf(object)) != LLVMPointerTypeKind)
        return QTT_FOREIGN_LLVM_INVALID_ARGUMENT;

    QttForeignLowering lowering;
    if (qtt_foreign_lower_certified(block, operation_index, &lowering) !=
        QTT_FOREIGN_LOWER_OK)
        return QTT_FOREIGN_LLVM_INVALID_PROOF;
    emission->action = lowering.action;
    emission->value = object;
    if (lowering.action == QTT_FOREIGN_RUNTIME_NONE) return QTT_FOREIGN_LLVM_OK;

    LLVMContextRef context = LLVMGetModuleContext(module);
    LLVMTypeRef parameter[] = {LLVMPointerTypeInContext(context, 0)};
    LLVMTypeRef function_type = LLVMFunctionType(
        LLVMVoidTypeInContext(context), parameter, 1, 0);
    int mismatch = 0;
    LLVMValueRef function = runtime_function(
        module, lowering.symbol, function_type, &mismatch);
    if (mismatch) return QTT_FOREIGN_LLVM_DECLARATION_MISMATCH;
    LLVMValueRef argument[] = {object};
    emission->instruction = LLVMBuildCall2(
        builder, LLVMGlobalGetValueType(function), function, argument, 1, "");
    return QTT_FOREIGN_LLVM_OK;
}
