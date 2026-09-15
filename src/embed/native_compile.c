#include "native_compile.h"

#include "../codegen.h"
#include "../runtime.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Native source loading uses ORCv2's explicit JIT/resource lifetime rather
 * than an interpreter or a subprocess compiler:
 * https://llvm.org/docs/ORCv2.html
 * The generated entry points use the platform C ABI, so a resolved hot call
 * has precisely the same indirect-call shape as a C function pointer:
 * https://llvm.org/docs/LangRef.html#calling-conventions
 */

typedef struct {
    char *name;
    char *docstring;
    void (*address)(void);
} MonadNativeEntry;

struct MonadNativeImage {
    LLVMOrcLLJITRef jit;
    LLVMOrcThreadSafeContextRef thread_context;
    MonadNativeEntry *entries;
    size_t count;
};

static char *native_copy(const char *text) {
    if (!text) return NULL;
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static void dispose_llvm_error(LLVMErrorRef error) {
    if (!error) return;
    char *message = LLVMGetErrorMessage(error);
    LLVMDisposeErrorMessage(message);
}

bool monad_native_image_build(
    const MonadCompilationUnit *unit, MonadNativeImage **output,
    const char **failed_definition) {
    if (output) *output = NULL;
    if (failed_definition) *failed_definition = NULL;
    size_t count = monad_compilation_unit_definition_count(unit);
    if (!output || !unit || !count) return false;
    MonadNativeImage *image = calloc(1, sizeof(*image));
    if (!image) return false;
    image->entries = calloc(count, sizeof(*image->entries));
    image->count = count;
    if (!image->entries) {
        monad_native_image_destroy(image);
        return false;
    }
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    CodegenContext production;
    codegen_init(&production, "monad.embedded.source");
    register_builtins(&production);
    declare_runtime_functions(&production);
    LLVMTypeRef init_type = LLVMFunctionType(
        LLVMVoidTypeInContext(production.context), NULL, 0, 0);
    production.init_fn = LLVMAddFunction(
        production.module, "__monad_embedded_init", init_type);
    LLVMBasicBlockRef init_entry = LLVMAppendBasicBlockInContext(
        production.context, production.init_fn, "entry");
    LLVMPositionBuilderAtEnd(production.builder, init_entry);
    AST **definitions = calloc(count, sizeof(*definitions));
    if (!definitions) {
        codegen_dispose(&production);
        monad_native_image_destroy(image);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        const AST *definition = NULL;
        monad_compilation_unit_definition_at(
            unit, i, NULL, NULL, &definition, NULL);
        definitions[i] = (AST *)definition;
    }
    codegen_predeclare_toplevel_functions(&production, definitions, count, 0);
    for (size_t i = 0; i < count; i++) {
        const char *name = NULL, *docstring = NULL;
        const AST *definition = NULL;
        if (!monad_compilation_unit_definition_at(
                unit, i, &name, NULL, &definition, &docstring) ||
            !definition) {
            if (failed_definition) *failed_definition = name;
            free(definitions);
            codegen_dispose(&production);
            monad_native_image_destroy(image);
            return false;
        }
        codegen_expr(&production, (AST *)definition);
        image->entries[i].name = native_copy(name);
        image->entries[i].docstring = native_copy(docstring);
        if (!image->entries[i].name) {
            free(definitions);
            codegen_dispose(&production);
            monad_native_image_destroy(image);
            return false;
        }
    }
    free(definitions);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(production.builder)))
        LLVMBuildRetVoid(production.builder);
    LLVMContextRef context = NULL;
    LLVMModuleRef module = codegen_take_module(&production, &context);
    char *verification = NULL;
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &verification)) {
        LLVMDisposeMessage(verification);
        LLVMDisposeModule(module);
        LLVMContextDispose(context);
        monad_native_image_destroy(image);
        return false;
    }
    LLVMDisposeMessage(verification);
    LLVMOrcLLJITRef jit = NULL;
    LLVMErrorRef error = LLVMOrcCreateLLJIT(&jit, NULL);
    if (error) {
        dispose_llvm_error(error);
        LLVMDisposeModule(module);
        LLVMContextDispose(context);
        monad_native_image_destroy(image);
        return false;
    }
    LLVMOrcThreadSafeContextRef thread_context =
        LLVMOrcCreateNewThreadSafeContextFromLLVMContext(context);
    LLVMOrcThreadSafeModuleRef thread_module =
        LLVMOrcCreateNewThreadSafeModule(module, thread_context);
    error = LLVMOrcLLJITAddLLVMIRModule(
        jit, LLVMOrcLLJITGetMainJITDylib(jit), thread_module);
    if (error) {
        dispose_llvm_error(error);
        LLVMOrcDisposeLLJIT(jit);
        LLVMOrcDisposeThreadSafeContext(thread_context);
        monad_native_image_destroy(image);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        LLVMOrcExecutorAddress address = 0;
        error = LLVMOrcLLJITLookup(jit, &address, image->entries[i].name);
        if (error || !address) {
            dispose_llvm_error(error);
            LLVMOrcDisposeLLJIT(jit);
            LLVMOrcDisposeThreadSafeContext(thread_context);
            monad_native_image_destroy(image);
            return false;
        }
        image->entries[i].address = (void (*)(void))(uintptr_t)address;
    }
    image->jit = jit;
    image->thread_context = thread_context;
    *output = image;
    return true;
}

size_t monad_native_image_count(const MonadNativeImage *image) {
    return image ? image->count : 0;
}
const char *monad_native_image_name(const MonadNativeImage *image, size_t i) {
    return image && i < image->count ? image->entries[i].name : NULL;
}
const char *monad_native_image_docstring(
    const MonadNativeImage *image, size_t i) {
    return image && i < image->count ? image->entries[i].docstring : NULL;
}
void (*monad_native_image_address(
    const MonadNativeImage *image, size_t i))(void) {
    return image && i < image->count ? image->entries[i].address : NULL;
}
void monad_native_image_destroy(void *opaque) {
    MonadNativeImage *image = opaque;
    if (!image) return;
    if (image->jit) dispose_llvm_error(LLVMOrcDisposeLLJIT(image->jit));
    if (image->thread_context)
        LLVMOrcDisposeThreadSafeContext(image->thread_context);
    for (size_t i = 0; i < image->count; i++) {
        free(image->entries[i].name);
        free(image->entries[i].docstring);
    }
    free(image->entries);
    free(image);
}
