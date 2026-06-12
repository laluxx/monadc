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

        /* Bare 'data' symbol: data Name a = C1 T1 | C2 T2 */
        if (tok.type == TOK_SYMBOL && strcmp(tok.value, "data") == 0) {
            free(tok.value);
            tok = lexer_next_token(&lex);
            free(tok.value);

            bool in_ctors = false;
            while (true) {
                tok = lexer_next_token(&lex);
                if (tok.type == TOK_EOF || tok.type == TOK_RPAREN || tok.type == TOK_KEYWORD) break;
                if (tok.type == TOK_SYMBOL && (strcmp(tok.value, "define") == 0 || strcmp(tok.value, "class") == 0 || strcmp(tok.value, "data") == 0 || strcmp(tok.value, "layout") == 0)) break;
                if (tok.type == TOK_SYMBOL && strcmp(tok.value, "=") == 0) {
                    free(tok.value);
                    tok = lexer_next_token(&lex);
                    in_ctors = true;
                    break;
                }
                if (tok.type == TOK_SYMBOL && tok.value[0] >= 'A' && tok.value[0] <= 'Z') {
                    in_ctors = true;
                    break;
                }
                free(tok.value);
            }

            if (in_ctors) {
                while (tok.type == TOK_SYMBOL || tok.type == TOK_LPAREN || tok.type == TOK_LBRACKET) {
                    if (tok.type == TOK_SYMBOL && strcmp(tok.value, "deriving") == 0) { free(tok.value); break; }
                    if (tok.type == TOK_SYMBOL && strcmp(tok.value, "|") == 0) {
                        free(tok.value);
                        tok = lexer_next_token(&lex);
                        continue;
                    }
                    if (tok.type == TOK_SYMBOL && tok.value[0] >= 'A' && tok.value[0] <= 'Z') {
                        char *cname = strdup(tok.value);
                        free(tok.value);
                        int arity = 0;
                        while (true) {
                            tok = lexer_next_token(&lex);
                            if (tok.type == TOK_EOF || tok.type == TOK_RPAREN || tok.type == TOK_KEYWORD) break;
                            if (tok.type == TOK_SYMBOL && strcmp(tok.value, "|") == 0) break;
                            if (tok.type == TOK_SYMBOL && strcmp(tok.value, "deriving") == 0) break;
                            if (tok.type == TOK_SYMBOL && (strcmp(tok.value, "define") == 0 || strcmp(tok.value, "class") == 0 || strcmp(tok.value, "data") == 0 || strcmp(tok.value, "layout") == 0)) break;

                            if (tok.type == TOK_LPAREN || tok.type == TOK_LBRACKET) {
                                int depth = 1;
                                free(tok.value);
                                while (depth > 0) {
                                    tok = lexer_next_token(&lex);
                                    if (tok.type == TOK_EOF) break;
                                    if (tok.type == TOK_LPAREN || tok.type == TOK_LBRACKET) depth++;
                                    if (tok.type == TOK_RPAREN || tok.type == TOK_RBRACKET) depth--;
                                    free(tok.value);
                                }
                                arity++;
                                continue;
                            }
                            arity++;
                            free(tok.value);
                        }
                        arity_set(t, cname, arity);
                        free(cname);
                        continue;
                    }
                    free(tok.value);
                    tok = lexer_next_token(&lex);
                }
            }
            continue;
        }

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

        /* Bare 'class' symbol: class Name a where ... */
        if (tok.type == TOK_SYMBOL && strcmp(tok.value, "class") == 0) {
            free(tok.value);
            tok = lexer_next_token(&lex); // ClassName
            free(tok.value);
            tok = lexer_next_token(&lex); // type_var
            free(tok.value);
            Lexer saved = lex;
            tok = lexer_next_token(&lex); // where
            if (tok.type == TOK_SYMBOL && strcmp(tok.value, "where") == 0) {
                free(tok.value);
            } else {
                lex = saved;
                free(tok.value);
            }

            while (true) {
                saved = lex;
                tok = lexer_next_token(&lex);
                if (tok.type == TOK_EOF) {
                    lex = saved; free(tok.value); break;
                }
                if (tok.type == TOK_SYMBOL && strcmp(tok.value, "type") == 0) {
                    Lexer peek = lex;
                    Token pt = lexer_next_token(&peek);
                    if (pt.type == TOK_SYMBOL && pt.value[0] >= 'A' && pt.value[0] <= 'Z') {
                        free(pt.value);
                        free(tok.value);
                        tok = lexer_next_token(&lex); // consume name
                        free(tok.value);
                        Lexer peek2 = lex;
                        Token pt2 = lexer_next_token(&peek2);
                        if (pt2.type == TOK_SYMBOL && pt2.value[0] >= 'a' && pt2.value[0] <= 'z') {
                            lex = peek2; // consume type var
                        }
                        free(pt2.value);
                        continue;
                    }
                    free(pt.value);
                    lex = saved; free(tok.value); break; // break on top-level type
                }
                if (tok.type == TOK_SYMBOL &&
                    (strcmp(tok.value, "define") == 0 || strcmp(tok.value, "class") == 0 ||
                     strcmp(tok.value, "instance") == 0 || strcmp(tok.value, "data") == 0 ||
                     strcmp(tok.value, "layout") == 0)) {
                    lex = saved; free(tok.value); break;
                }

                char *mname = NULL;
                if (tok.type == TOK_LPAREN) {
                    Lexer peek = lex;
                    Token t1 = lexer_next_token(&peek); // =
                    Token t2 = lexer_next_token(&peek); // )
                    Token t3 = lexer_next_token(&peek); // ::
                    if (t1.type == TOK_SYMBOL && t2.type == TOK_RPAREN &&
                        t3.type == TOK_SYMBOL && strcmp(t3.value, "::") == 0) {
                        mname = strdup(t1.value);
                        lex = peek; // fast forward lexer
                    }
                    free(t1.value); free(t2.value); free(t3.value);
                    if (!mname) {
                        lex = saved; free(tok.value); break; // Default impl, break out
                    }
                } else if (tok.type == TOK_SYMBOL) {
                    mname = strdup(tok.value);
                    Lexer peek = lex;
                    Token t1 = lexer_next_token(&peek); // ::
                    if (t1.type == TOK_SYMBOL && strcmp(t1.value, "::") == 0) {
                        lex = peek;
                    } else {
                        free(mname);
                        mname = NULL;
                    }
                    free(t1.value);
                }

                free(tok.value);

                if (mname) {
                    int arity = 0;
                    while (true) {
                        Lexer next_saved = lex;
                        tok = lexer_next_token(&lex);
                        if (tok.type == TOK_EOF || tok.type == TOK_LPAREN) {
                            /* Skip over parenthesized types like (a -> m b)
                             * without counting inner arrows as top-level params */
                            if (tok.type == TOK_LPAREN) {
                                free(tok.value);
                                int depth = 1;
                                while (depth > 0) {
                                    tok = lexer_next_token(&lex);
                                    if (tok.type == TOK_EOF) { depth = 0; free(tok.value); break; }
                                    if (tok.type == TOK_LPAREN) depth++;
                                    if (tok.type == TOK_RPAREN) depth--;
                                    free(tok.value);
                                }
                                continue;
                            }
                            lex = next_saved; free(tok.value); break;
                        }
                        if (tok.type == TOK_SYMBOL &&
                            (strcmp(tok.value, "define") == 0 || strcmp(tok.value, "class") == 0 ||
                             strcmp(tok.value, "instance") == 0 || strcmp(tok.value, "data") == 0 ||
                             strcmp(tok.value, "layout") == 0 || strcmp(tok.value, "type") == 0)) {
                            lex = next_saved; free(tok.value); break;
                        }
                        if (tok.type == TOK_ARROW ||
                            (tok.type == TOK_SYMBOL && strcmp(tok.value, "->") == 0)) {
                            arity++;
                            free(tok.value);
                            continue;
                        }
                        if (tok.type == TOK_SYMBOL) {
                            Lexer peek = lex;
                            Token ptok = lexer_next_token(&peek);
                            if (ptok.type == TOK_SYMBOL && strcmp(ptok.value, "::") == 0) {
                                free(ptok.value); free(tok.value);
                                lex = next_saved;
                                break;
                            }
                            free(ptok.value);
                        }
                        free(tok.value);
                    }
                    arity_set(t, mname, arity);
                    free(mname);
                }
            }
            continue;
        }

        /* Bare wisp-style: define name :: T -> ... -> Ret */
        /* fprintf(stderr, "DEBUG prescan tok: type=%d val='%s'\n", tok.type, tok.value ? tok.value : "NULL"); */
        if (tok.type == TOK_SYMBOL &&
            (strcmp(tok.value, "define") == 0 || strcmp(tok.value, "def") == 0)) {
            int def_line = tok.line;
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

                    /* Parse full signature until newline or next top-level form */
                    while (true) {
                        /* Peek to check if we hit the next definition/form */
                        Lexer peek = lex;
                        Token ptok = lexer_next_token(&peek);
                        if (ptok.type == TOK_EOF || ptok.line > def_line ||
                            (ptok.type == TOK_SYMBOL && ptok.value &&
                             (strcmp(ptok.value, "define") == 0 || strcmp(ptok.value, "def") == 0 ||
                              strcmp(ptok.value, "layout") == 0 || strcmp(ptok.value, "data") == 0 ||
                              strcmp(ptok.value, "class") == 0 || strcmp(ptok.value, "instance") == 0))) {
                            free(ptok.value);
                            break;
                        }
                        free(ptok.value);

                        tok = lexer_next_token(&lex);
                        if (tok.type == TOK_EOF) { free(tok.value); break; }

                        if (tok.type == TOK_ARROW || (tok.type == TOK_SYMBOL && strcmp(tok.value, "->") == 0)) {
                            free(tok.value);
                            found_arrow = true;
                            arity++; /* Every arrow means the previous chunk was a param */
                            continue;
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
                            continue;
                        }
                        /* bracketed param [x : T] or [x :: T] — extract param name */
                        if (tok.type == TOK_LBRACKET) {
                            free(tok.value);
                            tok = lexer_next_token(&lex);
                            if (tok.type == TOK_SYMBOL && tok.value &&
                                tok.value[0] >= 'a' && tok.value[0] <= 'z') {
                                arity_set(t, tok.value, 0);
                            }
                            free(tok.value);
                            int _bd = 1;
                            while (_bd > 0) {
                                tok = lexer_next_token(&lex);
                                if (tok.type == TOK_EOF) { _bd = 0; free(tok.value); break; }
                                if (tok.type == TOK_LBRACKET) _bd++;
                                if (tok.type == TOK_RBRACKET) _bd--;
                                free(tok.value);
                            }
                            continue;
                        }
                        free(tok.value);
                    }
                    if (found_arrow) {
                        arity_set_with_kinds(t, fname, arity, kinds);
                        /* store per-param fn arities */
                        unsigned int h = arity_hash(fname);
                        for (ArityEntry *e = t->buckets[h]; e; e = e->next)
                            if (strcmp(e->name, fname) == 0) {
                                memcpy(e->param_fn_arities, fn_arities, sizeof(fn_arities));
                                break;
                            }
                    } else {
                        /* define name :: ?  — hole type, treat as arity -1 (unknown) */
                        Lexer _peek = lex;
                        Token _pt = lexer_next_token(&_peek);
                        bool sig_is_hole = (_pt.type == TOK_SYMBOL && _pt.value &&
                                            _pt.value[0] == '?' && _pt.value[1] == '\0');
                        free(_pt.value);
                        if (sig_is_hole) {
                            tok = lexer_next_token(&lex);
                            free(tok.value);
                            arity_set(t, fname, -1);
                        }
                    }
                    /* no arrow, no hole = variable definition, not a function, skip */
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

        if (tok.type != TOK_SYMBOL ||
            (strcmp(tok.value, "define") != 0 && strcmp(tok.value, "def") != 0)) {
            free(tok.value); continue;
        }
        /* keep def as def — reader.c handles the scope enforcement */
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
                            char param_var_name[128] = {0};
                            int depth = 1;
                            bool first_tok_in_bracket = true;
                            while (depth > 0) {
                                tok = lexer_next_token(&lex);
                                if (tok.type == TOK_EOF) { depth = 0; free(tok.value); break; }
                                if (tok.type == TOK_LBRACKET) depth++;
                                if (tok.type == TOK_RBRACKET) depth--;
                                /* First symbol in bracket is the param name — register it */
                                if (depth > 0 && first_tok_in_bracket &&
                                    tok.type == TOK_SYMBOL && tok.value &&
                                    tok.value[0] >= 'a' && tok.value[0] <= 'z') {
                                    strncpy(param_var_name, tok.value, sizeof(param_var_name) - 1);
                                }
                                first_tok_in_bracket = false;
                                /* Fn keyword or arrow type inside bracket = function param */
                                if (depth > 0 && tok.type == TOK_SYMBOL && tok.value &&
                                    (strcmp(tok.value, "Fn") == 0 ||
                                     strcmp(tok.value, "->") == 0))
                                    is_fn = true;
                                if (depth > 0 && tok.type == TOK_ARROW)
                                    is_fn = true;
                                free(tok.value);
                            }
                            /* Register the param variable name as arity 0 (a value atom)
                             * so wisp's infix promotion treats it as lhs, not a callable */
                            if (param_var_name[0]) {
                                arity_set(t, param_var_name, 0);
                                /* fprintf(stderr, "DEBUG prescan: registered param '%s' arity=0\n", param_var_name); */
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

static const char *get_logical_line_end(const char *start) {
    const char *p = start;
    bool in_str = false;
    while (*p) {
        if (in_str) {
            if (*p == '\\' && *(p+1)) p++;
            else if (*p == '"') in_str = false;
        } else {
            if (*p == '"') in_str = true;
            else if (*p == ';') break;
        }
        p++;
    }
    while (p > start && (*(p-1) == ' ' || *(p-1) == '\t')) p--;
    return p;
}

static const char *skip_balanced_chars(const char *q) {
    char open_char = *q;
    char close_char = open_char == '[' ? ']' : (open_char == '(' ? ')' : '}');
    int depth = 0;
    const char *p = q;
    while (*p) {
        if (*p == open_char) depth++;
        else if (*p == close_char) {
            depth--;
            p++;
            if (depth == 0) return p;
            continue;
        }
        p++;
    }
    return p;
}

/* Expand pointer-type sugar in a type string: *U8 -> Pointer :: U8, **U8 -> Pointer :: Pointer :: U8 */
static char *wisp_expand_ptr_type(const char *s) {
    if (!s || s[0] != '*') return strdup(s);
    int stars = 0;
    while (s[stars] == '*') stars++;
    const char *base = s + stars;
    char buf[512];
    buf[0] = '\0';
    for (int i = 0; i < stars; i++) {
        if (i > 0) strncat(buf, " :: ", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, "Pointer", sizeof(buf) - strlen(buf) - 1);
    }
    if (base[0]) {
        strncat(buf, " :: ", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, base, sizeof(buf) - strlen(buf) - 1);
    }
    return strdup(buf);
}

/* Desugar size-based array literals: [16kb], [16mb I32], etc.
 * Converts size suffix + optional element type into element count.
 * [16kb]       -> [16384 U8]
 * [16kb I32]   -> [4096 I32]
 * [1mb F64]    -> [131072 F64]
 * Returns a malloc'd replacement string, or NULL if not this form. */
static char *desugar_size_array(const char *tok) {
    /* Must start with '[' */
    if (!tok || tok[0] != '[') return NULL;
    const char *p = tok + 1;
    while (*p == ' ' || *p == '\t') p++;

    /* Must start with a digit */
    if (*p < '0' || *p > '9') return NULL;

    /* Parse the numeric prefix */
    char *endptr;
    unsigned long long num = strtoull(p, &endptr, 10);
    if (endptr == p) return NULL;

    /* Parse the suffix: kb, mb, gb, tb (case-insensitive) */
    unsigned long long multiplier = 0;
    const char *after_suffix = endptr;
    if      ((endptr[0]=='k'||endptr[0]=='K') && (endptr[1]=='b'||endptr[1]=='B')) { multiplier = 1024ULL;                    after_suffix = endptr + 2; }
    else if ((endptr[0]=='m'||endptr[0]=='M') && (endptr[1]=='b'||endptr[1]=='B')) { multiplier = 1024ULL*1024;               after_suffix = endptr + 2; }
    else if ((endptr[0]=='g'||endptr[0]=='G') && (endptr[1]=='b'||endptr[1]=='B')) { multiplier = 1024ULL*1024*1024;          after_suffix = endptr + 2; }
    else if ((endptr[0]=='t'||endptr[0]=='T') && (endptr[1]=='b'||endptr[1]=='B')) { multiplier = 1024ULL*1024*1024*1024;     after_suffix = endptr + 2; }
    else return NULL;  /* no size suffix, not our form */

    unsigned long long byte_count = num * multiplier;

    /* Skip whitespace after suffix to find optional element type */
    while (*after_suffix == ' ' || *after_suffix == '\t') after_suffix++;

    /* Collect optional type name up to ']' */
    char elem_type[32] = "U8";  /* default: byte */
    unsigned long long elem_size = 1;

    if (*after_suffix != ']' && *after_suffix != '\0') {
        /* Read the type token */
        const char *type_start = after_suffix;
        const char *type_end   = after_suffix;
        while (*type_end && *type_end != ']' && *type_end != ' ' && *type_end != '\t')
            type_end++;
        size_t tlen = type_end - type_start;
        if (tlen == 0 || tlen >= sizeof(elem_type)) return NULL;
        memcpy(elem_type, type_start, tlen);
        elem_type[tlen] = '\0';

        /* Map type to byte size */
        if      (strcmp(elem_type,"I8")  ==0||strcmp(elem_type,"U8")  ==0) elem_size = 1;
        else if (strcmp(elem_type,"I16") ==0||strcmp(elem_type,"U16") ==0) elem_size = 2;
        else if (strcmp(elem_type,"I32") ==0||strcmp(elem_type,"U32") ==0||
                 strcmp(elem_type,"F32") ==0)                               elem_size = 4;
        else if (strcmp(elem_type,"I64") ==0||strcmp(elem_type,"U64") ==0||
                 strcmp(elem_type,"F64") ==0)                               elem_size = 8;
        else if (strcmp(elem_type,"I128")==0||strcmp(elem_type,"U128")==0) elem_size = 16;
        else return NULL;  /* unknown type, leave it alone */
    }

    /* Validate: byte_count must be divisible by elem_size */
    if (byte_count == 0 || elem_size == 0 || (byte_count % elem_size) != 0) return NULL;

    unsigned long long elem_count = byte_count / elem_size;

    /* Build replacement token: [count Type] */
    char buf[128];
    snprintf(buf, sizeof(buf), "[%llu %s]", elem_count, elem_type);
    return strdup(buf);
}

static bool try_consume_type_annotation(WTokenStream *s, const char **p_ptr, const char *tok, int indent, int lineno) {
    const char *peek = *p_ptr;
    while (*peek == ' ' || *peek == '\t') peek++;

    bool _double_colon = (strncmp(peek, "::", 2) == 0 && (peek[2] == ' ' || peek[2] == '\t' || peek[2] == '\0'));
    bool _single_colon = (!_double_colon && peek[0] == ':' && (peek[1] == ' ' || peek[1] == '\t' || peek[1] == '\0'));
    if (!_double_colon && !_single_colon) {
        return false;
    }

    SB ann; sb_init(&ann);
    sb_putc(&ann, '[');
    sb_puts(&ann, tok);

    const char *q = peek;
    while ((strncmp(q, "::", 2) == 0 && (q[2] == ' ' || q[2] == '\t' || q[2] == '\0')) ||
           (q[0] == ':' && q[1] != ':' && (q[1] == ' ' || q[1] == '\t' || q[1] == '\0'))) {
        int _skip = (q[0] == ':' && q[1] != ':') ? 1 : 2;
        q += _skip;
        while (*q == ' ' || *q == '\t') q++;

        const char *seg_start = q;
        const char *seg_end;
        if (*q == '[' || *q == '(' || *q == '{') {
            seg_end = skip_balanced_chars(q);
        } else {
            seg_end = q;
            while (*seg_end && *seg_end != ' ' && *seg_end != '\t' && *seg_end != ';' && *seg_end != '\n') seg_end++;
        }
        if (seg_end == seg_start) break;

        char *seg = strndup(seg_start, seg_end - seg_start);
        sb_puts(&ann, " : ");
        sb_puts(&ann, seg);
        free(seg);
        q = seg_end;

        while (*q == ' ' || *q == '\t') q++;

        /* consume -> chains */
        while (*q == '-' && *(q+1) == '>' && (q[2] == ' ' || q[2] == '\t' || q[2] == '\0')) {
            sb_puts(&ann, " : ->");
            q += 2;
            while (*q == ' ' || *q == '\t') q++;

            seg_start = q;
            if (*q == '[' || *q == '(' || *q == '{') {
                seg_end = skip_balanced_chars(q);
            } else {
                seg_end = q;
                while (*seg_end && *seg_end != ' ' && *seg_end != '\t' && *seg_end != ';' && *seg_end != '\n') seg_end++;
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
    *p_ptr = q;
    return true;
}

/* Forward declaration — tokenise_into calls wisp_parse_expr for body expansion */
static void wisp_parse_expr(ArityTable *t, WTokenStream *s, SB *out, int parent_indent, int parent_remaining, int caller_prec);

static int op_precedence(const char *name) {
    if (!name) return -1;
    if (strcmp(name, "or")  == 0) return 1;
    if (strcmp(name, "and") == 0) return 2;
    if (strcmp(name, "=")  == 0 || strcmp(name, "!=") == 0 ||
        strcmp(name, "<")  == 0 || strcmp(name, ">")  == 0 ||
        strcmp(name, "<=") == 0 || strcmp(name, ">=") == 0) return 3;
    if (strcmp(name, "+") == 0 || strcmp(name, "-") == 0) return 4;
    if (strcmp(name, "*") == 0 || strcmp(name, "/") == 0) return 5;
    return 6;
}

static int g_infix_fence = -1; /* stream pos: infix promotion stops before this */

static void wisp_syntax_error(int line, int column, const char *message, const char *hint) {
    snprintf(g_reader_error_msg, sizeof(g_reader_error_msg), "%s", message);
    fprintf(stderr, "%s:%d:%d: error: %s\n",
            current_filename ? current_filename : "<input>",
            line, column, message);

    const char *src = original_source ? original_source : current_source;
    if (src) {
        const char *line_start = src;
        int current_line = 1;
        while (current_line < line && *line_start) {
            if (*line_start == '\n') current_line++;
            line_start++;
        }
        const char *line_end = line_start;
        while (*line_end && *line_end != '\n') line_end++;
        fprintf(stderr, "%5d | %.*s\n", line, (int)(line_end - line_start), line_start);
        fprintf(stderr, "      | ");
        for (int i = 1; i < column; i++) fprintf(stderr, " ");
        fprintf(stderr, "^\n");
    }

    if (hint && hint[0]) fprintf(stderr, "  - Hint: %s\n", hint);

    if (g_reader_escape_set) {
        g_reader_escape_set = false;
        longjmp(g_reader_escape, 1);
    }
    exit(1);
}

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
            if (*p == '"') {
                wisp_syntax_error(
                    lineno,
                    (int)(p - line) + 1,
                    "adjacent string literals are not supported",
                    "write one string literal, for example \"helloworld\"; strings may already span multiple lines inside one pair of quotes");
            }
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

        /* corner-bracket quasiquote: ⌜...⌝ — treat as single atomic token
         * U+231C ⌜ = E2 8C 9C,  U+231D ⌝ = E2 8C 9D
         * U+231E ⌞ = E2 8C 9E,  U+231F ⌟ = E2 8C 9F
         * We collect everything from ⌜ to the matching ⌝ as one token so
         * wisp never tries to parse the interior as separate identifiers.
         * The reader already knows how to handle this token correctly.    */
        if ((unsigned char)p[0] == 0xE2 &&
            (unsigned char)p[1] == 0x8C &&
            (unsigned char)p[2] == 0x9C) {
            const char *start = p;
            p += 3; /* skip ⌜ */
            /* scan forward until matching ⌝, skipping nested ⌞⌟ pairs */
            while (*p) {
                if ((unsigned char)p[0] == 0xE2 &&
                    (unsigned char)p[1] == 0x8C &&
                    (unsigned char)p[2] == 0x9D) {
                    p += 3; /* skip ⌝ */
                    break;
                }
                /* skip string literals inside the bracket */
                if (*p == '"') {
                    p++;
                    while (*p && !(*p == '"' && *(p-1) != '\\')) p++;
                    if (*p == '"') p++;
                    continue;
                }
                p++;
            }
            char *tok = strndup(start, p - start);
            wts_push(s, tok, indent, lineno);
            free(tok);
            continue;
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

        /* heap array literal: ~[...] — keep as one balanced token.
         * The reader expects this exact surface form so it can lex '~'
         * adjacent to '[' and mark the resulting AST_ARRAY as heap-backed. */
        if (*p == '~' && *(p+1) == '[') {
            const char *start = p;
            p++; /* leave p on '[' so the balanced scanner includes it */
            char open = *p;
            char close = ']';
            int depth = 0; bool in_str = false;
            while (*p) {
                if (in_str) {
                    if (*p == '\\') p++;
                    else if (*p == '"') in_str = false;
                    p++; continue;
                }
                if (*p == '"') { in_str = true; p++; continue; }
                if (*p == ';') break;
                if (*p == open) depth++;
                if (*p == close) { depth--; p++; if (!depth) break; continue; }
                p++;
            }
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

            /* Desugar size-based array literals before any further processing */
            {
                char *desugared = desugar_size_array(tok);
                if (desugared) { free(tok); tok = desugared; }
            }

            /* After a grouped token, apply the same :: annotation grouping */
            const char *peek = p;
            while (*peek == ' ' || *peek == '\t') peek++;

            if (tok[0] == '[' &&
                strncmp(peek, "=>", 2) == 0 &&
                (peek[2] == ' ' || peek[2] == '\t' || peek[2] == '\0')) {
                /* [x y z] => body  ->  (lambda ([x] [y] [z]) body) */
                /* Parse param names out of the bracket token */
                SB lam; sb_init(&lam);
                sb_puts(&lam, "(lambda (");
                const char *tp = tok + 1; /* skip '[' */
                while (*tp && *tp != ']') {
                    while (*tp == ' ' || *tp == '\t') tp++;
                    if (!*tp || *tp == ']') break;
                    /* read one param name token */
                    const char *name_start = tp;
                    while (*tp && *tp != ' ' && *tp != '\t' && *tp != ']') tp++;
                    size_t nlen = tp - name_start;
                    if (nlen > 0) {
                        char *pname = strndup(name_start, nlen);
                        sb_putc(&lam, '[');
                        sb_puts(&lam, pname);
                        sb_puts(&lam, "] ");
                        free(pname);
                    }
                }
                sb_puts(&lam, ") ");
                /* skip '=>' */
                p = peek + 2;
                while (*p == ' ' || *p == '\t') p++;
                /* collect rest of line as body */
                const char *body_start = p;
                const char *body_end = get_logical_line_end(body_start);
                char *body = strndup(body_start, body_end - body_start);
                sb_puts(&lam, body);
                free(body);
                sb_putc(&lam, ')');
                p = body_end;
                char *lam_tok = sb_take(&lam);
                wts_push(s, lam_tok, indent, lineno);
                free(lam_tok);
                free(tok);
            } else if (try_consume_type_annotation(s, &p, tok, indent, lineno)) {
                free(tok);
            } else if (strncmp(peek, ">>=", 3) == 0 ||
                       strncmp(peek, ">>", 2) == 0) {
                /* Monadic operators after a grouped token — emit verbatim */
                wts_push(s, tok, indent, lineno);
                free(tok);
            } else if (strncmp(peek, "=>", 2) == 0 &&
                       (peek[2] == ' ' || peek[2] == '\t' || peek[2] == '\0')) {
                /* (pattern) => body -- keep as arrow clause token so the
                 * build_token_stream arrow-clause handler processes the body
                 * with full infix promotion. Emit pattern and => separately
                 * so the existing -> clause machinery picks them up.        */
                wts_push(s, tok, indent, lineno);
                free(tok);
                p = peek;
            } else {
                wts_push(s, tok, indent, lineno);
                free(tok);
            }
            continue;
        }

        /* ↦ (U+21A6, UTF-8: E2 86 A6) — sugar for -> */
        if ((unsigned char)p[0] == 0xE2 &&
            (unsigned char)p[1] == 0x86 &&
            (unsigned char)p[2] == 0xA6) {
            p += 3;
            wts_push(s, "->", indent, lineno);
            continue;
        }

        /* regular token */
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ';') p++;
        char *tok = strndup(start, p - start);

        /* Peek ahead: if next non-space token is '::', group entire chain */
        if (try_consume_type_annotation(s, &p, tok, indent, lineno)) {
            free(tok);
            continue;
        }

        /* Expand pointer-type sugar: *U8 -> [Pointer :: U8] as a bracketed annotation */
        if (tok[0] == '*' && tok[1] >= 'A' && tok[1] <= 'Z') {
            char *expanded = wisp_expand_ptr_type(tok);
            size_t elen = strlen(expanded);
            char *bracketed = malloc(elen + 3);
            bracketed[0] = '[';
            memcpy(bracketed + 1, expanded, elen);
            bracketed[elen + 1] = ']';
            bracketed[elen + 2] = '\0';
            free(expanded);
            wts_push(s, bracketed, indent, lineno);
            free(bracketed);
            free(tok);
            continue;
        }

        wts_push(s, tok, indent, lineno);
        free(tok);
    }
}

// Build token stream from source.
// Multi-line grouped expressions (starting with '(' '[' '{') are
// accumulated across lines and emitted as a single token.
static int g_for_iter_depth = 0;

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

        /* Normalize ↦ (U+21A6, UTF-8: E2 86 A6) to -> before any scanning.
         * This runs on the raw line so all downstream code sees only '->'. */
        {
            size_t raw_len = strlen(raw);
            /* Count occurrences to size the output buffer */
            int mapsto_count = 0;
            for (size_t _i = 0; _i + 2 < raw_len; _i++) {
                if ((unsigned char)raw[_i]   == 0xE2 &&
                    (unsigned char)raw[_i+1] == 0x86 &&
                    (unsigned char)raw[_i+2] == 0xA6) {
                    mapsto_count++;
                    _i += 2;
                }
            }
            if (mapsto_count > 0) {
                /* Each ↦ (3 bytes) becomes -> (2 bytes): net -1 byte each */
                char *normalized = malloc(raw_len - mapsto_count + 1);
                size_t _r = 0, _w = 0;
                while (_r < raw_len) {
                    if (_r + 2 < raw_len &&
                        (unsigned char)raw[_r]   == 0xE2 &&
                        (unsigned char)raw[_r+1] == 0x86 &&
                        (unsigned char)raw[_r+2] == 0xA6) {
                        normalized[_w++] = '-';
                        normalized[_w++] = '>';
                        _r += 3;
                    } else {
                        normalized[_w++] = raw[_r++];
                    }
                }
                normalized[_w] = '\0';
                free(raw);
                raw = normalized;
            }
        }

        const char *t = raw;
        while (*t == ' ' || *t == '\t') t++;

        /* skip blank/comment lines */
        if (!*t || *t == ';') { free(raw); lineno++; continue; }

        int indent = measure_indent(raw);

        /* Check for pmatch clause: "pattern -> body" or "| guard -> body".
         * We split the line at top-level '->'. */
        /* for c -> iterable body... or for c <- iterable body...
         * Desugar before the generic arrow-clause handler fires.
         * for c -> expr  =>  (for [iter 0 (count expr)] (define c (expr iter)) body)
         * for c <- expr  =>  (for [iter (count expr) 0 -1] (define c (expr iter)) body) */
        if ((strncmp(t, "for", 3) == 0 && (t[3] == ' ' || t[3] == '\t'))) {
            const char *after_for = t + 3;
            while (*after_for == ' ' || *after_for == '\t') after_for++;
            /* read binding name: must be a plain symbol (no bracket) */
            if (*after_for && *after_for != '[' && *after_for != '(') {
                const char *name_end = after_for;
                while (*name_end && *name_end != ' ' && *name_end != '\t') name_end++;
                const char *after_name = name_end;
                while (*after_name == ' ' || *after_name == '\t') after_name++;
                bool is_forward  = (after_name[0] == '-' && after_name[1] == '>');
                bool is_backward = (after_name[0] == '<' && after_name[1] == '-');
                if (is_forward || is_backward) {
                    char *bind_name = strndup(after_for, name_end - after_for);
                    const char *iter_expr_start = after_name + 2;
                    while (*iter_expr_start == ' ' || *iter_expr_start == '\t') iter_expr_start++;
                    const char *iter_expr_end = get_logical_line_end(iter_expr_start);
                    char *iter_expr = strndup(iter_expr_start, iter_expr_end - iter_expr_start);

                    /* Nesting depth is tracked via the recursive call stack:
                     * g_for_iter_depth is incremented before we recurse into
                     * the body and decremented after, so each level of nested
                     * for sugar gets a unique iterator name automatically.    */
                    g_for_iter_depth++;
                    int iter_depth = g_for_iter_depth;
                    /* Build iter variable name: iter, iter2, iter3 ... */
                    char iter_var[32];
                    if (iter_depth == 1)
                        snprintf(iter_var, sizeof(iter_var), "iter");
                    else
                        snprintf(iter_var, sizeof(iter_var), "iter%d", iter_depth);

                    /* Collect ALL body lines (deeper indent) into a raw source buffer
                     * so we can run build_token_stream recursively on them.
                     * This is the key: nested for sugar must fire via the full pipeline,
                     * not via tokenise_into which skips the for-desugaring path.       */
                    size_t body_src_cap = 256;
                    char *body_src = malloc(body_src_cap);
                    size_t body_src_len = 0;
                    body_src[0] = '\0';

                    while (*p) {
                        const char *bls = p;
                        while (*p && *p != '\n') p++;
                        if (*p == '\n') p++;
                        char *blraw = strndup(bls, p - bls);
                        const char *blt = blraw;
                        while (*blt == ' ' || *blt == '\t') blt++;
                        if (!*blt || *blt == ';') { free(blraw); lineno++; continue; }
                        int bl_ind = measure_indent(blraw);
                        /* Skip truly blank lines (only whitespace/newline) — the
                         * \n separator inserted between body_src lines appears as
                         * a zero-indent blank line and must never trigger a stop. */
                        bool line_is_blank = true;
                        for (const char *_bc = blraw; *_bc; _bc++) {
                            if (*_bc != ' ' && *_bc != '\t' && *_bc != '\r' && *_bc != '\n') {
                                line_is_blank = false;
                                break;
                            }
                        }
                        if (line_is_blank) { free(blraw); lineno++; continue; }
                        if (bl_ind <= indent) { p = bls; free(blraw); break; }
                        /* Preserve the raw line (with original indentation stripped to
                         * relative indent) so nested for sugar sees correct structure. */
                        const char *blt_end = get_logical_line_end(blt);
                        size_t blt_len = blt_end - blt;
                        /* Indent body lines by (bl_ind - indent - 1) spaces so that
                         * the recursive build_token_stream sees correct relative depth. */
                        int rel_indent = bl_ind - indent;
                        if (rel_indent < 0) rel_indent = 0;
                        size_t need = body_src_len + rel_indent + blt_len + 3;
                        while (need >= body_src_cap) { body_src_cap *= 2; body_src = realloc(body_src, body_src_cap); }
                        if (body_src_len > 0) body_src[body_src_len++] = '\n';
                        for (int _si = 0; _si < rel_indent; _si++) body_src[body_src_len++] = ' ';
                        memcpy(body_src + body_src_len, blt, blt_len);
                        body_src_len += blt_len;
                        body_src[body_src_len] = '\0';
                        free(blraw);
                        lineno++;
                    }

                    /* Recursively expand body source through the full pipeline.
                     * The recursive call inherits g_for_iter_depth so nested
                     * for sugar inside this body gets iter(depth+1) names.   */
                    SB body_expanded_sb; sb_init(&body_expanded_sb);
                    if (body_src_len > 0) {
                        WTokenStream _bts = build_token_stream(body_src, at);
                        bool _bfirst = true;
                        while (_bts.pos < _bts.count) {
                            if (!_bfirst) sb_putc(&body_expanded_sb, ' ');
                            _bfirst = false;
                            wisp_parse_expr(at, &_bts, &body_expanded_sb, -1, 1, 0);
                        }
                        wts_free(&_bts);
                    }
                    free(body_src);
                    g_for_iter_depth--;
                    char *body_expanded = sb_take(&body_expanded_sb);

                    /* Build desugared s-expression */
                    SB ds; sb_init(&ds);
                    if (is_forward) {
                        sb_puts(&ds, "(for [");
                        sb_puts(&ds, iter_var);
                        sb_puts(&ds, " 0 (count ");
                        sb_puts(&ds, iter_expr);
                        sb_puts(&ds, ")] (define ");
                        sb_puts(&ds, bind_name);
                        sb_puts(&ds, " (");
                        sb_puts(&ds, iter_expr);
                        sb_puts(&ds, " ");
                        sb_puts(&ds, iter_var);
                        sb_puts(&ds, "))");
                    } else {
                        sb_puts(&ds, "(for [");
                        sb_puts(&ds, iter_var);
                        sb_puts(&ds, " (- (count ");
                        sb_puts(&ds, iter_expr);
                        sb_puts(&ds, ") 1) -1 -1] (define ");
                        sb_puts(&ds, bind_name);
                        sb_puts(&ds, " (");
                        sb_puts(&ds, iter_expr);
                        sb_puts(&ds, " ");
                        sb_puts(&ds, iter_var);
                        sb_puts(&ds, "))");
                    }
                    if (body_expanded[0] != '\0') {
                        sb_putc(&ds, ' ');
                        sb_puts(&ds, body_expanded);
                    }
                    sb_putc(&ds, ')');
                    free(body_expanded);

                    char *desugared = sb_take(&ds);
                    wts_push(&s, desugared, indent, lineno);
                    free(desugared);
                    free(iter_expr);
                    free(bind_name);
                    free(raw);
                    lineno++;
                    goto next_line;
                }
            }
        }

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
                /* Skip char literals 'x' '\n' etc. so '"' inside
                 * a char literal like '"' does not open a string */
                if (*q == '\'' && *(q+1) != '\0') {
                    if (*(q+1) == '\\' && *(q+2) != '\0' && *(q+3) == '\'') {
                        q += 3; /* '\x' */
                        continue;
                    } else if (*(q+2) == '\'') {
                        q += 2; /* 'x' */
                        continue;
                    }
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
                    char def_name[128] = {0};
                    bool found_def = false;
                    for (int _ti = s.count - 1; _ti >= 0; _ti--) {
                        if (strcmp(s.tokens[_ti].text, "define") == 0 &&
                            _ti + 1 < s.count) {
                            const char *dn = s.tokens[_ti + 1].text;
                            /* strip leading '(' if present */
                            if (dn[0] == '(') dn++;
                            int _fi = 0;
                            while (dn[_fi] && dn[_fi] != ' ' && dn[_fi] != '\t' && dn[_fi] != ')' && _fi < 127) {
                                def_name[_fi] = dn[_fi];
                                _fi++;
                            }
                            def_name[_fi] = '\0';
                            found_def = true;
                            break;
                        }
                    }
                    if (found_def) {
                        unsigned int _h = arity_hash(def_name);
                        ArityEntry *_de = NULL;
                        for (ArityEntry *_e = at->buckets[_h]; _e; _e = _e->next)
                            if (strcmp(_e->name, def_name) == 0) { _de = _e; break; }
                        if (_de) {
                            /* Walk pattern tokens and assign arities */
                                const char *_ps = lscan;
                                int _pi = 0;
                                while (*_ps && _pi < _de->arity) {
                                    while (*_ps == ' ' || *_ps == '\t') _ps++;
                                    if (!*_ps) break;
                                    if (*_ps == '|') break;
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
                            /* Stop accumulating body if this line starts a new
                             * guard clause (begins with '|' after whitespace).
                             * This prevents the next guard from being swallowed
                             * into the current guard's body during where-block
                             * and multi-guard clause expansion.                */
                            if (*clt == '|') { p = cls; free(clraw); break; }
                            const char *clt_end = get_logical_line_end(clt);
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

                        /* --- where desugaring ---
                         * Scan ahead for a 'where' keyword on the next
                         * non-blank line indented deeper than the clause head.
                         * Collect all bindings and wrap body_acc in letrec. */
                        {
                            /* Step 1: find next non-blank line and check for 'where' */
                            const char *wscan = p;
                            int where_kw_indent = -1;
                            const char *after_where = NULL;
                            while (*wscan) {
                                /* read one line */
                                const char *wls = wscan;
                                while (*wscan && *wscan != '\n') wscan++;
                                if (*wscan == '\n') wscan++;
                                /* strip to content */
                                const char *wlt = wls;
                                while (*wlt == ' ' || *wlt == '\t') wlt++;
                                /* skip blank/comment lines — advance and loop */
                                if (!*wlt || *wlt == ';') continue;
                                /* non-blank line found - check if it's 'where' */
                                int wl_ind = measure_indent(wls);
                                if (wl_ind < indent) break; /* not deeper, no where */
                                if (strncmp(wlt, "where", 5) == 0 &&
                                    (wlt[5]==' '||wlt[5]=='\t'||
                                     wlt[5]=='\n'||wlt[5]=='\0'||wlt[5]==';')) {
                                    where_kw_indent = wl_ind;
                                    after_where = wscan;
                                }
                                break; /* only check the one next non-blank line */
                            }

                            if (after_where) {
                                p = after_where; /* consume through 'where' line */

                                /* Step 2: collect bindings.
                                 * Each binding: name rest_of_first_line
                                 *               [continuation lines at deeper indent]
                                 * New binding starts when indent == first-binding-indent.
                                 * Continuation is anything deeper than that. */
                                typedef struct {
                                    char *name;
                                    char *rest; /* first line after name */
                                    char *cont; /* '\n'-joined continuation lines */
                                } WBind;
                                WBind *wb = NULL;
                                int wb_n = 0, wb_cap = 0;
                                int bind_indent = -1;

                                while (*p) {
                                    const char *bls = p;
                                    while (*p && *p != '\n') p++;
                                    if (*p == '\n') p++;
                                    char *blraw = strndup(bls, p - bls);
                                    const char *blt = blraw;
                                    while (*blt == ' ' || *blt == '\t') blt++;
                                    if (!*blt || *blt == ';') {
                                        free(blraw); lineno++; continue;
                                    }
                                    int bl_ind = measure_indent(blraw);
                                    /* stop when indent drops back to where-kw level or less */
                                    if (bl_ind <= where_kw_indent) {
                                        p = bls; free(blraw); break;
                                    }
                                    if (bind_indent < 0) bind_indent = bl_ind;

                                    const char *le = get_logical_line_end(blt);
                                    char *ltext = strndup(blt, le - blt);

                                    if (bl_ind <= bind_indent) {
                                        /* new binding */
                                        const char *np = ltext;
                                        const char *ns = np;
                                        while (*np && *np != ' ' && *np != '\t') np++;
                                        if (wb_n >= wb_cap) {
                                            wb_cap = wb_cap ? wb_cap*2 : 4;
                                            wb = realloc(wb, sizeof(WBind)*wb_cap);
                                        }
                                        wb[wb_n].name = strndup(ns, np - ns);
                                        while (*np == ' ' || *np == '\t') np++;
                                        wb[wb_n].rest = strdup(np);
                                        wb[wb_n].cont = strdup("");
                                        wb_n++;
                                    } else {
                                        /* continuation of previous binding */
                                        if (wb_n > 0) {
                                            size_t ol = strlen(wb[wb_n-1].cont);
                                            size_t nl2 = strlen(ltext);
                                            wb[wb_n-1].cont = realloc(wb[wb_n-1].cont, ol+nl2+3);
                                            if (ol > 0) wb[wb_n-1].cont[ol++] = '\n';
                                            memcpy(wb[wb_n-1].cont + ol, ltext, nl2+1);
                                        }
                                    }
                                    free(ltext);
                                    free(blraw);
                                    lineno++;
                                }

                                /* Step 3: build (letrec ([name val] ...) body_acc) */
                                #define EXPAND_WISP(out_str, src_str) do {                              \
                                    WTokenStream _ts = build_token_stream((src_str), at);               \
                                    SB _sb; sb_init(&_sb);                                              \
                                    bool _first_exp = true;                                             \
                                    while (_ts.pos < _ts.count) {                                       \
                                        if (!_first_exp) sb_putc(&_sb, ' ');                            \
                                        _first_exp = false;                                             \
                                        wisp_parse_expr(at, &_ts, &_sb, -1, 1, 0);                      \
                                    }                                                                   \
                                    wts_free(&_ts);                                                     \
                                    (out_str) = sb_take(&_sb);                                          \
                                } while(0)

                                /* Expand body_acc FIRST before registering arities.
                                 * A bare body like "go" means return go as a value.
                                 * If we registered go's arity first, wisp_parse_expr
                                 * would turn "go" into "(go)" — a zero-arg call. */
                                {
                                    char *_early_body;
                                    EXPAND_WISP(_early_body, body_acc);
                                    free(body_acc);
                                    body_acc = _early_body;
                                }

                                /* Now register arities so binding bodies that call
                                 * go internally expand correctly. */
                                for (int bi = 0; bi < wb_n; bi++) {
                                    WBind *b = &wb[bi];
                                    int b_arity = 0;
                                    const char *ps = b->rest;
                                    while (*ps) {
                                        while (*ps == ' ' || *ps == '\t') ps++;
                                        if (!*ps) break;
                                        if (*ps == '|') break;
                                        if (*ps == '-' && *(ps+1) == '>') break;
                                        if (*ps == '=' && *(ps+1) != '=') break;
                                        if (*ps == '(' || *ps == '[') {
                                            char oc = *ps, cc = oc=='('?')':']';
                                            int gd = 0;
                                            while (*ps) {
                                                if (*ps==oc) gd++;
                                                else if (*ps==cc){gd--;ps++;if(!gd)break;continue;}
                                                ps++;
                                            }
                                        } else {
                                            while (*ps && *ps!=' ' && *ps!='\t' &&
                                                   *ps!='|' &&
                                                   !(*ps=='-'&&*(ps+1)=='>') &&
                                                   !(*ps=='='&&*(ps+1)!='=')) ps++;
                                        }
                                        b_arity++;
                                    }
                                    if (b_arity > 0)
                                        arity_set(at, b->name, b_arity);
                                }

                                SB lr; sb_init(&lr);
                                sb_puts(&lr, "(letrec (");

                                for (int bi = 0; bi < wb_n; bi++) {
                                    WBind *b = &wb[bi];
                                    sb_puts(&lr, "[");
                                    sb_puts(&lr, b->name);
                                    sb_puts(&lr, " ");

                                    /* Classify binding: '=' value/function, or '->'/'|' guard */
                                    const char *rt = b->rest;
                                    bool has_eq = false, has_arrow_or_guard = false;
                                    int fd = 0;
                                    for (const char *q = rt; *q && *q != '\n'; q++) {
                                        if (*q=='('||*q=='[') fd++;
                                        if (*q==')'||*q==']') fd--;
                                        if (fd==0 && *q=='=' && *(q+1)!='=') { has_eq=true; break; }
                                        if (fd==0 && *q=='-' && *(q+1)=='>') { has_arrow_or_guard=true; break; }
                                        if (fd==0 && *q=='|') { has_arrow_or_guard=true; break; }
                                    }
                                    if (!has_arrow_or_guard && b->cont[0]) {
                                        for (const char *q = b->cont; *q; q++) {
                                            if (*q=='|') { has_arrow_or_guard=true; break; }
                                            if (*q=='-'&&*(q+1)=='>') { has_arrow_or_guard=true; break; }
                                        }
                                    }

                                    if (has_eq && !has_arrow_or_guard) {
                                        /* '=' binding: name arg1 arg2 = expr */
                                        const char *ep = rt;
                                        char **eargs = NULL; int eac=0, eacap=0;
                                        while (*ep && !(*ep=='='&&*(ep+1)!='=')) {
                                            while (*ep==' '||*ep=='\t') ep++;
                                            if (!*ep||(*ep=='='&&*(ep+1)!='=')) break;
                                            const char *as2 = ep;
                                            while (*ep&&*ep!=' '&&*ep!='\t'&&!(*ep=='='&&*(ep+1)!='=')) ep++;
                                            if (ep>as2) {
                                                if (eac>=eacap){eacap=eacap?eacap*2:4; eargs=realloc(eargs,sizeof(char*)*eacap);}
                                                eargs[eac++]=strndup(as2,ep-as2);
                                            }
                                        }
                                        if (*ep=='=') ep++;
                                        while (*ep==' '||*ep=='\t') ep++;

                                        SB raw_body; sb_init(&raw_body);
                                        {
                                            const char *ep_scan = ep;
                                            while (*ep_scan == ' ' || *ep_scan == '\t') ep_scan++;
                                            size_t ep_len = strlen(ep_scan);
                                            while (ep_len > 0 &&
                                                   (ep_scan[ep_len-1] == '\n' ||
                                                    ep_scan[ep_len-1] == '\r' ||
                                                    ep_scan[ep_len-1] == ' '  ||
                                                    ep_scan[ep_len-1] == '\t'))
                                                ep_len--;
                                            for (size_t _ei = 0; _ei < ep_len; _ei++)
                                                sb_putc(&raw_body, ep_scan[_ei]);
                                        }
                                        if (b->cont[0]) { sb_putc(&raw_body, '\n'); sb_puts(&raw_body, b->cont); }
                                        char *raw_str = sb_take(&raw_body);
                                        char *expanded; EXPAND_WISP(expanded, raw_str);
                                        free(raw_str);

                                        if (eac > 0) {
                                            sb_puts(&lr, "(lambda (");
                                            for (int ai=0; ai<eac; ai++) {
                                                if (ai) sb_putc(&lr,' ');
                                                sb_putc(&lr,'['); sb_puts(&lr,eargs[ai]); sb_putc(&lr,']');
                                                free(eargs[ai]);
                                            }
                                            sb_puts(&lr, ") "); sb_puts(&lr, expanded); sb_putc(&lr, ')');
                                        } else {
                                            sb_puts(&lr, expanded);
                                        }
                                        free(expanded);
                                        free(eargs);
                                    } else {
                                        /* '->'/'|' binding: "improve g -> body" or "go x | guard -> body"
                                         * Strategy: extract param names from LHS (before first | or ->),
                                         * extract body from RHS (after ->), expand body with EXPAND_WISP,
                                         * wrap in (lambda (params...) expanded_body).
                                         * For bare value bindings (no arrow/guard), expand whole thing. */
                                        if (has_arrow_or_guard) {
                                            /* Step 1: collect param names from LHS */
                                            char *param_names[16];
                                            int param_name_count = 0;
                                            const char *ps = b->rest;
                                            while (*ps && param_name_count < 16) {
                                                while (*ps == ' ' || *ps == '\t') ps++;
                                                if (!*ps) break;
                                                if (*ps == '|') break;
                                                if (*ps == '-' && *(ps+1) == '>') break;
                                                if (*ps == '(' || *ps == '[') {
                                                    char oc = *ps, cc = oc=='('?')':']';
                                                    int gd = 0;
                                                    while (*ps) {
                                                        if (*ps==oc) gd++;
                                                        else if (*ps==cc) { gd--; ps++; if (!gd) break; continue; }
                                                        ps++;
                                                    }
                                                    param_name_count++;
                                                } else {
                                                    const char *ve = ps;
                                                    while (*ve && *ve!=' ' && *ve!='\t' &&
                                                           *ve!='|' &&
                                                           !(*ve=='-' && *(ve+1)=='>')) ve++;
                                                    size_t vl = ve - ps;
                                                    if (vl > 0) {
                                                        param_names[param_name_count] = strndup(ps, vl);
                                                        param_name_count++;
                                                    }
                                                    ps = ve;
                                                }
                                            }

                                            /* Step 2: build raw body string (body line + continuations) */
                                            SB raw_body; sb_init(&raw_body);
                                            sb_puts(&raw_body, b->rest);
                                            if (b->cont[0]) { sb_putc(&raw_body, '\n'); sb_puts(&raw_body, b->cont); }
                                            char *raw_str = sb_take(&raw_body);

                                            /* Step 3: expand body with full wisp infix promotion */
                                            char *expanded; EXPAND_WISP(expanded, raw_str);
                                            free(raw_str);

                                            /* Step 4: emit (lambda (p1 p2 ...) expanded_body) */
                                            sb_puts(&lr, "(lambda (");
                                            for (int ai = 0; ai < param_name_count; ai++) {
                                                if (ai) sb_putc(&lr, ' ');
                                                sb_putc(&lr, '[');
                                                sb_puts(&lr, param_names[ai]);
                                                sb_putc(&lr, ']');
                                                free(param_names[ai]);
                                            }
                                            sb_puts(&lr, ") ");
                                            sb_puts(&lr, expanded);
                                            sb_putc(&lr, ')');
                                            free(expanded);
                                        } else {
                                            /* Bare value binding: expand whole rest */
                                            SB raw_body; sb_init(&raw_body);
                                            sb_puts(&raw_body, b->rest);
                                            if (b->cont[0]) { sb_putc(&raw_body, '\n'); sb_puts(&raw_body, b->cont); }
                                            char *raw_str = sb_take(&raw_body);
                                            char *expanded; EXPAND_WISP(expanded, raw_str);
                                            free(raw_str);
                                            sb_puts(&lr, expanded);
                                            free(expanded);
                                        }
                                    }
                                    sb_puts(&lr, "] ");
                                    free(b->name); free(b->rest); free(b->cont);
                                }
                                free(wb);
                                sb_puts(&lr, ") ");

                                /* body_acc was already expanded before the arity
                                 * registration above, so emit it directly. */
                                const char *ba = body_acc;
                                while (*ba==' '||*ba=='\t') ba++;
                                if (ba[0] != '(' && ba[0] != '[' && ba[0] != '{' && strchr(ba, ' ')) {
                                    sb_putc(&lr, '(');
                                    sb_puts(&lr, ba);
                                    sb_putc(&lr, ')');
                                } else {
                                    sb_puts(&lr, ba);
                                }
                                sb_putc(&lr, ')');
                                free(body_acc);
                                body_acc = sb_take(&lr);
                                #undef EXPAND_WISP
                            }
                        }
                        /* --- end where desugaring --- */


                        char *body_raw = body_acc;
                        WTokenStream body_ts = {0};
                        tokenise_into(at, &body_ts, body_raw, indent, lineno);
                        SB body_sb; sb_init(&body_sb);
                        bool first_tok = true;

                        while (body_ts.pos < body_ts.count) {
                            if (!first_tok) sb_putc(&body_sb, ' ');
                            first_tok = false;

                            /* Scan ahead: find the first infix operator on this line.
                             * An infix op is a known-arity>=2 atom that is NOT the
                             * first token. Everything before it becomes the lhs,
                             * built by calling wisp_parse_expr repeatedly.           */
                            bool promoted = false;
                            const char *head_txt = body_ts.tokens[body_ts.pos].text;
                            bool is_keyword = (strcmp(head_txt, "if") == 0 || strcmp(head_txt, "let") == 0 || strcmp(head_txt, "match") == 0 || strcmp(head_txt, "cond") == 0);
                            int infix_op_scan = body_ts.pos + 1;
                            while (!is_keyword && infix_op_scan < body_ts.count) {
                                WToken *scan_tok = &body_ts.tokens[infix_op_scan];
                                bool is_atom = (scan_tok->text[0] != '(' &&
                                               scan_tok->text[0] != '[' &&
                                               scan_tok->text[0] != '{');
                                int scan_arity = arity_get(at, scan_tok->text);
                                if (is_atom && (scan_arity >= 2 || scan_arity == -1)) {
                                    /* Check if this operator is actually being passed as a function argument */
                                    ArityEntry *head_entry = arity_get_entry(at, head_txt);
                                    if (head_entry && head_entry->arity > 0) {
                                        int slot = infix_op_scan - body_ts.pos - 1;
                                        if (slot >= 0 && slot < WISP_MAX_PARAMS && head_entry->param_kinds[slot] == PARAM_FUNC) {
                                            infix_op_scan++;
                                            continue;
                                        }
                                    }

                                    /* Found infix op at infix_op_scan.
                                     * Build lhs by parsing tokens from pos up to
                                     * infix_op_scan into one grouped expression.   */
                                    int lhs_end = infix_op_scan;
                                    /* Parse lhs tokens */
                                    SB lhs_sb; sb_init(&lhs_sb);
                                    int lhs_tok_count = lhs_end - body_ts.pos;
                                    if (lhs_tok_count == 1) {
                                        /* Single token lhs */
                                        sb_puts(&lhs_sb, body_ts.tokens[body_ts.pos].text);
                                        body_ts.pos++;
                                    } else {
                                        /* Multi-token lhs: wrap in parens */
                                        SB tmp_sb; sb_init(&tmp_sb);
                                        sb_putc(&lhs_sb, '(');
                                        bool lf = true;
                                        while (body_ts.pos < lhs_end) {
                                            if (!lf) sb_putc(&lhs_sb, ' ');
                                            lf = false;
                                            sb_puts(&lhs_sb, body_ts.tokens[body_ts.pos].text);
                                            body_ts.pos++;
                                        }
                                        sb_putc(&lhs_sb, ')');
                                        sb_free(&tmp_sb);
                                    }
                                    char *accum = sb_take(&lhs_sb);

                                    /* Now consume infix chain */
                                    while (body_ts.pos < body_ts.count) {
                                        WToken *op_tok2 = &body_ts.tokens[body_ts.pos];
                                        if (op_tok2->text[0] == '(' || op_tok2->text[0] == '[' || op_tok2->text[0] == '{') break;
                                        int oa = arity_get(at, op_tok2->text);
                                        if (oa == -2) break;
                                        if (oa < 2 && oa != -1) break;

                                        char *op_name = strdup(op_tok2->text);
                                        body_ts.pos++;

                                        SB next_acc; sb_init(&next_acc);
                                        sb_putc(&next_acc, '('); sb_puts(&next_acc, op_name);
                                        sb_putc(&next_acc, ' '); sb_puts(&next_acc, accum);
                                        free(op_name); free(accum);

                                        int rhs_slots = (oa > 0) ? (oa - 1) : -1;
                                        if (rhs_slots == -1) {
                                            while (body_ts.pos < body_ts.count) {
                                                sb_putc(&next_acc, ' ');
                                                wisp_parse_expr(at, &body_ts, &next_acc, -1, 1, 0);
                                            }
                                        } else {
                                            for (int r = 0; r < rhs_slots && body_ts.pos < body_ts.count; r++) {
                                                sb_putc(&next_acc, ' ');
                                                wisp_parse_expr(at, &body_ts, &next_acc, -1, 1, 0);
                                            }
                                        }
                                        sb_putc(&next_acc, ')');
                                        accum = sb_take(&next_acc);
                                    }
                                    sb_puts(&body_sb, accum);
                                    free(accum);
                                    promoted = true;
                                    break;
                                }
                                /* Grouped tokens count as one unit, skip past them */
                                if (!is_atom) {
                                    infix_op_scan++;
                                    continue;
                                }
                                if (scan_arity == -2 || scan_arity == 0) {
                                    infix_op_scan++;
                                } else {
                                    break;
                                }
                            }
                            if (!promoted) {
                                wisp_parse_expr(at, &body_ts, &body_sb, -1, 1, 0);
                            }
                        }

                        char *body_expanded = sb_take(&body_sb);
                        wts_free(&body_ts);
                        free(body_raw);

                        lscan = left_str;
                        while (*lscan == ' ' || *lscan == '\t') lscan++;

                        /* Check if left_str contains '|' not at start —
                         * pattern with inline guard: "c | guard -> body"
                         * Split into pattern tokens + | + guard expansion. */
                        const char *inline_pipe = NULL;
                        {
                            const char *_sp = lscan;
                            int _bd = 0; bool _bs = false;
                            for (; *_sp; _sp++) {
                                if (_bs) { if (*_sp=='\\') _sp++; else if (*_sp=='"') _bs=false; continue; }
                                if (*_sp=='"') { _bs=true; continue; }
                                if (*_sp=='('||*_sp=='['||*_sp=='{') _bd++;
                                if (*_sp==')'||*_sp==']'||*_sp=='}') _bd--;
                                if (_bd==0 && *_sp=='|' &&
                                    (_sp==lscan || *(_sp-1)==' '||*(_sp-1)=='\t') &&
                                    (*(  _sp+1)==' '||*(  _sp+1)=='\t'||*(  _sp+1)=='\0')) {
                                    inline_pipe = _sp; break;
                                }
                            }
                        }
                        if (inline_pipe) {
                            /* emit pattern tokens before | */
                            size_t pat_part_len = inline_pipe - lscan;
                            while (pat_part_len > 0 &&
                                   (lscan[pat_part_len-1]==' '||lscan[pat_part_len-1]=='\t'))
                                pat_part_len--;
                            if (pat_part_len > 0) {
                                char *pat_part = malloc(pat_part_len + 2);
                                memcpy(pat_part, lscan, pat_part_len);
                                pat_part[pat_part_len] = ' ';
                                pat_part[pat_part_len+1] = '\0';
                                wts_push(&s, pat_part, indent, lineno);
                                free(pat_part);
                            }
                            /* emit | as a TOK_PIPE-equivalent marker */
                            wts_push(&s, "|", indent, lineno);
                            /* emit guard as expanded token */
                            const char *gstart = inline_pipe + 1;
                            while (*gstart == ' ' || *gstart == '\t') gstart++;
                            if (*gstart) {
                                WTokenStream guard_ts = {0};
                                tokenise_into(at, &guard_ts, gstart, indent, lineno);
                                fprintf(stderr, "DEBUG guard_ts count=%d tokens: ", guard_ts.count);
                                for (int _gi = 0; _gi < guard_ts.count; _gi++)
                                    fprintf(stderr, "[%d:'%s'(ar=%d)] ", _gi, guard_ts.tokens[_gi].text, arity_get(at, guard_ts.tokens[_gi].text));
                                fprintf(stderr, "\n");

                                /* Two-pass guard expansion with operator precedence:
                                 * Pass 1: reduce all comparison operators (>=,<=,>,<,=,!=)
                                 *         into grouped tokens: c >= 'a' -> (>= c 'a')
                                 * Pass 2: reduce logical operators (and, or) whose operands
                                 *         are now single grouped tokens from pass 1.
                                 * This correctly handles: c >= 'a' and c <= 'z'
                                 *   Pass1: [(>= c 'a')] [and] [(<= c 'z')]
                                 *   Pass2: [(and (>= c 'a') (<= c 'z'))]
                                 */

                                /* --- Pass 1: collapse comparisons --- */
                                /* Build a reduced token list where each
                                 * "atom cmp atom" triple becomes one string token */
                                char **p1_toks = malloc(sizeof(char*) * (guard_ts.count + 1));
                                int p1_count = 0;
                                int _gp = 0;
                                while (_gp < guard_ts.count) {
                                    const char *cur = guard_ts.tokens[_gp].text;
                                    bool is_cmp = (strcmp(cur,">=") == 0 || strcmp(cur,"<=") == 0 ||
                                                   strcmp(cur,">")  == 0 || strcmp(cur,"<")  == 0 ||
                                                   strcmp(cur,"=")  == 0 || strcmp(cur,"!=") == 0);
                                    bool is_logic = (strcmp(cur,"and") == 0 || strcmp(cur,"or") == 0);

                                    /* atom cmp atom -> "(cmp lhs rhs)" */
                                    if (!is_cmp && !is_logic &&
                                        _gp + 2 < guard_ts.count) {
                                        const char *op  = guard_ts.tokens[_gp + 1].text;
                                        const char *rhs = guard_ts.tokens[_gp + 2].text;
                                        bool op_is_cmp = (strcmp(op,">=") == 0 || strcmp(op,"<=") == 0 ||
                                                          strcmp(op,">")  == 0 || strcmp(op,"<")  == 0 ||
                                                          strcmp(op,"=")  == 0 || strcmp(op,"!=") == 0);
                                        if (op_is_cmp) {
                                            size_t len = strlen(cur) + strlen(op) + strlen(rhs) + 5;
                                            char *grouped = malloc(len);
                                            snprintf(grouped, len, "(%s %s %s)", op, cur, rhs);
                                            p1_toks[p1_count++] = grouped;
                                            _gp += 3;
                                            continue;
                                        }
                                    }
                                    p1_toks[p1_count++] = strdup(cur);
                                    _gp++;
                                }

                                fprintf(stderr, "DEBUG guard pass1: ");
                                for (int _gi = 0; _gi < p1_count; _gi++)
                                    fprintf(stderr, "[%s] ", p1_toks[_gi]);
                                fprintf(stderr, "\n");

                                /* --- Pass 2: collapse and/or --- */
                                char **p2_toks = malloc(sizeof(char*) * (p1_count + 1));
                                int p2_count = 0;
                                int _gp2 = 0;
                                while (_gp2 < p1_count) {
                                    const char *cur = p1_toks[_gp2];
                                    bool is_logic = (strcmp(cur,"and") == 0 || strcmp(cur,"or") == 0);
                                    if (!is_logic &&
                                        _gp2 + 2 < p1_count) {
                                        const char *op  = p1_toks[_gp2 + 1];
                                        const char *rhs = p1_toks[_gp2 + 2];
                                        bool op_is_logic = (strcmp(op,"and") == 0 || strcmp(op,"or") == 0);
                                        if (op_is_logic) {
                                            size_t len = strlen(cur) + strlen(op) + strlen(rhs) + 5;
                                            char *grouped = malloc(len);
                                            snprintf(grouped, len, "(%s %s %s)", op, cur, rhs);
                                            p2_toks[p2_count++] = grouped;
                                            _gp2 += 3;
                                            continue;
                                        }
                                    }
                                    p2_toks[p2_count++] = strdup(cur);
                                    _gp2++;
                                }

                                fprintf(stderr, "DEBUG guard pass2: ");
                                for (int _gi = 0; _gi < p2_count; _gi++)
                                    fprintf(stderr, "[%s] ", p2_toks[_gi]);
                                fprintf(stderr, "\n");

                                /* Join pass2 result */
                                SB guard_sb; sb_init(&guard_sb);
                                for (int _gi = 0; _gi < p2_count; _gi++) {
                                    if (_gi > 0) sb_putc(&guard_sb, ' ');
                                    sb_puts(&guard_sb, p2_toks[_gi]);
                                    free(p2_toks[_gi]);
                                }
                                for (int _gi = 0; _gi < p1_count; _gi++) free(p1_toks[_gi]);
                                free(p1_toks); free(p2_toks);
                                wts_free(&guard_ts);
                                char *guard_expanded = sb_take(&guard_sb);

                                /* Wrap in parens if multi-token so reader sees one expression */
                                bool needs_wrap = (guard_expanded[0] != '(' &&
                                                   guard_expanded[0] != '[' &&
                                                   guard_expanded[0] != '{' &&
                                                   strchr(guard_expanded, ' ') != NULL);
                                if (needs_wrap) {
                                    size_t glen = strlen(guard_expanded) + 3;
                                    char *wrapped = malloc(glen);
                                    snprintf(wrapped, glen, "(%s)", guard_expanded);
                                    wts_push(&s, wrapped, indent, lineno);
                                    free(wrapped);
                                } else {
                                    wts_push(&s, guard_expanded, indent, lineno);
                                }
                                free(guard_expanded);
                            }
                        } else {
                            char *pat_part = malloc(strlen(lscan) + 2);
                            strcpy(pat_part, lscan);
                            strcat(pat_part, " ");
                            wts_push(&s, pat_part, indent, lineno);
                            free(pat_part);
                        }
                        free(left_str);

                        wts_push(&s, (arrow[0] == '=') ? "=>" : "->", indent, lineno);
                        /* Check for a 'where' block following this clause.
                         * A where block appears at the same or deeper indent
                         * as the clause head, after the body, and contains
                         * name [args] = expr bindings.
                         * We collect them and wrap body in (let ...).      */
                        {
                            /* Scan ahead in source for 'where' keyword */
                            const char *where_scan = p;
                            int where_indent = -1;
                            const char *where_pos = NULL;
                            /* skip blank/comment lines */
                            while (*where_scan) {
                                const char *wls = where_scan;
                                while (*where_scan && *where_scan != '\n') where_scan++;
                                if (*where_scan == '\n') where_scan++;
                                const char *wlt = wls;
                                while (*wlt == ' ' || *wlt == '\t') wlt++;
                                if (!*wlt || *wlt == ';') { lineno++; continue; }
                                int wl_indent = measure_indent(wls);
                                /* where must be indented deeper than the define head
                                 * but at least as deep as the clause */
                                if (wl_indent <= indent) break;
                                if (strncmp(wlt, "where", 5) == 0 &&
                                    (wlt[5] == ' ' || wlt[5] == '\t' || wlt[5] == '\n' || wlt[5] == '\0')) {
                                    where_indent = wl_indent;
                                    where_pos = where_scan;
                                    break;
                                }
                                /* non-where line deeper than indent: could be
                                 * another clause or body continuation — stop */
                                break;
                            }

                            if (where_pos) {
                                /* Consume source up to and including where line */
                                p = where_pos;

                                /* Collect where bindings until indent drops */
                                /* Each binding: name [arg1 arg2 ...] = expr
                                 * We build a (let ([name (lambda (args) expr)] ...) body) */

                                typedef struct {
                                    char *name;
                                    char **args;
                                    int arg_count;
                                    char *expr;
                                } WhereBind;

                                WhereBind *binds = NULL;
                                int bind_count = 0;
                                int bind_cap = 0;

                                while (*p) {
                                    const char *bls = p;
                                    while (*p && *p != '\n') p++;
                                    if (*p == '\n') p++;
                                    char *blraw = strndup(bls, p - bls);
                                    const char *blt = blraw;
                                    while (*blt == ' ' || *blt == '\t') blt++;
                                    if (!*blt || *blt == ';') { free(blraw); lineno++; continue; }
                                    int bl_indent = measure_indent(blraw);
                                    if (bl_indent <= where_indent) {
                                        p = bls; free(blraw); break;
                                    }
                                    /* Parse: name [arg ...] = expr */
                                    const char *bp = blt;
                                    /* read name */
                                    const char *name_start = bp;
                                    while (*bp && *bp != ' ' && *bp != '\t' && *bp != '=') bp++;
                                    if (bp == name_start) { free(blraw); lineno++; continue; }
                                    char *bname = strndup(name_start, bp - name_start);
                                    while (*bp == ' ' || *bp == '\t') bp++;
                                    /* read args until '=' */
                                    char **bargs = NULL;
                                    int barg_count = 0, barg_cap = 0;
                                    while (*bp && *bp != '=') {
                                        if (*bp == '(' || *bp == '[') {
                                            /* skip grouped token */
                                            char oc = *bp, cc = oc == '(' ? ')' : ']';
                                            const char *gs = bp;
                                            int gd = 0;
                                            while (*bp) {
                                                if (*bp == oc) gd++;
                                                else if (*bp == cc) { gd--; bp++; if (!gd) break; continue; }
                                                bp++;
                                            }
                                            if (barg_count >= barg_cap) {
                                                barg_cap = barg_cap ? barg_cap * 2 : 4;
                                                bargs = realloc(bargs, sizeof(char*) * barg_cap);
                                            }
                                            bargs[barg_count++] = strndup(gs, bp - gs);
                                        } else {
                                            const char *as = bp;
                                            while (*bp && *bp != ' ' && *bp != '\t' && *bp != '=') bp++;
                                            if (bp > as) {
                                                if (barg_count >= barg_cap) {
                                                    barg_cap = barg_cap ? barg_cap * 2 : 4;
                                                    bargs = realloc(bargs, sizeof(char*) * barg_cap);
                                                }
                                                bargs[barg_count++] = strndup(as, bp - as);
                                            }
                                        }
                                        while (*bp == ' ' || *bp == '\t') bp++;
                                    }
                                    if (*bp == '=') bp++;
                                    while (*bp == ' ' || *bp == '\t') bp++;
                                    /* rest of line is the expr */
                                    const char *expr_start = bp;
                                    const char *expr_end = get_logical_line_end(expr_start);
                                    char *bexpr = strndup(expr_start, expr_end - expr_start);

                                    /* Accumulate continuation lines (deeper indent) */
                                    size_t bexpr_cap = strlen(bexpr) + 256;
                                    char *bexpr_acc = malloc(bexpr_cap);
                                    size_t bexpr_len = strlen(bexpr);
                                    memcpy(bexpr_acc, bexpr, bexpr_len + 1);
                                    free(bexpr);

                                    while (*p) {
                                        const char *cls = p;
                                        while (*p && *p != '\n') p++;
                                        if (*p == '\n') p++;
                                        char *clraw = strndup(cls, p - cls);
                                        const char *clt = clraw;
                                        while (*clt == ' ' || *clt == '\t') clt++;
                                        if (!*clt || *clt == ';') { free(clraw); lineno++; continue; }
                                        int cl_ind = measure_indent(clraw);
                                        if (cl_ind <= bl_indent) { p = cls; free(clraw); break; }
                                        const char *clt_end = get_logical_line_end(clt);
                                        size_t clt_len = clt_end - clt;
                                        while (bexpr_len + clt_len + 3 >= bexpr_cap) {
                                            bexpr_cap *= 2; bexpr_acc = realloc(bexpr_acc, bexpr_cap);
                                        }
                                        bexpr_acc[bexpr_len++] = ' ';
                                        memcpy(bexpr_acc + bexpr_len, clt, clt_len);
                                        bexpr_len += clt_len;
                                        bexpr_acc[bexpr_len] = '\0';
                                        free(clraw);
                                        lineno++;
                                    }

                                    if (bind_count >= bind_cap) {
                                        bind_cap = bind_cap ? bind_cap * 2 : 4;
                                        binds = realloc(binds, sizeof(WhereBind) * bind_cap);
                                    }
                                    binds[bind_count].name = bname;
                                    binds[bind_count].args = bargs;
                                    binds[bind_count].arg_count = barg_count;
                                    binds[bind_count].expr = bexpr_acc;
                                    bind_count++;
                                    free(blraw);
                                    lineno++;
                                }

                                /* Build let-wrapped body:
                                 * (let ([name (lambda ([a] [b]) expr)] ...) body) */
                                if (bind_count > 0) {
                                    SB wb; sb_init(&wb);
                                    sb_puts(&wb, "(let (");
                                    for (int bi = 0; bi < bind_count; bi++) {
                                        WhereBind *wb_bind = &binds[bi];
                                        sb_puts(&wb, "[");
                                        sb_puts(&wb, wb_bind->name);
                                        sb_puts(&wb, " ");
                                        if (wb_bind->arg_count > 0) {
                                            sb_puts(&wb, "(lambda (");
                                            for (int ai = 0; ai < wb_bind->arg_count; ai++) {
                                                if (ai) sb_putc(&wb, ' ');
                                                sb_putc(&wb, '[');
                                                sb_puts(&wb, wb_bind->args[ai]);
                                                sb_putc(&wb, ']');
                                            }
                                            sb_puts(&wb, ") ");
                                            sb_puts(&wb, wb_bind->expr);
                                            sb_putc(&wb, ')');
                                        } else {
                                            sb_puts(&wb, wb_bind->expr);
                                        }
                                        sb_puts(&wb, "] ");
                                    }
                                    sb_puts(&wb, ") ");
                                    /* append the original body */
                                    if (body_expanded[0] != '(' && body_expanded[0] != '[' &&
                                        body_expanded[0] != '{' && strchr(body_expanded, ' ')) {
                                        sb_putc(&wb, '(');
                                        sb_puts(&wb, body_expanded);
                                        sb_putc(&wb, ')');
                                    } else {
                                        sb_puts(&wb, body_expanded);
                                    }
                                    sb_putc(&wb, ')');
                                    char *let_body = sb_take(&wb);
                                    wts_push(&s, let_body, indent, lineno);
                                    free(let_body);

                                    for (int bi = 0; bi < bind_count; bi++) {
                                        free(binds[bi].name);
                                        free(binds[bi].expr);
                                        for (int ai = 0; ai < binds[bi].arg_count; ai++)
                                            free(binds[bi].args[ai]);
                                        free(binds[bi].args);
                                    }
                                    free(binds);
                                    free(body_expanded);
                                    free(raw);
                                    lineno++;
                                    continue;
                                }
                                free(binds);
                            }
                        }

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
                const char *t_end = get_logical_line_end(t);
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
                    const char *lt_end = get_logical_line_end(lt);
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

            /* Unbalanced - accumulate lines until balanced */
            SB acc; sb_init(&acc);

            /* copy first line, stripping inline comment */
            const char *t_end = get_logical_line_end(t);
            char *t_str = strndup(t, t_end - t);
            sb_puts(&acc, t_str);
            free(t_str);
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

                const char *lt_end = get_logical_line_end(lt);
                char *line_expanded = strndup(lt, lt_end - lt);

                char _ld[48];
                snprintf(_ld, sizeof(_ld), " (#line %d 1) ", lineno);
                sb_puts(&acc, _ld);

                /* If this continuation line starts with '|' inside a { } set
                 * literal, it is a refinement predicate. Run the expression
                 * after '|' through wisp infix promotion so that e.g.
                 * "Positive? n and Even? n" becomes "(and (Positive? n) (Even? n))"
                 * instead of being left as raw tokens for the reader.          */
                const char *le_scan = line_expanded;
                while (*le_scan == ' ' || *le_scan == '\t') le_scan++;
                if (*le_scan == '|' &&
                    (le_scan[1] == ' ' || le_scan[1] == '\t' || le_scan[1] == '\0')) {
                    sb_puts(&acc, "| ");
                    const char *pred_start = le_scan + 1;
                    while (*pred_start == ' ' || *pred_start == '\t') pred_start++;
                    if (*pred_start) {
                        /* Collect the first predicate line plus ALL subsequent
                         * continuation lines (deeper indent than the { block's
                         * indent) into one source buffer, then run the whole
                         * thing through build_token_stream + wisp_parse_expr.
                         * This lets multi-line variadic calls like:
                         *   | and s contains? "@"
                         *         s contains? "."
                         *         count s > 5
                         * expand correctly as a single (and ...) expression.
                         * We must NOT emit #line markers inside this buffer
                         * because they confuse the variadic indent tracking.  */
                        SB pred_src; sb_init(&pred_src);
                        sb_puts(&pred_src, pred_start);

                        /* Consume deeper continuation lines from p */
                        while (*p) {
                            const char *cls = p;
                            while (*p && *p != '\n') p++;
                            if (*p == '\n') p++;
                            char *clraw = strndup(cls, p - cls);
                            const char *clt = clraw;
                            while (*clt == ' ' || *clt == '\t') clt++;
                            if (!*clt || *clt == ';') { free(clraw); lineno++; continue; }
                            int cl_ind = measure_indent(clraw);
                            /* Stop when we reach a line at or shallower than
                             * the enclosing { block's indent level.
                             * If the line contains '}' it closes the set —
                             * strip everything from '}' onward and include
                             * only the predicate content before it, then stop. */
                            if (cl_ind <= indent) {
                                p = cls;
                                free(clraw);
                                break;
                            }
                            if (strchr(clt, '}')) {
                                /* Include content before '}' then stop */
                                const char *brace = strchr(clt, '}');
                                const char *seg_end = brace;
                                while (seg_end > clt &&
                                       (*(seg_end-1) == ' ' || *(seg_end-1) == '\t'))
                                    seg_end--;
                                if (seg_end > clt) {
                                    int rel = cl_ind - indent;
                                    sb_putc(&pred_src, '\n');
                                    for (int _si = 0; _si < rel; _si++)
                                        sb_putc(&pred_src, ' ');
                                    for (const char *_cp = clt; _cp < seg_end; _cp++)
                                        sb_putc(&pred_src, *_cp);
                                }
                                /* Rewind p to the '}' so the outer accumulator
                                 * still sees and emits the closing brace.       */
                                p = cls + (brace - clraw);
                                free(clraw);
                                break;
                            }
                            /* Compute indent relative to the | line so that
                             * wisp's variadic consumer sees correct structure. */
                            int rel = cl_ind - indent;
                            sb_putc(&pred_src, '\n');
                            for (int _si = 0; _si < rel; _si++) sb_putc(&pred_src, ' ');
                            const char *clt_end = get_logical_line_end(clt);
                            for (const char *_cp = clt; _cp < clt_end; _cp++)
                                sb_putc(&pred_src, *_cp);
                            free(clraw);
                            lineno++;
                        }

                        char *pred_src_str = sb_take(&pred_src);
                        WTokenStream pred_ts = build_token_stream(pred_src_str, at);
                        free(pred_src_str);
                        SB pred_sb; sb_init(&pred_sb);
                        bool pred_first = true;
                        while (pred_ts.pos < pred_ts.count) {
                            if (!pred_first) sb_putc(&pred_sb, ' ');
                            pred_first = false;
                            wisp_parse_expr(at, &pred_ts, &pred_sb, -1, 1, 0);
                        }
                        wts_free(&pred_ts);
                        char *pred_expanded = sb_take(&pred_sb);
                        sb_puts(&acc, pred_expanded);
                        free(pred_expanded);
                    }
                } else {
                    sb_puts(&acc, line_expanded);
                }
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

            char *final_acc = sb_take(&acc);
            wts_push(&s, final_acc, indent, lineno - 1);
            free(final_acc);
            continue;
        }

        /* Normal non-grouped line */
        /* Block lines: collect raw text until indent drops, wrap in parens.
         * Process arrow bodies through Wisp, pass the rest verbatim. */
        if ((strncmp(t, "layout", 6) == 0 &&
             (t[6] == ' ' || t[6] == '\t' || t[6] == '\0')) ||
            (strncmp(t, "data", 4) == 0 &&
             (t[4] == ' ' || t[4] == '\t' || t[4] == '\0')) ||
            (strncmp(t, "class", 5) == 0 &&
             (t[5] == ' ' || t[5] == '\t' || t[5] == '\0')) ||
            (strncmp(t, "instance", 8) == 0 &&
             (t[8] == ' ' || t[8] == '\t' || t[8] == '\0')) ||
            (strncmp(t, "match", 5) == 0 &&
             (t[5] == ' ' || t[5] == '\t' || t[5] == '\0'))) {

            size_t acc_cap = 256;
            char *acc = malloc(acc_cap);
            size_t acc_len = 0;
            acc[0] = '\0';

            acc[acc_len++] = '(';
            const char *t_end = get_logical_line_end(t);
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

                /* Detect => or -> at depth 0 so we can wisp-process the body */
                const char *fat_arrow = NULL;
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
                        if (bdepth == 0 && ((*q == '=' && *(q+1) == '>') || (*q == '-' && *(q+1) == '>'))) {
                            fat_arrow = q; break;
                        }
                    }
                }

                if (fat_arrow && !strstr(lt, "::")) {
                    char arrow_char = *fat_arrow;
                    /* Split at arrow: keep pattern verbatim, process body */
                    size_t pat_len = fat_arrow - lt;
                    while (pat_len > 0 &&
                           (lt[pat_len-1]==' '||lt[pat_len-1]=='\t')) pat_len--;
                    char *pat_str = strndup(lt, pat_len);

                    /* Body part: everything after => on this line */
                    const char *body_start = fat_arrow + 2;
                    while (*body_start==' '||*body_start=='\t') body_start++;
                    size_t body_inline_len = lt_end - body_start;

                    /* Accumulate body: inline part + all subsequent deeper lines */
                    size_t body_acc_cap = 256;
                    char *body_acc = malloc(body_acc_cap);
                    size_t body_acc_len = 0;
                    body_acc[0] = '\0';

                    if (body_inline_len > 0) {
                        while (body_acc_len + body_inline_len + 2 >= body_acc_cap) {
                            body_acc_cap *= 2; body_acc = realloc(body_acc, body_acc_cap);
                        }
                        memcpy(body_acc, body_start, body_inline_len);
                        body_acc_len = body_inline_len;
                        body_acc[body_acc_len] = '\0';
                    }

                    /* Accumulate all subsequent deeper lines */
                    while (*p) {
                        const char *nls = p;
                        while (*p && *p != '\n') p++;
                        if (*p == '\n') p++;
                        int nll = (int)(p - nls);
                        if (nll == 0) continue;

                        char *nlraw = strndup(nls, nll);
                        const char *nlt = nlraw;
                        while (*nlt == ' ' || *nlt == '\t') nlt++;

                        if (!*nlt || *nlt == ';') {
                            free(nlraw);
                            lineno++;
                            continue;
                        }

                        int nl_indent = measure_indent(nlraw);
                        if (nl_indent <= l_indent) {
                            p = nls;
                            free(nlraw);
                            break;
                        }

                        const char *nlt_end = nlt;
                        while (*nlt_end && *nlt_end != ';') nlt_end++;
                        while (nlt_end > nlt && (*(nlt_end-1)==' '||*(nlt_end-1)=='\t')) nlt_end--;
                        size_t nlt_len = nlt_end - nlt;

                        while (body_acc_len + nlt_len + 3 >= body_acc_cap) {
                            body_acc_cap *= 2; body_acc = realloc(body_acc, body_acc_cap);
                        }
                        body_acc[body_acc_len++] = ' ';
                        memcpy(body_acc + body_acc_len, nlt, nlt_len);
                        body_acc_len += nlt_len;
                        body_acc[body_acc_len] = '\0';
                        free(nlraw);
                        lineno++;
                    }

                    /* Trim leading whitespace: the continuation accumulator
                     * prepends a space before each appended line, so when the
                     * body is entirely on continuation lines (nothing inline
                     * after =>) body_acc starts with a space.  That fools the
                     * grouped-check into wrapping an already-grouped expression
                     * like "(if ...)" inside an extra pair of parens, producing
                     * "( (if ...))" which the reader parses as a zero-arg call
                     * applied to the if result — a double (a double!) mistake. */
                    const char *body_acc_trim = body_acc;
                    while (*body_acc_trim == ' ' || *body_acc_trim == '\t')
                        body_acc_trim++;

                    char *body_expanded;
                    bool body_already_grouped = (body_acc_trim[0] == '(' ||
                                                 body_acc_trim[0] == '[' ||
                                                 body_acc_trim[0] == '{');
                    bool body_single_token = (strchr(body_acc_trim, ' ') == NULL);
                    if (body_acc_len == 0 || body_acc_trim[0] == '\0') {
                        body_expanded = strdup("(undefined)");
                        free(body_acc);
                    } else if (!body_already_grouped && !body_single_token) {
                        size_t bflen = strlen(body_acc_trim);
                        body_expanded = malloc(bflen + 3);
                        body_expanded[0] = '(';
                        memcpy(body_expanded + 1, body_acc_trim, bflen);
                        body_expanded[bflen + 1] = ')';
                        body_expanded[bflen + 2] = '\0';
                        free(body_acc);
                    } else {
                        if (body_acc_trim != body_acc) {
                            size_t tlen = strlen(body_acc_trim);
                            memmove(body_acc, body_acc_trim, tlen + 1);
                        }
                        body_expanded = body_acc;
                    }

                    /* Build: (#line N 1) pat_str arrow body_expanded */
                    size_t need = acc_len + _ld2len + pat_len + 4 + strlen(body_expanded) + 4;
                    while (need >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
                    memcpy(acc + acc_len, _ld2, _ld2len); acc_len += _ld2len;
                    memcpy(acc + acc_len, pat_str, pat_len); acc_len += pat_len;
                    acc[acc_len++] = ' '; acc[acc_len++] = arrow_char; acc[acc_len++] = '>';
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
                    while (acc_len + (size_t)_ld2len + ltlen + 4 >= acc_cap) {
                        acc_cap *= 2; acc = realloc(acc, acc_cap);
                    }
                    memcpy(acc + acc_len, _ld2, _ld2len); acc_len += _ld2len;
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

                    WTokenStream body_ts = {0};
                    tokenise_into(at, &body_ts, body_str, l_indent, lineno);
                    free(body_str);

                    /* Accumulate all subsequent deeper lines into body_ts */
                    while (*p) {
                        const char *nls = p;
                        while (*p && *p != '\n') p++;
                        if (*p == '\n') p++;
                        int nll = (int)(p - nls);
                        if (nll == 0) continue;

                        char *nlraw = strndup(nls, nll);
                        const char *nlt = nlraw;
                        while (*nlt == ' ' || *nlt == '\t') nlt++;

                        if (!*nlt || *nlt == ';') {
                            free(nlraw);
                            lineno++;
                            continue;
                        }

                        int nl_indent = measure_indent(nlraw);
                        if (nl_indent <= l_indent) {
                            p = nls;
                            free(nlraw);
                            break;
                        }

                        tokenise_into(at, &body_ts, nlt, nl_indent, lineno + 1);
                        free(nlraw);
                        lineno++;
                    }

                    /* Convert body_ts tokens back to a source string and
                     * re-tokenize as a single grouped token so that
                     * multi-line bodies like "match ma with ..." are treated
                     * as one unit by the reader. This is the correct approach:
                     * we build the flat text, wrap it in parens if needed, and
                     * push it as a single pre-grouped token.                  */
                    SB body_src; sb_init(&body_src);
                    for (int _bti = 0; _bti < body_ts.count; _bti++) {
                        if (_bti > 0) sb_putc(&body_src, ' ');
                        sb_puts(&body_src, body_ts.tokens[_bti].text);
                    }
                    char *body_flat = sb_take(&body_src);
                    wts_free(&body_ts);

                    /* Wrap in parens so the reader sees it as one expression.
                     * Only wrap if not already grouped and has more than one token
                     * (single grouped tokens like "(Just x)" need no extra wrap). */
                    char *body_expanded;
                    bool already_grouped = (body_flat[0] == '(' || body_flat[0] == '[' || body_flat[0] == '{');
                    bool single_token = (strchr(body_flat, ' ') == NULL);
                    if (!already_grouped && !single_token) {
                        size_t bflen = strlen(body_flat);
                        body_expanded = malloc(bflen + 3);
                        body_expanded[0] = '(';
                        memcpy(body_expanded + 1, body_flat, bflen);
                        body_expanded[bflen + 1] = ')';
                        body_expanded[bflen + 2] = '\0';
                        free(body_flat);
                    } else {
                        body_expanded = body_flat;
                    }

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
            int _def_len = (strncmp(sig, "define", 6) == 0 && (sig[6] == ' ' || sig[6] == '\t')) ? 6 :
                           (strncmp(sig, "def", 3) == 0 && (sig[3] == ' ' || sig[3] == '\t')) ? 3 : 0;
            if (_def_len > 0) {
                const char *after_define = sig + _def_len;

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

                /* Case 0: define name :: [N] or define name :: [N Type] or define name :: [NNkb] etc.
                 * Pure array-type variable declaration — no function, no arrow.
                 * Desugars to: (define [name :: Arr :: N :: Type] [])
                 * Examples:
                 *   define buffer :: [512]        -> (define [buffer :: Arr :: 512 :: U8] [])
                 *   define buffer :: [16kb]       -> (define [buffer :: Arr :: 16384 :: U8] [])
                 *   define buffer :: [16kb I32]   -> (define [buffer :: Arr :: 4096 :: I32] [])
                 */
                if (after_name[0] == ':' && after_name[1] == ':') {
                    const char *_arr_sig = after_name + 2;
                    while (*_arr_sig == ' ' || *_arr_sig == '\t') _arr_sig++;
                    if (*_arr_sig == '[') {
                        /* Parse [N] or [N Type] or [Nkb] or [Nkb Type] */
                        const char *_inner = _arr_sig + 1;
                        while (*_inner == ' ' || *_inner == '\t') _inner++;
                        /* Must start with a digit (plain count or size suffix) */
                        if (*_inner >= '0' && *_inner <= '9') {
                            /* Read numeric prefix */
                            char *_endptr;
                            unsigned long long _num = strtoull(_inner, &_endptr, 10);
                            /* Check for size suffix */
                            unsigned long long _multiplier = 0;
                            const char *_after_suf = _endptr;
                            if ((_endptr[0]=='k'||_endptr[0]=='K') && (_endptr[1]=='b'||_endptr[1]=='B')) { _multiplier = 1024ULL;                   _after_suf = _endptr + 2; }
                            else if ((_endptr[0]=='m'||_endptr[0]=='M') && (_endptr[1]=='b'||_endptr[1]=='B')) { _multiplier = 1024ULL*1024;              _after_suf = _endptr + 2; }
                            else if ((_endptr[0]=='g'||_endptr[0]=='G') && (_endptr[1]=='b'||_endptr[1]=='B')) { _multiplier = 1024ULL*1024*1024;         _after_suf = _endptr + 2; }
                            else if ((_endptr[0]=='t'||_endptr[0]=='T') && (_endptr[1]=='b'||_endptr[1]=='B')) { _multiplier = 1024ULL*1024*1024*1024;    _after_suf = _endptr + 2; }
                            unsigned long long _byte_count = (_multiplier > 0) ? (_num * _multiplier) : 0;
                            /* Skip whitespace after number/suffix to find optional element type */
                            while (*_after_suf == ' ' || *_after_suf == '\t') _after_suf++;
                            /* Collect optional element type token */
                            char _elem_type[32] = "U8";
                            unsigned long long _elem_size = 1;
                            const char *_after_type = _after_suf;
                            if (*_after_suf != ']' && *_after_suf != '\0' && *_after_suf != ';') {
                                const char *_te = _after_suf;
                                while (*_te && *_te != ']' && *_te != ' ' && *_te != '\t') _te++;
                                size_t _tl = _te - _after_suf;
                                if (_tl > 0 && _tl < sizeof(_elem_type)) {
                                    memcpy(_elem_type, _after_suf, _tl);
                                    _elem_type[_tl] = '\0';
                                    /* Map type to byte size */
                                    if      (strcmp(_elem_type,"I8")  ==0||strcmp(_elem_type,"U8")  ==0) _elem_size = 1;
                                    else if (strcmp(_elem_type,"I16") ==0||strcmp(_elem_type,"U16") ==0) _elem_size = 2;
                                    else if (strcmp(_elem_type,"I32") ==0||strcmp(_elem_type,"U32") ==0||strcmp(_elem_type,"F32")==0) _elem_size = 4;
                                    else if (strcmp(_elem_type,"I64") ==0||strcmp(_elem_type,"U64") ==0||strcmp(_elem_type,"F64")==0) _elem_size = 8;
                                    else if (strcmp(_elem_type,"I128")==0||strcmp(_elem_type,"U128")==0) _elem_size = 16;
                                    else { strcpy(_elem_type, "U8"); _elem_size = 1; }
                                    _after_type = _te;
                                }
                            }
                            /* Skip to closing ] */
                            while (*_after_type == ' ' || *_after_type == '\t') _after_type++;
                            if (*_after_type == ']') {
                                /* Valid array declaration — compute element count */
                                unsigned long long _elem_count;
                                if (_multiplier > 0) {
                                    /* Size-based: byte_count / elem_size */
                                    if (_elem_size == 0 || (_byte_count % _elem_size) != 0) {
                                        /* Fall through to normal handling */
                                        goto _not_array_decl;
                                    }
                                    _elem_count = _byte_count / _elem_size;
                                } else {
                                    /* Plain count: use as-is */
                                    _elem_count = _num;
                                }
                                /* Architecture static array size limit check.
                                 * x86-64 uses R_X86_64_PC32 relocations for .bss
                                 * which are signed 32-bit PC-relative — the hard
                                 * ceiling is 2gb, but the runtime's own .bss
                                 * already consumes space so anything at or above
                                 * 1gb is unsafe in practice. Other architectures
                                 * have their own constraints. We catch this here,
                                 * at the earliest possible moment, before codegen. */
#if defined(__x86_64__) || defined(_M_X64)
#define MONAD_STATIC_ARR_MAX  (1ULL * 1024ULL * 1024ULL * 1024ULL)
#define MONAD_STATIC_ARR_ARCH "x86-64"
#define MONAD_STATIC_ARR_WHY  "R_X86_64_PC32 relocations are signed 32-bit; the hard ceiling is 2gb, but runtime .bss overhead makes 1gb the safe limit"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define MONAD_STATIC_ARR_MAX  (4ULL * 1024ULL * 1024ULL * 1024ULL)
#define MONAD_STATIC_ARR_ARCH "aarch64"
#define MONAD_STATIC_ARR_WHY  "AArch64 PC-relative addressing is limited to 4gb"
#elif defined(__riscv) && __riscv_xlen == 64
#define MONAD_STATIC_ARR_MAX  (2ULL * 1024ULL * 1024ULL * 1024ULL)
#define MONAD_STATIC_ARR_ARCH "riscv64"
#define MONAD_STATIC_ARR_WHY  "RISC-V PC-relative addressing uses signed 32-bit offsets"
#else
#define MONAD_STATIC_ARR_MAX  (1ULL * 1024ULL * 1024ULL * 1024ULL)
#define MONAD_STATIC_ARR_ARCH "this architecture"
#define MONAD_STATIC_ARR_WHY  "static arrays are limited by the platform's relocation range"
#endif
                                size_t _name_len = name_end - after_define;
                                unsigned long long _total_bytes = _elem_count * _elem_size;
                                if (_total_bytes >= MONAD_STATIC_ARR_MAX) {

                                    READER_ERROR(lineno, 0,
                                        "\n"
                                        "    • Static array '%.*s' is too large to allocate on %s\n"
                                        "    • Requested size : %llu bytes (%llu elements of %llu bytes)\n"
                                        "    • Maximum allowed: %llu bytes (1gb) on %s\n"
                                        "    • Reason         : %s\n"
                                        "  - Hint: use a heap-allocated fat pointer instead:\n"
                                        "      define %.*s :: Arr %s\n"
                                        "      set    %.*s (malloc %llu)",
                                        (int)_name_len, after_define,
                                        MONAD_STATIC_ARR_ARCH,
                                        _total_bytes, _elem_count, _elem_size,
                                        MONAD_STATIC_ARR_MAX, MONAD_STATIC_ARR_ARCH,
                                        MONAD_STATIC_ARR_WHY,
                                        (int)_name_len, after_define, _elem_type,
                                        (int)_name_len, after_define, _total_bytes);
                                }
#undef MONAD_STATIC_ARR_MAX
#undef MONAD_STATIC_ARR_ARCH
#undef MONAD_STATIC_ARR_WHY
                                /* Get the variable name */
                                char *_varname = strndup(after_define, _name_len);

                                /* Build: [varname :: Arr :: count :: ElemType] */
                                char _bracket[512];
                                snprintf(_bracket, sizeof(_bracket),
                                         "[%s :: Arr :: %llu :: %s]",
                                         _varname, _elem_count, _elem_type);
                                free(_varname);
                                wts_push(&s, "define", indent, lineno);
                                wts_push(&s, _bracket, indent, lineno);
                                wts_push(&s, "[]", indent, lineno);
                                free(raw);
                                lineno++;
                                goto next_line;
                            }
                        }
                    }
                }
                _not_array_decl:;
                /* Case 1: define name :: T -> ... -> Ret
                 * Also handles bare hole: define name :: ?  (no arrow needed) */
                const char *sig_after_colons = after_name + 2;
                while (*sig_after_colons == ' ' || *sig_after_colons == '\t') sig_after_colons++;
                bool sig_is_hole = (sig_after_colons[0] == '?' &&
                                    (sig_after_colons[1] == '\0' ||
                                     sig_after_colons[1] == ' '  ||
                                     sig_after_colons[1] == '\t' ||
                                     sig_after_colons[1] == ';'));
                /* Also fire for pure array-type declarations: define buf :: [512]
                 * define buf :: [16kb] etc. — no '->' but a bracketed type. */
                bool _sig_is_array_decl = false;
                {
                    const char *_sr = after_name + 2;
                    while (*_sr == ' ' || *_sr == '\t') _sr++;
                    if (*_sr == '[') _sig_is_array_decl = true;
                }
                if (after_name[0] == ':' && after_name[1] == ':' &&
                    (strstr(after_name + 2, "->") || sig_is_hole || _sig_is_array_decl)) {
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

                    /* Hole-only signature: define encode :: ?
                     * Emit as bare variable define — body provides the value.
                     * Reader sees (define encode <body>) which is correct. */
                    const char *sr = sig_rest;
                    while (*sr == ' ' || *sr == '\t') sr++;
                    bool sr_is_hole = (sr[0] == '?' &&
                                       (sr[1] == '\0' || sr[1] == ' ' ||
                                        sr[1] == '\t' || sr[1] == ';'));
                    if (sr_is_hole) {
                        /* Hole-typed define with no signature: emit as a plain
                         * variable define. The body line(s) — which may be
                         * pattern clauses like "xs -> ..." — will be consumed
                         * by the define's variadic body loop because define has
                         * arity 2 and the second arg is fname (a bare symbol),
                         * so _define_is_func is false. Instead we want the body
                         * to be parsed as a lambda with inferred param count.
                         *
                         * Strategy: scan the first body line to count how many
                         * tokens appear before the first top-level '->'. Those
                         * are the parameters. Emit a typed header like
                         * (fname __hole_0 __hole_1 -> ?) so the reader builds
                         * a proper lambda with the right param count.           */
                        int inferred_arity = 0;
                        {
                            /* Peek at the next line in the token stream.
                             * We already have the raw source pointer `p`
                             * positioned at the next line. Scan it. */
                            const char *scan = p;
                            /* skip blank/comment lines */
                            while (*scan) {
                                const char *ls = scan;
                                while (*scan && *scan != '\n') scan++;
                                if (*scan == '\n') scan++;
                                const char *t2 = ls;
                                while (*t2 == ' ' || *t2 == '\t') t2++;
                                if (!*t2 || *t2 == ';') continue;
                                /* measure indent — only process lines deeper than current */
                                int sc_indent = measure_indent(ls);
                                if (sc_indent <= indent && ls != source) break;
                                /* tokenise this line and count tokens before '->' */
                                Lexer sl; lexer_init(&sl, t2);
                                int depth2 = 0;
                                while (true) {
                                    Token st = lexer_next_token(&sl);
                                    if (st.type == TOK_EOF) { free(st.value); break; }
                                    if (st.type == TOK_ARROW && depth2 == 0) {
                                        free(st.value); break;
                                    }
                                    if (st.type == TOK_LBRACKET || st.type == TOK_LPAREN) depth2++;
                                    if (st.type == TOK_RBRACKET || st.type == TOK_RPAREN) depth2--;
                                    if (depth2 == 0) inferred_arity++;
                                    free(st.value);
                                }
                                break; /* only look at first body line */
                            }
                        }
                        /* Build header: (fname __h0 __h1 ... -> ?) */
                        SB hdr; sb_init(&hdr);
                        sb_putc(&hdr, '(');
                        sb_puts(&hdr, fname);
                        for (int _hi = 0; _hi < inferred_arity; _hi++) {
                            char _hbuf[32];
                            snprintf(_hbuf, sizeof(_hbuf), " __h%d", _hi);
                            sb_puts(&hdr, _hbuf);
                        }
                        sb_puts(&hdr, " -> [a])");
                        char *header = sb_take(&hdr);
                        arity_set(at, fname, inferred_arity);
                        wts_push(&s, "define", indent, lineno);
                        wts_push(&s, header,   indent, lineno);
                        free(header);
                        free(fname);
                        free(raw);
                        lineno++;
                        continue;
                    }

                    size_t siglen = strlen(sig_rest);
                    size_t hlen = 1 + name_len + 1 + siglen + 1 + 1;
                    char *header = malloc(hlen);
                    snprintf(header, hlen, "(%s %s)", fname, sig_rest);
                    free(fname);
                    wts_push(&s, "define", indent, lineno);
                    wts_push(&s, header, indent, lineno);
                    wts_push(&s, "[]", indent, lineno);
                    free(header);
                    free(raw);
                    lineno++;
                    continue;
                }

                /* Case 1b: define name param1 param2 ... -> body
                 * No '::' signature — bare params before '->'.
                 * e.g. "define square x -> x²"
                 * Desugar to a function header: (square x -> [a]) so the
                 * reader builds a lambda with the correct param list.
                 * We detect this by: no '::' after name, first char after
                 * name is a lowercase symbol (param name), and '->' appears
                 * somewhere on the line.                                    */
                if (after_name[0] >= 'a' && after_name[0] <= 'z' &&
                    strstr(after_name, "->")) {
                    size_t name_len = name_end - after_define;
                    char *fname = strndup(after_define, name_len);

                    /* Count params (tokens before '->') and find arrow */
                    const char *scan = after_name;
                    int arr_count = 0;
                    const char *arrow_pos = NULL;
                    while (*scan && *scan != ';') {
                        while (*scan == ' ' || *scan == '\t') scan++;
                        if (!*scan || *scan == ';') break;
                        if (*scan == '-' && *(scan+1) == '>') {
                            arrow_pos = scan;
                            break;
                        }
                        /* skip one token */
                        if (*scan == '[' || *scan == '(') {
                            char oc = *scan, cc = oc=='['?']':')';
                            int d = 0;
                            while (*scan) {
                                if (*scan==oc) d++;
                                else if (*scan==cc){d--;scan++;if(!d)break;continue;}
                                scan++;
                            }
                        } else {
                            while (*scan && *scan!=' ' && *scan!='\t' &&
                                   !(*scan=='-'&&*(scan+1)=='>')) scan++;
                        }
                        arr_count++;
                    }

                    if (arrow_pos && arr_count > 0) {
                        arity_set(at, fname, arr_count);

                        /* Emit three tokens: "define" "(fname p1 p2)" "body"
                         * so reader sees (define (fname p1 p2) body) —
                         * the standard short-form with body outside parens. */

                        /* Param string: everything before '->' */
                        size_t params_len = arrow_pos - after_name;
                        while (params_len > 0 &&
                               (after_name[params_len-1] == ' ' ||
                                after_name[params_len-1] == '\t'))
                            params_len--;

                        size_t hlen = 1 + name_len + 1 + params_len + 2;
                        char *header = malloc(hlen);
                        snprintf(header, hlen, "(%s %.*s)",
                                 fname, (int)params_len, after_name);

                        /* Body string: everything after '->' */
                        const char *body_start = arrow_pos + 2;
                        while (*body_start == ' ' || *body_start == '\t')
                            body_start++;
                        const char *body_end = get_logical_line_end(body_start);
                        size_t body_len = body_end - body_start;

                        /* Wrap in parens if multi-token and not already grouped */
                        bool body_has_space = false;
                        for (size_t _bi = 0; _bi < body_len; _bi++) {
                            if (body_start[_bi] == ' ' || body_start[_bi] == '\t') {
                                body_has_space = true;
                                break;
                            }
                        }
                        bool body_grouped = (body_len > 0 &&
                                             (body_start[0] == '(' ||
                                              body_start[0] == '[' ||
                                              body_start[0] == '{'));
                        char *body_tok;
                        if (body_has_space && !body_grouped) {
                            body_tok = malloc(body_len + 3);
                            body_tok[0] = '(';
                            memcpy(body_tok + 1, body_start, body_len);
                            body_tok[body_len + 1] = ')';
                            body_tok[body_len + 2] = '\0';
                        } else {
                            body_tok = strndup(body_start, body_len);
                        }

                        free(fname);
                        wts_push(&s, "define", indent, lineno);
                        wts_push(&s, header,   indent, lineno);
                        wts_push(&s, body_tok, indent, lineno);
                        free(header);
                        free(body_tok);
                        free(raw);
                        lineno++;
                        continue;
                    }
                    free(fname);
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
                    wts_push(&s, "[]", indent, lineno);
                    free(header);
                    free(raw);
                    lineno++;
                    continue;
                }

                /* Case 3: define name [p :: T] ... [p :: T] -> Ret
                 * The first non-space token after the name starts with '['.
                 * Only treat as function header if the bracket contains '::' or '->'
                 * (i.e. it's a typed parameter, not an array literal value). */
                if (after_name[0] == '[' && strstr(after_name, "->")) {
                    size_t name_len = name_end - after_define;
                    char *fname = strndup(after_define, name_len);

                    /* Scan to end of line (strip inline comment) */
                    const char *line_end = get_logical_line_end(after_name);

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
                    wts_push(&s, "[]", indent, lineno);
                    free(header);
                    free(raw);
                    lineno++;
                    continue;
                }
            }
        }

        /* Bare wisp-style if expression.
         * Form 1 - indentation with else keyword at same indent as if:
         *   if expr
         *     then_line1
         *     then_line2
         *   else
         *     else_line1
         *     else_line2
         * Form 2 - then/else keywords at same indent as if:
         *   if expr
         *   then stuff
         *   else stuff
         * Form 3 - fully inline:
         *   if expr then stuff else stuff
         * All forms emit: (if (expr) (then_body) (else_body))
         * Multiple body lines are wrapped in (begin ...).            */
        if (strncmp(t, "if", 2) == 0 &&
            (t[2] == ' ' || t[2] == '\t')) {

            const char *after_if = t + 2;
            while (*after_if == ' ' || *after_if == '\t') after_if++;
            const char *if_line_end = get_logical_line_end(after_if);

            /* Helper: wrap a string in parens if multi-token and not grouped */
            #define WRAP(src, dst) do { \
                bool _ag = ((src)[0]=='('||(src)[0]=='['||(src)[0]=='{'); \
                bool _st = (strchr((src),' ')==NULL && strchr((src),'\n')==NULL); \
                if (_ag || _st) { (dst) = strdup(src); } \
                else { \
                    size_t _l = strlen(src); \
                    (dst) = malloc(_l + 3); \
                    (dst)[0] = '('; \
                    memcpy((dst) + 1, (src), _l); \
                    (dst)[_l + 1] = ')'; \
                    (dst)[_l + 2] = '\0'; \
                } \
            } while(0)

            /* Helper: build a body string from an array of line strings.
             * Single line: wrap if needed. Multiple: (begin line1 line2 ...) */
            #define BUILD_BODY(lines, count, out) do { \
                if ((count) == 0) { \
                    (out) = strdup("(undefined)"); \
                } else if ((count) == 1) { \
                    WRAP((lines)[0], (out)); \
                } else { \
                    SB _bsb; sb_init(&_bsb); \
                    sb_puts(&_bsb, "(begin"); \
                    for (int _bi = 0; _bi < (count); _bi++) { \
                        sb_putc(&_bsb, ' '); \
                        char *_bw = NULL; \
                        WRAP((lines)[_bi], _bw); \
                        sb_puts(&_bsb, _bw); \
                        free(_bw); \
                    } \
                    sb_putc(&_bsb, ')'); \
                    (out) = sb_take(&_bsb); \
                } \
            } while(0)

            /* Check for Form 3: inline 'then' on same line */
            const char *inline_then = NULL;
            {
                int bdepth = 0; bool bins = false;
                for (const char *q = after_if; q < if_line_end - 4; q++) {
                    if (bins) {
                        if (*q=='\\') q++;
                        else if (*q=='"') bins = false;
                        continue;
                    }
                    if (*q=='"') { bins=true; continue; }
                    if (*q=='('||*q=='['||*q=='{') bdepth++;
                    if (*q==')'||*q==']'||*q=='}') bdepth--;
                    if (bdepth==0 &&
                        *q==' ' &&
                        strncmp(q+1,"then",4)==0 &&
                        (q[5]==' '||q[5]=='\t')) {
                        inline_then = q + 1;
                        break;
                    }
                }
            }

            char *cond_str  = NULL;
            char *then_body = NULL;
            char *else_body = NULL;

            if (inline_then) {
                /* Form 3: if expr then stuff [else stuff] */
                size_t cond_len = inline_then - after_if - 1;
                cond_str = strndup(after_if, cond_len);

                const char *after_then = inline_then + 5;
                while (*after_then==' '||*after_then=='\t') after_then++;

                /* Find inline else */
                const char *inline_else = NULL;
                {
                    int bdepth = 0; bool bins = false;
                    for (const char *q = after_then; q < if_line_end - 4; q++) {
                        if (bins) {
                            if (*q=='\\') q++;
                            else if (*q=='"') bins = false;
                            continue;
                        }
                        if (*q=='"') { bins=true; continue; }
                        if (*q=='('||*q=='['||*q=='{') bdepth++;
                        if (*q==')'||*q==']'||*q=='}') bdepth--;
                        if (bdepth==0 &&
                            *q==' ' &&
                            strncmp(q+1,"else",4)==0 &&
                            (q[5]==' '||q[5]=='\t'||q[5]=='\0')) {
                            inline_else = q + 1;
                            break;
                        }
                    }
                }

                if (inline_else) {
                    size_t then_len = inline_else - after_then - 1;
                    char *then_raw = strndup(after_then, then_len);
                    WRAP(then_raw, then_body);
                    free(then_raw);
                    const char *after_else = inline_else + 5;
                    while (*after_else==' '||*after_else=='\t') after_else++;
                    size_t else_len = if_line_end - after_else;
                    char *else_raw = strndup(after_else, else_len);
                    WRAP(else_raw, else_body);
                    free(else_raw);
                } else {
                    size_t then_len = if_line_end - after_then;
                    char *then_raw = strndup(after_then, then_len);
                    WRAP(then_raw, then_body);
                    free(then_raw);
                    else_body = strdup("(undefined)");
                }

            } else {
                /* Form 1 or Form 2: condition is rest of if line */
                cond_str = strndup(after_if, if_line_end - after_if);

                /* Collect then-lines and else-lines from subsequent lines */
                char **then_lines = NULL; int then_count = 0; int then_cap = 0;
                char **else_lines = NULL; int else_count = 0; int else_cap = 0;
                bool in_else = false;

                while (*p) {
                    const char *ls = p;
                    while (*p && *p != '\n') p++;
                    if (*p == '\n') p++;
                    char *lraw2 = strndup(ls, p - ls);
                    const char *lt2 = lraw2;
                    while (*lt2==' '||*lt2=='\t') lt2++;
                    if (!*lt2||*lt2==';') { free(lraw2); lineno++; continue; }

                    int li2 = measure_indent(lraw2);

                    /* Same-indent 'then' keyword (Form 2) */
                    if (li2 == indent &&
                        strncmp(lt2,"then",4)==0 &&
                        (lt2[4]==' '||lt2[4]=='\t')) {
                        const char *ta = lt2 + 4;
                        while (*ta==' '||*ta=='\t') ta++;
                        const char *te = get_logical_line_end(ta);
                        if (then_count >= then_cap) {
                            then_cap = then_cap ? then_cap*2 : 4;
                            then_lines = realloc(then_lines, sizeof(char*)*then_cap);
                        }
                        then_lines[then_count++] = strndup(ta, te - ta);
                        free(lraw2); lineno++;
                        continue;
                    }

                    /* Same-indent 'else' keyword (Form 1 and Form 2) */
                    if (li2 == indent &&
                        strncmp(lt2,"else",4)==0 &&
                        (lt2[4]==' '||lt2[4]=='\t'||lt2[4]=='\0')) {
                        in_else = true;
                        /* If 'else' has content on same line, grab it */
                        if (lt2[4]==' '||lt2[4]=='\t') {
                            const char *ea = lt2 + 4;
                            while (*ea==' '||*ea=='\t') ea++;
                            const char *ee = get_logical_line_end(ea);
                            if (ea < ee) {
                                if (else_count >= else_cap) {
                                    else_cap = else_cap ? else_cap*2 : 4;
                                    else_lines = realloc(else_lines, sizeof(char*)*else_cap);
                                }
                                else_lines[else_count++] = strndup(ea, ee - ea);
                            }
                        }
                        free(lraw2); lineno++;
                        continue;
                    }

                    /* Deeper indent = body line for current branch */
                    if (li2 > indent) {
                        const char *le2 = get_logical_line_end(lt2);
                        if (!in_else) {
                            if (then_count >= then_cap) {
                                then_cap = then_cap ? then_cap*2 : 4;
                                then_lines = realloc(then_lines, sizeof(char*)*then_cap);
                            }
                            then_lines[then_count++] = strndup(lt2, le2 - lt2);
                        } else {
                            if (else_count >= else_cap) {
                                else_cap = else_cap ? else_cap*2 : 4;
                                else_lines = realloc(else_lines, sizeof(char*)*else_cap);
                            }
                            else_lines[else_count++] = strndup(lt2, le2 - lt2);
                        }
                        free(lraw2); lineno++;
                        continue;
                    }

                    /* Lesser or same indent, not a keyword = end of if */
                    p = ls;
                    free(lraw2);
                    break;
                }

                BUILD_BODY(then_lines, then_count, then_body);
                BUILD_BODY(else_lines, else_count, else_body);

                for (int _i = 0; _i < then_count; _i++) free(then_lines[_i]);
                for (int _i = 0; _i < else_count; _i++) free(else_lines[_i]);
                free(then_lines);
                free(else_lines);
            }

            /* Wrap condition */
            char *wrap_cond = NULL;
            WRAP(cond_str, wrap_cond);

            size_t total = strlen(wrap_cond) + strlen(then_body) +
                           strlen(else_body) + 16;
            char *result = malloc(total);
            snprintf(result, total, "(if %s %s %s)",
                     wrap_cond, then_body, else_body);
            wts_push(&s, result, indent, lineno);
            free(result);
            free(wrap_cond);
            free(cond_str);
            free(then_body);
            free(else_body);

            #undef WRAP
            #undef BUILD_BODY

            free(raw);
            lineno++;
            continue;
        }

        tokenise_into(at, &s, t, indent, lineno);
        free(raw);
        lineno++;
        next_line:;
    }
    return s;
}

/// Recursive arity-driven parser

/*
 * Parse one complete expression from the token stream.
 * parent_indent: the indent level of the enclosing variadic form,
 *   used to stop variadic consumption. -1 means top level.
 */
static void wisp_parse_expr(ArityTable *t, WTokenStream *s, SB *out, int parent_indent, int parent_remaining, int caller_prec) {
    if (s->pos >= s->count) return;

    WToken *tok = &s->tokens[s->pos];
    const char *text = tok->text;
    int my_indent = tok->indent;
    int my_lineno = tok->lineno;

    SB prefix_sb; sb_init(&prefix_sb);

    if (parent_remaining == 0) {
        char linedir[64];
        snprintf(linedir, sizeof(linedir), "(#line %d %d) ", tok->lineno, 1);
        sb_puts(&prefix_sb, linedir);
    }

    bool is_grouped = (text[0] == '(' || text[0] == '[' || text[0] == '{' ||
                       (text[0] == '~' && text[1] == '[') ||
                       (text[0] == '#' && text[1] == '{'));

    int arity = is_grouped ? 0 : arity_get(t, text);

    if (!is_grouped) {
        /* fprintf(stderr, "DEBUG wisp_parse_expr: text='%s' arity=%d pos=%d\n", text, arity, s->pos); */
    }
    s->pos++;

    if (!is_grouped && strcmp(text, "if") == 0) {
        sb_putc(&prefix_sb, '(');
        sb_puts(&prefix_sb, "if");
        sb_putc(&prefix_sb, ' ');
        wisp_parse_expr(t, s, &prefix_sb, parent_indent, 1, 0);
        if (s->pos < s->count && strcmp(s->tokens[s->pos].text, "then") == 0) s->pos++;
        sb_putc(&prefix_sb, ' ');
        wisp_parse_expr(t, s, &prefix_sb, parent_indent, 1, 0);
        if (s->pos < s->count && strcmp(s->tokens[s->pos].text, "else") == 0) s->pos++;
        sb_putc(&prefix_sb, ' ');
        wisp_parse_expr(t, s, &prefix_sb, parent_indent, 1, 0);
        sb_putc(&prefix_sb, ')');
    }
    else if (is_grouped || arity == 0 || arity == -2) {
        /* Ratio literal in function position: if the next token is on the
         * same line and is a value (not an operator), treat the ratio as
         * a 1-arity scaling function: 25% 100 -> (25/100 100).
         * We detect ratio literals by the presence of '/' or a trailing '%'. */
        bool is_ratio_lit = false;
        if (!is_grouped && arity == -2) {
            /* Check for N/M or N% pattern */
            const char *sl = text;
            bool has_slash = (strchr(sl, '/') != NULL);
            bool has_pct   = (sl[strlen(sl) - 1] == '%');
            /* Must start with a digit or minus-digit to be a number */
            bool starts_num = (sl[0] >= '0' && sl[0] <= '9') ||
                              (sl[0] == '-' && sl[1] >= '0' && sl[1] <= '9');
            if (starts_num && (has_slash || has_pct))
                is_ratio_lit = true;
        }
        if (is_ratio_lit && s->pos < s->count &&
            s->tokens[s->pos].lineno == my_lineno) {
            /* Check the next token is a value, not an operator that would
             * be handled by the infix loop (arity >= 2). */
            int next_ar = arity_get(t, s->tokens[s->pos].text);
            bool next_is_value = (next_ar == 0 || next_ar == -2 ||
                                  s->tokens[s->pos].text[0] == '(' ||
                                  s->tokens[s->pos].text[0] == '[' ||
                                  (s->tokens[s->pos].text[0] == '~' &&
                                   s->tokens[s->pos].text[1] == '[') ||
                                  s->tokens[s->pos].text[0] == '{');
            if (next_is_value) {
                sb_putc(&prefix_sb, '(');
                sb_puts(&prefix_sb, text);
                sb_putc(&prefix_sb, ' ');
                wisp_parse_expr(t, s, &prefix_sb, my_indent, 1, 6);
                sb_putc(&prefix_sb, ')');
            } else {
                sb_puts(&prefix_sb, text);
            }
        } else {
            sb_puts(&prefix_sb, text);
        }
    }
    else {
        /* Fixed or variadic function */
        sb_putc(&prefix_sb, '(');
        sb_puts(&prefix_sb, text);

        if (arity > 0) {
            bool _define_is_func = false;
            if (strcmp(text, "define") == 0 && s->pos < s->count) {
                const char *first_arg = s->tokens[s->pos].text;
                _define_is_func = (first_arg[0] == '(');
            }
            bool args_are_bare = (strcmp(text, "import") == 0 ||
                                  strcmp(text, "module") == 0);
            ArityEntry *entry = arity_get_entry(t, text);

            for (int i = 0; i < arity && s->pos < s->count; i++) {
                sb_putc(&prefix_sb, ' ');
                ParamKind kind = (entry && i < WISP_MAX_PARAMS)
                               ? entry->param_kinds[i] : PARAM_VALUE;
                if (kind == PARAM_FUNC || args_are_bare) {
                    WToken *arg = &s->tokens[s->pos];
                    sb_puts(&prefix_sb, arg->text);
                    s->pos++;
                } else {
                    /* Function arguments bind tighter than anything else (prec 6) */
                    wisp_parse_expr(t, s, &prefix_sb, my_indent, 1, 6);
                }
            }

            if (strcmp(text, "define") == 0 && _define_is_func) {
                while (s->pos < s->count &&
                       s->tokens[s->pos].indent > my_indent) {
                    sb_putc(&prefix_sb, '\n');
                    wisp_parse_expr(t, s, &prefix_sb, my_indent, 0, 0);
                }
            }
        } else {
            /* Variadic (-1) */
            bool is_for_or_while = (strcmp(text, "for")   == 0 ||
                                    strcmp(text, "while")  == 0);
            bool first_arg_done = false;
            while (s->pos < s->count) {
                WToken *next = &s->tokens[s->pos];
                bool same_line  = (next->lineno == my_lineno);
                bool deeper     = (next->indent > my_indent &&
                                   next->lineno != my_lineno);
                if (!same_line && !deeper) break;
                if (same_line && (strcmp(next->text, "->") == 0 ||
                                  strcmp(next->text, "=>") == 0 ||
                                  strcmp(next->text, "then") == 0 ||
                                  strcmp(next->text, "else") == 0)) break;

                sb_putc(&prefix_sb, same_line ? ' ' : '\n');
                /* For 'for'/'while', the binding (first arg) must be consumed
                 * as a raw token with no Pratt infix promotion.             */
                int prec = (is_for_or_while && !first_arg_done) ? 999 : 0;
                first_arg_done = true;
                wisp_parse_expr(t, s, &prefix_sb, my_indent, 1, prec);
            }
        }
        sb_putc(&prefix_sb, ')');
    }

    /* Pratt Infix Loop */
    char *accum = sb_take(&prefix_sb);
    while (s->pos < s->count && (g_infix_fence == -1 || s->pos < g_infix_fence)) {
        WToken *op_tok = &s->tokens[s->pos];
        if (op_tok->lineno != my_lineno) break;
        bool op_atom = (op_tok->text[0] != '(' &&
                        op_tok->text[0] != '[' &&
                        !(op_tok->text[0] == '~' && op_tok->text[1] == '[') &&
                        op_tok->text[0] != '{');
        if (!op_atom) break;
        int op_ar = arity_get(t, op_tok->text);
        if (op_ar < 2 && op_ar != -1) break;

        int op_prec = op_precedence(op_tok->text);
        if (op_prec <= caller_prec) break;

        char *op_name = strdup(op_tok->text);
        s->pos++;

        SB next_acc; sb_init(&next_acc);
        sb_putc(&next_acc, '(');
        sb_puts(&next_acc, op_name);
        sb_putc(&next_acc, ' ');
        sb_puts(&next_acc, accum);
        free(op_name); free(accum);

        int rhs_slots = (op_ar > 0) ? (op_ar - 1) : 1;
        for (int r = 0; r < rhs_slots && s->pos < s->count; r++) {
            if (s->tokens[s->pos].lineno != my_lineno) break;
            sb_putc(&next_acc, ' ');
            wisp_parse_expr(t, s, &next_acc, my_indent, 1, op_prec);
        }
        sb_putc(&next_acc, ')');
        accum = sb_take(&next_acc);
    }

    sb_puts(out, accum);
    free(accum);
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

        /* All comments - replace with spaces/newlines to preserve exact line and column numbers.
         * This improves error reporting accuracy and massively improves performance
         * from O(N^2) to O(N) by eliminating memmove operations. */
        for (int j = start; j < end && j < len; j++)
            out[j] = (source[j] == '\n') ? '\n' : ' ';
    }

    return out;
}

