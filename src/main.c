#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#if defined(_WIN32)
#include <stdlib.h>
#endif

#include "reader.h"
#include "reader_syntax.h"
#include "macro.h"
#include "compat.h"
#include "cli.h"
#include "types.h"
#include "env.h"
#include "repl.h"
#include "features.h"
#include "runtime.h"
#include "codegen.h"
#include "module.h"
#include "typeclass.h"
#include "buildsystem.h"
#include "ffi.h"
#include "wisp.h"
#include "spirv.h"
#include "dep.h"
#include "typst_emit.h"
#include "optimizations.h"
#include "bytecode.h"
#include "tooling/lint.h"
#include "tooling/format.h"
#include "qtt/usage.h"
#include "qtt/bindings.h"
#include "qtt/compiler.h"
#include "qtt/interface.h"

#include <llvm-c/Core.h>
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
    AST          *source_ast;    // FUNC only — core-owned implementation metadata
    char         *hm_scheme;     // FUNC only — portable principal type scheme
    int           adt_tag;       // ADT constructor only
} CompiledExport;

typedef struct CompiledLayout {
    char *name;
    Type *type;
} CompiledLayout;

typedef struct CompiledModule {
    char           *module_name;
    char           *obj_path;
    bool            was_skipped;    // compiled from .o timestamp, no LLVMValueRef
    bool            object_reused;  // linked object predates this compiler run
    bool            persistent_core;/* immutable Core metadata kept by batch */
    bool            compiling;      // reserved during recursive dependency scan
    CompiledExport *exports;
    size_t          export_count;
    size_t          export_cap;
    CompiledLayout *layouts;
    size_t          layout_count;
    size_t          layout_cap;
    TypeClassRegistry *tc_registry;
    QttInterface      *qtt_interface;
    struct CompiledModule *next;
} CompiledModule;

static CompiledModule *g_compiled = NULL;
/* ``monad batch`` is the one mode whose process lifetime spans independent
 * compilation transactions.  Keep this separate from CompilerFlags so the
 * ordinary CLI and REPL retain their strict per-transaction ownership. */
static bool g_batch_mode = false;

/* Return the number of fields exposed by an FFI layout name.  Clang records
 * typedef'd record names as alias entries with zero fields, while the named
 * record appears separately in the same context.  Wisp needs the effective
 * constructor arity before layout injection, so follow record aliases here.
 */
static int ffi_wisp_layout_arity(const FFIContext *ffi, const FFIStruct *layout) {
    if (!ffi || !layout) return 0;
    if (layout->field_count > 0) return layout->field_count;
    const char *target = layout->alias_of;
    for (int depth = 0; target && depth < ffi->struct_count; depth++) {
        const FFIStruct *next = NULL;
        for (int i = 0; i < ffi->struct_count; i++) {
            const FFIStruct *candidate = &ffi->structs[i];
            if (candidate->name && strcmp(candidate->name, target) == 0) {
                next = candidate;
                break;
            }
        }
        if (!next) return 0;
        if (next->field_count > 0) return next->field_count;
        target = next->alias_of;
    }
    return 0;
}

/* Populate Wisp's frontend arity registry from include directives in one
 * source unit.  Dependency compilation intentionally clears that registry
 * when it finishes, so callers may replay this small pre-pass immediately
 * before parsing the unit that owns the source. */
static void register_source_ffi_arities(const char *source) {
    if (!source) return;
    FFIContext *ffi = ffi_context_create();
    if (!ffi) return;
    const char *p = source;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *q = p;
        bool system_inc = false;
        const char *hstart = NULL;
        const char *hend = NULL;
        if (strncmp(q, "include", 7) == 0 &&
            (q[7] == ' ' || q[7] == '\t' || q[7] == '<')) {
            q += 7;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '<') {
                system_inc = true;
                hstart = q + 1;
                hend = strchr(hstart, '>');
            } else if (*q == '"') {
                hstart = q + 1;
                hend = strchr(hstart, '"');
            }
        } else if (*q == '(' && strncmp(q + 1, "include", 7) == 0) {
            q += 8;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '<') {
                system_inc = true;
                hstart = q + 1;
                hend = strchr(hstart, '>');
            } else if (*q == '"') {
                hstart = q + 1;
                hend = strchr(hstart, '"');
            }
        }
        if (hstart && hend) {
            char header[256];
            size_t length = (size_t)(hend - hstart);
            if (length < sizeof(header)) {
                memcpy(header, hstart, length);
                header[length] = '\0';
                ffi_parse_header(ffi, header, system_inc);
            }
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    for (int fi = 0; fi < ffi->function_count; fi++)
        wisp_register_arity(ffi->functions[fi].name, ffi->functions[fi].param_count);
    for (int si = 0; si < ffi->struct_count; si++) {
        int arity = ffi_wisp_layout_arity(ffi, &ffi->structs[si]);
        if (arity > 0)
            wisp_register_arity(ffi->structs[si].name, arity);
    }
    ffi_context_free(ffi);
}

/* The dependent checker is currently a shadow pass; typeclass applications
 * are checked by HM inference and resolved by the typeclass registry.  Do not
 * reinterpret a class method such as Enum.succ as the dependent Nat
 * constructor with the same spelling. */
static bool ast_list_contains_typeclass_call(AST *ast, TypeClassRegistry *registry) {
    if (!ast || ast->type != AST_LIST) return false;
    if (ast->list.count > 0 && ast->list.items[0]->type == AST_SYMBOL &&
        tc_is_method(registry, ast->list.items[0]->symbol)) {
        return true;
    }
    for (size_t i = 0; i < ast->list.count; i++) {
        if (ast_list_contains_typeclass_call(ast->list.items[i], registry)) return true;
    }
    return false;
}

static void trace_qtt_toplevel(AST *ast) {
    if (!ast || ast->type != AST_LIST || ast->list.count < 3)
        return;
    AST *head = ast->list.items[0];
    AST *name = ast->list.items[1];
    AST *value = ast->list.items[2];
    if (!head || head->type != AST_SYMBOL ||
        strcmp(head->symbol, "define") != 0 ||
        !name || name->type != AST_SYMBOL ||
        !value || value->type != AST_LAMBDA)
        return;

    QttUsageReport *report = qtt_usage_analyze_lambda(value);
    if (!report) return;
    for (size_t i = 0; i < qtt_usage_report_count(report); i++) {
        printf("[qtt] %s/%s#%llu = %s\n",
               name->symbol,
               qtt_usage_report_name(report, i),
               (unsigned long long)qtt_usage_report_binder_id(report, i),
               qtt_usage_report_format(report, i));
    }
    qtt_usage_report_free(report);
}

/* FFI libraries to link, accumulated during compile_one */
static char  g_ffi_link_libs[2048] = {0};
static int   g_ffi_link_libs_len   = 0;
static const char *g_program_path = NULL;

static void ffi_libs_add(FFIContext *ffi) {
    for (int i = 0; i < ffi->included_count; i++) {
        const char *hdr  = ffi->included[i];
        const char *base = strrchr(hdr, '/');
        base = base ? base + 1 : hdr;
        char stem[256];
        strncpy(stem, base, sizeof(stem) - 1);
        stem[sizeof(stem)-1] = '\0';
        char *dot = strrchr(stem, '.');
        if (dot) *dot = '\0';

        /* If stem is a generic name like the header filename without version,
         * also try the parent directory name as stem.
         * e.g. /usr/include/SDL3/SDL.h -> try "SDL3" before "SDL" */
        char dir_stem[256] = {0};
        if (base > hdr + 1) {
            const char *dir_end = base - 1; /* points at '/' before filename */
            const char *dir_start = dir_end;
            while (dir_start > hdr && *(dir_start-1) != '/') dir_start--;
            size_t dlen = dir_end - dir_start;
            if (dlen > 0 && dlen < sizeof(dir_stem)) {
                memcpy(dir_stem, dir_start, dlen);
                dir_stem[dlen] = '\0';
            }
        }
        /* Try progressively shorter stems by stripping trailing digits
         * e.g. glfw3 -> glfw, gl2 -> gl, so headers like glfw3.h and
         * gl2.h find their actual libraries libglfw.so and libGL.so */
        const char *sufx[] = {".so",".so.5",".so.4",".so.3",".so.2",".so.1",NULL};
        bool found = false;
        char try_stem[256];
        strncpy(try_stem, stem, sizeof(try_stem) - 1);
        try_stem[sizeof(try_stem)-1] = '\0';

        /* Try parent directory name first (e.g. SDL3 from SDL3/SDL.h),
         * then original stem, then progressively strip trailing digits. */
        if (dir_stem[0] && strcmp(dir_stem, stem) != 0) {
            for (int s = 0; sufx[s] && !found; s++) {
                char soname[512];
                snprintf(soname, sizeof(soname), "/usr/lib/lib%s%s", dir_stem, sufx[s]);
                if (access(soname, F_OK) == 0) { found = true; strncpy(stem, dir_stem, sizeof(stem)-1); break; }
                snprintf(soname, sizeof(soname), "/usr/local/lib/lib%s%s", dir_stem, sufx[s]);
                if (access(soname, F_OK) == 0) { found = true; strncpy(stem, dir_stem, sizeof(stem)-1); break; }
            }
        }

        /* Try original stem first, then progressively strip trailing digits.
         * e.g. SDL3 finds libSDL3.so before trying libSDL.so
         *      glfw3 doesn't find libglfw3.so so strips to libglfw.so      */
        while (!found && try_stem[0]) {
            for (int s = 0; sufx[s] && !found; s++) {
                char soname[512];
                snprintf(soname, sizeof(soname), "/usr/lib/lib%s%s", try_stem, sufx[s]);
                if (access(soname, F_OK) == 0) { found = true; break; }
                snprintf(soname, sizeof(soname), "/usr/local/lib/lib%s%s", try_stem, sufx[s]);
                if (access(soname, F_OK) == 0) { found = true; break; }
            }
            if (found) {
                strncpy(stem, try_stem, sizeof(stem) - 1);
                break;
            }
            /* Strip one trailing digit and retry */
            size_t slen = strlen(try_stem);
            if (slen > 1 && try_stem[slen-1] >= '0' && try_stem[slen-1] <= '9')
                try_stem[slen-1] = '\0';
            else
                break;
        }


        /* Check the lib actually exists before adding */
        if (!found) {
            /* fprintf(stderr, "ffi_libs_add: no lib found for stem '%s' (from '%s')\n", stem, hdr); */
            continue;
        }
        /* fprintf(stderr, "ffi_libs_add: adding -l%s (from '%s')\n", stem, hdr); */

        /* Avoid duplicates */
        char flag[280];
        snprintf(flag, sizeof(flag), " -l%s", stem);
        if (strstr(g_ffi_link_libs, flag)) continue;
        g_ffi_link_libs_len += snprintf(g_ffi_link_libs + g_ffi_link_libs_len,
                                         sizeof(g_ffi_link_libs) - g_ffi_link_libs_len,
                                         "%s", flag);
    }
}

static const char *registry_path_tail(const char *name)
{
    const char *slash = strrchr(name, '/');
    const char *backslash = strrchr(name, '\\');
    const char *last = !slash || (backslash && backslash > slash)
        ? backslash : slash;
    return last ? last + 1 : name;
}

static bool registry_path_equal(const char *left, const char *right)
{
    if (!left || !right) return false;
    while (*left && *right) {
        bool left_sep = *left == '/' || *left == '\\';
        bool right_sep = *right == '/' || *right == '\\';
        if (left_sep && right_sep) {
            left++;
            right++;
            continue;
        }
        if (*left++ != *right++) return false;
    }
    return *left == '\0' && *right == '\0';
}

static CompiledModule *registry_find(const char *name) {
    if (!name) return NULL;

    const char *name_dot = strrchr(name, '.');
    const char *name_tail_dot = name_dot ? name_dot + 1 : name;
    const char *name_tail_slash = registry_path_tail(name);

    for (CompiledModule *m = g_compiled; m; m = m->next) {
        if (!m->module_name) continue;

        const char *mn = m->module_name;
        const char *mn_dot = strrchr(mn, '.');
        const char *mn_tail_dot = mn_dot ? mn_dot + 1 : mn;
        const char *mn_tail_slash = registry_path_tail(mn);

        if (strcmp(mn, name) == 0) return m;
        if (strcmp(mn_tail_dot, name) == 0) return m;
        if (strcmp(mn_tail_slash, name) == 0) return m;
        if (strcmp(mn, name_tail_dot) == 0) return m;
        if (strcmp(mn, name_tail_slash) == 0) return m;
        if (strcmp(mn_tail_dot, name_tail_dot) == 0) return m;
        if (strcmp(mn_tail_slash, name_tail_slash) == 0) return m;
    }

    return NULL;
}

static CompiledModule *registry_find_by_obj(const char *obj_path) {
    for (CompiledModule *m = g_compiled; m; m = m->next)
        if (registry_path_equal(m->obj_path, obj_path)) return m;
    return NULL;
}

/* Top-level method validation does not belong here.
 * In this compiler, Wisp/reader lower method syntax before codegen.
 * There is no ExprArray type and no AST_METHOD node in main.c.
 */

