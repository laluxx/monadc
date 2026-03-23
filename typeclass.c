#include "typeclass.h"
#include "codegen.h"
#include "env.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Lifecycle

TypeClassRegistry *tc_registry_create(void) {
    TypeClassRegistry *reg = calloc(1, sizeof(TypeClassRegistry));
    reg->class_cap    = 8;
    reg->instance_cap = 16;
    reg->classes   = malloc(sizeof(TCClass)    * reg->class_cap);
    reg->instances = malloc(sizeof(TCInstance) * reg->instance_cap);
    return reg;
}

void tc_registry_free(TypeClassRegistry *reg) {
    if (!reg) return;

    for (int i = 0; i < reg->class_count; i++) {
        TCClass *c = &reg->classes[i];
        free(c->name);
        free(c->type_var);
        for (int j = 0; j < c->method_count; j++) {
            free(c->methods[j].name);
            free(c->methods[j].type_str);
        }
        free(c->methods);
        for (int j = 0; j < c->default_count; j++) {
            free(c->default_names[j]);
            ast_free(c->default_bodies[j]);
        }
        free(c->default_names);
        free(c->default_bodies);
    }
    free(reg->classes);

    for (int i = 0; i < reg->instance_count; i++) {
        TCInstance *inst = &reg->instances[i];
        free(inst->class_name);
        free(inst->type_name);
        for (int j = 0; j < inst->method_count; j++)
            free(inst->method_names[j]);
        free(inst->method_names);
        free(inst->method_funcs);
    }
    free(reg->instances);
    free(reg);
}

/// Name helpers

void tc_dict_name(const char *class_name, const char *type_name,
                  char *out, size_t out_size) {
    snprintf(out, out_size, "__dict_%s_%s", class_name, type_name);
}

void tc_method_name(const char *class_name, const char *type_name,
                    const char *method_name, char *out, size_t out_size) {
    snprintf(out, out_size, "__impl_%s_%s_%s",
             class_name, type_name, method_name);
}

/// Lookup

TCClass *tc_find_class(TypeClassRegistry *reg, const char *class_name) {
    for (int i = 0; i < reg->class_count; i++)
        if (strcmp(reg->classes[i].name, class_name) == 0)
            return &reg->classes[i];
    return NULL;
}

TCInstance *tc_find_instance(TypeClassRegistry *reg, const char *class_name,
                             const char *type_name) {
    for (int i = 0; i < reg->instance_count; i++) {
        TCInstance *inst = &reg->instances[i];
        if (strcmp(inst->class_name, class_name) == 0 &&
            strcmp(inst->type_name,  type_name)  == 0)
            return inst;
    }
    return NULL;
}

bool tc_is_method(TypeClassRegistry *reg, const char *method_name) {
    return tc_method_class(reg, method_name) != NULL;
}

const char *tc_method_class(TypeClassRegistry *reg, const char *method_name) {
    for (int i = 0; i < reg->class_count; i++) {
        TCClass *c = &reg->classes[i];
        for (int j = 0; j < c->method_count; j++)
            if (strcmp(c->methods[j].name, method_name) == 0)
                return c->name;
    }
    return NULL;
}

/// Dictionary LLVM type
//
// The dictionary for a class is a struct of function pointers, one per method.
// e.g. Eq -> { ptr, ptr }  (one for =, one for !=)
// We cache it as an LLVM named struct "dict.Eq".
//
LLVMTypeRef tc_dict_type(TypeClassRegistry *reg, const char *class_name,
                         CodegenContext *ctx) {
    char struct_name[256];
    snprintf(struct_name, sizeof(struct_name), "dict.%s", class_name);

    LLVMTypeRef existing = LLVMGetTypeByName2(ctx->context, struct_name);
    if (existing) return existing;

    TCClass *c = tc_find_class(reg, class_name);
    if (!c) return NULL;

    /* All method slots are ptr (function pointer, erased type) */
    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef *fields = malloc(sizeof(LLVMTypeRef) * c->method_count);
    for (int i = 0; i < c->method_count; i++)
        fields[i] = ptr_t;

    LLVMTypeRef dict_t = LLVMStructCreateNamed(ctx->context, struct_name);
    LLVMStructSetBody(dict_t, fields, c->method_count, 0);
    free(fields);
    return dict_t;
}

