/// module.c — Module system for Monad
//
//  See module.h for the section map. This file mirrors it 1:1.

#include "module.h"

#include <ctype.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


/// §1  Includes and constants

//// Memory helpers
 //
 //  Thin abort-on-OOM wrappers, matching lsp.c's convention. A module
 //  system that runs out of memory mid-build has no good recovery story
 //  anyway, so we fail loudly and immediately.
 //
static void *mod_xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "module: out of memory\n"); abort(); }
    return p;
}

static void *mod_xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n, sz);
    if (!p) { fprintf(stderr, "module: out of memory\n"); abort(); }
    return p;
}

static void *mod_xrealloc(void *ptr, size_t n)
{
    void *p = realloc(ptr, n);
    if (!p) { fprintf(stderr, "module: out of memory\n"); abort(); }
    return p;
}

static char *mod_xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = mod_xmalloc(n);
    memcpy(d, s, n);
    return d;
}

/* Grow a heap array to at least (count + 1) slots. */
#define MOD_GROW(ptr, count, cap, type)                        \
    do {                                                       \
        if ((count) >= (cap)) {                                \
            (cap) = (cap) ? (cap) * 2 : 8;                      \
            (ptr) = mod_xrealloc((ptr), (cap) * sizeof(type));  \
        }                                                       \
    } while (0)


/// §2  Export lists and module declarations

void export_list_init(ExportList *list)
{
    list->symbols  = NULL;
    list->count    = 0;
    list->capacity = 0;
}

void export_list_add(ExportList *list, const char *symbol)
{
    MOD_GROW(list->symbols, list->count, list->capacity, char *);
    list->symbols[list->count++] = mod_xstrdup(symbol);
}

bool export_list_contains(const ExportList *list, const char *symbol)
{
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->symbols[i], symbol) == 0)
            return true;
    }
    return false;
}

void export_list_free(ExportList *list)
{
    for (size_t i = 0; i < list->count; i++)
        free(list->symbols[i]);
    free(list->symbols);
    list->symbols  = NULL;
    list->count    = 0;
    list->capacity = 0;
}

bool module_name_is_valid(const char *name)
{
    if (!name || !*name) return false;

    /* Must start with an uppercase letter. */
    if (!isupper((unsigned char)name[0])) return false;

    /* May contain letters, digits, dots, and underscores. */
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_')
            return false;
    }
    return true;
}

ModuleDecl *module_decl_create(const char *name, ExportMode mode)
{
    if (!module_name_is_valid(name)) {
        fprintf(stderr, "Invalid module name: %s (must start with uppercase)\n",
                name);
        return NULL;
    }

    ModuleDecl *decl = mod_xcalloc(1, sizeof(*decl));
    decl->name = mod_xstrdup(name);
    decl->mode = mode;
    export_list_init(&decl->exports);
    return decl;
}

void module_decl_add_export(ModuleDecl *decl, const char *symbol)
{
    export_list_add(&decl->exports, symbol);
}

void module_decl_add_reexport(ModuleDecl *decl, ReExportDecl *re)
{
    MOD_GROW(decl->reexports, decl->reexport_count, decl->reexport_capacity,
             ReExportDecl *);
    decl->reexports[decl->reexport_count++] = re;
}

bool module_decl_is_exported(ModuleDecl *decl, const char *symbol)
{
    /* No module declaration => everything is exported (script-style file). */
    if (!decl) return true;

    switch (decl->mode) {
    case EXPORT_ALL:
        return true;
    case EXPORT_NONE:
        return false;
    case EXPORT_SELECTED:
        return export_list_contains(&decl->exports, symbol);
    }
    return false;
}

void module_decl_free(ModuleDecl *decl)
{
    if (!decl) return;
    free(decl->name);
    export_list_free(&decl->exports);
    for (size_t i = 0; i < decl->reexport_count; i++)
        reexport_decl_free(decl->reexports[i]);
    free(decl->reexports);
    free(decl);
}


