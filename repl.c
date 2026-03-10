/*
 * repl.c
 *
 * Architecture:
 *   - One LLVMContext + one Env live for the whole session.
 *   - One MCJIT ExecutionEngine accumulates modules (one per expression).
 *   - fresh_module(): creates a new LLVMModule + builder, declares runtime
 *     functions, re-declares all env globals/funcs as extern so the verifier
 *     is happy, then registers every declaration's address with the engine
 *     via LLVMAddGlobalMapping so the JIT can actually call them.
 *   - codegen_expr() from codegen.c is called directly — no reimplementation.
 *   - exit() is shadowed by a macro that longjmps back to the eval loop so
 *     errors don't kill the process.
 */

#include "repl.h"
#include "reader.h"
#include "codegen.h"
#include "runtime.h"
#include "env.h"
#include "types.h"
#include "module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dlfcn.h>

#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/TargetMachine.h>

/* -------------------------------------------------------------------------
 * Error recovery: shadow exit() so codegen errors don't kill the process.
 * Only active while g_in_eval is true.
 * ------------------------------------------------------------------------- */
static jmp_buf g_repl_escape;
static bool    g_in_eval = false;

/* Catch SIGSEGV/SIGBUS from JIT-compiled code and longjmp back to the
 * eval loop so the REPL stays alive instead of crashing the process.
 * We reinstall SA_RESETHAND so a double-fault in host code still aborts. */
static void repl_signal_handler(int sig) {
    (void)sig;
    if (g_in_eval) {
        fprintf(stderr, "\nError: runtime crash (signal %d) in JIT code\n", sig);
        /* Restore default handler before longjmp so a future crash in
         * non-JIT code still terminates normally */
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS,  SIG_DFL);
        longjmp(g_repl_escape, 99);
    }
    /* Not in eval: re-raise so the process terminates with a core dump */
    signal(sig, SIG_DFL);
    raise(sig);
}

#define exit(code) \
    do { if (g_in_eval) longjmp(g_repl_escape, (code) ? (code) : 1); \
         else _Exit(code); } while(0)

/* -------------------------------------------------------------------------
 * Globals
 * ------------------------------------------------------------------------- */
/* Wrapper name is unique per expression to prevent MCJIT from returning
 * the cached address of the *first* compiled wrapper on every subsequent
 * LLVMGetFunctionAddress call.  Format: __repl_wrap_N */
#define WRAPPER_FMT  "__repl_wrap_%u"
static char g_wrapper_name[64] = "__repl_wrap_0";

static REPLContext *g_repl_ctx = NULL;

/* -------------------------------------------------------------------------
 * Runtime symbol table
 *
 * Every runtime function that generated code may call must be registered
 * with LLVMAddGlobalMapping on the specific LLVMValueRef declaration inside
 * the *current* module.  We cannot do this once at boot — each fresh_module()
 * creates new LLVMValueRefs via declare_runtime_functions(), and those new
 * values must be mapped individually.
 *
 * We keep a flat table of { name -> host address } and call
 * map_runtime_in_module() after every declare_runtime_functions() call.
 * ------------------------------------------------------------------------- */
typedef struct { const char *name; void *addr; } RTSym;

/* Populate the table lazily once using dlsym(RTLD_DEFAULT). */
static RTSym g_rt_syms[256];
static int   g_rt_sym_count = 0;

static void rt_sym_table_init(void) {
    if (g_rt_sym_count > 0) return;

#define ADD(n) do { \
    void *a = dlsym(RTLD_DEFAULT, #n); \
    if (a) { g_rt_syms[g_rt_sym_count].name = #n; \
              g_rt_syms[g_rt_sym_count].addr = a; \
              g_rt_sym_count++; } \
    } while(0)

    /* Runtime list */
    ADD(rt_list_create); ADD(rt_list_cons); ADD(rt_list_append);
    ADD(rt_list_car);    ADD(rt_list_cdr);  ADD(rt_list_nth);
    ADD(rt_list_length); ADD(rt_list_is_empty);
    ADD(rt_make_list);   ADD(rt_list_append_lists); ADD(rt_list_copy);
    ADD(rt_equal_p);
    /* Unboxing */
    ADD(rt_unbox_int);    ADD(rt_unbox_float); ADD(rt_unbox_char);
    ADD(rt_unbox_string); ADD(rt_unbox_list);  ADD(rt_value_is_nil);
    ADD(rt_print_value_newline);
    /* Value constructors */
    ADD(rt_value_int);   ADD(rt_value_float);   ADD(rt_value_char);
    ADD(rt_value_string);ADD(rt_value_symbol);  ADD(rt_value_keyword);
    ADD(rt_value_array); ADD(rt_array_set);      ADD(rt_array_get);
    ADD(rt_array_length);
    ADD(rt_value_list);  ADD(rt_value_nil);
    ADD(rt_print_value); ADD(rt_print_list);
    /* Ratio */
    ADD(rt_value_ratio);
    ADD(rt_ratio_add); ADD(rt_ratio_sub); ADD(rt_ratio_mul); ADD(rt_ratio_div);
    ADD(rt_ratio_to_int); ADD(rt_ratio_to_float);
    /* C stdlib used by codegen */
    ADD(printf); ADD(sprintf); ADD(fprintf);
    ADD(strlen);  ADD(strcmp);  ADD(strdup);
    ADD(malloc);  ADD(calloc);  ADD(memset); ADD(free); ADD(abort);
#undef ADD
}

