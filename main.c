#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "reader.h"
#include "cli.h"
#include "types.h"
#include "env.h"
#include "repl.h"

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/TargetMachine.h>

/// Helpers

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open file: %s\n", path);
        exit(1);
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *source = malloc(size + 1);
    fread(source, 1, size, f);
    source[size] = '\0';
    fclose(f);

    return source;
}

/// Codegen Context

typedef struct {
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMContextRef context;
    Env *env;
    // Created only when first needed
    LLVMValueRef fmt_str;   // "%s\n"
    LLVMValueRef fmt_char;  // "%c\n"
    LLVMValueRef fmt_int;   // "%ld\n"
    LLVMValueRef fmt_float; // "%g\n"
    LLVMValueRef fmt_hex;   // "0x%lX\n"
    LLVMValueRef fmt_bin;   // handled specially
    LLVMValueRef fmt_oct;   // "0o%lo\n"
} CodegenContext;

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
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    case TYPE_BOOL:
        return LLVMInt1TypeInContext(ctx->context);
    default:
        return LLVMDoubleTypeInContext(ctx->context);
    }
}

/// Type checking for operations

bool type_is_numeric(Type *t) {
    return t->kind == TYPE_INT || t->kind == TYPE_FLOAT ||
           t->kind == TYPE_HEX || t->kind == TYPE_BIN || t->kind == TYPE_OCT ||
           t->kind == TYPE_CHAR;
}

bool type_is_integer(Type *t) {
    return t->kind == TYPE_INT || t->kind == TYPE_HEX ||
           t->kind == TYPE_BIN || t->kind == TYPE_OCT || t->kind == TYPE_CHAR;
}

bool type_is_float(Type *t) {
    return t->kind == TYPE_FLOAT;
}

/// Codegen with type tracking

typedef struct {
    LLVMValueRef value;
    Type *type;
} CodegenResult;

// Forward declaration
CodegenResult codegen_expr(CodegenContext *ctx, AST *ast);

/// Helper to print an AST at runtime (for quoted expressions)

void codegen_print_ast(CodegenContext *ctx, AST *ast) {
    LLVMValueRef printf_fn = get_or_declare_printf(ctx);

    switch (ast->type) {
    case AST_NUMBER: {
        LLVMValueRef num = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), ast->number);
        LLVMValueRef args[] = {get_fmt_float(ctx), num};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        break;
    }

    case AST_SYMBOL: {
        LLVMValueRef sym = LLVMBuildGlobalStringPtr(ctx->builder, ast->symbol, "sym");
        LLVMValueRef args[] = {get_fmt_str(ctx), sym};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        break;
    }

    case AST_STRING: {
        LLVMValueRef str = LLVMBuildGlobalStringPtr(ctx->builder, ast->string, "str");
        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "\"%s\"\n", "fmt");
        LLVMValueRef args[] = {fmt, str};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        break;
    }

    case AST_CHAR: {
        LLVMValueRef ch = LLVMConstInt(LLVMInt8TypeInContext(ctx->context), ast->character, 0);
        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "'%c'\n", "fmt");
        LLVMValueRef args[] = {fmt, ch};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
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

        LLVMValueRef rparen = LLVMBuildGlobalStringPtr(ctx->builder, ")\n", "rparen");
        LLVMValueRef rparen_args[] = {rparen};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, rparen_args, 1, "");
        break;
    }

    default:
        break;
    }
}

