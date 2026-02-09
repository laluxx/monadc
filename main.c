#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include "reader.h"
#include "cli.h"

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

    // Print the source line
    if (current_source) {
        const char *line_start = current_source;
        int current_line = 1;

        // Find the start of the error line
        while (current_line < line && *line_start) {
            if (*line_start == '\n') current_line++;
            line_start++;
        }

        // Print the line
        const char *line_end = line_start;
        while (*line_end && *line_end != '\n') line_end++;

        fprintf(stderr, "%5d | %.*s\n", line, (int)(line_end - line_start), line_start);

        // Print the squiggle indicator
        fprintf(stderr, "      | ");
        for (int i = 1; i < column; i++) {
            fprintf(stderr, " ");
        }

        // Print squiggles for the range
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

    // Print the source line
    if (current_source) {
        const char *line_start = current_source;
        int current_line = 1;

        // Find the start of the error line
        while (current_line < line && *line_start) {
            if (*line_start == '\n') current_line++;
            line_start++;
        }

        // Print the line
        const char *line_end = line_start;
        while (*line_end && *line_end != '\n') line_end++;

        fprintf(stderr, "%5d | %.*s\n", line, (int)(line_end - line_start), line_start);

        // Print the caret indicator
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
} CodegenContext;

void codegen_init(CodegenContext *ctx, const char *module_name) {
    ctx->context = LLVMContextCreate();
    ctx->module = LLVMModuleCreateWithNameInContext(module_name, ctx->context);
    ctx->builder = LLVMCreateBuilderInContext(ctx->context);
}

void codegen_dispose(CodegenContext *ctx) {
    LLVMDisposeBuilder(ctx->builder);
    LLVMDisposeModule(ctx->module);
    LLVMContextDispose(ctx->context);
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
        true  // variadic
    );
    printf_fn = LLVMAddFunction(ctx->module, "printf", printf_type);
    return printf_fn;
}

/// AST with location tracking
typedef struct {
    AST *ast;
    int line;
    int column;
    int end_column;  // For multi-char expressions
} ASTWithLoc;

/// Calculate the end column of an expression
int calculate_end_column(const char *source, int line, int start_column) {
    const char *line_start = source;
    int current_line = 1;

    // Find the start of the line
    while (current_line < line && *line_start) {
        if (*line_start == '\n') current_line++;
        line_start++;
    }

    // Move to start_column position
    const char *pos = line_start;
    int col = 1;
    while (col < start_column && *pos && *pos != '\n') {
        pos++;
        col++;
    }

    if (*pos == '(') {
        // Find matching closing paren
        int depth = 1;
        pos++;
        col++;
        while (depth > 0 && *pos && *pos != '\n') {
            if (*pos == '(') depth++;
            if (*pos == ')') depth--;
            pos++;
            col++;
        }
        return col;
    } else if (*pos == '\'') {
        // Quote
        pos++;
        col++;
        // Skip the quoted expression (simplified - just to next space or paren)
        while (*pos && *pos != ' ' && *pos != '\n' && *pos != ')') {
            pos++;
            col++;
        }
        return col;
    } else {
        // Symbol or number - read to next space or delimiter
        while (*pos && *pos != ' ' && *pos != '\n' && *pos != ')' && *pos != '(') {
            pos++;
            col++;
        }
        return col;
    }
}

/// Forward declaration
LLVMValueRef codegen_expr_loc(CodegenContext *ctx, ASTWithLoc *ast_loc);

