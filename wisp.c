#include "wisp.h"
#include "compat.h"
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

static ArityTable *g_active_wisp_arities = NULL;
static bool g_wisp_trace_enabled = false;

static bool wisp_debug_enabled(void) {
    const char *v = getenv("MONAD_WISP_DEBUG");
    return v && v[0] && strcmp(v, "0") != 0;
}

static unsigned int arity_hash(const char *s) {
    unsigned int h = 5381;
    if (!s) return 0;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h % ARITY_BUCKETS;
}

static bool arity_name_valid(const char *name) {
    return name && name[0] != '\0';
}

static void arity_set(ArityTable *t, const char *name, int arity) {
    if (!t || !arity_name_valid(name)) return;

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
    if (!t || !arity_name_valid(name)) return;

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
    if (!t || !arity_name_valid(name)) return -2;

    unsigned int h = arity_hash(name);
    for (ArityEntry *e = t->buckets[h]; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e->arity;
    return -2; /* unknown */
}

static ArityEntry *arity_get_entry(ArityTable *t, const char *name) {
    if (!t || !arity_name_valid(name)) return NULL;

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

static int wisp_lookup_arity(ArityTable *t, const char *name) {
    int arity = arity_get(t, name);
    if (arity != -2)
        return arity;
    if (!g_ffi_arities_init)
        return arity;
    return arity_get(&g_ffi_arities, name);
}

void wisp_set_trace(bool enabled) {
    g_wisp_trace_enabled = enabled;
}

void wisp_clear_arities(void) {
    if (g_ffi_arities_init) {
        arity_free(&g_ffi_arities);
        g_ffi_arities_init = false;
    }
}


/// Pre-scan user-defined arities

static bool wisp_layout_lbracket_is_field(Lexer *lex) {
    if (!lex) return false;

    Lexer peek = *lex;
    Token name_tok = lexer_next_token(&peek);
    Token sep_tok  = lexer_next_token(&peek);

    bool is_field =
        name_tok.type == TOK_SYMBOL &&
        (sep_tok.type == TOK_ARROW ||
         (sep_tok.type == TOK_SYMBOL && sep_tok.value &&
          strcmp(sep_tok.value, "::") == 0));

    free(name_tok.value);
    free(sep_tok.value);
    return is_field;
}

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
                    if (tok.type == TOK_EOF || tok.type == TOK_RPAREN) { free(tok.value); break; }
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
                        if (wisp_layout_lbracket_is_field(&lex))
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
                } else if (tok.line > def_line) {
                    int clause_line = tok.line;
                    int arity = 0;
                    bool found_arrow = false;
                    int depth = 0;

                    while (tok.type != TOK_EOF && tok.line == clause_line) {
                        if (tok.type == TOK_ARROW ||
                            (tok.type == TOK_SYMBOL && tok.value &&
                             strcmp(tok.value, "->") == 0)) {
                            if (depth == 0) {
                                found_arrow = true;
                                free(tok.value);
                                break;
                            }
                        } else if (tok.type == TOK_LPAREN ||
                                   tok.type == TOK_LBRACKET) {
                            depth++;
                        } else if (tok.type == TOK_RPAREN ||
                                   tok.type == TOK_RBRACKET) {
                            if (depth > 0) depth--;
                        } else if (depth == 0 && tok.type == TOK_SYMBOL) {
                            arity++;
                        }

                        free(tok.value);
                        tok = lexer_next_token(&lex);
                    }

                    if (found_arrow && arity > 0)
                        arity_set(t, fname, arity);
                    else
                        free(tok.value);
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
                        if (wisp_layout_lbracket_is_field(&lex))
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

static bool wisp_index_ident_start_char(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= 'A' && u <= 'Z') ||
           (u >= 'a' && u <= 'z') ||
           u == '_';
}

static bool wisp_index_ident_char(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= 'A' && u <= 'Z') ||
           (u >= 'a' && u <= 'z') ||
           (u >= '0' && u <= '9') ||
           u == '_' || u == '-' || u == '?' || u == '!' || u == '.';
}

static char *wisp_rewrite_postfix_index_token(const char *text) {
    if (!text)
        return strdup("");

    {
        const char *close = strrchr(text, ')');
        if (close) {
            const char *p = close + 1;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '.') {
                const char *field = p + 1;
                if (wisp_index_ident_start_char(*field)) {
                    const char *end = field + 1;
                    while (wisp_index_ident_char(*end)) end++;
                    while (*end == ' ' || *end == '\t') end++;
                    if (*end == '\0') {
                        SB dot;
                        sb_init(&dot);
                        sb_puts(&dot, "(__dot ");
                        char *expr = strndup(text, (size_t)(close - text + 1));
                        sb_puts(&dot, expr);
                        free(expr);
                        sb_putc(&dot, ' ');
                        char *fname = strndup(field, (size_t)(end - field));
                        sb_puts(&dot, fname);
                        free(fname);
                        sb_putc(&dot, ')');
                        return sb_take(&dot);
                    }
                }
            }
        }
    }

    SB out;
    sb_init(&out);

    const char *p = text;

    while (*p) {
        if (*p == '"') {
            const char *start = p++;
            while (*p) {
                if (*p == '\\' && p[1]) {
                    p += 2;
                    continue;
                }
                if (*p == '"') {
                    p++;
                    break;
                }
                p++;
            }

            char *lit = strndup(start, (size_t)(p - start));
            sb_puts(&out, lit);
            free(lit);
            continue;
        }

        if (*p == '\'') {
            const char *start = p++;
            while (*p) {
                if (*p == '\\' && p[1]) {
                    p += 2;
                    continue;
                }
                if (*p == '\'') {
                    p++;
                    break;
                }
                p++;
            }

            char *lit = strndup(start, (size_t)(p - start));
            sb_puts(&out, lit);
            free(lit);
            continue;
        }

        if (wisp_index_ident_start_char(*p)) {
            const char *name_start = p;
            p++;

            while (wisp_index_ident_char(*p))
                p++;

            if (*p == '[') {
                const char *idx_start = p + 1;
                const char *idx = idx_start;

                while (*idx >= '0' && *idx <= '9')
                    idx++;

                if (idx > idx_start && *idx == ']') {
                    char *name = strndup(name_start,
                                         (size_t)(p - name_start));
                    char *index = strndup(idx_start,
                                          (size_t)(idx - idx_start));

                    sb_puts(&out, "(index ");
                    sb_puts(&out, name);
                    sb_putc(&out, ' ');
                    sb_puts(&out, index);
                    sb_putc(&out, ')');

                    free(name);
                    free(index);

                    p = idx + 1;
                    continue;
                }
            }

            char *name = strndup(name_start, (size_t)(p - name_start));
            sb_puts(&out, name);
            free(name);
            continue;
        }

        sb_putc(&out, *p);
        p++;
    }

    return sb_take(&out);
}

static void wts_push(WTokenStream *s, const char *text, int indent, int lineno) {
    if (!s) return;

    if (!text) {
        if (wisp_debug_enabled())
            fprintf(stderr, "[wisp] null token at line %d indent %d\n", lineno, indent);
        text = "";
    }

    if (s->count >= s->cap) {
        int new_cap = s->cap ? s->cap * 2 : 64;
        WToken *new_tokens = realloc(s->tokens, sizeof(WToken) * new_cap);
        if (!new_tokens) {
            fprintf(stderr, "[wisp] out of memory growing token stream at line %d\n", lineno);
            abort();
        }
        s->tokens = new_tokens;
        s->cap = new_cap;
    }

    s->tokens[s->count].text = wisp_rewrite_postfix_index_token(text);
    if (!s->tokens[s->count].text) {
        fprintf(stderr, "[wisp] out of memory copying token at line %d\n", lineno);
        abort();
    }

    s->tokens[s->count].indent = indent;
    s->tokens[s->count].lineno = lineno;

    if (wisp_debug_enabled())
        fprintf(stderr, "[wisp] push line=%d indent=%d token='%s'\n",
                lineno, indent, s->tokens[s->count].text);

    s->count++;
}

static void wts_free(WTokenStream *s) {
    if (!s) return;
    for (int i = 0; i < s->count; i++) {
        free(s->tokens[i].text);
        s->tokens[i].text = NULL;
    }
    free(s->tokens);
    s->tokens = NULL;
    s->count = 0;
    s->cap = 0;
    s->pos = 0;
}

static const char *get_logical_line_end(const char *start);
static WTokenStream build_token_stream(const char *source, ArityTable *at);
static void wisp_parse_expr(ArityTable *t, WTokenStream *s, SB *out,
                            int parent_indent, int parent_remaining,
                            int caller_prec);

static bool wisp_text_ends_inside_string(const char *text) {
    bool in_string = false;
    bool escape = false;

    if (!text)
        return false;

    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if (in_string) {
            if (escape) {
                escape = false;
                continue;
            }

            if (c == '\\') {
                escape = true;
                continue;
            }

            if (c == '"')
                in_string = false;

            continue;
        }

        if (c == '\'') {
            const char *q = p + 1;

            if (*q == '\\' && q[1] == 'x' && q[2] && q[3]) {
                q += 4;
            } else if (*q == '\\' && q[1]) {
                q += 2;
            } else if (*q) {
                q += 1;
            }

            if (*q == '\'') {
                p = q;
                continue;
            }
        }

        if (c == ';') {
            while (p[1] && p[1] != '\n')
                p++;
            continue;
        }

        if (c == '"') {
            in_string = true;
            escape = false;
            continue;
        }
    }

    return in_string;
}

static char *wisp_trim_range_dup(const char *start, const char *end) {
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
    return strndup(start, (size_t)(end - start));
}

static const char *wisp_skip_clause_function_name(WTokenStream *s,
                                                  const char *lscan) {
    if (!s || !lscan)
        return lscan;

    char def_name[128] = {0};

    for (int i = s->count - 1; i >= 0; i--) {
        bool is_define = strcmp(s->tokens[i].text, "define") == 0 ||
                         strcmp(s->tokens[i].text, "(define") == 0;

        if (!is_define || i + 1 >= s->count)
            continue;

        const char *name = s->tokens[i + 1].text;
        if (name[0] == '(')
            name++;

        int n = 0;
        while (name[n] &&
               name[n] != ' ' &&
               name[n] != '\t' &&
               name[n] != ')' &&
               n < 127) {
            def_name[n] = name[n];
            n++;
        }

        def_name[n] = '\0';
        break;
    }

    if (!def_name[0])
        return lscan;

    size_t len = strlen(def_name);

    if (strncmp(lscan, def_name, len) != 0)
        return lscan;

    if (lscan[len] != '\0' &&
        lscan[len] != ' ' &&
        lscan[len] != '\t')
        return lscan;

    lscan += len;
    while (*lscan == ' ' || *lscan == '\t')
        lscan++;

    return lscan;
}

static const char *wisp_find_top_level_arrow(const char *s) {
    int depth = 0;
    bool in_str = false;
    for (const char *q = s; *q; q++) {
        if (in_str) {
            if (*q == '\\' && *(q + 1)) q++;
            else if (*q == '"') in_str = false;
            continue;
        }
        if (*q == '"') { in_str = true; continue; }
        if (*q == ';') break;
        if (*q == '(' || *q == '[' || *q == '{') depth++;
        else if (*q == ')' || *q == ']' || *q == '}') { if (depth > 0) depth--; }
        else if (depth == 0 && *q == '-' && *(q + 1) == '>') return q;
    }
    return NULL;
}

static bool wisp_has_top_level_dotdot(const char *s) {
    int depth = 0;
    bool in_str = false;
    bool in_char = false;
    bool escape = false;

    for (const char *q = s; *q; q++) {
        if (in_str) {
            if (escape) escape = false;
            else if (*q == '\\') escape = true;
            else if (*q == '"') in_str = false;
            continue;
        }

        if (in_char) {
            if (escape) escape = false;
            else if (*q == '\\') escape = true;
            else if (*q == '\'') in_char = false;
            continue;
        }

        if (*q == '"') { in_str = true; continue; }
        if (*q == '\'') { in_char = true; continue; }
        if (*q == ';') break;

        if (*q == '(' || *q == '[' || *q == '{') {
            depth++;
            continue;
        }
        if (*q == ')' || *q == ']' || *q == '}') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth == 0 && q[0] == '.' && q[1] == '.')
            return true;
    }

    return false;
}

static char *wisp_wrap_bare_range_expr(const char *expr, bool *wrapped) {
    if (wrapped) *wrapped = false;
    const char *start = expr;
    while (*start == ' ' || *start == '\t') start++;
    const char *end = start + strlen(start);
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t'))
        end--;

    char *trimmed = strndup(start, (size_t)(end - start));
    if (!trimmed)
        return NULL;

    if (!wisp_has_top_level_dotdot(trimmed) ||
        trimmed[0] == '(' ||
        trimmed[0] == '[' ||
        trimmed[0] == '{')
        return trimmed;

    SB out;
    sb_init(&out);
    sb_putc(&out, '(');
    sb_puts(&out, trimmed);
    sb_putc(&out, ')');
    free(trimmed);
    if (wrapped) *wrapped = true;
    return sb_take(&out);
}

static bool wisp_has_top_level_pipe(const char *s) {
    int depth = 0;
    bool in_str = false;

    for (const char *q = s; *q; q++) {
        if (in_str) {
            if (*q == '\\' && *(q + 1)) q++;
            else if (*q == '"') in_str = false;
            continue;
        }

        if (*q == '"') {
            in_str = true;
            continue;
        }

        if (*q == ';')
            break;

        if (*q == '(' || *q == '[' || *q == '{') {
            depth++;
            continue;
        }

        if (*q == ')' || *q == ']' || *q == '}') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth == 0 && *q == '|')
            return true;
    }

    return false;
}

static const char *wisp_find_top_level_assignment(const char *s) {
    int depth = 0;
    bool in_str = false;
    for (const char *q = s; *q; q++) {
        if (in_str) {
            if (*q == '\\' && *(q + 1)) q++;
            else if (*q == '"') in_str = false;
            continue;
        }

        if (*q == '"') {
            in_str = true;
            continue;
        }

        if (*q == ';' || *q == '\n') break;

        if (*q == '\'' && *(q + 1)) {
            if (*(q + 1) == '\\' && *(q + 2) && *(q + 3) == '\'') {
                q += 3;
                continue;
            }
            if (*(q + 2) == '\'') {
                q += 2;
                continue;
            }
        }

        if (*q == '(' || *q == '[' || *q == '{') {
            depth++;
            continue;
        }

        if (*q == ')' || *q == ']' || *q == '}') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth == 0 && *q == '=' && *(q + 1) != '=' && *(q + 1) != '>') {
            char prev = q > s ? *(q - 1) : '\0';
            if (prev != '<' && prev != '>' && prev != '!' && prev != '=') {
                return q;
            }
        }
    }

    return NULL;
}

static bool wisp_is_simple_identifier_text(const char *s) {
    if (!s || !*s) return false;
    if (strcmp(s, "_") == 0) return false;
    unsigned char c = (unsigned char)s[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;
    for (const unsigned char *p = (const unsigned char *)s + 1; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '?' || *p == '!'))
            return false;
    }
    return true;
}

static bool wisp_parse_simple_param_names(const char *left, int expected,
                                          char **names) {
    Lexer lex;
    lexer_init(&lex, left);
    int count = 0;
    while (true) {
        Token tok = lexer_next_token(&lex);
        if (tok.type == TOK_EOF) { free(tok.value); break; }
        if (tok.type != TOK_SYMBOL || !wisp_is_simple_identifier_text(tok.value) ||
            count >= expected) {
            free(tok.value);
            for (int i = 0; i < count; i++) { free(names[i]); names[i] = NULL; }
            return false;
        }
        names[count++] = strdup(tok.value);
        free(tok.value);
    }
    if (count != expected) {
        for (int i = 0; i < count; i++) { free(names[i]); names[i] = NULL; }
        return false;
    }
    return true;
}

static int wisp_parse_simple_names_any(const char *left, char **names, int max_names) {
    Lexer lex;
    lexer_init(&lex, left);
    int count = 0;

    while (true) {
        Token tok = lexer_next_token(&lex);
        if (tok.type == TOK_EOF) {
            free(tok.value);
            break;
        }

        if (tok.type != TOK_SYMBOL ||
            !wisp_is_simple_identifier_text(tok.value) ||
            count >= max_names) {
            free(tok.value);
            for (int i = 0; i < count; i++) {
                free(names[i]);
                names[i] = NULL;
            }
            return -1;
        }

        names[count++] = strdup(tok.value);
        free(tok.value);
    }

    return count;
}

typedef struct WispPendingType {
    char *name;
    char *type;
    int indent;
    struct WispPendingType *next;
} WispPendingType;

static WispPendingType *g_wisp_pending_types = NULL;

static void wisp_pending_type_clear(void) {
    WispPendingType *p = g_wisp_pending_types;
    while (p) {
        WispPendingType *next = p->next;
        free(p->name);
        free(p->type);
        free(p);
        p = next;
    }
    g_wisp_pending_types = NULL;
}

static void wisp_pending_type_set(const char *name, const char *type, int indent) {
    for (WispPendingType *p = g_wisp_pending_types; p; p = p->next) {
        if (p->indent == indent && strcmp(p->name, name) == 0) {
            free(p->type);
            p->type = strdup(type);
            return;
        }
    }

    WispPendingType *p = malloc(sizeof(WispPendingType));
    p->name = strdup(name);
    p->type = strdup(type);
    p->indent = indent;
    p->next = g_wisp_pending_types;
    g_wisp_pending_types = p;
}

static char *wisp_pending_type_take(const char *name, int indent) {
    WispPendingType **pp = &g_wisp_pending_types;
    while (*pp) {
        WispPendingType *p = *pp;
        if (p->indent == indent && strcmp(p->name, name) == 0) {
            char *type = strdup(p->type);
            *pp = p->next;
            free(p->name);
            free(p->type);
            free(p);
            return type;
        }
        pp = &p->next;
    }
    return NULL;
}

static const char *wisp_find_top_level_double_colon_before(const char *start, const char *end) {
    int depth = 0;
    bool in_str = false;

    for (const char *q = start; q + 1 < end; q++) {
        if (in_str) {
            if (*q == '\\' && *(q + 1)) q++;
            else if (*q == '"') in_str = false;
            continue;
        }

        if (*q == '"') {
            in_str = true;
            continue;
        }

        if (*q == '(' || *q == '[' || *q == '{') {
            depth++;
            continue;
        }

        if (*q == ')' || *q == ']' || *q == '}') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth == 0 && q[0] == ':' && q[1] == ':') {
            return q;
        }
    }

    return NULL;
}

static bool wisp_try_store_standalone_type_decl(ArityTable *t, const char *line, int indent) {
    const char *start = line;
    while (*start == ' ' || *start == '\t') start++;

    if (!*start || *start == ';') return false;
    if (wisp_find_top_level_assignment(start)) return false;

    const char *end = get_logical_line_end(start);
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
    if (end <= start) return false;

    const char *body_start = start;
    const char *body_end = end;

    if (*body_start == '[') {
        if (*(body_end - 1) != ']') return false;
        body_start++;
        body_end--;
        while (body_start < body_end && (*body_start == ' ' || *body_start == '\t')) body_start++;
        while (body_end > body_start && (*(body_end - 1) == ' ' || *(body_end - 1) == '\t')) body_end--;
    }

    const char *type_mark = wisp_find_top_level_double_colon_before(body_start, body_end);
    if (!type_mark) return false;

    char *names_src = wisp_trim_range_dup(body_start, type_mark);
    char *type_src = wisp_trim_range_dup(type_mark + 2, body_end);

    if (!type_src[0]) {
        free(names_src);
        free(type_src);
        return false;
    }

    char *names[WISP_MAX_PARAMS];
    memset(names, 0, sizeof(names));
    int name_count = wisp_parse_simple_names_any(names_src, names, WISP_MAX_PARAMS);
    free(names_src);

    if (name_count <= 0) {
        free(type_src);
        for (int i = 0; i < WISP_MAX_PARAMS; i++) free(names[i]);
        return false;
    }

    for (int i = 0; i < name_count; i++) {
        wisp_pending_type_set(names[i], type_src, indent);
        if (t) arity_set(t, names[i], 0);
        free(names[i]);
    }

    free(type_src);
    return true;
}

static bool wisp_split_arrow_signature(const char *sig, int expected_params,
                                       char **param_types, char **ret_type) {
    const char *line_end = get_logical_line_end(sig);
    const char *seg_start = sig;
    int seg_count = 0;
    int depth = 0;
    bool in_str = false;
    for (const char *q = sig; q <= line_end; q++) {
        bool at_end = (q == line_end || *q == '\0' || *q == ';');
        if (!at_end && in_str) {
            if (*q == '\\' && *(q + 1)) q++;
            else if (*q == '"') in_str = false;
            continue;
        }
        if (!at_end) {
            if (*q == '"') { in_str = true; continue; }
            if (*q == '(' || *q == '[' || *q == '{') { depth++; continue; }
            if (*q == ')' || *q == ']' || *q == '}') { if (depth > 0) depth--; continue; }
        }
        if (at_end || (depth == 0 && *q == '-' && *(q + 1) == '>')) {
            char *seg = wisp_trim_range_dup(seg_start, q);
            if (seg_count < expected_params) param_types[seg_count] = seg;
            else if (seg_count == expected_params) *ret_type = seg;
            else free(seg);
            seg_count++;
            if (at_end) break;
            q++;
            seg_start = q + 1;
        }
    }
    if (seg_count != expected_params + 1 || !*ret_type) {
        for (int i = 0; i < expected_params; i++) { free(param_types[i]); param_types[i] = NULL; }
        free(*ret_type); *ret_type = NULL;
        return false;
    }
    return true;
}

static bool wisp_type_segment_is_ignored(const char *type) {
    if (!type) return false;
    while (*type == ' ' || *type == '\t') type++;
    const char *end = type + strlen(type);
    while (end > type && (end[-1] == ' ' || end[-1] == '\t')) end--;
    return (end - type) == 1 && type[0] == '_';
}

static void wisp_validate_ignored_signature_names(char **param_types,
                                                  char **names,
                                                  int count,
                                                  int line) {
    for (int i = 0; i < count; i++) {
        if (wisp_type_segment_is_ignored(param_types[i]) &&
            (!names[i] || strcmp(names[i], "_") != 0)) {
            READER_ERROR(line, 1,
                         "ignored signature parameter %d must use '_' in every clause",
                         i + 1);
        }
    }
}

static int wisp_function_type_arity(const char *type) {
    if (!type || !strstr(type, "->")) return -1;
    int arity = 0;
    int depth = 0;
    bool in_str = false;
    for (const char *p = type; *p; p++) {
        if (in_str) {
            if (*p == '\\' && *(p + 1)) p++;
            else if (*p == '"') in_str = false;
            continue;
        }
        if (*p == '"') { in_str = true; continue; }
        if (*p == '(' || *p == '[' || *p == '{') { depth++; continue; }
        if (*p == ')' || *p == ']' || *p == '}') { if (depth > 0) depth--; continue; }
        if (*p == '-' && *(p + 1) == '>') {
            arity++;
            p++;
        }
    }
    return arity > 0 ? arity : -1;
}

static bool wisp_error_token_is_stop(const char *text) {
    return text &&
           (strcmp(text, "->") == 0 ||
            strcmp(text, "=>") == 0 ||
            strcmp(text, "then") == 0 ||
            strcmp(text, "else") == 0);
}

static bool wisp_error_token_is_quoted_string(const char *text) {
    size_t len;

    if (!text || text[0] != '"')
        return false;

    len = strlen(text);
    return len >= 2 && text[len - 1] == '"';
}

static void wisp_error_string_put_escaped(SB *out, const char *s) {
    for (const char *p = s; p && *p; p++) {
        switch (*p) {
        case '\\': sb_puts(out, "\\\\"); break;
        case '"':  sb_puts(out, "\\\""); break;
        case '\n': sb_puts(out, "\\n"); break;
        case '\r': sb_puts(out, "\\r"); break;
        case '\t': sb_puts(out, "\\t"); break;
        default:   sb_putc(out, *p); break;
        }
    }
}

static char *wisp_quote_string_range(const char *start, const char *end) {
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;

    if (start < end && *start == '"')
        return strndup(start, (size_t)(end - start));

    SB out;
    sb_init(&out);
    sb_putc(&out, '"');
    for (const char *p = start; p < end; p++) {
        switch (*p) {
        case '\\': sb_puts(&out, "\\\\"); break;
        case '"':  sb_puts(&out, "\\\""); break;
        case '\n': sb_puts(&out, "\\n"); break;
        case '\r': sb_puts(&out, "\\r"); break;
        case '\t': sb_puts(&out, "\\t"); break;
        default:   sb_putc(&out, *p); break;
        }
    }
    sb_putc(&out, '"');
    return sb_take(&out);
}

static void wisp_append_metadata_range(SB *out, const char *start, const char *end) {
    const char *p = start;
    while (p < end && (*p == ' ' || *p == '\t')) p++;

    if (end > p && *p == ':' && p + 4 <= end &&
        strncmp(p, ":doc", 4) == 0 &&
        (p + 4 == end || p[4] == ' ' || p[4] == '\t')) {
        const char *msg = p + 4;
        char *quoted = wisp_quote_string_range(msg, end);
        sb_puts(out, ":doc ");
        sb_puts(out, quoted);
        free(quoted);
        return;
    }

    for (const char *c = start; c < end; c++)
        sb_putc(out, *c);
}

static void wisp_error_append_message_token(SB *msg, const char *text) {
    size_t len;

    if (!text)
        return;

    if (wisp_error_token_is_quoted_string(text)) {
        len = strlen(text);
        for (size_t i = 1; i + 1 < len; i++)
            sb_putc(msg, text[i]);
        return;
    }

    sb_puts(msg, text);
}

static bool wisp_error_can_consume_token(WToken *tok,
                                         int error_indent,
                                         int error_lineno) {
    bool same_line;
    bool deeper_line;

    if (!tok)
        return false;

    same_line = tok->lineno == error_lineno;
    deeper_line = tok->lineno != error_lineno && tok->indent > error_indent;

    if (!same_line && !deeper_line)
        return false;

    if (same_line && wisp_error_token_is_stop(tok->text))
        return false;

    return true;
}

static void wisp_emit_error_from_stream(WTokenStream *s,
                                        SB *out,
                                        int error_indent,
                                        int error_lineno) {
    int start_pos = s->pos;
    int end_pos = s->pos;
    int prev_lineno = error_lineno;
    SB msg;

    while (end_pos < s->count &&
           wisp_error_can_consume_token(&s->tokens[end_pos],
                                        error_indent,
                                        error_lineno)) {
        end_pos++;
    }

    if (end_pos == start_pos) {
        sb_puts(out, "(error \"error\")");
        return;
    }

    if (end_pos == start_pos + 1 &&
        wisp_error_token_is_quoted_string(s->tokens[start_pos].text)) {
        sb_puts(out, "(error ");
        sb_puts(out, s->tokens[start_pos].text);
        sb_putc(out, ')');
        s->pos = end_pos;
        return;
    }

    sb_init(&msg);

    for (int i = start_pos; i < end_pos; i++) {
        WToken *tok = &s->tokens[i];

        if (i > start_pos) {
            if (tok->lineno != prev_lineno)
                sb_putc(&msg, '\n');
            else
                sb_putc(&msg, ' ');
        }

        wisp_error_append_message_token(&msg, tok->text);
        prev_lineno = tok->lineno;
    }

    char *message = sb_take(&msg);

    sb_puts(out, "(error \"");
    wisp_error_string_put_escaped(out, message);
    sb_puts(out, "\")");

    free(message);
    s->pos = end_pos;
}

static char *wisp_try_expand_bare_error_message(const char *src) {
    if (!src)
        return NULL;

    const char *p = src;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;

    if (strncmp(p, "error", 5) != 0)
        return NULL;

    char after = p[5];
    if (after != ' ' && after != '\t' && after != '\n' && after != '\r')
        return NULL;

    const char *msg_start = p + 5;
    while (*msg_start == ' ' || *msg_start == '\t' ||
           *msg_start == '\n' || *msg_start == '\r') {
        msg_start++;
    }

    if (*msg_start == '\0' || *msg_start == '"')
        return NULL;

    const char *msg_end = msg_start + strlen(msg_start);
    while (msg_end > msg_start &&
           (msg_end[-1] == ' ' || msg_end[-1] == '\t' ||
            msg_end[-1] == '\n' || msg_end[-1] == '\r')) {
        msg_end--;
    }

    if (msg_start >= msg_end)
        return NULL;

    char *message = strndup(msg_start, (size_t)(msg_end - msg_start));

    SB out;
    sb_init(&out);
    sb_puts(&out, "(error \"");
    wisp_error_string_put_escaped(&out, message);
    sb_puts(&out, "\")");

    free(message);
    return sb_take(&out);
}

static char *wisp_expand_expr_snippet(ArityTable *at, const char *src) {
    char *error_message = wisp_try_expand_bare_error_message(src);
    if (error_message)
        return error_message;

    WTokenStream ts = build_token_stream(src, at);
    SB sb;
    sb_init(&sb);
    bool first = true;
    while (ts.pos < ts.count) {
        if (!first) sb_putc(&sb, ' ');
        first = false;
        wisp_parse_expr(at, &ts, &sb, -1, 1, 0);
    }
    wts_free(&ts);
    return sb_take(&sb);
}

static bool wisp_is_single_grouped_form(const char *src) {
    if (!src || (src[0] != '(' && src[0] != '[' && src[0] != '{'))
        return false;

    int depth = 0;
    bool in_string = false;

    for (const char *p = src; *p; p++) {
        if (in_string) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') in_string = false;
            continue;
        }

        if (*p == '"') {
            in_string = true;
            continue;
        }

        if (*p == '(' || *p == '[' || *p == '{') depth++;
        else if (*p == ')' || *p == ']' || *p == '}') {
            if (--depth == 0) {
                p++;
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                return *p == '\0';
            }
        }
    }

    return false;
}

static const char *wisp_find_top_level_fat_arrow_range(const char *start,
                                                       const char *end) {
    int depth = 0;
    bool in_str = false;
    bool in_char = false;
    bool escape = false;

    for (const char *p = start; p + 1 < end; p++) {
        if (in_str) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '"') in_str = false;
            continue;
        }

        if (in_char) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '\'') in_char = false;
            continue;
        }

        if (*p == '"') {
            in_str = true;
            continue;
        }

        if (*p == '\'') {
            in_char = true;
            continue;
        }

        if (*p == '(' || *p == '[' || *p == '{') {
            depth++;
            continue;
        }

        if (*p == ')' || *p == ']' || *p == '}') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth == 0 &&
            ((*p == '=' && p[1] == '>') ||
             (*p == '-' && p[1] == '>')))
            return p;
    }

    return NULL;
}

