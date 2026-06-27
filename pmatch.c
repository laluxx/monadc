#include "pmatch.h"
#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/// TODO [0/2]
// - [ ] First optimize the normal path, then
// - [ ] pmatch should always produce jump tables when appropriate

extern const char *layout_get_field_name(const char *layout_name, int index);

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
ASTPattern parse_single_pattern(Parser *p) {
    ASTPattern pat = {0};

    // Wildcard
    if (parser_at(p, TOK_SYMBOL) && strcmp(p->current.value, "_") == 0) {
        parser_advance(p);
        pat.kind = PAT_WILDCARD;
        return pat;
    }

    if (parser_at(p, TOK_SYMBOL) && strcmp(p->current.value, ",") == 0) {
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

    // Character literal — treat as integer (ASCII value)
    if (parser_at(p, TOK_CHAR)) {
        pat.kind      = PAT_LITERAL_INT;
        pat.lit_value = (double)(unsigned char)p->current.value[0];
        parser_advance(p);
        return pat;
    }

    // String literal
    if (parser_at(p, TOK_STRING)) {
        pat.kind = PAT_LITERAL_STRING;
        pat.var_name = my_strdup(p->current.value);
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

    // ADT constructor pattern (uppercase symbol = constructor name)

    // Handles both bare `Red` and destructuring `(Circle r)` / `(Rectangle w h)`
    if (parser_at(p, TOK_SYMBOL) &&
        p->current.value[0] >= 'A' && p->current.value[0] <= 'Z') {
        pat.kind     = PAT_CONSTRUCTOR;
        pat.var_name = my_strdup(p->current.value);
        parser_advance(p);
        return pat;
    }

    // [Constructor field1 field2 ...] — destructuring constructor pattern
    if (parser_at(p, TOK_LBRACKET) || parser_at(p, TOK_LPAREN)) {
        TokenType close_tok = parser_at(p, TOK_LBRACKET) ? TOK_RBRACKET : TOK_RPAREN;
        parser_advance(p); // consume '[' or '('

        // Peek: only treat as constructor pattern if first token inside is uppercase
        if (parser_at(p, TOK_SYMBOL) &&
            p->current.value[0] >= 'A' && p->current.value[0] <= 'Z') {
            pat.kind     = PAT_CONSTRUCTOR;
            pat.var_name = my_strdup(p->current.value);
            parser_advance(p);

            // Parse field patterns until ']' or ')'
            ASTPattern *fields = NULL;
            int         fcount = 0;
            int         fcap   = 0;
            while (!parser_at(p, close_tok) && !parser_at(p, TOK_EOF)) {
                ASTPattern fp = parse_single_pattern(p);
                if (fcount >= fcap) {
                    fcap = fcap == 0 ? 4 : fcap * 2;
                    fields = realloc(fields, sizeof(ASTPattern) * fcap);
                }
                fields[fcount++] = fp;
            }
            if (!parser_at(p, close_tok)) {
                fprintf(stderr, "pmatch: expected closing bracket/paren to close constructor pattern\n");
            } else {
                parser_advance(p); // consume closing token
            }
            pat.ctor_fields      = fields;
            pat.ctor_field_count = fcount;
            return pat;
        }
        /* Not a constructor pattern — fall through to existing list pattern
         * handling by re-entering the list pattern code. But we already
         * consumed '[', so handle as empty or non-constructor list pattern. */
        if (parser_at(p, close_tok)) {
            parser_advance(p);
            pat.kind = PAT_LIST_EMPTY;
            return pat;
        }

        /* Non-uppercase after '[' — treat as regular list pattern */
        ASTPattern *elems = NULL;
        int count = 0, cap = 0;
        ASTPattern *tail = NULL;

        while (!parser_at(p, close_tok) &&
               !parser_at(p, TOK_PIPE)     &&
               !(parser_at(p, TOK_SYMBOL) && p->current.value &&
                 strcmp(p->current.value, "|") == 0) &&
               !parser_at(p, TOK_EOF)) {
            if (parser_at(p, TOK_SYMBOL) && p->current.value &&
                strcmp(p->current.value, ",") == 0) {
                parser_advance(p);
                continue;
            }
            /* Detect fused token like "_|xs" or "x|xs" — symbol containing '|' */
            if (p->current.type == TOK_SYMBOL && p->current.value) {
                const char *pipe = strchr(p->current.value, '|');
                if (pipe) {
                    /* Split: part before '|' is an element pattern */
                    size_t before_len = (size_t)(pipe - p->current.value);
                    if (before_len > 0) {
                        char *before = my_strdup(p->current.value);
                        before[before_len] = '\0';
                        ASTPattern elem = {0};
                        if (strcmp(before, "_") == 0) {
                            elem.kind = PAT_WILDCARD;
                        } else {
                            elem.kind     = PAT_VAR;
                            elem.var_name = my_strdup(before);
                        }
                        free(before);
                        if (count >= cap) {
                            cap = cap == 0 ? 4 : cap * 2;
                            elems = realloc(elems, sizeof(ASTPattern) * cap);
                        }
                        elems[count++] = elem;
                    }
                    /* Part after '|' is the tail */
                    const char *tail_name = pipe + 1;
                    tail = calloc(1, sizeof(ASTPattern));
                    if (strcmp(tail_name, "_") == 0 || tail_name[0] == '\0') {
                        tail->kind = PAT_WILDCARD;
                    } else {
                        tail->kind     = PAT_VAR;
                        tail->var_name = my_strdup(tail_name);
                    }
                    parser_advance(p);
                    break; /* tail ends the element list */
                }
            }
            ASTPattern elem = parse_single_pattern(p);
            if (count >= cap) {
                cap = cap == 0 ? 4 : cap * 2;
                elems = realloc(elems, sizeof(ASTPattern) * cap);
            }
            elems[count++] = elem;
        }

        if (parser_at(p, TOK_PIPE) ||
            (parser_at(p, TOK_SYMBOL) && p->current.value && strcmp(p->current.value, "|") == 0)) {
            parser_advance(p);
            tail = calloc(1, sizeof(ASTPattern));
            *tail = parse_single_pattern(p);
            if (tail->kind != PAT_VAR && tail->kind != PAT_WILDCARD) {
                fprintf(stderr, "pmatch: tail after | must be a variable or _\n");
                tail->kind = PAT_WILDCARD;
            }
        }
        if (!parser_at(p, close_tok)) {
            fprintf(stderr, "pmatch: expected closing bracket/paren to close list pattern\n");
        } else {
            parser_advance(p);
        }
        pat.kind          = PAT_LIST;
        pat.elements      = elems;
        pat.element_count = count;
        pat.tail          = tail;
        return pat;
    }

    // Variable binding (lowercase symbol)
    if (parser_at(p, TOK_SYMBOL)) {
        Token sym_tok = p->current;
        char *vname = my_strdup(p->current.value);
        parser_advance(p);

        // If a bracket immediately follows (no space), it's an as-pattern! e.g. v[x y z]
        if (parser_at(p, TOK_LBRACKET) && p->current.column == sym_tok.column + (int)strlen(vname)) {
            pat = parse_single_pattern(p);
            // Attach the variable name to the list pattern so pmatch binds it
            if (pat.kind == PAT_LIST || pat.kind == PAT_LIST_EMPTY) {
                pat.var_name = vname;
            } else {
                free(vname);
            }
            return pat;
        }

        pat.kind     = PAT_VAR;
        pat.var_name = vname;
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

static bool is_at_pmatch_clause(Parser *p) {
    Lexer peek_lex = *p->lexer;
    Token peek_cur = p->current;
    /* Do not free peek_cur.value on the very first token since it belongs to p->current! */
    bool has_arrow = (peek_cur.type == TOK_ARROW);
    bool has_pipe  = (peek_cur.type == TOK_PIPE ||
                      (peek_cur.type == TOK_SYMBOL && peek_cur.value &&
                       strcmp(peek_cur.value, "|") == 0));
    int depth = 0;
            bool is_first = true;
            while (!has_arrow && !has_pipe &&
                   !(peek_cur.type == TOK_RPAREN && depth == 0) &&
                   peek_cur.type != TOK_EOF    &&
                   peek_cur.type != TOK_KEYWORD) {
        if (peek_cur.type == TOK_LPAREN || peek_cur.type == TOK_LBRACKET) depth++;
        if ((peek_cur.type == TOK_RPAREN || peek_cur.type == TOK_RBRACKET) && depth > 0) depth--;
        if (peek_cur.type == TOK_ARROW && depth == 0) { has_arrow = true; break; }
        if (peek_cur.type == TOK_PIPE  && depth == 0) { has_pipe = true; break; }
        if (peek_cur.type == TOK_SYMBOL && peek_cur.value &&
            strcmp(peek_cur.value, "|") == 0 && depth == 0) {
            has_pipe = true; break;
        }
        if (!is_first && peek_cur.value) free(peek_cur.value);
        is_first = false;
        peek_cur = lexer_next_token(&peek_lex);
    }
    if (!is_first && peek_cur.value) free(peek_cur.value);
    return has_arrow || has_pipe;
}

static AST *finish_expr_sequence(AST **exprs, int count, bool body_context) {
    AST *result;

    if (count == 0) {
        return body_context ? ast_new_symbol("undefined") : ast_new_symbol("True");
    }

    if (count == 1) {
        result = exprs[0];
        free(exprs);
        return result;
    }

    bool same_line = true;
    int first_line = exprs[0]->line;
    for (int i = 1; i < count; i++) {
        if (exprs[i]->line != first_line) {
            same_line = false;
            break;
        }
    }

    if (same_line) {
        result = ast_new_list();
        for (int i = 0; i < count; i++)
            ast_list_append(result, exprs[i]);
    } else if (body_context) {
        result = ast_new_list();
        ast_list_append(result, ast_new_symbol("begin"));
        for (int i = 0; i < count; i++)
            ast_list_append(result, exprs[i]);
    } else {
        result = ast_new_list();
        ast_list_append(result, ast_new_symbol("and"));
        for (int i = 0; i < count; i++)
            ast_list_append(result, exprs[i]);
    }

    free(exprs);
    return result;
}

static AST *parse_guard_expr(Parser *p) {
    AST **exprs = NULL;
    int count = 0;
    int cap = 0;

    while (!parser_at(p, TOK_ARROW) &&
           !parser_at(p, TOK_RPAREN) &&
           !parser_at(p, TOK_EOF) &&
           !parser_at(p, TOK_KEYWORD)) {
        AST *expr = parse_expr(p);
        if (count >= cap) {
            cap = cap == 0 ? 4 : cap * 2;
            exprs = realloc(exprs, sizeof(AST*) * cap);
        }
        exprs[count++] = expr;
    }

    return finish_expr_sequence(exprs, count, false);
}

static AST *parse_clause_body(Parser *p) {
    AST **body_exprs = NULL;
    int body_count = 0;
    int body_cap = 0;

    while (!parser_at(p, TOK_RPAREN) && !parser_at(p, TOK_EOF) && !parser_at(p, TOK_KEYWORD)) {
        if (body_count > 0 && is_at_pmatch_clause(p)) break;

        /* Skip #line directive nodes */
        if (p->current.type == TOK_LPAREN) {
            Lexer saved_lex = *p->lexer;
            Token saved_cur = p->current;
            Token peek = lexer_next_token(p->lexer);
            if (peek.type == TOK_SYMBOL && peek.value && strcmp(peek.value, "#line") == 0) {
                free(peek.value);
                while (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF) {
                    if (p->current.value) free(p->current.value);
                    p->current = lexer_next_token(p->lexer);
                }
                if (p->current.type == TOK_RPAREN) {
                    if (p->current.value) free(p->current.value);
                    p->current = lexer_next_token(p->lexer);
                }
                continue;
            } else {
                free(peek.value);
                *p->lexer = saved_lex;
                p->current = saved_cur;
            }
        }

        AST *expr = parse_expr(p);
        if (body_count >= body_cap) {
            body_cap = body_cap == 0 ? 4 : body_cap * 2;
            body_exprs = realloc(body_exprs, sizeof(AST*) * body_cap);
        }
        body_exprs[body_count++] = expr;
    }

    return finish_expr_sequence(body_exprs, body_count, true);
}

// Parse one full clause: pattern... -> expr
// Reads patterns dynamically until it hits an arrow or guard
static ASTPMatchClause parse_one_clause(Parser *p, int param_count) {
    ASTPMatchClause clause = {0};
    (void)param_count;

    ASTPattern *patterns = NULL;
    int act_count = 0;
    int cap = 0;

    if (parser_at(p, TOK_PIPE) ||
        (parser_at(p, TOK_SYMBOL) && p->current.value && strcmp(p->current.value, "|") == 0)) {
        parser_advance(p);
    }

    while (!parser_at(p, TOK_ARROW) && !parser_at(p, TOK_PIPE) &&
           !(parser_at(p, TOK_SYMBOL) && p->current.value && strcmp(p->current.value, "|") == 0) &&
           !parser_at(p, TOK_EOF)) {
        if (act_count >= cap) {
            cap = cap == 0 ? 4 : cap * 2;
            patterns = realloc(patterns, sizeof(ASTPattern) * cap);
        }
        patterns[act_count++] = parse_single_pattern(p);
    }

    // Check for guards: '|' means guarded clause, no '->' expected at top level
    if (parser_at(p, TOK_PIPE) ||
        (parser_at(p, TOK_SYMBOL) && p->current.value &&
         strcmp(p->current.value, "|") == 0)) {

        AST **guard_conds  = NULL;
        AST **guard_bodies = NULL;
        int   guard_count  = 0;
        int   guard_cap    = 0;

        while (parser_at(p, TOK_PIPE) ||
               (parser_at(p, TOK_SYMBOL) && p->current.value &&
                strcmp(p->current.value, "|") == 0)) {
            parser_advance(p); // consume '|'

            AST *cond = parse_guard_expr(p);

            if (!parser_at(p, TOK_ARROW)) {
                fprintf(stderr, "pmatch: expected '->' after guard condition, got type=%d value='%s'\n",
                        p->current.type,
                        p->current.value ? p->current.value : "NULL");
            } else {
                parser_advance(p);
            }

            AST *body = parse_clause_body(p);

            if (guard_count >= guard_cap) {
                guard_cap = guard_cap == 0 ? 4 : guard_cap * 2;
                guard_conds  = realloc(guard_conds,  sizeof(AST*) * guard_cap);
                guard_bodies = realloc(guard_bodies, sizeof(AST*) * guard_cap);
            }
            guard_conds[guard_count]  = cond;
            guard_bodies[guard_count] = body;
            guard_count++;
        }

        clause.patterns      = patterns;
        clause.pattern_count = act_count;
        clause.body          = NULL;
        clause.guard_conds   = guard_conds;
        clause.guard_bodies  = guard_bodies;
        clause.guard_count   = guard_count;
        return clause;
    }

    // No guards — consume -> and parse single body
    if (!parser_at(p, TOK_ARROW)) {
        fprintf(stderr, "pmatch: expected '->' after pattern, got token type=%d value='%s'\n",
                p->current.type,
                p->current.value ? p->current.value : "NULL");
    } else {
        parser_advance(p);
    }

    AST *body = parse_clause_body(p);

    clause.patterns      = patterns;
    clause.pattern_count = act_count;
    clause.body          = body;
    clause.guard_conds   = NULL;
    clause.guard_bodies  = NULL;
    clause.guard_count   = 0;
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

        if (!is_at_pmatch_clause(p)) {
            break;
        }

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

// Helper: (list-length param)
static AST *make_list_length(const char *param_name) {
    AST *items[] = {sym("list-length"), sym(param_name)};
    return make_list(items, 2);
}

// Helper: (count param)
static AST *make_count(const char *param_name) {
    AST *items[] = {sym("count"), sym(param_name)};
    return make_list(items, 2);
}

// Helper: (rt_coll_wrap coll_param elem)
static AST *make_coll_wrap(const char *coll_param, AST *elem) {
    AST *items[] = {sym("rt_coll_wrap"), sym(coll_param), elem};
    return make_list(items, 3);
}

// Helper: (rt_coll_is_empty param) -- O(1), safe for lazy/infinite lists
static AST *make_coll_is_empty(const char *param_name) {
    AST *items[] = {sym("rt_coll_is_empty"), sym(param_name)};
    AST *call = make_list(items, 2);
    /* wrap in (not ...) -- rt_coll_is_empty returns 1 when empty,
     * but PAT_LIST_EMPTY guard needs truthy-when-empty */
    /* Actually return directly -- caller uses it as a truthy guard */
    return call;
}

// Helper: collection has at least n elements.
// This avoids generating user-level overloaded >= in compiler-created
// pattern guards. For n == 1, this is just not empty. For n > 1,
// drop n - 1 elements and test whether the result is non-empty.
static AST *make_coll_has_at_least(const char *param_name, int n) {
    if (n <= 0) {
        return make_true();
    }

    AST *probe;
    if (n == 1) {
        probe = sym(param_name);
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", n - 1);
        AST *items[] = {
            sym("rt_coll_drop"),
            sym(param_name),
            ast_new_number((double)(n - 1), buf)
        };
        probe = make_list(items, 3);
    }

    AST *empty_items[] = {
        sym("rt_coll_is_empty"),
        probe
    };
    AST *is_empty = make_list(empty_items, 2);

    AST *if_items[] = {
        sym("if"),
        is_empty,
        sym("False"),
        sym("True")
    };
    return make_list(if_items, 4);
}

static bool pmatch_type_name_is_collection(const char *tn) {
    if (!tn) return false;

    while (*tn == ' ' || *tn == '\t' || *tn == '\n' || *tn == '\r') {
        tn++;
    }

    while (*tn == '(') {
        tn++;
        while (*tn == ' ' || *tn == '\t' || *tn == '\n' || *tn == '\r') {
            tn++;
        }
    }

    if (*tn == '[') return true;

    if (strncmp(tn, "Coll", 4) == 0 &&
        (tn[4] == '\0' || tn[4] == ' ' || tn[4] == '\t' ||
         tn[4] == ':'  || tn[4] == ')')) {
        return true;
    }

    if (strncmp(tn, "Arr", 3) == 0 &&
        (tn[3] == '\0' || tn[3] == ' ' || tn[3] == '\t' ||
         tn[3] == ':'  || tn[3] == ')')) {
        return true;
    }

    if (strncmp(tn, "List", 4) == 0 &&
        (tn[4] == '\0' || tn[4] == ' ' || tn[4] == '\t' ||
         tn[4] == ':'  || tn[4] == ')')) {
        return true;
    }

    return false;
}

static const char *pmatch_pick_collection_param(ASTPMatchClause *cl,
                                                ASTParam *params,
                                                int param_count) {
    if (cl && params) {
        int n = cl->pattern_count < param_count ? cl->pattern_count : param_count;
        for (int i = 0; i < n; i++) {
            if ((cl->patterns[i].kind == PAT_LIST ||
                 cl->patterns[i].kind == PAT_LIST_EMPTY) &&
                params[i].name) {
                return params[i].name;
            }
        }
    }

    if (params) {
        for (int i = 0; i < param_count; i++) {
            if (params[i].name &&
                pmatch_type_name_is_collection(params[i].type_name)) {
                return params[i].name;
            }
        }
    }

    return NULL;
}


// Helper: (= (count param) n)
static AST *make_count_eq(const char *param_name, int n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", n);
    AST *cnt = make_count(param_name);
    AST *items[] = {sym("="), cnt, ast_new_number((double)n, buf)};
    return make_list(items, 3);
}

// Helper: element access for collection patterns.
static AST *make_index_access(const char *param_name, int idx) {
    AST *coll = sym(param_name);

    if (idx > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", idx);
        AST *drop_items[] = {
            sym("rt_coll_drop"),
            coll,
            ast_new_number((double)idx, buf)
        };
        coll = make_list(drop_items, 3);
    }

    AST *items[] = {sym("head"), coll};
    return make_list(items, 2);
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
        if (!node->symbol) return ast_clone(node);
        for (int i = 0; i < count; i++) {
            if (!names[i]) continue;
            if (strcmp(node->symbol, names[i]) == 0)
                return ast_clone(exprs[i]);

            size_t nlen = strlen(names[i]);
            if (strncmp(node->symbol, names[i], nlen) == 0 && node->symbol[nlen] == '.') {
                if (exprs[i]->type == AST_SYMBOL) {
                    char new_sym[512];
                    snprintf(new_sym, sizeof(new_sym), "%s%s", exprs[i]->symbol, node->symbol + nlen);
                    AST *ns = ast_new_symbol(new_sym);
                    ns->line = node->line;
                    ns->column = node->column;
                    ns->end_column = node->end_column;
                    return ns;
                } else {
                    AST *dot_node = ast_new_list();
                    ast_list_append(dot_node, ast_new_symbol("dot"));
                    ast_list_append(dot_node, ast_clone(exprs[i]));
                    ast_list_append(dot_node, ast_new_symbol(node->symbol + nlen + 1));
                    dot_node->line = node->line;
                    dot_node->column = node->column;
                    dot_node->end_column = node->end_column;
                    return dot_node;
                }
            }
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

    // For arrays, recurse into each element — pure substitution only,
    // no rt_coll_wrap emission here (that happens in pmatch_desugar where
    // the collection param name is known).
    if (node->type == AST_ARRAY) {
        AST *cloned = ast_clone(node);
        for (size_t i = 0; i < cloned->array.element_count; i++) {
            AST *new_elem = ast_substitute(cloned->array.elements[i], names, exprs, count);
            ast_free(cloned->array.elements[i]);
            cloned->array.elements[i] = new_elem;
        }
        return cloned;
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
                if (node->lambda.params[j].name && names[i] && strcmp(node->lambda.params[j].name, names[i]) == 0) {
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
    /* First, substitute earlier bindings into later binding expressions
     * so that chained bindings like:
     *   __nested_Point_0 = (__adt_field __p_0 0)
     *   x1               = (__adt_field __nested_Point_0 0)
     * correctly resolve to:
     *   x1               = (__adt_field (__adt_field __p_0 0) 0)
     * before substituting into the body.                              */
    AST **resolved = malloc(sizeof(AST*) * count);
    for (int i = 0; i < count; i++) {
        /* Substitute all previous bindings into this expression */
        resolved[i] = ast_substitute(exprs[i],
                                     (const char **)names, resolved,
                                     i /* only previous bindings */);
    }
    /* Now substitute all resolved bindings into the body */
    AST *result = ast_substitute(body, names, resolved, count);
    for (int i = 0; i < count; i++)
        ast_free(resolved[i]);
    free(resolved);
    return result;
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
        // No guard, just bind name = param directly.
        // Never wrap in a type cast — the param already has the correct
        // type from the function signature. Casting causes bugs e.g.
        // (String x) on a String param goes through sprintf instead of
        // returning the string itself.
        PUSH_BIND(pat->var_name, sym(param_name));
        break;

    case PAT_LITERAL_INT:
        PUSH_GUARD(make_eq_int(sym(param_name), (long long)pat->lit_value));
        break;

    case PAT_LITERAL_FLOAT:
        PUSH_GUARD(make_eq_float(sym(param_name), pat->lit_value));
        break;

    case PAT_LITERAL_STRING: {
        AST *items[] = {sym("="), sym(param_name), ast_new_string(pat->var_name)};
        PUSH_GUARD(make_list(items, 3));
        break;
    }

    case PAT_LIST_EMPTY:
        if (pat->var_name) {
            PUSH_BIND(pat->var_name, sym(param_name));
        }
        PUSH_GUARD(make_coll_is_empty(param_name));
        break;

    case PAT_CONSTRUCTOR: {
        /* Guard: (= param.__tag __adt_tag_CtorName)
         * Use the fused string form so codegen_dot_chain handles it
         * exactly as it did before — it parses the dot-chain itself. */
        char tag_sym[256];
        snprintf(tag_sym, sizeof(tag_sym), "__adt_tag_%s", pat->var_name);
        char dot_sym[256];
        snprintf(dot_sym, sizeof(dot_sym), "%s.__tag", param_name);
        AST *tag_items[] = {sym("="), sym(dot_sym), sym(tag_sym)};
        PUSH_GUARD(make_list(tag_items, 3));

        /* For each field sub-pattern, extract via __adt_field and bind/guard */
        for (int fi = 0; fi < pat->ctor_field_count; fi++) {
            ASTPattern *fp = &pat->ctor_fields[fi];

            /* Use typed accessor __field_CtorName_fi instead of generic __adt_field */
            char acc_sym[256];
            snprintf(acc_sym, sizeof(acc_sym), "__field_%s_%d", pat->var_name, fi);
            AST *field_call = ast_new_list();
            ast_list_append(field_call, sym(acc_sym));
            ast_list_append(field_call, sym(param_name));

            switch (fp->kind) {
            case PAT_WILDCARD:
                break;

            case PAT_VAR:
                PUSH_BIND(fp->var_name, field_call);
                break;

            case PAT_LITERAL_INT:
                PUSH_GUARD(make_eq_int(field_call, (long long)fp->lit_value));
                break;

            case PAT_LITERAL_FLOAT:
                PUSH_GUARD(make_eq_float(field_call, fp->lit_value));
                break;

            case PAT_LITERAL_STRING: {
                AST *items[] = {sym("="), field_call, ast_new_string(fp->var_name)};
                PUSH_GUARD(make_list(items, 3));
                break;
            }

            case PAT_CONSTRUCTOR: {
                /* Nested: [Triangle [Point x1 y1] [Point x2 y2] ...]
                 *
                 * Step 1 — Bind: __nested_CtorName_fi = (__adt_field param fi)
                 * This lets ast_substitute replace __nested_CtorName_fi with
                 * the field expression everywhere in the body.
                 *
                 * Step 2 — Guard: check the nested field's tag via dot access
                 *   (= __nested_CtorName_fi.__tag __adt_tag_NestedCtor)
                 *
                 * Step 3 — Bind nested fields: x1 = (__adt_field __nested_... 0)
                 * We emit these as VAR bindings referencing the synthetic name.
                 * ast_substitute will chain-substitute them correctly.            */

                /* Step 1: bind synthetic name = field_call */
                char *nested_param = malloc(256);
                snprintf(nested_param, 256, "__nested_%s_%d", fp->var_name, fi);
                PUSH_BIND(nested_param, ast_clone(field_call));

                /* Step 2: tag guard using nested_param.__tag */
                char nested_tag_sym[256];
                snprintf(nested_tag_sym, sizeof(nested_tag_sym), "__adt_tag_%s", fp->var_name);
                char ndot_sym[256];
                snprintf(ndot_sym, sizeof(ndot_sym), "%s.__tag", nested_param);
                AST *ntag_items[] = {sym("="), sym(ndot_sym), sym(nested_tag_sym)};
                PUSH_GUARD(make_list(ntag_items, 3));

                /* Step 3: bind each nested field var to (__adt_field nested_param nfi)
                 * Do NOT recurse into build_pattern_conditions — that would emit
                 * a redundant tag guard using nested_param as a symbol, which is
                 * not yet a real variable at guard evaluation time.              */
                for (int nfi = 0; nfi < fp->ctor_field_count; nfi++) {
                    ASTPattern *nfp = &fp->ctor_fields[nfi];
                    char nfi_buf[16];
                    snprintf(nfi_buf, sizeof(nfi_buf), "%d", nfi);
                    char nacc_sym[256];
                    snprintf(nacc_sym, sizeof(nacc_sym), "__field_%s_%d",
                             fp->var_name, nfi);
                    AST *nfield_call = ast_new_list();
                    ast_list_append(nfield_call, sym(nacc_sym));
                    ast_list_append(nfield_call, sym(nested_param));
                    switch (nfp->kind) {
                    case PAT_WILDCARD:
                        break;
                    case PAT_VAR:
                        PUSH_BIND(nfp->var_name, nfield_call);
                        break;
                    case PAT_LITERAL_INT:
                        PUSH_GUARD(make_eq_int(ast_clone(nfield_call),
                                               (long long)nfp->lit_value));
                        break;
                    case PAT_LITERAL_FLOAT:
                        PUSH_GUARD(make_eq_float(ast_clone(nfield_call),
                                                 nfp->lit_value));
                        break;
                    case PAT_LITERAL_STRING: {
                        AST *items[] = {sym("="), ast_clone(nfield_call), ast_new_string(nfp->var_name)};
                        PUSH_GUARD(make_list(items, 3));
                        break;
                    }
                    default:
                        break;
                    }
                }
                break;
            }

            default:
                break;
            }
        }
        break;
    }

    case PAT_LIST: {
        if (pat->var_name) {
            PUSH_BIND(pat->var_name, sym(param_name));
        }
        int n = pat->element_count;
        bool has_tail = (pat->tail != NULL);

        /* Type-aware destructuring: if we know the exact ADT/Layout type,
         * destructure it as a zero-overhead struct instead of a dynamic list. */
        bool is_layout = false;
        if (elem_type_name) {
            const char *f0 = layout_get_field_name(elem_type_name, 0);
            if (f0 != NULL) is_layout = true;
        }

        if (is_layout) {
            for (int i = 0; i < n; i++) {
                ASTPattern *ep = &pat->elements[i];
                const char *fname = layout_get_field_name(elem_type_name, i);
                if (!fname) break; /* Ignore extra elements safely */

                char acc_sym[256];
                snprintf(acc_sym, sizeof(acc_sym), "%s.%s", param_name, fname);
                AST *elem_expr = sym(acc_sym);

                switch (ep->kind) {
                case PAT_WILDCARD:
                    break;
                case PAT_VAR:
                    PUSH_BIND(ep->var_name, elem_expr);
                    break;
                case PAT_LITERAL_INT:
                    PUSH_GUARD(make_eq_int(elem_expr, (long long)ep->lit_value));
                    break;
                case PAT_LITERAL_FLOAT:
                    PUSH_GUARD(make_eq_float(elem_expr, ep->lit_value));
                    break;
                case PAT_LITERAL_STRING: {
                    AST *items[] = {sym("="), elem_expr, ast_new_string(ep->var_name)};
                    PUSH_GUARD(make_list(items, 3));
                    break;
                }
                default:
                    break;
                }
            }
            break;
        }

        if (has_tail) {
            if (n > 0) {
                PUSH_GUARD(make_coll_has_at_least(param_name, n));
            }
        } else {
            PUSH_GUARD(make_count_eq(param_name, n));
        }

        // Per-element conditions and bindings
        for (int i = 0; i < n; i++) {
            ASTPattern *ep = &pat->elements[i];
            AST *elem_expr = make_index_access(param_name, i); // Unified

            switch (ep->kind) {
            case PAT_WILDCARD:
                break;
            case PAT_VAR:
                /* x is always a raw element — never pre-wrap.
                 * The ++ operator handles wrapping at the call site. */
                PUSH_BIND(ep->var_name, elem_expr);
                break;
            case PAT_LITERAL_INT:
                PUSH_GUARD(make_eq_int(elem_expr, (long long)ep->lit_value));
                break;
            case PAT_LITERAL_FLOAT:
                PUSH_GUARD(make_eq_float(elem_expr, ep->lit_value));
                break;
            case PAT_LITERAL_STRING: {
                AST *items[] = {sym("="), elem_expr, ast_new_string(ep->var_name)};
                PUSH_GUARD(make_list(items, 3));
                break;
            }
            case PAT_LIST: {
                int nn = ep->element_count;
                if (!ep->tail) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%d", nn);
                    AST *cnt_items[] = {sym("count"), ast_clone(elem_expr)};
                    AST *cnt = make_list(cnt_items, 2);
                    AST *eq_items[] = {sym("="), cnt,
                                       ast_new_number((double)nn, buf)};
                    PUSH_GUARD(make_list(eq_items, 3));
                }

                for (int ni = 0; ni < nn; ni++) {
                    ASTPattern *np = &ep->elements[ni];
                    char idx_buf[32];
                    snprintf(idx_buf, sizeof(idx_buf), "%d", ni);
                    AST *idx_items[] = {ast_clone(elem_expr),
                                        ast_new_number((double)ni, idx_buf)};
                    AST *sub_expr = make_list(idx_items, 2);

                    switch (np->kind) {
                    case PAT_WILDCARD:
                        ast_free(sub_expr);
                        break;
                    case PAT_VAR:
                        PUSH_BIND(np->var_name, sub_expr);
                        break;
                    case PAT_LITERAL_INT:
                        PUSH_GUARD(make_eq_int(sub_expr, (long long)np->lit_value));
                        break;
                    case PAT_LITERAL_FLOAT:
                        PUSH_GUARD(make_eq_float(sub_expr, np->lit_value));
                        break;
                    case PAT_LITERAL_STRING: {
                        AST *items[] = {sym("="), sub_expr,
                                        ast_new_string(np->var_name)};
                        PUSH_GUARD(make_list(items, 3));
                        break;
                    }
                    default:
                        ast_free(sub_expr);
                        break;
                    }
                }

                if (ep->tail && ep->tail->kind == PAT_VAR) {
                    int drop_n = nn;
                    char drop_buf[32];
                    snprintf(drop_buf, sizeof(drop_buf), "%d", drop_n);
                    AST *drop_items[] = {sym("rt_coll_drop"),
                                         ast_clone(elem_expr),
                                         ast_new_number((double)drop_n, drop_buf)};
                    PUSH_BIND(ep->tail->var_name, make_list(drop_items, 3));
                }
                ast_free(elem_expr);
                break;
            }
            default:
                ast_free(elem_expr);
                break;
            }
        }

        // Tail binding: xs = (drop n param)
        // Always use the C runtime directly — never the user-defined drop —
        // to avoid infinite recursion when drop itself uses [x|xs] patterns.
        // rt_coll_drop signature: (RuntimeValue* coll, int64_t n)
        // n is the number of elements before the tail — drop exactly n.
        if (has_tail && pat->tail->kind == PAT_VAR) {
            int drop_n = n;
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", drop_n);
            AST *drop_args[] = {sym("rt_coll_drop"),
                                sym(param_name),
                                ast_new_number((double)drop_n, buf)};
            PUSH_BIND(pat->tail->var_name, make_list(drop_args, 3));
        }
        break;
    }
    }

    #undef PUSH_GUARD
    #undef PUSH_BIND
}

// Rewrite single-element array literals [x] -> (rt_coll_wrap coll_param x)
// in a body AST, now that we know the collection param name.
static AST *rewrite_array_literals(AST *node, const char *coll_param) {
    if (!node) return node;
    if (node->type == AST_ARRAY) {
        if (node->array.element_count == 1) {
            // [x] -> (rt_coll_wrap coll_param x)
            // Do NOT recurse into the element — it is a plain value, not
            // a nested array literal. Recursing would double-wrap it.
            AST *elem = node->array.elements[0];
            node->array.elements[0] = NULL;
            AST *call = ast_new_list();
            ast_list_append(call, ast_new_symbol("rt_coll_wrap"));
            ast_list_append(call, ast_new_symbol(coll_param));
            ast_list_append(call, elem);
            ast_free(node);
            return call;
        }
        // zero-element array [] in body position is already handled by
        // the rt_coll_empty branch above; if we see it here, leave it alone.
        return node;
    }
    if (node->type == AST_LIST) {
        for (size_t i = 0; i < node->list.count; i++) {
            node->list.items[i] = rewrite_array_literals(node->list.items[i], coll_param);
        }
        return node;
    }
    return node;
}

// Propagate PAT_VAR names from the first pmatch clause back into params.
// When a signature uses type-only anonymous params (is_anon=true, name=__p_0),
// and the first clause binds them with user names (fd, buf, len),
// rename the param in-place so the rest of the pipeline sees the real name.
void pmatch_rename_anon_params(AST *pm, ASTParam *params, int param_count) {
    if (!pm || pm->type != AST_PMATCH || pm->pmatch.clause_count == 0) return;
    ASTPMatchClause *cl = &pm->pmatch.clauses[0];
    int n = cl->pattern_count < param_count ? cl->pattern_count : param_count;
    for (int i = 0; i < n; i++) {
        if (params[i].is_anon && cl->patterns[i].kind == PAT_VAR && cl->patterns[i].var_name) {
            free(params[i].name);
            params[i].name = my_strdup(cl->patterns[i].var_name);
            params[i].is_anon = false;
        }
    }
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
            const char *elem_type = params[j].type_name;

            build_pattern_conditions(
                &cl->patterns[j],
                params[j].name,
                elem_type,
                &guard_parts, &guard_count,
                &bind_names,  &bind_exprs,
                &bind_count,  &bind_cap);
        }

        if (cl->guard_count > 0) {
            // Guarded clause: pattern conditions are shared across all guards.
            // Build the pattern guard once, then AND it with each guard cond.
            AST *pattern_guard = (guard_count == 0)
                ? NULL
                : make_and(guard_parts, guard_count);

            for (int gi = 0; gi < cl->guard_count; gi++) {
                AST *gcond = ast_clone(cl->guard_conds[gi]);
                AST *gbody = ast_clone(cl->guard_bodies[gi]);

                if (bind_count > 0) {
                    gcond = make_let(bind_names, bind_exprs, bind_count, gcond);
                    gbody = make_let(bind_names, bind_exprs, bind_count, gbody);
                }

                bool is_otherwise = (gcond->type == AST_SYMBOL &&
                                     strcmp(gcond->symbol, "otherwise") == 0);
                AST *combined;
                if (is_otherwise) {
                    ast_free(gcond);
                    combined = pattern_guard ? ast_clone(pattern_guard) : sym("else");
                } else if (pattern_guard) {
                    AST *parts[2] = { ast_clone(pattern_guard), gcond };
                    combined = make_and(parts, 2);
                } else {
                    combined = gcond;
                }

                /* Polymorphic empty collection in guard body: [] -> (rt_coll_empty __p_N) */
                if ((gbody->type == AST_LIST  && gbody->list.count == 0) ||
                    (gbody->type == AST_ARRAY && gbody->array.element_count == 0)) {
                    ast_free(gbody);
                    const char *coll_param =
                        pmatch_pick_collection_param(cl, params, param_count);
                    if (coll_param) {
                        gbody = ast_new_list();
                        ast_list_append(gbody, sym("rt_coll_empty"));
                        ast_list_append(gbody, sym(coll_param));
                    } else {
                        gbody = ast_new_array();
                    }
                }

                if (cl->pattern_count < param_count) {
                    AST *app = ast_new_list();
                    ast_list_append(app, gbody);
                    for (int k = cl->pattern_count; k < param_count; k++) {
                        ast_list_append(app, sym(params[k].name));
                    }
                    gbody = app;
                }

                AST *branch = ast_new_list();
                ast_list_append(branch, combined);
                ast_list_append(branch, gbody);
                ast_list_append(cond_list, branch);
            }
            if (pattern_guard) ast_free(pattern_guard);
        } else {
            AST *guard = (guard_count == 0)
                ? sym("else")
                : make_and(guard_parts, guard_count);

            AST *body = ast_clone(cl->body);

            /* Polymorphic empty collection: if body is exactly [], desugar to
             * (rt_coll_empty __p_N) where __p_N is the first collection param. */
            if ((body->type == AST_LIST  && body->list.count == 0) ||
                (body->type == AST_ARRAY && body->array.element_count == 0)) {
                ast_free(body);
                const char *coll_param =
                    pmatch_pick_collection_param(cl, params, param_count);
                if (coll_param) {
                    body = ast_new_list();
                    ast_list_append(body, sym("rt_coll_empty"));
                    ast_list_append(body, sym(coll_param));
                } else {
                    body = ast_new_array();
                }
            }

            if (bind_count > 0)
                body = make_let(bind_names, bind_exprs, bind_count, body);

            {
                const char *coll_param =
                    pmatch_pick_collection_param(cl, params, param_count);
                if (coll_param) {
                    body = rewrite_array_literals(body, coll_param);
                }
            }

            if (cl->pattern_count < param_count) {
                /* If body is a letrec/let result: ((lambda ([binding]) ... sym) init)
                 * we must push the remaining-param application INSIDE the lambda body
                 * rather than outside, otherwise the type checker sees an extra function
                 * arrow wrapping the whole letrec expression.
                 * Detect: body is AST_LIST, items[0] is AST_LIST (the lambda call),
                 * items[0].items[0] is AST_LAMBDA — this is build_let's output.       */
                bool pushed_inside = false;
                if (body->type == AST_LIST &&
                    body->list.count >= 1 &&
                    body->list.items[0]->type == AST_LAMBDA) {
                    /* body is ((lambda ([go]) ... final_expr) init)
                     * Find the last body expression of the lambda and wrap it. */
                    AST *lam = body->list.items[0];
                    if (lam->lambda.body_count > 0) {
                        AST *last = lam->lambda.body_exprs[lam->lambda.body_count - 1];
                        AST *app = ast_new_list();
                        ast_list_append(app, last);
                        for (int k = cl->pattern_count; k < param_count; k++) {
                            ast_list_append(app, sym(params[k].name));
                        }
                        lam->lambda.body_exprs[lam->lambda.body_count - 1] = app;
                        lam->lambda.body = app;
                        pushed_inside = true;
                    }
                }
                if (!pushed_inside) {
                    AST *app = ast_new_list();
                    ast_list_append(app, body);
                    for (int k = cl->pattern_count; k < param_count; k++) {
                        ast_list_append(app, sym(params[k].name));
                    }
                    body = app;
                }
            }

            AST *clause = ast_new_list();
            ast_list_append(clause, guard);
            ast_list_append(clause, body);
            ast_list_append(cond_list, clause);
        }

        free(guard_parts);
        free(bind_names);
        free(bind_exprs);
    }

    // Fallthrough: non-exhaustive pattern match
    // Only add if the last clause wasn't already a wildcard (else)
    bool has_else = false;
    if (cond_list->list.count > 1) {
        AST *last_clause = cond_list->list.items[cond_list->list.count - 1];
        if (last_clause && last_clause->type == AST_LIST && last_clause->list.count > 0) {
            AST *last_guard  = last_clause->list.items[0];
            if (last_guard && last_guard->type == AST_SYMBOL && last_guard->symbol) {
                has_else = (strcmp(last_guard->symbol, "else") == 0);
            }
        }
    }
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
