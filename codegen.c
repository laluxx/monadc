#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "reader.h"
#include "types.h"
#include "env.h"
#include "asm.h"
#include "runtime.h"
#include "module.h"
#include <ctype.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/TargetMachine.h>

/* Replace all exit(1) calls in codegen.
 * If a recovery point is set (we're inside repl_eval_line), longjmp back.
 * Otherwise fall back to real exit so the standalone compiler still works. */
#define CODEGEN_ERROR(ctx, fmt, ...) \
    do { \
        snprintf((ctx)->error_msg, sizeof((ctx)->error_msg), fmt, ##__VA_ARGS__); \
        fprintf(stderr, "%s\n", (ctx)->error_msg); \
        if ((ctx)->error_jmp_set) { \
            (ctx)->error_jmp_set = false; \
            longjmp((ctx)->error_jmp, 1); \
        } \
        exit(1); \
    } while(0)


void codegen_init(CodegenContext *ctx, const char *module_name) {
    ctx->context = LLVMContextCreate();
    ctx->module = LLVMModuleCreateWithNameInContext(module_name, ctx->context);
    ctx->builder = LLVMCreateBuilderInContext(ctx->context);
    ctx->env = env_create();
    ctx->module_ctx = NULL;
    ctx->init_fn = NULL;
    // Initialize format strings to NULL - will be created lazily
    ctx->fmt_str   = NULL;
    ctx->fmt_char  = NULL;
    ctx->fmt_int   = NULL;
    ctx->fmt_float = NULL;
    ctx->fmt_hex   = NULL;
    ctx->fmt_bin   = NULL;
    ctx->fmt_oct   = NULL;
    ctx->error_jmp_set = false;
    memset(ctx->error_msg, 0, sizeof(ctx->error_msg));
}

LLVMValueRef get_fmt_str(CodegenContext *ctx) {
    if (!ctx->fmt_str) {
        ctx->fmt_str = LLVMBuildGlobalStringPtr(ctx->builder, "%s\n", "fmt_str");
    }
    return ctx->fmt_str;
}

LLVMValueRef get_fmt_char(CodegenContext *ctx) {
    if (!ctx->fmt_char) {
        ctx->fmt_char = LLVMBuildGlobalStringPtr(ctx->builder, "%c\n", "fmt_char");
    }
    return ctx->fmt_char;
}

LLVMValueRef get_fmt_int(CodegenContext *ctx) {
    if (!ctx->fmt_int) {
        ctx->fmt_int = LLVMBuildGlobalStringPtr(ctx->builder, "%ld\n", "fmt_int");
    }
    return ctx->fmt_int;
}

LLVMValueRef get_fmt_float(CodegenContext *ctx) {
    if (!ctx->fmt_float) {
        ctx->fmt_float = LLVMBuildGlobalStringPtr(ctx->builder, "%g\n", "fmt_float");
    }
    return ctx->fmt_float;
}

LLVMValueRef get_fmt_hex(CodegenContext *ctx) {
    if (!ctx->fmt_hex) {
        ctx->fmt_hex = LLVMBuildGlobalStringPtr(ctx->builder, "0x%lX\n", "fmt_hex");
    }
    return ctx->fmt_hex;
}

LLVMValueRef get_fmt_oct(CodegenContext *ctx) {
    if (!ctx->fmt_oct) {
        ctx->fmt_oct = LLVMBuildGlobalStringPtr(ctx->builder, "0o%lo\n", "fmt_oct");
    }
    return ctx->fmt_oct;
}

LLVMValueRef get_fmt_str_no_newline(CodegenContext *ctx) {
    return LLVMBuildGlobalStringPtr(ctx->builder, "%s", "fmt_str_nn");
}

LLVMValueRef get_fmt_char_no_newline(CodegenContext *ctx) {
    return LLVMBuildGlobalStringPtr(ctx->builder, "%c", "fmt_char_nn");
}

LLVMValueRef get_fmt_int_no_newline(CodegenContext *ctx) {
    return LLVMBuildGlobalStringPtr(ctx->builder, "%ld", "fmt_int_nn");
}

LLVMValueRef get_fmt_float_no_newline(CodegenContext *ctx) {
    return LLVMBuildGlobalStringPtr(ctx->builder, "%g", "fmt_float_nn");
}

void codegen_dispose(CodegenContext *ctx) {
    LLVMDisposeBuilder(ctx->builder);
    LLVMDisposeModule(ctx->module);
    LLVMContextDispose(ctx->context);
    env_free(ctx->env);
}

/// Runtime Functions Declaration

LLVMValueRef get_or_declare_printf(CodegenContext *ctx) {
    LLVMValueRef printf_fn = LLVMGetNamedFunction(ctx->module, "printf");
    if (printf_fn) {
        return printf_fn;
    }

    LLVMTypeRef printf_args[] = {LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0)};
    LLVMTypeRef printf_type = LLVMFunctionType(
        LLVMInt32TypeInContext(ctx->context),
        printf_args,
        1,
        true
    );
    printf_fn = LLVMAddFunction(ctx->module, "printf", printf_type);
    return printf_fn;
}


// Declares (or retrieves) a hand-rolled print_binary(long) helper
// emitted once into the module as an LLVM function.
LLVMValueRef get_or_declare_print_binary(CodegenContext *ctx) {
    LLVMValueRef fn = LLVMGetNamedFunction(ctx->module, "__print_binary");
    if (fn) return fn;

    // long __print_binary(long n)
    LLVMTypeRef param = LLVMInt64TypeInContext(ctx->context);
    LLVMTypeRef fn_type = LLVMFunctionType(
        LLVMInt64TypeInContext(ctx->context), &param, 1, 0);
    fn = LLVMAddFunction(ctx->module, "__print_binary", fn_type);

    LLVMBasicBlockRef saved = LLVMGetInsertBlock(ctx->builder);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx->context, fn, "entry");
    LLVMPositionBuilderAtEnd(ctx->builder, entry);

    LLVMValueRef n = LLVMGetParam(fn, 0);
    LLVMValueRef printf_fn = get_or_declare_printf(ctx);

    // print "0b" prefix
    LLVMValueRef prefix = LLVMBuildGlobalStringPtr(ctx->builder, "0b", "bin_prefix");
    LLVMValueRef prefix_args[] = { prefix };
    LLVMBuildCall2(ctx->builder,
        LLVMGlobalGetValueType(printf_fn), printf_fn, prefix_args, 1, "");

    // We emit a loop: find highest set bit, then print each bit MSB-first.
    // bit_index counts from 63 down to 0, we skip leading zeros.
    // For simplicity, use a recursive-style unrolled approach via a small
    // alloca'd index variable.

    LLVMValueRef idx_ptr = LLVMBuildAlloca(ctx->builder,
        LLVMInt32TypeInContext(ctx->context), "idx");
    LLVMBuildStore(ctx->builder,
        LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 63, 0), idx_ptr);

    LLVMValueRef started_ptr = LLVMBuildAlloca(ctx->builder,
        LLVMInt32TypeInContext(ctx->context), "started");
    LLVMBuildStore(ctx->builder,
        LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0), started_ptr);

    LLVMBasicBlockRef loop_cond = LLVMAppendBasicBlockInContext(ctx->context, fn, "loop_cond");
    LLVMBasicBlockRef loop_body = LLVMAppendBasicBlockInContext(ctx->context, fn, "loop_body");
    LLVMBasicBlockRef loop_end  = LLVMAppendBasicBlockInContext(ctx->context, fn, "loop_end");

    LLVMBuildBr(ctx->builder, loop_cond);

    // loop_cond: idx >= 0
    LLVMPositionBuilderAtEnd(ctx->builder, loop_cond);
    LLVMValueRef idx_val = LLVMBuildLoad2(ctx->builder,
        LLVMInt32TypeInContext(ctx->context), idx_ptr, "idx_val");
    LLVMValueRef cond = LLVMBuildICmp(ctx->builder, LLVMIntSGE, idx_val,
        LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0), "cond");
    LLVMBuildCondBr(ctx->builder, cond, loop_body, loop_end);

    // loop_body
    LLVMPositionBuilderAtEnd(ctx->builder, loop_body);
    LLVMValueRef idx_val2 = LLVMBuildLoad2(ctx->builder,
        LLVMInt32TypeInContext(ctx->context), idx_ptr, "idx_val2");
    LLVMValueRef idx64 = LLVMBuildSExt(ctx->builder, idx_val2,
        LLVMInt64TypeInContext(ctx->context), "idx64");
    LLVMValueRef bit = LLVMBuildLShr(ctx->builder, n, idx64, "bit");
    LLVMValueRef bit1 = LLVMBuildAnd(ctx->builder, bit,
        LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 1, 0), "bit1");

    LLVMValueRef started_val = LLVMBuildLoad2(ctx->builder,
        LLVMInt32TypeInContext(ctx->context), started_ptr, "started_val");

    // print if bit==1 or already started (to avoid leading zeros)
    // is_one = (bit1 == 1)
    LLVMValueRef is_one = LLVMBuildICmp(ctx->builder, LLVMIntEQ, bit1,
        LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 1, 0), "is_one");
    // is_started = (started_val != 0)
    LLVMValueRef is_started = LLVMBuildICmp(ctx->builder, LLVMIntNE, started_val,
        LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0), "is_started");
    LLVMValueRef should_print = LLVMBuildOr(ctx->builder, is_one, is_started, "should_print");

    LLVMBasicBlockRef print_bb  = LLVMAppendBasicBlockInContext(ctx->context, fn, "print_bit");
    LLVMBasicBlockRef skip_bb   = LLVMAppendBasicBlockInContext(ctx->context, fn, "skip_bit");
    LLVMBuildCondBr(ctx->builder, should_print, print_bb, skip_bb);

    LLVMPositionBuilderAtEnd(ctx->builder, print_bb);
    // mark started
    LLVMBuildStore(ctx->builder,
        LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 1, 0), started_ptr);
    // print the bit character
    LLVMValueRef fmt_ld = LLVMBuildGlobalStringPtr(ctx->builder, "%ld", "fmt_ld");
    LLVMValueRef print_args[] = { fmt_ld, bit1 };
    LLVMBuildCall2(ctx->builder,
        LLVMGlobalGetValueType(printf_fn), printf_fn, print_args, 2, "");
    LLVMBuildBr(ctx->builder, skip_bb);

    LLVMPositionBuilderAtEnd(ctx->builder, skip_bb);
    // decrement idx
    LLVMValueRef idx_val3 = LLVMBuildLoad2(ctx->builder,
        LLVMInt32TypeInContext(ctx->context), idx_ptr, "idx_val3");
    LLVMValueRef new_idx = LLVMBuildSub(ctx->builder, idx_val3,
        LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 1, 0), "new_idx");
    LLVMBuildStore(ctx->builder, new_idx, idx_ptr);
    LLVMBuildBr(ctx->builder, loop_cond);

    // loop_end: if never started, print "0", then print newline
    LLVMPositionBuilderAtEnd(ctx->builder, loop_end);
    LLVMValueRef started_final = LLVMBuildLoad2(ctx->builder,
        LLVMInt32TypeInContext(ctx->context), started_ptr, "started_final");
    LLVMValueRef never_started = LLVMBuildICmp(ctx->builder, LLVMIntEQ, started_final,
        LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0), "never_started");

    LLVMBasicBlockRef zero_bb   = LLVMAppendBasicBlockInContext(ctx->context, fn, "print_zero");
    LLVMBasicBlockRef newline_bb = LLVMAppendBasicBlockInContext(ctx->context, fn, "print_newline");
    LLVMBuildCondBr(ctx->builder, never_started, zero_bb, newline_bb);

    LLVMPositionBuilderAtEnd(ctx->builder, zero_bb);
    LLVMValueRef zero_str = LLVMBuildGlobalStringPtr(ctx->builder, "0", "zero_str");
    LLVMValueRef zero_args[] = { zero_str };
    LLVMBuildCall2(ctx->builder,
        LLVMGlobalGetValueType(printf_fn), printf_fn, zero_args, 1, "");
    LLVMBuildBr(ctx->builder, newline_bb);

    LLVMPositionBuilderAtEnd(ctx->builder, newline_bb);
    LLVMValueRef nl = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "nl");
    LLVMValueRef nl_args[] = { nl };
    LLVMBuildCall2(ctx->builder,
        LLVMGlobalGetValueType(printf_fn), printf_fn, nl_args, 1, "");
    LLVMBuildRet(ctx->builder, LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0));

    if (saved) LLVMPositionBuilderAtEnd(ctx->builder, saved);
    return fn;
}

/// Type to LLVM Type conversion

LLVMTypeRef type_to_llvm(CodegenContext *ctx, Type *t) {
    if (!t) return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    switch (t->kind) {
    case TYPE_INT:
    case TYPE_HEX:
    case TYPE_BIN:
    case TYPE_OCT:
        return LLVMInt64TypeInContext(ctx->context);
    case TYPE_FLOAT:
        return LLVMDoubleTypeInContext(ctx->context);
    case TYPE_CHAR:
        return LLVMInt8TypeInContext(ctx->context);
    case TYPE_STRING:
    case TYPE_SYMBOL:
    case TYPE_KEYWORD:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    case TYPE_LIST:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0); // RuntimeList*
    case TYPE_RATIO:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0); // RuntimeValue* for Ratio
    case TYPE_ARR:
        // Arrays are stack-allocated - return array type
        if (t->arr_element_type && t->arr_size > 0) {
            LLVMTypeRef elem_type = type_to_llvm(ctx, t->arr_element_type);
            return LLVMArrayType(elem_type, t->arr_size);
        }
        // Fallback for unknown size
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    case TYPE_LAYOUT: {
        char struct_name[256];
        snprintf(struct_name, sizeof(struct_name), "layout.%s", t->layout_name);
        LLVMTypeRef existing = LLVMGetTypeByName2(ctx->context, struct_name);
        if (existing) return LLVMPointerType(existing, 0);  // always pointer

        LLVMTypeRef *field_types = malloc(sizeof(LLVMTypeRef) *
                                          (t->layout_field_count ? t->layout_field_count : 1));
        for (int i = 0; i < t->layout_field_count; i++)
            field_types[i] = type_to_llvm(ctx, t->layout_fields[i].type);

        LLVMTypeRef struct_type = LLVMStructCreateNamed(ctx->context, struct_name);
        LLVMStructSetBody(struct_type, field_types, t->layout_field_count,
                          t->layout_packed ? 1 : 0);
        free(field_types);
        return LLVMPointerType(struct_type, 0);  // always pointer
    }
    case TYPE_BOOL:
        return LLVMInt1TypeInContext(ctx->context);
    case TYPE_UNKNOWN:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    default:
        return LLVMDoubleTypeInContext(ctx->context);
    }
}

// Returns the bare LLVM struct type for a layout — never the pointer wrapper.
// Use this as the element-type argument to GEP/Load/Store on layout values.
static LLVMTypeRef layout_struct_type(CodegenContext *ctx, Type *lay) {
    char struct_name[256];
    snprintf(struct_name, sizeof(struct_name), "layout.%s", lay->layout_name);
    LLVMTypeRef st = LLVMGetTypeByName2(ctx->context, struct_name);
    if (!st) {
        // Force creation via type_to_llvm and strip the pointer wrapper
        LLVMTypeRef ptr = type_to_llvm(ctx, lay);   // returns ptr-to-struct
        (void)ptr;
        st = LLVMGetTypeByName2(ctx->context, struct_name);
    }
    return st;
}

/// Type checking for operations

bool type_is_numeric(Type *t) {
    return t->kind == TYPE_INT  || t->kind == TYPE_FLOAT ||
           t->kind == TYPE_HEX  || t->kind == TYPE_BIN   || t->kind == TYPE_OCT ||
           t->kind == TYPE_CHAR || t->kind == TYPE_RATIO;
}

bool type_is_integer(Type *t) {
    return t->kind == TYPE_INT || t->kind == TYPE_HEX ||
           t->kind == TYPE_BIN || t->kind == TYPE_OCT || t->kind == TYPE_CHAR;
}

bool type_is_float(Type *t) {
    return t->kind == TYPE_FLOAT;
}

// Forward declaration
CodegenResult codegen_expr(CodegenContext *ctx, AST *ast);

// Helper: are we currently emitting into a top-level main function?
// If yes, `define` should produce globals rather than stack allocas.
static bool is_at_top_level(CodegenContext *ctx) {
    LLVMBasicBlockRef bb = LLVMGetInsertBlock(ctx->builder);
    if (!bb) return false;
    // ctx->init_fn is set by main.c before calling codegen_expr for top-level
    if (!ctx->init_fn) return false;
    LLVMValueRef parent = LLVMGetBasicBlockParent(bb);
    return parent == ctx->init_fn;
}


/// Helper to print an AST at runtime (for quoted expressions)

void codegen_print_ast(CodegenContext *ctx, AST *ast) {
    LLVMValueRef printf_fn = get_or_declare_printf(ctx);

    switch (ast->type) {
    case AST_NUMBER: {
        LLVMValueRef num = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), ast->number);
        LLVMValueRef args[] = {get_fmt_float_no_newline(ctx), num};  // CHANGED
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        break;
    }

    case AST_SYMBOL: {
        LLVMValueRef sym = LLVMBuildGlobalStringPtr(ctx->builder, ast->symbol, "sym");
        LLVMValueRef args[] = {get_fmt_str_no_newline(ctx), sym};  // CHANGED
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        break;
    }

    case AST_STRING: {
        LLVMValueRef str = LLVMBuildGlobalStringPtr(ctx->builder, ast->string, "str");
        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "\"%s\"", "fmt");  // CHANGED - removed \n
        LLVMValueRef args[] = {fmt, str};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        break;
    }

    case AST_CHAR: {
        LLVMValueRef ch = LLVMConstInt(LLVMInt8TypeInContext(ctx->context), ast->character, 0);
        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "'%c'", "fmt");  // CHANGED - removed \n
        LLVMValueRef args[] = {fmt, ch};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        break;
    }

    case AST_KEYWORD: {
        char keyword_buf[256];
        snprintf(keyword_buf, sizeof(keyword_buf), ":%s", ast->keyword);  // CHANGED - removed \n
        LLVMValueRef kw = LLVMBuildGlobalStringPtr(ctx->builder, keyword_buf, "kw_print");
        LLVMValueRef args[] = {kw};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 1, "");
        break;
    }

    case AST_LIST: {
        LLVMValueRef lparen = LLVMBuildGlobalStringPtr(ctx->builder, "(", "lparen");
        LLVMValueRef args[] = {lparen};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 1, "");

        for (size_t i = 0; i < ast->list.count; i++) {
            if (i > 0) {
                LLVMValueRef space = LLVMBuildGlobalStringPtr(ctx->builder, " ", "space");
                LLVMValueRef space_args[] = {space};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, space_args, 1, "");
            }
            codegen_print_ast(ctx, ast->list.items[i]);
        }

        LLVMValueRef rparen = LLVMBuildGlobalStringPtr(ctx->builder, ")", "rparen");  // CHANGED - removed \n
        LLVMValueRef rparen_args[] = {rparen};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, rparen_args, 1, "");
        break;
    }

    default:
        break;
    }
}

static uint64_t fnv1a_hash(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
    return h;
}

static LLVMValueRef ast_to_runtime_value(CodegenContext *ctx, AST *ast_elem) {
    LLVMValueRef rt_val = NULL;

    switch (ast_elem->type) {
        case AST_NUMBER: {
            Type *num_type = infer_literal_type(ast_elem->number, ast_elem->literal_str);
            if (type_is_float(num_type)) {
                LLVMValueRef fn = get_rt_value_float(ctx);
                LLVMValueRef val = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context),
                                                 ast_elem->number);
                LLVMValueRef args[] = {val};
                rt_val = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                       fn, args, 1, "rtval");
            } else {
                LLVMValueRef fn = get_rt_value_int(ctx);
                LLVMValueRef val = LLVMConstInt(LLVMInt64TypeInContext(ctx->context),
                                                (int64_t)ast_elem->number, 0);
                LLVMValueRef args[] = {val};
                rt_val = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                       fn, args, 1, "rtval");
            }
            break;
        }

        case AST_CHAR: {
            LLVMValueRef fn = get_rt_value_char(ctx);
            LLVMValueRef val = LLVMConstInt(LLVMInt8TypeInContext(ctx->context),
                                            ast_elem->character, 0);
            LLVMValueRef args[] = {val};
            rt_val = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                   fn, args, 1, "rtval");
            break;
        }

        case AST_STRING: {
            LLVMValueRef fn = get_rt_value_string(ctx);
            LLVMValueRef str = LLVMBuildGlobalStringPtr(ctx->builder,
                                                        ast_elem->string, "str");
            LLVMValueRef args[] = {str};
            rt_val = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                   fn, args, 1, "rtval");
            break;
        }

        case AST_KEYWORD: {
            LLVMValueRef fn = get_rt_value_keyword(ctx);
            LLVMValueRef kw = LLVMBuildGlobalStringPtr(ctx->builder,
                                                       ast_elem->keyword, "kw");
            LLVMValueRef args[] = {kw};
            rt_val = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                   fn, args, 1, "rtval");
            break;
        }

        case AST_SYMBOL: {
            LLVMValueRef fn  = get_rt_value_symbol(ctx);
            LLVMValueRef sym = LLVMBuildGlobalStringPtr(ctx->builder,
                                                        ast_elem->symbol, "sym");
            LLVMValueRef args[] = {sym};
            rt_val = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                    fn, args, 1, "rtval");
            break;
        }

        case AST_LIST: {
            // Recursively create a runtime list
            LLVMValueRef list_fn = get_rt_list_new(ctx);
            LLVMValueRef list = LLVMBuildCall2(ctx->builder,
                   LLVMGlobalGetValueType(list_fn), list_fn, NULL, 0, "sublist");


            for (size_t i = 0; i < ast_elem->list.count; i++) {
                LLVMValueRef elem_val = ast_to_runtime_value(ctx, ast_elem->list.items[i]);
                if (elem_val) {
                    LLVMValueRef append_fn = get_rt_list_append(ctx);
                    LLVMValueRef args[] = {list, elem_val};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(append_fn),
                                 append_fn, args, 2, "");
                }
            }

            // Wrap the list in a RuntimeValue
            LLVMValueRef fn = get_rt_value_list(ctx);
            LLVMValueRef args[] = {list};
            rt_val = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                   fn, args, 1, "rtval");
            break;
        }

    case AST_LAMBDA: {
            LLVMValueRef list_fn = get_rt_list_new(ctx);
            LLVMValueRef list = LLVMBuildCall2(ctx->builder,
                   LLVMGlobalGetValueType(list_fn), list_fn, NULL, 0, "lam_list");

            // symbol 'lambda'
            LLVMValueRef sym_fn = get_rt_value_symbol(ctx);
            LLVMValueRef lam_sym = LLVMBuildGlobalStringPtr(ctx->builder, "lambda", "lam_sym");
            LLVMValueRef lam_sym_val_args[] = {lam_sym};
            LLVMValueRef lam_sym_val = LLVMBuildCall2(ctx->builder,
                   LLVMGlobalGetValueType(sym_fn), sym_fn, lam_sym_val_args, 1, "");
            LLVMValueRef append_fn = get_rt_list_append(ctx);
            LLVMValueRef app_args[] = {list, lam_sym_val};
            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(append_fn), append_fn, app_args, 2, "");

            // params sublist
            LLVMValueRef params_list = LLVMBuildCall2(ctx->builder,
                   LLVMGlobalGetValueType(list_fn), list_fn, NULL, 0, "params_list");
            for (int i = 0; i < ast_elem->lambda.param_count; i++) {
                LLVMValueRef psym = LLVMBuildGlobalStringPtr(ctx->builder,
                                       ast_elem->lambda.params[i].name, "pname");
                LLVMValueRef psym_val_args[] = {psym};
                LLVMValueRef psym_val = LLVMBuildCall2(ctx->builder,
                       LLVMGlobalGetValueType(sym_fn), sym_fn, psym_val_args, 1, "");
                LLVMValueRef pa[] = {params_list, psym_val};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(append_fn), append_fn, pa, 2, "");
            }
            LLVMValueRef params_rv_fn = get_rt_value_list(ctx);
            LLVMValueRef params_rv_args[] = {params_list};
            LLVMValueRef params_rv = LLVMBuildCall2(ctx->builder,
                   LLVMGlobalGetValueType(params_rv_fn), params_rv_fn, params_rv_args, 1, "");
            LLVMValueRef pa2[] = {list, params_rv};
            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(append_fn), append_fn, pa2, 2, "");

            // body exprs
            for (int i = 0; i < ast_elem->lambda.body_count; i++) {
                LLVMValueRef body_val = ast_to_runtime_value(ctx, ast_elem->lambda.body_exprs[i]);
                LLVMValueRef ba[] = {list, body_val};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(append_fn), append_fn, ba, 2, "");
            }

            LLVMValueRef wrap_fn = get_rt_value_list(ctx);
            LLVMValueRef wrap_args[] = {list};
            rt_val = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(wrap_fn),
                                    wrap_fn, wrap_args, 1, "lam_rtval");
            break;
        }

        default:
            // For unsupported types, create nil
            LLVMValueRef fn = get_rt_value_nil(ctx);
            rt_val = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                   fn, NULL, 0, "rtval_nil");
            break;
    }

    return rt_val;
}

static LLVMValueRef codegen_to_runtime_value(CodegenContext *ctx,
                                             LLVMValueRef value,
                                             Type *type) {
    if (!type) return get_rt_value_nil(ctx);

    switch (type->kind) {
        case TYPE_INT:
        case TYPE_HEX:
        case TYPE_BIN:
        case TYPE_OCT: {
            LLVMValueRef fn = get_rt_value_int(ctx);
            LLVMValueRef args[] = {value};
            return LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                 fn, args, 1, "rtval");
        }

        case TYPE_FLOAT: {
            LLVMValueRef fn = get_rt_value_float(ctx);
            LLVMValueRef args[] = {value};
            return LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                 fn, args, 1, "rtval");
        }

        case TYPE_CHAR: {
            LLVMValueRef fn = get_rt_value_char(ctx);
            LLVMValueRef args[] = {value};
            return LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                 fn, args, 1, "rtval");
        }

        case TYPE_STRING: {
            LLVMValueRef fn = get_rt_value_string(ctx);
            LLVMValueRef args[] = {value};
            return LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                 fn, args, 1, "rtval");
        }

        case TYPE_RATIO:
        case TYPE_ARR:
        case TYPE_LIST:
            // Already RuntimeValue*
            return value;

        default:
            return get_rt_value_nil(ctx);
    }
}

bool should_export_symbol(ModuleContext *ctx, const char *symbol_name) {
    if (!ctx || !ctx->decl) {
        return true;  // No module declaration = export everything
    }

    return module_decl_is_exported(ctx->decl, symbol_name);
}

static EnvEntry *resolve_symbol_with_modules(CodegenContext *ctx, const char *symbol_name, AST *ast) {
    // Check if it's a qualified symbol (contains '.' like "M.phi")
    char *dot = strchr(symbol_name, '.');

    if (dot) {
        // Qualified symbol: Module.symbol
        size_t prefix_len = dot - symbol_name;
        char *module_prefix = malloc(prefix_len + 1);
        memcpy(module_prefix, symbol_name, prefix_len);
        module_prefix[prefix_len] = '\0';

        const char *local_symbol = dot + 1;

        if (!ctx->module_ctx) {
            free(module_prefix);
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: qualified symbol '%s' used but no module context",
                    parser_get_filename(), ast->line, ast->column, symbol_name);
        }

        // Find the import with this prefix
        ImportDecl *import = module_context_find_import(ctx->module_ctx, module_prefix);
        if (!import) {
            free(module_prefix);
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown module prefix '%s'",
                    parser_get_filename(), ast->line, ast->column, module_prefix);
        }

        // Check if this symbol is included in the import
        if (!import_decl_includes_symbol(import, local_symbol)) {
            free(module_prefix);
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: symbol '%s' not imported from module '%s'",
                    parser_get_filename(), ast->line, ast->column,
                    local_symbol, import->module_name);
        }

        // Look up the symbol in the environment
        // For now, we look up the fully qualified name
        EnvEntry *entry = env_lookup(ctx->env, symbol_name);
        if (!entry) {
            // Try looking up just the local symbol name
            entry = env_lookup(ctx->env, local_symbol);
        }

        free(module_prefix);
        return entry;
    } else {
        // Unqualified symbol - check local environment first
        EnvEntry *entry = env_lookup(ctx->env, symbol_name);
        if (entry) {
            return entry;
        }

        // If not found locally and we have a module context, check imports
        if (ctx->module_ctx) {
            // Try to resolve through unqualified imports
            for (size_t i = 0; i < ctx->module_ctx->import_count; i++) {
                ImportDecl *import = ctx->module_ctx->imports[i];

                // Skip qualified imports - they require explicit prefix
                if (import->mode == IMPORT_QUALIFIED) {
                    continue;
                }

                // Check if this import includes the symbol
                // For HIDING imports: symbol is available if NOT in the hide list
                // For SELECTIVE imports: symbol is available if IN the list
                // For UNQUALIFIED: always available
                bool sym_available;
                if (import->mode == IMPORT_HIDING) {
                    sym_available = !import_decl_includes_symbol(import, symbol_name);
                } else {
                    sym_available = import_decl_includes_symbol(import, symbol_name);
                }
                if (sym_available) {
                    // Try to find it in environment
                    // First try with module prefix (if alias exists)
                    if (import->alias) {
                        char qualified_name[256];
                        snprintf(qualified_name, sizeof(qualified_name), "%s.%s",
                                import->alias, symbol_name);
                        entry = env_lookup(ctx->env, qualified_name);
                        if (entry) {
                            return entry;
                        }
                    }

                    // Also try just the symbol name
                    entry = env_lookup(ctx->env, symbol_name);
                    if (entry) {
                        return entry;
                    }
                }
            }
        }

        return NULL;
    }
}