/// Helper to print an AST at runtime (for quoted expressions)
void codegen_print_ast(CodegenContext *ctx, AST *ast) {
    LLVMValueRef printf_fn = get_or_declare_printf(ctx);

    switch (ast->type) {
    case AST_NUMBER: {
        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "%g", "fmt");
        LLVMValueRef num = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), ast->number);
        LLVMValueRef args[] = {fmt, num};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        break;
    }

    case AST_SYMBOL: {
        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "%s", "fmt");
        LLVMValueRef sym = LLVMBuildGlobalStringPtr(ctx->builder, ast->symbol, "sym");
        LLVMValueRef args[] = {fmt, sym};
        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
        break;
    }

    case AST_STRING: {
        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "\"%s\"", "fmt");
        LLVMValueRef str = LLVMBuildGlobalStringPtr(ctx->builder, ast->string, "str");
        LLVMValueRef args[] = {fmt, str};
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

        LLVMValueRef rparen = LLVMBuildGlobalStringPtr(ctx->builder, ")", "rparen");
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
        return LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), ast->number);
    }

    case AST_SYMBOL: {
        compiler_error(line, column, "unbound variable: %s", ast->symbol);
    }

    case AST_STRING: {
        return LLVMBuildGlobalStringPtr(ctx->builder, ast->string, "str");
    }

    case AST_LIST: {
        if (ast->list.count == 0) {
            compiler_error(line, column, "empty list not supported");
        }

        AST *head = ast->list.items[0];

        if (head->type == AST_SYMBOL) {
            // Handle 'show' function
            if (strcmp(head->symbol, "show") == 0) {
                if (ast->list.count != 2) {
                    // Point at 'show' symbol (column + 1 for opening paren)
                    compiler_error_range(line, column + 1, ast_loc->end_column,
                                       "'show' requires 1 argument, got %zu", ast->list.count - 1);
                }

                ASTWithLoc arg_loc = {ast->list.items[1], line, column, ast_loc->end_column};
                AST *arg = ast->list.items[1];
                LLVMValueRef printf_fn = get_or_declare_printf(ctx);

                // Check if argument is a quoted expression
                if (arg->type == AST_LIST && arg->list.count > 0 &&
                    arg->list.items[0]->type == AST_SYMBOL &&
                    strcmp(arg->list.items[0]->symbol, "quote") == 0) {
                    // Print the quoted expression without evaluating
                    if (arg->list.count == 2) {
                        codegen_print_ast(ctx, arg->list.items[1]);
                    }
                } else if (arg->type == AST_STRING) {
                    // Print string directly
                    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "%s", "fmt");
                    LLVMValueRef str = LLVMBuildGlobalStringPtr(ctx->builder, arg->string, "str");
                    LLVMValueRef args[] = {fmt, str};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                } else {
                    // Evaluate and print the result
                    LLVMValueRef result = codegen_expr_loc(ctx, &arg_loc);
                    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder, "%g", "fmt");
                    LLVMValueRef args[] = {fmt, result};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, args, 2, "");
                }

                // Print newline
                LLVMValueRef newline = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "newline");
                LLVMValueRef nl_args[] = {newline};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn), printf_fn, nl_args, 1, "");

                // Return 0.0 as a dummy value
                return LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
            }

            // Variadic arithmetic operators
            if (strcmp(head->symbol, "+") == 0) {
                if (ast->list.count < 2) {
                    // Point at '+' symbol (column + 1 for opening paren)
                    compiler_error_range(line, column + 1, ast_loc->end_column,
                                       "'+' requires at least 1 argument");
                }

                ASTWithLoc first_loc = {ast->list.items[1], line, column, ast_loc->end_column};
                LLVMValueRef result = codegen_expr_loc(ctx, &first_loc);

                for (size_t i = 2; i < ast->list.count; i++) {
                    ASTWithLoc arg_loc = {ast->list.items[i], line, column, ast_loc->end_column};
                    LLVMValueRef rhs = codegen_expr_loc(ctx, &arg_loc);
                    result = LLVMBuildFAdd(ctx->builder, result, rhs, "addtmp");
                }
                return result;
            }

            if (strcmp(head->symbol, "-") == 0) {
                if (ast->list.count < 2) {
                    compiler_error_range(line, column + 1, ast_loc->end_column,
                                       "'-' requires at least 1 argument");
                }

                ASTWithLoc first_loc = {ast->list.items[1], line, column, ast_loc->end_column};
                LLVMValueRef result = codegen_expr_loc(ctx, &first_loc);

                if (ast->list.count == 2) {
                    // Unary negation
                    return LLVMBuildFNeg(ctx->builder, result, "negtmp");
                }

                for (size_t i = 2; i < ast->list.count; i++) {
                    ASTWithLoc arg_loc = {ast->list.items[i], line, column, ast_loc->end_column};
                    LLVMValueRef rhs = codegen_expr_loc(ctx, &arg_loc);
                    result = LLVMBuildFSub(ctx->builder, result, rhs, "subtmp");
                }
                return result;
            }

            if (strcmp(head->symbol, "*") == 0) {
                if (ast->list.count < 2) {
                    compiler_error_range(line, column + 1, ast_loc->end_column,
                                       "'*' requires at least 1 argument");
                }

                ASTWithLoc first_loc = {ast->list.items[1], line, column, ast_loc->end_column};
                LLVMValueRef result = codegen_expr_loc(ctx, &first_loc);

                for (size_t i = 2; i < ast->list.count; i++) {
                    ASTWithLoc arg_loc = {ast->list.items[i], line, column, ast_loc->end_column};
                    LLVMValueRef rhs = codegen_expr_loc(ctx, &arg_loc);
                    result = LLVMBuildFMul(ctx->builder, result, rhs, "multmp");
                }
                return result;
            }

            if (strcmp(head->symbol, "/") == 0) {
                if (ast->list.count < 2) {
                    compiler_error_range(line, column + 1, ast_loc->end_column,
                                       "'/' requires at least 1 argument");
                }

                ASTWithLoc first_loc = {ast->list.items[1], line, column, ast_loc->end_column};
                LLVMValueRef result = codegen_expr_loc(ctx, &first_loc);

                if (ast->list.count == 2) {
                    // 1/x
                    LLVMValueRef one = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 1.0);
                    return LLVMBuildFDiv(ctx->builder, one, result, "invtmp");
                }

                for (size_t i = 2; i < ast->list.count; i++) {
                    ASTWithLoc arg_loc = {ast->list.items[i], line, column, ast_loc->end_column};
                    LLVMValueRef rhs = codegen_expr_loc(ctx, &arg_loc);
                    result = LLVMBuildFDiv(ctx->builder, result, rhs, "divtmp");
                }
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

