#include "codegen.h"
#include "pmatch.h"
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
#include "infer.h"
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
    env_init_infer(ctx->env);
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
    // Initialize monomorphization cache
    ctx->mono_cache.entries  = NULL;
    ctx->mono_cache.count    = 0;
    ctx->mono_cache.capacity = 0;
    memset(ctx->error_msg, 0, sizeof(ctx->error_msg));
}

/// Monomorphization

static bool mono_types_match(Type **a, Type **b, int count) {
    for (int i = 0; i < count; i++) {
        if (!a[i] || !b[i]) return false;
        if (a[i]->kind != b[i]->kind) return false;
        /* For ground types, kind equality is enough.
         * For compound types we'd recurse — add as needed. */
    }
    return true;
}

LLVMValueRef mono_cache_lookup(MonoCache *cache, const char *fn_name,
                                Type **type_args, int type_arg_count) {
    for (int i = 0; i < cache->count; i++) {
        MonoCacheEntry *e = &cache->entries[i];
        if (strcmp(e->key.fn_name, fn_name) != 0) continue;
        if (e->key.type_arg_count != type_arg_count) continue;
        if (mono_types_match(e->key.type_args, type_args, type_arg_count))
            return e->fn;
    }
    return NULL;
}

void mono_cache_insert(MonoCache *cache, const char *fn_name,
                        Type **type_args, int type_arg_count,
                        LLVMValueRef fn, const char *specialized_name) {
    if (cache->count >= cache->capacity) {
        cache->capacity = cache->capacity == 0 ? 16 : cache->capacity * 2;
        cache->entries  = realloc(cache->entries,
                                   sizeof(MonoCacheEntry) * cache->capacity);
    }
    MonoCacheEntry *e    = &cache->entries[cache->count++];
    e->key.fn_name       = strdup(fn_name);
    e->key.type_arg_count = type_arg_count;
    e->key.type_args     = malloc(sizeof(Type*) * (type_arg_count ? type_arg_count : 1));
    for (int i = 0; i < type_arg_count; i++)
        e->key.type_args[i] = type_args[i];
    e->fn                = fn;
    e->specialized_name  = strdup(specialized_name);
}

void mono_cache_free(MonoCache *cache) {
    for (int i = 0; i < cache->count; i++) {
        MonoCacheEntry *e = &cache->entries[i];
        free(e->key.fn_name);
        free(e->key.type_args);
        free(e->specialized_name);
    }
    free(cache->entries);
    cache->entries  = NULL;
    cache->count    = 0;
    cache->capacity = 0;
}

//// Monomorphization helpers

// Build a specialized function name from base name and concrete types.
// e.g. "id" + [Int] -> "id_Int"
//   "const" + [Int, String] -> "const_Int_String"
static char *mono_make_name(const char *base, Type **types, int count) {
    char buf[256];
    int  pos = snprintf(buf, sizeof(buf), "%s", base);
    for (int i = 0; i < count; i++) {
        const char *tname = type_to_string(types[i]);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "_%s", tname);
    }
    return strdup(buf);
}

// Substitute TYPE_VAR nodes in a type according to a TypeSubst mapping.
// Returns a new concrete type with all type variables replaced.
static Type *mono_apply_subst(Type *t, TypeSubst *ts) {
    if (!t) return NULL;
    switch (t->kind) {
    case TYPE_VAR:
        for (int i = 0; i < ts->count; i++) {
            Type *from = ts->to[i];
            if (from && from->kind == TYPE_VAR &&
                from->var_id == t->var_id)
                return ts->to[i];
            // After unification, to[i] may already be concrete
            if (ts->from[i] == t->var_id)
                return ts->to[i];
        }
        return t;
    case TYPE_ARROW:
        return type_arrow(mono_apply_subst(t->arrow_param, ts),
                          mono_apply_subst(t->arrow_ret,   ts));
    case TYPE_LIST:
        return type_list(mono_apply_subst(t->list_elem, ts));
    default:
        return t;
    }
}

//// Specialization

// Recompile a polymorphic function with concrete types substituted in.
// Returns the specialized LLVM function, or NULL on failure.
// Results are cached in ctx->mono_cache.
static LLVMValueRef codegen_specialize(CodegenContext *ctx,
                                        const char    *fn_name,
                                        EnvEntry      *entry,
                                        TypeSubst     *ts) {
    if (!entry || !entry->source_ast) return NULL;
    if (ts->count == 0) return entry->func_ref; // already concrete

    // Resolve concrete types for each type variable
    Type **concrete = malloc(sizeof(Type*) * ts->count);
    for (int i = 0; i < ts->count; i++) {
        concrete[i] = subst_apply(
            env_get_infer(ctx->env)
                ? NULL  // we'll resolve via the type directly
                : NULL,
            ts->to[i]);
        // ts->to[i] is already resolved after unification
        concrete[i] = ts->to[i];
    }

    // Build specialized name
    char *spec_name = mono_make_name(fn_name, concrete, ts->count);

    // Check if already in current LLVM module
    LLVMValueRef existing = LLVMGetNamedFunction(ctx->module, spec_name);
    if (existing) {
        mono_cache_insert(&ctx->mono_cache, fn_name, concrete, ts->count,
                          existing, spec_name);
        free(spec_name);
        free(concrete);
        return existing;
    }

    // Check cache — may be from a previous REPL module
    LLVMValueRef cached = mono_cache_lookup(&ctx->mono_cache,
                                             fn_name, concrete, ts->count);
    if (cached) {
        // Re-declare as external in the current module
        EnvEntry *ce = env_lookup(ctx->env, spec_name);
        if (ce && ce->return_type) {
            int np = ce->param_count;
            LLVMTypeRef *pt = malloc(sizeof(LLVMTypeRef) * (np ? np : 1));
            for (int i = 0; i < np; i++)
                pt[i] = type_to_llvm(ctx, ce->params[i].type);
            LLVMTypeRef ft = LLVMFunctionType(type_to_llvm(ctx, ce->return_type),
                                               pt, np, 0);
            LLVMValueRef ext = LLVMAddFunction(ctx->module, spec_name, ft);
            LLVMSetLinkage(ext, LLVMExternalLinkage);
            free(pt);
            free(spec_name);
            free(concrete);
            return ext;
        }
        free(spec_name);
        free(concrete);
        return cached;
    }

    // Get the source AST — must be a (define (name params) body) form
    AST *source = entry->source_ast;
    if (!source) {
        free(spec_name);
        free(concrete);
        return NULL;
    }

    // Find the lambda in the source AST
    AST *lambda = NULL;
    if (source->type == AST_LAMBDA) {
        /* source_ast is the lambda itself */
        lambda = source;
    } else if (source->type == AST_LIST && source->list.count >= 3) {
        /* source_ast is a (define name lambda) form */
        AST *maybe_lambda = source->list.items[2];
        if (maybe_lambda->type == AST_LAMBDA)
            lambda = maybe_lambda;
    }
    if (!lambda) {
        free(spec_name);
        free(concrete);
        return NULL;
    }

    // Build concrete param types by substituting type vars
    int total_params = lambda->lambda.param_count;
    LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * (total_params ? total_params : 1));
    EnvParam    *env_params  = malloc(sizeof(EnvParam)    * (total_params ? total_params : 1));

    for (int i = 0; i < total_params; i++) {
        ASTParam *param = &lambda->lambda.params[i];
        Type *param_type = NULL;
        if (param->type_name) {
            param_type = type_from_name(param->type_name);
        }

        if (!param_type || param_type->kind == TYPE_UNKNOWN ||
            param_type->kind == TYPE_VAR) {
            // Try direct substitution first (i < ts->count)
            if (i < ts->count) {
                param_type = ts->to[i];
            } else {
                // More params than type vars — all extra params get
                // the same type as the first substitution (e.g. add x y
                // with one type var 'b means both x and y are 'b)
                param_type = mono_apply_subst(
                    (i < entry->param_count) ? entry->params[i].type
                                             : type_unknown(),
                    ts);
                // If still unknown, use first concrete type
                if (!param_type || param_type->kind == TYPE_UNKNOWN ||
                    param_type->kind == TYPE_VAR) {
                    if (ts->count > 0) param_type = ts->to[0];
                }
            }
        }
        if (!param_type || param_type->kind == TYPE_UNKNOWN)
            param_type = type_unknown();

        param_types[i]     = type_to_llvm(ctx, param_type);
        env_params[i].name = strdup(param->name);
        env_params[i].type = param_type;
    }

    // Determine concrete return type
    Type *ret_type = NULL;
    if (lambda->lambda.return_type) {
        ret_type = type_from_name(lambda->lambda.return_type);
    }
    if (!ret_type || ret_type->kind == TYPE_UNKNOWN ||
        ret_type->kind == TYPE_VAR) {
        /* Try substituting from the scheme's return type */
        ret_type = mono_apply_subst(entry->return_type, ts);
    }
    /* If still unknown, infer from the body after specialization.
     * For now use the first concrete type arg as return (handles id :: 'a -> 'a) */
    if (!ret_type || ret_type->kind == TYPE_UNKNOWN) {
        if (ts->count > 0 && ts->to[0])
            ret_type = ts->to[0];
        else
            ret_type = type_unknown();
    }

    LLVMTypeRef ret_llvm  = type_to_llvm(ctx, ret_type);
    LLVMTypeRef func_type = LLVMFunctionType(ret_llvm, param_types,
                                              total_params, 0);
    LLVMValueRef func     = LLVMAddFunction(ctx->module, spec_name, func_type);

    // Register in env so recursive calls resolve
    env_insert_func(ctx->env, spec_name,
                    clone_params(env_params, total_params),
                    total_params, type_clone(ret_type),
                    func, NULL);
    EnvEntry *spec_entry = env_lookup(ctx->env, spec_name);
    if (spec_entry) {
        spec_entry->lifted_count   = 0;
        spec_entry->is_closure_abi = false;
    }

    // Compile the body
    LLVMBasicBlockRef entry_block = LLVMAppendBasicBlockInContext(
                                        ctx->context, func, "entry");
    LLVMBasicBlockRef saved_block = LLVMGetInsertBlock(ctx->builder);
    LLVMPositionBuilderAtEnd(ctx->builder, entry_block);

    Env *saved_env = ctx->env;
    ctx->env = env_create_child(saved_env);

    // Bind parameters
    for (int i = 0; i < total_params; i++) {
        LLVMValueRef param = LLVMGetParam(func, i);
        LLVMSetValueName2(param, lambda->lambda.params[i].name,
                          strlen(lambda->lambda.params[i].name));
        LLVMValueRef alloca = LLVMBuildAlloca(ctx->builder, param_types[i],
                                               lambda->lambda.params[i].name);
        LLVMBuildStore(ctx->builder, param, alloca);
        env_insert(ctx->env, lambda->lambda.params[i].name,
                   type_clone(env_params[i].type), alloca);
    }

    // Codegen body
    CodegenResult body = {NULL, NULL};
    const char *prev   = ctx->current_function_name;
    ctx->current_function_name = spec_name;
    for (int i = 0; i < lambda->lambda.body_count; i++)
        body = codegen_expr(ctx, lambda->lambda.body_exprs[i]);
    ctx->current_function_name = prev;

    if (!body.value)
        body.value = LLVMConstNull(ret_llvm);

    // Coerce return value
    LLVMValueRef ret_val = body.value;
    LLVMTypeRef  actual  = LLVMTypeOf(ret_val);
    LLVMTypeRef  i64     = LLVMInt64TypeInContext(ctx->context);
    LLVMTypeRef  i1      = LLVMInt1TypeInContext(ctx->context);
    LLVMTypeRef  dbl     = LLVMDoubleTypeInContext(ctx->context);

    if (ret_llvm == i64 && actual == i1)
        ret_val = LLVMBuildZExt(ctx->builder, ret_val, i64, "ret_ext");
    else if (ret_llvm == i1 && actual == i64)
        ret_val = LLVMBuildTrunc(ctx->builder, ret_val, i1, "ret_trunc");
    else if (ret_llvm == dbl && type_is_integer(body.type))
        ret_val = LLVMBuildSIToFP(ctx->builder, ret_val, dbl, "ret_conv");
    else if (ret_llvm == i64 && type_is_float(body.type))
        ret_val = LLVMBuildFPToSI(ctx->builder, ret_val, i64, "ret_conv");

    LLVMBuildRet(ctx->builder, ret_val);

    // Restore
    env_free(ctx->env);
    ctx->env = saved_env;
    if (saved_block)
        LLVMPositionBuilderAtEnd(ctx->builder, saved_block);

    // Cache and return
    mono_cache_insert(&ctx->mono_cache, fn_name, concrete, ts->count,
                      func, spec_name);

    free(param_types);
    free(env_params);  // Note: names were strdup'd into env
    free(spec_name);
    free(concrete);
    return func;
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
    mono_cache_free(&ctx->mono_cache);
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
    case TYPE_SET:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    case TYPE_MAP:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    case TYPE_FN:
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
            char _mp_copy[256];
            strncpy(_mp_copy, module_prefix, sizeof(_mp_copy) - 1);
            _mp_copy[sizeof(_mp_copy)-1] = '\0';
            free(module_prefix);
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown module prefix '%s'",
                    parser_get_filename(), ast->line, ast->column, _mp_copy);
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
        case TYPE_SYMBOL: {
            LLVMValueRef fn        = get_rt_value_symbol(ctx);
            LLVMTypeRef  ft_args[] = {ptr};
            LLVMTypeRef  ft        = LLVMFunctionType(ptr, ft_args, 1, 0);
            LLVMValueRef args[]    = {val};
            return LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "boxed_sym");
        }
        case TYPE_KEYWORD: {
            LLVMValueRef fn        = get_rt_value_keyword(ctx);
            LLVMTypeRef  ft_args[] = {ptr};
            LLVMTypeRef  ft        = LLVMFunctionType(ptr, ft_args, 1, 0);
            LLVMValueRef args[]    = {val};
            return LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "boxed_kw");
        }

        default:
            // already a pointer / unknown — pass through
            return val;
    }
}


// ─── Wrapper generators ────────────────────────────────────────────────────
//
// These emit small LLVM functions that bridge typed user functions
// (e.g. i64->i64) into the RuntimeValue*->RuntimeValue* ABI that the
// runtime HOFs (map, filter, foldl, foldr, zipwith) expect.

static LLVMValueRef codegen_unary_wrapper(CodegenContext *ctx,
                                           LLVMValueRef  *out_closure_env) {
    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
    LLVMTypeRef wrapper_params[] = {ptr, i32, ptr};
    LLVMTypeRef wrapper_ft = LLVMFunctionType(ptr, wrapper_params, 3, 0);

    static int unary_count = 0;
    char wname[64];
    snprintf(wname, sizeof(wname), "__unary_wrapper_%d", unary_count++);

    LLVMValueRef      wrapper = LLVMAddFunction(ctx->module, wname, wrapper_ft);
    LLVMBasicBlockRef entry   = LLVMAppendBasicBlockInContext(ctx->context, wrapper, "entry");
    LLVMBasicBlockRef saved   = LLVMGetInsertBlock(ctx->builder);
    LLVMPositionBuilderAtEnd(ctx->builder, entry);

    LLVMValueRef env_param  = LLVMGetParam(wrapper, 0);
    /* param 1 is i32 n — ignored */
    LLVMValueRef args_array = LLVMGetParam(wrapper, 2);

    // args_array is already RuntimeValue** — pass directly to rt_closure_calln
    LLVMValueRef call_fn  = get_rt_closure_calln(ctx);
    LLVMTypeRef  ft_p[]   = {ptr, i32, ptr};
    LLVMTypeRef  ft       = LLVMFunctionType(ptr, ft_p, 3, 0);
    LLVMValueRef cargs[]  = {env_param, LLVMConstInt(i32, 1, 0), args_array};
    LLVMValueRef res      = LLVMBuildCall2(ctx->builder, ft, call_fn, cargs, 3, "res");

    LLVMBuildRet(ctx->builder, res);

    if (saved) LLVMPositionBuilderAtEnd(ctx->builder, saved);
    *out_closure_env = LLVMConstPointerNull(ptr); /* set by call site */
    return wrapper;
}


static LLVMValueRef codegen_binary_wrapper(CodegenContext *ctx,
                                            LLVMValueRef  *out_closure_env) {
    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
    LLVMTypeRef wrapper_params[] = {ptr, i32, ptr};
    LLVMTypeRef wrapper_ft = LLVMFunctionType(ptr, wrapper_params, 3, 0);

    static int binary_count = 0;
    char wname[64];
    snprintf(wname, sizeof(wname), "__binary_wrapper_%d", binary_count++);

    LLVMValueRef      wrapper = LLVMAddFunction(ctx->module, wname, wrapper_ft);
    LLVMBasicBlockRef entry   = LLVMAppendBasicBlockInContext(ctx->context, wrapper, "entry");
    LLVMBasicBlockRef saved   = LLVMGetInsertBlock(ctx->builder);
    LLVMPositionBuilderAtEnd(ctx->builder, entry);

    LLVMValueRef env_param  = LLVMGetParam(wrapper, 0);
    /* param 1 is i32 n — ignored, args come from the array */
    LLVMValueRef args_array = LLVMGetParam(wrapper, 2);

    // args_array is already RuntimeValue**  — pass directly to rt_closure_calln
    LLVMValueRef call_fn  = get_rt_closure_calln(ctx);
    LLVMTypeRef  ft_p[]   = {ptr, i32, ptr};
    LLVMTypeRef  ft       = LLVMFunctionType(ptr, ft_p, 3, 0);
    LLVMValueRef cargs[]  = {env_param, LLVMConstInt(i32, 2, 0), args_array};
    LLVMValueRef res      = LLVMBuildCall2(ctx->builder, ft, call_fn, cargs, 3, "res");

    LLVMBuildRet(ctx->builder, res);

    if (saved) LLVMPositionBuilderAtEnd(ctx->builder, saved);
    *out_closure_env = LLVMConstPointerNull(ptr);
    return wrapper;
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

// Helper: emit a set conj/disj call.
// fn_getter: get_rt_set_conj / get_rt_set_disj / mut variants
// name:      for LLVM value names
static CodegenResult codegen_set_op(CodegenContext *ctx, AST *ast,
                                    LLVMValueRef (*fn_getter)(CodegenContext *),
                                    const char *op_name) {
    CodegenResult result = {NULL, NULL};
    if (ast->list.count != 3) {
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' requires 2 arguments (set val)",
                      parser_get_filename(), ast->line, ast->column, op_name);
    }

    LLVMTypeRef  ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    CodegenResult set_r = codegen_expr(ctx, ast->list.items[1]);
    CodegenResult val_r = codegen_expr(ctx, ast->list.items[2]);

    LLVMValueRef boxed   = codegen_box(ctx, val_r.value, val_r.type);
    LLVMValueRef raw_set = set_r.value;

    if (set_r.type && set_r.type->kind == TYPE_SET) {
        LLVMValueRef ub_fn = get_rt_unbox_set(ctx);
        LLVMTypeRef  ft    = LLVMFunctionType(ptr, &ptr, 1, 0);
        LLVMValueRef ua[]  = {raw_set};
        raw_set = LLVMBuildCall2(ctx->builder, ft, ub_fn, ua, 1, "rawset");
    }

    LLVMValueRef op_fn   = fn_getter(ctx);
    LLVMTypeRef  op_p[]  = {ptr, ptr};
    LLVMTypeRef  op_ft   = LLVMFunctionType(ptr, op_p, 2, 0);
    LLVMValueRef op_a[]  = {raw_set, boxed};
    LLVMValueRef new_set = LLVMBuildCall2(ctx->builder, op_ft, op_fn, op_a, 2, op_name);

    LLVMValueRef wrap_fn = get_rt_value_set(ctx);
    LLVMTypeRef  wft     = LLVMFunctionType(ptr, &ptr, 1, 0);
    LLVMValueRef wa[]    = {new_set};
    result.value = LLVMBuildCall2(ctx->builder, wft, wrap_fn, wa, 1, "setval");
    result.type  = type_set();
    return result;
}

