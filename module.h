/// module.h — Module system for Monad
//
//  Implements module declarations, import declarations, symbol resolution,
//  and the workspace-wide module registry/index used by the build system
//  and the language server.
//
//  Source layout:
//
//    §1   Includes and constants
//    §2   Export lists and module declarations
//    §3   Re-export declarations
//    §4   Import declarations
//    §5   Module context (per-file compilation state)
//    §6   Module registry (global, content-addressed)
//    §7   Dependency graph
//    §8   Symbol resolution
//    §9   Module file path resolution
//    §10  Parsing module/import/export forms
//

#ifndef MODULE_H
#define MODULE_H

#include <stdbool.h>
#include <stdint.h>
#include "reader.h"

/// §1 Includes and constants

//// Module System
 //
 //  Rules:
 //    1. Module names MUST start with an uppercase letter.
 //    2. File names must match their module name
 //       (e.g. Math.monad for module Math, Std/Math.monad for Std.Math).
 //    3. The file exporting Main may be named anything.
 //
 //  Syntax:
 //
 //    (module Main)             ; export everything
 //    (module Main [])          ; export nothing
 //    (module Main [f1 f2])     ; export only f1 and f2
 //
 //    (export Std.Math [sqrt])  ; re-export sqrt from Std.Math under
 //                               ; this module's namespace
 //
 //    (import qualified Std.Math :as Math)  ; qualified, aliased
 //    (import qualified Math)               ; qualified, prefix "Math"
 //    (import Std.Math :as Math)            ; unqualified, aliased
 //    (import Std.Math [sqrt log])          ; selective
 //    (import Std.Math hiding [sqrt log])   ; hiding
 //
#define MODULE_INDEX_BUCKETS 256
#define MODULE_HASH_HEX_LEN  16   /* 64-bit FNV-1a, hex-encoded */

/// §2 Export lists and module declarations

//// Export modes
 //
 //  EXPORT_ALL      — every top-level binding is visible to importers.
 //  EXPORT_NONE     — nothing is visible (module is import-only / internal).
 //  EXPORT_SELECTED — only names in ModuleDecl.exports are visible.
 //
typedef enum {
    EXPORT_ALL,
    EXPORT_NONE,
    EXPORT_SELECTED,
} ExportMode;

//// Export list
 //
 //  A growable array of symbol names.  Used both for the module's own
 //  selective exports and for re-export selections.
 //
typedef struct ExportList {
    char **symbols;
    size_t count;
    size_t capacity;
} ExportList;

void export_list_init(ExportList *list);
void export_list_add(ExportList *list, const char *symbol);
bool export_list_contains(const ExportList *list, const char *symbol);
void export_list_free(ExportList *list);

//// Module declaration
 //
 //  Describes the `(module Name ...)` form at the top of a source file,
 //  plus any `(export ...)` re-export forms that follow it.
 //
typedef struct ModuleDecl {
    char *name;              // Module name (must start uppercase)
    ExportMode mode;
    ExportList exports;      // Used when mode == EXPORT_SELECTED

    struct ReExportDecl **reexports;
    size_t reexport_count;
    size_t reexport_capacity;
} ModuleDecl;

ModuleDecl *module_decl_create(const char *name, ExportMode mode);
void module_decl_free(ModuleDecl *decl);
void module_decl_add_export(ModuleDecl *decl, const char *symbol);
void module_decl_add_reexport(ModuleDecl *decl, struct ReExportDecl *re);
bool module_decl_is_exported(ModuleDecl *decl, const char *symbol);
bool module_name_is_valid(const char *name);

/// §3 Re-export declarations

//// Re-export declaration
 //
 //  (export Std.Math [sqrt log])   — re-export selected symbols from
 //                                    Std.Math under this module's name.
 //  (export Std.Math)              — re-export everything visible from
 //                                    Std.Math.
 //
 //  Re-exports let a module act as a facade over one or more other
 //  modules without requiring importers to know the underlying layout.
 //
typedef struct ReExportDecl {
    char *module_name;     // Source module being re-exported
    ExportList symbols;    // Selected symbols (empty => re-export all)
    bool reexport_all;     // True if no selection list was given
} ReExportDecl;

ReExportDecl *reexport_decl_create(const char *module_name, bool reexport_all);
void reexport_decl_add_symbol(ReExportDecl *re, const char *symbol);
void reexport_decl_free(ReExportDecl *re);

/// §4 Import declarations

//// Import modes
 //
 //  IMPORT_QUALIFIED   — must use prefix (Math.sqrt); prefix not added to
 //                       the unqualified namespace.
 //  IMPORT_UNQUALIFIED — both `sqrt` and `Math.sqrt` resolve.
 //  IMPORT_SELECTIVE   — only listed symbols are imported (unqualified).
 //  IMPORT_HIDING      — all except listed symbols are imported
 //                       (unqualified).
 //
typedef enum {
    IMPORT_QUALIFIED,
    IMPORT_UNQUALIFIED,
    IMPORT_SELECTIVE,
    IMPORT_HIDING,
} ImportMode;

//// Import declaration
 //
 //  Describes a single `(import ...)` form.  `alias`, when present, is the
 //  prefix used for qualified access; otherwise the prefix defaults to the
 //  last dot-component of `module_name`.
 //
typedef struct ImportDecl {
    char *module_name;     // Full module name (e.g. "Std.Math")
    char *alias;           // Alias for qualified access, or NULL
    ImportMode mode;
    bool qualified;        // True if declared with `qualified`
    char **symbols;        // For IMPORT_SELECTIVE / IMPORT_HIDING
    size_t symbol_count;
} ImportDecl;