static bool wisp_line_is_otherwise(const char *start, const char *end) {
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;

    return (size_t)(end - start) == strlen("otherwise") &&
           strncmp(start, "otherwise", strlen("otherwise")) == 0;
}

static const char *wisp_find_top_level_condition_equal(const char *start,
                                                       const char *end) {
    int depth = 0;
    bool in_str = false;
    bool in_char = false;
    bool escape = false;

    for (const char *p = start; p < end; p++) {
        if (in_str) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '"') in_str = false;
            continue;
        }

        if (in_char) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '\'') in_char = false;
            continue;
        }

        if (*p == '"') {
            in_str = true;
            continue;
        }

        if (*p == '\'') {
            in_char = true;
            continue;
        }

        if (*p == '(' || *p == '[' || *p == '{') {
            depth++;
            continue;
        }

        if (*p == ')' || *p == ']' || *p == '}') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth == 0 && *p == '=' &&
            !(p + 1 < end && p[1] == '=') &&
            !(p + 1 < end && p[1] == '>') &&
            !(p > start && p[-1] == '<') &&
            !(p > start && p[-1] == '>') &&
            !(p > start && p[-1] == '!')) {
            return p;
        }
    }

    return NULL;
}

static char *wisp_expand_condition_snippet(ArityTable *at,
                                           const char *src) {
    const char *start = src;
    while (*start == ' ' || *start == '\t') start++;

    const char *end = get_logical_line_end(start);
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;

    const char *eq = wisp_find_top_level_condition_equal(start, end);

    if (!eq)
        return wisp_expand_expr_snippet(at, src);

    char *lhs_src = wisp_trim_range_dup(start, eq);
    char *rhs_src = wisp_trim_range_dup(eq + 1, end);

    char *lhs = wisp_expand_expr_snippet(at, lhs_src);
    char *rhs = wisp_expand_expr_snippet(at, rhs_src);

    SB out;
    sb_init(&out);
    sb_puts(&out, "(= ");
    sb_puts(&out, lhs);
    sb_putc(&out, ' ');
    sb_puts(&out, rhs);
    sb_putc(&out, ')');

    free(lhs_src);
    free(rhs_src);
    free(lhs);
    free(rhs);

    return sb_take(&out);
}

typedef struct WispGuardClause {
    char *cond;
    char *body;
    bool otherwise;
} WispGuardClause;

static bool wisp_line_is_otherwise(const char *start, const char *end);
static char *wisp_expand_condition_snippet(ArityTable *at, const char *src);
static char *wisp_try_expand_simple_where_lambda(ArityTable *at,
                                                 const char *src);
static char *wisp_try_expand_simple_where_letrec(ArityTable *at,
                                                 const char *bindings_src,
                                                 const char *body_src);
static char *wisp_expand_pointfree_guard(ArityTable *at,
                                         const char *guard_src,
                                         char **subjects,
                                         int subject_count);
static int measure_indent(const char *s);
static void wisp_append_expanded_statement(ArityTable *at, SB *out,
                                           const char *start,
                                           const char *end,
                                           int *expr_count);

static const char *wisp_find_top_level_pipe_range(const char *start,
                                                  const char *end) {
    int depth = 0;
    bool in_str = false;
    bool in_char = false;
    bool escape = false;

    for (const char *p = start; p < end; p++) {
        if (in_str) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '"') in_str = false;
            continue;
        }

        if (in_char) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '\'') in_char = false;
            continue;
        }

        if (*p == '"') {
            in_str = true;
            continue;
        }

        if (*p == '\'') {
            in_char = true;
            continue;
        }

        if (*p == '(' || *p == '[' || *p == '{') {
            depth++;
            continue;
        }

        if (*p == ')' || *p == ']' || *p == '}') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth == 0 && *p == '|')
            return p;
    }

    return NULL;
}

static bool wisp_same_param_names(char **a, int a_count,
                                  char **b, int b_count) {
    if (a_count != b_count)
        return false;

    for (int i = 0; i < a_count; i++) {
        if (strcmp(a[i], b[i]) != 0)
            return false;
    }

    return true;
}

static const char *wisp_find_top_level_binary_guard_op(const char *start,
                                                       const char *end,
                                                       size_t *op_len) {
    int depth = 0;
    bool in_str = false;
    bool in_char = false;
    bool escape = false;

    for (const char *p = start; p < end; p++) {
        if (in_str) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '"') in_str = false;
            continue;
        }

        if (in_char) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '\'') in_char = false;
            continue;
        }

        if (*p == '"') { in_str = true; continue; }
        if (*p == '\'') { in_char = true; continue; }
        if (*p == '(' || *p == '[' || *p == '{') { depth++; continue; }
        if (*p == ')' || *p == ']' || *p == '}') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth != 0)
            continue;

        if (p + 1 < end &&
            ((p[0] == '<' && p[1] == '=') ||
             (p[0] == '>' && p[1] == '=') ||
             (p[0] == '!' && p[1] == '='))) {
            *op_len = 2;
            return p;
        }

        if (*p == '<' || *p == '>' || *p == '=') {
            *op_len = 1;
            return p;
        }
    }

    return NULL;
}

static char *wisp_expand_simple_guard_condition(ArityTable *at,
                                                const char *start,
                                                const char *end) {
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;

    size_t op_len = 0;
    const char *op = wisp_find_top_level_binary_guard_op(start, end, &op_len);
    if (!op)
        return wisp_expand_condition_snippet(at, start);

    const char *lhs_end = op;
    while (lhs_end > start && (lhs_end[-1] == ' ' || lhs_end[-1] == '\t'))
        lhs_end--;

    const char *rhs_start = op + op_len;
    while (rhs_start < end && (*rhs_start == ' ' || *rhs_start == '\t'))
        rhs_start++;

    if (lhs_end <= start || rhs_start >= end)
        return wisp_expand_condition_snippet(at, start);

    char *lhs_src = strndup(start, (size_t)(lhs_end - start));
    char *rhs_src = strndup(rhs_start, (size_t)(end - rhs_start));
    char *lhs = wisp_expand_expr_snippet(at, lhs_src);
    char *rhs = wisp_expand_expr_snippet(at, rhs_src);
    free(lhs_src);
    free(rhs_src);

    SB out;
    sb_init(&out);
    sb_putc(&out, '(');
    for (size_t i = 0; i < op_len; i++)
        sb_putc(&out, op[i]);
    sb_putc(&out, ' ');
    sb_puts(&out, lhs);
    sb_putc(&out, ' ');
    sb_puts(&out, rhs);
    sb_putc(&out, ')');

    free(lhs);
    free(rhs);
    return sb_take(&out);
}

static char *wisp_try_expand_simple_where_lambda(ArityTable *at,
                                                 const char *src) {
    const char *first_start = src;
    while (*first_start == ' ' || *first_start == '\t' || *first_start == '\n')
        first_start++;
    const char *first_end = first_start;
    while (*first_end && *first_end != '\n') first_end++;
    const char *after_first = first_end;
    if (*after_first == '\n')
        after_first++;

    const char *nested = after_first;
    while (*nested == ' ' || *nested == '\t' || *nested == '\n')
        nested++;
    if (strncmp(nested, "where", 5) == 0 &&
        (nested[5] == '\0' || nested[5] == '\n' ||
         nested[5] == ' ' || nested[5] == '\t' || nested[5] == ';')) {
        char *first_line =
            strndup(first_start, (size_t)(first_end - first_start));
        const char *arrow = wisp_find_top_level_arrow(first_line);
        if (!arrow) {
            free(first_line);
            return NULL;
        }

        const char *lhs_start = first_line;
        const char *lhs_end = arrow;
        while (lhs_end > lhs_start &&
               (lhs_end[-1] == ' ' || lhs_end[-1] == '\t')) {
            lhs_end--;
        }

        if (wisp_find_top_level_pipe_range(lhs_start, lhs_end)) {
            free(first_line);
            return NULL;
        }

        char *params_src =
            strndup(lhs_start, (size_t)(lhs_end - lhs_start));
        char *params_buf[WISP_MAX_PARAMS] = {0};
        int nested_param_count =
            wisp_parse_simple_names_any(params_src, params_buf, WISP_MAX_PARAMS);
        free(params_src);

        if (nested_param_count < 0) {
            free(first_line);
            return NULL;
        }

        const char *body_start = arrow + 2;
        while (*body_start == ' ' || *body_start == '\t')
            body_start++;

        const char *bindings_start = nested;
        while (*bindings_start && *bindings_start != '\n')
            bindings_start++;
        if (*bindings_start == '\n')
            bindings_start++;

        char *body_with_nested =
            wisp_try_expand_simple_where_letrec(at, bindings_start, body_start);

        if (!body_with_nested) {
            for (int i = 0; i < nested_param_count; i++)
                free(params_buf[i]);
            free(first_line);
            return NULL;
        }

        SB out;
        sb_init(&out);
        sb_puts(&out, "(lambda (");
        for (int i = 0; i < nested_param_count; i++) {
            if (i) sb_putc(&out, ' ');
            sb_puts(&out, params_buf[i]);
            free(params_buf[i]);
        }
        sb_puts(&out, ") ");
        sb_puts(&out, body_with_nested);
        sb_putc(&out, ')');

        free(body_with_nested);
        free(first_line);
        return sb_take(&out);
    }

    char **params = NULL;
    int param_count = -1;
    WispGuardClause *clauses = NULL;
    int clause_count = 0;
    int clause_cap = 0;
    bool ok = true;

    const char *p = src;
    while (*p && ok) {
        const char *ls = p;
        while (*p && *p != '\n') p++;
        const char *le = p;
        if (*p == '\n') p++;

        const char *lt = ls;
        while (lt < le && (*lt == ' ' || *lt == '\t')) lt++;
        while (le > lt && (le[-1] == ' ' || le[-1] == '\t' || le[-1] == '\r'))
            le--;

        if (lt >= le || *lt == ';')
            continue;

        char *line = strndup(lt, (size_t)(le - lt));
        const char *arrow = wisp_find_top_level_arrow(line);
        if (!arrow) {
            free(line);
            ok = false;
            break;
        }

        const char *lhs_start = line;
        const char *lhs_end = arrow;
        while (lhs_end > lhs_start &&
               (lhs_end[-1] == ' ' || lhs_end[-1] == '\t')) {
            lhs_end--;
        }

        const char *pipe = wisp_find_top_level_pipe_range(lhs_start, lhs_end);
        const char *params_end = pipe ? pipe : lhs_end;
        while (params_end > lhs_start &&
               (params_end[-1] == ' ' || params_end[-1] == '\t')) {
            params_end--;
        }

        if (params_end == lhs_start && pipe && param_count >= 0) {
            /* Continuation guard: "| otherwise -> ..." inherits the
             * parameter list from the previous local clause. */
        } else {
            char *params_src =
                strndup(lhs_start, (size_t)(params_end - lhs_start));
            char *line_params[WISP_MAX_PARAMS] = {0};
            int line_param_count =
                wisp_parse_simple_names_any(params_src, line_params, WISP_MAX_PARAMS);
            free(params_src);

            if (line_param_count < 0) {
                free(line);
                ok = false;
                break;
            }

            if (param_count < 0) {
                param_count = line_param_count;
                params = calloc((size_t)param_count, sizeof(char *));
                for (int i = 0; i < param_count; i++)
                    params[i] = line_params[i];
            } else if (!wisp_same_param_names(params, param_count,
                                              line_params, line_param_count)) {
                for (int i = 0; i < line_param_count; i++)
                    free(line_params[i]);
                free(line);
                ok = false;
                break;
            } else {
                for (int i = 0; i < line_param_count; i++)
                    free(line_params[i]);
            }
        }

        if (clause_count >= clause_cap) {
            clause_cap = clause_cap == 0 ? 4 : clause_cap * 2;
            clauses = realloc(clauses, sizeof(WispGuardClause) * (size_t)clause_cap);
        }

        if (!pipe) {
            clauses[clause_count].otherwise = true;
            clauses[clause_count].cond = strdup("True");
        } else {
            const char *guard_start = pipe + 1;
            while (guard_start < lhs_end &&
                   (*guard_start == ' ' || *guard_start == '\t')) {
                guard_start++;
            }

            clauses[clause_count].otherwise =
                wisp_line_is_otherwise(guard_start, lhs_end);

            if (clauses[clause_count].otherwise) {
                clauses[clause_count].cond = strdup("True");
            } else {
                char *guard_src =
                    strndup(guard_start, (size_t)(lhs_end - guard_start));
                const char *gt = guard_src;
                while (*gt == ' ' || *gt == '\t') gt++;
                size_t pf_op_len = 0;
                bool pointfree_guard = false;
                if (param_count == 1) {
                    if ((gt[0] == '<' || gt[0] == '>' || gt[0] == '!') &&
                        gt[1] == '=') {
                        pf_op_len = 2;
                        pointfree_guard = true;
                    } else if (gt[0] == '<' || gt[0] == '>' || gt[0] == '=') {
                        pf_op_len = 1;
                        pointfree_guard = true;
                    }
                }
                if (pointfree_guard) {
                    const char *rhs_start = gt + pf_op_len;
                    while (*rhs_start == ' ' || *rhs_start == '\t')
                        rhs_start++;
                    char *rhs = wisp_expand_expr_snippet(at, rhs_start);
                    SB cond;
                    sb_init(&cond);
                    sb_putc(&cond, '(');
                    for (size_t i = 0; i < pf_op_len; i++)
                        sb_putc(&cond, gt[i]);
                    sb_putc(&cond, ' ');
                    sb_puts(&cond, params[0]);
                    sb_putc(&cond, ' ');
                    sb_puts(&cond, rhs);
                    sb_putc(&cond, ')');
                    free(rhs);
                    clauses[clause_count].cond = sb_take(&cond);
                } else {
                    clauses[clause_count].cond =
                        wisp_expand_simple_guard_condition(
                            at, guard_src, guard_src + strlen(guard_src));
                }
                free(guard_src);
            }
        }

        const char *body_start = arrow + 2;
        while (*body_start == ' ' || *body_start == '\t')
            body_start++;
        const char *body_end = line + strlen(line);
        while (body_end > body_start &&
               (body_end[-1] == ' ' || body_end[-1] == '\t')) {
            body_end--;
        }

        SB body;
        sb_init(&body);
        int expr_count = 0;
        wisp_append_expanded_statement(at, &body, body_start, body_end, &expr_count);

        while (*p) {
            const char *cls = p;
            while (*p && *p != '\n') p++;
            const char *cle = p;
            if (*p == '\n') p++;

            const char *clt = cls;
            while (clt < cle && (*clt == ' ' || *clt == '\t')) clt++;
            while (cle > clt && (cle[-1] == ' ' || cle[-1] == '\t' || cle[-1] == '\r'))
                cle--;

            if (clt >= cle || *clt == ';')
                continue;

            int cont_indent = measure_indent(cls);
            char *cont_line = strndup(clt, (size_t)(cle - clt));
            const char *cont_arrow = wisp_find_top_level_arrow(cont_line);
            if (cont_indent <= 0 || cont_arrow) {
                free(cont_line);
                p = cls;
                break;
            }

            const char *cont_end = get_logical_line_end(cont_line);
            const char *cont_stmt = cont_line;
            char *joined_if = NULL;
            if (strncmp(cont_line, "if ", 3) == 0 &&
                strstr(cont_line, " else ") == NULL && *p) {
                const char *els = p;
                const char *ele = p;
                while (*ele && *ele != '\n') ele++;
                const char *elt = els;
                while (elt < ele && (*elt == ' ' || *elt == '\t')) elt++;
                if (ele - elt >= 4 && strncmp(elt, "else", 4) == 0 &&
                    (elt + 4 == ele || elt[4] == ' ' || elt[4] == '\t')) {
                    SB joined;
                    sb_init(&joined);
                    sb_puts(&joined, cont_line);
                    sb_putc(&joined, ' ');
                    sb_puts(&joined, elt);
                    joined_if = sb_take(&joined);
                    cont_stmt = joined_if;
                    p = (*ele == '\n') ? ele + 1 : ele;
                }
            }
            cont_end = get_logical_line_end(cont_stmt);
            wisp_append_expanded_statement(at, &body, cont_stmt, cont_end, &expr_count);
            free(joined_if);
            free(cont_line);
        }

        char *body_text = sb_take(&body);
        if (expr_count > 1) {
            SB seq;
            sb_init(&seq);
            sb_puts(&seq, "(begin ");
            sb_puts(&seq, body_text);
            sb_putc(&seq, ')');
            clauses[clause_count].body = sb_take(&seq);
            free(body_text);
        } else {
            clauses[clause_count].body = body_text;
        }
        clause_count++;

        free(line);
    }

    if (!ok || clause_count == 0 || param_count < 0) {
        if (params) {
            for (int i = 0; i < param_count; i++)
                free(params[i]);
            free(params);
        }
        for (int i = 0; i < clause_count; i++) {
            free(clauses[i].cond);
            free(clauses[i].body);
        }
        free(clauses);
        return NULL;
    }

    char *body = strdup("(undefined)");
    for (int i = clause_count - 1; i >= 0; i--) {
        if (clauses[i].otherwise) {
            free(body);
            body = strdup(clauses[i].body);
            continue;
        }

        SB next;
        sb_init(&next);
        sb_puts(&next, "(if ");
        sb_puts(&next, clauses[i].cond);
        sb_putc(&next, ' ');
        sb_puts(&next, clauses[i].body);
        sb_putc(&next, ' ');
        sb_puts(&next, body);
        sb_putc(&next, ')');
        free(body);
        body = sb_take(&next);
    }

    SB out;
    sb_init(&out);
    sb_puts(&out, "(lambda (");
    for (int i = 0; i < param_count; i++) {
        if (i) sb_putc(&out, ' ');
        sb_puts(&out, params[i]);
    }
    sb_puts(&out, ") ");
    sb_puts(&out, body);
    sb_putc(&out, ')');

    for (int i = 0; i < param_count; i++)
        free(params[i]);
    free(params);
    for (int i = 0; i < clause_count; i++) {
        free(clauses[i].cond);
        free(clauses[i].body);
    }
    free(clauses);
    free(body);

    return sb_take(&out);
}

typedef struct WispSimpleWhereBind {
    char *name;
    char *rest;
} WispSimpleWhereBind;

static char *wisp_try_expand_simple_where_letrec(ArityTable *at,
                                                 const char *bindings_src,
                                                 const char *body_src) {
    WispSimpleWhereBind *binds = NULL;
    int bind_count = 0;
    int bind_cap = 0;

    const char *p = bindings_src;
    while (*p) {
        const char *ls = p;
        while (*p && *p != '\n') p++;
        const char *le = p;
        if (*p == '\n') p++;

        const char *lt = ls;
        while (lt < le && (*lt == ' ' || *lt == '\t')) lt++;
        while (le > lt && (le[-1] == ' ' || le[-1] == '\t' || le[-1] == '\r'))
            le--;

        if (lt >= le || *lt == ';')
            continue;

        if (*lt == '|') {
            if (bind_count == 0) {
                goto fail;
            }

            size_t old_len = strlen(binds[bind_count - 1].rest);
            size_t add_len = (size_t)(le - lt);
            binds[bind_count - 1].rest =
                realloc(binds[bind_count - 1].rest, old_len + add_len + 2);
            binds[bind_count - 1].rest[old_len++] = '\n';
            memcpy(binds[bind_count - 1].rest + old_len, lt, add_len);
            binds[bind_count - 1].rest[old_len + add_len] = '\0';
            continue;
        }

        const char *name_start = lt;
        const char *name_end = lt;
        while (name_end < le &&
               *name_end != ' ' && *name_end != '\t' &&
               *name_end != '=' && *name_end != '-' && *name_end != '|') {
            name_end++;
        }

        if (name_end == name_start)
            goto fail;

        const char *rest_start = name_end;
        while (rest_start < le && (*rest_start == ' ' || *rest_start == '\t'))
            rest_start++;

        char *name = strndup(name_start, (size_t)(name_end - name_start));
        char *rest = strndup(rest_start, (size_t)(le - rest_start));

        int existing = -1;
        for (int i = 0; i < bind_count; i++) {
            if (strcmp(binds[i].name, name) == 0) {
                existing = i;
                break;
            }
        }

        if (existing >= 0) {
            size_t old_len = strlen(binds[existing].rest);
            size_t add_len = strlen(rest);
            binds[existing].rest =
                realloc(binds[existing].rest, old_len + add_len + 2);
            binds[existing].rest[old_len++] = '\n';
            memcpy(binds[existing].rest + old_len, rest, add_len + 1);
            free(name);
            free(rest);
            continue;
        }

        if (bind_count >= bind_cap) {
            bind_cap = bind_cap == 0 ? 4 : bind_cap * 2;
            binds = realloc(binds, sizeof(WispSimpleWhereBind) * (size_t)bind_cap);
        }

        binds[bind_count].name = name;
        binds[bind_count].rest = rest;
        bind_count++;
    }

    if (bind_count == 0)
        goto fail;

    for (int i = 0; i < bind_count; i++) {
        const char *rt = binds[i].rest;
        int arity = 0;
        while (*rt) {
            while (*rt == ' ' || *rt == '\t') rt++;
            if (!*rt || *rt == '|' || (*rt == '-' && rt[1] == '>'))
                break;
            while (*rt && *rt != ' ' && *rt != '\t' && *rt != '|' &&
                   !(*rt == '-' && rt[1] == '>')) {
                rt++;
            }
            arity++;
        }
        arity_set(at, binds[i].name, arity);
    }

    char *body_expanded = wisp_expand_expr_snippet(at, body_src);

    SB out;
    sb_init(&out);
    sb_puts(&out, "(letrec (");
    for (int i = 0; i < bind_count; i++) {
        char *lambda = wisp_try_expand_simple_where_lambda(at, binds[i].rest);
        if (!lambda) {
            free(body_expanded);
            char *partial = sb_take(&out);
            free(partial);
            goto fail;
        }

        sb_puts(&out, "[");
        sb_puts(&out, binds[i].name);
        sb_putc(&out, ' ');
        sb_puts(&out, lambda);
        sb_puts(&out, "] ");
        free(lambda);
    }
    sb_puts(&out, ") ");
    sb_puts(&out, body_expanded);
    sb_putc(&out, ')');

    free(body_expanded);
    for (int i = 0; i < bind_count; i++) {
        free(binds[i].name);
        free(binds[i].rest);
    }
    free(binds);
    return sb_take(&out);

fail:
    for (int i = 0; i < bind_count; i++) {
        free(binds[i].name);
        free(binds[i].rest);
    }
    free(binds);
    return NULL;
}

static char *wisp_expand_instance_method_body(ArityTable *at,
                                              const char *src) {
    if (!src)
        return strdup("(undefined)");

    WispGuardClause *clauses = NULL;
    int clause_count = 0;
    int clause_cap = 0;
    bool saw_guard_arrow = false;
    bool saw_non_guard_line = false;

    const char *p = src;

    while (*p) {
        const char *ls = p;
        while (*p && *p != '\n') p++;
        const char *le = p;
        if (*p == '\n') p++;

        const char *lt = ls;
        while (lt < le && (*lt == ' ' || *lt == '\t')) lt++;

        const char *lt_end = lt;
        bool in_str = false;
        while (lt_end < le) {
            if (in_str) {
                if (*lt_end == '\\' && lt_end + 1 < le) {
                    lt_end += 2;
                    continue;
                }
                if (*lt_end == '"') in_str = false;
                lt_end++;
                continue;
            }

            if (*lt_end == '"') {
                in_str = true;
                lt_end++;
                continue;
            }

            if (*lt_end == ';')
                break;

            lt_end++;
        }

        while (lt_end > lt &&
               (*(lt_end - 1) == ' ' || *(lt_end - 1) == '\t')) {
            lt_end--;
        }

        if (lt >= lt_end)
            continue;

        const char *arrow = wisp_find_top_level_arrow(lt);

        if (!arrow || arrow >= lt_end) {
            saw_non_guard_line = true;
            continue;
        }

        saw_guard_arrow = true;

        if (clause_count >= clause_cap) {
            clause_cap = clause_cap == 0 ? 4 : clause_cap * 2;
            clauses = realloc(clauses, sizeof(WispGuardClause) * clause_cap);
        }

        clauses[clause_count].otherwise = wisp_line_is_otherwise(lt, arrow);

        if (clauses[clause_count].otherwise) {
            clauses[clause_count].cond = strdup("True");
        } else {
            char *cond_src = wisp_trim_range_dup(lt, arrow);
            clauses[clause_count].cond = wisp_expand_condition_snippet(at, cond_src);
            free(cond_src);
        }

        const char *body_start = arrow + 2;
        while (body_start < lt_end &&
               (*body_start == ' ' || *body_start == '\t')) {
            body_start++;
        }

        char *body_src = wisp_trim_range_dup(body_start, lt_end);
        clauses[clause_count].body = wisp_expand_expr_snippet(at, body_src);
        free(body_src);

        clause_count++;
    }

    if (!saw_guard_arrow || saw_non_guard_line) {
        for (int i = 0; i < clause_count; i++) {
            free(clauses[i].cond);
            free(clauses[i].body);
        }
        free(clauses);
        return wisp_expand_expr_snippet(at, src);
    }

    char *result = strdup("(undefined)");

    for (int i = clause_count - 1; i >= 0; i--) {
        if (clauses[i].otherwise) {
            free(result);
            result = strdup(clauses[i].body);
            continue;
        }

        SB next;
        sb_init(&next);
        sb_puts(&next, "(if ");
        sb_puts(&next, clauses[i].cond);
        sb_putc(&next, ' ');
        sb_puts(&next, clauses[i].body);
        sb_putc(&next, ' ');
        sb_puts(&next, result);
        sb_putc(&next, ')');

        free(result);
        result = sb_take(&next);
    }

    for (int i = 0; i < clause_count; i++) {
        free(clauses[i].cond);
        free(clauses[i].body);
    }
    free(clauses);

    return result;
}

static bool wisp_grid_ident_start_char(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= 'A' && u <= 'Z') ||
           (u >= 'a' && u <= 'z') ||
           u == '_';
}

static bool wisp_grid_ident_char(char c) {
    unsigned char u = (unsigned char)c;
    return wisp_grid_ident_start_char(c) ||
           (u >= '0' && u <= '9') ||
           u == '-' || u == '?' || u == '!' || u == '.';
}

static bool wisp_grid_word_at(const char *p, const char *end,
                              const char *word) {
    size_t len = strlen(word);

    if (!p || !end || p + len > end)
        return false;

    if (strncmp(p, word, len) != 0)
        return false;

    if (p + len < end && wisp_grid_ident_char(p[len]))
        return false;

    return true;
}

static const char *wisp_find_top_level_assignment_range(const char *start,
                                                        const char *end) {
    int depth = 0;
    bool in_str = false;
    bool in_char = false;
    bool escape = false;

    for (const char *p = start; p < end; p++) {
        if (in_str) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '"') in_str = false;
            continue;
        }

        if (in_char) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '\'') in_char = false;
            continue;
        }

        if (*p == '"') {
            in_str = true;
            continue;
        }

        if (*p == '\'') {
            in_char = true;
            continue;
        }

        if (*p == '(' || *p == '[' || *p == '{') {
            depth++;
            continue;
        }

        if (*p == ')' || *p == ']' || *p == '}') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth == 0 && *p == '=' &&
            !(p + 1 < end && p[1] == '=') &&
            !(p + 1 < end && p[1] == '>')) {
            return p;
        }
    }

    return NULL;
}

static const char *wisp_find_next_grid_assignment_start(const char *start,
                                                        const char *end) {
    int depth = 0;
    bool in_str = false;
    bool in_char = false;
    bool escape = false;

    for (const char *p = start; p < end; p++) {
        if (in_str) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '"') in_str = false;
            continue;
        }

        if (in_char) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '\'') in_char = false;
            continue;
        }

        if (*p == '"') {
            in_str = true;
            continue;
        }

        if (*p == '\'') {
            in_char = true;
            continue;
        }

        if (*p == '(' || *p == '[' || *p == '{') {
            depth++;
            continue;
        }

        if (*p == ')' || *p == ']' || *p == '}') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth != 0 || *p != '=' ||
            (p + 1 < end && p[1] == '=') ||
            (p + 1 < end && p[1] == '>')) {
            continue;
        }

        const char *lhs_end = p;
        while (lhs_end > start && (*(lhs_end - 1) == ' ' ||
                                   *(lhs_end - 1) == '\t')) {
            lhs_end--;
        }

        const char *lhs_start = lhs_end;
        while (lhs_start > start && wisp_grid_ident_char(*(lhs_start - 1)))
            lhs_start--;

        if (lhs_start == lhs_end || !wisp_grid_ident_start_char(*lhs_start))
            continue;

        const char *space = lhs_start;
        int space_count = 0;
        while (space > start && (*(space - 1) == ' ' || *(space - 1) == '\t')) {
            space--;
            space_count++;
        }

        if (space_count >= 2)
            return lhs_start;
    }

    return NULL;
}

static bool wisp_emit_assignment_grid_row(ArityTable *at,
                                          WTokenStream *s,
                                          const char *line_start,
                                          const char *line_end,
                                          int indent,
                                          int lineno) {
    const char *first_assign =
        wisp_find_top_level_assignment_range(line_start, line_end);

    if (!first_assign)
        return false;

    const char *first_value = first_assign + 1;
    while (first_value < line_end &&
           (*first_value == ' ' || *first_value == '\t')) {
        first_value++;
    }

    const char *second_start =
        wisp_find_next_grid_assignment_start(first_value, line_end);

    if (!second_start)
        return false;

    const char *cur = line_start;

    while (cur < line_end) {
        while (cur < line_end && (*cur == ' ' || *cur == '\t'))
            cur++;

        if (cur >= line_end)
            break;

        const char *assign =
            wisp_find_top_level_assignment_range(cur, line_end);
        if (!assign)
            break;

        char *lhs = wisp_trim_range_dup(cur, assign);
        char *names[WISP_MAX_PARAMS];
        memset(names, 0, sizeof(names));

        int name_count = wisp_parse_simple_names_any(lhs, names,
                                                     WISP_MAX_PARAMS);
        free(lhs);

        if (name_count != 1) {
            for (int i = 0; i < WISP_MAX_PARAMS; i++)
                free(names[i]);
            return false;
        }

        const char *value_start = assign + 1;
        while (value_start < line_end &&
               (*value_start == ' ' || *value_start == '\t')) {
            value_start++;
        }

        const char *next_start =
            wisp_find_next_grid_assignment_start(value_start, line_end);

        const char *value_end = next_start ? next_start : line_end;
        while (value_end > value_start &&
               (*(value_end - 1) == ' ' || *(value_end - 1) == '\t')) {
            value_end--;
        }

        char *value_src = strndup(value_start,
                                  (size_t)(value_end - value_start));
        char *value_tok = wisp_expand_expr_snippet(at, value_src);
        free(value_src);

        SB form;
        sb_init(&form);
        sb_puts(&form, "(var ");
        sb_puts(&form, names[0]);
        sb_putc(&form, ' ');
        sb_puts(&form, value_tok);
        sb_putc(&form, ')');

        char *form_text = sb_take(&form);
        wts_push(s, form_text, indent, lineno);

        free(form_text);
        free(value_tok);
        for (int i = 0; i < WISP_MAX_PARAMS; i++)
            free(names[i]);

        if (!next_start)
            break;

        cur = next_start;
    }

    return true;
}

