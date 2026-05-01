#include "types.h"
#include "reader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/// Type alias

TypeAlias *g_aliases = NULL;

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

/// Refinement type

RefinementEntry *g_refinements = NULL;

void refinement_register(const char *name, const char *pred_name,
                          const char *base_type, void *pred_ast,
                          const char *var) {
    RefinementEntry *e = malloc(sizeof(RefinementEntry));
    e->name          = strdup(name);
    e->pred_name     = strdup(pred_name);
    e->base_type     = strdup(base_type);
    e->predicate_ast = pred_ast ? ast_clone((AST*)pred_ast) : NULL;
    e->var           = var ? strdup(var) : NULL;
    e->next          = g_refinements;
    g_refinements    = e;
}

const char *refinement_pred_name(const char *type_name) {
    for (RefinementEntry *e = g_refinements; e; e = e->next)
        if (strcmp(e->name, type_name) == 0) return e->pred_name;
    return NULL;
}

void refinement_free_all(void) {
    RefinementEntry *e = g_refinements;
    while (e) {
        RefinementEntry *n = e->next;
        free(e->name); free(e->pred_name); free(e->base_type);
        free(e->var);
        ast_free(e->predicate_ast);
        free(e);
        e = n;
    }
    g_refinements = NULL;
}

/* Statically evaluate a simple predicate AST against a numeric constant.
 * Returns 1 (true), 0 (false), or -1 (cannot evaluate statically).     */
static int static_eval_pred(AST *pred, const char *var, double val) {
    if (!pred) return -1;

    /* (op lhs rhs) */
    if (pred->type == AST_LIST && pred->list.count == 3 &&
        pred->list.items[0]->type == AST_SYMBOL) {
        const char *op  = pred->list.items[0]->symbol;
        AST        *lhs = pred->list.items[1];
        AST        *rhs = pred->list.items[2];

        double lv, rv;

        /* Evaluate lhs */
        if (lhs->type == AST_NUMBER) {
            lv = lhs->number;
        } else if (lhs->type == AST_SYMBOL && strcmp(lhs->symbol, var) == 0) {
            lv = val;
        } else if (lhs->type == AST_LIST) {
            /* nested expression like (% x 2) */
            int r = static_eval_pred(lhs, var, val);
            if (r == -1) return -1;
            lv = (double)r;
        } else return -1;

        /* Evaluate rhs */
        if (rhs->type == AST_NUMBER) {
            rv = rhs->number;
        } else if (rhs->type == AST_SYMBOL && strcmp(rhs->symbol, var) == 0) {
            rv = val;
        } else if (rhs->type == AST_LIST) {
            int r = static_eval_pred(rhs, var, val);
            if (r == -1) return -1;
            rv = (double)r;
        } else return -1;

        /* For arithmetic ops, return the value not bool */
        if (strcmp(op, "%") == 0 || strcmp(op, "mod") == 0)
            return (int)((long long)lv % (long long)rv);
        if (strcmp(op, "+") == 0) return (int)(lv + rv);
        if (strcmp(op, "-") == 0) return (int)(lv - rv);
        if (strcmp(op, "*") == 0) return (int)(lv * rv);

        /* Comparison ops return 0 or 1 */
        if (strcmp(op, "=")  == 0) return lv == rv ? 1 : 0;
        if (strcmp(op, "!=") == 0) return lv != rv ? 1 : 0;
        if (strcmp(op, ">")  == 0) return lv >  rv ? 1 : 0;
        if (strcmp(op, ">=") == 0) return lv >= rv ? 1 : 0;
        if (strcmp(op, "<")  == 0) return lv <  rv ? 1 : 0;
        if (strcmp(op, "<=") == 0) return lv <= rv ? 1 : 0;

        /* Logical ops */
        if (strcmp(op, "and") == 0) {
            int lr = static_eval_pred(lhs, var, val);
            int rr = static_eval_pred(rhs, var, val);
            if (lr == -1 || rr == -1) return -1;
            return (lr && rr) ? 1 : 0;
        }
        if (strcmp(op, "or") == 0) {
            int lr = static_eval_pred(lhs, var, val);
            int rr = static_eval_pred(rhs, var, val);
            if (lr == -1 || rr == -1) return -1;
            return (lr || rr) ? 1 : 0;
        }
        return -1;
    }

    /* (not expr) */
    if (pred->type == AST_LIST && pred->list.count == 2 &&
        pred->list.items[0]->type == AST_SYMBOL &&
        strcmp(pred->list.items[0]->symbol, "not") == 0) {
        int r = static_eval_pred(pred->list.items[1], var, val);
        if (r == -1) return -1;
        return r ? 0 : 1;
    }

    /* bare variable */
    if (pred->type == AST_SYMBOL && strcmp(pred->symbol, var) == 0)
        return val != 0 ? 1 : 0;

    /* bare number */
    if (pred->type == AST_NUMBER)
        return pred->number != 0 ? 1 : 0;

    /* function call: (Name? arg) — look up refinement predicate recursively */
    if (pred->type == AST_LIST && pred->list.count == 2 &&
        pred->list.items[0]->type == AST_SYMBOL) {
        const char *fname = pred->list.items[0]->symbol;
        AST        *arg   = pred->list.items[1];

        /* Evaluate the argument */
        double arg_val;
        if (arg->type == AST_NUMBER) {
            arg_val = arg->number;
        } else if (arg->type == AST_SYMBOL && strcmp(arg->symbol, var) == 0) {
            arg_val = val;
        } else {
            int r = static_eval_pred(arg, var, val);
            if (r == -1) return -1;
            arg_val = (double)r;
        }

        /* Strip trailing '?' to get refinement name: "Even?" -> "Even" */
        size_t flen = strlen(fname);
        if (flen > 1 && fname[flen - 1] == '?') {
            char rname[256];
            strncpy(rname, fname, flen - 1);
            rname[flen - 1] = '\0';
            /* Look up in refinement registry */
            int r = refinement_check_literal(rname, arg_val, NULL);
            if (r != -1) return r;
        }
        return -1;
    }

    return -1; /* cannot evaluate statically */
}

