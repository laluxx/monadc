#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "reader.h"
#include "cli.h"
#include "types.h"
#include "env.h"
#include "repl.h"
#include "features.h"
#include "runtime.h"
#include "codegen.h"

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

/// Compile

void compile(CompilerFlags *flags) {
    char *source = read_file(flags->input_file);

    // Set parser context for error reporting
    parser_set_context(flags->input_file, source);

    // Detect and register platform features BEFORE PARSING!
    AST *features_ast = detect_features();

    // Print detected features
    printf("Detected platform features: ");
    ast_print(features_ast);
    printf("\n\n");

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

    register_builtins(&ctx);
    declare_runtime_functions(&ctx);

    LLVMTypeRef main_type = LLVMFunctionType(LLVMInt32TypeInContext(ctx.context), NULL, 0, 0);
    LLVMValueRef main_fn = LLVMAddFunction(ctx.module, "main", main_type);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx.context, main_fn, "entry");
    LLVMPositionBuilderAtEnd(ctx.builder, entry);

    // Convert features AST to runtime list
    LLVMValueRef list_fn = get_rt_list_create(&ctx);
    LLVMValueRef features_list = LLVMBuildCall2(ctx.builder,
    LLVMGlobalGetValueType(list_fn), list_fn, NULL, 0, "features_list");

    // Append each feature keyword to the runtime list
    for (size_t i = 0; i < features_ast->list.count; i++) {
        AST *feature = features_ast->list.items[i];
        if (feature->type == AST_KEYWORD) {
            LLVMValueRef kw_fn = get_rt_value_keyword(&ctx);
            LLVMValueRef kw_str = LLVMBuildGlobalStringPtr(ctx.builder,
                                                           feature->keyword, "feat_kw");
            LLVMValueRef kw_args[] = {kw_str};
            LLVMValueRef rt_kw = LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(kw_fn),
                                                kw_fn, kw_args, 1, "feat_val");

            LLVMValueRef append_fn = get_rt_list_append(&ctx);
            LLVMValueRef append_args[] = {features_list, rt_kw};
            LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(append_fn),
                           append_fn, append_args, 2, "");
        }
    }

    // Register *features* in the environment
    Type *features_type = type_list(type_keyword());
    LLVMTypeRef features_llvm_type = type_to_llvm(&ctx, features_type);
    LLVMValueRef features_var = LLVMBuildAlloca(ctx.builder, features_llvm_type, "*features*");
    LLVMBuildStore(ctx.builder, features_list, features_var);
    env_insert(ctx.env, "*features*", features_type, features_var);

    // Clean up the AST
    ast_free(features_ast);


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

    // ALWAYS generate object file and executable
    // Flags just control additional outputs
    {
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

            // ALWAYS link to executable (even if --emit-obj was specified)

            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                     "gcc %s runtime.o -o %s `llvm-config --ldflags --libs "
                     "core` -lm -no-pie",
                     obj_file, base_name);

            int ret = system(cmd);
            if (ret == 0) {
                printf("Created executable: %s\n", base_name);
                // Only remove .o if user didn't request --emit-obj
                if (!flags->emit_obj) {
                    remove(obj_file);
                }
            } else {
                fprintf(stderr, "Failed to link executable\n");
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
