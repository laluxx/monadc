#include "repl.h"
#include "reader.h"
#include "types.h"
#include "env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <readline/readline.h>
#include <readline/history.h>

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>

// Global REPL context for readline completion
static REPLContext *global_repl_ctx = NULL;

/// Helper: Get or create format strings
/// These live as module-level globals so they survive across wrapper functions.

static LLVMValueRef get_fmt_str(REPLContext *ctx) {
    if (!ctx->fmt_str)
        ctx->fmt_str = LLVMBuildGlobalStringPtr(ctx->builder, "%s\n", "fmt_str");
    return ctx->fmt_str;
}

static LLVMValueRef get_fmt_char(REPLContext *ctx) {
    if (!ctx->fmt_char)
        ctx->fmt_char = LLVMBuildGlobalStringPtr(ctx->builder, "%c\n", "fmt_char");
    return ctx->fmt_char;
}

static LLVMValueRef get_fmt_int(REPLContext *ctx) {
    if (!ctx->fmt_int)
        ctx->fmt_int = LLVMBuildGlobalStringPtr(ctx->builder, "%ld\n", "fmt_int");
    return ctx->fmt_int;
}

static LLVMValueRef get_fmt_float(REPLContext *ctx) {
    if (!ctx->fmt_float)
        ctx->fmt_float = LLVMBuildGlobalStringPtr(ctx->builder, "%g\n", "fmt_float");
    return ctx->fmt_float;
}

/// Helper: Declare printf

static LLVMValueRef get_or_declare_printf(REPLContext *ctx) {
    LLVMValueRef fn = LLVMGetNamedFunction(ctx->module, "printf");
    if (fn) return fn;
    LLVMTypeRef args[]  = {LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0)};
    LLVMTypeRef fn_type = LLVMFunctionType(LLVMInt32TypeInContext(ctx->context), args, 1, true);
    return LLVMAddFunction(ctx->module, "printf", fn_type);
}

/// Type helpers

static bool type_is_numeric(Type *t) {
    return t->kind == TYPE_INT   || t->kind == TYPE_FLOAT ||
           t->kind == TYPE_HEX   || t->kind == TYPE_BIN   ||
           t->kind == TYPE_OCT   || t->kind == TYPE_CHAR;
}

static bool type_is_integer(Type *t) {
    return t->kind == TYPE_INT  || t->kind == TYPE_HEX ||
           t->kind == TYPE_BIN  || t->kind == TYPE_OCT ||
           t->kind == TYPE_CHAR;
}

static bool type_is_float(Type *t) {
    return t->kind == TYPE_FLOAT;
}

static LLVMTypeRef type_to_llvm(REPLContext *ctx, Type *type) {
    switch (type->kind) {
    case TYPE_INT: case TYPE_HEX: case TYPE_BIN: case TYPE_OCT:
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

/// ASTWithType

typedef struct {
    AST  *ast;
    Type *type;
    char *literal_str;
} ASTWithType;

// Forward declaration
static LLVMValueRef codegen_expr(REPLContext *ctx, ASTWithType *ast_typed);

/// Print an AST value at runtime (for quoted expressions)

static void codegen_print_ast(REPLContext *ctx, AST *ast) {
    LLVMValueRef printf_fn = get_or_declare_printf(ctx);

    switch (ast->type) {
    case AST_NUMBER: {
        LLVMValueRef num  = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), ast->number);
        LLVMValueRef args[] = {get_fmt_float(ctx), num};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        break;
    }
    case AST_SYMBOL: {
        LLVMValueRef sym  = LLVMBuildGlobalStringPtr(ctx->builder, ast->symbol, "sym");
        LLVMValueRef args[] = {get_fmt_str(ctx), sym};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        break;
    }
    case AST_STRING: {
        LLVMValueRef str  = LLVMBuildGlobalStringPtr(ctx->builder, ast->string, "str");
        LLVMValueRef fmt  = LLVMBuildGlobalStringPtr(ctx->builder, "\"%s\"\n", "fmt");
        LLVMValueRef args[] = {fmt, str};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        break;
    }
    case AST_CHAR: {
        LLVMValueRef ch   = LLVMConstInt(LLVMInt8TypeInContext(ctx->context), ast->character, 0);
        LLVMValueRef fmt  = LLVMBuildGlobalStringPtr(ctx->builder, "'%c'\n", "fmt");
        LLVMValueRef args[] = {fmt, ch};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        break;
    }
    case AST_LIST: {
        LLVMValueRef lp   = LLVMBuildGlobalStringPtr(ctx->builder, "(", "lp");
        LLVMValueRef largs[] = {lp};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, largs, 1, "");
        for (size_t i = 0; i < ast->list.count; i++) {
            if (i > 0) {
                LLVMValueRef sp = LLVMBuildGlobalStringPtr(ctx->builder, " ", "sp");
                LLVMValueRef sargs[] = {sp};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, sargs, 1, "");
            }
            codegen_print_ast(ctx, ast->list.items[i]);
        }
        LLVMValueRef rp   = LLVMBuildGlobalStringPtr(ctx->builder, ")\n", "rp");
        LLVMValueRef rargs[] = {rp};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, rargs, 1, "");
        break;
    }
    }
}

