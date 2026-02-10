#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include "reader.h"
#include "cli.h"
#include "types.h"
#include "env.h"

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

/// Error reporting

static const char *current_filename = NULL;
static const char *current_source = NULL;

void compiler_error_range(int line, int column, int end_column, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    fprintf(stderr, "%s:%d:%d: error: ", current_filename, line, column);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");

    if (current_source) {
        const char *line_start = current_source;
        int current_line = 1;

        while (current_line < line && *line_start) {
            if (*line_start == '\n') current_line++;
            line_start++;
        }

        const char *line_end = line_start;
        while (*line_end && *line_end != '\n') line_end++;

        fprintf(stderr, "%5d | %.*s\n", line, (int)(line_end - line_start), line_start);

        fprintf(stderr, "      | ");
        for (int i = 1; i < column; i++) {
            fprintf(stderr, " ");
        }

        if (end_column > column) {
            for (int i = column; i < end_column; i++) {
                if (i == column) {
                    fprintf(stderr, "^");
                } else {
                    fprintf(stderr, "~");
                }
            }
        } else {
            fprintf(stderr, "^");
        }
        fprintf(stderr, "\n");
    }

    va_end(args);
    exit(1);
}

void compiler_error(int line, int column, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    fprintf(stderr, "%s:%d:%d: error: ", current_filename, line, column);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");

    if (current_source) {
        const char *line_start = current_source;
        int current_line = 1;

        while (current_line < line && *line_start) {
            if (*line_start == '\n') current_line++;
            line_start++;
        }

        const char *line_end = line_start;
        while (*line_end && *line_end != '\n') line_end++;

        fprintf(stderr, "%5d | %.*s\n", line, (int)(line_end - line_start), line_start);

        fprintf(stderr, "      | ");
        for (int i = 1; i < column; i++) {
            fprintf(stderr, " ");
        }
        fprintf(stderr, "^\n");
    }

    va_end(args);
    exit(1);
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
} CodegenContext;


void codegen_init(CodegenContext *ctx, const char *module_name) {
    ctx->context = LLVMContextCreate();
    ctx->module = LLVMModuleCreateWithNameInContext(module_name, ctx->context);
    ctx->builder = LLVMCreateBuilderInContext(ctx->context);
    ctx->env = env_create();
    // Initialize format strings to NULL - will be created lazily
    ctx->fmt_str  = NULL;
    ctx->fmt_char  = NULL;
    ctx->fmt_int   = NULL;
    ctx->fmt_float = NULL;
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

/// AST with location and type tracking

typedef struct {
    AST *ast;
    int line;
    int column;
    int end_column;
    Type *type;  // Type of this expression
    char *literal_str;  // Original literal string for type inference
} ASTWithLoc;

// Forward declaration
LLVMValueRef codegen_expr_loc(CodegenContext *ctx, ASTWithLoc *ast_loc);

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
    }
}

/// Codegen

