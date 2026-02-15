#ifndef ASM_H
#define ASM_H

#include "reader.h"
#include "types.h"
#include "env.h"
#include <llvm-c/Core.h>

// Register allocation for System V AMD64 ABI
typedef struct {
    const char *int_regs[6];   // %rdi, %rsi, %rdx, %rcx, %r8, %r9
    const char *float_regs[8]; // %xmm0-%xmm7
    int int_count;
    int float_count;
} RegisterAllocator;

// Initialize register allocator
void reg_alloc_init(RegisterAllocator *ra);

// Get register for parameter by index and type
const char *reg_alloc_get(RegisterAllocator *ra, int param_index, Type *param_type);

// Assembly instruction representation (after preprocessing)
typedef struct {
    char *mnemonic;     // e.g., "add", "mov"
    char **operands;    // e.g., ["%rdi", "%rsi"]
    int operand_count;
} AsmInstruction;

// Context for asm preprocessing
typedef struct {
    EnvParam *params; // function parameters
    int param_count;
    RegisterAllocator *reg_alloc;
} AsmContext;

// Preprocess AST_ASM node:
// - Replace parameter names with registers
// - Convert AST instructions to AsmInstruction array
AsmInstruction *preprocess_asm(AST *asm_node, AsmContext *ctx, int *out_count);

// Free preprocessed instructions
void free_asm_instructions(AsmInstruction *instructions, int count);

// Generate LLVM inline assembly from preprocessed instructions
LLVMValueRef codegen_inline_asm(LLVMContextRef context,
                                 LLVMBuilderRef builder,
                                 AsmInstruction *instructions,
                                 int instruction_count,
                                 Type *return_type,
                                 LLVMValueRef *params,
                                 int param_count);

#endif
