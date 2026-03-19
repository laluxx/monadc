#ifndef FFI_H
#define FFI_H

#include <stdbool.h>
#include <stddef.h>
#include "types.h"
#include "codegen.h"
#include "env.h"

/// FFI Function Parameter

typedef struct FFIParam {
    char *name;   // may be NULL for anonymous params
    Type *type;
} FFIParam;

/// FFI Function

typedef struct FFIFunction {
    char      *name;
    FFIParam  *params;
    int        param_count;
    bool       variadic;
    Type      *return_type;
    char      *doc;   // extracted from trailing // or preceding /* */ comment
} FFIFunction;

/// FFI Constant (macro / enum value)

typedef struct FFIConstant {
    char   *name;
    long long value;
    bool   is_float;
    double float_value;
    bool   is_string;
    char  *str_value;
} FFIConstant;

/// FFI Struct Field

typedef struct FFIStructField {
    char *name;
    Type *type;
} FFIStructField;

/// FFI Struct
typedef struct FFIStruct {
    char           *name;
    char           *alias_of;   /* non-NULL if this is a typedef alias */
    FFIStructField *fields;
    int             field_count;
    int             size_bytes;
    bool            packed;
} FFIStruct;

/// FFI Context
//
// Accumulates everything parsed from (include ...) forms.
// Passed to ffi_inject_into_env() to populate the codegen env.
//
typedef struct FFIContext {
    FFIFunction  *functions;
    int           function_count;
    int           function_cap;

    FFIConstant  *constants;
    int           constant_count;
    int           constant_cap;

    FFIStruct    *structs;
    int           struct_count;
    int           struct_cap;

    /* Track which headers have already been included */
    char        **included;
    int           included_count;
    int           included_cap;
} FFIContext;

/// API

FFIContext *ffi_context_create(void);
void        ffi_context_free(FFIContext *ctx);

/* Parse a C header and populate ctx.
 * header_path: resolved path like "/usr/include/stdio.h"
 * system_include: true if written as <stdio.h>, false for "stdio.h"
 * Returns true on success. */
bool ffi_parse_header(FFIContext *ctx, const char *header_path,
                      bool system_include);

/* Inject all parsed symbols into the codegen env + LLVM module.
 * Functions get ENV_FUNC entries with ExternalLinkage.
 * Constants get ENV_VAR entries with constant values.
 * Structs get layout registrations. */
void ffi_inject_into_env(FFIContext *ffi, CodegenContext *cg);

/* Resolve an #include path to an absolute filesystem path.
 * name: the token inside <> or ""
 * system: true for <name>, false for "name"
 * Returns malloc'd path or NULL if not found. */
char *ffi_resolve_header(const char *name, bool system);

/* Map a clang type spelling to a Monad Type*.
 * Returns type_unknown() for unrecognised types. */
Type *ffi_map_c_type(const char *c_type_spelling);

/* Pretty-print the FFI context (for debugging) */
void ffi_dump(FFIContext *ctx);

#endif /* FFI_H */