// declare_externals — inject dep's exports into the current codegen context
EnvParam *clone_params(EnvParam *src, int count) {
    if (count == 0 || !src) return NULL;
    EnvParam *p = malloc(sizeof(EnvParam) * count);
    for (int i = 0; i < count; i++) {
        p[i].name = strdup(src[i].name ? src[i].name : "_");
        p[i].type = type_clone(src[i].type);
    }
    return p;
}

static void codegen_show_value(CodegenContext *ctx, LLVMValueRef val, Type *type, bool newline) {
    LLVMValueRef printf_fn = get_or_declare_printf(ctx);

    if (type->kind == TYPE_STRING) {
        LLVMValueRef args[] = {newline ? get_fmt_str(ctx) : get_fmt_str_no_newline(ctx), val};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
    } else if (type->kind == TYPE_CHAR) {
        LLVMValueRef args[] = {newline ? get_fmt_char(ctx) : get_fmt_char_no_newline(ctx), val};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
    } else if (type->kind == TYPE_FLOAT) {
        LLVMValueRef args[] = {newline ? get_fmt_float(ctx) : get_fmt_float_no_newline(ctx), val};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
    } else if (type->kind == TYPE_HEX) {
        LLVMValueRef args[] = {get_fmt_hex(ctx), val};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        if (newline) {
            LLVMValueRef nl = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "nl");
            LLVMValueRef nl_args[] = {nl};
            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, nl_args, 1, "");
        }
    } else if (type->kind == TYPE_BIN) {
        LLVMValueRef fn_bin = get_or_declare_print_binary(ctx);
        LLVMValueRef args[] = {val};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn_bin), fn_bin, args, 1, "");
        if (newline) {
            LLVMValueRef nl = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "nl");
            LLVMValueRef nl_args[] = {nl};
            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, nl_args, 1, "");
        }
    } else if (type->kind == TYPE_OCT) {
        LLVMValueRef args[] = {get_fmt_oct(ctx), val};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        if (newline) {
            LLVMValueRef nl = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "nl");
            LLVMValueRef nl_args[] = {nl};
            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, nl_args, 1, "");
        }
    } else if (type->kind == TYPE_INT) {
        LLVMValueRef args[] = {newline ? get_fmt_int(ctx) : get_fmt_int_no_newline(ctx), val};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
    } else if (type->kind == TYPE_LIST) {
        LLVMValueRef print_fn = get_rt_print_list(ctx);
        LLVMValueRef args[] = {val};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(print_fn), print_fn, args, 1, "");
        if (newline) {
            LLVMValueRef nl = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "nl");
            LLVMValueRef nl_args[] = {nl};
            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, nl_args, 1, "");
        }
    } else if (type->kind == TYPE_RATIO || type->kind == TYPE_SYMBOL) {
        LLVMValueRef print_fn = get_rt_print_value(ctx);
        LLVMValueRef args[] = {val};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(print_fn), print_fn, args, 1, "");
        if (newline) {
            LLVMValueRef nl = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "nl");
            LLVMValueRef nl_args[] = {nl};
            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, nl_args, 1, "");
        }
    } else if (type->kind == TYPE_KEYWORD) {
        LLVMValueRef args[] = {newline ? get_fmt_str(ctx) : get_fmt_str_no_newline(ctx), val};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
    } else if (type->kind == TYPE_BOOL) {
        LLVMValueRef true_str  = LLVMBuildGlobalStringPtr(ctx->builder, "True",  "true_str");
        LLVMValueRef false_str = LLVMBuildGlobalStringPtr(ctx->builder, "False", "false_str");
        LLVMValueRef bool_str  = LLVMBuildSelect(ctx->builder, val, true_str, false_str, "bool_str");
        LLVMValueRef args[] = {newline ? get_fmt_str(ctx) : get_fmt_str_no_newline(ctx), bool_str};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
    } else if (type->kind == TYPE_UNKNOWN) {
        // Opaque RuntimeValue* from car/list-ref — dispatch at runtime
        LLVMValueRef print_fn = get_rt_print_value_newline(ctx);
        LLVMTypeRef  ptr      = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
        LLVMTypeRef  ft_args[] = {ptr};
        LLVMTypeRef  ft       = LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), ft_args, 1, 0);
        LLVMValueRef args[]   = {val};
        LLVMBuildCall2(ctx->builder, ft, print_fn, args, 1, "");
    } else {
        // fallback: float
        LLVMValueRef args[] = {newline ? get_fmt_float(ctx) : get_fmt_float_no_newline(ctx), val};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
    }
}

static LLVMValueRef get_or_declare_strdup(CodegenContext *ctx) {
    LLVMValueRef fn = LLVMGetNamedFunction(ctx->module, "strdup");
    if (!fn) {
        LLVMTypeRef p  = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
        LLVMTypeRef ft = LLVMFunctionType(p, &p, 1, 0);
        fn = LLVMAddFunction(ctx->module, "strdup", ft);
        LLVMSetLinkage(fn, LLVMExternalLinkage);
    }
    return fn;
}

static void build_br_if_no_terminator(LLVMBuilderRef builder, LLVMBasicBlockRef dest) {
    LLVMBasicBlockRef cur = LLVMGetInsertBlock(builder);
    if (cur && !LLVMGetBasicBlockTerminator(cur))
        LLVMBuildBr(builder, dest);
}

static LLVMValueRef codegen_box(CodegenContext *ctx, LLVMValueRef val, Type *type) {
    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);

    if (!type) {
        // already a pointer — assume boxed
        return val;
    }
    switch (type->kind) {
        case TYPE_INT:
        case TYPE_HEX:
        case TYPE_BIN:
        case TYPE_OCT: {
            LLVMValueRef fn       = get_rt_value_int(ctx);
            LLVMTypeRef  ft_args[] = {i64};
            LLVMTypeRef  ft       = LLVMFunctionType(ptr, ft_args, 1, 0);
            LLVMValueRef coerced  = LLVMTypeOf(val) != i64
                ? LLVMBuildSExtOrBitCast(ctx->builder, val, i64, "box_int")
                : val;
            LLVMValueRef args[]   = {coerced};
            return LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "boxed_int");
        }
        case TYPE_FLOAT: {
            LLVMTypeRef  dbl      = LLVMDoubleTypeInContext(ctx->context);
            LLVMValueRef fn       = get_rt_value_float(ctx);
            LLVMTypeRef  ft_args[] = {dbl};
            LLVMTypeRef  ft       = LLVMFunctionType(ptr, ft_args, 1, 0);
            LLVMValueRef args[]   = {val};
            return LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "boxed_float");
        }
        case TYPE_CHAR: {
            LLVMTypeRef  i8       = LLVMInt8TypeInContext(ctx->context);
            LLVMValueRef fn       = get_rt_value_char(ctx);
            LLVMTypeRef  ft_args[] = {i8};
            LLVMTypeRef  ft       = LLVMFunctionType(ptr, ft_args, 1, 0);
            LLVMValueRef args[]   = {val};
            return LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "boxed_char");
        }
        case TYPE_STRING: {
            LLVMValueRef fn       = get_rt_value_string(ctx);
            LLVMTypeRef  ft_args[] = {ptr};
            LLVMTypeRef  ft       = LLVMFunctionType(ptr, ft_args, 1, 0);
            LLVMValueRef args[]   = {val};
            return LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "boxed_str");
        }
        case TYPE_BOOL: {
            // store as int (0/1)
            LLVMValueRef fn       = get_rt_value_int(ctx);
            LLVMTypeRef  ft_args[] = {i64};
            LLVMTypeRef  ft       = LLVMFunctionType(ptr, ft_args, 1, 0);
            LLVMValueRef ext      = LLVMBuildZExt(ctx->builder, val, i64, "bool_to_int");
            LLVMValueRef args[]   = {ext};
            return LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "boxed_bool");
        }
        case TYPE_LIST: {
            // already a RuntimeList* pointer — wrap in rt_value_list
            LLVMValueRef fn       = get_rt_value_list(ctx);
            LLVMTypeRef  ft_args[] = {ptr};
            LLVMTypeRef  ft       = LLVMFunctionType(ptr, ft_args, 1, 0);
            LLVMValueRef args[]   = {val};
            return LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "boxed_list");
        }
        default:
            // already a pointer / unknown — pass through
            return val;
    }
}

// Helper: resolve a function argument to a raw function pointer LLVMValueRef.
// Named functions -> direct LLVM function value (no load).
// Lambdas / other exprs -> codegen normally.
static LLVMValueRef codegen_fn_arg(CodegenContext *ctx, AST *node) {
    if (node->type == AST_SYMBOL) {
        EnvEntry *e = env_lookup(ctx->env, node->symbol);
        if (e && e->kind == ENV_FUNC && e->func_ref)
            return e->func_ref;
        LLVMValueRef fn = LLVMGetNamedFunction(ctx->module, node->symbol);
        if (fn) return fn;
        // last resort: try the llvm_name if it was mangled
        if (e && e->llvm_name) {
            fn = LLVMGetNamedFunction(ctx->module, e->llvm_name);
            if (fn) return fn;
        }
    }
    CodegenResult r = codegen_expr(ctx, node);
    if (!r.value) {
        fprintf(stderr, "codegen_fn_arg: could not resolve function '%s'\n",
                node->type == AST_SYMBOL ? node->symbol : "<expr>");
    }
    return r.value;
}

// ─── Wrapper generators ────────────────────────────────────────────────────
//
// These emit small LLVM functions that bridge typed user functions
// (e.g. i64->i64) into the RuntimeValue*->RuntimeValue* ABI that the
// runtime HOFs (map, filter, foldl, foldr, zipwith) expect.

static LLVMValueRef codegen_unary_wrapper(CodegenContext *ctx,
                                           LLVMValueRef   user_fn,
                                           Type          *arg_type,
                                           Type          *ret_type) {
    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
    LLVMTypeRef dbl = LLVMDoubleTypeInContext(ctx->context);
    LLVMTypeRef i1  = LLVMInt1TypeInContext(ctx->context);

    // wrapper signature: ptr -> ptr  (RuntimeValue* -> RuntimeValue*)
    LLVMTypeRef wrapper_ft = LLVMFunctionType(ptr, &ptr, 1, 0);

    static int unary_count = 0;
    char wname[64];
    snprintf(wname, sizeof(wname), "__unary_wrapper_%d", unary_count++);

    LLVMValueRef      wrapper = LLVMAddFunction(ctx->module, wname, wrapper_ft);
    LLVMBasicBlockRef entry   = LLVMAppendBasicBlockInContext(ctx->context, wrapper, "entry");
    LLVMBasicBlockRef saved   = LLVMGetInsertBlock(ctx->builder);
    LLVMPositionBuilderAtEnd(ctx->builder, entry);

    LLVMValueRef arg = LLVMGetParam(wrapper, 0);  // RuntimeValue*

    // ── unbox input ──────────────────────────────────────────────────────
    LLVMValueRef unboxed;
    LLVMTypeRef  user_arg_llvm;
    if (arg_type && type_is_integer(arg_type)) {
        LLVMTypeRef uft = LLVMFunctionType(i64, &ptr, 1, 0);
        unboxed       = LLVMBuildCall2(ctx->builder, uft,
                                       get_rt_unbox_int(ctx), &arg, 1, "unboxed");
        user_arg_llvm = i64;
    } else if (arg_type && type_is_float(arg_type)) {
        LLVMTypeRef uft = LLVMFunctionType(dbl, &ptr, 1, 0);
        unboxed       = LLVMBuildCall2(ctx->builder, uft,
                                       get_rt_unbox_float(ctx), &arg, 1, "unboxed");
        user_arg_llvm = dbl;
    } else {
        // ptr passthrough (list, string, already-boxed value)
        unboxed       = arg;
        user_arg_llvm = ptr;
    }

    // ── call user function ───────────────────────────────────────────────
    LLVMTypeRef user_ret_llvm;
    if (ret_type && type_is_integer(ret_type))   user_ret_llvm = i64;
    else if (ret_type && type_is_float(ret_type)) user_ret_llvm = dbl;
    else if (ret_type && ret_type->kind == TYPE_BOOL) user_ret_llvm = i1;
    else                                          user_ret_llvm = ptr;

    LLVMTypeRef  user_ft = LLVMFunctionType(user_ret_llvm, &user_arg_llvm, 1, 0);
    LLVMValueRef raw     = LLVMBuildCall2(ctx->builder, user_ft,
                                          user_fn, &unboxed, 1, "raw");

    // ── box result ───────────────────────────────────────────────────────
    LLVMValueRef boxed;
    if (ret_type && type_is_integer(ret_type)) {
        LLVMTypeRef bft = LLVMFunctionType(ptr, &i64, 1, 0);
        boxed = LLVMBuildCall2(ctx->builder, bft,
                               get_rt_value_int(ctx), &raw, 1, "boxed");
    } else if (ret_type && type_is_float(ret_type)) {
        LLVMTypeRef bft = LLVMFunctionType(ptr, &dbl, 1, 0);
        boxed = LLVMBuildCall2(ctx->builder, bft,
                               get_rt_value_float(ctx), &raw, 1, "boxed");
    } else if (ret_type && ret_type->kind == TYPE_BOOL) {
        // bool i1 -> box as int 0/1
        LLVMValueRef extended = LLVMBuildZExt(ctx->builder, raw, i64, "bool_to_i64");
        LLVMTypeRef  bft      = LLVMFunctionType(ptr, &i64, 1, 0);
        boxed = LLVMBuildCall2(ctx->builder, bft,
                               get_rt_value_int(ctx), &extended, 1, "boxed");
    } else {
        boxed = raw;  // already ptr
    }

    LLVMBuildRet(ctx->builder, boxed);
    if (saved) LLVMPositionBuilderAtEnd(ctx->builder, saved);
    return wrapper;
}

static LLVMValueRef codegen_binary_wrapper(CodegenContext *ctx,
                                            LLVMValueRef   user_fn,
                                            Type          *arg0_type,
                                            Type          *arg1_type,
                                            Type          *ret_type) {
    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
    LLVMTypeRef dbl = LLVMDoubleTypeInContext(ctx->context);

    // wrapper signature: (ptr, ptr) -> ptr
    LLVMTypeRef wrapper_params[] = {ptr, ptr};
    LLVMTypeRef wrapper_ft = LLVMFunctionType(ptr, wrapper_params, 2, 0);

    static int binary_count = 0;
    char wname[64];
    snprintf(wname, sizeof(wname), "__binary_wrapper_%d", binary_count++);

    LLVMValueRef      wrapper = LLVMAddFunction(ctx->module, wname, wrapper_ft);
    LLVMBasicBlockRef entry   = LLVMAppendBasicBlockInContext(ctx->context, wrapper, "entry");
    LLVMBasicBlockRef saved   = LLVMGetInsertBlock(ctx->builder);
    LLVMPositionBuilderAtEnd(ctx->builder, entry);

    LLVMValueRef arg0 = LLVMGetParam(wrapper, 0);
    LLVMValueRef arg1 = LLVMGetParam(wrapper, 1);

    // ── unbox arg0 ───────────────────────────────────────────────────────
    LLVMValueRef unboxed0;
    LLVMTypeRef  llvm_arg0;
    if (arg0_type && type_is_integer(arg0_type)) {
        LLVMTypeRef uft = LLVMFunctionType(i64, &ptr, 1, 0);
        unboxed0  = LLVMBuildCall2(ctx->builder, uft,
                                   get_rt_unbox_int(ctx), &arg0, 1, "ub0");
        llvm_arg0 = i64;
    } else if (arg0_type && type_is_float(arg0_type)) {
        LLVMTypeRef uft = LLVMFunctionType(dbl, &ptr, 1, 0);
        unboxed0  = LLVMBuildCall2(ctx->builder, uft,
                                   get_rt_unbox_float(ctx), &arg0, 1, "ub0");
        llvm_arg0 = dbl;
    } else {
        unboxed0  = arg0;
        llvm_arg0 = ptr;
    }

    // ── unbox arg1 ───────────────────────────────────────────────────────
    LLVMValueRef unboxed1;
    LLVMTypeRef  llvm_arg1;
    if (arg1_type && type_is_integer(arg1_type)) {
        LLVMTypeRef uft = LLVMFunctionType(i64, &ptr, 1, 0);
        unboxed1  = LLVMBuildCall2(ctx->builder, uft,
                                   get_rt_unbox_int(ctx), &arg1, 1, "ub1");
        llvm_arg1 = i64;
    } else if (arg1_type && type_is_float(arg1_type)) {
        LLVMTypeRef uft = LLVMFunctionType(dbl, &ptr, 1, 0);
        unboxed1  = LLVMBuildCall2(ctx->builder, uft,
                                   get_rt_unbox_float(ctx), &arg1, 1, "ub1");
        llvm_arg1 = dbl;
    } else {
        unboxed1  = arg1;
        llvm_arg1 = ptr;
    }

    // ── call user function ───────────────────────────────────────────────
    LLVMTypeRef user_ret_llvm;
    if (ret_type && type_is_integer(ret_type))    user_ret_llvm = i64;
    else if (ret_type && type_is_float(ret_type)) user_ret_llvm = dbl;
    else                                          user_ret_llvm = ptr;

    LLVMTypeRef  user_param_types[] = {llvm_arg0, llvm_arg1};
    LLVMTypeRef  user_ft  = LLVMFunctionType(user_ret_llvm, user_param_types, 2, 0);
    LLVMValueRef user_args[] = {unboxed0, unboxed1};
    LLVMValueRef raw = LLVMBuildCall2(ctx->builder, user_ft,
                                      user_fn, user_args, 2, "raw");

    // ── box result ───────────────────────────────────────────────────────
    LLVMValueRef boxed;
    if (ret_type && type_is_integer(ret_type)) {
        LLVMTypeRef bft = LLVMFunctionType(ptr, &i64, 1, 0);
        boxed = LLVMBuildCall2(ctx->builder, bft,
                               get_rt_value_int(ctx), &raw, 1, "boxed");
    } else if (ret_type && type_is_float(ret_type)) {
        LLVMTypeRef bft = LLVMFunctionType(ptr, &dbl, 1, 0);
        boxed = LLVMBuildCall2(ctx->builder, bft,
                               get_rt_value_float(ctx), &raw, 1, "boxed");
    } else {
        boxed = raw;
    }

    LLVMBuildRet(ctx->builder, boxed);
    if (saved) LLVMPositionBuilderAtEnd(ctx->builder, saved);
    return wrapper;
}

// Helper: extract arg/ret types from a named function in the env
static void lookup_fn_types(CodegenContext *ctx, AST *fn_node,
                             Type **arg0, Type **arg1, Type **ret) {
    *arg0 = type_unknown();
    *arg1 = type_unknown();
    *ret  = type_unknown();
    if (fn_node->type == AST_SYMBOL) {
        EnvEntry *e = env_lookup(ctx->env, fn_node->symbol);
        if (e && e->kind == ENV_FUNC) {
            if (e->param_count >= 1) *arg0 = e->params[0].type;
            if (e->param_count >= 2) *arg1 = e->params[1].type;
            if (e->return_type)      *ret  = e->return_type;
        }
    }
}

// Collect free variables in `ast` that are not in `bound_params`.
// Results are appended to `free_vars`/`free_count` (realloc'd).
// Only collects variables that actually exist in `outer_env` as locals
// (i.e. have an alloca value, not globals or functions).
static void collect_free_vars(AST *ast,
                               const char **bound_params, int bound_count,
                               const char **inner_names,  int inner_count,
                               char ***free_vars, int *free_count,
                               Env *outer_env) {
    if (!ast) return;

    switch (ast->type) {

    case AST_SYMBOL: {
        const char *name = ast->symbol;

        // Skip if bound by inner function's own params
        for (int i = 0; i < bound_count; i++)
            if (strcmp(bound_params[i], name) == 0) return;

        // Skip if it's the inner function's own name (recursive call)
        for (int i = 0; i < inner_count; i++)
            if (strcmp(inner_names[i], name) == 0) return;

        // Skip if already in free_vars
        for (int i = 0; i < *free_count; i++)
            if (strcmp((*free_vars)[i], name) == 0) return;

        // Check if it exists in outer env as a local (alloca)
        EnvEntry *e = env_lookup(outer_env, name);
        if (!e) return;
        if (e->kind != ENV_VAR) return;  // skip functions and builtins
        if (!e->value) return;
        // Only capture locals (allocas), not globals
        if (LLVMGetValueKind(e->value) == LLVMGlobalVariableValueKind) return;

        // It's a captured local — add it
        *free_vars = realloc(*free_vars, sizeof(char*) * (*free_count + 1));
        (*free_vars)[(*free_count)++] = strdup(name);
        return;
    }

    case AST_LIST:
        for (size_t i = 0; i < ast->list.count; i++)
            collect_free_vars(ast->list.items[i],
                              bound_params, bound_count,
                              inner_names,  inner_count,
                              free_vars, free_count, outer_env);
        return;

    case AST_ARRAY:
        for (size_t i = 0; i < ast->array.element_count; i++)
            collect_free_vars(ast->array.elements[i],
                              bound_params, bound_count,
                              inner_names,  inner_count,
                              free_vars, free_count, outer_env);
        return;

    case AST_RANGE:
        if (ast->range.start) collect_free_vars(ast->range.start, bound_params, bound_count, inner_names, inner_count, free_vars, free_count, outer_env);
        if (ast->range.step)  collect_free_vars(ast->range.step,  bound_params, bound_count, inner_names, inner_count, free_vars, free_count, outer_env);
        if (ast->range.end)   collect_free_vars(ast->range.end,   bound_params, bound_count, inner_names, inner_count, free_vars, free_count, outer_env);
        return;

    case AST_ADDRESS_OF:
        if (ast->list.count > 0)
            collect_free_vars(ast->list.items[0], bound_params, bound_count, inner_names, inner_count, free_vars, free_count, outer_env);
        return;

    case AST_LAMBDA: {
        // Descend into nested lambdas but add their params to bound set
        // so we find variables that are free at ANY depth
        int new_bound_count = bound_count + ast->lambda.param_count;
        const char **new_bound = malloc(sizeof(char*) * (new_bound_count ? new_bound_count : 1));
        memcpy(new_bound, bound_params, sizeof(char*) * bound_count);
        for (int i = 0; i < ast->lambda.param_count; i++)
            new_bound[bound_count + i] = ast->lambda.params[i].name;
        for (int i = 0; i < ast->lambda.body_count; i++)
            collect_free_vars(ast->lambda.body_exprs[i],
                              new_bound, new_bound_count,
                              inner_names, inner_count,
                              free_vars, free_count, outer_env);
        free(new_bound);
        return;
    }

    default:
        return;
    }
}

// Returns byte size of a primitive type name, for offset computation.
static int layout_field_byte_size(CodegenContext *ctx, Type *t) {
    if (!t) return 8;
    switch (t->kind) {
        case TYPE_INT:
        case TYPE_HEX:
        case TYPE_BIN:
        case TYPE_OCT:   return 8;
        case TYPE_FLOAT: return 8;
        case TYPE_CHAR:  return 1;
        case TYPE_BOOL:  return 1;
        case TYPE_ARR:
            if (t->arr_element_type && t->arr_size > 0)
                return layout_field_byte_size(ctx, t->arr_element_type) * t->arr_size;
            return 8;
        case TYPE_LAYOUT:
            return t->layout_total_size > 0 ? t->layout_total_size : 8;
        default:         return 8;
    }
}

void codegen_layout(CodegenContext *ctx, AST *ast) {
    const char *name = ast->layout.name;
    if (!name || !isupper((unsigned char)name[0])) {
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: layout name '%s' must start with an uppercase letter",
                      parser_get_filename(), ast->line, ast->column,
                      name ? name : "");
    }

    // ast->type == AST_LAYOUT
    // 1. Resolve each field's type
    LayoutField *fields = malloc(sizeof(LayoutField) * (ast->layout.field_count ? ast->layout.field_count : 1));

    for (int i = 0; i < ast->layout.field_count; i++) {
        ASTLayoutField *af = &ast->layout.fields[i];
        Type *ft = NULL;

        if (af->is_array) {
            // [ElemType Size] sugar
            Type *elem = type_from_name(af->array_elem);
            if (!elem) {
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown array element type '%s' in layout '%s'",
                              parser_get_filename(), ast->line, ast->column,
                              af->array_elem, ast->layout.name);
            }
            ft = type_arr(elem, af->array_size);
        } else {
            ft = type_from_name(af->type_name);
            if (!ft) {
                // Try layout registry (nested struct)
                ft = env_lookup_layout(ctx->env, af->type_name);
                if (ft) ft = type_clone(ft);  // clone so ownership is clear
            }
            if (!ft) {
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown type '%s' for field '%s' in layout '%s'",
                              parser_get_filename(), ast->line, ast->column,
                              af->type_name, af->name, ast->layout.name);
            }
        }

        fields[i].name = strdup(af->name);
        fields[i].type = ft;
        fields[i].size = layout_field_byte_size(ctx, ft);
        fields[i].offset = 0; // filled by layout_compute_offsets
    }

    // 2. Compute offsets
    int total_size = layout_compute_offsets(fields, ast->layout.field_count,
                                            ast->layout.packed,
                                            NULL); // elem_size_fn not needed (sizes already set)

    // Apply explicit alignment padding to total size
    if (ast->layout.align > 1) {
        int a = ast->layout.align;
        total_size = (total_size + a - 1) & ~(a - 1);
    }

    // 3. Build the Type and register it
    Type *layout_type = type_layout(ast->layout.name,
                                    fields, ast->layout.field_count,
                                    total_size,
                                    ast->layout.packed,
                                    ast->layout.align);
    env_insert_layout(ctx->env, ast->layout.name, layout_type, NULL);

    // 4. Force creation of the LLVM named struct type now
    type_to_llvm(ctx, layout_type);

    printf("Layout %s :: %d bytes (%d fields%s%s)\n",
           ast->layout.name, total_size, ast->layout.field_count,
           ast->layout.packed ? ", packed" : "",
           ast->layout.align  ? ", aligned" : "");
}


static void check_predicate_name(CodegenContext *ctx, const char *name,
                                  Type *return_type, AST *ast) {
    if (!name || !return_type) return;
    size_t len = strlen(name);
    bool ends_with_q  = (len > 0 && name[len - 1] == '?');
    bool ends_with_qi = (len > 1 && name[len - 2] == '?' && name[len - 1] == '!');
    bool returns_bool = (return_type->kind == TYPE_BOOL);

    if (returns_bool && !ends_with_q && !ends_with_qi) {
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' returns Bool, making it a "
                      "predicate — predicate functions must end with '?', "
                      "rename to '%s?'",
                      parser_get_filename(), ast->line, ast->column,
                      name, name);
    }

    if (!returns_bool && ends_with_q && !ends_with_qi) {
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' ends with '?' making it "
                      "is a predicate, but it returns %s — predicates must "
                      "return Bool, either fix the return type or rename to "
                      "remove the '?'",
                      parser_get_filename(), ast->line, ast->column,
                      name, type_to_string(return_type));
    }
}

static bool name_is_impure(const char *name) {
    if (!name || !*name) return false;
    return name[strlen(name) - 1] == '!';
}


static void check_purity(CodegenContext *ctx, const char *caller,
                         const char *callee, AST *ast) {
    if (!caller || !callee) return;
    if (!name_is_impure(callee)) return;

    /* Build the suggested impure name for the caller:
     * foo  -> foo!
     * foo? -> foo?!  (impure predicate)              */
    char impure_name[256];
    snprintf(impure_name, sizeof(impure_name), "%s!", caller);

    bool is_mutation = (strcmp(callee, "set!")        == 0 ||
                        strcmp(callee, "string-set!") == 0 ||
                        strcmp(callee, "array-set!")  == 0);

    if (is_mutation && ast->list.count >= 2) {
        AST *target = ast->list.items[1];
        const char *target_name = NULL;
        char base_name[256] = {0};

        if (target->type == AST_SYMBOL) {
            const char *dot = strchr(target->symbol, '.');
            if (dot) {
                size_t len = dot - target->symbol;
                strncpy(base_name, target->symbol, len < 255 ? len : 255);
                target_name = base_name;
            } else {
                target_name = target->symbol;
            }
        }

        if (target_name && env_is_local(ctx->env, target_name)) {
            if (name_is_impure(caller)) {
                size_t clen = strlen(caller);
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' only mutates local "
                              "variables — local mutation is pure, the '!' "
                              "suffix is unnecessary, rename to '%.*s'",
                              parser_get_filename(), ast->line, ast->column,
                              caller, (int)(clen - 1), caller);
            }
            return;
        }

        if (!name_is_impure(caller) && target_name) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: pure function '%s' mutates "
                          "global variable '%s' via '%s' — global mutation is "
                          "a side effect, rename to '%s' to declare it impure",
                          parser_get_filename(), ast->line, ast->column,
                          caller, target_name, callee, impure_name);
        }
        return;
    }

    if (!name_is_impure(caller)) {
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: pure function '%s' calls impure "
                      "function '%s' — rename to '%s' to declare it impure",
                      parser_get_filename(), ast->line, ast->column,
                      caller, callee, impure_name);
    }
}

