#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_CHAR,
    TYPE_STRING,
    TYPE_SYMBOL,
    TYPE_BOOL,
    TYPE_HEX,
    TYPE_BIN,
    TYPE_OCT,
    TYPE_LIST,
    TYPE_KEYWORD,
    TYPE_RATIO,
    TYPE_ARR,
    TYPE_FN,
    TYPE_LAYOUT,
    TYPE_SET,
    TYPE_MAP,
    TYPE_F32,
    TYPE_I8,
    TYPE_U8,
    TYPE_I16,
    TYPE_U16,
    TYPE_I32,
    TYPE_U32,
    TYPE_I64,
    TYPE_U64,
    TYPE_I128,
    TYPE_U128,
    TYPE_UNKNOWN,
    TYPE_VAR,           // HM type variable    — 'a, 'b, etc.
    TYPE_ARROW,         // function type       — param -> return
    TYPE_VARIADIC,      // List 'a rest params — (. args)
    TYPE_COLL,          // Abstract collection — List | Set | Arr
    TYPE_PTR,           // Pointer :: T        — typed pointer to T
    TYPE_INT_ARBITRARY, // I<n> / U<n>         — arbitrary-width integer, 1–64 or 128
    TYPE_F80,           // F80                 — x87 extended precision
    TYPE_OPTIONAL,      // T?                  — optional type
    TYPE_NIL,           // nil
} TypeKind;


/// A single function parameter descriptor

typedef struct FnParam {
    char *name;        // parameter name (may be NULL for builtins)
    struct Type *type; // parameter type (NULL = polymorphic '_')
    bool optional;     // => (#:optional)
    bool rest;         // variadic rest '. _'
} FnParam;

// One field inside a TYPE_LAYOUT
typedef struct LayoutField {
    char        *name;   // field name
    struct Type *type;   // resolved type of the field
    int          offset; // byte offset within the struct
    int          size;   // byte size of this field
} LayoutField;

/// Type

typedef struct Type {
    TypeKind kind;

    // TYPE_VAR
    int var_id;            // unique type-variable ID

    // TYPE_ARROW  (param -> ret)
    struct Type *arrow_param;
    struct Type *arrow_ret;

    // TYPE_FN
    struct FnParam *params;
    int             param_count;
    struct Type    *return_type;

    // TYPE_LIST
    struct Type *element_type;   // NULL = polymorphic/unknown

    // TYPE_ARR — fat pointer { T* data, i64 size }
    struct Type *arr_element_type;
    int          arr_size;          // -1 = unknown at compile time
    bool         arr_is_fat;        // true = runtime fat pointer

    // TYPE_LAYOUT
    char        *layout_name;
    LayoutField *layout_fields;
    int          layout_field_count;
    int          layout_total_size;
    bool         layout_packed;
    int          layout_align;
    bool         layout_is_scalar; /* true for opaque handle typedefs like VkInstance */
    bool         layout_is_inline; /* true when used as a nested field (not a pointer) */

    // TYPE_INT_ARBITRARY
    int  numeric_width;   // bit-width: 1–64 or 128
    bool numeric_signed;  // true = I (signed), false = U (unsigned)
} Type;

#define list_elem element_type


/// Type Refinement

typedef struct RefinementEntry {
    char *name;
    char *pred_name;
    char *base_type;
    struct AST *predicate_ast;  // owned clone for static evaluation
    char *var;                  // bound variable name e.g. "x"
    struct RefinementEntry *next;
} RefinementEntry;

extern RefinementEntry *g_refinements;


typedef struct TypeAlias {
    char *alias_name;
    char *target_name;
    struct TypeAlias *next;
} TypeAlias;

extern TypeAlias *g_aliases;


/// Constructors — ground types

Type *type_unknown(void);
Type *type_int(void);
Type *type_float(void);
Type *type_char(void);
Type *type_string(void);
Type *type_symbol(void);
Type *type_bool(void);
Type *type_hex(void);
Type *type_bin(void);
Type *type_oct(void);
Type *type_keyword(void);
Type *type_ratio(void);
Type *type_set(void);
Type *type_map(void);
Type *type_coll(void);
Type *type_f32(void);
Type *type_i8(void);
Type *type_u8(void);
Type *type_i16(void);
Type *type_u16(void);
Type *type_i32(void);
Type *type_u32(void);
Type *type_i64(void);
Type *type_u64(void);
Type *type_i128(void);
Type *type_u128(void);
Type *type_f80(void);
Type *type_int_arbitrary(int width, bool is_signed); // I<n> or U<n>


/// Constructors — compound types

Type *type_list(Type *element_type);
Type *type_arr(Type *element_type, int size);
Type *type_arr_fat(Type *element_type); // runtime fat pointer {data, size}
Type *type_fn(FnParam *params, int param_count, Type *return_type);
Type *type_fn_builtin(int min_args, int opt_args, bool variadic);
Type *type_layout(const char *name, LayoutField *fields, int field_count,
                  int total_size, bool packed, int align);
Type *type_layout_ref(const char *name);
Type *type_ptr(Type *pointee);
Type *type_optional(Type *inner);
Type *type_nil(void);


/// Constructors — HM types

Type *type_var(int id);                   // fresh type variable
Type *type_arrow(Type *param, Type *ret); // function/arrow type


Type *type_parse_fn_arrow(const char *fn_type_name);

/// Operations

Type       *type_clone(Type *t);
void        type_free(Type *t);
const char *type_to_string(Type *t);
bool        types_equal(Type *a, Type *b);  // structural equality


/// Utilities

Type *infer_literal_type(double value, const char *literal_str);
Type *type_from_name(const char *name);


/// Refinement Type

void refinement_register(const char *name, const char *pred_name,
                          const char *base_type, void *pred_ast,
                          const char *var);

const char *refinement_pred_name(const char *type_name);
int  refinement_check_literal(const char *type_name, double val,
                               const char **out_pred_src);
void  refinement_free_all(void);
void  type_alias_register(const char *alias_name, const char *target_name);
void  type_alias_free_all(void);


/// Annotation parsing

struct AST;
Type *parse_type_annotation(struct AST *ast);


/// Layout

int layout_compute_offsets(LayoutField *fields, int count, bool packed,
                           int (*elem_size_fn)(const char *type_name));

#endif // TYPES_H