/// §3  Re-export declarations

ReExportDecl *reexport_decl_create(const char *module_name, bool reexport_all)
{
    ReExportDecl *re = mod_xcalloc(1, sizeof(*re));
    re->module_name  = mod_xstrdup(module_name);
    re->reexport_all = reexport_all;
    export_list_init(&re->symbols);
    return re;
}

void reexport_decl_add_symbol(ReExportDecl *re, const char *symbol)
{
    export_list_add(&re->symbols, symbol);
}

void reexport_decl_free(ReExportDecl *re)
{
    if (!re) return;
    free(re->module_name);
    export_list_free(&re->symbols);
    free(re);
}


/// §4  Import declarations

ImportDecl *import_decl_create(const char *module_name, const char *alias,
                                ImportMode mode)
{
    ImportDecl *decl = mod_xcalloc(1, sizeof(*decl));
    decl->module_name = mod_xstrdup(module_name);
    decl->alias       = alias ? mod_xstrdup(alias) : NULL;
    decl->mode        = mode;
    decl->qualified   = false;
    decl->symbols     = NULL;
    decl->symbol_count = 0;
    return decl;
}

void import_decl_add_symbol(ImportDecl *decl, const char *symbol)
{
    decl->symbols = mod_xrealloc(decl->symbols,
                                  sizeof(char *) * (decl->symbol_count + 1));
    decl->symbols[decl->symbol_count++] = mod_xstrdup(symbol);
}

bool import_decl_includes_symbol(ImportDecl *decl, const char *symbol)
{
    switch (decl->mode) {
    case IMPORT_QUALIFIED:
    case IMPORT_UNQUALIFIED:
        return true;

    case IMPORT_SELECTIVE:
        for (size_t i = 0; i < decl->symbol_count; i++) {
            if (strcmp(decl->symbols[i], symbol) == 0)
                return true;
        }
        return false;

    case IMPORT_HIDING:
        for (size_t i = 0; i < decl->symbol_count; i++) {
            if (strcmp(decl->symbols[i], symbol) == 0)
                return false;
        }
        return true;
    }
    return false;
}

/* The prefix used for qualified access: the explicit alias if given,
   otherwise the last dot-component of the module name. */
const char *import_decl_prefix(const ImportDecl *decl)
{
    if (decl->alias) return decl->alias;
    const char *last_dot = strrchr(decl->module_name, '.');
    return last_dot ? last_dot + 1 : decl->module_name;
}

void import_decl_free(ImportDecl *decl)
{
    if (!decl) return;
    free(decl->module_name);
    free(decl->alias);
    for (size_t i = 0; i < decl->symbol_count; i++)
        free(decl->symbols[i]);
    free(decl->symbols);
    free(decl);
}


/// §5  Module context (per-file compilation state)

ModuleContext *module_context_create(void)
{
    ModuleContext *ctx = mod_xcalloc(1, sizeof(*ctx));
    ctx->import_capacity = 4;
    ctx->imports = mod_xmalloc(sizeof(ImportDecl *) * ctx->import_capacity);
    return ctx;
}

void module_context_free(ModuleContext *ctx)
{
    if (!ctx) return;
    if (ctx->decl) module_decl_free(ctx->decl);
    for (size_t i = 0; i < ctx->import_count; i++)
        import_decl_free(ctx->imports[i]);
    free(ctx->imports);
    free(ctx->current_file);
    free(ctx);
}

void module_context_set_file(ModuleContext *ctx, const char *filename)
{
    free(ctx->current_file);
    ctx->current_file = mod_xstrdup(filename);
}

void module_context_set_decl(ModuleContext *ctx, ModuleDecl *decl)
{
    if (ctx->decl) module_decl_free(ctx->decl);
    ctx->decl = decl;
}