CodegenResult codegen_expr(CodegenContext *ctx, AST *ast) {
    CodegenResult result = {NULL, NULL};

    switch (ast->type) {
    case AST_NUMBER: {
        fprintf(stderr, ">>> CODEGEN NUMBER: literal_str='%s', number=%g\n",
                ast->literal_str ? ast->literal_str : "NULL", ast->number);

        Type *num_type = infer_literal_type(ast->number, ast->literal_str);

        fprintf(stderr, ">>> INFERRED TYPE: %s\n", type_to_string(num_type));

        result.type = num_type;

        if (type_is_float(num_type)) {
            result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), ast->number);
        } else {
            result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), (long long)ast->number, 0);
        }
        return result;
    }
    case AST_CHAR: {
        result.type = type_char();
        result.value = LLVMConstInt(LLVMInt8TypeInContext(ctx->context), ast->character, 0);
        return result;
    }

    case AST_SYMBOL: {
        if (strcmp(ast->symbol, "nil") == 0) {
            LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
            result.value    = LLVMConstPointerNull(ptr);
            result.type     = type_unknown();
            return result;
        }
        // Boolean literals
        if (strcmp(ast->symbol, "True") == 0) {
            result.value = LLVMConstInt(LLVMInt1TypeInContext(ctx->context), 1, 0);
            result.type  = type_bool();
            return result;
        }
        if (strcmp(ast->symbol, "False") == 0) {
            result.value = LLVMConstInt(LLVMInt1TypeInContext(ctx->context), 0, 0);
            result.type  = type_bool();
            return result;
        }

        // ── Dot-access: p.x ─────────────────────────────────────────────────
        const char *dot = strchr(ast->symbol, '.');
        if (dot && dot != ast->symbol) {
            char var_name[256];
            size_t vlen = dot - ast->symbol;
            if (vlen >= sizeof(var_name)) vlen = sizeof(var_name) - 1;
            memcpy(var_name, ast->symbol, vlen);
            var_name[vlen] = '\0';
            const char *field_name = dot + 1;

            EnvEntry *base_entry = env_lookup(ctx->env, var_name);
            if (base_entry && base_entry->type && base_entry->type->kind == TYPE_LAYOUT) {
                Type *lay = base_entry->type;
                int field_idx = -1;
                for (int i = 0; i < lay->layout_field_count; i++) {
                    if (strcmp(lay->layout_fields[i].name, field_name) == 0) {
                        field_idx = i;
                        break;
                    }
                }
                if (field_idx < 0) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: layout '%s' has no field '%s'",
                                  parser_get_filename(), ast->line, ast->column,
                                  lay->layout_name, field_name);
                }
                // Get the underlying struct type (strip pointer wrapping from type_to_llvm)
                char struct_name[256];
                snprintf(struct_name, sizeof(struct_name), "layout.%s", lay->layout_name);
                LLVMTypeRef struct_llvm = LLVMGetTypeByName2(ctx->context, struct_name);
                if (!struct_llvm) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: LLVM struct type for '%s' not found",
                                  parser_get_filename(), ast->line, ast->column, lay->layout_name);
                }
                /* LLVMValueRef zero      = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0); */
                /* LLVMValueRef fidx      = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), field_idx, 0); */
                /* LLVMValueRef indices[] = {zero, fidx}; */
                /* LLVMValueRef gep       = LLVMBuildGEP2(ctx->builder, struct_llvm, */
                /*                                        base_entry->value, indices, 2, "fld_ptr"); */
                /* Type *ft = lay->layout_fields[field_idx].type; */
                /* result.value = LLVMBuildLoad2(ctx->builder, type_to_llvm(ctx, ft), gep, field_name); */
                /* result.type  = type_clone(ft); */
                /* return result; */
                LLVMTypeRef  ptr_t2   = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef get_fn2  = LLVMGetNamedFunction(ctx->module, "__layout_ptr_get");
                if (!get_fn2) {
                    LLVMTypeRef ft2 = LLVMFunctionType(ptr_t2, &ptr_t2, 1, 0);
                    get_fn2 = LLVMAddFunction(ctx->module, "__layout_ptr_get", ft2);
                    LLVMSetLinkage(get_fn2, LLVMExternalLinkage);
                }
                LLVMValueRef vname_str = LLVMBuildGlobalStringPtr(ctx->builder, var_name, "lay_name");
                LLVMValueRef heap_ptr2 = LLVMBuildCall2(ctx->builder,
                                             LLVMFunctionType(ptr_t2, &ptr_t2, 1, 0),
                                             get_fn2, &vname_str, 1, "lay_ptr");
                LLVMValueRef zero      = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                LLVMValueRef fidx      = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), field_idx, 0);
                LLVMValueRef indices[] = {zero, fidx};
                LLVMValueRef gep       = LLVMBuildGEP2(ctx->builder, struct_llvm,
                                                       heap_ptr2, indices, 2, "fld_ptr");
                Type *ft = lay->layout_fields[field_idx].type;
                result.value = LLVMBuildLoad2(ctx->builder, type_to_llvm(ctx, ft), gep, field_name);
                result.type  = type_clone(ft);
                return result;

            }
            // base exists but is not a layout — fall through to normal resolution
            // (handles module-qualified symbols like M.phi)
        }

        // ── Normal variable / module-qualified symbol ────────────────────────
        EnvEntry *entry = resolve_symbol_with_modules(ctx, ast->symbol, ast);
        if (!entry) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: unbound variable: %s",
                          parser_get_filename(), ast->line, ast->column, ast->symbol);
        }

        if (entry->type && entry->type->kind == TYPE_LAYOUT) {
            LLVMTypeRef  ptr_t  = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
            LLVMValueRef get_fn = LLVMGetNamedFunction(ctx->module, "__layout_ptr_get");
            if (!get_fn) {
                LLVMTypeRef ft = LLVMFunctionType(ptr_t, &ptr_t, 1, 0);
                get_fn = LLVMAddFunction(ctx->module, "__layout_ptr_get", ft);
                LLVMSetLinkage(get_fn, LLVMExternalLinkage);
            }
            LLVMValueRef name_str = LLVMBuildGlobalStringPtr(ctx->builder,
                                        ast->symbol, "lay_name");
            result.type  = entry->type;
            result.value = LLVMBuildCall2(ctx->builder,
                LLVMFunctionType(ptr_t, &ptr_t, 1, 0),
                get_fn, &name_str, 1, "lay_ptr");
            return result;
        }

        if (entry->kind == ENV_BUILTIN) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' is a built-in and cannot be passed as a value",
                          parser_get_filename(), ast->line, ast->column, ast->symbol);
        }
        if (!entry->type) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' has no type information",
                          parser_get_filename(), ast->line, ast->column, ast->symbol);
        }

        fprintf(stderr, "[symbol load] name='%s' entry=%p entry->value=%p is_global=%d\n",
                ast->symbol, (void*)entry, (void*)entry->value,
                entry->value ? (LLVMIsAGlobalVariable(entry->value) != NULL) : -1);

        result.type  = type_clone(entry->type);
        result.value = LLVMBuildLoad2(ctx->builder, type_to_llvm(ctx, entry->type),
                                      entry->value, ast->symbol);
        return result;
    }

    case AST_STRING: {
        result.type = type_string();
        result.value = LLVMBuildGlobalStringPtr(ctx->builder, ast->string, "str");
        return result;
    }

    case AST_ASM: {
        // Inline assembly - should only appear as function body
        // We need to know the current function's parameters

        CODEGEN_ERROR(ctx, "%s:%d:%d: error: inline asm can only be used as a function body",
                parser_get_filename(), ast->line, ast->column);
    }

    case AST_KEYWORD: {
        // Keywords are represented as strings with a special marker
        // For now, just return the keyword name as a string
        result.type = type_keyword();

        // Create a global string for the keyword (with : prefix for clarity)
        char keyword_str[256];
        snprintf(keyword_str, sizeof(keyword_str), ":%s", ast->keyword);
        result.value = LLVMBuildGlobalStringPtr(ctx->builder, keyword_str, "keyword");
        return result;
    }

    case AST_RATIO: {
        // Create runtime ratio value
        LLVMValueRef fn = get_rt_value_ratio(ctx);
        LLVMValueRef num = LLVMConstInt(LLVMInt64TypeInContext(ctx->context),
                                        ast->ratio.numerator, 1);
        LLVMValueRef denom = LLVMConstInt(LLVMInt64TypeInContext(ctx->context),
                                          ast->ratio.denominator, 1);
        LLVMValueRef args[] = {num, denom};

        result.type = type_ratio();
        result.value = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                      fn, args, 2, "ratio");
        return result;
    }

    case AST_ADDRESS_OF: {
        AST *operand = ast->list.items[0];
        if (operand->type == AST_SYMBOL) {
            EnvEntry *entry = env_lookup(ctx->env, operand->symbol);
            if (!entry) {
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: unbound variable: %s",
                              parser_get_filename(), ast->line, ast->column, operand->symbol);
                return result;
            }
            LLVMValueRef addr;
            LLVMTypeRef  i64 = LLVMInt64TypeInContext(ctx->context);
            if (entry->kind == ENV_FUNC) {
                addr = LLVMBuildPtrToInt(ctx->builder, entry->func_ref, i64, "fn_addr");
            } else if (entry->kind == ENV_VAR) {
                addr = LLVMBuildPtrToInt(ctx->builder, entry->value, i64, "var_addr");
            } else if (entry->kind == ENV_BUILTIN) {
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: cannot take address of builtin '%s'",
                              parser_get_filename(), ast->line, ast->column, operand->symbol);
                return result;
            } else {
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: cannot take address of '%s'",
                              parser_get_filename(), ast->line, ast->column, operand->symbol);
                return result;
            }
            result.value = addr;
            result.type  = type_hex();
            return result;
        }
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: '&' operand must be a symbol",
                      parser_get_filename(), ast->line, ast->column);
        return result;
    }

    case AST_ARRAY: {
        // Infer element type from first element - preserve literal types!
        Type *elem_type = NULL;
        if (ast->array.element_count > 0) {
            CodegenResult first = codegen_expr(ctx, ast->array.elements[0]);
            elem_type = first.type;

            // IMPORTANT: Clone the type so each array has its own type object
            if (elem_type) {
                elem_type = type_clone(elem_type);
            }
        } else {
            // Empty array - default to Int
            elem_type = type_int();
        }

        // Create array type
        result.type = type_arr(elem_type, (int)ast->array.element_count);
        LLVMTypeRef arr_type = type_to_llvm(ctx, result.type);

        // Allocate array on stack
        result.value = LLVMBuildAlloca(ctx->builder, arr_type, "array");

        // Initialize each element
        for (size_t i = 0; i < ast->array.element_count; i++) {
            CodegenResult elem = codegen_expr(ctx, ast->array.elements[i]);

            // Convert element to correct type if needed
            LLVMValueRef elem_val = elem.value;
            if (elem.type && elem_type) {
                if (type_is_integer(elem_type) && type_is_float(elem.type)) {
                    elem_val = LLVMBuildFPToSI(ctx->builder, elem.value,
                                               type_to_llvm(ctx, elem_type), "to_int");
                } else if (type_is_float(elem_type) && type_is_integer(elem.type)) {
                    elem_val = LLVMBuildSIToFP(ctx->builder, elem.value,
                                               type_to_llvm(ctx, elem_type), "to_float");
                }
            }

            // Get pointer to element
            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
            LLVMValueRef idx = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), i, 0);
            LLVMValueRef indices[] = {zero, idx};
            LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder, arr_type,
                                                  result.value, indices, 2, "elem_ptr");

            // Store element
            LLVMBuildStore(ctx->builder, elem_val, elem_ptr);
        }

        return result;
    }


    case AST_RANGE: {
        LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);

        CodegenResult start_r = codegen_expr(ctx, ast->range.start);
        LLVMValueRef start_val = type_is_float(start_r.type)
            ? LLVMBuildFPToSI(ctx->builder, start_r.value, i64, "range_start")
            : start_r.value;

        // ----------------------------------------------------------------
        // [lo .. hi]  — bracket syntax with end => eager stack array
        // ----------------------------------------------------------------
        if (ast->range.is_array) {
            // Parser already errors on infinite [lo..], so end is always present.
            CodegenResult end_r = codegen_expr(ctx, ast->range.end);
            LLVMValueRef end_val = type_is_float(end_r.type)
                ? LLVMBuildFPToSI(ctx->builder, end_r.value, i64, "range_end")
                : end_r.value;

            LLVMValueRef step_val;
            if (ast->range.step != NULL) {
                CodegenResult step_r = codegen_expr(ctx, ast->range.step);
                LLVMValueRef next_val = type_is_float(step_r.type)
                    ? LLVMBuildFPToSI(ctx->builder, step_r.value, i64, "range_next")
                    : step_r.value;
                step_val = LLVMBuildSub(ctx->builder, next_val, start_val, "range_step");
            } else {
                step_val = LLVMConstInt(i64, 1, 0);
            }

            // count = (end - start) / step + 1
            LLVMValueRef one   = LLVMConstInt(i64, 1, 0);
            LLVMValueRef diff  = LLVMBuildSub(ctx->builder, end_val, start_val, "diff");
            LLVMValueRef count = LLVMBuildAdd(ctx->builder,
                                              LLVMBuildSDiv(ctx->builder, diff, step_val, "divn"),
                                              one, "count");

            // Stack-allocate count × i64
            LLVMValueRef alloc = LLVMBuildArrayAlloca(ctx->builder, i64, count, "range_arr");

            // Loop: for i = 0; i < count; i++   arr[i] = start + i * step
            LLVMValueRef func    = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
            LLVMBasicBlockRef loop_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "arr_loop");
            LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "arr_body");
            LLVMBasicBlockRef cont_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "arr_cont");

            LLVMValueRef idx_ptr = LLVMBuildAlloca(ctx->builder, i64, "idx_ptr");
            LLVMBuildStore(ctx->builder, LLVMConstInt(i64, 0, 0), idx_ptr);
            LLVMBuildBr(ctx->builder, loop_bb);

            // loop header — check i < count
            LLVMPositionBuilderAtEnd(ctx->builder, loop_bb);
            LLVMValueRef idx  = LLVMBuildLoad2(ctx->builder, i64, idx_ptr, "idx");
            LLVMValueRef cond = LLVMBuildICmp(ctx->builder, LLVMIntSLT, idx, count, "cond");
            LLVMBuildCondBr(ctx->builder, cond, body_bb, cont_bb);

            // loop body — store start + idx * step
            LLVMPositionBuilderAtEnd(ctx->builder, body_bb);
            LLVMValueRef idx2  = LLVMBuildLoad2(ctx->builder, i64, idx_ptr, "idx2");
            LLVMValueRef elem  = LLVMBuildAdd(ctx->builder, start_val,
                                              LLVMBuildMul(ctx->builder, idx2, step_val, "scaled"),
                                              "elem");
            LLVMValueRef gep   = LLVMBuildGEP2(ctx->builder, i64, alloc, &idx2, 1, "gep");
            LLVMBuildStore(ctx->builder, elem, gep);
            LLVMValueRef next  = LLVMBuildAdd(ctx->builder, idx2, one, "next");
            LLVMBuildStore(ctx->builder, next, idx_ptr);
            LLVMBuildBr(ctx->builder, loop_bb);

            LLVMPositionBuilderAtEnd(ctx->builder, cont_bb);

            result.value = alloc;
            result.type  = type_arr(type_int(), 0);  // 0 = dynamic size
            return result;
        }

        // ----------------------------------------------------------------
        // (lo ..)  /  (lo, next ..)  /  (lo .. hi)  /  (lo, next .. hi)
        // — paren syntax => lazy list
        // ----------------------------------------------------------------
        if (ast->range.end == NULL) {
            // Infinite
            if (ast->range.step != NULL) {
                CodegenResult step_r = codegen_expr(ctx, ast->range.step);
                LLVMValueRef next_val = type_is_float(step_r.type)
                    ? LLVMBuildFPToSI(ctx->builder, step_r.value, i64, "range_next")
                    : step_r.value;
                LLVMValueRef step_val = LLVMBuildSub(ctx->builder, next_val, start_val, "range_step");

                LLVMValueRef fn = get_rt_list_from_step(ctx);
                LLVMTypeRef ft_args[] = {i64, i64};
                LLVMValueRef args[] = {start_val, step_val};
                result.value = LLVMBuildCall2(ctx->builder,
                                              LLVMFunctionType(ptr, ft_args, 2, 0), fn, args, 2, "list_from_step");
            } else {
                LLVMValueRef fn = get_rt_list_from(ctx);
                LLVMTypeRef ft_args[] = {i64};
                LLVMValueRef args[] = {start_val};
                result.value = LLVMBuildCall2(ctx->builder,
                                              LLVMFunctionType(ptr, ft_args, 1, 0), fn, args, 1, "list_from");
            }
        } else {
            // Finite
            CodegenResult end_r = codegen_expr(ctx, ast->range.end);
            LLVMValueRef end_val = type_is_float(end_r.type)
                ? LLVMBuildFPToSI(ctx->builder, end_r.value, i64, "range_end")
                : end_r.value;

            if (ast->range.step != NULL) {
                CodegenResult step_r = codegen_expr(ctx, ast->range.step);
                LLVMValueRef next_val = type_is_float(step_r.type)
                    ? LLVMBuildFPToSI(ctx->builder, step_r.value, i64, "range_next")
                    : step_r.value;
                LLVMValueRef step_val = LLVMBuildSub(ctx->builder, next_val, start_val, "range_step");

                // count = (end - start) / step + 1
                LLVMValueRef one   = LLVMConstInt(i64, 1, 0);
                LLVMValueRef diff  = LLVMBuildSub(ctx->builder, end_val, start_val, "diff");
                LLVMValueRef count = LLVMBuildAdd(ctx->builder,
                                                  LLVMBuildSDiv(ctx->builder, diff, step_val, "divn"),
                                                  one, "n");

                LLVMValueRef from_fn = get_rt_list_from_step(ctx);
                LLVMTypeRef ft_from_args[] = {i64, i64};
                LLVMValueRef from_args[] = {start_val, step_val};
                LLVMValueRef inf = LLVMBuildCall2(ctx->builder,
                                                  LLVMFunctionType(ptr, ft_from_args, 2, 0), from_fn, from_args, 2, "inf");

                LLVMValueRef take_fn = get_rt_list_take(ctx);
                LLVMTypeRef ft_take_args[] = {ptr, i64};
                LLVMValueRef take_args[] = {inf, count};
                result.value = LLVMBuildCall2(ctx->builder,
                                              LLVMFunctionType(ptr, ft_take_args, 2, 0), take_fn, take_args, 2, "range_step");
            } else {
                LLVMValueRef fn = get_rt_list_range(ctx);
                LLVMTypeRef ft_args[] = {i64, i64};
                LLVMValueRef args[] = {start_val, end_val};
                result.value = LLVMBuildCall2(ctx->builder,
                                              LLVMFunctionType(ptr, ft_args, 2, 0), fn, args, 2, "range");
            }
        }

        result.type = type_list(NULL);
        return result;
    }

    case AST_LIST: {
        if (ast->list.count == 0) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: empty list not supported",
                    parser_get_filename(), ast->line, ast->column);
        }

        AST *head = ast->list.items[0];

        if (head->type == AST_SYMBOL) {
            check_purity(ctx, ctx->current_function_name, head->symbol, ast);
            // Handle 'define' special form
            if (strcmp(head->symbol, "define") == 0) {
                if (ast->list.count < 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'define' requires at least 2 arguments",
                            parser_get_filename(), ast->line, ast->column);
                }

                AST *name_expr = ast->list.items[1];
                AST *value_expr = ast->list.items[2];

                char *var_name = NULL;
                Type *explicit_type = NULL;

                // Check if name is a type annotation [name :: Type]
                if (name_expr->type == AST_LIST) {
                    explicit_type = parse_type_annotation(name_expr);
                    if (explicit_type && name_expr->list.count > 0) {
                        if (name_expr->list.items[0]->type == AST_SYMBOL) {
                            var_name = name_expr->list.items[0]->symbol;
                        }
                    } else {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'define' name must be symbol or type annotation",
                                parser_get_filename(), ast->line, ast->column);
                    }
                } else if (name_expr->type == AST_SYMBOL) {
                    var_name = name_expr->symbol;
                } else {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'define' name must be symbol or type annotation",
                            parser_get_filename(), ast->line, ast->column);
                }

                if (!var_name) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'define' invalid name format",
                            parser_get_filename(), ast->line, ast->column);
                }

                // Check if value is a lambda - codegen it as a function
                if (value_expr->type == AST_LAMBDA) {
                    // Extract lambda info
                    AST *lambda = value_expr;

                    /* // Build parameter types */
                    /* LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * lambda->lambda.param_count); */
                    /* EnvParam *env_params = malloc(sizeof(EnvParam) * lambda->lambda.param_count); */

                    // ── Lambda lifting ────────────────────────────────────────────
                    char **lifted_vars  = NULL;
                    int    lifted_count = 0;

                    bool is_inner = (LLVMGetInsertBlock(ctx->builder) != NULL &&
                                     LLVMGetBasicBlockParent(
                                         LLVMGetInsertBlock(ctx->builder)) != NULL &&
                                     LLVMGetBasicBlockParent(
                                         LLVMGetInsertBlock(ctx->builder)) != ctx->init_fn);

                    if (is_inner) {
                        const char **bound = malloc(sizeof(char*) * lambda->lambda.param_count);
                        for (int i = 0; i < lambda->lambda.param_count; i++)
                            bound[i] = lambda->lambda.params[i].name;
                        const char *self_name = var_name;
                        for (int i = 0; i < lambda->lambda.body_count; i++) {
                            collect_free_vars(lambda->lambda.body_exprs[i],
                                              bound, lambda->lambda.param_count,
                                              &self_name, 1,
                                              &lifted_vars, &lifted_count,
                                              ctx->env);
                        }
                        free(bound);
                    }

                    int total_params = lambda->lambda.param_count + lifted_count;

                    // Build parameter types
                    LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * (total_params ? total_params : 1));
                    EnvParam    *env_params  = malloc(sizeof(EnvParam)    * (total_params ? total_params : 1));



                    for (int i = 0; i < lambda->lambda.param_count; i++) {
                        ASTParam *param = &lambda->lambda.params[i];

                        Type *param_type = NULL;
                        if (param->type_name) {
                            param_type = type_from_name(param->type_name);
                            if (!param_type) {
                                CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown parameter type '%s'",
                                        parser_get_filename(), lambda->line, lambda->column,
                                        param->type_name);
                            }
                        } else {
                            /* param_type = type_float(); // default */
                            // Generic/polymorphic parameter — opaque pointer
                            param_type = type_unknown();
                        }
                        param_types[i] = type_to_llvm(ctx, param_type);
                        env_params[i].name = strdup(param->name);
                        env_params[i].type = param_type;

                        param_types[i] = type_to_llvm(ctx, param_type);
                        env_params[i].name = strdup(param->name);
                        env_params[i].type = param_type;
                    }

                    // Append lifted captured variables as hidden extra params
                    for (int i = 0; i < lifted_count; i++) {
                        int idx = lambda->lambda.param_count + i;
                        EnvEntry *cap = env_lookup(ctx->env, lifted_vars[i]);
                        Type *cap_type = (cap && cap->type) ? type_clone(cap->type) : type_int();
                        param_types[idx]     = type_to_llvm(ctx, cap_type);
                        env_params[idx].name = strdup(lifted_vars[i]);
                        env_params[idx].type = cap_type;
                    }


                    // Determine return type

                    Type *ret_type = NULL;
                    if (lambda->lambda.return_type) {
                        ret_type = type_from_name(lambda->lambda.return_type);
                        if (!ret_type) {
                            CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown return type '%s'",
                                    parser_get_filename(), ast->line, ast->column,
                                    lambda->lambda.return_type);
                        }
                    } else {
                        ret_type = type_float(); // default when no annotation
                    }

                    LLVMTypeRef ret_llvm_type = type_to_llvm(ctx, ret_type);

                    // Create function type
                    LLVMTypeRef func_type = LLVMFunctionType(ret_llvm_type, param_types,
                                                             total_params, 0);

                    // Create function
                    LLVMValueRef func = LLVMAddFunction(ctx->module, var_name, func_type);


                    // Apply naked attribute if requested
                    if (lambda->lambda.naked) {
                        unsigned kind = LLVMGetEnumAttributeKindForName("naked", 5);
                        if (kind != 0) {
                            LLVMAttributeRef attr = LLVMCreateEnumAttribute(ctx->context, kind, 0);
                            LLVMAddAttributeAtIndex(func, LLVMAttributeFunctionIndex, attr);
                        } else {
                            // Fallback: string attribute
                            LLVMAttributeRef attr = LLVMCreateStringAttribute(
                                                                              ctx->context,
                                                                              "naked", 5,
                                                                              "", 0);
                            LLVMAddAttributeAtIndex(func, LLVMAttributeFunctionIndex, attr);
                        }
                    }

                    LLVMDumpValue(func);

                    // Forward-declare into the PARENT env before codegenning the body
                    // so recursive calls can resolve the function by name.
                    env_insert_func(ctx->env, var_name,
                                    clone_params(env_params, total_params),
                                    total_params,
                                    type_clone(ret_type), func,
                                    lambda->lambda.docstring);
                    EnvEntry *e1161 = env_lookup(ctx->env, var_name);
                    if (e1161) e1161->llvm_name = strdup(LLVMGetValueName(func));
                    if (e1161) e1161->lifted_count = lifted_count;

                    // Create entry block
                    LLVMBasicBlockRef entry_block = LLVMAppendBasicBlockInContext(ctx->context, func, "entry");

                    // Save current insert point
                    LLVMBasicBlockRef saved_block = LLVMGetInsertBlock(ctx->builder);

                    // Position at function entry
                    LLVMPositionBuilderAtEnd(ctx->builder, entry_block);

                    // Create a new scope for function parameters
                    Env *saved_env = ctx->env;
                    ctx->env = env_create_child(saved_env);

                    // Add parameters to the function's environment
                    for (int i = 0; i < lambda->lambda.param_count; i++) {
                        LLVMValueRef param = LLVMGetParam(func, i);
                        LLVMSetValueName2(param, lambda->lambda.params[i].name,
                                          strlen(lambda->lambda.params[i].name));

                        if (lambda->lambda.naked) {
                            continue;  // no alloca, no store, no env insert
                        }

                        LLVMValueRef param_alloca = LLVMBuildAlloca(ctx->builder,
                                                                    param_types[i],
                                                                    lambda->lambda.params[i].name);
                        LLVMBuildStore(ctx->builder, param, param_alloca);
                        env_insert(ctx->env, lambda->lambda.params[i].name,
                                   type_clone(env_params[i].type), param_alloca);
                    }

                    // Add lifted captured variables as extra params inside the function
                    for (int i = 0; i < lifted_count; i++) {
                        int idx = lambda->lambda.param_count + i;
                        LLVMValueRef lp = LLVMGetParam(func, idx);
                        LLVMSetValueName2(lp, lifted_vars[i], strlen(lifted_vars[i]));
                        LLVMValueRef alloca = LLVMBuildAlloca(ctx->builder, param_types[idx], lifted_vars[i]);
                        LLVMBuildStore(ctx->builder, lp, alloca);
                        env_insert(ctx->env, lifted_vars[i], type_clone(env_params[idx].type), alloca);
                    }


                    // Codegen function body
                    CodegenResult body_result = {NULL, NULL};
                    /* CodegenResult body_result; */

                    if (lambda->lambda.body->type == AST_ASM) {
                        RegisterAllocator reg_alloc;
                        reg_alloc_init(&reg_alloc);
                        AsmContext asm_ctx = {
                            .params      = env_params,
                            .param_count = lambda->lambda.param_count,
                            .reg_alloc   = &reg_alloc,
                            .naked       = lambda->lambda.naked
                        };
                        int asm_inst_count;
                        AsmInstruction *asm_instructions = preprocess_asm(
                            lambda->lambda.body, &asm_ctx, &asm_inst_count);

                        if (lambda->lambda.naked) {
                            body_result.value = codegen_inline_asm(
                                ctx->context, ctx->builder,
                                asm_instructions, asm_inst_count,
                                ret_type, NULL, 0, true);
                        } else {
                            LLVMValueRef *param_values = malloc(
                                sizeof(LLVMValueRef) * lambda->lambda.param_count);
                            for (int i = 0; i < lambda->lambda.param_count; i++)
                                param_values[i] = LLVMGetParam(func, i);
                            body_result.value = codegen_inline_asm(
                                ctx->context, ctx->builder,
                                asm_instructions, asm_inst_count,
                                ret_type, param_values,
                                lambda->lambda.param_count, false);
                            free(param_values);
                        }
                        body_result.type = ret_type;
                        free_asm_instructions(asm_instructions, asm_inst_count);
                    } else {
                        const char *prev_fn_name = ctx->current_function_name;
                        ctx->current_function_name = var_name;
                        for (int i = 0; i < lambda->lambda.body_count; i++) {
                            body_result = codegen_expr(ctx, lambda->lambda.body_exprs[i]);
                        }
                        ctx->current_function_name = prev_fn_name;

                        if (!body_result.type) {
                            body_result.type  = type_clone(ret_type);
                            body_result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                        }
                    }

                    // Convert return value to correct type if needed
                    LLVMValueRef ret_value = body_result.value;
                    if (body_result.type && ret_type) {
                        if (type_is_integer(ret_type) && type_is_float(body_result.type)) {
                            ret_value = LLVMBuildFPToSI(ctx->builder, body_result.value,
                                                        ret_llvm_type, "ret_conv");
                        } else if (type_is_float(ret_type) && type_is_integer(body_result.type)) {
                            ret_value = LLVMBuildSIToFP(ctx->builder, body_result.value,
                                                        ret_llvm_type, "ret_conv");
                        } else if (ret_type->kind == TYPE_CHAR && type_is_integer(body_result.type)) {
                            // Int -> Char: truncate to i8
                            ret_value = LLVMBuildTrunc(ctx->builder, body_result.value,
                                                       LLVMInt8TypeInContext(ctx->context), "ret_char");
                        } else if (type_is_integer(ret_type) && body_result.type->kind == TYPE_CHAR) {
                            // Char -> Int: zero-extend to i64
                            ret_value = LLVMBuildZExt(ctx->builder, body_result.value,
                                                      ret_llvm_type, "ret_int");
                        } else if (ret_type->kind == TYPE_BOOL) {
                            // Coerce any int/i64 to i1
                            if (LLVMTypeOf(ret_value) != LLVMInt1TypeInContext(ctx->context))
                                ret_value = LLVMBuildTrunc(ctx->builder, ret_value,
                                                           LLVMInt1TypeInContext(ctx->context), "to_bool");
                        } else if (type_is_integer(ret_type) &&
                                   LLVMTypeOf(ret_value) == LLVMInt1TypeInContext(ctx->context)) {
                            // comparison result (i1) returned from an Int-typed function -> zext to i64
                            ret_value = LLVMBuildZExt(ctx->builder, ret_value,
                                                      ret_llvm_type, "bool_to_int");
                        } else if (type_is_integer(ret_type) &&
                                   LLVMTypeOf(ret_value) == LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0)) {
                            // boxed RuntimeValue* returned from an Int-typed function -> unbox to i64
                            LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                            LLVMTypeRef uft = LLVMFunctionType(ret_llvm_type, &ptr, 1, 0);
                            ret_value = LLVMBuildCall2(ctx->builder, uft,
                                                       get_rt_unbox_int(ctx), &ret_value, 1, "unboxed_ret");
                        }
                    }


                    // Naked functions contain their own ret in the asm block
                    if (lambda->lambda.naked) {
                        LLVMBuildUnreachable(ctx->builder);
                    } else {
                        LLVMBuildRet(ctx->builder, ret_value);
                    }

                    // Restore environment and insert point
                    env_free(ctx->env);
                    ctx->env = saved_env;

                    if (saved_block) {
                        LLVMPositionBuilderAtEnd(ctx->builder, saved_block);
                    }

                    // Insert function into symbol table
                    env_insert_func(ctx->env, var_name, env_params, total_params,
                                   ret_type, func, lambda->lambda.docstring);
                    EnvEntry *efinal = env_lookup(ctx->env, var_name);
                    if (efinal) efinal->lifted_count = lifted_count;
                    if (efinal) efinal->source_ast = ast;  // borrow — ast lives in the parse tree

                    if (ast->lambda.alias_name) {
                        env_insert_func(ctx->env,
                                        ast->lambda.alias_name,
                                        clone_params(env_params, lambda->lambda.param_count),
                                        lambda->lambda.param_count,
                                        type_clone(ret_type),
                                        func,
                                        lambda->lambda.docstring);
                        printf("Alias: %s -> %s\n", ast->lambda.alias_name, var_name);
                    }

                    ctx->current_function_name = NULL;
                    check_predicate_name(ctx, var_name, ret_type, ast);
                    if (ctx->module_ctx && !should_export_symbol(ctx->module_ctx, var_name)) {


                        printf("Defined %s :: Fn (...) -> %s (private)\n",
                               var_name, type_to_string(ret_type));
                    } else {
                        printf("Defined %s :: Fn (...) -> %s\n",
                               var_name, type_to_string(ret_type));
                    }

                    /* /\* printf("Defined %s :: Fn (", var_name); *\/ */

                    /* for (int i = 0; i < lambda->lambda.param_count; i++) { */
                    /*     if (i > 0) printf(" "); */
                    /*     printf("%s", lambda->lambda.params[i].name); */
                    /* } */
                    /* printf(") -> %s\n", type_to_string(ret_type)); */

                    free(param_types);

                    // Return a dummy value
                    result.type = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Codegen the value
                // Special handling for empty arrays - DISALLOW without type annotation
                CodegenResult value_result;
                if (value_expr->type == AST_ARRAY && value_expr->array.element_count == 0) {
                    if (!explicit_type || explicit_type->kind != TYPE_ARR) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: cannot infer type of empty array - explicit type annotation required",
                                parser_get_filename(), ast->line, ast->column);
                    }
                    // Don't call codegen_expr for empty arrays - we'll handle them below in zero-init
                    value_result.type = explicit_type;
                    value_result.value = NULL;
                } else {
                    // Normal codegen for non-empty values
                    value_result = codegen_expr(ctx, value_expr);
                }


                // Validate array size if type annotation specifies size
                if (explicit_type && explicit_type->kind == TYPE_ARR &&
                    explicit_type->arr_size >= 0 && value_result.type &&
                    value_result.type->kind == TYPE_ARR) {

                    int declared_size = explicit_type->arr_size;
                    int actual_size = value_result.type->arr_size;

                    // Allow empty initialization (will zero-init)
                    if (actual_size > 0 && actual_size != declared_size) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: array size mismatch - declared size %d but got %d elements",
                                parser_get_filename(), ast->line, ast->column,
                                declared_size, actual_size);
                    }
                }

                // Infer or verify type
                Type *inferred_type = value_result.type;

                if (!inferred_type) {
                    if (value_expr->type == AST_CHAR) {
                        inferred_type = type_char();
                    } else if (value_expr->type == AST_STRING) {
                        inferred_type = type_string();
                    } else {
                        inferred_type = type_float();
                    }
                }

                Type *final_type = explicit_type ? explicit_type : inferred_type;

                // SPECIAL CASE: Arrays are already allocas from codegen_expr
                // Don't create a new alloca and store - just use the existing one
                if (final_type->kind == TYPE_ARR) {
                    // Handle empty array initialization with explicit type
                    if (value_expr->type == AST_ARRAY && value_expr->array.element_count == 0 &&
                        explicit_type && explicit_type->arr_size > 0) {

                        LLVMTypeRef arr_type = type_to_llvm(ctx, final_type);
                        LLVMValueRef arr;
                        if (is_at_top_level(ctx)) {
                            arr = LLVMGetNamedGlobal(ctx->module, var_name);
                            if (!arr) {
                                arr = LLVMAddGlobal(ctx->module, arr_type, var_name);
                                LLVMSetInitializer(arr, LLVMConstNull(arr_type));
                                LLVMSetLinkage(arr, LLVMExternalLinkage);
                            }
                            // Constant null initializer already zeroes everything
                            // (no element-wise stores needed for globals)
                            env_insert(ctx->env, var_name, final_type, arr);
                            printf("Defined %s :: %s (zero-initialized)\n", var_name, type_to_string(final_type));
                            result.type  = final_type;
                            result.value = arr;
                            return result;
                        }
                        arr = LLVMBuildAlloca(ctx->builder, arr_type, var_name);

                        result.type = final_type;
                        result.value = arr;
                        return result;
                    }

                    // value_result.value is already a pointer to the stack array
                    // Just insert it directly into the environment
                    env_insert(ctx->env, var_name, final_type, value_result.value);

                    printf("Defined %s :: %s\n", var_name, type_to_string(final_type));

                    result.type = final_type;
                    result.value = value_result.value;
                    return result;
                }

                if (final_type->kind == TYPE_LAYOUT) {
                    // Register the heap pointer in the host-side table via a runtime call
                    LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef  i8p   = ptr_t;

                    // Declare __layout_ptr_set(i8* name, i8* ptr) -> void
                    LLVMValueRef set_fn = LLVMGetNamedFunction(ctx->module, "__layout_ptr_set");
                    if (!set_fn) {
                        LLVMTypeRef params[] = {i8p, i8p};
                        LLVMTypeRef ft = LLVMFunctionType(
                            LLVMVoidTypeInContext(ctx->context), params, 2, 0);
                        set_fn = LLVMAddFunction(ctx->module, "__layout_ptr_set", ft);
                        LLVMSetLinkage(set_fn, LLVMExternalLinkage);
                    }
                    LLVMValueRef name_str = LLVMBuildGlobalStringPtr(ctx->builder, var_name, "lay_name");
                    LLVMValueRef set_args[] = {name_str, value_result.value};
                    LLVMTypeRef  set_params[] = {i8p, i8p};
                    LLVMBuildCall2(ctx->builder,
                        LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), set_params, 2, 0),
                        set_fn, set_args, 2, "");

                    // Also keep a global so redeclare_env_symbols tracks it,
                    // but we don't use it for actual pointer storage
                    LLVMValueRef gv = LLVMGetNamedGlobal(ctx->module, var_name);
                    if (!gv) {
                        gv = LLVMAddGlobal(ctx->module, ptr_t, var_name);
                        LLVMSetInitializer(gv, LLVMConstPointerNull(ptr_t));
                        LLVMSetLinkage(gv, LLVMExternalLinkage);
                    }

                    LLVMBuildStore(ctx->builder, value_result.value, gv);
                    Type *layout_type_copy = type_clone(final_type);
                    char *layout_name_copy = strdup(type_to_string(final_type));
                    env_insert(ctx->env, var_name, layout_type_copy, gv);

                    EnvEntry *elayout = env_lookup(ctx->env, var_name);
                    if (elayout) elayout->llvm_name = strdup(LLVMGetValueName(gv));
                    printf("Defined %s :: %s\n", var_name, layout_name_copy);
                    free(layout_name_copy);

                    result.type  = layout_type_copy;
                    result.value = value_result.value;
                    return result;
                }

                // For non-array types: global at module scope, alloca inside fn
                LLVMTypeRef llvm_type = type_to_llvm(ctx, final_type);

                // Convert value if needed
                LLVMValueRef stored_value = value_result.value;

                if (type_is_integer(final_type) && type_is_float(inferred_type)) {
                    stored_value = LLVMBuildFPToSI(ctx->builder, value_result.value,
                                                   LLVMInt64TypeInContext(ctx->context), "toint");
                } else if (type_is_float(final_type) && type_is_integer(inferred_type)) {
                    stored_value = LLVMBuildSIToFP(ctx->builder, value_result.value,
                                                   LLVMDoubleTypeInContext(ctx->context), "tofloat");
                } else if (final_type->kind == TYPE_CHAR && inferred_type->kind != TYPE_CHAR) {
                    if (type_is_float(inferred_type))
                        stored_value = LLVMBuildFPToSI(ctx->builder, value_result.value,
                                                       LLVMInt8TypeInContext(ctx->context), "tochar");
                    else if (type_is_integer(inferred_type))
                        stored_value = LLVMBuildTrunc(ctx->builder, value_result.value,
                                                      LLVMInt8TypeInContext(ctx->context), "tochar");
                }

                // Make string variables mutable by strdup-ing the value
                if (final_type->kind == TYPE_STRING && stored_value) {
                    LLVMValueRef strdup_fn = get_or_declare_strdup(ctx);
                    LLVMValueRef args[] = {stored_value};
                    stored_value = LLVMBuildCall2(ctx->builder,
                                                  LLVMGlobalGetValueType(strdup_fn),
                                                  strdup_fn, args, 1, "strdup");
                }

                LLVMValueRef var;
                if (is_at_top_level(ctx)) {
                    // Module-level: emit as an LLVM global (will be mangled in Phase 9)
                    var = LLVMGetNamedGlobal(ctx->module, var_name);
                    if (!var) {
                        var = LLVMAddGlobal(ctx->module, llvm_type, var_name);
                        LLVMSetInitializer(var, LLVMConstNull(llvm_type));
                        LLVMSetLinkage(var, LLVMExternalLinkage);
                    }
                    LLVMBuildStore(ctx->builder, stored_value, var);
                } else {
                    // Inside function: stack alloca
                    var = LLVMBuildAlloca(ctx->builder, llvm_type, var_name);
                    LLVMBuildStore(ctx->builder, stored_value, var);
                }

                env_insert(ctx->env, var_name, final_type, var);
                EnvEntry *e1479 = env_lookup(ctx->env, var_name);
                if (e1479) e1479->llvm_name = strdup(LLVMGetValueName(var));

                // Check for alias (list item [3] and [4])
                if (ast->list.count >= 5) {
                    AST *doc_node   = ast->list.items[3];
                    AST *alias_node = ast->list.items[4];

                    // Update docstring if provided
                    if (doc_node->type == AST_STRING) {
                        EnvEntry *ent = env_lookup(ctx->env, var_name);
                        if (ent) {
                            free(ent->docstring);
                            ent->docstring = strdup(doc_node->string);
                        }
                    }

                    // Register alias
                    if (alias_node->type == AST_SYMBOL &&
                        strcmp(alias_node->symbol, "__no_alias__") != 0) {
                        // Insert alias pointing to the same LLVM value
                        env_insert(ctx->env, alias_node->symbol,
                                   type_clone(final_type), var);
                        printf("Alias: %s -> %s\n", alias_node->symbol, var_name);
                    }
                }


                if (ctx->module_ctx && !should_export_symbol(ctx->module_ctx, var_name)) {
                    printf("Defined %s :: %s (private)\n", var_name, type_to_string(final_type));
                } else {
                    printf("Defined %s :: %s\n", var_name, type_to_string(final_type));
                }


                result.type = final_type;
                result.value = stored_value;
                return result;
            }

            if (strcmp(head->symbol, "nil?") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'nil?' requires exactly 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                CodegenResult arg = codegen_expr(ctx, ast->list.items[1]);
                if (!arg.value) return result;

                LLVMTypeRef  i1  = LLVMInt1TypeInContext(ctx->context);
                LLVMTypeRef  ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

                /* Integers, bools, floats — never nil by definition */
                if (arg.type && (arg.type->kind == TYPE_INT  ||
                                 arg.type->kind == TYPE_FLOAT ||
                                 arg.type->kind == TYPE_BOOL  ||
                                 arg.type->kind == TYPE_CHAR)) {
                    result.value = LLVMConstInt(i1, 0, 0);  /* always False */
                    result.type  = type_bool();
                    return result;
                }

                /* Pointer-like value — do the actual null check */
                LLVMValueRef val = arg.value;
                if (LLVMTypeOf(val) != ptr)
                    val = LLVMBuildBitCast(ctx->builder, val, ptr, "nil_cast");

                result.value = LLVMBuildICmp(ctx->builder, LLVMIntEQ, val,
                                             LLVMConstPointerNull(ptr), "is_nil");
                result.type  = type_bool();
                return result;
            }


            // Handle 'quote' special form
            if (strcmp(head->symbol, "quote") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'quote' requires 1 argument",
                            parser_get_filename(), ast->line, ast->column);
                }

                AST *quoted = ast->list.items[1];

                // Handle different quoted types
                if (quoted->type == AST_LIST) {
                    // Create runtime list
                    LLVMValueRef list_fn = get_rt_list_new(ctx);
                    LLVMValueRef list = LLVMBuildCall2(ctx->builder,
                                                       LLVMGlobalGetValueType(list_fn), list_fn, NULL, 0, "list");

                    // Append each element
                    for (size_t i = 0; i < quoted->list.count; i++) {
                        LLVMValueRef elem_val = ast_to_runtime_value(ctx, quoted->list.items[i]);
                        if (elem_val) {
                            LLVMValueRef append_fn = get_rt_list_append(ctx);
                            LLVMValueRef args[] = {list, elem_val};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(append_fn),
                                           append_fn, args, 2, "");
                        }
                    }

                    result.type = type_list(NULL);
                    result.value = list;
                    return result;
                }
                else if (quoted->type == AST_SYMBOL) {
                    // Currently returns a string — fix to return a symbol:
                    result.type  = type_symbol();
                    LLVMValueRef fn  = get_rt_value_symbol(ctx);
                    LLVMValueRef sym = LLVMBuildGlobalStringPtr(ctx->builder,
                                                                quoted->symbol, "quoted_sym");
                    LLVMValueRef args[] = {sym};
                    result.value = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                                  fn, args, 1, "sym");
                    return result;
                }

                else if (quoted->type == AST_KEYWORD) {
                    // Keyword stays as keyword
                    char keyword_str[256];
                    snprintf(keyword_str, sizeof(keyword_str), ":%s", quoted->keyword);
                    result.type = type_keyword();
                    result.value = LLVMBuildGlobalStringPtr(ctx->builder,
                                                            keyword_str, "quoted_kw");
                    return result;
                }
                else {
                    // For other types (numbers, strings, chars), just evaluate them
                    return codegen_expr(ctx, quoted);
                }
            }

            if (strcmp(head->symbol, "expand") == 0) {
                if (ast->list.count < 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: expand requires one argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                AST *arg = ast->list.items[1];
                // Unwrap quote
                if (arg->type == AST_LIST && arg->list.count == 2 &&
                    arg->list.items[0]->type == AST_SYMBOL &&
                    strcmp(arg->list.items[0]->symbol, "quote") == 0) {
                    arg = arg->list.items[1];
                }

                // Call rt_ast_to_runtime_value RIGHT NOW at codegen time
                // before the AST gets freed
                RuntimeValue *rv = rt_ast_to_runtime_value(arg);

                // Embed the resulting RuntimeValue* as a constant pointer in the IR
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i64     = LLVMInt64TypeInContext(ctx->context);
                LLVMValueRef addr    = LLVMConstInt(i64, (uint64_t)(uintptr_t)rv, 0);
                result.value = LLVMBuildIntToPtr(ctx->builder, addr, ptr, "expanded");
                result.type  = type_unknown();
                return result;
            }

            // Handle 'show' function
            if (strcmp(head->symbol, "show") == 0) {
                if (ast->list.count < 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'show' requires at least 1 argument",
                            parser_get_filename(), ast->line, ast->column);
                }

                LLVMValueRef printf_fn = get_or_declare_printf(ctx);
                AST *arg = ast->list.items[1];

                // ----------------------------------------------------------------
                // Variadic: (show "format _ string" val1 val2 ...)
                // ----------------------------------------------------------------
                if (ast->list.count > 2 && arg->type == AST_STRING) {
                    const char *fmt     = arg->string;
                    size_t      arg_idx = 2;
                    const char *p       = fmt;

                    while (*p) {
                        const char *underscore = strchr(p, '_');

                        if (!underscore) {
                            // No more placeholders — print the rest
                            if (*p) {
                                LLVMValueRef s = LLVMBuildGlobalStringPtr(ctx->builder, p, "fmt_tail");
                                LLVMValueRef args[] = {s};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                               printf_fn, args, 1, "");
                            }
                            break;
                        }

                        // Print chunk before '_'
                        if (underscore > p) {
                            char *chunk = strndup(p, underscore - p);
                            LLVMValueRef s = LLVMBuildGlobalStringPtr(ctx->builder, chunk, "fmt_chunk");
                            free(chunk);
                            LLVMValueRef args[] = {s};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                           printf_fn, args, 1, "");
                        }

                        // Print argument for '_'
                        if (arg_idx < ast->list.count) {
                            CodegenResult val = codegen_expr(ctx, ast->list.items[arg_idx++]);
                            codegen_show_value(ctx, val.value, val.type, false);
                        } else {
                            // No argument left — print literal '_'
                            LLVMValueRef s = LLVMBuildGlobalStringPtr(ctx->builder, "_", "lit_underscore");
                            LLVMValueRef args[] = {s};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                           printf_fn, args, 1, "");
                        }

                        p = underscore + 1;
                    }

                    // Always end with newline
                    LLVMValueRef nl = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "nl");
                    LLVMValueRef nl_args[] = {nl};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, nl_args, 1, "");

                    result.type  = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // ----------------------------------------------------------------
                // Single argument
                // ----------------------------------------------------------------

                // Quoted expression: (show '(+ 2 3))
                if (arg->type == AST_LIST && arg->list.count > 0 &&
                    arg->list.items[0]->type == AST_SYMBOL &&
                    strcmp(arg->list.items[0]->symbol, "quote") == 0) {
                    if (arg->list.count == 2) {
                        codegen_print_ast(ctx, arg->list.items[1]);
                        LLVMValueRef nl = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "nl");
                        LLVMValueRef nl_args[] = {nl};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, nl_args, 1, "");
                    }
                    result.type  = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // String literal
                if (arg->type == AST_STRING) {
                    LLVMValueRef str  = LLVMBuildGlobalStringPtr(ctx->builder, arg->string, "str");
                    LLVMValueRef args[] = {get_fmt_str(ctx), str};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                    result.type  = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Char literal
                if (arg->type == AST_CHAR) {
                    LLVMValueRef ch = LLVMConstInt(LLVMInt8TypeInContext(ctx->context), arg->character, 0);
                    LLVMValueRef args[] = {get_fmt_char(ctx), ch};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                    result.type  = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Keyword literal
                if (arg->type == AST_KEYWORD) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), ":%s\n", arg->keyword);
                    LLVMValueRef s    = LLVMBuildGlobalStringPtr(ctx->builder, buf, "kw");
                    LLVMValueRef args[] = {s};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 1, "");
                    result.type  = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Symbol (variable)
                if (arg->type == AST_SYMBOL) {
                    // Handle dot-access: (show p.x)
                    if (strchr(arg->symbol, '.')) {
                        CodegenResult field_r = codegen_expr(ctx, arg);
                        if (field_r.type)
                            codegen_show_value(ctx, field_r.value, field_r.type, true);
                        result.type  = type_float();
                        result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                        return result;
                    }
                    EnvEntry *entry = env_lookup(ctx->env, arg->symbol);
                    if (!entry) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: unbound variable: %s",
                                parser_get_filename(), ast->line, ast->column, arg->symbol);
                    }

                    // Arrays need special treatment — they're pointers to stack allocations
                    if (entry->type && entry->type->kind == TYPE_ARR) {
                        LLVMTypeRef arr_type = type_to_llvm(ctx, entry->type);
                        LLVMValueRef open = LLVMBuildGlobalStringPtr(ctx->builder, "[", "open");
                        LLVMValueRef open_args[] = {open};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, open_args, 1, "");

                        for (int i = 0; i < entry->type->arr_size; i++) {
                            if (i > 0) {
                                LLVMValueRef sp = LLVMBuildGlobalStringPtr(ctx->builder, " ", "sp");
                                LLVMValueRef sp_args[] = {sp};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                               printf_fn, sp_args, 1, "");
                            }
                            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                            LLVMValueRef idx  = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), i, 0);
                            LLVMValueRef indices[] = {zero, idx};
                            LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder, arr_type,
                                                                  entry->value, indices, 2, "elem_ptr");
                            LLVMTypeRef  elem_llvm = type_to_llvm(ctx, entry->type->arr_element_type);
                            LLVMValueRef elem      = LLVMBuildLoad2(ctx->builder, elem_llvm, elem_ptr, "elem");
                            codegen_show_value(ctx, elem, entry->type->arr_element_type, false);
                        }

                        LLVMValueRef close = LLVMBuildGlobalStringPtr(ctx->builder, "]\n", "close");
                        LLVMValueRef close_args[] = {close};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, close_args, 1, "");

                        result.type  = type_float();
                        result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                        return result;
                    }

                    // All other types: load and print
                    LLVMValueRef loaded = LLVMBuildLoad2(ctx->builder,
                                                         type_to_llvm(ctx, entry->type),
                                                         entry->value, arg->symbol);
                    codegen_show_value(ctx, loaded, entry->type, true);

                    result.type  = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Expression (not a symbol)
                CodegenResult arg_result = codegen_expr(ctx, arg);

                // Arrays from expressions like [1 2 3]
                if (arg_result.type && arg_result.type->kind == TYPE_ARR) {
                    LLVMTypeRef arr_type = type_to_llvm(ctx, arg_result.type);
                    LLVMValueRef open = LLVMBuildGlobalStringPtr(ctx->builder, "[", "open");
                    LLVMValueRef open_args[] = {open};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, open_args, 1, "");

                    for (int i = 0; i < arg_result.type->arr_size; i++) {
                        if (i > 0) {
                            LLVMValueRef sp = LLVMBuildGlobalStringPtr(ctx->builder, " ", "sp");
                            LLVMValueRef sp_args[] = {sp};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                           printf_fn, sp_args, 1, "");
                        }
                        LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                        LLVMValueRef idx  = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), i, 0);
                        LLVMValueRef indices[] = {zero, idx};
                        LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder, arr_type,
                                                              arg_result.value, indices, 2, "elem_ptr");
                        LLVMTypeRef  elem_llvm = type_to_llvm(ctx, arg_result.type->arr_element_type);
                        LLVMValueRef elem      = LLVMBuildLoad2(ctx->builder, elem_llvm, elem_ptr, "elem");
                        codegen_show_value(ctx, elem, arg_result.type->arr_element_type, false);
                    }

                    LLVMValueRef close = LLVMBuildGlobalStringPtr(ctx->builder, "]\n", "close");
                    LLVMValueRef close_args[] = {close};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, close_args, 1, "");

                    result.type  = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Everything else
                if (arg_result.type)
                    codegen_show_value(ctx, arg_result.value, arg_result.type, true);

                result.type  = type_float();
                result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                return result;
            }

            if (strcmp(head->symbol, "doc") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'doc' requires 1 argument",
                            parser_get_filename(), ast->line, ast->column);
                }

                AST *arg = ast->list.items[1];
                if (arg->type != AST_SYMBOL) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'doc' argument must be a symbol",
                            parser_get_filename(), ast->line, ast->column);
                }

                EnvEntry *entry = env_lookup(ctx->env, arg->symbol);
                if (!entry) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'doc' unbound variable: %s",
                            parser_get_filename(), ast->line, ast->column, arg->symbol);
                }

                /* const char *docstring = entry->docstring ? entry->docstring : ""; */
                const char *docstring = (entry->docstring && strlen(entry->docstring) > 0)
                    ? entry->docstring
                    : "(no documentation)";


                result.type  = type_string();
                result.value = LLVMBuildGlobalStringPtr(ctx->builder, docstring, "docstr");
                return result;
            }

            if (strcmp(head->symbol, "code") == 0) {
                if (ast->list.count != 2 ||
                    ast->list.items[1]->type != AST_SYMBOL) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'code' requires a symbol name",
                                  parser_get_filename(), ast->line, ast->column);
                }
                const char *fn_name = ast->list.items[1]->symbol;
                EnvEntry *e = env_lookup(ctx->env, fn_name);
                if (!e) {
                    // Also check layout env
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: unbound symbol: %s",
                                  parser_get_filename(), ast->line, ast->column, fn_name);
                }
                if (!e->source_text || !e->source_text[0]) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: no source available for '%s'",
                                  parser_get_filename(), ast->line, ast->column, fn_name);
                }
                AST *src_ast = parse(e->source_text);
                if (!src_ast) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: failed to parse source for '%s'",
                                  parser_get_filename(), ast->line, ast->column, fn_name);
                }
                RuntimeValue *rv = rt_ast_to_runtime_value(src_ast);
                ast_free(src_ast);
                LLVMTypeRef  ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i64 = LLVMInt64TypeInContext(ctx->context);
                LLVMValueRef addr = LLVMConstInt(i64, (uint64_t)(uintptr_t)rv, 0);
                result.value = LLVMBuildIntToPtr(ctx->builder, addr, ptr, "code_rv");
                result.type  = type_unknown();
                return result;
            }


            if (strcmp(head->symbol, "assert-eq") == 0) {
                if (ast->list.count != 4) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'assert-eq' requires 3 arguments: actual expected label",
                                  parser_get_filename(), ast->line, ast->column);
                }

                CodegenResult actual   = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult expected = codegen_expr(ctx, ast->list.items[2]);
                AST          *label_ast = ast->list.items[3];

                LLVMValueRef printf_fn = get_or_declare_printf(ctx);
                LLVMTypeRef  i64       = LLVMInt64TypeInContext(ctx->context);
                LLVMTypeRef  ptr       = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

                // Resolve label
                LLVMValueRef label_val;
                if (label_ast->type == AST_STRING) {
                    label_val = LLVMBuildGlobalStringPtr(ctx->builder,
                                                         label_ast->string, "assert_label");
                } else {
                    CodegenResult label_r = codegen_expr(ctx, label_ast);
                    label_val = label_r.value;
                }

                // ── Normalize both sides to a comparable type ─────────────────────
                //
                // Priority:
                //   1. String  -> strcmp
                //   2. Float   -> fcmp
                //   3. ptr (RuntimeValue*) -> rt_unbox_int both sides, then icmp i64
                //   4. i1/i64/integer -> zext to i64, then icmp i64

                LLVMValueRef lhs = actual.value;
                LLVMValueRef rhs = expected.value;

                // Helper lambda (inline): unbox a RuntimeValue* to i64
                // If a value is ptr type, call rt_unbox_int on it
