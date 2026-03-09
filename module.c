#include "module.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>  // for access()

/// Helper functions

static char *my_strdup(const char *s) {
    if (!s) return NULL;
    char *r = malloc(strlen(s) + 1);
    if (!r) return NULL;
    strcpy(r, s);
    return r;
}

static void export_list_init(ExportList *list) {
    list->symbols = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void export_list_add(ExportList *list, const char *symbol) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->symbols = realloc(list->symbols, sizeof(char *) * list->capacity);
    }
    list->symbols[list->count++] = my_strdup(symbol);
}

static void export_list_free(ExportList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->symbols[i]);
    }
    free(list->symbols);
}

/// Module context

ModuleContext *module_context_create(void) {
    ModuleContext *ctx = calloc(1, sizeof(ModuleContext));
    ctx->import_capacity = 4;
    ctx->imports = malloc(sizeof(ImportDecl *) * ctx->import_capacity);
    return ctx;
}

void module_context_free(ModuleContext *ctx) {
    if (!ctx) return;

    if (ctx->decl) {
        module_decl_free(ctx->decl);
    }

    for (size_t i = 0; i < ctx->import_count; i++) {
        import_decl_free(ctx->imports[i]);
    }
    free(ctx->imports);
    free(ctx->current_file);
    free(ctx);
}

void module_context_set_file(ModuleContext *ctx, const char *filename) {
    free(ctx->current_file);
    ctx->current_file = my_strdup(filename);
}

void module_context_set_decl(ModuleContext *ctx, ModuleDecl *decl) {
    if (ctx->decl) {
        module_decl_free(ctx->decl);
    }
    ctx->decl = decl;
}

void module_context_add_import(ModuleContext *ctx, ImportDecl *import) {
    if (ctx->import_count >= ctx->import_capacity) {
        ctx->import_capacity *= 2;
        ctx->imports = realloc(ctx->imports, sizeof(ImportDecl *) * ctx->import_capacity);
    }
    ctx->imports[ctx->import_count++] = import;
}

/* Find an import by prefix.  The prefix is matched against:
   1. The alias (`:as Name`) if one was given, OR
   2. The last component of the module name when no alias was given.
      e.g. `(import qualified Math)` → prefix "Math" matches module "Math".
      e.g. `(import qualified Std.Math)` → prefix "Math" matches too, because
           the last dot-component of "Std.Math" is "Math".
   This allows `Math.phi` to resolve even when no `:as` was written. */
ImportDecl *module_context_find_import(ModuleContext *ctx, const char *prefix) {
    for (size_t i = 0; i < ctx->import_count; i++) {
        ImportDecl *imp = ctx->imports[i];

        // Match against explicit alias first
        if (imp->alias && strcmp(imp->alias, prefix) == 0) {
            return imp;
        }

        // When there is no alias, match against the last component of the
        // module name (everything after the last '.', or the whole name).
        if (!imp->alias) {
            const char *last_dot = strrchr(imp->module_name, '.');
            const char *last_component = last_dot ? last_dot + 1 : imp->module_name;
            if (strcmp(last_component, prefix) == 0) {
                return imp;
            }
        }
    }
    return NULL;
}

/// Module declaration

bool module_name_is_valid(const char *name) {
    if (!name || !*name) return false;

    // Must start with uppercase letter
    if (!isupper(name[0])) return false;

    // Can contain letters, digits, dots, and underscores
    for (const char *p = name; *p; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '_') {
            return false;
        }
    }

    return true;
}

ModuleDecl *module_decl_create(const char *name, ExportMode mode) {
    if (!module_name_is_valid(name)) {
        fprintf(stderr, "Invalid module name: %s (must start with uppercase)\n", name);
        return NULL;
    }

    ModuleDecl *decl = calloc(1, sizeof(ModuleDecl));
    decl->name = my_strdup(name);
    decl->mode = mode;
    export_list_init(&decl->exports);
    return decl;
}

void module_decl_free(ModuleDecl *decl) {
    if (!decl) return;
    free(decl->name);
    export_list_free(&decl->exports);
    free(decl);
}

