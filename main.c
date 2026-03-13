#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#include "reader.h"
#include "cli.h"
#include "types.h"
#include "env.h"
#include "repl.h"
#include "features.h"
#include "runtime.h"
#include "codegen.h"
#include "module.h"
#include "buildsystem.h"

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/TargetMachine.h>

// In-memory compiled-module registry

typedef struct CompiledExport {
    char         *local_name;
    char         *mangled_name;
    EnvEntryKind  kind;
    Type         *type;         // VAR: variable type; FUNC: unused (use return_type)
    Type         *return_type;  // FUNC only
    EnvParam     *params;       // FUNC only, owned
    int           param_count;
    LLVMValueRef  func_ref;     // FUNC only — valid only when not skipped
} CompiledExport;

typedef struct CompiledModule {
    char           *module_name;
    char           *obj_path;
    bool            was_skipped;    // compiled from .o timestamp, no LLVMValueRef
    CompiledExport *exports;
    size_t          export_count;
    size_t          export_cap;
    struct CompiledModule *next;
} CompiledModule;

static CompiledModule *g_compiled = NULL;

static CompiledModule *registry_find(const char *name) {
    for (CompiledModule *m = g_compiled; m; m = m->next)
        if (strcmp(m->module_name, name) == 0) return m;
    return NULL;
}

static CompiledModule *registry_new(const char *name, const char *obj,
                                     bool skipped) {
    CompiledModule *m = calloc(1, sizeof(CompiledModule));
    m->module_name  = strdup(name);
    m->obj_path     = strdup(obj);
    m->was_skipped  = skipped;
    m->export_cap   = 8;
    m->exports      = malloc(sizeof(CompiledExport) * 8);
    m->next         = g_compiled;
    g_compiled      = m;
    return m;
}

static void registry_grow(CompiledModule *m) {
    if (m->export_count < m->export_cap) return;
    m->export_cap *= 2;
    m->exports = realloc(m->exports, sizeof(CompiledExport) * m->export_cap);
}

static void registry_push_var(CompiledModule *m, const char *local,
                               const char *mangled, Type *type) {
    registry_grow(m);
    CompiledExport *e = &m->exports[m->export_count++];
    memset(e, 0, sizeof(*e));
    e->local_name   = strdup(local);
    e->mangled_name = strdup(mangled);
    e->kind         = ENV_VAR;
    e->type         = type_clone(type);
}

static void registry_push_func(CompiledModule *m, const char *local,
                                const char *mangled, Type *ret,
                                EnvParam *params, int pc, LLVMValueRef fn) {
    registry_grow(m);
    CompiledExport *e = &m->exports[m->export_count++];
    memset(e, 0, sizeof(*e));
    e->local_name   = strdup(local);
    e->mangled_name = strdup(mangled);
    e->kind         = ENV_FUNC;
    e->return_type  = type_clone(ret);
    e->param_count  = pc;
    e->func_ref     = fn;
    if (pc > 0 && params) {
        e->params = malloc(sizeof(EnvParam) * pc);
        for (int i = 0; i < pc; i++) {
            e->params[i].name = strdup(params[i].name ? params[i].name : "_");
            e->params[i].type = type_clone(params[i].type);
        }
    }
}

static void registry_free_all(void) {
    CompiledModule *m = g_compiled;
    while (m) {
        CompiledModule *next = m->next;
        free(m->module_name);
        free(m->obj_path);
        for (size_t i = 0; i < m->export_count; i++) {
            CompiledExport *e = &m->exports[i];
            free(e->local_name);
            free(e->mangled_name);
            type_free(e->type);
            type_free(e->return_type);
            if (e->params) {
                for (int j = 0; j < e->param_count; j++) {
                    free(e->params[j].name);
                    type_free(e->params[j].type);
                }
                free(e->params);
            }
        }
        free(m->exports);
        free(m);
        m = next;
    }
    g_compiled = NULL;
}