#define UNBOX_TO_I64(val)                                               \
                do {                                                    \
                    if (LLVMTypeOf(val) == ptr) {                       \
                        LLVMTypeRef _uft = LLVMFunctionType(i64, &ptr, 1, 0); \
                        LLVMValueRef _arg = (val);                      \
                        (val) = LLVMBuildCall2(ctx->builder, _uft,      \
                                               get_rt_unbox_int(ctx), &_arg, 1, \
                                               "unboxed");              \
                    }                                                   \
                } while(0)

                LLVMValueRef cond = NULL;

                if (actual.type && actual.type->kind == TYPE_STRING) {
                    // String comparison via strcmp
                    LLVMValueRef strcmp_fn = LLVMGetNamedFunction(ctx->module, "strcmp");
                    if (!strcmp_fn) {
                        LLVMTypeRef params[] = {ptr, ptr};
                        LLVMTypeRef ft = LLVMFunctionType(LLVMInt32TypeInContext(ctx->context),
                                                          params, 2, 0);
                        strcmp_fn = LLVMAddFunction(ctx->module, "strcmp", ft);
                    }
                    LLVMValueRef args[] = {lhs, rhs};
                    LLVMValueRef cmp = LLVMBuildCall2(ctx->builder,
                                                      LLVMGlobalGetValueType(strcmp_fn),
                                                      strcmp_fn, args, 2, "strcmp");
                    cond = LLVMBuildICmp(ctx->builder, LLVMIntEQ, cmp,
                                         LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0),
                                         "eq");

                } else if (actual.type && type_is_float(actual.type)) {
                    // Float comparison
                    if (LLVMTypeOf(rhs) == ptr) {
                        LLVMTypeRef dbl  = LLVMDoubleTypeInContext(ctx->context);
                        LLVMTypeRef _uft = LLVMFunctionType(dbl, &ptr, 1, 0);
                        rhs = LLVMBuildCall2(ctx->builder, _uft,
                                             get_rt_unbox_float(ctx), &rhs, 1, "unbox_rhs_f");
                    }
                    cond = LLVMBuildFCmp(ctx->builder, LLVMRealOEQ, lhs, rhs, "eq");

                } else {
                    // Integer / ptr / bool / unknown — unbox ptrs, zext to i64, icmp
                    UNBOX_TO_I64(lhs);
                    UNBOX_TO_I64(rhs);

                    // Now both should be integer types — zext anything narrower to i64
                    if (LLVMTypeOf(lhs) != i64)
                        lhs = LLVMBuildZExt(ctx->builder, lhs, i64, "zext_lhs");
                    if (LLVMTypeOf(rhs) != i64)
                        rhs = LLVMBuildZExt(ctx->builder, rhs, i64, "zext_rhs");

                    cond = LLVMBuildICmp(ctx->builder, LLVMIntEQ, lhs, rhs, "eq");
                }

