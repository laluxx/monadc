#include "pmatch.h"
#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static char *my_strdup(const char *s) {
    char *r = malloc(strlen(s) + 1);
    strcpy(r, s);
    return r;
}

/// Pattern Parser

static Token parser_peek(Parser *p) {
    return p->current;
}

static Token parser_advance(Parser *p) {
    Token t = p->current;
    p->current = lexer_next_token(p->lexer);
    return t;
}

static bool parser_at(Parser *p, TokenType t) {
    return p->current.type == t;
}

// Parse a single pattern element — used both at top level and inside [...]
static ASTPattern parse_single_pattern(Parser *p) {
    ASTPattern pat = {0};

    // Wildcard
    if (parser_at(p, TOK_SYMBOL) && strcmp(p->current.value, "_") == 0) {
        parser_advance(p);
        pat.kind = PAT_WILDCARD;
        return pat;
    }

    // Numeric literal (positive)
    if (parser_at(p, TOK_NUMBER)) {
        const char *val = p->current.value;
        bool is_float = (strchr(val, '.') || strchr(val, 'e') || strchr(val, 'E'));
        pat.lit_value = atof(val);
        pat.kind = is_float ? PAT_LITERAL_FLOAT : PAT_LITERAL_INT;
        parser_advance(p);
        return pat;
    }

    // Negative number literal: symbol "-" followed by number
    if (parser_at(p, TOK_SYMBOL) && strcmp(p->current.value, "-") == 0) {
        parser_advance(p);
        if (parser_at(p, TOK_NUMBER)) {
            const char *val = p->current.value;
            bool is_float = (strchr(val, '.') || strchr(val, 'e') || strchr(val, 'E'));
            pat.lit_value = -atof(val);
            pat.kind = is_float ? PAT_LITERAL_FLOAT : PAT_LITERAL_INT;
            parser_advance(p);
            return pat;
        }
        // not a number after -, treat "-" as variable name
        pat.kind = PAT_VAR;
        pat.var_name = my_strdup("-");
        return pat;
    }

    // List pattern [...]
    if (parser_at(p, TOK_LBRACKET)) {
        parser_advance(p); // consume '['

        // Empty list []
        if (parser_at(p, TOK_RBRACKET)) {
            parser_advance(p);
            pat.kind = PAT_LIST_EMPTY;
            return pat;
        }

        // Parse element patterns until ] or |
        ASTPattern *elems = NULL;
        int         count = 0;
        int         cap   = 0;

        while (!parser_at(p, TOK_RBRACKET) &&
               !parser_at(p, TOK_PIPE)     &&
               !parser_at(p, TOK_EOF)) {
            ASTPattern elem = parse_single_pattern(p);
            if (count >= cap) {
                cap = cap == 0 ? 4 : cap * 2;
                elems = realloc(elems, sizeof(ASTPattern) * cap);
            }
            elems[count++] = elem;
        }

        ASTPattern *tail = NULL;

        // Optional | tail
        if (parser_at(p, TOK_PIPE)) {
            parser_advance(p); // consume '|'
            tail = malloc(sizeof(ASTPattern));
            *tail = parse_single_pattern(p);
            // tail must be a var or wildcard
            if (tail->kind != PAT_VAR && tail->kind != PAT_WILDCARD) {
                fprintf(stderr, "pmatch: tail after | must be a variable or _\n");
                tail->kind = PAT_WILDCARD;
            }
        }

        if (!parser_at(p, TOK_RBRACKET)) {
            fprintf(stderr, "pmatch: expected ']' to close list pattern\n");
        } else {
            parser_advance(p); // consume ']'
        }

        pat.kind          = PAT_LIST;
        pat.elements      = elems;
        pat.element_count = count;
        pat.tail          = tail;
        return pat;
    }

    // Variable binding (lowercase symbol)
    if (parser_at(p, TOK_SYMBOL)) {
        pat.kind     = PAT_VAR;
        pat.var_name = my_strdup(p->current.value);
        parser_advance(p);
        return pat;
    }

    // Fallback: wildcard
    fprintf(stderr, "pmatch: unexpected token '%s' in pattern, treating as wildcard\n",
            p->current.value ? p->current.value : "?");
    parser_advance(p);
    pat.kind = PAT_WILDCARD;
    return pat;
}

ASTPattern parse_pattern(Parser *p) {
    return parse_single_pattern(p);
}

