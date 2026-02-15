#ifndef CODEGEN_H
#define CODEGEN_H

#include <llvm-c/Core.h>
#include "reader.h"
#include "types.h"
#include "env.h"

typedef struct {
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMContextRef context;
    Env *env;
    // Cached format strings
    LLVMValueRef fmt_str;
    LLVMValueRef fmt_char;
    LLVMValueRef fmt_int;
    LLVMValueRef fmt_float;
    LLVMValueRef fmt_hex;
    LLVMValueRef fmt_bin;
    LLVMValueRef fmt_oct;
} CodegenContext;

typedef struct {
    LLVMValueRef value;
    Type *type;
} CodegenResult;

void codegen_init(CodegenContext *ctx, const char *module_name);
void codegen_dispose(CodegenContext *ctx);

// Format string getters
LLVMValueRef get_fmt_str(CodegenContext *ctx);
LLVMValueRef get_fmt_char(CodegenContext *ctx);
LLVMValueRef get_fmt_int(CodegenContext *ctx);
LLVMValueRef get_fmt_float(CodegenContext *ctx);
LLVMValueRef get_fmt_hex(CodegenContext *ctx);
LLVMValueRef get_fmt_oct(CodegenContext *ctx);
LLVMValueRef get_fmt_str_no_newline(CodegenContext *ctx);
LLVMValueRef get_fmt_char_no_newline(CodegenContext *ctx);
LLVMValueRef get_fmt_int_no_newline(CodegenContext *ctx);
LLVMValueRef get_fmt_float_no_newline(CodegenContext *ctx);

// Runtime function declarations
LLVMValueRef get_or_declare_printf(CodegenContext *ctx);
LLVMValueRef get_or_declare_print_binary(CodegenContext *ctx);

// Type conversion
LLVMTypeRef type_to_llvm(CodegenContext *ctx, Type *type);

// Type checking
bool type_is_numeric(Type *t);
bool type_is_integer(Type *t);
bool type_is_float(Type *t);

// AST printing at compile time (for quoted expressions)
void codegen_print_ast(CodegenContext *ctx, AST *ast);

// Main codegen function
CodegenResult codegen_expr(CodegenContext *ctx, AST *ast);

// Builtin registration
void register_builtins(CodegenContext *ctx);

#endif // CODEGEN_H
