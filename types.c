#include "types.h"
#include "reader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/// Type alias registry

typedef struct TypeAlias {
    char *alias_name;
    char *target_name;
    struct TypeAlias *next;
} TypeAlias;

static TypeAlias *g_aliases = NULL;

void type_alias_register(const char *alias_name, const char *target_name) {
    // Replace if already exists
    for (TypeAlias *a = g_aliases; a; a = a->next) {
        if (strcmp(a->alias_name, alias_name) == 0) {
            free(a->target_name);
            a->target_name = strdup(target_name);
            return;
        }
    }
    TypeAlias *a  = malloc(sizeof(TypeAlias));
    a->alias_name  = strdup(alias_name);
    a->target_name = strdup(target_name);
    a->next        = g_aliases;
    g_aliases      = a;
}

void type_alias_free_all(void) {
    TypeAlias *a = g_aliases;
    while (a) {
        TypeAlias *next = a->next;
        free(a->alias_name);
        free(a->target_name);
        free(a);
        a = next;
    }
    g_aliases = NULL;
}

// Resolve a name to a Type*, checking aliases after builtins.
// Always returns a fresh allocation (or NULL if unknown).
Type *type_from_name(const char *name) {
    if (!name) return NULL;

    // Built-in types first
    if (strcmp(name, "Int")     == 0) return type_int();
    if (strcmp(name, "Float")   == 0) return type_float();
    if (strcmp(name, "Char")    == 0) return type_char();
    if (strcmp(name, "String")  == 0) return type_string();
    if (strcmp(name, "Bool")    == 0) return type_bool();
    if (strcmp(name, "Hex")     == 0) return type_hex();
    if (strcmp(name, "Bin")     == 0) return type_bin();
    if (strcmp(name, "Oct")     == 0) return type_oct();
    if (strcmp(name, "Keyword") == 0) return type_keyword();
    if (strcmp(name, "Ratio")   == 0) return type_ratio();
    if (strcmp(name, "List")    == 0) return type_list(NULL);
    if (strcmp(name, "Arr")     == 0) return type_arr(NULL, -1);

    // Check alias registry (supports chained aliases: Code -> List -> ...)
    int depth = 0;
    const char *current = name;
    while (depth++ < 32) {  // prevent infinite loops
        for (TypeAlias *a = g_aliases; a; a = a->next) {
            if (strcmp(a->alias_name, current) == 0) {
                // Try to resolve the target as a builtin first
                Type *t = type_from_name(a->target_name);
                if (t) return t;
                current = a->target_name;
                goto next_iteration;
            }
        }
        break;  // not found in aliases
        next_iteration:;
    }

    return NULL;  // unknown type
}

/// Simple constructors

static Type *make_type(TypeKind kind) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = kind;
    t->arr_size = -1; // default for non-array types
    return t;
}

Type *type_int    (void) { return make_type(TYPE_INT);     }
Type *type_float  (void) { return make_type(TYPE_FLOAT);   }
Type *type_char   (void) { return make_type(TYPE_CHAR);    }
Type *type_string (void) { return make_type(TYPE_STRING);  }
Type *type_symbol (void) { return make_type(TYPE_SYMBOL);  }
Type *type_bool   (void) { return make_type(TYPE_BOOL);    }
Type *type_hex    (void) { return make_type(TYPE_HEX);     }
Type *type_bin    (void) { return make_type(TYPE_BIN);     }
Type *type_oct    (void) { return make_type(TYPE_OCT);     }
Type *type_keyword(void) { return make_type(TYPE_KEYWORD); }
Type *type_ratio  (void) { return make_type(TYPE_RATIO);   }

Type *type_list(Type *element_type) {
    Type *t = make_type(TYPE_LIST);
    t->element_type = element_type;
    return t;
}

