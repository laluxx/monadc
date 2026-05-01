#include "wisp.h"
#include "reader.h"
#include "macro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/// Arity table

typedef struct ArityEntry {
    char      *name;
    int        arity;
    ParamKind  param_kinds[WISP_MAX_PARAMS];
    int        param_fn_arities[WISP_MAX_PARAMS]; // arity of fn params, -1 if unknown
    struct ArityEntry *next;
} ArityEntry;

#define ARITY_BUCKETS 128

typedef struct {
    ArityEntry *buckets[ARITY_BUCKETS];
} ArityTable;

static unsigned int arity_hash(const char *s) {
    unsigned int h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h % ARITY_BUCKETS;
}

static void arity_set(ArityTable *t, const char *name, int arity) {
    unsigned int h = arity_hash(name);
    for (ArityEntry *e = t->buckets[h]; e; e = e->next)
        if (strcmp(e->name, name) == 0) { e->arity = arity; return; }
    ArityEntry *e = malloc(sizeof(ArityEntry));
    e->name  = strdup(name);
    e->arity = arity;
    memset(e->param_kinds, PARAM_VALUE, sizeof(e->param_kinds));
    memset(e->param_fn_arities, -1, sizeof(e->param_fn_arities));
    e->next  = t->buckets[h];
    t->buckets[h] = e;
}

static void arity_set_with_kinds(ArityTable *t, const char *name, int arity,
                                  const ParamKind *kinds) {
    arity_set(t, name, arity);
    if (!kinds) return;
    unsigned int h = arity_hash(name);
    for (ArityEntry *e = t->buckets[h]; e; e = e->next)
        if (strcmp(e->name, name) == 0) {
            for (int i = 0; i < arity && i < WISP_MAX_PARAMS; i++)
                e->param_kinds[i] = kinds[i];
            return;
        }
}