const char **repl_get_compiled_obj_paths(void) {
    size_t n = 0;
    for (CompiledModule *m = g_compiled; m; m = m->next) n++;
    const char **arr = malloc(sizeof(char *) * (n + 1));
    size_t i = 0;
    for (CompiledModule *m = g_compiled; m; m = m->next)
        arr[i++] = m->obj_path;
    arr[i] = NULL;
    return arr;
}

/// Helpers

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Cannot open file: %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = malloc(sz + 1);
    fread(src, 1, sz, f); src[sz] = '\0'; fclose(f);
    return src;
}

static time_t file_mtime(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) ? st.st_mtime : 0;
}

static bool file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

static char *base_no_ext(const char *path) {
    char *b = strdup(path);
    char *dot = strrchr(b, '.');
    if (dot) *dot = '\0';
    return b;
}

static char *path_to_module_name(const char *path) {
    char *b = base_no_ext(path);
    for (char *p = b; *p; p++) if (*p == '/') *p = '.';
    char *start = b;
    if (start[0] == '.' && start[1] == '/') start += 2;
    char *r = strdup(start);
    free(b);
    return r;
}

// "Math" + "phi" -> "Math__phi"
static char *mangle(const char *mod, const char *local) {
    size_t ml = strlen(mod), ll = strlen(local);
    char *r = malloc(ml + 2 + ll + 1);
    for (size_t i = 0; i < ml; i++)
        r[i] = (mod[i] == '.') ? '_' : mod[i];
    r[ml] = r[ml+1] = '_';
    memcpy(r + ml + 2, local, ll + 1);
    return r;
}

static bool emit_object(LLVMModuleRef mod, const char *obj_path) {
    char *triple = LLVMGetDefaultTargetTriple();
    char *error  = NULL;
    LLVMTargetRef target;
    if (LLVMGetTargetFromTriple(triple, &target, &error) != 0) {
        fprintf(stderr, "target error: %s\n", error);
        LLVMDisposeMessage(error); LLVMDisposeMessage(triple); return false;
    }
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target, triple, "generic", "",
        LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);
    char buf[512]; strncpy(buf, obj_path, sizeof(buf)-1); buf[511] = '\0';
    bool ok = true;
    if (LLVMTargetMachineEmitToFile(tm, mod, buf, LLVMObjectFile, &error) != 0) {
        fprintf(stderr, "emit error for %s: %s\n", obj_path, error);
        LLVMDisposeMessage(error); ok = false;
    }
    LLVMDisposeTargetMachine(tm);
    LLVMDisposeMessage(triple);
    return ok;
}