static CompiledModule *registry_new(const char *name, const char *obj,
                                     bool skipped) {
    CompiledModule *m = calloc(1, sizeof(CompiledModule));
    m->module_name  = strdup(name);
    m->obj_path     = strdup(obj);
    m->was_skipped  = skipped;
    m->compiling    = skipped;
    m->export_cap   = 8;
    m->exports      = malloc(sizeof(CompiledExport) * 8);
    m->layout_cap   = 8;
    m->layouts      = malloc(sizeof(CompiledLayout) * 8);
    m->tc_registry  = tc_registry_create();
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

static Type *registry_function_return_type(Type *t) {
    if (!t)
        return type_unknown();

    while (t && t->kind == TYPE_ARROW)
        t = t->arrow_ret;

    return t ? t : type_unknown();
}

static bool registry_type_is_unresolved(Type *type) {
    return !type || type->kind == TYPE_UNKNOWN || type->kind == TYPE_VAR;
}

static Type *registry_resolve_user_type(Env *env, const char *name) {
    Type *layout = env_lookup_layout(env, name);
    if (layout) return layout;

    /* codegen_data also records the result layout on every constructor.
     * Consult those entries because a same-named value can shadow the
     * standalone ENV_LAYOUT binding in the hash environment. */
    for (Env *scope = env; scope; scope = scope->parent) {
        for (size_t i = 0; i < scope->size; i++) {
            for (EnvEntry *entry = scope->buckets[i]; entry; entry = entry->next) {
                Type *type = entry->return_type ? entry->return_type : entry->type;
                if (entry->kind == ENV_ADT_CTOR && type &&
                    type->kind == TYPE_LAYOUT && type->layout_name &&
                    strcmp(type->layout_name, name) == 0)
                    return type;
            }
        }
    }
    return NULL;
}




static void registry_push_func(CompiledModule *m, const char *local,
                                const char *mangled, Type *ret,
                                EnvParam *params, int pc, LLVMValueRef fn,
                                AST *source_ast, EnvEntryKind kind,
                                int adt_tag, Env *provider_env,
                                TypeScheme *provider_scheme) {
    registry_grow(m);
    CompiledExport *e = &m->exports[m->export_count++];
    memset(e, 0, sizeof(*e));

    Type *abi_ret = registry_function_return_type(ret);
    AST *lambda = source_ast && source_ast->type == AST_LAMBDA
        ? source_ast : NULL;
    if (!lambda && source_ast && source_ast->type == AST_LIST) {
        for (size_t i = 0; i < source_ast->list.count; i++) {
            AST *candidate = source_ast->list.items[i];
            if (candidate && candidate->type == AST_LAMBDA) {
                lambda = candidate;
                break;
            }
        }
    }
    if (lambda && lambda->lambda.return_type &&
        registry_type_is_unresolved(abi_ret)) {
        Type *resolved = registry_resolve_user_type(
            provider_env, lambda->lambda.return_type);
        if (resolved) abi_ret = resolved;
    }

    e->local_name   = strdup(local);
    e->mangled_name = strdup(mangled);
    e->kind         = kind;
    e->adt_tag      = adt_tag;
    e->return_type  = type_clone(abi_ret);
    e->param_count  = pc;
    e->func_ref     = fn;
    e->source_ast   = ast_clone(source_ast);
    if (provider_scheme)
        e->hm_scheme = infer_type_scheme_serialize(provider_scheme);
    if (pc > 0 && params) {
        e->params = malloc(sizeof(EnvParam) * pc);
        for (int i = 0; i < pc; i++) {
            e->params[i].name = strdup(params[i].name ? params[i].name : "_");
            Type *param_type = params[i].type;
            if (lambda && i < lambda->lambda.param_count &&
                lambda->lambda.params[i].type_name &&
                registry_type_is_unresolved(param_type)) {
                Type *resolved = registry_resolve_user_type(
                    provider_env, lambda->lambda.params[i].type_name);
                if (resolved) param_type = resolved;
            }
            e->params[i].type = type_clone(param_type ? param_type
                                                      : type_unknown());
        }
    }
    if (getenv("MONAD_MODULE_DEBUG")) {
        fprintf(stderr, "[module-export] %s kind=%d return=%s", local,
                (int)kind, type_to_string(e->return_type));
        for (int i = 0; i < e->param_count; i++)
            fprintf(stderr, " param%d=%s", i,
                    type_to_string(e->params[i].type));
        fprintf(stderr, "\n");
    }
}

static void register_compiled_module_wisp_arities(CompiledModule *m) {
    if (!m)
        return;

    const char *module_name = m->module_name ? m->module_name : "";
    const char *module_tail = module_name;

    {
        const char *dot = strrchr(module_tail, '.');
        if (dot && dot[1]) module_tail = dot + 1;
    }

    {
        const char *slash = strrchr(module_tail, '/');
        if (slash && slash[1]) module_tail = slash + 1;
    }

    for (size_t i = 0; i < m->export_count; i++) {
        CompiledExport *e = &m->exports[i];
        if (e->kind != ENV_FUNC || !e->local_name)
            continue;

        wisp_register_arity(e->local_name, e->param_count);

        if (module_tail[0]) {
            char qualified_tail[512];
            snprintf(qualified_tail, sizeof(qualified_tail), "%s.%s",
                     module_tail, e->local_name);
            wisp_register_arity(qualified_tail, e->param_count);
        }

        if (module_name[0] && strcmp(module_name, module_tail) != 0) {
            char qualified_full[512];
            snprintf(qualified_full, sizeof(qualified_full), "%s.%s",
                     module_name, e->local_name);
            wisp_register_arity(qualified_full, e->param_count);
        }
    }
}

static void registry_free_module(CompiledModule *m) {
    if (!m) return;
    free(m->module_name);
    free(m->obj_path);
    for (size_t i = 0; i < m->export_count; i++) {
        CompiledExport *e = &m->exports[i];
        free(e->local_name);
        free(e->mangled_name);
        type_free(e->type);
        type_free(e->return_type);
        ast_free(e->source_ast);
        free(e->hm_scheme);
        if (e->params) {
            for (int j = 0; j < e->param_count; j++) {
                free(e->params[j].name);
                type_free(e->params[j].type);
            }
            free(e->params);
        }
    }
    free(m->exports);
    for (size_t i = 0; i < m->layout_count; i++) {
        free(m->layouts[i].name);
        type_free(m->layouts[i].type);
    }
    free(m->layouts);
    tc_registry_free(m->tc_registry);
    qtt_interface_free(m->qtt_interface);
    free(m);
}

static void registry_free_all(bool retain_core) {
    CompiledModule *m = g_compiled;
    CompiledModule *retained = NULL;
    while (m) {
        CompiledModule *next = m->next;
        if (retain_core && m->persistent_core) {
            /* LLVMValueRef handles belong to the just-disposed module and are
             * never valid in the next transaction.  Imported code is linked
             * from the persistent object path; only the portable export
             * metadata survives here. */
            for (size_t i = 0; i < m->export_count; i++)
                m->exports[i].func_ref = NULL;
            if (m->tc_registry) {
                for (int i = 0; i < m->tc_registry->instance_count; i++) {
                    TCInstance *instance = &m->tc_registry->instances[i];
                    /* These handles belong to the disposed LLVM context;
                     * method_symbols remain the stable cross-module ABI. */
                    instance->dict_global = NULL;
                    for (int j = 0; j < instance->method_count; j++)
                        instance->method_funcs[j] = NULL;
                }
            }
            m->object_reused = true;
            m->compiling = false;
            m->next = retained;
            retained = m;
        } else {
            registry_free_module(m);
        }
        m = next;
    }
    g_compiled = retained;
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
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open file: %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = malloc(sz + 1);
    size_t n = fread(src, 1, sz, f);
    src[n] = '\0'; fclose(f);
    return src;
}

static time_t file_mtime(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) ? st.st_mtime : 0;
}

static uint64_t file_content_fingerprint(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned char buffer[8192];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
        for (size_t i = 0; i < count; i++) {
            hash ^= buffer[i];
            hash *= UINT64_C(1099511628211);
        }
    bool valid = !ferror(file);
    fclose(file);
    return valid ? (hash ? hash : 1) : 0;
}

static const char *host_exe_suffix(void) {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    return ".exe";
#else
    return "";
#endif
}

static const char *host_no_pie_flag(void) {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    return "";
#else
    return " -no-pie";
#endif
}

static bool file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

/* Return whether NAME can be resolved through the host PATH.  This is kept
 * deliberately small and side-effect free because it runs for every native
 * fixture link.  The linker itself still validates that the selected tool can
 * consume the target objects. */
static bool host_command_exists(const char *name) {
    if (!name || !*name)
        return false;
    const char *path = getenv("PATH");
    if (!path || !*path)
        return false;

    const char *cursor = path;
    while (*cursor) {
        const char *sep = strchr(cursor, ':');
        size_t dir_len = sep ? (size_t)(sep - cursor) : strlen(cursor);
        if (dir_len == 0)
            dir_len = 1; /* an empty PATH component means the current dir */

        char candidate[1024];
        if (dir_len + 1 + strlen(name) + 1 <= sizeof(candidate)) {
            if (dir_len == 1 && cursor[0] == ':')
                snprintf(candidate, sizeof(candidate), "./%s", name);
            else
                snprintf(candidate, sizeof(candidate), "%.*s/%s",
                         (int)dir_len, cursor, name);
            if (access(candidate,
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
                       F_OK
#else
                       X_OK
#endif
                       ) == 0)
                return true;
        }
        if (!sep)
            break;
        cursor = sep + 1;
    }
    return false;
}

/* lld is substantially faster for Monad's many-object fixture links, but it
 * is not available on every supported host.  Prefer it when installed and
 * retain an explicit MONAD_LINKER=bfd escape hatch for toolchain debugging.
 * MONAD_LINKER=lld makes the choice explicit and lets clang report a useful
 * error if that linker is unavailable. */
static const char *host_linker_flag(void) {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    const char *requested = getenv("MONAD_LINKER");
    return (requested && strcmp(requested, "lld") == 0)
        ? " -fuse-ld=lld" : "";
#else
    const char *requested = getenv("MONAD_LINKER");
    if (requested && strcmp(requested, "bfd") == 0)
        return "";
    if ((requested && strcmp(requested, "lld") == 0) ||
        (!requested && host_command_exists("ld.lld")))
        return " -fuse-ld=lld";
    return "";
#endif
}

static bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void ensure_cache_dir(const char *home)
{
    if (!home || !*home)
        return;

    char dir[1024];
    monad_mkdir(home);
    snprintf(dir, sizeof(dir), "%s/.cache", home);
    monad_mkdir(dir);
    snprintf(dir, sizeof(dir), "%s/.cache/monad", home);
    monad_mkdir(dir);
    snprintf(dir, sizeof(dir), "%s/.cache/monad/core", home);
    monad_mkdir(dir);
}

static char *host_realpath(const char *path, char *resolved)
{
#if defined(_WIN32)
    return _fullpath(resolved, path, 1024);
#else
    return realpath(path, resolved);
#endif
}

static char *llvm_config_link_flags(void)
{
    FILE *pipe = popen("llvm-config --ldflags --libs core orcjit native passes", "r");
    if (!pipe)
        return strdup("");

    char buf[1024];
    size_t used = 0;
    while (used + 1 < sizeof(buf)) {
        size_t n = fread(buf + used, 1, sizeof(buf) - used - 1, pipe);
        used += n;
        if (n == 0)
            break;
    }
    pclose(pipe);
    buf[used] = '\0';

    for (size_t i = 0; i < used; i++) {
        if (buf[i] == '\r' || buf[i] == '\n')
            buf[i] = ' ';
    }
    while (used > 0 && (buf[used - 1] == ' ' || buf[used - 1] == '\t'))
        buf[--used] = '\0';
    return strdup(buf);
}

static bool dir_prefix_matches(const char *path, const char *dir) {
    if (!path || !dir || !*dir)
        return false;

    size_t dir_len = strlen(dir);
    if (strncmp(path, dir, dir_len) == 0 &&
        (path[dir_len] == '\0' || path[dir_len] == '/' || path[dir_len] == '\\'))
        return true;

    char path_real[1024];
    char dir_real[1024];
    if (host_realpath(path, path_real) && host_realpath(dir, dir_real)) {
        size_t real_len = strlen(dir_real);
        return strncmp(path_real, dir_real, real_len) == 0 &&
               (path_real[real_len] == '\0' ||
                path_real[real_len] == '/' ||
                path_real[real_len] == '\\');
    }

    return false;
}

static bool same_dir_path(const char *a, const char *b) {
    if (!a || !b)
        return false;
    if (strcmp(a, b) == 0)
        return true;

    char a_real[1024];
    char b_real[1024];
    if (host_realpath(a, a_real) && host_realpath(b, b_real))
        return strcmp(a_real, b_real) == 0;
    return false;
}

static char *dirname_dup(const char *path) {
    if (!path || !*path)
        return strdup(".");

    char *copy = strdup(path);
    char *last_sep = strrchr(copy, '/');
    char *last_backslash = strrchr(copy, '\\');
    if (!last_sep || (last_backslash && last_backslash > last_sep))
        last_sep = last_backslash;

    if (!last_sep) {
        free(copy);
        return strdup(".");
    }

    if (last_sep == copy)
        last_sep[1] = '\0';
    else
        *last_sep = '\0';
    return copy;
}

static char *path_join_dup(const char *dir, const char *leaf) {
    size_t dir_len = strlen(dir);
    bool needs_sep = dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\';
    char *out = malloc(dir_len + (needs_sep ? 1 : 0) + strlen(leaf) + 1);
    sprintf(out, "%s%s%s", dir, needs_sep ? "/" : "", leaf);
    return out;
}

static char *monad_core_dir(void) {
    const char *env_core = getenv("MONAD_CORE");
    if (env_core && *env_core)
        return strdup(env_core);

    if (dir_exists("core"))
        return strdup("core");

    if (g_program_path) {
        char *bin_dir = dirname_dup(g_program_path);
        char *beside_binary = path_join_dup(bin_dir, "core");
        if (dir_exists(beside_binary)) {
            free(bin_dir);
            return beside_binary;
        }
        free(beside_binary);

        char *build_tree = path_join_dup(bin_dir, "../core");
        if (dir_exists(build_tree)) {
            free(bin_dir);
            return build_tree;
        }
        free(build_tree);

        char *installed = path_join_dup(bin_dir, "../lib/monad/core");
        free(bin_dir);
        if (dir_exists(installed))
            return installed;
        free(installed);
    }

    /* PATH invocation commonly leaves argv[0] as only `monad`. A per-user
     * installation still has a deterministic sibling Core under ~/.local,
     * matching the runtime archive lookup below. */
    const char *user_home = getenv("HOME");
    if (user_home && *user_home) {
        char *local_core = path_join_dup(user_home, ".local/lib/monad/core");
        if (dir_exists(local_core))
            return local_core;
        free(local_core);
    }

    return strdup("/usr/local/lib/monad/core");
}

static char *runtime_archive_path(void) {
    const char *env_runtime = getenv("MONAD_RUNTIME_LIB");
    if (env_runtime && *env_runtime)
        return strdup(env_runtime);

    if (file_exists("libmonad.a"))
        return strdup("libmonad.a");

    if (g_program_path) {
        char *bin_dir = dirname_dup(g_program_path);
        char *beside_binary = path_join_dup(bin_dir, "libmonad.a");
        if (file_exists(beside_binary)) {
            free(bin_dir);
            return beside_binary;
        }
        free(beside_binary);

        char *installed = path_join_dup(bin_dir, "../lib/libmonad.a");
        free(bin_dir);
        if (file_exists(installed))
            return installed;
        free(installed);
    }

    /* A per-user installation normally places the compiler in ~/.local/bin
     * and the static runtime in ~/.local/lib.  argv[0] may be only `monad`
     * when the shell resolved it through PATH, so g_program_path alone cannot
     * locate the sibling archive. */
    const char *user_home = getenv("HOME");
    if (user_home && *user_home) {
        char *local_lib = path_join_dup(user_home, ".local/lib/libmonad.a");
        if (file_exists(local_lib))
            return local_lib;
        free(local_lib);
    }

    return strdup("/usr/local/lib/libmonad.a");
}

static char *base_no_ext(const char *path) {
    char *b = strdup(path);
    char *dot = strrchr(b, '.');
    if (dot) *dot = '\0';
    return b;
}

