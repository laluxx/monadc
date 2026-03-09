#include "buildsystem.h"
#include "module.h"
#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *my_strdup(const char *s) {
    if (!s) return NULL;
    char *r = malloc(strlen(s) + 1);
    strcpy(r, s);
    return r;
}

/// Build Context

BuildContext *build_context_create(void) {
    BuildContext *ctx = calloc(1, sizeof(BuildContext));
    ctx->artifact_capacity = 8;
    ctx->artifacts = malloc(sizeof(ModuleArtifact *) * ctx->artifact_capacity);
    ctx->build_dir = my_strdup(".");
    ctx->verbose = false;
    ctx->force_rebuild = false;
    return ctx;
}

void build_context_free(BuildContext *ctx) {
    if (!ctx) return;

    for (size_t i = 0; i < ctx->artifact_count; i++) {
        build_artifact_free(ctx->artifacts[i]);
    }
    free(ctx->artifacts);
    free(ctx->build_dir);
    free(ctx);
}

void build_context_set_build_dir(BuildContext *ctx, const char *dir) {
    free(ctx->build_dir);
    ctx->build_dir = my_strdup(dir);
}

void build_context_set_verbose(BuildContext *ctx, bool verbose) {
    ctx->verbose = verbose;
}

void build_context_set_force_rebuild(BuildContext *ctx, bool force) {
    ctx->force_rebuild = force;
}

void build_context_add_artifact(BuildContext *ctx, ModuleArtifact *artifact) {
    if (ctx->artifact_count >= ctx->artifact_capacity) {
        ctx->artifact_capacity *= 2;
        ctx->artifacts = realloc(ctx->artifacts,
                                sizeof(ModuleArtifact *) * ctx->artifact_capacity);
    }
    ctx->artifacts[ctx->artifact_count++] = artifact;
}

ModuleArtifact *build_context_find_artifact(BuildContext *ctx, const char *module_name) {
    for (size_t i = 0; i < ctx->artifact_count; i++) {
        if (strcmp(ctx->artifacts[i]->module_name, module_name) == 0) {
            return ctx->artifacts[i];
        }
    }
    return NULL;
}

ModuleArtifact *build_context_find_by_source(BuildContext *ctx, const char *source_path) {
    for (size_t i = 0; i < ctx->artifact_count; i++) {
        if (strcmp(ctx->artifacts[i]->source_path, source_path) == 0) {
            return ctx->artifacts[i];
        }
    }
    return NULL;
}

/// Module Artifact

ModuleArtifact *build_artifact_create(const char *module_name, const char *source_path) {
    ModuleArtifact *artifact = calloc(1, sizeof(ModuleArtifact));
    artifact->module_name = my_strdup(module_name);
    artifact->source_path = my_strdup(source_path);

    // Generate object file path
    char obj_path[512];
    snprintf(obj_path, sizeof(obj_path), "%s.o", module_name);
    artifact->object_path = my_strdup(obj_path);

    artifact->source_mtime = 0;
    artifact->object_mtime = 0;
    artifact->needs_recompile = true;
    artifact->decl = NULL;
    artifact->env = NULL;

    build_artifact_update_times(artifact);

    return artifact;
}

void build_artifact_free(ModuleArtifact *artifact) {
    if (!artifact) return;
    free(artifact->module_name);
    free(artifact->source_path);
    free(artifact->object_path);
    // Note: we don't free decl or env - they're managed elsewhere
    free(artifact);
}

void build_artifact_update_times(ModuleArtifact *artifact) {
    artifact->source_mtime = build_get_file_mtime(artifact->source_path);
    artifact->object_mtime = build_get_file_mtime(artifact->object_path);
}

bool build_artifact_needs_rebuild(ModuleArtifact *artifact) {
    build_artifact_update_times(artifact);

    // If object doesn't exist, need to rebuild
    if (artifact->object_mtime == 0) {
        return true;
    }

    // If source is newer than object, need to rebuild
    if (artifact->source_mtime > artifact->object_mtime) {
        return true;
    }

    return false;
}

/// Utilities