// Parse one full clause: pattern... -> expr
// For multi-param functions, reads `param_count` patterns then ->
static ASTPMatchClause parse_one_clause(Parser *p, int param_count) {
    ASTPMatchClause clause = {0};

    ASTPattern *patterns = malloc(sizeof(ASTPattern) * (param_count ? param_count : 1));
    for (int i = 0; i < param_count; i++)
        patterns[i] = parse_single_pattern(p);

    // Consume ->
    if (!parser_at(p, TOK_ARROW)) {
        fprintf(stderr, "pmatch: expected '->' after pattern\n");
    } else {
        parser_advance(p);
    }

    // Parse body expression using the existing reader's parse_expr
    // We expose this via parse() on a sub-expression — but since we
    // share the same Parser/Lexer we just call parse_expr directly.
    AST *body = parse_expr(p);

    clause.patterns      = patterns;
    clause.pattern_count = param_count;
    clause.body          = body;
    return clause;
}

AST *parse_pmatch_clauses(Parser *p, int param_count) {
    ASTPMatchClause *clauses = NULL;
    int              count   = 0;
    int              cap     = 0;

    // Keep parsing clauses until we hit ) or EOF or a keyword (metadata)
    while (!parser_at(p, TOK_RPAREN) &&
           !parser_at(p, TOK_EOF)    &&
           !parser_at(p, TOK_KEYWORD)) {
        ASTPMatchClause clause = parse_one_clause(p, param_count);
        if (count >= cap) {
            cap = cap == 0 ? 4 : cap * 2;
            clauses = realloc(clauses, sizeof(ASTPMatchClause) * cap);
        }
        clauses[count++] = clause;
    }

    return ast_new_pmatch(clauses, count);
}

/// Desugarer
//
// Transforms AST_PMATCH into a cond AST that codegen already knows how
// to handle. All generated AST nodes are heap-allocated and owned by
// the returned tree.

// Helper: make a symbol AST node
static AST *sym(const char *name) {
    return ast_new_symbol(name);
}

// Helper: make a list AST (a function call / special form)
static AST *make_list(AST **items, int count) {
    AST *list = ast_new_list();
    for (int i = 0; i < count; i++)
        ast_list_append(list, items[i]);
    return list;
}

// Helper: (= expr number)
static AST *make_eq_int(AST *expr, long long n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", n);
    AST *items[] = {sym("="), expr, ast_new_number((double)n, buf)};
    return make_list(items, 3);
}

// Helper: (= expr float)
static AST *make_eq_float(AST *expr, double f) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", f);
    AST *items[] = {sym("="), expr, ast_new_number(f, buf)};
    return make_list(items, 3);
}

// Helper: True literal
static AST *make_true(void) {
    return sym("True");
}

// Helper: (list-empty? param)
static AST *make_list_empty(const char *param_name) {
    AST *items[] = {sym("list-empty?"), sym(param_name)};
    return make_list(items, 2);
}

// Helper: (list-length param)
static AST *make_list_length(const char *param_name) {
    AST *items[] = {sym("list-length"), sym(param_name)};
    return make_list(items, 2);
}

// Helper: (= (list-length param) n)
static AST *make_length_eq(const char *param_name, int n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", n);
    AST *len   = make_list_length(param_name);
    AST *items[] = {sym("="), len, ast_new_number((double)n, buf)};
    return make_list(items, 3);
}

// Helper: (car param)
static AST *make_car(const char *param_name) {
    AST *items[] = {sym("car"), sym(param_name)};
    return make_list(items, 2);
}

// Helper: (cdr param)
static AST *make_cdr(const char *param_name) {
    AST *items[] = {sym("cdr"), sym(param_name)};
    return make_list(items, 2);
}

// Helper: (list-ref param i)
static AST *make_list_ref(const char *param_name, int idx) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", idx);
    AST *items[] = {sym("list-ref"), sym(param_name),
                    ast_new_number((double)idx, buf)};
    return make_list(items, 3);
}

// Helper: (and expr expr ...)
static AST *make_and(AST **exprs, int count) {
    if (count == 1) return exprs[0];
    AST *list = ast_new_list();
    ast_list_append(list, sym("and"));
    for (int i = 0; i < count; i++)
        ast_list_append(list, exprs[i]);
    return list;
}