void module_context_add_import(ModuleContext *ctx, ImportDecl *import)
{
    MOD_GROW(ctx->imports, ctx->import_count, ctx->import_capacity, ImportDecl *);
    ctx->imports[ctx->import_count++] = import;
}

/* Find an import by prefix.  The prefix is matched against:
     1. The alias (`:as Name`) if one was given, OR
     2. The last component of the module name when no alias was given.
        e.g. `(import qualified Math)` → prefix "Math" matches module "Math".
        e.g. `(import qualified Std.Math)` → prefix "Math" matches too,
             because the last dot-component of "Std.Math" is "Math".
   This allows `Math.phi` to resolve even when no `:as` was written. */
ImportDecl *module_context_find_import(ModuleContext *ctx, const char *prefix)
{
    for (size_t i = 0; i < ctx->import_count; i++) {
        ImportDecl *imp = ctx->imports[i];
        if (strcmp(import_decl_prefix(imp), prefix) == 0)
            return imp;
    }
    return NULL;
}


/// §6  Module registry (global, content-addressed)

//// FNV-1a hashing
 //
 //  Used both for the registry hash table (32-bit, for bucket selection)
 //  and for content-hashing source files for the build cache (64-bit,
 //  hex-encoded). FNV-1a is not cryptographically strong, but it is more
 //  than adequate for "did this file change" cache invalidation and has
 //  negligible cost.
 //