/*
 * For every name in g_rt_syms, look up the matching LLVMValueRef in `mod`
 * (put there by declare_runtime_functions) and register its host address
 * with the engine.
 */
static void map_runtime_in_module(LLVMExecutionEngineRef engine,
                                  LLVMModuleRef mod) {
    for (int i = 0; i < g_rt_sym_count; i++) {
        LLVMValueRef fn = LLVMGetNamedFunction(mod, g_rt_syms[i].name);
        if (fn) LLVMAddGlobalMapping(engine, fn, g_rt_syms[i].addr);
    }
}

/* -------------------------------------------------------------------------
 * Re-declare env globals/funcs as extern in the current module
 *
 * LLVM verifier rejects IR that references globals from a different module.
 * We add extern declarations so the verifier passes, and the JIT resolves
 * them through the engine's accumulated symbol table.
 * ------------------------------------------------------------------------- */
/*
 * Imported-symbol address table.
 * When handle_import runs repl_compile_module + dlopen, it registers each
 * mangled symbol name and its host address here.  redeclare_env_symbols then
 * calls LLVMAddGlobalMapping so MCJIT can resolve them.
 */
typedef struct { char name[256]; void *addr; } ImportedSym;
#define MAX_IMPORTED 4096
static ImportedSym g_imported[MAX_IMPORTED];
static int         g_imported_count = 0;

static void register_imported_sym(const char *name, void *addr) {
    if (g_imported_count >= MAX_IMPORTED) return;
    strncpy(g_imported[g_imported_count].name, name, 255);
    g_imported[g_imported_count].addr = addr;
    g_imported_count++;
}

static void *lookup_imported_sym(const char *name) {
    for (int i = 0; i < g_imported_count; i++)
        if (strcmp(g_imported[i].name, name) == 0)
            return g_imported[i].addr;
    return NULL;
}

static void redeclare_env_symbols(REPLContext *ctx) {
    Env *env = ctx->cg.env;

    for (size_t bi = 0; bi < env->size; bi++) {
        for (EnvEntry *e = env->buckets[bi]; e; e = e->next) {

            if (e->kind == ENV_VAR && e->value &&
                LLVMIsAGlobalVariable(e->value)) {

                const char *name = LLVMGetValueName(e->value);
                if (!name || !*name) name = e->name;

                /* FIX: always update e->value to point at the declaration
                 * in the *current* module, even if one already exists.
                 * Without this, e->value keeps pointing at the LLVMValueRef
                 * from the module it was originally defined in, and codegen
                 * emits a cross-module reference that the verifier rejects. */
                LLVMValueRef existing = LLVMGetNamedGlobal(ctx->cg.module, name);
                if (existing) {
                    e->value = existing;
                    continue;
                }

                LLVMTypeRef  lt = type_to_llvm(&ctx->cg, e->type);
                LLVMValueRef gv = LLVMAddGlobal(ctx->cg.module, lt, name);
                LLVMSetLinkage(gv, LLVMExternalLinkage);
                e->value = gv;
            }

            else if (e->kind == ENV_FUNC && e->func_ref) {
                /* Use the mangled name stored on the LLVMValueRef */
                const char *name = LLVMGetValueName(e->func_ref);
                if (!name || !*name) name = e->name;

                /* FIX: always update e->func_ref to point at the declaration
                 * in the *current* module.  The old early-continue was leaving
                 * e->func_ref pointing at a ValueRef from __repl_0 (or
                 * whichever module first defined the function), causing the
                 * verifier to emit "Referencing function in another module!" */
                LLVMValueRef existing = LLVMGetNamedFunction(ctx->cg.module, name);
                if (existing) {
                    e->func_ref = existing;
                    continue;
                }

                LLVMTypeRef *pt = e->param_count > 0
                    ? malloc(sizeof(LLVMTypeRef) * e->param_count) : NULL;
                for (int i = 0; i < e->param_count; i++)
                    pt[i] = type_to_llvm(&ctx->cg, e->params[i].type);
                LLVMTypeRef ft = LLVMFunctionType(
                    type_to_llvm(&ctx->cg, e->return_type),
                    pt, e->param_count, 0);
                if (pt) free(pt);

                LLVMValueRef fn = LLVMAddFunction(ctx->cg.module, name, ft);
                LLVMSetLinkage(fn, LLVMExternalLinkage);
                e->func_ref = fn;
            }
        }
    }
}

/*
 * map_imported_in_module — called in close_and_run after LLVMAddModule.
 *
 * Registers addresses for:
 *   (a) runtime functions (g_rt_syms) — same as map_runtime_in_module but
 *       we now do both in one post-AddModule pass.
 *   (b) imported module symbols (g_imported) — functions/globals from
 *       dlopen'd modules.
 *
 * We look up each symbol by name in the module that was just handed to the
 * engine.  If it exists, we call LLVMAddGlobalMapping.  This is the correct
 * order: map AFTER AddModule, not before.
 */
static void map_imported_in_module(LLVMExecutionEngineRef engine,
                                   LLVMModuleRef mod) {
    for (int i = 0; i < g_imported_count; i++) {
        const char *name = g_imported[i].name;
        void       *addr = g_imported[i].addr;
        if (!addr) continue;

        /* Try as function first, then global variable */
        LLVMValueRef ref = LLVMGetNamedFunction(mod, name);
        if (!ref) ref = LLVMGetNamedGlobal(mod, name);
        if (ref) {
            LLVMAddGlobalMapping(engine, ref, addr);
            if (getenv("REPL_DUMP_IR"))
                fprintf(stderr, "[map] %s -> %p\n", name, addr);
        }
    }
}