LLVMValueRef codegen_expr_loc(CodegenContext *ctx, ASTWithLoc *ast_loc) {
    AST *ast = ast_loc->ast;
    int line = ast_loc->line;
    int column = ast_loc->column;

    switch (ast->type) {
    case AST_NUMBER: {
        Type *num_type = infer_literal_type(ast->number, ast_loc->literal_str);
        ast_loc->type = num_type;

        if (type_is_float(num_type)) {
            return LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), ast->number);
        } else {
            // Integer types
            return LLVMConstInt(LLVMInt64TypeInContext(ctx->context), (long long)ast->number, 0);
        }
    }

    case AST_CHAR: {
        ast_loc->type = type_char();
        return LLVMConstInt(LLVMInt8TypeInContext(ctx->context), ast->character, 0);
    }

    case AST_SYMBOL: {
        // Look up symbol in symbol table
        EnvEntry *entry = env_lookup(ctx->env, ast->symbol);
        if (!entry) {
            compiler_error(line, column, "unbound variable: %s", ast->symbol);
        }
        ast_loc->type = entry->type;
        // Load the value from the alloca
        return LLVMBuildLoad2(ctx->builder, type_to_llvm(ctx, entry->type),
                             entry->value, ast->symbol);
    }

    case AST_STRING: {
        ast_loc->type = type_string();
        return LLVMBuildGlobalStringPtr(ctx->builder, ast->string, "str");
    }

    case AST_LIST: {
        if (ast->list.count == 0) {
            compiler_error(line, column, "empty list not supported");
        }

        AST *head = ast->list.items[0];

        if (head->type == AST_SYMBOL) {
            // Handle 'define' special form
            if (strcmp(head->symbol, "define") == 0) {
                if (ast->list.count < 3) {
                    compiler_error_range(line, column + 1, ast_loc->end_column,
                                       "'define' requires at least 2 arguments");
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
                        compiler_error(line, column, "'define' name must be symbol or type annotation");
                    }
                } else if (name_expr->type == AST_SYMBOL) {
                    var_name = name_expr->symbol;
                } else {
                    compiler_error(line, column, "'define' name must be symbol or type annotation");
                }

                if (!var_name) {
                    compiler_error(line, column, "'define' invalid name format");
                }

                // Create ASTWithLoc for value with literal string preservation
                ASTWithLoc value_loc = {value_expr, line, column, ast_loc->end_column, NULL, NULL};

                // If it's a number literal, use the stored literal string
                if (value_expr->type == AST_NUMBER && value_expr->literal_str) {
                    value_loc.literal_str = strdup(value_expr->literal_str);
                }

                // Codegen the value
                LLVMValueRef value = codegen_expr_loc(ctx, &value_loc);

                // Infer or verify type
                Type *inferred_type = value_loc.type;
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
                LLVMValueRef stored_value = value;

                // Handle type conversions
                if (type_is_integer(final_type) && type_is_float(inferred_type)) {
                    // Float to integer
                    stored_value = LLVMBuildFPToSI(ctx->builder, value,
                                                   LLVMInt64TypeInContext(ctx->context), "toint");
                } else if (type_is_float(final_type) && type_is_integer(inferred_type)) {
                    // Integer to float
                    stored_value = LLVMBuildSIToFP(ctx->builder, value,
                                                   LLVMDoubleTypeInContext(ctx->context), "tofloat");
                } else if (final_type->kind == TYPE_CHAR && inferred_type->kind != TYPE_CHAR) {
                    if (type_is_float(inferred_type)) {
                        stored_value = LLVMBuildFPToSI(ctx->builder, value,
                                                       LLVMInt8TypeInContext(ctx->context), "tochar");
                    } else if (type_is_integer(inferred_type)) {
                        stored_value = LLVMBuildTrunc(ctx->builder, value,
                                                      LLVMInt8TypeInContext(ctx->context), "tochar");
                    }
                }

                LLVMBuildStore(ctx->builder, stored_value, var);

                // Insert into symbol table - store the alloca pointer, not the loaded value
                env_insert(ctx->env, var_name, final_type, var);

                printf("Defined %s :: %s\n", var_name, type_to_string(final_type));

                // Free temporary literal string if allocated
                if (value_loc.literal_str) {
                    free(value_loc.literal_str);
                }

                // Return the stored value
                ast_loc->type = final_type;
                return stored_value;
            }

            // Handle `show' function
            if (strcmp(head->symbol, "show") == 0) {
                if (ast->list.count != 2) {
                    compiler_error_range(line, column + 1, ast_loc->end_column,
                                         "'show' requires 1 argument, got %zu",
                                         ast->list.count - 1);
                }

                ASTWithLoc arg_loc = {ast->list.items[1],  line, column,
                    ast_loc->end_column, NULL, NULL};
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
                    LLVMValueRef str =
                        LLVMBuildGlobalStringPtr(ctx->builder, arg->string, "str");
                    LLVMValueRef args[] = {get_fmt_str(ctx), str};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, args, 2, "");
                }
                // Handle character literals
                else if (arg->type == AST_CHAR) {
                    LLVMValueRef ch = LLVMConstInt(
                        LLVMInt8TypeInContext(ctx->context), arg->character, 0);
                    LLVMValueRef args[] = {get_fmt_char(ctx), ch};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, args, 2, "");
                }
                // Handle symbols (variables)
                else if (arg->type == AST_SYMBOL) {
                    EnvEntry *entry =
                        env_lookup(ctx->env, arg->symbol);
                    if (!entry) {
                        compiler_error(line, column, "unbound variable: %s",
                                       arg->symbol);
                    }

                    // Load the value from alloca
                    LLVMValueRef loaded_value =
                        LLVMBuildLoad2(ctx->builder, type_to_llvm(ctx, entry->type),
                                       entry->value, arg->symbol);

                    // Print based on type
                    if (entry->type->kind == TYPE_CHAR) {
                        LLVMValueRef args[] = {get_fmt_char(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder,
                                       LLVMGlobalGetValueType(printf_fn), printf_fn,
                                       args, 2, "");
                    } else if (entry->type->kind == TYPE_STRING) {
                        LLVMValueRef args[] = {get_fmt_str(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder,
                                       LLVMGlobalGetValueType(printf_fn), printf_fn,
                                       args, 2, "");
                    } else if (entry->type->kind == TYPE_INT ||
                               entry->type->kind == TYPE_HEX ||
                               entry->type->kind == TYPE_BIN ||
                               entry->type->kind == TYPE_OCT) {
                        LLVMValueRef args[] = {get_fmt_int(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder,
                                       LLVMGlobalGetValueType(printf_fn), printf_fn,
                                       args, 2, "");
                    } else {
                        LLVMValueRef args[] = {get_fmt_float(ctx), loaded_value};
                        LLVMBuildCall2(ctx->builder,
                                       LLVMGlobalGetValueType(printf_fn), printf_fn,
                                       args, 2, "");
                    }
                }
                // Handle expressions
                else {
                    LLVMValueRef result = codegen_expr_loc(ctx, &arg_loc);
                    Type *result_type = arg_loc.type;

                    if (result_type && type_is_integer(result_type)) {
                        LLVMValueRef args[] = {get_fmt_int(ctx), result};
                        LLVMBuildCall2(ctx->builder,
                                       LLVMGlobalGetValueType(printf_fn), printf_fn,
                                       args, 2, "");
                    } else {
                        LLVMValueRef args[] = {get_fmt_float(ctx), result};
                        LLVMBuildCall2(ctx->builder,
                                       LLVMGlobalGetValueType(printf_fn), printf_fn,
                                       args, 2, "");
                    }
                }

                // Return 0.0 as a dummy value
                ast_loc->type = type_float();
                return LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
            }

            // Arithmetic operators
            if (strcmp(head->symbol, "+") == 0 ||
                strcmp(head->symbol, "-") == 0 ||
                strcmp(head->symbol, "*") == 0 ||
                strcmp(head->symbol, "/") == 0) {

                const char *op = head->symbol;

                if (ast->list.count < 2) {
                    compiler_error_range(ast_loc->line, ast_loc->column + 1, ast_loc->end_column,
                                       "'%s' requires at least 1 argument", op);
                }

                // Calculate operator position (after opening paren of this list)
                int op_column = ast_loc->column + 1;
                int op_end_column = op_column + strlen(op);

                // Process first argument
                ASTWithLoc first_loc = {ast->list.items[1], line, column, ast_loc->end_column, NULL, NULL};
                if (ast->list.items[1]->type == AST_NUMBER) {
                    char temp[64];
                    snprintf(temp, sizeof(temp), "%g", ast->list.items[1]->number);
                    first_loc.literal_str = strdup(temp);
                }

                LLVMValueRef result = codegen_expr_loc(ctx, &first_loc);
                Type *result_type = first_loc.type;

                if (first_loc.literal_str) {
                    free(first_loc.literal_str);
                }

                // Check if first arg is numeric
                if (!type_is_numeric(result_type)) {
                    compiler_error_range(ast_loc->line, op_column, ast_loc->end_column - 1,
                                       "cannot perform arithmetic on type %s",
                                       type_to_string(result_type));
                }

                // Handle unary minus
                if (strcmp(op, "-") == 0 && ast->list.count == 2) {
                    if (type_is_float(result_type)) {
                        ast_loc->type = result_type;
                        return LLVMBuildFNeg(ctx->builder, result, "negtmp");
                    } else {
                        ast_loc->type = result_type;
                        LLVMValueRef zero = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                        return LLVMBuildSub(ctx->builder, zero, result, "negtmp");
                    }
                }

                // Handle reciprocal (unary /)
                if (strcmp(op, "/") == 0 && ast->list.count == 2) {
                    if (type_is_float(result_type)) {
                        LLVMValueRef one = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 1.0);
                        ast_loc->type = result_type;
                        return LLVMBuildFDiv(ctx->builder, one, result, "invtmp");
                    } else {
                        // For integers, convert to float
                        LLVMValueRef result_f = LLVMBuildSIToFP(ctx->builder, result,
                                                                LLVMDoubleTypeInContext(ctx->context), "tofloat");
                        LLVMValueRef one = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 1.0);
                        ast_loc->type = type_float();
                        return LLVMBuildFDiv(ctx->builder, one, result_f, "invtmp");
                    }
                }

                // Binary operations
                for (size_t i = 2; i < ast->list.count; i++) {
                    ASTWithLoc arg_loc = {ast->list.items[i], line, column, ast_loc->end_column, NULL, NULL};
                    if (ast->list.items[i]->type == AST_NUMBER) {
                        char temp[64];
                        snprintf(temp, sizeof(temp), "%g", ast->list.items[i]->number);
                        arg_loc.literal_str = strdup(temp);
                    }

                    LLVMValueRef rhs = codegen_expr_loc(ctx, &arg_loc);
                    Type *rhs_type = arg_loc.type;

                    if (arg_loc.literal_str) {
                        free(arg_loc.literal_str);
                    }

                    // Type checking
                    if (!type_is_numeric(rhs_type)) {
                        compiler_error_range(ast_loc->line, op_column, ast_loc->end_column - 1,
                                           "cannot perform arithmetic on type %s",
                                           type_to_string(rhs_type));
                    }

                    // Check for incompatible type mixing
                    if ((result_type->kind == TYPE_HEX || result_type->kind == TYPE_BIN || result_type->kind == TYPE_OCT) &&
                        (rhs_type->kind == TYPE_HEX || rhs_type->kind == TYPE_BIN || rhs_type->kind == TYPE_OCT) &&
                        result_type->kind != rhs_type->kind) {
                        compiler_error_range(ast_loc->line, op_column, ast_loc->end_column - 1,
                                           "cannot mix %s and %s in arithmetic - ambiguous result type",
                                           type_to_string(result_type), type_to_string(rhs_type));
                    }

                    // Determine result type
                    // Float + anything = Float
                    // Int-like + Int-like of same type = that type
                    // Int + Hex/Bin/Oct = Int (special types cast to Int when mixed with Int)
                    // Char + anything integer = Int (chars promote to Int in arithmetic)
                    Type *new_result_type;
                    if (type_is_float(result_type) || type_is_float(rhs_type)) {
                        new_result_type = type_float();
                    } else if (result_type->kind == TYPE_CHAR || rhs_type->kind == TYPE_CHAR) {
                        // Char arithmetic promotes to Int (like C)
                        new_result_type = type_int();
                    } else if (result_type->kind == rhs_type->kind) {
                        new_result_type = result_type;
                    } else if (result_type->kind == TYPE_INT || rhs_type->kind == TYPE_INT) {
                        new_result_type = type_int();
                    } else {
                        // This shouldn't happen due to earlier check, but just in case
                        new_result_type = type_int();
                    }

                    // Convert operands if needed
                    LLVMValueRef lhs_converted = result;
                    LLVMValueRef rhs_converted = rhs;

                    if (type_is_float(new_result_type)) {
                        if (type_is_integer(result_type)) {
                            if (result_type->kind == TYPE_CHAR) {
                                // Extend char to i64 first, then to float
                                LLVMValueRef extended = LLVMBuildSExt(ctx->builder, result,
                                                                      LLVMInt64TypeInContext(ctx->context), "ext");
                                lhs_converted = LLVMBuildSIToFP(ctx->builder, extended,
                                                               LLVMDoubleTypeInContext(ctx->context), "tofloat");
                            } else {
                                lhs_converted = LLVMBuildSIToFP(ctx->builder, result,
                                                               LLVMDoubleTypeInContext(ctx->context), "tofloat");
                            }
                        }
                        if (type_is_integer(rhs_type)) {
                            if (rhs_type->kind == TYPE_CHAR) {
                                // Extend char to i64 first, then to float
                                LLVMValueRef extended = LLVMBuildSExt(ctx->builder, rhs,
                                                                      LLVMInt64TypeInContext(ctx->context), "ext");
                                rhs_converted = LLVMBuildSIToFP(ctx->builder, extended,
                                                               LLVMDoubleTypeInContext(ctx->context), "tofloat");
                            } else {
                                rhs_converted = LLVMBuildSIToFP(ctx->builder, rhs,
                                                               LLVMDoubleTypeInContext(ctx->context), "tofloat");
                            }
                        }
                    } else {
                        // Integer arithmetic - extend chars to i64
                        if (result_type->kind == TYPE_CHAR) {
                            lhs_converted = LLVMBuildSExt(ctx->builder, result,
                                                         LLVMInt64TypeInContext(ctx->context), "ext");
                        }
                        if (rhs_type->kind == TYPE_CHAR) {
                            rhs_converted = LLVMBuildSExt(ctx->builder, rhs,
                                                         LLVMInt64TypeInContext(ctx->context), "ext");
                        }
                    }

                    // Perform operation
                    if (type_is_float(new_result_type)) {
                        if (strcmp(op, "+") == 0) {
                            result = LLVMBuildFAdd(ctx->builder, lhs_converted, rhs_converted, "addtmp");
                        } else if (strcmp(op, "-") == 0) {
                            result = LLVMBuildFSub(ctx->builder, lhs_converted, rhs_converted, "subtmp");
                        } else if (strcmp(op, "*") == 0) {
                            result = LLVMBuildFMul(ctx->builder, lhs_converted, rhs_converted, "multmp");
                        } else if (strcmp(op, "/") == 0) {
                            result = LLVMBuildFDiv(ctx->builder, lhs_converted, rhs_converted, "divtmp");
                        }
                    } else {
                        // Integer arithmetic
                        if (strcmp(op, "+") == 0) {
                            result = LLVMBuildAdd(ctx->builder, lhs_converted, rhs_converted, "addtmp");
                        } else if (strcmp(op, "-") == 0) {
                            result = LLVMBuildSub(ctx->builder, lhs_converted, rhs_converted, "subtmp");
                        } else if (strcmp(op, "*") == 0) {
                            result = LLVMBuildMul(ctx->builder, lhs_converted, rhs_converted, "multmp");
                        } else if (strcmp(op, "/") == 0) {
                            result = LLVMBuildSDiv(ctx->builder, lhs_converted, rhs_converted, "divtmp");
                        }
                    }

                    result_type = new_result_type;
                }

                ast_loc->type = result_type;
                return result;
            }

            compiler_error_range(line, column + 1, ast_loc->end_column,
                               "unknown function: %s", head->symbol);
        }

        compiler_error(line, column, "function call requires symbol in head position");
    }

    default:
        compiler_error(line, column, "unknown AST type: %d", ast->type);
    }
}

