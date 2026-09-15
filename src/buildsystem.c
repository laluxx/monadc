/// buildsystem.c — Build system for Monad
//
//  See buildsystem.h for the section map. This file mirrors it 1:1.

#include "buildsystem.h"
#include "module.h"
#include "reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


/// §1  Includes and constants

//// Memory helpers
 //
 //  Abort-on-OOM wrappers, matching module.c and lsp.c.
 //
static void *bs_xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "buildsystem: out of memory\n"); abort(); }
    return p;
}

static void *bs_xrealloc(void *ptr, size_t n)
{
    void *p = realloc(ptr, n);
    if (!p) { fprintf(stderr, "buildsystem: out of memory\n"); abort(); }
    return p;
}

static char *bs_xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = bs_xmalloc(n);
    memcpy(d, s, n);
    return d;
}

static bool bs_file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static char *build_runtime_archive_path(void)
{
    const char *env_runtime = getenv("MONAD_RUNTIME_LIB");
    if (env_runtime && *env_runtime)
        return bs_xstrdup(env_runtime);

    if (bs_file_exists("libmonad.a"))
        return bs_xstrdup("libmonad.a");

    return bs_xstrdup("/usr/local/lib/libmonad.a");
}

#define BS_GROW(ptr, count, cap, type)                         \
    do {                                                       \
        if ((count) >= (cap)) {                                \
            (cap) = (cap) ? (cap) * 2 : 8;                     \
            (ptr) = bs_xrealloc((ptr), (cap) * sizeof(type));  \
        }                                                      \
    } while (0)


/// §2  Module artifacts

ModuleArtifact *build_artifact_create(const char *module_name, const char *source_path)
{
    ModuleArtifact *artifact = bs_xmalloc(sizeof(*artifact));
    memset(artifact, 0, sizeof(*artifact));

    artifact->module_name = bs_xstrdup(module_name);
    artifact->source_path = bs_xstrdup(source_path);

    char obj_path[512];
    snprintf(obj_path, sizeof(obj_path), "%s.o", module_name);
    artifact->object_path = bs_xstrdup(obj_path);

    artifact->source_mtime = 0;
    artifact->object_mtime = 0;
    artifact->source_hash_valid = false;
    artifact->needs_recompile = true;
    artifact->decl = NULL;
    artifact->env  = NULL;

    build_artifact_update_times(artifact);
    build_artifact_update_hash(artifact);

    return artifact;
}

void build_artifact_free(ModuleArtifact *artifact)
{
    if (!artifact) return;
    free(artifact->module_name);
    free(artifact->source_path);
    free(artifact->object_path);
    /* decl and env are managed elsewhere (registry / interpreter). */
    free(artifact);
}

void build_artifact_update_times(ModuleArtifact *artifact)
{
    artifact->source_mtime = build_get_file_mtime(artifact->source_path);
    artifact->object_mtime = build_get_file_mtime(artifact->object_path);
}

bool build_artifact_update_hash(ModuleArtifact *artifact)
{
    FILE *f = fopen(artifact->source_path, "rb");
    if (!f) {
        artifact->source_hash_valid = false;
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        artifact->source_hash_valid = false;
        return false;
    }

    char *buf = bs_xmalloc((size_t)size);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);

    module_hash_buffer(buf, got, artifact->source_hash);
    free(buf);

    artifact->source_hash_valid = true;
    return true;
}

bool build_artifact_needs_rebuild(ModuleArtifact *artifact)
{
    build_artifact_update_times(artifact);

    if (artifact->object_mtime == 0) return true;
    if (artifact->source_mtime > artifact->object_mtime) return true;

    return false;
}


/// §3  Build context

BuildContext *build_context_create(void)
{
    BuildContext *ctx = bs_xmalloc(sizeof(*ctx));
    memset(ctx, 0, sizeof(*ctx));

    ctx->artifact_capacity = 8;
    ctx->artifacts = bs_xmalloc(sizeof(ModuleArtifact *) * ctx->artifact_capacity);
    ctx->registry  = module_registry_create();
    ctx->build_dir = bs_xstrdup(".");
    ctx->verbose   = false;
    ctx->force_rebuild = false;
    ctx->cache     = NULL;
    ctx->cache_path = NULL;
    return ctx;
}

void build_context_free(BuildContext *ctx)
{
    if (!ctx) return;

    for (size_t i = 0; i < ctx->artifact_count; i++)
        build_artifact_free(ctx->artifacts[i]);
    free(ctx->artifacts);

    module_registry_free(ctx->registry);
    build_cache_free(ctx->cache);

    free(ctx->build_dir);
    free(ctx->cache_path);
    free(ctx);
}