static const char *wisp_find_next_inline_def_start(const char *start,
                                                   const char *end) {
    int depth = 0;
    bool in_str = false;
    bool in_char = false;
    bool escape = false;

    for (const char *p = start; p < end; p++) {
        if (in_str) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '"') in_str = false;
            continue;
        }

        if (in_char) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '\'') in_char = false;
            continue;
        }

        if (*p == '"') {
            in_str = true;
            continue;
        }

        if (*p == '\'') {
            in_char = true;
            continue;
        }

        if (*p == '(' || *p == '[' || *p == '{') {
            depth++;
            continue;
        }

        if (*p == ')' || *p == ']' || *p == '}') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth == 0 && wisp_grid_word_at(p, end, "def")) {
            const char *space = p;
            int space_count = 0;

            while (space > start &&
                   (*(space - 1) == ' ' || *(space - 1) == '\t')) {
                space--;
                space_count++;
            }

            if (space_count >= 2)
                return p;
        }
    }

    return NULL;
}

static bool wisp_emit_inline_def_row(ArityTable *at,
                                     WTokenStream *s,
                                     const char *line_start,
                                     const char *line_end,
                                     int indent,
                                     int lineno) {
    const char *cur = line_start;

    while (cur < line_end && (*cur == ' ' || *cur == '\t'))
        cur++;

    if (!wisp_grid_word_at(cur, line_end, "def"))
        return false;

    while (cur < line_end) {
        if (!wisp_grid_word_at(cur, line_end, "def"))
            return false;

        cur += 3;
        while (cur < line_end && (*cur == ' ' || *cur == '\t'))
            cur++;

        char *name = NULL;

        if (cur + 3 <= line_end &&
            (unsigned char)cur[0] == 0xE2 &&
            (unsigned char)cur[1] == 0x8C &&
            (unsigned char)cur[2] == 0x9C) {
            const char *inner_start = cur + 3;
            const char *inner_end = NULL;

            for (const char *q = inner_start; q + 2 < line_end; q++) {
                if ((unsigned char)q[0] == 0xE2 &&
                    (unsigned char)q[1] == 0x8C &&
                    (unsigned char)q[2] == 0x9D) {
                    inner_end = q;
                    break;
                }
            }

            if (!inner_end || inner_end == inner_start)
                return false;

            name = strndup(inner_start, (size_t)(inner_end - inner_start));
            if (!wisp_is_simple_identifier_text(name)) {
                free(name);
                return false;
            }

            cur = inner_end + 3;
        } else {
            const char *name_start = cur;
            if (name_start >= line_end || !wisp_grid_ident_start_char(*name_start))
                return false;

            cur++;
            while (cur < line_end && wisp_grid_ident_char(*cur))
                cur++;

            name = strndup(name_start, (size_t)(cur - name_start));
        }

        while (cur < line_end && (*cur == ' ' || *cur == '\t'))
            cur++;

        if (cur < line_end && *cur == '=') {
            cur++;
            while (cur < line_end && (*cur == ' ' || *cur == '\t'))
                cur++;
        }

        const char *value_start = cur;
        const char *next_def =
            wisp_find_next_inline_def_start(value_start, line_end);
        const char *value_end = next_def ? next_def : line_end;

        while (value_end > value_start &&
               (*(value_end - 1) == ' ' || *(value_end - 1) == '\t')) {
            value_end--;
        }

        if (value_start >= value_end) {
            free(name);
            return false;
        }

        char *value_src = strndup(value_start,
                                  (size_t)(value_end - value_start));
        char *value_tok = wisp_expand_expr_snippet(at, value_src);
        free(value_src);

        arity_set(at, name, 0);

        SB form;
        sb_init(&form);
        sb_puts(&form, "(def ");
        sb_puts(&form, name);
        sb_putc(&form, ' ');
        sb_puts(&form, value_tok);
        sb_putc(&form, ')');

        char *form_text = sb_take(&form);
        wts_push(s, form_text, indent, lineno);

        free(form_text);
        free(value_tok);
        free(name);

        if (!next_def)
            break;

        cur = next_def;
    }

    return true;
}