static char *path_to_module_name(const char *path) {
    char *b = base_no_ext(path);

    /* Strip leading ./ and ../ segments */
    char *start = b;
    while (true) {
        if (start[0] == '.' && start[1] == '/') {
            start += 2;
        } else if (start[0] == '.' && start[1] == '.' && start[2] == '/') {
            start += 3;
        } else {
            break;
        }
    }

    /* Core namespaces may intentionally reuse a leaf name: for example,
     * System.Posix.Socket is the raw ABI seam beneath Network.Socket.  Using
     * only the final capitalized path component aliases both registry keys to
     * "Socket" and silently suppresses the second dependency.  Preserve the
     * declared namespace for non-prelude System and Network modules. */
    const char *core_system = strstr(start, "core/System/");
    const char *core_network = strstr(start, "core/Network/");
    if (core_system || core_network) {
        char *namespaced = core_system
            ? (char *)core_system + strlen("core/")
            : (char *)core_network + strlen("core/");
        for (char *p = namespaced; *p; p++)
            if (*p == '/') *p = '.';
        char *result = strdup(namespaced);
        free(b);
        return result;
    }

    for (char *p = start; *p; p++) if (*p == '/') *p = '.';

    /* If the result contains dots (path components like "src.Int"),
     * use only the last component as the module name when it starts
     * with an uppercase letter — type method files are always named
     * after their type (Int.mon -> "Int") regardless of directory.  */
    char *last_dot = strrchr(start, '.');
    char *r;
    if (last_dot && (last_dot[1] >= 'A' && last_dot[1] <= 'Z')) {
        r = strdup(last_dot + 1);
    } else {
        r = strdup(start);
    }
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

static LLVMCodeGenOptLevel codegen_opt_level(int level) {
    if (level <= 0) return LLVMCodeGenLevelNone;
    if (level == 1) return LLVMCodeGenLevelLess;
    return LLVMCodeGenLevelAggressive;
}

static bool emit_object(LLVMModuleRef mod, const char *obj_path, int opt_level) {
    char *triple = LLVMGetDefaultTargetTriple();
    char *error  = NULL;
    LLVMTargetRef target;
    if (LLVMGetTargetFromTriple(triple, &target, &error) != 0) {
        fprintf(stderr, "target error: %s\n", error);
        LLVMDisposeMessage(error); LLVMDisposeMessage(triple); return false;
    }
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target, triple, "generic", "",
        codegen_opt_level(opt_level), LLVMRelocPIC, LLVMCodeModelDefault);
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

static const QttInterfaceContract *compiled_qtt_contract(
    const CompiledModule *module, const char *name) {
    if (!module || !module->qtt_interface || !name) return NULL;
    for (size_t i = 0;
         i < qtt_interface_count(module->qtt_interface); i++) {
        const QttInterfaceContract *contract =
            qtt_interface_contract(module->qtt_interface, i);
        if (contract && strcmp(contract->name, name) == 0)
            return contract;
    }
    return NULL;
}

static bool register_interface_effect_judgment(
    const char *identity, const char *name,
    const QttInterfaceContract *contract) {
    if (!contract || contract->interface_version < 12) return true;
    if (!contract->has_effect_judgment) return false;
    size_t count = contract->effect_predicate_count;
    size_t *stages = count ? malloc(count * sizeof(*stages)) : NULL;
    const char **names = count ? malloc(count * sizeof(*names)) : NULL;
    if (count && (!stages || !names)) {
        free(stages); free(names); return false;
    }
    for (size_t i = 0; i < count; i++) {
        stages[i] = contract->effect_predicates[i].stage;
        names[i] = contract->effect_predicates[i].trait;
    }
    bool registered = qtt_compiler_register_effect_judgment(
        identity, name, contract->effect_row_fingerprint,
        contract->effect_constraint_fingerprint,
        (int)contract->effect_constraint_result,
        stages, names, count);
    free(stages);
    free(names);
    return registered;
}

static bool declare_externals(CodegenContext *ctx,
                               CompiledModule *dep,
                               ImportDecl *import) {
    if (dep && dep->qtt_interface) {
        for (size_t i = 0;
             i < qtt_interface_effect_declaration_count(dep->qtt_interface);
             i++) {
            const QttEffectDeclaration *declaration =
                qtt_interface_effect_declaration(dep->qtt_interface, i);
            if (!qtt_effect_declaration_register(declaration)) {
                fprintf(stderr,
                        "conflicting imported effect declaration: %s\n",
                        declaration && declaration->name
                            ? declaration->name : "<invalid>");
                return false;
            }
        }
        for (size_t i = 0;
             i < qtt_interface_handler_profile_count(dep->qtt_interface);
             i++) {
            const QttEffectHandlerProfile *profile =
                qtt_interface_handler_profile(dep->qtt_interface, i);
            if (!qtt_effect_handler_profile_register(profile)) {
                fprintf(stderr,
                        "conflicting imported effect-handler profile: %s\n",
                        profile && profile->name ? profile->name : "<invalid>");
                return false;
            }
        }
        for (size_t i = 0;
             i < qtt_interface_trait_implication_count(dep->qtt_interface);
             i++) {
            const QttEffectTraitImplication *edge =
                qtt_interface_trait_implication(dep->qtt_interface, i);
            if (!edge || !qtt_effect_trait_implication_register(
                    edge->premise, edge->consequence)) {
                fprintf(stderr,
                    "invalid imported effect-trait implication from %s\n",
                    edge && edge->provenance ? edge->provenance : "<unknown>");
                return false;
            }
        }
    }
    /* Re-register all layouts from the imported module first so that
     * field access on imported types works in the importing module. */
    for (size_t i = 0; i < dep->layout_count; i++) {
        if (!env_lookup_layout(ctx->env, dep->layouts[i].name))
            env_insert_layout(ctx->env, dep->layouts[i].name,
                              type_clone(dep->layouts[i].type), NULL);
    }

    tc_registry_merge(ctx->tc_registry, dep->tc_registry);

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
            /* Constructor metadata is also a reliable declaration of its
             * result ADT.  Register that layout before function signatures
             * are materialized so imported clients can name the sum type and
             * preserve its pointer ABI even when the standalone layout entry
             * was filtered or shadowed in the provider environment. */
            if (e->kind == ENV_ADT_CTOR && e->return_type &&
                e->return_type->kind == TYPE_LAYOUT &&
                e->return_type->layout_name &&
                !env_lookup_layout(ctx->env, e->return_type->layout_name)) {
                env_insert_layout(ctx->env, e->return_type->layout_name,
                                  type_clone(e->return_type), NULL);
            }
            /* ADT constructor objects are compiled against their principal
             * polymorphic scheme.  Export parameter metadata may have been
             * specialized by a provider-side use (for example Right Bool),
             * but rebuilding an imported declaration from that specialization
             * conflicts with the stable erased constructor ABI. */
            TypeScheme *ctor_scheme = (e->kind == ENV_ADT_CTOR && e->hm_scheme)
                ? infer_type_scheme_deserialize(e->hm_scheme) : NULL;
            Type *scheme_cursor = ctor_scheme ? ctor_scheme->type : NULL;
            LLVMTypeRef *pt = e->param_count > 0
                ? malloc(sizeof(LLVMTypeRef) * e->param_count) : NULL;
            for (int j = 0; j < e->param_count; j++) {
                Type *abi_param = e->params[j].type;
                if (scheme_cursor && scheme_cursor->kind == TYPE_ARROW) {
                    abi_param = scheme_cursor->arrow_param;
                    scheme_cursor = scheme_cursor->arrow_ret;
                }
                pt[j] = type_to_llvm(ctx, abi_param);
            }
            LLVMTypeRef fnt = LLVMFunctionType(
                type_to_llvm(ctx, e->return_type), pt, e->param_count, 0);
            if (pt) free(pt);
            scheme_free(ctor_scheme);
            LLVMValueRef fn = LLVMGetNamedFunction(ctx->module, e->mangled_name);
            if (!fn) {
                fn = LLVMAddFunction(ctx->module, e->mangled_name, fnt);
                LLVMSetLinkage(fn, LLVMExternalLinkage);
            }
            env_insert_func(ctx->env, qn,
                            clone_params(e->params, e->param_count),
                            e->param_count, type_clone(e->return_type), fn, NULL, NULL);
            EnvEntry *ent = env_lookup(ctx->env, qn);
            if (e->kind == ENV_ADT_CTOR) {
                env_insert_adt_ctor(ctx->env, qn, e->adt_tag,
                                    type_clone(e->return_type), fn);
                ent = env_lookup(ctx->env, qn);
            }
            if (ent) { ent->module_name = strdup(dep->module_name);
                       ent->llvm_name   = strdup(e->mangled_name);
                       env_entry_set_source_ast(ent, ast_clone(e->source_ast), true); }
            if (e->hm_scheme)
                (void)env_set_portable_scheme(ctx->env, qn, e->hm_scheme);
            const QttInterfaceContract *qtt_contract =
                compiled_qtt_contract(dep, e->local_name);
            if (qtt_contract && qtt_contract->hm_scheme)
                (void)env_install_hm_scheme(
                    ctx->env, qn, qtt_contract->hm_scheme);
            if (qtt_contract && qtt_contract->has_ownership_signature)
                (void)qtt_compiler_register_contract(
                    parser_get_filename(), qn,
                    dep->module_name, e->local_name,
                    &qtt_contract->signature);
            bool installed_qtt_callable =
                qtt_contract && qtt_contract->callable_contract &&
                env_install_callable_contract(
                    ctx->env, qn, qtt_contract->callable_contract,
                    qtt_contract->callable_contract_fingerprint,
                    qtt_contract->hm_scheme);
            if (installed_qtt_callable) {
                bool registered_callable =
                    qtt_compiler_register_callable_contract(
                    parser_get_filename(), qn,
                    qtt_contract->callable_contract,
                    qtt_contract->callable_contract_fingerprint);
                bool registered_judgment = !registered_callable ||
                    register_interface_effect_judgment(
                        parser_get_filename(), qn, qtt_contract);
                if (registered_callable && !registered_judgment) {
                    fprintf(stderr,
                        "incoherent imported effect judgment: %s.%s\n",
                        dep->module_name, e->local_name);
                    return false;
                }
                if (qtt_contract->hm_scheme)
                    (void)qtt_compiler_register_hm_scheme(
                        parser_get_filename(), qn,
                        qtt_contract->hm_scheme);
            }
            if (installed_qtt_callable && qtt_compiler_trace_detailed())
                printf("[effects] imported callable contract %s.%s as %s "
                       "fingerprint=%016llx\n",
                       dep->module_name, e->local_name, qn,
                       (unsigned long long)
                           qtt_contract->callable_contract_fingerprint);
            if ((!qtt_contract || !qtt_contract->has_ownership_signature) &&
                e->source_ast && !dep->object_reused)
                qtt_compiler_register_import(
                    parser_get_filename(), qn,
                    dep->module_name, e->local_name,
                    e->source_ast);

            if (import->mode != IMPORT_QUALIFIED) {
                env_insert_func(ctx->env, e->local_name,
                                clone_params(e->params, e->param_count),
                                e->param_count, type_clone(e->return_type), fn, NULL, NULL);
                EnvEntry *ent2 = env_lookup(ctx->env, e->local_name);
                if (e->kind == ENV_ADT_CTOR) {
                    env_insert_adt_ctor(ctx->env, e->local_name, e->adt_tag,
                                        type_clone(e->return_type), fn);
                    ent2 = env_lookup(ctx->env, e->local_name);
                }
                if (ent2) { ent2->module_name = strdup(dep->module_name);
                            ent2->llvm_name   = strdup(e->mangled_name);
                            env_entry_set_source_ast(ent2, ast_clone(e->source_ast), true); }
                if (e->hm_scheme)
                    (void)env_set_portable_scheme(
                        ctx->env, e->local_name, e->hm_scheme);
                if (qtt_contract && qtt_contract->has_ownership_signature)
                    (void)qtt_compiler_register_contract(
                        parser_get_filename(), e->local_name,
                        dep->module_name, e->local_name,
                        &qtt_contract->signature);
                if (qtt_contract && qtt_contract->hm_scheme)
                    (void)env_install_hm_scheme(
                        ctx->env, e->local_name, qtt_contract->hm_scheme);
                bool installed_unqualified_callable =
                    qtt_contract && qtt_contract->callable_contract &&
                    env_install_callable_contract(
                        ctx->env, e->local_name,
                        qtt_contract->callable_contract,
                        qtt_contract->callable_contract_fingerprint,
                        qtt_contract->hm_scheme);
                if (installed_unqualified_callable) {
                    bool registered_callable =
                        qtt_compiler_register_callable_contract(
                        parser_get_filename(), e->local_name,
                        qtt_contract->callable_contract,
                        qtt_contract->callable_contract_fingerprint);
                    bool registered_judgment = !registered_callable ||
                        register_interface_effect_judgment(
                            parser_get_filename(), e->local_name,
                            qtt_contract);
                    if (registered_callable && !registered_judgment) {
                        fprintf(stderr,
                            "incoherent imported effect judgment: %s.%s\n",
                            dep->module_name, e->local_name);
                        return false;
                    }
                    if (qtt_contract->hm_scheme)
                        (void)qtt_compiler_register_hm_scheme(
                            parser_get_filename(), e->local_name,
                            qtt_contract->hm_scheme);
                }
                if (installed_unqualified_callable &&
                    qtt_compiler_trace_detailed())
                    printf("[effects] imported callable contract %s.%s as %s "
                           "fingerprint=%016llx\n",
                           dep->module_name, e->local_name, e->local_name,
                           (unsigned long long)
                               qtt_contract->callable_contract_fingerprint);
                if ((!qtt_contract ||
                     !qtt_contract->has_ownership_signature) &&
                    e->source_ast && !dep->object_reused)
                    qtt_compiler_register_import(
                        parser_get_filename(), e->local_name,
                        dep->module_name, e->local_name,
                        e->source_ast);
            }
        }
    }
    return true;
}

static char *get_obj_path(const char *source_path, bool is_main_module,
                          bool test_mode) {
    const char *home = getenv("HOME");

    char *core_prefix = monad_core_dir();
    bool is_core_path = dir_prefix_matches(source_path, core_prefix);

    const char *core_cache_override = getenv("MONAD_CORE_CACHE");
    bool share_prelude_cache = core_cache_override && *core_cache_override &&
        (strstr(source_path, "/prelude/") != NULL ||
         strstr(source_path, "\\prelude\\") != NULL);
    if (is_core_path && (home || share_prelude_cache)) {
        const char *rel = source_path;
        char source_real[1024];
        char core_real[1024];
        if (host_realpath(source_path, source_real) && host_realpath(core_prefix, core_real)) {
            rel = source_real + strlen(core_real);
        } else if (strncmp(source_path, core_prefix, strlen(core_prefix)) == 0) {
            rel = source_path + strlen(core_prefix);
        }
        while (*rel == '/' || *rel == '\\') rel++;
        char *base = base_no_ext(rel);

        // Replace path separators with underscores for flat cache layout.
        for (char *p = base; *p; p++) if (*p == '/' || *p == '\\') *p = '_';

        char cache_dir[1024];
        if (share_prelude_cache) {
            snprintf(cache_dir, sizeof(cache_dir), "%s", core_cache_override);
            monad_mkdir(cache_dir);
        } else {
            snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/monad/core", home);
            ensure_cache_dir(home);
        }

        char obj[1024];
        if (is_main_module) {
            snprintf(obj, sizeof(obj), "%s/%s%s.o", cache_dir, base,
                     test_mode ? ".test" : "");
        } else {
            snprintf(obj, sizeof(obj), "%s/%s%s.module.o", cache_dir, base,
                     test_mode ? ".test" : "");
        }
        free(base);
        free(core_prefix);
        return strdup(obj);
    }
    free(core_prefix);

    // Normal case
    char *base = base_no_ext(source_path);
    const char *suffix = is_main_module
        ? (test_mode ? ".test.o" : ".o")
        : (test_mode ? ".test.module.o" : ".module.o");
    char *obj = malloc(strlen(base) + strlen(suffix) + 1);
    sprintf(obj, "%s%s", base, suffix);
    free(base);
    return obj;
}

/*
 * Core objects are a persistent compiler cache, not disposable linker
 * temporaries.  get_obj_path() places both imported and directly compiled
 * core modules here; deleting them after a successful link leaves .mqti
 * metadata pointing at artifacts which no longer exist.
 */