static uint64_t fnv1a64(const char *data, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint32_t fnv1a32_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint32_t)(unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

void module_hash_buffer(const char *data, size_t len, char *out)
{
    uint64_t h = fnv1a64(data, len);
    snprintf(out, MODULE_HASH_HEX_LEN + 1, "%016" PRIx64, h);
}

ModuleRegistry *module_registry_create(void)
{
    ModuleRegistry *registry = mod_xcalloc(1, sizeof(*registry));
    registry->sorted_capacity = 16;
    registry->sorted = mod_xmalloc(sizeof(ModuleRegistryEntry *) *
                                    registry->sorted_capacity);
    return registry;
}

ModuleRegistryEntry *module_registry_entry_create(const char *name)
{
    ModuleRegistryEntry *e = mod_xcalloc(1, sizeof(*e));
    e->source_path = NULL;
    e->dep_names   = NULL;
    e->dep_count   = 0;
    e->dep_capacity = 0;
    e->content_hash[0] = '\0';
    (void)name;
    return e;
}

void module_registry_entry_add_dep(ModuleRegistryEntry *entry, const char *dep_name)
{
    MOD_GROW(entry->dep_names, entry->dep_count, entry->dep_capacity, char *);
    entry->dep_names[entry->dep_count++] = mod_xstrdup(dep_name);
}

void module_registry_entry_free(ModuleRegistryEntry *entry)
{
    if (!entry) return;
    module_decl_free(entry->decl);
    free(entry->source_path);
    for (size_t i = 0; i < entry->dep_count; i++)
        free(entry->dep_names[i]);
    free(entry->dep_names);
    free(entry);
}

void module_registry_add(ModuleRegistry *registry, ModuleRegistryEntry *entry)
{
    if (!entry->decl || !entry->decl->name) return;

    /* Remove any prior entry for this module name first. */
    module_registry_remove(registry, entry->decl->name);

    uint32_t bucket = fnv1a32_str(entry->decl->name) % MODULE_INDEX_BUCKETS;
    entry->next = registry->buckets[bucket];
    registry->buckets[bucket] = entry;

    MOD_GROW(registry->sorted, registry->sorted_count, registry->sorted_capacity,
             ModuleRegistryEntry *);
    registry->sorted[registry->sorted_count++] = entry;
    registry->sorted_dirty = true;
}

ModuleRegistryEntry *module_registry_find(ModuleRegistry *registry, const char *name)
{
    if (!name) return NULL;
    uint32_t bucket = fnv1a32_str(name) % MODULE_INDEX_BUCKETS;
    for (ModuleRegistryEntry *e = registry->buckets[bucket]; e; e = e->next) {
        if (e->decl && e->decl->name && strcmp(e->decl->name, name) == 0)
            return e;
    }
    return NULL;
}

ModuleDecl *module_registry_find_decl(ModuleRegistry *registry, const char *name)
{
    ModuleRegistryEntry *e = module_registry_find(registry, name);
    return e ? e->decl : NULL;
}

void module_registry_remove(ModuleRegistry *registry, const char *name)
{
    if (!name) return;
    uint32_t bucket = fnv1a32_str(name) % MODULE_INDEX_BUCKETS;
    ModuleRegistryEntry **pp = &registry->buckets[bucket];
    while (*pp) {
        ModuleRegistryEntry *e = *pp;
        if (e->decl && e->decl->name && strcmp(e->decl->name, name) == 0) {
            *pp = e->next;
            for (size_t i = 0; i < registry->sorted_count; i++) {
                if (registry->sorted[i] == e) {
                    registry->sorted[i] = registry->sorted[--registry->sorted_count];
                    registry->sorted_dirty = true;
                    break;
                }
            }
            module_registry_entry_free(e);
            return;
        }
        pp = &(*pp)->next;
    }
}

void module_registry_free(ModuleRegistry *registry)
{
    if (!registry) return;
    for (size_t b = 0; b < MODULE_INDEX_BUCKETS; b++) {
        ModuleRegistryEntry *e = registry->buckets[b];
        while (e) {
            ModuleRegistryEntry *next = e->next;
            module_registry_entry_free(e);
            e = next;
        }
    }
    free(registry->sorted);
    free(registry);
}


/// §7  Dependency graph

char **module_dep_list_from_context(ModuleContext *ctx, size_t *out_count)
{
    *out_count = ctx->import_count;
    if (ctx->import_count == 0) return NULL;

    char **deps = mod_xmalloc(sizeof(char *) * ctx->import_count);
    for (size_t i = 0; i < ctx->import_count; i++)
        deps[i] = mod_xstrdup(ctx->imports[i]->module_name);
    return deps;
}

void module_dep_list_free(char **deps, size_t count)
{
    if (!deps) return;
    for (size_t i = 0; i < count; i++)
        free(deps[i]);
    free(deps);
}

//// Topological sort
 //
 //  Iterative-via-recursion post-order DFS over the dependency graph
 //  rooted at `root_module`. Modules with no registry entry (not yet
 //  discovered) are included in the order but treated as leaves — the
 //  build driver is responsible for discovering and registering them
 //  before their dependents are compiled.
 //
 //  Cycle detection uses a simple three-color (white/grey/black) scheme.
 //  On finding a cycle, traversal stops and `cycle_module` is set to the
 //  name of the module whose self-dependency closed the cycle.
 //
typedef struct {
    char **names;
    int *state;        /* 0 = white, 1 = grey, 2 = black */
    size_t count;
    size_t capacity;
} DepVisitSet;

static void dep_visit_set_init(DepVisitSet *s)
{
    s->names = NULL;
    s->state = NULL;
    s->count = 0;
    s->capacity = 0;
}

/* Find (or create) the visit-state slot for `name`. Both `names` and
   `state` share the same count/capacity, so a single MOD_GROW call
   (sized for the larger element) keeps them in sync. */
static size_t dep_visit_index(DepVisitSet *s, const char *name)
{
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->names[i], name) == 0)
            return i;
    }

    if (s->count >= s->capacity) {
        size_t new_cap = s->capacity ? s->capacity * 2 : 8;
        s->names = mod_xrealloc(s->names, new_cap * sizeof(char *));
        s->state = mod_xrealloc(s->state, new_cap * sizeof(int));
        s->capacity = new_cap;
    }

    size_t idx = s->count;
    s->names[idx] = mod_xstrdup(name);
    s->state[idx] = 0;
    s->count++;
    return idx;
}