static const char *wisp_skip_space_ptr(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *wisp_trim_end_ptr(const char *start, const char *end) {
    while (end > start &&
           (*(end - 1) == ' ' || *(end - 1) == '\t' ||
            *(end - 1) == '\n' || *(end - 1) == '\r')) {
        end--;
    }
    return end;
}

static const char *wisp_guard_expr_end(const char *p, const char *end) {
    p = wisp_skip_space_ptr(p, end);
    if (p >= end) return p;

    if (*p == '"') {
        p++;
        while (p < end) {
            if (*p == '\\' && p + 1 < end) {
                p += 2;
                continue;
            }
            if (*p == '"') {
                p++;
                break;
            }
            p++;
        }
        return p;
    }

    if (*p == '\'') {
        p++;
        if (p < end && *p == '\\') p++;
        if (p < end) p++;
        if (p < end && *p == '\'') p++;
        return p;
    }

    if (*p == '(' || *p == '[' || *p == '{') {
        char open = *p;
        char close = open == '(' ? ')' : open == '[' ? ']' : '}';
        int depth = 0;
        bool in_str = false;

        while (p < end) {
            if (in_str) {
                if (*p == '\\' && p + 1 < end) {
                    p += 2;
                    continue;
                }
                if (*p == '"') in_str = false;
                p++;
                continue;
            }

            if (*p == '"') {
                in_str = true;
                p++;
                continue;
            }

            if (*p == '\'') {
                p++;
                if (p < end && *p == '\\') p++;
                if (p < end) p++;
                if (p < end && *p == '\'') p++;
                continue;
            }

            if (*p == open) depth++;
            if (*p == close) {
                depth--;
                p++;
                if (depth == 0) break;
                continue;
            }

            p++;
        }

        return p;
    }

    while (p < end &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        p++;
    }

    return p;
}

static bool wisp_guard_special_head(const char *head) {
    if (!head) return true;
    return strcmp(head, "and") == 0 ||
           strcmp(head, "or") == 0 ||
           strcmp(head, "not") == 0 ||
           strcmp(head, "if") == 0 ||
           strcmp(head, "cond") == 0 ||
           strcmp(head, "quote") == 0 ||
           strcmp(head, "quasiquote") == 0 ||
           strcmp(head, "lambda") == 0 ||
           strcmp(head, "define") == 0 ||
           strcmp(head, "def") == 0 ||
           strcmp(head, "var") == 0 ||
           strcmp(head, "let") == 0 ||
           strcmp(head, "letrec") == 0 ||
           strcmp(head, "match") == 0 ||
           strcmp(head, "pmatch") == 0 ||
           strcmp(head, "otherwise") == 0 ||
           strcmp(head, "True") == 0 ||
           strcmp(head, "False") == 0;
}

static void wisp_guard_append_subjects(SB *out, char **subjects, int subject_count) {
    for (int i = 0; i < subject_count; i++) {
        sb_putc(out, ' ');
        sb_puts(out, subjects[i]);
    }
}

static bool wisp_name_looks_unary_predicate(const char *name) {
    size_t len = name ? strlen(name) : 0;
    return len > 0 && name[len - 1] == '?';
}

static void wisp_guard_append_implicit_subjects(ArityTable *at,
                                                SB *out,
                                                const char *head,
                                                char **subjects,
                                                int subject_count) {
    if (!subjects || subject_count <= 0)
        return;

    int arity = at ? arity_get(at, head) : -2;

    if (arity == 1 ||
        (arity == -2 && wisp_name_looks_unary_predicate(head))) {
        sb_putc(out, ' ');
        sb_puts(out, subjects[subject_count - 1]);
        return;
    }

    wisp_guard_append_subjects(out, subjects, subject_count);
}

static bool wisp_guard_bare_should_not_apply(const char *s) {
    if (!s || !*s) return true;
    if (wisp_guard_special_head(s)) return true;
    if (strcmp(s, "_") == 0) return true;

    if ((s[0] >= '0' && s[0] <= '9') ||
        ((s[0] == '-' || s[0] == '+') && s[1] >= '0' && s[1] <= '9')) {
        return true;
    }

    if (s[0] == '"' || s[0] == '\'' || s[0] == ':' ||
        s[0] == '[' || s[0] == '{') {
        return true;
    }

    return false;
}

static bool wisp_guard_bare_is_subject(const char *s, char **subjects, int subject_count) {
    if (!s || !subjects) return false;
    for (int i = 0; i < subject_count; i++) {
        if (subjects[i] && strcmp(s, subjects[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool wisp_guard_operator_head_p(const char *s) {
    if (!s) return false;

    return strcmp(s, ">=") == 0 ||
           strcmp(s, "<=") == 0 ||
           strcmp(s, ">") == 0 ||
           strcmp(s, "<") == 0 ||
           strcmp(s, "=") == 0 ||
           strcmp(s, "!=") == 0 ||
           strcmp(s, "+") == 0 ||
           strcmp(s, "-") == 0 ||
           strcmp(s, "*") == 0 ||
           strcmp(s, "/") == 0 ||
           strcmp(s, "mod") == 0 ||
           strcmp(s, "%") == 0;
}

static char *wisp_guard_apply_expr(ArityTable *at,
                                   const char *start,
                                   const char *end,
                                   char **subjects,
                                   int subject_count) {
    start = wisp_skip_space_ptr(start, end);
    end = wisp_trim_end_ptr(start, end);

    if (start >= end) return strdup("");

    if (*start != '(' || *(end - 1) != ')') {
        char *bare = strndup(start, (size_t)(end - start));

        if (subject_count <= 0 ||
            wisp_guard_bare_should_not_apply(bare) ||
            wisp_guard_bare_is_subject(bare, subjects, subject_count)) {
            return bare;
        }

        SB out;
        sb_init(&out);
        sb_putc(&out, '(');
        sb_puts(&out, bare);
        wisp_guard_append_implicit_subjects(at, &out, bare,
                                            subjects, subject_count);
        sb_putc(&out, ')');

        free(bare);
        return sb_take(&out);
    }

    const char *inner_start = start + 1;
    const char *inner_end = end - 1;
    inner_start = wisp_skip_space_ptr(inner_start, inner_end);

    if (inner_start >= inner_end) {
        return strndup(start, (size_t)(end - start));
    }

    const char *head_end = wisp_guard_expr_end(inner_start, inner_end);
    char *head = strndup(inner_start, (size_t)(head_end - inner_start));

    SB out;
    sb_init(&out);
    sb_putc(&out, '(');
    sb_puts(&out, head);

    int arg_count = 0;
    int arity = at ? arity_get(at, head) : -2;
    ArityEntry *entry = at ? arity_get_entry(at, head) : NULL;
    const char *p = head_end;

    while (true) {
        p = wisp_skip_space_ptr(p, inner_end);
        if (p >= inner_end) break;

        const char *arg_end = wisp_guard_expr_end(p, inner_end);
        char *rewritten = NULL;
        bool arg_is_func_slot = entry &&
                                arg_count < WISP_MAX_PARAMS &&
                                entry->param_kinds[arg_count] == PARAM_FUNC;
        bool preserve_later_fixed_slots = false;
        if (!arg_is_func_slot && arity > 0 && arg_count < arity - 1) {
            const char *ap = wisp_skip_space_ptr(p, arg_end);
            const char *ae = wisp_trim_end_ptr(ap, arg_end);
            bool arg_is_bare = ap < ae &&
                               *ap != '(' && *ap != '[' && *ap != '{' &&
                               *ap != '"' && *ap != '\'';
            if (arg_is_bare) {
                char *arg_name = strndup(ap, (size_t)(ae - ap));
                preserve_later_fixed_slots = arity_get(at, arg_name) > 0;
                free(arg_name);
            }
        }
        if (arg_is_func_slot ||
            preserve_later_fixed_slots ||
            wisp_guard_operator_head_p(head)) {
            rewritten = strndup(p, (size_t)(arg_end - p));
        } else {
            rewritten = wisp_guard_apply_expr(at, p, arg_end,
                                              subjects, subject_count);
        }

        sb_putc(&out, ' ');
        sb_puts(&out, rewritten);

        free(rewritten);
        arg_count++;
        p = arg_end;
    }

    if (!wisp_guard_special_head(head)) {
        if (arg_count == 0 && subject_count > 0) {
            wisp_guard_append_implicit_subjects(at, &out, head,
                                                subjects, subject_count);
        } else if (subject_count == 1 &&
                   arity > 0 &&
                   arg_count == arity - 1) {
            sb_putc(&out, ' ');
            sb_puts(&out, subjects[0]);
        }
    }

    sb_putc(&out, ')');
    free(head);
    return sb_take(&out);
}

static bool wisp_guard_is_binary_op(const char *s) {
    return strcmp(s, ">=") == 0 ||
           strcmp(s, "<=") == 0 ||
           strcmp(s, ">") == 0 ||
           strcmp(s, "<") == 0 ||
           strcmp(s, "=") == 0 ||
           strcmp(s, "!=") == 0 ||
           strcmp(s, "+") == 0 ||
           strcmp(s, "-") == 0 ||
           strcmp(s, "*") == 0 ||
           strcmp(s, "/") == 0 ||
           strcmp(s, "mod") == 0 ||
           strcmp(s, "%") == 0;
}

static char *wisp_guard_apply_implicit_subjects(ArityTable *at,
                                                const char *guard_expanded,
                                                char **subjects,
                                                int subject_count) {
    if (!guard_expanded) return strdup("");
    if (!subjects || subject_count <= 0) return strdup(guard_expanded);

    const char *start = guard_expanded;
    const char *end = guard_expanded + strlen(guard_expanded);

    return wisp_guard_apply_expr(at, start, end, subjects, subject_count);
}

static char *wisp_guard_take_one_operand(WTokenStream *ts) {
    if (ts->pos >= ts->count) {
        return strdup("");
    }

    WToken *tok = &ts->tokens[ts->pos];
    ts->pos++;

    return strdup(tok->text);
}

static char *wisp_expand_pointfree_guard(ArityTable *at,
                                         const char *guard_src,
                                         char **subjects,
                                         int subject_count) {
    WTokenStream ts = build_token_stream(guard_src, at);
    SB out;
    sb_init(&out);

    bool have_expr = false;
    char *pending_logic = NULL;

    while (ts.pos < ts.count) {
        WToken *tok = &ts.tokens[ts.pos];

        if ((strcmp(tok->text, "and") == 0 || strcmp(tok->text, "or") == 0) &&
            have_expr) {
            free(pending_logic);
            pending_logic = strdup(tok->text);
            ts.pos++;
            continue;
        }

        char *expr = NULL;

        if (wisp_guard_is_binary_op(tok->text)) {
            const char *op = tok->text;
            ts.pos++;

            if (ts.pos < ts.count && subject_count == 1) {
                char *rhs_expr = wisp_guard_take_one_operand(&ts);

                SB e;
                sb_init(&e);
                sb_putc(&e, '(');
                sb_puts(&e, op);
                sb_putc(&e, ' ');
                sb_puts(&e, subjects[0]);
                sb_putc(&e, ' ');
                sb_puts(&e, rhs_expr);
                sb_putc(&e, ')');

                free(rhs_expr);
                expr = sb_take(&e);
            } else if (ts.pos >= ts.count && subject_count > 0) {
                SB e;
                sb_init(&e);
                sb_putc(&e, '(');
                sb_puts(&e, op);
                wisp_guard_append_subjects(&e, subjects, subject_count);
                sb_putc(&e, ')');
                expr = sb_take(&e);
            } else {
                SB e;
                sb_init(&e);
                sb_putc(&e, '(');
                sb_puts(&e, op);

                while (ts.pos < ts.count) {
                    sb_putc(&e, ' ');
                    wisp_parse_expr(at, &ts, &e, -1, 1, 0);
                }

                sb_putc(&e, ')');
                expr = sb_take(&e);
            }
        } else {
            bool explicit_subject_call = false;

            if (subject_count > 0 &&
                tok->text &&
                !wisp_guard_special_head(tok->text) &&
                !wisp_guard_bare_should_not_apply(tok->text) &&
                ts.pos + 1 < ts.count &&
                wisp_guard_bare_is_subject(ts.tokens[ts.pos + 1].text,
                                           subjects,
                                           subject_count)) {
                explicit_subject_call = true;
            }

            if (explicit_subject_call) {
                SB e;
                sb_init(&e);
                sb_putc(&e, '(');
                sb_puts(&e, tok->text);
                sb_putc(&e, ' ');
                sb_puts(&e, ts.tokens[ts.pos + 1].text);
                sb_putc(&e, ')');
                ts.pos += 2;
                expr = sb_take(&e);
            } else {
                SB e;
                sb_init(&e);
                wisp_parse_expr(at, &ts, &e, -1, 1, 0);
                char *expanded = sb_take(&e);
                expr = wisp_guard_apply_implicit_subjects(at, expanded,
                                                          subjects,
                                                          subject_count);
                free(expanded);
            }
        }

        if (!have_expr) {
            sb_puts(&out, expr);
            have_expr = true;
        } else {
            const char *logic = pending_logic ? pending_logic : "and";
            char *left = sb_take(&out);

            sb_init(&out);
            sb_putc(&out, '(');
            sb_puts(&out, logic);
            sb_putc(&out, ' ');
            sb_puts(&out, left);
            sb_putc(&out, ' ');
            sb_puts(&out, expr);
            sb_putc(&out, ')');

            free(left);
        }

        free(pending_logic);
        pending_logic = NULL;
        free(expr);
    }

    free(pending_logic);
    wts_free(&ts);

    if (!have_expr) {
        sb_puts(&out, "True");
    }

    return sb_take(&out);
}

static void wisp_free_guard_subjects(char **subjects, int count) {
    if (!subjects) return;
    for (int i = 0; i < count; i++) {
        free(subjects[i]);
    }
    free(subjects);
}

static int wisp_extract_guard_subjects(const char *start, const char *end,
                                       char ***out_subjects) {
    *out_subjects = NULL;

    char *part = wisp_trim_range_dup(start, end);
    Lexer lex;
    lexer_init(&lex, part);

    char **subjects = NULL;
    int subject_count = 0;
    int subject_cap = 0;
    bool has_complex_pattern = false;

    while (true) {
        Token tok = lexer_next_token(&lex);

        if (tok.type == TOK_EOF) {
            free(tok.value);
            break;
        }

        if (tok.type == TOK_SYMBOL && tok.value && strcmp(tok.value, "_") == 0) {
            free(tok.value);
            continue;
        }

        if (tok.type == TOK_SYMBOL &&
            tok.value &&
            wisp_is_simple_identifier_text(tok.value)) {
            if (subject_count >= subject_cap) {
                subject_cap = subject_cap ? subject_cap * 2 : 4;
                subjects = realloc(subjects, sizeof(char *) * subject_cap);
            }

            subjects[subject_count++] = strdup(tok.value);
            free(tok.value);
            continue;
        }

        if (tok.type == TOK_LPAREN || tok.type == TOK_LBRACKET) {
            int depth = 1;
            free(tok.value);

            while (depth > 0) {
                tok = lexer_next_token(&lex);

                if (tok.type == TOK_EOF) {
                    free(tok.value);
                    break;
                }

                if (tok.type == TOK_LPAREN || tok.type == TOK_LBRACKET) {
                    depth++;
                } else if (tok.type == TOK_RPAREN || tok.type == TOK_RBRACKET) {
                    depth--;
                }

                free(tok.value);
            }

            has_complex_pattern = true;
            continue;
        }

        free(tok.value);
        has_complex_pattern = true;
    }

    free(part);

    if (has_complex_pattern) {
        wisp_free_guard_subjects(subjects, subject_count);
        *out_subjects = NULL;
        return 0;
    }

    *out_subjects = subjects;
    return subject_count;
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
            else if (*p == '\r') break;
            else if (*p == '\n') break;
        }
        p++;
    }
    while (p > start && (*(p-1) == ' ' || *(p-1) == '\t')) p--;
    return p;
}

static const char *wisp_find_top_level_left_arrow_before(const char *start,
                                                         const char *end) {
    int depth = 0;
    bool in_str = false;
    bool in_char = false;
    bool escape = false;

    for (const char *q = start; q + 1 < end; q++) {
        if (in_str) {
            if (escape) {
                escape = false;
            } else if (*q == '\\') {
                escape = true;
            } else if (*q == '"') {
                in_str = false;
            }
            continue;
        }

        if (in_char) {
            if (escape) {
                escape = false;
            } else if (*q == '\\') {
                escape = true;
            } else if (*q == '\'') {
                in_char = false;
            }
            continue;
        }

        if (*q == '"') {
            in_str = true;
            continue;
        }

        if (*q == '\'') {
            in_char = true;
            continue;
        }

        if (*q == ';')
            break;

        if (*q == '(' || *q == '[' || *q == '{') {
            depth++;
            continue;
        }

        if (*q == ')' || *q == ']' || *q == '}') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth == 0 && q[0] == '<' && q[1] == '-')
            return q;
    }

    return NULL;
}

static char *wisp_expand_statement_range(ArityTable *at,
                                         const char *start,
                                         const char *end) {
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;

    if (start >= end)
        return strdup("(undefined)");

    const char *assign = wisp_find_top_level_left_arrow_before(start, end);

    if (assign) {
        char *lhs = wisp_trim_range_dup(start, assign);
        const char *rhs_start = assign + 2;
        while (rhs_start < end && (*rhs_start == ' ' || *rhs_start == '\t'))
            rhs_start++;

        char *rhs_src = strndup(rhs_start, (size_t)(end - rhs_start));
        char *rhs = wisp_expand_expr_snippet(at, rhs_src);
        free(rhs_src);

        SB out;
        sb_init(&out);
        sb_puts(&out, lhs);
        sb_puts(&out, " <- ");
        sb_puts(&out, rhs);

        free(lhs);
        free(rhs);
        return sb_take(&out);
    }

    char *src = strndup(start, (size_t)(end - start));
    char *expanded = wisp_expand_expr_snippet(at, src);
    free(src);
    return expanded;
}

static void wisp_append_expanded_statement(ArityTable *at, SB *out,
                                           const char *start,
                                           const char *end,
                                           int *expr_count) {
    char *expanded = wisp_expand_statement_range(at, start, end);

    if (*expr_count > 0)
        sb_putc(out, ' ');

    sb_puts(out, expanded);
    (*expr_count)++;
    free(expanded);
}

static char *wisp_try_build_inferred_define_form(ArityTable *at,
                                                 const char *fname,
                                                 const char *body_start,
                                                 int parent_indent,
                                                 int header_lineno,
                                                 const char **out_pos,
                                                 int *out_lineno,
                                                 int *out_arity) {
    const char *scan = body_start;
    int line_no = header_lineno + 1;
    int first_clause_indent = -1;
    char *first_left = NULL;
    char *first_names[WISP_MAX_PARAMS];
    memset(first_names, 0, sizeof(first_names));
    int first_name_count = 0;

    while (*scan) {
        const char *ls = scan;
        while (*scan && *scan != '\n') scan++;
        const char *le = scan;
        if (*scan == '\n') scan++;

        const char *lt = ls;
        while (lt < le && (*lt == ' ' || *lt == '\t')) lt++;
        if (lt >= le || *lt == ';') {
            line_no++;
            continue;
        }

        int clause_indent = measure_indent(ls);
        if (clause_indent <= parent_indent)
            return NULL;

        char *line = strndup(lt, (size_t)(le - lt));
        const char *arrow = wisp_find_top_level_arrow(line);
        if (!arrow) {
            free(line);
            return NULL;
        }

        first_left = wisp_trim_range_dup(line, arrow);
        first_name_count = wisp_parse_simple_names_any(first_left,
                                                       first_names,
                                                       WISP_MAX_PARAMS);
        free(line);

        if (first_name_count <= 0) {
            free(first_left);
            for (int i = 0; i < WISP_MAX_PARAMS; i++)
                free(first_names[i]);
            return NULL;
        }

        first_clause_indent = clause_indent;
        scan = ls;
        break;
    }

    if (first_clause_indent < 0)
        return NULL;

    SB form;
    sb_init(&form);
    sb_puts(&form, "(define (");
    sb_puts(&form, fname);
    for (int i = 0; i < first_name_count; i++) {
        sb_putc(&form, ' ');
        sb_puts(&form, first_names[i]);
    }
    sb_puts(&form, " -> [a])");

    bool emitted_clause = false;

    while (*scan) {
        const char *ls = scan;
        while (*scan && *scan != '\n') scan++;
        const char *le = scan;
        if (*scan == '\n') scan++;

        const char *lt = ls;
        while (lt < le && (*lt == ' ' || *lt == '\t')) lt++;
        if (lt >= le || *lt == ';') {
            line_no++;
            continue;
        }

        int clause_indent = measure_indent(ls);
        if (clause_indent <= parent_indent) {
            scan = ls;
            break;
        }

        char *line = strndup(lt, (size_t)(le - lt));
        const char *arrow = wisp_find_top_level_arrow(line);
        if (!arrow) {
            free(line);
            scan = ls;
            break;
        }

        char *left = wisp_trim_range_dup(line, arrow);
        const char *rhs_start = arrow + 2;
        const char *rhs_end = get_logical_line_end(rhs_start);

        SB body;
        sb_init(&body);
        int expr_count = 0;
        wisp_append_expanded_statement(at, &body, rhs_start, rhs_end, &expr_count);

        line_no++;

        while (*scan) {
            const char *cls = scan;
            while (*scan && *scan != '\n') scan++;
            const char *cle = scan;
            if (*scan == '\n') scan++;

            const char *clt = cls;
            while (clt < cle && (*clt == ' ' || *clt == '\t')) clt++;
            if (clt >= cle || *clt == ';') {
                line_no++;
                continue;
            }

            int cont_indent = measure_indent(cls);
            if (cont_indent <= parent_indent) {
                scan = cls;
                break;
            }

            char *cont_line = strndup(clt, (size_t)(cle - clt));
            const char *cont_arrow = wisp_find_top_level_arrow(cont_line);
            if (cont_indent <= clause_indent && cont_arrow) {
                free(cont_line);
                scan = cls;
                break;
            }

            const char *cont_end = get_logical_line_end(cont_line);
            wisp_append_expanded_statement(at, &body, cont_line, cont_end, &expr_count);
            free(cont_line);
            line_no++;
        }

        char *body_text = sb_take(&body);

        sb_putc(&form, emitted_clause ? '\n' : ' ');
        sb_puts(&form, left);
        sb_puts(&form, " -> ");
        if (expr_count > 1) {
            sb_puts(&form, "(begin ");
            sb_puts(&form, body_text);
            sb_putc(&form, ')');
        } else {
            sb_puts(&form, body_text);
        }

        emitted_clause = true;
        free(body_text);
        free(left);
        free(line);
    }

    sb_putc(&form, ')');

    free(first_left);
    for (int i = 0; i < first_name_count; i++)
        free(first_names[i]);

    if (!emitted_clause) {
        char *discard = sb_take(&form);
        free(discard);
        return NULL;
    }

    *out_pos = scan;
    *out_lineno = line_no;
    *out_arity = first_name_count;
    return sb_take(&form);
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

static const char *skip_corner_quote_chars(const char *p) {
    if (!((unsigned char)p[0] == 0xE2 &&
          (unsigned char)p[1] == 0x8C &&
          (unsigned char)p[2] == 0x9C)) {
        return p;
    }

    p += 3;
    while (*p) {
        if ((unsigned char)p[0] == 0xE2 &&
            (unsigned char)p[1] == 0x8C &&
            (unsigned char)p[2] == 0x9D) {
            return p + 3;
        }
        if (*p == '"') {
            p++;
            while (*p && !(*p == '"' && *(p - 1) != '\\')) p++;
            if (*p == '"') p++;
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
    if (strcmp(name, "+") == 0 || strcmp(name, "-") == 0 ||
        strcmp(name, "mod") == 0 || strcmp(name, "%") == 0) return 4;
    if (strcmp(name, "*") == 0 || strcmp(name, "/") == 0) return 5;
    return 6;
}

static bool wisp_token_can_call_group(const char *text) {
    if (!text || !text[0])
        return false;

    unsigned char c = (unsigned char)text[0];
    if ((c >= '0' && c <= '9') ||
        (text[0] == '-' && text[1] >= '0' && text[1] <= '9') ||
        text[0] == '"' ||
        text[0] == '\'') {
        return false;
    }

    return true;
}

static bool wisp_token_is_infix_name(const char *text) {
    return text &&
           (strcmp(text, "&") == 0 ||
            strcmp(text, "+") == 0 ||
            strcmp(text, "-") == 0 ||
            strcmp(text, "*") == 0 ||
            strcmp(text, "/") == 0 ||
            strcmp(text, "%") == 0 ||
            strcmp(text, "=") == 0 ||
            strcmp(text, "!=") == 0 ||
            strcmp(text, "<") == 0 ||
            strcmp(text, ">") == 0 ||
            strcmp(text, "<=") == 0 ||
            strcmp(text, ">=") == 0 ||
            strcmp(text, "and") == 0 ||
            strcmp(text, "or") == 0 ||
            strcmp(text, "mod") == 0);
}

static char *wisp_rewrite_grouped_infix(ArityTable *t, const char *text) {
    size_t len = strlen(text);
    if (len < 2 || text[0] != '(' || text[len - 1] != ')')
        return strdup(text);

    char **items = NULL;
    int count = 0;
    int cap = 0;
    const char *inner = text + 1;
    const char *end = text + len - 1;
    const char *item_start = NULL;
    int depth = 0;
    bool in_str = false;
    bool in_char = false;
    bool escape = false;

    for (const char *p = inner; p <= end; p++) {
        char c = (p < end) ? *p : ' ';
        if (in_str) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (in_char) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '\'') in_char = false;
            continue;
        }
        if (c == '"') { in_str = true; if (!item_start) item_start = p; continue; }
        if (c == '\'') { in_char = true; if (!item_start) item_start = p; continue; }
        if (c == '(' || c == '[' || c == '{') {
            if (!item_start) item_start = p;
            depth++;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) depth--;
            continue;
        }
        if ((c == ' ' || c == '\t' || c == '\n' || c == '\r') && depth == 0) {
            if (item_start) {
                if (count >= cap) {
                    cap = cap ? cap * 2 : 8;
                    items = realloc(items, sizeof(char *) * cap);
                }
                items[count++] = strndup(item_start, (size_t)(p - item_start));
                item_start = NULL;
            }
            continue;
        }
        if (!item_start)
            item_start = p;
    }

    if (count == 0) {
        free(items);
        return strdup(text);
    }

    char **rewritten = calloc((size_t)count, sizeof(char *));
    for (int i = 0; i < count; i++) {
        rewritten[i] = (items[i][0] == '(')
                           ? wisp_rewrite_grouped_infix(t, items[i])
                           : strdup(items[i]);
    }

    int op_arity = count == 3 ? wisp_lookup_arity(t, items[1]) : -2;
    bool can_rewrite =
        count == 3 &&
        wisp_token_is_infix_name(items[1]) &&
        (op_arity >= 2 || op_arity == -1 || strcmp(items[1], "&") == 0);

    SB out;
    sb_init(&out);
    sb_putc(&out, '(');
    if (can_rewrite) {
        sb_puts(&out, items[1]);
        sb_putc(&out, ' ');
        sb_puts(&out, rewritten[0]);
        sb_putc(&out, ' ');
        sb_puts(&out, rewritten[2]);
    } else {
        for (int i = 0; i < count; i++) {
            if (i) sb_putc(&out, ' ');
            sb_puts(&out, rewritten[i]);
        }
    }
    sb_putc(&out, ')');

    for (int i = 0; i < count; i++) {
        free(items[i]);
        free(rewritten[i]);
    }
    free(items);
    free(rewritten);
    return sb_take(&out);
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

static int g_for_iter_depth = 0;

static int wisp_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

static int wisp_control_caret_value(unsigned char c) {
    if (c >= 'a' && c <= 'z')
        c = (unsigned char)(c - 'a' + 'A');

    if (c >= '@' && c <= '_')
        return (int)(c - '@');

    if (c == '?')
        return 127;

    return -1;
}

static char *wisp_control_char_literal_token(unsigned char ch) {
    char buf[8];

    if (ch < 0x20) {
        snprintf(buf, sizeof(buf), "'\\x%02x'", ch);
        return strdup(buf);
    }

    if (ch == 0x7F) {
        snprintf(buf, sizeof(buf), "'\\x%02x'", ch);
        return strdup(buf);
    }

    return NULL;
}

static char *wisp_canonical_char_literal_token(const char *start, size_t len) {
    if (!start || len < 3)
        return strndup(start ? start : "", start ? len : 0);

    if (start[0] != '\'' || start[len - 1] != '\'')
        return strndup(start, len);

    if (len == 3) {
        unsigned char ch = (unsigned char)start[1];
        char *control = wisp_control_char_literal_token(ch);
        if (control)
            return control;
        return strndup(start, len);
    }

    if (len == 4 && start[1] == '^') {
        int cv = wisp_control_caret_value((unsigned char)start[2]);
        if (cv >= 0) {
            char *control = wisp_control_char_literal_token((unsigned char)cv);
            if (control)
                return control;
        }
        return strndup(start, len);
    }

    if (len == 4 && start[1] == '\\') {
        unsigned char ch = (unsigned char)start[2];

        switch (ch) {
        case 'n': return strdup("'^J'");
        case 't': return strdup("'^I'");
        case 'r': return strdup("'^M'");
        case 'v': return strdup("'^K'");
        case 'f': return strdup("'^L'");
        case '0': return strdup("'^@'");
        default: break;
        }

        return strndup(start, len);
    }

    if (len == 6 &&
        start[1] == '\\' &&
        (start[2] == 'x' || start[2] == 'X')) {
        int hv = wisp_hex_value((unsigned char)start[3]);
        int lv = wisp_hex_value((unsigned char)start[4]);

        if (hv >= 0 && lv >= 0) {
            unsigned char ch = (unsigned char)((hv << 4) | lv);
            char *control = wisp_control_char_literal_token(ch);
            if (control)
                return control;
        }
    }

    return strndup(start, len);
}

/* Tokenise one line into the stream */
static void tokenise_into(ArityTable *t, WTokenStream *s, const char *line,
                           int indent, int lineno) {
    const char *p = line;
    const char *line_start = line;
    while (*line_start == ' ' || *line_start == '\t') line_start++;

    (void)s;
    if (wisp_try_store_standalone_type_decl(t, line_start, indent)) {
        return;
    }

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p || *p == '\r' || *p == ';') break;
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
        if (*p == '\'') {
            const char *start = p++;

            while (*p) {
                if (*p == '\\' && p[1]) {
                    p += 2;
                    continue;
                }

                if (*p == '\'') {
                    p++;
                    break;
                }

                p++;
            }

            if (p > start + 1 && p[-1] == '\'') {
                char *tok = wisp_canonical_char_literal_token(start, (size_t)(p - start));
                wts_push(s, tok, indent, lineno);
                free(tok);
                continue;
            }

            p = start;
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

            /* Desugar (for c <- expr body...) and (for c -> expr body...) s-expr forms.
             * These are the parenthesized equivalents of the bare wisp for-sugar.
             * Pattern: (for IDENT (<-|->) EXPR BODY...)
             * We detect this by scanning the interior of the grouped token.    */
            if (open == '(' && strncmp(tok + 1, "for ", 4) == 0) {
                /* Parse the interior: skip 'for', read bind name, check for arrow */
                const char *fp = tok + 1 + 4; /* skip '(for ' */
                while (*fp == ' ' || *fp == '\t') fp++;
                /* Read binding name — must be a plain symbol (no bracket) */
                if (*fp && *fp != '[' && *fp != '(' && *fp != ')') {
                    const char *name_start = fp;
                    while (*fp && *fp != ' ' && *fp != '\t' && *fp != ')') fp++;
                    size_t name_len = fp - name_start;
                    while (*fp == ' ' || *fp == '\t') fp++;
                    bool is_forward  = (fp[0] == '-' && fp[1] == '>');
                    bool is_backward = (fp[0] == '<' && fp[1] == '-');
                    if ((is_forward || is_backward) && name_len > 0) {
                        char *bind_name = strndup(name_start, name_len);
                        fp += 2; /* skip <- or -> */
                        while (*fp == ' ' || *fp == '\t') fp++;

                        /* Read the iterable expression — balanced token */
                        const char *iter_start = fp;
                        const char *iter_end;
                        if (*fp == '(' || *fp == '[' || *fp == '{' ||
                            *fp == '"') {
                            /* Balanced: scan to matching close or end of string */
                            if (*fp == '"') {
                                fp++;
                                while (*fp && !(*fp == '"' && *(fp-1) != '\\')) fp++;
                                if (*fp == '"') fp++;
                            } else {
                                char ic = *fp;
                                char cc = ic == '(' ? ')' : ic == '[' ? ']' : '}';
                                int id = 0;
                                while (*fp) {
                                    if (*fp == ic) id++;
                                    else if (*fp == cc) { id--; fp++; if (!id) break; continue; }
                                    fp++;
                                }
                            }
                            iter_end = fp;
                        } else {
                            /* Plain token: read until space or ) */
                            while (*fp && *fp != ' ' && *fp != '\t' && *fp != ')') fp++;
                            iter_end = fp;
                        }
                        char *iter_expr = strndup(iter_start, iter_end - iter_start);
                        while (*fp == ' ' || *fp == '\t') fp++;

                        /* Everything remaining until the final ')' is the body */
                        /* Find the matching close paren for the whole (for ...) */
                        size_t tok_len = strlen(tok);
                        /* strip trailing ')' */
                        const char *body_start = fp;
                        /* Find end of body: tok ends with ')', body is between fp and tok+tok_len-1 */
                        const char *body_end = tok + tok_len - 1;
                        /* Trim trailing whitespace before final ')' */
                        while (body_end > body_start &&
                               (*(body_end-1) == ' ' || *(body_end-1) == '\t'))
                            body_end--;
                        char *body_src = strndup(body_start, body_end - body_start);

                        /* Now generate the same desugared form as the bare wisp path */
                        g_for_iter_depth++;
                        int iter_depth = g_for_iter_depth;
                        char iter_var[32];
                        if (iter_depth == 1)
                            snprintf(iter_var, sizeof(iter_var), "iter");
                        else
                            snprintf(iter_var, sizeof(iter_var), "iter%d", iter_depth);

                        /* Expand body through the full wisp pipeline */
                        SB body_expanded_sb; sb_init(&body_expanded_sb);
                        if (body_src[0] != '\0') {
                            WTokenStream _bts = build_token_stream(body_src, t);
                            bool _bfirst = true;
                            while (_bts.pos < _bts.count) {
                                if (!_bfirst) sb_putc(&body_expanded_sb, ' ');
                                _bfirst = false;
                                wisp_parse_expr(t, &_bts, &body_expanded_sb, -1, 1, 0);
                            }
                            wts_free(&_bts);
                        }
                        g_for_iter_depth--;
                        char *body_expanded = sb_take(&body_expanded_sb);

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

                        char *desugared_for = sb_take(&ds);
                        wts_push(s, desugared_for, indent, lineno);
                        free(desugared_for);
                        free(iter_expr);
                        free(bind_name);
                        free(body_src);
                        free(tok);
                        continue;
                    }
                }
            }

            /* Desugar size-based array literals before any further processing */
            {
                char *desugared = desugar_size_array(tok);
                if (desugared) { free(tok); tok = desugared; }
            }

            /* After a grouped token, apply the same :: annotation grouping */
            const char *peek = p;
            while (*peek == ' ' || *peek == '\t' || *peek == '\r') peek++;

            /* Mutable variable declaration in bracket form: [name :: Type] = value.
             * Must be checked before try_consume_type_annotation and before the
             * '=>' lambda-sugar check, since a bare '=' here is not lambda sugar
             * and not a type-annotation continuation — it is a var-decl assignment.
             * Discriminated from a plain array literal like [1 2 3] by requiring
             * "::" inside tok; works at any nesting depth since tokenise_into is
             * also used for indented body lines (function bodies, where-blocks). */
            if (tok[0] == '[' && strstr(tok, "::") != NULL &&
                peek[0] == '=' && peek[1] != '=' && peek[1] != '>') {
                const char *val_start = peek + 1;
                while (*val_start == ' ' || *val_start == '\t') val_start++;
                const char *val_end = get_logical_line_end(val_start);
                char *val_src = strndup(val_start, (size_t)(val_end - val_start));
                char *val_tok = wisp_expand_expr_snippet(t, val_src);
                free(val_src);

                SB form;
                sb_init(&form);
                sb_puts(&form, "(var ");
                sb_puts(&form, tok);
                sb_putc(&form, ' ');
                sb_puts(&form, val_tok);
                sb_putc(&form, ')');

                char *var_form = sb_take(&form);
                wts_push(s, var_form, indent, lineno);

                free(var_form);
                free(val_tok);
                free(tok);
                p = val_end;
                continue;
            }

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
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != ';') p++;
        char *tok = strndup(start, p - start);

        const char *line_start = line;
        while (*line_start == ' ' || *line_start == '\t') line_start++;

        if (start == line_start) {
            const char *assign = wisp_find_top_level_assignment(line_start);

            if (assign) {
                const char *line_end_for_grid = get_logical_line_end(line_start);
                if (wisp_emit_assignment_grid_row(t, s,
                                                  line_start,
                                                  line_end_for_grid,
                                                  indent,
                                                  lineno)) {
                    free(tok);
                    p = line_end_for_grid;
                    continue;
                }

                const char *type_mark = NULL;
                int depth = 0;
                bool in_str = false;

                for (const char *q = line_start; q < assign; q++) {
                    if (in_str) {
                        if (*q == '\\' && *(q + 1)) q++;
                        else if (*q == '"') in_str = false;
                        continue;
                    }

                    if (*q == '"') {
                        in_str = true;
                        continue;
                    }

                    if (*q == '(' || *q == '[' || *q == '{') {
                        depth++;
                        continue;
                    }

                    if (*q == ')' || *q == ']' || *q == '}') {
                        if (depth > 0) depth--;
                        continue;
                    }

                    if (depth == 0 && q[0] == ':' && q[1] == ':') {
                        type_mark = q;
                        break;
                    }
                }

                const char *names_end = type_mark ? type_mark : assign;
                char *names_src = wisp_trim_range_dup(line_start, names_end);

                char *names[WISP_MAX_PARAMS];
                memset(names, 0, sizeof(names));
                int name_count = wisp_parse_simple_names_any(names_src, names, WISP_MAX_PARAMS);
                free(names_src);

                if (name_count > 0) {
                    const char *val_start = assign + 1;
                    while (*val_start == ' ' || *val_start == '\t') val_start++;
                    const char *val_end = get_logical_line_end(val_start);

                    char *val_src = strndup(val_start, (size_t)(val_end - val_start));
                    char *val_tok = wisp_expand_expr_snippet(t, val_src);
                    free(val_src);

                    char *type_src = NULL;
                    if (type_mark) {
                        type_src = wisp_trim_range_dup(type_mark, assign);
                    }

                    for (int i = 0; i < name_count; i++) {
                        if (type_src) {
                            char *old_pending_type = wisp_pending_type_take(names[i], indent);
                            free(old_pending_type);
                        }
                        char *binding = NULL;
                        char *pending_type = NULL;

                        if (!type_src) {
                            pending_type = wisp_pending_type_take(names[i], indent);
                        }

                        if (type_src || pending_type) {
                            SB b;
                            sb_init(&b);
                            sb_putc(&b, '[');
                            sb_puts(&b, names[i]);
                            sb_putc(&b, ' ');
                            if (type_src) {
                                sb_puts(&b, type_src);
                            } else {
                                sb_puts(&b, ":: ");
                                sb_puts(&b, pending_type);
                            }
                            sb_putc(&b, ']');
                            binding = sb_take(&b);
                        } else {
                            binding = strdup(names[i]);
                        }

                        SB form;
                        sb_init(&form);
                        sb_puts(&form, "(var ");
                        sb_puts(&form, binding);
                        sb_putc(&form, ' ');
                        sb_puts(&form, val_tok);
                        sb_putc(&form, ')');

                        char *var_form = sb_take(&form);
                        wts_push(s, var_form, indent, lineno);

                        free(var_form);
                        free(binding);
                        free(pending_type);
                    }

                    free(type_src);

                    free(val_tok);
                    for (int i = 0; i < name_count; i++) free(names[i]);

                    free(tok);
                    p = val_end;
                    continue;
                }

                for (int i = 0; i < WISP_MAX_PARAMS; i++) free(names[i]);
            }
        }

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

static WTokenStream build_token_stream(const char *source, ArityTable *at) {
    WTokenStream s = {0};
    const char *p = source;
    int lineno = 1;
    int pending_alias_doc_indent = -1;

    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        int len = (int)(p - line_start);
        if (*p == '\n') p++;

        char *raw = strndup(line_start, len);

        /* Build one logical Wisp line when a string literal spans physical
         * lines. This keeps :doc strings atomic before tokenise_into sees
         * them. The reader can already parse multiline string literals, but
         * Wisp must not split the literal into separate symbols first. */
        if (wisp_text_ends_inside_string(raw)) {
            size_t acc_cap = strlen(raw) + 256;
            size_t acc_len = strlen(raw);
            char *acc = malloc(acc_cap);

            if (!acc) {
                fprintf(stderr, "[wisp] out of memory joining multiline string at line %d\n",
                        lineno);
                abort();
            }

            memcpy(acc, raw, acc_len);
            acc[acc_len] = '\0';
            free(raw);

            while (wisp_text_ends_inside_string(acc) && *p) {
                const char *next_start = p;

                while (*p && *p != '\n')
                    p++;

                size_t next_len = (size_t)(p - next_start);

                if (*p == '\n')
                    p++;

                while (acc_len + next_len + 2 >= acc_cap) {
                    acc_cap *= 2;
                    char *grown = realloc(acc, acc_cap);
                    if (!grown) {
                        fprintf(stderr, "[wisp] out of memory growing multiline string at line %d\n",
                                lineno);
                        free(acc);
                        abort();
                    }
                    acc = grown;
                }

                acc[acc_len++] = '\n';
                memcpy(acc + acc_len, next_start, next_len);
                acc_len += next_len;
                acc[acc_len] = '\0';

                lineno++;
            }

            if (wisp_text_ends_inside_string(acc)) {
                wisp_syntax_error(
                    lineno,
                    1,
                    "unterminated string literal",
                    "close the string with a double quote before the end of the file");
            }

            raw = acc;
        }

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

        /* Accept visual/title-case keyword spelling at the Wisp surface
         * layer. Core output stays canonical lowercase. */
        if (strncmp(t, "Define", 6) == 0 &&
            (t[6] == ' ' || t[6] == '\t' || t[6] == '\0')) {
            memcpy((char *)t, "define", 6);
        } else if (strncmp(t, "Def", 3) == 0 &&
                   (t[3] == ' ' || t[3] == '\t' || t[3] == '\0')) {
            memcpy((char *)t, "def", 3);
        } else if (strncmp(t, "Infer", 5) == 0 &&
                   (t[5] == ' ' || t[5] == '\t' || t[5] == '\0')) {
            memcpy((char *)t, "infer", 5);
        }

        /* skip blank/comment lines */
        if (!*t || *t == ';') { free(raw); lineno++; continue; }

        int indent = measure_indent(raw);

        /* Strip | end-of-line comments on define lines only.
         * Rule: if this line starts with 'define' or 'def', scan for
         * ' | ' at depth 0 outside strings and wipe from there to EOL.
         * This does NOT apply to guard clauses, bit-or, or [x|xs] patterns
         * because those never appear on a bare define header line. */
        {
            int _def_check_len =
                (strncmp(t, "define", 6) == 0 && (t[6]==' '||t[6]=='\t')) ? 6 :
                (strncmp(t, "def",    3) == 0 && (t[3]==' '||t[3]=='\t')) ? 3 : 0;
            if (_def_check_len > 0) {
                int _pd = 0;
                bool _ps = false;
                for (char *q = raw; *q; q++) {
                    if (_ps) {
                        if (*q == '"') _ps = false;
                        continue;
                    }
                    if (*q == '"') { _ps = true; continue; }
                    if (*q == '(' || *q == '[' || *q == '{') _pd++;
                    if (*q == ')' || *q == ']' || *q == '}') { if (_pd > 0) _pd--; }
                    if (_pd == 0 && *q == ' ' && *(q+1) == '|') {
                        char *wipe = q;
                        while (*wipe) *wipe++ = ' ';
                        break;
                    }
                }
            }
        }

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
                    bool iter_expr_is_bare_range = false;
                    char *iter_expr_readable =
                        wisp_wrap_bare_range_expr(iter_expr, &iter_expr_is_bare_range);

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
                    char iterable_var[64];
                    if (iter_expr_is_bare_range) {
                        snprintf(iterable_var, sizeof(iterable_var),
                                 "__wisp_iterable%d", iter_depth);
                        sb_puts(&ds, "(let [");
                        sb_puts(&ds, iterable_var);
                        sb_putc(&ds, ' ');
                        sb_puts(&ds, iter_expr_readable);
                        sb_puts(&ds, "] ");
                    }
                    const char *iterable_expr = iter_expr_is_bare_range
                        ? iterable_var
                        : iter_expr_readable;
                        if (is_forward) {
                            sb_puts(&ds, "(for [");
                            sb_puts(&ds, iter_var);
                            sb_puts(&ds, " 0 (count ");
                            sb_puts(&ds, iterable_expr);
                            sb_puts(&ds, ")] (define ");
                            sb_puts(&ds, bind_name);
                            sb_puts(&ds, " (");
                            sb_puts(&ds, iterable_expr);
                            sb_puts(&ds, " ");
                            sb_puts(&ds, iter_var);
                            sb_puts(&ds, "))");
                        } else {
                            sb_puts(&ds, "(for [");
                            sb_puts(&ds, iter_var);
                            sb_puts(&ds, " (- (count ");
                            sb_puts(&ds, iterable_expr);
                            sb_puts(&ds, ") 1) -1 -1] (define ");
                            sb_puts(&ds, bind_name);
                            sb_puts(&ds, " (");
                            sb_puts(&ds, iterable_expr);
                            sb_puts(&ds, " ");
                            sb_puts(&ds, iter_var);
                            sb_puts(&ds, "))");
                        }
                    if (body_expanded[0] != '\0') {
                        sb_putc(&ds, ' ');
                        sb_puts(&ds, body_expanded);
                    }
                    sb_putc(&ds, ')');
                    if (iter_expr_is_bare_range)
                        sb_putc(&ds, ')');
                    free(body_expanded);

                    char *desugared = sb_take(&ds);
                    wts_push(&s, desugared, indent, lineno);
                    free(desugared);
                    free(iter_expr_readable);
                    free(iter_expr);
                    free(bind_name);
                    free(raw);
                    lineno++;
                    goto next_line;
                }
            }
        }

        if (indent == 0) {
            const char *line_end = get_logical_line_end(t);
            const char *arrow = wisp_find_top_level_arrow(t);
            if (arrow && arrow < line_end) {
                char *name = wisp_trim_range_dup(t, arrow);
                if (wisp_is_simple_identifier_text(name)) {
                    const char *rhs_start = arrow + 2;
                    while (rhs_start < line_end &&
                           (*rhs_start == ' ' || *rhs_start == '\t'))
                        rhs_start++;

                    if (rhs_start < line_end) {
                        char *rhs_src = wisp_trim_range_dup(rhs_start, line_end);
                        char *rhs = wisp_expand_expr_snippet(at, rhs_src);
                        SB form;
                        sb_init(&form);
                        sb_puts(&form, "(define ");
                        sb_puts(&form, name);
                        sb_putc(&form, ' ');
                        sb_puts(&form, rhs);
                        sb_putc(&form, ')');

                        char *desugared = sb_take(&form);
                        wts_push(&s, desugared, indent, lineno);
                        free(desugared);
                        free(rhs);
                        free(rhs_src);
                        free(name);
                        free(raw);
                        lineno++;
                        goto next_line;
                    }
                }
                free(name);
            }
        }

        if (strncmp(t, "define", 6) != 0 &&
            strncmp(t, "layout", 6) != 0 &&
            strncmp(t, "data", 4) != 0 &&
            !(strncmp(t, "class", 5) == 0 &&
              (t[5] == ' ' || t[5] == '\t' || t[5] == '\0')) &&
            !(strncmp(t, "instance", 8) == 0 &&
              (t[8] == ' ' || t[8] == '\t' || t[8] == '\0'))) {
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

            {
                const char *dcolon = strstr(t, "::");
                bool colon_before_arrow = (dcolon && dcolon < arrow);
                if (arrow && !colon_before_arrow) {
                    size_t left_len = arrow - t;
                    while (left_len > 0 && (t[left_len-1] == ' ' || t[left_len-1] == '\t')) left_len--;

                    char *left_str = strndup(t, left_len);
                    const char *lscan = left_str;
                    while (*lscan == ' ' || *lscan == '\t') lscan++;
                    lscan = wisp_skip_clause_function_name(&s, lscan);

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
                                    if (_vl > 0) {
                                        char _vname[128];
                                        size_t _vn = _vl < 127 ? _vl : 127;
                                        memcpy(_vname, _ps, _vn);
                                        _vname[_vn] = '\0';
                                        int _arity = _de->param_fn_arities[_pi] > 0
                                            ? _de->param_fn_arities[_pi]
                                            : 0;
                                        arity_set(at, _vname, _arity);
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
                         * guard clause (begins with '|' after whitespace),
                         * BUT only if the '|' is a standalone token (not
                         * part of an expression like "x | y").
                         * A guard-clause '|' has nothing before it on the
                         * line (it IS at the start after stripping indent).
                         * An expression '|' like "original.c_cflag | CS8"
                         * would only appear when the body depth is already
                         * non-zero from a prior open bracket, so we only
                         * break on standalone '|' when body_depth == 0
                         * AND the next non-space character after '|' is
                         * NOT a space-then-more-expression (i.e. it looks
                         * like a pmatch guard, not a bitwise-or mid-expr).
                         * The safest heuristic: only treat leading '|' as
                         * a guard separator when body_depth == 0 AND the
                         * current accumulated body already has content that
                         * looks like a complete expression (starts with a
                         * known form). For now: only break on '|' when
                         * body_depth == 0 AND body_acc_len == 0.          */
                        if (*clt == '|' && body_depth == 0 && body_acc_len == 0) {
                            p = cls; free(clraw); break;
                        }
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
                                if (!*blt || *blt == '\n' || *blt == '\r' || *blt == ';') {
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
                                WTokenStream _ts = build_token_stream((src_str), at); \
                                SB _sb; sb_init(&_sb);                  \
                                bool _first_exp = true;                 \
                                while (_ts.pos < _ts.count) {           \
                                    if (!_first_exp) sb_putc(&_sb, ' '); \
                                    _first_exp = false;                 \
                                    wisp_parse_expr(at, &_ts, &_sb, -1, 1, 0); \
                                }                                       \
                                wts_free(&_ts);                         \
                                (out_str) = sb_take(&_sb);              \
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

                                for (int bi = 0; bi < wb_n; bi++) {
                                    if (!wb[bi].name) continue;

                                    for (int bj = bi + 1; bj < wb_n; bj++) {
                                        if (!wb[bj].name) continue;
                                        if (strcmp(wb[bi].name, wb[bj].name) != 0) continue;

                                        size_t left_len = strlen(wb[bi].rest);
                                        size_t add_len = strlen(wb[bj].rest);
                                        size_t cont_len = wb[bj].cont ? strlen(wb[bj].cont) : 0;
                                        size_t new_len = left_len + add_len + cont_len + 4;

                                        wb[bi].rest = realloc(wb[bi].rest, new_len);
                                        wb[bi].rest[left_len++] = '\n';
                                        memcpy(wb[bi].rest + left_len, wb[bj].rest, add_len);
                                        left_len += add_len;

                                        if (cont_len > 0) {
                                            wb[bi].rest[left_len++] = '\n';
                                            memcpy(wb[bi].rest + left_len, wb[bj].cont, cont_len);
                                            left_len += cont_len;
                                        }

                                        wb[bi].rest[left_len] = '\0';

                                        free(wb[bj].name);
                                        free(wb[bj].rest);
                                        free(wb[bj].cont);
                                        wb[bj].name = NULL;
                                        wb[bj].rest = NULL;
                                        wb[bj].cont = NULL;
                                    }
                                }

                                SB lr; sb_init(&lr);
                                sb_puts(&lr, "(letrec (");

                                for (int bi = 0; bi < wb_n; bi++) {
                                    if (!wb[bi].name) continue;
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
                                            sb_puts(&lr, eargs[ai]);
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
                                        /* Step 1: collect arity from LHS and generate hygienic parameter slots.
                                         * The left side is a pattern clause, not a lambda parameter list.
                                         * Pattern names like n, x, ys, and _ must be introduced by pmatch,
                                         * not by the lambda itself. */
                                        char *param_names[16] = {0};
                                        int param_name_count = 0;
                                        const char *ps = b->rest;
                                        while (*ps && param_name_count < 16) {
                                            while (*ps == ' ' || *ps == '\t') ps++;
                                            if (!*ps) break;
                                            if (*ps == '|') break;
                                            if (*ps == '-' && *(ps+1) == '>') break;

                                            if (*ps == '(' || *ps == '[') {
                                                char oc = *ps;
                                                char cc = oc == '(' ? ')' : ']';
                                                int gd = 0;
                                                while (*ps) {
                                                    if (*ps == oc) {
                                                        gd++;
                                                    } else if (*ps == cc) {
                                                        gd--;
                                                        ps++;
                                                        if (!gd) break;
                                                        continue;
                                                    }
                                                    ps++;
                                                }
                                            } else {
                                                while (*ps &&
                                                       *ps != ' ' &&
                                                       *ps != '\t' &&
                                                       *ps != '|' &&
                                                       !(*ps == '-' && *(ps+1) == '>')) {
                                                    ps++;
                                                }
                                            }

                                            char synthetic[32];
                                            snprintf(synthetic, sizeof(synthetic),
                                                     "__wisp_arg_%d", param_name_count);
                                            param_names[param_name_count] = strdup(synthetic);
                                            param_name_count++;
                                        }

                                        for (int ai = 0; ai < param_name_count; ai++)
                                            arity_set(at, param_names[ai], 0);

                                        /* Step 2: build raw body string (body line + continuations) */
                                        SB raw_body; sb_init(&raw_body);
                                        sb_puts(&raw_body, b->rest);
                                        if (b->cont[0]) { sb_putc(&raw_body, '\n'); sb_puts(&raw_body, b->cont); }
                                        char *raw_str = sb_take(&raw_body);

                                        /* Step 3: simple local clauses can be
                                         * lowered directly to a lambda body.
                                         * This avoids leaving raw
                                         * "args | guard -> body" syntax inside
                                         * the generated lambda. */
                                        char *expanded =
                                            wisp_try_expand_simple_where_lambda(at, raw_str);
                                        if (!expanded)
                                            EXPAND_WISP(expanded, raw_str);
                                        free(raw_str);

                                        /* Step 4: emit (lambda (p1 p2 ...) expanded_body) */
                                        if (strncmp(expanded, "(lambda ", 8) == 0) {
                                            sb_puts(&lr, expanded);
                                            for (int ai = 0; ai < param_name_count; ai++)
                                                free(param_names[ai]);
                                        } else {
                                            sb_puts(&lr, "(lambda (");
                                            for (int ai = 0; ai < param_name_count; ai++) {
                                                if (ai) sb_putc(&lr, ' ');
                                                sb_puts(&lr, param_names[ai]);
                                                free(param_names[ai]);
                                            }
                                            sb_puts(&lr, ") ");
                                            sb_puts(&lr, expanded);
                                            sb_putc(&lr, ')');
                                        }
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
                    WTokenStream body_ts = build_token_stream(body_raw, at);
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
                                    int op_prec = op_precedence(op_name);
                                    body_ts.pos++;

                                    SB next_acc; sb_init(&next_acc);
                                    sb_putc(&next_acc, '(');
                                    sb_puts(&next_acc, op_name);
                                    sb_putc(&next_acc, ' ');
                                    sb_puts(&next_acc, accum);
                                    free(op_name);
                                    free(accum);

                                    if (body_ts.pos < body_ts.count &&
                                        body_ts.tokens[body_ts.pos].lineno == op_tok2->lineno) {
                                        sb_putc(&next_acc, ' ');
                                        wisp_parse_expr(at, &body_ts, &next_acc,
                                                        -1, 1, op_prec);
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
                    lscan = wisp_skip_clause_function_name(&s, lscan);

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
                        char **guard_subjects = NULL;
                        int guard_subject_count = 0;

                        /* emit pattern tokens before | */
                        size_t pat_part_len = inline_pipe - lscan;
                        while (pat_part_len > 0 &&
                               (lscan[pat_part_len-1]==' '||lscan[pat_part_len-1]=='\t'))
                            pat_part_len--;

                        guard_subject_count = wisp_extract_guard_subjects(lscan, lscan + pat_part_len,
                                                                          &guard_subjects);

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
                            char *guard_expanded = NULL;

                            if (guard_subject_count > 0) {
                                guard_expanded = wisp_expand_pointfree_guard(at, gstart,
                                                                             guard_subjects,
                                                                             guard_subject_count);
                            } else {
                                guard_expanded = wisp_expand_expr_snippet(at, gstart);
                            }

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

                        wisp_free_guard_subjects(guard_subjects, guard_subject_count);
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
        }

        bool is_class_or_instance_line =
            (strncmp(t, "class", 5) == 0 &&
             (t[5] == ' ' || t[5] == '\t' || t[5] == '\0')) ||
            (strncmp(t, "instance", 8) == 0 &&
             (t[8] == ' ' || t[8] == '\t' || t[8] == '\0'));

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
            if (!is_class_or_instance_line &&
                _pre_depth != 0 &&
                !(*t == '(' || *t == '[' || *t == '{' || (*t == '#' && *(t+1) == '{'))) {
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
            (strncmp(t, "match", 5) == 0 &&
             (t[5] == ' ' || t[5] == '\t' || t[5] == '\0')) ||
            (strncmp(t, "method", 6) == 0 &&
             (t[6] == ' ' || t[6] == '\t' || t[6] == '\0'))) {

            bool is_method_block =
                (strncmp(t, "method", 6) == 0 &&
                 (t[6] == ' ' || t[6] == '\t' || t[6] == '\0'));

            size_t method_meta_cap = 128;
            size_t method_meta_len = 0;
            char *method_meta = malloc(method_meta_cap);
            method_meta[0] = '\0';

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

            char **block_guard_subjects = NULL;
            int block_guard_subject_count = 0;

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

                bool is_method_meta_line =
                    is_method_block &&
                    ((lt_end - lt >= 4 &&
                      strncmp(lt, ":doc", 4) == 0 &&
                      (lt + 4 == lt_end || lt[4] == ' ' || lt[4] == '\t')) ||
                     (lt_end - lt >= 6 &&
                      strncmp(lt, ":alias", 6) == 0 &&
                      (lt + 6 == lt_end || lt[6] == ' ' || lt[6] == '\t')));

                if (is_method_meta_line) {
                    size_t ltlen = lt_end - lt;
                    while (method_meta_len + ltlen + 4 >= method_meta_cap) {
                        method_meta_cap *= 2;
                        method_meta = realloc(method_meta, method_meta_cap);
                    }

                    if (method_meta_len > 0) {
                        method_meta[method_meta_len++] = ' ';
                    }
                    memcpy(method_meta + method_meta_len, lt, ltlen);
                    method_meta_len += ltlen;
                    method_meta[method_meta_len] = '\0';

                    free(lraw);
                    lineno++;
                    continue;
                }

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
                    /* Split at arrow: keep pattern verbatim, process body.
                     * In class/instance blocks we also support visual layout:
                     *
                     *   (base ^ exp)
                     *     =>
                     *     pow-int base exp
                     *
                     * In that case the current line starts with => and the
                     * previous appended item is the method pattern. */
                    size_t pat_len = fat_arrow - lt;
                    while (pat_len > 0 &&
                           (lt[pat_len-1]==' '||lt[pat_len-1]=='\t')) pat_len--;

                    if (pat_len == 0 && arrow_char == '=') {
                        size_t back = acc_len;
                        while (back > 0 &&
                               (acc[back - 1] == ' ' || acc[back - 1] == '\t')) {
                            back--;
                        }

                        if (back > 0 && acc[back - 1] == ')') {
                            size_t start = back - 1;
                            int depth_back = 0;

                            while (start > 0) {
                                start--;
                                if (acc[start] == ')') {
                                    depth_back++;
                                    continue;
                                }
                                if (acc[start] == '(') {
                                    if (depth_back == 0)
                                        break;
                                    depth_back--;
                                }
                            }

                            if (acc[start] == '(') {
                                char *pat_str = strndup(acc + start,
                                                        (size_t)(back - start));
                                acc_len = start;
                                while (acc_len > 0 &&
                                       (acc[acc_len - 1] == ' ' ||
                                        acc[acc_len - 1] == '\t')) {
                                    acc_len--;
                                }
                                acc[acc_len] = '\0';

                                const char *body_start = fat_arrow + 2;
                                while (*body_start == ' ' || *body_start == '\t')
                                    body_start++;

                                const char *body_end = lt_end;
                                while (body_end > body_start &&
                                       (*(body_end - 1) == ' ' ||
                                        *(body_end - 1) == '\t')) {
                                    body_end--;
                                }

                                char *body_src = NULL;

                                if (body_start < body_end) {
                                    body_src = strndup(body_start,
                                                       (size_t)(body_end - body_start));
                                } else {
                                    const char *next_pos = p;
                                    int body_indent = -1;
                                    SB body_acc;
                                    sb_init(&body_acc);

                                    while (*next_pos) {
                                        const char *bls = next_pos;
                                        while (*next_pos && *next_pos != '\n') next_pos++;
                                        const char *ble = next_pos;
                                        if (*next_pos == '\n') next_pos++;

                                        const char *blt = bls;
                                        while (blt < ble &&
                                               (*blt == ' ' || *blt == '\t')) {
                                            blt++;
                                        }

                                        if (blt >= ble || *blt == ';') {
                                            lineno++;
                                            p = next_pos;
                                            continue;
                                        }

                                        int bl_indent = measure_indent(bls);
                                        if (bl_indent <= l_indent) {
                                            next_pos = bls;
                                            break;
                                        }

                                        if (body_indent < 0)
                                            body_indent = bl_indent;

                                        const char *bend = get_logical_line_end(blt);
                                        while (bend > blt &&
                                               (*(bend - 1) == ' ' ||
                                                *(bend - 1) == '\t')) {
                                            bend--;
                                        }

                                        if (body_acc.len > 0)
                                            sb_putc(&body_acc, ' ');
                                        for (const char *bc = blt; bc < bend; bc++)
                                            sb_putc(&body_acc, *bc);

                                        lineno++;
                                        p = next_pos;
                                    }

                                    body_src = sb_take(&body_acc);
                                }

                                char *body_expanded = wisp_expand_expr_snippet(at, body_src);
                                free(body_src);

                                char *body_wrapped = NULL;
                                if (body_expanded[0] != '(' &&
                                    body_expanded[0] != '[' &&
                                    body_expanded[0] != '{' &&
                                    strchr(body_expanded, ' ') != NULL) {
                                    size_t bwlen = strlen(body_expanded) + 3;
                                    body_wrapped = malloc(bwlen);
                                    snprintf(body_wrapped, bwlen, "(%s)", body_expanded);
                                } else {
                                    body_wrapped = strdup(body_expanded);
                                }

                                while (acc_len + strlen(pat_str) +
                                       strlen(body_wrapped) + 8 >= acc_cap) {
                                    acc_cap *= 2;
                                    acc = realloc(acc, acc_cap);
                                }

                                acc[acc_len++] = ' ';
                                memcpy(acc + acc_len, pat_str, strlen(pat_str));
                                acc_len += strlen(pat_str);
                                acc[acc_len++] = ' ';
                                acc[acc_len++] = '=';
                                acc[acc_len++] = '>';
                                acc[acc_len++] = ' ';
                                memcpy(acc + acc_len, body_wrapped,
                                       strlen(body_wrapped));
                                acc_len += strlen(body_wrapped);
                                acc[acc_len] = '\0';

                                free(body_wrapped);
                                free(body_expanded);
                                free(pat_str);
                                free(lraw);
                                lineno++;
                                continue;
                            }
                        }
                    }

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

                    /* Accumulate all subsequent deeper body lines.
                     * A deeper line starting with | is a new guard clause,
                     * not a continuation of the previous clause body. */
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

                        if (*nlt == '|' &&
                            (nlt[1] == ' ' || nlt[1] == '\t' ||
                             nlt[1] == '\0' || nlt[1] == ';')) {
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

                    /* Trim leading whitespace */
                    const char *body_acc_trim = body_acc;
                    while (*body_acc_trim == ' ' || *body_acc_trim == '\t')
                        body_acc_trim++;

                    /* Always run the body through wisp_expand_expr_snippet so
                     * infix operators like = inside method bodies are promoted.
                     * e.g. "and s.r = o.r s.g = o.g" becomes
                     *      "(and (= s.r o.r) (= s.g o.g))" correctly. */
                    char *body_expanded;
                    if (body_acc_len == 0 || body_acc_trim[0] == '\0') {
                        body_expanded = strdup("(undefined)");
                        free(body_acc);
                    } else {
                        /* Run through full wisp expansion to promote infix ops */
                        body_expanded = wisp_expand_expr_snippet(at, body_acc_trim);
                        free(body_acc);
                        /* If still multi-token and not grouped, wrap in parens */
                        bool already_grouped = (body_expanded[0] == '(' ||
                                                body_expanded[0] == '[' ||
                                                body_expanded[0] == '{');
                        bool single_token = (strchr(body_expanded, ' ') == NULL);
                        if (!already_grouped && !single_token) {
                            size_t bflen = strlen(body_expanded);
                            char *wrapped = malloc(bflen + 3);
                            wrapped[0] = '(';
                            memcpy(wrapped + 1, body_expanded, bflen);
                            wrapped[bflen + 1] = ')';
                            wrapped[bflen + 2] = '\0';
                            free(body_expanded);
                            body_expanded = wrapped;
                        }
                    }

                    /* Build: (#line N 1) pat arrow body.
                     *
                     * Also expand method guard shorthand:
                     *   c | >= 'a' and <= 'z'
                     * becomes:
                     *   c | (and (>= c 'a') (<= c 'z'))
                     *
                     * Continuation guards inherit the previous explicit
                     * pattern subjects:
                     *   | = '\t'
                     * becomes:
                     *   | (= c '\t')
                     */
                    char *pat_emit = NULL;
                    const char *inline_pipe = NULL;

                    {
                        int pdepth = 0;
                        bool pin_str = false;

                        for (const char *q = pat_str; *q; q++) {
                            if (pin_str) {
                                if (*q == '\\' && q[1]) q++;
                                else if (*q == '"') pin_str = false;
                                continue;
                            }

                            if (*q == '"') {
                                pin_str = true;
                                continue;
                            }

                            if (*q == '(' || *q == '[' || *q == '{') {
                                pdepth++;
                                continue;
                            }

                            if (*q == ')' || *q == ']' || *q == '}') {
                                if (pdepth > 0) pdepth--;
                                continue;
                            }

                            if (pdepth == 0 &&
                                *q == '|' &&
                                (q == pat_str || q[-1] == ' ' || q[-1] == '\t') &&
                                (q[1] == ' ' || q[1] == '\t' || q[1] == '\0')) {
                                inline_pipe = q;
                                break;
                            }
                        }
                    }

                    if (inline_pipe) {
                        size_t pat_part_len = (size_t)(inline_pipe - pat_str);
                        while (pat_part_len > 0 &&
                               (pat_str[pat_part_len - 1] == ' ' ||
                                pat_str[pat_part_len - 1] == '\t')) {
                            pat_part_len--;
                        }

                        if (pat_part_len > 0) {
                            wisp_free_guard_subjects(block_guard_subjects,
                                                     block_guard_subject_count);
                            block_guard_subjects = NULL;
                            block_guard_subject_count =
                                wisp_extract_guard_subjects(pat_str,
                                                            pat_str + pat_part_len,
                                                            &block_guard_subjects);
                        }

                        const char *gstart = inline_pipe + 1;
                        while (*gstart == ' ' || *gstart == '\t') gstart++;

                        char *guard_expanded = NULL;
                        if (*gstart == '\0') {
                            guard_expanded = strdup("True");
                        } else if (block_guard_subject_count > 0) {
                            guard_expanded =
                                wisp_expand_pointfree_guard(at, gstart,
                                                            block_guard_subjects,
                                                            block_guard_subject_count);
                        } else {
                            guard_expanded = wisp_expand_expr_snippet(at, gstart);
                        }

                        bool guard_grouped = (guard_expanded[0] == '(' ||
                                              guard_expanded[0] == '[' ||
                                              guard_expanded[0] == '{');
                        bool guard_single = (strchr(guard_expanded, ' ') == NULL);

                        char *guard_emit = guard_expanded;
                        if (!guard_grouped && !guard_single) {
                            size_t glen = strlen(guard_expanded);
                            guard_emit = malloc(glen + 3);
                            guard_emit[0] = '(';
                            memcpy(guard_emit + 1, guard_expanded, glen);
                            guard_emit[glen + 1] = ')';
                            guard_emit[glen + 2] = '\0';
                            free(guard_expanded);
                        }

                        size_t emit_len = pat_part_len + strlen(guard_emit) + 5;
                        pat_emit = malloc(emit_len);

                        if (pat_part_len > 0) {
                            snprintf(pat_emit, emit_len, "%.*s | %s",
                                     (int)pat_part_len, pat_str, guard_emit);
                        } else {
                            snprintf(pat_emit, emit_len, "| %s", guard_emit);
                        }

                        free(guard_emit);
                    } else {
                        char **new_subjects = NULL;
                        int new_subject_count =
                            wisp_extract_guard_subjects(pat_str,
                                                        pat_str + strlen(pat_str),
                                                        &new_subjects);

                        if (new_subject_count > 0) {
                            wisp_free_guard_subjects(block_guard_subjects,
                                                     block_guard_subject_count);
                            block_guard_subjects = new_subjects;
                            block_guard_subject_count = new_subject_count;
                        } else {
                            wisp_free_guard_subjects(new_subjects,
                                                     new_subject_count);
                        }

                        if (is_method_block && !wisp_has_top_level_pipe(pat_str)) {
                            size_t plen = strlen(pat_str);
                            pat_emit = malloc(plen + 8);
                            snprintf(pat_emit, plen + 8, "%s | True", pat_str);

                            if (wisp_debug_enabled()) {
                                fprintf(stderr,
                                        "[wisp-method] default guard line=%d pat='%s' emit='%s'\n",
                                        lineno, pat_str, pat_emit);
                            }
                        } else {
                            pat_emit = strdup(pat_str);
                        }
                    }

                    size_t pat_emit_len = strlen(pat_emit);
                    size_t line_len = is_method_block ? 1 : (size_t)_ld2len;
                    size_t need = acc_len + line_len + pat_emit_len + 4 + strlen(body_expanded) + 4;
                    while (need >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }

                    if (is_method_block) {
                        acc[acc_len++] = ' ';
                    } else {
                        memcpy(acc + acc_len, _ld2, _ld2len);
                        acc_len += _ld2len;
                    }

                    if (wisp_debug_enabled()) {
                        fprintf(stderr,
                                "[wisp-method] append clause line=%d pat='%s' body='%s' before_len=%zu\n",
                                lineno, pat_emit, body_expanded, acc_len);
                    }

                    memcpy(acc + acc_len, pat_emit, pat_emit_len); acc_len += pat_emit_len;

                    acc[acc_len++] = ' '; acc[acc_len++] = arrow_char; acc[acc_len++] = '>';
                    acc[acc_len++] = ' ';
                    size_t bel = strlen(body_expanded);
                    while (acc_len + bel + 2 >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
                    memcpy(acc + acc_len, body_expanded, bel); acc_len += bel;
                    acc[acc_len] = '\0';

                    free(pat_emit);
                    free(body_expanded);
                    free(pat_str);
                } else {
                    /* :: signature line or other -- keep verbatim */
                    size_t ltlen = lt_end - lt;
                    size_t line_len = is_method_block ? 1 : (size_t)_ld2len;
                    while (acc_len + line_len + ltlen + 4 >= acc_cap) {
                        acc_cap *= 2; acc = realloc(acc, acc_cap);
                    }

                    if (is_method_block) {
                        acc[acc_len++] = ' ';
                    } else {
                        memcpy(acc + acc_len, _ld2, _ld2len);
                        acc_len += _ld2len;
                    }

                    if (wisp_debug_enabled()) {
                        fprintf(stderr,
                                "[wisp-method] append raw line=%d text='%.*s' before_len=%zu\n",
                                lineno, (int)ltlen, lt, acc_len);
                    }

                    memcpy(acc + acc_len, lt, ltlen); acc_len += ltlen;
                    acc[acc_len] = '\0';
                }
                free(lraw);
                lineno++;
            }

            if (method_meta_len > 0) {
                while (acc_len + method_meta_len + 5 >= acc_cap) {
                    acc_cap *= 2;
                    acc = realloc(acc, acc_cap);
                }
                acc[acc_len++] = ' ';
                memcpy(acc + acc_len, method_meta, method_meta_len);
                acc_len += method_meta_len;
                acc[acc_len] = '\0';
            }

            while (acc_len + 2 >= acc_cap) { acc_cap *= 2; acc = realloc(acc, acc_cap); }
            acc[acc_len++] = ')';
            acc[acc_len]   = '\0';

            wisp_free_guard_subjects(block_guard_subjects,
                                     block_guard_subject_count);

            if (wisp_debug_enabled()) {
                fprintf(stderr,
                        "[wisp-block] final token line=%d text='%s'\n",
                        lineno - 1, acc);
            }

            wts_push(&s, acc, indent, lineno - 1);
            free(acc);
            free(method_meta);
            continue;
        }

        /* class and instance blocks are semantic blocks, not raw text.
         * The reader expects one complete form:
         *
         *   (class C a where ...)
         *   (instance C T where type Result T = R (x op y) => body)
         *
         * Wisp owns the visual layout, so it must keep associated types and
         * method implementations inside the same emitted form. */
        if ((strncmp(t, "class", 5) == 0 &&
             (t[5] == ' ' || t[5] == '\t' || t[5] == '\0')) ||
            (strncmp(t, "instance", 8) == 0 &&
             (t[8] == ' ' || t[8] == '\t' || t[8] == '\0'))) {

            size_t acc_cap = 256;
            char *acc = malloc(acc_cap);
            size_t acc_len = 0;
            acc[0] = '\0';

            acc[acc_len++] = '(';
            acc[acc_len] = '\0';

            const char *t_end = get_logical_line_end(t);
            while (t_end > t && (*(t_end - 1) == ' ' ||
                                 *(t_end - 1) == '\t')) {
                t_end--;
            }

            size_t tlen = (size_t)(t_end - t);
            while (acc_len + tlen + 4 >= acc_cap) {
                acc_cap *= 2;
                acc = realloc(acc, acc_cap);
            }

            memcpy(acc + acc_len, t, tlen);
            acc_len += tlen;
            acc[acc_len] = '\0';

            free(raw);
            lineno++;

            char *pending_pattern = NULL;
            int pending_pattern_line = 0;

            while (*p) {
                const char *ls = p;
                while (*p && *p != '\n') p++;
                const char *le = p;
                if (*p == '\n') p++;

                char *lraw = strndup(ls, (size_t)(le - ls));
                const char *lt = lraw;
                while (*lt == ' ' || *lt == '\t') lt++;

                if (!*lt || *lt == ';') {
                    free(lraw);
                    lineno++;
                    continue;
                }

                int l_indent = measure_indent(lraw);
                if (l_indent <= indent) {
                    p = ls;
                    free(lraw);
                    break;
                }

                const char *lt_end = get_logical_line_end(lt);
                while (lt_end > lt &&
                       (*(lt_end - 1) == ' ' || *(lt_end - 1) == '\t')) {
                    lt_end--;
                }

                char line_tag[48];
                int line_tag_len =
                    snprintf(line_tag, sizeof(line_tag),
                             " (#line %d 1) ", lineno);

                const char *fat_arrow = strstr(lt, "::")
                    ? NULL
                    : wisp_find_top_level_fat_arrow_range(lt, lt_end);

                if (fat_arrow || pending_pattern) {
                    char *pattern = NULL;
                    int pattern_line = lineno;

                    if (fat_arrow && fat_arrow > lt) {
                        pattern = wisp_trim_range_dup(lt, fat_arrow);
                    } else if (pending_pattern) {
                        pattern = pending_pattern;
                        pending_pattern = NULL;
                        pattern_line = pending_pattern_line;
                    } else {
                        free(lraw);
                        lineno++;
                        continue;
                    }

                    /* Regular Wisp writes method clauses as `method args ->`.
                     * The core reader's class AST uses `(method args) =>`, so
                     * group an unparenthesized Wisp pattern during lowering. */
                    if (pattern[0] != '(') {
                        size_t pattern_len = strlen(pattern);
                        char *grouped_pattern = malloc(pattern_len + 3);
                        grouped_pattern[0] = '(';
                        memcpy(grouped_pattern + 1, pattern, pattern_len);
                        grouped_pattern[pattern_len + 1] = ')';
                        grouped_pattern[pattern_len + 2] = '\0';
                        free(pattern);
                        pattern = grouped_pattern;
                    }

                    const char *body_start = fat_arrow ? fat_arrow + 2 : lt;
                    while (body_start < lt_end &&
                           (*body_start == ' ' || *body_start == '\t')) {
                        body_start++;
                    }

                    SB body_src;
                    sb_init(&body_src);
                    int body_base_indent = -1;

                    if (body_start < lt_end) {
                        for (const char *bc = body_start; bc < lt_end; bc++)
                            sb_putc(&body_src, *bc);
                    }

                    while (*p) {
                        const char *bls = p;
                        while (*p && *p != '\n') p++;
                        const char *ble = p;
                        if (*p == '\n') p++;

                        char *braw = strndup(bls, (size_t)(ble - bls));
                        const char *bt = braw;
                        while (*bt == ' ' || *bt == '\t') bt++;

                        if (!*bt || *bt == ';') {
                            free(braw);
                            lineno++;
                            continue;
                        }

                        int b_indent = measure_indent(braw);
                        if (b_indent <= l_indent) {
                            p = bls;
                            free(braw);
                            break;
                        }

                        const char *bt_end = get_logical_line_end(bt);
                        while (bt_end > bt &&
                               (*(bt_end - 1) == ' ' ||
                                *(bt_end - 1) == '\t')) {
                            bt_end--;
                        }

                        if (body_base_indent < 0)
                            body_base_indent = b_indent;

                        if (body_src.len > 0)
                            sb_putc(&body_src, '\n');

                        /* Keep layout relative to the first method-body line.
                         * Nested if/then/else belongs to Wisp's indentation
                         * grammar; flattening every line here changes which
                         * else belongs to which if. */
                        for (int pad = body_base_indent; pad < b_indent; pad++)
                            sb_putc(&body_src, ' ');

                        for (const char *bc = bt; bc < bt_end; bc++)
                            sb_putc(&body_src, *bc);

                        free(braw);
                        lineno++;
                    }

                    char *body_src_text = sb_take(&body_src);
                    char *body_core =
                        wisp_expand_instance_method_body(at, body_src_text);
                    free(body_src_text);

                    char *body_expanded = NULL;
                    bool body_grouped = wisp_is_single_grouped_form(body_core);
                    bool body_single_token =
                        strchr(body_core, ' ') == NULL &&
                        strchr(body_core, '\t') == NULL;

                    if (!body_grouped && !body_single_token) {
                        size_t body_core_len = strlen(body_core);
                        body_expanded = malloc(body_core_len + 3);
                        body_expanded[0] = '(';
                        memcpy(body_expanded + 1, body_core, body_core_len);
                        body_expanded[body_core_len + 1] = ')';
                        body_expanded[body_core_len + 2] = '\0';
                        free(body_core);
                    } else {
                        body_expanded = body_core;
                    }

                    char method_tag[48];
                    int method_tag_len =
                        snprintf(method_tag, sizeof(method_tag),
                                 " (#line %d 1) ", pattern_line);

                    size_t need = acc_len + (size_t)method_tag_len +
                                  strlen(pattern) + strlen(body_expanded) + 8;

                    while (need >= acc_cap) {
                        acc_cap *= 2;
                        acc = realloc(acc, acc_cap);
                    }

                    memcpy(acc + acc_len, method_tag, (size_t)method_tag_len);
                    acc_len += (size_t)method_tag_len;

                    memcpy(acc + acc_len, pattern, strlen(pattern));
                    acc_len += strlen(pattern);

                    acc[acc_len++] = ' ';
                    acc[acc_len++] = '=';
                    acc[acc_len++] = '>';
                    acc[acc_len++] = ' ';

                    memcpy(acc + acc_len, body_expanded,
                           strlen(body_expanded));
                    acc_len += strlen(body_expanded);
                    acc[acc_len] = '\0';

                    free(pattern);
                    free(body_expanded);
                    free(lraw);
                    lineno++;
                    continue;
                }

                bool looks_like_method_pattern =
                    lt < lt_end &&
                    *lt == '(' &&
                    !strstr(lt, "::") &&
                    !strstr(lt, "=>");

                if (looks_like_method_pattern) {
                    free(pending_pattern);
                    pending_pattern = wisp_trim_range_dup(lt, lt_end);
                    pending_pattern_line = lineno;
                    free(lraw);
                    lineno++;
                    continue;
                }

                size_t ltlen = (size_t)(lt_end - lt);
                size_t need = acc_len + (size_t)line_tag_len + ltlen + 4;

                while (need >= acc_cap) {
                    acc_cap *= 2;
                    acc = realloc(acc, acc_cap);
                }

                memcpy(acc + acc_len, line_tag, (size_t)line_tag_len);
                acc_len += (size_t)line_tag_len;
                memcpy(acc + acc_len, lt, ltlen);
                acc_len += ltlen;
                acc[acc_len] = '\0';

                free(lraw);
                lineno++;
            }

            free(pending_pattern);

            while (acc_len + 2 >= acc_cap) {
                acc_cap *= 2;
                acc = realloc(acc, acc_cap);
            }

            acc[acc_len++] = ')';
            acc[acc_len] = '\0';

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

                /* Case -1: define (name [p : T] ... -> R)
                 * Bare Wisp accepts the same parenthesized function header that
                 * the reader accepts inside (define ...). Keep the header as one
                 * grouped token and let the indented body supply the function
                 * body/pattern clauses. Do not append a dummy [] clause. */
                if (*after_define == '(') {
                    const char *line_end = get_logical_line_end(after_define);
                    char *header = strndup(after_define, line_end - after_define);

                    /* Register the function arity from top-level arrows in the
                     * header so calls in subsequent Wisp code expand correctly. */
                    const char *hp = header + 1;
                    while (*hp == ' ' || *hp == '\t') hp++;
                    const char *hn = hp;
                    while (*hp && *hp != ' ' && *hp != '\t' && *hp != ')') hp++;
                    if (hp > hn) {
                        char *fname2 = strndup(hn, hp - hn);
                        int arr_count2 = 0;
                        int d2 = 0;
                        bool ins2 = false;
                        for (const char *q2 = hp; *q2; q2++) {
                            if (ins2) {
                                if (*q2 == '\\' && *(q2+1)) q2++;
                                else if (*q2 == '"') ins2 = false;
                                continue;
                            }
                            if (*q2 == '"') { ins2 = true; continue; }
                            if (*q2 == '(' || *q2 == '[' || *q2 == '{') d2++;
                            else if (*q2 == ')' || *q2 == ']' || *q2 == '}') { if (d2 > 0) d2--; }
                            else if (d2 == 0 && *q2 == '-' && *(q2+1) == '>') arr_count2++;
                        }
                        arity_set(at, fname2, arr_count2);
                        free(fname2);
                    }

                    wts_push(&s, "define", indent, lineno);
                    wts_push(&s, header, indent, lineno);
                    free(header);
                    free(raw);
                    lineno++;
                    continue;
                }

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
                                SB form;
                                sb_init(&form);
                                sb_puts(&form, "(define ");
                                sb_puts(&form, _bracket);
                                sb_puts(&form, " [])");
                                char *define_form = sb_take(&form);
                                wts_push(&s, define_form, indent, lineno);
                                free(define_form);
                                free(raw);
                                lineno++;
                                goto next_line;
                            }
                        }
                    }
                }
                _not_array_decl:;
                /* Case 0b: define name :: Type value
                 * Wisp value binding sugar. Lisp keeps the bracketed layer
                 * explicit as (define [name :: Type] value), but Wisp users
                 * may write the clearer unbracketed form. */

                /* Case 0a: define [name :: Type] value :attr val ...
                 * The name is a full bracketed annotation. after_name points
                 * directly at the value (and any trailing :alias etc).
                 * Collect the entire rest of the line as one token so the
                 * reader sees: (define [name :: Type] value :alias sym)     */
                if (*after_define == '[' && name_end > after_define) {
                    const char *rest = after_name;
                    const char *rest_end = get_logical_line_end(rest);
                    const char *physical_end = strchr(rest, '\n');
                    if (physical_end && physical_end < rest_end)
                        rest_end = physical_end;
                    while (rest_end > rest && (*(rest_end-1) == ' ' || *(rest_end-1) == '\t')) rest_end--;
                    if (rest < rest_end) {
                        char *bracket = strndup(after_define, name_end - after_define);
                        char *rest_src = strndup(rest, rest_end - rest);
                        /* expand the whole value expression through wisp.
                         * Only split trailing metadata when a top-level
                         * :keyword appears after whitespace. This keeps:
                         *   define [delta :: Int] 'a' - 'A'
                         *   define [space :: Char] ' '
                         * intact instead of cutting them at the first space. */
                        const char *attr_p = rest_end;
                        const char *scan = rest;
                        int scan_depth = 0;

                        while (scan < rest_end) {
                            if (*scan == '"') {
                                scan++;
                                while (scan < rest_end) {
                                    if (*scan == '\\' && scan + 1 < rest_end) {
                                        scan += 2;
                                        continue;
                                    }
                                    if (*scan == '"') {
                                        scan++;
                                        break;
                                    }
                                    scan++;
                                }
                                continue;
                            }

                            if (*scan == '\'') {
                                scan++;
                                while (scan < rest_end) {
                                    if (*scan == '\\' && scan + 1 < rest_end) {
                                        scan += 2;
                                        continue;
                                    }
                                    if (*scan == '\'') {
                                        scan++;
                                        break;
                                    }
                                    scan++;
                                }
                                continue;
                            }

                            if ((unsigned char)scan[0] == 0xE2 &&
                                (unsigned char)scan[1] == 0x8C &&
                                (unsigned char)scan[2] == 0x9C) {
                                scan = skip_corner_quote_chars(scan);
                                continue;
                            }

                            if (*scan == '(' || *scan == '[' || *scan == '{') {
                                scan_depth++;
                                scan++;
                                continue;
                            }

                            if (*scan == ')' || *scan == ']' || *scan == '}') {
                                if (scan_depth > 0) scan_depth--;
                                scan++;
                                continue;
                            }

                            if (scan_depth == 0 &&
                                *scan == ':' &&
                                scan + 1 < rest_end &&
                                scan[1] != ':' &&
                                scan > rest &&
                                (scan[-1] == ' ' || scan[-1] == '\t')) {
                                attr_p = scan;
                                break;
                            }

                            if (scan_depth == 0 &&
                                scan > rest &&
                                (scan[-1] == ' ' || scan[-1] == '\t') &&
                                rest_end - scan >= 14 &&
                                strncmp(scan, "Documentation.", 14) == 0 &&
                                (scan + 14 == rest_end ||
                                 scan[14] == ' ' || scan[14] == '\t' ||
                                 scan[14] == ';')) {
                                attr_p = scan;
                                break;
                            }

                            if (scan_depth == 0 && *scan == ';') {
                                attr_p = scan;
                                break;
                            }

                            scan++;
                        }

                        const char *value_end = attr_p;
                        while (value_end > rest &&
                               (value_end[-1] == ' ' || value_end[-1] == '\t')) {
                            value_end--;
                        }

                        size_t val_len = value_end - rest;
                        char *val_src = strndup(rest, val_len);
                        char *val_tok = wisp_expand_expr_snippet(at, val_src);
                        free(val_src);

                        /* collect rest of line (attrs like :alias sym) */
                        while (attr_p < rest_end && (*attr_p == ' ' || *attr_p == '\t')) attr_p++;
                        SB full_val; sb_init(&full_val);
                        sb_puts(&full_val, val_tok);
                        free(val_tok);
                        if (attr_p < rest_end && *attr_p == ':') {
                            sb_putc(&full_val, ' ');
                            wisp_append_metadata_range(&full_val, attr_p, rest_end);
                        } else if (attr_p < rest_end &&
                                   rest_end - attr_p >= 14 &&
                                   strncmp(attr_p, "Documentation.", 14) == 0) {
                            sb_puts(&full_val, " :doc \"Documentation.\"");
                        }
                        char *full_val_str = sb_take(&full_val);

                        while (*p) {
                            const char *mls = p;
                            while (*p && *p != '\n') p++;
                            const char *mle = p;
                            if (*p == '\n') p++;

                            char *mraw = strndup(mls, (size_t)(mle - mls));
                            if (wisp_text_ends_inside_string(mraw)) {
                                size_t mcap = strlen(mraw) + 256;
                                size_t mlen = strlen(mraw);
                                char *joined = malloc(mcap);
                                if (!joined) {
                                    fprintf(stderr, "[wisp] out of memory joining multiline metadata at line %d\n",
                                            lineno);
                                    abort();
                                }

                                memcpy(joined, mraw, mlen);
                                joined[mlen] = '\0';
                                free(mraw);

                                while (wisp_text_ends_inside_string(joined) && *p) {
                                    const char *next_start = p;
                                    while (*p && *p != '\n') p++;
                                    size_t next_len = (size_t)(p - next_start);
                                    if (*p == '\n') p++;

                                    while (mlen + next_len + 2 >= mcap) {
                                        mcap *= 2;
                                        char *grown = realloc(joined, mcap);
                                        if (!grown) {
                                            fprintf(stderr, "[wisp] out of memory growing multiline metadata at line %d\n",
                                                    lineno);
                                            free(joined);
                                            abort();
                                        }
                                        joined = grown;
                                    }

                                    joined[mlen++] = '\n';
                                    memcpy(joined + mlen, next_start, next_len);
                                    mlen += next_len;
                                    joined[mlen] = '\0';
                                    lineno++;
                                }

                                if (wisp_text_ends_inside_string(joined)) {
                                    free(joined);
                                    wisp_syntax_error(
                                        lineno,
                                        1,
                                        "unterminated string literal",
                                        "close the string with a double quote before the end of the file");
                                }

                                mraw = joined;
                            }
                            const char *mt = mraw;
                            while (*mt == ' ' || *mt == '\t') mt++;

                            if (!*mt || *mt == '\r' || *mt == ';') {
                                p = mls;
                                free(mraw);
                                break;
                            }

                            int meta_indent = measure_indent(mraw);
                            bool is_define_meta =
                                meta_indent > indent &&
                                ((strncmp(mt, ":alias", 6) == 0 &&
                                  (mt[6] == ' ' || mt[6] == '\t' ||
                                   mt[6] == '\r' || mt[6] == '\0')) ||
                                 (strncmp(mt, ":doc", 4) == 0 &&
                                  (mt[4] == ' ' || mt[4] == '\t' ||
                                   mt[4] == '\r' || mt[4] == '\0')) ||
                                 (strncmp(mt, "Documentation.", 14) == 0 &&
                                  (mt[14] == '\0' || mt[14] == ' ' ||
                                   mt[14] == '\t' || mt[14] == '\r' ||
                                   mt[14] == ';')));

                            if (!is_define_meta) {
                                p = mls;
                                free(mraw);
                                break;
                            }

                            const char *mt_end = get_logical_line_end(mt);
                            while (mt_end > mt &&
                                   (*(mt_end - 1) == ' ' || *(mt_end - 1) == '\t')) {
                                mt_end--;
                            }

                            SB meta_sb;
                            sb_init(&meta_sb);
                            sb_puts(&meta_sb, full_val_str);
                            sb_putc(&meta_sb, ' ');
                            if (strncmp(mt, "Documentation.", 14) == 0) {
                                sb_puts(&meta_sb, ":doc \"Documentation.\"");
                            } else {
                                wisp_append_metadata_range(&meta_sb, mt, mt_end);
                            }

                            free(full_val_str);
                            full_val_str = sb_take(&meta_sb);

                            free(mraw);
                            lineno++;
                        }

                        SB form;
                        sb_init(&form);
                        sb_puts(&form, "(define ");
                        sb_puts(&form, bracket);
                        sb_putc(&form, ' ');
                        sb_puts(&form, full_val_str);
                        sb_putc(&form, ')');
                        char *form_str = sb_take(&form);
                        wts_push(&s, form_str, indent, lineno);
                        free(form_str);
                        free(bracket);
                        free(full_val_str);
                        free(rest_src);
                        free(raw);
                        lineno++;
                        goto next_line;
                    }
                }
                if (after_name[0] == ':' && after_name[1] == ':' &&
                    !wisp_find_top_level_arrow(after_name + 2)) {
                    size_t name_len = name_end - after_define;
                    const char *type_start = after_name + 2;
                    while (*type_start == ' ' || *type_start == '\t') type_start++;

                    const char *type_end = type_start;
                    if (*type_end == '(' || *type_end == '[' || *type_end == '{') {
                        type_end = skip_balanced_chars(type_end);
                    } else {
                        while (*type_end && *type_end != ' ' && *type_end != '\t' && *type_end != ';')
                            type_end++;
                    }
                    while (true) {
                        const char *q = type_end;
                        while (*q == ' ' || *q == '\t') q++;
                        if (!(q[0] == ':' && q[1] == ':')) break;
                        q += 2;
                        while (*q == ' ' || *q == '\t') q++;
                        if (*q == '(' || *q == '[' || *q == '{') q = skip_balanced_chars(q);
                        else while (*q && *q != ' ' && *q != '\t' && *q != ';') q++;
                        type_end = q;
                    }

                    const char *value_start = type_end;
                    while (*value_start == ' ' || *value_start == '\t') value_start++;
                    if (*value_start && *value_start != ';') {
                        const char *value_end = get_logical_line_end(value_start);
                        char *value_src = strndup(value_start, (size_t)(value_end - value_start));
                        char *value_tok = wisp_expand_expr_snippet(at, value_src);
                        free(value_src);
                        const char *attr_scan_src = value_end;

                        size_t type_len = (size_t)(type_end - type_start);
                        size_t blen = name_len + type_len + 8;
                        char *bracket = malloc(blen);
                        snprintf(bracket, blen, "[%.*s :: %.*s]",
                                 (int)name_len, after_define,
                                 (int)type_len, type_start);

                        /* Collect trailing :keyword value pairs on same line
                         * e.g. :alias pi so they arrive inside the define   */
                        const char *attr_scan = attr_scan_src;
                        while (*attr_scan == ' ' || *attr_scan == '\t') attr_scan++;
                        char *value_with_attrs = NULL;
                        if (*attr_scan == ':' && *(attr_scan + 1) != ':') {
                            SB attr_sb; sb_init(&attr_sb);
                            sb_puts(&attr_sb, value_tok);
                            free(value_tok);
                            while (*attr_scan == ':' && *(attr_scan + 1) != ':') {
                                const char *kw_s = attr_scan;
                                while (*attr_scan && *attr_scan != ' ' && *attr_scan != '\t' && *attr_scan != ';') attr_scan++;
                                sb_putc(&attr_sb, ' ');
                                for (const char *_c = kw_s; _c < attr_scan; _c++) sb_putc(&attr_sb, *_c);
                                while (*attr_scan == ' ' || *attr_scan == '\t') attr_scan++;
                                if (*attr_scan && *attr_scan != ';' && *attr_scan != ':') {
                                    const char *vs = attr_scan;
                                    sb_putc(&attr_sb, ' ');
                                    if ((size_t)(attr_scan - kw_s) == 4 &&
                                        strncmp(kw_s, ":doc", 4) == 0) {
                                        const char *msg_end = get_logical_line_end(vs);
                                        char *quoted = wisp_quote_string_range(vs, msg_end);
                                        sb_puts(&attr_sb, quoted);
                                        free(quoted);
                                        attr_scan = msg_end;
                                    } else {
                                        while (*attr_scan && *attr_scan != ' ' && *attr_scan != '\t' && *attr_scan != ';') attr_scan++;
                                        for (const char *_c = vs; _c < attr_scan; _c++) sb_putc(&attr_sb, *_c);
                                    }
                                }
                                while (*attr_scan == ' ' || *attr_scan == '\t') attr_scan++;
                            }
                            value_with_attrs = sb_take(&attr_sb);
                        } else {
                            value_with_attrs = value_tok;
                        }
                        wts_push(&s, "define", indent, lineno);
                        wts_push(&s, bracket,  indent, lineno);
                        wts_push(&s, value_with_attrs, indent, lineno);
                        free(bracket);
                        free(value_with_attrs);
                        free(raw);
                        lineno++;
                        continue;
                    }
                }

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

                    /* Case 1a: define name :: T1 -> ... -> R
                     *              x y -> body
                     * If the first clause is only simple variable names, use
                     * those names as the function parameters and compile the
                     * right-hand side as the direct body. Pattern clauses that
                     * contain _, literals, [], [x|xs], guards, etc. keep the
                     * normal pattern-matching path. */
                    if (arr_count > 0) {
                        const char *body_line = p;
                        while (*body_line) {
                            const char *candidate_start = body_line;
                            const char *candidate_end = body_line;
                            while (*candidate_end && *candidate_end != '\n')
                                candidate_end++;

                            const char *ct = candidate_start;
                            while (ct < candidate_end && (*ct == ' ' || *ct == '\t'))
                                ct++;

                            int candidate_indent = measure_indent(candidate_start);
                            if (ct < candidate_end && candidate_indent > indent &&
                                *ct != ';' &&
                                strncmp(ct, ":doc", 4) != 0 &&
                                strncmp(ct, ":alias", 6) != 0 &&
                                wisp_find_top_level_arrow(ct)) {
                                break;
                            }

                            if (ct < candidate_end && candidate_indent <= indent)
                                break;

                            body_line = (*candidate_end == '\n') ? candidate_end + 1 : candidate_end;
                        }

                        if (*body_line) {
                            const char *line_start = body_line;
                            const char *line_end0 = body_line;
                            while (*line_end0 && *line_end0 != '\n') line_end0++;
                            const char *next_line = (*line_end0 == '\n') ? line_end0 + 1 : line_end0;
                            const char *after_body = next_line;
                            bool followed_by_where = false;
                            const char *where_line_start = NULL;
                            int where_indent = -1;
                            while (*after_body) {
                                const char *als = after_body;
                                while (*after_body && *after_body != '\n') after_body++;
                                const char *alt = als;
                                while (*alt == ' ' || *alt == '\t') alt++;
                                if (*alt && *alt != ';') {
                                    followed_by_where =
                                        strncmp(alt, "where", 5) == 0 &&
                                        (alt[5] == '\0' || alt[5] == ' ' || alt[5] == '\t' || alt[5] == '\n');
                                    if (followed_by_where) {
                                        where_line_start = als;
                                        where_indent = measure_indent(als);
                                    }
                                    break;
                                }
                                if (*after_body == '\n') after_body++;
                            }
                            int body_indent = measure_indent(line_start);
                            char *body_raw = strndup(line_start, (size_t)(line_end0 - line_start));
                            const char *bt = body_raw;
                            while (*bt == ' ' || *bt == '\t') bt++;
                            const char *body_arrow = wisp_find_top_level_arrow(bt);
                            if (body_indent > indent && body_arrow) {
                                char *left = wisp_trim_range_dup(bt, body_arrow);
                                char *names[WISP_MAX_PARAMS] = {0};
                                const char *guard_pipe =
                                    wisp_find_top_level_pipe_range(bt, body_arrow);
                                if (guard_pipe && arr_count < WISP_MAX_PARAMS) {
                                    char *guard_params =
                                        wisp_trim_range_dup(bt, guard_pipe);
                                    if (wisp_parse_simple_param_names(guard_params,
                                                                      arr_count,
                                                                      names)) {
                                        const char *block_end = line_start;
                                        const char *scan_clause = line_start;
                                        while (*scan_clause) {
                                            const char *cls = scan_clause;
                                            while (*scan_clause && *scan_clause != '\n')
                                                scan_clause++;
                                            const char *cle = scan_clause;
                                            const char *clt = cls;
                                            while (clt < cle &&
                                                   (*clt == ' ' || *clt == '\t'))
                                                clt++;
                                            if (clt < cle && *clt != ';') {
                                                int cl_indent = measure_indent(cls);
                                                if (cl_indent <= indent || *clt == ':')
                                                    break;
                                            }
                                            block_end = (*scan_clause == '\n')
                                                            ? scan_clause + 1
                                                            : scan_clause;
                                            if (*scan_clause == '\n')
                                                scan_clause++;
                                        }

                                        char *block_src =
                                            strndup(line_start,
                                                    (size_t)(block_end - line_start));
                                        char *lambda =
                                            wisp_try_expand_simple_where_lambda(at,
                                                                               block_src);
                                        free(block_src);

                                        if (lambda) {
                                            char *param_types[WISP_MAX_PARAMS] = {0};
                                            char *ret_type = NULL;
                                            if (wisp_split_arrow_signature(sig_rest, arr_count,
                                                                           param_types,
                                                                           &ret_type)) {
                                                wisp_validate_ignored_signature_names(param_types,
                                                                                      names,
                                                                                      arr_count,
                                                                                      lineno);
                                                for (int i = 0; i < arr_count; i++)
                                                    arity_set(at, names[i], 0);

                                                SB hdr;
                                                sb_init(&hdr);
                                                sb_putc(&hdr, '(');
                                                sb_puts(&hdr, fname);
                                                for (int i = 0; i < arr_count; i++) {
                                                    const char *ptype = param_types[i];
                                                    while (*ptype == ' ' || *ptype == '\t')
                                                        ptype++;
                                                    sb_puts(&hdr, " [");
                                                    sb_puts(&hdr, names[i]);
                                                    sb_puts(&hdr, " : ");
                                                    sb_puts(&hdr, ptype);
                                                    sb_putc(&hdr, ']');
                                                }
                                                sb_puts(&hdr, " -> ");
                                                const char *rtype = ret_type;
                                                while (*rtype == ' ' || *rtype == '\t')
                                                    rtype++;
                                                sb_puts(&hdr, rtype);
                                                sb_putc(&hdr, ')');
                                                char *header = sb_take(&hdr);

                                                SB call;
                                                sb_init(&call);
                                                sb_putc(&call, '(');
                                                sb_puts(&call, lambda);
                                                for (int i = 0; i < arr_count; i++) {
                                                    sb_putc(&call, ' ');
                                                    sb_puts(&call, names[i]);
                                                }
                                                sb_putc(&call, ')');
                                                char *body_call = sb_take(&call);

                                                const char *consume_end = block_end;
                                                const char *meta_scan = consume_end;
                                                while (*meta_scan) {
                                                    const char *mls = meta_scan;
                                                    while (*meta_scan && *meta_scan != '\n')
                                                        meta_scan++;
                                                    const char *mle = meta_scan;
                                                    const char *mlt = mls;
                                                    while (mlt < mle &&
                                                           (*mlt == ' ' || *mlt == '\t'))
                                                        mlt++;
                                                    if (mlt >= mle || *mlt == ';') {
                                                        consume_end = (*meta_scan == '\n')
                                                                          ? meta_scan + 1
                                                                          : meta_scan;
                                                        if (*meta_scan == '\n') meta_scan++;
                                                        continue;
                                                    }
                                                    if (measure_indent(mls) <= indent ||
                                                        (*mlt != ':' &&
                                                         !wisp_find_top_level_arrow(mlt)))
                                                        break;
                                                    if (*mlt == ':') {
                                                        consume_end = (*meta_scan == '\n')
                                                                          ? meta_scan + 1
                                                                          : meta_scan;
                                                        if (*meta_scan == '\n') meta_scan++;
                                                        continue;
                                                    }
                                                    break;
                                                }

                                                wts_push(&s, "define", indent, lineno);
                                                wts_push(&s, header, indent, lineno);
                                                wts_push(&s, body_call, indent, lineno + 1);

                                                free(header);
                                                free(body_call);
                                                for (int i = 0; i < arr_count; i++) {
                                                    free(param_types[i]);
                                                    free(names[i]);
                                                }
                                                free(ret_type);
                                                free(lambda);
                                                free(guard_params);
                                                free(left);
                                                free(body_raw);
                                                free(fname);
                                                free(raw);
                                                int consumed_lines = 1;
                                                for (const char *q = p; q < consume_end; q++)
                                                    if (*q == '\n')
                                                        consumed_lines++;
                                                p = consume_end;
                                                lineno += consumed_lines;
                                                continue;
                                            }

                                            for (int i = 0; i < arr_count; i++)
                                                free(param_types[i]);
                                            free(ret_type);
                                            free(lambda);
                                        }
                                    }

                                    for (int i = 0; i < arr_count; i++) {
                                        free(names[i]);
                                        names[i] = NULL;
                                    }
                                    free(guard_params);
                                }

                                if (arr_count < WISP_MAX_PARAMS &&
                                    wisp_parse_simple_param_names(left, arr_count, names)) {
                                    char *param_types[WISP_MAX_PARAMS] = {0};
                                    char *ret_type = NULL;
                                    if (wisp_split_arrow_signature(sig_rest, arr_count,
                                                                   param_types, &ret_type)) {
                                        wisp_validate_ignored_signature_names(param_types,
                                                                              names,
                                                                              arr_count,
                                                                              lineno);
                                        for (int i = 0; i < arr_count; i++) {
                                            int fn_arity = wisp_function_type_arity(param_types[i]);
                                            if (fn_arity > 0) arity_set(at, names[i], fn_arity);
                                            else arity_set(at, names[i], 0);
                                        }

                                        const char *rhs_start = body_arrow + 2;
                                        while (*rhs_start == ' ' || *rhs_start == '\t') rhs_start++;
                                        const char *rhs_end = get_logical_line_end(rhs_start);

                                        /* Check if the RHS on this line is unbalanced
                                         * (e.g. opens an (asm ... block that continues
                                         * on subsequent more-indented lines). If so,
                                         * accumulate those continuation lines before
                                         * expanding, so the whole (asm ...) form is
                                         * passed to wisp_expand_expr_snippet intact. */
                                        int rhs_depth = 0;
                                        {
                                            bool rhs_in_str = false;
                                            for (const char *rq = rhs_start; rq < rhs_end; rq++) {
                                                if (rhs_in_str) {
                                                    if (*rq == '\\') rq++;
                                                    else if (*rq == '"') rhs_in_str = false;
                                                    continue;
                                                }
                                                if (*rq == '"') { rhs_in_str = true; continue; }
                                                if (*rq=='('||*rq=='['||*rq=='{') rhs_depth++;
                                                if (*rq==')'||*rq==']'||*rq=='}') rhs_depth--;
                                            }
                                        }

                                        size_t rhs_acc_len = (size_t)(rhs_end - rhs_start);
                                        size_t rhs_acc_cap = rhs_acc_len + 256;
                                        char *rhs_acc = malloc(rhs_acc_cap);
                                        memcpy(rhs_acc, rhs_start, rhs_acc_len);
                                        rhs_acc[rhs_acc_len] = '\0';

                                        while (rhs_depth != 0 && *next_line) {
                                            const char *cls = next_line;
                                            while (*next_line && *next_line != '\n') next_line++;
                                            int cll = (int)(next_line - cls);
                                            if (*next_line == '\n') next_line++;
                                            char *clraw = strndup(cls, cll);
                                            const char *clt = clraw;
                                            while (*clt == ' ' || *clt == '\t') clt++;
                                            if (!*clt || *clt == ';') { free(clraw); continue; }
                                            const char *clt_end = get_logical_line_end(clt);
                                            size_t cltlen = clt_end - clt;
                                            while (rhs_acc_len + cltlen + 3 >= rhs_acc_cap) {
                                                rhs_acc_cap *= 2; rhs_acc = realloc(rhs_acc, rhs_acc_cap);
                                            }
                                            rhs_acc[rhs_acc_len++] = ' ';
                                            memcpy(rhs_acc + rhs_acc_len, clt, cltlen);
                                            rhs_acc_len += cltlen;
                                            rhs_acc[rhs_acc_len] = '\0';
                                            for (const char *bq = clt; bq < clt_end; bq++) {
                                                if (*bq=='('||*bq=='['||*bq=='{') rhs_depth++;
                                                if (*bq==')'||*bq==']'||*bq=='}') rhs_depth--;
                                            }
                                            free(clraw);
                                            lineno++;
                                        }

                                        char *rhs_src = rhs_acc;
                                        char *rhs_tok = NULL;
                                        const char *consume_end = next_line;

                                        SB seq_body;
                                        bool have_seq_body = false;
                                        int seq_expr_count = 0;
                                        const char *cont_scan = next_line;
                                        while (*cont_scan) {
                                            const char *cls = cont_scan;
                                            while (*cont_scan && *cont_scan != '\n')
                                                cont_scan++;
                                            const char *cle = cont_scan;
                                            const char *after_cont = (*cont_scan == '\n')
                                                                         ? cont_scan + 1
                                                                         : cont_scan;

                                            const char *clt = cls;
                                            while (clt < cle &&
                                                   (*clt == ' ' || *clt == '\t'))
                                                clt++;
                                            while (cle > clt &&
                                                   (cle[-1] == ' ' || cle[-1] == '\t' ||
                                                    cle[-1] == '\r'))
                                                cle--;

                                            if (clt >= cle || *clt == ';') {
                                                consume_end = after_cont;
                                                cont_scan = after_cont;
                                                continue;
                                            }

                                            int cont_indent = measure_indent(cls);
                                            if (cont_indent <= indent ||
                                                cont_indent <= body_indent ||
                                                *clt == ':') {
                                                break;
                                            }

                                            char *cont_line =
                                                strndup(clt, (size_t)(cle - clt));
                                            const char *cont_arrow =
                                                wisp_find_top_level_arrow(cont_line);
                                            if (cont_arrow) {
                                                free(cont_line);
                                                break;
                                            }

                                            if (!have_seq_body) {
                                                sb_init(&seq_body);
                                                have_seq_body = true;
                                                const char *rhs_stmt_end =
                                                    get_logical_line_end(rhs_src);
                                                wisp_append_expanded_statement(at,
                                                                               &seq_body,
                                                                               rhs_src,
                                                                               rhs_stmt_end,
                                                                               &seq_expr_count);
                                            }

                                            const char *cont_stmt = cont_line;
                                            char *joined_if = NULL;
                                            if (strncmp(cont_line, "if ", 3) == 0 &&
                                                strstr(cont_line, " else ") == NULL &&
                                                *after_cont) {
                                                const char *els = after_cont;
                                                const char *ele = after_cont;
                                                while (*ele && *ele != '\n')
                                                    ele++;
                                                const char *elt = els;
                                                while (elt < ele &&
                                                       (*elt == ' ' || *elt == '\t'))
                                                    elt++;
                                                if (ele - elt >= 4 &&
                                                    strncmp(elt, "else", 4) == 0 &&
                                                    (elt + 4 == ele ||
                                                     elt[4] == ' ' || elt[4] == '\t')) {
                                                    SB joined;
                                                    sb_init(&joined);
                                                    sb_puts(&joined, cont_line);
                                                    sb_putc(&joined, ' ');
                                                    sb_puts(&joined, elt);
                                                    joined_if = sb_take(&joined);
                                                    cont_stmt = joined_if;
                                                    after_cont = (*ele == '\n')
                                                                     ? ele + 1
                                                                     : ele;
                                                }
                                            }
                                            const char *cont_end =
                                                get_logical_line_end(cont_stmt);
                                            wisp_append_expanded_statement(at,
                                                                           &seq_body,
                                                                           cont_stmt,
                                                                           cont_end,
                                                                           &seq_expr_count);
                                            free(joined_if);
                                            free(cont_line);
                                            consume_end = after_cont;
                                            cont_scan = after_cont;
                                        }

                                        if (have_seq_body) {
                                            char *seq_text = sb_take(&seq_body);
                                            free(rhs_src);
                                            if (seq_expr_count > 1) {
                                                SB wrapped;
                                                sb_init(&wrapped);
                                                sb_puts(&wrapped, "(begin ");
                                                sb_puts(&wrapped, seq_text);
                                                sb_putc(&wrapped, ')');
                                                rhs_src = sb_take(&wrapped);
                                                free(seq_text);
                                            } else {
                                                rhs_src = seq_text;
                                            }
                                        }

                                        if (followed_by_where && where_line_start) {
                                            const char *bindings_start = where_line_start;
                                            while (*bindings_start && *bindings_start != '\n')
                                                bindings_start++;
                                            if (*bindings_start == '\n')
                                                bindings_start++;

                                            const char *block_end = bindings_start;
                                            const char *scan_bind = bindings_start;
                                            while (*scan_bind) {
                                                const char *bls = scan_bind;
                                                while (*scan_bind && *scan_bind != '\n')
                                                    scan_bind++;
                                                const char *ble = scan_bind;
                                                const char *blt = bls;
                                                while (blt < ble &&
                                                       (*blt == ' ' || *blt == '\t'))
                                                    blt++;
                                                if (blt >= ble || *blt == ';') {
                                                    block_end = (*scan_bind == '\n') ? scan_bind + 1 : scan_bind;
                                                    if (*scan_bind == '\n') scan_bind++;
                                                    continue;
                                                }

                                                int bind_indent = measure_indent(bls);
                                                if (bind_indent <= where_indent)
                                                    break;

                                                block_end = (*scan_bind == '\n') ? scan_bind + 1 : scan_bind;
                                                if (*scan_bind == '\n') scan_bind++;
                                            }

                                            if (block_end > bindings_start) {
                                                char *bindings =
                                                    strndup(bindings_start,
                                                            (size_t)(block_end - bindings_start));
                                                rhs_tok = wisp_try_expand_simple_where_letrec(
                                                    at, bindings, rhs_src);
                                                free(bindings);
                                                if (rhs_tok)
                                                    consume_end = block_end;
                                            }
                                        }

                                        if (!rhs_tok)
                                            rhs_tok = wisp_expand_expr_snippet(at, rhs_src);
                                        free(rhs_src);

                                        SB metadata;
                                        sb_init(&metadata);
                                        {
                                            const char *meta_scan = consume_end;
                                            while (*meta_scan) {
                                                const char *mls = meta_scan;
                                                while (*meta_scan && *meta_scan != '\n')
                                                    meta_scan++;
                                                const char *mle = meta_scan;
                                                const char *mlt = mls;
                                                while (mlt < mle &&
                                                       (*mlt == ' ' || *mlt == '\t'))
                                                    mlt++;

                                                if (mlt >= mle || *mlt == ';') {
                                                    consume_end = (*meta_scan == '\n')
                                                                    ? meta_scan + 1
                                                                    : meta_scan;
                                                    if (*meta_scan == '\n') meta_scan++;
                                                    continue;
                                                }

                                                int meta_indent = measure_indent(mls);
                                                bool is_define_metadata =
                                                    meta_indent > indent &&
                                                    ((strncmp(mlt, ":doc", 4) == 0 &&
                                                      (mlt + 4 == mle ||
                                                       mlt[4] == ' ' || mlt[4] == '\t')) ||
                                                     (strncmp(mlt, ":alias", 6) == 0 &&
                                                      (mlt + 6 == mle ||
                                                       mlt[6] == ' ' || mlt[6] == '\t')));
                                                if (!is_define_metadata)
                                                    break;

                                                sb_putc(&metadata, ' ');
                                                wisp_append_metadata_range(&metadata, mlt, mle);
                                                consume_end = (*meta_scan == '\n')
                                                                ? meta_scan + 1
                                                                : meta_scan;
                                                if (*meta_scan == '\n') meta_scan++;
                                            }
                                        }

                                        SB hdr;
                                        sb_init(&hdr);
                                        sb_putc(&hdr, '(');
                                        sb_puts(&hdr, fname);
                                        for (int i = 0; i < arr_count; i++) {
                                            const char *ptype = param_types[i];
                                            while (*ptype == ' ' || *ptype == '\t') ptype++;
                                            if (ptype[0] == '.') {
                                                const char *rest_type = ptype + 1;
                                                while (*rest_type == ' ' || *rest_type == '\t') rest_type++;
                                                sb_puts(&hdr, " . ");
                                                sb_puts(&hdr, rest_type);
                                                continue;
                                            }
                                            sb_puts(&hdr, " [");
                                            sb_puts(&hdr, names[i]);
                                            sb_puts(&hdr, " : ");
                                            sb_puts(&hdr, ptype);
                                            sb_putc(&hdr, ']');
                                        }
                                        sb_puts(&hdr, " -> ");
                                        const char *rtype = ret_type;
                                        while (*rtype == ' ' || *rtype == '\t') rtype++;
                                        sb_puts(&hdr, rtype);
                                        sb_putc(&hdr, ')');
                                        char *header = sb_take(&hdr);

                                        SB form;
                                        sb_init(&form);
                                        sb_puts(&form, "(define ");
                                        sb_puts(&form, header);
                                        sb_putc(&form, ' ');
                                        sb_puts(&form, rhs_tok);
                                        char *metadata_str = sb_take(&metadata);
                                        sb_puts(&form, metadata_str);
                                        sb_putc(&form, ')');
                                        char *form_str = sb_take(&form);
                                        wts_push(&s, form_str, indent, lineno);
                                        free(form_str);
                                        free(metadata_str);

                                        free(header);
                                        free(rhs_tok);
                                        for (int i = 0; i < arr_count; i++) {
                                            free(names[i]);
                                            free(param_types[i]);
                                        }
                                        free(ret_type);
                                        free(left);
                                        free(body_raw);
                                        free(fname);
                                        free(raw);
                                        int consumed_lines = 1;
                                        for (const char *q = p; q < consume_end; q++)
                                            if (*q == '\n')
                                                consumed_lines++;
                                        p = consume_end;
                                        lineno += consumed_lines;
                                        continue;
                                    }
                                    for (int i = 0; i < arr_count; i++) free(param_types[i]);
                                    free(ret_type);
                                }
                                for (int i = 0; i < arr_count; i++) free(names[i]);
                                free(left);
                            }
                            free(body_raw);
                        }
                    }

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

                    char *header = NULL;
                    if (arr_count > 0) {
                        char *param_types[WISP_MAX_PARAMS] = {0};
                        char *ret_type = NULL;
                        if (arr_count < WISP_MAX_PARAMS &&
                            wisp_split_arrow_signature(sig_rest, arr_count,
                                                       param_types, &ret_type)) {
                            char *body_param_names[WISP_MAX_PARAMS] = {0};
                            bool have_body_param_names = false;
                            const char *body_peek = p;
                            while (*body_peek) {
                                const char *bls = body_peek;
                                while (*body_peek && *body_peek != '\n') body_peek++;
                                const char *ble = body_peek;
                                if (*body_peek == '\n') body_peek++;

                                const char *bt = bls;
                                while (bt < ble && (*bt == ' ' || *bt == '\t')) bt++;
                                if (bt >= ble || *bt == ';' ||
                                    strncmp(bt, ":doc", 4) == 0 ||
                                    strncmp(bt, ":alias", 6) == 0) {
                                    continue;
                                }

                                char *body_line = strndup(bt, (size_t)(ble - bt));
                                const char *body_arrow = wisp_find_top_level_arrow(body_line);
                                if (body_arrow) {
                                    char *left = wisp_trim_range_dup(body_line, body_arrow);
                                    have_body_param_names =
                                        wisp_parse_simple_param_names(left, arr_count,
                                                                      body_param_names);
                                    free(left);
                                }
                                free(body_line);
                                break;
                            }

                            SB hdr;
                            sb_init(&hdr);
                            sb_putc(&hdr, '(');
                            sb_puts(&hdr, fname);
                            if (have_body_param_names) {
                                wisp_validate_ignored_signature_names(param_types,
                                                                      body_param_names,
                                                                      arr_count,
                                                                      lineno);
                            }
                            for (int i = 0; i < arr_count; i++) {
                                char pname[32];
                                snprintf(pname, sizeof(pname), "__p_%d", i);
                                const char *param_name =
                                    have_body_param_names ? body_param_names[i] : pname;
                                const char *ptype = param_types[i];
                                while (*ptype == ' ' || *ptype == '\t') ptype++;
                                if (ptype[0] == '.') {
                                    const char *rest_type = ptype + 1;
                                    while (*rest_type == ' ' || *rest_type == '\t') rest_type++;
                                    sb_puts(&hdr, " . ");
                                    sb_puts(&hdr, rest_type);
                                    free(param_types[i]);
                                    continue;
                                }
                                sb_puts(&hdr, " [");
                                sb_puts(&hdr, param_name);
                                sb_puts(&hdr, " : ");
                                sb_puts(&hdr, ptype);
                                sb_putc(&hdr, ']');
                                free(param_types[i]);
                            }
                            if (have_body_param_names) {
                                for (int i = 0; i < arr_count; i++)
                                    free(body_param_names[i]);
                            }
                            sb_puts(&hdr, " -> ");
                            const char *rtype = ret_type;
                            while (*rtype == ' ' || *rtype == '\t') rtype++;
                            sb_puts(&hdr, rtype);
                            sb_putc(&hdr, ')');
                            free(ret_type);
                            header = sb_take(&hdr);
                        }
                    }
                    if (!header) {
                        size_t siglen = strlen(sig_rest);
                        size_t hlen = 1 + name_len + 1 + siglen + 1 + 1;
                        header = malloc(hlen);
                        snprintf(header, hlen, "(%s %s)", fname, sig_rest);
                    }
                    free(fname);
                    const char *block_start = p;
                    const char *block_end = p;
                    const char *scan_clause = p;
                    bool has_clause_body = false;
                    while (*scan_clause) {
                        const char *ls = scan_clause;
                        while (*scan_clause && *scan_clause != '\n')
                            scan_clause++;
                        const char *le = scan_clause;
                        const char *lt = ls;
                        while (lt < le && (*lt == ' ' || *lt == '\t'))
                            lt++;
                        if (lt < le && *lt != ';') {
                            int clause_indent = measure_indent(ls);
                            if (clause_indent <= indent || *lt == ':')
                                break;
                            if (wisp_find_top_level_arrow(lt))
                                has_clause_body = true;
                        }
                        block_end = (*scan_clause == '\n') ? scan_clause + 1 : scan_clause;
                        if (*scan_clause == '\n')
                            scan_clause++;
                    }
                    if (has_clause_body && block_end > block_start) {
                        SB form;
                        sb_init(&form);
                        sb_puts(&form, "(define ");
                        sb_puts(&form, header);
                        char **block_guard_subjects = NULL;
                        int block_guard_subject_count = 0;
                        for (const char *q = block_start; q < block_end;) {
                            const char *ls = q;
                            while (q < block_end && *q != '\n')
                                q++;
                            const char *le = q;
                            const char *lt = ls;
                            while (lt < le && (*lt == ' ' || *lt == '\t'))
                                lt++;
                            if (lt < le && *lt != ';') {
                                sb_putc(&form, ' ');
                                char *clause_line = strndup(lt, (size_t)(le - lt));
                                const char *arrow = wisp_find_top_level_arrow(clause_line);
                                if (arrow) {
                                    const char *lhs_start = clause_line;
                                    const char *lhs_end = arrow;
                                    while (lhs_end > lhs_start &&
                                           (lhs_end[-1] == ' ' || lhs_end[-1] == '\t'))
                                        lhs_end--;

                                    const char *body_start = arrow + 2;
                                    while (*body_start == ' ' || *body_start == '\t')
                                        body_start++;
                                    char *body_src = wisp_trim_range_dup(body_start,
                                                                          clause_line + strlen(clause_line));
                                    bool body_needs_expand = false;
                                    const char *bt = body_src;
                                    while (*bt == ' ' || *bt == '\t') bt++;
                                    if (strncmp(bt, "if ", 3) == 0)
                                        body_needs_expand = true;
                                    int body_depth = 0;
                                    bool body_in_str = false;
                                    for (const char *c = bt; *c && !body_needs_expand; c++) {
                                        if (body_in_str) {
                                            if (*c == '\\' && c[1]) c++;
                                            else if (*c == '"') body_in_str = false;
                                            continue;
                                        }
                                        if (*c == '"') {
                                            body_in_str = true;
                                            continue;
                                        }
                                        if (*c == '(' || *c == '[' || *c == '{') {
                                            body_depth++;
                                            continue;
                                        }
                                        if (*c == ')' || *c == ']' || *c == '}') {
                                            if (body_depth > 0) body_depth--;
                                            continue;
                                        }
                                        if (body_depth == 0 &&
                                            ((c[0] == '+' && c[1] == '+') ||
                                             (c[0] == '<' && c[1] == '=') ||
                                             (c[0] == '>' && c[1] == '=') ||
                                             (c[0] == '!' && c[1] == '=') ||
                                             c[0] == '+' || c[0] == '-' ||
                                             c[0] == '*' || c[0] == '/' ||
                                             c[0] == '%' || c[0] == '<' ||
                                             c[0] == '>' || c[0] == '=')) {
                                            body_needs_expand = true;
                                        }
                                    }
                                    char *body_expanded = NULL;
                                    if (body_needs_expand) {
                                        body_expanded = wisp_expand_expr_snippet(at, body_src);
                                    } else if (body_src[0] != '(' &&
                                               body_src[0] != '[' &&
                                               body_src[0] != '{' &&
                                               strchr(body_src, ' ') != NULL) {
                                        size_t blen = strlen(body_src);
                                        body_expanded = malloc(blen + 3);
                                        body_expanded[0] = '(';
                                        memcpy(body_expanded + 1, body_src, blen);
                                        body_expanded[blen + 1] = ')';
                                        body_expanded[blen + 2] = '\0';
                                    } else {
                                        body_expanded = strdup(body_src);
                                    }
                                    free(body_src);

                                    const char *pipe = wisp_find_top_level_pipe_range(lhs_start, lhs_end);
                                    if (pipe) {
                                        size_t pat_len = (size_t)(pipe - lhs_start);
                                        while (pat_len > 0 &&
                                               (lhs_start[pat_len - 1] == ' ' ||
                                                lhs_start[pat_len - 1] == '\t'))
                                            pat_len--;

                                        if (pat_len > 0) {
                                            wisp_free_guard_subjects(block_guard_subjects,
                                                                     block_guard_subject_count);
                                            block_guard_subjects = NULL;
                                            block_guard_subject_count =
                                                wisp_extract_guard_subjects(lhs_start,
                                                                            lhs_start + pat_len,
                                                                            &block_guard_subjects);
                                        }

                                        const char *guard_start = pipe + 1;
                                        while (guard_start < lhs_end &&
                                               (*guard_start == ' ' || *guard_start == '\t'))
                                            guard_start++;

                                        char *guard_expanded = NULL;
                                        if (wisp_line_is_otherwise(guard_start, lhs_end)) {
                                            guard_expanded = strdup("True");
                                        } else if (block_guard_subject_count > 0) {
                                            char *guard_src = wisp_trim_range_dup(guard_start, lhs_end);
                                            guard_expanded =
                                                wisp_expand_pointfree_guard(at, guard_src,
                                                                            block_guard_subjects,
                                                                            block_guard_subject_count);
                                            free(guard_src);
                                        } else {
                                            char *guard_src = wisp_trim_range_dup(guard_start, lhs_end);
                                            guard_expanded = wisp_expand_expr_snippet(at, guard_src);
                                            free(guard_src);
                                        }

                                        bool guard_grouped = (guard_expanded[0] == '(' ||
                                                              guard_expanded[0] == '[' ||
                                                              guard_expanded[0] == '{');
                                        bool guard_single = (strchr(guard_expanded, ' ') == NULL);
                                        char *guard_emit = guard_expanded;
                                        if (!guard_grouped && !guard_single) {
                                            size_t glen = strlen(guard_expanded);
                                            guard_emit = malloc(glen + 3);
                                            guard_emit[0] = '(';
                                            memcpy(guard_emit + 1, guard_expanded, glen);
                                            guard_emit[glen + 1] = ')';
                                            guard_emit[glen + 2] = '\0';
                                            free(guard_expanded);
                                        }

                                        if (pat_len > 0) {
                                            for (size_t i = 0; i < pat_len; i++)
                                                sb_putc(&form, lhs_start[i]);
                                            sb_puts(&form, " | ");
                                            sb_puts(&form, guard_emit);
                                            sb_puts(&form, " -> ");
                                            sb_puts(&form, body_expanded);
                                        } else {
                                            sb_puts(&form, "| ");
                                            sb_puts(&form, guard_emit);
                                            sb_puts(&form, " -> ");
                                            sb_puts(&form, body_expanded);
                                        }
                                        free(guard_emit);
                                    } else {
                                        char **new_subjects = NULL;
                                        int new_subject_count =
                                            wisp_extract_guard_subjects(lhs_start, lhs_end,
                                                                        &new_subjects);
                                        if (new_subject_count > 0) {
                                            wisp_free_guard_subjects(block_guard_subjects,
                                                                     block_guard_subject_count);
                                            block_guard_subjects = new_subjects;
                                            block_guard_subject_count = new_subject_count;
                                        } else {
                                            wisp_free_guard_subjects(new_subjects,
                                                                     new_subject_count);
                                        }
                                        for (const char *c = lhs_start; c < lhs_end; c++)
                                            sb_putc(&form, *c);
                                        sb_puts(&form, " -> ");
                                        sb_puts(&form, body_expanded);
                                    }
                                    free(body_expanded);
                                } else {
                                    sb_puts(&form, clause_line);
                                }
                                free(clause_line);
                            }
                            if (q < block_end && *q == '\n')
                                q++;
                        }
                        wisp_free_guard_subjects(block_guard_subjects,
                                                 block_guard_subject_count);
                        sb_putc(&form, ')');
                        char *complete_define = sb_take(&form);
                        wts_push(&s, complete_define, indent, lineno);
                        free(complete_define);
                        int consumed_lines = 0;
                        for (const char *q = block_start; q < block_end; q++)
                            if (*q == '\n')
                                consumed_lines++;
                        p = block_end;
                        lineno += consumed_lines + 1;
                    } else {
                        wts_push(&s, "define", indent, lineno);
                        wts_push(&s, header, indent, lineno);
                        lineno++;
                    }
                    free(header);
                    free(raw);
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
                        while (body_end > body_start &&
                               (body_end[-1] == ' ' || body_end[-1] == '\t')) {
                            body_end--;
                        }

                        char *body_tok = NULL;
                        const char *where_kw = NULL;
                        for (const char *q = body_start; q + 7 <= body_end; q++) {
                            if ((q == body_start || q[-1] == ' ' || q[-1] == '\t') &&
                                strncmp(q, "where", 5) == 0 &&
                                (q + 5 == body_end || q[5] == ' ' || q[5] == '\t')) {
                                where_kw = q;
                                break;
                            }
                        }

                        if (where_kw) {
                            const char *main_end = where_kw;
                            while (main_end > body_start &&
                                   (main_end[-1] == ' ' || main_end[-1] == '\t')) {
                                main_end--;
                            }
                            char *main_src = strndup(body_start, (size_t)(main_end - body_start));
                            char *main_expr = wisp_expand_expr_snippet(at, main_src);
                            free(main_src);

                            const char *bind_start = where_kw + 5;
                            while (bind_start < body_end &&
                                   (*bind_start == ' ' || *bind_start == '\t')) {
                                bind_start++;
                            }
                            const char *eq = NULL;
                            for (const char *q = bind_start; q < body_end; q++) {
                                if (*q == '=') {
                                    eq = q;
                                    break;
                                }
                            }
                            if (eq) {
                                char *lhs = wisp_trim_range_dup(bind_start, eq);
                                const char *rhs_start = eq + 1;
                                while (rhs_start < body_end &&
                                       (*rhs_start == ' ' || *rhs_start == '\t')) {
                                    rhs_start++;
                                }
                                char *rhs_src = strndup(rhs_start, (size_t)(body_end - rhs_start));
                                char *rhs_expr = wisp_expand_expr_snippet(at, rhs_src);
                                free(rhs_src);

                                char *bind_names[WISP_MAX_PARAMS] = {0};
                                int bind_name_count =
                                    wisp_parse_simple_names_any(lhs, bind_names, WISP_MAX_PARAMS);
                                if (bind_name_count > 0) {
                                    SB where_body;
                                    sb_init(&where_body);
                                    sb_puts(&where_body, "(letrec ([");
                                    sb_puts(&where_body, bind_names[0]);
                                    sb_putc(&where_body, ' ');
                                    if (bind_name_count > 1) {
                                        sb_puts(&where_body, "(lambda (");
                                        for (int i = 1; i < bind_name_count; i++) {
                                            if (i > 1) sb_putc(&where_body, ' ');
                                            sb_puts(&where_body, bind_names[i]);
                                        }
                                        sb_puts(&where_body, ") ");
                                        sb_puts(&where_body, rhs_expr);
                                        sb_putc(&where_body, ')');
                                    } else {
                                        sb_puts(&where_body, rhs_expr);
                                    }
                                    sb_puts(&where_body, "]) ");
                                    sb_puts(&where_body, main_expr);
                                    sb_putc(&where_body, ')');
                                    body_tok = sb_take(&where_body);
                                }
                                for (int i = 0; i < bind_name_count; i++)
                                    free(bind_names[i]);
                                free(lhs);
                                free(rhs_expr);
                            }
                            free(main_expr);
                        }

                        if (!body_tok) {
                            size_t body_len = body_end - body_start;
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
                            if (body_has_space && !body_grouped) {
                                body_tok = malloc(body_len + 3);
                                body_tok[0] = '(';
                                memcpy(body_tok + 1, body_start, body_len);
                                body_tok[body_len + 1] = ')';
                                body_tok[body_len + 2] = '\0';
                            } else {
                                body_tok = strndup(body_start, body_len);
                            }
                        }

                        SB define_form;
                        sb_init(&define_form);
                        sb_puts(&define_form, "(define ");
                        sb_puts(&define_form, header);
                        sb_putc(&define_form, ' ');
                        sb_puts(&define_form, body_tok);
                        sb_putc(&define_form, ')');
                        char *define_tok = sb_take(&define_form);

                        free(fname);
                        wts_push(&s, define_tok, indent, lineno);
                        free(define_tok);
                        free(header);
                        free(body_tok);
                        free(raw);
                        lineno++;
                        continue;
                    }
                    free(fname);
                }

                /* Case 1c: define name
                 *             x y -> body
                 * No explicit type signature and no inline params on the
                 * header line. Infer the parameter names from the first
                 * indented arrow clause and emit one complete reader-level
                 * define form so the generic variadic define path never gets
                 * a chance to split the clause into top-level fragments. */
                if ((*after_name == '\0' || *after_name == ';') && *p) {
                    size_t name_len = name_end - after_define;
                    char *fname = strndup(after_define, name_len);
                    const char *next_pos = p;
                    int next_lineno = lineno + 1;
                    int inferred_arity = 0;

                    char *complete = wisp_try_build_inferred_define_form(at,
                                                                         fname,
                                                                         p,
                                                                         indent,
                                                                         lineno,
                                                                         &next_pos,
                                                                         &next_lineno,
                                                                         &inferred_arity);
                    if (complete) {
                        arity_set(at, fname, inferred_arity);
                        wts_push(&s, complete, indent, lineno);
                        free(complete);
                        free(fname);
                        free(raw);
                        p = next_pos;
                        lineno = next_lineno;
                        continue;
                    }

                    free(fname);
                }

                /* Case 2: define name -> Ret  (zero-param, return type only)
                 * Collect the indented body and emit one complete s-expression:
                 * (define name -> RetType body)
                 * This is identical to the s-expression form the reader already
                 * handles correctly, avoiding the lambda-with-->-as-param bug. */
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
                    char *ret_type = strndup(ret_start, ret_len);

                    const char *clause_start_pos = p;
                    const char *scan_pos = p;
                    int scan_lineno = lineno + 1;
                    char *params_text = NULL;
                    char *body_text = NULL;
                    int inferred_arity = 0;
                    bool found_clause = false;

                    while (*scan_pos) {
                        const char *ls = scan_pos;
                        while (*scan_pos && *scan_pos != '\n') scan_pos++;
                        const char *le = scan_pos;
                        if (*scan_pos == '\n') scan_pos++;

                        const char *lt = ls;
                        while (lt < le && (*lt == ' ' || *lt == '\t')) lt++;

                        if (lt >= le || *lt == ';') {
                            scan_lineno++;
                            continue;
                        }

                        int clause_indent = measure_indent(ls);
                        if (clause_indent <= indent) {
                            scan_pos = ls;
                            break;
                        }

                        char *line = strndup(lt, (size_t)(le - lt));
                        const char *arrow = wisp_find_top_level_arrow(line);
                        if (!arrow) {
                            free(line);
                            scan_pos = ls;
                            break;
                        }

                        params_text = wisp_trim_range_dup(line, arrow);
                        char *param_names[WISP_MAX_PARAMS];
                        memset(param_names, 0, sizeof(param_names));
                        inferred_arity = wisp_parse_simple_names_any(params_text,
                                                                     param_names,
                                                                     WISP_MAX_PARAMS);
                        for (int i = 0; i < inferred_arity; i++)
                            free(param_names[i]);

                        const char *rhs_start = arrow + 2;
                        while (*rhs_start == ' ' || *rhs_start == '\t')
                            rhs_start++;

                        SB body;
                        sb_init(&body);
                        int body_count = 0;

                        const char *rhs_end = get_logical_line_end(rhs_start);
                        while (rhs_end > rhs_start &&
                               (*(rhs_end - 1) == ' ' || *(rhs_end - 1) == '\t')) {
                            rhs_end--;
                        }

                        if (rhs_start < rhs_end) {
                            char *rhs_src = strndup(rhs_start,
                                                    (size_t)(rhs_end - rhs_start));
                            char *rhs = wisp_expand_expr_snippet(at, rhs_src);
                            free(rhs_src);
                            sb_puts(&body, rhs);
                            free(rhs);
                            body_count++;
                        }

                        free(line);
                        scan_lineno++;

                        while (*scan_pos) {
                            const char *bls = scan_pos;
                            while (*scan_pos && *scan_pos != '\n') scan_pos++;
                            const char *ble = scan_pos;
                            if (*scan_pos == '\n') scan_pos++;

                            const char *blt = bls;
                            while (blt < ble && (*blt == ' ' || *blt == '\t')) blt++;

                            if (blt >= ble || *blt == ';') {
                                scan_lineno++;
                                continue;
                            }

                            int bl_indent = measure_indent(bls);
                            if (bl_indent <= clause_indent) {
                                scan_pos = bls;
                                break;
                            }

                            char *body_line = strndup(blt, (size_t)(ble - blt));
                            const char *body_line_end = get_logical_line_end(body_line);
                            while (body_line_end > body_line &&
                                   (*(body_line_end - 1) == ' ' ||
                                    *(body_line_end - 1) == '\t')) {
                                body_line_end--;
                            }

                            if (body_line < body_line_end) {
                                char *line_src = strndup(body_line,
                                                         (size_t)(body_line_end - body_line));
                                char *line_expr = wisp_expand_expr_snippet(at, line_src);
                                free(line_src);

                                if (body_count > 0)
                                    sb_putc(&body, ' ');
                                sb_puts(&body, line_expr);
                                free(line_expr);
                                body_count++;
                            }

                            free(body_line);
                            scan_lineno++;
                        }

                        if (body_count == 0) {
                            sb_puts(&body, "(undefined)");
                            body_count = 1;
                        }

                        if (body_count > 1) {
                            char *raw_body = sb_take(&body);
                            SB wrapped;
                            sb_init(&wrapped);
                            sb_puts(&wrapped, "(begin ");
                            sb_puts(&wrapped, raw_body);
                            sb_putc(&wrapped, ')');
                            free(raw_body);
                            body_text = sb_take(&wrapped);
                        } else {
                            body_text = sb_take(&body);
                        }

                        found_clause = inferred_arity > 0;
                        break;
                    }

                    if (found_clause) {
                        arity_set(at, fname, inferred_arity);

                        SB full;
                        sb_init(&full);
                        sb_puts(&full, "(define (");
                        sb_puts(&full, fname);
                        sb_putc(&full, ' ');
                        sb_puts(&full, params_text);
                        sb_puts(&full, " -> ");
                        sb_puts(&full, ret_type);
                        sb_puts(&full, ") ");
                        sb_puts(&full, params_text);
                        sb_puts(&full, " -> ");
                        sb_puts(&full, body_text);
                        sb_putc(&full, ')');

                        char *complete = sb_take(&full);
                        wts_push(&s, complete, indent, lineno);

                        free(complete);
                        free(params_text);
                        free(body_text);
                        free(fname);
                        free(ret_type);
                        free(raw);
                        p = scan_pos;
                        lineno = scan_lineno;
                        continue;
                    }

                    p = clause_start_pos;

                    /* No indented parameter clause followed the return type:
                     * keep this as a true nullary function. */
                    arity_set(at, fname, 0);

                    SB full;
                    sb_init(&full);
                    sb_puts(&full, "(define (");
                    sb_puts(&full, fname);
                    sb_puts(&full, " -> ");
                    sb_puts(&full, ret_type);
                    sb_puts(&full, ") (undefined))");

                    char *complete = sb_take(&full);
                    wts_push(&s, complete, indent, lineno);

                    free(complete);
                    free(fname);
                    free(ret_type);
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
                    free(header);
                    free(raw);
                    lineno++;
                    continue;
                }

                /* Case 4: define name value-expression
                 * Example:
                 *   define case-delta 'a' - 'A'
                 * becomes:
                 *   (define case-delta (- 'a' 'A'))
                 *
                 * This must collect the whole rest of the line as the value,
                 * not just the first token. */
                if (*after_name && *after_name != ';') {
                    size_t name_len = name_end - after_define;
                    char *fname = strndup(after_define, name_len);

                    const char *value_start = after_name;
                    while (*value_start == ' ' || *value_start == '\t') value_start++;

                    const char *value_end = get_logical_line_end(value_start);
                    while (value_end > value_start &&
                           (*(value_end - 1) == ' ' || *(value_end - 1) == '\t')) {
                        value_end--;
                    }

                    if (value_start < value_end) {
                        if (strncmp(value_start, "::", 2) == 0) {
                            const char *type_start = value_start + 2;
                            while (*type_start == ' ' || *type_start == '\t') type_start++;
                            char *type_src =
                                strndup(type_start, (size_t)(value_end - type_start));
                            char *type_tok = wisp_expand_expr_snippet(at, type_src);
                            free(type_src);

                            arity_set(at, fname, 0);

                            SB form;
                            sb_init(&form);
                            sb_puts(&form, "(define [");
                            sb_puts(&form, fname);
                            sb_puts(&form, " :: ");
                            sb_puts(&form, type_tok);
                            sb_puts(&form, "] [])");
                            char *define_form = sb_take(&form);
                            wts_push(&s, define_form, indent, lineno);

                            free(define_form);
                            free(type_tok);
                            free(fname);
                            free(raw);
                            lineno++;
                            continue;
                        }

                        if (_def_len == 3) {
                            const char *line_end_for_defs = get_logical_line_end(t);
                            if (wisp_emit_inline_def_row(at, &s,
                                                         t,
                                                         line_end_for_defs,
                                                         indent,
                                                         lineno)) {
                                free(fname);
                                free(raw);
                                lineno++;
                                continue;
                            }
                        }

                        char *value_src = strndup(value_start, (size_t)(value_end - value_start));
                        char *value_tok = wisp_expand_expr_snippet(at, value_src);
                        free(value_src);

                        arity_set(at, fname, 0);

                        SB form;
                        sb_init(&form);
                        sb_putc(&form, '(');
                        sb_puts(&form, _def_len == 3 ? "def" : "define");
                        sb_putc(&form, ' ');
                        sb_puts(&form, fname);
                        sb_putc(&form, ' ');
                        sb_puts(&form, value_tok);
                        sb_putc(&form, ')');
                        char *define_form = sb_take(&form);
                        wts_push(&s, define_form, indent, lineno);

                        free(define_form);
                        free(value_tok);
                        free(fname);
                        free(raw);
                        lineno++;
                        continue;
                    }

                    free(fname);
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
                bool _ag = wisp_is_single_grouped_form(src); \
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
                    char *_expanded = wisp_expand_expr_snippet(at, (lines)[0]); \
                    WRAP(_expanded, (out)); \
                    free(_expanded); \
                } else { \
                    SB _bsb; sb_init(&_bsb); \
                    sb_puts(&_bsb, "(begin"); \
                    for (int _bi = 0; _bi < (count); _bi++) { \
                        sb_putc(&_bsb, ' '); \
                        char *_expanded = wisp_expand_expr_snippet(at, (lines)[_bi]); \
                        char *_bw = NULL; \
                        WRAP(_expanded, _bw); \
                        sb_puts(&_bsb, _bw); \
                        free(_bw); \
                        free(_expanded); \
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
                    char *then_expanded = wisp_expand_expr_snippet(at, then_raw);
                    free(then_raw);
                    WRAP(then_expanded, then_body);
                    free(then_expanded);

                    const char *after_else = inline_else + 5;
                    while (*after_else==' '||*after_else=='\t') after_else++;
                    size_t else_len = if_line_end - after_else;
                    char *else_raw = strndup(after_else, else_len);
                    char *else_expanded = wisp_expand_expr_snippet(at, else_raw);
                    free(else_raw);
                    WRAP(else_expanded, else_body);
                    free(else_expanded);
                } else {
                    size_t then_len = if_line_end - after_then;
                    char *then_raw = strndup(after_then, then_len);
                    char *then_expanded = wisp_expand_expr_snippet(at, then_raw);
                    free(then_raw);
                    WRAP(then_expanded, then_body);
                    free(then_expanded);
                    /* No inline else — scan subsequent lines at same indent
                     * for a trailing 'else' keyword (mixed Form 3 + Form 1/2).
                     * e.g.:  if n <= 0 then -1
                     *        else buf[0]                                      */
                    else_body = NULL;
                    while (*p) {
                        const char *ls = p;
                        while (*p && *p != '\n') p++;
                        if (*p == '\n') p++;
                        char *lraw2 = strndup(ls, p - ls);
                        const char *lt2 = lraw2;
                        while (*lt2==' '||*lt2=='\t') lt2++;
                        if (!*lt2 || *lt2==';') { free(lraw2); lineno++; continue; }

                        int li2 = measure_indent(lraw2);

                        if (li2 == indent &&
                            strncmp(lt2, "else", 4) == 0 &&
                            (lt2[4]==' '||lt2[4]=='\t'||lt2[4]=='\n'||
                             lt2[4]=='\r'||lt2[4]=='\0')) {
                            const char *ea = lt2 + 4;
                            while (*ea==' '||*ea=='\t') ea++;
                            if (*ea) {
                                /* else content on same line */
                                const char *ee = get_logical_line_end(ea);
                                char *else_raw = strndup(ea, ee - ea);
                                char *else_expanded = wisp_expand_expr_snippet(at, else_raw);
                                free(else_raw);
                                WRAP(else_expanded, else_body);
                                free(else_expanded);
                            } else {
                                /* else body on next deeper line */
                                char **else_lines = NULL;
                                int else_count = 0, else_cap = 0;
                                free(lraw2); lineno++;
                                while (*p) {
                                    const char *ls3 = p;
                                    while (*p && *p != '\n') p++;
                                    if (*p == '\n') p++;
                                    char *lraw3 = strndup(ls3, p - ls3);
                                    const char *lt3 = lraw3;
                                    while (*lt3==' '||*lt3=='\t') lt3++;
                                    if (!*lt3||*lt3==';') { free(lraw3); lineno++; continue; }
                                    int li3 = measure_indent(lraw3);
                                    if (li3 > indent) {
                                        const char *le3 = get_logical_line_end(lt3);
                                        if (else_count >= else_cap) {
                                            else_cap = else_cap ? else_cap*2 : 4;
                                            else_lines = realloc(else_lines, sizeof(char*)*else_cap);
                                        }
                                        else_lines[else_count++] = strndup(lt3, le3 - lt3);
                                        free(lraw3); lineno++;
                                        continue;
                                    }
                                    p = ls3;
                                    free(lraw3);
                                    break;
                                }
                                BUILD_BODY(else_lines, else_count, else_body);
                                for (int _i = 0; _i < else_count; _i++) free(else_lines[_i]);
                                free(else_lines);
                            }
                            free(lraw2);
                            break;
                        }

                        /* Not an else line — put it back and stop */
                        p = ls;
                        free(lraw2);
                        break;
                    }
                    if (!else_body) else_body = strdup("(undefined)");
                }
            } else {
                /* Form 1 or Form 2: condition is rest of if line */
                cond_str = strndup(after_if, if_line_end - after_if);

                /* Collect then-lines and else-lines from subsequent lines */
                char **then_lines = NULL; int then_count = 0; int then_cap = 0;
                char **else_lines = NULL; int else_count = 0; int else_cap = 0;
                bool in_else = false;
                int then_layout_base = -1;
                int else_layout_base = -1;

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
                        (lt2[4]==' '||lt2[4]=='\t'||lt2[4]=='\n'||
                         lt2[4]=='\r'||lt2[4]=='\0')) {
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
                        (lt2[4]==' '||lt2[4]=='\t'||lt2[4]=='\n'||
                         lt2[4]=='\r'||lt2[4]=='\0')) {
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
                        int *layout_base = in_else
                            ? &else_layout_base
                            : &then_layout_base;
                        if (*layout_base < 0)
                            *layout_base = li2;
                        int relative_indent = li2 - *layout_base;
                        size_t content_len = (size_t)(le2 - lt2);
                        char *layout_line = malloc((size_t)relative_indent +
                                                   content_len + 1);
                        memset(layout_line, ' ', (size_t)relative_indent);
                        memcpy(layout_line + relative_indent, lt2, content_len);
                        layout_line[relative_indent + content_len] = '\0';
                        if (!in_else) {
                            if (then_count >= then_cap) {
                                then_cap = then_cap ? then_cap*2 : 4;
                                then_lines = realloc(then_lines, sizeof(char*)*then_cap);
                            }
                            then_lines[then_count++] = layout_line;
                        } else {
                            if (else_count >= else_cap) {
                                else_cap = else_cap ? else_cap*2 : 4;
                                else_lines = realloc(else_lines, sizeof(char*)*else_cap);
                            }
                            else_lines[else_count++] = layout_line;
                        }
                        free(lraw2); lineno++;
                        continue;
                    }

                    /* Lesser or same indent, not a keyword = end of if */
                    p = ls;
                    free(lraw2);
                    break;
                }

                /* A branch beginning with `if` owns its following layout
                 * lines.  Expanding those lines separately turns `then` and
                 * `else` into ordinary identifiers and changes association.
                 * Re-run the complete branch through Wisp so nested
                 * conditionals are lowered recursively. */
                if (then_count > 1 &&
                    strncmp(then_lines[0], "if", 2) == 0 &&
                    (then_lines[0][2] == ' ' || then_lines[0][2] == '\t')) {
                    SB nested;
                    sb_init(&nested);
                    for (int i = 0; i < then_count; i++) {
                        if (i > 0) sb_putc(&nested, '\n');
                        sb_puts(&nested, then_lines[i]);
                    }
                    char *nested_src = sb_take(&nested);
                    then_body = wisp_expand_expr_snippet(at, nested_src);
                    free(nested_src);
                } else {
                    BUILD_BODY(then_lines, then_count, then_body);
                }

                if (else_count > 1 &&
                    strncmp(else_lines[0], "if", 2) == 0 &&
                    (else_lines[0][2] == ' ' || else_lines[0][2] == '\t')) {
                    SB nested;
                    sb_init(&nested);
                    for (int i = 0; i < else_count; i++) {
                        if (i > 0) sb_putc(&nested, '\n');
                        sb_puts(&nested, else_lines[i]);
                    }
                    char *nested_src = sb_take(&nested);
                    else_body = wisp_expand_expr_snippet(at, nested_src);
                    free(nested_src);
                } else {
                    BUILD_BODY(else_lines, else_count, else_body);
                }

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

        /* Pattern guards can be written with the pattern head on one line and
         * guarded alternatives below it:
         *   p [x|xs]
         *     | p x -> ...
         *
         * The head is pattern syntax, not an expression. If a later clause or
         * function body has taught the arity table that "p" is callable, normal
         * Wisp tokenisation would turn the head into "(p [x|xs])". Keep it raw
         * when the next meaningful line is a deeper guard clause. */
        {
            bool followed_by_guard = false;
            const char *look = p;
            while (*look) {
                const char *ls = look;
                while (*look && *look != '\n') look++;
                if (*look == '\n') look++;

                char *lraw = strndup(ls, look - ls);
                const char *lt = lraw;
                while (*lt == ' ' || *lt == '\t') lt++;
                if (!*lt || *lt == ';') {
                    free(lraw);
                    continue;
                }

                int l_indent = measure_indent(lraw);
                followed_by_guard = (l_indent > indent && *lt == '|');
                free(lraw);
                break;
            }

            if (*t == '|') {
                const char *t_end = get_logical_line_end(t);
                while (t_end > t && (*(t_end - 1) == ' ' || *(t_end - 1) == '\t'))
                    t_end--;
                char *guard_line = malloc((size_t)(t_end - t) + 2);
                memcpy(guard_line, t, (size_t)(t_end - t));
                guard_line[t_end - t] = ' ';
                guard_line[t_end - t + 1] = '\0';
                wts_push(&s, guard_line, indent, lineno);
                free(guard_line);
            } else if (followed_by_guard) {
                const char *t_end = get_logical_line_end(t);
                while (t_end > t && (*(t_end - 1) == ' ' || *(t_end - 1) == '\t'))
                    t_end--;
                char *pat_head = malloc((size_t)(t_end - t) + 2);
                memcpy(pat_head, t, (size_t)(t_end - t));
                pat_head[t_end - t] = ' ';
                pat_head[t_end - t + 1] = '\0';
                wts_push(&s, pat_head, indent, lineno);
                free(pat_head);
            } else if (strncmp(t, ":doc", 4) == 0 &&
                       (t[4] == ' ' || t[4] == '\t')) {
                const char *msg = t + 4;
                while (*msg == ' ' || *msg == '\t') msg++;
                if (*msg == '"') {
                    tokenise_into(at, &s, t, indent, lineno);
                } else {
                    const char *t_end = get_logical_line_end(t);
                    char *quoted = wisp_quote_string_range(msg, t_end);
                    wts_push(&s, ":doc", indent, lineno);
                    wts_push(&s, quoted, indent, lineno);
                    free(quoted);
                }
            } else if (pending_alias_doc_indent == indent &&
                       *t != ':' &&
                       !wisp_find_top_level_arrow(t)) {
                const char *t_end = get_logical_line_end(t);
                char *quoted = wisp_quote_string_range(t, t_end);
                wts_push(&s, ":doc", indent, lineno);
                wts_push(&s, quoted, indent, lineno);
                free(quoted);
            } else {
                tokenise_into(at, &s, t, indent, lineno);
            }

            pending_alias_doc_indent =
                (strncmp(t, ":alias", 6) == 0 &&
                 (t[6] == ' ' || t[6] == '\t' || t[6] == '\0'))
                    ? indent
                    : -1;
        }
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

    /* Skip any leading #line directive tokens transparently — they are
     * location markers emitted by the accumulator and must never be
     * treated as expressions or consumed as function arguments.
     * We emit them directly to `out` so they are preserved for the reader's
     * source-map, but they do not count as a parsed expression.           */
    while (s->pos < s->count) {
        const char *nt = s->tokens[s->pos].text;
        if (nt[0] == '(' && strncmp(nt, "(#line", 6) == 0) {
            sb_putc(out, ' ');
            sb_puts(out, nt);
            s->pos++;
        } else {
            break;
        }
    }
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

    if ((text[0] == '|' && (text[1] == '\0' || text[1] == ' ' || text[1] == '\t')) ||
        (text[0] == '-' && text[1] == '>' &&
         (text[2] == '\0' || text[2] == ' ' || text[2] == '\t'))) {
        char *prefix = sb_take(&prefix_sb);
        sb_puts(out, prefix);
        free(prefix);
        sb_puts(out, text);
        s->pos++;
        return;
    }

    bool is_grouped = (text[0] == '(' || text[0] == '[' || text[0] == '{' ||
                       (text[0] == '~' && text[1] == '[') ||
                       (text[0] == '#' && text[1] == '{'));

    int arity = is_grouped ? 0 : wisp_lookup_arity(t, text);
    if (!is_grouped && strcmp(text, "show") == 0)
        arity = 1;

    if (!is_grouped) {
        /* fprintf(stderr, "DEBUG wisp_parse_expr: text='%s' arity=%d pos=%d\n", text, arity, s->pos); */
    }
    s->pos++;

    if (!is_grouped && strcmp(text, "error") == 0) {
        wisp_emit_error_from_stream(s, &prefix_sb, my_indent, my_lineno);
    }
    else if (!is_grouped && strcmp(text, "if") == 0) {
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
        if (!is_grouped && arity == -2 && caller_prec < 6 &&
            wisp_token_can_call_group(text) &&
            s->pos < s->count &&
            s->tokens[s->pos].lineno == my_lineno &&
            s->tokens[s->pos].text[0] == '(') {
            sb_putc(&prefix_sb, '(');
            sb_puts(&prefix_sb, text);
            sb_putc(&prefix_sb, ' ');
            wisp_parse_expr(t, s, &prefix_sb, my_indent, 1, 6);
            sb_putc(&prefix_sb, ')');
        } else if (is_ratio_lit && s->pos < s->count &&
            s->tokens[s->pos].lineno == my_lineno) {
            /* Check the next token is a value, not an operator that would
             * be handled by the infix loop (arity >= 2). */
            int next_ar = wisp_lookup_arity(t, s->tokens[s->pos].text);
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
            if (is_grouped && text[0] == '(') {
                char *rewritten = wisp_rewrite_grouped_infix(t, text);
                sb_puts(&prefix_sb, rewritten);
                free(rewritten);
            } else {
                sb_puts(&prefix_sb, text);
            }
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
                /* Skip any #line directive tokens transparently —
                 * they are location markers, not real arguments,
                 * and must not consume an arity slot.             */
                while (s->pos < s->count) {
                    const char *nt = s->tokens[s->pos].text;
                    bool is_line_dir = (nt[0] == '(' &&
                                        strncmp(nt, "(#line", 6) == 0);
                    if (!is_line_dir) break;
                    sb_putc(&prefix_sb, ' ');
                    sb_puts(&prefix_sb, nt);
                    s->pos++;
                }
                if (s->pos >= s->count) break;
                sb_putc(&prefix_sb, ' ');
                ParamKind kind = (entry && i < WISP_MAX_PARAMS)
                               ? entry->param_kinds[i] : PARAM_VALUE;
                bool preserve_later_fixed_slots = false;
                if (!args_are_bare && kind == PARAM_VALUE && i < arity - 1) {
                    WToken *arg = &s->tokens[s->pos];
                    bool arg_is_atom = arg->text[0] != '(' &&
                                       arg->text[0] != '[' &&
                                       arg->text[0] != '{' &&
                                       !(arg->text[0] == '~' && arg->text[1] == '[') &&
                                       !(arg->text[0] == '#' && arg->text[1] == '{');
                    int arg_arity = arg_is_atom ? wisp_lookup_arity(t, arg->text) : -2;
                    preserve_later_fixed_slots = arg_is_atom && arg_arity > 0;
                }
                if (kind == PARAM_FUNC || args_are_bare || preserve_later_fixed_slots) {
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
        int op_ar = wisp_lookup_arity(t, op_tok->text);
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
            if (strcmp(e->name, "show") == 0)
                arity = 1;

            if (!g_ffi_arities_init) {
                memset(&g_ffi_arities, 0, sizeof(g_ffi_arities));
                g_ffi_arities_init = true;
            }
            arity_set_with_kinds(&g_ffi_arities, e->name, arity, e->param_kinds);
        }
    }
}

static int wisp_line_comment_marker_len_at(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] == ';') return 1;
    if (p[0] == 0xE2 && p[1] == 0x95 && p[2] == 0xAD) return 3;
    if (p[0] == 0xE2 && p[1] == 0x95 && p[2] == 0xAE) return 3;
    if (p[0] == 0xE2 && p[1] == 0x95 && p[2] == 0xAF) return 3;
    if (p[0] == 0xE2 && p[1] == 0x95 && p[2] == 0xB0) return 3;
    return 0;
}