/// Module lifecycle

static void fresh_module(REPLContext *ctx, const char *mod_name) {
    if (ctx->cg.builder) {
        LLVMDisposeBuilder(ctx->cg.builder);
        ctx->cg.builder = NULL;
    }
    /* Previous module was handed to the engine — do NOT dispose it. */

    ctx->cg.module  = LLVMModuleCreateWithNameInContext(mod_name,
                                                        ctx->cg.context);
    ctx->cg.builder = LLVMCreateBuilderInContext(ctx->cg.context);

    /* Reset cached format strings */
    ctx->cg.fmt_str = ctx->cg.fmt_char = ctx->cg.fmt_int =
    ctx->cg.fmt_float = ctx->cg.fmt_hex = ctx->cg.fmt_bin =
    ctx->cg.fmt_oct = NULL;

    /* 1. Declare runtime functions in this module */
    declare_runtime_functions(&ctx->cg);

    /* 2. Re-declare env globals/funcs from previous modules (declarations only).
     * LLVMAddGlobalMapping is called in close_and_run AFTER LLVMAddModule —
     * MCJIT requires mappings to be registered after the module is added.   */
    redeclare_env_symbols(ctx);
}

static void open_wrapper(REPLContext *ctx) {
    LLVMTypeRef  ft  = LLVMFunctionType(
        LLVMVoidTypeInContext(ctx->cg.context), NULL, 0, 0);
    LLVMValueRef wfn = LLVMAddFunction(ctx->cg.module, g_wrapper_name, ft);
    LLVMBasicBlockRef bb = LLVMAppendBasicBlockInContext(
        ctx->cg.context, wfn, "entry");
    LLVMPositionBuilderAtEnd(ctx->cg.builder, bb);
    ctx->cg.init_fn = wfn;
}

/// Auto-print

static bool expr_is_silent(AST *ast) {
    if (!ast || ast->type != AST_LIST || ast->list.count == 0) return false;
    if (ast->list.items[0]->type != AST_SYMBOL)               return false;
    const char *s = ast->list.items[0]->symbol;
    return strcmp(s, "define") == 0 ||
           strcmp(s, "show")   == 0 ||
           strcmp(s, "for")    == 0;
}

/* -------------------------------------------------------------------------
 * build_proc_description — format:
 *
 *   [name :: Fn (_ => _ . _)] ~[0x7f2dfcb50040]
 *   [Module.name :: Fn (param1 param2)] ~[0x...]
 *
 * Signature conventions:
 *   _         = required positional arg
 *   => _      = optional arg (from that point on)
 *   . _       = variadic rest arg
 *   Fn _      = zero-min variadic (e.g. list)
 *
 * The address suffix ~[0xADDR] shows the function pointer if known.
 * ------------------------------------------------------------------------- */
static void build_proc_description(EnvEntry *e, char *buf, size_t sz) {
    char name_buf[128];
    if (e->module_name)
        snprintf(name_buf, sizeof(name_buf), "%s.%s", e->module_name, e->name);
    else
        snprintf(name_buf, sizeof(name_buf), "%s", e->name);

    char sig[256] = "";

    if (e->kind == ENV_FUNC) {
        /* Named parameters from env */
        if (e->param_count == 0) {
            strcpy(sig, "");
        } else {
            for (int i = 0; i < e->param_count; i++) {
                if (i > 0) strncat(sig, " ", sizeof(sig) - strlen(sig) - 1);
                const char *pn = (e->params[i].name && e->params[i].name[0])
                                 ? e->params[i].name : "_";
                strncat(sig, pn, sizeof(sig) - strlen(sig) - 1);
            }
        }
        /* Addr from func_ref */
        char addr_buf[32] = "";
        if (e->func_ref) {
            snprintf(addr_buf, sizeof(addr_buf), " ~[%p]",
                     (void*)(uintptr_t)e->func_ref);
        }
        if (sig[0])
            snprintf(buf, sz, "[%s :: Fn (%s)]%s", name_buf, sig, addr_buf);
        else
            snprintf(buf, sz, "[%s :: Fn]%s", name_buf, addr_buf);
    } else {
        /* Builtin: render arity as _  =>  .  convention */
        int mn = e->arity_min > 0 ? e->arity_min : 0;
        int mx = e->arity_max; /* -1 = variadic */

        if (mn == 0 && mx == -1) {
            /* zero-min variadic: Fn _ */
            strcpy(sig, "");
            snprintf(buf, sz, "[%s :: Fn _]", name_buf);
            return;
        }

        /* Required args */
        for (int i = 0; i < mn; i++) {
            if (i > 0) strncat(sig, " ", sizeof(sig) - strlen(sig) - 1);
            strncat(sig, "_", sizeof(sig) - strlen(sig) - 1);
        }

        /* Optional args */
        if (mx > mn && mx != -1) {
            strncat(sig, mn > 0 ? " =>" : "=>", sizeof(sig) - strlen(sig) - 1);
            for (int i = mn; i < mx; i++)
                strncat(sig, " _", sizeof(sig) - strlen(sig) - 1);
        }

        /* Rest / variadic */
        if (mx == -1 && mn > 0) {
            strncat(sig, " => _ . _", sizeof(sig) - strlen(sig) - 1);
        }

        if (sig[0])
            snprintf(buf, sz, "[%s :: Fn (%s)]", name_buf, sig);
        else
            snprintf(buf, sz, "[%s :: Fn]", name_buf);
    }
}