void module_decl_add_export(ModuleDecl *decl, const char *symbol) {
    export_list_add(&decl->exports, symbol);
}

bool module_decl_is_exported(ModuleDecl *decl, const char *symbol) {
    if (!decl) return true; // No module declaration means everything is exported

    switch (decl->mode) {
    case EXPORT_ALL:
        return true;
    case EXPORT_NONE:
        return false;
    case EXPORT_SELECTED:
        for (size_t i = 0; i < decl->exports.count; i++) {
            if (strcmp(decl->exports.symbols[i], symbol) == 0) {
                return true;
            }
        }
        return false;
    }
    return false;
}

/// Import declaration

ImportDecl *import_decl_create(const char *module_name, const char *alias, ImportMode mode) {
    ImportDecl *decl = calloc(1, sizeof(ImportDecl));
    decl->module_name = my_strdup(module_name);
    decl->alias = alias ? my_strdup(alias) : NULL;
    decl->mode = mode;
    decl->symbols = NULL;
    decl->symbol_count = 0;
    return decl;
}

void import_decl_free(ImportDecl *decl) {
    if (!decl) return;
    free(decl->module_name);
    free(decl->alias);
    for (size_t i = 0; i < decl->symbol_count; i++) {
        free(decl->symbols[i]);
    }
    free(decl->symbols);
    free(decl);
}

void import_decl_add_symbol(ImportDecl *decl, const char *symbol) {
    decl->symbols = realloc(decl->symbols, sizeof(char *) * (decl->symbol_count + 1));
    decl->symbols[decl->symbol_count++] = my_strdup(symbol);
}

bool import_decl_includes_symbol(ImportDecl *decl, const char *symbol) {
    switch (decl->mode) {
    case IMPORT_QUALIFIED:
    case IMPORT_UNQUALIFIED:
        // All symbols are imported
        return true;

    case IMPORT_SELECTIVE:
        // Only listed symbols are imported
        for (size_t i = 0; i < decl->symbol_count; i++) {
            if (strcmp(decl->symbols[i], symbol) == 0) {
                return true;
            }
        }
        return false;

    case IMPORT_HIDING:
        // All except listed symbols are imported
        for (size_t i = 0; i < decl->symbol_count; i++) {
            if (strcmp(decl->symbols[i], symbol) == 0) {
                return false;
            }
        }
        return true;
    }
    return false;
}

/// Symbol resolution

char *module_resolve_symbol(ModuleContext *ctx, const char *symbol) {
    // Check if it's already qualified (contains '.')
    if (strchr(symbol, '.')) {
        // Extract the prefix and check if it's a valid import
        char *dot = strchr(symbol, '.');
        size_t prefix_len = dot - symbol;
        char *prefix = malloc(prefix_len + 1);
        memcpy(prefix, symbol, prefix_len);
        prefix[prefix_len] = '\0';

        ImportDecl *import = module_context_find_import(ctx, prefix);
        if (import) {
            // Check if the symbol is included in this import
            const char *sym_name = dot + 1;
            if (import_decl_includes_symbol(import, sym_name)) {
                char *result = my_strdup(symbol);
                free(prefix);
                return result;
            }
        }
        free(prefix);
        return NULL;
    }

    // Unqualified symbol - check all unqualified imports
    for (size_t i = 0; i < ctx->import_count; i++) {
        ImportDecl *import = ctx->imports[i];

        if (import->mode == IMPORT_QUALIFIED) {
            continue; // Skip qualified imports for unqualified symbols
        }

        if (import_decl_includes_symbol(import, symbol)) {
            // Found it in an unqualified import
            const char *last_dot = strrchr(import->module_name, '.');
            const char *prefix = import->alias
                ? import->alias
                : (last_dot ? last_dot + 1 : import->module_name);

            // Build qualified name: prefix.symbol
            size_t len = strlen(prefix) + 1 + strlen(symbol) + 1;
            char *result = malloc(len);
            snprintf(result, len, "%s.%s", prefix, symbol);
            return result;
        }
    }

    // Not found in any import - might be a local symbol
    return my_strdup(symbol);
}

