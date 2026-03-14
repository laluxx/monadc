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

/* Arm the reader escape point. Usage:
 *   REPL_PARSE(var, source)  — sets var to the parsed AST or NULL on error */
#define REPL_PARSE(out, src) \
    do { \
        g_reader_escape_set = true; \
        if (setjmp(g_reader_escape) != 0) { \
            g_reader_escape_set = false; \
            (out) = NULL; \
        } else { \
            (out) = parse(src); \
            g_reader_escape_set = false; \
        } \
    } while(0)

/* -------------------------------------------------------------------------
 * Error recovery: shadow exit() so codegen errors don't kill the process.
 * Only active while g_in_eval is true.
 * ------------------------------------------------------------------------- */
static jmp_buf g_repl_escape;
static bool    g_in_eval = false;
static volatile sig_atomic_t g_interrupted = 0;

static void repl_sigint_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
    rt_interrupted = 1;
    // NO arena_reset here — the arena is still live on the call stack
    write(STDERR_FILENO, "\n", 1);
}

/* Catch SIGSEGV/SIGBUS from JIT-compiled code and longjmp back to the
 * eval loop so the REPL stays alive instead of crashing the process. */
static void repl_signal_handler(int sig) {
    /* Reinstall ourselves immediately so the next crash is also caught */
    signal(SIGSEGV, repl_signal_handler);
    signal(SIGBUS,  repl_signal_handler);
    if (g_in_eval) {
        g_in_eval = false;  /* prevent re-entrancy */
        /* Use write() — async-signal-safe, unlike fprintf */
        const char msg[] = "\nError: runtime crash (SIGSEGV) in JIT code\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        longjmp(g_repl_escape, 99);
    }
    /* Not in eval: restore default and re-raise for core dump */
    signal(sig, SIG_DFL);
    raise(sig);
}


static sigjmp_buf *g_assert_jmp_ptr = NULL;

void __monad_assert_fail(const char *label) {
    fprintf(stderr, "\x1b[31;1mAssertion failed:\x1b[0m %s\n", label);
    if (g_assert_jmp_ptr)
        longjmp(*g_assert_jmp_ptr, 1);
    abort();
}

/* -------------------------------------------------------------------------
 * Globals
 * ------------------------------------------------------------------------- */
/* Wrapper name is unique per expression to prevent MCJIT from returning
 * the cached address of the *first* compiled wrapper on every subsequent
 * LLVMGetFunctionAddress call.  Format: __repl_wrap_N */
#define WRAPPER_FMT  "__repl_wrap_%u"
static char g_wrapper_name[64] = "__repl_wrap_0";
static unsigned int g_wrapper_seq = 0;

static REPLContext *g_repl_ctx = NULL;

// Host-side table mapping variable name -> malloc'd layout heap pointer
typedef struct { char name[256]; void *ptr; } LayoutPtr;
#define MAX_LAYOUT_PTRS 1024
static LayoutPtr g_layout_ptrs[MAX_LAYOUT_PTRS];
static int       g_layout_ptr_count = 0;

static void layout_ptr_set(const char *name, void *ptr) {
    for (int i = 0; i < g_layout_ptr_count; i++) {
        if (strcmp(g_layout_ptrs[i].name, name) == 0) {
            g_layout_ptrs[i].ptr = ptr;
            return;
        }
    }
    if (g_layout_ptr_count < MAX_LAYOUT_PTRS) {
        strncpy(g_layout_ptrs[g_layout_ptr_count].name, name, 255);
        g_layout_ptrs[g_layout_ptr_count].ptr = ptr;
        g_layout_ptr_count++;
    }
}

static void *layout_ptr_get(const char *name) {
    for (int i = 0; i < g_layout_ptr_count; i++)
        if (strcmp(g_layout_ptrs[i].name, name) == 0)
            return g_layout_ptrs[i].ptr;
    return NULL;
}

// Called from JIT-compiled code to register a layout heap pointer
void __layout_ptr_set(const char *name, void *ptr) {
    layout_ptr_set(name, ptr);
}


// Called from JIT-compiled code to retrieve a layout heap pointer
void *__layout_ptr_get(const char *name) {
    void *result = layout_ptr_get(name);
    return result;
}


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

    // Runtime list
    ADD(rt_ast_to_runtime_value);
    ADD(rt_list_car);     ADD(rt_list_cdr);          ADD(rt_list_nth);
    ADD(rt_list_length);  ADD(rt_list_append_lists); ADD(rt_list_copy);
    ADD(rt_make_list);    ADD(rt_list_empty);        ADD(rt_thunk_of_value);
    ADD(rt_equal_p);      ADD(rt_list_lazy_cons);    ADD(rt_list_is_empty_list);
    ADD(rt_thunk_create); ADD(rt_force);             ADD(rt_list_range);
    ADD(rt_list_from);    ADD(rt_list_from_step);    ADD(rt_list_take);
    ADD(rt_list_drop);    ADD(rt_value_thunk);       ADD(rt_print_list_limited);
    ADD(rt_string_take);

    // Set
    ADD(rt_set_new);        ADD(rt_set_of);       ADD(rt_set_from_list);
    ADD(rt_set_from_array); ADD(rt_set_contains); ADD(rt_set_conj);
    ADD(rt_set_disj);       ADD(rt_set_conj_mut); ADD(rt_set_disj_mut);
    ADD(rt_set_get);        ADD(rt_set_count);    ADD(rt_set_seq);
    ADD(rt_value_set);      ADD(rt_unbox_set);

    ADD(__layout_ptr_set);
    ADD(__layout_ptr_get);

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
    /* assert failure handler lives in repl.c — map it explicitly */
    LLVMValueRef assert_fail = LLVMGetNamedFunction(mod, "__monad_assert_fail");
    if (assert_fail)
        LLVMAddGlobalMapping(engine, assert_fail, (void *)__monad_assert_fail);
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

static void *lookup_imported_sym(const char *name) __attribute__((unused));
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
            if (e->kind == ENV_VAR &&
                (!e->value || LLVMIsAGlobalVariable(e->value))) {

                const char *name = (e->llvm_name && e->llvm_name[0])
                                   ? e->llvm_name : e->name;

                LLVMValueRef existing = LLVMGetNamedGlobal(ctx->cg.module, name);
                if (existing) {
                    e->value = existing;
                    continue;
                }

                // Layout variables are stored as i8* globals (holding heap ptr)
                LLVMTypeRef lt;
                if (e->type && e->type->kind == TYPE_LAYOUT) {
                    lt = LLVMPointerType(LLVMInt8TypeInContext(ctx->cg.context), 0);
                } else {
                    lt = type_to_llvm(&ctx->cg, e->type);
                }
                LLVMValueRef gv = LLVMAddGlobal(ctx->cg.module, lt, name);
                LLVMSetLinkage(gv, LLVMExternalLinkage);
                e->value = gv;
            }
            else if (e->kind == ENV_FUNC && e->func_ref) {
                const char *name = (e->llvm_name && e->llvm_name[0])
                                   ? e->llvm_name : e->name;

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
    if (!ast) return false;
    if (ast->type == AST_LAYOUT) return true;
    if (ast->type == AST_TYPE_ALIAS) return true;   // ← add this
    if (ast->type != AST_LIST || ast->list.count == 0) return false;
    if (ast->list.items[0]->type != AST_SYMBOL) return false;
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


static void emit_char_annotation(REPLContext *ctx, LLVMValueRef v64) {
    LLVMValueRef pf  = get_or_declare_printf(&ctx->cg);
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->cg.builder,
                           " (0o%lo, 0x%lX, %ld)\n", "fmt_char_ann");
    LLVMValueRef args[] = {fmt, v64, v64, v64};
    LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, args, 4, "");
}