/* -------------------------------------------------------------------------
 * emit_auto_print — generate IR to print `val` of type `t`.
 *
 * Key design decisions:
 *
 *   TYPE_LIST  — codegen returns a raw RuntimeList* (from rt_list_create /
 *                rt_list_cons etc.).  We call rt_print_list(list) + "\n".
 *                NOT rt_print_value_newline — that expects a RuntimeValue*.
 *
 *   TYPE_ARR   — val is a stack [N x ElemT]*.  We print it with a compile-
 *                time-unrolled printf loop.  We never call rt_value_array
 *                because that symbol may not be exported from the runtime.
 *
 *   TYPE_RATIO / TYPE_SYMBOL / TYPE_KEYWORD / TYPE_UNKNOWN
 *              — val IS already a RuntimeValue*. Use rt_print_value_newline.
 *
 *   NULL type  — imported function result whose type wasn't propagated.
 *                Treat as RuntimeValue* and call rt_print_value_newline.
 * ------------------------------------------------------------------------- */
static void emit_auto_print(REPLContext *ctx, LLVMValueRef val, Type *t) {
    if (!val) return;

    LLVMValueRef pf  = get_or_declare_printf(&ctx->cg);
    LLVMTypeRef  ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->cg.context), 0);

    /* Emit call to rt_print_value_newline(val) — val must be RuntimeValue* */
#define EMIT_RV_PRINT(v) \
    do { \
        LLVMValueRef _pfn = get_rt_print_value_newline(&ctx->cg); \
        LLVMTypeRef  _ft  = LLVMFunctionType( \
            LLVMVoidTypeInContext(ctx->cg.context), &ptr, 1, 0); \
        LLVMValueRef _a[] = {(v)}; \
        LLVMBuildCall2(ctx->cg.builder, _ft, _pfn, _a, 1, ""); \
    } while(0)

    /* Emit call to rt_print_list(list) + printf("\n") — val must be RuntimeList* */
#define EMIT_LIST_PRINT(v) \
    do { \
        LLVMValueRef _pfn = get_rt_print_list(&ctx->cg); \
        LLVMTypeRef  _ft  = LLVMFunctionType( \
            LLVMVoidTypeInContext(ctx->cg.context), &ptr, 1, 0); \
        LLVMValueRef _a[] = {(v)}; \
        LLVMBuildCall2(ctx->cg.builder, _ft, _pfn, _a, 1, ""); \
        LLVMValueRef _nl = LLVMBuildGlobalStringPtr(ctx->cg.builder, "\n", "nl"); \
        LLVMValueRef _na[] = {_nl}; \
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, _na, 1, ""); \
    } while(0)

    /* NULL type: best-effort — assume RuntimeValue* (covers imported functions) */
    if (!t) {
        EMIT_RV_PRINT(val);
        return;
    }

    switch (t->kind) {

    case TYPE_FLOAT: {
        LLVMValueRef a[] = {get_fmt_float(&ctx->cg), val};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
        break; }

    case TYPE_INT: case TYPE_HEX: case TYPE_BIN: case TYPE_OCT: {
        LLVMValueRef a[] = {get_fmt_int(&ctx->cg), val};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
        break; }

    case TYPE_CHAR: {
        LLVMValueRef a[] = {get_fmt_char(&ctx->cg), val};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
        break; }

    case TYPE_STRING: {
        LLVMValueRef a[] = {get_fmt_str(&ctx->cg), val};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
        break; }

    case TYPE_BOOL: {
        LLVMValueRef ts = LLVMBuildGlobalStringPtr(ctx->cg.builder, "True\n",  "ts");
        LLVMValueRef fs = LLVMBuildGlobalStringPtr(ctx->cg.builder, "False\n", "fs");
        LLVMValueRef s  = LLVMBuildSelect(ctx->cg.builder, val, ts, fs, "bs");
        LLVMValueRef a[] = {s};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 1, "");
        break; }

    case TYPE_LIST:
        /* val is RuntimeList* — use rt_print_list, NOT rt_print_value_newline */
        EMIT_LIST_PRINT(val);
        break;

    case TYPE_RATIO:
    case TYPE_SYMBOL:
    case TYPE_KEYWORD:
    case TYPE_UNKNOWN:
        /* val is RuntimeValue* */
        EMIT_RV_PRINT(val);
        break;

    case TYPE_ARR: {
        /* val is a stack-allocated [N x ElemType]*.
         * We never call rt_value_array (may not be exported).
         * Instead: print "[" then each element with printf, then "]\n".
         * All element types we support are either i64, double, or i8. */
        if (!t->arr_element_type || t->arr_size <= 0) {
            /* Unknown shape — print as pointer */
            LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->cg.context);
            LLVMValueRef as_int = LLVMBuildPtrToInt(ctx->cg.builder, val, i64, "ap");
            LLVMValueRef a[] = {get_fmt_int(&ctx->cg), as_int};
            LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
            break;
        }

        int n = t->arr_size;
        LLVMTypeRef arr_llvm  = type_to_llvm(&ctx->cg, t);
        LLVMTypeRef elem_llvm = type_to_llvm(&ctx->cg, t->arr_element_type);
        TypeKind    ek        = t->arr_element_type->kind;

        /* "[" */
        { LLVMValueRef s = LLVMBuildGlobalStringPtr(ctx->cg.builder, "[", "ob");
          LLVMValueRef a[] = {s};
          LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 1, ""); }

        for (int i = 0; i < n; i++) {
            if (i > 0) {
                LLVMValueRef sp = LLVMBuildGlobalStringPtr(ctx->cg.builder, " ", "sp");
                LLVMValueRef a[] = {sp};
                LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 1, "");
            }
            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(ctx->cg.context), 0, 0);
            LLVMValueRef idx  = LLVMConstInt(LLVMInt32TypeInContext(ctx->cg.context), i, 0);
            LLVMValueRef idxs[] = {zero, idx};
            LLVMValueRef ep = LLVMBuildGEP2(ctx->cg.builder, arr_llvm, val, idxs, 2, "ep");
            LLVMValueRef el = LLVMBuildLoad2(ctx->cg.builder, elem_llvm, ep, "el");

            if (ek == TYPE_FLOAT) {
                LLVMValueRef a[] = {get_fmt_float_no_newline(&ctx->cg), el};
                LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
            } else if (ek == TYPE_CHAR) {
                LLVMValueRef a[] = {get_fmt_char_no_newline(&ctx->cg), el};
                LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
            } else {
                /* INT / HEX / BIN / OCT — widen to i64 if needed */
                LLVMTypeRef i64t = LLVMInt64TypeInContext(ctx->cg.context);
                if (LLVMTypeOf(el) != i64t)
                    el = LLVMBuildZExt(ctx->cg.builder, el, i64t, "wi");
                LLVMValueRef a[] = {get_fmt_int_no_newline(&ctx->cg), el};
                LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
            }
        }

        /* "]\n" */
        { LLVMValueRef s = LLVMBuildGlobalStringPtr(ctx->cg.builder, "]\n", "cb");
          LLVMValueRef a[] = {s};
          LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 1, ""); }
        break;
    }

    default: break;
    }

