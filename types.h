#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_CHAR,
    TYPE_STRING,
    TYPE_BOOL,
    TYPE_HEX,
    TYPE_BIN,
    TYPE_OCT,
    TYPE_FN,
    TYPE_UNKNOWN,
} TypeKind;

/// A single function parameter descriptor
typedef struct FnParam {
    char *name;        // parameter name (may be NULL for builtins)
    struct Type *type; // parameter type (NULL = polymorphic '_')
    bool optional;     // #:optional
    bool rest;         // variadic rest '. _'
} FnParam;

typedef struct Type {
    TypeKind kind;

    // TYPE_FN fields
    struct FnParam *params;   // array of parameters
    int             param_count;
    struct Type    *return_type;  // NULL = unknown/polymorphic
} Type;

/// Constructors
Type *type_int(void);
Type *type_float(void);
Type *type_char(void);
Type *type_string(void);
Type *type_bool(void);
Type *type_hex(void);
Type *type_bin(void);
Type *type_oct(void);
Type *type_fn(FnParam *params, int param_count, Type *return_type);

// Build a builtin Fn type with raw arity info (no names).
// min_args  = required positional args
// opt_args  = optional positional args  (-1 = not applicable)
// variadic  = true if it accepts a rest arg
Type *type_fn_builtin(int min_args, int opt_args, bool variadic);
Type *type_clone(Type *t);
void type_free(Type *t);

// Pretty-print a type.
// For TYPE_FN produces:  Fn (_ _)  /  Fn (#:optional _ _)  / Fn (_ . _)  etc.
const char *type_to_string(Type *t);

// Infer the concrete type of a numeric literal from its value + original string.
Type *infer_literal_type(double value, const char *literal_str);

// Parse a bracket type annotation  [name :: TypeName]
// Returns the Type, or NULL if not a valid annotation.
struct AST;
Type *parse_type_annotation(struct AST *ast);

#endif