/// Module registry

ModuleRegistry *module_registry_create(void) {
    ModuleRegistry *registry = calloc(1, sizeof(ModuleRegistry));
    registry->capacity = 8;
    registry->modules = malloc(sizeof(ModuleDecl *) * registry->capacity);
    return registry;
}

void module_registry_free(ModuleRegistry *registry) {
    if (!registry) return;
    for (size_t i = 0; i < registry->count; i++) {
        module_decl_free(registry->modules[i]);
    }
    free(registry->modules);
    free(registry);
}

void module_registry_add(ModuleRegistry *registry, ModuleDecl *decl) {
    if (registry->count >= registry->capacity) {
        registry->capacity *= 2;
        registry->modules = realloc(registry->modules, sizeof(ModuleDecl *) * registry->capacity);
    }
    registry->modules[registry->count++] = decl;
}

ModuleDecl *module_registry_find(ModuleRegistry *registry, const char *name) {
    for (size_t i = 0; i < registry->count; i++) {
        if (strcmp(registry->modules[i]->name, name) == 0) {
            return registry->modules[i];
        }
    }
    return NULL;
}

/// Module file path resolution

#include <dirent.h>
#include <sys/stat.h>

// Recursively search dir for <module_name>.mon
// Returns a strdup'd path on success, NULL if not found
static char *find_mon_recursive(const char *dir, const char *module_name) {
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
            result = strdup(full);
        } else {
            struct stat st;
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
                result = find_mon_recursive(full, module_name);
        }
    }

    closedir(d);
    return result;
}

char *module_name_to_path(const char *module_name) {
    // Convert dots to slashes for nested module names
    char rel[512];
    size_t len = strlen(module_name);
    for (size_t i = 0; i < len; i++)
        rel[i] = (module_name[i] == '.') ? '/' : module_name[i];
    rel[len] = '\0';

    char candidate[1024];

    // 1. Local: ./ModuleName.mon
    snprintf(candidate, sizeof(candidate), "%s.mon", rel);
    if (access(candidate, F_OK) == 0) return strdup(candidate);

    // 2. $MONAD_CORE env var (development without installing)
    const char *env_core = getenv("MONAD_CORE");
    if (env_core) {
        char *found = find_mon_recursive(env_core, module_name);
        if (found) return found;
    }

    // 3. Installed core: recursive search under /usr/local/lib/monad/core/
    {
        char *found = find_mon_recursive("/usr/local/lib/monad/core", module_name);
        if (found) return found;
    }

    // Not found — return local path as before (caller will error)
    snprintf(candidate, sizeof(candidate), "%s.mon", rel);
    return strdup(candidate);
}

/* char *module_name_to_path(const char *module_name) { */
/*     // Convert "Std.Math" to "Std/Math.mon" or "Std/Math.monad" */
/*     size_t len = strlen(module_name); */
/*     char *path = malloc(len + 7); // +7 for ".monad" + null terminator */

/*     strcpy(path, module_name); */

/*     // Replace dots with slashes */
/*     for (char *p = path; *p; p++) { */
/*         if (*p == '.') { */
/*             *p = '/'; */
/*         } */
/*     } */

/*     // Try .mon first (preferred extension) */
/*     strcat(path, ".mon"); */

/*     // Check if .mon file exists */
/*     if (access(path, F_OK) == 0) { */
/*         return path; */
/*     } */

/*     // If not, try .monad */
/*     strcpy(path + strlen(path) - 4, ".monad"); */

/*     return path; */
/* } */

/// Parse module and import declarations