static CodegenResult codegen_map_op(CodegenContext *ctx, AST *ast,
                                    LLVMValueRef (*fn_getter)(CodegenContext *),
                                    const char *op_name, int expect_args) {
    CodegenResult result = {NULL, NULL};
    if ((int)ast->list.count != expect_args + 1) {
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' requires %d argument%s",
                      parser_get_filename(), ast->line, ast->column,
                      op_name, expect_args, expect_args == 1 ? "" : "s");
    }
    LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    CodegenResult map_r  = codegen_expr(ctx, ast->list.items[1]);
    LLVMValueRef raw_map = map_r.value;
    if (map_r.type && map_r.type->kind == TYPE_MAP) {
        LLVMValueRef ub_fn = get_rt_unbox_map(ctx);
        LLVMTypeRef  ft    = LLVMFunctionType(ptr, &ptr, 1, 0);
        LLVMValueRef ua[]  = {raw_map};
        raw_map = LLVMBuildCall2(ctx->builder, ft, ub_fn, ua, 1, "rawmap");
    }

    LLVMValueRef op_fn = fn_getter(ctx);

    if (expect_args == 1) {
        /* unary: (keys m), (vals m), (count m) */
        LLVMTypeRef ft_args[] = {ptr};
        LLVMTypeRef ft = LLVMFunctionType(ptr, ft_args, 1, 0);
        LLVMValueRef args[] = {raw_map};
        result.value = LLVMBuildCall2(ctx->builder, ft, op_fn, args, 1, op_name);
        result.type  = type_list(NULL);
        return result;
    }

    if (expect_args == 2) {
        /* binary: (dissoc m key), (contains? m key), (find m key) */
        CodegenResult key_r = codegen_expr(ctx, ast->list.items[2]);
        LLVMValueRef  boxed = codegen_box(ctx, key_r.value, key_r.type);
        LLVMTypeRef   ft_args[] = {ptr, ptr};
        LLVMTypeRef   ft = LLVMFunctionType(ptr, ft_args, 2, 0);
        LLVMValueRef  args[] = {raw_map, boxed};
        LLVMValueRef  new_map = LLVMBuildCall2(ctx->builder, ft, op_fn, args, 2, op_name);
        LLVMValueRef  wrap_fn = get_rt_value_map(ctx);
        LLVMTypeRef   wft     = LLVMFunctionType(ptr, &ptr, 1, 0);
        LLVMValueRef  wa[]    = {new_map};
        result.value = LLVMBuildCall2(ctx->builder, wft, wrap_fn, wa, 1, "mapval");
        result.type  = type_map();
        return result;
    }

    /* ternary: (assoc m key val) */
    CodegenResult key_r = codegen_expr(ctx, ast->list.items[2]);
    CodegenResult val_r = codegen_expr(ctx, ast->list.items[3]);
    LLVMValueRef  bkey  = codegen_box(ctx, key_r.value, key_r.type);
    LLVMValueRef  bval  = codegen_box(ctx, val_r.value, val_r.type);
    LLVMTypeRef   ft_args[] = {ptr, ptr, ptr};
    LLVMTypeRef   ft = LLVMFunctionType(ptr, ft_args, 3, 0);
    LLVMValueRef  args[] = {raw_map, bkey, bval};
    LLVMValueRef  new_map = LLVMBuildCall2(ctx->builder, ft, op_fn, args, 3, op_name);
    LLVMValueRef  wrap_fn = get_rt_value_map(ctx);
    LLVMTypeRef   wft     = LLVMFunctionType(ptr, &ptr, 1, 0);
    LLVMValueRef  wa[]    = {new_map};
    result.value = LLVMBuildCall2(ctx->builder, wft, wrap_fn, wa, 1, "mapval");
    result.type  = type_map();
    return result;
}

/* Generate a closure-ABI trampoline for a typed-ABI function, then wrap
 * it in a RuntimeValue*(RT_CLOSURE). Used when a typed function needs to
 * be passed as a first-class value to a HOF or closure call site.       */
static LLVMValueRef wrap_func_as_closure(CodegenContext *ctx, EnvEntry *e) {
    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef i32   = LLVMInt32TypeInContext(ctx->context);
    LLVMTypeRef i64   = LLVMInt64TypeInContext(ctx->context);
    LLVMTypeRef dbl   = LLVMDoubleTypeInContext(ctx->context);
    LLVMTypeRef i1    = LLVMInt1TypeInContext(ctx->context);
    int declared = e->param_count - e->lifted_count;

    LLVMValueRef fn_to_wrap = e->func_ref;

    if (!e->is_closure_abi) {
        /* Build a closure-ABI trampoline:
         * (ptr env, ptr arg0, ..., ptr argN) -> ptr
         * Unboxes args, calls the real typed function, boxes result. */
        int np = declared + 1;
        LLVMTypeRef *tp = malloc(sizeof(LLVMTypeRef) * (np ? np : 1));
        tp[0] = ptr_t;
        for (int i = 0; i < declared; i++) tp[i + 1] = ptr_t;
        LLVMTypeRef tft = LLVMFunctionType(ptr_t, tp, np, 0);
        free(tp);

        static int tramp_count = 0;
        char tname[64];
        snprintf(tname, sizeof(tname), "__tramp_%d", tramp_count++);

        LLVMValueRef      tramp  = LLVMAddFunction(ctx->module, tname, tft);
        LLVMBasicBlockRef tentry = LLVMAppendBasicBlockInContext(
                                       ctx->context, tramp, "entry");
        LLVMBasicBlockRef tsaved = LLVMGetInsertBlock(ctx->builder);
        LLVMPositionBuilderAtEnd(ctx->builder, tentry);

        /* Unbox each argument */
        LLVMValueRef *real_args  = malloc(sizeof(LLVMValueRef) * (declared ? declared : 1));
        LLVMTypeRef  *real_types = malloc(sizeof(LLVMTypeRef)  * (declared ? declared : 1));
        for (int i = 0; i < declared; i++) {
            LLVMValueRef boxed = LLVMGetParam(tramp, i + 1);
            Type *pt = (e->params && i < e->param_count) ? e->params[i].type : NULL;
            LLVMTypeRef native = pt ? type_to_llvm(ctx, pt) : ptr_t;
            real_types[i] = native;
            if (native == i64) {
                LLVMTypeRef uft = LLVMFunctionType(i64, &ptr_t, 1, 0);
                real_args[i] = LLVMBuildCall2(ctx->builder, uft,
                                   get_rt_unbox_int(ctx), &boxed, 1, "ua");
            } else if (native == dbl) {
                LLVMTypeRef uft = LLVMFunctionType(dbl, &ptr_t, 1, 0);
                real_args[i] = LLVMBuildCall2(ctx->builder, uft,
                                   get_rt_unbox_float(ctx), &boxed, 1, "ua");
            } else {
                real_args[i] = boxed;
            }
        }

        /* Call the real typed function */
        Type *rt = e->return_type;
        LLVMTypeRef native_ret = rt ? type_to_llvm(ctx, rt) : ptr_t;
        LLVMTypeRef real_ft = LLVMFunctionType(native_ret, real_types, declared, 0);
        LLVMValueRef raw = LLVMBuildCall2(ctx->builder, real_ft,
                                           fn_to_wrap, real_args, declared, "raw");
        free(real_args);
        free(real_types);

        /* Box result */
        LLVMValueRef boxed_ret;
        if (native_ret == i64) {
            LLVMTypeRef bft = LLVMFunctionType(ptr_t, &i64, 1, 0);
            boxed_ret = LLVMBuildCall2(ctx->builder, bft,
                                       get_rt_value_int(ctx), &raw, 1, "br");
        } else if (native_ret == dbl) {
            LLVMTypeRef bft = LLVMFunctionType(ptr_t, &dbl, 1, 0);
            boxed_ret = LLVMBuildCall2(ctx->builder, bft,
                                       get_rt_value_float(ctx), &raw, 1, "br");
        } else if (native_ret == i1) {
            LLVMValueRef ext = LLVMBuildZExt(ctx->builder, raw, i64, "ext");
            LLVMTypeRef  bft = LLVMFunctionType(ptr_t, &i64, 1, 0);
            boxed_ret = LLVMBuildCall2(ctx->builder, bft,
                                       get_rt_value_int(ctx), &ext, 1, "br");
        } else {
            boxed_ret = raw;
        }

        LLVMBuildRet(ctx->builder, boxed_ret);
        if (tsaved) LLVMPositionBuilderAtEnd(ctx->builder, tsaved);

        fn_to_wrap = tramp;
    }

    /* Wrap fn_to_wrap in rt_value_closure with empty env */
    LLVMValueRef fn_ptr = LLVMBuildBitCast(ctx->builder, fn_to_wrap, ptr_t, "fn_ptr");
    LLVMValueRef clo_fn = get_rt_value_closure(ctx);
    LLVMTypeRef  cp[]   = {ptr_t, ptr_t, i32, i32};
    LLVMTypeRef  cft    = LLVMFunctionType(ptr_t, cp, 4, 0);
    LLVMValueRef cargs[] = {
        fn_ptr,
        LLVMConstPointerNull(ptr_t),
        LLVMConstInt(i32, 0, 0),
        LLVMConstInt(i32, declared, 0)
    };
    return LLVMBuildCall2(ctx->builder, cft, clo_fn, cargs, 4, "closure");
}

static LLVMValueRef resolve_to_closure(CodegenContext *ctx, AST *fn_node) {
    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

    if (fn_node->type == AST_SYMBOL) {
        EnvEntry *e = env_lookup(ctx->env, fn_node->symbol);

        /* Already a stored closure variable */
        if (e && e->kind == ENV_VAR && e->type && e->type->kind == TYPE_FN)
            return LLVMBuildLoad2(ctx->builder, ptr, e->value, fn_node->symbol);

        /* Named function — trampoline if needed, then wrap */
        if (e && e->kind == ENV_FUNC && e->func_ref)
            return wrap_func_as_closure(ctx, e);
    }

    /* Inline lambda or any expression — codegen returns closure value */
    CodegenResult r = codegen_expr(ctx, fn_node);

    /* If result is not already a closure (e.g. non-closure-ABI lambda),
     * look it up by name and wrap it                                    */
    if (!r.type || r.type->kind != TYPE_FN) {
        /* Try to find a freshly defined anon function */
        if (fn_node->type == AST_LAMBDA) {
            /* The AST_LAMBDA case in codegen_expr handles wrapping */
        }
    }

    return r.value;
}

static void print_defined(const char *var_name, Type *fallback_type,
                           Env *env, bool is_private) {
    EnvEntry *e = env_lookup(env, var_name);
    const char *suffix = is_private ? " (private)" : "";

    if (e && e->scheme && e->scheme->type) {
        /* Flatten nested arrows into "a -> b -> c" style */
        char buf[512] = "";
        Type *t = e->scheme->type;
        /* Apply substitution if available — use raw type */
        bool first = true;
        while (t && t->kind == TYPE_ARROW) {
            char part[128];
            snprintf(part, sizeof(part), "%s%s",
                     first ? "" : " -> ",
                     type_to_string(t->arrow_param));
            strncat(buf, part, sizeof(buf) - strlen(buf) - 1);
            t = t->arrow_ret;
            first = false;
        }
        /* Final return type */
        char ret[128];
        snprintf(ret, sizeof(ret), "%s%s",
                 first ? "" : " -> ",
                 t ? type_to_string(t) : "?");
        strncat(buf, ret, sizeof(buf) - strlen(buf) - 1);
        fprintf(stdout, "\x1b[1mDEFINED\x1b[0m [%s :: %s]%s\n",
                var_name, buf, suffix);
    } else {
        fprintf(stdout, "\x1b[1mDEFINED\x1b[0m [%s :: %s]%s\n",
                var_name, type_to_string(fallback_type), suffix);
    }
}