static void emit_numeric_annotation(REPLContext *ctx, LLVMValueRef val, bool show_hex) {
    LLVMValueRef pf  = get_or_declare_printf(&ctx->cg);
    LLVMTypeRef  i64 = LLVMInt64TypeInContext(ctx->cg.context);
    LLVMValueRef func = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->cg.builder));

    LLVMValueRef v = val;
    if (LLVMTypeOf(val) != i64)
        v = LLVMBuildZExt(ctx->cg.builder, val, i64, "ann_i64");

    LLVMValueRef lo  = LLVMConstInt(i64, 0,   0);
    LLVMValueRef hi  = LLVMConstInt(i64, 127, 0);
    LLVMValueRef ge  = LLVMBuildICmp(ctx->cg.builder, LLVMIntSGE, v, lo, "ge0");
    LLVMValueRef le  = LLVMBuildICmp(ctx->cg.builder, LLVMIntSLE, v, hi, "le127");
    LLVMValueRef in_range = LLVMBuildAnd(ctx->cg.builder, ge, le, "in_range");

    LLVMBasicBlockRef range_bb   = LLVMAppendBasicBlockInContext(ctx->cg.context, func, "ann_range");
    LLVMBasicBlockRef norange_bb = LLVMAppendBasicBlockInContext(ctx->cg.context, func, "ann_norange");
    LLVMBasicBlockRef cont_bb    = LLVMAppendBasicBlockInContext(ctx->cg.context, func, "ann_cont");
    LLVMBuildCondBr(ctx->cg.builder, in_range, range_bb, norange_bb);

    LLVMPositionBuilderAtEnd(ctx->cg.builder, range_bb);
    {
        LLVMValueRef c32     = LLVMConstInt(i64, 32, 0);
        LLVMValueRef is_ctrl = LLVMBuildICmp(ctx->cg.builder, LLVMIntSLT, v, c32, "is_ctrl");
        LLVMBasicBlockRef ctrl_bb  = LLVMAppendBasicBlockInContext(ctx->cg.context, func, "ann_ctrl");
        LLVMBasicBlockRef nctrl_bb = LLVMAppendBasicBlockInContext(ctx->cg.context, func, "ann_nctrl");
        LLVMBuildCondBr(ctx->cg.builder, is_ctrl, ctrl_bb, nctrl_bb);

        LLVMPositionBuilderAtEnd(ctx->cg.builder, ctrl_bb);
        {
            static const char *ctrl_names[32] = {
                "\\0", "\\x01","\\x02","\\x03","\\x04","\\x05","\\x06","\\a",
                "\\b", "\\t",  "\\n",  "\\v",  "\\f",  "\\r",  "\\x0E","\\x0F",
                "\\x10","\\x11","\\x12","\\x13","\\x14","\\x15","\\x16","\\x17",
                "\\x18","\\x19","\\x1A","\\x1B","\\x1C","\\x1D","\\x1E","\\x1F"
            };
            const char *fmt_str = show_hex ? " (0o%lo, 0x%lx, '%s')\n"
                                           : " (0o%lo, %ld, '%s')\n";
            LLVMValueRef fmt_with = LLVMBuildGlobalStringPtr(ctx->cg.builder, fmt_str, "fmt_ctrl");
            LLVMBasicBlockRef ctrl_cont = LLVMAppendBasicBlockInContext(ctx->cg.context, func, "ctrl_cont");
            LLVMValueRef sw = LLVMBuildSwitch(ctx->cg.builder, v, ctrl_cont, 32);
            for (int i = 0; i < 32; i++) {
                LLVMBasicBlockRef bb = LLVMAppendBasicBlockInContext(ctx->cg.context, func, "ctrl_case");
                LLVMAddCase(sw, LLVMConstInt(i64, i, 0), bb);
                LLVMPositionBuilderAtEnd(ctx->cg.builder, bb);
                LLVMValueRef name = LLVMBuildGlobalStringPtr(ctx->cg.builder, ctrl_names[i], "cname");
                LLVMValueRef args[] = {fmt_with, v, v, name};
                LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, args, 4, "");
                LLVMBuildBr(ctx->cg.builder, ctrl_cont);
            }
            LLVMPositionBuilderAtEnd(ctx->cg.builder, ctrl_cont);
            LLVMBuildBr(ctx->cg.builder, cont_bb);
        }

        LLVMPositionBuilderAtEnd(ctx->cg.builder, nctrl_bb);
        {
            LLVMValueRef c127   = LLVMConstInt(i64, 127, 0);
            LLVMValueRef is_del = LLVMBuildICmp(ctx->cg.builder, LLVMIntEQ, v, c127, "is_del");
            LLVMBasicBlockRef del_bb   = LLVMAppendBasicBlockInContext(ctx->cg.context, func, "ann_del");
            LLVMBasicBlockRef print_bb = LLVMAppendBasicBlockInContext(ctx->cg.context, func, "ann_print");
            LLVMBuildCondBr(ctx->cg.builder, is_del, del_bb, print_bb);

            LLVMPositionBuilderAtEnd(ctx->cg.builder, del_bb);
            {
                const char *fmt_str = show_hex ? " (0o%lo, 0x%lx, 'delete')\n"
                                               : " (0o%lo, %ld, 'delete')\n";
                LLVMValueRef fmt  = LLVMBuildGlobalStringPtr(ctx->cg.builder, fmt_str, "fmt_del");
                LLVMValueRef args[] = {fmt, v, v};
                LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, args, 3, "");
            }
            LLVMBuildBr(ctx->cg.builder, cont_bb);

            LLVMPositionBuilderAtEnd(ctx->cg.builder, print_bb);
            {
                const char *fmt_str = show_hex ? " (0o%lo, 0x%lx, '%c')\n"
                                               : " (0o%lo, %ld, '%c')\n";
                LLVMValueRef fmt  = LLVMBuildGlobalStringPtr(ctx->cg.builder, fmt_str, "fmt_print");
                LLVMValueRef args[] = {fmt, v, v, v};
                LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, args, 4, "");
            }
            LLVMBuildBr(ctx->cg.builder, cont_bb);
        }
    }

    LLVMPositionBuilderAtEnd(ctx->cg.builder, norange_bb);
    {
        const char *fmt_str = show_hex ? " (0o%lo, 0x%lx)\n"
                                       : " (0o%lo, %ld)\n";
        LLVMValueRef fmt  = LLVMBuildGlobalStringPtr(ctx->cg.builder, fmt_str, "fmt_norange");
        LLVMValueRef args[] = {fmt, v, v};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, args, 3, "");
    }
    LLVMBuildBr(ctx->cg.builder, cont_bb);

    LLVMPositionBuilderAtEnd(ctx->cg.builder, cont_bb);
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
static void emit_auto_print(REPLContext *ctx, LLVMValueRef val, Type *t, bool list_is_rv) {
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

    case TYPE_INT: {
        LLVMValueRef a[] = {get_fmt_int(&ctx->cg), val};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
        emit_numeric_annotation(ctx, val, true);
        break; }
    case TYPE_HEX: {
        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->cg.builder, "0x%lX\n", "fmt_hex");
        LLVMValueRef a[] = {fmt, val};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
        emit_numeric_annotation(ctx, val, false);
        break; }
    case TYPE_BIN: {
        LLVMValueRef bin_fn = get_or_declare_print_binary(&ctx->cg);
        LLVMValueRef a[] = {val};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(bin_fn), bin_fn, a, 1, "");
        emit_numeric_annotation(ctx, val, true);
        break; }
    case TYPE_OCT: {
        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->cg.builder, "0o%lo\n", "fmt_oct");
        LLVMValueRef a[] = {fmt, val};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
        emit_numeric_annotation(ctx, val, true);
        break; }

    case TYPE_CHAR: {
        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->cg.builder, "'%c'\n", "fmt_char_quoted");
        LLVMValueRef a[] = {fmt, val};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
        LLVMTypeRef  i64 = LLVMInt64TypeInContext(ctx->cg.context);
        LLVMValueRef v64 = LLVMBuildZExt(ctx->cg.builder, val, i64, "char_i64");
        emit_char_annotation(ctx, v64);
        break; }

    case TYPE_STRING: {
        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(ctx->cg.builder, "\"%s\"\n", "fmt_str_quoted");
        LLVMValueRef a[] = {fmt, val};
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
        /* Two cases:
         *  list_is_rv=true  → val is RuntimeValue* (imported fn like cadr)
         *                     → call rt_print_value_newline directly
         *  list_is_rv=false → val is RuntimeList* (quote, (list), cons…)
         *                     → box via rt_value_list then print              */
        if (list_is_rv) {
            EMIT_RV_PRINT(val);
        } else {
            LLVMValueRef box_fn  = get_rt_value_list(&ctx->cg);
            LLVMTypeRef  ft2     = LLVMFunctionType(ptr, &ptr, 1, 0);
            LLVMValueRef bargs[] = {val};
            LLVMValueRef boxed   = LLVMBuildCall2(ctx->cg.builder, ft2,
                                                  box_fn, bargs, 1, "boxed_list");
            EMIT_RV_PRINT(boxed);
        }
        break;

    case TYPE_KEYWORD:
        /* Keywords are stored as bare char* (e.g. ":foo") by codegen — same
         * representation as TYPE_STRING.  Use printf, not rt_print_value_newline. */
        {
            LLVMValueRef a[] = {get_fmt_str(&ctx->cg), val};
            LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
        }
        break;

    case TYPE_SET:
    case TYPE_RATIO:
    case TYPE_SYMBOL:
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

    case TYPE_LAYOUT: {
        char struct_name[256];
        snprintf(struct_name, sizeof(struct_name), "layout.%s", t->layout_name);
        LLVMTypeRef struct_llvm = LLVMGetTypeByName2(ctx->cg.context, struct_name);
        if (!struct_llvm) break;

        int max_name_len = 0;
        for (int i = 0; i < t->layout_field_count; i++) {
            int n = (int)strlen(t->layout_fields[i].name);
            if (n > max_name_len) max_name_len = n;
        }

        // We can't know value widths at IR-gen time, so we use a two-pass
        // approach: emit a sprintf into a temp buffer, measure with strlen,
        // track max across fields, then print aligned.
        // Since this is complex in IR, we use a simpler approach:
        // print each field into a host-side buffer at JIT runtime via a
        // helper function. Instead, we use printf with %*#g and let the
        // caller figure out width by printing name+value into fixed buf.
        //
        // Simplest practical approach: use snprintf at codegen time on
        // constant values — but values aren't constant. So: emit the
        // struct name line, then for each field emit "[name value]" with
        // the value printed via printf, using max_name_len for name padding
        // and a runtime-computed value width via two printf calls (one to
        // a buffer, one to stdout). This is complex.
        //
        // Practical compromise: use a fixed value column of max(12, longest_name+2)
        // but make the ] position dynamic by printing into a stack buffer
        // and then printing that buffer.

        // Print "(StructName \n"

        char open_buf[128];
        snprintf(open_buf, sizeof(open_buf), "(%s", t->layout_name);
        LLVMValueRef open_s = LLVMBuildGlobalStringPtr(ctx->cg.builder, open_buf, "lay_open");
        LLVMValueRef oa[] = {open_s};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, oa, 1, "");

        int indent = (int)strlen(t->layout_name) + 2;

        // Declare snprintf
        LLVMValueRef snprintf_fn = LLVMGetNamedFunction(ctx->cg.module, "snprintf");
        if (!snprintf_fn) {
            LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->cg.context);
            LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->cg.context), 0);
            LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->cg.context);
            LLVMTypeRef params[] = {ptr, i64, ptr};
            LLVMTypeRef ft = LLVMFunctionType(i32, params, 3, true);
            snprintf_fn = LLVMAddFunction(ctx->cg.module, "snprintf", ft);
            LLVMSetLinkage(snprintf_fn, LLVMExternalLinkage);
        }
        LLVMValueRef strlen_fn = LLVMGetNamedFunction(ctx->cg.module, "strlen");
        if (!strlen_fn) {
            LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->cg.context), 0);
            LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->cg.context);
            LLVMTypeRef ft = LLVMFunctionType(i64, &ptr, 1, 0);
            strlen_fn = LLVMAddFunction(ctx->cg.module, "strlen", ft);
            LLVMSetLinkage(strlen_fn, LLVMExternalLinkage);
        }

        // For each field: snprintf value into a 64-byte stack buffer,
        // then print "[name<pad> <valbuf><pad>]" aligned by value length.
        // We track max_val_len across fields using alloca'd variables.
        LLVMTypeRef  i32 = LLVMInt32TypeInContext(ctx->cg.context);
        LLVMTypeRef  i64 = LLVMInt64TypeInContext(ctx->cg.context);
        LLVMTypeRef  ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->cg.context), 0);

        // Allocate one 64-byte buffer per field for value strings
        // and store their lengths
        LLVMValueRef *val_bufs   = malloc(sizeof(LLVMValueRef) * t->layout_field_count);
        LLVMValueRef *val_lens   = malloc(sizeof(LLVMValueRef) * t->layout_field_count);

        LLVMValueRef max_len_ptr = LLVMBuildAlloca(ctx->cg.builder, i32, "max_val_len");
        LLVMBuildStore(ctx->cg.builder,
                       LLVMConstInt(i32, 1, 0), max_len_ptr);

        LLVMValueRef func = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ctx->cg.builder));

        for (int i = 0; i < t->layout_field_count; i++) {
            val_bufs[i] = LLVMBuildArrayAlloca(ctx->cg.builder,
                              LLVMInt8TypeInContext(ctx->cg.context),
                              LLVMConstInt(i32, 64, 0), "vbuf");

            LLVMValueRef zero_idx = LLVMConstInt(i64, 0, 0);
            LLVMValueRef fidx_v   = LLVMConstInt(i32, i, 0);
            LLVMValueRef idxs[]   = {LLVMConstInt(i32, 0, 0), fidx_v};
            LLVMValueRef gep      = LLVMBuildGEP2(ctx->cg.builder, struct_llvm, val, idxs, 2, "fp");
            Type *ft_field        = t->layout_fields[i].type;
            LLVMTypeRef ft_llvm   = type_to_llvm(&ctx->cg, ft_field);
            LLVMValueRef fval     = LLVMBuildLoad2(ctx->cg.builder, ft_llvm, gep, "fv");

            // snprintf value into buffer
            LLVMValueRef buf_size = LLVMConstInt(i64, 64, 0);
            if (ft_field->kind == TYPE_FLOAT) {
                LLVMValueRef fmt_s = LLVMBuildGlobalStringPtr(ctx->cg.builder, "%g", "fmtf");
                LLVMTypeRef  sn_params[] = {ptr, i64, ptr, LLVMDoubleTypeInContext(ctx->cg.context)};
                LLVMTypeRef  sn_ft = LLVMFunctionType(i32, sn_params, 4, false);
                LLVMValueRef sargs[] = {val_bufs[i], buf_size, fmt_s, fval};
                LLVMBuildCall2(ctx->cg.builder, sn_ft, snprintf_fn, sargs, 4, "");
                // If no '.' or 'e' in result, append ".0" to show it's a float
                LLVMValueRef strchr_fn = LLVMGetNamedFunction(ctx->cg.module, "strchr");
                if (!strchr_fn) {
                    LLVMTypeRef sc_params[] = {ptr, i32};
                    LLVMTypeRef sc_ft = LLVMFunctionType(ptr, sc_params, 2, false);
                    strchr_fn = LLVMAddFunction(ctx->cg.module, "strchr", sc_ft);
                    LLVMSetLinkage(strchr_fn, LLVMExternalLinkage);
                }
                LLVMValueRef strcat_fn = LLVMGetNamedFunction(ctx->cg.module, "strcat");
                if (!strcat_fn) {
                    LLVMTypeRef sc_params[] = {ptr, ptr};
                    LLVMTypeRef sc_ft = LLVMFunctionType(ptr, sc_params, 2, false);
                    strcat_fn = LLVMAddFunction(ctx->cg.module, "strcat", sc_ft);
                    LLVMSetLinkage(strcat_fn, LLVMExternalLinkage);
                }
                // has_dot = strchr(buf, '.') != NULL
                LLVMValueRef dot_char = LLVMConstInt(i32, '.', 0);
                LLVMValueRef sc_args1[] = {val_bufs[i], dot_char};
                LLVMTypeRef  sc_ft1_p[] = {ptr, i32};
                LLVMValueRef has_dot = LLVMBuildCall2(ctx->cg.builder,
                    LLVMFunctionType(ptr, sc_ft1_p, 2, false),
                    strchr_fn, sc_args1, 2, "has_dot");
                // has_e = strchr(buf, 'e') != NULL
                LLVMValueRef e_char = LLVMConstInt(i32, 'e', 0);
                LLVMValueRef sc_args2[] = {val_bufs[i], e_char};
                LLVMValueRef has_e = LLVMBuildCall2(ctx->cg.builder,
                    LLVMFunctionType(ptr, sc_ft1_p, 2, false),
                    strchr_fn, sc_args2, 2, "has_e");
                // null ptr = 0
                LLVMValueRef null_ptr = LLVMConstPointerNull(ptr);
                LLVMValueRef no_dot = LLVMBuildICmp(ctx->cg.builder, LLVMIntEQ, has_dot, null_ptr, "no_dot");
                LLVMValueRef no_e   = LLVMBuildICmp(ctx->cg.builder, LLVMIntEQ, has_e,   null_ptr, "no_e");
                LLVMValueRef needs_dot = LLVMBuildAnd(ctx->cg.builder, no_dot, no_e, "needs_dot");
                LLVMBasicBlockRef append_bb = LLVMAppendBasicBlockInContext(ctx->cg.context, func, "append_dot");
                LLVMBasicBlockRef skip_bb   = LLVMAppendBasicBlockInContext(ctx->cg.context, func, "skip_dot");
                LLVMBuildCondBr(ctx->cg.builder, needs_dot, append_bb, skip_bb);
                LLVMPositionBuilderAtEnd(ctx->cg.builder, append_bb);
                LLVMValueRef dot_str = LLVMBuildGlobalStringPtr(ctx->cg.builder, ".0", "dot_str");
                LLVMValueRef cat_args[] = {val_bufs[i], dot_str};
                LLVMTypeRef  cat_ft_p[] = {ptr, ptr};
                LLVMBuildCall2(ctx->cg.builder,
                    LLVMFunctionType(ptr, cat_ft_p, 2, false),
                    strcat_fn, cat_args, 2, "");
                LLVMBuildBr(ctx->cg.builder, skip_bb);
                LLVMPositionBuilderAtEnd(ctx->cg.builder, skip_bb);
            } else if (ft_field->kind == TYPE_CHAR) {
                LLVMValueRef fmt_s = LLVMBuildGlobalStringPtr(ctx->cg.builder, "%c", "fmtc");
                LLVMValueRef sargs[] = {val_bufs[i], buf_size, fmt_s, fval};
                LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(snprintf_fn),
                               snprintf_fn, sargs, 4, "");
            } else if (ft_field->kind == TYPE_STRING) {
                LLVMValueRef fmt_s = LLVMBuildGlobalStringPtr(ctx->cg.builder, "\"%s\"", "fmts");
                LLVMValueRef sargs[] = {val_bufs[i], buf_size, fmt_s, fval};
                LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(snprintf_fn),
                               snprintf_fn, sargs, 4, "");
            } else if (ft_field->kind == TYPE_ARR) {
                // Print array fields as "[...]"
                LLVMValueRef fmt_s = LLVMBuildGlobalStringPtr(ctx->cg.builder, "[array]", "fmtarr");
                LLVMTypeRef  sn_p[] = {ptr, i64, ptr};
                LLVMBuildCall2(ctx->cg.builder,
                    LLVMFunctionType(i32, sn_p, 3, false),
                    snprintf_fn,
                    (LLVMValueRef[]){val_bufs[i], buf_size, fmt_s}, 3, "");
            } else {
                LLVMTypeRef i64t = LLVMInt64TypeInContext(ctx->cg.context);
                if (LLVMTypeOf(fval) != i64t)
                    fval = LLVMBuildZExt(ctx->cg.builder, fval, i64t, "wi");
                LLVMValueRef fmt_s = LLVMBuildGlobalStringPtr(ctx->cg.builder, "%ld", "fmti");
                LLVMTypeRef  sn_p[] = {ptr, i64, ptr, i64t};
                LLVMBuildCall2(ctx->cg.builder,
                    LLVMFunctionType(i32, sn_p, 4, false),
                    snprintf_fn,
                    (LLVMValueRef[]){val_bufs[i], buf_size, fmt_s, fval}, 4, "");
            }

            // strlen of the buffer
            LLVMValueRef slen_args[] = {val_bufs[i]};
            LLVMValueRef slen = LLVMBuildCall2(ctx->cg.builder,
                                    LLVMGlobalGetValueType(strlen_fn),
                                    strlen_fn, slen_args, 1, "slen");
            LLVMValueRef slen32 = LLVMBuildTrunc(ctx->cg.builder, slen, i32, "slen32");
            val_lens[i] = slen32;

            // update max_len_ptr if slen32 > current max
            LLVMValueRef cur_max = LLVMBuildLoad2(ctx->cg.builder, i32, max_len_ptr, "cur_max");
            LLVMValueRef is_bigger = LLVMBuildICmp(ctx->cg.builder, LLVMIntSGT,
                                                   slen32, cur_max, "is_bigger");
            LLVMBasicBlockRef update_bb = LLVMAppendBasicBlockInContext(ctx->cg.context, func, "upd");
            LLVMBasicBlockRef cont_bb   = LLVMAppendBasicBlockInContext(ctx->cg.context, func, "cont");
            LLVMBuildCondBr(ctx->cg.builder, is_bigger, update_bb, cont_bb);
            LLVMPositionBuilderAtEnd(ctx->cg.builder, update_bb);
            LLVMBuildStore(ctx->cg.builder, slen32, max_len_ptr);
            LLVMBuildBr(ctx->cg.builder, cont_bb);
            LLVMPositionBuilderAtEnd(ctx->cg.builder, cont_bb);
        }

        // Now print each field using max_val_len for alignment
        LLVMValueRef max_len = LLVMBuildLoad2(ctx->cg.builder, i32, max_len_ptr, "max_len");

        for (int i = 0; i < t->layout_field_count; i++) {
            // prefix: indent + "[" + name padded to max_name_len + " "
            char prefix_buf[256];
            if (i == 0) {
                snprintf(prefix_buf, sizeof(prefix_buf), " [%-*s ",
                         max_name_len, t->layout_fields[i].name);
            } else {
                snprintf(prefix_buf, sizeof(prefix_buf), "%*s[%-*s ",
                         indent, "", max_name_len, t->layout_fields[i].name);
            }

            LLVMValueRef pre_s = LLVMBuildGlobalStringPtr(ctx->cg.builder, prefix_buf, "lay_pre");
            LLVMValueRef pa[] = {pre_s};
            LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, pa, 1, "");

            // print value from buffer
            LLVMValueRef fmt_s = LLVMBuildGlobalStringPtr(ctx->cg.builder, "%-*s", "fmtv");
            LLVMValueRef vargs[] = {fmt_s, max_len, val_bufs[i]};
            LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, vargs, 3, "");

            // closing bracket + newline
            const char *suffix = (i < t->layout_field_count - 1) ? "]\n" : "])\n";
            LLVMValueRef suf_s = LLVMBuildGlobalStringPtr(ctx->cg.builder, suffix, "lay_suf");
            LLVMValueRef sa[] = {suf_s};
            LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, sa, 1, "");
        }

        free(val_bufs);
        free(val_lens);
        break;
    }

    default: break;
    }

