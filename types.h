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
    TYPE_UNKNOWN,
    TYPE_VAR,    /* HM type variable — 'a, 'b, etc.          */
    TYPE_ARROW,  /* function type — param → return            */
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
    int var_id;            /* unique type-variable ID */

    // TYPE_ARROW  (param -> ret)
    struct Type *arrow_param;
    struct Type *arrow_ret;

    // TYPE_FN
    struct FnParam *params;
    int             param_count;
    struct Type    *return_type;

    // TYPE_LIST
    struct Type *element_type;   /* NULL = polymorphic/unknown             */

    // TYPE_ARR
    struct Type *arr_element_type;
    int          arr_size;

    // TYPE_LAYOUT
    char        *layout_name;
    LayoutField *layout_fields;
    int          layout_field_count;
    int          layout_total_size;
    bool         layout_packed;
    int          layout_align;
} Type;

#define list_elem element_type

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


/// Constructors — compound types

Type *type_list(Type *element_type);
Type *type_arr(Type *element_type, int size);
Type *type_fn(FnParam *params, int param_count, Type *return_type);
Type *type_fn_builtin(int min_args, int opt_args, bool variadic);
Type *type_layout(const char *name, LayoutField *fields, int field_count,
                  int total_size, bool packed, int align);


/// Constructors — HM types

Type *type_var(int id);               /* fresh type variable                */
Type *type_arrow(Type *param, Type *ret); /* function/arrow type            */


/// Operations

Type       *type_clone(Type *t);
void        type_free(Type *t);
const char *type_to_string(Type *t);
bool        types_equal(Type *a, Type *b);  /* structural equality           */


/// Utilities

Type *infer_literal_type(double value, const char *literal_str);
Type *type_from_name(const char *name);
void  type_alias_register(const char *alias_name, const char *target_name);
void  type_alias_free_all(void);


/// Annotation parsing

struct AST;
Type *parse_type_annotation(struct AST *ast);


/// Layout

int layout_compute_offsets(LayoutField *fields, int count, bool packed,
                           int (*elem_size_fn)(const char *type_name));

#endif // TYPES_H