static void declare_externals(CodegenContext *ctx,
                               CompiledModule *dep,
                               ImportDecl *import) {
    for (size_t i = 0; i < dep->export_count; i++) {
        CompiledExport *e = &dep->exports[i];

        bool do_import = false;
        switch (import->mode) {
        case IMPORT_QUALIFIED:
        case IMPORT_UNQUALIFIED: do_import = true; break;
        case IMPORT_SELECTIVE:
            do_import =  import_decl_includes_symbol(import, e->local_name); break;
        case IMPORT_HIDING:
            do_import = !import_decl_includes_symbol(import, e->local_name); break;
        }
        if (!do_import) continue;

        const char *last_dot = strrchr(dep->module_name, '.');
        const char *mod_last = last_dot ? last_dot + 1 : dep->module_name;
        const char *prefix   = import->alias ? import->alias : mod_last;
        char qn[512];
        snprintf(qn, sizeof(qn), "%s.%s", prefix, e->local_name);

        if (e->kind == ENV_VAR) {
            LLVMTypeRef  lt = type_to_llvm(ctx, e->type);
            LLVMValueRef gv = LLVMGetNamedGlobal(ctx->module, e->mangled_name);
            if (!gv) {
                gv = LLVMAddGlobal(ctx->module, lt, e->mangled_name);
                LLVMSetLinkage(gv, LLVMExternalLinkage);
            }
            env_insert_from_module(ctx->env, qn, dep->module_name,
                                   type_clone(e->type), gv, true);
            EnvEntry *eq = env_lookup(ctx->env, qn);
            if (eq) eq->llvm_name = strdup(e->mangled_name);

            if (import->mode != IMPORT_QUALIFIED) {
                env_insert_from_module(ctx->env, e->local_name, dep->module_name,
                                       type_clone(e->type), gv, true);
                EnvEntry *el = env_lookup(ctx->env, e->local_name);
                if (el) el->llvm_name = strdup(e->mangled_name);
            }
        } else { /* FUNC */
            LLVMTypeRef *pt = e->param_count > 0
                ? malloc(sizeof(LLVMTypeRef) * e->param_count) : NULL;
            for (int j = 0; j < e->param_count; j++)
                pt[j] = type_to_llvm(ctx, e->params[j].type);
            LLVMTypeRef fnt = LLVMFunctionType(
                type_to_llvm(ctx, e->return_type), pt, e->param_count, 0);
            if (pt) free(pt);
            LLVMValueRef fn = LLVMGetNamedFunction(ctx->module, e->mangled_name);
            if (!fn) {
                fn = LLVMAddFunction(ctx->module, e->mangled_name, fnt);
                LLVMSetLinkage(fn, LLVMExternalLinkage);
            }
            env_insert_func(ctx->env, qn,
                            clone_params(e->params, e->param_count),
                            e->param_count, type_clone(e->return_type), fn, NULL);
            EnvEntry *ent = env_lookup(ctx->env, qn);
            if (ent) { ent->module_name = strdup(dep->module_name);
                       ent->llvm_name   = strdup(e->mangled_name); }

            if (import->mode != IMPORT_QUALIFIED) {
                env_insert_func(ctx->env, e->local_name,
                                clone_params(e->params, e->param_count),
                                e->param_count, type_clone(e->return_type), fn, NULL);
                EnvEntry *ent2 = env_lookup(ctx->env, e->local_name);
                if (ent2) { ent2->module_name = strdup(dep->module_name);
                            ent2->llvm_name   = strdup(e->mangled_name); }
            }
        }
    }
}

static char *get_obj_path(const char *source_path) {
    const char *home = getenv("HOME");

    // Check system core
    const char *system_core = "/usr/local/lib/monad/core/";
    // Check env core
    const char *env_core = getenv("MONAD_CORE");

    const char *core_prefix = NULL;
    if (strncmp(source_path, system_core, strlen(system_core)) == 0)
        core_prefix = system_core;
    else if (env_core && strncmp(source_path, env_core, strlen(env_core)) == 0)
        core_prefix = env_core;

    if (core_prefix && home) {
        const char *rel = source_path + strlen(core_prefix);
        char *base = base_no_ext(rel);

        // Replace slashes with underscores for flat cache layout
        for (char *p = base; *p; p++) if (*p == '/') *p = '_';

        char cache_dir[1024];
        snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/monad/core", home);
        char mkdir_cmd[1024];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", cache_dir);
        system(mkdir_cmd);

        char obj[1024];
        snprintf(obj, sizeof(obj), "%s/%s.o", cache_dir, base);
        free(base);
        return strdup(obj);
    }

    // Normal case
    char *base = base_no_ext(source_path);
    char *obj  = malloc(strlen(base) + 3);
    sprintf(obj, "%s.o", base);
    free(base);
    return obj;
}