#undef EMIT_RV_PRINT
#undef EMIT_LIST_PRINT
}

/* -------------------------------------------------------------------------
 * emit_proc_print — emit IR that prints a procedure description string.
 * We bake the description as a global string constant at JIT compile time.
 * ------------------------------------------------------------------------- */
static void emit_proc_print(REPLContext *ctx, EnvEntry *e) {
    char desc[512];
    build_proc_description(e, desc, sizeof(desc));

    /* Append newline */
    char desc_nl[520];
    snprintf(desc_nl, sizeof(desc_nl), "%s\n", desc);

    LLVMValueRef gs  = LLVMBuildGlobalStringPtr(ctx->cg.builder, desc_nl, "procdesc");
    LLVMValueRef pf  = get_or_declare_printf(&ctx->cg);
    LLVMValueRef fmt = get_fmt_str(&ctx->cg);
    LLVMValueRef a[] = {fmt, gs};
    LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
}

/// JIT compile and run

static bool close_and_run(REPLContext *ctx) {
    LLVMBuildRetVoid(ctx->cg.builder);

    LLVMValueRef wfn = LLVMGetNamedFunction(ctx->cg.module, g_wrapper_name);
    if (LLVMVerifyFunction(wfn, LLVMPrintMessageAction) != 0) {
        fprintf(stderr, "Error: IR verification failed\n");
        return false;
    }
    char *err = NULL;
    if (LLVMVerifyModule(ctx->cg.module, LLVMPrintMessageAction, &err) != 0) {
        fprintf(stderr, "Error: module verification: %s\n", err ? err : "");
        LLVMDisposeMessage(err);
        return false;
    }
    if (err) LLVMDisposeMessage(err);

    if (getenv("REPL_DUMP_IR")) {
        char *ir = LLVMPrintModuleToString(ctx->cg.module);
        fprintf(stderr, "=== IR ===\n%s\n=== END IR ===\n", ir);
        LLVMDisposeMessage(ir);
    }
    LLVMModuleRef mod = ctx->cg.module;  /* save before handing to engine */
    LLVMAddModule(ctx->engine, mod);
    ctx->cg.module = NULL;   /* engine owns it now */

    /* Register all symbol addresses NOW — MCJIT requires mappings to be
     * set after the module is added to the engine, not before.          */
    map_runtime_in_module(ctx->engine, mod);
    map_imported_in_module(ctx->engine, mod);

    void (*fn)(void) = (void(*)(void))
        LLVMGetFunctionAddress(ctx->engine, g_wrapper_name);
    if (!fn) {
        fprintf(stderr, "Error: JIT could not find %s\n", g_wrapper_name);
        return false;
    }
    if (getenv("REPL_DUMP_IR"))
        fprintf(stderr, "[jit] %s -> %p\n", g_wrapper_name, (void*)fn);
    fn();
    return true;
}

// Discard a broken module and recover a clean state
static void recover_module(REPLContext *ctx) {
    if (ctx->cg.module) {
        LLVMDisposeModule(ctx->cg.module);
        ctx->cg.module = NULL;
    }
    char name[64];
    snprintf(name, sizeof(name), "__repl_%u_r", ctx->expr_count);
    fresh_module(ctx, name);
}

/// Import support

// Compile the module to a shared library, dlopen it, run its init function,
// then use nm to discover exported symbols and inject them into the env.

static bool file_exists_r(const char *p) { return access(p, F_OK) == 0; }
static time_t file_mtime_r(const char *p) {
    struct stat st; return stat(p, &st) == 0 ? st.st_mtime : 0;
}