static bool wisp_line_comment_marker_erases_left_at(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    return p[0] == 0xE2 && p[1] == 0x95 &&
           (p[2] == 0xAE || p[2] == 0xAF);
}

/* Strip -| ... |-, paragraph comments, and line comments from source,
 * replacing comment bytes with spaces/newlines to preserve line and column
 * positions.  This is intentionally a single canonical source-level pass:
 * later Wisp scanners should not need to know every comment spelling. */
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

        for (int j = start; j < end && j < len; j++)
            out[j] = (source[j] == '\n') ? '\n' : ' ';
    }

    bool in_string = false;
    bool in_char = false;
    bool escape = false;

    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)out[i];

        if (in_string) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (in_char) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '\'') {
                in_char = false;
            } else if (c == '\n' || c == '\r') {
                in_char = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            continue;
        }

        if (c == '\'') {
            in_char = true;
            continue;
        }

        int marker_len = wisp_line_comment_marker_len_at(out + i);
        if (marker_len > 0) {
            int start = i;

            if (wisp_line_comment_marker_erases_left_at(out + i)) {
                while (start > 0 && out[start - 1] != '\n')
                    start--;
            }

            while (start < len && out[start] != '\n') {
                out[start] = ' ';
                start++;
            }

            i = start;
            if (i < len && out[i] == '\n')
                out[i] = '\n';
        }
    }

    return out;
}

