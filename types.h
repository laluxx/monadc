#ifndef TYPES_H
#define TYPES_H
#include <stdbool.h>

/// Type System

typedef enum {
    TYPE_UNKNOWN,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_CHAR,
    TYPE_STRING,
    TYPE_HEX,
    TYPE_BIN,
    TYPE_OCT,
    TYPE_BOOL,
    TYPE_FUNCTION,
    TYPE_GENERIC,
} TypeKind;

typedef struct Type {
    TypeKind kind;
    union {
        struct {
            struct Type *param_type;
            struct Type *return_type;
        } func;
        struct {
            char *name;  // For generic type variables like 'a', 'b'
        } generic;
    };
} Type;

/// Type constructors
Type *type_int(void);
Type *type_float(void);
Type *type_char(void);
Type *type_string(void);
Type *type_hex(void);
Type *type_bin(void);
Type *type_oct(void);
Type *type_bool(void);
Type *type_unknown(void);
Type *type_function(Type *param, Type *ret);
Type *type_generic(const char *name);

/// Type operations
bool type_equals(Type *a, Type *b);
void type_free(Type *t);
const char *type_to_string(Type *t);
Type *type_clone(Type *t);

/// Type inference from AST
Type *infer_literal_type(double value, const char *literal_str);
Type *parse_type_annotation(void *ast);  // AST* from reader.h

#endif