static bool handle_import(REPLContext *ctx, AST *ast) {
    if (ast->list.count < 2 || ast->list.items[1]->type != AST_SYMBOL) {
        fprintf(stderr, "Error: import requires a module name symbol\n");
        return false;
    }
    ImportDecl *imp = parse_import_decl(ast);
    if (!imp) { fprintf(stderr, "Error: invalid import\n"); return false; }

    const char *mod_name = imp->module_name;

    /* Step 1: Run the full compiler pipeline (compile_one + declare_externals).
     * This populates ctx->cg.env with typed entries for every export. */
    if (!repl_compile_module(&ctx->cg, imp)) {
        fprintf(stderr, "Error: failed to compile module '%s'\n", mod_name);
        return false;
    }

    /* Step 2: Build a .so from the cached .o so we can dlopen it and get
     * real host addresses for every exported symbol.                       */

    /* Locate the .o that compile_one wrote */
    char *src_path = module_name_to_path(mod_name);
    char obj_path[512], so_path[512];

    /* Mirror get_obj_path() logic: core modules go to ~/.cache/monad/core/ */
    const char *home = getenv("HOME");
    const char *sys_core = "/usr/local/lib/monad/core/";
    if (home && src_path && strncmp(src_path, sys_core, strlen(sys_core)) == 0) {
        const char *rel = src_path + strlen(sys_core);
        char base[512]; strncpy(base, rel, sizeof(base)-1);
        /* strip extension */
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        /* flatten slashes */
        for (char *p = base; *p; p++) if (*p == '/') *p = '_';
        snprintf(obj_path, sizeof(obj_path), "%s/.cache/monad/core/%s.o", home, base);
    } else {
        /* local module: .o sits next to the source */
        char base[512];
        strncpy(base, src_path ? src_path : mod_name, sizeof(base)-1);
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        snprintf(obj_path, sizeof(obj_path), "%s.o", base);
    }
    free(src_path);

    snprintf(so_path, sizeof(so_path), "/tmp/__mrepl_%s.so", mod_name);

    if (!file_exists_r(obj_path)) {
        fprintf(stderr, "Error: object file not found: %s\n", obj_path);
        return false;
    }

    /* Rebuild .so if stale */
    if (!file_exists_r(so_path) || file_mtime_r(obj_path) > file_mtime_r(so_path)) {
        char cmd[2048];
        /* Do NOT link libmonad.a here — runtime symbols are already in
         * the host monad binary.  We use --unresolved-symbols=ignore-all
         * so the link succeeds; dlopen(RTLD_GLOBAL) resolves them from
         * the host process at load time.                                 */
        snprintf(cmd, sizeof(cmd),
                 "gcc -shared -fPIC -o %s %s"
                 " -Wl,--unresolved-symbols=ignore-all -lm 2>&1",
                 so_path, obj_path);
        if (system(cmd) != 0) {
            fprintf(stderr, "Error: failed to link .so for '%s'\n", mod_name);
            return false;
        }
    }

    /* Step 3: dlopen so the symbols live in the host process.
     * First re-open the main binary with RTLD_GLOBAL so its symbols
     * (rt_list_create etc.) are visible when the .so is loaded.     */
    { void *self = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL); (void)self; }
    void *handle = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        fprintf(stderr, "Error: dlopen %s: %s\n", so_path, dlerror());
        return false;
    }

    /* Run module init so top-level stores execute (e.g. lowercase = "abc...") */
    char init_name[256];
    snprintf(init_name, sizeof(init_name), "__init_%s", mod_name);
    for (char *p = init_name + 8; *p; p++) if (*p == '.') *p = '_';
    void (*init_fn)(void) = (void(*)(void))dlsym(handle, init_name);
    if (init_fn) init_fn();

    /* Step 4: Walk the .so exports and register addresses in g_imported[].
     * We use the same mangle prefix as the compiler: ModName__ */
    char prefix[256];
    size_t ml = strlen(mod_name);
    for (size_t i = 0; i < ml; i++)
        prefix[i] = (mod_name[i] == '.') ? '_' : mod_name[i];
    prefix[ml] = prefix[ml+1] = '_'; prefix[ml+2] = '\0';
    size_t prefix_len = ml + 2;

    char nm_cmd[512];
    snprintf(nm_cmd, sizeof(nm_cmd),
             "nm -D --defined-only %s 2>/dev/null", so_path);
    FILE *nm = popen(nm_cmd, "r");
    int count = 0;
    if (nm) {
        char line[512];
        while (fgets(line, sizeof(line), nm)) {
            char addr_s[32], tc[4], sym[256];
            if (sscanf(line, "%31s %3s %255s", addr_s, tc, sym) != 3) continue;
            if (tc[0]!='T' && tc[0]!='D' && tc[0]!='B') continue;
            if (strncmp(sym, prefix, prefix_len) != 0) continue;
            void *addr = dlsym(handle, sym);
            if (!addr) continue;
            register_imported_sym(sym, addr);
            count++;
        }
        pclose(nm);
    }

    /* Step 5: Harvest docstrings from the source file.
     * The manifest / .o carry no docstring info, but the source is still
     * on disk.  We re-parse each top-level (define (fn ...) "docstring" ...)
     * form and write the docstring into the already-populated env entry so
     * (doc list-reverse) etc. work correctly.
     *
     * We only patch ENV_FUNC entries whose module_name matches mod_name.
     */
    {
        char *src = module_name_to_path(mod_name);
        if (src && file_exists_r(src)) {
            FILE *sf = fopen(src, "r");
            if (sf) {
                fseek(sf, 0, SEEK_END);
                long fsz = ftell(sf);
                rewind(sf);
                char *src_buf = malloc(fsz + 1);
                if (src_buf && fsz > 0) {
                    fread(src_buf, 1, fsz, sf);
                    src_buf[fsz] = '\0';
                    fclose(sf);

                    /* Parse top-level forms one at a time using the same
                     * parser that the compiler uses.  We advance through the
                     * source string and call parse() repeatedly.            */
                    const char *cursor = src_buf;
                    while (*cursor) {
                        /* Skip whitespace and comments */
                        while (*cursor && (isspace((unsigned char)*cursor) ||
                               (*cursor == ';'))) {
                            if (*cursor == ';')
                                while (*cursor && *cursor != '\n') cursor++;
                            else
                                cursor++;
                        }
                        if (!*cursor) break;

                        parser_set_context(src, cursor);
                        AST *form = parse(cursor);
                        if (!form) break;

                        /* Advance cursor past this form (approximate: find
                         * the closing paren at depth 0)                    */
                        if (*cursor == '(') {
                            int depth = 0;
                            while (*cursor) {
                                if (*cursor == '(') depth++;
                                else if (*cursor == ')') { depth--; if (depth == 0) { cursor++; break; } }
                                cursor++;
                            }
                        } else {
                            /* Non-list form: skip to next whitespace */
                            while (*cursor && !isspace((unsigned char)*cursor)) cursor++;
                        }

                        /* We only care about: (define (name ...) "docstring" ...) */
                        if (form->type == AST_LIST && form->list.count >= 3 &&
                            form->list.items[0]->type == AST_SYMBOL &&
                            strcmp(form->list.items[0]->symbol, "define") == 0) {

                            AST *lam = form->list.items[2];
                            if (lam->type == AST_LAMBDA &&
                                lam->lambda.docstring && lam->lambda.docstring[0]) {

                                AST *name_node = form->list.items[1];
                                const char *fn_name = NULL;
                                if (name_node->type == AST_LIST &&
                                    name_node->list.count > 0 &&
                                    name_node->list.items[0]->type == AST_SYMBOL) {
                                    fn_name = name_node->list.items[0]->symbol;
                                } else if (name_node->type == AST_SYMBOL) {
                                    fn_name = name_node->symbol;
                                }

                                if (fn_name) {
                                    EnvEntry *ent = env_lookup(ctx->cg.env, fn_name);
                                    if (ent && ent->module_name &&
                                        strcmp(ent->module_name, mod_name) == 0) {
                                        free(ent->docstring);
                                        ent->docstring = strdup(lam->lambda.docstring);
                                    }
                                }
                            }
                        }
                        ast_free(form);
                    }
                    free(src_buf);
                } else {
                    if (src_buf) free(src_buf);
                    fclose(sf);
                }
            }
        }
        free(src);
    }

    printf("Imported %d symbol(s) from '%s'.\n", count, mod_name);
    return true;
}