#undef UNBOX_TO_I64

                // ── Branch on condition ───────────────────────────────────────────
                LLVMValueRef      func    = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                LLVMBasicBlockRef pass_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "assert_pass");
                LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "assert_fail");
                LLVMBasicBlockRef cont_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "assert_cont");
                LLVMBuildCondBr(ctx->builder, cond, pass_bb, fail_bb);

                // ── Pass branch ───────────────────────────────────────────────────
                LLVMPositionBuilderAtEnd(ctx->builder, pass_bb);
                if (ctx->test_mode) {
                    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder,
                                                                "  \x1b[32m✓\x1b[0m %s\n", "pass_fmt");
                    LLVMValueRef a[] = {fmt, label_val};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, a, 2, "");
                }
                LLVMBuildBr(ctx->builder, cont_bb);
                // ── Fail branch ───────────────────────────────────────────────────
                LLVMPositionBuilderAtEnd(ctx->builder, fail_bb);
                {
                    LLVMValueRef assert_fail_fn = LLVMGetNamedFunction(ctx->module,
                                                                       "__monad_assert_fail");
                    if (!assert_fail_fn) {
                        LLVMTypeRef ft = LLVMFunctionType(LLVMVoidTypeInContext(ctx->context),
                                                          &ptr, 1, 0);
                        assert_fail_fn = LLVMAddFunction(ctx->module, "__monad_assert_fail", ft);
                        LLVMSetLinkage(assert_fail_fn, LLVMExternalLinkage);
                    }
                    LLVMValueRef fail_args[] = {label_val};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(assert_fail_fn),
                                   assert_fail_fn, fail_args, 1, "");
                    LLVMBuildUnreachable(ctx->builder);
                }

                // ── Continue ──────────────────────────────────────────────────────
                LLVMPositionBuilderAtEnd(ctx->builder, cont_bb);
                result.type  = type_bool();
                result.value = cond;
                return result;
            }

            if (strcmp(head->symbol, "undefined") == 0) {
                // Guard: only valid inside a function body
                LLVMValueRef cur_fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                if (cur_fn == ctx->init_fn) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'undefined' is only valid inside a function body",
                            parser_get_filename(), ast->line, ast->column);
                }

                // Emit: fprintf(stderr, "<file>:<line>:<col>: error: called undefined\n")
                LLVMValueRef stderr_fn = LLVMGetNamedFunction(ctx->module, "fprintf");
                if (!stderr_fn) {
                    LLVMTypeRef fprintf_params[] = {
                        LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0),
                        LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0)
                    };
                    LLVMTypeRef fprintf_type = LLVMFunctionType(
                        LLVMInt32TypeInContext(ctx->context), fprintf_params, 2, true);
                    stderr_fn = LLVMAddFunction(ctx->module, "fprintf", fprintf_type);
                    LLVMSetLinkage(stderr_fn, LLVMExternalLinkage);
                }

                // Declare stderr via __acrt_iob_func or just use a format string to stdout
                // Simplest portable approach: use printf to stderr via fd 2 — but easiest
                // is just printf since we already use it everywhere for errors at runtime
                LLVMValueRef printf_fn = get_or_declare_printf(ctx);
                const char *fn_name = LLVMGetValueName(cur_fn);
                char msg[512];
                snprintf(msg, sizeof(msg),
                         "%s:%d:%d: error: called undefined function '%s'\n",
                         parser_get_filename(), ast->line, ast->column, fn_name);
                LLVMValueRef msg_str = LLVMBuildGlobalStringPtr(ctx->builder, msg, "undef_msg");
                LLVMValueRef msg_args[] = {msg_str};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                               printf_fn, msg_args, 1, "");

                // Call abort()
                LLVMValueRef abort_fn = LLVMGetNamedFunction(ctx->module, "abort");
                if (!abort_fn) {
                    LLVMTypeRef abort_ft = LLVMFunctionType(
                        LLVMVoidTypeInContext(ctx->context), NULL, 0, 0);
                    abort_fn = LLVMAddFunction(ctx->module, "abort", abort_ft);
                    LLVMSetLinkage(abort_fn, LLVMExternalLinkage);
                }
                LLVMTypeRef abort_ft = LLVMFunctionType(
                    LLVMVoidTypeInContext(ctx->context), NULL, 0, 0);
                LLVMBuildCall2(ctx->builder, abort_ft, abort_fn, NULL, 0, "");
                LLVMBuildUnreachable(ctx->builder);

                // Move builder into a dead block so subsequent IR (e.g. ret)
                // doesn't get emitted after the terminator
                LLVMBasicBlockRef dead_bb = LLVMAppendBasicBlockInContext(
                    ctx->context, cur_fn, "undef_dead");
                LLVMPositionBuilderAtEnd(ctx->builder, dead_bb);

                // Return undef of the correct return type — never actually reached
                LLVMTypeRef fn_type  = LLVMGlobalGetValueType(cur_fn);
                LLVMTypeRef ret_type = LLVMGetReturnType(fn_type);
                LLVMValueRef undef_val = (LLVMGetTypeKind(ret_type) == LLVMVoidTypeKind)
                    ? LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0)
                    : LLVMGetUndef(ret_type);

                result.value = undef_val;
                result.type  = type_int(); // never reached, type doesn't matter
                return result;
            }

            if (strcmp(head->symbol, "string-length") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'string-length' requires 1 argument",
                            parser_get_filename(), ast->line, ast->column);
                }

                // Declare strlen
                LLVMValueRef strlen_fn = LLVMGetNamedFunction(ctx->module, "strlen");
                if (!strlen_fn) {
                    LLVMTypeRef p  = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef ft = LLVMFunctionType(LLVMInt64TypeInContext(ctx->context), &p, 1, 0);
                    strlen_fn = LLVMAddFunction(ctx->module, "strlen", ft);
                    LLVMSetLinkage(strlen_fn, LLVMExternalLinkage);
                }

                CodegenResult arg = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef args[] = {arg.value};
                result.value = LLVMBuildCall2(ctx->builder,
                                              LLVMGlobalGetValueType(strlen_fn),
                                              strlen_fn, args, 1, "strlen");
                result.type  = type_int();
                return result;
            }

            if (strcmp(head->symbol, "string-ref") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'string-ref' requires 2 arguments",
                            parser_get_filename(), ast->line, ast->column);
                }

                CodegenResult str_r = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult idx_r = codegen_expr(ctx, ast->list.items[2]);

                LLVMTypeRef  i8  = LLVMInt8TypeInContext(ctx->context);
                LLVMTypeRef  i64 = LLVMInt64TypeInContext(ctx->context);
                LLVMValueRef idx = type_is_float(idx_r.type)
                    ? LLVMBuildFPToSI(ctx->builder, idx_r.value, i64, "idx")
                    : idx_r.value;

                LLVMValueRef ptr = LLVMBuildGEP2(ctx->builder, i8, str_r.value, &idx, 1, "char_ptr");
                result.value = LLVMBuildLoad2(ctx->builder, i8, ptr, "char");
                result.type  = type_char();
                return result;
            }

            if (strcmp(head->symbol, "string-set!") == 0) {
                if (ast->list.count != 4) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'string-set!' requires 3 arguments: string index char",
                            parser_get_filename(), ast->line, ast->column);
                }

                CodegenResult str_r  = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult idx_r  = codegen_expr(ctx, ast->list.items[2]);
                CodegenResult char_r = codegen_expr(ctx, ast->list.items[3]);

                LLVMTypeRef  i8  = LLVMInt8TypeInContext(ctx->context);
                LLVMTypeRef  i64 = LLVMInt64TypeInContext(ctx->context);

                LLVMValueRef idx = type_is_float(idx_r.type)
                    ? LLVMBuildFPToSI(ctx->builder, idx_r.value, i64, "idx")
                    : idx_r.value;

                // Coerce char value to i8
                LLVMValueRef ch = char_r.value;
                if (char_r.type->kind != TYPE_CHAR)
                    ch = LLVMBuildTrunc(ctx->builder, ch, i8, "to_char");

                LLVMValueRef ptr = LLVMBuildGEP2(ctx->builder, i8, str_r.value, &idx, 1, "char_ptr");
                LLVMBuildStore(ctx->builder, ch, ptr);

                // Returns the string (mutated in place)
                result.value = str_r.value;
                result.type  = type_string();
                return result;
            }

            if (strcmp(head->symbol, "make-string") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'make-string' requires 2 arguments: length fill-char",
                            parser_get_filename(), ast->line, ast->column);
                }

                // Declare calloc
                LLVMValueRef calloc_fn = LLVMGetNamedFunction(ctx->module, "calloc");
                if (!calloc_fn) {
                    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                    LLVMTypeRef p   = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef params[] = {i64, i64};
                    LLVMTypeRef ft = LLVMFunctionType(p, params, 2, 0);
                    calloc_fn = LLVMAddFunction(ctx->module, "calloc", ft);
                    LLVMSetLinkage(calloc_fn, LLVMExternalLinkage);
                }

                // Declare memset
                LLVMValueRef memset_fn = LLVMGetNamedFunction(ctx->module, "memset");
                if (!memset_fn) {
                    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
                    LLVMTypeRef p   = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef params[] = {p, i32, i64};
                    LLVMTypeRef ft = LLVMFunctionType(p, params, 3, 0);
                    memset_fn = LLVMAddFunction(ctx->module, "memset", ft);
                    LLVMSetLinkage(memset_fn, LLVMExternalLinkage);
                }

                CodegenResult len_r  = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult fill_r = codegen_expr(ctx, ast->list.items[2]);

                LLVMTypeRef  i64 = LLVMInt64TypeInContext(ctx->context);
                LLVMTypeRef  i32 = LLVMInt32TypeInContext(ctx->context);

                LLVMValueRef len = type_is_float(len_r.type)
                    ? LLVMBuildFPToSI(ctx->builder, len_r.value, i64, "len")
                    : len_r.value;

                // Allocate len+1 bytes (null terminator)
                LLVMValueRef one = LLVMConstInt(i64, 1, 0);
                LLVMValueRef len1 = LLVMBuildAdd(ctx->builder, len, one, "len1");
                LLVMValueRef one2 = LLVMConstInt(i64, 1, 0);
                LLVMValueRef calloc_args[] = {len1, one2};
                LLVMValueRef buf = LLVMBuildCall2(ctx->builder,
                                                  LLVMGlobalGetValueType(calloc_fn),
                                                  calloc_fn, calloc_args, 2, "buf");

                // Fill with the char using memset
                LLVMValueRef fill_i32 = LLVMBuildZExt(ctx->builder, fill_r.value, i32, "fill_i32");
                LLVMValueRef memset_args[] = {buf, fill_i32, len};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(memset_fn),
                               memset_fn, memset_args, 3, "");

                result.value = buf;
                result.type  = type_string();
                return result;
            }


            // (list) -> List  — new list (empty list)
            if (strcmp(head->symbol, "list") == 0) {
                LLVMTypeRef  ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef fn  = get_rt_list_new(ctx);  // was get_rt_list_empty
                LLVMTypeRef  ft  = LLVMFunctionType(ptr, NULL, 0, 0);
                result.value = LLVMBuildCall2(ctx->builder, ft, fn, NULL, 0, "list");
                result.type  = type_list(NULL);
                return result;
            }

            // (cons val xs) -> List
            if (strcmp(head->symbol, "cons") == 0) {
                LLVMTypeRef  ptr      = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult val_r   = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult list_r  = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef  boxed   = codegen_box(ctx, val_r.value, val_r.type);
                LLVMValueRef  fn      = get_rt_list_cons(ctx);
                LLVMTypeRef   ft_args[] = {ptr, ptr};
                LLVMTypeRef   ft      = LLVMFunctionType(ptr, ft_args, 2, 0);
                LLVMValueRef  args[]  = {boxed, list_r.value};
                result.value = LLVMBuildCall2(ctx->builder, ft, fn, args, 2, "cons");
                result.type  = type_list(NULL);
                return result;
            }

            // (car xs) -> RuntimeValue* (opaque, use show-value or unbox)
            if (strcmp(head->symbol, "car") == 0) {
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult list_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef  fn     = get_rt_list_car(ctx);
                LLVMTypeRef   ft_args[] = {ptr};
                LLVMTypeRef   ft     = LLVMFunctionType(ptr, ft_args, 1, 0);
                LLVMValueRef  args[] = {list_r.value};
                result.value = LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "car");
                result.type  = type_unknown();
                return result;
            }

            // (cdr xs) -> List
            if (strcmp(head->symbol, "cdr") == 0) {
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult list_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef  fn     = get_rt_list_cdr(ctx);
                LLVMTypeRef   ft_args[] = {ptr};
                LLVMTypeRef   ft     = LLVMFunctionType(ptr, ft_args, 1, 0);
                LLVMValueRef  args[] = {list_r.value};
                result.value = LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "cdr");
                result.type  = type_list(NULL);
                return result;
            }

            // (list-ref xs i) -> RuntimeValue* (opaque)
            if (strcmp(head->symbol, "list-ref") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'list-ref' requires 2 arguments (xs index)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i64     = LLVMInt64TypeInContext(ctx->context);
                CodegenResult list_r = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult idx_r  = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef  fn     = get_rt_list_nth(ctx);
                LLVMTypeRef   ft_args[] = {ptr, i64};
                LLVMTypeRef   ft     = LLVMFunctionType(ptr, ft_args, 2, 0);
                LLVMValueRef  args[] = {list_r.value, idx_r.value};
                result.value = LLVMBuildCall2(ctx->builder, ft, fn, args, 2, "list_ref");
                result.type  = type_unknown();
                return result;
            }

            // (take n xs) -> List
            if (strcmp(head->symbol, "take") == 0) {
                LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                CodegenResult n_r    = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult list_r = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef n_val = type_is_float(n_r.type)
                    ? LLVMBuildFPToSI(ctx->builder, n_r.value, i64, "n") : n_r.value;

                if (list_r.type && list_r.type->kind == TYPE_STRING) {
                    LLVMValueRef fn = get_rt_string_take(ctx);
                    LLVMTypeRef ft_args[] = {ptr, i64};
                    LLVMTypeRef ft = LLVMFunctionType(ptr, ft_args, 2, 0);
                    LLVMValueRef args[] = {list_r.value, n_val};
                    result.value = LLVMBuildCall2(ctx->builder, ft, fn, args, 2, "take_str");
                    result.type  = type_string();
                    return result;
                }

                LLVMValueRef fn = get_rt_list_take(ctx);
                LLVMTypeRef ft_args[] = {ptr, i64};
                LLVMTypeRef ft = LLVMFunctionType(ptr, ft_args, 2, 0);
                LLVMValueRef args[] = {list_r.value, n_val};
                result.value = LLVMBuildCall2(ctx->builder, ft, fn, args, 2, "take");
                result.type  = type_list(NULL);
                return result;
            }

            // (drop n xs) -> List
            if (strcmp(head->symbol, "drop") == 0) {
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i64     = LLVMInt64TypeInContext(ctx->context);
                CodegenResult n_r    = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult list_r = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef  n_val  = type_is_float(n_r.type)
                    ? LLVMBuildFPToSI(ctx->builder, n_r.value, i64, "n") : n_r.value;
                LLVMValueRef  fn     = get_rt_list_drop(ctx);
                LLVMTypeRef   ft_args[] = {ptr, i64};
                LLVMTypeRef   ft     = LLVMFunctionType(ptr, ft_args, 2, 0);
                LLVMValueRef  args[] = {list_r.value, n_val};
                result.value = LLVMBuildCall2(ctx->builder, ft, fn, args, 2, "drop");
                result.type  = type_list(NULL);
                return result;
            }



            // (map fn xs) -> List
            if (strcmp(head->symbol, "map") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'map' requires 2 arguments (fn xs)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef   ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef  raw_fn = codegen_fn_arg(ctx, ast->list.items[1]);
                CodegenResult list_r = codegen_expr(ctx, ast->list.items[2]);

                Type *arg0_t, *arg1_t, *ret_t;
                lookup_fn_types(ctx, ast->list.items[1], &arg0_t, &arg1_t, &ret_t);
                LLVMValueRef wrapper = codegen_unary_wrapper(ctx, raw_fn, arg0_t, ret_t);

                LLVMValueRef rt_fn    = get_rt_list_map(ctx);
                LLVMTypeRef  ft_args[] = {ptr, ptr};
                LLVMTypeRef  ft       = LLVMFunctionType(ptr, ft_args, 2, 0);
                LLVMValueRef args[]   = {list_r.value, wrapper};
                result.value = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 2, "map");
                result.type  = type_list(NULL);
                return result;
            }

            // (foldl fn init xs) -> value
            if (strcmp(head->symbol, "foldl") == 0) {
                if (ast->list.count != 4) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'foldl' requires 3 arguments (fn init xs)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef   ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef  raw_fn = codegen_fn_arg(ctx, ast->list.items[1]);
                CodegenResult init_r = codegen_expr(ctx, ast->list.items[2]);
                CodegenResult list_r = codegen_expr(ctx, ast->list.items[3]);

                // box init to RuntimeValue*
                LLVMValueRef init_val = init_r.value;
                if (init_r.type && type_is_integer(init_r.type)) {
                    LLVMTypeRef bft = LLVMFunctionType(ptr, &(LLVMTypeRef){LLVMInt64TypeInContext(ctx->context)}, 1, 0);
                    init_val = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_int(ctx), &init_r.value, 1, "box_init");
                } else if (init_r.type && type_is_float(init_r.type)) {
                    LLVMTypeRef bft = LLVMFunctionType(ptr, &(LLVMTypeRef){LLVMDoubleTypeInContext(ctx->context)}, 1, 0);
                    init_val = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_float(ctx), &init_r.value, 1, "box_init");
                }

                // foldl fn is binary: (acc, elem) -> acc
                // arg0 = accumulator type (same as init), arg1 = element type
                Type *arg0_t, *arg1_t, *ret_t;
                lookup_fn_types(ctx, ast->list.items[1], &arg0_t, &arg1_t, &ret_t);
                LLVMValueRef wrapper = codegen_binary_wrapper(ctx, raw_fn, arg0_t, arg1_t, ret_t);

                LLVMValueRef rt_fn    = get_rt_list_foldl(ctx);
                LLVMTypeRef  ft_args[] = {ptr, ptr, ptr};
                LLVMTypeRef  ft       = LLVMFunctionType(ptr, ft_args, 3, 0);
                LLVMValueRef args[]   = {list_r.value, init_val, wrapper};
                result.value = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 3, "foldl");
                result.type  = type_unknown();
                return result;
            }

            // (foldr fn init xs) -> value
            if (strcmp(head->symbol, "foldr") == 0) {
                if (ast->list.count != 4) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'foldr' requires 3 arguments (fn init xs)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef   ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef  raw_fn = codegen_fn_arg(ctx, ast->list.items[1]);
                CodegenResult init_r = codegen_expr(ctx, ast->list.items[2]);
                CodegenResult list_r = codegen_expr(ctx, ast->list.items[3]);

                LLVMValueRef init_val = init_r.value;
                if (init_r.type && type_is_integer(init_r.type)) {
                    LLVMTypeRef bft = LLVMFunctionType(ptr, &(LLVMTypeRef){LLVMInt64TypeInContext(ctx->context)}, 1, 0);
                    init_val = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_int(ctx), &init_r.value, 1, "box_init");
                } else if (init_r.type && type_is_float(init_r.type)) {
                    LLVMTypeRef bft = LLVMFunctionType(ptr, &(LLVMTypeRef){LLVMDoubleTypeInContext(ctx->context)}, 1, 0);
                    init_val = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_float(ctx), &init_r.value, 1, "box_init");
                }

                Type *arg0_t, *arg1_t, *ret_t;
                lookup_fn_types(ctx, ast->list.items[1], &arg0_t, &arg1_t, &ret_t);
                LLVMValueRef wrapper = codegen_binary_wrapper(ctx, raw_fn, arg0_t, arg1_t, ret_t);

                LLVMValueRef rt_fn    = get_rt_list_foldr(ctx);
                LLVMTypeRef  ft_args[] = {ptr, ptr, ptr};
                LLVMTypeRef  ft       = LLVMFunctionType(ptr, ft_args, 3, 0);
                LLVMValueRef args[]   = {list_r.value, init_val, wrapper};
                result.value = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 3, "foldr");
                result.type  = type_unknown();
                return result;
            }

            // (filter pred xs) -> List
            if (strcmp(head->symbol, "filter") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'filter' requires 2 arguments (pred xs)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef   ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef  raw_fn = codegen_fn_arg(ctx, ast->list.items[1]);
                CodegenResult list_r = codegen_expr(ctx, ast->list.items[2]);

                // filter predicate: elem -> bool/int (nonzero = keep)
                // We wrap it as a unary wrapper; the runtime RT_PredFn calls it and
                // checks rt_unbox_int(result) != 0
                Type *arg0_t, *arg1_t, *ret_t;
                lookup_fn_types(ctx, ast->list.items[1], &arg0_t, &arg1_t, &ret_t);
                LLVMValueRef wrapper = codegen_unary_wrapper(ctx, raw_fn, arg0_t, ret_t);

                // rt_list_filter expects RT_PredFn: int(*)(RuntimeValue*)
                // but we pass a unary wrapper (ptr->ptr) and cast in runtime via unbox_int
                // So update rt_list_filter to accept RT_UnaryFn and unbox result:
                LLVMValueRef rt_fn    = get_rt_list_filter(ctx);
                LLVMTypeRef  ft_args[] = {ptr, ptr};
                LLVMTypeRef  ft       = LLVMFunctionType(ptr, ft_args, 2, 0);
                LLVMValueRef args[]   = {list_r.value, wrapper};
                result.value = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 2, "filter");
                result.type  = type_list(NULL);
                return result;
            }

            // (zip xs ys) -> List   — no function arg, no wrapper needed
            if (strcmp(head->symbol, "zip") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'zip' requires 2 arguments (xs ys)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef   ptr  = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult a_r  = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult b_r  = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef  rt_fn = get_rt_list_zip(ctx);
                LLVMTypeRef   ft_args[] = {ptr, ptr};
                LLVMTypeRef   ft   = LLVMFunctionType(ptr, ft_args, 2, 0);
                LLVMValueRef  args[] = {a_r.value, b_r.value};
                result.value = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 2, "zip");
                result.type  = type_list(NULL);
                return result;
            }

            // (zipwith fn xs ys) -> List
            if (strcmp(head->symbol, "zipwith") == 0) {
                if (ast->list.count != 4) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'zipwith' requires 3 arguments (fn xs ys)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef   ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef  raw_fn = codegen_fn_arg(ctx, ast->list.items[1]);
                CodegenResult a_r    = codegen_expr(ctx, ast->list.items[2]);
                CodegenResult b_r    = codegen_expr(ctx, ast->list.items[3]);

                Type *arg0_t, *arg1_t, *ret_t;
                lookup_fn_types(ctx, ast->list.items[1], &arg0_t, &arg1_t, &ret_t);
                LLVMValueRef wrapper = codegen_binary_wrapper(ctx, raw_fn, arg0_t, arg1_t, ret_t);

                LLVMValueRef rt_fn    = get_rt_list_zipwith(ctx);
                LLVMTypeRef  ft_args[] = {ptr, ptr, ptr};
                LLVMTypeRef  ft       = LLVMFunctionType(ptr, ft_args, 3, 0);
                LLVMValueRef args[]   = {a_r.value, b_r.value, wrapper};
                result.value = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 3, "zipwith");
                result.type  = type_list(NULL);
                return result;
            }




            // (show-value v) — print any RuntimeValue* with newline
            if (strcmp(head->symbol, "show-value") == 0) {
                LLVMTypeRef  ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult val_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef  fn    = get_rt_print_value_newline(ctx);
                LLVMTypeRef   ft_args[] = {ptr};
                LLVMTypeRef   ft    = LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), ft_args, 1, 0);
                LLVMValueRef  args[] = {val_r.value};
                LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "");
                result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                result.type  = type_int();
                return result;
            }

            // (list-length xs) -> Int
            if (strcmp(head->symbol, "list-length") == 0) {
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i64     = LLVMInt64TypeInContext(ctx->context);
                CodegenResult list_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef  fn     = get_rt_list_length(ctx);
                LLVMTypeRef   ft_args[] = {ptr};
                LLVMTypeRef   ft     = LLVMFunctionType(i64, ft_args, 1, 0);
                LLVMValueRef  args[] = {list_r.value};
                result.value = LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "list_length");
                result.type  = type_int();
                return result;
            }

            // (list-empty? xs) -> Bool
            if (strcmp(head->symbol, "list-empty?") == 0) {
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i32     = LLVMInt32TypeInContext(ctx->context);
                LLVMTypeRef  i1      = LLVMInt1TypeInContext(ctx->context);
                CodegenResult list_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef  fn     = get_rt_list_is_empty(ctx);
                LLVMTypeRef   ft_args[] = {ptr};
                LLVMTypeRef   ft     = LLVMFunctionType(i32, ft_args, 1, 0);
                LLVMValueRef  args[] = {list_r.value};
                LLVMValueRef  i32val = LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "is_empty");
                result.value = LLVMBuildTrunc(ctx->builder, i32val, i1, "list_empty");
                result.type  = type_bool();
                return result;
            }

            // (list-copy xs) -> List
            if (strcmp(head->symbol, "list-copy") == 0) {
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult list_r = codegen_expr(ctx, ast->list.items[1]);
                // list-copy = create new list + cons all elements
                // delegate to runtime helper
                LLVMValueRef  fn     = get_rt_list_copy(ctx);
                LLVMTypeRef   ft_args[] = {ptr};
                LLVMTypeRef   ft     = LLVMFunctionType(ptr, ft_args, 1, 0);
                LLVMValueRef  args[] = {list_r.value};
                result.value = LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "list_copy");
                result.type  = type_list(NULL);
                return result;
            }

            // (append xs ys) -> List  — pure, returns new list
            if (strcmp(head->symbol, "append") == 0) {
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult a_r    = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult b_r    = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef  fn     = get_rt_list_append_lists(ctx);
                LLVMTypeRef   ft_args[] = {ptr, ptr};
                LLVMTypeRef   ft     = LLVMFunctionType(ptr, ft_args, 2, 0);
                LLVMValueRef  args[] = {a_r.value, b_r.value};
                result.value = LLVMBuildCall2(ctx->builder, ft, fn, args, 2, "append");
                result.type  = type_list(NULL);
                return result;
            }

            // (append! xs val) -> void  — destructive, mutates xs
            if (strcmp(head->symbol, "append!") == 0) {
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult list_r = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult val_r  = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef  boxed  = codegen_box(ctx, val_r.value, val_r.type);
                LLVMValueRef  fn     = get_rt_list_append(ctx);
                LLVMTypeRef   ft_args[] = {ptr, ptr};
                LLVMTypeRef   ft     = LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), ft_args, 2, 0);
                LLVMValueRef  args[] = {list_r.value, boxed};
                LLVMBuildCall2(ctx->builder, ft, fn, args, 2, "");
                result.value = list_r.value;
                result.type  = type_list(NULL);
                return result;
            }

            // (make-list n) or (make-list n val) -> List
            if (strcmp(head->symbol, "make-list") == 0) {
                LLVMTypeRef  ptr   = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i64   = LLVMInt64TypeInContext(ctx->context);
                CodegenResult n_r  = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef  fill;
                if (ast->list.count >= 3) {
                    CodegenResult fill_r = codegen_expr(ctx, ast->list.items[2]);
                    fill = codegen_box(ctx, fill_r.value, fill_r.type);
                } else {
                    // default fill: nil
                    LLVMValueRef nil_fn = get_rt_value_nil(ctx);
                    LLVMTypeRef  nil_ft = LLVMFunctionType(ptr, NULL, 0, 0);
                    fill = LLVMBuildCall2(ctx->builder, nil_ft, nil_fn, NULL, 0, "nil");
                }
                LLVMValueRef  fn      = get_rt_make_list(ctx);
                LLVMTypeRef   ft_args[] = {i64, ptr};
                LLVMTypeRef   ft      = LLVMFunctionType(ptr, ft_args, 2, 0);
                LLVMValueRef  args[]  = {n_r.value, fill};
                result.value = LLVMBuildCall2(ctx->builder, ft, fn, args, 2, "make_list");
                result.type  = type_list(NULL);
                return result;
            }

            // (equal? a b) -> Bool
            if (strcmp(head->symbol, "equal?") == 0) {
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i32     = LLVMInt32TypeInContext(ctx->context);
                LLVMTypeRef  i1      = LLVMInt1TypeInContext(ctx->context);
                CodegenResult a_r    = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult b_r    = codegen_expr(ctx, ast->list.items[2]);
                // box both sides if they are not already opaque
                LLVMValueRef  aval   = (a_r.type->kind == TYPE_UNKNOWN)
                    ? a_r.value
                    : codegen_box(ctx, a_r.value, a_r.type);
                LLVMValueRef  bval   = (b_r.type->kind == TYPE_UNKNOWN)
                    ? b_r.value
                    : codegen_box(ctx, b_r.value, b_r.type);
                LLVMValueRef  fn     = get_rt_equal_p(ctx);
                LLVMTypeRef   ft_args[] = {ptr, ptr};
                LLVMTypeRef   ft     = LLVMFunctionType(i32, ft_args, 2, 0);
                LLVMValueRef  args[] = {aval, bval};
                LLVMValueRef  i32val = LLVMBuildCall2(ctx->builder, ft, fn, args, 2, "equal");
                result.value = LLVMBuildTrunc(ctx->builder, i32val, i1, "equal_bool");
                result.type  = type_bool();
                return result;
            }

            if (strcmp(head->symbol, "set!") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'set!' requires 2 arguments",
                                  parser_get_filename(), ast->line, ast->column);
                }

                AST *name_ast = ast->list.items[1];
                if (name_ast->type != AST_SYMBOL) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'set!' target must be a symbol",
                                  parser_get_filename(), ast->line, ast->column);
                }

                // ── Dot-access: (set! p.x 5.0) ──────────────────────────────────────
                const char *dot = strchr(name_ast->symbol, '.');
                if (dot && dot != name_ast->symbol) {
                    char var_name[256];
                    size_t vlen = dot - name_ast->symbol;
                    memcpy(var_name, name_ast->symbol, vlen);
                    var_name[vlen] = '\0';
                    const char *field_name = dot + 1;

                    EnvEntry *base_entry = env_lookup(ctx->env, var_name);
                    if (!base_entry) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: unbound variable: %s",
                                      parser_get_filename(), ast->line, ast->column, var_name);
                    }
                    if (base_entry->type && base_entry->type->kind != TYPE_LAYOUT) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' is not a layout type",
                                      parser_get_filename(), ast->line, ast->column, var_name);
                    }

                    Type *lay = base_entry->type;
                    int field_idx = -1;
                    for (int i = 0; i < lay->layout_field_count; i++) {
                        if (strcmp(lay->layout_fields[i].name, field_name) == 0) {
                            field_idx = i;
                            break;
                        }
                    }
                    if (field_idx < 0) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: layout '%s' has no field '%s'",
                                      parser_get_filename(), ast->line, ast->column,
                                      lay->layout_name, field_name);
                    }

                    CodegenResult val = codegen_expr(ctx, ast->list.items[2]);
                    Type *ft = lay->layout_fields[field_idx].type;

                    char sname_set[256];
                    snprintf(sname_set, sizeof(sname_set), "layout.%s", lay->layout_name);
                    LLVMTypeRef struct_llvm = LLVMGetTypeByName2(ctx->context, sname_set);
                    if (!struct_llvm) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: LLVM struct type for '%s' not found",
                                      parser_get_filename(), ast->line, ast->column, lay->layout_name);
                    }

                    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMValueRef get_fn = LLVMGetNamedFunction(ctx->module, "__layout_ptr_get");
                    if (!get_fn) {
                        LLVMTypeRef gft = LLVMFunctionType(ptr_t, &ptr_t, 1, 0);
                        get_fn = LLVMAddFunction(ctx->module, "__layout_ptr_get", gft);
                        LLVMSetLinkage(get_fn, LLVMExternalLinkage);
                    }
                    LLVMValueRef name_str2 = LLVMBuildGlobalStringPtr(ctx->builder, var_name, "lay_name");
                    LLVMValueRef heap_ptr  = LLVMBuildCall2(ctx->builder,
                                                            LLVMFunctionType(ptr_t, &ptr_t, 1, 0),
                                                            get_fn, &name_str2, 1, "lay_ptr");
                    LLVMValueRef gep_zero  = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                    LLVMValueRef gep_fidx  = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), field_idx, 0);
                    LLVMValueRef gep_idx[] = {gep_zero, gep_fidx};
                    LLVMValueRef gep       = LLVMBuildGEP2(ctx->builder, struct_llvm,
                                                           heap_ptr, gep_idx, 2, "fld_ptr");

                    LLVMValueRef stored = val.value;
                    if (type_is_integer(ft) && type_is_float(val.type))
                        stored = LLVMBuildFPToSI(ctx->builder, val.value, type_to_llvm(ctx, ft), "set_conv");
                    else if (type_is_float(ft) && type_is_integer(val.type))
                        stored = LLVMBuildSIToFP(ctx->builder, val.value, type_to_llvm(ctx, ft), "set_conv");

                    LLVMBuildStore(ctx->builder, stored, gep);
                    result.value = stored;
                    result.type  = type_clone(ft);
                    return result;
                }

                // ── Plain variable: (set! x 5.0) ────────────────────────────────────
                EnvEntry *entry = env_lookup(ctx->env, name_ast->symbol);
                if (!entry) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: unbound variable: %s",
                                  parser_get_filename(), ast->line, ast->column, name_ast->symbol);
                }

                /* Resolve the store target pointer.
                 *
                 * Locals (allocas) always live in the current function/module —
                 * entry->value is valid, use it directly.
                 *
                 * Globals may have a stale entry->value pointing into a dead module
                 * (e.g. after a failed compile + recover_module).  Always re-resolve
                 * globals by name from the current module.  If not yet declared here,
                 * add an extern declaration on the spot.                             */
                LLVMValueRef target;
                bool is_global = (entry->value &&
                                  LLVMGetValueKind(entry->value) == LLVMGlobalVariableValueKind);

                if (is_global) {
                    const char *gname = (entry->llvm_name && entry->llvm_name[0])
                        ? entry->llvm_name : entry->name;
                    target = LLVMGetNamedGlobal(ctx->module, gname);
                    if (!target) {
                        /* Declare extern in this module */
                        LLVMTypeRef lt = type_to_llvm(ctx, entry->type);
                        target = LLVMAddGlobal(ctx->module, lt, gname);
                        LLVMSetLinkage(target, LLVMExternalLinkage);
                    }
                    /* Keep entry in sync so subsequent loads in this module
                     * also get the correct in-module declaration.           */
                    entry->value = target;
                } else {
                    /* Local alloca — always valid in the current function */
                    target = entry->value;
                    if (!target) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'set!' target '%s' has no value",
                                      parser_get_filename(), ast->line, ast->column, name_ast->symbol);
                    }
                }

                CodegenResult val = codegen_expr(ctx, ast->list.items[2]);

                LLVMValueRef stored = val.value;
                if (type_is_integer(entry->type) && type_is_float(val.type)) {
                    stored = LLVMBuildFPToSI(ctx->builder, val.value,
                                             type_to_llvm(ctx, entry->type), "set_conv");
                } else if (type_is_float(entry->type) && type_is_integer(val.type)) {
                    stored = LLVMBuildSIToFP(ctx->builder, val.value,
                                             type_to_llvm(ctx, entry->type), "set_conv");
                }

                LLVMBuildStore(ctx->builder, stored, target);
                result.value = stored;
                result.type  = type_clone(entry->type);
                return result;
            }

            if (strcmp(head->symbol, "and") == 0) {
                if (ast->list.count < 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'and' requires at least 2 arguments",
                            parser_get_filename(), ast->line, ast->column);
                }

                LLVMTypeRef i1  = LLVMInt1TypeInContext(ctx->context);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);

                // Evaluate first condition
                CodegenResult first = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef acc = type_is_float(first.type)
                    ? LLVMBuildFCmp(ctx->builder, LLVMRealONE, first.value,
                                    LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0), "and0")
                    : LLVMBuildICmp(ctx->builder, LLVMIntNE, first.value,
                                    LLVMConstInt(LLVMTypeOf(first.value), 0, 0), "and0");

                // AND with remaining conditions
                for (size_t i = 2; i < ast->list.count; i++) {
                    CodegenResult next = codegen_expr(ctx, ast->list.items[i]);
                    LLVMValueRef cond = type_is_float(next.type)
                        ? LLVMBuildFCmp(ctx->builder, LLVMRealONE, next.value,
                                        LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0), "andi")
                        : LLVMBuildICmp(ctx->builder, LLVMIntNE, next.value,
                                        LLVMConstInt(LLVMTypeOf(next.value), 0, 0), "andi"); // use TypeOf not i64
                    acc = LLVMBuildAnd(ctx->builder, acc, cond, "and");
                }

                // Extend i1 to i64
                result.value = LLVMBuildZExt(ctx->builder, acc, i64, "and_ext");
                result.type  = type_int();
                return result;
            }

            if (strcmp(head->symbol, "or") == 0) {
                if (ast->list.count < 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'or' requires at least 2 arguments",
                            parser_get_filename(), ast->line, ast->column);
                }

                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);

                CodegenResult first = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef acc = type_is_float(first.type)
                    ? LLVMBuildFCmp(ctx->builder, LLVMRealONE, first.value,
                                    LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0), "or0")
                    : LLVMBuildICmp(ctx->builder, LLVMIntNE, first.value,
                                    LLVMConstInt(LLVMTypeOf(first.value), 0, 0), "or0");
                for (size_t i = 2; i < ast->list.count; i++) {
                    CodegenResult next = codegen_expr(ctx, ast->list.items[i]);
                    LLVMValueRef cond = type_is_float(next.type)
                        ? LLVMBuildFCmp(ctx->builder, LLVMRealONE, next.value,
                                        LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0), "ori")
                        : LLVMBuildICmp(ctx->builder, LLVMIntNE, next.value,
                                        LLVMConstInt(LLVMTypeOf(next.value), 0, 0), "ori");
                    acc = LLVMBuildOr(ctx->builder, acc, cond, "or");
                }

                result.value = LLVMBuildZExt(ctx->builder, acc, i64, "or_ext");
                result.type  = type_int();
                return result;
            }

            if (strcmp(head->symbol, "begin") == 0) {
                if (ast->list.count < 2) {
                    result.type  = type_int();
                    result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                    return result;
                }
                for (size_t i = 1; i < ast->list.count - 1; i++)
                    codegen_expr(ctx, ast->list.items[i]);
                return codegen_expr(ctx, ast->list.items[ast->list.count - 1]);
            }

            if (strcmp(head->symbol, "if") == 0) {
                if (ast->list.count < 3 || ast->list.count > 4) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'if' requires 2 or 3 arguments",
                            parser_get_filename(), ast->line, ast->column);
                }

                // Condition
                CodegenResult cond_result = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef cond_val;
                if (type_is_float(cond_result.type)) {
                    cond_val = LLVMBuildFCmp(ctx->builder, LLVMRealONE,
                                             cond_result.value,
                                             LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0),
                                             "ifcond");
                } else {
                    cond_val = LLVMBuildICmp(ctx->builder, LLVMIntNE,
                                             cond_result.value,
                                             LLVMConstInt(LLVMTypeOf(cond_result.value), 0, 0),
                                             "ifcond");
                }

                LLVMValueRef      func     = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                LLVMBasicBlockRef then_bb  = LLVMAppendBasicBlockInContext(ctx->context, func, "then");
                LLVMBasicBlockRef else_bb  = LLVMAppendBasicBlockInContext(ctx->context, func, "else");
                LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "ifmerge");
                LLVMBuildCondBr(ctx->builder, cond_val, then_bb, else_bb);

                // Then branch
                LLVMPositionBuilderAtEnd(ctx->builder, then_bb);
                CodegenResult then_result = codegen_expr(ctx, ast->list.items[2]);
                LLVMBasicBlockRef then_end_bb = LLVMGetInsertBlock(ctx->builder);

                // Else branch
                LLVMPositionBuilderAtEnd(ctx->builder, else_bb);
                CodegenResult else_result = {NULL, NULL};
                if (ast->list.count == 4)
                    else_result = codegen_expr(ctx, ast->list.items[3]);
                LLVMBasicBlockRef else_end_bb = LLVMGetInsertBlock(ctx->builder);

                // No else or no values — branch both to merge, return dummy
                if (ast->list.count != 4 || !then_result.value || !else_result.value
                    || !then_result.type || !else_result.type) {
                    LLVMPositionBuilderAtEnd(ctx->builder, then_end_bb);
                    build_br_if_no_terminator(ctx->builder, merge_bb);
                    LLVMPositionBuilderAtEnd(ctx->builder, else_end_bb);
                    build_br_if_no_terminator(ctx->builder, merge_bb);
                    LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
                    result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                    result.type  = type_int();
                    return result;
                }

                // Classify branch types
                bool then_is_char   = then_result.type->kind == TYPE_CHAR;
                bool else_is_char   = else_result.type->kind == TYPE_CHAR;
                bool then_is_float  = type_is_float(then_result.type);
                bool else_is_float  = type_is_float(else_result.type);
                bool then_is_int    = type_is_integer(then_result.type);
                bool else_is_int    = type_is_integer(else_result.type);
                bool then_is_string = then_result.type->kind == TYPE_STRING;
                bool else_is_string = else_result.type->kind == TYPE_STRING;
                bool then_is_bool   = then_result.type->kind == TYPE_BOOL;
                bool else_is_bool   = else_result.type->kind == TYPE_BOOL;

                // Determine common PHI type
                LLVMTypeRef phi_llvm_type;
                Type       *phi_type;

                if (then_is_string || else_is_string) {
                    phi_llvm_type = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    phi_type      = type_string();
                } else if (then_is_char || else_is_char) {
                    phi_llvm_type = LLVMInt8TypeInContext(ctx->context);
                    phi_type      = type_char();
                } else if (then_is_float || else_is_float) {
                    phi_llvm_type = LLVMDoubleTypeInContext(ctx->context);
                    phi_type      = type_float();
                } else if (then_is_bool && else_is_bool) {
                    phi_llvm_type = LLVMInt1TypeInContext(ctx->context);
                    phi_type      = type_bool();
                } else {
                    phi_llvm_type = LLVMInt64TypeInContext(ctx->context);
                    phi_type      = type_int();
                }

                // Coerce then value and branch to merge
                LLVMPositionBuilderAtEnd(ctx->builder, then_end_bb);
                LLVMValueRef then_val = then_result.value;
                if (!LLVMGetBasicBlockTerminator(then_end_bb)) {
                    if (then_is_string || else_is_string) {
                        // no coercion needed for pointers
                    } else if (then_is_char || else_is_char) {
                        if (!then_is_char && then_is_int)
                            then_val = LLVMBuildTrunc(ctx->builder, then_val, phi_llvm_type, "to_char");
                    } else if (then_is_float || else_is_float) {
                        if (then_is_int)
                            then_val = LLVMBuildSIToFP(ctx->builder, then_val, phi_llvm_type, "to_float");
                    } else {
                        if (LLVMTypeOf(then_val) != phi_llvm_type)
                            then_val = LLVMBuildZExt(ctx->builder, then_val, phi_llvm_type, "ext");
                    }
                    LLVMBuildBr(ctx->builder, merge_bb);
                }
                LLVMBasicBlockRef then_phi_bb = LLVMGetInsertBlock(ctx->builder);

                // Coerce else value and branch to merge
                LLVMPositionBuilderAtEnd(ctx->builder, else_end_bb);
                LLVMValueRef else_val = else_result.value;
                if (!LLVMGetBasicBlockTerminator(else_end_bb)) {
                    if (then_is_string || else_is_string) {
                        // no coercion needed for pointers
                    } else if (then_is_char || else_is_char) {
                        if (!else_is_char && else_is_int)
                            else_val = LLVMBuildTrunc(ctx->builder, else_val, phi_llvm_type, "to_char");
                    } else if (then_is_float || else_is_float) {
                        if (else_is_int)
                            else_val = LLVMBuildSIToFP(ctx->builder, else_val, phi_llvm_type, "to_float");
                    } else {
                        if (LLVMTypeOf(else_val) != phi_llvm_type)
                            else_val = LLVMBuildZExt(ctx->builder, else_val, phi_llvm_type, "ext");
                    }
                    LLVMBuildBr(ctx->builder, merge_bb);
                }
                LLVMBasicBlockRef else_phi_bb = LLVMGetInsertBlock(ctx->builder);

                // Merge + PHI
                LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
                LLVMValueRef phi = LLVMBuildPhi(ctx->builder, phi_llvm_type, "iftmp");
                LLVMAddIncoming(phi, &then_val, &then_phi_bb, 1);
                LLVMAddIncoming(phi, &else_val, &else_phi_bb, 1);
                result.value = phi;
                result.type  = phi_type;
                return result;
            }

            if (strcmp(head->symbol, "for") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'for' requires a binding and a body",
                            parser_get_filename(), ast->line, ast->column);
                }

                AST *binding = ast->list.items[1];
                LLVMTypeRef  i64  = LLVMInt64TypeInContext(ctx->context);
                LLVMValueRef func = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));

                if (binding->type != AST_ARRAY ||
                    binding->array.element_count < 1 ||
                    binding->array.element_count > 4) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'for' binding must be [n], [var start end], or [var start end step]",
                            parser_get_filename(), ast->line, ast->column);
                }

                LLVMValueRef start_val;
                LLVMValueRef end_val;
                LLVMValueRef step_val;
                const char  *var_name;
                bool         has_var;
                bool         negative_step;

                if (binding->array.element_count == 1) {
                    CodegenResult count_r = codegen_expr(ctx, binding->array.elements[0]);
                    start_val     = LLVMConstInt(i64, 0, 0);
                    end_val       = type_is_float(count_r.type)
                        ? LLVMBuildFPToSI(ctx->builder, count_r.value, i64, "count")
                        : count_r.value;
                    step_val      = LLVMConstInt(i64, 1, 0);
                    var_name      = "__for_i";
                    has_var       = false;
                    negative_step = false;
                } else {
                    if (binding->array.elements[0]->type != AST_SYMBOL) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'for' loop variable must be a symbol",
                                parser_get_filename(), ast->line, ast->column);
                    }

                    CodegenResult start_r = codegen_expr(ctx, binding->array.elements[1]);
                    CodegenResult end_r   = codegen_expr(ctx, binding->array.elements[2]);
                    CodegenResult step_r;
                    if (binding->array.element_count == 4) {
                        step_r = codegen_expr(ctx, binding->array.elements[3]);
                    } else {
                        step_r.value = LLVMConstInt(i64, 1, 0);
                        step_r.type  = type_int();
                    }

                    start_val = type_is_float(start_r.type)
                        ? LLVMBuildFPToSI(ctx->builder, start_r.value, i64, "start")
                        : start_r.value;
                    end_val = type_is_float(end_r.type)
                        ? LLVMBuildFPToSI(ctx->builder, end_r.value, i64, "end")
                        : end_r.value;
                    step_val = type_is_float(step_r.type)
                        ? LLVMBuildFPToSI(ctx->builder, step_r.value, i64, "step")
                        : step_r.value;

                    var_name      = binding->array.elements[0]->symbol;
                    has_var       = true;
                    negative_step = (binding->array.element_count == 4 &&
                                     binding->array.elements[3]->type == AST_NUMBER &&
                                     binding->array.elements[3]->number < 0);
                }

                LLVMValueRef i_ptr = LLVMBuildAlloca(ctx->builder, i64, var_name);
                LLVMBuildStore(ctx->builder, start_val, i_ptr);

                LLVMBasicBlockRef cond_bb  = LLVMAppendBasicBlockInContext(ctx->context, func, "for_cond");
                LLVMBasicBlockRef body_bb  = LLVMAppendBasicBlockInContext(ctx->context, func, "for_body");
                LLVMBasicBlockRef after_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "for_after");

                LLVMBuildBr(ctx->builder, cond_bb);

                // Condition
                LLVMPositionBuilderAtEnd(ctx->builder, cond_bb);
                LLVMValueRef i_val    = LLVMBuildLoad2(ctx->builder, i64, i_ptr, var_name);
                LLVMValueRef cond_val = negative_step
                    ? LLVMBuildICmp(ctx->builder, LLVMIntSGT, i_val, end_val, "for_cond")
                    : LLVMBuildICmp(ctx->builder, LLVMIntSLT, i_val, end_val, "for_cond");
                LLVMBuildCondBr(ctx->builder, cond_val, body_bb, after_bb);

                // Body
                LLVMPositionBuilderAtEnd(ctx->builder, body_bb);
                Env *saved_env = ctx->env;
                ctx->env = env_create_child(saved_env);
                if (has_var)
                    env_insert(ctx->env, var_name, type_int(), i_ptr);

                codegen_expr(ctx, ast->list.items[2]);

                env_free(ctx->env);
                ctx->env = saved_env;

                // Emit increment in current block (wherever body left us)
                // If body ended with a terminator (e.g. nested for's after_bb already branched),
                // we need a fresh block for the increment
                if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder))) {
                    LLVMBasicBlockRef inc_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "for_inc");
                    LLVMPositionBuilderAtEnd(ctx->builder, inc_bb);
                }
                LLVMValueRef i_cur  = LLVMBuildLoad2(ctx->builder, i64, i_ptr, "i_cur");
                LLVMValueRef i_next = LLVMBuildAdd(ctx->builder, i_cur, step_val, "i_next");
                LLVMBuildStore(ctx->builder, i_next, i_ptr);
                LLVMBuildBr(ctx->builder, cond_bb);

                // After
                LLVMPositionBuilderAtEnd(ctx->builder, after_bb);
                result.value = LLVMConstInt(i64, 0, 0);
                result.type  = type_int();
                return result;
            }

            if (strcmp(head->symbol, "<")  == 0 || strcmp(head->symbol, ">")  == 0 ||
                strcmp(head->symbol, "<=") == 0 || strcmp(head->symbol, ">=") == 0 ||
                strcmp(head->symbol, "=")  == 0 || strcmp(head->symbol, "!=") == 0) {

                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' requires 2 arguments",
                            parser_get_filename(), ast->line, ast->column, head->symbol);
                }

                CodegenResult lhs = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult rhs = codegen_expr(ctx, ast->list.items[2]);

                LLVMValueRef lv = lhs.value, rv = rhs.value;
                bool use_float = type_is_float(lhs.type) || type_is_float(rhs.type);
                if (use_float) {
                    if (type_is_integer(lhs.type))
                        lv = LLVMBuildSIToFP(ctx->builder, lv,
                                             LLVMDoubleTypeInContext(ctx->context), "lf");
                    if (type_is_integer(rhs.type))
                        rv = LLVMBuildSIToFP(ctx->builder, rv,
                                             LLVMDoubleTypeInContext(ctx->context), "rf");
                }

                LLVMValueRef cmp;
                const char *op = head->symbol;
                if (use_float) {
                    LLVMRealPredicate pred =
                        strcmp(op, "<")  == 0 ? LLVMRealOLT :
                        strcmp(op, ">")  == 0 ? LLVMRealOGT :
                        strcmp(op, "<=") == 0 ? LLVMRealOLE :
                        strcmp(op, ">=") == 0 ? LLVMRealOGE :
                        strcmp(op, "=")  == 0 ? LLVMRealOEQ : LLVMRealONE;
                    cmp = LLVMBuildFCmp(ctx->builder, pred, lv, rv, "cmptmp");
                } else {
                    LLVMIntPredicate pred =
                        strcmp(op, "<")  == 0 ? LLVMIntSLT :
                        strcmp(op, ">")  == 0 ? LLVMIntSGT :
                        strcmp(op, "<=") == 0 ? LLVMIntSLE :
                        strcmp(op, ">=") == 0 ? LLVMIntSGE :
                        strcmp(op, "=")  == 0 ? LLVMIntEQ  : LLVMIntNE;
                    cmp = LLVMBuildICmp(ctx->builder, pred, lv, rv, "cmptmp");
                }

                // Extend i1 to i64 so it can be used as a value
                /* result.value = LLVMBuildZExt(ctx->builder, cmp, */
                /*                              LLVMInt64TypeInContext(ctx->context), "bool"); */
                /* result.type  = type_int(); */
                result.value = cmp;
                result.type  = type_bool();
                return result;
            }

            // Modulo operator
            if (strcmp(head->symbol, "mod") == 0 ||
                strcmp(head->symbol, "%")   == 0) {

                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'mod' requires 2 arguments",
                                  parser_get_filename(), ast->line, ast->column);
                }

                CodegenResult lhs = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult rhs = codegen_expr(ctx, ast->list.items[2]);

                bool use_float = type_is_float(lhs.type) || type_is_float(rhs.type);

                if (use_float) {
                    LLVMValueRef lv = lhs.value, rv = rhs.value;
                    if (type_is_integer(lhs.type))
                        lv = LLVMBuildSIToFP(ctx->builder, lv,
                                             LLVMDoubleTypeInContext(ctx->context), "lf");
                    if (type_is_integer(rhs.type))
                        rv = LLVMBuildSIToFP(ctx->builder, rv,
                                             LLVMDoubleTypeInContext(ctx->context), "rf");
                    result.value = LLVMBuildFRem(ctx->builder, lv, rv, "fremtmp");
                    result.type  = type_float();
                } else {
                    result.value = LLVMBuildSRem(ctx->builder, lhs.value, rhs.value, "sremtmp");
                    result.type  = type_int();
                }
                return result;
            }


            // Arithmetic operators
            if (strcmp(head->symbol, "+") == 0 ||
                strcmp(head->symbol, "-") == 0 ||
                strcmp(head->symbol, "*") == 0 ||
                strcmp(head->symbol, "/") == 0) {

                const char *op = head->symbol;

                if (ast->list.count < 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' requires at least 1 argument",
                            parser_get_filename(), ast->line, ast->column, op);
                }

                CodegenResult first = codegen_expr(ctx, ast->list.items[1]);
                Type         *result_type  = first.type;
                LLVMValueRef  result_value = first.value;

                if (!type_is_numeric(result_type)) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: cannot perform arithmetic on type %s",
                            parser_get_filename(), ast->line, ast->column, type_to_string(result_type));
                }

                // Coerce Char to Int upfront
                if (result_type->kind == TYPE_CHAR) {
                    result_value = LLVMBuildZExt(ctx->builder, result_value,
                                                 LLVMInt64TypeInContext(ctx->context), "char_to_int");
                    result_type  = type_int();
                }

                // Unary minus
                if (strcmp(op, "-") == 0 && ast->list.count == 2) {
                    if (type_is_float(result_type)) {
                        result.type  = result_type;
                        result.value = LLVMBuildFNeg(ctx->builder, result_value, "negtmp");
                    } else {
                        result.type  = result_type;
                        result.value = LLVMBuildSub(ctx->builder,
                                                    LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0),
                                                    result_value, "negtmp");
                    }
                    return result;
                }

                // Unary reciprocal
                if (strcmp(op, "/") == 0 && ast->list.count == 2) {
                    LLVMValueRef one = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 1.0);
                    if (!type_is_float(result_type))
                        result_value = LLVMBuildSIToFP(ctx->builder, result_value,
                                                       LLVMDoubleTypeInContext(ctx->context), "tofloat");
                    result.type  = type_float();
                    result.value = LLVMBuildFDiv(ctx->builder, one, result_value, "invtmp");
                    return result;
                }

                // Ratio arithmetic
                if (result_type->kind == TYPE_RATIO) {
                    for (size_t i = 2; i < ast->list.count; i++) {
                        CodegenResult rhs = codegen_expr(ctx, ast->list.items[i]);
                        if (rhs.type->kind != TYPE_RATIO) {
                            CODEGEN_ERROR(ctx, "%s:%d:%d: error: cannot mix Ratio with other numeric types",
                                    parser_get_filename(), ast->line, ast->column);
                        }
                        LLVMValueRef fn = NULL;
                        if      (strcmp(op, "+") == 0) fn = get_rt_ratio_add(ctx);
                        else if (strcmp(op, "-") == 0) fn = get_rt_ratio_sub(ctx);
                        else if (strcmp(op, "*") == 0) fn = get_rt_ratio_mul(ctx);
                        else if (strcmp(op, "/") == 0) fn = get_rt_ratio_div(ctx);
                        LLVMValueRef args[] = {result_value, rhs.value};
                        result_value = LLVMBuildCall2(ctx->builder,
                                                      LLVMGlobalGetValueType(fn),
                                                      fn, args, 2, "ratio_op");
                    }
                    result.type  = result_type;
                    result.value = result_value;
                    return result;
                }

                // Binary operations
                for (size_t i = 2; i < ast->list.count; i++) {
                    CodegenResult rhs = codegen_expr(ctx, ast->list.items[i]);

                    if (!type_is_numeric(rhs.type)) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: cannot perform arithmetic on type %s",
                                parser_get_filename(), ast->line, ast->column, type_to_string(rhs.type));
                    }

                    // Coerce Char rhs to Int
                    if (rhs.type->kind == TYPE_CHAR) {
                        rhs.value = LLVMBuildZExt(ctx->builder, rhs.value,
                                                  LLVMInt64TypeInContext(ctx->context), "char_to_int");
                        rhs.type  = type_int();
                    }

                    // Check incompatible base types
                    if ((result_type->kind == TYPE_HEX || result_type->kind == TYPE_BIN || result_type->kind == TYPE_OCT) &&
                        (rhs.type->kind  == TYPE_HEX || rhs.type->kind  == TYPE_BIN || rhs.type->kind  == TYPE_OCT) &&
                        result_type->kind != rhs.type->kind) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: cannot mix %s and %s in arithmetic",
                                parser_get_filename(), ast->line, ast->column,
                                type_to_string(result_type), type_to_string(rhs.type));
                    }

                    // Determine result type
                    Type *new_result_type;
                    if (type_is_float(result_type) || type_is_float(rhs.type))
                        new_result_type = type_float();
                    else if (result_type->kind == rhs.type->kind)
                        new_result_type = result_type;
                    else
                        new_result_type = type_int();

                    // Convert operands to common type
                    LLVMValueRef lhs_val = result_value;
                    LLVMValueRef rhs_val = rhs.value;

                    if (type_is_float(new_result_type)) {
                        if (type_is_integer(result_type))
                            lhs_val = LLVMBuildSIToFP(ctx->builder, lhs_val,
                                                      LLVMDoubleTypeInContext(ctx->context), "tofloat");
                        if (type_is_integer(rhs.type))
                            rhs_val = LLVMBuildSIToFP(ctx->builder, rhs_val,
                                                      LLVMDoubleTypeInContext(ctx->context), "tofloat");
                    }

                    // Perform operation
                    if (type_is_float(new_result_type)) {
                        if      (strcmp(op, "+") == 0) result_value = LLVMBuildFAdd(ctx->builder, lhs_val, rhs_val, "addtmp");
                        else if (strcmp(op, "-") == 0) result_value = LLVMBuildFSub(ctx->builder, lhs_val, rhs_val, "subtmp");
                        else if (strcmp(op, "*") == 0) result_value = LLVMBuildFMul(ctx->builder, lhs_val, rhs_val, "multmp");
                        else if (strcmp(op, "/") == 0) result_value = LLVMBuildFDiv(ctx->builder, lhs_val, rhs_val, "divtmp");
                    } else {
                        if      (strcmp(op, "+") == 0) result_value = LLVMBuildAdd (ctx->builder, lhs_val, rhs_val, "addtmp");
                        else if (strcmp(op, "-") == 0) result_value = LLVMBuildSub (ctx->builder, lhs_val, rhs_val, "subtmp");
                        else if (strcmp(op, "*") == 0) result_value = LLVMBuildMul (ctx->builder, lhs_val, rhs_val, "multmp");
                        else if (strcmp(op, "/") == 0) result_value = LLVMBuildSDiv(ctx->builder, lhs_val, rhs_val, "divtmp");
                    }

                    result_type = new_result_type;
                }

                result.type  = result_type;
                result.value = result_value;
                return result;
            }

            // Check if it's a user-defined function or variable
            EnvEntry *entry = env_lookup(ctx->env, head->symbol);

            // If it's a variable being used in function position, that's an error
            if (entry && entry->kind == ENV_VAR) {
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' is a variable, not a function",
                        parser_get_filename(), ast->line, ast->column, head->symbol);
            }

            // Function call - FIXED VERSION
            if (entry && entry->kind == ENV_FUNC) {
                // Check argument count
                int declared_params = entry->param_count - entry->lifted_count;
                size_t arg_count = ast->list.count - 1;
                if ((int)arg_count != declared_params) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: function '%s' expects %d arguments, got %zu",
                            parser_get_filename(), ast->line, ast->column,
                            head->symbol, entry->param_count, arg_count);
                }

                int total_args = declared_params + entry->lifted_count;
                LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (total_args ? total_args : 1));
                for (int i = 0; i < declared_params; i++) {
                    CodegenResult arg_result = codegen_expr(ctx, ast->list.items[i + 1]);
                    Type *expected_type = entry->params[i].type;
                    Type *actual_type   = arg_result.type;

                    LLVMValueRef converted_arg = arg_result.value;

                    // Generic/polymorphic parameter — auto-box the argument
                    if (expected_type && expected_type->kind == TYPE_UNKNOWN) {
                        if (actual_type->kind == TYPE_UNKNOWN) {
                            // already opaque RuntimeValue* — pass through
                            converted_arg = arg_result.value;
                        } else {
                            // typed value — box it into RuntimeValue*
                            converted_arg = codegen_box(ctx, arg_result.value, arg_result.type);
                        }
                    }
                    // Typed parameter — perform type conversions as before
                    else if (expected_type && actual_type && expected_type->kind != actual_type->kind) {
                        LLVMTypeRef expected_llvm = type_to_llvm(ctx, expected_type);

                        if (type_is_integer(expected_type) && type_is_float(actual_type)) {
                            converted_arg = LLVMBuildFPToSI(ctx->builder, arg_result.value,
                                                            expected_llvm, "arg_conv");
                        } else if (type_is_float(expected_type) && type_is_integer(actual_type)) {
                            if (actual_type->kind == TYPE_CHAR) {
                                LLVMValueRef extended = LLVMBuildSExt(ctx->builder, arg_result.value,
                                                                      LLVMInt64TypeInContext(ctx->context), "ext");
                                converted_arg = LLVMBuildSIToFP(ctx->builder, extended,
                                                                expected_llvm, "arg_conv");
                            } else {
                                converted_arg = LLVMBuildSIToFP(ctx->builder, arg_result.value,
                                                                expected_llvm, "arg_conv");
                            }
                        } else if (expected_type->kind == TYPE_CHAR && type_is_integer(actual_type)) {
                            converted_arg = LLVMBuildTrunc(ctx->builder, arg_result.value,
                                                           expected_llvm, "arg_conv");

                        } else if (type_is_integer(expected_type) && actual_type->kind == TYPE_CHAR) {
                            converted_arg = LLVMBuildSExt(ctx->builder, arg_result.value,
                                                          expected_llvm, "arg_conv");
                        } else if (type_is_integer(expected_type) && type_is_integer(actual_type)) {
                            converted_arg = arg_result.value;
                        } else if (type_is_integer(expected_type) && actual_type->kind == TYPE_CHAR) {
                            converted_arg = LLVMBuildSExt(ctx->builder, arg_result.value,
                                                          expected_llvm, "arg_conv");
                        } else if (type_is_integer(expected_type) && type_is_integer(actual_type)) {
                            converted_arg = arg_result.value;
                        } else if (type_is_integer(expected_type) && actual_type->kind == TYPE_BOOL) {
                            converted_arg = LLVMBuildZExt(ctx->builder, arg_result.value,
                                                          type_to_llvm(ctx, expected_type), "bool_to_int");
                        } else if (expected_type->kind == TYPE_BOOL && type_is_integer(actual_type)) {
                            converted_arg = LLVMBuildICmp(ctx->builder, LLVMIntNE, arg_result.value,
                                                          LLVMConstInt(LLVMTypeOf(arg_result.value), 0, 0),
                                                          "int_to_bool");
                        }

                    }

                    args[i] = converted_arg;
                }

                // Append hidden captured variables by loading from current env (walks parent chain)
                for (int i = 0; i < entry->lifted_count; i++) {
                    int           idx      = declared_params + i;
                    const char   *cap_name = entry->params[idx].name;
                    EnvEntry     *cap_e    = env_lookup(ctx->env, cap_name);
                    LLVMValueRef  cap_val  = NULL;
                    if (cap_e && cap_e->kind == ENV_VAR && cap_e->value) {
                        LLVMTypeRef cap_llvm = type_to_llvm(ctx, cap_e->type);
                        cap_val = LLVMBuildLoad2(ctx->builder, cap_llvm, cap_e->value, cap_name);
                    } else {
                        cap_val = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                    }
                    args[idx] = cap_val;
                }

                // Build call type from entry metadata, not from the LLVM function value
                // (LLVMGlobalGetValueType may have wrong types if function was declared with defaults)
                LLVMTypeRef *param_llvm_types = malloc(sizeof(LLVMTypeRef) * (total_args ? total_args : 1));
                for (int i = 0; i < total_args; i++)
                    param_llvm_types[i] = type_to_llvm(ctx, entry->params[i].type);
                LLVMTypeRef call_ft = LLVMFunctionType(
                    type_to_llvm(ctx, entry->return_type),
                    param_llvm_types,
                    total_args, 0);
                free(param_llvm_types);

                result.value = LLVMBuildCall2(ctx->builder, call_ft, entry->func_ref,
                                              args, total_args, "calltmp");


                result.type = type_clone(entry->return_type);


                free(args);
                return result;
            }

            // Type cast: (TypeName expr)
            const char *cast_target = NULL;
            if (strcmp(head->symbol, "Int")    == 0) cast_target = "Int";
            else if (strcmp(head->symbol, "Float")  == 0) cast_target = "Float";
            else if (strcmp(head->symbol, "Char")   == 0) cast_target = "Char";
            else if (strcmp(head->symbol, "String") == 0) cast_target = "String";
            else if (strcmp(head->symbol, "Hex")    == 0) cast_target = "Hex";
            else if (strcmp(head->symbol, "Bin")    == 0) cast_target = "Bin";
            else if (strcmp(head->symbol, "Oct")    == 0) cast_target = "Oct";

            if (cast_target) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: type cast '%s' requires exactly 1 argument",
                            parser_get_filename(), ast->line, ast->column, cast_target);
                }

                AST *arg = ast->list.items[1];

                // Special case: String cast - needs to produce a string representation
                if (strcmp(cast_target, "String") == 0) {
                    LLVMValueRef printf_fn = get_or_declare_printf(ctx);

                    // For string identity
                    if (arg->type == AST_STRING) {
                        result.type  = type_string();
                        result.value = LLVMBuildGlobalStringPtr(ctx->builder, arg->string, "cast_str");
                        return result;
                    }

                    // For char -> string: single char as string
                    if (arg->type == AST_CHAR) {
                        char buf[2] = { arg->character, '\0' };
                        result.type  = type_string();
                        result.value = LLVMBuildGlobalStringPtr(ctx->builder, buf, "cast_str");
                        return result;
                    }

                    // For symbol (variable)
                    if (arg->type == AST_SYMBOL) {
                        // Check if it's a function name
                        EnvEntry *sym_entry = env_lookup(ctx->env, arg->symbol);
                        if (sym_entry && sym_entry->kind == ENV_FUNC) {
                            result.type  = type_string();
                            result.value = LLVMBuildGlobalStringPtr(ctx->builder, arg->symbol, "cast_str");
                            return result;
                        }
                        if (sym_entry && sym_entry->kind == ENV_BUILTIN) {
                            result.type  = type_string();
                            result.value = LLVMBuildGlobalStringPtr(ctx->builder, arg->symbol, "cast_str");
                            return result;
                        }
                    }

                    // For numeric types: build a snprintf buffer
                    // We'll allocate a static 64-byte buffer and use sprintf
                    LLVMValueRef snprintf_fn = LLVMGetNamedFunction(ctx->module, "sprintf");
                    if (!snprintf_fn) {
                        LLVMTypeRef sprintf_args[] = {
                            LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0),
                            LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0)
                        };
                        LLVMTypeRef sprintf_type = LLVMFunctionType(
                            LLVMInt32TypeInContext(ctx->context), sprintf_args, 2, true);
                        snprintf_fn = LLVMAddFunction(ctx->module, "sprintf", sprintf_type);
                    }

                    LLVMValueRef buf = LLVMBuildArrayAlloca(ctx->builder,
                        LLVMInt8TypeInContext(ctx->context),
                        LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 64, 0),
                        "str_buf");

                    CodegenResult arg_result = codegen_expr(ctx, arg);
                    Type *at = arg_result.type;

                    LLVMValueRef fmt_s;
                    LLVMValueRef val_to_print = arg_result.value;

                    if (at->kind == TYPE_FLOAT) {
                        fmt_s = LLVMBuildGlobalStringPtr(ctx->builder, "%g", "fmt_sg");
                        LLVMValueRef sargs[] = { buf, fmt_s, val_to_print };
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(snprintf_fn),
                                       snprintf_fn, sargs, 3, "");
                    } else if (at->kind == TYPE_CHAR) {
                        fmt_s = LLVMBuildGlobalStringPtr(ctx->builder, "%c", "fmt_sc");
                        LLVMValueRef sargs[] = { buf, fmt_s, val_to_print };
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(snprintf_fn),
                                       snprintf_fn, sargs, 3, "");
                    } else if (at->kind == TYPE_HEX) {
                        fmt_s = LLVMBuildGlobalStringPtr(ctx->builder, "0x%lX", "fmt_shex");
                        LLVMValueRef sargs[] = { buf, fmt_s, val_to_print };
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(snprintf_fn),
                                       snprintf_fn, sargs, 3, "");
                    } else if (at->kind == TYPE_BIN) {
                        // Build "0b" + binary digits via helper but into buffer
                        // We'll use a small inline approach: sprintf the int then convert
                        // For simplicity: emit a call to a helper that fills the buffer
                        LLVMValueRef bin_to_str_fn = LLVMGetNamedFunction(ctx->module, "__bin_to_str");
                        if (!bin_to_str_fn) {
                            LLVMTypeRef bts_params[] = {
                                LLVMInt64TypeInContext(ctx->context),
                                LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0)
                            };
                            LLVMTypeRef bts_type = LLVMFunctionType(
                                LLVMVoidTypeInContext(ctx->context), bts_params, 2, false);
                            bin_to_str_fn = LLVMAddFunction(ctx->module, "__bin_to_str", bts_type);

                            LLVMBasicBlockRef saved2 = LLVMGetInsertBlock(ctx->builder);
                            LLVMBasicBlockRef bts_entry = LLVMAppendBasicBlockInContext(
                                ctx->context, bin_to_str_fn, "entry");
                            LLVMPositionBuilderAtEnd(ctx->builder, bts_entry);

                            LLVMValueRef bts_n   = LLVMGetParam(bin_to_str_fn, 0);
                            LLVMValueRef bts_buf = LLVMGetParam(bin_to_str_fn, 1);

                            // Write "0b" then bits
                            LLVMValueRef pos_ptr = LLVMBuildAlloca(ctx->builder,
                                LLVMInt32TypeInContext(ctx->context), "pos");
                            LLVMBuildStore(ctx->builder,
                                LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0), pos_ptr);

                            // store '0'
                            LLVMValueRef p0 = LLVMBuildLoad2(ctx->builder,
                                LLVMInt32TypeInContext(ctx->context), pos_ptr, "p");
                            LLVMValueRef p0_64 = LLVMBuildSExt(ctx->builder, p0,
                                LLVMInt64TypeInContext(ctx->context), "p64");
                            LLVMValueRef gep0 = LLVMBuildGEP2(ctx->builder,
                                LLVMInt8TypeInContext(ctx->context), bts_buf, &p0_64, 1, "gep");
                            LLVMBuildStore(ctx->builder,
                                LLVMConstInt(LLVMInt8TypeInContext(ctx->context), '0', 0), gep0);
                            LLVMBuildStore(ctx->builder,
                                LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 1, 0), pos_ptr);

                            // store 'b'
                            LLVMValueRef p1 = LLVMBuildLoad2(ctx->builder,
                                LLVMInt32TypeInContext(ctx->context), pos_ptr, "p");
                            LLVMValueRef p1_64 = LLVMBuildSExt(ctx->builder, p1,
                                LLVMInt64TypeInContext(ctx->context), "p64");
                            LLVMValueRef gep1 = LLVMBuildGEP2(ctx->builder,
                                LLVMInt8TypeInContext(ctx->context), bts_buf, &p1_64, 1, "gep");
                            LLVMBuildStore(ctx->builder,
                                LLVMConstInt(LLVMInt8TypeInContext(ctx->context), 'b', 0), gep1);
                            LLVMBuildStore(ctx->builder,
                                LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 2, 0), pos_ptr);

                            // bit loop
                            LLVMValueRef bidx_ptr = LLVMBuildAlloca(ctx->builder,
                                LLVMInt32TypeInContext(ctx->context), "bidx");
                            LLVMBuildStore(ctx->builder,
                                LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 63, 0), bidx_ptr);
                            LLVMValueRef bstarted_ptr = LLVMBuildAlloca(ctx->builder,
                                LLVMInt32TypeInContext(ctx->context), "bstarted");
                            LLVMBuildStore(ctx->builder,
                                LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0), bstarted_ptr);

                            LLVMBasicBlockRef blc = LLVMAppendBasicBlockInContext(ctx->context, bin_to_str_fn, "blc");
                            LLVMBasicBlockRef blb = LLVMAppendBasicBlockInContext(ctx->context, bin_to_str_fn, "blb");
                            LLVMBasicBlockRef ble = LLVMAppendBasicBlockInContext(ctx->context, bin_to_str_fn, "ble");
                            LLVMBuildBr(ctx->builder, blc);

                            LLVMPositionBuilderAtEnd(ctx->builder, blc);
                            LLVMValueRef biv = LLVMBuildLoad2(ctx->builder,
                                LLVMInt32TypeInContext(ctx->context), bidx_ptr, "biv");
                            LLVMValueRef bcond = LLVMBuildICmp(ctx->builder, LLVMIntSGE, biv,
                                LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0), "bcond");
                            LLVMBuildCondBr(ctx->builder, bcond, blb, ble);

                            LLVMPositionBuilderAtEnd(ctx->builder, blb);
                            LLVMValueRef biv2 = LLVMBuildLoad2(ctx->builder,
                                LLVMInt32TypeInContext(ctx->context), bidx_ptr, "biv2");
                            LLVMValueRef biv64 = LLVMBuildSExt(ctx->builder, biv2,
                                LLVMInt64TypeInContext(ctx->context), "biv64");
                            LLVMValueRef bbit = LLVMBuildLShr(ctx->builder, bts_n, biv64, "bbit");
                            LLVMValueRef bbit1 = LLVMBuildAnd(ctx->builder, bbit,
                                LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 1, 0), "bbit1");
                            LLVMValueRef bstarted = LLVMBuildLoad2(ctx->builder,
                                LLVMInt32TypeInContext(ctx->context), bstarted_ptr, "bstarted");
                            LLVMValueRef bis_one = LLVMBuildICmp(ctx->builder, LLVMIntEQ, bbit1,
                                LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 1, 0), "bis_one");
                            LLVMValueRef bis_started = LLVMBuildICmp(ctx->builder, LLVMIntNE, bstarted,
                                LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0), "bis_started");
                            LLVMValueRef bsp = LLVMBuildOr(ctx->builder, bis_one, bis_started, "bsp");

                            LLVMBasicBlockRef bpb = LLVMAppendBasicBlockInContext(ctx->context, bin_to_str_fn, "bpb");
                            LLVMBasicBlockRef bsb = LLVMAppendBasicBlockInContext(ctx->context, bin_to_str_fn, "bsb");
                            LLVMBuildCondBr(ctx->builder, bsp, bpb, bsb);

                            LLVMPositionBuilderAtEnd(ctx->builder, bpb);
                            LLVMBuildStore(ctx->builder,
                                LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 1, 0), bstarted_ptr);
                            LLVMValueRef bpos = LLVMBuildLoad2(ctx->builder,
                                LLVMInt32TypeInContext(ctx->context), pos_ptr, "bpos");
                            LLVMValueRef bpos64 = LLVMBuildSExt(ctx->builder, bpos,
                                LLVMInt64TypeInContext(ctx->context), "bpos64");
                            LLVMValueRef bgep = LLVMBuildGEP2(ctx->builder,
                                LLVMInt8TypeInContext(ctx->context), bts_buf, &bpos64, 1, "bgep");
                            // char = '0' + bit
                            LLVMValueRef bbit1_8 = LLVMBuildTrunc(ctx->builder, bbit1,
                                LLVMInt8TypeInContext(ctx->context), "bbit1_8");
                            LLVMValueRef bchar = LLVMBuildAdd(ctx->builder, bbit1_8,
                                LLVMConstInt(LLVMInt8TypeInContext(ctx->context), '0', 0), "bchar");
                            LLVMBuildStore(ctx->builder, bchar, bgep);
                            LLVMValueRef bnewpos = LLVMBuildAdd(ctx->builder, bpos,
                                LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 1, 0), "bnewpos");
                            LLVMBuildStore(ctx->builder, bnewpos, pos_ptr);
                            LLVMBuildBr(ctx->builder, bsb);

                            LLVMPositionBuilderAtEnd(ctx->builder, bsb);
                            LLVMValueRef biv3 = LLVMBuildLoad2(ctx->builder,
                                LLVMInt32TypeInContext(ctx->context), bidx_ptr, "biv3");
                            LLVMValueRef bnidx = LLVMBuildSub(ctx->builder, biv3,
                                LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 1, 0), "bnidx");
                            LLVMBuildStore(ctx->builder, bnidx, bidx_ptr);
                            LLVMBuildBr(ctx->builder, blc);

                            LLVMPositionBuilderAtEnd(ctx->builder, ble);
                            // if never started, write '0'
                            LLVMValueRef bsf = LLVMBuildLoad2(ctx->builder,
                                LLVMInt32TypeInContext(ctx->context), bstarted_ptr, "bsf");
                            LLVMValueRef bns = LLVMBuildICmp(ctx->builder, LLVMIntEQ, bsf,
                                LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0), "bns");
                            LLVMBasicBlockRef bzb = LLVMAppendBasicBlockInContext(ctx->context, bin_to_str_fn, "bzb");
                            LLVMBasicBlockRef bnb = LLVMAppendBasicBlockInContext(ctx->context, bin_to_str_fn, "bnb");
                            LLVMBuildCondBr(ctx->builder, bns, bzb, bnb);

                            LLVMPositionBuilderAtEnd(ctx->builder, bzb);
                            LLVMValueRef bzpos = LLVMBuildLoad2(ctx->builder,
                                LLVMInt32TypeInContext(ctx->context), pos_ptr, "bzpos");
                            LLVMValueRef bzpos64 = LLVMBuildSExt(ctx->builder, bzpos,
                                LLVMInt64TypeInContext(ctx->context), "bzpos64");
                            LLVMValueRef bzgep = LLVMBuildGEP2(ctx->builder,
                                LLVMInt8TypeInContext(ctx->context), bts_buf, &bzpos64, 1, "bzgep");
                            LLVMBuildStore(ctx->builder,
                                LLVMConstInt(LLVMInt8TypeInContext(ctx->context), '0', 0), bzgep);
                            LLVMValueRef bznp = LLVMBuildAdd(ctx->builder, bzpos,
                                LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 1, 0), "bznp");
                            LLVMBuildStore(ctx->builder, bznp, pos_ptr);
                            LLVMBuildBr(ctx->builder, bnb);

                            LLVMPositionBuilderAtEnd(ctx->builder, bnb);
                            // null terminate
                            LLVMValueRef bfpos = LLVMBuildLoad2(ctx->builder,
                                LLVMInt32TypeInContext(ctx->context), pos_ptr, "bfpos");
                            LLVMValueRef bfpos64 = LLVMBuildSExt(ctx->builder, bfpos,
                                LLVMInt64TypeInContext(ctx->context), "bfpos64");
                            LLVMValueRef bfgep = LLVMBuildGEP2(ctx->builder,
                                LLVMInt8TypeInContext(ctx->context), bts_buf, &bfpos64, 1, "bfgep");
                            LLVMBuildStore(ctx->builder,
                                LLVMConstInt(LLVMInt8TypeInContext(ctx->context), 0, 0), bfgep);
                            LLVMBuildRetVoid(ctx->builder);

                            LLVMPositionBuilderAtEnd(ctx->builder, saved2);
                        }
                        LLVMValueRef bts_args[] = { arg_result.value, buf };
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(bin_to_str_fn),
                                       bin_to_str_fn, bts_args, 2, "");
                    } else if (at->kind == TYPE_OCT) {
                        fmt_s = LLVMBuildGlobalStringPtr(ctx->builder, "0o%lo", "fmt_soct");
                        LLVMValueRef sargs[] = { buf, fmt_s, val_to_print };
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(snprintf_fn),
                                       snprintf_fn, sargs, 3, "");
                    } else if (at->kind == TYPE_INT || at->kind == TYPE_BIN) {
                        fmt_s = LLVMBuildGlobalStringPtr(ctx->builder, "%ld", "fmt_sld");
                        LLVMValueRef sargs[] = { buf, fmt_s, val_to_print };
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(snprintf_fn),
                                       snprintf_fn, sargs, 3, "");
                    } else {
                        fmt_s = LLVMBuildGlobalStringPtr(ctx->builder, "%ld", "fmt_sld");
                        LLVMValueRef sargs[] = { buf, fmt_s, val_to_print };
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(snprintf_fn),
                                       snprintf_fn, sargs, 3, "");
                    }

                    result.type  = type_string();
                    result.value = buf;
                    return result;
                }

                // For all other casts: codegen the argument first
                CodegenResult arg_result = codegen_expr(ctx, arg);
                Type *at = arg_result.type;
                LLVMValueRef av = arg_result.value;

                // Helper: get i64 value regardless of integer subtype
                // (all integer subtypes are already i64 in LLVM)
                LLVMValueRef as_i64 = NULL;
                LLVMValueRef as_double = NULL;

                if (at->kind == TYPE_FLOAT) {
                    as_double = av;
                    as_i64 = LLVMBuildFPToSI(ctx->builder, av,
                        LLVMInt64TypeInContext(ctx->context), "to_i64");
                } else if (at->kind == TYPE_CHAR) {
                    as_i64 = LLVMBuildSExt(ctx->builder, av,
                        LLVMInt64TypeInContext(ctx->context), "to_i64");
                    as_double = LLVMBuildSIToFP(ctx->builder, as_i64,
                        LLVMDoubleTypeInContext(ctx->context), "to_double");
                } else if (at->kind == TYPE_INT || at->kind == TYPE_HEX ||
                           at->kind == TYPE_BIN || at->kind == TYPE_OCT) {
                    as_i64 = av; // already i64
                    as_double = LLVMBuildSIToFP(ctx->builder, av,
                        LLVMDoubleTypeInContext(ctx->context), "to_double");
                } else if (at->kind == TYPE_STRING) {
                    // hash the string pointer value (just use pointer as integer)
                    as_i64 = LLVMBuildPtrToInt(ctx->builder, av,
                        LLVMInt64TypeInContext(ctx->context), "str_hash");
                    as_double = LLVMBuildSIToFP(ctx->builder, as_i64,
                        LLVMDoubleTypeInContext(ctx->context), "to_double");
                } else {
                    // function or unknown: FNV-1a of symbol name at compile time
                    if (arg->type == AST_SYMBOL) {
                        uint64_t h = fnv1a_hash(arg->symbol);
                        as_i64 = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), h, 0);
                    } else {
                        as_i64 = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                    }
                    as_double = LLVMBuildSIToFP(ctx->builder, as_i64,
                        LLVMDoubleTypeInContext(ctx->context), "to_double");
                }

                if (strcmp(cast_target, "Ratio") == 0) {
                    if (ast->list.count != 2) {
                        fprintf(stderr, "%s:%d:%d: error: Ratio is not 2 items\n",
                                parser_get_filename(), ast->line, ast->column);
                    }

                    CodegenResult arg_result = codegen_expr(ctx, ast->list.items[1]);

                    // Convert to ratio
                    if (arg_result.type->kind == TYPE_RATIO) {
                        // Already a ratio
                        result = arg_result;
                    } else if (type_is_integer(arg_result.type)) {
                        // Integer to ratio: n/1
                        LLVMValueRef fn = get_rt_value_ratio(ctx);
                        LLVMValueRef one = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 1, 0);
                        LLVMValueRef args[] = {arg_result.value, one};
                        result.type = type_ratio();
                        result.value = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                                      fn, args, 2, "ratio");
                    } else {
                        fprintf(stderr, "%s:%d:%d: error: Cannot convert to Ratio\n",
                                parser_get_filename(), ast->line, ast->column);
                    }

                    return result;
                }

                if (strcmp(cast_target, "Int") == 0) {
                    result.type  = type_int();
                    result.value = as_i64;
                } else if (strcmp(cast_target, "Float") == 0) {
                    result.type  = type_float();
                    result.value = as_double ? as_double
                                             : LLVMBuildSIToFP(ctx->builder, as_i64,
                                                 LLVMDoubleTypeInContext(ctx->context), "to_f");
                } else if (strcmp(cast_target, "Char") == 0) {
                    result.type  = type_char();
                    result.value = LLVMBuildTrunc(ctx->builder, as_i64,
                        LLVMInt8TypeInContext(ctx->context), "to_char");
                } else if (strcmp(cast_target, "Hex") == 0) {
                    result.type  = type_hex();
                    result.value = as_i64; // same bits, different display type
                } else if (strcmp(cast_target, "Bin") == 0) {
                    result.type  = type_bin();
                    result.value = as_i64;
                } else if (strcmp(cast_target, "Oct") == 0) {
                    result.type  = type_oct();
                    result.value = as_i64;
                }

                return result;
            }

            // Layout constructor: (Point 3.0 4.0) or (Point) for zero-init
            {
                Type *lay = env_lookup_layout(ctx->env, head->symbol);
                if (lay && lay->kind == TYPE_LAYOUT) {

                    // Get the bare struct type (NOT the pointer wrapper from type_to_llvm)
                    char sname[256];
                    snprintf(sname, sizeof(sname), "layout.%s", lay->layout_name);
                    LLVMTypeRef struct_llvm = LLVMGetTypeByName2(ctx->context, sname);
                    if (!struct_llvm) {
                        // Force creation and retrieve bare type
                        type_to_llvm(ctx, lay);
                        struct_llvm = LLVMGetTypeByName2(ctx->context, sname);
                    }

                    // Allocate with malloc so the struct survives across REPL modules
                    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

                    LLVMValueRef malloc_fn = LLVMGetNamedFunction(ctx->module, "malloc");
                    if (!malloc_fn) {
                        LLVMTypeRef ft = LLVMFunctionType(ptr, &i64, 1, 0);
                        malloc_fn = LLVMAddFunction(ctx->module, "malloc", ft);
                        LLVMSetLinkage(malloc_fn, LLVMExternalLinkage);
                    }

                    LLVMValueRef size     = LLVMSizeOf(struct_llvm);
                    LLVMValueRef heap_ptr = LLVMBuildCall2(ctx->builder,
                                                           LLVMFunctionType(ptr, &i64, 1, 0),
                                                           malloc_fn, &size, 1, "lay_ptr");

                    // Initialize fields
                    int arg_count = (int)ast->list.count - 1;

                    if (arg_count == 0) {
                        // (Point) — zero initialize via memset
                        LLVMValueRef memset_fn = LLVMGetNamedFunction(ctx->module, "memset");
                        if (!memset_fn) {
                            LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
                            LLVMTypeRef ms_params[] = {ptr, i32, i64};
                            LLVMTypeRef ms_ft = LLVMFunctionType(ptr, ms_params, 3, 0);
                            memset_fn = LLVMAddFunction(ctx->module, "memset", ms_ft);
                            LLVMSetLinkage(memset_fn, LLVMExternalLinkage);
                        }
                        LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
                        LLVMValueRef ms_args[] = {
                            heap_ptr,
                            LLVMConstInt(i32, 0, 0),
                            size
                        };
                        LLVMBuildCall2(ctx->builder,
                                       LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr, i32, i64}, 3, 0),
                                       memset_fn, ms_args, 3, "");

                    } else if (arg_count == lay->layout_field_count) {
                        // Positional: (Point 3.0 4.0)
                        for (int i = 0; i < arg_count; i++) {
                            CodegenResult fv = codegen_expr(ctx, ast->list.items[i + 1]);
                            LLVMValueRef zero    = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                            LLVMValueRef fidx    = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), i, 0);
                            LLVMValueRef indices[] = {zero, fidx};
                            LLVMValueRef gep     = LLVMBuildGEP2(ctx->builder, struct_llvm,
                                                                 heap_ptr, indices, 2, "fptr");
                            Type        *ft      = lay->layout_fields[i].type;
                            LLVMValueRef stored  = fv.value;
                            if (type_is_integer(ft) && type_is_float(fv.type))
                                stored = LLVMBuildFPToSI(ctx->builder, fv.value, type_to_llvm(ctx, ft), "conv");
                            else if (type_is_float(ft) && type_is_integer(fv.type))
                                stored = LLVMBuildSIToFP(ctx->builder, fv.value, type_to_llvm(ctx, ft), "conv");
                            LLVMBuildStore(ctx->builder, stored, gep);
                        }

                    } else {
                        // Keyword: (Point :x 3.0 :y 4.0) — zero-init first, then fill named fields
                        LLVMValueRef memset_fn = LLVMGetNamedFunction(ctx->module, "memset");
                        if (!memset_fn) {
                            LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
                            LLVMTypeRef ms_params[] = {ptr, i32, i64};
                            LLVMTypeRef ms_ft = LLVMFunctionType(ptr, ms_params, 3, 0);
                            memset_fn = LLVMAddFunction(ctx->module, "memset", ms_ft);
                            LLVMSetLinkage(memset_fn, LLVMExternalLinkage);
                        }
                        LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
                        LLVMValueRef ms_args[] = {heap_ptr, LLVMConstInt(i32, 0, 0), size};
                        LLVMBuildCall2(ctx->builder,
                                       LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr, i32, i64}, 3, 0),
                                       memset_fn, ms_args, 3, "");

                        int i = 1;
                        while (i < (int)ast->list.count) {
                            AST *kw = ast->list.items[i];
                            if (kw->type != AST_KEYWORD) {
                                CODEGEN_ERROR(ctx, "%s:%d:%d: error: expected keyword argument in '%s' constructor",
                                              parser_get_filename(), kw->line, kw->column, head->symbol);
                            }
                            if (i + 1 >= (int)ast->list.count) {
                                CODEGEN_ERROR(ctx, "%s:%d:%d: error: missing value for keyword :%s",
                                              parser_get_filename(), kw->line, kw->column, kw->keyword);
                            }
                            int field_idx = -1;
                            for (int j = 0; j < lay->layout_field_count; j++) {
                                if (strcmp(lay->layout_fields[j].name, kw->keyword) == 0) {
                                    field_idx = j;
                                    break;
                                }
                            }
                            if (field_idx < 0) {
                                CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown field ':%s' in layout '%s'",
                                              parser_get_filename(), kw->line, kw->column,
                                              kw->keyword, head->symbol);
                            }
                            CodegenResult fv     = codegen_expr(ctx, ast->list.items[i + 1]);
                            LLVMValueRef zero    = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                            LLVMValueRef fidx    = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), field_idx, 0);
                            LLVMValueRef indices[] = {zero, fidx};
                            LLVMValueRef gep     = LLVMBuildGEP2(ctx->builder, struct_llvm,
                                                                 heap_ptr, indices, 2, "fptr");
                            Type        *ft      = lay->layout_fields[field_idx].type;
                            LLVMValueRef stored  = fv.value;
                            if (type_is_integer(ft) && type_is_float(fv.type))
                                stored = LLVMBuildFPToSI(ctx->builder, fv.value, type_to_llvm(ctx, ft), "conv");
                            else if (type_is_float(ft) && type_is_integer(fv.type))
                                stored = LLVMBuildSIToFP(ctx->builder, fv.value, type_to_llvm(ctx, ft), "conv");
                            LLVMBuildStore(ctx->builder, stored, gep);
                            i += 2;
                        }
                    }

                    // Return the heap pointer — env_insert (in the define path) will
                    // store this into a global so it survives across REPL modules.
                    result.value = heap_ptr;
                    result.type  = type_clone(lay);
                    return result;
                }
            }

            // Dot access: p.x
            {
                const char *dot = strchr(ast->symbol, '.');
                if (dot && dot != ast->symbol) {
                    char var_name[256];
                    size_t vlen = dot - ast->symbol;
                    if (vlen >= sizeof(var_name)) vlen = sizeof(var_name) - 1;
                    memcpy(var_name, ast->symbol, vlen);
                    var_name[vlen] = '\0';
                    const char *field_name = dot + 1;

                    EnvEntry *base_entry = env_lookup(ctx->env, var_name);
                    if (base_entry && base_entry->type && base_entry->type->kind == TYPE_LAYOUT) {
                        Type *lay = base_entry->type;

                        int field_idx = -1;
                        for (int i = 0; i < lay->layout_field_count; i++) {
                            if (strcmp(lay->layout_fields[i].name, field_name) == 0) {
                                field_idx = i;
                                break;
                            }
                        }
                        if (field_idx < 0) {
                            CODEGEN_ERROR(ctx, "%s:%d:%d: error: layout '%s' has no field '%s'",
                                          parser_get_filename(), ast->line, ast->column,
                                          lay->layout_name, field_name);
                        }

                        // Get bare struct type
                        char sname[256];
                        snprintf(sname, sizeof(sname), "layout.%s", lay->layout_name);
                        LLVMTypeRef struct_llvm = LLVMGetTypeByName2(ctx->context, sname);
                        if (!struct_llvm) {
                            CODEGEN_ERROR(ctx, "%s:%d:%d: error: LLVM struct type for '%s' not found",
                                          parser_get_filename(), ast->line, ast->column, lay->layout_name);
                        }


                        LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                        LLVMValueRef get_fn = LLVMGetNamedFunction(ctx->module, "__layout_ptr_get");
                        if (!get_fn) {
                            LLVMTypeRef ft = LLVMFunctionType(ptr_t, &ptr_t, 1, 0);
                            get_fn = LLVMAddFunction(ctx->module, "__layout_ptr_get", ft);
                            LLVMSetLinkage(get_fn, LLVMExternalLinkage);
                        }
                        LLVMValueRef name_str = LLVMBuildGlobalStringPtr(ctx->builder, var_name, "lay_name");
                        LLVMValueRef heap_ptr = LLVMBuildCall2(ctx->builder,
                                                               LLVMFunctionType(ptr_t, &ptr_t, 1, 0),
                                                               get_fn, &name_str, 1, "lay_ptr");


                        LLVMValueRef zero      = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                        LLVMValueRef fidx      = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), field_idx, 0);
                        LLVMValueRef indices[] = {zero, fidx};
                        LLVMValueRef gep       = LLVMBuildGEP2(ctx->builder, struct_llvm,
                                                               heap_ptr, indices, 2, "fld_ptr");
                        Type        *ft        = lay->layout_fields[field_idx].type;
                        result.value = LLVMBuildLoad2(ctx->builder, type_to_llvm(ctx, ft), gep, field_name);
                        result.type  = type_clone(ft);
                        return result;
                    }

                    if (base_entry) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' is not a layout type",
                                      parser_get_filename(), ast->line, ast->column, var_name);
                    }
                    if (!ctx->module_ctx) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: unbound variable: %s",
                                      parser_get_filename(), ast->line, ast->column, var_name);
                    }
                }
            }

            CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown function: %s",
                    parser_get_filename(), ast->line, ast->column, head->symbol);
        }

        // Handle ((lambda ...) args...) — immediately invoked lambda (let desugaring)
        if (head->type == AST_LAMBDA) {
            static int anon_count = 0;
            char anon_name[64];
            snprintf(anon_name, sizeof(anon_name), "__anon_%d", anon_count++);

            // Codegen args first to infer types
            int arg_count = ast->list.count - 1;
            CodegenResult *arg_results = malloc(sizeof(CodegenResult) * (arg_count ? arg_count : 1));
            for (int i = 0; i < arg_count; i++) {
                arg_results[i] = codegen_expr(ctx, ast->list.items[i + 1]);
                // Patch lambda param type from inferred arg type
                if (i < head->lambda.param_count &&
                    head->lambda.params[i].type_name == NULL &&
                    arg_results[i].type) {
                    head->lambda.params[i].type_name = strdup(type_to_string(arg_results[i].type));
                }
            }

            // Also infer return type from body if not annotated
            if (head->lambda.return_type == NULL) {
                Env *tmp_env = env_create_child(ctx->env);
                Env *saved_env = ctx->env;
                ctx->env = tmp_env;
                for (int i = 0; i < arg_count && i < head->lambda.param_count; i++) {
                    LLVMTypeRef pt = type_to_llvm(ctx, arg_results[i].type);
                    LLVMValueRef tmp_alloca = LLVMBuildAlloca(ctx->builder, pt,
                                                              head->lambda.params[i].name);
                    LLVMBuildStore(ctx->builder, arg_results[i].value, tmp_alloca);
                    env_insert(ctx->env, head->lambda.params[i].name,
                               type_clone(arg_results[i].type), tmp_alloca);
                }
                CodegenResult peek = {NULL, NULL};
                for (int i = 0; i < head->lambda.body_count; i++)
                    peek = codegen_expr(ctx, head->lambda.body_exprs[i]);
                if (peek.type)
                    head->lambda.return_type = strdup(type_to_string(peek.type));
                ctx->env = saved_env;
                env_free(tmp_env);
            }

            AST *name_node = ast_new_symbol(anon_name);
            AST *define_node = ast_new_list();
            ast_list_append(define_node, ast_new_symbol("define"));
            ast_list_append(define_node, name_node);
            ast_list_append(define_node, head);

            codegen_expr(ctx, define_node);

            EnvEntry *entry = env_lookup(ctx->env, anon_name);
            if (!entry) {
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: failed to codegen anonymous lambda",
                              parser_get_filename(), ast->line, ast->column);
            }

            LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (arg_count ? arg_count : 1));
            for (int i = 0; i < arg_count; i++)
                args[i] = arg_results[i].value;

            LLVMTypeRef *param_llvm_types = malloc(sizeof(LLVMTypeRef) * (arg_count ? arg_count : 1));
            for (int i = 0; i < arg_count; i++)
                param_llvm_types[i] = type_to_llvm(ctx, entry->params[i].type);
            LLVMTypeRef call_ft = LLVMFunctionType(
                type_to_llvm(ctx, entry->return_type),
                param_llvm_types, arg_count, 0);
            free(param_llvm_types);

            result.value = LLVMBuildCall2(ctx->builder, call_ft,
                                          entry->func_ref, args, arg_count, "let_call");
            result.type  = type_clone(entry->return_type);
            free(args);
            free(arg_results);
            return result;
        }


        CODEGEN_ERROR(ctx, "%s:%d:%d: error: function call requires symbol in head position",
                parser_get_filename(), ast->line, ast->column);
    }

    case AST_TYPE_ALIAS: {
        const char *alias_name = ast->type_alias.alias_name;
        if (!alias_name || !isupper((unsigned char)alias_name[0])) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: type alias '%s' must start with an uppercase letter",
                          parser_get_filename(), ast->line, ast->column,
                          alias_name ? alias_name : "");
        }
        type_alias_register(alias_name, ast->type_alias.target_name);
        printf("Type alias: %s = %s\n", alias_name, ast->type_alias.target_name);
        result.type  = NULL;
        result.value = NULL;
        return result;
    }

    case AST_TESTS: {
        // Only emit tests if test mode is enabled
        if (!ctx->test_mode) {
            result.type  = type_int();
            result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
            return result;
        }

        LLVMValueRef printf_fn = get_or_declare_printf(ctx);

        // Print module test header
        const char *mod_name = (ctx->module_ctx && ctx->module_ctx->decl)
            ? ctx->module_ctx->decl->name : "?";

        char header[256];
        snprintf(header, sizeof(header), "Testing %s:\n", mod_name);
        LLVMValueRef hdr = LLVMBuildGlobalStringPtr(ctx->builder, header, "test_hdr");
        LLVMValueRef hdr_args[] = {hdr};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                       printf_fn, hdr_args, 1, "");

        // Codegen each assertion
        for (int i = 0; i < ast->tests.count; i++) {
            codegen_expr(ctx, ast->tests.assertions[i]);
        }

        result.type  = type_int();
        result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
        return result;
    }

    case AST_LAYOUT: {
        codegen_layout(ctx, ast);
        result.type  = type_int();
        result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
        return result;
    }

    default:
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown AST type: %d",
                parser_get_filename(), ast->line, ast->column, ast->type);
    }
}

