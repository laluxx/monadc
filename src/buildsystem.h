#ifndef BUILDSYSTEM_H
#define BUILDSYSTEM_H

/// buildsystem.h - Build system for Monad
//
//  Section map (mirrors buildsystem.c):
//
//    §1  Includes and constants
//    §2  Module artifacts
//    §3  Build context
//    §4  Persistent build cache (manifest)
//    §5  Dependency discovery
//    §6  Build execution
//    §7  Utilities

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "module.h"


/// §1  Includes and constants

//// Cache manifest
 //
 //  The persistent build cache is a plain-text file written to the build
 //  directory after every successful build. Bumping BUILD_CACHE_VERSION
 //  forces a cold cache on all existing builds (do this whenever the line
 //  format changes incompatibly).
 //
#define BUILD_CACHE_VERSION   1
#define BUILD_CACHE_FILENAME  ".monad_cache"


/// §2  Module artifacts

//// ModuleArtifact
 //
 //  Everything the build system needs to know about one compiled module.
 //  Owns module_name, source_path, and object_path (heap strings).
 //  decl and env are owned by the registry / interpreter respectively.
 //
typedef struct {
    char   *module_name;                        // "Std.Math"
    char   *source_path;                        // "Std/Math.monad"
    char   *object_path;                        // "build/Std.Math.o"
    time_t  source_mtime;                       // mtime of source file
    time_t  object_mtime;                       // mtime of object file
    char    source_hash[MODULE_HASH_HEX_LEN+1]; // hex SHA-256 of source
    bool    source_hash_valid;                  // true once hash computed
    bool    needs_recompile;                    // set by build planner
    ModuleDecl *decl;                           // NULL until discovered
    void       *env;                            // Env* - opaque to build layer
} ModuleArtifact;

ModuleArtifact *build_artifact_create(const char *module_name,
                                      const char *source_path);
void            build_artifact_free(ModuleArtifact *artifact);
void            build_artifact_update_times(ModuleArtifact *artifact);
bool            build_artifact_update_hash(ModuleArtifact *artifact);
bool            build_artifact_needs_rebuild(ModuleArtifact *artifact);


/// §3  Build context

//// BuildContext
 //
 //  Top-level state for one build invocation. Owns artifacts[], build_dir,
 //  cache_path, cache, and registry. Call build_context_free() to release
 //  all of them.
 //
typedef struct {
    ModuleArtifact    **artifacts;        // flat array of all artifacts
    size_t              artifact_count;
    size_t              artifact_capacity;

    ModuleRegistry     *registry;         // module declaration graph
    char               *build_dir;        // output dir for .o files (".")
    char               *cache_path;       // explicit cache path (or NULL)
    bool                verbose;
    bool                force_rebuild;

    struct BuildCache  *cache;            // persistent manifest (or NULL)
} BuildContext;

BuildContext *build_context_create(void);
void          build_context_free(BuildContext *ctx);
void          build_context_set_build_dir(BuildContext *ctx, const char *dir);
void          build_context_set_verbose(BuildContext *ctx, bool verbose);
void          build_context_set_force_rebuild(BuildContext *ctx, bool force);
void          build_context_add_artifact(BuildContext *ctx, ModuleArtifact *artifact);
ModuleArtifact *build_context_find_artifact(BuildContext *ctx,
                                            const char *module_name);
ModuleArtifact *build_context_find_by_source(BuildContext *ctx,
                                             const char *source_path);


/// §4  Persistent build cache (manifest)

//// BuildCache
 //
 //  In-memory image of the cache manifest file.  Entries are
 //  ModuleArtifact* carrying only the fields that matter for freshness
 //  checks (module_name, object_path, source_hash / source_hash_valid).
 //  The entries array is owned by the cache; free with build_cache_free().
 //
typedef struct BuildCache {
    ModuleArtifact **entries;
    size_t           count;
    size_t           capacity;
} BuildCache;

BuildCache     *build_cache_load(const char *build_dir);
void            build_cache_free(BuildCache *cache);
ModuleArtifact *build_cache_find(BuildCache *cache, const char *module_name);
bool            build_cache_is_fresh(BuildCache *cache, ModuleArtifact *artifact);
bool            build_cache_save(BuildContext *ctx);


/// §5  Dependency discovery

ModuleContext *build_extract_module_context(const char *source_path);
char         **build_resolve_dependencies(ModuleContext *mod_ctx,
                                          size_t *out_count);
void           build_free_dependency_list(char **deps, size_t count);


/// §6  Build execution

ModuleArtifact *build_compile_with_deps(BuildContext *build_ctx,
                                        const char *source_path);
bool            build_link_executable(BuildContext *build_ctx,
                                      const char *output_name,
                                      ModuleArtifact **artifacts,
                                      size_t artifact_count);
bool            build_project(const char *source_path,
                              const char *output_name,
                              bool verbose,
                              bool force_rebuild);


/// §7  Utilities

time_t  build_get_file_mtime(const char *path);
bool    build_file_exists(const char *path);
char   *build_module_name_to_source_path(const char *module_name);
char   *build_module_name_to_object_path(const char *module_name,
                                         const char *build_dir);

#endif /* BUILDSYSTEM_H */