CodegenResult codegen_expr(CodegenContext *ctx, AST *ast) {
    CodegenResult result = {NULL, NULL};

    /* setenv("REPL_DUMP_IR", "1", 1); */

    switch (ast->type) {
    case AST_NUMBER: {
        /* fprintf(stderr, ">>> CODEGEN NUMBER: literal_str='%s', number=%g\n", */
        /*         ast->literal_str ? ast->literal_str : "NULL", ast->number); */

        Type *num_type = infer_literal_type(ast->number, ast->literal_str);

        /* fprintf(stderr, ">>> INFERRED TYPE: %s\n", type_to_string(num_type)); */

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

        if (entry->kind == ENV_FUNC) {
            /* Function used as a value — wrap in a closure */
            LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
            LLVMTypeRef  i32     = LLVMInt32TypeInContext(ctx->context);
            int lifted   = entry->lifted_count;
            int declared = entry->param_count - lifted;

            /* Build env array of captured values */
            LLVMValueRef env_ptr;
            if (lifted > 0) {
                LLVMTypeRef  arr_t = LLVMArrayType(ptr, lifted);
                LLVMValueRef arr   = LLVMBuildAlloca(ctx->builder, arr_t, "clo_env");
                for (int i = 0; i < lifted; i++) {
                    const char *cap_name = entry->params[declared + i].name;
                    EnvEntry   *cap_e    = env_lookup(ctx->env, cap_name);
                    LLVMValueRef cap_val = LLVMConstPointerNull(ptr);
                    if (cap_e && cap_e->kind == ENV_VAR && cap_e->value) {
                        LLVMTypeRef cap_llvm = type_to_llvm(ctx, cap_e->type);
                        LLVMValueRef loaded  = LLVMBuildLoad2(ctx->builder, cap_llvm,
                                                              cap_e->value, cap_name);
                        cap_val = codegen_box(ctx, loaded, cap_e->type);
                    }
                    LLVMValueRef zero    = LLVMConstInt(i32, 0, 0);
                    LLVMValueRef idx     = LLVMConstInt(i32, i, 0);
                    LLVMValueRef idxs[]  = {zero, idx};
                    LLVMValueRef slot    = LLVMBuildGEP2(ctx->builder, arr_t,
                                                         arr, idxs, 2, "slot");
                    LLVMBuildStore(ctx->builder, cap_val, slot);
                }
                env_ptr = LLVMBuildBitCast(ctx->builder, arr, ptr, "env_ptr");
            } else {
                env_ptr = LLVMConstPointerNull(ptr);
            }

            /* Cast function pointer to ptr */
            LLVMValueRef fn_ptr = LLVMBuildBitCast(ctx->builder,
                                                   entry->func_ref, ptr, "fn_ptr");

            LLVMValueRef clo_fn  = get_rt_value_closure(ctx);
            LLVMTypeRef  clo_params[] = {ptr, ptr, i32, i32};
            LLVMTypeRef  clo_ft  = LLVMFunctionType(ptr, clo_params, 4, 0);
            LLVMValueRef clo_args[] = {
                fn_ptr,
                env_ptr,
                LLVMConstInt(i32, lifted, 0),
                LLVMConstInt(i32, declared, 0)
            };
            result.value = LLVMBuildCall2(ctx->builder, clo_ft, clo_fn,
                                          clo_args, 4, "closure");
            result.type  = type_fn(NULL, 0, NULL);
            return result;
        }

        if (entry->kind == ENV_BUILTIN) {
            /* Auto-wrap builtin as a 2-arg closure trampoline.
             * The trampoline unboxes two args, calls the builtin,
             * and boxes the result. Works for all binary builtins. */
            LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
            LLVMTypeRef  i32   = LLVMInt32TypeInContext(ctx->context);
            LLVMTypeRef  i64   = LLVMInt64TypeInContext(ctx->context);
            LLVMTypeRef  dbl   = LLVMDoubleTypeInContext(ctx->context);

            static int builtin_wrap_count = 0;
            char wname[64];
            snprintf(wname, sizeof(wname), "__builtin_wrap_%s_%d",
                     ast->symbol, builtin_wrap_count++);

            // Trampoline: (ptr env, ptr a, ptr b) -> ptr
            LLVMTypeRef  wp[]  = {ptr_t, ptr_t, ptr_t};
            LLVMTypeRef  wft   = LLVMFunctionType(ptr_t, wp, 3, 0);
            LLVMValueRef wfunc = LLVMAddFunction(ctx->module, wname, wft);

            // env_count=0 means rt_closure_calln will call fn(null, n, args)
            // which matches the builtin trampoline signature (env, a, b) only
            // if n=2 and args=[a,b]. But rt_closure_calln passes (env, n, args)
            // not (env, a, b) — so the trampoline must use the calln ABI.
            // Re-generate trampoline with calln ABI: (ptr env, i32 n, ptr args).
            {
                LLVMTypeRef  calln_p[] = {ptr_t, i32, ptr_t};
                LLVMTypeRef  calln_ft  = LLVMFunctionType(ptr_t, calln_p, 3, 0);

                static int builtin_calln_count = 0;
                char cname[64];
                snprintf(cname, sizeof(cname), "__builtin_calln_%s_%d",
                         ast->symbol, builtin_calln_count++);

                LLVMValueRef      calln_w = LLVMAddFunction(ctx->module, cname, calln_ft);
                LLVMBasicBlockRef ce      = LLVMAppendBasicBlockInContext(ctx->context, calln_w, "entry");
                LLVMBasicBlockRef csaved  = LLVMGetInsertBlock(ctx->builder);
                LLVMPositionBuilderAtEnd(ctx->builder, ce);

                LLVMValueRef c_args_ptr = LLVMGetParam(calln_w, 2);
                // Load arg0 and arg1 from args array
                LLVMValueRef idx0   = LLVMConstInt(i32, 0, 0);
                LLVMValueRef idx1   = LLVMConstInt(i32, 1, 0);
                LLVMValueRef slot_a = LLVMBuildGEP2(ctx->builder, ptr_t, c_args_ptr, &idx0, 1, "slot_a");
                LLVMValueRef slot_b = LLVMBuildGEP2(ctx->builder, ptr_t, c_args_ptr, &idx1, 1, "slot_b");
                LLVMValueRef ba     = LLVMBuildLoad2(ctx->builder, ptr_t, slot_a, "ba");
                LLVMValueRef bb     = LLVMBuildLoad2(ctx->builder, ptr_t, slot_b, "bb");

                // Unbox, operate, box — same logic as before
                LLVMTypeRef  uft  = LLVMFunctionType(i64, &ptr_t, 1, 0);
                LLVMValueRef ua   = LLVMBuildCall2(ctx->builder, uft, get_rt_unbox_int(ctx), &ba, 1, "ua");
                LLVMValueRef ub   = LLVMBuildCall2(ctx->builder, uft, get_rt_unbox_int(ctx), &bb, 1, "ub");

                LLVMValueRef wr = NULL;
                const char *sym = ast->symbol;
                if      (strcmp(sym, "+")   == 0) wr = LLVMBuildAdd (ctx->builder, ua, ub, "r");
                else if (strcmp(sym, "-")   == 0) wr = LLVMBuildSub (ctx->builder, ua, ub, "r");
                else if (strcmp(sym, "*")   == 0) wr = LLVMBuildMul (ctx->builder, ua, ub, "r");
                else if (strcmp(sym, "/")   == 0) wr = LLVMBuildSDiv(ctx->builder, ua, ub, "r");
                else if (strcmp(sym, "mod") == 0 ||
                         strcmp(sym, "%")   == 0) wr = LLVMBuildSRem(ctx->builder, ua, ub, "r");
                else if (strcmp(sym, "=")   == 0) {
                    LLVMValueRef cmp = LLVMBuildICmp(ctx->builder, LLVMIntEQ,  ua, ub, "cmp");
                    wr = LLVMBuildZExt(ctx->builder, cmp, i64, "r");
                } else if (strcmp(sym, "<")  == 0) {
                    LLVMValueRef cmp = LLVMBuildICmp(ctx->builder, LLVMIntSLT, ua, ub, "cmp");
                    wr = LLVMBuildZExt(ctx->builder, cmp, i64, "r");
                } else if (strcmp(sym, ">")  == 0) {
                    LLVMValueRef cmp = LLVMBuildICmp(ctx->builder, LLVMIntSGT, ua, ub, "cmp");
                    wr = LLVMBuildZExt(ctx->builder, cmp, i64, "r");
                } else if (strcmp(sym, "<=") == 0) {
                    LLVMValueRef cmp = LLVMBuildICmp(ctx->builder, LLVMIntSLE, ua, ub, "cmp");
                    wr = LLVMBuildZExt(ctx->builder, cmp, i64, "r");
                } else if (strcmp(sym, ">=") == 0) {
                    LLVMValueRef cmp = LLVMBuildICmp(ctx->builder, LLVMIntSGE, ua, ub, "cmp");
                    wr = LLVMBuildZExt(ctx->builder, cmp, i64, "r");
                } else {
                    wr = LLVMConstInt(i64, 0, 0);
                }

                LLVMTypeRef  bft    = LLVMFunctionType(ptr_t, &i64, 1, 0);
                LLVMValueRef boxed  = LLVMBuildCall2(ctx->builder, bft,
                                          get_rt_value_int(ctx), &wr, 1, "boxed");
                LLVMBuildRet(ctx->builder, boxed);
                if (csaved) LLVMPositionBuilderAtEnd(ctx->builder, csaved);

                LLVMValueRef fn_ptr2 = LLVMBuildBitCast(ctx->builder, calln_w, ptr_t, "fn_ptr");
                LLVMValueRef clo_fn2  = get_rt_value_closure(ctx);
                LLVMTypeRef  cp2[]    = {ptr_t, ptr_t, i32, i32};
                LLVMTypeRef  cft2     = LLVMFunctionType(ptr_t, cp2, 4, 0);
                LLVMValueRef cargs2[] = {
                    fn_ptr2,
                    LLVMConstPointerNull(ptr_t),
                    LLVMConstInt(i32, 0, 0),
                    LLVMConstInt(i32, 2, 0)
                };
                result.value = LLVMBuildCall2(ctx->builder, cft2, clo_fn2, cargs2, 4, "builtin_clo");
                result.type  = type_fn(NULL, 0, NULL);
                return result;
            }
        }

        if (!entry->type) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' has no type information",
                          parser_get_filename(), ast->line, ast->column, ast->symbol);
        }

        /* fprintf(stderr, "[symbol load] name='%s' entry=%p entry->value=%p is_global=%d\n", */
        /*         ast->symbol, (void*)entry, (void*)entry->value, */
        /*         entry->value ? (LLVMIsAGlobalVariable(entry->value) != NULL) : -1); */

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

    case AST_SET: {
        LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

        LLVMValueRef set_fn = get_rt_set_new(ctx);
        LLVMTypeRef  set_ft = LLVMFunctionType(ptr, NULL, 0, 0);
        LLVMValueRef set    = LLVMBuildCall2(ctx->builder, set_ft,
                                             set_fn, NULL, 0, "set");

        LLVMValueRef conj_fn = get_rt_set_conj(ctx);
        LLVMTypeRef  conj_params[] = {ptr, ptr};
        LLVMTypeRef  conj_ft = LLVMFunctionType(ptr, conj_params, 2, 0);

        for (size_t i = 0; i < ast->set.element_count; i++) {
            CodegenResult elem = codegen_expr(ctx, ast->set.elements[i]);
            LLVMValueRef  boxed = codegen_box(ctx, elem.value, elem.type);
            LLVMValueRef  args[] = {set, boxed};
            set = LLVMBuildCall2(ctx->builder, conj_ft, conj_fn, args, 2, "set");
        }

        LLVMValueRef wrap_fn = get_rt_value_set(ctx);
        LLVMTypeRef  wrap_ft = LLVMFunctionType(ptr, &ptr, 1, 0);
        LLVMValueRef args[]  = {set};
        result.value = LLVMBuildCall2(ctx->builder, wrap_ft, wrap_fn, args, 1, "setval");
        result.type  = type_set();
        return result;
    }

    case AST_MAP: {
        LLVMTypeRef  ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
        LLVMValueRef map_fn = get_rt_map_new(ctx);
        LLVMTypeRef  map_ft = LLVMFunctionType(ptr, NULL, 0, 0);
        LLVMValueRef map    = LLVMBuildCall2(ctx->builder, map_ft, map_fn, NULL, 0, "map");

        LLVMValueRef assoc_fn = get_rt_map_assoc_mut(ctx);
        LLVMTypeRef  ap[]     = {ptr, ptr, ptr};
        LLVMTypeRef  aft      = LLVMFunctionType(ptr, ap, 3, 0);

        for (size_t i = 0; i < ast->map.count; i++) {
            CodegenResult k = codegen_expr(ctx, ast->map.keys[i]);
            CodegenResult v = codegen_expr(ctx, ast->map.vals[i]);
            LLVMValueRef bk = codegen_box(ctx, k.value, k.type);
            LLVMValueRef bv = codegen_box(ctx, v.value, v.type);
            LLVMValueRef aa[] = {map, bk, bv};
            map = LLVMBuildCall2(ctx->builder, aft, assoc_fn, aa, 3, "map");
        }

        LLVMValueRef wrap_fn = get_rt_value_map(ctx);
        LLVMTypeRef  wft     = LLVMFunctionType(ptr, &ptr, 1, 0);
        LLVMValueRef wa[]    = {map};
        result.value = LLVMBuildCall2(ctx->builder, wft, wrap_fn, wa, 1, "mapval");
        result.type  = type_map();
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

      //// define

            if (strcmp(head->symbol, "define") == 0) {
                if (ast->list.count < 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'define' requires at least 2 arguments",
                            parser_get_filename(), ast->line, ast->column);
                }

                AST *name_expr  = ast->list.items[1];
                AST *value_expr = ast->list.items[2];

                char *var_name    = NULL;
                Type *explicit_type = NULL;

                if (name_expr->type == AST_LIST) {
                    explicit_type = parse_type_annotation(name_expr);
                    if (explicit_type && name_expr->list.count > 0 &&
                        name_expr->list.items[0]->type == AST_SYMBOL) {
                        var_name = name_expr->list.items[0]->symbol;
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

                if (value_expr->type == AST_LAMBDA) {
                    AST *lambda = value_expr;

                    TypeScheme *hm_scheme = env_hm_infer_define(ctx->env, var_name,
                                                                lambda, parser_get_filename());

           ///// Collect free variables for inner functions

                    char **captured_vars  = NULL;
                    int    captured_count = 0;

                    bool is_inner = (LLVMGetInsertBlock(ctx->builder) != NULL &&
                                     LLVMGetBasicBlockParent(
                                         LLVMGetInsertBlock(ctx->builder)) != NULL &&
                                     LLVMGetBasicBlockParent(
                                         LLVMGetInsertBlock(ctx->builder)) != ctx->init_fn);

                    if (is_inner) {
                        const char **bound = malloc(sizeof(char*) *
                                             (lambda->lambda.param_count ? lambda->lambda.param_count : 1));
                        for (int i = 0; i < lambda->lambda.param_count; i++)
                            bound[i] = lambda->lambda.params[i].name;
                        const char *self_name = var_name;
                        for (int i = 0; i < lambda->lambda.body_count; i++)
                            collect_free_vars(lambda->lambda.body_exprs[i],
                                              bound, lambda->lambda.param_count,
                                              &self_name, 1,
                                              &captured_vars, &captured_count,
                                              ctx->env);
                        free(bound);
                    }

                    // Closure ABI only for inner functions with captures.
                    // Top-level polymorphic functions use typed ABI with stub body.
                    bool use_closure_abi = is_inner || captured_count > 0;

           ///// Build parameter types

                    int total_params     = lambda->lambda.param_count;
                    int llvm_param_count = use_closure_abi ? 3 : total_params;

                    LLVMTypeRef  ptr_t       = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef  i32_t       = LLVMInt32TypeInContext(ctx->context);
                    LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) *
                                                      (llvm_param_count ? llvm_param_count : 1));
                    EnvParam    *env_params  = malloc(sizeof(EnvParam) *
                                                      (total_params ? total_params : 1));

                    if (use_closure_abi) {
                        param_types[0] = ptr_t;   // env
                        param_types[1] = i32_t;   // n
                        param_types[2] = ptr_t;   // args
                    }


                    bool all_params_unknown = (total_params > 0);
                    for (int i = 0; i < total_params; i++) {
                        ASTParam *param = &lambda->lambda.params[i];
                        if (param->is_rest) {
                            env_params[i].name = strdup(param->name);
                            // Typed rest . [args :: T] stores type_list(T) so the
                            // call site can enforce element types. Bare . args stays
                            // type_list(NULL) meaning untyped.
                            if (param->type_name) {
                                Type *elem = type_from_name(param->type_name);
                                if (!elem)
                                    CODEGEN_ERROR(ctx,
                                        "%s:%d:%d: error: unknown type '%s' for rest parameter '%s'",
                                        parser_get_filename(), lambda->line, lambda->column,
                                        param->type_name, param->name);
                                env_params[i].type = type_list(elem);
                            } else {
                                env_params[i].type = type_list(NULL);
                            }
                            /* closure ABI: param_types already set, skip */
                            if (!use_closure_abi)
                                param_types[i] =
                                    LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                            all_params_unknown = false;
                            continue;
                        }
                        Type *param_type = NULL;
                        if (param->type_name) {
                            param_type = type_from_name(param->type_name);
                            if (!param_type)
                                CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown parameter type '%s' (param name='%s' is_rest=%d is_anon=%d)",
                                              parser_get_filename(), lambda->line, lambda->column,
                                              param->type_name,
                                              param->name ? param->name : "NULL",
                                              param->is_rest,
                                              param->is_anon);
                            all_params_unknown = false;
                        } else {
                            param_type = type_unknown();
                        }
                        env_params[i].name = strdup(param->name);
                        env_params[i].type = param_type;

                        /* closure ABI: param_types[0..2] already set above, skip */
                        if (use_closure_abi) { continue; }

                        int llvm_idx = i;
                        if (param_type->kind == TYPE_UNKNOWN && hm_scheme) {
                            // Use HM-inferred concrete param type if available
                            Type *hm_param = NULL;
                            Type *t = hm_scheme->type;
                            for (int j = 0; j < i && t && t->kind == TYPE_ARROW; j++)
                                t = t->arrow_ret;
                            if (t && t->kind == TYPE_ARROW && t->arrow_param &&
                                t->arrow_param->kind != TYPE_VAR &&
                                t->arrow_param->kind != TYPE_UNKNOWN)
                                hm_param = t->arrow_param;
                            param_types[llvm_idx] = hm_param
                                                  ? type_to_llvm(ctx, hm_param)
                                                  : ptr_t;
                            if (hm_param) {
                                // Don't free — type_unknown() returns a fresh alloc
                                // but env_params[i].type may point to a shared type.
                                // Just replace with clone, old pointer leaks minimally.
                                env_params[i].type = type_clone(hm_param);
                                all_params_unknown = false;
                            }

                        } else {
                            param_types[llvm_idx] = type_to_llvm(ctx, param_type);
                        }

                    }

           ///// Determine return type

                    Type *ret_type = NULL;
                    if (lambda->lambda.return_type) {
                        ret_type = type_from_name(lambda->lambda.return_type);
                        if (!ret_type) ret_type = type_unknown();
                    } else {
                        ret_type = type_unknown();
                    }
                    // Use HM-inferred return type if still unknown
                    if (ret_type->kind == TYPE_UNKNOWN && hm_scheme) {
                        Type *t = hm_scheme->type;
                        while (t && t->kind == TYPE_ARROW)
                            t = t->arrow_ret;
                        if (t && t->kind != TYPE_VAR && t->kind != TYPE_UNKNOWN)
                            ret_type = type_clone(t);
                    }


                    // Stub functions and closure ABI use ptr return
                    // Only emit a stub if HM scheme has quantified type vars.
                    // Monomorphic functions (e.g. add :: Int->Int->Int inferred)
                    // should compile their body normally even without annotations.
                    bool has_type_vars = hm_scheme && hm_scheme->quantified_count > 0;
                    bool is_poly_stub  = !use_closure_abi && all_params_unknown && has_type_vars;

                    LLVMTypeRef ret_llvm_type = (use_closure_abi || is_poly_stub)
                                              ? ptr_t
                                              : type_to_llvm(ctx, ret_type);


                    LLVMTypeRef func_type = LLVMFunctionType(ret_llvm_type, param_types,
                                                             llvm_param_count, 0);
                    ///// Create LLVM function

                    LLVMValueRef func = LLVMAddFunction(ctx->module, var_name, func_type);

                    if (lambda->lambda.naked) {
                        unsigned kind = LLVMGetEnumAttributeKindForName("naked", 5);
                        if (kind != 0) {
                            LLVMAttributeRef attr = LLVMCreateEnumAttribute(ctx->context, kind, 0);
                            LLVMAddAttributeAtIndex(func, LLVMAttributeFunctionIndex, attr);
                        } else {
                            LLVMAttributeRef attr = LLVMCreateStringAttribute(
                                ctx->context, "naked", 5, "", 0);
                            LLVMAddAttributeAtIndex(func, LLVMAttributeFunctionIndex, attr);
                        }
                    }

                    LLVMDumpValue(func);

           ///// Forward-declare in env (enables recursion)

                    env_insert_func(ctx->env, var_name,
                                    clone_params(env_params, total_params),
                                    total_params, type_clone(ret_type),
                                    func, lambda->lambda.docstring);
                    if (hm_scheme) env_set_scheme(ctx->env, var_name, hm_scheme);
                    EnvEntry *e_fwd = env_lookup(ctx->env, var_name);
                    if (e_fwd) {
                        e_fwd->llvm_name      = strdup(LLVMGetValueName(func));
                        e_fwd->lifted_count   = 0;
                        e_fwd->is_closure_abi = use_closure_abi;
                        // Store source_ast after body desugaring happens below.
                        // We set it to NULL here and update it after the body
                        // codegen loop where pmatch is expanded.
                        e_fwd->source_ast     = ast_clone(lambda);
                    }

           ///// Build function body

                    LLVMBasicBlockRef entry_block = LLVMAppendBasicBlockInContext(
                                                        ctx->context, func, "entry");
                    LLVMBasicBlockRef saved_block = LLVMGetInsertBlock(ctx->builder);
                    LLVMPositionBuilderAtEnd(ctx->builder, entry_block);

                    Env *saved_env = ctx->env;
                    ctx->env = env_create_child(saved_env);

                    // env param (index 0 in closure ABI)
                    LLVMValueRef env_llvm_param = NULL;
                    if (use_closure_abi) {
                        env_llvm_param = LLVMGetParam(func, 0);
                        LLVMSetValueName2(env_llvm_param, "env", 3);
                        /* param 1 = n (ignored), param 2 = args array */
                        LLVMValueRef args_param = LLVMGetParam(func, 2);

                        for (int i = 0; i < total_params; i++) {
                            if (lambda->lambda.naked) continue;
                            Type        *param_type = env_params[i].type;
                            LLVMTypeRef  typed_llvm = type_to_llvm(ctx, param_type);

                            /* Load boxed arg from args[i] — args is RuntimeValue**,
                             * so GEP must index into an array of ptr_t elements.  */
                            /* args_param is RuntimeValue** — index with single i64 index */
                            LLVMValueRef idx   = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), i, 0);
                            LLVMValueRef slot  = LLVMBuildGEP2(ctx->builder, ptr_t,
                                                               args_param, &idx, 1, "arg_slot");
                            LLVMValueRef boxed = LLVMBuildLoad2(ctx->builder, ptr_t,
                                                                slot, "boxed_arg");

                            /* Unbox based on known type */
                            LLVMValueRef unboxed = boxed;
                            if (type_is_integer(param_type)) {
                                LLVMTypeRef uft = LLVMFunctionType(
                                    LLVMInt64TypeInContext(ctx->context), &ptr_t, 1, 0);
                                unboxed = LLVMBuildCall2(ctx->builder, uft,
                                              get_rt_unbox_int(ctx), &boxed, 1, "unboxed");
                            } else if (type_is_float(param_type)) {
                                LLVMTypeRef uft = LLVMFunctionType(
                                    LLVMDoubleTypeInContext(ctx->context), &ptr_t, 1, 0);
                                unboxed = LLVMBuildCall2(ctx->builder, uft,
                                              get_rt_unbox_float(ctx), &boxed, 1, "unboxed");
                            } else if (param_type->kind == TYPE_CHAR) {
                                LLVMTypeRef uft = LLVMFunctionType(
                                    LLVMInt8TypeInContext(ctx->context), &ptr_t, 1, 0);
                                unboxed = LLVMBuildCall2(ctx->builder, uft,
                                              get_rt_unbox_char(ctx), &boxed, 1, "unboxed");
                            } else if (param_type->kind == TYPE_BOOL) {
                                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                                LLVMTypeRef i1  = LLVMInt1TypeInContext(ctx->context);
                                LLVMTypeRef uft = LLVMFunctionType(i64, &ptr_t, 1, 0);
                                LLVMValueRef as_i64 = LLVMBuildCall2(ctx->builder, uft,
                                                          get_rt_unbox_int(ctx), &boxed, 1, "ub");
                                unboxed = LLVMBuildTrunc(ctx->builder, as_i64, i1, "unboxed");
                            }

                            LLVMValueRef alloca = LLVMBuildAlloca(ctx->builder, typed_llvm,
                                                                   lambda->lambda.params[i].name);
                            LLVMBuildStore(ctx->builder, unboxed, alloca);
                            env_insert(ctx->env, lambda->lambda.params[i].name,
                                       type_clone(param_type), alloca);
                        }
                    } else {
                        // Typed ABI — bind parameters normally
                        for (int i = 0; i < total_params; i++) {
                            int          llvm_idx   = i;
                            LLVMValueRef param      = LLVMGetParam(func, llvm_idx);
                            Type        *param_type = env_params[i].type;

                            LLVMSetValueName2(param, lambda->lambda.params[i].name,
                                              strlen(lambda->lambda.params[i].name));

                            if (lambda->lambda.naked) continue;

                            LLVMTypeRef  typed_llvm = type_to_llvm(ctx, param_type);
                            LLVMValueRef alloca     = LLVMBuildAlloca(ctx->builder, typed_llvm,
                                                                       lambda->lambda.params[i].name);
                            LLVMBuildStore(ctx->builder, param, alloca);
                            env_insert(ctx->env, lambda->lambda.params[i].name,
                                       type_clone(param_type), alloca);
                        }
                    }

                    // Load captured variables from env array (closure ABI only)
                    if (use_closure_abi && captured_count > 0 && env_llvm_param) {
                        LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
                        for (int i = 0; i < captured_count; i++) {
                            EnvEntry *cap_e  = env_lookup(saved_env, captured_vars[i]);
                            Type *cap_type   = (cap_e && cap_e->type) ? cap_e->type : type_unknown();
                            LLVMTypeRef cap_llvm = type_to_llvm(ctx, cap_type);

                            LLVMValueRef idx   = LLVMConstInt(i32, i, 0);
                            LLVMValueRef gep   = LLVMBuildGEP2(ctx->builder, ptr_t,
                                                                env_llvm_param, &idx, 1, "cap_ptr");
                            LLVMValueRef boxed = LLVMBuildLoad2(ctx->builder, ptr_t,
                                                                 gep, "cap_boxed");
                            LLVMValueRef unboxed = boxed;
                            if (type_is_integer(cap_type)) {
                                LLVMTypeRef uft = LLVMFunctionType(
                                    LLVMInt64TypeInContext(ctx->context), &ptr_t, 1, 0);
                                unboxed = LLVMBuildCall2(ctx->builder, uft,
                                              get_rt_unbox_int(ctx), &boxed, 1, "cap");
                            } else if (type_is_float(cap_type)) {
                                LLVMTypeRef uft = LLVMFunctionType(
                                    LLVMDoubleTypeInContext(ctx->context), &ptr_t, 1, 0);
                                unboxed = LLVMBuildCall2(ctx->builder, uft,
                                              get_rt_unbox_float(ctx), &boxed, 1, "cap");
                            } else if (cap_type->kind == TYPE_BOOL) {
                                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                                LLVMTypeRef i1  = LLVMInt1TypeInContext(ctx->context);
                                LLVMTypeRef uft = LLVMFunctionType(i64, &ptr_t, 1, 0);
                                LLVMValueRef as_i64 = LLVMBuildCall2(ctx->builder, uft,
                                                          get_rt_unbox_int(ctx), &boxed, 1, "ub");
                                unboxed = LLVMBuildTrunc(ctx->builder, as_i64, i1, "cap");
                            }
                            LLVMValueRef alloca = LLVMBuildAlloca(ctx->builder, cap_llvm,
                                                                   captured_vars[i]);
                            LLVMBuildStore(ctx->builder, unboxed, alloca);
                            env_insert(ctx->env, captured_vars[i], type_clone(cap_type), alloca);
                        }
                    }

           ///// Codegen body

                    LLVMValueRef ret_value;

                    if (is_poly_stub) {
                        // Polymorphic stub — when called via closure dispatch,
                        // unbox args, call a runtime dispatch helper, box result.
                        // For now emit an rt_unbox_int + add + rt_value_int
                        // for the common numeric case.
                        // Real fix: generate a dispatch thunk per call site type.
                        //
                        // Emit: unbox all ptr params, do the operation, box result
                        // This only works if all params are the same numeric type —
                        // which is true for +, -, *, add, mul etc.
                        LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);

                        // Unbox all params and re-do the body with i64 types
                        // by inserting them into a child env with TYPE_INT
                        Env *stub_env = env_create_child(ctx->env);
                        for (int i = 0; i < total_params; i++) {
                            int llvm_idx = use_closure_abi ? i + 1 : i;
                            LLVMValueRef raw_param = LLVMGetParam(func, llvm_idx);
                            LLVMTypeRef uft = LLVMFunctionType(i64, &ptr_t, 1, 0);
                            LLVMValueRef unboxed = LLVMBuildCall2(ctx->builder, uft,
                                                      get_rt_unbox_int(ctx),
                                                      &raw_param, 1, "stub_ub");
                            LLVMValueRef alloca = LLVMBuildAlloca(ctx->builder,
                                                                   i64, env_params[i].name);
                            LLVMBuildStore(ctx->builder, unboxed, alloca);
                            env_insert(stub_env, env_params[i].name, type_int(), alloca);
                        }
                        Env *prev_env = ctx->env;
                        ctx->env = stub_env;
                        const char *prev_fn = ctx->current_function_name;
                        ctx->current_function_name = var_name;
                        CodegenResult stub_body = {NULL, NULL};
                        for (int i = 0; i < lambda->lambda.body_count; i++)
                            stub_body = codegen_expr(ctx, lambda->lambda.body_exprs[i]);
                        ctx->current_function_name = prev_fn;
                        ctx->env = prev_env;
                        env_free(stub_env);

                        if (stub_body.value) {
                            LLVMValueRef box_val = stub_body.value;
                            LLVMTypeRef  actual  = LLVMTypeOf(box_val);
                            if (actual == ptr_t) {
                                // Already a RuntimeValue* — return as-is
                                ret_value = box_val;
                            } else {
                                if (actual != i64)
                                    box_val = LLVMBuildSExtOrBitCast(ctx->builder,
                                                                      box_val, i64, "ext");
                                LLVMTypeRef bft = LLVMFunctionType(ptr_t, &i64, 1, 0);
                                ret_value = LLVMBuildCall2(ctx->builder, bft,
                                               get_rt_value_int(ctx), &box_val, 1, "boxed");
                            }
                        } else {
                            ret_value = LLVMConstPointerNull(ptr_t);
                        }


                    } else {
                        CodegenResult body_result = {NULL, NULL};

                        if (lambda->lambda.body->type == AST_ASM) {
                            RegisterAllocator reg_alloc;
                            reg_alloc_init(&reg_alloc);
                            AsmContext asm_ctx = {
                                .params      = env_params,
                                .param_count = total_params,
                                .reg_alloc   = &reg_alloc,
                                .naked       = lambda->lambda.naked
                            };
                            int asm_inst_count;
                            AsmInstruction *asm_instructions = preprocess_asm(
                                lambda->lambda.body, &asm_ctx, &asm_inst_count);
                            if (lambda->lambda.naked) {
                                body_result.value = codegen_inline_asm(
                                    ctx->context, ctx->builder, asm_instructions,
                                    asm_inst_count, ret_type, NULL, 0, true);
                            } else {
                                LLVMValueRef *param_values = malloc(
                                    sizeof(LLVMValueRef) * total_params);
                                for (int i = 0; i < total_params; i++)
                                    param_values[i] = LLVMGetParam(func, i);
                                body_result.value = codegen_inline_asm(
                                    ctx->context, ctx->builder, asm_instructions,
                                    asm_inst_count, ret_type, param_values,
                                    total_params, false);
                                free(param_values);
                            }
                            body_result.type = ret_type;
                            free_asm_instructions(asm_instructions, asm_inst_count);
                        } else {
                            const char *prev_fn   = ctx->current_function_name;
                            ctx->current_function_name = var_name;
                            for (int i = 0; i < lambda->lambda.body_count; i++)
                                body_result = codegen_expr(ctx, lambda->lambda.body_exprs[i]);


                            ctx->current_function_name = prev_fn;
                            if (!body_result.type) {
                                body_result.type  = type_clone(ret_type);
                                body_result.value = LLVMConstInt(
                                    LLVMInt64TypeInContext(ctx->context), 0, 0);
                            }
                        }

              ///// Coerce return value to declared return type

                        ret_value = body_result.value;
                        if (body_result.type && ret_type) {
                            LLVMTypeRef i1    = LLVMInt1TypeInContext(ctx->context);
                            LLVMTypeRef i64   = LLVMInt64TypeInContext(ctx->context);
                            LLVMTypeRef dbl   = LLVMDoubleTypeInContext(ctx->context);

                            LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                            if (LLVMTypeOf(ret_value) == ptr_t && ret_llvm_type == ptr_t) {
                                // both pointer — no coercion needed
                            } else if (type_is_integer(ret_type) && type_is_float(body_result.type)) {
                                ret_value = LLVMBuildFPToSI(ctx->builder, ret_value,
                                                            ret_llvm_type, "ret_conv");
                            } else if (type_is_float(ret_type) && type_is_integer(body_result.type)) {
                                ret_value = LLVMBuildSIToFP(ctx->builder, ret_value,
                                                            ret_llvm_type, "ret_conv");
                            } else if (ret_type->kind == TYPE_CHAR &&
                                       type_is_integer(body_result.type)) {
                                ret_value = LLVMBuildTrunc(ctx->builder, ret_value,
                                    LLVMInt8TypeInContext(ctx->context), "ret_char");
                            } else if (type_is_integer(ret_type) &&
                                       body_result.type->kind == TYPE_CHAR) {
                                ret_value = LLVMBuildZExt(ctx->builder, ret_value,
                                                          ret_llvm_type, "ret_int");
                            } else if (ret_type->kind == TYPE_BOOL) {
                                if (LLVMTypeOf(ret_value) == ptr_t) {
                                    LLVMTypeRef uft = LLVMFunctionType(i64, &ptr_t, 1, 0);
                                    ret_value = LLVMBuildCall2(ctx->builder, uft,
                                                  get_rt_unbox_int(ctx), &ret_value, 1, "unbox_bool");
                                    ret_value = LLVMBuildTrunc(ctx->builder, ret_value, i1, "to_bool");
                                } else if (LLVMTypeOf(ret_value) != i1) {
                                    ret_value = LLVMBuildTrunc(ctx->builder, ret_value,
                                                               i1, "to_bool");
                                }
                            } else if (type_is_integer(ret_type) &&
                                       LLVMTypeOf(ret_value) == i1) {
                                ret_value = LLVMBuildZExt(ctx->builder, ret_value,
                                                          ret_llvm_type, "bool_to_int");
                            } else if (type_is_integer(ret_type) &&
                                       LLVMTypeOf(ret_value) == ptr_t &&
                                       !use_closure_abi) {
                                /* Only unbox if we are in typed ABI —
                                 * closure ABI keeps values boxed until the
                                 * boxing block above handles them.          */
                                LLVMTypeRef uft = LLVMFunctionType(ret_llvm_type, &ptr_t, 1, 0);
                                ret_value = LLVMBuildCall2(ctx->builder, uft,
                                              get_rt_unbox_int(ctx), &ret_value, 1, "unboxed_ret");
                            }
                        }

              ///// Box return value for closure ABI

                        if (use_closure_abi && ret_value) {
                            LLVMTypeRef actual = LLVMTypeOf(ret_value);
                            LLVMTypeRef i64    = LLVMInt64TypeInContext(ctx->context);
                            LLVMTypeRef dbl    = LLVMDoubleTypeInContext(ctx->context);
                            LLVMTypeRef i1     = LLVMInt1TypeInContext(ctx->context);
                            LLVMTypeRef i8     = LLVMInt8TypeInContext(ctx->context);
                            LLVMTypeRef i32    = LLVMInt32TypeInContext(ctx->context);

                            if (actual == ptr_t) {
                                /* already a RuntimeValue* — return as-is */
                            } else if (actual == i64 || actual == i32) {
                                LLVMValueRef ext = (actual != i64)
                                    ? LLVMBuildSExt(ctx->builder, ret_value, i64, "ext")
                                    : ret_value;
                                LLVMTypeRef bft = LLVMFunctionType(ptr_t, &i64, 1, 0);
                                ret_value = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_int(ctx), &ext, 1, "boxed_ret");
                            } else if (actual == dbl) {
                                LLVMTypeRef bft = LLVMFunctionType(ptr_t, &dbl, 1, 0);
                                ret_value = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_float(ctx), &ret_value, 1, "boxed_ret");
                            } else if (actual == i1) {
                                LLVMValueRef ext = LLVMBuildZExt(ctx->builder,
                                                                   ret_value, i64, "bool_ext");
                                LLVMTypeRef bft = LLVMFunctionType(ptr_t, &i64, 1, 0);
                                ret_value = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_int(ctx), &ext, 1, "boxed_ret");
                            } else if (actual == i8) {
                                LLVMValueRef ext = LLVMBuildZExt(ctx->builder,
                                                                   ret_value, i64, "char_ext");
                                LLVMTypeRef bft = LLVMFunctionType(ptr_t, &i64, 1, 0);
                                ret_value = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_int(ctx), &ext, 1, "boxed_ret");
                            } else {
                                /* unknown type — box as int best-effort */
                                LLVMValueRef cast = LLVMBuildBitCast(ctx->builder,
                                                        ret_value, i64, "cast");
                                LLVMTypeRef bft = LLVMFunctionType(ptr_t, &i64, 1, 0);
                                ret_value = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_int(ctx), &cast, 1, "boxed_ret");
                            }
                        }

                    }

           ///// Emit return

                    if (lambda->lambda.naked)
                        LLVMBuildUnreachable(ctx->builder);
                    else
                        LLVMBuildRet(ctx->builder, ret_value);

           ///// Restore env and builder

                    env_free(ctx->env);
                    ctx->env = saved_env;
                    if (saved_block)
                        LLVMPositionBuilderAtEnd(ctx->builder, saved_block);

           ///// Emit closure value in outer block (closure ABI only)

                    LLVMValueRef closure_val = NULL;
                    if (use_closure_abi) {
                        LLVMValueRef fn_ptr = LLVMBuildBitCast(ctx->builder, func,
                                                                ptr_t, "fn_ptr");
                        LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
                        LLVMValueRef env_array_ptr;

                        if (captured_count > 0) {
                            LLVMTypeRef  arr_t = LLVMArrayType(ptr_t, captured_count);
                            LLVMValueRef arr   = LLVMBuildAlloca(ctx->builder, arr_t, "clo_env");
                            for (int i = 0; i < captured_count; i++) {
                                EnvEntry    *cap_e   = env_lookup(ctx->env, captured_vars[i]);
                                LLVMValueRef cap_val = LLVMConstPointerNull(ptr_t);
                                if (cap_e && cap_e->kind == ENV_VAR && cap_e->value) {
                                    LLVMTypeRef  cap_llvm = type_to_llvm(ctx, cap_e->type);
                                    LLVMValueRef loaded   = LLVMBuildLoad2(ctx->builder, cap_llvm,
                                                                cap_e->value, captured_vars[i]);
                                    cap_val = codegen_box(ctx, loaded, cap_e->type);
                                }
                                LLVMValueRef zero   = LLVMConstInt(i32, 0, 0);
                                LLVMValueRef idx    = LLVMConstInt(i32, i, 0);
                                LLVMValueRef idxs[] = {zero, idx};
                                LLVMValueRef slot   = LLVMBuildGEP2(ctx->builder, arr_t,
                                                                     arr, idxs, 2, "slot");
                                LLVMBuildStore(ctx->builder, cap_val, slot);
                            }
                            env_array_ptr = LLVMBuildBitCast(ctx->builder, arr, ptr_t, "env_ptr");
                        } else {
                            env_array_ptr = LLVMConstPointerNull(ptr_t);
                        }

                        LLVMTypeRef  i32t       = LLVMInt32TypeInContext(ctx->context);
                        LLVMValueRef clo_fn     = get_rt_value_closure(ctx);
                        LLVMTypeRef  clo_params[] = {ptr_t, ptr_t, i32t, i32t};
                        LLVMTypeRef  clo_ft     = LLVMFunctionType(ptr_t, clo_params, 4, 0);
                        LLVMValueRef clo_args[] = {
                            fn_ptr, env_array_ptr,
                            LLVMConstInt(i32t, captured_count, 0),
                            LLVMConstInt(i32t, total_params, 0)
                        };
                        closure_val = LLVMBuildCall2(ctx->builder, clo_ft,
                                                     clo_fn, clo_args, 4, "closure");
                    }

           ///// Register in symbol table

                    env_insert_func(ctx->env, var_name, env_params, total_params,
                                    ret_type, func, lambda->lambda.docstring);
                    if (hm_scheme) env_set_scheme(ctx->env, var_name, hm_scheme);
                    EnvEntry *efinal = env_lookup(ctx->env, var_name);
                    if (efinal) {
                        efinal->lifted_count   = 0;
                        efinal->is_closure_abi = use_closure_abi;
                        // Clone the lambda AFTER body desugaring so source_ast
                        // contains the expanded if-chain, not the raw AST_PMATCH.
                        efinal->source_ast     = ast_clone(lambda);
                    }

                    if (ast->lambda.alias_name) {
                        env_insert_func(ctx->env, ast->lambda.alias_name,
                                        clone_params(env_params, total_params),
                                        total_params, type_clone(ret_type),
                                        func, lambda->lambda.docstring);
                        if (hm_scheme)
                            env_set_scheme(ctx->env, ast->lambda.alias_name, hm_scheme);
                        printf("Alias: %s -> %s\n", ast->lambda.alias_name, var_name);
                    }

                    ctx->current_function_name = NULL;

                    if (strncmp(var_name, "__hof_lambda_", 13) != 0 &&
                        strncmp(var_name, "__anon_", 7) != 0)
                        check_predicate_name(ctx, var_name, ret_type, ast);

                    bool is_private = ctx->module_ctx &&
                                      !should_export_symbol(ctx->module_ctx, var_name);
                    print_defined(var_name, ret_type, ctx->env, is_private);

                    free(param_types);

                    // Return closure value or dummy for typed functions

                    if (use_closure_abi && closure_val) {
                        if (!is_at_top_level(ctx)) {
                            LLVMValueRef alloca = LLVMBuildAlloca(ctx->builder, ptr_t, var_name);
                            LLVMBuildStore(ctx->builder, closure_val, alloca);
                            env_insert(ctx->env, var_name, type_fn(NULL, 0, NULL), alloca);
                        }
                        for (int i = 0; i < captured_count; i++) free(captured_vars[i]);
                        free(captured_vars);
                        result.type  = type_fn(NULL, 0, NULL);
                        result.value = closure_val;
                        return result;
                    }

                    for (int i = 0; i < captured_count; i++) free(captured_vars[i]);
                    free(captured_vars);
                    result.type  = type_unknown();
                    result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    return result;
                }

                // Non-lambda value: (define x 42) or (define [x :: Int] 42)
                CodegenResult value_result;
                if (value_expr->type == AST_ARRAY && value_expr->array.element_count == 0) {
                    if (!explicit_type || explicit_type->kind != TYPE_ARR) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: cannot infer type of empty array — explicit type annotation required",
                                parser_get_filename(), ast->line, ast->column);
                    }
                    value_result.type  = explicit_type;
                    value_result.value = NULL;
                } else {
                    value_result = codegen_expr(ctx, value_expr);
                }

                // Validate array size mismatch
                if (explicit_type && explicit_type->kind == TYPE_ARR &&
                    explicit_type->arr_size >= 0 &&
                    value_result.type && value_result.type->kind == TYPE_ARR) {
                    int declared_size = explicit_type->arr_size;
                    int actual_size   = value_result.type->arr_size;
                    if (actual_size > 0 && actual_size != declared_size) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: array size mismatch — declared %d but got %d elements",
                                parser_get_filename(), ast->line, ast->column,
                                declared_size, actual_size);
                    }
                }

                Type *inferred_type = value_result.type;
                if (!inferred_type) {
                    if (value_expr->type == AST_CHAR)        inferred_type = type_char();
                    else if (value_expr->type == AST_STRING) inferred_type = type_string();
                    else                                     inferred_type = type_float();
                }

                Type *final_type = explicit_type ? explicit_type : inferred_type;

        ///// Array variables

                if (final_type->kind == TYPE_ARR) {
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
                            env_insert(ctx->env, var_name, final_type, arr);
                            print_defined(var_name, final_type, ctx->env, false);
                            result.type  = final_type;
                            result.value = arr;
                            return result;
                        }
                        arr = LLVMBuildAlloca(ctx->builder, arr_type, var_name);
                        env_insert(ctx->env, var_name, final_type, arr);
                        print_defined(var_name, final_type, ctx->env, false);
                        result.type  = final_type;
                        result.value = arr;
                        return result;
                    }
                    env_insert(ctx->env, var_name, final_type, value_result.value);
                    print_defined(var_name, final_type, ctx->env, false);
                    result.type  = final_type;
                    result.value = value_result.value;
                    return result;
                }

        ///// Layout variables

                if (final_type->kind == TYPE_LAYOUT) {
                    LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMValueRef set_fn = LLVMGetNamedFunction(ctx->module, "__layout_ptr_set");
                    if (!set_fn) {
                        LLVMTypeRef params[] = {ptr_t, ptr_t};
                        LLVMTypeRef ft = LLVMFunctionType(
                            LLVMVoidTypeInContext(ctx->context), params, 2, 0);
                        set_fn = LLVMAddFunction(ctx->module, "__layout_ptr_set", ft);
                        LLVMSetLinkage(set_fn, LLVMExternalLinkage);
                    }
                    LLVMValueRef name_str = LLVMBuildGlobalStringPtr(ctx->builder, var_name, "lay_name");
                    LLVMValueRef set_args[] = {name_str, value_result.value};
                    LLVMTypeRef  set_params[] = {ptr_t, ptr_t};
                    LLVMBuildCall2(ctx->builder,
                        LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), set_params, 2, 0),
                        set_fn, set_args, 2, "");

                    LLVMValueRef gv = LLVMGetNamedGlobal(ctx->module, var_name);
                    if (!gv) {
                        gv = LLVMAddGlobal(ctx->module, ptr_t, var_name);
                        LLVMSetInitializer(gv, LLVMConstPointerNull(ptr_t));
                        LLVMSetLinkage(gv, LLVMExternalLinkage);
                    }
                    LLVMBuildStore(ctx->builder, value_result.value, gv);
                    Type *layout_type_copy = type_clone(final_type);
                    env_insert(ctx->env, var_name, layout_type_copy, gv);
                    EnvEntry *elayout = env_lookup(ctx->env, var_name);
                    if (elayout) elayout->llvm_name = strdup(LLVMGetValueName(gv));
                    print_defined(var_name, final_type, ctx->env, false);
                    result.type  = layout_type_copy;
                    result.value = value_result.value;
                    return result;
                }

        ///// Scalar variables

                LLVMTypeRef  llvm_type    = type_to_llvm(ctx, final_type);
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

                if (final_type->kind == TYPE_STRING && stored_value) {
                    LLVMValueRef strdup_fn = get_or_declare_strdup(ctx);
                    LLVMValueRef args[] = {stored_value};
                    stored_value = LLVMBuildCall2(ctx->builder,
                                                  LLVMGlobalGetValueType(strdup_fn),
                                                  strdup_fn, args, 1, "strdup");
                }

                LLVMValueRef var;
                if (is_at_top_level(ctx)) {
                    var = LLVMGetNamedGlobal(ctx->module, var_name);
                    if (!var) {
                        var = LLVMAddGlobal(ctx->module, llvm_type, var_name);
                        LLVMSetInitializer(var, LLVMConstNull(llvm_type));
                        LLVMSetLinkage(var, LLVMExternalLinkage);
                    }
                    LLVMBuildStore(ctx->builder, stored_value, var);
                } else {
                    var = LLVMBuildAlloca(ctx->builder, llvm_type, var_name);
                    LLVMBuildStore(ctx->builder, stored_value, var);
                }

                env_insert(ctx->env, var_name, final_type, var);
                EnvEntry *evar = env_lookup(ctx->env, var_name);
                if (evar) evar->llvm_name = strdup(LLVMGetValueName(var));

                // Optional alias at items[4]
                if (ast->list.count >= 5) {
                    AST *doc_node   = ast->list.items[3];
                    AST *alias_node = ast->list.items[4];
                    if (doc_node->type == AST_STRING) {
                        EnvEntry *ent = env_lookup(ctx->env, var_name);
                        if (ent) { free(ent->docstring); ent->docstring = strdup(doc_node->string); }
                    }
                    if (alias_node->type == AST_SYMBOL &&
                        strcmp(alias_node->symbol, "__no_alias__") != 0) {
                        env_insert(ctx->env, alias_node->symbol, type_clone(final_type), var);
                        printf("Alias: %s -> %s\n", alias_node->symbol, var_name);
                    }
                }

                bool is_private = ctx->module_ctx &&
                                  !should_export_symbol(ctx->module_ctx, var_name);
                print_defined(var_name, final_type, ctx->env, is_private);

                result.type  = final_type;
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
                } else if (actual.type && actual.type->kind == TYPE_BOOL) {
                    /* Bool result — compare as i1 directly, box expected if needed */
                    LLVMTypeRef i1 = LLVMInt1TypeInContext(ctx->context);

                    /* Normalize lhs to i1 */
                    LLVMValueRef lhs_bool = lhs;
                    if (LLVMTypeOf(lhs) == ptr) {
                        LLVMTypeRef uft = LLVMFunctionType(i64, &ptr, 1, 0);
                        LLVMValueRef unboxed = LLVMBuildCall2(ctx->builder, uft,
                                                              get_rt_unbox_int(ctx), &lhs, 1, "ub_lhs");
                        lhs_bool = LLVMBuildTrunc(ctx->builder, unboxed, i1, "lhs_bool");
                    } else if (LLVMTypeOf(lhs) != i1) {
                        lhs_bool = LLVMBuildTrunc(ctx->builder, lhs, i1, "lhs_bool");
                    }

                    /* Normalize rhs to i1 */
                    LLVMValueRef rhs_bool = rhs;
                    if (LLVMTypeOf(rhs) == ptr) {
                        LLVMTypeRef uft = LLVMFunctionType(i64, &ptr, 1, 0);
                        LLVMValueRef unboxed = LLVMBuildCall2(ctx->builder, uft,
                                                              get_rt_unbox_int(ctx), &rhs, 1, "ub_rhs");
                        rhs_bool = LLVMBuildTrunc(ctx->builder, unboxed, i1, "rhs_bool");
                    } else if (LLVMTypeOf(rhs) != i1) {
                        rhs_bool = LLVMBuildTrunc(ctx->builder, rhs, i1, "rhs_bool");
                    }

                    cond = LLVMBuildICmp(ctx->builder, LLVMIntEQ, lhs_bool, rhs_bool, "eq");


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




            if (strcmp(head->symbol, "map") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'map' requires 2 arguments (fn xs)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef   ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef  closure = resolve_to_closure(ctx, ast->list.items[1]);
                CodegenResult col_r   = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef  env_out;
                LLVMValueRef  wrapper = codegen_unary_wrapper(ctx, &env_out);
                env_out = closure;

                if (col_r.type && col_r.type->kind == TYPE_SET) {
                    LLVMValueRef raw_set = col_r.value;
                    LLVMValueRef ub_fn   = get_rt_unbox_set(ctx);
                    LLVMTypeRef  ub_ft   = LLVMFunctionType(ptr, &ptr, 1, 0);
                    LLVMValueRef ub_a[]  = {raw_set};
                    raw_set = LLVMBuildCall2(ctx->builder, ub_ft, ub_fn, ub_a, 1, "rawset");
                    LLVMValueRef rt_fn    = get_rt_set_map(ctx);
                    LLVMTypeRef  ft_args[] = {ptr, ptr, ptr};
                    LLVMTypeRef  ft        = LLVMFunctionType(ptr, ft_args, 3, 0);
                    LLVMValueRef args[]    = {raw_set, env_out, wrapper};
                    result.value = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 3, "set_map");
                    result.type  = type_list(NULL);
                    return result;
                }
                LLVMValueRef rt_fn    = get_rt_list_map(ctx);
                LLVMTypeRef  ft_args[] = {ptr, ptr, ptr};
                LLVMTypeRef  ft        = LLVMFunctionType(ptr, ft_args, 3, 0);
                LLVMValueRef args[]    = {col_r.value, env_out, wrapper};
                result.value = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 3, "map");
                result.type  = type_list(NULL);
                return result;
            }

            if (strcmp(head->symbol, "foldl") == 0) {
                if (ast->list.count != 4) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'foldl' requires 3 arguments (fn init xs)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef   ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef  closure = resolve_to_closure(ctx, ast->list.items[1]);
                CodegenResult init_r  = codegen_expr(ctx, ast->list.items[2]);
                CodegenResult col_r   = codegen_expr(ctx, ast->list.items[3]);

                LLVMValueRef init_val = init_r.value;
                if (init_r.type && init_r.type->kind == TYPE_BOOL) {
                    LLVMTypeRef i64  = LLVMInt64TypeInContext(ctx->context);
                    LLVMValueRef ext = LLVMBuildZExt(ctx->builder, init_r.value, i64, "bool_ext");
                    LLVMTypeRef bft  = LLVMFunctionType(ptr, &i64, 1, 0);
                    init_val = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_int(ctx), &ext, 1, "box_init");
                } else if (init_r.type && type_is_integer(init_r.type)) {
                    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                    LLVMTypeRef bft = LLVMFunctionType(ptr, &i64, 1, 0);
                    init_val = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_int(ctx), &init_r.value, 1, "box_init");
                } else if (init_r.type && type_is_float(init_r.type)) {
                    LLVMTypeRef dbl = LLVMDoubleTypeInContext(ctx->context);
                    LLVMTypeRef bft = LLVMFunctionType(ptr, &dbl, 1, 0);
                    init_val = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_float(ctx), &init_r.value, 1, "box_init");
                }

                LLVMValueRef env_out;
                LLVMValueRef wrapper = codegen_binary_wrapper(ctx, &env_out);
                env_out = closure;

                if (col_r.type && col_r.type->kind == TYPE_SET) {
                    LLVMValueRef raw_set = col_r.value;
                    LLVMValueRef ub_fn   = get_rt_unbox_set(ctx);
                    LLVMTypeRef  ub_ft   = LLVMFunctionType(ptr, &ptr, 1, 0);
                    LLVMValueRef ub_a[]  = {raw_set};
                    raw_set = LLVMBuildCall2(ctx->builder, ub_ft, ub_fn, ub_a, 1, "rawset");
                    LLVMValueRef rt_fn    = get_rt_set_foldl(ctx);
                    LLVMTypeRef  ft_args[] = {ptr, ptr, ptr, ptr};
                    LLVMTypeRef  ft        = LLVMFunctionType(ptr, ft_args, 4, 0);
                    LLVMValueRef args[]    = {raw_set, init_val, env_out, wrapper};
                    result.value = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 4, "set_foldl");
                    result.type  = type_unknown();
                    return result;
                }
                LLVMValueRef rt_fn    = get_rt_list_foldl(ctx);
                LLVMTypeRef  ft_args[] = {ptr, ptr, ptr, ptr};
                LLVMTypeRef  ft        = LLVMFunctionType(ptr, ft_args, 4, 0);
                LLVMValueRef args[]    = {col_r.value, init_val, env_out, wrapper};
                result.value = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 4, "foldl");
                result.type  = type_unknown();
                return result;
            }

            if (strcmp(head->symbol, "foldr") == 0) {
                if (ast->list.count != 4) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'foldr' requires 3 arguments (fn init xs)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef   ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef  closure = resolve_to_closure(ctx, ast->list.items[1]);
                CodegenResult init_r  = codegen_expr(ctx, ast->list.items[2]);
                CodegenResult list_r  = codegen_expr(ctx, ast->list.items[3]);

                LLVMValueRef init_val = init_r.value;
                if (init_r.type && type_is_integer(init_r.type)) {
                    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                    LLVMTypeRef bft = LLVMFunctionType(ptr, &i64, 1, 0);
                    init_val = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_int(ctx), &init_r.value, 1, "box_init");
                } else if (init_r.type && type_is_float(init_r.type)) {
                    LLVMTypeRef dbl = LLVMDoubleTypeInContext(ctx->context);
                    LLVMTypeRef bft = LLVMFunctionType(ptr, &dbl, 1, 0);
                    init_val = LLVMBuildCall2(ctx->builder, bft,
                                              get_rt_value_float(ctx), &init_r.value, 1, "box_init");
                }

                LLVMValueRef env_out;
                LLVMValueRef wrapper = codegen_binary_wrapper(ctx, &env_out);
                env_out = closure;

                LLVMValueRef rt_fn    = get_rt_list_foldr(ctx);
                LLVMTypeRef  ft_args[] = {ptr, ptr, ptr, ptr};
                LLVMTypeRef  ft        = LLVMFunctionType(ptr, ft_args, 4, 0);
                LLVMValueRef args[]    = {list_r.value, init_val, env_out, wrapper};
                result.value = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 4, "foldr");
                result.type  = type_unknown();
                return result;
            }

            if (strcmp(head->symbol, "filter") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'filter' requires 2 arguments (fn xs)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef   ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef  closure = resolve_to_closure(ctx, ast->list.items[1]);
                CodegenResult col_r   = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef  env_out;
                LLVMValueRef  wrapper = codegen_unary_wrapper(ctx, &env_out);
                env_out = closure;

                if (col_r.type && col_r.type->kind == TYPE_SET) {
                    LLVMValueRef raw_set = col_r.value;
                    LLVMValueRef ub_fn   = get_rt_unbox_set(ctx);
                    LLVMTypeRef  ub_ft   = LLVMFunctionType(ptr, &ptr, 1, 0);
                    LLVMValueRef ub_a[]  = {raw_set};
                    raw_set = LLVMBuildCall2(ctx->builder, ub_ft, ub_fn, ub_a, 1, "rawset");
                    LLVMValueRef rt_fn    = get_rt_set_filter(ctx);
                    LLVMTypeRef  ft_args[] = {ptr, ptr, ptr};
                    LLVMTypeRef  ft        = LLVMFunctionType(ptr, ft_args, 3, 0);
                    LLVMValueRef args[]    = {raw_set, env_out, wrapper};
                    LLVMValueRef raw_out   = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 3, "set_filter");
                    LLVMValueRef wrap_fn   = get_rt_value_set(ctx);
                    LLVMTypeRef  wft       = LLVMFunctionType(ptr, &ptr, 1, 0);
                    LLVMValueRef wa[]      = {raw_out};
                    result.value = LLVMBuildCall2(ctx->builder, wft, wrap_fn, wa, 1, "setval");
                    result.type  = type_set();
                    return result;
                }
                LLVMValueRef rt_fn    = get_rt_list_filter(ctx);
                LLVMTypeRef  ft_args[] = {ptr, ptr, ptr};
                LLVMTypeRef  ft        = LLVMFunctionType(ptr, ft_args, 3, 0);
                LLVMValueRef args[]    = {col_r.value, env_out, wrapper};
                result.value = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 3, "filter");
                result.type  = type_list(NULL);
                return result;
            }

            if (strcmp(head->symbol, "zipwith") == 0) {
                if (ast->list.count != 4) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'zipwith' requires 3 arguments (fn xs ys)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef   ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef  closure = resolve_to_closure(ctx, ast->list.items[1]);
                CodegenResult a_r     = codegen_expr(ctx, ast->list.items[2]);
                CodegenResult b_r     = codegen_expr(ctx, ast->list.items[3]);
                LLVMValueRef  env_out;
                LLVMValueRef  wrapper = codegen_binary_wrapper(ctx, &env_out);
                env_out = closure;

                LLVMValueRef rt_fn    = get_rt_list_zipwith(ctx);
                LLVMTypeRef  ft_args[] = {ptr, ptr, ptr, ptr};
                LLVMTypeRef  ft        = LLVMFunctionType(ptr, ft_args, 4, 0);
                LLVMValueRef args[]    = {a_r.value, b_r.value, env_out, wrapper};
                result.value = LLVMBuildCall2(ctx->builder, ft, rt_fn, args, 4, "zipwith");
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
                LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

                CodegenResult first = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef acc;
                if (first.type && first.type->kind == TYPE_BOOL) {
                    acc = first.value;
                } else if (type_is_float(first.type)) {
                    acc = LLVMBuildFCmp(ctx->builder, LLVMRealONE, first.value,
                                        LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0), "and0");
                } else if (first.type && (first.type->kind == TYPE_SET   ||
                                          first.type->kind == TYPE_MAP   ||
                                          first.type->kind == TYPE_LIST  ||
                                          first.type->kind == TYPE_UNKNOWN)) {
                    /* pointer — non-null is truthy */
                    LLVMValueRef null_ptr = LLVMConstNull(ptr);
                    acc = LLVMBuildICmp(ctx->builder, LLVMIntNE, first.value, null_ptr, "and0");
                } else {
                    acc = LLVMBuildICmp(ctx->builder, LLVMIntNE, first.value,
                                        LLVMConstInt(LLVMTypeOf(first.value), 0, 0), "and0");
                }

                for (size_t i = 2; i < ast->list.count; i++) {
                    CodegenResult next = codegen_expr(ctx, ast->list.items[i]);
                    LLVMValueRef cond;
                    if (next.type && next.type->kind == TYPE_BOOL) {
                        cond = next.value;
                    } else if (type_is_float(next.type)) {
                        cond = LLVMBuildFCmp(ctx->builder, LLVMRealONE, next.value,
                                             LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0), "andi");
                    } else if (next.type && (next.type->kind == TYPE_SET   ||
                                             next.type->kind == TYPE_MAP   ||
                                             next.type->kind == TYPE_LIST  ||
                                             next.type->kind == TYPE_UNKNOWN)) {
                        LLVMValueRef null_ptr = LLVMConstNull(ptr);
                        cond = LLVMBuildICmp(ctx->builder, LLVMIntNE, next.value, null_ptr, "andi");
                    } else {
                        cond = LLVMBuildICmp(ctx->builder, LLVMIntNE, next.value,
                                             LLVMConstInt(LLVMTypeOf(next.value), 0, 0), "andi");
                    }
                    acc = LLVMBuildAnd(ctx->builder, acc, cond, "and");
                }

                result.value = acc;
                result.type  = type_bool();
                return result;
            }

            if (strcmp(head->symbol, "or") == 0) {
                if (ast->list.count < 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'or' requires at least 2 arguments",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

                CodegenResult first = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef acc;
                if (first.type && first.type->kind == TYPE_BOOL) {
                    acc = first.value;
                } else if (type_is_float(first.type)) {
                    acc = LLVMBuildFCmp(ctx->builder, LLVMRealONE, first.value,
                                        LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0), "or0");
                } else if (first.type && (first.type->kind == TYPE_SET   ||
                                          first.type->kind == TYPE_MAP   ||
                                          first.type->kind == TYPE_LIST  ||
                                          first.type->kind == TYPE_UNKNOWN)) {
                    LLVMValueRef null_ptr = LLVMConstNull(ptr);
                    acc = LLVMBuildICmp(ctx->builder, LLVMIntNE, first.value, null_ptr, "or0");
                } else {
                    acc = LLVMBuildICmp(ctx->builder, LLVMIntNE, first.value,
                                        LLVMConstInt(LLVMTypeOf(first.value), 0, 0), "or0");
                }

                for (size_t i = 2; i < ast->list.count; i++) {
                    CodegenResult next = codegen_expr(ctx, ast->list.items[i]);
                    LLVMValueRef cond;
                    if (next.type && next.type->kind == TYPE_BOOL) {
                        cond = next.value;
                    } else if (type_is_float(next.type)) {
                        cond = LLVMBuildFCmp(ctx->builder, LLVMRealONE, next.value,
                                             LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0), "ori");
                    } else if (next.type && (next.type->kind == TYPE_SET   ||
                                             next.type->kind == TYPE_MAP   ||
                                             next.type->kind == TYPE_LIST  ||
                                             next.type->kind == TYPE_UNKNOWN)) {
                        LLVMValueRef null_ptr = LLVMConstNull(ptr);
                        cond = LLVMBuildICmp(ctx->builder, LLVMIntNE, next.value, null_ptr, "ori");
                    } else {
                        cond = LLVMBuildICmp(ctx->builder, LLVMIntNE, next.value,
                                             LLVMConstInt(LLVMTypeOf(next.value), 0, 0), "ori");
                    }
                    acc = LLVMBuildOr(ctx->builder, acc, cond, "or");
                }

                result.value = acc;
                result.type  = type_bool();
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

                LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

                /* Condition */
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

                /* Then branch */
                LLVMPositionBuilderAtEnd(ctx->builder, then_bb);
                CodegenResult then_result = codegen_expr(ctx, ast->list.items[2]);
                LLVMBasicBlockRef then_end_bb = LLVMGetInsertBlock(ctx->builder);

                /* Else branch */
                LLVMPositionBuilderAtEnd(ctx->builder, else_bb);
                CodegenResult else_result = {NULL, NULL};
                if (ast->list.count == 4)
                    else_result = codegen_expr(ctx, ast->list.items[3]);
                LLVMBasicBlockRef else_end_bb = LLVMGetInsertBlock(ctx->builder);

                /* No else or missing values — branch both to merge, return dummy */
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

                /* Classify branch types */
                bool then_is_ptr = then_result.type->kind == TYPE_SET     ||
                    then_result.type->kind == TYPE_MAP     ||
                    then_result.type->kind == TYPE_LIST    ||
                    then_result.type->kind == TYPE_STRING  ||
                    then_result.type->kind == TYPE_SYMBOL  ||
                    then_result.type->kind == TYPE_KEYWORD ||
                    then_result.type->kind == TYPE_RATIO   ||
                    then_result.type->kind == TYPE_UNKNOWN;
                bool else_is_ptr = else_result.type->kind == TYPE_SET     ||
                    else_result.type->kind == TYPE_MAP     ||
                    else_result.type->kind == TYPE_LIST    ||
                    else_result.type->kind == TYPE_STRING  ||
                    else_result.type->kind == TYPE_SYMBOL  ||
                    else_result.type->kind == TYPE_KEYWORD ||
                    else_result.type->kind == TYPE_RATIO   ||
                    else_result.type->kind == TYPE_UNKNOWN;
                bool then_is_char  = then_result.type->kind == TYPE_CHAR;
                bool else_is_char  = else_result.type->kind == TYPE_CHAR;
                bool then_is_float = type_is_float(then_result.type);
                bool else_is_float = type_is_float(else_result.type);
                bool then_is_int   = type_is_integer(then_result.type);
                bool else_is_int   = type_is_integer(else_result.type);
                bool then_is_bool  = then_result.type->kind == TYPE_BOOL;
                bool else_is_bool  = else_result.type->kind == TYPE_BOOL;

                /* Determine common PHI type */
                LLVMTypeRef phi_llvm_type;
                Type       *phi_type;

                if (then_is_ptr || else_is_ptr) {
                    phi_llvm_type = ptr;
                    phi_type      = then_is_ptr ? then_result.type : else_result.type;
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

                /* Coerce then-value and branch to merge */
                LLVMPositionBuilderAtEnd(ctx->builder, then_end_bb);
                LLVMValueRef then_val = then_result.value;
                if (!LLVMGetBasicBlockTerminator(then_end_bb)) {
                    if (then_is_ptr || else_is_ptr) {
                        /* Box scalar side if the other is a pointer */
                        if (!then_is_ptr) {
                            if (then_is_int) {
                                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                                LLVMTypeRef bft = LLVMFunctionType(ptr, &i64, 1, 0);
                                then_val = LLVMBuildCall2(ctx->builder, bft,
                                                          get_rt_value_int(ctx), &then_val, 1, "box_then");
                            } else if (then_is_float) {
                                LLVMTypeRef dbl = LLVMDoubleTypeInContext(ctx->context);
                                LLVMTypeRef bft = LLVMFunctionType(ptr, &dbl, 1, 0);
                                then_val = LLVMBuildCall2(ctx->builder, bft,
                                                          get_rt_value_float(ctx), &then_val, 1, "box_then");
                            }
                        }
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

                /* Coerce else-value and branch to merge */
                LLVMPositionBuilderAtEnd(ctx->builder, else_end_bb);
                LLVMValueRef else_val = else_result.value;
                if (!LLVMGetBasicBlockTerminator(else_end_bb)) {
                    if (then_is_ptr || else_is_ptr) {
                        if (!else_is_ptr) {
                            if (else_is_int) {
                                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                                LLVMTypeRef bft = LLVMFunctionType(ptr, &i64, 1, 0);
                                else_val = LLVMBuildCall2(ctx->builder, bft,
                                                          get_rt_value_int(ctx), &else_val, 1, "box_else");
                            } else if (else_is_float) {
                                LLVMTypeRef dbl = LLVMDoubleTypeInContext(ctx->context);
                                LLVMTypeRef bft = LLVMFunctionType(ptr, &dbl, 1, 0);
                                else_val = LLVMBuildCall2(ctx->builder, bft,
                                                          get_rt_value_float(ctx), &else_val, 1, "box_else");
                            }
                        }
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

                /* Merge + PHI */
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

            if (strcmp(head->symbol, "set") == 0) {
                LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef set_fn = get_rt_set_new(ctx);
                LLVMTypeRef  set_ft = LLVMFunctionType(ptr, NULL, 0, 0);
                LLVMValueRef set    = LLVMBuildCall2(ctx->builder, set_ft, set_fn, NULL, 0, "set");

                if (ast->list.count == 2) {
                    /* (set collection) — convert list or array */
                    CodegenResult arg = codegen_expr(ctx, ast->list.items[1]);
                    if (arg.type && arg.type->kind == TYPE_LIST) {
                        LLVMValueRef fn = get_rt_set_from_list(ctx);
                        LLVMTypeRef  ft_args[] = {ptr};
                        LLVMTypeRef  ft = LLVMFunctionType(ptr, ft_args, 1, 0);
                        LLVMValueRef args[] = {arg.value};
                        set = LLVMBuildCall2(ctx->builder, ft, fn, args, 1, "set");
                    } else if (arg.type && arg.type->kind == TYPE_ARR) {
                        LLVMTypeRef arr_llvm = type_to_llvm(ctx, arg.type);
                        LLVMTypeRef elem_llvm = type_to_llvm(ctx, arg.type->arr_element_type);
                        int n = arg.type->arr_size;

                        LLVMValueRef conj_fn = get_rt_set_conj(ctx);
                        LLVMTypeRef  cp[]    = {ptr, ptr};
                        LLVMTypeRef  cft     = LLVMFunctionType(ptr, cp, 2, 0);

                        LLVMValueRef raw_set_fn = get_rt_set_new(ctx);
                        LLVMValueRef raw_set    = LLVMBuildCall2(ctx->builder,
                                                                 LLVMFunctionType(ptr, NULL, 0, 0),
                                                                 raw_set_fn, NULL, 0, "raw_set");

                        for (int ei = 0; ei < n; ei++) {
                            LLVMValueRef zero  = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                            LLVMValueRef eidx  = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), ei, 0);
                            LLVMValueRef idxs[] = {zero, eidx};
                            LLVMValueRef ep    = LLVMBuildGEP2(ctx->builder, arr_llvm,
                                                               arg.value, idxs, 2, "ep");
                            LLVMValueRef ev    = LLVMBuildLoad2(ctx->builder, elem_llvm, ep, "ev");
                            LLVMValueRef bv    = codegen_box(ctx, ev, arg.type->arr_element_type);
                            LLVMValueRef ca[]  = {raw_set, bv};
                            raw_set = LLVMBuildCall2(ctx->builder, cft, conj_fn, ca, 2, "set");
                        }

                        LLVMValueRef wrap_fn2 = get_rt_value_set(ctx);
                        LLVMTypeRef  wft2     = LLVMFunctionType(ptr, &ptr, 1, 0);
                        LLVMValueRef wa2[]    = {raw_set};
                        result.value = LLVMBuildCall2(ctx->builder, wft2, wrap_fn2, wa2, 1, "setval");
                        result.type  = type_set();
                        return result;
                    } else {
                        /* single value — treat as (set val) */
                        LLVMValueRef conj_fn = get_rt_set_conj(ctx);
                        LLVMTypeRef  cp[] = {ptr, ptr};
                        LLVMTypeRef  cft  = LLVMFunctionType(ptr, cp, 2, 0);
                        LLVMValueRef boxed = codegen_box(ctx, arg.value, arg.type);
                        LLVMValueRef ca[] = {set, boxed};
                        set = LLVMBuildCall2(ctx->builder, cft, conj_fn, ca, 2, "set");
                    }
                } else {
                    /* (set v1 v2 v3 ...) */
                    LLVMValueRef conj_fn = get_rt_set_conj(ctx);
                    LLVMTypeRef  cp[]    = {ptr, ptr};
                    LLVMTypeRef  cft     = LLVMFunctionType(ptr, cp, 2, 0);
                    for (size_t i = 1; i < ast->list.count; i++) {
                        CodegenResult elem = codegen_expr(ctx, ast->list.items[i]);
                        LLVMValueRef  boxed = codegen_box(ctx, elem.value, elem.type);
                        LLVMValueRef  ca[]  = {set, boxed};
                        set = LLVMBuildCall2(ctx->builder, cft, conj_fn, ca, 2, "set");
                    }
                }
                LLVMValueRef wrap_fn = get_rt_value_set(ctx);
                LLVMTypeRef  wft     = LLVMFunctionType(ptr, &ptr, 1, 0);
                LLVMValueRef wa[]    = {set};
                result.value = LLVMBuildCall2(ctx->builder, wft, wrap_fn, wa, 1, "setval");
                result.type  = type_set();
                return result;
            }

            if (strcmp(head->symbol, "conj") == 0)
                return codegen_set_op(ctx, ast, get_rt_set_conj, "conj");

            if (strcmp(head->symbol, "disj") == 0)
                return codegen_set_op(ctx, ast, get_rt_set_disj, "disj");

            if (strcmp(head->symbol, "conj!") == 0)
                return codegen_set_op(ctx, ast, get_rt_set_conj_mut, "conj!");

            if (strcmp(head->symbol, "disj!") == 0)
                return codegen_set_op(ctx, ast, get_rt_set_disj_mut, "disj!");

            if (strcmp(head->symbol, "set?") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'set?' requires 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  i1     = LLVMInt1TypeInContext(ctx->context);
                CodegenResult arg   = codegen_expr(ctx, ast->list.items[1]);
                /* check type at compile time if known */
                if (arg.type && arg.type->kind == TYPE_SET) {
                    result.value = LLVMConstInt(i1, 1, 0);
                } else {
                    result.value = LLVMConstInt(i1, 0, 0);
                }
                result.type = type_bool();
                return result;
            }

            // Map
            if (strcmp(head->symbol, "assoc") == 0) {
                if (ast->list.count != 4) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'assoc' requires 3 arguments (map key val)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                return codegen_map_op(ctx, ast, get_rt_map_assoc, "assoc", 3);
            }

            if (strcmp(head->symbol, "assoc!") == 0) {
                if (ast->list.count != 4) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'assoc!' requires 3 arguments (map key val)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                return codegen_map_op(ctx, ast, get_rt_map_assoc_mut, "assoc!", 3);
            }

            if (strcmp(head->symbol, "dissoc") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'dissoc' requires 2 arguments (map key)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                return codegen_map_op(ctx, ast, get_rt_map_dissoc, "dissoc", 2);
            }

            if (strcmp(head->symbol, "dissoc!") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'dissoc!' requires 2 arguments (map key)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                return codegen_map_op(ctx, ast, get_rt_map_dissoc_mut, "dissoc!", 2);
            }

            if (strcmp(head->symbol, "find") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'find' requires 2 arguments (map key)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult map_r = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult key_r = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef raw_map = map_r.value;
                if (map_r.type && map_r.type->kind == TYPE_MAP) {
                    LLVMValueRef ub_fn = get_rt_unbox_map(ctx);
                    LLVMTypeRef  ft    = LLVMFunctionType(ptr, &ptr, 1, 0);
                    LLVMValueRef ua[]  = {raw_map};
                    raw_map = LLVMBuildCall2(ctx->builder, ft, ub_fn, ua, 1, "rawmap");
                }
                LLVMValueRef  boxed  = codegen_box(ctx, key_r.value, key_r.type);
                LLVMValueRef  fn     = get_rt_map_find(ctx);
                LLVMTypeRef   fp[]   = {ptr, ptr};
                LLVMTypeRef   fft    = LLVMFunctionType(ptr, fp, 2, 0);
                LLVMValueRef  fa[]   = {raw_map, boxed};
                result.value = LLVMBuildCall2(ctx->builder, fft, fn, fa, 2, "find");
                result.type  = type_unknown();
                return result;
            }

            if (strcmp(head->symbol, "keys") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'keys' requires 1 argument (map)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult map_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef raw_map = map_r.value;
                if (map_r.type && map_r.type->kind == TYPE_MAP) {
                    LLVMValueRef ub_fn = get_rt_unbox_map(ctx);
                    LLVMTypeRef  ft    = LLVMFunctionType(ptr, &ptr, 1, 0);
                    LLVMValueRef ua[]  = {raw_map};
                    raw_map = LLVMBuildCall2(ctx->builder, ft, ub_fn, ua, 1, "rawmap");
                }
                LLVMValueRef fn   = get_rt_map_keys(ctx);
                LLVMTypeRef  fp[] = {ptr};
                LLVMTypeRef  fft  = LLVMFunctionType(ptr, fp, 1, 0);
                LLVMValueRef fa[] = {raw_map};
                result.value = LLVMBuildCall2(ctx->builder, fft, fn, fa, 1, "keys");
                result.type  = type_list(NULL);
                return result;
            }

            if (strcmp(head->symbol, "vals") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'vals' requires 1 argument (map)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult map_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef raw_map = map_r.value;
                if (map_r.type && map_r.type->kind == TYPE_MAP) {
                    LLVMValueRef ub_fn = get_rt_unbox_map(ctx);
                    LLVMTypeRef  ft    = LLVMFunctionType(ptr, &ptr, 1, 0);
                    LLVMValueRef ua[]  = {raw_map};
                    raw_map = LLVMBuildCall2(ctx->builder, ft, ub_fn, ua, 1, "rawmap");
                }
                LLVMValueRef fn   = get_rt_map_vals(ctx);
                LLVMTypeRef  fp[] = {ptr};
                LLVMTypeRef  fft  = LLVMFunctionType(ptr, fp, 1, 0);
                LLVMValueRef fa[] = {raw_map};
                result.value = LLVMBuildCall2(ctx->builder, fft, fn, fa, 1, "vals");
                result.type  = type_list(NULL);
                return result;
            }

            if (strcmp(head->symbol, "merge") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'merge' requires 2 arguments (map map)",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult a_r    = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult b_r    = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef  raw_a  = a_r.value;
                LLVMValueRef  raw_b  = b_r.value;
                if (a_r.type && a_r.type->kind == TYPE_MAP) {
                    LLVMValueRef ub = get_rt_unbox_map(ctx);
                    LLVMTypeRef  ft = LLVMFunctionType(ptr, &ptr, 1, 0);
                    LLVMValueRef ua[] = {raw_a};
                    raw_a = LLVMBuildCall2(ctx->builder, ft, ub, ua, 1, "rawmap_a");
                }
                if (b_r.type && b_r.type->kind == TYPE_MAP) {
                    LLVMValueRef ub = get_rt_unbox_map(ctx);
                    LLVMTypeRef  ft = LLVMFunctionType(ptr, &ptr, 1, 0);
                    LLVMValueRef ub_a[] = {raw_b};
                    raw_b = LLVMBuildCall2(ctx->builder, ft, ub, ub_a, 1, "rawmap_b");
                }
                LLVMValueRef  fn      = get_rt_map_merge(ctx);
                LLVMTypeRef   mp[]    = {ptr, ptr};
                LLVMTypeRef   mft     = LLVMFunctionType(ptr, mp, 2, 0);
                LLVMValueRef  ma[]    = {raw_a, raw_b};
                LLVMValueRef  new_map = LLVMBuildCall2(ctx->builder, mft, fn, ma, 2, "merged");
                LLVMValueRef  wrap_fn = get_rt_value_map(ctx);
                LLVMTypeRef   wft     = LLVMFunctionType(ptr, &ptr, 1, 0);
                LLVMValueRef  wa[]    = {new_map};
                result.value = LLVMBuildCall2(ctx->builder, wft, wrap_fn, wa, 1, "mapval");
                result.type  = type_map();
                return result;
            }

            if (strcmp(head->symbol, "map?") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'map?' requires 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  i1  = LLVMInt1TypeInContext(ctx->context);
                CodegenResult arg = codegen_expr(ctx, ast->list.items[1]);
                result.value = (arg.type && arg.type->kind == TYPE_MAP)
                    ? LLVMConstInt(i1, 1, 0)
                    : LLVMConstInt(i1, 0, 0);
                result.type  = type_bool();
                return result;
            }


            if (strcmp(head->symbol, "contains?") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'contains?' requires 2 arguments",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i32    = LLVMInt32TypeInContext(ctx->context);
                LLVMTypeRef  i1     = LLVMInt1TypeInContext(ctx->context);
                CodegenResult col_r = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult key_r = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef  boxed = codegen_box(ctx, key_r.value, key_r.type);
                LLVMValueRef  raw   = col_r.value;

                LLVMValueRef fn;
                if (col_r.type && col_r.type->kind == TYPE_MAP) {
                    LLVMValueRef ub_fn = get_rt_unbox_map(ctx);
                    LLVMTypeRef  ft    = LLVMFunctionType(ptr, &ptr, 1, 0);
                    LLVMValueRef ua[]  = {raw};
                    raw = LLVMBuildCall2(ctx->builder, ft, ub_fn, ua, 1, "rawmap");
                    fn  = get_rt_map_contains(ctx);
                } else {
                    if (col_r.type && col_r.type->kind == TYPE_SET) {
                        LLVMValueRef ub_fn = get_rt_unbox_set(ctx);
                        LLVMTypeRef  ft    = LLVMFunctionType(ptr, &ptr, 1, 0);
                        LLVMValueRef ua[]  = {raw};
                        raw = LLVMBuildCall2(ctx->builder, ft, ub_fn, ua, 1, "rawset");
                    }
                    fn = get_rt_set_contains(ctx);
                }

                LLVMTypeRef  cp[]   = {ptr, ptr};
                LLVMTypeRef  cft    = LLVMFunctionType(i32, cp, 2, 0);
                LLVMValueRef ca[]   = {raw, boxed};
                LLVMValueRef i32val = LLVMBuildCall2(ctx->builder, cft, fn, ca, 2, "contains");
                result.value = LLVMBuildTrunc(ctx->builder, i32val, i1, "contains_bool");
                result.type  = type_bool();
                return result;
            }

            if (strcmp(head->symbol, "count") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'count' requires 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i64    = LLVMInt64TypeInContext(ctx->context);
                CodegenResult col_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef  raw   = col_r.value;
                LLVMValueRef  fn;

                if (col_r.type && col_r.type->kind == TYPE_MAP) {
                    LLVMValueRef ub_fn = get_rt_unbox_map(ctx);
                    LLVMTypeRef  ft    = LLVMFunctionType(ptr, &ptr, 1, 0);
                    LLVMValueRef ua[]  = {raw};
                    raw = LLVMBuildCall2(ctx->builder, ft, ub_fn, ua, 1, "rawmap");
                    fn  = get_rt_map_count(ctx);
                } else {
                    if (col_r.type && col_r.type->kind == TYPE_SET) {
                        LLVMValueRef ub_fn = get_rt_unbox_set(ctx);
                        LLVMTypeRef  ft    = LLVMFunctionType(ptr, &ptr, 1, 0);
                        LLVMValueRef ua[]  = {raw};
                        raw = LLVMBuildCall2(ctx->builder, ft, ub_fn, ua, 1, "rawset");
                    }
                    fn = get_rt_set_count(ctx);
                }

                LLVMTypeRef  fp[] = {ptr};
                LLVMTypeRef  fft  = LLVMFunctionType(i64, fp, 1, 0);
                LLVMValueRef fa[] = {raw};
                result.value = LLVMBuildCall2(ctx->builder, fft, fn, fa, 1, "count");
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

                const char *op = head->symbol;

                CodegenResult lhs = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult rhs = codegen_expr(ctx, ast->list.items[2]);

                bool lhs_is_ptr_type = lhs.type && (lhs.type->kind == TYPE_SET     ||
                                                    lhs.type->kind == TYPE_MAP     ||
                                                    lhs.type->kind == TYPE_LIST    ||
                                                    lhs.type->kind == TYPE_RATIO   ||
                                                    lhs.type->kind == TYPE_UNKNOWN);
                bool rhs_is_ptr_type = rhs.type && (rhs.type->kind == TYPE_SET     ||
                                                    rhs.type->kind == TYPE_MAP     ||
                                                    rhs.type->kind == TYPE_LIST    ||
                                                    rhs.type->kind == TYPE_RATIO   ||
                                                    rhs.type->kind == TYPE_UNKNOWN);

                /* Collections (set, map, list, ratio): only = and != are defined */
                if (lhs_is_ptr_type || rhs_is_ptr_type) {
                    if (strcmp(op, "=") != 0 && strcmp(op, "!=") != 0) {
                        Type       *bad   = lhs_is_ptr_type ? lhs.type : rhs.type;
                        const char *tname = bad ? type_to_string(bad) : "collection";
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: operator '%s' is not defined for %s "
                                      "— only '=' and '!=' are supported for collections",
                                      parser_get_filename(), ast->line, ast->column, op, tname);
                    }

                    LLVMTypeRef  ptr       = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef  i32       = LLVMInt32TypeInContext(ctx->context);
                    LLVMValueRef fn        = get_rt_equal_p(ctx);
                    LLVMTypeRef  ft_args[] = {ptr, ptr};
                    LLVMTypeRef  ft        = LLVMFunctionType(i32, ft_args, 2, 0);

                    LLVMValueRef lv = lhs_is_ptr_type ? lhs.value : codegen_box(ctx, lhs.value, lhs.type);
                    LLVMValueRef rv = rhs_is_ptr_type ? rhs.value : codegen_box(ctx, rhs.value, rhs.type);

                    LLVMValueRef args[]  = {lv, rv};
                    LLVMValueRef i32val  = LLVMBuildCall2(ctx->builder, ft, fn, args, 2, "eq_p");
                    LLVMValueRef eq      = LLVMBuildICmp(ctx->builder, LLVMIntNE, i32val,
                                                         LLVMConstInt(i32, 0, 0), "eq");
                    result.value = (strcmp(op, "!=") == 0)
                        ? LLVMBuildNot(ctx->builder, eq, "neq")
                        : eq;
                    result.type  = type_bool();
                    return result;
                }

                /* Strings: use strcmp for = and != */
                if (lhs.type && lhs.type->kind == TYPE_STRING &&
                    (strcmp(op, "=") == 0 || strcmp(op, "!=") == 0)) {
                    LLVMTypeRef  ptr       = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef  i32       = LLVMInt32TypeInContext(ctx->context);
                    LLVMValueRef strcmp_fn = LLVMGetNamedFunction(ctx->module, "strcmp");
                    if (!strcmp_fn) {
                        LLVMTypeRef params[] = {ptr, ptr};
                        LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, 0);
                        strcmp_fn = LLVMAddFunction(ctx->module, "strcmp", ft);
                    }
                    LLVMValueRef args[] = {lhs.value, rhs.value};
                    LLVMValueRef cmp    = LLVMBuildCall2(ctx->builder,
                                                         LLVMGlobalGetValueType(strcmp_fn),
                                                         strcmp_fn, args, 2, "strcmp");
                    LLVMValueRef eq     = LLVMBuildICmp(ctx->builder, LLVMIntEQ, cmp,
                                                        LLVMConstInt(i32, 0, 0), "str_eq");
                    result.value = (strcmp(op, "!=") == 0)
                        ? LLVMBuildNot(ctx->builder, eq, "str_neq")
                        : eq;
                    result.type  = type_bool();
                    return result;
                }

                /* Numeric and bool comparison */
                LLVMValueRef lv = lhs.value;
                LLVMValueRef rv = rhs.value;

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

                if (!type_is_numeric(result_type) &&
                    result_type->kind != TYPE_UNKNOWN) {
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

                    if (!type_is_numeric(rhs.type) &&
                        rhs.type->kind != TYPE_UNKNOWN) {
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

            if (entry && entry->kind == ENV_VAR &&
                entry->type && entry->type->kind == TYPE_SET) {
                /* Set used as function: (s key) -> (get s key) */
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: set used as function requires exactly 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  ptr_t   = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef set_val = LLVMBuildLoad2(ctx->builder,
                                                      type_to_llvm(ctx, entry->type),
                                                      entry->value, head->symbol);
                LLVMValueRef ub_fn   = get_rt_unbox_set(ctx);
                LLVMTypeRef  ub_ft   = LLVMFunctionType(ptr_t, &ptr_t, 1, 0);
                LLVMValueRef ub_args[] = {set_val};
                LLVMValueRef raw_set = LLVMBuildCall2(ctx->builder, ub_ft, ub_fn, ub_args, 1, "rawset");

                CodegenResult key_r  = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef  boxed  = codegen_box(ctx, key_r.value, key_r.type);

                LLVMValueRef  get_fn  = get_rt_set_get(ctx);
                LLVMTypeRef   gp[]    = {ptr_t, ptr_t};
                LLVMTypeRef   gft     = LLVMFunctionType(ptr_t, gp, 2, 0);
                LLVMValueRef  ga[]    = {raw_set, boxed};
                result.value = LLVMBuildCall2(ctx->builder, gft, get_fn, ga, 2, "setget");
                result.type  = type_unknown();
                return result;
            }

            if (entry && entry->kind == ENV_VAR &&
                entry->type && entry->type->kind == TYPE_MAP) {
                /* Map used as function: (m key) -> (get m key nil)
                 * (m key default) -> (get m key default)           */
                if (ast->list.count < 2 || ast->list.count > 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: map used as function requires 1 or 2 arguments",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef map_val = LLVMBuildLoad2(ctx->builder,
                                                      type_to_llvm(ctx, entry->type),
                                                      entry->value, head->symbol);
                LLVMValueRef ub_fn   = get_rt_unbox_map(ctx);
                LLVMTypeRef  ub_ft   = LLVMFunctionType(ptr, &ptr, 1, 0);
                LLVMValueRef ub_a[]  = {map_val};
                LLVMValueRef raw_map = LLVMBuildCall2(ctx->builder, ub_ft, ub_fn, ub_a, 1, "rawmap");

                CodegenResult key_r  = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef  bkey   = codegen_box(ctx, key_r.value, key_r.type);

                LLVMValueRef default_val;
                if (ast->list.count == 3) {
                    CodegenResult def_r = codegen_expr(ctx, ast->list.items[2]);
                    default_val = codegen_box(ctx, def_r.value, def_r.type);
                } else {
                    LLVMValueRef nil_fn = get_rt_value_nil(ctx);
                    LLVMTypeRef  nil_ft = LLVMFunctionType(ptr, NULL, 0, 0);
                    default_val = LLVMBuildCall2(ctx->builder, nil_ft, nil_fn, NULL, 0, "nil");
                }

                LLVMValueRef  get_fn  = get_rt_map_get(ctx);
                LLVMTypeRef   gp[]    = {ptr, ptr, ptr};
                LLVMTypeRef   gft     = LLVMFunctionType(ptr, gp, 3, 0);
                LLVMValueRef  ga[]    = {raw_map, bkey, default_val};
                result.value = LLVMBuildCall2(ctx->builder, gft, get_fn, ga, 3, "mapget");
                result.type  = type_unknown();
                return result;
            }

            /* Closure call: variable of type Fn or unknown used in call position */
            if (entry && entry->kind == ENV_VAR &&
                entry->type && (entry->type->kind == TYPE_FN ||
                                entry->type->kind == TYPE_UNKNOWN)) {
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i32     = LLVMInt32TypeInContext(ctx->context);
                int          n_args  = (int)ast->list.count - 1;
                LLVMValueRef clo_val = LLVMBuildLoad2(ctx->builder,
                                                      type_to_llvm(ctx, entry->type),
                                                      entry->value, head->symbol);

                /* Build args array on stack — array of ptr_t (RuntimeValue*) */
                LLVMTypeRef  arr_t   = LLVMArrayType(ptr, n_args ? n_args : 1);
                LLVMValueRef arr_ptr = LLVMBuildAlloca(ctx->builder, arr_t, "clo_args");
                for (int i = 0; i < n_args; i++) {
                    CodegenResult ar  = codegen_expr(ctx, ast->list.items[i + 1]);
                    LLVMValueRef  bv  = codegen_box(ctx, ar.value, ar.type);
                    LLVMValueRef  zero   = LLVMConstInt(i32, 0, 0);
                    LLVMValueRef  idx    = LLVMConstInt(i32, i, 0);
                    LLVMValueRef  idxs[] = {zero, idx};
                    LLVMValueRef  slot   = LLVMBuildGEP2(ctx->builder, arr_t,
                                                          arr_ptr, idxs, 2, "slot");
                    LLVMBuildStore(ctx->builder, bv, slot);
                }

                /* Decay to RuntimeValue** — bitcast array ptr to ptr */
                LLVMValueRef args_ptr = n_args > 0
                    ? LLVMBuildBitCast(ctx->builder, arr_ptr, ptr, "args_ptr")
                    : LLVMConstPointerNull(ptr);

                LLVMValueRef calln_fn = get_rt_closure_calln(ctx);
                LLVMTypeRef  calln_p[] = {ptr, i32, ptr};
                LLVMTypeRef  calln_ft  = LLVMFunctionType(ptr, calln_p, 3, 0);
                LLVMValueRef calln_a[] = {
                    clo_val,
                    LLVMConstInt(i32, n_args, 0),
                    args_ptr
                };
                result.value = LLVMBuildCall2(ctx->builder, calln_ft,
                                              calln_fn, calln_a, 3, "clo_calln");
                result.type  = type_unknown();
                return result;
            }


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

                // Find if last param is a rest param
                bool has_rest = (entry->param_count > 0 &&
                                 entry->source_ast &&
                                 entry->source_ast->type == AST_LAMBDA &&
                                 entry->source_ast->lambda.param_count > 0 &&
                                 entry->source_ast->lambda.params[
                                     entry->source_ast->lambda.param_count - 1
                                 ].is_rest);

                int required_params = has_rest ? declared_params - 1 : declared_params;

                if (!has_rest && (int)arg_count > declared_params) {
                    // Auto-curry: apply declared_params args, then call the
                    // result as a closure with the remaining args.
                    // (true 1 2) => ((true 1) 2)
                    LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef  i32   = LLVMInt32TypeInContext(ctx->context);

                    // Build first call with declared_params args.
                    // If the function uses closure ABI, route through
                    // rt_closure_calln with boxed args instead of direct call.
                    LLVMValueRef partial;
                    if (entry->is_closure_abi) {
                        LLVMTypeRef  arr_t2  = LLVMArrayType(ptr_t, declared_params);
                        LLVMValueRef arr_ptr2 = LLVMBuildAlloca(ctx->builder, arr_t2, "p_args");
                        for (int i = 0; i < declared_params; i++) {
                            CodegenResult ar = codegen_expr(ctx, ast->list.items[i + 1]);
                            LLVMValueRef  bv = codegen_box(ctx, ar.value, ar.type);
                            LLVMValueRef  zero   = LLVMConstInt(i32, 0, 0);
                            LLVMValueRef  idx    = LLVMConstInt(i32, i, 0);
                            LLVMValueRef  idxs[] = {zero, idx};
                            LLVMValueRef  slot   = LLVMBuildGEP2(ctx->builder, arr_t2,
                                                                  arr_ptr2, idxs, 2, "slot");
                            LLVMBuildStore(ctx->builder, bv, slot);
                        }
                        LLVMValueRef ap2     = LLVMBuildBitCast(ctx->builder, arr_ptr2, ptr_t, "ap");
                        LLVMValueRef calln2  = get_rt_closure_calln(ctx);
                        LLVMTypeRef  cp2[]   = {ptr_t, i32, ptr_t};
                        LLVMTypeRef  cft2    = LLVMFunctionType(ptr_t, cp2, 3, 0);
                        // Wrap func_ref as closure first
                        LLVMValueRef fn_ptr2 = LLVMBuildBitCast(ctx->builder,
                                                   entry->func_ref, ptr_t, "fn_ptr");
                        LLVMValueRef clo_fn2  = get_rt_value_closure(ctx);
                        LLVMTypeRef  clo_p2[] = {ptr_t, ptr_t, i32, i32};
                        LLVMTypeRef  clo_ft2  = LLVMFunctionType(ptr_t, clo_p2, 4, 0);
                        LLVMValueRef clo_a2[] = {
                            fn_ptr2, LLVMConstPointerNull(ptr_t),
                            LLVMConstInt(i32, 0, 0),
                            LLVMConstInt(i32, declared_params, 0)
                        };
                        LLVMValueRef clo2 = LLVMBuildCall2(ctx->builder, clo_ft2,
                                                            clo_fn2, clo_a2, 4, "clo");
                        LLVMValueRef ca2[] = {clo2, LLVMConstInt(i32, declared_params, 0), ap2};
                        partial = LLVMBuildCall2(ctx->builder, cft2, calln2, ca2, 3, "partial");
                    } else {
                        LLVMTypeRef *call_types = malloc(sizeof(LLVMTypeRef) * declared_params);
                        LLVMValueRef *call_args = malloc(sizeof(LLVMValueRef) * declared_params);
                        for (int i = 0; i < declared_params; i++) {
                            CodegenResult ar = codegen_expr(ctx, ast->list.items[i + 1]);
                            Type *pt = entry->params[i].type;
                            LLVMValueRef cv = ar.value;
                            LLVMTypeRef  expected = type_to_llvm(ctx, pt ? pt : ar.type);
                            LLVMTypeRef  ptr_t2   = LLVMPointerType(
                                                        LLVMInt8TypeInContext(ctx->context), 0);
                            if (expected == ptr_t2) {
                                // Expected ptr — box the value
                                cv = codegen_box(ctx, ar.value, ar.type);
                            } else if (pt && type_is_integer(pt) && type_is_float(ar.type)) {
                                cv = LLVMBuildFPToSI(ctx->builder, cv, expected, "conv");
                            } else if (pt && type_is_float(pt) && type_is_integer(ar.type)) {
                                cv = LLVMBuildSIToFP(ctx->builder, cv, expected, "conv");
                            }
                            call_args[i]  = cv;
                            call_types[i] = expected;
                        }
                        LLVMTypeRef call_ft = LLVMFunctionType(
                            type_to_llvm(ctx, entry->return_type),
                            call_types, declared_params, 0);
                        partial = LLVMBuildCall2(ctx->builder, call_ft,
                                                  entry->func_ref, call_args,
                                                  declared_params, "partial");
                        free(call_args);
                        free(call_types);
                    }

                    // Now call the result as a closure with remaining args
                    int remaining = (int)arg_count - declared_params;
                    LLVMTypeRef  arr_t   = LLVMArrayType(ptr_t, remaining);
                    LLVMValueRef arr_ptr = LLVMBuildAlloca(ctx->builder, arr_t, "curry_args");
                    for (int i = 0; i < remaining; i++) {
                        CodegenResult ar  = codegen_expr(ctx, ast->list.items[declared_params + i + 1]);
                        LLVMValueRef  bv  = codegen_box(ctx, ar.value, ar.type);
                        LLVMValueRef  zero   = LLVMConstInt(i32, 0, 0);
                        LLVMValueRef  idx    = LLVMConstInt(i32, i, 0);
                        LLVMValueRef  idxs[] = {zero, idx};
                        LLVMValueRef  slot   = LLVMBuildGEP2(ctx->builder, arr_t,
                                                              arr_ptr, idxs, 2, "slot");
                        LLVMBuildStore(ctx->builder, bv, slot);
                    }
                    LLVMValueRef args_ptr = LLVMBuildBitCast(ctx->builder,
                                               arr_ptr, ptr_t, "args_ptr");
                    LLVMValueRef calln_fn = get_rt_closure_calln(ctx);
                    LLVMTypeRef  calln_p[] = {ptr_t, i32, ptr_t};
                    LLVMTypeRef  calln_ft  = LLVMFunctionType(ptr_t, calln_p, 3, 0);
                    LLVMValueRef calln_a[] = {
                        partial,
                        LLVMConstInt(i32, remaining, 0),
                        args_ptr
                    };
                    result.value = LLVMBuildCall2(ctx->builder, calln_ft,
                                                  calln_fn, calln_a, 3, "autocurry");
                    result.type  = type_unknown();
                    return result;
                }
                if ((int)arg_count < required_params) {
                    // Partial application — handled below
                }


                // Partial application — return a closure capturing supplied args
                if ((int)arg_count < declared_params) {
                    LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef  i32   = LLVMInt32TypeInContext(ctx->context);
                    int supplied  = (int)arg_count;
                    int remaining = declared_params - supplied;

                    // Build a partial application trampoline:
                    // (ptr env, ptr arg_supplied...) -> ptr
                    // Trampoline uses calln ABI: (ptr env, i32 n, ptr args_array)
                    int tramp_params = 3;
                    LLVMTypeRef *tramp_types = malloc(sizeof(LLVMTypeRef) * tramp_params);
                    tramp_types[0] = ptr_t;  // env
                    tramp_types[1] = i32;    // n (number of args)
                    tramp_types[2] = ptr_t;  // args array

                    static int curry_count = 0;
                    char tname[64];
                    snprintf(tname, sizeof(tname), "__curry_%d", curry_count++);

                    LLVMTypeRef  tramp_ft = LLVMFunctionType(ptr_t, tramp_types, tramp_params, 0);
                    LLVMValueRef tramp    = LLVMAddFunction(ctx->module, tname, tramp_ft);
                    LLVMBasicBlockRef tramp_entry = LLVMAppendBasicBlockInContext(
                                                        ctx->context, tramp, "entry");
                    LLVMBasicBlockRef saved_block = LLVMGetInsertBlock(ctx->builder);
                    LLVMPositionBuilderAtEnd(ctx->builder, tramp_entry);

                    // env layout: [supplied_arg0, supplied_arg1, ...]
                    LLVMValueRef env_param = LLVMGetParam(tramp, 0);

                    // Unbox supplied args from env
                    LLVMValueRef args_array_param = LLVMGetParam(tramp, 2);
                    LLVMValueRef *all_args = malloc(sizeof(LLVMValueRef) * declared_params);
                    for (int i = 0; i < supplied; i++) {
                        LLVMValueRef idx   = LLVMConstInt(i32, i, 0);
                        LLVMValueRef gep   = LLVMBuildGEP2(ctx->builder, ptr_t,
                                                            env_param, &idx, 1, "cap_ptr");
                        LLVMValueRef boxed = LLVMBuildLoad2(ctx->builder, ptr_t, gep, "cap");
                        Type *pt = (i < entry->param_count) ? entry->params[i].type : NULL;
                        if (pt && type_is_integer(pt)) {
                            LLVMTypeRef uft = LLVMFunctionType(
                                LLVMInt64TypeInContext(ctx->context), &ptr_t, 1, 0);
                            all_args[i] = LLVMBuildCall2(ctx->builder, uft,
                                              get_rt_unbox_int(ctx), &boxed, 1, "ua");
                        } else if (pt && type_is_float(pt)) {
                            LLVMTypeRef uft = LLVMFunctionType(
                                LLVMDoubleTypeInContext(ctx->context), &ptr_t, 1, 0);
                            all_args[i] = LLVMBuildCall2(ctx->builder, uft,
                                              get_rt_unbox_float(ctx), &boxed, 1, "ua");
                        } else {
                            all_args[i] = boxed;
                        }
                    }

                    // Unbox remaining args from calln args array
                    for (int i = 0; i < remaining; i++) {
                        LLVMValueRef idx   = LLVMConstInt(i32, i, 0);
                        LLVMValueRef gep   = LLVMBuildGEP2(ctx->builder, ptr_t,
                                                            args_array_param, &idx, 1, "arg_ptr");
                        LLVMValueRef boxed = LLVMBuildLoad2(ctx->builder, ptr_t, gep, "arg");
                        Type *pt = (supplied + i < entry->param_count)
                                 ? entry->params[supplied + i].type : NULL;
                        if (pt && type_is_integer(pt)) {
                            LLVMTypeRef uft = LLVMFunctionType(
                                LLVMInt64TypeInContext(ctx->context), &ptr_t, 1, 0);
                            all_args[supplied + i] = LLVMBuildCall2(ctx->builder, uft,
                                                         get_rt_unbox_int(ctx), &boxed, 1, "ua");
                        } else if (pt && type_is_float(pt)) {
                            LLVMTypeRef uft = LLVMFunctionType(
                                LLVMDoubleTypeInContext(ctx->context), &ptr_t, 1, 0);
                            all_args[supplied + i] = LLVMBuildCall2(ctx->builder, uft,
                                                         get_rt_unbox_float(ctx), &boxed, 1, "ua");
                        } else {
                            all_args[supplied + i] = boxed;
                        }
                    }

                    // Call the real function with all args
                    LLVMTypeRef *call_param_types = malloc(sizeof(LLVMTypeRef) * declared_params);
                    for (int i = 0; i < declared_params; i++)
                        call_param_types[i] = type_to_llvm(ctx, entry->params[i].type);
                    LLVMTypeRef call_ft = LLVMFunctionType(
                        type_to_llvm(ctx, entry->return_type),
                        call_param_types, declared_params, 0);
                    LLVMValueRef raw = LLVMBuildCall2(ctx->builder, call_ft,
                                                      entry->func_ref, all_args,
                                                      declared_params, "curry_call");

                    // Box the result
                    LLVMValueRef boxed_ret = codegen_box(ctx, raw, entry->return_type);
                    LLVMBuildRet(ctx->builder, boxed_ret);

                    if (saved_block)
                        LLVMPositionBuilderAtEnd(ctx->builder, saved_block);
                    free(tramp_types);
                    free(call_param_types);
                    free(all_args);

                    // Build env array with supplied args (boxed)
                    LLVMTypeRef  arr_t   = LLVMArrayType(ptr_t, supplied ? supplied : 1);
                    LLVMValueRef env_arr = LLVMBuildAlloca(ctx->builder, arr_t, "curry_env");
                    for (int i = 0; i < supplied; i++) {
                        CodegenResult ar = codegen_expr(ctx, ast->list.items[i + 1]);
                        LLVMValueRef  bv = codegen_box(ctx, ar.value, ar.type);
                        LLVMValueRef zero   = LLVMConstInt(i32, 0, 0);
                        LLVMValueRef idx    = LLVMConstInt(i32, i, 0);
                        LLVMValueRef idxs[] = {zero, idx};
                        LLVMValueRef slot   = LLVMBuildGEP2(ctx->builder, arr_t,
                                                             env_arr, idxs, 2, "slot");
                        LLVMBuildStore(ctx->builder, bv, slot);
                    }
                    LLVMValueRef env_ptr = LLVMBuildBitCast(ctx->builder, env_arr, ptr_t, "env_ptr");

                    // Wrap trampoline + env in rt_value_closure
                    LLVMValueRef fn_ptr  = LLVMBuildBitCast(ctx->builder, tramp, ptr_t, "fn_ptr");
                    LLVMValueRef clo_fn  = get_rt_value_closure(ctx);
                    LLVMTypeRef  cp[]    = {ptr_t, ptr_t, i32, i32};
                    LLVMTypeRef  cft     = LLVMFunctionType(ptr_t, cp, 4, 0);
                    LLVMValueRef clo_args[] = {
                        fn_ptr, env_ptr,
                        LLVMConstInt(i32, supplied, 0),
                        LLVMConstInt(i32, remaining, 0)
                    };
                    result.value = LLVMBuildCall2(ctx->builder, cft, clo_fn, clo_args, 4, "curry");
                    result.type  = type_fn(NULL, 0, NULL);
                    return result;
                }


                if (entry->scheme) {
                    Type **_atypes = malloc(sizeof(Type*) * (declared_params ? declared_params : 1));
                    for (int _i = 0; _i < declared_params; _i++)
                        _atypes[_i] = entry->params[_i].type;
                    env_hm_check_call(ctx->env, head->symbol, _atypes, declared_params,
                                      parser_get_filename(), ast->line, ast->column);
                    free(_atypes);
                }

                /* ── Monomorphization: specialize if polymorphic ─────────── */
                if (!has_rest &&
                    entry->scheme && entry->scheme->quantified_count > 0
                    && entry->source_ast) {
                    // Collect concrete argument types from call site
                    int        nq      = entry->scheme->quantified_count;
                    TypeSubst  ts;
                    ts.count = nq;
                    ts.from  = malloc(sizeof(int)   * nq);
                    ts.to    = malloc(sizeof(Type*)  * nq);

                    // Map each quantified var to the concrete type of the
                    // corresponding argument
                    for (int i = 0; i < nq && i < declared_params; i++) {
                        ts.from[i] = entry->scheme->quantified[i];
                        AST *arg_ast = ast->list.items[i + 1];
                        Type *arg_type = arg_ast->inferred_type
                                       ? arg_ast->inferred_type
                                       : type_unknown();
                        ts.to[i] = arg_type;
                    }

                    // Fill remaining type vars with unknown if fewer args
                    for (int i = declared_params; i < nq; i++) {
                        ts.from[i] = entry->scheme->quantified[i];
                        ts.to[i]   = type_unknown();
                    }

                    // Check if all type vars are concrete
                    bool all_concrete = true;
                    for (int i = 0; i < nq; i++) {
                        if (!ts.to[i] || ts.to[i]->kind == TYPE_VAR ||
                            ts.to[i]->kind == TYPE_UNKNOWN ||
                            ts.to[i]->kind == TYPE_FN ||
                            ts.to[i]->kind == TYPE_ARROW ||
                            ts.to[i]->kind == TYPE_LIST) {
                            all_concrete = false;
                            break;
                        }
                    }



                    if (all_concrete) {
                        LLVMValueRef spec_fn = codegen_specialize(ctx,
                                                  head->symbol, entry, &ts);
                        if (spec_fn) {
                            // Look up the specialized entry
                            char *spec_name = mono_make_name(head->symbol,
                                                 ts.to, ts.count);
                            EnvEntry *spec_e = env_lookup(ctx->env, spec_name);
                            free(spec_name);

                            if (spec_e) {
                                // Call the specialized function directly
                                int sargs = spec_e->param_count;
                                LLVMValueRef *call_args = malloc(
                                    sizeof(LLVMValueRef) * (sargs ? sargs : 1));
                                LLVMTypeRef *call_types = malloc(
                                    sizeof(LLVMTypeRef) * (sargs ? sargs : 1));

                                for (int i = 0; i < sargs; i++) {
                                    CodegenResult ar = codegen_expr(ctx,
                                                          ast->list.items[i + 1]);
                                    call_args[i] = ar.value;
                                    // Use actual LLVM param type from the
                                    // specialized function, not env entry
                                    call_types[i] = LLVMTypeOf(
                                        LLVMGetParam(spec_fn, i));
                                    // Coerce if needed
                                    LLVMTypeRef at   = LLVMTypeOf(ar.value);
                                    LLVMTypeRef et   = call_types[i];
                                    LLVMTypeRef i64t = LLVMInt64TypeInContext(ctx->context);
                                    LLVMTypeRef dblt = LLVMDoubleTypeInContext(ctx->context);
                                    LLVMTypeRef i1t  = LLVMInt1TypeInContext(ctx->context);
                                    if (at != et) {
                                        if (et == i64t && at == i1t)
                                            call_args[i] = LLVMBuildZExt(ctx->builder,
                                                               ar.value, i64t, "coerce");
                                        else if (et == dblt && type_is_integer(ar.type))
                                            call_args[i] = LLVMBuildSIToFP(ctx->builder,
                                                               ar.value, dblt, "coerce");
                                        else if (et == i64t && at == dblt)
                                            call_args[i] = LLVMBuildFPToSI(ctx->builder,
                                                               ar.value, i64t, "coerce");
                                    }
                                }

                                for (int i = 0; i < sargs; i++)
                                    fprintf(stderr, "  arg[%d] type_kind=%d llvm_type=%d\n",
                                            i, spec_e->params[i].type->kind,
                                            (int)LLVMGetTypeKind(call_types[i]));
                                fprintf(stderr, "  ret type_kind=%d\n",
                                        spec_e->return_type->kind);

                                LLVMTypeRef spec_ret = type_to_llvm(ctx, spec_e->return_type);
                                LLVMTypeRef call_ft = LLVMFunctionType(
                                    spec_ret, call_types, sargs, 0);
                                // Verify spec_fn return type matches
                                LLVMTypeRef actual_ret = LLVMGetReturnType(
                                    LLVMGlobalGetValueType(spec_fn));
                                if (actual_ret != spec_ret) {
                                    // Use the actual function's return type
                                    call_ft = LLVMFunctionType(actual_ret,
                                                               call_types, sargs, 0);
                                    spec_ret = actual_ret;
                                }
                                result.value = LLVMBuildCall2(ctx->builder,
                                    call_ft, spec_fn, call_args, sargs, "mono_call");
                                result.type  = type_clone(spec_e->return_type);

                                result.type  = type_clone(spec_e->return_type);

                                fprintf(stderr, "MONO [%s :: %s]\n",
                                        spec_e->name ? spec_e->name : "?",
                                        type_to_string(spec_e->return_type));

                                free(call_args);
                                free(call_types);
                                free(ts.from);
                                free(ts.to);
                                return result;
                            }
                        }
                    }
                    free(ts.from);
                    free(ts.to);
                }

                /* ── Closure ABI direct call ────────────────────────────── */
                if (entry->is_closure_abi) {
                    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef i32   = LLVMInt32TypeInContext(ctx->context);

                    /* Wrap function in closure with empty env */
                    LLVMValueRef fn_ptr = LLVMBuildBitCast(ctx->builder,
                                                            entry->func_ref, ptr_t, "fn_ptr");
                    LLVMValueRef clo_fn  = get_rt_value_closure(ctx);
                    LLVMTypeRef  cp[]    = {ptr_t, ptr_t, i32, i32};
                    LLVMTypeRef  cft     = LLVMFunctionType(ptr_t, cp, 4, 0);
                    LLVMValueRef clo_args[] = {
                        fn_ptr,
                        LLVMConstPointerNull(ptr_t),
                        LLVMConstInt(i32, 0, 0),
                        LLVMConstInt(i32, declared_params, 0)
                    };
                    LLVMValueRef clo = LLVMBuildCall2(ctx->builder, cft,
                                                      clo_fn, clo_args, 4, "direct_clo");

                    /* Build args array on heap to avoid dynamic alloca in branches */
                    LLVMTypeRef  arr_t    = LLVMArrayType(ptr_t, declared_params ? declared_params : 1);
                    LLVMTypeRef  i64_t    = LLVMInt64TypeInContext(ctx->context);
                    LLVMValueRef arr_size = LLVMConstInt(i64_t,
                                               sizeof(void*) * (declared_params ? declared_params : 1), 0);
                    LLVMValueRef malloc_fn = LLVMGetNamedFunction(ctx->module, "malloc");
                    if (!malloc_fn) {
                        LLVMTypeRef ft = LLVMFunctionType(ptr_t, &i64_t, 1, 0);
                        malloc_fn = LLVMAddFunction(ctx->module, "malloc", ft);
                        LLVMSetLinkage(malloc_fn, LLVMExternalLinkage);
                    }
                    LLVMValueRef arr_ptr_raw = LLVMBuildCall2(ctx->builder,
                        LLVMFunctionType(ptr_t, &i64_t, 1, 0), malloc_fn, &arr_size, 1, "clo_args_heap");
                    LLVMValueRef arr_ptr = arr_ptr_raw;
                    for (int i = 0; i < declared_params; i++) {
                        CodegenResult ar  = codegen_expr(ctx, ast->list.items[i + 1]);
                        LLVMValueRef  bv  = codegen_box(ctx, ar.value, ar.type);
                        LLVMValueRef  zero   = LLVMConstInt(i32, 0, 0);
                        LLVMValueRef  idx    = LLVMConstInt(i32, i, 0);
                        LLVMValueRef  idxs[] = {zero, idx};
                        LLVMValueRef  slot   = LLVMBuildGEP2(ctx->builder, arr_t,
                                                              arr_ptr, idxs, 2, "slot");
                        LLVMBuildStore(ctx->builder, bv, slot);
                    }

                    /* Decay to RuntimeValue** — bitcast array ptr to ptr */
                    LLVMValueRef args_ptr = declared_params > 0
                        ? LLVMBuildBitCast(ctx->builder, arr_ptr, ptr_t, "args_ptr")
                        : LLVMConstPointerNull(ptr_t);

                    /* Call via rt_closure_calln */
                    LLVMValueRef calln_fn = get_rt_closure_calln(ctx);
                    LLVMTypeRef  calln_p[] = {ptr_t, i32, ptr_t};
                    LLVMTypeRef  calln_ft  = LLVMFunctionType(ptr_t, calln_p, 3, 0);
                    LLVMValueRef calln_a[] = {
                        clo,
                        LLVMConstInt(i32, declared_params, 0),
                        args_ptr
                    };
                    result.value = LLVMBuildCall2(ctx->builder, calln_ft,
                                                  calln_fn, calln_a, 3, "clo_calln");
                    /* Free the heap-allocated args array */
                    LLVMValueRef free_fn = LLVMGetNamedFunction(ctx->module, "free");
                    if (!free_fn) {
                        LLVMTypeRef ft = LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), &ptr_t, 1, 0);
                        free_fn = LLVMAddFunction(ctx->module, "free", ft);
                        LLVMSetLinkage(free_fn, LLVMExternalLinkage);
                    }
                    LLVMBuildCall2(ctx->builder,
                        LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), &ptr_t, 1, 0),
                        free_fn, &arr_ptr_raw, 1, "");
                    result.type  = type_unknown();
                    return result;
                }

                /* ── Typed ABI direct call ──────────────────────────────── */
                int total_args = declared_params + entry->lifted_count;
                LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (total_args ? total_args : 1));
                for (int i = 0; i < declared_params; i++) {
                    if (has_rest && i == declared_params - 1) {
                        // Collect remaining args into a runtime list
                        LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                        LLVMValueRef list_fn = get_rt_list_new(ctx);
                        LLVMTypeRef  list_ft = LLVMFunctionType(ptr, NULL, 0, 0);
                        LLVMValueRef list    = LLVMBuildCall2(ctx->builder, list_ft,
                                                              list_fn, NULL, 0, "rest_list");
                        LLVMValueRef append_fn = get_rt_list_append(ctx);
                        LLVMTypeRef  ap[]      = {ptr, ptr};
                        LLVMTypeRef  aft       = LLVMFunctionType(
                            LLVMVoidTypeInContext(ctx->context), ap, 2, 0);

                        // Resolve the declared element type for typed rest params.
                        // entry->params[i].type is type_list(T) for `. [args :: T]`
                        // and type_list(NULL) for bare rest params.
                        Type *rest_elem_type = NULL;
                        {
                            Type *rpt = entry->params[i].type;
                            if (rpt && rpt->kind == TYPE_LIST &&
                                rpt->list_elem && rpt->list_elem->kind != TYPE_UNKNOWN)
                                rest_elem_type = rpt->list_elem;
                        }

                        // Special case: exactly one argument and it is already a
                        // List — pass it directly as the rest param instead of
                        // wrapping it. This handles (my-sum xs) where xs is the
                        // tail of a previous rest param.
                        if (ast->list.count == (size_t)(i + 2)) {
                            CodegenResult only = codegen_expr(ctx, ast->list.items[i + 1]);
                            if (only.type && (only.type->kind == TYPE_LIST ||
                                              only.type->kind == TYPE_UNKNOWN)) {
                                // Already a list (or opaque ptr) — pass directly
                                // as the rest param without re-boxing.
                                args[i] = only.value;
                                break;
                            }
                            // Not a list — fall through to normal per-element path
                            // by putting it back. We re-codegen below which is
                            // slightly wasteful but correct.
                        }

                        // If there are no variadic arguments, pass an empty list
                        if ((size_t)(i + 1) >= ast->list.count) {
                            LLVMValueRef empty_fn = get_rt_list_new(ctx);
                            LLVMTypeRef  ptr      = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                            LLVMTypeRef  empty_ft = LLVMFunctionType(ptr, NULL, 0, 0);
                            args[i] = LLVMBuildCall2(ctx->builder, empty_ft,
                                                     empty_fn, NULL, 0, "empty_rest");
                            break;
                        }

                        for (size_t j = i + 1; j < ast->list.count; j++) {
                            CodegenResult er = codegen_expr(ctx, ast->list.items[j]);


                            // Type-check each element against the declared element type.
                            if (rest_elem_type && er.type &&
                                rest_elem_type->kind != TYPE_UNKNOWN) {
                                bool type_ok = (er.type->kind == rest_elem_type->kind);
                                // Integer subtypes (Hex, Bin, Oct) all satisfy Int
                                if (!type_ok && rest_elem_type->kind == TYPE_INT &&
                                    type_is_integer(er.type))
                                    type_ok = true;
                                // Integer widens to Float
                                if (!type_ok && rest_elem_type->kind == TYPE_FLOAT &&
                                    type_is_integer(er.type))
                                    type_ok = true;

                                if (!type_ok) {
                                    CODEGEN_ERROR(ctx,
                                                  "%s:%d:%d: error: variadic argument %zu to '%s' "
                                                  "has type %s but rest parameter '[%s :: %s]' requires %s",
                                                  parser_get_filename(),
                                                  ast->list.items[j]->line,
                                                  ast->list.items[j]->column,
                                                  j - i,
                                                  head->symbol,
                                                  type_to_string(er.type),
                                                  entry->params[i].name,
                                                  type_to_string(rest_elem_type),
                                                  type_to_string(rest_elem_type));
                                }

                                // Coerce integer up to float if needed
                                if (rest_elem_type->kind == TYPE_FLOAT &&
                                    type_is_integer(er.type)) {
                                    er.value = LLVMBuildSIToFP(ctx->builder, er.value,
                                                               LLVMDoubleTypeInContext(ctx->context),
                                                               "rest_widen");
                                    er.type  = type_float();
                                }
                            }

                            LLVMValueRef bv  = codegen_box(ctx, er.value, er.type);
                            LLVMValueRef aa[] = {list, bv};
                            LLVMBuildCall2(ctx->builder, aft, append_fn, aa, 2, "");
                        }
                        args[i] = list;
                        break;
                    }

                    CodegenResult arg_result = codegen_expr(ctx, ast->list.items[i + 1]);

                    Type *expected_type = entry->params[i].type;
                    Type *actual_type   = arg_result.type;

                    LLVMValueRef converted_arg = arg_result.value;

                    /* If expected type is Fn, wrap argument as a closure */
                    if (expected_type && expected_type->kind == TYPE_FN) {
                        if (ast->list.items[i + 1]->type == AST_SYMBOL) {
                            EnvEntry *ae = env_lookup(ctx->env,
                                           ast->list.items[i + 1]->symbol);
                            if (ae && ae->kind == ENV_FUNC && ae->func_ref) {
                                converted_arg = wrap_func_as_closure(ctx, ae);
                            } else if (ae && ae->kind == ENV_VAR &&
                                       ae->type && ae->type->kind == TYPE_FN) {
                                LLVMTypeRef ptr_t = LLVMPointerType(
                                    LLVMInt8TypeInContext(ctx->context), 0);
                                converted_arg = LLVMBuildLoad2(ctx->builder,
                                    ptr_t, ae->value, ae->name);
                            }
                        }
                        /* Lambda or expression — codegen_expr already returned closure */
                        args[i] = converted_arg;
                        continue;
                    }

                    if (expected_type && expected_type->kind == TYPE_UNKNOWN) {
                        if (actual_type->kind == TYPE_UNKNOWN) {
                            converted_arg = arg_result.value;
                        } else {
                            converted_arg = codegen_box(ctx, arg_result.value, arg_result.type);
                        }
                    } else if (expected_type && actual_type && expected_type->kind != actual_type->kind) {
                        LLVMTypeRef expected_llvm = type_to_llvm(ctx, expected_type);
                        if (type_is_integer(expected_type) && type_is_float(actual_type)) {
                            converted_arg = LLVMBuildFPToSI(ctx->builder, arg_result.value,
                                                            expected_llvm, "arg_conv");
                        } else if (type_is_float(expected_type) && type_is_integer(actual_type)) {
                            if (actual_type->kind == TYPE_CHAR) {
                                LLVMValueRef ext = LLVMBuildSExt(ctx->builder, arg_result.value,
                                                                  LLVMInt64TypeInContext(ctx->context), "ext");
                                converted_arg = LLVMBuildSIToFP(ctx->builder, ext,
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

                for (int i = 0; i < entry->lifted_count; i++) {
                    int           idx      = declared_params + i;
                    const char   *cap_name = entry->params[idx].name;
                    EnvEntry     *cap_e    = env_lookup(ctx->env, cap_name);
                    LLVMValueRef  cap_val  = NULL;
                    if (cap_e && cap_e->kind == ENV_VAR && cap_e->value) {
                        LLVMTypeRef cap_llvm = type_to_llvm(ctx, cap_e->type);
                        cap_val = LLVMBuildLoad2(ctx->builder, cap_llvm,
                                                  cap_e->value, cap_name);
                    } else {
                        cap_val = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                    }
                    args[idx] = cap_val;
                }

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
                result.type  = type_clone(entry->return_type);
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
                } else if (at->kind == TYPE_UNKNOWN ||
                           at->kind == TYPE_LIST   ||
                           at->kind == TYPE_RATIO  ||
                           at->kind == TYPE_SYMBOL) {
                    // Opaque RuntimeValue* — unbox as int
                    LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef  uft   = LLVMFunctionType(LLVMInt64TypeInContext(ctx->context),
                                                          &ptr_t, 1, 0);
                    as_i64 = LLVMBuildCall2(ctx->builder, uft,
                                 get_rt_unbox_int(ctx), &av, 1, "unbox_int");
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

        // Handle ((expr) args...) — calling a non-symbol expression in head position
        // e.g. ((true 1) 2) where (true 1) returns a closure
        if (head->type != AST_SYMBOL && head->type != AST_LAMBDA) {
            LLVMTypeRef  ptr_t  = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
            LLVMTypeRef  i32    = LLVMInt32TypeInContext(ctx->context);
            CodegenResult fn_r  = codegen_expr(ctx, head);
            int           n_args = (int)ast->list.count - 1;

            LLVMTypeRef  arr_t   = LLVMArrayType(ptr_t, n_args ? n_args : 1);
            LLVMValueRef arr_ptr = LLVMBuildAlloca(ctx->builder, arr_t, "call_args");
            for (int i = 0; i < n_args; i++) {
                CodegenResult ar  = codegen_expr(ctx, ast->list.items[i + 1]);
                LLVMValueRef  bv  = codegen_box(ctx, ar.value, ar.type);
                LLVMValueRef  zero   = LLVMConstInt(i32, 0, 0);
                LLVMValueRef  idx    = LLVMConstInt(i32, i, 0);
                LLVMValueRef  idxs[] = {zero, idx};
                LLVMValueRef  slot   = LLVMBuildGEP2(ctx->builder, arr_t,
                                                      arr_ptr, idxs, 2, "slot");
                LLVMBuildStore(ctx->builder, bv, slot);
            }
            LLVMValueRef args_ptr = n_args > 0
                ? LLVMBuildBitCast(ctx->builder, arr_ptr, ptr_t, "args_ptr")
                : LLVMConstPointerNull(ptr_t);

            LLVMValueRef calln_fn = get_rt_closure_calln(ctx);
            LLVMTypeRef  calln_p[] = {ptr_t, i32, ptr_t};
            LLVMTypeRef  calln_ft  = LLVMFunctionType(ptr_t, calln_p, 3, 0);
            LLVMValueRef calln_a[] = {
                fn_r.value,
                LLVMConstInt(i32, n_args, 0),
                args_ptr
            };
            result.value = LLVMBuildCall2(ctx->builder, calln_ft,
                                          calln_fn, calln_a, 3, "expr_calln");
            result.type  = type_unknown();
            return result;
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
                // Patch lambda param type from inferred arg type,
                // but only if the type is concrete — never patch with
                // unknown/'?' since that will fail type_from_name later.
                if (i < head->lambda.param_count &&
                    head->lambda.params[i].type_name == NULL &&
                    arg_results[i].type &&
                    arg_results[i].type->kind != TYPE_UNKNOWN) {
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

    case AST_LAMBDA: {
        static int anon_val_count = 0;
        char anon_name[64];
        snprintf(anon_name, sizeof(anon_name), "__anon_val_%d", anon_val_count++);
        AST *name_node   = ast_new_symbol(anon_name);
        AST *define_node = ast_new_list();
        ast_list_append(define_node, ast_new_symbol("define"));
        ast_list_append(define_node, name_node);
        ast_list_append(define_node, ast);
        CodegenResult clo_result = codegen_expr(ctx, define_node);

        /* Closure ABI — define path already returned closure value */
        if (clo_result.type && clo_result.type->kind == TYPE_FN) {
            result = clo_result;
            return result;
        }

        /* Non-closure ABI — wrap with trampoline */
        EnvEntry *e = env_lookup(ctx->env, anon_name);
        if (e && e->kind == ENV_FUNC && e->func_ref) {
            result.value = wrap_func_as_closure(ctx, e);
            result.type  = type_fn(NULL, 0, NULL);
            return result;
        }

        result = clo_result;
        return result;
    }

    default:
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown AST type: %d",
                parser_get_filename(), ast->line, ast->column, ast->type);
    }
}

/// Module


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

    // Map
    env_insert_builtin(ctx->env, "map?",     1, 0, "Test if value is a map");
    env_insert_builtin(ctx->env, "assoc",    3, 0, "Add or update a key-value pair in a map (immutable)");
    env_insert_builtin(ctx->env, "assoc!",   3, 0, "Add or update a key-value pair in a map in place");
    env_insert_builtin(ctx->env, "dissoc",   2, 0, "Remove a key from a map (immutable)");
    env_insert_builtin(ctx->env, "dissoc!",  2, 0, "Remove a key from a map in place");
    env_insert_builtin(ctx->env, "find",     2, 0, "Return (key val) pair for key in map, or nil");
    env_insert_builtin(ctx->env, "keys",     1, 0, "Return a list of all keys in a map");
    env_insert_builtin(ctx->env, "vals",     1, 0, "Return a list of all values in a map");
    env_insert_builtin(ctx->env, "merge",    2, 0, "Merge two maps, rightmost wins on conflict");

    // Set
    env_insert_builtin(ctx->env, "set",       0, -1, "Create a set from arguments or convert a collection");
    env_insert_builtin(ctx->env, "set?",      1,  0, "Test if value is a set");
    env_insert_builtin(ctx->env, "conj",      2,  0, "Add an element to a set");
    env_insert_builtin(ctx->env, "disj",      2,  0, "Remove an element from a set");
    env_insert_builtin(ctx->env, "conj!",     2,  0, "Mutate a set by adding an element in place");
    env_insert_builtin(ctx->env, "disj!",     2,  0, "Mutate a set by removing an element in place");
    env_insert_builtin(ctx->env, "contains?", 2,  0, "Test if a set contains an element");
    env_insert_builtin(ctx->env, "count",     1,  0, "Get number of elements in a set");

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

    env_insert_builtin(ctx->env, "take", 2, 0, "Take n elements from a (possibly infinite) list");
    env_insert_builtin(ctx->env, "drop", 2, 0, "Drop n elements from a list");
}