static CompiledModule *compile_one(const char *source_path,
                                    CompilerFlags *flags,
                                    bool is_main_module) {
    // Defensively own the path — callers may free dep_src right after returning
    char *my_source_path = strdup(source_path);

    char *base     = base_no_ext(my_source_path);
    /* char *obj_path = malloc(strlen(base) + 3); */
    /* sprintf(obj_path, "%s.o", base); */
    char *obj_path = get_obj_path(my_source_path);


    // Incremental check for library modules
    if (!is_main_module) {
        char *guessed = path_to_module_name(my_source_path);
        CompiledModule *cached = registry_find(guessed);
        free(guessed);
        if (cached) {
            free(base); free(obj_path); free(my_source_path);
            return cached;
        }
    }

    // Check if we can skip emitting a new .o (but we still run full codegen
    // to populate the registry with correct types — it's cheap without emit)
    time_t src_t = file_mtime(my_source_path);
    time_t obj_t = file_mtime(obj_path);
    bool skip_emit = !is_main_module && (obj_t > 0 && obj_t > src_t);

    if (skip_emit)
        printf("[skip]    %s (.o is up to date)\n", my_source_path);
    else
        printf("[compile] %s\n", my_source_path);

    // Read + parse
    char *source = read_file(my_source_path);
    parser_set_context(my_source_path, source);
    ASTList exprs = parse_all(source);
    if (exprs.count == 0) {
        fprintf(stderr, "%s: error: no expressions found\n", my_source_path);
        exit(1);
    }

/// Phase 1: Module + import declarations

    ModuleContext *mod_ctx = module_context_create();
    module_context_set_file(mod_ctx, my_source_path);
    ModuleDecl *module_decl = NULL;
    size_t first_code = 0;

    for (size_t i = 0; i < exprs.count; i++) {
        AST *expr = exprs.exprs[i];
        if (expr->type != AST_LIST || expr->list.count == 0 ||
            expr->list.items[0]->type != AST_SYMBOL) break;
        const char *head = expr->list.items[0]->symbol;
        if (strcmp(head, "module") == 0) {
            if (module_decl) {
                fprintf(stderr, "%s:%d: error: duplicate module declaration\n",
                        my_source_path, expr->line); exit(1);
            }
            module_decl = parse_module_decl(expr);
            if (!module_decl) {
                fprintf(stderr, "%s:%d: error: invalid module declaration\n",
                        my_source_path, expr->line); exit(1);
            }
            module_context_set_decl(mod_ctx, module_decl);
            first_code = i + 1;
        } else if (strcmp(head, "import") == 0) {
            ImportDecl *imp = parse_import_decl(expr);
            if (!imp) {
                fprintf(stderr, "%s:%d: error: invalid import\n",
                        my_source_path, expr->line); exit(1);
            }
            module_context_add_import(mod_ctx, imp);
            first_code = i + 1;
        } else {
            break;
        }
    }

    if (!module_decl) {
        char *guessed = path_to_module_name(my_source_path);
        module_decl = module_decl_create(guessed, EXPORT_ALL);
        if (!module_decl) {
            free(guessed);
            module_decl = module_decl_create("Main", EXPORT_ALL);
        } else {
            free(guessed);
        }
        module_context_set_decl(mod_ctx, module_decl);
    }
    const char *mod_name = module_decl->name;
    printf("  module: %s\n", mod_name);

/// Phase 2: Recursively compile dependencies

    for (size_t i = 0; i < mod_ctx->import_count; i++) {
        ImportDecl *imp = mod_ctx->imports[i];
        char *dep_src = module_name_to_path(imp->module_name);
        if (!file_exists(dep_src)) {
            fprintf(stderr, "error: cannot find module '%s' (tried: %s)",
                    imp->module_name, dep_src);
            free(dep_src); exit(1);
        }
        // dep_src is freed here after compile_one returns; compile_one
        // strdups it internally so this is safe
        compile_one(dep_src, flags, false);
        free(dep_src);
        parser_set_context(my_source_path, source);
    }

/// Phase 3: LLVM setup

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();

    CodegenContext ctx;
    codegen_init(&ctx, mod_name);
    ctx.module_ctx = mod_ctx;
    ctx.test_mode  = flags->test_mode;
    register_builtins(&ctx);
    declare_runtime_functions(&ctx);

/// Phase 4: Declare externals from compiled deps

    for (size_t i = 0; i < mod_ctx->import_count; i++) {
        ImportDecl *imp = mod_ctx->imports[i];
        CompiledModule *dep = registry_find(imp->module_name);
        if (!dep) {
            fprintf(stderr, "internal error: '%s' not in registry after compile\n",
                    imp->module_name); exit(1);
        }
        declare_externals(&ctx, dep, imp);
    }

/// Phase 5: Create init/main function and position builder
// (Must happen BEFORE any IR-building calls like features setup)

    char init_name[256];
    snprintf(init_name, sizeof(init_name), "__init_%s", mod_name);
    for (char *p = init_name; *p; p++) if (*p == '.') *p = '_';

    LLVMValueRef init_fn;
    if (is_main_module) {
        LLVMTypeRef mt = LLVMFunctionType(
            LLVMInt32TypeInContext(ctx.context), NULL, 0, 0);
        init_fn = LLVMAddFunction(ctx.module, "main", mt);
    } else {
        LLVMTypeRef vt = LLVMFunctionType(
            LLVMVoidTypeInContext(ctx.context), NULL, 0, 0);
        init_fn = LLVMAddFunction(ctx.module, init_name, vt);
    }
    LLVMBasicBlockRef entry_blk = LLVMAppendBasicBlockInContext(
        ctx.context, init_fn, "entry");
    LLVMPositionBuilderAtEnd(ctx.builder, entry_blk);
    ctx.init_fn = init_fn;

/// Phase 6: *features* global

    AST *feat_ast = detect_features();

    // Build the list back-to-front as a proper cons chain.
    // rt_list_append has undefined behaviour on the new fused ConsCell
    // implementation for strict lists — use rt_list_cons instead.
    LLVMValueRef empty_fn = get_rt_list_empty(&ctx);
    LLVMValueRef feat_list = LLVMBuildCall2(ctx.builder,
        LLVMGlobalGetValueType(empty_fn), empty_fn, NULL, 0, "feats");

    for (int i = (int)feat_ast->list.count - 1; i >= 0; i--) {
        AST *fk = feat_ast->list.items[i];
        if (fk->type != AST_KEYWORD) continue;

        LLVMValueRef kwf = get_rt_value_keyword(&ctx);
        LLVMValueRef kws = LLVMBuildGlobalStringPtr(ctx.builder, fk->keyword, "fk");
        LLVMValueRef ka[] = {kws};
        LLVMValueRef kv   = LLVMBuildCall2(ctx.builder,
            LLVMGlobalGetValueType(kwf), kwf, ka, 1, "kv");

        LLVMValueRef cons_fn = get_rt_list_cons(&ctx);
        LLVMTypeRef  ptr     = LLVMPointerType(LLVMInt8TypeInContext(ctx.context), 0);
        LLVMTypeRef  cons_ft = LLVMFunctionType(ptr, (LLVMTypeRef[]){ptr, ptr}, 2, 0);
        LLVMValueRef ca[]    = {kv, feat_list};
        feat_list = LLVMBuildCall2(ctx.builder, cons_ft, cons_fn, ca, 2, "feat_list");
    }

    ast_free(feat_ast);
    Type *ft = type_list(type_keyword());
    LLVMTypeRef flt = type_to_llvm(&ctx, ft);
    LLVMValueRef fgv = LLVMAddGlobal(ctx.module, flt, "__features__");
    LLVMSetInitializer(fgv, LLVMConstNull(flt));
    LLVMSetLinkage(fgv, LLVMInternalLinkage);
    LLVMBuildStore(ctx.builder, feat_list, fgv);
    env_insert(ctx.env, "*features*", ft, fgv);

/* /// Phase 6: *features* global (builder is now positioned — safe to emit IR) */

/*     AST *feat_ast = detect_features(); */
/*     LLVMValueRef lf = get_rt_list_empty(&ctx); */
/*     LLVMValueRef feat_list = LLVMBuildCall2(ctx.builder, */
/*         LLVMGlobalGetValueType(lf), lf, NULL, 0, "feats"); */
/*     for (size_t i = 0; i < feat_ast->list.count; i++) { */
/*         AST *fk = feat_ast->list.items[i]; */
/*         if (fk->type == AST_KEYWORD) { */
/*             LLVMValueRef kwf = get_rt_value_keyword(&ctx); */
/*             LLVMValueRef kws = LLVMBuildGlobalStringPtr(ctx.builder, */
/*                                                         fk->keyword, "fk"); */
/*             LLVMValueRef ka[] = {kws}; */
/*             LLVMValueRef kv   = LLVMBuildCall2(ctx.builder, */
/*                 LLVMGlobalGetValueType(kwf), kwf, ka, 1, "kv"); */
/*             LLVMValueRef af   = get_rt_list_append(&ctx); */
/*             LLVMValueRef aa[] = {feat_list, kv}; */
/*             LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(af), */
/*                            af, aa, 2, ""); */
/*         } */
/*     } */
/*     ast_free(feat_ast); */
/*     Type *ft = type_list(type_keyword()); */
/*     LLVMTypeRef flt = type_to_llvm(&ctx, ft); */
/*     LLVMValueRef fgv = LLVMAddGlobal(ctx.module, flt, "__features__"); */
/*     LLVMSetInitializer(fgv, LLVMConstNull(flt)); */
/*     LLVMSetLinkage(fgv, LLVMInternalLinkage); */
/*     LLVMBuildStore(ctx.builder, feat_list, fgv); */
/*     env_insert(ctx.env, "*features*", ft, fgv); */

/// Phase 7: Codegen top-level expressions

    // For the main module: call each imported library's init function first
    // so their top-level variable stores (e.g. phi = 3.14) run before we use them.
    if (is_main_module) {
        for (size_t i = 0; i < mod_ctx->import_count; i++) {
            ImportDecl *imp = mod_ctx->imports[i];
            // Build the init function name the same way compile_one does
            char dep_init[256];
            snprintf(dep_init, sizeof(dep_init), "__init_%s", imp->module_name);
            for (char *p = dep_init; *p; p++) if (*p == '.') *p = '_';

            // Declare it as extern void fn(void) if not already present
            LLVMTypeRef dep_init_t = LLVMFunctionType(
                LLVMVoidTypeInContext(ctx.context), NULL, 0, 0);
            LLVMValueRef dep_fn = LLVMGetNamedFunction(ctx.module, dep_init);
            if (!dep_fn) {
                dep_fn = LLVMAddFunction(ctx.module, dep_init, dep_init_t);
                LLVMSetLinkage(dep_fn, LLVMExternalLinkage);
            }
            LLVMBuildCall2(ctx.builder, dep_init_t, dep_fn, NULL, 0, "");
        }
    }

    CodegenResult last = {NULL, NULL};
    for (size_t i = first_code; i < exprs.count; i++) {
        AST *expr = exprs.exprs[i];
        if (expr->type == AST_LIST && expr->list.count > 0 &&
            expr->list.items[0]->type == AST_SYMBOL) {
            const char *h = expr->list.items[0]->symbol;
            if (strcmp(h, "module") == 0) {
                fprintf(stderr, "%s:%d: error: 'module' must be first\n",
                        my_source_path, expr->line); exit(1);
            }
            if (strcmp(h, "import") == 0) {
                fprintf(stderr, "%s:%d: error: 'import' must precede code\n",
                        my_source_path, expr->line); exit(1);
            }
        }
        last = codegen_expr(&ctx, expr);
    }

/// Phase 8: Terminate function

    if (is_main_module) {
        LLVMValueRef rc = LLVMConstInt(LLVMInt32TypeInContext(ctx.context), 0, 0);
        if (last.value && last.type && type_is_integer(last.type))
            rc = LLVMBuildTrunc(ctx.builder, last.value,
                                LLVMInt32TypeInContext(ctx.context), "rc");
        LLVMBuildRet(ctx.builder, rc);
    } else {
        LLVMBuildRetVoid(ctx.builder);
    }

/// Phase 9: Build registry entry + rename LLVM symbols to mangled names

    CompiledModule *cm = registry_new(mod_name, obj_path, false);

    for (size_t bi = 0; bi < ctx.env->size; bi++) {
        EnvEntry *ent = ctx.env->buckets[bi];
        while (ent) {
            if (ent->kind == ENV_BUILTIN || ent->module_name != NULL ||
                ent->name[0] == '*') { ent = ent->next; continue; }
            if (!module_decl_is_exported(module_decl, ent->name)) {
                ent = ent->next; continue;
            }

            char *ms = mangle(mod_name, ent->name);

            if (ent->kind == ENV_FUNC) {
                registry_push_func(cm, ent->name, ms,
                                   ent->return_type,
                                   ent->params, ent->param_count,
                                   ent->func_ref);
                if (ent->func_ref) {
                    const char *cur = LLVMGetValueName(ent->func_ref);
                    if (!cur || strcmp(cur, ms) != 0)
                        LLVMSetValueName2(ent->func_ref, ms, strlen(ms));
                }
            } else {
                if (!ent->type) { free(ms); ent = ent->next; continue; }
                registry_push_var(cm, ent->name, ms, ent->type);
                LLVMValueRef gv = ent->value;
                if (gv && LLVMIsAGlobalVariable(gv)) {
                    const char *cur = LLVMGetValueName(gv);
                    if (!cur || strcmp(cur, ms) != 0)
                        LLVMSetValueName2(gv, ms, strlen(ms));
                    LLVMSetLinkage(gv, LLVMExternalLinkage);
                }
            }
            free(ms);
            ent = ent->next;
        }
    }

/// Phase 10: Verify + optional IR/asm output

    char *error = NULL;
    if (LLVMVerifyModule(ctx.module, LLVMPrintMessageAction, &error) != 0) {
        fprintf(stderr, "IR verification failed for %s:\n%s\n",
                my_source_path, error ? error : "");
        LLVMDisposeMessage(error); exit(1);
    }
    if (error) LLVMDisposeMessage(error);

    if (flags->emit_ir) {
        char ir[512]; snprintf(ir, sizeof(ir), "%s.ll", base);
        error = NULL; LLVMPrintModuleToFile(ctx.module, ir, &error);
        if (error) LLVMDisposeMessage(error);
        else printf("  wrote IR: %s\n", ir);
    }
    if (flags->emit_asm) {
        char as[512]; snprintf(as, sizeof(as), "%s.s", base);
        char *triple = LLVMGetDefaultTargetTriple();
        LLVMTargetRef tgt; error = NULL;
        LLVMGetTargetFromTriple(triple, &tgt, &error);
        if (error) LLVMDisposeMessage(error);
        LLVMTargetMachineRef mach = LLVMCreateTargetMachine(
            tgt, triple, "generic", "",
            LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);
        char asbuf[512]; strncpy(asbuf, as, 511);
        error = NULL;
        LLVMTargetMachineEmitToFile(mach, ctx.module, asbuf,
                                    LLVMAssemblyFile, &error);
        if (error) LLVMDisposeMessage(error);
        LLVMDisposeTargetMachine(mach); LLVMDisposeMessage(triple);
    }

/// Phase 11: Emit object file (skipped if .o is already up to date)

    if (!skip_emit) {
        if (!emit_object(ctx.module, obj_path)) {
            fprintf(stderr, "failed to emit object for %s\n", my_source_path);
            exit(1);
        }
        printf("  wrote object: %s\n", obj_path);
    }

///// Cleanup

    codegen_dispose(&ctx);
    module_context_free(mod_ctx);
    for (size_t i = 0; i < exprs.count; i++) ast_free(exprs.exprs[i]);
    free(exprs.exprs);
    free(source);
    free(obj_path);
    free(base);
    free(my_source_path);
    type_alias_free_all();  // clear aliases between compilations
    return cm;
}