static bool is_persistent_core_object(const char *path) {
    const char *home = getenv("HOME");
    const char *override = getenv("MONAD_CORE_CACHE");
    const char *cache = (override && override[0]) ? override : NULL;
    if (!cache && home && home[0]) {
        static char home_cache[1024];
        snprintf(home_cache, sizeof(home_cache), "%s/.cache/monad/core", home);
        cache = home_cache;
    }
    if (!path || !cache || !cache[0]) return false;
    char prefix[1024];
    int length = snprintf(prefix, sizeof(prefix), "%s/", cache);
    return length > 0 && (size_t)length < sizeof(prefix) &&
           strncmp(path, prefix, (size_t)length) == 0;
}

static bool object_has_matching_interface(const char *object_path) {
    if (!object_path) return false;
    size_t length = strlen(object_path);
    char *interface_path = malloc(length + 6);
    if (!interface_path) return false;
    memcpy(interface_path, object_path, length + 1);
    char *extension = strrchr(interface_path, '.');
    if (extension && strcmp(extension, ".o") == 0)
        strcpy(extension, ".mqti");
    else
        strcat(interface_path, ".mqti");

    QttInterfaceError error = QTT_INTERFACE_OK;
    QttInterface *interface = qtt_interface_read(interface_path, &error);
    uint64_t fingerprint = file_content_fingerprint(object_path);
    bool matches = interface && fingerprint &&
        qtt_interface_artifact_fingerprint(interface) == fingerprint;
    qtt_interface_free(interface);
    free(interface_path);
    return matches;
}

/* Load the certified callable fragment of a dependency without its source.
 * The object fingerprint binds executable code to the .mqti evidence; only
 * contracts carrying a complete ownership/ABI signature become linkable
 * exports.  Layouts, macros, typeclasses, and FFI headers remain deliberately
 * unavailable until their own manifests are proof-carrying. */
static CompiledModule *load_certified_binary_module(
    const char *module_name, const char *source_path) {
    if (!module_name || !source_path) return NULL;

    CompiledModule *cached = registry_find(module_name);
    if (cached) return cached;

    char *base = base_no_ext(source_path);
    size_t base_len = strlen(base);
    char *object_path = malloc(base_len + strlen(".module.o") + 1);
    char *interface_path = malloc(base_len + strlen(".module.mqti") + 1);
    if (!object_path || !interface_path) {
        free(base); free(object_path); free(interface_path);
        return NULL;
    }
    sprintf(object_path, "%s.module.o", base);
    sprintf(interface_path, "%s.module.mqti", base);
    free(base);

    if (!file_exists(object_path) || !file_exists(interface_path)) {
        free(object_path); free(interface_path);
        return NULL;
    }

    QttInterfaceError error = QTT_INTERFACE_OK;
    QttInterface *interface = qtt_interface_read(interface_path, &error);
    uint64_t object_fingerprint = file_content_fingerprint(object_path);
    if (!interface || !object_fingerprint ||
        qtt_interface_artifact_fingerprint(interface) != object_fingerprint ||
        !qtt_interface_module(interface) ||
        strcmp(qtt_interface_module(interface), module_name) != 0) {
        fprintf(stderr,
                "error: certified binary interface rejected for module '%s' "
                "(interface error=%d, object binding mismatch)\n",
                module_name, (int)error);
        qtt_interface_free(interface);
        free(object_path); free(interface_path);
        return NULL;
    }

    CompiledModule *module = registry_new(module_name, object_path, true);
    module->object_reused = true;
    module->compiling = false;
    module->qtt_interface = interface;

    for (size_t i = 0; i < qtt_interface_count(interface); i++) {
        const QttInterfaceContract *contract =
            qtt_interface_contract(interface, i);
        if (!contract || !contract->name ||
            !contract->has_ownership_signature)
            continue;

        const QttFunctionSignature *signature = &contract->signature;
        EnvParam *parameters = NULL;
        if (signature->parameter_count > 0) {
            parameters = calloc(signature->parameter_count, sizeof(EnvParam));
            if (!parameters) continue;
            for (size_t p = 0; p < signature->parameter_count; p++) {
                char parameter_name[32];
                snprintf(parameter_name, sizeof(parameter_name), "__p%zu", p);
                parameters[p].name = strdup(parameter_name);
                parameters[p].type = type_clone(
                    (Type *)signature->parameters[p].type);
            }
        }

        char *mangled = mangle(module_name, contract->name);
        registry_push_func(
            module, contract->name, mangled,
            (Type *)(signature->result_type ? signature->result_type
                                            : signature->result.type),
            parameters, (int)signature->parameter_count, NULL, NULL,
            ENV_FUNC, 0, NULL, NULL);
        free(mangled);
        if (parameters) {
            for (size_t p = 0; p < signature->parameter_count; p++) {
                free(parameters[p].name);
                type_free(parameters[p].type);
            }
            free(parameters);
        }
    }

    register_compiled_module_wisp_arities(module);
    free(object_path); free(interface_path);
    return module;
}

static FFIContext *g_ffi = NULL;

static FFIContext *get_global_ffi(void) {
    if (!g_ffi) g_ffi = ffi_context_create();
    return g_ffi;
}

static CompiledModule *compile_one(const char *source_path,
                                    CompilerFlags *flags,
                                    bool is_main_module);

static bool source_is_prelude_file(const char *path)
{
    if (!path)
        return false;

    for (const char *p = path; (p = strstr(p, "prelude")) != NULL; p++) {
        bool left = p == path || p[-1] == '/' || p[-1] == '\\';
        bool right = p[7] == '\0' || p[7] == '/' || p[7] == '\\';
        if (left && right) return true;
    }

    char real[1024];
    if (host_realpath(path, real)) {
        for (const char *p = real; (p = strstr(p, "prelude")) != NULL; p++) {
            bool left = p == real || p[-1] == '/' || p[-1] == '\\';
            bool right = p[7] == '\0' || p[7] == '/' || p[7] == '\\';
            if (left && right) return true;
        }
    }

    return false;
}

/* Core/prelude modules are immutable ABI metadata in a batch worker.  Their
 * reader declarations are retained by the scoped reader registry below; the
 * one known opaque ABI exception (Data.Map) stays transactional.  This keeps
 * the cache conservative while retaining the whole prelude dependency closure. */
static bool source_is_reusable_core_module(const char *path, const char *source)
{
    if (!source_is_prelude_file(path) || !source) return false;
    /* Map helpers expose an intentionally opaque, representation-polymorphic
     * ABI.  Until that ABI is fully serialized in .mqti, rebuild this one
     * primitive per transaction instead of retaining an unsafe signature. */
    const char *tail = strrchr(path, '/');
    if (!tail) tail = strrchr(path, '\\');
    if (tail && (strcmp(tail + 1, "Map.mon") == 0 ||
                 strcmp(tail + 1, "Eq.mon") == 0 ||
                 strcmp(tail + 1, "Ord.mon") == 0 ||
                 strcmp(tail + 1, "Set.mon") == 0 ||
                 strcmp(tail + 1, "Vec.mon") == 0)) return false;
    (void)source;
    /* Reader declarations are retained separately by
     * reader_syntax_clear_noncore(), so even Data.Set can be reused safely. */
    return true;
}

static bool path_is_current_source(const char *path, const char *current)
{
    if (!path || !current) return false;
    if (strcmp(path, current) == 0) return true;

    char path_real[1024];
    char current_real[1024];
    if (host_realpath(path, path_real) && host_realpath(current, current_real))
        return strcmp(path_real, current_real) == 0;
    return false;
}

static bool mon_file_stem(const char *filename, char *out, size_t out_size)
{
    size_t len = strlen(filename);
    if (len <= 4 || strcmp(filename + len - 4, ".mon") != 0)
        return false;
    if (len - 4 >= out_size)
        return false;
    memcpy(out, filename, len - 4);
    out[len - 4] = '\0';
    return true;
}

typedef struct {
    char **items;
    size_t count;
} CoreModuleList;

static CoreModuleList core_primitive_module_stems(void)
{
    CoreModuleList modules = {0};
    char *core_dir = monad_core_dir();
    const char *relative_dirs[] = {"prelude/Data", "prelude"};

    /* Bootstrap ownership is ordinary module metadata.  Discover marked
     * modules from the Core tree itself so a missing side manifest cannot
     * alter or disable the language's public primitive methods. */
    for (size_t di = 0; di < sizeof(relative_dirs) / sizeof(relative_dirs[0]); di++) {
        char dir_path[1024];
        snprintf(dir_path, sizeof(dir_path), "%s/%s", core_dir,
                 relative_dirs[di]);
        DIR *dir = opendir(dir_path);
        if (!dir) continue;

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            char stem[256];
            if (!mon_file_stem(ent->d_name, stem, sizeof(stem)) ||
                !module_name_is_valid(stem))
                continue;

            char source_path[1280];
            snprintf(source_path, sizeof(source_path), "%s/%s", dir_path,
                     ent->d_name);
            FILE *source_file = fopen(source_path, "rb");
            if (!source_file) continue;

            bool marked = false;
            char line[512];
            while (fgets(line, sizeof(line), source_file)) {
                char *start = line;
                while (*start == ' ' || *start == '\t') start++;
                if (strncmp(start, ":bootstrap primitive", 20) == 0 &&
                    (start[20] == '\0' || start[20] == '\r' ||
                     start[20] == '\n' || start[20] == ' ' ||
                     start[20] == '\t')) {
                    marked = true;
                    break;
                }
                /* Metadata belongs in the module header. */
                if (strncmp(start, "module ", 7) == 0 ||
                    strncmp(start, "(module ", 8) == 0)
                    break;
            }
            fclose(source_file);
            if (!marked) continue;

            modules.items = realloc(modules.items,
                                    sizeof(char *) * (modules.count + 1));
            modules.items[modules.count++] = strdup(stem);
        }
        closedir(dir);
    }
    free(core_dir);

    /* readdir order is platform-dependent.  Module dependencies are explicit;
     * lexical discovery order keeps bootstrap reproducible across hosts. */
    for (size_t i = 0; i < modules.count; i++) {
        for (size_t j = i + 1; j < modules.count; j++) {
            if (strcmp(modules.items[i], modules.items[j]) > 0) {
                char *tmp = modules.items[i];
                modules.items[i] = modules.items[j];
                modules.items[j] = tmp;
            }
        }
    }
    return modules;
}

static void core_module_list_free(CoreModuleList *modules)
{
    if (!modules) return;
    for (size_t i = 0; i < modules->count; i++) free(modules->items[i]);
    free(modules->items);
    modules->items = NULL;
    modules->count = 0;
}

static void compile_prelude_dir(const char *dir, const char *current_source,
                                CompilerFlags *flags, const char *source)
{
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        char module_name[256];
        if (!mon_file_stem(ent->d_name, module_name, sizeof(module_name)))
            continue;
        if (!module_name_is_valid(module_name))
            continue;

        char *current_module = path_to_module_name(current_source);
        bool is_current_module =
            current_module && strcmp(module_name, current_module) == 0;
        free(current_module);

        if (is_current_module)
            continue;
        CompiledModule *cached_prelude = registry_find(module_name);
        if (cached_prelude) {
            if (g_batch_mode && cached_prelude->persistent_core &&
                getenv("MONAD_BATCH_TRACE_REUSE"))
                printf("[batch-reuse] %s\n", cached_prelude->module_name);
            register_compiled_module_wisp_arities(cached_prelude);
            continue;
        }

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (!file_exists(path) || path_is_current_source(path, current_source))
            continue;

        CompiledModule *prelude_cm = compile_one(path, flags, false);
        register_compiled_module_wisp_arities(prelude_cm);
        parser_set_context(current_source, source);
    }

    closedir(d);
}

