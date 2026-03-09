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
    case TYPE_SYMBOL:
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
            fprintf(stderr, "%s:%d:%d: error: qualified symbol '%s' used but no module context\n",
                    parser_get_filename(), ast->line, ast->column, symbol_name);
            free(module_prefix);
            exit(1);
        }

        // Find the import with this prefix
        ImportDecl *import = module_context_find_import(ctx->module_ctx, module_prefix);
        if (!import) {
            fprintf(stderr, "%s:%d:%d: error: unknown module prefix '%s'\n",
                    parser_get_filename(), ast->line, ast->column, module_prefix);
            free(module_prefix);
            exit(1);
        }

        // Check if this symbol is included in the import
        if (!import_decl_includes_symbol(import, local_symbol)) {
            fprintf(stderr, "%s:%d:%d: error: symbol '%s' not imported from module '%s'\n",
                    parser_get_filename(), ast->line, ast->column,
                    local_symbol, import->module_name);
            free(module_prefix);
            exit(1);
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
    } else {
        // fallback: float
        LLVMValueRef args[] = {newline ? get_fmt_float(ctx) : get_fmt_float_no_newline(ctx), val};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
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
        EnvEntry *entry = resolve_symbol_with_modules(ctx, ast->symbol, ast);

        if (!entry) {
            fprintf(stderr, "%s:%d:%d: error: unbound variable: %s\n",
                    parser_get_filename(), ast->line, ast->column, ast->symbol);
            exit(1);
        }

        result.type = type_clone(entry->type);
        result.value =
            LLVMBuildLoad2(ctx->builder, type_to_llvm(ctx, entry->type),
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

                        Type *param_type = NULL;
                        if (param->type_name) {
                            param_type = type_from_name(param->type_name);
                            if (!param_type) {
                                fprintf(stderr, "%s:%d:%d: error: unknown parameter type '%s'\n",
                                        parser_get_filename(), lambda->line, lambda->column,
                                        param->type_name);
                                exit(1);
                            }
                        } else {
                            param_type = type_float(); // default
                        }
                        param_types[i] = type_to_llvm(ctx, param_type);
                        env_params[i].name = strdup(param->name);
                        env_params[i].type = param_type;

                        param_types[i] = type_to_llvm(ctx, param_type);
                        env_params[i].name = strdup(param->name);
                        env_params[i].type = param_type;
                    }

                    // Determine return type

                    Type *ret_type = NULL;
                    if (lambda->lambda.return_type) {
                        ret_type = type_from_name(lambda->lambda.return_type);
                        if (!ret_type) {
                            fprintf(stderr, "%s:%d:%d: error: unknown return type '%s'\n",
                                    parser_get_filename(), ast->line, ast->column,
                                    lambda->lambda.return_type);
                            exit(1);
                        }
                    } else {
                        ret_type = type_float(); // default when no annotation
                    }

                    LLVMTypeRef ret_llvm_type = type_to_llvm(ctx, ret_type);

                    // Create function type
                    LLVMTypeRef func_type = LLVMFunctionType(ret_llvm_type, param_types,
                                                             lambda->lambda.param_count, 0);

                    // Create function
                    LLVMValueRef func = LLVMAddFunction(ctx->module, var_name, func_type);


                    // Apply naked attribute if requested
                    if (lambda->lambda.naked) {
                        unsigned kind = LLVMGetEnumAttributeKindForName("naked", 5);
                        fprintf(stderr, "DEBUG: naked attribute kind=%u\n", kind);
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

                    // Codegen function body
                    CodegenResult body_result;

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
                    env_insert_func(ctx->env, var_name, env_params, lambda->lambda.param_count,
                                   ret_type, func, lambda->lambda.docstring);

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

            // Handle 'show' function
            if (strcmp(head->symbol, "show") == 0) {
                if (ast->list.count < 2) {
                    fprintf(stderr, "%s:%d:%d: error: 'show' requires at least 1 argument\n",
                            parser_get_filename(), ast->line, ast->column);
                    exit(1);
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
                    EnvEntry *entry = env_lookup(ctx->env, arg->symbol);
                    if (!entry) {
                        fprintf(stderr, "%s:%d:%d: error: unbound variable: %s\n",
                                parser_get_filename(), ast->line, ast->column, arg->symbol);
                        exit(1);
                    }

                    // Arrays need special treatment — they're pointers to stack allocations
                    if (entry->type->kind == TYPE_ARR) {
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
                    fprintf(stderr, "%s:%d:%d: error: 'doc' requires 1 argument\n",
                            parser_get_filename(), ast->line, ast->column);
                    exit(1);
                }

                AST *arg = ast->list.items[1];
                if (arg->type != AST_SYMBOL) {
                    fprintf(stderr, "%s:%d:%d: error: 'doc' argument must be a symbol\n",
                            parser_get_filename(), ast->line, ast->column);
                    exit(1);
                }

                EnvEntry *entry = env_lookup(ctx->env, arg->symbol);
                if (!entry) {
                    fprintf(stderr, "%s:%d:%d: error: 'doc' unbound variable: %s\n",
                            parser_get_filename(), ast->line, ast->column, arg->symbol);
                    exit(1);
                }

                /* const char *docstring = entry->docstring ? entry->docstring : ""; */
                const char *docstring = (entry->docstring && strlen(entry->docstring) > 0)
                    ? entry->docstring
                    : "(no documentation)";


                result.type  = type_string();
                result.value = LLVMBuildGlobalStringPtr(ctx->builder, docstring, "docstr");
                return result;
            }

            if (strcmp(head->symbol, "assert-eq") == 0) {
                if (ast->list.count != 4) {
                    fprintf(stderr, "%s:%d:%d: error: 'assert-eq' requires 3 arguments: actual expected label\n",
                            parser_get_filename(), ast->line, ast->column);
                    exit(1);
                }

                CodegenResult actual   = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult expected = codegen_expr(ctx, ast->list.items[2]);
                AST *label_ast         = ast->list.items[3];

                const char *label = (label_ast->type == AST_STRING)
                    ? label_ast->string : "unnamed";

                LLVMValueRef printf_fn = get_or_declare_printf(ctx);

                // Compare based on type
                LLVMValueRef cond = NULL;
                if (actual.type->kind == TYPE_STRING) {
                    // strcmp-based comparison
                    LLVMValueRef strcmp_fn = LLVMGetNamedFunction(ctx->module, "strcmp");
                    if (!strcmp_fn) {
                        LLVMTypeRef p = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                        LLVMTypeRef params[] = {p, p};
                        LLVMTypeRef ft = LLVMFunctionType(LLVMInt32TypeInContext(ctx->context), params, 2, 0);
                        strcmp_fn = LLVMAddFunction(ctx->module, "strcmp", ft);
                    }
                    LLVMValueRef args[] = {actual.value, expected.value};
                    LLVMValueRef cmp = LLVMBuildCall2(ctx->builder,
                                                      LLVMGlobalGetValueType(strcmp_fn),
                                                      strcmp_fn, args, 2, "strcmp");
                    cond = LLVMBuildICmp(ctx->builder, LLVMIntEQ, cmp,
                                         LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0), "eq");
                } else if (type_is_float(actual.type)) {
                    cond = LLVMBuildFCmp(ctx->builder, LLVMRealOEQ,
                                         actual.value, expected.value, "eq");
                } else {
                    cond = LLVMBuildICmp(ctx->builder, LLVMIntEQ,
                                         actual.value, expected.value, "eq");
                }

                // Emit pass/fail branches
                LLVMValueRef func        = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                LLVMBasicBlockRef pass_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "assert_pass");
                LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "assert_fail");
                LLVMBasicBlockRef cont_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "assert_cont");

                LLVMBuildCondBr(ctx->builder, cond, pass_bb, fail_bb);

                // Pass branch
                LLVMPositionBuilderAtEnd(ctx->builder, pass_bb);
                char pass_msg[512];
                snprintf(pass_msg, sizeof(pass_msg), "  \x1b[32m✓\x1b[0m %s\n", label);
                LLVMValueRef pass_str = LLVMBuildGlobalStringPtr(ctx->builder, pass_msg, "pass_msg");
                LLVMValueRef pass_args[] = {pass_str};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                               printf_fn, pass_args, 1, "");
                LLVMBuildBr(ctx->builder, cont_bb);

                // Fail branch
                LLVMPositionBuilderAtEnd(ctx->builder, fail_bb);
                char fail_msg[512];
                snprintf(fail_msg, sizeof(fail_msg), "  \x1b[31m✗ %s FAILED\x1b[0m\n", label);
                LLVMValueRef fail_str = LLVMBuildGlobalStringPtr(ctx->builder, fail_msg, "fail_msg");
                LLVMValueRef fail_args[] = {fail_str};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                               printf_fn, fail_args, 1, "");
                LLVMBuildBr(ctx->builder, cont_bb);


                LLVMPositionBuilderAtEnd(ctx->builder, cont_bb);

                result.type  = type_int();
                result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                return result;
            }

            if (strcmp(head->symbol, "undefined") == 0) {
                // Guard: only valid inside a function body
                LLVMValueRef cur_fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                if (cur_fn == ctx->init_fn) {
                    fprintf(stderr, "%s:%d:%d: error: 'undefined' is only valid inside a function body\n",
                            parser_get_filename(), ast->line, ast->column);
                    exit(1);
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

            if (strcmp(head->symbol, "if") == 0) {
                if (ast->list.count < 3 || ast->list.count > 4) {
                    fprintf(stderr, "%s:%d:%d: error: 'if' requires 2 or 3 arguments\n",
                            parser_get_filename(), ast->line, ast->column);
                    exit(1);
                }

                CodegenResult cond_result = codegen_expr(ctx, ast->list.items[1]);

                // Coerce condition to i1
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

                LLVMValueRef func          = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                LLVMBasicBlockRef then_bb  = LLVMAppendBasicBlockInContext(ctx->context, func, "then");
                LLVMBasicBlockRef else_bb  = LLVMAppendBasicBlockInContext(ctx->context, func, "else");
                LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "ifmerge");

                LLVMBuildCondBr(ctx->builder, cond_val, then_bb, else_bb);

                // Then branch
                LLVMPositionBuilderAtEnd(ctx->builder, then_bb);
                CodegenResult then_result = codegen_expr(ctx, ast->list.items[2]);
                LLVMBuildBr(ctx->builder, merge_bb);
                LLVMBasicBlockRef then_end_bb = LLVMGetInsertBlock(ctx->builder);

                // Else branch
                LLVMPositionBuilderAtEnd(ctx->builder, else_bb);
                CodegenResult else_result = {NULL, NULL};
                if (ast->list.count == 4) {
                    else_result = codegen_expr(ctx, ast->list.items[3]);
                }
                LLVMBuildBr(ctx->builder, merge_bb);
                LLVMBasicBlockRef else_end_bb = LLVMGetInsertBlock(ctx->builder);

                // Merge
                LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);

                // If both branches produce a value of the same type, emit a phi
                if (ast->list.count == 4 && then_result.value && else_result.value
                    && then_result.type && else_result.type
                    && then_result.type->kind == else_result.type->kind) {
                    LLVMValueRef phi = LLVMBuildPhi(ctx->builder,
                                                    type_to_llvm(ctx, then_result.type), "iftmp");
                    LLVMAddIncoming(phi, &then_result.value, &then_end_bb, 1);
                    LLVMAddIncoming(phi, &else_result.value, &else_end_bb, 1);
                    result.value = phi;
                    result.type  = type_clone(then_result.type);
                } else {
                    result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                    result.type  = type_int();
                }
                return result;
            }

            if (strcmp(head->symbol, "for") == 0) {
                if (ast->list.count != 3) {
                    fprintf(stderr, "%s:%d:%d: error: 'for' requires a binding and a body\n",
                            parser_get_filename(), ast->line, ast->column);
                    exit(1);
                }

                AST *binding = ast->list.items[1];
                LLVMTypeRef  i64  = LLVMInt64TypeInContext(ctx->context);
                LLVMValueRef func = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));

                if (binding->type != AST_ARRAY ||
                    binding->array.element_count < 1 ||
                    binding->array.element_count > 4) {
                    fprintf(stderr, "%s:%d:%d: error: 'for' binding must be [n], [var start end], or [var start end step]\n",
                            parser_get_filename(), ast->line, ast->column);
                    exit(1);
                }

                LLVMValueRef start_val;
                LLVMValueRef end_val;
                LLVMValueRef step_val;
                const char  *var_name;
                bool         has_var;
                bool         negative_step;

                if (binding->array.element_count == 1) {
                    // (for [n] body) — repeat n times, no named loop variable
                    CodegenResult count_r = codegen_expr(ctx, binding->array.elements[0]);
                    start_val    = LLVMConstInt(i64, 0, 0);
                    end_val      = type_is_float(count_r.type)
                        ? LLVMBuildFPToSI(ctx->builder, count_r.value, i64, "count")
                        : count_r.value;
                    step_val     = LLVMConstInt(i64, 1, 0);
                    var_name     = "__for_i";
                    has_var      = false;
                    negative_step = false;
                } else {
                    // (for [var start end] body) or (for [var start end step] body)
                    if (binding->array.elements[0]->type != AST_SYMBOL) {
                        fprintf(stderr, "%s:%d:%d: error: 'for' loop variable must be a symbol\n",
                                parser_get_filename(), ast->line, ast->column);
                        exit(1);
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

                    var_name = binding->array.elements[0]->symbol;
                    has_var  = true;
                    negative_step = (binding->array.element_count == 4 &&
                                     binding->array.elements[3]->type == AST_NUMBER &&
                                     binding->array.elements[3]->number < 0);
                }

                // Alloca for loop variable
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

                // Body — push loop var into new scope only if it has a name
                LLVMPositionBuilderAtEnd(ctx->builder, body_bb);
                Env *saved_env = ctx->env;
                ctx->env = env_create();
                if (has_var)
                    env_insert(ctx->env, var_name, type_int(), i_ptr);

                codegen_expr(ctx, ast->list.items[2]);

                env_free(ctx->env);
                ctx->env = saved_env;

                // Increment
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
                    fprintf(stderr, "%s:%d:%d: error: '%s' requires 2 arguments\n",
                            parser_get_filename(), ast->line, ast->column, head->symbol);
                    exit(1);
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
                result.value = LLVMBuildZExt(ctx->builder, cmp,
                                             LLVMInt64TypeInContext(ctx->context), "bool");
                result.type  = type_int();
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

    case AST_TYPE_ALIAS: {
        type_alias_register(ast->type_alias.alias_name,
                            ast->type_alias.target_name);
        printf("Type alias: %s = %s\n", ast->type_alias.alias_name,
               ast->type_alias.target_name);
        result.type = type_int();
        result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
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

    default:
        fprintf(stderr, "%s:%d:%d: error: unknown AST type: %d\n",
                parser_get_filename(), ast->line, ast->column, ast->type);
        exit(1);
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