static void dep_visit_set_free(DepVisitSet *s)
{
    for (size_t i = 0; i < s->count; i++)
        free(s->names[i]);
    free(s->names);
    free(s->state);
}

static bool dep_dfs(ModuleRegistry *registry, DepVisitSet *visit,
                     const char *name, char ***order, size_t *order_count,
                     size_t *order_cap, char **cycle_module)
{
    size_t idx = dep_visit_index(visit, name);
    if (visit->state[idx] == 1) {
        *cycle_module = mod_xstrdup(name);
        return false;
    }
    if (visit->state[idx] == 2) return true;

    visit->state[idx] = 1; /* grey */

    ModuleRegistryEntry *entry = module_registry_find(registry, name);
    if (entry) {
        for (size_t i = 0; i < entry->dep_count; i++) {
            if (!dep_dfs(registry, visit, entry->dep_names[i],
                          order, order_count, order_cap, cycle_module))
                return false;
        }
    }

    /* Re-fetch index: visit->names/state may have been reallocated by
       recursive calls growing the arrays. */
    idx = dep_visit_index(visit, name);
    visit->state[idx] = 2; /* black */

    MOD_GROW(*order, *order_count, *order_cap, char *);
    (*order)[(*order_count)++] = mod_xstrdup(name);
    return true;
}

ModuleDepOrder *module_dep_order_compute(ModuleRegistry *registry,
                                          const char *root_module)
{
    ModuleDepOrder *result = mod_xcalloc(1, sizeof(*result));

    DepVisitSet visit;
    dep_visit_set_init(&visit);

    char **order = NULL;
    size_t order_count = 0, order_cap = 0;
    char *cycle = NULL;

    bool ok = dep_dfs(registry, &visit, root_module,
                       &order, &order_count, &order_cap, &cycle);

    if (!ok) {
        for (size_t i = 0; i < order_count; i++) free(order[i]);
        free(order);
        result->order = NULL;
        result->order_count = 0;
        result->cycle_module = cycle;
    } else {
        result->order = order;
        result->order_count = order_count;
        result->cycle_module = NULL;
    }

    dep_visit_set_free(&visit);
    return result;
}

void module_dep_order_free(ModuleDepOrder *order)
{
    if (!order) return;
    for (size_t i = 0; i < order->order_count; i++)
        free(order->order[i]);
    free(order->order);
    free(order->cycle_module);
    free(order);
}


/// §8  Symbol resolution

char *module_resolve_symbol(ModuleContext *ctx, const char *symbol)
{
    /* Already qualified (contains '.')? */
    const char *dot = strchr(symbol, '.');
    if (dot) {
        size_t prefix_len = (size_t)(dot - symbol);
        char *prefix = mod_xmalloc(prefix_len + 1);
        memcpy(prefix, symbol, prefix_len);
        prefix[prefix_len] = '\0';

        ImportDecl *import = module_context_find_import(ctx, prefix);
        free(prefix);

        if (import) {
            const char *sym_name = dot + 1;
            if (import_decl_includes_symbol(import, sym_name))
                return mod_xstrdup(symbol);
        }
        return NULL;
    }

    /* Unqualified: check all non-qualified-only imports. */
    for (size_t i = 0; i < ctx->import_count; i++) {
        ImportDecl *import = ctx->imports[i];
        if (import->mode == IMPORT_QUALIFIED) continue;

        if (import_decl_includes_symbol(import, symbol)) {
            const char *prefix = import_decl_prefix(import);
            size_t len = strlen(prefix) + 1 + strlen(symbol) + 1;
            char *result = mod_xmalloc(len);
            snprintf(result, len, "%s.%s", prefix, symbol);
            return result;
        }
    }

    /* Possibly a local symbol. */
    return mod_xstrdup(symbol);
}