static void strip_define_pipe_comments_in_place(char *source) {
    char *line = source;
    while (*line) {
        char *line_end = strchr(line, '\n');
        if (!line_end)
            line_end = line + strlen(line);

        char *t = line;
        while (t < line_end && (*t == ' ' || *t == '\t'))
            t++;

        int def_len =
            (line_end - t >= 6 && strncmp(t, "define", 6) == 0 &&
             (t[6] == ' ' || t[6] == '\t')) ? 6 :
            (line_end - t >= 3 && strncmp(t, "def", 3) == 0 &&
             (t[3] == ' ' || t[3] == '\t')) ? 3 : 0;

        if (def_len > 0) {
            int depth = 0;
            bool in_string = false;
            bool escape = false;
            for (char *q = line; q < line_end; q++) {
                if (in_string) {
                    if (escape) {
                        escape = false;
                    } else if (*q == '\\') {
                        escape = true;
                    } else if (*q == '"') {
                        in_string = false;
                    }
                    continue;
                }
                if (*q == '"') {
                    in_string = true;
                    continue;
                }
                if (*q == '(' || *q == '[' || *q == '{') {
                    depth++;
                    continue;
                }
                if (*q == ')' || *q == ']' || *q == '}') {
                    if (depth > 0)
                        depth--;
                    continue;
                }
                if (depth == 0 && *q == ' ' && q + 1 < line_end && q[1] == '|') {
                    for (char *wipe = q; wipe < line_end; wipe++)
                        *wipe = ' ';
                    break;
                }
            }
        }

        line = (*line_end == '\n') ? line_end + 1 : line_end;
    }
}

