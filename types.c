#include "types.h"
#include "reader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/// Simple constructors

static Type *make_type(TypeKind kind) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = kind;
    return t;
}

Type *type_int    (void) { return make_type(TYPE_INT);    }
Type *type_float  (void) { return make_type(TYPE_FLOAT);  }
Type *type_char   (void) { return make_type(TYPE_CHAR);   }
Type *type_string (void) { return make_type(TYPE_STRING); }
Type *type_bool   (void) { return make_type(TYPE_BOOL);   }
Type *type_hex    (void) { return make_type(TYPE_HEX);    }
Type *type_bin    (void) { return make_type(TYPE_BIN);    }
Type *type_oct    (void) { return make_type(TYPE_OCT);    }

Type *type_fn(FnParam *params, int param_count, Type *return_type) {
    Type *t        = make_type(TYPE_FN);
    t->params      = params;
    t->param_count = param_count;
    t->return_type = return_type;
    return t;
}

Type *type_fn_builtin(int min_args, int opt_args, bool variadic) {
    int total = min_args + (opt_args > 0 ? opt_args : 0) + (variadic ? 1 : 0);
    FnParam *params = total > 0 ? calloc(total, sizeof(FnParam)) : NULL;
    int idx = 0;

    for (int i = 0; i < min_args; i++) {
        params[idx].name     = NULL;
        params[idx].type     = NULL;
        params[idx].optional = false;
        params[idx].rest     = false;
        idx++;
    }
    for (int i = 0; i < opt_args; i++) {
        params[idx].name     = NULL;
        params[idx].type     = NULL;
        params[idx].optional = true;
        params[idx].rest     = false;
        idx++;
    }
    if (variadic) {
        params[idx].name     = NULL;
        params[idx].type     = NULL;
        params[idx].optional = false;
        params[idx].rest     = true;
    }

    return type_fn(params, total, NULL);
}

Type *type_clone(Type *t) {
    if (!t) return NULL;
    switch (t->kind) {
        case TYPE_INT:    return type_int();
        case TYPE_FLOAT:  return type_float();
        case TYPE_CHAR:   return type_char();
        case TYPE_STRING: return type_string();
        case TYPE_BOOL:   return type_bool();
        case TYPE_HEX:    return type_hex();
        case TYPE_BIN:    return type_bin();
        case TYPE_OCT:    return type_oct();
        default:          return make_type(t->kind);
    }
}

void type_free(Type *t) {
    if (!t) return;
    if (t->kind == TYPE_FN) {
        if (t->params) {
            for (int i = 0; i < t->param_count; i++) {
                free(t->params[i].name);
                type_free(t->params[i].type);
            }
            free(t->params);
        }
        type_free(t->return_type);
    }
    free(t);
}

// Returns a pointer to a static or thread-local buffer — caller must not free.
const char *type_to_string(Type *t) {
    if (!t) return "?";

    static char buf[512];

    switch (t->kind) {
    case TYPE_INT:     return "Int";
    case TYPE_FLOAT:   return "Float";
    case TYPE_CHAR:    return "Char";
    case TYPE_STRING:  return "String";
    case TYPE_BOOL:    return "Bool";
    case TYPE_HEX:     return "Hex";
    case TYPE_BIN:     return "Bin";
    case TYPE_OCT:     return "Oct";
    case TYPE_UNKNOWN: return "?";

    case TYPE_FN: {
        if (t->param_count == 0) {
            // Fn with no params — variadic list style
            snprintf(buf, sizeof(buf), "Fn _");
            return buf;
        }

        // Build the arity signature
        char sig[400] = {0};
        bool first_opt_seen = false;

        for (int i = 0; i < t->param_count; i++) {
            FnParam *p = &t->params[i];

            if (p->rest) {
                // rest arg: ". _"
                if (i > 0) strcat(sig, " ");
                strcat(sig, ". _");
            } else {
                if (p->optional && !first_opt_seen) {
                    if (i > 0) strcat(sig, " ");
                    strcat(sig, "#:optional");
                    first_opt_seen = true;
                }
                if (i > 0 || first_opt_seen) strcat(sig, " ");
                strcat(sig, "_");
            }
        }

        snprintf(buf, sizeof(buf), "Fn (%s)", sig);
        return buf;
    }
    }
    return "?";
}

Type *infer_literal_type(double value, const char *literal_str) {
    if (!literal_str) {
        // No string: check if value is integer
        if (value == (long long)value)
            return type_int();
        return type_float();
    }

    // Hex
    if (literal_str[0] == '0' &&
        (literal_str[1] == 'x' || literal_str[1] == 'X'))
        return type_hex();

    // Binary
    if (literal_str[0] == '0' &&
        (literal_str[1] == 'b' || literal_str[1] == 'B'))
        return type_bin();

    // Octal
    if (literal_str[0] == '0' &&
        (literal_str[1] == 'o' || literal_str[1] == 'O'))
        return type_oct();

    // Float: contains '.' or 'e'/'E'
    for (const char *p = literal_str; *p; p++) {
        if (*p == '.' || *p == 'e' || *p == 'E')
            return type_float();
    }

    return type_int();
}

// Parse type annotation [name :: TypeName] or [TypeName]
Type *parse_type_annotation(AST *ast) {
    if (!ast || ast->type != AST_LIST) return NULL;

    // Walk the list looking for "::" then the type name after it.
    // Form: [name :: TypeName]  or just [TypeName]
    for (size_t i = 0; i < ast->list.count; i++) {
        AST *item = ast->list.items[i];
        if (item->type == AST_SYMBOL && strcmp(item->symbol, "::") == 0) {
            // next item should be the type name
            if (i + 1 < ast->list.count) {
                AST *type_node = ast->list.items[i + 1];
                if (type_node->type != AST_SYMBOL) return NULL;
                const char *tn = type_node->symbol;
                if (strcmp(tn, "Int")    == 0) return type_int();
                if (strcmp(tn, "Float")  == 0) return type_float();
                if (strcmp(tn, "Char")   == 0) return type_char();
                if (strcmp(tn, "String") == 0) return type_string();
                if (strcmp(tn, "Bool")   == 0) return type_bool();
                if (strcmp(tn, "Hex")    == 0) return type_hex();
                if (strcmp(tn, "Bin")    == 0) return type_bin();
                if (strcmp(tn, "Oct")    == 0) return type_oct();
            }
            return NULL;
        }
    }
    return NULL;
}