#undef EMIT_RV_PRINT
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

    if (getenv("REPL_DUMP_IR")) {
        char *ir = LLVMPrintModuleToString(ctx->cg.module);
        fprintf(stderr, "=== IR ===\n%s=== END IR ===\n", ir);
        LLVMDisposeMessage(ir);
    }

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

    LLVMModuleRef mod = ctx->cg.module;
    LLVMAddModule(ctx->engine, mod);
    ctx->cg.module = NULL;

    map_runtime_in_module(ctx->engine, mod);
    map_imported_in_module(ctx->engine, mod);

    /* Map env globals so the JIT can resolve cross-module references
     * like @counter defined in a previous module.                    */
    Env *env = ctx->cg.env;
    for (size_t bi = 0; bi < env->size; bi++) {
        for (EnvEntry *e = env->buckets[bi]; e; e = e->next) {
            if (e->kind != ENV_VAR || !e->value) continue;
            if (LLVMGetValueKind(e->value) != LLVMGlobalVariableValueKind) continue;
            const char *gname = (e->llvm_name && e->llvm_name[0])
                                ? e->llvm_name : e->name;
            LLVMValueRef in_mod = LLVMGetNamedGlobal(mod, gname);
            if (!in_mod) continue;
            uint64_t addr = LLVMGetGlobalValueAddress(ctx->engine, gname);
            if (addr)
                LLVMAddGlobalMapping(ctx->engine, in_mod, (void*)(uintptr_t)addr);
        }
    }

    void (*fn)(void) = (void(*)(void))
        LLVMGetFunctionAddress(ctx->engine, g_wrapper_name);
    if (!fn) {
        fprintf(stderr, "Error: JIT could not find %s\n", g_wrapper_name);
        return false;
    }

    fn();
    arena_reset(&g_eval_arena);
    return true;
}