// Substitute all occurrences of names[i] with exprs[i] in body.
// Returns a new AST with substitutions applied (deep clone with replacements).
static AST *ast_substitute(AST *node,
                            const char **names, AST **exprs, int count) {
    if (!node) return NULL;

    // Check if this node is a symbol that should be substituted
    if (node->type == AST_SYMBOL) {
        for (int i = 0; i < count; i++) {
            if (strcmp(node->symbol, names[i]) == 0)
                return ast_clone(exprs[i]);
        }
        return ast_clone(node);
    }

    // For lists, recurse into each item
    if (node->type == AST_LIST) {
        AST *result = ast_new_list();
        result->line   = node->line;
        result->column = node->column;
        for (size_t i = 0; i < node->list.count; i++)
            ast_list_append(result,
                ast_substitute(node->list.items[i], names, exprs, count));
        return result;
    }

    // For lambdas, substitute in body but not in params (they shadow)
    if (node->type == AST_LAMBDA) {
        // Build reduced substitution list excluding shadowed names
        const char **filtered_names = malloc(sizeof(char*) * count);
        AST       **filtered_exprs  = malloc(sizeof(AST*)  * count);
        int         filtered_count  = 0;
        for (int i = 0; i < count; i++) {
            bool shadowed = false;
            for (int j = 0; j < node->lambda.param_count; j++) {
                if (strcmp(node->lambda.params[j].name, names[i]) == 0) {
                    shadowed = true;
                    break;
                }
            }
            if (!shadowed) {
                filtered_names[filtered_count] = names[i];
                filtered_exprs[filtered_count] = exprs[i];
                filtered_count++;
            }
        }
        AST *cloned = ast_clone(node);
        if (filtered_count > 0) {
            // Substitute in each body expression
            for (int i = 0; i < cloned->lambda.body_count; i++) {
                AST *new_body = ast_substitute(cloned->lambda.body_exprs[i],
                                               filtered_names, filtered_exprs,
                                               filtered_count);
                ast_free(cloned->lambda.body_exprs[i]);
                cloned->lambda.body_exprs[i] = new_body;
            }
            cloned->lambda.body = cloned->lambda.body_exprs[cloned->lambda.body_count - 1];
        }
        free(filtered_names);
        free(filtered_exprs);
        return cloned;
    }

    // Everything else: just clone
    return ast_clone(node);
}

static AST *make_let(const char **names, AST **exprs, int count, AST *body) {
    // Use direct substitution instead of let-binding via immediately-invoked
    // lambda. This avoids boxing/unboxing issues with the closure ABI.
    return ast_substitute(body, names, exprs, count);
}

// Build guard + bindings for one pattern applied to one parameter.
// guard_parts: accumulated AND conditions (caller appends to this array)
// bind_names/bind_exprs: accumulated let bindings
// Returns false if the pattern is irrefutable (wildcard/var — no guard needed).
static void build_pattern_conditions(
        ASTPattern    *pat,
        const char    *param_name,
        const char    *elem_type_name,  // type of list elements, NULL if unknown
        AST         ***guard_parts,    int *guard_count,
        const char  ***bind_names,     AST ***bind_exprs,
        int           *bind_count,
        int           *bind_cap) {

    // Helper to append a guard condition
    #define PUSH_GUARD(g) do { \
        *guard_parts = realloc(*guard_parts, sizeof(AST*) * (*guard_count + 1)); \
        (*guard_parts)[(*guard_count)++] = (g); \
    } while(0)

    // Helper to append a let binding
    #define PUSH_BIND(n, e) do { \
        if (*bind_count >= *bind_cap) { \
            *bind_cap = *bind_cap == 0 ? 4 : *bind_cap * 2; \
            *bind_names = realloc(*bind_names, sizeof(char*)  * *bind_cap); \
            *bind_exprs = realloc(*bind_exprs, sizeof(AST *)  * *bind_cap); \
        } \
        (*bind_names)[*bind_count] = (n); \
        (*bind_exprs)[*bind_count] = (e); \
        (*bind_count)++; \
    } while(0)

    switch (pat->kind) {

    case PAT_WILDCARD:
        // No guard, no binding
        break;

    case PAT_VAR:
        // No guard, bind name = param
        // For scalar params, wrap in type cast if type is known
        if (elem_type_name) {
            AST *cast = ast_new_list();
            ast_list_append(cast, sym(elem_type_name));
            ast_list_append(cast, sym(param_name));
            PUSH_BIND(pat->var_name, cast);
        } else {
            PUSH_BIND(pat->var_name, sym(param_name));
        }
        break;

    case PAT_LITERAL_INT:
        PUSH_GUARD(make_eq_int(sym(param_name), (long long)pat->lit_value));
        break;

    case PAT_LITERAL_FLOAT:
        PUSH_GUARD(make_eq_float(sym(param_name), pat->lit_value));
        break;

    case PAT_LIST_EMPTY:
        PUSH_GUARD(make_list_empty(param_name));
        break;

    case PAT_LIST: {
        int n = pat->element_count;
        bool has_tail = (pat->tail != NULL);

        if (has_tail) {
            // [e0 e1 ... | xs] — list must have at least n elements
            // Guard: (>= (list-length param) n)
            if (n > 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d", n);
                AST *len     = make_list_length(param_name);
                AST *items[] = {sym(">="), len, ast_new_number((double)n, buf)};
                PUSH_GUARD(make_list(items, 3));
            } else {
                // [|xs] means any list — no length guard, just bind xs = param
            }
        } else {
            // [e0 e1 ...] — exact length
            PUSH_GUARD(make_length_eq(param_name, n));
        }

        // Per-element conditions and bindings
        for (int i = 0; i < n; i++) {
            ASTPattern *ep = &pat->elements[i];
            AST *elem_expr = make_list_ref(param_name, i);

            switch (ep->kind) {
            case PAT_WILDCARD:
                break;
            case PAT_VAR:
                // Wrap list-ref result in a type cast so codegen gets
                // a concrete typed value rather than a RuntimeValue*.
                if (elem_type_name) {
                    AST *cast = ast_new_list();
                    ast_list_append(cast, sym(elem_type_name));
                    ast_list_append(cast, elem_expr);
                    PUSH_BIND(ep->var_name, cast);
                } else {
                    PUSH_BIND(ep->var_name, elem_expr);
                }
                break;
            case PAT_LITERAL_INT:
                PUSH_GUARD(make_eq_int(elem_expr, (long long)ep->lit_value));
                break;
            case PAT_LITERAL_FLOAT:
                PUSH_GUARD(make_eq_float(elem_expr, ep->lit_value));
                break;
            default:
                break;
            }
        }

        // Tail binding: xs = (drop n param)
        if (has_tail && pat->tail->kind == PAT_VAR) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", n);
            AST *drop_args[] = {sym("drop"),
                                ast_new_number((double)n, buf),
                                sym(param_name)};
            PUSH_BIND(pat->tail->var_name, make_list(drop_args, 3));
        }
        break;
    }
    }

    #undef PUSH_GUARD
    #undef PUSH_BIND
}