typedef struct { char bytes[8]; int is_blank; } VChar;
typedef struct { VChar *chars; int len; } VLine;

static VLine parse_vline(const char *s) {
    VLine vl = { .chars = malloc((strlen(s) + 1) * sizeof(VChar)), .len = 0 };
    while (*s) {
        VChar vc = {0};
        if ((*s & 0x80) == 0) {
            vc.bytes[0] = *s; vc.bytes[1] = '\0'; s++;
        } else if ((*s & 0xE0) == 0xC0) {
            vc.bytes[0] = *s; vc.bytes[1] = *(s+1); vc.bytes[2] = '\0'; s += 2;
        } else if ((*s & 0xF0) == 0xE0) {
            vc.bytes[0] = *s; vc.bytes[1] = *(s+1); vc.bytes[2] = *(s+2); vc.bytes[3] = '\0'; s += 3;
        } else if ((*s & 0xF8) == 0xF0) {
            vc.bytes[0] = *s; vc.bytes[1] = *(s+1); vc.bytes[2] = *(s+2); vc.bytes[3] = *(s+3); vc.bytes[4] = '\0'; s += 4;
        } else {
            vc.bytes[0] = *s; vc.bytes[1] = '\0'; s++;
        }
        vc.is_blank = (vc.bytes[0] == ' ' || vc.bytes[0] == '\t');
        vl.chars[vl.len++] = vc;
    }
    return vl;
}