// Discard a broken module and recover a clean state
static void recover_module(REPLContext *ctx) {
    if (ctx->cg.module) {
        LLVMDisposeModule(ctx->cg.module);
        ctx->cg.module = NULL;
    }
    if (ctx->cg.builder) {
        LLVMDisposeBuilder(ctx->cg.builder);
        ctx->cg.builder = NULL;
    }
}

/// Import support

// Compile the module to a shared library, dlopen it, run its init function,
// then use nm to discover exported symbols and inject them into the env.

static bool file_exists_r(const char *p) { return access(p, F_OK) == 0; }
static time_t file_mtime_r(const char *p) {
    struct stat st; return stat(p, &st) == 0 ? st.st_mtime : 0;
}

static bool module_already_loaded(REPLContext *ctx, const char *mod_name) {
    Env *env = ctx->cg.env;
    for (size_t bi = 0; bi < env->size; bi++)
        for (EnvEntry *e = env->buckets[bi]; e; e = e->next)
            if (e->module_name && strcmp(e->module_name, mod_name) == 0)
                return true;
    return false;
}


/* -------------------------------------------------------------------------
 * harvest_source_for_module
 *
 * Walk every top-level form in src_buf (the content of the module source
 * file at src_path).  For each (define name ...) found — whether at the
 * top level or nested one level inside a (module Name ...) wrapper — patch
 * the matching env entry with source_text and docstring.
 *
 * The cursor is advanced with a string-aware depth-walker so that string
 * values containing parentheses don't confuse the parser.
 * ------------------------------------------------------------------------- */
static void harvest_patch_entry(Env *env, const char *mod_name,
                                const char *fn_name,
                                const char *form_src, size_t form_len,
                                AST *form)
{
    char qn[512];
    snprintf(qn, sizeof(qn), "%s.%s", mod_name, fn_name);

    /* Patch both the qualified entry and the bare-name entry — they are
     * separate EnvEntry structs inserted by declare_externals, and (code)
     * may look up either one depending on how the user wrote the name.   */
    EnvEntry *entries[2] = {
        env_lookup(env, qn),
        env_lookup(env, fn_name),
    };

    for (int i = 0; i < 2; i++) {
        EnvEntry *ent = entries[i];
        if (!ent) continue;
        if (!ent->module_name || strcmp(ent->module_name, mod_name) != 0) continue;

        if (!ent->source_text && form_len > 0)
            ent->source_text = strndup(form_src, form_len);

        if (!ent->docstring && form->list.count >= 3) {
            AST *lam = form->list.items[2];
            if (lam->type == AST_LAMBDA &&
                lam->lambda.docstring && lam->lambda.docstring[0])
                ent->docstring = strdup(lam->lambda.docstring);
        }
    }
}

/* Advance *pp past one complete top-level form, respecting strings.
 * Returns a pointer to the start of the form (after leading whitespace),
 * or NULL if no form is found. Sets *len to the byte length of the form. */
static const char *advance_form(const char **pp, size_t *len)
{
    const char *p = *pp;

    /* Skip whitespace and line comments */
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ';') { while (*p && *p != '\n') p++; continue; }
        break;
    }
    if (!*p) { *pp = p; return NULL; }

    const char *start = p;

    if (*p == '(') {
        int depth = 0;
        bool in_str = false;
        while (*p) {
            if (in_str) {
                if (*p == '\\') { p++; if (*p) p++; continue; }
                if (*p == '"')  { in_str = false; p++; continue; }
                p++;
            } else {
                if (*p == '"')  { in_str = true;  p++; continue; }
                if (*p == '(')  { depth++; p++; continue; }
                if (*p == ')') {
                    depth--;
                    p++;
                    if (depth == 0) break;
                    continue;
                }
                p++;
            }
        }
    } else {
        /* Atom */
        while (*p && !isspace((unsigned char)*p)) p++;
    }

    *len = (size_t)(p - start);
    *pp  = p;
    return start;
}

/* Walk one parsed AST node: if it's a (define ...), patch the entry.
 * If it's a (module Name ...) or (begin ...), recurse into children. */