static bool wisp_metadata_key_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '?' || c == '!' ||
           c > 127;
}

static bool wisp_drawer_name_char(unsigned char c) {
    return wisp_metadata_key_char(c) || c == ' ';
}

static bool wisp_drawer_marker_line(const char *line,
                                    const char *line_end,
                                    char **name_out) {
    const char *p = line;
    while (p < line_end && (*p == ' ' || *p == '\t'))
        p++;

    if (p >= line_end || *p != ':')
        return false;

    const char *name_start = p + 1;
    const char *q = name_start;
    while (q < line_end && *q != ':') {
        if (!wisp_drawer_name_char((unsigned char)*q))
            return false;
        q++;
    }

    if (q >= line_end || q == name_start)
        return false;

    const char *r = q + 1;
    while (r < line_end && (*r == ' ' || *r == '\t'))
        r++;
    if (r != line_end)
        return false;

    *name_out = strndup(name_start, (size_t)(q - name_start));
    return *name_out != NULL;
}

static void strip_org_drawers_in_place(char *source) {
    char *line = source;

    while (*line) {
        char *line_end = strchr(line, '\n');
        if (!line_end)
            line_end = line + strlen(line);

        char *name = NULL;
        if (!wisp_drawer_marker_line(line, line_end, &name)) {
            line = (*line_end == '\n') ? line_end + 1 : line_end;
            continue;
        }

        char *scan = (*line_end == '\n') ? line_end + 1 : line_end;
        char *close_end = NULL;
        while (*scan) {
            char *scan_end = strchr(scan, '\n');
            if (!scan_end)
                scan_end = scan + strlen(scan);

            char *close_name = NULL;
            if (wisp_drawer_marker_line(scan, scan_end, &close_name)) {
                bool same = strcmp(name, close_name) == 0;
                free(close_name);
                if (same) {
                    close_end = (*scan_end == '\n') ? scan_end + 1 : scan_end;
                    break;
                }
            }

            scan = (*scan_end == '\n') ? scan_end + 1 : scan_end;
        }

        free(name);

        if (!close_end) {
            line = (*line_end == '\n') ? line_end + 1 : line_end;
            continue;
        }

        for (char *p = line; p < close_end; p++)
            if (*p != '\n')
                *p = ' ';

        line = close_end;
    }
}

