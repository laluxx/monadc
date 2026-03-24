#include "ffi.h"
#include "codegen.h"
#include "env.h"
#include "types.h"
#include <clang-c/Index.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dlfcn.h>

#include <llvm-c/Core.h>

/// Helpers


static char *my_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = malloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/// FFI Context lifecycle

FFIContext *ffi_context_create(void) {
    FFIContext *ctx = calloc(1, sizeof(FFIContext));
    ctx->function_cap  = 64;
    ctx->constant_cap  = 256;
    ctx->struct_cap    = 64;
    ctx->included_cap  = 512;
    ctx->functions  = malloc(sizeof(FFIFunction)  * ctx->function_cap);
    ctx->constants  = malloc(sizeof(FFIConstant)  * ctx->constant_cap);
    ctx->structs    = malloc(sizeof(FFIStruct)     * ctx->struct_cap);
    ctx->included   = malloc(sizeof(char *)        * ctx->included_cap);
    return ctx;
}

void ffi_context_free(FFIContext *ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->function_count; i++) {
        free(ctx->functions[i].name);
        free(ctx->functions[i].doc);
        for (int j = 0; j < ctx->functions[i].param_count; j++) {
            free(ctx->functions[i].params[j].name);
            type_free(ctx->functions[i].params[j].type);
        }
        free(ctx->functions[i].params);
        type_free(ctx->functions[i].return_type);
    }
    free(ctx->functions);
    for (int i = 0; i < ctx->constant_count; i++) {
        free(ctx->constants[i].name);
        free(ctx->constants[i].str_value);
    }
    free(ctx->constants);
    for (int i = 0; i < ctx->struct_count; i++) {
        free(ctx->structs[i].name);
        for (int j = 0; j < ctx->structs[i].field_count; j++) {
            free(ctx->structs[i].fields[j].name);
            type_free(ctx->structs[i].fields[j].type);
        }
        free(ctx->structs[i].fields);
    }
    free(ctx->structs);
    for (int i = 0; i < ctx->included_count; i++)
        free(ctx->included[i]);
    free(ctx->included);
    free(ctx);
}

/// Type mapping  C spelling -> Monad Type*

Type *ffi_map_c_type(const char *s) {
    if (!s) return type_unknown();

    /* void */
    if (strcmp(s, "void") == 0) return NULL; /* NULL signals void return to inject */

    /* bool */
    if (strcmp(s, "_Bool") == 0 || strcmp(s, "bool") == 0) return type_bool();

    /* char — signed char and plain char map to Char,
     * unsigned char is a numeric byte type, maps to U8  */
    if (strcmp(s, "char")        == 0 ||
        strcmp(s, "signed char") == 0) return type_char();
    if (strcmp(s, "unsigned char") == 0) return type_u8();

    /* 8-bit */
    if (strstr(s, "int8_t")  || strcmp(s, "signed char") == 0) return type_i8();
    if (strstr(s, "uint8_t"))                                   return type_u8();

    /* 16-bit */
    if (strstr(s, "int16_t")  || strcmp(s, "short") == 0 ||
        strcmp(s, "signed short") == 0 || strcmp(s, "short int") == 0)
        return type_i16();
    if (strstr(s, "uint16_t") || strcmp(s, "unsigned short") == 0 ||
        strcmp(s, "unsigned short int") == 0)
        return type_u16();

    /* 32-bit */
    if (strstr(s, "int32_t") || strcmp(s, "int") == 0 ||
        strcmp(s, "signed int") == 0 || strcmp(s, "signed") == 0 ||
        strcmp(s, "unsigned int") == 0 || strcmp(s, "unsigned") == 0)
        return type_i32();
    if (strstr(s, "uint32_t") || strcmp(s, "unsigned int") == 0 ||
        strcmp(s, "unsigned") == 0)
        return type_u32();

    /* 64-bit */
    if (strstr(s, "int64_t")  || strcmp(s, "long") == 0 ||
        strcmp(s, "long int") == 0 || strcmp(s, "signed long") == 0 ||
        strcmp(s, "long long") == 0 || strcmp(s, "signed long long") == 0)
        return type_i64();
    if (strstr(s, "uint64_t") || strcmp(s, "unsigned long") == 0 ||
        strcmp(s, "unsigned long long") == 0 || strcmp(s, "size_t") == 0 ||
        strcmp(s, "uintptr_t") == 0 || strcmp(s, "ptrdiff_t") == 0)
        return type_u64();

    /* float / double */
    if (strcmp(s, "float") == 0)  return type_f32();
    if (strcmp(s, "double") == 0) return type_float(); /* Float = f64 */
    if (strcmp(s, "long double") == 0) return type_float();

    /* const char * — treat as String (must come before generic pointer catch) */
    if (strstr(s, "char") && strchr(s, '*')) return type_string();

    /* Pointer to named struct — extract base name and return layout ref */
    if (strchr(s, '*')) {
        /* Extract the base type name before the '*' and any qualifiers */
        char base[256];
        strncpy(base, s, sizeof(base) - 1);
        base[sizeof(base)-1] = '\0';
        /* Strip trailing spaces and '*' */
        char *star = strchr(base, '*');
        if (star) *star = '\0';
        /* Strip trailing spaces */
        char *end = base + strlen(base) - 1;
        while (end > base && (*end == ' ' || *end == '\t')) *end-- = '\0';
        /* Strip leading "const ", "struct ", "unsigned " etc. */
        char *start = base;
        if (strncmp(start, "const ",  6) == 0) start += 6;
        if (strncmp(start, "struct ", 7) == 0) start += 7;
        /* Strip trailing spaces again after prefix removal */
        end = start + strlen(start) - 1;
        while (end > start && (*end == ' ' || *end == '\t')) *end-- = '\0';
        /* Named struct pointer (uppercase) — copy to heap before passing */
        if (start[0] && isupper((unsigned char)start[0])) {
            char *heap_name = strdup(start);
            Type *t = type_layout_ref(heap_name);
            free(heap_name);
            return t;
        }
        /* Primitive pointer — wrap in type_ptr with mapped pointee */
        if (strcmp(start, "float")          == 0) return type_ptr(type_f32());
        if (strcmp(start, "double")         == 0) return type_ptr(type_float());
        if (strcmp(start, "int")            == 0) return type_ptr(type_i32());
        if (strcmp(start, "unsigned int")   == 0) return type_ptr(type_u32());
        if (strcmp(start, "unsigned char")  == 0) return type_ptr(type_u8());
        if (strcmp(start, "unsigned short") == 0) return type_ptr(type_u16());
        if (strcmp(start, "short")          == 0) return type_ptr(type_i16());
        if (strcmp(start, "void")           == 0) return type_ptr(NULL);
        /* Unknown pointer — opaque hex */
        return type_unknown();
    }

    /* Named struct/typedef — return a layout reference by name */
    return type_layout_ref(s);
}

/// Clang visitor state

typedef struct {
    FFIContext *ffi;
    bool        in_system_header;
} VisitorState;

static bool already_included(FFIContext *ctx, const char *name) {
    for (int i = 0; i < ctx->included_count; i++)
        if (strcmp(ctx->included[i], name) == 0) return true;
    return false;
}

static bool already_have_function(FFIContext *ctx, const char *name) {
    for (int i = 0; i < ctx->function_count; i++)
        if (strcmp(ctx->functions[i].name, name) == 0) return true;
    return false;
}

static void ffi_parse_struct_macros(FFIContext *ctx, const char *header_path);

static bool already_have_constant(FFIContext *ctx, const char *name) {
    for (int i = 0; i < ctx->constant_count; i++)
        if (strcmp(ctx->constants[i].name, name) == 0) return true;
    return false;
}

static void ffi_add_function(FFIContext *ctx, FFIFunction fn) {
    if (ctx->function_count >= ctx->function_cap) {
        ctx->function_cap *= 2;
        ctx->functions = realloc(ctx->functions,
                                  sizeof(FFIFunction) * ctx->function_cap);
    }
    ctx->functions[ctx->function_count++] = fn;
}

static void ffi_add_constant(FFIContext *ctx, FFIConstant c) {
    if (ctx->constant_count >= ctx->constant_cap) {
        ctx->constant_cap *= 2;
        ctx->constants = realloc(ctx->constants,
                                  sizeof(FFIConstant) * ctx->constant_cap);
    }
    ctx->constants[ctx->constant_count++] = c;
}

/// Struct visitors

static bool already_have_struct(FFIContext *ctx, const char *name) {
    for (int i = 0; i < ctx->struct_count; i++)
        if (strcmp(ctx->structs[i].name, name) == 0) return true;
    return false;
}