/// Parser code (moved from reader.c for location tracking)

typedef struct {
    Lexer *lexer;
    Token current;
} Parser;

static void parser_init(Parser *p, Lexer *lex) {
    p->lexer = lex;
    p->current = lexer_next_token(lex);
}

static ASTWithLoc parse_expr(Parser *p);

static ASTWithLoc parse_list(Parser *p) {
    int start_line = p->current.line;
    int start_column = p->current.column;

    AST *list = ast_new_list();

    p->current = lexer_next_token(p->lexer);

    while (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF) {
        ASTWithLoc item = parse_expr(p);
        ast_list_append(list, item.ast);
    }

    if (p->current.type != TOK_RPAREN) {
        compiler_error(p->current.line, p->current.column, "expected ')'");
    }

    int end_column = p->current.column + 1;

    p->current = lexer_next_token(p->lexer);

    ASTWithLoc result = {list, start_line, start_column, end_column, NULL, NULL};
    return result;
}

static ASTWithLoc parse_bracket_list(Parser *p) {
    int start_line = p->current.line;
    int start_column = p->current.column;

    AST *list = ast_new_list();

    p->current = lexer_next_token(p->lexer);

    while (p->current.type != TOK_RBRACKET && p->current.type != TOK_EOF) {
        ASTWithLoc item = parse_expr(p);
        ast_list_append(list, item.ast);
    }

    if (p->current.type != TOK_RBRACKET) {
        compiler_error(p->current.line, p->current.column, "expected ']'");
    }

    int end_column = p->current.column + 1;

    p->current = lexer_next_token(p->lexer);

    ASTWithLoc result = {list, start_line, start_column, end_column, NULL, NULL};
    return result;
}