/// Module

static CodegenResult codegen_define_variable(CodegenContext *ctx,
                                              const char *var_name,
                                              Type *final_type,
                                              LLVMValueRef stored_value,
                                              int source_line,
                                              int source_column) {
    CodegenResult result = {NULL, NULL};
    bool top_level = is_at_top_level(ctx);

    LLVMTypeRef llvm_type = type_to_llvm(ctx, final_type);
    LLVMValueRef var;

    if (top_level) {
        // Module-level definition: use an LLVM global variable.
        // We give it the local name now; main.c Phase 9 will rename it to
        // the mangled name and replace all uses.
        var = LLVMGetNamedGlobal(ctx->module, var_name);
        if (!var) {
            var = LLVMAddGlobal(ctx->module, llvm_type, var_name);
            LLVMSetInitializer(var, LLVMConstNull(llvm_type));
            // External linkage so the linker can resolve it from other modules.
            LLVMSetLinkage(var, LLVMExternalLinkage);
        }
        // Emit a store from within the init function
        LLVMBuildStore(ctx->builder, stored_value, var);
    } else {
        // Inside a function: keep the stack alloca
        var = LLVMBuildAlloca(ctx->builder, llvm_type, var_name);
        LLVMBuildStore(ctx->builder, stored_value, var);
    }

    env_insert(ctx->env, var_name, final_type, var);

    if (ctx->module_ctx && !should_export_symbol(ctx->module_ctx, var_name)) {
        printf("Defined %s :: %s (private)\n", var_name, type_to_string(final_type));
    } else {
        printf("Defined %s :: %s\n", var_name, type_to_string(final_type));
    }

    result.type  = final_type;
    result.value = stored_value;
    return result;
}