static enum CXChildVisitResult ffi_count_fields(CXCursor cursor,
                                                 CXCursor parent,
                                                 CXClientData data) {
    (void)parent;
    if (clang_getCursorKind(cursor) == CXCursor_FieldDecl)
        (*(int *)data)++;
    return CXChildVisit_Continue;
}

typedef struct { FFIStruct *s; int idx; } FieldVisitState;

static enum CXChildVisitResult field_visitor(CXCursor cursor,
                                              CXCursor parent,
                                              CXClientData client_data) {
    (void)parent;
    FieldVisitState *fs = (FieldVisitState *)client_data;
    if (clang_getCursorKind(cursor) != CXCursor_FieldDecl)
        return CXChildVisit_Continue;
    if (fs->idx >= fs->s->field_count)
        return CXChildVisit_Continue;

    CXString fname   = clang_getCursorSpelling(cursor);
    CXType   ftype   = clang_getCursorType(cursor);
    CXString ftype_s = clang_getTypeSpelling(ftype);

    fs->s->fields[fs->idx].name = my_strdup(clang_getCString(fname));
    fs->s->fields[fs->idx].type = ffi_map_c_type(clang_getCString(ftype_s));
    fs->idx++;

    clang_disposeString(fname);
    clang_disposeString(ftype_s);
    return CXChildVisit_Continue;
}

/// Clang AST visitor

static enum CXChildVisitResult visitor(CXCursor cursor,
                                        CXCursor parent,
                                        CXClientData client_data) {
    (void)parent;
    VisitorState *state = (VisitorState *)client_data;
    FFIContext   *ffi   = state->ffi;

    /* Skip cursors not from the directly included header.
     * We allow everything when system_include=true since the user
     * explicitly asked for that header. */
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    if (clang_Location_isInSystemHeader(loc) && !state->in_system_header)
        return CXChildVisit_Continue;

    /* Track all header files we visit so ffi_parse_struct_macros can
     * scan them all for macros clang's evaluator missed */
    {
        CXFile   file;
        unsigned line, col, offset;
        clang_getExpansionLocation(loc, &file, &line, &col, &offset);
        if (file) {
            CXString fname = clang_getFileName(file);
            const char *fpath = clang_getCString(fname);
            if (fpath && fpath[0] && !already_included(state->ffi, fpath))
                state->ffi->included[state->ffi->included_count++] = my_strdup(fpath);
            clang_disposeString(fname);
        }
    }

    enum CXCursorKind kind = clang_getCursorKind(cursor);

    /* ── Function declarations ─────────────────────────────────────────── */
    if (kind == CXCursor_FunctionDecl) {
        CXString cx_name = clang_getCursorSpelling(cursor);
        const char *name = clang_getCString(cx_name);

        if (name && name[0] && !already_have_function(ffi, name)) {
            CXType fn_type = clang_getCursorType(cursor);
            CXType ret_cx  = clang_getResultType(fn_type);
            CXString ret_s = clang_getTypeSpelling(ret_cx);

            FFIFunction fn = {0};
            fn.name        = my_strdup(name);
            fn.return_type = ffi_map_c_type(clang_getCString(ret_s));
            fn.variadic    = clang_isFunctionTypeVariadic(fn_type);

            int nparams = clang_Cursor_getNumArguments(cursor);
            fn.param_count = nparams;
            fn.params = nparams > 0
                ? malloc(sizeof(FFIParam) * nparams) : NULL;

            for (int i = 0; i < nparams; i++) {
                CXCursor    arg      = clang_Cursor_getArgument(cursor, i);
                CXString    aname_s  = clang_getCursorSpelling(arg);
                CXType      atype_cx = clang_getCursorType(arg);
                CXString    atype_s  = clang_getTypeSpelling(atype_cx);

                const char *aname = clang_getCString(aname_s);
                fn.params[i].name = (aname && aname[0])
                    ? my_strdup(aname) : NULL;
                fn.params[i].type = ffi_map_c_type(clang_getCString(atype_s));

                clang_disposeString(aname_s);
                clang_disposeString(atype_s);
            }

            ffi_add_function(ffi, fn);
            clang_disposeString(ret_s);
        }
        clang_disposeString(cx_name);
        return CXChildVisit_Continue;
    }

    /* ── Enum constants ────────────────────────────────────────────────── */
    if (kind == CXCursor_EnumConstantDecl) {
        CXString cx_name = clang_getCursorSpelling(cursor);
        const char *name = clang_getCString(cx_name);
        if (name && name[0] && !already_have_constant(ffi, name)) {
            FFIConstant c = {0};
            c.name        = my_strdup(name);
            c.value       = clang_getEnumConstantDeclValue(cursor);
            c.is_float    = false;
            ffi_add_constant(ffi, c);
        }
        clang_disposeString(cx_name);
        return CXChildVisit_Continue;
    }

    /* ── Macro definitions ─────────────────────────────────────────────── */
    if (kind == CXCursor_MacroDefinition) {
        CXString cx_name = clang_getCursorSpelling(cursor);
        const char *name = clang_getCString(cx_name);

        /* Only process simple ALL_CAPS or known constant macros */
        if (name && name[0] && !already_have_constant(ffi, name)) {
            /* We can't evaluate arbitrary macros without preprocessing.
             * Use clang_Cursor_Evaluate to get numeric values where possible. */
            CXEvalResult eval = clang_Cursor_Evaluate(cursor);
            if (eval) {
                CXEvalResultKind ek = clang_EvalResult_getKind(eval);
                FFIConstant c = {0};
                c.name = my_strdup(name);
                if (ek == CXEval_Int) {
                    c.value    = clang_EvalResult_getAsLongLong(eval);
                    c.is_float = false;
                    ffi_add_constant(ffi, c);
                } else if (ek == CXEval_Float) {
                    c.float_value = clang_EvalResult_getAsDouble(eval);
                    c.is_float    = true;
                    ffi_add_constant(ffi, c);
                } else {
                    free(c.name); /* string/other — skip for now */
                }
                clang_EvalResult_dispose(eval);
            }
        }
        clang_disposeString(cx_name);
        return CXChildVisit_Continue;
    }

    /* ── Struct / typedef struct ───────────────────────────────────────── */
    if (kind == CXCursor_StructDecl || kind == CXCursor_TypedefDecl) {
        CXString    cx_name = clang_getCursorSpelling(cursor);
        const char *name    = clang_getCString(cx_name);

        /* Handle typedef aliases: typedef Texture Texture2D */
        if (kind == CXCursor_TypedefDecl && name && name[0]) {
            CXType cx_type = clang_getCursorType(cursor);
            CXType canon   = clang_getCanonicalType(cx_type);
            if (canon.kind == CXType_Record) {
                CXString     canon_s    = clang_getTypeSpelling(canon);
                const char  *canon_raw  = clang_getCString(canon_s);
                /* Strip "struct " prefix */
                const char  *canon_name = (strncmp(canon_raw, "struct ", 7) == 0)
                                          ? canon_raw + 7 : canon_raw;
                if (strcmp(name, canon_name) != 0 &&
                    !already_have_struct(ffi, name)) {
                    if (ffi->struct_count >= ffi->struct_cap) {
                        ffi->struct_cap *= 2;
                        ffi->structs = realloc(ffi->structs,
                                               sizeof(FFIStruct) * ffi->struct_cap);
                    }
                    FFIStruct *s  = &ffi->structs[ffi->struct_count++];
                    memset(s, 0, sizeof(FFIStruct));
                    s->name       = my_strdup(name);
                    s->alias_of   = my_strdup(canon_name);
                    s->field_count = 0;
                    s->fields      = NULL;
                }
                clang_disposeString(canon_s);
                clang_disposeString(cx_name);
                return CXChildVisit_Continue;
            }
        }

        if (name && name[0] && !already_have_struct(ffi, name)) {
            int n_fields = 0;
            clang_visitChildren(cursor, ffi_count_fields, &n_fields);

            if (n_fields > 0) {
                /* Get size BEFORE any realloc that visitChildren may trigger */
                CXType    cx_type = clang_getCursorType(cursor);
                CXType    canon   = clang_getCanonicalType(cx_type);
                long long sz      = clang_Type_getSizeOf(canon);

                /* Build a temporary struct on the stack — do NOT take a
                 * pointer into ffi->structs until after field_visitor runs,
                 * because clang_visitChildren may recurse into nested struct
                 * declarations which call ffi_add_struct → realloc,
                 * invalidating any pointer we hold into the array.        */
                FFIStruct tmp;
                memset(&tmp, 0, sizeof(tmp));
                tmp.name        = my_strdup(name);
                tmp.field_count = n_fields;
                tmp.fields      = calloc(n_fields, sizeof(FFIStructField));
                tmp.packed      = false;
                tmp.size_bytes  = (sz > 0) ? (int)sz : 0;

                FieldVisitState fs = { &tmp, 0 };
                clang_visitChildren(cursor, field_visitor, &fs);

                /* Now safe to grow the array and copy in */
                if (ffi->struct_count >= ffi->struct_cap) {
                    ffi->struct_cap *= 2;
                    ffi->structs = realloc(ffi->structs,
                                           sizeof(FFIStruct) * ffi->struct_cap);
                }
                ffi->structs[ffi->struct_count++] = tmp;
            }
        }
        clang_disposeString(cx_name);
        return CXChildVisit_Recurse;
    }

    return CXChildVisit_Continue;
}