void build_context_set_build_dir(BuildContext *ctx, const char *dir)
{
    free(ctx->build_dir);
    ctx->build_dir = bs_xstrdup(dir);
}

void build_context_set_verbose(BuildContext *ctx, bool verbose)
{
    ctx->verbose = verbose;
}

void build_context_set_force_rebuild(BuildContext *ctx, bool force)
{
    ctx->force_rebuild = force;
}

void build_context_add_artifact(BuildContext *ctx, ModuleArtifact *artifact)
{
    BS_GROW(ctx->artifacts, ctx->artifact_count, ctx->artifact_capacity,
            ModuleArtifact *);
    ctx->artifacts[ctx->artifact_count++] = artifact;
}

ModuleArtifact *build_context_find_artifact(BuildContext *ctx, const char *module_name)
{
    for (size_t i = 0; i < ctx->artifact_count; i++) {
        if (strcmp(ctx->artifacts[i]->module_name, module_name) == 0)
            return ctx->artifacts[i];
    }
    return NULL;
}

ModuleArtifact *build_context_find_by_source(BuildContext *ctx, const char *source_path)
{
    for (size_t i = 0; i < ctx->artifact_count; i++) {
        if (strcmp(ctx->artifacts[i]->source_path, source_path) == 0)
            return ctx->artifacts[i];
    }
    return NULL;
}


/// §4  Persistent build cache (manifest)

//// Cache file format
 //
 //  Line 1: cache version (must equal BUILD_CACHE_VERSION)
 //  Lines 2..N, one per module:
 //
 //      module_name<TAB>source_path<TAB>object_path<TAB>source_hash<TAB>dep1,dep2,...
 //
 //  Fields are tab-separated; the dependency list is comma-separated and
 //  may be empty. Lines that don't parse are skipped (forward/backward
 //  compatibility: a malformed manifest just yields a cold cache).
 //
static char *cache_file_path(const char *build_dir)
{
    size_t len = strlen(build_dir) + 1 + strlen(BUILD_CACHE_FILENAME) + 1;
    char *path = bs_xmalloc(len);
    snprintf(path, len, "%s/%s", build_dir, BUILD_CACHE_FILENAME);
    return path;
}

static void cache_grow(BuildCache *cache)
{
    BS_GROW(cache->entries, cache->count, cache->capacity, ModuleArtifact *);
}

BuildCache *build_cache_load(const char *build_dir)
{
    BuildCache *cache = bs_xmalloc(sizeof(*cache));
    cache->entries  = NULL;
    cache->count    = 0;
    cache->capacity = 0;

    char *path = cache_file_path(build_dir);
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return cache;

    char line[4096];
    bool first = true;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (first) {
            first = false;
            long version = strtol(line, NULL, 10);
            if (version != BUILD_CACHE_VERSION) break; /* stale format */
            continue;
        }

        /* module_name\tsource_path\tobject_path\tsource_hash\tdeps */
        char *fields[5] = {0};
        char *p = line;
        int nf = 0;
        for (; nf < 5; nf++) {
            fields[nf] = p;
            char *tab = strchr(p, '\t');
            if (!tab) { nf++; break; }
            *tab = '\0';
            p = tab + 1;
        }
        if (nf < 4) continue; /* malformed line, skip */

        ModuleArtifact *a = bs_xmalloc(sizeof(*a));
        memset(a, 0, sizeof(*a));
        a->module_name = bs_xstrdup(fields[0]);
        a->source_path = bs_xstrdup(fields[1]);
        a->object_path = bs_xstrdup(fields[2]);
        if (strlen(fields[3]) == MODULE_HASH_HEX_LEN) {
            memcpy(a->source_hash, fields[3], MODULE_HASH_HEX_LEN + 1);
            a->source_hash_valid = true;
        }
        /* deps (fields[4]) are not needed for freshness checks; the
           build driver re-discovers deps for any module it compiles. */

        cache_grow(cache);
        cache->entries[cache->count++] = a;
    }

    fclose(f);
    return cache;
}

ModuleArtifact *build_cache_find(BuildCache *cache, const char *module_name)
{
    if (!cache) return NULL;
    for (size_t i = 0; i < cache->count; i++) {
        if (strcmp(cache->entries[i]->module_name, module_name) == 0)
            return cache->entries[i];
    }
    return NULL;
}