static void compile_prelude_modules(const char *current_source,
                                    CompilerFlags *flags, const char *source)
{
    if (source_is_prelude_file(current_source))
        return;

    char *core_dir = monad_core_dir();
    bool is_core_library = dir_prefix_matches(current_source, core_dir);

    if (is_core_library) {
        const char *p = source;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if ((strncmp(p, "import", 6) == 0 &&
                 (p[6] == ' ' || p[6] == '\t')) ||
                (*p == '(' && strncmp(p + 1, "import", 6) == 0 &&
                 (p[7] == ' ' || p[7] == '\t'))) {
                free(core_dir);
                return;
            }
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/prelude", core_dir);
    compile_prelude_dir(path, current_source, flags, source);
    free(core_dir);
}

static CompiledModule *compile_one(const char *source_path,
                                    CompilerFlags *flags,
                                    bool is_main_module) {

    /* Resolve the checkout/install core once and expose the same path to the
     * module resolver. Without this, prelude discovery can find a core beside
     * build/monad while explicit imports inside that prelude still search the
     * package working directory. */
    if (!getenv("MONAD_CORE")) {
        char source_real[1024];
        char source_core[1024];
        bool source_owns_core = false;
        if (host_realpath(source_path, source_real)) {
            char *prelude = strstr(source_real, "/prelude/");
            if (prelude) {
                size_t length = (size_t)(prelude - source_real);
                if (length > 0 && length < sizeof(source_core)) {
                    memcpy(source_core, source_real, length);
                    source_core[length] = '\0';
                    source_owns_core = dir_exists(source_core);
                }
            }
        }
        if (source_owns_core) {
            monad_setenv("MONAD_CORE", source_core);
        } else {
            char *resolved_core = monad_core_dir();
            if (resolved_core && dir_exists(resolved_core))
                monad_setenv("MONAD_CORE", resolved_core);
            free(resolved_core);
        }
    }

    struct timespec _phase_t0, _phase_t1;
    #define PHASE_START() clock_gettime(CLOCK_MONOTONIC, &_phase_t0)
    #define PHASE_END(name) do { \
        clock_gettime(CLOCK_MONOTONIC, &_phase_t1); \
        double _ms = (_phase_t1.tv_sec - _phase_t0.tv_sec) * 1000.0 + \
                     (_phase_t1.tv_nsec - _phase_t0.tv_nsec) / 1e6; \
        if ((flags->verbose_level > 0 || flags->trace_codegen) && _ms > 50.0) \
            printf("  [phase] %s: %.1f ms\n", name, _ms);   \
    } while(0)

    // Defensively own the path — callers may free dep_src right after returning
    char *my_source_path = strdup(source_path);

    if (flags->jit && !is_main_module) {
        free(my_source_path);
        return NULL;
    }

    // Early-exit if this module is already compiled and registered.
    // This prevents duplicate .o entries from recursive pre-scan calls.
    if (!is_main_module) {
        char *guessed = path_to_module_name(my_source_path);
        CompiledModule *cached = registry_find(guessed);
        free(guessed);
        if (cached) {
            /* Reusing code must also replay the frontend contract.  A fresh
             * module parse still needs dependency function arities even when
             * their object code was compiled earlier in this REPL. */
            register_compiled_module_wisp_arities(cached);
            free(my_source_path);
            return cached;
        }
    }

    char *base     = base_no_ext(my_source_path);
    /* char *obj_path = malloc(strlen(base) + 3); */
    /* sprintf(obj_path, "%s.o", base); */
    char *obj_path = get_obj_path(my_source_path, is_main_module,
                                  flags->test_mode);


    // Incremental check for library modules
    if (!is_main_module) {
        char *guessed = path_to_module_name(my_source_path);
        CompiledModule *cached = registry_find(guessed);
        free(guessed);
        if (cached) {
            for (size_t fi = 0; fi < cached->export_count; fi++)
                if (cached->exports[fi].kind == ENV_FUNC)
                    wisp_register_arity(cached->exports[fi].local_name,
                                        cached->exports[fi].param_count);
            free(base); free(obj_path); free(my_source_path);
            return cached;
        }
    }

    // Reserve this module in the registry immediately by obj_path so any
    // recursive pre-scan calls for the same file bail out before doing any work.
    if (!is_main_module) {
        CompiledModule *_existing = registry_find_by_obj(obj_path);
        if (_existing && (_existing->compiling || !_existing->was_skipped)) {
            /* Already fully compiled — safe to return immediately */
            free(base); free(obj_path); free(my_source_path);
            return _existing;
        }
        if (!_existing) {
            /* Register placeholder so recursive calls see it immediately */
            char *guessed = path_to_module_name(my_source_path);
            registry_new(guessed, obj_path, true);
            free(guessed);
        }
        /* If _existing but was_skipped=true, fall through to run full codegen
         * so exports get populated, then update it in Phase 9. */
    }

    // Check if we can skip emitting a new .o (but we still run full codegen
    // to populate the registry with correct types — it's cheap without emit)
    time_t src_t = file_mtime(my_source_path);

    time_t obj_t = file_mtime(obj_path);
    bool skip_emit = !is_main_module && (obj_t > 0 && obj_t > src_t) &&
                     object_has_matching_interface(obj_path);

    if (flags->verbose_level > 0 || flags->trace_codegen) {
        if (skip_emit)
            printf("[skip]    %s (.o is up to date)\n", my_source_path);
        else
            printf("[compile] %s\n", my_source_path);
    }

    {
        char *_dbg_name = path_to_module_name(my_source_path);
        free(_dbg_name);
    }

    // Read + parse
    char *source = read_file(my_source_path);
    reader_syntax_scope_push(my_source_path);
    macro_scope_push(my_source_path);

    PHASE_START();

    /* Pre-pass: parse FFI includes to populate Wisp constructor/function
     * arities before the full Wisp expansion runs. */
    register_source_ffi_arities(source);
    PHASE_END("ffi pre-pass");

    /* Register builtin arities before wisp parse so forms like
     * define/until/if are known to the arity-driven expander. */
    {
        Env *tmp_env = env_create();
        CodegenContext tmp_ctx = {0};
        tmp_ctx.env = tmp_env;
        register_builtins(&tmp_ctx);
        wisp_register_arities_from_env(tmp_env);
        env_free(tmp_env);
    }

    /* Auto-load primitive type method files (Int.mon, String.mon, etc.).
     * We identify type method files by matching against the canonical set
     * of primitive type names — NOT by capitalization, because user modules
     * like Main.mon also start with uppercase and are not type method files. */
    {
        CoreModuleList primitive_modules = core_primitive_module_stems();

        /* Derive the directory containing my_source_path */
        char src_dir[1024] = ".";
        const char *last_sep = strrchr(my_source_path, '/');
        if (!last_sep) last_sep = strrchr(my_source_path, '\\');
        if (last_sep) {
            size_t dlen = last_sep - my_source_path;
            if (dlen < sizeof(src_dir)) {
                memcpy(src_dir, my_source_path, dlen);
                src_dir[dlen] = '\0';
            }
        }

        bool in_core_data_dir = false;
        {
            char *core_dir = monad_core_dir();
            char data_dir[1024];
            snprintf(data_dir, sizeof(data_dir), "%s/Data", core_dir);
            in_core_data_dir = same_dir_path(src_dir, data_dir);
            if (!in_core_data_dir) {
                snprintf(data_dir, sizeof(data_dir), "%s/prelude/Data", core_dir);
                in_core_data_dir = same_dir_path(src_dir, data_dir);
            }
            free(core_dir);
        }
        if (in_core_data_dir || source_is_prelude_file(my_source_path))
            goto skip_primitive_type_autoload;

        for (size_t _ti = 0; _ti < primitive_modules.count; _ti++) {
            const char *stem = primitive_modules.items[_ti];

            char type_paths[2][1024];
            int type_path_count = 0;
            {
                char *core_dir = monad_core_dir();
                snprintf(type_paths[type_path_count++], sizeof(type_paths[0]),
                         "%s/prelude/Data/%s.mon", core_dir, stem);
                snprintf(type_paths[type_path_count++], sizeof(type_paths[0]),
                         "%s/prelude/%s.mon", core_dir, stem);
                free(core_dir);
            }

            for (int _pi = 0; _pi < type_path_count; _pi++) {
                const char *type_path = type_paths[_pi];

                /* Only proceed if the file actually exists */
                if (!file_exists(type_path)) continue;

                /* Do not auto-load ourselves.
                 * Use both realpath and stem comparison. The realpath check can
                 * miss relative spelling differences during recursive core loads,
                 * so the stem check prevents Tuple.mon from loading ./Tuple.mon
                 * while it is already being compiled. */
                if (path_is_current_source(type_path, my_source_path)) continue;

                {
                    char *current_base = base_no_ext(my_source_path);
                    const char *slash = strrchr(current_base, '/');
                    const char *current_stem = slash ? slash + 1 : current_base;

                    if (strcmp(current_stem, stem) == 0) {
                        if (flags->verbose_level > 0 || flags->trace_codegen) {
                            fprintf(stderr,
                                    "[type-debug] skip self type module stem=%s current=%s candidate=%s\n",
                                    stem, my_source_path, type_path);
                        }
                        free(current_base);
                        continue;
                    }

                    free(current_base);
                }

                /* Skip if already in the registry under any path tail matching stem */
                bool already_done = false;
                for (CompiledModule *_cm = g_compiled; _cm && !already_done; _cm = _cm->next) {
                    const char *mn = _cm->module_name;
                    const char *dot   = strrchr(mn, '.');
                    const char *slash = strrchr(mn, '/');
                    const char *tail_dot   = dot   ? dot   + 1 : mn;
                    const char *tail_slash = slash ? slash + 1 : mn;
                    if (strcmp(mn,         stem) == 0) already_done = true;
                    if (strcmp(tail_dot,   stem) == 0) already_done = true;
                    if (strcmp(tail_slash, stem) == 0) already_done = true;
                }

                if (already_done) continue;

                /* Compile the type method file as a dependency */
                if (flags->verbose_level > 0 || flags->trace_codegen)
                    printf("[type]    %s\n", type_path);
                CompiledModule *type_cm = compile_one(type_path, flags, false);
                register_compiled_module_wisp_arities(type_cm);
                parser_set_context(my_source_path, source);
            }
        }
skip_primitive_type_autoload:
        core_module_list_free(&primitive_modules);
        ;
    }

    /* Pre-scan imports and compile dependencies BEFORE wisp expansion
     * so their FFI arities are available when we expand this module. */
    {
        compile_prelude_modules(my_source_path, flags, source);

        const char *p = source;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            /* Match: (import ModuleName) or (import qualified ModuleName) */
            if (*p == '(' &&
                strncmp(p + 1, "import", 6) == 0 &&
                (p[7] == ' ' || p[7] == '\t')) {
                const char *q = p + 7;
                while (*q == ' ' || *q == '\t') q++;
                if (strncmp(q, "for-syntax", 10) == 0 &&
                    (q[10] == ' ' || q[10] == '\t'))
                    q += 10;
                while (*q == ' ' || *q == '\t') q++;
                /* skip optional 'qualified' */
                if (strncmp(q, "qualified", 9) == 0 &&
                    (q[9] == ' ' || q[9] == '\t'))
                    q += 9;
                while (*q == ' ' || *q == '\t') q++;
                /* read module name: letters, digits, dots */
                const char *mstart = q;
                while (*q && (isalnum((unsigned char)*q) || *q == '.')) q++;
                if (q > mstart) {
                    char mod_name[256];
                    size_t mlen = q - mstart;
                    if (mlen < sizeof(mod_name)) {
                        memcpy(mod_name, mstart, mlen);
                        mod_name[mlen] = '\0';
                        char *dep_src = module_name_to_path(mod_name);
                        if (dep_src && file_exists(dep_src)) {
                            CompiledModule *dep_cm = compile_one(dep_src, flags, false);
                            register_compiled_module_wisp_arities(dep_cm);
                            reader_syntax_scope_allow(dep_src);
                            macro_scope_allow(dep_src);
                            parser_set_context(my_source_path, source);
                            /* dep headers already in global FFI context */
                        } else if (dep_src) {
                            load_certified_binary_module(mod_name, dep_src);
                        }
                        free(dep_src);
                    }
                }
            }
            /* also match bare wisp-style: import ModuleName */
            else if (strncmp(p, "import", 6) == 0 &&
                     (p[6] == ' ' || p[6] == '\t')) {
                const char *q = p + 6;
                while (*q == ' ' || *q == '\t') q++;
                if (strncmp(q, "for-syntax", 10) == 0 &&
                    (q[10] == ' ' || q[10] == '\t'))
                    q += 10;
                while (*q == ' ' || *q == '\t') q++;
                if (strncmp(q, "qualified", 9) == 0 &&
                    (q[9] == ' ' || q[9] == '\t'))
                    q += 9;
                while (*q == ' ' || *q == '\t') q++;
                const char *mstart = q;
                while (*q && (isalnum((unsigned char)*q) || *q == '.')) q++;
                if (q > mstart) {
                    char mod_name[256];
                    size_t mlen = q - mstart;
                    if (mlen < sizeof(mod_name)) {
                        memcpy(mod_name, mstart, mlen);
                        mod_name[mlen] = '\0';
                        char *dep_src = module_name_to_path(mod_name);
                        if (dep_src && file_exists(dep_src)) {
                            CompiledModule *dep_cm = compile_one(dep_src, flags, false);
                            register_compiled_module_wisp_arities(dep_cm);
                            reader_syntax_scope_allow(dep_src);
                            macro_scope_allow(dep_src);
                            parser_set_context(my_source_path, source);
                            /* Re-parse dep's headers into our FFI context
                             * so types like VkApplicationInfo are visible */
                            char *dep_source = read_file(dep_src);
                            FFIContext *dep_ffi = ffi_context_create();
                            const char *dp = dep_source;
                            while (*dp) {
                                while (*dp == ' ' || *dp == '\t') dp++;
                                const char *line = dp;
                                bool is_include = false;
                                if (strncmp(dp, "include", 7) == 0 &&
                                    (dp[7] == ' ' || dp[7] == '\t' || dp[7] == '<'))
                                    is_include = true;
                                if (*dp == '(' && strncmp(dp+1, "include", 7) == 0)
                                    is_include = true;
                                if (is_include) {
                                    const char *q = strchr(dp, '<');
                                    const char *qq = strchr(dp, '"');
                                    bool sys = false;
                                    const char *hstart = NULL, *hend = NULL;
                                    if (q && (!qq || q < qq)) {
                                        sys = true; hstart = q+1;
                                        hend = strchr(hstart, '>');
                                    } else if (qq) {
                                        sys = false; hstart = qq+1;
                                        hend = strchr(hstart, '"');
                                    }
                                    if (hstart && hend) {
                                        char hdr[256];
                                        size_t hl = hend - hstart;
                                        if (hl < sizeof(hdr)) {
                                            memcpy(hdr, hstart, hl);
                                            hdr[hl] = '\0';
                                            ffi_parse_header(dep_ffi, hdr, sys);
                                        }
                                    }
                                }
                                while (*dp && *dp != '\n') dp++;
                                if (*dp == '\n') dp++;
                            }
                            for (int fi = 0; fi < dep_ffi->function_count; fi++)
                                wisp_register_arity(dep_ffi->functions[fi].name,
                                                    dep_ffi->functions[fi].param_count);
                            for (int si = 0; si < dep_ffi->struct_count; si++) {
                                int arity = ffi_wisp_layout_arity(dep_ffi,
                                                                  &dep_ffi->structs[si]);
                                if (arity > 0)
                                    wisp_register_arity(dep_ffi->structs[si].name,
                                                        arity);
                            }
                            ffi_context_free(dep_ffi);
                            free(dep_source);
                        } else if (dep_src) {
                            load_certified_binary_module(mod_name, dep_src);
                        }
                        free(dep_src);
                    }
                }
            }
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    }

    /* Nested dependency compilation may clear the process-local FFI arity
     * table. Replay this module's includes after the dependency scan so its
     * constructors are available to the Wisp parser below. */
    register_source_ffi_arities(source);
    parser_set_context(my_source_path, source);
    AST *_feat_early = detect_features();
    ast_free(_feat_early);
    wisp_set_trace(flags->trace_ast);
    ASTList exprs = wisp_parse_all(source, my_source_path);
    reader_syntax_scope_pop();
    macro_scope_pop();

    /* Reader recovery may deliberately leave a null slot after emitting a
     * structured diagnostic (notably rejected infinite arrays).  Do not send
     * that slot into LLVM, where it would mask the useful reader error with an
     * internal "null AST" failure. */
    for (size_t i = 0; i < exprs.count; i++) {
        if (exprs.exprs[i]) continue;
        for (size_t j = 0; j < exprs.count; j++) ast_free(exprs.exprs[j]);
        free(exprs.exprs);
        free(my_source_path);
        free(source);
        return NULL;
    }

    if (flags->optimization_level > 0) {
        OptimizationOptions opt_options = optimization_options_default();
        opt_options.level = flags->optimization_level >= 2
            ? OPT_LEVEL_AGGRESSIVE
            : OPT_LEVEL_BASIC;
        opt_options.print_stats = flags->verbose_level > 0 && !flags->trace_semantic;
        opt_options.trace_semantic = flags->trace_semantic;
        opt_options.source_name = my_source_path;
        OptimizationStats opt_stats = {0};
        optimize_ast_list(&exprs, &opt_options, &opt_stats);
    }

    /* Surface AST for typst emission — parsed before wisp desugaring.
     * We re-parse from the original source so the emitter sees the
     * user's actual syntax: named params, pattern clauses, clean types.
     * This is cheap — parse_all is a pure read with no codegen side effects. */
    ASTList surface_exprs = {0};
    if (flags->emit_typst) {
        parser_set_context(my_source_path, source);
        surface_exprs = parse_all(source);
    }

    if (flags->trace_ast) {
        /* Show fully desugared AST after all reader transforms */
        fprintf(stderr, "\n=== desugared AST (%s) ===\n", my_source_path);
        for (size_t i = 0; i < exprs.count; i++) {
            ast_print(exprs.exprs[i]);
            printf("\n");
            fflush(stdout);
        }
        fprintf(stderr, "=== end desugared AST ===\n\n");
        fflush(stderr);
    }

    if (is_main_module &&
        (flags->emit_bytecode || flags->bytecode_verify ||
         flags->bytecode_disassemble || flags->bytecode_decompile ||
         flags->bytecode_dump_sections || flags->bytecode_trace ||
         flags->bytecode_baseline_jit)) {
        fprintf(stderr,
                "bytecode: register VM substrate v%u.%u is available; "
                "AST-to-bytecode lowering is not wired into compile yet\n",
                BC_VERSION_MAJOR, BC_VERSION_MINOR);
        if (flags->bytecode_baseline_jit)
            fprintf(stderr, "bytecode: baseline JIT flag accepted; backend lowering is pending\n");
        if (flags->jit) {
            fprintf(stderr, "bytecode: -jit requested; skipping LLVM compile because bytecode lowering is pending\n");
            for (size_t i = 0; i < exprs.count; i++) ast_free(exprs.exprs[i]);
            free(exprs.exprs);
            for (size_t i = 0; i < surface_exprs.count; i++) ast_free(surface_exprs.exprs[i]);
            free(surface_exprs.exprs);
            free(my_source_path);
            free(source);
            type_alias_free_all();
            return NULL;
        }
    }

    if (flags->emit_json) {
        char json_path[512];
        if (flags->output_name)
            snprintf(json_path, sizeof(json_path), "%s.json", flags->output_name);
        else {
            strncpy(json_path, my_source_path, sizeof(json_path) - 6);
            char *dot = strrchr(json_path, '.');
            if (dot) strcpy(dot, ".json");
            else strncat(json_path, ".json", sizeof(json_path) - strlen(json_path) - 1);
        }

        FILE *jf = fopen(json_path, "w");
        if (!jf) { perror(json_path); }
        else {
            fprintf(jf, "[\n");
            for (size_t i = 0; i < exprs.count; i++) {
                char *j = ast_to_json(exprs.exprs[i]);
                fprintf(jf, "  %s%s\n", j, i + 1 < exprs.count ? "," : "");
                free(j);
            }
            fprintf(jf, "]\n");
            fclose(jf);
            if (flags->verbose_level > 0 || flags->trace_ast)
                printf("  wrote json: %s\n", json_path);
        }

        /* JSON-only mode: clean up and stop — no codegen, no binary */
        for (size_t i = 0; i < exprs.count; i++) ast_free(exprs.exprs[i]);
        free(exprs.exprs);
        free(source);
        free(obj_path);
        free(base);
        free(my_source_path);
        wisp_clear_arities();
        type_alias_free_all();
        return NULL;
    }

    if (flags->emit_typst) {
        char typ_path[512];
        if (flags->output_name)
            snprintf(typ_path, sizeof(typ_path), "%s.typ", flags->output_name);
        else {
            strncpy(typ_path, my_source_path, sizeof(typ_path) - 6);
            char *dot = strrchr(typ_path, '.');
            if (dot) strcpy(dot, ".typ");
            else strncat(typ_path, ".typ", sizeof(typ_path) - strlen(typ_path) - 1);
        }

        TypstEmitOpts topts = {
            .mode                  = TYPST_DOC_FULL,
            .prefer_inferred_types = true,
            .sequent_style         = true,
            .emit_labels           = true,
            .number_equations      = false,
            .title                 = my_source_path,
            .author                = "",
            .paper                 = "a4",
        };

        char *pdf_path = NULL;
        int trc = typst_emit_pdf(surface_exprs.exprs, surface_exprs.count,
                                 typ_path, &pdf_path, &topts);
        if (trc == 0)
            printf("  wrote typst: %s  ->  %s\n", typ_path,
                   pdf_path ? pdf_path : "(no pdf)");
        else
            fprintf(stderr, "  typst compile failed (is `typst` on PATH?)\n");
        free(pdf_path);

        for (size_t i = 0; i < surface_exprs.count; i++)
            ast_free(surface_exprs.exprs[i]);
        free(surface_exprs.exprs);
    }

    PHASE_END("wisp+parse");

    if (exprs.count == 0) {
        /* Empty and comment-only files are valid no-op compilation units.
         * Editors routinely invoke the compiler while a buffer is empty;
         * success here is part of the syntax-checker protocol.  Returning
         * NULL tells compile() there is intentionally nothing to link, just
         * like the existing emit-only success paths. */
        free(exprs.exprs);
        free(source);
        free(obj_path);
        free(base);
        free(my_source_path);
        wisp_clear_arities();
        type_alias_free_all();
        return NULL;
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
        /* Scripts need no declaration. A lowercase filename such as
         * `test.mon` is not a legal library namespace, so give it the
         * conventional anonymous executable module instead of diagnosing
         * an invalid declaration and only then falling back. */
        module_decl = module_name_is_valid(guessed)
            ? module_decl_create(guessed, EXPORT_ALL)
            : module_decl_create("Main", EXPORT_ALL);
        free(guessed);
        module_context_set_decl(mod_ctx, module_decl);
    }
    module_context_add_prelude_imports(mod_ctx);
    const char *mod_name = module_decl->name;
    if (flags->verbose_level > 0 || flags->trace_codegen)
        printf("  module: %s\n", mod_name);

    /* Top-level method validation is not performed here. */

///// Phase 2: Verify dependencies are compiled

    // (pre-scan already did the work)
    for (size_t i = 0; i < mod_ctx->import_count; i++) {
        ImportDecl *imp = mod_ctx->imports[i];
        if (!registry_find(imp->module_name)) {
            // Fallback: pre-scan missed it (e.g. non-standard path)
            char *dep_src = module_name_to_path(imp->module_name);
            if (!file_exists(dep_src)) {
                if (!load_certified_binary_module(imp->module_name, dep_src)) {
                    fprintf(stderr,
                            "error: cannot find source or certified binary module '%s' "
                            "(tried: %s)",
                            imp->module_name, dep_src);
                    free(dep_src); exit(1);
                }
            } else {
                compile_one(dep_src, flags, false);
            }
            parser_set_context(my_source_path, source);
            free(dep_src);
        }
    }

/// Phase 3: LLVM setup

    PHASE_START();
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();

    CodegenContext ctx;
    codegen_init(&ctx, mod_name);
    ctx.module_ctx = mod_ctx;
    ctx.test_mode  = flags->test_mode;
    ctx.optimization_level = flags->optimization_level;
    ctx.ffi        = get_global_ffi();

    DepCtx *dep_ctx = dep_ctx_create(my_source_path);
    dep_register_builtins(dep_ctx);
    ctx.env->dep_ctx = dep_ctx;

    PHASE_END("llvm init");
    PHASE_START();
    register_builtins(&ctx);
    wisp_register_arities_from_env(ctx.env);
    declare_runtime_functions(&ctx);
    PHASE_END("llvm init + builtins");

/// Phase 4: Declare externals from compiled deps

    for (size_t i = 0; i < mod_ctx->import_count; i++) {
        ImportDecl *imp = mod_ctx->imports[i];
        CompiledModule *dep = registry_find(imp->module_name);
        if (!dep) {
            fprintf(stderr, "internal error: '%s' not in registry after compile\n",
                    imp->module_name); exit(1);
        }
        if (!imp->for_syntax && !declare_externals(&ctx, dep, imp)) {
            fprintf(stderr, "failed to declare imports from '%s'\n",
                    imp->module_name);
            exit(1);
        }

        /* Headers from imported modules are already in the global FFI context
         * (parsed when that module was compiled) — nothing to do here. */
    }

    /* Auto-declare primitive type method modules (Int, String, etc.).
     * The canonical list is owned by the core manifest. For each stem,
     * find its compiled module in the registry and call declare_externals
     * so that Int.double etc. are visible in the current module's env.  */
    {
        CoreModuleList primitive_modules = core_primitive_module_stems();

        for (size_t _ti = 0; _ti < primitive_modules.count; _ti++) {
            const char *stem2 = primitive_modules.items[_ti];

            /* Find the compiled module for this primitive type */
            CompiledModule *type_mod = NULL;
            for (CompiledModule *_cm = g_compiled; _cm; _cm = _cm->next) {
                const char *mn = _cm->module_name;
                /* Match exact name, or tail after last dot (e.g. "src.Int" -> "Int"),
                 * or tail after last slash (defensive, for any un-normalised paths) */
                const char *dot   = strrchr(mn, '.');
                const char *slash = strrchr(mn, '/');
                const char *tail_dot   = dot   ? dot   + 1 : mn;
                const char *tail_slash = slash ? slash + 1 : mn;
                if (strcmp(mn, stem2) == 0 ||
                    strcmp(tail_dot,   stem2) == 0 ||
                    strcmp(tail_slash, stem2) == 0) {
                    type_mod = _cm;
                    break;
                }
            }

            if (!type_mod) continue;

            /* Already declared via explicit import? Skip. */
            bool already = false;
            for (size_t ii = 0; ii < mod_ctx->import_count; ii++) {
                const char *imn = mod_ctx->imports[ii]->module_name;
                const char *slash = strrchr(imn, '/');
                const char *tail  = slash ? slash + 1 : imn;
                if (strcmp(tail, stem2) == 0 || strcmp(imn, stem2) == 0) {
                    already = true; break;
                }
            }
            if (already) continue;

            /* Force the module_name on the registry entry to the bare stem
             * so declare_externals builds "Int.double" not "src/Int.double" */
            char *old_mn = type_mod->module_name;
            type_mod->module_name = (char *)stem2;  /* temporary, not freed */

            ImportDecl *syn = import_decl_create(stem2, NULL, IMPORT_UNQUALIFIED);
            if (!declare_externals(&ctx, type_mod, syn)) {
                fprintf(stderr, "failed to declare core imports from '%s'\n",
                        stem2);
                exit(1);
            }
            import_decl_free(syn);

            type_mod->module_name = old_mn;  /* restore */
        }
        core_module_list_free(&primitive_modules);
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
    ctx.top_level_fn = init_fn;

    if (!is_main_module) {
        /* Module initialization is a graph operation, not a main-module
         * convention. Guard every library initializer so diamonds and
         * explicit+transitive imports remain idempotent. */
        char guard_name[320];
        snprintf(guard_name, sizeof(guard_name), "%s_guard", init_name);
        LLVMTypeRef i1 = LLVMInt1TypeInContext(ctx.context);
        LLVMValueRef guard = LLVMAddGlobal(ctx.module, i1, guard_name);
        LLVMSetInitializer(guard, LLVMConstInt(i1, 0, 0));
        LLVMSetLinkage(guard, LLVMInternalLinkage);

        LLVMBasicBlockRef init_body = LLVMAppendBasicBlockInContext(
            ctx.context, init_fn, "initialize");
        LLVMBasicBlockRef already_initialized = LLVMAppendBasicBlockInContext(
            ctx.context, init_fn, "already_initialized");
        LLVMValueRef initialized = LLVMBuildLoad2(
            ctx.builder, i1, guard, "initialized");
        LLVMBuildCondBr(ctx.builder, initialized, already_initialized,
                        init_body);
        LLVMPositionBuilderAtEnd(ctx.builder, already_initialized);
        LLVMBuildRetVoid(ctx.builder);
        LLVMPositionBuilderAtEnd(ctx.builder, init_body);
        LLVMBuildStore(ctx.builder, LLVMConstInt(i1, 1, 0), guard);
    }

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
    Type *kw_t = type_keyword();
    Type *ft = type_list(&kw_t, 1);
    LLVMTypeRef flt = type_to_llvm(&ctx, ft);
    LLVMValueRef fgv = LLVMAddGlobal(ctx.module, flt, "__features__");
    LLVMSetInitializer(fgv, LLVMConstNull(flt));
    LLVMSetLinkage(fgv, LLVMInternalLinkage);
    LLVMBuildStore(ctx.builder, feat_list, fgv);
    env_insert(ctx.env, "*features*", ft, fgv);

/// Phase 6.5: Dependent Type Checking (Shadow Pass)

    g_trace_enabled = flags->trace_dep;
    if (flags->verbose_level > 0 || flags->trace_dep)
        printf("[dep] running bidirectional type checker...\n");
    bool dep_failed = false;

    /* Recursive dependency compilation clears the transient type-name
     * registry. Republish this unit's nominal declarations here, after all
     * imports, so both dependent annotations and codegen layout fields retain
     * the ADT identity established while parsing this source. Coverage is
     * likewise declaration-order independent: publish every local ADT's
     * constructor family before checking any function body. */
    for (size_t i = first_code; i < exprs.count; i++)
        if (exprs.exprs[i] && exprs.exprs[i]->type == AST_DATA) {
            if (!type_nominal_register(exprs.exprs[i]->data.name)) {
                fprintf(stderr, "%s:%d: error: could not register data type '%s'\n",
                        my_source_path, exprs.exprs[i]->line,
                        exprs.exprs[i]->data.name);
                exit(1);
            }
            dep_register_data_type(dep_ctx, exprs.exprs[i]);
            AST *data = exprs.exprs[i];
            for (int ctor = 0; ctor < data->data.constructor_count; ctor++) {
                ASTDataConstructor *constructor = &data->data.constructors[ctor];
                if (constructor->name && constructor->type_signature)
                    env_hm_register_type(ctx.env, constructor->name,
                                         type_from_name(constructor->type_signature));
            }
        }

    for (size_t i = first_code; i < exprs.count; i++) {
        AST *expr = exprs.exprs[i];


        // Skip module/import nodes for the type checker
        if (expr->type == AST_LIST && expr->list.count > 0 && expr->list.items[0]->type == AST_SYMBOL) {
            const char *h = expr->list.items[0]->symbol;
            if (strcmp(h, "module") == 0 || strcmp(h, "import") == 0) continue;
        }

        if (ast_list_contains_typeclass_call(expr, ctx.tc_registry)) continue;

        Term *out_type = NULL;
        Term *elaborated = dep_toplevel(dep_ctx, expr, &out_type);

        if (!elaborated || dep_ctx->had_error) {
            dep_failed = true;
            break; // Stop checking on first error
        }

        // Uncomment to see the elaborator's beautiful proofs!
        // printf("  [dep] ok: ");
        // dep_print_term(elaborated);
        // printf(" : ");
        // dep_print_term(out_type);
        // printf("\n");

        term_free(elaborated);
        term_free(out_type);
    }

    if (dep_failed) {
        dep_error_print(dep_ctx);
        fprintf(stderr, "Compilation halted due to dependent type errors.\n");
        dep_ctx_free(dep_ctx);
        exit(1);
    }

    /* Recursive module/type work may change the reader's process-global
     * diagnostic context. Restore this compilation unit before HM/QTT
     * registration so portable contracts use the same module identity as
     * the source pipeline and interface writer. */
    parser_set_context(my_source_path, source);

/// Phase 6.75: Quantitative ownership analysis

    qtt_compiler_set_user_module(is_main_module);
    if (qtt_compiler_trace_enabled() && !flags->trace_qtt)
        printf("[qtt] running quantitative ownership analysis...\n");
    QttBindingSummary qtt_bindings =
        qtt_bindings_resolve_many(exprs.exprs, exprs.count);
    if (qtt_bindings.error != QTT_BINDINGS_OK) {
        if (qtt_bindings.error == QTT_BINDINGS_DUPLICATE_BINDER &&
            qtt_bindings.duplicate_name) {
            fprintf(stderr,
                    "%s: error: quantitative binder resolution failed: "
                    "duplicate binder '%s'\n",
                    my_source_path, qtt_bindings.duplicate_name);
        } else {
            fprintf(stderr,
                    "%s: error: quantitative binder resolution failed "
                    "(code %d, after %llu binder(s))\n",
                    my_source_path, (int)qtt_bindings.error,
                    (unsigned long long)qtt_bindings.binder_count);
        }
        dep_ctx_free(dep_ctx);
        exit(1);
    }

    if (is_main_module &&
        (qtt_compiler_trace_detailed())) {
        for (size_t i = first_code; i < exprs.count; i++)
            trace_qtt_toplevel(exprs.exprs[i]);
    }
    QttCompilerPipelineResult qtt_pipeline =
        qtt_compiler_run_source_pipeline(
            my_source_path, exprs.exprs + first_code,
            exprs.count - first_code);
    if (qtt_pipeline.status == QTT_COMPILER_PIPELINE_INTERNAL_ERROR &&
        qtt_compiler_trace_detailed())
        fprintf(stderr,
                "[qtt] internal verification failure; "
                "using conservative code generation\n");
/// Phase 7: Codegen top-level expressions

    PHASE_START();
    /* Nominal layouts are declarations, not order-sensitive computations.
     * Register them before function predeclaration so annotated parameters
     * and returns receive their real ABI even in large grouped modules. */
    for (size_t i = 0; i < exprs.count; i++) {
        if (exprs.exprs[i]->type == AST_LAYOUT ||
            exprs.exprs[i]->type == AST_TYPE_SET)
            (void)codegen_expr(&ctx, exprs.exprs[i]);
    }
    codegen_predeclare_toplevel_functions(&ctx, exprs.exprs, exprs.count,
                                           first_code);

    // Every module initializes its direct runtime imports before evaluating
    // its own top-level stores. Per-module guards make the transitive graph
    // safe under diamonds and repeated direct imports.
    {
        for (size_t i = 0; i < mod_ctx->import_count; i++) {
            ImportDecl *imp = mod_ctx->imports[i];
            if (imp->for_syntax) continue;
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
        if (expr->type == AST_LAYOUT || expr->type == AST_TYPE_SET)
            continue; /* registered in the nominal-declaration pass above */
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
    qtt_compiler_trace_module(my_source_path);

    PHASE_END("codegen");

/// Phase 8: Terminate function

    if (is_main_module) {
        LLVMValueRef rc = LLVMConstInt(LLVMInt32TypeInContext(ctx.context), 0, 0);
        if (last.value && last.type && type_is_integer(last.type))
            rc = LLVMBuildTrunc(ctx.builder, last.value,
                                LLVMInt32TypeInContext(ctx.context), "rc");
        LLVMBuildRet(ctx.builder, rc);
    } else {
        LLVMBuildRetVoid(ctx.builder);

        const char *tail = strrchr(mod_name, '.');
        if (tail && tail[1]) {
            char tail_init_name[256];
            snprintf(tail_init_name, sizeof(tail_init_name), "__init_%s", tail + 1);
            for (char *p = tail_init_name; *p; p++) {
                if (*p == '.') *p = '_';
            }

            if (strcmp(tail_init_name, init_name) != 0 &&
                !LLVMGetNamedFunction(ctx.module, tail_init_name)) {
                LLVMTypeRef vt = LLVMFunctionType(
                    LLVMVoidTypeInContext(ctx.context), NULL, 0, 0);
                LLVMValueRef alias_fn = LLVMAddFunction(ctx.module, tail_init_name, vt);
                LLVMSetLinkage(alias_fn, LLVMExternalLinkage);

                LLVMBasicBlockRef alias_entry =
                    LLVMAppendBasicBlockInContext(ctx.context, alias_fn, "entry");
                LLVMPositionBuilderAtEnd(ctx.builder, alias_entry);

                LLVMBuildCall2(ctx.builder, vt, init_fn, NULL, 0, "");
                LLVMBuildRetVoid(ctx.builder);
            }
        }
    }

/// Phase 9: Build registry entry + rename LLVM symbols to mangled names

    // Find the placeholder we reserved at the top (by obj_path), update it.
    CompiledModule *cm = registry_find_by_obj(obj_path);
    if (cm) {
        free(cm->module_name);
        cm->module_name = strdup(mod_name);
        cm->was_skipped = false;
        cm->object_reused = skip_emit;
        cm->persistent_core = g_batch_mode &&
            source_is_reusable_core_module(my_source_path, source);
        cm->compiling = false;
    } else {
        cm = registry_new(mod_name, obj_path, false);
        cm->persistent_core = g_batch_mode &&
            source_is_reusable_core_module(my_source_path, source);
    }
    if (cm->persistent_core)
        cm->object_reused = true;
    tc_registry_free(cm->tc_registry);
    cm->tc_registry = tc_registry_clone(ctx.tc_registry);
    for (size_t bi = 0; bi < ctx.env->size; bi++) {
        EnvEntry *ent = ctx.env->buckets[bi];
        while (ent) {
            if (ent->kind == ENV_BUILTIN || ent->module_name != NULL ||
                ent->name[0] == '*') { ent = ent->next; continue; }

            /* A class method declaration is registry metadata, not an
             * ordinary linkable function. Instance implementation symbols
             * and dictionaries are emitted separately. */
            if (ent->kind == ENV_FUNC && ent->func_ref == NULL &&
                tc_is_method(ctx.tc_registry, ent->name)) {
                ent = ent->next;
                continue;
            }

            /* Canonicalize the public export name before export filtering.
             *
             * Type-module methods are defined internally as "Char.upcase",
             * but the module export list contains the member name "upcase".
             * Therefore export filtering must accept either spelling:
             * the internal implementation name or the public member name.
             */
            const char *_local_name = ent->name;
            {
                const char *_dot = strrchr(ent->name, '.');
                if (_dot && _dot[1]) _local_name = _dot + 1;
            }

            /* Force-export closure-ABI functions so inner closures work in .so */
            bool force_export = (ent->kind == ENV_FUNC && ent->is_closure_abi);
            /* A constructor belongs to the head constructor of its result
             * type.  For an ordinary ADT that is a TYPE_LAYOUT (Maybe a),
             * while an indexed/GADT result such as Vec (S n) t is retained
             * as TYPE_APP.  Treat both forms alike: exporting Vec must also
             * export [] and '.', just as exporting a non-indexed data type
             * exports all of its constructors. */
            const char *adt_result_name = NULL;
            if (ent->kind == ENV_ADT_CTOR && ent->type) {
                if (ent->type->kind == TYPE_LAYOUT)
                    adt_result_name = ent->type->layout_name;
                else if (ent->type->kind == TYPE_APP)
                    adt_result_name = ent->type->app_constructor;
            }
            bool exported_adt_constructor =
                adt_result_name &&
                module_decl_is_exported(module_decl, adt_result_name);
            bool exported_adt_accessor =
                ent->kind == ENV_FUNC &&
                strncmp(ent->name, "__field_", 8) == 0 &&
                ent->param_count == 1 && ent->params && ent->params[0].type &&
                ent->params[0].type->kind == TYPE_LAYOUT &&
                ent->params[0].type->layout_name &&
                module_decl_is_exported(module_decl,
                                        ent->params[0].type->layout_name);
            bool exported_type_method = false;
            if (ent->kind == ENV_FUNC) {
                const char *method_dot = strchr(ent->name, '.');
                if (method_dot && method_dot != ent->name) {
                    size_t receiver_len = (size_t)(method_dot - ent->name);
                    char receiver_name[256];
                    if (receiver_len < sizeof(receiver_name)) {
                        memcpy(receiver_name, ent->name, receiver_len);
                        receiver_name[receiver_len] = '\0';
                        exported_type_method =
                            module_decl_is_exported(module_decl, receiver_name);
                    }
                }
            }
            if (!force_export &&
                !exported_adt_constructor &&
                !exported_adt_accessor &&
                !exported_type_method &&
                !module_decl_is_exported(module_decl, ent->name) &&
                !module_decl_is_exported(module_decl, _local_name)) {
                /* Also check if this is an alias for an exported symbol. */
                bool alias_exported = false;
                if (ent->llvm_name) {
                    const char *base = strstr(ent->llvm_name, "__");
                    if (base) base += 2;
                    else base = ent->llvm_name;

                    if (module_decl_is_exported(module_decl, base))
                        alias_exported = true;

                    const char *base_dot = strrchr(base, '.');
                    if (base_dot && base_dot[1] &&
                        module_decl_is_exported(module_decl, base_dot + 1))
                        alias_exported = true;
                }
                if (!alias_exported) { ent = ent->next; continue; }
            }

            /* Nominal layout declarations are type metadata, not linkable
             * value exports. They are recorded in CompiledModule.layouts
             * below. Exporting the same name as an ENV_VAR overwrites the
             * imported ENV_LAYOUT entry and makes the type disappear from
             * client annotations and constructor dispatch. */
            if (ent->kind == ENV_LAYOUT) {
                ent = ent->next;
                continue;
            }

            char *ms = NULL;
            if (ent->kind == ENV_FUNC && ent->func_ref) {
                const char *implementation_name = LLVMGetValueName(ent->func_ref);
                if (implementation_name &&
                    strncmp(implementation_name, "__impl_", 7) == 0) {
                    /* Public class-method entries point at the instance
                     * implementation. Keep that stable symbol: renaming it
                     * to Module__method breaks the instance metadata used by
                     * importing modules. */
                    ms = strdup(implementation_name);
                }
            }
            if (!ms)
                ms = mangle(mod_name, _local_name);

            if (ent->kind == ENV_FUNC || ent->kind == ENV_ADT_CTOR) {
                /* Never rename FFI functions — they must keep their original
                 * symbol names so the linker can find them in libraylib etc. */
                if (ent->is_ffi) { free(ms); ent = ent->next; continue; }
                EnvEntry *export_signature = ent;
                if (ent->func_ref) {
                    const char *implementation_name = LLVMGetValueName(ent->func_ref);
                    if (implementation_name &&
                        strncmp(implementation_name, "__impl_", 7) == 0) {
                        EnvEntry *implementation =
                            env_lookup(ctx.env, implementation_name);
                        if (implementation)
                            export_signature = implementation;
                    }
                }
                registry_push_func(cm, _local_name, ms,
                                   export_signature->return_type
                                       ? export_signature->return_type
                                       : export_signature->type,
                                   export_signature->params,
                                   export_signature->param_count,
                                   ent->func_ref,
                                   export_signature->source_ast,
                                   ent->kind, ent->adt_tag, ctx.env,
                                   export_signature->scheme);
                if (ent->func_ref) {
                    const char *cur = LLVMGetValueName(ent->func_ref);
                    if (!cur || strcmp(cur, ms) != 0)
                        LLVMSetValueName2(ent->func_ref, ms, strlen(ms));
                }
                /* Push aliases for functions too */
                for (size_t bj = 0; bj < ctx.env->size; bj++) {
                    for (EnvEntry *other = ctx.env->buckets[bj]; other; other = other->next) {
                        if (other != ent &&
                            (other->kind == ENV_FUNC ||
                             other->kind == ENV_ADT_CTOR) &&
                            other->module_name == NULL &&
                            other->func_ref == ent->func_ref &&
                            strcmp(other->name, ent->name) != 0) {
                            const char *other_local_name = other->name;
                            const char *other_dot = strrchr(other->name, '.');
                            if (other_dot && other_dot[1]) other_local_name = other_dot + 1;
                            registry_push_func(cm, other_local_name, ms,
                                               ent->return_type ? ent->return_type : ent->type,
                                               ent->params, ent->param_count,
                                               ent->func_ref,
                                               other->source_ast
                                                   ? other->source_ast
                                                   : ent->source_ast,
                                               other->kind, other->adt_tag,
                                               ctx.env,
                                               other->scheme
                                                   ? other->scheme
                                                   : ent->scheme);
                        }
                    }
                }
            } else {
                if (!ent->type) { free(ms); ent = ent->next; continue; }
                registry_push_var(cm, _local_name, ms, ent->type);
                LLVMValueRef gv = ent->value;
                if (gv && LLVMIsAGlobalVariable(gv)) {
                    const char *cur = LLVMGetValueName(gv);
                    if (!cur || strcmp(cur, ms) != 0)
                        LLVMSetValueName2(gv, ms, strlen(ms));
                    LLVMSetLinkage(gv, LLVMExternalLinkage);
                }
                /* Push aliases — entries sharing the same llvm_name */
                for (size_t bj = 0; bj < ctx.env->size; bj++) {
                    for (EnvEntry *other = ctx.env->buckets[bj]; other; other = other->next) {
                        if (other == ent) continue;
                        if (other->kind != ENV_VAR) continue;
                        if (other->module_name != NULL) continue;
                        if (strcmp(other->name, ent->name) == 0) continue;
                        /* Compare by LLVM global name before mangling */
                        const char *other_llvm = other->llvm_name ? other->llvm_name : other->name;
                        const char *ent_llvm   = ent->name; /* before Phase 9 rename, ent->name IS the llvm name */
                        if (strcmp(other_llvm, ent_llvm) == 0) {
                            const char *other_local_name = other->name;
                            const char *other_dot = strrchr(other->name, '.');
                            if (other_dot && other_dot[1]) other_local_name = other_dot + 1;
                            registry_push_var(cm, other_local_name, ms,
                                              other->type ? other->type : ent->type);
                        }
                    }
                }
                for (size_t bj = 0; bj < ctx.env->size; bj++) {
                    EnvEntry *other = ctx.env->buckets[bj];
                    while (other) {
                        if (other != ent &&
                            other->kind == ENV_VAR &&
                            other->module_name == NULL &&
                            other->value == ent->value &&
                            strcmp(other->name, ent->name) != 0) {
                            const char *other_local_name = other->name;
                            const char *other_dot = strrchr(other->name, '.');
                            if (other_dot && other_dot[1]) other_local_name = other_dot + 1;
                            registry_push_var(cm, other_local_name, ms, other->type ? other->type : ent->type);
                        }
                        other = other->next;
                    }
                }
            }
            free(ms);
            ent = ent->next;
        }
    }

    /* Save all ENV_LAYOUT entries into the compiled module registry */
    for (size_t bi = 0; bi < ctx.env->size; bi++) {
        for (EnvEntry *ent = ctx.env->buckets[bi]; ent; ent = ent->next) {
            if (ent->kind != ENV_LAYOUT || !ent->name || !ent->type) continue;
            if (cm->layout_count >= cm->layout_cap) {
                cm->layout_cap *= 2;
                cm->layouts = realloc(cm->layouts,
                                      sizeof(CompiledLayout) * cm->layout_cap);
            }
            cm->layouts[cm->layout_count].name = strdup(ent->name);
            cm->layouts[cm->layout_count].type = type_clone(ent->type);
            cm->layout_count++;
        }
    }

/// Phase 10: Verify + optional IR/asm output

    char *error = NULL;
    if (LLVMVerifyModule(ctx.module, LLVMPrintMessageAction, &error) != 0) {
        fprintf(stderr, "IR verification failed for %s:\n%s\n",
                my_source_path, error ? error : "");
        if (base && *base) {
            char bad_ir[512];
            snprintf(bad_ir, sizeof(bad_ir), "%s.bad.ll", base);
            char *dump_error = NULL;
            if (LLVMPrintModuleToFile(ctx.module, bad_ir, &dump_error) == 0) {
                fprintf(stderr, "wrote invalid IR: %s\n", bad_ir);
            }
            if (dump_error) LLVMDisposeMessage(dump_error);
        }
        LLVMDisposeMessage(error); exit(1);
    }
    if (error) LLVMDisposeMessage(error);

    if (flags->emit_ir) {
        char ir[512]; snprintf(ir, sizeof(ir), "%s.ll", base);
        error = NULL; LLVMPrintModuleToFile(ctx.module, ir, &error);
        if (error) LLVMDisposeMessage(error);
        else if (flags->verbose_level > 0 || flags->trace_codegen)
            printf("  wrote IR: %s\n", ir);
    }
    if (flags->emit_asm) {
        char as[512]; snprintf(as, sizeof(as), "%s.s", base);
        char *triple = LLVMGetDefaultTargetTriple();
        LLVMTargetRef tgt; error = NULL;
        LLVMGetTargetFromTriple(triple, &tgt, &error);
        if (error) LLVMDisposeMessage(error);
        LLVMTargetMachineRef mach = LLVMCreateTargetMachine(
            tgt, triple, "generic", "",
            codegen_opt_level(flags->optimization_level), LLVMRelocPIC, LLVMCodeModelDefault);
        char asbuf[512]; strncpy(asbuf, as, 511);
        error = NULL;
        LLVMTargetMachineEmitToFile(mach, ctx.module, asbuf,
                                    LLVMAssemblyFile, &error);
        if (error) LLVMDisposeMessage(error);
        LLVMDisposeTargetMachine(mach); LLVMDisposeMessage(triple);
    }

/// Phase 11: Emit object file (skipped if .o is already up to date)

    PHASE_START();
    if (!skip_emit) {
        if (!emit_object(ctx.module, obj_path, flags->optimization_level)) {
            fprintf(stderr, "failed to emit object for %s\n", my_source_path);
            exit(1);
        }
        if (flags->verbose_level > 0 || flags->trace_codegen)
            printf("  wrote object: %s\n", obj_path);
    }

    PHASE_END("emit object");

    {
        uint64_t object_fingerprint =
            file_content_fingerprint(obj_path);
        size_t object_length = strlen(obj_path);
        char *interface_path = malloc(object_length + 6);
        if (interface_path) {
            memcpy(interface_path, obj_path, object_length + 1);
            char *extension = strrchr(interface_path, '.');
            if (extension && strcmp(extension, ".o") == 0)
                strcpy(extension, ".mqti");
            else
                strcat(interface_path, ".mqti");
            if (!skip_emit && object_fingerprint &&
                !qtt_compiler_write_interface(
                    my_source_path, mod_name, interface_path,
                    object_fingerprint) &&
                qtt_compiler_trace_detailed())
                printf("[qtt] interface omitted: %s\n",
                       interface_path);
            QttInterfaceError interface_error = QTT_INTERFACE_OK;
            QttInterface *loaded =
                qtt_interface_read(interface_path, &interface_error);
            if (loaded && object_fingerprint &&
                qtt_interface_artifact_fingerprint(loaded) ==
                    object_fingerprint) {
                qtt_interface_free(cm->qtt_interface);
                cm->qtt_interface = loaded;
            } else {
                qtt_interface_free(loaded);
                if (qtt_compiler_trace_detailed())
                    printf("[qtt] interface rejected: %s error=%d "
                           "artifact=%s\n",
                           interface_path, (int)interface_error,
                           object_fingerprint
                               ? "mismatch-or-missing" : "unreadable");
            }
            free(interface_path);
        }
    }

///// Cleanup

    dep_ctx_free(dep_ctx);
    ffi_libs_add(g_ffi);
    ctx.ffi = NULL;  /* don't free the global */
    if (cm->persistent_core) {
        /* LLVM destroys these handles with this module context.  The retained
         * registry is metadata-only and will be imported through its object
         * path by the next batch transaction. */
        for (size_t i = 0; i < cm->export_count; i++)
            cm->exports[i].func_ref = NULL;
    }
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

static bool compile(CompilerFlags *flags) {
    g_ffi_link_libs[0]  = '\0';
    g_ffi_link_libs_len = 0;
    codegen_set_trace(flags->trace_codegen || flags->verbose_level > 0);
    infer_set_trace(flags->trace_dep || flags->verbose_level > 1);
    qtt_compiler_set_trace(flags->verbose_level, flags->trace_qtt);

    CompiledModule *main_mod = compile_one(flags->input_file, flags, true);
    if (!main_mod) {
        /* Emit-only and empty units can still compile/register dependency
         * modules before returning NULL.  They need the same registry teardown
         * as a linked main module, especially in a persistent batch worker. */
        registry_free_all(g_batch_mode);
        qtt_compiler_reset();
        /* A batch worker may compile hundreds of independent modules.  Macro
         * definitions and non-Core reader declarations are transaction-local;
         * retain only the Core reader rules needed by cached prelude modules. */
        macro_clear();
        if (g_batch_mode) {
            char *core_dir = monad_core_dir();
            reader_syntax_clear_noncore(core_dir);
            free(core_dir);
        } else {
            reader_syntax_clear();
        }
        wisp_clear_arities();
        if (g_ffi) { ffi_context_free(g_ffi); g_ffi = NULL; }
#if defined(__GLIBC__)
        /* Return completely free arenas to the OS in persistent batch mode;
         * otherwise glibc keeps peak compilation pages mapped indefinitely. */
        malloc_trim(0);
#endif
        return true;  /* emit-json/JIT mode, no linking needed */
    }

    // Collect .o files: registry is prepend (newest first), reverse to get
    // deps first so linker resolves symbols correctly. Deduplicate by realpath
    // so aliases like "Char.o" and "./Char.o" are linked only once.
    size_t raw_n = 0;
    for (CompiledModule *m = g_compiled; m; m = m->next) raw_n++;

    const char **objs = malloc(sizeof(char *) * (raw_n ? raw_n : 1));
    size_t n = 0;

    for (CompiledModule *m = g_compiled; m; m = m->next) {
        const char *candidate = m->obj_path;
        bool seen = false;

        char candidate_real[1024];
        bool candidate_has_real = host_realpath(candidate, candidate_real) != NULL;

        for (size_t i = 0; i < n; i++) {
            if (strcmp(objs[i], candidate) == 0) {
                seen = true;
                break;
            }

            if (candidate_has_real) {
                char existing_real[1024];
                if (host_realpath(objs[i], existing_real) &&
                    strcmp(candidate_real, existing_real) == 0) {
                    seen = true;
                    break;
                }
            }
        }

        if (!seen) {
            objs[n++] = candidate;
        }
    }

    for (size_t i = 0; i < n / 2; i++) {
        const char *tmp = objs[i];
        objs[i] = objs[n - 1 - i];
        objs[n - 1 - i] = tmp;
    }


    char *exec_base = flags->output_name
        ? strdup(flags->output_name)
        : get_base_executable_name(flags->input_file);

    const char *exe_suffix = host_exe_suffix();
    bool exec_has_exe_suffix = exe_suffix[0] &&
        strlen(exec_base) >= strlen(exe_suffix) &&
        strcmp(exec_base + strlen(exec_base) - strlen(exe_suffix), exe_suffix) == 0;

    // Append _test suffix for test-run mode
    char *exec_name;
    if (flags->test_run) {
        exec_name = malloc(strlen(exec_base) + strlen(exe_suffix) + 6);
        if (exec_has_exe_suffix) {
            size_t stem_len = strlen(exec_base) - strlen(exe_suffix);
            snprintf(exec_name, stem_len + 1, "%s", exec_base);
            sprintf(exec_name + stem_len, "_test%s", exe_suffix);
        } else {
            sprintf(exec_name, "%s_test%s", exec_base, exe_suffix);
        }
        free(exec_base);
    } else {
        if (exec_has_exe_suffix || exe_suffix[0] == '\0') {
            exec_name = exec_base;
        } else {
            exec_name = malloc(strlen(exec_base) + strlen(exe_suffix) + 1);
            sprintf(exec_name, "%s%s", exec_base, exe_suffix);
            free(exec_base);
        }
    }

    /* Prefer lld for the generated executable link when available.  The
     * fallback remains Clang's target-default linker, preserving portability
     * on hosts without lld and allowing MONAD_LINKER=bfd for diagnostics. */
    const char *ld_flag = host_linker_flag();

    char cmd[4096];
    int w = snprintf(cmd, sizeof(cmd), "clang%s", ld_flag);
    for (size_t i = 0; i < n; i++)
        w += snprintf(cmd + w, sizeof(cmd) - w, " %s", objs[i]);

    char *runtime_archive = runtime_archive_path();
    char *llvm_flags = llvm_config_link_flags();
    w += snprintf(cmd + w, sizeof(cmd) - w,
                  " -o %s %s"
                  " %s -lm -lgmp%s%s%s",
                  exec_name, runtime_archive,
                  llvm_flags,
                  host_no_pie_flag(), g_ffi_link_libs,
#ifdef __linux__
                  " -Wl,--no-eh-frame-hdr"
#else
                  ""
#endif
                  );
    free(llvm_flags);


    if (flags->verbose_level > 0 || flags->trace_codegen)
        printf("\n[link] %s\n", cmd);
    struct timespec _lt0, _lt1;
    clock_gettime(CLOCK_MONOTONIC, &_lt0);
    int rc = system(cmd);
    clock_gettime(CLOCK_MONOTONIC, &_lt1);
    double _lms = (_lt1.tv_sec - _lt0.tv_sec) * 1000.0 +
                  (_lt1.tv_nsec - _lt0.tv_nsec) / 1e6;
    if (flags->verbose_level > 0 || flags->trace_codegen)
        printf("[link] %.0f ms\n", _lms);
    if (rc == 0) {
        /* printf("[done] %s", exec_name); */
        if (flags->verbose_level > 0 || flags->trace_codegen)
            printf("[done] %s\n\n", exec_name);
        bool keep_objects = flags->emit_obj || flags->emit_ir ||
                             flags->emit_asm || flags->emit_bc;
        if (!keep_objects)
            for (size_t i = 0; i < n; i++)
                if (!is_persistent_core_object(objs[i]))
                    remove(objs[i]);
        if (flags->run_after_compile) {
            fflush(stdout);
            rc = cmd_run_executable(exec_name);
        }
    } else {
        fprintf(stderr, "[error] linking failed\n");
    }

    free(objs);
    free(exec_name);
    free(runtime_archive);
    registry_free_all(g_batch_mode);
    qtt_compiler_reset();
    wisp_clear_arities();
    macro_clear();
    if (g_batch_mode) {
        char *core_dir = monad_core_dir();
        reader_syntax_clear_noncore(core_dir);
        free(core_dir);
    } else {
        reader_syntax_clear();
    }
    if (g_ffi) { ffi_context_free(g_ffi); g_ffi = NULL; }
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
    return rc == 0;
}

/* Compile independent jobs from stdin without starting a new compiler
 * process for each source file. Each non-empty, non-comment line contains
 * <input.mon><TAB><output-path>[<TAB><HOME>]. Common compiler flags are supplied on
 * the command line; the optional HOME keeps test-worker caches isolated.
 * invocation; this protocol is the foundation for persistent frontend state
 * and gives the test harness a stable linear batch path today. */
void cmd_batch(const CompilerFlags *template_flags)
{
    if (!template_flags) return;

    /* Keep immutable Core metadata at each transaction boundary.  This flag
     * is process-local and cannot affect normal
     * one-shot compilation or REPL ownership rules. */
    g_batch_mode = true;

    char line[16384];
    size_t line_no = 0;
    size_t jobs = 0;
    bool all_ok = true;
    char *batch_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;

    while (fgets(line, sizeof(line), stdin)) {
        line_no++;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;

        char *tab = strchr(line, '\t');
        if (!tab || tab == line || tab[1] == '\0') {
            fprintf(stderr, "batch: line %zu must be <input.mon> TAB <output-path>\n",
                    line_no);
            all_ok = false;
            continue;
        }
        *tab = '\0';
        char *input = line;
        char *output = tab + 1;
        char *job_home = strchr(output, '\t');
        if (job_home) {
            *job_home++ = '\0';
            if (*job_home == '\0') job_home = NULL;
        }

        if (job_home)
            monad_setenv("HOME", job_home);

        CompilerFlags flags = *template_flags;
        flags.input_file = input;
        flags.output_name = output;
        flags.run_after_compile = false;

        bool ok = compile(&flags);
        if (batch_home)
            monad_setenv("HOME", batch_home);
        else
            monad_setenv("HOME", "");
        jobs++;
        if (ok) {
            printf("BATCH OK %s\n", output);
        } else {
            printf("BATCH FAIL %s\n", output);
            all_ok = false;
        }
        fflush(stdout);
    }

    if (ferror(stdin)) {
        fprintf(stderr, "batch: failed while reading stdin\n");
        all_ok = false;
    }
    if (jobs == 0 && all_ok)
        fprintf(stderr, "batch: no jobs received\n");
    if (jobs == 0)
        all_ok = false;
    free(batch_home);
    exit(all_ok ? 0 : 1);
}

bool repl_compile_module(CodegenContext *ctx, ImportDecl *imp) {
    const char *mod_name = imp->module_name;

    /* A REPL can compile many unrelated modules in one process.  Wisp's
     * frontend arity table is compilation-local; retaining the previous
     * module's ordinary identifiers changes how later source and prose are
     * grouped.  Dependencies and builtins are registered again by
     * compile_one, so start every imported module from a clean parser state. */
    wisp_clear_arities();

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
    return declare_externals(ctx, cm, imp);
}

void cmd_eval(const char *code) {
    if (!code || !*code) {
        fprintf(stderr, "eval requires code\n");
        exit(1);
    }

    REPLContext ctx;
    repl_init(&ctx);
    bool ok = true;
    char *source = strdup(code);
    char *cursor = source;

    /* Imports are stateful top-level forms. Allow one-shot evaluation to use
     * the same natural "imports, then expression" shape as a source file while
     * leaving the remaining expression intact for multiline parsing. */
    while (ok) {
        while (*cursor == ' ' || *cursor == '\t' ||
               *cursor == '\r' || *cursor == '\n') cursor++;
        if (strncmp(cursor, "import", 6) != 0 ||
            (cursor[6] != ' ' && cursor[6] != '\t')) break;

        char *line_end = strchr(cursor, '\n');
        if (!line_end) break;
        *line_end = '\0';
        ok = repl_eval_line(&ctx, cursor);
        cursor = line_end + 1;
    }
    while (*cursor == ' ' || *cursor == '\t' ||
           *cursor == '\r' || *cursor == '\n') cursor++;
    if (ok && *cursor)
        ok = repl_eval_line(&ctx, cursor);

    free(source);
    repl_dispose(&ctx);
    exit(ok ? 0 : 1);
}


int main(int argc, char **argv) {
    if (argc > 0)
        g_program_path = argv[0];
    CompilerFlags flags = parse_flags(argc, argv);
    env_require_explicit_effect_arrows(!flags.allow_implicit_effects);
    switch (flags.mode) {
    case CMD_REPL:    repl_run();                        return 0;
    case CMD_NEW:     cmd_new(flags.package_name);       return 0;
    case CMD_BUILD:   cmd_build(&flags);                 return 0;
    case CMD_RUN:     cmd_run(&flags);                   return 0;
    case CMD_CLEAN:   cmd_clean();                       return 0;
    case CMD_INSTALL: cmd_install();                     return 0;
    case CMD_TEST:    cmd_test(&flags);                  return 0;
    case CMD_BATCH:   cmd_batch(&flags);                 return 0;
    case CMD_CHECK:   cmd_check(flags.input_file);       return 0;
    case CMD_LINT:    return cmd_lint(flags.input_file, flags.lint_json,
                                     flags.lint_fix);
    case CMD_FORMAT:  return cmd_format(flags.input_file,
                              flags.format_ascii ? FORMAT_CONTROL_ASCII : FORMAT_CONTROL_GLYPH,
                              flags.format_doc_glyph ? FORMAT_DOC_GLYPH : FORMAT_DOC_INLINE,
                              flags.format_write, flags.format_check);
    case CMD_LSP:     cmd_lsp();                         return 0;
    case CMD_EVAL:    cmd_eval(flags.eval_code);         return 0;
    case CMD_DEBUG:   cmd_debug(&flags);                 return 0;
    case CMD_SPIRV: {
        char *error = NULL;
        int written = spirv_write_monad_module(flags.input_file,
                                               flags.output_name,
                                               flags.spirv_name,
                                               &error);
        if (!written) {
            fprintf(stderr, "error: %s\n", error ? error : "cannot emit SPIR-V module");
            free(error);
            return 1;
        }
        return 0;
    }
    case CMD_COMPILE:
    default:
        return compile(&flags) ? 0 : 1;
    }
}