/// Header resolution

char *ffi_resolve_header(const char *name, bool system) {
    if (!system) {
        /* Local header — check cwd first */
        if (file_exists(name)) return my_strdup(name);
    }

    /* Standard system include paths */
    static const char *sys_paths[] = {
        "/usr/include",
        "/usr/local/include",
        "/usr/lib/clang/include",
        "/usr/include/x86_64-linux-gnu",
        NULL
    };

    for (int i = 0; sys_paths[i]; i++) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s/%s", sys_paths[i], name);
        if (file_exists(buf)) return my_strdup(buf);
    }

    /* Let clang find it via its own search path by returning the name */
    return my_strdup(name);
}

/// Docstring extraction

/* Extract a docstring for `fname` from the header source text.
 * Handles three patterns:
 *   1. Trailing //  comment on the same line as the declaration
 *   2. Trailing / * * / comment on the same line
 *   3. Preceding block comment / * ... * / immediately above the declaration
 * Returns a malloc'd string or NULL. */
static char *extract_doc_for_function(const char *src, const char *fname) {
    if (!src || !fname) return NULL;

    /* Build a pattern that matches the function declaration line:
     * look for "fname(" not preceded by -> or * (excludes comments/calls) */
    const char *p = src;
    while (*p) {
        /* Find next occurrence of fname( */
        const char *found = strstr(p, fname);
        if (!found) break;

        /* Must be followed by '(' possibly with spaces and newlines */
        const char *after = found + strlen(fname);
        while (*after == ' ' || *after == '\t' || *after == '\n' ||
               *after == '\r' || *after == '\\') after++;
        if (*after != '(') { p = found + 1; continue; }

        if (found > src) {
            char immediately_before = *(found - 1);
            if (immediately_before == '_' ||
                isalnum((unsigned char)immediately_before)) {
                p = found + 1; continue;
            }
            if (found >= src + 2 &&
                *(found - 1) == ' ' && *(found - 2) == '>') {
                p = found + 1; continue;
            }
        }

        /* Reject if the line is a comment line — skip to next occurrence */
        {
            const char *ls = found;
            while (ls > src && *(ls-1) != '\n') ls--;
            const char *lp = ls;
            while (*lp == ' ' || *lp == '\t') lp++;
            if (lp[0] == '/' || lp[0] == '*' ||
                (lp[0] == '*') ||
                strstr(ls, "//") < found) {
                /* Check if any // appears before fname on this line */
                const char *sl2 = ls;
                bool in_comment = false;
                while (sl2 < found) {
                    if (sl2[0] == '/' && sl2[1] == '/') { in_comment = true; break; }
                    if (sl2[0] == '/' && sl2[1] == '*') { in_comment = true; break; }
                    if (sl2[0] == '*' && sl2 == lp)     { in_comment = true; break; }
                    sl2++;
                }
                if (in_comment) { p = found + 1; continue; }
            }
        }

        /* Found a real declaration — find the end of this line */
        const char *line_start = found;
        while (line_start > src && *(line_start-1) != '\n') line_start--;
        const char *line_end = found;
        while (*line_end && *line_end != '\n') line_end++;

        // Pattern 1 & 2: trailing // or /* comment on same line */
        const char *comment = NULL;
        /* Search for // after the declaration */
        const char *sl = strstr(found, "//");
        if (sl && sl < line_end) comment = sl + 2;
        if (!comment) {
            // Search for /* ... */ on same line */
            const char *ml = strstr(found, "/*");
            if (ml && ml < line_end) {
                ml += 2;
                while (*ml == ' ' || *ml == '*') ml++;
                comment = ml;
            }
        }
        if (comment) {
            while (*comment == ' ' || *comment == '\t') comment++;
            // Trim trailing whitespace and */ and newline */
            const char *cend = comment;
            while (*cend && *cend != '\n') cend++;
            // Strip trailing */ */
            while (cend > comment && (*(cend-1) == ' ' || *(cend-1) == '\t'
                                      || *(cend-1) == '/' || *(cend-1) == '*'))
                cend--;
            if (cend > comment) return strndup(comment, cend - comment);
        }

        // Pattern 3: preceding block comment /* ... */
        // Walk backward from line_start to find a closing */
        {
            /* Step 1: from line_start go backward skipping whitespace/newlines */
            const char *back = line_start;
            if (back > src) back--;  /* step over the \n before this line */
            while (back > src && (*back == '\n' || *back == '\r' ||
                                   *back == ' '  || *back == '\t'))
                back--;
            /* back now points at the last non-whitespace char before our line.
             * For a block comment it should be '/' with '*' before it: ...* / */
            if (back > src && *back == '/' && *(back-1) == '*') {
                const char *close = back - 1; /* points at '*' of closing * / */
                /* Step 2: find the opening / * walking backward */
                const char *open = close - 1;
                while (open > src) {
                    if (*open == '*' && open > src && *(open-1) == '/') break;
                    open--;
                }
                if (open > src && *(open-1) == '/') {
                    const char *content = open + 1; /* skip the '*' of opening */
                    // skip whitespace/newline immediately after opening /*
                    while (*content == ' ' || *content == '\t' ||
                           *content == '\n' || *content == '\r') content++;
                    /* skip leading * on first line if present */
                    if (*content == '*' && *(content+1) != '/') content++;
                    while (*content == ' ' || *content == '\t') content++;
                    /* Step 3: collect text, stripping leading ' * ' per line */
                    char buf[2048] = {0};
                    int  bi = 0;
                    const char *cp = content;
                    while (cp < close && bi < (int)sizeof(buf) - 2) {
                        /* Skip leading whitespace, '*', newlines at line start */
                        if (*cp == '\n' || *cp == '\r') {
                            cp++;
                            /* skip leading whitespace and * on next line */
                            while (*cp == ' ' || *cp == '\t') cp++;
                            if (*cp == '*' && *(cp+1) != '/') cp++;
                            while (*cp == ' ' || *cp == '\t') cp++;
                            /* empty line = keep as \n\n, continuation = space */
                            if (*cp == '\n' || *cp == '\r') {
                                buf[bi++] = '\n';
                                buf[bi++] = '\n';
                            } else {
                                if (bi > 0 && buf[bi-1] != ' ' &&
                                    buf[bi-1] != '\n')
                                    buf[bi++] = ' ';
                            }
                            continue;
                        }
                        buf[bi++] = *cp++;
                    }
                    /* Trim trailing whitespace */
                    while (bi > 0 && (buf[bi-1] == ' ' || buf[bi-1] == '\t'
                                      || buf[bi-1] == '\n'))
                        bi--;
                    buf[bi] = '\0';
                    if (bi > 0) return strdup(buf);
                }
            }
        }

        /* Found the declaration but no comment — stop */
        fprintf(stderr, "  [%s] found decl but no comment on line: %.*s\n",
                fname, (int)(line_end - line_start), line_start);
        return NULL;
    }
    return NULL;
}

/// Main parse entry point