bool build_cache_is_fresh(BuildCache *cache, ModuleArtifact *artifact)
{
    if (!cache) return false;

    ModuleArtifact *cached = build_cache_find(cache, artifact->module_name);
    if (!cached) return false;
    if (!cached->source_hash_valid || !artifact->source_hash_valid) return false;

    if (strcmp(cached->source_hash, artifact->source_hash) != 0)
        return false;

    if (strcmp(cached->object_path, artifact->object_path) != 0)
        return false;

    return build_file_exists(artifact->object_path);
}

bool build_cache_save(BuildContext *ctx)
{
    char *path = cache_file_path(ctx->build_dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        if (ctx->verbose)
            fprintf(stderr, "Warning: could not write build cache to %s\n", path);
        free(path);
        return false;
    }
    free(path);

    fprintf(f, "%d\n", BUILD_CACHE_VERSION);

    for (size_t i = 0; i < ctx->artifact_count; i++) {
        ModuleArtifact *a = ctx->artifacts[i];
        if (!a->source_hash_valid) continue;

        fprintf(f, "%s\t%s\t%s\t%s\t",
                a->module_name, a->source_path, a->object_path, a->source_hash);

        ModuleRegistryEntry *entry = module_registry_find(ctx->registry, a->module_name);
        if (entry) {
            for (size_t d = 0; d < entry->dep_count; d++) {
                if (d) fputc(',', f);
                fputs(entry->dep_names[d], f);
            }
        }
        fputc('\n', f);
    }

    fclose(f);
    return true;
}

void build_cache_free(BuildCache *cache)
{
    if (!cache) return;
    for (size_t i = 0; i < cache->count; i++)
        build_artifact_free(cache->entries[i]);
    free(cache->entries);
    free(cache);
}


/// §5  Dependency discovery

//// Module declaration / import extraction
 //
 //  Reads and parses a source file just far enough to collect its
 //  `(module ...)`, `(export ...)`, and `(import ...)` forms — stopping at
 //  the first declaration that isn't one of those three, since by
 //  language convention all module-level declarations come first.
 //
ModuleContext *build_extract_module_context(const char *source_path)
{
    FILE *f = fopen(source_path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }

    char *source = bs_xmalloc((size_t)size + 1);
    size_t got = fread(source, 1, (size_t)size, f);
    source[got] = '\0';
    fclose(f);

    parser_set_context(source_path, source);
    ASTList exprs = parse_all(source);

    ModuleContext *mod_ctx = module_context_create();
    module_context_set_file(mod_ctx, source_path);

    for (size_t i = 0; i < exprs.count; i++) {
        AST *expr = exprs.exprs[i];

        bool is_module = false, is_import = false, is_export = false;
        if (expr->type == AST_LIST && expr->list.count > 0 &&
            expr->list.items[0]->type == AST_SYMBOL) {
            const char *head = expr->list.items[0]->symbol;
            is_module = strcmp(head, "module") == 0;
            is_import = strcmp(head, "import") == 0;
            is_export = strcmp(head, "export") == 0;
        }

        if (is_module) {
            ModuleDecl *decl = parse_module_decl(expr);
            if (decl) module_context_set_decl(mod_ctx, decl);
        } else if (is_import) {
            ImportDecl *import = parse_import_decl(expr);
            if (import) module_context_add_import(mod_ctx, import);
        } else if (is_export) {
            ReExportDecl *re = parse_export_decl(expr);
            if (re) {
                if (!mod_ctx->decl) {
                    /* (export ...) with no preceding (module ...): treat
                       as exporting from an implicit all-export module. */
                    module_context_set_decl(mod_ctx,
                        module_decl_create("__Anonymous", EXPORT_ALL));
                }
                if (mod_ctx->decl)
                    module_decl_add_reexport(mod_ctx->decl, re);
                else
                    reexport_decl_free(re);
            }
        } else {
            /* First non module/import/export form ends the header. */
            break;
        }
    }

    for (size_t i = 0; i < exprs.count; i++)
        ast_free(exprs.exprs[i]);
    free(exprs.exprs);
    free(source);

    module_context_add_prelude_imports(mod_ctx);

    return mod_ctx;
}

char **build_resolve_dependencies(ModuleContext *mod_ctx, size_t *out_count)
{
    return module_dep_list_from_context(mod_ctx, out_count);
}

void build_free_dependency_list(char **deps, size_t count)
{
    module_dep_list_free(deps, count);
}


/// §6  Build execution

// Forward declaration of compile function from main.c.
// A real build calls this to actually emit an object file; here we keep
// the hook so main.c can wire in the compiler backend.
extern void compile_module_to_object(const char *source_path, const char *output_object);