/// API

void repl_init(REPLContext *ctx) {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    LLVMLinkInMCJIT();

    rt_sym_table_init();

    /* Install signal handlers so JIT crashes don't kill the process */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = repl_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND; /* auto-restore default after first delivery */
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);

    ctx->cg.context    = LLVMContextCreate();
    ctx->cg.module     = NULL;
    ctx->cg.builder    = NULL;
    ctx->cg.env        = env_create();
    ctx->cg.module_ctx = NULL;
    ctx->cg.init_fn    = NULL;
    ctx->cg.test_mode  = false;
    ctx->cg.fmt_str = ctx->cg.fmt_char = ctx->cg.fmt_int =
    ctx->cg.fmt_float = ctx->cg.fmt_hex = ctx->cg.fmt_bin =
    ctx->cg.fmt_oct = NULL;

    /* Bootstrap engine from an empty module */
    LLVMModuleRef boot = LLVMModuleCreateWithNameInContext(
        "__repl_boot", ctx->cg.context);
    char *error = NULL;
    if (LLVMCreateExecutionEngineForModule(&ctx->engine, boot, &error) != 0) {
        fprintf(stderr, "REPL: engine creation failed: %s\n", error);
        LLVMDisposeMessage(error);
        _Exit(1);
    }
    /* boot is owned by engine now */

    register_builtins(&ctx->cg);
    ctx->expr_count = 0;

    /* First real module */
    fresh_module(ctx, "__repl_0");

    printf("Monad REPL v0.1\n");
    printf("Type expressions to evaluate, Ctrl-D to exit.\n\n");
}

void repl_dispose(REPLContext *ctx) {
    if (ctx->cg.builder) LLVMDisposeBuilder(ctx->cg.builder);
    if (ctx->cg.module)  LLVMDisposeModule(ctx->cg.module);
    LLVMDisposeExecutionEngine(ctx->engine);
    env_free(ctx->cg.env);
}

/* -------------------------------------------------------------------------
 * repl_eval_line — parse one line, codegen it, JIT-run it.
 *
 * Changes vs original:
 *   - Bare symbol that resolves to ENV_FUNC or ENV_BUILTIN: don't call
 *     codegen_expr (which would crash or produce meaningless IR), instead
 *     emit a proc-description string directly via emit_proc_print.
 *   - emit_auto_print now also handles NULL type (imported fn results).
 * ------------------------------------------------------------------------- */