/// Class registration
//
// Records the class declaration in the registry and registers each method
// in the HM type environment so inference knows about them.
//
//
void tc_register_class(TypeClassRegistry *reg, AST *ast) {
    if (!ast || ast->type != AST_CLASS) return;

    /* Grow registry if needed */
    if (reg->class_count >= reg->class_cap) {
        reg->class_cap *= 2;
        reg->classes = realloc(reg->classes,
                               sizeof(TCClass) * reg->class_cap);
    }

    TCClass *c = &reg->classes[reg->class_count++];
    c->name       = strdup(ast->class_decl.name);
    c->type_var   = strdup(ast->class_decl.type_var);

    /* Copy method signatures */
    c->method_count = ast->class_decl.method_count;
    c->methods = malloc(sizeof(TCMethod) * (c->method_count ? c->method_count : 1));
    for (int i = 0; i < c->method_count; i++) {
        c->methods[i].name     = strdup(ast->class_decl.method_names[i]);
        c->methods[i].type_str = strdup(ast->class_decl.method_types[i]);
    }

    /* Copy default implementations */
    c->default_count = ast->class_decl.default_count;
    c->default_names  = malloc(sizeof(char*) * (c->default_count ? c->default_count : 1));
    c->default_bodies = malloc(sizeof(AST*)  * (c->default_count ? c->default_count : 1));
    for (int i = 0; i < c->default_count; i++) {
        c->default_names[i]  = strdup(ast->class_decl.default_names[i]);
        c->default_bodies[i] = ast_clone(ast->class_decl.default_bodies[i]);
    }

    printf("Class: %s %s (%d methods, %d defaults)\n",
           c->name, c->type_var, c->method_count, c->default_count);
    for (int i = 0; i < c->method_count; i++)
        printf("  method: %s :: %s\n", c->methods[i].name, c->methods[i].type_str);
    for (int i = 0; i < c->default_count; i++)
        printf("  default: %s\n", c->default_names[i]);
}

/// Instance registration
//
// For each method in the instance:
//   1. Compile the lambda body into an LLVM function named __impl_Class_Type_method
//   2. Fill the default implementations for any missing methods
//   3. Build the dictionary struct global __dict_Class_Type and initialize it
//      with pointers to the method functions
//   4. Register each method in the env so (= x y) can be dispatched
//
void tc_register_instance(TypeClassRegistry *reg, AST *ast,
                          CodegenContext *ctx) {
    if (!ast || ast->type != AST_INSTANCE) return;

    const char *class_name = ast->instance_decl.class_name;
    const char *type_name  = ast->instance_decl.type_name;

    TCClass *c = tc_find_class(reg, class_name);
    if (!c) {
        fprintf(stderr, "instance: unknown class '%s'\n", class_name);
        return;
    }

    /* Grow instance registry if needed */
    if (reg->instance_count >= reg->instance_cap) {
        reg->instance_cap *= 2;
        reg->instances = realloc(reg->instances,
                                 sizeof(TCInstance) * reg->instance_cap);
    }

    TCInstance *inst = &reg->instances[reg->instance_count++];
    inst->class_name   = strdup(class_name);
    inst->type_name    = strdup(type_name);
    inst->method_count = c->method_count; /* all methods, including defaults */
    inst->method_names = malloc(sizeof(char*)        * c->method_count);
    inst->method_funcs = malloc(sizeof(LLVMValueRef) * c->method_count);

    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef i64   = LLVMInt64TypeInContext(ctx->context);

    /* ── Step 1: compile each explicitly provided method ───────────────── */
    for (int mi = 0; mi < c->method_count; mi++) {
        const char *mname = c->methods[mi].name;
        inst->method_names[mi] = strdup(mname);
        inst->method_funcs[mi] = NULL; /* filled below */

        /* Find this method in the instance declaration */
        AST *body_lam = NULL;
        for (int ji = 0; ji < ast->instance_decl.method_count; ji++) {
            if (strcmp(ast->instance_decl.method_names[ji], mname) == 0) {
                body_lam = ast->instance_decl.method_bodies[ji];
                break;
            }
        }

        /* Fall back to default implementation if not provided */
        if (!body_lam) {
            for (int di = 0; di < c->default_count; di++) {
                if (strcmp(c->default_names[di], mname) == 0) {
                    body_lam = c->default_bodies[di];
                    break;
                }
            }
        }

        if (!body_lam) {
            fprintf(stderr, "instance %s %s: no implementation for method '%s' "
                    "and no default\n", class_name, type_name, mname);
            continue;
        }

        /* Build LLVM function name */
        char fn_name[256];
        tc_method_name(class_name, type_name, mname, fn_name, sizeof(fn_name));

        /* The method uses closure ABI: (ptr env, i32 n, ptr args) -> ptr
         * We compile the lambda body as a closure-ABI function.
         * This reuses the existing define path by synthesizing a define node. */
        AST *define_node = ast_new_list();
        ast_list_append(define_node, ast_new_symbol("define"));
        ast_list_append(define_node, ast_new_symbol(fn_name));
        ast_list_append(define_node, ast_clone(body_lam));

        codegen_expr(ctx, define_node);
        ast_free(define_node);

        /* Look up the compiled function */
        LLVMValueRef fn = LLVMGetNamedFunction(ctx->module, fn_name);
        if (!fn) {
            /* Try env lookup — define may have stored it under original name */
            EnvEntry *e = env_lookup(ctx->env, fn_name);
            if (e) fn = e->func_ref ? e->func_ref : e->value;
        }
        inst->method_funcs[mi] = fn;

        printf("  compiled method: %s -> %s\n", mname, fn_name);
    }

    /* ── Step 2: build the dictionary global ───────────────────────────── */
    //
    // The dictionary is a global struct of ptr-typed function pointers.
    // Layout matches the order of methods in the class declaration.
    //
    LLVMTypeRef dict_t = tc_dict_type(reg, class_name, ctx);
    if (!dict_t) {
        fprintf(stderr, "instance: could not create dict type for '%s'\n",
                class_name);
        return;
    }

    char dict_gname[256];
    tc_dict_name(class_name, type_name, dict_gname, sizeof(dict_gname));

    LLVMValueRef dict_global = LLVMGetNamedGlobal(ctx->module, dict_gname);
    if (!dict_global) {
        dict_global = LLVMAddGlobal(ctx->module, dict_t, dict_gname);
        LLVMSetLinkage(dict_global, LLVMExternalLinkage);
        LLVMSetInitializer(dict_global, LLVMConstNull(dict_t));
    }
    inst->dict_global = dict_global;

    /* Store each method function pointer into the dict struct */
    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
    for (int mi = 0; mi < c->method_count; mi++) {
        if (!inst->method_funcs[mi]) continue;

        LLVMValueRef fn_ptr = LLVMBuildBitCast(ctx->builder,
                                  inst->method_funcs[mi], ptr_t, "mptr");
        LLVMValueRef zero   = LLVMConstInt(i32, 0, 0);
        LLVMValueRef idx    = LLVMConstInt(i32, mi, 0);
        LLVMValueRef idxs[] = {zero, idx};
        LLVMValueRef slot   = LLVMBuildGEP2(ctx->builder, dict_t,
                                             dict_global, idxs, 2, "dict_slot");
        LLVMBuildStore(ctx->builder, fn_ptr, slot);
    }

    /* ── Step 3: register methods in the env ──────────────────────────── */
    //
    // For each method, register it in the env under its plain name (e.g. "=")
    // pointing to the instance's method function. This enables direct dispatch
    // when the type is known at compile time — the most common case.
    //
    // When the type is not known statically (polymorphic call sites), the
    // dictionary must be passed explicitly — that's the next step.
    //
    for (int mi = 0; mi < c->method_count; mi++) {
        if (!inst->method_funcs[mi]) continue;
        const char *mname = inst->method_names[mi];

        /* Register as ENV_FUNC so direct typed calls work */
        EnvEntry *existing = env_lookup(ctx->env, mname);
        if (!existing) {
            /* 2-param closure ABI function */
            EnvParam *params = malloc(sizeof(EnvParam) * 2);
            params[0].name = strdup("__x");
            params[0].type = type_unknown();
            params[1].name = strdup("__y");
            params[1].type = type_unknown();
            env_insert_func(ctx->env, mname, params, 2,
                            type_unknown(), inst->method_funcs[mi], NULL);
            EnvEntry *e = env_lookup(ctx->env, mname);
            if (e) {
                e->is_closure_abi = true;
                e->lifted_count   = 0;
            }
        }
    }

    printf("Instance: %s %s -> dict %s\n",
           class_name, type_name, dict_gname);
}