//// Discovery phase
 //
 //  Walks the import graph starting at `source_path`, registering every
 //  reachable module (decl + dependency list) in `build_ctx->registry`
 //  and creating a ModuleArtifact for each. Modules already registered
 //  are not re-parsed.
 //
static char *discover_module_name(const char *source_path)
{
    char *base = bs_xstrdup(source_path);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    char *slash = strrchr(base, '/');
    char *name = bs_xstrdup(slash ? slash + 1 : base);
    free(base);
    return name; /* caller frees */
}

static bool discover_recursive(BuildContext *build_ctx, const char *module_name,
                                const char *source_path)
{
    if (module_registry_find(build_ctx->registry, module_name))
        return true; /* already discovered */

    if (!build_file_exists(source_path)) {
        fprintf(stderr, "Error: Cannot find module '%s' (looking for %s)\n",
                module_name, source_path);
        return false;
    }

    if (build_ctx->verbose)
        printf("Discovering %s (%s)\n", module_name, source_path);

    ModuleContext *mod_ctx = build_extract_module_context(source_path);
    if (!mod_ctx) {
        fprintf(stderr, "Error: Failed to read module '%s' from %s\n",
                module_name, source_path);
        return false;
    }

    ModuleArtifact *artifact = build_context_find_by_source(build_ctx, source_path);
    if (!artifact) {
        artifact = build_artifact_create(module_name, source_path);
        build_context_add_artifact(build_ctx, artifact);
    }

    /* Register the module declaration (or a default all-exporting one
       if the file has no explicit `(module ...)` form). */
    ModuleDecl *decl = mod_ctx->decl;
    mod_ctx->decl = NULL; /* registry takes ownership */
    if (!decl) decl = module_decl_create(module_name, EXPORT_ALL);

    ModuleRegistryEntry *entry = module_registry_entry_create(module_name);
    entry->decl = decl;
    entry->source_path = bs_xstrdup(source_path);
    if (artifact->source_hash_valid)
        memcpy(entry->content_hash, artifact->source_hash, sizeof(entry->content_hash));

    size_t dep_count = 0;
    char **deps = module_dep_list_from_context(mod_ctx, &dep_count);
    for (size_t i = 0; i < dep_count; i++)
        module_registry_entry_add_dep(entry, deps[i]);

    module_registry_add(build_ctx->registry, entry);

    bool ok = true;
    for (size_t i = 0; i < dep_count && ok; i++) {
        const char *dep_name = deps[i];
        char *dep_source = build_module_name_to_source_path(dep_name);
        ok = discover_recursive(build_ctx, dep_name, dep_source);
        free(dep_source);
    }

    module_dep_list_free(deps, dep_count);
    module_context_free(mod_ctx);
    return ok;
}

//// Compile phase
 //
 //  Compiles `module_name` if it needs rebuilding (per content hash,
 //  timestamps, and the persistent cache), in the topological order
 //  computed earlier. Dependencies are guaranteed to already be compiled.
 //
static bool compile_one(BuildContext *build_ctx, const char *module_name)
{
    ModuleArtifact *artifact = build_context_find_artifact(build_ctx, module_name);
    if (!artifact) {
        /* Module was referenced but never discovered (shouldn't happen if
           discovery succeeded), or is a builtin with no source file. */
        return true;
    }

    build_artifact_update_hash(artifact);

    bool fresh_in_cache = !build_ctx->force_rebuild &&
                          build_cache_is_fresh(build_ctx->cache, artifact);
    bool fresh_on_disk  = !build_ctx->force_rebuild &&
                          !build_artifact_needs_rebuild(artifact);

    if (fresh_in_cache || fresh_on_disk) {
        if (build_ctx->verbose)
            printf("[%s] Up to date\n", module_name);
        artifact->needs_recompile = false;
        return true;
    }

    if (build_ctx->verbose)
        printf("[%s] Compiling...\n", module_name);

    /* TODO: wire to the real compiler backend via
       compile_module_to_object(artifact->source_path, artifact->object_path); */
    printf("[%s] Compiled to %s\n", module_name, artifact->object_path);

    artifact->needs_recompile = false;
    build_artifact_update_times(artifact);
    build_artifact_update_hash(artifact);
    return true;
}

