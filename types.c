#include "types.h"
#include "reader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/// Type constructors

Type *type_int(void) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_INT;
    return t;
}

Type *type_float(void) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_FLOAT;
    return t;
}

Type *type_char(void) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_CHAR;
    return t;
}

Type *type_string(void) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_STRING;
    return t;
}

Type *type_hex(void) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_HEX;
    return t;
}

Type *type_bin(void) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_BIN;
    return t;
}

Type *type_oct(void) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_OCT;
    return t;
}

Type *type_bool(void) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_BOOL;
    return t;
}

Type *type_unknown(void) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_UNKNOWN;
    return t;
}

Type *type_function(Type *param, Type *ret) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_FUNCTION;
    t->func.param_type = param;
    t->func.return_type = ret;
    return t;
}

Type *type_generic(const char *name) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_GENERIC;
    t->generic.name = strdup(name);
    return t;
}

/// Type operations

bool type_equals(Type *a, Type *b) {
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
    case TYPE_FUNCTION:
        return type_equals(a->func.param_type, b->func.param_type) &&
               type_equals(a->func.return_type, b->func.return_type);
    case TYPE_GENERIC:
        return strcmp(a->generic.name, b->generic.name) == 0;
    default:
        return true;
    }
}

void type_free(Type *t) {
    if (!t) return;

    switch (t->kind) {
    case TYPE_FUNCTION:
        type_free(t->func.param_type);
        type_free(t->func.return_type);
        break;
    case TYPE_GENERIC:
        free(t->generic.name);
        break;
    default:
        break;
    }
    free(t);
}

const char *type_to_string(Type *t) {
    if (!t) return "null";

    switch (t->kind) {
    case TYPE_INT:      return "Int";
    case TYPE_FLOAT:    return "Float";
    case TYPE_CHAR:     return "Char";
    case TYPE_STRING:   return "String";
    case TYPE_HEX:      return "Hex";
    case TYPE_BIN:      return "Bin";
    case TYPE_OCT:      return "Oct";
    case TYPE_BOOL:     return "Bool";
    case TYPE_UNKNOWN:  return "Unknown";
    case TYPE_FUNCTION: return "Function";
    case TYPE_GENERIC:  return t->generic.name;
    }
    return "Unknown";
}

Type *type_clone(Type *t) {
    if (!t) return NULL;

    switch (t->kind) {
    case TYPE_INT:     return type_int();
    case TYPE_FLOAT:   return type_float();
    case TYPE_CHAR:    return type_char();
    case TYPE_STRING:  return type_string();
    case TYPE_HEX:     return type_hex();
    case TYPE_BIN:     return type_bin();
    case TYPE_OCT:     return type_oct();
    case TYPE_BOOL:    return type_bool();
    case TYPE_UNKNOWN: return type_unknown();
    case TYPE_FUNCTION:
        return type_function(type_clone(t->func.param_type),
                           type_clone(t->func.return_type));
    case TYPE_GENERIC:
        return type_generic(t->generic.name);
    }
    return type_unknown();
}

/// Type inference

Type *infer_literal_type(double value, const char *literal_str) {
    // Check the literal string to determine the type
    if (literal_str && strlen(literal_str) > 2) {
        if (literal_str[0] == '0') {
            char prefix = literal_str[1];
            if (prefix == 'x' || prefix == 'X') {
                return type_hex();
            }
            if (prefix == 'b' || prefix == 'B') {
                return type_bin();
            }
            if (prefix == 'o' || prefix == 'O') {
                return type_oct();
            }
        }
    }

    // Check if literal string contains a decimal point - if so, it's a float
    if (literal_str && strchr(literal_str, '.')) {
        return type_float();
    }

    // Check if it's an integer
    if (floor(value) == value) {
        return type_int();
    }
    return type_float();
}

// Parse type annotation from AST like [x :: Int]
Type *parse_type_annotation(void *ast_ptr) {
    AST *ast = (AST *)ast_ptr;

    if (!ast || ast->type != AST_LIST) {
        return NULL;
    }

    // Expecting format: [name :: Type]
    if (ast->list.count != 3) {
        return NULL;
    }

    // Second element should be "::"
    if (ast->list.items[1]->type != AST_SYMBOL ||
        strcmp(ast->list.items[1]->symbol, "::") != 0) {
        return NULL;
    }

    // Third element is the type
    AST *type_ast = ast->list.items[2];
    if (type_ast->type != AST_SYMBOL) {
        return NULL;
    }

    const char *type_name = type_ast->symbol;

    if (strcmp(type_name, "Int")    == 0) return type_int();
    if (strcmp(type_name, "Float")  == 0) return type_float();
    if (strcmp(type_name, "Char")   == 0) return type_char();
    if (strcmp(type_name, "String") == 0) return type_string();
    if (strcmp(type_name, "Hex")    == 0) return type_hex();
    if (strcmp(type_name, "Bin")    == 0) return type_bin();
    if (strcmp(type_name, "Oct")    == 0) return type_oct();
    if (strcmp(type_name, "Bool")   == 0) return type_bool();

    // Could be a generic type variable
    return type_generic(type_name);
}