/// Check arity against env entry, print error and return false on mismatch.
/// argc = number of arguments (not counting the function name itself).

static bool check_arity(const char *name, EnvEntry *entry, size_t argc) {
    if (!entry) return true;  // unknown, let codegen handle it
    int min = entry->arity_min;
    int max = entry->arity_max;
    if (min < 0) return true; // not constrained
    if ((int)argc < min) {
        fprintf(stderr, "Error: '%s' requires at least %d argument(s), got %zu\n",
                name, min, argc);
        return false;
    }
    if (max >= 0 && (int)argc > max) {
        fprintf(stderr, "Error: '%s' requires at most %d argument(s), got %zu\n",
                name, max, argc);
        return false;
    }
    return true;
}

/// Code generation

static LLVMValueRef codegen_expr(REPLContext *ctx, ASTWithType *ast_typed) {
    AST *ast = ast_typed->ast;

    switch (ast->type) {

    case AST_NUMBER: {
        Type *num_type = infer_literal_type(ast->number, ast_typed->literal_str);
        ast_typed->type = num_type;
        if (type_is_float(num_type))
            return LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), ast->number);
        else
            return LLVMConstInt(LLVMInt64TypeInContext(ctx->context),
                                (long long)ast->number, 0);
    }

    case AST_CHAR: {
        ast_typed->type = type_char();
        return LLVMConstInt(LLVMInt8TypeInContext(ctx->context), ast->character, 0);
    }

    case AST_SYMBOL: {
        EnvEntry *entry = env_lookup(ctx->env, ast->symbol);
        if (!entry || entry->kind != ENV_VAR) {
            fprintf(stderr, "Error: unbound variable: %s\n", ast->symbol);
            return NULL;
        }
        ast_typed->type = entry->type;
        return LLVMBuildLoad2(ctx->builder,
                              type_to_llvm(ctx, entry->type),
                              entry->value, ast->symbol);
    }

    case AST_STRING: {
        ast_typed->type = type_string();
        return LLVMBuildGlobalStringPtr(ctx->builder, ast->string, "str");
    }

    case AST_LIST: {
        if (ast->list.count == 0) {
            fprintf(stderr, "Error: empty list\n");
            return NULL;
        }

        AST *head = ast->list.items[0];

        if (head->type != AST_SYMBOL) {
            fprintf(stderr, "Error: function call requires a symbol in head position\n");
            return NULL;
        }

        const char *sym  = head->symbol;
        size_t       argc = ast->list.count - 1;

        // ----------------------------------------------------------------
        // Special form: quote
        // ----------------------------------------------------------------
        if (strcmp(sym, "quote") == 0) {
            if (argc != 1) {
                fprintf(stderr, "Error: 'quote' requires exactly 1 argument\n");
                return NULL;
            }
            codegen_print_ast(ctx, ast->list.items[1]);
            ast_typed->type = type_float();
            return LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
        }

        // ----------------------------------------------------------------
        // Special form: define
        // ----------------------------------------------------------------
        if (strcmp(sym, "define") == 0) {
            if (argc < 2) {
                fprintf(stderr, "Error: 'define' requires at least 2 arguments\n");
                return NULL;
            }

            AST *name_expr  = ast->list.items[1];
            AST *value_expr = ast->list.items[2];

            // --- Variable definition: (define name value)
            //                      or (define [name :: Type] value)
            if (name_expr->type == AST_SYMBOL ||
                (name_expr->type == AST_LIST)) {

                char *var_name      = NULL;
                Type *explicit_type = NULL;

                if (name_expr->type == AST_LIST) {
                    explicit_type = parse_type_annotation(name_expr);
                    if (explicit_type && name_expr->list.count > 0 &&
                        name_expr->list.items[0]->type == AST_SYMBOL) {
                        var_name = name_expr->list.items[0]->symbol;
                    } else {
                        fprintf(stderr, "Error: bad type annotation in 'define'\n");
                        return NULL;
                    }
                } else {
                    var_name = name_expr->symbol;
                }

                if (!var_name) {
                    fprintf(stderr, "Error: 'define' invalid name\n");
                    return NULL;
                }

                ASTWithType val_typed = {value_expr, NULL, NULL};
                if (value_expr->type == AST_NUMBER && value_expr->literal_str)
                    val_typed.literal_str = strdup(value_expr->literal_str);

                LLVMValueRef value = codegen_expr(ctx, &val_typed);
                if (val_typed.literal_str) free(val_typed.literal_str);
                if (!value) return NULL;

                Type *inferred = val_typed.type;
                if (!inferred) {
                    if      (value_expr->type == AST_CHAR)   inferred = type_char();
                    else if (value_expr->type == AST_STRING) inferred = type_string();
                    else                                      inferred = type_float();
                }

                Type *final_type = explicit_type ? explicit_type : inferred;

                // Variables are stored as LLVM globals so they persist across
                // separately JIT-compiled wrapper functions.
                LLVMTypeRef  llvm_type = type_to_llvm(ctx, final_type);
                LLVMValueRef global    = LLVMGetNamedGlobal(ctx->module, var_name);
                if (!global) {
                    global = LLVMAddGlobal(ctx->module, llvm_type, var_name);
                    LLVMSetInitializer(global, LLVMConstNull(llvm_type));
                    LLVMSetLinkage(global, LLVMExternalLinkage);
                }

                // Type conversions
                LLVMValueRef stored = value;
                if (type_is_integer(final_type) && type_is_float(inferred)) {
                    stored = LLVMBuildFPToSI(ctx->builder, value,
                                             LLVMInt64TypeInContext(ctx->context), "toint");
                } else if (type_is_float(final_type) && type_is_integer(inferred)) {
                    stored = LLVMBuildSIToFP(ctx->builder, value,
                                             LLVMDoubleTypeInContext(ctx->context), "tofloat");
                } else if (final_type->kind == TYPE_CHAR && inferred->kind != TYPE_CHAR) {
                    if (type_is_float(inferred))
                        stored = LLVMBuildFPToSI(ctx->builder, value,
                                                 LLVMInt8TypeInContext(ctx->context), "tochar");
                    else if (type_is_integer(inferred))
                        stored = LLVMBuildTrunc(ctx->builder, value,
                                                LLVMInt8TypeInContext(ctx->context), "tochar");
                }

                LLVMBuildStore(ctx->builder, stored, global);
                env_insert(ctx->env, var_name, final_type, global);

                printf("%s :: %s\n", var_name, type_to_string(final_type));
                ast_typed->type = final_type;
                return stored;
            }

            fprintf(stderr, "Error: 'define' name must be a symbol or type annotation\n");
            return NULL;
        }

        // ----------------------------------------------------------------
        // Special form: show
        // ----------------------------------------------------------------
        if (strcmp(sym, "show") == 0) {
            EnvEntry *e = env_lookup(ctx->env, "show");
            if (!check_arity("show", e, argc)) return NULL;

            AST          *arg       = ast->list.items[1];
            LLVMValueRef  printf_fn = get_or_declare_printf(ctx);

            if (arg->type == AST_LIST && arg->list.count > 0 &&
                arg->list.items[0]->type == AST_SYMBOL &&
                strcmp(arg->list.items[0]->symbol, "quote") == 0) {
                if (arg->list.count == 2)
                    codegen_print_ast(ctx, arg->list.items[1]);
            } else if (arg->type == AST_STRING) {
                LLVMValueRef str  = LLVMBuildGlobalStringPtr(ctx->builder, arg->string, "str");
                LLVMValueRef args[] = {get_fmt_str(ctx), str};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                               printf_fn, args, 2, "");
            } else if (arg->type == AST_CHAR) {
                LLVMValueRef ch   = LLVMConstInt(LLVMInt8TypeInContext(ctx->context),
                                                  arg->character, 0);
                LLVMValueRef args[] = {get_fmt_char(ctx), ch};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                               printf_fn, args, 2, "");
            } else if (arg->type == AST_SYMBOL) {
                EnvEntry *entry = env_lookup(ctx->env, arg->symbol);
                if (!entry || entry->kind != ENV_VAR) {
                    fprintf(stderr, "Error: unbound variable: %s\n", arg->symbol);
                    return NULL;
                }
                LLVMValueRef loaded = LLVMBuildLoad2(ctx->builder,
                                                      type_to_llvm(ctx, entry->type),
                                                      entry->value, arg->symbol);
                LLVMValueRef fmt_args[2];
                if (entry->type->kind == TYPE_CHAR) {
                    fmt_args[0] = get_fmt_char(ctx);
                } else if (entry->type->kind == TYPE_STRING) {
                    fmt_args[0] = get_fmt_str(ctx);
                } else if (type_is_integer(entry->type)) {
                    fmt_args[0] = get_fmt_int(ctx);
                } else {
                    fmt_args[0] = get_fmt_float(ctx);
                }
                fmt_args[1] = loaded;
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                               printf_fn, fmt_args, 2, "");
            } else {
                ASTWithType arg_typed = {arg, NULL, NULL};
                LLVMValueRef result   = codegen_expr(ctx, &arg_typed);
                if (!result) return NULL;
                LLVMValueRef fmt_args[2];
                if (arg_typed.type && type_is_integer(arg_typed.type))
                    fmt_args[0] = get_fmt_int(ctx);
                else
                    fmt_args[0] = get_fmt_float(ctx);
                fmt_args[1] = result;
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                               printf_fn, fmt_args, 2, "");
            }

            ast_typed->type = type_float();
            return LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
        }

        // ----------------------------------------------------------------
        // Arithmetic builtins: + - * /
        // ----------------------------------------------------------------
        if (strcmp(sym, "+") == 0 || strcmp(sym, "-") == 0 ||
            strcmp(sym, "*") == 0 || strcmp(sym, "/") == 0) {

            EnvEntry *e = env_lookup(ctx->env, sym);
            if (!check_arity(sym, e, argc)) return NULL;

            // First operand
            ASTWithType first_typed = {ast->list.items[1], NULL, NULL};
            if (ast->list.items[1]->type == AST_NUMBER) {
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "%g", ast->list.items[1]->number);
                first_typed.literal_str = strdup(tmp);
            }
            LLVMValueRef result = codegen_expr(ctx, &first_typed);
            if (first_typed.literal_str) free(first_typed.literal_str);
            if (!result) return NULL;

            Type *result_type = first_typed.type;
            if (!type_is_numeric(result_type)) {
                fprintf(stderr, "Error: cannot perform arithmetic on type %s\n",
                        type_to_string(result_type));
                return NULL;
            }

            // Unary minus
            if (strcmp(sym, "-") == 0 && argc == 1) {
                ast_typed->type = result_type;
                if (type_is_float(result_type))
                    return LLVMBuildFNeg(ctx->builder, result, "negtmp");
                else {
                    LLVMValueRef zero = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                    return LLVMBuildSub(ctx->builder, zero, result, "negtmp");
                }
            }

            // Unary reciprocal
            if (strcmp(sym, "/") == 0 && argc == 1) {
                if (type_is_float(result_type)) {
                    LLVMValueRef one = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 1.0);
                    ast_typed->type  = result_type;
                    return LLVMBuildFDiv(ctx->builder, one, result, "invtmp");
                } else {
                    LLVMValueRef rf  = LLVMBuildSIToFP(ctx->builder, result,
                                                        LLVMDoubleTypeInContext(ctx->context), "tof");
                    LLVMValueRef one = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 1.0);
                    ast_typed->type  = type_float();
                    return LLVMBuildFDiv(ctx->builder, one, rf, "invtmp");
                }
            }

            // Binary fold
            for (size_t i = 2; i <= argc; i++) {
                ASTWithType rhs_typed = {ast->list.items[i], NULL, NULL};
                if (ast->list.items[i]->type == AST_NUMBER) {
                    char tmp[64];
                    snprintf(tmp, sizeof(tmp), "%g", ast->list.items[i]->number);
                    rhs_typed.literal_str = strdup(tmp);
                }
                LLVMValueRef rhs = codegen_expr(ctx, &rhs_typed);
                if (rhs_typed.literal_str) free(rhs_typed.literal_str);
                if (!rhs) return NULL;

                Type *rhs_type = rhs_typed.type;
                if (!type_is_numeric(rhs_type)) {
                    fprintf(stderr, "Error: cannot perform arithmetic on type %s\n",
                            type_to_string(rhs_type));
                    return NULL;
                }

                // Forbid mixing distinct special integer bases
                if ((result_type->kind == TYPE_HEX || result_type->kind == TYPE_BIN ||
                     result_type->kind == TYPE_OCT) &&
                    (rhs_type->kind   == TYPE_HEX || rhs_type->kind   == TYPE_BIN ||
                     rhs_type->kind   == TYPE_OCT) &&
                    result_type->kind != rhs_type->kind) {
                    fprintf(stderr, "Error: cannot mix %s and %s in arithmetic\n",
                            type_to_string(result_type), type_to_string(rhs_type));
                    return NULL;
                }

                // Determine promoted type
                Type *new_type;
                if (type_is_float(result_type) || type_is_float(rhs_type))
                    new_type = type_float();
                else if (result_type->kind == TYPE_CHAR || rhs_type->kind == TYPE_CHAR)
                    new_type = type_int();
                else if (result_type->kind == rhs_type->kind)
                    new_type = result_type;
                else
                    new_type = type_int();

                // Widen operands
                LLVMValueRef lv = result, rv = rhs;
                if (type_is_float(new_type)) {
                    if (type_is_integer(result_type)) {
                        if (result_type->kind == TYPE_CHAR) {
                            LLVMValueRef ext = LLVMBuildSExt(ctx->builder, result,
                                                              LLVMInt64TypeInContext(ctx->context), "ext");
                            lv = LLVMBuildSIToFP(ctx->builder, ext,
                                                  LLVMDoubleTypeInContext(ctx->context), "tof");
                        } else {
                            lv = LLVMBuildSIToFP(ctx->builder, result,
                                                  LLVMDoubleTypeInContext(ctx->context), "tof");
                        }
                    }
                    if (type_is_integer(rhs_type)) {
                        if (rhs_type->kind == TYPE_CHAR) {
                            LLVMValueRef ext = LLVMBuildSExt(ctx->builder, rhs,
                                                              LLVMInt64TypeInContext(ctx->context), "ext");
                            rv = LLVMBuildSIToFP(ctx->builder, ext,
                                                  LLVMDoubleTypeInContext(ctx->context), "tof");
                        } else {
                            rv = LLVMBuildSIToFP(ctx->builder, rhs,
                                                  LLVMDoubleTypeInContext(ctx->context), "tof");
                        }
                    }
                } else {
                    if (result_type->kind == TYPE_CHAR)
                        lv = LLVMBuildSExt(ctx->builder, result,
                                           LLVMInt64TypeInContext(ctx->context), "ext");
                    if (rhs_type->kind == TYPE_CHAR)
                        rv = LLVMBuildSExt(ctx->builder, rhs,
                                           LLVMInt64TypeInContext(ctx->context), "ext");
                }

                // Emit instruction
                if (type_is_float(new_type)) {
                    if      (strcmp(sym, "+") == 0) result = LLVMBuildFAdd(ctx->builder, lv, rv, "add");
                    else if (strcmp(sym, "-") == 0) result = LLVMBuildFSub(ctx->builder, lv, rv, "sub");
                    else if (strcmp(sym, "*") == 0) result = LLVMBuildFMul(ctx->builder, lv, rv, "mul");
                    else                            result = LLVMBuildFDiv(ctx->builder, lv, rv, "div");
                } else {
                    if      (strcmp(sym, "+") == 0) result = LLVMBuildAdd (ctx->builder, lv, rv, "add");
                    else if (strcmp(sym, "-") == 0) result = LLVMBuildSub (ctx->builder, lv, rv, "sub");
                    else if (strcmp(sym, "*") == 0) result = LLVMBuildMul (ctx->builder, lv, rv, "mul");
                    else                            result = LLVMBuildSDiv(ctx->builder, lv, rv, "div");
                }
                result_type = new_type;
            }

            ast_typed->type = result_type;
            return result;
        }

        // ----------------------------------------------------------------
        // User-defined function call
        // ----------------------------------------------------------------
        {
            EnvEntry *entry = env_lookup(ctx->env, sym);
            if (!entry || entry->kind != ENV_FUNC) {
                fprintf(stderr, "Error: unknown function: %s\n", sym);
                return NULL;
            }
            if (!check_arity(sym, entry, argc)) return NULL;

            LLVMValueRef *arg_vals = malloc(sizeof(LLVMValueRef) * argc);
            for (size_t i = 0; i < argc; i++) {
                ASTWithType at = {ast->list.items[i + 1], NULL, NULL};
                arg_vals[i]    = codegen_expr(ctx, &at);
                if (!arg_vals[i]) { free(arg_vals); return NULL; }
            }

            LLVMTypeRef  fn_type = LLVMGlobalGetValueType(entry->func_ref);
            LLVMValueRef call    = LLVMBuildCall2(ctx->builder, fn_type,
                                                   entry->func_ref,
                                                   arg_vals, (unsigned)argc, "call");
            free(arg_vals);
            ast_typed->type = entry->return_type;
            return call;
        }
    } // AST_LIST

    default:
        fprintf(stderr, "Error: unknown AST node type %d\n", ast->type);
        return NULL;
    }
}