/// Parse all expressions in file

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

    int end_line = p->current.line;
    int end_column = p->current.column + 1; // +1 for the closing paren

    p->current = lexer_next_token(p->lexer);

    ASTWithLoc result = {list, start_line, start_column, end_column};
    return result;
}

static ASTWithLoc parse_expr(Parser *p) {
    Token tok = p->current;

    switch (tok.type) {
    case TOK_NUMBER: {
        int end_col = tok.column + (tok.value ? strlen(tok.value) : 1);
        p->current = lexer_next_token(p->lexer);
        ASTWithLoc result = {ast_new_number(atof(tok.value)), tok.line, tok.column, end_col};
        return result;
    }

    case TOK_SYMBOL: {
        int end_col = tok.column + (tok.value ? strlen(tok.value) : 1);
        p->current = lexer_next_token(p->lexer);
        ASTWithLoc result = {ast_new_symbol(tok.value), tok.line, tok.column, end_col};
        return result;
    }

    case TOK_STRING: {
        int end_col = tok.column + (tok.value ? strlen(tok.value) : 1) + 2; // +2 for quotes
        p->current = lexer_next_token(p->lexer);
        ASTWithLoc result = {ast_new_string(tok.value), tok.line, tok.column, end_col};
        return result;
    }

    case TOK_LPAREN:
        return parse_list(p);

    case TOK_QUOTE: {
        int quote_line = tok.line;
        int quote_column = tok.column;
        p->current = lexer_next_token(p->lexer);
        ASTWithLoc quoted = parse_expr(p);
        AST *list = ast_new_list();
        ast_list_append(list, ast_new_symbol("quote"));
        ast_list_append(list, quoted.ast);
        ASTWithLoc result = {list, quote_line, quote_column, quoted.end_column};
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

    // Set global error reporting context
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

    // Create main function that returns i32
    LLVMTypeRef main_type = LLVMFunctionType(LLVMInt32TypeInContext(ctx.context), NULL, 0, 0);
    LLVMValueRef main_fn = LLVMAddFunction(ctx.module, "main", main_type);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx.context, main_fn, "entry");
    LLVMPositionBuilderAtEnd(ctx.builder, entry);

    // Codegen all expressions, keep only the last result
    LLVMValueRef result = NULL;
    for (size_t i = 0; i < exprs.count; i++) {
        result = codegen_expr_loc(&ctx, &exprs.exprs[i]);
    }

    // Convert double to i32 and return
    LLVMValueRef result_i32 = LLVMBuildFPToSI(ctx.builder, result,
                                               LLVMInt32TypeInContext(ctx.context), "result");
    LLVMBuildRet(ctx.builder, result_i32);

    char *error = NULL;
    LLVMVerifyModule(ctx.module, LLVMAbortProcessAction, &error);
    LLVMDisposeMessage(error);

    char *base_name = flags->output_name ? strdup(flags->output_name) : get_base_executable_name(flags->input_file);

    // Emit IR
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

    // Emit bitcode
    if (flags->emit_bc) {
        char bc_file[256];
        snprintf(bc_file, sizeof(bc_file), "%s.bc", base_name);
        if (LLVMWriteBitcodeToFile(ctx.module, bc_file) != 0) {
            fprintf(stderr, "Failed to write bitcode\n");
        } else {
            printf("Wrote bitcode to %s\n", bc_file);
        }
    }

    // Emit object/asm/executable (default)
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

            // Link to executable (default behavior)
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

    free(base_name);
    for (size_t i = 0; i < exprs.count; i++) {
        ast_free(exprs.exprs[i].ast);
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
