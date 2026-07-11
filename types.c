#include "types.h"
#include "compat.h"
#include "reader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static Type *make_type(TypeKind kind);

static char *type_trim_copy(const char *s) {
    if (!s) return strdup("");
    while (*s == ' ' || *s == '\t') s++;
    const char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;
    return strndup(s, (size_t)(end - s));
}

static const char *type_find_top_level_arrow(const char *s) {
    int depth = 0;
    for (const char *p = s; p && *p; p++) {
        if (*p == '(' || *p == '[' || *p == '{') {
            depth++;
        } else if (*p == ')' || *p == ']' || *p == '}') {
            if (depth > 0) depth--;
        } else if (depth == 0 && p[0] == '-' && p[1] == '>') {
            return p;
        }
    }
    return NULL;
}

static Type *type_parse_arrow_chain(const char *name) {
    const char *arrow = type_find_top_level_arrow(name);
    if (!arrow) return NULL;

    char *left_s = strndup(name, (size_t)(arrow - name));
    char *left = type_trim_copy(left_s);
    free(left_s);
    char *right = type_trim_copy(arrow + 2);

    Type *ret = type_from_name(right);
    free(right);
    if (!ret) ret = type_unknown();

    Type *param = type_from_name(left);
    free(left);
    if (!param) param = type_unknown();

    return type_arrow(param, ret);
}

static bool type_has_top_level_comma(const char *name) {
    int depth = 0;

    for (const char *p = name; p && *p; p++) {
        if (*p == '(' || *p == '[' || *p == '{') {
            depth++;
            continue;
        }

        if (*p == ')' || *p == ']' || *p == '}') {
            if (depth > 0)
                depth--;
            continue;
        }

        if (depth == 0 && *p == ',')
            return true;
    }

    return false;
}

static Type *type_parse_comma_tuple(const char *name) {
    if (!name || !type_has_top_level_comma(name))
        return NULL;

    Type *t = make_type(TYPE_LIST);
    t->list_count = 0;
    t->list_types = NULL;

    const char *seg_start = name;
    int depth = 0;

    for (const char *p = name; ; p++) {
        bool at_end = (*p == '\0');
        bool at_split = false;

        if (!at_end) {
            if (*p == '(' || *p == '[' || *p == '{') {
                depth++;
            } else if (*p == ')' || *p == ']' || *p == '}') {
                if (depth > 0)
                    depth--;
            } else if (depth == 0 && *p == ',') {
                at_split = true;
            }
        }

        if (at_end || at_split) {
            char *raw = strndup(seg_start, (size_t)(p - seg_start));
            char *part = type_trim_copy(raw);
            free(raw);

            if (part[0]) {
                Type *elem = type_from_name(part);
                if (!elem)
                    elem = type_unknown();

                t->list_count++;
                t->list_types = realloc(t->list_types,
                                        sizeof(Type *) * t->list_count);
                t->list_types[t->list_count - 1] = elem;
            }

            free(part);

            if (at_end)
                break;

            seg_start = p + 1;
        }
    }

    if (t->list_count < 2) {
        type_free(t);
        return NULL;
    }

    return t;
}

static bool type_name_is_builtin_constructor(const char *name) {
    return strcmp(name, "Int") == 0 || strcmp(name, "Float") == 0 ||
           strcmp(name, "Char") == 0 || strcmp(name, "Byte") == 0 ||
           strcmp(name, "String") == 0 || strcmp(name, "Bool") == 0 ||
           strcmp(name, "Hex") == 0 || strcmp(name, "Bin") == 0 ||
           strcmp(name, "Oct") == 0 || strcmp(name, "Keyword") == 0 ||
           strcmp(name, "Ratio") == 0 || strcmp(name, "List") == 0 ||
           strcmp(name, "Arr") == 0 || strcmp(name, "Set") == 0 ||
           strcmp(name, "Map") == 0 || strcmp(name, "Coll") == 0 ||
           strcmp(name, "Fn") == 0 || strcmp(name, "Pointer") == 0 ||
           strcmp(name, "Path") == 0 || strcmp(name, "Escape") == 0 ||
           strcmp(name, "Unit") == 0 || strcmp(name, "Heap") == 0;
}