bool ffi_parse_header(FFIContext *ctx, const char *header_path,
                      bool system_include) {
    /* Avoid double-parsing */
    if (already_included(ctx, header_path)) return true;

    /* Record as included — use a larger initial cap since sub-headers
     * will be added during the visitor pass */
    if (ctx->included_count >= ctx->included_cap) {
        ctx->included_cap *= 2;
        ctx->included = realloc(ctx->included,
                                sizeof(char *) * ctx->included_cap);
    }

    char *resolved_path = ffi_resolve_header(header_path, system_include);
    ctx->included[ctx->included_count++] = resolved_path;

    /* Try cache first */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (ffi_cache_load(ctx, resolved_path)) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                    (t1.tv_nsec - t0.tv_nsec) / 1e6;
        printf("FFI: cache loaded in %.1f ms\n", ms);
        return true;
    }

    /* Build a small translation unit that just #includes the header */
    char tu_src[512];
    if (system_include)
        snprintf(tu_src, sizeof(tu_src), "#include <%s>\n", header_path);
    else
        snprintf(tu_src, sizeof(tu_src), "#include \"%s\"\n", header_path);

    /* clang args — enable macros, system includes */
    /* Find clang resource dir for builtins like stddef.h */
    static char resource_arg[256] = {0};
    static bool resource_found = false;
    if (!resource_found) {
        const char *candidates[] = {
            "/usr/lib/clang/21/include",
            "/usr/lib/clang/20/include",
            "/usr/lib/clang/19/include",
            "/usr/lib/clang/18/include",
            "/usr/lib/clang/17/include",
            "/usr/lib64/clang/21/include",
            "/usr/lib64/clang/20/include",
            NULL
        };
        for (int ci = 0; candidates[ci]; ci++) {
            if (file_exists(candidates[ci])) {
                snprintf(resource_arg, sizeof(resource_arg),
                         "-I%s", candidates[ci]);
                resource_found = true;
                break;
            }
        }
        if (!resource_found) {
            /* Try to find via clang --print-resource-dir at runtime */
            FILE *pp = popen("clang --print-resource-dir 2>/dev/null", "r");
            if (pp) {
                char rdir[256] = {0};
                if (fgets(rdir, sizeof(rdir), pp)) {
                    size_t rlen = strlen(rdir);
                    while (rlen > 0 && (rdir[rlen-1] == '\n' ||
                                        rdir[rlen-1] == '\r')) rdir[--rlen] = '\0';
                    snprintf(resource_arg, sizeof(resource_arg),
                             "-I%s/include", rdir);
                    resource_found = true;
                }
                pclose(pp);
            }
        }
    }

    const char *clang_args[8];
    int n_args = 0;
    clang_args[n_args++] = "-x";
    clang_args[n_args++] = "c";
    clang_args[n_args++] = "-std=c11";
    clang_args[n_args++] = "-D_GNU_SOURCE";
    if (resource_found && resource_arg[0])
        clang_args[n_args++] = resource_arg;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    CXIndex index = clang_createIndex(0, 0);

    /* Parse from an in-memory buffer */
    struct CXUnsavedFile unsaved = {
        .Filename = "__ffi_include__.c",
        .Contents = tu_src,
        .Length   = strlen(tu_src)
    };

    CXTranslationUnit tu = clang_parseTranslationUnit(
        index,
        "__ffi_include__.c",
        clang_args, n_args,
        &unsaved, 1,
        CXTranslationUnit_DetailedPreprocessingRecord |
        CXTranslationUnit_SkipFunctionBodies
    );

    if (!tu) {
        fprintf(stderr, "ffi: failed to parse header '%s'\n", header_path);
        clang_disposeIndex(index);
        return false;
    }

    /* Report errors */
    unsigned n_diag = clang_getNumDiagnostics(tu);
    for (unsigned i = 0; i < n_diag; i++) {
        CXDiagnostic diag = clang_getDiagnostic(tu, i);
        enum CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(diag);
        if (sev >= CXDiagnostic_Error) {
            CXString msg = clang_getDiagnosticSpelling(diag);
            fprintf(stderr, "ffi: %s\n", clang_getCString(msg));
            clang_disposeString(msg);
        }
        clang_disposeDiagnostic(diag);
    }

    /* Visit the AST */
    VisitorState state = { .ffi = ctx, .in_system_header = true };
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visitor, &state);

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);

    /* Post-parse: read source text once and extract docstrings for all
     * functions that were added during this parse. */
    {
        const char *resolved = ctx->included[ctx->included_count - 1];
        FILE *sf = fopen(resolved, "r");
        if (sf) {
            fseek(sf, 0, SEEK_END);
            long fsz = ftell(sf);
            rewind(sf);
            char *src = malloc(fsz + 1);
            if (src) {
                fread(src, 1, fsz, sf);
                src[fsz] = '\0';
                for (int i = 0; i < ctx->function_count; i++) {
                    if (!ctx->functions[i].doc)
                        ctx->functions[i].doc =
                            extract_doc_for_function(src, ctx->functions[i].name);
                }
                free(src);
            }
            fclose(sf);
        }
    }

    /* extract struct-literal macros and numeric macros
     * that clang's evaluator couldn't handle.
     * Scan ALL resolved headers, not just the top-level one — constants
     * like SDL_INIT_VIDEO are defined in sub-headers (SDL_init.h etc.) */
    for (int i = 0; i < ctx->included_count; i++)
        ffi_parse_struct_macros(ctx, ctx->included[i]);

    /* Save to cache for next time */
    ffi_cache_save(ctx, resolved_path);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("FFI: full parse in %.1f ms (cache saved)\n", ms);
    return true;
}