static char *vline_to_str(VLine vl) {
    char *res = malloc(vl.len * 1024 + 1);
    int pos = 0;
    for (int i = 0; i < vl.len; i++) {
        strcpy(res + pos, vl.chars[i].bytes);
        pos += strlen(vl.chars[i].bytes);
    }
    res[pos] = '\0';
    return res;
}

static char *desugar_fractions(char *source) {
    int line_count = 0;
    int cap = 16;
    char **lines = malloc(sizeof(char*) * cap);
    const char *p = source;
    while (*p) {
        const char *s = p;
        while (*p && *p != '\n') p++;
        if (line_count >= cap) { cap *= 2; lines = realloc(lines, sizeof(char*) * cap); }
        lines[line_count++] = strndup(s, p - s);
        if (*p == '\n') p++;
    }

    for (int i = 1; i < line_count - 1; i++) {
        if (strstr(lines[i], "\xE2\x94\x80")) { /* UTF-8 encoding of U+2500 '─' */
            VLine L_prev = parse_vline(lines[i-1]);
            VLine L_curr = parse_vline(lines[i]);
            VLine L_next = parse_vline(lines[i+1]);

            bool changed = false;
            int j = 0;
            while (j < L_curr.len) {
                if (strcmp(L_curr.chars[j].bytes, "\xE2\x94\x80") == 0) {
                    int run_start = j;
                    while (j < L_curr.len && strcmp(L_curr.chars[j].bytes, "\xE2\x94\x80") == 0) j++;
                    int run_end = j;

                    int n_start = run_start, n_end = run_end - 1;
                    while (n_start < L_prev.len && n_start <= n_end && L_prev.chars[n_start].is_blank) n_start++;
                    while (n_end >= 0 && n_end >= n_start && (n_end >= L_prev.len || L_prev.chars[n_end].is_blank)) n_end--;

                    int d_start = run_start, d_end = run_end - 1;
                    while (d_start < L_next.len && d_start <= d_end && L_next.chars[d_start].is_blank) d_start++;
                    while (d_end >= 0 && d_end >= d_start && (d_end >= L_next.len || L_next.chars[d_end].is_blank)) d_end--;

                    SB n_sb; sb_init(&n_sb);
                    for (int k = n_start; k <= n_end && k < L_prev.len; k++) sb_puts(&n_sb, L_prev.chars[k].bytes);
                    char *N_str = sb_take(&n_sb);
                    if (N_str[0] == '\0') { free(N_str); N_str = strdup("_"); }

                    SB d_sb; sb_init(&d_sb);
                    for (int k = d_start; k <= d_end && k < L_next.len; k++) sb_puts(&d_sb, L_next.chars[k].bytes);
                    char *D_str = sb_take(&d_sb);
                    if (D_str[0] == '\0') { free(D_str); D_str = strdup("_"); }

                    char repl[1024];
                    snprintf(repl, sizeof(repl), "(/ %s %s)", N_str, D_str);
                    free(N_str); free(D_str);

                    strcpy(L_curr.chars[run_start].bytes, repl);
                    for (int k = run_start + 1; k < run_end; k++) {
                        strcpy(L_curr.chars[k].bytes, " ");
                        L_curr.chars[k].is_blank = 1;
                    }

                    for (int k = run_start; k < run_end; k++) {
                        if (k < L_prev.len) { strcpy(L_prev.chars[k].bytes, " "); L_prev.chars[k].is_blank = 1; }
                        if (k < L_next.len) { strcpy(L_next.chars[k].bytes, " "); L_next.chars[k].is_blank = 1; }
                    }
                    changed = true;
                } else {
                    j++;
                }
            }

            if (changed) {
                free(lines[i-1]); lines[i-1] = vline_to_str(L_prev);
                free(lines[i]);   lines[i]   = vline_to_str(L_curr);
                free(lines[i+1]); lines[i+1] = vline_to_str(L_next);
            }
            free(L_prev.chars); free(L_curr.chars); free(L_next.chars);
        }
    }

    SB final_sb; sb_init(&final_sb);
    for (int i = 0; i < line_count; i++) {
        sb_puts(&final_sb, lines[i]);
        if (i < line_count - 1) sb_putc(&final_sb, '\n');
        free(lines[i]);
    }
    free(lines);

    char *ret = sb_take(&final_sb);
    free(source);
    return ret;
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

static int pure_is_known_function(const char *name) {
    if (!name) return 0;
    if (strcmp(name, "if") == 0 || strcmp(name, "cond") == 0 ||
        strcmp(name, "let") == 0 || strcmp(name, "let*") == 0 ||
        strcmp(name, "letrec") == 0 || strcmp(name, "lambda") == 0 ||
        strcmp(name, "match") == 0 || strcmp(name, "define") == 0 ||
        strcmp(name, "quote") == 0 || strcmp(name, "begin") == 0 ||
        strcmp(name, "set!") == 0 || strcmp(name, "for") == 0 ||
        strcmp(name, "while") == 0 || strcmp(name, "when") == 0 ||
        strcmp(name, "unless") == 0 || strcmp(name, "case") == 0 ||
        strcmp(name, "data") == 0 || strcmp(name, "class") == 0 ||
        strcmp(name, "instance") == 0 || strcmp(name, "type") == 0 ||
        strcmp(name, "layout") == 0 || strcmp(name, "module") == 0 ||
        strcmp(name, "import") == 0 || strcmp(name, "show") == 0 ||
        strcmp(name, "do") == 0)
        return 1;
    if (strcmp(name, "+") == 0 || strcmp(name, "-") == 0 ||
        strcmp(name, "*") == 0 || strcmp(name, "/") == 0 ||
        strcmp(name, "%") == 0)
        return 1;
    if (strcmp(name, "=") == 0 || strcmp(name, "!=") == 0 ||
        strcmp(name, "<") == 0 || strcmp(name, ">") == 0 ||
        strcmp(name, "<=") == 0 || strcmp(name, ">=") == 0)
        return 1;
    if (strcmp(name, "and") == 0 || strcmp(name, "or") == 0 ||
        strcmp(name, "not") == 0)
        return 1;
    if (strcmp(name, "car") == 0 || strcmp(name, "cdr") == 0 ||
        strcmp(name, "cons") == 0 || strcmp(name, "list") == 0 ||
        strcmp(name, "map") == 0 || strcmp(name, "filter") == 0 ||
        strcmp(name, "foldl") == 0 || strcmp(name, "foldr") == 0)
        return 1;
    if (strcmp(name, ">>=") == 0 || strcmp(name, ">>") == 0 ||
        strcmp(name, "=<<") == 0 || strcmp(name, ">=>") == 0 ||
        strcmp(name, "<=<") == 0)
        return 1;
    if (strcmp(name, "True") == 0 || strcmp(name, "False") == 0)
        return 1;
    return 0;
}

ASTList wisp_parse_all(const char *source, const char *filename) {
    /* Strip comments first, preserving line structure */
    char *stripped = strip_comments(source);

    /* 2D Fraction Desugaring */
    stripped = desugar_fractions(stripped);

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
        g_is_known_function = pure_is_known_function;
        parser_set_context(filename, stripped);
        ASTList result = parse_all(stripped);
        result = macro_expand_all(result.exprs, result.count);
        g_is_known_function = NULL;
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

    arity_set(&t, "match", -1);
    arity_set(&t, "assert-eq", 3);

    arity_prescan(&t, stripped);

    /* Build flat token stream */
    WTokenStream s = build_token_stream(stripped, &t);

    /* Parse all top-level expressions */
    SB out; sb_init(&out);
    bool first = true;
    while (s.pos < s.count) {
        if (!first) sb_putc(&out, '\n');
        first = false;
        wisp_parse_expr(&t, &s, &out, -1, 0, 0);
    }

    char *transformed = sb_take(&out);

    fprintf(stderr, "\n=== wisp expanded (%s) ===\n%s\n=== end ===\n\n",
            filename ? filename : "<input>", transformed);

    /* Install param-kind hook so reader.c can do automatic infix detection */
    g_param_kind_is_func = wisp_param_kind_is_func;
    g_is_known_function  = wisp_is_known_function;

    parser_set_context(filename, transformed);
    parser_set_original_source(source);
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