int refinement_check_literal(const char *type_name, double val,
                               const char **out_pred_src) {
    /* Walk alias chain, stopping as soon as we hit a refinement entry */
    const char *name = type_name;
    char buf[256];
    for (int depth = 0; depth < 32; depth++) {
        /* Direct refinement lookup */
        for (RefinementEntry *e = g_refinements; e; e = e->next) {
            if (strcmp(e->name, name) == 0) {
                if (out_pred_src) *out_pred_src = e->pred_name;
                if (!e->predicate_ast) return -1;
                return static_eval_pred(e->predicate_ast, e->var, val);
            }
        }
        /* Not a refinement — try one alias step */
        bool stepped = false;
        for (TypeAlias *a = g_aliases; a; a = a->next) {
            if (strcmp(a->alias_name, name) == 0) {
                strncpy(buf, a->target_name, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                name = buf;
                stepped = true;
                break;
            }
        }
        if (!stepped) break;
    }
    return -1;
}


// Resolve a name to a Type*, checking aliases after builtins.
// Always returns a fresh allocation (or NULL if unknown).
Type *type_from_name(const char *name) {
    if (!name) return NULL;

    size_t len = strlen(name);
    if (len > 1 && name[len - 1] == '?') {
        char *inner_name = strndup(name, len - 1);
        Type *inner = type_from_name(inner_name);
        free(inner_name);
        if (inner) return type_optional(inner);
    }

    // Compound types (parsed dynamically from string annotations)
    if (strncmp(name, "Fn :: ", 6) == 0) {
        /* Fn :: (Int -> Int) — parse and return the full arrow type chain.
         * Codegen treats TYPE_ARROW params the same as TYPE_FN for closures. */
        Type *arrow = type_parse_fn_arrow(name);
        return arrow ? arrow : type_fn(NULL, 0, NULL);
    }
    if (strncmp(name, "Pointer :: ", 11) == 0) {
        const char *inner_name = name + 11;
        Type *inner_type = type_from_name(inner_name);
        if (!inner_type) {
            /* Treat undefined inner types as opaque C structs for FFI */
            inner_type = type_layout_ref(inner_name);
        }
        return type_ptr(inner_type);
    }

    if (strncmp(name, "Arr :: ", 7) == 0) {
        char buf[256];
        strncpy(buf, name + 7, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char *delim = strstr(buf, " :: ");
        if (delim) {
            *delim = '\0';
            Type *elem_type = type_from_name(buf);
            int size = atoi(delim + 4);
            return type_arr(elem_type, size);
        } else {
            Type *elem_type = type_from_name(buf);
            return type_arr_fat(elem_type);
        }
    }

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
    if (strcmp(name, "Arr")     == 0) return type_arr_fat(NULL);
    if (strcmp(name, "Set")     == 0) return type_set();
    if (strcmp(name, "Map")     == 0) return type_map();
    if (strcmp(name, "Coll")    == 0) return type_coll();
    if (strcmp(name, "F32")     == 0) return type_f32();
    if (strcmp(name, "I8")      == 0) return type_i8();
    if (strcmp(name, "U8")      == 0) return type_u8();
    if (strcmp(name, "I16")     == 0) return type_i16();
    if (strcmp(name, "U16")     == 0) return type_u16();
    if (strcmp(name, "I32")     == 0) return type_i32();
    if (strcmp(name, "U32")     == 0) return type_u32();
    if (strcmp(name, "I64")     == 0) return type_i64();
    if (strcmp(name, "U64")     == 0) return type_u64();
    if (strcmp(name, "I128")    == 0) return type_i128();
    if (strcmp(name, "U128")    == 0) return type_u128();
    if (strcmp(name, "Fn")      == 0) return type_fn(NULL, 0, NULL);
    if (strcmp(name, "Pointer") == 0) return type_ptr(NULL);
    if (strcmp(name, "F80")     == 0) return type_f80();

    /* Arbitrary-width integers: I<n> and U<n> */
    {
        int width = 0; bool is_signed = false;
        if (parse_int_type(name, 0, 0, &width, &is_signed))
            return type_int_arbitrary(width, is_signed);
    }

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

Type *type_unknown(void) { return make_type(TYPE_UNKNOWN); }
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
Type *type_set    (void) { return make_type(TYPE_SET);     }
Type *type_map    (void) { return make_type(TYPE_MAP);     }
Type *type_coll   (void) { return make_type(TYPE_COLL);    }
Type *type_f32    (void) { return make_type(TYPE_F32);     }
Type *type_i8     (void) { return make_type(TYPE_I8);      }
Type *type_u8     (void) { return make_type(TYPE_U8);      }
Type *type_i16    (void) { return make_type(TYPE_I16);     }
Type *type_u16    (void) { return make_type(TYPE_U16);     }
Type *type_i32    (void) { return make_type(TYPE_I32);     }
Type *type_u32    (void) { return make_type(TYPE_U32);     }
Type *type_i64    (void) { return make_type(TYPE_I64);     }
Type *type_u64    (void) { return make_type(TYPE_U64);     }
Type *type_i128   (void) { return make_type(TYPE_I128);    }
Type *type_u128   (void) { return make_type(TYPE_U128);    }
Type *type_f80    (void) { return make_type(TYPE_F80);     }

Type *type_int_arbitrary(int width, bool is_signed) {
    Type *t = make_type(TYPE_INT_ARBITRARY);
    t->numeric_width  = width;
    t->numeric_signed = is_signed;
    return t;
}

Type *type_nil(void) { return make_type(TYPE_NIL); }

Type *type_optional(Type *inner) {
    Type *t = make_type(TYPE_OPTIONAL);
    t->element_type = inner;
    return t;
}

Type *type_list(Type *element_type) {
    Type *t = make_type(TYPE_LIST);
    t->element_type = element_type;
    return t;
}

Type *type_arr(Type *element_type, int size) {
    Type *t = make_type(TYPE_ARR);
    t->arr_element_type = element_type;
    t->arr_size = size;
    t->arr_is_fat = false;
    return t;
}

Type *type_arr_fat(Type *element_type) {
    Type *t = make_type(TYPE_ARR);
    t->arr_element_type = element_type;
    t->arr_size = -1;
    t->arr_is_fat = true;
    return t;
}

Type *type_layout(const char *name,
                  LayoutField *fields, int field_count,
                  int total_size, bool packed, int align) {
    Type *t = make_type(TYPE_LAYOUT);
    t->layout_name        = strdup(name);
    t->layout_fields      = fields;
    t->layout_field_count = field_count;
    t->layout_total_size  = total_size;
    t->layout_packed      = packed;
    t->layout_align       = align;
    return t;
}

Type *type_ptr(Type *pointee) {
    Type *t = make_type(TYPE_PTR);
    t->element_type = pointee;  // reuse element_type field for pointee
    return t;
}

Type *type_layout_ref(const char *name) {
    Type *t = calloc(1, sizeof(Type));
    t->kind               = TYPE_LAYOUT;
    t->layout_name        = strdup(name);
    t->layout_fields      = NULL;
    t->layout_field_count = 0;
    t->layout_total_size  = 0;
    t->layout_packed      = false;
    t->layout_align       = 0;
    t->arr_size           = -1;
    return t;
}

Type *type_fn(FnParam *params, int param_count, Type *return_type) {
    Type *t        = make_type(TYPE_FN);
    t->params      = params;
    t->param_count = param_count;
    t->return_type = return_type;
    return t;
}

Type *type_var(int id) {
    Type *t  = make_type(TYPE_VAR);
    t->var_id = id;
    return t;
}

Type *type_arrow(Type *param, Type *ret) {
    Type *t        = make_type(TYPE_ARROW);
    t->arrow_param = param;
    t->arrow_ret   = ret;
    return t;
}

bool types_equal(Type *a, Type *b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case TYPE_VAR:
        return a->var_id == b->var_id;
    case TYPE_ARROW:
        return types_equal(a->arrow_param, b->arrow_param)
            && types_equal(a->arrow_ret,   b->arrow_ret);
    case TYPE_LIST:
    case TYPE_OPTIONAL:
        return types_equal(a->element_type, b->element_type);
    case TYPE_ARR:
        return a->arr_size == b->arr_size
            && types_equal(a->arr_element_type, b->arr_element_type);
    default:
        return true;  /* ground types with equal kinds are equal */
    }
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

/* Parse "Fn :: (Int -> Int)" into a TYPE_ARROW chain for HM checking.
 * Returns NULL if the name is not a Fn :: (...) annotation.
 * Caller owns the returned type.                                        */
Type *type_parse_fn_arrow(const char *name) {
    if (!name || strncmp(name, "Fn :: ", 6) != 0) return NULL;
    const char *inner = name + 6;
    if (inner[0] == '(') inner++;
    char buf[512];
    strncpy(buf, inner, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    size_t blen = strlen(buf);
    if (blen > 0 && buf[blen - 1] == ')') buf[--blen] = '\0';
    const char *parts[16];
    int nparts = 0;
    char *p2 = buf;
    while (p2 && nparts < 16) {
        parts[nparts++] = p2;
        char *arrow_pos = strstr(p2, " -> ");
        if (!arrow_pos) break;
        *arrow_pos = '\0';
        p2 = arrow_pos + 4;
    }
    if (nparts < 2) return NULL;
    Type *result = type_from_name(parts[nparts - 1]);
    if (!result) result = type_unknown();
    for (int _i = nparts - 2; _i >= 0; _i--) {
        Type *param_t = type_from_name(parts[_i]);
        if (!param_t) param_t = type_unknown();
        result = type_arrow(param_t, result);
    }
    return result;
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
        case TYPE_COLL:    return type_coll();
        case TYPE_F32:     return type_f32();
        case TYPE_I8:      return type_i8();
        case TYPE_U8:      return type_u8();
        case TYPE_I16:     return type_i16();
        case TYPE_U16:     return type_u16();
        case TYPE_I32:     return type_i32();
        case TYPE_U32:     return type_u32();
        case TYPE_I64:     return type_i64();
        case TYPE_U64:     return type_u64();
        case TYPE_I128:    return type_i128();
        case TYPE_U128:    return type_u128();
        case TYPE_ARR: {
            Type *c = type_arr(type_clone(t->arr_element_type), t->arr_size);
            c->arr_is_fat = t->arr_is_fat;
            return c;
        }
        case TYPE_PTR:          return type_ptr(type_clone(t->element_type));
        case TYPE_OPTIONAL:     return type_optional(type_clone(t->element_type));
        case TYPE_NIL:          return type_nil();
        case TYPE_F80:          return type_f80();
        case TYPE_INT_ARBITRARY: return type_int_arbitrary(t->numeric_width, t->numeric_signed);
        case TYPE_VAR:     return type_var(t->var_id);
        case TYPE_ARROW:   return type_arrow(type_clone(t->arrow_param), type_clone(t->arrow_ret));
        case TYPE_LAYOUT: {
            Type *c = calloc(1, sizeof(Type));
            c->kind                = TYPE_LAYOUT;
            c->layout_name         = t->layout_name ? strdup(t->layout_name) : NULL;
            c->layout_fields       = t->layout_fields;       // shared, not deep-copied
            c->layout_field_count  = t->layout_field_count;
            c->layout_total_size   = t->layout_total_size;
            c->layout_packed       = t->layout_packed;
            c->layout_align        = t->layout_align;
            c->layout_is_inline    = t->layout_is_inline;
            return c;
        }

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
    if (t->kind == TYPE_LIST || t->kind == TYPE_PTR || t->kind == TYPE_OPTIONAL) {
        type_free(t->element_type);
    }
    if (t->kind == TYPE_ARR) {
        type_free(t->arr_element_type);
    }
    if (t->kind == TYPE_LAYOUT) {
        free(t->layout_name);
        // layout_fields is shared with the registry — do not free it
    }
    if (t->kind == TYPE_ARROW) {
        type_free(t->arrow_param);
        type_free(t->arrow_ret);
    }
    if (t->kind == TYPE_VAR) {}
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
    case TYPE_SET:     return "Set";
    case TYPE_MAP:     return "Map";
    case TYPE_F32:     return "F32";
    case TYPE_I8:      return "I8";
    case TYPE_U8:      return "U8";
    case TYPE_I16:     return "I16";
    case TYPE_U16:     return "U16";
    case TYPE_I32:     return "I32";
    case TYPE_U32:     return "U32";
    case TYPE_I64:     return "I64";
    case TYPE_U64:     return "U64";
    case TYPE_I128:    return "I128";
    case TYPE_U128:    return "U128";
    case TYPE_UNKNOWN: return "?";
    case TYPE_NIL:     return "Nil";
    case TYPE_F80:     return "F80";
    case TYPE_INT_ARBITRARY: {
        static char ibuf[16];
        snprintf(ibuf, sizeof(ibuf), "%c%d",
                 t->numeric_signed ? 'I' : 'U', t->numeric_width);
        return ibuf;
    }
    case TYPE_PTR: {
        static char pbuf[256];
        snprintf(pbuf, sizeof(pbuf), "Pointer :: %s",
                 type_to_string(t->element_type));
        return pbuf;
    }
    case TYPE_OPTIONAL: {
        static char obuf[256];
        snprintf(obuf, sizeof(obuf), "%s?", type_to_string(t->element_type));
        return obuf;
    }
    case TYPE_VAR: {
        static char vbuf[32];
        snprintf(vbuf, sizeof(vbuf), "'%c",
                 'a' + (t->var_id % 26));
        if (t->var_id >= 26)
            snprintf(vbuf + 2, sizeof(vbuf) - 2, "%d", t->var_id / 26);
        return vbuf;
    }

    case TYPE_ARROW: {
        static char abuf[256];
        snprintf(abuf, sizeof(abuf), "(%s -> %s)",
                 type_to_string(t->arrow_param),
                 type_to_string(t->arrow_ret));
        return abuf;
    }

    case TYPE_LIST: {
        if (t->element_type) {
            snprintf(buf, sizeof(buf), "(%s)", type_to_string(t->element_type));
        } else {
            snprintf(buf, sizeof(buf), "(a)");
        }
        return buf;
    }
    case TYPE_COLL:    return "[a]";

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

    case TYPE_LAYOUT:
        return t->layout_name ? t->layout_name : "<layout>";

    case TYPE_FN: {
        if (t->param_count == 0) {
            // Fn with no params — variadic list style
            snprintf(buf, sizeof(buf), "Fn");
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
Type *parse_type_annotation(struct AST *ast) {
    if (!ast) return NULL;
    size_t count;
    struct AST **items;

    if (ast->type == AST_LIST) {
        count = ast->list.count;
        items = ast->list.items;
    } else if (ast->type == AST_ARRAY) {
        count = ast->array.element_count;
        items = ast->array.elements;
    } else {
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        bool is_colon = items[i]->type == AST_SYMBOL && strcmp(items[i]->symbol, "::") == 0;
        bool is_arrow = items[i]->type == AST_SYMBOL && strcmp(items[i]->symbol, "->") == 0;

        if (!is_colon && !is_arrow) continue;
        if (i + 1 >= count) return NULL;

        /* [name -> T] is sugar for [name :: Pointer :: T] */
        if (is_arrow) {
            struct AST *inner_node = items[i + 1];
            Type *inner = NULL;

            // Support array syntax in arrow shorthand
            if (inner_node->type == AST_ARRAY) {
                inner = type_coll();
            } else if (inner_node->type == AST_SYMBOL) {
                inner = type_from_name(inner_node->symbol);
                if (!inner) inner = type_layout_ref(inner_node->symbol);
            }

            if (!inner) return NULL;
            return type_ptr(inner);
        }

        struct AST *type_node = items[i + 1];

        // ALIAS: Treat any array literal in a type signature as Coll
        if (type_node->type == AST_ARRAY) {
            return type_coll();
        }

        if (type_node->type != AST_SYMBOL) return NULL;
        const char *tn = type_node->symbol;

        if (strcmp(tn, "Arr") == 0) {
            Type *elem_type = NULL;
            int   size      = -1;
            if (i + 2 < count && items[i+2]->type == AST_SYMBOL &&
                strcmp(items[i+2]->symbol, "::") == 0 &&
                i + 3 < count && items[i+3]->type == AST_SYMBOL) {
                elem_type = type_from_name(items[i+3]->symbol);
                if (i + 4 < count && items[i+4]->type == AST_SYMBOL &&
                    strcmp(items[i+4]->symbol, "::") == 0 &&
                    i + 5 < count && items[i+5]->type == AST_NUMBER) {
                    size = (int)items[i+5]->number;
                }
            }
            return type_arr(elem_type, size);
        }
        if (strcmp(tn, "Pointer") == 0) {
            if (i + 2 < count && items[i+2]->type == AST_SYMBOL &&
                strcmp(items[i+2]->symbol, "::") == 0 &&
                i + 3 < count && items[i+3]->type == AST_SYMBOL) {
                Type *inner = type_from_name(items[i+3]->symbol);
                if (!inner) inner = type_layout_ref(items[i+3]->symbol);
                return type_ptr(inner);
            }
            return type_ptr(NULL);
        }
        return type_from_name(tn);
    }
    return NULL;
}

// Compute field sizes and offsets, respecting packed/align.
// Returns the total struct size.
// elem_size_fn: callback that returns byte size for a type name.
int layout_compute_offsets(LayoutField *fields, int count,
                           bool packed,
                           int (*elem_size_fn)(const char *type_name)) {
    int offset = 0;
    for (int i = 0; i < count; i++) {
        // If size is already filled (codegen path), use it directly.
        // If not and elem_size_fn is provided, use it.
        int sz = fields[i].size;
        if (sz <= 0 && elem_size_fn)
            sz = elem_size_fn(fields[i].name);
        if (sz <= 0) sz = 8; // fallback

        if (!packed && sz > 1) {
            int align = sz < 8 ? sz : 8;
            offset = (offset + align - 1) & ~(align - 1);
        }
        fields[i].offset = offset;
        fields[i].size   = sz;
        offset += sz;
    }
    return offset;
}