static ASTWithLoc parse_expr(Parser *p) {
    Token tok = p->current;

    switch (tok.type) {
    case TOK_NUMBER: {
        int end_col = tok.column + (tok.value ? strlen(tok.value) : 1);

        // Parse the number value
        double value;
        if (tok.value[0] == '0' && (tok.value[1] == 'x' || tok.value[1] == 'X')) {
            value = (double)strtol(tok.value, NULL, 16);
        } else if (tok.value[0] == '0' && (tok.value[1] == 'b' || tok.value[1] == 'B')) {
            value = (double)strtol(tok.value + 2, NULL, 2);
        } else if (tok.value[0] == '0' && (tok.value[1] == 'o' || tok.value[1] == 'O')) {
            value = (double)strtol(tok.value + 2, NULL, 8);
        } else {
            value = atof(tok.value);
        }

        p->current = lexer_next_token(p->lexer);

        // Store the literal string for type inference
        char *literal_str = tok.value ? strdup(tok.value) : NULL;
        ASTWithLoc result = {ast_new_number(value, tok.value), tok.line, tok.column, end_col, NULL, literal_str};
        return result;
    }

    case TOK_SYMBOL: {
        int end_col = tok.column + (tok.value ? strlen(tok.value) : 1);
        p->current = lexer_next_token(p->lexer);
        ASTWithLoc result = {ast_new_symbol(tok.value), tok.line, tok.column, end_col, NULL, NULL};
        return result;
    }

    case TOK_STRING: {
        int end_col = tok.column + (tok.value ? strlen(tok.value) : 1) + 2;
        p->current = lexer_next_token(p->lexer);
        ASTWithLoc result = {ast_new_string(tok.value), tok.line, tok.column, end_col, NULL, NULL};
        return result;
    }

    case TOK_CHAR: {
        int end_col = tok.column + 3;
        p->current = lexer_next_token(p->lexer);
        ASTWithLoc result = {ast_new_char(tok.value[0]), tok.line, tok.column, end_col, NULL, NULL};
        return result;
    }

    case TOK_LPAREN:
        return parse_list(p);

    case TOK_LBRACKET:
        return parse_bracket_list(p);

    case TOK_QUOTE: {
        int quote_line = tok.line;
        int quote_column = tok.column;
        p->current = lexer_next_token(p->lexer);
        ASTWithLoc quoted = parse_expr(p);
        AST *list = ast_new_list();
        ast_list_append(list, ast_new_symbol("quote"));
        ast_list_append(list, quoted.ast);
        ASTWithLoc result = {list, quote_line, quote_column, quoted.end_column, NULL, NULL};
        return result;
    }

    default:
        compiler_error(tok.line, tok.column, "unexpected token type: %d", tok.type);
    }
}

