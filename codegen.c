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
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/TargetMachine.h>

void codegen_init(CodegenContext *ctx, const char *module_name) {
    ctx->context = LLVMContextCreate();
    ctx->module = LLVMModuleCreateWithNameInContext(module_name, ctx->context);
    ctx->builder = LLVMCreateBuilderInContext(ctx->context);
    ctx->env = env_create();
    // Initialize format strings to NULL - will be created lazily
    ctx->fmt_str   = NULL;
    ctx->fmt_char  = NULL;
    ctx->fmt_int   = NULL;
    ctx->fmt_float = NULL;
    ctx->fmt_hex   = NULL;
    ctx->fmt_bin   = NULL;
    ctx->fmt_oct   = NULL;
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


LLVMTypeRef type_to_llvm(CodegenContext *ctx, Type *type) {
    switch (type->kind) {
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
    case TYPE_KEYWORD:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    case TYPE_LIST:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0); // RuntimeList*
    case TYPE_RATIO:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0); // RuntimeValue* for Ratio
    case TYPE_ARR:
        // Arrays are stack-allocated - return array type
        if (type->arr_element_type && type->arr_size > 0) {
            LLVMTypeRef elem_type = type_to_llvm(ctx, type->arr_element_type);
            return LLVMArrayType(elem_type, type->arr_size);
        }
        // Fallback for unknown size
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    case TYPE_BOOL:
        return LLVMInt1TypeInContext(ctx->context);
    default:
        return LLVMDoubleTypeInContext(ctx->context);
    }
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
            // Quoted symbols become strings
            LLVMValueRef fn = get_rt_value_string(ctx);
            LLVMValueRef sym = LLVMBuildGlobalStringPtr(ctx->builder,
                                                        ast_elem->symbol, "sym");
            LLVMValueRef args[] = {sym};
            rt_val = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn),
                                   fn, args, 1, "rtval");
            break;
        }

        case AST_LIST: {
            // Recursively create a runtime list
            LLVMValueRef list_fn = get_rt_list_create(ctx);
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

CodegenResult codegen_expr(CodegenContext *ctx, AST *ast) {
    CodegenResult result = {NULL, NULL};

    switch (ast->type) {
    case AST_NUMBER: {
        // DEBUG
        fprintf(stderr, ">>> CODEGEN NUMBER: literal_str='%s', number=%g\n",
                ast->literal_str ? ast->literal_str : "NULL", ast->number);

        Type *num_type = infer_literal_type(ast->number, ast->literal_str);

        // DEBUG
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
        EnvEntry *entry = env_lookup(ctx->env, ast->symbol);
        if (!entry) {
            fprintf(stderr, "%s:%d:%d: error: unbound variable: %s\n",
                    parser_get_filename(), ast->line, ast->column, ast->symbol);
            exit(1);
        }
        result.type = entry->type;
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

        fprintf(stderr, "%s:%d:%d: error: inline asm can only be used as a function body\n",
                parser_get_filename(), ast->line, ast->column);
        exit(1);
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
    case AST_LIST: {
        if (ast->list.count == 0) {
            fprintf(stderr, "%s:%d:%d: error: empty list not supported\n",
                    parser_get_filename(), ast->line, ast->column);
            exit(1);
        }

        AST *head = ast->list.items[0];

        if (head->type == AST_SYMBOL) {
            // Handle 'define' special form
            if (strcmp(head->symbol, "define") == 0) {
                if (ast->list.count < 3) {
                    fprintf(stderr, "%s:%d:%d: error: 'define' requires at least 2 arguments\n",
                            parser_get_filename(), ast->line, ast->column);
                    exit(1);
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
                        fprintf(stderr, "%s:%d:%d: error: 'define' name must be symbol or type annotation\n",
                                parser_get_filename(), ast->line, ast->column);
                        exit(1);
                    }
                } else if (name_expr->type == AST_SYMBOL) {
                    var_name = name_expr->symbol;
                } else {
                    fprintf(stderr, "%s:%d:%d: error: 'define' name must be symbol or type annotation\n",
                            parser_get_filename(), ast->line, ast->column);
                    exit(1);
                }

                if (!var_name) {
                    fprintf(stderr, "%s:%d:%d: error: 'define' invalid name format\n",
                            parser_get_filename(), ast->line, ast->column);
                    exit(1);
                }

                // Check if value is a lambda - codegen it as a function
                if (value_expr->type == AST_LAMBDA) {
                    // Extract lambda info
                    AST *lambda = value_expr;

                    // Build parameter types
                    LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * lambda->lambda.param_count);
                    EnvParam *env_params = malloc(sizeof(EnvParam) * lambda->lambda.param_count);

                    for (int i = 0; i < lambda->lambda.param_count; i++) {
                        ASTParam *param = &lambda->lambda.params[i];

                        // Parse parameter type
                        Type *param_type = NULL;
                        if (param->type_name) {
                            if (strcmp(param->type_name, "Int")         == 0) param_type = type_int();
                            else if (strcmp(param->type_name, "Float")  == 0) param_type = type_float();
                            else if (strcmp(param->type_name, "Char")   == 0) param_type = type_char();
                            else if (strcmp(param->type_name, "String") == 0) param_type = type_string();
                            else if (strcmp(param->type_name, "Bool")   == 0) param_type = type_bool();
                            else if (strcmp(param->type_name, "Hex")    == 0) param_type = type_hex();
                            else if (strcmp(param->type_name, "Bin")    == 0) param_type = type_bin();
                            else if (strcmp(param->type_name, "Oct")    == 0) param_type = type_oct();
                            else {
                                fprintf(stderr, "%s:%d:%d: error: unknown type '%s'\n",
                                        parser_get_filename(), lambda->line, lambda->column, param->type_name);
                                exit(1);
                            }
                        } else {
                            // Default to polymorphic (float for now)
                            param_type = type_float();
                        }

                        param_types[i] = type_to_llvm(ctx, param_type);
                        env_params[i].name = strdup(param->name);
                        env_params[i].type = param_type;
                    }

                    // Determine return type
                    Type *ret_type = NULL;
                    if (lambda->lambda.return_type) {
                        if (strcmp(lambda->lambda.return_type, "Int")         == 0) ret_type = type_int();
                        else if (strcmp(lambda->lambda.return_type, "Float")  == 0) ret_type = type_float();
                        else if (strcmp(lambda->lambda.return_type, "Char")   == 0) ret_type = type_char();
                        else if (strcmp(lambda->lambda.return_type, "String") == 0) ret_type = type_string();
                        else if (strcmp(lambda->lambda.return_type, "Bool")   == 0) ret_type = type_bool();
                        else if (strcmp(lambda->lambda.return_type, "Hex")    == 0) ret_type = type_hex();
                        else if (strcmp(lambda->lambda.return_type, "Bin")    == 0) ret_type = type_bin();
                        else if (strcmp(lambda->lambda.return_type, "Oct")    == 0) ret_type = type_oct();

                        else {
                            fprintf(stderr, "%s:%d:%d: error: unknown return type '%s'\n",
                                    parser_get_filename(), lambda->line, lambda->column, lambda->lambda.return_type);
                            exit(1);
                        }
                    } else {
                        ret_type = type_float(); // default
                    }

                    LLVMTypeRef ret_llvm_type = type_to_llvm(ctx, ret_type);

                    // Create function type
                    LLVMTypeRef func_type = LLVMFunctionType(ret_llvm_type, param_types,
                                                             lambda->lambda.param_count, 0);

                    // Create function
                    LLVMValueRef func = LLVMAddFunction(ctx->module, var_name, func_type);

                    // Create entry block
                    LLVMBasicBlockRef entry_block = LLVMAppendBasicBlockInContext(ctx->context, func, "entry");

                    // Save current insert point
                    LLVMBasicBlockRef saved_block = LLVMGetInsertBlock(ctx->builder);

                    // Position at function entry
                    LLVMPositionBuilderAtEnd(ctx->builder, entry_block);

                    // Create a new scope for function parameters
                    Env *saved_env = ctx->env;
                    ctx->env = env_create();

                    // Add parameters to the function's environment
                    for (int i = 0; i < lambda->lambda.param_count; i++) {
                        LLVMValueRef param = LLVMGetParam(func, i);
                        LLVMSetValueName2(param, lambda->lambda.params[i].name,
                                         strlen(lambda->lambda.params[i].name));

                        // Create alloca for parameter and store it
                        LLVMValueRef param_alloca = LLVMBuildAlloca(ctx->builder,
                                                                    param_types[i],
                                                                    lambda->lambda.params[i].name);
                        LLVMBuildStore(ctx->builder, param, param_alloca);

                        env_insert(ctx->env, lambda->lambda.params[i].name,
                                   type_clone(env_params[i].type), param_alloca);
                    }

                    // Codegen function body
                    /* CodegenResult body_result = codegen_expr(ctx, lambda->lambda.body); */

                    // Codegen function body
                    CodegenResult body_result;

                    // Check if body is inline assembly
                    if (lambda->lambda.body->type == AST_ASM) {
                        // Preprocess ASM
                        RegisterAllocator reg_alloc;
                        reg_alloc_init(&reg_alloc);

                        AsmContext asm_ctx = {.params = env_params,
                            .param_count =
                            lambda->lambda.param_count,
                            .reg_alloc = &reg_alloc};

                        int asm_inst_count;
                        AsmInstruction *asm_instructions = preprocess_asm(
                            lambda->lambda.body, &asm_ctx, &asm_inst_count);

                        // Collect parameter values
                        LLVMValueRef *param_values = malloc(
                            sizeof(LLVMValueRef) * lambda->lambda.param_count);
                        for (int i = 0; i < lambda->lambda.param_count; i++) {
                            param_values[i] = LLVMGetParam(func, i);
                        }

                        // Generate inline asm
                        body_result.value = codegen_inline_asm(
                            ctx->context, ctx->builder, asm_instructions,
                            asm_inst_count, ret_type, param_values,
                            lambda->lambda.param_count);
                        body_result.type = ret_type;

                        free(param_values);
                        free_asm_instructions(asm_instructions, asm_inst_count);
                    } else {
                        // Normal codegen
                        body_result = codegen_expr(ctx, lambda->lambda.body);
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
                        }
                    }

                    LLVMBuildRet(ctx->builder, ret_value);

                    // Restore environment and insert point
                    env_free(ctx->env);
                    ctx->env = saved_env;

                    if (saved_block) {
                        LLVMPositionBuilderAtEnd(ctx->builder, saved_block);
                    }

                    // Insert function into symbol table
                    env_insert_func(ctx->env, var_name, env_params, lambda->lambda.param_count,
                                   ret_type, func, lambda->lambda.docstring);

                    printf("Defined %s :: Fn (", var_name);
                    for (int i = 0; i < lambda->lambda.param_count; i++) {
                        if (i > 0) printf(" ");
                        printf("%s", lambda->lambda.params[i].name);
                    }
                    printf(") -> %s\n", type_to_string(ret_type));

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
                        fprintf(stderr, "%s:%d:%d: error: cannot infer type of empty array - explicit type annotation required\n",
                                parser_get_filename(), ast->line, ast->column);
                        exit(1);
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
                        fprintf(stderr, "%s:%d:%d: error: array size mismatch - declared size %d but got %d elements\n",
                                parser_get_filename(), ast->line, ast->column,
                                declared_size, actual_size);
                        exit(1);
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
                        // Create zero-initialized array
                        LLVMTypeRef arr_type = type_to_llvm(ctx, final_type);
                        LLVMValueRef arr = LLVMBuildAlloca(ctx->builder, arr_type, var_name);

                        // Zero-initialize the array
                        LLVMTypeRef elem_type_llvm = type_to_llvm(ctx, final_type->arr_element_type);
                        LLVMValueRef zero_val;

                        if (type_is_float(final_type->arr_element_type)) {
                            zero_val = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                        } else if (final_type->arr_element_type->kind == TYPE_STRING ||
                                   final_type->arr_element_type->kind == TYPE_KEYWORD ||
                                   final_type->arr_element_type->kind == TYPE_RATIO) {
                            zero_val = LLVMConstPointerNull(elem_type_llvm);
                        } else {
                            zero_val = LLVMConstInt(elem_type_llvm, 0, 0);
                        }

                        for (int i = 0; i < final_type->arr_size; i++) {
                            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                            LLVMValueRef idx = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), i, 0);
                            LLVMValueRef indices[] = {zero, idx};
                            LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder, arr_type,
                                                                  arr, indices, 2, "elem_ptr");
                            LLVMBuildStore(ctx->builder, zero_val, elem_ptr);
                        }

                        env_insert(ctx->env, var_name, final_type, arr);
                        printf("Defined %s :: %s (zero-initialized)\n", var_name, type_to_string(final_type));

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

                // For non-array types, create an alloca and store normally
                LLVMTypeRef llvm_type = type_to_llvm(ctx, final_type);
                LLVMValueRef var = LLVMBuildAlloca(ctx->builder, llvm_type, var_name);

                // Convert value if needed
                LLVMValueRef stored_value = value_result.value;

                // Handle type conversions
                if (type_is_integer(final_type) && type_is_float(inferred_type)) {
                    stored_value = LLVMBuildFPToSI(ctx->builder, value_result.value,
                                                   LLVMInt64TypeInContext(ctx->context), "toint");
                } else if (type_is_float(final_type) && type_is_integer(inferred_type)) {
                    stored_value = LLVMBuildSIToFP(ctx->builder, value_result.value,
                                                   LLVMDoubleTypeInContext(ctx->context), "tofloat");
                } else if (final_type->kind == TYPE_CHAR && inferred_type->kind != TYPE_CHAR) {
                    if (type_is_float(inferred_type)) {
                        stored_value = LLVMBuildFPToSI(ctx->builder, value_result.value,
                                                       LLVMInt8TypeInContext(ctx->context), "tochar");
                    } else if (type_is_integer(inferred_type)) {
                        stored_value = LLVMBuildTrunc(ctx->builder, value_result.value,
                                                      LLVMInt8TypeInContext(ctx->context), "tochar");
                    }
                }

                LLVMBuildStore(ctx->builder, stored_value, var);

                // Insert into symbol table
                env_insert(ctx->env, var_name, final_type, var);

                printf("Defined %s :: %s\n", var_name, type_to_string(final_type));

                result.type = final_type;
                result.value = stored_value;
                return result;
            }

            // Handle 'quote' special form
            if (strcmp(head->symbol, "quote") == 0) {
                if (ast->list.count != 2) {
                    fprintf(stderr, "%s:%d:%d: error: 'quote' requires 1 argument\n",
                            parser_get_filename(), ast->line, ast->column);
                    exit(1);
                }

                AST *quoted = ast->list.items[1];

                // Handle different quoted types
                if (quoted->type == AST_LIST) {
                    // Create runtime list
                    LLVMValueRef list_fn = get_rt_list_create(ctx);
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
                    // Quoted symbol becomes a string
                    result.type = type_string();
                    result.value = LLVMBuildGlobalStringPtr(ctx->builder,
                                                            quoted->symbol, "quoted_sym");
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

            // Handle 'show' function
            if (strcmp(head->symbol, "show") == 0) {
                if (ast->list.count != 2) {
                    fprintf(stderr, "%s:%d:%d: error: 'show' requires 1 argument, got %zu\n",
                            parser_get_filename(), ast->line, ast->column, ast->list.count - 1);
                    exit(1);
                }

                AST *arg = ast->list.items[1];
                LLVMValueRef printf_fn = get_or_declare_printf(ctx);

                // Handle quoted expressions (compile-time AST printing)
                if (arg->type == AST_LIST && arg->list.count > 0 &&
                    arg->list.items[0]->type == AST_SYMBOL &&
                    strcmp(arg->list.items[0]->symbol, "quote") == 0) {
                    if (arg->list.count == 2) {
                        codegen_print_ast(ctx, arg->list.items[1]);
                        LLVMValueRef newline = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "newline");
                        LLVMValueRef nl_args[] = {newline};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, nl_args, 1, "");
                    }

                    result.type = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Handle string literals
                if (arg->type == AST_STRING) {
                    LLVMValueRef str = LLVMBuildGlobalStringPtr(ctx->builder, arg->string, "str");
                    LLVMValueRef args[] = {get_fmt_str(ctx), str};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, args, 2, "");

                    result.type = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Handle character literals
                if (arg->type == AST_CHAR) {
                    LLVMValueRef ch = LLVMConstInt(LLVMInt8TypeInContext(ctx->context),
                                                   arg->character, 0);
                    LLVMValueRef args[] = {get_fmt_char(ctx), ch};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, args, 2, "");

                    result.type = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Handle keyword literals
                if (arg->type == AST_KEYWORD) {
                    char keyword_buf[256];
                    snprintf(keyword_buf, sizeof(keyword_buf), ":%s\n", arg->keyword);
                    LLVMValueRef kw_str = LLVMBuildGlobalStringPtr(ctx->builder, keyword_buf, "kw");
                    LLVMValueRef args[] = {kw_str};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, args, 1, "");

                    result.type = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Handle symbols (variables)
                if (arg->type == AST_SYMBOL) {
                    EnvEntry *entry = env_lookup(ctx->env, arg->symbol);
                    if (!entry) {
                        fprintf(stderr, "%s:%d:%d: error: unbound variable: %s\n",
                                parser_get_filename(), ast->line, ast->column, arg->symbol);
                        exit(1);
                    }

                    // SPECIAL CASE: Arrays must be handled BEFORE loading
                    // Because arrays are pointers to stack allocations, not scalar values
                    if (entry->type->kind == TYPE_ARR) {
                        LLVMTypeRef arr_type = type_to_llvm(ctx, entry->type);

                        // Print opening bracket
                        LLVMValueRef open = LLVMBuildGlobalStringPtr(ctx->builder, "[", "open");
                        LLVMValueRef open_args[] = {open};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, open_args, 1, "");

                        // Print each element with TYPE PRESERVATION
                        for (int i = 0; i < entry->type->arr_size; i++) {
                            if (i > 0) {
                                LLVMValueRef space = LLVMBuildGlobalStringPtr(ctx->builder, " ", "space");
                                LLVMValueRef space_args[] = {space};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                               printf_fn, space_args, 1, "");
                            }

                            // Get pointer to element: &array[0][i]
                            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                            LLVMValueRef idx = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), i, 0);
                            LLVMValueRef indices[] = {zero, idx};
                            LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder, arr_type,
                                                                  entry->value, indices, 2, "elem_ptr");

                            // Load the element value
                            LLVMTypeRef elem_type_llvm = type_to_llvm(ctx, entry->type->arr_element_type);
                            LLVMValueRef elem = LLVMBuildLoad2(ctx->builder, elem_type_llvm, elem_ptr, "elem");

                            // Print element based on EXACT type (not just type_is_integer)
                            Type *elem_type = entry->type->arr_element_type;

                            if (elem_type->kind == TYPE_CHAR) {
                                LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "'%c'", "fmt_char_q");
                                LLVMValueRef elem_args[] = {fmt, elem};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                               printf_fn, elem_args, 2, "");
                            } else if (elem_type->kind == TYPE_STRING) {
                                LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "\"%s\"", "fmt_str_q");
                                LLVMValueRef elem_args[] = {fmt, elem};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                               printf_fn, elem_args, 2, "");
                            } else if (elem_type->kind == TYPE_KEYWORD) {
                                LLVMValueRef elem_args[] = {get_fmt_str_no_newline(ctx), elem};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                               printf_fn, elem_args, 2, "");
                            } else if (elem_type->kind == TYPE_HEX) {
                                LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "0x%lX", "fmt_hex_nn");
                                LLVMValueRef elem_args[] = {fmt, elem};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                               printf_fn, elem_args, 2, "");
                            } else if (elem_type->kind == TYPE_BIN) {
                                LLVMValueRef fn_bin = get_or_declare_print_binary(ctx);
                                LLVMValueRef args[] = {elem};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn_bin),
                                               fn_bin, args, 1, "");
                            } else if (elem_type->kind == TYPE_OCT) {
                                LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "0o%lo", "fmt_oct_nn");
                                LLVMValueRef elem_args[] = {fmt, elem};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                               printf_fn, elem_args, 2, "");
                            } else if (elem_type->kind == TYPE_RATIO) {
                                LLVMValueRef print_fn = get_rt_print_value(ctx);
                                LLVMValueRef args[] = {elem};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(print_fn),
                                               print_fn, args, 1, "");
                            } else if (elem_type->kind == TYPE_INT) {
                                LLVMValueRef elem_args[] = {get_fmt_int_no_newline(ctx), elem};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                               printf_fn, elem_args, 2, "");
                            } else if (elem_type->kind == TYPE_FLOAT) {
                                LLVMValueRef elem_args[] = {get_fmt_float_no_newline(ctx), elem};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                               printf_fn, elem_args, 2, "");
                            }
                        }

                        // Print closing bracket and newline
                        LLVMValueRef close = LLVMBuildGlobalStringPtr(ctx->builder, "]\n", "close");
                        LLVMValueRef close_args[] = {close};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, close_args, 1, "");

                        result.type = type_float();
                        result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                        return result;
                    }

                    // For all other types, load the value normally
                    LLVMValueRef loaded_value = LLVMBuildLoad2(ctx->builder,
                                                               type_to_llvm(ctx, entry->type),
                                                               entry->value, arg->symbol);

                    // Handle list type - use runtime printer
                    if (entry->type->kind == TYPE_LIST) {
                        LLVMValueRef print_fn = get_rt_print_list(ctx);
                        LLVMValueRef args[] = {loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(print_fn),
                                       print_fn, args, 1, "");

                        LLVMValueRef newline = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "newline");
                        LLVMValueRef nl_args[] = {newline};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, nl_args, 1, "");
                    }
                    // Handle ratio type
                    else if (entry->type->kind == TYPE_RATIO) {
                        LLVMValueRef print_fn = get_rt_print_value(ctx);
                        LLVMValueRef args[] = {loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(print_fn),
                                       print_fn, args, 1, "");

                        LLVMValueRef newline = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "newline");
                        LLVMValueRef nl_args[] = {newline};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, nl_args, 1, "");
                    }
                    // Handle char type
                    else if (entry->type->kind == TYPE_CHAR) {
                        LLVMValueRef args[] = {get_fmt_char(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, args, 2, "");
                    }
                    // Handle string type
                    else if (entry->type->kind == TYPE_STRING) {
                        LLVMValueRef args[] = {get_fmt_str(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, args, 2, "");
                    }
                    // Handle keyword type
                    else if (entry->type->kind == TYPE_KEYWORD) {
                        LLVMValueRef args[] = {get_fmt_str(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, args, 2, "");
                    }
                    // Handle hex type
                    else if (entry->type->kind == TYPE_HEX) {
                        LLVMValueRef args[] = {get_fmt_hex(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, args, 2, "");
                    }
                    // Handle binary type
                    else if (entry->type->kind == TYPE_BIN) {
                        LLVMValueRef fn_bin = get_or_declare_print_binary(ctx);
                        LLVMValueRef args[] = {loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn_bin),
                                       fn_bin, args, 1, "");
                    }
                    // Handle octal type
                    else if (entry->type->kind == TYPE_OCT) {
                        LLVMValueRef args[] = {get_fmt_oct(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, args, 2, "");
                    }
                    // Handle int type
                    else if (entry->type->kind == TYPE_INT) {
                        LLVMValueRef args[] = {get_fmt_int(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, args, 2, "");
                    }
                    // Handle float type (default)
                    else {
                        LLVMValueRef args[] = {get_fmt_float(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, args, 2, "");
                    }

                    result.type = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Handle expressions (not symbols)
                CodegenResult arg_result = codegen_expr(ctx, arg);

                // Handle Ratio type
                if (arg_result.type && arg_result.type->kind == TYPE_RATIO) {
                    LLVMValueRef print_fn = get_rt_print_value(ctx);
                    LLVMValueRef args[] = {arg_result.value};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(print_fn),
                                   print_fn, args, 1, "");

                    LLVMValueRef newline = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "newline");
                    LLVMValueRef nl_args[] = {newline};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, nl_args, 1, "");

                    result.type = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Handle Array type (from expressions like [1 2 3])
                if (arg_result.type && arg_result.type->kind == TYPE_ARR) {
                    LLVMTypeRef arr_type = type_to_llvm(ctx, arg_result.type);

                    // Print opening bracket
                    LLVMValueRef open = LLVMBuildGlobalStringPtr(ctx->builder, "[", "open");
                    LLVMValueRef open_args[] = {open};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                  printf_fn, open_args, 1, "");

                    // Print each element with type preservation
                    for (int i = 0; i < arg_result.type->arr_size; i++) {
                        if (i > 0) {
                            LLVMValueRef space = LLVMBuildGlobalStringPtr(ctx->builder, " ", "space");
                            LLVMValueRef space_args[] = {space};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                          printf_fn, space_args, 1, "");
                        }

                        // Get element pointer: &array[0][i]
                        LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                        LLVMValueRef idx = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), i, 0);
                        LLVMValueRef indices[] = {zero, idx};
                        LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder, arr_type,
                                                             arg_result.value, indices, 2, "elem_ptr");

                        // Load element
                        LLVMTypeRef elem_type_llvm = type_to_llvm(ctx, arg_result.type->arr_element_type);
                        LLVMValueRef elem = LLVMBuildLoad2(ctx->builder, elem_type_llvm, elem_ptr, "elem");

                        // Print element based on ACTUAL type (not converted to Int)
                        Type *elem_type = arg_result.type->arr_element_type;

                        if (elem_type->kind == TYPE_CHAR) {
                            LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "'%c'", "fmt_char_q");
                            LLVMValueRef elem_args[] = {fmt, elem};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                          printf_fn, elem_args, 2, "");
                        } else if (elem_type->kind == TYPE_STRING) {
                            LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "\"%s\"", "fmt_str_q");
                            LLVMValueRef elem_args[] = {fmt, elem};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                          printf_fn, elem_args, 2, "");
                        } else if (elem_type->kind == TYPE_KEYWORD) {
                            LLVMValueRef elem_args[] = {get_fmt_str_no_newline(ctx), elem};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                          printf_fn, elem_args, 2, "");
                        } else if (elem_type->kind == TYPE_HEX) {
                            LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "0x%lX", "fmt_hex_nn");
                            LLVMValueRef elem_args[] = {fmt, elem};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                          printf_fn, elem_args, 2, "");
                        } else if (elem_type->kind == TYPE_BIN) {
                            // Simplified: just print with 0b prefix and the number
                            // Full binary would require the helper function
                            LLVMValueRef fn_bin = get_or_declare_print_binary(ctx);
                            LLVMValueRef args[] = {elem};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn_bin),
                                          fn_bin, args, 1, "");
                        } else if (elem_type->kind == TYPE_OCT) {
                            LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "0o%lo", "fmt_oct_nn");
                            LLVMValueRef elem_args[] = {fmt, elem};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                          printf_fn, elem_args, 2, "");
                        } else if (elem_type->kind == TYPE_RATIO) {
                            LLVMValueRef print_fn = get_rt_print_value(ctx);
                            LLVMValueRef args[] = {elem};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(print_fn),
                                          print_fn, args, 1, "");
                        } else if (type_is_integer(elem_type)) {
                            LLVMValueRef elem_args[] = {get_fmt_int_no_newline(ctx), elem};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                          printf_fn, elem_args, 2, "");
                        } else if (type_is_float(elem_type)) {
                            LLVMValueRef elem_args[] = {get_fmt_float_no_newline(ctx), elem};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                          printf_fn, elem_args, 2, "");
                        }
                    }

                    // Print closing bracket and newline
                    LLVMValueRef close = LLVMBuildGlobalStringPtr(ctx->builder, "]\n", "close");
                    LLVMValueRef close_args[] = {close};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                  printf_fn, close_args, 1, "");

                    result.type = type_float();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Handle list type in expressions
                if (arg_result.type && arg_result.type->kind == TYPE_LIST) {
                    LLVMValueRef print_fn = get_rt_print_list(ctx);
                    LLVMValueRef args[] = {arg_result.value};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(print_fn),
                                   print_fn, args, 1, "");

                    LLVMValueRef newline = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "newline");
                    LLVMValueRef nl_args[] = {newline};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, nl_args, 1, "");
                }
                // Handle hex type
                else if (arg_result.type && arg_result.type->kind == TYPE_HEX) {
                    LLVMValueRef args[] = {get_fmt_hex(ctx), arg_result.value};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, args, 2, "");
                }
                // Handle binary type
                else if (arg_result.type && arg_result.type->kind == TYPE_BIN) {
                    LLVMValueRef fn_bin = get_or_declare_print_binary(ctx);
                    LLVMValueRef args[] = {arg_result.value};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn_bin),
                                   fn_bin, args, 1, "");
                }
                // Handle octal type
                else if (arg_result.type && arg_result.type->kind == TYPE_OCT) {
                    LLVMValueRef args[] = {get_fmt_oct(ctx), arg_result.value};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, args, 2, "");
                }
                // Handle integer types
                else if (arg_result.type && type_is_integer(arg_result.type)) {
                    LLVMValueRef args[] = {get_fmt_int(ctx), arg_result.value};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, args, 2, "");
                }
                // Handle float type (default)
                else {
                    LLVMValueRef args[] = {get_fmt_float(ctx), arg_result.value};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, args, 2, "");
                }

                result.type = type_float();
                result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                return result;
            }

            // Arithmetic operators
            if (strcmp(head->symbol, "+") == 0 ||
                strcmp(head->symbol, "-") == 0 ||
                strcmp(head->symbol, "*") == 0 ||
                strcmp(head->symbol, "/") == 0) {

                const char *op = head->symbol;

                if (ast->list.count < 2) {
                    fprintf(stderr, "%s:%d:%d: error: '%s' requires at least 1 argument\n",
                            parser_get_filename(), ast->line, ast->column, op);
                    exit(1);
                }

                // Process first argument
                CodegenResult first = codegen_expr(ctx, ast->list.items[1]);
                Type *result_type = first.type;
                LLVMValueRef result_value = first.value;

                if (!type_is_numeric(result_type)) {
                    fprintf(stderr, "%s:%d:%d: error: cannot perform arithmetic on type %s\n",
                            parser_get_filename(), ast->line, ast->column, type_to_string(result_type));
                    exit(1);
                }

                // Handle unary minus
                if (strcmp(op, "-") == 0 && ast->list.count == 2) {
                    if (type_is_float(result_type)) {
                        result.type = result_type;
                        result.value = LLVMBuildFNeg(ctx->builder, result_value, "negtmp");
                        return result;
                    } else {
                        result.type = result_type;
                        LLVMValueRef zero = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                        result.value = LLVMBuildSub(ctx->builder, zero, result_value, "negtmp");
                        return result;
                    }
                }

                // Handle reciprocal (unary /)
                if (strcmp(op, "/") == 0 && ast->list.count == 2) {
                    if (type_is_float(result_type)) {
                        LLVMValueRef one = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 1.0);
                        result.type = result_type;
                        result.value = LLVMBuildFDiv(ctx->builder, one, result_value, "invtmp");
                        return result;
                    } else {
                        LLVMValueRef result_f = LLVMBuildSIToFP(ctx->builder, result_value,
                                                                LLVMDoubleTypeInContext(ctx->context), "tofloat");
                        LLVMValueRef one = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 1.0);
                        result.type = type_float();
                        result.value = LLVMBuildFDiv(ctx->builder, one, result_f, "invtmp");
                        return result;
                    }
                }

                // Check if we're dealing with ratios
                if (result_type->kind == TYPE_RATIO) {
                    // Use ratio arithmetic
                    for (size_t i = 2; i < ast->list.count; i++) {
                        CodegenResult rhs = codegen_expr(ctx, ast->list.items[i]);

                        if (rhs.type->kind != TYPE_RATIO) {
                            fprintf(stderr, "Error: cannot mix Ratio with other numeric types\n");
                            exit(1);
                        }

                        LLVMValueRef fn = NULL;
                        if (strcmp(op, "+") == 0) fn = get_rt_ratio_add(ctx);
                        else if (strcmp(op, "-") == 0) fn = get_rt_ratio_sub(ctx);
                        else if (strcmp(op, "*") == 0) fn = get_rt_ratio_mul(ctx);
                        else if (strcmp(op, "/") == 0) fn = get_rt_ratio_div(ctx);

                        LLVMValueRef args[] = {result_value, rhs.value};
                        result_value = LLVMBuildCall2(ctx->builder,
                                                      LLVMGlobalGetValueType(fn),
                                                      fn, args, 2, "ratio_op");
                    }

                    result.type = result_type;
                    result.value = result_value;
                    return result;
                }

                // Binary operations
                for (size_t i = 2; i < ast->list.count; i++) {
                    CodegenResult rhs = codegen_expr(ctx, ast->list.items[i]);

                    if (!type_is_numeric(rhs.type)) {
                        fprintf(stderr, "%s:%d:%d: error: cannot perform arithmetic on type %s\n",
                                parser_get_filename(), ast->line, ast->column, type_to_string(rhs.type));
                        exit(1);
                    }

                    // Check for incompatible type mixing
                    if ((result_type->kind == TYPE_HEX || result_type->kind == TYPE_BIN || result_type->kind == TYPE_OCT) &&
                        (rhs.type->kind == TYPE_HEX || rhs.type->kind == TYPE_BIN || rhs.type->kind == TYPE_OCT) &&
                        result_type->kind != rhs.type->kind) {
                        fprintf(stderr, "%s:%d:%d: error: cannot mix %s and %s in arithmetic - ambiguous result type\n",
                                parser_get_filename(), ast->line, ast->column,
                                type_to_string(result_type), type_to_string(rhs.type));
                        exit(1);
                    }

                    // Determine result type
                    Type *new_result_type;
                    if (type_is_float(result_type) || type_is_float(rhs.type)) {
                        new_result_type = type_float();
                    } else if (result_type->kind == TYPE_CHAR || rhs.type->kind == TYPE_CHAR) {
                        new_result_type = type_int();
                    } else if (result_type->kind == rhs.type->kind) {
                        new_result_type = result_type;
                    } else if (result_type->kind == TYPE_INT || rhs.type->kind == TYPE_INT) {
                        new_result_type = type_int();
                    } else {
                        new_result_type = type_int();
                    }

                    // Convert operands if needed
                    LLVMValueRef lhs_converted = result_value;
                    LLVMValueRef rhs_converted = rhs.value;

                    if (type_is_float(new_result_type)) {
                        if (type_is_integer(result_type)) {
                            if (result_type->kind == TYPE_CHAR) {
                                LLVMValueRef extended = LLVMBuildSExt(ctx->builder, result_value,
                                                                      LLVMInt64TypeInContext(ctx->context), "ext");
                                lhs_converted = LLVMBuildSIToFP(ctx->builder, extended,
                                                               LLVMDoubleTypeInContext(ctx->context), "tofloat");
                            } else {
                                lhs_converted = LLVMBuildSIToFP(ctx->builder, result_value,
                                                               LLVMDoubleTypeInContext(ctx->context), "tofloat");
                            }
                        }
                        if (type_is_integer(rhs.type)) {
                            if (rhs.type->kind == TYPE_CHAR) {
                                LLVMValueRef extended = LLVMBuildSExt(ctx->builder, rhs.value,
                                                                      LLVMInt64TypeInContext(ctx->context), "ext");
                                rhs_converted = LLVMBuildSIToFP(ctx->builder, extended,
                                                               LLVMDoubleTypeInContext(ctx->context), "tofloat");
                            } else {
                                rhs_converted = LLVMBuildSIToFP(ctx->builder, rhs.value,
                                                               LLVMDoubleTypeInContext(ctx->context), "tofloat");
                            }
                        }
                    } else {
                        if (result_type->kind == TYPE_CHAR) {
                            lhs_converted = LLVMBuildSExt(ctx->builder, result_value,
                                                         LLVMInt64TypeInContext(ctx->context), "ext");
                        }
                        if (rhs.type->kind == TYPE_CHAR) {
                            rhs_converted = LLVMBuildSExt(ctx->builder, rhs.value,
                                                         LLVMInt64TypeInContext(ctx->context), "ext");
                        }
                    }

                    // Perform operation
                    if (type_is_float(new_result_type)) {
                        if (strcmp(op, "+") == 0) {
                            result_value = LLVMBuildFAdd(ctx->builder, lhs_converted, rhs_converted, "addtmp");
                        } else if (strcmp(op, "-") == 0) {
                            result_value = LLVMBuildFSub(ctx->builder, lhs_converted, rhs_converted, "subtmp");
                        } else if (strcmp(op, "*") == 0) {
                            result_value = LLVMBuildFMul(ctx->builder, lhs_converted, rhs_converted, "multmp");
                        } else if (strcmp(op, "/") == 0) {
                            result_value = LLVMBuildFDiv(ctx->builder, lhs_converted, rhs_converted, "divtmp");
                        }
                    } else {
                        if (strcmp(op, "+") == 0) {
                            result_value = LLVMBuildAdd(ctx->builder, lhs_converted, rhs_converted, "addtmp");
                        } else if (strcmp(op, "-") == 0) {
                            result_value = LLVMBuildSub(ctx->builder, lhs_converted, rhs_converted, "subtmp");
                        } else if (strcmp(op, "*") == 0) {
                            result_value = LLVMBuildMul(ctx->builder, lhs_converted, rhs_converted, "multmp");
                        } else if (strcmp(op, "/") == 0) {
                            result_value = LLVMBuildSDiv(ctx->builder, lhs_converted, rhs_converted, "divtmp");
                        }
                    }

                    result_type = new_result_type;
                }

                result.type = result_type;
                result.value = result_value;
                return result;
            }

            // Check if it's a user-defined function or variable
            EnvEntry *entry = env_lookup(ctx->env, head->symbol);

            // If it's a variable being used in function position, that's an error
            if (entry && entry->kind == ENV_VAR) {
                fprintf(stderr, "%s:%d:%d: error: '%s' is a variable, not a function\n",
                        parser_get_filename(), ast->line, ast->column, head->symbol);
                exit(1);
            }

            // Function call - FIXED VERSION
            if (entry && entry->kind == ENV_FUNC) {
                // Check argument count
                size_t arg_count = ast->list.count - 1;
                if ((int)arg_count != entry->param_count) {
                    fprintf(stderr, "%s:%d:%d: error: function '%s' expects %d arguments, got %zu\n",
                            parser_get_filename(), ast->line, ast->column,
                            head->symbol, entry->param_count, arg_count);
                    exit(1);
                }

                // Codegen arguments with proper type conversion
                LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * arg_count);
                for (size_t i = 0; i < arg_count; i++) {
                    CodegenResult arg_result = codegen_expr(ctx, ast->list.items[i + 1]);

                    // Get expected parameter type
                    Type *expected_type = entry->params[i].type;
                    Type *actual_type = arg_result.type;

                    LLVMValueRef converted_arg = arg_result.value;

                    // Perform type conversion if types don't match
                    if (expected_type && actual_type && expected_type->kind != actual_type->kind) {
                        LLVMTypeRef expected_llvm = type_to_llvm(ctx, expected_type);

                        // Float to Integer conversion
                        if (type_is_integer(expected_type) && type_is_float(actual_type)) {
                            converted_arg = LLVMBuildFPToSI(ctx->builder, arg_result.value,
                                                           expected_llvm, "arg_conv");
                        }
                        // Integer to Float conversion
                        else if (type_is_float(expected_type) && type_is_integer(actual_type)) {
                            if (actual_type->kind == TYPE_CHAR) {
                                // Char needs to be extended to i64 first, then converted to float
                                LLVMValueRef extended = LLVMBuildSExt(ctx->builder, arg_result.value,
                                                                      LLVMInt64TypeInContext(ctx->context), "ext");
                                converted_arg = LLVMBuildSIToFP(ctx->builder, extended,
                                                               expected_llvm, "arg_conv");
                            } else {
                                converted_arg = LLVMBuildSIToFP(ctx->builder, arg_result.value,
                                                               expected_llvm, "arg_conv");
                            }
                        }
                        // Integer to Char conversion
                        else if (expected_type->kind == TYPE_CHAR && type_is_integer(actual_type)) {
                            converted_arg = LLVMBuildTrunc(ctx->builder, arg_result.value,
                                                          expected_llvm, "arg_conv");
                        }
                        // Char to Integer conversion
                        else if (type_is_integer(expected_type) && actual_type->kind == TYPE_CHAR) {
                            converted_arg = LLVMBuildSExt(ctx->builder, arg_result.value,
                                                         expected_llvm, "arg_conv");
                        }
                        // Between different integer types (hex, bin, oct, int)
                        else if (type_is_integer(expected_type) && type_is_integer(actual_type)) {
                            // All integer types use i64, so no conversion needed
                            // But we keep this branch for clarity
                            converted_arg = arg_result.value;
                        }
                    }

                    args[i] = converted_arg;
                }

                // Call the function
                result.value = LLVMBuildCall2(ctx->builder,
                                             LLVMGlobalGetValueType(entry->func_ref),
                                             entry->func_ref,
                                             args, arg_count, "calltmp");
                result.type = entry->return_type;

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
                    fprintf(stderr, "%s:%d:%d: error: type cast '%s' requires exactly 1 argument\n",
                            parser_get_filename(), ast->line, ast->column, cast_target);
                    exit(1);
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

            fprintf(stderr, "%s:%d:%d: error: unknown function: %s\n",
                    parser_get_filename(), ast->line, ast->column, head->symbol);
            exit(1);
        }

        fprintf(stderr, "%s:%d:%d: error: function call requires symbol in head position\n",
                parser_get_filename(), ast->line, ast->column);
        exit(1);
    }

    default:
        fprintf(stderr, "%s:%d:%d: error: unknown AST type: %d\n",
                parser_get_filename(), ast->line, ast->column, ast->type);
        exit(1);
    }
}

void register_builtins(CodegenContext *ctx) {
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
}