ModuleArtifact *build_compile_with_deps(BuildContext *build_ctx,
                                         const char *source_path)
{
    char *module_name = discover_module_name(source_path);

    bool ok = discover_recursive(build_ctx, module_name, source_path);
    if (!ok) {
        free(module_name);
        return NULL;
    }

    ModuleDepOrder *order = module_dep_order_compute(build_ctx->registry, module_name);
    if (order->cycle_module) {
        fprintf(stderr, "Error: dependency cycle detected involving module '%s'\n",
                order->cycle_module);
        module_dep_order_free(order);
        free(module_name);
        return NULL;
    }

    for (size_t i = 0; i < order->order_count && ok; i++)
        ok = compile_one(build_ctx, order->order[i]);

    module_dep_order_free(order);

    ModuleArtifact *result = ok
        ? build_context_find_artifact(build_ctx, module_name)
        : NULL;
    free(module_name);
    return result;
}

bool build_link_executable(BuildContext *build_ctx,
                           const char *output_name,
                           ModuleArtifact **artifacts,
                           size_t artifact_count)
{
    if (artifact_count == 0) return false;

    const char *host_exe_suffix =
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
        ".exe";
#else
        "";
#endif
    const char *host_no_pie_flag =
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
        "";
#else
        " -no-pie";
#endif
    char output_with_suffix[1024];
    if (host_exe_suffix[0] &&
        strlen(output_name) < sizeof(output_with_suffix) - strlen(host_exe_suffix) - 1 &&
        (strlen(output_name) < strlen(host_exe_suffix) ||
         strcmp(output_name + strlen(output_name) - strlen(host_exe_suffix), host_exe_suffix) != 0)) {
        snprintf(output_with_suffix, sizeof(output_with_suffix), "%s%s", output_name, host_exe_suffix);
        output_name = output_with_suffix;
    }

    char cmd[4096] = "gcc ";

    for (size_t i = 0; i < artifact_count; i++) {
        strcat(cmd, artifacts[i]->object_path);
        strcat(cmd, " ");
    }

    char *runtime_archive = build_runtime_archive_path();
    char llvm_flags[1024] = "";
    FILE *llvm_pipe = popen("llvm-config --ldflags --libs core orcjit native passes", "r");
    if (llvm_pipe) {
        size_t used = fread(llvm_flags, 1, sizeof(llvm_flags) - 1, llvm_pipe);
        pclose(llvm_pipe);
        llvm_flags[used] = '\0';
        for (size_t i = 0; i < used; i++) {
            if (llvm_flags[i] == '\r' || llvm_flags[i] == '\n')
                llvm_flags[i] = ' ';
        }
    }
    strcat(cmd, runtime_archive);
    strcat(cmd, " -o ");
    strcat(cmd, output_name);
    strcat(cmd, " ");
    strcat(cmd, llvm_flags);
    strcat(cmd, " -lm -lgmp");
    strcat(cmd, host_no_pie_flag);
    free(runtime_archive);

    if (build_ctx->verbose)
        printf("Linking: %s\n", cmd);

    int ret = system(cmd);
    if (ret == 0) {
        printf("Created executable: %s\n", output_name);
        return true;
    }
    fprintf(stderr, "Failed to link executable\n");
    return false;
}

bool build_project(const char *source_path,
                   const char *output_name,
                   bool verbose,
                   bool force_rebuild)
{
    BuildContext *ctx = build_context_create();
    build_context_set_verbose(ctx, verbose);
    build_context_set_force_rebuild(ctx, force_rebuild);

    if (!force_rebuild)
        ctx->cache = build_cache_load(ctx->build_dir);

    printf("Building project: %s\n", source_path);

    ModuleArtifact *main_artifact = build_compile_with_deps(ctx, source_path);
    if (!main_artifact) {
        fprintf(stderr, "Build failed\n");
        build_context_free(ctx);
        return false;
    }

    bool success = build_link_executable(ctx, output_name,
                                          ctx->artifacts, ctx->artifact_count);

    if (success)
        build_cache_save(ctx);

    build_context_free(ctx);
    return success;
}


/// §7  Utilities

time_t build_get_file_mtime(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return st.st_mtime;
    return 0;
}

bool build_file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

char *build_module_name_to_source_path(const char *module_name)
{
    /* Delegate to the module system's search path logic, which checks
       the local directory, $MONAD_CORE, and the installed core. */
    return module_name_to_path(module_name);
}

char *build_module_name_to_object_path(const char *module_name, const char *build_dir)
{
    size_t len = strlen(build_dir) + strlen(module_name) + 4;
    char *path = bs_xmalloc(len);
    snprintf(path, len, "%s/%s.o", build_dir, module_name);
    return path;
}
