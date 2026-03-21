#include "wisp.h"
#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/// Arity table

typedef struct ArityEntry {
    char *name;
    int   arity;
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
    e->next  = t->buckets[h];
    t->buckets[h] = e;
}

static int arity_get(ArityTable *t, const char *name) {
    unsigned int h = arity_hash(name);
    for (ArityEntry *e = t->buckets[h]; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e->arity;
    return -2; /* unknown */
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

/* Tokenise one line into the stream */
static void tokenise_into(WTokenStream *s, const char *line,
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
                if (*p == open)  depth++;
                if (*p == close) { depth--; p++; if (!depth) break; continue; }
                p++;
            }
            char *tok = strndup(start, p - start);
            wts_push(s, tok, indent, lineno);
            free(tok);
            continue;
        }

        /* backtick infix: atom `op` atom → keep backticks, group as
         * (lhs `op` rhs) so reader.c's infix logic handles it */
        if (*p == '`') {
            const char *bt_start = p++;
            while (*p && *p != '`') p++;
            if (*p == '`') p++;
            while (*p == ' ' || *p == '\t') p++;
            const char *rhs_start = p;
            if (*p == '"') {
                p++;
                while (*p && !(*p == '"' && *(p-1) != '\\')) p++;
                if (*p == '"') p++;
            } else {
                while (*p && *p != ' ' && *p != '\t' && *p != ';') p++;
            }
            if (s->count > 0 && s->tokens[s->count-1].lineno == lineno) {
                char *lhs = s->tokens[s->count-1].text;
                /* extract op name between backticks */
                const char *op_s = bt_start + 1;
                const char *op_e = memchr(op_s, '`', p - op_s);
                if (!op_e) op_e = p;
                /* trim trailing spaces */
                while (op_e > op_s && *(op_e-1) == ' ') op_e--;
                char op[64]; size_t op_len = op_e - op_s;
                if (op_len >= sizeof(op)) op_len = sizeof(op)-1;
                memcpy(op, op_s, op_len); op[op_len] = '\0';
                char *rhs = strndup(rhs_start, p - rhs_start);
                /* preserve backticks so reader.c infix logic fires */
                size_t tlen = 1 + strlen(lhs) + 3 + op_len + 3 + strlen(rhs) + 1 + 1;
                char *grouped = malloc(tlen);
                snprintf(grouped, tlen, "(%s `%s` %s)", lhs, op, rhs);
                free(s->tokens[s->count-1].text);
                s->tokens[s->count-1].text = grouped;
                free(rhs);
            }
            continue;
        }

        /* regular token */
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ';') p++;
        char *tok = strndup(start, p - start);
        wts_push(s, tok, indent, lineno);
        free(tok);
    }
}

// Build token stream from source.
// Multi-line grouped expressions (starting with '(' '[' '{') are
// accumulated across lines and emitted as a single token.
static WTokenStream build_token_stream(const char *source) {
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
                if (*q=='('||*q=='['||*q=='{') depth++;
                if (*q==')'||*q==']'||*q=='}') depth--;
            }

            if (depth == 0) {
                /* Already balanced on one line — normal tokenise */
                tokenise_into(&s, t, indent, lineno);
                free(raw);
                lineno++;
                continue;
            }

            /* Unbalanced — accumulate lines until balanced */
            size_t acc_cap = 256;
            char *acc = malloc(acc_cap);
            size_t acc_len = 0;

            /* copy first line */
            size_t tlen = strlen(t);
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

                /* append a space + trimmed line to accumulator */
                size_t ltlen = strlen(lt);
                while (acc_len + ltlen + 3 >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
                acc[acc_len++] = ' ';
                memcpy(acc + acc_len, lt, ltlen);
                acc_len += ltlen;
                acc[acc_len] = '\0';

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

            /* emit the entire multi-line group as one token */
            wts_push(&s, acc, indent, lineno - 1);
            free(acc);
            continue;
        }

        /* Normal non-grouped line */
        tokenise_into(&s, t, indent, lineno);
        free(raw);
        lineno++;
    }
    return s;
}

