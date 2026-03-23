#ifndef CODEGEN_H
#define CODEGEN_H

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <setjmp.h>
#include "reader.h"
#include "types.h"
#include "env.h"

// Forward declaration
typedef struct ModuleContext ModuleContext;

/// Algebraic Data Types

int  adt_constructor_tag(const char *name);

/// Monomorphization cache
//
// Maps (function_name, concrete_arg_types) -> specialized LLVMValueRef.
// When a polymorphic function is called with concrete types, we either
// find an existing specialization or generate a new one.
//
typedef struct MonoKey {
    char   *fn_name;        // original function name
    Type  **type_args;      // concrete types for each type variable
    int     type_arg_count;
} MonoKey;

typedef struct MonoCacheEntry {
    MonoKey       key;
    LLVMValueRef  fn;               // specialized LLVM function
    char         *specialized_name; // e.g. "id_Int", "id_String"
} MonoCacheEntry;

typedef struct MonoCache {
    MonoCacheEntry *entries;
    int             count;
    int             capacity;
} MonoCache;


typedef struct CodegenContext {
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMContextRef context;
    Env *env;
    ModuleContext *module_ctx;  // Module system context
    bool is_top_level;          // Are we generating top-level (global init) code?
    LLVMValueRef init_fn;       // The __module_init_<name> function for side-effects
    // Cached format strings
    LLVMValueRef fmt_str;
    LLVMValueRef fmt_char;
    LLVMValueRef fmt_int;
    LLVMValueRef fmt_float;
    LLVMValueRef fmt_hex;
    LLVMValueRef fmt_bin;
    LLVMValueRef fmt_oct;
    bool test_mode;
    const char *current_function_name;  // NULL at top level, set when inside a define

    // Monomorphization cache
    MonoCache mono_cache;
    struct TypeClassRegistry *tc_registry;

    /* Error recovery — set by CODEGEN_ERROR, caught by codegen_expr callers */
    jmp_buf  error_jmp;
    bool     error_jmp_set;   // true while a recovery point is active
    struct FFIContext *ffi;   /* NULL until first (include ...) */
    char     error_msg[512];  // last error message, for display
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

LLVMTypeRef type_to_llvm(CodegenContext *ctx, Type *t);
bool type_is_numeric(Type *t);
bool type_is_integer(Type *t);
bool type_is_float(Type *t);

void codegen_print_ast(CodegenContext *ctx, AST *ast);
EnvParam *clone_params(EnvParam *src, int count);
CodegenResult codegen_expr(CodegenContext *ctx, AST *ast);
void register_builtins(CodegenContext *ctx);

// Declare an external symbol (from another module) into this module's env.
// Creates an LLVMAddGlobal / LLVMAddFunction declaration and inserts it.
void codegen_declare_external_var(CodegenContext *ctx,
                                  const char *mangled_name,
                                  Type *type);

void codegen_declare_external_func(CodegenContext *ctx,
                                   const char *mangled_name,
                                   EnvParam *params, int param_count,
                                   Type *return_type);

// Build and register an LLVM struct type from an AST_LAYOUT node.
// Resolves field types, computes offsets, calls layout_register.
void codegen_layout(CodegenContext *ctx, AST *ast);

char *mangle_unicode_name(const char *name);


/// Fat arrays

LLVMValueRef arr_fat_data(CodegenContext *ctx, LLVMValueRef fat_ptr, Type *elem_type);
LLVMValueRef arr_fat_size(CodegenContext *ctx, LLVMValueRef fat_ptr);

/// Monomorphization API
LLVMValueRef mono_cache_lookup(MonoCache *cache, const char *fn_name,
                                Type **type_args, int type_arg_count);
void mono_cache_insert(MonoCache *cache, const char *fn_name,
                                Type **type_args, int type_arg_count,
                                LLVMValueRef fn, const char *specialized_name);
void mono_cache_free(MonoCache *cache);

#endif // CODEGEN_H