static CodegenResult codegen_define_array_zeroinit(CodegenContext *ctx,
                                                    const char *var_name,
                                                    Type *final_type) {
    CodegenResult result = {NULL, NULL};
    bool top_level = is_at_top_level(ctx);
    LLVMTypeRef arr_type = type_to_llvm(ctx, final_type);

    LLVMValueRef arr;
    if (top_level) {
        arr = LLVMGetNamedGlobal(ctx->module, var_name);
        if (!arr) {
            arr = LLVMAddGlobal(ctx->module, arr_type, var_name);
            // LLVMConstNull gives a zero-initialized aggregate
            LLVMSetInitializer(arr, LLVMConstNull(arr_type));
            LLVMSetLinkage(arr, LLVMExternalLinkage);
        }
        // No explicit store needed — the global is zero-initialized by default.
        // But we still emit element-wise stores in case of non-zero init (handled
        // in the caller; for zero-init we just use ConstNull above).
    } else {
        arr = LLVMBuildAlloca(ctx->builder, arr_type, var_name);

        // Zero-initialize each element explicitly (for stack arrays)
        LLVMTypeRef elem_type_llvm = type_to_llvm(ctx, final_type->arr_element_type);
        LLVMValueRef zero_val;
        if (type_is_float(final_type->arr_element_type)) {
            zero_val = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
        } else if (final_type->arr_element_type->kind == TYPE_STRING  ||
                   final_type->arr_element_type->kind == TYPE_KEYWORD ||
                   final_type->arr_element_type->kind == TYPE_RATIO) {
            zero_val = LLVMConstPointerNull(elem_type_llvm);
        } else {
            zero_val = LLVMConstInt(elem_type_llvm, 0, 0);
        }
        for (int idx = 0; idx < final_type->arr_size; idx++) {
            LLVMValueRef zero_i = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
            LLVMValueRef elem_i = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), idx, 0);
            LLVMValueRef indices[] = {zero_i, elem_i};
            LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder, arr_type, arr,
                                                   indices, 2, "elem_ptr");
            LLVMBuildStore(ctx->builder, zero_val, elem_ptr);
        }
    }

    env_insert(ctx->env, var_name, final_type, arr);
    printf("Defined %s :: %s (zero-initialized)\n", var_name, type_to_string(final_type));

    result.type  = final_type;
    result.value = arr;
    return result;
}