/// Post-parse struct-literal macro extractor
//
// Handles patterns like:
//   #define BLACK  CLITERAL(Color){ 0, 0, 0, 255 }
//   #define BLACK  (Color){ 0, 0, 0, 255 }
//   #define PI     3.14159265f
//   #define WIDTH  800
//
// Called after ffi_parse_header to fill in constants the clang evaluator
// missed (struct literals, expressions referencing other macros).
//
static void ffi_parse_struct_macros(FFIContext *ctx, const char *header_path) {
    fprintf(stderr, "ffi_parse_struct_macros: header_path='%s'\n", header_path);
    fflush(stderr);
    FILE *f = fopen(header_path, "r");
    if (!f) {
        fprintf(stderr, "ffi_parse_struct_macros: failed to open file\n");
        return;
    }
    fprintf(stderr, "ffi_parse_struct_macros: file opened ok\n");
    fflush(stderr);

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = malloc(sz + 1);
    if (!src) { fclose(f); return; }
    fread(src, 1, sz, f);
    src[sz] = '\0';
    fclose(f);

    const char *p = src;
    while (*p) {
        /* Find next # character */
        while (*p && *p != '#') p++;
        if (!*p) break;
        p++; /* skip # */
        /* Skip whitespace between # and directive name */
        while (*p == ' ' || *p == '\t') p++;
        /* Only process #define */
        if (strncmp(p, "define", 6) != 0) {
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }
        p += 6;
        /* Must have whitespace after 'define' */
        if (*p != ' ' && *p != '\t') {
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }

        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;

        /* Read macro name */
        const char *name_start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        if (p == name_start) { continue; }
        size_t name_len = p - name_start;
        char name[256];
        if (name_len >= sizeof(name)) { continue; }
        memcpy(name, name_start, name_len);
        name[name_len] = '\0';

        if (strstr(name, "SDL_INIT"))
            fprintf(stderr, "ffi_parse_struct_macros: saw '%s' p[0]='%c' already=%d\n",
                    name, *p, already_have_constant(ctx, name));

        /* Skip function-like macros */
        if (*p == '(') {            /* But only if the '(' immediately follows the name with no space
             * i.e. #define FOO(x) — function-like macro definition.
             * We already consumed the name and whitespace so if we're at '('
             * it must be a parameter list. Skip it. */
            while (*p && *p != '\n') p++;
            continue;
        }

        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;

        /* Read value — may span multiple lines with backslash continuation */
        char value[512] = {0};
        int vi = 0;
        while (*p && *p != '\n' && vi < (int)sizeof(value) - 1) {
            if (*p == '\\' && *(p+1) == '\n') { p += 2; continue; }
            if (*p == '/' && *(p+1) == '/') break;  /* line comment */
            if (*p == '/' && *(p+1) == '*') break;  /* block comment */
            value[vi++] = *p++;
        }
        value[vi] = '\0';

        /* Trim trailing whitespace */
        while (vi > 0 && (value[vi-1] == ' ' || value[vi-1] == '\t'))
            value[--vi] = '\0';

        if (!value[0]) continue;
        if (already_have_constant(ctx, name)) continue;
        fprintf(stderr, "ffi_parse_struct_macros: name='%s' value='%s'\n", name, value);

        /* ── Pattern 1: struct literal macro ──────────────────────────────
         * CLITERAL(TypeName){ a, b, c, d }
         * (TypeName){ a, b, c, d }
         * TypeName{ a, b, c, d }                                          */
        {
            const char *v = value;
            /* Skip CLITERAL(...) wrapper */
            if (strncmp(v, "CLITERAL", 8) == 0) {
                v += 8;
                while (*v == ' ') v++;
                if (*v == '(') {
                    v++;
                    while (*v && *v != ')') v++;
                    if (*v == ')') v++;
                }
            }
            /* Skip optional cast (TypeName) */
            if (*v == '(') {
                v++;
                while (*v && *v != ')') v++;
                if (*v == ')') v++;
            }
            while (*v == ' ') v++;

            if (*v == '{') {
                /* Parse comma-separated integers */
                v++;
                long long fields[16];
                int nf = 0;
                while (*v && *v != '}' && nf < 16) {
                    while (*v == ' ' || *v == '\t' || *v == ',') v++;
                    if (*v == '}') break;
                    char *end;
                    long long val = strtoll(v, &end, 0);
                    if (end != v) {
                        fields[nf++] = val;
                        v = end;
                    } else {
                        /* Not a plain integer — skip this macro */
                        nf = -1;
                        break;
                    }
                }
                if (nf > 0) {
                    /* Store each sub-field as NAME.0, NAME.1, ... etc.
                     * Then add a sentinel (value=-1) so PASS 5 in
                     * ffi_inject_into_env can pack them into an integer. */
                    for (int fi = 0; fi < nf; fi++) {
                        char subname[280];
                        snprintf(subname, sizeof(subname), "%s.%d", name, fi);
                        if (!already_have_constant(ctx, subname)) {
                            FFIConstant sc = {0};
                            sc.name  = my_strdup(subname);
                            sc.value = fields[fi];
                            ffi_add_constant(ctx, sc);
                        }
                    }
                    FFIConstant sentinel = {0};
                    sentinel.name              = my_strdup(name);
                    sentinel.value             = -1;
                    sentinel.is_float          = false;
                    sentinel.is_struct_literal = true;
                    ffi_add_constant(ctx, sentinel);
                    continue;
                }
            }
        }

        /* ── Pattern 2: plain numeric literal ─────────────────────────── */
        {
            const char *v = value;
            while (*v == ' ') v++;
            /* Strip common integer cast wrappers:
             * SDL_UINT64_C(x), UINT64_C(x), INT64_C(x), (uint64_t)(x) etc.
             * These are used to define 64-bit constants portably. */
            static const char *cast_wrappers[] = {
                "SDL_UINT64_C", "SDL_INT64_C",
                "UINT64_C", "INT64_C", "UINT32_C", "INT32_C",
                "UINT16_C", "INT16_C", "UINT8_C",  "INT8_C",
                "(uint64_t)", "(int64_t)", "(uint32_t)", "(int32_t)",
                "(Uint64)", "(Sint64)", "(Uint32)", "(Sint32)",
                NULL
            };
            for (int wi = 0; cast_wrappers[wi]; wi++) {
                size_t wlen = strlen(cast_wrappers[wi]);
                if (strncmp(v, cast_wrappers[wi], wlen) == 0) {
                    v += wlen;
                    while (*v == ' ') v++;
                    /* Strip surrounding parens if present */
                    if (*v == '(') {
                        v++;
                        /* find matching ) */
                        const char *end = v;
                        while (*end && *end != ')') end++;
                        /* copy inner value to a temp buffer */
                        static char inner[64];
                        size_t ilen = end - v;
                        if (ilen < sizeof(inner)) {
                            memcpy(inner, v, ilen);
                            inner[ilen] = '\0';
                            v = inner;
                        }
                    }
                    break;
                }
            }
            while (*v == '(' || *v == ' ') v++;
            /* Strip trailing f/F/l/L float suffixes into a temp buffer */
            char numval[256];
            strncpy(numval, v, sizeof(numval) - 1);
            numval[sizeof(numval)-1] = '\0';
            int nlen = strlen(numval);
            while (nlen > 0 && (numval[nlen-1] == 'f' || numval[nlen-1] == 'F' ||
                                 numval[nlen-1] == 'l' || numval[nlen-1] == 'L' ||
                                 numval[nlen-1] == 'u' || numval[nlen-1] == 'U')) {
                numval[--nlen] = '\0';
            }
            char *end;
            /* Try float first */
            double fval = strtod(numval, &end);
            if (end != numval) {
                /* Check if it's actually a float (has . or e or f suffix) */
                bool is_float = false;
                for (const char *cp = v; *cp && cp < v + strlen(v); cp++) {
                    if (*cp == '.' || *cp == 'e' || *cp == 'E' || *cp == 'f' || *cp == 'F') {
                        is_float = true; break;
                    }
                }
                FFIConstant c = {0};
                c.name = my_strdup(name);
                if (is_float) {
                    c.is_float    = true;
                    c.float_value = fval;
                } else {
                    c.is_float = false;
                    c.value    = (long long)strtoll(numval, NULL, 0);
                }
                ffi_add_constant(ctx, c);
                continue;
            }
        }

        /* ── Pattern 4: bitwise OR of already-known constants ─────────── */
        {
            const char *v = value;
            while (*v == '(') v++;

            long long result = 0;
            bool ok = false;
            bool any = false;
            const char *cur = v;

            while (*cur) {
                while (*cur == ' ' || *cur == '\t') cur++;
                if (!*cur || *cur == ')') break;

                const char *tok_start = cur;
                while (*cur && *cur != ' ' && *cur != '\t' &&
                       *cur != '|' && *cur != ')') cur++;
                size_t tok_len = cur - tok_start;
                if (tok_len == 0) { any = false; break; }

                char tok[256] = {0};
                if (tok_len >= sizeof(tok)) { any = false; break; }
                memcpy(tok, tok_start, tok_len);

                bool found = false;
                for (int j = 0; j < ctx->constant_count; j++) {
                    if (strcmp(ctx->constants[j].name, tok) == 0 &&
                        !ctx->constants[j].is_float &&
                        !ctx->constants[j].is_string) {
                        result |= ctx->constants[j].value;
                        found = true;
                        break;
                    }
                }
                if (!found) { any = false; break; }
                any = true;

                while (*cur == ' ' || *cur == '\t') cur++;
                if (*cur == '|') cur++;
            }
            ok = any;

            if (ok) {
                FFIConstant c = {0};
                c.name  = my_strdup(name);
                c.value = result;
                ffi_add_constant(ctx, c);
                continue;
            }
        }

        /* ── Pattern 3: string literal ───────────────────────────────── */
        {
            const char *v = value;
            while (*v == ' ') v++;
            if (*v == '"') {
                v++; /* skip opening quote */
                char strval[256] = {0};
                int si = 0;
                while (*v && *v != '"' && si < (int)sizeof(strval) - 1) {
                    if (*v == '\\' && *(v+1)) {
                        v++;
                        switch (*v) {
                        case 'n': strval[si++] = '\n'; break;
                        case 't': strval[si++] = '\t'; break;
                        default:  strval[si++] = *v;   break;
                        }
                        v++;
                    } else {
                        strval[si++] = *v++;
                    }
                }
                strval[si] = '\0';
                /* Store as a string constant with value=0 and
                 * a special negative float_value to signal string type.
                 * We store the string pointer in float_value via a union
                 * trick — but FFIConstant doesn't support strings.
                 * Simplest: store a hash of the string as the int value
                 * and separately keep a string table. For now, skip
                 * string constants that aren't version numbers. */
                FFIConstant c = {0};
                c.name      = my_strdup(name);
                c.is_string = true;
                c.str_value = my_strdup(strval);
                ffi_add_constant(ctx, c);
                continue;
            }
        }
    }

    free(src);
}

/// Inject into codegen env

/* Strip any matching prefix from name, return pointer into name or name itself */
static const char *strip_prefix(FFIContext *ffi, const char *name) {
    for (int i = 0; i < ffi->strip_prefix_count; i++) {
        const char *pfx = ffi->strip_prefixes[i];
        size_t plen = strlen(pfx);
        if (strncmp(name, pfx, plen) == 0 && name[plen] != '\0')
            return name + plen;
    }
    return NULL; /* no match */
}