static void compile(CompilerFlags *flags) {
    compile_one(flags->input_file, flags, true);

    // Collect .o files: registry is prepend (newest first), reverse to get
    // deps first so linker resolves symbols correctly
    size_t n = 0;
    for (CompiledModule *m = g_compiled; m; m = m->next) n++;

    const char **objs = malloc(sizeof(char *) * n);
    size_t idx = n;
    for (CompiledModule *m = g_compiled; m; m = m->next)
        objs[--idx] = m->obj_path;


    char *exec_base = flags->output_name
        ? strdup(flags->output_name)
        : get_base_executable_name(flags->input_file);

    // Append _test suffix for test-run mode
    char *exec_name;
    if (flags->test_run) {
        exec_name = malloc(strlen(exec_base) + 6);
        sprintf(exec_name, "%s_test", exec_base);
        free(exec_base);
    } else {
        exec_name = exec_base;
    }


    char cmd[4096];
    int w = snprintf(cmd, sizeof(cmd), "gcc");
    for (size_t i = 0; i < n; i++)
        w += snprintf(cmd + w, sizeof(cmd) - w, " %s", objs[i]);

    w += snprintf(cmd + w, sizeof(cmd) - w,
                  " -o %s /usr/local/lib/libmonad.a"
                  " `llvm-config --ldflags --libs core` -lm -lgmp -no-pie", exec_name);


    printf("\n[link] %s\n", cmd);
    int rc = system(cmd);
    if (rc == 0) {
        /* printf("[done] %s", exec_name); */
        printf("[done] %s\n", exec_name);
        if (!flags->emit_obj)
            for (size_t i = 0; i < n; i++) remove(objs[i]);
    } else {
        fprintf(stderr, "[error] linking failed\n");
    }

    free(objs);
    free(exec_name);
    registry_free_all();
}