bool repl_eval_line(REPLContext *ctx, const char *line) {
    if (!line) return true;
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return true;

    parser_set_context("<input>", line);
    AST *ast = parse(line);
    if (!ast) { fprintf(stderr, "Error: parse failed\n"); return false; }

    /* Special: (import ...) */
    if (ast->type == AST_LIST && ast->list.count >= 1 &&
        ast->list.items[0]->type == AST_SYMBOL &&
        strcmp(ast->list.items[0]->symbol, "import") == 0) {
        bool ok = handle_import(ctx, ast);
        ast_free(ast);
        return ok;
    }

    bool silent = expr_is_silent(ast);

    /* -----------------------------------------------------------------------
     * Fast path: bare symbol that names a function or builtin.
     * codegen_expr would try to load/call it as a value which is wrong.
     * Instead we build a trivial wrapper that just prints the description.
     * --------------------------------------------------------------------- */
    if (!silent && ast->type == AST_SYMBOL) {
        EnvEntry *e = env_lookup(ctx->cg.env, ast->symbol);
        if (e && (e->kind == ENV_FUNC || e->kind == ENV_BUILTIN)) {
            char mod_name[64];
            snprintf(mod_name,       sizeof(mod_name),       "__repl_%u",  ctx->expr_count);
            snprintf(g_wrapper_name, sizeof(g_wrapper_name), WRAPPER_FMT,  ctx->expr_count);
            fresh_module(ctx, mod_name);
            open_wrapper(ctx);
            emit_proc_print(ctx, e);
            ast_free(ast);

            signal(SIGSEGV, repl_signal_handler);
            signal(SIGBUS,  repl_signal_handler);
            g_in_eval = true;
            bool ran = false;
            if (setjmp(g_repl_escape) == 0)
                ran = close_and_run(ctx);
            g_in_eval = false;

            if (ran) ctx->expr_count++;
            char next[64];
            snprintf(next, sizeof(next), "__repl_%u", ctx->expr_count);
            fresh_module(ctx, next);
            return ran;
        }
    }

    /* Unique names per expression — MCJIT caches by name, reusing the same
     * name would return the first compiled wrapper's address every time. */
    char mod_name[64];
    snprintf(mod_name,      sizeof(mod_name),      "__repl_%u",      ctx->expr_count);
    snprintf(g_wrapper_name, sizeof(g_wrapper_name), WRAPPER_FMT, ctx->expr_count);
    fresh_module(ctx, mod_name);
    open_wrapper(ctx);

    /* Codegen with error recovery */
    CodegenResult res = {NULL, NULL};
    bool ok = true;

    /* Keep g_in_eval=true through the entire codegen + JIT execution so
     * the SIGSEGV handler can longjmp back here on JIT crashes.
     * SA_RESETHAND clears the handler on delivery, so reinstall each time. */
    signal(SIGSEGV, repl_signal_handler);
    signal(SIGBUS,  repl_signal_handler);
    g_in_eval = true;

    if (setjmp(g_repl_escape) == 0) {
        res = codegen_expr(&ctx->cg, ast);
        if (!res.value) ok = false;

        if (ok) {
            ast_free(ast);
            ast = NULL;

            if (!silent)
                emit_auto_print(ctx, res.value, res.type);

            bool ran = close_and_run(ctx);
            g_in_eval = false;
            if (ran) ctx->expr_count++;

            char next[64];
            snprintf(next, sizeof(next), "__repl_%u", ctx->expr_count);
            fresh_module(ctx, next);
            return ran;
        }
    } else {
        /* longjmp from either exit() or signal handler */
        ok = false;
    }

    g_in_eval = false;
    if (ast) { ast_free(ast); ast = NULL; }
    recover_module(ctx);
    return false;
}

/// readline completion

/* -------------------------------------------------------------------------
 * repl_completion_generator — readline tab-completion.
 *
 * FIX: The original state machine advanced `cur = cur->next` at the TOP of
 * the function, before the entry was checked.  This caused two bugs:
 *   1. The very first entry in each bucket was skipped (cur started NULL,
 *      was set to buckets[bi] inside the inner loop, then immediately
 *      advanced past it on the next call).
 *   2. When an imported symbol had a NULL or short name, strncmp crashed.
 *
 * New approach: initialize cur = buckets[0] on reset, advance AFTER match,
 * null-check e->name before comparing.
 * ------------------------------------------------------------------------- */
char *repl_completion_generator(const char *text, int state) {
    static size_t    bi;
    static EnvEntry *cur;
    static size_t    len;
    static int       ki;

    static const char *kws[] = {
        "Int","Float","Char","String","Hex","Bin","Oct","Bool",
        "define","show","if","for","quote","begin","set!","and","or","not",
        "import","cons","car","cdr","list","append","list-ref","list-length",
        "make-list","equal?","list-empty?","list-copy","append!",
        "string-length","string-ref","string-set!","make-string","show-value",
        NULL
    };

    if (!g_repl_ctx) return NULL;
    Env *env = g_repl_ctx->cg.env;
    if (!env) return NULL;

    if (!state) {
        bi  = 0;
        cur = env->buckets[0];   /* FIX: start at first bucket entry */
        len = strlen(text);
        ki  = 0;
    }

    /* Walk env hash table */
    while (bi < env->size) {
        while (cur) {
            EnvEntry *e = cur;
            cur = cur->next;     /* advance BEFORE returning so next call is safe */
            if (!e->name) continue;  /* FIX: null-check */
            if (strncmp(e->name, text, len) == 0)
                return strdup(e->name);
        }
        /* Move to next bucket */
        bi++;
        cur = (bi < env->size) ? env->buckets[bi] : NULL;
    }

    /* Walk static keyword list */
    while (kws[ki]) {
        const char *k = kws[ki++];
        if (strncmp(k, text, len) == 0) return strdup(k);
    }
    return NULL;
}

char **repl_completion(const char *text, int start, int end) {
    (void)start; (void)end;
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, repl_completion_generator);
}

void repl_run(void) {
    REPLContext ctx;
    repl_init(&ctx);
    g_repl_ctx = &ctx;
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
