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
                if (LLVMGetNamedGlobal(ctx->cg.module, name)) continue;

                LLVMTypeRef  lt = type_to_llvm(&ctx->cg, e->type);
                LLVMValueRef gv = LLVMAddGlobal(ctx->cg.module, lt, name);
                LLVMSetLinkage(gv, LLVMExternalLinkage);
                e->value = gv;
            }

            else if (e->kind == ENV_FUNC && e->func_ref) {
                /* Use the mangled name stored on the LLVMValueRef */
                const char *name = LLVMGetValueName(e->func_ref);
                if (!name || !*name) name = e->name;
                if (LLVMGetNamedFunction(ctx->cg.module, name)) continue;

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

static void emit_auto_print(REPLContext *ctx, LLVMValueRef val, Type *t) {
    if (!val || !t) return;
    LLVMValueRef pf = get_or_declare_printf(&ctx->cg);

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
        /* String literals: codegen returns bare char* via LLVMBuildGlobalStringPtr */
        LLVMValueRef a[] = {get_fmt_str(&ctx->cg), val};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 2, "");
        break; }
    case TYPE_KEYWORD:
    case TYPE_SYMBOL: {
        /* Symbols/keywords: codegen returns RuntimeValue* — delegate to rt_print_value_newline */
        LLVMValueRef pfn = get_rt_print_value_newline(&ctx->cg);
        LLVMTypeRef  ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->cg.context),0);
        LLVMTypeRef  ft2 = LLVMFunctionType(
            LLVMVoidTypeInContext(ctx->cg.context), &ptr, 1, 0);
        LLVMValueRef a[] = {val};
        LLVMBuildCall2(ctx->cg.builder, ft2, pfn, a, 1, "");
        break; }
    case TYPE_BOOL: {
        LLVMValueRef ts = LLVMBuildGlobalStringPtr(ctx->cg.builder,"True\n","ts");
        LLVMValueRef fs = LLVMBuildGlobalStringPtr(ctx->cg.builder,"False\n","fs");
        LLVMValueRef s  = LLVMBuildSelect(ctx->cg.builder, val, ts, fs, "bs");
        LLVMValueRef a[] = {s};
        LLVMBuildCall2(ctx->cg.builder, LLVMGlobalGetValueType(pf), pf, a, 1, "");
        break; }
    case TYPE_LIST:
    case TYPE_RATIO:
    case TYPE_UNKNOWN: {
        /* val is RuntimeValue* — rt_print_value_newline handles all boxed types */
        LLVMValueRef pfn = get_rt_print_value_newline(&ctx->cg);
        LLVMTypeRef  ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->cg.context),0);
        LLVMTypeRef  ft2 = LLVMFunctionType(
            LLVMVoidTypeInContext(ctx->cg.context), &ptr, 1, 0);
        LLVMValueRef a[] = {val};
        LLVMBuildCall2(ctx->cg.builder, ft2, pfn, a, 1, "");
        break; }
    default: break;
    }
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

    g_in_eval = true;
    if (setjmp(g_repl_escape) == 0) {
        res = codegen_expr(&ctx->cg, ast);
        if (!res.value) ok = false;
    } else {
        ok = false;
    }
    g_in_eval = false;

    ast_free(ast);

    if (!ok) {
        recover_module(ctx);
        return false;
    }

    if (!silent && res.type)
        emit_auto_print(ctx, res.value, res.type);

    bool ran = close_and_run(ctx);
    if (ran) ctx->expr_count++;

    /* Prepare module for next expression */
    char next[64];
    snprintf(next, sizeof(next), "__repl_%u", ctx->expr_count);
    fresh_module(ctx, next);

    return ran;
}

/// readline completion

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

    if (!state) { bi = 0; cur = NULL; len = strlen(text); ki = 0; }

    Env *env = g_repl_ctx->cg.env;
    if (cur) cur = cur->next;

    while (bi < env->size) {
        if (!cur) { cur = env->buckets[bi]; if (!cur) { bi++; continue; } }
        while (cur) {
            if (strncmp(cur->name, text, len) == 0) {
                char *r = strdup(cur->name);
                cur = cur->next;
                return r;
            }
            cur = cur->next;
        }
        bi++;
    }

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