/// Codegen

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
    /* case AST_NUMBER: { */
    /*     Type *num_type = infer_literal_type(ast->number, ast->literal_str); */
    /*     result.type = num_type; */

    /*     if (type_is_float(num_type)) { */
    /*         result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), ast->number); */
    /*     } else { */
    /*         result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), (long long)ast->number, 0); */
    /*     } */
    /*     return result; */
    /* } */

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
                    CodegenResult body_result = codegen_expr(ctx, lambda->lambda.body);

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
                CodegenResult value_result = codegen_expr(ctx, value_expr);

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

                // Create an alloca for the variable
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

            // Handle 'show' function
            if (strcmp(head->symbol, "show") == 0) {
                if (ast->list.count != 2) {
                    fprintf(stderr, "%s:%d:%d: error: 'show' requires 1 argument, got %zu\n",
                            parser_get_filename(), ast->line, ast->column, ast->list.count - 1);
                    exit(1);
                }

                AST *arg = ast->list.items[1];
                LLVMValueRef printf_fn = get_or_declare_printf(ctx);

                // Handle quoted expressions
                if (arg->type == AST_LIST && arg->list.count > 0 &&
                    arg->list.items[0]->type == AST_SYMBOL &&
                    strcmp(arg->list.items[0]->symbol, "quote") == 0) {
                    if (arg->list.count == 2) {
                        codegen_print_ast(ctx, arg->list.items[1]);
                    }
                }
                // Handle string literals
                else if (arg->type == AST_STRING) {
                    LLVMValueRef str = LLVMBuildGlobalStringPtr(ctx->builder, arg->string, "str");
                    LLVMValueRef args[] = {get_fmt_str(ctx), str};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, args, 2, "");
                }
                // Handle character literals
                else if (arg->type == AST_CHAR) {
                    LLVMValueRef ch = LLVMConstInt(LLVMInt8TypeInContext(ctx->context), arg->character, 0);
                    LLVMValueRef args[] = {get_fmt_char(ctx), ch};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, args, 2, "");
                }
                // Handle symbols (variables)
                else if (arg->type == AST_SYMBOL) {
                    EnvEntry *entry = env_lookup(ctx->env, arg->symbol);
                    if (!entry) {
                        fprintf(stderr, "%s:%d:%d: error: unbound variable: %s\n",
                                parser_get_filename(), ast->line, ast->column, arg->symbol);
                        exit(1);
                    }

                    LLVMValueRef loaded_value = LLVMBuildLoad2(ctx->builder, type_to_llvm(ctx, entry->type),
                                                               entry->value, arg->symbol);

                    if (entry->type->kind == TYPE_CHAR) {
                        LLVMValueRef args[] = {get_fmt_char(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                    } else if (entry->type->kind == TYPE_STRING) {
                        LLVMValueRef args[] = {get_fmt_str(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                    } else if (entry->type->kind == TYPE_HEX) {
                        LLVMValueRef args[] = {get_fmt_hex(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                    } else if (entry->type->kind == TYPE_BIN) {
                        LLVMValueRef fn_bin = get_or_declare_print_binary(ctx);
                        LLVMValueRef args[] = {loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn_bin), fn_bin, args, 1, "");
                    } else if (entry->type->kind == TYPE_OCT) {
                        LLVMValueRef args[] = {get_fmt_oct(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                    } else if (entry->type->kind == TYPE_INT) {
                        LLVMValueRef args[] = {get_fmt_int(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                    /* } else if (entry->type->kind == TYPE_INT || */
                    /*            entry->type->kind == TYPE_HEX || */
                    /*            entry->type->kind == TYPE_BIN || */
                    /*            entry->type->kind == TYPE_OCT) { */
                    /*     LLVMValueRef args[] = {get_fmt_int(ctx), loaded_value}; */
                    /*     LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, ""); */
                    } else {
                        LLVMValueRef args[] = {get_fmt_float(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                    }
                }
                // Handle expressions
                else {
                    CodegenResult arg_result = codegen_expr(ctx, arg);

                    if (arg_result.type && arg_result.type->kind == TYPE_HEX) {
                        LLVMValueRef args[] = {get_fmt_hex(ctx), arg_result.value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                    } else if (arg_result.type && arg_result.type->kind == TYPE_BIN) {
                        LLVMValueRef fn_bin = get_or_declare_print_binary(ctx);
                        LLVMValueRef args[] = {arg_result.value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(fn_bin), fn_bin, args, 1, "");
                    } else if (arg_result.type && arg_result.type->kind == TYPE_OCT) {
                        LLVMValueRef args[] = {get_fmt_oct(ctx), arg_result.value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                    } else if (arg_result.type && type_is_integer(arg_result.type)) {
                        LLVMValueRef args[] = {get_fmt_int(ctx), arg_result.value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                    } else {
                        LLVMValueRef args[] = {get_fmt_float(ctx), arg_result.value};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                    }
                    /* if (arg_result.type && type_is_integer(arg_result.type)) { */
                    /*     LLVMValueRef args[] = {get_fmt_int(ctx), arg_result.value}; */
                    /*     LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, ""); */
                    /* } else { */
                    /*     LLVMValueRef args[] = {get_fmt_float(ctx), arg_result.value}; */
                    /*     LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, ""); */
                    /* } */
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

/// Compile

void compile(CompilerFlags *flags) {
    char *source = read_file(flags->input_file);

    // Set parser context for error reporting
    parser_set_context(flags->input_file, source);

    ASTList exprs = parse_all(source);

    if (exprs.count == 0) {
        fprintf(stderr, "%s:1:1: error: no expression(s) found\n", flags->input_file);
        exit(1);
    }

    printf("Compiling %zu expression(s)\n", exprs.count);

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();

    CodegenContext ctx;
    codegen_init(&ctx, "monad_module");

    LLVMTypeRef main_type = LLVMFunctionType(LLVMInt32TypeInContext(ctx.context), NULL, 0, 0);
    LLVMValueRef main_fn = LLVMAddFunction(ctx.module, "main", main_type);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx.context, main_fn, "entry");
    LLVMPositionBuilderAtEnd(ctx.builder, entry);

    CodegenResult result = {NULL, NULL};
    for (size_t i = 0; i < exprs.count; i++) {
        /* printf("DEBUG: Expression %zu at line %d, column %d:\n", */
        /*        i, exprs.exprs[i]->line, exprs.exprs[i]->column); */
        printf("  ");
        ast_print(exprs.exprs[i]);
        printf("\n");
        result = codegen_expr(&ctx, exprs.exprs[i]);
    }

    if (!result.value) {
        result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx.context), 0.0);
        result.type = type_float();
    }

    // Convert result to i32 based on its type
    LLVMValueRef result_i32;
    if (result.type && type_is_integer(result.type)) {
        result_i32 = LLVMBuildTrunc(ctx.builder, result.value,
                                    LLVMInt32TypeInContext(ctx.context), "result");
    } else {
        result_i32 = LLVMBuildFPToSI(ctx.builder, result.value,
                                     LLVMInt32TypeInContext(ctx.context), "result");
    }

    LLVMBuildRet(ctx.builder, result_i32);

    char *error = NULL;
    LLVMVerifyModule(ctx.module, LLVMAbortProcessAction, &error);
    LLVMDisposeMessage(error);

    char *base_name = flags->output_name ? strdup(flags->output_name) : get_base_executable_name(flags->input_file);

    if (flags->emit_ir) {
        char ir_file[256];
        snprintf(ir_file, sizeof(ir_file), "%s.ll", base_name);
        if (LLVMPrintModuleToFile(ctx.module, ir_file, &error) != 0) {
            fprintf(stderr, "Failed to write IR: %s\n", error);
            LLVMDisposeMessage(error);
        } else {
            printf("Wrote IR to %s\n", ir_file);
        }
    }

    if (flags->emit_bc) {
        char bc_file[256];
        snprintf(bc_file, sizeof(bc_file), "%s.bc", base_name);
        if (LLVMWriteBitcodeToFile(ctx.module, bc_file) != 0) {
            fprintf(stderr, "Failed to write bitcode\n");
        } else {
            printf("Wrote bitcode to %s\n", bc_file);
        }
    }

    if (flags->emit_obj || flags->emit_asm || (!flags->emit_ir && !flags->emit_bc)) {
        LLVMTargetRef target;
        char *triple = LLVMGetDefaultTargetTriple();

        if (LLVMGetTargetFromTriple(triple, &target, &error) != 0) {
            fprintf(stderr, "Failed to get target: %s\n", error);
            LLVMDisposeMessage(error);
            exit(1);
        }

        LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
            target, triple, "generic", "",
            LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault
        );

        if (flags->emit_asm) {
            char asm_file[256];
            snprintf(asm_file, sizeof(asm_file), "%s.s", base_name);
            if (LLVMTargetMachineEmitToFile(machine, ctx.module, asm_file,
                                            LLVMAssemblyFile, &error) != 0) {
                fprintf(stderr, "Failed to emit assembly: %s\n", error);
                LLVMDisposeMessage(error);
            } else {
                printf("Wrote assembly to %s\n", asm_file);
            }
        }

        char obj_file[256];
        snprintf(obj_file, sizeof(obj_file), "%s.o", base_name);

        if (LLVMTargetMachineEmitToFile(machine, ctx.module, obj_file,
                                        LLVMObjectFile, &error) != 0) {
            fprintf(stderr, "Failed to emit object file: %s\n", error);
            LLVMDisposeMessage(error);
        } else {
            if (flags->emit_obj) {
                printf("Wrote object file to %s\n", obj_file);
            }

            if (!flags->emit_ir && !flags->emit_bc && !flags->emit_obj && !flags->emit_asm) {
                char cmd[512];
                snprintf(cmd, sizeof(cmd), "gcc %s -o %s -lm -no-pie", obj_file, base_name);
                int ret = system(cmd);
                if (ret == 0) {
                    printf("Created executable: %s\n", base_name);
                    remove(obj_file);
                } else {
                    fprintf(stderr, "Failed to link executable\n");
                }
            }
        }

        LLVMDisposeTargetMachine(machine);
        LLVMDisposeMessage(triple);
    }

    printf("\nSymbol Table:\n");
    env_print(ctx.env);

    free(base_name);
    for (size_t i = 0; i < exprs.count; i++) {
        ast_free(exprs.exprs[i]);
    }
    free(exprs.exprs);
    free(source);
    codegen_dispose(&ctx);
}

int main(int argc, char **argv) {
    CompilerFlags flags = parse_flags(argc, argv);

    if (flags.start_repl) {
        repl_run();
        return 0;
    }

    compile(&flags);
    return 0;
}
