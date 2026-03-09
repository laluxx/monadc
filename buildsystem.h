#ifndef BUILDSYSTEM_H
#define BUILDSYSTEM_H

#include <stdbool.h>
#include <time.h>

// Build System - Handles dependency tracking and incremental compilation
//
// Features:
// - Timestamp-based change detection
// - Recursive dependency compilation
// - Caching of compiled modules
// - Build order resolution

typedef struct ModuleContext ModuleContext;
typedef struct ModuleDecl ModuleDecl;
typedef struct ImportDecl ImportDecl;

// Compiled module artifact info
typedef struct {
    char *module_name;     // Module name (e.g., "Std.Math")
    char *source_path;     // Source file path (e.g., "Std/Math.monad")
    char *object_path;     // Object file path (e.g., "Std/Math.o")
    time_t source_mtime;   // Last modification time of source
    time_t object_mtime;   // Last modification time of object
    bool needs_recompile;  // Whether this module needs recompilation
    ModuleDecl *decl;      // Module declaration (NULL if not compiled)
    void *env;             // Environment (Env*) - opaque pointer
} ModuleArtifact;

// Build context - tracks all modules in the build
typedef struct {
    ModuleArtifact **artifacts; // All known modules
    size_t artifact_count;
    size_t artifact_capacity;

    char *build_dir;            // Output directory for .o files (default: ".")
    bool verbose;               // Print detailed build info
    bool force_rebuild;         // Rebuild everything
} BuildContext;

// Build context management
BuildContext *build_context_create(void);
void build_context_free(BuildContext *ctx);
void build_context_set_build_dir(BuildContext *ctx, const char *dir);
void build_context_set_verbose(BuildContext *ctx, bool verbose);
void build_context_set_force_rebuild(BuildContext *ctx, bool force);

// Module artifact tracking
ModuleArtifact *build_artifact_create(const char *module_name, const char *source_path);
void build_artifact_free(ModuleArtifact *artifact);
void build_artifact_update_times(ModuleArtifact *artifact);
bool build_artifact_needs_rebuild(ModuleArtifact *artifact);

// Build context operations
void build_context_add_artifact(BuildContext *ctx, ModuleArtifact *artifact);
ModuleArtifact *build_context_find_artifact(BuildContext *ctx, const char *module_name);
ModuleArtifact *build_context_find_by_source(BuildContext *ctx, const char *source_path);

// Dependency resolution
// Returns a list of module names in the order they should be compiled
char **build_resolve_dependencies(ModuleContext *mod_ctx, size_t *out_count);
void build_free_dependency_list(char **deps, size_t count);

// Build execution
// Compile a single module (returns artifact)
ModuleArtifact *build_compile_module(BuildContext *build_ctx,
                                      const char *module_name,
                                      const char *source_path);

// Compile a module and all its dependencies (returns main module artifact)
ModuleArtifact *build_compile_with_deps(BuildContext *build_ctx,
                                         const char *source_path);

// Link all compiled modules into an executable
bool build_link_executable(BuildContext *build_ctx,
                           const char *output_name,
                           ModuleArtifact **artifacts,
                           size_t artifact_count);

// Utilities
time_t build_get_file_mtime(const char *path);
bool build_file_exists(const char *path);
char *build_module_name_to_source_path(const char *module_name);
char *build_module_name_to_object_path(const char *module_name, const char *build_dir);

// High-level build function
// Compiles source_path and all dependencies, creates executable
bool build_project(const char *source_path,
                   const char *output_name,
                   bool verbose,
                   bool force_rebuild);

#endif // BUILDSYSTEM_H