Type *type_arr(Type *element_type, int size) {
    Type *t = make_type(TYPE_ARR);
    t->arr_element_type = element_type;
    t->arr_size = size;
    return t;
}

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
        case TYPE_INT:     return type_int();
        case TYPE_FLOAT:   return type_float();
        case TYPE_CHAR:    return type_char();
        case TYPE_STRING:  return type_string();
        case TYPE_SYMBOL:  return type_symbol();
        case TYPE_BOOL:    return type_bool();
        case TYPE_HEX:     return type_hex();
        case TYPE_BIN:     return type_bin();
        case TYPE_OCT:     return type_oct();
        case TYPE_KEYWORD: return type_keyword();
        case TYPE_RATIO:   return type_ratio();
        case TYPE_LIST:    return type_list(type_clone(t->element_type));
        case TYPE_ARR:     return type_arr(type_clone(t->arr_element_type), t->arr_size);
        default:           return make_type(t->kind);
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
    if (t->kind == TYPE_LIST) {
        type_free(t->element_type);
    }
    if (t->kind == TYPE_ARR) {
        type_free(t->arr_element_type);
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
    case TYPE_SYMBOL:  return "Symbol";
    case TYPE_BOOL:    return "Bool";
    case TYPE_HEX:     return "Hex";
    case TYPE_BIN:     return "Bin";
    case TYPE_OCT:     return "Oct";
    case TYPE_KEYWORD: return "Keyword";
    case TYPE_RATIO:   return "Ratio";
    case TYPE_UNKNOWN: return "?";

    case TYPE_LIST: {
        if (t->element_type) {
            snprintf(buf, sizeof(buf), "List<%s>", type_to_string(t->element_type));
        } else {
            snprintf(buf, sizeof(buf), "List<?>");
        }
        return buf;
    }

    case TYPE_ARR: {
        if (t->arr_element_type && t->arr_size >= 0) {
            snprintf(buf, sizeof(buf), "Arr :: %s :: %d",
                     type_to_string(t->arr_element_type), t->arr_size);
        } else if (t->arr_element_type) {
            snprintf(buf, sizeof(buf), "Arr :: %s", type_to_string(t->arr_element_type));
        } else if (t->arr_size >= 0) {
            snprintf(buf, sizeof(buf), "Arr :: ? :: %d", t->arr_size);
        } else {
            snprintf(buf, sizeof(buf), "Arr");
        }
        return buf;
    }

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

    // Ratio: contains '/' but not in hex/bin/oct context
    // Must check this BEFORE other checks
    const char *slash = strchr(literal_str, '/');
    if (slash && slash > literal_str && *(slash + 1) != '\0') {
        // Make sure it's not a comment or something else
        // Check that there are digits on both sides
        bool has_digit_before = false;
        bool has_digit_after = false;

        for (const char *p = literal_str; p < slash; p++) {
            if (*p >= '0' && *p <= '9') has_digit_before = true;
        }
        for (const char *p = slash + 1; *p; p++) {
            if (*p >= '0' && *p <= '9') has_digit_after = true;
        }

        if (has_digit_before && has_digit_after) {
            return type_ratio();
        }
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

// Parse type annotation [name :: TypeName] or [name :: Arr :: Int :: 3]
Type *parse_type_annotation(AST *ast) {
    if (!ast || ast->type != AST_LIST) return NULL;

    for (size_t i = 0; i < ast->list.count; i++) {
        AST *item = ast->list.items[i];
        if (item->type == AST_SYMBOL && strcmp(item->symbol, "::") == 0) {
            if (i + 1 >= ast->list.count) return NULL;
            AST *type_node = ast->list.items[i + 1];
            if (type_node->type != AST_SYMBOL) return NULL;
            const char *tn = type_node->symbol;

            // Array: [x :: Arr :: ElemType :: Size]
            if (strcmp(tn, "Arr") == 0) {
                Type *elem_type = NULL;
                int   size      = -1;

                if (i + 2 < ast->list.count &&
                    ast->list.items[i+2]->type == AST_SYMBOL &&
                    strcmp(ast->list.items[i+2]->symbol, "::") == 0 &&
                    i + 3 < ast->list.count &&
                    ast->list.items[i+3]->type == AST_SYMBOL) {

                    elem_type = type_from_name(ast->list.items[i+3]->symbol);

                    if (i + 4 < ast->list.count &&
                        ast->list.items[i+4]->type == AST_SYMBOL &&
                        strcmp(ast->list.items[i+4]->symbol, "::") == 0 &&
                        i + 5 < ast->list.count &&
                        ast->list.items[i+5]->type == AST_NUMBER) {
                        size = (int)ast->list.items[i+5]->number;
                    }
                }
                return type_arr(elem_type, size);
            }

            // Everything else — including aliases
            return type_from_name(tn);
        }
    }
    return NULL;
}