ModuleDecl *parse_module_decl(AST *ast) {
    // (module Name) or (module Name [exports...])
    if (ast->type != AST_LIST || ast->list.count < 2) {
        return NULL;
    }

    AST *head = ast->list.items[0];
    if (head->type != AST_SYMBOL || strcmp(head->symbol, "module") != 0) {
        return NULL;
    }

    AST *name_ast = ast->list.items[1];
    if (name_ast->type != AST_SYMBOL) {
        fprintf(stderr, "Module name must be a symbol\n");
        return NULL;
    }

    // Determine export mode
    ExportMode mode;
    ModuleDecl *decl;

    if (ast->list.count == 2) {
        // (module Name) - export everything
        mode = EXPORT_ALL;
        decl = module_decl_create(name_ast->symbol, mode);
    } else if (ast->list.count == 3) {
        AST *exports_ast = ast->list.items[2];

        if (exports_ast->type == AST_ARRAY && exports_ast->array.element_count == 0) {
            // (module Name []) - export nothing
            mode = EXPORT_NONE;
            decl = module_decl_create(name_ast->symbol, mode);
        } else if (exports_ast->type == AST_ARRAY) {
            // (module Name [f1 f2]) - export selected
            mode = EXPORT_SELECTED;
            decl = module_decl_create(name_ast->symbol, mode);

            for (size_t i = 0; i < exports_ast->array.element_count; i++) {
                AST *sym = exports_ast->array.elements[i];
                if (sym->type == AST_SYMBOL) {
                    module_decl_add_export(decl, sym->symbol);
                }
            }
        } else {
            fprintf(stderr, "Invalid export list in module declaration\n");
            return NULL;
        }
    } else {
        fprintf(stderr, "Invalid module declaration syntax\n");
        return NULL;
    }

    return decl;
}