static char **type_split_top_level_space(const char *name, int *out_count) {
    *out_count = 0;
    char **parts = NULL;
    const char *p = name;

    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char *start = p;
        int depth = 0;
        while (*p) {
            if (*p == '(' || *p == '[' || *p == '{') {
                depth++;
            } else if (*p == ')' || *p == ']' || *p == '}') {
                if (depth > 0) depth--;
            } else if (depth == 0 && (*p == ' ' || *p == '\t')) {
                break;
            }
            p++;
        }

        char *raw = strndup(start, (size_t)(p - start));
        char *part = type_trim_copy(raw);
        free(raw);
        if (part[0]) {
            parts = realloc(parts, sizeof(char *) * ((size_t)*out_count + 1));
            parts[*out_count] = part;
            (*out_count)++;
        } else {
            free(part);
        }
    }

    return parts;
}

static void type_free_split_parts(char **parts, int count) {
    for (int i = 0; i < count; i++)
        free(parts[i]);
    free(parts);
}

static Type *type_parse_type_application(const char *name) {
    int count = 0;
    char **parts = type_split_top_level_space(name, &count);
    if (count < 2) {
        type_free_split_parts(parts, count);
        return NULL;
    }

    const char *constructor = parts[0];
    if (!(constructor[0] >= 'A' && constructor[0] <= 'Z') ||
        type_name_is_builtin_constructor(constructor)) {
        type_free_split_parts(parts, count);
        return NULL;
    }

    Type *arg = NULL;
    if (count == 2) {
        arg = type_from_name(parts[1]);
        if (!arg) arg = type_unknown();
    } else {
        arg = make_type(TYPE_LIST);
        arg->list_count = 0;
        arg->list_types = NULL;
        for (int i = 1; i < count; i++) {
            Type *elem = type_from_name(parts[i]);
            if (!elem) elem = type_unknown();
            arg->list_count++;
            arg->list_types = realloc(arg->list_types,
                                      sizeof(Type *) * arg->list_count);
            arg->list_types[arg->list_count - 1] = elem;
        }
    }

    Type *app = type_app(constructor, arg);
    type_free_split_parts(parts, count);
    return app;
}

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

static const char *type_alias_target_name(const char *type_name) {
    if (!type_name) return NULL;
    for (TypeAlias *a = g_aliases; a; a = a->next)
        if (a->alias_name && strcmp(a->alias_name, type_name) == 0)
            return a->target_name;
    return NULL;
}

static const char *refinement_base_name(const char *type_name) {
    if (!type_name) return NULL;
    for (RefinementEntry *e = g_refinements; e; e = e->next)
        if (e->name && strcmp(e->name, type_name) == 0)
            return e->base_type;
    return NULL;
}

static bool numeric_type_info(const char *name, bool *is_int,
                              bool *is_unsigned, int *width,
                              bool *is_float) {
    if (!name) return false;
    *is_int = false;
    *is_unsigned = false;
    *width = 0;
    *is_float = false;

    if (strcmp(name, "Int") == 0) {
        *is_int = true;
        *width = 64;
        return true;
    }
    if (strcmp(name, "Byte") == 0 || strcmp(name, "U8") == 0) {
        *is_int = true;
        *is_unsigned = true;
        *width = 8;
        return true;
    }
    if (strcmp(name, "Hex") == 0 || strcmp(name, "Bin") == 0 ||
        strcmp(name, "Oct") == 0) {
        *is_int = true;
        *width = 64;
        return true;
    }
    if ((name[0] == 'U' || name[0] == 'I') &&
        name[1] >= '1' && name[1] <= '9') {
        char *end = NULL;
        long parsed = strtol(name + 1, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 128) {
            *is_int = true;
            *is_unsigned = name[0] == 'U';
            *width = (int)parsed;
            return true;
        }
    }
    if (strcmp(name, "Float") == 0) {
        *is_float = true;
        *width = 64;
        return true;
    }
    if (strcmp(name, "F32") == 0) {
        *is_float = true;
        *width = 32;
        return true;
    }
    if (strcmp(name, "F80") == 0) {
        *is_float = true;
        *width = 80;
        return true;
    }

    return false;
}