void ffi_inject_into_env(FFIContext *ffi, CodegenContext *cg) {
    struct timespec _t0, _t1;
    clock_gettime(CLOCK_MONOTONIC, &_t0);
    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);

    static const char *protected[] = {
        "printf", "fprintf", "sprintf", "snprintf",
        "malloc", "calloc", "realloc", "free",
        "memset", "memcpy", "memmove", "strlen",
        "strcmp", "strncmp", "strcpy", "strdup",
        "abort", "exit", NULL
    };

    /* ── PASS 1: Structs → layouts ──────────────────────────────────────── */
    for (int i = 0; i < ffi->struct_count; i++) {
        FFIStruct *s = &ffi->structs[i];
        if (!s->name) continue;
        if (env_lookup_layout(cg->env, s->name)) continue;

        if (s->alias_of) {
            /* Wait for aliases until base types are registered */
            continue;
        }
        if (s->field_count == 0) continue;

        LayoutField *fields = malloc(sizeof(LayoutField) * s->field_count);
        for (int j = 0; j < s->field_count; j++) {
            fields[j].name   = strdup(s->fields[j].name ? s->fields[j].name : "_");
            fields[j].type   = s->fields[j].type ? type_clone(s->fields[j].type) : type_int();
            fields[j].size   = s->size_bytes > 0 ? s->size_bytes / s->field_count : 8;
            fields[j].offset = 0;
        }
        layout_compute_offsets(fields, s->field_count, false, NULL);
        Type *layout_type = type_layout(s->name, fields, s->field_count,
                                        s->size_bytes, false, 0);
        env_insert_layout(cg->env, s->name, layout_type, NULL);
        type_to_llvm(cg, layout_type);
        printf("FFI: layout %s (%d fields, %d bytes)\n",
               s->name, s->field_count, s->size_bytes);
    }

    /* ── PASS 2: Typedef aliases ─────────────────────────────────────────── */
    for (int i = 0; i < ffi->struct_count; i++) {
        FFIStruct *s = &ffi->structs[i];
        if (!s->name || !s->alias_of) continue;
        if (env_lookup_layout(cg->env, s->name)) continue;
        Type *target = env_lookup_layout(cg->env, s->alias_of);
        if (!target) continue;
        Type *alias_t = type_layout(s->name,
                                    target->layout_fields,
                                    target->layout_field_count,
                                    target->layout_total_size,
                                    target->layout_packed,
                                    target->layout_align);
        env_insert_layout(cg->env, s->name, alias_t, NULL);
        type_to_llvm(cg, alias_t);
        printf("FFI: layout alias %s -> %s\n", s->name, s->alias_of);
    }

    /* ── PASS 3: Functions ───────────────────────────────────────────────── */
    /* fprintf(stderr, "ffi PASS3: %d functions, env=%p\n", */
    /*         ffi->function_count, (void*)cg->env); */
    fflush(stderr);
    for (int i = 0; i < ffi->function_count; i++) {
        FFIFunction *f = &ffi->functions[i];
        /* fprintf(stderr, "ffi PASS3 fn[%d]: name=%p '%s'\n", */
        /*         i, (void*)f->name, f->name ? f->name : "NULL"); */
        /* for (int j = 0; j < f->param_count; j++) { */
        /*     Type *pt2 = f->params[j].type; */
        /*     fprintf(stderr, "  param[%d]: type=%p kind=%d layout_name=%p\n", */
        /*             j, (void*)pt2, */
        /*             pt2 ? pt2->kind : -1, */
        /*             pt2 ? (void*)pt2->layout_name : NULL); */
        /* } */
        /* fflush(stderr); */
        if (env_lookup(cg->env, f->name)) continue;

        bool skip = false;
        for (int p = 0; protected[p]; p++)
            if (strcmp(f->name, protected[p]) == 0) { skip = true; break; }
        if (skip) continue;

        LLVMTypeRef *pt = f->param_count > 0
            ? malloc(sizeof(LLVMTypeRef) * f->param_count) : NULL;
        EnvParam    *ep = f->param_count > 0
            ? malloc(sizeof(EnvParam)    * f->param_count) : NULL;

        for (int j = 0; j < f->param_count; j++) {
            ep[j].name = f->params[j].name ? strdup(f->params[j].name) : strdup("_");
            ep[j].type = type_clone(f->params[j].type);

            Type *ptype = f->params[j].type;
            if (!ptype) {
                pt[j] = ptr_t;
            } else if (ptype->kind == TYPE_I32 || ptype->kind == TYPE_U32) {
                pt[j] = LLVMInt32TypeInContext(cg->context);
            } else if (ptype->kind == TYPE_LAYOUT) {
                if (!ptype->layout_name ||
                    (uintptr_t)ptype->layout_name < 0x1000) {
                    fprintf(stderr, "ffi: corrupt layout_name ptr %p for param %d of %s\n",
                            (void*)ptype->layout_name, j, f->name);
                    pt[j] = ptr_t;
                    ep[j].type = type_clone(ptype);
                    continue;
                }
                Type *full = env_lookup_layout(cg->env, ptype->layout_name);
                int sz = full ? full->layout_total_size : 0;
                if      (sz > 0 && sz <= 4)  pt[j] = LLVMInt32TypeInContext(cg->context);
                else if (sz > 4 && sz <= 8)  pt[j] = LLVMInt64TypeInContext(cg->context);
                else if (sz > 8 && sz <= 16) {
                    LLVMTypeRef i64 = LLVMInt64TypeInContext(cg->context);
                    LLVMTypeRef f2[] = {i64, i64};
                    pt[j] = LLVMStructTypeInContext(cg->context, f2, 2, 0);
                } else {
                    pt[j] = ptr_t;
                }
            } else {
                pt[j] = type_to_llvm(cg, ptype);
            }
        }

        LLVMTypeRef ret_llvm = f->return_type
            ? type_to_llvm(cg, f->return_type)
            : LLVMVoidTypeInContext(cg->context);

        LLVMTypeRef fn_type = LLVMFunctionType(ret_llvm, pt,
                                               f->param_count, f->variadic ? 1 : 0);
        LLVMValueRef fn_ref = LLVMGetNamedFunction(cg->module, f->name);
        if (!fn_ref) {
            fn_ref = LLVMAddFunction(cg->module, f->name, fn_type);
            LLVMSetLinkage(fn_ref, LLVMExternalLinkage);
        }

        // TODO HERE
        /* fprintf(stderr, "FFI doc [%s]: %s\n", f->name, */
        /*         f->doc ? f->doc : "(null)"); */
        env_insert_func(cg->env, f->name, ep, f->param_count,
                        f->return_type ? type_clone(f->return_type) : NULL,
                        fn_ref, f->doc ? f->doc : NULL);
        const char *f_short = strip_prefix(ffi, f->name);
        if (f_short && !env_lookup(cg->env, f_short))
            env_insert_func(cg->env, f_short, ep, f->param_count,
                            f->return_type ? type_clone(f->return_type) : NULL,
                            fn_ref, f->doc ? f->doc : NULL);

        EnvEntry *e = env_lookup(cg->env, f->name);
        if (e) {
            e->is_closure_abi = false;
            e->lifted_count = 0; e->is_ffi = true;
            /* Store the header path so the REPL can jump to it */
            if (ffi->included_count > 0 && !e->header_path)
                e->header_path = strdup(ffi->included[0]);
        }
        if (pt) free(pt);
    }

    /* ── PASS 4: Numeric/float/string constants ──────────────────────────── */
    for (int i = 0; i < ffi->constant_count; i++) {
        FFIConstant *c = &ffi->constants[i];
        if (env_lookup(cg->env, c->name)) continue;
        /* Skip sub-field constants (RED.r etc.) and sentinels */
        if (strchr(c->name, '.')) continue;
        if (!c->is_float && !c->is_string && c->is_struct_literal) continue;

        if (c->is_string) {
            LLVMTypeRef  i8    = LLVMInt8TypeInContext(cg->context);
            LLVMTypeRef  sptr  = LLVMPointerType(i8, 0);
            size_t       slen  = strlen(c->str_value);
            LLVMTypeRef  arr_t = LLVMArrayType(i8, slen + 1);
            char         aname[280];
            snprintf(aname, sizeof(aname), ".str.%s", c->name);
            LLVMValueRef arr = LLVMAddGlobal(cg->module, arr_t, aname);
            LLVMSetInitializer(arr, LLVMConstStringInContext(cg->context,
                                    c->str_value, (unsigned)slen, 0));
            LLVMSetLinkage(arr, LLVMPrivateLinkage);
            LLVMSetUnnamedAddr(arr, LLVMGlobalUnnamedAddr);
            LLVMSetGlobalConstant(arr, 1);
            LLVMValueRef gv = LLVMAddGlobal(cg->module, sptr, c->name);
            LLVMSetInitializer(gv, LLVMConstBitCast(arr, sptr));
            LLVMSetLinkage(gv, LLVMInternalLinkage);
            LLVMSetGlobalConstant(gv, 1);
            env_insert(cg->env, c->name, type_string(), gv);
            { const char *s = strip_prefix(ffi, c->name);
              if (s && !env_lookup(cg->env, s)) env_insert(cg->env, s, type_string(), gv); }
        } else if (c->is_float) {
            LLVMTypeRef  lt = LLVMDoubleTypeInContext(cg->context);
            LLVMValueRef lv = LLVMConstReal(lt, c->float_value);
            LLVMValueRef gv = LLVMAddGlobal(cg->module, lt, c->name);
            LLVMSetInitializer(gv, lv);
            LLVMSetGlobalConstant(gv, 1);
            LLVMSetLinkage(gv, LLVMInternalLinkage);
            env_insert(cg->env, c->name, type_float(), gv);
            { const char *s = strip_prefix(ffi, c->name);
              if (s && !env_lookup(cg->env, s)) env_insert(cg->env, s, type_float(), gv); }
        } else {
            LLVMTypeRef  lt = LLVMInt64TypeInContext(cg->context);
            LLVMValueRef lv = LLVMConstInt(lt, (unsigned long long)c->value, 0);
            LLVMValueRef gv = LLVMAddGlobal(cg->module, lt, c->name);
            LLVMSetInitializer(gv, lv);
            LLVMSetGlobalConstant(gv, 1);
            LLVMSetLinkage(gv, LLVMInternalLinkage);
            env_insert(cg->env, c->name, type_int(), gv);
            { const char *s = strip_prefix(ffi, c->name);
              if (s && !env_lookup(cg->env, s)) env_insert(cg->env, s, type_int(), gv); }
        }
    }

    /* ── PASS 5: Struct literal constants (any layout, any size) ────────── */
    for (int i = 0; i < ffi->constant_count; i++) {
        FFIConstant *c = &ffi->constants[i];
        if (strchr(c->name, '.'))  continue;
        if (c->is_float || c->is_string) continue;
        if (env_lookup(cg->env, c->name)) continue;
        /* A sentinel value of -1 signals a struct literal constant.
         * Collect all sub-field constants named "<NAME>.<field_index>" */
        if (!c->is_struct_literal) continue;

        /* Find how many sub-fields exist by scanning for <NAME>.0, <NAME>.1...
         * Sub-fields are stored as "<NAME>.<fieldname>" — collect them in order */
        long long fields[64];
        int nf = 0;
        /* Walk all constants looking for ones starting with "<NAME>." */
        /* We need them in insertion order — they were inserted r,g,b,a order */
        for (int j = 0; j < ffi->constant_count && nf < 64; j++) {
            const char *dot = strchr(ffi->constants[j].name, '.');
            if (!dot) continue;
            size_t prefix_len = dot - ffi->constants[j].name;
            if (strlen(c->name) != prefix_len) continue;
            if (strncmp(ffi->constants[j].name, c->name, prefix_len) != 0) continue;
            fields[nf++] = ffi->constants[j].value;
        }
        if (nf == 0) continue;

        /* Determine the integer type wide enough to hold the packed struct.
         * Each sub-field is 1 byte (u8) for the common case; derive from count. */
        int total_bytes = nf; /* assume 1 byte per field — holds for Color etc. */
        LLVMTypeRef int_t;
        if      (total_bytes <= 1) int_t = LLVMInt8TypeInContext(cg->context);
        else if (total_bytes <= 2) int_t = LLVMInt16TypeInContext(cg->context);
        else if (total_bytes <= 4) int_t = LLVMInt32TypeInContext(cg->context);
        else                       int_t = LLVMInt64TypeInContext(cg->context);

        /* Pack fields little-endian */
        uint64_t packed = 0;
        for (int fi = 0; fi < nf; fi++)
            packed |= ((uint64_t)(unsigned char)fields[fi]) << (fi * 8);

        LLVMValueRef gv = LLVMAddGlobal(cg->module, int_t, c->name);
        LLVMSetInitializer(gv, LLVMConstInt(int_t, packed, 0));
        LLVMSetLinkage(gv, LLVMInternalLinkage);
        LLVMSetGlobalConstant(gv, 1);
        /* Use the matching integer type so loads work correctly */
        Type *int_type = (total_bytes <= 1) ? type_u8()  :
                         (total_bytes <= 2) ? type_u16() :
                         (total_bytes <= 4) ? type_i32() : type_int();
        env_insert(cg->env, c->name, int_type, gv);
        { const char *s = strip_prefix(ffi, c->name);
          if (s && !env_lookup(cg->env, s)) env_insert(cg->env, s, int_type, gv); }
        printf("FFI: struct const %s = 0x%llx (%d bytes)\n",
               c->name, (unsigned long long)packed, total_bytes);
    }

    clock_gettime(CLOCK_MONOTONIC, &_t1);
    double _ms = (_t1.tv_sec - _t0.tv_sec) * 1000.0 +
                 (_t1.tv_nsec - _t0.tv_nsec) / 1e6;
    printf("FFI: injected %d functions, %d constants, %d structs in %.1f ms\n",
           ffi->function_count, ffi->constant_count, ffi->struct_count, _ms);

    /* ── dlopen the library for JIT symbol resolution ───────────────────── */
    for (int i = 0; i < ffi->included_count; i++) {
        const char *hdr  = ffi->included[i];
        const char *base = strrchr(hdr, '/');
        base = base ? base + 1 : hdr;
        char stem[256];
        strncpy(stem, base, sizeof(stem) - 1);
        stem[sizeof(stem)-1] = '\0';
        char *dot = strrchr(stem, '.');
        if (dot) *dot = '\0';
        const char *sufx[] = {".so",".so.5",".so.4",".so.3",".so.2",".so.1",NULL};
        for (int s = 0; sufx[s]; s++) {
            char soname[512];
            snprintf(soname, sizeof(soname), "lib%s%s", stem, sufx[s]);
            void *h = dlopen(soname, RTLD_NOW | RTLD_GLOBAL);
            if (h) { printf("FFI: loaded %s for JIT symbol resolution\n", soname); break; }
        }
    }
}