static int arity_get(ArityTable *t, const char *name) {
    unsigned int h = arity_hash(name);
    for (ArityEntry *e = t->buckets[h]; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e->arity;
    return -2; /* unknown */
}

static ArityEntry *arity_get_entry(ArityTable *t, const char *name) {
    unsigned int h = arity_hash(name);
    for (ArityEntry *e = t->buckets[h]; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e;
    return NULL;
}

static void arity_free(ArityTable *t) {
    for (int i = 0; i < ARITY_BUCKETS; i++) {
        ArityEntry *e = t->buckets[i];
        while (e) { ArityEntry *n = e->next; free(e->name); free(e); e = n; }
        t->buckets[i] = NULL;
    }
}

/// Global FFI arity registry

static ArityTable g_ffi_arities;
static bool       g_ffi_arities_init = false;

void wisp_register_arity(const char *name, int arity) {
    if (!g_ffi_arities_init) {
        memset(&g_ffi_arities, 0, sizeof(g_ffi_arities));
        g_ffi_arities_init = true;
    }
    arity_set(&g_ffi_arities, name, arity);
}

int wisp_get_arity(const char *name) {
    if (!g_ffi_arities_init) return -99;
    return arity_get(&g_ffi_arities, name);
}

void wisp_clear_arities(void) {
    if (g_ffi_arities_init) {
        arity_free(&g_ffi_arities);
        g_ffi_arities_init = false;
    }
}


/// Pre-scan user-defined arities

static void arity_prescan(ArityTable *t, const char *source) {
    Lexer lex;
    lexer_init(&lex, source);
    while (true) {
        Token tok = lexer_next_token(&lex);
        if (tok.type == TOK_EOF) { free(tok.value); break; }
        /* Bare 'layout' symbol (wisp style): layout Name [f :: T] ... */
        if (tok.type == TOK_SYMBOL && strcmp(tok.value, "layout") == 0) {
            free(tok.value);
            tok = lexer_next_token(&lex); /* consume name */
            if (tok.type == TOK_SYMBOL) {
                char *lname = strdup(tok.value);
                free(tok.value);
                int field_count = 0;
                while (true) {
                    tok = lexer_next_token(&lex);
                    if (tok.type == TOK_EOF || tok.type == TOK_RPAREN ||
                        tok.type == TOK_KEYWORD) { free(tok.value); break; }
                    /* stop at next top-level symbol (next define/layout/etc) */
                    if (tok.type == TOK_SYMBOL && tok.value &&
                        (strcmp(tok.value, "define") == 0 ||
                         strcmp(tok.value, "layout") == 0 ||
                         strcmp(tok.value, "data")   == 0 ||
                         strcmp(tok.value, "type")   == 0 ||
                         strcmp(tok.value, "class")  == 0)) {
                        /* Put this token back by re-initialising the lexer
                         * to the position just before this token — we can't
                         * unget, so instead process it directly here.       */
                        if (strcmp(tok.value, "define") == 0) {
                            free(tok.value);
                            tok = lexer_next_token(&lex); /* fname */
                            if (tok.type == TOK_SYMBOL) {
                                char *fname = strdup(tok.value);
                                free(tok.value);
                                tok = lexer_next_token(&lex);
                                if (tok.type == TOK_LBRACKET) {
                                    int arity = 0;
                                    while (tok.type == TOK_LBRACKET) {
                                        arity++;
                                        int depth = 1;
                                        while (depth > 0) {
                                            free(tok.value);
                                            tok = lexer_next_token(&lex);
                                            if (tok.type == TOK_EOF) { depth = 0; break; }
                                            if (tok.type == TOK_LBRACKET) depth++;
                                            if (tok.type == TOK_RBRACKET) depth--;
                                        }
                                        free(tok.value);
                                        tok = lexer_next_token(&lex);
                                    }
                                    free(tok.value);
                                    arity_set(t, fname, arity);
                                } else {
                                    free(tok.value);
                                }
                                free(fname);
                            } else {
                                free(tok.value);
                            }
                        } else {
                            free(tok.value);
                        }
                        break;
                    }
                    if (tok.type == TOK_LBRACKET) {
                        field_count++;
                        int depth = 1;
                        while (depth > 0) {
                            free(tok.value);
                            tok = lexer_next_token(&lex);
                            if (tok.type == TOK_EOF) { depth = 0; break; }
                            if (tok.type == TOK_LBRACKET) depth++;
                            if (tok.type == TOK_RBRACKET) depth--;
                        }
                    }
                    free(tok.value);
                }
                arity_set(t, lname, field_count);
                free(lname);
            } else {
                free(tok.value);
            }
            continue;
        }

        /* (layout Name [f1 :: T1] [f2 :: T2] ...) — register constructor arity */

        /* Bare 'type' symbol (wisp style): type Name { ... } */
        if (tok.type == TOK_SYMBOL && strcmp(tok.value, "type") == 0) {
            free(tok.value);
            tok = lexer_next_token(&lex);
            if (tok.type == TOK_SYMBOL &&
                tok.value[0] >= 'A' && tok.value[0] <= 'Z') {
                char pred_name[256];
                snprintf(pred_name, sizeof(pred_name), "%s?", tok.value);
                arity_set(t, pred_name,  1);
            }
            free(tok.value);
            continue;
        }

        /* Bare wisp-style: define name :: T -> ... -> Ret */
        /* fprintf(stderr, "DEBUG prescan tok: type=%d val='%s'\n", tok.type, tok.value ? tok.value : "NULL"); */
        if (tok.type == TOK_SYMBOL && strcmp(tok.value, "define") == 0) {
            free(tok.value);
            tok = lexer_next_token(&lex);
            if (tok.type == TOK_SYMBOL) {
                char *fname = strdup(tok.value);
                free(tok.value);
                tok = lexer_next_token(&lex);
                if (tok.type == TOK_SYMBOL && strcmp(tok.value, "::") == 0) {
                    free(tok.value);
                    int arity = 0;
                    int fn_arities[WISP_MAX_PARAMS];
                    memset(fn_arities, -1, sizeof(fn_arities));
                    ParamKind kinds[WISP_MAX_PARAMS];
                    memset(kinds, PARAM_VALUE, sizeof(kinds));
                    bool found_arrow = false;
                    while (true) {
                        tok = lexer_next_token(&lex);
                        if (tok.type == TOK_EOF) { free(tok.value); break; }
                        if (tok.type == TOK_ARROW) {
                            free(tok.value); found_arrow = true; break;
                        }
                        if (tok.type == TOK_SYMBOL && strcmp(tok.value, "->") == 0) {
                            free(tok.value); found_arrow = true; break;
                        }
                        /* bracketed type like (a -> b -> b) — count inner arrows */
                        if (tok.type == TOK_LPAREN) {
                            free(tok.value);
                            int inner_arrows = 0;
                            bool is_fn = false;
                            int depth = 1;
                            while (depth > 0) {
                                tok = lexer_next_token(&lex);
                                if (tok.type == TOK_EOF) { depth = 0; free(tok.value); break; }
                                if (tok.type == TOK_LPAREN) depth++;
                                if (tok.type == TOK_RPAREN) { depth--; if (!depth) { free(tok.value); break; } }
                                if (depth > 0 && (tok.type == TOK_ARROW ||
                                    (tok.type == TOK_SYMBOL && strcmp(tok.value, "->") == 0))) {
                                    inner_arrows++;
                                    is_fn = true;
                                }
                                free(tok.value);
                            }
                            if (arity < WISP_MAX_PARAMS) {
                                kinds[arity] = is_fn ? PARAM_FUNC : PARAM_VALUE;
                                fn_arities[arity] = is_fn ? inner_arrows : -1;
                            }
                            arity++;
                            continue;
                        }
                        if (tok.type == TOK_SYMBOL && strcmp(tok.value, "::") != 0)
                            arity++;
                        free(tok.value);
                    }
                    if (found_arrow) {
                        /* skip just the return type token */
                        tok = lexer_next_token(&lex);
                        free(tok.value);
                        arity_set_with_kinds(t, fname, arity, kinds);
                        /* store per-param fn arities */
                        unsigned int h = arity_hash(fname);
                        for (ArityEntry *e = t->buckets[h]; e; e = e->next)
                            if (strcmp(e->name, fname) == 0) {
                                memcpy(e->param_fn_arities, fn_arities, sizeof(fn_arities));
                                break;
                            }
                    }
                    /* no arrow = variable definition, not a function, skip */
                } else if (tok.type == TOK_ARROW ||
                           (tok.type == TOK_SYMBOL && strcmp(tok.value, "->") == 0)) {
                    /* define name -> RetType — nullary function, arity 0 */
                    free(tok.value);
                    tok = lexer_next_token(&lex); /* consume return type */
                    free(tok.value);
                    arity_set(t, fname, 0);
                } else {
                    free(tok.value);
                }
                free(fname);
            } else {
                free(tok.value);
            }
            continue;
        }


        if (tok.type != TOK_LPAREN) { free(tok.value); continue; }
        free(tok.value);
        tok = lexer_next_token(&lex);

        /* (layout Name [f1 :: T1] [f2 :: T2] ...) — register constructor arity */
        if (tok.type == TOK_SYMBOL && strcmp(tok.value, "layout") == 0) {
            free(tok.value);
            tok = lexer_next_token(&lex); /* consume name */
            if (tok.type == TOK_SYMBOL) {
                char *lname = strdup(tok.value);
                free(tok.value);
                int field_count = 0;
                /* count [ tokens at depth 0 until ) or EOF */
                while (true) {
                    tok = lexer_next_token(&lex);
                    if (tok.type == TOK_EOF || tok.type == TOK_RPAREN) { free(tok.value); break; }
                    if (tok.type == TOK_LBRACKET) {
                        field_count++;
                        /* skip to matching ] */
                        int depth = 1;
                        while (depth > 0) {
                            free(tok.value);
                            tok = lexer_next_token(&lex);
                            if (tok.type == TOK_EOF) { depth = 0; break; }
                            if (tok.type == TOK_LBRACKET) depth++;
                            if (tok.type == TOK_RBRACKET) depth--;
                        }
                    }
                    free(tok.value);
                }
                arity_set(t, lname, field_count);
                free(lname);
            } else {
                free(tok.value);
            }
            continue;
        }

        /* (type Name ...) s-expression form */
        if (tok.type == TOK_SYMBOL && strcmp(tok.value, "type") == 0) {
            free(tok.value);
            tok = lexer_next_token(&lex);
            if (tok.type == TOK_SYMBOL &&
                tok.value[0] >= 'A' && tok.value[0] <= 'Z') {
                char pred_name[256];
                snprintf(pred_name, sizeof(pred_name), "%s?", tok.value);
                arity_set(t, pred_name,  1);
            }
            free(tok.value);
            continue;
        }

        if (tok.type != TOK_SYMBOL || strcmp(tok.value, "define") != 0) {
            free(tok.value); continue;
        }
        free(tok.value);
        tok = lexer_next_token(&lex);
        if (tok.type == TOK_LPAREN) {
                    free(tok.value);
                    tok = lexer_next_token(&lex);
                    if (tok.type != TOK_SYMBOL) { free(tok.value); continue; }
                    char *fname = strdup(tok.value);
                    free(tok.value);
                    int arity = 0;
                    ParamKind kinds[WISP_MAX_PARAMS];
                    memset(kinds, PARAM_VALUE, sizeof(kinds));
                    while (true) {
                        tok = lexer_next_token(&lex);
                        if (tok.type == TOK_EOF || tok.type == TOK_RPAREN) { free(tok.value); break; }
                        if (tok.type == TOK_LBRACKET) {
                            /* Peek inside bracket to detect Fn param:
                             * [name :: Fn :: ...] or [name :: (a -> b)] */
                            free(tok.value);
                            bool is_fn = false;
                            int depth = 1;
                            while (depth > 0) {
                                tok = lexer_next_token(&lex);
                                if (tok.type == TOK_EOF) { depth = 0; free(tok.value); break; }
                                if (tok.type == TOK_LBRACKET) depth++;
                                if (tok.type == TOK_RBRACKET) depth--;
                                /* Fn keyword or arrow type inside bracket = function param */
                                if (depth > 0 && tok.type == TOK_SYMBOL && tok.value &&
                                    (strcmp(tok.value, "Fn") == 0 ||
                                     strcmp(tok.value, "->") == 0))
                                    is_fn = true;
                                if (depth > 0 && tok.type == TOK_ARROW)
                                    is_fn = true;
                                free(tok.value);
                            }
                            if (arity < WISP_MAX_PARAMS)
                                kinds[arity] = is_fn ? PARAM_FUNC : PARAM_VALUE;
                            arity++; continue;
                        }
                        if (tok.type == TOK_ARROW) { free(tok.value); continue; }
                        if (tok.type == TOK_SYMBOL) {
                            if (strcmp(tok.value, "::") == 0) {
                                free(tok.value);
                                tok = lexer_next_token(&lex);
                                free(tok.value);
                                continue;
                            }
                            arity++;
                        }
                        free(tok.value);
                    }
                    arity_set_with_kinds(t, fname, arity, kinds);
                    free(fname);
        } else if (tok.type == TOK_SYMBOL) {
            char *vname = strdup(tok.value);
            free(tok.value);
            tok = lexer_next_token(&lex);
            /* define name :: T -> T -> Ret */
            if (tok.type == TOK_SYMBOL && strcmp(tok.value, "::") == 0) {
                free(tok.value);
                int arity = 0;
                bool seen_arrow = false;
                while (true) {
                    tok = lexer_next_token(&lex);
                    if (tok.type == TOK_EOF) { free(tok.value); break; }
                    if (tok.type == TOK_ARROW) { free(tok.value); seen_arrow = true; break; }
                    if (tok.type == TOK_SYMBOL && strcmp(tok.value, "->") == 0) {
                        free(tok.value); seen_arrow = true; break;
                    }
                    if (tok.type == TOK_SYMBOL && strcmp(tok.value, "::") != 0)
                        arity++;
                    free(tok.value);
                }
                /* skip just the return type token */
                if (seen_arrow) {
                    tok = lexer_next_token(&lex);
                    free(tok.value);
                }
                arity_set(t, vname, arity);
                free(vname);
                continue;
            }
            if (tok.type != TOK_LPAREN) { free(tok.value); free(vname); continue; }
            free(tok.value);
            tok = lexer_next_token(&lex);
            if (tok.type != TOK_SYMBOL || strcmp(tok.value, "lambda") != 0) {
                free(tok.value); free(vname); continue;
            }
            free(tok.value);
            tok = lexer_next_token(&lex);
            if (tok.type != TOK_LPAREN) { free(tok.value); free(vname); continue; }
            free(tok.value);
            int arity = 0;
            while (true) {
                tok = lexer_next_token(&lex);
                if (tok.type == TOK_EOF || tok.type == TOK_RPAREN) { free(tok.value); break; }
                if (tok.type == TOK_LBRACKET) {
                    free(tok.value);
                    int depth = 1;
                    while (depth > 0) {
                        tok = lexer_next_token(&lex);
                        if (tok.type == TOK_EOF) depth = 0;
                        if (tok.type == TOK_LBRACKET) depth++;
                        if (tok.type == TOK_RBRACKET) depth--;
                        free(tok.value);
                    }
                    arity++; continue;
                }
                if (tok.type == TOK_ARROW) { free(tok.value); continue; }
                if (tok.type == TOK_SYMBOL) {
                    if (strcmp(tok.value, "::") == 0) {
                        free(tok.value);
                        tok = lexer_next_token(&lex);
                        free(tok.value);
                        continue;
                    }
                    arity++;
                }
                free(tok.value);
            }
            arity_set(t, vname, arity);
            free(vname);
        } else if (tok.type == TOK_SYMBOL) {
            /* define name :: T1 -> T2 -> ... -> Ret */
            char *fname = strdup(tok.value);
            free(tok.value);
            tok = lexer_next_token(&lex);
            if (tok.type == TOK_SYMBOL && strcmp(tok.value, "::") == 0) {
                free(tok.value);
                int arity = 0;
                while (true) {
                    tok = lexer_next_token(&lex);
                    if (tok.type == TOK_EOF) { free(tok.value); break; }
                    if (tok.type == TOK_ARROW) { free(tok.value); break; }
                    if (tok.type == TOK_SYMBOL && strcmp(tok.value, "->") == 0) { free(tok.value); break; }
                    if (tok.type == TOK_SYMBOL && strcmp(tok.value, "::") != 0) arity++;
                    free(tok.value);
                }
                /* skip return type — stop at EOF or next '(' which
                 * signals the start of a new top-level form         */
                while (true) {
                    tok = lexer_next_token(&lex);
                    if (tok.type == TOK_EOF) { free(tok.value); break; }
                    if (tok.type == TOK_LPAREN) { free(tok.value); break; }
                    free(tok.value);
                }
                arity_set(t, fname, arity);
            } else {
                free(tok.value);
            }
            free(fname);
        } else {
            free(tok.value);
        }
    }
}

/// Token stream
//
// We flatten the entire source into a stream of string tokens,
// each tagged with its line's indent level. Indentation is only
// used to determine where a VARIADIC form ends — fixed-arity forms
// consume exactly N tokens regardless of indent.
//
typedef struct {
    char *text;    // token text, owned
    int   indent;  // indent of the line this token came from
    int   lineno;
} WToken;

typedef struct {
    WToken *tokens;
    int     count;
    int     cap;
    int     pos;   // current read position
} WTokenStream;

static void wts_push(WTokenStream *s, const char *text, int indent, int lineno) {
    if (s->count >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->tokens = realloc(s->tokens, sizeof(WToken) * s->cap);
    }
    s->tokens[s->count].text   = strdup(text);
    s->tokens[s->count].indent = indent;
    s->tokens[s->count].lineno = lineno;
    s->count++;
}

static void wts_free(WTokenStream *s) {
    for (int i = 0; i < s->count; i++) free(s->tokens[i].text);
    free(s->tokens);
}

#define WISP_TAB 4
static int measure_indent(const char *s) {
    int col = 0;
    while (*s == ' ' || *s == '\t') {
        if (*s == '\t') col += WISP_TAB - (col % WISP_TAB);
        else col++;
        s++;
    }
    return col;
}

/* Forward declaration — tokenise_into calls wisp_parse_expr for body expansion */
static void wisp_parse_expr(ArityTable *t, WTokenStream *s, SB *out, int parent_indent, int parent_remaining);

/* Tokenise one line into the stream */
static void tokenise_into(ArityTable *t, WTokenStream *s, const char *line,
                           int indent, int lineno) {
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == ';') break;
        if (*p == ',') { p++; continue; }

        /* string literal */
        if (*p == '"') {
            const char *start = p++;
            while (*p && !(*p == '"' && *(p-1) != '\\')) p++;
            if (*p == '"') p++;
            char *tok = strndup(start, p - start);
            wts_push(s, tok, indent, lineno);
            free(tok);
            continue;
        }

        /* char literal */
        if (*p == '\'' && *(p+1) && *(p+2) == '\'') {
            char tok[4]; memcpy(tok, p, 3); tok[3] = 0;
            wts_push(s, tok, indent, lineno);
            p += 3; continue;
        }
        if (*p == '\'' && *(p+1) == '\\' && *(p+2) && *(p+3) == '\'') {
            char tok[5]; memcpy(tok, p, 4); tok[4] = 0;
            wts_push(s, tok, indent, lineno);
            p += 4; continue;
        }

        /* quote prefix: '(...) — treat as single token */
        if (*p == '\'' && (*(p+1) == '(' || *(p+1) == '[' || *(p+1) == '{')) {
            const char *start = p++;
            char open  = *p;
            char close = open == '(' ? ')' : open == '[' ? ']' : '}';
            int depth = 0; bool in_str = false;
            while (*p) {
                if (in_str) {
                    if (*p == '\\') p++;
                    else if (*p == '"') in_str = false;
                    p++; continue;
                }
                if (*p == '"') { in_str = true; p++; continue; }
                if (*p == ';') break;
                if (*p == open)  depth++;
                if (*p == close) { depth--; p++; if (!depth) break; continue; }
                p++;
            }
            char *tok = strndup(start, p - start);
            wts_push(s, tok, indent, lineno);
            free(tok);
            continue;
        }

        /* grouped expression — keep as single token */
        if (*p == '(' || *p == '[' || *p == '{' ||
            (*p == '#' && *(p+1) == '{')) {
            const char *start = p;
            char open  = (*p == '#') ? (p++, '{') : *p;
            char close = open == '(' ? ')' : open == '[' ? ']' : '}';
            int depth = 0; bool in_str = false;
            while (*p) {
                if (in_str) {
                    if (*p == '\\') p++;
                    else if (*p == '"') in_str = false;
                    p++; continue;
                }
                if (*p == '"') { in_str = true; p++; continue; }
                if (*p == ';') break; /* strip inline comment */
                if (*p == open)  depth++;
                if (*p == close) { depth--; p++; if (!depth) break; continue; }
                p++;
            }
            /* trim trailing whitespace before comment */
            const char *end = p;
            if (*p == ';') {
                end = p;
                while (end > start && (*(end-1) == ' ' || *(end-1) == '\t'))
                    end--;
            }
            char *tok = strndup(start, end - start);

            /* After a grouped token, apply the same :: annotation grouping
             * that plain symbols get. If next non-space token is '::',
             * consume the whole 'grouped :: type -> type -> ...' as one
             * token so variadic form bodies stay as single logical units.
             * Example: (=) :: a -> a -> Bool  =>  [(=) :: a -> a -> Bool]
             * Example: (x != y) => body       =>  kept as arrow clause   */
            const char *peek = p;
            while (*peek == ' ' || *peek == '\t') peek++;

            if (strncmp(peek, "::", 2) == 0 &&
                (peek[2] == ' ' || peek[2] == '\t' || peek[2] == '\0')) {
                /* Reuse the same :: grouping logic as for plain symbols:
                 * build [tok :: T1 -> T2 -> ...] */
                SB ann; sb_init(&ann);
                sb_putc(&ann, '[');
                sb_puts(&ann, tok);
                free(tok);

                const char *q = peek;
                while (strncmp(q, "::", 2) == 0 &&
                       (q[2] == ' ' || q[2] == '\t' || q[2] == '\0')) {
                    q += 2;
                    while (*q == ' ' || *q == '\t') q++;
                    const char *seg_start = q;
                    const char *seg_end;
                    if (*q == '[' || *q == '(') {
                        char sopen  = *q;
                        char sclose = sopen == '[' ? ']' : ')';
                        int d = 0;
                        seg_end = q;
                        while (*seg_end) {
                            if (*seg_end == sopen)  d++;
                            if (*seg_end == sclose) { d--; seg_end++; if (!d) break; continue; }
                            seg_end++;
                        }
                    } else {
                        seg_end = q;
                        while (*seg_end && *seg_end != ' ' && *seg_end != '\t' &&
                               *seg_end != ';' && *seg_end != '\n') seg_end++;
                    }
                    if (seg_end == seg_start) break;
                    char *seg = strndup(seg_start, seg_end - seg_start);
                    sb_puts(&ann, " :: ");
                    sb_puts(&ann, seg);
                    free(seg);
                    q = seg_end;
                    while (*q == ' ' || *q == '\t') q++;
                    /* consume -> chains */
                    while (*q == '-' && *(q+1) == '>' &&
                           (q[2] == ' ' || q[2] == '\t' || q[2] == '\0')) {
                        sb_puts(&ann, " :: ->");
                        q += 2;
                        while (*q == ' ' || *q == '\t') q++;
                        if (*q == '[' || *q == '(') {
                            char sopen2  = *q;
                            char sclose2 = sopen2 == '[' ? ']' : ')';
                            int d2 = 0;
                            seg_start = q;
                            seg_end   = q;
                            while (*seg_end) {
                                if (*seg_end == sopen2)  d2++;
                                if (*seg_end == sclose2) { d2--; seg_end++; if (!d2) break; continue; }
                                seg_end++;
                            }
                        } else {
                            seg_start = q;
                            seg_end   = q;
                            while (*seg_end && *seg_end != ' ' && *seg_end != '\t' &&
                                   *seg_end != ';' && *seg_end != '\n') seg_end++;
                        }
                        if (seg_end == seg_start) break;
                        char *seg2 = strndup(seg_start, seg_end - seg_start);
                        sb_puts(&ann, " :: ");
                        sb_puts(&ann, seg2);
                        free(seg2);
                        q = seg_end;
                        while (*q == ' ' || *q == '\t') q++;
                    }
                }
                sb_putc(&ann, ']');
                char *bracketed = sb_take(&ann);
                wts_push(s, bracketed, indent, lineno);
                free(bracketed);
                p = q;
            } else if (strncmp(peek, "=>", 2) == 0 &&
                       (peek[2] == ' ' || peek[2] == '\t' || peek[2] == '\0')) {
                /* (pattern) => body -- keep as arrow clause token so the
                 * build_token_stream arrow-clause handler processes the body
                 * with full infix promotion. Emit pattern and => separately
                 * so the existing -> clause machinery picks them up.        */
                wts_push(s, tok, indent, lineno);
                free(tok);
                /* p is already past the grouped token; peek points at =>
                 * let the main loop continue and tokenize => and body */
                p = peek;
            } else {
                wts_push(s, tok, indent, lineno);
                free(tok);
            }
            continue;
        }

        /* regular token */
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ';') p++;
        char *tok = strndup(start, p - start);

        /* removed -> single-token grouping hack */

        /* Peek ahead: if next non-space token is '::',
         * group entire chain as [tok :: T1 :: T2 :: ...].
         * Types may themselves be parenthesized arrow types: (Int -> Int). */
        const char *peek = p;
        while (*peek == ' ' || *peek == '\t') peek++;
        if (strncmp(peek, "::", 2) == 0 &&
            (peek[2] == ' ' || peek[2] == '\t' || peek[2] == '\0')) {

            /* Build the bracket content: start with the name */
            SB ann; sb_init(&ann);
            sb_putc(&ann, '[');
            sb_puts(&ann, tok);

            const char *q = peek;
            while (strncmp(q, "::", 2) == 0 &&
                   (q[2] == ' ' || q[2] == '\t' || q[2] == '\0')) {
                /* consume '::' */
                q += 2;
                while (*q == ' ' || *q == '\t') q++;

                /* consume one type token (may be bracketed, or plain word,
                 * followed optionally by -> chains for arrow types)         */
                const char *seg_start = q;
                const char *seg_end;
                if (*q == '[' || *q == '(') {
                    /* bracketed segment — find matching close */
                    char open  = *q;
                    char close = open == '[' ? ']' : ')';
                    int depth  = 0;
                    seg_end = q;
                    while (*seg_end) {
                        if (*seg_end == open)  depth++;
                        if (*seg_end == close) { depth--; seg_end++; if (!depth) break; continue; }
                        seg_end++;
                    }
                } else {
                    seg_end = q;
                    while (*seg_end && *seg_end != ' ' && *seg_end != '\t' &&
                           *seg_end != ';' && *seg_end != '\n') seg_end++;
                }

                if (seg_end == seg_start) break; /* nothing to consume */

                char *seg = strndup(seg_start, seg_end - seg_start);
                sb_puts(&ann, " :: ");
                sb_puts(&ann, seg);
                free(seg);
                q = seg_end;

                /* skip spaces and peek at next token */
                while (*q == ' ' || *q == '\t') q++;

                /* Also consume any trailing -> Type pairs so that
                 * (Int -> Int) -> (a) -> (a) becomes one annotation:
                 * [name :: (Int -> Int) :: -> :: (a) :: -> :: (a)]
                 * which parse_fn_signature already knows how to read.      */
                while (*q == '-' && *(q+1) == '>' &&
                       (q[2] == ' ' || q[2] == '\t' || q[2] == '\0')) {
                    sb_puts(&ann, " :: ->");
                    q += 2;
                    while (*q == ' ' || *q == '\t') q++;

                    /* consume next type segment */
                    if (*q == '[' || *q == '(') {
                        char open2  = *q;
                        char close2 = open2 == '[' ? ']' : ')';
                        int depth2  = 0;
                        seg_start = q;
                        seg_end   = q;
                        while (*seg_end) {
                            if (*seg_end == open2)  depth2++;
                            if (*seg_end == close2) { depth2--; seg_end++; if (!depth2) break; continue; }
                            seg_end++;
                        }
                    } else {
                        seg_start = q;
                        seg_end   = q;
                        while (*seg_end && *seg_end != ' ' && *seg_end != '\t' &&
                               *seg_end != ';' && *seg_end != '\n') seg_end++;
                    }

                    if (seg_end == seg_start) break;

                    char *seg2 = strndup(seg_start, seg_end - seg_start);
                    sb_puts(&ann, " :: ");
                    sb_puts(&ann, seg2);
                    free(seg2);
                    q = seg_end;
                    while (*q == ' ' || *q == '\t') q++;
                }
            }

            sb_putc(&ann, ']');
            char *bracketed = sb_take(&ann);
            wts_push(s, bracketed, indent, lineno);
            free(bracketed);
            free(tok);
            p = q;
            continue;
        }

        wts_push(s, tok, indent, lineno);
        free(tok);
    }
}