bool module_resolve_export(ModuleRegistry *registry, ModuleDecl *decl,
                            const char *symbol)
{
    if (!decl) return true;

    if (module_decl_is_exported(decl, symbol))
        return true;

    /* Follow re-exports transitively. */
    for (size_t i = 0; i < decl->reexport_count; i++) {
        ReExportDecl *re = decl->reexports[i];

        bool selected = re->reexport_all || export_list_contains(&re->symbols, symbol);
        if (!selected) continue;

        ModuleDecl *target = module_registry_find_decl(registry, re->module_name);
        if (!target) {
            /* Target not yet registered: trust the re-export declaration. */
            return true;
        }
        if (module_resolve_export(registry, target, symbol))
            return true;
    }

    return false;
}


/// §9  Module file path resolution

// Recursively search dir for <module_name>.mon, returning a strdup'd
// path on success or NULL if not found.
static char *find_mon_recursive(const char *dir, const char *module_name)
{
    char target[512];
    snprintf(target, sizeof(target), "%s.mon", module_name);

    DIR *d = opendir(dir);
    if (!d) return NULL;

    char *result = NULL;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && !result) {
        if (ent->d_name[0] == '.') continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);

        if (strcmp(ent->d_name, target) == 0) {
            result = mod_xstrdup(full);
        } else {
            struct stat st;
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
                result = find_mon_recursive(full, module_name);
        }
    }

    closedir(d);
    return result;
}

char *module_name_to_path(const char *module_name)
{
    char rel[512];
    size_t len = strlen(module_name);
    if (len >= sizeof(rel)) len = sizeof(rel) - 1;
    for (size_t i = 0; i < len; i++)
        rel[i] = (module_name[i] == '.') ? '/' : module_name[i];
    rel[len] = '\0';

    char candidate[1024];

    /* 1. Local: ./ModuleName.mon */
    snprintf(candidate, sizeof(candidate), "%s.mon", rel);
    if (access(candidate, F_OK) == 0) return mod_xstrdup(candidate);

    /* 1b. Local: ./ModuleName.monad */
    snprintf(candidate, sizeof(candidate), "%s.monad", rel);
    if (access(candidate, F_OK) == 0) return mod_xstrdup(candidate);

    /* 2. $MONAD_CORE (development without installing) */
    const char *env_core = getenv("MONAD_CORE");
    if (env_core) {
        char *found = find_mon_recursive(env_core, module_name);
        if (found) return found;
    }

    /* 3. Installed core */
    {
        char *found = find_mon_recursive("/usr/local/lib/monad/core", module_name);
        if (found) return found;
    }

    /* Not found — return the local .mon path; caller reports the error. */
    snprintf(candidate, sizeof(candidate), "%s.mon", rel);
    return mod_xstrdup(candidate);
}


/// §10  Parsing module/import/export forms

ModuleDecl *parse_module_decl(AST *ast)
{
    /* (module Name) | (module Name []) | (module Name [f1 f2]) */
    if (ast->type != AST_LIST || ast->list.count < 2) return NULL;

    AST *head = ast->list.items[0];
    if (head->type != AST_SYMBOL || strcmp(head->symbol, "module") != 0)
        return NULL;

    AST *name_ast = ast->list.items[1];
    if (name_ast->type != AST_SYMBOL) {
        fprintf(stderr, "Module name must be a symbol\n");
        return NULL;
    }

    if (ast->list.count == 2)
        return module_decl_create(name_ast->symbol, EXPORT_ALL);

    if (ast->list.count != 3) {
        fprintf(stderr, "Invalid module declaration syntax\n");
        return NULL;
    }

    AST *exports_ast = ast->list.items[2];
    if (exports_ast->type != AST_ARRAY) {
        fprintf(stderr, "Invalid export list in module declaration\n");
        return NULL;
    }

    if (exports_ast->array.element_count == 0)
        return module_decl_create(name_ast->symbol, EXPORT_NONE);

    ModuleDecl *decl = module_decl_create(name_ast->symbol, EXPORT_SELECTED);
    if (!decl) return NULL;

    for (size_t i = 0; i < exports_ast->array.element_count; i++) {
        AST *sym = exports_ast->array.elements[i];
        if (sym->type == AST_SYMBOL)
            module_decl_add_export(decl, sym->symbol);
    }
    return decl;
}