/// Register built-in operators in the environment

static void register_builtins(REPLContext *ctx) {
    // Arithmetic: 1 arg (unary) up to unlimited args
    env_insert_builtin(ctx->env, "+", 1, -1);
    env_insert_builtin(ctx->env, "-", 1, -1);
    env_insert_builtin(ctx->env, "*", 1, -1);
    env_insert_builtin(ctx->env, "/", 1, -1);
    // show: exactly 1 arg
    env_insert_builtin(ctx->env, "show",  1,  1);
    // quote: exactly 1 arg
    env_insert_builtin(ctx->env, "quote", 1,  1);
    // define is a special form — mark it so completion finds it
    env_insert_builtin(ctx->env, "define", 2, -1);
}

/// REPL Initialization

void repl_init(REPLContext *ctx) {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    LLVMLinkInMCJIT();

    ctx->context = LLVMContextCreate();
    ctx->module  = LLVMModuleCreateWithNameInContext("repl_module", ctx->context);
    ctx->builder = LLVMCreateBuilderInContext(ctx->context);
    ctx->env     = env_create();

    ctx->fmt_str = ctx->fmt_char = ctx->fmt_int = ctx->fmt_float = NULL;
    ctx->expr_count = 0;

    char *error = NULL;
    if (LLVMCreateExecutionEngineForModule(&ctx->engine, ctx->module, &error) != 0) {
        fprintf(stderr, "Failed to create execution engine: %s\n", error);
        LLVMDisposeMessage(error);
        exit(1);
    }

    // We need a dummy builder position so format string globals can be emitted
    // before any wrapper function exists.  Create a tiny init function, emit
    // globals there, then we can re-position per wrapper.
    LLVMTypeRef  init_type = LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), NULL, 0, 0);
    LLVMValueRef init_fn   = LLVMAddFunction(ctx->module, "__repl_init_globals", init_type);
    LLVMBasicBlockRef init_bb = LLVMAppendBasicBlockInContext(ctx->context, init_fn, "entry");
    LLVMPositionBuilderAtEnd(ctx->builder, init_bb);

    // Force format strings to be created now (into the module as globals)
    get_fmt_str(ctx);
    get_fmt_char(ctx);
    get_fmt_int(ctx);
    get_fmt_float(ctx);

    LLVMBuildRetVoid(ctx->builder);

    register_builtins(ctx);

    printf("Monad REPL v0.1\n");
    printf("Type expressions to evaluate. Use Ctrl-D to exit.\n\n");
}