bool repl_compile_module(CodegenContext *ctx, ImportDecl *imp) {
    const char *mod_name = imp->module_name;

    char *src_path = module_name_to_path(mod_name);
    if (!src_path) {
        fprintf(stderr, "import error: cannot resolve module '%s'\n", mod_name);
        return false;
    }
    if (!file_exists(src_path)) {
        fprintf(stderr, "import error: cannot find '%s' (tried: %s)\n",
                mod_name, src_path);
        free(src_path);
        return false;
    }

    /* Use dummy flags — no special emit, not a main module */
    CompilerFlags flags = {0};
    flags.emit_obj  = true;   /* ensure .o is written so deps link later */
    flags.test_mode = false;

    /* compile_one populates the global g_compiled registry */
    CompiledModule *cm = compile_one(src_path, &flags, false);
    free(src_path);

    if (!cm) {
        fprintf(stderr, "import error: compile_one failed for '%s'\n", mod_name);
        return false;
    }

    /* Inject the module's exports into the REPL's env + current LLVM module */
    declare_externals(ctx, cm, imp);
    return true;
}


int main(int argc, char **argv) {
    CompilerFlags flags = parse_flags(argc, argv);
    switch (flags.mode) {
    case CMD_REPL:    repl_run();                        return 0;
    case CMD_NEW:     cmd_new(flags.package_name);       return 0;
    case CMD_BUILD:   cmd_build();                       return 0;
    case CMD_RUN:     cmd_run();                         return 0;
    case CMD_CLEAN:   cmd_clean();                       return 0;
    case CMD_INSTALL: cmd_install();                     return 0;
    case CMD_TEST:    cmd_test(flags.input_file);        return 0;
    case CMD_COMPILE:
    default:
        compile(&flags);
        return 0;
    }
}
