#include "wisp.h"
#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/// Arity table

typedef struct ArityEntry {
    char      *name;
    int        arity;
    ParamKind  param_kinds[WISP_MAX_PARAMS];
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
                        if (tok.type == TOK_SYMBOL && strcmp(tok.value, "::") != 0)
                            arity++;
                        free(tok.value);
                    }
                    if (found_arrow) {
                        /* skip just the return type token */
                        tok = lexer_next_token(&lex);
                        free(tok.value);
                        arity_set(t, fname, arity);
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
            arity_set(t, fname, arity);
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
            wts_push(s, tok, indent, lineno);
            free(tok);
            continue;
        }

        /* regular token */
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ';') p++;
        char *tok = strndup(start, p - start);

        /* Peek ahead: if this token is followed by '->' on the same line,
         * group the entire 'pattern -> body' as a single token so pmatch
         * clauses survive wisp expansion intact.                          */
        {
            const char *peek = p;
            while (*peek == ' ' || *peek == '\t') peek++;
            if (peek > p && /* space must exist between token and '->' */
                strncmp(peek, "->", 2) == 0 &&
                (peek[2] == ' ' || peek[2] == '\t' || peek[2] == '\0')) {
                /* consume '->' */
                peek += 2;
                while (*peek == ' ' || *peek == '\t') peek++;
                /* consume the rest of the line as body */
                const char *body_start = peek;
                const char *body_end   = peek;
                while (*body_end && *body_end != ';') body_end++;
                /* trim trailing whitespace */
                while (body_end > body_start &&
                       (*(body_end-1) == ' ' || *(body_end-1) == '\t'))
                    body_end--;
                if (body_end > body_start) {
                    char *body_raw = strndup(body_start, body_end - body_start);
                    /* Run the body through wisp expansion so that
                     * e.g. "count x" becomes "(count x)"           */
                    WTokenStream body_ts = {0};
                    tokenise_into(t, &body_ts, body_raw, indent, lineno);
                    SB body_sb; sb_init(&body_sb);
                    while (body_ts.pos < body_ts.count)
                        wisp_parse_expr(t, &body_ts, &body_sb, -1, 0);
                    char *body_expanded = sb_take(&body_sb);
                    wts_free(&body_ts);
                    free(body_raw);
                    /* build "tok -> body_expanded" as a single token */
                    size_t tlen = strlen(tok) + 4 + strlen(body_expanded) + 1;
                    char *clause = malloc(tlen);
                    snprintf(clause, tlen, "%s -> %s", tok, body_expanded);
                    wts_push(s, clause, indent, lineno);
                    free(clause);
                    free(body_expanded);
                    free(tok);
                    p = body_end;
                    continue;
                }
            }
        }

        /* Peek ahead: if next non-space token is '::',
         * group entire chain as [tok :: T1 :: T2 :: ...] */
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

                /* consume one token (type name, number, or bracketed group) */
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

        /* Check if this line starts a grouped expression */
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
                /* Check for pmatch clause starting with a grouped pattern:
                 * "[...] -> body" — group the whole thing as one token.  */
                {
                    /* find end of the grouped pattern token */
                    const char *after_pat = t;
                    /* skip the grouped token */
                    if (*after_pat == '[' || *after_pat == '(') {
                        char open  = *after_pat;
                        char close = open == '[' ? ']' : ')';
                        int d = 0;
                        while (*after_pat) {
                            if (*after_pat == open)  d++;
                            if (*after_pat == close) { d--; after_pat++; if (!d) break; continue; }
                            after_pat++;
                        }
                        /* skip spaces */
                        while (*after_pat == ' ' || *after_pat == '\t') after_pat++;
                        /* check for -> */
                        if (strncmp(after_pat, "->", 2) == 0 &&
                            (after_pat[2] == ' ' || after_pat[2] == '\t' || after_pat[2] == '\0')) {
                            /* consume '->' and the body */
                            const char *body_start = after_pat + 2;
                            while (*body_start == ' ' || *body_start == '\t') body_start++;
                            const char *body_end = body_start;
                            while (*body_end && *body_end != ';') body_end++;
                            while (body_end > body_start &&
                                   (*(body_end-1) == ' ' || *(body_end-1) == '\t'))
                                body_end--;
                            /* expand the body through wisp */
                            char *body_raw = strndup(body_start, body_end - body_start);
                            WTokenStream body_ts = {0};
                            tokenise_into(at, &body_ts, body_raw, indent, lineno);
                            SB body_sb; sb_init(&body_sb);
                            while (body_ts.pos < body_ts.count)
                                wisp_parse_expr(at, &body_ts, &body_sb, -1, 0);
                            char *body_expanded = sb_take(&body_sb);
                            wts_free(&body_ts);
                            free(body_raw);
                            /* build "pattern -> body" as single token */
                            size_t pat_len = after_pat - t;
                            /* remove trailing spaces from pattern */
                            while (pat_len > 0 && (t[pat_len-1] == ' ' || t[pat_len-1] == '\t'))
                                pat_len--;
                            char *pattern = strndup(t, pat_len);
                            size_t clen = pat_len + 4 + strlen(body_expanded) + 1;
                            char *clause = malloc(clen);
                            snprintf(clause, clen, "%s -> %s", pattern, body_expanded);
                            wts_push(&s, clause, indent, lineno);
                            free(clause);
                            free(pattern);
                            free(body_expanded);
                            free(raw);
                            lineno++;
                            continue;
                        }
                    }
                }
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
        if (strncmp(t, "data", 4) == 0 &&
            (t[4] == ' ' || t[4] == '\t' || t[4] == '\0')) {

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
                size_t ltlen = lt_end - lt;

                char _ld2[48];
                int _ld2len = snprintf(_ld2, sizeof(_ld2), " (#line %d 1) ", lineno);
                while (acc_len + (size_t)_ld2len + ltlen + 4 >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
                memcpy(acc + acc_len, _ld2, _ld2len);
                acc_len += _ld2len;
                memcpy(acc + acc_len, lt, ltlen);
                acc_len += ltlen;
                acc[acc_len] = '\0';
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
                while (*name_end && *name_end != ' ' && *name_end != '\t') name_end++;
                const char *after_name = name_end;
                while (*after_name == ' ' || *after_name == '\t') after_name++;

                /* Case 1: define name :: T -> ... -> Ret */
                if (after_name[0] == ':' && after_name[1] == ':' &&
                    strstr(after_name + 2, "->")) {
                    size_t name_len = name_end - after_define;
                    char *fname = strndup(after_define, name_len);
                    const char *sig_rest = after_name + 2;
                    while (*sig_rest == ' ' || *sig_rest == '\t') sig_rest++;
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
                 * Collect everything up to and including the final '-> Ret'
                 * and wrap as a single header token (fname [..] [..] -> Ret). */
                if (after_name[0] == '[') {
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

    /* Emit a #line directive so the reader maps errors back to original source */
    char linedir[64];
    snprintf(linedir, sizeof(linedir), "(#line %d %d)", tok->lineno, 1);
    sb_puts(out, linedir);
    sb_putc(out, ' ');

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
        for (int i = 0; i < arity && s->pos < s->count; i++) {
            sb_putc(out, ' ');
            ParamKind kind = (entry && i < WISP_MAX_PARAMS)
                           ? entry->param_kinds[i] : PARAM_VALUE;
            if (kind == PARAM_FUNC) {
                /* Emit bare — this slot expects a function value, not a call result */
                WToken *arg = &s->tokens[s->pos];
                sb_puts(out, arg->text);
                s->pos++;
            } else {
                wisp_parse_expr(t, s, out, my_indent, arity - i - 1);
            }
        }
        /* Special case: define may have optional metadata (docstring,
         * :keyword value pairs) before the body — consume them all. */
        if (strcmp(text, "define") == 0) {
            /* bare string docstring */
            if (s->pos < s->count &&
                s->tokens[s->pos].text[0] == '"' &&
                s->tokens[s->pos].indent > my_indent) {
                sb_putc(out, '\n');
                wisp_parse_expr(t, s, out, my_indent, 0);
            }
            /* :keyword value pairs */
            while (s->pos + 1 < s->count &&
                   s->tokens[s->pos].indent > my_indent &&
                   s->tokens[s->pos].text[0] == ':') {
                /* emit the :keyword */
                sb_putc(out, '\n');
                wisp_parse_expr(t, s, out, my_indent, 0);
                /* emit the value (must exist and be on same or deeper line) */
                if (s->pos < s->count &&
                    s->tokens[s->pos].indent > my_indent) {
                    sb_putc(out, ' ');
                    wisp_parse_expr(t, s, out, my_indent, 0);
                }
            }
            /* body expressions — consume all deeper-indented lines,
             * including pmatch clauses (tokens containing "->") and
             * plain expressions */
            while (s->pos < s->count &&
                   s->tokens[s->pos].indent > my_indent) {
                sb_putc(out, '\n');
                wisp_parse_expr(t, s, out, my_indent, 0);
            }
        }
        /* Special case: define with pmatch clauses.
         * After consuming the fixed arity args, if the next token on a
         * deeper-indented line starts a pmatch clause (is a pattern
         * followed by -> on the same line, i.e. contains "->"), consume
         * all deeper-indented lines as part of this define.            */
        if (strcmp(text, "define") == 0) {
            while (s->pos < s->count) {
                WToken *next = &s->tokens[s->pos];
                /* only consume lines strictly deeper than the define */
                if (next->indent <= my_indent) break;
                /* check if this token or line contains -> (pmatch clause) */
                bool is_pmatch_clause = (strstr(next->text, "->") != NULL);
                if (!is_pmatch_clause) break;
                sb_putc(out, '\n');
                sb_putc(out, '\n');
                wisp_parse_expr(t, s, out, my_indent, 0);
            }
        }
    } else {
        /* Variadic (-1):
         * - consume tokens on the same line as the head (same lineno)
         * - then consume subsequent lines indented strictly deeper
         *   than the head's own indent level */
        int my_lineno = tok->lineno;
        while (s->pos < s->count) {
            WToken *next = &s->tokens[s->pos];
            if (next->lineno == my_lineno) {
                /* same line as head — always consume */
                sb_putc(out, ' ');
            } else if (next->indent > my_indent) {
                /* deeper indented line — consume as child */
                sb_putc(out, '\n');
            } else {
                /* same or shallower indent on a different line — stop */
                break;
            }
            wisp_parse_expr(t, s, out, my_indent, 0);
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
        free(stripped);
        return result;
    }

    /* Build arity table */
    ArityTable t;
    memset(&t, 0, sizeof(t));
    arity_prescan(&t, stripped);
    if (g_ffi_arities_init)
        for (int i = 0; i < ARITY_BUCKETS; i++)
            for (ArityEntry *e = g_ffi_arities.buckets[i]; e; e = e->next)
                arity_set_with_kinds(&t, e->name, e->arity, e->param_kinds);

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

    g_param_kind_is_func = NULL;
    g_is_known_function  = NULL;

    free(transformed);
    free(stripped);
    wts_free(&s);
    arity_free(&t);

    return result;
}