ImportDecl *parse_import_decl(AST *ast) {
    // (import qualified Std.Math :as Math)  ; qualified, aliased
    // (import qualified Math)               ; qualified, no alias → use "Math" as prefix
    // (import Std.Math :as Math)            ; unqualified, aliased
    // (import Std.Math [sqrt log])          ; selective
    // (import Std.Math hiding [sqrt log])   ; hiding

    if (ast->type != AST_LIST || ast->list.count < 2) {
        return NULL;
    }

    AST *head = ast->list.items[0];
    if (head->type != AST_SYMBOL || strcmp(head->symbol, "import") != 0) {
        return NULL;
    }

    size_t idx = 1;
    bool qualified = false;

    // Check for 'qualified' keyword
    if (idx < ast->list.count && ast->list.items[idx]->type == AST_SYMBOL &&
        strcmp(ast->list.items[idx]->symbol, "qualified") == 0) {
        qualified = true;
        idx++;
    }

    // Get module name
    if (idx >= ast->list.count || ast->list.items[idx]->type != AST_SYMBOL) {
        fprintf(stderr, "Expected module name in import declaration\n");
        return NULL;
    }

    const char *module_name = ast->list.items[idx]->symbol;
    idx++;

    // Check for :as alias
    char *alias = NULL;
    if (idx < ast->list.count && ast->list.items[idx]->type == AST_KEYWORD &&
        strcmp(ast->list.items[idx]->keyword, "as") == 0) {
        idx++;

        if (idx >= ast->list.count || ast->list.items[idx]->type != AST_SYMBOL) {
            fprintf(stderr, "Expected alias after :as\n");
            return NULL;
        }

        alias = ast->list.items[idx]->symbol;

        // ── NEW RULE: alias must start with an uppercase letter ──────────────
        if (!alias || !isupper((unsigned char)alias[0])) {
            fprintf(stderr, "%s:%d:%d: error: module alias '%s' must start with "
                    "an uppercase letter (e.g. :as %c%s)\n",
                    ast->list.items[idx]->type == AST_SYMBOL
                        ? "(import)" : "<import>",
                    ast->list.items[idx]->line,
                    ast->list.items[idx]->column,
                    alias,
                    alias ? toupper((unsigned char)alias[0]) : '?',
                    alias && alias[0] ? alias + 1 : "");
            exit(1);
        }
        // ────────────────────────────────────────────────────────────────────

        idx++;
    }

    // Determine import mode
    ImportMode mode;
    ImportDecl *decl;

    if (qualified) {
        /* A qualified import can optionally also specify hiding or selective.
           e.g. (import qualified Math hiding [phi] :as M)
                (import qualified Math [sqrt])
           When neither is present it imports everything (qualified).        */
        if (idx < ast->list.count) {
            AST *spec = ast->list.items[idx];

            if (spec->type == AST_SYMBOL && strcmp(spec->symbol, "hiding") == 0) {
                mode = IMPORT_HIDING;
                idx++;

                if (idx >= ast->list.count || ast->list.items[idx]->type != AST_ARRAY) {
                    fprintf(stderr, "Expected array after 'hiding'\n");
                    return NULL;
                }

                decl = import_decl_create(module_name, alias, mode);
                AST *symbols_ast = ast->list.items[idx];
                idx++;

                for (size_t i = 0; i < symbols_ast->array.element_count; i++) {
                    AST *sym = symbols_ast->array.elements[i];
                    if (sym->type == AST_SYMBOL) {
                        import_decl_add_symbol(decl, sym->symbol);
                    }
                }
            } else if (spec->type == AST_ARRAY) {
                mode = IMPORT_SELECTIVE;
                decl = import_decl_create(module_name, alias, mode);
                idx++;

                for (size_t i = 0; i < spec->array.element_count; i++) {
                    AST *sym = spec->array.elements[i];
                    if (sym->type == AST_SYMBOL) {
                        import_decl_add_symbol(decl, sym->symbol);
                    }
                }
            } else {
                /* No extra spec — plain qualified import */
                mode = IMPORT_QUALIFIED;
                decl = import_decl_create(module_name, alias, mode);
            }
        } else {
            mode = IMPORT_QUALIFIED;
            decl = import_decl_create(module_name, alias, mode);
        }

        /* Re-check :as that might appear AFTER hiding/selective spec.
           e.g. (import qualified Math hiding [phi] :as M)
           The alias was already consumed above if it came before the spec.
           If alias is still NULL here, check for a trailing :as. */
        if (!decl->alias && idx < ast->list.count &&
            ast->list.items[idx]->type == AST_KEYWORD &&
            strcmp(ast->list.items[idx]->keyword, "as") == 0) {
            idx++;
            if (idx < ast->list.count && ast->list.items[idx]->type == AST_SYMBOL) {
                const char *late_alias = ast->list.items[idx]->symbol;
                if (!isupper((unsigned char)late_alias[0])) {
                    fprintf(stderr, "%d:%d: error: module alias '%s' must start with "
                            "an uppercase letter\n",
                            ast->list.items[idx]->line,
                            ast->list.items[idx]->column,
                            late_alias);
                    import_decl_free(decl);
                    exit(1);
                }
                decl->alias = strdup(late_alias);
                idx++;
            }
        }

    } else if (idx < ast->list.count) {
        AST *spec = ast->list.items[idx];

        if (spec->type == AST_SYMBOL && strcmp(spec->symbol, "hiding") == 0) {
            // (import Std.Math hiding [symbols...])
            mode = IMPORT_HIDING;
            idx++;

            if (idx >= ast->list.count || ast->list.items[idx]->type != AST_ARRAY) {
                fprintf(stderr, "Expected array after 'hiding'\n");
                return NULL;
            }

            decl = import_decl_create(module_name, alias, mode);
            AST *symbols_ast = ast->list.items[idx];

            for (size_t i = 0; i < symbols_ast->array.element_count; i++) {
                AST *sym = symbols_ast->array.elements[i];
                if (sym->type == AST_SYMBOL) {
                    import_decl_add_symbol(decl, sym->symbol);
                }
            }
        } else if (spec->type == AST_ARRAY) {
            // (import Std.Math [symbols...])
            mode = IMPORT_SELECTIVE;
            decl = import_decl_create(module_name, alias, mode);

            for (size_t i = 0; i < spec->array.element_count; i++) {
                AST *sym = spec->array.elements[i];
                if (sym->type == AST_SYMBOL) {
                    import_decl_add_symbol(decl, sym->symbol);
                }
            }
        } else {
            fprintf(stderr, "Invalid import specification\n");
            return NULL;
        }
    } else {
        // (import Std.Math :as Math) - unqualified
        mode = IMPORT_UNQUALIFIED;
        decl = import_decl_create(module_name, alias, mode);
    }

    return decl;
}