/// Method dispatch
//
// Returns the LLVM function for a specific method in a specific instance.
// Used when the type is known statically at the call site.
//
LLVMValueRef tc_get_method(TypeClassRegistry *reg, const char *class_name,
                            const char *type_name, const char *method_name,
                            CodegenContext *ctx) {
    TCInstance *inst = tc_find_instance(reg, class_name, type_name);
    if (!inst) return NULL;

    for (int i = 0; i < inst->method_count; i++) {
        if (strcmp(inst->method_names[i], method_name) == 0)
            return inst->method_funcs[i];
    }
    return NULL;
}

LLVMValueRef tc_get_dict(TypeClassRegistry *reg, const char *class_name,
                         const char *type_name, CodegenContext *ctx) {
    TCInstance *inst = tc_find_instance(reg, class_name, type_name);
    if (!inst) return NULL;

    /* Re-declare in current module if needed (REPL cross-module) */
    LLVMTypeRef dict_t = tc_dict_type(reg, class_name, ctx);
    if (!dict_t) return NULL;

    char dict_gname[256];
    tc_dict_name(class_name, type_name, dict_gname, sizeof(dict_gname));

    LLVMValueRef gv = LLVMGetNamedGlobal(ctx->module, dict_gname);
    if (!gv) {
        gv = LLVMAddGlobal(ctx->module, dict_t, dict_gname);
        LLVMSetLinkage(gv, LLVMExternalLinkage);
    }
    return gv;
}