/// Debug dump

void ffi_dump(FFIContext *ctx) {
    printf("=== FFI Context ===\n");
    printf("Functions (%d):\n", ctx->function_count);
    for (int i = 0; i < ctx->function_count; i++) {
        FFIFunction *f = &ctx->functions[i];
        printf("  %s :: ", f->name);
        for (int j = 0; j < f->param_count; j++) {
            printf("%s -> ", type_to_string(f->params[j].type));
        }
        printf("%s%s\n", type_to_string(f->return_type),
               f->variadic ? " (variadic)" : "");
    }
    printf("Constants (%d):\n", ctx->constant_count);
    for (int i = 0; i < ctx->constant_count; i++) {
        FFIConstant *c = &ctx->constants[i];
        if (c->is_float)
            printf("  %s = %g\n", c->name, c->float_value);
        else
            printf("  %s = %lld\n", c->name, c->value);
    }
}

/// FFI cache
//
// Cache is stored in ~/.cache/monad/<escaped_header_path>.ffic
// Format: magic + mtime + serialized functions/constants/structs.
// If the header mtime matches, we skip the clang parse entirely.
//
#define FFI_CACHE_MAGIC 0x464649C0  /* "FFI\xC0" */
#define FFI_CACHE_VERSION 1

static char *ffi_cache_path(const char *header_path) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    /* escape the header path: replace / with _ */
    char escaped[512] = {0};
    const char *p = header_path;
    int ei = 0;
    while (*p && ei < (int)sizeof(escaped) - 6) {
        escaped[ei++] = (*p == '/') ? '_' : *p;
        p++;
    }
    escaped[ei] = '\0';
    char *result = malloc(512);
    snprintf(result, 512, "%s/.cache/monad/%s.ffic", home, escaped);
    return result;
}

static time_t ffi_header_mtime(const char *header_path) {
    struct stat st;
    if (stat(header_path, &st) != 0) return 0;
    return st.st_mtime;
}

static void ffi_cache_mkdir(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/.cache", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/.cache/monad", home);
    mkdir(dir, 0755);
}

static void write_str(FILE *f, const char *s) {
    if (!s) { uint32_t z = 0; fwrite(&z, 4, 1, f); return; }
    uint32_t len = strlen(s);
    fwrite(&len, 4, 1, f);
    fwrite(s, 1, len, f);
}

static char *read_str(FILE *f) {
    uint32_t len;
    if (fread(&len, 4, 1, f) != 1) return NULL;
    if (len == 0) return NULL;
    if (len > 65536) return NULL; /* sanity */
    char *s = malloc(len + 1);
    if (fread(s, 1, len, f) != len) { free(s); return NULL; }
    s[len] = '\0';
    return s;
}

static void write_type(FILE *f, Type *t) {
    if (!t) { int32_t k = -1; fwrite(&k, 4, 1, f); return; }
    int32_t kind = t->kind;
    fwrite(&kind, 4, 1, f);
    write_str(f, t->layout_name);
}

