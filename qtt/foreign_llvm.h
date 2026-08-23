#ifndef MONAD_QTT_FOREIGN_LLVM_H
#define MONAD_QTT_FOREIGN_LLVM_H

#include "foreign_lowering.h"
#include <llvm-c/Core.h>

typedef enum {
    QTT_FOREIGN_LLVM_OK,
    QTT_FOREIGN_LLVM_INVALID_ARGUMENT,
    QTT_FOREIGN_LLVM_INVALID_PROOF,
    QTT_FOREIGN_LLVM_DECLARATION_MISMATCH,
} QttForeignLlvmError;

typedef struct {
    QttForeignRuntimeAction action;
    LLVMValueRef value;
    LLVMValueRef instruction;
} QttForeignLlvmEmission;

QttForeignLlvmError qtt_foreign_llvm_emit_certified(
    LLVMModuleRef module, LLVMBuilderRef builder, LLVMValueRef object,
    const QttResourceBlock *block, size_t operation_index,
    QttForeignLlvmEmission *emission);

#endif