/// Recursive arity-driven parser

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} SB;

static void sb_init(SB *b) { b->data = malloc(256); b->data[0]='\0'; b->len=0; b->cap=256; }
static void sb_free(SB *b) { free(b->data); }
static char *sb_take(SB *b) { char *r = b->data; b->data = NULL; return r; }

static void sb_putc(SB *b, char c) {
    if (b->len + 1 >= b->cap) { b->cap *= 2; b->data = realloc(b->data, b->cap); }
    b->data[b->len++] = c;
    b->data[b->len]   = '\0';
}

static void sb_puts(SB *b, const char *s) {
    size_t l = strlen(s);
    while (b->len + l + 1 >= b->cap) { b->cap *= 2; b->data = realloc(b->data, b->cap); }
    memcpy(b->data + b->len, s, l + 1);
    b->len += l;
}

// Forward declaration
static void wisp_parse_expr(ArityTable *t, WTokenStream *s, SB *out, int parent_indent);

/*
 * Parse one complete expression from the token stream.
 * parent_indent: the indent level of the enclosing variadic form,
 *   used to stop variadic consumption. -1 means top level.
 */
static void wisp_parse_expr(ArityTable *t, WTokenStream *s, SB *out, int parent_indent) {
    if (s->pos >= s->count) return;

    WToken *tok = &s->tokens[s->pos];
    const char *text = tok->text;
    int my_indent = tok->indent;

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
        for (int i = 0; i < arity && s->pos < s->count; i++) {
            sb_putc(out, ' ');
            wisp_parse_expr(t, s, out, my_indent);
        }
        /* Special case: define may have an optional docstring as the
         * next token — consume it unconditionally if it's a string,
         * regardless of indentation or line position. */
        if (strcmp(text, "define") == 0 &&
            s->pos < s->count &&
            s->tokens[s->pos].text[0] == '"') {
            sb_putc(out, '\n');
            wisp_parse_expr(t, s, out, my_indent);
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
            wisp_parse_expr(t, s, out, my_indent);
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
                /* variadic */
                arity = -1;
            } else if (e->arity_max == 0) {
                /* sentinel: fixed arity = arity_min */
                arity = e->arity_min;
            } else {
                /* fixed arity = arity_max */
                arity = e->arity_max;
            }
            wisp_register_arity(e->name, arity);
        }
    }
}

ASTList wisp_parse_all(const char *source, const char *filename) {
    /* Check if file has any wisp-style lines at all */
    bool has_wisp = false;
    const char *p = source;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        char c = *p;
        if (c && c != '\n' && c != ';' && c != '(' && c != '[' && c != '{' && c != '"')
            { has_wisp = true; break; }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    if (!has_wisp)
        return parse_all(source);

    /* Build arity table */
    ArityTable t;
    memset(&t, 0, sizeof(t));
    arity_prescan(&t, source);
    if (g_ffi_arities_init)
        for (int i = 0; i < ARITY_BUCKETS; i++)
            for (ArityEntry *e = g_ffi_arities.buckets[i]; e; e = e->next)
                arity_set(&t, e->name, e->arity);

    /* Build flat token stream */
    WTokenStream s = build_token_stream(source);

    /* Parse all top-level expressions */
    SB out; sb_init(&out);
    bool first = true;
    while (s.pos < s.count) {
        if (!first) sb_putc(&out, '\n');
        first = false;
        wisp_parse_expr(&t, &s, &out, -1);
    }

    char *transformed = sb_take(&out);

    fprintf(stderr, "\n=== wisp expanded (%s) ===\n%s\n=== end ===\n\n",
            filename ? filename : "<input>", transformed);

    parser_set_context(filename, transformed);
    ASTList result = parse_all(transformed);

    free(transformed);
    wts_free(&s);
    arity_free(&t);

    return result;
}