AST *pmatch_desugar(AST *node, ASTParam *params, int param_count) {
    if (!node || node->type != AST_PMATCH) return node;

    // Build (cond [guard body] ... [else (undefined)]) then desugar
    // into if-chains via desugar_cond_ast. make_let already calls
    // desugar_let_ast so bindings are handled correctly.
    AST *cond_list = ast_new_list();
    ast_list_append(cond_list, sym("cond"));

    for (int i = 0; i < node->pmatch.clause_count; i++) {
        ASTPMatchClause *cl = &node->pmatch.clauses[i];

        AST    **guard_parts = NULL;
        int      guard_count = 0;
        const char **bind_names = NULL;
        AST        **bind_exprs = NULL;
        int          bind_count = 0;
        int          bind_cap   = 0;

        for (int j = 0; j < cl->pattern_count && j < param_count; j++) {
            // For rest params, elem_type is the element type of the list.
            // For scalar params, elem_type is the param's own type.
            const char *elem_type = NULL;
            if (params[j].is_rest) {
                // type_name on a rest param is the element type (e.g. "Int")
                elem_type = params[j].type_name;
            } else {
                elem_type = params[j].type_name;
            }
            build_pattern_conditions(
                &cl->patterns[j],
                params[j].name,
                elem_type,
                &guard_parts, &guard_count,
                &bind_names,  &bind_exprs,
                &bind_count,  &bind_cap);
        }

        AST *guard = (guard_count == 0)
            ? sym("else")
            : make_and(guard_parts, guard_count);

        AST *body = ast_clone(cl->body);
        if (bind_count > 0)
            body = make_let(bind_names, bind_exprs, bind_count, body);

        free(guard_parts);
        free(bind_names);
        free(bind_exprs);

        AST *clause = ast_new_list();
        ast_list_append(clause, guard);
        ast_list_append(clause, body);
        ast_list_append(cond_list, clause);
    }

    // Fallthrough: non-exhaustive pattern match
    // Only add if the last clause wasn't already a wildcard (else)
    AST *last_clause = cond_list->list.items[cond_list->list.count - 1];
    AST *last_guard  = last_clause->list.items[0];
    bool has_else = (last_guard->type == AST_SYMBOL &&
                     strcmp(last_guard->symbol, "else") == 0);
    if (!has_else) {
        AST *undef_call = ast_new_list();
        ast_list_append(undef_call, sym("undefined"));
        AST *else_clause = ast_new_list();
        ast_list_append(else_clause, sym("else"));
        ast_list_append(else_clause, undef_call);
        ast_list_append(cond_list, else_clause);
    }

    return desugar_cond_ast(cond_list);
}

void pattern_free(ASTPattern *p) {
    ast_pattern_free(p);
}