ReExportDecl *parse_export_decl(AST *ast)
{
    /* (export Std.Math)            -> reexport everything from Std.Math
       (export Std.Math [sqrt log]) -> reexport selected symbols       */
    if (ast->type != AST_LIST || ast->list.count < 2) return NULL;

    AST *head = ast->list.items[0];
    if (head->type != AST_SYMBOL || strcmp(head->symbol, "export") != 0)
        return NULL;

    AST *name_ast = ast->list.items[1];
    if (name_ast->type != AST_SYMBOL) {
        fprintf(stderr, "Expected module name in export declaration\n");
        return NULL;
    }

    if (ast->list.count == 2)
        return reexport_decl_create(name_ast->symbol, /*reexport_all=*/true);

    AST *list_ast = ast->list.items[2];
    if (list_ast->type != AST_ARRAY) {
        fprintf(stderr, "Expected symbol array in export declaration\n");
        return NULL;
    }

    ReExportDecl *re = reexport_decl_create(name_ast->symbol, /*reexport_all=*/false);
    for (size_t i = 0; i < list_ast->array.element_count; i++) {
        AST *sym = list_ast->array.elements[i];
        if (sym->type == AST_SYMBOL)
            reexport_decl_add_symbol(re, sym->symbol);
    }
    return re;
}

//// Import alias validation
 //
 //  Aliases (`:as Name`) must start with an uppercase letter, matching
 //  module-name conventions. Violations are reported with source position
 //  when available and abort parsing.
 //
static void require_uppercase_alias(const AST *alias_ast, const char *alias)
{
    if (alias && isupper((unsigned char)alias[0])) return;

    fprintf(stderr,
            "%d:%d: error: module alias '%s' must start with an "
            "uppercase letter (e.g. :as %c%s)\n",
            alias_ast ? alias_ast->line : 0,
            alias_ast ? alias_ast->column : 0,
            alias ? alias : "",
            alias && alias[0] ? (char)toupper((unsigned char)alias[0]) : '?',
            (alias && alias[0]) ? alias + 1 : "");
    exit(1);
}