static Type *read_type(FILE *f) {
    int32_t kind;
    if (fread(&kind, 4, 1, f) != 1) return NULL;
    if (kind == -1) return NULL;
    char *lname = read_str(f);
    /* Reconstruct basic types from kind */
    Type *t = NULL;
    switch (kind) {
    case TYPE_INT:    t = type_int();    break;
    case TYPE_FLOAT:  t = type_float();  break;
    case TYPE_BOOL:   t = type_bool();   break;
    case TYPE_CHAR:   t = type_char();   break;
    case TYPE_STRING: t = type_string(); break;
    case TYPE_I8:     t = type_i8();     break;
    case TYPE_U8:     t = type_u8();     break;
    case TYPE_I16:    t = type_i16();    break;
    case TYPE_U16:    t = type_u16();    break;
    case TYPE_I32:    t = type_i32();    break;
    case TYPE_U32:    t = type_u32();    break;
    case TYPE_I64:    t = type_i64();    break;
    case TYPE_U64:    t = type_u64();    break;
    case TYPE_F32:    t = type_f32();    break;
    case TYPE_LAYOUT: t = lname ? type_layout_ref(lname) : type_unknown(); break;
    case TYPE_PTR:    t = type_ptr(NULL); break;
    default:          t = type_unknown(); break;
    }
    free(lname);
    return t;
}

bool ffi_cache_save(FFIContext *ctx, const char *header_path) {
    ffi_cache_mkdir();
    char *path = ffi_cache_path(header_path);
    FILE *f = fopen(path, "wb");
    free(path);
    if (!f) return false;

    /* Header */
    uint32_t magic = FFI_CACHE_MAGIC;
    uint32_t ver   = FFI_CACHE_VERSION;
    int64_t  mtime = (int64_t)ffi_header_mtime(header_path);
    fwrite(&magic, 4, 1, f);
    fwrite(&ver,   4, 1, f);
    fwrite(&mtime, 8, 1, f);

    /* Functions */
    uint32_t nfn = ctx->function_count;
    fwrite(&nfn, 4, 1, f);
    for (int i = 0; i < ctx->function_count; i++) {
        FFIFunction *fn = &ctx->functions[i];
        write_str(f, fn->name);
        write_str(f, fn->doc);
        write_type(f, fn->return_type);
        uint8_t var = fn->variadic ? 1 : 0;
        fwrite(&var, 1, 1, f);
        uint32_t np = fn->param_count;
        fwrite(&np, 4, 1, f);
        for (int j = 0; j < fn->param_count; j++) {
            write_str(f, fn->params[j].name);
            write_type(f, fn->params[j].type);
        }
    }

    /* Constants */
    uint32_t nc = ctx->constant_count;
    fwrite(&nc, 4, 1, f);
    for (int i = 0; i < ctx->constant_count; i++) {
        FFIConstant *c = &ctx->constants[i];
        write_str(f, c->name);
        write_str(f, c->str_value);
        fwrite(&c->value,       8, 1, f);
        fwrite(&c->float_value, 8, 1, f);
        uint8_t flags = (c->is_float ? 1 : 0)
                      | (c->is_string ? 2 : 0)
                      | (c->is_struct_literal ? 4 : 0);
        fwrite(&flags, 1, 1, f);
    }

    /* Structs */
    uint32_t ns = ctx->struct_count;
    fwrite(&ns, 4, 1, f);
    for (int i = 0; i < ctx->struct_count; i++) {
        FFIStruct *s = &ctx->structs[i];
        write_str(f, s->name);
        write_str(f, s->alias_of);
        uint32_t nf = s->field_count;
        int32_t  sb = s->size_bytes;
        uint8_t  pk = s->packed ? 1 : 0;
        fwrite(&nf, 4, 1, f);
        fwrite(&sb, 4, 1, f);
        fwrite(&pk, 1, 1, f);
        for (int j = 0; j < s->field_count; j++) {
            write_str(f, s->fields[j].name);
            write_type(f, s->fields[j].type);
        }
    }

    /* Included paths */
    uint32_t ni = ctx->included_count;
    fwrite(&ni, 4, 1, f);
    for (int i = 0; i < ctx->included_count; i++)
        write_str(f, ctx->included[i]);

    fclose(f);
    printf("FFI: cache saved (%d fns, %d consts, %d structs)\n",
           ctx->function_count, ctx->constant_count, ctx->struct_count);
    return true;
}

bool ffi_cache_load(FFIContext *ctx, const char *header_path) {
    char *path = ffi_cache_path(header_path);
    FILE *f = fopen(path, "rb");
    free(path);
    if (!f) return false;

    uint32_t magic, ver;
    int64_t  cached_mtime;
    if (fread(&magic, 4, 1, f) != 1 || magic != FFI_CACHE_MAGIC) { fclose(f); return false; }
    if (fread(&ver,   4, 1, f) != 1 || ver   != FFI_CACHE_VERSION) { fclose(f); return false; }
    if (fread(&cached_mtime, 8, 1, f) != 1) { fclose(f); return false; }

    /* Check mtime of the top-level header */
    int64_t current_mtime = (int64_t)ffi_header_mtime(header_path);
    if (current_mtime != cached_mtime) {
        fclose(f); return false; /* stale */
    }

    /* Functions */
    uint32_t nfn;
    if (fread(&nfn, 4, 1, f) != 1) { fclose(f); return false; }
    for (uint32_t i = 0; i < nfn; i++) {
        FFIFunction fn = {0};
        fn.name       = read_str(f);
        fn.doc        = read_str(f);
        fn.return_type = read_type(f);
        uint8_t var;
        fread(&var, 1, 1, f);
        fn.variadic = var;
        uint32_t np;
        fread(&np, 4, 1, f);
        fn.param_count = np;
        fn.params = np > 0 ? malloc(sizeof(FFIParam) * np) : NULL;
        for (uint32_t j = 0; j < np; j++) {
            fn.params[j].name = read_str(f);
            fn.params[j].type = read_type(f);
        }
        ffi_add_function(ctx, fn);
    }

    /* Constants */
    uint32_t nc;
    if (fread(&nc, 4, 1, f) != 1) { fclose(f); return false; }
    for (uint32_t i = 0; i < nc; i++) {
        FFIConstant c = {0};
        c.name      = read_str(f);
        c.str_value = read_str(f);
        fread(&c.value,       8, 1, f);
        fread(&c.float_value, 8, 1, f);
        uint8_t flags;
        fread(&flags, 1, 1, f);
        c.is_float          = (flags & 1) ? true : false;
        c.is_string         = (flags & 2) ? true : false;
        c.is_struct_literal = (flags & 4) ? true : false;
        ffi_add_constant(ctx, c);
    }

    /* Structs */
    uint32_t ns;
    if (fread(&ns, 4, 1, f) != 1) { fclose(f); return false; }
    for (uint32_t i = 0; i < ns; i++) {
        FFIStruct s = {0};
        s.name     = read_str(f);
        s.alias_of = read_str(f);
        uint32_t nf; int32_t sb; uint8_t pk;
        fread(&nf, 4, 1, f);
        fread(&sb, 4, 1, f);
        fread(&pk, 1, 1, f);
        s.field_count = nf;
        s.size_bytes  = sb;
        s.packed      = pk;
        s.fields = nf > 0 ? calloc(nf, sizeof(FFIStructField)) : NULL;
        for (uint32_t j = 0; j < nf; j++) {
            s.fields[j].name = read_str(f);
            s.fields[j].type = read_type(f);
        }
        if (ctx->struct_count >= ctx->struct_cap) {
            ctx->struct_cap *= 2;
            ctx->structs = realloc(ctx->structs, sizeof(FFIStruct) * ctx->struct_cap);
        }
        ctx->structs[ctx->struct_count++] = s;
    }

    /* Included paths */
    uint32_t ni;
    if (fread(&ni, 4, 1, f) != 1) { fclose(f); return false; }
    for (uint32_t i = 0; i < ni; i++) {
        char *inc = read_str(f);
        if (inc) {
            if (ctx->included_count >= ctx->included_cap) {
                ctx->included_cap *= 2;
                ctx->included = realloc(ctx->included, sizeof(char*) * ctx->included_cap);
            }
            ctx->included[ctx->included_count++] = inc;
        }
    }

    fclose(f);
    printf("FFI: cache loaded (%d fns, %d consts, %d structs)\n",
           ctx->function_count, ctx->constant_count, ctx->struct_count);
    return true;
}
