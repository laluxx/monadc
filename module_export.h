#ifndef MODULE_EXPORT_H
#define MODULE_EXPORT_H

#include <stdbool.h>
#include <stddef.h>

// TypeKind integer constants — must match the TypeKind enum in types.h.
// Duplicated here so module_export.h has no dependency on the rest of the
// compiler. The manifest stores types as plain integers.
#define MEXP_TYPE_INT     0
#define MEXP_TYPE_FLOAT   1
#define MEXP_TYPE_CHAR    2
#define MEXP_TYPE_STRING  3
#define MEXP_TYPE_BOOL    4
#define MEXP_TYPE_HEX     5
#define MEXP_TYPE_BIN     6
#define MEXP_TYPE_OCT     7
#define MEXP_TYPE_LIST    8
#define MEXP_TYPE_RATIO   9
#define MEXP_TYPE_ARR    10
#define MEXP_TYPE_KEYWORD 11
#define MEXP_TYPE_FN     12

// ----------------------------------------------------------------------------
// Module Export Manifest
//
// When module Foo.mon is compiled to Foo.o, we also write Foo.mexp (module
// export manifest).  The manifest is a plain-text file that records every
// exported symbol together with enough type info to emit an `extern`
// declaration in any module that imports Foo.
//
// Format (one record per line):
//   VAR   <mangled_name>  <type_kind>   [<arr_size> <arr_elem_kind>]
//   FUNC  <mangled_name>  <ret_kind>    <param_count>  [<param_name> <param_kind>]*
//
// type_kind is the integer value of TypeKind.
// Mangled name for top-level symbol `phi` in module `Math` is `Math__phi`.
// ----------------------------------------------------------------------------

typedef enum {
    EXPORT_ENTRY_VAR,
    EXPORT_ENTRY_FUNC,
} ExportEntryKind;

typedef struct ExportParamDesc {
    char *name;
    int   type_kind;   // TypeKind integer
} ExportParamDesc;

typedef struct ExportEntry {
    ExportEntryKind kind;
    char *mangled_name;   // e.g. "Math__phi"
    char *local_name;     // e.g. "phi"
    int   type_kind;      // TypeKind integer (for VAR: var type; for FUNC: return type)
    // Array extras
    int   arr_size;
    int   arr_elem_kind;
    // Func extras
    ExportParamDesc *params;
    int              param_count;
} ExportEntry;

typedef struct ExportManifest {
    char *module_name;
    ExportEntry *entries;
    size_t entry_count;
    size_t entry_capacity;
} ExportManifest;

ExportManifest *manifest_create(const char *module_name);
void            manifest_free(ExportManifest *m);
void            manifest_add_var(ExportManifest *m, const char *local_name,
                                 const char *mangled_name, int type_kind,
                                 int arr_size, int arr_elem_kind);
void            manifest_add_func(ExportManifest *m, const char *local_name,
                                  const char *mangled_name, int ret_kind,
                                  ExportParamDesc *params, int param_count);

// Write to "<module_name>.mexp"
bool manifest_write(ExportManifest *m, const char *path);

// Read from "<module_name>.mexp"
ExportManifest *manifest_read(const char *path);

// Mangle: "Math" + "phi" -> "Math__phi"
char *manifest_mangle(const char *module_name, const char *local_name);

#endif // MODULE_EXPORT_H
