#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "reader.h"
#include "types.h"
#include "env.h"
#include "ffi.h"
#include "asm.h"
#include "runtime.h"
#include "module.h"
#include "infer.h"
#include "typeclass.h"
#include "pmatch.h"
#include <ctype.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/TargetMachine.h>
#include <dlfcn.h>

static LLVMValueRef emit_call_0(CodegenContext *ctx, LLVMValueRef fn, LLVMTypeRef ret_t, const char *name);
static LLVMValueRef emit_call_1(CodegenContext *ctx, LLVMValueRef fn, LLVMTypeRef ret_t, LLVMValueRef a1, const char *name);
static LLVMValueRef emit_call_2(CodegenContext *ctx, LLVMValueRef fn, LLVMTypeRef ret_t, LLVMValueRef a1, LLVMValueRef a2, const char *name);
static LLVMValueRef emit_call_3(CodegenContext *ctx, LLVMValueRef fn, LLVMTypeRef ret_t, LLVMValueRef a1, LLVMValueRef a2, LLVMValueRef a3, const char *name);
static void emit_runtime_error_val(CodegenContext *ctx, AST *ast, LLVMValueRef msg_val);
static void emit_runtime_error(CodegenContext *ctx, AST *ast, const char *msg);

static void emit_bounds_check(CodegenContext *ctx, AST *ast, LLVMValueRef idx, LLVMValueRef size, const char *err_msg) {
    LLVMValueRef ge_zero = LLVMBuildICmp(ctx->builder, LLVMIntSGE, idx, LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0), "ge_z");
    LLVMValueRef lt_size = LLVMBuildICmp(ctx->builder, LLVMIntSLT, idx, size, "lt_s");
    LLVMValueRef in_bounds = LLVMBuildAnd(ctx->builder, ge_zero, lt_size, "in_bounds");
    LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(ctx->context, LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder)), "ok");
    LLVMBasicBlockRef err_bb = LLVMAppendBasicBlockInContext(ctx->context, LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder)), "err");
    LLVMBuildCondBr(ctx->builder, in_bounds, ok_bb, err_bb);
    LLVMPositionBuilderAtEnd(ctx->builder, err_bb);
    emit_runtime_error(ctx, ast, err_msg);
    LLVMPositionBuilderAtEnd(ctx->builder, ok_bb);
}

static LLVMValueRef emit_type_cast(CodegenContext *ctx, LLVMValueRef val, LLVMTypeRef dst_t) {
    LLVMTypeRef src_t = LLVMTypeOf(val);
    if (src_t == dst_t) return val;
    LLVMTypeKind sk = LLVMGetTypeKind(src_t), dk = LLVMGetTypeKind(dst_t);
    if (sk == LLVMIntegerTypeKind && dk == LLVMPointerTypeKind) {
        LLVMTypeRef i64t = LLVMInt64TypeInContext(ctx->context);
        if (src_t != i64t) val = LLVMBuildZExt(ctx->builder, val, i64t, "zext");
        return emit_call_1(ctx, get_rt_value_int(ctx), dst_t, val, "box");
    }
    if ((sk == LLVMDoubleTypeKind || sk == LLVMFloatTypeKind) && dk == LLVMPointerTypeKind) {
        LLVMTypeRef dblt = LLVMDoubleTypeInContext(ctx->context);
        if (sk == LLVMFloatTypeKind) val = LLVMBuildFPExt(ctx->builder, val, dblt, "fpext");
        return emit_call_1(ctx, get_rt_value_float(ctx), dst_t, val, "box");
    }
    if (sk == LLVMPointerTypeKind && dk == LLVMIntegerTypeKind) return LLVMBuildPtrToInt(ctx->builder, val, dst_t, "p2i");
    if (sk == LLVMPointerTypeKind && (dk == LLVMFloatTypeKind || dk == LLVMDoubleTypeKind)) {
        LLVMTypeRef pt = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
        LLVMTypeRef i64t = LLVMInt64TypeInContext(ctx->context);
        LLVMValueRef u = emit_call_1(ctx, get_rt_unbox_int(ctx), i64t, val, "ub");
        return LLVMBuildSIToFP(ctx->builder, u, dst_t, "u2f");
    }
    if (sk == LLVMIntegerTypeKind && (dk == LLVMFloatTypeKind || dk == LLVMDoubleTypeKind)) return LLVMBuildSIToFP(ctx->builder, val, dst_t, "i2f");
    if ((sk == LLVMFloatTypeKind || sk == LLVMDoubleTypeKind) && dk == LLVMIntegerTypeKind) return LLVMBuildFPToSI(ctx->builder, val, dst_t, "f2i");
    if (sk == LLVMFloatTypeKind && dk == LLVMDoubleTypeKind) return LLVMBuildFPExt(ctx->builder, val, dst_t, "fpext");
    if (sk == LLVMDoubleTypeKind && dk == LLVMFloatTypeKind) return LLVMBuildFPTrunc(ctx->builder, val, dst_t, "fptrunc");
    if (sk == LLVMIntegerTypeKind && dk == LLVMIntegerTypeKind) return LLVMBuildIntCast2(ctx->builder, val, dst_t, 0, "icast");
    return LLVMBuildBitCast(ctx->builder, val, dst_t, "bc");
}

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
    ctx->ffi = NULL;
    // Initialize monomorphization cache
    ctx->mono_cache.entries  = NULL;
    ctx->mono_cache.count    = 0;
    ctx->mono_cache.capacity = 0;
    memset(ctx->error_msg, 0, sizeof(ctx->error_msg));
    ctx->tc_registry = tc_registry_create();
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
                    func, NULL, NULL);
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

static LLVMValueRef get_or_build_fmt(CodegenContext *ctx, LLVMValueRef *cache, const char *fmt, const char *name) {
    if (cache && !*cache) *cache = LLVMBuildGlobalStringPtr(ctx->builder, fmt, name);
    return cache ? *cache : LLVMBuildGlobalStringPtr(ctx->builder, fmt, name);
}

LLVMValueRef get_fmt_str             (CodegenContext *ctx) { return get_or_build_fmt(ctx, &ctx->fmt_str,   "%s\n",    "fmt_str"     ); }
LLVMValueRef get_fmt_char            (CodegenContext *ctx) { return get_or_build_fmt(ctx, &ctx->fmt_char,  "%c\n",    "fmt_char"    ); }
LLVMValueRef get_fmt_int             (CodegenContext *ctx) { return get_or_build_fmt(ctx, &ctx->fmt_int,   "%ld\n",   "fmt_int"     ); }
LLVMValueRef get_fmt_float           (CodegenContext *ctx) { return get_or_build_fmt(ctx, &ctx->fmt_float, "%.16g\n", "fmt_float"   ); }
LLVMValueRef get_fmt_hex             (CodegenContext *ctx) { return get_or_build_fmt(ctx, &ctx->fmt_hex,   "0x%lX\n", "fmt_hex"     ); }
LLVMValueRef get_fmt_oct             (CodegenContext *ctx) { return get_or_build_fmt(ctx, &ctx->fmt_oct,   "0o%lo\n", "fmt_oct"     ); }
LLVMValueRef get_fmt_str_no_newline  (CodegenContext *ctx) { return get_or_build_fmt(ctx, NULL,            "%s",      "fmt_str_nn"  ); }
LLVMValueRef get_fmt_char_no_newline (CodegenContext *ctx) { return get_or_build_fmt(ctx, NULL,            "%c",      "fmt_char_nn" ); }
LLVMValueRef get_fmt_int_no_newline  (CodegenContext *ctx) { return get_or_build_fmt(ctx, NULL,            "%ld",     "fmt_int_nn"  ); }
LLVMValueRef get_fmt_float_no_newline(CodegenContext *ctx) { return get_or_build_fmt(ctx, NULL,            "%.16g",   "fmt_float_nn"); }

void codegen_dispose(CodegenContext *ctx) {
    LLVMDisposeBuilder(ctx->builder);
    LLVMDisposeModule(ctx->module);
    LLVMContextDispose(ctx->context);
    mono_cache_free(&ctx->mono_cache);
    env_free(ctx->env);
    tc_registry_free(ctx->tc_registry);
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
    case TYPE_ARR: {
        /* Fat pointer struct: { T* data, i64 size }
         * Used when arr_is_fat=true OR arr_size is unknown.
         * Known-size stack arrays keep their [N x T] type for
         * internal storage but are wrapped in fat ptr when passed/returned. */
        if (t->arr_is_fat || t->arr_size < 0) {
            LLVMTypeRef existing = LLVMGetTypeByName2(ctx->context, "arr.fat");
            if (existing) return LLVMPointerType(existing, 0);
            LLVMTypeRef ptr_t    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
            LLVMTypeRef i64_t    = LLVMInt64TypeInContext(ctx->context);
            LLVMTypeRef fields[] = {ptr_t, i64_t};
            LLVMTypeRef fat      = LLVMStructCreateNamed(ctx->context, "arr.fat");
            LLVMStructSetBody(fat, fields, 2, 0);
            return LLVMPointerType(fat, 0);
        }
        /* Known-size stack array */
        LLVMTypeRef elem_type = type_to_llvm(ctx, t->arr_element_type);
        return LLVMArrayType(elem_type, t->arr_size);
    }
    case TYPE_SET:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    case TYPE_MAP:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    case TYPE_COLL:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    case TYPE_F32:  return LLVMFloatTypeInContext(ctx->context);
    case TYPE_I8:  case TYPE_U8:  return LLVMInt8TypeInContext(ctx->context);
    case TYPE_I16: case TYPE_U16: return LLVMInt16TypeInContext(ctx->context);
    case TYPE_I32: case TYPE_U32: return LLVMInt32TypeInContext(ctx->context);
    case TYPE_I64: case TYPE_U64: return LLVMInt64TypeInContext(ctx->context);
    case TYPE_I128:case TYPE_U128:return LLVMInt128TypeInContext(ctx->context);

    case TYPE_INT_ARBITRARY:
        return LLVMIntTypeInContext(ctx->context, t->numeric_width);
    case TYPE_FN:
    case TYPE_ARROW:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    case TYPE_LAYOUT: {
        /* Resolve name-only refs (from FFI) to the full registered layout */
        /* When used as a nested field, return raw struct not pointer */
        if (t->layout_is_inline) {
            if (t->layout_field_count == 0 && t->layout_name) {
                Type *full = env_lookup_layout(ctx->env, t->layout_name);
                if (full && full->layout_field_count > 0) t = full;
            }
            char struct_name[256];
            snprintf(struct_name, sizeof(struct_name), "layout.%s", t->layout_name);
            LLVMTypeRef existing = LLVMGetTypeByName2(ctx->context, struct_name);
            if (existing) return existing;
            /* Not yet created — build it now inline */
            if (t->layout_field_count > 0) {
                LLVMTypeRef st = LLVMStructCreateNamed(ctx->context, struct_name);
                LLVMTypeRef *fts = malloc(sizeof(LLVMTypeRef) * t->layout_field_count);
                for (int i = 0; i < t->layout_field_count; i++)
                    fts[i] = type_to_llvm(ctx, t->layout_fields[i].type);
                LLVMStructSetBody(st, fts, t->layout_field_count, t->layout_packed ? 1 : 0);
                free(fts);
                return st;
            }
        }
        /* Resolve name-only refs (from FFI) to the full registered layout */
        if (t->layout_field_count == 0 && t->layout_name) {
            Type *full = env_lookup_layout(ctx->env, t->layout_name);
            if (full && full->layout_field_count > 0)
                t = full;
        }
        char struct_name[256];
        snprintf(struct_name, sizeof(struct_name), "layout.%s", t->layout_name);
        /* Always check for existing named struct first — the LLVM context
         * is shared across all REPL modules so the type persists */
        LLVMTypeRef existing = LLVMGetTypeByName2(ctx->context, struct_name);
        if (existing) return LLVMPointerType(existing, 0);
        /* If we still have no fields, return opaque pointer — do NOT
         * create an empty named struct as it breaks GEP later */
        if (t->layout_field_count == 0)
            return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
        /* Create the named struct BEFORE recursing into fields so any
         * self-referential or mutually recursive structs (e.g. SDL linked
         * list nodes) find the existing type and return a pointer to it
         * instead of recursing infinitely. */
        LLVMTypeRef struct_type = LLVMStructCreateNamed(ctx->context, struct_name);
        LLVMTypeRef *field_types = malloc(sizeof(LLVMTypeRef) *
                                          t->layout_field_count);
        for (int i = 0; i < t->layout_field_count; i++)
            field_types[i] = type_to_llvm(ctx, t->layout_fields[i].type);
        LLVMStructSetBody(struct_type, field_types, t->layout_field_count,
                          t->layout_packed ? 1 : 0);
        free(field_types);
        return LLVMPointerType(struct_type, 0);
    }
    case TYPE_BOOL:
        return LLVMInt1TypeInContext(ctx->context);
    case TYPE_PTR:
    case TYPE_OPTIONAL:
    case TYPE_NIL:
        /* Typed pointer / Optionals — represented as i8* in LLVM, inner type is
         * tracked in the type system only for user-facing display/safety */
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    case TYPE_UNKNOWN:
        return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    default:
        return LLVMDoubleTypeInContext(ctx->context);
    }
}

/// Fat array helpers

/* Get or create the arr.fat struct type */
static LLVMTypeRef get_arr_fat_type(CodegenContext *ctx) {
    LLVMTypeRef existing = LLVMGetTypeByName2(ctx->context, "arr.fat");
    if (existing) return existing;
    LLVMTypeRef ptr_t    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef i64_t    = LLVMInt64TypeInContext(ctx->context);
    LLVMTypeRef fields[] = {ptr_t, i64_t};
    LLVMTypeRef fat      = LLVMStructCreateNamed(ctx->context, "arr.fat");
    LLVMStructSetBody(fat, fields, 2, 0);
    return fat;
}

/* Wrap a stack-allocated [N x T]* into a heap-allocated fat pointer.
 * Returns an arr.fat* whose data field points to the stack array. */
static LLVMValueRef arr_make_fat(CodegenContext *ctx,
                                  LLVMValueRef stack_ptr,
                                  int size,
                                  Type *elem_type) {
    LLVMTypeRef fat_t = get_arr_fat_type(ctx);
    LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);
    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

    /* Heap-allocate both the data and the fat struct so they survive
     * across REPL modules and stack frames */
    LLVMValueRef malloc_fn = LLVMGetNamedFunction(ctx->module, "malloc");
    if (!malloc_fn) {
        LLVMTypeRef ft = LLVMFunctionType(ptr_t, &i64_t, 1, 0);
        malloc_fn = LLVMAddFunction(ctx->module, "malloc", ft);
        LLVMSetLinkage(malloc_fn, LLVMExternalLinkage);
    }
    LLVMValueRef memcpy_fn = LLVMGetNamedFunction(ctx->module, "memcpy");
    if (!memcpy_fn) {
        LLVMTypeRef p3[] = {ptr_t, ptr_t, i64_t};
        LLVMTypeRef ft   = LLVMFunctionType(ptr_t, p3, 3, 0);
        memcpy_fn = LLVMAddFunction(ctx->module, "memcpy", ft);
        LLVMSetLinkage(memcpy_fn, LLVMExternalLinkage);
    }

    /* Allocate heap copy of the element data */
    LLVMTypeRef  elem_llvm   = elem_type ? type_to_llvm(ctx, elem_type) : i64_t;
    LLVMValueRef elem_size   = LLVMSizeOf(elem_llvm);
    LLVMValueRef n           = LLVMConstInt(i64_t, size, 0);
    LLVMValueRef data_bytes  = LLVMBuildMul(ctx->builder, elem_size, n, "data_bytes");
    LLVMValueRef data_heap   = LLVMBuildCall2(ctx->builder,
                                   LLVMFunctionType(ptr_t, &i64_t, 1, 0),
                                   malloc_fn, &data_bytes, 1, "data_heap");
    /* Copy stack data to heap */
    LLVMValueRef src_ptr     = LLVMBuildBitCast(ctx->builder, stack_ptr, ptr_t, "src");
    LLVMValueRef mc_args[]   = {data_heap, src_ptr, data_bytes};
    LLVMBuildCall2(ctx->builder,
        LLVMFunctionType(ptr_t, (LLVMTypeRef[]){ptr_t, ptr_t, i64_t}, 3, 0),
        memcpy_fn, mc_args, 3, "");

    /* Heap-allocate the fat struct */
    LLVMValueRef fat_size  = LLVMSizeOf(fat_t);
    LLVMValueRef fat_heap  = LLVMBuildCall2(ctx->builder,
                                 LLVMFunctionType(ptr_t, &i64_t, 1, 0),
                                 malloc_fn, &fat_size, 1, "fat_heap");
    LLVMValueRef fat       = LLVMBuildBitCast(ctx->builder, fat_heap,
                                 LLVMPointerType(fat_t, 0), "fat_ptr");

    /* Store data pointer and size into fat struct */
    LLVMValueRef data_field = LLVMBuildStructGEP2(ctx->builder, fat_t, fat, 0, "data_field");
    LLVMBuildStore(ctx->builder, data_heap, data_field);

    LLVMValueRef size_field = LLVMBuildStructGEP2(ctx->builder, fat_t, fat, 1, "size_field");
    LLVMBuildStore(ctx->builder, LLVMConstInt(i64_t, size, 0), size_field);

    return fat;
}



/* Extract data pointer from fat array, cast to elem_type* */
LLVMValueRef arr_fat_data(CodegenContext *ctx,
                                  LLVMValueRef fat_ptr,
                                  Type *elem_type) {
    LLVMTypeRef fat_t = get_arr_fat_type(ctx);
    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMValueRef data_field = LLVMBuildStructGEP2(ctx->builder, fat_t, fat_ptr, 0, "data_f");
    LLVMValueRef data_raw   = LLVMBuildLoad2(ctx->builder, ptr_t, data_field, "data_raw");
    LLVMTypeRef  elem_llvm  = elem_type ? type_to_llvm(ctx, elem_type) : ptr_t;
    return LLVMBuildBitCast(ctx->builder, data_raw,
                            LLVMPointerType(elem_llvm, 0), "data_ptr");
}

/* Extract runtime size from fat array */
LLVMValueRef arr_fat_size(CodegenContext *ctx, LLVMValueRef fat_ptr) {
    LLVMTypeRef fat_t = get_arr_fat_type(ctx);
    LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);
    LLVMValueRef size_field = LLVMBuildStructGEP2(ctx->builder, fat_t, fat_ptr, 1, "size_f");
    return LLVMBuildLoad2(ctx->builder, i64_t, size_field, "arr_size");
}


/// Type checking for operations

bool type_is_numeric (Type *t) { return t && (t->kind == TYPE_INT   || t->kind == TYPE_FLOAT || t->kind == TYPE_HEX || t->kind == TYPE_BIN || t->kind == TYPE_OCT  || t->kind == TYPE_CHAR || t->kind == TYPE_RATIO); }
bool type_is_integer (Type *t) { return t && (t->kind == TYPE_INT   || t->kind == TYPE_HEX   || t->kind == TYPE_BIN || t->kind == TYPE_OCT || t->kind == TYPE_CHAR || (t->kind >= TYPE_I8 && t->kind <= TYPE_U128) || t->kind == TYPE_INT_ARBITRARY); }
bool type_is_float   (Type *t) { return t && (t->kind == TYPE_FLOAT || t->kind == TYPE_F32   || t->kind == TYPE_F80); }
bool type_is_unsigned(Type *t) { return t && (t->kind == TYPE_U8    || t->kind == TYPE_U16   || t->kind == TYPE_U32 || t->kind == TYPE_U64 || t->kind == TYPE_U128); }

char *mangle_unicode_name(const char *name) {
    /* Check if name needs mangling — any non-ASCII char OR any ASCII
     * char that is invalid in an LLVM/system symbol name.            */
    static const char *special = "^~!@#$%&*+-=|<>?/\\`'\"()[]{},.;: ";
    bool needs_mangle = false;
    for (const char *cp = name; *cp; cp++) {
        if ((unsigned char)*cp > 127) { needs_mangle = true; break; }
        if (strchr(special, *cp))     { needs_mangle = true; break; }
    }
    if (!needs_mangle) return NULL;
    char *mangled = malloc(256);
    strcpy(mangled, "__alias_");
    size_t mpos = strlen(mangled);
    for (const char *cp = name; *cp && mpos < 252; cp++)
        mpos += snprintf(mangled + mpos, 256 - mpos, "%02x", (unsigned char)*cp);
    return mangled;
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
    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);

    switch (ast->type) {
    case AST_NUMBER:
        emit_call_2(ctx, printf_fn, i32, get_fmt_float_no_newline(ctx), LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), ast->number), "");
        break;
    case AST_SYMBOL:
        emit_call_2(ctx, printf_fn, i32, get_fmt_str_no_newline(ctx), LLVMBuildGlobalStringPtr(ctx->builder, ast->symbol, "sym"), "");
        break;
    case AST_STRING:
        emit_call_2(ctx, printf_fn, i32, LLVMBuildGlobalStringPtr(ctx->builder, "\"%s\"", "fmt"), LLVMBuildGlobalStringPtr(ctx->builder, ast->string, "str"), "");
        break;
    case AST_CHAR:
        emit_call_2(ctx, printf_fn, i32, LLVMBuildGlobalStringPtr(ctx->builder, "'%c'", "fmt"), LLVMConstInt(LLVMInt8TypeInContext(ctx->context), ast->character, 0), "");
        break;
    case AST_KEYWORD: {
        char keyword_buf[256];
        snprintf(keyword_buf, sizeof(keyword_buf), ":%s", ast->keyword);
        emit_call_1(ctx, printf_fn, i32, LLVMBuildGlobalStringPtr(ctx->builder, keyword_buf, "kw_print"), "");
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
    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

    switch (ast_elem->type) {
        case AST_NUMBER:
            if (type_is_float(infer_literal_type(ast_elem->number, ast_elem->literal_str)))
                rt_val = emit_call_1(ctx, get_rt_value_float(ctx), ptr, LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), ast_elem->number), "rtval");
            else
                rt_val = emit_call_1(ctx, get_rt_value_int(ctx), ptr, LLVMConstInt(LLVMInt64TypeInContext(ctx->context), (int64_t)ast_elem->number, 0), "rtval");
            break;
        case AST_CHAR:
            rt_val = emit_call_1(ctx, get_rt_value_char(ctx), ptr, LLVMConstInt(LLVMInt8TypeInContext(ctx->context), ast_elem->character, 0), "rtval");
            break;
        case AST_STRING:
            rt_val = emit_call_1(ctx, get_rt_value_string(ctx), ptr, LLVMBuildGlobalStringPtr(ctx->builder, ast_elem->string, "str"), "rtval");
            break;
        case AST_KEYWORD:
            rt_val = emit_call_1(ctx, get_rt_value_keyword(ctx), ptr, LLVMBuildGlobalStringPtr(ctx->builder, ast_elem->keyword, "kw"), "rtval");
            break;
        case AST_SYMBOL:
            rt_val = emit_call_1(ctx, get_rt_value_symbol(ctx), ptr, LLVMBuildGlobalStringPtr(ctx->builder, ast_elem->symbol, "sym"), "rtval");
            break;

        case AST_LIST: {
            LLVMValueRef list = emit_call_0(ctx, get_rt_list_new(ctx), ptr, "sublist");
            for (size_t i = 0; i < ast_elem->list.count; i++) {
                LLVMValueRef ev = ast_to_runtime_value(ctx, ast_elem->list.items[i]);
                if (ev) emit_call_2(ctx, get_rt_list_append(ctx), LLVMVoidTypeInContext(ctx->context), list, ev, "");
            }
            rt_val = emit_call_1(ctx, get_rt_value_list(ctx), ptr, list, "rtval");
            break;
        }
        case AST_LAMBDA: {
            LLVMTypeRef void_t = LLVMVoidTypeInContext(ctx->context);
            LLVMValueRef list = emit_call_0(ctx, get_rt_list_new(ctx), ptr, "lam_list");
            emit_call_2(ctx, get_rt_list_append(ctx), void_t, list, emit_call_1(ctx, get_rt_value_symbol(ctx), ptr, LLVMBuildGlobalStringPtr(ctx->builder, "lambda", "lsym"), ""), "");
            LLVMValueRef plist = emit_call_0(ctx, get_rt_list_new(ctx), ptr, "plist");
            for (int i = 0; i < ast_elem->lambda.param_count; i++)
                emit_call_2(ctx, get_rt_list_append(ctx), void_t, plist, emit_call_1(ctx, get_rt_value_symbol(ctx), ptr, LLVMBuildGlobalStringPtr(ctx->builder, ast_elem->lambda.params[i].name, "pname"), ""), "");
            emit_call_2(ctx, get_rt_list_append(ctx), void_t, list, emit_call_1(ctx, get_rt_value_list(ctx), ptr, plist, ""), "");
            for (int i = 0; i < ast_elem->lambda.body_count; i++)
                emit_call_2(ctx, get_rt_list_append(ctx), void_t, list, ast_to_runtime_value(ctx, ast_elem->lambda.body_exprs[i]), "");
            rt_val = emit_call_1(ctx, get_rt_value_list(ctx), ptr, list, "lam_rtval");
            break;
        }
        default:
            rt_val = emit_call_0(ctx, get_rt_value_nil(ctx), ptr, "rtval_nil");
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
    /* For non-ASCII names, try the mangled name first to get the entry
     * — the entry's func_ref still points to the real function         */
    {
        char *mangled = mangle_unicode_name(symbol_name);
        if (mangled) {
            EnvEntry *me = env_lookup(ctx->env, mangled);
            free(mangled);
            if (me) return me;
        }
    }

    // Check if it's a qualified symbol (contains '.' like "M.phi")
    const char *dot = strchr(symbol_name, '.');

    if (dot) {
        // Qualified symbol: Module.symbol
        size_t prefix_len = dot - symbol_name;
        char *module_prefix = malloc(prefix_len + 1);
        memcpy(module_prefix, symbol_name, prefix_len);
        module_prefix[prefix_len] = '\0';

        const char *local_symbol = dot + 1;

        if (!ctx->module_ctx) {
            free(module_prefix);
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: qualified symbol ‘%s’ used but no module context",
                    parser_get_filename(), ast->line, ast->column, symbol_name);
        }

        // Find the import with this prefix
        ImportDecl *import = module_context_find_import(ctx->module_ctx, module_prefix);
        if (!import) {
            char _mp_copy[256];
            strncpy(_mp_copy, module_prefix, sizeof(_mp_copy) - 1);
            _mp_copy[sizeof(_mp_copy)-1] = '\0';
            free(module_prefix);
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown module prefix ‘%s’",
                    parser_get_filename(), ast->line, ast->column, _mp_copy);
        }

        // Check if this symbol is included in the import
        if (!import_decl_includes_symbol(import, local_symbol)) {
            free(module_prefix);
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: symbol ‘%s’ not imported from module ‘%s’",
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
    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
    LLVMTypeRef void_t = LLVMVoidTypeInContext(ctx->context);
    LLVMValueRef nl_str = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "nl");

    if (type->kind == TYPE_STRING) emit_call_2(ctx, printf_fn, i32, newline ? get_fmt_str(ctx) : get_fmt_str_no_newline(ctx), val, "");
    else if (type->kind == TYPE_CHAR) emit_call_2(ctx, printf_fn, i32, newline ? get_fmt_char(ctx) : get_fmt_char_no_newline(ctx), val, "");
    else if (type->kind == TYPE_FLOAT) emit_call_2(ctx, printf_fn, i32, newline ? get_fmt_float(ctx) : get_fmt_float_no_newline(ctx), val, "");
    else if (type->kind == TYPE_HEX || type->kind == TYPE_OCT) {
        emit_call_2(ctx, printf_fn, i32, type->kind == TYPE_HEX ? get_fmt_hex(ctx) : get_fmt_oct(ctx), val, "");
        if (newline) emit_call_1(ctx, printf_fn, i32, nl_str, "");
    } else if (type->kind == TYPE_BIN) {
        emit_call_1(ctx, get_or_declare_print_binary(ctx), LLVMInt64TypeInContext(ctx->context), val, "");
        if (newline) emit_call_1(ctx, printf_fn, i32, nl_str, "");
    } else if (type_is_integer(type) && !type_is_unsigned(type)) {
        LLVMValueRef ext = LLVMTypeOf(val) != LLVMInt64TypeInContext(ctx->context) ? LLVMBuildSExt(ctx->builder, val, LLVMInt64TypeInContext(ctx->context), "ext") : val;
        emit_call_2(ctx, printf_fn, i32, newline ? get_fmt_int(ctx) : get_fmt_int_no_newline(ctx), ext, "");
    } else if (type_is_unsigned(type)) {
        LLVMValueRef ext = LLVMTypeOf(val) != LLVMInt64TypeInContext(ctx->context) ? LLVMBuildZExt(ctx->builder, val, LLVMInt64TypeInContext(ctx->context), "ext") : val;
        emit_call_2(ctx, printf_fn, i32, LLVMBuildGlobalStringPtr(ctx->builder, newline ? "%lu\n" : "%lu", "fmt_uint"), ext, "");
    } else if (type->kind == TYPE_F32) {
        emit_call_2(ctx, printf_fn, i32, newline ? get_fmt_float(ctx) : get_fmt_float_no_newline(ctx), LLVMBuildFPExt(ctx->builder, val, LLVMDoubleTypeInContext(ctx->context), "fext"), "");
    } else if (type->kind == TYPE_LIST) {
        emit_call_1(ctx, get_rt_print_list(ctx), void_t, val, "");
    } else if (type->kind == TYPE_RATIO || type->kind == TYPE_SYMBOL) {
        emit_call_1(ctx, get_rt_print_value(ctx), void_t, val, "");
        if (newline) emit_call_1(ctx, printf_fn, i32, nl_str, "");
    } else if (type->kind == TYPE_KEYWORD) {
        emit_call_1(ctx, printf_fn, i32, LLVMBuildGlobalStringPtr(ctx->builder, ":", "kw_colon"), "");
        emit_call_2(ctx, printf_fn, i32, newline ? get_fmt_str(ctx) : get_fmt_str_no_newline(ctx), val, "");
    } else if (type->kind == TYPE_BOOL) {
        LLVMValueRef bs = LLVMBuildSelect(ctx->builder, val, LLVMBuildGlobalStringPtr(ctx->builder, "True", "t"), LLVMBuildGlobalStringPtr(ctx->builder, "False", "f"), "bs");
        emit_call_2(ctx, printf_fn, i32, newline ? get_fmt_str(ctx) : get_fmt_str_no_newline(ctx), bs, "");
    } else if (type->kind == TYPE_COLL) {
        emit_call_1(ctx, get_rt_print_value(ctx), void_t, val, "");
        if (newline) emit_call_1(ctx, printf_fn, i32, nl_str, "");
    } else if (type->kind == TYPE_UNKNOWN) {
        emit_call_1(ctx, get_rt_print_value_newline(ctx), void_t, val, "");
    } else if (type->kind == TYPE_PTR) {
        emit_call_2(ctx, printf_fn, i32, LLVMBuildGlobalStringPtr(ctx->builder, newline ? "%p\n" : "%p", "fmt_ptr"), val, "");
    } else if (type->kind == TYPE_LAYOUT) {
        /* Check if this is an ADT type by looking for constructors */
        /* Collect all constructors for this type */
        typedef struct { const char *name; int tag; int nfields; } CtorInfo;
        CtorInfo ctors[64];
        int nctors = 0;
        for (size_t _bi = 0; _bi < ctx->env->size && nctors < 64; _bi++) {
            for (EnvEntry *_e = ctx->env->buckets[_bi]; _e; _e = _e->next) {
                if (_e->kind == ENV_ADT_CTOR && _e->type &&
                    _e->type->kind == TYPE_LAYOUT &&
                    strcmp(_e->type->layout_name, type->layout_name) == 0) {
                    ctors[nctors].name    = _e->name;
                    ctors[nctors].tag     = _e->adt_tag;
                    ctors[nctors].nfields = _e->param_count;
                    nctors++;
                }
            }
        }
        if (nctors > 0) {
            /* Print: TypeName :: CtorName (field0 field1 ...) */
            LLVMTypeRef i32  = LLVMInt32TypeInContext(ctx->context);
            LLVMTypeRef i64  = LLVMInt64TypeInContext(ctx->context);
            LLVMTypeRef dbl  = LLVMDoubleTypeInContext(ctx->context);

            /* Get the struct LLVM type */
            char sname[256];
            snprintf(sname, sizeof(sname), "data.%s", type->layout_name);
            LLVMTypeRef struct_t = LLVMGetTypeByName2(ctx->context, sname);

            /* Print "TypeName :: [" prefix */
            char prefix[256];
            snprintf(prefix, sizeof(prefix), "%s :: [", type->layout_name);
            LLVMValueRef pfx_str = LLVMBuildGlobalStringPtr(ctx->builder, prefix, "adt_pfx");
            LLVMValueRef pfx_args[] = {pfx_str};
            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                           printf_fn, pfx_args, 1, "");

            /* Read the tag from offset 0 */
            LLVMTypeRef  i32ptr  = LLVMPointerType(i32, 0);
            LLVMValueRef tag_ptr = LLVMBuildBitCast(ctx->builder, val, i32ptr, "tag_ptr");
            LLVMValueRef tag_val = LLVMBuildLoad2(ctx->builder, i32, tag_ptr, "tag_val");
            LLVMValueRef tag_i64 = LLVMBuildSExt(ctx->builder, tag_val, i64, "tag_i64");

            /* Sort ctors by tag so we can use a switch */
            /* Simple: emit if-chain comparing tag to each known tag */
            LLVMValueRef cur_fn  = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
            LLVMBasicBlockRef after_bb = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "adt_after");

            for (int ci = 0; ci < nctors; ci++) {
                LLVMBasicBlockRef match_bb = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "adt_match");
                LLVMBasicBlockRef next_bb  = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "adt_next");

                LLVMValueRef expected = LLVMConstInt(i64, (uint64_t)ctors[ci].tag, 0);
                LLVMValueRef cmp = LLVMBuildICmp(ctx->builder, LLVMIntEQ, tag_i64, expected, "tag_cmp");
                LLVMBuildCondBr(ctx->builder, cmp, match_bb, next_bb);

                /* match_bb: print constructor name and fields */
                LLVMPositionBuilderAtEnd(ctx->builder, match_bb);

                /* Print constructor name */
                LLVMValueRef cname_str = LLVMBuildGlobalStringPtr(ctx->builder,
                                             ctors[ci].name, "ctor_name");
                LLVMValueRef cn_args[] = {cname_str};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                               printf_fn, cn_args, 1, "");

                if (ctors[ci].nfields > 0 && struct_t) {
                    /* Print " (" then each field then ")" */
                    LLVMValueRef lp = LLVMBuildGlobalStringPtr(ctx->builder, " (", "lp");
                    LLVMValueRef lp_a[] = {lp};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, lp_a, 1, "");

                    for (int fi = 0; fi < ctors[ci].nfields; fi++) {
                        if (fi > 0) {
                            LLVMValueRef sp = LLVMBuildGlobalStringPtr(ctx->builder, " ", "sp");
                            LLVMValueRef sp_a[] = {sp};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                           printf_fn, sp_a, 1, "");
                        }
                        /* Field is at struct index fi+1 (index 0 is the tag) */
                        LLVMValueRef zero  = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                        LLVMValueRef fidx  = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), fi + 1, 0);
                        LLVMValueRef idxs[] = {zero, fidx};
                        LLVMValueRef fgep  = LLVMBuildGEP2(ctx->builder, struct_t,
                                                            val, idxs, 2, "fgep");
                        /* Fields are stored as i64 in the struct — load and reinterpret */
                        LLVMValueRef fval  = LLVMBuildLoad2(ctx->builder, i64, fgep, "fval");
                        /* Reinterpret as double for Float fields */
                        LLVMValueRef fdbl  = LLVMBuildBitCast(ctx->builder, fval, dbl, "fdbl");
                        LLVMValueRef fmt_f = get_fmt_float_no_newline(ctx);
                        LLVMValueRef fa[]  = {fmt_f, fdbl};
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                       printf_fn, fa, 2, "");
                    }

                    LLVMValueRef rp = LLVMBuildGlobalStringPtr(ctx->builder, ")", "rp");
                    LLVMValueRef rp_a[] = {rp};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                                   printf_fn, rp_a, 1, "");
                }

                LLVMValueRef rb = LLVMBuildGlobalStringPtr(ctx->builder, "]", "rb");
                LLVMValueRef rb_a[] = {rb};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                               printf_fn, rb_a, 1, "");
                LLVMBuildBr(ctx->builder, after_bb);
                LLVMPositionBuilderAtEnd(ctx->builder, next_bb);
            }
            /* Fallthrough for unknown tag */
            LLVMBuildBr(ctx->builder, after_bb);
            LLVMPositionBuilderAtEnd(ctx->builder, after_bb);

            if (newline) {
                LLVMValueRef nl = LLVMBuildGlobalStringPtr(ctx->builder, "\n", "nl");
                LLVMValueRef nl_a[] = {nl};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                               printf_fn, nl_a, 1, "");
            }
        } else {
            /* Plain layout (FFI struct) — just print pointer address */
            LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->builder,
                newline ? "<layout %p>\n" : "<layout %p>", "lay_fmt");
            LLVMValueRef args[] = {fmt, val};
            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                           printf_fn, args, 2, "");
        }
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

static LLVMValueRef emit_call_0(CodegenContext *ctx, LLVMValueRef fn, LLVMTypeRef ret_t, const char *name) {
    return LLVMBuildCall2(ctx->builder, LLVMFunctionType(ret_t, NULL, 0, 0), fn, NULL, 0, name);
}

static LLVMValueRef emit_call_1(CodegenContext *ctx, LLVMValueRef fn, LLVMTypeRef ret_t, LLVMValueRef a1, const char *name) {
    LLVMTypeRef t1 = LLVMTypeOf(a1);
    return LLVMBuildCall2(ctx->builder, LLVMFunctionType(ret_t, &t1, 1, 0), fn, &a1, 1, name);
}

static LLVMValueRef emit_call_2(CodegenContext *ctx, LLVMValueRef fn, LLVMTypeRef ret_t, LLVMValueRef a1, LLVMValueRef a2, const char *name) {
    LLVMTypeRef pt[] = {LLVMTypeOf(a1), LLVMTypeOf(a2)};
    LLVMValueRef pa[] = {a1, a2};
    return LLVMBuildCall2(ctx->builder, LLVMFunctionType(ret_t, pt, 2, 0), fn, pa, 2, name);
}

static LLVMValueRef emit_call_3(CodegenContext *ctx, LLVMValueRef fn, LLVMTypeRef ret_t, LLVMValueRef a1, LLVMValueRef a2, LLVMValueRef a3, const char *name) {
    LLVMTypeRef pt[] = {LLVMTypeOf(a1), LLVMTypeOf(a2), LLVMTypeOf(a3)};
    LLVMValueRef pa[] = {a1, a2, a3};
    return LLVMBuildCall2(ctx->builder, LLVMFunctionType(ret_t, pt, 3, 0), fn, pa, 3, name);
}

static LLVMValueRef codegen_box(CodegenContext *ctx, LLVMValueRef val, Type *type) {
    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);

    if (!type) {
        // already a pointer — assume boxed
        return val;
    }
    switch (type->kind) {
        case TYPE_INT: case TYPE_HEX: case TYPE_BIN: case TYPE_OCT:
            return emit_call_1(ctx, get_rt_value_int(ctx), ptr,
                LLVMTypeOf(val) != i64 ? LLVMBuildSExtOrBitCast(ctx->builder, val, i64, "box_int") : val, "boxed_int");
        case TYPE_FLOAT:
            return emit_call_1(ctx, get_rt_value_float(ctx), ptr, val, "boxed_float");
        case TYPE_CHAR:
            return emit_call_1(ctx, get_rt_value_char(ctx), ptr, val, "boxed_char");
        case TYPE_STRING:
            return emit_call_1(ctx, get_rt_value_string(ctx), ptr, val, "boxed_str");
        case TYPE_BOOL:
            return emit_call_1(ctx, get_rt_value_int(ctx), ptr,
                LLVMBuildZExt(ctx->builder, val, i64, "bool_to_int"), "boxed_bool");
        case TYPE_ARR: {
            LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
            LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);

            LLVMValueRef len = arr_fat_size(ctx, val);

            // Allocate the RuntimeValue* Array wrapper
            LLVMValueRef alloc_fn = LLVMGetNamedFunction(ctx->module, "rt_value_array");
            if (!alloc_fn) {
                LLVMTypeRef ft = LLVMFunctionType(ptr_t, &i64_t, 1, 0);
                alloc_fn = LLVMAddFunction(ctx->module, "rt_value_array", ft);
            }
            LLVMValueRef boxed_arr = LLVMBuildCall2(ctx->builder, LLVMFunctionType(ptr_t, &i64_t, 1, 0), alloc_fn, &len, 1, "boxed_arr");

            // Loop and copy elements into the boxed array
            LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(ctx->builder);
            LLVMValueRef cur_fn = LLVMGetBasicBlockParent(cur_bb);
            LLVMBasicBlockRef loop_bb = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "box_arr_loop");
            LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "box_arr_body");
            LLVMBasicBlockRef exit_bb = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "box_arr_exit");

            LLVMBuildBr(ctx->builder, loop_bb);

            LLVMPositionBuilderAtEnd(ctx->builder, loop_bb);
            LLVMValueRef phi_i = LLVMBuildPhi(ctx->builder, i64_t, "i");
            LLVMValueRef start_val = LLVMConstInt(i64_t, 0, 0);
            LLVMAddIncoming(phi_i, &start_val, &cur_bb, 1);
            LLVMValueRef cond = LLVMBuildICmp(ctx->builder, LLVMIntSLT, phi_i, len, "cond");
            LLVMBuildCondBr(ctx->builder, cond, body_bb, exit_bb);

            LLVMPositionBuilderAtEnd(ctx->builder, body_bb);
            LLVMValueRef data = arr_fat_data(ctx, val, type->arr_element_type);
            LLVMTypeRef elem_llvm = type_to_llvm(ctx, type->arr_element_type);
            LLVMValueRef ep = LLVMBuildGEP2(ctx->builder, elem_llvm, data, &phi_i, 1, "ep");
            LLVMValueRef ev = LLVMBuildLoad2(ctx->builder, elem_llvm, ep, "ev");

            // Recursively box the inner element
            LLVMValueRef boxed_ev = codegen_box(ctx, ev, type->arr_element_type);

            LLVMValueRef set_fn = LLVMGetNamedFunction(ctx->module, "rt_array_set");
            if (!set_fn) {
                LLVMTypeRef set_args[] = {ptr_t, i64_t, ptr_t};
                set_fn = LLVMAddFunction(ctx->module, "rt_array_set", LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), set_args, 3, 0));
            }
            LLVMValueRef set_args[] = {boxed_arr, phi_i, boxed_ev};
            LLVMBuildCall2(ctx->builder, LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), (LLVMTypeRef[]){ptr_t, i64_t, ptr_t}, 3, 0), set_fn, set_args, 3, "");

            LLVMValueRef one_val = LLVMConstInt(i64_t, 1, 0);
            LLVMValueRef next_i = LLVMBuildAdd(ctx->builder, phi_i, one_val, "next_i");
            LLVMBasicBlockRef body_end = LLVMGetInsertBlock(ctx->builder);
            LLVMAddIncoming(phi_i, &next_i, &body_end, 1);
            LLVMBuildBr(ctx->builder, loop_bb);

            LLVMPositionBuilderAtEnd(ctx->builder, exit_bb);
            return boxed_arr;
        }
    case TYPE_LIST:
            // If this is already a RuntimeValue* (e.g. function parameter or
            // call result), pass it through — do NOT wrap again with rt_value_list.
            // Only raw RuntimeList* values (from rt_list_new etc.) need wrapping.
            // We detect already-boxed values by checking if the LLVM type is ptr
            // AND the value came from a load or call (not from rt_list_new/empty).
            // The safest rule: if type kind is TYPE_LIST but the value is already
            // a tagged RuntimeValue* pointer, return as-is. Since we cannot
            // distinguish at this level, we return val directly — callers that
            // have a raw RuntimeList* must box it explicitly before calling codegen_box.
            return val;
        case TYPE_SYMBOL:
            return emit_call_1(ctx, get_rt_value_symbol(ctx), ptr, val, "boxed_sym");
        case TYPE_KEYWORD:
            return emit_call_1(ctx, get_rt_value_keyword(ctx), ptr, val, "boxed_kw");
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

static void emit_runtime_error_val(CodegenContext *ctx, AST *ast, LLVMValueRef msg_val) {
    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);
    LLVMValueRef rterr_fn = get___monad_runtime_error(ctx);
    LLVMTypeRef rte_ft = LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), (LLVMTypeRef[]){ptr_t, i64_t, i64_t, ptr_t}, 4, 0);
    LLVMValueRef file_str = LLVMBuildGlobalStringPtr(ctx->builder, parser_get_filename(), "err_file");
    LLVMValueRef rte_args[] = { file_str, LLVMConstInt(i64_t, ast ? ast->line : 0, 0), LLVMConstInt(i64_t, ast ? ast->column : 0, 0), msg_val };
    LLVMBuildCall2(ctx->builder, rte_ft, rterr_fn, rte_args, 4, "");
    LLVMBuildUnreachable(ctx->builder);
}

static void emit_runtime_error(CodegenContext *ctx, AST *ast, const char *msg) {
    emit_runtime_error_val(ctx, ast, LLVMBuildGlobalStringPtr(ctx->builder, msg, "err_msg"));
}

static LLVMValueRef codegen_arity_wrapper(CodegenContext *ctx, LLVMValueRef *out_closure_env, int arity) {
    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
    LLVMTypeRef wrapper_params[] = {ptr, i32, ptr};
    LLVMTypeRef wrapper_ft = LLVMFunctionType(ptr, wrapper_params, 3, 0);

    static int wrapper_count = 0;
    char wname[64];
    snprintf(wname, sizeof(wname), "__wrapper_arity_%d_%d", arity, wrapper_count++);

    LLVMValueRef      wrapper = LLVMAddFunction(ctx->module, wname, wrapper_ft);
    LLVMBasicBlockRef entry   = LLVMAppendBasicBlockInContext(ctx->context, wrapper, "entry");
    LLVMBasicBlockRef saved   = LLVMGetInsertBlock(ctx->builder);
    LLVMPositionBuilderAtEnd(ctx->builder, entry);

    LLVMValueRef env_param  = LLVMGetParam(wrapper, 0);
    LLVMValueRef args_array = LLVMGetParam(wrapper, 2);

    LLVMValueRef call_fn  = get_rt_closure_calln(ctx);
    LLVMTypeRef  ft       = LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr, i32, ptr}, 3, 0);
    LLVMValueRef cargs[]  = {env_param, LLVMConstInt(i32, arity, 0), args_array};
    LLVMValueRef res      = LLVMBuildCall2(ctx->builder, ft, call_fn, cargs, 3, "res");

    LLVMBuildRet(ctx->builder, res);

    if (saved) LLVMPositionBuilderAtEnd(ctx->builder, saved);
    *out_closure_env = LLVMConstPointerNull(ptr);
    return wrapper;
}

static LLVMValueRef codegen_unary_wrapper(CodegenContext *ctx, LLVMValueRef *out_closure_env) {
    return codegen_arity_wrapper(ctx, out_closure_env, 1);
}

static LLVMValueRef codegen_binary_wrapper(CodegenContext *ctx, LLVMValueRef *out_closure_env) {
    return codegen_arity_wrapper(ctx, out_closure_env, 2);
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

void codegen_data(CodegenContext *ctx, AST *ast) {
    // Each data type becomes a tagged union:
    // struct { i32 tag; [max-payload fields as i64] }
    // Constructors are LLVM functions that fill and heap-allocate the struct.

    const char *type_name = ast->data.name;
    int nctors = ast->data.constructor_count;

    // 1. Find max payload field count across all constructors
    int max_fields = 0;
    for (int i = 0; i < nctors; i++) {
        if (ast->data.constructors[i].field_count > max_fields)
            max_fields = ast->data.constructors[i].field_count;
    }

    // 2. Build LLVM struct type: { i32 tag, i64 field0, i64 field1, ... }
    int struct_field_count = 1 + max_fields;
    LLVMTypeRef *field_types = malloc(sizeof(LLVMTypeRef) * struct_field_count);
    field_types[0] = LLVMInt32TypeInContext(ctx->context); // tag
    for (int i = 1; i < struct_field_count; i++)
        field_types[i] = LLVMInt64TypeInContext(ctx->context); // payload slots

    char struct_name[256];
    snprintf(struct_name, sizeof(struct_name), "data.%s", type_name);

    LLVMTypeRef existing = LLVMGetTypeByName2(ctx->context, struct_name);
    LLVMTypeRef struct_t;
    if (existing) {
        struct_t = existing;
    } else {
        struct_t = LLVMStructCreateNamed(ctx->context, struct_name);
        LLVMStructSetBody(struct_t, field_types, struct_field_count, 0);
    }
    free(field_types);

    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef i64   = LLVMInt64TypeInContext(ctx->context);
    LLVMTypeRef i32   = LLVMInt32TypeInContext(ctx->context);

    // Register a layout type for the data type so env knows its shape
    LayoutField *lay_fields = malloc(sizeof(LayoutField) * struct_field_count);
    lay_fields[0].name   = strdup("__tag");
    lay_fields[0].type   = type_i32();
    lay_fields[0].size   = 4;
    lay_fields[0].offset = 0;
    for (int i = 1; i < struct_field_count; i++) {
        char fname[32];
        snprintf(fname, sizeof(fname), "__field%d", i - 1);
        lay_fields[i].name   = strdup(fname);
        lay_fields[i].type   = type_int();
        lay_fields[i].size   = 8;
        lay_fields[i].offset = 4 + (i - 1) * 8;
    }
    int total_size = 4 + max_fields * 8;
    Type *data_layout_type = type_layout(type_name, lay_fields, struct_field_count,
                                          total_size, false, 0);
    env_insert_layout(ctx->env, type_name, data_layout_type, NULL);

    // 3. For each constructor, emit a function
    for (int ci = 0; ci < nctors; ci++) {
        ASTDataConstructor *ctor = &ast->data.constructors[ci];
        int nfields = ctor->field_count;

        // Build param types from field type names
        LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * (nfields ? nfields : 1));
        EnvParam    *env_params  = malloc(sizeof(EnvParam)    * (nfields ? nfields : 1));
        Type       **field_type_objs = malloc(sizeof(Type*)   * (nfields ? nfields : 1));

        for (int fi = 0; fi < nfields; fi++) {
            Type *ft = type_from_name(ctor->field_types[fi]);
            if (!ft || ft->kind == TYPE_UNKNOWN) ft = type_int();
            field_type_objs[fi]  = ft;
            param_types[fi]      = type_to_llvm(ctx, ft);
            char pname[32];
            snprintf(pname, sizeof(pname), "__f%d", fi);
            env_params[fi].name = strdup(pname);
            env_params[fi].type = type_clone(ft);
        }

        // Return type is ptr (heap-allocated struct)
        LLVMTypeRef func_type = LLVMFunctionType(ptr_t, param_types, nfields, 0);

        // Check if already defined (REPL redefinition)
        LLVMValueRef func = LLVMGetNamedFunction(ctx->module, ctor->name);
        if (!func) {
            func = LLVMAddFunction(ctx->module, ctor->name, func_type);
            LLVMSetLinkage(func, LLVMExternalLinkage);
        }

        LLVMBasicBlockRef saved_block = LLVMGetInsertBlock(ctx->builder);
        LLVMBasicBlockRef entry_block = LLVMAppendBasicBlockInContext(
                                            ctx->context, func, "entry");
        LLVMPositionBuilderAtEnd(ctx->builder, entry_block);

        // malloc(sizeof(struct))
        LLVMValueRef malloc_fn = LLVMGetNamedFunction(ctx->module, "malloc");
        if (!malloc_fn) {
            LLVMTypeRef mft = LLVMFunctionType(ptr_t, &i64, 1, 0);
            malloc_fn = LLVMAddFunction(ctx->module, "malloc", mft);
            LLVMSetLinkage(malloc_fn, LLVMExternalLinkage);
        }
        LLVMValueRef size     = LLVMSizeOf(struct_t);
        LLVMValueRef heap_ptr = LLVMBuildCall2(ctx->builder,
                                    LLVMFunctionType(ptr_t, &i64, 1, 0),
                                    malloc_fn, &size, 1, "ctor_ptr");

        // Store tag (constructor index)
        LLVMValueRef zero   = LLVMConstInt(i32, 0, 0);
        LLVMValueRef tag_idx = LLVMConstInt(i32, 0, 0);
        LLVMValueRef tag_idxs[] = {zero, tag_idx};
        LLVMValueRef tag_gep = LLVMBuildGEP2(ctx->builder, struct_t,
                                              heap_ptr, tag_idxs, 2, "tag_ptr");
        LLVMBuildStore(ctx->builder,
                       LLVMConstInt(i32, ci, 0), tag_gep);

        // Store each field
        for (int fi = 0; fi < nfields; fi++) {
            LLVMValueRef fidx     = LLVMConstInt(i32, fi + 1, 0);
            LLVMValueRef fidxs[]  = {zero, fidx};
            LLVMValueRef fgep     = LLVMBuildGEP2(ctx->builder, struct_t,
                                                   heap_ptr, fidxs, 2, "fld_ptr");
            LLVMValueRef param_v  = LLVMGetParam(func, fi);
            // Coerce to i64 slot
            LLVMValueRef to_store;
            Type *ft = field_type_objs[fi];
            if (type_is_float(ft)) {
                to_store = LLVMBuildBitCast(ctx->builder, param_v, i64, "f_to_i64");
            } else if (ft->kind == TYPE_CHAR) {
                to_store = LLVMBuildZExt(ctx->builder, param_v, i64, "c_to_i64");
            } else if (ft->kind == TYPE_LAYOUT) {
                /* ADT/layout pointer — store as i64 via ptrtoint */
                to_store = LLVMBuildPtrToInt(ctx->builder, param_v, i64, "ptr_to_i64");
            } else if (type_is_integer(ft) && LLVMTypeOf(param_v) != i64) {
                to_store = LLVMBuildSExt(ctx->builder, param_v, i64, "i_to_i64");
            } else {
                to_store = param_v;
            }
            LLVMBuildStore(ctx->builder, to_store, fgep);
        }

        LLVMBuildRet(ctx->builder, heap_ptr);

        if (saved_block)
            LLVMPositionBuilderAtEnd(ctx->builder, saved_block);

        // Register in env as a function returning ptr (data type)
        Type *ret_type = type_clone(data_layout_type);
        env_insert_func(ctx->env, ctor->name,
                        clone_params(env_params, nfields),
                        nfields, ret_type, func, NULL, NULL);
        env_insert_adt_ctor(ctx->env, ctor->name, ci,
                            type_clone(data_layout_type), func);
        EnvEntry *ctor_e = env_lookup(ctx->env, ctor->name);
        if (ctor_e) {
            ctor_e->is_closure_abi = false;
            ctor_e->lifted_count   = 0;
        }

        // For nullary constructors (no fields), also register as a global variable
        // so `Red` can be used as a value directly, not just as a function call
        if (nfields == 0) {
            // Call the constructor now to get the singleton value
            LLVMValueRef gv = LLVMGetNamedGlobal(ctx->module, ctor->name);
            if (!gv) {
                gv = LLVMAddGlobal(ctx->module, ptr_t, ctor->name);
                // Can't use ctor->name — conflicts with function name.
                // Use a mangled global name instead.
                char gname[256];
                snprintf(gname, sizeof(gname), "__data_%s_%s", type_name, ctor->name);
                gv = LLVMGetNamedGlobal(ctx->module, gname);
                if (!gv) {
                    gv = LLVMAddGlobal(ctx->module, ptr_t, gname);
                    LLVMSetInitializer(gv, LLVMConstPointerNull(ptr_t));
                    LLVMSetLinkage(gv, LLVMExternalLinkage);
                }
            }
            // Initialize by calling the constructor function (zero-arg call)
            LLVMValueRef singleton = LLVMBuildCall2(ctx->builder,
                                         LLVMFunctionType(ptr_t, NULL, 0, 0),
                                         func, NULL, 0, "singleton");
            char gname[256];
            snprintf(gname, sizeof(gname), "__data_%s_%s", type_name, ctor->name);
            LLVMValueRef gvar = LLVMGetNamedGlobal(ctx->module, gname);
            if (gvar)
                LLVMBuildStore(ctx->builder, singleton, gvar);

            // Register the nullary constructor as a VAR pointing to the global
            // so `Red` resolves as a value, not just a 0-arg function call
            if (gvar) {
                env_insert_adt_ctor(ctx->env, ctor->name, ci,
                                    type_clone(data_layout_type), func);
                EnvEntry *ve = env_lookup(ctx->env, ctor->name);
                if (ve) ve->llvm_name = strdup(gname);
            }
        }

        /* Emit typed field accessor functions:
         * __field_CtorName_fi :: ptr -> FieldType
         * These replace the generic __adt_field hack.              */
        for (int fi = 0; fi < nfields; fi++) {
            char acc_name[256];
            snprintf(acc_name, sizeof(acc_name), "__field_%s_%d", ctor->name, fi);

            Type       *ft       = field_type_objs[fi];
            /* For layout/ADT fields, always return i8* ptr — avoids struct
             * type mismatches between modules. Callers cast as needed.    */
            LLVMTypeRef  ret_llvm = (ft->kind == TYPE_LAYOUT)
                                  ? ptr_t
                                  : type_to_llvm(ctx, ft);

            /* Accessor takes the raw ADT ptr and returns the field value */
            LLVMTypeRef  acc_params[] = {ptr_t};
            LLVMTypeRef  acc_ft       = LLVMFunctionType(ret_llvm, acc_params, 1, 0);
            LLVMValueRef acc_fn       = LLVMAddFunction(ctx->module, acc_name, acc_ft);
            LLVMSetLinkage(acc_fn, LLVMExternalLinkage);

            LLVMBasicBlockRef acc_saved = LLVMGetInsertBlock(ctx->builder);
            LLVMBasicBlockRef acc_entry = LLVMAppendBasicBlockInContext(
                                              ctx->context, acc_fn, "entry");
            LLVMPositionBuilderAtEnd(ctx->builder, acc_entry);

            LLVMValueRef acc_ptr = LLVMGetParam(acc_fn, 0);

            /* GEP: byte offset 8 + fi*8 (tag=i32 at 0, padding 4 bytes, fields at 8+) */
            long long byte_off = 8 + fi * 8;
            LLVMValueRef offset   = LLVMConstInt(i64, (uint64_t)byte_off, 0);
            LLVMValueRef i8base   = LLVMBuildBitCast(ctx->builder, acc_ptr,
                                        LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0),
                                        "base");
            LLVMValueRef fptr_i8  = LLVMBuildGEP2(ctx->builder,
                                        LLVMInt8TypeInContext(ctx->context),
                                        i8base, &offset, 1, "fptr");
            LLVMTypeRef  i64ptr   = LLVMPointerType(i64, 0);
            LLVMValueRef fptr64   = LLVMBuildBitCast(ctx->builder, fptr_i8, i64ptr, "fptr64");
            LLVMValueRef fval_i64 = LLVMBuildLoad2(ctx->builder, i64, fptr64, "fval");

            /* Reinterpret to correct type */
            LLVMValueRef fval;
            if (type_is_float(ft)) {
                fval = LLVMBuildBitCast(ctx->builder, fval_i64,
                                        LLVMDoubleTypeInContext(ctx->context), "fval_dbl");
            } else if (ft->kind == TYPE_LAYOUT) {
                fval = LLVMBuildIntToPtr(ctx->builder, fval_i64, ptr_t, "fval_ptr");
            } else if (ft->kind == TYPE_CHAR) {
                fval = LLVMBuildTrunc(ctx->builder, fval_i64,
                                      LLVMInt8TypeInContext(ctx->context), "fval_char");
            } else {
                fval = fval_i64;
            }

            LLVMBuildRet(ctx->builder, fval);

            if (acc_saved) LLVMPositionBuilderAtEnd(ctx->builder, acc_saved);

            /* Register accessor in env with correct type */
            EnvParam acc_eparam = {strdup("__self"), type_clone(data_layout_type)};
            env_insert_func(ctx->env, acc_name,
                            clone_params(&acc_eparam, 1), 1,
                            type_clone(ft), acc_fn, NULL, NULL);
            EnvEntry *acc_e = env_lookup(ctx->env, acc_name);
            if (acc_e) {
                acc_e->is_closure_abi = false;
                acc_e->lifted_count   = 0;
            }
            free(acc_eparam.name);
        }

        free(param_types);
        for (int fi = 0; fi < nfields; fi++) free(env_params[fi].name);
        free(env_params);
        free(field_type_objs);

        printf("Constructor: %s", ctor->name);
        for (int fi = 0; fi < nfields; fi++)
            printf(" %s", ctor->field_types[fi]);
        printf(" -> %s\n", type_name);
    }

printf("Data type: %s (%d constructors)\n", type_name, nctors);

    /* Auto-derive requested typeclasses */
    for (int di = 0; di < ast->data.deriving_count; di++) {
        const char *derive_name = ast->data.deriving[di];

        if (strcmp(derive_name, "Eq") == 0) {
            /* Check class is registered */
            TCClass *cls = tc_find_class(ctx->tc_registry, "Eq");
            if (!cls) {
                fprintf(stderr, "warning: deriving Eq for ‘%s’ but Eq class not defined\n",
                        type_name);
                continue;
            }
            /* Check instance not already defined */
            if (tc_find_instance(ctx->tc_registry, "Eq", type_name)) continue;

            /* Build instance AST synthetically:
             * For each constructor pair (A, A) => True, plus (_ = _) => False */
            int total_clauses = nctors + 1;
            char    **method_names  = malloc(sizeof(char*));
            AST     **method_bodies = malloc(sizeof(AST*));
            method_names[0] = strdup("=");

            ASTPMatchClause *clauses = malloc(sizeof(ASTPMatchClause) * total_clauses);

            for (int ci = 0; ci < nctors; ci++) {
                const char *cname = ast->data.constructors[ci].name;
                ASTPattern *pats  = malloc(sizeof(ASTPattern) * 2);
                pats[0].kind            = PAT_CONSTRUCTOR;
                pats[0].var_name        = strdup(cname);
                pats[0].ctor_fields     = NULL;
                pats[0].ctor_field_count = 0;
                pats[0].elements        = NULL;
                pats[0].element_count   = 0;
                pats[0].tail            = NULL;
                pats[1] = pats[0];
                pats[1].var_name = strdup(cname);

                clauses[ci].patterns      = pats;
                clauses[ci].pattern_count = 2;
                clauses[ci].body          = ast_new_symbol("True");
            }

            /* Wildcard clause: (_ = _) => False */
            ASTPattern *wild_pats = malloc(sizeof(ASTPattern) * 2);
            wild_pats[0].kind            = PAT_WILDCARD;
            wild_pats[0].var_name        = NULL;
            wild_pats[0].elements        = NULL;
            wild_pats[0].element_count   = 0;
            wild_pats[0].tail            = NULL;
            wild_pats[0].ctor_fields     = NULL;
            wild_pats[0].ctor_field_count = 0;
            wild_pats[1] = wild_pats[0];
            clauses[nctors].patterns      = wild_pats;
            clauses[nctors].pattern_count = 2;
            clauses[nctors].body          = ast_new_symbol("False");

            /* Desugar pmatch into a lambda */
            ASTParam *params = malloc(sizeof(ASTParam) * 2);
            params[0].name      = strdup("__p0");
            params[0].type_name = strdup(type_name);
            params[0].is_rest   = false;
            params[0].is_anon   = false;
            params[1].name      = strdup("__p1");
            params[1].type_name = strdup(type_name);
            params[1].is_rest   = false;
            params[1].is_anon   = false;

            AST *pm       = ast_new_pmatch(clauses, total_clauses);
            AST *desugared = pmatch_desugar(pm, params, 2);
            free(pm);

            AST **body_exprs = malloc(sizeof(AST*));
            body_exprs[0]    = desugared;
            method_bodies[0] = ast_new_lambda(params, 2, "Bool", NULL, NULL,
                                              false, desugared, body_exprs, 1);

            AST *inst_ast = ast_new_instance("Eq", type_name,
                                             method_names, method_bodies, 1);
            tc_register_instance(ctx->tc_registry, inst_ast, ctx);
            ast_free(inst_ast);

            printf("Derived: instance Eq %s\n", type_name);
        }
    }
}


void codegen_layout(CodegenContext *ctx, AST *ast) {
    const char *name = ast->layout.name;
    if (!name || !isupper((unsigned char)name[0])) {
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: layout name ‘%s’ must start with an uppercase letter",
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
                // Try layout registry for array elements (arrays of structs)
                elem = env_lookup_layout(ctx->env, af->array_elem);
                if (elem) elem = type_clone(elem);
            }
            if (!elem) {
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown array element type ‘%s’ in layout ‘%s’",
                              parser_get_filename(), ast->line, ast->column,
                              af->array_elem, ast->layout.name);
            }
            ft = type_arr(elem, af->array_size);
        } else {
            ft = type_from_name(af->type_name);
            if (!ft) {
                // Try layout registry (nested struct)
                ft = env_lookup_layout(ctx->env, af->type_name);
                if (ft) {
                    ft = type_clone(ft);  // clone so ownership is clear
                    if (!af->is_ptr) ft->layout_is_inline = true;
                }
            }
            if (!ft) {
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown type ‘%s’ for field ‘%s’ in layout ‘%s’",
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

    /* Exempt all compiler-generated internal names from predicate checks.
     * These include typeclass method impls (__impl_*), field accessors
     * (__field_*), ADT helpers (__adt_*), and any other __ prefix names
2     * generated by the compiler that the user never sees or names.      */
    if (strncmp(name, "__", 2) == 0) return;

    size_t len = strlen(name);
    bool ends_with_q  = (len > 0 && name[len - 1] == '?');
    bool ends_with_qi = (len > 1 && name[len - 2] == '?' && name[len - 1] == '!');
    bool returns_bool = (return_type->kind == TYPE_BOOL);

    if (returns_bool && !ends_with_q && !ends_with_qi) {
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: ‘%s’ returns Bool, making it a "
                      "predicate — predicate functions must end with '?', "
                      "rename to ‘%s?’",
                      parser_get_filename(), ast->line, ast->column,
                      name, name);
    }
    if (!returns_bool && ends_with_q && !ends_with_qi) {
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: ‘%s’ ends with '?' making it "
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

    bool is_mutation = (strcmp(callee, "set!") == 0);

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
            // Allow Mutating a local variable Silently
            return;
        }

        if (!name_is_impure(caller) && target_name) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: pure function ‘%s’ mutates "
                          "global variable ‘%s’ via ‘%s’ — global mutation is "
                          "a side effect, rename to ‘%s’ to declare it impure",
                          parser_get_filename(), ast->line, ast->column,
                          caller, target_name, callee, impure_name);
        }
        return;
    }

    if (!name_is_impure(caller)) {
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: pure function ‘%s’ calls impure "
                      "function ‘%s’ — rename to ‘%s’ to declare it impure",
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
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: ‘%s’ requires 2 arguments (set val)",
                      parser_get_filename(), ast->line, ast->column, op_name);
    }
    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    CodegenResult set_r = codegen_expr(ctx, ast->list.items[1]);
    CodegenResult val_r = codegen_expr(ctx, ast->list.items[2]);
    LLVMValueRef boxed = codegen_box(ctx, val_r.value, val_r.type);
    LLVMValueRef raw_set = set_r.value;
    if (set_r.type && set_r.type->kind == TYPE_SET)
        raw_set = emit_call_1(ctx, get_rt_unbox_set(ctx), ptr, raw_set, "rawset");

    LLVMValueRef new_set = emit_call_2(ctx, fn_getter(ctx), ptr, raw_set, boxed, op_name);
    result.value = emit_call_1(ctx, get_rt_value_set(ctx), ptr, new_set, "setval");
    result.type = type_set();
    return result;
}

static CodegenResult codegen_map_op(CodegenContext *ctx, AST *ast,
                                    LLVMValueRef (*fn_getter)(CodegenContext *),
                                    const char *op_name, int expect_args) {
    CodegenResult result = {NULL, NULL};
    if ((int)ast->list.count != expect_args + 1) {
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: ‘%s’ requires %d argument%s",
                      parser_get_filename(), ast->line, ast->column,
                      op_name, expect_args, expect_args == 1 ? "" : "s");
    }
    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    CodegenResult map_r = codegen_expr(ctx, ast->list.items[1]);
    LLVMValueRef raw_map = map_r.value;
    if (map_r.type && map_r.type->kind == TYPE_MAP)
        raw_map = emit_call_1(ctx, get_rt_unbox_map(ctx), ptr, raw_map, "rawmap");

    LLVMValueRef op_fn = fn_getter(ctx);
    if (expect_args == 1) {
        result.value = emit_call_1(ctx, op_fn, ptr, raw_map, op_name);
        result.type  = type_list(NULL);
        return result;
    }

    CodegenResult key_r = codegen_expr(ctx, ast->list.items[2]);
    LLVMValueRef bkey = codegen_box(ctx, key_r.value, key_r.type);
    if (expect_args == 2) {
        LLVMValueRef new_map = emit_call_2(ctx, op_fn, ptr, raw_map, bkey, op_name);
        result.value = emit_call_1(ctx, get_rt_value_map(ctx), ptr, new_map, "mapval");
        result.type  = type_map();
        return result;
    }

    CodegenResult val_r = codegen_expr(ctx, ast->list.items[3]);
    LLVMValueRef bval = codegen_box(ctx, val_r.value, val_r.type);
    LLVMValueRef new_map = emit_call_3(ctx, op_fn, ptr, raw_map, bkey, bval, op_name);
    result.value = emit_call_1(ctx, get_rt_value_map(ctx), ptr, new_map, "mapval");
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

    if (e->is_closure_abi) {
        /* Closure-ABI function — wrap directly without typed trampoline.
         * The function already expects (ptr env, i32 n, ptr args).     */
        LLVMValueRef fn_ptr = LLVMBuildBitCast(ctx->builder, fn_to_wrap, ptr_t, "fn_ptr");
        LLVMValueRef clo_fn = get_rt_value_closure(ctx);
        LLVMTypeRef  cp[]   = {ptr_t, ptr_t, i32, i32};
        LLVMTypeRef  cft    = LLVMFunctionType(ptr_t, cp, 4, 0);
        int declared = e->param_count - e->lifted_count;
        LLVMValueRef cargs[] = {
            fn_ptr,
            LLVMConstPointerNull(ptr_t),
            LLVMConstInt(i32, 0, 0),
            LLVMConstInt(i32, declared, 0)
        };
        return LLVMBuildCall2(ctx->builder, cft, clo_fn, cargs, 4, "closure");
    }

    if (!e->is_closure_abi) {
        /* Build a calln-ABI trampoline:
         * (ptr env, i32 n, ptr args_array) -> ptr
         * Unboxes args from args_array, calls the real typed function,
         * boxes result. This matches what rt_closure_calln expects.   */
        LLVMTypeRef  i32   = LLVMInt32TypeInContext(ctx->context);
        LLVMTypeRef  tp[]  = {ptr_t, i32, ptr_t};
        LLVMTypeRef  tft   = LLVMFunctionType(ptr_t, tp, 3, 0);

        static int tramp_count = 0;
        char tname[64];
        snprintf(tname, sizeof(tname), "__tramp_%d", tramp_count++);

        LLVMValueRef      tramp  = LLVMAddFunction(ctx->module, tname, tft);
        LLVMBasicBlockRef tentry = LLVMAppendBasicBlockInContext(
                                       ctx->context, tramp, "entry");
        LLVMBasicBlockRef tsaved = LLVMGetInsertBlock(ctx->builder);
        LLVMPositionBuilderAtEnd(ctx->builder, tentry);

        /* Unbox each argument from the args array */
        LLVMValueRef args_param  = LLVMGetParam(tramp, 2);
        LLVMValueRef *real_args  = malloc(sizeof(LLVMValueRef) * (declared ? declared : 1));
        LLVMTypeRef  *real_types = malloc(sizeof(LLVMTypeRef)  * (declared ? declared : 1));
        for (int i = 0; i < declared; i++) {
            LLVMValueRef idx   = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), i, 0);
            LLVMValueRef slot  = LLVMBuildGEP2(ctx->builder, ptr_t,
                                               args_param, &idx, 1, "arg_slot");
            LLVMValueRef boxed = LLVMBuildLoad2(ctx->builder, ptr_t, slot, "boxed");
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
            } else if (LLVMGetTypeKind(native) == LLVMIntegerTypeKind && native != LLVMInt1TypeInContext(ctx->context)) {
                LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx->context);
                if (native == i8) {
                    LLVMTypeRef uft = LLVMFunctionType(i8, &ptr_t, 1, 0);
                    real_args[i] = LLVMBuildCall2(ctx->builder, uft,
                                       get_rt_unbox_char(ctx), &boxed, 1, "ua");
                } else {
                    LLVMTypeRef uft = LLVMFunctionType(i64, &ptr_t, 1, 0);
                    LLVMValueRef unboxed64 = LLVMBuildCall2(ctx->builder, uft,
                                       get_rt_unbox_int(ctx), &boxed, 1, "ua");
                    real_args[i] = LLVMBuildTrunc(ctx->builder, unboxed64, native, "ua_trunc");
                }
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
        } else if (LLVMGetTypeKind(native_ret) == LLVMIntegerTypeKind) {
            LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx->context);
            if (native_ret == i8) {
                LLVMTypeRef  bft = LLVMFunctionType(ptr_t, &i8, 1, 0);
                boxed_ret = LLVMBuildCall2(ctx->builder, bft,
                                           get_rt_value_char(ctx), &raw, 1, "br");
            } else {
                LLVMValueRef ext = LLVMBuildSExt(ctx->builder, raw, i64, "ext");
                LLVMTypeRef  bft = LLVMFunctionType(ptr_t, &i64, 1, 0);
                boxed_ret = LLVMBuildCall2(ctx->builder, bft,
                                           get_rt_value_int(ctx), &ext, 1, "br");
            }
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

static unsigned int g_jit_pred_seq = 0;
static LLVMExecutionEngineRef g_refinement_engine = NULL;
static LLVMContextRef         g_refinement_ctx    = NULL;

static void ensure_refinement_ctx(void) {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMLinkInMCJIT();
}

/* Cache: type_name -> result for already-checked literals */
typedef struct RefinementCache {
    char *type_name;
    long long arg;
    int result;
    struct RefinementCache *next;
} RefinementCache;

static RefinementCache *g_ref_cache = NULL;

static int cache_lookup(const char *type_name, long long arg) {
    for (RefinementCache *c = g_ref_cache; c; c = c->next)
        if (strcmp(c->type_name, type_name) == 0 && c->arg == arg)
            return c->result;
    return -2; /* not found */
}

static void cache_insert(const char *type_name, long long arg, int result) {
    RefinementCache *c = malloc(sizeof(RefinementCache));
    c->type_name = strdup(type_name);
    c->arg       = arg;
    c->result    = result;
    c->next      = g_ref_cache;
    g_ref_cache  = c;
}

static int jit_eval_refinement(CodegenContext *ctx,
                                const char *type_name,
                                AST *arg_ast) {

    /* For symbol arguments, try to resolve to a compile-time constant */
    AST *resolved_ast = arg_ast;
    if (arg_ast->type == AST_SYMBOL) {
        EnvEntry *e = env_lookup(ctx->env, arg_ast->symbol);
        if (e && e->kind == ENV_VAR && e->source_ast &&
            (e->source_ast->type == AST_STRING ||
             e->source_ast->type == AST_NUMBER)) {
            resolved_ast = e->source_ast;
        }
    }
    if (resolved_ast->type != AST_NUMBER && resolved_ast->type != AST_STRING) return -1;
    arg_ast = resolved_ast;


    /* Resolve alias chain to find the RefinementEntry */
    const char *name = type_name;
    char buf[256];
    RefinementEntry *found = NULL;
    for (int depth = 0; depth < 32 && !found; depth++) {
        for (RefinementEntry *e = g_refinements; e; e = e->next) {
            if (strcmp(e->name, name) == 0) { found = e; break; }
        }

        if (found) break;
        bool stepped = false;
        for (TypeAlias *a = g_aliases; a; a = a->next) {
            if (strcmp(a->alias_name, name) == 0) {
                strncpy(buf, a->target_name, sizeof(buf)-1);
                buf[sizeof(buf)-1] = '\0';
                name = buf; stepped = true; break;
            }
        }
        if (!stepped) break;
    }
    if (!found || !found->predicate_ast || !found->var) return -1;

    Type *base_t = type_from_name(found->base_type);
    bool  is_str = base_t && base_t->kind == TYPE_STRING;

    /* Each check gets a completely fresh context+engine+module.
     * This avoids all cross-module symbol resolution issues.    */
    ensure_refinement_ctx();

    LLVMContextRef eval_ctx = LLVMContextCreate();
    char wrap_mod_name[64];
    snprintf(wrap_mod_name, sizeof(wrap_mod_name), "__ref_eval_%u", g_jit_pred_seq);
    LLVMModuleRef  tmp_mod  = LLVMModuleCreateWithNameInContext(
                                  wrap_mod_name, eval_ctx);
    LLVMSetTarget(tmp_mod, LLVMGetDefaultTargetTriple());

    char wrap_name[64];
    snprintf(wrap_name, sizeof(wrap_name), "__ref_wrap_%u", g_jit_pred_seq++);

    LLVMBuilderRef tmp_bld = LLVMCreateBuilderInContext(eval_ctx);

    CodegenContext jit_ctx;
    memset(&jit_ctx, 0, sizeof(jit_ctx));
    jit_ctx.module  = tmp_mod;
    jit_ctx.builder = tmp_bld;
    jit_ctx.context = eval_ctx;
    jit_ctx.env     = env_create_child(ctx->env);
    jit_ctx.init_fn = NULL;
    jit_ctx.tc_registry = ctx->tc_registry;

    /* Declare runtime functions */
    declare_runtime_functions(&jit_ctx);

    /* Pre-declare user-defined predicate functions (Bool-returning) as externs.
     * Only Bool/integer returning functions are needed in the JIT predicate module. */
    for (size_t bi = 0; bi < ctx->env->size; bi++) {
        for (EnvEntry *e = ctx->env->buckets[bi]; e; e = e->next) {
            if (e->kind != ENV_FUNC || !e->func_ref) continue;
            if (!e->return_type) continue;
            /* Only include Bool-returning functions (predicates).
             * Exclude Int/Float returning functions like constructors
             * which generate complex IR incompatible with the JIT module. */
            if (e->return_type->kind != TYPE_BOOL) continue;
            const char *fname = e->name;
            if (LLVMGetNamedFunction(tmp_mod, fname)) continue;
            int np = e->param_count;
            LLVMTypeRef *pt = malloc(sizeof(LLVMTypeRef) * (np ? np : 1));
            for (int pi = 0; pi < np; pi++)
                pt[pi] = type_to_llvm(&jit_ctx, e->params[pi].type);
            LLVMTypeRef ft = LLVMFunctionType(
                type_to_llvm(&jit_ctx, e->return_type), pt, np, 0);
            free(pt);
            LLVMValueRef decl = LLVMAddFunction(tmp_mod, fname, ft);
            LLVMSetLinkage(decl, LLVMExternalLinkage);
            /* Register in jit_ctx.env so codegen_expr finds it */
            env_insert_func(jit_ctx.env, fname,
                            clone_params(e->params, np), np,
                            type_clone(e->return_type), decl, NULL, NULL);
            EnvEntry *je = env_lookup(jit_ctx.env, fname);
            if (je) {
                je->is_closure_abi = e->is_closure_abi;
                je->lifted_count   = e->lifted_count;
                je->source_ast     = e->source_ast;
            }
        }
    }

    /* Compile all refinement predicates directly into this module,
     * but only if they haven't already been compiled into a previous
     * JIT module — duplicate definitions cause MCJIT to fail.       */
    for (RefinementEntry *re = g_refinements; re; re = re->next) {
        if (!re->predicate_ast || !re->var) continue;
        char sub_name[256];
        snprintf(sub_name, sizeof(sub_name), "%s?", re->name);
        if (LLVMGetNamedFunction(tmp_mod, sub_name)) continue;


        /* Always recompile into this module — cross-module symbol resolution
         * is unreliable with MCJIT's LLVMAddModule. Each module must be
         * self-contained with all predicates it depends on defined inline.
         *
         * Also recompile any user-defined functions referenced by this predicate. */
        {
            /* Walk the predicate AST and recompile any user ENV_FUNC entries
             * that are not refinement predicates and not yet in tmp_mod.     */
            char **refs = NULL; int ref_count = 0;
            collect_free_vars(re->predicate_ast, NULL, 0, NULL, 0,
                              &refs, &ref_count, ctx->env);
            for (int ri = 0; ri < ref_count; ri++) {
                EnvEntry *ue = env_lookup(ctx->env, refs[ri]);
                free(refs[ri]);
                if (!ue || ue->kind != ENV_FUNC || !ue->source_ast) continue;
                if (LLVMGetNamedFunction(tmp_mod, refs[ri])) continue;
                /* Recompile this function into the JIT module */
                AST *src = ue->source_ast;
                AST *lam = NULL;
                if (src->type == AST_LAMBDA) lam = src;
                else if (src->type == AST_LIST && src->list.count >= 3 &&
                         src->list.items[2]->type == AST_LAMBDA)
                    lam = src->list.items[2];
                if (!lam) continue;
                int np = ue->param_count;
                LLVMTypeRef *pt = malloc(sizeof(LLVMTypeRef) * (np ? np : 1));
                for (int pi = 0; pi < np; pi++)
                    pt[pi] = type_to_llvm(&jit_ctx, ue->params[pi].type);
                LLVMTypeRef rft = LLVMFunctionType(
                    type_to_llvm(&jit_ctx, ue->return_type), pt, np, 0);
                free(pt);
                LLVMValueRef ufn = LLVMAddFunction(tmp_mod, refs[ri], rft);
                LLVMBasicBlockRef ubb = LLVMAppendBasicBlockInContext(
                    eval_ctx, ufn, "entry");
                LLVMBasicBlockRef usaved = LLVMGetInsertBlock(tmp_bld);
                LLVMPositionBuilderAtEnd(tmp_bld, ubb);
                Env *uenv = env_create_child(jit_ctx.env);
                for (int pi = 0; pi < np; pi++) {
                    LLVMValueRef up = LLVMGetParam(ufn, pi);
                    LLVMTypeRef  ut = type_to_llvm(&jit_ctx, ue->params[pi].type);
                    LLVMValueRef ua = LLVMBuildAlloca(tmp_bld, ut, ue->params[pi].name);
                    LLVMBuildStore(tmp_bld, up, ua);
                    env_insert(uenv, ue->params[pi].name,
                               type_clone(ue->params[pi].type), ua);
                }
                Env *prev_env = jit_ctx.env;
                jit_ctx.env = uenv;
                CodegenResult ur = {NULL, NULL};
                for (int bi2 = 0; bi2 < lam->lambda.body_count; bi2++)
                    ur = codegen_expr(&jit_ctx, lam->lambda.body_exprs[bi2]);
                jit_ctx.env = prev_env;
                env_free(uenv);
                if (ur.value) {
                    LLVMTypeRef ret_llvm = type_to_llvm(&jit_ctx, ue->return_type);
                    LLVMValueRef rv = ur.value;
                    if (LLVMTypeOf(rv) != ret_llvm) {
                        if (LLVMGetTypeKind(ret_llvm) == LLVMIntegerTypeKind)
                            rv = LLVMBuildIntCast2(tmp_bld, rv, ret_llvm, 1, "rc");
                    }
                    LLVMBuildRet(tmp_bld, rv);
                } else {
                    LLVMBuildRet(tmp_bld,
                        LLVMConstNull(type_to_llvm(&jit_ctx, ue->return_type)));
                }
                if (usaved) LLVMPositionBuilderAtEnd(tmp_bld, usaved);
            }
            free(refs);
        }


        Type *sub_base = type_from_name(re->base_type);
        bool  sub_str  = sub_base && sub_base->kind == TYPE_STRING;
        LLVMTypeRef sub_arg_t = sub_str
            ? LLVMPointerType(LLVMInt8TypeInContext(eval_ctx), 0)
            : LLVMInt64TypeInContext(eval_ctx);
        LLVMTypeRef sub_i1 = LLVMInt1TypeInContext(eval_ctx);
        LLVMTypeRef sub_ft = LLVMFunctionType(sub_i1, &sub_arg_t, 1, 0);
        LLVMValueRef sub_fn = LLVMAddFunction(tmp_mod, sub_name, sub_ft);
        LLVMBasicBlockRef sub_bb = LLVMAppendBasicBlockInContext(
            eval_ctx, sub_fn, "entry");

        LLVMBasicBlockRef saved_insert = LLVMGetInsertBlock(tmp_bld);
        LLVMPositionBuilderAtEnd(tmp_bld, sub_bb);
        jit_ctx.init_fn = sub_fn;

        LLVMValueRef sub_param  = LLVMGetParam(sub_fn, 0);
        LLVMValueRef sub_alloca = LLVMBuildAlloca(tmp_bld, sub_arg_t, re->var);
        LLVMBuildStore(tmp_bld, sub_param, sub_alloca);
        Type *sub_pt = sub_str ? type_string() : type_int();
        env_insert(jit_ctx.env, re->var, sub_pt, sub_alloca);

        CodegenResult sub_r = codegen_expr(&jit_ctx, re->predicate_ast);
        env_remove(jit_ctx.env, re->var);

        if (sub_r.value) {
            LLVMValueRef sub_ret = sub_r.value;
            if (LLVMTypeOf(sub_ret) != sub_i1 &&
                LLVMGetTypeKind(LLVMTypeOf(sub_ret)) == LLVMIntegerTypeKind)
                sub_ret = LLVMBuildTrunc(tmp_bld, sub_ret, sub_i1, "to_i1");
            LLVMBuildRet(tmp_bld, sub_ret);
        } else {
            LLVMBuildRet(tmp_bld, LLVMConstInt(sub_i1, 1, 0));
        }

        if (saved_insert)
            LLVMPositionBuilderAtEnd(tmp_bld, saved_insert);
    }

    /* Build the predicate wrapper function */
    LLVMTypeRef arg_llvm = is_str
        ? LLVMPointerType(LLVMInt8TypeInContext(eval_ctx), 0)
        : LLVMInt64TypeInContext(eval_ctx);
    LLVMTypeRef i1      = LLVMInt1TypeInContext(eval_ctx);
    LLVMTypeRef fn_type = LLVMFunctionType(i1, &arg_llvm, 1, 0);
    LLVMValueRef fn     = LLVMAddFunction(tmp_mod, wrap_name, fn_type);
    LLVMBasicBlockRef bb = LLVMAppendBasicBlockInContext(
                               eval_ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(tmp_bld, bb);

    LLVMPositionBuilderAtEnd(tmp_bld, bb);
    jit_ctx.init_fn = fn;

    /* Bind parameter */
    LLVMValueRef param  = LLVMGetParam(fn, 0);
    LLVMValueRef alloca = LLVMBuildAlloca(tmp_bld, arg_llvm, found->var);
    LLVMBuildStore(tmp_bld, param, alloca);
    Type *param_type = is_str ? type_string() : type_int();
    env_insert(jit_ctx.env, found->var, param_type, alloca);

    /* Codegen the predicate body */
    CodegenResult pred_r = codegen_expr(&jit_ctx, found->predicate_ast);

    env_remove(jit_ctx.env, found->var);

    int result = -1;

    if (pred_r.value) {
        LLVMValueRef ret = pred_r.value;
        if (LLVMTypeOf(ret) != i1) {
            if (LLVMGetTypeKind(LLVMTypeOf(ret)) == LLVMIntegerTypeKind)
                ret = LLVMBuildTrunc(tmp_bld, ret, i1, "to_i1");
            else goto cleanup;
        }
        LLVMBuildRet(tmp_bld, ret);

        /* Now resolve any unresolved user-defined functions that were
         * added as extern declarations during predicate/wrapper codegen */
        {
            bool changed = true;
            while (changed) {
                changed = false;
                LLVMValueRef f = LLVMGetFirstFunction(tmp_mod);
                while (f) {
                    LLVMValueRef next = LLVMGetNextFunction(f);
                    if (LLVMCountBasicBlocks(f) == 0) {
                        const char *fname = LLVMGetValueName(f);
                        if (strncmp(fname, "rt_", 3) != 0 &&
                            strncmp(fname, "__monad_", 8) != 0) {
                            char fname_clean[256];
                            if (fname[0] == '"') {
                                size_t len = strlen(fname);
                                size_t copy = (len - 2) < 255 ? (len - 2) : 255;
                                memcpy(fname_clean, fname + 1, copy);
                                fname_clean[copy] = '\0';
                                fname = fname_clean;
                            }
                            EnvEntry *ue = env_lookup(ctx->env, fname);
                            if (ue && ue->kind == ENV_FUNC && ue->source_ast &&
                                ue->return_type &&
                                ue->return_type->kind == TYPE_BOOL) {
                                AST *src = ue->source_ast;
                                AST *lam = NULL;
                                if (src->type == AST_LAMBDA) lam = src;
                                else if (src->type == AST_LIST &&
                                         src->list.count >= 3 &&
                                         src->list.items[2]->type == AST_LAMBDA)
                                    lam = src->list.items[2];
                                if (lam) {
                                    int np = ue->param_count;
                                    LLVMBasicBlockRef ubb = LLVMAppendBasicBlockInContext(
                                        eval_ctx, f, "entry");
                                    LLVMBasicBlockRef usaved = LLVMGetInsertBlock(tmp_bld);
                                    LLVMPositionBuilderAtEnd(tmp_bld, ubb);
                                    Env *uenv = env_create_child(jit_ctx.env);
                                    for (int pi = 0; pi < np; pi++) {
                                        LLVMValueRef up = LLVMGetParam(f, pi);
                                        LLVMTypeRef  ut = type_to_llvm(&jit_ctx, ue->params[pi].type);
                                        LLVMValueRef ua = LLVMBuildAlloca(tmp_bld, ut,
                                                              ue->params[pi].name);
                                        LLVMBuildStore(tmp_bld, up, ua);
                                        env_insert(uenv, ue->params[pi].name,
                                                   type_clone(ue->params[pi].type), ua);
                                    }
                                    Env *prev_env = jit_ctx.env;
                                    jit_ctx.env = uenv;
                                    CodegenResult ur = {NULL, NULL};
                                    for (int bi2 = 0; bi2 < lam->lambda.body_count; bi2++)
                                        ur = codegen_expr(&jit_ctx, lam->lambda.body_exprs[bi2]);
                                    jit_ctx.env = prev_env;
                                    env_free(uenv);
                                    LLVMTypeRef ret_llvm = type_to_llvm(&jit_ctx, ue->return_type);
                                    LLVMValueRef rv = ur.value
                                        ? ur.value : LLVMConstNull(ret_llvm);
                                    if (LLVMTypeOf(rv) != ret_llvm &&
                                        LLVMGetTypeKind(ret_llvm) == LLVMIntegerTypeKind)
                                        rv = LLVMBuildIntCast2(tmp_bld, rv, ret_llvm, 1, "rc");
                                    LLVMBuildRet(tmp_bld, rv);
                                    if (usaved) LLVMPositionBuilderAtEnd(tmp_bld, usaved);
                                    changed = true;
                                }
                            }
                        }
                    }
                    f = next;
                }
            }
        }

        /* Dump IR for debugging */
        char *ir = LLVMPrintModuleToString(tmp_mod);
        LLVMDisposeMessage(ir);

        /* Verify */
        char *err = NULL;
        if (LLVMVerifyModule(tmp_mod, LLVMReturnStatusAction, &err) != 0) {
            if (err) LLVMDisposeMessage(err);
            goto cleanup;
        }
        if (err) LLVMDisposeMessage(err);

        LLVMExecutionEngineRef eval_ee = NULL;
        char *ee_err = NULL;
        if (LLVMCreateExecutionEngineForModule(&eval_ee, tmp_mod, &ee_err) != 0) {
            if (ee_err) LLVMDisposeMessage(ee_err);
            goto cleanup;
        }
        tmp_mod = NULL; /* engine owns it */

        uint64_t fn_addr = LLVMGetFunctionAddress(eval_ee, wrap_name);
        if (fn_addr) {
            if (is_str) {
                typedef long long (*pred_str_t)(const char *);
                long long r = ((pred_str_t)(uintptr_t)fn_addr)(arg_ast->string);
                result = (r & 0xFF) ? 1 : 0;
            } else {
                typedef long long (*pred_int_t)(long long);
                long long r = ((pred_int_t)(uintptr_t)fn_addr)((long long)arg_ast->number);
                result = (r & 0xFF) ? 1 : 0;
            }
        }
        LLVMDisposeExecutionEngine(eval_ee);

    } else {
        LLVMBuildRet(tmp_bld, LLVMConstInt(i1, 1, 0));
    }

cleanup:
    LLVMDisposeBuilder(tmp_bld);
    if (tmp_mod) LLVMDisposeModule(tmp_mod);
    LLVMContextDispose(eval_ctx);

    env_free(jit_ctx.env);

    return result;
}

static LLVMValueRef codegen_dot_chain(CodegenContext *ctx, const char *symbol, Type **out_type, AST *ast) {
    const char *first_dot = strchr(symbol, '.');
    if (!first_dot || first_dot == symbol) return NULL; // Not a dot-chain

    char var_name[256];
    size_t vlen = first_dot - symbol;
    if (vlen >= sizeof(var_name)) vlen = sizeof(var_name) - 1;
    memcpy(var_name, symbol, vlen);
    var_name[vlen] = '\0';

    EnvEntry *base_entry = resolve_symbol_with_modules(ctx, var_name, ast);
    if (!base_entry || !base_entry->type) {
        return NULL; // Might be a module prefix
    }

    Type *current_lay = base_entry->type;

    /* Auto-dereference if it's a Pointer :: Layout (like face.glyph) */
    if (current_lay->kind == TYPE_PTR && current_lay->element_type) {
        Type *inner = current_lay->element_type;
        if (inner->kind == TYPE_LAYOUT && inner->layout_field_count == 0 && inner->layout_name) {
            Type *resolved = env_lookup_layout(ctx->env, inner->layout_name);
            if (resolved) inner = resolved;
        }
        if (inner->kind == TYPE_LAYOUT) {
            current_lay = inner;
        } else {
            return NULL;
        }
    } else if (current_lay->kind != TYPE_LAYOUT) {
        return NULL;
    }


    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

    if (!base_entry->value) {
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: unbound variable or null value: %s",
                      parser_get_filename(), ast->line, ast->column, var_name);
        return NULL;
    }

    LLVMValueRef current_ptr = LLVMBuildLoad2(ctx->builder, ptr_t, base_entry->value, var_name);

    const char *p = first_dot + 1;
    while (*p) {
        if (current_lay->layout_field_count == 0 && current_lay->layout_name) {
            Type *resolved = env_lookup_layout(ctx->env, current_lay->layout_name);
            if (resolved) current_lay = resolved;
        }

        const char *next_dot = strchr(p, '.');
        char field_name[256];
        if (next_dot) {
            size_t flen = next_dot - p;
            if (flen >= sizeof(field_name)) flen = sizeof(field_name) - 1;
            memcpy(field_name, p, flen);
            field_name[flen] = '\0';
        } else {
            strncpy(field_name, p, sizeof(field_name) - 1);
            field_name[sizeof(field_name) - 1] = '\0';
        }

        int field_idx = -1;
        for (int i = 0; i < current_lay->layout_field_count; i++) {
            if (strcmp(current_lay->layout_fields[i].name, field_name) == 0) {
                field_idx = i;
                break;
            }
        }

        if (field_idx < 0) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: layout '%s' has no field '%s'",
                          parser_get_filename(), ast->line, ast->column,
                          current_lay->layout_name, field_name);
            return NULL;
        }

        char sname[256];
        snprintf(sname, sizeof(sname), "layout.%s", current_lay->layout_name);
        LLVMTypeRef struct_llvm = LLVMGetTypeByName2(ctx->context, sname);
        if (!struct_llvm) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: LLVM struct type for '%s' not found",
                          parser_get_filename(), ast->line, ast->column, current_lay->layout_name);
            return NULL;
        }

        LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
        LLVMValueRef fidx = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), field_idx, 0);
        LLVMValueRef indices[] = {zero, fidx};

        current_ptr = LLVMBuildGEP2(ctx->builder, struct_llvm, current_ptr, indices, 2, "fld_ptr");
        current_lay = current_lay->layout_fields[field_idx].type;

        // If this isn't the last field, continue chaining the GEP
        if (next_dot) {
            if (current_lay->kind != TYPE_LAYOUT) {
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: field '%s' is not a layout type, cannot access nested fields",
                              parser_get_filename(), ast->line, ast->column, field_name);
                return NULL;
            }
            // DO NOT LOAD! current_ptr is already the pointer to the nested struct.
            p = next_dot + 1;
        } else {
            break;
        }

    }

    if (out_type) *out_type = current_lay;
    return current_ptr;
}

static LLVMValueRef codegen_lvalue(CodegenContext *ctx, AST *ast, Type **out_type) {
     if (ast->type == AST_LIST && ast->list.count == 2) {
        Type *arr_type = NULL;
        /* Recursively resolve the base (e.g. font.chars) to get its memory pointer */
        LLVMValueRef arr_ptr = codegen_lvalue(ctx, ast->list.items[0], &arr_type);
        if (!arr_ptr) return NULL;

        if (arr_type && (arr_type->kind == TYPE_STRING || arr_type->kind == TYPE_PTR)) {
            LLVMTypeRef  i8   = LLVMInt8TypeInContext(ctx->context);
            LLVMTypeRef  i64  = LLVMInt64TypeInContext(ctx->context);
            LLVMTypeRef  ptr  = LLVMPointerType(i8, 0);
            LLVMValueRef str_val = LLVMBuildLoad2(ctx->builder, type_to_llvm(ctx, arr_type), arr_ptr, "str_val");
            CodegenResult idx_r = codegen_expr(ctx, ast->list.items[1]);
            LLVMValueRef idx = type_is_float(idx_r.type) ? LLVMBuildFPToSI(ctx->builder, idx_r.value, i64, "idx") : idx_r.value;
            if (LLVMTypeOf(idx) != i64) idx = LLVMBuildZExt(ctx->builder, idx, i64, "idx64");
            // For TYPE_PTR (pointer buffers like calloc'd arrays of structs/layouts),
            // stride by the pointee type size, not by pointer size.
            if (arr_type->kind == TYPE_PTR) {
                Type *pointee = arr_type->element_type;
                if (pointee && pointee->kind == TYPE_LAYOUT && pointee->layout_name) {
                    Type *full = env_lookup_layout(ctx->env, pointee->layout_name);
                    if (!full) full = pointee;
                    char struct_name[256];
                    snprintf(struct_name, sizeof(struct_name), "layout.%s", full->layout_name);
                    LLVMTypeRef elem_llvm = LLVMGetTypeByName2(ctx->context, struct_name);
                    if (!elem_llvm) elem_llvm = LLVMInt8TypeInContext(ctx->context);
                    if (out_type) *out_type = type_clone(full);
                    return LLVMBuildGEP2(ctx->builder, elem_llvm, str_val, &idx, 1, "ptr_idx");
                }
                if (out_type) *out_type = type_ptr(type_unknown());
                return LLVMBuildGEP2(ctx->builder, ptr, str_val, &idx, 1, "ptr_idx");
            }
            if (out_type) *out_type = type_char();
            return LLVMBuildGEP2(ctx->builder, i8, str_val, &idx, 1, "char_ptr");
        }

        if (arr_type && arr_type->kind == TYPE_ARR) {
            LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
            LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
            LLVMTypeRef arr_llvm = type_to_llvm(ctx, arr_type);

            if (arr_type->arr_is_fat) {
                LLVMTypeRef fat_ptr_t = arr_llvm;
                LLVMValueRef fat = LLVMBuildLoad2(ctx->builder, fat_ptr_t, arr_ptr, "fat_ptr");
                Type *et = arr_type->arr_element_type;
                if (out_type) *out_type = et;
                LLVMValueRef data_ptr = arr_fat_data(ctx, fat, et);

                CodegenResult idx_r2 = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef idx2 = type_is_float(idx_r2.type) ? LLVMBuildFPToSI(ctx->builder, idx_r2.value, i64, "idx") : idx_r2.value;
                if (LLVMTypeOf(idx2) != i64) idx2 = LLVMBuildZExt(ctx->builder, idx2, i64, "idx64");

                LLVMTypeRef elem_llvm2 = type_to_llvm(ctx, et);
                return LLVMBuildGEP2(ctx->builder, elem_llvm2, data_ptr, &idx2, 1, "ep");
            }

            Type *elem_type = (arr_type->arr_element_type && arr_type->arr_element_type->kind != TYPE_UNKNOWN)
                            ? arr_type->arr_element_type : type_int();
            if (out_type) *out_type = elem_type;
            LLVMTypeRef elem_llvm = type_to_llvm(ctx, elem_type);

            CodegenResult idx_r = codegen_expr(ctx, ast->list.items[1]);
            LLVMValueRef idx = type_is_float(idx_r.type) ? LLVMBuildFPToSI(ctx->builder, idx_r.value, i64, "idx") : idx_r.value;
            if (LLVMTypeOf(idx) != i64) idx = LLVMBuildZExt(ctx->builder, idx, i64, "idx64");

            bool arr_is_sized = (LLVMGetTypeKind(arr_llvm) == LLVMArrayTypeKind);
            if (arr_is_sized) {
                LLVMValueRef zero = LLVMConstInt(i32, 0, 0);
                LLVMValueRef idx32 = LLVMBuildTrunc(ctx->builder, idx, i32, "idx32");
                LLVMValueRef idxs[] = {zero, idx32};
                return LLVMBuildGEP2(ctx->builder, arr_llvm, arr_ptr, idxs, 2, "elem_ptr");
            } else {
                LLVMValueRef data_ptr = LLVMBuildLoad2(ctx->builder, LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0), arr_ptr, "arr_data_ptr");
                return LLVMBuildGEP2(ctx->builder, elem_llvm, data_ptr, &idx, 1, "elem_ptr");
            }
        }
     }

     if (ast->type == AST_SYMBOL) {
        Type *dot_type = NULL;
        LLVMValueRef dot_ptr = codegen_dot_chain(ctx, ast->symbol, &dot_type, ast);
        if (dot_ptr) {
            if (out_type) *out_type = dot_type;
            return dot_ptr;
        }

        EnvEntry *entry = env_lookup(ctx->env, ast->symbol);
        if (!entry) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: unbound variable: %s", parser_get_filename(), ast->line, ast->column, ast->symbol);
            return NULL;
        }
        if (out_type) *out_type = entry->type;

        if (entry->kind == ENV_FUNC) return entry->func_ref;
        if (entry->kind == ENV_BUILTIN) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: cannot take address of builtin '%s'", parser_get_filename(), ast->line, ast->column, ast->symbol);
            return NULL;
        }

        LLVMValueRef target;
        bool is_global = (entry->value && LLVMGetValueKind(entry->value) == LLVMGlobalVariableValueKind);
        if (is_global) {
            const char *gname = (entry->llvm_name && entry->llvm_name[0]) ? entry->llvm_name : entry->name;
            target = LLVMGetNamedGlobal(ctx->module, gname);
            if (!target) {
                LLVMTypeRef lt = type_to_llvm(ctx, entry->type);
                target = LLVMAddGlobal(ctx->module, lt, gname);
                LLVMSetLinkage(target, LLVMExternalLinkage);
            }
            entry->value = target;
        } else {
            target = entry->value;
        }
        return target;
    }

    CODEGEN_ERROR(ctx, "%s:%d:%d: error: expression is not an lvalue", parser_get_filename(), ast->line, ast->column);
    return NULL;
}

#define REQUIRE_ARGS(expected) \
    if (ast->list.count != (expected) + 1) \
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' requires %d argument(s)", parser_get_filename(), ast->line, ast->column, head->symbol, (expected))

#define REQUIRE_MIN_ARGS(expected) \
    if (ast->list.count < (expected) + 1) \
        CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' requires at least %d argument(s)", parser_get_filename(), ast->line, ast->column, head->symbol, (expected))

LLVMValueRef codegen_current_fn_zero_value(CodegenContext *ctx) {
    LLVMValueRef cur_fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
    LLVMTypeRef ret_t = LLVMGetReturnType(LLVMGlobalGetValueType(cur_fn));
    LLVMTypeKind kind = LLVMGetTypeKind(ret_t);
    if (kind == LLVMDoubleTypeKind || kind == LLVMFloatTypeKind) return LLVMConstReal(ret_t, 0.0);
    if (kind == LLVMPointerTypeKind) return LLVMConstPointerNull(ret_t);
    if (kind == LLVMVoidTypeKind) return NULL;
    return LLVMConstInt(ret_t, 0, 0);
}

CodegenResult codegen_expr(CodegenContext *ctx, AST *ast) {
    CodegenResult result = {NULL, NULL};

    /* setenv("REPL_DUMP_IR", "1", 1); */

    switch (ast->type) {
    case AST_NUMBER: {
        /* fprintf(stderr, ">>> CODEGEN NUMBER: literal_str=‘%s’, number=%g\n", */
        /*         ast->literal_str ? ast->literal_str : "NULL", ast->number); */

        Type *num_type = infer_literal_type(ast->number, ast->literal_str);

        /* fprintf(stderr, ">>> INFERRED TYPE: %s\n", type_to_string(num_type)); */

        result.type = num_type;

        if (type_is_float(num_type)) {
            result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), ast->number);
        } else if (ast->has_raw_int) {
            result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context),
                                        (int64_t)ast->raw_int, 0);
        } else {
            result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), (long long)ast->number, 0);
        }
        if (ast->has_raw_int) {
            char *val_str = LLVMPrintValueToString(result.value);
            LLVMDisposeMessage(val_str);
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

        // __adt_tag_CtorName — resolve to the integer tag index for that constructor
        if (strncmp(ast->symbol, "__adt_tag_", 10) == 0) {
            const char *ctor_name = ast->symbol + 10;
            EnvEntry *ce = env_lookup_adt_ctor(ctx->env, ctor_name);
            if (!ce) {
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown ADT constructor ‘%s’",
                              parser_get_filename(), ast->line, ast->column, ctor_name);
            }
            result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context),
                                        (uint64_t)ce->adt_tag, 0);
            result.type  = type_int();
            return result;
        }

        // ── Dot-access: p.x.y.z ─────────────────────────────────────────────
        Type *dot_type = NULL;
        LLVMValueRef dot_ptr = codegen_dot_chain(ctx, ast->symbol, &dot_type, ast);
        if (dot_ptr) {
            if (dot_type && dot_type->kind == TYPE_LAYOUT && dot_type->layout_field_count == 0 && dot_type->layout_name) {
                Type *full = env_lookup_layout(ctx->env, dot_type->layout_name);
                if (full) dot_type = full;
            }
            result.value = LLVMBuildLoad2(ctx->builder, type_to_llvm(ctx, dot_type), dot_ptr, "dot_val");
            result.type  = type_clone(dot_type);
            return result;
        }

        // ── Normal variable / module-qualified symbol ────────────────────────
        EnvEntry *entry = resolve_symbol_with_modules(ctx, ast->symbol, ast);
        if (!entry) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: unbound variable: %s",
                          parser_get_filename(), ast->line, ast->column, ast->symbol);
        }

        // ADT constructor used as a value — call it (nullary: no args)
        if (entry->kind == ENV_ADT_CTOR) {
            if (entry->func_ref) {
                LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                if (entry->param_count == 0) {
                    /* Nullary constructor — call zero-arg function to allocate struct */
                    LLVMTypeRef  ft = LLVMFunctionType(ptr_t, NULL, 0, 0);
                    /* Re-declare in current module if needed (REPL cross-module) */
                    const char *fname = LLVMGetValueName(entry->func_ref);
                    LLVMValueRef fn = LLVMGetNamedFunction(ctx->module, fname);
                    if (!fn) {
                        fn = LLVMAddFunction(ctx->module, fname, ft);
                        LLVMSetLinkage(fn, LLVMExternalLinkage);
                    }
                    result.value = LLVMBuildCall2(ctx->builder, ft, fn, NULL, 0, ast->symbol);
                } else {
                    /* Non-nullary constructor used as value — wrap as closure */
                    result.value = wrap_func_as_closure(ctx, entry);
                }
                result.type = type_clone(entry->type);
            } else {
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: ADT constructor ‘%s’ has no function ref",
                              parser_get_filename(), ast->line, ast->column, ast->symbol);
            }
            return result;
        }

        if (entry->type && entry->type->kind == TYPE_LAYOUT) {
            LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
            result.type  = entry->type;
            /* Load directly from the global — no runtime table lookup needed */
            result.value = LLVMBuildLoad2(ctx->builder, ptr_t,
                                          entry->value, ast->symbol);
            return result;
        }

        if (entry->kind == ENV_FUNC) {
            /* For non-ASCII names, re-resolve func_ref via mangled name
             * so the JIT can find the underlying function               */
            char *mangled = mangle_unicode_name(ast->symbol);
            if (mangled) {
                char *mname = mangled;
                EnvEntry *me = env_lookup(ctx->env, mname);
                if (me && me->kind == ENV_FUNC && me->func_ref) {
                    entry = me;
                }
                free(mangled);
            }
            result.value = wrap_func_as_closure(ctx, entry);
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

                /* Runtime-dispatched numeric builtin wrapper.
                 * If either argument is a float, use float arithmetic.
                 * Otherwise use integer arithmetic.                    */
                const char *sym = ast->symbol;

                /* Check if either arg is a float by inspecting the type tag.
                 * RuntimeValue.type is the first field (int at offset 0).   */
                LLVMTypeRef  i32_t   = LLVMInt32TypeInContext(ctx->context);
                LLVMValueRef rt_float_tag = LLVMConstInt(i32_t, RT_FLOAT, 0);

                /* Load type tag of ba and bb */
                LLVMValueRef tag_a = LLVMBuildLoad2(ctx->builder, i32_t, ba, "tag_a");
                LLVMValueRef tag_b = LLVMBuildLoad2(ctx->builder, i32_t, bb, "tag_b");
                LLVMValueRef is_float_a = LLVMBuildICmp(ctx->builder, LLVMIntEQ,
                                                         tag_a, rt_float_tag, "ifa");
                LLVMValueRef is_float_b = LLVMBuildICmp(ctx->builder, LLVMIntEQ,
                                                         tag_b, rt_float_tag, "ifb");
                LLVMValueRef use_float  = LLVMBuildOr(ctx->builder, is_float_a,
                                                       is_float_b, "use_float");

                LLVMValueRef      cur_fn2   = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                LLVMBasicBlockRef float_bb  = LLVMAppendBasicBlockInContext(ctx->context, cur_fn2, "flt");
                LLVMBasicBlockRef int_bb    = LLVMAppendBasicBlockInContext(ctx->context, cur_fn2, "int");
                LLVMBasicBlockRef merge_bb2 = LLVMAppendBasicBlockInContext(ctx->context, cur_fn2, "merge");
                LLVMBuildCondBr(ctx->builder, use_float, float_bb, int_bb);

                /* Float branch */
                LLVMPositionBuilderAtEnd(ctx->builder, float_bb);
                LLVMTypeRef  uft_f   = LLVMFunctionType(dbl, &ptr_t, 1, 0);
                LLVMValueRef fa2     = LLVMBuildCall2(ctx->builder, uft_f,
                                           get_rt_unbox_float(ctx), &ba, 1, "fa");
                LLVMValueRef fb2     = LLVMBuildCall2(ctx->builder, uft_f,
                                           get_rt_unbox_float(ctx), &bb, 1, "fb");
                LLVMValueRef fwr = NULL;
                if      (strcmp(sym, "+") == 0) fwr = LLVMBuildFAdd(ctx->builder, fa2, fb2, "fr");
                else if (strcmp(sym, "-") == 0) fwr = LLVMBuildFSub(ctx->builder, fa2, fb2, "fr");
                else if (strcmp(sym, "*") == 0) fwr = LLVMBuildFMul(ctx->builder, fa2, fb2, "fr");
                else if (strcmp(sym, "/") == 0) fwr = LLVMBuildFDiv(ctx->builder, fa2, fb2, "fr");
                else fwr = LLVMConstReal(dbl, 0.0);
                LLVMTypeRef  bft_f   = LLVMFunctionType(ptr_t, &dbl, 1, 0);
                LLVMValueRef fboxed  = LLVMBuildCall2(ctx->builder, bft_f,
                                           get_rt_value_float(ctx), &fwr, 1, "fboxed");
                LLVMBuildBr(ctx->builder, merge_bb2);
                LLVMBasicBlockRef float_end = LLVMGetInsertBlock(ctx->builder);

                /* Int branch */
                LLVMPositionBuilderAtEnd(ctx->builder, int_bb);
                LLVMValueRef wr = NULL;
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
                LLVMValueRef iboxed = LLVMBuildCall2(ctx->builder, bft,
                                          get_rt_value_int(ctx), &wr, 1, "iboxed");
                LLVMBuildBr(ctx->builder, merge_bb2);
                LLVMBasicBlockRef int_end = LLVMGetInsertBlock(ctx->builder);

                /* Merge */
                LLVMPositionBuilderAtEnd(ctx->builder, merge_bb2);
                LLVMValueRef phi = LLVMBuildPhi(ctx->builder, ptr_t, "boxed");
                LLVMAddIncoming(phi, &fboxed, &float_end, 1);
                LLVMAddIncoming(phi, &iboxed, &int_end,   1);
                LLVMBuildRet(ctx->builder, phi);
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
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: ‘%s’ has no type information",
                          parser_get_filename(), ast->line, ast->column, ast->symbol);
        }

        /* fprintf(stderr, "[symbol load] name=‘%s’ entry=%p entry->value=%p is_global=%d\n", */
        /*         ast->symbol, (void*)entry, (void*)entry->value, */
        /*         entry->value ? (LLVMIsAGlobalVariable(entry->value) != NULL) : -1); */

        result.type = type_clone(entry->type);

        /* For non-ASCII symbol names, resolve via mangled LLVM global name */
        LLVMValueRef load_target = entry->value;
        {
            char *mangled = mangle_unicode_name(ast->symbol);
            if (mangled) {
                /* Only use mangled name if the variable was actually stored
                 * under that mangled name (i.e. it IS a unicode/special-char
                 * variable). For normal variables with hyphens like the-email,
                 * mangle_unicode_name fires but the variable is stored under
                 * its original name — use entry->value directly.            */
                LLVMValueRef mgv = LLVMGetNamedGlobal(ctx->module, mangled);
                if (mgv) {
                    load_target = mgv;
                }
                free(mangled);
            }
        }

        result.value = LLVMBuildLoad2(ctx->builder, type_to_llvm(ctx, entry->type),
                                      load_target, ast->symbol);
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
        Type *target_type = NULL;
        LLVMValueRef target_ptr = codegen_lvalue(ctx, operand, &target_type);
        if (!target_ptr) return result;
        /* Layout globals hold either a malloc'd struct ptr (non-scalar layouts)
         * or a null handle slot (scalar typedefs like VkInstance).
         * For malloc'd structs: load through to get the struct pointer.
         * For handle slots: pass the global address so Vulkan can write into it. */
        if (target_type && target_type->kind == TYPE_LAYOUT &&
            operand->type == AST_SYMBOL &&
            LLVMGetValueKind(target_ptr) == LLVMGlobalVariableValueKind) {
            Type *full = env_lookup_layout(ctx->env,
                             target_type->layout_name ? target_type->layout_name : "");
            if (full && full->layout_field_count > 1) {
                /* Multi-field struct — global holds malloc'd ptr, load through */
                LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                result.value = LLVMBuildLoad2(ctx->builder, ptr_t, target_ptr, "lay_addr");
                result.type  = type_ptr(target_type);
                return result;
            }
            /* Single-field or unknown — scalar handle, pass global address directly */
        }
        result.value = target_ptr;
        result.type  = type_ptr(target_type ? target_type : type_unknown());
        return result;
    }

    case AST_ARRAY: {
        Type *elem_type = NULL;
        if (ast->array.element_count > 0) {
            CodegenResult first = codegen_expr(ctx, ast->array.elements[0]);
            elem_type = first.type ? type_clone(first.type) : type_int();
        } else elem_type = type_int();

        int n = (int)ast->array.element_count;
        LLVMTypeRef elem_llvm = type_to_llvm(ctx, elem_type);
        LLVMTypeRef arr_llvm  = LLVMArrayType(elem_llvm, n);
        LLVMValueRef stack    = LLVMBuildAlloca(ctx->builder, arr_llvm, "arr_stack");

        for (int i = 0; i < n; i++) {
            CodegenResult elem = codegen_expr(ctx, ast->array.elements[i]);
            LLVMValueRef ev = elem.type && elem_type ? emit_type_cast(ctx, elem.value, elem_llvm) : elem.value;
            LLVMValueRef idxs[] = {LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0), LLVMConstInt(LLVMInt32TypeInContext(ctx->context), i, 0)};
            LLVMBuildStore(ctx->builder, ev, LLVMBuildGEP2(ctx->builder, arr_llvm, stack, idxs, 2, "ep"));
        }

        result.value = arr_make_fat(ctx, stack, n, elem_type);
        result.type  = type_arr_fat(type_clone(elem_type));
        result.type->arr_size = n;
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

        // Handle ("string" i) — inline string indexing
        if (head->type == AST_STRING && ast->list.count == 2) {
            LLVMTypeRef  i8  = LLVMInt8TypeInContext(ctx->context);
            LLVMTypeRef  i64 = LLVMInt64TypeInContext(ctx->context);
            LLVMValueRef str_val = LLVMBuildGlobalStringPtr(ctx->builder, head->string, "str");
            CodegenResult idx_r  = codegen_expr(ctx, ast->list.items[1]);
            LLVMValueRef idx = type_is_float(idx_r.type)
                ? LLVMBuildFPToSI(ctx->builder, idx_r.value, i64, "idx")
                : idx_r.value;
            if (LLVMTypeOf(idx) != i64)
                idx = LLVMBuildZExt(ctx->builder, idx, i64, "idx64");
            LLVMValueRef char_ptr = LLVMBuildGEP2(ctx->builder, i8, str_val, &idx, 1, "char_ptr");
            result.value = LLVMBuildLoad2(ctx->builder, i8, char_ptr, "char");
            result.type  = type_char();
            return result;
        }

        // Handle ([1 2 3] i) — inline array indexing
        if (head->type == AST_ARRAY && ast->list.count == 2) {
            CodegenResult arr_r = codegen_expr(ctx, head);
            CodegenResult idx_r = codegen_expr(ctx, ast->list.items[1]);
            LLVMTypeRef  i64       = LLVMInt64TypeInContext(ctx->context);
            LLVMTypeRef  ptr_t     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
            Type        *elem_type = arr_r.type && arr_r.type->arr_element_type
                                   ? arr_r.type->arr_element_type : type_int();
            LLVMTypeRef  elem_llvm = type_to_llvm(ctx, elem_type);
            LLVMValueRef idx = type_is_float(idx_r.type)
                ? LLVMBuildFPToSI(ctx->builder, idx_r.value, i64, "idx")
                : idx_r.value;
            if (LLVMTypeOf(idx) != i64)
                idx = LLVMBuildZExt(ctx->builder, idx, i64, "idx64");

            /* Bounds check */
            emit_bounds_check(ctx, ast, idx, arr_fat_size(ctx, arr_r.value), "array index out of bounds");

            /* Extract data pointer and load element */
            LLVMValueRef data_ptr = arr_fat_data(ctx, arr_r.value, elem_type);
            LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder, elem_llvm,
                                                   data_ptr, &idx, 1, "elem_ptr");
            result.value = LLVMBuildLoad2(ctx->builder, elem_llvm, elem_ptr, "elem");
            result.type  = type_clone(elem_type);
            return result;
        }

        // Handle ('(1 2 3) i) — inline quoted list indexing
        if (head->type == AST_LIST && head->list.count == 2 &&
            head->list.items[0]->type == AST_SYMBOL &&
            strcmp(head->list.items[0]->symbol, "quote") == 0 &&
            ast->list.count == 2) {

            AST *quoted_content = head->list.items[1];
            AST *idx_ast = ast->list.items[1];

            // --- FANCY COMPILE-TIME CHECK ---
            if (quoted_content->type == AST_LIST && idx_ast->type == AST_NUMBER) {
                int list_len = (int)quoted_content->list.count;
                int idx_val  = (int)idx_ast->number;
                if (idx_val < 0 || idx_val >= list_len) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: index %d is out of bounds for literal list of length %d",
                                  parser_get_filename(), idx_ast->line, idx_ast->column,
                                  idx_val, list_len);
                }
            }

            // Normal codegen if check passes or index is dynamic
            CodegenResult list_r = codegen_expr(ctx, head);
            CodegenResult idx_r  = codegen_expr(ctx, idx_ast);

            LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
            LLVMTypeRef i64   = LLVMInt64TypeInContext(ctx->context);
            LLVMValueRef idx = type_is_float(idx_r.type) ? LLVMBuildFPToSI(ctx->builder, idx_r.value, i64, "idx") : idx_r.value;
            if (LLVMTypeOf(idx) != i64) idx = LLVMBuildZExt(ctx->builder, idx, i64, "idx64");

            result.value = LLVMBuildCall2(ctx->builder, LLVMFunctionType(ptr_t, (LLVMTypeRef[]){ptr_t, i64}, 2, 0),
                                          get_rt_list_nth(ctx), (LLVMValueRef[]){list_r.value, idx}, 2, "list_idx");
            result.type  = type_unknown();
            return result;
        }

        // Handle (coll i) — inline indexing on any expression of type Coll
        // This acts as the catch-all for non-literal collection indexing
        if (ast->list.count == 2) {
            CodegenResult coll_r = codegen_expr(ctx, head);
            if (coll_r.type && coll_r.type->kind == TYPE_COLL) {
                LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef i64   = LLVMInt64TypeInContext(ctx->context);

                // Unbox the collection value to a raw list pointer
                CodegenResult idx_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef idx = type_is_float(idx_r.type)
                    ? LLVMBuildFPToSI(ctx->builder, idx_r.value, i64, "idx")
                    : idx_r.value;
                if (LLVMTypeOf(idx) != i64)
                    idx = LLVMBuildZExt(ctx->builder, idx, i64, "idx64");

                LLVMTypeRef i32_t = LLVMInt32TypeInContext(ctx->context);
                LLVMValueRef tag = LLVMBuildLoad2(ctx->builder, i32_t, coll_r.value, "cidx_tag");
                LLVMValueRef is_str = LLVMBuildICmp(ctx->builder, LLVMIntEQ, tag,
                                                     LLVMConstInt(i32_t, RT_STRING, 0), "cidx_is_str");

                LLVMValueRef cur_fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                LLVMBasicBlockRef str_bb   = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "cidx_str");
                LLVMBasicBlockRef list_bb  = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "cidx_list");
                LLVMBasicBlockRef ok_bb    = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "coll_ok");
                LLVMBasicBlockRef err_bb   = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "coll_err");
                LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "cidx_merge");

                LLVMBuildCondBr(ctx->builder, is_str, str_bb, list_bb);

                /* String path: get char at index */
                LLVMPositionBuilderAtEnd(ctx->builder, str_bb);
                LLVMValueRef raw_str = LLVMBuildCall2(ctx->builder,
                                           LLVMFunctionType(ptr_t, &ptr_t, 1, 0),
                                           get_rt_unbox_string(ctx), &coll_r.value, 1, "cidx_raw_str");
                LLVMValueRef strlen_fn = LLVMGetNamedFunction(ctx->module, "strlen");
                if (!strlen_fn) {
                    LLVMTypeRef ft = LLVMFunctionType(i64, &ptr_t, 1, 0);
                    strlen_fn = LLVMAddFunction(ctx->module, "strlen", ft);
                    LLVMSetLinkage(strlen_fn, LLVMExternalLinkage);
                }

                LLVMValueRef str_len = LLVMBuildCall2(ctx->builder, LLVMFunctionType(i64, &ptr_t, 1, 0), strlen_fn, &raw_str, 1, "cidx_str_len");
                emit_bounds_check(ctx, ast, idx, str_len, "collection index out of bounds");
                LLVMBasicBlockRef str_ok_bb = LLVMGetInsertBlock(ctx->builder);
                LLVMValueRef char_ptr = LLVMBuildGEP2(ctx->builder, LLVMInt8TypeInContext(ctx->context),
                                            raw_str, &idx, 1, "char_ptr");
                LLVMValueRef char_val = LLVMBuildLoad2(ctx->builder, LLVMInt8TypeInContext(ctx->context),
                                            char_ptr, "char_val");
                LLVMValueRef boxed_char = LLVMBuildCall2(ctx->builder,
                                              LLVMFunctionType(ptr_t, (LLVMTypeRef[]){LLVMInt8TypeInContext(ctx->context)}, 1, 0),
                                              get_rt_value_char(ctx), &char_val, 1, "boxed_char");
                LLVMBuildBr(ctx->builder, merge_bb);

                /* List path: bounds check then rt_list_nth */
                LLVMPositionBuilderAtEnd(ctx->builder, list_bb);
                LLVMValueRef raw_coll_ptr = LLVMBuildCall2(ctx->builder,
                                                LLVMFunctionType(ptr_t, &ptr_t, 1, 0),
                                                get_rt_unbox_list(ctx), &coll_r.value, 1, "raw_coll");
                /* Skip length check — rt_list_length walks infinite lazy lists */
                LLVMBuildBr(ctx->builder, ok_bb);

                LLVMPositionBuilderAtEnd(ctx->builder, err_bb);
                emit_runtime_error(ctx, ast, "collection index out of bounds");

                LLVMPositionBuilderAtEnd(ctx->builder, ok_bb);
                LLVMValueRef nth_val = LLVMBuildCall2(ctx->builder,
                                           LLVMFunctionType(ptr_t, (LLVMTypeRef[]){ptr_t, i64}, 2, 0),
                                           get_rt_list_nth(ctx), (LLVMValueRef[]){raw_coll_ptr, idx}, 2, "coll_idx");
                LLVMBuildBr(ctx->builder, merge_bb);

                LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
                LLVMValueRef phi = LLVMBuildPhi(ctx->builder, ptr_t, "cidx_result");
                LLVMValueRef phi_vals[] = { boxed_char, nth_val };
                LLVMBasicBlockRef phi_bbs[] = { str_ok_bb, ok_bb };
                LLVMAddIncoming(phi, phi_vals, phi_bbs, 2);

                result.value = phi;
                result.type  = type_unknown();
                return result;
            }
        }



        if (head->type == AST_SYMBOL) {
            /* check_purity(ctx, ctx->current_function_name, head->symbol, ast); */ // TODO HERE

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

                    {
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
                        /* fprintf(stderr, "DEBUG param[%d] name=‘%s’ type_name=‘%s’ type_kind=%d\n", */
                        /*         i, */
                        /*         lambda->lambda.params[i].name ? lambda->lambda.params[i].name : "?", */
                        /*         lambda->lambda.params[i].type_name ? lambda->lambda.params[i].type_name : "NULL", */
                        /*         env_params[i].type ? env_params[i].type->kind : -1); */
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
                                        "%s:%d:%d: error: unknown type ‘%s’ for rest parameter ‘%s’",
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
                            /* Not a primitive — check ADT/layout registry */
                            if (!param_type || param_type->kind == TYPE_UNKNOWN) {
                                Type *lay = env_lookup_layout(ctx->env, param->type_name);
                                if (lay) param_type = type_clone(lay);
                            }
                            if (!param_type || param_type->kind == TYPE_UNKNOWN) {
                                /* Check type aliases (refinements) */
                                bool found_alias = false;
                                for (TypeAlias *a = g_aliases; a; a = a->next) {
                                    if (strcmp(a->alias_name, param->type_name) == 0) {
                                        param_type = type_from_name(a->target_name);
                                        found_alias = true;
                                        break;
                                    }
                                }
                                if (!found_alias && (!param_type || param_type->kind == TYPE_UNKNOWN)) {
                                    /* Single lowercase letter = type variable (e.g. 'a', 'b').
                                     * Treat as unknown/polymorphic, same as return type does. */
                                    bool is_type_var = (strlen(param->type_name) == 1 &&
                                                        param->type_name[0] >= 'a' &&
                                                        param->type_name[0] <= 'z');
                                    if (is_type_var) {
                                        param_type = type_unknown();
                                    } else {
                                        CODEGEN_ERROR(ctx,
                                            "%s:%d:%d: error: unknown input parameter type '%s' "
                                            "for parameter '%s' — is '%s' defined as a data type, "
                                            "layout, or type alias before this function?",
                                            parser_get_filename(), lambda->line, lambda->column,
                                            param->type_name,
                                            param->name ? param->name : "?",
                                            param->type_name);
                                    }
                                }                            }
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
                            // Use HM-inferred concrete param type if available,
                            // but only for simple ground types (Int, Float, Bool, Char).
                            // For function/arrow types, always use ptr since they
                            // are closures and must be passed as RuntimeValue*.
                            Type *hm_param = NULL;
                            Type *t = hm_scheme->type;
                            for (int j = 0; j < i && t && t->kind == TYPE_ARROW; j++)
                                t = t->arrow_ret;
                            if (t && t->kind == TYPE_ARROW && t->arrow_param) {
                                Type *hp = t->arrow_param;
                                // Only use concrete scalar types — not arrows/vars/unknown
                                if (hp->kind != TYPE_VAR    &&
                                    hp->kind != TYPE_UNKNOWN &&
                                    hp->kind != TYPE_ARROW   &&
                                    hp->kind != TYPE_FN)
                                    hm_param = hp;
                            }
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

                    /* Refine use_closure_abi: if all params have known types
                     * and there are no captures, use typed ABI even for inner
                     * functions — this makes let/let* work correctly.        */
                    if (use_closure_abi && captured_count == 0) {
                        bool all_typed = (total_params > 0);
                        for (int _i = 0; _i < total_params; _i++) {
                            if (!env_params[_i].type ||
                                env_params[_i].type->kind == TYPE_UNKNOWN ||
                                env_params[_i].type->kind == TYPE_VAR) {
                                all_typed = false;
                                break;
                            }
                        }
                        if (all_typed) {
                            use_closure_abi = false;
                            /* Recompute llvm_param_count and param_types for typed ABI */
                            llvm_param_count = total_params;
                            free(param_types);
                            param_types = malloc(sizeof(LLVMTypeRef) *
                                                 (llvm_param_count ? llvm_param_count : 1));
                            for (int _i = 0; _i < total_params; _i++)
                                param_types[_i] = type_to_llvm(ctx, env_params[_i].type);
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

                    /* Only use HM-inferred return type if no explicit annotation was given.
                     * If the user wrote -> a (a type variable), keep it as unknown so the
                     * function returns a boxed RuntimeValue* and preserves float/int. */
                    bool ret_was_explicit_var = (lambda->lambda.return_type &&
                                                 strlen(lambda->lambda.return_type) == 1 &&
                                                 lambda->lambda.return_type[0] >= 'a' &&
                                                 lambda->lambda.return_type[0] <= 'z');
                    if (ret_type->kind == TYPE_UNKNOWN && hm_scheme && !ret_was_explicit_var) {
                        Type *t = hm_scheme->type;
                        while (t && t->kind == TYPE_ARROW)
                            t = t->arrow_ret;
                        if (t && t->kind != TYPE_VAR && t->kind != TYPE_UNKNOWN)
                            ret_type = type_clone(t);
                    }
                    if (ret_was_explicit_var) {
                        ret_type = type_unknown();
                    }


                    // Stub functions and closure ABI use ptr return
                    // Only emit a stub if HM scheme has quantified type vars.
                    // Monomorphic functions (e.g. add :: Int->Int->Int inferred)
                    // should compile their body normally even without annotations.
                    bool has_type_vars = hm_scheme && hm_scheme->quantified_count > 0;
                    /* fprintf(stderr, "DEBUG all_params_unknown=%d has_type_vars=%d use_closure_abi=%d\n", */
                    /*         all_params_unknown, has_type_vars, use_closure_abi); */
                    bool is_poly_stub  = !use_closure_abi && all_params_unknown && has_type_vars;
                    /* fprintf(stderr, "DEBUG is_poly_stub=%d\n", is_poly_stub); */

                    /* ── Compile-time refinement check for 0-arg functions ──
                     * If the declared return type is a refinement and the
                     * function takes no arguments, JIT-compile and run it now,
                     * then verify the result satisfies the predicate.         */

                    /* Check that the declared return type actually exists */
                    if (lambda->lambda.return_type) {
                        const char *rtn = lambda->lambda.return_type;
                        Type *rtn_type = type_from_name(rtn);
                        /* single lowercase letter = type variable, always valid */
                        bool rtn_is_typevar = (rtn[0] >= 'a' && rtn[0] <= 'z' && rtn[1] == '\0');
                        bool rtn_known = rtn_is_typevar ||
                                         (rtn_type && rtn_type->kind != TYPE_UNKNOWN &&
                                          rtn_type->kind != TYPE_VAR);
                        if (!rtn_known) {
                            /* Check aliases */
                            for (TypeAlias *a = g_aliases; a && !rtn_known; a = a->next)
                                if (strcmp(a->alias_name, rtn) == 0) rtn_known = true;
                            /* Check refinements */
                            for (RefinementEntry *e = g_refinements; e && !rtn_known; e = e->next)
                                if (strcmp(e->name, rtn) == 0) rtn_known = true;
                            /* Check layouts */
                            if (!rtn_known && env_lookup_layout(ctx->env, rtn))
                                rtn_known = true;
                        }
                        if (!rtn_known) {
                            CODEGEN_ERROR(ctx,
                                "%s:%d:%d: error: function ‘%s’ declares unknown return type ‘%s’",
                                parser_get_filename(), ast->line, ast->column,
                                var_name, rtn);
                        }
                    }

                    if (lambda->lambda.return_type && lambda->lambda.param_count == 0 &&
                        lambda->lambda.body_count > 0) {
                        const char *rtype_name = lambda->lambda.return_type;
                        /* fprintf(stderr, "DEBUG retcheck: fn=‘%s’ rtype=‘%s’\n", var_name, rtype_name); */
                        RefinementEntry *rentry = NULL;
                        {
                            const char *name = rtype_name;
                            char rbuf[256];
                            for (int depth = 0; depth < 32 && !rentry; depth++) {
                                for (RefinementEntry *e = g_refinements; e; e = e->next)
                                    if (strcmp(e->name, name) == 0) { rentry = e; break; }
                                if (rentry) break;
                                bool stepped = false;
                                for (TypeAlias *a = g_aliases; a; a = a->next) {
                                    if (strcmp(a->alias_name, name) == 0) {
                                        strncpy(rbuf, a->target_name, sizeof(rbuf)-1);
                                        rbuf[sizeof(rbuf)-1] = '\0';
                                        name = rbuf; stepped = true; break;
                                    }
                                }
                                if (!stepped) break;
                            }
                        }
                        if (rentry) {
                            ensure_refinement_ctx();

                            LLVMContextRef jit_ctx2 = LLVMContextCreate();
                            char jmod_name[64];
                            snprintf(jmod_name, sizeof(jmod_name), "__retcheck_%u", g_jit_pred_seq);
                            LLVMModuleRef  jit_mod  = LLVMModuleCreateWithNameInContext(jmod_name, jit_ctx2);
                            LLVMSetTarget(jit_mod, LLVMGetDefaultTargetTriple());
                            LLVMBuilderRef jit_bld  = LLVMCreateBuilderInContext(jit_ctx2);

                            CodegenContext jc;
                            memset(&jc, 0, sizeof(jc));
                            jc.module  = jit_mod;
                            jc.builder = jit_bld;
                            jc.context = jit_ctx2;
                            jc.env     = env_create_child(ctx->env);
                            jc.init_fn = NULL;
                            declare_runtime_functions(&jc);

                            /* Walk base type chain to find primitive type */
                            const char *base_walk = rentry->base_type;
                            char base_buf[256];
                            for (int depth = 0; depth < 32; depth++) {
                                RefinementEntry *be = NULL;
                                for (RefinementEntry *e = g_refinements; e; e = e->next)
                                    if (strcmp(e->name, base_walk) == 0) { be = e; break; }
                                if (!be) break;
                                strncpy(base_buf, be->base_type, sizeof(base_buf)-1);
                                base_buf[sizeof(base_buf)-1] = '\0';
                                base_walk = base_buf;
                            }
                            Type *fn_ret_base = type_from_name(base_walk);
                            bool fn_ret_str   = fn_ret_base && fn_ret_base->kind == TYPE_STRING;
                            LLVMTypeRef fn_ret_llvm = fn_ret_str
                                ? LLVMPointerType(LLVMInt8TypeInContext(jit_ctx2), 0)
                                : LLVMInt64TypeInContext(jit_ctx2);

                            /* Build 0-arg function that runs the body */
                            char jfn_name[256];
                            snprintf(jfn_name, sizeof(jfn_name), "__jret_%u", g_jit_pred_seq++);
                            LLVMTypeRef  jfn_ft = LLVMFunctionType(fn_ret_llvm, NULL, 0, 0);
                            LLVMValueRef jfn    = LLVMAddFunction(jit_mod, jfn_name, jfn_ft);
                            LLVMBasicBlockRef jfn_bb = LLVMAppendBasicBlockInContext(
                                                           jit_ctx2, jfn, "entry");
                            LLVMPositionBuilderAtEnd(jit_bld, jfn_bb);
                            jc.init_fn = jfn;

                            /* Swap ctx to JIT for body codegen */
                            LLVMModuleRef  swap_mod  = ctx->module;
                            LLVMBuilderRef swap_bld  = ctx->builder;
                            LLVMContextRef swap_cctx = ctx->context;
                            Env           *swap_env  = ctx->env;
                            ctx->module  = jit_mod;
                            ctx->builder = jit_bld;
                            ctx->context = jit_ctx2;
                            ctx->env     = env_create_child(jc.env);

                            CodegenResult jbody = {NULL, NULL};
                            const char *saved_fname = ctx->current_function_name;
                            ctx->current_function_name = var_name;
                            for (int bi = 0; bi < lambda->lambda.body_count; bi++)
                                jbody = codegen_expr(ctx, lambda->lambda.body_exprs[bi]);
                            ctx->current_function_name = saved_fname;

                            Env *body_env = ctx->env;
                            ctx->module  = swap_mod;
                            ctx->builder = swap_bld;
                            ctx->context = swap_cctx;
                            ctx->env     = swap_env;
                            env_free(body_env);

                            if (jbody.value) {
                                LLVMValueRef jret = jbody.value;
                                LLVMTypeRef  jact = LLVMTypeOf(jret);
                                if (jact != fn_ret_llvm) {
                                    if (fn_ret_str && LLVMGetTypeKind(jact) == LLVMPointerTypeKind)
                                        ; /* ok */
                                    else if (!fn_ret_str && LLVMGetTypeKind(jact) == LLVMIntegerTypeKind)
                                        jret = LLVMBuildIntCast2(jit_bld, jret, fn_ret_llvm, 1, "rc");
                                    else if (!fn_ret_str && type_is_float(jbody.type))
                                        jret = LLVMBuildFPToSI(jit_bld, jret, fn_ret_llvm, "rc");
                                }
                                LLVMBuildRet(jit_bld, jret);
                            } else {
                                LLVMBuildRet(jit_bld, fn_ret_str
                                    ? (LLVMValueRef)LLVMBuildGlobalStringPtr(jit_bld, "", "empty")
                                    : LLVMConstInt(fn_ret_llvm, 0, 0));
                            }

                            /* Verify and run to extract the return value */
                            char *verr = NULL;
                            if (LLVMVerifyModule(jit_mod, LLVMReturnStatusAction, &verr) == 0) {
                                LLVMExecutionEngineRef ee = NULL;
                                char *ee_err = NULL;
                                if (LLVMCreateExecutionEngineForModule(&ee, jit_mod, &ee_err) == 0) {
                                    jit_mod = NULL;
                                    uint64_t addr = LLVMGetFunctionAddress(ee, jfn_name);
                                    if (addr) {
                                        AST *ret_ast = NULL;
                                        if (fn_ret_str) {
                                            typedef const char *(*str_fn_t)(void);
                                            const char *ret_str = ((str_fn_t)(uintptr_t)addr)();
                                            if (ret_str) ret_ast = ast_new_string((char*)ret_str);
                                        } else {
                                            typedef long long (*int_fn_t)(void);
                                            long long ret_int = ((int_fn_t)(uintptr_t)addr)();
                                            ret_ast = ast_new_number((double)ret_int, NULL);
                                            ret_ast->has_raw_int = true;
                                            ret_ast->raw_int     = ret_int;
                                        }
                                        if (ret_ast) {
                                            int ok = jit_eval_refinement(ctx, rtype_name, ret_ast);
                                            ast_free(ret_ast);
                                            if (ok == 0) {
                                                LLVMDisposeExecutionEngine(ee);
                                                LLVMDisposeBuilder(jit_bld);
                                                LLVMContextDispose(jit_ctx2);
                                                env_free(jc.env);
                                                CODEGEN_ERROR(ctx,
                                                    "%s:%d:%d: error: function ‘%s’ declares return type "
                                                    "‘%s’ but its return value does not satisfy the refinement",
                                                    parser_get_filename(), ast->line, ast->column,
                                                    var_name, rtype_name);
                                            }
                                        }
                                    }
                                    LLVMDisposeExecutionEngine(ee);
                                } else {
                                    if (ee_err) LLVMDisposeMessage(ee_err);
                                }
                            } else {
                                if (verr) LLVMDisposeMessage(verr);
                            }

                            LLVMDisposeBuilder(jit_bld);
                            if (jit_mod) LLVMDisposeModule(jit_mod);
                            LLVMContextDispose(jit_ctx2);
                            env_free(jc.env);
                        }
                    }

                    LLVMTypeRef ret_llvm_type = (use_closure_abi || is_poly_stub ||
                                                 ret_type->kind == TYPE_VAR     ||
                                                 ret_type->kind == TYPE_UNKNOWN)
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

                    // LLVMDumpValue(func); // DEBUG

                    /* Give closure-ABI inner functions external linkage with a
                     * module-qualified name so they survive in .so exports    */
                    if (use_closure_abi && is_inner) {
                        char mangled_inner[256];
                        const char *mod = ctx->module_ctx && ctx->module_ctx->decl
                            ? ctx->module_ctx->decl->name : "anon";
                        snprintf(mangled_inner, sizeof(mangled_inner),
                                 "__%s_closure_%s", mod, var_name);
                        LLVMSetValueName2(func, mangled_inner, strlen(mangled_inner));
                        LLVMSetLinkage(func, LLVMExternalLinkage);
                    }

           ///// Forward-declare in env (enables recursion)

                    env_insert_func(ctx->env, var_name,
                                    clone_params(env_params, total_params),
                                    total_params, type_clone(ret_type),
                                    func, lambda->lambda.docstring, NULL);
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

                    LLVMValueRef self_alloca = NULL;
                    if (use_closure_abi && ctx->current_function_name != NULL) {
                        LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                        self_alloca = LLVMBuildAlloca(ctx->builder, ptr_t, var_name);
                        LLVMBuildStore(ctx->builder, LLVMConstPointerNull(ptr_t), self_alloca);
                        env_insert(ctx->env, var_name, type_fn(NULL, 0, NULL), self_alloca);
                        EnvEntry *clo_var = env_lookup(ctx->env, var_name);
                        if (clo_var) {
                            clo_var->func_ref       = func;
                            clo_var->is_closure_abi = true;
                            clo_var->param_count    = total_params;
                            clo_var->params         = clone_params(env_params, total_params);
                            clo_var->return_type    = type_clone(ret_type);
                        }
                    }

          ///// Build function body

                    LLVMBasicBlockRef entry_block = LLVMAppendBasicBlockInContext(
                                                        ctx->context, func, "entry");
                    LLVMBasicBlockRef saved_block = LLVMGetInsertBlock(ctx->builder);
                    LLVMValueRef      saved_init  = ctx->init_fn;
                    LLVMPositionBuilderAtEnd(ctx->builder, entry_block);
                    ctx->init_fn = func;

                    Env *saved_env = ctx->env;
                    ctx->env = env_create_child(saved_env);

                    // env param (index 0 in closure ABI)
                    LLVMValueRef env_llvm_param = NULL;
                    if (use_closure_abi) {
                        env_llvm_param = LLVMGetParam(func, 0);
                        LLVMSetValueName2(env_llvm_param, "env", 3);

                        // Self-bind the function inside its own body to avoid outer scope access
                        if (ctx->current_function_name != NULL) {
                            LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                            LLVMValueRef fn_ptr = LLVMBuildBitCast(ctx->builder, func, ptr_t, "fn_ptr");
                            LLVMTypeRef i32t = LLVMInt32TypeInContext(ctx->context);
                            LLVMValueRef clo_fn = get_rt_value_closure(ctx);
                            LLVMTypeRef clo_params[] = {ptr_t, ptr_t, i32t, i32t};
                            LLVMTypeRef clo_ft = LLVMFunctionType(ptr_t, clo_params, 4, 0);
                            LLVMValueRef clo_args[] = {
                                fn_ptr, env_llvm_param,
                                LLVMConstInt(i32t, captured_count, 0),
                                LLVMConstInt(i32t, total_params, 0)
                            };
                            LLVMValueRef self_clo = LLVMBuildCall2(ctx->builder, clo_ft, clo_fn, clo_args, 4, "self_clo");
                            LLVMValueRef self_alloca = LLVMBuildAlloca(ctx->builder, ptr_t, var_name);
                            LLVMBuildStore(ctx->builder, self_clo, self_alloca);
                            env_insert(ctx->env, var_name, type_fn(NULL, 0, NULL), self_alloca);
                            EnvEntry *clo_var = env_lookup(ctx->env, var_name);
                            if (clo_var) {
                                clo_var->func_ref       = func;
                                clo_var->is_closure_abi = true;
                                clo_var->param_count    = total_params;
                                clo_var->params         = clone_params(env_params, total_params);
                                clo_var->return_type    = type_clone(ret_type);
                            }
                        }

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
                            if (param_type->kind == TYPE_LAYOUT) {
                                /* ADT/layout param — the boxed value IS the raw heap ptr,
                                 * no unboxing needed, pass through directly */
                                unboxed = boxed;
                            } else if (type_is_integer(param_type)) {
                                /* For TYPE_INT_ARBITRARY the value arrives as a
                                 * raw iN integer (typed ABI), not a boxed ptr.
                                 * Only call rt_unbox_int when the arg is actually
                                 * a pointer (closure/polymorphic ABI).           */
                                LLVMTypeRef actual_t = LLVMTypeOf(boxed);
                                if (actual_t == ptr_t) {
                                    LLVMTypeRef uft = LLVMFunctionType(
                                        LLVMInt64TypeInContext(ctx->context), &ptr_t, 1, 0);
                                    unboxed = LLVMBuildCall2(ctx->builder, uft,
                                                  get_rt_unbox_int(ctx), &boxed, 1, "unboxed");
                                    if (typed_llvm != LLVMInt64TypeInContext(ctx->context))
                                        unboxed = LLVMBuildTrunc(ctx->builder, unboxed,
                                                                 typed_llvm, "unboxed_trunc");
                                } else {
                                    /* Raw integer value — cast to target width directly */
                                    unboxed = (actual_t == typed_llvm)
                                        ? boxed
                                        : LLVMBuildIntCast2(ctx->builder, boxed,
                                                            typed_llvm, 1, "int_cast");
                                }
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

                            // Check HM-inferred type for this param
                            Type *hm_pt = NULL;
                            if (hm_scheme) {
                                Type *t = hm_scheme->type;
                                for (int j = 0; j < i && t && t->kind == TYPE_ARROW; j++)
                                    t = t->arrow_ret;
                                if (t && t->kind == TYPE_ARROW)
                                    hm_pt = t->arrow_param;
                            }

                            bool is_fn_param = hm_pt &&
                                (hm_pt->kind == TYPE_ARROW ||
                                 hm_pt->kind == TYPE_FN);

                            if (is_fn_param) {
                                // Keep as ptr — it's a closure/unknown, don't unbox
                                LLVMValueRef alloca = LLVMBuildAlloca(ctx->builder,
                                                                       ptr_t, env_params[i].name);
                                LLVMBuildStore(ctx->builder, raw_param, alloca);
                                env_insert(stub_env, env_params[i].name,
                                           type_fn(NULL, 0, NULL), alloca);
                            } else {
                                // Scalar param — unbox as int
                                LLVMTypeRef uft = LLVMFunctionType(i64, &ptr_t, 1, 0);
                                LLVMValueRef unboxed = LLVMBuildCall2(ctx->builder, uft,
                                                          get_rt_unbox_int(ctx),
                                                          &raw_param, 1, "stub_ub");
                                LLVMValueRef alloca = LLVMBuildAlloca(ctx->builder,
                                                                       i64, env_params[i].name);
                                LLVMBuildStore(ctx->builder, unboxed, alloca);
                                env_insert(stub_env, env_params[i].name, type_int(), alloca);
                            }
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
                                /* Already a RuntimeValue* — return as-is */
                                ret_value = box_val;
                            } else if (actual == ret_llvm_type && actual != ptr_t) {
                                /* Concrete typed result matches declared return —
                                 * return directly without boxing. This is the
                                 * path taken for TYPE_INT_ARBITRARY (iN) values
                                 * passed through identity-like functions.       */
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
                        if (!ret_value) ret_value = LLVMConstNull(ret_llvm_type);
                        else ret_value = emit_type_cast(ctx, ret_value, ret_llvm_type);
                        LLVMBuildRet(ctx->builder, ret_value);
                    }

          ///// Restore env and builder

                    env_free(ctx->env);
                    ctx->env     = saved_env;
                    ctx->init_fn = saved_init;
                    if (saved_block)
                        LLVMPositionBuilderAtEnd(ctx->builder, saved_block);
                    /* fprintf(stderr, "DEBUG FINAL IR for ‘%s’:\n", var_name); */
                    // LLVMDumpValue(func); // DEBUG

           ///// Emit closure value in outer block (closure ABI only)

                    LLVMValueRef closure_val = NULL;
                    if (use_closure_abi) {
                        LLVMValueRef fn_ptr = LLVMBuildBitCast(ctx->builder, func,
                                                                ptr_t, "fn_ptr");
                        LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
                        LLVMValueRef env_array_ptr;

                        if (captured_count > 0) {
                            LLVMTypeRef  arr_t = LLVMArrayType(ptr_t, captured_count);
                            /* Heap-allocate the env array so it survives across
                             * module boundaries and dlopen'd .so calls          */
                            LLVMValueRef malloc_fn = LLVMGetNamedFunction(ctx->module, "malloc");
                            if (!malloc_fn) {
                                LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);
                                LLVMTypeRef mft = LLVMFunctionType(ptr_t, &i64_t, 1, 0);
                                malloc_fn = LLVMAddFunction(ctx->module, "malloc", mft);
                                LLVMSetLinkage(malloc_fn, LLVMExternalLinkage);
                            }
                            LLVMTypeRef  i64_t    = LLVMInt64TypeInContext(ctx->context);
                            LLVMValueRef arr_size = LLVMSizeOf(arr_t);
                            LLVMValueRef heap_ptr = LLVMBuildCall2(ctx->builder,
                                LLVMFunctionType(ptr_t, &i64_t, 1, 0),
                                malloc_fn, &arr_size, 1, "clo_env_heap");
                            LLVMValueRef arr = LLVMBuildBitCast(ctx->builder,
                                heap_ptr, LLVMPointerType(arr_t, 0), "clo_env");
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
                                    ret_type, func, lambda->lambda.docstring, NULL);
                    if (hm_scheme) env_set_scheme(ctx->env, var_name, hm_scheme);
                    EnvEntry *efinal = env_lookup(ctx->env, var_name);
                    if (efinal) {
                        efinal->lifted_count   = 0;
                        efinal->is_closure_abi = use_closure_abi;
                        // Clone the lambda AFTER body desugaring so source_ast
                        // contains the expanded if-chain, not the raw AST_PMATCH.
                        efinal->source_ast     = ast_clone(lambda);
                    }

                    if (lambda->lambda.alias_name) {
                        const char *alias_sym = lambda->lambda.alias_name;
                        env_insert_func(ctx->env, alias_sym,
                                        clone_params(env_params, total_params),
                                        total_params, type_clone(ret_type),
                                        func, lambda->lambda.docstring, NULL);
                        if (hm_scheme)
                            env_set_scheme(ctx->env, alias_sym, hm_scheme);
                        EnvEntry *alias_e = env_lookup(ctx->env, alias_sym);
                        if (alias_e) {
                            alias_e->is_closure_abi = use_closure_abi;
                            alias_e->lifted_count   = 0;
                            alias_e->source_ast     = ast_clone(lambda);
                            alias_e->func_ref       = func;
                            alias_e->llvm_name      = strdup(LLVMGetValueName(func));
                        }

                        /* For non-ASCII alias names, also register under
                         * a mangled ASCII name so LLVM JIT can find it   */
                        char *mangled = mangle_unicode_name(alias_sym);
                        if (mangled) {
                            /* Create a true LLVM alias pointing to the same function */
                            LLVMValueRef mfn = LLVMGetNamedFunction(ctx->module, mangled);
                            if (!mfn) {
                                LLVMTypeRef mft = LLVMGlobalGetValueType(func);
                                /* Use LLVMAddAlias so it shares the exact same code */
                                mfn = LLVMAddAlias2(ctx->module, mft, 0, func, mangled);
                                if (!mfn) {
                                    /* Fallback: declare extern pointing to func */
                                    mfn = LLVMAddFunction(ctx->module, mangled, mft);
                                    LLVMSetLinkage(mfn, LLVMExternalLinkage);
                                }
                            }
                            env_insert_func(ctx->env, mangled,
                                            clone_params(env_params, total_params),
                                            total_params, type_clone(ret_type),
                                            func, lambda->lambda.docstring, NULL);
                            if (hm_scheme)
                                env_set_scheme(ctx->env, mangled, hm_scheme);
                            EnvEntry *mangled_e = env_lookup(ctx->env, mangled);
                            if (mangled_e) {
                                mangled_e->is_closure_abi = use_closure_abi;
                                mangled_e->lifted_count   = 0;
                                mangled_e->func_ref       = func;
                                mangled_e->llvm_name      = strdup(LLVMGetValueName(func));
                            }
                            free(mangled);
                        }
                        printf("ALIAS: %s -> %s\n", alias_sym, var_name);
                    }

                    if (strncmp(var_name, "__hof_lambda_", 13) != 0 &&
                        strncmp(var_name, "__anon_", 7) != 0)
                        check_predicate_name(ctx, var_name, ret_type, ast);

                    bool is_private = ctx->module_ctx &&
                                      !should_export_symbol(ctx->module_ctx, var_name);
                    print_defined(var_name, ret_type, ctx->env, is_private);

                    free(param_types);

                    // Return closure value or dummy for typed functions

                    if (use_closure_abi && closure_val) {
                        if (self_alloca) {
                            LLVMBuildStore(ctx->builder, closure_val, self_alloca);
                        } else if (ctx->current_function_name != NULL) {
                            LLVMValueRef alloca = LLVMBuildAlloca(ctx->builder, ptr_t, var_name);
                            LLVMBuildStore(ctx->builder, closure_val, alloca);
                            env_insert(ctx->env, var_name, type_fn(NULL, 0, NULL), alloca);
                            /* Store func_ref on the VAR entry so direct calls work */
                            EnvEntry *clo_var = env_lookup(ctx->env, var_name);
                            if (clo_var) {
                                clo_var->func_ref       = func;
                                clo_var->is_closure_abi = true;
                                clo_var->param_count    = total_params;
                                clo_var->params         = clone_params(env_params, total_params);
                                clo_var->return_type    = type_clone(ret_type);
                            }
                        }
                        for (int i = 0; i < captured_count; i++) free(captured_vars[i]);
                        free(captured_vars);
                        result.type  = type_fn(NULL, 0, NULL);
                        result.value = closure_val;
                        return result;
                    }

                    for (int i = 0; i < captured_count; i++) free(captured_vars[i]);
                    free(captured_vars);
                    result.value = codegen_current_fn_zero_value(ctx);
                    result.type  = type_unknown();
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

                if (explicit_type && explicit_type->kind == TYPE_UNKNOWN)
                    explicit_type = NULL;
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
                    /* Non-empty array literal — at top level, promote to a
                     * zero-initialised global and copy elements in via stores.
                     * This keeps the alloca alive across REPL modules.        */
                    if (is_at_top_level(ctx)) {
                        LLVMTypeRef arr_type = type_to_llvm(ctx, final_type);
                        LLVMTypeRef  gv_type = final_type->arr_is_fat
                            ? LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0)
                            : arr_type;
                        LLVMValueRef gv = LLVMGetNamedGlobal(ctx->module, var_name);
                        if (!gv) {
                            gv = LLVMAddGlobal(ctx->module, gv_type, var_name);
                            LLVMSetInitializer(gv, LLVMConstNull(gv_type));
                            LLVMSetLinkage(gv, LLVMExternalLinkage);
                        }
                        /* Fat pointer — store the fat struct pointer as a global i8* */
                        LLVMTypeRef  ptr_t = LLVMPointerType(
                            LLVMInt8TypeInContext(ctx->context), 0);
                        LLVMValueRef fat_cast = LLVMBuildBitCast(ctx->builder,
                                                                 value_result.value, ptr_t, "fat_cast");
                        LLVMBuildStore(ctx->builder, fat_cast, gv);
                        env_insert(ctx->env, var_name, final_type, gv);
                        EnvEntry *earr = env_lookup(ctx->env, var_name);
                        if (earr) earr->llvm_name = strdup(LLVMGetValueName(gv));
                        print_defined(var_name, final_type, ctx->env, false);
                        result.type  = final_type;
                        result.value = gv;
                        return result;
                    }
                    /* Inside a function — keep on stack as before */
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
                        LLVMSetLinkage(gv, LLVMInternalLinkage);
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
                /* Coerce integer constant to match annotated integer type width */
                if (final_type->kind == TYPE_U32 || final_type->kind == TYPE_I32) {
                    LLVMTypeRef target_t = type_to_llvm(ctx, final_type);
                    if (LLVMTypeOf(stored_value) != target_t) {
                        if (type_is_float(inferred_type))
                            stored_value = LLVMBuildFPToSI(ctx->builder, stored_value, target_t, "toi32");
                        else
                            stored_value = LLVMBuildIntCast2(ctx->builder, stored_value, target_t, 0, "toi32");
                    }
                } else if (final_type->kind == TYPE_F32) {
                    LLVMTypeRef f32_t = LLVMFloatTypeInContext(ctx->context);
                    if (LLVMTypeOf(stored_value) != f32_t) {
                        if (type_is_float(inferred_type))
                            stored_value = LLVMBuildFPTrunc(ctx->builder, stored_value, f32_t, "tof32");
                        else if (type_is_integer(inferred_type))
                            stored_value = LLVMBuildSIToFP(ctx->builder, stored_value, f32_t, "tof32");
                    }
                } else if (type_is_integer(final_type) && type_is_float(inferred_type)) {
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

                /* Only strdup string literals and string variables — never
                 * strdup pointers returned from FFI calls (e.g. char**).
                 * An FFI call result has inferred type UNKNOWN or PTR;
                 * a real string comes from AST_STRING or a TYPE_STRING var. */
                bool should_strdup = (final_type->kind == TYPE_STRING && stored_value);
                if (should_strdup) {
                    bool from_literal  = (value_expr->type == AST_STRING);
                    bool from_str_var  = (value_expr->type == AST_SYMBOL &&
                                         inferred_type && inferred_type->kind == TYPE_STRING);
                    bool from_ffi_call = (value_expr->type == AST_LIST &&
                                         inferred_type && (inferred_type->kind == TYPE_UNKNOWN ||
                                                           inferred_type->kind == TYPE_PTR));
                    if (from_ffi_call) should_strdup = false;
                    if (!from_literal && !from_str_var && !from_ffi_call) should_strdup = false;
                }
                if (should_strdup) {
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
                        LLVMSetLinkage(var, LLVMInternalLinkage);
                    }
                    LLVMBuildStore(ctx->builder, stored_value, var);
                } else {
                    var = LLVMBuildAlloca(ctx->builder, llvm_type, var_name);
                    LLVMBuildStore(ctx->builder, stored_value, var);
                }

                env_insert(ctx->env, var_name, final_type, var);
                EnvEntry *evar = env_lookup(ctx->env, var_name);
                if (evar) {
                    evar->llvm_name = strdup(LLVMGetValueName(var));
                    /* Store the literal AST for compile-time refinement checks */
                    if (value_expr->type == AST_STRING ||
                        value_expr->type == AST_NUMBER)
                        evar->source_ast = ast_clone(value_expr);
                }

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
                        const char *alias_sym = alias_node->symbol;
                        /* Check if alias contains non-ASCII — if so, declare an
                         * ASCII-mangled global that mirrors the original var     */
                        bool is_ascii = true;
                        for (const char *cp = alias_sym; *cp; cp++) {
                            if ((unsigned char)*cp > 127) { is_ascii = false; break; }
                        }
                        if (!is_ascii) {
                            char *mangled = mangle_unicode_name(alias_sym);
                            LLVMValueRef alias_gv = LLVMGetNamedGlobal(ctx->module, mangled);
                            if (!alias_gv) {
                                alias_gv = LLVMAddGlobal(ctx->module, llvm_type, mangled);
                                LLVMSetLinkage(alias_gv, LLVMExternalLinkage);
                                LLVMSetInitializer(alias_gv, LLVMConstNull(llvm_type));
                            }
                            LLVMBuildStore(ctx->builder, stored_value, alias_gv);
                            /* Point alias env entry to the SAME var as original */
                            env_insert(ctx->env, alias_sym, type_clone(final_type), var);
                            EnvEntry *alias_e = env_lookup(ctx->env, alias_sym);
                            if (alias_e) alias_e->llvm_name = strdup(LLVMGetValueName(var));
                            env_insert(ctx->env, mangled, type_clone(final_type), var);
                            EnvEntry *mangled_e = env_lookup(ctx->env, mangled);
                            if (mangled_e) mangled_e->llvm_name = strdup(LLVMGetValueName(var));
                            free(mangled);
                        } else {
                            env_insert(ctx->env, alias_sym, type_clone(final_type), var);
                            EnvEntry *alias_e = env_lookup(ctx->env, alias_sym);
                            if (alias_e) alias_e->llvm_name = strdup(LLVMGetValueName(var));
                        }
                        printf("ALIAS: %s -> %s\n", alias_sym, var_name);
                    }
                }

                bool is_private = ctx->module_ctx &&
                                  !should_export_symbol(ctx->module_ctx, var_name);
                print_defined(var_name, final_type, ctx->env, is_private);

                result.type  = final_type;
                result.value = stored_value;
                return result;
            }

            if (strcmp(head->symbol, "include") == 0) {
                if (ast->list.count < 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: malformed include",
                                  parser_get_filename(), ast->line, ast->column);
                }
                const char *header = ast->list.items[1]->string;
                bool system_inc    = (strcmp(ast->list.items[2]->symbol,
                                             "system") == 0);
                if (!ctx->ffi) {
                    ctx->ffi = ffi_context_create();
                }
                /* Load :unprefix prefixes (items 3+) onto the FFI context */
                for (int i = 0; i < ctx->ffi->strip_prefix_count; i++)
                    free(ctx->ffi->strip_prefixes[i]);
                ctx->ffi->strip_prefix_count = 0;
                int extra = (int)ast->list.count - 3;
                if (extra > 0) {
                    ctx->ffi->strip_prefixes = realloc(ctx->ffi->strip_prefixes,
                                                       sizeof(char *) * extra);
                    for (int i = 0; i < extra; i++) {
                        ctx->ffi->strip_prefixes[ctx->ffi->strip_prefix_count++] =
                            strdup(ast->list.items[3 + i]->string);
                    }
                }
                bool ok = ffi_parse_header(ctx->ffi, header, system_inc);
                if (!ok) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: failed to parse "
                                  "header '%s'",
                                  parser_get_filename(), ast->line, ast->column,
                                  header);
                }
                ffi_inject_into_env(ctx->ffi, ctx);
                result.type  = type_int();
                result.value = LLVMConstInt(
                    LLVMInt64TypeInContext(ctx->context), 0, 0);
                return result;
            }

            // Type predicates: Int?, Float?, Boolean?, ...
            {
                struct { const char *pred; int tag; int tag2; } type_preds[] = {
                    {"Int?", RT_INT, -1}, {"Float?", RT_FLOAT, -1}, {"Number?", RT_INT, RT_FLOAT}, {"Char?", RT_CHAR, -1}, {"String?", RT_STRING, -1}, {"Bool?", -3, -1}, {"Symbol?", RT_SYMBOL, -1}, {"Keyword?", RT_KEYWORD, -1}, {"List?", RT_LIST, -1}, {"Ratio?", RT_RATIO, -1}, {"Set?", RT_SET, -1}, {"Map?", RT_MAP, -1}, {"Fn?", RT_CLOSURE, -1}, {"Hex?", RT_INT, -1}, {"Bin?", RT_INT, -1}, {"Oct?", RT_INT, -1}, {"Arr?", -2, -1}, {NULL, 0, 0}
                };
                for (int _pi = 0; type_preds[_pi].pred; _pi++) {
                    if (strcmp(head->symbol, type_preds[_pi].pred) != 0) continue;
                    REQUIRE_ARGS(1);
                    CodegenResult arg = codegen_expr(ctx, ast->list.items[1]);
                    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context), i1 = LLVMInt1TypeInContext(ctx->context), ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    if (arg.type && arg.type->kind != TYPE_UNKNOWN) {
                        bool m = false; int t = type_preds[_pi].tag, t2 = type_preds[_pi].tag2;
                        if (t == RT_INT) m = type_is_integer(arg.type) || arg.type->kind == TYPE_BOOL || (t2 == RT_FLOAT && type_is_float(arg.type));
                        else if (t == RT_FLOAT) m = type_is_float(arg.type);
                        else if (t == RT_CHAR) m = arg.type->kind == TYPE_CHAR;
                        else if (t == RT_STRING) m = arg.type->kind == TYPE_STRING;
                        else if (t == RT_SYMBOL) m = arg.type->kind == TYPE_SYMBOL;
                        else if (t == RT_KEYWORD) m = arg.type->kind == TYPE_KEYWORD;
                        else if (t == RT_LIST) m = arg.type->kind == TYPE_LIST;
                        else if (t == RT_RATIO) m = arg.type->kind == TYPE_RATIO;
                        else if (t == RT_SET) m = arg.type->kind == TYPE_SET;
                        else if (t == RT_MAP) m = arg.type->kind == TYPE_MAP;
                        else if (t == RT_CLOSURE) m = arg.type->kind == TYPE_FN;
                        else if (t == -2) m = arg.type->kind == TYPE_ARR;
                        else if (t == -3) m = arg.type->kind == TYPE_BOOL;
                        result.value = LLVMConstInt(i1, m, 0); result.type = type_bool(); return result;
                    }
                    if (type_preds[_pi].tag < 0) { result.value = LLVMConstInt(i1, 0, 0); result.type = type_bool(); return result; }
                    LLVMValueRef val = LLVMTypeOf(arg.value) != ptr ? LLVMBuildBitCast(ctx->builder, arg.value, ptr, "rv_ptr") : arg.value;
                    LLVMValueRef tag_ptr = LLVMBuildGEP2(ctx->builder, LLVMInt8TypeInContext(ctx->context), val, (LLVMValueRef[]){LLVMConstInt(i32, 0, 0)}, 1, "tp");
                    LLVMValueRef tag = LLVMBuildLoad2(ctx->builder, i32, LLVMBuildBitCast(ctx->builder, tag_ptr, LLVMPointerType(i32, 0), "tp32"), "tag");
                    LLVMValueRef cmp = LLVMBuildICmp(ctx->builder, LLVMIntEQ, tag, LLVMConstInt(i32, type_preds[_pi].tag, 0), "eq");
                    if (type_preds[_pi].tag2 >= 0) cmp = LLVMBuildOr(ctx->builder, cmp, LLVMBuildICmp(ctx->builder, LLVMIntEQ, tag, LLVMConstInt(i32, type_preds[_pi].tag2, 0), "eq2"), "or");
                    result.value = cmp; result.type = type_bool(); return result;
                }
            }

            if (strcmp(head->symbol, "nil?") == 0) {
                REQUIRE_ARGS(1);
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

                    LLVMValueRef vl_fn = get_rt_value_list(ctx);
                    LLVMTypeRef  vl_pt = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef  vl_ft = LLVMFunctionType(vl_pt, &vl_pt, 1, 0);
                    LLVMValueRef vl_a[] = {list};
                    result.value = LLVMBuildCall2(ctx->builder, vl_ft, vl_fn, vl_a, 1, "quoted_list");
                    result.type = type_list(NULL);
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

            /* ---- null-coalescing (?? a b) -------------------------------- */
            if (strcmp(head->symbol, "??") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: '?\?' requires exactly 2 arguments",
                                  parser_get_filename(), ast->line, ast->column);
                }

                ctx->in_coalesce_depth++;
                CodegenResult lhs = codegen_expr(ctx, ast->list.items[1]);
                ctx->in_coalesce_depth--;

                /* If lhs is a scalar, it can never be nil. Just return lhs directly. */
                if (lhs.type && (lhs.type->kind == TYPE_INT ||
                                 lhs.type->kind == TYPE_FLOAT ||
                                 lhs.type->kind == TYPE_BOOL ||
                                 lhs.type->kind == TYPE_CHAR)) {
                    return lhs;
                }

                LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef val_to_check = lhs.value;

                /* Ensure we are passing a pointer to rt_value_is_nil */
                if (LLVMTypeOf(val_to_check) != ptr_t) {
                    if (LLVMGetTypeKind(LLVMTypeOf(val_to_check)) == LLVMIntegerTypeKind) {
                        val_to_check = LLVMBuildIntToPtr(ctx->builder, val_to_check, ptr_t, "nil_cast");
                    } else {
                        val_to_check = LLVMBuildBitCast(ctx->builder, val_to_check, ptr_t, "nil_cast");
                    }
                }

                LLVMValueRef is_nil_fn = get_rt_value_is_nil(ctx);
                LLVMValueRef args[] = { val_to_check };
                LLVMValueRef is_nil = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(is_nil_fn), is_nil_fn, args, 1, "is_nil");

                LLVMValueRef is_nil_bool = LLVMBuildICmp(ctx->builder, LLVMIntNE, is_nil, LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0), "is_nil_bool");

                LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(ctx->context, LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder)), "coalesce_then");
                LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(ctx->context, LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder)), "coalesce_else");
                LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(ctx->context, LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder)), "coalesce_merge");

                LLVMBuildCondBr(ctx->builder, is_nil_bool, then_bb, else_bb);

                // Then block (lhs is nil, evaluate rhs)
                LLVMPositionBuilderAtEnd(ctx->builder, then_bb);
                CodegenResult rhs = codegen_expr(ctx, ast->list.items[2]);
                LLVMBasicBlockRef then_end_bb = LLVMGetInsertBlock(ctx->builder); // rhs evaluation might have created blocks

                // Else block (lhs is not nil, return lhs unwrapped)
                LLVMPositionBuilderAtEnd(ctx->builder, else_bb);

                /* Unbox the LHS if the RHS is a primitive scalar type so they unify in the PHI node */
                LLVMValueRef unwrapped_lhs = lhs.value;
                if (rhs.type && type_is_integer(rhs.type)) {
                    LLVMValueRef unbox_fn = get_rt_unbox_int(ctx);
                    unwrapped_lhs = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(unbox_fn), unbox_fn, &val_to_check, 1, "unbox_lhs_int");
                } else if (rhs.type && type_is_float(rhs.type)) {
                    LLVMValueRef unbox_fn = get_rt_unbox_float(ctx);
                    unwrapped_lhs = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(unbox_fn), unbox_fn, &val_to_check, 1, "unbox_lhs_float");
                }

                LLVMBuildBr(ctx->builder, merge_bb);

                // Merge block
                LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);

                // Ensure rhs value matches the phi target type
                LLVMValueRef rhs_val = rhs.value;
                LLVMTypeRef target_llvm_type = LLVMTypeOf(unwrapped_lhs);
                if (LLVMTypeOf(rhs_val) != target_llvm_type) {
                    LLVMPositionBuilderAtEnd(ctx->builder, then_end_bb);
                    rhs_val = emit_type_cast(ctx, rhs_val, target_llvm_type);
                    LLVMBuildBr(ctx->builder, merge_bb);
                    LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
                } else {
                    LLVMPositionBuilderAtEnd(ctx->builder, then_end_bb);
                    LLVMBuildBr(ctx->builder, merge_bb);
                    LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
                }

                LLVMValueRef phi = LLVMBuildPhi(ctx->builder, target_llvm_type, "coalesce_tmp");
                LLVMValueRef incoming_values[] = { rhs_val, unwrapped_lhs };
                LLVMBasicBlockRef incoming_blocks[] = { then_end_bb, else_bb };
                LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);

                result.value = phi;
                result.type = rhs.type; // Unwrapped type
                return result;
            }


            if (strcmp(head->symbol, "expand") == 0) {
                REQUIRE_MIN_ARGS(1);
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

            if (strcmp(head->symbol, "show") == 0) {
                REQUIRE_MIN_ARGS(1);
                LLVMValueRef printf_fn = get_or_declare_printf(ctx);
                AST *arg = ast->list.items[1];

                // Variadic: (show "format _ string" val1 val2 ...)
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
                    if (strcmp(arg->symbol, "nil") == 0) {
                        LLVMValueRef nil_fn = get_rt_value_nil(ctx);
                        LLVMValueRef nil_val = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(nil_fn), nil_fn, NULL, 0, "nil");
                        LLVMValueRef print_fn = get_rt_print_value_newline(ctx);
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(print_fn), print_fn, &nil_val, 1, "");
                        result.type  = type_float();
                        result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                        return result;
                    }
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

                    if (entry->type && entry->type->kind == TYPE_ARR) {
                        /* Fat pointer path */
                        if (entry->type->arr_is_fat) {
                            LLVMTypeRef fat_pt = type_to_llvm(ctx, entry->type);
                            LLVMValueRef fat = LLVMBuildLoad2(ctx->builder,
                                                fat_pt, entry->value, "fat");
                            LLVMValueRef sz  = arr_fat_size(ctx, fat);
                            Type *et         = entry->type->arr_element_type
                                             ? entry->type->arr_element_type : type_int();
                            LLVMValueRef dp  = arr_fat_data(ctx, fat, et);
                            LLVMTypeRef  el  = type_to_llvm(ctx, et);
                            LLVMValueRef pf2 = get_or_declare_printf(ctx);
                            LLVMValueRef open2 = LLVMBuildGlobalStringPtr(ctx->builder, "[", "op");
                            LLVMValueRef oa2[] = {open2};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(pf2), pf2, oa2, 1, "");
                            /* Runtime loop to print elements */
                            LLVMValueRef func2 = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                            LLVMValueRef iptr  = LLVMBuildAlloca(ctx->builder, LLVMInt64TypeInContext(ctx->context), "si");
                            LLVMBuildStore(ctx->builder, LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0), iptr);
                            LLVMBasicBlockRef scond = LLVMAppendBasicBlockInContext(ctx->context, func2, "sc");
                            LLVMBasicBlockRef sbody = LLVMAppendBasicBlockInContext(ctx->context, func2, "sb");
                            LLVMBasicBlockRef safter= LLVMAppendBasicBlockInContext(ctx->context, func2, "sa");
                            LLVMBuildBr(ctx->builder, scond);
                            LLVMPositionBuilderAtEnd(ctx->builder, scond);
                            LLVMValueRef si   = LLVMBuildLoad2(ctx->builder, LLVMInt64TypeInContext(ctx->context), iptr, "si");
                            LLVMValueRef scmp = LLVMBuildICmp(ctx->builder, LLVMIntSLT, si, sz, "sc");
                            LLVMBuildCondBr(ctx->builder, scmp, sbody, safter);
                            LLVMPositionBuilderAtEnd(ctx->builder, sbody);
                            LLVMValueRef si2  = LLVMBuildLoad2(ctx->builder, LLVMInt64TypeInContext(ctx->context), iptr, "si2");
                            LLVMValueRef ep2  = LLVMBuildGEP2(ctx->builder, el, dp, &si2, 1, "sep");
                            LLVMValueRef ev2  = LLVMBuildLoad2(ctx->builder, el, ep2, "sev");
                            LLVMValueRef spfmt = type_is_float(et)
                                ? get_fmt_float_no_newline(ctx)
                                : get_fmt_int_no_newline(ctx);
                            if (type_is_float(et)) {
                                LLVMValueRef sa2[] = {spfmt, ev2};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(pf2), pf2, sa2, 2, "");
                            } else {
                                LLVMTypeRef i64t = LLVMInt64TypeInContext(ctx->context);
                                LLVMValueRef ext2 = LLVMTypeOf(ev2) != i64t
                                    ? LLVMBuildZExt(ctx->builder, ev2, i64t, "wi") : ev2;
                                LLVMValueRef sa2[] = {spfmt, ext2};
                                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(pf2), pf2, sa2, 2, "");
                            }
                            LLVMValueRef snext = LLVMBuildAdd(ctx->builder, si2, LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 1, 0), "sn");
                            LLVMBuildStore(ctx->builder, snext, iptr);
                            LLVMBuildBr(ctx->builder, scond);
                            LLVMPositionBuilderAtEnd(ctx->builder, safter);
                            LLVMValueRef close2 = LLVMBuildGlobalStringPtr(ctx->builder, "]\n", "cl");
                            LLVMValueRef ca2[] = {close2};
                            LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(pf2), pf2, ca2, 1, "");
                            result.type  = type_float();
                            result.value = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                            return result;
                        }
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
                    if (entry->type->kind == TYPE_OPTIONAL || entry->type->kind == TYPE_NIL) {
                        LLVMValueRef print_fn = get_rt_print_value_newline(ctx);
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(print_fn), print_fn, &loaded, 1, "");
                    } else {
                        codegen_show_value(ctx, loaded, entry->type, true);
                    }

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
                if (arg_result.type) {
                    if (arg_result.type->kind == TYPE_OPTIONAL || arg_result.type->kind == TYPE_NIL) {
                        LLVMValueRef print_fn = get_rt_print_value_newline(ctx);
                        LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(print_fn), print_fn, &arg_result.value, 1, "");
                    } else {
                        codegen_show_value(ctx, arg_result.value, arg_result.type, true);
                    }
                }

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
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: no source available for ‘%s’",
                                  parser_get_filename(), ast->line, ast->column, fn_name);
                }
                AST *src_ast = parse(e->source_text);
                if (!src_ast) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: failed to parse source for ‘%s’",
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
                REQUIRE_ARGS(3);
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
                    LLVMTypeRef dbl = LLVMDoubleTypeInContext(ctx->context);
                    if (LLVMTypeOf(rhs) == ptr) {
                        LLVMTypeRef _uft = LLVMFunctionType(dbl, &ptr, 1, 0);
                        rhs = LLVMBuildCall2(ctx->builder, _uft,
                                             get_rt_unbox_float(ctx), &rhs, 1, "unbox_rhs_f");
                    } else if (LLVMTypeOf(rhs) != dbl) {
                        rhs = LLVMBuildSIToFP(ctx->builder, rhs, dbl, "rhs_to_float");
                    }
                    if (LLVMTypeOf(lhs) != dbl) {
                        lhs = LLVMBuildSIToFP(ctx->builder, lhs, dbl, "lhs_to_float");
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
                    LLVMTypeRef  i64_t    = LLVMInt64TypeInContext(ctx->context);
                    LLVMTypeRef  ptr_t    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    /* Build "Assertion failed: <label>" message */
                    LLVMValueRef snprintf_fn = LLVMGetNamedFunction(ctx->module, "snprintf");
                    if (!snprintf_fn) {
                        LLVMTypeRef sp[] = {ptr_t, i64_t, ptr_t};
                        LLVMTypeRef sft  = LLVMFunctionType(
                            LLVMInt32TypeInContext(ctx->context), sp, 3, true);
                        snprintf_fn = LLVMAddFunction(ctx->module, "snprintf", sft);
                        LLVMSetLinkage(snprintf_fn, LLVMExternalLinkage);
                    }
                    LLVMValueRef msg_buf = LLVMBuildArrayAlloca(ctx->builder,
                                                                LLVMInt8TypeInContext(ctx->context),
                                                                LLVMConstInt(i64_t, 512, 0), "assert_msg");
                    LLVMValueRef fmt_str = LLVMBuildGlobalStringPtr(ctx->builder,
                                                                    "Assertion failed: %s", "assert_fmt");
                    LLVMValueRef sp_args[] = {msg_buf, LLVMConstInt(i64_t, 512, 0),
                        fmt_str, label_val};
                    LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(snprintf_fn),
                                   snprintf_fn, sp_args, 4, "");
                    emit_runtime_error_val(ctx, ast, msg_buf);
                }

                // ── Continue ──────────────────────────────────────────────────────
                LLVMPositionBuilderAtEnd(ctx->builder, cont_bb);
                result.type  = type_bool();
                result.value = cond;
                return result;
            }

            if (strcmp(head->symbol, "error") == 0) {
                REQUIRE_MIN_ARGS(1);
                LLVMValueRef printf_fn = get_or_declare_printf(ctx);
                LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i64_t = LLVMInt64TypeInContext(ctx->context);

                /* Evaluate the message argument */
                CodegenResult msg_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef  msg_val;

                if (msg_r.type && msg_r.type->kind == TYPE_STRING) {
                    msg_val = msg_r.value;
                } else {
                    /* Non-string: coerce to something printable */
                    msg_val = LLVMBuildGlobalStringPtr(ctx->builder,
                                                       "(error called)", "err_fallback");
                }

                /* Print: "<file>:<line>:<col>: error: <msg>\n" */
                char location[256];
                snprintf(location, sizeof(location), "%s:%d:%d: error: ",
                         parser_get_filename(), ast->line, ast->column);
                LLVMValueRef loc_str = LLVMBuildGlobalStringPtr(ctx->builder,
                                                                location, "err_loc");
                LLVMValueRef newline = LLVMBuildGlobalStringPtr(ctx->builder,
                                                                "\n", "err_nl");

                /* Print location prefix */
                LLVMValueRef loc_args[] = {loc_str};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                               printf_fn, loc_args, 1, "");

                /* Print user message */
                LLVMValueRef fmt_s = LLVMBuildGlobalStringPtr(ctx->builder,
                                                              "%s", "err_fmt");
                LLVMValueRef msg_args[] = {fmt_s, msg_val};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                               printf_fn, msg_args, 2, "");

                /* Print newline */
                LLVMValueRef nl_args[] = {newline};
                LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(printf_fn),
                               printf_fn, nl_args, 1, "");

                /* Call abort() */
                LLVMValueRef abort_fn = LLVMGetNamedFunction(ctx->module, "abort");
                if (!abort_fn) {
                    LLVMTypeRef abort_ft = LLVMFunctionType(
                        LLVMVoidTypeInContext(ctx->context), NULL, 0, 0);
                    abort_fn = LLVMAddFunction(ctx->module, "abort", abort_ft);
                    LLVMSetLinkage(abort_fn, LLVMExternalLinkage);
                }
                LLVMValueRef exit_fn = LLVMGetNamedFunction(ctx->module, "exit");
                if (!exit_fn) {
                    LLVMTypeRef exit_param = LLVMInt32TypeInContext(ctx->context);
                    LLVMTypeRef exit_ft = LLVMFunctionType(
                        LLVMVoidTypeInContext(ctx->context), &exit_param, 1, 0);
                    exit_fn = LLVMAddFunction(ctx->module, "exit", exit_ft);
                    LLVMSetLinkage(exit_fn, LLVMExternalLinkage);
                }
                LLVMTypeRef exit_param = LLVMInt32TypeInContext(ctx->context);
                LLVMTypeRef exit_ft = LLVMFunctionType(
                    LLVMVoidTypeInContext(ctx->context), &exit_param, 1, 0);
                LLVMValueRef exit_code = LLVMConstInt(
                    LLVMInt32TypeInContext(ctx->context), 1, 0);
                LLVMBuildCall2(ctx->builder, exit_ft, exit_fn, &exit_code, 1, "");
                LLVMBuildUnreachable(ctx->builder);

                /* Dead block so the builder stays valid after this point */
                LLVMValueRef cur_fn = LLVMGetBasicBlockParent(
                                          LLVMGetInsertBlock(ctx->builder));
                LLVMBasicBlockRef dead_bb = LLVMAppendBasicBlockInContext(
                    ctx->context, cur_fn, "error_dead");
                LLVMPositionBuilderAtEnd(ctx->builder, dead_bb);

                /* Return type is unknown so it unifies with any branch */
                result.value = LLVMConstPointerNull(ptr_t);
                result.type  = type_unknown();
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
                         "%s:%d:%d: error: called undefined function ‘%s’\n",
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

                /* The value produced here feeds into a PHI node in the enclosing
                 * if-merge block. It must match whatever type the other branches
                 * produce. Since we can't know that here, we produce typed null
                 * values for all common types and let the if-codegen coerce.
                 * The key insight: return TYPE_UNKNOWN with a ptr null so the
                 * if-form treats this branch as ptr and boxes the other side too,
                 * giving a consistent ptr PHI.                                  */
                LLVMTypeRef fn_type2  = LLVMGlobalGetValueType(cur_fn);
                LLVMTypeRef ret_type2 = LLVMGetReturnType(fn_type2);
                LLVMTypeRef ptr_t     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

                LLVMValueRef null_val;
                LLVMTypeRef  null_type;
                if (LLVMGetTypeKind(ret_type2) == LLVMDoubleTypeKind) {
                    null_val  = LLVMConstReal(ret_type2, 0.0);
                    null_type = ret_type2;
                } else if (LLVMGetTypeKind(ret_type2) == LLVMFloatTypeKind) {
                    null_val  = LLVMConstReal(ret_type2, 0.0);
                    null_type = ret_type2;
                } else if (LLVMGetTypeKind(ret_type2) == LLVMIntegerTypeKind) {
                    null_val  = LLVMConstInt(ret_type2, 0, 0);
                    null_type = ret_type2;
                } else {
                    /* ptr or unknown — use null ptr */
                    null_val  = LLVMConstPointerNull(ptr_t);
                    null_type = ptr_t;
                }

                /* Use the actual function return type to determine result type
                 * so the PHI in the enclosing if-merge gets a consistent type */
                result.value = null_val;
                result.type  = LLVMGetTypeKind(null_type) == LLVMDoubleTypeKind ? type_float()
                             : LLVMGetTypeKind(null_type) == LLVMIntegerTypeKind ? type_int()
                             : type_unknown();
                return result;
            }

            if (strcmp(head->symbol, "make-string") == 0) {
                REQUIRE_ARGS(2);
                LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0), i64 = LLVMInt64TypeInContext(ctx->context), i32 = LLVMInt32TypeInContext(ctx->context);
                LLVMValueRef calloc_fn = LLVMGetNamedFunction(ctx->module, "calloc");
                if (!calloc_fn) { calloc_fn = LLVMAddFunction(ctx->module, "calloc", LLVMFunctionType(ptr, (LLVMTypeRef[]){i64, i64}, 2, 0)); LLVMSetLinkage(calloc_fn, LLVMExternalLinkage); }
                LLVMValueRef memset_fn = LLVMGetNamedFunction(ctx->module, "memset");
                if (!memset_fn) { memset_fn = LLVMAddFunction(ctx->module, "memset", LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr, i32, i64}, 3, 0)); LLVMSetLinkage(memset_fn, LLVMExternalLinkage); }

                CodegenResult len_r = codegen_expr(ctx, ast->list.items[1]), fill_r = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef len = type_is_float(len_r.type) ? LLVMBuildFPToSI(ctx->builder, len_r.value, i64, "len") : len_r.value;
                LLVMValueRef len1 = LLVMBuildAdd(ctx->builder, len, LLVMConstInt(i64, 1, 0), "len1");
                LLVMValueRef buf = emit_call_2(ctx, calloc_fn, ptr, len1, LLVMConstInt(i64, 1, 0), "buf");
                emit_call_3(ctx, memset_fn, ptr, buf, LLVMBuildZExt(ctx->builder, fill_r.value, i32, "fill"), len, "");
                result.value = buf; result.type = type_string(); return result;
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

            if (strcmp(head->symbol, "cons") == 0 ||
                strcmp(head->symbol, ".")    == 0)  {
                LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);

                CodegenResult val_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef  boxed  = codegen_box(ctx, val_r.value, val_r.type);

                LLVMValueRef head_thunk_fn   = get_rt_thunk_of_value(ctx);
                LLVMTypeRef  tov_ft          = LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr}, 1, 0);
                LLVMValueRef head_thunk      = LLVMBuildCall2(ctx->builder, tov_ft,
                                                              head_thunk_fn, &boxed, 1, "head_thunk");

                // Collect free variables in the tail expression
                char **free_vars  = NULL;
                int    free_count = 0;
                collect_free_vars(ast->list.items[2], NULL, 0, NULL, 0,
                                  &free_vars, &free_count, ctx->env);

                // Pack free vars into heap env array (boxed)
                LLVMValueRef env_array_ptr = LLVMConstNull(ptr);
                if (free_count > 0) {
                    LLVMValueRef malloc_fn = LLVMGetNamedFunction(ctx->module, "malloc");
                    if (!malloc_fn) {
                        LLVMTypeRef mft = LLVMFunctionType(ptr, &i64, 1, 0);
                        malloc_fn = LLVMAddFunction(ctx->module, "malloc", mft);
                        LLVMSetLinkage(malloc_fn, LLVMExternalLinkage);
                    }
                    LLVMValueRef arr_bytes = LLVMConstInt(i64, sizeof(void*) * free_count, 0);
                    LLVMValueRef heap_arr  = LLVMBuildCall2(ctx->builder,
                                                            LLVMFunctionType(ptr, &i64, 1, 0),
                                                            malloc_fn, &arr_bytes, 1, "tail_env");
                    for (int i = 0; i < free_count; i++) {
                        EnvEntry    *cap_e = env_lookup(ctx->env, free_vars[i]);
                        LLVMValueRef cap_val = LLVMConstPointerNull(ptr);
                        if (cap_e && cap_e->kind == ENV_VAR && cap_e->value) {
                            LLVMValueRef loaded = LLVMBuildLoad2(ctx->builder,
                                                                 type_to_llvm(ctx, cap_e->type),
                                                                 cap_e->value, free_vars[i]);
                            cap_val = codegen_box(ctx, loaded, cap_e->type);
                        }
                        LLVMValueRef idx  = LLVMConstInt(i64, i, 0);
                        LLVMValueRef slot = LLVMBuildGEP2(ctx->builder, ptr,
                                                          heap_arr, &idx, 1, "cap_slot");
                        LLVMBuildStore(ctx->builder, cap_val, slot);
                    }
                    env_array_ptr = heap_arr;
                }

                // Build thunk function
                static int thunk_counter = 0;
                char thunk_name[64];
                snprintf(thunk_name, sizeof(thunk_name), "__cons_tail_thunk_%d", thunk_counter++);

                LLVMTypeRef  thunk_fn_type = LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr}, 1, 0);
                LLVMValueRef thunk_fn      = LLVMAddFunction(ctx->module, thunk_name, thunk_fn_type);
                LLVMSetLinkage(thunk_fn, LLVMInternalLinkage);

                LLVMBasicBlockRef resume_bb   = LLVMGetInsertBlock(ctx->builder);
                LLVMBasicBlockRef thunk_entry = LLVMAppendBasicBlockInContext(
                    ctx->context, thunk_fn, "entry");
                LLVMPositionBuilderAtEnd(ctx->builder, thunk_entry);

                // Unpack captured vars from env param
                LLVMValueRef env_param = LLVMGetParam(thunk_fn, 0);
                Env *saved_env = ctx->env;
                ctx->env = env_create_child(saved_env);

                for (int i = 0; i < free_count; i++) {
                    EnvEntry    *cap_e    = env_lookup(saved_env, free_vars[i]);
                    if (!cap_e) continue;
                    Type        *cap_type = cap_e->type ? cap_e->type : type_unknown();
                    LLVMTypeRef  cap_llvm = type_to_llvm(ctx, cap_type);
                    LLVMValueRef idx      = LLVMConstInt(i64, i, 0);
                    LLVMValueRef slot     = LLVMBuildGEP2(ctx->builder, ptr,
                                                          env_param, &idx, 1, "cap_slot");
                    LLVMValueRef bcap     = LLVMBuildLoad2(ctx->builder, ptr, slot, "bcap");
                    LLVMValueRef unboxed;
                    if (type_is_integer(cap_type)) {
                        unboxed = LLVMBuildCall2(ctx->builder,
                                                 LLVMFunctionType(i64, &ptr, 1, 0),
                                                 get_rt_unbox_int(ctx), &bcap, 1, "ub");
                    } else if (type_is_float(cap_type)) {
                        LLVMTypeRef dbl = LLVMDoubleTypeInContext(ctx->context);
                        unboxed = LLVMBuildCall2(ctx->builder,
                                                 LLVMFunctionType(dbl, &ptr, 1, 0),
                                                 get_rt_unbox_float(ctx), &bcap, 1, "ub");
                    } else if (cap_type->kind == TYPE_CHAR) {
                        LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx->context);
                        unboxed = LLVMBuildCall2(ctx->builder,
                                                 LLVMFunctionType(i8, &ptr, 1, 0),
                                                 get_rt_unbox_char(ctx), &bcap, 1, "ub");
                    } else {
                        unboxed = bcap;
                    }
                    LLVMValueRef alloca = LLVMBuildAlloca(ctx->builder, cap_llvm, free_vars[i]);
                    LLVMBuildStore(ctx->builder, unboxed, alloca);
                    env_insert(ctx->env, free_vars[i], type_clone(cap_type), alloca);
                }

                // Codegen tail — this produces a RuntimeList* (ptr)
                // DO NOT call codegen_box or rt_value_list on it —
                // _force_tail expects the thunk fn to return RuntimeValue* where
                // .type == RT_LIST and .data.list_val == RuntimeList*.
                // codegen_box on TYPE_LIST already calls rt_value_list, so use that
                // directly. The key: tail_r.value is already a RuntimeList* ptr,
                // codegen_box wraps it once into RuntimeValue*(RT_LIST). That is
                // exactly what _force_tail needs. Do NOT wrap again.
                CodegenResult tail_r = codegen_expr(ctx, ast->list.items[2]);

                LLVMValueRef ret_val;
                // tail_r.value is already a RuntimeValue* from codegen_expr (function calls
                // return boxed values directly). Use it as-is — do NOT call rt_value_list again.
                ret_val = tail_r.value;
                LLVMBuildRet(ctx->builder, ret_val);

                env_free(ctx->env);
                ctx->env = saved_env;
                LLVMPositionBuilderAtEnd(ctx->builder, resume_bb);

                // rt_thunk_create(thunk_fn_ptr, env_array_ptr)
                LLVMValueRef tc_fn      = get_rt_thunk_create(ctx);
                LLVMTypeRef  tc_ft      = LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr, ptr}, 2, 0);
                LLVMValueRef tc_call[]  = {thunk_fn, env_array_ptr};
                LLVMValueRef tail_thunk = LLVMBuildCall2(ctx->builder, tc_ft, tc_fn,
                                                         tc_call, 2, "tail_thunk");

                // rt_list_lazy_cons(head_thunk, tail_thunk)
                LLVMValueRef lc_fn     = get_rt_list_lazy_cons(ctx);
                LLVMTypeRef  lc_ft     = LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr, ptr}, 2, 0);
                LLVMValueRef lc_call[] = {head_thunk, tail_thunk};
                LLVMValueRef lazy_cons_raw = LLVMBuildCall2(ctx->builder, lc_ft, lc_fn,
                                                            lc_call, 2, "lazy_cons_raw");
                LLVMValueRef vl_fn = get_rt_value_list(ctx);
                LLVMTypeRef  vl_ft = LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr}, 1, 0);
                result.value = LLVMBuildCall2(ctx->builder, vl_ft, vl_fn,
                                             &lazy_cons_raw, 1, "lazy_cons");
                result.type  = type_unknown();

                for (int i = 0; i < free_count; i++) free(free_vars[i]);
                free(free_vars);
                return result;
            }

            if (strcmp(head->symbol, "pair?") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'pair?' requires 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                CodegenResult val_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef boxed = codegen_box(ctx, val_r.value, val_r.type);

                LLVMValueRef fn = get_rt_is_pair(ctx);
                LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
                LLVMTypeRef ft = LLVMFunctionType(i32, &ptr, 1, 0);

                LLVMValueRef res = LLVMBuildCall2(ctx->builder, ft, fn, &boxed, 1, "is_pair");
                LLVMValueRef bool_val = LLVMBuildICmp(ctx->builder, LLVMIntNE, res, LLVMConstInt(i32, 0, 0), "pair_bool");

                result.type = type_bool();
                result.value = bool_val;
                return result;
            }

            // (head xs) -> first element, optimized per collection type
            if (strcmp(head->symbol, "head") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'head' requires 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);
                CodegenResult col_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef val = col_r.value;
                // 1. Known fat array: GEP index 0 into data pointer
                if (col_r.type && col_r.type->kind == TYPE_ARR && col_r.type->arr_is_fat) {
                    LLVMValueRef data_ptr = arr_fat_data(ctx, val, col_r.type->arr_element_type);
                    LLVMTypeRef  elem_t   = type_to_llvm(ctx, col_r.type->arr_element_type);
                    LLVMValueRef zero     = LLVMConstInt(i64_t, 0, 0);
                    LLVMValueRef gep      = LLVMBuildGEP2(ctx->builder, elem_t, data_ptr, &zero, 1, "arr_head_ptr");
                    result.value = LLVMBuildLoad2(ctx->builder, elem_t, gep, "arr_head");
                    result.type  = col_r.type->arr_element_type;
                    return result;
                }
                // 2. Known fixed-size array: GEP index 0 directly
                if (col_r.type && col_r.type->kind == TYPE_ARR && !col_r.type->arr_is_fat) {
                    LLVMTypeRef  elem_t = type_to_llvm(ctx, col_r.type->arr_element_type);
                    LLVMValueRef zero   = LLVMConstInt(i64_t, 0, 0);
                    LLVMValueRef gep    = LLVMBuildGEP2(ctx->builder, elem_t, val, &zero, 1, "arr_head_ptr");
                    result.value = LLVMBuildLoad2(ctx->builder, elem_t, gep, "arr_head");
                    result.type  = col_r.type->arr_element_type;
                    return result;
                }
                // 3. String: load byte at index 0, return as i64
                if (col_r.type && col_r.type->kind == TYPE_STRING) {
                    LLVMTypeRef  i8_t = LLVMInt8TypeInContext(ctx->context);
                    LLVMValueRef zero  = LLVMConstInt(i64_t, 0, 0);
                    LLVMValueRef gep   = LLVMBuildGEP2(ctx->builder, i8_t, val, &zero, 1, "str_head_ptr");
                    LLVMValueRef ch    = LLVMBuildLoad2(ctx->builder, i8_t, gep, "str_head");
                    result.value = LLVMBuildZExt(ctx->builder, ch, i64_t, "str_head_i64");
                    result.type  = type_int();
                    return result;
                }
                // 4. Known list: call rt_list_car directly
                if (col_r.type && col_r.type->kind == TYPE_LIST) {
                    LLVMTypeRef  ft = LLVMFunctionType(ptr_t, &ptr_t, 1, 0);
                    result.value = LLVMBuildCall2(ctx->builder, ft, get_rt_list_car(ctx), &val, 1, "list_head");
                    result.type  = type_unknown();
                    return result;
                }
                // 5. Unknown/Coll: unbox to list then call rt_list_car
                {
                    LLVMTypeRef  ft_ul = LLVMFunctionType(ptr_t, &ptr_t, 1, 0);
                    LLVMValueRef lst   = LLVMBuildCall2(ctx->builder, ft_ul, get_rt_unbox_list(ctx), &val, 1, "unboxed_list");
                    result.value = LLVMBuildCall2(ctx->builder, ft_ul, get_rt_list_car(ctx), &lst, 1, "coll_head");
                    result.type  = type_unknown();
                    return result;
                }
            }

            // (tail xs) -> collection without first element, optimized per collection type
            if (strcmp(head->symbol, "tail") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'tail' requires 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);
                CodegenResult col_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef val = col_r.value;
                // 1. Known fat array: bump data pointer by 1, decrement size
                if (col_r.type && col_r.type->kind == TYPE_ARR && col_r.type->arr_is_fat) {
                    LLVMTypeRef  fat_t    = get_arr_fat_type(ctx);
                    LLVMTypeRef  elem_t   = type_to_llvm(ctx, col_r.type->arr_element_type);
                    LLVMValueRef data_ptr = arr_fat_data(ctx, val, col_r.type->arr_element_type);
                    LLVMValueRef old_size = arr_fat_size(ctx, val);
                    LLVMValueRef one      = LLVMConstInt(i64_t, 1, 0);
                    LLVMValueRef new_data = LLVMBuildGEP2(ctx->builder, elem_t, data_ptr, &one, 1, "tail_data");
                    LLVMValueRef new_size = LLVMBuildSub(ctx->builder, old_size, one, "tail_size");
                    /* malloc a new fat struct */
                    LLVMValueRef malloc_fn = LLVMGetNamedFunction(ctx->module, "malloc");
                    if (!malloc_fn) {
                        LLVMTypeRef ft = LLVMFunctionType(ptr_t, &i64_t, 1, 0);
                        malloc_fn = LLVMAddFunction(ctx->module, "malloc", ft);
                        LLVMSetLinkage(malloc_fn, LLVMExternalLinkage);
                    }
                    LLVMValueRef fat_sz  = LLVMSizeOf(fat_t);
                    LLVMValueRef fat_raw = LLVMBuildCall2(ctx->builder,
                                              LLVMFunctionType(ptr_t, &i64_t, 1, 0),
                                              malloc_fn, &fat_sz, 1, "tail_fat_raw");
                    LLVMValueRef fat_ptr = LLVMBuildBitCast(ctx->builder, fat_raw,
                                              LLVMPointerType(fat_t, 0), "tail_fat");
                    LLVMValueRef new_data_raw = LLVMBuildBitCast(ctx->builder, new_data, ptr_t, "tail_data_raw");
                    LLVMValueRef df = LLVMBuildStructGEP2(ctx->builder, fat_t, fat_ptr, 0, "tail_df");
                    LLVMBuildStore(ctx->builder, new_data_raw, df);
                    LLVMValueRef sf = LLVMBuildStructGEP2(ctx->builder, fat_t, fat_ptr, 1, "tail_sf");
                    LLVMBuildStore(ctx->builder, new_size, sf);
                    result.value = fat_ptr;
                    result.type  = col_r.type;
                    return result;
                }
                // 2. Known fixed-size array: GEP by 1 into the data (returns pointer into same array)
                if (col_r.type && col_r.type->kind == TYPE_ARR && !col_r.type->arr_is_fat) {
                    LLVMTypeRef  elem_t = type_to_llvm(ctx, col_r.type->arr_element_type);
                    LLVMValueRef one    = LLVMConstInt(i64_t, 1, 0);
                    result.value = LLVMBuildGEP2(ctx->builder, elem_t, val, &one, 1, "arr_tail");
                    result.type  = col_r.type;
                    return result;
                }
                // 3. String: return pointer+1 (tail of C string)
                if (col_r.type && col_r.type->kind == TYPE_STRING) {
                    LLVMTypeRef  i8_t = LLVMInt8TypeInContext(ctx->context);
                    LLVMValueRef one  = LLVMConstInt(i64_t, 1, 0);
                    result.value = LLVMBuildGEP2(ctx->builder, i8_t, val, &one, 1, "str_tail");
                    result.type  = type_string();
                    return result;
                }
                // 4. Known list: call rt_list_cdr directly
                if (col_r.type && col_r.type->kind == TYPE_LIST) {
                    LLVMTypeRef  ft = LLVMFunctionType(ptr_t, &ptr_t, 1, 0);
                    result.value = LLVMBuildCall2(ctx->builder, ft, get_rt_list_cdr(ctx), &val, 1, "list_tail");
                    result.type  = type_list(NULL);
                    return result;
                }
                // 5. Unknown/Coll: unbox to list then call rt_list_cdr
                {
                    LLVMTypeRef  ft_ul = LLVMFunctionType(ptr_t, &ptr_t, 1, 0);
                    LLVMValueRef lst   = LLVMBuildCall2(ctx->builder, ft_ul, get_rt_unbox_list(ctx), &val, 1, "unboxed_list");
                    result.value = LLVMBuildCall2(ctx->builder, ft_ul, get_rt_list_cdr(ctx), &lst, 1, "coll_tail");
                    result.type  = type_list(NULL);
                    return result;
                }
            }

            if (strcmp(head->symbol, "car") == 0) {
                REQUIRE_ARGS(1);
                LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult lr = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef raw = emit_call_1(ctx, get_rt_unbox_list(ctx), ptr_t, codegen_box(ctx, lr.value, lr.type), "car_l");
                result.value = emit_call_1(ctx, get_rt_list_car(ctx), ptr_t, raw, "car");
                result.type = type_unknown(); return result;
            }

            if (strcmp(head->symbol, "cdr") == 0) {
                REQUIRE_ARGS(1);
                LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult lr = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef raw = emit_call_1(ctx, get_rt_unbox_list(ctx), ptr_t, codegen_box(ctx, lr.value, lr.type), "cdr_l");
                result.value = emit_call_1(ctx, get_rt_value_list(ctx), ptr_t, emit_call_1(ctx, get_rt_list_cdr(ctx), ptr_t, raw, "cdr_raw"), "cdr");
                result.type = type_list(NULL); return result;
            }

            if (strcmp(head->symbol, "rt_coll_wrap") == 0) {
                CodegenResult ref = codegen_expr(ctx, ast->list.items[1]), item = codegen_expr(ctx, ast->list.items[2]);
                result.value = emit_call_2(ctx, get_rt_coll_wrap(ctx), LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0), ref.value, codegen_box(ctx, item.value, item.type), "wrapped_val");
                result.type = type_coll(); return result;
            }
            if (strcmp(head->symbol, "rt_coll_empty") == 0) {
                CodegenResult ref = codegen_expr(ctx, ast->list.items[1]);
                result.value = emit_call_1(ctx, get_rt_coll_empty(ctx), LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0), ref.value, "empty_coll");
                result.type = type_coll(); return result;
            }
            if (strcmp(head->symbol, "rt_coll_drop") == 0) {
                CodegenResult coll_r = codegen_expr(ctx, ast->list.items[1]), n_r = codegen_expr(ctx, ast->list.items[2]);
                LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);
                LLVMValueRef n_val = LLVMTypeOf(n_r.value) != i64_t ? LLVMBuildIntCast2(ctx->builder, n_r.value, i64_t, 1, "drop_n") : n_r.value;
                result.value = emit_call_2(ctx, get_rt_coll_drop(ctx), LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0), coll_r.value, n_val, "drop_coll");
                result.type = type_coll(); return result;
            }
            if (strcmp(head->symbol, "rt_coll_is_empty") == 0) {
                LLVMTypeRef i32_t = LLVMInt32TypeInContext(ctx->context);
                CodegenResult ref = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef res = emit_call_1(ctx, get_rt_coll_is_empty(ctx), i32_t, codegen_box(ctx, ref.value, ref.type), "coll_is_empty");
                result.value = LLVMBuildICmp(ctx->builder, LLVMIntNE, res, LLVMConstInt(i32_t, 0, 0), "is_empty_bool");
                result.type = type_bool(); return result;
            }



            if (strcmp(head->symbol, "++") == 0) {
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: '++' requires 2 arguments",
                                  parser_get_filename(), ast->line, ast->column);
                }

                LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                CodegenResult left_r = codegen_expr(ctx, ast->list.items[1]);

                /* String */
                if (left_r.type && left_r.type->kind == TYPE_STRING) {
                    CodegenResult right_r = codegen_expr(ctx, ast->list.items[2]);
                    result.value = LLVMBuildCall2(ctx->builder,
                                                  LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr, ptr}, 2, 0),
                                                  get_rt_string_concat(ctx),
                                                  (LLVMValueRef[]){left_r.value, right_r.value}, 2, "");
                    result.type = type_string();
                    return result;
                }

                /* Array */
                if (left_r.type && left_r.type->kind == TYPE_ARR) {
                    CodegenResult right_r = codegen_expr(ctx, ast->list.items[2]);
                    LLVMTypeRef fat_t  = get_arr_fat_type(ctx);
                    LLVMTypeRef elem_t = type_to_llvm(ctx, left_r.type->arr_element_type);

                    LLVMValueRef l_data    = arr_fat_data(ctx, left_r.value, left_r.type->arr_element_type);
                    LLVMValueRef l_len     = arr_fat_size(ctx, left_r.value);
                    LLVMValueRef r_data    = arr_fat_data(ctx, right_r.value, left_r.type->arr_element_type);
                    LLVMValueRef r_len     = arr_fat_size(ctx, right_r.value);
                    LLVMValueRef elem_size = LLVMSizeOf(elem_t);

                    LLVMValueRef new_data = LLVMBuildCall2(ctx->builder,
                                                           LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr, i64, ptr, i64, i64}, 5, 0),
                                                           get_rt_arr_concat(ctx),
                                                           (LLVMValueRef[]){l_data, l_len, r_data, r_len, elem_size}, 5, "");
                    LLVMValueRef new_len   = LLVMBuildAdd(ctx->builder, l_len, r_len, "");
                    LLVMValueRef new_fat   = LLVMBuildAlloca(ctx->builder, fat_t, "");
                    LLVMValueRef cast_data = LLVMBuildBitCast(ctx->builder, new_data, LLVMPointerType(elem_t, 0), "");
                    LLVMBuildStore(ctx->builder, cast_data,
                                   LLVMBuildStructGEP2(ctx->builder, fat_t, new_fat, 0, ""));
                    LLVMBuildStore(ctx->builder, new_len,
                                   LLVMBuildStructGEP2(ctx->builder, fat_t, new_fat, 1, ""));
                    result.value          = new_fat;
                    result.type           = type_clone(left_r.type);
                    result.type->arr_size = -1;
                    return result;
                }

                /* Coll / List: do NOT evaluate rhs eagerly — it may be recursive.
                 * Capture all ENV_VAR symbols referenced in the rhs AST into a
                 * malloc'd env array, emit a thunk function that re-evaluates rhs
                 * with those locals restored, then call rt_coll_lazy_cons(lhs, thunk). */
                {
#define MAX_CAPS 64
                    const char  *cap_names[MAX_CAPS];
                    LLVMValueRef cap_vals[MAX_CAPS];
                    int          ncaps = 0;

                    /* Walk rhs AST, collect unique symbols that resolve to ENV_VAR */
                    AST *stk[256];
                    int  sp = 0;
                    stk[sp++] = ast->list.items[2];
                    while (sp > 0 && ncaps < MAX_CAPS) {
                        AST *node = stk[--sp];
                        if (!node) continue;
                        if (node->type == AST_SYMBOL) {
                            EnvEntry *ee = env_lookup(ctx->env, node->symbol);
                            if (!ee) continue;
                            if (ee->kind == ENV_FUNC || ee->kind == ENV_BUILTIN) continue;
                            bool dup = false;
                            for (int i = 0; i < ncaps; i++)
                                if (strcmp(cap_names[i], node->symbol) == 0) { dup = true; break; }
                            if (dup) continue;
                            AST tmp = { .type = AST_SYMBOL, .symbol = (char*)node->symbol };
                            LLVMValueRef v = codegen_expr(ctx, &tmp).value;
                            if (LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMIntegerTypeKind) {
                                LLVMTypeRef  ft = LLVMFunctionType(ptr, &i64, 1, 0);
                                LLVMValueRef ci = LLVMBuildIntCast2(ctx->builder, v, i64, 1, "");
                                v = LLVMBuildCall2(ctx->builder, ft, get_rt_value_int(ctx), &ci, 1, "");
                            } else if (LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMDoubleTypeKind) {
                                LLVMTypeRef dbl = LLVMDoubleTypeInContext(ctx->context);
                                LLVMTypeRef ft  = LLVMFunctionType(ptr, &dbl, 1, 0);
                                v = LLVMBuildCall2(ctx->builder, ft, get_rt_value_float(ctx), &v, 1, "");
                            }
                            cap_names[ncaps] = node->symbol;
                            cap_vals[ncaps]  = v;
                            ncaps++;
                        } else if (node->type == AST_LIST) {
                            for (size_t i = 0; i < node->list.count && sp < 255; i++)
                                stk[sp++] = node->list.items[i];
                        }
                    }

                    /* Allocate env array on heap: ncaps ptr-sized slots */
                    LLVMValueRef malloc_fn = LLVMGetNamedFunction(ctx->module, "malloc");
                    if (!malloc_fn) {
                        LLVMTypeRef ft = LLVMFunctionType(ptr, &i64, 1, 0);
                        malloc_fn = LLVMAddFunction(ctx->module, "malloc", ft);
                        LLVMSetLinkage(malloc_fn, LLVMExternalLinkage);
                    }
                    LLVMTypeRef  malloc_ft = LLVMFunctionType(ptr, &i64, 1, 0);
                    LLVMValueRef env_bytes = LLVMConstInt(i64, (uint64_t)ncaps * sizeof(void*), 0);
                    LLVMValueRef env_ptr   = LLVMBuildCall2(ctx->builder, malloc_ft, malloc_fn, &env_bytes, 1, "");

                    for (int i = 0; i < ncaps; i++)
                        LLVMBuildStore(ctx->builder, cap_vals[i],
                                       LLVMBuildGEP2(ctx->builder, ptr, env_ptr,
                                                     (LLVMValueRef[]){LLVMConstInt(i64, i, 0)}, 1, ""));

                    LLVMBasicBlockRef outer_bb = LLVMGetInsertBlock(ctx->builder);

                    static int thunk_counter = 0;
                    char thunk_name[64];
                    snprintf(thunk_name, sizeof(thunk_name), "__pp_rhs_thunk_%d", thunk_counter++);

                    LLVMTypeRef  thunk_ft = LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr}, 1, 0);
                    LLVMValueRef thunk_fn = LLVMAddFunction(ctx->module, thunk_name, thunk_ft);
                    LLVMSetLinkage(thunk_fn, LLVMInternalLinkage);

                    LLVMBasicBlockRef thunk_bb = LLVMAppendBasicBlockInContext(ctx->context, thunk_fn, "entry");
                    LLVMPositionBuilderAtEnd(ctx->builder, thunk_bb);
                    LLVMValueRef ep = LLVMGetParam(thunk_fn, 0);

                    /* Swap in a child env so the thunk body sees the restored locals */
                    Env *saved_env = ctx->env;
                    ctx->env = env_create_child(ctx->env);

                    for (int i = 0; i < ncaps; i++) {
                        LLVMValueRef sv = LLVMBuildLoad2(ctx->builder, ptr,
                                                         LLVMBuildGEP2(ctx->builder, ptr, ep,
                                                                       (LLVMValueRef[]){LLVMConstInt(i64, i, 0)},
                                                                       1, ""), "");
                        EnvEntry *ee = env_lookup(saved_env, cap_names[i]);
                        bool is_int  = ee && ee->type &&
                            (ee->type->kind == TYPE_INT ||
                             ee->type->kind == TYPE_I64 ||
                             ee->type->kind == TYPE_I32);
                        LLVMValueRef final_val = sv;
                        if (is_int) {
                            LLVMTypeRef ft = LLVMFunctionType(i64, &ptr, 1, 0);
                            final_val = LLVMBuildCall2(ctx->builder, ft, get_rt_unbox_int(ctx), &sv, 1, "");
                        }
                        Type *cap_type = (ee && ee->type) ? type_clone(ee->type) : NULL;
                        LLVMValueRef slot = LLVMBuildAlloca(ctx->builder, LLVMTypeOf(final_val), cap_names[i]);
                        LLVMBuildStore(ctx->builder, final_val, slot);
                        env_insert(ctx->env, cap_names[i], cap_type, slot);
                    }

                    CodegenResult rhs_r = codegen_expr(ctx, ast->list.items[2]);
                    LLVMValueRef  rhs_v = rhs_r.value;
                    if (LLVMGetTypeKind(LLVMTypeOf(rhs_v)) == LLVMIntegerTypeKind) {
                        LLVMTypeRef  ft = LLVMFunctionType(ptr, &i64, 1, 0);
                        LLVMValueRef ci = LLVMBuildIntCast2(ctx->builder, rhs_v, i64, 1, "");
                        rhs_v = LLVMBuildCall2(ctx->builder, ft, get_rt_value_int(ctx), &ci, 1, "");
                    }
                    LLVMBuildRet(ctx->builder, rhs_v);

                    env_free(ctx->env);
                    ctx->env = saved_env;

                    LLVMPositionBuilderAtEnd(ctx->builder, outer_bb);

                    LLVMValueRef thunk_v = LLVMBuildCall2(ctx->builder,
                                                          LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr, ptr}, 2, 0),
                                                          get_rt_thunk_create(ctx),
                                                          (LLVMValueRef[]){thunk_fn, env_ptr}, 2, "");

                    result.value = LLVMBuildCall2(ctx->builder,
                                                  LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr, ptr}, 2, 0),
                                                  get_rt_coll_lazy_cons(ctx),
                                                  (LLVMValueRef[]){left_r.value, thunk_v}, 2, "");
                    result.type = type_coll();
                    /* DEBUG: verify n reaches 0 */
                    fprintf(stderr, "DEBUG: emitted lazy_cons in take\n");
                    return result;
#undef MAX_CAPS
                }
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

            if (strcmp(head->symbol, "__adt_tag") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: '__adt_tag' requires 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  i32   = LLVMInt32TypeInContext(ctx->context);
                LLVMTypeRef  i64   = LLVMInt64TypeInContext(ctx->context);
                LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult arg  = codegen_expr(ctx, ast->list.items[1]);
                if (!arg.value) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: __adt_tag got null value",
                                  parser_get_filename(), ast->line, ast->column);
                }
                /* arg is always a ptr — typed accessors guarantee this.
                 * If we get i64, use inttoptr (not bitcast which is invalid). */
                LLVMValueRef adt_ptr = arg.value;
                LLVMTypeRef  arg_t   = LLVMTypeOf(adt_ptr);
                if (LLVMGetTypeKind(arg_t) == LLVMIntegerTypeKind)
                    adt_ptr = LLVMBuildIntToPtr(ctx->builder, adt_ptr, ptr_t, "adt_ptr");
                else if (LLVMGetTypeKind(arg_t) != LLVMPointerTypeKind)
                    adt_ptr = LLVMBuildBitCast(ctx->builder, adt_ptr, ptr_t, "adt_ptr");
                LLVMTypeRef  i32ptr  = LLVMPointerType(i32, 0);
                LLVMValueRef tag_ptr = LLVMBuildBitCast(ctx->builder, adt_ptr, i32ptr, "tag_ptr");
                LLVMValueRef tag     = LLVMBuildLoad2(ctx->builder, i32, tag_ptr, "adt_tag");
                result.value = LLVMBuildSExt(ctx->builder, tag, i64, "adt_tag_i64");
                result.type  = type_int();
                return result;
            }

            if (strcmp(head->symbol, "list-empty?") == 0 || strcmp(head->symbol, "empty?") == 0) {
                REQUIRE_ARGS(1);
                CodegenResult lr = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef res = emit_call_1(ctx, get_rt_list_is_empty(ctx), LLVMInt32TypeInContext(ctx->context), lr.value, "is_empty");
                result.value = LLVMBuildTrunc(ctx->builder, res, LLVMInt1TypeInContext(ctx->context), "list_empty");
                result.type = type_bool(); return result;
            }

            if (strcmp(head->symbol, "list-copy") == 0) {
                REQUIRE_ARGS(1);
                LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                result.value = emit_call_1(ctx, get_rt_list_copy(ctx), ptr, codegen_expr(ctx, ast->list.items[1]).value, "list_copy");
                result.type = type_list(NULL); return result;
            }

            if (strcmp(head->symbol, "append!") == 0) {
                REQUIRE_ARGS(2);
                CodegenResult lr = codegen_expr(ctx, ast->list.items[1]), vr = codegen_expr(ctx, ast->list.items[2]);
                emit_call_2(ctx, get_rt_list_append(ctx), LLVMVoidTypeInContext(ctx->context), lr.value, codegen_box(ctx, vr.value, vr.type), "");
                result.value = lr.value; result.type = type_list(NULL); return result;
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

                Type *target_type = NULL;
                LLVMValueRef target_ptr = codegen_lvalue(ctx, ast->list.items[1], &target_type);
                if (!target_ptr) return result;

                CodegenResult val_r = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef stored = val_r.value;
                if (target_type && target_type->kind != TYPE_UNKNOWN && val_r.type && val_r.type->kind != TYPE_UNKNOWN) {
                    LLVMTypeRef elem_llvm = type_to_llvm(ctx, target_type);
                    if (LLVMGetTypeKind(elem_llvm) != LLVMStructTypeKind) {
                        stored = emit_type_cast(ctx, stored, elem_llvm);
                    }
                }

                LLVMBuildStore(ctx->builder, stored, target_ptr);

                result.value = stored;
                result.type  = target_type ? type_clone(target_type) : type_unknown();
                return result;
            }

            bool is_band = strcmp(head->symbol, "&") == 0, is_bor = strcmp(head->symbol, "bit-or") == 0 || strcmp(head->symbol, "∨") == 0, is_bxor = strcmp(head->symbol, "bit-xor") == 0 || strcmp(head->symbol, "⊕") == 0;
            if (is_band || is_bor || is_bxor) {
                REQUIRE_MIN_ARGS(2);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                CodegenResult first = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef acc = first.value;
                if (LLVMGetTypeKind(LLVMTypeOf(acc)) == LLVMPointerTypeKind) acc = LLVMBuildPtrToInt(ctx->builder, acc, i64, "p2i");
                else if (LLVMGetTypeKind(LLVMTypeOf(acc)) == LLVMStructTypeKind) acc = LLVMConstInt(i64, 0, 0);
                else if (LLVMTypeOf(acc) != i64) acc = LLVMBuildZExt(ctx->builder, acc, i64, "ext");
                for (size_t i = 2; i < ast->list.count; i++) {
                    CodegenResult next = codegen_expr(ctx, ast->list.items[i]);
                    LLVMValueRef nv = next.value;
                    if (LLVMGetTypeKind(LLVMTypeOf(nv)) == LLVMPointerTypeKind) nv = LLVMBuildPtrToInt(ctx->builder, nv, i64, "p2i");
                    else if (LLVMGetTypeKind(LLVMTypeOf(nv)) == LLVMStructTypeKind) continue;
                    else if (LLVMTypeOf(nv) != i64) nv = LLVMBuildZExt(ctx->builder, nv, i64, "ext");
                    acc = is_band ? LLVMBuildAnd(ctx->builder, acc, nv, "band") : (is_bor ? LLVMBuildOr(ctx->builder, acc, nv, "bor") : LLVMBuildXor(ctx->builder, acc, nv, "bxor"));
                }
                result.value = acc; result.type = type_int(); return result;
            }


            if (strcmp(head->symbol, "~") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: '~' requires 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                CodegenResult a = codegen_expr(ctx, ast->list.items[1]);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                LLVMValueRef av = LLVMTypeOf(a.value) != i64
                    ? LLVMBuildZExt(ctx->builder, a.value, i64, "bnot_a") : a.value;
                result.value = LLVMBuildNot(ctx->builder, av, "bnot");
                result.type  = type_int();
                return result;
            }

            bool is_shl = strcmp(head->symbol, "<<") == 0, is_ashr = strcmp(head->symbol, ">>") == 0, is_lshr = strcmp(head->symbol, ">>>") == 0;
            if (is_shl || is_ashr || is_lshr) {
                REQUIRE_ARGS(2);
                CodegenResult a = codegen_expr(ctx, ast->list.items[1]), b = codegen_expr(ctx, ast->list.items[2]);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                LLVMValueRef av = LLVMTypeOf(a.value) != i64 ? (is_ashr ? LLVMBuildSExt(ctx->builder, a.value, i64, "av") : LLVMBuildZExt(ctx->builder, a.value, i64, "av")) : a.value;
                LLVMValueRef bv = LLVMTypeOf(b.value) != i64 ? LLVMBuildZExt(ctx->builder, b.value, i64, "bv") : b.value;
                result.value = is_shl ? LLVMBuildShl(ctx->builder, av, bv, "res") : (is_ashr ? LLVMBuildAShr(ctx->builder, av, bv, "res") : LLVMBuildLShr(ctx->builder, av, bv, "res"));
                result.type = type_int(); return result;
            }

            bool is_and = strcmp(head->symbol, "and") == 0, is_or = strcmp(head->symbol, "or") == 0;
            if (is_and || is_or) {
                REQUIRE_MIN_ARGS(2);
                LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMValueRef acc = NULL;
                for (size_t i = 1; i < ast->list.count; i++) {
                    CodegenResult next = codegen_expr(ctx, ast->list.items[i]);
                    LLVMValueRef cond;
                    if (next.type && next.type->kind == TYPE_BOOL) cond = next.value;
                    else if (type_is_float(next.type)) cond = LLVMBuildFCmp(ctx->builder, LLVMRealONE, next.value, LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0), "cond_f");
                    else if (next.type && (next.type->kind == TYPE_SET || next.type->kind == TYPE_MAP || next.type->kind == TYPE_LIST || next.type->kind == TYPE_UNKNOWN))
                        cond = LLVMBuildICmp(ctx->builder, LLVMIntNE, next.value, LLVMConstNull(ptr), "cond_p");
                    else cond = LLVMBuildICmp(ctx->builder, LLVMIntNE, next.value, LLVMConstInt(LLVMTypeOf(next.value), 0, 0), "cond_i");
                    acc = (i == 1) ? cond : (is_and ? LLVMBuildAnd(ctx->builder, acc, cond, "and") : LLVMBuildOr(ctx->builder, acc, cond, "or"));
                }
                result.value = acc; result.type = type_bool(); return result;
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
                    LLVMTypeRef float_t = LLVMTypeOf(cond_result.value);
                    cond_val = LLVMBuildFCmp(ctx->builder, LLVMRealONE,
                                             cond_result.value,
                                             LLVMConstReal(float_t, 0.0),
                                             "ifcond");
                } else {
                    LLVMValueRef cond_v = cond_result.value;
                    LLVMTypeRef  cond_t = LLVMTypeOf(cond_v);
                    if (LLVMGetTypeKind(cond_t) == LLVMPointerTypeKind) {
                        /* Boxed bool from closure call — unbox to i64 then compare */
                        LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);
                        LLVMTypeRef uft   = LLVMFunctionType(i64_t, &cond_t, 1, 0);
                        cond_v = LLVMBuildCall2(ctx->builder, uft,
                                     get_rt_unbox_int(ctx), &cond_v, 1, "cond_unbox");
                        cond_t = i64_t;
                    }
                    cond_val = LLVMBuildICmp(ctx->builder, LLVMIntNE,
                                             cond_v,
                                             LLVMConstInt(cond_t, 0, 0),
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
                    then_result.type->kind == TYPE_UNKNOWN ||
                    then_result.type->kind == TYPE_OPTIONAL||
                    then_result.type->kind == TYPE_NIL;
                bool else_is_ptr = else_result.type->kind == TYPE_SET     ||
                    else_result.type->kind == TYPE_MAP     ||
                    else_result.type->kind == TYPE_LIST    ||
                    else_result.type->kind == TYPE_STRING  ||
                    else_result.type->kind == TYPE_SYMBOL  ||
                    else_result.type->kind == TYPE_KEYWORD ||
                    else_result.type->kind == TYPE_RATIO   ||
                    else_result.type->kind == TYPE_UNKNOWN ||
                    else_result.type->kind == TYPE_OPTIONAL||
                    else_result.type->kind == TYPE_NIL;

                bool then_is_arr = then_result.type->kind == TYPE_ARR;
                bool else_is_arr = else_result.type->kind == TYPE_ARR;

                /* Box arrays early if unifying with a generic pointer (like Coll/TYPE_UNKNOWN) */
                if (then_is_ptr && else_is_arr) {
                    LLVMPositionBuilderAtEnd(ctx->builder, else_end_bb);
                    else_result.value = codegen_box(ctx, else_result.value, else_result.type);
                    else_result.type  = type_unknown();
                    else_is_ptr = true;
                    else_is_arr = false;
                } else if (else_is_ptr && then_is_arr) {
                    LLVMPositionBuilderAtEnd(ctx->builder, then_end_bb);
                    then_result.value = codegen_box(ctx, then_result.value, then_result.type);
                    then_result.type  = type_unknown();
                    then_is_ptr = true;
                    then_is_arr = false;
                } else if (then_is_arr && else_is_arr && then_result.type->arr_size != else_result.type->arr_size) {
                    /* If both are arrays but different sizes (e.g. an empty literal [] and a dynamically sized recursive return),
                       box both to allow them to unify as generic collections. */
                    LLVMPositionBuilderAtEnd(ctx->builder, then_end_bb);
                    then_result.value = codegen_box(ctx, then_result.value, then_result.type);
                    then_result.type  = type_unknown();
                    then_is_ptr = true;
                    then_is_arr = false;

                    LLVMPositionBuilderAtEnd(ctx->builder, else_end_bb);
                    else_result.value = codegen_box(ctx, else_result.value, else_result.type);
                    else_result.type  = type_unknown();
                    else_is_ptr = true;
                    else_is_arr = false;
                }

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

                if (then_is_ptr || else_is_ptr ||
                    then_result.type->kind == TYPE_COLL || else_result.type->kind == TYPE_COLL) {
                    phi_llvm_type = ptr;
                    if (then_result.type->kind == TYPE_OPTIONAL) {
                        phi_type = then_result.type;
                    } else if (else_result.type->kind == TYPE_OPTIONAL) {
                        phi_type = else_result.type;
                    } else {
                        /* If we are unifying collections, use TYPE_UNKNOWN as the common denominator */
                        phi_type = type_unknown();
                    }
                } else if (then_is_arr || else_is_arr) {
                    phi_llvm_type = then_is_arr ? LLVMTypeOf(then_result.value) : LLVMTypeOf(else_result.value);
                    phi_type      = then_is_arr ? then_result.type : else_result.type;
                } else if (then_is_char || else_is_char) {

                    phi_llvm_type = LLVMInt8TypeInContext(ctx->context);
                    phi_type      = type_char();
                } else if (then_is_float || else_is_float) {
                    // Prevent F32/Float mismatch
                    if (then_result.type->kind == TYPE_F32 && else_result.type->kind == TYPE_F32) {
                        phi_llvm_type = LLVMFloatTypeInContext(ctx->context);
                        phi_type      = type_f32();
                    } else {
                        phi_llvm_type = LLVMDoubleTypeInContext(ctx->context);
                        phi_type      = type_float();
                    }
                } else if (then_is_bool && else_is_bool) {
                    phi_llvm_type = LLVMInt1TypeInContext(ctx->context);
                    phi_type      = type_bool();
                } else if (then_is_int && else_is_int) {
                    phi_llvm_type = LLVMInt64TypeInContext(ctx->context);
                    phi_type      = type_int();
                } else {
                    /* Fallback for complex unifications: Box everything and treat as opaque pointers */
                    phi_llvm_type = ptr;
                    phi_type      = type_unknown();
                }

                /* Coerce then-value and branch to merge */
                LLVMPositionBuilderAtEnd(ctx->builder, then_end_bb);
                LLVMValueRef then_val = then_result.value;
                if (!LLVMGetBasicBlockTerminator(then_end_bb)) {
                    if (phi_llvm_type == ptr && !then_is_ptr) {
                        then_val = codegen_box(ctx, then_val, then_result.type);
                    } else {
                        then_val = emit_type_cast(ctx, then_val, phi_llvm_type);
                    }
                    LLVMBuildBr(ctx->builder, merge_bb);
                }
                LLVMBasicBlockRef then_phi_bb = LLVMGetInsertBlock(ctx->builder);

                /* Coerce else-value and branch to merge */
                LLVMPositionBuilderAtEnd(ctx->builder, else_end_bb);
                LLVMValueRef else_val = else_result.value;
                if (!LLVMGetBasicBlockTerminator(else_end_bb)) {
                    if (phi_llvm_type == ptr && !else_is_ptr) {
                        else_val = codegen_box(ctx, else_val, else_result.type);
                    } else {
                        else_val = emit_type_cast(ctx, else_val, phi_llvm_type);
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

            if (strcmp(head->symbol, "not") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'not' requires 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                CodegenResult arg = codegen_expr(ctx, ast->list.items[1]);
                LLVMTypeRef  i1  = LLVMInt1TypeInContext(ctx->context);
                LLVMValueRef val = arg.value;
                if (arg.type && arg.type->kind == TYPE_BOOL) {
                    result.value = LLVMBuildNot(ctx->builder, val, "not");
                } else if (arg.type && type_is_float(arg.type)) {
                    LLVMValueRef zero = LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0);
                    LLVMValueRef cmp  = LLVMBuildFCmp(ctx->builder, LLVMRealOEQ, val, zero, "not_f");
                    result.value = cmp;
                } else {
                    LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(val), 0, 0);
                    LLVMValueRef cmp  = LLVMBuildICmp(ctx->builder, LLVMIntEQ, val, zero, "not_i");
                    result.value = cmp;
                }
                result.type = type_bool();
                return result;
            }

            if (strcmp(head->symbol, "unless") == 0) {
                if (ast->list.count < 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'unless' requires at least a condition and one body expression",
                                  parser_get_filename(), ast->line, ast->column);
                }
                /* (unless cond body...) = (if (not cond) body...) */
                CodegenResult cond_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef cond_val = cond_r.value;
                if (type_is_float(cond_r.type)) {
                    cond_val = LLVMBuildFCmp(ctx->builder, LLVMRealOEQ, cond_val,
                                             LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0),
                                             "unless_cond");
                } else if (cond_r.type && cond_r.type->kind == TYPE_BOOL) {
                    cond_val = LLVMBuildNot(ctx->builder, cond_val, "unless_not");
                } else {
                    cond_val = LLVMBuildICmp(ctx->builder, LLVMIntEQ, cond_val,
                                             LLVMConstInt(LLVMTypeOf(cond_val), 0, 0),
                                             "unless_cond");
                }
                LLVMValueRef func     = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "unless_then");
                LLVMBasicBlockRef exit_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "unless_exit");
                LLVMBuildCondBr(ctx->builder, cond_val, then_bb, exit_bb);
                LLVMPositionBuilderAtEnd(ctx->builder, then_bb);
                Env *saved_env = ctx->env;
                ctx->env = env_create_child(saved_env);
                for (size_t bi = 2; bi < ast->list.count; bi++)
                    codegen_expr(ctx, ast->list.items[bi]);
                env_free(ctx->env);
                ctx->env = saved_env;
                if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder)))
                    LLVMBuildBr(ctx->builder, exit_bb);
                LLVMPositionBuilderAtEnd(ctx->builder, exit_bb);
                result.value = codegen_current_fn_zero_value(ctx);
                result.type  = type_unknown();
                return result;
            }

            if (strcmp(head->symbol, "until") == 0) {
                REQUIRE_MIN_ARGS(2);
                /* (until cond body) = (while (not cond) body) */
                LLVMValueRef func     = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "until_cond");
                LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "until_body");
                LLVMBasicBlockRef exit_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "until_exit");

                LLVMBuildBr(ctx->builder, cond_bb);

                LLVMPositionBuilderAtEnd(ctx->builder, cond_bb);
                CodegenResult cond_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef cond_val = cond_r.value;
                if (type_is_float(cond_r.type)) {
                    cond_val = LLVMBuildFCmp(ctx->builder, LLVMRealONE, cond_val,
                                             LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0),
                                             "until_cond_f");
                } else if (cond_r.type && cond_r.type->kind != TYPE_BOOL) {
                    cond_val = LLVMBuildICmp(ctx->builder, LLVMIntNE, cond_val,
                                             LLVMConstInt(LLVMTypeOf(cond_val), 0, 0),
                                             "until_cond_i");
                }
                /* Negate — loop while condition is FALSE */
                LLVMValueRef neg = LLVMBuildNot(ctx->builder, cond_val, "until_neg");
                LLVMBuildCondBr(ctx->builder, neg, body_bb, exit_bb);

                LLVMPositionBuilderAtEnd(ctx->builder, body_bb);
                Env *saved_env = ctx->env;
                ctx->env = env_create_child(saved_env);
                for (size_t bi = 2; bi < ast->list.count; bi++)
                    codegen_expr(ctx, ast->list.items[bi]);
                env_free(ctx->env);
                ctx->env = saved_env;
                if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder)))
                    LLVMBuildBr(ctx->builder, cond_bb);

                LLVMPositionBuilderAtEnd(ctx->builder, exit_bb);
                result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                result.type  = type_int();
                return result;
            }

            if (strcmp(head->symbol, "while") == 0) {
                if (ast->list.count < 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'while' requires at least a condition and one body expression",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMValueRef func     = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "while_cond");
                LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "while_body");
                LLVMBasicBlockRef exit_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "while_exit");

                LLVMBuildBr(ctx->builder, cond_bb);

                /* Condition */
                LLVMPositionBuilderAtEnd(ctx->builder, cond_bb);
                CodegenResult cond_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef cond_val = cond_r.value;
                if (type_is_float(cond_r.type)) {
                    cond_val = LLVMBuildFCmp(ctx->builder, LLVMRealONE,
                                             cond_val,
                                             LLVMConstReal(LLVMDoubleTypeInContext(ctx->context), 0.0),
                                             "while_cond_f");
                } else if (cond_r.type && cond_r.type->kind != TYPE_BOOL) {
                    cond_val = LLVMBuildICmp(ctx->builder, LLVMIntNE,
                                             cond_val,
                                             LLVMConstInt(LLVMTypeOf(cond_val), 0, 0),
                                             "while_cond_i");
                }
                LLVMBuildCondBr(ctx->builder, cond_val, body_bb, exit_bb);

                /* Body */
                LLVMPositionBuilderAtEnd(ctx->builder, body_bb);
                Env *saved_env = ctx->env;
                ctx->env = env_create_child(saved_env);
                for (size_t bi = 2; bi < ast->list.count; bi++)
                    codegen_expr(ctx, ast->list.items[bi]);
                env_free(ctx->env);
                ctx->env = saved_env;
                if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder)))
                    LLVMBuildBr(ctx->builder, cond_bb);

                /* Exit */
                LLVMPositionBuilderAtEnd(ctx->builder, exit_bb);
                result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
                result.type  = type_int();
                return result;
            }

            if (strcmp(head->symbol, "for") == 0) {
                if (ast->list.count < 3) {
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
                        : (LLVMTypeOf(count_r.value) != i64 ? LLVMBuildSExt(ctx->builder, count_r.value, i64, "count_ext") : count_r.value);
                    step_val      = LLVMConstInt(i64, 1, 0);
                    var_name      = "__for_i";
                    has_var       = false;
                    negative_step = false;
                } else if (binding->array.element_count == 2) {
                    if (binding->array.elements[0]->type != AST_SYMBOL) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'for' loop variable must be a symbol",
                                parser_get_filename(), ast->line, ast->column);
                    }
                    CodegenResult count_r = codegen_expr(ctx, binding->array.elements[1]);
                    start_val     = LLVMConstInt(i64, 0, 0);
                    end_val       = type_is_float(count_r.type)
                        ? LLVMBuildFPToSI(ctx->builder, count_r.value, i64, "count")
                        : (LLVMTypeOf(count_r.value) != i64 ? LLVMBuildSExt(ctx->builder, count_r.value, i64, "count_ext") : count_r.value);
                    step_val      = LLVMConstInt(i64, 1, 0);
                    var_name      = binding->array.elements[0]->symbol;
                    has_var       = true;
                    negative_step = false;
                } else {
                    if (binding->array.elements[0]->type != AST_SYMBOL) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'for' loop variable must be a symbol",
                                parser_get_filename(), ast->line, ast->column);
                    }

                    CodegenResult start_r = codegen_expr(ctx, binding->array.elements[1]);
                    CodegenResult end_r   = codegen_expr(ctx, binding->array.elements[2]);  // BUG HERE
                    CodegenResult step_r;
                    if (binding->array.element_count == 4) {
                        step_r = codegen_expr(ctx, binding->array.elements[3]);
                    } else {
                        step_r.value = LLVMConstInt(i64, 1, 0);
                        step_r.type  = type_int();
                    }

                    start_val = type_is_float(start_r.type)
                        ? LLVMBuildFPToSI(ctx->builder, start_r.value, i64, "start")
                        : (LLVMTypeOf(start_r.value) != i64 ? LLVMBuildSExt(ctx->builder, start_r.value, i64, "start_ext") : start_r.value);
                    end_val = type_is_float(end_r.type)
                        ? LLVMBuildFPToSI(ctx->builder, end_r.value, i64, "end")
                        : (LLVMTypeOf(end_r.value) != i64 ? LLVMBuildSExt(ctx->builder, end_r.value, i64, "end_ext") : end_r.value);
                    step_val = type_is_float(step_r.type)
                        ? LLVMBuildFPToSI(ctx->builder, step_r.value, i64, "step")
                        : (LLVMTypeOf(step_r.value) != i64 ? LLVMBuildSExt(ctx->builder, step_r.value, i64, "step_ext") : step_r.value);

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

                for (size_t bi = 2; bi < ast->list.count; bi++)
                    codegen_expr(ctx, ast->list.items[bi]);

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

            if (strcmp(head->symbol, "Set?") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'Set?' requires 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  i1     = LLVMInt1TypeInContext(ctx->context);
                CodegenResult arg   = codegen_expr(ctx, ast->list.items[1]);
                if (arg.type && arg.type->kind == TYPE_SET) {
                    result.value = LLVMConstInt(i1, 1, 0);
                } else {
                    result.value = LLVMConstInt(i1, 0, 0);
                }
                result.type = type_bool();
                return result;
            }

            if (strcmp(head->symbol, "collection?") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'collection?' requires 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }
                LLVMTypeRef  i1  = LLVMInt1TypeInContext(ctx->context);
                CodegenResult arg = codegen_expr(ctx, ast->list.items[1]);
                if (arg.type && (arg.type->kind == TYPE_LIST ||
                                 arg.type->kind == TYPE_SET  ||
                                 arg.type->kind == TYPE_ARR  ||
                                 arg.type->kind == TYPE_COLL)) {
                    result.value = LLVMConstInt(i1, 1, 0);
                } else {
                    result.value = LLVMConstInt(i1, 0, 0);
                }
                result.type = type_bool();
                return result;
            }

            // Map
            if (strcmp(head->symbol, "assoc")   == 0) return codegen_map_op(ctx, ast, get_rt_map_assoc,      "assoc",   3);
            if (strcmp(head->symbol, "assoc!")  == 0) return codegen_map_op(ctx, ast, get_rt_map_assoc_mut,  "assoc!",  3);
            if (strcmp(head->symbol, "dissoc")  == 0) return codegen_map_op(ctx, ast, get_rt_map_dissoc,     "dissoc",  2);
            if (strcmp(head->symbol, "dissoc!") == 0) return codegen_map_op(ctx, ast, get_rt_map_dissoc_mut, "dissoc!", 2);
            if (strcmp(head->symbol, "keys")    == 0) return codegen_map_op(ctx, ast, get_rt_map_keys,       "keys",    1);
            if (strcmp(head->symbol, "vals")    == 0) return codegen_map_op(ctx, ast, get_rt_map_vals,       "vals",    1);

            if (strcmp(head->symbol, "find") == 0) {
                REQUIRE_ARGS(2);
                LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                CodegenResult map_r = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult key_r = codegen_expr(ctx, ast->list.items[2]);
                LLVMValueRef raw_map = map_r.value;
                if (map_r.type && map_r.type->kind == TYPE_MAP)
                    raw_map = emit_call_1(ctx, get_rt_unbox_map(ctx), ptr, raw_map, "rawmap");
                result.value = emit_call_2(ctx, get_rt_map_find(ctx), ptr, raw_map, codegen_box(ctx, key_r.value, key_r.type), "find");
                result.type  = type_unknown();
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

            if (strcmp(head->symbol, "Map?") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'Map?' requires 1 argument",
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
                LLVMValueRef  raw   = col_r.value;

                /* String contains?: use strstr */
                if (col_r.type && col_r.type->kind == TYPE_STRING) {
                    LLVMValueRef strstr_fn = LLVMGetNamedFunction(ctx->module, "strstr");
                    if (!strstr_fn) {
                        LLVMTypeRef params[] = {ptr, ptr};
                        LLVMTypeRef ft = LLVMFunctionType(ptr, params, 2, 0);
                        strstr_fn = LLVMAddFunction(ctx->module, "strstr", ft);
                        LLVMSetLinkage(strstr_fn, LLVMExternalLinkage);
                    }
                    LLVMValueRef needle = key_r.value;
                    /* If needle is boxed, unbox to string ptr */
                    if (key_r.type && key_r.type->kind != TYPE_STRING) {
                        LLVMTypeRef uft = LLVMFunctionType(ptr, &ptr, 1, 0);
                        needle = LLVMBuildCall2(ctx->builder, uft,
                                     get_rt_unbox_string(ctx), &needle, 1, "needle");
                    }
                    LLVMValueRef args[] = {raw, needle};
                    LLVMValueRef found  = LLVMBuildCall2(ctx->builder,
                                             LLVMGlobalGetValueType(strstr_fn),
                                             strstr_fn, args, 2, "strstr");
                    /* strstr returns NULL if not found */
                    result.value = LLVMBuildICmp(ctx->builder, LLVMIntNE, found,
                                                 LLVMConstPointerNull(ptr), "contains_bool");
                    result.type  = type_bool();
                    return result;
                }

                LLVMValueRef  boxed = codegen_box(ctx, key_r.value, key_r.type);
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

            if (strcmp(head->symbol, "starts-with?") == 0 ||
                strcmp(head->symbol, "ends-with?") == 0) {
                bool is_ends = (strcmp(head->symbol, "ends-with?") == 0);
                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: ‘%s’ requires 2 arguments",
                                  parser_get_filename(), ast->line, ast->column, head->symbol);
                }
                LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
                LLVMTypeRef i1  = LLVMInt1TypeInContext(ctx->context);
                LLVMTypeRef i8  = LLVMInt8TypeInContext(ctx->context);
                CodegenResult col_r = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult suf_r = codegen_expr(ctx, ast->list.items[2]);

                /* ── String ── */
                if (col_r.type && col_r.type->kind == TYPE_STRING) {
                    LLVMValueRef strlen_fn = LLVMGetNamedFunction(ctx->module, "strlen");
                    if (!strlen_fn) {
                        LLVMTypeRef ft = LLVMFunctionType(i64, &ptr, 1, 0);
                        strlen_fn = LLVMAddFunction(ctx->module, "strlen", ft);
                        LLVMSetLinkage(strlen_fn, LLVMExternalLinkage);
                    }
                    LLVMValueRef strncmp_fn = LLVMGetNamedFunction(ctx->module, "strncmp");
                    if (!strncmp_fn) {
                        LLVMTypeRef p3[] = {ptr, ptr, i64};
                        LLVMTypeRef ft   = LLVMFunctionType(i32, p3, 3, 0);
                        strncmp_fn = LLVMAddFunction(ctx->module, "strncmp", ft);
                        LLVMSetLinkage(strncmp_fn, LLVMExternalLinkage);
                    }
                    /* Coerce suffix to string ptr */
                    LLVMValueRef suffix = suf_r.value;
                    if (suf_r.type && suf_r.type->kind == TYPE_CHAR) {
                        LLVMValueRef buf = LLVMBuildAlloca(ctx->builder, LLVMArrayType(i8, 2), "chbuf");
                        LLVMValueRef z = LLVMConstInt(i64, 0, 0);
                        LLVMValueRef o = LLVMConstInt(i64, 1, 0);
                        LLVMBuildStore(ctx->builder, suf_r.value,
                            LLVMBuildGEP2(ctx->builder, i8, buf, &z, 1, "p0"));
                        LLVMBuildStore(ctx->builder, LLVMConstInt(i8, 0, 0),
                            LLVMBuildGEP2(ctx->builder, i8, buf, &o, 1, "p1"));
                        suffix = LLVMBuildBitCast(ctx->builder, buf, ptr, "chstr");
                    } else if (suf_r.type && suf_r.type->kind != TYPE_STRING) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: ‘%s’ on String requires String or Char suffix, got %s",
                                      parser_get_filename(), ast->line, ast->column,
                                      head->symbol, type_to_string(suf_r.type));
                    }
                    LLVMValueRef sla[] = {col_r.value};
                    LLVMValueRef fla[] = {suffix};
                    LLVMValueRef slen   = LLVMBuildCall2(ctx->builder,
                        LLVMFunctionType(i64, &ptr, 1, 0), strlen_fn, sla, 1, "slen");
                    LLVMValueRef suflen = LLVMBuildCall2(ctx->builder,
                        LLVMFunctionType(i64, &ptr, 1, 0), strlen_fn, fla, 1, "suflen");
                    LLVMValueRef enough = LLVMBuildICmp(ctx->builder, LLVMIntSGE, slen, suflen, "enough");
                    LLVMValueRef cmp_ptr = is_ends
                        ? LLVMBuildGEP2(ctx->builder, i8, col_r.value,
                              &(LLVMValueRef){LLVMBuildSub(ctx->builder, slen, suflen, "diff")}, 1, "tail")
                        : col_r.value;
                    LLVMValueRef nc_args[] = {cmp_ptr, suffix, suflen};
                    LLVMValueRef cmp = LLVMBuildCall2(ctx->builder,
                        LLVMFunctionType(i32, (LLVMTypeRef[]){ptr, ptr, i64}, 3, 0),
                        strncmp_fn, nc_args, 3, "cmp");
                    result.value = LLVMBuildAnd(ctx->builder, enough,
                        LLVMBuildICmp(ctx->builder, LLVMIntEQ, cmp, LLVMConstInt(i32, 0, 0), "eq"),
                        "sw_result");
                    result.type = type_bool();
                    return result;
                }

                /* ── Array (fat pointer) ── */
                if (col_r.type && col_r.type->kind == TYPE_ARR) {
                    Type *et = col_r.type->arr_element_type ? col_r.type->arr_element_type : type_int();
                    LLVMTypeRef elem_t = type_to_llvm(ctx, et);

                    /* Extract col data+size */
                    LLVMValueRef col_data = arr_fat_data(ctx, col_r.value, et);
                    LLVMValueRef col_size = arr_fat_size(ctx, col_r.value);

                    /* Extract suffix data+size */
                    LLVMValueRef suf_data, suf_size;
                    if (suf_r.type && suf_r.type->kind == TYPE_ARR) {
                        suf_data = arr_fat_data(ctx, suf_r.value, et);
                        suf_size = arr_fat_size(ctx, suf_r.value);
                    } else {
                        /* Single scalar element */
                        LLVMValueRef buf = LLVMBuildAlloca(ctx->builder, elem_t, "scal");
                        LLVMValueRef sv  = suf_r.value;
                        if (LLVMTypeOf(sv) != elem_t)
                            sv = LLVMBuildBitCast(ctx->builder, sv, elem_t, "sc");
                        LLVMBuildStore(ctx->builder, sv, buf);
                        suf_data = buf;
                        suf_size = LLVMConstInt(i64, 1, 0);
                    }

                    LLVMValueRef enough = LLVMBuildICmp(ctx->builder, LLVMIntSGE,
                                             col_size, suf_size, "enough");
                    LLVMValueRef offset = is_ends
                        ? LLVMBuildSub(ctx->builder, col_size, suf_size, "off")
                        : LLVMConstInt(i64, 0, 0);

                    /* Loop: for i in [0, suf_size): col_data[offset+i] == suf_data[i] */
                    LLVMValueRef func2   = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                    LLVMValueRef res_ptr = LLVMBuildAlloca(ctx->builder, i1, "res");
                    LLVMBuildStore(ctx->builder, enough, res_ptr);
                    LLVMValueRef iptr = LLVMBuildAlloca(ctx->builder, i64, "i");
                    LLVMBuildStore(ctx->builder, LLVMConstInt(i64, 0, 0), iptr);

                    LLVMBasicBlockRef chk  = LLVMAppendBasicBlockInContext(ctx->context, func2, "aw_chk");
                    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(ctx->context, func2, "aw_body");
                    LLVMBasicBlockRef inc  = LLVMAppendBasicBlockInContext(ctx->context, func2, "aw_inc");
                    LLVMBasicBlockRef fail = LLVMAppendBasicBlockInContext(ctx->context, func2, "aw_fail");
                    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx->context, func2, "aw_done");

                    LLVMBuildCondBr(ctx->builder, enough, chk, done);

                    LLVMPositionBuilderAtEnd(ctx->builder, chk);
                    LLVMValueRef iv = LLVMBuildLoad2(ctx->builder, i64, iptr, "iv");
                    LLVMBuildCondBr(ctx->builder,
                        LLVMBuildICmp(ctx->builder, LLVMIntSLT, iv, suf_size, "c"),
                        body, done);

                    LLVMPositionBuilderAtEnd(ctx->builder, body);
                    LLVMValueRef iv2  = LLVMBuildLoad2(ctx->builder, i64, iptr, "iv2");
                    LLVMValueRef ci   = LLVMBuildAdd(ctx->builder, offset, iv2, "ci");
                    LLVMValueRef lv   = LLVMBuildLoad2(ctx->builder, elem_t,
                                           LLVMBuildGEP2(ctx->builder, elem_t, col_data, &ci,  1, "lp"), "lv");
                    LLVMValueRef sv2  = LLVMBuildLoad2(ctx->builder, elem_t,
                                           LLVMBuildGEP2(ctx->builder, elem_t, suf_data, &iv2, 1, "sp"), "sv");
                    /* Compare elements — use icmp for integers, fcmp for floats */
                    LLVMValueRef eq;
                    if (type_is_float(et))
                        eq = LLVMBuildFCmp(ctx->builder, LLVMRealOEQ, lv, sv2, "eq");
                    else
                        eq = LLVMBuildICmp(ctx->builder, LLVMIntEQ, lv, sv2, "eq");
                    LLVMBuildCondBr(ctx->builder,
                        LLVMBuildNot(ctx->builder, eq, "neq"), fail, inc);

                    LLVMPositionBuilderAtEnd(ctx->builder, inc);
                    LLVMBuildStore(ctx->builder,
                        LLVMBuildAdd(ctx->builder, iv2, LLVMConstInt(i64, 1, 0), "n"), iptr);
                    LLVMBuildBr(ctx->builder, chk);

                    LLVMPositionBuilderAtEnd(ctx->builder, fail);
                    LLVMBuildStore(ctx->builder, LLVMConstInt(i1, 0, 0), res_ptr);
                    LLVMBuildBr(ctx->builder, done);

                    LLVMPositionBuilderAtEnd(ctx->builder, done);
                    result.value = LLVMBuildLoad2(ctx->builder, i1, res_ptr, "aw_result");
                    result.type  = type_bool();
                    return result;
                }

                /* ── List ── */
                {
                    LLVMValueRef func2  = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                    LLVMTypeRef  lft    = LLVMFunctionType(i64, &ptr, 1, 0);
                    LLVMValueRef len_fn = get_rt_list_length(ctx);
                    LLVMValueRef nth_fn = get_rt_list_nth(ctx);
                    LLVMValueRef eq_fn  = get_rt_equal_p(ctx);

                    LLVMValueRef col_val = col_r.value;
                    LLVMValueRef suf_val;

                    /* Wrap scalar suffix in single-element list */
                    if (suf_r.type && suf_r.type->kind != TYPE_LIST) {
                        LLVMValueRef sl = LLVMBuildCall2(ctx->builder,
                            LLVMFunctionType(ptr, NULL, 0, 0), get_rt_list_new(ctx), NULL, 0, "sl");
                        LLVMValueRef bv = codegen_box(ctx, suf_r.value, suf_r.type);
                        LLVMValueRef aa[] = {sl, bv};
                        LLVMBuildCall2(ctx->builder,
                            LLVMFunctionType(LLVMVoidTypeInContext(ctx->context),
                                             (LLVMTypeRef[]){ptr, ptr}, 2, 0),
                            get_rt_list_append(ctx), aa, 2, "");
                        suf_val = sl;
                    } else {
                        suf_val = suf_r.value;
                    }

                    LLVMValueRef lla[]  = {col_val};
                    LLVMValueRef sla[]  = {suf_val};
                    LLVMValueRef llen   = LLVMBuildCall2(ctx->builder, lft, len_fn, lla, 1, "llen");
                    LLVMValueRef slen   = LLVMBuildCall2(ctx->builder, lft, len_fn, sla, 1, "slen");
                    LLVMValueRef enough = LLVMBuildICmp(ctx->builder, LLVMIntSGE, llen, slen, "enough");
                    LLVMValueRef offset = is_ends
                        ? LLVMBuildSub(ctx->builder, llen, slen, "off")
                        : LLVMConstInt(i64, 0, 0);

                    LLVMValueRef res_ptr = LLVMBuildAlloca(ctx->builder, i1, "res");
                    LLVMBuildStore(ctx->builder, enough, res_ptr);
                    LLVMValueRef iptr = LLVMBuildAlloca(ctx->builder, i64, "i");
                    LLVMBuildStore(ctx->builder, LLVMConstInt(i64, 0, 0), iptr);

                    LLVMBasicBlockRef chk  = LLVMAppendBasicBlockInContext(ctx->context, func2, "lw_chk");
                    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(ctx->context, func2, "lw_body");
                    LLVMBasicBlockRef inc  = LLVMAppendBasicBlockInContext(ctx->context, func2, "lw_inc");
                    LLVMBasicBlockRef fail = LLVMAppendBasicBlockInContext(ctx->context, func2, "lw_fail");
                    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx->context, func2, "lw_done");

                    LLVMBuildCondBr(ctx->builder, enough, chk, done);

                    LLVMPositionBuilderAtEnd(ctx->builder, chk);
                    LLVMValueRef iv = LLVMBuildLoad2(ctx->builder, i64, iptr, "iv");
                    LLVMBuildCondBr(ctx->builder,
                        LLVMBuildICmp(ctx->builder, LLVMIntSLT, iv, slen, "c"),
                        body, done);

                    LLVMPositionBuilderAtEnd(ctx->builder, body);
                    LLVMValueRef iv2 = LLVMBuildLoad2(ctx->builder, i64, iptr, "iv2");
                    LLVMValueRef li  = LLVMBuildAdd(ctx->builder, offset, iv2, "li");
                    LLVMTypeRef  nft_p[] = {ptr, i64};
                    LLVMTypeRef  nft = LLVMFunctionType(ptr, nft_p, 2, 0);
                    LLVMValueRef lna[] = {col_val, li};
                    LLVMValueRef sna[] = {suf_val, iv2};
                    LLVMValueRef lv  = LLVMBuildCall2(ctx->builder, nft, nth_fn, lna, 2, "lv");
                    LLVMValueRef sv  = LLVMBuildCall2(ctx->builder, nft, nth_fn, sna, 2, "sv");
                    LLVMTypeRef  eft_p[] = {ptr, ptr};
                    LLVMValueRef ea[] = {lv, sv};
                    LLVMValueRef eqv = LLVMBuildCall2(ctx->builder,
                        LLVMFunctionType(i32, eft_p, 2, 0), eq_fn, ea, 2, "eqv");
                    LLVMBuildCondBr(ctx->builder,
                        LLVMBuildICmp(ctx->builder, LLVMIntEQ, eqv, LLVMConstInt(i32, 0, 0), "neq"),
                        fail, inc);

                    LLVMPositionBuilderAtEnd(ctx->builder, inc);
                    LLVMBuildStore(ctx->builder,
                        LLVMBuildAdd(ctx->builder, iv2, LLVMConstInt(i64, 1, 0), "n"), iptr);
                    LLVMBuildBr(ctx->builder, chk);

                    LLVMPositionBuilderAtEnd(ctx->builder, fail);
                    LLVMBuildStore(ctx->builder, LLVMConstInt(i1, 0, 0), res_ptr);
                    LLVMBuildBr(ctx->builder, done);

                    LLVMPositionBuilderAtEnd(ctx->builder, done);
                    result.value = LLVMBuildLoad2(ctx->builder, i1, res_ptr, "lw_result");
                    result.type  = type_bool();
                    return result;
                }
            }

            if (strcmp(head->symbol, "count") == 0) {
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: 'count' requires 1 argument",
                                  parser_get_filename(), ast->line, ast->column);
                }

                LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);

                CodegenResult col_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef val = col_r.value;

                // 1. Array handling: Use fat pointer size or compile-time constant
                if (col_r.type && col_r.type->kind == TYPE_ARR) {
                    if (col_r.type->arr_is_fat) {
                        result.value = arr_fat_size(ctx, val);
                    } else {
                        result.value = LLVMConstInt(i64_t, col_r.type->arr_size, 0);
                    }
                    result.type = type_int();
                    return result;
                }

                // 2. String handling: Use strlen
                if (col_r.type && col_r.type->kind == TYPE_STRING) {
                    LLVMValueRef strlen_fn = LLVMGetNamedFunction(ctx->module, "strlen");
                    if (!strlen_fn) {
                        LLVMTypeRef ft = LLVMFunctionType(i64_t, &ptr_t, 1, 0);
                        strlen_fn = LLVMAddFunction(ctx->module, "strlen", ft);
                        LLVMSetLinkage(strlen_fn, LLVMExternalLinkage);
                    }
                    result.value = LLVMBuildCall2(ctx->builder, LLVMFunctionType(i64_t, &ptr_t, 1, 0),
                                                  strlen_fn, &val, 1, "str_len");
                    result.type = type_int();
                    return result;
                }

                // 3. Map handling: Unbox and call rt_map_count
                if (col_r.type && col_r.type->kind == TYPE_MAP) {
                    LLVMValueRef ub_fn = get_rt_unbox_map(ctx);
                    LLVMValueRef raw = LLVMBuildCall2(ctx->builder, LLVMFunctionType(ptr_t, &ptr_t, 1, 0),
                                                      ub_fn, &val, 1, "raw_map");
                    result.value = LLVMBuildCall2(ctx->builder, LLVMFunctionType(i64_t, &ptr_t, 1, 0),
                                                  get_rt_map_count(ctx), &raw, 1, "map_cnt");
                    result.type = type_int();
                    return result;
                }

                // 4. Set handling: Unbox and call rt_set_count
                if (col_r.type && col_r.type->kind == TYPE_SET) {
                    LLVMValueRef ub_fn = get_rt_unbox_set(ctx);
                    LLVMValueRef raw = LLVMBuildCall2(ctx->builder, LLVMFunctionType(ptr_t, &ptr_t, 1, 0),
                                                      ub_fn, &val, 1, "raw_set");
                    result.value = LLVMBuildCall2(ctx->builder, LLVMFunctionType(i64_t, &ptr_t, 1, 0),
                                                  get_rt_set_count(ctx), &raw, 1, "set_cnt");
                    result.type = type_int();
                    return result;
                }

                // 5. Generic Collection (Coll), List, Array, or Unknown
                if (col_r.type && (col_r.type->kind == TYPE_COLL ||
                                   col_r.type->kind == TYPE_LIST ||
                                   col_r.type->kind == TYPE_ARR  ||
                                   col_r.type->kind == TYPE_UNKNOWN)) {
                    LLVMValueRef count_fn = get_rt_coll_count(ctx);
                    result.value = LLVMBuildCall2(ctx->builder, LLVMFunctionType(i64_t, &ptr_t, 1, 0),
                                                  count_fn, &val, 1, "coll_cnt");
                    result.type = type_int();
                    return result;
                }

                CODEGEN_ERROR(ctx, "error: 'count' not supported for type %s", type_to_string(col_r.type));
            }

            if (strcmp(head->symbol, "<")  == 0 || strcmp(head->symbol, ">")  == 0 ||
                strcmp(head->symbol, "<=") == 0 || strcmp(head->symbol, ">=") == 0 ||
                strcmp(head->symbol, "=")  == 0 || strcmp(head->symbol, "!=") == 0) {

                if (ast->list.count == 2) {
                    /* Partial application: (< 5) => lambda y -> (< 5 y) */
                    LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef  i32   = LLVMInt32TypeInContext(ctx->context);
                    LLVMTypeRef  i64   = LLVMInt64TypeInContext(ctx->context);

                    /* Codegen the single supplied arg and box it */
                    CodegenResult supplied = codegen_expr(ctx, ast->list.items[1]);
                    LLVMValueRef  boxed    = codegen_box(ctx, supplied.value, supplied.type);

                    /* Store op string and boxed arg in env: [op_ptr, boxed_arg] */
                    LLVMValueRef op_global = LLVMBuildGlobalStringPtr(ctx->builder,
                                                head->symbol, "partial_op");

                    /* Build trampoline: (ptr env, i32 n, ptr args[]) -> ptr */
                    static int partial_cmp_count = 0;
                    char tname[64];
                    snprintf(tname, sizeof(tname), "__partial_cmp_%d", partial_cmp_count++);

                    LLVMTypeRef  tramp_params[] = {ptr_t, i32, ptr_t};
                    LLVMTypeRef  tramp_ft = LLVMFunctionType(ptr_t, tramp_params, 3, 0);
                    LLVMValueRef tramp    = LLVMAddFunction(ctx->module, tname, tramp_ft);
                    LLVMBasicBlockRef saved_bb    = LLVMGetInsertBlock(ctx->builder);
                    LLVMBasicBlockRef tramp_entry = LLVMAppendBasicBlockInContext(
                                                       ctx->context, tramp, "entry");
                    LLVMPositionBuilderAtEnd(ctx->builder, tramp_entry);

                    /* env[0] = op string ptr, env[1] = boxed captured arg */
                    LLVMValueRef env_p  = LLVMGetParam(tramp, 0);
                    LLVMValueRef args_p = LLVMGetParam(tramp, 2);

                    /* Load captured (lhs) value from env[1] */
                    LLVMValueRef cap_idx  = LLVMConstInt(i32, 1, 0);
                    LLVMValueRef cap_slot = LLVMBuildGEP2(ctx->builder, ptr_t,
                                               env_p, &cap_idx, 1, "cap_slot");
                    LLVMValueRef cap_val  = LLVMBuildLoad2(ctx->builder, ptr_t,
                                               cap_slot, "cap_val");

                    /* Load the incoming (rhs) arg from args[0] */
                    LLVMValueRef zero_idx = LLVMConstInt(i32, 0, 0);
                    LLVMValueRef arg_slot = LLVMBuildGEP2(ctx->builder, ptr_t,
                                               args_p, &zero_idx, 1, "arg_slot");
                    LLVMValueRef arg_val  = LLVMBuildLoad2(ctx->builder, ptr_t,
                                               arg_slot, "arg_val");

                    /* op is a compile-time constant — specialize the trampoline body */
                    const char *op = head->symbol;
                    LLVMValueRef ret_val;

                    if (strcmp(op, "=") == 0 || strcmp(op, "!=") == 0) {
                        /* Use rt_equal_p — works for Int, Char, String, Keyword,
                         * List, Array, Map, Set, Ratio, etc.                    */
                        LLVMTypeRef  eq_params[] = {ptr_t, ptr_t};
                        LLVMTypeRef  eq_ft       = LLVMFunctionType(
                                                       LLVMInt32TypeInContext(ctx->context),
                                                       eq_params, 2, 0);
                        LLVMValueRef eq_args[]   = {arg_val, cap_val};
                        LLVMValueRef eq_i32      = LLVMBuildCall2(ctx->builder, eq_ft,
                                                       get_rt_equal_p(ctx),
                                                       eq_args, 2, "eq_p");
                        /* eq_i32 is 1 (equal) or 0 (not equal) */
                        LLVMValueRef result_i32;
                        if (strcmp(op, "!=") == 0) {
                            /* flip: xor with 1 */
                            result_i32 = LLVMBuildXor(ctx->builder, eq_i32,
                                             LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 1, 0),
                                             "neq");
                        } else {
                            result_i32 = eq_i32;
                        }
                        LLVMValueRef result_i64 = LLVMBuildZExt(ctx->builder, result_i32, i64, "pcmp_i64");
                        ret_val = LLVMBuildCall2(ctx->builder,
                                      LLVMFunctionType(ptr_t, &i64, 1, 0),
                                      get_rt_value_int(ctx), &result_i64, 1, "pcmp_box");
                    } else {
                        /* Ordering ops (<, >, <=, >=): unbox to i64 and compare */
                        LLVMTypeRef  uft     = LLVMFunctionType(i64, &ptr_t, 1, 0);
                        LLVMValueRef lhs_int = LLVMBuildCall2(ctx->builder, uft,
                                                  get_rt_unbox_int(ctx), &arg_val, 1, "lhs_i");
                        LLVMValueRef rhs_int = LLVMBuildCall2(ctx->builder, uft,
                                                  get_rt_unbox_int(ctx), &cap_val, 1, "rhs_i");
                        LLVMIntPredicate pred =
                            strcmp(op, "<")  == 0 ? LLVMIntSLT :
                            strcmp(op, ">")  == 0 ? LLVMIntSGT :
                            strcmp(op, "<=") == 0 ? LLVMIntSLE : LLVMIntSGE;
                        LLVMValueRef cmp_i1  = LLVMBuildICmp(ctx->builder, pred,
                                                  lhs_int, rhs_int, "pcmp");
                        LLVMValueRef cmp_i64 = LLVMBuildZExt(ctx->builder, cmp_i1, i64, "pcmp_i64");
                        ret_val = LLVMBuildCall2(ctx->builder,
                                      LLVMFunctionType(ptr_t, &i64, 1, 0),
                                      get_rt_value_int(ctx), &cmp_i64, 1, "pcmp_box");
                    }
                    LLVMBuildRet(ctx->builder, ret_val);

                    LLVMPositionBuilderAtEnd(ctx->builder, saved_bb);

                    /* Build env: [op_ptr, boxed_captured_arg] — heap alloc so closure outlives stack frame */
                    LLVMTypeRef  env_arr_t = LLVMArrayType(ptr_t, 2);
                    LLVMValueRef env_sz    = LLVMConstInt(LLVMInt64TypeInContext(ctx->context),
                                                          2 * sizeof(void*), 0);
                    LLVMValueRef malloc_fn2 = LLVMGetNamedFunction(ctx->module, "malloc");
                    if (!malloc_fn2) {
                        LLVMTypeRef i64t2 = LLVMInt64TypeInContext(ctx->context);
                        LLVMTypeRef ft2   = LLVMFunctionType(ptr_t, &i64t2, 1, 0);
                        malloc_fn2 = LLVMAddFunction(ctx->module, "malloc", ft2);
                        LLVMSetLinkage(malloc_fn2, LLVMExternalLinkage);
                    }
                    LLVMValueRef env_arr   = LLVMBuildCall2(ctx->builder,
                        LLVMFunctionType(ptr_t, &(LLVMTypeRef){LLVMInt64TypeInContext(ctx->context)}, 1, 0),
                        malloc_fn2, &env_sz, 1, "pcmp_env");
                    LLVMValueRef s0 = LLVMBuildGEP2(ctx->builder, env_arr_t, env_arr,
                                         (LLVMValueRef[]){LLVMConstInt(i32,0,0),LLVMConstInt(i32,0,0)}, 2, "s0");
                    LLVMValueRef s1 = LLVMBuildGEP2(ctx->builder, env_arr_t, env_arr,
                                         (LLVMValueRef[]){LLVMConstInt(i32,0,0),LLVMConstInt(i32,1,0)}, 2, "s1");
                    LLVMBuildStore(ctx->builder, op_global, s0);
                    LLVMBuildStore(ctx->builder, boxed, s1);
                    LLVMValueRef env_ptr = LLVMBuildBitCast(ctx->builder, env_arr, ptr_t, "pcmp_env_ptr");

                    /* Wrap in rt_value_closure(tramp, env, 1_captured, 1_remaining) */
                    LLVMValueRef fn_ptr   = LLVMBuildBitCast(ctx->builder, tramp, ptr_t, "fn_ptr");
                    LLVMValueRef clo_fn   = get_rt_value_closure(ctx);
                    LLVMTypeRef  clo_p[]  = {ptr_t, ptr_t, i32, i32};
                    LLVMTypeRef  clo_ft   = LLVMFunctionType(ptr_t, clo_p, 4, 0);
                    LLVMValueRef clo_args[] = {
                        fn_ptr, env_ptr,
                        LLVMConstInt(i32, 2, 0),
                        LLVMConstInt(i32, 1, 0)
                    };
                    result.value = LLVMBuildCall2(ctx->builder, clo_ft,
                                                  clo_fn, clo_args, 4, "partial_cmp_clo");
                    result.type  = type_fn(NULL, 0, NULL);
                    return result;
                }

                if (ast->list.count != 3) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' requires 2 arguments",
                                  parser_get_filename(), ast->line, ast->column, head->symbol);
                }
                const char *op = head->symbol;

        /// Typeclass dispatch

                if (tc_is_method(ctx->tc_registry, op)) {
                    const char *cls = tc_method_class(ctx->tc_registry, op);

                    CodegenResult lhs = codegen_expr(ctx, ast->list.items[1]);
                    CodegenResult rhs = codegen_expr(ctx, ast->list.items[2]);

                    /* Skip typeclass dispatch for internal pmatch-generated
                     * __adt_tag comparisons — these are integer comparisons
                     * emitted by pmatch_desugar inside instance method bodies,
                     * not user-level typeclass calls.                         */
                    {
                        bool is_adt_tag_call = false;
                        AST *arg1 = ast->list.items[1];
                        AST *arg2 = ast->list.items[2];
                        /* (= (__adt_tag x) __adt_tag_Ctor) pattern */
                        if (arg1->type == AST_LIST && arg1->list.count > 0 &&
                            arg1->list.items[0]->type == AST_SYMBOL &&
                            strcmp(arg1->list.items[0]->symbol, "__adt_tag") == 0)
                            is_adt_tag_call = true;
                        if (arg2->type == AST_SYMBOL &&
                            strncmp(arg2->symbol, "__adt_tag_", 10) == 0)
                            is_adt_tag_call = true;
                        if (is_adt_tag_call) goto normal_comparison;
                    }

                    /* Determine concrete type name from lhs */
                    const char *type_name = NULL;
                    if (lhs.type && lhs.type->kind == TYPE_LAYOUT)
                        type_name = lhs.type->layout_name;

                    if (!type_name && ast->list.items[1]->type == AST_SYMBOL) {
                        EnvEntry *e = env_lookup(ctx->env, ast->list.items[1]->symbol);
                        if (e && e->type && e->type->kind == TYPE_LAYOUT)
                            type_name = e->type->layout_name;
                    }

                    /* If we can't determine a concrete layout type, the call is
                     * either inside a polymorphic default implementation or an
                     * internal helper — fall through to normal comparison.     */
                    if (!type_name) goto normal_comparison;

                    /* We have a concrete type — check an instance exists */
                    TCInstance *inst = tc_find_instance(ctx->tc_registry, cls, type_name);
                    if (!inst) {
                        CODEGEN_ERROR(ctx,
                            "%s:%d:%d: error:\n"
                            "    • No instance for ‘%s %s’ arising from a use of ‘%s’\n"
                            "    • In the expression: (%s %s %s)\n"
                            "  - Hint: add ‘deriving %s’ to the ‘%s’ data declaration,\n"
                            "  - or define it manually: (instance %s %s where ...)",
                            parser_get_filename(), ast->line, ast->column,
                            cls, type_name, op,
                            op,
                            ast->list.items[1]->type == AST_SYMBOL ? ast->list.items[1]->symbol : "?",
                            ast->list.items[2]->type == AST_SYMBOL ? ast->list.items[2]->symbol : "?",
                            cls, type_name,
                            cls, type_name);
                    }

                    /* Find method in instance */
                    LLVMValueRef method_fn = NULL;
                    for (int _mi = 0; _mi < inst->method_count; _mi++) {
                        if (strcmp(inst->method_names[_mi], op) == 0) {
                            method_fn = inst->method_funcs[_mi];
                            break;
                        }
                    }

                    if (!method_fn) {
                        CODEGEN_ERROR(ctx,
                            "%s:%d:%d: error: instance ‘%s %s’ has no implementation for ‘%s’",
                            parser_get_filename(), ast->line, ast->column,
                            cls, type_name, op);
                    }

                    /* Re-declare in current module if needed */
                    const char  *fn_name = LLVMGetValueName(method_fn);
                    LLVMTypeRef  fn_t    = LLVMGlobalGetValueType(method_fn);
                    LLVMValueRef fn      = LLVMGetNamedFunction(ctx->module, fn_name);
                    if (!fn) {
                        fn = LLVMAddFunction(ctx->module, fn_name, fn_t);
                        LLVMSetLinkage(fn, LLVMExternalLinkage);
                    }

                    LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef  i32_t = LLVMInt32TypeInContext(ctx->context);
                    LLVMTypeRef  i64_t = LLVMInt64TypeInContext(ctx->context);

                    /* Pass raw values — typed ABI methods take the concrete
                     * type directly (ADT heap ptr, int, etc.)               */
                    LLVMValueRef lv = lhs.value;
                    LLVMValueRef rv = rhs.value;
                    /* Ensure ptr type for layout args */
                    if (LLVMGetTypeKind(LLVMTypeOf(lv)) != LLVMPointerTypeKind &&
                        lhs.type && lhs.type->kind != TYPE_LAYOUT)
                        lv = codegen_box(ctx, lv, lhs.type);
                    if (LLVMGetTypeKind(LLVMTypeOf(rv)) != LLVMPointerTypeKind &&
                        rhs.type && rhs.type->kind != TYPE_LAYOUT)
                        rv = codegen_box(ctx, rv, rhs.type);

                    /* Method uses closure ABI: (ptr env, i32 n, ptr args[]) -> ptr
                     * Build a 2-element args array on the stack and call directly */
                    LLVMTypeRef  arr_t   = LLVMArrayType(ptr_t, 2);
                    LLVMValueRef arr_ptr = LLVMBuildAlloca(ctx->builder, arr_t, "tc_args");
                    LLVMValueRef zero    = LLVMConstInt(i32_t, 0, 0);
                    LLVMValueRef one     = LLVMConstInt(i32_t, 1, 0);
                    LLVMValueRef slot0   = LLVMBuildGEP2(ctx->builder, arr_t, arr_ptr,
                                              (LLVMValueRef[]){zero, zero}, 2, "slot0");
                    LLVMValueRef slot1   = LLVMBuildGEP2(ctx->builder, arr_t, arr_ptr,
                                              (LLVMValueRef[]){zero, one},  2, "slot1");
                    LLVMBuildStore(ctx->builder, lv, slot0);
                    LLVMBuildStore(ctx->builder, rv, slot1);
                    LLVMValueRef args_ptr = LLVMBuildBitCast(ctx->builder,
                                               arr_ptr, ptr_t, "tc_args_ptr");

                    /* Look up the env entry to determine ABI */
                    const char *impl_fn_name = LLVMGetValueName(fn);
                    EnvEntry   *impl_entry   = env_lookup(ctx->env, impl_fn_name);
                    bool        impl_is_clo  = impl_entry && impl_entry->is_closure_abi;
                    /* { */
                    /*     char *ir = LLVMPrintValueToString(fn); */
                    /*     fprintf(stderr, "DEBUG tc dispatch: fn=‘%s’ is_clo=%d\n  IR: %s\n", */
                    /*             impl_fn_name, (int)impl_is_clo, ir); */
                    /*     LLVMDisposeMessage(ir); */
                    /* } */

                    LLVMValueRef call_r;
                    if (impl_is_clo) {
                        /* Closure ABI: fn(null_env, 2, args[]) -> ptr */
                        LLVMTypeRef  clo_params[] = {ptr_t, i32_t, ptr_t};
                        LLVMTypeRef  clo_ft       = LLVMFunctionType(ptr_t, clo_params, 3, 0);
                        LLVMValueRef clo_args[]   = {
                            LLVMConstPointerNull(ptr_t),
                            LLVMConstInt(i32_t, 2, 0),
                            args_ptr
                        };
                        call_r = LLVMBuildCall2(ctx->builder, clo_ft,
                                                fn, clo_args, 3, "tc_call");
                        /* Unbox ptr result to i1 */
                        LLVMTypeRef  uft     = LLVMFunctionType(i64_t, &ptr_t, 1, 0);
                        LLVMValueRef unboxed = LLVMBuildCall2(ctx->builder, uft,
                                                  get_rt_unbox_int(ctx), &call_r, 1, "tc_unbox");
                        result.value = LLVMBuildICmp(ctx->builder, LLVMIntNE,
                                           unboxed, LLVMConstInt(i64_t, 0, 0), "tc_bool");
                    } else {
                        /* Typed ABI: fn(ptr arg0, ptr arg1) -> ptr (boxed result) */
                        LLVMTypeRef  typed_params[] = {ptr_t, ptr_t};
                        LLVMTypeRef  actual_ret     = LLVMGetReturnType(
                                                          LLVMGlobalGetValueType(fn));
                        LLVMTypeRef  typed_ft       = LLVMFunctionType(
                                                          actual_ret, typed_params, 2, 0);
                        LLVMValueRef typed_args[]   = {lv, rv};
                        call_r = LLVMBuildCall2(ctx->builder, typed_ft,
                                                fn, typed_args, 2, "tc_call");
                        /* Unbox ptr result → i64 → i1 */
                        if (LLVMGetTypeKind(actual_ret) == LLVMPointerTypeKind) {
                            LLVMTypeRef  uft     = LLVMFunctionType(i64_t, &ptr_t, 1, 0);
                            LLVMValueRef unboxed = LLVMBuildCall2(ctx->builder, uft,
                                                      get_rt_unbox_int(ctx), &call_r, 1, "tc_unbox");
                            result.value = LLVMBuildICmp(ctx->builder, LLVMIntNE,
                                               unboxed, LLVMConstInt(i64_t, 0, 0), "tc_bool");
                        } else {
                            /* Already i1 */
                            result.value = LLVMBuildTrunc(ctx->builder, call_r,
                                               LLVMInt1TypeInContext(ctx->context), "tc_bool");
                        }
                    }
                    result.type  = type_bool();
                    return result;
                }

                /* ── end typeclass dispatch ─────────────────────────────── */

              normal_comparison:;
                CodegenResult lhs = codegen_expr(ctx, ast->list.items[1]);
                CodegenResult rhs = codegen_expr(ctx, ast->list.items[2]);

                bool lhs_is_ptr_type = lhs.type && (lhs.type->kind == TYPE_SET     ||
                                                    lhs.type->kind == TYPE_MAP     ||
                                                    lhs.type->kind == TYPE_LIST    ||
                                                    lhs.type->kind == TYPE_COLL    ||
                                                    lhs.type->kind == TYPE_ARR     ||
                                                    lhs.type->kind == TYPE_RATIO   ||
                                                    lhs.type->kind == TYPE_UNKNOWN);
                bool rhs_is_ptr_type = rhs.type && (rhs.type->kind == TYPE_SET     ||
                                                    rhs.type->kind == TYPE_MAP     ||
                                                    rhs.type->kind == TYPE_LIST    ||
                                                    rhs.type->kind == TYPE_COLL    ||
                                                    rhs.type->kind == TYPE_ARR     ||
                                                    rhs.type->kind == TYPE_RATIO   ||
                                                    rhs.type->kind == TYPE_UNKNOWN);

                /* Collections (set, map, list, ratio): only = and != are defined */
                if (lhs_is_ptr_type || rhs_is_ptr_type) {
                    if (strcmp(op, "=") != 0 && strcmp(op, "!=") != 0) {
                        /* Unbox both sides as integers for ordering comparisons */
                        LLVMTypeRef ptr_ub = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                        LLVMTypeRef i64_ub = LLVMInt64TypeInContext(ctx->context);
                        if (lhs_is_ptr_type) {
                            LLVMTypeRef uft = LLVMFunctionType(i64_ub, &ptr_ub, 1, 0);
                            lhs.value = LLVMBuildCall2(ctx->builder, uft,
                                            get_rt_unbox_int(ctx), &lhs.value, 1, "unbox_lhs");
                            lhs.type  = type_int();
                            lhs_is_ptr_type = false;
                        }
                        if (rhs_is_ptr_type) {
                            LLVMTypeRef uft = LLVMFunctionType(i64_ub, &ptr_ub, 1, 0);
                            rhs.value = LLVMBuildCall2(ctx->builder, uft,
                                            get_rt_unbox_int(ctx), &rhs.value, 1, "unbox_rhs");
                            rhs.type  = type_int();
                            rhs_is_ptr_type = false;
                        }
                    }

                    LLVMTypeRef  ptr       = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef  i32       = LLVMInt32TypeInContext(ctx->context);
                    LLVMValueRef fn        = get_rt_equal_p(ctx);
                    LLVMTypeRef  ft_args[] = {ptr, ptr};
                    LLVMTypeRef  ft        = LLVMFunctionType(i32, ft_args, 2, 0);

                    bool lhs_is_boxed = lhs.type && (lhs.type->kind == TYPE_COLL  ||
                                                     lhs.type->kind == TYPE_ARR   ||
                                                     lhs.type->kind == TYPE_RATIO ||
                                                     lhs.type->kind == TYPE_UNKNOWN);
                    bool rhs_is_boxed = rhs.type && (rhs.type->kind == TYPE_COLL  ||
                                                     rhs.type->kind == TYPE_ARR   ||
                                                     rhs.type->kind == TYPE_RATIO ||
                                                     rhs.type->kind == TYPE_UNKNOWN);
                    LLVMValueRef lv = lhs_is_boxed ? lhs.value : codegen_box(ctx, lhs.value, lhs.type);
                    LLVMValueRef rv = rhs_is_boxed ? rhs.value : codegen_box(ctx, rhs.value, rhs.type);

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

                /* Unbox unknown/collection types before comparison */
                LLVMTypeRef ptr_t2 = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef i64_t  = LLVMInt64TypeInContext(ctx->context);
                if (lhs.type && (lhs.type->kind == TYPE_UNKNOWN ||
                                 lhs.type->kind == TYPE_LIST    ||
                                 lhs.type->kind == TYPE_COLL)) {
                    LLVMTypeRef uft = LLVMFunctionType(i64_t, &ptr_t2, 1, 0);
                    lv = LLVMBuildCall2(ctx->builder, uft,
                             get_rt_unbox_int(ctx), &lv, 1, "unbox_lhs");
                    lhs.type = type_int();
                }
                if (rhs.type && (rhs.type->kind == TYPE_UNKNOWN ||
                                 rhs.type->kind == TYPE_LIST    ||
                                 rhs.type->kind == TYPE_COLL)) {
                    LLVMTypeRef uft = LLVMFunctionType(i64_t, &ptr_t2, 1, 0);
                    rv = LLVMBuildCall2(ctx->builder, uft,
                             get_rt_unbox_int(ctx), &rv, 1, "unbox_rhs");
                    rhs.type = type_int();
                }

                bool use_float = type_is_float(lhs.type) || type_is_float(rhs.type);
                if (use_float) {
                    /* Use F32 only if both sides are F32, otherwise promote to double */
                    bool both_f32 = (lhs.type && lhs.type->kind == TYPE_F32) &&
                                    (rhs.type && rhs.type->kind == TYPE_F32);
                    LLVMTypeRef target_float = both_f32
                        ? LLVMFloatTypeInContext(ctx->context)
                        : LLVMDoubleTypeInContext(ctx->context);
                    if (type_is_integer(lhs.type))
                        lv = LLVMBuildSIToFP(ctx->builder, lv, target_float, "lf");
                    else if (LLVMTypeOf(lv) != target_float)
                        lv = LLVMBuildFPExt(ctx->builder, lv, target_float, "lf_ext");
                    if (type_is_integer(rhs.type))
                        rv = LLVMBuildSIToFP(ctx->builder, rv, target_float, "rf");
                    else if (LLVMTypeOf(rv) != target_float)
                        rv = LLVMBuildFPExt(ctx->builder, rv, target_float, "rf_ext");
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
                    /* Promote integers to the same bit-width before comparing */
                    unsigned lw = LLVMGetIntTypeWidth(LLVMTypeOf(lv));
                    unsigned rw = LLVMGetIntTypeWidth(LLVMTypeOf(rv));
                    if (lw > rw) {
                        rv = LLVMBuildSExt(ctx->builder, rv, LLVMTypeOf(lv), "sext");
                    } else if (rw > lw) {
                        lv = LLVMBuildSExt(ctx->builder, lv, LLVMTypeOf(rv), "sext");
                    }

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
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: ‘%s’ requires at least 1 argument",
                            parser_get_filename(), ast->line, ast->column, op);
                }

                CodegenResult first = codegen_expr(ctx, ast->list.items[1]);
                Type         *result_type  = first.type;
                LLVMValueRef  result_value = first.value;

                /* Bypass broken type_is_numeric by using a blacklist */
                if (result_type->kind == TYPE_LAYOUT || result_type->kind == TYPE_STRING) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: cannot perform arithmetic on type %s",
                            parser_get_filename(), ast->line, ast->column, type_to_string(result_type));
                }

                // Coerce Char to Int upfront
                if (result_type->kind == TYPE_CHAR) {
                    result_value = LLVMBuildZExt(ctx->builder, result_value,
                                                 LLVMInt64TypeInContext(ctx->context), "char_to_int");
                    result_type  = type_int();
                }
                // Coerce TYPE_UNKNOWN or TYPE_FN ptr to Int via unbox
                if (result_type->kind == TYPE_UNKNOWN ||
                    result_type->kind == TYPE_FN) {
                    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);
                    LLVMTypeRef uft   = LLVMFunctionType(i64_t, &ptr_t, 1, 0);
                    result_value = LLVMBuildCall2(ctx->builder, uft,
                                       get_rt_unbox_int(ctx), &result_value, 1, "unbox_arith");
                    result_type  = type_int();
                }

                if (ast->list.count == 2) {
                    /* Partial application: (+ 5) => lambda y -> (+ 5 y) */
                    LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef  i32   = LLVMInt32TypeInContext(ctx->context);
                    LLVMTypeRef  i64   = LLVMInt64TypeInContext(ctx->context);

                    /* Codegen the single supplied arg and box it */
                    LLVMValueRef boxed = codegen_box(ctx, result_value, result_type);

                    /* Store op string and boxed arg in env: [op_ptr, boxed_arg] */
                    LLVMValueRef op_global = LLVMBuildGlobalStringPtr(ctx->builder,
                                                op, "partial_op");

                    /* Build trampoline: (ptr env, i32 n, ptr args[]) -> ptr */
                    static int partial_arith_count = 0;
                    char tname[64];
                    snprintf(tname, sizeof(tname), "__partial_arith_%d", partial_arith_count++);

                    LLVMTypeRef  tramp_params[] = {ptr_t, i32, ptr_t};
                    LLVMTypeRef  tramp_ft = LLVMFunctionType(ptr_t, tramp_params, 3, 0);
                    LLVMValueRef tramp    = LLVMAddFunction(ctx->module, tname, tramp_ft);
                    LLVMBasicBlockRef saved_bb    = LLVMGetInsertBlock(ctx->builder);
                    LLVMBasicBlockRef tramp_entry = LLVMAppendBasicBlockInContext(
                                                       ctx->context, tramp, "entry");
                    LLVMPositionBuilderAtEnd(ctx->builder, tramp_entry);

                    /* env[0] = op string ptr, env[1] = boxed captured arg */
                    LLVMValueRef env_p  = LLVMGetParam(tramp, 0);
                    LLVMValueRef args_p = LLVMGetParam(tramp, 2);

                    /* Load captured (lhs) value from env[1] */
                    LLVMValueRef cap_idx  = LLVMConstInt(i32, 1, 0);
                    LLVMValueRef cap_slot = LLVMBuildGEP2(ctx->builder, ptr_t,
                                               env_p, &cap_idx, 1, "cap_slot");
                    LLVMValueRef cap_val  = LLVMBuildLoad2(ctx->builder, ptr_t,
                                               cap_slot, "cap_val");

                    /* Load the incoming (rhs) arg from args[0] */
                    LLVMValueRef zero_idx = LLVMConstInt(i32, 0, 0);
                    LLVMValueRef arg_slot = LLVMBuildGEP2(ctx->builder, ptr_t,
                                               args_p, &zero_idx, 1, "arg_slot");
                    LLVMValueRef arg_val  = LLVMBuildLoad2(ctx->builder, ptr_t,
                                               arg_slot, "arg_val");

                    /* Unbox to i64 and compute */
                    LLVMTypeRef  uft     = LLVMFunctionType(i64, &ptr_t, 1, 0);
                    LLVMValueRef lhs_int = LLVMBuildCall2(ctx->builder, uft,
                                              get_rt_unbox_int(ctx), &cap_val, 1, "lhs_i");
                    LLVMValueRef rhs_int = LLVMBuildCall2(ctx->builder, uft,
                                              get_rt_unbox_int(ctx), &arg_val, 1, "rhs_i");

                    LLVMValueRef calc_i64;
                    if (strcmp(op, "+") == 0) {
                        calc_i64 = LLVMBuildAdd(ctx->builder, lhs_int, rhs_int, "padd");
                    } else if (strcmp(op, "-") == 0) {
                        calc_i64 = LLVMBuildSub(ctx->builder, lhs_int, rhs_int, "psub");
                    } else if (strcmp(op, "*") == 0) {
                        calc_i64 = LLVMBuildMul(ctx->builder, lhs_int, rhs_int, "pmul");
                    } else {
                        calc_i64 = LLVMBuildSDiv(ctx->builder, lhs_int, rhs_int, "pdiv");
                    }

                    LLVMValueRef ret_val = LLVMBuildCall2(ctx->builder,
                                              LLVMFunctionType(ptr_t, &i64, 1, 0),
                                              get_rt_value_int(ctx), &calc_i64, 1, "parith_box");

                    LLVMBuildRet(ctx->builder, ret_val);

                    LLVMPositionBuilderAtEnd(ctx->builder, saved_bb);

                    /* Build env: [op_ptr, boxed_captured_arg] */
                    LLVMTypeRef  env_arr_t = LLVMArrayType(ptr_t, 2);
                    LLVMValueRef env_sz    = LLVMConstInt(LLVMInt64TypeInContext(ctx->context),
                                                          2 * sizeof(void*), 0);
                    LLVMValueRef malloc_fn2 = LLVMGetNamedFunction(ctx->module, "malloc");
                    if (!malloc_fn2) {
                        LLVMTypeRef i64t2 = LLVMInt64TypeInContext(ctx->context);
                        LLVMTypeRef ft2   = LLVMFunctionType(ptr_t, &i64t2, 1, 0);
                        malloc_fn2 = LLVMAddFunction(ctx->module, "malloc", ft2);
                        LLVMSetLinkage(malloc_fn2, LLVMExternalLinkage);
                    }
                    LLVMValueRef env_arr   = LLVMBuildCall2(ctx->builder,
                        LLVMFunctionType(ptr_t, &(LLVMTypeRef){LLVMInt64TypeInContext(ctx->context)}, 1, 0),
                        malloc_fn2, &env_sz, 1, "parith_env");
                    LLVMValueRef s0 = LLVMBuildGEP2(ctx->builder, env_arr_t, env_arr,
                                         (LLVMValueRef[]){LLVMConstInt(i32,0,0),LLVMConstInt(i32,0,0)}, 2, "s0");
                    LLVMValueRef s1 = LLVMBuildGEP2(ctx->builder, env_arr_t, env_arr,
                                         (LLVMValueRef[]){LLVMConstInt(i32,0,0),LLVMConstInt(i32,1,0)}, 2, "s1");
                    LLVMBuildStore(ctx->builder, op_global, s0);
                    LLVMBuildStore(ctx->builder, boxed, s1);
                    LLVMValueRef env_ptr = LLVMBuildBitCast(ctx->builder, env_arr, ptr_t, "parith_env_ptr");

                    /* Wrap in rt_value_closure(tramp, env, 1_captured, 1_remaining) */
                    LLVMValueRef fn_ptr   = LLVMBuildBitCast(ctx->builder, tramp, ptr_t, "fn_ptr");
                    LLVMValueRef clo_fn   = get_rt_value_closure(ctx);
                    LLVMTypeRef  clo_p[]  = {ptr_t, ptr_t, i32, i32};
                    LLVMTypeRef  clo_ft   = LLVMFunctionType(ptr_t, clo_p, 4, 0);
                    LLVMValueRef clo_args[] = {
                        fn_ptr, env_ptr,
                        LLVMConstInt(i32, 2, 0),
                        LLVMConstInt(i32, 1, 0)
                    };
                    result.value = LLVMBuildCall2(ctx->builder, clo_ft,
                                                  clo_fn, clo_args, 4, "partial_arith_clo");
                    result.type  = type_fn(NULL, 0, NULL);
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

                    if (rhs.type->kind == TYPE_LAYOUT || rhs.type->kind == TYPE_STRING) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: cannot perform arithmetic on type %s",
                                parser_get_filename(), ast->line, ast->column, type_to_string(rhs.type));
                    }

                    // Coerce Char rhs to Int
                    if (rhs.type->kind == TYPE_CHAR) {
                        rhs.value = LLVMBuildZExt(ctx->builder, rhs.value,
                                                  LLVMInt64TypeInContext(ctx->context), "char_to_int");
                        rhs.type  = type_int();
                    }
                    // Coerce TYPE_UNKNOWN or TYPE_FN rhs to Int via unbox
                    if (rhs.type->kind == TYPE_UNKNOWN ||
                        rhs.type->kind == TYPE_FN) {
                        LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                        LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);
                        LLVMTypeRef uft   = LLVMFunctionType(i64_t, &ptr_t, 1, 0);
                        rhs.value = LLVMBuildCall2(ctx->builder, uft,
                                        get_rt_unbox_int(ctx), &rhs.value, 1, "unbox_arith");
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

                    // Convert operands to common type
                    Type *new_result_type;
                    LLVMValueRef lhs_val = result_value;
                    LLVMValueRef rhs_val = rhs.value;

                    bool lhs_is_float = type_is_float(result_type);
                    bool rhs_is_float = type_is_float(rhs.type);

                    // Pointer arithmetic: ptr + int or int + ptr
                    if ((result_type->kind == TYPE_PTR || LLVMGetTypeKind(LLVMTypeOf(result_value)) == LLVMPointerTypeKind) && !type_is_float(rhs.type)) {
                        LLVMTypeRef i8_t = LLVMInt8TypeInContext(ctx->context);
                        LLVMValueRef ptr_as_i8 = LLVMBuildBitCast(ctx->builder, result_value,
                                                     LLVMPointerType(i8_t, 0), "ptr_i8");
                        result_value = LLVMBuildGEP2(ctx->builder, i8_t, ptr_as_i8,
                                                      &rhs.value, 1, "ptrtmp");
                        result_type = rhs.type->kind == TYPE_PTR ? result_type : result_type;
                        continue;
                    }

                    if (lhs_is_float || rhs_is_float) {
                        // Float promotion (default to the largest float type)
                        new_result_type = type_float();
                        LLVMTypeRef target_llvm = LLVMDoubleTypeInContext(ctx->context);

                        if (!lhs_is_float) lhs_val = LLVMBuildSIToFP(ctx->builder, lhs_val, target_llvm, "tofloat");
                        else if (LLVMTypeOf(lhs_val) != target_llvm) lhs_val = LLVMBuildFPExt(ctx->builder, lhs_val, target_llvm, "fpext");

                        if (!rhs_is_float) rhs_val = LLVMBuildSIToFP(ctx->builder, rhs_val, target_llvm, "tofloat");
                        else if (LLVMTypeOf(rhs_val) != target_llvm) rhs_val = LLVMBuildFPExt(ctx->builder, rhs_val, target_llvm, "fpext");
                    } else {
                        // Integer promotion: promote the smaller bit-width to the larger bit-width
                        unsigned lhs_bits = LLVMGetIntTypeWidth(LLVMTypeOf(lhs_val));
                        unsigned rhs_bits = LLVMGetIntTypeWidth(LLVMTypeOf(rhs_val));

                        if (lhs_bits > rhs_bits) {
                            new_result_type = result_type;
                            rhs_val = LLVMBuildSExt(ctx->builder, rhs_val, LLVMTypeOf(lhs_val), "sext");
                        } else if (rhs_bits > lhs_bits) {
                            new_result_type = rhs.type;
                            lhs_val = LLVMBuildSExt(ctx->builder, lhs_val, LLVMTypeOf(rhs_val), "sext");
                        } else {
                            new_result_type = result_type; // Same size
                        }
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
                entry->type && entry->type->kind == TYPE_STRING &&
                ast->list.count == 2) {
                /* String used as function: (str i) -> char at index i */
                LLVMTypeRef  i8  = LLVMInt8TypeInContext(ctx->context);
                LLVMTypeRef  i64 = LLVMInt64TypeInContext(ctx->context);
                LLVMValueRef str_val = LLVMBuildLoad2(ctx->builder,
                                           type_to_llvm(ctx, entry->type),
                                           entry->value, head->symbol);
                CodegenResult idx_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef idx = type_is_float(idx_r.type)
                    ? LLVMBuildFPToSI(ctx->builder, idx_r.value, i64, "idx")
                    : idx_r.value;
                if (LLVMTypeOf(idx) != i64)
                    idx = LLVMBuildZExt(ctx->builder, idx, i64, "idx64");
                LLVMValueRef char_ptr = LLVMBuildGEP2(ctx->builder, i8, str_val, &idx, 1, "char_ptr");
                result.value = LLVMBuildLoad2(ctx->builder, i8, char_ptr, "char");
                result.type  = type_char();
                return result;
            }

            if (entry && entry->kind == ENV_VAR &&
                entry->type && (entry->type->kind == TYPE_ARR ||
                                (entry->type->kind == TYPE_UNKNOWN &&
                                 ast->list.count == 2))) {
                /* Array used as function: (arr i) -> element at index i */
                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: array used as function requires exactly 1 argument (index)",
                                  parser_get_filename(), ast->line, ast->column);
                }

                LLVMTypeRef  i64      = LLVMInt64TypeInContext(ctx->context);
                LLVMTypeRef  i32      = LLVMInt32TypeInContext(ctx->context);
                LLVMTypeRef  ptr_t    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                bool arr_is_typed = (entry->type->kind == TYPE_ARR);
                LLVMTypeRef  arr_llvm = arr_is_typed
                    ? type_to_llvm(ctx, entry->type) : ptr_t;

                /* Derive elem_type early — needed by fat pointer path */
                Type *elem_type = (arr_is_typed &&
                                   entry->type->arr_element_type &&
                                   entry->type->arr_element_type->kind != TYPE_UNKNOWN)
                                ? entry->type->arr_element_type
                                : type_int();

                /* Re-resolve globals by name so cross-module refs work */
                LLVMValueRef arr_val = entry->value;
                bool is_global = arr_val &&
                    LLVMGetValueKind(arr_val) == LLVMGlobalVariableValueKind;
                if (is_global && entry->type && entry->type->arr_is_fat) {
                    /* Global fat array — global holds i8* to fat struct.
                     * Load the i8*, cast to arr.fat*, extract data ptr. */
                    LLVMTypeRef ptr_t  = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    const char *gname  = (entry->llvm_name && entry->llvm_name[0])
                                       ? entry->llvm_name : entry->name;
                    LLVMValueRef gv    = LLVMGetNamedGlobal(ctx->module, gname);
                    if (!gv) {
                        gv = LLVMAddGlobal(ctx->module, ptr_t, gname);
                        LLVMSetLinkage(gv, LLVMExternalLinkage);
                    }
                    LLVMValueRef fat_i8 = LLVMBuildLoad2(ctx->builder, ptr_t, gv, "fat_i8");
                    LLVMTypeRef  fat_t  = get_arr_fat_type(ctx);
                    LLVMTypeRef  fat_pt = LLVMPointerType(fat_t, 0);
                    LLVMValueRef fat    = LLVMBuildBitCast(ctx->builder, fat_i8, fat_pt, "fat");
                    arr_val  = arr_fat_data(ctx, fat, elem_type);
                    arr_llvm = LLVMPointerType(type_to_llvm(ctx, elem_type), 0);
                } else if (is_global) {
                    const char *gname = (entry->llvm_name && entry->llvm_name[0])
                        ? entry->llvm_name : entry->name;
                    LLVMValueRef gv = LLVMGetNamedGlobal(ctx->module, gname);
                    if (!gv) {
                        gv = LLVMAddGlobal(ctx->module, arr_llvm, gname);
                        LLVMSetLinkage(gv, LLVMExternalLinkage);
                    }
                    arr_val = gv;
                } else if (arr_val && entry->type &&
                           entry->type->kind == TYPE_ARR &&
                           entry->type->arr_is_fat) {
                    /* Fat pointer — value in alloca is arr.fat*
                     * Load the fat pointer, then extract data field */
                    LLVMTypeRef fat_t   = get_arr_fat_type(ctx);
                    LLVMTypeRef fat_pt  = LLVMPointerType(fat_t, 0);
                    LLVMValueRef fat    = LLVMBuildLoad2(ctx->builder, fat_pt,
                                                         arr_val, "fat_ptr");
                    arr_val  = arr_fat_data(ctx, fat, elem_type);
                    arr_llvm = LLVMPointerType(type_to_llvm(ctx, elem_type), 0);
                } else if (arr_val &&
                           LLVMGetTypeKind(arr_llvm) != LLVMArrayTypeKind) {
                    arr_val = LLVMBuildLoad2(ctx->builder,
                        LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0),
                        arr_val, "arr_data_ptr");
                }

                CodegenResult idx_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef  idx   = type_is_float(idx_r.type)
                    ? LLVMBuildFPToSI(ctx->builder, idx_r.value, i64, "idx")
                    : idx_r.value;
                if (LLVMTypeOf(idx) != i64)
                    idx = LLVMBuildZExt(ctx->builder, idx, i64, "idx64");

                /* Bounds check for fat arrays */
                if (entry->type && entry->type->arr_is_fat) {
                    LLVMTypeRef  fat_t  = get_arr_fat_type(ctx);
                    LLVMTypeRef  fat_pt = LLVMPointerType(fat_t, 0);
                    /* Re-load fat pointer to get size */
                    LLVMValueRef fat_for_size;
                    bool _is_global = entry->value &&
                        LLVMGetValueKind(entry->value) == LLVMGlobalVariableValueKind;
                    if (_is_global) {
                        LLVMTypeRef ptr_t2 = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                        const char *gname2 = (entry->llvm_name && entry->llvm_name[0])
                                           ? entry->llvm_name : entry->name;
                        LLVMValueRef gv2 = LLVMGetNamedGlobal(ctx->module, gname2);
                        if (!gv2) {
                            gv2 = LLVMAddGlobal(ctx->module, ptr_t2, gname2);
                            LLVMSetLinkage(gv2, LLVMExternalLinkage);
                        }
                        LLVMValueRef fat_i8 = LLVMBuildLoad2(ctx->builder, ptr_t2, gv2, "fat_i8");
                        fat_for_size = LLVMBuildBitCast(ctx->builder, fat_i8, fat_pt, "fat_sz");
                    } else {
                        fat_for_size = LLVMBuildLoad2(ctx->builder, fat_pt,
                                                      entry->value, "fat_sz");
                    }
                    LLVMValueRef arr_sz = arr_fat_size(ctx, fat_for_size);

                    emit_bounds_check(ctx, ast, idx, arr_sz, "array index out of bounds");
                }

                /* Element type: use known type if available, fall back to i64. */
                LLVMTypeRef elem_llvm = type_to_llvm(ctx, elem_type);


                LLVMValueRef elem_ptr;
                bool arr_is_ptr = (LLVMGetTypeKind(arr_llvm) != LLVMArrayTypeKind);
                if (arr_is_ptr) {
                    /* Untyped/unsized Arr parameter — raw pointer, single-index GEP */
                    LLVMValueRef idx64 = idx;
                    elem_ptr = LLVMBuildGEP2(ctx->builder, elem_llvm,
                                             arr_val, &idx64, 1, "elem_ptr");
                } else {
                    /* Sized array — two-index GEP [0, i] */
                    LLVMValueRef zero      = LLVMConstInt(i32, 0, 0);
                    LLVMValueRef idx32     = LLVMBuildTrunc(ctx->builder, idx, i32, "idx32");
                    LLVMValueRef indices[] = {zero, idx32};
                    elem_ptr = LLVMBuildGEP2(ctx->builder, arr_llvm,
                                             arr_val, indices, 2, "elem_ptr");
                }
                result.value = LLVMBuildLoad2(ctx->builder, elem_llvm, elem_ptr, "elem");
                result.type  = type_clone(elem_type);
                return result;
            }

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
                entry->type && entry->type->kind == TYPE_LIST &&
                ast->list.count == 2) {

                LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef i64   = LLVMInt64TypeInContext(ctx->context);

                // Load the list pointer
                LLVMValueRef list_val = LLVMBuildLoad2(ctx->builder, ptr_t, entry->value, head->symbol);
                CodegenResult idx_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef idx = type_is_float(idx_r.type) ? LLVMBuildFPToSI(ctx->builder, idx_r.value, i64, "idx") : idx_r.value;
                if (LLVMTypeOf(idx) != i64) idx = LLVMBuildZExt(ctx->builder, idx, i64, "idx64");

                // ─── RUNTIME BOUNDS CHECK ───
                LLVMValueRef list_len = LLVMBuildCall2(ctx->builder, LLVMFunctionType(i64, &ptr_t, 1, 0), get_rt_list_length(ctx), &list_val, 1, "cur_len");
                emit_bounds_check(ctx, ast, idx, list_len, "list index out of bounds");
                result.value = LLVMBuildCall2(ctx->builder, LLVMFunctionType(ptr_t, (LLVMTypeRef[]){ptr_t, i64}, 2, 0),
                                              get_rt_list_nth(ctx), (LLVMValueRef[]){list_val, idx}, 2, "res");
                result.type = type_unknown();
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
            fprintf(stderr, "DEBUG closure-check: sym=%s entry=%p kind=%d type_kind=%d\n",
                    head->type == AST_SYMBOL ? head->symbol : "?",
                    (void*)entry,
                    entry ? entry->kind : -1,
                    (entry && entry->type) ? entry->type->kind : -1);
            if (entry && entry->kind == ENV_VAR &&
                entry->type && (entry->type->kind == TYPE_FN ||
                                entry->type->kind == TYPE_ARROW ||
                                entry->type->kind == TYPE_UNKNOWN)) {
                LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i32     = LLVMInt32TypeInContext(ctx->context);
                int          n_args  = (int)ast->list.count - 1;

                /* If this is a locally-defined inner closure with a known
                 * func_ref, call it directly instead of through rt_closure_calln.
                 * This avoids storing JIT trampoline addresses in closures that
                 * get exported to .so files.                                    */
                {
                    EnvEntry *func_entry = env_lookup(ctx->env, head->symbol);
                    if (false && func_entry && func_entry->func_ref &&
                        func_entry->is_closure_abi) {
                        /* Load the closure to get the env pointer */
                        LLVMValueRef clo_val2 = LLVMBuildLoad2(ctx->builder,
                                                    ptr, entry->value, head->symbol);
                        /* Build args array */
                        LLVMTypeRef arr_t2   = LLVMArrayType(ptr, n_args ? n_args : 1);
                        LLVMValueRef arr_ptr2 = LLVMBuildAlloca(ctx->builder, arr_t2, "direct_args");
                        for (int i = 0; i < n_args; i++) {
                            CodegenResult ar  = codegen_expr(ctx, ast->list.items[i + 1]);
                            LLVMValueRef  bv  = codegen_box(ctx, ar.value, ar.type);
                            LLVMValueRef  zero   = LLVMConstInt(i32, 0, 0);
                            LLVMValueRef  idx    = LLVMConstInt(i32, i, 0);
                            LLVMValueRef  idxs[] = {zero, idx};
                            LLVMValueRef  slot   = LLVMBuildGEP2(ctx->builder, arr_t2,
                                                                  arr_ptr2, idxs, 2, "slot");
                            LLVMBuildStore(ctx->builder, bv, slot);
                        }
                        LLVMValueRef args_ptr2 = n_args > 0
                            ? LLVMBuildBitCast(ctx->builder, arr_ptr2, ptr, "args_ptr")
                            : LLVMConstPointerNull(ptr);
                        /* Extract env from closure value via rt_closure_get_env */
                        LLVMValueRef get_env_fn = LLVMGetNamedFunction(ctx->module, "rt_closure_get_env");
                        if (!get_env_fn) {
                            LLVMTypeRef ft = LLVMFunctionType(ptr, &ptr, 1, 0);
                            get_env_fn = LLVMAddFunction(ctx->module, "rt_closure_get_env", ft);
                            LLVMSetLinkage(get_env_fn, LLVMExternalLinkage);
                        }
                        LLVMValueRef env_ptr2 = LLVMBuildCall2(ctx->builder,
                            LLVMFunctionType(ptr, &ptr, 1, 0),
                            get_env_fn, &clo_val2, 1, "clo_env");
                        /* Call directly: func(env, n, args) */
                        LLVMTypeRef  rec_params[] = {ptr, i32, ptr};
                        LLVMTypeRef  rec_ft = LLVMFunctionType(ptr, rec_params, 3, 0);
                        LLVMValueRef rec_args[] = {
                            env_ptr2,
                            LLVMConstInt(i32, n_args, 0),
                            args_ptr2
                        };
                        result.value = LLVMBuildCall2(ctx->builder, rec_ft,
                                                      func_entry->func_ref, rec_args, 3, "direct_clo_call");
                        result.type  = type_unknown();
                        /* Unbox if needed */
                        if (func_entry->return_type &&
                            func_entry->return_type->kind != TYPE_UNKNOWN) {
                            LLVMTypeRef expected = type_to_llvm(ctx, func_entry->return_type);
                            if (LLVMTypeOf(result.value) == ptr && expected != ptr) {
                                if (type_is_float(func_entry->return_type)) {
                                    LLVMTypeRef dbl = LLVMDoubleTypeInContext(ctx->context);
                                    LLVMTypeRef uft = LLVMFunctionType(dbl, &ptr, 1, 0);
                                    result.value = LLVMBuildCall2(ctx->builder, uft,
                                        get_rt_unbox_float(ctx), &result.value, 1, "unbox_direct");
                                    result.type = type_float();
                                } else if (type_is_integer(func_entry->return_type)) {
                                    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                                    LLVMTypeRef uft = LLVMFunctionType(i64, &ptr, 1, 0);
                                    result.value = LLVMBuildCall2(ctx->builder, uft,
                                        get_rt_unbox_int(ctx), &result.value, 1, "unbox_direct");
                                    result.type = type_int();
                                }
                            }
                        }
                        return result;
                    }
                }

                if (!LLVMGetInsertBlock(ctx->builder)) {
                    result.value = LLVMConstPointerNull(ptr);
                    result.type  = type_unknown();
                    return result;
                }
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

            // Pointer indexing: (ptr i) where ptr is a raw pointer/unknown type
            if (entry && entry->kind == ENV_VAR &&
                ast->list.count == 2 &&
                entry->type && (entry->type->kind == TYPE_UNKNOWN ||
                                entry->type->kind == TYPE_U64    ||
                                entry->type->kind == TYPE_I64    ||
                                entry->type->kind == TYPE_PTR)) {
                LLVMTypeRef  ptr  = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                LLVMTypeRef  i64  = LLVMInt64TypeInContext(ctx->context);
                LLVMValueRef base = LLVMBuildLoad2(ctx->builder,
                                                   type_to_llvm(ctx, entry->type),
                                                   entry->value, head->symbol);
                if (LLVMTypeOf(base) != ptr)
                    base = LLVMBuildIntToPtr(ctx->builder, base, ptr, "ptr_cast");
                CodegenResult idx_r = codegen_expr(ctx, ast->list.items[1]);
                LLVMValueRef idx = idx_r.value;
                if (LLVMTypeOf(idx) != i64)
                    idx = LLVMBuildZExt(ctx->builder, idx, i64, "idx64");
                /* If the pointer has a known layout pointee, stride by its size */
                Type *pointee = (entry->type->kind == TYPE_PTR)
                    ? entry->type->element_type : NULL;
                if (pointee && pointee->kind == TYPE_LAYOUT && pointee->layout_name) {
                    Type *full = env_lookup_layout(ctx->env, pointee->layout_name);
                    if (!full) full = pointee;
                    /* type_to_llvm returns ptr-to-struct, we need the struct itself for GEP */
                    char struct_name[256];
                    snprintf(struct_name, sizeof(struct_name), "layout.%s", full->layout_name);
                    LLVMTypeRef elem_llvm = LLVMGetTypeByName2(ctx->context, struct_name);
                    if (!elem_llvm) elem_llvm = LLVMInt8TypeInContext(ctx->context);
                    LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder,
                                                          elem_llvm, base, &idx, 1, "ptr_idx");
                    result.value = elem_ptr;
                    result.type  = type_clone(full);
                    return result;
                }
                LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder,
                                                      ptr, base, &idx, 1, "ptr_idx");
                result.value = LLVMBuildLoad2(ctx->builder, ptr, elem_ptr, "ptr_elem");
                result.type  = type_unknown();
                return result;
            }

            // If it's a variable being used in function position, that's an error
            if (entry && entry->kind == ENV_VAR) {
                /* Fn-typed variables in call position are valid — they are
                 * higher-order function parameters passed as closures.
                 * If we reach here with TYPE_FN it means the builder has no
                 * active block (pre-scan pass) — return a dummy value.      */
                if (entry->type && (entry->type->kind == TYPE_FN ||
                                    entry->type->kind == TYPE_ARROW)) {
                    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    result.value = LLVMConstPointerNull(ptr);
                    result.type  = type_unknown();
                    return result;
                }
                CODEGEN_ERROR(ctx, "%s:%d:%d: error: '%s' is a variable, not a function",
                        parser_get_filename(), ast->line, ast->column, head->symbol);
            }

            // ADT constructor call: (Circle 3.0), (Rectangle 1.0 2.0) etc.
            if (entry && entry->kind == ENV_ADT_CTOR) {
                int n_args = (int)ast->list.count - 1;
                if (entry->param_count == 0 && n_args == 0) {
                    /* Nullary — just call with no args */
                    LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMTypeRef  ft    = LLVMFunctionType(ptr_t, NULL, 0, 0);
                    const char  *fname = LLVMGetValueName(entry->func_ref);
                    LLVMValueRef fn    = LLVMGetNamedFunction(ctx->module, fname);
                    if (!fn) {
                        fn = LLVMAddFunction(ctx->module, fname, ft);
                        LLVMSetLinkage(fn, LLVMExternalLinkage);
                    }
                    result.value = LLVMBuildCall2(ctx->builder, ft, fn, NULL, 0, head->symbol);
                    result.type  = type_clone(entry->type);
                    return result;
                }
                /* Non-nullary — build param types from field_types stored at ctor time.
                 * The constructor function takes Float/Int/etc and returns ptr.        */
                LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                /* Re-declare constructor in current module if needed */
                const char  *fname  = LLVMGetValueName(entry->func_ref);
                LLVMTypeRef  fn_t   = LLVMGlobalGetValueType(entry->func_ref);
                LLVMValueRef fn     = LLVMGetNamedFunction(ctx->module, fname);
                if (!fn) {
                    fn = LLVMAddFunction(ctx->module, fname, fn_t);
                    LLVMSetLinkage(fn, LLVMExternalLinkage);
                }
                /* Codegen arguments */
                int nparams = LLVMCountParamTypes(fn_t);
                LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * (nparams ? nparams : 1));
                LLVMGetParamTypes(fn_t, param_types);
                LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (nparams ? nparams : 1));
                for (int i = 0; i < nparams && i < n_args; i++) {
                    CodegenResult ar = codegen_expr(ctx, ast->list.items[i + 1]);
                    LLVMValueRef  av = ar.value;
                    LLVMTypeRef   want = param_types[i];
                    LLVMTypeRef   have = LLVMTypeOf(av);
                    args[i] = emit_type_cast(ctx, av, want);
                }
                result.value = LLVMBuildCall2(ctx->builder, fn_t, fn, args, nparams, head->symbol);
                result.type  = type_clone(entry->type);
                free(args);
                free(param_types);
                return result;
            }

      /// Function call

            if (entry && entry->kind == ENV_FUNC) {
                /* Re-resolve non-ASCII names via mangled entry */
                {
                    char *mangled = mangle_unicode_name(head->symbol);
                    if (mangled) {
                        EnvEntry *me = env_lookup(ctx->env, mangled);
                        if (me && me->kind == ENV_FUNC && me->func_ref)
                            entry = me;
                        free(mangled);
                    }
                }
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

                if (entry->scheme && declared_params > 0) {
                    Type **_atypes = malloc(sizeof(Type*) * declared_params);
                    for (int _i = 0; _i < declared_params; _i++) {
                        AST *_arg = ast->list.items[_i + 1];
                        /* Always prefer env lookup for symbols — inferred_type
                         * may be stale or wrong for function-valued symbols   */
                        if (_arg->type == AST_SYMBOL) {
                            EnvEntry *_ae = env_lookup(ctx->env, _arg->symbol);
                            if (_ae && (_ae->kind == ENV_FUNC ||
                                        _ae->kind == ENV_BUILTIN)) {
                                /* Prefer arrow scheme so signature comparison works */
                                if (_ae->scheme && _ae->scheme->type &&
                                    _ae->scheme->type->kind == TYPE_ARROW) {
                                    _atypes[_i] = _ae->scheme->type;
                                } else if (_ae->kind == ENV_FUNC &&
                                           _ae->param_count > 0 &&
                                           _ae->return_type) {
                                    Type *_arrow = type_clone(_ae->return_type);
                                    for (int _pi = _ae->param_count - 1; _pi >= 0; _pi--) {
                                        Type *_pt = _ae->params[_pi].type
                                                  ? type_clone(_ae->params[_pi].type)
                                                  : type_unknown();
                                        _arrow = type_arrow(_pt, _arrow);
                                    }
                                    _atypes[_i] = _arrow;
                                } else {
                                    _atypes[_i] = type_fn(NULL, 0, NULL);
                                }
                            } else if (_ae && _ae->type)
                                _atypes[_i] = _ae->type;
                            else
                                _atypes[_i] = type_unknown();
                        } else {
                            /* Resolve type for non-symbol expressions */
                            Type *_arg_t = _arg->inferred_type;
                            /* quote always produces a List */
                            if (!_arg_t || _arg_t->kind == TYPE_UNKNOWN) {
                                if (_arg->type == AST_LIST &&
                                    _arg->list.count >= 1 &&
                                    _arg->list.items[0]->type == AST_SYMBOL &&
                                    strcmp(_arg->list.items[0]->symbol, "quote") == 0)
                                    _arg_t = type_list(NULL);
                                else if (_arg->type == AST_ARRAY)
                                    _arg_t = type_arr(NULL, -1);
                                else if (_arg->type == AST_NUMBER)
                                    _arg_t = type_int();
                                else if (_arg->type == AST_STRING)
                                    _arg_t = type_string();
                            }
                            _atypes[_i] = _arg_t ? _arg_t : type_unknown();
                        }
                        fprintf(stderr, "DEBUG _atypes[%d] kind=%d for arg '%s'\n",
                                _i, _atypes[_i] ? (int)_atypes[_i]->kind : -1,
                                _arg->type == AST_SYMBOL ? _arg->symbol : "<expr>");
                    }
                    env_hm_check_call(ctx->env, head->symbol, _atypes, declared_params,
                                      parser_get_filename(), ast->list.items[1]->line,
                                      ast->list.items[1]->column);
                    free(_atypes);
                }

                /* ── Monomorphization: specialize if polymorphic ─────────── */
                /* Skip mono for functions whose parameters include Arr —
                 * array params are already concrete (ptr ABI), specializing
                 * them produces broken IR with mismatched array types.      */
                bool has_arr_param = false;
                for (int _pi = 0; _pi < entry->param_count; _pi++) {
                    if (entry->params[_pi].type &&
                        entry->params[_pi].type->kind == TYPE_ARR) {
                        has_arr_param = true;
                        break;
                    }
                }
                if (!has_rest && !has_arr_param &&
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
                                        if (LLVMGetTypeKind(et) == LLVMIntegerTypeKind && LLVMGetTypeKind(at) == LLVMIntegerTypeKind)
                                            call_args[i] = LLVMBuildIntCast2(ctx->builder, ar.value, et, 0, "coerce_int");
                                        else if (LLVMGetTypeKind(et) == LLVMFloatTypeKind && LLVMGetTypeKind(at) == LLVMDoubleTypeKind)
                                            call_args[i] = LLVMBuildFPTrunc(ctx->builder, ar.value, et, "coerce_fptrunc");
                                        else if (LLVMGetTypeKind(et) == LLVMDoubleTypeKind && LLVMGetTypeKind(at) == LLVMFloatTypeKind)
                                            call_args[i] = LLVMBuildFPExt(ctx->builder, ar.value, et, "coerce_fpext");
                                        else if (LLVMGetTypeKind(et) == LLVMDoubleTypeKind && type_is_integer(ar.type))
                                            call_args[i] = LLVMBuildSIToFP(ctx->builder, ar.value, et, "coerce_sitofp");
                                        else if (LLVMGetTypeKind(et) == LLVMIntegerTypeKind && LLVMGetTypeKind(at) == LLVMDoubleTypeKind)
                                            call_args[i] = LLVMBuildFPToSI(ctx->builder, ar.value, et, "coerce_fptosi");
                                        else if (LLVMGetTypeKind(et) == LLVMPointerTypeKind && LLVMGetTypeKind(at) == LLVMIntegerTypeKind)
                                            call_args[i] = LLVMBuildIntToPtr(ctx->builder, ar.value, et, "coerce_i2p");
                                        else if (LLVMGetTypeKind(et) == LLVMIntegerTypeKind && LLVMGetTypeKind(at) == LLVMPointerTypeKind)
                                            call_args[i] = LLVMBuildPtrToInt(ctx->builder, ar.value, et, "coerce_p2i");
                                        else
                                            call_args[i] = LLVMBuildBitCast(ctx->builder, ar.value, et, "coerce_bc");
                                    }
                                }

                                for (int i = 0; i < sargs; i++)
                                    fprintf(stderr, "  arg[%d] type_kind=%d llvm_type=%d\n", // DEBUG
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

                    /* Self-recursive closure call — call directly with current
                     * env pointer instead of going through rt_closure_calln.
                     * This avoids stack overflow from closure dispatch overhead
                     * and correctly passes the captured environment.          */
                    LLVMValueRef cur_fn = LLVMGetBasicBlockParent(
                                             LLVMGetInsertBlock(ctx->builder));
                    if (ctx->current_function_name &&
                        strcmp(head->symbol, ctx->current_function_name) == 0 &&
                        cur_fn == entry->func_ref) {
                        /* Build args array */
                        LLVMTypeRef arr_t    = LLVMArrayType(ptr_t, declared_params ? declared_params : 1);
                        LLVMValueRef arr_ptr = LLVMBuildAlloca(ctx->builder, arr_t, "rec_args");
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
                        LLVMValueRef args_ptr = declared_params > 0
                            ? LLVMBuildBitCast(ctx->builder, arr_ptr, ptr_t, "args_ptr")
                            : LLVMConstPointerNull(ptr_t);
                        /* Get the env parameter (first param of current closure fn) */
                        LLVMValueRef env_param = LLVMGetParam(cur_fn, 0);
                        LLVMTypeRef  rec_params[] = {ptr_t, i32, ptr_t};
                        LLVMTypeRef  rec_ft = LLVMFunctionType(ptr_t, rec_params, 3, 0);
                        LLVMValueRef rec_args[] = {
                            env_param,
                            LLVMConstInt(i32, declared_params, 0),
                            args_ptr
                        };
                        result.value = LLVMBuildCall2(ctx->builder, rec_ft,
                                                      entry->func_ref, rec_args, 3, "rec_call");
                        result.type  = type_unknown();
                        /* Unbox the ptr result to the declared return type if needed */
                        /* Also check env_params return type from the outer function */
                        Type *rec_ret = entry->return_type;
                        if (rec_ret) {
                            LLVMTypeRef expected = type_to_llvm(ctx, entry->return_type);
                            LLVMTypeRef got      = LLVMTypeOf(result.value);
                            if (got == ptr_t && expected != ptr_t) {
                                if (type_is_float(entry->return_type)) {
                                    LLVMTypeRef dbl = LLVMDoubleTypeInContext(ctx->context);
                                    LLVMTypeRef uft = LLVMFunctionType(dbl, &ptr_t, 1, 0);
                                    result.value = LLVMBuildCall2(ctx->builder, uft,
                                                      get_rt_unbox_float(ctx), &result.value, 1, "unbox_rec");
                                    result.type  = type_float();
                                } else if (type_is_integer(entry->return_type)) {
                                    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                                    LLVMTypeRef uft = LLVMFunctionType(i64, &ptr_t, 1, 0);
                                    result.value = LLVMBuildCall2(ctx->builder, uft,
                                                      get_rt_unbox_int(ctx), &result.value, 1, "unbox_rec");
                                    result.type  = type_int();
                                }
                            }
                        }
                        return result;

                    }

                    /* If the symbol has a materialized entry, it means the closure was
                     * already created with its captured env baked in
                     * (by define's closure_val path). Load and use it directly
                     * instead of re-wrapping func_ref with a null env.        */
                    {
                        EnvEntry *var_e = env_lookup(ctx->env, head->symbol);
                        if (var_e && (var_e->kind == ENV_VAR || var_e->kind == ENV_FUNC) && var_e->value &&
                            var_e->func_ref && var_e->is_closure_abi) {
                            /* Load the materialized closure RuntimeValue* */
                            LLVMValueRef clo = LLVMBuildLoad2(ctx->builder, ptr_t,
                                                              var_e->value, "clo_load");

                            /* Build args array on heap */
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
                            LLVMValueRef arr_ptr = LLVMBuildCall2(ctx->builder,
                                LLVMFunctionType(ptr_t, &i64_t, 1, 0), malloc_fn, &arr_size, 1, "clo_args_heap");
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
                            LLVMValueRef args_ptr = declared_params > 0
                                ? LLVMBuildBitCast(ctx->builder, arr_ptr, ptr_t, "args_ptr")
                                : LLVMConstPointerNull(ptr_t);

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
                            LLVMValueRef free_fn = LLVMGetNamedFunction(ctx->module, "free");
                            if (!free_fn) {
                                LLVMTypeRef ft = LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), &ptr_t, 1, 0);
                                free_fn = LLVMAddFunction(ctx->module, "free", ft);
                                LLVMSetLinkage(free_fn, LLVMExternalLinkage);
                            }
                            LLVMBuildCall2(ctx->builder,
                                LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), &ptr_t, 1, 0),
                                free_fn, &arr_ptr, 1, "");
                            result.type = type_unknown();
                            return result;
                        }
                    }

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
                bool auto_lift = false;
                LLVMValueRef auto_lift_nil_check = NULL;
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
                                                  "%s:%d:%d: error: variadic argument %zu to ‘%s’ "
                                                  "has type %s but rest parameter ‘[%s :: %s]’ requires %s",
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

                    /* FFI call with array arg — always unwrap fat ptr to raw data pointer */
                    if (entry->is_ffi && arg_result.type &&
                        arg_result.type->kind == TYPE_ARR) {
                        LLVMTypeRef fat_t  = get_arr_fat_type(ctx);
                        LLVMTypeRef fat_pt = LLVMPointerType(fat_t, 0);
                        LLVMTypeRef ptr_t  = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                        LLVMTypeRef cv_t   = LLVMTypeOf(arg_result.value);
                        if (cv_t == fat_pt) {
                            arg_result.value = arr_fat_data(ctx, arg_result.value,
                                                            arg_result.type->arr_element_type);
                        } else if (cv_t == ptr_t) {
                            LLVMValueRef fat_cast = LLVMBuildBitCast(ctx->builder,
                                                        arg_result.value, fat_pt, "fat_cast");
                            arg_result.value = arr_fat_data(ctx, fat_cast,
                                                            arg_result.type->arr_element_type);
                        }
                        arg_result.type = type_unknown();
                    }

                    /* Layout-typed globals hold a pointer-to-struct, not the struct
                     * pointer itself. Load through the global to get the actual
                     * heap pointer before passing to FFI functions.             */
                    if (arg_result.type && arg_result.type->kind == TYPE_LAYOUT &&
                        ast->list.items[i + 1]->type == AST_SYMBOL) {
                        EnvEntry *ae = env_lookup(ctx->env, ast->list.items[i + 1]->symbol);
                        if (ae && ae->kind == ENV_VAR && ae->value &&
                            LLVMGetValueKind(ae->value) == LLVMGlobalVariableValueKind) {
                            LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                            arg_result.value = LLVMBuildLoad2(ctx->builder, ptr_t,
                                                              ae->value, "lay_load");
                        }
                    }

                    expected_type = entry->params[i].type;

                    // Compile-time refinement check via JIT — fully general,
                    // works for any predicate the user writes.
                    {
                        AST *arg_ast = ast->list.items[i + 1];
                        /* Unwrap unary minus: (- 1) -> treat as negative number */
                        if (arg_ast->type == AST_LIST &&
                            arg_ast->list.count == 2 &&
                            arg_ast->list.items[0]->type == AST_SYMBOL &&
                            strcmp(arg_ast->list.items[0]->symbol, "-") == 0 &&
                            arg_ast->list.items[1]->type == AST_NUMBER) {
                            AST *neg = ast_clone(arg_ast->list.items[1]);
                            neg->number      = -neg->number;
                            neg->raw_int     = -neg->raw_int;
                            neg->has_raw_int = arg_ast->list.items[1]->has_raw_int;
                            arg_ast = neg;
                        }
                        /* Resolve symbol to its compile-time literal if available */
                        if (arg_ast->type == AST_SYMBOL) {
                            EnvEntry *ve = env_lookup(ctx->env, arg_ast->symbol);
                            if (ve && ve->kind == ENV_VAR && ve->source_ast &&
                                (ve->source_ast->type == AST_STRING ||
                                 ve->source_ast->type == AST_NUMBER)) {
                                arg_ast = ve->source_ast;
                            }
                        }
                        if ((arg_ast->type == AST_NUMBER || arg_ast->type == AST_STRING) &&
                            entry->source_ast &&
                            entry->source_ast->type == AST_LAMBDA &&
                            i < entry->source_ast->lambda.param_count) {
                            const char *ann = entry->source_ast->lambda.params[i].type_name;
                            if (ann) {
                                int ok = jit_eval_refinement(ctx, ann, arg_ast);
                                /* fprintf(stderr, "DEBUG refinement check: type=‘%s’ ok=%d\n", ann, ok); */
                                if (ok == 0) {
                                    if (arg_ast->type == AST_STRING) {
                                        CODEGEN_ERROR(ctx,
                                            "%s:%d:%d: error: \"%s\" does not satisfy "
                                            "refinement type %s",
                                            parser_get_filename(),
                                            arg_ast->line, arg_ast->column,
                                            arg_ast->string, ann);
                                    } else {
                                        CODEGEN_ERROR(ctx,
                                            "%s:%d:%d: error: %g does not satisfy "
                                            "refinement type %s",
                                            parser_get_filename(),
                                            arg_ast->line, arg_ast->column,
                                            arg_ast->number, ann);
                                    }
                                }
                            }
                        }
                    }

                    Type *actual_type   = arg_result.type;
                    LLVMValueRef converted_arg = arg_result.value;

                    /* ── Optional Safety Trap & Auto-Lift ────────────────────── */
                    if (expected_type && actual_type && expected_type->kind != TYPE_UNKNOWN && actual_type->kind != expected_type->kind) {
                        if ((actual_type->kind == TYPE_OPTIONAL || actual_type->kind == TYPE_NIL) &&
                            expected_type->kind != TYPE_OPTIONAL && expected_type->kind != TYPE_NIL && expected_type->kind != TYPE_PTR) {

                            if (ctx->in_coalesce_depth > 0) {
                                auto_lift = true;
                                LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                                LLVMTypeRef i32_t = LLVMInt32TypeInContext(ctx->context);
                                LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);

                                LLVMValueRef val_to_check = converted_arg;
                                if (LLVMTypeOf(val_to_check) != ptr_t) {
                                    if (LLVMGetTypeKind(LLVMTypeOf(val_to_check)) == LLVMIntegerTypeKind) {
                                        val_to_check = LLVMBuildIntToPtr(ctx->builder, val_to_check, ptr_t, "nil_cast");
                                    } else {
                                        val_to_check = LLVMBuildBitCast(ctx->builder, val_to_check, ptr_t, "nil_cast");
                                    }
                                }

                                LLVMValueRef is_nil_fn = get_rt_value_is_nil(ctx);
                                LLVMValueRef check = LLVMBuildCall2(ctx->builder, LLVMGlobalGetValueType(is_nil_fn), is_nil_fn, &val_to_check, 1, "al_check");
                                LLVMValueRef check_bool = LLVMBuildICmp(ctx->builder, LLVMIntNE, check, LLVMConstInt(i32_t, 0, 0), "al_bool");

                                if (auto_lift_nil_check) {
                                    auto_lift_nil_check = LLVMBuildOr(ctx->builder, auto_lift_nil_check, check_bool, "al_or");
                                } else {
                                    auto_lift_nil_check = check_bool;
                                }

                                /* Prevent crash in unbox: if nil, substitute a dummy valid boxed zero */
                                LLVMValueRef vi_fn = get_rt_value_int(ctx);
                                LLVMTypeRef vi_ft = LLVMFunctionType(ptr_t, &i64_t, 1, 0);
                                LLVMValueRef zero_val = LLVMConstInt(i64_t, 0, 0);
                                LLVMValueRef dummy_zero = LLVMBuildCall2(ctx->builder, vi_ft, vi_fn, &zero_val, 1, "dummy_zero");
                                converted_arg = LLVMBuildSelect(ctx->builder, check_bool, dummy_zero, converted_arg, "safe_arg");
                            } else {
                                char exp_str[64];
                                char act_str[64];
                                snprintf(exp_str, sizeof(exp_str), "%s", type_to_string(expected_type));
                                snprintf(act_str, sizeof(act_str), "%s", type_to_string(actual_type));

                                CODEGEN_ERROR(ctx,
                                              "%s:%d:%d: error: Type mismatch in function call to '%s'\n"
                                              "    • Argument %d evaluates to an optional '%s' but the function expects strict '%s'.\n"
                                              "  - Hint: You cannot directly pass an Optional type to a function expecting a raw value.\n"
                                              "  - Fix: Explicitly handle the possible 'nil' using the '?\?' operator (e.g. call ?? default).",
                                              parser_get_filename(), ast->list.items[i+1]->line, ast->list.items[i+1]->column,
                                              head->symbol, i + 1, act_str, exp_str);
                            }
                        }
                    }

                    fprintf(stderr, "LAST DEBUG arg[%d] to '%s': actual_type_kind=%d arr_is_fat=%d expected_type_kind=%d llvm_type_kind=%d\n",
                            i, head->symbol,
                            actual_type ? actual_type->kind : -1,
                            actual_type ? actual_type->arr_is_fat : -1,
                            expected_type ? expected_type->kind : -1,
                            (int)LLVMGetTypeKind(LLVMTypeOf(converted_arg)));

                    /* Fat array passed to a raw pointer param — unwrap to data pointer.
                     * actual_type may be TYPE_UNKNOWN for top-level fat array globals
                     * (their i8* global loses array type on load), so also check the
                     * env entry's original type via the argument symbol.             */
                    if (ast->list.items[i + 1]->type == AST_SYMBOL) {
                        EnvEntry *ae = env_lookup(ctx->env, ast->list.items[i + 1]->symbol);
                        fprintf(stderr, "SYMBOL ARG '%s' to '%s': ae=%p kind=%d type_kind=%d arr_is_fat=%d\n",
                                ast->list.items[i + 1]->symbol, head->symbol,
                                (void*)ae,
                                ae ? ae->kind : -1,
                                (ae && ae->type) ? ae->type->kind : -1,
                                (ae && ae->type) ? ae->type->arr_is_fat : -1);
                        if (ae && ae->kind == ENV_VAR && ae->type &&
                            ae->type->kind == TYPE_ARR && ae->type->arr_is_fat) {
                            LLVMTypeRef fat_t  = get_arr_fat_type(ctx);
                            LLVMTypeRef fat_pt = LLVMPointerType(fat_t, 0);
                            LLVMTypeRef ptr_t2 = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                            LLVMValueRef fat_cast = LLVMBuildBitCast(ctx->builder,
                                                        converted_arg, fat_pt, "fat_cast");
                            converted_arg = arr_fat_data(ctx, fat_cast,
                                                         ae->type->arr_element_type);
                        }
                    }
                    /* Fat array passed to a raw pointer param — unwrap to data pointer */

                    if (actual_type && actual_type->kind == TYPE_ARR &&
                        expected_type && (expected_type->kind == TYPE_PTR  ||
                                          expected_type->kind == TYPE_U8   ||
                                          expected_type->kind == TYPE_UNKNOWN)) {
                        /* converted_arg may be an i8* to a fat struct (global)
                         * or an arr.fat* (local) — check the LLVM type to decide */
                        LLVMTypeRef fat_t  = get_arr_fat_type(ctx);
                        LLVMTypeRef fat_pt = LLVMPointerType(fat_t, 0);
                        LLVMTypeRef ptr_t  = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                        LLVMTypeRef cv_t   = LLVMTypeOf(converted_arg);
                        if (cv_t == fat_pt) {
                            /* Local fat ptr — GEP directly */
                            converted_arg = arr_fat_data(ctx, converted_arg,
                                                         actual_type->arr_element_type);
                        } else if (cv_t == ptr_t) {
                            /* Global i8* — bitcast to fat*, then GEP */
                            LLVMValueRef fat_cast = LLVMBuildBitCast(ctx->builder,
                                                        converted_arg, fat_pt, "fat_cast");
                            converted_arg = arr_fat_data(ctx, fat_cast,
                                                         actual_type->arr_element_type);
                        }
                    }

                    /* Array argument passed to Arr parameter — pass the fat

                     * pointer directly. Load it from the global/alloca.     */
                    if (expected_type && expected_type->kind == TYPE_ARR &&
                        actual_type   && actual_type->kind   == TYPE_ARR &&
                        ast->list.items[i + 1]->type == AST_SYMBOL) {
                        EnvEntry *ae = env_lookup(ctx->env,
                                           ast->list.items[i + 1]->symbol);
                        if (ae && ae->kind == ENV_VAR && ae->value) {
                            LLVMTypeRef ptr_t = LLVMPointerType(
                                LLVMInt8TypeInContext(ctx->context), 0);
                            LLVMValueRef ae_ptr = ae->value;
                            bool ae_global = LLVMGetValueKind(ae_ptr) ==
                                             LLVMGlobalVariableValueKind;
                            if (ae_global) {
                                const char *gname = (ae->llvm_name && ae->llvm_name[0])
                                    ? ae->llvm_name : ae->name;
                                LLVMValueRef gv = LLVMGetNamedGlobal(ctx->module, gname);
                                if (!gv) {
                                    gv = LLVMAddGlobal(ctx->module, ptr_t, gname);
                                    LLVMSetLinkage(gv, LLVMExternalLinkage);
                                }
                                /* Load the fat pointer from the global */
                                converted_arg = LLVMBuildLoad2(ctx->builder,
                                                    ptr_t, gv, "fat_load");
                            } else {
                                /* Local alloca holding fat ptr — load it */
                                if (ae->type && ae->type->arr_is_fat) {
                                    LLVMTypeRef fat_t = get_arr_fat_type(ctx);
                                    LLVMTypeRef fat_pt = LLVMPointerType(fat_t, 0);
                                    converted_arg = LLVMBuildLoad2(ctx->builder,
                                                        fat_pt, ae_ptr, "fat_load");
                                } else {
                                    converted_arg = LLVMBuildBitCast(ctx->builder,
                                                        ae_ptr, ptr_t, "arr_ptr");
                                }
                            }
                        }
                    }

                    Type *cb_type = expected_type;
                    if (cb_type && cb_type->kind == TYPE_PTR && cb_type->element_type &&
                        (cb_type->element_type->kind == TYPE_FN ||
                         cb_type->element_type->kind == TYPE_ARROW)) {
                        cb_type = cb_type->element_type;
                    }

                    AST *arg_ast = ast->list.items[i + 1];
                    bool is_ffi_lambda = (entry && entry->is_ffi && arg_ast->type == AST_LAMBDA);

                    if (is_ffi_lambda || (cb_type && (cb_type->kind == TYPE_FN ||
                                                      cb_type->kind == TYPE_ARROW))) {
                        if (entry && entry->is_ffi) {
                            /* Generate a static C-ABI trampoline backed by a global closure slot. */
                            LLVMTypeRef  ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                            LLVMTypeRef  i32   = LLVMInt32TypeInContext(ctx->context);
                            LLVMTypeRef  i64   = LLVMInt64TypeInContext(ctx->context);

                            int cb_arity = 0;
                            LLVMTypeRef *cb_params = NULL;
                            LLVMTypeRef cb_ret = LLVMVoidTypeInContext(ctx->context);

                            /* Prefer the AST lambda signature if available to ensure perfect sync with the inner wrapper */
                            if (is_ffi_lambda) {
                                cb_arity = arg_ast->lambda.param_count;
                                cb_params = malloc(sizeof(LLVMTypeRef) * (cb_arity ? cb_arity : 1));
                                for (int _ci = 0; _ci < cb_arity; _ci++) {
                                    const char *tname = arg_ast->lambda.params[_ci].type_name;
                                    Type *pt = tname ? type_from_name(tname) : NULL;
                                    cb_params[_ci] = pt ? type_to_llvm(ctx, pt) : ptr_t;
                                }
                                const char *rtname = arg_ast->lambda.return_type;
                                Type *rt = rtname ? type_from_name(rtname) : NULL;
                                cb_ret = rt ? type_to_llvm(ctx, rt) : LLVMVoidTypeInContext(ctx->context);
                            } else {
                                Type *resolve_type = cb_type;
                                if ((!resolve_type || resolve_type->param_count == 0) && actual_type && actual_type->kind == TYPE_FN) {
                                    resolve_type = actual_type;
                                }
                                cb_arity = resolve_type->param_count;
                                cb_params = malloc(sizeof(LLVMTypeRef) * (cb_arity ? cb_arity : 1));
                                for (int _ci = 0; _ci < cb_arity; _ci++) {
                                    cb_params[_ci] = resolve_type->params[_ci].type ? type_to_llvm(ctx, resolve_type->params[_ci].type) : ptr_t;
                                }
                                cb_ret = resolve_type->return_type ? type_to_llvm(ctx, resolve_type->return_type) : LLVMVoidTypeInContext(ctx->context);
                            }

                            LLVMTypeRef cb_ft = LLVMFunctionType(cb_ret, cb_params, cb_arity, 0);
                            free(cb_params);

                            static int cb_slot_count = 0;
                            char slot_name[64];
                            snprintf(slot_name, sizeof(slot_name), "__cb_closure_slot_%d", cb_slot_count);

                            LLVMValueRef slot_gv = LLVMGetNamedGlobal(ctx->module, slot_name);
                            if (!slot_gv) {
                                slot_gv = LLVMAddGlobal(ctx->module, ptr_t, slot_name);
                                LLVMSetInitializer(slot_gv, LLVMConstPointerNull(ptr_t));
                                LLVMSetLinkage(slot_gv, LLVMExternalLinkage);
                            }

                            LLVMBuildStore(ctx->builder, converted_arg, slot_gv);

                            char tramp_name[64];
                            snprintf(tramp_name, sizeof(tramp_name), "__cb_tramp_%d", cb_slot_count++);

                            LLVMValueRef tramp_fn = LLVMGetNamedFunction(ctx->module, tramp_name);
                            if (!tramp_fn) {
                                tramp_fn = LLVMAddFunction(ctx->module, tramp_name, cb_ft);
                                LLVMSetLinkage(tramp_fn, LLVMExternalLinkage);

                                LLVMBasicBlockRef saved_bb    = LLVMGetInsertBlock(ctx->builder);
                                LLVMBasicBlockRef tramp_entry = LLVMAppendBasicBlockInContext(
                                                                   ctx->context, tramp_fn, "entry");
                                LLVMPositionBuilderAtEnd(ctx->builder, tramp_entry);

                                LLVMValueRef clo = LLVMBuildLoad2(ctx->builder, ptr_t, slot_gv, "clo");

                                LLVMTypeRef arr_t   = LLVMArrayType(ptr_t, cb_arity ? cb_arity : 1);
                                LLVMValueRef arr_ptr = LLVMBuildAlloca(ctx->builder, arr_t, "cb_args");
                                for (int _ci = 0; _ci < cb_arity; _ci++) {
                                    LLVMValueRef raw = LLVMGetParam(tramp_fn, _ci);
                                    LLVMValueRef boxed;
                                    LLVMTypeRef  raw_t = LLVMTypeOf(raw);
                                    if (LLVMGetTypeKind(raw_t) == LLVMIntegerTypeKind) {
                                        LLVMValueRef ext = LLVMBuildZExt(ctx->builder, raw, i64, "ext");
                                        LLVMTypeRef  bft = LLVMFunctionType(ptr_t, &i64, 1, 0);
                                        LLVMValueRef vi  = LLVMGetNamedFunction(ctx->module, "rt_value_int");
                                        if (!vi) {
                                            vi = LLVMAddFunction(ctx->module, "rt_value_int", bft);
                                            LLVMSetLinkage(vi, LLVMExternalLinkage);
                                        }
                                        boxed = LLVMBuildCall2(ctx->builder, bft, vi, &ext, 1, "boxed");
                                    } else {
                                        boxed = LLVMBuildBitCast(ctx->builder, raw, ptr_t, "boxed");
                                    }
                                    LLVMValueRef zero   = LLVMConstInt(i32, 0, 0);
                                    LLVMValueRef idx    = LLVMConstInt(i32, _ci, 0);
                                    LLVMValueRef idxs[] = {zero, idx};
                                    LLVMValueRef slot2  = LLVMBuildGEP2(ctx->builder, arr_t,
                                                                         arr_ptr, idxs, 2, "slot");
                                    LLVMBuildStore(ctx->builder, boxed, slot2);
                                }

                                LLVMValueRef args_ptr = cb_arity > 0
                                    ? LLVMBuildBitCast(ctx->builder, arr_ptr, ptr_t, "args_ptr")
                                    : LLVMConstPointerNull(ptr_t);

                                LLVMValueRef calln_fn = get_rt_closure_calln(ctx);
                                LLVMTypeRef  calln_p[] = {ptr_t, i32, ptr_t};
                                LLVMTypeRef  calln_ft  = LLVMFunctionType(ptr_t, calln_p, 3, 0);
                                LLVMValueRef calln_a[] = {
                                    clo,
                                    LLVMConstInt(i32, cb_arity, 0),
                                    args_ptr
                                };
                                LLVMValueRef ret = LLVMBuildCall2(ctx->builder, calln_ft,
                                                                   calln_fn, calln_a, 3, "cb_ret");

                                if (LLVMGetTypeKind(cb_ret) == LLVMVoidTypeKind) {
                                    LLVMBuildRetVoid(ctx->builder);
                                } else if (LLVMGetTypeKind(cb_ret) == LLVMIntegerTypeKind) {
                                    LLVMTypeRef  uft     = LLVMFunctionType(i64, &ptr_t, 1, 0);
                                    LLVMValueRef vi_fn   = LLVMGetNamedFunction(ctx->module, "rt_unbox_int");
                                    if (!vi_fn) {
                                        vi_fn = LLVMAddFunction(ctx->module, "rt_unbox_int", uft);
                                        LLVMSetLinkage(vi_fn, LLVMExternalLinkage);
                                    }
                                    LLVMValueRef unboxed = LLVMBuildCall2(ctx->builder, uft, vi_fn, &ret, 1, "r");
                                    LLVMValueRef cast    = LLVMBuildIntCast2(ctx->builder, unboxed, cb_ret, 0, "r");
                                    LLVMBuildRet(ctx->builder, cast);
                                } else if (LLVMGetTypeKind(cb_ret) == LLVMFloatTypeKind || LLVMGetTypeKind(cb_ret) == LLVMDoubleTypeKind) {
                                    LLVMTypeRef dbl_t = LLVMDoubleTypeInContext(ctx->context);
                                    LLVMTypeRef uft = LLVMFunctionType(dbl_t, &ptr_t, 1, 0);
                                    LLVMValueRef unboxed = LLVMBuildCall2(ctx->builder, uft, get_rt_unbox_float(ctx), &ret, 1, "r");
                                    if (LLVMGetTypeKind(cb_ret) == LLVMFloatTypeKind) {
                                        unboxed = LLVMBuildFPTrunc(ctx->builder, unboxed, cb_ret, "r_trunc");
                                    }
                                    LLVMBuildRet(ctx->builder, unboxed);
                                } else {
                                    LLVMBuildRet(ctx->builder, LLVMBuildBitCast(ctx->builder, ret, cb_ret, "r"));
                                }

                                if (saved_bb) LLVMPositionBuilderAtEnd(ctx->builder, saved_bb);
                            }

                            converted_arg = LLVMBuildBitCast(ctx->builder, tramp_fn, ptr_t, "tramp_ptr");
                            args[i] = converted_arg;
                            continue;
                        } else {
                            /* NOT FFI: Normal internal closure handling */
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
                    }

                    // TODO HERE
                    if (expected_type && expected_type->kind == TYPE_COLL) {
                        if (arg_result.type && arg_result.type->kind == TYPE_ARR) {
                            converted_arg = codegen_box(ctx, arg_result.value, arg_result.type);
                        } else if (arg_result.type && arg_result.type->kind == TYPE_SET) {

                            /* Convert set to RuntimeList* before passing as Coll */
                            LLVMTypeRef  ptr_t  = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                            LLVMValueRef ub_fn  = get_rt_unbox_set(ctx);
                            LLVMTypeRef  ub_ft  = LLVMFunctionType(ptr_t, &ptr_t, 1, 0);
                            LLVMValueRef ub_a[] = {arg_result.value};
                            LLVMValueRef raw_set = LLVMBuildCall2(ctx->builder, ub_ft,
                                                                   ub_fn, ub_a, 1, "rawset");
                            LLVMValueRef seq_fn  = get_rt_set_seq(ctx);
                            LLVMTypeRef  seq_ft  = LLVMFunctionType(ptr_t, &ptr_t, 1, 0);
                            LLVMValueRef seq_a[] = {raw_set};
                            LLVMValueRef seq_list = LLVMBuildCall2(ctx->builder, seq_ft,
                                                           seq_fn, seq_a, 1, "set_as_list");
                            LLVMValueRef vl_fn = get_rt_value_list(ctx);
                            LLVMTypeRef  vl_ft = LLVMFunctionType(ptr_t, &ptr_t, 1, 0);
                            converted_arg = LLVMBuildCall2(ctx->builder, vl_ft, vl_fn, &seq_list, 1, "boxed_seq");
                        } else if (arg_result.type && arg_result.type->kind == TYPE_COLL) {
                            converted_arg = arg_result.value;
                        } else {
                            converted_arg = codegen_box(ctx, arg_result.value, arg_result.type);
                        }
                    } else if (expected_type && expected_type->kind == TYPE_UNKNOWN) {

                        if (actual_type->kind == TYPE_UNKNOWN) {
                            converted_arg = arg_result.value;
                        } else {
                            converted_arg = codegen_box(ctx, arg_result.value, arg_result.type);
                        }
                    } else if (expected_type && expected_type->kind == TYPE_LAYOUT &&
                               actual_type   && actual_type->kind   == TYPE_LAYOUT &&
                               LLVMGetTypeKind(LLVMTypeOf(converted_arg)) != LLVMPointerTypeKind) {
                        /* ADT types are heap-allocated and always passed as pointers.
                         * Only apply struct-by-value C ABI packing for FFI layouts. */
                        EnvEntry *lay_e = env_lookup(ctx->env, expected_type->layout_name);
                        bool is_adt = lay_e && (lay_e->kind == ENV_ADT_CTOR ||
                                      env_lookup_adt_ctor(ctx->env, expected_type->layout_name));
                        /* Check if any constructor for this type exists as ADT_CTOR */
                        if (!is_adt) {
                            /* Walk env to find if layout_name matches any ADT ctor's type */
                            for (size_t _bi = 0; _bi < ctx->env->size && !is_adt; _bi++) {
                                for (EnvEntry *_e = ctx->env->buckets[_bi]; _e && !is_adt; _e = _e->next) {
                                    if (_e->kind == ENV_ADT_CTOR && _e->type &&
                                        _e->type->kind == TYPE_LAYOUT &&
                                        strcmp(_e->type->layout_name, expected_type->layout_name) == 0)
                                        is_adt = true;
                                }
                            }
                        }
                        if (is_adt) {
                            /* ADT — pass heap pointer directly, no unpacking */
                            converted_arg = arg_result.value;
                        } else {
                            /* FFI layout — struct-by-value C ABI packing */
                            Type *full = env_lookup_layout(ctx->env,
                                             expected_type->layout_name);
                            int sz = full ? full->layout_total_size : 0;
                            if (sz > 0 && sz <= 4) {
                                LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
                                converted_arg = LLVMBuildLoad2(ctx->builder, i32,
                                                    arg_result.value, "struct_i32");
                            } else if (sz > 4 && sz <= 8) {
                                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                                converted_arg = LLVMBuildLoad2(ctx->builder, i64,
                                                    arg_result.value, "struct_i64");
                            } else {
                                converted_arg = arg_result.value;
                            }
                        }
                    } else if (LLVMTypeOf(converted_arg) ==
                               LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0) &&
                               expected_type && type_is_integer(expected_type) &&
                               !(actual_type && actual_type->kind == TYPE_PTR)) {
                        LLVMTypeRef i64   = LLVMInt64TypeInContext(ctx->context);
                        LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                        LLVMTypeRef uft   = LLVMFunctionType(i64, &ptr_t, 1, 0);
                        converted_arg = LLVMBuildCall2(ctx->builder, uft,
                                            get_rt_unbox_int(ctx), &converted_arg, 1, "unbox_arg");
                    } else if (LLVMTypeOf(converted_arg) ==
                               LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0) &&
                               expected_type && type_is_float(expected_type)) {
                        LLVMTypeRef dbl   = LLVMDoubleTypeInContext(ctx->context);
                        LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                        LLVMTypeRef uft   = LLVMFunctionType(dbl, &ptr_t, 1, 0);
                        converted_arg = LLVMBuildCall2(ctx->builder, uft,
                                            get_rt_unbox_float(ctx), &converted_arg, 1, "unbox_arg");
                    } else if (expected_type && expected_type->kind == TYPE_F32 &&
                               LLVMGetTypeKind(LLVMTypeOf(converted_arg)) == LLVMDoubleTypeKind) {
                        converted_arg = LLVMBuildFPTrunc(ctx->builder, converted_arg,
                                                         LLVMFloatTypeInContext(ctx->context),
                                                         "fp_trunc");
                    } else if (expected_type && actual_type &&
                               expected_type->kind == TYPE_F32 &&
                               actual_type->kind == TYPE_FLOAT) {
                        converted_arg = LLVMBuildFPTrunc(ctx->builder, converted_arg,
                                                         LLVMFloatTypeInContext(ctx->context),
                                                         "fp_trunc");
                    } else if (expected_type && actual_type && expected_type->kind != actual_type->kind) {
                        LLVMTypeRef expected_llvm = type_to_llvm(ctx, expected_type);
                        if (type_is_integer(expected_type) && type_is_integer(actual_type) &&
                            LLVMTypeOf(arg_result.value) != expected_llvm) {
                            converted_arg = LLVMBuildIntCast2(ctx->builder, arg_result.value,
                                                              expected_llvm, 1, "int_cast");
                        } else if (type_is_integer(expected_type) && type_is_float(actual_type)) {
                            converted_arg = LLVMBuildFPToSI(ctx->builder, arg_result.value,
                                                            expected_llvm, "arg_conv");
                        } else if (type_is_float(expected_type) && type_is_float(actual_type) &&
                                   expected_type->kind == TYPE_F32 && actual_type->kind == TYPE_FLOAT) {
                            converted_arg = LLVMBuildFPTrunc(ctx->builder, arg_result.value,
                                                             LLVMFloatTypeInContext(ctx->context),
                                                             "fp_trunc");
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
                            /* Coerce to exact LLVM type the function expects (e.g. i64 -> i32 for C int) */
                            LLVMTypeRef expected_llvm = type_to_llvm(ctx, expected_type);
                            LLVMTypeRef actual_llvm   = LLVMTypeOf(arg_result.value);
                            if (actual_llvm != expected_llvm)
                                converted_arg = LLVMBuildIntCast2(ctx->builder, arg_result.value,
                                                                  expected_llvm, 1, "int_cast");
                            else
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

                /* Coerce all args to exactly the LLVM types the function declares. */
                {
                    LLVMTypeRef fn_t = LLVMGlobalGetValueType(entry->func_ref);
                    unsigned nparams = LLVMCountParamTypes(fn_t);
                    if (nparams > 0) {
                        LLVMTypeRef *param_ts = malloc(sizeof(LLVMTypeRef) * nparams);
                        LLVMGetParamTypes(fn_t, param_ts);
                        for (int _i = 0; _i < (int)nparams && _i < total_args; _i++) {
                            args[_i] = emit_type_cast(ctx, args[_i], param_ts[_i]);
                        }
                        free(param_ts);
                    }
                }
                LLVMTypeRef call_ft = LLVMGlobalGetValueType(entry->func_ref);

                /* For FFI functions, look up the declaration in the current
                 * module by name — func_ref may point to a different module */
                /* If the coercion pass patched the function type (ptr args to
                 * narrow-int params), rebuild call_ft from the patched args. */
                {
                    LLVMTypeRef fn_t2 = LLVMGlobalGetValueType(entry->func_ref);
                    unsigned np2 = LLVMCountParamTypes(fn_t2);
                    if (np2 > 0) {
                        LLVMTypeRef *pt2 = malloc(sizeof(LLVMTypeRef) * np2);
                        LLVMGetParamTypes(fn_t2, pt2);
                        bool patched2 = false;
                        for (int _i = 0; _i < (int)np2 && _i < total_args; _i++) {
                            if (LLVMGetTypeKind(LLVMTypeOf(args[_i])) == LLVMPointerTypeKind &&
                                LLVMGetTypeKind(pt2[_i]) == LLVMIntegerTypeKind &&
                                LLVMGetIntTypeWidth(pt2[_i]) < 64) {
                                pt2[_i] = LLVMTypeOf(args[_i]);
                                patched2 = true;
                            } else if (LLVMGetTypeKind(LLVMTypeOf(args[_i])) == LLVMDoubleTypeKind &&
                                       LLVMGetTypeKind(pt2[_i]) == LLVMFloatTypeKind) {
                                args[_i] = LLVMBuildFPTrunc(ctx->builder, args[_i],
                                                             LLVMFloatTypeInContext(ctx->context),
                                                             "fp_trunc");
                            }
                        }
                        if (patched2)
                            call_ft = LLVMFunctionType(LLVMGetReturnType(fn_t2),
                                                       pt2, np2,
                                                       LLVMIsFunctionVarArg(fn_t2));
                        free(pt2);
                    }
                }

                LLVMValueRef call_target = entry->func_ref;
                if (entry->is_ffi) {
                    const char *fname = entry->llvm_name && entry->llvm_name[0]
                                      ? entry->llvm_name : entry->name;
                    LLVMValueRef in_mod = LLVMGetNamedFunction(ctx->module, fname);
                    if (!in_mod) {
                        in_mod = LLVMAddFunction(ctx->module, fname, call_ft);
                        LLVMSetLinkage(in_mod, LLVMExternalLinkage);
                    }
                    call_target = in_mod;
                }

                /* Check if function returns void */
                LLVMTypeRef ret_llvm = LLVMGetReturnType(call_ft);
                bool is_void_ret = (LLVMGetTypeKind(ret_llvm) == LLVMVoidTypeKind);

                LLVMBasicBlockRef al_nil_bb = NULL;
                LLVMBasicBlockRef al_call_bb = NULL;
                LLVMBasicBlockRef al_merge_bb = NULL;

                if (auto_lift && auto_lift_nil_check) {
                    LLVMValueRef cur_fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                    al_nil_bb = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "al_nil");
                    al_call_bb = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "al_call");
                    al_merge_bb = LLVMAppendBasicBlockInContext(ctx->context, cur_fn, "al_merge");
                    LLVMBuildCondBr(ctx->builder, auto_lift_nil_check, al_nil_bb, al_call_bb);

                    LLVMPositionBuilderAtEnd(ctx->builder, al_call_bb);
                }

                if (is_void_ret) {
                    LLVMBuildCall2(ctx->builder, call_ft, call_target,
                                   args, total_args, "");
                    /* Return a null ptr as sentinel for void functions */
                    LLVMTypeRef ptr_t = LLVMPointerType(
                        LLVMInt8TypeInContext(ctx->context), 0);
                    result.value = LLVMConstPointerNull(ptr_t);
                    result.type  = NULL; /* NULL signals void */
                } else {
                    /* fprintf(stderr, "DEBUG call ‘%s’ total_args=%d\n", head->symbol, total_args); */
                    for (int _di = 0; _di < total_args; _di++)
                        fprintf(stderr, "  arg[%d] llvm_type_kind=%d\n", _di,
                                (int)LLVMGetTypeKind(LLVMTypeOf(args[_di])));

                    result.value = LLVMBuildCall2(ctx->builder, call_ft, call_target,
                                                  args, total_args, "calltmp");
                    result.type  = entry->return_type
                        ? type_clone(entry->return_type) : NULL;
                    if (strcmp(head->symbol, "color-to-string") == 0) {
                        LLVMValueRef _dbg_fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->builder));
                        /* if (_dbg_fn) { fprintf(stderr, "DEBUG call site IR:\n"); LLVMDumpValue(_dbg_fn); } */
                    }
                    /* Propagate array size from argument if return type is
                     * unsized Arr and first argument is a sized array */
                    if (result.type && result.type->kind == TYPE_ARR &&
                        total_args > 0) {
                        for (int _ai = 0; _ai < declared_params; _ai++) {
                            if (entry->params[_ai].type &&
                                entry->params[_ai].type->kind == TYPE_ARR) {
                                AST *arg_ast = ast->list.items[_ai + 1];
                                int _sz = 0;
                                Type *_et = NULL;
                                if (arg_ast->type == AST_SYMBOL) {
                                    EnvEntry *ae = env_lookup(ctx->env, arg_ast->symbol);
                                    if (ae && ae->type &&
                                        ae->type->kind == TYPE_ARR) {
                                        _sz = ae->type->arr_size;
                                        _et = ae->type->arr_element_type
                                            ? type_clone(ae->type->arr_element_type)
                                            : type_int();
                                    }
                                } else if (arg_ast->type == AST_ARRAY) {
                                    _sz = (int)arg_ast->array.element_count;
                                    if (_sz > 0) {
                                        CodegenResult _er = codegen_expr(ctx,
                                            arg_ast->array.elements[0]);
                                        _et = _er.type
                                            ? type_clone(_er.type) : type_int();
                                    } else {
                                        _et = type_int();
                                    }
                                }
                                /* Propagate fat flag and element type */
                                result.type->arr_is_fat = true;
                                if (_sz > 0) result.type->arr_size = _sz;
                                if (_et && !result.type->arr_element_type)
                                    result.type->arr_element_type = _et;
                                else if (_et)
                                    type_free(_et);
                                break;
                            }
                        }
                    }

                }

                if (auto_lift && auto_lift_nil_check) {
                    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMValueRef boxed_res = result.value;
                    if (!is_void_ret && LLVMGetTypeKind(ret_llvm) != LLVMPointerTypeKind) {
                        boxed_res = codegen_box(ctx, result.value, result.type);
                    }
                    LLVMBasicBlockRef call_end_bb = LLVMGetInsertBlock(ctx->builder);
                    LLVMBuildBr(ctx->builder, al_merge_bb);

                    LLVMPositionBuilderAtEnd(ctx->builder, al_nil_bb);
                    LLVMValueRef nil_fn = get_rt_value_nil(ctx);
                    LLVMValueRef nil_res = LLVMBuildCall2(ctx->builder, LLVMFunctionType(ptr_t, NULL, 0, 0), nil_fn, NULL, 0, "nil_res");
                    LLVMBuildBr(ctx->builder, al_merge_bb);

                    LLVMPositionBuilderAtEnd(ctx->builder, al_merge_bb);
                    if (!is_void_ret) {
                        LLVMValueRef phi = LLVMBuildPhi(ctx->builder, ptr_t, "al_phi");
                        LLVMValueRef inc_vals[] = { nil_res, boxed_res };
                        LLVMBasicBlockRef inc_bbs[] = { al_nil_bb, call_end_bb };
                        LLVMAddIncoming(phi, inc_vals, inc_bbs, 2);
                        result.value = phi;
                    }
                    if (result.type) result.type->kind = TYPE_OPTIONAL;
                }

                free(args);
                return result;
            }

            // Type cast: (TypeName expr)
            const char *cast_target = NULL;
            const char *types[] = {"Int", "Float", "Char", "String", "Hex", "Bin", "Oct", "F32", "I8", "U8", "I16", "U16", "I32", "U32", "I64", "U64", "I128", "U128", NULL};
            for (int i = 0; types[i]; i++) if (strcmp(head->symbol, types[i]) == 0) { cast_target = types[i]; break; }

            if (cast_target) {

                if (ast->list.count != 2) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: type cast ‘%s’ requires exactly 1 argument",
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
                LLVMValueRef as_i64 = NULL;
                LLVMValueRef as_double = NULL;

                if (type_is_float(at)) {
                    if (at->kind == TYPE_F32) {
                        as_double = LLVMBuildFPExt(ctx->builder, av, LLVMDoubleTypeInContext(ctx->context), "to_double");
                    } else {
                        as_double = av;
                    }
                    as_i64 = LLVMBuildFPToSI(ctx->builder, as_double, LLVMInt64TypeInContext(ctx->context), "to_i64");
                } else if (type_is_integer(at) || at->kind == TYPE_BOOL) {
                    LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx->context);
                    LLVMTypeRef actual_t = LLVMTypeOf(av);
                    if (actual_t == i64_t) {
                        as_i64 = av;
                    } else if (LLVMGetTypeKind(actual_t) == LLVMIntegerTypeKind) {
                        if (type_is_unsigned(at) || at->kind == TYPE_BOOL) {
                            as_i64 = LLVMBuildZExt(ctx->builder, av, i64_t, "to_i64");
                        } else {
                            as_i64 = LLVMBuildSExt(ctx->builder, av, i64_t, "to_i64");
                        }
                    } else {
                        as_i64 = LLVMBuildPtrToInt(ctx->builder, av, i64_t, "to_i64");
                    }
                    if (type_is_unsigned(at) || at->kind == TYPE_BOOL) {
                        as_double = LLVMBuildUIToFP(ctx->builder, as_i64, LLVMDoubleTypeInContext(ctx->context), "to_double");
                    } else {
                        as_double = LLVMBuildSIToFP(ctx->builder, as_i64, LLVMDoubleTypeInContext(ctx->context), "to_double");
                    }
                } else if (at->kind == TYPE_STRING) {                    // hash the string pointer value (just use pointer as integer)
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
                } else if (strcmp(cast_target, "F32") == 0) {
                    LLVMValueRef f32val = as_double
                        ? LLVMBuildFPTrunc(ctx->builder, as_double,
                                           LLVMFloatTypeInContext(ctx->context), "to_f32")
                        : LLVMBuildSIToFP(ctx->builder, as_i64,
                                          LLVMFloatTypeInContext(ctx->context), "to_f32");
                    result.type  = type_f32();
                    result.value = f32val;
                } else if (strcmp(cast_target, "I8")  == 0) { result.type = type_i8();   result.value = LLVMBuildTrunc(ctx->builder, as_i64, LLVMInt8TypeInContext(ctx->context), "to_i8");
                } else if (strcmp(cast_target, "U8")  == 0) { result.type = type_u8();   result.value = LLVMBuildTrunc(ctx->builder, as_i64, LLVMInt8TypeInContext(ctx->context), "to_u8");
                } else if (strcmp(cast_target, "I16") == 0) { result.type = type_i16();  result.value = LLVMBuildTrunc(ctx->builder, as_i64, LLVMInt16TypeInContext(ctx->context), "to_i16");
                } else if (strcmp(cast_target, "U16") == 0) { result.type = type_u16();  result.value = LLVMBuildTrunc(ctx->builder, as_i64, LLVMInt16TypeInContext(ctx->context), "to_u16");
                } else if (strcmp(cast_target, "I32") == 0) { result.type = type_i32();  result.value = LLVMBuildTrunc(ctx->builder, as_i64, LLVMInt32TypeInContext(ctx->context), "to_i32");
                } else if (strcmp(cast_target, "U32") == 0) { result.type = type_u32();  result.value = LLVMBuildTrunc(ctx->builder, as_i64, LLVMInt32TypeInContext(ctx->context), "to_u32");
                } else if (strcmp(cast_target, "I64") == 0) { result.type = type_i64();  result.value = as_i64;
                } else if (strcmp(cast_target, "U64") == 0) { result.type = type_u64();  result.value = as_i64;
                } else if (strcmp(cast_target, "I128")== 0) { result.type = type_i128(); result.value = LLVMBuildSExt(ctx->builder, as_i64, LLVMInt128TypeInContext(ctx->context), "to_i128");
                } else if (strcmp(cast_target, "U128")== 0) { result.type = type_u128(); result.value = LLVMBuildZExt(ctx->builder, as_i64, LLVMInt128TypeInContext(ctx->context), "to_u128");
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

                    /* Scalar typedef handles (VkInstance, VkDevice, etc.) are
                     * just opaque pointers — allocate a single pointer-sized slot
                     * rather than a wrapper struct, so &handle gives VkInstance*
                     * directly as Vulkan expects.                               */
                    if (lay->layout_is_scalar) {
                        LLVMValueRef null_ptr = LLVMConstPointerNull(ptr);
                        result.value = null_ptr;
                        result.type  = type_clone(lay);
                        return result;
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
                                CODEGEN_ERROR(ctx, "%s:%d:%d: error: expected keyword argument in ‘%s’ constructor",
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
                                CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown field ‘:%s’ in layout ‘%s’",
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

            // Dot-access function call or array indexing: (p.x.y.func arg) or (font.chars i)
            {
                Type *dot_type = NULL;
                LLVMValueRef dot_ptr = codegen_dot_chain(ctx, head->symbol, &dot_type, head);
                if (dot_ptr) {
                    if (dot_type && dot_type->kind == TYPE_LAYOUT && dot_type->layout_field_count == 0 && dot_type->layout_name) {
                        Type *full = env_lookup_layout(ctx->env, dot_type->layout_name);
                        if (full) dot_type = full;
                    }
                    LLVMValueRef dot_val = LLVMBuildLoad2(ctx->builder, type_to_llvm(ctx, dot_type), dot_ptr, "dot_val");

                    // 1. Array Indexing: (font.chars i)
                    if (dot_type && dot_type->kind == TYPE_ARR && ast->list.count == 2) {
                        CodegenResult idx_r = codegen_expr(ctx, ast->list.items[1]);
                        LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                        LLVMValueRef idx = type_is_float(idx_r.type) ? LLVMBuildFPToSI(ctx->builder, idx_r.value, i64, "idx") : idx_r.value;
                        if (LLVMTypeOf(idx) != i64) idx = LLVMBuildZExt(ctx->builder, idx, i64, "idx64");

                        Type *elem_type = dot_type->arr_element_type ? dot_type->arr_element_type : type_int();
                        LLVMTypeRef elem_llvm = type_to_llvm(ctx, elem_type);

                        LLVMValueRef data_ptr;
                        if (dot_type->arr_is_fat) {
                            data_ptr = arr_fat_data(ctx, dot_val, elem_type);
                        } else if (LLVMGetTypeKind(type_to_llvm(ctx, dot_type)) == LLVMArrayTypeKind) {
                            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                            LLVMValueRef idx32 = LLVMBuildTrunc(ctx->builder, idx, LLVMInt32TypeInContext(ctx->context), "idx32");
                            LLVMValueRef indices[] = {zero, idx32};
                            LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder, type_to_llvm(ctx, dot_type), dot_ptr, indices, 2, "elem_ptr");
                            result.value = LLVMBuildLoad2(ctx->builder, elem_llvm, elem_ptr, "elem");
                            result.type = type_clone(elem_type);
                            return result;
                        } else {
                            data_ptr = dot_val;
                        }
                        LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder, elem_llvm, data_ptr, &idx, 1, "elem_ptr");
                        result.value = LLVMBuildLoad2(ctx->builder, elem_llvm, elem_ptr, "elem");
                        result.type  = type_clone(elem_type);
                        return result;
                    }

                    // 2. String Indexing: (layout.str i)
                    if (dot_type && dot_type->kind == TYPE_STRING && ast->list.count == 2) {
                        CodegenResult idx_r = codegen_expr(ctx, ast->list.items[1]);
                        LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
                        LLVMTypeRef i8  = LLVMInt8TypeInContext(ctx->context);
                        LLVMValueRef idx = type_is_float(idx_r.type) ? LLVMBuildFPToSI(ctx->builder, idx_r.value, i64, "idx") : idx_r.value;
                        if (LLVMTypeOf(idx) != i64) idx = LLVMBuildZExt(ctx->builder, idx, i64, "idx64");

                        LLVMValueRef char_ptr = LLVMBuildGEP2(ctx->builder, i8, dot_val, &idx, 1, "char_ptr");
                        result.value = LLVMBuildLoad2(ctx->builder, i8, char_ptr, "char");
                        result.type = type_char();
                        return result;
                    }

                    // 3. Function Call: (obj.func arg)
                    if (dot_type && (dot_type->kind == TYPE_FN || dot_type->kind == TYPE_ARROW || dot_type->kind == TYPE_UNKNOWN)) {
                        LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                        LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
                        int n_args = (int)ast->list.count - 1;
                        LLVMTypeRef arr_t = LLVMArrayType(ptr, n_args ? n_args : 1);
                        LLVMValueRef arr_ptr = LLVMBuildAlloca(ctx->builder, arr_t, "call_args");
                        for (int i = 0; i < n_args; i++) {
                            CodegenResult ar = codegen_expr(ctx, ast->list.items[i + 1]);
                            LLVMValueRef bv = codegen_box(ctx, ar.value, ar.type);
                            LLVMValueRef zero = LLVMConstInt(i32, 0, 0);
                            LLVMValueRef a_idx = LLVMConstInt(i32, i, 0);
                            LLVMValueRef idxs[] = {zero, a_idx};
                            LLVMValueRef slot = LLVMBuildGEP2(ctx->builder, arr_t, arr_ptr, idxs, 2, "slot");
                            LLVMBuildStore(ctx->builder, bv, slot);
                        }
                        LLVMValueRef args_ptr = n_args > 0 ? LLVMBuildBitCast(ctx->builder, arr_ptr, ptr, "args_ptr") : LLVMConstPointerNull(ptr);
                        LLVMValueRef calln_fn = get_rt_closure_calln(ctx);
                        LLVMTypeRef calln_p[] = {ptr, i32, ptr};
                        LLVMTypeRef calln_ft = LLVMFunctionType(ptr, calln_p, 3, 0);

                        LLVMValueRef clo_val = dot_val;
                        if (LLVMTypeOf(clo_val) != ptr) clo_val = LLVMBuildBitCast(ctx->builder, clo_val, ptr, "clo_cast");

                        LLVMValueRef calln_a[] = { clo_val, LLVMConstInt(i32, n_args, 0), args_ptr };
                        result.value = LLVMBuildCall2(ctx->builder, calln_ft, calln_fn, calln_a, 3, "dot_call");
                        result.type = type_unknown();
                        return result;
                    }

                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: cannot call or index type %s via dot access", parser_get_filename(), ast->line, ast->column, type_to_string(dot_type));
                }
            }

            // (dot base-expr field) — postfix dot on a call result
            if (strcmp(head->symbol, "dot") == 0 && ast->list.count == 3) {
                CodegenResult base = codegen_expr(ctx, ast->list.items[1]);
                const char *field_name = ast->list.items[2]->symbol;
                Type *lay = base.type;
                /* Unwrap pointer */
                if (lay && lay->kind == TYPE_PTR && lay->element_type)
                    lay = lay->element_type;
                /* Resolve named layout */
                if (lay && lay->kind == TYPE_LAYOUT &&
                    lay->layout_field_count == 0 && lay->layout_name) {
                    Type *resolved = env_lookup_layout(ctx->env, lay->layout_name);
                    if (resolved) lay = resolved;
                }
                /* Unknown/untyped pointer — search all layouts for the field */
                if (!lay || lay->kind != TYPE_LAYOUT) {
                    lay = env_find_layout_with_field(ctx->env, field_name);
                    if (!lay) {
                        CODEGEN_ERROR(ctx, "%s:%d:%d: error: no layout with field '%s' found",
                                      parser_get_filename(), ast->line, ast->column, field_name);
                    }
                }
                int field_idx = -1;
                for (int i = 0; i < lay->layout_field_count; i++) {
                    if (strcmp(lay->layout_fields[i].name, field_name) == 0) {
                        field_idx = i; break;
                    }
                }
                if (field_idx < 0) {
                    CODEGEN_ERROR(ctx, "%s:%d:%d: error: layout '%s' has no field '%s'",
                                  parser_get_filename(), ast->line, ast->column,
                                  lay->layout_name, field_name);
                }
                char sname[256];
                snprintf(sname, sizeof(sname), "layout.%s", lay->layout_name);
                LLVMTypeRef struct_llvm = LLVMGetTypeByName2(ctx->context, sname);
                LLVMValueRef ptr = base.value;
                if (LLVMGetTypeKind(LLVMTypeOf(ptr)) != LLVMPointerTypeKind) {
                    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
                    LLVMValueRef tmp = LLVMBuildAlloca(ctx->builder, LLVMTypeOf(ptr), "tmp");
                    LLVMBuildStore(ctx->builder, ptr, tmp);
                    ptr = tmp;
                }
                LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), 0, 0);
                LLVMValueRef fidx = LLVMConstInt(LLVMInt32TypeInContext(ctx->context), field_idx, 0);
                LLVMValueRef indices[] = {zero, fidx};
                LLVMValueRef fld_ptr = LLVMBuildGEP2(ctx->builder, struct_llvm, ptr, indices, 2, "fld");
                Type *fld_type = lay->layout_fields[field_idx].type;
                result.value = LLVMBuildLoad2(ctx->builder, type_to_llvm(ctx, fld_type), fld_ptr, field_name);
                result.type  = type_clone(fld_type);
                return result;
            }

            CODEGEN_ERROR(ctx, "%s:%d:%d: error: unknown function: %s",
                    parser_get_filename(), ast->line, ast->column, head->symbol);
        }
        result.value = codegen_current_fn_zero_value(ctx);
        result.type = type_unknown();
        return result;


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
        // Inline bindings as alloca/store in the current function scope.
        if (head->type == AST_LAMBDA) {
            int arg_count = ast->list.count - 1;

            Env *saved_env = ctx->env;
            ctx->env = env_create_child(saved_env);

            for (int i = 0; i < arg_count && i < head->lambda.param_count; i++) {
                CodegenResult init = codegen_expr(ctx, ast->list.items[i + 1]);
                ASTParam *param = &head->lambda.params[i];
                LLVMTypeRef llvm_type = type_to_llvm(ctx, init.type);
                LLVMValueRef alloca = LLVMBuildAlloca(ctx->builder, llvm_type,
                                                      param->name ? param->name : "let_tmp");
                LLVMBuildStore(ctx->builder, init.value, alloca);
                env_insert(ctx->env, param->name, type_clone(init.type), alloca);
            }

            CodegenResult body_result = {0};
            for (int i = 0; i < head->lambda.body_count; i++)
                body_result = codegen_expr(ctx, head->lambda.body_exprs[i]);

            ctx->env = saved_env;
            return body_result;
        }

        CODEGEN_ERROR(ctx, "%s:%d:%d: error: function call requires symbol in head position",
                parser_get_filename(), ast->line, ast->column);
    }

    case AST_REFINEMENT: {
        const char *rname     = ast->refinement.name;
        const char *var       = ast->refinement.var;
        const char *base      = ast->refinement.base_type;
        AST        *pred      = ast->refinement.predicate;
        const char *doc       = ast->refinement.docstring;
        const char *alias_sym = ast->refinement.alias_name;

        /* Validate that the base type exists before doing anything */
        if (base && rname) {
            Type *base_type = type_from_name(base);
            bool base_known = (base_type && base_type->kind != TYPE_UNKNOWN &&
                               base_type->kind != TYPE_VAR);
            if (!base_known) {
                for (TypeAlias *a = g_aliases; a && !base_known; a = a->next)
                    if (strcmp(a->alias_name, base) == 0) base_known = true;
                for (RefinementEntry *e = g_refinements; e && !base_known; e = e->next)
                    if (strcmp(e->name, base) == 0) base_known = true;
                if (!base_known && env_lookup_layout(ctx->env, base))
                    base_known = true;
            }
            if (!base_known) {
                CODEGEN_ERROR(ctx,
                    "%s:%d:%d: error: refinement type ‘%s’ has unknown base type ‘%s’ — "
                    "did you forget to define it first?",
                    parser_get_filename(), ast->line, ast->column,
                    rname, base);
            }
        }

        /* Anonymous refinement { x ∈ T | pred } used as a value —
         * emit as a lambda: (lambda ([x :: T]) pred)              */
        if (!rname) {
            ASTParam *params = malloc(sizeof(ASTParam));
            params[0].name      = strdup(var ? var : "x");
            params[0].type_name = base ? strdup(base) : NULL;
            params[0].is_rest   = false;
            params[0].is_anon   = false;

            AST **body_exprs = malloc(sizeof(AST*));
            body_exprs[0] = ast_clone(pred);

            AST *lam = ast_new_lambda(params, 1, "Bool", NULL, NULL, false,
                                      body_exprs[0], body_exprs, 1);
            lam->line   = ast->line;
            lam->column = ast->column;
            CodegenResult r = codegen_expr(ctx, lam);
            ast_free(lam);
            return r;
        }

        if (!isupper((unsigned char)rname[0])) {
            CODEGEN_ERROR(ctx, "%s:%d:%d: error: refinement type name ‘%s’ must start uppercase",
                          parser_get_filename(), ast->line, ast->column, rname);
        }

        /* 1. Register as a type alias and refinement */
        type_alias_register(rname, base);
        /* Also register the alias name if present so it works in type annotations */
        if (alias_sym) {
            type_alias_register(alias_sym, rname);
            /* fprintf(stderr, "DEBUG alias registered: ‘%s’ -> ‘%s’\n", alias_sym, rname); */
        }
        if (pred) {
            char pred_name_buf[256];
            snprintf(pred_name_buf, sizeof(pred_name_buf), "%s?", rname);
            refinement_register(rname, pred_name_buf, base, pred, var);
        }

        /* Pure alias — no predicate to generate */
        if (!pred) {
            if (alias_sym) {
                /* register the alias name too */
                type_alias_register(alias_sym, base);
            }
            printf("Type alias: %s -> %s\n", rname, base);
            result.type  = NULL;
            result.value = NULL;
            return result;
        }

        /* 2. Generate predicate function: Name? :: Base -> Bool
         *    (define (Name? var) pred)                          */
        char pred_name[256];
        snprintf(pred_name, sizeof(pred_name), "%s?", rname);

        /* Build: (define (Name? [var :: Base]) pred) */
        ASTParam *params = malloc(sizeof(ASTParam));
        params[0].name      = strdup(var);
        params[0].type_name = strdup(base);
        params[0].is_rest   = false;
        params[0].is_anon   = false;

        AST **body_exprs = malloc(sizeof(AST*));
        body_exprs[0] = ast_clone(pred);

        char pred_doc[512] = {0};
        if (doc)
            snprintf(pred_doc, sizeof(pred_doc), "Returns True if %s satisfies: %s", var, doc);
        else
            snprintf(pred_doc, sizeof(pred_doc), "Returns True if value satisfies %s refinement", rname);

        AST *pred_lambda = ast_new_lambda(params, 1, "Bool",
                                          pred_doc, NULL, false,
                                          body_exprs[0], body_exprs, 1);
        AST *pred_fname  = ast_new_symbol(pred_name);
        AST *pred_define = ast_new_list();
        ast_list_append(pred_define, ast_new_symbol("define"));
        ast_list_append(pred_define, pred_fname);
        ast_list_append(pred_define, pred_lambda);

        codegen_expr(ctx, pred_define);
        ast_free(pred_define);

        /* 3. Generate constructor: Name :: Base -> Base
         *    Checks predicate, errors if false, returns value  */
        char cons_doc[512] = {0};
        if (doc)
            snprintf(cons_doc, sizeof(cons_doc), "%s", doc);
        else
            snprintf(cons_doc, sizeof(cons_doc), "Construct a %s value (checks refinement predicate)", rname);

        /* Build the constructor body:
         * (if (Name? var) var (error "type error: value does not satisfy Name")) */
        AST *check_call = ast_new_list();
        ast_list_append(check_call, ast_new_symbol(pred_name));
        ast_list_append(check_call, ast_new_symbol(var));

        char errmsg[512];
        snprintf(errmsg, sizeof(errmsg),
                 "type error: value does not satisfy %s", rname);

        /* Use show + abort for the error branch */
        AST *err_show = ast_new_list();
        ast_list_append(err_show, ast_new_symbol("show"));
        ast_list_append(err_show, ast_new_string(errmsg));

        AST *cons_if = ast_new_list();
        ast_list_append(cons_if, ast_new_symbol("if"));
        ast_list_append(cons_if, check_call);
        ast_list_append(cons_if, ast_new_symbol(var));
        ast_list_append(cons_if, err_show);

        ASTParam *cparams = malloc(sizeof(ASTParam));
        cparams[0].name      = strdup(var);
        cparams[0].type_name = strdup(base);
        cparams[0].is_rest   = false;
        cparams[0].is_anon   = false;

        AST **cbody = malloc(sizeof(AST*));
        cbody[0] = cons_if;

        AST *cons_lambda = ast_new_lambda(cparams, 1, base,
                                          cons_doc, alias_sym, false,
                                          cons_if, cbody, 1);
        AST *cons_fname  = ast_new_symbol(rname);
        AST *cons_define = ast_new_list();
        ast_list_append(cons_define, ast_new_symbol("define"));
        ast_list_append(cons_define, cons_fname);
        ast_list_append(cons_define, cons_lambda);

        codegen_expr(ctx, cons_define);
        ast_free(cons_define);

        printf("Refinement type: %s (%s) where %s satisfies predicate\n",
               rname, base, var);

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

    case AST_DATA: {
        codegen_data(ctx, ast);
        result.type  = type_int();
        result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
        return result;
    }

    case AST_CLASS: {
        tc_register_class(ctx->tc_registry, ast);
        result.type  = type_int();
        result.value = LLVMConstInt(LLVMInt64TypeInContext(ctx->context), 0, 0);
        return result;
    }

    case AST_INSTANCE: {
        tc_register_instance(ctx->tc_registry, ast, ctx);
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
    if (!result.value) {
        result.value = codegen_current_fn_zero_value(ctx);
        result.type = type_unknown();
    }
    return result;
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
    env_insert_builtin(ctx->env, "??",        2,  0, "Null coalescing operator: (?? expr default)", NULL);
    // Arithmetic operators
    env_insert_builtin(ctx->env, "+",  1, -1, "Add numbers", NULL);
    env_insert_builtin(ctx->env, "-",  1, -1, "Subtract or negate numbers", NULL);
    env_insert_builtin(ctx->env, "*",  1, -1, "Multiply numbers", NULL);
    env_insert_builtin(ctx->env, "/",  1, -1, "Divide numbers", NULL);
    env_insert_builtin(ctx->env, "%",  2,  0, "Modulo operation", NULL);

    // Bitwise operators
    env_insert_builtin(ctx->env, "&",         2, -1, "Bitwise AND of integers: (& a b c ...).\n As a prefix &x returns the memory address of x", NULL);
    env_insert_builtin(ctx->env, "bit-or",    2, -1, "Bitwise OR of integers: (bit-or a b c ...)", NULL);
    env_insert_builtin(ctx->env, "∨",         2, -1, "Bitwise OR of integers: (∨ a b c ...)", NULL);
    env_insert_builtin(ctx->env, "bit-xor",   2, -1, "Bitwise XOR of integers: (bit-xor a b c ...)", NULL);
    env_insert_builtin(ctx->env, "⊕",         2, -1, "Bitwise XOR of integers: (⊕ a b c ...)", NULL);
    env_insert_builtin(ctx->env, "~",         1,  0, "Bitwise NOT of an integer: (~ x)", NULL);
    env_insert_builtin(ctx->env, "<<",        2,  0, "Left shift: (<< x n)", NULL);
    env_insert_builtin(ctx->env, ">>",        2,  0, "Arithmetic right shift, sign-preserving: (>> x n)", NULL);
    env_insert_builtin(ctx->env, ">>>",       2,  0, "Logical right shift, zero-fill: (>>> x n)", NULL);

    // Comparison operators
    env_insert_builtin(ctx->env, "=",  2, -1, "Test equality", NULL);
    env_insert_builtin(ctx->env, "!=", 2, -1, "Test inequality", NULL);
    env_insert_builtin(ctx->env, "<",  2, -1, "Less than", NULL);
    env_insert_builtin(ctx->env, "<=", 2, -1, "Less than or equal", NULL);
    env_insert_builtin(ctx->env, ">",  2, -1, "Greater than", NULL);
    env_insert_builtin(ctx->env, ">=", 2, -1, "Greater than or equal", NULL);

    env_insert_builtin(ctx->env, "code", 1, 0, "Return the source AST of a defined function", NULL);

    // Map
    env_insert_builtin(ctx->env, "Map?",     1, 0, "Test if value is a map", NULL);
    env_insert_builtin(ctx->env, "assoc",    3, 0, "Add or update a key-value pair in a map (immutable)", NULL);
    env_insert_builtin(ctx->env, "assoc!",   3, 0, "Add or update a key-value pair in a map in place", NULL);
    env_insert_builtin(ctx->env, "dissoc",   2, 0, "Remove a key from a map (immutable)", NULL);
    env_insert_builtin(ctx->env, "dissoc!",  2, 0, "Remove a key from a map in place", NULL);
    env_insert_builtin(ctx->env, "find",     2, 0, "Return (key val) pair for key in map, or nil", NULL);
    env_insert_builtin(ctx->env, "keys",     1, 0, "Return a list of all keys in a map", NULL);
    env_insert_builtin(ctx->env, "vals",     1, 0, "Return a list of all values in a map", NULL);
    env_insert_builtin(ctx->env, "merge",    2, 0, "Merge two maps, rightmost wins on conflict", NULL);

    // Set
    env_insert_builtin(ctx->env, "set",          0, -1, "Create a set from arguments or convert a collection", NULL);
    env_insert_builtin(ctx->env, "Set?",         1,  0, "Test if value is a set", NULL);
    env_insert_builtin(ctx->env, "collection?",  1,  0, "Test if value is a List, Set, or Arr", NULL);
    env_insert_builtin(ctx->env, "conj",         2,  0, "Add an element to a set", NULL);
    env_insert_builtin(ctx->env, "disj",         2,  0, "Remove an element from a set", NULL);
    env_insert_builtin(ctx->env, "conj!",        2,  0, "Mutate a set by adding an element in place", NULL);
    env_insert_builtin(ctx->env, "disj!",        2,  0, "Mutate a set by removing an element in place", NULL);
    env_insert_builtin(ctx->env, "contains?",    2,  0, "Test if a set contains an element", NULL);
    env_insert_builtin(ctx->env, "ends-with?",   2,  0, "Test if a string, list or array ends with a suffix", NULL);
    env_insert_builtin(ctx->env, "starts-with?", 2,  0, "Test if a string, list or array starts with a prefix", NULL);
    env_insert_builtin(ctx->env, "count",        1,  0, "Get number of elements in a set", NULL);

    // List operations
    env_insert_builtin(ctx->env, "list",    0, -1, "Create a list from arguments", NULL);
    env_insert_builtin(ctx->env, "cons",    2,  0, "Cons an element onto a list", NULL);
    env_insert_builtin(ctx->env, ".",       2,  0, "Cons an element onto a list", NULL);
    env_insert_builtin(ctx->env, "car",     1,  0, "Get first element of list", NULL);
    env_insert_builtin(ctx->env, "cdr",     1,  0, "Get rest of list", NULL);
    env_insert_builtin(ctx->env, "head",    1,  0, "Get first element of any collection (List, Array, String)", NULL);
    env_insert_builtin(ctx->env, "tail",    1,  0, "Get all but first element of any collection (List, Array, String)", NULL);
    env_insert_builtin(ctx->env, "length",  1,  0, "Get length of list or string", NULL);
    env_insert_builtin(ctx->env, "++",      2,  0, "Concatenate two collections", NULL);
    env_insert_builtin(ctx->env, "reverse", 1,  0, "Reverse a list", NULL);
    env_insert_builtin(ctx->env, "nth",     2,  0, "Get nth element of list (0-indexed)", NULL);
    { static const ParamKind pk[] = {PARAM_FUNC, PARAM_VALUE, PARAM_VALUE};
      env_insert_builtin(ctx->env, "reduce", 3, 0, "Reduce list with function and initial value", pk); }
    env_insert_builtin(ctx->env, "empty?",  1,  0, "Test if list is empty", NULL);
    env_insert_builtin(ctx->env, "pair?",   1,  0, "Test if a list is a dotted pair (tail is an atom)", NULL);
    env_insert_builtin(ctx->env, "append!", 2,  0, "Destructively append a value to a list in place: (append! xs val)", NULL);

    // Logic operators
    env_insert_builtin(ctx->env, "and", 2, -1, "Logical AND (short-circuit)", NULL);
    env_insert_builtin(ctx->env, "or",  2, -1, "Logical OR (short-circuit)", NULL);
    env_insert_builtin(ctx->env, "not", 1, -1, "Logical NOT", NULL);

    // Control flow
    env_insert_builtin(ctx->env, "if",     3,  0, "Conditional: (if cond then else)", NULL);
    env_insert_builtin(ctx->env, "for",    2, -1, "Loop with binding: (for [i 0 10] body) or (for [n] body)", NULL);
    env_insert_builtin(ctx->env, "while",  2, -1, "Loop while condition is true: (while cond body...)", NULL);
    env_insert_builtin(ctx->env, "until",  2, -1, "Loop until condition is true: (until cond body...)", NULL);
    env_insert_builtin(ctx->env, "unless", 2,  0, "Execute body if condition is False: (unless cond body)", NULL);
    env_insert_builtin(ctx->env, "when",   2, -1, "Execute when condition is true", NULL);
    env_insert_builtin(ctx->env, "unless", 2, -1, "Execute unless condition is true", NULL);
    env_insert_builtin(ctx->env, "cond",   1, -1, "Multi-branch conditional", NULL);
    env_insert_builtin(ctx->env, "let",    2, -1, "Binds variable(s) and executes body. Returns last expression.", NULL);

    // Special forms (even though handled specially, should be in environment)
    env_insert_builtin(ctx->env, "define",     2,  0, "Define a variable or function", NULL);
    env_insert_builtin(ctx->env, "include",   -1, -1, "Include a C header via FFI", NULL);
    env_insert_builtin(ctx->env, "import",     1,  0, "Import a module", NULL);
    env_insert_builtin(ctx->env, "module",     1, -1, "Declare a module", NULL);
    env_insert_builtin(ctx->env, "type",       1, -1, "Define a refinement type", NULL);
    env_insert_builtin(ctx->env, "lambda",     2, -1, "Create anonymous function", NULL);
    env_insert_builtin(ctx->env, "quote",      1,  0, "Quote expression without evaluation", NULL);
    env_insert_builtin(ctx->env, "show",       1,  0, "Print a value to stdout", NULL);
    env_insert_builtin(ctx->env, "let",        2, -1, "Bind variables in scope: (let ([x e] ...) body)", NULL);
    env_insert_builtin(ctx->env, "let*",       2, -1, "Bind variables sequentially, each visible to the next:\n (let* ([x e] [y (f x)] ...) body)", NULL);
    env_insert_builtin(ctx->env, "letrec",     2, -1, "Bind mutually recursive variables:\n (letrec ([f (lambda ...)]) body)", NULL);
    env_insert_builtin(ctx->env, "set!",       2,  0, "Mutate an existing variable: (set! x value)", NULL);

    // Type conversions
    env_insert_builtin(ctx->env, "Int",    1, 0, "Convert value to integer", NULL);
    env_insert_builtin(ctx->env, "Float",  1, 0, "Convert value to float", NULL);
    env_insert_builtin(ctx->env, "Char",   1, 0, "Convert value to character", NULL);
    env_insert_builtin(ctx->env, "String", 1, 0, "Convert value to string", NULL);
    env_insert_builtin(ctx->env, "Hex",    1, 0, "Convert to hexadecimal integer", NULL);
    env_insert_builtin(ctx->env, "Bin",    1, 0, "Convert to binary integer", NULL);
    env_insert_builtin(ctx->env, "Oct",    1, 0, "Convert to octal integer", NULL);
    env_insert_builtin(ctx->env, "F32",    1, 0, "Convert to 32-bit float", NULL);
    env_insert_builtin(ctx->env, "I8",     1, 0, "Convert to signed 8-bit integer", NULL);
    env_insert_builtin(ctx->env, "U8",     1, 0, "Convert to unsigned 8-bit integer", NULL);
    env_insert_builtin(ctx->env, "I16",    1, 0, "Convert to signed 16-bit integer", NULL);
    env_insert_builtin(ctx->env, "U16",    1, 0, "Convert to unsigned 16-bit integer", NULL);
    env_insert_builtin(ctx->env, "I32",    1, 0, "Convert to signed 32-bit integer", NULL);
    env_insert_builtin(ctx->env, "U32",    1, 0, "Convert to unsigned 32-bit integer", NULL);
    env_insert_builtin(ctx->env, "I64",    1, 0, "Convert to signed 64-bit integer", NULL);
    env_insert_builtin(ctx->env, "U64",    1, 0, "Convert to unsigned 64-bit integer", NULL);
    env_insert_builtin(ctx->env, "I128",   1, 0, "Convert to signed 128-bit integer", NULL);
    env_insert_builtin(ctx->env, "U128",   1, 0, "Convert to unsigned 128-bit integer", NULL);

    /* Type predicates — auto-generated for every known type */
    env_insert_builtin(ctx->env, "nil?",     1, 1, "Return True if value is nil (null pointer)", NULL);
    env_insert_builtin(ctx->env, "Int?",     1, 0, "Return True if value is an Int", NULL);
    env_insert_builtin(ctx->env, "Float?",   1, 0, "Return True if value is a Float", NULL);
    env_insert_builtin(ctx->env, "Char?",    1, 0, "Return True if value is a Char", NULL);
    env_insert_builtin(ctx->env, "String?",  1, 0, "Return True if value is a String", NULL);
    env_insert_builtin(ctx->env, "Bool?",    1, 0, "Return True if value is a Bool", NULL);
    env_insert_builtin(ctx->env, "Symbol?",  1, 0, "Return True if value is a Symbol", NULL);
    env_insert_builtin(ctx->env, "Keyword?", 1, 0, "Return True if value is a Keyword", NULL);
    env_insert_builtin(ctx->env, "List?",    1, 0, "Return True if value is a List", NULL);
    env_insert_builtin(ctx->env, "Ratio?",   1, 0, "Return True if value is a Ratio", NULL);
    env_insert_builtin(ctx->env, "Set?",     1, 0, "Return True if value is a Set", NULL);
    env_insert_builtin(ctx->env, "Map?",     1, 0, "Return True if value is a Map", NULL);
    env_insert_builtin(ctx->env, "Arr?",     1, 0, "Return True if value is an Arr", NULL);
    env_insert_builtin(ctx->env, "Fn?",      1, 0, "Return True if value is a Fn/closure", NULL);
    env_insert_builtin(ctx->env, "Number?",  1, 0, "Return True if value is Int or Float", NULL);
    env_insert_builtin(ctx->env, "Boolean?", 1, 0, "Return True if value is a Bool", NULL);
    env_insert_builtin(ctx->env, "Hex?",     1, 0, "Return True if value is a Hex", NULL);
    env_insert_builtin(ctx->env, "Bin?",     1, 0, "Return True if value is a Bin", NULL);
    env_insert_builtin(ctx->env, "Oct?",     1, 0, "Return True if value is an Oct", NULL);

    // String operations
    env_insert_builtin(ctx->env, "concat",      2, -1, "Concatenate strings", NULL);
    env_insert_builtin(ctx->env, "substring",   3,  0, "Get substring (str start end)", NULL);
    env_insert_builtin(ctx->env, "make-string", 2,  0, "Create a string of length n filled with char c", NULL);

    env_insert_builtin(ctx->env, "layout",   1, -1, "Define a struct layout", NULL);
    env_insert_builtin(ctx->env, "data",     1, -1, "Define an algebraic data type", NULL);
    env_insert_builtin(ctx->env, "class",    1, -1, "Define a typeclass", NULL);
    env_insert_builtin(ctx->env, "instance", 1, -1, "Define a typeclass instance", NULL);
    env_insert_builtin(ctx->env, "tests",    1, -1, "Define a test block", NULL);
    env_insert_builtin(ctx->env, "error",    1,  0, "Abort with a runtime error message: (error \"msg\")", NULL);
    env_insert_builtin(ctx->env, "asm",      1, -1, "Inline assembly block", NULL);

    env_insert_builtin(ctx->env, "rt_coll_empty", 1, 0, "Internal polymorphic empty collection", NULL);
    env_insert_builtin(ctx->env, "rt_coll_is_empty", 1, 0, "Internal polymorphic empty check (O(1))", NULL);
}