static bool wisp_is_toplevel_metadata_line(const char *line, const char *line_end) {
    if (!line || !line_end || line >= line_end)
        return false;

    if (line[0] != ':')
        return false;

    if (line + 1 >= line_end)
        return false;

    if (line[1] == ':' || line[1] == ' ' || line[1] == '\t')
        return false;

    if (!wisp_metadata_key_char((unsigned char)line[1]))
        return false;

    const char *p = line + 2;
    while (p < line_end && *p != ' ' && *p != '\t') {
        if (!wisp_metadata_key_char((unsigned char)*p))
            return false;
        p++;
    }

    return true;
}

static void strip_toplevel_module_metadata_in_place(char *source) {
    char *line = source;
    bool in_metadata_block = false;
    int metadata_indent = 0;

    while (*line) {
        char *line_end = strchr(line, '\n');
        if (!line_end)
            line_end = line + strlen(line);

        char *trim = line;
        while (trim < line_end && (*trim == ' ' || *trim == '\t'))
            trim++;
        bool blank = (trim >= line_end || *trim == '\r');
        bool metadata_line = wisp_is_toplevel_metadata_line(line, line_end);
        bool metadata_continuation =
            in_metadata_block && !blank && measure_indent(line) > metadata_indent;

        int line_indent = measure_indent(line);

        if (metadata_line || metadata_continuation) {
            for (char *p = line; p < line_end; p++)
                *p = ' ';

            if (metadata_line) {
                in_metadata_block = true;
                metadata_indent = line_indent;
            }
        } else if (blank) {
            in_metadata_block = false;
        } else {
            in_metadata_block = false;
        }

        line = (*line_end == '\n') ? line_end + 1 : line_end;
    }
}

static bool wisp_comment_section_marker_p(const char *line,
                                          const char *line_end,
                                          const char *marker) {
    const char *p = line;

    while (p < line_end && (*p == ' ' || *p == '\t'))
        p++;

    if (p >= line_end || *p != ';')
        return false;

    while (p < line_end && *p == ';')
        p++;

    while (p < line_end && (*p == ' ' || *p == '\t'))
        p++;

    size_t marker_len = strlen(marker);
    if ((size_t)(line_end - p) < marker_len)
        return false;

    if (strncmp(p, marker, marker_len) != 0)
        return false;

    p += marker_len;
    return p == line_end || *p == '\r' || *p == ' ' || *p == '\t';
}

static void strip_commentary_sections_in_place(char *source) {
    bool in_commentary = false;
    char *line = source;

    while (*line) {
        char *line_end = strchr(line, '\n');
        if (!line_end)
            line_end = line + strlen(line);

        bool starts_commentary =
            wisp_comment_section_marker_p(line, line_end, "Commentary:");
        bool starts_code =
            wisp_comment_section_marker_p(line, line_end, "Code:");

        if (starts_commentary)
            in_commentary = true;

        if (in_commentary) {
            for (char *p = line; p < line_end; p++)
                *p = ' ';
        }

        if (starts_code)
            in_commentary = false;

        line = (*line_end == '\n') ? line_end + 1 : line_end;
    }
}

typedef struct {
    char *bytes;
    int   is_blank;
} VChar;

typedef struct {
    VChar *chars;
    int    len;
} VLine;

static void vline_free(VLine vl) {
    for (int i = 0; i < vl.len; i++)
        free(vl.chars[i].bytes);
    free(vl.chars);
}

static VLine parse_vline(const char *s) {
    VLine vl = { .chars = malloc((strlen(s) + 1) * sizeof(VChar)), .len = 0 };
    while (*s) {
        const char *start = s;
        size_t len = 1;

        if (((unsigned char)*s & 0x80) == 0) {
            len = 1;
        } else if (((unsigned char)*s & 0xE0) == 0xC0 && s[1]) {
            len = 2;
        } else if (((unsigned char)*s & 0xF0) == 0xE0 && s[1] && s[2]) {
            len = 3;
        } else if (((unsigned char)*s & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
            len = 4;
        }

        VChar vc;
        vc.bytes = strndup(start, len);
        vc.is_blank = (vc.bytes[0] == ' ' || vc.bytes[0] == '\t');
        vl.chars[vl.len++] = vc;
        s += len;
    }
    return vl;
}

static void vchar_set(VChar *vc, const char *text) {
    if (!vc) return;
    free(vc->bytes);
    vc->bytes = strdup(text ? text : "");
    vc->is_blank = (vc->bytes[0] == ' ' || vc->bytes[0] == '\t');
}

static char *vline_to_str(VLine vl) {
    size_t total = 1;
    for (int i = 0; i < vl.len; i++)
        total += strlen(vl.chars[i].bytes);

    char *res = malloc(total);
    size_t pos = 0;
    for (int i = 0; i < vl.len; i++) {
        size_t len = strlen(vl.chars[i].bytes);
        memcpy(res + pos, vl.chars[i].bytes, len);
        pos += len;
    }
    res[pos] = '\0';
    return res;
}

static bool wisp_fraction_operand_is_grouped(const char *s) {
    if (!s)
        return false;

    while (*s == ' ' || *s == '\t')
        s++;

    return *s == '(' || *s == '[' || *s == '{' ||
           (*s == '~' && s[1] == '[') ||
           (*s == '#' && s[1] == '{');
}

static bool wisp_fraction_operand_needs_group(const char *s) {
    if (!s)
        return false;

    while (*s == ' ' || *s == '\t')
        s++;

    if (!*s || wisp_fraction_operand_is_grouped(s))
        return false;

    int depth = 0;
    bool in_str = false;
    bool in_char = false;
    bool escape = false;

    for (const char *p = s; *p; p++) {
        if (in_str) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '"') in_str = false;
            continue;
        }

        if (in_char) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '\'') in_char = false;
            continue;
        }

        if (*p == '"') {
            in_str = true;
            continue;
        }

        if (*p == '\'') {
            in_char = true;
            continue;
        }

        if (*p == '(' || *p == '[' || *p == '{') {
            depth++;
            continue;
        }

        if (*p == ')' || *p == ']' || *p == '}') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth == 0 && (*p == ' ' || *p == '\t'))
            return true;
    }

    return false;
}

static char *wisp_fraction_group_operand(const char *src) {
    const char *start = src ? src : "";
    while (*start == ' ' || *start == '\t')
        start++;

    const char *end = start + strlen(start);
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t'))
        end--;

    char *trimmed = wisp_trim_range_dup(start, end);

    if (!wisp_fraction_operand_needs_group(trimmed))
        return trimmed;

    size_t len = strlen(trimmed);
    char *grouped = malloc(len + 3);
    grouped[0] = '(';
    memcpy(grouped + 1, trimmed, len);
    grouped[len + 1] = ')';
    grouped[len + 2] = '\0';

    free(trimmed);
    return grouped;
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

                    char *N_form = wisp_fraction_group_operand(N_str);
                    char *D_form = wisp_fraction_group_operand(D_str);

                    char repl[1024];
                    snprintf(repl, sizeof(repl), "(/ %s %s)", N_form, D_form);

                    free(N_str);
                    free(D_str);
                    free(N_form);
                    free(D_form);

                    vchar_set(&L_curr.chars[run_start], repl);
                    for (int k = run_start + 1; k < run_end; k++)
                        vchar_set(&L_curr.chars[k], " ");

                    for (int k = run_start; k < run_end; k++) {
                        if (k < L_prev.len) vchar_set(&L_prev.chars[k], " ");
                        if (k < L_next.len) vchar_set(&L_next.chars[k], " ");
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
            vline_free(L_prev);
            vline_free(L_curr);
            vline_free(L_next);
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
    ArityEntry *e = NULL;
    if (g_active_wisp_arities)
        e = arity_get_entry(g_active_wisp_arities, func_name);
    if (!e && g_ffi_arities_init)
        e = arity_get_entry(&g_ffi_arities, func_name);
    if (!e) return 0;
    if (arg_index < 0 || arg_index >= WISP_MAX_PARAMS) return 0;
    return e->param_kinds[arg_index] == PARAM_FUNC ? 1 : 0;
}

static int wisp_is_known_function(const char *name) {
    ArityEntry *e = NULL;
    if (g_active_wisp_arities)
        e = arity_get_entry(g_active_wisp_arities, name);
    if (!e && g_ffi_arities_init)
        e = arity_get_entry(&g_ffi_arities, name);
    if (e)
        return e->arity != 0 ? 1 : 0;
    return 0;
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
        strcmp(name, "%") == 0 || strcmp(name, "mod") == 0)
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
    wisp_pending_type_clear();

    /* Commentary sections are documentation, not code. Strip them before
     * ordinary comment stripping so raw ASCII art and prose between
     * ;;; Commentary: and ;;; Code: never reach the lexer. */
    char *section_stripped = strdup(source);
    strip_commentary_sections_in_place(section_stripped);

    /* Strip comments first, preserving line structure */
    char *stripped = strip_comments(section_stripped);
    free(section_stripped);
    strip_define_pipe_comments_in_place(stripped);
    strip_org_drawers_in_place(stripped);

    /* 2D Fraction Desugaring */
    stripped = desugar_fractions(stripped);

    /* Top-level :Keyword lines are module metadata, not executable code.
     * Blank them before Wisp detection, arity prescan, and token lowering so
     * arbitrary payloads like 0.0.1, names, tags, URLs, or prose never reach
     * the reader as expressions. Newlines are preserved for diagnostics. */
    strip_toplevel_module_metadata_in_place(stripped);

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
    arity_set(&t, "doc", -1);
    arity_set(&t, "var", 2);
    arity_set(&t, "assert-eq", 3);
    arity_set(&t, "method", -1);
    /* def inside a function body must NOT be given arity 2 by wisp —
     * reader.c handles def specially. Setting arity -2 (unknown) makes
     * wisp_parse_expr emit it as a passthrough atom so the reader sees
     * the raw (def name value) token sequence and handles it correctly. */
    arity_set(&t, "def", -2);

    arity_set(&t, "Byte", 1);
    arity_set(&t, "Byte?", 1);
    arity_set(&t, "Path", 1);
    arity_set(&t, "Path?", 1);

    arity_set(&t, "mod", 2);
    arity_set(&t, "%", 2);
    arity_set(&t, "+", 2);
    arity_set(&t, "-", 2);
    arity_set(&t, "*", 2);
    arity_set(&t, "/", 2);
    arity_set(&t, "=", 2);
    arity_set(&t, "!=", 2);
    arity_set(&t, "<", 2);
    arity_set(&t, ">", 2);
    arity_set(&t, "<=", 2);
    arity_set(&t, ">=", 2);
    arity_set(&t, "and", 2);
    arity_set(&t, "or", 2);

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

    if (g_wisp_trace_enabled) {
        fprintf(stderr, "\n=== wisp expanded (%s) ===\n%s\n=== end ===\n\n",
                filename ? filename : "<input>", transformed);
    }

    if (wisp_debug_enabled()) {
        const char *m = transformed;
        while ((m = strstr(m, "(method ")) != NULL) {
            const char *e = strchr(m, '\n');
            if (!e)
                e = m + strlen(m);
            fprintf(stderr, "[wisp-debug] raw method form: %.*s\n",
                    (int)(e - m), m);
            m = e;
        }
    }

    /* Install param-kind hook so reader.c can do automatic infix detection */
    g_active_wisp_arities = &t;
    g_param_kind_is_func  = wisp_param_kind_is_func;
    g_is_known_function   = wisp_is_known_function;

    parser_set_context(filename, transformed);
    parser_set_original_source(source);
    ASTList result = parse_all(transformed);
    result = macro_expand_all(result.exprs, result.count);

    g_param_kind_is_func  = NULL;
    g_is_known_function   = NULL;
    g_active_wisp_arities = NULL;

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