time_t build_get_file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_mtime;
    }
    return 0;  // File doesn't exist
}

bool build_file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

char *build_module_name_to_source_path(const char *module_name) {
    // Convert "Std.Math" to "Std/Math.mon" or "Std/Math.monad"
    size_t len = strlen(module_name);
    char *path = malloc(len + 7);  // +7 for ".monad" + null

    strcpy(path, module_name);

    // Replace dots with slashes
    for (char *p = path; *p; p++) {
        if (*p == '.') *p = '/';
    }

    // Try .mon first (preferred extension)
    strcat(path, ".mon");
    if (access(path, F_OK) == 0) {
        return path;
    }

    // If not found, try .monad
    strcpy(path + strlen(path) - 4, ".monad");
    return path;
}

char *build_module_name_to_object_path(const char *module_name, const char *build_dir) {
    // Convert "Std.Math" to "build_dir/Std.Math.o"
    size_t len = strlen(build_dir) + strlen(module_name) + 4;
    char *path = malloc(len);
    snprintf(path, len, "%s/%s.o", build_dir, module_name);
    return path;
}

/// Dependency Resolution

// Helper to extract imports from a source file
static ImportDecl **extract_imports(const char *source_path, size_t *out_count) {
    *out_count = 0;

    // Read the source file
    FILE *f = fopen(source_path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *source = malloc(size + 1);
    fread(source, 1, size, f);
    source[size] = '\0';
    fclose(f);

    // Parse the source
    parser_set_context(source_path, source);
    ASTList exprs = parse_all(source);

    // Extract imports
    ImportDecl **imports = NULL;
    size_t import_capacity = 0;

    for (size_t i = 0; i < exprs.count; i++) {
        AST *expr = exprs.exprs[i];

        if (expr->type == AST_LIST && expr->list.count > 0 &&
            expr->list.items[0]->type == AST_SYMBOL &&
            strcmp(expr->list.items[0]->symbol, "import") == 0) {

            ImportDecl *import = parse_import_decl(expr);
            if (import) {
                if (*out_count >= import_capacity) {
                    import_capacity = import_capacity == 0 ? 4 : import_capacity * 2;
                    imports = realloc(imports, sizeof(ImportDecl *) * import_capacity);
                }
                imports[(*out_count)++] = import;
            }
        }

        // Stop at first non-module/import declaration
        if (expr->type == AST_LIST && expr->list.count > 0 &&
            expr->list.items[0]->type == AST_SYMBOL) {
            const char *head = expr->list.items[0]->symbol;
            if (strcmp(head, "module") != 0 && strcmp(head, "import") != 0) {
                break;
            }
        }
    }

    // Cleanup
    for (size_t i = 0; i < exprs.count; i++) {
        ast_free(exprs.exprs[i]);
    }
    free(exprs.exprs);
    free(source);

    return imports;
}

char **build_resolve_dependencies(ModuleContext *mod_ctx, size_t *out_count) {
    // Simple approach: return imports in order (no topological sort yet)
    // TODO: Implement proper dependency graph with cycle detection

    *out_count = mod_ctx->import_count;

    if (*out_count == 0) {
        return NULL;
    }

    char **deps = malloc(sizeof(char *) * *out_count);
    for (size_t i = 0; i < *out_count; i++) {
        deps[i] = my_strdup(mod_ctx->imports[i]->module_name);
    }

    return deps;
}

void build_free_dependency_list(char **deps, size_t count) {
    if (!deps) return;
    for (size_t i = 0; i < count; i++) {
        free(deps[i]);
    }
    free(deps);
}

/// Recursive compilation

// Forward declaration of compile function from main.c
// We'll need to expose this or refactor
extern void compile_module_to_object(const char *source_path, const char *output_object);

// Recursive helper to compile module and its dependencies
static bool compile_module_recursive(BuildContext *build_ctx,
                                     const char *module_name,
                                     const char *source_path,
                                     int depth) {
    // Check if already compiled and up-to-date
    ModuleArtifact *artifact = build_context_find_artifact(build_ctx, module_name);

    if (artifact && !build_ctx->force_rebuild && !build_artifact_needs_rebuild(artifact)) {
        if (build_ctx->verbose) {
            printf("%*s[%s] Up to date\n", depth * 2, "", module_name);
        }
        return true;
    }

    if (build_ctx->verbose) {
        printf("%*s[%s] Compiling...\n", depth * 2, "", module_name);
    }

    // Create artifact if it doesn't exist
    if (!artifact) {
        artifact = build_artifact_create(module_name, source_path);
        build_context_add_artifact(build_ctx, artifact);
    }

    // Extract imports from this module
    size_t import_count;
    ImportDecl **imports = extract_imports(source_path, &import_count);

    // Recursively compile dependencies
    for (size_t i = 0; i < import_count; i++) {
        const char *dep_name = imports[i]->module_name;
        char *dep_source = build_module_name_to_source_path(dep_name);

        if (!build_file_exists(dep_source)) {
            fprintf(stderr, "Error: Cannot find module '%s' (looking for %s)\n",
                    dep_name, dep_source);
            free(dep_source);
            return false;
        }

        bool success = compile_module_recursive(build_ctx, dep_name, dep_source, depth + 1);
        free(dep_source);

        if (!success) {
            return false;
        }
    }

    // Cleanup imports
    for (size_t i = 0; i < import_count; i++) {
        import_decl_free(imports[i]);
    }
    free(imports);

    // Now compile this module (dependencies are ready)
    if (build_ctx->verbose) {
        printf("%*s[%s] Building object file...\n", depth * 2, "", module_name);
    }

    // TODO: Call actual compile function
    // For now, this is a placeholder
    printf("%*s[%s] Compiled to %s\n", depth * 2, "", module_name, artifact->object_path);

    // Update artifact
    artifact->needs_recompile = false;
    build_artifact_update_times(artifact);

    return true;
}

ModuleArtifact *build_compile_with_deps(BuildContext *build_ctx,
                                         const char *source_path) {
    // Extract module name from source path
    // For now, use a simple approach - assume filename matches module name
    char *base_name = strdup(source_path);
    char *dot = strrchr(base_name, '.');
    if (dot) *dot = '\0';
    char *slash = strrchr(base_name, '/');
    char *module_name = slash ? slash + 1 : base_name;

    bool success = compile_module_recursive(build_ctx, module_name, source_path, 0);

    ModuleArtifact *result = success ? build_context_find_artifact(build_ctx, module_name) : NULL;
    free(base_name);

    return result;
}

bool build_link_executable(BuildContext *build_ctx,
                           const char *output_name,
                           ModuleArtifact **artifacts,
                           size_t artifact_count) {
    if (artifact_count == 0) {
        return false;
    }

    // Build gcc command with all object files
    char cmd[4096] = "gcc ";

    for (size_t i = 0; i < artifact_count; i++) {
        strcat(cmd, artifacts[i]->object_path);
        strcat(cmd, " ");
    }

    // Add runtime and libraries
    strcat(cmd, "runtime.o -o ");
    strcat(cmd, output_name);
    strcat(cmd, " `llvm-config --ldflags --libs core` -lm -no-pie");

    if (build_ctx->verbose) {
        printf("Linking: %s\n", cmd);
    }

    int ret = system(cmd);
    if (ret == 0) {
        printf("Created executable: %s\n", output_name);
        return true;
    } else {
        fprintf(stderr, "Failed to link executable\n");
        return false;
    }
}

bool build_project(const char *source_path,
                   const char *output_name,
                   bool verbose,
                   bool force_rebuild) {
    BuildContext *ctx = build_context_create();
    build_context_set_verbose(ctx, verbose);
    build_context_set_force_rebuild(ctx, force_rebuild);

    printf("Building project: %s\n", source_path);

    ModuleArtifact *main_artifact = build_compile_with_deps(ctx, source_path);

    if (!main_artifact) {
        fprintf(stderr, "Build failed\n");
        build_context_free(ctx);
        return false;
    }

    // Link all compiled modules
    bool success = build_link_executable(ctx, output_name,
                                         ctx->artifacts, ctx->artifact_count);

    build_context_free(ctx);
    return success;
}