typedef struct {
    ASTWithLoc *exprs;
    size_t count;
} ASTList;

ASTList parse_all(const char *source) {
    Lexer lex;
    lexer_init(&lex, source);

    ASTList list = {0};
    list.exprs = NULL;
    list.count = 0;
    size_t capacity = 0;

    Parser p;
    parser_init(&p, &lex);

    while (p.current.type != TOK_EOF) {
        ASTWithLoc expr = parse_expr(&p);

        if (list.count >= capacity) {
            capacity = capacity == 0 ? 4 : capacity * 2;
            list.exprs = realloc(list.exprs, sizeof(ASTWithLoc) * capacity);
        }
        list.exprs[list.count++] = expr;
    }

    return list;
}

/// Compile

void compile(CompilerFlags *flags) {
    char *source = read_file(flags->input_file);

    current_filename = flags->input_file;
    current_source = source;

    ASTList exprs = parse_all(source);

    if (exprs.count == 0) {
        compiler_error(1, 1, "no expression(s) found");
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

    LLVMValueRef result = NULL;
    for (size_t i = 0; i < exprs.count; i++) {
        result = codegen_expr_loc(&ctx, &exprs.exprs[i]);
    }

    if (!result) {
        result = LLVMConstReal(LLVMDoubleTypeInContext(ctx.context), 0.0);
    }

    // Convert result to i32 based on its type
    LLVMValueRef result_i32;
    if (exprs.exprs[exprs.count - 1].type && type_is_integer(exprs.exprs[exprs.count - 1].type)) {
        result_i32 = LLVMBuildTrunc(ctx.builder, result,
                                    LLVMInt32TypeInContext(ctx.context), "result");
    } else {
        result_i32 = LLVMBuildFPToSI(ctx.builder, result,
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
        ast_free(exprs.exprs[i].ast);
        if (exprs.exprs[i].literal_str) {
            free(exprs.exprs[i].literal_str);
        }
    }
    free(exprs.exprs);
    free(source);
    codegen_dispose(&ctx);
}

int main(int argc, char **argv) {
    CompilerFlags flags = parse_flags(argc, argv);
    compile(&flags);
    return 0;
}