void codegen_declare_external_var(CodegenContext *ctx,
                                  const char *mangled_name,
                                  Type *type) {
    if (LLVMGetNamedGlobal(ctx->module, mangled_name)) return;
    LLVMTypeRef lt = type_to_llvm(ctx, type);
    LLVMValueRef gv = LLVMAddGlobal(ctx->module, lt, mangled_name);
    LLVMSetLinkage(gv, LLVMExternalLinkage);
    // No LLVMSetInitializer — this is an extern declaration only
}

void codegen_declare_external_func(CodegenContext *ctx,
                                   const char *mangled_name,
                                   EnvParam *params, int param_count,
                                   Type *return_type) {
    if (LLVMGetNamedFunction(ctx->module, mangled_name)) return;
    LLVMTypeRef *ptypes = malloc(sizeof(LLVMTypeRef) * (param_count + 1));
    for (int i = 0; i < param_count; i++)
        ptypes[i] = type_to_llvm(ctx, params[i].type);
    LLVMTypeRef ft = LLVMFunctionType(type_to_llvm(ctx, return_type),
                                      ptypes, param_count, 0);
    LLVMValueRef fn = LLVMAddFunction(ctx->module, mangled_name, ft);
    LLVMSetLinkage(fn, LLVMExternalLinkage);
    free(ptypes);
}

void register_builtins(CodegenContext *ctx) {
    env_insert_builtin(ctx->env, "nil?", 1, 1, "Return True if value is nil (null pointer)");
    // Arithmetic operators
    env_insert_builtin(ctx->env, "+",  1, -1, "Add numbers");
    env_insert_builtin(ctx->env, "-",  1, -1, "Subtract or negate numbers");
    env_insert_builtin(ctx->env, "*",  1, -1, "Multiply numbers");
    env_insert_builtin(ctx->env, "/",  1, -1, "Divide numbers");
    env_insert_builtin(ctx->env, "%",  2,  0, "Modulo operation");
    env_insert_builtin(ctx->env, "**", 2,  0, "Exponentiation");

    // Comparison operators
    env_insert_builtin(ctx->env, "=",  2, -1, "Test equality");
    env_insert_builtin(ctx->env, "!=", 2, -1, "Test inequality");
    env_insert_builtin(ctx->env, "<",  2, -1, "Less than");
    env_insert_builtin(ctx->env, "<=", 2, -1, "Less than or equal");
    env_insert_builtin(ctx->env, ">",  2, -1, "Greater than");
    env_insert_builtin(ctx->env, ">=", 2, -1, "Greater than or equal");

    env_insert_builtin(ctx->env, "code", 1, 0, "Return the source AST of a defined function");

    // List operations
    env_insert_builtin(ctx->env, "list",    0, -1, "Create a list from arguments");
    env_insert_builtin(ctx->env, "cons",    2,  0, "Cons an element onto a list");
    env_insert_builtin(ctx->env, "car",     1,  0, "Get first element of list");
    env_insert_builtin(ctx->env, "cdr",     1,  0, "Get rest of list");
    env_insert_builtin(ctx->env, "first",   1,  0, "Get first element (alias for car)");
    env_insert_builtin(ctx->env, "rest",    1,  0, "Get rest of list (alias for cdr)");
    env_insert_builtin(ctx->env, "length",  1,  0, "Get length of list or string");
    env_insert_builtin(ctx->env, "append",  2, -1, "Concatenate lists");
    env_insert_builtin(ctx->env, "reverse", 1,  0, "Reverse a list");
    env_insert_builtin(ctx->env, "nth",     2,  0, "Get nth element of list (0-indexed)");
    env_insert_builtin(ctx->env, "map",     2,  0, "Map function over list");
    env_insert_builtin(ctx->env, "filter",  2,  0, "Filter list by predicate");
    env_insert_builtin(ctx->env, "reduce",  3,  0, "Reduce list with function and initial value");
    env_insert_builtin(ctx->env, "empty?",  1,  0, "Test if list is empty");

    // Logic operators
    env_insert_builtin(ctx->env, "and", 2, -1, "Logical AND (short-circuit)");
    env_insert_builtin(ctx->env, "or",  2, -1, "Logical OR (short-circuit)");
    env_insert_builtin(ctx->env, "not", 1,  0, "Logical NOT");

    // Control flow
    env_insert_builtin(ctx->env, "if",     3,  0, "Conditional: (if cond then else)");
    env_insert_builtin(ctx->env, "when",   2, -1, "Execute when condition is true");
    env_insert_builtin(ctx->env, "unless", 2, -1, "Execute unless condition is true");
    env_insert_builtin(ctx->env, "cond",   1, -1, "Multi-branch conditional");

    // Special forms (even though handled specially, should be in environment)
    env_insert_builtin(ctx->env, "define", 2,  0, "Define a variable or function");
    env_insert_builtin(ctx->env, "lambda", 2, -1, "Create anonymous function");
    env_insert_builtin(ctx->env, "quote",  1,  0, "Quote expression without evaluation");
    env_insert_builtin(ctx->env, "show",   1,  0, "Print a value to stdout");

    // Type predicates
    env_insert_builtin(ctx->env, "int?",      1, 0, "Test if value is an integer");
    env_insert_builtin(ctx->env, "float?",    1, 0, "Test if value is a float");
    env_insert_builtin(ctx->env, "char?",     1, 0, "Test if value is a character");
    env_insert_builtin(ctx->env, "string?",   1, 0, "Test if value is a string");
    env_insert_builtin(ctx->env, "list?",     1, 0, "Test if value is a list");
    env_insert_builtin(ctx->env, "keyword?",  1, 0, "Test if value is a keyword");
    env_insert_builtin(ctx->env, "nil?",      1, 0, "Test if value is nil/empty");
    env_insert_builtin(ctx->env, "number?",   1, 0, "Test if value is a number");
    env_insert_builtin(ctx->env, "function?", 1, 0, "Test if value is a function");

    // Type conversions (already implemented but add to env)
    env_insert_builtin(ctx->env, "Int",    1, 0, "Convert value to integer");
    env_insert_builtin(ctx->env, "Float",  1, 0, "Convert value to float");
    env_insert_builtin(ctx->env, "Char",   1, 0, "Convert value to character");
    env_insert_builtin(ctx->env, "String", 1, 0, "Convert value to string");
    env_insert_builtin(ctx->env, "Hex",    1, 0, "Convert to hexadecimal integer");
    env_insert_builtin(ctx->env, "Bin",    1, 0, "Convert to binary integer");
    env_insert_builtin(ctx->env, "Oct",    1, 0, "Convert to octal integer");

    // String operations
    env_insert_builtin(ctx->env, "concat",        2, -1, "Concatenate strings");
    env_insert_builtin(ctx->env, "substring",     3,  0, "Get substring (str start end)");
    env_insert_builtin(ctx->env, "string-length", 1,  0, "Get string length");

    // Math functions
    env_insert_builtin(ctx->env, "abs",   1,  0, "Absolute value");
    env_insert_builtin(ctx->env, "sqrt",  1,  0, "Square root");
    env_insert_builtin(ctx->env, "pow",   2,  0, "Power function");
    env_insert_builtin(ctx->env, "sin",   1,  0, "Sine");
    env_insert_builtin(ctx->env, "cos",   1,  0, "Cosine");
    env_insert_builtin(ctx->env, "tan",   1,  0, "Tangent");
    env_insert_builtin(ctx->env, "floor", 1,  0, "Floor function");
    env_insert_builtin(ctx->env, "ceil",  1,  0, "Ceiling function");
    env_insert_builtin(ctx->env, "round", 1,  0, "Round to nearest integer");
    env_insert_builtin(ctx->env, "min",   2, -1, "Minimum value");
    env_insert_builtin(ctx->env, "max",   2, -1, "Maximum value");

    env_insert_builtin(ctx->env, "take", 2, 0, "Take n elements from a (possibly infinite) list");
    env_insert_builtin(ctx->env, "drop", 2, 0, "Drop n elements from a list");
}
