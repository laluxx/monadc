#ifndef MODULE_H
#define MODULE_H

#include <stdbool.h>
#include "reader.h"

/// Module System
//
// Rules:
// 1. Module names MUST be capitalized
// 2. File names must match their module name (E.g. Math.monad for module Math)
// 3. The file exporting Main can be named anything
//
// Syntax:
//   (module Main)           ; Export everything
//   (module Main [])        ; Export nothing
//   (module Main [f1 f2])   ; Export only f1 and f2
//
//   (import qualified Std.Math :as Math)  ; Qualified - must use Math.sqrt
//   (import Std.Math :as Math)            ; Can use sqrt or Math:sqrt
//   (import Std.Math [sqrt log])          ; Import only sqrt and log
//   (import Std.Math hiding [sqrt log])   ; Import everything except sqrt and log
//

typedef enum {
    EXPORT_ALL,       // Export everything
    EXPORT_NONE,      // Export nothing
    EXPORT_SELECTED,  // Export only specified symbols
} ExportMode;

typedef struct ExportList {
    char **symbols;
    size_t count;
    size_t capacity;
} ExportList;

typedef struct ModuleDecl {
    char *name;            // Module name (must be capitalized)
    ExportMode mode;
    ExportList exports;    // Only used when mode == EXPORT_SELECTED
} ModuleDecl;

typedef enum {
    IMPORT_QUALIFIED,      // Must use prefix (Math:sqrt)
    IMPORT_UNQUALIFIED,    // Can use with or without prefix (sqrt or Math:sqrt)
    IMPORT_SELECTIVE,      // Import only specified symbols
    IMPORT_HIDING,         // Import everything except specified symbols
} ImportMode;

typedef struct ImportDecl {
    char *module_name;     // Full module name (e.g., "Std.Math")
    char *alias;           // Alias for the module (e.g., "Math")
    ImportMode mode;
    char **symbols;        // For IMPORT_SELECTIVE and IMPORT_HIDING
    size_t symbol_count;
} ImportDecl;

/// Module context - tracks the current module being compiled
typedef struct ModuleContext {
    ModuleDecl *decl;            // Current module declaration
    ImportDecl **imports;        // List of imports
    size_t import_count;
    size_t import_capacity;
    char *current_file;          // Current file being compiled
} ModuleContext;

/// Module registry - tracks all compiled modules
typedef struct ModuleRegistry {
    ModuleDecl **modules;
    size_t count;
    size_t capacity;
} ModuleRegistry;

// Module context management
ModuleContext *module_context_create(void);
void module_context_free(ModuleContext *ctx);
void module_context_set_file(ModuleContext *ctx, const char *filename);

// Module declaration
ModuleDecl *module_decl_create(const char *name, ExportMode mode);
void module_decl_free(ModuleDecl *decl);
void module_decl_add_export(ModuleDecl *decl, const char *symbol);
bool module_decl_is_exported(ModuleDecl *decl, const char *symbol);
bool module_name_is_valid(const char *name);

// Import declaration
ImportDecl *import_decl_create(const char *module_name, const char *alias, ImportMode mode);
void import_decl_free(ImportDecl *decl);
void import_decl_add_symbol(ImportDecl *decl, const char *symbol);
bool import_decl_includes_symbol(ImportDecl *decl, const char *symbol);

// Module context operations
void module_context_set_decl(ModuleContext *ctx, ModuleDecl *decl);
void module_context_add_import(ModuleContext *ctx, ImportDecl *import);
ImportDecl *module_context_find_import(ModuleContext *ctx, const char *prefix);

// Symbol resolution
// Returns the fully qualified name for a symbol (e.g., "Math:sqrt")
// Returns NULL if the symbol is not accessible in the current context
char *module_resolve_symbol(ModuleContext *ctx, const char *symbol);

// Module registry
ModuleRegistry *module_registry_create(void);
void module_registry_free(ModuleRegistry *registry);
void module_registry_add(ModuleRegistry *registry, ModuleDecl *decl);
ModuleDecl *module_registry_find(ModuleRegistry *registry, const char *name);

// Module file path resolution
// Given a module name like "Std.Math", returns the file path "Std/Math.monad"
char *module_name_to_path(const char *module_name);

// Parse module and import declarations from AST
ModuleDecl *parse_module_decl(AST *ast);
ImportDecl *parse_import_decl(AST *ast);

#endif // MODULE_H