ImportDecl *parse_import_decl(AST *ast)
{
    /* (import qualified Std.Math :as Math)
       (import qualified Math)
       (import Std.Math :as Math)
       (import Std.Math [sqrt log])
       (import Std.Math hiding [sqrt log]) */

    if (ast->type != AST_LIST || ast->list.count < 2) return NULL;

    AST *head = ast->list.items[0];
    if (head->type != AST_SYMBOL || strcmp(head->symbol, "import") != 0)
        return NULL;

    size_t idx = 1;
    bool qualified = false;

    if (idx < ast->list.count && ast->list.items[idx]->type == AST_SYMBOL &&
        strcmp(ast->list.items[idx]->symbol, "qualified") == 0) {
        qualified = true;
        idx++;
    }

    if (idx >= ast->list.count || ast->list.items[idx]->type != AST_SYMBOL) {
        fprintf(stderr, "Expected module name in import declaration\n");
        return NULL;
    }
    const char *module_name = ast->list.items[idx]->symbol;
    idx++;

    /* Optional leading :as alias */
    char *alias = NULL;
    if (idx < ast->list.count && ast->list.items[idx]->type == AST_KEYWORD &&
        strcmp(ast->list.items[idx]->keyword, "as") == 0) {
        idx++;
        if (idx >= ast->list.count || ast->list.items[idx]->type != AST_SYMBOL) {
            fprintf(stderr, "Expected alias after :as\n");
            return NULL;
        }
        alias = ast->list.items[idx]->symbol;
        require_uppercase_alias(ast->list.items[idx], alias);
        idx++;
    }

    ImportMode mode;
    ImportDecl *decl;

    if (qualified) {
        /* A qualified import may also specify hiding or a selective list.
           e.g. (import qualified Math hiding [phi] :as M)
                (import qualified Math [sqrt])
           With neither, it imports everything (qualified). */
        if (idx < ast->list.count) {
            AST *spec = ast->list.items[idx];

            if (spec->type == AST_SYMBOL && strcmp(spec->symbol, "hiding") == 0) {
                idx++;
                if (idx >= ast->list.count || ast->list.items[idx]->type != AST_ARRAY) {
                    fprintf(stderr, "Expected array after 'hiding'\n");
                    return NULL;
                }
                mode = IMPORT_HIDING;
                decl = import_decl_create(module_name, alias, mode);
                AST *symbols_ast = ast->list.items[idx];
                idx++;
                for (size_t i = 0; i < symbols_ast->array.element_count; i++) {
                    AST *sym = symbols_ast->array.elements[i];
                    if (sym->type == AST_SYMBOL)
                        import_decl_add_symbol(decl, sym->symbol);
                }
            } else if (spec->type == AST_ARRAY) {
                mode = IMPORT_SELECTIVE;
                decl = import_decl_create(module_name, alias, mode);
                idx++;
                for (size_t i = 0; i < spec->array.element_count; i++) {
                    AST *sym = spec->array.elements[i];
                    if (sym->type == AST_SYMBOL)
                        import_decl_add_symbol(decl, sym->symbol);
                }
            } else {
                mode = IMPORT_QUALIFIED;
                decl = import_decl_create(module_name, alias, mode);
            }
        } else {
            mode = IMPORT_QUALIFIED;
            decl = import_decl_create(module_name, alias, mode);
        }
        decl->qualified = true;

        /* A trailing :as may appear after hiding/selective spec:
             (import qualified Math hiding [phi] :as M) */
        if (!decl->alias && idx < ast->list.count &&
            ast->list.items[idx]->type == AST_KEYWORD &&
            strcmp(ast->list.items[idx]->keyword, "as") == 0) {
            idx++;
            if (idx < ast->list.count && ast->list.items[idx]->type == AST_SYMBOL) {
                const char *late_alias = ast->list.items[idx]->symbol;
                require_uppercase_alias(ast->list.items[idx], late_alias);
                decl->alias = mod_xstrdup(late_alias);
                idx++;
            }
        }
        return decl;
    }

    /* Unqualified */
    if (idx < ast->list.count) {
        AST *spec = ast->list.items[idx];

        if (spec->type == AST_SYMBOL && strcmp(spec->symbol, "hiding") == 0) {
            idx++;
            if (idx >= ast->list.count || ast->list.items[idx]->type != AST_ARRAY) {
                fprintf(stderr, "Expected array after 'hiding'\n");
                return NULL;
            }
            mode = IMPORT_HIDING;
            decl = import_decl_create(module_name, alias, mode);
            AST *symbols_ast = ast->list.items[idx];
            for (size_t i = 0; i < symbols_ast->array.element_count; i++) {
                AST *sym = symbols_ast->array.elements[i];
                if (sym->type == AST_SYMBOL)
                    import_decl_add_symbol(decl, sym->symbol);
            }
        } else if (spec->type == AST_ARRAY) {
            mode = IMPORT_SELECTIVE;
            decl = import_decl_create(module_name, alias, mode);
            for (size_t i = 0; i < spec->array.element_count; i++) {
                AST *sym = spec->array.elements[i];
                if (sym->type == AST_SYMBOL)
                    import_decl_add_symbol(decl, sym->symbol);
            }
        } else {
            fprintf(stderr, "Invalid import specification\n");
            return NULL;
        }
    } else {
        mode = IMPORT_UNQUALIFIED;
        decl = import_decl_create(module_name, alias, mode);
    }

    decl->qualified = false;
    return decl;
}