/// REPL Cleanup

void repl_dispose(REPLContext *ctx) {
    LLVMDisposeExecutionEngine(ctx->engine);
    LLVMDisposeBuilder(ctx->builder);
    LLVMContextDispose(ctx->context);
    env_free(ctx->env);
}

/// Evaluate one line

bool repl_eval_line(REPLContext *ctx, const char *line) {
    if (!line || !*line) return true;

    const char *p = line;
    while (*p && isspace(*p)) p++;
    if (!*p) return true;

    AST *ast = parse(line);
    if (!ast) {
        fprintf(stderr, "Error: failed to parse expression\n");
        return false;
    }

    bool should_print =
        !(ast->type == AST_LIST && ast->list.count > 0 &&
          ast->list.items[0]->type == AST_SYMBOL &&
          (strcmp(ast->list.items[0]->symbol, "define") == 0 ||
           strcmp(ast->list.items[0]->symbol, "show")   == 0));

    // Build a self-contained wrapper function for this expression
    char func_name[64];
    snprintf(func_name, sizeof(func_name), "__repl_expr_%u", ctx->expr_count);

    LLVMTypeRef  void_type   = LLVMVoidTypeInContext(ctx->context);
    LLVMTypeRef  func_type   = LLVMFunctionType(void_type, NULL, 0, 0);
    LLVMValueRef wrapper_fn  = LLVMAddFunction(ctx->module, func_name, func_type);
    LLVMBasicBlockRef bb     = LLVMAppendBasicBlockInContext(ctx->context, wrapper_fn, "entry");
    LLVMPositionBuilderAtEnd(ctx->builder, bb);

    ASTWithType ast_typed = {ast, NULL, NULL};
    if (ast->type == AST_NUMBER && ast->literal_str)
        ast_typed.literal_str = strdup(ast->literal_str);

    LLVMValueRef result = codegen_expr(ctx, &ast_typed);
    if (ast_typed.literal_str) free(ast_typed.literal_str);

    if (!result) {
        ast_free(ast);
        LLVMDeleteFunction(wrapper_fn);
        return false;
    }

    if (should_print) {
        LLVMValueRef printf_fn = get_or_declare_printf(ctx);
        Type        *rt        = ast_typed.type ? ast_typed.type : type_float();

        LLVMValueRef fmt_args[2];
        if (rt->kind == TYPE_CHAR) {
            fmt_args[0] = get_fmt_char(ctx);
        } else if (rt->kind == TYPE_STRING) {
            fmt_args[0] = get_fmt_str(ctx);
        } else if (type_is_integer(rt)) {
            fmt_args[0] = get_fmt_int(ctx);
        } else {
            fmt_args[0] = get_fmt_float(ctx);
        }
        fmt_args[1] = result;
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                       printf_fn, fmt_args, 2, "");
    }

    LLVMBuildRetVoid(ctx->builder);   // terminator — always required

    if (LLVMVerifyFunction(wrapper_fn, LLVMPrintMessageAction) != 0) {
        fprintf(stderr, "Error: IR verification failed\n");
        ast_free(ast);
        return false;
    }

    void (*func_ptr)(void) =
        (void (*)(void))LLVMGetFunctionAddress(ctx->engine, func_name);

    if (!func_ptr) {
        fprintf(stderr, "Error: failed to get function address for %s\n", func_name);
        ast_free(ast);
        return false;
    }

    func_ptr();

    ast_free(ast);
    ctx->expr_count++;
    return true;
}