ImportDecl *import_decl_create(const char *module_name, const char *alias,
                                ImportMode mode);
void import_decl_free(ImportDecl *decl);
void import_decl_add_symbol(ImportDecl *decl, const char *symbol);
bool import_decl_includes_symbol(ImportDecl *decl, const char *symbol);
const char *import_decl_prefix(const ImportDecl *decl);

/// §5 Module context (per-file compilation state)

//// Module context
 //
 //  Tracks the module declaration and import list for the file currently
 //  being compiled or analyzed.
 //
typedef struct ModuleContext {
    ModuleDecl *decl;
    ImportDecl **imports;
    size_t import_count;
    size_t import_capacity;
    char *current_file;
} ModuleContext;

ModuleContext *module_context_create(void);
void module_context_free(ModuleContext *ctx);
void module_context_set_file(ModuleContext *ctx, const char *filename);
void module_context_set_decl(ModuleContext *ctx, ModuleDecl *decl);
void module_context_add_import(ModuleContext *ctx, ImportDecl *import);
void module_context_add_prelude_imports(ModuleContext *ctx);
ImportDecl *module_context_find_import(ModuleContext *ctx, const char *prefix);

/// §6 Module registry (global, content-addressed)

//// Module registry entry
 //
 //  One entry per compiled/known module.  In addition to the module
 //  declaration, the registry tracks a content hash of the source file
 //  so that the build cache can detect changes independent of mtimes
 //  (useful for VCS checkouts, CI, and reproducible builds).
 //
typedef struct ModuleRegistryEntry {
    ModuleDecl *decl;
    char *source_path;
    char content_hash[MODULE_HASH_HEX_LEN + 1];  // FNV-1a 64, hex
    char **dep_names;       // Direct dependency module names
    size_t dep_count;
    size_t dep_capacity;
    struct ModuleRegistryEntry *next;  // Hash bucket chain
} ModuleRegistryEntry;

//// Module registry
 //
 //  A hash table (FNV-1a) keyed by module name, with a parallel sorted
 //  array for deterministic iteration (used by dependency resolution).
 //
typedef struct ModuleRegistry {
    ModuleRegistryEntry *buckets[MODULE_INDEX_BUCKETS];
    ModuleRegistryEntry **sorted;
    size_t sorted_count;
    size_t sorted_capacity;
    bool sorted_dirty;
} ModuleRegistry;

ModuleRegistry *module_registry_create(void);
void module_registry_free(ModuleRegistry *registry);

ModuleRegistryEntry *module_registry_entry_create(const char *name);
void module_registry_entry_free(ModuleRegistryEntry *entry);
void module_registry_entry_add_dep(ModuleRegistryEntry *entry, const char *dep_name);

void module_registry_add(ModuleRegistry *registry, ModuleRegistryEntry *entry);
ModuleRegistryEntry *module_registry_find(ModuleRegistry *registry, const char *name);
ModuleDecl *module_registry_find_decl(ModuleRegistry *registry, const char *name);
void module_registry_remove(ModuleRegistry *registry, const char *name);

// FNV-1a 64-bit hash of a buffer, hex-encoded into `out` (>= 17 bytes).
void module_hash_buffer(const char *data, size_t len, char *out);

/// §7 Dependency graph

//// Dependency graph
 //
 //  Built from a ModuleRegistry by following each entry's dep_names.
 //  Supports topological ordering (for build order) with cycle
 //  detection, reported via `cycle_module` when present.
 //
typedef struct {
    char **order;          // Topologically sorted module names
    size_t order_count;
    char *cycle_module;     // Non-NULL if a cycle was detected
} ModuleDepOrder;

// Resolve the build order for `root_module` and its transitive
// dependencies, using `registry` for dependency lookups.
ModuleDepOrder *module_dep_order_compute(ModuleRegistry *registry,
                                          const char *root_module);
void module_dep_order_free(ModuleDepOrder *order);

// Extract the list of imported module names from a ModuleContext, in
// declaration order. Caller owns the returned array (free with
// build_free_dependency_list-style loop, or module_dep_list_free).
char **module_dep_list_from_context(ModuleContext *ctx, size_t *out_count);
void module_dep_list_free(char **deps, size_t count);

/// §8 Symbol resolution

// Resolve `symbol` (qualified or unqualified) to its fully-qualified form
// given the imports in `ctx`. Returns NULL if the symbol is qualified
// with an unknown prefix or excluded by an import filter. Returns a copy
// of `symbol` for local/unqualified bindings with no matching import.
char *module_resolve_symbol(ModuleContext *ctx, const char *symbol);

// Resolve `symbol` against a module's exports, following re-exports
// transitively. Returns true if `symbol` is visible when importing
// `decl` from `registry`.
bool module_resolve_export(ModuleRegistry *registry, ModuleDecl *decl,
                            const char *symbol);

/// §9 Module file path resolution

// Convert "Std.Math" to "Std/Math.mon" (or ".monad" fallback), searching:
//   1. relative to the current directory
//   2. $MONAD_CORE (if set)
//   3. /usr/local/lib/monad/core
// Returns a heap-allocated path; caller must free().
char *module_name_to_path(const char *module_name);

/// §10 Parsing module/import/export forms

ModuleDecl *parse_module_decl(AST *ast);
ImportDecl *parse_import_decl(AST *ast);
ReExportDecl *parse_export_decl(AST *ast);

#endif // MODULE_H