static void harvest_one_form(Env *env, const char *mod_name,
                             AST *form,
                             const char *form_src, size_t form_len)
{
    if (!form || form->type != AST_LIST || form->list.count < 1) return;
    AST *head = form->list.items[0];
    if (!head || head->type != AST_SYMBOL) return;

    if (strcmp(head->symbol, "define") == 0 && form->list.count >= 2) {
        AST *name_node = form->list.items[1];
        const char *fn_name = NULL;
        if (name_node->type == AST_SYMBOL)
            fn_name = name_node->symbol;
        else if (name_node->type == AST_LIST &&
                 name_node->list.count > 0 &&
                 name_node->list.items[0]->type == AST_SYMBOL)
            fn_name = name_node->list.items[0]->symbol;
        if (fn_name)
            harvest_patch_entry(env, mod_name, fn_name,
                                form_src, form_len, form);
        return;
    }

    /* (module Name body...) or (begin body...) — recurse into children */
    if ((strcmp(head->symbol, "module") == 0 ||
         strcmp(head->symbol, "begin")  == 0) &&
        form->list.count >= 2)
    {
        /* We don't have per-child source offsets here, so we re-parse the
         * form_src to find each child's source range.  form_src is the
         * text of the outer (module ...) form.  We skip past "(module Name"
         * and then walk child forms with advance_form. */
        const char *p = form_src;
        /* Skip the opening ( and the keyword */
        if (*p == '(') p++;
        while (*p && !isspace((unsigned char)*p)) p++; /* skip "module"/"begin" */
        /* For (module Name ...), skip the Name token too */
        if (strcmp(head->symbol, "module") == 0) {
            while (*p && isspace((unsigned char)*p)) p++;
            while (*p && !isspace((unsigned char)*p) && *p != ')') p++;
        }
        /* Now p is at the start of the child forms */
        while (*p) {
            size_t child_len = 0;
            const char *child_src = advance_form(&p, &child_len);
            if (!child_src) break;
            if (*child_src != '(') continue; /* skip atoms */
            /* We need a parsed AST for the child to check for define/lambda */
            char *child_copy = strndup(child_src, child_len);
            if (!child_copy) continue;
            parser_set_context("<module>", child_copy);
            AST *child_ast = NULL;
            REPL_PARSE(child_ast, child_copy);
            if (child_ast) {
                harvest_one_form(env, mod_name, child_ast,
                                 child_src, child_len);
                ast_free(child_ast);
            }
            free(child_copy);
        }
    }
}

static void harvest_source_for_module(Env *env, const char *mod_name,
                                      const char *src_path,
                                      const char *src_buf)
{
    const char *p = src_buf;
    while (*p) {
        size_t form_len = 0;
        const char *form_src = advance_form(&p, &form_len);
        if (!form_src) break;
        if (*form_src != '(') continue;

        char *form_copy = strndup(form_src, form_len);
        if (!form_copy) continue;

        parser_set_context(src_path, form_copy);
        AST *form = NULL;
        REPL_PARSE(form, form_copy);
        if (form) {
            harvest_one_form(env, mod_name, form, form_src, form_len);
            ast_free(form);
        }
        free(form_copy);
    }
}

static bool handle_import(REPLContext *ctx, AST *ast) {
    if (ast->list.count < 2 || ast->list.items[1]->type != AST_SYMBOL) {
        fprintf(stderr, "Error: import requires a module name symbol\n");
        return false;
    }
    ImportDecl *imp = parse_import_decl(ast);
    if (!imp) { fprintf(stderr, "Error: invalid import\n"); return false; }

    const char *mod_name = imp->module_name;

    if (module_already_loaded(ctx, mod_name)) {
        printf("Module '%s' already loaded.\n", mod_name);
        return true;
    }

    /* ------------------------------------------------------------------ */
    /* Step 1: Compile the module (populates env with typed exports)       */
    /* ------------------------------------------------------------------ */
    if (!repl_compile_module(&ctx->cg, imp)) {
        fprintf(stderr, "Error: failed to compile module '%s'\n", mod_name);
        return false;
    }

    /* ------------------------------------------------------------------ */
    /* Step 2: Build .so from the exact .o files the compiler registered.  */
    /*                                                                     */
    /* repl_get_compiled_obj_paths() returns every .o that compile_one()   */
    /* produced, in order, with deduplication already handled by the       */
    /* registry.  No find glob, no path exclusions, no duplicates.         */
    /* ------------------------------------------------------------------ */
    char so_path[512];
    snprintf(so_path, sizeof(so_path), "/tmp/__mrepl_%s.so", mod_name);
    if (file_exists_r(so_path)) remove(so_path);

    {
        const char **all_objs = repl_get_compiled_obj_paths();

        char cmd[16384];
        int w = snprintf(cmd, sizeof(cmd), "gcc -shared -fPIC -o %s", so_path);
        for (int i = 0; all_objs[i]; i++)
            w += snprintf(cmd + w, sizeof(cmd) - w, " %s", all_objs[i]);
        w += snprintf(cmd + w, sizeof(cmd) - w,
                      " -Wl,--unresolved-symbols=ignore-all -lm 2>&1");
        free(all_objs);

        if (system(cmd) != 0) {
            fprintf(stderr, "Error: failed to link .so for '%s'\n", mod_name);
            return false;
        }
    }

    /* ------------------------------------------------------------------ */
    /* Step 3: dlopen — make symbols live in the host process              */
    /* ------------------------------------------------------------------ */
    { void *self = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL); (void)self; }
    void *handle = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        fprintf(stderr, "Error: dlopen %s: %s\n", so_path, dlerror());
        return false;
    }

    /* Run module init so top-level stores execute */
    char init_name[256];
    snprintf(init_name, sizeof(init_name), "__init_%s", mod_name);
    for (char *p = init_name + 8; *p; p++) if (*p == '.') *p = '_';
    void (*init_fn)(void) = (void(*)(void))dlsym(handle, init_name);
    if (init_fn) init_fn();

    /* ------------------------------------------------------------------ */
    /* Step 4: Register symbol addresses from the .so                      */
    /* ------------------------------------------------------------------ */
    char prefix[256];
    size_t ml = strlen(mod_name);
    for (size_t i = 0; i < ml; i++)
        prefix[i] = (mod_name[i] == '.') ? '_' : mod_name[i];
    prefix[ml] = prefix[ml+1] = '_';
    prefix[ml+2] = '\0';
    size_t prefix_len = ml + 2;

    char nm_cmd[512];
    snprintf(nm_cmd, sizeof(nm_cmd), "nm -D --defined-only %s 2>/dev/null", so_path);
    FILE *nm = popen(nm_cmd, "r");
    int count = 0;
    if (nm) {
        char line[512];
        while (fgets(line, sizeof(line), nm)) {
            char addr_s[32], tc[4], sym[256];
            if (sscanf(line, "%31s %3s %255s", addr_s, tc, sym) != 3) continue;
            if (tc[0] != 'T' && tc[0] != 'D' && tc[0] != 'B') continue;
            if (strncmp(sym, prefix, prefix_len) != 0) continue;
            void *addr = dlsym(handle, sym);
            if (!addr) continue;
            register_imported_sym(sym, addr);
            count++;
        }
        pclose(nm);
    }

    /* ------------------------------------------------------------------ */
    /* Step 5: Harvest source_text + docstrings from the module source.   */
    /*                                                                    */
    /* We re-open the source file and walk every top-level form.  For     */
    /* each (define name ...) we find, we look up the env entry and patch */
    /* source_text (the pretty-printed source) and docstring (if the body */
    /* is a lambda with a docstring).  This makes (code fn) and (doc fn)  */
    /* work for imported symbols.                                         */
    /*                                                                    */
    /* The cursor is advanced with a paren-aware walker that respects     */
    /* string literals, so forms whose values contain ( or ) are handled  */
    /* correctly.                                                         */
    /* ------------------------------------------------------------------ */
    {
        char *src_path = module_name_to_path(mod_name);
        FILE *sf = src_path ? fopen(src_path, "r") : NULL;
        if (sf) {
            fseek(sf, 0, SEEK_END);
            long fsz = ftell(sf);
            rewind(sf);
            char *src_buf = malloc(fsz + 1);
            if (src_buf && fsz > 0) {
                fread(src_buf, 1, fsz, sf);
                src_buf[fsz] = '\0';
                fclose(sf);
                harvest_source_for_module(ctx->cg.env, mod_name,
                                          src_path, src_buf);
                free(src_buf);
            } else {
                if (src_buf) free(src_buf);
                fclose(sf);
            }
        }
        free(src_path);
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

    /* Install signal handlers so JIT crashes don't kill the process.
     * No SA_RESETHAND — we want the handler to persist across crashes. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = repl_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* persistent, no SA_RESETHAND */
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);

    struct sigaction sa_int;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = repl_sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    sigaction(SIGINT, &sa_int, NULL);


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
    ctx->cg.error_jmp_set = false;
    memset(ctx->cg.error_msg, 0, sizeof(ctx->cg.error_msg));
    ctx->cg.current_function_name = NULL;

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
    arena_init(&g_eval_arena, 4 * 1024 * 1024);
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

int cmp_entry(const void *a, const void *b) {
    EnvEntry *ea = *(EnvEntry **)a;
    EnvEntry *eb = *(EnvEntry **)b;
    if (ea->kind == ENV_BUILTIN && eb->kind != ENV_BUILTIN) return  1;
    if (ea->kind != ENV_BUILTIN && eb->kind == ENV_BUILTIN) return -1;
    int ma = ea->module_name ? 1 : 0;
    int mb = eb->module_name ? 1 : 0;
    if (ma != mb) return ma - mb;
    if (ea->module_name && eb->module_name) {
        int mc = strcmp(ea->module_name, eb->module_name);
        if (mc) return mc;
    }
    return strcmp(ea->name, eb->name);
}

/* Strip line comments (;...\n) and blank lines from src into a fresh
 * malloc'd buffer.  Preserves newlines so line numbers stay roughly
 * correct.  Caller must free() the result. */
static char *strip_comments(const char *src) {
    size_t len = strlen(src);
    char  *out = malloc(len + 1);
    if (!out) return NULL;

    const char *r = src;
    char       *w = out;
    bool in_str   = false;

    while (*r) {
        if (in_str) {
            if (*r == '\\' && *(r+1)) { *w++ = *r++; *w++ = *r++; continue; }
            if (*r == '"')  in_str = false;
            *w++ = *r++;
        } else {
            if (*r == '"')  { in_str = true; *w++ = *r++; }
            else if (*r == ';') {
                /* skip to end of line, emit the newline to preserve line count */
                while (*r && *r != '\n') r++;
                if (*r == '\n') *w++ = *r++;
            } else {
                *w++ = *r++;
            }
        }
    }
    *w = '\0';
    return out;
}