// Build token stream from source.
// Multi-line grouped expressions (starting with '(' '[' '{') are
// accumulated across lines and emitted as a single token.
static WTokenStream build_token_stream(const char *source, ArityTable *at) {
    WTokenStream s = {0};
    const char *p = source;
    int lineno = 1;

    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        int len = (int)(p - line_start);
        if (*p == '\n') p++;

        char *raw = strndup(line_start, len);
        const char *t = raw;
        while (*t == ' ' || *t == '\t') t++;

        /* skip blank/comment lines */
        if (!*t || *t == ';') { free(raw); lineno++; continue; }

        int indent = measure_indent(raw);

        /* Check for pmatch clause: "pattern -> body" or "| guard -> body".
         * We split the line at top-level '->'. */
        if (strncmp(t, "define", 6) != 0 && strncmp(t, "layout", 6) != 0 && strncmp(t, "data", 4) != 0) {
            const char *arrow = NULL;
            int bdepth = 0;
            bool instr = false;
            for (const char *q = t; *q; q++) {
                if (instr) {
                    if (*q == '\\' && *(q+1)) q++;
                    else if (*q == '"') instr = false;
                    continue;
                }
                if (*q == '"') { instr = true; continue; }
                if (*q == ';') break;
                if (*q == '(' || *q == '[' || *q == '{') bdepth++;
                if (*q == ')' || *q == ']' || *q == '}') bdepth--;
                if (bdepth == 0 && (
                        (*q == '-' && *(q+1) == '>') ||
                        (*q == '=' && *(q+1) == '>'))) {
                    if (q == t || *(q-1) == ' ' || *(q-1) == '\t') {
                        arrow = q;
                        break;
                    }
                }
            }

            if (arrow && !strstr(t, "::")) {
                size_t left_len = arrow - t;
                while (left_len > 0 && (t[left_len-1] == ' ' || t[left_len-1] == '\t')) left_len--;

                const char *right_start = arrow + 2;
                while (*right_start == ' ' || *right_start == '\t') right_start++;
                const char *right_end = right_start;
                while (*right_end && *right_end != ';') right_end++;
                while (right_end > right_start && (*(right_end-1) == ' ' || *(right_end-1) == '\t')) right_end--;

                /* Check if the body has unbalanced brackets — if so,
                         * accumulate continuation lines until balanced.       */
                        int body_depth = 0;
                        bool body_in_str = false;
                        for (const char *bq = right_start; bq < right_end; bq++) {
                            if (body_in_str) {
                                if (*bq == '\\') bq++;
                                else if (*bq == '"') body_in_str = false;
                                continue;
                            }
                            if (*bq == '"') { body_in_str = true; continue; }
                            if (*bq == '(' || *bq == '[' || *bq == '{') body_depth++;
                            if (*bq == ')' || *bq == ']' || *bq == '}') body_depth--;
                        }

                        size_t body_acc_cap = 256;
                        char *body_acc = malloc(body_acc_cap);
                        size_t body_acc_len = right_end - right_start;
                        memcpy(body_acc, right_start, body_acc_len);
                        body_acc[body_acc_len] = '\0';

                        while (body_depth != 0 && *p) {
                            const char *cls = p;
                            while (*p && *p != '\n') p++;
                            if (*p == '\n') p++;
                            int cll = (int)(p - cls);
                            char *clraw = strndup(cls, cll);
                            const char *clt = clraw;
                            while (*clt == ' ' || *clt == '\t') clt++;
                            if (!*clt || *clt == ';') { free(clraw); lineno++; continue; }
                            const char *clt_end = clt;
                            bool clt_ins = false;
                            while (*clt_end) {
                                if (clt_ins) {
                                    if (*clt_end == '\\') clt_end++;
                                    else if (*clt_end == '"') clt_ins = false;
                                    clt_end++; continue;
                                }
                                if (*clt_end == '"') { clt_ins = true; clt_end++; continue; }
                                if (*clt_end == ';') break;
                                clt_end++;
                            }
                            while (clt_end > clt && (*(clt_end-1)==' '||*(clt_end-1)=='\t')) clt_end--;
                            size_t cltlen = clt_end - clt;
                            while (body_acc_len + cltlen + 3 >= body_acc_cap) {
                                body_acc_cap *= 2; body_acc = realloc(body_acc, body_acc_cap);
                            }
                            body_acc[body_acc_len++] = ' ';
                            memcpy(body_acc + body_acc_len, clt, cltlen);
                            body_acc_len += cltlen;
                            body_acc[body_acc_len] = '\0';
                            for (const char *bq = clt; *bq && *bq != ';'; bq++) {
                                if (*bq == '(' || *bq == '[' || *bq == '{') body_depth++;
                                if (*bq == ')' || *bq == ']' || *bq == '}') body_depth--;
                            }
                            free(clraw);
                            lineno++;
                        }

                        char *body_raw = body_acc;
                        WTokenStream body_ts = {0};
                        tokenise_into(at, &body_ts, body_raw, indent, lineno);
                        SB body_sb; sb_init(&body_sb);
                        bool first_tok = true;
                        while (body_ts.pos < body_ts.count) {
                            if (!first_tok) sb_putc(&body_sb, ' ');
                            first_tok = false;
                            wisp_parse_expr(at, &body_ts, &body_sb, -1, 1);
                        }
                        char *body_expanded = sb_take(&body_sb);
                        wts_free(&body_ts);
                        free(body_raw);

                        char *left_str = strndup(t, left_len);
                        const char *lscan = left_str;
                        while (*lscan == ' ' || *lscan == '\t') lscan++;

                        /* Register pattern variable arities from the enclosing
                         * define's param_fn_arities so body calls like
                         * "f x xs" expand correctly when f :: (a->b->b).
                         * We look backwards in the token stream for the most
                         * recent "define" token and use its fn_arities.      */
                        {
                            /* Find the define name from recent tokens */
                            const char *def_name = NULL;
                            for (int _ti = s.count - 1; _ti >= 0; _ti--) {
                                if (strcmp(s.tokens[_ti].text, "define") == 0 &&
                                    _ti + 1 < s.count) {
                                    const char *dn = s.tokens[_ti + 1].text;
                                    /* strip leading '(' if present */
                                    if (dn[0] == '(') dn++;
                                    def_name = dn;
                                    break;
                                }
                            }
                            if (def_name) {
                                unsigned int _h = arity_hash(def_name);
                                ArityEntry *_de = NULL;
                                for (ArityEntry *_e = at->buckets[_h]; _e; _e = _e->next)
                                    if (strcmp(_e->name, def_name) == 0) { _de = _e; break; }
                                if (_de) {
                                    /* Walk pattern tokens and assign arities */
                                    const char *_ps = lscan;
                                    if (*_ps == '|') { /* skip guard marker */
                                        while (*_ps && *_ps != ' ' && *_ps != '\t') _ps++;
                                        while (*_ps == ' ' || *_ps == '\t') _ps++;
                                    }
                                    int _pi = 0;
                                    while (*_ps && _pi < _de->arity) {
                                        while (*_ps == ' ' || *_ps == '\t') _ps++;
                                        if (!*_ps) break;
                                        /* skip wildcard '_' */
                                        if (*_ps == '_') {
                                            _pi++;
                                            while (*_ps && *_ps != ' ' && *_ps != '\t') _ps++;
                                            continue;
                                        }
                                        /* skip grouped pattern like [] or [x|xs] */
                                        if (*_ps == '[' || *_ps == '(') {
                                            char _oc = *_ps;
                                            char _cc = _oc == '[' ? ']' : ')';
                                            int _d = 0;
                                            while (*_ps) {
                                                if (*_ps == _oc) _d++;
                                                else if (*_ps == _cc) { _d--; _ps++; if (!_d) break; continue; }
                                                _ps++;
                                            }
                                            _pi++;
                                            continue;
                                        }
                                        /* plain variable name */
                                        const char *_ve = _ps;
                                        while (*_ve && *_ve != ' ' && *_ve != '\t') _ve++;
                                        size_t _vl = _ve - _ps;
                                        if (_vl > 0 && _de->param_fn_arities[_pi] > 0) {
                                            char _vname[128];
                                            size_t _vn = _vl < 127 ? _vl : 127;
                                            memcpy(_vname, _ps, _vn);
                                            _vname[_vn] = '\0';
                                            arity_set(at, _vname, _de->param_fn_arities[_pi]);
                                        }
                                        _ps = _ve;
                                        _pi++;
                                    }
                                }
                            }
                        }

                        if (*lscan == '|') {
                            wts_push(&s, "|", indent, lineno);
                            lscan++;
                            while (*lscan == ' ' || *lscan == '\t') lscan++;
                            if (*lscan && *lscan != '\0') {
                                bool has_space = false;
                                for (const char *c = lscan; *c; c++) {
                                    if (*c == ' ' || *c == '\t') { has_space = true; break; }
                                }
                                size_t glen = strlen(lscan) + 3;
                                char *guard = malloc(glen);
                                if (has_space) snprintf(guard, glen, "(%s)", lscan);
                                else snprintf(guard, glen, "%s", lscan);
                                wts_push(&s, guard, indent, lineno);
                                free(guard);
                            }
                        } else {
                            tokenise_into(at, &s, left_str, indent, lineno);
                        }
                        free(left_str);

                        wts_push(&s, (arrow[0] == '=') ? "=>" : "->", indent, lineno);
                        /* If body is a bare multi-token call (not already grouped),
                         * wrap it so wisp_parse_expr emits it as a single s-expr. */
                        if (body_expanded[0] != '(' && body_expanded[0] != '[' &&
                            body_expanded[0] != '{' && strchr(body_expanded, ' ')) {
                            size_t wlen = strlen(body_expanded) + 3;
                            char *wrapped = malloc(wlen);
                            snprintf(wrapped, wlen, "(%s)", body_expanded);
                            wts_push(&s, wrapped, indent, lineno);
                            free(wrapped);
                        } else {
                            wts_push(&s, body_expanded, indent, lineno);
                        }
                        free(body_expanded);

                        free(raw);
                        lineno++;
                        continue;

            }
        }

        /* Check if this line starts a grouped expression
           an unbalanced one (e.g. "define foo [" split across lines) */
        {
            int _pre_depth = 0;
            bool _pre_str = false;
            for (const char *_q = t; *_q; _q++) {
                if (_pre_str) {
                    if (*_q == '\\') _q++;
                    else if (*_q == '"') _pre_str = false;
                    continue;
                }
                if (*_q == '"') { _pre_str = true; continue; }
                if (*_q == ';') break;
                if (*_q == '(' || *_q == '[' || *_q == '{') _pre_depth++;
                if (*_q == ')' || *_q == ']' || *_q == '}') _pre_depth--;
            }
            if (_pre_depth != 0 && !(*t == '(' || *t == '[' || *t == '{' || (*t == '#' && *(t+1) == '{'))) {
                /* Line has unbalanced brackets but doesn't start with one —
                 * append continuation lines until balanced */
                size_t acc_cap = 256;
                char *acc = malloc(acc_cap);
                size_t acc_len = 0;
                const char *t_end = t;
                bool t_ins = false;
                while (*t_end) {
                    if (t_ins) { if (*t_end=='\\') t_end++; else if (*t_end=='"') t_ins=false; t_end++; continue; }
                    if (*t_end=='"') { t_ins=true; t_end++; continue; }
                    if (*t_end==';') break;
                    t_end++;
                }
                while (t_end > t && (*(t_end-1)==' '||*(t_end-1)=='\t')) t_end--;
                size_t tlen = t_end - t;
                while (acc_len + tlen + 2 >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
                memcpy(acc + acc_len, t, tlen);
                acc_len += tlen;
                acc[acc_len] = '\0';
                free(raw);
                lineno++;
                int depth = _pre_depth;
                while (depth != 0 && *p) {
                    const char *ls = p;
                    while (*p && *p != '\n') p++;
                    if (*p == '\n') p++;
                    int ll = (int)(p - ls);
                    char *lraw = strndup(ls, ll);
                    const char *lt = lraw;
                    while (*lt == ' ' || *lt == '\t') lt++;
                    if (!*lt || *lt == ';') { free(lraw); lineno++; continue; }
                    const char *lt_end = lt;
                    bool lt_ins = false;
                    while (*lt_end) {
                        if (lt_ins) { if (*lt_end=='\\') lt_end++; else if (*lt_end=='"') lt_ins=false; lt_end++; continue; }
                        if (*lt_end=='"') { lt_ins=true; lt_end++; continue; }
                        if (*lt_end==';') break;
                        lt_end++;
                    }
                    while (lt_end > lt && (*(lt_end-1)==' '||*(lt_end-1)=='\t')) lt_end--;
                    size_t ltlen = lt_end - lt;
                    while (acc_len + ltlen + 3 >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
                    acc[acc_len++] = ' ';
                    memcpy(acc + acc_len, lt, ltlen);
                    acc_len += ltlen;
                    acc[acc_len] = '\0';
                    for (const char *q = lt; *q && *q != ';'; q++) {
                        if (*q=='('||*q=='['||*q=='{') depth++;
                        if (*q==')'||*q==']'||*q=='}') depth--;
                    }
                    free(lraw);
                    lineno++;
                }
                tokenise_into(at, &s, acc, indent, lineno - 1);
                free(acc);
                continue;
            }
        }
        if (*t == '(' || *t == '[' || *t == '{' ||
            (*t == '#' && *(t+1) == '{')) {
            /* Count balance — if unbalanced, accumulate more lines */
            int depth = 0;
            bool in_str = false;
            for (const char *q = t; *q; q++) {
                if (in_str) {
                    if (*q == '\\') q++;
                    else if (*q == '"') in_str = false;
                    continue;
                }
                if (*q == '"') { in_str = true; continue; }
                if (*q == ';') break; /* stop at inline comment */
                if (*q=='('||*q=='['||*q=='{') depth++;
                if (*q==')'||*q==']'||*q=='}') depth--;
            }

            if (depth == 0) {
                tokenise_into(at, &s, t, indent, lineno);
                free(raw);
                lineno++;
                continue;
            }

            /* Unbalanced — accumulate lines until balanced */
            size_t acc_cap = 256;
            char *acc = malloc(acc_cap);
            size_t acc_len = 0;

            /* copy first line, stripping inline comment */
            const char *t_end = t;
            bool t_in_str = false;
            while (*t_end) {
                if (t_in_str) {
                    if (*t_end == '\\') t_end++;
                    else if (*t_end == '"') t_in_str = false;
                    t_end++; continue;
                }
                if (*t_end == '"') { t_in_str = true; t_end++; continue; }
                if (*t_end == ';') break;
                t_end++;
            }
            /* trim trailing whitespace */
            while (t_end > t && (*(t_end-1) == ' ' || *(t_end-1) == '\t')) t_end--;
            size_t tlen = t_end - t;
            while (acc_len + tlen + 2 >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
            memcpy(acc + acc_len, t, tlen);
            acc_len += tlen;
            acc[acc_len] = '\0';
            free(raw);
            lineno++;

            while (depth != 0 && *p) {
                const char *ls = p;
                while (*p && *p != '\n') p++;
                int ll = (int)(p - ls);
                if (*p == '\n') p++;

                char *lraw = strndup(ls, ll);
                const char *lt = lraw;
                while (*lt == ' ' || *lt == '\t') lt++;

                /* skip blank/comment lines inside grouped expr */
                if (!*lt || *lt == ';') { free(lraw); lineno++; continue; }

                /* Strip inline comments from continuation lines */
                const char *lt_end = lt;
                bool in_str2 = false;
                while (*lt_end) {
                    if (in_str2) {
                        if (*lt_end == '\\') lt_end++;
                        else if (*lt_end == '"') in_str2 = false;
                        lt_end++;
                        continue;
                    }
                    if (*lt_end == '"') { in_str2 = true; lt_end++; continue; }
                    if (*lt_end == ';') break;
                    lt_end++;
                }
                /* trim trailing whitespace before comment */
                while (lt_end > lt && (*(lt_end-1) == ' ' || *(lt_end-1) == '\t'))
                    lt_end--;
                char *line_expanded = strndup(lt, lt_end - lt);
                size_t ltlen = strlen(line_expanded);

                char _ld[48];
                int _ldlen = snprintf(_ld, sizeof(_ld), " (#line %d 1) ", lineno);
                while (acc_len + (size_t)_ldlen + ltlen + 3 >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
                memcpy(acc + acc_len, _ld, _ldlen);
                acc_len += _ldlen;
                memcpy(acc + acc_len, line_expanded, ltlen);
                acc_len += ltlen;
                acc[acc_len] = '\0';
                free(line_expanded);

                /* update depth */
                in_str = false;
                for (const char *q = lt; *q; q++) {
                    if (in_str) {
                        if (*q == '\\') q++;
                        else if (*q == '"') in_str = false;
                        continue;
                    }
                    if (*q == '"') { in_str = true; continue; }
                    if (*q=='('||*q=='['||*q=='{') depth++;
                    if (*q==')'||*q==']'||*q=='}') depth--;
                }
                free(lraw);
                lineno++;
            }

            wts_push(&s, acc, indent, lineno - 1);
            free(acc);
            continue;

        }

        /* Normal non-grouped line */
        /* 'data' lines: collect raw text until indent drops, wrap in parens,
         * pass verbatim to the reader — no wisp processing of internals.   */
        if ((strncmp(t, "layout", 6) == 0 &&
             (t[6] == ' ' || t[6] == '\t' || t[6] == '\0')) ||
            (strncmp(t, "data", 4) == 0 &&
             (t[4] == ' ' || t[4] == '\t' || t[4] == '\0')) ||
            (strncmp(t, "class", 5) == 0 &&
             (t[5] == ' ' || t[5] == '\t' || t[5] == '\0')) ||
            (strncmp(t, "instance", 8) == 0 &&
             (t[8] == ' ' || t[8] == '\t' || t[8] == '\0'))) {

            size_t acc_cap = 256;
            char *acc = malloc(acc_cap);
            size_t acc_len = 0;
            acc[0] = '\0';

            acc[acc_len++] = '(';
            const char *t_end = t;
            bool t_ins = false;
            while (*t_end) {
                if (t_ins) {
                    if (*t_end == '\\') t_end++;
                    else if (*t_end == '"') t_ins = false;
                    t_end++; continue;
                }
                if (*t_end == '"') { t_ins = true; t_end++; continue; }
                if (*t_end == ';') break;
                t_end++;
            }
            while (t_end > t && (*(t_end-1)==' '||*(t_end-1)=='\t')) t_end--;
            size_t tlen = t_end - t;
            while (acc_len + tlen + 4 >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
            memcpy(acc + acc_len, t, tlen);
            acc_len += tlen;
            acc[acc_len] = '\0';
            free(raw);
            lineno++;

            while (*p) {
                const char *ls = p;
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
                int ll = (int)(p - ls);
                if (ll > 0 && *(p-1) == '\n') ll--;

                char *lraw = strndup(ls, ll);
                const char *lt = lraw;
                while (*lt == ' ' || *lt == '\t') lt++;

                if (!*lt || *lt == ';') { free(lraw); lineno++; continue; }

                int l_indent = measure_indent(lraw);
                if (l_indent <= indent) { p = ls; free(lraw); break; }

                const char *lt_end = lt;
                bool lt_ins = false;
                while (*lt_end) {
                    if (lt_ins) {
                        if (*lt_end == '\\') lt_end++;
                        else if (*lt_end == '"') lt_ins = false;
                        lt_end++; continue;
                    }
                    if (*lt_end == '"') { lt_ins = true; lt_end++; continue; }
                    if (*lt_end == ';') break;
                    lt_end++;
                }
                while (lt_end > lt && (*(lt_end-1)==' '||*(lt_end-1)=='\t')) lt_end--;

                char _ld2[48];
                int _ld2len = snprintf(_ld2, sizeof(_ld2), " (#line %d 1) ", lineno);

                /* Detect => at depth 0 so we can wisp-process the body */
                const char *fat_arrow = NULL;
                {
                    int bdepth = 0; bool bins = false;
                    for (const char *q = lt; q < lt_end; q++) {
                        if (bins) {
                            if (*q=='\\') q++;
                            else if (*q=='"') bins = false;
                            continue;
                        }
                        if (*q=='"') { bins=true; continue; }
                        if (*q=='('||*q=='['||*q=='{') bdepth++;
                        if (*q==')'||*q==']'||*q=='}') bdepth--;
                        if (bdepth==0 && *q=='=' && *(q+1)=='>') {
                            fat_arrow = q; break;
                        }
                    }
                }

                if (fat_arrow) {
                    /* Pattern part: kept verbatim */
                    size_t pat_len = fat_arrow - lt;
                    while (pat_len > 0 &&
                           (lt[pat_len-1]==' '||lt[pat_len-1]=='\t')) pat_len--;

                    /* Body part: run through wisp token stream + parser */
                    const char *body_start = fat_arrow + 2;
                    while (*body_start==' '||*body_start=='\t') body_start++;
                    size_t body_len = lt_end - body_start;
                    char *body_str = strndup(body_start, body_len);

                    WTokenStream body_ts = {0};
                    tokenise_into(at, &body_ts, body_str, l_indent, lineno);
                    free(body_str);

                    SB body_sb; sb_init(&body_sb);
                    /* Parse the entire body as ONE expression from the
                     * token stream. Since body_ts tokens all share the
                     * same lineno, the first wisp_parse_expr call will
                     * consume everything via last-arg infix promotion.
                     * We force this by setting parent_remaining=1 so
                     * the call knows it is filling a slot — but the
                     * real driver is that the head's arity loop with
                     * infix promotion sees all tokens on the same line. */
                    for (int _dbi = 0; _dbi < body_ts.count; _dbi++)
                        fprintf(stderr, "  body_ts[%d] = '%s' lineno=%d arity=%d\n",
                                _dbi, body_ts.tokens[_dbi].text,
                                body_ts.tokens[_dbi].lineno,
                                arity_get(at, body_ts.tokens[_dbi].text));
                    while (body_ts.pos < body_ts.count) {
                        if (body_ts.pos > 0) sb_putc(&body_sb, ' ');
                        wisp_parse_expr(at, &body_ts, &body_sb, l_indent, 1);
                    }
                    char *body_expanded = sb_take(&body_sb);
                    wts_free(&body_ts);

                    size_t need = acc_len + _ld2len + pat_len + 4
                                  + strlen(body_expanded) + 4;
                    while (need >= acc_cap) { acc_cap*=2; acc=realloc(acc,acc_cap); }
                    memcpy(acc+acc_len, _ld2, _ld2len); acc_len += _ld2len;
                    memcpy(acc+acc_len, lt, pat_len);   acc_len += pat_len;
                    acc[acc_len++]=' '; acc[acc_len++]='='; acc[acc_len++]='>';
                    acc[acc_len++]=' ';
                    size_t bel = strlen(body_expanded);
                    while (acc_len+bel+2 >= acc_cap) { acc_cap*=2; acc=realloc(acc,acc_cap); }
                    memcpy(acc+acc_len, body_expanded, bel); acc_len += bel;
                    acc[acc_len] = '\0';
                    free(body_expanded);
                } else {
                    /* Signature line (::) or other -- verbatim */
                    size_t ltlen = lt_end - lt;
                    while (acc_len + (size_t)_ld2len + ltlen + 4 >= acc_cap) {
                        acc_cap *= 2; acc = realloc(acc, acc_cap);
                    }
                    memcpy(acc+acc_len, _ld2, _ld2len); acc_len += _ld2len;
                    memcpy(acc+acc_len, lt, ltlen);      acc_len += ltlen;
                    acc[acc_len] = '\0';
                }
                free(lraw);
                lineno++;
            }

            while (acc_len + 2 >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
            acc[acc_len++] = ')';
            acc[acc_len]   = '\0';

            wts_push(&s, acc, indent, lineno - 1);
            free(acc);
            continue;
        }

        /* class and instance: collect header on first line, then process
         * each body line through wisp infix promotion for => bodies,
         * keeping :: signature lines verbatim. Emit as single (class ...)
         * token so reader.c's existing class/instance parser handles it. */
        if ((strncmp(t, "class", 5) == 0 &&
             (t[5] == ' ' || t[5] == '\t' || t[5] == '\0')) ||
            (strncmp(t, "instance", 8) == 0 &&
             (t[8] == ' ' || t[8] == '\t' || t[8] == '\0'))) {

            size_t acc_cap = 256;
            char *acc = malloc(acc_cap);
            size_t acc_len = 0;
            acc[0] = '\0';

            /* opening paren + first line */
            acc[acc_len++] = '(';
            acc[acc_len] = '\0';

            const char *t_end = t;
            bool t_ins = false;
            while (*t_end) {
                if (t_ins) {
                    if (*t_end == '\\') t_end++;
                    else if (*t_end == '"') t_ins = false;
                    t_end++; continue;
                }
                if (*t_end == '"') { t_ins = true; t_end++; continue; }
                if (*t_end == ';') break;
                t_end++;
            }
            while (t_end > t && (*(t_end-1)==' '||*(t_end-1)=='\t')) t_end--;
            size_t tlen = t_end - t;
            while (acc_len + tlen + 4 >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
            memcpy(acc + acc_len, t, tlen);
            acc_len += tlen;
            acc[acc_len] = '\0';
            free(raw);
            lineno++;

            /* collect body lines indented deeper than the class head */
            while (*p) {
                const char *ls = p;
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
                int ll = (int)(p - ls);
                char *lraw = strndup(ls, ll);
                const char *lt = lraw;
                while (*lt == ' ' || *lt == '\t') lt++;
                if (!*lt || *lt == ';') { free(lraw); lineno++; continue; }

                int l_indent = measure_indent(lraw);
                if (l_indent <= indent) { p = ls; free(lraw); break; }

                /* strip inline comment and trailing whitespace */
                const char *lt_end = lt;
                bool lt_ins = false;
                while (*lt_end) {
                    if (lt_ins) {
                        if (*lt_end == '\\') lt_end++;
                        else if (*lt_end == '"') lt_ins = false;
                        lt_end++; continue;
                    }
                    if (*lt_end == '"') { lt_ins = true; lt_end++; continue; }
                    if (*lt_end == ';') break;
                    lt_end++;
                }
                while (lt_end > lt && (*(lt_end-1)==' '||*(lt_end-1)=='\t')) lt_end--;

                /* Check if this line has a => body that needs wisp processing */
                const char *arrow = NULL;
                {
                    int bdepth = 0; bool bins = false;
                    for (const char *q = lt; q < lt_end; q++) {
                        if (bins) {
                            if (*q == '\\') q++;
                            else if (*q == '"') bins = false;
                            continue;
                        }
                        if (*q == '"') { bins = true; continue; }
                        if (*q == '(' || *q == '[' || *q == '{') bdepth++;
                        if (*q == ')' || *q == ']' || *q == '}') bdepth--;
                        if (bdepth == 0 && *q == '=' && *(q+1) == '>') {
                            arrow = q; break;
                        }
                    }
                }

                char _ld[48];
                int _ldlen = snprintf(_ld, sizeof(_ld), " (#line %d 1) ", lineno);

                if (arrow) {
                    /* Split at =>: keep pattern verbatim, process body */
                    size_t pat_len = arrow - lt;
                    while (pat_len > 0 && (lt[pat_len-1]==' '||lt[pat_len-1]=='\t')) pat_len--;
                    char *pat_str = strndup(lt, pat_len);

                    const char *body_start = arrow + 2;
                    while (*body_start == ' ' || *body_start == '\t') body_start++;
                    size_t body_len = lt_end - body_start;
                    char *body_str = strndup(body_start, body_len);

                    /* Run wisp infix promotion on the body.
                     * Parse as a single expression with parent_remaining=1
                     * so the last-argument infix promotion fires correctly.
                     * This makes "not x = y" -> "(not (= x y))" because
                     * not has arity 1, its last (only) slot sees "=" next,
                     * and promotion builds (= x y) as the argument.        */
                    WTokenStream body_ts = {0};
                    tokenise_into(at, &body_ts, body_str, l_indent, lineno);
                    SB body_sb; sb_init(&body_sb);
                    if (body_ts.count > 0) {
                        wisp_parse_expr(at, &body_ts, &body_sb, -1, 1);
                    }
                    char *body_expanded = sb_take(&body_sb);
                    wts_free(&body_ts);
                    free(body_str);

                    /* Build: (#line N 1) pat_str => body_expanded */
                    size_t need = acc_len + _ldlen + pat_len + 4 + strlen(body_expanded) + 4;
                    while (need >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
                    memcpy(acc + acc_len, _ld, _ldlen); acc_len += _ldlen;
                    memcpy(acc + acc_len, pat_str, pat_len); acc_len += pat_len;
                    acc[acc_len++] = ' '; acc[acc_len++] = '='; acc[acc_len++] = '>';
                    acc[acc_len++] = ' ';
                    size_t bel = strlen(body_expanded);
                    while (acc_len + bel + 2 >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
                    memcpy(acc + acc_len, body_expanded, bel); acc_len += bel;
                    acc[acc_len] = '\0';
                    free(body_expanded);
                    free(pat_str);
                } else {
                    /* :: signature line or other -- keep verbatim */
                    size_t ltlen = lt_end - lt;
                    while (acc_len + (size_t)_ldlen + ltlen + 4 >= acc_cap) {
                        acc_cap *= 2; acc = realloc(acc, acc_cap);
                    }
                    memcpy(acc + acc_len, _ld, _ldlen); acc_len += _ldlen;
                    memcpy(acc + acc_len, lt, ltlen); acc_len += ltlen;
                    acc[acc_len] = '\0';
                }
                free(lraw);
                lineno++;
            }

            while (acc_len + 2 >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
            acc[acc_len++] = ')';
            acc[acc_len]   = '\0';

            wts_push(&s, acc, indent, lineno - 1);
            free(acc);
            continue;
        }

        /* Detect: "define name :: T -> ... -> Ret" type-signature style.
         * Also handles: "define name -> Ret" (zero-param function).
         * Also handles: "define name [p :: T] [p :: T] -> Ret" bracketed params. */
        {
            const char *sig = t;
            if (strncmp(sig, "define", 6) == 0 &&
                (sig[6] == ' ' || sig[6] == '\t')) {
                const char *after_define = sig + 6;
                while (*after_define == ' ' || *after_define == '\t') after_define++;
                const char *name_end = after_define;
                if (*name_end == '[') {
                    /* bracketed binding [name :: T] — scan to matching ] */
                    int depth = 0;
                    while (*name_end) {
                        if (*name_end == '[') depth++;
                        else if (*name_end == ']') { depth--; name_end++; if (!depth) break; continue; }
                        name_end++;
                    }
                } else {
                    while (*name_end && *name_end != ' ' && *name_end != '\t') name_end++;
                }
                const char *after_name = name_end;
                while (*after_name == ' ' || *after_name == '\t') after_name++;

                /* Case 1: define name :: T -> ... -> Ret */
                if (after_name[0] == ':' && after_name[1] == ':' &&
                    strstr(after_name + 2, "->")) {
                    size_t name_len = name_end - after_define;
                    char *fname = strndup(after_define, name_len);
                    const char *sig_rest = after_name + 2;
                    while (*sig_rest == ' ' || *sig_rest == '\t') sig_rest++;

                    int arr_count = 0;
                    int bdepth = 0;
                    bool in_str = false;
                    ParamKind sig_kinds[WISP_MAX_PARAMS];
                    memset(sig_kinds, PARAM_VALUE, sizeof(sig_kinds));
                    int sig_param = 0;
                    /* Walk the type signature tracking each param type.
                     * A param whose type contains '->' (inside parens/brackets)
                     * or is literally (a -> b) is a function param. */
                    const char *sp = sig_rest;
                    while (*sp && *sp != ';') {
                        /* skip spaces */
                        while (*sp == ' ' || *sp == '\t') sp++;
                        if (!*sp || *sp == ';') break;
                        /* detect top-level '->' separator */
                        if (*sp == '-' && *(sp+1) == '>') {
                            arr_count++;
                            sp += 2;
                            sig_param++;
                            continue;
                        }
                        /* consume one type token (may be bracketed) */
                        if (*sp == '(' || *sp == '[') {
                            char open = *sp, close = open == '(' ? ')' : ']';
                            int d = 0;
                            const char *tstart = sp;
                            bool has_arrow_inside = false;
                            while (*sp) {
                                if (*sp == open)  d++;
                                if (*sp == close) { d--; sp++; if (!d) break; continue; }
                                if (d == 1 && *sp == '-' && *(sp+1) == '>') has_arrow_inside = true;
                                sp++;
                            }
                            /* This bracketed type is a param if a top-level ->
                             * follows it — we'll mark it after seeing the arrow.
                             * Store tentatively: mark previous slot if arrow seen. */
                            (void)tstart;
                            if (has_arrow_inside && sig_param < WISP_MAX_PARAMS)
                                sig_kinds[sig_param] = PARAM_FUNC;
                        } else {
                            /* plain word type */
                            while (*sp && *sp != ' ' && *sp != '\t' && *sp != ';' &&
                                   !(*sp == '-' && *(sp+1) == '>')) sp++;
                        }
                    }
                    arity_set_with_kinds(at, fname, arr_count, sig_kinds);

                    size_t siglen = strlen(sig_rest);
                    size_t hlen = 1 + name_len + 1 + siglen + 1 + 1;
                    char *header = malloc(hlen);
                    snprintf(header, hlen, "(%s %s)", fname, sig_rest);
                    free(fname);
                    wts_push(&s, "define", indent, lineno);
                    wts_push(&s, header, indent, lineno);
                    free(header);
                    free(raw);
                    lineno++;
                    continue;
                }

                /* Case 2: define name -> Ret  (zero-param, return type only) */
                if (after_name[0] == '-' && after_name[1] == '>') {
                    size_t name_len = name_end - after_define;
                    char *fname = strndup(after_define, name_len);
                    arity_set(at, fname, 0);
                    const char *ret_start = after_name + 2;
                    while (*ret_start == ' ' || *ret_start == '\t') ret_start++;
                    const char *ret_end = ret_start;
                    while (*ret_end && *ret_end != ' ' && *ret_end != '\t' &&
                           *ret_end != ';') ret_end++;
                    size_t ret_len = ret_end - ret_start;
                    /* Build "(fname -> RetType)" */
                    size_t hlen = 1 + name_len + 4 + ret_len + 1 + 1;
                    char *header = malloc(hlen);
                    snprintf(header, hlen, "(%s -> %.*s)", fname,
                             (int)ret_len, ret_start);
                    free(fname);
                    wts_push(&s, "define", indent, lineno);
                    wts_push(&s, header, indent, lineno);
                    free(header);
                    free(raw);
                    lineno++;
                    continue;
                }

                /* Case 3: define name [p :: T] ... [p :: T] -> Ret
                 * The first non-space token after the name starts with '['.
                 * Only treat as function header if the bracket contains '::' or '->'
                 * (i.e. it's a typed parameter, not an array literal value). */
                if (after_name[0] == '[' && (strstr(after_name, "::") || strstr(after_name, "->"))) {
                    size_t name_len = name_end - after_define;
                    char *fname = strndup(after_define, name_len);

                    /* Scan to end of line (strip inline comment) */
                    const char *line_end = after_name;
                    bool in_str = false;
                    while (*line_end) {
                        if (in_str) {
                            if (*line_end == '\\') line_end++;
                            else if (*line_end == '"') in_str = false;
                            line_end++; continue;
                        }
                        if (*line_end == '"') { in_str = true; line_end++; continue; }
                        if (*line_end == ';') break;
                        line_end++;
                    }
                    /* trim trailing whitespace */
                    while (line_end > after_name &&
                           (*(line_end-1) == ' ' || *(line_end-1) == '\t'))
                        line_end--;

                    int arr_count = 0;
                    int bdepth = 0;
                    for (const char *p = after_name; p < line_end; p++) {
                        if (*p == '[' || *p == '(' || *p == '{') bdepth++;
                        else if (*p == ']' || *p == ')' || *p == '}') bdepth--;
                        else if (bdepth == 0 && *p == '-' && *(p+1) == '>') arr_count++;
                    }
                    arity_set(at, fname, arr_count);

                    size_t rest_len = line_end - after_name;
                    /* Build "(fname [p :: T] ... -> Ret)" */
                    size_t hlen = 1 + name_len + 1 + rest_len + 1 + 1;
                    char *header = malloc(hlen);
                    snprintf(header, hlen, "(%s %.*s)", fname,
                             (int)rest_len, after_name);
                    free(fname);
                    wts_push(&s, "define", indent, lineno);
                    wts_push(&s, header, indent, lineno);
                    free(header);
                    free(raw);
                    lineno++;
                    continue;
                }
            }
        }

        tokenise_into(at, &s, t, indent, lineno);
        free(raw);
        lineno++;
    }
    return s;
}

/// Recursive arity-driven parser

/*
 * Parse one complete expression from the token stream.
 * parent_indent: the indent level of the enclosing variadic form,
 *   used to stop variadic consumption. -1 means top level.
 */
static void wisp_parse_expr(ArityTable *t, WTokenStream *s, SB *out, int parent_indent, int parent_remaining) {
    if (s->pos >= s->count) return;

    WToken *tok = &s->tokens[s->pos];
    const char *text = tok->text;
    int my_indent = tok->indent;

    /* Emit a #line directive so the reader maps errors back to original source.
     * Do NOT emit one if we are mid-form (parent_remaining > 0 and this is
     * not a top-level token) to avoid injecting #line inside define headers. */
    if (parent_remaining == 0) {
        char linedir[64];
        snprintf(linedir, sizeof(linedir), "(#line %d %d)", tok->lineno, 1);
        sb_puts(out, linedir);
        sb_putc(out, ' ');
    }

    /* Already a grouped s-expression — emit verbatim */
    if (text[0] == '(' || text[0] == '[' || text[0] == '{' ||
        (text[0] == '#' && text[1] == '{')) {
        sb_puts(out, text);
        s->pos++;
        return;
    }

    int arity = arity_get(t, text);
    s->pos++;

    /* Atom with no args */
    if (arity == 0) {
        sb_putc(out, '(');
        sb_puts(out, text);
        sb_putc(out, ')');
        return;
    }

    if (arity == -2) {
        /* Unknown arity — emit as bare atom */
        sb_puts(out, text);
        return;
    }

    /* Fixed or variadic — open a form */
    sb_putc(out, '(');
    sb_puts(out, text);

    if (arity > 0) {
        /* Fixed arity: consume exactly N args regardless of indentation */
        ArityEntry *entry = arity_get_entry(t, text);
        bool _define_is_func = false;
        if (strcmp(text, "define") == 0 && s->pos < s->count) {
            const char *first_arg = s->tokens[s->pos].text;
            _define_is_func = (first_arg[0] == '(');
        }
        /* These forms take bare name arguments, never function calls */
        bool args_are_bare = (strcmp(text, "import") == 0 ||
                              strcmp(text, "module") == 0);
        for (int i = 0; i < arity && s->pos < s->count; i++) {
            sb_putc(out, ' ');
            ParamKind kind = (entry && i < WISP_MAX_PARAMS)
                           ? entry->param_kinds[i] : PARAM_VALUE;
            if (kind == PARAM_FUNC || args_are_bare) {
                /* Emit bare -- slot expects a name/value, not a call result */
                WToken *arg = &s->tokens[s->pos];
                sb_puts(out, arg->text);
                s->pos++;
            } else {
                /* Last-argument infix promotion:
                 * When filling the last slot of a fixed-arity call, peek
                 * past the next atom. If a known function follows on the
                 * same line, the programmer intends the whole infix/postfix
                 * chain as the argument, not just the bare atom.
                 *
                 * Example: show Red = Red   =>  (show (= Red Red))
                 *          show Red != Green =>  (show (!= Red Green))
                 *          foo x + y         =>  (foo (+ x y))
                 *
                 * Algorithm (left-associative chain):
                 *   1. Must be the last argument slot.
                 *   2. Next token is a bare atom on the same line as head.
                 *   3. Token after that is a known function on the same line.
                 *   4. Consume: lhs fn rhs fn2 rhs2 ... building left-assoc tree.
                 */
                bool is_last_arg = (i == arity - 1);
                bool did_infix = false;
                if (is_last_arg && s->pos + 1 < s->count) {
                    WToken *cand_lhs = &s->tokens[s->pos];
                    WToken *cand_fn  = &s->tokens[s->pos + 1];
                    bool lhs_is_atom = (cand_lhs->text[0] != '('
                                        && cand_lhs->text[0] != '['
                                        && cand_lhs->text[0] != '{');
                    bool fn_same_line = (cand_fn->lineno == tok->lineno);
                    bool fn_is_atom   = (cand_fn->text[0] != '('
                                         && cand_fn->text[0] != '['
                                         && cand_fn->text[0] != '{');
                    int  fn_arity     = arity_get(t, cand_fn->text);
                    bool fn_known     = (fn_arity != -2);
                    if (lhs_is_atom && fn_same_line && fn_is_atom && fn_known) {
                        /* Build left-associative infix chain on this line.
                         * Start with lhs as the accumulator string. */
                        char *accum = strdup(cand_lhs->text);
                        s->pos++; /* consume lhs */
                        while (s->pos < s->count) {
                            WToken *op_tok = &s->tokens[s->pos];
                            if (op_tok->lineno != tok->lineno) break;
                            if (op_tok->text[0] == '('
                                || op_tok->text[0] == '['
                                || op_tok->text[0] == '{') break;
                            int op_ar = arity_get(t, op_tok->text);
                            if (op_ar == -2) break; /* unknown, stop */
                            char *op_name = strdup(op_tok->text);
                            s->pos++; /* consume op */
                            /* Collect rhs tokens: exactly (op_ar - 1) args
                             * since one slot is filled by accum.
                             * Each rhs arg may itself be a grouped token. */
                            /* For variadic ops (op_ar == -1) or unknown,
                             * consume all remaining atoms on the same line.
                             * For fixed arity N, consume N-1 rhs tokens
                             * (one slot is already filled by accum/lhs).   */
                            int rhs_count = (op_ar > 0) ? (op_ar - 1)
                                          : -1; /* -1 = consume rest of line */
                            /* Build the new accumulator: (op accum rhs...) */
                            SB next; sb_init(&next);
                            sb_putc(&next, '(');
                            sb_puts(&next, op_name);
                            sb_putc(&next, ' ');
                            sb_puts(&next, accum);
                            free(op_name);
                            free(accum);
                            if (rhs_count == -1) {
                                /* Variadic: consume all remaining atoms on line */
                                while (s->pos < s->count) {
                                    WToken *rhs_tok = &s->tokens[s->pos];
                                    if (rhs_tok->lineno != tok->lineno) break;
                                    sb_putc(&next, ' ');
                                    sb_puts(&next, rhs_tok->text);
                                    s->pos++;
                                }
                            } else {
                                for (int r = 0; r < rhs_count && s->pos < s->count; r++) {
                                    WToken *rhs_tok = &s->tokens[s->pos];
                                    if (rhs_tok->lineno != tok->lineno) break;
                                    sb_putc(&next, ' ');
                                    sb_puts(&next, rhs_tok->text);
                                    s->pos++;
                                }
                            }
                            sb_putc(&next, ')');
                            accum = sb_take(&next);
                        }
                        sb_puts(out, accum);
                        free(accum);
                        did_infix = true;
                    }
                }
                if (!did_infix) {
                    wisp_parse_expr(t, s, out, my_indent, arity - i - 1);
                }
            }
        }
        /* For function defines only: consume all deeper-indented body lines. */
        if (strcmp(text, "define") == 0 && _define_is_func) {
            while (s->pos < s->count &&
                   s->tokens[s->pos].indent > my_indent) {
                sb_putc(out, '\n');
                wisp_parse_expr(t, s, out, my_indent, 0);
            }
        }
    } else {
        /* Variadic (-1):
         * - consume tokens on the same line as the head (same lineno)
         * - then consume subsequent lines indented strictly deeper
         *   than the head's own indent level
         *
         * Infix promotion applies here too: when the token after the
         * next atom is a known function on the same line, the whole
         * remaining same-line sequence is one infix expression.
         * Example: not x = y  =>  (not (= x y))
         *   'not' is variadic, consumes same-line tokens.
         *   Before consuming 'x', we peek at 'x+1' = '=' which is known.
         *   So we build (= x y) as a single child instead of x and = y.
         */
        int my_lineno = tok->lineno;
        while (s->pos < s->count) {
            WToken *next = &s->tokens[s->pos];
            bool same_line  = (next->lineno == my_lineno);
            bool deeper     = (next->indent > my_indent &&
                               next->lineno != my_lineno);
            if (!same_line && !deeper) break;

            sb_putc(out, same_line ? ' ' : '\n');

            /* Infix promotion for variadic bodies:
             * If current token is a bare atom AND the token after it
             * is a known function on the same line, consume the whole
             * infix chain as one compound expression.                */
            bool promoted = false;
            if (s->pos + 1 < s->count) {
                WToken *cand_lhs = &s->tokens[s->pos];
                WToken *cand_op  = &s->tokens[s->pos + 1];
                bool lhs_is_atom = (cand_lhs->text[0] != '('
                                    && cand_lhs->text[0] != '['
                                    && cand_lhs->text[0] != '{');
                bool op_same_line = (cand_op->lineno == my_lineno);
                bool op_is_atom   = (cand_op->text[0] != '('
                                     && cand_op->text[0] != '['
                                     && cand_op->text[0] != '{');
                int  op_arity     = arity_get(t, cand_op->text);
                bool op_known     = (op_arity != -2);
                if (lhs_is_atom && op_same_line && op_is_atom && op_known) {
                    /* Consume left-associative infix chain */
                    char *accum = strdup(cand_lhs->text);
                    s->pos++; /* consume lhs */
                    while (s->pos < s->count) {
                        WToken *op_tok2  = &s->tokens[s->pos];
                        if (op_tok2->lineno != my_lineno) break;
                        if (op_tok2->text[0]=='('||
                            op_tok2->text[0]=='['||
                            op_tok2->text[0]=='{') break;
                        int oa = arity_get(t, op_tok2->text);
                        if (oa == -2) break;
                        char *op_name = strdup(op_tok2->text);
                        s->pos++; /* consume op */
                        SB next_acc; sb_init(&next_acc);
                        sb_putc(&next_acc, '(');
                        sb_puts(&next_acc, op_name);
                        sb_putc(&next_acc, ' ');
                        sb_puts(&next_acc, accum);
                        free(op_name); free(accum);
                        /* consume rhs tokens */
                        if (oa == -1) {
                            /* variadic op: consume rest of line */
                            while (s->pos < s->count &&
                                   s->tokens[s->pos].lineno == my_lineno) {
                                sb_putc(&next_acc, ' ');
                                sb_puts(&next_acc, s->tokens[s->pos].text);
                                s->pos++;
                            }
                        } else {
                            /* fixed arity: consume op_arity-1 rhs tokens */
                            for (int r = 0; r < oa - 1 && s->pos < s->count; r++) {
                                if (s->tokens[s->pos].lineno != my_lineno) break;
                                sb_putc(&next_acc, ' ');
                                sb_puts(&next_acc, s->tokens[s->pos].text);
                                s->pos++;
                            }
                        }
                        sb_putc(&next_acc, ')');
                        accum = sb_take(&next_acc);
                    }
                    sb_puts(out, accum);
                    free(accum);
                    promoted = true;
                }
            }
            if (!promoted) {
                wisp_parse_expr(t, s, out, my_indent, 0);
            }
        }
    }

    sb_putc(out, ')');
}

/// Main entry point

void wisp_register_arities_from_env(Env *env) {
    for (size_t i = 0; i < env->size; i++) {
        for (EnvEntry *e = env->buckets[i]; e; e = e->next) {
            if (e->kind != ENV_BUILTIN && e->kind != ENV_FUNC) continue;
            int arity;
            if (e->arity_max == -1) {
                arity = -1;
            } else if (e->arity_max == 0) {
                arity = e->arity_min;
            } else {
                arity = e->arity_max;
            }
            if (!g_ffi_arities_init) {
                memset(&g_ffi_arities, 0, sizeof(g_ffi_arities));
                g_ffi_arities_init = true;
            }
            arity_set_with_kinds(&g_ffi_arities, e->name, arity, e->param_kinds);
        }
    }
}

/* Strip -| ... |- and paragraph comments from source, replacing
 * comment content with spaces/newlines to preserve line numbers. */
static char *strip_comments(const char *source) {
    comment_map_build(source);
    int len = (int)strlen(source);
    char *out = malloc(len + 1);
    memcpy(out, source, len + 1);

    for (int i = 0; i < g_comment_count; i++) {
        CommentSpan *span = &g_comment_spans[i];
        int start = span->open_pos;
        int end   = span->close_pos >= 0
                  ? span->close_pos + 2
                  : span->para_end;

        /* Check if this is a single-line inline comment */
        bool is_single_line = true;
        for (int j = start; j < end && j < len; j++) {
            if (source[j] == '\n') { is_single_line = false; break; }
        }

        if (is_single_line) {
            /* Inline comment — remove entirely by shifting remaining content left */
            int span_len = end - start;
            memmove(out + start, out + end, len - end + 1);
            len -= span_len;
            /* Adjust all subsequent span positions */
            for (int k = i + 1; k < g_comment_count; k++) {
                if (g_comment_spans[k].open_pos > start)
                    g_comment_spans[k].open_pos -= span_len;
                if (g_comment_spans[k].close_pos > start)
                    g_comment_spans[k].close_pos -= span_len;
                if (g_comment_spans[k].para_end > start)
                    g_comment_spans[k].para_end -= span_len;
            }
        } else {
            /* Multiline comment — replace with spaces/newlines to preserve line numbers */
            for (int j = start; j < end && j < len; j++)
                out[j] = (source[j] == '\n') ? '\n' : ' ';
        }
    }

    return out;
}

static int wisp_param_kind_is_func(const char *func_name, int arg_index) {
    if (!g_ffi_arities_init) return 0;
    ArityEntry *e = arity_get_entry(&g_ffi_arities, func_name);
    if (!e) return 0;
    if (arg_index < 0 || arg_index >= WISP_MAX_PARAMS) return 0;
    return e->param_kinds[arg_index] == PARAM_FUNC ? 1 : 0;
}

static int wisp_is_known_function(const char *name) {
    if (!g_ffi_arities_init) return 0;
    return arity_get_entry(&g_ffi_arities, name) != NULL ? 1 : 0;
}

ASTList wisp_parse_all(const char *source, const char *filename) {
    /* Strip comments first, preserving line structure */
    char *stripped = strip_comments(source);

    /* Check if file has any wisp-style lines at all */
    bool has_wisp = false;
    const char *p = stripped;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        char c = *p;
        if (c && c != '\n' && c != ';' && c != '(' && c != '[' && c != '{' && c != '"')
            { has_wisp = true; break; }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    if (!has_wisp) {
        parser_set_context(filename, stripped);
        ASTList result = parse_all(stripped);
        result = macro_expand_all(result.exprs, result.count);
        free(stripped);
        return result;
    }

    /* Build arity table */
    ArityTable t;
    memset(&t, 0, sizeof(t));
    if (g_ffi_arities_init)
        for (int i = 0; i < ARITY_BUCKETS; i++)
            for (ArityEntry *e = g_ffi_arities.buckets[i]; e; e = e->next)
                arity_set_with_kinds(&t, e->name, e->arity, e->param_kinds);
    arity_prescan(&t, stripped);

    /* Build flat token stream */
    WTokenStream s = build_token_stream(stripped, &t);

    /* Parse all top-level expressions */
    SB out; sb_init(&out);
    bool first = true;
    while (s.pos < s.count) {
        if (!first) sb_putc(&out, '\n');
        first = false;
        wisp_parse_expr(&t, &s, &out, -1, 0);
    }

    char *transformed = sb_take(&out);

    fprintf(stderr, "\n=== wisp expanded (%s) ===\n%s\n=== end ===\n\n",
            filename ? filename : "<input>", transformed);

    /* Install param-kind hook so reader.c can do automatic infix detection */
    g_param_kind_is_func = wisp_param_kind_is_func;
    g_is_known_function  = wisp_is_known_function;

    parser_set_context(filename, transformed);
    ASTList result = parse_all(transformed);
    result = macro_expand_all(result.exprs, result.count);

    g_param_kind_is_func = NULL;
    g_is_known_function  = NULL;

    free(transformed);
    free(stripped);
    wts_free(&s);

    // Persist all discovered arities back into g_ffi_arities so that
    // modules imported later inherit everything this module knew about,
    // including C functions from its includes.
    if (!g_ffi_arities_init) {
        memset(&g_ffi_arities, 0, sizeof(g_ffi_arities));
        g_ffi_arities_init = true;
    }
    for (int i = 0; i < ARITY_BUCKETS; i++)
        for (ArityEntry *e = t.buckets[i]; e; e = e->next)
            arity_set_with_kinds(&g_ffi_arities, e->name, e->arity, e->param_kinds);

    arity_free(&t);


    return result;
}