bool type_name_is_subtype(const char *sub_name, const char *sup_name) {
    if (!sub_name || !sup_name) return false;
    if (strcmp(sub_name, sup_name) == 0) return true;

    const char *base = refinement_base_name(sub_name);
    if (base && type_name_is_subtype(base, sup_name))
        return true;

    const char *alias = type_alias_target_name(sub_name);
    if (alias && strcmp(alias, sub_name) != 0 &&
        type_name_is_subtype(alias, sup_name))
        return true;

    bool sub_int, sub_unsigned, sub_float;
    bool sup_int, sup_unsigned, sup_float;
    int sub_width, sup_width;
    if (numeric_type_info(sub_name, &sub_int, &sub_unsigned,
                          &sub_width, &sub_float) &&
        numeric_type_info(sup_name, &sup_int, &sup_unsigned,
                          &sup_width, &sup_float)) {
        if (sub_float || sup_float) {
            if (sub_float && sup_float)
                return sub_width <= sup_width || strcmp(sup_name, "Float") == 0;
            return false;
        }
        if (sub_int && sup_int) {
            if (strcmp(sup_name, "Int") == 0)
                return true;
            if (sub_unsigned == sup_unsigned)
                return sub_width <= sup_width;
        }
    }

    return false;
}

bool type_is_subtype(Type *sub, Type *sup) {
    if (!sub || !sup) return false;
    if (types_equal(sub, sup)) return true;
    return type_name_is_subtype(type_to_string(sub), type_to_string(sup));
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

    char *trimmed_name = type_trim_copy(name);
    if (!trimmed_name) return NULL;
    if (trimmed_name[0] == '\0') {
        free(trimmed_name);
        return NULL;
    }
    if (strcmp(trimmed_name, name) != 0) {
        Type *t = type_from_name(trimmed_name);
        free(trimmed_name);
        return t;
    }
    free(trimmed_name);

    const char *constraint_arrow = strstr(name, "=>");
    if (constraint_arrow) {
        Type *t = type_from_name(constraint_arrow + 2);
        if (t) return t;
    }

    size_t len = strlen(name);

    if (strcmp(name, "()") == 0 || strcmp(name, "Unit") == 0)
        return type_unit();

    Type *arrow_type = type_parse_arrow_chain(name);
    if (arrow_type) return arrow_type;

    Type *comma_tuple = type_parse_comma_tuple(name);
    if (comma_tuple) return comma_tuple;

    Type *type_app = type_parse_type_application(name);
    if (type_app) return type_app;

    if (len > 1 && name[0] == '*') {
        const char *inner_name = name + 1;
        Type *inner_type = type_from_name(inner_name);
        if (!inner_type) {
            inner_type = type_layout_ref(inner_name);
        }
        return type_ptr(inner_type);
    }

    if (len == 1 && name[0] >= 'a' && name[0] <= 'z') {
        return type_var(2000 + (name[0] - 'a'));
    }

    /* Parenthesized type application: (Maybe a) is grouping, not the
     * list/tuple spelling below. Wisp/desugar can preserve ADT applications
     * with these parens in lambda annotations. */
    if (len > 2 && name[0] == '(' && name[len - 1] == ')') {
        const char *inner_start = name + 1;
        size_t inner_len = len - 2;
        while (inner_len > 0 && (*inner_start == ' ' || *inner_start == '\t')) {
            inner_start++;
            inner_len--;
        }
        while (inner_len > 0 &&
               (inner_start[inner_len - 1] == ' ' ||
                inner_start[inner_len - 1] == '\t')) {
            inner_len--;
        }

        char *inner_name = strndup(inner_start, inner_len);
        char *trimmed_inner = type_trim_copy(inner_name);
        free(inner_name);
        Type *inner_arrow = type_parse_arrow_chain(trimmed_inner);
        free(trimmed_inner);
        if (inner_arrow) return inner_arrow;

        if (inner_len > 0 && inner_start[0] >= 'A' && inner_start[0] <= 'Z') {
            int depth = 0;
            bool has_top_level_space = false;
            for (size_t i = 0; i < inner_len; i++) {
                char c = inner_start[i];
                if (c == '(' || c == '[' || c == '{') depth++;
                else if (c == ')' || c == ']' || c == '}') depth--;
                else if ((c == ' ' || c == '\t') && depth == 0) {
                    has_top_level_space = true;
                    break;
                }
            }
            if (has_top_level_space) {
                char *inner_name = strndup(inner_start, inner_len);
                Type *inner_type = type_from_name(inner_name);
                free(inner_name);
                if (inner_type) return inner_type;
            }
        }
    }

    // Parenthesized product: (T1 T2 ...) or comma tuple spelling.
    // A single parenthesized type (T) is grouping, not a unary product.
    if (len > 1 && name[0] == '(' && name[len - 1] == ')') {
        char *inner_name = strndup(name + 1, len - 2);
        Type *t = make_type(TYPE_LIST);
        t->list_count = 0;
        t->list_types = NULL;

        bool has_top_level_comma = false;
        int scan_depth = 0;
        for (char *q = inner_name; *q; q++) {
            if (*q == '(' || *q == '[' || *q == '{') scan_depth++;
            else if (*q == ')' || *q == ']' || *q == '}') scan_depth--;
            else if (*q == ',' && scan_depth == 0) {
                has_top_level_comma = true;
                break;
            }
        }

        char *p = inner_name;
        while (*p) {
            while (*p == ' ' || *p == '\t' || (has_top_level_comma && *p == ',')) p++;
            if (!*p) break;

            char *start = p;
            int depth = 0;
            while (*p) {
                if (*p == '(' || *p == '[' || *p == '{') depth++;
                if (*p == ')' || *p == ']' || *p == '}') depth--;
                if (depth == 0) {
                    if (has_top_level_comma && *p == ',') break;
                    if (!has_top_level_comma && (*p == ' ' || *p == '\t')) break;
                }
                p++;
            }
            char *end = p;
            while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
            char *elem_str = strndup(start, end - start);
            Type *elem_t = type_from_name(elem_str);
            if (!elem_t) elem_t = type_unknown();
            free(elem_str);

            t->list_count++;
            t->list_types = realloc(t->list_types, t->list_count * sizeof(Type*));
            t->list_types[t->list_count - 1] = elem_t;
            if (has_top_level_comma && *p == ',') p++;
        }
        free(inner_name);

        if (t->list_count == 1) {
            Type *only = t->list_types[0];
            free(t->list_types);
            free(t);
            return only;
        }

        return t;
    }

    // Bracketed type syntax:
    //   [T] = abstract collection of T in type annotations.
    // Concrete/runtime arrays must use Arr :: T or Arr :: T :: N.
    if (len > 2 && name[0] == '[' && name[len - 1] == ']') {
        char *inner_name = strndup(name + 1, len - 2);
        char *trimmed_inner = type_trim_copy(inner_name);
        free(inner_name);

        Type *inner = type_from_name(trimmed_inner);
        free(trimmed_inner);

        Type *t = type_coll();
        t->element_type = inner ? inner : type_unknown();
        return t;
    }

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

    if (strncmp(name, "Coll :: ", 8) == 0) {
        const char *inner_name = name + 8;
        Type *inner_type = type_from_name(inner_name);
        Type *t = type_coll();
        t->element_type = inner_type ? inner_type : type_unknown();
        return t;
    }

    if (strncmp(name, "Arr :: ", 7) == 0) {
        char buf[256];
        strncpy(buf, name + 7, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char *delim = strstr(buf, " :: ");
        if (delim) {
            *delim = '\0';
            Type *elem_type = type_from_name(buf);
            int64_t size = (int64_t)strtoll(delim + 4, NULL, 10);
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
    if (strcmp(name, "Byte")    == 0) return type_byte();
    if (strcmp(name, "String")  == 0) return type_string();
    if (strcmp(name, "Bool")    == 0) return type_bool();
    if (strcmp(name, "Hex")     == 0) return type_hex();
    if (strcmp(name, "Bin")     == 0) return type_bin();
    if (strcmp(name, "Oct")     == 0) return type_oct();
    if (strcmp(name, "Keyword") == 0) return type_keyword();
    if (strcmp(name, "Ratio")   == 0) return type_ratio();
    if (strcmp(name, "List")    == 0) return type_list(NULL, 0);
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
    if (strcmp(name, "Path")    == 0) return type_path();
    if (strcmp(name, "Escape")  == 0) return type_escape();
    if (strcmp(name, "Unit")    == 0) return type_unit();
    if (strcmp(name, "Heap")    == 0) return type_arr_heap(NULL);

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
Type *type_byte   (void) { return make_type(TYPE_BYTE);    }
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
Type *type_path   (void) { return make_type(TYPE_PATH);    }
Type *type_escape (void) { return make_type(TYPE_ESCAPE);  }
Type *type_unit   (void) { return make_type(TYPE_UNIT);    }

Type *type_arr_heap(Type *element_type) {
    Type *t = make_type(TYPE_ARR);
    t->arr_element_type = element_type;
    t->arr_size    = -1;
    t->arr_is_fat  = false;
    t->arr_is_heap = true;
    return t;
}

Type *type_int_arbitrary(int width, bool is_signed) {
    Type *t = make_type(TYPE_INT_ARBITRARY);
    t->numeric_width  = width;
    t->numeric_signed = is_signed;
    return t;
}

Type *type_nil(void) { return make_type(TYPE_NIL); }

Type *type_app(const char *constructor, Type *arg) {
    Type *t = make_type(TYPE_APP);
    t->app_constructor = constructor ? strdup(constructor) : NULL;
    t->app_arg = arg;
    return t;
}

Type *type_optional(Type *inner) {
    Type *t = make_type(TYPE_OPTIONAL);
    t->element_type = inner;
    return t;
}

Type *type_list(Type **types, int count) {
    Type *t = make_type(TYPE_LIST);
    t->list_types = NULL;
    if (count > 0 && types) {
        t->list_types = malloc(count * sizeof(Type*));
        for (int i = 0; i < count; i++) {
            t->list_types[i] = types[i] ? type_clone(types[i]) : type_unknown();
        }
        if (count == 1)
            t->list_elem = types[0] ? type_clone(types[0]) : type_unknown();
    }
    t->list_count = count;
    return t;
}

Type *type_arr(Type *element_type, int64_t size) {
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

    if ((a->kind == TYPE_BYTE && b->kind == TYPE_CHAR) ||
        (a->kind == TYPE_CHAR && b->kind == TYPE_BYTE))
        return true;

    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case TYPE_VAR:
        return a->var_id == b->var_id;
    case TYPE_ARROW:
        return types_equal(a->arrow_param, b->arrow_param)
            && types_equal(a->arrow_ret,   b->arrow_ret);
    case TYPE_LIST:
        if (a->list_count != b->list_count) return false;
        for (int i = 0; i < a->list_count; i++) {
            if (!types_equal(a->list_types[i], b->list_types[i])) return false;
        }
        return true;
    case TYPE_OPTIONAL:
    case TYPE_PTR:
    case TYPE_COLL:
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
    Type *result = NULL;
    {
        char *trimmed = type_trim_copy(parts[nparts - 1]);
        result = type_from_name(trimmed);
        free(trimmed);
    }
    if (!result) result = type_unknown();
    for (int _i = nparts - 2; _i >= 0; _i--) {
        char *trimmed = type_trim_copy(parts[_i]);
        Type *param_t = type_from_name(trimmed);
        free(trimmed);
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
        case TYPE_BYTE:    return type_byte();
        case TYPE_STRING:  return type_string();
        case TYPE_SYMBOL:  return type_symbol();
        case TYPE_BOOL:    return type_bool();
        case TYPE_HEX:     return type_hex();
        case TYPE_BIN:     return type_bin();
        case TYPE_OCT:     return type_oct();
        case TYPE_KEYWORD: return type_keyword();
        case TYPE_RATIO:   return type_ratio();
        case TYPE_LIST: {
            Type *c = type_list(NULL, 0);
            c->list_count = t->list_count;
            if (t->list_count > 0) {
                c->list_types = malloc(t->list_count * sizeof(Type*));
                for (int i = 0; i < t->list_count; i++) {
                    c->list_types[i] = type_clone(t->list_types[i]);
                }
            }
            if (t->list_elem)
                c->list_elem = type_clone(t->list_elem);
            return c;
        }
        case TYPE_COLL: {
            Type *c = type_coll();
            c->element_type = type_clone(t->element_type);
            return c;
        }
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
            c->arr_is_fat  = t->arr_is_fat;
            c->arr_is_heap = t->arr_is_heap;
            return c;
        }
        case TYPE_PTR:          return type_ptr(type_clone(t->element_type));
        case TYPE_OPTIONAL:     return type_optional(type_clone(t->element_type));
        case TYPE_UNIT:         return type_unit();
        case TYPE_ESCAPE:       return type_escape();
        case TYPE_APP:          return type_app(t->app_constructor, type_clone(t->app_arg));
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
    if (t->kind == TYPE_LIST) {
        for (int i = 0; i < t->list_count; i++) {
            type_free(t->list_types[i]);
        }
        free(t->list_types);
        type_free(t->list_elem);
    }
    if (t->kind == TYPE_PTR || t->kind == TYPE_OPTIONAL || t->kind == TYPE_COLL) {
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
    if (t->kind == TYPE_APP) {
        free(t->app_constructor);
        type_free(t->app_arg);
    }
    if (t->kind == TYPE_VAR) {}
    free(t);
}

// Returns a pointer to a static or thread-local buffer — caller must not free.
const char *type_to_string(Type *t) {
    if (!t) return "?";

    static char bufs[32][512];
    static int buf_idx = 0;
    char *buf = bufs[buf_idx];
    buf_idx = (buf_idx + 1) % 32;

    switch (t->kind) {
    case TYPE_INT:     return "Int";
    case TYPE_FLOAT:   return "Float";
    case TYPE_CHAR:    return "Char";
    case TYPE_BYTE:    return "Byte";
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
    case TYPE_UNIT:    return "()";
    case TYPE_APP:
        if (t->app_constructor && t->app_arg) {
            snprintf(buf, 512, "%s %s", t->app_constructor, type_to_string(t->app_arg));
            return buf;
        }
        return t->app_constructor ? t->app_constructor : "?";
    case TYPE_F80:     return "F80";
    case TYPE_INT_ARBITRARY:
        snprintf(buf, 512, "%c%d", t->numeric_signed ? 'I' : 'U', t->numeric_width);
        return buf;
    case TYPE_PTR:
        snprintf(buf, 512, "Pointer :: %s", type_to_string(t->element_type));
        return buf;
    case TYPE_OPTIONAL:
        snprintf(buf, 512, "%s?", type_to_string(t->element_type));
        return buf;
    case TYPE_VAR:
        if (t->var_id >= 2000 && t->var_id < 2026) {
            snprintf(buf, 512, "%c", 'a' + (t->var_id - 2000));
            return buf;
        }
        snprintf(buf, 512, "%c", 'a' + (t->var_id % 26));
        return buf;
    case TYPE_ARROW:
        snprintf(buf, 512, "%s -> %s", type_to_string(t->arrow_param), type_to_string(t->arrow_ret));
        return buf;
        case TYPE_LIST: {
            if (t->list_count == 0) {
                snprintf(buf, 512, "()");
                return buf;
            }

            int offset = snprintf(buf, 512, "(");
            for (int i = 0; i < t->list_count; i++) {
                if (i > 0)
                    offset += snprintf(buf + offset, 512 - offset, ", ");
                offset += snprintf(buf + offset,
                                   512 - offset,
                                   "%s",
                                   type_to_string(t->list_types[i]));
            }
            snprintf(buf + offset, 512 - offset, ")");
            return buf;
        }
        case TYPE_COLL:
            if (t->element_type)
                snprintf(buf, 512, "Coll :: %s", type_to_string(t->element_type));
            else
                snprintf(buf, 512, "Coll");
            return buf;
        case TYPE_PATH: return "Path";
        case TYPE_ESCAPE: return "Escape";
        case TYPE_ARR:
        if (t->arr_is_heap) {
            if (t->arr_element_type)
                snprintf(buf, 512, "Heap :: %s", type_to_string(t->arr_element_type));
            else
                snprintf(buf, 512, "Heap");
            return buf;
        }
        if (t->arr_element_type && t->arr_size >= 0) {
            snprintf(buf, 512, "Arr :: %s :: %lld",
                     type_to_string(t->arr_element_type), (long long)t->arr_size);
        } else if (t->arr_element_type) {
            snprintf(buf, 512, "[%s]", type_to_string(t->arr_element_type));
        } else if (t->arr_size >= 0) {
            snprintf(buf, 512, "Arr :: ? :: %lld", (long long)t->arr_size);
        } else {
            snprintf(buf, 512, "Arr");
        }
        return buf;
    case TYPE_LAYOUT:
        return t->layout_name ? t->layout_name : "<layout>";
    case TYPE_FN: {
        if (t->param_count == 0) {
            snprintf(buf, 512, "Fn");
            return buf;
        }
        char sig[400] = {0};
        bool first_opt_seen = false;
        for (int i = 0; i < t->param_count; i++) {
            FnParam *p = &t->params[i];
            if (p->rest) {
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
        snprintf(buf, 512, "Fn (%s)", sig);
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

    // Byte / Hex
    if (literal_str[0] == '0' &&
        (literal_str[1] == 'x' || literal_str[1] == 'X')) {
        int hex_digits = 0;
        unsigned long long hex_value_acc = 0;

        for (const char *p = literal_str + 2; *p; p++) {
            unsigned char ch = (unsigned char)*p;
            int digit = -1;

            if (ch == '_')
                continue;

            if (ch >= '0' && ch <= '9')
                digit = ch - '0';
            else if (ch >= 'a' && ch <= 'f')
                digit = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F')
                digit = ch - 'A' + 10;
            else
                break;

            hex_digits++;
            hex_value_acc = (hex_value_acc << 4) | (unsigned long long)digit;
        }

        if (hex_digits > 0 && hex_digits <= 2 && hex_value_acc <= 0xff)
            return type_byte();

        return type_hex();
    }

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

static void append_type_node(char *buf, size_t cap, struct AST *node) {
    size_t off = strlen(buf);
    if (!node || off + 1 >= cap) return;

    switch (node->type) {
    case AST_SYMBOL:
        snprintf(buf + off, cap - off, "%s", node->symbol);
        break;
    case AST_NUMBER:
        snprintf(buf + off, cap - off, "%.0f", node->number);
        break;
    case AST_ARRAY:
        snprintf(buf + off, cap - off, "[");
        for (size_t i = 0; i < node->array.element_count; i++) {
            if (i > 0) strncat(buf, " ", cap - strlen(buf) - 1);
            append_type_node(buf, cap, node->array.elements[i]);
        }
        strncat(buf, "]", cap - strlen(buf) - 1);
        break;
    case AST_LIST:
        snprintf(buf + off, cap - off, "(");
        for (size_t i = 0; i < node->list.count; i++) {
            if (i > 0) strncat(buf, " ", cap - strlen(buf) - 1);
            append_type_node(buf, cap, node->list.items[i]);
        }
        strncat(buf, ")", cap - strlen(buf) - 1);
        break;
    default:
        snprintf(buf + off, cap - off, "?");
        break;
    }
}

static Type *parse_type_slice(struct AST **items, size_t start, size_t count) {
    char buf[512] = {0};
    for (size_t i = start; i < count; i++) {
        if (i > start) strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
        append_type_node(buf, sizeof(buf), items[i]);
    }
    Type *t = type_from_name(buf);
    return t ? t : type_unknown();
}

static Type *parse_type_node(struct AST *node) {
    if (!node) return NULL;
    if (node->type == AST_ARRAY) {
        char buf[512] = {0};
        append_type_node(buf, sizeof(buf), node);
        Type *t = type_from_name(buf);
        return t ? t : type_unknown();
    }

    if (node->type == AST_LIST) {
        if (node->list.count == 0)
            return type_unit();

        Type **elems = malloc(sizeof(Type*) * node->list.count);
        for (size_t j = 0; j < node->list.count; j++) {
            elems[j] = parse_type_node(node->list.items[j]);
        }
        Type *t = type_list(elems, node->list.count);
        free(elems);
        return t;
    }
    if (node->type == AST_SYMBOL) {
        Type *g = type_from_name(node->symbol);
        if (!g) g = type_unknown();
        return g;
    }
    return type_unknown();
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
                inner = parse_type_node(inner_node);
            } else if (inner_node->type == AST_SYMBOL) {
                inner = type_from_name(inner_node->symbol);
                if (!inner) inner = type_layout_ref(inner_node->symbol);
            }

            if (!inner) return NULL;
            return type_ptr(inner);
        }

        struct AST *type_node = items[i + 1];

        if (type_node->type == AST_SYMBOL) {
            const char *tn = type_node->symbol;
            if (strcmp(tn, "Arr") == 0) {
                Type *elem_type = NULL;
                int64_t size    = -1;
                /* Support both orderings:
                 *   Arr :: ElemType :: Size  (e.g. Arr :: U8 :: 256)
                 *   Arr :: Size :: ElemType  (e.g. Arr :: 256 :: U8) */
                if (i + 2 < count && items[i+2]->type == AST_SYMBOL &&
                    strcmp(items[i+2]->symbol, "::") == 0 && i + 3 < count) {
                    struct AST *a = items[i+3];
                    if (a->type == AST_NUMBER) {
                        /* Arr :: Size :: ElemType */
                        size = (int64_t)a->number;
                        if (i + 4 < count && items[i+4]->type == AST_SYMBOL &&
                            strcmp(items[i+4]->symbol, "::") == 0 &&
                            i + 5 < count && items[i+5]->type == AST_SYMBOL) {
                            elem_type = type_from_name(items[i+5]->symbol);
                        }
                    } else if (a->type == AST_SYMBOL) {
                        /* Arr :: ElemType :: Size */
                        elem_type = type_from_name(a->symbol);
                        if (i + 4 < count && items[i+4]->type == AST_SYMBOL &&
                            strcmp(items[i+4]->symbol, "::") == 0 &&
                            i + 5 < count && items[i+5]->type == AST_NUMBER) {
                            size = (int64_t)items[i+5]->number;
                        }
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
        }

        return parse_type_slice(items, i + 1, count);
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