bool repl_eval_line(REPLContext *ctx, const char *line) {
    if (!line) return true;
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return true;

    // ,command dispatch
    if (*p == ',') {
        const char *cmd = p + 1;

        if (strncmp(cmd, "env", 3) == 0 && (!cmd[3] || isspace((unsigned char)cmd[3]))) {
            env_print(ctx->cg.env);
            return true;
        }

        if (strncmp(cmd, "load", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\t')) {
            const char *path = cmd + 5;
            while (*path && isspace((unsigned char)*path)) path++;

            char fpath[512];
            strncpy(fpath, path, sizeof(fpath) - 1);
            fpath[sizeof(fpath) - 1] = '\0';
            char *end = fpath + strlen(fpath) - 1;
            while (end > fpath && isspace((unsigned char)*end)) *end-- = '\0';

            FILE *f = fopen(fpath, "r");
            if (!f) {
                fprintf(stderr, "Error: cannot open file: %s\n", fpath);
                return false;
            }

            fseek(f, 0, SEEK_END);
            long fsz = ftell(f);
            rewind(f);
            char *src = malloc(fsz + 1);
            if (!src) { fclose(f); fprintf(stderr, "Error: out of memory\n"); return false; }
            fread(src, 1, fsz, f);
            src[fsz] = '\0';
            fclose(f);

            /* ---- detect module declaration in first form ---- */
            const char *cursor = src;
            char *found_module = NULL;
            while (*cursor) {
                while (*cursor && isspace((unsigned char)*cursor)) cursor++;
                if (!*cursor) break;
                if (*cursor == ';') { while (*cursor && *cursor != '\n') cursor++; continue; }

                parser_set_context(fpath, cursor);
                AST *form = NULL;
                REPL_PARSE(form, cursor);
                if (form && form->type == AST_LIST && form->list.count >= 2 &&
                    form->list.items[0]->type == AST_SYMBOL &&
                    strcmp(form->list.items[0]->symbol, "module") == 0 &&
                    form->list.items[1]->type == AST_SYMBOL) {
                    found_module = strdup(form->list.items[1]->symbol);
                    ast_free(form);
                } else {
                    if (form) ast_free(form);
                }
                break;
            }
            free(src);

            if (found_module) {
                printf("Loading module '%s' from '%s'\n", found_module, fpath);
                char import_expr[256];
                snprintf(import_expr, sizeof(import_expr), "(import %s)", found_module);
                free(found_module);

                parser_set_context("<load>", import_expr);
                AST *imp_ast = NULL;
                REPL_PARSE(imp_ast, import_expr);
                if (!imp_ast) {
                    fprintf(stderr, "Error: failed to build import for module\n");
                    return false;
                }
                bool ok = handle_import(ctx, imp_ast);
                ast_free(imp_ast);
                return ok;
            }

            /* ---- re-open for the actual load loop ---- */
            FILE *f2 = fopen(fpath, "r");
            if (!f2) { fprintf(stderr, "Error: cannot open file: %s\n", fpath); return false; }
            fseek(f2, 0, SEEK_END);
            fsz = ftell(f2);
            rewind(f2);
            src = malloc(fsz + 1);
            if (!src) { fclose(f2); fprintf(stderr, "Error: out of memory\n"); return false; }
            fread(src, 1, fsz, f2);
            src[fsz] = '\0';
            fclose(f2);

            cursor = src;
            int loaded = 0, failed = 0, skipped = 0;

            while (*cursor) {
                /* skip whitespace and line comments */
                while (*cursor && isspace((unsigned char)*cursor)) cursor++;
                if (!*cursor) break;
                if (*cursor == ';') { while (*cursor && *cursor != '\n') cursor++; continue; }

                const char *form_start = cursor;

                /* ---- string-aware depth walker to find form_end ---- */
                if (*cursor == '(') {
                    int depth = 0;
                    bool in_str = false;
                    while (*cursor) {
                        if (in_str) {
                            if (*cursor == '\\') { cursor++; if (*cursor) cursor++; continue; }
                            if (*cursor == '"')  { in_str = false; cursor++; continue; }
                            cursor++;
                        } else {
                            if (*cursor == '"')  { in_str = true;  cursor++; continue; }
                            if (*cursor == '(')  { depth++; cursor++; continue; }
                            if (*cursor == ')') {
                                depth--;
                                cursor++;
                                if (depth == 0) break;
                                continue;
                            }
                            cursor++;
                        }
                    }
                } else {
                    while (*cursor && !isspace((unsigned char)*cursor)) cursor++;
                }
                const char *form_end = cursor;

                /* ---- parse the form we just measured ---- */
                char *form_copy = strndup(form_start, form_end - form_start);
                if (!form_copy) continue;
                parser_set_context(fpath, form_copy);
                AST *form = NULL;
                REPL_PARSE(form, form_copy);
                free(form_copy);
                if (!form) break;

                if (form->type != AST_LIST || form->list.count < 1 ||
                    form->list.items[0]->type != AST_SYMBOL) {
                    ast_free(form); skipped++; continue;
                }

                const char *head = form->list.items[0]->symbol;

                if (strcmp(head, "tests") == 0) {
                    ast_free(form); skipped++; continue;
                }
                if (strcmp(head, "import") == 0) {
                    handle_import(ctx, form);
                    ast_free(form); skipped++; continue;
                }

                /* ---- harvest source_text and docstring before codegen ---- */
                char harvested_name[256] = {0};
                char *harvested_doc = NULL;
                bool is_define = (strcmp(head, "define") == 0 && form->list.count >= 2);
                bool is_layout = (form->type == AST_LAYOUT);

                if (is_define) {
                    AST *name_expr = form->list.items[1];
                    if (name_expr->type == AST_SYMBOL)
                        strncpy(harvested_name, name_expr->symbol, 255);
                    else if (name_expr->type == AST_LIST && name_expr->list.count > 0 &&
                             name_expr->list.items[0]->type == AST_SYMBOL)
                        strncpy(harvested_name, name_expr->list.items[0]->symbol, 255);

                    if (form->list.count >= 3) {
                        AST *body = form->list.items[2];
                        if (body->type == AST_LAMBDA &&
                            body->lambda.docstring && body->lambda.docstring[0])
                            harvested_doc = strdup(body->lambda.docstring);
                    }
                }

                char mod_name[64];
                snprintf(mod_name,       sizeof(mod_name),       "__repl_%u", ctx->expr_count);
                snprintf(g_wrapper_name, sizeof(g_wrapper_name), WRAPPER_FMT, g_wrapper_seq++);
                fresh_module(ctx, mod_name);
                open_wrapper(ctx);

                ctx->cg.error_jmp_set = true;
                if (setjmp(ctx->cg.error_jmp) != 0) {
                    ctx->cg.error_jmp_set = false;
                    ast_free(form);
                    free(harvested_doc);
                    recover_module(ctx);
                    fresh_module(ctx, "__repl_recover");
                    failed++;
                    continue;
                }

                CodegenResult res = codegen_expr(&ctx->cg, form);
                ctx->cg.error_jmp_set = false;
                ast_free(form);

                /* ---- patch env entry with source_text + docstring ---- */
                if (is_define && harvested_name[0]) {
                    EnvEntry *e = env_lookup(ctx->cg.env, harvested_name);
                    if (e) {
                        if (!e->source_text)
                            e->source_text = strndup(form_start, form_end - form_start);
                        if (!e->docstring && harvested_doc) {
                            e->docstring = harvested_doc;
                            harvested_doc = NULL;
                        }
                    }
                }
                if (is_layout) {
                    /* layout name is in form->layout.name but form is freed —
                     * we can recover it from the env: the most recently added
                     * ENV_LAYOUT entry without source_text is ours.
                     * Simpler: re-derive from form_start which still points
                     * into src (valid until free(src) below). */
                    /* parse just the layout name from form_start */
                    const char *lp = form_start;
                    if (*lp == '(') lp++;
                    while (*lp && isspace((unsigned char)*lp)) lp++;
                    while (*lp && !isspace((unsigned char)*lp)) lp++; /* skip "layout" */
                    while (*lp && isspace((unsigned char)*lp)) lp++;
                    const char *lname_start = lp;
                    while (*lp && !isspace((unsigned char)*lp) && *lp != ')') lp++;
                    char lname[256] = {0};
                    strncpy(lname, lname_start, (size_t)(lp - lname_start) < 255
                                                ? (size_t)(lp - lname_start) : 255);
                    if (lname[0]) {
                        EnvEntry *e = env_lookup(ctx->cg.env, lname);
                        if (e && e->kind == ENV_LAYOUT && !e->source_text)
                            e->source_text = strndup(form_start, form_end - form_start);
                    }
                }
                free(harvested_doc);
                harvested_doc = NULL;

                if (!res.value) {
                    recover_module(ctx);
                    fresh_module(ctx, "__repl_recover");
                    failed++;
                    continue;
                }

                jmp_buf assert_jmp;
                g_assert_jmp_ptr = &assert_jmp;
                g_in_eval = true;
                bool ran = false;
                if (setjmp(assert_jmp) != 0) {
                    g_assert_jmp_ptr = NULL;
                    g_in_eval = false;
                    rt_interrupted = 0;
                    recover_module(ctx);
                    free(src);
                    return false;
                }
                if (setjmp(g_repl_escape) == 0)
                    ran = close_and_run(ctx);
                g_in_eval = false;
                g_assert_jmp_ptr = NULL;
                rt_interrupted = 0;

                if (ran) {
                    ctx->expr_count++;
                    char next[64];
                    snprintf(next, sizeof(next), "__repl_%u", ctx->expr_count);
                    fresh_module(ctx, next);
                    loaded++;
                } else {
                    recover_module(ctx);
                    fresh_module(ctx, "__repl_recover");
                    failed++;
                }
            }

            free(src);
            printf("Loaded '%s': %d ok, %d skipped, %d failed.\n",
                   fpath, loaded, skipped, failed);
            return failed == 0;
        }

        if (strncmp(cmd, "complete", 8) == 0 && isspace((unsigned char)cmd[8])) {
            const char *prefix = cmd + 9;
            while (*prefix && isspace((unsigned char)*prefix)) prefix++;
            size_t plen = strlen(prefix);

            printf("__COMPLETIONS__\n");

            Env *env = ctx->cg.env;
            for (size_t bi = 0; bi < env->size; bi++) {
                for (EnvEntry *e = env->buckets[bi]; e; e = e->next) {
                    if (!e->name) continue;
                    if (strncmp(e->name, prefix, plen) != 0) continue;

                    char sig[256] = "";
                    switch (e->kind) {
                    case ENV_VAR:
                        printf("%s\tvar\t%s\t%s\n",
                               e->name,
                               e->type ? type_to_string(e->type) : "?",
                               e->docstring ? e->docstring : "");
                        break;
                    case ENV_BUILTIN: {
                        int mn = e->arity_min > 0 ? e->arity_min : 0;
                        int mx = e->arity_max;
                        if (mn == 0 && mx == -1) {
                            strcpy(sig, "Fn _");
                        } else {
                            strcpy(sig, "Fn (");
                            for (int i = 0; i < mn; i++) {
                                if (i > 0) strncat(sig, " ", sizeof(sig)-strlen(sig)-1);
                                strncat(sig, "_", sizeof(sig)-strlen(sig)-1);
                            }
                            if (mx > mn && mx != -1) {
                                strncat(sig, mn > 0 ? " =>" : "=>", sizeof(sig)-strlen(sig)-1);
                                for (int i = mn; i < mx; i++)
                                    strncat(sig, " _", sizeof(sig)-strlen(sig)-1);
                            }
                            if (mx == -1 && mn > 0)
                                strncat(sig, " => _ . _", sizeof(sig)-strlen(sig)-1);
                            strncat(sig, ")", sizeof(sig)-strlen(sig)-1);
                        }
                        printf("%s\tbuiltin\t%s\t%s\n",
                               e->name, sig,
                               e->docstring ? e->docstring : "");
                        break;
                    }
                    case ENV_FUNC: {
                        snprintf(sig, sizeof(sig), "(%s", e->name);
                        for (int i = 0; i < e->param_count; i++) {
                            const char *pn = (e->params[i].name && e->params[i].name[0])
                                ? e->params[i].name : "_";
                            char param[128] = "";
                            if (e->params[i].type)
                                snprintf(param, sizeof(param), " [%s :: %s]",
                                         pn, type_to_string(e->params[i].type));
                            else
                                snprintf(param, sizeof(param), " [%s]", pn);
                            strncat(sig, param, sizeof(sig)-strlen(sig)-1);
                        }
                        if (e->return_type) {
                            strncat(sig, " -> ", sizeof(sig)-strlen(sig)-1);
                            strncat(sig, type_to_string(e->return_type),
                                    sizeof(sig)-strlen(sig)-1);
                        }
                        strncat(sig, ")", sizeof(sig)-strlen(sig)-1);
                        printf("%s\tfunc\t%s\t%s\n",
                               e->name, sig,
                               e->docstring ? e->docstring : "");
                        break;
                    }
                    }
                }
            }

            static const char *kws[] = {
                "Int","Float","Char","String","Hex","Bin","Oct","Bool",
                "define","show","if","for","quote","begin","set!","and","or","not",
                "import","cons","car","cdr","list","append","list-ref","list-length",
                "make-list","equal?","list-empty?","list-copy","append!",
                "string-length","string-ref","string-set!","make-string","show-value",
                NULL
            };
            for (int i = 0; kws[i]; i++)
                if (strncmp(kws[i], prefix, plen) == 0)
                    printf("%s\tkeyword\t\t\n", kws[i]);

            printf("__END__\n");
            fflush(stdout);
            return true;
        }

        if (strncmp(cmd, "help", 4) == 0) {
            printf("REPL commands:\n");
            printf("  ,env              dump all bindings\n");
            printf("  ,complete <pfx>   list completions for prefix\n");
            printf("  ,load <path>      load and evaluate a source file\n");
            printf("  ,help             show this help\n");
            return true;
        }

        fprintf(stderr, "Unknown command: ,%s\n", cmd);
        fprintf(stderr, "Try ,help for a list of commands.\n");
        return false;
    }

    /// Parse

    char *clean = strip_comments(line);
    if (!clean) return false;

    const char *chk = clean;
    while (*chk && isspace((unsigned char)*chk)) chk++;
    if (!*chk) { free(clean); return true; }

    parser_set_context("<input>", clean);
    AST *ast = NULL;
    REPL_PARSE(ast, clean);
    free(clean);
    if (!ast) return false;

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
     * Fast path: bare layout name — print its definition
     * ----------------------------------------------------------------------- */
    if (!silent && ast->type == AST_SYMBOL) {
        Type *lay = env_lookup_layout(ctx->cg.env, ast->symbol);
        if (lay && lay->kind == TYPE_LAYOUT) {
            printf("(layout %s\n", lay->layout_name);
            for (int i = 0; i < lay->layout_field_count; i++) {
                LayoutField *f = &lay->layout_fields[i];
                if (f->type && f->type->kind == TYPE_ARR) {
                    printf("  [%s :: [%s %d]]",
                           f->name,
                           f->type->arr_element_type
                               ? type_to_string(f->type->arr_element_type) : "?",
                           f->type->arr_size);
                } else {
                    printf("  [%s :: %s]",
                           f->name,
                           f->type ? type_to_string(f->type) : "?");
                }
                if (i < lay->layout_field_count - 1) printf("\n");
            }
            if (lay->layout_packed) printf("\n  :packed True");
            if (lay->layout_align)  printf("\n  :align %d", lay->layout_align);
            printf(")\n");
            ast_free(ast);
            return true;
        }
    }

    /* -----------------------------------------------------------------------
     * Fast path: bare symbol naming a function or builtin
     * ----------------------------------------------------------------------- */
    if (!silent && ast->type == AST_SYMBOL) {
        EnvEntry *e = env_lookup(ctx->cg.env, ast->symbol);
        if (e && (e->kind == ENV_FUNC || e->kind == ENV_BUILTIN)) {
            char mod_name[64];
            snprintf(mod_name,       sizeof(mod_name),       "__repl_%u", ctx->expr_count);
            snprintf(g_wrapper_name, sizeof(g_wrapper_name), WRAPPER_FMT, g_wrapper_seq++);
            fresh_module(ctx, mod_name);
            open_wrapper(ctx);
            emit_proc_print(ctx, e);
            ast_free(ast);

            g_in_eval = true;
            bool ran = false;
            if (setjmp(g_repl_escape) == 0)
                ran = close_and_run(ctx);
            g_in_eval = false;
            rt_interrupted = 0;

            if (ran) ctx->expr_count++;
            char next[64];
            snprintf(next, sizeof(next), "__repl_%u", ctx->expr_count);
            fresh_module(ctx, next);
            return ran;
        }
    }

    /* -----------------------------------------------------------------------
     * Normal path: codegen + JIT
     * ----------------------------------------------------------------------- */
    char mod_name[64];
    snprintf(mod_name,       sizeof(mod_name),       "__repl_%u", ctx->expr_count);
    snprintf(g_wrapper_name, sizeof(g_wrapper_name), WRAPPER_FMT, g_wrapper_seq++);
    fresh_module(ctx, mod_name);
    open_wrapper(ctx);

    /* Phase 1: codegen */
    ctx->cg.error_jmp_set = true;
    if (setjmp(ctx->cg.error_jmp) != 0) {
        ctx->cg.error_jmp_set = false;
        if (ast) { ast_free(ast); ast = NULL; }
        recover_module(ctx);
        fresh_module(ctx, "__repl_recover");
        return false;
    }

    bool ast_was_layout = (ast->type == AST_LAYOUT);
    char layout_name_buf[256] = {0};
    if (ast_was_layout)
        strncpy(layout_name_buf, ast->layout.name, sizeof(layout_name_buf) - 1);

    // Store source text for (code fn) reflection
    char *source_copy = NULL;
    if (ast->type == AST_LIST && ast->list.count >= 3 &&
        ast->list.items[0]->type == AST_SYMBOL &&
        strcmp(ast->list.items[0]->symbol, "define") == 0) {
        source_copy = strdup(line);  // raw input line
    }

    CodegenResult res = codegen_expr(&ctx->cg, ast);
    ctx->cg.error_jmp_set = false;

    if (source_copy && ast && ast->type == AST_LIST && ast->list.count >= 3) {
        AST *name_expr = ast->list.items[1];
        const char *fn_name = NULL;
        if (name_expr->type == AST_SYMBOL)
            fn_name = name_expr->symbol;
        else if (name_expr->type == AST_LIST && name_expr->list.count > 0 &&
                 name_expr->list.items[0]->type == AST_SYMBOL)
            fn_name = name_expr->list.items[0]->symbol;
        if (fn_name) {
            EnvEntry *e = env_lookup(ctx->cg.env, fn_name);
            if (e) {
                free(e->source_text);
                e->source_text = source_copy;
                source_copy = NULL;
            }
        }
    }
    free(source_copy);


    if (!res.value) {
        if (ast) ast_free(ast);
        recover_module(ctx);
        fresh_module(ctx, "__repl_recover");
        return false;
    }

    /* Phase 2: emit auto-print */
    bool list_is_rv = false;
    if (res.type && res.type->kind == TYPE_LIST &&
        ast->type == AST_LIST && ast->list.count > 0 &&
        ast->list.items[0]->type == AST_SYMBOL) {
        EnvEntry *ce = env_lookup(ctx->cg.env, ast->list.items[0]->symbol);
        if (ce && ce->module_name)
            list_is_rv = true;
    }

    ast_free(ast);
    ast = NULL;

    if (!silent)
        emit_auto_print(ctx, res.value, res.type, list_is_rv);

    /* Phase 3: JIT compile + run */
    jmp_buf assert_jmp;
    g_assert_jmp_ptr = &assert_jmp;
    g_in_eval = true;
    bool ran = false;
    if (setjmp(assert_jmp) != 0) {
        g_assert_jmp_ptr = NULL;
        g_in_eval = false;
        rt_interrupted = 0;
        recover_module(ctx);
        return false;
    }
    if (setjmp(g_repl_escape) == 0)
        ran = close_and_run(ctx);
    g_in_eval = false;
    g_assert_jmp_ptr = NULL;
    rt_interrupted = 0;

    /* if (ran) { */
    /*     ctx->expr_count++; */
    /*     char next[64]; */
    /*     snprintf(next, sizeof(next), "__repl_%u", ctx->expr_count); */
    /*     fresh_module(ctx, next); */
    /* } else { */
    if (ran) {
        ctx->expr_count++;
        // Attach source text to layout env entry for (code Layout) reflection
        if (ast_was_layout) {
            EnvEntry *e = env_lookup(ctx->cg.env, layout_name_buf);
            if (e && e->kind == ENV_LAYOUT && !e->source_text)
                e->source_text = strdup(line);
        }
        char next[64];
        snprintf(next, sizeof(next), "__repl_%u", ctx->expr_count);
        fresh_module(ctx, next);
    } else {
        recover_module(ctx);
        fresh_module(ctx, "__repl_recover");
    }
    return ran;
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

/* -------------------------------------------------------------------------
 * Electric pair mode — mirrors Emacs electric-pair-mode:
 *
 *  Typing ( inserts ()   cursor between them
 *  Typing [ inserts []   cursor between them
 *  Typing { inserts {}   cursor between them
 *  Typing " inserts ""   cursor between them; skip if next char is "
 *
 *  NOTE: ' is intentionally NOT electric — it is the quote shorthand.
 *
 *  Backspace at (|) / [|] / {|} / "|"  deletes the whole pair.
 *  Backspace elsewhere behaves normally.
 *  Both C-h (ASCII 8) and DEL (127 / actual Backspace) are bound.
 * ------------------------------------------------------------------------- */

static int electric_insert_pair(int count, int key) {
    (void)count;
    char close;
    switch (key) {
        case '(': close = ')'; break;
        case '[': close = ']'; break;
        case '{': close = '}'; break;
        case '"': close = '"'; break;
        default:  rl_insert(1, key); return 0;
    }

    /* For ": if next char is already the close, skip over it */
    if (key == '"' && rl_point < rl_end &&
        rl_line_buffer[rl_point] == close) {
        rl_point++;
        return 0;
    }

    rl_insert(1, key);
    rl_insert(1, close);
    rl_point--;
    return 0;
}

static int electric_backspace(int count, int key) {
    (void)count; (void)key;

    if (rl_point > 0 && rl_point < rl_end) {
        char prev = rl_line_buffer[rl_point - 1];
        char next = rl_line_buffer[rl_point];
        int is_pair = (prev == '(' && next == ')') ||
                      (prev == '[' && next == ']') ||
                      (prev == '{' && next == '}') ||
                      (prev == '"' && next == '"');
        if (is_pair) {
            rl_delete(1, 0);
            rl_rubout(1, '\b');
            return 0;
        }
    }
    rl_rubout(1, '\b');
    return 0;
}

static void setup_electric_pairs(void) {
    rl_bind_key('(',    electric_insert_pair);
    rl_bind_key('[',    electric_insert_pair);
    rl_bind_key('{',    electric_insert_pair);
    rl_bind_key('"',    electric_insert_pair);
    /* C-h (ASCII 8) — traditional backspace */
    rl_bind_key('\b',   electric_backspace);
    /* DEL (127) — the actual Backspace key on most terminals.
     * rl_bind_key() clips its argument to 0-127 but key 127 sits exactly
     * at the boundary; some readline builds mis-handle it.  Use
     * rl_bind_keyseq() with the raw escape sequence to be safe. */
    rl_bind_keyseq("\\177", electric_backspace);   /* \177 = octal 127 = DEL */
    /* Also bind the VT220 / xterm "Backspace sends ^?" sequence */
    rl_bind_keyseq("\\C-?", electric_backspace);
}

/* -------------------------------------------------------------------------
 * Colored prompt.
 *
 * On success: "Monad \033[32mλ\033[0m "  (green λ)
 * On error:   "Monad \033[31mλ\033[0m "  (red λ)
 *
 * readline requires RL_PROMPT_START_IGNORE / RL_PROMPT_END_IGNORE wrappers
 * (\001 and \002) around invisible escape sequences so it can measure the
 * visible width correctly.
 * ------------------------------------------------------------------------- */
#define PROMPT_OK    "\001\033[34m\002Monad\001\033[0m\002 \001\033[32m\002λ\001\033[0m\002 "
#define PROMPT_ERROR "\001\033[34m\002Monad\001\033[0m\002 \001\033[31m\002λ\001\033[0m\002 "
// When under Emacs
#define PROMPT_OK_PLAIN    "\033[34mMonad\033[0m \033[32m\xce\xbb\033[0m "
#define PROMPT_ERROR_PLAIN "\033[34mMonad\033[0m \033[31m\xce\xbb\033[0m "


/* Count paren depth of a string, ignoring chars inside strings/comments */


static int paren_depth(const char *s) {
    int depth = 0;
    bool in_str = false;
    for (; *s; s++) {
        if (in_str) {
            if (*s == '\\') { s++; continue; }  /* skip escaped char */
            if (*s == '"')  in_str = false;
        } else {
            if (*s == '"')        in_str = true;
            else if (*s == ';')   break;          /* line comment — stop */
            else if (*s == '(')   depth++;
            else if (*s == ')')   depth--;
        }
    }
    return depth;
}

void repl_run(void) {
    REPLContext ctx;
    repl_init(&ctx);
    g_repl_ctx = &ctx;

    /* Emacs runs the process with INSIDE_EMACS set, or with TERM=dumb.
     * In both cases readline is useless (no terminal, no electric pairs)
     * and — crucially — it reads one line at a time via the pty, which
     * breaks multiline input.  Use the plain accumulation loop instead. */
    bool use_readline = isatty(STDIN_FILENO)
                        && !getenv("INSIDE_EMACS")
                        && !(getenv("TERM") && strcmp(getenv("TERM"), "dumb") == 0);

    /* MONAD_NO_PROMPT=1 suppresses the prompt entirely — useful when Emacs
     * or another tool drives the process and handles its own prompt display. */
    bool show_prompt = !(getenv("MONAD_NO_PROMPT") &&
                         strcmp(getenv("MONAD_NO_PROMPT"), "0") != 0);

    if (use_readline) {
        rl_attempted_completion_function = repl_completion;
        setup_electric_pairs();
    }

    /* Readline needs \001/\002 wrappers around invisible escape sequences
     * so it can measure the visible prompt width correctly.  Emacs (and
     * plain pipes) must never see those control characters.              */
    const char *prompt_ok    = use_readline ? PROMPT_OK    : PROMPT_OK_PLAIN;
    const char *prompt_error = use_readline ? PROMPT_ERROR : PROMPT_ERROR_PLAIN;
    const char *prompt       = prompt_ok;

    char  accum[65536];
    int   accum_len = 0;
    int   depth     = 0;
    bool  multiline = false;

    char *line;
    while (1) {
        if (use_readline) {
            line = readline(multiline ? "" : prompt);
            if (!line) break;
        } else {
            /* Print prompt only at the start of a new expression */
            if (show_prompt && !multiline) {
                fprintf(stdout, "%s", prompt);
                fflush(stdout);
            }

            char buf[4096];
            if (!fgets(buf, sizeof(buf), stdin)) break;
            size_t n = strlen(buf);
            if (n > 0 && buf[n-1] == '\n') buf[--n] = '\0';
            line = strdup(buf);
        }

        /* Skip blank lines when not mid-expression */
        if (!multiline && !*line) {
            free(line);
            continue;
        }

        int len = strlen(line);
        if (accum_len + len + 2 < (int)sizeof(accum)) {
            if (accum_len > 0) accum[accum_len++] = '\n';
            memcpy(accum + accum_len, line, len);
            accum_len += len;
            accum[accum_len] = '\0';
        }

        depth += paren_depth(line);
        free(line);

        if (depth > 0) {
            multiline = true;
            continue;
        }

        multiline = false;
        depth     = 0;

        if (*accum) {
            if (use_readline) add_history(accum);
            bool ok = repl_eval_line(&ctx, accum);
            if (g_interrupted) {
                g_interrupted = 0;
                if (use_readline) {
                    rl_free_line_state();
                    rl_cleanup_after_signal();
                }
                prompt = prompt_error;
            } else {
                prompt = ok ? prompt_ok : prompt_error;
            }
        }

        accum_len = 0;
        accum[0]  = '\0';
    }

    /* EOF mid-expression — evaluate whatever we have */
    if (accum_len > 0)
        repl_eval_line(&ctx, accum);

    printf("\n");
    repl_dispose(&ctx);
}