/// Readline completion

char *repl_completion_generator(const char *text, int state) {
    static size_t    bucket_index;
    static EnvEntry *current_entry;
    static size_t    len;

    if (!global_repl_ctx || !global_repl_ctx->env) return NULL;

    if (!state) {
        bucket_index  = 0;
        current_entry = NULL;
        len           = strlen(text);
    }

    Env *env = global_repl_ctx->env;

    if (current_entry)
        current_entry = current_entry->next;

    while (bucket_index < env->size) {
        if (!current_entry) {
            current_entry = env->buckets[bucket_index];
            if (!current_entry) { bucket_index++; continue; }
        }
        while (current_entry) {
            const char *name = current_entry->name;
            if (strncmp(name, text, len) == 0) {
                char *r = strdup(name);
                current_entry = current_entry->next;
                return r;
            }
            current_entry = current_entry->next;
        }
        bucket_index++;
    }

    // Type keywords not already in the env
    static const char *keywords[] = {
        "Int", "Float", "Char", "String", "Hex", "Bin", "Oct", "Bool", NULL
    };
    static int kw_idx;
    if (!state) kw_idx = 0;
    while (keywords[kw_idx]) {
        const char *kw = keywords[kw_idx++];
        if (strncmp(kw, text, len) == 0)
            return strdup(kw);
    }

    return NULL;
}

char **repl_completion(const char *text, int start, int end) {
    (void)start; (void)end;
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, repl_completion_generator);
}

/// Main loop

void repl_run(void) {
    REPLContext ctx;
    repl_init(&ctx);

    global_repl_ctx = &ctx;
    rl_attempted_completion_function = repl_completion;

    char *line;
    while ((line = readline("monad> ")) != NULL) {
        if (*line) add_history(line);
        repl_eval_line(&ctx, line);
        free(line);
    }

    printf("\n");
    repl_dispose(&ctx);
}
