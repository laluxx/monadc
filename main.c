#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#if defined(_WIN32)
#include <stdlib.h>
#endif

#include "reader.h"
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
#include "dep.h"
#include "typst_emit.h"
#include "optimizations.h"
#include "bytecode.h"

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
} CompiledExport;

typedef struct CompiledLayout {
    char *name;
    Type *type;
} CompiledLayout;

typedef struct CompiledModule {
    char           *module_name;
    char           *obj_path;
    bool            was_skipped;    // compiled from .o timestamp, no LLVMValueRef
    CompiledExport *exports;
    size_t          export_count;
    size_t          export_cap;
    CompiledLayout *layouts;
    size_t          layout_count;
    size_t          layout_cap;
    TypeClassRegistry *tc_registry;
    struct CompiledModule *next;
} CompiledModule;

static CompiledModule *g_compiled = NULL;

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

static CompiledModule *registry_find(const char *name) {
    if (!name) return NULL;

    const char *name_dot = strrchr(name, '.');
    const char *name_slash = strrchr(name, '/');
    const char *name_tail_dot = name_dot ? name_dot + 1 : name;
    const char *name_tail_slash = name_slash ? name_slash + 1 : name;

    for (CompiledModule *m = g_compiled; m; m = m->next) {
        if (!m->module_name) continue;

        const char *mn = m->module_name;
        const char *mn_dot = strrchr(mn, '.');
        const char *mn_slash = strrchr(mn, '/');
        const char *mn_tail_dot = mn_dot ? mn_dot + 1 : mn;
        const char *mn_tail_slash = mn_slash ? mn_slash + 1 : mn;

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
        if (strcmp(m->obj_path, obj_path) == 0) return m;
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




static void registry_push_func(CompiledModule *m, const char *local,
                                const char *mangled, Type *ret,
                                EnvParam *params, int pc, LLVMValueRef fn) {
    registry_grow(m);
    CompiledExport *e = &m->exports[m->export_count++];
    memset(e, 0, sizeof(*e));

    Type *abi_ret = registry_function_return_type(ret);

    e->local_name   = strdup(local);
    e->mangled_name = strdup(mangled);
    e->kind         = ENV_FUNC;
    e->return_type  = type_clone(abi_ret);
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
        for (size_t i = 0; i < m->layout_count; i++) {
            free(m->layouts[i].name);
            type_free(m->layouts[i].type);
        }
        free(m->layouts);
        tc_registry_free(m->tc_registry);
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

static bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void ensure_cache_dir(const char *home)
{
    if (!home || !*home)
        return;

    char dir[1024];
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

static void declare_externals(CodegenContext *ctx,
                               CompiledModule *dep,
                               ImportDecl *import) {
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
                            e->param_count, type_clone(e->return_type), fn, NULL, NULL);
            EnvEntry *ent = env_lookup(ctx->env, qn);
            if (ent) { ent->module_name = strdup(dep->module_name);
                       ent->llvm_name   = strdup(e->mangled_name); }

            if (import->mode != IMPORT_QUALIFIED) {
                env_insert_func(ctx->env, e->local_name,
                                clone_params(e->params, e->param_count),
                                e->param_count, type_clone(e->return_type), fn, NULL, NULL);
                EnvEntry *ent2 = env_lookup(ctx->env, e->local_name);
                if (ent2) { ent2->module_name = strdup(dep->module_name);
                            ent2->llvm_name   = strdup(e->mangled_name); }
            }
        }
    }
}

static char *get_obj_path(const char *source_path, bool is_main_module) {
    const char *home = getenv("HOME");

    char *core_prefix = monad_core_dir();
    bool is_core_path = dir_prefix_matches(source_path, core_prefix);

    if (is_core_path && home) {
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
        snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/monad/core", home);
        ensure_cache_dir(home);

        char obj[1024];
        if (is_main_module) {
            snprintf(obj, sizeof(obj), "%s/%s.o", cache_dir, base);
        } else {
            snprintf(obj, sizeof(obj), "%s/%s.module.o", cache_dir, base);
        }
        free(base);
        free(core_prefix);
        return strdup(obj);
    }
    free(core_prefix);

    // Normal case
    char *base = base_no_ext(source_path);
    const char *suffix = is_main_module ? ".o" : ".module.o";
    char *obj = malloc(strlen(base) + strlen(suffix) + 1);
    sprintf(obj, "%s%s", base, suffix);
    free(base);
    return obj;
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

    if (strstr(path, "/prelude/"))
        return true;

    char real[1024];
    if (host_realpath(path, real) && strstr(real, "/prelude/"))
        return true;

    return false;
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
        if (registry_find(module_name))
            continue;

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
        if (cached) return cached;
    }

    char *base     = base_no_ext(my_source_path);
    /* char *obj_path = malloc(strlen(base) + 3); */
    /* sprintf(obj_path, "%s.o", base); */
    char *obj_path = get_obj_path(my_source_path, is_main_module);


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
        if (_existing && !_existing->was_skipped) {
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
    bool skip_emit = !is_main_module && (obj_t > 0 && obj_t > src_t);

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

    PHASE_START();

    /* Pre-pass: parse FFI includes to populate wisp arity table before
     * the full wisp expansion runs. We create a temporary FFI context,
     * scan for (include ...) lines, parse those headers, then register
     * all function and layout arities with wisp. */
    {
        FFIContext *pre_ffi = ffi_context_create();
        /* Scan source for include directives using a simple line scan */
        const char *p = source;
        while (*p) {
            /* Skip whitespace */
            while (*p == ' ' || *p == '\t') p++;
            /* Match: include <header> or (include <header> system) */
            if (strncmp(p, "include", 7) == 0 && (p[7] == ' ' || p[7] == '\t' || p[7] == '<')) {
                const char *q = p + 7;
                while (*q == ' ' || *q == '\t') q++;
                bool system_inc = false;
                const char *hstart = NULL;
                const char *hend   = NULL;
                if (*q == '<') {
                    system_inc = true;
                    hstart = q + 1;
                    hend   = strchr(hstart, '>');
                } else if (*q == '"') {
                    system_inc = false;
                    hstart = q + 1;
                    hend   = strchr(hstart, '"');
                }
                if (hstart && hend) {
                    char header[256];
                    size_t hlen = hend - hstart;
                    if (hlen < sizeof(header)) {
                        memcpy(header, hstart, hlen);
                        header[hlen] = '\0';
                        ffi_parse_header(pre_ffi, header, system_inc);
                    }
                }
            }
            /* Also match s-expression form: (include "header" system) */
            if (*p == '(' && strncmp(p+1, "include", 7) == 0) {
                const char *q = p + 8;
                while (*q == ' ' || *q == '\t') q++;
                bool system_inc = false;
                const char *hstart = NULL;
                const char *hend   = NULL;
                if (*q == '<') {
                    system_inc = true;
                    hstart = q + 1;
                    hend   = strchr(hstart, '>');
                } else if (*q == '"') {
                    system_inc = false;
                    hstart = q + 1;
                    hend   = strchr(hstart, '"');
                }
                if (hstart && hend) {
                    char header[256];
                    size_t hlen = hend - hstart;
                    if (hlen < sizeof(header)) {
                        memcpy(header, hstart, hlen);
                        header[hlen] = '\0';
                        ffi_parse_header(pre_ffi, header, system_inc);
                    }
                }
            }
            /* Advance to next line */
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
        /* Register all FFI function arities with wisp */
        for (int fi = 0; fi < pre_ffi->function_count; fi++)
            wisp_register_arity(pre_ffi->functions[fi].name,
                                pre_ffi->functions[fi].param_count);
        /* Register all layout constructors with their field counts */
        for (int si = 0; si < pre_ffi->struct_count; si++)
            if (!pre_ffi->structs[si].alias_of)
                wisp_register_arity(pre_ffi->structs[si].name,
                                    pre_ffi->structs[si].field_count);
        ffi_context_free(pre_ffi);
    }
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
        static const char *k_primitive_type_stems[] = {
            "Int", "I8", "I16", "I32", "I64", "I128",
            "U8",  "U16", "U32", "U64", "U128",
            "Float", "F32", "F80",
            "Bool", "String", "Char",
            NULL
        };

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

        for (int _ti = 0; k_primitive_type_stems[_ti]; _ti++) {
            const char *stem = k_primitive_type_stems[_ti];

            char type_paths[3][1024];
            int type_path_count = 0;
            snprintf(type_paths[type_path_count++], sizeof(type_paths[0]),
                     "%s/%s.mon", src_dir, stem);
            {
                char *core_dir = monad_core_dir();
                snprintf(type_paths[type_path_count++], sizeof(type_paths[0]),
                         "%s/prelude/Data/%s.mon", core_dir, stem);
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
                            parser_set_context(my_source_path, source);
                            /* dep headers already in global FFI context */
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
                            for (int si = 0; si < dep_ffi->struct_count; si++)
                                if (!dep_ffi->structs[si].alias_of)
                                    wisp_register_arity(dep_ffi->structs[si].name,
                                                        dep_ffi->structs[si].field_count);
                            ffi_context_free(dep_ffi);
                            free(dep_source);
                        }
                        free(dep_src);
                    }
                }
            }
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    }

    parser_set_context(my_source_path, source);
    AST *_feat_early = detect_features();
    ast_free(_feat_early);
    wisp_set_trace(flags->trace_ast);
    ASTList exprs = wisp_parse_all(source, my_source_path);

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
                fprintf(stderr, "error: cannot find module '%s' (tried: %s)",
                        imp->module_name, dep_src);
                free(dep_src); exit(1);
            }
            compile_one(dep_src, flags, false);
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
        declare_externals(&ctx, dep, imp);

        /* Headers from imported modules are already in the global FFI context
         * (parsed when that module was compiled) — nothing to do here. */
    }

    /* Auto-declare primitive type method modules (Int, String, etc.).
     * Same canonical list as the auto-compile pass above — no directory
     * scan, no capitalization heuristic.  For each known primitive stem,
     * find its compiled module in the registry and call declare_externals
     * so that Int.double etc. are visible in the current module's env.  */
    {
        static const char *k_primitive_type_stems2[] = {
            "Int", "I8", "I16", "I32", "I64", "I128",
            "U8",  "U16", "U32", "U64", "U128",
            "Float", "F32", "F80",
            "Bool", "String", "Char",
            NULL
        };

        for (int _ti = 0; k_primitive_type_stems2[_ti]; _ti++) {
            const char *stem2 = k_primitive_type_stems2[_ti];

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
            declare_externals(&ctx, type_mod, syn);
            import_decl_free(syn);

            type_mod->module_name = old_mn;  /* restore */
        }
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

    for (size_t i = first_code; i < exprs.count; i++) {
        AST *expr = exprs.exprs[i];

        // Skip module/import nodes for the type checker
        if (expr->type == AST_LIST && expr->list.count > 0 && expr->list.items[0]->type == AST_SYMBOL) {
            const char *h = expr->list.items[0]->symbol;
            if (strcmp(h, "module") == 0 || strcmp(h, "import") == 0) continue;
        }

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

/// Phase 7: Codegen top-level expressions

    PHASE_START();
    codegen_predeclare_toplevel_functions(&ctx, exprs.exprs, exprs.count,
                                           first_code);

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
    } else {
        cm = registry_new(mod_name, obj_path, false);
    }
    tc_registry_free(cm->tc_registry);
    cm->tc_registry = tc_registry_clone(ctx.tc_registry);
    for (size_t bi = 0; bi < ctx.env->size; bi++) {
        EnvEntry *ent = ctx.env->buckets[bi];
        while (ent) {
            if (ent->kind == ENV_BUILTIN || ent->module_name != NULL ||
                ent->name[0] == '*') { ent = ent->next; continue; }

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
            if (!force_export &&
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

            char *ms = mangle(mod_name, _local_name);

            if (ent->kind == ENV_FUNC || ent->kind == ENV_ADT_CTOR) {
                /* Never rename FFI functions — they must keep their original
                 * symbol names so the linker can find them in libraylib etc. */
                if (ent->is_ffi) { free(ms); ent = ent->next; continue; }
                registry_push_func(cm, _local_name, ms,
                                   ent->return_type ? ent->return_type : ent->type,
                                   ent->params, ent->param_count,
                                   ent->func_ref);
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
                                               ent->func_ref);
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

///// Cleanup

    dep_ctx_free(dep_ctx);
    ffi_libs_add(g_ffi);
    ctx.ffi = NULL;  /* don't free the global */
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
    g_ffi_link_libs[0]  = '\0';
    g_ffi_link_libs_len = 0;
    codegen_set_trace(flags->trace_codegen || flags->verbose_level > 0);
    infer_set_trace(flags->trace_dep || flags->verbose_level > 1);

    CompiledModule *main_mod = compile_one(flags->input_file, flags, true);
    if (!main_mod) return;  /* emit-json mode, no linking needed */

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

    const char *ld_flag = "";
    if (access("/usr/bin/mold", X_OK) == 0 ||
        access("/usr/local/bin/mold", X_OK) == 0)
        ld_flag = " -fuse-ld=mold";
    else if (access("/usr/bin/ld.lld", X_OK) == 0 ||
             access("/usr/local/bin/ld.lld", X_OK) == 0)
        ld_flag = " -fuse-ld=lld";

    char cmd[4096];
    int w = snprintf(cmd, sizeof(cmd), "clang%s", ld_flag);
    for (size_t i = 0; i < n; i++)
        w += snprintf(cmd + w, sizeof(cmd) - w, " %s", objs[i]);

    char *runtime_archive = runtime_archive_path();
    char *llvm_flags = llvm_config_link_flags();
    w += snprintf(cmd + w, sizeof(cmd) - w,
                  " -o %s %s"
                  " %s -lm -lgmp%s%s",
                  exec_name, runtime_archive,
                  llvm_flags,
                  host_no_pie_flag(), g_ffi_link_libs);
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
            for (size_t i = 0; i < n; i++) remove(objs[i]);
    } else {
        fprintf(stderr, "[error] linking failed\n");
    }

    free(objs);
    free(exec_name);
    free(runtime_archive);
    registry_free_all();
    wisp_clear_arities();
    if (g_ffi) { ffi_context_free(g_ffi); g_ffi = NULL; }
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

void cmd_eval(const char *code) {
    if (!code || !*code) {
        fprintf(stderr, "eval requires code\n");
        exit(1);
    }

    REPLContext ctx;
    repl_init(&ctx);
    bool ok = repl_eval_line(&ctx, code);
    repl_dispose(&ctx);
    exit(ok ? 0 : 1);
}


int main(int argc, char **argv) {
    if (argc > 0)
        g_program_path = argv[0];
    CompilerFlags flags = parse_flags(argc, argv);
    switch (flags.mode) {
    case CMD_REPL:    repl_run();                        return 0;
    case CMD_NEW:     cmd_new(flags.package_name);       return 0;
    case CMD_BUILD:   cmd_build(&flags);                 return 0;
    case CMD_RUN:     cmd_run(&flags);                   return 0;
    case CMD_CLEAN:   cmd_clean();                       return 0;
    case CMD_INSTALL: cmd_install();                     return 0;
    case CMD_TEST:    cmd_test(&flags);                  return 0;
    case CMD_CHECK:   cmd_check(flags.input_file);       return 0;
    case CMD_LSP:     cmd_lsp();                         return 0;
    case CMD_EVAL:    cmd_eval(flags.eval_code);         return 0;
    case CMD_DEBUG:   cmd_debug(&flags);                 return 0;
    case CMD_COMPILE:
    default:
        compile(&flags);
        return 0;
    }
}
